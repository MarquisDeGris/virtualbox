#!/bin/sh
# The second test with a real device: a small Linux guest which has the
# drivers the host has for the device.  It is put together here from the
# kernel the host runs, the modules of that kernel for the device and their
# firmware, and a script which tells on the serial port of the VM what the
# guest makes of the device: which driver has it, its interrupts, and for a
# graphics card its connectors and whether a monitor is on one.  Nobody has
# to look at a screen for it, though a monitor on the card shows the text
# console of the guest if all is well.
#
#   sudo sh hw/linux/test-b.sh        writes hw-out/test-b.txt
#
# It runs as root, like hw/linux/test-a.sh, and after it.  It needs busybox as
# a static program, cpio, gzip, zstd, mkfs.vfat, mtools, sgdisk and ukify, and tells
# what to install if one is missing.  HW_KERNEL names another kernel image
# than /boot/vmlinuz-<the running one>, HW_GUEST_MB another size of the memory
# of the VM than 4096 MB.  HW_EXPECT_DRIVER=no is for a device no driver of
# the kernel takes although a module names it.
. "$(dirname "$0")/common.sh"
RESULT="$out/test-b.txt"
: > "$RESULT"

test_begin test-b.sh

# ---- The guest.
missing=
for t in busybox cpio gzip zstd mkfs.vfat mmd mcopy sgdisk modprobe modinfo; do
    have "$t" || missing="$missing $t"
done
if [ -n "$missing" ]; then
    check "what it takes to put the guest together is there" FAILED "missing:$missing; as root: apt-get install busybox-static cpio zstd dosfstools mtools gdisk kmod"
    test_end
fi
# A unified kernel image is what the firmware of the VM starts by itself.
# The other way, a script for the shell of the firmware, did not start the
# guest where it was tried (run 289 of the workflow).
ukify=$(command -v ukify || ls /usr/lib/systemd/ukify 2> /dev/null)
if [ -z "$ukify" ] || [ ! -e /usr/lib/systemd/boot/efi/linuxx64.efi.stub ]; then
    check "what it takes to put the guest together is there" FAILED "ukify and the EFI stub of systemd are missing; as root: apt-get install systemd-ukify systemd-boot-efi"
    test_end
fi
bbx=$(command -v busybox)
if ldd "$bbx" 2> /dev/null | grep -q '=>'; then
    check "what it takes to put the guest together is there" FAILED "busybox is not a static program; as root: apt-get install busybox-static"
    test_end
fi
kver=$(uname -r)
kernel=${HW_KERNEL:-/boot/vmlinuz-$kver}
if [ ! -r "$kernel" ]; then
    check "the kernel for the guest is there" FAILED "no readable kernel image for $kver"
    test_end
fi

# The modules the kernel has for the functions of the device, by what the
# functions say they are.
mods=
for f in $FUNCS; do
    for m in $(modprobe -R "$(cat "/sys/bus/pci/devices/$f/modalias")" 2> /dev/null); do
        case " $mods " in
            *" $m "*) ;;
            *) mods="$mods $m" ;;
        esac
    done
done

root="$work/guest-root"
rm -rf "$root"
mkdir -p "$root/bin" "$root/dev" "$root/proc" "$root/sys" "$root/tmp" "$root/modules" "$root/lib/firmware"
cp "$bbx" "$root/bin/busybox"
cp "$here/guest-init" "$root/init"
chmod 755 "$root/init" "$root/bin/busybox"
: > "$root/modules.list"
nfw=0
for m in $mods; do
    modprobe --show-depends "$m" 2> /dev/null | while read -r kind file rest; do
        [ "$kind" = insmod ] || continue
        base=$(basename "$file")
        base=${base%.zst}
        base=${base%.xz}
        base=${base%.gz}
        [ -e "$root/modules/$base" ] && continue
        case "$file" in
            *.zst) zstd -q -d -c "$file" > "$root/modules/$base" ;;
            *.xz)  xz -d -c "$file" > "$root/modules/$base" ;;
            *.gz)  gzip -d -c "$file" > "$root/modules/$base" ;;
            *)     cp "$file" "$root/modules/$base" ;;
        esac
        echo "$base" >> "$root/modules.list"
        # The firmware the module names, as far as the host has it, compressed
        # or not: the kernel of the guest takes it as the one of the host does.
        for fw in $(modinfo -F firmware "$file" 2> /dev/null); do
            for src in "/lib/firmware/$fw" "/lib/firmware/$fw.zst" "/lib/firmware/$fw.xz"; do
                [ -e "$src" ] || continue
                mkdir -p "$root/lib/firmware/$(dirname "$fw")"
                cp -L "$src" "$root/lib/firmware/$(dirname "$fw")/"
            done
        done
    done
done
nfw=$(find "$root/lib/firmware" -type f | wc -l)
( cd "$root" && find . | cpio -o -H newc -R 0:0 --quiet | gzip -1 ) > "$work/guest.cpio.gz"
mb=$(( ( $(wc -c < "$work/guest.cpio.gz") + $(wc -c < "$kernel") ) / 1048576 + 1 ))

