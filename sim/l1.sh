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
#
# The machine can have a device with two functions instead, each an NVMe
# controller with a drive of its own (SIM_MD5B is there then): what a graphics
# card with its audio function is to the host, two functions which the IOMMU
# does not keep apart and which go to a VM together.

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
echo "CPU flags for the time stamp counter: $(grep -m 1 '^flags' /proc/cpuinfo | grep -o -w -E 'constant_tsc|nonstop_tsc|tsc_reliable' | tr '\n' ' ')"
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

# The device: the first function which is an NVMe controller, and with it
# every function at its address.
addr=
for d in /sys/bus/pci/devices/*; do
    [ -z "$addr" ] && [ "$(cat "$d/class")" = 0x010802 ] && addr=${d##*/}
done
funcs=
if [ -n "$addr" ]; then
    for d in /sys/bus/pci/devices/"${addr%.*}".*; do
        funcs="$funcs${funcs:+ }${d##*/}"
    done
fi
lspci -nn 2> /dev/null || ls /sys/bus/pci/devices
if [ -z "$addr" ]; then
    stage "the NVMe drive is on the PCI bus" FAILED
    exit 1
fi
echo "the drive is at $addr, the functions of the device are: $funcs"
echo "its IOMMU group has: $(ls "/sys/bus/pci/devices/$addr/iommu_group/devices" 2>&1 | tr '\n' ' ')"
if [ ! -e "/sys/bus/pci/devices/$addr/iommu_group" ]; then
    stage "the drive is behind the IOMMU" FAILED
    dmesg | tail -n 40
    exit 1
fi
stage "the drive is behind the IOMMU" ok
if [ "$funcs" != "$addr" ]; then
    # What makes the functions one device for the host: the IOMMU does not
    # keep them apart, so they are in one group.
    if [ "$(ls "/sys/bus/pci/devices/$addr/iommu_group/devices" | tr '\n' ' ')" = "$funcs " ]; then
        stage "the functions of the device are in one IOMMU group, with nothing else" ok
    else
        stage "the functions of the device are in one IOMMU group, with nothing else" skipped "this host has them apart"
    fi
fi

bound=yes
for f in $funcs; do
    cur=
    [ -e "/sys/bus/pci/devices/$f/driver" ] && cur=$(basename "$(readlink "/sys/bus/pci/devices/$f/driver")")
    [ -n "$cur" ] && [ "$cur" != vfio-pci ] && echo "$f" > "/sys/bus/pci/devices/$f/driver/unbind"
    echo vfio-pci > "/sys/bus/pci/devices/$f/driver_override"
    echo "$f" > /sys/bus/pci/drivers_probe
    ls "/sys/bus/pci/devices/$f/vfio-dev/" 2>&1 | tr '\n' ' '
    ls /sys/bus/pci/devices/"$f"/vfio-dev/vfio* > /dev/null 2>&1 || bound=no
done
ls /dev/vfio/devices/ 2>&1 | tr '\n' ' '
echo
if [ "$bound" = yes ] && ls /dev/vfio/devices/vfio* > /dev/null 2>&1; then
    stage "the drive is bound to vfio-pci and has its VFIO device node" ok
else
    stage "the drive is bound to vfio-pci and has its VFIO device node" FAILED
    dmesg | tail -n 30
    exit 1
fi

echo "==== $WHERE: the checklist of INSTALL.md for $funcs"
if sh "$SIM_DIR/../tools/pci-passthrough-check.sh" $funcs; then
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

if [ "${SIM_GUEST:-linux}" = win ]; then
    # Windows as the guest: its disk is the virtio disk of this machine, see
    # run.sh and win-phases.sh.
    lsblk 2> /dev/null || ls -l /dev/vd*
    if [ ! -b /dev/vda ]; then
        stage "the disk of Windows is there" FAILED
        dmesg | grep -i -E 'virtio|vda' | tail -n 10
        exit 1
    fi
    # The memory of the VM in huge pages where the kernel can: Windows goes
    # through all of its memory early in its start, and at this depth every
    # page a guest touches for the first time is costly.  With huge pages
    # there are 512 times fewer of them for the kernel to map.  Run 231:
    # Windows shows its start screen and does not get to its disk driver in
    # 40 minutes, with next to no exits to VirtualBox meanwhile.
    for f in enabled defrag; do
        echo always > /sys/kernel/mm/transparent_hugepage/$f 2> /dev/null
        echo "transparent huge pages, $f: $(cat /sys/kernel/mm/transparent_hugepage/$f 2>&1)"
    done
    VBOX_BIN=$SIM_WS/out/linux.amd64/release/bin \
    SIM_WORK=/tmp/sim \
    SIM_OUT=$OUT/$WHERE \
    SIM_STAGES=$STAGES \
    SIM_WHERE=$WHERE \
    SIM_HOSTADDR=$addr \
    SIM_HOSTFUNCS="$funcs" \
    WIN_DISK_DEV=/dev/vda \
    WIN_CMD_ISO=$SIM_WS/sim-work/cmd-shutdown.iso \
    WIN_CMD_INTX_ISO=$SIM_WS/sim-work/cmd-intx.iso \
    WIN_CMD_AUDIO_ISO=$SIM_WS/sim-work/cmd-audio.iso \
    WIN_START_WAIT=${SIM_BOOT_WAIT:-3600} \
        bash "$SIM_DIR/win-phases.sh" nested
    echo "==== $WHERE: up for $(cut -d' ' -f1 /proc/uptime) s; kernel messages about the IOMMU and VFIO"
    dmesg | grep -i -E 'DMAR.*fault|vfio|iommufd|AER|pcieport' | cut -c1-220 | tail -n 40
    dmesg > "$OUT/$WHERE/dmesg.log" 2> /dev/null
    exit 0
