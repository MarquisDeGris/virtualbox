#!/bin/bash
# Part of the simulation, see run.sh: runs the VirtualBox of the build with a
# small Linux guest and goes through what the hardware test asks for, as far
# as that can be done without hardware.  Called with the names of the phases
# to run: on the runner itself, which has no IOMMU and so no device to pass
# through, and inside the virtual machine which stands in for a host with an
# IOMMU and a PCIe device.
#
# Every check ends in a line "STAGE <where>/<phase>: <what>: ok" or
# "... FAILED" on the output and in the file $SIM_STAGES.  A failed check does
# not end the script, the phases do not depend on each other.

set -u
: "${VBOX_BIN:?}" "${SIM_WORK:?}" "${SIM_OUT:?}" "${SIM_STAGES:?}" "${GUEST_RAW:?}"
WHERE=${SIM_WHERE:-host}
HOSTADDR=${SIM_HOSTADDR:-}
# Every function of the device of the host, the first being $HOSTADDR.  Where
# the device has two, the drive of the second has the checksum $SIM_MD5B.
HOSTFUNCS=${SIM_HOSTFUNCS:-$HOSTADDR}
EXPECT_MD5=${SIM_MD5:-none}
EXPECT_MD5B=${SIM_MD5B:-none}
# Whether the host has been told to forget how it resets the functions of the
# device by themselves, see forget_resets.
RESETS_FORGOTTEN=no
BOOT_WAIT=${SIM_BOOT_WAIT:-240}
# The waits for a line of the guest are this many times as long as written
# down where they are made.  They are written down for the runner.  In the
# virtual machine standing in for a host a guest was three to eight times as
# slow as usual in two VMs out of six (run 231), and a wait which ends early is
# a check which failed for the pace and not for what it is about.  A wait ends
# when the line is there, so patience costs time only where something is wrong.
PATIENCE=${SIM_PATIENCE:-1}

export USER=root LOGNAME=root HOME="$SIM_WORK/home"
export VBOX_USER_HOME="$SIM_WORK/vbox"
mkdir -p "$SIM_WORK" "$SIM_OUT" "$HOME" "$VBOX_USER_HOME"

SIMLIB=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

VM=sim
SERIAL=$SIM_WORK/serial.log
SERIAL_FROM=1
PHASE=setup
ATTACHED=
VMDIR=
VM_T0=$(date +%s)
# What the guest says on its serial port when it has booted and while it runs.
UP_RE='GUEST: UP'
ALIVE_RE='GUEST: ALIVE'
# Whether the tree has the two changes to Main which let a VM process end and
# have the service know of it, see main_stage.
MAIN_ENDS=${SIM_MAIN_ENDS:-yes}
# Whether the KVM backend of the tree enters the guest with the deadline of
# the next timer of the VM, see guest_pace.
TIMER_DEADLINE=${SIM_TIMER_DEADLINE:-yes}

# A host address nothing is at, and a layout as an NVMe drive has it, for the
# VM which is started without a device where there is none to be had.
FAKE_ADDR=0000:7f:00.0
FAKE_LAYOUT='ConfigSpaceSize=4096;Bar0Size=16384;Bar0IoPorts=0;Bar064Bit=1;Bar0Prefetchable=0;Bar0Mappable=1;RomSize=0;Vga=0;Msix=1;MsixBar=0;IntxVectors=1;MsiVectors=0;MsixVectors=65;MsixTableOffset=8192;MsixPbaOffset=12288'


stage() # <what> <ok|FAILED|skipped> [<note>]
{
    local line="$WHERE/$PHASE: $1: $2${3:+ ($3)}"
    echo "STAGE $line"
    echo "$line" >> "$SIM_STAGES"
}

vbm()
{
    timeout -k 10 "${VBM_TIMEOUT:-120}" "$VBOX_BIN/VBoxManage" "$@"
}

# Runs VBoxManage, shows the command and what it says, returns its exit code.
run()
{
    echo "+ VBoxManage $*"
    vbm "$@" 2>&1 | cut -c1-400 | sed 's/^/    /'
    return "${PIPESTATUS[0]}"
}

vmstate()
{
    vbm showvminfo "$VM" --machinereadable 2> /dev/null | sed -n 's/^VMState="\(.*\)"$/\1/p'
}

wait_state() # <state> <seconds>
{
    local i=0
    while [ "$i" -lt "$2" ]; do
        [ "$(vmstate)" = "$1" ] && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# The serial port of the guest goes to a file.  The checks look at what came
# after the last serial_mark.
serial_new()
{
    tail -n +"$SERIAL_FROM" "$SERIAL" 2> /dev/null | tr -d '\r'
}

serial_has() # <regex>
{
    serial_new | grep -a -q -E "$1"
}

serial_show() # <regex> [<lines>]
{
    serial_new | grep -a -E "$1" | head -n "${2:-20}" | cut -c1-220 | sed 's/^/    | /'
}

serial_mark()
{
    SERIAL_FROM=$(( $( { wc -l < "$SERIAL"; } 2> /dev/null || echo 0) + 1 ))
}

# Called now and then while a wait goes on and the VM runs.  Nothing here;
# win-phases.sh has its own, which looks whether the VM still gets anywhere.
stall_check()
{
    :
}

wait_serial() # <regex> <seconds> [<times as long, instead of $PATIENCE>]
{
    local i=0 st limit=$(( $2 * ${3:-$PATIENCE} ))
    while [ "$i" -lt "$limit" ]; do
        serial_has "$1" && return 0
        if [ $((i % 10)) = 9 ]; then
            st=$(vmstate)
            case "$st" in
                running) stall_check ;;
                paused|starting|restoring) ;;
                *) echo "    the VM is in state '$st', not waiting any longer"; return 1 ;;
            esac
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# What there is to look at when something did not go as it should.
diag()
{
    echo "    ---- the VM is in state '$(vmstate)'; the last lines on the serial port of the guest:"
    tail -n 25 "$SERIAL" 2> /dev/null | tr -d '\r' | cut -c1-220 | sed 's/^/    | /'
    local log="$VMDIR/Logs/VBox.log"
    if [ -n "$VMDIR" ] && [ -f "$log" ]; then
        echo "    ---- VBox.log about the device, the port and errors:"
        grep -a -E 'VFIO#|ich9pcibridge|[Hh]ot-plug|VERR_|VMSetError|Guru Med|Power.*fail|NEM:.*(fail|error)' "$log" \
            | cut -c1-300 | tail -n 40 | sed 's/^/    | /'
        echo "    ---- VBox.log, the last lines:"
        tail -n 12 "$log" | cut -c1-300 | sed 's/^/    | /'
    else
        echo "    ---- there is no VBox.log"
    fi
}

keep_logs() # [<suffix>]
{
    [ -n "$VMDIR" ] && cp -f "$VMDIR/Logs/VBox.log" "$SIM_OUT/VBox-$PHASE${1:-}.log" 2> /dev/null
    cp -f "$SERIAL" "$SIM_OUT/serial-$PHASE${1:-}.log" 2> /dev/null
    return 0
}

# Powers the VM off.  A VM which does not let itself be powered off has its
# process ended, so that the phases after this one have a VM to start: a VM
# left in the state "saving" took no command any more and cost the three
# phases after it (run 250, on the 7.1 line, where saving the state had been
# refused).
power_off()
{
    local i=0
    run controlvm "$VM" poweroff
    wait_state poweroff 60 && return 0
    echo "    the VM is in state '$(vmstate)' and not powered off, its process is ended"
    pkill -KILL -f "VBoxHeadless.*--comment $VM " 2> /dev/null
    while [ "$i" -lt 30 ]; do
        case "$(vmstate)" in
            poweroff|aborted) return 0 ;;
            aborted-saved|saved) run discardstate "$VM"; return 0 ;;
        esac
        sleep 1
        i=$((i + 1))
    done
    echo "    the VM is in state '$(vmstate)'"
    return 1
}

# Brings the VM back to being powered off without a device of the host.
reset_vm()
{
    local a
    case "$(vmstate)" in
        running|paused|stuck|gurumeditation|starting|restoring|saving)
            power_off
            ;;
        saved|aborted-saved)
            run discardstate "$VM"
            ;;
    esac
    settle
    for a in $ATTACHED; do
        run modifyvm "$VM" --pci-detach "$a"
    done
    ATTACHED=
    vbm setextradata "$VM" VBoxInternal2/HostPCIHotPlug > /dev/null 2>&1
    rm -f "$SERIAL"
    SERIAL_FROM=1
}

# How the VM and the machine it runs on are getting on, for telling a slow run
# from a broken one: the load and the CPU time of the machine (the last number
# of the cpu line is the time something else had its processors), the threads
# which used most of a processor, and how far the timers VirtualBox emulates
# are behind.  They fall behind when the thread of the CPU is late for them.
pace_note()
{
    local lag
    echo "    ---- pace: phase $PHASE, the VM was started $(( $(date +%s) - VM_T0 )) s ago; load $(cut -d' ' -f1-4 /proc/loadavg 2> /dev/null)"
    echo "    | $(grep -m 1 '^cpu ' /proc/stat 2> /dev/null | cut -d' ' -f1-10)"
    echo "    | $(grep -E '^(MemAvailable|AnonHugePages)' /proc/meminfo 2> /dev/null | tr -s ' ' | tr '\n' ' ')"
    ps -eLo pcpu,comm --sort=-pcpu 2> /dev/null | head -n 6 | sed 's/^/    | /'
    lag=$(VBM_TIMEOUT=60 vbm debugvm "$VM" statistics --pattern '/TM/VirtualSync/CurrentOffset' 2> /dev/null \
          | grep -a -o -E '(c|val)="[0-9]+"' | head -n 1 | tr -dc 0-9)
    [ -n "$lag" ] && echo "    | the timers of the VM are $(( lag / 1000000 )) ms behind"
    return 0
}

end_phase()
{
    case "$(vmstate)" in
        running|paused|stuck|gurumeditation)
            pace_note
            power_off
            ;;
        saving)
            power_off
            ;;
    esac
    keep_logs
}

start_vm()
{
    local rc
    rm -f "$SERIAL"
    SERIAL_FROM=1
    VM_T0=$(date +%s)
    echo "+ VBoxManage startvm $VM --type headless${STARTVM_MORE:+ $STARTVM_MORE}"
    VBM_TIMEOUT=300 vbm startvm "$VM" --type headless ${STARTVM_MORE:-} > "$SIM_WORK/startvm.out" 2>&1
    rc=$?
    cut -c1-400 "$SIM_WORK/startvm.out" | sed 's/^/    /'
    return $rc
}

boot() # <what>
{
    if ! start_vm; then
        stage "$1" FAILED "the VM does not start"
        diag
        return 1
    fi
    if ! wait_serial 'GUEST: READY' "$BOOT_WAIT" 1; then
        stage "$1" FAILED "the guest does not come up"
        diag
        return 1
    fi
    stage "$1" ok
}

pcistate()
{
    vbm showvminfo "$VM" --machinereadable 2> /dev/null | sed -n 's/^AttachedHostPCIState=[^,]*,\(.*\)$/\1/p' | head -n 1
}

check_pcistate() # <hotplug|fixed>,<present|absent>,<inuse|unused>
{
    local s
    s=$(pcistate)
    if [ "$s" = "$1" ]; then
        stage "showvminfo tells $1" ok
    else
        stage "showvminfo tells $1" FAILED "it tells '${s:-nothing}'"
    fi
}

# The registers of the slot as the guest left them, read from the outside
# (the port with the slot is the one in front of bus 1).  This does not depend
# on what the guest tells: a guest whose hot-plug driver runs the slot has
# switched on the hot-plug interrupt of the slot and the events it is for.
slot_registers_check()
{
    local line
    line=$(vbm debugvm "$VM" info pci verbose 2> /dev/null | perl "$SIMLIB/slotregs.pl" 1 | head -n 1)
    echo "    ${line:-nothing about a slot in what debugvm info pci verbose tells}" | cut -c1-400
    case "$line" in
        *" taken=1") stage "the registers of the slot show the guest running it (hot-plug interrupt and events on)" ok ;;
        *)           stage "the registers of the slot show the guest running it (hot-plug interrupt and events on)" FAILED ;;
    esac
}

# What the guest made of the hot-plug capable slot when it booted.
guest_slot_checks()
{
    serial_show 'GUEST: k: .*(_OSC|pciehp|PNP0A0)' 12
    if serial_has 'GUEST: k: .*_OSC: OS now controls \[[^]]*PCIeHotplug'; then
        stage "guest: the firmware hands PCIe hot-plug over to it" ok
    else
        stage "guest: the firmware hands PCIe hot-plug over to it" FAILED
    fi
    if serial_has 'GUEST: k: .*pciehp: Slot #[0-9]+ AttnBtn\+ PwrCtrl\+'; then
        stage "guest: its hot-plug driver has the slot" ok
    else
        stage "guest: its hot-plug driver has the slot" FAILED
    fi
    slot_registers_check
}

guest_reads() # <round> <what> [<seconds>]
{
    if wait_serial "GUEST: READ $1 md5=$EXPECT_MD5" "${3:-180}"; then
        stage "$2" ok
        return 0
    fi
    stage "$2" FAILED "last: $(serial_new | grep -a -E 'GUEST: (READ|NODRIVE|PRESENT|GONE)' | tail -n 1 | cut -c1-70)"
    diag
    return 1
}

guest_goes_on() # <what> [<seconds>]
{
    serial_mark
    if wait_serial "$ALIVE_RE" "${2:-45}"; then
        stage "$1" ok
        return 0
    fi
    stage "$1" FAILED
    diag
    return 1
}

# The power button of the VM is pressed.  The Linux guest does nothing about
# it, but its kernel counts the interrupts of the ACPI hardware, and the
# button is one: the count the guest tells with "ALIVE" has to go up.
power_button_check() # <what>
{
    local re='s/^GUEST: ALIVE [0-9]* sci=\([0-9][0-9]*\).*$/\1/p' before after
    before=$(tr -d '\r' < "$SERIAL" 2> /dev/null | sed -n "$re" | tail -n 1)
    run controlvm "$VM" acpipowerbutton
    sleep 2
    serial_mark
    wait_serial 'GUEST: ALIVE [0-9]+ sci=[0-9]' 45
    after=$(serial_new | sed -n "$re" | tail -n 1)
    if [ -n "$before" ] && [ -n "$after" ] && [ "$after" -gt "$before" ]; then
        stage "$1" ok "interrupts of the ACPI hardware: $before, then $after"
    else
        stage "$1" FAILED "interrupts of the ACPI hardware: ${before:-?}, then ${after:-?}"
    fi
}

# A device which goes in the normal way must not leave lines in the log of the
# VM which read like errors of the device.  The reset which the guest causes by
# switching the slot off used to, when it met a function already out of
# service or a host holding on to the device it waits for.
check_log_quiet()
{
    local log="$VMDIR/Logs/VBox.log" n
    local re='Failed to update command register|could not be reset|t be reset on its own|Failed to clear the command register'
    # Where the host has been told to forget how it resets a function by
    # itself (forget_resets), a function refusing its own reset is how it
    # has to go, and check_bus_resets counts on those lines.
    [ "$RESETS_FORGOTTEN" = yes ] && re='Failed to update command register|could not be reset|Failed to clear the command register'
    # What the device says once of a function the host has no reset for by
    # itself is no failure: devices with several functions are often so.
    local note='a reset is going to affect everything on its bus'
    n=$(grep -a -E "$re" "$log" 2> /dev/null | grep -a -c -v "$note")
    if [ "${n:-0}" = 0 ]; then
        stage "the log of the VM has no line about a failed reset or register write" ok
    else
        stage "the log of the VM has no line about a failed reset or register write" FAILED "$n"
        grep -a -E "$re" "$log" | grep -a -v "$note" | cut -c1-200 | sed 's/^/    | /'
    fi
    grep -a "$note" "$log" | cut -c1-200 | head -n 2 | sed 's/^/    | /'
    grep -a -E 'The device is not reset now|refuses a reset' "$log" | cut -c1-200 | head -n 2 | sed 's/^/    | /'
}

# A function of the device of the host and the driver it has to be bound to.
bind_one() # <address>
{
    local d=/sys/bus/pci/devices/$1 i=0
    while [ ! -e "$d" ] && [ "$i" -lt 10 ]; do
        sleep 1
        i=$((i + 1))
    done
    if [ ! -e "$d" ]; then
        echo "    the device $1 is not on the host"
        return 1
    fi
    local cur=
    [ -e "$d/driver" ] && cur=$(basename "$(readlink "$d/driver")")
    if [ "$cur" != vfio-pci ]; then
        [ -n "$cur" ] && echo "$1" > "$d/driver/unbind"
        echo vfio-pci > "$d/driver_override"
        echo "$1" > /sys/bus/pci/drivers_probe
        cur=
        [ -e "$d/driver" ] && cur=$(basename "$(readlink "$d/driver")")
    fi
    [ "$cur" = vfio-pci ]
}

# The device of the host, which is all of its functions.
bind_dev()
{
    local a rc=0
    for a in $HOSTFUNCS; do
        bind_one "$a" || rc=1
    done
    return $rc
}

# After a VM was powered off it takes a moment until its settings can be
# changed again.
settle()
{
    local i=0
    while [ "$i" -lt 30 ]; do
        vbm modifyvm "$VM" --description "simulation" > /dev/null 2>&1 && return 0
        sleep 1
        i=$((i + 1))
    done
    echo "    the settings of the VM stay locked"
    return 1
}

unbind_dev()
{
    local a
    for a in $HOSTFUNCS; do
        echo "$a" > /sys/bus/pci/drivers/vfio-pci/unbind
    done
}