# A disk with an EFI system partition which has the kernel and the initramfs
# as one unified kernel image, where the firmware looks for something to
# start.  HW_CMDLINE is added to the command line of the kernel.
cmdline="console=tty0 console=ttyS0,115200 loglevel=4 rdinit=/init panic=30${HW_CMDLINE:+ $HW_CMDLINE}"
esp="$work/guest-esp.img"
espmb=$(( mb + 64 ))
[ "$espmb" -ge 200 ] || espmb=200
# A disk of that name from a run before is none of this run.
vbm closemedium disk "$work/guest.vdi" --delete > /dev/null 2>&1
rm -f "$esp" "$work/guest.raw" "$work/guest.vdi"
truncate -s "${espmb}M" "$esp"
built=yes
mkfs.vfat -F 32 -n HWGUEST "$esp" > /dev/null 2>&1 || built=no
rm -f "$work/BOOTX64.EFI"
if "$ukify" build --linux="$kernel" --initrd="$work/guest.cpio.gz" --cmdline="$cmdline" --output="$work/BOOTX64.EFI" > "$work/ukify.log" 2>&1; then
    mmd -i "$esp" ::/EFI ::/EFI/BOOT || built=no
    mcopy -i "$esp" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI || built=no
else
    tail -n 5 "$work/ukify.log" | short 200
    built=no
fi
truncate -s "$(( espmb + 4 ))M" "$work/guest.raw"
sgdisk -n "1:2048:+${espmb}M" -t 1:ef00 "$work/guest.raw" > /dev/null 2>&1 || built=no
dd if="$esp" of="$work/guest.raw" bs=1M seek=1 conv=notrunc status=none || built=no
rm -f "$esp"
if [ "$built" = yes ] && run convertfromraw "$work/guest.raw" "$work/guest.vdi" --format VDI; then
    check "the guest is put together" ok "kernel $kver, modules for the device:${mods:- none, which is so when the driver is part of the kernel or there is none}, $(wc -l < "$root/modules.list") module files, $nfw firmware files, $mb MB"
else
    check "the guest is put together" FAILED "see what was said above"
    test_end
fi
rm -f "$work/guest.raw"

# ---- The VM with it.
test_vm --memory "${HW_GUEST_MB:-4096}" --boot1 disk --boot2 none --boot3 none --boot4 none
if    ! run storagectl "$VM" --name sata --add sata --controller IntelAhci --portcount 1 \
   || ! run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$work/guest.vdi"; then
    check "the VM has the disk of the guest" FAILED "$(short 160 < "$work/last.out" | sed -n '1p')"
    test_end
fi
test_start

# What the guest has said so far.  A line of it may have what the firmware
# left on the line in front of it.
said() { tr -d '\r' < "$work/serial.log" 2> /dev/null | sed -n 's/^.*\(HW: .*\)$/\1/p'; }
# The last of what came on the serial port, as text: for when the guest says
# nothing.
serial_tail()
{
    echo "---- the last of the serial port of the VM:" >> "$RESULT"
    tr -c '\11\12\40-\176' ' ' < "$work/serial.log" 2> /dev/null | tr -s ' ' | grep -v '^ *$' | tail -n 15 | short 200 >> "$RESULT"
}
wait_said() # <regex> <seconds>
{
    local i=0
    while [ "$i" -lt "$2" ]; do
        said | grep -q -E "$1" && return 0
        [ "$(vmstate)" = running ] || return 1
        sleep 1
        i=$((i + 1))
    done
    return 1
}

if wait_said '^HW: UP ' "${HW_BOOT_WAIT:-240}"; then
    check "the guest starts" ok "$(said | sed -n 's/^HW: UP //p' | sed -n '1p')"
else
    check "the guest starts" FAILED "nothing from it on the serial port after ${HW_BOOT_WAIT:-240} seconds, the VM is in state '$(vmstate)'"
    serial_tail
    test_end
fi
if wait_said '^HW: READY' 300; then
    check "the guest has looked at the device" ok
else
    check "the guest has looked at the device" FAILED "it did not get done in 300 seconds"
fi

# Function by function: there with the IDs it has on the host, and a driver
# has it.
for f in $FUNCS; do
    g="0000:01:00.${f##*.}"
    line=$(said | grep -F "HW: PCI $g " | sed -n '1p')
    case "$line" in
        *" $(ids_of "$f") "*) check "guest: has function ${f##*.} of the device, with the IDs it has on the host" ok "$(ids_of "$f")" ;;
        *)                    check "guest: has function ${f##*.} of the device, with the IDs it has on the host" FAILED "${line:-the guest tells nothing of $g}" ;;
    esac
    drv=$(echo "$line" | sed -n 's/^.* driver \([^ ]*\) .*$/\1/p')
    if [ -n "$drv" ] && [ "$drv" != none ]; then
        check "guest: a driver has function ${f##*.}" ok "$(echo "$line" | sed -e 's/^.* driver //')"
    elif [ -z "$mods" ]; then
        check "guest: a driver has function ${f##*.}" skipped "the kernel has no module for it"
    elif [ "${HW_EXPECT_DRIVER:-yes}" = no ]; then
        check "guest: a driver has function ${f##*.}" skipped "none, and none is expected for this device; the modules which name it were:$mods"
    else
        check "guest: a driver has function ${f##*.}" FAILED "none; the modules were:$mods"
    fi
