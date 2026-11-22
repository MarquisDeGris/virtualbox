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

if [ "$(id -u)" != 0 ]; then
    echo "Run it as root: sudo sh hw/linux/test-a.sh"
    exit 1
fi
BIN=${HW_VBOX_BIN:-$work/bin}
if [ ! -x "$BIN/VBoxManage" ]; then
    stage "the build is there" FAILED "run hw/linux/fetch.sh first"
    exit 1
fi
if ! pick_device; then
    stage "a device for the VM is there" FAILED "name a function with HW_DEV=<address>"
    exit 1
fi
stage "the device" ok "$FUNCS"
unbound=
for f in $FUNCS; do
    d="/sys/bus/pci/devices/$f"
    echo "function $f: class $(first "$d/class"), id $(first "$d/vendor" | sed 's/^0x//'):$(first "$d/device" | sed 's/^0x//'), driver $(driver_of "$f")" >> "$RESULT"
    [ "$(driver_of "$f")" = vfio-pci ] || unbound="$unbound $f"
done
if [ -n "$unbound" ]; then
    stage "every function of the device is bound to vfio-pci" FAILED "not:$unbound; run sudo sh hw/linux/bind.sh --do"
    exit 1
fi
stage "every function of the device is bound to vfio-pci" ok
if [ ! -c /dev/kvm ] || [ ! -c /dev/iommu ]; then
    stage "/dev/kvm and /dev/iommu are there" FAILED "see hw-out/prereq.txt"
    exit 1
fi

export USER=root LOGNAME=root HOME="$work/home" VBOX_USER_HOME="$work/vbox"
mkdir -p "$HOME" "$VBOX_USER_HOME"
ulimit -l unlimited 2> /dev/null
VM=hwtest
failed=0

vbm() { timeout -k 10 "${VBM_TIMEOUT:-120}" "$BIN/VBoxManage" "$@"; }
# Runs VBoxManage, shows the command and what it says, returns its exit code.
run()
{
    local rc
    echo "+ VBoxManage $*"
    vbm "$@" > "$work/last.out" 2>&1
    rc=$?
    short 300 < "$work/last.out" | sed 's/^/    /'
    return "$rc"
}
check() # <what> <ok|FAILED> [<note>]
{
    [ "$2" = FAILED ] && failed=$((failed + 1))
    stage "$@"
}
vmstate() { vbm showvminfo "$VM" --machinereadable 2> /dev/null | sed -n 's/^VMState="\(.*\)"$/\1/p'; }
# What VirtualBox tells of the device in the slot: "<hotplug|fixed>,<present|absent>,<inuse|unused>".
pcistate() { vbm showvminfo "$VM" --machinereadable 2> /dev/null | sed -n 's/^AttachedHostPCIState=[^,]*,\(.*\)$/\1/p' | sed -n '1p'; }
wait_pcistate() # <pattern for case> <seconds>
{
    local i=0
    while [ "$i" -lt "$2" ]; do
        case "$(pcistate)" in $1) return 0 ;; esac
        sleep 1
        i=$((i + 1))
    done
    return 1
}
wait_vmstate() # <state> <seconds>
{
    local i=0
    while [ "$i" -lt "$2" ]; do
        [ "$(vmstate)" = "$1" ] && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}
# The lines the log of the VM has from the passthrough device, without their
# time stamps, and how many of them tell of something which did not work.
log_lines() { grep -a 'VFIO#' "$LOG" 2> /dev/null | sed -e 's/^[0-9:.]* *//'; }
log_trouble() { log_lines | grep -c -i -E 'fail|could not|error|refus|not supported|timed out'; }
finish()
{
    case "$(vmstate)" in
        running|paused|stuck|gurumeditation|starting)
            vbm controlvm "$VM" poweroff > /dev/null 2>&1
            wait_vmstate poweroff 60 || pkill -KILL -f "VBoxHeadless.*--comment $VM " 2> /dev/null
            ;;
    esac
    echo "---- what the log of the VM has from the device (the first 60 lines):" >> "$RESULT"
    log_lines | short 200 | sed -n '1,60p' >> "$RESULT"
    echo "checks which failed: $failed" | tee -a "$RESULT"
    pkill -x VBoxSVC 2> /dev/null
    privacy_check "$RESULT"
    give_back
    echo
    echo "Written to $RESULT. Publish it with: sh hw/publish.sh"
    [ "$failed" = 0 ]
    exit $?
}

# A VM of its own, made anew, with all it has under the work directory.
vbm controlvm "$VM" poweroff > /dev/null 2>&1
vbm unregistervm "$VM" --delete > /dev/null 2>&1
run setproperty machinefolder "$work/vms"
if ! run createvm --name "$VM" --ostype Linux_64 --register; then
    check "the VM is created" FAILED "$(short 160 < "$work/last.out" | sed -n '1p')"
    finish
fi
run modifyvm "$VM" --memory 2048 --cpus 2 --ioapic on --chipset ich9 --firmware efi \
    --vm-execution-engine native-api --nic1 none --audio-enabled off \
    --graphicscontroller vboxvga --vram 16 --uart1 0x3f8 4 --uartmode1 file "$work/serial.log" \
    --boot1 none --boot2 none --boot3 none --boot4 none
attached=yes
for f in $FUNCS; do
    run modifyvm "$VM" --pci-attach "$f@0000:01:00.${f##*.}" || attached=no
done
run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1 || attached=no
if [ "$attached" = yes ]; then
    check "the VM is created, with the device in the hot-plug capable slot" ok
else
    check "the VM is created, with the device in the hot-plug capable slot" FAILED "$(short 160 < "$work/last.out" | sed -n '1p')"
    finish
fi
LOG="$(dirname "$(vbm showvminfo "$VM" --machinereadable | sed -n 's/^CfgFile="\(.*\)"$/\1/p')")/Logs/VBox.log"

# It starts.
if VBM_TIMEOUT=300 run startvm "$VM" --type headless && wait_vmstate running 30; then
    check "the VM starts with the device" ok
else
    check "the VM starts with the device" FAILED "$(grep -a -i 'error' "$work/last.out" | sed -n '1p' | short 160)"
    finish
fi
sleep 20
if [ "$(vmstate)" = running ]; then
    check "the VM runs 20 seconds later" ok
else
    check "the VM runs 20 seconds later" FAILED "it is in state '$(vmstate)'"
    finish
fi
s=$(pcistate)
case "$s" in
    hotplug,present,inuse) check "VirtualBox tells that the device is in the slot and in use" ok ;;
    *)                     check "VirtualBox tells that the device is in the slot and in use" FAILED "it tells '${s:-nothing}'" ;;
esac
n=$(log_trouble)
if [ "${n:-0}" = 0 ]; then
    check "the log has no line of the device about something which did not work, after the start" ok
else
    check "the log has no line of the device about something which did not work, after the start" FAILED "$n line(s), see below"
fi

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
        finish
    fi
done

# A reset of the VM is a reset of the device.
if run controlvm "$VM" reset && sleep 10 && [ "$(vmstate)" = running ]; then
    check "the VM is reset and runs on" ok "VirtualBox tells '$(pcistate)'"
else
    check "the VM is reset and runs on" FAILED "it is in state '$(vmstate)'"
fi
n=$(log_trouble)
if [ "${n:-0}" = 0 ]; then
    check "the log has no line of the device about something which did not work, at the end" ok
else
    check "the log has no line of the device about something which did not work, at the end" FAILED "$n line(s), see below"
fi

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
finish