fi

VBOX_BIN=$SIM_WS/out/linux.amd64/release/bin \
SIM_WORK=/tmp/sim \
SIM_OUT=$OUT/$WHERE \
SIM_STAGES=$STAGES \
GUEST_RAW=$SIM_WS/sim-work/guest-quiet.raw \
GUEST_RAW_NOMSI=$SIM_WS/sim-work/guest-nomsi.raw \
SIM_WHERE=$WHERE \
SIM_HOSTADDR=$addr \
SIM_HOSTFUNCS="$funcs" \
SIM_BOOT_WAIT=${SIM_BOOT_WAIT:-300} \
SIM_PATIENCE=${SIM_PATIENCE:-3} \
    bash "$SIM_DIR/vbox-phases.sh" $PHASES

# The first test for a machine with real hardware (hw/linux/test-a.sh), with
# the drive in the place of a graphics card: it is to have run here before it
# runs where a run costs somebody's time.
if [ -f "$SIM_DIR/../hw/linux/test-a.sh" ]; then
    echo "==== $WHERE: hw/linux/test-a.sh, with the drive as the device"
    i=0
    while pgrep -x VBoxSVC > /dev/null 2>&1 && [ "$i" -lt 30 ]; do
        sleep 1
        i=$((i + 1))
    done
    mkdir -p "$OUT/$WHERE/hw-out"
    HW_WORK=/tmp/hw-work HW_OUT="$OUT/$WHERE/hw-out" HW_DEV=$addr \
    HW_VBOX_BIN=$SIM_WS/out/linux.amd64/release/bin \
        sh "$SIM_DIR/../hw/linux/test-a.sh" > "$OUT/$WHERE/hw-test-a.log" 2>&1
    rc=$?
    cut -c1-220 "$OUT/$WHERE/hw-test-a.log" | sed 's/^/    /'
    if [ "$rc" = 0 ]; then
        stage "the first script for a machine with real hardware goes through its checks with the drive" ok \
              "$(grep -c ': ok' "$OUT/$WHERE/hw-out/test-a.txt" 2> /dev/null) checks"
    else
        stage "the first script for a machine with real hardware goes through its checks with the drive" FAILED \
              "$(grep -c ': FAILED' "$OUT/$WHERE/hw-out/test-a.txt" 2> /dev/null) of its checks failed, or it did not get to them"
    fi
    # The second one, which puts a guest together from the kernel of the
    # machine it runs on and the drivers that kernel has for the device.
    if [ -f "$SIM_DIR/../hw/linux/test-b.sh" ]; then
        echo "==== $WHERE: hw/linux/test-b.sh, with the drive as the device"
        i=0
        while pgrep -x VBoxSVC > /dev/null 2>&1 && [ "$i" -lt 30 ]; do
            sleep 1
            i=$((i + 1))
        done
        kimg=$(ls "$SIM_WS/sim-work/kroot/boot/vmlinuz-$(uname -r)" "$SIM_WS/sim-work/vmlinuz-$(uname -r)" 2> /dev/null | head -n 1)
        HW_WORK=/tmp/hw-work HW_OUT="$OUT/$WHERE/hw-out" HW_DEV=$addr HW_KERNEL=$kimg HW_GUEST_MB=1536 HW_BOOT_WAIT=600 \
        HW_VBOX_BIN=$SIM_WS/out/linux.amd64/release/bin \
            sh "$SIM_DIR/../hw/linux/test-b.sh" > "$OUT/$WHERE/hw-test-b.log" 2>&1
        rc=$?
        cut -c1-220 "$OUT/$WHERE/hw-test-b.log" | sed 's/^/    /'
        if [ "$rc" = 0 ]; then
            stage "the second script for a machine with real hardware goes through its checks with the drive" ok \
                  "$(grep -c ': ok' "$OUT/$WHERE/hw-out/test-b.txt" 2> /dev/null) checks"
        else
            stage "the second script for a machine with real hardware goes through its checks with the drive" FAILED \
                  "$(grep -c ': FAILED' "$OUT/$WHERE/hw-out/test-b.txt" 2> /dev/null) of its checks failed, or it did not get to them"
        fi
    fi
fi

echo "==== $WHERE: up for $(cut -d' ' -f1 /proc/uptime) s; kernel messages about the IOMMU and VFIO"
dmesg | grep -i -E 'DMAR.*fault|vfio|iommufd|AER|pcieport' | cut -c1-220 | tail -n 40
dmesg > "$OUT/$WHERE/dmesg.log" 2> /dev/null
exit 0
