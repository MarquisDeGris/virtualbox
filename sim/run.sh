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
#      The runner is a virtual machine itself, so the guest of VirtualBox is
#      the third level of hardware virtualization at least, and every exit
#      from it to VirtualBox takes about a millisecond (run 220).  A kernel
#      does not get past switching on its timer tick of 1000 interrupts a
#      second that way: while they still go through the PIC each takes
#      several exits.  So the timers VirtualBox emulates run at a twentieth
#      of their speed in that VM (the "warp drive" of its time manager).
#      Once the kernel is up it takes its time from KVM and none of that
#      matters any more (run 221).
#
#      Emulating that machine in software instead, with an AMD processor
#      whose virtualization extension QEMU emulates as well, was tried in run
#      221: VirtualBox does not get a guest going on that, with or without a
#      device.
#   2b. The same with a device of two functions (SIM_PARTS has "mfhost"): two
#      NVMe controllers at one address behind the root port, each with a drive
#      of its own.  The host has them in one IOMMU group, and VirtualBox passes
#      them on as the one device they are: what a graphics card with its audio
#      function is.
#   3. With Windows as the guest (SIM_PARTS has "win"): on the runner, a VM
#      with an empty hot-plug capable slot into which Windows Server is
#      installed from its evaluation image, see win-phases.sh.
#   4. That Windows in a virtual machine as in 2 (SIM_PARTS has "winhost"),
#      with the drive in the hot-plug capable slot: the drive is taken out
#      and put in while Windows runs.
#
#      That machine tells its processor to have an invariant time stamp
#      counter, which QEMU leaves out unless asked.  Windows reads the time
#      from the HPET of VirtualBox otherwise, with an exit for every look at
#      the clock: five million of them in 70 minutes without getting through
#      its start (run 230), where a start takes less than half a million exits
#      in all when the counter is there.  With the counter told of, Windows
#      leaves the HPET alone (run 231).
#
#   4b. The same with the device of two functions of 2b (SIM_PARTS has
#      "mfwinhost"), whose functions the host cannot reset by themselves
#      there: what a graphics card with its audio function mostly is.
#
# Which parts run is told by SIM_PARTS, a list with commas: runner, simhost,
# mfhost, win, winhost, mfwinhost.  Without it: runner,simhost.
#
# The guest is the same in 1 and 2: a kernel with guest-init, which tells on
# its serial port what it sees of the drive.  vbox-phases.sh drives VirtualBox
# and looks at what the guest tells.
#
# Results are lines "STAGE <where>/<phase>: <what>: ok|FAILED|skipped", on the
# output and in sim-out/stages.txt.  The exit code is 0 unless SIM_STRICT says
# otherwise: while this is being set up, a run is for finding out, not for
# failing.  With "yes" the exit code is 1 if something failed or a part did
# not get as far as running its guest.  With "linux" that goes for parts 1
# and 2 only, and parts 3 and 4 tell what they found: what Windows does with
# the slot is still being found out, and a download from Microsoft is part of
# it.

set -u
WS=${GITHUB_WORKSPACE:?}
SIM=$(cd "$(dirname "$0")" && pwd)
WORK=$WS/sim-work
OUT=$WS/sim-out
BIN=$WS/out/linux.amd64/release/bin
STAGES=$OUT/stages.txt
STRICT=${SIM_STRICT:-no}
PARTS=,${SIM_PARTS:-runner,simhost},
# The number of lines in $STAGES when the parts with Windows begin.
LINUX_STAGES=1000000
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

has_part() # <runner|simhost|mfhost|win|winhost|mfwinhost>
{
    case "$PARTS" in *",$1,"*) return 0 ;; esac
    return 1
}