# Saves the state of the VM, which takes the slot being empty, and loads it.
# STATE_LOADED tells afterwards whether it got as far as the VM running with
# the loaded state.
STATE_LOADED=no
saved_state_round()
{
    STATE_LOADED=no
    VBM_TIMEOUT=300 run controlvm "$VM" savestate
    if [ $? -ne 0 ] || ! wait_state saved 60; then
        stage "the state is saved while the slot is empty" FAILED "the VM is in state '$(vmstate)'"
        diag
        return 1
    fi
    stage "the state is saved while the slot is empty" ok
    keep_logs -saved
    if ! start_vm; then
        stage "the saved state is loaded and the guest goes on" FAILED "the VM does not start"
        diag
        return 1
    fi
    STATE_LOADED=yes
    if ! wait_serial "$ALIVE_RE" 120; then
        stage "the saved state is loaded and the guest goes on" FAILED "nothing from the guest"
        diag
        return 1
    fi
    if serial_has "$UP_RE"; then
        stage "the saved state is loaded and the guest goes on" FAILED "the guest booted anew"
        return 1
    fi
    stage "the saved state is loaded and the guest goes on" ok
}


setup()
{
    PHASE=setup
    echo "==== VirtualBox on $WHERE: $("$VBOX_BIN/VBoxManage" --version 2>&1 | tail -n 1)"
    ls -l /dev/kvm /dev/iommu 2>&1 | sed 's/^/    /'
    run setproperty machinefolder "$SIM_WORK/vms"
    if ! run convertfromraw "$GUEST_RAW" "$SIM_WORK/guest.vdi" --format VDI; then
        stage "VBoxManage and VBoxSVC work" FAILED
        tail -n 30 "$VBOX_USER_HOME/VBoxSVC.log" 2> /dev/null | cut -c1-300 | sed 's/^/    | /'
        return 1
    fi
    stage "VBoxManage and VBoxSVC work" ok
    if ! run createvm --name "$VM" --ostype Linux_64 --register; then
        stage "the VM is created" FAILED
        return 1
    fi
    run modifyvm "$VM" --memory 768 --cpus 1 --ioapic on --chipset ich9 --firmware efi \
        --vm-execution-engine native-api --nic1 none --audio-enabled off \
        --graphicscontroller vmsvga --vram 16 --uart1 0x3f8 4 --uartmode1 file "$SERIAL" \
        --boot1 disk --boot2 none --boot3 none --boot4 none
    # Where every exit from the guest is very slow, time in the VM can be made
    # to pass more slowly than outside of it.
    if [ -n "${SIM_WARP:-}" ]; then
        run setextradata "$VM" VBoxInternal/TM/WarpDrivePercentage "$SIM_WARP"
    fi
    run storagectl "$VM" --name sata --add sata --controller IntelAhci --portcount 1
    if ! run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest.vdi"; then
        stage "the VM is created" FAILED "no disk"
        return 1
    fi
    VMDIR=$(dirname "$(vbm showvminfo "$VM" --machinereadable | sed -n 's/^CfgFile="\(.*\)"$/\1/p')")
    vbm showvminfo "$VM" --machinereadable | grep -a -i -E '^(chipset|firmware|VMExecutionEngine|ioapic|uart1|uartmode1|SATA|storagecontroller)' \
        | cut -c1-200 | sed 's/^/    /'
    stage "the VM is created" ok
}


# The time in front of the lines of the log of a VM, which users ask about in
# the tracker of upstream (requests 390 and 131; a text for the manual says
# what there is): the log tells the date and the time at which it was opened,
# every line has the time since the start of the VM process in front, and with
# "time" among the flags of the release log, which the VM process takes from
# its environment, every line has the time of day in UTC in front of that.
phase_logtime()
{
    local log="$VMDIR/Logs/VBox.log" opened n at want
    PHASE=logtime
    reset_vm
    STARTVM_MORE="--putenv VBOX_RELEASE_LOG_FLAGS=time"
    if boot "a VM which is started with the time of day for its log boots the guest"; then
        opened=$(grep -a -m 1 ' Log opened [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9:.]*Z' "$log")
        echo "    | $(echo "$opened" | awk '{ print substr($0, 1, 120) }')"
        if [ -n "$opened" ]; then
            stage "the log tells the date and the time it was opened at, in UTC" ok
        else
            stage "the log tells the date and the time it was opened at, in UTC" FAILED "no such line"
        fi
        # Two times in front of a line now: the time of day, then the time since the start.
        n=$(grep -a -c -E '^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6} ' "$log")
        at=$(echo "$opened" | awk '{ print substr($1, 1, 8) }')
        want=$(echo "$opened" | awk '{ for (i = 1; i <= NF; i++) if ($i == "opened") { split($(i + 1), a, "T"); print substr(a[2], 1, 8) } }')
        echo "    $n lines of $(grep -a -c '' "$log") have two times in front; the line about the opening has $at in front and tells of $want"
        if [ "${n:-0}" -ge 100 ] && [ -n "$at" ] && [ "$at" = "$want" ]; then
            stage "with the flag time every line of the log has the time of day in front, in UTC" ok
        else
            stage "with the flag time every line of the log has the time of day in front, in UTC" FAILED "$n lines with two times, $at against $want"
        fi
    fi
    STARTVM_MORE=
    end_phase
}

# A VM as there are many, without anything of this work being asked for.
phase_plain()
{
    PHASE=plain
    reset_vm
    if boot "a VM without a device of the host boots the guest"; then
        serial_show 'GUEST: pci:' 20
        serial_show 'GUEST: k: .*(_OSC|PNP0A0)' 8
        guest_goes_on "the guest keeps running"
        if [ "$WHERE" = runner ]; then
            power_button_check "guest: gets the interrupt of the power button"
        fi
    fi
    end_phase
}

# The hot-plug capable slot without a device: a VM which is configured for a
# device which is not there, with a layout to be built from.
phase_emptyslot()
{
    PHASE=emptyslot
    reset_vm
    if [ ! -e /dev/iommu ]; then
        stage "a VM with an empty hot-plug capable slot boots the guest" skipped "no /dev/iommu here"
        return
    fi
    local addr=$FAKE_ADDR
    run modifyvm "$VM" --pci-attach "$addr@0000:01:00.0" && ATTACHED=$addr
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    run setextradata "$VM" "VBoxInternal2/HostPCILayout/$addr" "$FAKE_LAYOUT"
    if boot "a VM with an empty hot-plug capable slot boots the guest"; then
        guest_slot_checks
        serial_show 'GUEST: pci:' 20
        if serial_has 'GUEST: PRESENT'; then
            stage "guest: finds the slot empty" FAILED
        else
            stage "guest: finds the slot empty" ok
        fi
        check_pcistate hotplug,absent,unused
        if run controlvm "$VM" plugpci "$addr"; then
            stage "plugpci of a device which is not there is refused" FAILED "it went through"
        else
            stage "plugpci of a device which is not there is refused" ok
        fi
        guest_goes_on "the VM runs on after that"
        if [ "$WHERE" = runner ]; then
            power_button_check "guest: gets the interrupt of the power button"
        fi
        if saved_state_round; then
            guest_goes_on "the guest keeps running after the state was loaded"
            if [ "$WHERE" = runner ]; then
                power_button_check "guest: gets the interrupt of the power button after the state was loaded"
            fi
        fi
    fi
    end_phase
}

# A VM with a network adapter behind NAT: an address from the DHCP server of
# the NAT engine, its gateway answering a ping, and a file fetched through it
# from the machine the VM runs on, compared by its checksum.  Nothing of the
# passthrough work is in this.  It is here for what the branches for 7.1 took
# over from 7.2 in the NAT library and in the virtio network device, which no
# other check goes through; on the other branches it tells that a VM has a
# network at all.
phase_nat()
{
    PHASE=nat
    reset_vm
    if [ -z "${SIM_HTTP_MD5:-}" ]; then
        stage "a VM with a network adapter behind NAT boots the guest" skipped "nothing to fetch was set up"
        return
    fi
    # The file comes from the loopback interface of the machine the VM runs
    # on, by way of the gateway address of the NAT engine.  A VM does not get
    # there unless it is allowed to, and a new VM is not.
    run modifyvm "$VM" --nic1 nat --nictype1 virtio --cableconnected1 on --nat-localhostreachable1 on
    if boot "a VM with a network adapter behind NAT boots the guest"; then
        wait_serial 'GUEST: NET http' 150
        serial_show 'GUEST: (NET|pci: .* driver virtio)' 8
        if serial_has 'GUEST: NET dhcp [0-9]'; then
            stage "guest: gets an address from the NAT engine" ok
        else
            stage "guest: gets an address from the NAT engine" FAILED
        fi
        if serial_has 'GUEST: NET ping ok'; then
            stage "guest: the gateway of the NAT engine answers a ping" ok
        else
            stage "guest: the gateway of the NAT engine answers a ping" FAILED
        fi
        if serial_has "GUEST: NET http md5=$SIM_HTTP_MD5 "; then
            stage "guest: fetches a file through NAT from the machine the VM runs on, the checksum is right" ok
        else
            stage "guest: fetches a file through NAT from the machine the VM runs on, the checksum is right" FAILED \
                  "$(serial_new | grep -a 'GUEST: NET http' | tail -n 1 | cut -c1-100)"
            diag
        fi
        guest_goes_on "the guest keeps running"
    fi
    end_phase
    settle
    run modifyvm "$VM" --nic1 none
}

# The VM as an appliance: exported, imported again, and the guest booted from
# what was imported.  Nothing of the passthrough work is in this.  It is here
# for what the branches for 7.1 took over from 7.2 in the import: a serial
# port which writes to a file and the shared folders of the machine are not
# taken from an appliance, its extra data only when that is asked for; and
# the disk goes through the VMDK code on its way out and back in, which the
# guest booting from it tells about.  A build which does not have one of these
# is told from what the import says of the appliance before it does anything,
# and the checks about it are left out there.
phase_import()
{
    PHASE=import
    reset_vm
    local ova="$SIM_WORK/sim.ova" dry="$SIM_WORK/import-dry.out" imp=sim-imported kept=sim-kept
    local info="$SIM_WORK/import-info.out" has_san=no has_opt=no val orig_vm=$VM orig_dir=$VMDIR
    rm -f "$ova"
    mkdir -p "$SIM_WORK/share"
    run setextradata "$VM" SimNote kept
    run sharedfolder add "$VM" --name simshare --hostpath "$SIM_WORK/share"
    if ! VBM_TIMEOUT=900 run export "$VM" --output "$ova"; then
        stage "the VM is exported to an appliance" FAILED
    else
        stage "the VM is exported to an appliance" ok
        ls -s "$ova" 2>&1 | sed 's/^/    /'
        VBM_TIMEOUT=300 vbm import "$ova" -n > "$dry" 2>&1
        grep -a -i -E 'warning|error' "$dry" | cut -c1-300 | sed 's/^/    | /'
        grep -a -q 'raw file' "$dry" && has_san=yes
        VBM_TIMEOUT=300 vbm import "$ova" -n --options KeepExtraData > /dev/null 2>&1 && has_opt=yes
        echo "    this build leaves the serial port and the shared folders out: $has_san; it has the option for the extra data: $has_opt"

        if ! VBM_TIMEOUT=900 run import "$ova" --vsys 0 --vmname "$imp"; then
            stage "the appliance is imported" FAILED
            tail -n 30 "$VBOX_USER_HOME/VBoxSVC.log" 2> /dev/null | cut -c1-300 | sed 's/^/    | /'
        else
            stage "the appliance is imported" ok
            vbm showvminfo "$imp" --machinereadable > "$info" 2>&1
            grep -a -i -E '^(uart1|uartmode1|SharedFolder|firmware|VMExecutionEngine|"?sata)' "$info" | cut -c1-200 | sed 's/^/    /'
            val=$(vbm getextradata "$imp" SimNote 2>&1 | tail -n 1)
            echo "    extra data of the imported VM: $val"
            if [ "$has_san" = yes ]; then
                if grep -a -q '^uartmode1="file' "$info"; then
                    stage "the serial port of the imported VM does not write to a file" FAILED
                else
                    stage "the serial port of the imported VM does not write to a file" ok
                fi
                if grep -a -q '^SharedFolderNameMachineMapping' "$info"; then
                    stage "the imported VM has no shared folder" FAILED
                else
                    stage "the imported VM has no shared folder" ok
                fi
            else
                stage "the serial port of the imported VM does not write to a file" skipped "this build takes the port and the folders over"
            fi
            if [ "$has_opt" = yes ]; then
                if [ "$val" = "No value set!" ]; then
                    stage "the imported VM has none of the extra data" ok
                else
                    stage "the imported VM has none of the extra data" FAILED "$val"
                fi
                if VBM_TIMEOUT=900 run import "$ova" --vsys 0 --vmname "$kept" --options KeepExtraData; then
                    val=$(vbm getextradata "$kept" SimNote 2>&1 | tail -n 1)
                    if [ "$val" = "Value: kept" ]; then
                        stage "an import which is asked to keeps the extra data" ok
                    else
                        stage "an import which is asked to keeps the extra data" FAILED "$val"
                    fi
                else
                    stage "an import which is asked to keeps the extra data" FAILED "the import fails"
                fi
                run unregistervm "$kept" --delete
            else
                stage "the imported VM has none of the extra data" skipped "this build has no option for it"
            fi

            # The guest from the imported disk, with the serial port the checks read.
            VM=$imp
            VMDIR=$(dirname "$(vbm showvminfo "$VM" --machinereadable | sed -n 's/^CfgFile="\(.*\)"$/\1/p')")
            run modifyvm "$VM" --uartmode1 file "$SERIAL"
            if boot "the imported VM boots the guest"; then
                guest_goes_on "the guest keeps running"
            fi
            end_phase
            settle
            run unregistervm "$VM" --delete
            VM=$orig_vm
            VMDIR=$orig_dir
        fi
    fi
    run sharedfolder remove "$VM" --name simshare
    vbm setextradata "$VM" SimNote > /dev/null 2>&1
    rm -f "$ova"
}

# The device as upstream passes it through: there when the VM starts, no
# hot-plug.
phase_passthrough()
{
    PHASE=passthrough
    reset_vm
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    run modifyvm "$VM" --pci-attach "$HOSTADDR@0000:01:00.0" && ATTACHED=$HOSTADDR
    if boot "a VM with the device passed through boots the guest"; then
        guest_reads 1 "guest: reads from the drive which is passed through"
        # The guest writes after it has read, and tells of its interrupts
        # after that.
        wait_serial 'GUEST: (IRQ|NODRIVE) 1' 60
        serial_show 'GUEST: (pci: 0000:01|PRESENT|READ|IRQ|WROTE|WRITE-FAILED|NODRIVE)' 10
        grep -a 'VFIO#' "$VMDIR/Logs/VBox.log" | cut -c1-200 | head -n 30 | sed 's/^/    | /'
        if serial_has 'GUEST: WROTE 1'; then
            stage "guest: writes to the drive" ok
        else
            stage "guest: writes to the drive" FAILED
        fi
        check_pcistate fixed,present,inuse

        run controlvm "$VM" pause
        sleep 3
        run controlvm "$VM" resume
        serial_mark
        if wait_serial "GUEST: REREAD [0-9]+ md5=$EXPECT_MD5" 150; then
            stage "guest: reads from the drive after pause and resume" ok
        else
            stage "guest: reads from the drive after pause and resume" FAILED
            serial_show 'GUEST: REREAD' 5
            diag
        fi

        VBM_TIMEOUT=120 run controlvm "$VM" savestate
        if [ $? -eq 0 ]; then
            stage "saving the state with the device in the VM is refused" FAILED "it was saved"
        else
            stage "saving the state with the device in the VM is refused" ok
            sleep 3
            [ "$(vmstate)" = paused ] && run controlvm "$VM" resume
            guest_goes_on "the VM runs on after that" 60
        fi

        # A snapshot of a running VM has the state of the VM saved as well, so
        # it is refused in the same way, and nothing of it must be left.  On
        # the 7.1 line Main took a state which was not saved for a saved one
        # (run 250).
        if [ "$(vmstate)" = running ]; then
            local n
            VBM_TIMEOUT=120 run snapshot "$VM" take refused
            if [ $? -eq 0 ]; then
                stage "a snapshot of the running VM with the device in it is refused" FAILED "it was taken"
            else
                stage "a snapshot of the running VM with the device in it is refused" ok
            fi
            sleep 3
            [ "$(vmstate)" = paused ] && run controlvm "$VM" resume
            n=$(vbm showvminfo "$VM" --machinereadable 2> /dev/null | grep -a -c '^SnapshotName')
            if [ "$n" = 0 ]; then
                stage "the VM has no snapshot after that" ok
            else
                stage "the VM has no snapshot after that" FAILED "$n"
            fi
            guest_goes_on "the VM runs on after the snapshot was refused" 60
        fi
    fi
    end_phase
    # Should there be a snapshot after all, the phases after this one are not
    # to start from it.
    if vbm showvminfo "$VM" --machinereadable 2> /dev/null | grep -a -q '^SnapshotName'; then
        settle
        VBM_TIMEOUT=300 run snapshot "$VM" delete refused
    fi
}

