#!/bin/bash
# Part of the simulation, see run.sh: what runs inside the virtual machine
# which stands in for the host.  That machine has an emulated IOMMU and an
# emulated NVMe drive behind a PCIe root port.  The drive is bound to vfio-pci
# here and VirtualBox passes it on to its own guest.

set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
: "${SIM_DIR:?}" "${SIM_WS:?}"
OUT=/mnt
STAGES=$OUT/stages.txt

stage() # <what> <ok|FAILED|skipped> [<note>]
{
    local line="simulated host/setup: $1: $2${3:+ ($3)}"
    echo "STAGE $line"
    echo "$line" >> "$STAGES"
}

echo "==== the simulated host: Linux $(uname -r)"
grep -m 1 'model name' /proc/cpuinfo
echo "CPU flags for virtualization: $(grep -m 1 '^flags' /proc/cpuinfo | grep -o -w -E 'svm|vmx' | tr '\n' ' ')"
free -m | sed -n '1,2p'
dmesg | grep -i -E 'DMAR|iommu' | cut -c1-200 | head -n 25

modprobe kvm_amd 2> /dev/null || modprobe kvm_intel 2> /dev/null
if [ -c /dev/kvm ]; then
    stage "KVM is there (nested virtualization)" ok
else
    stage "KVM is there (nested virtualization)" FAILED
    dmesg | grep -i -E 'kvm|svm|vmx' | tail -n 10
fi

modprobe vfio-pci || echo "modprobe vfio-pci failed"
modprobe iommufd 2> /dev/null
if [ -c /dev/iommu ]; then
    stage "/dev/iommu is there" ok
else
    stage "/dev/iommu is there" FAILED
fi

addr=
for d in /sys/bus/pci/devices/*; do
    [ "$(cat "$d/class")" = 0x010802 ] && addr=${d##*/}
done
lspci -nn 2> /dev/null || ls /sys/bus/pci/devices
if [ -z "$addr" ]; then
    stage "the NVMe drive is on the PCI bus" FAILED
    exit 1
fi
echo "the drive is at $addr, its IOMMU group has: $(ls "/sys/bus/pci/devices/$addr/iommu_group/devices" 2>&1 | tr '\n' ' ')"
if [ ! -e "/sys/bus/pci/devices/$addr/iommu_group" ]; then
    stage "the drive is behind the IOMMU" FAILED
    dmesg | tail -n 40
    exit 1
fi
stage "the drive is behind the IOMMU" ok

cur=
[ -e "/sys/bus/pci/devices/$addr/driver" ] && cur=$(basename "$(readlink "/sys/bus/pci/devices/$addr/driver")")
[ -n "$cur" ] && [ "$cur" != vfio-pci ] && echo "$addr" > "/sys/bus/pci/devices/$addr/driver/unbind"
echo vfio-pci > "/sys/bus/pci/devices/$addr/driver_override"
echo "$addr" > /sys/bus/pci/drivers_probe
ls -l "/sys/bus/pci/devices/$addr/vfio-dev/" /dev/vfio/devices/ 2>&1
if ls /sys/bus/pci/devices/"$addr"/vfio-dev/vfio* > /dev/null 2>&1 && ls /dev/vfio/devices/vfio* > /dev/null 2>&1; then
    stage "the drive is bound to vfio-pci and has its VFIO device node" ok
else
    stage "the drive is bound to vfio-pci and has its VFIO device node" FAILED
    dmesg | tail -n 30
    exit 1
fi

if [ ! -c /dev/kvm ]; then
    stage "VirtualBox with the drive passed through" skipped "it takes KVM"
    exit 0
fi

ulimit -l unlimited
mkdir -p "$OUT/host"
VBOX_BIN=$SIM_WS/out/linux.amd64/release/bin \
SIM_WORK=/tmp/sim \
SIM_OUT=$OUT/host \
SIM_STAGES=$STAGES \
GUEST_RAW=$SIM_WS/sim-work/guest.raw \
SIM_WHERE="simulated host" \
SIM_HOSTADDR=$addr \
SIM_BOOT_WAIT=600 \
    bash "$SIM_DIR/vbox-phases.sh" passthrough hotplug hostremove startempty

echo "==== the simulated host: kernel messages about the IOMMU and VFIO"
dmesg | grep -i -E 'DMAR.*fault|vfio|iommufd|AER|pcieport' | cut -c1-220 | tail -n 40
dmesg > "$OUT/host/dmesg.log" 2> /dev/null
exit 0
