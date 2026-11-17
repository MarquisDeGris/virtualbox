#!/bin/bash
# Simulation: runs the VirtualBox which the workflow has built, with a small
# Linux guest, instead of only compiling it.
#
#   1. On the runner itself.  The runner has KVM and no IOMMU, so no device
#      can be passed through, but a VM can run: one without anything of this
#      work, and one with an empty hot-plug capable slot.
#   2. In a virtual machine which stands in for a host: QEMU with an emulated
#      IOMMU and an emulated NVMe drive behind a PCIe root port, running a
#      kernel which has VFIO.  In there the drive is bound to vfio-pci and
#      VirtualBox passes it on to its guest.
#
#      That machine is emulated in software (TCG), with an AMD processor whose
#      virtualization extension QEMU emulates too, and its time is counted in
#      instructions (-icount).  With KVM instead, the guest of VirtualBox is
#      the third level of hardware virtualization on a runner which is a
#      virtual machine itself; run 220 showed every exit from that guest to
#      take about a millisecond there, and the guest kernel not to get past
#      switching on its 1000 Hz timer tick.  In software all levels live in
#      one emulator and slow down together, clocks included, so nothing times
#      out against anything else.
#
#      Should that not get a guest to read from the drive, the machine is
#      tried once more with KVM and with time in the VM of VirtualBox slowed
#      down, to learn whether that is a way.
#
# The guest is the same in both: a kernel with guest-init, which tells on its
# serial port what it sees of the drive.  vbox-phases.sh drives VirtualBox and
# looks at what the guest tells.
#
# Results are lines "STAGE <where>/<phase>: <what>: ok|FAILED|skipped", on the
# output and in sim-out/stages.txt.  The exit code is 0 unless SIM_STRICT is
# "yes" and something failed or the second part did not get to run VirtualBox:
# while this is being set up, a run is for finding out, not for failing.

set -u
WS=${GITHUB_WORKSPACE:?}
SIM=$(cd "$(dirname "$0")" && pwd)
WORK=$WS/sim-work
OUT=$WS/sim-out
BIN=$WS/out/linux.amd64/release/bin
STAGES=$OUT/stages.txt
STRICT=${SIM_STRICT:-no}
export MTOOLS_SKIP_CHECK=1

rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT"
: > "$STAGES"

note() { echo; echo "==== $*"; }

stage() # <what> <ok|FAILED|skipped> [<note>]
{
    local line="runner/setup: $1: $2${3:+ ($3)}"
    echo "STAGE $line"
    echo "$line" >> "$STAGES"
}

finish()
{
    note "Result"
    cat "$STAGES"
    local failed ran
    failed=$(grep -c ': FAILED' "$STAGES")
    ran=$(grep -c '^simhost[^/]*/passthrough: ' "$STAGES")
    echo
    echo "$(grep -c ': ok' "$STAGES") ok, $failed failed, $(grep -c ': skipped' "$STAGES") skipped"
    if [ "$STRICT" = yes ] && { [ "$failed" != 0 ] || [ "$ran" = 0 ]; }; then
        exit 1
    fi
    exit 0
}


note "The runner"
uname -r
grep -m 1 'model name' /proc/cpuinfo
echo "CPU flags for virtualization: $(grep -m 1 '^flags' /proc/cpuinfo | grep -o -w -E 'svm|vmx' | tr '\n' ' ')"
for m in kvm_amd kvm_intel; do
    [ -e /sys/module/$m/parameters/nested ] && echo "$m nested=$(cat /sys/module/$m/parameters/nested)"
done
nproc
free -g | sed -n '1,2p'
ls -l /dev/kvm /dev/iommu 2>&1
grep -E '^CONFIG_(IOMMUFD|VFIO|VFIO_PCI|VFIO_DEVICE_CDEV|INTEL_IOMMU|AMD_IOMMU|HOTPLUG_PCI_PCIE|NET_9P_VIRTIO|9P_FS|BLK_DEV_NVME|KVM_AMD|KVM_INTEL)=' \
    "/boot/config-$(uname -r)" 2> /dev/null | tr '\n' ' '
echo

if [ ! -x "$BIN/VBoxManage" ] || [ ! -x "$BIN/VBoxHeadless" ]; then
    stage "VirtualBox was built" FAILED
    finish
fi


