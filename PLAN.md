# PCIe passthrough for VirtualBox: hot-plug, removal safety, hardening

Working plan for the branch `pci-passthrough-hotplug` of this fork.
Baseline: upstream `main` at `b6bea721` (2026-11-11).

## 1. Goal

Make VFIO based PCIe passthrough in VirtualBox usable for devices that come
and go at run time (Thunderbolt/USB4 enclosures, docks, external GPUs) on a
minimal, hardened Linux host running a Windows guest, without weakening the
isolation between guest, device and host.

Deliverable: a branch plus a merge request that is marked *ready for hardware
test*. Everything that can be verified without the physical hardware has been
verified by then, and the remaining hardware checks are written down as a
step-by-step test plan.

### Non-goals

- Showing the monitors of a passed-through GPU inside the host desktop. A
  passed-through GPU drives its own outputs; that is inherent to passthrough.
- Live migration or saving the state of a device that stays attached.
- Hosts other than Linux, or hosts without a working IOMMU.

## 2. Baseline: what upstream already has

`src/VBox/Devices/Bus/DevPciVfio.cpp` (PDM device `pci-vfio`, ring-3 only,
Linux only, enabled by `VBOX_WITH_VFIO_PCI_PASSTHROUGH`):

- VFIO cdev + IOMMUFD, with fallback to the legacy group/container API
- multi-function devices, config space filtering with per-byte intercept map
- BARs: I/O ports via `pread`/`pwrite`, MMIO either trapped or mapped straight
  into the guest (MMIO2 with existing backing)
- MSI and MSI-X delivered by a poller thread per function, option ROM, legacy
  VGA ranges, one device quirk
- cold attach through the Main API (`IMachine::attachHostPCIDevice`,
  `VBoxManage modifyvm --pci-attach`), requires ICH9 + EFI

What is missing or fragile there (found by reading the code, not yet by test):

| # | Gap | Consequence |
|---|-----|-------------|
| G1 | No handling of device removal on the host (no request/error notification, I/O errors are returned to the guest access path) | Unplugging the device while the VM runs is undefined behaviour |
| G2 | No PCIe hot-plug towards the guest (root port has no hot-plug capability, device has no attach/detach) | Device must be present from VM start to power-off |
| G3 | Saved state is neither supported nor refused | Saving a VM with an attached device silently produces a state that cannot work |
| G4 | Quantities reported by the device are not consistently checked against internal limits | Robustness against unusual or misbehaving devices |
| G5 | DMA mapping failures are only logged; mapping is done lazily on first bus-master enable | Device may run with a partial mapping; long stall in the guest access path |
| G6 | Reset does not bring interrupt state back in sync with the device | Stale interrupt routing after a guest reset |
| G7 | INTx does not work (no unmask after guest EOI) | Devices or guests without MSI/MSI-X do not work |
| G8 | No unit tests, no fuzzing target, documentation block out of date | Regressions go unnoticed |

## 3. Constraints that shape the design

1. **VMM**: I/O port ranges, MMIO ranges and MMIO2 ranges can only be created
   while the VM is being constructed, not at run time. Mapping and unmapping
   existing ranges at run time is supported.
   Consequence: a hot-pluggable slot has to create its resources at VM
   construction. Version 1 therefore binds a slot to a *device layout* (BAR
   sizes and types, function count, interrupt capabilities) that is known at
   VM start, either from the device itself or from a recorded profile. At
   attach time the layout of the arriving device is compared with the slot
   and the attach is refused on any mismatch.
2. **Module boundary**: changes stay in `src/VBox/Devices/Bus` (device, root
   port), `src/VBox/Devices/testcase` (tests) and the thin Main/VBoxManage
   glue. No changes to VMM core unless a work package proves it unavoidable.
3. **Host**: the host is minimal. No dependency on udev, D-Bus, systemd,
   firmware menus or user-space helpers. Only kernel interfaces: VFIO,
   IOMMUFD, sysfs.
