# Status

Updated: 2026-11-11

## Where things are

| Item | Value |
|------|-------|
| Upstream base | `main` @ `b6bea721` |
| Code branch | `pci-passthrough-hotplug` @ `f31b5dda` (5 commits; the `fixup!` commit gets squashed when the merge request is opened) |
| Notes and CI branch | `meta` (this branch, default branch of the fork) |
| Merge request | not opened yet |
| Throwaway branches | `scratch/*` (tests against the unmodified device, used as evidence) |

## Current step

WP2 hardening, continued. Done so far: interrupt setup; MSI-X table and
pending bits; memory BARs in segments. Next: capability lists, config space
write semantics, DMA mapping, error paths.

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
- Gap list G1 to G8 and the plan in `PLAN.md`.

## Test evidence

Each new test was first run against the device without the fix it belongs to.

| Test | Before the fix | With fixes (run 22) |
|------|----------------|---------------------|
| Construct, config space, BAR, DMA map, destruct | pass | pass |
| MSI delivery | pass | pass |
| MSI vector count (all Multiple Message Enable encodings) | fails, then the process aborts (run 15) | pass |
| MSI-X, 64 vectors, dedicated BAR | fails, 8 checks (run 15) | pass |
| MSI-X, table and PBA inside a BAR with device registers | fails, 9 checks (run 21) | pass |
| Two functions, each with a memory BAR | fails, 3 checks (run 21) | pass |
| BAR which can not be mapped | VM construction fails (run 21) | pass |
| Reset with MSI enabled | fails, 1 check (run 15) | pass |
| Reset with MSI-X enabled | fails, 3 checks (run 15) | pass |
| Unknown device node | pass | pass |

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
  `pwrite`. Regions are backed by a memfd, interrupts by real eventfds, so
  the device's own poller thread and mappings run unmodified. Tracks open
  descriptors and DMA mappings for leak checks, can fail one ioctl on demand
  and can make a device "gone";
- `tstDevPciVfio.cpp`: the tests.

Still to add to the harness: the root port (`ich9pcibridge`) in front of the
device, saved-state stream, legacy group/container API, config space write
semantics in the device model.

## Next (WP2 continued)

1. Capability lists in any order, with loop detection.
2. Config space writes which span registers with different handling, and
   byte writes to registers handled as a whole.
3. DMA mapping: fail closed, at a defined point, with explicit unmap.
4. Error paths: descriptor and memory leaks, host I/O errors in guest access
   handlers, validation of configuration strings, explicit no-IOMMU check.
5. Documentation block of the device.
6. Sanitizer flavor (ASan/UBSan) in CI; fuzz target (T2).

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

## Test tiers (see PLAN.md section 6)

| Tier | State | Commit | Note |
|------|-------|--------|------|
| T1 unit | running in CI, 10 tests | `f31b5dda` | all pass (run 22) |
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
| 18 to 20 | `scratch/msix-shared-bar` | tests | passed | development of the MSI-X and BAR rework |
| 21 | `scratch/red-evidence-2` | tests | failed as intended | three new tests against the driver before the rework, 13 failed checks |
| 22 | code branch @ `f31b5dda` | tests | passed | 10 tests |