note "Tools"
for p in qemu-system-x86 busybox-static mtools dosfstools gdisk zstd cpio pciutils systemd-boot-efi systemd-ukify; do
    sudo apt-get install -y -q --no-install-recommends "$p" > "$WORK/apt-$p.log" 2>&1 \
        || { echo "not installed: $p"; tail -n 3 "$WORK/apt-$p.log"; }
done
qemu-system-x86_64 --version | head -n 1
busybox | head -n 1
if busybox --list > /dev/null 2>&1 && ! ldd "$(command -v busybox)" > /dev/null 2>&1; then
    echo "busybox is statically linked"
else
    echo "busybox is NOT statically linked, the guests will not get far"
fi


# The kernel for the guest and for the simulated host: the generic kernel of
# the distribution, as somebody trying this on a PC would have it.  Its
# packages are unpacked, not installed; the modules go where modprobe looks.
note "Kernel"
KVER=
VMLINUZ=
get_generic_kernel()
{
    local meta img pkgs p moddir
    for meta in linux-image-generic-hwe-24.04 linux-image-generic; do
        img=$(apt-cache depends "$meta" 2> /dev/null \
              | awk '$1 == "Depends:" && $2 ~ /^linux-image-[0-9].*-generic$/ { print $2; exit }')
        [ -n "$img" ] && break
    done
    [ -n "$img" ] || { echo "no generic kernel image found"; return 1; }
    KVER=${img#linux-image-}
    echo "generic kernel: $KVER (from $meta)"
    dpkg --compare-versions "${KVER%%-*}" ge 6.12 || { echo "older than 6.12"; return 1; }
    pkgs="$img linux-modules-$KVER"
    apt-cache show "linux-modules-extra-$KVER" > /dev/null 2>&1 && pkgs="$pkgs linux-modules-extra-$KVER"
    mkdir -p "$WORK/kdeb" "$WORK/kroot"
    ( cd "$WORK/kdeb" && apt-get download $pkgs ) > "$WORK/kernel-download.log" 2>&1 \
        || { tail -n 5 "$WORK/kernel-download.log"; return 1; }
    for p in "$WORK"/kdeb/*.deb; do
        dpkg-deb -x "$p" "$WORK/kroot" || return 1
    done
    VMLINUZ=$WORK/kroot/boot/vmlinuz-$KVER
    moddir=$(ls -d "$WORK/kroot/lib/modules/$KVER" "$WORK/kroot/usr/lib/modules/$KVER" 2> /dev/null | head -n 1)
    [ -f "$VMLINUZ" ] && [ -d "$moddir" ] || { echo "the packages do not have what is expected"; ls "$WORK/kroot"; return 1; }
    [ "$KVER" != "$(uname -r)" ] || { echo "this is the kernel which is running"; return 1; }
    sudo rm -rf "/lib/modules/$KVER"
    sudo cp -a "$moddir" "/lib/modules/$KVER" || return 1
    sudo depmod "$KVER" || return 1
}
if ! get_generic_kernel; then
    # The kernel the runner runs.
    KVER=$(uname -r)
    VMLINUZ=$WORK/vmlinuz-$KVER
    sudo cat "/boot/vmlinuz-$KVER" > "$VMLINUZ"
    echo "using the kernel of the runner instead: $KVER"
fi
ls -l "$VMLINUZ"
if [ ! -s "$VMLINUZ" ]; then
    stage "a kernel for the guest" FAILED
    finish
fi
grep -E '^CONFIG_(IOMMUFD|VFIO|VFIO_PCI|VFIO_DEVICE_CDEV|INTEL_IOMMU|HOTPLUG_PCI_PCIE|NET_9P_VIRTIO|9P_FS|BLK_DEV_NVME|VIRTIO_PCI|KVM_AMD|KVM_INTEL)=' \
    "$WORK/kroot/boot/config-$KVER" "/boot/config-$KVER" 2> /dev/null | sed 's/^.*config-[^:]*://' | sort -u | tr '\n' ' '
echo


# Copies the modules named and what they depend on into a directory,
# uncompressed, and lists them in the order they have to be loaded in.
collect_modules() # <root of the initramfs> <module>...
{
    local root=$1 m kind f rest base
    shift
    mkdir -p "$root/modules"
    : > "$root/modules.list"
    for m in "$@"; do
        modprobe -S "$KVER" --show-depends "$m" 2> /dev/null | while read -r kind f rest; do
            [ "$kind" = insmod ] || continue
            base=$(basename "$f")
            base=${base%.zst}
            base=${base%.xz}
            base=${base%.gz}
            [ -e "$root/modules/$base" ] && continue
            case "$f" in
                *.zst) zstd -q -d -c "$f" > "$root/modules/$base" ;;
                *.xz)  xz -d -c "$f" > "$root/modules/$base" ;;
                *.gz)  gzip -d -c "$f" > "$root/modules/$base" ;;
                *)     cp "$f" "$root/modules/$base" ;;
            esac
            echo "$base" >> "$root/modules.list"
        done
    done
    echo "modules for $(basename "$root"): $(tr '\n' ' ' < "$root/modules.list")"
}

make_initrd() # <name> <init script> <module>...
{
    local name=$1 init=$2 root=$WORK/$1-root
    shift 2
    rm -rf "$root"
    mkdir -p "$root/bin" "$root/dev" "$root/proc" "$root/sys" "$root/tmp"
    cp "$(command -v busybox)" "$root/bin/busybox"
    cp "$init" "$root/init"
    chmod 755 "$root/init" "$root/bin/busybox"
    collect_modules "$root" "$@"
    ( cd "$root" && find . | cpio -o -H newc -R 0:0 --quiet | gzip -1 ) > "$WORK/$name.cpio.gz"
    ls -l "$WORK/$name.cpio.gz"
}


note "The guest"
make_initrd guest "$SIM/guest-init" nvme

# A disk for the guest: a GPT with an EFI system partition which has the
# kernel as a unified kernel image where the firmware looks for one, and, should
# that not work, a script for the shell of the firmware.
make_guest_disk() # <name> <kernel command line>
{
    local name=$1 cmdline=$2 esp=$WORK/$1-esp.img
    rm -f "$esp" "$WORK/$name.raw" "$WORK/$name-BOOTX64.EFI"
    truncate -s 200M "$esp"
    mkfs.vfat -F 32 -n SIM "$esp" > /dev/null
    mmd -i "$esp" ::/EFI ::/EFI/BOOT
    mcopy -i "$esp" "$VMLINUZ" ::/vmlinuz.efi
    mcopy -i "$esp" "$WORK/guest.cpio.gz" ::/initrd.img
    printf 'echo -off\r\nfs0:\r\n\\vmlinuz.efi %s initrd=\\initrd.img\r\n' "$cmdline" > "$WORK/$name-startup.nsh"
    mcopy -i "$esp" "$WORK/$name-startup.nsh" ::/startup.nsh
    if [ -n "$UKIFY" ] && "$UKIFY" build --linux="$VMLINUZ" --initrd="$WORK/guest.cpio.gz" --cmdline="$cmdline" \
           --output="$WORK/$name-BOOTX64.EFI" > "$WORK/$name-ukify.log" 2>&1; then
        mcopy -i "$esp" "$WORK/$name-BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
    else
        echo "$name: no unified kernel image, the guest boots through the shell of the firmware"
        tail -n 5 "$WORK/$name-ukify.log" 2> /dev/null
    fi
    truncate -s 204M "$WORK/$name.raw"
    sgdisk -n 1:2048:+200M -t 1:ef00 "$WORK/$name.raw" > /dev/null
    dd if="$esp" of="$WORK/$name.raw" bs=1M seek=1 conv=notrunc status=none
    echo "$name.raw: $(mdir -i "$esp" -/ -b :: | tr '\n' ' ')"
    rm -f "$esp"
}
UKIFY=$(command -v ukify || ls /usr/lib/systemd/ukify 2> /dev/null)
# On the runner the kernel of the guest tells everything on the serial port.
# Where virtualization is nested every character there is costly, so there it
# keeps quiet and guest-init passes on what matters.
make_guest_disk guest "console=ttyS0,115200 ignore_loglevel rdinit=/init panic=30 pciehp.dyndbg=+p"
make_guest_disk guest-quiet "console=ttyS0,115200 quiet loglevel=3 mitigations=off rdinit=/init panic=30 pciehp.dyndbg=+p"

# The drive: random data, so that the guest can only tell the checksum of
# what it has read from it.
head -c 16M /dev/urandom > "$WORK/nvme.img"
MD5=$(head -c 8M "$WORK/nvme.img" | md5sum | cut -d' ' -f1)
echo "checksum of the first 8 MiB of the drive: $MD5"


note "Part 1: VirtualBox on the runner"
sudo chmod 666 /dev/kvm
sudo modprobe iommufd 2> /dev/null || echo "the kernel of the runner has no iommufd module"
mkdir -p "$OUT/runner"
sudo env VBOX_BIN="$BIN" SIM_WORK=/tmp/sim-runner SIM_OUT="$OUT/runner" SIM_STAGES="$STAGES" \
         GUEST_RAW="$WORK/guest.raw" SIM_WHERE=runner SIM_BOOT_WAIT=240 \
    timeout -k 30 1500 bash "$SIM/vbox-phases.sh" plain emptyslot
echo "part 1 ended with $?"


note "Part 2: VirtualBox in a virtual machine with an IOMMU and a PCIe device"
make_initrd l1 "$SIM/l1-init" virtio_pci 9pnet_virtio 9p
if ! grep -q '^9p\.ko$' "$WORK/l1-root/modules.list" \
   && ! grep -q -x 'CONFIG_9P_FS=y' "$WORK/kroot/boot/config-$KVER" "/boot/config-$KVER" 2> /dev/null; then
    stage "the kernel can mount the file system of the runner" FAILED "no 9p"
    finish
fi
# Runs the virtual machine which stands in for the host until its script is
# done or the time is up.
run_simhost() # <tcg|kvm> <name in the results> <phases> <seconds to wait for a guest> <warp or ""> <time limit in seconds>
{
    local accel=$1 where=$2 phases=$3 bootwait=$4 warp=$5 limit=$6 t0 rc
    local cmdline="console=ttyS0 loglevel=4 mitigations=off intel_iommu=on rdinit=/init panic=-1"
    cmdline="$cmdline SIM_DIR=$SIM SIM_WS=$WS SIM_MD5=$MD5 SIM_WHERE=$where SIM_PHASES=$phases SIM_BOOT_WAIT=$bootwait"
    [ -n "$warp" ] && cmdline="$cmdline SIM_WARP=$warp"
    local -a machine
    if [ "$accel" = tcg ]; then
        # All of it in software, with an AMD processor as QEMU emulates one, and
        # the time of the machine counted in instructions: one nanosecond each.
        machine=(-machine q35 -accel tcg -cpu max,la57=off -smp 2 -m 6G -icount shift=0,sleep=on)
    else
        machine=(-machine q35,kernel-irqchip=split -accel kvm -cpu host -smp 4 -m 8G)
    fi
    t0=$(date +%s)
    sudo timeout -k 30 "$limit" qemu-system-x86_64 "${machine[@]}" \
        -nodefaults -display none -monitor none -serial stdio -no-reboot \
        -kernel "$VMLINUZ" -initrd "$WORK/l1.cpio.gz" -append "$cmdline" \
        -device intel-iommu,intremap=on \
        -virtfs "local,path=/,mount_tag=hostroot,security_model=none,readonly=on,multidevs=remap" \
        -virtfs "local,path=$OUT,mount_tag=simout,security_model=none" \
        -device pcie-root-port,id=rp1,chassis=1,slot=1 \
        -drive "file=$WORK/nvme.img,if=none,id=nvm0,format=raw" \
        -device nvme,drive=nvm0,serial=SIM0001,bus=rp1 \
        < /dev/null 2>&1 | tr -d '\r' | cut -c1-400 | tee "$OUT/$where-console.log"
    rc=${PIPESTATUS[0]}
    echo "$where ($accel) ended with $rc after $(( $(date +%s) - t0 )) s"
    if ! grep -q "^$where/setup: KVM is there" "$STAGES"; then
        stage "$where: the virtual machine standing in for the host ran its script" FAILED
    fi
}

run_simhost tcg simhost plain,passthrough,hotplug,startempty,hostremove 150 "" 6000

if ! grep -q '^simhost/passthrough: guest: reads from the drive which is passed through: ok' "$STAGES"; then
    note "Part 2 once more, with KVM and time in the VM at a twentieth of its speed"
    run_simhost kvm simhost-kvm plain,passthrough 900 5 2400
fi

if grep -q 'written by the guest' "$WORK/nvme.img"; then
    stage "what the guest wrote is on the drive of the simulated host" ok \
          "$(grep -a -o 'written by the guest, round [0-9]*' "$WORK/nvme.img" | tail -n 1)"
elif grep -q '^simhost[^/]*/passthrough: guest: writes to the drive: ok' "$STAGES"; then
    stage "what the guest wrote is on the drive of the simulated host" FAILED
fi

sudo chmod -R a+rX "$OUT" 2> /dev/null
finish
