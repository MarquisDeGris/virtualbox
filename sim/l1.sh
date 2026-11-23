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
# A build with the sanitizers (SIM_SAN on the command line of the kernel):
# where they write, which is a directory of the runner, where the programs
# find the libraries they load while they run, and more patience with a
# guest, as everything takes longer.  "mask" lets one kind of finding pass
# which every program of a tree without a certain correction has, see run.sh.
if [ -n "${SIM_SAN:-}" ]; then
    mkdir -p "$OUT/sanitizers"
    ASAN_OPTIONS="detect_leaks=0:log_path=$OUT/sanitizers/asan"
    [ "$SIM_SAN" = mask ] && ASAN_OPTIONS="$ASAN_OPTIONS:new_delete_type_mismatch=0"
    UBSAN_OPTIONS="print_stacktrace=1:log_path=$OUT/sanitizers/ubsan"
    LD_LIBRARY_PATH="$SIM_WS/out/linux.amd64/release/bin"
    export ASAN_OPTIONS UBSAN_OPTIONS LD_LIBRARY_PATH
    SIM_PATIENCE=$(( ${SIM_PATIENCE:-3} * 2 ))
    export SIM_PATIENCE
fi

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
# every function at its address.  SIM_DEVCLASS names another class than that
# of an NVMe controller for a part which has another device.
addr=
for d in /sys/bus/pci/devices/*; do
    [ -z "$addr" ] && [ "$(cat "$d/class")" = "${SIM_DEVCLASS:-0x010802}" ] && addr=${d##*/}
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
if [ "${SIM_HWTEST:-}" = bar ]; then
    # The script for a machine with real hardware binds the device, as it
    # would there, and keeps what the ROM of the device is.
    echo "==== $WHERE: hw/linux/bind.sh --do"
    HW_WORK=/tmp/hw-work HW_OUT="$OUT/$WHERE/hw-out" HW_DEV=$addr sh "$SIM_DIR/../hw/linux/bind.sh" --do || bound=no
    for f in $funcs; do
        ls /sys/bus/pci/devices/"$f"/vfio-dev/vfio* > /dev/null 2>&1 || bound=no
    done
else
    for f in $funcs; do
        cur=
        [ -e "/sys/bus/pci/devices/$f/driver" ] && cur=$(basename "$(readlink "/sys/bus/pci/devices/$f/driver")")
        [ -n "$cur" ] && [ "$cur" != vfio-pci ] && echo "$f" > "/sys/bus/pci/devices/$f/driver/unbind"
        echo vfio-pci > "/sys/bus/pci/devices/$f/driver_override"
        echo "$f" > /sys/bus/pci/drivers_probe
        ls "/sys/bus/pci/devices/$f/vfio-dev/" 2>&1 | tr '\n' ' '
        ls /sys/bus/pci/devices/"$f"/vfio-dev/vfio* > /dev/null 2>&1 || bound=no
    done
fi
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

# The scripts for a machine with real hardware, one after the other, with the
# device of this machine in the place of a graphics card.  HW_CMDLINE is for
# the guest of the second one.
hw_script() # <a|b> <what the stage calls it> <its result, under the directory of this machine in the results> <name of its log> [<VARIABLE=value>...]
{
    local t=$1 name=$2 res=$3 log=$4 rc kimg i=0
    shift 4
    [ -f "$SIM_DIR/../hw/linux/test-$t.sh" ] || return 0
    kimg=$(ls "$SIM_WS/sim-work/kroot/boot/vmlinuz-$(uname -r)" "$SIM_WS/sim-work/vmlinuz-$(uname -r)" 2> /dev/null | head -n 1)
    mkdir -p "$OUT/$WHERE/hw-out"
    echo "==== $WHERE: hw/linux/test-$t.sh${*:+ with $*}, with the device of this machine"
    while pgrep -x VBoxSVC > /dev/null 2>&1 && [ "$i" -lt 30 ]; do
        sleep 1
        i=$((i + 1))
    done
    env HW_WORK=/tmp/hw-work HW_OUT="$OUT/$WHERE/hw-out" HW_DEV="$addr" HW_KERNEL="$kimg" HW_GUEST_MB=1536 HW_BOOT_WAIT=600 \
        HW_CPUS=1 HW_WARP="${SIM_WARP:-5}" \
        HW_VBOX_BIN="$SIM_WS/out/linux.amd64/release/bin" "$@" \
        sh "$SIM_DIR/../hw/linux/test-$t.sh" > "$OUT/$WHERE/$log.log" 2>&1
    rc=$?
    cut -c1-220 "$OUT/$WHERE/$log.log" | sed 's/^/    /'
    # The log of the VM of the script, which the next script removes.
    cp -f /tmp/hw-work/vms/hwtest/Logs/VBox.log "$OUT/$WHERE/$log-VBox.log" 2> /dev/null
    if [ "$rc" = 0 ]; then
        stage "$name goes through its checks with the device" ok \
              "$(grep -c ': ok' "$OUT/$WHERE/$res" 2> /dev/null) checks"
    else
        stage "$name goes through its checks with the device" FAILED \
              "$(grep -c ': FAILED' "$OUT/$WHERE/$res" 2> /dev/null) of its checks failed, or it did not get to them"
    fi
}

