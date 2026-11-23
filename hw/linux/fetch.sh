#!/bin/sh
# Fetches the VirtualBox the workflow of the fork has built for machines with
# real hardware, the newest one hw/builds.txt lists, and unpacks it into
# hw-work/bin.  Nothing is installed and nothing is built.  It tells which
# packages of the distribution the build needs and are not there; installing
# them is for whoever is at the machine.
#
#   sh hw/linux/fetch.sh          writes hw-out/fetch.txt
#   HW_BUILD=-3d- sh hw/linux/fetch.sh
#                                 the newest build whose name has "-3d-" in
#                                 it, which is one with 3D acceleration for VMs
. "$(dirname "$0")/common.sh"
RESULT="$out/fetch.txt"
: > "$RESULT"

line=$(grep -v -e '^#' -e '^$' "$top/hw/builds.txt" 2> /dev/null | grep -F -e "${HW_BUILD:-}" | tail -n 1)
if [ -z "$line" ]; then
    stage "a build to fetch is listed" FAILED "hw/builds.txt has none${HW_BUILD:+ with $HW_BUILD in its name}, bring the clone up to date"
    exit 1
fi
set -- $line
file=$1
sum=$2
url="https://github.com/MarquisDeGris/virtualbox/releases/download/hw-builds/$file"

if [ ! -s "$work/$file" ]; then
    echo "fetching $url"
    if ! curl -f -L -sS --retry 3 --connect-timeout 30 -o "$work/$file.part" "$url" || ! mv "$work/$file.part" "$work/$file"; then
        stage "the build is fetched" FAILED "$file"
        exit 1
    fi
fi
if ! echo "$sum  $work/$file" | sha256sum -c - > /dev/null 2>&1; then
    stage "the build is the one listed" FAILED "the checksum of $file differs; remove hw-work/$file and fetch again"
    exit 1
fi
stage "the build is fetched and is the one listed" ok "$file"

rm -rf "$work/bin"
if ! tar --zstd -xf "$work/$file" -C "$work" || [ ! -x "$work/bin/VBoxManage" ]; then
    stage "the build is unpacked" FAILED "tar with zstd is needed"
    exit 1
fi
stage "the build is unpacked" ok

# What the build says of itself, without the list of packages.
sed -n '1,7p' "$work/bin/BUILD-INFO.txt" 2> /dev/null | sed -e 's/^/build: /' | short 200 >> "$RESULT"

pkgs=$(sed -n '/^packages of Ubuntu/{n;p;}' "$work/bin/BUILD-INFO.txt" 2> /dev/null)
missing=
if have dpkg; then
    for p in $pkgs; do
        dpkg -s "$p" > /dev/null 2>&1 || missing="$missing $p"
    done
    if [ -z "$missing" ]; then
        stage "the packages the build needs are installed" ok
    else
        stage "the packages the build needs are installed" FAILED "missing:$missing"
        echo
        echo "Install them as root, then run this again:"
        echo "    apt-get install$missing"
        echo
    fi
else
    stage "the packages the build needs are installed" skipped "this is no system with dpkg, the list is in hw-work/bin/BUILD-INFO.txt"
fi

v=$("$work/bin/VBoxManage" --version 2>&1 | tail -n 1)
case "$v" in
    [0-9]*) stage "VBoxManage of the build runs" ok "$v" ;;
    *)      stage "VBoxManage of the build runs" FAILED "$(echo "$v" | short 120)" ;;
esac

privacy_check "$RESULT"
give_back
echo
echo "Written to $RESULT."
