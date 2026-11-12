# Status

Updated: 2026-11-12

## Where things are

| Item | Value |
|------|-------|
| Upstream base | `main` @ `b6bea721` |
| Code branch | `pci-passthrough-hotplug` @ `6176a66b` (20 commits; the `fixup!` commit gets squashed when the merge request is opened) |
| Notes and CI branch | `meta` (this branch, default branch of the fork) |
| Merge request | not opened yet |
| Throwaway branches | `scratch/*` (tests against the unmodified device, used as evidence) |

## Current step

WP2 (hardening), WP3 (removal of the device on the host), the first part of
WP4 (saved state is refused), WP5 (hot-plug slot in the root port, ACPI) and
the first part of WP6 (the guest is asked before a device is taken out of
the slot) are done. The testcase runs clean with AddressSanitizer,
LeakSanitizer and UndefinedBehaviorSanitizer, including the fuzz tests (T2).
Next: putting a device into the empty slot while the VM runs (WP6).

## Done

- WP0: reference build of unmodified upstream is green in CI: headless
  release build, 12.5 minutes, 871 compiler warnings in the unmodified tree.
  The build output of the base is cached; a test run takes about 100 seconds.
- WP1: test harness `tstDevPciVfio` (see below) compiles warning-free and
  passes against the unmodified device (run 9).
- WP2, part 1 (commits `778d8d88`, `eb637e18`):
  - interrupt setup tracked on the configuring side, derived in one place
    from command / MSI / MSI-X control registers; poller acknowledges mode
    switches by sequence number;
  - eventfds, poll array and the `VFIO_DEVICE_SET_IRQS` buffer sized by the
    vector counts of the device; devices with more vectors than supported
    are refused; the guest never gets more MSI vectors than the device has;
  - read-only bits of the MSI-X message control register are kept;
  - reset masks MSI-X vectors and brings the interrupt setup in line;
  - a passthrough device on the root bus gets the PCI helper pointer it
    needs for MSI-X delivery (it was only set behind a bridge).
- WP2, part 2 (commit `f31b5dda`):
  - the guest gets an MSI-X table and pending bit array of its own at the
    place where the device has them; the table of the physical device is no
    longer reachable by the guest;
  - memory BARs keep the size they have on the device and are made of
    segments which are mapped or trapped; the pages with the MSI-X table and
    pending bits are trapped, the rest of such a page is forwarded;
  - masked vectors become pending and are delivered on unmask (vector and
    function mask); the pending bits are read-only for the guest;
  - BARs which cannot be mapped (VFIO flag, sub-page size) are accessed
    through the device file descriptor instead of failing the VM start;
  - memory BARs are registered for the function they belong to.
- WP2, part 3 (commit `33ba49dc`):
  - capability lists may come in any order; the config space bytes each
    capability claims are tracked, so lists which loop and capabilities
    which overlap are refused at VM start;
  - the extended capability list is checked for loops and bad pointers;
  - a PCI Express capability of an unknown version is hidden.
- WP2, part 4 (commit `05617ea1`):
  - the MSI capability is emulated in the device with the layout the
    physical device has (32 or 64 bit address, with or without per-vector
    masking), so it never covers more config space than on the device;
  - per-vector masking with pending bits, delivery on unmask; the vector
    number goes into the message data when several vectors are enabled;
  - config space writes are taken apart by register: partial writes are
    merged with the current content, writes covering several registers
    reach each of them;
  - reset clears the MSI state.
- WP2, part 5 (commit `f3983a48`):
  - the guest memory is made available to the device when the VM is
    constructed: all of it, or the VM does not start and nothing stays
    mapped;
  - the guest cannot enable bus mastering on a device which has not got
    the guest memory;
  - mappings are removed explicitly when the device is destroyed;
  - the device requires the native execution engine (KVM), where guest
    memory stays at a fixed place for the life of the VM, and refuses to
    be constructed with another one.
- WP2, part 6 (commit `b27efb27`):
  - a device which does not answer reads as all ones and ignores writes on
    all guest access paths (I/O port BARs, legacy VGA ports and memory,
    memory BARs); no host error status reaches the instruction emulation;
  - the legacy VGA memory fill stays inside the video memory;
  - memory BARs are only mapped if they may be read and written;
  - config space handling settings replace each other instead of mixing;
    the device quirk for a config space range leaves capabilities alone;
  - no release assertion is left in the config space read path.
