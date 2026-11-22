#!/bin/sh
# Tells what a Linux machine has of what the hardware test needs: processor,
# IOMMU, KVM, the graphics cards and what shares their IOMMU groups.
#
# It only reads, needs no root, changes nothing and installs nothing.  What it
# writes is meant to be published: no host name, no user name, no serial
# numbers, no network addresses, only the fields below.
#
#   sh hw/inventory.sh            writes hw-out/inventory.txt and shows it
#
# See hw/README.md.
VERSION=1
here=$(cd "$(dirname "$0")" && pwd)
out="$here/../hw-out"
mkdir -p "$out" || exit 1
f="$out/inventory.txt"

have() { command -v "$1" > /dev/null 2>&1; }
# One line "key: value", the value shortened to what a line can take.
kv() { printf '%s: %s\n' "$1" "$(printf '%s\n' "$2" | tr -s ' \t\n' ' ' | sed -e 's/^\(.\{0,160\}\).*/\1/' -e 's/ $//')"; }
first() { sed -n '1p' "$1" 2> /dev/null; }

{
    kv inventory "linux, version $VERSION"
    kv date "$(date -u '+%Y-%m-%d %H:%M UTC')"
    kv os "$(sed -n 's/^PRETTY_NAME="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' /etc/os-release 2> /dev/null)"
    kv kernel "$(uname -r)"
    kv arch "$(uname -m)"
    kv cpu "$(sed -n 's/^model name[^:]*: *//p' /proc/cpuinfo | sed -n '1p')"
    kv cpu-threads "$(grep -c '^processor' /proc/cpuinfo)"
    virt=none
    grep -q -w vmx /proc/cpuinfo && virt=vmx
    grep -q -w svm /proc/cpuinfo && virt=svm
    kv cpu-virtualization "$virt"
    if have systemd-detect-virt; then kv this-machine-is "$(systemd-detect-virt 2> /dev/null || true)"; fi
    kv memory-gib "$(awk '/^MemTotal:/ { printf "%.0f", $2 / 1048576 }' /proc/meminfo)"
    kv free-disk-gib "$(df -Pk "$here" 2> /dev/null | awk 'NR == 2 { printf "%.0f", $4 / 1048576 }')"

    # KVM and what the passthrough device talks to.
    if [ -c /dev/kvm ]; then
        if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then kv kvm "there, usable"; else kv kvm "there, not usable by this account"; fi
    else
        kv kvm "no /dev/kvm"
    fi
    kv iommu-groups "$(ls /sys/kernel/iommu_groups 2> /dev/null | wc -l)"
    kv iommu-on-kernel-command-line "$(tr ' ' '\n' < /proc/cmdline | grep -E '^(intel_iommu|amd_iommu|iommu)=' | tr '\n' ' ')"
    if [ -c /dev/iommu ]; then kv dev-iommu there; else kv dev-iommu "not there"; fi
    cfg="/boot/config-$(uname -r)"
    if [ -r "$cfg" ]; then
        kv kernel-config "$(grep -E '^CONFIG_(IOMMUFD|VFIO_DEVICE_CDEV|VFIO_PCI|VFIO|KVM|KVM_INTEL|KVM_AMD)=' "$cfg" | tr '\n' ' ')"
    else
        kv kernel-config "not readable"
    fi
    if have modinfo; then
        if modinfo vfio-pci > /dev/null 2>&1; then kv vfio-pci "module or built in"; else kv vfio-pci "not found"; fi
    fi
    if have mokutil; then kv secure-boot "$(mokutil --sb-state 2> /dev/null | sed -n '1p')"; fi

    # The graphics cards (PCI class 03) and sound devices (0403), by what
    # sysfs has: address, IDs, driver, IOMMU group, and whether the firmware
    # showed its screen on it.
    n=0
    for d in /sys/bus/pci/devices/*; do
        class=$(first "$d/class")
        case "$class" in
            0x03*|0x0403*) ;;
            *) continue ;;
        esac
        n=$((n + 1))
        addr=${d##*/}
        drv=none
        [ -e "$d/driver" ] && drv=$(basename "$(readlink "$d/driver")")
        grp=none
        [ -e "$d/iommu_group" ] && grp=$(basename "$(readlink "$d/iommu_group")")
        boot=
        [ "$(first "$d/boot_vga")" = 1 ] && boot=", the firmware's screen"
        name=
        if have lspci; then name=$(lspci -s "$addr" 2> /dev/null | sed -e 's/^[^ ]* //' -e 's/^\(.\{0,90\}\).*/\1/'); fi
        kv "pci-$addr" "class $class, id $(first "$d/vendor" | sed 's/^0x//'):$(first "$d/device" | sed 's/^0x//'), driver $drv, IOMMU group $grp$boot${name:+, $name}"
        if [ "$grp" != none ]; then
            others=
            for o in "$d"/iommu_group/devices/*; do
                oa=${o##*/}
                [ "$oa" = "$addr" ] && continue
                others="$others $oa($(first "$o/class"))"
            done
            kv "pci-$addr-shares-its-group-with" "${others:- nothing}"
        fi
    done
    kv graphics-and-sound-devices "$n"

    # Thunderbolt and USB4.
    if [ -d /sys/bus/thunderbolt/devices ]; then
        kv thunderbolt "$(ls /sys/bus/thunderbolt/devices 2> /dev/null | wc -l) entries, security $(cat /sys/bus/thunderbolt/devices/domain*/security 2> /dev/null | sort -u | tr '\n' ' ')"
    else
        kv thunderbolt "none"
    fi

    # What there is for building and for 3D.
    for t in gcc make git kmk qemu-system-x86_64 VBoxManage vulkaninfo glxinfo; do
        if have "$t"; then kv "tool-$t" "$("$t" --version 2> /dev/null | sed -n '1p')"; else kv "tool-$t" "not installed"; fi
    done
    if have vulkaninfo; then
        kv vulkan-devices "$(vulkaninfo --summary 2> /dev/null | sed -n 's/^[[:space:]]*deviceName[[:space:]]*= *//p' | tr '\n' ';')"
    fi
} > "$f" 2> /dev/null

# Nothing which names this machine or its user may be in there.
for word in "$(id -un 2> /dev/null)" "$(hostname 2> /dev/null)"; do
    [ ${#word} -ge 3 ] || continue
    if grep -q -i -w -F -- "$word" "$f"; then
        echo "The result has the name of this machine or of its user in it and was removed: $f" >&2
        rm -f "$f"
        exit 1
    fi
done

cat "$f"
echo
echo "Written to $f ($(wc -l < "$f") lines). Publish it with: sh hw/publish.sh"
