#!/bin/bash
# Part of the simulation, see run.sh: Windows as the guest.
#
#   win-phases.sh runner
#       On the runner, which has KVM and no IOMMU, so there is no device to put
#       into the slot.  Windows Server is installed from its evaluation image
#       without anybody at the keyboard (win/Autounattend.xml on a second DVD),
#       into a VM as it is configured for a device of the host with hot-plug.
#       So setup and the installed Windows both meet the PCI Express root
#       bridge, what the firmware hands over and the port with the slot.
#
#   win-phases.sh nested
#       In the virtual machine which stands in for a host, see l1.sh: the
#       Windows which was installed on the runner is started there with the
#       NVMe drive of that machine in the slot, and the drive is taken out and
#       put in while Windows runs.  The disk of Windows is the block device
#       $WIN_DISK_DEV there.
#
# A script in the guest (win/report.ps1) tells on the serial port what Windows
# made of the root bridge, the port and what is behind it, and the slot
# registers are read from the outside.  The guest is told to shut down by a
# DVD which is put into its drive ($WIN_CMD_ISO).
#
# Results are "STAGE ..." lines as in vbox-phases.sh, whose functions are used.

set -u
MODE=${1:-runner}
: "${VBOX_BIN:?}" "${SIM_WORK:?}" "${SIM_OUT:?}" "${SIM_STAGES:?}" "${WIN_CMD_ISO:?}"
GUEST_RAW=none
SIM_LIB_ONLY=1
. "$(cd "$(dirname "$0")" && pwd)/vbox-phases.sh"

VM=simwin
SERIAL=$SIM_WORK/win-serial.log
UP_RE='WIN: UP'
ALIVE_RE='WIN: ALIVE'
INSTALL_WAIT=${WIN_INSTALL_WAIT:-3000}
START_WAIT=${WIN_START_WAIT:-900}
SHUTDOWN_WAIT=240
SHOTS=0
BUTTON_WORKS=unknown
# Whether the Windows of the VM which was started last got as far as its report.
WIN_UP=no
# The paravirtualization interface Windows gets, see create_vm.  It depends
# on the KVM backend the tree was built with: a build with the backend of
# Cyberus Technology says so in what configure wrote (--with-kvm).
if [ -n "${WIN_PARAVIRT:-}" ]; then
    PARAVIRT=$WIN_PARAVIRT
elif grep -q -s -E '^VBOX_WITH_KVM +:= +1' "$VBOX_BIN/../../../../AutoConfig.kmk"; then
    PARAVIRT=default
else
    PARAVIRT=none
fi
STALL_EVERY=${WIN_STALL_EVERY:-60}
STALL_T=0
STALL_N=
STALLS=0
MARKS='^WIN: (SETUP|SPECIALIZE|FIRSTLOGON|BUTTON-DEFAULT|UP|OS=|REPORT-END|SHUTDOWN)'
# The drive behind the port with the slot, as the script in the guest tells
# of it once the driver of Windows has it.
DRIVE_RE='^WIN: PRESENT [0-9]+ at=1\.0\.0 id=[^ ]* status=OK problem=0 service=stornvme'
# The same for the second function of a device which has two, each an NVMe
# controller with a drive of its own (run.sh, part "mfwinhost").
DRIVE2_RE='^WIN: PRESENT [0-9]+ at=1\.0\.1 id=[^ ]* status=OK problem=0 service=stornvme'
# The second function where it is a High Definition Audio controller: the bus
# driver Windows has for those has it.
AUDIO_RE='^WIN: PRESENT [0-9]+ at=1\.0\.1 id=[^ ]* status=OK problem=0 service=[Hh][Dd][Aa]ud[Bb]us'


# A picture of the screen of the VM, the only way to see what setup is doing.
shot()
{
    SHOTS=$((SHOTS + 1))
    local f
    f=$(printf '%s/win-screen-%03d.png' "$SIM_OUT" "$SHOTS")
    if vbm controlvm "$VM" screenshotpng "$f" > "$SIM_WORK/shot.out" 2>&1 && [ -s "$f" ]; then
        echo "    picture of the screen: $(basename "$f"), $(stat -c %s "$f") bytes"
    else
        echo "    no picture of the screen: $(head -n 1 "$SIM_WORK/shot.out" | cut -c1-200)"
        rm -f "$f"
    fi
}

serial_lines()
{
    { wc -l < "$SERIAL"; } 2> /dev/null || echo 0
}

# How often the guest has left for VirtualBox so far, all its CPUs together.
# Nothing when the VM cannot be asked.  Asking does not go through the threads
# of the CPUs.
exits_now()
{
    VBM_TIMEOUT=60 vbm debugvm "$VM" statistics --pattern '/NEM/CPU*/Exit' 2> /dev/null \
        | grep -a -o -E '<Counter c="[0-9]+"[^>]*name="/NEM/CPU[0-9]+/Exit"' \
        | sed -E 's/^<Counter c="([0-9]+)".*$/\1/' \
        | awk '{ s += $1; n++ } END { if (n) print s }'
}

# Where a Windows which says nothing yet has got to, as far as that shows from
# the outside: how often it has left for VirtualBox, how much it has read from
# its disk (the loader reads some 1300 times, the disk driver of Windows after
# it), how many interrupts the I/O APIC has passed on (none before Windows has
# started its first driver with one), where CPU 0 is, and how much of the
# memory of this machine is in huge pages.
progress_note()
{
    local s
    s=$(VBM_TIMEOUT=60 vbm debugvm "$VM" statistics --pattern '/NEM/CPU*/Exit|/Public/Storage/AHCI0/Port0/ReqsRead|/Devices/ioapic/Vectors/*' 2> /dev/null | tr -d '\r')
    printf 'exits %s, disk reads %s, interrupts by the I/O APIC %s, %s, %s' \
        "$(echo "$s" | grep -a -o -E '<Counter c="[0-9]+"[^>]*name="/NEM/CPU[0-9]+/Exit"' | sed -E 's/^<Counter c="([0-9]+)".*$/\1/' | awk '{ t += $1 } END { print t + 0 }')" \
        "$(echo "$s" | grep -a -o -E '<Counter c="[0-9]+"[^>]*name="[^"]*ReqsRead"' | sed -E 's/^<Counter c="([0-9]+)".*$/\1/' | head -n 1)" \
        "$(echo "$s" | grep -a -o -E '<Counter c="[0-9]+"[^>]*name="/Devices/ioapic/Vectors/[^"]*"' | sed -E 's/^<Counter c="([0-9]+)".*$/\1/' | awk '{ t += $1 } END { print t + 0 }')" \
        "$(VBM_TIMEOUT=30 vbm debugvm "$VM" getregisters --cpu=0 rip 2>&1 | tr -d '\r' | grep -a -m 1 -E '^rip|rror' | tr -s ' ' | cut -c1-40)" \
        "$(grep -E '^AnonHugePages' /proc/meminfo 2> /dev/null | tr -s ' ')"
}

# Run 227 had a VM which did nothing any more from 40 s after its start: the
# thread of its CPU had missed being told that a timer was due, and Windows
# waited for that timer with its CPUs halted (STATUS.md has it).  A Windows
# which is alive leaves for VirtualBox many times a second.  So this looks,
# at most once every $STALL_EVERY seconds, whether the count has moved since
# the last look.  If it has not, that is told and counted, and the thread of
# CPU 0 is made to come out by asking for a register, which lets the VM go on.
stall_check()
{
    local now n rip
    now=$(date +%s)
    [ $((now - STALL_T)) -ge "$STALL_EVERY" ] || return 0
    STALL_T=$now
    n=$(exits_now)
    if [ -n "$n" ] && [ "$n" = "$STALL_N" ]; then
        STALLS=$((STALLS + 1))
        rip=$(VBM_TIMEOUT=30 vbm debugvm "$VM" getregisters --cpu=0 rip 2>&1 | tr -d '\r' | grep -a -m 1 -E '^rip|rror' | cut -c1-80)
        echo "    the guest has not left for VirtualBox once in the last $STALL_EVERY s or more ($n times in all): CPU 0 woken from the outside, $rip"
        n=
    fi
    STALL_N=$n
}

stalls_reset()
{
    STALLS=0
    STALL_N=
}

stalls_stage()
{
    if [ "$STALLS" = 0 ]; then
        stage "the VM never stood still and had to be woken from the outside" ok
    else
        stage "the VM never stood still and had to be woken from the outside" FAILED "it did, $STALLS time(s)"
    fi
    stalls_reset
}

# Waits for a line of the guest.  Tells what the guest says about where it is
# meanwhile, and takes a picture of the screen every three minutes.
#
# With a third argument the wait ends early, with 2, for a Windows which is
# stuck in its start: it has not said a word yet and has not read from its
# disk for that many seconds.  A Windows which starts reads from its disk all
# the time until its script runs.
wait_win() # <regex> <seconds> [<seconds without a read from the disk>]
{
    local t0 now n st last=0 shown=$SERIAL_FROM reads lastreads= since=0
    t0=$(date +%s)
    while :; do
        now=$(( $(date +%s) - t0 ))
        n=$(serial_lines)
        if [ "$n" -ge "$shown" ]; then
            sed -n "${shown},${n}p" "$SERIAL" | tr -d '\r' | grep -a -E "$MARKS" | cut -c1-200 | sed "s/^/    [$now s] /"
            shown=$((n + 1))
        fi
        serial_has "$1" && return 0
        [ "$now" -ge "$2" ] && return 1
        if [ $((now - last)) -ge 180 ]; then
            last=$now
            st=$(vmstate)
            case "$st" in
                running|starting|restoring) ;;
                *) echo "    the VM is in state '$st', not waiting any longer"; return 1 ;;
            esac
            echo "    [$now s] waiting, the VM is $st; $(progress_note)$(kvm_brief)"
            shot
            # A start which works reads thousands of times in three minutes.
            # One which reads a few times in a quarter of an hour is as stuck
            # as one which does not read at all (run 268: 500 reads in an
            # hour).
            if [ -n "${3:-}" ] && ! serial_has '^WIN: '; then
                reads=$(disk_reads_now)
                if [ -z "$reads" ] || [ -z "$lastreads" ] || [ "$reads" -ge $((lastreads + 100)) ]; then
                    lastreads=$reads
                    since=$now
                elif [ $((now - since)) -ge "$3" ]; then
                    echo "    [$now s] next to nothing was read from the disk for $((now - since)) s ($reads reads in all, $lastreads then) and the guest has not said a word"
                    return 2
                fi
            fi
        fi
        stall_check
        sleep 5
    done
}