- WP2, part 7 (commit `cb26af23`):
  - the host address of a function has to be a PCI address before it is
    used in paths;
  - all file descriptors are close-on-exec;
  - the option for trapping memory BAR accesses is read where it is
    declared;
  - checks which are not system calls no longer take their status from
    errno;
  - the destructor only undoes what was set up.
- WP2, part 8 (commit `71037836`):
  - a reset of the VM first makes each function keep quiet (no interrupts,
    no bus mastering, no decoding), then resets it;
  - functions which cannot be reset on their own, as is common for devices
    with several functions like graphics cards, are reset through the bus
    they are on; if that is not possible either they stay disabled until
    the guest sets them up again;
  - when the guest clears the decode bits of the command register, the
    guest mappings of the BARs are removed before the device stops
    decoding;
  - trapped accesses to a memory BAR go through the device file, never
    through the mapping of the BAR in the VM process.
- WP2, part 9 (commit `f9f95548`): the documentation block of the device
  describes what is there now.
- CI has a `sanitize` flavor: the testcase, the device and the bus code
  compiled into it are built with AddressSanitizer and
  UndefinedBehaviorSanitizer; memory errors, leaks and undefined behaviour
  fail the run. All 29 tests pass with it (run 46).
- WP2, part 10 (commit `f4691957`): a function's MSI and MSI-X capability
  are taken once; further ones are left out of the guest's capability list.
  Found by the fuzz test with the leak check.
- T2 fuzz test (commit `680a21c5`): random device models (BARs, capability
  lists including broken ones, MSI and MSI-X layouts, extended capabilities,
  ROM, VGA ranges, devices without reset or IOMMU) with random guest
  accesses, interrupts, resets and a device which stops answering. Checked:
  the VM is constructed or nothing is left behind; guest memory mappings
  never change; no request to the kernel beyond the vectors and regions a
  function has; nothing left open or mapped afterwards. Model count and seed
  come from the environment, CI runs 2000 models, 20000 with sanitizers.
- The tests were checked against deliberate defects on throwaway branches:
  an unlimited MSI vector count is caught by its unit test (run 51); an
  off-by-one at the end of the MSI-X table was not caught at first (runs 52,
  55), which led to unit checks at the ends of table and pending bits and to
  the fuzz test aiming at such places; both now catch it (runs 54, 58).
- Harness (commit `7afa04ad`): the fake kernel has the legacy interface with
  container and group as an alternative; construction, guest memory mapping
  and its failure, BAR, MSI, reset, bus reset and descriptor inheritance are
  tested with it, and the fuzz test uses it for a part of its models.
- WP3 (commit `24515f86`): the host taking the device back.
  - the request and error notifications of VFIO are watched by a thread
    which works in every VM state;
  - a function the host wants back or reports an error of is taken out of
    service: guest accesses are answered locally (all ones, writes dropped),
    the device memory mapped into the guest is replaced in place by ordinary
    memory, the device file descriptor is replaced by one which refuses
    everything, which lets go of the device;
  - an EMT then removes the guest mappings; from there on the guest finds
    nothing where the function was;
  - other functions stay in service; a reset leaves such a function alone.
  - Not yet: asking the guest first when the host only asks politely. This
    needs the hot-plug slot of WP6; until then every removal is a surprise
    removal for the guest.
- WP4, first part (commit `f6a1395f`): saving the state of the VM and
  teleporting it are refused while a device is passed through, with a
  message for the user; loading a saved state into a VM which has the device
  is refused too. The second part (saving with an empty slot) comes with
  the hot-plug slot of WP6.
- WP5 (commits `2e543155`, `bd836fb5`): the guest side of hot-plug.
  - A PCI express root port configured with `HotPlug` has a slot for PCI
    express native hot-plug: power controller, attention button, the two
    indicators, hot-plug surprise; commands complete at once; slot events
    are signalled by MSI.
  - The PCI express capability of such a port has the access attributes of
    the specification (read-only, read-write, write-1-to-clear). Other
    bridges are unchanged.
  - While the link to the slot is down (no device, no power, link disabled)
    the guest gets no answer from what is behind the port.
  - A device which comes and goes registers with the port and is told about
    the link. The passthrough device does not do that yet (WP6).
  - Nothing new in the saved state; after loading, the guest is told what
    changed in the slot. A state of a port with another slot is refused.
  - ACPI: with the setting `PciExpressNativeHotPlug` of the ACPI device,
    `_OSC` grants the guest native hot-plug, PME, AER and the PCI express
    capability structure, and the PCI root bridge is a PCI express root
    bridge (`PNP0A08`). Without the setting nothing changes but a compatible
    ID being there.
  - Until WP7 both settings are extra data:
    `VBoxInternal/Devices/ich9pcibridge/<n>/Config/HotPlug` and
    `VBoxInternal/Devices/acpi/0/Config/PciExpressNativeHotPlug`.