# The device in the hot-plug capable slot: taken out and put in while the VM
# runs, with the guest being asked and without.
phase_hotplug()
{
    PHASE=hotplug
    reset_vm
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    run modifyvm "$VM" --pci-attach "$HOSTADDR@0000:01:00.0" && ATTACHED=$HOSTADDR
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    if boot "a VM with the device in the hot-plug capable slot boots the guest"; then
        guest_slot_checks
        guest_reads 1 "guest: reads from the drive in the slot"
        check_pcistate hotplug,present,inuse
        local lay
        lay=$(vbm getextradata "$VM" "VBoxInternal2/HostPCILayout/$HOSTADDR" 2> /dev/null | sed -n 's/^Value: //p')
        echo "    layout stored: ${lay:-none}"
        if [ -n "$lay" ]; then
            stage "the layout of the device is stored for a start without it" ok
        else
            stage "the layout of the device is stored for a start without it" FAILED
        fi

        # A snapshot for which the VM keeps running while its state is saved
        # is refused with the device in the slot.  It is the thread which
        # saves and no EMT which finds that and has the VM told; a build of
        # before the evening of 2026-11-25 held a lock of the device
        # meanwhile, and a VM could stand still there.  Nothing of the
        # snapshot must be left, and the VM runs on.
        if VBM_TIMEOUT=120 run snapshot "$VM" take refused-live --live; then
            stage "a live snapshot with the device in the slot is refused" FAILED "it was taken"
        else
            stage "a live snapshot with the device in the slot is refused" ok
        fi
        sleep 3
        [ "$(vmstate)" = paused ] && run controlvm "$VM" resume
        if vbm showvminfo "$VM" --machinereadable 2> /dev/null | grep -a -q '^SnapshotName'; then
            stage "the VM has no snapshot after the live one was refused" FAILED
        else
            stage "the VM has no snapshot after the live one was refused" ok
        fi
        guest_goes_on "the VM runs on after the live snapshot was refused" 60

        serial_mark
        local t0
        t0=$(date +%s)
        if VBM_TIMEOUT=120 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=60000; then
            stage "unplugpci: the guest is asked and lets go of the device" ok "$(( $(date +%s) - t0 )) s"
        else
            stage "unplugpci: the guest is asked and lets go of the device" FAILED
            diag
        fi
        if wait_serial 'GUEST: GONE 1' 30; then
            stage "guest: the drive is gone" ok
        else
            stage "guest: the drive is gone" FAILED
        fi
        serial_show 'pciehp|GUEST: GONE' 10
        check_pcistate hotplug,absent,unused

        serial_mark
        if run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci puts the device into the slot" ok
        else
            stage "plugpci puts the device into the slot" FAILED
            diag
        fi
        guest_reads 2 "guest: takes the device in the slot and reads from the drive"
        serial_show 'pciehp|GUEST: (PRESENT|READ)' 10
        check_pcistate hotplug,present,inuse

        serial_mark
        if VBM_TIMEOUT=60 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=1000 --force; then
            stage "unplugpci --force takes the device away from a guest which uses it" ok
        else
            stage "unplugpci --force takes the device away from a guest which uses it" FAILED
            diag
        fi
        if wait_serial 'GUEST: GONE 2' 150; then
            stage "guest: notices that the drive is gone" ok
        else
            stage "guest: notices that the drive is gone" FAILED
        fi
        guest_goes_on "the VM runs on after that" 150

        serial_mark
        if run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci after the forced removal" ok
        else
            stage "plugpci after the forced removal" FAILED
            diag
        fi
        guest_reads 3 "guest: takes the device again and reads from the drive"
        check_log_quiet
    fi
    end_phase
    # Should there be a snapshot after all, the phases after this one are not
    # to start from it.
    if vbm showvminfo "$VM" --machinereadable 2> /dev/null | grep -a -q '^SnapshotName'; then
        settle
        VBM_TIMEOUT=300 run snapshot "$VM" delete refused-live
    fi
}

# The host wants the device back while the guest uses it, as when it is
# unplugged in an orderly way.
phase_hostremove()
{
    PHASE=hostremove
    reset_vm
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    run modifyvm "$VM" --pci-attach "$HOSTADDR@0000:01:00.0" && ATTACHED=$HOSTADDR
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    if boot "a VM with the device in the hot-plug capable slot boots the guest"; then
        guest_reads 1 "guest: reads from the drive in the slot"
        serial_mark
        echo "+ echo 1 > /sys/bus/pci/devices/$HOSTADDR/remove"
        # The write returns when the device was let go of.  It can not be
        # interrupted while it waits, so it is left behind if it takes too long.
        local t0 pid i=0
        t0=$(date +%s)
        ( echo 1 > "/sys/bus/pci/devices/$HOSTADDR/remove" ) &
        pid=$!
        while kill -0 "$pid" 2> /dev/null && [ "$i" -lt 120 ]; do
            sleep 1
            i=$((i + 1))
        done
        if kill -0 "$pid" 2> /dev/null; then
            stage "host: gets the device back when it asks for it" FAILED "kept waiting"
            diag
        else
            stage "host: gets the device back when it asks for it" ok "$(( $(date +%s) - t0 )) s"
        fi
        if wait_serial 'GUEST: GONE 1' 60; then
            stage "guest: was asked and let go, the drive is gone" ok
        else
            stage "guest: was asked and let go, the drive is gone" FAILED
        fi
        serial_show 'pciehp|GUEST: GONE' 10
        guest_goes_on "the VM runs on after that" 90
        check_pcistate hotplug,absent,unused

        echo "+ echo 1 > /sys/bus/pci/rescan"
        echo 1 > /sys/bus/pci/rescan
        if bind_dev; then
            serial_mark
            if run controlvm "$VM" plugpci "$HOSTADDR"; then
                stage "plugpci of the device which came back" ok
            else
                stage "plugpci of the device which came back" FAILED
                diag
            fi
            guest_reads 2 "guest: takes the device which came back and reads from the drive"
        else
            stage "host: the device is back after a rescan" FAILED
        fi
        check_log_quiet
    fi
    end_phase
    # For the phases after this one.
    [ -e "/sys/bus/pci/devices/$HOSTADDR" ] || echo 1 > /sys/bus/pci/rescan
    bind_dev
}

# Asks the runner to do to this machine what it cannot do to itself, see
# host-agent.sh: the request goes into the directory this machine shares with
# the runner, and so does the answer, which is shown.  Fails if there is no
# answer or if it does not begin with "OK".
host_request() # <request>
{
    local i=0
    rm -f "$SIM_OUT/host-reply"
    echo "$*" > "$SIM_OUT/host-request.tmp" && mv "$SIM_OUT/host-request.tmp" "$SIM_OUT/host-request"
    while [ ! -e "$SIM_OUT/host-reply" ] && [ "$i" -lt 60 ]; do
        sleep 1
        i=$((i + 1))
    done
    if [ ! -e "$SIM_OUT/host-reply" ]; then
        echo "    no answer from the runner in $i s"
        rm -f "$SIM_OUT/host-request"
        return 1
    fi
    head -n 5 "$SIM_OUT/host-reply" | sed -e 's/^\(.\{200\}\).*/\1/' -e 's/^/    runner: /'
    grep -q '^OK' "$SIM_OUT/host-reply"
}

# Where the host has the memory of each function of the device: the six BAR
# registers as the device itself has them, read through sysfs.  A reset of the
# bus by the kernel of the host, which is what it does about an error it takes
# for fatal, leaves them empty unless something writes them again, and a
# device without them is of no use to a guest which gets it next.
host_bars()
{
    local f s=
    for f in $HOSTFUNCS; do
        s="$s ${f##*:}=$(od -A n -t x1 -j 16 -N 24 "/sys/bus/pci/devices/$f/config" 2> /dev/null | tr -d ' \n')"
    done
    echo "${s# }"
}
host_bars_check() # <what they were> <the moment, as the stage is to call it>
{
    local now f
    now=$(host_bars)
    if [ -z "$1" ]; then
        stage "host: the functions of the device have their memory addresses $2" skipped "they were not read before"
    elif [ "$now" = "$1" ]; then
        stage "host: the functions of the device have their memory addresses $2" ok
    else
        stage "host: the functions of the device have their memory addresses $2" FAILED "the BAR registers were: $1; they are: $now"
        for f in $HOSTFUNCS; do
            echo "    | $f, the first 64 bytes of its configuration space: $(od -A n -t x1 -N 64 "/sys/bus/pci/devices/$f/config" 2> /dev/null | tr -s ' \n' ' ')"
        done
    fi
}

# A build which holds on to a device after an error the host reported of it
# lets go of the device in its own time, and that is when the host puts back
# what the device had before its bus was reset.  No device goes into the slot
# before, so this waits for it.  A message of the build tells whether it is
# one which does that.
hold_knows()
{
    grep -a -q 'is held on to for a moment' "$VBOX_BIN/VBoxDD.so" 2> /dev/null
}
hold_lines()
{
    local n
    n=$(grep -a -c 'which was held on to is let go of' "$VMDIR/Logs/VBox.log" 2> /dev/null)
    echo "${n:-0}"
}
hold_wait() # <what hold_lines told before the error> <the moment, as the stage is to call it>
{
    local want i=0 n
    if ! hold_knows; then
        stage "VirtualBox holds on to the device for a moment and lets go of it then $2" skipped "this build lets go of a device at once"
        return 0
    fi
    want=$(echo $HOSTFUNCS | wc -w)
    while [ "$i" -lt 60 ]; do
        n=$(( $(hold_lines) - ${1:-0} ))
        [ "$n" -ge "$want" ] && break
        sleep 1
        i=$((i + 1))
    done
    grep -a 'held on to' "$VMDIR/Logs/VBox.log" | tail -n $((want * 2)) | awk '{ print "    | " substr($0, 1, 200) }'
    if [ "$n" -ge "$want" ]; then
        stage "VirtualBox holds on to the device for a moment and lets go of it then $2" ok
    else
        stage "VirtualBox holds on to the device for a moment and lets go of it then $2" FAILED "the log of the VM tells of $n function(s) let go of, of $want, after $i s"
    fi
}

# One error of the device, as the host tells of it, with the device in the
# slot and the guest using it.
host_error_round() # <round> <what the error is called> <status>
{
    local round=$1 what=$2 status=$3 log="$VMDIR/Logs/VBox.log" n0 n1 want i=0 bars h0
    n0=$(grep -a -c 'the host reported an error of the device' "$log" 2> /dev/null)
    h0=$(hold_lines)
    want=$(echo $HOSTFUNCS | wc -w)
    bars=$(host_bars)
    serial_mark
    dmesg -C 2> /dev/null
    port_says "before the error which is $what"
    echo "+ $HE_PORT reports an error: $what (status $status)"
    if ! host_request aer "$status"; then
        stage "host: $HE_PORT reports an error which is $what" FAILED "the machine was not made to"
        return 1
    fi
    stage "host: $HE_PORT reports an error which is $what" ok
    # The kernel tells the driver which has the device, and that one VirtualBox.
    while [ "$i" -lt 30 ] && [ "$(grep -a -c 'the host reported an error of the device' "$log" 2> /dev/null)" = "${n0:-0}" ]; do
        sleep 1
        i=$((i + 1))
    done
    dmesg | grep -i -E 'aer|vfio|pcieport|nvme|reset' | sed -e 's/^\(.\{200\}\).*/\1/' -e 's/^/    | /' | head -n 30
    if dmesg | grep -q -i -E 'AER: .*error'; then
        stage "host: its kernel takes the error up ($what)" ok
    else
        stage "host: its kernel takes the error up ($what)" FAILED "its log has no line of the error"
    fi
    if [ "$(grep -a -c 'the host reported an error of the device' "$log" 2> /dev/null)" != "${n0:-0}" ]; then
        stage "VirtualBox is told of the error and takes the device out of service ($what)" ok "after $i s"
    else
        stage "VirtualBox is told of the error and takes the device out of service ($what)" FAILED "nothing in the log of the VM after $i s"
        diag
        return 1
    fi
    if [ "$want" -gt 1 ]; then
        # The kernel tells the driver of each function, and the device goes as
        # the one device its functions are, whichever is told of first.
        i=0
        while [ "$i" -lt 10 ]; do
            n1=$(grep -a -c 'the host reported an error of the device' "$log" 2> /dev/null)
            [ $(( ${n1:-0} - ${n0:-0} )) -ge "$want" ] && break
            sleep 1
            i=$((i + 1))
        done
        if [ $(( ${n1:-0} - ${n0:-0} )) = "$want" ]; then
            stage "every function of the device is taken out of service, once ($what)" ok
        else
            stage "every function of the device is taken out of service, once ($what)" FAILED "$(( ${n1:-0} - ${n0:-0} )) lines for $want functions"
        fi
    fi
    grep -a -E 'taken out of service|reported an error' "$log" | tail -n 4 | sed -e 's/^\(.\{200\}\).*/\1/' -e 's/^/    | /'
    if wait_serial "GUEST: GONE $round" 150; then
        stage "guest: notices that $HE_DRIVES gone ($what)" ok
    else
        stage "guest: notices that $HE_DRIVES gone ($what)" FAILED
    fi
    serial_show 'pciehp|GUEST: GONE' 10
    guest_goes_on "the VM runs on after the error ($what)" 150
    $HE_STATE hotplug,absent,unused
    port_says "after the error which is $what"
    hold_wait "$h0" "after the error ($what)"
    # The device is what it was to the host, or is made that again.
    [ -e "/sys/bus/pci/devices/$HOSTADDR" ] || echo 1 > /sys/bus/pci/rescan
    if ! bind_dev; then
        stage "host: has the device after the error ($what)" FAILED
        return 1
    fi
    stage "host: has the device after the error ($what)" ok
    host_bars_check "$bars" "after the error ($what)"
    serial_mark
    if run controlvm "$VM" plugpci "$HOSTADDR"; then
        stage "plugpci after the error ($what)" ok
    else
        stage "plugpci after the error ($what)" FAILED
        diag
        return 1
    fi
    $HE_READS $((round + 1)) "guest: takes the device again and reads from $HE_FROM ($what)"
}

# The host reports an error of the device while the guest uses it.  The port
# the device is behind, which the machine standing in for the host emulates,
# is made to report an error it cannot correct, from outside that machine
# (see host_request).  The kernel of the host, the driver which has the
# device for VirtualBox and VirtualBox then go through what they do about
# such an error: the kernel tells the drivers of all which is behind the
# port.  First an error the kernel does not take for fatal, then one it
# does, after which it resets the bus the device is on.
phase_hosterror()
{
    local port root
    PHASE=hosterror
    reset_vm
    # The device of one function, or the one of two, which a graphics card
    # with its audio function is: the kernel tells the drivers of both then.
    HE_READS=guest_reads
    HE_STATE=check_pcistate
    HE_DRIVES="the drive is"
    HE_FROM="the drive"
    if [ "$HOSTFUNCS" != "$HOSTADDR" ]; then
        HE_READS=guest_reads_both
        HE_STATE=check_pcistate_all
        HE_DRIVES="both drives are"
        HE_FROM="both drives"
    fi
    if [ -z "${SIM_HOSTAGENT:-}" ]; then
        stage "the port in front of the device can be made to report an error" skipped "nobody outside this machine to do that"
        return
    fi
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    port=$(basename "$(readlink -f "/sys/bus/pci/devices/$HOSTADDR/..")")
    echo "    the device is $HOSTADDR, the port in front of it $port"
    # The port which reports the error is the root port.  That is the port
    # in front of the device, or the one above the switch the device is
    # behind.
    HE_PORT="the port in front of the device"
    root=$(root_port_of "$port")
    if [ -n "$root" ] && [ "$root" != "$port" ]; then
        HE_PORT="the root port above the switch the device is behind"
        echo "    the root port above it is $root, with between them: $(readlink -f "/sys/bus/pci/devices/$port" | sed -e "s#^.*/$root/##" -e "s#/$port\$##")"
        echo "    the IOMMU group of the device has: $(ls "/sys/bus/pci/devices/$HOSTADDR/iommu_group/devices" 2>&1 | tr '\n' ' ')"
        port=$root
    fi
    lspci -s "$port" -vv 2> /dev/null | grep -A 6 'Advanced Error Reporting' | sed -e 's/^\(.\{160\}\).*/\1/' -e 's/^/    | /'
    if [ "$HOSTFUNCS" != "$HOSTADDR" ]; then
        attach_funcs
    else
        run modifyvm "$VM" --pci-attach "$HOSTADDR@0000:01:00.0" && ATTACHED=$HOSTADDR
    fi
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    if boot "a VM with the device in the hot-plug capable slot boots the guest"; then
        $HE_READS 1 "guest: reads from $HE_FROM in the slot"
        # Completer Abort, then Malformed TLP: by the specification the first
        # is not fatal unless a device says so, and the second is.
        if host_error_round 1 "not fatal" 0x8000; then
            host_error_round 2 "fatal" 0x40000
        fi
        echo "    errors the kernel has counted for $port as not fatal: $(tr '\n' ' ' < "/sys/bus/pci/devices/$port/aer_dev_nonfatal" 2> /dev/null | sed -e 's/^\(.\{200\}\).*/\1/')"
        echo "    and as fatal: $(tr '\n' ' ' < "/sys/bus/pci/devices/$port/aer_dev_fatal" 2> /dev/null | sed -e 's/^\(.\{200\}\).*/\1/')"
    fi
    end_phase
    # For the phases after this one.
    [ -e "/sys/bus/pci/devices/$HOSTADDR" ] || echo 1 > /sys/bus/pci/rescan
    bind_dev
}