# How often the guest has read from its disk so far.
disk_reads_now()
{
    VBM_TIMEOUT=60 vbm debugvm "$VM" statistics --pattern '/Public/Storage/AHCI0/Port0/ReqsRead' 2> /dev/null | tr -d '\r' \
        | grep -a -o -E '<Counter c="[0-9]+"[^>]*name="[^"]*ReqsRead"' | sed -E 's/^<Counter c="([0-9]+)".*$/\1/' | head -n 1
}

# What the KVM of this machine counts for its VMs, a line for each: how often
# the guest left and what for, how many interrupts were put into it, how often
# it halted, and how the time stamp counter of its first CPU is shifted and
# scaled.  Nothing where the kernel does not show that.
kvm_counters()
{
    local d f
    mount -t debugfs debugfs /sys/kernel/debug 2> /dev/null
    for d in /sys/kernel/debug/kvm/[0-9]*-[0-9]*; do
        [ -d "$d" ] || continue
        for f in exits irq_exits irq_injections irq_window_exits nmi_injections halt_exits halt_wakeup io_exits mmio_exits signal_exits hypercalls; do
            [ -r "$d/$f" ] && printf '%s=%s ' "$f" "$(cat "$d/$f" 2> /dev/null)"
        done
        for f in tsc-offset tsc-scaling-ratio guest_mode; do
            [ -r "$d/vcpu0/$f" ] && printf '%s=%s ' "$f" "$(cat "$d/vcpu0/$f" 2> /dev/null)"
        done
        echo
    done
}

# The same in a few words, all VMs of the machine together, for the lines
# which tell how a start is getting on: a start which works and one which does
# not differ in these numbers long before they differ in anything else.
kvm_brief()
{
    local d f v e=0 i=0 h=0 n=0
    mount -t debugfs debugfs /sys/kernel/debug 2> /dev/null
    for d in /sys/kernel/debug/kvm/[0-9]*-[0-9]*; do
        [ -r "$d/exits" ] || continue
        n=$((n + 1))
        v=$(cat "$d/exits" 2> /dev/null);          e=$((e + ${v:-0}))
        v=$(cat "$d/irq_injections" 2> /dev/null); i=$((i + ${v:-0}))
        v=$(cat "$d/halt_exits" 2> /dev/null);     h=$((h + ${v:-0}))
    done
    [ "$n" = 0 ] && return 0
    printf ', for KVM: exits %s, interrupts put in %s, halts %s' "$e" "$i" "$h"
}

# A Windows which is stuck early in its start (run 265, once, on the branch
# for 7.1 with the KVM backend of Cyberus Technology and an AMD EPYC 7763: the
# kernel of Windows had been started, nothing was read from the disk any more,
# the guest left for VirtualBox for interrupts of the host only, some 70 times
# a second, and the picture on the screen changed once in ten minutes).  What
# shows of it from the outside: where CPU 0 is and whether that moves, what
# the guest leaves for, and what time it is in the VM.
stuck_note()
{
    local i what
    echo "    ---- the guest is stuck in its start; what the VM shows of it:"
    for i in 1 2 3 4 5 6; do
        echo "    | exits $(exits_now), $(VBM_TIMEOUT=30 vbm debugvm "$VM" getregisters --cpu=0 rip rsp rflags cr8 2>&1 | tr -d '\r' | grep -a -E '^(rip|rsp|rflags|cr8)|rror' | tr -s ' ' | tr '\n' ' ' | cut -c1-170)"
        sleep 2
    done
    echo "    ---- what KVM counts for the VM, twice, ten seconds apart:"
    kvm_counters | cut -c1-400 | sed 's/^/    | /'
    sleep 10
    kvm_counters | cut -c1-400 | sed 's/^/    | /'
    for what in clocks activetimers lapic; do
        echo "    ---- debugvm info $what:"
        VBM_TIMEOUT=60 vbm debugvm "$VM" info "$what" 2>&1 | tr -d '\r' | grep -a -v '^ *$' | head -n 30 | cut -c1-170 | sed 's/^/    | /'
    done
    VBM_TIMEOUT=60 vbm debugvm "$VM" statistics --pattern '/NEM/CPU0/Exit*|/TM/VirtualSync/CurrentOffset|/TM/TSC/*|/Public/Storage/AHCI0/Port0/Reqs*' 2> /dev/null \
        | grep -a -o -E '<(Counter|U64) (c|val)="[1-9][0-9]*"[^>]*name="[^"]*"' \
        | sed -E 's/^<[A-Za-z0-9]+ [a-z]+="([0-9]+)".*name="([^"]*)"$/\2: \1/' | head -n 40 | sed 's/^/    | /'
    shot
}

# Waits until the VM is powered off, by the clock.
wait_off() # <seconds>
{
    local t0 st
    t0=$(date +%s)
    while [ $(( $(date +%s) - t0 )) -lt "$1" ]; do
        st=$(vmstate)
        [ "$st" = poweroff ] && return 0
        [ "$st" = running ] && stall_check
        sleep 2
    done
    [ "$(vmstate)" = poweroff ]
}

# The number of exits from the guest which the log of a VM which has ended
# tells, for knowing what Windows costs where exits are slow.
show_exits() # <log>
{
    local n
    n=$(grep -a -E '^[0-9:.]+ /NEM/CPU[0-9]+/Exit +[0-9]+ times' "$1" 2> /dev/null | awk '{ s += $3 } END { print s + 0 }')
    echo "    exits from the guest in $(basename "$1"): ${n:-0}"
    grep -a -E '^[0-9:.]+ /(NEM/CPU[0-9]+/Exit(/(Io|Mmio|Intr|RdMsr|WrMsr))?|Devices/hpet/ReadCounter32bit) +[0-9]+ times' "$1" 2> /dev/null \
        | sed -E 's/^[0-9:.]+ //; s/ +/ /g' | sed 's/^/    | /'
    # A guest which is not told that the time stamp counter is invariant
    # reads the time from the HPET, which is where the reads above come from:
    # the last number of this line is 00000100 if it is told, 00000000 if not.
    grep -a -m 1 -E '^[0-9:.]+ +Gst: 80000007/' "$1" 2> /dev/null | sed -E 's/^[0-9:.]+ +//; s/ +/ /g' | sed 's/^/    | CPUID /'
    # VirtualBox tells in its log when it had to tell the thread of a CPU a
    # second time that a timer was due, at most 64 times per VM.  The line
    # reads differently since 2026-11-25; both are counted.
    n=$(grep -a -c -E 'TM: VMCPU_FF_TIMER (pending for more than|set at two ticks in a row)' "$1" 2> /dev/null)
    echo "    times the thread of a CPU had to be told again that a timer was due, in $(basename "$1"): ${n:-0}"
    grep -a -E 'TM: VMCPU_FF_TIMER (pending for more than|set at two ticks in a row)' "$1" 2> /dev/null | head -n 3 | cut -c1-160 | sed 's/^/    | /'
}

serial_value() # <regex with one group>: what the group matched in the first line which matches
{
    serial_new | sed -n -E "s/^.*$1.*\$/\\1/p" | head -n 1
}

# The scan codes for typing a line on the keyboard of the VM, which has the
# US layout for the firmware: small letters, digits, ':', '\' and '.', with
# the Enter key at the end.
scancodes() # <text>
{
    local text=$1 i c code out=
    local letters=abcdefghijklmnopqrstuvwxyz
    local lcodes=(1e 30 2e 20 12 21 22 23 17 24 25 26 32 31 18 19 10 13 1f 14 16 2f 11 2d 15 2c)
    local dcodes=(0b 02 03 04 05 06 07 08 09 0a)
    for ((i = 0; i < ${#text}; i++)); do
        c=${text:i:1}
        case "$c" in
            [abcdefghijklmnopqrstuvwxyz])
                code=${letters%%"$c"*}
                code=${lcodes[${#code}]}
                ;;
            [0123456789]) code=${dcodes[$c]} ;;
            :)            out="$out 2a 27 a7 aa"; continue ;;
            \\)           code=2b ;;
            .)            code=34 ;;
            *)            continue ;;
        esac
        out="$out $code $(printf '%02x' $((0x$code | 0x80)))"
    done
    echo "$out 1c 9c"
}

# Presses the Enter key once a second until setup has written to the disk of
# the VM, at most the number of times given.  Counts in the variables keys and
# size of the caller.
press_until_disk() # <times>
{
    local n=0
    while [ "$n" -lt "$1" ]; do
        size=$(stat -c %s "$SIM_WORK/win.vdi" 2> /dev/null || echo 0)
        [ "$size" -gt 16000000 ] && return 0
        vbm controlvm "$VM" keyboardputscancode 1c 9c > "$SIM_WORK/key.out" 2>&1 \
            || { [ "$keys" = 0 ] && cut -c1-200 "$SIM_WORK/key.out" | sed 's/^/    /'; }
        sleep 1
        n=$((n + 1))
        keys=$((keys + 1))
        [ "$keys" = 40 ] && shot
    done
    size=$(stat -c %s "$SIM_WORK/win.vdi" 2> /dev/null || echo 0)
    [ "$size" -gt 16000000 ]
}