- WP6, first part (commit `6176a66b`): the passthrough device behind a root
  port with a hot-plug capable slot is the device in the slot.
  - The guest switching the slot off, or disabling the link, disables and
    resets the device like a reset of the VM.
  - `pciVfioR3SlotDetach()` takes the device out of the slot: the guest is
    asked by the attention button and gets time to switch the slot off; if
    it does not, the device stays or goes the hard way, as the caller says.
    Nothing calls it yet (WP7).
  - The host wanting a function back: the guest is asked first if the
    device still answers and the slot is on, and gets `GuestReleaseTimeoutMs`
    (new setting, 15 seconds by default). An unplugged device, an error and
    a slot which is off empty the slot at once. The whole device goes and
    the guest is told that the slot is empty.
  - Not yet: putting a device back into the slot, starting with an empty
    slot, saved state with an empty slot.
- Gap list G1 to G8 and the plan in `PLAN.md`.

## Test evidence

Each new test was first run against the device without the fix it belongs to.

| Test | Before the fix | With fixes (run 112) |
|------|----------------|---------------------|
| Construct, config space, BAR, DMA map, destruct | pass; with the DMA mapping expectations of part 5: fails, 3 checks (run 33) | pass |
| MSI delivery | pass | pass |
| MSI vector count (all Multiple Message Enable encodings) | fails, then the process aborts (run 15) | pass |
| MSI-X, 64 vectors, dedicated BAR | fails, 8 checks (run 15) | pass |
| MSI-X, table and PBA inside a BAR with device registers | fails, 9 checks (run 20) | pass |
| Two functions, each with a memory BAR | fails, 3 checks (run 20) | pass |
| BAR which can not be mapped | VM construction fails (run 20) | pass |
| Capability list from high to low offsets | VM construction fails (run 26) | pass |
| Capability list which loops | pass | pass |
| Extended capability list which loops | never finishes (run 26) | pass |
| 32-bit MSI without masking, followed by another capability | fails (runs 23, 29) | pass |
| MSI with several vectors and per-vector masking | fails, 4 checks (run 29) | pass |
| Config space writes of all widths | fails, 9 checks (run 29) | pass |
| Reset with MSI enabled | fails, 1 check (run 15) | pass |
| Reset with MSI-X enabled | fails, 3 checks (run 15) | pass |
| Unknown device node | pass | pass |
| Execution engines without fixed guest memory | fails, 6 checks (run 33) | pass |
| Guest memory can not be made available to the device | fails, 10 checks (run 33) | pass |
| I/O port BAR, device which stops answering | fails, 3 checks (runs 37, 38) | pass |
| Legacy VGA ranges of a graphics device | fails, 8 checks (runs 37, 38) | pass |
| Device quirk for a config space range, with and without a capability in it | the process aborts (run 37) | pass |
| BAR which can not be written | the process crashes (run 38) | pass |
| File descriptors are not inherited by child processes | fails, 1 check (run 42) | pass |
| Memory BAR with all accesses trapped on request | fails, 1 check (run 42) | pass |
| Device without IOMMU, device which is not PCI | pass | pass |
| Host address which is not a PCI address | fails, 10 checks (run 42) | pass |
| Guest mappings are gone before the device stops decoding | fails, 2 checks (run 44) | pass |
| Reset of functions which can not be reset on their own | fails, 2 checks (run 44) | pass |
| Trapped BAR accesses do not need the mapping of the BAR | the process crashes (run 44) | pass |
| MSI and MSI-X capability more than once | fails, 8 checks, and leaks memory (run 64) | pass |
| Random device models and guest accesses | found the above (run 57) | pass |
| Legacy interface with container and group | not run against older states | pass |
| The host takes the device back while the VM runs | fails, 26 checks (run 81) | pass |
| Device taken back before power on, device error | fails, 16 checks (run 81) | pass |
| The host takes the device back while the guest is busy with it | fails (run 81) | pass |
| Saved state is refused | fails, 8 checks (run 87) | pass |
| Root port: registers of the hot-plug slot | new feature; deliberate defects are found (runs 92, 95, 98, 99) | pass |
| Root port: slot events and interrupts | new feature; deliberate defects are found (runs 92, 94, 97) | pass |
| Root port: device in the hot-plug slot | new feature; deliberate defects are found (runs 93, 97) | pass |
| Root port: hot-plug slot and saved state | new feature; deliberate defect is found (run 96) | pass |
| Root port: random guest accesses and slot events | new feature; deliberate defect is found (run 98) | pass |
| Hot-plug slot: the guest switches the slot off and on | new feature; deliberate defect is found (run 105) | pass |
| Hot-plug slot: the device is detached | new feature; deliberate defects are found (runs 106, 108, 114) | pass |
| Hot-plug slot: the host wants the device back | new feature; deliberate defects are found (runs 107, 108, 109, 115) | pass |
| ACPI table: hardware ID of the root bridge and `_OSC`, executed with the ACPICA tools | without the change: ID and `_OSC` as before (first half of the check) | pass |