4. **Firmware**: an IOMMU (Intel VT-d or AMD-Vi) is required and cannot be
   replaced by software. Nothing beyond that is required from firmware.
5. **Execution engine**: the guest memory is mapped for the device once and
   has to stay at the same place in the VM process for as long as the VM
   exists. Only the native execution engine (KVM) allocates guest memory
   that way, so the device requires it and refuses to be constructed with
   any other one (`VBoxManage modifyvm --vm-execution-engine native-api`).
   Simulation (T4) and hardware tests therefore run with KVM.

## 4. Security requirements

Threat model: the guest is hostile; the passed-through device is untrusted
(it may be arbitrary hardware plugged into an external port); the host
administrator and host kernel are trusted.

| ID | Requirement | Verified by |
|----|-------------|-------------|
| S1 | Never operate without IOMMU protection. VFIO no-IOMMU devices are rejected explicitly. | unit test, simulation |
| S2 | Every value read from the device or from VFIO that is used as a size, count, offset or index is range-checked before use. | unit tests with hostile device model, fuzzing |
| S3 | A DMA mapping failure fails the attach (or the VM start). The device never runs with a partial mapping. | unit test with fault injection |
| S4 | After detach nothing of the device stays reachable from the guest: BARs unmapped, interrupts torn down, DMA mappings removed, file descriptors closed, in that order. | unit test, simulation |
| S5 | Guest-triggerable paths never assert, never return host error codes into the instruction emulation path, and never block an EMT unboundedly. | I/O fuzzing, code review checklist |
| S6 | Configuration strings that end up in host paths are validated against a strict format. | unit test |
| S7 | A device that disappears is treated like a pulled card: reads return all ones, writes are dropped, no host signal can kill the VM process. | unit test, simulation |
| S8 | Saved state with an attached device is refused with a clear error unless the device was detached first. | unit test |
| S9 | No new privileges: the VM process needs nothing beyond access to the VFIO device node(s) and `/dev/iommu` (or the legacy group/container nodes). | review, documentation |
| S10 | New code builds warning-free and passes with AddressSanitizer, LeakSanitizer and UndefinedBehaviorSanitizer in the test runs. | CI, flavor `sanitize` |
| S11 | The guest memory mapped for the device stays in place for the life of the VM; the device refuses to run where that is not guaranteed (constraint 5). | unit test |

## 5. Work packages

Each package is a small series of self-contained commits, has its own tests
and leaves the tree buildable. Order is by dependency and risk.

### WP0 Build and test infrastructure

- CI job building unmodified upstream headless on Linux (reference result).
- CI job building the branch with testcases enabled, running the device tests
  normally and under ASan/UBSan.
- Acceptance: baseline build is green; a deliberately broken test turns CI red.

### WP1 Test harness: device model without hardware

- A fake VFIO kernel interface for the testcase binary only (interposes
  `open`, `ioctl`, `pread`, `pwrite`, `mmap`, `poll`, `eventfd`), driving a
  scriptable PCI device model: config space with capability chains, BARs of
  all types, interrupt counts, option ROM, fault injection per call.
- Device instantiated through the existing `tstDevice` PDM mock
  (`src/VBox/Devices/testcase`), extended where it lacks helpers.
- Acceptance: the unmodified device constructs and destructs against the
  model; coverage of `DevPciVfio.cpp` is reported by CI.

### WP2 Hardening of the existing device (G4, G5, G6, S1, S2, S3, S5, S6)

- Range checks for every device-reported quantity; refuse or degrade cleanly.
- DMA map: fail closed, undo partial work, do it at a defined point instead of
  inside a guest config write.
- Reset: resynchronise interrupt mode, MSI-X mask state and mappings.
- Config string validation, explicit no-IOMMU rejection, descriptor and
  memory leak fixes on error paths, documentation block brought up to date.