finish()
{
    note "Result"
    cat "$STAGES"
    local failed counted missing=0
    failed=$(grep -c ': FAILED' "$STAGES")
    counted=$failed
    # A part which was asked for and did not get to its work counts as failed.
    if has_part simhost && ! grep -q '^simhost[^/]*/passthrough: ' "$STAGES"; then
        missing=1
    fi
    if has_part mfhost && ! grep -q '^mfhost/mfpassthrough: ' "$STAGES"; then
        missing=1
    fi
    if [ "$STRICT" = linux ]; then
        # What came before the parts with Windows decides.
        counted=$(head -n "$LINUX_STAGES" "$STAGES" | grep -c ': FAILED')
    else
        if has_part win && ! grep -q '^runner/win-install: Windows is installed.*: ok' "$STAGES"; then
            missing=1
        fi
        if    has_part winhost && ! grep -q '^winhost/win-hotplug: Windows starts with the drive.*: ok' "$STAGES" \
           && ! grep -q '^runner/setup: Windows with the drive in the slot: skipped (no AMD processor' "$STAGES"; then
            missing=1
        fi
        if    has_part mfwinhost && ! grep -q '^mfwinhost/win-hotplug: Windows starts with the drive.*: ok' "$STAGES" \
           && ! grep -q '^runner/setup: Windows with the device of two functions in the slot: skipped (no AMD processor' "$STAGES"; then
            missing=1
        fi
    fi
    echo
    echo "$(grep -c ': ok' "$STAGES") ok, $failed failed, $(grep -c ': skipped' "$STAGES") skipped"
    case "$STRICT" in
        linux) echo "failed in the parts with the Linux guest, which decide here: $counted" ;;
        yes)   ;;
        *)     exit 0 ;;
    esac
    if [ "$counted" != 0 ] || [ "$missing" != 0 ]; then
        exit 1
    fi
    exit 0
}


note "The runner"
uname -r
grep -m 1 'model name' /proc/cpuinfo
echo "CPU flags for virtualization: $(grep -m 1 '^flags' /proc/cpuinfo | grep -o -w -E 'svm|vmx' | tr '\n' ' ')"
echo "CPU flags for the time stamp counter: $(grep -m 1 '^flags' /proc/cpuinfo | grep -o -w -E 'constant_tsc|nonstop_tsc|tsc_reliable' | tr '\n' ' ')"
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
make_initrd guest "$SIM/guest-init" nvme virtio_net

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
# A guest which does not use MSI or MSI-X: its drivers get the INTx interrupts
# of their devices (the phase "intx").
make_guest_disk guest-nomsi "console=ttyS0,115200 quiet loglevel=3 mitigations=off rdinit=/init panic=30 pci=nomsi"

# The drive: random data, so that the guest can only tell the checksum of
# what it has read from it.
head -c 16M /dev/urandom > "$WORK/nvme.img"
MD5=$(head -c 8M "$WORK/nvme.img" | md5sum | cut -d' ' -f1)
echo "checksum of the first 8 MiB of the drive: $MD5"
# The drive of the second function, where the device has two.
head -c 16M /dev/urandom > "$WORK/nvme2.img"
MD5B=$(head -c 8M "$WORK/nvme2.img" | md5sum | cut -d' ' -f1)
echo "checksum of the first 8 MiB of the second drive: $MD5B"


note "The checklist of INSTALL.md on the runner, which has no IOMMU"
sh "$SIM/../tools/pci-passthrough-check.sh" > "$WORK/check-runner.log" 2>&1
rc=$?
cat "$WORK/check-runner.log"
if [ "$rc" = 1 ] && grep -q '^MISSING  no IOMMU in use' "$WORK/check-runner.log"; then
    stage "the checklist script tells that the runner has no IOMMU" ok
else
    stage "the checklist script tells that the runner has no IOMMU" FAILED "exit code $rc"
fi


note "Part 1: VirtualBox on the runner"
sudo chmod 666 /dev/kvm
sudo modprobe iommufd 2> /dev/null || echo "the kernel of the runner has no iommufd module"
mkdir -p "$OUT/runner"
# For the phase "nat": a file the guest fetches through the NAT engine from
# this machine, which the guest reaches as its gateway (guest-init has the
# port).  Nothing but this machine itself can reach the server.
mkdir -p "$WORK/http"
head -c 2M /dev/urandom > "$WORK/http/sim.bin"
HTTP_MD5=$(md5sum < "$WORK/http/sim.bin" | cut -d' ' -f1)
( cd "$WORK/http" && exec python3 -m http.server 8099 --bind 127.0.0.1 > "$WORK/http.log" 2>&1 ) &
HTTP_PID=$!
sudo env VBOX_BIN="$BIN" SIM_WORK=/tmp/sim-runner SIM_OUT="$OUT/runner" SIM_STAGES="$STAGES" \
         GUEST_RAW="$WORK/guest.raw" SIM_WHERE=runner SIM_BOOT_WAIT=240 SIM_HTTP_MD5="$HTTP_MD5" \
    timeout -k 30 1800 bash "$SIM/vbox-phases.sh" plain emptyslot nat import
echo "part 1 ended with $?"
kill "$HTTP_PID" 2> /dev/null
echo "requests to the file server: $(grep -c 'GET /sim.bin' "$WORK/http.log" 2> /dev/null)"