## Test harness (WP1)

One testcase binary, `tstDevPciVfio`, built with `VBOX_WITH_TESTCASES` on
Linux only:

- compiles the real `DevPciVfio.cpp`, `DevPciIch9.cpp`, `MsiCommon.cpp`,
  `MsixCommon.cpp` into the test, so guest config accesses run through the
  real ICH9 bus code;
- `tstDevPciVfioPdm.cpp`: a focused mock of the PDM device helpers these
  files use: CFGM tree, I/O port / MMIO / MMIO2 registries with map state and
  a guest access dispatcher (ECAM for config space), PCI bus registration,
  interrupt log, PDM threads on IPRT threads, VM reset. Unimplemented helpers
  trap;
- `tstDevPciVfioKrnl.cpp`: a fake VFIO/IOMMUFD kernel in the test process,
  reached through linker `--wrap` of `open`, `close`, `ioctl`, `pread`,
  `pwrite`, `mmap`. Regions are backed by a memfd, interrupts by real eventfds, so
  the device's own poller thread and mappings run unmodified. Tracks open
  descriptors and DMA mappings for leak checks, can fail one ioctl on demand
  and can make a device "gone";
- `tstDevPciVfio.cpp`: the tests.

Still to add to the harness: the root port (`ich9pcibridge`) in front of the
device, saved-state stream.

## Next

1. WP6, rest: a device is put into the empty slot while the VM runs; start
   with an empty slot; saved state with an empty slot (second part of WP4);
   interface for the management.
2. WP7: management interface, which also sets the two settings of WP5.
3. T4: a guest operating system using the slot, in CI.

## Decisions