- Tests: one regression test per fix, hostile-device corpus, I/O fuzz target
  registered with the existing fuzz testcase.
- Acceptance: all S-requirements that do not depend on hot-plug are covered
  by a failing-before / passing-after test.

### WP3 Removal safety on the host side (G1, S4, S7)

- Listen to the VFIO request and error notifications.
- "Gone" state: accesses complete locally (all ones / dropped), direct BAR
  mappings are replaced by inert backing before the device is released, so
  neither the guest nor a late host access can fault the process.
- Orderly release sequence with a bounded wait for the guest.
- Acceptance: model pulls the device at every point of the life cycle
  (idle, mid-I/O, during interrupt reconfiguration, during reset) without
  crash, hang or leak.

### WP4 Saved state policy (G3, S8)

- Register a saved-state unit that refuses to save while a device is
  attached and records the slot layout when it is empty, so that a restore
  can re-arm the empty slot.
- Acceptance: save with device attached fails with a specific error; save
  with empty slot and restore round-trips.

### WP5 PCIe native hot-plug in the root port (G2, guest side)

Done, see `STATUS.md`. As built:

- Root port configured with `HotPlug`: slot with power controller, attention
  button, indicators, hot-plug surprise; command completed for every command;
  slot events by MSI; access attributes of the specification for the whole
  PCI express capability; no answer from behind the port while the link is
  down.
- The slot adds nothing to the saved state. After loading, the port tells the
  guest what changed in the slot.
- ACPI: `_OSC` grants native hot-plug with the setting
  `PciExpressNativeHotPlug` of the ACPI device, the root bridge is a PCI
  express root bridge then.
- Tests: registers, slot events and interrupts, device behind the port,
  saved state, random accesses; the ACPI table is executed in CI.
- Left for T4: a guest operating system using the slot.

### WP6 Hot-pluggable passthrough slot (G2, device side)

The passthrough device becomes the client of the slot of WP5. One slot holds
one device, which is all functions configured for the passthrough device.

Design:

- The device instance has two states: occupied and empty. Empty is what WP3
  already leaves behind when the host takes a device back: the file
  descriptors are stand-ins which refuse everything, the address ranges of
  the BARs are backed by ordinary memory, the guest gets no answer. WP6 adds
  the way back.
- What the VM is built from at construction is the layout of the device: per
  function the BARs (kind, size, address space flags, whether they can be
  mapped), the place and size of the MSI-X table and pending bits, the size
  of the ROM, whether there is INTx, and the number of functions (constraint
  1: ranges can only be created while the VM is constructed). The address
  space for the BARs is reserved in the process at construction and the
  device memory is mapped over it when a device is attached.
- The layout comes from the device if it is there when the VM starts, else
  from the configuration (written by the management of WP7 when the device
  is assigned). A VM can start with an empty slot only with a layout in the
  configuration and behind a root port with a hot-plug capable slot.
- Attach: open the device, bind it to the I/O address space, compare its
  layout with the one the VM was built from and refuse it if they differ,
  set up config space, capabilities and interrupts like the constructor
  does today, map the BARs over the reserved ranges, put the file
  descriptors in place of the stand-ins, tell the port that a device is
  present. The guest memory mapping for DMA stays in place while the slot is
  empty with IOMMUFD; with the legacy container it is redone on attach.
- Detach on request: if the link is up, press the attention button and wait
  a bounded time for the guest to switch the slot off (link down). Then, or
  at once if the link was down, or after the wait if the caller insists,
  take the device out of service the way WP3 does and tell the port that
  the slot is empty.
- The host wanting a device back (WP3) goes the same way when the device is
  in a hot-plug slot: ask the guest, wait a bounded time, then let go. An
  error reported by the host still means letting go at once.
- Link callback of the port: link down means the guest has no use for the
  device (bus master, decoding and interrupts off, device reset); link up
  after that means the device is in the state after a reset.
