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
#   sudo HW_VGA=yes sh hw/linux/test-c.sh     writes hw-out/test-c-vga.txt
#
# is the same with the VM of HW_VGA=yes of hw/linux/test-b.sh, which has no
# graphics adapter of its own and was given the legacy VGA ranges of the
# card: the ranges stay those of the VM while the card is out.  The guest
# reads through them that nothing answers then, and reads the register of the
# card again when the card is back.
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
