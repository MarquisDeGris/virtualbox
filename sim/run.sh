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
#   4c. The same with a device whose second function is a High Definition
#      Audio controller (SIM_PARTS has "hdawinhost"): a drive and the sound
#      of a card as one device, which is how a graphics card comes.  Windows
#      has its own driver for such a controller and runs it with a line
#      interrupt unless it is told otherwise; the part tells which interrupt
#      it has by what the host has for it, and has Windows play sound.
#
#   2c. A device with four gigabytes of memory (SIM_PARTS has "barhost"):
#      what a graphics card has and the drive does not.  The device is one
#      whose memory is nothing but memory, kept in a file on the runner.  The
#      scripts for a machine with real hardware (hw/linux/) run with it: the
#      VM, the slot, and a guest which tells where it has that memory and
#      writes to its first and its last bytes, which are then looked for in
#      the file.  The device has a ROM as well, a file of the runner too: the
#      guest is to read the bytes of that file.
#
#   2d. A graphics card, as far as the simulation has one (SIM_PARTS has
#      "vgahost"): the standard VGA adapter of QEMU behind the root port.
#      It has video memory, registers, a ROM with a BIOS in it and a driver
#      in the kernel which makes a card with a connector of it, which is
#      what the second script for real hardware looks for with a graphics
#      card.  Nobody sees its picture.
#
#   2e. An error of the device (SIM_PARTS has "errhost"): the kernel of the
#      machine which stands in for the host is told that the device has
#      reported an error it cannot correct, by the means the kernel has for
#      trying that out, while a guest uses the drive in the hot-plug capable
#      slot.  The error is made up; what the kernel, the driver which has the
#      device and VirtualBox do about it is what they do about a real one.
#      The device goes out of the VM, the VM runs on, and the device can be
#      put in again.  Once with an error the kernel does not take for fatal
#      and once with one it does, after which it resets the bus.
#
#   2f. The same with the device of two functions of part 2b (SIM_PARTS has
#      "mferrhost"): the kernel tells the drivers of both functions of the
#      error, and the device goes out of the VM as the one device it is.
#
#   2g. The device is unplugged from the host and plugged in again (SIM_PARTS
#      has "plughost"): the port the drive is behind is told so from outside
#      the machine, and the hot-plug of the machine's own kernel takes the
#      drive out, which is gone from the machine afterwards, and takes it in
#      again as a new device.  What an enclosure on a cable does to a host.
#
#   2h. The same with the device of two functions (SIM_PARTS has
#      "mfplughost"): both functions go when the device is unplugged, and
#      both are there again when it is plugged in.
#
# Which parts run is told by SIM_PARTS, a list with commas: runner, simhost,
# mfhost, barhost, vgahost, errhost, mferrhost, plughost, mfplughost, win,
# winhost, mfwinhost, hdawinhost.
# Without it: runner,simhost.
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
    if has_part barhost && ! grep -q '^barhost/setup: the first script' "$STAGES"; then
        missing=1
    fi
    if has_part vgahost && ! grep -q '^vgahost/setup: the first script' "$STAGES"; then
        missing=1
    fi
    if has_part errhost && ! grep -q '^errhost/hosterror: ' "$STAGES"; then
        missing=1
    fi
    if has_part mferrhost && ! grep -q '^mferrhost/hosterror: ' "$STAGES"; then
        missing=1
    fi
    if has_part plughost && ! grep -q '^plughost/hostplug: ' "$STAGES"; then
        missing=1
    fi
    if has_part mfplughost && ! grep -q '^mfplughost/hostplug: ' "$STAGES"; then
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
        if    has_part hdawinhost && ! grep -q '^hdawinhost/win-hotplug: Windows starts with the drive.*: ok' "$STAGES" \
           && ! grep -q '^runner/setup: Windows with a device whose second function is an audio controller: skipped (no AMD processor' "$STAGES"; then
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
    mkdir -p "$WORK/kdeb" "$WORK/kroot" "$WS/kdeb-cache"
    # The packages as an earlier run fetched them, where the workflow has
    # kept them (it keeps them by the version of the kernel), or fetched now
    # and left for the workflow to keep.  Fetching gets 25 minutes.
    local have=yes t0
    for p in $pkgs; do
        ls "$WS/kdeb-cache/${p}_"*.deb > /dev/null 2>&1 || have=no
    done
    if [ "$have" = yes ]; then
        cp "$WS"/kdeb-cache/*.deb "$WORK/kdeb/" || return 1
        echo "the packages of the kernel are those an earlier run fetched"
    else
        t0=$(date +%s)
        ( cd "$WORK/kdeb" && timeout 1500 apt-get download $pkgs ) > "$WORK/kernel-download.log" 2>&1 \
            || { tail -n 5 "$WORK/kernel-download.log"; return 1; }
        echo "the packages of the kernel were fetched in $(( $(date +%s) - t0 )) s"
        cp "$WORK"/kdeb/*.deb "$WS/kdeb-cache/" 2> /dev/null
    fi
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
# For the phase "gfx3d", in a run with a build which has 3D acceleration
# (SIM_3D is yes): the same guest with the driver for the graphics adapter of
# the VM.
if [ "${SIM_3D:-no}" = yes ]; then
    make_initrd guest3d "$SIM/guest-init" nvme virtio_net vmwgfx
fi

# A disk for the guest: a GPT with an EFI system partition which has the
# kernel as a unified kernel image where the firmware looks for one, and, should
# that not work, a script for the shell of the firmware.
make_guest_disk() # <name> <kernel command line> [<name of the initramfs, "guest" if not given>]
{
    local name=$1 cmdline=$2 esp=$WORK/$1-esp.img initrd=$WORK/${3:-guest}.cpio.gz
    rm -f "$esp" "$WORK/$name.raw" "$WORK/$name-BOOTX64.EFI"
    truncate -s 200M "$esp"
    mkfs.vfat -F 32 -n SIM "$esp" > /dev/null
    mmd -i "$esp" ::/EFI ::/EFI/BOOT
    mcopy -i "$esp" "$VMLINUZ" ::/vmlinuz.efi
    mcopy -i "$esp" "$initrd" ::/initrd.img
    printf 'echo -off\r\nfs0:\r\n\\vmlinuz.efi %s initrd=\\initrd.img\r\n' "$cmdline" > "$WORK/$name-startup.nsh"
    mcopy -i "$esp" "$WORK/$name-startup.nsh" ::/startup.nsh
    if [ -n "$UKIFY" ] && "$UKIFY" build --linux="$VMLINUZ" --initrd="$initrd" --cmdline="$cmdline" \
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
# A guest which looks at the graphics adapter of its VM and writes to its
# screen (the phase "gfx3d").  The serial port stays the console the script
# of the guest talks on.
GUEST_RAW_3D=
if [ "${SIM_3D:-no}" = yes ]; then
    make_guest_disk guest-3d "console=tty0 console=ttyS0,115200 quiet loglevel=3 mitigations=off rdinit=/init panic=30 sim3d=1" guest3d
    GUEST_RAW_3D=$WORK/guest-3d.raw
    echo "what this machine has to render with:"
    vulkaninfo --summary 2> /dev/null | grep -E 'deviceName|deviceType|driverName|apiVersion' | sed 's/^/    /' | head -n 12
fi

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
runner_phases="plain emptyslot nat import"
# A tree which is not about the passthrough device, told by its not having
# the testcase of it, has no hot-plug capable slot to look at.
[ -e "$WS/src/VBox/Devices/testcase/tstDevPciVfio.cpp" ] || runner_phases="plain nat import"
[ -n "$GUEST_RAW_3D" ] && runner_phases="$runner_phases gfx3d"
# A tree whose VBoxManage can ask the firmware of a VM for its setup.
if grep -q -s 'firmwaresetup' "$WS/src/VBox/Frontends/VBoxManage/VBoxManageModifyNvram.cpp"; then
    runner_phases="$runner_phases fwsetup"
fi
# A build with the sanitizers (SIM_SANITIZED is yes): what they find in a
# program of VirtualBox goes to files, one for each process, and the program
# ends where the address sanitizer finds something.  Memory which is not
# freed at the end of a program is not what this is after.
san_asan=
san_ubsan=
boot_wait=240
if [ "${SIM_SANITIZED:-no}" = yes ]; then
    mkdir -p "$OUT/sanitizers"
    chmod 777 "$OUT/sanitizers"
    san_asan="detect_leaks=0:log_path=$OUT/sanitizers/asan"
    san_ubsan="print_stacktrace=1:log_path=$OUT/sanitizers/ubsan"
    # One thing the address sanitizer finds in every program of VirtualBox
    # which has an event queue (run 305): an event of the queue is freed as
    # another type than it was allocated as, which ends the program there.
    # A tree which has that put right is told by its source; for one which
    # has not, this kind of finding is switched off, so that the programs get
    # as far as there is something else to find.
    if grep -q 'delete pMyEvent;' "$WS/src/VBox/Main/glue/NativeEventQueue.cpp" 2> /dev/null; then
        echo "the tree frees the events of its queue as what they are"
    else
        echo "the tree frees the events of its queue as another type: the address sanitizer is told to let that pass"
        san_asan="$san_asan:new_delete_type_mismatch=0"
    fi
    # Everything takes two or three times as long with them.
    boot_wait=600
fi
sudo env VBOX_BIN="$BIN" SIM_WORK=/tmp/sim-runner SIM_OUT="$OUT/runner" SIM_STAGES="$STAGES" \
         GUEST_RAW="$WORK/guest.raw" GUEST_RAW_3D="$GUEST_RAW_3D" SIM_WHERE=runner SIM_BOOT_WAIT=$boot_wait SIM_HTTP_MD5="$HTTP_MD5" \
         ${san_asan:+ASAN_OPTIONS="$san_asan"} ${san_ubsan:+UBSAN_OPTIONS="$san_ubsan"} ${san_asan:+SIM_PATIENCE=3} \
         ${san_asan:+LD_LIBRARY_PATH="$BIN"} \
    timeout -k 30 3000 bash "$SIM/vbox-phases.sh" $runner_phases
echo "part 1 ended with $?"
# What the sanitizers wrote down so far, after a part of the simulation.
sanitizers_say() # <which programs: text for the name of the check>
{
    local na nu f what
    [ "${SIM_SANITIZED:-no}" = yes ] || return 0
    what="the address sanitizer has nothing to say of the programs of VirtualBox which ran $1"
    # What the sanitizers wrote down.  The address sanitizer writes when it
    # has found an access to memory which is not the program's to access; the
    # other one writes about arithmetic and conversions which the language
    # leaves undefined, of which a tree of this size has some.
    # Both sanitizers write to the files of either name (run 305), so the
    # files are told apart by what is in them.
    sudo chmod -R a+rX "$OUT/sanitizers" 2> /dev/null
    na=$(cat "$OUT"/sanitizers/* 2> /dev/null | grep -a -c 'ERROR: AddressSanitizer')
    nu=$(cat "$OUT"/sanitizers/* 2> /dev/null | grep -a -c 'runtime error')
    echo "files of the sanitizers: $(ls "$OUT/sanitizers" 2> /dev/null | wc -l)"
    if [ "$na" = 0 ]; then
        stage "$what" ok
    else
        stage "$what" FAILED "$na report(s), by kind and place below, and the first of them"
        cat "$OUT"/sanitizers/* | grep -a '^SUMMARY: AddressSanitizer' | cut -c1-200 | sort | uniq -c | sort -rn | head -n 20
        f=$(grep -a -l 'ERROR: AddressSanitizer' "$OUT"/sanitizers/* | head -n 1)
        echo "---- $f"
        head -n 40 "$f" | cut -c1-220
    fi
    echo "reports of undefined behaviour: $nu"
    cat "$OUT"/sanitizers/* 2> /dev/null | grep -a 'runtime error' | sed -e 's/^.*\/src\/VBox\//src\/VBox\//' | cut -c1-200 | sort | uniq -c | sort -rn | head -n 40
    # What was found is kept apart from what the next part finds.
    mkdir -p "$OUT/sanitizers-seen"
    sudo sh -c "mv '$OUT'/sanitizers/* '$OUT/sanitizers-seen/'" 2> /dev/null
    return 0
}
sanitizers_say "on the runner, with the Linux guest"
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
# Makes a ROM for a device of the simulation: 64 KiB, with the header and the
# PCI data structure a kernel wants to see before it takes a ROM for one, and
# bytes of chance after them.  Its code type is Open Firmware, which no
# firmware here takes for something to run.
make_rom() # <file> <vendor ID> <device ID>, the IDs as four hexadecimal digits
{
    local f=$1 v=$2 d=$3
    head -c 65536 /dev/urandom > "$f"
    # The signature, and at 0x18 where the PCI data structure is: at 0x20.
    printf '\x55\xaa' | dd of="$f" bs=1 seek=0 conv=notrunc status=none
    printf '\x20\x00' | dd of="$f" bs=1 seek=24 conv=notrunc status=none
    # "PCIR", the IDs, no list of devices, 24 bytes of structure in revision
    # 0, the class (memory controller), the image with 128 blocks of 512
    # bytes, revision 0 of the code, code type 1, the last image.
    printf "PCIR\\x${v:2:2}\\x${v:0:2}\\x${d:2:2}\\x${d:0:2}\\x00\\x00\\x18\\x00\\x00\\x00\\x00\\x05\\x80\\x00\\x00\\x00\\x01\\x80\\x00\\x00" \
        | dd of="$f" bs=1 seek=32 conv=notrunc status=none
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
    # A build with the sanitizers: the script in the machine is told to set
    # them up, with the one kind of finding let pass or not as on the runner,
    # and everything gets twice the time.
    if [ -n "$san_asan" ]; then
        case "$san_asan" in
            *new_delete_type_mismatch=0*) cmdline="$cmdline SIM_SAN=mask" ;;
            *)                            cmdline="$cmdline SIM_SAN=all" ;;
        esac
        limit=$(( limit * 2 ))
    fi
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
    # The part about an error of the device: after an error it takes for
    # fatal the kernel resets the bus behind the port and then waits for the
    # link.  With the port as it is by default that wait is in vain, as with
    # the device of two functions below, and the kernel calls its recovery
    # failed (run 327, where all else went as it should).  This port lets it
    # get through.
    [ "$where" = errhost ] && port="$port,x-speed=2_5,x-width=1"
    if [ "$where" = plughost ]; then
        # The part in which the device is unplugged from this machine and
        # plugged in again.  The drive is given a name the device can be
        # taken away from, and the image is not the device's: a drive which
        # is given as the others are goes when its device goes.  And the
        # kernel of the machine is to run the slot of the port itself, as a
        # kernel does for a port to which an enclosure on a cable connects:
        # by default the firmware of this kind of machine keeps that.
        dev=(-blockdev "driver=file,filename=$WORK/nvme.img,node-name=nvmf"
             -blockdev "driver=raw,file=nvmf,node-name=nvm0"
             -device "nvme,drive=nvm0,serial=SIM0001,bus=rp1,id=dev0")
        extra+=(-global "ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off")
    fi
    if [ "$funcs" = 2 ]; then
        port="$port,x-speed=2_5,x-width=1"
        cmdline="$cmdline SIM_MD5B=$MD5B"
        dev=(-drive "file=$WORK/nvme.img,if=none,id=nvm0,format=raw"
             -device "nvme,drive=nvm0,serial=SIM0001,bus=rp1,addr=00.0,multifunction=on"
             -drive "file=$WORK/nvme2.img,if=none,id=nvm1,format=raw"
             -device "nvme,drive=nvm1,serial=SIM0002,bus=rp1,addr=00.1")
    fi
    if [ "$funcs" = bar ]; then
        # A device whose memory is four gigabytes of a file, and which has no
        # reset of its own.  Its class is "memory controller"; the script in
        # the machine is told to take that for the device, and to run the
        # scripts for real hardware instead of the phases with the drive.
        port="$port,x-speed=2_5,x-width=1"
        cmdline="$cmdline SIM_DEVCLASS=0x050000 SIM_HWTEST=bar"
        # The first script runs once more there, with the build as it was
        # fetched, if it was.
        [ -n "$HWBIN" ] && cmdline="$cmdline SIM_HWBIN=$HWBIN"
        rm -f "$WORK/bar.img"
        # It has a ROM as well, as a graphics card has one with its firmware
        # in it: the guest is to read the bytes which are in the file.
        make_rom "$WORK/bar.rom" 1af4 1110
        cmdline="$cmdline SIM_ROM=$(stat -c %s "$WORK/bar.rom"):$(md5sum < "$WORK/bar.rom" | cut -d' ' -f1)"
        dev=(-object "memory-backend-file,id=shm0,share=on,mem-path=$WORK/bar.img,size=4G"
             -device "ivshmem-plain,memdev=shm0,bus=rp1,romfile=$WORK/bar.rom")
    fi
    if [ "$funcs" = vga ]; then
        # A graphics card: the standard VGA adapter of QEMU, with 16 MB of
        # video memory, its registers in memory, a ROM with its BIOS in it,
        # and no reset of its own.  It is the only graphics adapter of this
        # machine, so it is the one its firmware starts with.  The scripts for
        # real hardware run with it, as with the device of part 2c.
        port="$port,x-speed=2_5,x-width=1"
        cmdline="$cmdline SIM_DEVCLASS=0x030000 SIM_HWTEST=vga"
        dev=(-device "VGA,bus=rp1")
    fi
    if [ "$funcs" = hda ]; then
        # A drive and a High Definition Audio controller as the two functions
        # of one device.  The sound goes nowhere, in the time sound takes.
        port="$port,x-speed=2_5,x-width=1"
        dev=(-drive "file=$WORK/nvme.img,if=none,id=nvm0,format=raw"
             -device "nvme,drive=nvm0,serial=SIM0001,bus=rp1,addr=00.0,multifunction=on"
             -audiodev "none,id=snd0"
             -device "intel-hda,id=hda0,bus=rp1,addr=00.1"
             -device "hda-output,bus=hda0.0,audiodev=snd0")
    fi
    if [ "$where" = mfplughost ]; then
        # The same as for the part plughost above, with the device of two
        # functions: each drive is a name of its own which outlives its
        # controller, and both controllers have a name to be told by.
        dev=(-blockdev "driver=file,filename=$WORK/nvme.img,node-name=nvmf"
             -blockdev "driver=raw,file=nvmf,node-name=nvm0"
             -blockdev "driver=file,filename=$WORK/nvme2.img,node-name=nvmg"
             -blockdev "driver=raw,file=nvmg,node-name=nvm1"
             -device "nvme,drive=nvm0,serial=SIM0001,bus=rp1,addr=00.0,multifunction=on,id=dev0"
             -device "nvme,drive=nvm1,serial=SIM0002,bus=rp1,addr=00.1,id=dev1")
        extra+=(-global "ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off")
    fi
    # A part whose phases ask for something the machine cannot do to itself
    # has somebody on the runner who does it, see host-agent.sh.  The machine
    # gets a socket for that which nothing inside it can reach.
    local qmp=
    local -a mon=()
    case ",$phases," in
        *,hosterror,*|*,hostplug,*|*,win-hotplug,*)
            qmp=$WORK/$where-qmp.sock
            sudo mkdir -p "$OUT/$where"
            sudo rm -f "$qmp" "$OUT/$where/host-request" "$OUT/$where/host-reply" "$OUT/$where/host-agent.stop"
            mon=(-qmp "unix:$qmp,server=on,wait=off")
            cmdline="$cmdline SIM_HOSTAGENT=1"
            sudo bash "$SIM/host-agent.sh" "$OUT/$where" "$qmp" "$limit" "$funcs" &
            ;;
    esac
    t0=$(date +%s)
    sudo timeout -k 30 "$limit" qemu-system-x86_64 \
        -machine q35,kernel-irqchip=split -accel kvm -cpu "$cpu" -smp 4 -m "$mem" \
        -nodefaults -display none -monitor none "${mon[@]}" -serial stdio -no-reboot \
        -kernel "$VMLINUZ" -initrd "$WORK/l1.cpio.gz" -append "$cmdline" \
        -device intel-iommu,intremap=on \
        -virtfs "local,path=/,mount_tag=hostroot,security_model=none,readonly=on,multidevs=remap" \
        -virtfs "local,path=$OUT,mount_tag=simout,security_model=none" \
        -device "$port" \
        "${dev[@]}" \
        "${extra[@]}" \
        < /dev/null 2>&1 | tr -d '\r' | cut -c1-400 | tee "$OUT/$where-console.log"
    rc=${PIPESTATUS[0]}
    [ -n "$qmp" ] && sudo touch "$OUT/$where/host-agent.stop"
    echo "$where ended with $rc after $(( $(date +%s) - t0 )) s"
    if ! grep -q "^$where/setup: KVM is there" "$STAGES"; then
        stage "$where: the virtual machine standing in for the host ran its script" FAILED
    fi
    sanitizers_say "in the machine standing in for a host ($where)"
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

# The scripts for a machine with real hardware which need no device, on the
# runner itself: the one which fetches the build, the inventory, and the one
# which publishes results.  That one pushes to a repository which is made
# here for it and is gone with the runner.  HWBIN is where the fetched build
# is, for the part which runs a test with it.
HWBIN=
hw_runner_scripts()
{
    local top="$WORK/hwtop" remote="$WORK/hwremote.git" seed="$WORK/hwseed" rc what
    note "The scripts for a machine with real hardware which need no device"
    mkdir -p "$OUT/hw-runner"

    what="the script which fetches the build gets the one which is published, and its VBoxManage runs"
    # The build which is fetched is not one with the sanitizers, whatever the
    # build of this run is: it is not to find the libraries of this run.
    env -u LD_LIBRARY_PATH -u ASAN_OPTIONS -u UBSAN_OPTIONS HW_WORK="$WORK/hw-fetch" HW_OUT="$OUT/hw-runner" \
        sh "$SIM/../hw/linux/fetch.sh" 2>&1 | cut -c1-220 | sed 's/^/    /'
    if grep -q '^VBoxManage of the build runs: ok' "$OUT/hw-runner/fetch.txt" 2> /dev/null; then
        stage "$what" ok "$(sed -n 's/^the build is fetched and is the one listed: ok (\(.*\))$/\1/p' "$OUT/hw-runner/fetch.txt")"
        HWBIN="$WORK/hw-fetch/bin"
    else
        stage "$what" FAILED "$(grep -m 1 'FAILED' "$OUT/hw-runner/fetch.txt" 2> /dev/null | cut -c1-160)"
    fi

    # A copy of the scripts as a clone of its own, as they are on such a
    # machine, with a repository of this run as its origin, which has the
    # branch the results go to.
    rm -rf "$top" "$remote" "$seed"
    mkdir -p "$top" "$seed"
    cp -r "$SIM/../hw" "$SIM/../tools" "$top/"
    git init -q --bare "$remote"
    git init -q "$seed"
    echo "results" > "$seed/README.md"
    git -C "$seed" add README.md
    git -C "$seed" -c user.name=popolupo -c user.email=4758069+MarquisDeGris@users.noreply.github.com \
        commit -q -m "the branch for results"
    git -C "$seed" push -q "$remote" HEAD:refs/heads/hw-results
    git init -q "$top"
    git -C "$top" remote add origin "file://$remote"

    what="the inventory script writes its inventory"
    sh "$top/hw/run.sh" > "$OUT/hw-runner/run.log" 2>&1
    rc=$?
    if [ "$rc" = 0 ] && [ -s "$top/hw-out/inventory.txt" ]; then
        stage "$what" ok "$(wc -l < "$top/hw-out/inventory.txt") lines"
        cp "$top/hw-out/inventory.txt" "$OUT/hw-runner/inventory.txt"
    else
        stage "$what" FAILED "$(tail -n 1 "$OUT/hw-runner/run.log" | cut -c1-160)"
    fi

    what="the script which publishes results puts them on the branch for them, as popolupo"
    sh "$top/hw/publish.sh" > "$OUT/hw-runner/publish.log" 2>&1
    rc=$?
    cut -c1-220 "$OUT/hw-runner/publish.log" | sed 's/^/    /'
    if    [ "$rc" = 0 ] \
       && git --git-dir="$remote" ls-tree -r --name-only hw-results 2> /dev/null | grep -q '^results/[0-9-]*-pc1/inventory.txt$' \
       && [ "$(git --git-dir="$remote" log -1 --format=%an hw-results 2> /dev/null)" = popolupo ]; then
        stage "$what" ok "$(git --git-dir="$remote" ls-tree -r --name-only hw-results | grep '^results/' | tr '\n' ' ')"
    else
        stage "$what" FAILED "$(tail -n 1 "$OUT/hw-runner/publish.log" | cut -c1-160)"
    fi
}
if has_part barhost || has_part vgahost; then
    hw_runner_scripts
fi

if has_part barhost; then
    note "Part 2c: a device with four gigabytes of memory"
    if part_simhost_prepare; then
        run_simhost barhost none 600 5 3000 "" bar
        # What the guest wrote to the first and to the last four bytes of the
        # memory of the device, in the file which is that memory.
        if [ -s "$WORK/bar.img" ]; then
            first=$(od -A n -t x4 -N 4 "$WORK/bar.img" | tr -d ' ')
            last=$(tail -c 4 "$WORK/bar.img" | od -A n -t x4 | tr -d ' ')
            if [ "$first" = 48574241 ] && [ "$last" = 52454e44 ]; then
                stage "what the guest wrote to the first and to the last bytes of the memory of the device is there" ok "$(stat -c %s "$WORK/bar.img") bytes"
            else
                stage "what the guest wrote to the first and to the last bytes of the memory of the device is there" FAILED "first $first, last $last"
            fi
        else
            stage "what the guest wrote to the first and to the last bytes of the memory of the device is there" FAILED "there is no file"
        fi
        sudo rm -f "$WORK/bar.img"
    fi
fi

if has_part vgahost; then
    note "Part 2d: a graphics card, as far as the simulation has one"
    if part_simhost_prepare; then
        run_simhost vgahost none 600 5 3000 "" vga
    fi
fi

if has_part errhost; then
    # A machine of its own for this: where its kernel does not get over what
    # it is told, nothing else is lost.
    note "Part 2e: an error of the device, as the kernel of the host tells of it"
    if part_simhost_prepare; then
        run_simhost errhost hosterror 600 5 2400
    fi
fi

if has_part plughost; then
    note "Part 2g: the device is unplugged from the host and plugged in again"
    if part_simhost_prepare; then
        run_simhost plughost hostplug 600 5 2400
    fi
fi

if has_part mfplughost; then
    note "Part 2h: the same with a device of two functions"
    if part_simhost_prepare; then
        run_simhost mfplughost hostplug 600 5 2400 "" 2
    fi
fi

if has_part mferrhost; then
    note "Part 2f: the same with a device of two functions"
    if part_simhost_prepare; then
        run_simhost mferrhost hosterror 600 5 2400 "" 2
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
    # And one which tells it to run the NVMe controllers behind the port with
    # a line interrupt (INTx) instead of MSI-X, see win_intx in win-phases.sh.
    rm -rf "$WORK/cmdi" "$WORK/cmd-intx.iso"
    mkdir -p "$WORK/cmdi/simcmd"
    echo intx > "$WORK/cmdi/simcmd/intx"
    # With another name and another size than the one for shutting down.  In
    # run 278 Windows did not see the second one after it had had the first,
    # and two media which are alike in all a drive tells of them are one
    # medium to it; that is the likeliest reason.
    head -c 300000 /dev/zero > "$WORK/cmdi/simcmd/padding"
    genisoimage -quiet -J -r -V SIMINTX -o "$WORK/cmd-intx.iso" "$WORK/cmdi" || return 1
    # And one which has it play three seconds of sound, see win_audio there.
    rm -rf "$WORK/cmda" "$WORK/cmd-audio.iso"
    mkdir -p "$WORK/cmda/simcmd"
    echo audio > "$WORK/cmda/simcmd/audio"
    head -c 700000 /dev/zero > "$WORK/cmda/simcmd/padding"
    genisoimage -quiet -J -r -V SIMAUDIO -o "$WORK/cmd-audio.iso" "$WORK/cmda" || return 1
    # And one which has it install the Guest Additions from the other DVD
    # drive, see phase_gfx3d there.
    rm -rf "$WORK/cmdg" "$WORK/cmd-additions.iso"
    mkdir -p "$WORK/cmdg/simcmd"
    echo additions > "$WORK/cmdg/simcmd/additions"
    head -c 1100000 /dev/zero > "$WORK/cmdg/simcmd/padding"
    genisoimage -quiet -J -r -V SIMGA -o "$WORK/cmd-additions.iso" "$WORK/cmdg" || return 1
}

# The Guest Additions as Oracle publishes them, for a run with a build which
# has 3D acceleration: the graphics driver for Windows which goes with it is
# part of them.  Those of the version the build says it is, or, where there
# are none of that version, the newest of the stable line.
get_guest_additions()
{
    local v url
    rm -f "$WORK/ga.iso"
    v=$("$BIN/VBoxManage" --version 2> /dev/null | tail -n 1 | sed -e 's/[^0-9.].*$//')
    for v in "$v" "$(curl -f -s --max-time 30 https://download.virtualbox.org/virtualbox/LATEST-STABLE.TXT | tr -dc '0-9.')"; do
        [ -n "$v" ] || continue
        url="https://download.virtualbox.org/virtualbox/$v/VBoxGuestAdditions_$v.iso"
        if curl -f -L -sS --retry 3 --retry-delay 10 --connect-timeout 30 --max-time 600 -o "$WORK/ga.iso" "$url" \
           && [ "$(stat -c %s "$WORK/ga.iso")" -gt 20000000 ]; then
            echo "the Guest Additions $v: $(stat -c %s "$WORK/ga.iso") bytes, sha256 $(sha256sum "$WORK/ga.iso" | cut -d' ' -f1)"
            return 0
        fi
        rm -f "$WORK/ga.iso"
    done
    echo "no Guest Additions could be fetched"
    return 1
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
        # With a build which has 3D acceleration Windows gets the Guest
        # Additions and is started with it, see phase_gfx3d in win-phases.sh.
        win_ga=
        if [ "${SIM_3D:-no}" = yes ]; then
            if get_guest_additions; then
                win_ga=$WORK/ga.iso
            else
                stage "the Guest Additions for Windows are fetched" FAILED
            fi
        fi
        sudo env VBOX_BIN="$BIN" SIM_WORK=/tmp/sim-win SIM_OUT="$OUT/win" SIM_STAGES="$STAGES" SIM_WHERE=runner \
                 WIN_ISO="$WORK/win.iso" WIN_UNATTEND_ISO="$WORK/unattend.iso" WIN_CMD_ISO="$WORK/cmd-shutdown.iso" \
                 WIN_GA_ISO="$win_ga" WIN_CMD_ADDITIONS_ISO="$WORK/cmd-additions.iso" \
                 ${san_asan:+ASAN_OPTIONS="$san_asan"} ${san_ubsan:+UBSAN_OPTIONS="$san_ubsan"} ${san_asan:+LD_LIBRARY_PATH="$BIN"} \
                 ${san_asan:+WIN_INSTALL_WAIT=9000} ${san_asan:+WIN_START_WAIT=2700} \
            timeout -k 30 15000 bash "$SIM/win-phases.sh" runner
        echo "part 3 ended with $?"
        sanitizers_say "on the runner, with Windows"
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
        if [ "$funcs" = 2 ] || [ "$funcs" = hda ]; then
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

if has_part hdawinhost; then
    note "Part 4c: the same with a device whose second function is an audio controller"
    part_winhost hdawinhost hda "Windows with a device whose second function is an audio controller"
fi

sudo chmod -R a+rX "$OUT" 2> /dev/null
finish