# The type VirtualBox has for the guest, as somebody setting up such a VM
# would choose it.
pick_ostype()
{
    local os
    for os in Windows2025_64 Windows2022_64 Windows11_64 Windows10_64; do
        if vbm list ostypes 2> /dev/null | grep -q -E "^ID( / Description)?: +$os( |\$)"; then
            echo "$os"
            return 0
        fi
    done
    echo Other_64
}

# The VM: ICH9, EFI, KVM, the disk and two DVD drives on SATA, a serial port
# which goes to a file, no network adapter, and the slot configured for a
# device of the host.
create_vm() # <cpus> <MiB> <disk> <DVD or emptydrive> <DVD or emptydrive> <host address> <layout or "">
{
    local os
    os=$(pick_ostype)
    echo "    type of the guest: $os"
    run setproperty machinefolder "$SIM_WORK/vms"
    if ! run createvm --name "$VM" --ostype "$os" --register; then
        stage "the VM for Windows is created" FAILED
        return 1
    fi
    run modifyvm "$VM" --memory "$2" --cpus "$1" --ioapic on --chipset ich9 --firmware efi \
        --vm-execution-engine native-api --nic1 none --audio-enabled off \
        --graphicscontroller vboxsvga --vram 64 --uart1 0x3f8 4 --uartmode1 file "$SERIAL" \
        --boot1 dvd --boot2 disk --boot3 none --boot4 none
    # With KVM VirtualBox cannot give a Windows guest the Hyper-V interface its
    # guest type asks for and gives it the minimal one.  Windows then knows
    # of a hypervisor, does not trust the time stamp counter, keeps its time
    # by the HPET of VirtualBox instead and takes several times as long for
    # everything (run 228: 62 million reads of the HPET during setup).  With
    # no such interface it takes the time stamp counter (run 226).
    # That is the KVM backend of the development line.  The one of Cyberus
    # Technology gives the guest the Hyper-V interface with the help of KVM,
    # so there the VM is left at its default, as somebody creating a VM for
    # Windows has it.  Without the interface Windows kept its time by the
    # HPET on that backend too and did not get through setup in 50 minutes on
    # an Intel processor (run 248: 57 million reads).
    echo "    paravirtualization interface of the VM: $PARAVIRT"
    run modifyvm "$VM" --paravirt-provider "$PARAVIRT"
    run storagectl "$VM" --name sata --add sata --controller IntelAhci --portcount 3
    if    ! run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$3" \
       || ! run storageattach "$VM" --storagectl sata --port 1 --device 0 --type dvddrive --medium "$4" \
       || ! run storageattach "$VM" --storagectl sata --port 2 --device 0 --type dvddrive --medium "$5"; then
        stage "the VM for Windows is created" FAILED "disk or DVD"
        return 1
    fi
    run modifyvm "$VM" --pci-attach "$6@0000:01:00.0" && ATTACHED=$6
    # The other functions of the device of the host, where it has more than
    # one: each at its function number of the same guest device.
    if [ -n "$6" ] && [ "$6" = "$HOSTADDR" ]; then
        local a
        for a in $HOSTFUNCS; do
            [ "$a" = "$6" ] && continue
            run modifyvm "$VM" --pci-attach "$a@0000:01:00.${a##*.}" && ATTACHED="$ATTACHED $a"
        done
    fi
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    [ -n "$7" ] && run setextradata "$VM" "VBoxInternal2/HostPCILayout/$6" "$7"
    VMDIR=$(dirname "$(vbm showvminfo "$VM" --machinereadable | sed -n 's/^CfgFile="\(.*\)"$/\1/p')")
    vbm showvminfo "$VM" --machinereadable \
        | grep -a -i -E '^"?(ostype|memory|cpus|chipset|firmware|VMExecutionEngine|paravirtprovider|graphicscontroller|uart1|SATA-[0-9]|AttachedHostPCI)' \
        | cut -c1-200 | sed 's/^/    /'
    return 0
}


# What the report of the guest says, and what the outside sees.
report_checks() # <"" or text put in front of each check> <state showvminfo has to tell>
{
    local pre=$1 v n
    if [ -z "$pre" ]; then
        serial_show '^WIN: (OS=|CPUS=|ROOTBUS |ROOTBUS-PROP |ROOTBUSES |DEV |DEVS |PORT |PORT-PROP |PROBLEM |PROBLEMS |EVENT |EVENTS |BUGCHECK |BUGCHECKS |BUTTON)' 120
    else
        serial_show '^WIN: (OS=|ROOTBUS |ROOTBUS-PROP .*HotPlugControl|PORT |DEVS |PROBLEM |PROBLEMS |EVENTS |BUGCHECKS |BUTTON-ACTION)' 30
    fi

    if serial_has '^WIN: ROOTBUS id=ACPI\\PNP0A08\\[^ ]* status=OK problem=0 '; then
        stage "${pre}guest: the PCI root bridge is a PCI Express one to it (PNP0A08) and works" ok
    else
        stage "${pre}guest: the PCI root bridge is a PCI Express one to it (PNP0A08) and works" FAILED \
              "$(serial_value 'WIN: (ROOTBUS id=[^ ]* status=[^ ]* problem=[^ ]*)')"
    fi

    v=$(serial_value 'WIN: ROOTBUS-PROP DEVPKEY_PciRootBus_PCIExpressNativeHotPlugControl=([A-Za-z]*)')
    if [ "$v" = True ]; then
        stage "${pre}guest: the firmware hands PCIe hot-plug over to it" ok
    else
        stage "${pre}guest: the firmware hands PCIe hot-plug over to it" FAILED "the property says '${v:-nothing}'"
    fi

    n=$(serial_new | grep -a -c -E '^WIN: PORT at=[^ ]* id=[^ ]* status=OK problem=0 service=pci$')
    if [ "$n" -ge 1 ] && ! serial_has '^WIN: PORT at=[^ ]* id=[^ ]* status=[^ ]* problem=(-|[1-9])'; then
        stage "${pre}guest: its PCI driver runs the port with the slot, without a problem" ok
    else
        stage "${pre}guest: its PCI driver runs the port with the slot, without a problem" FAILED \
              "$(serial_value 'WIN: (PORT at=[^ ]* id=[^ ]* status=[^ ]* problem=[^ ]* service=[^ ]*)')"
    fi

    # The device for the Guest Additions has no driver in a Windows without
    # them, with or without this work.  A device behind the port may still be
    # on its way when the report is made; it is looked at by itself.
    n=$(serial_new | grep -a -E '^WIN: DEV at=0\..* problem=(-|[1-9])' | grep -a -c -v 'VEN_80EE&DEV_CAFE')
    if serial_has '^WIN: DEVS n=[1-9]' && [ "$n" = 0 ]; then
        stage "${pre}guest: no PCI device with a problem" ok
    else
        stage "${pre}guest: no PCI device with a problem" FAILED "$n"
    fi

    if serial_has '^WIN: BUGCHECKS n=0$'; then
        stage "${pre}guest: has not crashed" ok
    else
        stage "${pre}guest: has not crashed" FAILED "$(serial_value 'WIN: (BUGCHECKS n=[0-9]*)')"
    fi

    slot_registers_check
    check_pcistate "$2"
}

# Starts the VM and waits for the report of the guest.
start_and_report() # <what> <seconds>
{
    local t0
    t0=$(date +%s)
    WIN_UP=no
    if ! start_vm; then
        stage "$1" FAILED "the VM does not start"
        diag
        return 1
    fi
    local rc=0
    wait_win '^WIN: REPORT-END' "$2" 900 || rc=$?
    if [ "$rc" = 2 ]; then
        # Stuck early in its start.  That counts as failed, is looked at, and
        # the VM is started once more: what comes after the start is what this
        # is here for, and a second start tells whether the first was chance.
        stage "$1" FAILED "it is stuck early in its start, $(( $(date +%s) - t0 )) s after the VM was started"
        stuck_note
        diag
        keep_logs -stuck
        power_off
        t0=$(date +%s)
        if ! start_vm; then
            stage "$1, at a second attempt" FAILED "the VM does not start"
            diag
            return 1
        fi
        rc=0
        wait_win '^WIN: REPORT-END' "$2" 900 || rc=$?
        if [ "$rc" = 0 ]; then
            WIN_UP=yes
            stage "$1, at a second attempt" ok "$(( $(date +%s) - t0 )) s until its report was done"
            return 0
        fi
        stage "$1, at a second attempt" FAILED "the last from the guest is: $(serial_new | grep -a '^WIN: ' | tail -n 1 | cut -c1-60)"
        [ "$rc" = 2 ] && stuck_note
        shot
        diag
        return 1
    fi
    if [ "$rc" != 0 ]; then
        stage "$1" FAILED "the last from the guest is: $(serial_new | grep -a '^WIN: ' | tail -n 1 | cut -c1-60)"
        shot
        diag
        return 1
    fi
    WIN_UP=yes
    stage "$1" ok "$(( $(date +%s) - t0 )) s until its report was done"
}

# The DVD drive on port 2 gets a medium, also while the VM runs.
dvd() # <medium or emptydrive>
{
    run storageattach "$VM" --storagectl sata --port 2 --device 0 --type dvddrive --medium "$1" --forceunmount
}

# Has the guest shut Windows down: the script in it looks for a DVD which says
# so.  If that does not work the VM is powered off.
guest_shutdown()
{
    case "$(vmstate)" in
        running) ;;
        poweroff) return 0 ;;
        saved) run discardstate "$VM"; return 1 ;;
        *) end_phase; return 1 ;;
    esac
    serial_mark
    dvd "$WIN_CMD_ISO"
    wait_serial '^WIN: SHUTDOWN' "$SHUTDOWN_WAIT" || echo "    the guest has not said that it shuts down"
    if wait_off "$SHUTDOWN_WAIT"; then
        stage "guest: shuts down when the simulation tells it to" ok
    else
        stage "guest: shuts down when the simulation tells it to" FAILED "the VM is in state '$(vmstate)'"
        shot
        power_off
    fi
    keep_logs
    settle
    dvd emptydrive
}

