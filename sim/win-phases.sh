#!/bin/bash
# Part of the simulation, see run.sh: Windows as the guest of a VM with an
# empty hot-plug capable slot, on the runner (which has KVM and no IOMMU, so
# there is no device to put into the slot).
#
# Windows Server is installed from its evaluation image without anybody at the
# keyboard (win/Autounattend.xml on a second DVD), into the VM as it is
# configured for a device of the host with hot-plug.  So setup and the
# installed Windows both meet the PCI Express root bridge, what the firmware
# hands over and the port with the slot.  When it is installed, a script in
# the guest (win/report.ps1) tells on the serial port what Windows made of
# them, and the slot registers are read from the outside.
#
# Results are "STAGE ..." lines as in vbox-phases.sh, whose functions are used.

set -u
: "${VBOX_BIN:?}" "${SIM_WORK:?}" "${SIM_OUT:?}" "${SIM_STAGES:?}" "${WIN_ISO:?}" "${WIN_UNATTEND_ISO:?}"
GUEST_RAW=none
SIM_LIB_ONLY=1
. "$(cd "$(dirname "$0")" && pwd)/vbox-phases.sh"

VM=simwin
SERIAL=$SIM_WORK/win-serial.log
UP_RE='WIN: UP'
ALIVE_RE='WIN: ALIVE'
INSTALL_WAIT=${WIN_INSTALL_WAIT:-4800}
RESTART_WAIT=${WIN_RESTART_WAIT:-900}
SHOTS=0
MARKS='^WIN: (SETUP|SPECIALIZE|FIRSTLOGON|UP|OS=|REPORT-END)'


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

# Waits for a line of the guest.  Tells what the guest says about where it is
# meanwhile, and takes a picture of the screen every three minutes.
wait_win() # <regex> <seconds>
{
    local t0 now n st last=0 shown=$SERIAL_FROM
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
            echo "    [$now s] waiting, the VM is $st, its disk image has $(du -h "$SIM_WORK/win.vdi" 2> /dev/null | cut -f1)"
            shot
        fi
        sleep 5
    done
}

# The number of exits from the guest which the log of a VM which has ended
# tells, for knowing what a start of Windows costs where exits are slow.
show_exits() # <log>
{
    local n
    n=$(grep -a -E '^[0-9:.]+ /NEM/CPU[0-9]+/Exit +[0-9]+ times' "$1" 2> /dev/null | awk '{ s += $3 } END { print s + 0 }')
    echo "    exits from the guest in $(basename "$1"): ${n:-0}"
    grep -a -E '^[0-9:.]+ /NEM/CPU[0-9]+/Exit(/(Io|Mmio|Intr|RdMsr|WrMsr|Hlt))? +[0-9]+ times' "$1" 2> /dev/null \
        | sed -E 's/^[0-9:.]+ //; s/ +/ /g' | sed 's/^/    | /'
}

serial_value() # <regex with one group>: what the group matched in the first line which matches
{
    serial_new | sed -n -E "s/^.*$1.*\$/\\1/p" | head -n 1
}


setup_win()
{
    PHASE=win-setup
    echo "==== VirtualBox on $WHERE, Windows as the guest: $("$VBOX_BIN/VBoxManage" --version 2>&1 | tail -n 1)"
    ls -l /dev/kvm /dev/iommu "$WIN_ISO" "$WIN_UNATTEND_ISO" 2>&1 | sed 's/^/    /'
    run setproperty machinefolder "$SIM_WORK/vms"
    local os=
    for os in Windows2025_64 Windows2022_64 Windows11_64 Windows10_64 Other_64; do
        vbm list ostypes 2> /dev/null | grep -q -E "^ID: +$os\$" && break
    done
    echo "    type of the guest: $os"
    if ! run createvm --name "$VM" --ostype "$os" --register; then
        stage "the VM for Windows is created" FAILED
        return 1
    fi
    run modifyvm "$VM" --memory 4096 --cpus 2 --ioapic on --chipset ich9 --firmware efi \
        --vm-execution-engine native-api --nic1 none --audio-enabled off \
        --graphicscontroller vboxsvga --vram 64 --uart1 0x3f8 4 --uartmode1 file "$SERIAL" \
        --boot1 dvd --boot2 disk --boot3 none --boot4 none
    run storagectl "$VM" --name sata --add sata --controller IntelAhci --portcount 3
    if    ! run createmedium disk --filename "$SIM_WORK/win.vdi" --size 40960 --format VDI \
       || ! run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$SIM_WORK/win.vdi" \
       || ! run storageattach "$VM" --storagectl sata --port 1 --device 0 --type dvddrive --medium "$WIN_ISO" \
       || ! run storageattach "$VM" --storagectl sata --port 2 --device 0 --type dvddrive --medium "$WIN_UNATTEND_ISO"; then
        stage "the VM for Windows is created" FAILED "disk or DVD"
        return 1
    fi
    # The slot: configured for a device of the host which is not there, with a
    # layout to be built from, as in the phase emptyslot of vbox-phases.sh.
    run modifyvm "$VM" --pci-attach "$FAKE_ADDR@0000:01:00.0" && ATTACHED=$FAKE_ADDR
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
    run setextradata "$VM" "VBoxInternal2/HostPCILayout/$FAKE_ADDR" "$FAKE_LAYOUT"
    VMDIR=$(dirname "$(vbm showvminfo "$VM" --machinereadable | sed -n 's/^CfgFile="\(.*\)"$/\1/p')")
    vbm showvminfo "$VM" --machinereadable \
        | grep -a -i -E '^"?(ostype|memory|cpus|chipset|firmware|VMExecutionEngine|paravirtprovider|graphicscontroller|uart1|SATA-[0-9]|AttachedHostPCI)' \
        | cut -c1-200 | sed 's/^/    /'
    stage "the VM for Windows is created, with an empty hot-plug capable slot" ok
}