# Whether a function has an address in one of its BAR registers, going by
# what host_bars tells of it: the bits of a register above the lowest four,
# which say what kind of register it is and stay as they are.
bars_have_address() # <the six registers of a function as host_bars tells them>
{
    local h=$1 i=0 b0
    [ "${#h}" -ge 48 ] || return 1
    while [ "$i" -lt 6 ]; do
        [ "${h:$((i * 8 + 2)):6}" != 000000 ] && return 0
        b0=$(( 16#${h:$((i * 8)):2} ))
        if [ $((b0 & 1)) = 1 ]; then
            # I/O ports: the address is above the lowest two bits.
            [ $((b0 & 0xfc)) != 0 ] && return 0
        else
            [ $((b0 & 0xf0)) != 0 ] && return 0
            # A 64-bit register has the upper half of the address in the next one.
            if [ $((b0 & 6)) = 4 ] && [ "$i" -lt 5 ]; then
                [ "${h:$((i * 8 + 8)):8}" != 00000000 ] && return 0
                i=$((i + 1))
            fi
        fi
        i=$((i + 1))
    done
    return 1
}

# No function of the device has an address in a BAR register.
host_bars_none() # <what host_bars told>
{
    local w
    for w in $1; do
        bars_have_address "${w#*=}" && return 1
    done
    return 0
}

# Whether the kernel of the host has the functions of the device awake or
# asleep, which it puts a device to that nobody uses.
host_rpm()
{
    local f s=
    for f in $HOSTFUNCS; do
        s="$s ${f##*:}=$(cat "/sys/bus/pci/devices/$f/power/runtime_status" 2> /dev/null || echo '?')"
    done
    echo "${s# }"
}

host_rpm_wait() # <active|suspended> <seconds>
{
    local i=0 f all
    while [ "$i" -lt "$2" ]; do
        all=1
        for f in $HOSTFUNCS; do
            [ "$(cat "/sys/bus/pci/devices/$f/power/runtime_status" 2> /dev/null)" = "$1" ] || all=
        done
        [ -n "$all" ] && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

host_rpm_control() # <on|auto>
{
    local f
    for f in $HOSTFUNCS; do
        echo "$1" > "/sys/bus/pci/devices/$f/power/control" 2> /dev/null
    done
}

# The bus the device is on is reset through the port in front of it: the bit
# of the bridge control register which does that is set and cleared again,
# and nobody is told, neither the kernel of this machine nor a driver.  It is
# what that kernel does itself about an error it takes for fatal, and there
# it tells nobody afterwards either.
host_bus_reset_by_hand()
{
    local port
    port=$(basename "$(readlink -f "/sys/bus/pci/devices/$HOSTADDR/..")")
    echo "+ setpci -s $port 3e.w=0040:0040, and 0000:0040 a second later (the register was $(setpci -s "$port" 3e.w 2> /dev/null || echo '?'))"
    setpci -s "$port" 3e.w=0040:0040 || return 1
    sleep 1
    setpci -s "$port" 3e.w=0000:0040 || return 1
    sleep 2
    return 0
}

# What the kernel of the host said of the device since its log was cleared.
lostaddr_dmesg()
{
    dmesg | grep -i -E 'vfio|enabling device|BAR |nvme|pcieport' | awk '{ print "    | " substr($0, 1, 200) }' | head -n 12
}

# The device goes into the slot again, as the host has it now.  One which has
# its addresses is taken and the guest reads from it.  One which has lost them
# is of no use to a guest: a build which tells such a device refuses it and
# says what gets the addresses back, which is then done here, and a build
# which does not is watched with it.  LA_ROUND counts what the guest counts.
lostaddr_plug() # <how the device got where it is, for the stages>
{
    local how=$1 now out rc f i
    now=$(host_bars)
    dmesg -C 2> /dev/null
    serial_mark
    if ! host_bars_none "$now"; then
        if run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci of the device ($how)" ok
        else
            stage "plugpci of the device ($how)" FAILED
            diag
            return 1
        fi
        LA_ROUND=$((LA_ROUND + 1))
        $HE_READS "$LA_ROUND" "guest: takes the device and reads from $HE_FROM ($how)"
        lostaddr_dmesg
        return 0
    fi

    echo "+ VBoxManage controlvm $VM plugpci $HOSTADDR"
    out=$(VBM_TIMEOUT=180 vbm controlvm "$VM" plugpci "$HOSTADDR" 2>&1)
    rc=$?
    echo "$out" | awk '{ print "    " substr($0, 1, 600) }'
    lostaddr_dmesg
    if [ "$rc" != 0 ]; then
        if [ -n "$LA_KNOWN" ]; then
            stage "plugpci of a device which has lost its memory addresses is refused ($how)" ok
            if echo "$out" | grep -q 'rescan'; then
                stage "it says what gets the addresses back ($how)" ok
            else
                stage "it says what gets the addresses back ($how)" FAILED
            fi
        else
            stage "plugpci of a device which has lost its memory addresses is refused ($how)" skipped "this build does not tell such a device, and has refused it with other words"
        fi
        $HE_STATE hotplug,absent,unused
        guest_goes_on "the VM runs on after that ($how)" 90
    else
        if [ -n "$LA_KNOWN" ]; then
            stage "plugpci of a device which has lost its memory addresses is refused ($how)" FAILED "it was taken"
        else
            stage "plugpci of a device which has lost its memory addresses is refused ($how)" skipped "this build does not tell such a device and takes it as it is"
        fi
        # What a guest makes of it.
        LA_ROUND=$((LA_ROUND + 1))
        wait_serial "GUEST: (READ|NODRIVE) $LA_ROUND" 150
        echo "    the guest with the device as it is: $(serial_new | grep -a -E 'GUEST: (READ|NODRIVE|PRESENT|WROTE|WRITE-FAILED) ' | tail -n 3 | awk '{ print substr($0, 1, 90) }' | tr '\n' '|')"
        serial_show 'GUEST: k: .*nvme' 8
        serial_mark
        VBM_TIMEOUT=120 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=20000 --force
        wait_serial "GUEST: GONE $LA_ROUND" 150 || echo "    the guest does not tell that the device went"
        guest_goes_on "the VM runs on with such a device taken away again ($how)" 150
    fi

    # What gets the addresses back: the host forgets the device and finds it
    # anew, and sets up what it finds.
    for f in $HOSTFUNCS; do
        echo "+ echo 1 > /sys/bus/pci/devices/$f/remove"
        echo 1 > "/sys/bus/pci/devices/$f/remove"
    done
    echo "+ echo 1 > /sys/bus/pci/rescan"
    echo 1 > /sys/bus/pci/rescan
    i=0
    while [ "$i" -lt 10 ] && ! host_has_all; do
        sleep 1
        i=$((i + 1))
    done
    now=$(host_bars)
    echo "    the BAR registers of the device after that: $now"
    if host_has_all && ! host_bars_none "$now"; then
        stage "host: the device has memory addresses again after it was removed and found anew ($how)" ok
    else
        stage "host: the device has memory addresses again after it was removed and found anew ($how)" FAILED
        return 1
    fi
    if ! bind_dev; then
        stage "the device is bound to vfio-pci again ($how)" FAILED
        return 1
    fi
    dmesg -C 2> /dev/null
    serial_mark
    if run controlvm "$VM" plugpci "$HOSTADDR"; then
        stage "plugpci of the device which has its addresses again ($how)" ok
    else
        stage "plugpci of the device which has its addresses again ($how)" FAILED
        diag
        return 1
    fi
    LA_ROUND=$((LA_ROUND + 1))
    $HE_READS "$LA_ROUND" "guest: takes the device which has its addresses again and reads from $HE_FROM ($how)"
}

# The guest lets go of the device, and nobody has it then.
lostaddr_unplug() # <what the stage is to add>
{
    serial_mark
    if VBM_TIMEOUT=120 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=60000; then
        stage "unplugpci: the guest lets go of the device ($1)" ok
    else
        stage "unplugpci: the guest lets go of the device ($1)" FAILED
        diag
        return 1
    fi
    wait_serial "GUEST: GONE $LA_ROUND" 60 || echo "    the guest does not tell that the device went"
    $HE_STATE hotplug,absent,unused
}

# The bus of the device is reset on the host while nobody has the device, the
# VM not either: the device is out of the slot, bound to the driver which has
# it for VirtualBox, and unused.  A reset leaves a device without the
# addresses of its memory and with nothing switched on, and whether it has
# them again when it is opened next depends on what the kernel of the host
# kept of it.  Asleep, which is where that kernel puts a device nobody uses,
# it wakes up with what it had when it fell asleep.  Awake it has nothing put
# back, and falls asleep later as what the reset left of it.  The second is
# what a device is left as which VirtualBox let go of after an error the host
# takes for fatal, before the kernel had reset the bus for that error: the
# kernel keeps a device awake while it deals with an error of it (see "A
# drive which Windows does not start after an error the host takes for
# fatal" in STATUS.md).
phase_lostaddr()
{
    local bars now
    PHASE=lostaddr
    reset_vm
    HE_READS=guest_reads
    HE_STATE=check_pcistate
    HE_FROM="the drive"
    if [ "$HOSTFUNCS" != "$HOSTADDR" ]; then
        HE_READS=guest_reads_both
        HE_STATE=check_pcistate_all
        HE_FROM="both drives"
    fi
    LA_ROUND=1
    LA_KNOWN=
    if ! command -v setpci > /dev/null 2>&1; then
        stage "the bus of the device can be reset by hand" skipped "this machine has no setpci"
        return
    fi
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    if grep -a -q 'has lost its addresses on the host' "$VBOX_BIN/VBoxDD.so" 2> /dev/null; then
        LA_KNOWN=1
        echo "    this build tells a device which has lost its memory addresses"
    else
        echo "    this build does not tell a device which has lost its memory addresses"
    fi
    if [ "$HOSTFUNCS" != "$HOSTADDR" ]; then
        attach_funcs
    else
        run modifyvm "$VM" --pci-attach "$HOSTADDR@0000:01:00.0" && ATTACHED=$HOSTADDR
    fi
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    if boot "a VM with the device in the hot-plug capable slot boots the guest"; then
        $HE_READS 1 "guest: reads from $HE_FROM in the slot"
        bars=$(host_bars)
        echo "    the BAR registers of the device as the host has set them up: $bars"

        # Asleep.
        if lostaddr_unplug "the first time"; then
            if host_rpm_wait suspended 20; then
                echo "    with nobody having the device its functions are: $(host_rpm)"
                if host_bus_reset_by_hand; then
                    echo "    the BAR registers after the reset: $(host_bars), the functions: $(host_rpm)"
                    # Woken up and left to fall asleep again, which is what
                    # opening the device and closing it does to it.
                    host_rpm_control on
                    host_rpm_wait active 10
                    now=$(host_bars)
                    host_rpm_control auto
                    host_rpm_wait suspended 20
                    if [ "$now" = "$bars" ]; then
                        stage "host: a device which slept through a reset of its bus wakes up with its memory addresses" ok
                    else
                        stage "host: a device which slept through a reset of its bus wakes up with its memory addresses" FAILED "the BAR registers were: $bars; they are: $now"
                    fi
                    lostaddr_plug "after a reset of the bus it slept through"
                else
                    stage "the bus of the device is reset by hand" FAILED
                fi
            else
                stage "host: puts the device to sleep when nobody has it" skipped "its functions are: $(host_rpm)"
                lostaddr_plug "with nothing done to it"
            fi
        fi

        # Awake.
        if lostaddr_unplug "the second time"; then
            host_rpm_control on
            if host_rpm_wait active 10; then
                bars=$(host_bars)
                if host_bus_reset_by_hand; then
                    host_rpm_control auto
                    host_rpm_wait suspended 20
                    now=$(host_bars)
                    echo "    the BAR registers after the reset, with the device left to itself again: $now, the functions: $(host_rpm)"
                    if host_bars_none "$now"; then
                        stage "host: a reset of the bus with the device awake and nobody having it leaves the device without memory addresses" ok
                    else
                        stage "host: a reset of the bus with the device awake and nobody having it leaves the device without memory addresses" FAILED "the BAR registers were: $bars; they are: $now"
                    fi
                    lostaddr_plug "after a reset of the bus with the device awake"
                else
                    stage "the bus of the device is reset by hand" FAILED
                fi
            else
                stage "host: the device can be kept awake" skipped "its functions are: $(host_rpm)"
                host_rpm_control auto
            fi
        fi
        guest_goes_on "the VM runs on after all of that" 90
    fi
    end_phase
    # For the phases after this one.
    host_rpm_control auto
    host_has_all || echo 1 > /sys/bus/pci/rescan
    bind_dev
}

# The root port a port is behind: itself, where it is one.
root_port_of() # <port>
{
    readlink -f "/sys/bus/pci/devices/$1" 2> /dev/null | sed -n -e 's#^/sys/devices/pci[^/]*/\([^/]*\).*$#\1#p'
}

# What the port in front of the device tells of its link and of its slot, for
# the log: the registers Link Status, Slot Status and Link Capabilities of its
# PCI Express capability.  A port which claims a link of no width is one the
# kernel of the host does not take a device from.  Where the device is behind
# a switch, the root port above it is shown as well.
port_says() # <when>
{
    local port root
    port=$(basename "$(readlink -f "/sys/bus/pci/devices/${HOSTADDR%:*}:00.0/.." 2> /dev/null)" 2> /dev/null)
    [ -e "/sys/bus/pci/devices/$port/vendor" ] || port=$HOST_PORT
    [ -n "$port" ] || return 0
    HOST_PORT=$port
    echo "    the port $port, $1: link status $(setpci -s "$port" CAP_EXP+12.w 2> /dev/null || echo '?'), slot status $(setpci -s "$port" CAP_EXP+1a.w 2> /dev/null || echo '?'), link capabilities $(setpci -s "$port" CAP_EXP+0c.l 2> /dev/null || echo '?')"
    root=$(root_port_of "$port")
    if [ -n "$root" ] && [ "$root" != "$port" ]; then
        echo "    the root port $root above it: link status $(setpci -s "$root" CAP_EXP+12.w 2> /dev/null || echo '?'), link capabilities $(setpci -s "$root" CAP_EXP+0c.l 2> /dev/null || echo '?')"
    fi
}
HOST_PORT=
HE_PORT="the port in front of the device"

# Whether the port in front of the device tells of a link which has a width.
# The port this machine emulates has lost the width and the speed of its link
# once it was made to report an error, the first one already, which its
# kernel does not take for fatal and resets nothing for (run 345: link status
# 2011 before, 2000 after).  It is the emulator: see the part swplughost in
# run.sh for what it does.  The kernel of the host does not take a device
# which is plugged into such a port ("Cannot train link").  No reset has to
# do with it (run 343), and a port of a real host keeps its link.  The port
# of the switch this emulator has tells of no width from the start (run 346).
# Where the register cannot be read, the port is taken to be in order.
port_link_has_width()
{
    local port v
    port=$(basename "$(readlink -f "/sys/bus/pci/devices/$HOSTADDR/.." 2> /dev/null)" 2> /dev/null)
    [ -e "/sys/bus/pci/devices/$port/vendor" ] || port=$HOST_PORT
    [ -n "$port" ] || return 0
    HOST_PORT=$port
    v=$(setpci -s "$port" CAP_EXP+12.w 2> /dev/null) || return 0
    [ -n "$v" ] || return 0
    [ $(( (16#$v >> 4) & 63 )) != 0 ]
}

# A port which has a command to its slot completed and pending, with nobody
# having taken note of it (bit 4 of its slot status), has that bit cleared.
# Seen in run 347 with the port of the emulated switch, after the kernel had
# reset everything below the root port for an error it took for fatal: slot
# status 0050 from then on, and not a word from the kernel when the port was
# told that its device is to be unplugged, for three minutes.  What is taken
# to be behind it: the kernel puts the registers of the port back after the
# reset, the slot control among them, which is a command to the slot; the
# emulator notes it as completed while the interrupt of the port is not on
# again; and it tells of an event of the slot only when none was pending
# before, so the kernel hears of none after that.  Clearing the bit is what
# the kernel does when it hears of a completed command.  A port which does
# not tell of completed commands at all, as many real ones do not, has no
# such bit to be left with.  Nothing of this is VirtualBox.
port_take_note()
{
    local port=$HOST_PORT v
    [ -n "$port" ] || return 0
    v=$(setpci -s "$port" CAP_EXP+1a.w 2> /dev/null) || return 0
    [ -n "$v" ] || return 0
    if [ $(( 16#$v & 16 )) != 0 ]; then
        echo "    the port $port has a completed command pending which its kernel took no note of (slot status $v, slot control $(setpci -s "$port" CAP_EXP+18.w 2> /dev/null || echo '?')): cleared here"
        setpci -s "$port" CAP_EXP+1a.w=0010 2> /dev/null
        echo "    the port $port after that: slot status $(setpci -s "$port" CAP_EXP+1a.w 2> /dev/null || echo '?')"
    fi
}

# Whether the host has a function of the device, and whether it has them all.
host_has_any()
{
    local a
    for a in $HOSTFUNCS; do
        [ -e "/sys/bus/pci/devices/$a" ] && return 0
    done
    return 1
}

host_has_all()
{
    local a
    for a in $HOSTFUNCS; do
        [ -e "/sys/bus/pci/devices/$a" ] || return 1
    done
    return 0
}

# The device is unplugged from the host and plugged in again, as a device in
# an enclosure on a cable is.  Not by a word to the kernel as in the phase
# hostremove: the port the device is behind is told from outside this machine
# (see host_request) that the device is to go, the hot-plug of the host's own
# kernel takes it from there, and afterwards the host has no such device any
# more.  The kernel asks the driver which has the device for VirtualBox to let
# go, that one asks VirtualBox, and VirtualBox asks the guest.  Plugged in
# again the device is a new one to the host, which has no driver bound to it.
phase_hostplug()
{
    local i t0 drv reads=guest_reads state=check_pcistate from="the drive"
    # The device of one function, or the one of two, which a graphics card
    # with its audio function is: all of it goes and comes as one.
    if [ "$HOSTFUNCS" != "$HOSTADDR" ]; then
        reads=guest_reads_both
        state=check_pcistate_all
        from="both drives"
    fi
    PHASE=${1:-hostplug}
    reset_vm
    if [ -z "${SIM_HOSTAGENT:-}" ]; then
        stage "the device can be unplugged from the host" skipped "nobody outside this machine to do that"
        return
    fi
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    dmesg | grep -i -E '_OSC: OS now controls|pciehp|acpiphp' | cut -c1-200 | tail -n 6 | sed 's/^/    | /'
    if [ "$HOSTFUNCS" != "$HOSTADDR" ]; then
        attach_funcs
    else
        run modifyvm "$VM" --pci-attach "$HOSTADDR@0000:01:00.0" && ATTACHED=$HOSTADDR
    fi
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    if boot "a VM with the device in the hot-plug capable slot boots the guest"; then
        $reads 1 "guest: reads from $from in the slot"

        serial_mark
        dmesg -C 2> /dev/null
        port_says "with the device in the VM"
        port_take_note
        echo "+ the device is unplugged from the host"
        t0=$(date +%s)
        if host_request unplug; then
            stage "host: is told that the device is to be unplugged" ok
        else
            stage "host: is told that the device is to be unplugged" FAILED
            end_phase
            return
        fi
        i=0
        while host_has_any && [ "$i" -lt 180 ]; do
            sleep 1
            i=$((i + 1))
        done
        dmesg | grep -i -E 'pciehp|acpiphp|vfio|nvme' | cut -c1-200 | head -n 20 | sed 's/^/    | /'
        if ! host_has_any; then
            stage "host: the device is gone from it" ok "$(( $(date +%s) - t0 )) s after it was told"
        else
            stage "host: the device is gone from it" FAILED "it is still there after $i s"
            diag
        fi
        port_says "with the device unplugged"
        if wait_serial 'GUEST: GONE 1' 60; then
            stage "guest: was asked and let go, the drive is gone" ok
        else
            stage "guest: was asked and let go, the drive is gone" FAILED
        fi
        serial_show 'pciehp|GUEST: GONE' 10
        grep -a -E 'taken out of service|wants the device back' "$VMDIR/Logs/VBox.log" | tail -n 3 | cut -c1-200 | sed 's/^/    | /'
        guest_goes_on "the VM runs on after that" 90
        $state hotplug,absent,unused

        # What is not on the host cannot be put into the VM.
        if ! host_has_any; then
            if run controlvm "$VM" plugpci "$HOSTADDR"; then
                stage "plugpci of the device which is not on the host is refused" FAILED "it was taken"
            else
                stage "plugpci of the device which is not on the host is refused" ok
            fi
            guest_goes_on "the VM runs on after the command which was refused" 90
        fi

        if ! port_link_has_width; then
            stage "host: the device is plugged in again" skipped "the emulated port tells of a link without a width, which is its emulator's doing (a port which was made to report an error, and a port of its switch from the start), and the kernel of the host takes no device from such a port"
            end_phase
            return
        fi
        dmesg -C 2> /dev/null
        echo "+ the device is plugged into the host again"
        if host_request plug; then
            stage "host: the device is plugged in again" ok
        else
            stage "host: the device is plugged in again" FAILED
            end_phase
            return
        fi
        i=0
        while ! host_has_all && [ "$i" -lt 120 ]; do
            sleep 1
            i=$((i + 1))
        done
        dmesg | grep -i -E 'pciehp|acpiphp|pci 0000:0|nvme' | cut -c1-200 | head -n 20 | sed 's/^/    | /'
        port_says "with the device plugged in again"
        if host_has_all; then
            drv=
            [ -e "/sys/bus/pci/devices/$HOSTADDR/driver" ] && drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$HOSTADDR/driver")")
            stage "host: has the device again" ok "after $i s, with ${drv:-no} driver"
        else
            stage "host: has the device again" FAILED "nothing at $HOSTADDR after $i s"
            diag
            end_phase
            return
        fi
        # A device which came back is a new one to the host: what was done to
        # bind it to vfio-pci went with the old one.
        if bind_dev; then
            stage "host: the device which came back is bound to vfio-pci" ok
        else
            stage "host: the device which came back is bound to vfio-pci" FAILED
            end_phase
            return
        fi
        serial_mark
        if run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci of the device which came back" ok
        else
            stage "plugpci of the device which came back" FAILED
            diag
        fi
        $reads 2 "guest: takes the device which came back and reads from $from"
        $state hotplug,present,inuse
        check_log_quiet
    fi
    end_phase
    # For what comes after this phase.
    [ -e "/sys/bus/pci/devices/$HOSTADDR" ] || echo 1 > /sys/bus/pci/rescan
    bind_dev
}

# The VM starts without the device and gets it later; its state is saved
# while the slot is empty.
phase_startempty()
{
    PHASE=startempty
    reset_vm
    run modifyvm "$VM" --pci-attach "$HOSTADDR@0000:01:00.0" && ATTACHED=$HOSTADDR
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    local lay
    lay=$(vbm getextradata "$VM" "VBoxInternal2/HostPCILayout/$HOSTADDR" 2> /dev/null | sed -n 's/^Value: //p')
    echo "    layout stored by an earlier start with the device: ${lay:-none}"
    echo "+ unbinding $HOSTADDR from vfio-pci, the device is not to be had"
    unbind_dev
    if boot "a VM whose device is not there boots the guest"; then
        if serial_has 'GUEST: PRESENT'; then
            stage "guest: finds the slot empty" FAILED
        else
            stage "guest: finds the slot empty" ok
        fi
        check_pcistate hotplug,absent,unused
        if run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci of a device which is not bound to vfio-pci is refused" FAILED "it went through"
        else
            stage "plugpci of a device which is not bound to vfio-pci is refused" ok
        fi

        echo "+ binding $HOSTADDR to vfio-pci again"
        bind_dev || echo "    binding failed"
        serial_mark
        if run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci puts the device into the VM which started without it" ok
        else
            stage "plugpci puts the device into the VM which started without it" FAILED
            diag
        fi
        guest_reads 1 "guest: takes the device and reads from the drive"
        check_pcistate hotplug,present,inuse

        serial_mark
        if VBM_TIMEOUT=120 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=60000; then
            stage "unplugpci before the state is saved" ok
        else
            stage "unplugpci before the state is saved" FAILED
            diag
        fi
        wait_serial 'GUEST: GONE 1' 30 || echo "    the guest has not said that the drive is gone"
        if saved_state_round; then
            # The device is there when the state is loaded, so it is put into the
            # slot again without anybody asking: a device being plugged in for
            # the guest.
            guest_reads 2 "guest: gets the device back when the state is loaded with the device there, and reads from the drive"
            check_pcistate hotplug,present,inuse
            if run controlvm "$VM" plugpci "$HOSTADDR"; then
                stage "plugpci of the device which is in the VM already is refused" FAILED "it went through"
            else
                stage "plugpci of the device which is in the VM already is refused" ok
            fi

            # The same with the device not being there when the state is loaded.
            serial_mark
            if VBM_TIMEOUT=120 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=60000; then
                stage "unplugpci before the state is saved a second time" ok
            else
                stage "unplugpci before the state is saved a second time" FAILED
                diag
            fi
            wait_serial 'GUEST: GONE 2' 30 || echo "    the guest has not said that the drive is gone"
            echo "+ unbinding $HOSTADDR from vfio-pci, the device is not to be had"
            unbind_dev
            if saved_state_round; then
                if serial_has 'GUEST: PRESENT'; then
                    stage "guest: finds the slot empty when the state is loaded without the device" FAILED
                else
                    stage "guest: finds the slot empty when the state is loaded without the device" ok
                fi
                check_pcistate hotplug,absent,unused
                echo "+ binding $HOSTADDR to vfio-pci again"
                bind_dev || echo "    binding failed"
                serial_mark
                if run controlvm "$VM" plugpci "$HOSTADDR"; then
                    stage "plugpci after the state was loaded without the device" ok
                else
                    stage "plugpci after the state was loaded without the device" FAILED
                    diag
                fi
                guest_reads 3 "guest: takes the device after the state was loaded without it, and reads from the drive"
            fi
        fi
    fi
    end_phase
    bind_dev
}


# The device with a guest which does not use MSI or MSI-X (pci=nomsi on its
# kernel command line): its driver gets the INTx interrupt of the drive.  Every
# command to the drive ends with an interrupt, so a guest which has found the
# drive, read eight megabytes from it and written to it has had dozens.  The
# device passes each on as a pulse and asks the drive for the next.  Before it
# did, the first interrupt of a function was the last one and the interrupt
# line of the guest was left high: the kernel of a guest takes that for an
# interrupt nobody cares about and switches the line off, and its driver waits
# half a minute for every command.  A build without the change is told by a
# message the device has since, and the phase is left out there.
phase_intx()
{
    PHASE=intx
    reset_vm
    if [ ! -s "${GUEST_RAW_NOMSI:-}" ]; then
        stage "a guest which does not use MSI boots with the device passed through" skipped "there is no disk for such a guest"
        return
    fi
    if ! grep -a -q 'Asking the device for its next interrupt' "$VBOX_BIN/VBoxDD.so" 2> /dev/null; then
        stage "a guest which does not use MSI boots with the device passed through" skipped "this build passes the first INTx interrupt of a function on and no more"
        return
    fi
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    # The disk whose kernel is told not to use MSI, in the place of the usual one.
    if    ! run convertfromraw "$GUEST_RAW_NOMSI" "$SIM_WORK/guest-nomsi.vdi" --format VDI \
       || ! run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest-nomsi.vdi"; then
        stage "a guest which does not use MSI boots with the device passed through" FAILED "no disk"
        run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest.vdi"
        return
    fi
    run modifyvm "$VM" --pci-attach "$HOSTADDR@0000:01:00.0" && ATTACHED=$HOSTADDR
    if boot "a guest which does not use MSI boots with the device passed through"; then
        local irq bad
        guest_reads 1 "guest: reads from the drive, whose interrupt is INTx"
        wait_serial 'GUEST: (IRQ|NODRIVE) 1' 60
        serial_show 'GUEST: (pci: 0000:01|PRESENT|READ|IRQ|WROTE|WRITE-FAILED|NODRIVE)' 10
        irq=$(serial_new | grep -a 'GUEST: IRQ 1 ' | head -n 1 | sed -e 's/^.*lines: *//' | cut -c1-70)
        if serial_has 'GUEST: IRQ 1 .*IO-APIC[^|]*nvme' && ! serial_has 'GUEST: IRQ 1 .*PCI-MSI'; then
            stage "guest: the interrupt of the drive is at the I/O APIC, not MSI or MSI-X" ok "$irq"
        else
            stage "guest: the interrupt of the drive is at the I/O APIC, not MSI or MSI-X" FAILED "$irq"
        fi
        if serial_has 'GUEST: WROTE 1'; then
            stage "guest: writes to the drive" ok
        else
            stage "guest: writes to the drive" FAILED
        fi

        run controlvm "$VM" pause
        sleep 3
        run controlvm "$VM" resume
        serial_mark
        if wait_serial "GUEST: REREAD [0-9]+ md5=$EXPECT_MD5" 150; then
            stage "guest: reads from the drive after pause and resume" ok
        else
            stage "guest: reads from the drive after pause and resume" FAILED
            serial_show 'GUEST: REREAD' 5
            diag
        fi

        # A reset of the VM is a reset of the device, and INTx is set up anew.
        serial_mark
        run controlvm "$VM" reset
        if wait_serial 'GUEST: READY' "$BOOT_WAIT" 1; then
            stage "the guest boots again after the VM was reset" ok
            guest_reads 1 "guest: reads from the drive after the VM was reset"
        else
            stage "the guest boots again after the VM was reset" FAILED
            diag
        fi

        # What the kernel of the guest says of an interrupt line nobody answers
        # for, and of a command it had to look after because no interrupt came.
        bad=$(tr -d '\r' < "$SERIAL" 2> /dev/null | grep -a -c -E 'nobody cared|Disabling IRQ|completion polled|I/O [^ ]+ .*timeout')
        if [ "${bad:-0}" = 0 ]; then
            stage "guest: no interrupt of the drive went missing, and none came which nobody answered for" ok
        else
            stage "guest: no interrupt of the drive went missing, and none came which nobody answered for" FAILED "$bad line(s) of its kernel"
            tr -d '\r' < "$SERIAL" | grep -a -E 'nobody cared|Disabling IRQ|completion polled|timeout' | head -n 6 | cut -c1-200 | sed 's/^/    | /'
        fi
        grep -a -E 'VFIO#.*(next interrupt|passing on the interrupt)' "$VMDIR/Logs/VBox.log" | cut -c1-200 | head -n 4 | sed 's/^/    | /'
    fi
    end_phase
    settle
    run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest.vdi"
}


# A check of what the two changes to Main of 2026-11-25 and -26 are for.  A
# tree without them (MAIN_ENDS is no, told by its source in run.sh) is run all
# the same: what goes wrong there is told and not counted.
main_stage() # <what> <ok|FAILED|skipped> [<note>]
{
    if [ "$MAIN_ENDS" = no ] && [ "$2" = FAILED ]; then
        stage "$1" skipped "a tree without the change to Main for it: ${3:-}"
    else
        stage "$1" "$2" "${3:-}"
    fi
}

# Where those threads of a process are which are in the Console or in the
# session, for a process which does not end.
console_threads() # <pid>
{
    if ! command -v gdb > /dev/null 2>&1; then
        echo "    ---- no gdb here"
        return 0
    fi
    echo "    ---- the threads of process $1 which are in the Console or in the session:"
    timeout -k 5 120 gdb -batch -p "$1" -ex "thread apply all bt 16" 2>&1 \
        | grep -a -E '^Thread |^#[0-9]+ ' \
        | awk '/^Thread / { if (blk ~ /Console[:][:]|Session[:][:]/) printf "%s", blk; blk = "" }
               { blk = blk "    | " substr($0, 1, 170) "\n" }
               END { if (blk ~ /Console[:][:]|Session[:][:]/) printf "%s", blk }' \
        | head -n 120
}

# The VM process is told to end, as a frontend is when its user logs out or a
# service is stopped: it tries to save the state of a running VM, and where
# that is refused, as it is with a device of the host in the VM, it closes the
# session, which powers the VM down.  The process is to end, and VirtualBox is
# to know afterwards that the VM is off.  With "unplug" a command which takes
# the device out is waiting for the guest in that moment.
# With "paused" and "dump" there is no device of the host in the VM, see
# phase_endpaused.
frontend_told_to_end() # <plain|unplug|paused|dump>
{
    local pid i=0 st= what core= log a b tell=main_stage
    DUMP_HIT=
    log=$VMDIR/Logs/VBox.log
    case "$1" in
        unplug) what="while a device is being taken out" ;;
        paused) what="with the VM paused" ;;
        dump)   what="with the VM paused and its memory being written to a file" ;;
        *)      what="with a device of the host in the VM" ;;
    esac
    pid=$(pgrep -f "VBoxHeadless.*--comment $VM " | head -n 1)
    if [ -z "$pid" ]; then
        stage "the VM process ends when it is told to $what" skipped "no process of the VM was found"
        return 1
    fi
    if [ "$1" = unplug ]; then
        ( VBM_TIMEOUT=120 vbm controlvm "$VM" unplugpci "$HOSTADDR" --timeout=60000 > "$SIM_WORK/unplug-bg.out" 2>&1 ) &
        sleep 5
    fi
    if [ "$1" = dump ]; then
        # Somebody who holds on to the VM for a while: the memory of the guest
        # is written to a file through the API.  The VM process is told to end
        # as soon as the file is there.
        core=$SIM_WORK/guest-core
        rm -f "$core"
        ( VBM_TIMEOUT=180 vbm debugvm "$VM" dumpvmcore --filename="$core" > "$SIM_WORK/dump-bg.out" 2>&1 ) &
        while [ ! -e "$core" ] && [ "$i" -lt 1500 ]; do
            sleep 0.02
            i=$((i + 1))
        done
        if [ ! -e "$core" ]; then
            wait
            DUMP_HIT=yes
            stage "the VM process ends when it is told to $what" skipped "no file was written"
            head -n 4 "$SIM_WORK/dump-bg.out" 2> /dev/null | awk '{ print "    | " substr($0, 1, 200) }'
            return 1
        fi
        i=0
    fi
    echo "+ kill -TERM $pid (the VM process)"
    kill -TERM "$pid"
    while kill -0 "$pid" 2> /dev/null && [ "$i" -lt 90 ]; do
        sleep 1
        i=$((i + 1))
    done
    if kill -0 "$pid" 2> /dev/null; then
        main_stage "the VM process ends when it is told to $what" FAILED "it is still there after $i s"
        console_threads "$pid"
        kill -KILL "$pid"
        DUMP_HIT=yes
    elif [ "$1" = dump ]; then
        # Whether the memory was still being written when the session got
        # closed is in the log of the VM: the line of the Console about
        # powering the VM down from its uninit() comes before the line about
        # the file then.  Only such a round tells something.
        a=$(grep -a -n -m 1 'A request to power off the VM has been issued.*InUninit=1' "$log" 2> /dev/null | awk -F: '{ print $1 }')
        b=$(grep -a -n -m 1 'Successfully wrote guest core dump' "$log" 2> /dev/null | awk -F: '{ print $1 }')
        if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; then
            DUMP_HIT=yes
            stage "the VM process ends when it is told to $what" ok "$i s; the file was still being written when the session got closed"
        else
            DUMP_HIT=no
            tell=:
            echo "    the file was written before the session got closed (lines ${a:-none} and ${b:-none} of the log of the VM): this round tells nothing"
        fi
    else
        stage "the VM process ends when it is told to $what" ok "$i s"
    fi
    if [ "$1" = dump ] || [ "$1" = paused ]; then
        grep -a -E 'A request to power off the VM has been issued|guest core dump|Machine state changed to' "$log" 2> /dev/null \
            | tail -n 8 | awk '{ print "    | " substr($0, 1, 200) }'
    fi
    if [ "$1" = unplug ]; then
        wait
        head -n 4 "$SIM_WORK/unplug-bg.out" 2> /dev/null | sed -e 's/^\(.\{0,200\}\).*/    | \1/'
    fi
    if [ "$1" = dump ]; then
        wait
        head -n 4 "$SIM_WORK/dump-bg.out" 2> /dev/null | awk '{ print "    | " substr($0, 1, 200) }'
        ls -s "$core" 2> /dev/null | awk '{ print "    | the file has " $1 " blocks" }'
        rm -f "$core"
    fi
    # What VirtualBox makes of it: a minute for the VM to be off for it.
    i=0
    while [ "$i" -lt 60 ]; do
        st=$(VBM_TIMEOUT=20 vbm showvminfo "$VM" --machinereadable 2> /dev/null | sed -n 's/^VMState="\(.*\)"$/\1/p')
        case "$st" in poweroff|aborted) break ;; esac
        sleep 2
        i=$((i + 2))
    done
    case "$st" in
        poweroff|aborted)
            $tell "VirtualBox knows that the VM is off after its process was told to end $what" ok "$st, after $i s"
            return 0
            ;;
    esac
    $tell "VirtualBox knows that the VM is off after its process was told to end $what" FAILED "it tells '${st:-nothing}'"
    echo "    ---- what there is of VirtualBox:"
    pgrep -a -f 'VBox(SVC|Headless|XPCOMIPCD|Manage)' | sed -e 's/^\(.\{0,160\}\).*/    | \1/'
    echo "    ---- showvminfo, 20 seconds given:"
    VBM_TIMEOUT=20 vbm showvminfo "$VM" --machinereadable 2>&1 | grep -a -i -E '^VMState|^Session|error' | head -n 6 | sed -e 's/^\(.\{0,200\}\).*/    | \1/'
    if command -v gdb > /dev/null 2>&1; then
        echo "    ---- where the threads of VBoxSVC are:"
        timeout -k 5 90 gdb -batch -p "$(pgrep -x VBoxSVC | head -n 1)" -ex "thread apply all bt 14" 2>&1 \
            | grep -a -E '^Thread |^#[0-9]+ ' | sed -e 's/^\(.\{0,170\}\).*/    | \1/' | head -n 220
    else
        echo "    ---- no gdb here"
    fi
    # The service is ended, so that what follows has one which knows.
    pkill -x VBoxSVC 2> /dev/null
    i=0
    while pgrep -x VBoxSVC > /dev/null 2>&1 && [ "$i" -lt 20 ]; do
        sleep 1
        i=$((i + 1))
    done
    pkill -KILL -x VBoxSVC 2> /dev/null
    sleep 2
    echo "    the service was ended; the VM is in state '$(vmstate)' for the new one"
    return 1
}

# The same guest, which uses neither MSI nor MSI-X, with the device in the
# hot-plug capable slot.  The interrupt of the drive is INTx and goes through
# the port of the slot, and the port has no MSI for telling the guest of its
# slot either: by a decision of 2026-11-12 it tells of the slot by MSI only,
# so this guest is not told and does not let go of the device when asked.
# A snapshot for which the VM keeps running is refused, which is where a VM
# could stand still: an INTx interrupt is work for an EMT, and the thread
# which refuses waits for an EMT.  And as this guest keeps whoever takes the
# device out waiting, the frontend is told to end in that moment.
phase_intxslot()
{
    PHASE=intxslot
    local what="a guest which does not use MSI boots with the device in the hot-plug capable slot"
    reset_vm
    if [ ! -s "${GUEST_RAW_NOMSI:-}" ]; then
        stage "$what" skipped "there is no disk for such a guest"
        return
    fi
    if ! grep -a -q 'Asking the device for its next interrupt' "$VBOX_BIN/VBoxDD.so" 2> /dev/null; then
        stage "$what" skipped "this build passes the first INTx interrupt of a function on and no more"
        return
    fi
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    [ -s "$SIM_WORK/guest-nomsi.vdi" ] || run convertfromraw "$GUEST_RAW_NOMSI" "$SIM_WORK/guest-nomsi.vdi" --format VDI
    if ! run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest-nomsi.vdi"; then
        stage "$what" FAILED "no disk"
        run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest.vdi"
        return
    fi
    run modifyvm "$VM" --pci-attach "$HOSTADDR@0000:01:00.0" && ATTACHED=$HOSTADDR
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    if boot "$what"; then
        local irq bad t0
        guest_reads 1 "guest: reads from the drive in the slot, whose interrupt is INTx"
        wait_serial 'GUEST: (IRQ|NODRIVE) 1' 60
        serial_show 'GUEST: (pci: 0000:01|PRESENT|READ|IRQ|NODRIVE)|pciehp' 12
        irq=$(serial_new | grep -a 'GUEST: IRQ 1 ' | head -n 1 | sed -e 's/^.*lines: *//' -e 's/^\(.\{0,70\}\).*/\1/')
        if serial_has 'GUEST: IRQ 1 .*IO-APIC[^|]*nvme' && ! serial_has 'GUEST: IRQ 1 .*PCI-MSI'; then
            stage "guest: the interrupt of the drive in the slot is at the I/O APIC, not MSI or MSI-X" ok "$irq"
        else
            stage "guest: the interrupt of the drive in the slot is at the I/O APIC, not MSI or MSI-X" FAILED "$irq"
        fi
        check_pcistate hotplug,present,inuse

        # The live snapshot, as in the phase with the slot.
        if VBM_TIMEOUT=120 run snapshot "$VM" take refused-live --live; then
            stage "a live snapshot with the device in the slot is refused" FAILED "it was taken"
        else
            stage "a live snapshot with the device in the slot is refused" ok
        fi
        sleep 3
        [ "$(vmstate)" = paused ] && run controlvm "$VM" resume
        guest_goes_on "the VM runs on after the live snapshot was refused" 60

        # The port tells of its slot by MSI only, see the decisions in
        # STATUS.md, so this guest does not hear that it is asked for the
        # device and does not let go of it.  The command says so when the
        # time the guest was given is over, and the device stays where it is.
        t0=$(date +%s)
        what="unplugpci: a guest which is not told of its slot does not let go, and the command says so"
        if VBM_TIMEOUT=120 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=15000; then
            stage "$what" FAILED "it tells that the device was taken out"
        else
            stage "$what" ok "$(( $(date +%s) - t0 )) s"
        fi
        check_pcistate hotplug,present,inuse
        guest_goes_on "the VM runs on with the device after that" 60

        bad=$(tr -d '\r' < "$SERIAL" 2> /dev/null | grep -a -c -E 'nobody cared|Disabling IRQ|completion polled|I/O [^ ]+ .*timeout')
        if [ "${bad:-0}" = 0 ]; then
            stage "guest: no interrupt of the drive went missing, and none came which nobody answered for" ok
        else
            stage "guest: no interrupt of the drive went missing, and none came which nobody answered for" FAILED "$bad line(s) of its kernel"
            tr -d '\r' < "$SERIAL" | grep -a -E 'nobody cared|Disabling IRQ|completion polled|timeout' | head -n 6 | sed -e 's/^\(.\{0,200\}\).*/    | \1/'
        fi
        check_log_quiet

        # Last, as the VM is gone after each: the VM process is told to end,
        # once with nothing else going on and once while a device is being
        # taken out.  The method of Main which takes the device out holds on
        # to the VM for as long as the guest is given, and until late on
        # 2026-11-25 what waits for the VM to be let go of could wait for good
        # then: the VM process did not end.
        frontend_told_to_end plain
        if boot "the guest boots once more with the device in the slot"; then
            guest_reads 1 "guest: reads from the drive after that start"
            frontend_told_to_end unplug
        fi
    fi
    end_phase
    if vbm showvminfo "$VM" --machinereadable 2> /dev/null | grep -a -q '^SnapshotName'; then
        settle
        VBM_TIMEOUT=300 run snapshot "$VM" delete refused-live
    fi
    settle
    run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest.vdi"
}


# A VM with 3D acceleration.  Nothing of the passthrough is in it: the
# graphics adapter of the VM gets the backend which renders with the graphics
# card of the host, or with what the host has in the place of one (a runner
# has a renderer in software), and the guest has the driver for the adapter.
# With the adapter in this mode that driver keeps what is on the screen in
# objects of guest memory, which the host reads from and writes to at the
# guest's word.  Nobody looks at the picture.  The phase is about a guest
# getting through that, and about the host having done it.  A build without
# 3D acceleration does not take the setting, and the phase is left out there.
phase_gfx3d()
{
    PHASE=gfx3d
    local what="a VM with 3D acceleration boots the guest" k n
    reset_vm
    if [ ! -s "${GUEST_RAW_3D:-}" ]; then
        stage "$what" skipped "there is no disk for such a guest"
        return
    fi
    if    ! run convertfromraw "$GUEST_RAW_3D" "$SIM_WORK/guest-3d.vdi" --format VDI \
       || ! run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest-3d.vdi"; then
        stage "$what" FAILED "no disk"
        run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest.vdi"
        return
    fi
    if ! run modifyvm "$VM" --graphicscontroller vmsvga --vram 128 --accelerate-3d on; then
        stage "$what" skipped "this build takes no 3D acceleration for a VM"
        run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest.vdi"
        return
    fi
    if boot "$what"; then
        wait_serial 'GUEST: GFX done' 180
        serial_show 'GUEST: GFX' 60
        if serial_has 'GUEST: GFX card0 driver vmwgfx'; then
            stage "guest: its driver has the graphics adapter" ok
        else
            stage "guest: its driver has the graphics adapter" FAILED "$(serial_new | grep -a 'GUEST: GFX card0' | head -n 1 | cut -c1-120)"
        fi
        # What the driver says the adapter can do: with 3D acceleration it is
        # more than what an adapter without it offers ("Legacy").
        k=$(serial_new | grep -a -i 'GUEST: GFX k: .*Available shader model' | head -n 1 | sed -e 's/^.*Available shader model: *//' | cut -c1-60)
        case "$k" in
            '')        stage "guest: the adapter offers it 3D acceleration" FAILED "its driver does not tell" ;;
            Legacy*)   stage "guest: the adapter offers it 3D acceleration" FAILED "$k" ;;
            *)         stage "guest: the adapter offers it 3D acceleration" ok "$k" ;;
        esac
        if serial_has 'GUEST: GFX wrote [0-9]+ lines'; then
            stage "guest: writes some hundred screens of text" ok
        else
            stage "guest: writes some hundred screens of text" FAILED "$(serial_new | grep -a 'GUEST: GFX no screen' | head -n 1 | cut -c1-80)"
        fi
        if serial_has 'GUEST: GFX trouble'; then
            stage "guest: its driver tells of no trouble with the adapter" FAILED "$(serial_new | grep -a -c 'GUEST: GFX trouble') line(s)"
        else
            stage "guest: its driver tells of no trouble with the adapter" ok
        fi
        echo "    what the adapter has counted (what is not zero, without the registers):"
        VBM_TIMEOUT=60 vbm debugvm "$VM" statistics --pattern '/Devices/vga/VMSVGA/*' 2>&1 \
            | grep -a -v -E '/VMSVGA/Reg/| c="0"| cPeriods="0"| val="0"' | sed -e 's/ unit="[^"]*"//' -e 's/ vis="[^"]*"//' | cut -c1-160 | head -n 40 | sed 's/^/    | /'
        # What the guest wrote is in guest memory, and the host has it from
        # there: a picture of the screen as the host has it.  A screen with
        # nothing on it makes a file of a few kilobytes, one full of text
        # several times that.
        rm -f "$SIM_OUT/screen-gfx3d.png"
        run controlvm "$VM" screenshotpng "$SIM_OUT/screen-gfx3d.png"
        n=$(stat -c %s "$SIM_OUT/screen-gfx3d.png" 2> /dev/null)
        if [ "${n:-0}" -gt 12000 ]; then
            stage "the screen of the VM, as the host has it, has what the guest wrote" ok "a picture of $n bytes"
        else
            stage "the screen of the VM, as the host has it, has what the guest wrote" FAILED "a picture of ${n:-0} bytes"
        fi
        guest_goes_on "the guest keeps running"
        echo "    what the log of the VM says of 3D:"
        grep -a -i -E 'VMSVGA3d|vmsvga.*3d|dxvk|D3D11|vulkan|3D ' "$VMDIR/Logs/VBox.log" | grep -a -v -E 'stub|^[0-9:. ]*Vulkan 1\.[0-9]$' | cut -c1-200 | head -n 40 | sed 's/^/    | /'
    else
        echo "    what the log of the VM says of 3D:"
        grep -a -i -E 'VMSVGA3d|vmsvga.*3d|dxvk|D3D11|vulkan|3D ' "$VMDIR/Logs/VBox.log" 2> /dev/null | grep -a -v -E 'stub|^[0-9:. ]*Vulkan 1\.[0-9]$' | cut -c1-200 | head -n 40 | sed 's/^/    | /'
    fi
    end_phase
    settle
    run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/guest.vdi"
    run modifyvm "$VM" --accelerate-3d off --vram 16
}