| Date | Decision | Reason |
|------|----------|--------|
| 2026-11-11 | No pull request against upstream; merge request inside the fork plus an upstream issue | Upstream requires a signed contributor agreement |
| 2026-11-11 | IOMMU is mandatory, no-IOMMU mode is rejected | Without it a device can reach all host memory |
| 2026-11-11 | Hot-plug slot is bound to a device layout known at VM start | VMM cannot create memory or I/O ranges at run time |
| 2026-11-11 | Builds and tests run on the fork's GitHub Actions | No local Linux build environment |
| 2026-11-11 | Planning files and CI definitions live on `meta`, code on its own branch | Keeps the merge request free of unrelated files |
| 2026-11-11 | Own testcase binary with the real bus code linked in, instead of the generic `tstDevice` framework | Needs a fake kernel interface inside the process; the generic framework loads the real device module and is off by default |
| 2026-11-11 | CI builds the tree without testcases and then only the Devices testcases; build output of the upstream base is cached | Short runs, independent of unrelated testcases |
| 2026-11-11 | Every fix comes with a test that was seen failing against the unmodified device | Evidence that the test hits the defect |
| 2026-11-11 | The code branch is only ever fast-forwarded; no history rewriting on published branches until the merge request is prepared | Keeps the public history stable |
| 2026-11-11 | INTx stays armed as upstream does although it is not functional yet | Scope; handled in WP8 |
| 2026-11-11 | The MSI capability is emulated in the device instead of using the MSI support of the PCI bus | The bus code knows one capability layout only; the guest has to see the layout of the device |
| 2026-11-11 | The device requires the native execution engine (KVM) | Guest memory has to stay at a fixed place in the VM process while a device can access it; only this engine guarantees that |
| 2026-11-11 | Guest memory is mapped for the device when the VM is constructed, not when the guest enables bus mastering | A failure stops the VM start with a clear error instead of leaving a device which half works |
| 2026-11-11 | A device which does not answer never produces an error status in a guest access path | Same behaviour as an absent device; the guest cannot be blamed and the VM must keep running |
| 2026-11-12 | Trapped BAR accesses go through the device file, the mapping of a BAR is only used for mapping it into the guest | The host kernel invalidates the mapping at times; a file access fails cleanly where touching the mapping raises a signal |
| 2026-11-12 | A function which cannot be reset on its own is reset through its bus; without any reset it is left disabled | Devices with several functions usually have no function reset; state must not carry over a guest reboot unnoticed |
| 2026-11-12 | The fuzz test is part of the testcase binary and generates device models itself instead of mutating bytes | Most random bytes are refused early; made-up models which are valid most of the time reach the code behind the checks. Deterministic by seed, so a failure can be repeated |
| 2026-11-12 | Tests are checked against deliberate defects now and then | A test which cannot fail proves nothing |
| 2026-11-12 | The notifications of the host are watched by a thread of the device's own, not a PDM thread | PDM threads stand still while the VM is paused; the host waits for the device to be let go of |
| 2026-11-12 | Device memory mapped into the guest is replaced in place by ordinary memory when the device goes away; the file descriptor is replaced in place too | Everybody holding the address or the descriptor number stays valid without locking, and nothing can touch the device afterwards |
| 2026-11-12 | A function with an error the host cannot recover from is let go of, the VM keeps running | The alternative, stopping the VM, takes down everything else the guest does; the guest sees a device which failed |
| 2026-11-12 | The hot-plug slot has a power controller, an attention button, both indicators and is surprise capable; commands complete at once | This is the slot of the root ports of other virtual machines, which Windows and Linux guests are known to handle; a Windows guest cannot be tried before the hardware test |
| 2026-11-12 | Slot events are signalled by MSI only, the root port gets no interrupt pin | Allowed by the specification; passthrough depends on MSI anyway; no new interrupt routing |
| 2026-11-12 | Only root ports configured with `HotPlug` get the strict access attributes for the PCI express capability | The guest takes control of the capability with native hot-plug; all other bridges and with them all existing VMs stay as they are |
| 2026-11-12 | The port itself keeps the guest from what is behind it while the link is down | Does not depend on the device behind the port doing it right |
| 2026-11-12 | The slot adds nothing to the saved state; after loading, the port compares what is in the slot with what the guest saw last | The config space is all the guest knows; a device which was put in or taken out while the state was on disk is an ordinary hot-plug event |
| 2026-11-12 | `_OSC` grants native hot-plug, PME, AER and the capability structure together, and only with a setting of its own; the root bridge is `PNP0A08` then | Same as in other virtual machines Windows guests do native hot-plug with; VMs without the setting keep their ACPI behaviour |
| 2026-11-12 | The ACPI table is checked in CI by executing it with the ACPICA tools | The table is the one part of WP5 the device testcase cannot reach |
| 2026-11-12 | Before a device in a hot-plug slot is taken away the guest is asked by the attention button and gets a bounded time; then the device goes anyway if the host waits for it or the caller insists | The guest can stop using the device in an orderly way; a guest which does not react must not keep the host or the user waiting for ever |
| 2026-11-12 | The attention button is not pressed again within ten seconds | Guests take a second press within five seconds for the request being taken back, and a press on a slot which is off for a request to switch it on |
| 2026-11-12 | In a hot-plug slot the whole device goes when the host wants one function back or reports an error of one | A slot holds one device; the guest cannot be told about a part of it |
| 2026-11-12 | A device which no longer answers when the host wants it back is taken out of the slot at once | It was unplugged; there is nothing the guest could do in an orderly way |
| 2026-11-12 | The link to the slot going down resets the device | The guest expects a device in its reset state when it switches the slot on again, as after a power cycle |

## Test tiers (see PLAN.md section 6)

