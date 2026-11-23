#!/bin/sh
# Everything of the first tests which takes root, one after the other: gives
# the device to vfio-pci, goes through the checklist as root, and runs the
# tests.  Each step is a script of this directory, which can be read and run
# by itself, and writes its own result to hw-out/.
#
#   sudo sh hw/linux/tests.sh
#
# A test which fails does not keep the next one from running; without the
# device bound to vfio-pci none runs.  The device stays with vfio-pci
# afterwards: sudo sh hw/linux/bind.sh --undo gives it back to the host, and
# so does starting the machine anew.
#
# For a graphics card the second test runs twice, the second time with a VM
# which has no graphics adapter of its own and is given the legacy VGA ranges
# of the card.  HW_SKIP_VGA=yes leaves that out.
here=$(cd "$(dirname "$0")" && pwd)
. "$here/common.sh"

if [ "$(id -u)" != 0 ]; then
    echo "Run it as root: sudo sh hw/linux/tests.sh"
    exit 1
fi
if ! pick_device; then
    echo "There is no device for the tests: no graphics card but the one the firmware shows its screen on. Name a function with HW_DEV=<address>."
    exit 1
fi
# The scripts pick the device again; they are to pick this one.
export HW_DEV="$DEV"

step() # <what it is> <script> [<argument>]
{
    echo
    echo "======== $1: hw/linux/$2${3:+ $3}"
    sh "$here/$2" ${3:+"$3"}
}

if ! step "the device goes to vfio-pci" bind.sh --do; then
    echo
    echo "The device is not bound to vfio-pci, so no test was run."
    exit 1
fi
step "the checklist, as root" prereq.sh

failed=
step "the first test" test-a.sh || failed="$failed test-a"
step "the second test" test-b.sh || failed="$failed test-b"
display=no
for f in $FUNCS; do
    case "$(first "/sys/bus/pci/devices/$f/class")" in 0x03*) display=yes ;; esac
done
if [ "$display" = yes ] && [ "${HW_SKIP_VGA:-no}" != yes ]; then
    echo
    echo "======== the second test, with the legacy VGA ranges of the card given to the VM: HW_VGA=yes hw/linux/test-b.sh"
    HW_VGA=yes sh "$here/test-b.sh" || failed="$failed test-b-vga"
fi

give_back
echo
echo "======== done"
ls "$out"/*.txt 2> /dev/null | sed -e 's#^.*/#result: hw-out/#'
if [ -z "$failed" ]; then
    echo "Every test went through its checks. Publish the results with: sh hw/publish.sh"
    exit 0
fi
echo "Not every check passed in:$failed. The results tell which; publish them all the same with: sh hw/publish.sh"
exit 1
