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
MARKS='^WIN: (SETUP|SPECIALIZE|FIRSTLOGON|BUTTON-DEFAULT|UP|OS=|REPORT-END|SHUTDOWN)'
# The drive behind the port with the slot, as the script in the guest tells
# of it once the driver of Windows has it.
DRIVE_RE='^WIN: PRESENT [0-9]+ at=1\.0\.0 id=[^ ]* status=OK problem=0 service=stornvme'


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
            echo "    [$now s] waiting, the VM is $st"
            shot
        fi
        sleep 5
    done
}

# Waits until the VM is powered off, by the clock.
wait_off() # <seconds>
{
    local t0
    t0=$(date +%s)
    while [ $(( $(date +%s) - t0 )) -lt "$1" ]; do
        [ "$(vmstate)" = poweroff ] && return 0
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
    run storagectl "$VM" --name sata --add sata --controller IntelAhci --portcount 3
    if    ! run storageattach "$VM" --storagectl sata --port 0 --device 0 --type hdd --medium "$3" \
       || ! run storageattach "$VM" --storagectl sata --port 1 --device 0 --type dvddrive --medium "$4" \
       || ! run storageattach "$VM" --storagectl sata --port 2 --device 0 --type dvddrive --medium "$5"; then
        stage "the VM for Windows is created" FAILED "disk or DVD"
        return 1
    fi
    run modifyvm "$VM" --pci-attach "$6@0000:01:00.0" && ATTACHED=$6
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
    if ! start_vm; then
        stage "$1" FAILED "the VM does not start"
        diag
        return 1
    fi
    if ! wait_win '^WIN: REPORT-END' "$2"; then
        stage "$1" FAILED "the last from the guest is: $(serial_new | grep -a '^WIN: ' | tail -n 1 | cut -c1-60)"
        shot
        diag
        return 1
    fi
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
        run controlvm "$VM" poweroff
        wait_state poweroff 60
    fi
    keep_logs
    settle
    dvd emptydrive
}

# The power button.  Windows is set to shut down when it is pressed
# (win/first.cmd), so the VM powering off shows that the press arrived.
power_button() # <what>
{
    [ "$(vmstate)" = running ] || return 1
    run controlvm "$VM" acpipowerbutton
    if wait_off 150; then
        stage "$1" ok
        BUTTON_WORKS=yes
        keep_logs
        return 0
    fi
    stage "$1" FAILED "the VM is in state '$(vmstate)' after 150 s"
    BUTTON_WORKS=no
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
    echo "    what Windows does when the power button is pressed, as installed: $(serial_value 'WIN: BUTTON-DEFAULT (.*)')"
    shot
    report_checks "" hotplug,absent,unused

    if run controlvm "$VM" plugpci "$FAKE_ADDR"; then
        stage "plugpci of a device which is not there is refused" FAILED "it went through"
    else
        stage "plugpci of a device which is not there is refused" ok
    fi
    guest_goes_on "the VM runs on after that" 120

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
    start_and_report "the installed Windows starts again" "$START_WAIT" || { end_phase; return 1; }
    report_checks "after the restart: " hotplug,absent,unused
    guest_goes_on "the guest keeps running" 120
    if saved_state_round; then
        guest_goes_on "the guest keeps running after the state was loaded" 120
        if [ "$BUTTON_WORKS" = yes ]; then
            power_button "guest: shuts down when the power button is pressed after the state was loaded"
        fi
    fi
    guest_shutdown
    show_exits "$SIM_OUT/VBox-$PHASE-saved.log"
    [ "$(vmstate)" = poweroff ]
}