# What the I/O APIC of the VM has for its pins, and how often it has passed
# on which vector.  The interrupt of the ACPI hardware, which a press of the
# power button raises, is pin 9: whether the guest has it masked, whether it
# is waiting there (irr), whether the guest has still to say that it is done
# with the one before (rirr).
irq_state() # <when>
{
    echo "    ---- the I/O APIC of the VM, $1:"
    VBM_TIMEOUT=60 vbm debugvm "$VM" info ioapic 2>&1 | tr -d '\r' | sed -n '/Redirection Table/,$p' | grep -a -v '^ *$' \
        | head -n 30 | cut -c1-130 | sed 's/^/    | /'
    VBM_TIMEOUT=60 vbm debugvm "$VM" statistics --pattern '/Devices/ioapic/*' 2> /dev/null \
        | grep -a -o -E '<Counter c="[1-9][0-9]*"[^>]*name="[^"]*"' \
        | sed -E 's/^<Counter c="([0-9]+)".*name="([^"]*)"$/\2: \1/' | sed 's/^/    | /'
}

# The power button.  Windows is set to shut down when it is pressed
# (win/first.cmd), so the VM powering off shows that the press arrived.
power_button() # <what>
{
    [ "$(vmstate)" = running ] || return 1
    irq_state "before the power button is pressed"
    run controlvm "$VM" acpipowerbutton
    if wait_off 150; then
        stage "$1" ok
        BUTTON_WORKS=yes
        keep_logs
        return 0
    fi
    stage "$1" FAILED "the VM is in state '$(vmstate)' after 150 s"
    BUTTON_WORKS=no
    irq_state "150 s after the power button was pressed"
    shot
    return 1
}


# ---- on the runner

# Setup from the DVD up to the first report of the installed Windows.
phase_install()
{
    PHASE=win-install
    if [ ! -e /dev/iommu ]; then
        stage "Windows is installed in the VM with the empty hot-plug capable slot" skipped "no /dev/iommu here"
        return 1
    fi
    local t0 keys=0 size=0
    t0=$(date +%s)
    stalls_reset
    if ! start_vm; then
        stage "Windows is installed in the VM with the empty hot-plug capable slot" FAILED "the VM does not start"
        diag
        return 1
    fi
    # The boot loader of the DVD wants a key pressed within a few seconds, or
    # it does not start setup.  So the Enter key is pressed once a second
    # until setup writes to the disk, which is empty until then.  That is long
    # before setup starts the VM anew: from then on no key may be pressed, or
    # setup would begin again from the DVD instead of going on from the disk.
    echo "+ VBoxManage controlvm $VM keyboardputscancode 1c 9c, until setup writes to the disk"
    if ! press_until_disk 150; then
        # Nothing yet.  Should the firmware not have taken the DVD and be at its
        # shell, the boot loader of the DVD can be started from there by name.
        # Where setup is on its way after all, the typing does no harm.
        shot
        local fs
        for fs in fs0 fs1 fs2; do
            echo "+ typing $fs:\\efi\\boot\\bootx64.efi, for the shell of the firmware"
            # shellcheck disable=SC2046
            vbm controlvm "$VM" keyboardputscancode $(scancodes "$fs:\\efi\\boot\\bootx64.efi") > /dev/null 2>&1
            press_until_disk 100 && break
        done
    fi
    echo "    Enter was pressed $keys times in $(( $(date +%s) - t0 )) s, the disk image has $size bytes"
    shot
    if [ "$size" -le 16000000 ]; then
        stage "Windows is installed in the VM with the empty hot-plug capable slot" FAILED "setup has not started, nothing was written to the disk"
        diag
        return 1
    fi
    if ! wait_win '^WIN: REPORT-END' "$INSTALL_WAIT"; then
        stage "Windows is installed in the VM with the empty hot-plug capable slot" FAILED \
              "after $(( $(date +%s) - t0 )) s the last from the guest is: $(serial_new | grep -a '^WIN: ' | tail -n 1 | cut -c1-60)"
        stalls_stage
        shot
        diag
        return 1
    fi
    stage "Windows is installed in the VM with the empty hot-plug capable slot" ok \
          "$(( $(date +%s) - t0 )) s, $(serial_value 'WIN: OS=(.*)')"
    echo "    what Windows does when the power button is pressed, as installed: $(serial_value 'WIN: BUTTON-DEFAULT (.*)')"
    shot
    report_checks "" hotplug,absent,unused

    if run controlvm "$VM" plugpci "$FAKE_ADDR"; then
        stage "plugpci of a device which is not there is refused" FAILED "it went through"
    else
        stage "plugpci of a device which is not there is refused" ok
    fi
    guest_goes_on "the VM runs on after that" 120
    stalls_stage

    power_button "guest: shuts down when the power button is pressed" || guest_shutdown
    show_exits "$SIM_OUT/VBox-$PHASE.log"
    [ "$(vmstate)" = poweroff ]
}

# The installed Windows started once more: the same again, what a start
# costs, and a saved state.
phase_restart()
{
    PHASE=win-restart
    settle
    stalls_reset
    start_and_report "the installed Windows starts again" "$START_WAIT" || { stalls_stage; end_phase; return 1; }
    report_checks "after the restart: " hotplug,absent,unused
    guest_goes_on "the guest keeps running" 120
    local went_on=no
    if saved_state_round && guest_goes_on "the guest keeps running after the state was loaded" 120; then
        went_on=yes
    fi
    if [ "$went_on" = yes ]; then
        if [ "$BUTTON_WORKS" = yes ]; then
            power_button "guest: shuts down when the power button is pressed after the state was loaded"
        fi
    elif [ "$STATE_LOADED" = yes ] && [ "$(vmstate)" = running ]; then
        # A guest which stands still after the load tells nothing about the
        # power button, and it is not told to shut down either, it would not
        # hear it: Windows with the Hyper-V interface on the KVM backend of
        # Cyberus Technology for 7.1 said one more line after the load in
        # run 249 and none in run 250.  Its disk is not left as a power cut
        # leaves it, for what comes after this phase: the VM is switched off,
        # started once more and then shut down by the guest below.
        shot
        power_off
        keep_logs -stood
        settle
        start_and_report "Windows starts again after the VM with the loaded state was switched off" "$START_WAIT"
    fi
    stalls_stage
    guest_shutdown
    show_exits "$SIM_OUT/VBox-$PHASE-saved.log"
    [ "$(vmstate)" = poweroff ]
}

# What Main sets for the ACPI device when a VM has a hot-plug capable slot:
# the firmware tables hand native hot-plug to the guest.  Setting it here goes
# on top of what Main does.
ACPI_KEY=VBoxInternal/Devices/acpi/0/Config/PciExpressNativeHotPlug

# One VM for comparison: the installed Windows in a VM which differs from the
# one of this work in what is given, and the power button pressed there.  The
# disk and the VM go back to what they were afterwards, so that Windows does
# not carry what it made of that VM into what follows.
control_vm() # <phase> <what the VM is> <the port with the slot: yes|no> <native hot-plug in the firmware tables: 0|1>
{
    local works=$BUTTON_WORKS
    PHASE=$1
    echo "---- for comparison: $2"
    stalls_reset
    if [ "$3" = no ]; then
        run modifyvm "$VM" --pci-detach "$FAKE_ADDR"
        run setextradata "$VM" VBoxInternal2/HostPCIHotPlug
    fi
    case "$3:$4" in
        yes:1|no:0) ;;
        *) run setextradata "$VM" "$ACPI_KEY" "$4" ;;
    esac
    if start_and_report "for comparison: Windows starts in $2" "$START_WAIT"; then
        serial_show '^WIN: (ROOTBUS |ROOTBUS-PROP .*HotPlugControl|PORT |DEVS |PROBLEMS |BUGCHECKS |BUTTON)' 20
        guest_goes_on "for comparison: Windows keeps running in $2" 120
        power_button "for comparison: Windows shuts down when the power button is pressed in $2"
    fi
    BUTTON_WORKS=$works
    guest_shutdown
    settle
    run setextradata "$VM" "$ACPI_KEY"
    run snapshot "$VM" restore before-control
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
}

# For comparison, should Windows not have shut down when the power button was
# pressed: the same Windows in VMs which have one half of what the work adds,
# the other half, and neither.
phase_control()
{
    PHASE=win-control
    settle
    if ! run snapshot "$VM" take before-control; then
        stage "for comparison: the installed Windows in other VMs" skipped "no snapshot"
        return
    fi
    control_vm win-control-port "a VM with the port and its slot, native hot-plug not handed over" yes 0
    control_vm win-control-acpi "a VM without the port, native hot-plug handed over" no 1
    control_vm win-control "a VM without the slot, as VMs are without this work" no 0
    run snapshot "$VM" delete before-control
}

# The installed Windows in the same VM with a TPM 2.0 added, as a VM for
# Windows 11 has one: Windows has the TPM and its driver runs it, and a state
# is saved and loaded with it.  The TPM is not part of this work.  It is here
# because the guests the work is meant for need one, and because nothing else
# in this VM has its registers read and written the way a TPM has.  The disk
# and the VM go back to what they were afterwards, as after the VMs for
# comparison: what follows is to have the Windows which never saw a TPM.
phase_tpm()
{
    PHASE=win-tpm
    settle
    if ! run snapshot "$VM" take before-tpm; then
        stage "Windows starts in the VM with a TPM 2.0 added" skipped "no snapshot"
        return
    fi
    if ! run modifyvm "$VM" --tpm-type 2.0; then
        stage "Windows starts in the VM with a TPM 2.0 added" skipped "the VM takes no TPM"
        run snapshot "$VM" delete before-tpm
        return
    fi
    vbm showvminfo "$VM" --machinereadable | grep -a -i -E '^"?tpm' | cut -c1-200 | sed 's/^/    /'
    stalls_reset
    if start_and_report "Windows starts in the VM with a TPM 2.0 added" "$START_WAIT"; then
        serial_show '^WIN: (TPM |TPMDEV |PROBLEM |PROBLEMS |BUGCHECKS )' 12
        if serial_has '^WIN: TPM present=1 spec=2\.0'; then
            stage "guest: has the TPM, as one of version 2.0" ok
        else
            stage "guest: has the TPM, as one of version 2.0" FAILED "$(serial_value 'WIN: (TPM present=[^ ]* [^ ]*)')"
        fi
        if serial_has '^WIN: TPMDEV id=[^ ]* status=OK problem=0 '; then
            stage "guest: its driver runs the TPM without a problem" ok
        else
            stage "guest: its driver runs the TPM without a problem" FAILED \
                  "$(serial_value 'WIN: (TPMDEV id=[^ ]* status=[^ ]* problem=[^ ]*)')"
        fi
        guest_goes_on "the guest keeps running" 120
        if saved_state_round; then
            guest_goes_on "the guest keeps running after the state was loaded" 120
        fi
    fi
    stalls_stage
    case "$(vmstate)" in
        running)  guest_shutdown ;;
        poweroff) keep_logs ;;
        *)        end_phase ;;
    esac
    settle
    run snapshot "$VM" restore before-tpm
    run snapshot "$VM" delete before-tpm
}