hw_scripts()
{
    hw_script a "the first script for a machine with real hardware" hw-out/test-a.txt hw-test-a
    hw_script b "the second script for a machine with real hardware" hw-out/test-b.txt hw-test-b
    if [ "${SIM_HWTEST:-}" = vga ]; then
        # Once more with the legacy VGA ranges of the card given to the
        # guest, which writes to registers and memory there: nobody looks at
        # this card.
        hw_script b "the second script, with a VM which is given the legacy VGA ranges of the card," hw-out/test-b-vga.txt hw-test-b-vga \
                  HW_VGA=yes HW_CMDLINE=hwvgawrite=1
    fi
    if [ -n "${SIM_HWBIN:-}" ] && [ -x "$SIM_HWBIN/VBoxManage" ]; then
        # The first script once more, with the build as it is published for
        # machines with real hardware and as the runner fetched it.
        hw_script a "the first script, with the build as it is published," hw-out-published/test-a.txt hw-test-a-published \
                  HW_OUT="$OUT/$WHERE/hw-out-published" HW_VBOX_BIN="$SIM_HWBIN" LD_LIBRARY_PATH=
    fi
}

# The inventory and the checklist for the device, as the scripts for a machine
# with real hardware write them.
hw_inventory()
{
    local what
    mkdir -p "$OUT/$WHERE/hw-out"
    echo "==== $WHERE: hw/run.sh"
    HW_OUT="$OUT/$WHERE/hw-out" sh "$SIM_DIR/../hw/run.sh" 2>&1 | cut -c1-220 | sed 's/^/    /'
    what="the inventory script tells of the IOMMU of this machine"
    if grep -q '^iommu-groups: [1-9]' "$OUT/$WHERE/hw-out/inventory.txt" 2> /dev/null; then
        stage "$what" ok "$(grep '^iommu-groups: ' "$OUT/$WHERE/hw-out/inventory.txt")"
    else
        stage "$what" FAILED "$(grep '^iommu-groups: ' "$OUT/$WHERE/hw-out/inventory.txt" 2> /dev/null)"
    fi
    if [ "${SIM_HWTEST:-}" = vga ]; then
        what="the inventory script tells of the graphics card"
        if grep -q "^pci-$addr: class 0x03" "$OUT/$WHERE/hw-out/inventory.txt" 2> /dev/null; then
            stage "$what" ok "$(grep "^pci-$addr: " "$OUT/$WHERE/hw-out/inventory.txt" | cut -c1-160)"
        else
            stage "$what" FAILED
        fi
    fi
    echo "==== $WHERE: hw/linux/prereq.sh"
    what="the script with the checklist finds nothing missing for the device"
    if HW_WORK=/tmp/hw-work HW_OUT="$OUT/$WHERE/hw-out" HW_DEV="$addr" sh "$SIM_DIR/../hw/linux/prereq.sh" > "$OUT/$WHERE/hw-prereq.log" 2>&1 \
       && grep -q '^the checklist finds nothing missing: ok' "$OUT/$WHERE/hw-out/prereq.txt" 2> /dev/null; then
        stage "$what" ok
    else
        stage "$what" FAILED "$(grep -m 1 -E 'FAILED|MISSING' "$OUT/$WHERE/hw-out/prereq.txt" 2> /dev/null | cut -c1-160)"
    fi
    what="the script with the checklist finds what the second test puts its guest together with"
    if grep -q '^what the second test needs to put its guest together is there: ok' "$OUT/$WHERE/hw-out/prereq.txt" 2> /dev/null; then
        stage "$what" ok
    else
        stage "$what" FAILED "$(grep -m 1 '^what the second test needs' "$OUT/$WHERE/hw-out/prereq.txt" 2> /dev/null | cut -c1-200)"
    fi
}

