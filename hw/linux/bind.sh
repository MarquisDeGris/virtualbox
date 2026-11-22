#!/bin/sh
# Gives the device to vfio-pci, which is what a VM gets it from: each of its
# functions is taken from the driver the host has it with and bound to
# vfio-pci.  That takes root.
#
#   sh hw/linux/bind.sh            prints what it would do, and does nothing
#   sudo sh hw/linux/bind.sh --do  does it
#   sudo sh hw/linux/bind.sh --undo
#                                  gives the functions back to the drivers of
#                                  the host
#
# The device is the one hw/linux/prereq.sh names; HW_DEV=<address> names
# another.  It lasts until the machine is started anew.
#
# A graphics card the desktop of the host uses does not take this well: the
# picture can stand still for good.  It is refused for the card the firmware
# shows its screen on.  For the other card: no monitor on it and nothing of
# the host using it is the safe way; if its driver is loaded, see the
# description at the top of src/VBox/Devices/Bus/DevPciVfio.cpp about keeping
# the driver away from the start.
. "$(dirname "$0")/common.sh"

if ! pick_device; then
    echo "There is no device to bind: no graphics card but the one the firmware shows its screen on. Name a function with HW_DEV=<address>."
    exit 1
fi
for f in $FUNCS; do
    if [ "$(first "/sys/bus/pci/devices/$f/boot_vga")" = 1 ]; then
        echo "$f is the card the firmware shows its screen on. This script does not take that one from the host."
        exit 1
    fi
done

case "${1:-}" in
    --do|--undo) ;;
    '')
        echo "The device: $FUNCS"
        for f in $FUNCS; do
            echo "  $f has the driver: $(driver_of "$f")"
        done
        echo
        echo "With --do, as root, this script would run:"
        echo "    modprobe vfio-pci"
        for f in $FUNCS; do
            [ "$(driver_of "$f")" != none ] && [ "$(driver_of "$f")" != vfio-pci ] \
                && echo "    echo $f > /sys/bus/pci/devices/$f/driver/unbind"
            echo "    echo vfio-pci > /sys/bus/pci/devices/$f/driver_override"
            echo "    echo $f > /sys/bus/pci/drivers_probe"
        done
        echo
        echo "Nothing was done. To do it: sudo sh hw/linux/bind.sh --do"
        exit 0
        ;;
    *)
        echo "usage: bind.sh [--do|--undo]"
        exit 1
        ;;
esac

if [ "$(id -u)" != 0 ]; then
    echo "That takes root: sudo sh hw/linux/bind.sh $1"
    exit 1
fi

if [ "$1" = --undo ]; then
    for f in $FUNCS; do
        [ "$(driver_of "$f")" = vfio-pci ] && echo "$f" > "/sys/bus/pci/devices/$f/driver/unbind"
        echo > "/sys/bus/pci/devices/$f/driver_override"
        echo "$f" > /sys/bus/pci/drivers_probe
        echo "$f has the driver: $(driver_of "$f")"
    done
    exit 0
fi

modprobe vfio-pci 2> /dev/null
bound=yes
for f in $FUNCS; do
    cur=$(driver_of "$f")
    if [ "$cur" != none ] && [ "$cur" != vfio-pci ]; then
        echo "taking $f from $cur"
        echo "$f" > "/sys/bus/pci/devices/$f/driver/unbind"
    fi
    echo vfio-pci > "/sys/bus/pci/devices/$f/driver_override"
    echo "$f" > /sys/bus/pci/drivers_probe
    echo "$f has the driver: $(driver_of "$f")"
    [ "$(driver_of "$f")" = vfio-pci ] || bound=no
done
if [ "$bound" = yes ]; then
    echo "The device is bound to vfio-pci. Next: sudo sh hw/linux/prereq.sh, then sudo sh hw/linux/test-a.sh"
else
    echo "Not every function is bound to vfio-pci, see above."
    exit 1
fi