part_simhost_prepare()
{
    make_initrd l1 "$SIM/l1-init" virtio_pci 9pnet_virtio 9p virtio_blk
    if ! grep -q '^9p\.ko$' "$WORK/l1-root/modules.list" \
       && ! grep -q -x 'CONFIG_9P_FS=y' "$WORK/kroot/boot/config-$KVER" "/boot/config-$KVER" 2> /dev/null; then
        stage "the kernel can mount the file system of the runner" FAILED "no 9p"
        return 1
    fi
}
# Runs the virtual machine which stands in for the host until its script is
# done or the time is up.
run_simhost() # <name in the results> <phases> <seconds to wait for a guest> <warp or ""> <time limit in seconds> [<disk image of a Windows guest or ""> [<functions of the device: 1 or 2>]]
{
    local where=$1 phases=$2 bootwait=$3 warp=$4 limit=$5 windisk=${6:-} funcs=${7:-1} t0 rc
    # The eighth argument, "noreset": the functions of the device are ones
    # the host cannot reset by themselves, see forget_resets in
    # vbox-phases.sh.  The phases with the Linux guest have one of their own
    # for that; with Windows it is so from the start.
    local cmdline="console=ttyS0 loglevel=4 mitigations=off intel_iommu=on rdinit=/init panic=-1"
    cmdline="$cmdline SIM_DIR=$SIM SIM_WS=$WS SIM_MD5=$MD5 SIM_WHERE=$where SIM_PHASES=$phases SIM_BOOT_WAIT=$bootwait"
    [ -n "$warp" ] && cmdline="$cmdline SIM_WARP=$warp"
    [ "${8:-}" = noreset ] && cmdline="$cmdline SIM_NORESET=1"
    local mem=8G cpu=host
    local -a extra=()
    if [ -n "$windisk" ]; then
        # The disk of the Windows which was installed on the runner, as a disk
        # of this machine.  What is written to it goes to a temporary file,
        # the image stays as it is.
        cmdline="$cmdline SIM_GUEST=win"
        mem=10G
        extra=(-drive "file=$windisk,format=vdi,if=none,id=wdisk,snapshot=on" -device "virtio-blk-pci,drive=wdisk")
        # The processor of the runner tells of an invariant time stamp
        # counter, QEMU does not pass that on by itself, and VirtualBox tells
        # its guest what its host tells it.  See the top of this file for
        # what Windows does without.  The machine for the Linux guest stays as
        # it has been.
        cpu=host,invtsc=on
    fi
    # The device behind the root port: an NVMe controller with its drive, or
    # two of them as the functions of one device.
    local -a dev=(-drive "file=$WORK/nvme.img,if=none,id=nvm0,format=raw"
                  -device "nvme,drive=nvm0,serial=SIM0001,bus=rp1")
    # The root port.  For the device of two functions it is one which does
    # not tell whether its link is up (a port for 2.5 GT/s has not got to).
    # The port QEMU emulates by default says that it tells and then never
    # does, and a kernel which resets the bus behind such a port waits for
    # the link in vain and calls the reset failed, having done it (run 265:
    # "Data Link Layer Link Active not set"); a port of a real host tells.
    # The reset of the bus is what the phase mfbusreset is about.
    local port="pcie-root-port,id=rp1,chassis=1,slot=1"
    if [ "$funcs" = 2 ]; then
        port="$port,x-speed=2_5,x-width=1"
        cmdline="$cmdline SIM_MD5B=$MD5B"
        dev=(-drive "file=$WORK/nvme.img,if=none,id=nvm0,format=raw"
             -device "nvme,drive=nvm0,serial=SIM0001,bus=rp1,addr=00.0,multifunction=on"
             -drive "file=$WORK/nvme2.img,if=none,id=nvm1,format=raw"
             -device "nvme,drive=nvm1,serial=SIM0002,bus=rp1,addr=00.1")
    fi
    t0=$(date +%s)
    sudo timeout -k 30 "$limit" qemu-system-x86_64 \
        -machine q35,kernel-irqchip=split -accel kvm -cpu "$cpu" -smp 4 -m "$mem" \
        -nodefaults -display none -monitor none -serial stdio -no-reboot \
        -kernel "$VMLINUZ" -initrd "$WORK/l1.cpio.gz" -append "$cmdline" \
        -device intel-iommu,intremap=on \
        -virtfs "local,path=/,mount_tag=hostroot,security_model=none,readonly=on,multidevs=remap" \
        -virtfs "local,path=$OUT,mount_tag=simout,security_model=none" \
        -device "$port" \
        "${dev[@]}" \
        "${extra[@]}" \
        < /dev/null 2>&1 | tr -d '\r' | cut -c1-400 | tee "$OUT/$where-console.log"
    rc=${PIPESTATUS[0]}
    echo "$where ended with $rc after $(( $(date +%s) - t0 )) s"
    if ! grep -q "^$where/setup: KVM is there" "$STAGES"; then
        stage "$where: the virtual machine standing in for the host ran its script" FAILED
    fi
}

