#!/bin/sh
# Tells which device the tests are going to be about and goes through the
# checklist of INSTALL.md for it (tools/pci-passthrough-check.sh).  It only
# reads.  Run it as the user, and once more as root after hw/linux/bind.sh:
# the tests run as root.
#
#   sh hw/linux/prereq.sh                       writes hw-out/prereq.txt
#   HW_DEV=0000:0a:00.0 sh hw/linux/prereq.sh   for another device than the
#                                               one it picks
. "$(dirname "$0")/common.sh"
RESULT="$out/prereq.txt"
: > "$RESULT"

if ! pick_device; then
    stage "a device for the VM is there" FAILED "no graphics card but the one the firmware shows its screen on; name a function with HW_DEV=<address>"
    exit 1
fi
stage "the device for the VM" ok "$FUNCS"
for f in $FUNCS; do
    d="/sys/bus/pci/devices/$f"
    boot=
    [ "$(first "$d/boot_vga")" = 1 ] && boot=", THE FIRMWARE SHOWS ITS SCREEN ON IT"
    echo "function $f: class $(first "$d/class"), id $(first "$d/vendor" | sed 's/^0x//'):$(first "$d/device" | sed 's/^0x//'), driver $(driver_of "$f")$boot" >> "$RESULT"
done

echo "---- the checklist, run as $( [ "$(id -u)" = 0 ] && echo root || echo "a user" )" >> "$RESULT"
if sh "$top/tools/pci-passthrough-check.sh" $FUNCS >> "$RESULT" 2>&1; then
    stage "the checklist finds nothing missing" ok
else
    stage "the checklist finds nothing missing" FAILED "see the lines which start with MISSING"
fi

privacy_check "$RESULT"
give_back
cat "$RESULT"
echo
echo "Written to $RESULT."
