# Building, installing, preparing the host

How to get from the branch of the merge request to a VirtualBox which can
pass a PCIe device through. What to try with it is in [HWTEST.md](HWTEST.md).

What of this was done by somebody or something, and what was not:

| Part | State |
|------|-------|
| 1. The checklist and its script | Written from the source and the documentation of the kernel; what the simulation has of it is said at its end. The script ran in the simulation |
| 2. Building | What the workflow of this fork does in every run, on Ubuntu 24.04 |
| 3. Running from the build directory, without kernel modules of VirtualBox | Done by the simulation in every run, as root, see [STATUS.md](STATUS.md) |
| 4. Preparing a device: binding to `vfio-pci`, permissions | Binding as the simulation does it with an emulated device, as root; never with a real one, never as another user |
| 6. Taking a device out and putting it in while the VM runs | Done by the simulation with an emulated device, see [STATUS.md](STATUS.md) |
| 4. Thunderbolt and USB4 | From the documentation of the kernel, **never tried** |
| A build with the graphical user interface, packages, an installer | **Not built** by anything here |

## 0. Which branch

| Branch | What it is on | What it is good for |
|--------|---------------|---------------------|
| `pci-passthrough-hotplug-v1` | the development line of VirtualBox (`main`) | **The one to build and try.** Everything in this file and in HWTEST.md is about it |
| `pci-passthrough-hotplug-7.2` | the maintenance branch of 7.2 (7.2.22) | The same work on that base. It builds and its tests pass. **No VM with a passed through device runs on it**, see below |
| `pci-passthrough-hotplug-7.1` | the maintenance branch of 7.1 (7.1.18) | As for 7.2: it builds and its tests pass, no VM with a device runs on it |
| `pci-passthrough-hotplug-7.2-kvm` | the branch for 7.2 with the KVM backend Cyberus Technology publishes for VirtualBox | **The one to take on a released line.** 7.2 with an engine the device runs on, on the newest state of upstream's branch. Every check of the simulation passes on it, with a Linux guest and with Windows taking the drive in the hot-plug capable slot, letting go of it and taking it again (run 253). Not what Oracle ships: see below |
| `pci-passthrough-hotplug-7.1-kvm` | the branch for 7.1 with that backend in its last version for 7.1 | The same for 7.1, with what the 7.2 line received after upstream ended its support for 7.1. Every check of the simulation passes on it (run 254); in an earlier run Windows stopped after letting go of the drive, which is not understood. See [STATUS.md](STATUS.md) for how far it has been built and run |
| `backports-7.1` | the maintenance branch of 7.1, without the passthrough work | For who stays on 7.1 for other reasons: changes which the 7.2 line received and 7.1 did not. Built like upstream's 7.1; see [STATUS.md](STATUS.md), "The 7.1 line and what the 7.2 line received after it". The branches for 7.1 above have the same changes |

Why the two branches for the released lines do not run a VM with a
device: the work takes KVM as execution engine, so that the memory of the
guest stays where it is while the device can reach it, and the released
lines have no KVM for Linux on x86. It is not built there, and switched on
it does not compile; upstream made it work on the development line in
2026 and has not released that yet. A VM on 7.1 or 7.2 runs on the kernel
driver of VirtualBox, and the passthrough device as this work leaves it
refuses that engine. STATUS.md, "The maintenance branches", has the
details and what could be done about it.

