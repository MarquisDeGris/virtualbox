# Runs on a machine with real hardware

The workflow of this fork builds and tests on runners which have no graphics
card, no Thunderbolt and no device to pass through. This directory is for a
machine which has: scripts which are started there, write down what they
find, and publish it on the branch `hw-results`, where it is read later and
elsewhere. Nobody has to wait for anybody.

Whoever starts the scripts needs to think about nothing: what to run is in
the scripts, what it means is worked out where the results are read.

## On the machine

Linux, or Windows with Git for Windows (the commands in Git Bash):

    git clone --depth 1 https://github.com/MarquisDeGris/virtualbox.git vbox-meta
    cd vbox-meta
    sh hw/run.sh
    sh hw/publish.sh

Later runs: `git pull` first, as the scripts change.

- `hw/run.sh` is the inventory for now: processor, virtualization, IOMMU,
  the graphics cards and what shares their IOMMU groups, what there is for
  building. It only reads, needs no root and no administrator, changes
  nothing and installs nothing. It writes `hw-out/inventory.txt` and shows
  it.
- `hw/publish.sh` copies the text files of `hw-out/` into a directory of
  their own on the branch `hw-results` and pushes that. It needs the right
  to push to the fork. Without it, the text of `hw-out/inventory.txt` can be
  copied by hand to where it is read; it is some forty lines.

A machine other than the first gets a name of its own for its results:
`HW_TAG=pc2 sh hw/publish.sh`.

## Where the machine runs Windows

The inventory runs there as it is. Little else does: the passthrough device
is for Linux hosts, and a VirtualBox built from this fork for a Windows host
is not to be had here. So the plan for such a machine, of which only the
inventory exists so far:

1. The inventory under Windows, which also tells whether the drive of
   Windows is encrypted with BitLocker. If it is, the recovery key should be
   at hand before the settings of the firmware are changed.
2. Linux on an external USB drive, started from the boot menu of the
   firmware. Windows and the drives inside stay as they are. In the
   firmware: hardware virtualization and the IOMMU (VT-d, AMD-Vi) switched
   on. Secure Boot can stay on, a VirtualBox built with the KVM backend
   loads no kernel modules of its own.
3. The inventory under that Linux: whether the IOMMU is on, and whether the
   graphics card which is to go to a VM is in an IOMMU group of its own.
4. A VirtualBox built by the workflow of this fork, fetched; nothing is
   built on the machine.
5. The card bound to `vfio-pci`, the one step which takes root: the script
   prints the commands, the person at the machine runs them.
6. The tests, a command each: the card in a small Linux VM, out and in
   again, taken back by the host; then a guest with a desktop and the
   monitor on the card; later Windows as the guest.

A card inside the machine is not a device on Thunderbolt: what happens on
the host when a device is plugged in and pulled is not tested this way.

## What leaves the machine

Only the text files in `hw-out/`, and those have what the scripts put into
them field by field: names of processors and graphics cards, PCI addresses
and IDs, versions, yes and no. No host name, no user name, no serial number,
no network address, no path of a home directory. Both scripts look for the
name of the machine and of its user in the result and refuse it if they find
one. The commit on `hw-results` is made as `popolupo`, whatever git is set
up with on the machine.

The fork is public, and so is what is published there.

## With Claude Code, where somebody wants it to do the typing

The two commands need no assistant. Where one is to run them all the same,
this keeps it to exactly that: one question, a small model, the two commands
and nothing else allowed, no files of the project read into its context.

    claude -p "$(cat hw/PROMPT.txt)" --model haiku --safe-mode --tools "Bash" \
        --allowedTools "Bash(sh hw/run.sh *)" "Bash(sh hw/publish.sh *)" \
        --disallowedTools "mcp__*" --permission-mode dontAsk --max-turns 4 \
        --no-session-persistence

The options are as the documentation of Claude Code had them on 2026-11-22;
`claude --help` tells what a given version knows. Nothing here was tried
with them.

What the assistant may not do there is not left to its judgement: the list
of allowed commands has the two scripts, and the scripts themselves ask for
nothing a user without special rights cannot do. Steps which need root, when
tests come which have such, are printed for the person at the machine and
not run.
