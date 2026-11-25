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

- `hw/run.sh` is the inventory: processor, virtualization, IOMMU,
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

## Under Linux: the steps to a first test

Each is one command, writes a text file to `hw-out/`, and can be published
with `sh hw/publish.sh` before the next. What takes root is run with `sudo`
by whoever is at the machine; the scripts are short enough to be read first.

    sh hw/run.sh                    # the inventory
    sh hw/linux/fetch.sh            # the VirtualBox the workflow built, into hw-work/bin
    sh hw/linux/prereq.sh           # which device it is going to be, and the checklist for it
    sh hw/linux/bind.sh             # shows what giving the device to vfio-pci takes
    sudo sh hw/linux/tests.sh       # does that, and runs the tests
    sh hw/publish.sh

`tests.sh` is all which takes root, one script after the other, each of
which can also be run by itself:

    sudo sh hw/linux/bind.sh --do   # the device goes to vfio-pci; --undo gives it back to the host
    sudo sh hw/linux/prereq.sh      # the checklist once more, as the tests will run
    sudo sh hw/linux/test-a.sh      # the first test: a VM without an operating system
    sudo sh hw/linux/test-b.sh      # the second: a small Linux guest with the driver of the device
    sudo HW_VGA=yes sh hw/linux/test-b.sh   # for a graphics card, see below
    sudo sh hw/linux/test-c.sh      # the third: the device leaves that guest and comes back

- The device is the first graphics card which is not the one the firmware
  shows its screen on, with all its functions. `HW_DEV=<address>` in front
  of a command names another.
- `fetch.sh` installs nothing. It tells which packages of the distribution
  the build needs and are missing, as an `apt-get install` line to run.
  The build it fetches has 3D acceleration for VMs, which takes a graphics
  card with a Vulkan driver on the host for a VM that has it switched on;
  the tests here do not switch it on. It is the build of the commit
  `cc3dc981`, the head of its branch on 2026-11-25. There is a build
  without 3D acceleration as well:
  `HW_BUILD=60-ubuntu sh hw/linux/fetch.sh`, see `hw/builds.txt`. That one
  is older and lacks three changes to the device. One is of 2026-11-24:
  without it a log can have lines about a register read which failed while
  a device was on its way out, which the tests count by themselves and not
  as trouble. The other is of 2026-11-25 and about a device which is in
  the VM for good, not in the slot the tests here use: when the host takes
  such a device back, what is left to do for it now waits for a write of
  the guest to its configuration space which is under way. The third is of
  that day too: a snapshot of a running VM is refused while the device is in
  the slot, and with the older build a VM with one CPU can stand still at
  that moment where it is only to say no.
- `bind.sh` refuses the card the firmware shows its screen on. A card the
  desktop of the host uses does not take being taken from its driver well;
  no monitor on the card and nothing of the host using it is the safe way.
  The binding lasts until the machine is started anew.
- `test-a.sh` needs no operating system in the VM: a VM with the device in
  the hot-plug capable slot starts, the device is taken out and put in
  twice, the VM is reset and powered off. The result has what VirtualBox
  tells of the device at each step and the lines the log of the VM has from
  the device. It makes a VM called `hwtest` under `hw-work/` and touches
  nothing else.

- `test-b.sh` puts a small Linux guest together from the kernel the machine
  runs, the modules that kernel has for the device and their firmware, and
  starts it in the VM. The guest tells on the serial port of the VM which
  driver has the device, where its memory is and whether it reaches to the
  end of it, whether it reads the ROM of the device as the host does (a
  graphics card has its firmware there), its interrupts, and for a graphics
  card its connectors and whether a monitor is on one; the script turns that
  into checks. Nobody has to look at a screen for it. It needs a few small
  programs (busybox as a static program, cpio, zstd, mkfs.vfat, mtools,
  sgdisk) and names the packages if one is missing.
- `sudo HW_VGA=yes sh hw/linux/test-b.sh` is the second test with a VM which
  has no graphics adapter of its own and is given the legacy VGA ranges of
  the card, which is what it takes for a card to show the guest from its
  start. The guest reads a register of the card through them. The result is
  `hw-out/test-b-vga.txt`.
- `test-c.sh` has the guest of the second test, looks at what that test looks
  at, and then does what this work is about, twice: the guest is asked for
  the device and lets go of it, the device is out of the VM and stays with
  `vfio-pci`, it is put in again, and the drivers of the guest take it
  again. The guest tells on the serial port which driver has which function
  whenever that changes, and for a graphics card how many connectors it has
  and on how many of them something is; the script turns that into checks,
  and the result has what the kernel of the guest said meanwhile. The device
  stays where it is on the host all the while, so this is not a device on
  Thunderbolt being pulled. `tests.sh` runs it last; `HW_SKIP_PLUG=yes` in
  front of that command leaves it out.

The workflow runs the three tests in its simulation, with an emulated NVMe
drive as the device, with two of them at one address, with a device which
has four gigabytes of memory and a ROM, and with the graphics adapter an
emulator has, whose driver lets go of the card and takes it again in the
third test. The other scripts run there as well: the build is fetched as it
is published and the first test passes with it, and a result is published
to a repository made for the run. On hardware nothing here has run yet.

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