# Windows with 3D acceleration, for a build which has it (the DVDs for it are
# only made then).  A graphics adapter with 3D acceleration is of use to
# Windows with the driver for it only, which is part of the Guest Additions:
# those Oracle publishes for the release line are installed, without a
# question asked, and Windows is started again in the VM with 3D acceleration
# switched on.  What puts the windows on the screen then draws with that
# driver.  The simulation has the screen change: the keys which bring up the
# screen for signing in and the one which leaves it, forty times.  Nobody
# looks at the pictures but the checks; some are kept.
#
# All of it is done on top of a snapshot which is given up at the end: the
# disk stays as it was for the parts which follow.
gfx_keys() # <rounds>
{
    local i=0
    while [ "$i" -lt "$1" ] && [ "$(vmstate)" = running ]; do
        # Ctrl, Alt and Delete down, and up again.
        vbm controlvm "$VM" keyboardputscancode 1d 38 e0 53 e0 d3 b8 9d > /dev/null 2>&1
        sleep 2
        # A few letters, and the key which leaves the screen.
        # shellcheck disable=SC2046
        vbm controlvm "$VM" keyboardputscancode $(scancodes "abc") > /dev/null 2>&1
        sleep 1
        vbm controlvm "$VM" keyboardputscancode 01 81 > /dev/null 2>&1
        sleep 2
        i=$((i + 1))
    done
    echo "    the keys were pressed $i times"
}

phase_gfx3d()
{
    PHASE=win-gfx3d
    local what
    [ -s "${WIN_GA_ISO:-}" ] && [ -s "${WIN_CMD_ADDITIONS_ISO:-}" ] || return 0
    settle
    if ! run snapshot "$VM" take before-3d; then
        stage "Windows gets the Guest Additions installed" skipped "no snapshot"
        return
    fi

    what="Windows gets the Guest Additions installed"
    run storageattach "$VM" --storagectl sata --port 1 --device 0 --type dvddrive --medium "$WIN_GA_ISO" --forceunmount
    dvd "$WIN_CMD_ADDITIONS_ISO"
    stalls_reset
    if start_and_report "Windows starts for the Guest Additions to be installed" "$START_WAIT"; then
        serial_show '^WIN: VIDEO' 4
        if wait_serial '^WIN: ADDITIONS (exit=|failed)' 1200; then
            wait_serial '^WIN: VIDEO after-additions dwm' 60
            serial_show '^WIN: (ADDITIONS|VIDEO after)' 12
            if serial_has '^WIN: ADDITIONS exit=0 '; then
                stage "$what" ok "$(serial_value 'WIN: ADDITIONS (exit=.*)')"
            else
                stage "$what" FAILED "$(serial_value 'WIN: (ADDITIONS .*)')"
            fi
        else
            stage "$what" FAILED "the guest has not told of it in 20 minutes"
            shot
        fi
        guest_shutdown
    fi
    stalls_stage
    case "$(vmstate)" in
        poweroff) ;;
        *)        end_phase ;;
    esac
    settle
    run storageattach "$VM" --storagectl sata --port 1 --device 0 --type dvddrive --medium emptydrive --forceunmount
    dvd emptydrive

    what="Windows starts in the VM with 3D acceleration"
    if ! run modifyvm "$VM" --graphicscontroller vboxsvga --vram 128 --accelerate-3d on; then
        stage "$what" skipped "this build takes no 3D acceleration for a VM"
    else
        PHASE=win-gfx3d-on
        stalls_reset
        if start_and_report "$what" "$START_WAIT"; then
            serial_show '^WIN: VIDEO' 4
            if serial_has '^WIN: VIDEO start name=VirtualBox_Graphics_Adapter[^ ]* driver=[^ ]+ status=OK'; then
                stage "guest: the graphics driver of the Guest Additions has the adapter" ok "$(serial_value 'WIN: VIDEO start (name=[^ ]* driver=[^ ]*)')"
            else
                stage "guest: the graphics driver of the Guest Additions has the adapter" FAILED "$(serial_value 'WIN: VIDEO start (name=.*)')"
            fi
            shot
            gfx_keys 40
            shot
            if [ "$(vmstate)" = running ]; then
                stage "the VM with 3D acceleration runs on while the screen of Windows changes" ok
            else
                stage "the VM with 3D acceleration runs on while the screen of Windows changes" FAILED "it is in state '$(vmstate)'"
                diag
            fi
            guest_goes_on "the guest keeps running" 120
            echo "    what the adapter has counted (what is not zero, without the registers):"
            VBM_TIMEOUT=60 vbm debugvm "$VM" statistics --pattern '/Devices/vga/VMSVGA/*' 2>&1 \
                | grep -a -v -E '/VMSVGA/Reg/| c="0"| cPeriods="0"| val="0"' | sed -e 's/ unit="[^"]*"//' -e 's/ vis="[^"]*"//' | cut -c1-160 | head -n 40 | sed 's/^/    | /'
            echo "    what the log of the VM says of 3D:"
            grep -a -i -E 'VMSVGA3d|vmsvga.*3d|dxvk|D3D11|vulkan|3D ' "$VMDIR/Logs/VBox.log" | grep -a -v -E 'stub|^[0-9:. ]*Vulkan 1\.[0-9]$' | cut -c1-200 | head -n 40 | sed 's/^/    | /'
        fi
        stalls_stage
        case "$(vmstate)" in
            running)  guest_shutdown ;;
            poweroff) keep_logs ;;
            *)        end_phase ;;
        esac
    fi
    settle
    run snapshot "$VM" restore before-3d
    run snapshot "$VM" delete before-3d
}

mode_runner()
{
    : "${WIN_ISO:?}" "${WIN_UNATTEND_ISO:?}"
    PHASE=win-setup
    echo "==== VirtualBox on $WHERE, Windows as the guest: $("$VBOX_BIN/VBoxManage" --version 2>&1 | tail -n 1)"
    ls -l /dev/kvm /dev/iommu "$WIN_ISO" "$WIN_UNATTEND_ISO" "$WIN_CMD_ISO" 2>&1 | sed 's/^/    /'
    run setproperty machinefolder "$SIM_WORK/vms"
    if ! run createmedium disk --filename "$SIM_WORK/win.vdi" --size 40960 --format VDI; then
        stage "the VM for Windows is created" FAILED "no disk"
        return
    fi
    create_vm 2 4096 "$SIM_WORK/win.vdi" "$WIN_ISO" "$WIN_UNATTEND_ISO" "$FAKE_ADDR" "$FAKE_LAYOUT" || return
    stage "the VM for Windows is created, with an empty hot-plug capable slot" ok

    echo "==== phase win-install on $WHERE"
    if ! phase_install; then
        end_phase
        return
    fi
    # From here on Windows starts from its disk and the DVD drives are for
    # what the simulation tells the guest.
    settle
    run storageattach "$VM" --storagectl sata --port 1 --device 0 --type dvddrive --medium emptydrive --forceunmount
    dvd emptydrive

    echo "==== phase win-restart on $WHERE"
    phase_restart
    if [ "$BUTTON_WORKS" = no ]; then
        echo "==== phase win-control on $WHERE"
        phase_control
    fi
    if [ "$(vmstate)" = poweroff ]; then
        echo "==== phase win-tpm on $WHERE"
        phase_tpm
    fi
    if [ "$(vmstate)" = poweroff ] && [ -s "${WIN_GA_ISO:-}" ]; then
        echo "==== phase win-gfx3d on $WHERE"
        phase_gfx3d
    fi
    end_phase
    ls -l "$SIM_WORK/win.vdi" 2>&1 | sed 's/^/    /'
}


# ---- in the virtual machine standing in for a host

win_reads() # <what> [<seconds>]
{
    if wait_serial "^WIN: READ [0-9]+ md5=$EXPECT_MD5\$" "${2:-420}"; then
        stage "$1" ok
        return 0
    fi
    stage "$1" FAILED "last: $(serial_new | grep -a -E '^WIN: (READ|NODRIVE|DRIVE|PRESENT|GONE)' | tail -n 1 | cut -c1-110)"
    diag
    return 1
}

win_has_drive() # <what> [<seconds>]
{
    if wait_serial "$DRIVE_RE" "${2:-420}"; then
        stage "$1" ok
        serial_show '^WIN: (PRESENT|DRIVE|READ|NODRIVE)' 6
        return 0
    fi
    stage "$1" FAILED "last: $(serial_new | grep -a -E '^WIN: (PRESENT|GONE)' | tail -n 1 | cut -c1-140)"
    diag
    return 1
}

win_drive_gone() # <what> [<seconds>]
{
    if wait_serial '^WIN: GONE [0-9]+ at=1\.0\.0' "${2:-300}"; then
        stage "$1" ok
        return 0
    fi
    stage "$1" FAILED "last: $(serial_new | grep -a -E '^WIN: (PRESENT|GONE)' | tail -n 1 | cut -c1-140)"
    return 1
}

