# VirtualBox PCIe passthrough: hot-plug and hardening (work in progress)

This fork of [VirtualBox](https://github.com/VirtualBox/virtualbox) carries
work on the VFIO based PCIe passthrough device (`pci-vfio`):

- safe handling of devices that are removed on the host while the VM runs
- PCIe native hot-plug towards the guest
- hardening of the existing device and a test harness that needs no hardware

| Branch | Content |
|--------|---------|
| `meta` | this page, the [plan](PLAN.md), the [status](STATUS.md), CI definitions |
| `main` | unmodified mirror of upstream `main` |
| `pci-passthrough-hotplug` | the code, based on upstream `main` |

The code is licensed like VirtualBox itself (GPL-3.0-only).

Status: early. Nothing here has been tested on real hardware yet. See
[STATUS.md](STATUS.md) for what has and has not been verified.