if has_part simhost; then
    note "Part 2: VirtualBox in a virtual machine with an IOMMU and a PCIe device"
    if part_simhost_prepare; then
        # 12 minutes in run 224, 26 in run 231 with two VMs out of six slow.
        run_simhost simhost plain,passthrough,hotplug,startempty,hostremove,intx 600 5 5400

        if grep -q 'written by the guest' "$WORK/nvme.img"; then
            stage "what the guest wrote is on the drive of the simulated host" ok \
                  "$(grep -a -o 'written by the guest, round [0-9]*' "$WORK/nvme.img" | tail -n 1)"
        elif grep -q '^simhost[^/]*/passthrough: guest: writes to the drive: ok' "$STAGES"; then
            stage "what the guest wrote is on the drive of the simulated host" FAILED
        fi
    fi
fi

if has_part mfhost; then
    note "Part 2b: the same with a device of two functions"
    if part_simhost_prepare; then
        # The first drive is the one of part 2 and has what that part wrote to
        # it; what tells of this part is on the second drive.
        run_simhost mfhost mfpassthrough,mfhotplug,mfstartempty,mfhostremove,mfbusreset 600 5 4200 "" 2

        if grep -q 'written by the guest to the second drive' "$WORK/nvme2.img"; then
            stage "what the guest wrote to the second drive is on that drive of the simulated host" ok \
                  "$(grep -a -o 'written by the guest to the second drive, round [0-9]*' "$WORK/nvme2.img" | tail -n 1)"
        elif grep -q '^mfhost/mfpassthrough: guest: writes to both drives: ok' "$STAGES"; then
            stage "what the guest wrote to the second drive is on that drive of the simulated host" FAILED
        fi
    fi
fi


# Windows as the guest.  The image is the evaluation version of Windows Server
# which Microsoft offers for download to everybody, for trying it out for 180
# days; it is fetched for the run and gone with the runner.  The VM it is
# installed into has no network adapter.
get_windows_image()
{
    local url=${WIN_ISO_URL:-'https://go.microsoft.com/fwlink/?linkid=2293312&clcid=0x409&culture=en-us&country=us'}
    local t0 final
    t0=$(date +%s)
    final=$(curl -f -L -sS --retry 3 --retry-delay 10 --connect-timeout 30 --max-time 900 \
                 -o "$WORK/win.iso" -w '%{url_effective}' "$url") || { echo "the download failed"; return 1; }
    echo "downloaded in $(( $(date +%s) - t0 )) s: $(basename "${final%%\?*}"), $(stat -c %s "$WORK/win.iso") bytes"
    # A DVD image (ISO 9660 or UDF), and large enough to be the one meant.
    case "$(dd if="$WORK/win.iso" bs=1 skip=32769 count=5 status=none 2> /dev/null)" in
        CD001|BEA01) ;;
        *) echo "this is not a DVD image"; return 1 ;;
    esac
    [ "$(stat -c %s "$WORK/win.iso")" -gt 3000000000 ] || { echo "the image is too small"; return 1; }
    echo "sha256: $(sha256sum "$WORK/win.iso" | cut -d' ' -f1)"
}

# The second DVD: the answers for setup, and what runs in the installed Windows.
make_unattend_image()
{
    local d=$WORK/unattend
    rm -rf "$d" "$WORK/unattend.iso"
    mkdir -p "$d/w1"
    cp "$SIM/win/Autounattend.xml" "$d/Autounattend.xml" || return 1
    cp "$SIM/win/report.ps1" "$d/w1/report.ps1" || return 1
    # A batch file is to have the line ends of DOS.
    sed -e 's/\r$//' -e 's/$/\r/' "$SIM/win/first.cmd" > "$d/w1/first.cmd" || return 1
    command -v genisoimage > /dev/null || sudo apt-get install -y -q --no-install-recommends genisoimage > "$WORK/apt-genisoimage.log" 2>&1
    genisoimage -quiet -J -r -V SIMW1 -o "$WORK/unattend.iso" "$d" || return 1
    ls -l "$WORK/unattend.iso"
    # A third one, which is put into the drive later: it tells the script in
    # the guest to shut Windows down.
    rm -rf "$WORK/cmd" "$WORK/cmd-shutdown.iso"
    mkdir -p "$WORK/cmd/simcmd"
    echo shutdown > "$WORK/cmd/simcmd/shutdown"
    genisoimage -quiet -J -r -V SIMCMD -o "$WORK/cmd-shutdown.iso" "$WORK/cmd" || return 1
}

