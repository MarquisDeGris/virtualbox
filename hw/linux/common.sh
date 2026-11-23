# Read in by the scripts of this directory, not run by itself.

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
# Where the scripts work and where their results go; a test of the scripts
# themselves puts them elsewhere.
work=${HW_WORK:-$top/hw-work}
out=${HW_OUT:-$top/hw-out}
mkdir -p "$work" "$out" || exit 1

have() { command -v "$1" > /dev/null 2>&1; }
first() { sed -n '1p' "$1" 2> /dev/null; }
# What comes in, shortened to <n> characters a line.
short() { sed -e 's/^\(.\{0,'"${1:-160}"'\}\).*/\1/'; }

# A line of the result: "<what>: ok", "<what>: FAILED (<why>)", ...  $RESULT
# is the file of the script.
stage() # <what> <ok|FAILED|skipped|look> [<note>]
{
    printf '%s: %s%s\n' "$1" "$2" "${3:+ ($3)}" | tee -a "$RESULT"
}

# The device the scripts are about.  HW_DEV names a function of it.  Without
# that it is the first graphics card which is not the one the firmware showed
# its screen on.  Sets DEV to its first function and FUNCS to all functions
# at its place on the bus.
pick_device()
{
    local d
    DEV=${HW_DEV:-}
    if [ -z "$DEV" ]; then
        for d in /sys/bus/pci/devices/*; do
            case "$(first "$d/class")" in
                0x03*) ;;
                *) continue ;;
            esac
            [ "$(first "$d/boot_vga")" = 1 ] && continue
            DEV=${d##*/}
            break
        done
    fi
    [ -n "$DEV" ] && [ -e "/sys/bus/pci/devices/$DEV" ] || return 1
    FUNCS=
    for d in /sys/bus/pci/devices/"${DEV%.*}".*; do
        FUNCS="$FUNCS${FUNCS:+ }${d##*/}"
    done
    DEV=${FUNCS%% *}
}

# The driver the host has a function with, "none" if it has none.
driver_of() # <address>
{
    if [ -e "/sys/bus/pci/devices/$1/driver" ]; then
        basename "$(readlink "/sys/bus/pci/devices/$1/driver")"
    else
        echo none
    fi
}

# "<vendor>:<device>" of a function, as the host has it.
ids_of() # <address>
{
    echo "$(first "/sys/bus/pci/devices/$1/vendor" | sed 's/^0x//'):$(first "/sys/bus/pci/devices/$1/device" | sed 's/^0x//')"
}

# The ROM of a function as the host reads it: "<bytes> <md5>", nothing if the
# function has none or the host does not get to read it.  A graphics card has
# its firmware there.  It takes root, and leaves the device as it was.
rom_of() # <address>
{
    local r="/sys/bus/pci/devices/$1/rom"
    [ -e "$r" ] || return 0
    echo 1 > "$r" 2> /dev/null || return 0
    if cat "$r" > "$work/rom.bin" 2> /dev/null && [ -s "$work/rom.bin" ]; then
        echo "$(wc -c < "$work/rom.bin") $(md5sum < "$work/rom.bin" | sed -e 's/ .*$//')"
    fi
    echo 0 > "$r" 2> /dev/null
    rm -f "$work/rom.bin"
}