# The phases below are for a device of the host with more than one function:
# two NVMe controllers at one address, each with a drive of its own, which the
# host has in one IOMMU group (run.sh, l1.sh).  That is what a graphics card
# with its audio function is.  Each function is attached to the VM by itself,
# at its function number of guest device 01:00, and the device they make goes
# into the slot and out of it as one.  The guest tells of the second function
# in lines of their own (READ2, WROTE2, REREAD2), and says GONE when neither
# controller is there any more.

mf_wanted()
{
    [ "$HOSTFUNCS" != "$HOSTADDR" ] && return 0
    stage "the device of the host has more than one function" skipped "it has one"
    return 1
}

attach_funcs()
{
    local a
    ATTACHED=
    for a in $HOSTFUNCS; do
        run modifyvm "$VM" --pci-attach "$a@0000:01:00.${a##*.}" && ATTACHED="$ATTACHED $a"
    done
}

guest_reads2() # <round> <what> [<seconds>]
{
    if wait_serial "GUEST: READ2 $1 md5=$EXPECT_MD5B" "${3:-180}"; then
        stage "$2" ok
        return 0
    fi
    stage "$2" FAILED "last: $(serial_new | grep -a -E 'GUEST: (READ2|NODRIVE2|PRESENT2|GONE)' | tail -n 1 | cut -c1-70)"
    diag
    return 1
}

