# Status

Updated: 2026-11-12

## Where things are

| Item | Value |
|------|-------|
| Upstream base | `main` @ `b6bea721` |
| Code branch | `pci-passthrough-hotplug` @ `71037836` (11 commits; the `fixup!` commit gets squashed when the merge request is opened) |
| Notes and CI branch | `meta` (this branch, default branch of the fork) |
| Merge request | not opened yet |
| Throwaway branches | `scratch/*` (tests against the unmodified device, used as evidence) |

## Current step

WP2 hardening, code changes done: interrupt setup; MSI-X table and pending
bits; memory BARs in segments; capability lists; MSI capability; config
space writes; DMA mapping; guest access paths; configuration validation and
cleanup; reset and command register. Next: documentation block, sanitizer
and fuzz runs, then WP3 (removal of the device on the host).

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
- Gap list G1 to G8 and the plan in `PLAN.md`.

## Test evidence

Each new test was first run against the device without the fix it belongs to.

| Test | Before the fix | With fixes (run 43) |
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
device, saved-state stream, legacy group/container API.

## Next (WP2 continued)

1. Documentation block of the device.
2. Sanitizer flavor (ASan/UBSan) in CI; fuzz target (T2).
3. Harness: legacy group/container API, so those paths get covered too.

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

## Test tiers (see PLAN.md section 6)

| Tier | State | Commit | Note |
|------|-------|--------|------|
| T1 unit | running in CI, 29 tests | `71037836` | all pass (run 43) |
| T2 fuzz | not started | | |
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

Run numbers are those GitHub shows for the `build` workflow.
