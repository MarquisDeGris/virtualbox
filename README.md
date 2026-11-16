# VirtualBox PCIe passthrough: hot-plug and hardening

This fork of [VirtualBox](https://github.com/VirtualBox/virtualbox) carries
work on the VFIO based PCIe passthrough device (`pci-vfio`):

- safe handling of devices that are removed on the host while the VM runs
- PCIe native hot-plug towards the guest, with `VBoxManage` commands for
  putting a device into a running VM and taking it out
- hardening of the existing device and a test harness that needs no hardware

| Branch | Content |
|--------|---------|
| `meta` | this page, the [plan](PLAN.md), the [status](STATUS.md), [building and installing](INSTALL.md), the [hardware test](HWTEST.md), CI definitions |
| `main` | unmodified mirror of upstream `main` |
| `pci-passthrough-hotplug-v1` | the code, based on upstream `main`: the branch of [pull request 1](https://github.com/MarquisDeGris/virtualbox/pull/1) |
| `pci-passthrough-hotplug` | an older state of the code, no longer moved |

The code is licensed like VirtualBox itself (GPL-3.0-only).

Status: ready for a test on hardware. Nothing here has been tested on real
hardware yet. See [STATUS.md](STATUS.md) for what has and has not been
verified.