LINUX_STAGES=$(wc -l < "$STAGES")

if has_part win; then
    note "Part 3: Windows as the guest, on the runner"
    df -h "$WORK" | sed -n 2p
    if ! get_windows_image; then
        stage "the evaluation image of Windows Server is downloaded" FAILED
    elif ! make_unattend_image; then
        stage "the DVD with the answers for setup is made" FAILED
    else
        stage "the evaluation image of Windows Server is downloaded" ok
        mkdir -p "$OUT/win"
        # The VirtualBox of part 1 has to be gone, a new one starts with another
        # directory for its settings.
        i=0
        while pgrep -x VBoxSVC > /dev/null && [ "$i" -lt 30 ]; do
            sleep 1
            i=$((i + 1))
        done
        sudo env VBOX_BIN="$BIN" SIM_WORK=/tmp/sim-win SIM_OUT="$OUT/win" SIM_STAGES="$STAGES" SIM_WHERE=runner \
                 WIN_ISO="$WORK/win.iso" WIN_UNATTEND_ISO="$WORK/unattend.iso" WIN_CMD_ISO="$WORK/cmd-shutdown.iso" \
            timeout -k 30 4500 bash "$SIM/win-phases.sh" runner
        echo "part 3 ended with $?"
    fi
fi

# Windows where the device is: the Windows which part 3 installed, in a
# virtual machine as in part 2 or 2b.
part_winhost() # <name in the results> <functions of the device: 1 or 2> <what this is called in the results>
{
    local where=$1 funcs=$2 what=$3
    if ! grep -q '^runner/win-install: Windows is installed.*: ok' "$STAGES" || ! sudo test -s /tmp/sim-win/win.vdi; then
        stage "$what" skipped "there is no installed Windows"
    elif    ! grep -q AuthenticAMD /proc/cpuinfo && [ "${WIN_NESTED_ALWAYS:-no}" != yes ] \
         && ! grep -q -s -E '^VBOX_WITH_KVM +:= +1' "$WS/AutoConfig.kmk"; then
        # Runs 228 and 229 had runners with Intel processors.  Windows has its
        # tick from the HPET of VirtualBox there, some 800 times a second, and
        # where an exit takes a millisecond it did not get through a start in
        # 50 minutes.  On the AMD processor of run 226 Windows had its tick
        # from the local APIC, which the kernel emulates, and a start took
        # half a million exits.  Which processor a runner has is not ours to
        # choose.
        # That is the KVM backend of the development line.  A build with the
        # one of Cyberus Technology (the third condition) gives Windows the
        # Hyper-V interface, with which it does not need the HPET for its
        # time, so there it is tried on every processor.
        stage "$what" skipped "no AMD processor here, and Windows does not get through a start at this depth on others"
    elif part_simhost_prepare; then
        sudo ls -l /tmp/sim-win/win.vdi
        # Half a million exits of a millisecond each make some ten minutes for
        # the start, and that is not all of it: in run 231, with the clock out
        # of the way, Windows was still not at its disk driver after 40
        # minutes, with few exits.  It gets an hour, less memory to go through
        # and that memory in huge pages (l1.sh), and tells every three minutes
        # how far it is.
        if [ "$funcs" = 2 ]; then
            run_simhost "$where" win-hotplug 3600 "" 7500 /tmp/sim-win/win.vdi "$funcs" noreset
        else
            run_simhost "$where" win-hotplug 3600 "" 7500 /tmp/sim-win/win.vdi "$funcs"
        fi
    fi
}

if has_part winhost; then
    note "Part 4: Windows with the drive in the slot, in a virtual machine with an IOMMU and a PCIe device"
    part_winhost winhost 1 "Windows with the drive in the slot"
fi

if has_part mfwinhost; then
    note "Part 4b: the same with a device of two functions"
    # The disk of Windows is the one part 4 had: what that part wrote to it
    # went to a temporary file.
    part_winhost mfwinhost 2 "Windows with the device of two functions in the slot"
fi

sudo chmod -R a+rX "$OUT" 2> /dev/null
finish