# The device of the host has a second function.
two_funcs()
{
    [ "$HOSTFUNCS" != "$HOSTADDR" ]
}

# What Windows makes of the second function, where the device has one: its
# driver has the controller, and what is read from its drive is what is on it.
# The second function of the device is a High Definition Audio controller,
# which is PCI class 0403.
second_is_audio()
{
    two_funcs || return 1
    case "$(cat "/sys/bus/pci/devices/${HOSTFUNCS##* }/class" 2> /dev/null)" in
        0x0403*) return 0 ;;
    esac
    return 1
}

win_second() # <what> [<seconds>]
{
    two_funcs || return 0
    if second_is_audio; then
        # Its driver has the controller, and it has found the codec behind it,
        # which takes commands to the controller and answers from it.
        local what=${1/and reads from its drive/and finds its codec}
        # A Windows without a driver for such a controller has the function
        # and nothing to run it with (problem 28, which is what the Server
        # Core of the simulation tells: run 285).  That is told once, and
        # what needs the driver is left out.
        if    wait_serial '^WIN: PRESENT [0-9]+ at=1\.0\.1 ' "${2:-420}" \
           && serial_has '^WIN: PRESENT [0-9]+ at=1\.0\.1 .* problem=28 '; then
            if [ "${AUDIO_DRIVER:-}" != no ]; then
                stage "guest: has a driver for the audio controller" skipped "this Windows has none for it (problem 28); the function is there"
            fi
            AUDIO_DRIVER=no
            return 0
        fi
        if    wait_serial "$AUDIO_RE" "${2:-420}" \
           && wait_serial '^WIN: AUDIO [0-9]+ at=1\.0\.1 codecs=[1-9]' 420; then
            stage "$what" ok "$(serial_new | grep -a '^WIN: AUDIO [0-9]* at=1\.0\.1 ' | tail -n 1 | sed -e 's/^WIN: AUDIO [0-9]* at=1\.0\.1 //' | cut -c1-60)"
            serial_show '^WIN: (PRESENT [0-9]+ at=1\.0\.1|AUDIO)' 6
            return 0
        fi
        stage "$what" FAILED "last: $(serial_new | grep -a -E '^WIN: (PRESENT [0-9]+ at=1\.0\.1|AUDIO|GONE)' | tail -n 1 | cut -c1-140)"
        diag
        return 1
    fi
    if    wait_serial "$DRIVE2_RE" "${2:-420}" \
       && wait_serial "^WIN: READ2 [0-9]+ md5=$EXPECT_MD5B\$" 420; then
        stage "$1" ok
        serial_show '^WIN: (PRESENT [0-9]+ at=1\.0\.1|DRIVE2|READ2|NODRIVE2|DRIVE-NOTE)' 6
        return 0
    fi
    stage "$1" FAILED "last: $(serial_new | grep -a -E '^WIN: (PRESENT [0-9]+ at=1\.0\.1|READ2|NODRIVE2|GONE)' | tail -n 1 | cut -c1-140)"
    diag
    return 1
}

win_second_gone() # <what> [<seconds>]
{
    two_funcs || return 0
    if wait_serial '^WIN: GONE [0-9]+ at=1\.0\.1' "${2:-300}"; then
        stage "$1" ok
        return 0
    fi
    stage "$1" FAILED "last: $(serial_new | grep -a -E '^WIN: (PRESENT|GONE)' | tail -n 2 | cut -c1-140 | tr '\n' '|')"
    return 1
}

# What the host has in /proc/interrupts for a function of the device: a line
# "vfio-intx(<address>)" while the function has a line interrupt set up, and
# lines "vfio-msix[<n>](<address>)" or "vfio-msi[<n>](<address>)" while it
# has messages.
host_irq_lines() # <address>
{
    grep -a -F "($1)" /proc/interrupts | grep -a 'vfio-'
}

# How often an interrupt of the host has come, all CPUs together, by a copy of
# /proc/interrupts; 0 if it is not in there.
irq_count() # <number> <file>
{
    awk -v n="$1:" '$1 == n { c = 0; for (i = 2; i <= NF && $i ~ /^[0-9]+$/; i++) c += $i; print c; f = 1; exit } END { if (!f) print 0 }' "$2" 2> /dev/null
}

# The drive with the interrupt of old.  Windows runs an NVMe controller with
# MSI-X; a DVD with a word for the script in the guest has it told not to
# (MSISupported in the registry, which is how a driver is told) and the
# controller started anew, so that it gets a line interrupt, INTx.  That is
# what Windows gives the audio function of a graphics card unless it is told
# otherwise.  Reading eight megabytes from the drive then takes many INTx
# interrupts, each of which the device passes on as a pulse.  A build without
# that is told by a message the device has since, and this is left out there.
win_intx()
{
    [ -s "${WIN_CMD_INTX_ISO:-}" ] || return 0
    local what="guest: runs the controller of the drive with a line interrupt (INTx)"
    if ! grep -a -q 'Asking the device for its next interrupt' "$VBOX_BIN/VBoxDD.so" 2> /dev/null; then
        stage "$what" skipped "this build passes the first INTx interrupt of a function on and no more"
        return 0
    fi
    local before="$SIM_WORK/interrupts-before-intx" after="$SIM_WORK/interrupts-after-intx"
    local f n=0 at tag md5 name reads irq msgs c0 c1 told
    cat /proc/interrupts > "$before" 2> /dev/null
    echo "    ---- the interrupts the host has for the device, before:"
    for f in $HOSTFUNCS; do host_irq_lines "$f" | tr -s ' ' | cut -c1-160 | sed 's/^/    | /'; done
    serial_mark
    dvd "$WIN_CMD_INTX_ISO"
    if ! wait_serial '^WIN: INTX-DONE ' 900; then
        stage "$what" FAILED "its script did not get done with it"
        serial_show '^WIN: INTX' 10
        diag
        dvd emptydrive
        return 1
    fi
    serial_show '^WIN: (INTX|DRIVE|READ|NODRIVE)' 14
    cat /proc/interrupts > "$after" 2> /dev/null
    echo "    ---- the interrupts the host has for the device, after:"
    for f in $HOSTFUNCS; do host_irq_lines "$f" | tr -s ' ' | cut -c1-160 | sed 's/^/    | /'; done
    for f in $HOSTFUNCS; do
        if [ "$n" != 0 ] && second_is_audio; then
            # The script in the guest does this with the controllers of drives.
            n=$((n + 1))
            continue
        fi
        at="1\\.0\\.$n"
        if [ "$n" = 0 ]; then
            tag=; md5=$EXPECT_MD5; name=$what; reads="guest: reads from the drive with its INTx interrupt"
        else
            tag=2; md5=$EXPECT_MD5B; name="guest: the same with the second function"
            reads="guest: reads from the drive of the second function with its INTx interrupt"
        fi
        # The guest tells that its driver has the controller again.  What kind of
        # interrupt the function has is what the host tells: a line interrupt
        # and no messages.  The numbers Windows has for it are told along.
        irq=$(grep -a -F "vfio-intx($f)" "$after" | head -n 1 | sed -n 's/^ *\([0-9]*\):.*$/\1/p')
        msgs=$(grep -a -F "($f)" "$after" | grep -a -c -E 'vfio-msix?\[')
        told=$(serial_new | grep -a -E "^WIN: INTX [0-9]+ at=$at " | tail -n 1 | sed -e 's/^WIN: INTX [0-9]* //' | cut -c1-90)
        if serial_has "^WIN: INTX [0-9]+ at=$at status=OK " && [ -n "$irq" ] && [ "$msgs" = 0 ]; then
            stage "$name" ok "interrupt $irq of the host; the guest: $told"
        else
            stage "$name" FAILED "the host has ${irq:+interrupt $irq as its line interrupt and }$msgs message interrupt(s) for $f; the guest: ${told:-nothing}"
        fi
        # Reading eight megabytes takes interrupts: what was read is what is on
        # the drive, and the host has counted its line interrupt meanwhile.
        c0=$(irq_count "${irq:-none}" "$before")
        c1=$(irq_count "${irq:-none}" "$after")
        : "${c0:=0}" "${c1:=0}"
        if serial_has "^WIN: READ$tag [0-9]+ md5=$md5\$" && [ -n "$irq" ] && [ "$c1" -gt "$c0" ]; then
            stage "$reads" ok "$((c1 - c0)) interrupts of the host meanwhile"
        else
            stage "$reads" FAILED "last: $(serial_new | grep -a -E "^WIN: (READ|NODRIVE|DRIVE)$tag " | tail -n 1 | cut -c1-100); interrupt ${irq:-none} of the host: $c0 before, $c1 after"
        fi
        n=$((n + 1))
    done
    dvd emptydrive
    guest_goes_on "the guest keeps running with the drive at INTx" 300
}

