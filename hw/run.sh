#!/bin/sh
# What is run on a machine with real hardware, see hw/README.md.  One command
# for whoever starts it, on Linux and in Git Bash on Windows:
#
#   sh hw/run.sh
#
# For now that is the inventory: what the machine has.  Tests follow when it
# is known what they can be run on.  Results go to hw-out/ and nowhere else;
# hw/publish.sh publishes them.
here=$(cd "$(dirname "$0")" && pwd)
case "$(uname -s 2> /dev/null)" in
    Linux)
        sh "$here/inventory.sh"
        ;;
    MINGW*|MSYS*|CYGWIN*)
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here/inventory.ps1"
        ;;
    *)
        echo "hw/run.sh knows Linux and Windows (Git Bash), not $(uname -s 2> /dev/null)"
        exit 1
        ;;
esac