The two `-kvm` branches are one of the things which can be done about
it. They add the KVM backend which Cyberus Technology publishes for
VirtualBox
([virtualbox-kvm](https://github.com/cyberus-technology/virtualbox-kvm),
GPL version 3) to the released lines, as one commit, and one small
change to it after that. A VirtualBox built from them runs its VMs on
KVM without the kernel modules of VirtualBox, as the development line
does. They are built like this, with the packages of part 2:

    git clone -b pci-passthrough-hotplug-7.2-kvm \
        https://github.com/MarquisDeGris/virtualbox.git
    cd virtualbox
    # The 7.1 branch names its kBuild without saying where it is from:
    # there, clone https://github.com/VirtualBox/kbuild.git into kBuild
    # and check out the commit "git ls-tree HEAD kBuild" tells.
    git submodule update --init --recursive
    export VBOX_SVN_REV=...                     # as in part 2
    export KBUILD_PATH="$PWD/kBuild/kBuild"
    # 7.1 does not know --disable-dxvk: leave it out there.
    ./configure --ose --build-headless --disable-docs --disable-python \
        --disable-hardening --disable-extpack --disable-sdl --disable-java \
        --disable-kmods --disable-dxvk --with-kvm --with-kbuild="$KBUILD_PATH"
    # What this build leaves out; the translations need tools of Qt.
    printf '%s :=\n' VBOX_WITH_DOCS VBOX_WITH_ADDITIONS VBOX_WITH_VALIDATIONKIT \
        VBOX_WITH_WEBSERVICES VBOX_WITH_UPDATE_AGENT VBOX_WITH_TESTCASES \
        'override VBOX_WITH_NLS' VBOX_WITH_MAIN_NLS VBOX_WITH_PUEL_NLS \
        VBOX_WITH_VBOXMANAGE_NLS VBOX_WITH_VIDEOHWACCEL > LocalConfig.kmk
    . ./env.sh
    kmk -j"$(nproc)" KBUILD_TYPE=release

This is what the workflow does for these branches; nobody has done it by
hand. Three things differ on them from what is said for the development
line: a Windows guest is left at its default paravirtualization
interface ([HWTEST.md](HWTEST.md) says `--paravirt-provider none`, which
is for the backend of the development line; STATUS.md says what has run
which way); the backend has a change of this work about saved states of
guests with the Hyper-V interface, without which a Windows guest with
two CPUs did not go on after its state was loaded ([HWTEST.md](HWTEST.md)
asks to try that); and the readme of the backend calls AMD processors
experimental.

The branches for 7.1 have one change to Main which is not about the
device: saving the state of a VM tells its caller when it did not work
out, as on the lines after 7.1. Without it a refused save, which is what
a VM with a device of the host answers, left the VM in a state it could
not be got out of.

About the 7.1 line itself: upstream ended its support with 7.1.18 in
April 2026 and has published its security updates for 7.2 only since
then: four of them from July to October 2026, with 62 CVE numbers which
each name a version of 7.2 and say nothing about 7.1. The branches for
7.1 here are on the head of upstream's 7.1 branch,
which went on for five months after that release, and
[STATUS.md](STATUS.md), "The 7.1 line and what the 7.2 line received
after it", says what was taken over from 7.2 beyond that and what was
not. Who can choose takes a branch for 7.2. The last name in the list for
`LocalConfig.kmk` above is for 7.1: it leaves the 2D video acceleration
for guests out of the build, which the lines after 7.1 no longer have at
all and which a guest with a current Windows does not use.

## 1. Checklist: what has to be switched on

Everything in this list has to be there. On a PC with a current
distribution most of it is; a host which is locked down may have some of
it switched off. The list says for each item how to see whether it is
there, what happens when it is not, and what can be done then.

A script goes through the list on a host and tells what is missing. It
only reads. Run it as the user who is to run the VM, with the PCI
addresses of the functions of the device if the device is plugged in:

    sh tools/pci-passthrough-check.sh 0000:0a:00.0 0000:0a:00.1

([tools/pci-passthrough-check.sh](tools/pci-passthrough-check.sh) on this
branch.)

There is no reduced mode: where an item marked **stop** cannot be had, the
device cannot be passed through on that host.

### Firmware

Nothing in the running system can change these.

| | What | How to see | When it is not there | What can be done |
|---|------|-----------|----------------------|------------------|
| F1 | The IOMMU (Intel VT-d, AMD-Vi) is on | `ls /sys/kernel/iommu_groups` is not empty | The device is in no IOMMU group and cannot be bound for passing through | **stop.** It has to be switched on in the firmware. Without it a device could reach all memory of the host, so there is no mode without it |
| F2 | Interrupt remapping, a part of the IOMMU | `/proc/interrupts` has names starting with `IR-`; the kernel log says `DMAR-IR: Enabled IRQ remapping` or `AMD-Vi: Interrupt remapping enabled` | The kernel does not hand the device out | **stop.** See "What must not be used" below |
| F3 | Hardware virtualization (VT-x, AMD-V) is on | `/dev/kvm` is there once the KVM module is loaded | No VM with KVM | **stop** |
| F4 | For a device on Thunderbolt or USB4: PCIe tunnelling is allowed | `cat /sys/bus/thunderbolt/devices/domain0/security` says `none`, `user` or `secure` | With `dponly`, `usbonly` or `nopcie` the PCIe devices of an enclosure never appear on the host | **stop** for such devices. Devices in a slot of the PC do not need it |

### Kernel

Whoever builds the system for the host decides these.

| | What | How to see | When it is not there | What can be done |
|---|------|-----------|----------------------|------------------|
| K1 | Linux 6.12 or newer | `uname -r` | The device was written for the interface of that kernel | **stop** |
| K2 | KVM (`CONFIG_KVM`, module `kvm_intel` or `kvm_amd`) | `ls -l /dev/kvm` | The VM does not start. If the VM uses another execution engine: "PCI passthrough requires the native execution engine (KVM), which this VM doesn't use" | **stop.** A host which runs its VMs with the kernel driver of VirtualBox (`vboxdrv`) only cannot pass a device through: the device needs the memory of the guest to stay where it is, which only the KVM engine guarantees. Changing that is work in the VMM which has not been done |
| K3 | VFIO for PCI (`CONFIG_VFIO_PCI`, module `vfio-pci`) | `ls /sys/bus/pci/drivers/vfio-pci` after `modprobe vfio-pci` | No device can be bound for passing through | **stop** |
| K4 | IOMMUFD and VFIO device nodes (`CONFIG_IOMMUFD`, `CONFIG_VFIO_DEVICE_CDEV`) | `/dev/iommu` is there; `/dev/vfio/devices/vfio0` appears when a device is bound to `vfio-pci` | The device falls back by itself to the older interface, `/dev/vfio/vfio` with a node for the IOMMU group (`CONFIG_VFIO_CONTAINER`, `CONFIG_VFIO_GROUP`) | Works with a limit: the VM cannot start without its device. The older interface was only ever tested against a made-up kernel interface |
| K5 | The IOMMU is switched on in the kernel | as F1; the kernel log says `DMAR: IOMMU enabled` | as F1 | On Intel, `intel_iommu=on` on the kernel command line unless the kernel was built with it as its default (`CONFIG_INTEL_IOMMU_DEFAULT_ON`). Where the command line cannot be changed, it takes a kernel built that way |
| K6 | The modules can be loaded, or are loaded | `cat /proc/sys/kernel/modules_disabled` is 0 | With 1 nothing more can be loaded | Load `kvm_intel` or `kvm_amd`, `vfio-pci`, `iommufd` and, for Thunderbolt, `thunderbolt` before the host locks module loading, or build them into the kernel |
| K7 | For devices which get plugged in: PCIe hot-plug and Thunderbolt on the host (`CONFIG_HOTPLUG_PCI_PCIE`, `CONFIG_USB4`) | plug the device in, it shows in `/sys/bus/pci/devices` | The host does not see the device arrive | **stop** for such devices |

### The device on the host

Whoever administers the host does these, each time the device was plugged
in or the host started. See section 4 for how.

| | What | How to see | When it is not so | What can be done |
|---|------|-----------|-------------------|------------------|
| D1 | A device on Thunderbolt or USB4 is authorized, if the security level is `user` or `secure` | its PCI functions are in `/sys/bus/pci/devices` | The PCI devices do not appear | `boltctl authorize`, or a rule of the host which does it |
| D2 | All functions of the device, and nothing else, are in one IOMMU group | `ls /sys/bus/pci/devices/<address>/iommu_group/devices` | What else is in the group belongs to the same VM as far as the IOMMU can tell | Another slot or port. See "What must not be used" below |
| D3 | Each function is bound to `vfio-pci`, and the driver of the host for the device (a graphics driver, say) does not have it | `readlink /sys/bus/pci/devices/<address>/driver` | The VM does not start: "The PCI device 0000:0a:00.0 is not bound to a VFIO driver". With hot-plug switched on it starts with the slot empty instead, if it was started with the device once before | Binding takes root and writing to sysfs (`driver_override`, `unbind`, `drivers_probe`). Where nobody may do that by hand, a service of the host has to, when the device appears |

### The user who runs the VM

| | What | How to see | When it is not so | What can be done |
|---|------|-----------|-------------------|------------------|
| U1 | May read and write `/dev/kvm` | `ls -l /dev/kvm` | The VM does not start | Group or ownership of the node |
| U2 | May read and write `/dev/iommu` and the node of each function, `/dev/vfio/devices/vfio<n>` (older interface: `/dev/vfio/vfio` and `/dev/vfio/<group>`) | `ls -l` | The VM does not start, the message names the path which could not be opened | Ownership of the nodes, set when the device is bound. The node of a function is new each time the device is bound |
| U3 | May lock as much memory as the VM has RAM | `ulimit -l`, in KiB | "Failed to make the guest memory available to the passed through PCI device. The amount of memory the user running the VM may lock might be too low" | The limit of the user (`RLIMIT_MEMLOCK`), for example in `/etc/security/limits.conf` or in the unit which starts the VM |
| U4 | May read `/sys/bus/pci/devices/<address>/` and what is below it | `ls` | The node of the function is not found | Where the VM process is confined (a policy of SELinux or AppArmor, a namespace with its own `/sys` or `/dev`), these paths and the nodes of U1 and U2 have to be allowed for it |

The kernel driver of VirtualBox (`vboxdrv`) is not needed and does not have
to be loaded.

### The VM

| | What | How |
|---|------|-----|
| V1 | Chipset ICH9, firmware EFI | `VBoxManage modifyvm <vm> --chipset ich9 --firmware efi` |
| V2 | KVM as execution engine | `VBoxManage modifyvm <vm> --vm-execution-engine native-api` |
| V3 | The device attached, for hot-plug at device 0 of guest bus 1 or 2 and alone there | section 2 of [HWTEST.md](HWTEST.md) |
| V4 | For a graphics card which is to show the start of the guest: the emulated graphics controller off, `ExposeVga` set | the top of `src/VBox/Devices/Bus/DevPciVfio.cpp` |

### What must not be used to get around a missing item

Each of these makes the missing item go away and takes the protection
with it which the item is there for:

- The mode of VFIO without an IOMMU (`enable_unsafe_noiommu_mode`). The
  device refuses a function which is in that mode.
- `allow_unsafe_interrupts`, for a host without interrupt remapping.
- Patches and kernel parameters which split IOMMU groups the hardware
  does not separate (`pcie_acs_override`).

### What of this list was seen working

The simulation (see [STATUS.md](STATUS.md)) runs with the generic kernel
7.0 of Ubuntu 24.04 in a virtual machine with an emulated Intel IOMMU:
F1, F2, K1 to K5, D2, D3 and U1 to U4 for root are what it has, and with
them everything it tries works. The script ran there (run 223): it finds
nothing missing for the drive in that machine, and on the runner outside
of it, which has no IOMMU, it says so. The messages quoted above are from
the source; of them the one of D3 was seen there. F4, K7 and D1 are from
the documentation of the kernel and were never tried, as nothing here has
Thunderbolt. A host on which an item is switched off was not tried
either, apart from the IOMMU.

## 2. Building

On Ubuntu 24.04; other distributions need the same things under their names.

    sudo apt-get install --no-install-recommends \
        build-essential gcc-multilib g++-multilib libc6-dev-i386 pkg-config \
        acpica-tools chrpath nasm yasm xsltproc libxml2-utils genisoimage makeself \
        glslang-tools python3 python3-dev \
        libasound2-dev libcap-dev libcurl4-openssl-dev libdevmapper-dev \
        libpam0g-dev libpulse-dev libssl-dev zlib1g-dev libzstd-dev \
        libxml2-dev libxslt1-dev libvpx-dev libopus-dev libogg-dev libvorbis-dev \
        liblzf-dev liblzma-dev libtpms-dev libpng-dev libjpeg-dev \
        libslirp-dev libssh-dev libvncserver-dev \
        libx11-dev libxext-dev libxmu-dev libxrandr-dev libxinerama-dev \
        libxcursor-dev libxcomposite-dev libxfixes-dev libxt-dev \
        libgl-dev libglu1-mesa-dev mesa-common-dev libvulkan-dev

    git clone --recurse-submodules -b pci-passthrough-hotplug-v1 \
        https://github.com/MarquisDeGris/virtualbox.git
    cd virtualbox

    # The build takes the revision of the product from a line in the commit
    # message which the commits of this fork do not have; take it from the
    # newest upstream commit.
    export VBOX_SVN_REV=$(git log -50 --format=%B \
        | grep -m1 -o -E '^svn:sync-xref-src-repo-rev: r[0-9]+$' | grep -o -E '[0-9]+$')

    export KBUILD_PATH="$PWD/kBuild/kBuild"
    ./configure --ose --build-headless --disable-docs --disable-additions \
        --disable-validationkit --disable-python --disable-hardening \
        --disable-extpack --disable-sdl
    . ./env.sh
    kmk -j"$(nproc)" KBUILD_TYPE=release

In the workflow this takes 11 to 16 minutes on four cores. The result is in
`out/linux.amd64/release/bin`.

What this build is, and is not:

- Headless: `VBoxManage` and `VBoxHeadless`, no graphical user interface.
  For a graphics card that is enough, its picture is on the monitor
  connected to the card. Leaving out `--build-headless` builds the user
  interface too and needs Qt; nothing here has built that.
- Without hardening (`--disable-hardening`), so it runs from the build
  directory. A build with hardening has to be installed with its installer
  to run, which is how the product is shipped; nothing here has built that
  either.

## 3. Running it

Nothing has to be installed, and no kernel module of VirtualBox has to be
loaded: with KVM as execution engine VirtualBox runs without `vboxdrv`. The
log of a VM says `SUP: In driverless mode.` then. Without the kernel modules
of VirtualBox there is no bridged and no host-only networking; NAT works.

    cd out/linux.amd64/release/bin
    ./VBoxManage --version
    ./VBoxManage createvm --name test --ostype Windows11_64 --register
    ./VBoxManage modifyvm test --chipset ich9 --firmware efi --ioapic on \
        --vm-execution-engine native-api
    ...
    ./VBoxHeadless --startvm test

The user who runs the VM needs:

- read and write access to `/dev/kvm`, to `/dev/iommu` and to the VFIO
  device nodes of the functions of the device, see below,
- to be allowed to lock all of the memory of the guest:
  `ulimit -l unlimited`, or a limit of at least the RAM of the VM, for
  example through `/etc/security/limits.conf`.

Running the VM as root needs none of that and is the quickest way to a
first result; it is not how it should stay.

## 4. Preparing the device

1. A device on Thunderbolt or USB4 has to be authorized on the host before
   its PCI devices show up, if the security level of the port is `user` or
   `secure`: `boltctl list`, `boltctl authorize <uuid>`. With the level
   `none` there is nothing to do. With `dponly`, `usbonly` or `nopcie`
   there are no PCI devices to be had from the port at all, see F4 of the
   checklist.
2. Find the device and all its functions:

       lspci -D -nn

   For example a graphics card at `0000:0a:00.0` with its audio function at
   `0000:0a:00.1`.
3. All functions of the device, and nothing else, have to be in one IOMMU
   group:

       ls /sys/bus/pci/devices/0000:0a:00.0/iommu_group/devices

4. Bind each function to `vfio-pci`, as root:

       modprobe vfio-pci
       for f in 0000:0a:00.0 0000:0a:00.1; do
           [ -e /sys/bus/pci/devices/$f/driver ] && echo $f > /sys/bus/pci/devices/$f/driver/unbind
           echo vfio-pci > /sys/bus/pci/devices/$f/driver_override
           echo $f > /sys/bus/pci/drivers_probe
       done
       ls /sys/bus/pci/devices/0000:0a:00.0/vfio-dev    # vfio0, say
       ls -l /dev/iommu /dev/vfio/devices

   A graphics card whose driver was loaded once may not survive the driver
   being unbound; then keep the driver of the host away from it from the
   start, see the description at the top of
   `src/VBox/Devices/Bus/DevPciVfio.cpp`.
5. Give the user who runs the VM access:

       chown <user> /dev/iommu /dev/vfio/devices/vfio0 /dev/vfio/devices/vfio1

   The binding and the ownership are gone after the device was unplugged or
   the host rebooted. A udev rule can do both when the device appears;
   nothing here ships one.

## 5. The VM

See part 2 of [HWTEST.md](HWTEST.md): attaching the device with
`VBoxManage modifyvm --pci-attach`, switching hot-plug on.

## 6. When a device is plugged in while the VM runs

Nothing here does this on its own. In this order, by hand or by a script
which reacts to the device appearing:

1. Authorize the device if the port asks for it (4.1).
2. Bind its functions to `vfio-pci` and give the user access (4.4, 4.5).
3. Put it into the VM:

       VBoxManage controlvm <vm> plugpci 0000:0a:00.0

Before a device is unplugged, take it out of the VM, the guest is asked to
let go of it:

    VBoxManage controlvm <vm> unplugpci 0000:0a:00.0

A device which is pulled without that is taken away from the guest when the
host notices; the VM keeps running.