# Sound over the audio controller, where the second function of the device is
# one.  Which interrupt Windows runs the controller with is its own choice and
# is told by the host, see host_irq_lines.  Then the script in the guest plays
# three seconds of silence, which takes the controller an interrupt for each
# piece of it: with too few of them it takes longer, with none it does not
# end.  The sound goes nowhere on the simulated host, in the time it takes.
win_audio() # <what is added to the name of the check>
{
    second_is_audio || return 0
    [ -s "${WIN_CMD_AUDIO_ISO:-}" ] || return 0
    # Without a driver for the controller there is nothing to play with and
    # no interrupt set up by one, see win_second.
    [ "${AUDIO_DRIVER:-}" = no ] && return 0
    local f=${HOSTFUNCS##* } before="$SIM_WORK/interrupts-before-audio" after="$SIM_WORK/interrupts-after-audio"
    local irq msgs line ms c0 c1 kind
    echo "    ---- the interrupts the host has for the audio controller:"
    host_irq_lines "$f" | tr -s ' ' | cut -c1-160 | sed 's/^/    | /'
    cat /proc/interrupts > "$before" 2> /dev/null
    irq=$(grep -a -F "vfio-intx($f)" "$before" | head -n 1 | sed -n 's/^ *\([0-9]*\):.*$/\1/p')
    msgs=$(grep -a -F "($f)" "$before" | grep -a -c -E 'vfio-msix?\[')
    if [ -n "$irq" ] && [ "$msgs" = 0 ]; then
        kind="a line interrupt (INTx), interrupt $irq of the host"
    elif [ "$msgs" != 0 ]; then
        kind="$msgs message interrupt(s)"
        irq=$(grep -a -F "($f)" "$before" | grep -a -E 'vfio-msix?\[' | head -n 1 | sed -n 's/^ *\([0-9]*\):.*$/\1/p')
    else
        kind=
    fi
    if [ -n "$kind" ]; then
        stage "host: the audio controller has an interrupt set up$1" ok "$kind"
    else
        stage "host: the audio controller has an interrupt set up$1" FAILED "the host has none for $f"
    fi
    serial_mark
    dvd "$WIN_CMD_AUDIO_ISO"
    if wait_serial '^WIN: AUDIO-PLAY ' 600; then
        cat /proc/interrupts > "$after" 2> /dev/null
        line=$(serial_new | grep -a '^WIN: AUDIO-PLAY ' | tail -n 1)
        ms=$(echo "$line" | sed -n 's/^WIN: AUDIO-PLAY [0-9]* ms=\([0-9]*\) .*$/\1/p')
        c0=$(irq_count "${irq:-none}" "$before")
        c1=$(irq_count "${irq:-none}" "$after")
        : "${c0:=0}" "${c1:=0}"
        serial_show '^WIN: AUDIO' 8
        if [ -n "$ms" ] && [ "$ms" -ge 2500 ] && [ "$ms" -le 12000 ] && [ "$c1" -gt "$c0" ]; then
            stage "guest: plays three seconds of sound over the audio controller$1" ok "$ms ms, $((c1 - c0)) interrupts of the host meanwhile"
        elif [ -n "$ms" ] && [ "$ms" -lt 2500 ]; then
            stage "guest: plays three seconds of sound over the audio controller$1" skipped "Windows was done after $ms ms: it has nothing to play sound with"
        else
            stage "guest: plays three seconds of sound over the audio controller$1" FAILED "$(echo "$line" | cut -c1-120); interrupt ${irq:-none} of the host: $c0 before, $c1 after"
        fi
    else
        stage "guest: plays three seconds of sound over the audio controller$1" FAILED "the guest did not tell of it"
        serial_show '^WIN: AUDIO' 8
        diag
    fi
    dvd emptydrive
    guest_goes_on "the guest keeps running after the sound$1" 300
}

# A guest which has stopped telling anything while its VM runs (run 251: from
# the moment it had switched the slot off, with the events of the slot left
# unanswered).  What shows of it from the outside: the slot and the message
# the port interrupts with, where CPU 0 is and whether the guest still leaves
# for VirtualBox (a guest which waits does not move, one which spins does),
# the interrupt controllers, and a picture.
frozen_note()
{
    local i what
    echo "    ---- the guest says nothing any more; what the VM shows of it:"
    vbm debugvm "$VM" info pci verbose 2> /dev/null | perl "$SIMLIB/slotregs.pl" 1 | head -n 1 | cut -c1-500 | sed 's/^/    | /'
    for i in 1 2 3 4 5; do
        echo "    | exits $(exits_now), $(VBM_TIMEOUT=30 vbm debugvm "$VM" getregisters --cpu=0 rip rflags cr8 2>&1 | tr -d '\r' | grep -a -E '^(rip|rflags|cr8)|rror' | tr -s ' ' | tr '\n' ' ' | cut -c1-160)"
        sleep 3
    done
    echo "    ---- what KVM counts for the VM, twice, ten seconds apart:"
    kvm_counters | cut -c1-400 | sed 's/^/    | /'
    sleep 10
    kvm_counters | cut -c1-400 | sed 's/^/    | /'
    for what in apic lapic ioapic; do
        echo "    ---- debugvm info $what:"
        VBM_TIMEOUT=60 vbm debugvm "$VM" info "$what" 2>&1 | tr -d '\r' | grep -a -v '^ *$' | head -n 45 | cut -c1-160 | sed 's/^/    | /'
    done
    VBM_TIMEOUT=60 vbm debugvm "$VM" statistics --pattern '/NEM/CPU0/Exit*|/Devices/ioapic/*|/Public/Storage/AHCI0/Port0/Reqs*' 2> /dev/null \
        | grep -a -o -E '<Counter c="[1-9][0-9]*"[^>]*name="[^"]*"' \
        | sed -E 's/^<Counter c="([0-9]+)".*name="([^"]*)"$/\2: \1/' | head -n 40 | sed 's/^/    | /'
    shot
    # Is it the whole guest, or what it does about its devices?  The keyboard
    # and the power button are interrupts which have nothing to do with the
    # slot.  The window of Windows Server takes "15" and Enter for leaving to
    # a command line, which changes the picture; the power button shuts
    # Windows down, or at least has it say so on the screen.
    local before
    before=$(exits_now)
    echo "+ VBoxManage controlvm $VM keyboardputscancode: 1, 5, Enter"
    vbm controlvm "$VM" keyboardputscancode 02 82 06 86 1c 9c > /dev/null 2>&1
    sleep 20
    echo "    | exits before the keys $before, 20 s after them $(exits_now)"
    shot
    run controlvm "$VM" acpipowerbutton
    if wait_off 120; then
        echo "    | the guest shut down when the power button was pressed: it was alive, and what stood still was less than all of it"
    else
        echo "    | the VM is in state '$(vmstate)' 120 s after the power button was pressed"
        irq_state "120 s after the power button was pressed"
        shot
    fi
}

# The drive in the hot-plug capable slot of a VM with Windows: there when
# Windows starts, then taken out and put in while it runs, with the guest
# being asked and without.
# One error of the device, as the host tells of it, while Windows uses the
# drive in the slot.  The port in front of the device is made to report it
# from outside this machine, see host_request and phase_hosterror in
# vbox-phases.sh, where the same is done with the Linux guest.
win_host_error_round() # <what the error is called> <status>
{
    local what=$1 status=$2 log="$VMDIR/Logs/VBox.log" n0 i=0
    n0=$(grep -a -c 'the host reported an error of the device' "$log" 2> /dev/null)
    serial_mark
    dmesg -C 2> /dev/null
    echo "+ the port in front of the device reports an error: $what (status $status)"
    if ! host_request aer "$status"; then
        stage "host: the port in front of the device reports an error which is $what" FAILED "the machine was not made to"
        return 1
    fi
    stage "host: the port in front of the device reports an error which is $what" ok
    while [ "$i" -lt 60 ] && [ "$(grep -a -c 'the host reported an error of the device' "$log" 2> /dev/null)" = "${n0:-0}" ]; do
        sleep 1
        i=$((i + 1))
    done
    dmesg | grep -i -E 'aer|pcieport|reset' | cut -c1-200 | head -n 20 | sed 's/^/    | /'
    if [ "$(grep -a -c 'the host reported an error of the device' "$log" 2> /dev/null)" != "${n0:-0}" ]; then
        stage "VirtualBox is told of the error and takes the device out of service ($what)" ok "after $i s"
    else
        stage "VirtualBox is told of the error and takes the device out of service ($what)" FAILED "nothing in the log of the VM after $i s"
        diag
        return 1
    fi
    grep -a -E 'taken out of service|reported an error' "$log" | tail -n 4 | cut -c1-200 | sed 's/^/    | /'
    win_drive_gone "guest: notices that the drive is gone ($what)" 420
    win_second_gone "guest: notices that the second function is gone ($what)" 420
    if ! guest_goes_on "the VM runs on after the error ($what)" 300; then
        frozen_note
        WIN_UP=no
        return 1
    fi
    check_pcistate hotplug,absent,unused
    [ -e "/sys/bus/pci/devices/$HOSTADDR" ] || echo 1 > /sys/bus/pci/rescan
    if ! bind_dev; then
        stage "host: has the device after the error ($what)" FAILED
        return 1
    fi
    stage "host: has the device after the error ($what)" ok
    serial_mark
    if VBM_TIMEOUT=300 run controlvm "$VM" plugpci "$HOSTADDR"; then
        stage "plugpci after the error ($what)" ok
    else
        stage "plugpci after the error ($what)" FAILED
        diag
        return 1
    fi
    win_has_drive "guest: takes the device again after the error, its driver has it ($what)" 600
    win_reads "guest: reads from the drive after the error ($what)"
    win_second "guest: takes the second function again after the error ($what)" 600
}

# The host reports an error of the device, first one its kernel does not take
# for fatal, then one it does.  Only where there is somebody outside this
# machine to make the port report it.
win_host_error()
{
    [ -n "${SIM_HOSTAGENT:-}" ] || return 0
    if win_host_error_round "not fatal" 0x8000; then
        win_host_error_round "fatal" 0x40000
    fi
    return 0
}

# The device is unplugged from the host and plugged in again while Windows
# uses it, see phase_hostplug in vbox-phases.sh, where the same is done with
# the Linux guest: the hot-plug of the host's own kernel takes the device out,
# VirtualBox is asked and asks Windows, and afterwards the host has no such
# device.  Only in a machine which is made for it (SIM_HOSTPLUG, run.sh).
win_host_plug()
{
    [ -n "${SIM_HOSTPLUG:-}" ] || return 0
    local i t0 drv
    serial_mark
    dmesg -C 2> /dev/null
    port_says "with the device in the VM"
    echo "+ the device is unplugged from the host"
    t0=$(date +%s)
    if ! host_request unplug; then
        stage "host: is told that the device is to be unplugged" FAILED
        return 1
    fi
    stage "host: is told that the device is to be unplugged" ok
    i=0
    while host_has_any && [ "$i" -lt 420 ]; do
        sleep 1
        i=$((i + 1))
    done
    dmesg | grep -i -E 'pciehp|vfio' | cut -c1-200 | head -n 20 | sed 's/^/    | /'
    if ! host_has_any; then
        stage "host: the device is gone from it" ok "$(( $(date +%s) - t0 )) s after it was told"
    else
        stage "host: the device is gone from it" FAILED "it is still there after $i s"
        diag
        return 1
    fi
    grep -a -E 'taken out of service|wants the device back' "$VMDIR/Logs/VBox.log" | tail -n 4 | cut -c1-200 | sed 's/^/    | /'
    win_drive_gone "guest: the drive is gone after the device was unplugged from the host" 420
    win_second_gone "guest: the second function is gone with it" 420
    if ! guest_goes_on "the VM runs on after the device was unplugged from the host" 300; then
        frozen_note
        WIN_UP=no
        return 1
    fi
    check_pcistate hotplug,absent,unused
    # What is not on the host cannot be put into the VM.
    if VBM_TIMEOUT=300 run controlvm "$VM" plugpci "$HOSTADDR"; then
        stage "plugpci of the device which is not on the host is refused" FAILED "it was taken"
    else
        stage "plugpci of the device which is not on the host is refused" ok
    fi

    if ! port_link_has_width; then
        stage "host: the device is plugged in again" skipped "the emulated port tells of a link without a width, which is its emulator's doing (a port which was made to report an error, and a port of its switch from the start), and the kernel of the host takes no device from such a port"
        return 0
    fi
    dmesg -C 2> /dev/null
    echo "+ the device is plugged into the host again"
    if ! host_request plug; then
        stage "host: the device is plugged in again" FAILED
        return 1
    fi
    stage "host: the device is plugged in again" ok
    i=0
    while ! host_has_all && [ "$i" -lt 120 ]; do
        sleep 1
        i=$((i + 1))
    done
    dmesg | grep -i -E 'pciehp|pci 0000:01' | cut -c1-200 | head -n 20 | sed 's/^/    | /'
    port_says "with the device plugged in again"
    if host_has_all; then
        drv=
        [ -e "/sys/bus/pci/devices/$HOSTADDR/driver" ] && drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$HOSTADDR/driver")")
        stage "host: has the device again" ok "after $i s, with ${drv:-no} driver"
    else
        stage "host: has the device again" FAILED "after $i s"
        diag
        return 1
    fi
    if bind_dev; then
        stage "host: the device which came back is bound to vfio-pci" ok
    else
        stage "host: the device which came back is bound to vfio-pci" FAILED
        return 1
    fi
    serial_mark
    if VBM_TIMEOUT=300 run controlvm "$VM" plugpci "$HOSTADDR"; then
        stage "plugpci of the device which came back" ok
    else
        stage "plugpci of the device which came back" FAILED
        diag
        return 1
    fi
    win_has_drive "guest: takes the device which came back, its driver has it" 600
    win_reads "guest: reads from the drive of the device which came back"
    win_second "guest: takes the second function of the device which came back" 600
    check_pcistate hotplug,present,inuse
    return 0
}

