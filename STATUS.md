# Status

Updated: 2026-11-13

## Where things are

| Item | Value |
|------|-------|
| Upstream base | `main` @ `b6bea721` |
| Code branch | `pci-passthrough-hotplug` @ `c5cc9847` (28 commits, one of them a `fixup!`) |
| Branch of the merge request | `pci-passthrough-hotplug-v1` @ `52582e7d`: the code branch with the `fixup!` commit folded into the commit it belongs to, 27 commits, the same tree (`5501632b`) |
| Notes and CI branch | `meta` (this branch, default branch of the fork) |
| Merge request | [#1](https://github.com/MarquisDeGris/virtualbox/pull/1), inside this fork, a draft: WP0 to WP6. The management (WP7) is not in it, see below |
| Throwaway branches | `scratch/*` (tests against the unmodified device, used as evidence) |

## Current step

WP2 (hardening), WP3 (removal of the device on the host), WP4 (saved state:
refused while the device is in the VM, possible while the hot-plug slot is
empty), WP5 (hot-plug slot in the root port, ACPI) and WP6 (the passthrough
device in the slot) are done: the guest is asked before a device is taken out
of the slot, a device can be put into the empty slot while the VM runs, the
VM can start with an empty slot, its state can be saved and loaded while the
slot is empty, and there is an interface for the management. The testcase has
49 tests and three random tests and runs clean with AddressSanitizer,
LeakSanitizer and UndefinedBehaviorSanitizer (T2). Next: WP7, the management
in Main and `VBoxManage`.

### Not built, not tested

Since 2026-11-13 GitHub does not start workflow runs for this repository
("Actions has been disabled for this repository"), and the workflow is the
only way this work gets built and tested. Everything up to the code branch
@ `c5cc9847` was built and tested before (runs 212, 213, and 180 for the
full build of an earlier state). What was written after that is on
`scratch/mgmt`, on top of the code branch, and has seen no compiler:

- The interfaces of the passthrough device are found at LUN 0 of the device
  instance, which is where Main looks for the interfaces of a device
  (`PDMR3QueryDeviceLun`); with a test.
- Host PCI addresses are ordered by their domain first
  (`PCIBusAddress::operator<`). With addresses from more than one domain
  the order was not a consistent one, and the configuration of a VM is
  built in that order. With a test, which makes it 50 tests and three
  fuzz tests once they can run.
- WP7, the management, as a first complete draft:
  - Hot-plug is switched on for a VM with the extra data item
    `VBoxInternal2/HostPCIHotPlug`. Main then gives the root port in front
    of each PCI device of the host the hot-plug capable slot and has the
    ACPI tables grant native hot-plug. The device has to be attached as
    device 0 of guest bus 1 or 2.
  - After the VM was created Main asks each passthrough device for its
    layout and stores it as extra data
    (`VBoxInternal2/HostPCILayout/<host address>`); it goes into the
    configuration at the next start, so the VM starts without the device.
  - `IConsole::plugHostPCIDevice`, `unplugHostPCIDevice` and
    `getHostPCIDeviceState`; `VBoxManage controlvm <vm> plugpci` and
    `unplugpci`, the state in `VBoxManage showvminfo`; man pages.
  - Main cannot be run in the test without hardware. What it does is
    part of the hardware test.
  - Changelog entries.
  - Checked without a compiler, which is no substitute for one: the XML
    files which were changed (API definition, the two man pages, the
    changelog) have their elements balanced; the two new subcommands
    leave `controlvm` within the number of help scopes its generated
    help can have (55 of 60); the signatures of the new `IConsole`
    methods follow what the wrapper generator makes of comparable ones.

[HWTEST.md](HWTEST.md) has the procedure for the hardware test as a
draft.

Nothing of it goes onto the code branch before it was built and tested.

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
- WP6, second part (commit `defa244c`): `pciVfioR3SlotAttach()` puts the
  configured device into the empty slot while the VM runs.
  - The VM is built around the device it was started with (BARs, place of
    the MSI-X table, ROM, interrupts); a device which differs is refused.
  - The device file takes the place of the stand-in, the device memory is
    mapped where the guest mappings are, the config space the guest sees is
    put together anew; all functions or none.
  - The device starts out disabled and reset, the guest is told that the
    slot is occupied.
  - With the legacy interface the group stays with the VM while the slot
    is empty, so the host cannot use the device for something else.
  - Nothing calls it yet (WP7).
- WP6, third part (commit `949a5c03`): the VM can start without the device.
  - Each function can have a `Layout` node in its configuration which says
    what to build the VM from: BARs, ROM, interrupts, place of the MSI-X
    table. `pciVfioR3QueryLayout()` returns it for a VM which was started
    with the device; storing it is for the management to do (WP7).
  - If the device cannot be opened and there is a layout, the VM is built
    from it and the slot starts empty; `pciVfioR3SlotAttach()` puts the
    device in later if it matches.
  - Needs the hot-plug slot; all functions of a device are there or none;
    a device which is there at the start is what counts.
- A message of the PCI bus (commit `48f771d9`): a saved state which has a
  device with another vendor ID where the VM has one is refused with a
  message again. The message was given the IDs where its format takes the
  config spaces, so it could not be put together and loading did not get
  as far as refusing. Found by the tests of the fourth part of WP6.
- Loading a saved state leaves MMIO2 pages alone which are zero already
  (commit `2ef64145`, in PGM). The memory which keeps the place of the
  BARs of a device which is not there stays without host memory behind it
  when a state is loaded. The device testcase does not reach PGM, so this
  has no test of its own. Only a baseline build compiles it, which run 180
  did.
- WP6, fourth part, which is the second part of WP4 (commit `fb6dca3a`):
  the state of the VM can be saved and the VM can be teleported while the
  hot-plug slot is empty.
  - Refused while the device is in the slot, as before. With the slot
    empty, what an EMT has left to do about the device is done first and
    what the config space has of the device is cleared, so the PCI bus
    saves the same whether the VM was started with the device or without.
  - The saved state unit of the device has the layout the VM is built
    from. A state is only loaded into a VM which is built from the same.
  - If the VM which loads a state has the device, the device is taken out
    of the slot before anything is loaded and put back afterwards; the
    guest sees a device being plugged in. The reason is the memory: PGM
    reads all MMIO2 pages when it saves a state and writes to them when
    it loads one, and the BARs mapped into the guest are such pages. That
    has to be the ordinary memory keeping their place, never the memory
    of a device.
  - A device is put into the slot only while the VM is running or
    suspended, so not while a state is being saved or loaded.
  - One critical section for everything which is done about the slot
    being empty. This also closes two gaps of the second part for VMs
    with several virtual CPUs (the EMT part of a removal running next to
    a device being put in, or twice at the same time).
  - If ordinary memory cannot be put into the place of device memory when
    a device is let go of, no state is saved or loaded until the next
    device was put in and taken out.
  - Limit: a live save (teleportation, snapshot of a running VM) has PGM
    scan the memory keeping the place of the BARs in every pass, which
    takes time and bookkeeping memory in proportion to the size of the
    BARs.
- WP6, fifth part (commit `80a60fc9`): an interface for whoever manages the
  VM, `PDMIPCIPASSTHROUGHPORT` in `include/VBox/vmm/pdmifs.h`, which the
  device has on its base interface.
  - It tells whether the device is in a hot-plug capable slot, whether the
    PCI device of the host is in the VM and whether the guest can reach it;
    it puts the device into the slot and takes it out.
  - It tells the layout of a function as values with the names they have
    in the configuration. The management stores them and puts them into
    the configuration as they are, without knowing what they mean.
  - The tests get at the device through it, the way the management does.
- Interrupts after the VM was suspended and resumed (commit `07cbc95b`):
  the thread waiting for the interrupts of a function is entered anew each
  time the VM gets going and put together what to wait for only when the
  interrupt mode had changed. After a suspend and resume, which is also
  what a snapshot does, the interrupts of the device no longer reached the
  guest. The driver has had this all along; the random test below found
  it.
- WP6, sixth part (commit `c5cc9847`): a random test for the device in the
  slot. Guest, management, host and VM do things in random order: accesses,
  devices which come and go in every way there is, another kind of device
  plugged in, resets, suspending, saving and loading states. After each
  operation it checks what has to hold whatever happened, among it that a
  device in the slot is reached by the guest and gets its interrupts
  through. One case it does not get to has a test of its own: a state is
  loaded while the host waits for the device.
- Gap list G1 to G8 and the plan in `PLAN.md`.

## Test evidence

Each new test was first run against the device without the fix it belongs to.

| Test | Before the fix | With fixes (run 212) |
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
| Hot-plug slot: a device is put into the slot | new feature; deliberate defects are found (runs 120 to 126, 130) | pass |
| Hot-plug slot: the VM starts without the device | new feature; deliberate defects are found (runs 133, 134, 136 to 139, 141, 142) | pass |
| Hot-plug slot: saved state with the slot being empty | new feature; deliberate defects are found (runs 167 to 169, 171, 172) | pass |
| Hot-plug slot: saved states which are refused | new feature; deliberate defects are found (runs 166 to 168, 170 to 179) | pass |
| Hot-plug slot: a state is loaded while the host waits for the device | a deliberate defect which went unnoticed before (run 211) is found (run 214) | pass |
| Hot-plug slot: the interface for the management | new feature; deliberate defects are found (runs 185 to 192) | pass |
| Interrupts after the VM was suspended and resumed | fails, 14 checks (run 202) | pass |
| Hot-plug slot: random operations with devices coming and going | new test; found the interrupts lost after suspend and resume (run 195); deliberate defects are found (runs 207, 215) | pass |
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
  interrupt log, PDM threads on IPRT threads, VM reset, suspend and resume,
  the VM state. Saved states are saved and loaded for real, the way SSM
  does it: preparation, the data of each unit as a stream of typed items,
  completion, with a unit standing in for PGM which reads every MMIO2 page
  when saving and writes to every one when loading. A test can change an
  item of a state or take a unit out. Unimplemented helpers trap;
- `tstDevPciVfioKrnl.cpp`: a fake VFIO/IOMMUFD kernel in the test process,
  reached through linker `--wrap` of `open`, `close`, `ioctl`, `pread`,
  `pwrite`, `mmap`. Regions are backed by a memfd, interrupts by real eventfds, so
  the device's own poller thread and mappings run unmodified. Tracks open
  descriptors and DMA mappings for leak checks, can fail one ioctl on demand,
  can make a device "gone", and can fail or announce the mapping which puts
  ordinary memory into the place of device memory;
- `tstDevPciVfio.cpp`: the tests.

The root port (`ich9pcibridge`) in front of the device is part of the
harness since WP5, saved states since the fourth part of WP6.

## Next

1. WP7: the management in Main and `VBoxManage`, which uses the interface
   of the device, sets the two settings of WP5 and stores the layout.
2. T4: a guest operating system using the slot, in CI.

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
| 2026-11-12 | A device put into the slot has to match the device the VM was started with in BARs, place of the MSI-X table, ROM size and interrupts | The VM is built around these at construction and cannot be rebuilt while it runs |
| 2026-11-12 | Putting a device into the slot is done on an EMT, with the functions out of service until everything is in place | Nothing of the VM can get in the way, and the paths of the guest stay closed by the means which closed them when the device went |
| 2026-11-12 | The device file of a device put into the slot takes the number of the stand-in | Everybody holding the number stays valid, as when the device went |
| 2026-11-12 | With the legacy interface the group stays with the VM while the slot is empty | The guest memory mapping belongs to the container and would be lost with the group; the current interface (IOMMUFD) has no such tie |
| 2026-11-12 | A VM which starts without the device takes what it is built from out of its configuration, in the form a VM started with the device reports | The exact view VFIO has of the device, not one derived from elsewhere; a device which does not match is refused when it is put in |
| 2026-11-12 | A device which is there when the VM starts is what counts, a configured layout is ignored then | The layout in the configuration may be out of date; the management stores a fresh one |
| 2026-11-12 | Starting without the device needs the hot-plug slot, and all functions of the device are there or none | Without the slot the guest could not be told when the device arrives; a slot holds one device |
| 2026-11-12 | The state of the VM is saved only while the hot-plug slot is empty; a device which is there is taken out of the slot before a state is loaded and put back afterwards | PGM reads all MMIO2 pages when saving and writes to them when loading, and the BARs mapped into the guest are such pages; this must never be the memory of a device |
| 2026-11-12 | Before a state is saved or loaded the config space of the functions of the empty slot has nothing of a device, the vendor ID reads 0xffff | The PCI bus saves the config space and refuses a state with another vendor ID; a VM started with the device and one started without it have to look the same |
| 2026-11-12 | The saved state unit of the device holds the layout of each function and nothing else; a state with another layout is refused | The VM is built around the layout; the PCI bus and PGM notice only some of the differences |
| 2026-11-12 | A device is put into the slot only in the VM states running and suspended | The VM state changes before the saved state callbacks run, which keeps the slot empty for as long as a state is saved or loaded |
| 2026-11-12 | One critical section serializes putting a device in, taking functions out of service, the EMT part of a removal and the saved state callbacks | Several EMTs and the watcher thread can be at these at the same time |
| 2026-11-12 | PGM leaves MMIO2 pages alone which are zero when a state is loaded | Writing zeros to the memory keeping the place of the BARs makes the host back it, which can be many gigabytes nobody needs |
| 2026-11-12 | The mock saves and loads states as a stream of typed items, with a unit standing in for PGM which writes to every MMIO2 page | A get which does not match its put shows, and so does a load which reaches device memory |
| 2026-11-12 | The management gets the layout of a function as values under the names they have in the configuration and stores them as they are | Nothing outside the device has to know what a layout consists of, and nothing has to change there when it gets another value |
| 2026-11-12 | A random test for the device in the slot with checks after every operation, next to the tests for single cases | The cases nobody thought of are sequences; the first run with interrupts in it found a defect the driver has had all along |

## Test tiers (see PLAN.md section 6)

| Tier | State | Commit | Note |
|------|-------|--------|------|
| T1 unit | running in CI, 44 tests, ACPI table check | `949a5c03` | all pass (run 131); clean with ASan, LSan and UBSan (run 132) |
| T2 fuzz | running in CI | `949a5c03` | 2000 models and 400000 operations on the root port per run, 20000 and 2000000 with sanitizers (run 132); 200000 models with another seed and sanitizers (run 70, before WP3) |
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
| 116 to 119 | `scratch/slotattach` | tests, sanitize | passed | second part of WP6, 43 tests |
| 120 to 126 | `scratch/mutant-sa1` to `scratch/mutant-sa7` | tests | failed as intended | seven deliberate defects in putting a device into the slot, each one caught |
| 127 | `scratch/mutant-sa8` | tests | passed | an eighth defect went unnoticed: the test did not have a device unplugged which was put in after an unplugging |
| 128, 129 | code branch @ `defa244c` (as `scratch/slotattach`) | tests, sanitize | passed | 43 tests with the missing case, two fuzz tests, ACPI table check |
| 130 | `scratch/mutant-sa8b` | tests | failed as intended | the eighth defect, caught now |
| 131, 132 | code branch @ `949a5c03` (as `scratch/slotempty`) | tests, sanitize | passed | third part of WP6, 44 tests, two fuzz tests, ACPI table check |
| 133 to 142 | `scratch/mutant-se1` to `scratch/mutant-se8`, `-se3b`, `-se8b` | tests | failed as intended | eight deliberate defects in starting with an empty slot, each one caught (runs 135 and 140 did not build, repeated as 141 and 142) |
| 143, 144 | `scratch/slotstate` | tests, sanitize | passed | fourth part of WP6, 46 tests |
| 145 to 147 | `scratch/slotstate` | tests, sanitize | passed | more cases: device memory which stays mapped, a state without the unit of the device |
| 148 to 160 | `scratch/mutant-st1` to `scratch/mutant-st13` | tests | failed as intended, 154 passed | thirteen deliberate defects in the saved state support. Twelve caught, two of these (149, 153) by a crash in the message of the PCI bus about another vendor ID, which is a defect of its own and got fixed. The seventh (154) changed nothing a guest or a state could tell and was replaced |
| 161, 162 | `scratch/slotstate` | tests, sanitize | passed | a device is let go of under the critical section of the slot, with a test |
| 163 | `scratch/mutant-st14` | tests | failed as intended | a fourteenth defect (a device is let go of without the critical section), caught |
| 164, 165 | `scratch/slotstate` | tests, sanitize | passed | with the message of the PCI bus fixed and a state with another vendor ID as a test; the tree of the code branch @ `fb6dca3a` |
| 166 to 179 | `scratch/mutant-su1` to `scratch/mutant-su14` | tests | failed as intended | all fourteen deliberate defects on the final tree, each one caught by a failing test, none by a crash |
| 181, 182 | code branch @ `fb6dca3a` | tests, sanitize | passed | 46 tests, two fuzz tests, ACPI table check |
| 180 | code branch @ `fb6dca3a` | baseline | passed | full build, the only kind which compiles the PGM change. 875 warning lines, none in a file the branch changes; `main` had 871 and 874 in runs 5 and 7, the number depends on how lines of parallel jobs fall together |
| 183, 184 | `scratch/slotifc` | tests, sanitize | passed | fifth part of WP6, 47 tests; the tree of the code branch @ `80a60fc9` |
| 185 to 192 | `scratch/mutant-if1` to `scratch/mutant-if8` | tests | failed as intended | eight deliberate defects in the interface for the management, each one caught |
| 193, 194 | `scratch/slotfuzz` | tests, sanitize | passed | first version of the third random test |
| 195, 196 | `scratch/slotfuzz` | tests, sanitize | failed | the random test with interrupts of the device in it: one did not arrive after the VM was suspended and resumed |
| 197 to 201 | `scratch/mutant-fz1` to `scratch/mutant-fz5` | tests | failed | of no use: started before run 195 was looked at, they fail for its reason |
| 202 | `scratch/red-evidence-15` | tests | failed as intended | the test for interrupts after suspend and resume against the driver without the fix |
| 203, 204 | `scratch/irqresume` | tests, sanitize | passed | with the fix, 48 tests; the tree of the code branch @ `07cbc95b` |
| 205, 206 | `scratch/slotfuzz2` | tests, sanitize | passed | the random test on top of the fix; 40000 operations in run 205: the device put in 1034 times, taken by the host 503 times, 837 states saved, 844 loaded, 713 interrupts of the device |
| 207 to 211 | `scratch/mutant-fy1` to `scratch/mutant-fy5` | tests | 207, 209, 210 failed as intended; 208, 211 passed | five deliberate defects which take a sequence of events to show. One is caught by the random test alone (207), two by other tests (209, 210). One changes nothing (208: the hold-off of the attention button is cleared when the link goes down as well). One went unnoticed (211: the watcher thread is not woken when a device is put back after a state was loaded), no test got to that |
| 212, 213 | `scratch/slotfuzz2` | tests, sanitize | passed | with a test for that case: 49 tests and three random tests; the tree of the code branch @ `c5cc9847` |
| 214, 215 | `scratch/mutant-fy5b`, `scratch/mutant-fy1b` | tests | failed as intended | the defect which went unnoticed is caught by the new test, the other one by the random test again |

Run numbers are those GitHub shows for the `build` workflow.
