# Building, installing, preparing the host

How to get from the branch of the merge request to a VirtualBox which can
pass a PCIe device through. What to try with it is in [HWTEST.md](HWTEST.md).

What of this was done by somebody or something, and what was not:

| Part | State |
|------|-------|
| 2. Building | What the workflow of this fork does in every run, on Ubuntu 24.04 |
| 3. Running from the build directory, without kernel modules of VirtualBox | What the simulation does, see [STATUS.md](STATUS.md) for how far that got |
| 4. Preparing a device: binding to `vfio-pci`, permissions | Binding as the simulation does it with an emulated device; never with a real one |
| 4. Thunderbolt and USB4 | From the documentation of the kernel, **never tried** |
| A build with the graphical user interface, packages, an installer | **Not built** by anything here |

## 1. What the host needs

- A PC with an IOMMU (Intel VT-d or AMD-Vi) which is switched on in the
  firmware. This is the only firmware setting needed. Without an IOMMU the
  device refuses to work, there is no mode without it.
- Linux 6.12 or newer with `CONFIG_IOMMUFD`, `CONFIG_VFIO_PCI`,
  `CONFIG_VFIO_DEVICE_CDEV` and KVM. The kernels of current distributions
  have them. On Intel the kernel may need `intel_iommu=on` on its command
  line.
- Check:

      dmesg | grep -i -e DMAR -e IOMMU      # the IOMMU is found and enabled
      ls /sys/kernel/iommu_groups           # not empty
      ls -l /dev/kvm

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
   its PCI devices show up, depending on the security level of the port:
   `boltctl list`, `boltctl authorize <uuid>`. With the level `none` or
   `dponly` there is nothing to do.
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