phase_hotplug()
{
    PHASE=win-hotplug
    stalls_reset
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    start_and_report "Windows starts with the drive in the hot-plug capable slot" "$START_WAIT" || return
    shot
    report_checks "" hotplug,present,inuse
    win_has_drive "guest: has the drive in the slot, with its driver" || { shot; return; }
    win_reads "guest: reads from the drive in the slot"
    win_second "guest: has the second function too, with its driver, and reads from its drive"
    win_audio ""
    local lay t0 second=${HOSTFUNCS##* }
    lay=$(vbm getextradata "$VM" "VBoxInternal2/HostPCILayout/$HOSTADDR" 2> /dev/null | sed -n 's/^Value: //p')
    echo "    layout stored: ${lay:-none}"

    serial_mark
    t0=$(date +%s)
    if VBM_TIMEOUT=400 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=300000; then
        stage "unplugpci: the guest is asked and lets go of the device" ok "$(( $(date +%s) - t0 )) s"
    else
        stage "unplugpci: the guest is asked and lets go of the device" FAILED "$(( $(date +%s) - t0 )) s"
        diag
    fi
    win_drive_gone "guest: the drive is gone"
    win_second_gone "guest: the second function is gone with it"
    check_pcistate hotplug,absent,unused
    slot_registers_check
    if ! guest_goes_on "the guest keeps running" 300; then
        # Nothing of what follows can be told of a guest which says nothing,
        # and waiting for it took 70 minutes of run 251.  It is looked at, and
        # the phase ends; the VM is powered off without asking the guest.
        frozen_note
        WIN_UP=no
        return
    fi

    if [ "$(pcistate)" = hotplug,absent,unused ]; then
        serial_mark
        # Where the device has two functions the command names the second
        # one here: a device is all of its functions, and any of them names it.
        if VBM_TIMEOUT=300 run controlvm "$VM" plugpci "$second"; then
            stage "plugpci puts the device into the slot" ok
        else
            stage "plugpci puts the device into the slot" FAILED
            diag
        fi
        win_has_drive "guest: takes the device in the slot, its driver has it" 600
        win_reads "guest: reads from the drive which was put in"
        win_second "guest: takes the second function as well and reads from its drive" 600
        check_pcistate hotplug,present,inuse
        slot_registers_check
    fi

    serial_mark
    if VBM_TIMEOUT=300 run controlvm "$VM" unplugpci "$HOSTADDR" --timeout=1000 --force; then
        stage "unplugpci --force takes the device away from a guest which uses it" ok
    else
        stage "unplugpci --force takes the device away from a guest which uses it" FAILED
        diag
    fi
    win_drive_gone "guest: notices that the drive is gone" 420
    win_second_gone "guest: notices that the second function is gone" 420
    if ! guest_goes_on "the VM runs on after that" 300; then
        frozen_note
        WIN_UP=no
        return
    fi

    serial_mark
    if VBM_TIMEOUT=300 run controlvm "$VM" plugpci "$HOSTADDR"; then
        stage "plugpci after the forced removal" ok
    else
        stage "plugpci after the forced removal" FAILED
        diag
    fi
    win_has_drive "guest: takes the device again, its driver has it" 600
    win_reads "guest: reads from the drive again"
    win_second "guest: takes the second function again and reads from its drive" 600
    win_intx
    win_audio ", with the drive at INTx as well"
    # The device is unplugged from the host and plugged in again, and the host
    # reports an error of it.  In this order where the port still has its
    # link: after the first error it has not, and the device could not be
    # plugged in again (see port_link_has_width in vbox-phases.sh, and the
    # part swplughost in run.sh).  The port had its link here in both parts of
    # run 343, the one whose device is reset by a reset of its bus among
    # them.  Should it not, the unplugging comes last and is all there is of
    # it.
    if port_link_has_width; then
        win_host_plug
        [ "$WIN_UP" = yes ] && win_host_error
    else
        win_host_error
        [ "$WIN_UP" = yes ] && win_host_plug
    fi
    check_log_quiet
    shot
}

mode_nested()
{
    : "${WIN_DISK_DEV:?}" "${SIM_HOSTADDR:?}"
    PHASE=win-setup
    START_WAIT=${WIN_START_WAIT:-3600}
    SHUTDOWN_WAIT=600
    echo "==== VirtualBox on $WHERE, Windows as the guest: $("$VBOX_BIN/VBoxManage" --version 2>&1 | tail -n 1)"
    ls -l /dev/kvm /dev/iommu "$WIN_DISK_DEV" "$WIN_CMD_ISO" 2>&1 | sed 's/^/    /'
    run setproperty machinefolder "$SIM_WORK/vms"
    # The disk of Windows is a disk of this machine, used as it is.
    if ! run createmedium disk --filename "$SIM_WORK/win-raw.vmdk" --format=VMDK --variant=RawDisk --property=RawDrive="$WIN_DISK_DEV"; then
        stage "the VM for Windows is created" FAILED "no disk"
        return
    fi
    # 2 GiB, which is what Windows Server asks for at least: the less memory,
    # the less there is for Windows to go through when it starts.
    create_vm 1 2048 "$SIM_WORK/win-raw.vmdk" emptydrive emptydrive "$HOSTADDR" "" || return
    stage "the VM for Windows is created, with the drive in the hot-plug capable slot" ok
    # The device of two functions is one whose functions the host cannot
    # reset by themselves here (SIM_NORESET, run.sh), as it is with many
    # graphics cards: every reset of the device, which is what Windows
    # switching the slot off comes to, is a reset of the bus then.
    if [ "${SIM_NORESET:-}" = 1 ] && two_funcs; then
        if forget_resets; then
            stage "the host has no reset for a function of the device by itself" ok
        else
            stage "the host has no reset for a function of the device by itself" skipped "its kernel does not let that be set"
        fi
    fi

    echo "==== phase win-hotplug on $WHERE"
    phase_hotplug
    stalls_stage
    # Without a Windows which got up there was no reset to look for.
    if [ "$RESETS_FORGOTTEN" = yes ] && [ "$WIN_UP" = yes ]; then
        check_bus_resets
    fi
    # A Windows which did not get through its start is not asked to shut
    # down: its script is not there yet for hearing it, and waiting for that
    # took 20 minutes of run 230.
    if [ "$(vmstate)" = running ] && [ "$WIN_UP" = yes ]; then
        guest_shutdown
    else
        end_phase
    fi
    show_exits "$SIM_OUT/VBox-$PHASE.log"
    [ "$RESETS_FORGOTTEN" = yes ] && remember_resets
    return 0
}


case "$MODE" in
    runner) mode_runner ;;
    nested) mode_nested ;;
    *)      echo "unknown mode $MODE" ;;
esac

cp -f "$VBOX_USER_HOME/VBoxSVC.log" "$SIM_OUT/VBoxSVC-win.log" 2> /dev/null
pkill -x VBoxSVC 2> /dev/null
exit 0