# The guest has read from both drives in the round given.
guest_reads_both() # <round> <what>
{
    if    wait_serial "GUEST: READ $1 md5=$EXPECT_MD5" 180 \
       && wait_serial "GUEST: READ2 $1 md5=$EXPECT_MD5B" 180; then
        stage "$2" ok
        return 0
    fi
    stage "$2" FAILED "last: $(serial_new | grep -a -E 'GUEST: (READ2?|NODRIVE2?|PRESENT2?|GONE) ' | tail -n 2 | cut -c1-70 | tr '\n' '|')"
    diag
    return 1
}

# What showvminfo tells of every function of the device: each has a line and
# all of them say the same, a device being all of its functions.
check_pcistate_all() # <hotplug|fixed>,<present|absent>,<inuse|unused>
{
    local all want=0 a
    for a in $HOSTFUNCS; do
        want=$((want + 1))
    done
    all=$(vbm showvminfo "$VM" --machinereadable 2> /dev/null | sed -n 's/^AttachedHostPCIState=[^,]*,\(.*\)$/\1/p')
    if [ "$(echo "$all" | grep -c -x "$1")" = "$want" ] && [ "$(echo "$all" | grep -c .)" = "$want" ]; then
        stage "showvminfo tells $1 for each function" ok
    else
        stage "showvminfo tells $1 for each function" FAILED "it tells '$(echo "$all" | tr '\n' ' ')'"
    fi
}

# Both functions passed through, there when the VM starts, no hot-plug.
phase_mfpassthrough()
{
    PHASE=mfpassthrough
    reset_vm
    mf_wanted || return
    bind_dev || { stage "the functions of the device are bound to vfio-pci" FAILED; return; }
    attach_funcs
    if boot "a VM with both functions of the device passed through boots the guest"; then
        guest_reads 1 "guest: reads from the drive of the first function"
        guest_reads2 1 "guest: reads from the drive of the second function"
        wait_serial 'GUEST: (WROTE2|WRITE2-FAILED|NODRIVE2) 1' 60
        serial_show 'GUEST: (pci: 0000:01|PRESENT|READ|IRQ|WROTE|WRITE|NODRIVE)' 14
        grep -a 'VFIO#' "$VMDIR/Logs/VBox.log" | cut -c1-200 | head -n 40 | sed 's/^/    | /'
        if serial_has 'GUEST: WROTE 1' && serial_has 'GUEST: WROTE2 1'; then
            stage "guest: writes to both drives" ok
        else
            stage "guest: writes to both drives" FAILED
        fi
        check_pcistate_all fixed,present,inuse

        run controlvm "$VM" pause
        sleep 3
        run controlvm "$VM" resume
        serial_mark
        if    wait_serial "GUEST: REREAD [0-9]+ md5=$EXPECT_MD5" 150 \
           && wait_serial "GUEST: REREAD2 [0-9]+ md5=$EXPECT_MD5B" 60; then
            stage "guest: reads from both drives after pause and resume" ok
        else
            stage "guest: reads from both drives after pause and resume" FAILED
            serial_show 'GUEST: REREAD' 6
            diag
        fi

        VBM_TIMEOUT=120 run controlvm "$VM" savestate
        if [ $? -eq 0 ]; then
            stage "saving the state with the device in the VM is refused" FAILED "it was saved"
        else
            stage "saving the state with the device in the VM is refused" ok
            sleep 3
            [ "$(vmstate)" = paused ] && run controlvm "$VM" resume
            guest_goes_on "the VM runs on after that" 60
        fi
    fi
    end_phase
}