# For comparison, should Windows not have shut down when the power button was
# pressed: the same Windows in a VM without the slot, as VMs are without this
# work.  The disk goes back to what it was afterwards, so that Windows does
# not carry what it made of that VM into what follows.
phase_control()
{
    PHASE=win-control
    settle
    if ! run snapshot "$VM" take before-control; then
        stage "for comparison: the installed Windows in a VM without the slot" skipped "no snapshot"
        return
    fi
    run modifyvm "$VM" --pci-detach "$FAKE_ADDR"
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug
    if start_and_report "for comparison: the installed Windows starts in a VM without the slot" "$START_WAIT"; then
        serial_show '^WIN: (ROOTBUS |ROOTBUS-PROP .*HotPlugControl|PORT |DEVS |PROBLEMS |BUGCHECKS |BUTTON)' 20
        guest_goes_on "for comparison: that guest keeps running" 120
        local works=$BUTTON_WORKS
        power_button "for comparison: that guest shuts down when the power button is pressed"
        BUTTON_WORKS=$works
    fi
    guest_shutdown
    settle
    run snapshot "$VM" restore before-control
    run snapshot "$VM" delete before-control
    run modifyvm "$VM" --pci-attach "$FAKE_ADDR@0000:01:00.0"
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1
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

# The drive in the hot-plug capable slot of a VM with Windows: there when
# Windows starts, then taken out and put in while it runs, with the guest
# being asked and without.
phase_hotplug()
{
    PHASE=win-hotplug
    bind_dev || { stage "the device is bound to vfio-pci" FAILED; return; }
    start_and_report "Windows starts with the drive in the hot-plug capable slot" "$START_WAIT" || return
    shot
    report_checks "" hotplug,present,inuse
    win_has_drive "guest: has the drive in the slot, with its driver" || { shot; return; }
    win_reads "guest: reads from the drive in the slot"
    local lay t0
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
    check_pcistate hotplug,absent,unused
    slot_registers_check
    guest_goes_on "the guest keeps running" 300

    if [ "$(pcistate)" = hotplug,absent,unused ]; then
        serial_mark
        if VBM_TIMEOUT=300 run controlvm "$VM" plugpci "$HOSTADDR"; then
            stage "plugpci puts the device into the slot" ok
        else
            stage "plugpci puts the device into the slot" FAILED
            diag
        fi
        win_has_drive "guest: takes the device in the slot, its driver has it" 600
        win_reads "guest: reads from the drive which was put in"
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
    guest_goes_on "the VM runs on after that" 300

    serial_mark
    if VBM_TIMEOUT=300 run controlvm "$VM" plugpci "$HOSTADDR"; then
        stage "plugpci after the forced removal" ok
    else
        stage "plugpci after the forced removal" FAILED
        diag
    fi
    win_has_drive "guest: takes the device again, its driver has it" 600
    win_reads "guest: reads from the drive again"
    check_log_quiet
    shot
}

mode_nested()
{
    : "${WIN_DISK_DEV:?}" "${SIM_HOSTADDR:?}"
    PHASE=win-setup
    START_WAIT=${WIN_START_WAIT:-3000}
    SHUTDOWN_WAIT=600
    echo "==== VirtualBox on $WHERE, Windows as the guest: $("$VBOX_BIN/VBoxManage" --version 2>&1 | tail -n 1)"
    ls -l /dev/kvm /dev/iommu "$WIN_DISK_DEV" "$WIN_CMD_ISO" 2>&1 | sed 's/^/    /'
    run setproperty machinefolder "$SIM_WORK/vms"
    # The disk of Windows is a disk of this machine, used as it is.
    if ! run createmedium disk --filename "$SIM_WORK/win-raw.vmdk" --format=VMDK --variant=RawDisk --property=RawDrive="$WIN_DISK_DEV"; then
        stage "the VM for Windows is created" FAILED "no disk"
        return
    fi
    create_vm 1 3072 "$SIM_WORK/win-raw.vmdk" emptydrive emptydrive "$HOSTADDR" "" || return
    stage "the VM for Windows is created, with the drive in the hot-plug capable slot" ok

    echo "==== phase win-hotplug on $WHERE"
    phase_hotplug
    case "$(vmstate)" in
        running) guest_shutdown ;;
        *)       end_phase ;;
    esac
    show_exits "$SIM_OUT/VBox-$PHASE.log"
}


case "$MODE" in
    runner) mode_runner ;;
    nested) mode_nested ;;
    *)      echo "unknown mode $MODE" ;;
esac

cp -f "$VBOX_USER_HOME/VBoxSVC.log" "$SIM_OUT/VBoxSVC-win.log" 2> /dev/null
pkill -x VBoxSVC 2> /dev/null
exit 0
