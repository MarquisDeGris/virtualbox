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
EXPECT_MD5=${SIM_MD5:-none}
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

# Brings the VM back to being powered off without a device of the host.
reset_vm()
{
    local a
    case "$(vmstate)" in
        running|paused|stuck|gurumeditation|starting|restoring|saving)
            run controlvm "$VM" poweroff
            wait_state poweroff 60 || echo "    the VM is in state '$(vmstate)'"
            ;;
        saved)
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
            run controlvm "$VM" poweroff
            wait_state poweroff 60 || echo "    the VM is in state '$(vmstate)'"
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
    echo "+ VBoxManage startvm $VM --type headless"
    VBM_TIMEOUT=300 vbm startvm "$VM" --type headless > "$SIM_WORK/startvm.out" 2>&1
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
    n=$(grep -a -c -E "$re" "$log" 2> /dev/null)
    if [ "${n:-0}" = 0 ]; then
        stage "the log of the VM has no line about a failed reset or register write" ok
    else
        stage "the log of the VM has no line about a failed reset or register write" FAILED "$n"
        grep -a -E "$re" "$log" | cut -c1-200 | sed 's/^/    | /'
    fi
    grep -a 'The device is not reset now' "$log" | cut -c1-200 | head -n 2 | sed 's/^/    | /'
}

# The device of the host and the driver it has to be bound to.
bind_dev()
{
    local d=/sys/bus/pci/devices/$HOSTADDR i=0
    while [ ! -e "$d" ] && [ "$i" -lt 10 ]; do
        sleep 1
        i=$((i + 1))
    done
    if [ ! -e "$d" ]; then
        echo "    the device $HOSTADDR is not on the host"
        return 1
    fi
    local cur=
    [ -e "$d/driver" ] && cur=$(basename "$(readlink "$d/driver")")
    if [ "$cur" != vfio-pci ]; then
        [ -n "$cur" ] && echo "$HOSTADDR" > "$d/driver/unbind"
        echo vfio-pci > "$d/driver_override"
        echo "$HOSTADDR" > /sys/bus/pci/drivers_probe
        cur=
        [ -e "$d/driver" ] && cur=$(basename "$(readlink "$d/driver")")
    fi
    [ "$cur" = vfio-pci ]
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
    echo "$HOSTADDR" > /sys/bus/pci/drivers/vfio-pci/unbind
}

# Saves the state of the VM, which takes the slot being empty, and loads it.
saved_state_round()
{
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
    fi
    end_phase
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


# win-phases.sh takes the functions above and has phases of its own.
[ -n "${SIM_LIB_ONLY:-}" ] && return 0

if setup; then
    for p in "$@"; do
        echo "==== phase $p on $WHERE"
        case "$p" in
            plain)       phase_plain ;;
            emptyslot)   phase_emptyslot ;;
            passthrough) phase_passthrough ;;
            hotplug)     phase_hotplug ;;
            hostremove)  phase_hostremove ;;
            startempty)  phase_startempty ;;
            *)           echo "unknown phase $p" ;;
        esac
    done
fi

cp -f "$VBOX_USER_HOME/VBoxSVC.log" "$SIM_OUT/VBoxSVC.log" 2> /dev/null
# VBoxSVC ends on its own when nothing uses it any more; do not wait for that.
pkill -x VBoxSVC 2> /dev/null
exit 0
