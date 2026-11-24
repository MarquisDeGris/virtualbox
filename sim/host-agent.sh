#!/bin/bash
# Part of the simulation, see run.sh: runs on the runner next to the virtual
# machine which stands in for a host, and does to that machine what it cannot
# do to itself.  The script in the machine leaves a request in the directory
# it shares with the runner (host-request); this one acts on it through the
# monitor of the machine and leaves the answer there (host-reply), which
# begins with "OK" if it was done.
#
# The requests, one line each:
#   aer <status>   the root port which the device is behind (rp1) reports an
#                  uncorrectable error with that status, one bit as a
#                  hexadecimal number.  The port tells the machine as hardware
#                  does: by its registers and its interrupt.
#   unplug         the device behind that port (dev0) is unplugged: the port
#                  tells the machine that somebody wants it out, and the device
#                  is gone once the machine has switched its slot off.
#   plug           the same device is plugged into the port again; where it has
#                  two functions, the second one first, as the machine is told
#                  of a device when its first function is there.
# Nothing else is taken, and no request has a word of the machine in what is
# given to the monitor but the status of an error, which is checked.  The
# machine has no monitor of its own.
#
# usage: host-agent.sh <directory shared with the machine> <QMP socket> <seconds to live at most> [<functions of the device: 1 or 2>]
# Ends when the file host-agent.stop appears in that directory.

dir=$1
qmp=$2
limit=$3
funcs=${4:-1}
here=$(cd "$(dirname "$0")" && pwd)

# monitor <what has to be at the start of the answer, or "" for an answer without an error> <command line>
monitor()
{
    local ok=no
    if python3 "$here/qmp.py" "$qmp" "$2" > "$dir/host-reply.tmp" 2>&1; then
        if [ -n "$1" ]; then
            grep -q "^$1" "$dir/host-reply.tmp" && ok=yes
        elif ! grep -q -i -E 'error|not found|failed' "$dir/host-reply.tmp"; then
            ok=yes
        fi
    fi
    if [ "$ok" = yes ]; then
        if [ -z "$1" ]; then
            # An answer which says nothing is one which went well.
            if [ -s "$dir/host-reply.tmp" ]; then
                sed -i -e '1s/^/OK /' "$dir/host-reply.tmp"
            else
                echo "OK" > "$dir/host-reply.tmp"
            fi
        fi
    else
        # What the monitor said, behind a word which is not "OK".
        [ -s "$dir/host-reply.tmp" ] || echo "the monitor said nothing" > "$dir/host-reply.tmp"
        sed -i -e '1s/^/failed: /' "$dir/host-reply.tmp"
    fi
}

t0=$(date +%s)
while [ ! -e "$dir/host-agent.stop" ] && [ $(( $(date +%s) - t0 )) -lt "$limit" ]; do
    if [ -s "$dir/host-request" ]; then
        what= arg= rest=
        read -r what arg rest < "$dir/host-request"
        rm -f "$dir/host-request"
        if [ "$what" = aer ] && [[ "$arg" =~ ^0x[0-9a-f]{1,8}$ ]] && [ -z "$rest" ]; then
            monitor OK "pcie_aer_inject_error rp1 $arg"
        elif [ "$what" = unplug ] && [ -z "$arg" ]; then
            monitor "" "device_del dev0"
        elif [ "$what" = plug ] && [ -z "$arg" ]; then
            if [ "$funcs" = 2 ]; then
                monitor "" "device_add nvme,drive=nvm1,serial=SIM0002,bus=rp1,addr=00.1,id=dev1"
                if grep -q '^OK' "$dir/host-reply.tmp"; then
                    monitor "" "device_add nvme,drive=nvm0,serial=SIM0001,bus=rp1,addr=00.0,multifunction=on,id=dev0"
                fi
            else
                monitor "" "device_add nvme,drive=nvm0,serial=SIM0001,bus=rp1,id=dev0"
            fi
        else
            echo "refused: not a request this takes" > "$dir/host-reply.tmp"
        fi
        mv "$dir/host-reply.tmp" "$dir/host-reply"
        echo "host agent: $what $arg -> $(head -n 1 "$dir/host-reply")"
    fi
    sleep 1
done
rm -f "$dir/host-agent.stop"
exit 0
