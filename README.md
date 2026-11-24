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
| `pci-passthrough-hotplug-7.2`, `pci-passthrough-hotplug-7.1` | the same work on the maintenance branches of 7.2 and 7.1. They build and their tests pass; no VM with a device runs on them, the released lines have no execution engine the device takes |
| `pci-passthrough-hotplug-7.2-kvm`, `pci-passthrough-hotplug-7.1-kvm` | those two with the KVM backend which Cyberus Technology publishes for VirtualBox, on which the device runs on the released lines. [Pull request 2](https://github.com/MarquisDeGris/virtualbox/pull/2) is the one for 7.2, [pull request 4](https://github.com/MarquisDeGris/virtualbox/pull/4) the one for 7.1 |
| `backports-7.1` | nothing of the passthrough work: changes which the 7.2 line received and the 7.1 line, which upstream no longer supports, did not. [Pull request 3](https://github.com/MarquisDeGris/virtualbox/pull/3) |
| `feat/modifyvm-graphics-order`, `feat/doc-3d-object-memory`, `feat/nvram-firmware-setup` | nothing of the passthrough work: three things users asked for in the tracker of upstream, each a branch from `main`. `VBoxManage modifyvm` takes `--accelerate-3d` and `--vram` in any order with `--graphicscontroller` ([pull request 5](https://github.com/MarquisDeGris/virtualbox/pull/5)), and the manual says what the memory for 3D objects is and how to change its limit ([pull request 6](https://github.com/MarquisDeGris/virtualbox/pull/6)), and `VBoxManage modifynvram <vm> firmwaresetup` has a VM enter the setup of its UEFI firmware at the next start ([pull request 8](https://github.com/MarquisDeGris/virtualbox/pull/8)) |
| `VBox-7.2`, `VBox-7.1` | unmodified copies of upstream's maintenance branches, the bases of the branches above |
| `pci-passthrough-hotplug` | an older state of the code, no longer moved |

Which branch to take is in [INSTALL.md](INSTALL.md), part 0. The code is
licensed like VirtualBox itself (GPL-3.0-only); the KVM backend of Cyberus
Technology is under the same licence.

Status: ready for a test on hardware. Nothing here has been tested on real
hardware yet. See [STATUS.md](STATUS.md) for what has and has not been
verified.
