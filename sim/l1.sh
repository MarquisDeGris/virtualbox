#!/bin/bash
# Part of the simulation, see run.sh: what runs inside the virtual machine
# which stands in for the host.  That machine has an emulated IOMMU and an
# emulated NVMe drive behind a PCIe root port.  The drive is bound to vfio-pci
# here and VirtualBox passes it on to its own guest.
#
# From the kernel command line: SIM_DIR (these scripts), SIM_WS (the
# workspace with the build), SIM_WHERE (the name of this machine in the
# results), SIM_PHASES (comma separated, see vbox-phases.sh), SIM_BOOT_WAIT,
# SIM_MD5 and, optionally, SIM_WARP.

set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
: "${SIM_DIR:?}" "${SIM_WS:?}"
WHERE=${SIM_WHERE:-simhost}
PHASES=$(echo "${SIM_PHASES:-plain,passthrough}" | tr ',' ' ')
OUT=/mnt
STAGES=$OUT/stages.txt

stage() # <what> <ok|FAILED|skipped> [<note>]
{
    local line="$WHERE/setup: $1: $2${3:+ ($3)}"
    echo "STAGE $line"
    echo "$line" >> "$STAGES"
}

echo "==== $WHERE: Linux $(uname -r), up for $(cut -d' ' -f1 /proc/uptime) s"
grep -m 1 'model name' /proc/cpuinfo
echo "CPU flags for virtualization: $(grep -m 1 '^flags' /proc/cpuinfo | grep -o -w -E 'svm|vmx|npt|ept' | tr '\n' ' ')"
free -m | sed -n '1,2p'
dmesg | grep -i -E 'DMAR|iommu' | cut -c1-200 | head -n 25

modprobe kvm_amd 2> /dev/null || modprobe kvm_intel 2> /dev/null
if [ -c /dev/kvm ]; then
    stage "KVM is there" ok
else
    stage "KVM is there" FAILED
    dmesg | grep -i -E 'kvm|svm|vmx' | tail -n 10
fi
dmesg | grep -i -E 'kvm' | cut -c1-200 | tail -n 6

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
ls "/sys/bus/pci/devices/$addr/vfio-dev/" /dev/vfio/devices/ 2>&1 | tr '\n' ' '
echo
if ls /sys/bus/pci/devices/"$addr"/vfio-dev/vfio* > /dev/null 2>&1 && ls /dev/vfio/devices/vfio* > /dev/null 2>&1; then
    stage "the drive is bound to vfio-pci and has its VFIO device node" ok
else
    stage "the drive is bound to vfio-pci and has its VFIO device node" FAILED
    dmesg | tail -n 30
    exit 1
fi

echo "==== $WHERE: the checklist of INSTALL.md for $addr"
if sh "$SIM_DIR/../tools/pci-passthrough-check.sh" "$addr"; then
    stage "the checklist script finds nothing missing for the drive" ok
else
    stage "the checklist script finds nothing missing for the drive" FAILED
fi

if [ ! -c /dev/kvm ]; then
    stage "VirtualBox with the drive passed through" skipped "it takes KVM"
    exit 0
fi

ulimit -l unlimited
mkdir -p "$OUT/$WHERE"
VBOX_BIN=$SIM_WS/out/linux.amd64/release/bin \
SIM_WORK=/tmp/sim \
SIM_OUT=$OUT/$WHERE \
SIM_STAGES=$STAGES \
GUEST_RAW=$SIM_WS/sim-work/guest-quiet.raw \
SIM_WHERE=$WHERE \
SIM_HOSTADDR=$addr \
SIM_BOOT_WAIT=${SIM_BOOT_WAIT:-300} \
    bash "$SIM_DIR/vbox-phases.sh" $PHASES

echo "==== $WHERE: up for $(cut -d' ' -f1 /proc/uptime) s; kernel messages about the IOMMU and VFIO"
dmesg | grep -i -E 'DMAR.*fault|vfio|iommufd|AER|pcieport' | cut -c1-220 | tail -n 40
dmesg > "$OUT/$WHERE/dmesg.log" 2> /dev/null
exit 0
