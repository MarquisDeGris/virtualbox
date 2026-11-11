# Status

Updated: 2026-11-11

## Where things are

| Item | Value |
|------|-------|
| Upstream base | `main` @ `b6bea721` |
| Code branch | `pci-passthrough-hotplug` (not pushed yet) |
| Notes and CI branch | `meta` (this branch, default branch of the fork) |
| Merge request | not opened yet |

## Current step

- WP0: reference build of unmodified upstream in CI (second attempt running).
- WP1: writing the test harness `tstDevPciVfio` (design below).

## Done

- Read `DevPciVfio.cpp` completely; gap list G1 to G8 in `PLAN.md`.
- Checked VMM limits for run-time creation of I/O, MMIO and MMIO2 ranges
  (creation only during VM construction; PCI region registration likewise;
  config access interception cannot be installed while the VM runs).
- Found that cold attach through Main and `VBoxManage modifyvm --pci-attach`
  already exists upstream.
- Wrote `PLAN.md`.
- CI workflow `build.yml` (started through the API).

## Test harness design (WP1)

One testcase binary, `tstDevPciVfio`, built with `VBOX_WITH_TESTCASES` on
Linux only:

- compiles the real `DevPciVfio.cpp`, `DevPciIch9.cpp`, `MsiCommon.cpp`,
  `MsixCommon.cpp` into the test, so guest config accesses run through the
  real ICH9 bus and root port code;
- a focused mock of the PDM device helpers these files use (about 60
  functions): CFGM tree, I/O port / MMIO / MMIO2 registries with map state,
  PCI bus registration, interrupt sinks that record what was raised, PDM
  threads on IPRT threads, saved-state stream in memory;
- a fake VFIO/IOMMUFD kernel in the test process, reached through linker
  `--wrap` of `open`, `close`, `ioctl`, `pread`, `pwrite`, `mmap`. Regions
  are backed by a memfd, interrupts by real eventfds, so the device's own
  poller thread and mappings run unmodified;
- a scriptable PCI device model (config space with capability chains, BAR
  layout, interrupt counts, option ROM) and fault injection per call
  ("device gone", ioctl failure N, short read).

## Next

1. Get the reference build green; record build time and warnings count.
2. Add a build cache so that incremental CI runs take minutes.
3. Harness skeleton: construct and destruct the unmodified device.
4. First hardening commits with regression tests (WP2).

## Decisions

| Date | Decision | Reason |
|------|----------|--------|
| 2026-11-11 | No pull request against upstream; merge request inside the fork plus an upstream issue | Upstream requires a signed contributor agreement |
| 2026-11-11 | IOMMU is mandatory, no-IOMMU mode is rejected | Without it a device can reach all host memory |
| 2026-11-11 | Hot-plug slot is bound to a device layout known at VM start | VMM cannot create memory or I/O ranges at run time |
| 2026-11-11 | Builds and tests run on the fork's GitHub Actions | No local Linux build environment |
| 2026-11-11 | Planning files and CI definitions live on `meta`, code on its own branch | Keeps the merge request free of unrelated files |
| 2026-11-11 | Own testcase binary with the real bus code linked in, instead of the generic `tstDevice` framework | Needs a fake kernel interface inside the process; the generic framework loads the real device module and is off by default |

## Test tiers (see PLAN.md section 6)

| Tier | State | Commit | Note |
|------|-------|--------|------|
| T1 unit | in work | | harness being written |
| T2 fuzz | not started | | |
| T3 kernel simulation | not started | | |
| T4 system simulation | not started | | runner has `/dev/kvm`, kernel 7.0, 4 CPUs, 15 GB RAM |
| T5 hardware | not run | | needs a human with hardware |

## CI runs

| Run | Ref | Flavor | Result | Note |
|-----|-----|--------|--------|------|
| 1 | `main` | baseline | failed | bootstrap of kBuild is not possible from the mirror and not needed (binaries are in the submodule) |
| 2 | `main` | baseline | running | |
