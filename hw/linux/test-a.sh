#!/bin/sh
# The first test with a real device, and one which needs no operating system
# in the VM: a VM with the device in the hot-plug capable slot starts, the
# device is taken out of the slot and put in again, twice, the VM is reset
# and powered off.  What is looked at is what VirtualBox tells of the device
# and what the log of the VM has about it.  The guest is the firmware of the
# VM, which waits for something to start from and finds nothing.
#
#   sudo sh hw/linux/test-a.sh        writes hw-out/test-a.txt
#
# It runs as root: the VM has to reach the device and to lock its memory.
# Before it: hw/linux/fetch.sh, and sudo sh hw/linux/bind.sh --do.  The device
# is the one hw/linux/prereq.sh names, HW_DEV=<address> names another.
#
# It creates a VM called "hwtest" with settings of its own under hw-work/ and
# removes the one it finds there from the run before.  Nothing else of the
# machine is touched; the device stays bound to vfio-pci.
. "$(dirname "$0")/common.sh"
RESULT="$out/test-a.txt"
: > "$RESULT"

test_begin test-a.sh
test_vm --memory 2048 --boot1 none --boot2 none --boot3 none --boot4 none
test_start

sleep 20
if [ "$(vmstate)" = running ]; then
    check "the VM runs 20 seconds later" ok
else
    check "the VM runs 20 seconds later" FAILED "it is in state '$(vmstate)'"
    test_end
fi
s=$(pcistate)
case "$s" in
    hotplug,present,inuse) check "VirtualBox tells that the device is in the slot and in use" ok ;;
    *)                     check "VirtualBox tells that the device is in the slot and in use" FAILED "it tells '${s:-nothing}'" ;;
esac
log_check "after the start"

# Out of the slot and in again, twice.  Nothing in the VM answers when it is
# asked to let go of the device, so the device is taken.
for round in 1 2; do
    if VBM_TIMEOUT=120 run controlvm "$VM" unplugpci "$DEV" --timeout=3000 --force && wait_pcistate 'hotplug,absent,unused' 60; then
        check "round $round: the device is taken out of the slot" ok
    else
        check "round $round: the device is taken out of the slot" FAILED "VirtualBox tells '$(pcistate)'"
    fi
    for f in $FUNCS; do
        [ "$(driver_of "$f")" = vfio-pci ] || check "round $round: $f is still with vfio-pci while it is out" FAILED "driver $(driver_of "$f")"
    done
    if VBM_TIMEOUT=300 run controlvm "$VM" plugpci "$DEV" && wait_pcistate 'hotplug,present,*' 60; then
        check "round $round: the device is put into the slot" ok "VirtualBox tells '$(pcistate)'"
    else
        check "round $round: the device is put into the slot" FAILED "VirtualBox tells '$(pcistate)'"
    fi
    if [ "$(vmstate)" != running ]; then
        check "round $round: the VM runs on" FAILED "it is in state '$(vmstate)'"
        test_end
    fi
done

# A reset of the VM is a reset of the device.
if run controlvm "$VM" reset && sleep 10 && [ "$(vmstate)" = running ]; then
    check "the VM is reset and runs on" ok "VirtualBox tells '$(pcistate)'"
else
    check "the VM is reset and runs on" FAILED "it is in state '$(vmstate)'"
fi
log_check "at the end"

if run controlvm "$VM" poweroff && wait_vmstate poweroff 60; then
    check "the VM is powered off" ok
else
    check "the VM is powered off" FAILED "it is in state '$(vmstate)'"
fi
left=
for f in $FUNCS; do
    [ "$(driver_of "$f")" = vfio-pci ] || left="$left $f"
done
if [ -z "$left" ]; then
    check "the device is with vfio-pci afterwards, as before" ok
else
    check "the device is with vfio-pci afterwards, as before" FAILED "not:$left"
fi
test_end
