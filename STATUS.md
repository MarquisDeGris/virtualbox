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

WP0: get a reference build of unmodified upstream running in CI.

## Done

- Read `DevPciVfio.cpp` completely; gap list G1 to G8 in `PLAN.md`.
- Checked VMM limits for run-time creation of I/O, MMIO and MMIO2 ranges
  (creation only during VM construction); design consequence in `PLAN.md`.
- Found that cold attach through Main and `VBoxManage modifyvm --pci-attach`
  already exists upstream.
- Wrote `PLAN.md`.

## Next

1. CI workflow: headless reference build of upstream `main` (WP0).
2. CI workflow: build with testcases, run `tstDevice*` (WP0).
3. Fake VFIO interface and device model for the testcase binary (WP1).
4. First hardening commits with regression tests (WP2).

## Decisions

| Date | Decision | Reason |
|------|----------|--------|
| 2026-11-11 | No pull request against upstream; merge request inside the fork plus an upstream issue | Upstream requires a signed contributor agreement |
| 2026-11-11 | IOMMU is mandatory, no-IOMMU mode is rejected | Without it a device can reach all host memory |
| 2026-11-11 | Hot-plug slot is bound to a device layout known at VM start | VMM cannot create memory or I/O ranges at run time |
| 2026-11-11 | Builds and tests run on the fork's GitHub Actions | No local Linux build environment |
| 2026-11-11 | Planning files and CI definitions live on `meta`, code on its own branch | Keeps the merge request free of unrelated files |

## Test tiers (see PLAN.md section 6)

| Tier | State | Commit | Note |
|------|-------|--------|------|
| T1 unit | not started | | |
| T2 fuzz | not started | | |
| T3 kernel simulation | not started | | |
| T4 system simulation | not started | | |
| T5 hardware | not run | | needs a human with hardware |

## CI runs

None yet.