# The device with its two functions in the hot-plug capable slot: taken out
# and put in while the VM runs, as one device, whichever of its functions the
# command names.
phase_mfhotplug()
{
    PHASE=mfhotplug
    reset_vm
    mf_wanted || return
    bind_dev || { stage "the functions of the device are bound to vfio-pci" FAILED; return; }
    local second=${HOSTFUNCS##* } a lay stored=0 want=0 t0
    attach_funcs
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    if boot "a VM with the device of two functions in the hot-plug capable slot boots the guest"; then
        guest_slot_checks
        guest_reads_both 1 "guest: reads from both drives of the device in the slot"
        check_pcistate_all hotplug,present,inuse
        for a in $HOSTFUNCS; do
            want=$((want + 1))
            lay=$(vbm getextradata "$VM" "VBoxInternal2/HostPCILayout/$a" 2> /dev/null | sed -n 's/^Value: //p')
            echo "    layout stored for $a: ${lay:-none}"
            [ -n "$lay" ] && stored=$((stored + 1))
        done
        if [ "$stored" = "$want" ]; then
            stage "the layout of each function is stored for a start without the device" ok
        else
            stage "the layout of each function is stored for a start without the device" FAILED "$stored of $want"
        fi

        serial_mark
        t0=$(date +%s)
        if VBM_TIMEOUT=120 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=60000; then
            stage "unplugpci: the guest is asked and lets go of the device" ok "$(( $(date +%s) - t0 )) s"
        else
            stage "unplugpci: the guest is asked and lets go of the device" FAILED
            diag
        fi
        if wait_serial 'GUEST: GONE 1' 30; then
            stage "guest: both drives are gone" ok
        else
            stage "guest: both drives are gone" FAILED
        fi
        serial_show 'pciehp|GUEST: GONE' 10
        check_pcistate_all hotplug,absent,unused

        # The command is given the second function this time: the device is
        # all of its functions, and any of them names it.
        serial_mark
        if run controlvm "$VM" plugpci "$second"; then
            stage "plugpci, naming the second function, puts the device into the slot" ok
        else
            stage "plugpci, naming the second function, puts the device into the slot" FAILED
            diag
        fi
        guest_reads_both 2 "guest: takes the device in the slot and reads from both drives"
        serial_show 'pciehp|GUEST: (PRESENT|READ)' 12
        check_pcistate_all hotplug,present,inuse

        serial_mark
        if VBM_TIMEOUT=60 run controlvm "$VM" unplugpci "$second" --timeout=1000 --force; then
            stage "unplugpci --force takes the device away from a guest which uses it" ok
        else
            stage "unplugpci --force takes the device away from a guest which uses it" FAILED
            diag
        fi
        if wait_serial 'GUEST: GONE 2' 150; then
            stage "guest: notices that both drives are gone" ok
        else
            stage "guest: notices that both drives are gone" FAILED
        fi
        guest_goes_on "the VM runs on after that" 150

        serial_mark
        if run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci after the forced removal" ok
        else
            stage "plugpci after the forced removal" FAILED
            diag
        fi
        guest_reads_both 3 "guest: takes the device again and reads from both drives"
        check_log_quiet
    fi
    end_phase
}

# The VM starts without the device of two functions, built from the layouts
# stored by the phase before, and gets the device later; a device of which one
# function is not to be had is not put in at all.
phase_mfstartempty()
{
    PHASE=mfstartempty
    reset_vm
    mf_wanted || return
    local second=${HOSTFUNCS##* }
    attach_funcs
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    echo "+ unbinding $HOSTFUNCS from vfio-pci, the device is not to be had"
    unbind_dev
    if boot "a VM whose device of two functions is not there boots the guest"; then
        if serial_has 'GUEST: PRESENT'; then
            stage "guest: finds the slot empty" FAILED
        else
            stage "guest: finds the slot empty" ok
        fi
        check_pcistate_all hotplug,absent,unused

        echo "+ binding $HOSTADDR to vfio-pci, and not $second"
        bind_one "$HOSTADDR" || echo "    binding failed"
        serial_mark
        if run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci of a device of which one function is not bound to vfio-pci is refused" FAILED "it went through"
        else
            stage "plugpci of a device of which one function is not bound to vfio-pci is refused" ok
        fi
        grep -a -E 'VFIO#.*(No device was put|put into the slot)' "$VMDIR/Logs/VBox.log" | cut -c1-200 | tail -n 3 | sed 's/^/    | /'
        sleep 8
        if serial_has 'GUEST: PRESENT'; then
            stage "guest: has nothing of the device after that" FAILED
        else
            stage "guest: has nothing of the device after that" ok
        fi
        check_pcistate_all hotplug,absent,unused
        guest_goes_on "the VM runs on after that"

        echo "+ binding $second to vfio-pci as well"
        bind_dev || echo "    binding failed"
        serial_mark
        if run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci puts the device into the VM which started without it" ok
        else
            stage "plugpci puts the device into the VM which started without it" FAILED
            diag
        fi
        guest_reads_both 1 "guest: takes the device and reads from both drives"
        check_pcistate_all hotplug,present,inuse

        serial_mark
        if VBM_TIMEOUT=120 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=60000; then
            stage "unplugpci before the state is saved" ok
        else
            stage "unplugpci before the state is saved" FAILED
            diag
        fi
        wait_serial 'GUEST: GONE 1' 30 || echo "    the guest has not said that the drives are gone"
        if saved_state_round; then
            guest_reads_both 2 "guest: gets the device back when the state is loaded with the device there, and reads from both drives"
            check_pcistate_all hotplug,present,inuse
        fi
    fi
    end_phase
    bind_dev
}

# The host wants the device of two functions back while the guest uses it:
# both functions are taken from the host at once, as when the device is
# unplugged in an orderly way.
phase_mfhostremove()
{
    PHASE=mfhostremove
    reset_vm
    mf_wanted || return
    bind_dev || { stage "the functions of the device are bound to vfio-pci" FAILED; return; }
    attach_funcs
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    if boot "a VM with the device of two functions in the hot-plug capable slot boots the guest"; then
        guest_reads_both 1 "guest: reads from both drives of the device in the slot"
        serial_mark
        # The writes return when the functions were let go of.  They can not be
        # interrupted while they wait, so they are left behind if that takes
        # too long.
        local t0 a i=0 pids= p waiting
        t0=$(date +%s)
        for a in $HOSTFUNCS; do
            echo "+ echo 1 > /sys/bus/pci/devices/$a/remove"
            ( echo 1 > "/sys/bus/pci/devices/$a/remove" ) &
            pids="$pids $!"
        done
        while [ "$i" -lt 120 ]; do
            waiting=no
            for p in $pids; do
                kill -0 "$p" 2> /dev/null && waiting=yes
            done
            [ "$waiting" = no ] && break
            sleep 1
            i=$((i + 1))
        done
        if [ "$waiting" = yes ]; then
            stage "host: gets both functions back when it asks for them" FAILED "kept waiting"
            diag
        else
            stage "host: gets both functions back when it asks for them" ok "$(( $(date +%s) - t0 )) s"
        fi
        if wait_serial 'GUEST: GONE 1' 60; then
            stage "guest: was asked and let go, both drives are gone" ok
        else
            stage "guest: was asked and let go, both drives are gone" FAILED
        fi
        serial_show 'pciehp|GUEST: GONE' 10
        guest_goes_on "the VM runs on after that" 90
        check_pcistate_all hotplug,absent,unused

        echo "+ echo 1 > /sys/bus/pci/rescan"
        echo 1 > /sys/bus/pci/rescan
        if bind_dev; then
            serial_mark
            if run controlvm "$VM" plugpci "$HOSTADDR"; then
                stage "plugpci of the device which came back" ok
            else
                stage "plugpci of the device which came back" FAILED
                diag
            fi
            guest_reads_both 2 "guest: takes the device which came back and reads from both drives"
        else
            stage "host: the device is back after a rescan" FAILED
        fi
        check_log_quiet
    fi
    end_phase
    # For the phases after this one.
    [ -e "/sys/bus/pci/devices/$HOSTADDR" ] || echo 1 > /sys/bus/pci/rescan
    bind_dev
}

# A device whose functions the host cannot reset by themselves, as it is with
# many graphics cards.  A reset of the device is then a reset of the bus the
# functions are on, which the host does for the VM when everything on that
# bus is the VM's.  The emulated controllers can each be reset by themselves,
# so the host is told to forget how (reset_method in sysfs), and told to
# remember afterwards.
forget_resets()
{
    local a set=yes
    for a in $HOSTFUNCS; do
        # A function which the host has no reset for to begin with has no such
        # file: nothing to forget, and it is as wanted.
        if [ ! -e "/sys/bus/pci/devices/$a/reset_method" ]; then
            echo "+ the host has no reset for $a by itself to begin with"
            continue
        fi
        echo "+ the host forgets how to reset $a by itself (it knew: $(cat "/sys/bus/pci/devices/$a/reset_method" 2>&1))"
        echo "" > "/sys/bus/pci/devices/$a/reset_method" 2> /dev/null || set=no
        [ -z "$(cat "/sys/bus/pci/devices/$a/reset_method" 2> /dev/null)" ] || set=no
    done
    if [ "$set" != yes ]; then
        remember_resets
        return 1
    fi
    RESETS_FORGOTTEN=yes
}

remember_resets()
{
    local a
    for a in $HOSTFUNCS; do
        [ -e "/sys/bus/pci/devices/$a/reset_method" ] || continue
        echo default > "/sys/bus/pci/devices/$a/reset_method" 2> /dev/null
        echo "+ the host knows how to reset $a: $(cat "/sys/bus/pci/devices/$a/reset_method" 2>&1)"
    done
    RESETS_FORGOTTEN=no
}

# What the log of the VM has of the resets after forget_resets: the functions
# refusing a reset of their own, which is what leads to the reset of the bus,
# and no word of a reset which did not work.
check_bus_resets()
{
    local log="$VMDIR/Logs/VBox.log" n
    n=$(grep -a -c -E "The function can't be reset on its own \(" "$log" 2> /dev/null)
    grep -a -E "can't be reset on its own \(|could not be reset|is not reset now|refuses a reset" "$log" | cut -c1-200 | head -n 6 | sed 's/^/    | /'
    if [ "${n:-0}" -gt 0 ] && ! grep -a -q -E 'could not be reset|Failed to update command register|Failed to clear the command register' "$log"; then
        stage "the device was reset by a reset of its bus, and the log has no word of a reset which failed" ok "$n times a function had no reset of its own"
    else
        stage "the device was reset by a reset of its bus, and the log has no word of a reset which failed" FAILED "${n:-0} times a function had no reset of its own"
        grep -a -E 'could not be reset|Failed to (update|clear)' "$log" | cut -c1-200 | head -n 6 | sed 's/^/    | /'
    fi
}

# The device of two functions when the host has no reset for a function by
# itself: the VM is reset, and the device is taken out of the slot and put in.
phase_mfbusreset()
{
    PHASE=mfbusreset
    reset_vm
    mf_wanted || return
    bind_dev || { stage "the functions of the device are bound to vfio-pci" FAILED; return; }
    local a log notes=0 want=0
    for a in $HOSTFUNCS; do
        want=$((want + 1))
    done
    if ! forget_resets; then
        stage "the host has no reset for a function by itself" skipped "its kernel does not let that be set"
        return
    fi
    attach_funcs
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    if boot "a VM with the device whose functions the host cannot reset by themselves boots the guest"; then
        log="$VMDIR/Logs/VBox.log"
        guest_reads_both 1 "guest: reads from both drives of the device"
        for a in $HOSTFUNCS; do
            grep -a -q "VFIO#0\.${a##*.}: The function can't be reset on its own, a reset is going to affect everything on its bus" "$log" \
                && notes=$((notes + 1))
        done
        if [ "$notes" = "$want" ]; then
            stage "the device tells of each function that it cannot be reset on its own" ok
        else
            stage "the device tells of each function that it cannot be reset on its own" FAILED "$notes of $want"
            grep -a -E 'VFIO#.*reset' "$log" | cut -c1-200 | head -n 8 | sed 's/^/    | /'
        fi

        # A reset of the VM is a reset of the device.
        serial_mark
        run controlvm "$VM" reset
        if wait_serial 'GUEST: READY' "$BOOT_WAIT" 1; then
            stage "the guest boots again after the VM was reset" ok
            guest_reads_both 1 "guest: reads from both drives after the VM was reset"
        else
            stage "the guest boots again after the VM was reset" FAILED
            diag
        fi

        # The guest switching the slot off is a reset of the device as well.
        serial_mark
        if VBM_TIMEOUT=120 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=60000; then
            stage "unplugpci: the guest is asked and lets go of the device" ok
        else
            stage "unplugpci: the guest is asked and lets go of the device" FAILED
            diag
        fi
        wait_serial 'GUEST: GONE 1' 30 || echo "    the guest has not said that the drives are gone"
        serial_mark
        if run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci puts the device into the slot again" ok
        else
            stage "plugpci puts the device into the slot again" FAILED
            diag
        fi
        guest_reads_both 2 "guest: takes the device and reads from both drives"
        check_bus_resets
    fi
    end_phase
    remember_resets
}


# The variable through which the firmware of the VM is asked for things at its
# next start, as hexadecimal bytes; "none" if the VM has no such variable.
os_indications()
{
    rm -f "$SIM_WORK/osind.bin"
    if    vbm modifynvram "$VM" queryvar --name OsIndications --filename "$SIM_WORK/osind.bin" > /dev/null 2>&1 \
       && [ -e "$SIM_WORK/osind.bin" ]; then
        od -A n -t x1 "$SIM_WORK/osind.bin" | tr -d ' \n'
        echo
    else
        echo none
    fi
}

# The VM is asked to enter the setup of its firmware at its next start
# (VBoxManage modifynvram firmwaresetup): the firmware does that instead of
# starting the guest, once, and takes the request back.  Nothing of the
# passthrough work is in this.  It is here for the command, which a branch
# has or has not, and for the firmware going by what the command writes.
phase_fwsetup()
{
    local n t0 took quiet=90
    PHASE=fwsetup
    reset_vm
    echo "    OsIndications before: $(os_indications)"
    if ! run modifynvram "$VM" firmwaresetup --enable; then
        # The variable store of the VM is there once the VM has run.  Where
        # this phase is the first one, it is made.
        run modifynvram "$VM" inituefivarstore
        if ! run modifynvram "$VM" firmwaresetup --enable; then
            stage "the setup of the firmware is asked for" FAILED
            end_phase
            return
        fi
    fi
    n=$(os_indications)
    case "$n" in
        ?[13579bdf]??????????????)
            stage "the setup of the firmware is asked for" ok "OsIndications is $n" ;;
        *)
            stage "the setup of the firmware is asked for" FAILED "OsIndications is $n"
            end_phase
            return ;;
    esac

    # The start with the request: no guest comes up, and the VM runs on.
    if ! start_vm; then
        stage "the VM which was asked for the setup of its firmware starts" FAILED
        diag
        end_phase
        return
    fi
    if wait_serial 'GUEST: READY' "$quiet" 1; then
        stage "the VM which was asked for the setup of its firmware does not start the guest" FAILED "the guest came up"
    elif [ "$(vmstate)" != running ]; then
        stage "the VM which was asked for the setup of its firmware does not start the guest" FAILED "the VM is in state '$(vmstate)'"
    else
        stage "the VM which was asked for the setup of its firmware does not start the guest" ok "none in $quiet seconds, and the VM runs"
    fi
    # What the screen shows is kept for a person to look at; nothing here reads it.
    rm -f "$SIM_OUT/screen-fwsetup.png"
    run controlvm "$VM" screenshotpng "$SIM_OUT/screen-fwsetup.png"
    echo "    the screen of the VM is kept, a picture of $(stat -c %s "$SIM_OUT/screen-fwsetup.png" 2> /dev/null || echo 0) bytes"
    keep_logs -setup
    power_off
    settle
    n=$(os_indications)
    case "$n" in
        ?[02468ace]??????????????)
            stage "the firmware has taken the request back" ok "OsIndications is $n" ;;
        *)
            stage "the firmware has taken the request back" FAILED "OsIndications is $n" ;;
    esac

    # The start after it is an ordinary one.
    t0=$(date +%s)
    if boot "the next start boots the guest"; then
        took=$(( $(date +%s) - t0 ))
        if [ "$took" -ge "$quiet" ]; then
            echo "    that took $took seconds: no guest in $quiet seconds tells nothing on a machine as slow as this"
        else
            echo "    in $took seconds"
        fi
    fi
    keep_logs -next
    reset_vm

    # A request which is taken back before the VM starts is none.
    if run modifynvram "$VM" firmwaresetup --enable && run modifynvram "$VM" firmwaresetup --disable; then
        echo "    OsIndications after asking and taking it back: $(os_indications)"
        boot "a request which was taken back leaves the start as it is"
    else
        stage "a request which was taken back leaves the start as it is" FAILED "the command failed"
    fi
    end_phase
}


# The KVM paravirtualization interface without its clock: the configuration
# value /GIM/KVM/Clock, set as extra data of the VM.  Nothing of the
# passthrough work is in this.  It is here for the value, which a branch has
# or has not.  What the guest does about its clock its kernel tells on the
# serial port, and what the VM did for that clock is in the log of the VM.
clock_in_log() # how often the VM has set up the clock of the interface
{
    local n
    n=$(grep -a -c 'GIM: KVM: VCPU.*Enabled system-time struct' "$VMDIR/Logs/VBox.log" 2> /dev/null)
    echo "${n:-0}"
}

clock_left_out() # whether the VM says that it does not offer the clock
{
    grep -a -q 'GIM: KVM: Not offering the paravirtualized clock' "$VMDIR/Logs/VBox.log" 2> /dev/null
}

alive_up() # the uptime the guest told with its last "ALIVE", in whole seconds
{
    serial_new | grep -a 'GUEST: ALIVE ' | tail -n 1 \
        | awk '{ for (i = 1; i <= NF; i++) if (index($i, "up=") == 1) { split(substr($i, 4), a, "."); print a[1] } }'
}

# How the time of the guest goes.  The guest tells its uptime with every
# "ALIVE", ten seconds apart by its own clock; two of them are waited for and
# the time between them is taken here as well.  A guest with the clock of the
# interface has its time from the processor.  A guest without it which cannot
# find out how fast the TSC counts has its time from the HPET, which is a
# device of the VM, and that time falls behind where the timers of the VM are
# served late.  The KVM backend of upstream enters the guest without the
# deadline of the next timer, so they are served by a tick of 10 ms there and
# such a guest is slow; with a backend which sets a timer for the deadline it
# has to keep time.
guest_pace() # <what> <whether a slow guest counts: yes|no>
{
    local t1 t2 u1 u2 real guest
    serial_mark
    if ! wait_serial "$ALIVE_RE" 240; then
        stage "$1" FAILED "nothing from the guest in 240 seconds"
        diag
        return 1
    fi
    t1=$(date +%s)
    u1=$(alive_up)
    serial_mark
    if ! wait_serial "$ALIVE_RE" 240; then
        stage "$1" FAILED "nothing more from the guest in 240 seconds"
        diag
        return 1
    fi
    t2=$(date +%s)
    u2=$(alive_up)
    real=$((t2 - t1))
    guest=$(( ${u2:-0} - ${u1:-0} ))
    if [ "$guest" -le 0 ]; then
        stage "$1" FAILED "the uptime the guest tells does not go on: ${u1:-?}, then ${u2:-?}"
    elif [ "$real" -le $(( 2 * guest + 3 )) ]; then
        stage "$1" ok "$guest s of the guest in $real s"
    elif [ "$2" = yes ]; then
        stage "$1" FAILED "$guest s of the guest took $real s"
    else
        stage "$1" skipped "a tree whose KVM backend enters the guest without the deadline of the next timer: $guest s of the guest took $real s"
    fi
}