done

# The memory of the device: the guest has each BAR of a megabyte or more with
# the size it has on the host, and reaches its first and its last bytes,
# unless a driver of the guest has the BAR.  A card with gigabytes of memory
# which the firmware of the VM or the guest cannot place shows here.
for f in $FUNCS; do
    g="0000:01:00.${f##*.}"
    n=0
    while read -r start end flags; do
        bar=$n
        n=$((n + 1))
        [ "$bar" -le 5 ] || break
        [ "$((start))" != 0 ] || continue
        [ "$(( flags & 0x200 ))" != 0 ] || continue
        size=$(( end - start + 1 ))
        [ "$size" -ge 1048576 ] || continue
        line=$(said | grep -F "HW: BAR $g $bar " | sed -n '1p')
        what="guest: has BAR $bar of function ${f##*.} with its $(( size / 1048576 )) MB, to its last byte"
        case "$line" in
            '')
                check "$what" FAILED "the guest has not placed it"
                ;;
            *" size=$size first=busy "*)
                check "$what" ok "a driver of the guest has it"
                ;;
            *" size=$size "*failed*|*" size=$size "*refused*)
                check "$what" FAILED "$(echo "$line" | sed -e 's/^HW: BAR [^ ]* [0-9]* //')"
                ;;
            *" size=$size "*)
                if echo " ${HW_CMDLINE:-} " | grep -q " hwbarwrite=$(ids_of "$f") "; then
                    case "$line" in
                        *"first=0x48574241 last=0x52454E44"*) check "$what" ok "what the guest wrote to both ends it reads back" ;;
                        *) check "$what" FAILED "written and not read back: $(echo "$line" | sed -e 's/^.* first=/first=/')" ;;
                    esac
                else
                    check "$what" ok "$(echo "$line" | sed -e 's/^.* first=/first=/')"
                fi
                ;;
            *)
                check "$what" FAILED "the guest has it as: $(echo "$line" | sed -e 's/^HW: BAR [^ ]* [0-9]* //')"
                ;;
        esac
    done < "/sys/bus/pci/devices/$f/resource"
done

# A graphics card: the driver has made a card of it, and a monitor.
display=no
for f in $FUNCS; do
    case "$(first "/sys/bus/pci/devices/$f/class")" in 0x03*) display=yes ;; esac
done
if [ "$display" = yes ]; then
    # The connectors of the card which was passed through, not those of the
    # graphics adapter the VM has anyway.
    if said | grep -q '^HW: DRM card[^ ]* dev=0000:01:00\.'; then
        check "guest: the graphics driver has the card and its connectors" ok "$(said | grep -c '^HW: DRM card[^ ]* dev=0000:01:00\.') connectors"
        on=$(said | sed -n 's/^HW: DRM \([^ ]*\) dev=0000:01:00\.[0-7] connected .*$/\1/p' | tr '\n' ' ')
        if [ -n "$on" ]; then
            check "guest: a monitor is on a connector of the card" ok "$on"
        else
            check "guest: a monitor is on a connector of the card" skipped "none is told of; with a monitor on the card one would be"
        fi
    else
        check "guest: the graphics driver has the card and its connectors" FAILED "the guest tells of no card"
    fi
fi

# What the host has for the device meanwhile.
echo "---- the interrupts the host has for the device:" >> "$RESULT"
for f in $FUNCS; do
    grep -a -F "($f)" /proc/interrupts | grep -a 'vfio-' | tr -s ' ' | short 160 >> "$RESULT"
done

# It keeps running.
n0=$(said | grep -c '^HW: ALIVE')
sleep 15
n1=$(said | grep -c '^HW: ALIVE')
if [ "$n1" -gt "$n0" ] && [ "$(vmstate)" = running ]; then
    check "the guest keeps running with the device" ok
else
    check "the guest keeps running with the device" FAILED "the VM is in state '$(vmstate)', the guest said it was alive $n1 times"
fi
bad=$(said | grep -c -i -E '^HW: k: .*(nobody cared|Disabling IRQ|timed? ?out|fail|error)')
if [ "${bad:-0}" = 0 ]; then
    check "guest: its kernel tells of no trouble about the device" ok
else
    check "guest: its kernel tells of no trouble about the device" look "$bad line(s) which may or may not matter, among those below"
fi
log_check "with the guest running"

if run controlvm "$VM" poweroff && wait_vmstate poweroff 60; then
    check "the VM is powered off" ok
else
    check "the VM is powered off" FAILED "it is in state '$(vmstate)'"
fi

echo "---- what the guest said (at most 150 lines):" >> "$RESULT"
said | grep -v '^HW: ALIVE' | short 200 | sed -n '1,150p' >> "$RESULT"
test_end
