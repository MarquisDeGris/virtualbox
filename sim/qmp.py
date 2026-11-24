#!/usr/bin/env python3
# Part of the simulation, see host-agent.sh: gives one command to the human
# monitor of a QEMU through its QMP socket and prints what the monitor says.
#
# usage: qmp.py <socket> <command line for the human monitor>
import json
import socket
import sys


def main():
    if len(sys.argv) != 3:
        print("usage: qmp.py <socket> <command line>")
        return 2
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(20)
    sock.connect(sys.argv[1])
    stream = sock.makefile("rw")

    def answer():
        # What the monitor tells by itself in between is not an answer.
        while True:
            line = stream.readline()
            if not line:
                raise RuntimeError("the monitor closed the connection")
            message = json.loads(line)
            if "event" not in message:
                return message

    def command(name, arguments=None):
        request = {"execute": name}
        if arguments is not None:
            request["arguments"] = arguments
        stream.write(json.dumps(request) + "\n")
        stream.flush()
        return answer()

    answer()  # The greeting.
    reply = command("qmp_capabilities")
    if "error" in reply:
        print("error:", reply["error"].get("desc", reply["error"]))
        return 1
    reply = command("human-monitor-command", {"command-line": sys.argv[2]})
    if "error" in reply:
        print("error:", reply["error"].get("desc", reply["error"]))
        return 1
    print(str(reply.get("return", "")).strip())
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as failure:  # A socket which is not there, a time which ran out.
        print("error:", failure)
        sys.exit(1)
