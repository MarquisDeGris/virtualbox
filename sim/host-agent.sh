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
# Nothing else is taken.  The machine has no monitor of its own.
#
# usage: host-agent.sh <directory shared with the machine> <QMP socket> <seconds to live at most>
# Ends when the file host-agent.stop appears in that directory.

dir=$1
qmp=$2
limit=$3
here=$(cd "$(dirname "$0")" && pwd)
t0=$(date +%s)
while [ ! -e "$dir/host-agent.stop" ] && [ $(( $(date +%s) - t0 )) -lt "$limit" ]; do
    if [ -s "$dir/host-request" ]; then
        what= arg= rest=
        read -r what arg rest < "$dir/host-request"
        rm -f "$dir/host-request"
        if [ "$what" = aer ] && [[ "$arg" =~ ^0x[0-9a-f]{1,8}$ ]] && [ -z "$rest" ]; then
            if python3 "$here/qmp.py" "$qmp" "pcie_aer_inject_error rp1 $arg" > "$dir/host-reply.tmp" 2>&1 \
               && grep -q '^OK' "$dir/host-reply.tmp"; then
                :
            else
                # What the monitor said, behind a word which is not "OK".
                sed -i -e '1s/^/failed: /' "$dir/host-reply.tmp"
                [ -s "$dir/host-reply.tmp" ] || echo "failed: the monitor said nothing" > "$dir/host-reply.tmp"
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
