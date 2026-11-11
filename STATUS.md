# Status

Updated: 2026-11-11

## Where things are

| Item | Value |
|------|-------|
| Upstream base | `main` @ `b6bea721` |
| Code branch | `pci-passthrough-hotplug` (pushed, 1 commit: the test harness) |
| Notes and CI branch | `meta` (this branch, default branch of the fork) |
| Merge request | not opened yet |

## Current step

WP1: first CI run of the test harness `tstDevPciVfio` (compile and run).
Expect a few rounds of compile fixes; the harness was written without a
local compiler.

## Done

- WP0: reference build of unmodified upstream is green in CI (run 5):
  headless release build, 12.5 minutes build time, 871 compiler warnings in
  the unmodified tree.
- Read `DevPciVfio.cpp` completely; gap list G1 to G8 in `PLAN.md`.
- Checked VMM limits for run-time creation of I/O, MMIO and MMIO2 ranges
  (creation only during VM construction; PCI region registration likewise;
  config access interception cannot be installed while the VM runs).
- Found that cold attach through Main and `VBoxManage modifyvm --pci-attach`
  already exists upstream.
- Wrote `PLAN.md`.
- CI workflow `build.yml` (started through the API), flavors `baseline` and
  `tests`.
- WP1 code written: `src/VBox/Devices/testcase/tstDevPciVfio{.h,.cpp,Krnl.cpp,Pdm.cpp}`
  and the makefile entry.

## Test harness design (WP1)

One testcase binary, `tstDevPciVfio`, built with `VBOX_WITH_TESTCASES` on
Linux only:

- compiles the real `DevPciVfio.cpp`, `DevPciIch9.cpp`, `MsiCommon.cpp`,
  `MsixCommon.cpp` into the test, so guest config accesses run through the
  real ICH9 bus code;
- `tstDevPciVfioPdm.cpp`: a focused mock of the PDM device helpers these
  files use: CFGM tree, I/O port / MMIO / MMIO2 registries with map state and
  a guest access dispatcher (ECAM for config space), PCI bus registration,
  interrupt log, PDM threads on IPRT threads. Unimplemented helpers trap;
- `tstDevPciVfioKrnl.cpp`: a fake VFIO/IOMMUFD kernel in the test process,
  reached through linker `--wrap` of `open`, `close`, `ioctl`, `pread`,
  `pwrite`. Regions are backed by a memfd, interrupts by real eventfds, so
  the device's own poller thread and mappings run unmodified. Tracks open
  descriptors and DMA mappings for leak checks, can fail one ioctl on demand
  and can make a device "gone";
- `tstDevPciVfio.cpp`: the tests. First set: construct/destruct, config space
  identity, BAR sizing and mapping, DMA map on bus master enable, MSI
  delivery, failing constructor.

Still to add to the harness: the root port (`ich9pcibridge`) in front of the
device, saved-state stream, legacy group/container API, config space write
semantics in the device model.

## Next

1. Get `tstDevPciVfio` compiling and passing against the unmodified device.
2. Add a build cache so that incremental CI runs take a few minutes.
3. WP2: hardening commits, each with a regression test (multi-vector MSI-X
   devices first).
4. Sanitizer build flavor (ASan/UBSan) for the testcase.

## Decisions

| Date | Decision | Reason |
|------|----------|--------|
| 2026-11-11 | No pull request against upstream; merge request inside the fork plus an upstream issue | Upstream requires a signed contributor agreement |
| 2026-11-11 | IOMMU is mandatory, no-IOMMU mode is rejected | Without it a device can reach all host memory |
| 2026-11-11 | Hot-plug slot is bound to a device layout known at VM start | VMM cannot create memory or I/O ranges at run time |
| 2026-11-11 | Builds and tests run on the fork's GitHub Actions | No local Linux build environment |
| 2026-11-11 | Planning files and CI definitions live on `meta`, code on its own branch | Keeps the merge request free of unrelated files |
| 2026-11-11 | Own testcase binary with the real bus code linked in, instead of the generic `tstDevice` framework | Needs a fake kernel interface inside the process; the generic framework loads the real device module and is off by default |
| 2026-11-11 | CI builds the tree without testcases and then only the Devices testcases | Keeps the run short and independent of unrelated testcases |

## Test tiers (see PLAN.md section 6)

| Tier | State | Commit | Note |
|------|-------|--------|------|
| T1 unit | in work | `7a9aba7f` | harness written, first CI run pending |
| T2 fuzz | not started | | |
| T3 kernel simulation | not started | | |
| T4 system simulation | not started | | runner has `/dev/kvm`, kernel 7.0, 4 CPUs, 15 GB RAM |
| T5 hardware | not run | | needs a human with hardware |

## CI runs

| Run | Ref | Flavor | Result | Note |
|-----|-----|--------|--------|------|
| 1 | `main` | baseline | failed | bootstrap of kBuild is not possible from the mirror and not needed (binaries are in the submodule) |
| 2 | `main` | baseline | failed | configure picked libstdc++ headers of another GCC version on the runner |
| 3, 4 | `main` | baseline | failed | workflow script errors (pipe, dispatch raced the push) |
| 5 | `main` | baseline | passed | reference: 12.5 min build, 871 warnings |
| 6 | `pci-passthrough-hotplug` @ `7a9aba7f` | tests | running | first build of the harness |
