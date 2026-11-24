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
    grep -a 'The device is not reset now' "$log" | cut -c1-200 | head -n 2 | sed 's/^/    | /'
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

# A number as the four bytes it is in memory, lowest first.
le32() # <number>
{
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $(( $1 & 255 )) $(( ($1 >> 8) & 255 )) $(( ($1 >> 16) & 255 )) $(( ($1 >> 24) & 255 )))"
}

# Has the kernel of the host go through what it does when a device reports an
# error it cannot correct (PCI Express Advanced Error Reporting).  The error
# is made up, by the means the kernel has for trying this out (the module
# aer_inject); what the kernel then does about it is what it does about a
# real one: it tells the drivers of the device and of what is behind it, and
# for an error it takes for fatal it resets the bus behind the port.
# The device named has to have the capability for reporting errors.
aer_inject() # <address> <status of uncorrectable errors>
{
    local a=$1 dom bus dev fn rec=$SIM_WORK/aer-inject.bin
    dom=$(( 16#${a%%:*} )); a=${a#*:}
    bus=$(( 16#${a%%:*} )); a=${a#*:}
    dev=$(( 16#${a%%.*} ))
    fn=$(( 16#${a#*.} ))
    # struct aer_error_inj of the kernel: bus, device, function, a byte of
    # padding, then the status of uncorrectable and of correctable errors,
    # four words of a header log and the domain.  One write of all of it.
    {
        printf "$(printf '\\x%02x\\x%02x\\x%02x\\x00' "$bus" "$dev" "$fn")"
        le32 "$2"; le32 0; le32 0; le32 0; le32 0; le32 0; le32 "$dom"
    } > "$rec"
    if [ "$(stat -c %s "$rec")" != 32 ]; then
        echo "    the record for the kernel has $(stat -c %s "$rec") bytes, not 32"
        return 1
    fi
    dd if="$rec" of=/dev/aer_inject bs=32 count=1 2>&1 | grep -v -E 'records (in|out)|copied' | sed 's/^/    /'
    return "${PIPESTATUS[0]}"
}

# One error of the device, as the host tells of it, with the device in the
# slot and the guest using it.
host_error_round() # <round> <what the error is called> <address to report it for> <status>
{
    local round=$1 what=$2 at=$3 status=$4 log="$VMDIR/Logs/VBox.log" n0 i=0
    n0=$(grep -a -c 'the host reported an error of the device' "$log" 2> /dev/null)
    serial_mark
    dmesg -C 2> /dev/null
    echo "+ an error of $at for the kernel of the host: $what (status $(printf '0x%08x' "$status"))"
    if ! aer_inject "$at" "$status"; then
        stage "host: its kernel is told of an error which is $what" FAILED "the kernel did not take it"
        dmesg | tail -n 5 | cut -c1-200 | sed 's/^/    | /'
        return 1
    fi
    stage "host: its kernel is told of an error which is $what" ok
    # VirtualBox is told by the driver which has the device.
    while [ "$i" -lt 30 ] && [ "$(grep -a -c 'the host reported an error of the device' "$log" 2> /dev/null)" = "${n0:-0}" ]; do
        sleep 1
        i=$((i + 1))
    done
    dmesg | grep -i -E 'aer|vfio|pcieport|nvme|reset' | cut -c1-200 | head -n 25 | sed 's/^/    | /'
    if [ "$(grep -a -c 'the host reported an error of the device' "$log" 2> /dev/null)" != "${n0:-0}" ]; then
        stage "VirtualBox is told of the error and takes the device out of service ($what)" ok "after $i s"
    else
        stage "VirtualBox is told of the error and takes the device out of service ($what)" FAILED "nothing in the log of the VM after $i s"
        diag
        return 1
    fi
    grep -a -E 'taken out of service|reported an error' "$log" | tail -n 3 | cut -c1-200 | sed 's/^/    | /'
    if wait_serial "GUEST: GONE $round" 150; then
        stage "guest: notices that the drive is gone ($what)" ok
    else
        stage "guest: notices that the drive is gone ($what)" FAILED
    fi
    serial_show 'pciehp|GUEST: GONE' 10
    guest_goes_on "the VM runs on after the error ($what)" 150
    check_pcistate hotplug,absent,unused
    # The device is what it was to the host, or is made that again.
    [ -e "/sys/bus/pci/devices/$HOSTADDR" ] || echo 1 > /sys/bus/pci/rescan
    if ! bind_dev; then
        stage "host: has the device after the error ($what)" FAILED
        return 1
    fi
    stage "host: has the device after the error ($what)" ok
    serial_mark
    if run controlvm "$VM" plugpci "$HOSTADDR"; then
        stage "plugpci after the error ($what)" ok
    else
        stage "plugpci after the error ($what)" FAILED
        diag
        return 1
    fi
    guest_reads $((round + 1)) "guest: takes the device again and reads from the drive ($what)"
}

# The host reports an error of the device while the guest uses it.  The error
# is made up (see aer_inject); the kernel of the host, the driver which has the
# device for VirtualBox and VirtualBox go through what they do about a real
# one.  First an error the kernel does not take for fatal, then one it does,
# after which it resets the bus the device is on.
phase_hosterror()
{
    local at port
    PHASE=hosterror
    reset_vm
    if ! modprobe aer_inject 2> /dev/null || [ ! -e /dev/aer_inject ]; then
        stage "the kernel of the host can be told of an error which is none" skipped "it has no module aer_inject"
        return
    fi
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    # The error is reported for the device if it can report errors, and for
    # the port it is behind if it cannot: the kernel tells everything behind
    # a port of an error of that port.
    port=$(basename "$(readlink -f "/sys/bus/pci/devices/$HOSTADDR/..")")
    if [ -e "/sys/bus/pci/devices/$HOSTADDR/aer_dev_fatal" ]; then
        at=$HOSTADDR
    else
        at=$port
    fi
    echo "    the device is $HOSTADDR behind the port $port; errors are reported for $at"
    lspci -s "$at" -vv 2> /dev/null | grep -A 6 'Advanced Error Reporting' | cut -c1-160 | sed 's/^/    | /'
    if [ ! -e "/sys/bus/pci/devices/$at/aer_dev_fatal" ]; then
        stage "the kernel of the host can be told of an error which is none" skipped "neither the device nor its port reports errors"
        return
    fi
    run modifyvm "$VM" --pci-attach "$HOSTADDR@0000:01:00.0" && ATTACHED=$HOSTADDR
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    if boot "a VM with the device in the hot-plug capable slot boots the guest"; then
        guest_reads 1 "guest: reads from the drive in the slot"
        # Completer Abort, then Malformed TLP: by the specification the first
        # is not fatal unless a device says so, and the second is.
        if host_error_round 1 "not fatal" "$at" $(( 0x00008000 )); then
            host_error_round 2 "fatal" "$at" $(( 0x00040000 ))
        fi
        echo "    errors the host has counted for $at as not fatal: $(tr '\n' ' ' < "/sys/bus/pci/devices/$at/aer_dev_nonfatal" 2> /dev/null | cut -c1-200)"
        echo "    and as fatal: $(tr '\n' ' ' < "/sys/bus/pci/devices/$at/aer_dev_fatal" 2> /dev/null | cut -c1-200)"
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
    grep -a -E "can't be reset on its own \(|could not be reset|is not reset now" "$log" | cut -c1-200 | head -n 6 | sed 's/^/    | /'
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
            passthrough) phase_passthrough ;;
            hotplug)     phase_hotplug ;;
            hostremove)  phase_hostremove ;;
            hosterror)   phase_hosterror ;;
            startempty)  phase_startempty ;;
            intx)        phase_intx ;;
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