# Nothing which names this machine or who uses it may be in a result.  The
# file is removed if it has; that is told and the script ends.
privacy_check() # <file>
{
    local word
    for word in "$(id -un 2> /dev/null)" "${SUDO_USER:-}" "$(hostname 2> /dev/null)"; do
        [ ${#word} -ge 3 ] || continue
        [ "$word" = root ] && continue
        if grep -q -i -w -F -- "$word" "$1"; then
            echo "The result has the name of this machine or of its user in it and was removed: $1" >&2
            rm -f "$1"
            exit 1
        fi
    done
}

# A script which ran as root leaves its files to whoever called sudo.
give_back()
{
    if [ "$(id -u)" = 0 ] && [ -n "${SUDO_UID:-}" ]; then
        chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$work" "$out" 2> /dev/null
    fi
}


# ---- For the tests, which run a VM called "hwtest" as root.

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

# A line of the result which counts: one which FAILED makes the script end
# with 1.
check() # <what> <ok|FAILED|skipped|look> [<note>]
{
    [ "$2" = FAILED ] && failed=$((failed + 1))
    stage "$@"
}

# Ends the service of the VirtualBox of these scripts, the one which was
# started from $BIN, and waits until it is gone: a test which starts while
# the service of the test before it is still ending talks to that one and
# does not get its VM cleared away (run 314 of the workflow).  A VirtualBox
# which is installed on the machine has a service of its own, which is left
# alone.
svc_stop()
{
    local i=0
    pkill -f "$BIN/VBoxSVC" 2> /dev/null
    while pgrep -f "$BIN/VBoxSVC" > /dev/null 2>&1 && [ "$i" -lt 30 ]; do
        sleep 1
        i=$((i + 1))
    done
    pkill -KILL -f "$BIN/VBoxSVC" 2> /dev/null
    pkill -f "$BIN/VBoxXPCOMIPCD" 2> /dev/null
    return 0
}

vmstate() { vbm showvminfo "$VM" --machinereadable 2> /dev/null | sed -n 's/^VMState="\(.*\)"$/\1/p'; }
# What VirtualBox tells of the device in the slot: "<hotplug|fixed>,<present|absent>,<inuse|unused>".
pcistate() { vbm showvminfo "$VM" --machinereadable 2> /dev/null | sed -n 's/^AttachedHostPCIState=[^,]*,\(.*\)$/\1/p' | sed -n '1p'; }

wait_pcistate() # <pattern as for case> <seconds>
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
# One kind of line reads like trouble and is none.  When a function is taken
# out of the VM, the device lets go of it at once and the VM is told a moment
# later; a register of the function which is read in between cannot be had
# from the device any more, is answered with zero, and the build this is
# written for says "Failed to passthrough PCI config read" of it (run 313 of
# the workflow has what was looked at).  Such lines are counted by themselves.
LOG_KNOWN='Failed to passthrough PCI config (read|write)'
log_trouble_lines() { log_lines | grep -i -E 'fail|could not|error|refus|not supported|timed out' | grep -v -E "$LOG_KNOWN"; }
log_trouble() { log_trouble_lines | grep -c .; }
log_known() { log_lines | grep -c -E "$LOG_KNOWN"; }

log_check() # <when>
{
    local n k
    n=$(log_trouble)
    k=$(log_known)
    if [ "${n:-0}" = 0 ]; then
        if [ "${k:-0}" = 0 ]; then
            check "the log has no line of the device about something which did not work, $1" ok
        else
            check "the log has no line of the device about something which did not work, $1" ok \
                  "$k line(s) of a register read while a function was on its way out, which is no trouble"
        fi
    else
        check "the log has no line of the device about something which did not work, $1" FAILED "$n line(s), see below"
    fi
}

# What every test begins with: root, the build, the device and that it is
# bound, and a place of its own for VirtualBox to keep its settings in.
test_begin() # <name of the script>
{
    local f unbound=
    if [ "$(id -u)" != 0 ]; then
        echo "Run it as root: sudo sh hw/linux/$1"
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
    for f in $FUNCS; do
        echo "function $f: class $(first "/sys/bus/pci/devices/$f/class"), id $(ids_of "$f"), driver $(driver_of "$f")" >> "$RESULT"
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
    # A VM of its own, made anew, with all it has under the work directory.
    # What a test before this one left is cleared away first: its VM, if it
    # still runs, the service, and what the service knew, so that this test
    # starts with a VirtualBox which knows of no VM and no disk.
    vbm controlvm "$VM" poweroff > /dev/null 2>&1
    pkill -KILL -f "$BIN/VBoxHeadless.*--comment $VM " 2> /dev/null
    svc_stop
    rm -rf "$work/vbox" "$work/vms"
    mkdir -p "$VBOX_USER_HOME"
    run setproperty machinefolder "$work/vms"
}

# Creates the VM with the device in the hot-plug capable slot.  What is given
# is passed on to modifyvm.  Ends the test if that does not work.
test_vm() # <options for modifyvm>...
{
    local f attached=yes
    if ! run createvm --name "$VM" --ostype Linux_64 --register; then
        check "the VM is created" FAILED "$(short 160 < "$work/last.out" | sed -n '1p')"
        test_end
    fi
    run modifyvm "$VM" --cpus "${HW_CPUS:-2}" --ioapic on --chipset ich9 --firmware efi \
        --vm-execution-engine native-api --nic1 none --audio-enabled off \
        --graphicscontroller vboxvga --vram 16 --uart1 0x3f8 4 --uartmode1 file "$work/serial.log" "$@" || attached=no
    for f in $FUNCS; do
        run modifyvm "$VM" --pci-attach "$f@0000:01:00.${f##*.}" || attached=no
    done
    run setextradata "$VM" VBoxInternal2/HostPCIHotPlug 1 || attached=no
    # Where the machine is itself a VM, every exit from the guest is slow, and
    # a guest which keeps time by interrupts gets nowhere.  HW_WARP is the
    # percentage of the pace of time in the VM then.  Not for real hardware.
    [ -n "${HW_WARP:-}" ] && run setextradata "$VM" VBoxInternal/TM/WarpDrivePercentage "$HW_WARP"
    if [ "$attached" = yes ]; then
        check "the VM is created, with the device in the hot-plug capable slot" ok
    else
        check "the VM is created, with the device in the hot-plug capable slot" FAILED "$(short 160 < "$work/last.out" | sed -n '1p')"
        test_end
    fi
    LOG="$(dirname "$(vbm showvminfo "$VM" --machinereadable | sed -n 's/^CfgFile="\(.*\)"$/\1/p')")/Logs/VBox.log"
}

# Starts the VM and ends the test if it does not run.
test_start()
{
    : > "$work/serial.log"
    if VBM_TIMEOUT=300 run startvm "$VM" --type headless && wait_vmstate running 30; then
        check "the VM starts with the device" ok
    else
        check "the VM starts with the device" FAILED "$(grep -a -i 'error' "$work/last.out" | sed -n '1p' | short 160)"
        test_end
    fi
}

# What every test ends with: the VM off, the lines of the device from the
# log, the count, the files given back.  The exit code is 1 if a check failed.
test_end()
{
    case "$(vmstate)" in
        running|paused|stuck|gurumeditation|starting)
            vbm controlvm "$VM" poweroff > /dev/null 2>&1
            wait_vmstate poweroff 60 || pkill -KILL -f "VBoxHeadless.*--comment $VM " 2> /dev/null
            ;;
    esac
    echo "---- what the log of the VM has from the device (the first 60 lines):" >> "$RESULT"
    log_lines | short 200 | sed -n '1,60p' >> "$RESULT"
    # The lines a check above counted as telling of something which did not
    # work, wherever in the log they are: without them the count says little.
    if [ "$(log_trouble)" != 0 ]; then
        echo "---- the lines of it which tell of something which did not work (at most 30):" >> "$RESULT"
        log_trouble_lines | short 240 | sed -n '1,30p' >> "$RESULT"
    fi
    echo "checks which failed: $failed" | tee -a "$RESULT"
    svc_stop
    privacy_check "$RESULT"
    give_back
    echo
    echo "Written to $RESULT. Publish it with: sh hw/publish.sh"
    [ "$failed" = 0 ]
    exit $?
}
