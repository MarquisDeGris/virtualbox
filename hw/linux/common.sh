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
stage() # <what> <ok|FAILED|skipped> [<note>]
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