if [ -n "${SIM_HWTEST:-}" ]; then
    # A device which is no drive, see run.sh, parts 2c and 2d: nothing of the
    # phases with the drive applies, the scripts for real hardware do.
    ids="$(cat "/sys/bus/pci/devices/$addr/vendor" | sed 's/^0x//'):$(cat "/sys/bus/pci/devices/$addr/device" | sed 's/^0x//')"
    echo "the memory of the device: $(cat "/sys/bus/pci/devices/$addr/resource" | awk '$1 != "0x0000000000000000" { printf "%s..%s ", $1, $2 }')"
    if [ "$SIM_HWTEST" = bar ]; then
        # The guest of the second script writes to the memory of the device.
        export HW_CMDLINE="hwbarwrite=$ids"
        # The device has the vendor of the virtio devices, whose driver names
        # all of that vendor and takes this one not.
        export HW_EXPECT_DRIVER=no
    fi
    if [ -n "${SIM_ROM:-}" ]; then
        # The ROM of the device is a file on the runner, which told how many
        # bytes it has and their checksum.  What the script which bound the
        # device kept of the ROM is what the second script compares with.
        seen=$(grep "^$addr " /tmp/hw-work/rom-seen.txt 2> /dev/null | head -n 1)
        if [ "$seen" = "$addr $ids ${SIM_ROM%%:*} ${SIM_ROM##*:}" ]; then
            stage "the script which binds the device read its ROM as the file of the ROM is" ok "${SIM_ROM%%:*} bytes"
        else
            stage "the script which binds the device read its ROM as the file of the ROM is" FAILED "the file: ${SIM_ROM%%:*} ${SIM_ROM##*:}; kept: ${seen:-nothing}"
        fi
    fi
    hw_inventory
    hw_scripts
    if [ "$SIM_HWTEST" = bar ]; then
        # The script which bound the device gives it back to the host, which
        # has no driver for this one.
        echo "==== $WHERE: hw/linux/bind.sh --undo"
        HW_WORK=/tmp/hw-work HW_OUT="$OUT/$WHERE/hw-out" HW_DEV=$addr sh "$SIM_DIR/../hw/linux/bind.sh" --undo
        cur=none
        [ -e "/sys/bus/pci/devices/$addr/driver" ] && cur=$(basename "$(readlink "/sys/bus/pci/devices/$addr/driver")")
        if [ "$cur" != vfio-pci ] && [ -z "$(tr -d '\n' < "/sys/bus/pci/devices/$addr/driver_override" | sed 's/(null)//')" ]; then
            stage "the script which binds the device gives it back to the host" ok "its driver now: $cur"
        else
            stage "the script which binds the device gives it back to the host" FAILED "its driver now: $cur, told to be: $(cat "/sys/bus/pci/devices/$addr/driver_override")"
        fi
        # And all of it once more the way somebody at such a machine starts
        # it: the one script which binds the device, goes through the
        # checklist as root and runs the tests.
        echo "==== $WHERE: hw/linux/tests.sh"
        i=0
        while pgrep -x VBoxSVC > /dev/null 2>&1 && [ "$i" -lt 30 ]; do
            sleep 1
            i=$((i + 1))
        done
        kimg=$(ls "$SIM_WS/sim-work/kroot/boot/vmlinuz-$(uname -r)" "$SIM_WS/sim-work/vmlinuz-$(uname -r)" 2> /dev/null | head -n 1)
        env HW_WORK=/tmp/hw-work HW_OUT="$OUT/$WHERE/hw-out-all" HW_DEV="$addr" HW_KERNEL="$kimg" HW_GUEST_MB=1536 HW_BOOT_WAIT=600 \
            HW_CPUS=1 HW_WARP="${SIM_WARP:-5}" HW_VBOX_BIN="$SIM_WS/out/linux.amd64/release/bin" \
            sh "$SIM_DIR/../hw/linux/tests.sh" > "$OUT/$WHERE/hw-tests.log" 2>&1
        rc=$?
        cut -c1-220 "$OUT/$WHERE/hw-tests.log" | sed 's/^/    /'
        cp -f /tmp/hw-work/vms/hwtest/Logs/VBox.log "$OUT/$WHERE/hw-tests-VBox.log" 2> /dev/null
        # What the first test of it found in the log of its VM, if it did.
        sed -n '/^---- the lines of it which tell/,$p' "$OUT/$WHERE/hw-out-all/test-a.txt" 2> /dev/null | cut -c1-240 | sed 's/^/    | /'
        if [ "$rc" = 0 ]; then
            stage "the one script for all which takes root binds the device and has both tests go through their checks" ok \
                  "$(grep -c ': ok' "$OUT/$WHERE/hw-out-all/test-a.txt" 2> /dev/null) and $(grep -c ': ok' "$OUT/$WHERE/hw-out-all/test-b.txt" 2> /dev/null) checks"
        else
            stage "the one script for all which takes root binds the device and has both tests go through their checks" FAILED \
                  "$(tail -n 1 "$OUT/$WHERE/hw-tests.log" | cut -c1-160)"
        fi
    fi
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

# The scripts for a machine with real hardware, with the drive in the place
# of a graphics card: they are to have run here before they run where a run
# costs somebody's time.
hw_scripts

echo "==== $WHERE: up for $(cut -d' ' -f1 /proc/uptime) s; kernel messages about the IOMMU and VFIO"
dmesg | grep -i -E 'DMAR.*fault|vfio|iommufd|AER|pcieport' | cut -c1-220 | tail -n 40
dmesg > "$OUT/$WHERE/dmesg.log" 2> /dev/null
exit 0