# The state of the guest which runs is saved and loaded by a VM which is
# configured the other way.  The guest goes on with what it has, and the VM
# says that the state is unlike its configuration.
clock_state_round() # <the extra data key> <its value for the load, or nothing> <offered|did not offer> <suffix for the logs> <whether a slow guest counts> <what>
{
    VBM_TIMEOUT=300 run controlvm "$VM" savestate
    if [ $? -ne 0 ] || ! wait_state saved 60; then
        stage "$6" FAILED "the state is not saved, the VM is in state '$(vmstate)'"
        diag
        return 1
    fi
    if [ -n "$2" ]; then
        run setextradata "$VM" "$1" "$2"
    else
        run setextradata "$VM" "$1"
    fi
    if ! start_vm; then
        stage "$6" FAILED "the VM does not start"
        diag
    elif ! wait_serial "$ALIVE_RE" 240; then
        stage "$6" FAILED "nothing from the guest"
        diag
    elif serial_has "$UP_RE"; then
        stage "$6" FAILED "the guest booted anew"
    elif ! grep -a -q "GIM: KVM: The saved state is of a VM which $3 the paravirtualized clock" "$VMDIR/Logs/VBox.log"; then
        stage "$6" FAILED "the VM does not say that the state is of a VM which $3 the clock"
    elif [ "$3" = offered ] && ! clock_left_out; then
        stage "$6" FAILED "the VM which loaded the state was not configured without the clock"
    elif [ "$3" != offered ] && [ "$(clock_in_log)" -ne 0 ]; then
        stage "$6" FAILED "the VM has set the clock up for a guest which has none"
    else
        stage "$6" ok "the VM has set the clock up $(clock_in_log) times for the state"
        guest_pace "the time of the guest of that state goes as it does outside" "$5"
    fi
    keep_logs "$4"
}

kvmclock_rounds() # <the extra data key>
{
    local n what src

    # A VM without the value: the guest sets the clock up.
    boot "the guest boots with the interface as it is" || return
    n=$(clock_in_log)
    serial_show 'kvm-clock: Using msrs|Hypervisor detected|Switched to clocksource' 6
    what="the guest sets up the clock of the interface where it is offered"
    if serial_has 'kvm-clock: Using msrs' && [ "$n" -gt 0 ] && ! clock_left_out; then
        stage "$what" ok
    else
        stage "$what" FAILED "the VM has set it up $n times"
    fi
    guest_pace "the time of the guest goes as it does outside, with the clock" yes
    keep_logs -offered
    clock_state_round "$1" 0 "offered" -loaded-with yes \
        "a state which was saved with the clock is loaded where the clock is not offered, and the guest goes on"
    reset_vm

    # A start with the value: the guest boots and has nothing of the clock.
    run setextradata "$VM" "$1" 0
    if boot "the guest boots where the clock is not offered"; then
        n=$(clock_in_log)
        serial_show 'kvm-clock|Hypervisor detected|Switched to clocksource|tsc: ' 10
        src=$(serial_new | grep -a -o 'Switched to clocksource [a-z0-9_-]*' | tail -n 1 | awk '{print $4}')
        what="the guest does without the clock which is not offered"
        if serial_has 'kvm-clock'; then
            stage "$what" FAILED "its kernel speaks of kvm-clock"
        elif [ "$n" -ne 0 ]; then
            stage "$what" FAILED "the VM has set the clock up $n times"
        elif ! clock_left_out; then
            stage "$what" FAILED "the VM does not say that it leaves the clock out"
        else
            stage "$what" ok "its clock source is ${src:-not told}"
        fi
        if serial_has 'Hypervisor detected: KVM'; then
            stage "the guest finds the interface without the clock" ok
        else
            stage "the guest finds the interface without the clock" FAILED
        fi
        guest_pace "the time of the guest goes as it does outside, without the clock" "$TIMER_DEADLINE"
        keep_logs -without
        clock_state_round "$1" "" "did not offer" -loaded-without "$TIMER_DEADLINE" \
            "a state which was saved without the clock is loaded where the clock is offered, and the guest goes on"
    fi
    reset_vm

    # The value taken back: as before.
    run setextradata "$VM" "$1"
    if boot "the guest boots with the value taken back"; then
        n=$(clock_in_log)
        what="the clock is offered again when the value is taken back"
        if serial_has 'kvm-clock: Using msrs' && [ "$n" -gt 0 ] && ! clock_left_out; then
            stage "$what" ok
        else
            stage "$what" FAILED "the VM has set it up $n times"
        fi
    fi
}

phase_kvmclock()
{
    local key=VBoxInternal/GIM/KVM/Clock
    PHASE=kvmclock
    reset_vm
    vbm setextradata "$VM" "$key" > /dev/null 2>&1
    kvmclock_rounds "$key"
    end_phase
    # The phases after this one get the VM as it was.
    vbm setextradata "$VM" "$key" > /dev/null 2>&1
}


# A guest which has no clock from a paravirtualization interface, on any
# tree.  Its kernel has to find out how fast the TSC counts with the PIT or
# the HPET, and it watches the TSC with the HPET afterwards.  Both are
# devices of the VM.  What comes of it where the timers of the VM are served
# late is told at guest_pace: on a tree whose KVM backend sets a timer for the
# deadline the guest has to keep time, and Windows guests which have their
# time from the HPET depend on the same.
#
# Twice.  First a guest which is told not to use the clock (no-kvmclock on
# its command line) in the VM as it is.  Then the usual guest in a VM which is
# told to have no paravirtualization interface.  That one did not get through
# its start with the KVM backend of upstream (run 387): the kernel of the
# guest found that the PIT does not tick through the I/O APIC, had it come
# through the 8259A, and the VM stopped where the backend had a second
# interrupt of that one for a kernel which still had the first.  A tree with
# that backend has this told and not counted.
devtime_look() # <what the round is>
{
    local src
    serial_show 'kvm-clock|Hypervisor detected|Switched to clocksource|tsc: |Marking|MP-BIOS|8259A' 18
    src=$(serial_new | grep -a -o 'Switched to clocksource [a-z0-9_-]*' | tail -n 1 | awk '{print $4}')
    if serial_has 'kvm-clock: Using msrs'; then
        stage "the guest has no clock of an interface, $1" FAILED "its kernel has set up kvm-clock"
    else
        stage "the guest has no clock of an interface, $1" ok "its clock source is ${src:-not told}"
    fi
    guest_pace "the time of the guest goes as it does outside, $1" "$TIMER_DEADLINE"
    grep -a -E 'TM: (Giving up|Not bothering)' "$VMDIR/Logs/VBox.log" 2> /dev/null \
        | tail -n 4 | awk '{ print "    | " substr($0, 1, 200) }'
}

phase_devtime()
{
    local what disk="--storagectl sata --port 0 --device 0 --type hdd --medium"
    PHASE=devtime
    reset_vm

    what="with a guest told not to use the clock of the interface"
    if [ ! -s "${GUEST_RAW_NOCLOCK:-}" ]; then
        stage "the guest boots, $what" skipped "there is no disk for such a guest"
    else
        [ -s "$SIM_WORK/guest-noclock.vdi" ] || run convertfromraw "$GUEST_RAW_NOCLOCK" "$SIM_WORK/guest-noclock.vdi" --format VDI
        if ! run storageattach "$VM" $disk "$SIM_WORK/guest-noclock.vdi"; then
            stage "the guest boots, $what" FAILED "no disk"
        elif boot "the guest boots, $what"; then
            devtime_look "$what"
        fi
        keep_logs -noclock
        reset_vm
        run storageattach "$VM" $disk "$SIM_WORK/guest.vdi"
    fi

    what="in a VM without a paravirtualization interface"
    if ! run modifyvm "$VM" --paravirt-provider none; then
        stage "the guest boots, $what" FAILED "the VM does not take the setting"
    elif ! start_vm; then
        stage "the guest boots, $what" FAILED "the VM does not start"
        diag
    elif wait_serial 'GUEST: READY' "$BOOT_WAIT" 1; then
        stage "the guest boots, $what" ok
        if serial_has 'Hypervisor detected'; then
            stage "the guest finds no interface" FAILED "its kernel found one"
        else
            stage "the guest finds no interface" ok
        fi
        devtime_look "$what"
    elif    [ "$TIMER_DEADLINE" = no ] && grep -a -q 'Guru Meditation' "$VMDIR/Logs/VBox.log" 2> /dev/null \
         && grep -a -q 'AssertLogRel.*nemHCLnxHandleInterruptFF' "$VMDIR/Logs/VBox.log"; then
        serial_show 'TIMER|MP-BIOS|8259A|works' 6
        grep -a -A 1 'AssertLogRel.*nemHCLnxHandleInterruptFF' "$VMDIR/Logs/VBox.log" \
            | head -n 2 | awk '{ print "    | " substr($0, 1, 220) }'
        grep -a 'Guru Meditation' "$VMDIR/Logs/VBox.log" | head -n 1 | awk '{ print "    | " substr($0, 1, 120) }'
        stage "the guest boots, $what" skipped "a tree with the KVM backend of upstream: the VM stops where the guest has its timer interrupt come through the 8259A"
    else
        stage "the guest boots, $what" FAILED "the guest does not come up"
        diag
    fi
    end_phase
    # The phases after this one get the VM as it was.
    reset_vm
    run modifyvm "$VM" --paravirt-provider default
}


# The frontend is told to end with nothing of the passthrough device in the
# VM.  The VM is paused, for which VBoxHeadless tries nothing: it closes the
# session, and the Console powers the VM down from its uninit().  Twice: by
# itself, and while somebody else holds on to the VM, which writing the memory
# of the guest to a file does for as long as it takes.  Both went wrong in
# the code this work began with (STATUS.md, "Who waits for whom"): the
# service kept the VM as "stopping" for good, and the VM process did not end.
# With the file it is a matter of the moment, so a round in which the file was
# written before the session got closed tells nothing and there is another.
phase_endpaused()
{
    PHASE=endpaused
    local round=0
    reset_vm
    if boot "the guest boots, for its VM to be paused"; then
        run controlvm "$VM" pause
        if wait_state paused 30; then
            frontend_told_to_end paused
        else
            stage "the VM process ends when it is told to with the VM paused" skipped "the VM is in state '$(vmstate)' and not paused"
        fi
    fi
    keep_logs -paused
    DUMP_HIT=
    while [ "$round" -lt 3 ] && [ "$DUMP_HIT" != yes ]; do
        round=$((round + 1))
        reset_vm
        boot "the guest boots once more, for the memory of its VM to be written (round $round)" || break
        run controlvm "$VM" pause
        if ! wait_state paused 30; then
            echo "    the VM is in state '$(vmstate)' and not paused"
            break
        fi
        frontend_told_to_end dump
        keep_logs "-dump$round"
    done
    if [ "$DUMP_HIT" != yes ]; then
        stage "the VM process ends when it is told to with the VM paused and its memory being written to a file" skipped \
              "in $round rounds the file was never still being written when the session got closed"
    fi
    end_phase
}


# A VM which is to have the legacy VGA ranges of a graphics card (ExposeVga),
# on a host whose kernel does not give them away: vfio-pci is loaded with
# disable_vga in the machine of this part (l1.sh), and a kernel which was
# built without CONFIG_VFIO_PCI_VGA is the other way to get there.  The VM
# gets the card by extra data, as the top of DevPciVfio.cpp says, which every
# tree with the device takes.  It must not start, and what it gives for a
# reason is told: a tree with this work names what the kernel lacks, the code
# of upstream tells of a region and of an error number.
phase_vganone()
{
    local n node= said key
    PHASE=vganone
    reset_vm
    if [ "$(cat /sys/module/vfio_pci/parameters/disable_vga 2> /dev/null)" != Y ]; then
        stage "the kernel of the host gives no legacy VGA ranges away" skipped "vfio-pci is not loaded with disable_vga in this machine"
        end_phase
        return 0
    fi
    stage "the kernel of the host gives no legacy VGA ranges away" ok "vfio-pci has disable_vga set"
    for n in "/sys/bus/pci/devices/$HOSTADDR"/vfio-dev/vfio*; do
        [ -e "$n" ] && node=/dev/vfio/devices/${n##*/}
    done
    echo "    the card is $HOSTADDR, class $(cat "/sys/bus/pci/devices/$HOSTADDR/class" 2>&1), its node $node"
    run modifyvm "$VM" --graphicscontroller none
    run setextradata "$VM" "VBoxInternal/Devices/ich9pcibridge/0/Config/ExpressEnabled" 1
    run setextradata "$VM" "VBoxInternal/Devices/ich9pcibridge/0/Config/ExpressPortType" RootCmplxRootPort
    run setextradata "$VM" "VBoxInternal/Devices/pci-vfio/0/Config/Fun0/VfioPath" "$node"
    run setextradata "$VM" "VBoxInternal/Devices/pci-vfio/0/Config/Fun0/ExposeVga" 1
    run setextradata "$VM" "VBoxInternal/Devices/pci-vfio/0/Config/IommuPath" /dev/iommu
    run setextradata "$VM" "VBoxInternal/Devices/pci-vfio/0/PCIBusNo" 1
    run setextradata "$VM" "VBoxInternal/Devices/pci-vfio/0/PCIDeviceNo" 0
    run setextradata "$VM" "VBoxInternal/Devices/pci-vfio/0/PCIFunctionNo" 0
    run setextradata "$VM" "VBoxInternal/Devices/pci-vfio/0/Trusted" 1
    if start_vm && wait_state running 20; then
        stage "a VM which is to have the legacy VGA ranges of the card does not start" FAILED "it runs"
        grep -a -E 'VFIO#0' "$VMDIR/Logs/VBox.log" 2> /dev/null | grep -a -v -E 'flags:|index:|cap_offset:|size:|offset:|count:' | tail -n 12 | awk '{ print "    | " substr($0, 1, 200) }'
    else
        stage "a VM which is to have the legacy VGA ranges of the card does not start" ok
        # What it says: the answer to the command, and the log of the VM.
        said=$(cat "$SIM_WORK/startvm.out" "$VMDIR/Logs/VBox.log" 2> /dev/null | grep -a -E 'legacy VGA|CONFIG_VFIO_PCI_VGA|Getting information for region|VMSetError|VERR_' | grep -a -v 'VINF_' | tail -n 6)
        echo "$said" | awk '{ print "    | " substr($0, 1, 260) }'
        if echo "$said" | grep -q 'CONFIG_VFIO_PCI_VGA'; then
            stage "it says what the kernel of the host lacks" ok
        elif echo "$said" | grep -q 'Getting information for region 8 of opened VFIO device failed with 22'; then
            stage "it says what the kernel of the host lacks" skipped "a tree with the device as upstream has it: it says that getting information for region 8 failed with 22"
        else
            stage "it says what the kernel of the host lacks" FAILED "$(echo "$said" | tail -n 1 | awk '{ print substr($0, 1, 200) }')"
            diag
        fi
    fi
    keep_logs -vganone
    reset_vm
    for key in ich9pcibridge/0/Config/ExpressEnabled ich9pcibridge/0/Config/ExpressPortType pci-vfio/0/Config/Fun0/VfioPath \
               pci-vfio/0/Config/Fun0/ExposeVga pci-vfio/0/Config/IommuPath pci-vfio/0/PCIBusNo pci-vfio/0/PCIDeviceNo \
               pci-vfio/0/PCIFunctionNo pci-vfio/0/Trusted; do
        vbm setextradata "$VM" "VBoxInternal/Devices/$key" > /dev/null 2>&1
    done
    run modifyvm "$VM" --graphicscontroller vmsvga
    end_phase
}


# win-phases.sh takes the functions above and has phases of its own.
[ -n "${SIM_LIB_ONLY:-}" ] && return 0

if setup; then
    for p in "$@"; do
        echo "==== phase $p on $WHERE"
        case "$p" in
            plain)       phase_plain ;;
            emptyslot)   phase_emptyslot ;;
            nat)         phase_nat ;;
            import)      phase_import ;;
            gfx3d)       phase_gfx3d ;;
            fwsetup)     phase_fwsetup ;;
            kvmclock)    phase_kvmclock ;;
            passthrough) phase_passthrough ;;
            hotplug)     phase_hotplug ;;
            hostremove)  phase_hostremove ;;
            hosterror)   phase_hosterror ;;
            lostaddr)    phase_lostaddr ;;
            hostplug)    phase_hostplug ;;
            hostplug-again) phase_hostplug hostplug-again ;;
            startempty)  phase_startempty ;;
            intx)        phase_intx ;;
            intxslot)    phase_intxslot ;;
            devtime)     phase_devtime ;;
            endpaused)   phase_endpaused ;;
            logtime)     phase_logtime ;;
            vganone)     phase_vganone ;;
            mfpassthrough) phase_mfpassthrough ;;
            mfhotplug)     phase_mfhotplug ;;
            mfstartempty)  phase_mfstartempty ;;
            mfhostremove)  phase_mfhostremove ;;
            mfbusreset)    phase_mfbusreset ;;
            *)           echo "unknown phase $p" ;;
        esac
    done
fi

cp -f "$VBOX_USER_HOME/VBoxSVC.log" "$SIM_OUT/VBoxSVC.log" 2> /dev/null
# VBoxSVC ends on its own when nothing uses it any more; do not wait for that.
pkill -x VBoxSVC 2> /dev/null
exit 0
