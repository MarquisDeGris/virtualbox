#!/bin/sh
# The third test with a real device: the device leaves the guest and comes
# back while the guest runs and a driver of the guest has it, which is what
# pulling a device and plugging it in again does to a guest.  The guest is
# the small Linux guest of hw/linux/test-b.sh, and what that test looks at is
# looked at here once more first.  Then, twice: the guest is asked for the
# device and lets go of it, the device is out of the slot and stays with
# vfio-pci on the host; it is put into the slot again, and the guest has it
# again with the drivers which had it, a graphics card with its connectors
# and what was on them.
#
#   sudo sh hw/linux/test-c.sh        writes hw-out/test-c.txt
#
# It runs as root and takes what hw/linux/test-b.sh takes.  HW_PLUG_WAIT is
# how many seconds the drivers of the guest are given to take the device
# which came back, 180 if nothing is said.
#
# The device stays where it is on the host all the while: a device on
# Thunderbolt which is pulled for real is not what this is.
HW_PLUG=yes
export HW_PLUG
exec sh "$(dirname "$0")/test-b.sh" "$@"
