#!/bin/sh
# Goes through what a Linux host needs for passing a PCIe device through to a
# VirtualBox VM with the pci-vfio device, and tells what is there and what is
# missing.  See INSTALL.md, section 1, for what each item is about and what
# can be done when it is missing.
#
#   pci-passthrough-check.sh [<PCI address of a function of the device>...]
#
# for example
#
#   pci-passthrough-check.sh 0000:0a:00.0 0000:0a:00.1
#
# Without an address only the host is looked at.  Run it as the user who is
# to run the VM to have the access rights checked for that user.  It only
# reads, it changes nothing and loads nothing.  It needs a shell and cat, ls,
# grep, readlink, uname, id, wc; lspci and the like are not needed.
#
# Lines:  ok       the item is there
#         MISSING  the item is not there, the device cannot be passed through
#         check    could not be found out from here, look yourself
#         note     for information
# The exit code is 1 if something is MISSING, else 0.

missing=0
ok()      { echo "ok       $*"; }
bad()     { echo "MISSING  $*"; missing=$((missing + 1)); }
unsure()  { echo "check    $*"; }
note()    { echo "note     $*"; }

rw() { [ -r "$1" ] && [ -w "$1" ]; }

# Who runs this.  The name of the user is not printed: what this prints is meant
# to be passed on.
me="this user"
[ "$(id -u)" = 0 ] && me=root

echo "---- the kernel"
rel=$(uname -r)
maj=${rel%%.*}
rest=${rel#*.}
min=${rest%%[!0-9]*}
if [ "$maj" -gt 6 ] 2> /dev/null || { [ "$maj" = 6 ] && [ "${min:-0}" -ge 12 ]; }; then
    ok "Linux $rel (6.12 or newer)"
else
    bad "Linux $rel: 6.12 or newer is needed"
fi

if [ -e /proc/sys/kernel/modules_disabled ] && [ "$(cat /proc/sys/kernel/modules_disabled)" = 1 ]; then
    note "no kernel module can be loaded any more on this host: what is not loaded now is not to be had"
fi
if [ -r /sys/kernel/security/lockdown ]; then
    note "kernel lockdown: $(cat /sys/kernel/security/lockdown)"
fi

echo "---- the IOMMU"
ngroups=0
[ -d /sys/kernel/iommu_groups ] && ngroups=$(ls /sys/kernel/iommu_groups | wc -l)
if [ "$ngroups" -gt 0 ]; then
    ok "the IOMMU is in use ($ngroups groups; $(ls /sys/class/iommu 2> /dev/null | tr '\n' ' '))"
else
    bad "no IOMMU in use: it is off in the firmware (VT-d, AMD-Vi) or in the kernel (intel_iommu=on on the kernel command line)"
fi
if [ -r /proc/interrupts ]; then
    if grep -q 'IR-' /proc/interrupts; then
        ok "interrupt remapping is in use"
    elif [ "$ngroups" -gt 0 ]; then
        unsure "interrupt remapping: /proc/interrupts shows no remapped interrupt; the kernel log has a line about it (DMAR-IR or AMD-Vi). Without it the kernel does not hand out devices"
    fi
fi

echo "---- KVM"
if [ -c /dev/kvm ]; then
    if rw /dev/kvm; then
        ok "/dev/kvm is there and $me may use it"
    else
        bad "/dev/kvm is there but $me may not read and write it"
    fi
else
    bad "/dev/kvm is not there: KVM is not in the kernel, its module (kvm_intel, kvm_amd) is not loaded, or virtualization is off in the firmware. The device needs KVM as execution engine of the VM"
fi
if [ -c /dev/vboxdrv ]; then
    note "/dev/vboxdrv is there: the kernel driver of VirtualBox is loaded. It is not needed for this, and a VM with the device has to use KVM (--vm-execution-engine native-api)"
fi

echo "---- VFIO"
if [ -d /sys/bus/pci/drivers/vfio-pci ]; then
    ok "the vfio-pci driver is there"
elif ls /lib/modules/"$rel"/kernel/drivers/vfio/pci/vfio-pci.ko* > /dev/null 2>&1; then
    unsure "the vfio-pci driver is not loaded, the kernel has it as a module: modprobe vfio-pci (as root)"
else
    bad "the vfio-pci driver is not there (CONFIG_VFIO_PCI)"
fi
if [ -c /dev/iommu ]; then
    if rw /dev/iommu; then
        ok "/dev/iommu is there and $me may use it"
    else
        bad "/dev/iommu is there but $me may not read and write it"
    fi
    if [ -d /dev/vfio/devices ] || [ ! -d /sys/bus/pci/drivers/vfio-pci ]; then
        :
    else
        note "/dev/vfio/devices is not there: it appears with the first device bound to vfio-pci, if the kernel has CONFIG_VFIO_DEVICE_CDEV"
    fi
elif [ -c /dev/vfio/vfio ]; then
    note "/dev/iommu is not there (CONFIG_IOMMUFD, module iommufd), /dev/vfio/vfio is: the device uses the older interface then. A VM cannot start without its device that way"
    if rw /dev/vfio/vfio; then
        ok "/dev/vfio/vfio is there and $me may use it"
    else
        bad "/dev/vfio/vfio is there but $me may not read and write it"
    fi
else
    unsure "neither /dev/iommu nor /dev/vfio/vfio is there: the modules iommufd and vfio are not loaded, or the kernel has neither CONFIG_IOMMUFD nor CONFIG_VFIO_CONTAINER"
fi

echo "---- memory the user may lock"
lim=$(ulimit -l 2> /dev/null)
[ "$(id -u)" = 0 ] && lim=unlimited     # the limit does not hold root back
case "$lim" in
    unlimited) ok "$me may lock any amount of memory" ;;
    '')        unsure "could not find out how much memory $me may lock (ulimit -l)" ;;
    *)         note "$me may lock $((lim / 1024)) MiB (ulimit -l): this has to be at least the RAM of the VM" ;;