| Tier | State | Commit | Note |
|------|-------|--------|------|
| T1 unit | running in CI, 42 tests, ACPI table check | `6176a66b` | all pass (run 112); clean with ASan, LSan and UBSan (run 113) |
| T2 fuzz | running in CI | `6176a66b` | 2000 models and 400000 operations on the root port per run, 20000 and 2000000 with sanitizers (run 113); 200000 models with another seed and sanitizers (run 70, before WP3) |
| T3 kernel simulation | not started | | |
| T4 system simulation | not started | | runner has `/dev/kvm`, kernel 7.0, 4 CPUs, 15 GB RAM |
| T5 hardware | not run | | needs a human with hardware |

## CI runs

| Run | Ref | Flavor | Result | Note |
|-----|-----|--------|--------|------|
| 1 to 4 | `main` | baseline | failed | workflow problems (kBuild bootstrap not needed, libstdc++ header mix on the runner, script errors) |
| 5 | `main` | baseline | passed | reference: 12.5 min build, 871 warnings |
| 6 | code branch | tests | failed | build needs the product revision, which fork commits do not carry; now passed in by the workflow |
| 7 | `main` | baseline | passed | fills the build cache |
| 8 | code branch @ `7a9aba7f` | tests | failed | one compile error in the harness |
| 9 | code branch @ `5ee30cb8` | tests | passed | harness green against the unmodified device, 100 s |
| 10 | `scratch/irq-vectors` | tests | failed as intended | MSI vector count test against the unmodified device |
| 11 to 14 | `scratch/irq-vectors` | tests | mixed | development of the interrupt rework |
| 15 | `scratch/red-evidence` | tests | failed as intended | all new tests against the unmodified device |
| 16 | code branch @ `eb637e18` | tests | passed | 7 tests |
| 17 | `scratch/msix-shared-bar` | tests | failed as intended | MSI-X shared BAR test before the fix |
| 18, 19 | `scratch/msix-shared-bar` | tests | passed | development of the MSI-X and BAR rework |
| 20 | `scratch/red-evidence-2` | tests | failed as intended | three new tests against the driver before the rework, 13 failed checks |
| 21 | code branch @ `f31b5dda` | tests | passed | 10 tests |
| 22, 24 | `scratch/caps` | tests | failed | development of the capability list handling |
| 23 | `scratch/red-evidence-3` | tests | failed as intended | small MSI capability test against the driver before the change |
| 25 | `scratch/caps` | tests | passed | 13 tests |
| 26 | `scratch/red-evidence-4` | tests | failed as intended | capability list tests against the driver before the change; ran into the time limit |
| 27 | code branch @ `33ba49dc` | tests | passed | 13 tests |
| 28 | code branch @ `05617ea1` (as `scratch/msi`) | tests | passed | 16 tests |
| 29 | `scratch/red-evidence-5` | tests | failed as intended | three new tests against the driver before the MSI emulation, 14 failed checks |
| 30 | code branch @ `05617ea1` | tests | passed | 16 tests; first run with the watchdog for hanging tests |
| 31 | `scratch/red-evidence-4` | tests | failed as intended | hanging test stopped after three minutes with a backtrace |
| 32 | code branch @ `f3983a48` (as `scratch/dma`) | tests | passed | 18 tests |
| 33 | `scratch/red-evidence-6` | tests | failed as intended | DMA mapping tests against the driver before the change, 19 failed checks |
| 34 | code branch @ `b27efb27` (as `scratch/access`) | tests | passed | 22 tests |
| 35, 36 | `scratch/red-evidence-7`, `-8` | tests | cancelled | superseded by 37 and 38 |
| 37 | `scratch/red-evidence-7` | tests | failed as intended | access path tests against the driver before the change, quirk test last |
| 38 | `scratch/red-evidence-8` | tests | failed as intended | same with the read-only BAR test last |
| 39, 40 | `scratch/config`, `scratch/red-evidence-9` | tests | failed | a mistake in the fake kernel (copies of eventfds lost a flag) |
| 41 | code branch @ `cb26af23` (as `scratch/config`) | tests | passed | 26 tests |
| 42 | `scratch/red-evidence-9` | tests | failed as intended | configuration tests against the driver before the change, 12 failed checks |
| 43 | code branch @ `71037836` (as `scratch/reset`) | tests | passed | 29 tests |
| 44 | `scratch/red-evidence-10` | tests | failed as intended | reset and command register tests against the driver before the change |
| 45 | code branch @ `f9f95548` (as `scratch/docs`) | tests | passed | 29 tests |
| 46 | code branch @ `f9f95548` (as `scratch/docs`) | sanitize | passed | 29 tests, no reports from the sanitizers |
| 47 to 50 | `scratch/fuzz` | tests, sanitize | passed | first version of the fuzz test, up to 20000 models |
| 51 | `scratch/mutant-1` | sanitize | failed as intended | deliberate defect, caught by a unit test |
| 52, 55 | `scratch/mutant-2`, `-2b` | sanitize, tests | passed | deliberate defect not caught: tests improved |
| 53 | `scratch/fuzz` | sanitize | failed | configure ran into a time limit on the runner; it gets three attempts now |
| 54, 58 | `scratch/mutant-2c`, `-2d` | tests | failed as intended | the deliberate defect is caught by the unit test and by the fuzz test alone |
| 56 | `scratch/fuzz` | sanitize | passed | fuzz test aiming at edges |
| 57 | `scratch/fuzz` | sanitize | failed | the fuzz test found a leak with a capability being there twice |
| 59 to 61 | `scratch/dupcaps`, `scratch/red-evidence-12` | tests, sanitize | failed | compile error in the new test |
| 62, 63 | code branch @ `f4691957` (as `scratch/dupcaps`) | tests, sanitize | passed | 30 tests |
| 64 | `scratch/red-evidence-12` | sanitize | failed as intended | duplicate capability test against the driver before the change |
| 65 to 67 | `scratch/fuzz2` | tests, sanitize | failed, cancelled | a merge done wrong |
| 68, 69 | code branch @ `680a21c5` (as `scratch/fuzz2`) | tests, sanitize | passed | 30 tests and the fuzz test |
| 70 | code branch @ `680a21c5` | sanitize | passed | 200000 random device models with another seed |
| 71, 72 | code branch @ `7afa04ad` (as `scratch/legacy`) | tests, sanitize | passed | 31 tests and the fuzz test |
| 73 to 78 | `scratch/removal` | tests, sanitize | failed, cancelled | development of WP3 |
| 79, 80 | `scratch/removal` | tests, sanitize | passed | 34 tests and the fuzz test |
| 81 | `scratch/red-evidence-13` | tests | failed as intended | removal tests against the driver before the change |
| 82 | `scratch/mutant-3` | tests | failed as intended | deliberate defect (device memory mappings not replaced), caught |
| 83, 84 | code branch @ `24515f86` | tests, sanitize | passed | 34 tests and the fuzz test |
| 85, 86 | code branch @ `f6a1395f` (as `scratch/ssm`) | tests, sanitize | passed | 35 tests and the fuzz test |
| 87 | `scratch/red-evidence-14` | tests | failed as intended | saved state test against the driver before the change |
| 88 to 90 | `scratch/hotplug` | tests | failed | development of WP5 (test did not compile; a check of the new fuzz test was too strict; option of a CI tool) |
| 91, 100 | `scratch/hotplug` | tests, sanitize | passed | 39 tests, two fuzz tests, ACPI table check |
| 92 to 99 | `scratch/mutant-hp1` to `scratch/mutant-hp8` | tests | failed as intended | eight deliberate defects in the hot-plug slot, each one caught |
| 101, 102 | code branch @ `bd836fb5` | tests, sanitize | passed | 39 tests, two fuzz tests, ACPI table check |
| 103 | `scratch/slotclient` | tests | passed | first part of WP6, 42 tests |
| 104 | `scratch/slotclient` | sanitize | failed | a gap of the test harness: with the sanitizers the compiler uses a variant of `pread` the fake kernel did not catch |
| 105 to 109 | `scratch/mutant-sc1` to `scratch/mutant-sc5` | tests | failed as intended | five deliberate defects in the slot handling of the passthrough device, each one caught |
| 110, 111 | `scratch/mutant-sc6`, `scratch/mutant-sc7` | tests | failed to build | the two defects did not compile |
| 112, 113 | code branch @ `6176a66b` (as `scratch/slotclient`) | tests, sanitize | passed | 42 tests, two fuzz tests, ACPI table check |
| 114, 115 | `scratch/mutant-sc6b`, `scratch/mutant-sc7b` | tests | failed as intended | the two defects again, each one caught |

Run numbers are those GitHub shows for the `build` workflow.