- Saved state (second part of WP4): saving is allowed while the slot is
  empty; a state can only be loaded into a VM with the same layout; if a
  device is in the slot when the state was loaded the guest sees it arrive.
- Management reaches attach and detach through a PDM interface of the
  device (used by WP7).

Steps, each with tests in the model and a green CI run:

1. Constructor split into layout, VM structures and attach, without change
   of behaviour (all existing tests pass unchanged).
2. Detach and attach while the VM runs, slot client, link callback; tests:
   loops without leaks, mismatching device refused, guest plays along or
   does not.
3. Start with an empty slot from a configured layout; saved state with an
   empty slot; the host wanting the device back asks the guest first.
4. PDM interface for the management; fuzz test with devices coming and
   going.

- Acceptance: attach/detach loops in the model without leaks; mismatching
  device is refused; guest sees removal and re-arrival in simulation.

### WP7 Management interface

- Run-time attach/detach through the existing Main PCI attachment API and
  `VBoxManage controlvm`; events for device arrival and removal; manual and
  changelog entries.
- "Detach, save, restore, re-attach" sequence as one documented operation.

### WP8 INTx (G7)

- Unmask after guest EOI, using what the interrupt controller emulation and
  the hypervisor backend offer. May require a narrowly scoped VMM helper;
  decided after WP5 when the interrupt paths are fully understood.

## 6. Test strategy

| Tier | What | Where | Needs hardware |
|------|------|-------|----------------|
| T1 | Unit tests against the device model (WP1) incl. fault injection | CI, every push | no |
| T2 | Fuzzing of guest-reachable handlers and of the saved-state loader, ASan/UBSan | CI, time-boxed per run | no |
| T3 | Kernel-level simulation with VFIO sample devices that need no hardware (real VFIO and IOMMUFD code paths, no physical IOMMU) | CI where the runner kernel allows it | no |
| T4 | System simulation: a virtual machine acting as host with an emulated IOMMU and emulated PCIe devices that can be plugged and pulled, VirtualBox running inside | CI if nested virtualisation is sufficient, otherwise documented as not run | no |
| T5 | Real hardware: Thunderbolt/USB4 enclosure with GPU, NVMe, NIC; plug, pull, sleep, resume | human, following `HWTEST.md` | yes |

Every result table in the merge request states for each tier whether it ran
and on which commit. Tiers that could not run are listed as not run, never as
passed.

## 7. Definition of "ready for hardware test"

1. WP0 to WP6 complete; WP7 at least for `VBoxManage`; WP8 optional.
2. T1 and T2 green on the head commit, including sanitizer runs.
3. T3 and T4 either green or listed as not run with the reason.
4. All S-requirements mapped to a passing test or, where only hardware can
   show it, to a step in `HWTEST.md`.
5. Tree builds warning-free; upstream coding guidelines followed; manual,
   VBoxManage documentation and changelog updated.
6. `HWTEST.md` contains the hardware test procedure with expected results
   and the log lines to look for.

## 8. Contribution route

Upstream accepts pull requests only from contributors who signed the Oracle
Contributor Agreement. This branch is published under GPL-3.0-only in this
fork; the merge request lives here. An upstream issue points to it so that
someone entitled to submit can pick it up.

Findings that could be security relevant in upstream code are fixed here with
minimal description, following upstream's `SECURITY.md`.

## 9. Risks and open points

- The layout-bound slot (constraint 1) does not cover plugging in a device
  model the VM has never seen. Lifting that needs VMM support for creating
  ranges at run time; out of scope for version 1, noted for upstream.
- Only the KVM based execution engine is supported (constraint 5). Using the
  VirtualBox kernel driver based engine would need support in the VMM memory
  manager; out of scope for version 1, noted for upstream.
- Nested virtualisation on CI runners may be too limited for T4.
- Thunderbolt/USB4 authorisation and host-side PCI resource allocation are
  host kernel matters and stay outside this work; they are prerequisites
  listed in `HWTEST.md`.