esac

echo "---- Thunderbolt and USB4"
found=0
for d in /sys/bus/thunderbolt/devices/domain*; do
    [ -r "$d/security" ] || continue
    found=1
    sec=$(cat "$d/security")
    case "$sec" in
        none)        ok "${d##*/}: security level none, PCIe devices behind it come up on their own" ;;
        user|secure) ok "${d##*/}: security level $sec, PCIe devices behind it come up once the device is authorized (boltctl, or the authorized attribute in sysfs)" ;;
        *)           bad "${d##*/}: security level $sec, no PCIe tunnelling: PCIe devices in an enclosure never appear on this host. This is set in the firmware" ;;
    esac
    [ -r "$d/iommu_dma_protection" ] && note "${d##*/}: iommu_dma_protection=$(cat "$d/iommu_dma_protection")"
done
[ "$found" = 0 ] && note "no Thunderbolt or USB4 controller with the thunderbolt driver on it (not needed for devices in a slot of the PC)"

for addr in "$@"; do
    echo "---- the function $addr"
    dev=/sys/bus/pci/devices/$addr
    if [ ! -e "$dev" ]; then
        bad "$addr is not on the PCI bus of the host (not plugged in, not authorized, or not this address: 0000:0a:00.0 is the form)"
        continue
    fi
    ok "$addr is there: $(cat "$dev/vendor" 2> /dev/null):$(cat "$dev/device" 2> /dev/null), class $(cat "$dev/class" 2> /dev/null)"
    if [ -e "$dev/iommu_group" ]; then
        slot=${addr%.*}
        others=
        ports=
        path=$(readlink -f "$dev")
        for m in "$dev"/iommu_group/devices/*; do
            m=${m##*/}
            [ "${m%.*}" = "$slot" ] && continue
            # A port the device is behind (a bridge on the way from the
            # root to it) is no other device to be handed over: the kernel
            # keeps its own driver on it.  It is in the group where it has
            # no access control services (ACS).
            case "$path" in
                */"$m"/*)
                    case "$(cat "/sys/bus/pci/devices/$m/class" 2> /dev/null)" in
                        0x0604*) ports="$ports $m"; continue ;;
                    esac
                    ;;
            esac
            others="$others $m"
        done
        if [ -n "$others" ]; then
            bad "its IOMMU group has other devices too:$others. They would be handed over with it; the host cannot separate them"
        elif [ -n "$ports" ]; then
            ok "its IOMMU group has the functions of this device and nothing else to hand over: $(ls "$dev/iommu_group/devices" | tr '\n' ' ')"
            note "a port above the device is in that group too:$ports. It stays with the host, which permits this; the port does not tell how it keeps what is behind it apart (no ACS)"
        else
            ok "its IOMMU group has the functions of this device only: $(ls "$dev/iommu_group/devices" | tr '\n' ' ')"
        fi
    else
        bad "it is in no IOMMU group"
    fi
    drv=
    [ -e "$dev/driver" ] && drv=$(basename "$(readlink "$dev/driver")")
    case "$drv" in
        vfio-pci)
            ok "it is bound to vfio-pci"
            node=
            for n in "$dev"/vfio-dev/vfio*; do
                [ -e "$n" ] && node=/dev/vfio/devices/${n##*/}
            done
            if [ -z "$node" ]; then
                note "it has no VFIO device node (CONFIG_VFIO_DEVICE_CDEV): the older interface is used, through /dev/vfio/$(basename "$(readlink "$dev/iommu_group")")"
            elif rw "$node"; then
                ok "$node is there and $me may use it"
            else
                bad "$node is there but $me may not read and write it"
            fi
            ;;
        '') bad "it is bound to no driver: it has to be bound to vfio-pci (INSTALL.md, section 4)" ;;
        *)  bad "it is bound to the driver $drv of the host: it has to be bound to vfio-pci instead (INSTALL.md, section 4)" ;;
    esac
    [ -r "$dev/reset_method" ] && note "ways the host can reset it: $(cat "$dev/reset_method" 2> /dev/null)"
done

echo "----"
if [ "$missing" = 0 ]; then
    echo "nothing is missing as far as this can tell"
    exit 0
fi
echo "$missing missing"
exit 1