# What the report of the guest says, and what the outside sees.
report_checks() # <"" or text put in front of each check>
{
    local pre=$1 v n
    if [ -z "$pre" ]; then
        serial_show '^WIN: (OS=|CPUS=|ROOTBUS |ROOTBUS-PROP |ROOTBUSES |DEV |DEVS |PORT |PORT-PROP |PROBLEM |PROBLEMS |EVENT |EVENTS |BUGCHECK |BUGCHECKS )' 120
    else
        serial_show '^WIN: (OS=|ROOTBUS |ROOTBUS-PROP .*HotPlugControl|PORT |DEVS |PROBLEM |PROBLEMS |EVENTS |BUGCHECKS )' 30
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
    # them, with or without this work.
    n=$(serial_new | grep -a -E '^WIN: DEV .* problem=(-|[1-9])' | grep -a -c -v 'VEN_80EE&DEV_CAFE')
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
    check_pcistate hotplug,absent,unused
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
        shot
        diag
        return 1
    fi
    stage "Windows is installed in the VM with the empty hot-plug capable slot" ok \
          "$(( $(date +%s) - t0 )) s, $(serial_value 'WIN: OS=(.*)')"
    shot
    report_checks ""

    if run controlvm "$VM" plugpci "$FAKE_ADDR"; then
        stage "plugpci of a device which is not there is refused" FAILED "it went through"
    else
        stage "plugpci of a device which is not there is refused" ok
    fi
    guest_goes_on "the VM runs on after that" 120
    saved_state_round && guest_goes_on "the guest keeps running after the state was loaded" 120

    # Windows shuts down when the power button is pressed.
    case "$(vmstate)" in
        running)
            run controlvm "$VM" acpipowerbutton
            if wait_state poweroff 300; then
                stage "guest: shuts down when the power button is pressed" ok
            else
                stage "guest: shuts down when the power button is pressed" FAILED "the VM is in state '$(vmstate)'"
                shot
            fi
            ;;
    esac
    end_phase
    show_exits "$SIM_OUT/VBox-$PHASE-saved.log"
}


# The installed Windows started once more: the same again, and what a start
# costs.
phase_restart()
{
    PHASE=win-restart
    [ "$(vmstate)" = saved ] && run discardstate "$VM"
    settle
    local t0
    t0=$(date +%s)
    if ! start_vm; then
        stage "the installed Windows starts again" FAILED "the VM does not start"
        diag
        return 1
    fi
    if ! wait_win '^WIN: REPORT-END' "$RESTART_WAIT"; then
        stage "the installed Windows starts again" FAILED \
              "the last from the guest is: $(serial_new | grep -a '^WIN: ' | tail -n 1 | cut -c1-60)"
        shot
        diag
        end_phase
        return 1
    fi
    stage "the installed Windows starts again" ok "$(( $(date +%s) - t0 )) s until its report was done"
    report_checks "after the restart: "
    guest_goes_on "the guest keeps running" 120
    end_phase
    show_exits "$SIM_OUT/VBox-$PHASE.log"
}


if setup_win; then
    echo "==== phase win-install on $WHERE"
    if phase_install; then
        echo "==== phase win-restart on $WHERE"
        phase_restart
    else
        end_phase
    fi
fi

cp -f "$VBOX_USER_HOME/VBoxSVC.log" "$SIM_OUT/VBoxSVC-win.log" 2> /dev/null
pkill -x VBoxSVC 2> /dev/null
exit 0
