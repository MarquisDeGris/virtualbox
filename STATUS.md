# Status

Updated: 2026-11-26

## Where things are

| Item | Value |
|------|-------|
| Upstream base | `main` @ `b6bea721` |
| Branch of the merge request | `pci-passthrough-hotplug-v1` @ `2409b39e`, 46 commits. The last two are of 2026-11-26 and in Main, which is code of upstream: the VM process ends and VirtualBox knows that the VM is off when a frontend is told to end with a device of the host in the VM, see "Who waits for whom, in Main and in the root port". The five before them are of 2026-11-25 and came of a second reading of four commits and of the reading of what came of that, see "A second reader for four later commits": the PCI lock where a function goes away for the guest and the lock of the slot where a live state is refused, each with a test, and what the log and some comments say. The one before them is in the testcase only, which waits for the slot to be told that it is empty, see "A check of the testcase which came too early". The one before it has the device not ask a function which is out of service, in since 2026-11-24, see "VMs with the sanitizers". The one before it is INTx, in since 2026-11-22, see "INTx (WP8)". The two before that are for hosts with KVM and not about the device (a timer notification which was missed, level-triggered interrupts after a saved state was loaded), in since 2026-11-19, see runs 227 to 233 |
| Branch the runs were made on | `scratch/timer-wakeup` @ `f563e7f8`, the commit which is the head of the merge request; before the two commits `scratch/quiet-reset` @ `dd43ddf3` |
| Branches for the maintenance lines | `pci-passthrough-hotplug-7.2` and `-7.1`: the work on those lines, built and tested, without an execution engine for it. `pci-passthrough-hotplug-7.2-kvm` and `-7.1-kvm`: the same with the KVM backend of Cyberus Technology, on which the simulation runs the device. See "The maintenance branches" |
| Old code branch | `pci-passthrough-hotplug` @ `c5cc9847`, no longer moved |
| Notes and CI branch | `meta` (this branch, default branch of the fork) |
| Merge request | [#1](https://github.com/MarquisDeGris/virtualbox/pull/1), inside this fork, marked ready for the hardware test: WP0 to WP7 |
| Merge requests for the released lines | [#2](https://github.com/MarquisDeGris/virtualbox/pull/2): `pci-passthrough-hotplug-7.2-kvm` against `VBox-7.2`. [#3](https://github.com/MarquisDeGris/virtualbox/pull/3): `backports-7.1` against `VBox-7.1`, nothing of the passthrough work, what the 7.2 line received and 7.1 did not. [#4](https://github.com/MarquisDeGris/virtualbox/pull/4): `pci-passthrough-hotplug-7.1-kvm` against `VBox-7.1` |
| Merge requests for requests of the upstream tracker | [#5](https://github.com/MarquisDeGris/virtualbox/pull/5): `feat/modifyvm-graphics-order` against `main`. [#6](https://github.com/MarquisDeGris/virtualbox/pull/6): `feat/doc-3d-object-memory` against `main`. [#8](https://github.com/MarquisDeGris/virtualbox/pull/8): `feat/nvram-firmware-setup` against `main`. [#9](https://github.com/MarquisDeGris/virtualbox/pull/9): `fix/doc-modifymedium-compact` against `main`, for a report and not a request. Nothing of the passthrough work in any of them. See "Requests from the tracker of upstream" |
| Issues in the tracker of upstream | [999](https://github.com/VirtualBox/virtualbox/issues/999): the work, since 2026-11-23. [1008](https://github.com/VirtualBox/virtualbox/issues/1008): two things read in the interrupt code of `main`, not tried. [1009](https://github.com/VirtualBox/virtualbox/issues/1009): two defects of Main, shown with `main` as it is, with the two commits which put them right. Both since 2026-11-26 |
| Throwaway branches | `scratch/*` (tests against the unmodified device, used as evidence) |

## Current step

WP2 (hardening), WP3 (removal of the device on the host), WP4 (saved state:
refused while the device is in the VM, possible while the hot-plug slot is
empty), WP5 (hot-plug slot in the root port, ACPI), WP6 (the passthrough
device in the slot) and WP7 (the management in Main and `VBoxManage`) are
done: the guest is asked before a device is taken out of the slot, a device
can be put into the empty slot while the VM runs, the VM can start with an
empty slot, its state can be saved and loaded while the slot is empty, and
`VBoxManage controlvm` puts a device in and takes it out. The testcase has
56 tests and three random tests and runs clean with AddressSanitizer,
LeakSanitizer and UndefinedBehaviorSanitizer (runs 232 and 233, and
runs 262 and 263 on the branches for the released lines). Main and
`VBoxManage` are built, and since 2026-11-17 a simulation runs all of it
with a Linux guest and an emulated device behind an emulated IOMMU: 80
checks, all pass (run 232; run 224, which fails if one does, for the head
before the last two commits), see the next section. Windows Server has
been through the hot-plug capable slot while it is empty: 27 checks, all
pass (runs 230 to 232). The hardware test, which needs a person with the
hardware, has not been done; the merge request is marked ready for it,
see "Ready for the hardware test" below.

Since 2026-11-19 the work goes on with what comes after the hardware
test without waiting for it: there is time for development now and
there will not be later. Nothing is written down as tested on hardware
because of that. First of it: the same work on the maintenance lines 7.2
and 7.1, see "The maintenance branches". On those lines the simulation
runs with the KVM backend of Cyberus Technology, and every check of it
passes on both (runs 253 and 254, 150 checks each, and on 7.1 run 258
with 155 and run 259 with 172, on 7.2 run 261 with 172): the Linux guest
with the device, and Windows, which takes a drive of the host in the
hot-plug capable slot, lets go of it and takes it again. In one earlier
run Windows stopped after letting go of the drive, which is not
understood yet (run 251). The branches for 7.1 also have what the 7.2
line received after upstream ended its support for 7.1, see "The 7.1
line and what the 7.2 line received after it"; that is on a branch of
its own as well, `backports-7.1`.

### Installation and the simulation, from 16 November

A check of what was delivered found two gaps. Nothing said how to build
and install the branch and how to prepare the host: that is
[INSTALL.md](INSTALL.md) now. And the README still called the work a draft
in an early state.

The system simulation (T4), which had been dropped on 2026-11-15, is being
set up after all: it is the one thing without hardware which shows the
code at work instead of compiled, with the real VMM, a real kernel
interface, a guest operating system, Main and `VBoxManage`. The flavor
`sim` of the workflow runs the VirtualBox it has built with a small Linux
guest (`sim/` on this branch):

1. On the runner itself, which has KVM and no IOMMU: a VM without anything
   of this work, and a VM with an empty hot-plug capable slot, including
   its saved state.
2. In a virtual machine standing in for a host, with an emulated IOMMU and
   an emulated NVMe drive behind a PCIe root port: the drive is bound to
   `vfio-pci` there and VirtualBox passes it on to its guest, which reads
   from it and writes to it. Then what the hardware test asks for as far
   as it goes without hardware: pause and resume, the refused saved state,
   `unplugpci` and `plugpci` with the guest being asked and without, the
   host asking for the device back, a start without the device, a saved
   state with the empty slot.

The first runs are for setting this up and do not fail on what they find
(`sim_strict: no`).

**Run 220, the first one** (2026-11-16, head of the merge request):

The first part did everything it was to do. This is the first time the
code ran in a VM, and the first time Main and `VBoxManage` ran at all:

| What | Result |
|------|--------|
| VirtualBox from the build directory, no kernel module of VirtualBox loaded | works; `VBoxManage`, `VBoxSVC`, `VBoxHeadless` |
| A VM without anything of this work (ICH9, EFI, KVM) | boots the guest in 11 seconds; the root bridge is `PNP0A03` and the guest is not given PCIe hot-plug, as before |
| A VM configured for a device which is not there, with hot-plug | starts with the layout from the extra data; the guest finds `PNP0A08`, `_OSC: OS now controls [PCIeHotplug SHPCHotplug PME AER PCIeCapability]`, and its driver takes the slot: `pciehp: Slot #1 AttnBtn+ PwrCtrl+ MRL- AttnInd+ PwrInd+ HotPlug+ Surprise+ Interlock- NoCompl- IbPresDis- LLActRep+`; the slot is empty; no complaint of the guest kernel about the port |
| `showvminfo` for that VM | `hotplug,absent,unused` |
| `controlvm plugpci` of a device which is not there | refused with a message, the VM runs on |
| `controlvm savestate` with the empty slot, then starting the VM again | the state is saved and loaded, the guest goes on where it was |

The second part got as far as the guest kernel starting:

| What | Result |
|------|--------|
| The virtual machine standing in for the host | boots, has KVM (nested), `/dev/iommu`, the NVMe drive behind the emulated IOMMU in a group of its own, bound to `vfio-pci` |
| A VM with the drive passed through | starts: the device opens the drive through VFIO and IOMMUFD of a real kernel, the host kernel resets the drive, regions, interrupts and capabilities are read as they are (MSI-X with 65 vectors is emulated, the PCI Express capability too, power management is left out); the firmware runs and loads the kernel |
| The layout Main stores for a start without the device | stored, and it is what the drive has |
| A VM started without the drive (unbound from `vfio-pci`) | starts |
| The guest in these VMs | **does not get through its start**: every exit from this guest takes about a millisecond, 725000 of them in eleven minutes. The kernel switches on its timer tick of 1000 interrupts a second, each of which takes several exits while the interrupts still go through the PIC (115000 interrupts were delivered that way, 558000 were raised), and from there on it does nothing but serve them |

That is a property of where this ran, not of the device: the runner is a
virtual machine itself, so the guest of VirtualBox was at the third level
of hardware virtualization at least, and the cost of an exit multiplies
with each level. The same VM on the runner needs 200000 exits for a
complete start, which is nothing there.

**Run 221** (2026-11-17) tried two ways around that:

- The machine standing in for the host emulated in software (QEMU without
  KVM, with an AMD processor whose virtualization extension QEMU emulates
  as well, time counted in instructions). That machine works, KVM in it
  loads, and VirtualBox starts its VM, but the VM ends at once with a
  triple fault of the guest, with or without a device. Not followed up:
  it is VirtualBox on an emulated processor nobody runs it on.
- KVM as before, and the timers VirtualBox emulates slowed down to a
  twentieth of their speed in that VM (`VBoxInternal/TM/WarpDrivePercentage`
  = 5), the kernel of the guest keeping quiet on the serial port. **That
  works**: the tick which the kernel could not get past comes twenty
  times less often, and once the kernel is up it takes its time from KVM.

With the second way the simulation got through the first two phases, in
four and a half minutes:

| What | Result |
|------|--------|
| A VM without a device, in the virtual machine standing in for the host | boots the guest, the guest keeps running |
| A VM with the NVMe drive passed through (no hot-plug) | boots the guest; the guest finds the drive at `01:00.0` with its real IDs and its driver binds |
| The guest reads the first 8 MiB of the drive | the checksum is that of the image file on the outside: memory access of the device through the IOMMU, to guest memory which VirtualBox mapped through IOMMUFD |
| The guest writes to the drive | what it wrote is in the image file on the outside |
| Interrupts | MSI-X, two vectors in use, both counting (`/proc/interrupts` of the guest) |
| Pause and resume of the VM, then reading again | works |
| `controlvm savestate` with the device in the VM | refused; the log and the runtime error of the VM say why; the VM runs on. `VBoxManage` itself only says that saving failed |

This is the first time the device passed anything through.

**Run 222** (2026-11-17) added the device in the hot-plug capable slot,
on a runner of another kind than the two runs before it. 67 checks, two
of them failed, both for one check being wrong:

| What | Result |
|------|--------|
| A VM with the drive in the hot-plug capable slot | boots; the guest is given PCIe hot-plug, its driver has the slot (`Slot Status 0x0040`: occupied), the guest reads from the drive |
| The layout Main stores | stored |
| `controlvm unplugpci` | the guest is asked (`pciehp: Slot(1): Button press: will power off in 5 sec`), lets go, the command returns after 6 seconds, the guest has no drive any more, `showvminfo` tells `hotplug,absent,unused` |
| `controlvm plugpci` | the guest is told (`Button press: will power on`, `Card present`), switches the slot on, its driver binds, it reads from the drive what is on it |
| `controlvm unplugpci --timeout=1000 --force` while the guest uses the drive | the device is taken away, the guest notices, the VM runs on; `plugpci` afterwards gives the guest the drive back |
| A VM whose device is not bound to `vfio-pci` when it starts | starts from the stored layout with the slot empty; `plugpci` is refused while the device is not bound, and puts it in once it is; the guest reads from the drive |
| `unplugpci`, `savestate`, start again with the device there | the state is saved and loaded, and the device is put into the slot again by itself, as designed for a device which is there when a state is loaded: the guest takes it and reads from the drive. The check expected to have to call `plugpci` and failed on "is in the virtual machine already"; it is corrected, and the case of the device not being there when the state is loaded is added |
| The host wants the device back (`echo 1 > /sys/bus/pci/devices/.../remove`) while the guest uses it | the kernel relays the request once, the guest is asked and lets go, the host has the device after 6 seconds, the VM runs on; after a rescan `plugpci` gives the guest the drive back |

The whole second part took 14 minutes. Among the messages of the guest
kernel about the slot and the drive, in all phases, there is none about a
timeout, a failure or an error.

The logs of the VMs have two lines which read like errors and are none
where they stand: the guest switching the slot off resets the device, and
that reset finds the function already taken out of service (`Failed to
update command register in device (VERR_SEEK_ON_DEVICE)`) or the host
holding on to the device it is waiting for (`The device could not be
reset (VERR_TRY_AGAIN)`). The device is released either way and the host
kernel resets it then. The device no longer writes them there, see run
224 below.

**Run 223** (2026-11-17) is the first in which everything passes: 77
checks, none failed, none skipped, 20 minutes. New in it: a state which
is loaded while the device is there (the device is back in the slot by
itself and the guest reads from the drive), one which is loaded while the
device is not there (the slot stays empty, `plugpci` puts the device in
afterwards), `plugpci` refused for a device which is in the VM already,
and the script of the checklist in [INSTALL.md](INSTALL.md): on the
runner it tells that there is no IOMMU, in the simulated host it finds
nothing missing for the drive.

**Runs 224 and 225** (2026-11-17) are for the one change to the code
which the simulation led to, the commit `dd43ddf3` on top of the merge
request as it was: a device on its way out is not reported as failing.
A failed write to the command register is not logged for a function
which is out of service by then. A reset which the host refuses because
it is busy with the device is told apart from a function which has no
reset of its own: the bus is not reset instead, which the host would
refuse as well, and one line says what is the case (`The device is not
reset now, the host is busy with it and refuses. The host resets it when
it is let go of`). Nothing changes for the guest or the host. The fake
kernel of the testcase refuses resets while it waits for a function, as
the kernel does, and a new test has the host ask for a device the guest
uses (56 tests). Run 224: the tree builds, the tests pass, and the
simulation passes with `sim_strict: yes`, 79 checks, among them that the
logs of the VMs have none of the old lines; the new line is in the log
where the host asks for the device. Run 225: the tests with the
sanitizers, no report. The new test was not run against the device as it
was: it fails there by construction, as the old reset asks for the reset
of the bus which the fake kernel counts as refused.

The simulation has passed on both kinds of runner seen so far (AWS in
runs 221, 223 and 224, Azure in run 222). With `sim_strict: yes` a run
fails when a check fails; run 224 was made that way.

What the simulation does not have, and cannot: a device with more than
one function, a graphics card, Thunderbolt. What it could have and does
not yet: a device which is pulled without the host being asked first, and
an error of the device reported by the host.

**A Windows guest** (prepared on 2026-11-17, first run on 2026-11-18, see
run 226 below). The guest the work is for is Windows, and no Windows had
met the hot-plug capable slot before. A workflow can have one after all: Microsoft offers Windows
Server as an evaluation image for download, and it installs without
anybody at the keyboard. The flavor `winsim` of the workflow does that
(`sim/win-phases.sh`, `sim/win/`):

1. The first part of the simulation as before, with the Linux guest on
   the runner.
2. Windows Server 2025 (the edition without the desktop) is installed
   into a VM which is configured for a device of the host with hot-plug,
   the device not being there. Setup and the installed Windows both meet
   the PCI Express root bridge, what the firmware hands over, and the
   port with the slot.
3. A script in the guest tells on the serial port what Windows made of
   them: which root bridge it found and whether it works, whether the
   firmware handed PCIe hot-plug over (a property Windows keeps of the
   root bridge), whether its PCI driver runs the port, devices with a
   problem, crashes.
4. From the outside: the registers of the slot as the guest left them
   (`VBoxManage debugvm ... info pci verbose`, read by `sim/slotregs.pl`).
   A guest whose hot-plug driver runs the slot has switched on the
   hot-plug interrupt of the slot and the events it is for. The Linux
   guest is checked the same way from now on, in the phases `emptyslot`
   and `hotplug`.
5. `plugpci` of the device which is not there, a saved state with the
   empty slot, the power button, and the installed Windows started a
   second time.

What this can show: whether Windows takes the root bridge, the firmware
tables and the empty slot as they are. What it cannot: Windows with a
device in the slot, as the runner has no IOMMU. Whether Windows can be
started in the virtual machine standing in for a host, where the drive
is, depends on what a start of Windows costs there; the run counts the
exits of a start for that.

Tried before the first run, on a Windows 11 at hand: the script of the
guest (it finds the root bridge `PNP0A03` there, hot-plug not handed
over, as expected of a VM without this work), and the checks of
`win-phases.sh` against what that script told and against made-up
answers of `VBoxManage`. Not tried: the installation itself. The answer
file for setup follows what public templates for unattended
installations of Windows Server 2025 in virtual machines have. Two things
reported about such installations are provided for: setup of this
version is said to leave disks on some buses offline, not on SATA, which
is what the VM has; and installations with EFI are said to end in the
shell of the firmware when the boot loader of the DVD does not get the
key it asks for, so the Enter key is pressed until setup writes to the
disk, and the boot loader is started by name should the shell be there
after all. A second reader for these scripts was started and did not
come back with anything (it hung), so they go into their first run with
the author's own checks only.

**Run 226** (2026-11-18, the head of the merge request, 31 minutes): the
first Windows guest. 41 checks, 40 pass.

| What | Result |
|------|--------|
| Windows Server 2025 (evaluation, without the desktop) installed without anybody at the keyboard into a VM with the empty hot-plug capable slot | installed and reporting after 411 seconds; setup wrote to the disk 25 seconds after the VM was started, and asked nothing |
| The PCI root bridge | `ACPI\PNP0A08`, working, driver `pci` |
| What the firmware hands over, by the properties Windows keeps of the root bridge | PCI Express native control: yes; native hot-plug: yes; PME: yes; AER: yes; the capability structure: yes; SHPC hot-plug: no (Windows did not ask); extended configuration space: available |
| The port with the slot | `PCI Express Root Port`, status OK, no problem code, driver `pci` (`pci.inf` of Windows itself) |
| The slot registers, read from the outside | slot control `0x17fb`: hot-plug interrupt on, with it the attention button, presence, power fault, link state and command completed; power of the slot off, both indicators off. That is a guest whose hot-plug driver runs the slot. (The Linux guest in the same run: `0x17f1`, the same without presence and power fault.) |
| Devices with a problem | one, the device of the Guest Additions, which has no driver in a Windows without them |
| Crashes, and errors or warnings in the system log from ACPI, PCI, WHEA | none |
| `showvminfo`, `plugpci` of a device which is not there | `hotplug,absent,unused`; refused, Windows runs on |
| A saved state with the empty slot | saved and loaded, Windows goes on where it was |
| The installed Windows started a second time | its report is done after 32 seconds, everything as before |
| The power button | **Windows did not shut down** within five minutes. Not yet known whether the press arrived: what Windows does on a press is a setting, and the editions differ in it. The next run has Windows tell the setting, sets it to shutting down, and has the Linux guest count the interrupts of the ACPI hardware around a press, in a VM with the slot and in one without |

The VM had the guest type `Other` in this run, as the script did not
find the Windows types in the list VirtualBox gives (a mistake in the
script, corrected).

What a start of Windows costs, which decides whether it can be started
where the drive is: 524000 exits from the guest to VirtualBox for a
start up to the report (the Linux guest: 200000). At the millisecond an
exit took in the virtual machine standing in for a host, that is some ten
minutes. In that second start Windows read the HPET of VirtualBox twice
in all, so it keeps its time otherwise (the VM shows it a time stamp
counter which is invariant). During setup and after a saved state was
loaded it read the HPET constantly, 16000 times a second after the state
was loaded, which would not do there; so no state is loaded there. The
next step is what the first run was to decide about:

**Windows with the drive in the slot** (written on 2026-11-18, run 227).
The Windows which was installed on the runner is started in the virtual
machine standing in for a host, its disk given to that machine as a
disk, with the NVMe drive in the hot-plug capable slot. The script in
the guest tells when a device behind the port comes and goes and which
driver has it, and reads the first 8 MiB of the drive, whose checksum is
known outside. From the outside: `unplugpci` with the guest being asked,
`plugpci`, `unplugpci --force`, `plugpci`, as with the Linux guest. The
guest is told to shut down by a DVD which is put into its drive, so
nothing depends on the power button. All of this was tried beforehand
against a made-up `VBoxManage` whose answers stand in for the guest, as
far as that goes.

**Run 227** (2026-11-18, the head of the merge request, 74 minutes): the
part with the Linux guest passed with its new checks. The installation of
Windows did not get through, so Windows with the drive in the slot was
not reached.

| What | Result |
|------|--------|
| The slot registers of the Linux guest, read from the outside (new) | passed: the hot-plug interrupt of the slot and its events are on |
| The power button with the Linux guest (new) | the guest counts one more interrupt of the ACPI hardware after each press: in a VM without the slot, in one with the empty slot, and after a saved state was loaded. A press arrives; that Windows did not shut down in run 226 is about Windows |
| The installation of Windows | setup wrote to the disk after 38 seconds (18 presses of the Enter key), showed "Searching for disks" and stayed there for 50 minutes, nothing on the serial port |

Why: the VM stood still from 40 seconds after its start. Its log tells at
power-off that each of the two CPU threads had been in one single call
into KVM for the last 3002 seconds, that virtual time stood at 39.8
seconds, at a timer of the HPET which was due, and that a timer of the
real clock was 50 minutes overdue. The timers of VirtualBox are run by
the thread of CPU 0, which has to be got out of KVM for that with a
signal. A signal which arrives after that thread has looked for work and
before it has entered KVM gets it out of nothing, and it is not sent a
second time while the work waits. The next time the guest leaves for
VirtualBox makes up for that. Windows, which has its tick from the HPET
of VirtualBox here, had halted both CPUs for waiting for that tick: no
tick, so nothing to leave for, so no tick. Until second 38 the presses of
the Enter key had made the guest leave once a second.

This is in VirtualBox as it is without this work (`tmR3TimerCallback` in
`VMMR3/TMR3.cpp` and the loop around `KVM_RUN` in
`NEMR3Native-linux-x86.cpp`), on a Linux host with KVM, for a guest which
has its tick from a timer VirtualBox emulates. A VM with a network
adapter or with somebody at its keyboard gets going again with the next
interrupt, which makes it a hiccup there; this VM has neither. The one
difference between this VM and that of run 226 is the guest type, Windows
instead of Other, with which VirtualBox shows the guest that it runs
under a hypervisor. Whether that made the difference or chance did is not
known.

What was done: one commit on the branch `scratch/timer-wakeup`, which is
the head of the merge request plus that commit. The function which looks
at the timers every 10 ms notifies the thread again when the work has
been waiting at two of its calls in a row while the thread executes the
guest, and says so in the log. Linux hosts only. This does not close the
window; it bounds what a missed notification costs, to 20 ms. The Windows
part of the simulation watches for a VM which stands still from now on:
once a minute it looks whether the guest has left for VirtualBox at all.
If not, that is a failed check, and the thread of CPU 0 is woken from the
outside so that the run goes on. It also tells how often VirtualBox
wrote the new line into its log.

**Run 228** (2026-11-18, `scratch/timer-wakeup`, 161 minutes): the commit
about the timer builds and does what it is for. Windows with the drive in
the slot was not reached again, for another reason.

| What | Result |
|------|--------|
| The installation of Windows, the VM as in run 227 | gets through, in 3072 seconds. No VM stood still. VirtualBox wrote the new line 64 times in the first 11 minutes of that VM (it writes it 64 times at most) and 11 times in the three minutes of the VM of the second start: with this guest a notification is missed every ten to twenty seconds |
| The checks of run 226 | all pass again |
| The time Windows takes | setup 51 minutes where run 226 had 7, a start 208 seconds where it had 32. During setup Windows read the HPET of VirtualBox 62 million times (3.8 million in run 226) and had its tick from it (3.2 million interrupts of the first timer of the HPET, none in run 226) |
| The power button | Windows in the VM with the slot does not shut down; the picture of the screen shows it idle at the menu of its console. The same Windows shuts down at once in a VM without the slot |
| Windows in the virtual machine standing in for a host, with the drive in the slot | the firmware starts it (after finding nothing to start on the drive in the slot, which is in order), its kernel is loaded two minutes after the start, and there is no report of the guest within 50 minutes. The VM did not stand still: 2 million reads of the HPET in that time |

Why Windows is slow: the VM has a Windows guest type since run 227. For
such a guest VirtualBox means to show the interface of Hyper-V. With KVM
it cannot, shows its minimal interface instead and says so in the log.
Windows then knows of a hypervisor which tells it nothing about time,
does not use the time stamp counter and takes the HPET, each read of
which is an exit to VirtualBox. The VM of run 226 had the guest type
Other by mistake, so no such interface at all, and Windows took the time
stamp counter. That is a setting of the VM (`--paravirt-provider none`).
The Windows part of the simulation uses it from the next run on; the
hardware test gets told once that run has shown it again. It is also
what makes the missed notifications matter: a Windows which has its tick
from the HPET waits for a timer of VirtualBox whenever it is idle.

The power button is open. The Linux guest gets the interrupt with the
slot and without. The next run looks at the I/O APIC of the VM before and
after a press, and presses the button in three more VMs: one with the
port and its slot where the firmware tables do not hand native hot-plug
over, one without the port where they do, and one with neither.

**Run 229** (2026-11-19, `scratch/timer-wakeup`, 72 minutes): the Windows
VM without a paravirtualization interface.

| What | Result |
|------|--------|
| The time Windows takes | setup 569 seconds, a start 63 seconds, on the same kind of runner as run 228 (an Intel processor). Run 226 was faster still and had an AMD processor: there Windows did not have its tick from the HPET of VirtualBox, here it has, 800 times a second |
| The power button in the VM with the slot | Windows shuts down |
| The power button after a saved state was loaded | Windows does not shut down. The I/O APIC of the VM, read from the outside: the entry of pin 9, which is the interrupt of the ACPI hardware, has its remote IRR bit set before the press already, and after the press the interrupt is waiting in addition |
| The power button in the three VMs for comparison | Windows shuts down in each |
| Windows in the virtual machine standing in for a host, with the drive in the slot | no report of the guest within 50 minutes again: 5.9 million exits, 880000 interrupts of the HPET |

The power button, then, is not about the slot. With KVM the local APICs
are those of the kernel, which passes the EOI of a level-triggered
interrupt on to the I/O APIC of VirtualBox only for vectors it was told
of, and it is told when the guest writes an entry of the redirection
table. After a saved state was loaded the entries are there and the
kernel was not told: the first interrupt of such a pin is delivered, the
I/O APIC never learns that the guest is done with it, and it is the last
one. A Linux guest does not depend on that for this pin. This is in
VirtualBox as it is without this work (`ioapicR3LoadExec` in
`DevIoApic.cpp`, `apicR3KvmIoApicRteChanged` in
`APICR3Nem-linux-x86.cpp`). One more commit on `scratch/timer-wakeup`
(`f563e7f8`) tells the APIC about the loaded entries, on Linux hosts
with KVM only; the check which failed is what it has to turn. Why the
press did nothing in run 228, in a VM whose state had not been loaded,
is not known; the I/O APIC is shown before every press from now on.

Windows where the drive is: given up on runners with an Intel processor,
where it does not get through a start with either setting. On the AMD
processor of run 226 a start took half a million exits, which would be
some ten minutes there; the part runs when a run happens to get such a
runner and is skipped otherwise. Until it has run, Windows with a device
in the slot is for the hardware test alone.

**Run 230** (2026-11-19, `scratch/timer-wakeup` @ `f563e7f8`, 106
minutes): the commit about the I/O APIC, on a runner with an AMD
processor.

| What | Result |
|------|--------|
| The commit about the I/O APIC | builds without a warning, and Windows shuts down on the power button after a saved state was loaded. The entry of pin 9 has no remote IRR bit left before the press |
| Everything else on the runner | passes: 47 checks with the Linux guest and with Windows, none fails. Setup of Windows in 414 seconds, a start in 37 with two reads of the HPET. VirtualBox made up for a missed notification of a timer in the VM of the setup and in that of the second start, 64 times each, which is as often as it writes the line |
| Windows in the virtual machine standing in for a host, with the drive in the slot | ran for the first time on an AMD processor and did not get to its report in 50 minutes. The firmware of the VM talks to the drive in the slot (it looks for something to start on it, finds nothing, which is in order, and starts Windows from the disk). The kernel of Windows is loaded after a minute. From then on: 5.3 million reads of the HPET, no timer of the HPET set, the CPU never idle |

Why Windows does not get on there: that machine did not tell of an
invariant time stamp counter. The runner does, QEMU leaves it out of the
processor it shows unless asked, VirtualBox tells its guest what its host
tells it, and a Windows which is not told reads the time from the HPET,
with an exit for every look at the clock. The log of VirtualBox has it:
CPUID leaf 80000007 of the guest is 00000100 on the runner and 00000000
in that machine. On the runner a start of Windows takes 430000 exits in
all. This is about the machine the simulation builds, not about
VirtualBox and not about a real host, whose processor tells of the
counter. The machine for Windows asks QEMU for it from the next run on
(`invtsc=on`); the machine for the Linux guest stays as it passed.

What else changes with the next run: a Windows which did not get through
its start is powered off instead of being asked to shut down, which took
20 minutes of this run, and the start gets 40 minutes. The flavor
`allsim` runs the Linux guest and Windows in one run, with one build;
`sim_strict: linux` lets such a run fail for a check with the Linux guest
and not for one with Windows, which is still being found out and has a
download from Microsoft in it.

**Run 231** (2026-11-19, `allsim` with `sim_strict: linux`, the same
commit, 93 minutes, an AMD processor again): **failed**, for four checks
with the Linux guest in the virtual machine standing in for a host. All
four are waits which ended before the guest was done. Nothing in the run
points at the code.

| What | Result |
|------|--------|
| The tree, the 56 tests, everything on the runner | builds and passes, Windows included: setup in 370 seconds, a start in 32, the power button after a saved state was loaded |
| The Linux guest where the drive is | 60 of 64 checks pass, among them all of hot-plug, the start without the device and the saved states. Two of the six VMs were slow from their first second: the one with the drive passed through without hot-plug ran 7 minutes and the one for the host taking the drive back 12, where 2 are usual. In them the guest took 15 seconds to the point in its start it reaches in 5 otherwise, and three to eight times as long for everything about the drive |
| The four checks | the write to the drive (the check looked for it at once after the read instead of waiting for it; the guest wrote), the read after pause and resume (came after the 90 seconds it had), the guest running on after the refused saved state (its line came after the 60 seconds it had), and the drive after the host had taken it and given it back (the guest waits 40 seconds for the drive behind the controller and gave up; its driver had set the controller up and was not done with the drive in 90 seconds, without a message) |
| What the slow VMs have in common | the same number of exits to VirtualBox as the fast ones, in three to six times the time. The timers VirtualBox emulates were 1.8 and 4.2 seconds behind when the VMs ended, against less than 0.1 seconds in the four others and in all VMs of run 222 on the same kind of processor: the thread of the CPU was two milliseconds late for each of them instead of a tenth of one. VirtualBox did not have to make up for a missed notification in them. Why those two VMs and not the others is not known; a VM set up the same way was fast |
| Windows where the drive is | the time stamp counter told of does what it should: two reads of the HPET where run 230 had five million. Windows shows its start screen and is alive, and was not at its disk driver after 40 minutes: 1285 reads from the disk, which is what its loader reads, no interrupt of a device yet, 356000 exits in all |

What changes with it, all in the simulation and nothing in the code:

- The waits for a line of the guest are three times as long in the
  virtual machine standing in for a host. A wait ends when the line is
  there, so this costs time only where something is wrong. The check for
  the write waits for it now.
- The guest waits a minute for the drive behind its controller, then
  tells everything its kernel has to say and which tasks wait for what,
  and goes on looking instead of giving up.
- Every VM tells at its end how it got on: the load and the processor
  time of the machine it ran on, the busiest threads, how far the
  emulated timers are behind. The next slow VM is to tell why.
- Windows where the drive is: 2 GiB of memory instead of 3, the memory
  of the VM in huge pages (Windows goes through all of its memory early,
  and every page touched for the first time is costly at that depth,
  which is a guess the next run tests), an hour for its start, and every
  three minutes where it is, read from the outside.

The next run is for finding out again (`sim_strict: no`): the scripts
have changed too much for a run which fails by them.

**Run 232** (2026-11-19, `allsim`, for finding out, the same commit, 43
minutes, an Intel processor): all 112 checks pass, none fails.

| What | Result |
|------|--------|
| The Linux guest, on the runner and where the drive is | all 80 checks pass with the two commits which are not about the device: 16 on the runner, 64 in the virtual machine standing in for a host, where the six VMs took two minutes each and the whole part 690 seconds, as in run 224 |
| Windows on the runner | all 27 checks pass again, on an Intel processor this time: setup in 560 seconds, a start in 72, the power button also after a saved state was loaded |
| Windows where the drive is | skipped, the runner had no AMD processor. What was changed for it after run 231 has not run yet |
| How the VMs got on | every VM told. Nothing was slow, so the report has nothing to explain yet. One thing it corrects: how far the emulated timers are behind says little by itself. With the Linux guest on the runner they are behind by two thirds of the time the VM has run, in every run since 222 and before the commit about the timer, in VMs which are as fast as they get. With KVM VirtualBox looks at its timers every 10 milliseconds while the guest waits in the kernel, and a guest which keeps one of them set for less than that has it late every time. The two slow VMs of run 231 are still not explained |

With this run the two commits have been through everything the head of
the merge request has been through with the Linux guest, and through
Windows on both kinds of processor. What is left before they go into the
merge request is the run with the sanitizers.

### The maintenance branches, from 19 November

The merge request is on the development line of VirtualBox (`main`,
which calls itself 7.2.97). Nobody runs that. Upstream has two
maintenance branches in the same repository, `VBox-7.2` (7.2.22 is its
last release) and `VBox-7.1` (7.1.18).

| | `VBox-7.2` | `VBox-7.1` |
|---|-----------|-----------|
| The passthrough device (`DevPciVfio.cpp`) | there: upstream took it over from `main` in May 2026, with what it needs in PGM, PDM and the runtime | not there |
| What it lacks of the device as on `main` | four commits of June and July 2026: the capability pointer which may point backwards, the quirk for one graphics card, guest memory mapped in two ranges, a fix for the legacy interface | everything |
| The management of upstream (host PCI devices in the API, `--pci-attach` with a domain, Main configuring the device) | not there: the old PCI passthrough code is still in the tree, the device is configured through extra data only. On `main` this is some twenty commits of February and March 2026 | not there |
| The other files the 36 commits touch | differ from `main` in their headers only, apart from the I/O APIC, the time manager, `VBoxManage` and Main | all differ; the files of the VMM have other names |
| KVM as execution engine | the source is there and is **not built** for Linux on x86: `Config.kmk` builds the KVM backend for Linux on ARM only, and a VM does not start without the support driver. The development line builds it for x86 since 2026-02-02 (r172508) and has changed it with 58 commits since, none of which is here: on this branch the APIC is emulated in VirtualBox, not in the kernel | the same as 7.2, with a backend which differs from that of 7.2 in some 80 lines |
| What the device of upstream runs on | the VirtualBox kernel driver, as everything on this branch: the changes it came with reach into ring-0 for that | |
| Building it with the workflow here | works since run 236, with a `configure` which takes other options and translations which have to be switched off | not tried yet |
| Releases | 7.2.22 of 2026-10-19, a release with security fixes | 7.1.18 of 2026-04-21 is the last one. The branch got commits until 2026-09-25 and no release since April, while 7.2 had 7.2.20 and 7.2.22 in September and October |

What this means for the work, which takes KVM as execution engine and a
VM process without the support driver (security requirement S11: the
memory of the guest has to stay where it is while a device can reach
it): on the maintenance branches it does not run as they are built. Run
240 tried the change which builds the KVM backend there: the backend of
7.2 for x86 does not compile. It is the backend as it was before upstream
made it build and work, with a build fix on 2026-01-25 and some forty
changes after it (the APIC of KVM, interrupt injection, the MSRs a guest
gets intercepted, the state of the CPUs, timekeeping). Taking those to a
maintenance branch is a change to its VMM far beyond PCI passthrough, and
it is not done here.

So there are three ways from here, and which one it is depends on what
the hosts this is meant for can run:

1. The development line, which becomes the next release of VirtualBox
   with KVM on Linux. The merge request is that. Nothing more to do but
   to follow upstream until the release.
2. The engine of the maintenance branches, which is the VirtualBox kernel
   driver. The device of upstream runs on it on 7.2. The work would have
   to give up S11 as it stands and make sure another way that no page of
   guest memory changes its place or is given back while a device can
   reach it (all memory allocated when the VM starts, no ballooning, no
   page sharing, no replacing of pages by large ones, and the kernel
   holding on to every page it has mapped for the device). That is a
   piece of work of its own, in PGM and in the device, with a simulation
   which loads the kernel driver, and it is not started. On 7.1 it also
   takes the changes to the kernel module which upstream made for the
   device on 7.2.
3. KVM on a maintenance branch, with the backend of the development line
   taken over. Not recommended, see above.

The branches for 7.2 and 7.1 stay as what they are: the work and its
tests on those bases, building, for whoever takes way 2 or 3.

A fourth way showed up later on 2026-11-20: there is a KVM backend for
the released lines which is not upstream's. Cyberus Technology publishes
one for VirtualBox as a patch
([virtualbox-kvm](https://github.com/cyberus-technology/virtualbox-kvm),
GPL version 3, last updated 2026-10-21), and that patch goes onto the
7.2 branch with nothing but its line endings in the way. It uses the
interrupt controller of KVM with the I/O APIC in VirtualBox, keeps the
memory of the guest in the VM process in one piece, and makes the
process do without the support driver, which is what the passthrough
device asks of an engine. `pci-passthrough-hotplug-7.2-kvm` is the branch
for 7.2 with that patch on top (one place merged by hand). It builds,
and the simulation runs on it with the device (runs 247 and 248 below).
Its readme calls AMD processors experimental.

State of `pci-passthrough-hotplug-7.2` (pushed): the 7.2 branch of
upstream, then one commit with 24 revisions of upstream (the management
in the API, `VBoxManage` and Main, the PCI domain in host addresses, the
removal of the old passthrough code, four changes to the device; without
the list of PCI IDs, a data file this fork does not copy, so that Main
has no names for the PCI devices of the host), then 35 of the 36 commits
(all but the one about the I/O APIC, which is about the KVM backend of
the development line and does not fit this one), then r172508, which
builds the KVM backend, and the commit which takes r172508 back after run
240. The device, its testcase and the configuration
of the VM in Main are line for line what the merge request has. It
builds without a warning in a file the work changes and the 56 tests
pass (run 238). `IConsole` keeps its UUID there: the three new methods
take the place of three of its reserved ones.

State of `pci-passthrough-hotplug-7.1` (pushed, being built): the 7.1
branch of upstream, the device as upstream took it to 7.2 (without what
that changed in the kernel module and its runtime: on 7.1 an MMIO2
region with an existing backing is refused in a VM which runs on the
ring-0 hypervisor), the management of upstream (without the removal of
the old passthrough code, which is switched off there as it is), and the
same 35 commits, which went on with one conflict of a line. It builds
and its tests pass since run 246. One commit more since run 250:
`Console::i_saveState` returns its result, as on the lines after 7.1
(see that run). Two more since run 252: what the 7.2 branch received
and this one had not, see "The 7.1 line and what the 7.2 line received
after it".

**Run 247** (2026-11-20, `allsim`, for finding out,
`pci-passthrough-hotplug-7.2-kvm` @ `2139a88e`, 27 minutes, AMD EPYC
9V74): the first build and the first simulation of the 7.2 branch with
the backend of Cyberus Technology.

- It builds with `configure --with-kvm` (7 minutes 18 seconds, every
  file again because the option changes the defines of all of them), and
  the tests of the device pass.
- On the runner all of it passes: the Linux guest with the empty slot,
  with a saved state and the power button, and Windows (installed in 546
  seconds, a start takes 45, the slot is run by its hot-plug driver, the
  power button works after a loaded state). The same processor with the
  backend of the development line had 414 and 37 seconds in run 230.
  This is the first time VMs of this work run on a released line of
  VirtualBox.
- Inside the virtual machine standing in for a host no VM gets anywhere:
  each stops a millisecond after it was started, with
  `Guru Meditation -6893 (VERR_NEM_IPE_3)` and no line saying what
  failed. So nothing with a device was tried on this backend yet.

What was found about the last point by reading: the status is what the
backend returns when one of the calls fails with which it hands the
state of the CPU to KVM, and a release build keeps quiet about which.
Other than the backend of the development line this one hands over two
registers more, `IA32_ARCH_CAPABILITIES` and `IA32_SPEC_CTRL`. The
second is one whose value the kernel tries on the processor of the host
before it takes it, and the processor of that host is a virtual one
which has no such register; the two levels differ in nothing else that
the logs show. Changed on both `-kvm` branches, as a commit after the
backend: the register is handed over for a guest processor which has it
only, and the checks of these calls write to the release log.

**Run 248** (2026-11-20, `allsim`, for finding out,
`pci-passthrough-hotplug-7.2-kvm` @ `e45e74e1`, 74 minutes, Intel Xeon
6975P-C): the same with that change.

- **The device runs on the released line 7.2.** Inside the virtual
  machine standing in for a host all 64 checks with the Linux guest pass
  on the backend of Cyberus Technology, in 608 seconds: the drive passed
  through and read and written, pause and resume, a saved state refused
  while the device is in the VM; the drive in the hot-plug capable slot,
  taken out with the guest asked, put in, taken by force, put in again; a
  start without the device and the device put in later; saved states
  with the empty slot, loaded with and without the device there; the
  host asking for the device back. No assertion and no unexpected error
  in the logs of the six VMs, and no VM was behind with its timers.
- On the runner the 20 checks with the Linux guest pass.
- Windows on the runner did not get through its setup in 50 minutes
  (it was at the second of its three stages): 87 million exits, 57
  million of them reads of the HPET counter. The VM had no
  paravirtualization interface, which is what the backend of the
  development line wants (see run 228). This backend gives a guest the
  interface of Hyper-V with the help of KVM, and Windows without it
  takes the HPET for its clock on this processor. On the AMD processor
  of run 247 the same setting cost 28 million exits and setup got
  through. The simulation leaves the VM at its default on a build with
  this backend from here on.
- What it does not tell: whether the change after run 247 is what the
  VMs of that run needed. That was an AMD processor; on this one the
  host inside has the register. It is known when a run gets an AMD
  processor again.

**Run 249** (2026-11-20, `allsim`, for finding out,
`pci-passthrough-hotplug-7.1-kvm` @ `160d83a6`, 36 minutes, AMD EPYC
9V45): the first build of the 7.1 branch with the backend, and the
first run of the simulation with Windows left at its default
paravirtualization interface on such a build. 67 passed, 10 failed.

- It builds at the first attempt (6 minutes 52 seconds), the tests of
  the device pass.
- On the runner the 19 checks with the Linux guest pass.
- Windows on the runner, with the Hyper-V interface: installed in 276
  seconds and started in 25, with 2.9 million exits for the installation
  and not one read of the HPET (run 247, the same kind of processor
  without the interface: 546 seconds, 28 million exits). Root bridge,
  hand-over of hot-plug, port and slot, the refused plug and the power
  button are as before.
- **Windows does not go on after a saved state is loaded** there. The
  state is saved and loaded and the guest writes one line more. After
  that its line which comes every few seconds does not come any more, the
  power button does nothing, and told to shut down it says that it does
  and is still running four minutes later, with a few hundred exits a
  second: a guest which reacts and whose time does not move as it
  should. When the state is saved the backend
  fails to read eleven registers of the Hyper-V interface per CPU from
  the kernel and fails to write them when it is loaded; those are
  registers this kernel does not have, which is noise, and what Windows
  is missing after the load was not found. It is the backend and that
  interface, not the slot: the Linux guest goes through the same round
  on this backend (runs 247 to 249), and so did Windows without the
  interface (run 247). Whether the version of the backend for 7.2 has it
  too is not known yet. A VM with a device of the host cannot be saved
  anyway; this is about a VM whose slot is empty.
- Inside the virtual machine standing in for a host every VM stopped at
  its first step again, and the release log names the call now:
  `nemHCLnxExportState: rc=11 iMsr=12 (->0x48)`. Register 0x48 is
  `IA32_SPEC_CTRL`, as read after run 247, but the condition of the
  change after that run was the wrong one. The guest processor there has
  a feature which is in the register (PSFD), so it was handed over; the
  kernel of that host does not take it, its own processor being a
  virtual one which has the feature and not the register. Changed on
  both branches: the backend hands back to the kernel what it read of
  the register when the VM is set up, once, and leaves the register out
  from then on when the kernel does not take that. Where the kernel
  takes it, which is every host which is not itself a VM of this kind,
  nothing differs from the backend as it is published.

**Run 250** (2026-11-20, `allsim`, for finding out,
`pci-passthrough-hotplug-7.1-kvm` @ `2aaddf20`, 54 minutes, Intel Xeon
6975P-C): the same branch with the second change about the register. 95
passed, 6 failed.

- **Windows takes the drive in the hot-plug capable slot, lets go of it
  and takes it again**, inside the virtual machine standing in for a
  host: all 35 checks of that part pass, for the first time on any
  branch. Windows starts with the drive of the host in the slot (454
  seconds at that depth), its NVMe driver has it and reads the checksum;
  `unplugpci` presses the attention button and Windows switches the slot
  off after 6 seconds; `plugpci` puts the drive back and Windows reads
  from it; `unplugpci --force` takes it from a guest which uses it and
  the VM runs on; put in a third time it is read again. No line about a
  failed reset or register write in the log, the VM never stood still,
  Windows shuts down when told to. This is the 7.1 line with the backend
  of Cyberus Technology and Windows at its default (Hyper-V) interface.
- **A device of the host runs in a VM on the 7.1 line**, with the Linux
  guest as well: passed through, read, written, with its interrupts,
  after pause and resume.
- **Found on the 7.1 line: a refused save leaves the VM unusable.**
  `controlvm savestate` is refused with the device in the VM, as it
  should be, and after that the machine stays in the state Saving: it
  cannot be resumed and not powered off. On that line
  `Console::i_saveState` ends with success whatever became of the state,
  and the code which puts the machine back after a failed save never
  runs; a snapshot of a running VM goes on in the same way as if its
  state had been written. The lines after 7.1 return the result there.
  It is not a fault of the device, but the device refusing is what makes
  it happen in normal use, so both 7.1 branches return the result now
  (one line). The three phases after that one could not start their VM
  (four of the six failed checks), so the slot with the Linux guest has
  still not been seen on 7.1.
- Windows on the runner: installed in 341 seconds, started in 41. After
  its saved state is loaded it says nothing at all this time (the other
  two failed checks); the screen is the one from before the save. As in
  run 249.
- The host inside has the register this time and the kernel takes it:
  the line about it is not in any log, and the VMs run. An AMD processor
  is still to come for the second change.
- No assertion and no guru meditation in any log.

Changed in the simulation after it: a VM which cannot be powered off has
its process ended, so that the phases after it run; a snapshot of the
running VM with the device in it is tried next to the saved state (it
has to be refused, leave no snapshot and a VM which runs on); a Windows
which says nothing after the load is switched off and started once more,
as one which stops later already was.

**Run 251** (2026-11-20, `allsim`, for finding out,
`pci-passthrough-hotplug-7.1-kvm` @ `3b085eca`, 124 minutes, Intel Xeon
Platinum 8573C): the same with `Console::i_saveState` returning its
result and with the changed scripts. 148 passed, 12 failed.

- **The slot with the Linux guest passes on the 7.1 line**: all 67
  checks inside the virtual machine standing in for a host, in 405
  seconds. After the refused save the VM runs on. The three new checks
  pass: a snapshot of the running VM with the device in it is refused
  with the message of the device, no snapshot is left, the VM runs on.
  Then the drive in the slot taken out with the guest asked, put in,
  taken by force, put in again; a start without the device; saved states
  with the empty slot, loaded with and without the device there; the
  host asking for the device back.
- Windows on the runner: installed in 337 seconds, started in 30. After
  its saved state was loaded it went on this time and wrote its lines;
  the power button, which reached it (the pin of the I/O APIC shows the
  interrupt taken and done with), did not get it shut down in 150
  seconds, and neither did telling it to (two failed checks). Each of
  the three runs with a loaded state on this backend went wrong in
  another way. What reading the backend had shown by then, see the
  decisions: it saves and loads the registers of the Hyper-V interface
  of the first CPU only, and this VM has two.
- **Windows where the drive is stopped after the drive was taken out
  the first time** (ten failed checks). It starts with the drive in the
  slot (298 seconds), reads from it, and when asked lets go of it as in
  run 250: the attention button, six seconds, the slot switched off, the
  device let go of. From then on the guest writes nothing any more, with
  its VM running and its screen as it was. The registers of the slot
  show where it stands: Windows has switched the power of the slot off
  and has not yet switched the power indicator from blinking to off,
  which it did in run 250, and the three events of that moment (command
  completed, link state changed, presence changed) are still waiting to
  be taken note of. The port sends its message when the first of them
  comes up and none was waiting, as the specification has it, and
  Windows did not come for them. Whether the message did not arrive, or
  Windows could not take it, the logs do not tell; the VM left the guest
  some 66 times a second for an interrupt of the machine under it and
  hardly for anything else, which is a guest that executes and not one
  that waits. The same code passed all of this in run 250, on another
  processor. Not understood yet. The rest of that part then waited 70
  minutes for a guest which said nothing.

Changed in the simulation after it: when the guest says nothing any more
after a device was taken out, the part ends there, and before it does it
tells what the VM shows of the guest: the slot with the message of the
port and what the guest has masked of it, where the CPU is and whether
it moves, its interrupt flag and priority, the interrupt controllers.

**Run 253** (2026-11-20, `allsim`, for finding out,
`pci-passthrough-hotplug-7.2-kvm` @ `e759aa6f`, 39 minutes, Intel Xeon
6975P-C): the 7.2 branch with the change to the backend about the
Hyper-V registers in a saved state. **150 passed, none failed, none
skipped**: the first run in which every check of all four parts passes.

- The change builds (6 minutes 38 seconds for all of the tree).
- **Windows goes on after its saved state is loaded and shuts down when
  the power button is pressed**, two seconds after the press, with two
  CPUs and the Hyper-V interface. That is the check which went wrong in
  three ways in runs 249 to 251 on the branch for 7.1; this version of
  the backend has the same code there, and it was not run without the
  change. The saved state has the registers of the second CPU now and
  KVM hands them back without a complaint; the eleven lines per save and
  per load about registers the kernel does not have are still in the
  log, as before.
- Inside the virtual machine standing in for a host all 67 checks with
  the Linux guest pass on this branch too, the three about the snapshot
  among them (638 seconds).
- **Windows where the drive is passes on 7.2 as well**, all 35 checks
  (491 seconds; Windows starts in 413): the drive taken out with the
  guest asked (6 seconds), put in, taken by force, put in again, read
  each time. It did not stop as in run 251; that makes two runs out of
  three in which this part passes, on two branches.
- What the line about the slot shows since this run: Windows masks every
  message of the port but the one it uses, the Linux guest masks none.
  So Windows makes use of the mask register of the MSI capability, which
  is where a message waits when it is sent while masked. Whether one
  was waiting when Windows stopped in run 251 is what the next stop will
  show.

**Run 254** (2026-11-20, `allsim`, for finding out,
`pci-passthrough-hotplug-7.1-kvm` @ `65feb364`, 63 minutes of which 24
went into downloading the image of Windows, Intel Xeon 6975P-C): the 7.1
branch with the same change to the backend and with the 26 changes taken
over from the 7.2 branch. **150 passed, none failed, none skipped**, as
on 7.2 in the run before.

- It builds (7 minutes 42 seconds for all of the tree).
- **What was taken over from 7.2 has run in VMs**: all 20 checks on the
  runner and all 67 inside the virtual machine standing in for a host
  pass with it (677 seconds), with the change to the VMM about a device
  and its own MMIO region in every access of a device to guest memory.
- **Windows goes on after its saved state is loaded and shuts down on
  the power button on this version of the backend too** (4 seconds after
  the press), which is where it went wrong in runs 249 to 251.
- **Windows where the drive is passes**, all 35 checks (488 seconds;
  Windows starts in 414, lets go of the drive 7 seconds after being
  asked). That part has now passed in three runs out of four, on both
  branches; the stop of run 251 has not come again and is not explained.

**Run 258** (2026-11-20, `allsim`, for finding out,
`pci-passthrough-hotplug-7.1-kvm` @ `1bfd163b`, 42 minutes, Intel Xeon
Platinum 8573C): the 7.1 branch with what came after run 254, the fixes
to the NAT library and the change about the extra data of an appliance,
and the first run of the check with a VM behind NAT. **155 passed, none
failed, none skipped.**

- It builds (10 minutes for all of the tree).
- **A VM behind NAT**: the guest has the virtio network adapter, gets
  the address 10.0.2.15 from the NAT engine, its gateway answers a ping,
  and the file of two megabytes it fetches through the gateway from the
  runner has the right checksum. That goes through the NAT library with
  the fixes and through the VirtIO network device with what was taken
  over from 7.2. The VM has to be allowed to reach the loopback
  interface of its host for it (`--nat-localhostreachable1 on`), which a
  new VM is not; the check sets that since a correction made while the
  run was building.
- The 67 checks inside the virtual machine standing in for a host pass,
  the 27 with Windows on the runner pass (Windows installs in 422
  seconds, goes on after its saved state is loaded, shuts down on the
  power button), and **Windows where the drive is passes**, all 35
  (Windows starts in 243 seconds, lets go of the drive 6 seconds after
  being asked). That part has now passed in four runs out of five on the
  two branches; the stop of run 251 has not come again.
- Not gone through by this run: the import of an appliance (a check for
  it was written while the run was under way and comes with the next
  one), and the commit about the size of a custom ACPI table, which came
  after it.

**Run 259** (2026-11-20, `allsim`, for finding out,
`pci-passthrough-hotplug-7.1-kvm` @ `9e858a53`, 57 minutes, Intel Xeon
6975P-C): the first run with the size check in the ACPI device, with the
testcases of the host services, with an appliance exported and imported,
and with Windows in a VM with a TPM 2.0. **172 passed, none failed, none
skipped.** One testcase outside of the simulation did not run through,
see the second point.

- It builds (7 minutes on top of the cached build of upstream's 7.1
  branch).
- **The testcases of the host services**: seven of eight pass
  (`tstHGCMSvc`, `tstGuestPropSvc`, `tstClipboardServiceHost`,
  `tstClipboardServiceImpl`, `tstSharedFolderService`, `tstShflCase`,
  `tstShflSizes`); the changes taken over from 7.2 had extended two of
  them. The eighth, `tstGuestControlSvc`, ended in its first step with a
  segmentation fault. That is the testcase and not the service: it
  connects its client without the memory in which the service keeps the
  state of a client, it is the same file on upstream's 7.1, 7.2 and
  development branches, and the test driver of upstream's validation kit
  has it on the list of testcases it leaves out. The step of the run
  which reports these testcases shows as failed for it, the only step
  which does. The testcase was rewritten after the run, see below.
- **An appliance exported and imported** (8 checks): the VM of the
  simulation is given a serial port which writes to a file, a shared
  folder and an extra data item, and is exported. `VBoxManage import`
  warns that the setting of the serial port is not imported; the VM
  which was imported has no file for its serial port, no shared folder
  and none of the extra data; imported with `--options KeepExtraData` it
  has the extra data; and it boots the guest from the disk which was
  imported. That goes through the changes to the import in Main and
  through the VMDK code, both with what was taken over.
- **Windows in a VM with a TPM 2.0** (9 checks): the installed Windows
  Server 2025 starts with the TPM added (41 seconds), reports a TPM of
  specification 2.0 which is enabled and activated, runs it with its own
  driver without a problem (`ACPI\MSFT0101`), goes on after a saved
  state is loaded and shuts down. This is the TPM device of 7.1 as it
  is, with the command buffer interface which 7.2 has reworked.
- What passed before passes: the VM behind NAT, the 67 checks inside the
  virtual machine standing in for a host, the 27 with Windows on the
  runner (Windows installs in 329 seconds), and **Windows where the
  drive is**, all 35 (Windows starts in 429 seconds, lets go of the
  drive 7 seconds after being asked). That part has now passed in five
  runs out of six on the two branches.
- The size check in the ACPI device is built and every VM of the run
  starts with it. No VM here names a table as a file, so the check
  itself was not gone through.
- **A correction.** The build without the 2D video acceleration for
  guests is nothing new: `configure --build-headless`, which the
  workflow and the recipe in [INSTALL.md](INSTALL.md) use, switches it
  off on 7.1 together with OpenGL, and has done so in every run. What
  these notes and the description of merge request 3 said before this
  run, that the runs so far had it in, was wrong and is taken back. The
  line in `LocalConfig.kmk` says the same once more and makes a
  difference only for a build with the user interface, which nobody has
  made here.

After run 259:

- `tstGuestControlSvc` rewritten, as commit 7 on `backports-7.1` and on
  the two branches which carry its commits. It gives its client the
  memory the service asks for, and tests messages of the host which the
  service has to refuse, a client fetching a message with
  `GUEST_MSG_PEEK_NOWAIT` and `GUEST_MSG_GET` (what it is told, and that
  it gets nothing when it asks for another message, with another number
  of parameters, with a parameter of another type or with a buffer which
  is too small), and a client fetching it the way of older guests, which
  is the code the change taken over from 7.2 is in. Written by reading
  the service, without a compiler; run 260 was its first build.
- The step of the workflow runs more of the testcases the tree has:
  three for the image formats (`tstVD-2`, `tstVDIo`, `tstVDSnap`), and
  of those which were built all along and not run `tstAudioMixBuffer`,
  `tstUsbMouse`, `tstIntNetR0`, `tstGuestControlMockHGCM` and
  `tstClipboardTransfers`. A testcase which is new to the step is tried
  first: the run tells what it does and does not fail for it, until it
  has been seen passing.

**Run 260** (2026-11-20, `tests`, which is the build and the testcases
without the simulation, `pci-passthrough-hotplug-7.1-kvm` @ `7f324666`,
15 minutes, AMD EPYC 7763): **all 16 testcases of the step pass**, and
the device testcase with its 56 tests.

- The rewritten `tstGuestControlSvc` builds without a warning and its
  three tests pass: the messages of the host, a client fetching a
  message, a client fetching a message the old way. One of its checks
  is there for the change taken over from 7.2 (with a parameter of
  another type the client gets none of the parameters before it); that
  this check fails on the service as it was before is what reading the
  two versions says, and was not run.
- The seven testcases of the host services which passed in run 259 pass
  again; `tstGuestControlMockHGCM`, `tstClipboardTransfers`, `tstVD-2`,
  `tstVDIo` (its six built-in scripts, on VDI, VMDK and VHD images in
  memory), `tstVDSnap` (VMDK images with differencing images written,
  merged and compared), `tstAudioMixBuffer`, `tstUsbMouse` and
  `tstIntNetR0` pass.
- All of them count from now on, but for `tstIntNetR0`, which upstream's
  test driver leaves out and which measures a throughput for a minute:
  it stays one which is tried.
- This run happened to have an AMD processor and no simulation in it;
  the VMs with a device have still not had one on these branches.

**Run 261** (2026-11-20, `allsim`, for finding out,
`pci-passthrough-hotplug-7.2-kvm` @ `e759aa6f`, 38 minutes, AMD EPYC
9V45): the branch for 7.2 with the checks which came since its last run
(a VM behind NAT, an appliance exported and imported, Windows in a VM
with a TPM 2.0, the testcases of the tree), and the first run of a
branch with this backend on an AMD processor since the two changes about
the register. **172 passed, none failed, none skipped.**

- It builds (5 minutes 30 seconds on top of the cached build of
  upstream's 7.2 branch), and the 15 testcases of the tree which this
  branch can run pass. The one of the guest control service is as
  upstream has it here and is not run.
- **The VMs inside the virtual machine standing in for a host run on an
  AMD processor.** In runs 247 and 249, on such a processor, every VM
  there stopped at its first step, for a register the backend handed to
  a kernel which does not take it. Each of the seven VMs has the line
  now which the second change about the register writes (`NEM: The
  kernel does not take IA32_SPEC_CTRL for the guest (0), the register is
  left to it`) and runs: all 67 checks with the Linux guest and the
  device pass, and **all 35 with Windows and the drive in the slot**
  (Windows starts in 521 seconds, lets go of the drive 7 seconds after
  being asked, takes it again, has it taken by force and takes it once
  more). The IOMMU there is an emulated one of Intel on this processor
  too; an IOMMU of AMD has still not been part of anything.
- New on this branch and passing: the VM behind NAT, the appliance (the
  import warns about the serial port, the imported VM has no file for
  it, no shared folder and none of the extra data, has the extra data
  with `--options KeepExtraData`, and boots the guest), and Windows with
  the TPM 2.0 (starts in 25 seconds, uses the TPM with its own driver,
  goes on after a saved state is loaded). This is the TPM device with
  the command buffer interface as 7.2 has reworked it.
- Windows on the runner: installed in 260 seconds, started in 26. When
  its state is saved and loaded the log has the eleven lines about
  registers of the Hyper-V interface which the kernel does not have, as
  before; no VM of the run ended with a guru meditation.
- Windows letting go of a device has now passed in six runs out of
  seven on the two branches, on Intel processors and on this one.

**Runs 262 and 263** (2026-11-20, `sanitize`, for finding out, the
branches without the backend: `pci-passthrough-hotplug-7.1` @ `80d2f488`,
11 minutes, Intel Xeon 6975P-C, and `pci-passthrough-hotplug-7.2` @
`5d633ceb`, 13 minutes, Intel Xeon Platinum 8573C): the device testcase
built with AddressSanitizer, LeakSanitizer and
UndefinedBehaviorSanitizer on the two released lines, which it had only
been on the development line (run 233). On both: the 56 tests pass, the
three random tests run in their full length (20000 device models, two
million accesses to the root port, 100000 operations with devices
coming and going), and no sanitizer reports anything. The device, the
PCI bus code and the testcase are the same files on the branches with
the backend, so this stands for those too. The testcases of the tree
pass in both runs, 16 on 7.1 and 15 on 7.2.

**Run 264** (2026-11-20 to 21, `sim`, for finding out,
`pci-passthrough-hotplug-7.2-kvm` @ `e759aa6f`, 34 minutes, Intel Xeon
6975P-C): the first run of the simulation with a device of two
functions, which is the part `mfhost`. **160 passed, none failed, none
skipped**; 59 of them are the new part.

- The virtual machine standing in for a host has two NVMe controllers at
  0000:01:00.0 and 0000:01:00.1 behind its root port, each with a drive
  of its own. Its kernel has them in one IOMMU group with nothing else
  in it, which is what makes them one device to hand over; the checklist
  script of INSTALL.md says so for each. Both are bound to `vfio-pci`.
- **Both functions passed through, no hot-plug** (8 checks): the guest
  has two controllers, reads from each drive what is on it (two
  checksums), writes to both, gets MSI-X interrupts of both, reads again
  after pause and resume; saving the state is refused and the VM runs
  on. What the guest wrote is on both drives of the host afterwards.
- **The device in the hot-plug capable slot** (19 checks): `showvminfo`
  tells the same for each function; the layout of each function is
  stored; `unplugpci` with the first function named has the guest asked
  and both drives gone after 8 seconds; `plugpci` with the second
  function named puts the device back and the guest reads from both
  drives; `unplugpci --force` takes both away from a guest which uses
  them and the VM runs on; put in again both are read. No line about a
  failed reset or register write.
- **A start without the device** (15 checks): the VM is built from the
  two stored layouts and the guest finds the slot empty. With one of the
  two functions bound to `vfio-pci` and the other not, `plugpci` is
  refused and the guest gets nothing of the device: all functions or
  none. With both bound the device goes in and both drives are read.
  The state is saved with the slot empty, and when it is loaded with
  the device there, the guest has both drives again.
- **The host takes the device back** (9 checks): both functions are
  removed on the host at once; the guest is asked and lets go, the host
  has both after 6 seconds, the VM runs on; after a rescan the device
  is put in again and both drives are read.
- The guest numbers its two controllers differently from one time to
  the next (the first function was `nvme0` once and `nvme1` twice in one
  VM), so its script finds each drive by the PCI device it belongs to.
- What this still does not have: the host can reset each of these two
  functions by itself. Functions which can only be reset together, as
  those of many graphics cards, are gone through by the testcase alone
  ("Reset of functions which can't be reset on their own"). And the
  guest was Linux.

**Run 265** (2026-11-21, `allsim`, for finding out,
`pci-passthrough-hotplug-7.1-kvm` @ `7f324666`, 128 minutes, AMD EPYC
7763): the branch for 7.1 with the device of two functions, the first
run of Windows with such a device, and the first run of a phase about
functions which the host cannot reset by themselves. **253 passed, 2
failed, none skipped.** It is also the first run in which the VMs
inside the virtual machine standing in for a host run on an AMD
processor on the branch for 7.1.

- What passed before passes: the 34 checks with the Linux guest on the
  runner, the 67 with the device of one function, the 36 with Windows on
  the runner.
- **The device of two functions on the 7.1 line**: the 59 checks of run
  264 pass here too.
- **Windows with the device of two functions** (41 checks, all pass):
  Windows starts with both functions in the slot (607 seconds), its
  NVMe driver has both controllers without a problem, and what it reads
  from each drive is what is on that drive; asked to, it lets go of the
  device and both functions are gone (8 seconds); put in again by
  naming the second function, it takes both and reads from both; taken
  by force and put in a third time, the same. The script in the guest
  finds the disk of a controller by the device the disk belongs to.
- **Functions the host cannot reset by themselves** (9 checks, 8 pass):
  the host is told to forget how it resets each of the two emulated
  controllers, which leaves the reset of their bus, as with many
  graphics cards. The device says so of each function, the VM is reset
  and the guest boots again and reads from both drives, the device is
  taken out of the slot and put in and is read again. The check which
  fails is the one on the log: the device asks the kernel for the reset
  of the bus, **the kernel does it and answers that it failed**
  (`VFIO#0: The device could not be reset (VERR_INVALID_FUNCTION)`,
  twice). The log of that kernel says why: `pcieport 0000:00:03.0: Data
  Link Layer Link Active not set in 100 msec`. The root port which QEMU
  emulates says that it reports whether its link is up and never
  reports it up, so a kernel which has reset the bus behind it waits for
  the link in vain. That is the port of the simulation, not the device
  of this work, and a port of a real host reports its link. What the
  run does show of the device: asked for a reset it cannot have
  confirmed, it says so once per reset, leaves the functions disabled
  until the guest sets them up, and the guest gets on. The simulation
  gives the machine with two functions a port which does not claim to
  report its link from here on (2.5 GT/s), with which the kernel takes
  the reset for done.
- **Windows where the drive is, the device of one function: Windows was
  stuck early in its start**, once, and the run waited the hour it had
  for it. The firmware had loaded Windows and the kernel of Windows was
  running (the start screen with the circle), 48 seconds after the VM
  was started. From then on nothing was read from the disk (1283 reads,
  which is what the loader makes), the storage driver of Windows never
  reset its controller, the guest left for VirtualBox for interrupts of
  the host only, some 70 times a second, and never for a device or a
  register, with CPU 0 at changing places in the kernel and its
  interrupts on. The circle on the start screen moved a few times in
  the hour, going by the pictures taken every three minutes. The VM
  never stood still. The same Windows, on the same runner, started in
  the machine with two functions afterwards without anything of the
  kind, and so it had in six runs before on other processors. This is not the stop of run 251, which came
  after a device was let go of; it is before Windows has a driver for
  anything. Not understood: the clocks of the two machines and of the
  two VMs are set up the same as far as the logs tell. The simulation
  now ends such a start after 15 minutes without a read from the disk,
  tells where the CPU is, what it leaves for and what time it is in the
  VM, and starts the VM once more, with the first start counted as
  failed.

Changed in the simulation after it: the port of the machine with two
functions, as said; the look at a stuck start and the second attempt;
and Windows with the device of two functions has the functions without
a reset of their own from the start, so that every time Windows
switches the slot off the bus is reset.

**Run 266** (2026-11-21, `allsim`, for finding out,
`pci-passthrough-hotplug-7.2-kvm` @ `e759aa6f`, 68 minutes, Intel Xeon
6975P-C): the branch for 7.2 with all of that. **284 passed, none
failed, none skipped.**

- **The reset of the bus works against a real kernel.** With the port
  which does not claim to report its link, the kernel of the simulated
  host does the reset the device asks for and says so. With the Linux
  guest (9 checks): the device says of each function that it cannot be
  reset on its own, the VM is reset, the device is taken out of the slot
  and put in, the guest reads from both drives each time, and the log
  has the functions refusing their own reset three times over and no
  word of a reset which failed. The log of the kernel has the two
  functions reset together each time.
- **Windows with the device of two functions which can only be reset
  together** (43 checks, all pass): Windows starts with it in the slot
  (413 seconds), runs both controllers with its driver and reads from
  both drives; asked to, it lets go and both functions are gone (7
  seconds); put in by naming the second function it takes both and
  reads; taken by force and put in again, the same. Each time Windows
  switches the slot off the bus is reset, three times in the run, with
  no word of a failed reset. This is as near to a graphics card with its
  audio function in an enclosure as the simulation gets.
- Windows with the device of one function passes again, all 35 (it was
  stuck in its start in run 265; no start was stuck in this run).
- The 59 checks with the Linux guest and the device of two functions,
  the 67 with the device of one, and what runs on the runner itself
  pass.

What the simulation has not got of such a device: more than two
functions, a function which is not an NVMe controller, an option ROM,
legacy VGA, and a host which is not a virtual machine. (Functions
which are no drives, a ROM and legacy VGA have come since, see
"Machines with real hardware".)

**Run 267** (2026-11-21, `sim`, for finding out,
`pci-passthrough-hotplug-v1` @ `f563e7f8`, 38 minutes, Intel Xeon
6975P-C): the branch of the merge request, which is the development
line with its own KVM backend, with what the simulation has got since
its last run there (232). **169 passed, none failed, none skipped.**

- The device of two functions on the development line: all 68 checks
  pass, the 59 of run 264 and the 9 about functions which the host
  cannot reset by themselves, with the reset of the bus done by the
  kernel and reported done.
- The 67 checks with the device of one function pass, as in run 232.
- On the runner, new for this branch and passing: the VM behind NAT and
  the appliance exported and imported (the development line has the
  changes to the import by itself).
- The testcases of the tree which this line has pass, 12 of them; three
  of the shared clipboard are not where the step looks for them on this
  line, and the one of the guest control service is as upstream has it.

With that the device of two functions has been through the simulation on
all three lines, and the reset of the bus on the development line and
on 7.2. The device, the port and the testcase are the same code on the
three.

**Run 268** (2026-11-21, `allsim`, for finding out,
`pci-passthrough-hotplug-7.1-kvm` @ `7f324666`, 145 minutes, AMD EPYC
9V45): the branch for 7.1 with the changed root port, and a second look
at Windows where the device is on an AMD processor. **222 passed, 4
failed, none skipped.** The four are one thing: Windows did not get
through its start in the VMs inside the virtual machine standing in for
a host, in three starts out of three.

- **The reset of the bus on the 7.1 line**: the 9 checks of the phase
  pass, with the kernel doing the reset and reporting it done. With the
  59 others the device of two functions passes all 68 with the Linux
  guest; the 67 with the device of one function and everything on the
  runner pass too.
- **Windows on the runner**, which is one level of virtualization less
  on the same processor, with the same build: installed in 259 seconds,
  started in 25 and 21, all 36 checks pass.
- **Windows where the device is does not get through its start**, with
  the device of one function (an hour) and with the device of two (a
  first start given up after 18 minutes, a second one after 18 more).
  It is not stuck, as it looked in run 265: with the device of one
  function its disk was read some 500 times in the hour, where a start
  which works reads thousands of times in three minutes. It crawls.
- **What it is busy with**, from the counters the KVM of that machine
  keeps for the VM, which the simulation reads since run 265: in 18
  minutes the guest left 6.9 million times, 6300 times a second, of
  which VirtualBox saw 375000; 415000 interrupts were put into the
  guest, 380 a second, more than nine in ten of them after waiting for
  the guest to take interrupts again; it halted 13 times in all. No
  device of VirtualBox is involved (nothing by the I/O APIC, no access
  to a device since the firmware), so these are most likely interrupts
  of the timers which the kernel emulates for the guest, and what the
  guest does about each: some sixteen exits an interrupt. The CPU is at 100 per cent with
  that. At this depth an exit costs a hundred times and more what it
  costs on hardware (a millisecond to VirtualBox in run 220), and 6300
  of them fill the second; on a host which is not itself a virtual
  machine they would be next to nothing.
- So this is the simulation at its limit, on these AMD processors, and
  no finding about the device or about what a guest gets on hardware.
  On Intel processors Windows got through its start at this depth in
  every run so far (seven, runs 250 to 259 and 266),
  on AMD ones it did twice (run 261 on the branch for 7.2, 521 seconds;
  run 265 with the device of two functions, 607 seconds) and did not
  four times (runs 265 and 268, both on the branch for 7.1). Whether
  the two lines differ in this or the runs did is not known; the
  backend is the same code on both.
- What it does mean for the branch for 7.1: Windows with the device of
  two functions rests on one run there (265), and Windows with
  functions which can only be reset together has not run on that branch
  at all. Windows with the device of one function has passed on it four
  times (runs 250, 254, 258 and 259). The device code is the same on
  the three lines, and on 7.2 all of it has passed (run 266).

Changed in the simulation after it: every line which tells how a start
of Windows is getting on has the counters of KVM, so that a start which
works can be compared with one which does not; a start counts as stuck
when its disk is read fewer than a hundred times in a quarter of an
hour; the look at a guest which says nothing any more (run 251) has the
counters too; and the check on the resets of the bus is left out when
Windows never got up.

### INTx (WP8), from 2026-11-22

Not on any branch of a merge request yet: the work is on
`scratch/intx`, three commits on the head of
`pci-passthrough-hotplug-v1` (`6fe456f0`). The same three apply to the
branches for 7.2 and 7.1 as they are and are on `scratch/intx-7.2` and
`scratch/intx-7.1`. See "Where it is now" at the end of this section.

What was wrong is more than the notes said. They said that a device
which only has INTx does not work beyond its first interrupt. Reading
the device for this showed what the first interrupt leaves behind: the
interrupt line of the guest was raised and stayed raised until the
guest disabled INTx, switched to MSI or MSI-X, or the VM was reset. For
a guest a line which stays high is an interrupt which never ends: its
handler is called again as soon as it has returned. A Linux guest ends
that by switching the line off after a hundred thousand rounds, with
whatever else is on that line; Windows has no such way out. A driver
which enables MSI before its device can interrupt never meets this,
and the drive of the simulation is such a device, so no run did. The
driver of Windows for a High Definition Audio controller, which is what
the audio function of a graphics card is to it, uses INTx unless its
installation says otherwise, and graphics drivers are known to. So a
graphics card in a Windows guest, which is what this work is for, was
likely to meet it. That is from what is known of those drivers, not
from a run.

What the commit does: an interrupt of the device is a pulse on the line
of the guest, up and down again, which the PCI bus and the I/O APIC
have a level for. The host masks the interrupt of a device when it
passes one on, and nothing tells the device when the guest is done with
it, so the interrupt thread asks the device again (unmasks it) 100
microseconds later. A device which still has an interrupt says so at
once, which is another pulse; while it keeps saying so the time is
doubled, up to a longest time, which bounds what a device costs which
nobody deals with. It stays inside the device: the I/O APIC and the VMM
are as they were.

The longest time was 8 milliseconds in the first commit, which is what
runs 269 and 270 built, and is 2 milliseconds since the second, which
runs 271 and 272 built. A device which has its next interrupt ready every
time it is asked cannot be told from one nobody deals with, so the
longest time is also the pace of a device with many interrupts: 125 a
second with 8 milliseconds, 500 with 2. That is the price of sampling
the device instead of knowing when the guest is done, and why such a
device is better off with MSI or MSI-X.

The I/O APIC as the KVM backend of Cyberus Technology has it knows of
the trouble a raised line makes: it counts how often a line is passed
on without having gone down and holds it back for a tenth of a second
after ten thousand times (`Interrupt storm on GSI ..., delaying
injection`). With the line going down after each pulse that count
starts anew each time.

Two other ways were looked at and not taken. A callback from the I/O
APIC at the EOI of the guest is what the comment in the code had asked
for; it changes a device every VM has and the interfaces between it,
PDM and the PCI bus, on three lines and for two backends, and none of
that can be gone through by the testcase. Trapping every access of the
guest to the device while an interrupt is pending, which is what QEMU
does where the kernel cannot tell it of the EOI, needs the memory of
the device to change between mapped and trapped while the guest runs,
in the code which also takes a device out of the address space when the
host wants it back.

**Run 269** (2026-11-22, `tests`, for finding out, `scratch/intx` @
`42d73998`, 10 minutes, Intel Xeon 6975P-C): the first build. It builds
without a warning in a file it changes, and all tests pass, the new one
among them: five interrupts of a device which is dealt with at once
arrive as five pulses, with the line low after each and the device asked
after each; a device which holds its interrupt gets pulses at times
which grow, within bounds; the pulses end when it lets go, when the
guest disables INTx and when it switches to MSI, with nobody asking the
device then; a device which still holds its interrupt is heard at once
when the guest is back at INTx; after a reset of the VM the device is
asked and its interrupt arrives; and a device taken out of the slot
while it holds its interrupt gets no pulse and no question after that.
The fake kernel of the testcase has a function which holds its
interrupt for this.

**Run 270** (2026-11-22, `sanitize`, the same commit, 11 minutes): the
same with AddressSanitizer, LeakSanitizer and
UndefinedBehaviorSanitizer and the random tests in full length: all
pass, no report.

**Run 271** (2026-11-22, `tests`, for finding out, `scratch/intx` @
`b2b6e1ee`, 10 minutes, AMD EPYC 7763): the second commit, with the
longest time at 2 milliseconds and the bounds of the test changed with
it. It builds, and all tests pass.

**Run 272** (2026-11-22, `sanitize`, the same commit, 10 minutes, AMD
EPYC 9V45): the same with the sanitizers and the random tests in full
length: all pass, no report.

**Run 273** (2026-11-22, `sim`, for finding out, the same commit, 45
minutes, Intel Xeon 6975P-C): the simulation with the change, 177
checks, none failed. Its phase `intx` (on the `meta` branch) has the
Linux guest started with `pci=nomsi`, so that the driver of the drive
gets a line interrupt: the guest boots with the device, reads from the
drive, has the interrupt of the drive at the I/O APIC (`IO-APIC
16-fasteoi nvme0q0, nvme0q1`) and not as MSI or MSI-X, writes, reads
again after the VM was paused and resumed, boots again after a reset of
the VM and reads, and its kernel has no word of an interrupt nobody
answered for, of a line switched off or of a command which timed out. A
build without the change leaves the phase out. All other phases pass as
before, the device of two functions and the reset of its bus among
them.

**Runs 274 and 275** (2026-11-22, `tests`, for finding out,
`scratch/intx-7.2` @ `601e7710` and `scratch/intx-7.1` @ `97f447c7`, 10
and 8 minutes, Intel Xeon 6975P-C and AMD EPYC 9V45): the two commits on
the lines for 7.2 and 7.1, where they apply as they are. Both build, all
tests pass.

A reading by somebody who had not written it (2026-11-22). The change
to the device and its tests were given to a reader who knew neither,
with the questions a review asks: the threads and the file descriptors,
the arithmetic of the times, whether a device can end up never asked
again, and whether the tests check what they say. Nothing was found
which keeps a device from working. What was found, and what became of
it as a third commit (`6fe456f0`):

- When asking the device failed it was not asked again, with the host
  still masking its interrupt. It is tried again at the slowest pace
  now, and a test has the host refuse once.
- The check that a device which was taken out is not asked could not
  fail: the fake kernel did not see a request to what stands in for the
  device. It does now. And the device did send one such request, which
  it does not any more: a function which is out of service is not
  asked, and the interrupt thread takes the critical section of the
  slot for asking, without waiting for it.
- The fake kernel let INTx be unmasked which was not set up; vfio-pci
  does not. It refuses now and counts that with what it refuses
  otherwise, which the tests with interrupts check, the random ones
  among them.
- The rule for the time until the device is asked was tested by timing
  only, and the half of it which is for a device the host cannot ask
  directly not at all. It is a function of its own now, and a test goes
  through it by its numbers.
- The INTx test did not suspend and resume the VM. It does now, with
  the device holding its interrupt. Its lower bound for the pulses of
  such a device would have passed with a longest time of 25
  milliseconds; it goes by the time now.
- A comment was wrong about what asking at once is for when the thread
  gets going.
- Left as it is: for a device the host has to mask at its interrupt
  controller, which old devices are, the bound of 500 interrupts a
  second depends on how quickly the host passes the answer of the
  device on, and a slow host has the time start short again. What such
  a device costs stays bounded, by the shortest time plus that slowness.
  A rule which is slower to start short again would also slow down a
  device which is dealt with at once and has many interrupts, and
  nothing here can tell which of the two matters more. The comment at
  the times says so. Also left: each interrupt asks for an EMT by
  itself, which is 500 requests a second at most.

**Run 276** (2026-11-22, `sanitize`, for finding out, `scratch/intx` @
`6fe456f0`, 13 minutes, Intel Xeon Platinum 8573C): the third commit
with the sanitizers and the random tests in full length: it builds, 60
tests pass, no report.

Where it is now (2026-11-22, 11:17 UTC). The three commits are one
commit for each line, with the tree which was tested (`git diff` between
the two is empty): `4add131b` for `pci-passthrough-hotplug-v1`,
`a170413a` for the 7.2 branch, `2486f750` for the 7.1 branch. They are
not on those branches yet. Merged into the branches with the KVM
backend they are `scratch/intx-7.2-kvm` @ `26e4045a` and
`scratch/intx-7.1-kvm` @ `9f8f5cba`. Before the branches of the merge
requests get them: run 277, the simulation on `scratch/intx` with the
third commit; run 278, the whole simulation with Linux and Windows on
`scratch/intx-7.2-kvm`; and the same on `scratch/intx-7.1-kvm`. The
simulation has a step with Windows for this since (`win_intx` on the
`meta` branch): the script in the guest sets `MSISupported` to 0 for
the controller of the drive and starts the controller anew, so that
Windows gives it a line interrupt, and reads from the drive again. That
step has not run yet.

**Run 277** (2026-11-22, `sim`, for finding out, `scratch/intx` @
`6fe456f0`, 45 minutes, AMD EPYC 7763): the simulation with the third
commit: 177 checks, none failed, the phase `intx` among them. With that
`pci-passthrough-hotplug-v1` has the commit (`4add131b`, pushed on
2026-11-22). The branches for 7.2 and 7.1 and those with the KVM
backend wait for runs 278 and 279, the whole simulation on
`scratch/intx-7.2-kvm` and on `scratch/intx-7.1-kvm`.

**Run 278** (2026-11-22, `allsim`, for finding out,
`scratch/intx-7.2-kvm` @ `26e4045a`, 133 minutes, AMD EPYC 7763): the
whole simulation on the 7.2 line with the KVM backend: 264 checks pass
and 5 fail, none of them for the device. Everything with the Linux
guest passes, the phase `intx` with it: a guest which does not use MSI
has its drive at the I/O APIC that backend has. Windows with the device
of two functions got through its start and through every step with the
slot. The five:

- Windows with the drive of one function did not get through its
  start, twice, as on this kind of runner before (see "Run 268").
- The step with Windows and INTx ran for the first time. Windows
  started both controllers anew with `MSISupported` at 0, its driver had
  both again, and what it read from both drives afterwards is what is
  on them. Whether it was a line interrupt it read with, the run does
  not tell: the script in the guest asked WMI for the interrupts of a
  controller and found nothing, and the two checks which wanted a small
  interrupt number failed for that.
- After that step the guest did not shut down when it was told to. It
  was alive, with 56 lines saying so after the step. It did not see the
  DVD which tells it to shut down, which was the second DVD it was
  given; the two were alike in name and size, which is the likeliest
  reason and is not shown.

Changed in the simulation after it (`meta` @ `151334a`): what kind of
interrupt a function has is told by the host, whose `/proc/interrupts`
has a line `vfio-intx` for a function with a line interrupt and lines
`vfio-msix` for one with messages, and counts them; the checks want the
first, none of the second, and the count to have gone up while the
guest read. The script in the guest goes through all resources Windows
has given out. The two DVDs differ in name and size. Run 282, the part
of the simulation with Windows on the same branch, is the first with
that. The branches for 7.2 and 7.1 wait for it.

**Run 279** (2026-11-22, `allsim`, for finding out,
`scratch/intx-7.1-kvm` @ `9f8f5cba`, 110 minutes, Intel Xeon 6975P-C):
the same on the 7.1 line, started before the simulation was changed:
292 checks pass and 5 fail. Everything with the Linux guest passes, the
phase `intx` with it. Windows got through its start with the drive of
one function and with the device of two, and through every step with
the slot. The five are those of run 278 again, in both parts with
Windows: the script in the guest could not tell the interrupt of a
controller, and the guest did not see the DVD which tells it to shut
down. As there, Windows read from the drives after their controllers
were started anew, and what it read is what is on them. Run 283 is the
part with Windows on this branch with the simulation as it is since.

**Run 282** (2026-11-22, `winsim`, for finding out, the branch of run
278, 46 minutes, Intel Xeon 6975P-C): the part with Windows, with the
simulation as changed: all 155 checks pass. Before the step Windows has
messages for both controllers (interrupt numbers above four thousand
million) and the host has its lines `vfio-msix` for them. After it
Windows has IRQ 16 for both, and the host has one line for both:
`23: ... IR-IO-APIC 23-fasteoi vfio-intx(0000:01:00.0),
vfio-intx(0000:01:00.1)`. Those are two functions of a device which
share an interrupt line, as the functions of a graphics card can.
Windows reads what is on the drives with that, the host counting 128
interrupts meanwhile with the drive of one function and 311 with the
device of two, keeps running, and shuts down when it is told to.

With that the branches for 7.2 have the change:
`pci-passthrough-hotplug-7.2` @ `a170413a` and
`pci-passthrough-hotplug-7.2-kvm` @ `26e4045a`, pushed on 2026-11-22.

**Run 283** (2026-11-22, `winsim`, for finding out, the branch of run
279, 42 minutes, Intel Xeon 6975P-C): the same on the 7.1 line: all 155
checks pass, with the same to be seen of the interrupts: IRQ 16 in
Windows for both controllers, one line `vfio-intx` for both functions
on the host, 106 interrupts of the host while Windows read with the
drive of one function and 251 with the device of two.

With that the branches for 7.1 have the change as well,
`pci-passthrough-hotplug-7.1` @ `2486f750` and
`pci-passthrough-hotplug-7.1-kvm` @ `9f8f5cba`, and every branch of a
merge request about the passthrough work has it (2026-11-22, 14:35
UTC). What "Where it is now" above names as waiting is done. Seen
nowhere: a device which has nothing but INTx, the audio function of a
graphics card, hardware.

**A device whose second function is an audio controller** (the part
`hdawinhost` of the simulation, on the `meta` branch since 2026-11-22).
What comes nearest here to a graphics card with its sound: the device
behind the port is a drive and a High Definition Audio controller as
its two functions, both in one IOMMU group, and Windows is the guest.
Windows has a driver of its own for such a controller, and HWTEST.md
says of it that it runs the controller with a line interrupt unless it
is told otherwise; the part is to show whether that is so. It tells
which interrupt the audio function has by what the host has for it,
whether the driver of Windows finds the codec behind the controller,
and it has Windows play three seconds of silence and tells how long
that took and how many interrupts the host counted meanwhile, once as
Windows set the device up and once after the drive was told to use
INTx, so that both functions have a line interrupt. The steps with the
slot are those of the part with two drives.

**Run 284** (2026-11-22, `winsim`, for finding out,
`pci-passthrough-hotplug-7.2-kvm` @ `26e4045a`, 163 minutes, AMD EPYC
9V74): the first run with that part, and it tells nothing about it: 93
checks pass, 5 fail, 1 is skipped. The runner had an AMD processor, on
which Windows does not get through its start two levels of
virtualization deep (see "Run 268"): the parts with one drive and with
two gave up after two attempts each, and the new one waited an hour for
a Windows which crept along at the pace known from there, some seven
thousand exits a second for 440 interrupts. What the run does show of
the new part: the host has the two functions in one group and both
bound to `vfio-pci`, and the VM is created with both. The audio
controller as QEMU emulates it has no reset of its own; the step which
has the host forget the resets of the functions gave up over that and
counts it as done since.

Changed after it: a run can name the parts of the simulation it is
about (input `sim_parts` of the workflow), and such a run is cancelled
at its start when it is for Windows two levels deep and its runner has
an AMD processor, which the workflow tells in its first steps. Run 285
is the part with Windows on the runner and the new part, nothing else.

**Run 285** (2026-11-22, `winsim` with those two parts, for finding
out, the same branch, 60 minutes, Intel Xeon 6975P-C): 116 checks pass,
3 fail, 2 are skipped. The part ran. Windows started with the device of
a drive and an audio controller and went through every step with the
slot: asked to let go of it, the device put in again, taken away
without asking, put in again, each time with the reset of the bus the
device needs when its functions have no reset of their own (the audio
controller as QEMU emulates it has none to begin with). The drive
worked each time, at the end with a line interrupt.

What the part was for it could not tell. The Windows of the simulation
is a Server Core, and that has no driver for a High Definition Audio
controller: the function is there and shows with problem 28, and
Windows has nothing to play sound with. The three checks about the
codec are the three which failed; since this run that is told once, as
skipped. The line interrupt the host has for the audio function is the
one a function has before any driver has set it up.

What Windows does with such a controller where it has the driver is
told another way since: the inventory for Windows (`hw/inventory.ps1`,
see "Machines with real hardware") has a line for each graphics card
and each sound controller on the PCI bus with the kind of interrupt
Windows runs it with. Where it was tried, on a Windows 11 which is a VM
itself, the High Definition Audio controller of that VM has the driver
of Windows for it (`HDAudBus`) and a line interrupt. That is one
emulated controller. The inventory of a machine with a graphics card
tells it of the sound function of a real one.

Not done: the new test against the device as it was
(`scratch/intx-before`), a run which ends well when the test fails.
That the test fails there is what reading says: the line is left high
and the device is never asked again, and the test checks both. It was
not seen.

State of `pci-passthrough-hotplug-7.2-kvm` (pushed): the 7.2 branch as
above, the backend of Cyberus Technology in its version of 2026-10-21 as
one commit (source, licence and the one place merged by hand are in its
message), two commits about the register, and one about the Hyper-V
registers in a saved state. It is built with `configure --with-kvm`.
Every check of the simulation passes on it since run 253; run 261 went
through all 172 there are now, on an AMD processor.

State of `pci-passthrough-hotplug-7.1-kvm` (pushed): the 7.1 branch as
above, the backend in its last version for the 7.1 line (2025-11-03, two
places merged by hand), the same two commits about the register, and the
7.1 branch merged in again for the change to `Console::i_saveState`. It
builds since run 249. Since run 254 it also has the change to the
backend about the Hyper-V registers in a saved state and what was taken
over from the 7.2 branch, and every check of the simulation passes on
it. Windows has had the drive in the slot, out and in again in runs 250,
254, 258 and 259 and stopped after the first removal in run 251, which
is not understood yet. Run 259 had the head `9e858a53`, with everything
taken over from 7.2 and the size check in the ACPI device; one commit
came after it, which changes a testcase only (`tstGuestControlSvc`) and
is built, with the testcase passing, in run 260.
Merge request [#4](https://github.com/MarquisDeGris/virtualbox/pull/4).

### Machines with real hardware, from 2026-11-22

The runners of the workflow have no graphics card, no IOMMU to switch
on and no device to pass through, and the machine this work is done on
is a virtual machine. For a machine which has such things the `meta`
branch has a directory `hw/` since 2026-11-22, see its README: scripts
which are started there with one command, write down what they find
field by field, and publish it on the branch `hw-results`, from where
it is read when this work goes on. Whoever starts them does not have to
know what they are for, and an assistant is not needed for it; where
one is used, the README has the one command which keeps it to the two
scripts.

So far there is the inventory, for Linux and for Windows: processor,
virtualization, IOMMU groups, the graphics cards and what shares their
groups, what there is for building. It only reads. What it writes has
no name of a machine or a person in it, and both scripts refuse a
result which has; the fork is public.

For a machine which runs Windows the README has the plan: the
inventory there, then Linux from an external drive with the IOMMU
switched on in the firmware, a VirtualBox built by the workflow fetched
instead of built, the card bound to `vfio-pci` by the person at the
machine, and the tests as one command each.

What exists for the steps under Linux (`hw/linux/`, the README lists
them):

- `fetch.sh` fetches the VirtualBox the workflow has built and tells
  which packages of the distribution it needs and are missing. Nothing
  is built or installed on the machine.
- `prereq.sh` names the device the tests are going to be about, the
  first graphics card which is not the one the firmware shows its
  screen on, and goes through the checklist of INSTALL.md for it.
- `bind.sh` prints what giving the device to `vfio-pci` takes and does
  it when it is told to, as root. It refuses the card the firmware
  shows its screen on.
- `test-a.sh` is the first test and needs no operating system in the
  VM: a VM with the device in the hot-plug capable slot starts, the
  device is taken out and put in twice, the VM is reset and powered
  off. The result has what VirtualBox tells of the device at each step
  and the lines the log of the VM has from the device.

**The build to fetch.** The workflow has a flavor `package`: like
`tests`, and then the directory VirtualBox runs from is made one file,
with a text in it which says what it was built from and what it needs
at run time. A second workflow, `release.yml`, publishes the file of
such a run in the release `hw-builds` of the fork, a pre-release, from
where it can be fetched without logging in; that job is the one place
in the workflows which may write to the repository. **Run 286**
(2026-11-22, `package`, `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a`,
11 minutes) made the first one, 110 MB, and the release workflow
published it. `hw/builds.txt` lists it. That build is the 7.2 line with
the KVM backend; it has no emulated 3D in it (`--disable-dxvk`).
**Run 321** (2026-11-24, `package` with 3D acceleration, the same
branch @ `585a8166`, 37 minutes, Intel Xeon 6975P-C) made the build
which was fetched from then until run 362, `vbox-585a816662b0-3d-ubuntu2404.tar.zst`,
155 MB. It has the change to the device of that day, see "The device
does not ask a function which is out of service". The build without 3D
acceleration is still the one of run 286 and does not have it, which
`hw/README.md` says. **Run 322** (the same day, `sim` with the part
`barhost`, the same branch and commit, 24 minutes, Intel Xeon Platinum
8573C) fetched the new build as a machine with real hardware does: the
script which fetches gets the file which is listed, with its checksum,
and its `VBoxManage` runs; the first test goes through its 15 checks
with that build and the device of four gigabytes. All 50 checks of the
run pass. **Run 362** (2026-11-25, `package` with 3D acceleration, the
same branch @ `572356d1`, 12 minutes, AMD EPYC 9V45) made a build
which was the one to fetch for some hours,
`vbox-572356d18f9e-3d-ubuntu2404.tar.zst`, 155 MB: it has the PCI lock
where a function goes away for the guest, see "A second reader for four
later commits", and the testcase passes on that head in the same run.
No run fetched it. **Run 366** (the same day, `package` with 3D
acceleration, the same branch @ `cc3dc981`, 16 minutes, AMD EPYC 9V74)
made the build which was fetched until the next day,
`vbox-cc3dc981fd82-3d-ubuntu2404.tar.zst`, 155 MB: it has, besides, the
lock of the slot let go of where a live state is refused, see the same
section, and the testcase with its 59 tests passes in the run. **Run
367** (`sim` with the parts runner, simhost and barhost, the same
commit, 49 minutes, AMD EPYC 9V74) fetched it as a machine with real
hardware does: the script gets the file which is listed, with its
checksum, its `VBoxManage` runs, and the first test goes through its 15
checks with that build. All 130 checks of the run pass. **Run 378**
(2026-11-26, `package` with 3D acceleration, the same branch @
`b1136fa2`, 20 minutes, Intel Xeon 6975P-C) made the build which is
fetched since, `vbox-b1136fa26e96-3d-ubuntu2404.tar.zst`, 155 MB: it
has, besides, the two changes to Main of that day, see "Who waits for
whom, in Main and in the root port". The testcase passes in the run.
**Run 379** (the same day, `sim` with the parts runner and barhost,
strict, the same commit, 29 minutes, AMD EPYC 9V74) fetched it as a
machine with real hardware does: the script gets the file which is
listed, with its checksum, its `VBoxManage` runs, and the first test
goes through its 15 checks with that build. All 51 checks of the run
pass.

**The scripts tried here.** The simulation runs `test-a.sh` at the end
of its parts with the Linux guest, with the emulated NVMe drive as the
device, so that the script has run before it runs where a run costs
somebody an evening. **Run 287** (2026-11-22, `sim`, the same branch,
42 minutes, Intel Xeon 6975P-C; 177 checks pass, 2 fail) was its first
run, and the two which failed are the script, with one function and
with two: it ended at creating its VM, whose settings went to the home
directory of root and not under the directory the script says it stays
in. Corrected. **Run 288** (2026-11-22, `sim` with the parts `runner`
and `simhost`, 29 minutes, Intel Xeon 6975P-C): all 109 checks pass,
and the script goes through its 15 checks: the VM starts with the
device, VirtualBox tells the device in the slot and in use, out and in
twice, the reset, the power off, the device with `vfio-pci` afterwards,
and no line of the device in the log about something which did not
work.

**A second test** (`hw/linux/test-b.sh`, from the evening of
2026-11-22). The first test has no operating system in the VM. The
second puts a small Linux guest together on the machine it runs on: the
kernel that machine runs, the modules the kernel has for the functions
of the device with their firmware, and an init which tells on the
serial port of the VM what the guest makes of the device: which driver
has it, where its BARs are and how large, its interrupts, and for a
graphics card its connectors and whether a monitor is on one. The
script makes checks of that. It is what a run on hardware is to tell
beyond "there was a picture".

It took four runs to get it to run here (289 to 292, each `sim` with
some parts; the first script passed in all of them):

- Run 289: the guest did not start. It was to be started by a script
  for the shell of the firmware, a way the simulation has as a fallback
  and had never gone. The guest is a unified kernel image now, as the
  guests of the simulation are.
- Runs 290 and 291: the guest still said nothing, in ten minutes. The
  VM of the scripts had two CPUs and time passing at its usual pace;
  the guests of the simulation have one CPU and time at a twentieth of
  its pace in the machine standing in for the host, as every exit from
  a guest is slow there. The scripts can be told both, which the
  simulation does and a machine with real hardware does not need.
- Run 292: the guest starts. With the drive the script goes through
  its 12 checks.

**A device with four gigabytes of memory** (the part `barhost` of the
simulation, since the same evening). What a graphics card has and the
drive of the simulation does not is memory by the gigabyte, in a BAR
which the firmware of the VM and the guest have to place above four
gigabytes and which the VM has to map as a whole. Nothing had tried
that. The part has a device whose memory is nothing but memory (ivshmem
of QEMU), four gigabytes of it, kept in a file on the runner, and no
reset of its own. The two scripts run with it, and the guest of the
second writes to the first and to the last four bytes of the memory.

What runs 291 and 292 showed with it: the VM starts with the device,
the device is taken out of the slot and put in twice, the VM is reset
and powered off (the first script, 15 checks). The guest has the memory
at four gigabytes, above the memory of the VM, with its full size; it
reads back what it wrote to both ends; and what it wrote is in the file
at both ends (run 292). One check of the second script failed there
and was the check's fault: a module of the kernel names the device,
the one of the virtio devices, which names every device of their
vendor and does not take this one. Run 293 has the check as it is meant, with a device no driver is expected for, and the connectors counted by whose they are: both scripts go through their checks with the device, 15 and 12, and what the guest wrote is in the file.

That is four gigabytes with a device which has no driver. A graphics
card has more, registers besides memory, and a driver which sets the
card up; the size alone is what this tells of.

**The ROM of the device** (run 294). A graphics card has its firmware
in its ROM, and its driver reads it there. The passthrough device
copies the ROM when the VM gets the device, and until this run that
had run with the stand-in for the kernel of the unit tests only. The
device of four gigabytes has a ROM now, 64 KiB which are a file of the
runner, and the second script has the guest read the ROM and compares
its size and checksum with what the host has: what `bind.sh` read
before the device went to vfio-pci, or what the host reads when the
test starts. Run 294: `bind.sh`, which ran for the first time, read
the bytes of the file, and the guest reads the same.

**A graphics card, as far as the simulation has one** (the part
`vgahost`, run 294). The device is the standard VGA adapter of QEMU:
16 MB of video memory, registers in memory, a ROM with a BIOS in it,
legacy VGA ranges, no interrupt and no reset of its own. It is the
adapter the machine standing in for the host starts with, so its ROM
is the copy the firmware of that machine made. The kernel has a driver
for it, which makes a card with one connector of it and takes a
monitor to be on that: what the second script looks for with a
graphics card and had had nothing to find it with. Run 294, at the
first try:

- The first script goes through its 15 checks with it.
- The second, 16: the driver of the guest has the card and its video
  memory, the guest reads the 39424 bytes of the BIOS as the host
  does, the card has its connector with a monitor on it, and the
  connector of the adapter the VM has anyway is not counted for it.
- The second once more with `HW_VGA=yes`, 20: the VM has no graphics
  adapter of its own and is given the legacy VGA ranges of the card
  (`ExposeVga`), which is what it takes for a card to show a guest
  from its start. Before a driver has the card the guest reads a
  register through the legacy ports and the start of the text memory,
  which has the first letter of what the firmware of the machine
  standing in for the host wrote to its screen. It sets three
  registers through the ports and reads them back, and reads back what
  it writes to both ends of the memory at 0xa0000. Then the driver
  takes the card as before.

What this is not: a card with a display engine a driver has to set up,
with firmware to load, with interrupts, with a driver for the firmware
of the VM in its ROM, or with anything which takes its time on real
hardware. It is the paths of the passthrough device for a ROM, for
video memory which a driver maps, and for the legacy ranges, each for
the first time with a guest.

Runs 292 to 295 are of the branch for 7.2 with the KVM backend of
Cyberus Technology, which is the build that is published for machines
with real hardware. Run 296 is the development line with the same two
parts and run 297 the branch for 7.1 with that backend, the latter on
an AMD processor; all their checks pass as well, 58 and 59.

**Every script once** (run 295). What a machine with real hardware is
to run had in part run nowhere. The simulation runs all of it now,
with one of the two parts above:

- On the runner: `fetch.sh` gets the build as it is published, finds
  it to be the one `builds.txt` lists, and its VBoxManage runs. The
  inventory is written, and `publish.sh` puts it on the branch for
  results as `popolupo`, in a repository which is made for that on the
  runner and gone with it.
- In the machine standing in for the host: the inventory tells of the
  IOMMU and, in the part with the VGA adapter, of the graphics card
  with its driver, its IOMMU group and that the firmware shows its
  screen on it; `prereq.sh` finds nothing missing; and the first test
  passes once more with the build which was fetched, which is the one
  somebody at such a machine gets.

All 58 checks of the run pass. The inventory for Windows ran on one
machine, a VM, and publishing it from there worked the same way
(2026-11-22).

Since run 297 the part with the device of four gigabytes ends with
`bind.sh --undo`, after which the host has the device back.

**One command for what takes root** (`hw/linux/tests.sh`, run 299).
Binding the device, the checklist as root and the tests were four or
five commands with `sudo`. They are one now, which runs the scripts
that were there one after the other, the second test twice for a
graphics card, and tells at its end which results there are.
`prereq.sh`, which is run before it and without root, tells what the
second test needs to put its guest together, so that it can be
installed first and no test stops for it. Run 299: after the device was
given back to the host, the one script binds it again, goes through the
checklist and has both tests pass, 15 and 14 checks.

**A third test, in which the device leaves the guest and comes back**
(`hw/linux/test-c.sh`, 2026-11-25, run 357). The scripts had a VM
without an operating system which has the device taken out and put in,
and a guest whose drivers take the device. What this work is about was
not among them: a guest whose driver has the device, and the device
leaving and coming back. The third test is the second one with that
added, twice: the guest is asked for the device and lets go of it, the
device is out of the slot and stays with `vfio-pci`, it is put in
again, and the drivers which had it take it again. The guest tells on
its serial port, whenever it changes and every five seconds, which
driver has which function, how many connectors the graphics cards
behind the port have and on how many of them something is, and what its
kernel says about the devices meanwhile; the script makes checks of
that. `tests.sh` runs it last, `HW_SKIP_PLUG=yes` leaves it out.

Run 357 (`sim` with the four parts which run these scripts,
`pci-passthrough-hotplug-7.2-kvm` @ `05f1d33a`, 67 minutes, AMD EPYC
9V74): all 215 checks pass. The third test passes with the drive (25
checks; the driver `nvme` has it again), with the device of two
functions (27), with the device of four gigabytes which no driver takes
(26, and the same once more from `tests.sh`), and with the graphics
adapter of the emulator (31): its driver, `bochs-drm`, lets go of the
card and takes it again with its connector and what is on it, two to
three seconds after the card was put in. The guest lets go five to
eight seconds after it was asked, five of which its kernel waits after
the attention button. What this is not: a real graphics card, whose
driver has more to give up and to set up again than that one, and a
cable which is pulled, as the device stays where it is on the host.

Not tried: anything on a machine with real hardware and with the
graphics card of one; a device which a driver of the host takes back,
as the simulated host has none for its device; publishing to the fork
from another machine, which takes the right to push there; the command
for an assistant. No result has arrived on `hw-results` (2026-11-23,
05:45 UTC).

### 3D acceleration for VMs, from 2026-11-23

What the tracker of upstream has about 3D, requests and reports, takes
a VM with 3D acceleration to be worked on, and nothing here could run
one: the builds of the workflow have no user interface, for which
configure switches 3D off, and a runner has no graphics card.

Both can be had. A run which has `VBOX_WITH_VMSVGA3D=1` among its
arguments for kmk builds with 3D acceleration: configure is not told
to leave out DXVK, which is what renders for the graphics adapter of a
VM on a Linux host and is part of the tree from 7.2 on, and
`LocalConfig.kmk` switches on again what configure switches off. On a
runner the renderer is the one Mesa has in software for Vulkan, which
DXVK takes where there is nothing else.

The simulation has a phase for such a build, `gfx3d`, on the runner: a
VM with 3D acceleration and a guest which has the kernel driver for
the graphics adapter of the VM. With the adapter in this mode that
driver keeps what is on the screen in objects of guest memory, which
the host reads from and writes to at the guest's word; the guest
writes 4000 lines of text to its screen. Nobody looks at the picture.

Run 301 (2026-11-23, `sim` with the part on the runner,
`scratch/intx-7.2-kvm` @ `26e4045a`, 20 minutes, AMD EPYC 7763), at
the first try:

- The tree builds with 3D acceleration, in 13 minutes on top of the
  build of the base.
- A VM with it starts and boots the guest. Its log has DXVK 2.6.2 and
  Direct3D at feature level 11_1. The driver of the guest has the
  adapter, is offered shader model 5, writes its screens, and the
  guest runs on.
- 37 checks pass and 2 fail, both for what the check looked at: three
  lines the driver says of every adapter which is not of its maker,
  and counters which count something else than was thought. A picture
  of the screen as the host has it is taken in their place.

Run 302 (the same day, the same commit, with the parts on the runner,
with the device of four gigabytes and with the VGA adapter, 31
minutes, Intel Xeon Platinum 8573C): all 69 checks pass. The phase
with its checks put right: the picture of the screen of the VM, taken
from the host, is the last 49 of the 4000 lines the guest wrote, each
readable. And the build with 3D acceleration goes through the parts
with a passed through device as the build without it does, the
scripts for a machine with real hardware included.

Run 303 made the package of that build, which is published next to
the one without 3D acceleration and has "-3d-" in its name. It is the
build `hw/linux/fetch.sh` fetches when it is not told which: one build
for whatever is tried on a machine with a graphics card, the
passthrough and VMs with 3D acceleration.

**Windows with 3D acceleration** (run 306, 2026-11-23, the parts on
the runner with the Linux guest and with Windows, 47 minutes, Intel
Xeon 6975P-C, all 86 checks pass at the first try). A graphics adapter
with 3D acceleration is of use to Windows with the driver for it only,
which is part of the Guest Additions. The part with Windows has a
phase for a build with 3D acceleration: the Guest Additions which
Oracle publishes for the release line (7.2.22) are fetched and
installed in the guest without a question asked, Windows is started
again in the VM with 3D acceleration switched on, and the simulation
has the screen change forty times with keys. It is done on top of a
snapshot which is given up afterwards.

- The Guest Additions install in 16 seconds, and the graphics adapter
  goes from the basic driver of Windows to the one of the Guest
  Additions.
- Started with 3D acceleration, that driver tells the host that it
  runs with it (in the log of the VM: 3D, VGPU10), the host puts the
  screen out by the accelerated way and counts 266 pictures brought to
  the screen that way in four minutes. The picture kept at the end is
  the sign-in screen of Windows, drawn as it should be.
- The VM and the guest run on through it, and Windows shuts down when
  told to.

It is Windows Server 2025 without the desktop, whose sign-in screen is
most of what there is to draw; a desktop with programs which draw
themselves is more than this.

What this is not: a program in a guest which draws in 3D, or the
driver of a graphics card. It is the way from the driver of a guest
through the adapter to a renderer, with a Linux guest which writes
text and with Windows which shows its sign-in screen.

What is to come of it: Windows with 3D acceleration with the whole
build under the sanitizers (see "VMs with the sanitizers"), and a
Windows with a desktop and a program which draws, which is what the
reports in the tracker are about.

### VMs with the sanitizers, from 2026-11-23

The testcase of the passthrough device has run with the sanitizers of
GCC from the start, against a stand-in for the kernel. Since run 305
the workflow can build the whole tree with them
(`VBOX_WITH_GCC_SANITIZER=1` among the arguments for kmk, the switch
the tree has for it) and run the simulation with that build: VBoxSVC,
VBoxManage and the VM process with every device in it, with a real
kernel under them and a guest which runs. What the address sanitizer
finds ends the program at that place and is written to a file.

Run 305 (2026-11-23, `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a`,
the part on the runner, built with 3D acceleration too, 37 minutes,
Intel Xeon 6975P-C):

- The tree builds that way, in 22 minutes, and its programs run: the
  VMs boot with KVM, and a phase takes about twice its time.
- One finding, six times, the same in VBoxSVC and in each of the five
  VM processes: the event queue Main has for XPCOM frees an event as
  another type than it was allocated as (`NativeEventQueue.cpp`,
  `plEventDestructor`), with the size of the smaller type, 120 bytes
  for an object of 128. It is code of upstream which has nothing to do
  with a guest, and the allocator of the C library takes no notice of
  the size. The sanitizer ends the program at it, which made one check
  fail: the VM which had saved its state was gone before that was
  told.
- Nothing else. No report about a device, about the VMM, about the
  passthrough code as far as the part on the runner runs it (the empty
  slot), or about the graphics adapter with 3D acceleration and the
  renderer under it, whose phase passed. No report of undefined
  behaviour at all.
- 35 checks pass, 2 fail: the saved state, and the check that the
  address sanitizer has nothing to say.

The finding is put right on a branch of its own,
`fix/event-queue-delete-type`, one commit on `main` of upstream, which
is merge request #7 of the fork. Run 307 (the same day, 22 minutes,
AMD EPYC 9V74) is that branch with the sanitizers: the development
line builds that way too, in 17 minutes, all 20 checks pass, and with
this kind of finding left switched on the address sanitizer has
nothing to say, of VBoxSVC or of a VM process. For a tree which has
the defect the simulation tells the sanitizer to let this one kind of
finding pass, so that the programs get further.

**The parts with a passed through device, with the sanitizers** (run
309, the same day, `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a`, the
parts on the runner, with the drive and with the device of four
gigabytes, 61 minutes, AMD EPYC 7763). The settings of the sanitizers
go into the machine which stands in for a host, where VirtualBox has a
kernel with an IOMMU under it and a device to pass on. With the drive:
passed through, read and written, with its interrupts, after pause and
resume; taken out of the slot with the guest asked and by force and
put in again; a start without it; the host asking for it back; a
guest which uses INTx. With the device of four gigabytes: its memory,
its ROM, the scripts for a machine with real hardware. The part with
the drive takes 24 minutes that way, 17 without.

- The sanitizers wrote nothing, in any part: no report about the
  passthrough device, the bus, Main or the VMM, with the one kind of
  finding of run 305 let pass, and none of undefined behaviour.
- 135 checks pass and 2 fail, neither for something a sanitizer found.
  The build which is fetched was started with the libraries of the
  build of the run and did not run, which is put right in the scripts.
  And the first test, run once more by the script for all which takes
  root, counted twelve lines of the device in the log of its VM which
  tell of something that did not work, after it had passed with no
  such line earlier in the same part. Which lines is not known: the
  result had the first sixty lines of the log and not those. A test
  writes the lines it counted into its result since. Run 310 repeated
  the part the same way, and the test passed both times with no such
  line: what the twelve lines were is not explained, and the kernel of
  the machine standing in for the host has nothing about the device
  from that time but resets which were done.

Run 310 also had the build which is fetched run, and found the check
which keeps names out of a result at work in the wrong place: the
build with 3D acceleration says of itself with which options it was
configured, one of them a path of the runner which built it, and on a
runner that path has the name of the user in it. The script which
fetches leaves that option out of its result now, and so does a
package when it is made.

**Windows with 3D acceleration, with the sanitizers** (run 308, the
same day, `scratch/intx-7.2-kvm` @ `26e4045a`, the parts on the runner
with the Linux guest and with Windows, 99 minutes of which 58 were the
runner fetching the packages of a kernel, Intel Xeon 6975P-C). All 88
checks pass. Windows Server 2025 is installed in 351 seconds, started
again, given a TPM, saved and loaded, gets the Guest Additions, is
started with 3D acceleration and has its screen change forty times,
all of it with the sanitizers in VBoxSVC and in the VM process, the
graphics adapter and the renderer under it included. The sanitizers
wrote nothing.

**The development line, with the sanitizers** (run 314, the same day,
`pci-passthrough-hotplug-v1` @ `4add131b`, the parts on the runner,
with the drive and with the device of four gigabytes, 46 minutes, AMD
EPYC 9V45). The branch of the first merge request has the KVM backend
of upstream under the same passthrough device. The sanitizers wrote
nothing there either, in any part. 130 checks pass and 1 fails, which
was a defect of the scripts for a machine with real hardware: a test
ended the service of VirtualBox and did not wait for it to be gone,
and the test after it, started at once by the script which runs them
all, talked to the service which was ending and did not get the VM of
the test before cleared away ("Machine settings file ... already
exists"). The simulation waits between two tests, which is why only
the script for all of them met it. The twelve lines of run 309 were
something else, which run 313 found out, see below. A test waits for the service to be
gone now, at its end and at its start, starts with a VirtualBox which
knows of no VM, and ends only the service which was started from its
own build, not one of a VirtualBox installed on the machine.

**The device of two functions and the VGA adapter, with the
sanitizers** (run 313, the same day, `pci-passthrough-hotplug-7.2-kvm`
@ `26e4045a`, 52 minutes, AMD EPYC 7763). The sanitizers wrote nothing
in either part: two functions passed through, taken out and put in as
one, a start without them, the host asking for both back, the reset of
their bus; the VGA adapter with its ROM and its legacy ranges. 127
checks pass and 1 fails, and that one explains the twelve lines of
run 309, as the lines are in the result now: the first test counted
three lines of the device at its end,

    Failed to passthrough PCI config read at offset 0xc with VERR_SEEK_ON_DEVICE, returning 0

and the same for 0xd and 0xf, 70 milliseconds after the first function
was taken out of service in a removal by force. What they are: the
device lets go of a function at once, its file descriptor taken by one
which refuses everything, and has an EMT do the rest a moment later;
until then the function is still there for the guest. A guest which
reads registers of the function in that moment gets the ones the
device passes on to the hardware from the stand-in, which refuses
those reads. The three offsets fit one read of four bytes at 0xc, of
which the byte at 0xe is not passed on. Nothing which is not there is
reached and the reader is given zero; the line says "Failed". It takes
a guest which reads at that moment, as the firmware of a VM without an
operating system does for a while after the start, longer where all
is slower, which is why the sanitizers brought it out.

(Corrected the same day. This said that the PCI bus reads these
registers when the EMT switches the decoding of the function off. The
code of the bus reads none of the three; the reader is the guest.)

Two things follow. The test counts lines of this kind by themselves
and not as something which did not work. And the device should answer
such a read from what it knows of the function instead of asking a
device it has let go of, and say nothing of it: a change to the device
on all its branches, with a test. It is written and on every branch
since 2026-11-24, see "The device does not ask a function which is out
of service" below.

**The scripts once more, after their correction** (run 315, the same
day, `scratch/intx-7.2-kvm` @ `26e4045a`, the part with the device of
four gigabytes, without the sanitizers, 34 minutes, Intel Xeon
6975P-C). All 50 checks pass. The script which runs every test ran
them one after the other, 15 and 14 checks, and the second one found a
VirtualBox which knows of no VM. No line of the kind run 313 found was
written in this run, so the count of such lines was not tried by it.

**The device does not ask a function which is out of service** (the
same day). The change which follows from run 313. A register which
the device passes on to the hardware, and the status register, are
read from the hardware while the function is in service and at no
other time. From the moment it is out of service until the EMT has
made it go away for the guest, such a read is answered from what is
kept of the register for the PCI bus, like the registers the device
never asks the hardware for; after that everything reads as ones, as
before. A read which is under way when the function goes out of
service fails at the stand-in and is answered the same way. No line is
written for either, and none for a write which is under way then; a
write after that was dropped before already.

What a guest has of it: the three registers of run 313 read as zero in
that moment as they did, going by the code, which keeps nothing of
them (no test reads their values then), and the status register reads
as what is kept of it, with its bit for the capability list, where it
read as zero. What the log has of it: nothing.

The test. The fake kernel of the testcase counts the reads and writes
at an offset which the real kernel refuses because the file has no
offsets, which is what becomes of a read of the stand-in, and it can
call the test before a read of the configuration space is done. The
test of a device which is taken back has two new parts. In the first
the guest reads all 256 bytes of the configuration space after the
device has let go of the function and before the EMT has done its
part: the IDs read as before, the status register has its bit for the
capability list, and nobody has asked the stand-in. In the second the
plug is pulled from inside the read of the status register, at the
point where the device is about to ask: that read is the one which
reaches the stand-in, the guest gets the status register as kept, and
the same read again gets the same without anybody being asked.

The runs. On the development line the testcase passes with the
sanitizers, its 57 tests and the three random tests in full length, no
report (run 316, `scratch/cfg-gone` @ `432ae816`). On the 7.2 line
with the KVM backend the testcase passes and so does the whole
simulation with a Linux guest, all 211 checks: the runner, the drive,
the device of two functions, the device of four gigabytes and the VGA
adapter (run 317, `scratch/cfg-gone-7.2-kvm` @ `585a8166`). On the 7.1
line with the KVM backend the testcase passes (run 318,
`scratch/cfg-gone-7.1-kvm` @ `656e0e59`); the simulation was not run
again there, the change being the same lines. Two deliberate defects
in the change, each on a branch of its own. Without the second look at
the function after a read which failed, the part which pulls the plug
from inside a read fails and nothing else does: the status register
reads as zero in that read and as kept in the next (run 319).
With the device asked whether the function is in service or not, both
parts fail and nothing else does: the read of the whole configuration
space counts 65 reads which reach the stand-in, and the part with the
plug pulled counts two where one was under way (run 320).

The branches of the merge requests have the change since 2026-11-24,
each by a push which only adds to what was there:
`pci-passthrough-hotplug-v1` @ `432ae816`, `-7.2` @ `5edbdf1a`,
`-7.2-kvm` @ `585a8166`, `-7.1` @ `9cd83b17` and `-7.1-kvm` @
`656e0e59`. The build which a machine with real hardware fetches has
it since run 321, see "The build to fetch" under "Machines with real
hardware".

What no run can show: that the lines of run 313 are gone from a VM.
They came in two runs on one kind of runner and in none on another
(runs 309 and 313, and 310), so a run without them proves nothing. The
testcase is what shows it, with the device asked for nothing where
it wrote those lines.

**Windows with a device in the slot, with the sanitizers** (run 323,
2026-11-24, `scratch/cfg-gone-7.2-kvm` @ `585a8166`, which is the head
of the merge request for 7.2 with the KVM backend; the parts on the
runner, Windows on the runner and Windows in the machine standing in
for a host; 62 minutes, Intel Xeon 6975P-C). What was missing here:
Windows Server 2025 in a VM whose VBoxSVC and VM process are built
with both sanitizers, with the drive in the hot-plug capable slot. All
110 checks pass, the 32 with Windows and the drive among them: Windows
starts with the drive and reads from it, lets go of it when asked,
takes it when it is put in, loses it by force and takes it again, runs
its controller with a line interrupt, and shuts down. The sanitizers
wrote nothing in any part, no file at all. Windows took 432 seconds
to its first report there, which is what it takes without the
sanitizers as well (409 to 521 seconds in five earlier runs).

Not done this way: Windows with the device of two functions and with
the one whose second function is an audio controller.

### An error of the device, in the simulation (2026-11-24)

What VirtualBox does when the host reports an error of a device it has
passed on was tried by the testcase only, against the kernel the
testcase makes up; "Next" had the same in the simulation as not
planned. It is there now, as a part of its own (`errhost`, a machine
of its own, so that a kernel which does not get over an error costs
no other check): a Linux guest uses the drive in the hot-plug capable
slot, and the port the drive is behind reports an error it cannot
correct. The kernel of the machine which stands in for the host takes
that up as it does with real hardware: its AER driver reads the
registers of the port, tells the drivers of all which is behind the
port, and for an error it takes for fatal resets the bus. The driver
which has the drive for VirtualBox (`vfio-pci`) tells VirtualBox, and
VirtualBox takes the device out of service and tells the guest that
the slot is empty.

The error has to come from somewhere, and it is made up. The first
try was the means the kernel has for trying this out, the module
`aer_inject`; the kernel of the runners does not have it (**run 326**,
where the phase was skipped). So it comes from outside that machine:
the emulator which is the machine has a command for making a port
report an error, the machine gets a socket for commands which nothing
inside it can reach, and a script on the runner (`sim/host-agent.sh`)
gives that one command when the script inside the machine asks for it
through the directory the two share. It takes no other request. The
port then does what hardware does: it sets its status and raises its
interrupt.

The phase (`hosterror`), twice, with an error which by the
specification is not fatal (Completer Abort) and with one which is
(Malformed TLP): the port is made to report it; the kernel has it in
its log; the log of the VM says "The function is taken out of service:
the host reported an error of the device"; the guest finds its slot
empty and its drive gone; the VM runs on; VirtualBox tells the slot is
empty; the host still has the device; `plugpci` puts it back; the
guest takes it and reads from the drive.

**Run 327** (2026-11-24, `sim` with that part only, for finding out,
`pci-passthrough-hotplug-7.2-kvm` @ `585a8166`, 18 minutes, Intel Xeon
Platinum 8573C): all 20 checks of the phase pass, both errors.
VirtualBox had the device out of service in the second the kernel
logged the error. After the error which is not fatal the kernel says
"device recovery successful". After the fatal one it reset the bus
behind the port, waited for a link which that emulated port never
tells of ("Data Link Layer Link Active not set"), and said "device
recovery failed": the port of the machine again, as in run 265, and no
finding about VirtualBox, which did the same as before, and the device
could be put back all the same. A host whose recovery fails is a case
of its own, and this run is what there is of it.

**Run 328** (the same day, the same part and branch, 17 minutes, Intel
Xeon 6975P-C): the machine of this part has the port which does not
claim to tell of its link, as the parts with other devices have it.
All 20 checks pass again, and after the fatal error the kernel says
"Root Port link has been reset" and "device recovery successful": its
reset of the bus got through, with the device already let go of by
VirtualBox. The part is among those a run of the simulation has by
default since.

**The same with a device of two functions** (a part of its own,
`mferrhost`, with the machine of the part about such a device; **run
329**, the same day, the two parts with an error, the same branch, 19
minutes, Intel Xeon 6975P-C). A graphics card with its audio function
is such a device, and the kernel tells the drivers of both functions
when the port in front of them reports an error, so VirtualBox is told
twice of what is one event. All 22 checks of the phase pass with it,
and the 20 with the drive again, 89 in the run: both functions are
taken out of service, each once and within a fraction of a second of
each other, the guest finds both drives gone, the VM runs on, and
`plugpci` puts the device back with both drives read from again; after
the fatal error the kernel's reset of the bus gets through as well.
This part is a default one too.

**The other two lines** (runs 330 and 331, the same day, the two
parts with an error): on the branch of the merge request,
`pci-passthrough-hotplug-v1` @ `432ae816`, which has the KVM backend
of upstream (15 minutes, AMD EPYC 9V45), and on the branch for 7.1
with the KVM backend, `pci-passthrough-hotplug-7.1-kvm` @ `656e0e59`
(21 minutes, AMD EPYC 7763). All 89 checks pass on both, the 20 and
the 22 of the phase among them, and the kernel's recovery succeeds
after each error. With that every branch of a merge request about the
passthrough work has been through it.

**With Windows as the guest** (the three parts with Windows in the
machine standing in for a host end with the same two errors now;
**run 336**, 2026-11-24, `winsim`, `scratch/cfg-gone-7.2-kvm` @
`585a8166`, which is the head of the merge request for 7.2 with the
KVM backend, 61 minutes, Intel Xeon 6975P-C). All 258 checks pass and
one is left out as before (this Windows has no driver for the audio
controller). With the drive, with the device of two functions and
with the one whose second function is an audio controller: VirtualBox
takes the device out of service in the second the kernel logs the
error, Windows finds the drive gone, and the second function with it,
and runs on; `plugpci` puts the device back, and the driver of Windows
has it and reads from the drive again. 16 to 20 checks for each
device. In the machine with the drive the kernel of the host calls its
recovery failed after the fatal error, for what its port does not tell
(as in run 327); in the other two it succeeds. The run before it which
was meant for this, run 332, had a runner on which Windows does not
get through its start, and two more were cancelled for that at once.

What this does not show: an error of a real device, which may leave
the device in a state no reset gets it out of; and a device which is
pulled without the host being told first, which the emulated port
cannot do.

### The device leaves the host and comes back, in the simulation (2026-11-24)

A device in an enclosure on a cable is not there all the time. So far
the simulation had the host take the device back by a word to its
kernel and have it again by looking for it; the device never left the
machine. With the script on the runner which does things to that
machine from outside (see the section before this one) it can. A part
of its own, `plughost`: its machine has its kernel run the slot of the
port itself, as a kernel does for a port an enclosure connects to (the
firmware of the emulated machine keeps that to itself by default), and
a drive which outlives the device which is taken away.

The phase (`hostplug`): a Linux guest uses the drive in the hot-plug
capable slot of the VM. The device is unplugged from the host: the
port is told so from the runner, the kernel of the host announces that
it will switch the slot off and asks the driver which has the device
to let go of it. That driver asks VirtualBox, VirtualBox asks the
guest, the guest lets go, and the host has no such device any more.
`plugpci` of the device is refused then, and the VM runs on. The
device is plugged in again: the kernel of the host finds a card in the
slot and a new device at the address, with no driver; it is bound to
`vfio-pci`, which INSTALL.md says is to be done each time a device was
plugged in; `plugpci` puts it into the VM, and the guest reads from
the drive.

**Run 333** (2026-11-24, `sim` with that part only, for finding out,
`pci-passthrough-hotplug-7.2-kvm` @ `585a8166`, 14 minutes, AMD EPYC
9V45): all 16 checks of the phase pass at its first run, 55 in the
run. The device was gone from the host 13 seconds after the port was
told; five of them the kernel of the host waits after its own
announcement, and five the guest does the same in its VM. The log of
the VM has "The device is not reset now, the host is busy with it and
refuses. The host resets it when it is let go of", which is how it is
meant to go when it is the host which takes the device. The part is
one a run has by default since.

**The same with a device of two functions** (a part of its own,
`mfplughost`; **run 337**, the same day, the two parts in which the
device is unplugged, the same branch, 19 minutes, Intel Xeon 6975P-C).
All 16 checks of the phase pass with it, and the 16 with the drive
again, 79 in the run. The kernel of the host asks the driver of each
function to let go, the second function six seconds before the first;
VirtualBox asks the guest once and takes both functions out of service
in the same moment; both are gone from the host 13 seconds after the
port was told. Plugged in again, second function first as the emulator
wants it, both are there within a second, are bound and put into the
VM, and the guest reads from both drives. The phase tells rightly now
that a device which came back has no driver. This part is a default
one too.

**All parts in one run** (**run 338**, the same day,
`pci-passthrough-hotplug-7.2-kvm` @ `585a8166`, the simulation as it
is by default now, with its nine parts, 72 minutes, Intel Xeon
6975P-C). All 315 checks pass, none is left out, and the testcase of
the device passes in the same run. The four parts of this day, the two
with an error and the two with the device unplugged, go through with
the five there were.

**With Windows as the guest, and what the emulated port does not do**
(**run 340**, the same day, `winsim` with the device of two functions,
36 minutes, Intel Xeon 6975P-C; 147 checks pass and 1 fails). The part
with Windows and the device of two functions ended with the device
being unplugged from the host. That went as it should: the host lost
both functions 15 seconds after the port was told, Windows found the
drive and the second function gone and ran on, and `plugpci` was
refused. Plugged in again the device did not come back to the host,
whose kernel said "Cannot train link: status 0x2000".

**Run 341** (the same day, the two parts with the Linux guest, each
going through the unplugging, an error of the device and the
unplugging once more, and telling what the port says of its link at
each step; 32 minutes, Intel Xeon 6975P-C; 141 pass, 2 fail) shows
what that is. The port has link status 2011 while nothing has happened
to it, and the device comes back when it is plugged in. After the
phase with the two errors the port has link status 2000: the speed and
the width of its link are gone, and stay gone. Unplugging still works.
Plugged in again the device is not taken, with the same words as in
run 340, in both parts. So it is the emulator: its port does not keep
the width of its link over what happens to it in the phase with the
errors, and the kernel of the host takes no device from a port whose
link has no width. In run 340 the errors had come before the
unplugging. A port of a real host keeps its link, and this is no
finding about VirtualBox; what it means for the simulation is that a
device cannot be plugged in again after such an error. The phases look
at the port before they try, and leave the plugging in out, saying
why, where the port has no width; with Windows the unplugging and
plugging in comes before the error where the port still has its link.

This paragraph said until run 343 that any reset of the bus behind the
port does it, and until run 345 that it is the reset after the error
which the kernel takes for fatal. Neither is so: no reset has to do
with it, see runs 343 and 345 below.

**Run 342** (the same day, the same two parts with the phases as they
are after run 341, 29 minutes, Intel Xeon 6975P-C): 139 checks pass,
none fails, and 2 are left out with their reason. In both parts the
device is unplugged and plugged in again, the two errors are gone
through, and the device is unplugged once more, which passes; it is
not plugged in again then, the port having link status 2000.

**Windows as the guest, with the order turned round** (**run 343**,
the same day, `winsim` with the two parts in which the device can be
unplugged, the one with the drive and the one with the device of two
functions, 49 minutes, Intel Xeon 6975P-C). All 223 checks pass and
none is left out. In both parts the port had link status 2011 when its
turn came, so the unplugging came before the errors. The host lost the
device 13 and 14 seconds after the port was told; Windows found the
drive gone, and the second function with it, and ran on; `plugpci` of
the device which is not on the host was refused. Plugged in again the
device was on the host within a second, with no driver, was bound to
`vfio-pci`, `plugpci` put it into the VM, and the driver of Windows
took it and read from the drive: 13 checks with the drive and 15 with
the device of two functions. The two errors came after that and went
as in run 336. In the part with two functions the device had been
reset by a reset of its bus three times by then, by the log of the VM,
and the port had its link all the same.

**The other two lines** (**run 344**, the same day, the two parts with
the Linux guest on the development line, `pci-passthrough-hotplug-v1`
@ `432ae816`, 27 minutes, AMD EPYC 9V74; **run 345**, the same on
`pci-passthrough-hotplug-7.1-kvm` @ `656e0e59`, 29 minutes, Intel Xeon
6975P-C). In each 139 checks pass, none fails, and 2 are left out with
their reason, as in run 342. The device leaving the host and coming
back has passed on all three lines with that.

**What takes the width of the link** (run 345, and the source of the
emulator). Run 345 is the first in which the port is looked at before
and after each error. It has link status 2011 before the first error,
the one the kernel does not take for fatal and resets nothing for, and
2000 after it. So it is no reset, and it is not VirtualBox, which is
told of the error and does nothing to a port. It is how the emulator
makes a port report an error which cannot be corrected. Read in its
source, QEMU 8.2.2, which is what the runner has: the function which
does it (`pcie_aer_inject_uncor_error` in `hw/pci/pcie_aer.c`) writes
the device status register of the port, 16 bits, with 32. The 16 bits
after it are the lower half of the link capabilities, where the
highest speed and width of the port are, and they are zero from then
on. A port of the emulator tells of its link, each time it is asked,
as what the device behind it tells, limited to its own capabilities
(`pcie_sync_bridge_lnk` in `hw/pci/pcie.c`): no speed and no width,
with whatever device. The current source of the emulator writes 16
bits. And it is seen, since the phases show the link capabilities of
the port (run 347): 00300604 for the root port before the first error,
with the speed and the width in the lower half, and 00300000 after it,
the link status going from 2011 to 2000 with that.

**A part with a switch** (`swplughost`, 2026-11-25). The machine has a
root port, the two ports of a switch behind it, and the drive in the
slot of the lower one, which is how a device in an enclosure on a
cable is connected: behind the ports of the switch the cable ends in.
It was written so that the drive could be plugged in again after an
error, the error being reported by the root port and the slot being at
another port.

**Run 346** (2026-11-25, `sim` with that part only, for finding out,
`pci-passthrough-hotplug-7.2-kvm` @ `585a8166`, 17 minutes, Intel Xeon
6975P-C; 47 checks pass, 3 fail, 1 is left out) shows what works and
what does not. The drive behind the switch, at the host address
`0000:03:00.0`, is passed through as any other: the VM starts with it
and the guest reads from it. It is unplugged from the port of the
switch as it is from a root port, gone from the host 12 seconds after
the port was told, the guest having been asked. What does not work is
the plugging in: the downstream port of the switch this emulator has
tells of link capabilities without a speed and a width (00000400) and
so of a link without a width, from the start, with nothing done to it.
The phase saw that and left the plugging in out, and the two phases
after it had no device, which is two of the three checks that fail.
So this part cannot show a device which comes back, after an error or
before one. It goes through the errors first and the unplugging last
since: an error which the root port reports, with a switch between it
and the device, and the reset of all of that after the error which the
kernel takes for fatal.

**Run 347** (the same day, the part with the errors first, 25 minutes,
Intel Xeon 6975P-C; 63 checks pass, 3 fail, 1 is left out). The error
phase passes with the switch, all of it. The root port reports the
error, the kernel tells the driver of the drive two ports below, and
VirtualBox takes the drive out of service in the same second; the
guest finds its slot empty, the VM runs on, `plugpci` puts the drive
back and the guest reads from it. With the error the kernel takes for
fatal it resets everything below the root port, the switch with it,
calls its recovery successful, and the drive is put back and read from
after that as well. The checklist script finds nothing missing and has
its line about the port.

The three which fail are the unplugging after that and what follows
from it: the port of the switch is told that the drive is to go, and
the kernel of the host does nothing, for three minutes. That port has
slot status 0050 since the reset, a completed command pending which
nobody took note of, where it had 0040 before. So it is the host, the
kernel and the emulated port between them, and not VirtualBox, which
is not asked anything. What is taken to be behind it is with the
function `port_take_note` in `sim/vbox-phases.sh`; the phase clears
that bit before it unplugs since, and says so where it does.

**Run 348** (the same day, 19 minutes, Intel Xeon 6975P-C) has the
part pass: all 68 checks of the run, with the plugging in left out as
it has to be. Before the unplugging the phase found slot status 0050
at the port of the switch, with the slot control as the kernel has it
(03f1, the interrupt of the slot on), cleared the bit, and the kernel
heard of the request: the drive was gone from the host 12 seconds
after the port was told, the guest having been asked and having let
go. The part has 37 checks: 7 for its setup, 20 for the two errors, 10
for the unplugging, the one which is left out among them, so 36 which
pass. It is one a run of the simulation has by default since. What it adds to the others: a device which is not behind a root
port but behind a switch, at a bus two below; an error which is
reported by another port than the one the device is in; and the reset
of a switch with the device behind it.

**The other two lines, and all parts in one run.** The part with the
switch passes on the development line (**run 350**,
`pci-passthrough-hotplug-v1` @ `432ae816`, 19 minutes, Intel Xeon
6975P-C) and on the 7.1 line with the KVM backend (**run 352**, @
`68c5b765`, 22 minutes, AMD EPYC 7763), with the same 68 checks and
the same one left out as in run 348. **Run 349** (`sim` as it is by
default now, with its ten parts, `pci-passthrough-hotplug-7.2-kvm` @
`585a8166`, 84 minutes, Intel Xeon 6975P-C) is the first run of all of them
since the phases the parts share were changed for the part with the
switch: all 411 checks pass, and 3 are left out with their
reason, the plugging in after the errors in the two parts without a
switch and the plugging in in the part with one.

The third check which failed in run 346 is one of the checklist script of
INSTALL.md, and it was the script which was wrong. The port of this
switch has no access control services, so the kernel has the port and
the drive in one IOMMU group, and the script called that a group with
other devices in it which would be handed over. A port the device is
behind is not handed over: it keeps the driver of the kernel, which
permits a group like that, and VirtualBox had the drive. The script
tells of such a port in a line of its own now and counts nothing as
missing for it; another device in the group is what it was. Item D2
of INSTALL.md says the same.

What this does not show: a device which is gone without the host
having been asked, as when a cable is pulled; with the emulated port
the host is asked first. And a device which is plugged in again after
an error, for the reasons above: no port of this emulator takes one
then. That is left for the hardware, see HWTEST.md, part H.

### A check of the testcase which came too early (2026-11-25)

**Run 351** (2026-11-25, the part with the switch on
`pci-passthrough-hotplug-7.1-kvm` @ `656e0e59`, 10 minutes, Intel Xeon
6975P-C) failed before it got to the simulation. The testcase of the
device, `tstDevPciVfio`, had one of its tests fail, "Hot-plug slot: a
state is loaded while the host waits for the device", with "line 7178:
slot status 0x150". The workflow runs a testcase which failed once
more in the debugger, and there it passed. The same commit had passed
it in every run before, run 345 the last.

What it is: when the host takes a device back, the thread of the
device which watches out for that lets go of the functions first and
tells the port of the slot right after, in that order so that nothing
of the device can be reached by the time the guest hears of an empty
slot. For a moment the host has the device and the slot still tells of
a card; slot status 0x150 is a card which is there, a link which has
changed and a command which was completed. Five places of the testcase
waited for the host to have the device and looked at the slot at once,
and this one looked in that moment. The testcase was wrong to expect
otherwise. A guest which looks in that moment sees a slot it has
switched off itself with the card still in it, and is told of the card
going right after. The device is as it was, and the order stays.

The testcase waits for the slot at these five places now
(`tstSlotWaitForEmpty`, five seconds at most): `20438da2` on the
development line, `53ca3518` on the 7.1 line, merged into the 7.1 line
with the KVM backend (`68c5b765`). **Run 352** (see "The other two
lines" above) has the testcase pass on that head and the simulation
after it; **run 353** (`tests`, `pci-passthrough-hotplug-v1` @
`20438da2`, 10 minutes, AMD EPYC 7763) has it pass on the development
line, and **run 355** (`sanitize` on the same head, 11 minutes, Intel
Xeon 6975P-C) with the sanitizers, which have nothing to report, and
with the three random tests in full length. A test which fails once in
some hundreds of runs is not shown
gone by two which pass; what is shown is that the change builds and
that the tests pass with it. The 7.2 line has the same commit since
run 349 was done, which ran on the head before it (`7af415e3`), and
so has the 7.2 line with the KVM backend, by a merge (`05f1d33a`);
**run 354** (`tests` on that head, 11 minutes, AMD EPYC 9V74) has the
testcase pass there. The heads of the five branches are
`pci-passthrough-hotplug-v1` @ `20438da2`, `-7.2` @ `7af415e3`,
`-7.2-kvm` @ `05f1d33a`, `-7.1` @ `53ca3518` and `-7.1-kvm` @
`68c5b765` with that. The two lines without the KVM backend had no
run of their own for it: their commit is what the lines with the
backend have, which were run. The build for a machine with real
hardware stayed the one of `585a8166` for that: the head after it
differs in the testcase only. (It is the one of run 362 since the same
day, for another change.)

### A second reader for four later commits (2026-11-25)

The decision of 2026-11-13 has unbuilt code read by somebody who did
not write it, and the one of 2026-11-22 had the INTx change read that
way before it went to the branches. Four commits written in between
and after had no such reader: `dd43ddf3` (a device on its way out is
not reported as failing), `432ae816` (a function which is out of
service is not asked for its config space), `20fd927d` (TM, an EMT
which missed the notification of the timer) and `f563e7f8` (I/O APIC,
level-triggered interrupts after a saved state was loaded with KVM).
Two readers got them on 2026-11-25, the two about the device and the
two others, with the code and without what the writer thinks of it.
Every point was checked against the code before anything was changed.

**What was wrong, and is changed.** When a function goes out of
service, an EMT clears the decode bits of its command register, which
removes the guest mappings of the BARs, and makes the function go away
for the guest (`pciVfioFunGoneOnEmt`). It did that with the lock of the
slot and without the PCI lock, which an EMT holds while it is in a
write of the guest to the config space. Such a write can be past the
check for a function which is out of service when the function goes,
and it changes the command register and the mappings too. So two EMTs
could be at the mappings of a function at the same time, and a write
which enables decoding could come after the other EMT had looked at
the command register: the memory which stands in for the BARs of a
function that is gone stayed mapped then, until a reset of the VM. The
code is older than the two commits which were read; it is where the
first of them says "while this was on its way". A device in the
hot-plug capable slot was not affected, as the slot is emptied with
its lock held and its link is down before an EMT gets there; a device
which is in the VM for good was. It takes the host taking the device
back, or an error of the device, in the same moment as a write of the
guest on another CPU. `pciVfioFunGoneOnEmt` takes the PCI lock now,
after the lock of the slot, the order the device has everywhere else.

A new test (`tstRemovalWhileWriting`, 58 tests) has a write of the
guest on its way to the device when the host takes the device back,
once to the command register and once to a register which is passed
through, and another thread playing a second EMT which gets around to
what is left to do: it has to wait for the write, and afterwards the
function is gone for the guest with nothing of it mapped.

**What was said and is not so, and is said differently now.**

- The two commits about the device kept three kinds of access from
  being reported as failing when the function went out of service in
  that moment. Others still were: the interrupt setup ("expect a
  broken device"), a write to the MSI or MSI-X control register, and
  an access to a BAR or to the legacy VGA ranges through the device
  file. They look at the function first now. Three of these lines had
  no limit and have one of 64 now.
- The log said of a reset which the host refuses because it is busy
  with the device: "The host resets it when it is let go of". Nothing
  in the device knows that, a host is busy with a device for other
  reasons than waiting for it, and the reader recalls the kernel not
  resetting a device which it cannot lock when it is let go of either.
  The line ends "It keeps its state apart from being disabled" now.
  Not changed: the reset is not tried a second time. What holds the
  lock of a device for a moment is rare, and what holds it longest
  besides the unbinding is the kernel recovering from an error of the
  device, which resets it anyway.
- "What is kept of the register" which the guest reads of a function
  on its way out is nothing for a register which is passed through:
  most read as zero, as they did when the read failed. The comment
  says so now.
- TM: the comment and the commit message named one way for
  `VMCPU_FF_TIMER` to be set without the EMT getting to know, a poke
  which comes too early. There is a second, and it is no race: with
  the KVM backend of upstream every EMT polls the timers before it
  enters the guest, and one which is not the EMT for the timers and
  finds a timer expired sets the flag and notifies nobody
  (`tmTimerPollInternal`). Before `20fd927d` nothing served that until
  the EMT left the guest for another reason; since then the
  notification of that commit does, one to two ticks (10 to 20
  milliseconds) after the flag was set, where a timer is served within
  one tick otherwise. The backend of Cyberus Technology, which the
  branches for use have, does not poll that way and has a timer of its
  own for the next deadline. The comment and the log line are
  corrected. Not changed: notifying at the first tick already would
  halve that delay on the backend of upstream; it is a change to the
  timer code which wants runs of its own with guests of several CPUs,
  and is noted under "Next".
- I/O APIC: the comment said the interrupt was the last one "until the
  guest writes the entry anew". A write of the guest keeps the remote
  IRR bit; an EOI or a reset clears it. And a state which was saved
  from a VM that already had the bit stuck, by a build before
  `f563e7f8`, loads with the bit stuck: the load cannot tell it from an
  interrupt in service. Such a VM gets the pin back with a reset. The
  comment says both now.

**What the readers found in order**, among other things: the order in
which a function goes out of service (the flag before the descriptor
is replaced, both under the lock of the slot), that the stand-in never
answers the way a busy host does, that nothing uninitialised or out of
bounds is read for a function which is gone, the order of the locks,
that the saved state of the I/O APIC is unchanged in format and that
no interrupt is delivered twice by the load, that the new member of TM
has one writer, and that neither change reaches other hosts or other
execution engines.

**Not looked into**: a reader suspects that the EOI of a
level-triggered interrupt can get lost after a load with another
execution engine of upstream as well, where a table which says which
EOIs to pass on is not rebuilt. That is not this work and was not
checked.

**What the tests do not show**, as the readers say and as is so: no
test looks at the log, so the changes to what is logged can be taken
out with every test passing. A write to a register which is passed
through and which fails because the function goes in that moment is
not made to happen by a test: the fake kernel tells a test of a write
after it has taken it. The values the guest reads of registers which
are passed through while a function is on its way out are read and not
looked at, and the device of the tests has a config space of 256
bytes, so nothing beyond that is read in that state. Where the host
refuses a reset, the test passes whether the device has asked once or
not at all.

**Runs.** Run 358 (`scratch/red-evidence-19` @ `c365019f`, the new test
without the change to the device, expected to fail): the check the test
is about fails in both of its variants. Two more checks of the test as
it was then failed too, and wrongly: they expected one access to what
stands in for the device, and there is none, as the fake kernel tells of
a write after it has taken it. They are not in the test any more; the
test as it is was not run against the device without the change. Run
359 (`sanitize` on the head of the development line, `2e4e6786`): the 58
tests and the three random tests pass, and no sanitizer has a report.
Run 360 (`tests` on `pci-passthrough-hotplug-7.1-kvm` @ `625a36ed`):
they pass there.

The heads were, with that, `pci-passthrough-hotplug-v1` @ `2e4e6786` (three commits
more), `-7.2` @ `1bccc3ab`, `-7.2-kvm` @ `572356d1`, `-7.1` @ `296c0002`
and `-7.1-kvm` @ `625a36ed`. The 7.2 line has the comment in TM and not
the one in the I/O APIC, whose change it does not have; the 7.1 line has
neither of the two changes and so the two commits about the device
only. Not run: the development line without the sanitizers, and the two
lines without the KVM backend by themselves. The simulation on the new
head of the 7.2 line with the backend is run 361, see below. Run 362
(`package` with 3D acceleration, 12 minutes, AMD EPYC 9V45) was the
build of that head for a machine with real hardware, published, and the
one to fetch until the build of run 366 took its place the same day.

**A reader for what came of it, and a second thing which is older.**
The three commits above went to the branches without a reader of their
own, which is not how it is decided either; they got one the same day.
He found nothing wrong in what they do: no caller of
`pciVfioFunGoneOnEmt` holds the PCI lock already, the lock of the slot
is always taken before the PCI lock and never the other way round, what
is done with the PCI lock held is what a write of the guest to the
command register does with it, and the test fails without the lock and
has no way to hang. Three of his points are about texts and are a
commit of their own: the line about a reset which the host refuses
spoke of the device where the functions are reset one by one (it says
now that what was not reset keeps its state), the description of a
member of TM still had the old wording, and a comment in the I/O APIC
said more than the code of upstream keeps to.

Going through who waits for whom, he found this, which is older than
any of it. The state of a VM cannot be saved while a device is in the
slot, and the VM is told so with a runtime error. For a state which is
saved while the VM runs on, a live snapshot or teleporting, the thread
which saves finds that out, and such a thread has an EMT do the telling
and waits for it. `pciVfioSaveCheck` held the lock of the slot all the
while, and everything the device asks to be done on an EMT takes that
lock: an EMT which had taken up an INTx pulse in that moment, say,
waited for the lock, and with one CPU the VM stood still where it was
to say no. The lock is let go of before the VM is told now. A state
which is saved for good, with the VM suspended for it, was not
affected: that is refused on an EMT, which tells by itself. A fourth
reader read this change before it went to the branches: nothing wrong
in it, what it returns is what it returned, and no state can get saved
with a device in the slot because of it; a sentence each of a comment
and of the message were inexact and are corrected.

A new test (`tstSlotLiveStateRefused`, 59 tests) has the device raise
its INTx interrupt, so that there is work for an EMT, asks whether a
live state can be saved, and has a thread playing the EMT do that work
while the VM is being told: it has to get done. For this the stand-in
for PDM can call the test when a device raises a runtime error. Run 363
(`scratch/red-evidence-20` @ `5a8518a8`, the test without the change,
expected to fail): that one check fails, after the five seconds the
thread is given, and nothing else does. Run 364 (`sanitize` on the head
of the development line, `d85a8eed`): the 59 tests and the three random
tests pass, no sanitizer has a report.

Run 365 (`tests` on `pci-passthrough-hotplug-7.1-kvm` @ `4220a8d5`):
the 59 tests pass there. Run 361 (`sim` with its ten parts, strict,
`pci-passthrough-hotplug-7.2-kvm` @ `572356d1`, which is the head with
the PCI lock and before this last change, 103 minutes, Intel Xeon
6975P-C): 415 checks pass, none fails, and three are left out with
their reason, the plugging in after an error which no port of the
emulator takes. It is the first run of all ten parts with the third
script for a machine with real hardware in the four parts which run
those scripts.

The heads are `pci-passthrough-hotplug-v1` @ `d85a8eed` (44 commits),
`-7.2` @ `690cd60a`, `-7.2-kvm` @ `cc3dc981`, `-7.1` @ `35068a8e` and
`-7.1-kvm` @ `4220a8d5`. The 7.2 line has the text in TM and not the
one in the I/O APIC, the 7.1 line the line of the log only. Not run:
the development line without the sanitizers, and the two lines without
the KVM backend by themselves.

Run 366 (`package` with 3D acceleration on that head of the 7.2 line
with the backend, 16 minutes, AMD EPYC 9V74) is the build for a machine
with real hardware which has both changes; it is published, and
`hw/linux/fetch.sh` fetched it until the build of run 378 took its
place, see "Machines with real hardware". The testcase passes in that run. Run 367 (`sim` with the
parts runner, simhost and barhost, strict, the same head, 49 minutes,
AMD EPYC 9V74) is the simulation on that head: all 130 checks pass, and
it fetched that build as a machine with real hardware does, ran its
`VBoxManage`, and had the first of the scripts go through its 15 checks
with it.

The simulation had no snapshot for which the VM keeps running: the
one it asks for with the device in the VM has the VM paused first, and
that is refused on an EMT. Its part with the device in the slot asks
for a live one as well now, which has to be refused with nothing of it
left and the VM running on. Run 368 (`sim` with the part simhost, not
strict, `pci-passthrough-hotplug-7.2-kvm` @ `cc3dc981`, 39 minutes,
AMD EPYC 7763) is its first run: the live snapshot is refused, the VM
has no snapshot afterwards and runs on, and all 114 checks of the run
pass. The drive of the simulation uses MSI-X there, so no EMT has work
of the device in that moment; what the run shows is the way through
the VMM which the test has a stand-in for.

### Who waits for whom, in Main and in the root port (2026-11-25)

The two things the readers found on that day were of one kind:
somebody waits for somebody else with a lock held, or goes without a
lock where one is needed. The device had been gone through for that by
then, by the third reader. Main and the root port had not, so a reader
got them: every place on the way of `plugpci` and `unplugpci` which
waits, with what is held there, and every function of the slot with
its lock and its callers.

**What was wrong, and is changed.** `Console::i_releaseVMCaller()`,
which is code of upstream, counts down how many hold on to the VM, and
powering the VM down waits for that count to reach zero. It gave up
without counting where it could not become a caller of the Console
object, and nobody can from the moment `Console::uninit()` starts. So
whoever held on to the VM when the Console was uninitialised left the
count up, the VM was never powered off, and the VM process stayed for
good. `Console::unplugHostPCIDevice()` of this work holds on to the VM
for as long as the guest is given to let go of a device, which made a
moment of a minute: the frontend being told to end while a device is
being taken out is enough. `VBoxHeadless` then tries to save the state
of a running VM, which is refused with the device in the VM, and closes
the session. The count goes down now whatever the state of the object.
Another reader read that change before it went in and found nothing
wrong in it. It does not cover a power-up which failed while somebody
held on to the VM; that is upstream's as well, has nothing to do with a
device, and was left.

**What was found and left**, all in code of this work:

- When the guest switches the slot off, disables the link or resets
  the bus behind the port, the port tells the device with the PCI lock
  held, and the device resets the device of the host there, which can
  take a second or more with a reset of the bus. The PCI lock is the
  lock of all devices of the VM, so the VM waits for its own doing. No
  two threads wait for each other in it.
- A device is taken out without `--force` when the guest lets go of
  it, and that is told by the link of the slot being down. A guest
  which disables the link or resets the bus behind the port while it is
  asked has its device taken as well, without having switched the slot
  off. The slot is one from which a device can go at any time, and the
  guest was asked; it is not what the guest said, though.
- A VM which was started paused and never ran: `plugpci` is refused
  with a message about the state of the VM which does not fit, and
  `unplugpci` goes through.
- Two more commands for the same slot within the ten milliseconds
  between two looks of one which waits can have that one take a device
  which was put in after it was started.

**What was found in order**, among other things: no lock of Main is
held where `plugpci` or `unplugpci` wait; the VM cannot be destroyed
while a thread of the API is in the device, as powering down waits for
it, one piece of a quarter of a second at most; the port takes the PCI
lock only, the device its own lock first and that one second, and
nothing takes them the other way round; the registers of the slot
change with the PCI lock held; the slot has no timers; and nothing a
guest writes to the slot makes Main or the device wait without an end,
or has a command tell of something which did not happen.

**How it shows.** No testcase reaches Main, so the simulation has it.
A new phase of its part with the drive (`intxslot`) has a guest which
uses no MSI and the device in the hot-plug capable slot, which no
phase had: the interrupt of the drive is INTx and goes through the
port of the slot (the guest reads from the drive with it), and a live
snapshot is refused with the VM running on, which with INTx is where
a VM could stand still before the change of the same day. Such a
guest is not told of its slot, as the port tells by MSI only (decision
of 2026-11-12); the first run of the phase expected otherwise and had
four checks fail for that (run 369), and the phase goes by the
decision since: the command which asks the guest for the device says
after the time it was given that the guest did not let go, and the
device stays. That makes this guest the one which keeps a command
waiting, and at the end of the phase the VM process is told to end
while `unplugpci` waits for the guest. **Run 370** (`sim` with the
part simhost, exploring, `pci-passthrough-hotplug-7.2-kvm` @
`cc3dc981`, the head without the change to Main, 44 minutes, AMD EPYC
7763): the VM process is still there 90 seconds later, the one check
of 126 which fails.

**Run 371** (`sim` with the part simhost, the head with the change to
Main, `d9083189`, 37 minutes, Intel Xeon 6975P-C): the VM process ends
within a second, with the VM powered off in order, and all 126 checks
pass.

**A second thing, behind the first.** No check of run 371 looked at
what VirtualBox makes of the VM afterwards, and what the run showed by
the way was that the settings of the VM stayed locked: every command
which wanted the VM failed. The phase was given a second look for that,
whether the service knows a minute later that the VM is off, and has
the VM process told to end once more before, with nothing else going
on. **Run 374** (the same part, exploring, the same head, 41 minutes,
Intel Xeon 6975P-C): the VM process ends within a second both times,
and both times the service does not know; 129 checks pass and these
two fail. The service is not waiting for anything, as the places its
threads are in show. So it has nothing to do with a device being taken
out. What happens, read in the code of upstream: `Console::uninit()`
powers the VM down, the VM tells of its last states on another thread,
and the function which takes the last one, the VM being off, to the
service gives up there, because nobody but the thread of `uninit()`
can become a caller of an object which is being uninitialised. It is
the same pattern as with the count above, at another place. The
service is left with the state "stopping" for the machine, the session
ends in good order right after, and for a session which ends that way
the service leaves the state alone: a machine without a session in a
state which says that it is busy, which takes no session until the
service is started anew. `VBoxHeadless` gets there whenever it is told
to end and cannot save the state of the VM, which with this work is so
for every VM with a device of the host in it, and with a VM which is
paused. `uninit()` does now what the callback could not: when there is
no VM any more and the state is still "stopping", it tells the service
that the VM is off, on its own thread. That is so too when it was not
`uninit()` which powered the VM down but the guest, which was shutting
down as the session got closed; the reader of the change added that
case. A first version let the function get through for every thread
and every state; the same reader found that this reaches to where a
state save which failed is being undone, and that a VM process could
come to stand there. It did not go to the branches.

The reader of that change read both versions. Of the one which went in
he says: nothing wrong in what it does. The call to the server is made
by the thread which closes the session, with the lock of the session
held, as the call a moment earlier which says "stopping" is; a setter
of the server which waits for that session in that moment and the call
would wait for each other, which was so before for the earlier call.
The event for the state is fired from `uninit()` now, to whoever still
listens; of the Console itself that is the display, which does nothing
with it. And a session which is closed while a state save of a paused
VM that failed is being undone still leaves the machine paused without
a session, as before. These are left.

**Runs with both changes.** Run 375 (`sim` with the part simhost, not
strict, `pci-passthrough-hotplug-7.2-kvm` @ `b1136fa2`, 43 minutes,
Intel Xeon 6975P-C): all 131 checks pass. The VM process ends within a
second both times, and both times the service has the VM as powered off
at once. Runs 372 and 376 (`tests` on the development line, `d8d44eb9`
with the first change and `2409b39e` with both) and runs 373 and 377
(the same on the 7.1 line with the KVM backend, `2c7878c5` and
`6d552882`): they build with the changes to Main and the tests pass.
Run 378 (`package` with 3D acceleration, `b1136fa2`, 20 minutes, Intel
Xeon 6975P-C) is the build for a machine with real hardware which has
both; it is published and the one which is fetched, see "Machines with
real hardware".

The heads are `pci-passthrough-hotplug-v1` @ `2409b39e` (46 commits),
`-7.2` @ `4b6e5ddc`, `-7.2-kvm` @ `b1136fa2`, `-7.1` @ `fc72156b` and
`-7.1-kvm` @ `6d552882`. Not run: the sanitizers on these heads, as the
two changes are in Main, which the testcase does not reach, and the
last simulation with the sanitizers in the whole build is older; the
two lines without the KVM backend by themselves; the simulation on the
7.1 line (the development line had it with run 381, see below). Run 379
fetched the build of run
378 and used it, see "Machines with real hardware".

**The same with upstream's code alone (2026-11-26).** Whether to tell
upstream of the two things in Main had been left to who this work is
for, and the answer of 2026-11-26 was yes. Until then both had been seen
with a device of the host in the VM only; that they are upstream's was
read, not shown. The part of the simulation on the runner has a phase
for it since, the last one (`endpaused`): a VM with nothing of this work
in it is paused and its process is told to end. `VBoxHeadless` tries
nothing for a paused VM and closes the session, so the Console powers
the VM down from its `uninit()`. Once by itself, and once while the
memory of the guest is being written to a file through the API
(`debugvm dumpvmcore`), which holds on to the VM for as long as it
writes. With the file it is a matter of the moment, so the log of the VM
is read for whether the file was still being written when the session
got closed; a round in which it was not tells nothing, and there are
three. A tree without the two changes, told by its source, runs the
phase all the same, and what goes wrong there is told as skipped and not
counted.

**Run 380** (`sim` with the part runner, exploring, `main` @ `b6bea721`,
which is upstream's without any change, 10 minutes, AMD EPYC 7763; the
engine is KVM there): both show. With the VM paused the process ends
within two seconds; the log of the VM has no line about the VM being
off and, where it would be, the failed attempt of the function which
sets the state to become a caller of the Console; and `VBoxManage
showvminfo` fails from then on for the 90 seconds the service was given
(`LockMachine`, `NS_ERROR_FAILURE`, which is the server refusing a
session for a machine in a busy state). A service started anew has the
VM as powered off. With the file being written, in the first round: the
VM process is still there 90 seconds later, its main thread in
`Console::i_powerDown()` under `Console::uninit()`, and the log has the
failed attempt of `i_releaseVMCaller()` right after the line about the
file.

**Run 381** (`sim` with the parts runner and simhost, not strict as the
first run with the new phase, `pci-passthrough-hotplug-v1` @
`2409b39e`, 44 minutes, Intel Xeon 6975P-C): all 137 checks pass and
none is skipped. In the new phase the process ends within two seconds
both times, the second time with the file still being written when the
session got closed (it was done 0.4 seconds after), the log of the VM
has its line about the VM being off, and the service has the VM as
powered off at once. It is the first simulation of the development
line with the two changes as well: the part with the drive passes
there, 98 checks, with the phase of the guest without MSI in which the
frontend is told to end with the device in the VM and while it is
being taken out.

Upstream was told on 2026-11-26: issue 1009 of its
tracker, https://github.com/VirtualBox/virtualbox/issues/1009, in the
form for bugs: what goes wrong and where in the source, the commands
which show it, what run 380 has of it (the lines of the logs, the stack
of the process which stays), the two commits, and what run 381 has
with them. It says what was not tried: other hosts than Linux, other
frontends, and the released lines, whose code is the same in these
places.

### Two things read in the interrupt code of upstream (2026-11-26)

The readers of 2026-11-25 had left two suspicions about code of upstream
which is outside the passthrough device. Whether to spend time on them
was a question to who this work is for, and the answer of 2026-11-26 was
yes: look into them, and tell upstream if they hold. Both hold as facts
of the source of `main` (b6bea721). Neither was tried in a VM. Both came
with virtual interrupt delivery on 2026-08-25 (472003b54f, 66f002734f,
92ac7a1f6e of upstream) and are in `main` only: the 7.2 and 7.1 lines
have none of that code, so the lines with the KVM backend, which are
the ones for the target, are not affected.

- **After a saved state is loaded, the EOI of a level-triggered
  interrupt does not get to the I/O APIC**, with VT-x and virtual
  interrupt delivery. The EOI exit bitmap is built from a copy of the
  redirection table which the APIC keeps, the copy is written only when
  the guest writes an entry, it is not in the saved state, and loading
  the table does not tell the APIC. The bitmaps start as zero and are
  written anew only when the copy changes. So after a load the EOI is
  virtualized without an exit, the remote IRR bit of the entry stays
  set, and the pin delivers one interrupt and no more. It is the same
  gap which `f563e7f8e7` of this work closed for the KVM backend of
  `main` on 2026-11-19, where it was seen with a Windows guest and the
  power button; that commit left the other backends alone on purpose.
  Trying it takes an Intel host which offers virtual interrupt delivery
  to VirtualBox. The runners of
  GitHub run VirtualBox with KVM as its engine (run 380), so the
  simulation cannot.
- **The I/O APIC stores an entry which it computed before it released
  its lock.** `ioapicSetRedirTableEntry` reads the entry, computes the
  new one with the bits a guest cannot write taken over, releases the
  lock to tell the APIC (which gathers all EMTs when something of
  the route changed), takes the lock again and stores what it computed.
  An EOI on another EMT in between clears the remote IRR bit, and the
  store sets it again: the pin is dead from then on. It takes a guest
  which writes a level-triggered entry while the interrupt of that entry
  is in service on another CPU. Linux does both under one lock, so it is
  probably rare; nothing here tried to hit it.

Neither is a way for a guest to do anything to a host: a guest loses
interrupts of its own. Nothing was changed for them in the branches.
The development line is as `main` is in both; a change there would be
one to the I/O APIC and the APIC with no test which reaches it, for a
host this work has none of. Upstream was told: issue 1008 of its
tracker, https://github.com/VirtualBox/virtualbox/issues/1008, filed
on 2026-11-26 in the form for bugs, saying in its first paragraph that
both are from reading and neither was tried. Before it was filed
somebody who had not been told what was suspected was given the source
and the two questions, what the EOI exit bitmaps are made of after a
load and whether every value the function stores is still valid when
it stores it, and came to the same two answers. That reader added that
the pin comes back with a reset of the I/O APIC or a write to its EOI
register, that an interrupt delivered in the window is delivered twice,
and that only the xAPIC mode of the guest was traced; the issue says
so.

### The issue with upstream, 2026-11-23

The work is made known to upstream since 2026-11-23: issue 999 in the
tracker of `VirtualBox/virtualbox`,
https://github.com/VirtualBox/virtualbox/issues/999, a feature request
in the form upstream asks for. It says what the passthrough device of
`main` cannot do with a device which comes and goes, that there are
patches for it in this fork and where, what they have, how they were
tried and what was not tried, which is everything on hardware. It lists
seven defects of `main` which were found on the way and do not depend
on the rest, each with the commit which has its description. It says
that this is no pull request because the work is done under a pseudonym
and no contributor agreement can be signed that way, and that upstream
is welcome to use any of it.

It promises one thing: what the first test on hardware shows is added
there.

Before it was filed, all the fork has in public was searched for names
which are not to be public: the history of every branch with all its
changes, who the commits are by, the names of the branches, the texts
of the merge requests and releases, and the profile of the account.
Nothing was found. The search is a script which is kept with the tools
of this work; it takes the names from the machine it runs on, hands
them to the programs which search as a file, and prints counts.

Whoever answers there is answered there. What may be a way for a guest
to harm a host is not written there or anywhere else in public, see
`SECURITY.md` of upstream.

Since 2026-11-24 the issue has a first comment, from this side: the
defect in the event queue of Main which the sanitizers found (see "VMs
with the sanitizers"), as an eighth defect of `main` which does not
depend on the rest, with its commit and with merge request #7 of the
fork for what was run. Whether to tell upstream of it had been left to
who this work is for, and the answer was yes. Before it was posted,
all the fork has in public was searched for names again, as before
the issue was filed: nothing was found (183 commits on this branch,
368 on the code branches, the names of the branches, the texts of the
merge requests and releases, the profile). Nobody else has written
there so far.

Since 2026-11-26 there are two more issues from this side, both in the
form for bugs and both after a yes of who this work is for: 1008,
https://github.com/VirtualBox/virtualbox/issues/1008, with two things
read in the interrupt code of `main` (see "Two things read in the
interrupt code of upstream"), and 1009,
https://github.com/VirtualBox/virtualbox/issues/1009, with the two
defects of Main which let a VM process stay and a machine keep the
state "stopping" (see "Who waits for whom, in Main and in the root
port"). The text of each was searched for names before it was filed,
and all the fork has in public on the same day, before and after:
nothing was found (215 commits on this branch, 404 on the code
branches, the names of the branches, the texts of the merge requests
and releases, the profile). The three issues are looked at whenever the
work is taken up, and whoever answers there is answered there.

### Requests from the tracker of upstream, from 2026-11-22

Asked for on 2026-11-22: to take up what users ask for in the public
tracker of VirtualBox where it can be done and tested here, what is
about 3D first. The tracker has 62 open requests (label `enhancement`)
and no priorities. Each one which is taken up is a branch of its own
from the development branch of upstream and a merge request inside this
fork. Nothing is sent upstream: that takes a contributor agreement,
which is not for this work to sign.

How 3D and this work relate: the 3D acceleration of VirtualBox is for
the graphics card it emulates, the passthrough is for a graphics card
of the host. A VM uses the one or the other. What is done for 3D here
is for VMs without a card of their own.

| Request | What it asks for | What became of it |
|---------|------------------|-------------------|
| 798 | `VBoxManage modifyvm --accelerate-3d=on --graphicscontroller=vmsvga` to work in that order as well | done, merge request [#5](https://github.com/MarquisDeGris/virtualbox/pull/5), see below |
| 674, 904 | more video memory than 256 MB | The 256 MB are the screens. The 3D objects of a guest have a limit of their own, 4 GB and up to 32, which the manual did not say: a paragraph for the manual, merge request [#6](https://github.com/MarquisDeGris/virtualbox/pull/6). A setting of its own for it in the API and the user interface is not done |
| 668 | Vulkan for Linux guests by paravirtualization | months of work, not taken up |
| 518 | WDDM 2.0 and DirectX 12 for Windows guests | months of work, not taken up |
| 678 | hot-plugged devices to be seen in the settings | the graphical user interface, which is not built here |
| 944 | a way into the setup of the UEFI firmware which does not take a key hit in time | done, merge request [#8](https://github.com/MarquisDeGris/virtualbox/pull/8): `VBoxManage modifynvram <vm> firmwaresetup`, see below |
| 810 | strings of one's own in the DMI table of type 11, for handing a guest its name or a key | looked at on 2026-11-24, not taken up. Two such strings can be set today, by reading the code and not tried: the values `DmiOEMVBoxVer` and `DmiOEMVBoxRev` of the firmware device. More of them, or longer ones than 255 characters, take a DMI table larger than the 768 bytes it has, which reaches into the BIOS |
| 881, 936 (reported as bugs) | the size of a screen does not follow the window with the Guest Additions in a guest with Wayland | looked at on 2026-11-24, not taken up: it takes a guest with a desktop and the Guest Additions, which the simulation does not have |
| 580 | the KVM interface for a guest without its clock | not looked into |

The tracker also has 20 open bugs about 3D. Nearly all need a graphics
card and a certain host to be seen, which the runners do not have.

**Request 798** (`feat/modifyvm-graphics-order` @ `ca3e5b3b`, one
commit on `main`). `modifyvm` applies its options as they come, and two
of the graphics options depend on a third: whether 3D acceleration can
be had and which sizes the video memory can have depend on the graphics
controller. `--accelerate-3d` and `--vram` are remembered now and set
when all options have been gone through, the 3D acceleration first. The
report was about the first; the second has the same fault and nobody
had reported it. Two kinds of command lines which went through before
are refused now, as they ask for what the VM cannot have: 3D
acceleration followed by a graphics controller without it, and a size
of the video memory which fits the VM as it is and not what the same
command makes of it. Manual and changelog say so.

The workflow has a step for this since (`Command line checks`):
`VBoxManage` of the build with a `VBoxSVC` of its own creates a VM,
changes it and reads it back, without starting it. Six checks, three
command lines which have to work and three which have to be refused
with the VM left as it was. A tree without the change is told by its
source and is not checked. The step which runs the testcase of the
passthrough device ends well on a tree which does not have it, so that
such a branch can be built with the `tests` flavor.

**Run 280** (2026-11-22, `tests`, for finding out, that branch, 5
minutes): it builds. The build as the workflow configures it has no 3D
acceleration; the two checks which do without it pass.

**Run 281** (2026-11-22, the same with `VBOX_WITH_3D_ACCELERATION=1`
for the build, which changes what the API says a graphics controller
can do and nothing else, 7 minutes): all six checks pass. They count
since. No run has shown them failing without the change: for the
command of the report that is what the report shows, for the others it
is what reading the code says.

**The manual text** (`feat/doc-3d-object-memory` @ `7c65ccc3`, one
commit on `main`): one paragraph and one command under "Screen Tab".
The numbers and the name of the configuration value are read from the
source of the graphics device. The file is well-formed XML; the
workflow does not build the manual, and no VM was run with the value
set.

**Request 944** (`feat/nvram-firmware-setup` @ `acd8fb87`, two commits
on `main`). The firmware of a VM gets to the operating system so
quickly that the key which leads into its setup is hard to hit, in a
window which has to be clicked into first. The UEFI specification has
a way which needs no key: the variable `OsIndications`, in which the
firmware is asked to enter its setup at the next start. The firmware
VirtualBox comes with goes by it and takes the request back when it
does, as its source says and as run 324 then showed. What was missing
is a way to ask from the host. `VBoxManage modifynvram <vm>
firmwaresetup --enable` makes the request in the variable store of
the VM and creates the variable if the store does not have it;
`--disable` takes back a request the firmware has not acted on; the
other bits of the variable are left as they are. It uses what the API
has. Manual and changelog say so.

The checks for it. The step `Command line checks` of the workflow has
eight, about what the command writes: a VM without a variable store,
nothing to take back, asked for, asked for twice, taken back, no
option, and both ways with another bit of the variable set. The
simulation has a phase `fwsetup` on the runner, about the firmware
going by it: the VM is asked and started, brings up no guest for 90
seconds and runs; its screen is kept; when it is powered off the
firmware has cleared the request; the next start boots the guest; and
a request which was taken back before the start changes nothing.
Neither does anything for a tree without the sub-command.

**Run 324** (2026-11-24, `sim` with the part on the runner, for
finding out, the first commit `83aa0765`, 25 minutes, Intel Xeon
6975P-C). It builds. The simulation: all 24 checks pass, the five of
the new phase among them; the guest boots in 12 seconds where it was
not there after 90 with the request. The picture which was kept shows
the first page of the setup of the firmware, with its device manager,
boot manager and boot maintenance manager; it was looked at, nothing
checks it. The command line checks: one of the eight passes. The
others fail where the variable store was just made and has no variable
yet. That version of the sub-command went through the list of all
variables to see whether the one it wants is there, which did not work
for such a store.

**Run 325** (the same day, the second commit `acd8fb87`, 9 minutes,
AMD EPYC 7763). The sub-command asks for its one variable by name and
takes not getting it as its not being there. All eight command line
checks pass, and all 24 of the simulation again, the guest booting in
5 seconds. The command line checks count since.

Not tried: a guest whose boot manager is in the variable store,
Windows for one. The firmware acts on the request before it looks at
a boot entry, so it should not matter. Not done: the same in the
graphical user interface, which the report asks for too and which is
not built here.

**Report 996, about the manual** (2026-11-25). Not a request, and as
small as a change gets: the text about `VBoxManage modifymedium
--compact` has "non-VID" where it means VDI, and names ext2 and ext3
as the file systems the `zerofree` utility supports, which has
supported ext4 as well for years. Two places in
`doc/manual/en_US/man_VBoxManage-modifymedium.xml`, on
`fix/doc-modifymedium-compact` @ `5476f2d7`, merge request #9 in the
fork against `main`. The help which `VBoxManage` prints is made from
that text, so the step `Command line checks` of the workflow looks at
the help where a tree has the text corrected. **Run 356** (`tests`, 5
minutes, AMD EPYC 7763): the branch builds, and the help of the
`VBoxManage` which was built says "non-VDI" and "ext2, ext3 and ext4".
The check counts since. That the utility supports ext4 is from what it
says of itself; it was not tried here.

### The 7.1 line and what the 7.2 line received after it (2026-11-20)

Upstream ended its support for 7.1 with 7.1.18 in April 2026. The
security updates it has published since then (July, August, September
and October 2026) are releases of 7.2 only. The public CVE database
(NVD, asked on 2026-11-20) has 62 numbers of Oracle for VirtualBox from
those four dates: 16, 21, 19 and 6, each naming a version of 7.2 as the
supported version affected and none saying anything about 7.1, which is
what "not supported" means and not that 7.1 is free of them. All have
the component "Core"; four are reached "via RDP", which on 7.1 is code
of the extension pack and not in the source tree. One more number of
that time, from another source, is about the PCnet device before 7.2.8;
that file is the same on upstream's 7.1 and 7.2 branches. Its 7.1 branch
in git went on
until 2026-09-25, with 115 commits after 7.1.18, most of them taken over
from the development line; the branches of this work for 7.1 are on the
head of that branch and so have them, which no release of 7.1 does.

What the 7.2 branch got from March 2026 on and the 7.1 branch did not
was looked up by the revisions the commits of both name, and each change
which is about code a Linux host runs was then tried on 7.1:

- Taken over to the 7.1 branch (`pci-passthrough-hotplug-7.1` @
  `ec8bcc46`; built in run 252, every changed file compiled for ring 3
  and for the kernel driver without a warning, the tests of the device
  pass; run in VMs on the branch with the KVM backend in run 254, where
  every check of the simulation passes with it): 25 changes as they are,
  to devices (VirtIO, USB, audio),
  the host services a guest talks to, the image formats (VMDK, VDI, GPT,
  ISO), the import of appliances and the console, two of them adapted in
  one place each; and r175241, with which the VMM keeps a device from
  reaching its own MMIO region by way of guest memory, 18 files, one
  place adapted. One further change was on the branch already under
  another revision. The commits list them with their revisions.
- Not taken:
  - Three changes to the execution engine in the kernel driver (r174867,
    r174869, r174871): 240 lines of the assembly which enters the guest,
    with a conflict, and nothing here runs that engine, so it could not
    be tried. They are for VMs on the kernel driver of VirtualBox, on
    which the device of this work does not run.
  - The move of the NAT library to the version of 7.2. 7.2 went from
    libslirp 4.8.0 to 4.9.5 and has changes of its own in it (r175239 is
    about code 7.1 does not have); the code around it differs between
    the lines by tens of thousands of lines, so the library cannot be
    exchanged. What its changelog lists under "Security" between the two
    versions was taken instead, see below.
  - What the source tree of 7.1 does not have: the remote desktop
    server and the USB webcam, card reader and network devices, which
    were in the extension pack then.
  - Windows hosts, the Guest Additions, the user interface, and the
    other libraries the tree carries, which a Linux build takes from the
    distribution.
- From the NAT library (added later on 2026-11-20): the four entries its
  changelog has under "Security" between 4.8.0 and 4.9.5, one of them
  with a CVE number of the library's own, and one fix which belongs with
  them, put into the copy of 4.8.0 as 7.2 has them in its copy of 4.9.5
  (urgent data of TCP, the header length of reassembled IP fragments,
  the length of IPv6 packets, OEM commands of NC-SI, the DHCPv6 reply).
  Built in run 256, where the copy of the library in the tree is what
  gets compiled, without a warning in a file which was changed, and run
  with a VM behind NAT in run 258.
- One more from Main, found on a second pass through what had been set
  aside by its subject line: r175077, with which the extra data of an
  appliance is imported only when the new import option `KeepExtraData`
  asks for it. Taken without its part for the user interface, which
  does not apply to 7.1 as it is and is not built here. The rest of
  that second pass was on 7.1 already under another revision, or about
  nothing but compiler warnings, or new features.
- A second pass on 2026-11-20, by the files and not by the commits: the
  sources of the devices on `backports-7.1` and on upstream's 7.2 branch
  were compared file by file, the lines about revision and copyright
  left aside (storage, bus, input, serial, VMMDev, VirtIO, audio, the
  TPM, USB, the PC devices, VGA and the smaller network devices, and
  outside of the devices the image formats and the host services; of
  the E1000, VMSVGA and the NAT driver only the history). What it
  showed:
  - Nothing among the changes since March 2026 which the first pass
    missed. The changes to graphics and to the firmware configuration
    device which 7.2 got from April to September are on upstream's own
    7.1 branch under the same revisions: upstream went on taking fixes
    to that branch after its last release of 7.1.
  - What 7.2 has beyond that is older than 2026 and was never taken to
    7.1 by upstream while it supported 7.1: the buffers of USB requests
    reworked (August 2025), the command buffer interface of the TPM
    reworked together with a change to how MMIO accesses of odd sizes
    reach a device (September 2025), a new variant of the E1000, the
    graphics device for 3D with DirectX 11, and many changes for
    compiler warnings. They are reworks of hundreds of lines which
    depend on each other and on the VMM of 7.2, not fixes which can be
    lifted out, and they are not taken here. This is the main reason
    why 7.1 with everything on these branches is still not what 7.2 is.
  - Taken: one check in the ACPI device, of the size of a table which
    the configuration of a VM names as a file, which on 7.1 was made and
    not acted on. Built in run 259.
  - The 2D video acceleration for guests, which 7.2 no longer has at
    all, is not in what is built here: `configure --build-headless`
    leaves it out on 7.1, in every run so far. `LocalConfig.kmk` has the
    switch (`VBOX_WITH_VIDEOHWACCEL`) as well since run 259, for whoever
    builds with the user interface; see [INSTALL.md](INSTALL.md).
- The KVM backend of the two lines compared (2026-11-20): the version
  for 7.1 is of 2025-11-03, the one for 7.2 of 2026-10-21, and its
  authors have published nothing newer. What the two patches add and
  remove was compared file by file: it is the same but for six files of
  the VMM and one register which upstream renamed between the lines, and
  a blank in a makefile. So the backend for 7.1 lacks no change which
  the one for 7.2 has, and the three changes of this work to the backend
  are on both branches.
- The same without the passthrough work: branch `backports-7.1`, seven
  commits on upstream's 7.1 branch (the 25 changes, r175241, the result
  of `Console::i_saveState`, the NAT library, r175077, the size check in
  the ACPI device, and the testcase of the guest control service), which
  is merge request [#3](https://github.com/MarquisDeGris/virtualbox/pull/3)
  in the fork. It builds from nothing with its first five commits (run
  256); the sixth is built as part of the branch with the KVM backend
  (run 259), the seventh too (run 260). Who is on 7.1 and does not want the
  passthrough work can take these alone.
- What has gone through what, on the branch with the KVM backend: the
  NAT library and the VirtIO network device with a VM behind NAT (runs
  258 and 259); the import of appliances with a VM exported and imported
  (run 259); the guest properties, the shared clipboard, the shared
  folders and the code the services share with the testcases of the tree
  (run 259); the guest control service with its rewritten testcase, the
  audio mixing buffer and the switch of the internal network with theirs
  (run 260); VDI with the disk of every VM of the simulation, VMDK with
  the appliance, and both with the testcases of the image formats (run
  260). Not gone through by anything here: the changes to drag and
  drop, to the host channel, to the HDA and SB16 devices, to VUSB and
  the USB capture, to GPT and ISO. The tree has no testcase for these
  which runs without a guest.
- For trying it: the simulation has a VM with a virtio network adapter
  behind NAT since then (an address by DHCP, a ping to the gateway, a
  file fetched from the runner and compared), which goes through the
  NAT library and the VirtIO network device; it passes on the branch
  with the KVM backend in run 258. A branch without an
  execution engine cannot run VMs in the workflow, so what is on
  `backports-7.1` is run as part of `pci-passthrough-hotplug-7.1-kvm`,
  which has the same commits.
- What this does not make of 7.1: the line which has what 7.2.22 has.
  Upstream does not say which change belongs to which of its advisories,
  the comparison goes by commit messages, and a change can depend on
  others in ways only its authors know. The line which gets the updates
  is 7.2. The branches of this work for 7.2 are on the newest state of
  that branch (after 7.2.22; upstream had nothing newer on 2026-11-20).

### The management and the review, 13 to 15 November

From 2026-11-13 to 2026-11-15 GitHub did not start workflow runs for this
repository: it had disabled the workflows of the fork, as it does to save
compute, after 215 runs in two days of which many failed on purpose. The
workflow is the only way this work gets built and tested. What follows was
written in that time, on `scratch/mgmt`, without a compiler, and reviewed
instead. The first build after it (run 216) found one mistake, a status
code which `VBoxManage` does not have, with all tests passing; run 217 is
the tree with that put right, run 218 the same with the sanitizers. It is
in the merge request as six commits since:

- The interfaces of the passthrough device are found at LUN 0 of the device
  instance, which is where Main looks for the interfaces of a device
  (`PDMR3QueryDeviceLun`); with a test.
- Host PCI addresses are ordered by their domain first
  (`PCIBusAddress::operator<`). With addresses from more than one domain
  the order was not a consistent one, and the configuration of a VM is
  built in that order. With a test.
- WP7, the management:
  - Hot-plug is switched on for a VM with the extra data item
    `VBoxInternal2/HostPCIHotPlug`. Main then gives the root port in front
    of each PCI device of the host the hot-plug capable slot and has the
    ACPI tables grant native hot-plug. The device has to be attached at
    guest address 01:00.0 or 02:00.0 and to be alone on that bus, so a VM
    has two such devices at most.
  - After the VM was created Main asks each passthrough device for its
    layout and stores it as extra data
    (`VBoxInternal2/HostPCILayout/<host address>`); it goes into the
    configuration at the next start, so the VM starts without the device.
  - `IConsole::plugHostPCIDevice`, `unplugHostPCIDevice` and
    `getHostPCIDeviceState`; `VBoxManage controlvm <vm> plugpci` and
    `unplugpci`, the state in `VBoxManage showvminfo`; man pages.
  - Main cannot be run in the test without hardware. What it does is
    part of the hardware test.
  - Changelog entries, and a topic for the security guide of the manual.
    The manual is not built by the workflow; the files are well-formed.

- A review of everything above and of the hot-plug code of WP5 and WP6:
  three independent readings, then each point checked against the code,
  and for the behaviour of guests and of the host kernel against the
  sources of Linux and QEMU. What it changed, with tests where the
  testcase reaches it, which makes it 55 tests and three random tests:
  - Root port: the attention button is pressed along with a device being
    put into the slot if that does not bring the link up. A Linux guest
    does not ask for being told about presence changes of a slot with a
    button and has the power of an empty slot off; it would not have
    noticed a device put into the running VM. The tests had a guest which
    asked for all events, there is one now which asks like Linux does.
  - Root port: the secondary bus reset of the bridge control register
    takes the link down for as long as it is set, which resets the device
    in the slot. Guests reset devices without a function level reset this
    way. A reset of the VM clears the bit.
  - Root port: all writes to its config space take the PCI lock. The
    guest comes with it, a reset of the VM and loading a saved state did
    not, while the device in the slot comes and goes on other threads.
  - Root port: if the device in its slot comes and goes, the port does not
    let the VM be built with anything else on its secondary bus. Main
    checks the same from the configuration (below); the port does not
    rely on that.
  - Passthrough device: a host which is short of interrupt vectors sets
    up none and tells how many it has. The device takes those instead of
    going without interrupts.
  - Passthrough device: taking the device out of the slot is one step for
    whoever puts a device in.
  - Passthrough device: INTx is raised on an EMT. The thread which waits
    for the interrupts did it and could wait for the PCI lock while the
    holder of that lock waited for the thread, which cost five seconds
    and the change of the interrupt setup the guest had asked for. INTx
    is lowered when the device cannot be the one raising it any more: the
    guest disabled it or uses MSI or MSI-X, the device was reset, or it
    is gone. INTx still does not work beyond the first interrupt (WP8).
    (Written on 15 November. It works since 2026-11-22, and the line is
    not left raised any more, see "INTx (WP8)".)
  - Passthrough device: an interrupt which the device raised under an
    earlier interrupt setup, and which nobody got around to, is no
    interrupt of the next setup or of the next device in the slot.
  - Passthrough device: a device which could not be let go of counts as
    still in use, so no state is saved. The time a guest was given for a
    device does not count for the next device in the slot.
  - Main: the check that the PCI device of the host is alone on its guest
    bus is made when all devices of the VM have their addresses, as some
    storage controllers and network adapters get theirs on guest bus 1
    and 2 as well. The VM does not start otherwise.
  - Main: hot-plug is a setting of the VM only, not of all VMs.
    `IConsole::unplugHostPCIDevice` waits for the guest in pieces, so the
    VM can be powered off while it waits.
  - `VBoxManage`: a host PCI address with a number which is out of range
    is invalid instead of standing for another device.
  - Looked at and left: a guest which has no MSI for the root port gets
    no slot events (the slot has no interrupt pin, see the decisions);
    memory of the device stays mapped in the VM process until the process
    ends, as before this work.

[HWTEST.md](HWTEST.md) has the procedure for the hardware test as a
draft.

What the review round wrote was read once more by three readers who had
not seen it, in place of the compiler and of a first test run: one for the
device code, one who went through every new and changed test step by step
against the code, one for Main and `VBoxManage`. None found anything which
would not compile, and the tests came out as written. What they did find
was looked at and changed:

- The interrupt eventfds are all emptied before an interrupt setup, not
  only as many as the setup has vectors, and they no longer make a reader
  wait. INTx is lowered before its setup is taken down, as that can fail.
- A check added in the first round is gone again: a port without the slot
  refusing the saved state of a port with one. It went by config space
  which the guest of such a port can write to, so a guest could have kept
  its own state from being loaded. A state which does not fit is refused
  by the passthrough device, which has a unit of its own in the state.
- The time a guest gets in `IConsole::unplugHostPCIDevice` counts from
  when it is asked. A mistake in the configuration of the PCI devices of
  the host ends the VM start with its message and without a release
  assertion. The message about a PCI bridge on the bus of the slot says
  what the bridge is there for.
- `VBoxManage`: the numbers of a host PCI address take no prefix, and
  `@` may follow the address only where a guest address can.
- A test which was there before could fail now and then (the reset count
  when the guest lets go of a device while another thread detaches it);
  it allows for both orders now.

That the readers found no mistake a compiler would have found, and that
the compiler then found one, in a line written after their reading, is
worth noting down: reading by somebody else is worth doing when nothing
can be built, and it is no build.

The six commits of the merge request for all of this, on top of the 27
it had: the interfaces of the device at LUN 0 (`98f2f08e`); the order of
host PCI addresses (`82a5f83b`); what the review changed in the root port
and the passthrough device, with the tests (`3c877103`); Main
(`f8ea63a1`); `VBoxManage` and its man pages (`e7fc29a1`); changelog and
manual (`3d9ea801`). The head is what was built and tested. The commits
before it were not built one by one, to keep the number of runs down; the
first three change the devices and their testcase only.

## Done

- WP0: reference build of unmodified upstream is green in CI: headless
  release build, 12.5 minutes, 871 compiler warnings in the unmodified tree.
  The build output of the base is cached; a test run takes about 100 seconds.
- WP1: test harness `tstDevPciVfio` (see below) compiles warning-free and
  passes against the unmodified device (run 9).
- WP2, part 1 (commits `778d8d88`, `eb637e18`):
  - interrupt setup tracked on the configuring side, derived in one place
    from command / MSI / MSI-X control registers; poller acknowledges mode
    switches by sequence number;
  - eventfds, poll array and the `VFIO_DEVICE_SET_IRQS` buffer sized by the
    vector counts of the device; devices with more vectors than supported
    are refused; the guest never gets more MSI vectors than the device has;
  - read-only bits of the MSI-X message control register are kept;
  - reset masks MSI-X vectors and brings the interrupt setup in line;
  - a passthrough device on the root bus gets the PCI helper pointer it
    needs for MSI-X delivery (it was only set behind a bridge).
- WP2, part 2 (commit `f31b5dda`):
  - the guest gets an MSI-X table and pending bit array of its own at the
    place where the device has them; the table of the physical device is no
    longer reachable by the guest;
  - memory BARs keep the size they have on the device and are made of
    segments which are mapped or trapped; the pages with the MSI-X table and
    pending bits are trapped, the rest of such a page is forwarded;
  - masked vectors become pending and are delivered on unmask (vector and
    function mask); the pending bits are read-only for the guest;
  - BARs which cannot be mapped (VFIO flag, sub-page size) are accessed
    through the device file descriptor instead of failing the VM start;
  - memory BARs are registered for the function they belong to.
- WP2, part 3 (commit `33ba49dc`):
  - capability lists may come in any order; the config space bytes each
    capability claims are tracked, so lists which loop and capabilities
    which overlap are refused at VM start;
  - the extended capability list is checked for loops and bad pointers;
  - a PCI Express capability of an unknown version is hidden.
- WP2, part 4 (commit `05617ea1`):
  - the MSI capability is emulated in the device with the layout the
    physical device has (32 or 64 bit address, with or without per-vector
    masking), so it never covers more config space than on the device;
  - per-vector masking with pending bits, delivery on unmask; the vector
    number goes into the message data when several vectors are enabled;
  - config space writes are taken apart by register: partial writes are
    merged with the current content, writes covering several registers
    reach each of them;
  - reset clears the MSI state.
- WP2, part 5 (commit `f3983a48`):
  - the guest memory is made available to the device when the VM is
    constructed: all of it, or the VM does not start and nothing stays
    mapped;
  - the guest cannot enable bus mastering on a device which has not got
    the guest memory;
  - mappings are removed explicitly when the device is destroyed;
  - the device requires the native execution engine (KVM), where guest
    memory stays at a fixed place for the life of the VM, and refuses to
    be constructed with another one.
- WP2, part 6 (commit `b27efb27`):
  - a device which does not answer reads as all ones and ignores writes on
    all guest access paths (I/O port BARs, legacy VGA ports and memory,
    memory BARs); no host error status reaches the instruction emulation;
  - the legacy VGA memory fill stays inside the video memory;
  - memory BARs are only mapped if they may be read and written;
  - config space handling settings replace each other instead of mixing;
    the device quirk for a config space range leaves capabilities alone;
  - no release assertion is left in the config space read path.
- WP2, part 7 (commit `cb26af23`):
  - the host address of a function has to be a PCI address before it is
    used in paths;
  - all file descriptors are close-on-exec;
  - the option for trapping memory BAR accesses is read where it is
    declared;
  - checks which are not system calls no longer take their status from
    errno;
  - the destructor only undoes what was set up.
- WP2, part 8 (commit `71037836`):
  - a reset of the VM first makes each function keep quiet (no interrupts,
    no bus mastering, no decoding), then resets it;
  - functions which cannot be reset on their own, as is common for devices
    with several functions like graphics cards, are reset through the bus
    they are on; if that is not possible either they stay disabled until
    the guest sets them up again;
  - when the guest clears the decode bits of the command register, the
    guest mappings of the BARs are removed before the device stops
    decoding;
  - trapped accesses to a memory BAR go through the device file, never
    through the mapping of the BAR in the VM process.
- WP2, part 9 (commit `f9f95548`): the documentation block of the device
  describes what is there now.
- CI has a `sanitize` flavor: the testcase, the device and the bus code
  compiled into it are built with AddressSanitizer and
  UndefinedBehaviorSanitizer; memory errors, leaks and undefined behaviour
  fail the run. All 29 tests pass with it (run 46).
- WP2, part 10 (commit `f4691957`): a function's MSI and MSI-X capability
  are taken once; further ones are left out of the guest's capability list.
  Found by the fuzz test with the leak check.
- T2 fuzz test (commit `680a21c5`): random device models (BARs, capability
  lists including broken ones, MSI and MSI-X layouts, extended capabilities,
  ROM, VGA ranges, devices without reset or IOMMU) with random guest
  accesses, interrupts, resets and a device which stops answering. Checked:
  the VM is constructed or nothing is left behind; guest memory mappings
  never change; no request to the kernel beyond the vectors and regions a
  function has; nothing left open or mapped afterwards. Model count and seed
  come from the environment, CI runs 2000 models, 20000 with sanitizers.
- The tests were checked against deliberate defects on throwaway branches:
  an unlimited MSI vector count is caught by its unit test (run 51); an
  off-by-one at the end of the MSI-X table was not caught at first (runs 52,
  55), which led to unit checks at the ends of table and pending bits and to
  the fuzz test aiming at such places; both now catch it (runs 54, 58).
- Harness (commit `7afa04ad`): the fake kernel has the legacy interface with
  container and group as an alternative; construction, guest memory mapping
  and its failure, BAR, MSI, reset, bus reset and descriptor inheritance are
  tested with it, and the fuzz test uses it for a part of its models.
- WP3 (commit `24515f86`): the host taking the device back.
  - the request and error notifications of VFIO are watched by a thread
    which works in every VM state;
  - a function the host wants back or reports an error of is taken out of
    service: guest accesses are answered locally (all ones, writes dropped),
    the device memory mapped into the guest is replaced in place by ordinary
    memory, the device file descriptor is replaced by one which refuses
    everything, which lets go of the device;
  - an EMT then removes the guest mappings; from there on the guest finds
    nothing where the function was;
  - other functions stay in service; a reset leaves such a function alone.
  - Not yet: asking the guest first when the host only asks politely. This
    needs the hot-plug slot of WP6; until then every removal is a surprise
    removal for the guest.
- WP4, first part (commit `f6a1395f`): saving the state of the VM and
  teleporting it are refused while a device is passed through, with a
  message for the user; loading a saved state into a VM which has the device
  is refused too. The second part (saving with an empty slot) comes with
  the hot-plug slot of WP6.
- WP5 (commits `2e543155`, `bd836fb5`): the guest side of hot-plug.
  - A PCI express root port configured with `HotPlug` has a slot for PCI
    express native hot-plug: power controller, attention button, the two
    indicators, hot-plug surprise; commands complete at once; slot events
    are signalled by MSI.
  - The PCI express capability of such a port has the access attributes of
    the specification (read-only, read-write, write-1-to-clear). Other
    bridges are unchanged.
  - While the link to the slot is down (no device, no power, link disabled)
    the guest gets no answer from what is behind the port.
  - A device which comes and goes registers with the port and is told about
    the link. The passthrough device does not do that yet (WP6).
  - Nothing new in the saved state; after loading, the guest is told what
    changed in the slot. A state of a port with another slot is refused.
  - ACPI: with the setting `PciExpressNativeHotPlug` of the ACPI device,
    `_OSC` grants the guest native hot-plug, PME, AER and the PCI express
    capability structure, and the PCI root bridge is a PCI express root
    bridge (`PNP0A08`). Without the setting nothing changes but a compatible
    ID being there.
  - Until WP7 both settings are extra data:
    `VBoxInternal/Devices/ich9pcibridge/<n>/Config/HotPlug` and
    `VBoxInternal/Devices/acpi/0/Config/PciExpressNativeHotPlug`.
- WP6, first part (commit `6176a66b`): the passthrough device behind a root
  port with a hot-plug capable slot is the device in the slot.
  - The guest switching the slot off, or disabling the link, disables and
    resets the device like a reset of the VM.
  - `pciVfioR3SlotDetach()` takes the device out of the slot: the guest is
    asked by the attention button and gets time to switch the slot off; if
    it does not, the device stays or goes the hard way, as the caller says.
    Nothing calls it yet (WP7).
  - The host wanting a function back: the guest is asked first if the
    device still answers and the slot is on, and gets `GuestReleaseTimeoutMs`
    (new setting, 15 seconds by default). An unplugged device, an error and
    a slot which is off empty the slot at once. The whole device goes and
    the guest is told that the slot is empty.
- WP6, second part (commit `defa244c`): `pciVfioR3SlotAttach()` puts the
  configured device into the empty slot while the VM runs.
  - The VM is built around the device it was started with (BARs, place of
    the MSI-X table, ROM, interrupts); a device which differs is refused.
  - The device file takes the place of the stand-in, the device memory is
    mapped where the guest mappings are, the config space the guest sees is
    put together anew; all functions or none.
  - The device starts out disabled and reset, the guest is told that the
    slot is occupied.
  - With the legacy interface the group stays with the VM while the slot
    is empty, so the host cannot use the device for something else.
  - Nothing calls it yet (WP7).
- WP6, third part (commit `949a5c03`): the VM can start without the device.
  - Each function can have a `Layout` node in its configuration which says
    what to build the VM from: BARs, ROM, interrupts, place of the MSI-X
    table. `pciVfioR3QueryLayout()` returns it for a VM which was started
    with the device; storing it is for the management to do (WP7).
  - If the device cannot be opened and there is a layout, the VM is built
    from it and the slot starts empty; `pciVfioR3SlotAttach()` puts the
    device in later if it matches.
  - Needs the hot-plug slot; all functions of a device are there or none;
    a device which is there at the start is what counts.
- A message of the PCI bus (commit `48f771d9`): a saved state which has a
  device with another vendor ID where the VM has one is refused with a
  message again. The message was given the IDs where its format takes the
  config spaces, so it could not be put together and loading did not get
  as far as refusing. Found by the tests of the fourth part of WP6.
- Loading a saved state leaves MMIO2 pages alone which are zero already
  (commit `2ef64145`, in PGM). The memory which keeps the place of the
  BARs of a device which is not there stays without host memory behind it
  when a state is loaded. The device testcase does not reach PGM, so this
  has no test of its own. Only a baseline build compiles it, which run 180
  did.
- WP6, fourth part, which is the second part of WP4 (commit `fb6dca3a`):
  the state of the VM can be saved and the VM can be teleported while the
  hot-plug slot is empty.
  - Refused while the device is in the slot, as before. With the slot
    empty, what an EMT has left to do about the device is done first and
    what the config space has of the device is cleared, so the PCI bus
    saves the same whether the VM was started with the device or without.
  - The saved state unit of the device has the layout the VM is built
    from. A state is only loaded into a VM which is built from the same.
  - If the VM which loads a state has the device, the device is taken out
    of the slot before anything is loaded and put back afterwards; the
    guest sees a device being plugged in. The reason is the memory: PGM
    reads all MMIO2 pages when it saves a state and writes to them when
    it loads one, and the BARs mapped into the guest are such pages. That
    has to be the ordinary memory keeping their place, never the memory
    of a device.
  - A device is put into the slot only while the VM is running or
    suspended, so not while a state is being saved or loaded.
  - One critical section for everything which is done about the slot
    being empty. This also closes two gaps of the second part for VMs
    with several virtual CPUs (the EMT part of a removal running next to
    a device being put in, or twice at the same time).
  - If ordinary memory cannot be put into the place of device memory when
    a device is let go of, no state is saved or loaded until the next
    device was put in and taken out.
  - Limit: a live save (teleportation, snapshot of a running VM) has PGM
    scan the memory keeping the place of the BARs in every pass, which
    takes time and bookkeeping memory in proportion to the size of the
    BARs.
- WP6, fifth part (commit `80a60fc9`): an interface for whoever manages the
  VM, `PDMIPCIPASSTHROUGHPORT` in `include/VBox/vmm/pdmifs.h`, which the
  device has on its base interface.
  - It tells whether the device is in a hot-plug capable slot, whether the
    PCI device of the host is in the VM and whether the guest can reach it;
    it puts the device into the slot and takes it out.
  - It tells the layout of a function as values with the names they have
    in the configuration. The management stores them and puts them into
    the configuration as they are, without knowing what they mean.
  - The tests get at the device through it, the way the management does.
- Interrupts after the VM was suspended and resumed (commit `07cbc95b`):
  the thread waiting for the interrupts of a function is entered anew each
  time the VM gets going and put together what to wait for only when the
  interrupt mode had changed. After a suspend and resume, which is also
  what a snapshot does, the interrupts of the device no longer reached the
  guest. The driver has had this all along; the random test below found
  it.
- WP6, sixth part (commit `c5cc9847`): a random test for the device in the
  slot. Guest, management, host and VM do things in random order: accesses,
  devices which come and go in every way there is, another kind of device
  plugged in, resets, suspending, saving and loading states. After each
  operation it checks what has to hold whatever happened, among it that a
  device in the slot is reached by the guest and gets its interrupts
  through. One case it does not get to has a test of its own: a state is
  loaded while the host waits for the device.
- Gap list G1 to G8 and the plan in `PLAN.md`.

## Test evidence

Each new test was run against the device without the fix it belongs to: one
by one before the fix up to run 215, the tests of the review together and
afterwards (run 219).

| Test | Before the fix | With fixes (run 217) |
|------|----------------|---------------------|
| Construct, config space, BAR, DMA map, destruct | pass; with the DMA mapping expectations of part 5: fails, 3 checks (run 33) | pass |
| MSI delivery | pass | pass |
| MSI vector count (all Multiple Message Enable encodings) | fails, then the process aborts (run 15) | pass |
| MSI-X, 64 vectors, dedicated BAR | fails, 8 checks (run 15) | pass |
| MSI-X, table and PBA inside a BAR with device registers | fails, 9 checks (run 20) | pass |
| Two functions, each with a memory BAR | fails, 3 checks (run 20) | pass |
| BAR which can not be mapped | VM construction fails (run 20) | pass |
| Capability list from high to low offsets | VM construction fails (run 26) | pass |
| Capability list which loops | pass | pass |
| Extended capability list which loops | never finishes (run 26) | pass |
| 32-bit MSI without masking, followed by another capability | fails (runs 23, 29) | pass |
| MSI with several vectors and per-vector masking | fails, 4 checks (run 29) | pass |
| Config space writes of all widths | fails, 9 checks (run 29) | pass |
| Reset with MSI enabled | fails, 1 check (run 15) | pass |
| Reset with MSI-X enabled | fails, 3 checks (run 15) | pass |
| Unknown device node | pass | pass |
| Execution engines without fixed guest memory | fails, 6 checks (run 33) | pass |
| Guest memory can not be made available to the device | fails, 10 checks (run 33) | pass |
| I/O port BAR, device which stops answering | fails, 3 checks (runs 37, 38) | pass |
| Legacy VGA ranges of a graphics device | fails, 8 checks (runs 37, 38) | pass |
| Device quirk for a config space range, with and without a capability in it | the process aborts (run 37) | pass |
| BAR which can not be written | the process crashes (run 38) | pass |
| File descriptors are not inherited by child processes | fails, 1 check (run 42) | pass |
| Memory BAR with all accesses trapped on request | fails, 1 check (run 42) | pass |
| Device without IOMMU, device which is not PCI | pass | pass |
| Host address which is not a PCI address | fails, 10 checks (run 42) | pass |
| Guest mappings are gone before the device stops decoding | fails, 2 checks (run 44) | pass |
| Reset of functions which can not be reset on their own | fails, 2 checks (run 44) | pass |
| Trapped BAR accesses do not need the mapping of the BAR | the process crashes (run 44) | pass |
| MSI and MSI-X capability more than once | fails, 8 checks, and leaks memory (run 64) | pass |
| Random device models and guest accesses | found the above (run 57) | pass |
| Legacy interface with container and group | not run against older states | pass |
| The host takes the device back while the VM runs | fails, 26 checks (run 81) | pass |
| Device taken back before power on, device error | fails, 16 checks (run 81) | pass |
| The host takes the device back while the guest is busy with it | fails (run 81) | pass |
| Saved state is refused | fails, 8 checks (run 87) | pass |
| Root port: registers of the hot-plug slot | new feature; deliberate defects are found (runs 92, 95, 98, 99) | pass |
| Root port: slot events and interrupts | new feature; deliberate defects are found (runs 92, 94, 97) | pass |
| Root port: device in the hot-plug slot | new feature; deliberate defects are found (runs 93, 97) | pass |
| Root port: hot-plug slot and saved state | new feature; deliberate defect is found (run 96) | pass |
| Root port: random guest accesses and slot events | new feature; deliberate defect is found (run 98) | pass |
| Hot-plug slot: the guest switches the slot off and on | new feature; deliberate defect is found (run 105) | pass |
| Hot-plug slot: the device is detached | new feature; deliberate defects are found (runs 106, 108, 114) | pass |
| Hot-plug slot: the host wants the device back | new feature; deliberate defects are found (runs 107, 108, 109, 115) | pass |
| Hot-plug slot: a device is put into the slot | new feature; deliberate defects are found (runs 120 to 126, 130) | pass |
| Hot-plug slot: the VM starts without the device | new feature; deliberate defects are found (runs 133, 134, 136 to 139, 141, 142) | pass |
| Hot-plug slot: saved state with the slot being empty | new feature; deliberate defects are found (runs 167 to 169, 171, 172) | pass |
| Hot-plug slot: saved states which are refused | new feature; deliberate defects are found (runs 166 to 168, 170 to 179) | pass |
| Hot-plug slot: a state is loaded while the host waits for the device | a deliberate defect which went unnoticed before (run 211) is found (run 214) | pass |
| Hot-plug slot: the interface for the management | new feature; deliberate defects are found (runs 185 to 192) | pass |
| Interrupts after the VM was suspended and resumed | fails, 14 checks (run 202) | pass |
| Hot-plug slot: random operations with devices coming and going | new test; found the interrupts lost after suspend and resume (run 195); deliberate defects are found (runs 207, 215) | pass |
| Order of PCI addresses | not run without the change | pass |
| Interrupts with a host which is short of vectors | fails, 3 checks (run 219) | pass |
| INTx: raised for the guest and lowered again | fails, 12 checks (run 219) | pass, until 2026-11-22. The test "INTx: a pulse for each interrupt, and the device is asked for the next" took its place then, with the device changed; that one passes and was not run against the device as it was |
| INTx: raised while the guest changes the interrupt setup | fails, 7 checks (run 219) | pass |
| Root port: a device is put into the slot of a guest going by the attention button | fails, 9 checks (run 219) | pass |
| Hot-plug slot: interrupts of a device which was taken out | fails, 3 checks (run 219) | pass |
| Root port tests which got new cases with the review (slot events, device in the slot, saved state) | fail with 56, 2 and 1 checks (run 219) | pass |
| Random device models and guest accesses, with the mock taking the PCI lock for an interrupt line like PDM does | runs into the time limit (run 219): each INTx raised while the guest changes the interrupt setup costs five seconds | pass |
| ACPI table: hardware ID of the root bridge and `_OSC`, executed with the ACPICA tools | without the change: ID and `_OSC` as before (first half of the check) | pass |

## Test harness (WP1)

One testcase binary, `tstDevPciVfio`, built with `VBOX_WITH_TESTCASES` on
Linux only:

- compiles the real `DevPciVfio.cpp`, `DevPciIch9.cpp`, `MsiCommon.cpp`,
  `MsixCommon.cpp` into the test, so guest config accesses run through the
  real ICH9 bus code;
- `tstDevPciVfioPdm.cpp`: a focused mock of the PDM device helpers these
  files use: CFGM tree, I/O port / MMIO / MMIO2 registries with map state and
  a guest access dispatcher (ECAM for config space), PCI bus registration,
  interrupt log, PDM threads on IPRT threads, VM reset, suspend and resume,
  the VM state. Saved states are saved and loaded for real, the way SSM
  does it: preparation, the data of each unit as a stream of typed items,
  completion, with a unit standing in for PGM which reads every MMIO2 page
  when saving and writes to every one when loading. A test can change an
  item of a state or take a unit out. Unimplemented helpers trap;
- `tstDevPciVfioKrnl.cpp`: a fake VFIO/IOMMUFD kernel in the test process,
  reached through linker `--wrap` of `open`, `close`, `ioctl`, `pread`,
  `pwrite`, `mmap`. Regions are backed by a memfd, interrupts by real eventfds, so
  the device's own poller thread and mappings run unmodified. Tracks open
  descriptors and DMA mappings for leak checks, can fail one ioctl on demand,
  can make a device "gone", and can fail or announce the mapping which puts
  ordinary memory into the place of device memory;
- `tstDevPciVfio.cpp`: the tests.

The root port (`ich9pcibridge`) in front of the device is part of the
harness since WP5, saved states since the fourth part of WP6.

## Next

1. The hardware test ([HWTEST.md](HWTEST.md)), which needs a person with
   the hardware. The merge request is marked ready for it;
   [INSTALL.md](INSTALL.md) says how to build and install.
2. With every change to the code: the simulation next to the tests and
   the sanitizers, with `sim_strict: yes` or `linux` once its scripts
   have been through a run as they are.
3. Done on 2026-11-19: the sanitizers on `f563e7f8` (run 233), and the
   two commits which are not about the device are in the merge request,
   with its description and HWTEST.md saying what they are for.
4. The maintenance branches, see "The maintenance branches": the work is
   on 7.2 and 7.1 as far as it goes there without KVM. Both build and
   their tests pass (runs 238 and 246), also with the sanitizers (runs
   262 and 263). Nothing is left of this item.
4a. Whether the hosts this is meant for can run VMs with KVM, or only
   with the VirtualBox kernel driver: not answered yet. Since 2026-11-20
   the work goes on as if they can, on the word of who this is for and
   until the answer is there (see the decisions). The branches with the
   KVM backend are what it goes on with then.
4b. The branches with the KVM backend of Cyberus Technology: 7.2 runs the
   simulation with the device (run 248), and so does 7.1, with all
   checks of the Linux guest (run 251). On both every check passes with
   the change to the backend about the Hyper-V registers in a saved
   state (runs 253 and 254), on 7.1 with what was taken over from 7.2.
   Done since: `backports-7.1` builds (run 256), the 7.1 one has run
   with the NAT library and a VM with a network (run 258), and the
   three branches have their merge requests in the fork (#2, #3, #4).
   Run 259 was the first with the size check in the ACPI device, with
   the testcases of the host services, with an appliance exported and
   imported, and with Windows in a VM with a TPM 2.0: all 172 checks
   pass, and run 260 built the rewritten testcase of the guest control
   service and ran 16 testcases of the tree, which all pass. Run 261 had
   the same checks on the branch for 7.2, on an AMD processor, which the
   VMs with a device had not run on since the changes about the
   register: all 172 pass. Runs 262 and 263 had the sanitizers on the
   branches without the backend. Nothing is left of this item which a
   run of the workflow can show; what is left is in items 1 and 5.
4c. A device with more than one function in the simulation, since
   2026-11-21: what this work is for is a graphics card, which is a
   device of several functions, and the simulation had a device of one.
   A part of its own (`mfhost`) has two NVMe controllers at one address
   behind the root port, in one IOMMU group, and goes through both
   functions passed through together, the device of two functions in
   the slot (taken out with the guest asked, put in by naming either
   function, taken by force), a start without it, a device of which one
   function is not to be had, a saved state, and the host taking both
   functions back. Its first run, 264 on the branch for 7.2 with the KVM
   backend, passes all of it: 59 checks, and so does run 265 on the
   branch for 7.1, which also has Windows with such a device (part
   `mfwinhost`, 41 checks, all pass). A phase about functions which the
   host cannot reset by themselves came with run 265 and showed what
   the root port of the simulated host does not do; with the port
   changed, run 266 on the branch for 7.2 passes all of it, 284 checks:
   the reset of the bus with the Linux guest, and Windows with a device
   of two functions which can only be reset together, and run 267 on
   the branch of the merge request passes the part with the Linux
   guest, 169 checks in all, and run 268 on the branch for 7.1 has the
   reset of the bus passing with the Linux guest. Not done: Windows with
   functions which can only be reset together on the branch for 7.1,
   where Windows did not get through its start in run 268 (see 4d).
4d. Windows which does not get through its start in the VM inside the
   virtual machine standing in for a host, on AMD processors: once in
   run 265, three times out of three in run 268 (the branch for 7.1
   with the KVM backend). Run 268 measured what goes on: the guest
   leaves for the kernel 6300 times a second for 380 timer interrupts
   a second, which at that depth of virtualization is all the time
   there is. That is the simulation, not the device. Open: why a start
   gets through on such a processor sometimes (twice so far), and what
   the numbers are for a start which works; every start tells them from
   here on. Should the stop of run 251 come again, the same counters
   are read there.
5. Windows where the drive is: passed in runs 250, 253, 254, 258, 259,
   261 and 266 (and with the device of two functions in runs 265 and
   266) and stopped after the first removal in run 251, on the
   branches with the backend of Cyberus Technology; run 261 had an AMD
   processor, the others Intel ones. Every run on such a
   branch goes through it again and tells what the VM shows of the guest
   when it stops; until it is understood, the hardware test is where a
   Windows guest letting go of a device counts.
6. The two slow VMs of run 231: every VM reports its pace now; the next
   slow one is looked at.
4e. INTx (WP8), see "INTx (WP8)" above: done on 2026-11-22. It is on
   every branch of a merge request about the passthrough work, read by
   a second reader, built and tested on all three lines, and run in the
   simulation with a Linux guest and with Windows, neither using MSI
   (runs 269 to 283). Left for the hardware: a real device with INTx.
   Not planned: seeing the new test fail on the device as it was.
4g. Machines with real hardware, see the section of that name: waiting
   for a first inventory on `hw-results`. What can be prepared without
   it is there: the build to fetch and the scripts up to a first test.
   After a first result: a test with a guest which has a driver for the
   card, and a build with emulated 3D for the tests of that.
4f. Requests from the tracker of upstream, see the section of that
   name: 798 and the manual text for 674 and 904 are merge requests,
   and so are 944 (the setup of the firmware) and the report 996 about
   the manual.
   What could follow, each small and to be tested without a graphics
   card: other requests about `VBoxManage` and about settings. The two
   large ones about 3D are not planned.
7. Not planned for now: T3 (see the test tiers), events in the API for a
   device coming and going, and a device pulled without notice in the
   simulation, which the port it emulates cannot do. A device error in
   the simulation is done since 2026-11-24, with a device of one function
   and of two, see "An error of the device, in the simulation", and so
   is the device leaving the host and coming back by the host's own
   hot-plug, see "The device leaves the host and comes back, in the
   simulation". Both have run with a device of two functions and with
   Windows as the guest (runs 336, 337 and 343), and the unplugging
   on all three lines (runs 342, 344 and 345). A device which is
   plugged in again after an error is left for the hardware: no port
   of the emulator the runner has takes one then (runs 345 and 346).
   The part with the drive behind a switch (`swplughost`) passes since
   run 348, is a default one, has passed on all three lines (runs 348,
   350 and 352) and with the other nine parts in one run (run 349).
   The commit which has the testcase wait for the slot is on all five
   branches (runs 352 to 354). Nothing of this is left for the
   simulation; the plugging in after an error is for the hardware.
7a. From the second reading of 2026-11-25, see "A second reader for
   four later commits": the device waits with the PCI lock where a
   function goes away for the guest, and lets go of the lock of its slot
   before it tells the VM that a live state is refused, on all five
   branches. Left, and not urgent: with the KVM backend of upstream, the EMT for the timers
   could be notified at the first tick at which the flag is found set
   and not at the second, which halves a delay of 10 to 20 milliseconds
   for timers another EMT found expired. That is a change to the timer
   code and wants runs with guests of several CPUs on that backend; the
   branches for use have another backend, which does not go that way.
7b. From the reading of Main and the root port of 2026-11-25, see "Who
   waits for whom, in Main and in the root port": two changes to Main,
   which is code of upstream, on all five branches, after which the VM
   process ends and VirtualBox knows that the VM is off when a frontend
   is told to end with a device of the host in the VM. Left, and listed
   there: what the readers found and what was not changed. The
   simulation with these changes ran on the development line (run 381),
   and both things were shown with upstream's code alone (run 380) and
   told to upstream (issue 1009). Not done: the simulation with these
   changes on the 7.1 line.
8. Done on 2026-11-24: a register of a function which went out of
   service a moment ago is answered from what is kept of the function,
   and no line is written, see "The device does not ask a function
   which is out of service" under "VMs with the sanitizers". The build
   for a machine with real hardware has the change (run 321), and the
   simulation has fetched and used that build (run 322). Nothing is
   left of this.

## Decisions

| Date | Decision | Reason |
|------|----------|--------|
| 2026-11-11 | No pull request against upstream; merge request inside the fork plus an upstream issue | Upstream requires a signed contributor agreement |
| 2026-11-11 | IOMMU is mandatory, no-IOMMU mode is rejected | Without it a device can reach all host memory |
| 2026-11-11 | Hot-plug slot is bound to a device layout known at VM start | VMM cannot create memory or I/O ranges at run time |
| 2026-11-11 | Builds and tests run on the fork's GitHub Actions | No local Linux build environment |
| 2026-11-11 | Planning files and CI definitions live on `meta`, code on its own branch | Keeps the merge request free of unrelated files |
| 2026-11-11 | Own testcase binary with the real bus code linked in, instead of the generic `tstDevice` framework | Needs a fake kernel interface inside the process; the generic framework loads the real device module and is off by default |
| 2026-11-11 | CI builds the tree without testcases and then only the Devices testcases; build output of the upstream base is cached | Short runs, independent of unrelated testcases |
| 2026-11-11 | Every fix comes with a test that was seen failing against the unmodified device | Evidence that the test hits the defect |
| 2026-11-11 | The code branch is only ever fast-forwarded; no history rewriting on published branches until the merge request is prepared | Keeps the public history stable |
| 2026-11-11 | INTx stays armed as upstream does although it is not functional yet | Scope; handled in WP8 |
| 2026-11-11 | The MSI capability is emulated in the device instead of using the MSI support of the PCI bus | The bus code knows one capability layout only; the guest has to see the layout of the device |
| 2026-11-11 | The device requires the native execution engine (KVM) | Guest memory has to stay at a fixed place in the VM process while a device can access it; only this engine guarantees that |
| 2026-11-11 | Guest memory is mapped for the device when the VM is constructed, not when the guest enables bus mastering | A failure stops the VM start with a clear error instead of leaving a device which half works |
| 2026-11-11 | A device which does not answer never produces an error status in a guest access path | Same behaviour as an absent device; the guest cannot be blamed and the VM must keep running |
| 2026-11-12 | Trapped BAR accesses go through the device file, the mapping of a BAR is only used for mapping it into the guest | The host kernel invalidates the mapping at times; a file access fails cleanly where touching the mapping raises a signal |
| 2026-11-12 | A function which cannot be reset on its own is reset through its bus; without any reset it is left disabled | Devices with several functions usually have no function reset; state must not carry over a guest reboot unnoticed |
| 2026-11-12 | The fuzz test is part of the testcase binary and generates device models itself instead of mutating bytes | Most random bytes are refused early; made-up models which are valid most of the time reach the code behind the checks. Deterministic by seed, so a failure can be repeated |
| 2026-11-12 | Tests are checked against deliberate defects now and then | A test which cannot fail proves nothing |
| 2026-11-12 | The notifications of the host are watched by a thread of the device's own, not a PDM thread | PDM threads stand still while the VM is paused; the host waits for the device to be let go of |
| 2026-11-12 | Device memory mapped into the guest is replaced in place by ordinary memory when the device goes away; the file descriptor is replaced in place too | Everybody holding the address or the descriptor number stays valid without locking, and nothing can touch the device afterwards |
| 2026-11-12 | A function with an error the host cannot recover from is let go of, the VM keeps running | The alternative, stopping the VM, takes down everything else the guest does; the guest sees a device which failed |
| 2026-11-12 | The hot-plug slot has a power controller, an attention button, both indicators and is surprise capable; commands complete at once | This is the slot of the root ports of other virtual machines, which Windows and Linux guests are known to handle; a Windows guest cannot be tried before the hardware test |
| 2026-11-12 | Slot events are signalled by MSI only, the root port gets no interrupt pin | Allowed by the specification; passthrough depends on MSI anyway; no new interrupt routing |
| 2026-11-12 | Only root ports configured with `HotPlug` get the strict access attributes for the PCI express capability | The guest takes control of the capability with native hot-plug; all other bridges and with them all existing VMs stay as they are |
| 2026-11-12 | The port itself keeps the guest from what is behind it while the link is down | Does not depend on the device behind the port doing it right |
| 2026-11-12 | The slot adds nothing to the saved state; after loading, the port compares what is in the slot with what the guest saw last | The config space is all the guest knows; a device which was put in or taken out while the state was on disk is an ordinary hot-plug event |
| 2026-11-12 | `_OSC` grants native hot-plug, PME, AER and the capability structure together, and only with a setting of its own; the root bridge is `PNP0A08` then | Same as in other virtual machines Windows guests do native hot-plug with; VMs without the setting keep their ACPI behaviour |
| 2026-11-12 | The ACPI table is checked in CI by executing it with the ACPICA tools | The table is the one part of WP5 the device testcase cannot reach |
| 2026-11-12 | Before a device in a hot-plug slot is taken away the guest is asked by the attention button and gets a bounded time; then the device goes anyway if the host waits for it or the caller insists | The guest can stop using the device in an orderly way; a guest which does not react must not keep the host or the user waiting for ever |
| 2026-11-12 | The attention button is not pressed again within ten seconds | Guests take a second press within five seconds for the request being taken back, and a press on a slot which is off for a request to switch it on |
| 2026-11-12 | In a hot-plug slot the whole device goes when the host wants one function back or reports an error of one | A slot holds one device; the guest cannot be told about a part of it |
| 2026-11-12 | A device which no longer answers when the host wants it back is taken out of the slot at once | It was unplugged; there is nothing the guest could do in an orderly way |
| 2026-11-12 | The link to the slot going down resets the device | The guest expects a device in its reset state when it switches the slot on again, as after a power cycle |
| 2026-11-12 | A device put into the slot has to match the device the VM was started with in BARs, place of the MSI-X table, ROM size and interrupts | The VM is built around these at construction and cannot be rebuilt while it runs |
| 2026-11-12 | Putting a device into the slot is done on an EMT, with the functions out of service until everything is in place | Nothing of the VM can get in the way, and the paths of the guest stay closed by the means which closed them when the device went |
| 2026-11-12 | The device file of a device put into the slot takes the number of the stand-in | Everybody holding the number stays valid, as when the device went |
| 2026-11-12 | With the legacy interface the group stays with the VM while the slot is empty | The guest memory mapping belongs to the container and would be lost with the group; the current interface (IOMMUFD) has no such tie |
| 2026-11-12 | A VM which starts without the device takes what it is built from out of its configuration, in the form a VM started with the device reports | The exact view VFIO has of the device, not one derived from elsewhere; a device which does not match is refused when it is put in |
| 2026-11-12 | A device which is there when the VM starts is what counts, a configured layout is ignored then | The layout in the configuration may be out of date; the management stores a fresh one |
| 2026-11-12 | Starting without the device needs the hot-plug slot, and all functions of the device are there or none | Without the slot the guest could not be told when the device arrives; a slot holds one device |
| 2026-11-12 | The state of the VM is saved only while the hot-plug slot is empty; a device which is there is taken out of the slot before a state is loaded and put back afterwards | PGM reads all MMIO2 pages when saving and writes to them when loading, and the BARs mapped into the guest are such pages; this must never be the memory of a device |
| 2026-11-12 | Before a state is saved or loaded the config space of the functions of the empty slot has nothing of a device, the vendor ID reads 0xffff | The PCI bus saves the config space and refuses a state with another vendor ID; a VM started with the device and one started without it have to look the same |
| 2026-11-12 | The saved state unit of the device holds the layout of each function and nothing else; a state with another layout is refused | The VM is built around the layout; the PCI bus and PGM notice only some of the differences |
| 2026-11-12 | A device is put into the slot only in the VM states running and suspended | The VM state changes before the saved state callbacks run, which keeps the slot empty for as long as a state is saved or loaded |
| 2026-11-12 | One critical section serializes putting a device in, taking functions out of service, the EMT part of a removal and the saved state callbacks | Several EMTs and the watcher thread can be at these at the same time |
| 2026-11-12 | PGM leaves MMIO2 pages alone which are zero when a state is loaded | Writing zeros to the memory keeping the place of the BARs makes the host back it, which can be many gigabytes nobody needs |
| 2026-11-12 | The mock saves and loads states as a stream of typed items, with a unit standing in for PGM which writes to every MMIO2 page | A get which does not match its put shows, and so does a load which reaches device memory |
| 2026-11-12 | The management gets the layout of a function as values under the names they have in the configuration and stores them as they are | Nothing outside the device has to know what a layout consists of, and nothing has to change there when it gets another value |
| 2026-11-12 | A random test for the device in the slot with checks after every operation, next to the tests for single cases | The cases nobody thought of are sequences; the first run with interrupts in it found a defect the driver has had all along |
| 2026-11-13 | The attention button of the slot is pressed along with a device being put in when that does not bring the link up | A guest with a slot which has a button need not ask for presence changes, and Linux does not; other virtual machines press the button on insertion for the same reason. With the power of the slot on, the link coming up tells the guest and the button stays alone, as a press on a slot which is on asks for the opposite |
| 2026-11-13 | The slot keeps its attention button | Without it there is no way to ask the guest to let go of a device in an orderly way |
| 2026-11-13 | The secondary bus reset of the root port takes the link to the slot down | A guest which resets a device this way goes on as if the device had been reset; with the link going down it is |
| 2026-11-13 | INTx is raised on an EMT and lowered when the interrupt setup it came from ends or the device goes | The thread waiting for interrupts must not wait for a lock whose holder waits for it; a line raised by a device which is gone or uses another interrupt must not stay raised |
| 2026-11-13 | A host which has fewer interrupt vectors than asked for: the device takes what there is | The alternative is a device without any interrupt; guests mostly use the first vectors |
| 2026-11-13 | A PCI device of the host in a hot-plug slot has to be alone on its guest bus, checked when the VM starts | Everything on the bus of the slot comes and goes for the guest with the device; an emulated controller there would vanish with it |
| 2026-11-15 | Few workflow runs: one per batch of work, the whole log of a failed run is dealt with before the next one, sanitizer and full builds only for candidates for the merge request, no runs which fail on purpose | GitHub had disabled the workflows of the fork. A run which is to show that a test catches a defect is started with `expect: fail` and is green when the testcase fails; deliberate defects on throwaway branches are no longer run one by one |
| 2026-11-15 | The simulation tiers T3 and T4 are not run; the merge request goes to the hardware test without them | T3 does not fit a driver which takes PCI devices behind an IOMMU only. T4 would take many trial runs to set up, which is what the workflows of the fork were disabled for; its first finding would be what part G of the hardware test shows |
| 2026-11-15 | No build of the head from nothing | The `tests` flavor builds the tree on top of the cached build of `main` and compiles every file which differs; a build from nothing costs a long run and would tell the same |
| 2026-11-15 | A failed build of the tree does not end a `tests` run at once: the device testcases are built and run with what was built, and the run fails at its end | One run tells about the tree and about the testcase, where it took two before |
| 2026-11-16 | The system simulation (T4) is set up after all, which takes back the decision of 2026-11-15 for T4 | Everything else which was verified is compiled code and a testcase against a kernel interface written by the same hand; only the simulation shows the VMM, the kernel, a guest, Main and `VBoxManage` working together before hardware does. The cost in runs is kept down: the scripts are written in one go, a run which is for finding out succeeds whatever it finds, and each run is read completely before the next |
| 2026-11-17 | A reset which the host refuses because it is busy with the device is not followed by a reset of the bus, and neither that nor a write to a function which has just gone out of service is logged as a failure of the device | Seen in the simulation with a real kernel: the host has a device locked while it waits to get it back, and the guest letting go of the device resets it at that very moment. The bus reset would be refused for the same reason; the host resets the device when it has it. The log is what a person testing on hardware reads |
| 2026-11-17 | In the simulation the VM inside the virtual machine standing in for the host has the timers VirtualBox emulates at a twentieth of their speed, and its guest kernel keeps quiet on the serial port | Run 220: the guest of VirtualBox is at the third level of hardware virtualization at least there, an exit from it takes a millisecond, and its kernel does not get past its timer tick. Run 221: with the slow timers it does, and what follows runs at its normal speed. Emulating the host in software instead was tried in the same run and does not get VirtualBox to run a guest |
| 2026-11-16 | The simulated host has an NVMe drive as its device, the guest is a kernel with a script | A drive shows memory access by the device and interrupts in both directions with a checksum, and its driver is in every kernel. A Windows guest cannot be had in a workflow (taken back on 2026-11-17, see the next line) |
| 2026-11-17 | A Windows guest in the workflow: Windows Server from the evaluation image Microsoft offers for download, installed unattended into a VM with the empty hot-plug capable slot (flavor `winsim`). The image is fetched for the run and not kept; the VM has no network adapter | Windows is the guest the work is for and the one guest nothing had been tried with. The slot, the root bridge and the firmware tables were made after what Windows is known to handle elsewhere, which is not the same as Windows having handled them. The evaluation image is for trying things out, and this is that |
| 2026-11-17 | What a guest made of the slot is also read from the outside: the slot registers through the debug interface of the VM | It does not depend on what the guest tells, so it is the same check for Linux and Windows, and it is the one sign that the hot-plug driver of Windows runs the slot which can be had without a device |
| 2026-11-18 | A VM which stands still because the thread of its CPU missed the notification of a timer is dealt with here: the timer callback notifies again (a commit of its own, Linux hosts only), and the Windows part of the simulation watches for it | It is not part of PCI passthrough. But a guest with a device of the host runs on KVM, where this is, the guest the work is for is Windows, which ran into it, and a person testing on hardware would take a guest which stops for one the device broke. The smallest change which bounds it was chosen. Closing the window in the KVM backend takes one of the two ways KVM has for it (`immediate_exit`, or the signal blocked outside `KVM_RUN`); both change the loop every exit of every guest goes through, and nothing but the simulation is there for trying that |
| 2026-11-19 | The Windows guest of the simulation gets no paravirtualization interface (`--paravirt-provider none`) | With KVM the default for a Windows guest type ends as the minimal interface, with which Windows keeps its time by the HPET and takes seven times as long (run 228 against run 226). In the virtual machine standing in for a host, where an exit costs a millisecond, Windows does not get through a start that way. Run 228 stays as what is known about the default |
| 2026-11-19 | Level-triggered interrupts which stop after a saved state was loaded, with KVM, are dealt with here as well: the I/O APIC tells the APIC about the loaded entries (a commit of its own, Linux hosts with KVM only) | As with the timer: not part of PCI passthrough, but in the way of the guest the work is for, on the engine the work needs, and a check of the simulation fails because of it. A VM with an empty slot can be saved and loaded, which is part of this work, and its guest should have a power button afterwards |
| 2026-11-19 | Windows where the drive is (the fourth part of the simulation) runs on runners with an AMD processor only | Two runs on Intel processors spent 70 minutes each on a Windows which did not get through its start at that depth, with either paravirtualization setting. What is known of AMD is one run on the runner itself (226) |
| 2026-11-19 | The machine standing in for a host tells of an invariant time stamp counter when its guest is Windows; the one for the Linux guest stays as it is | Run 230: without it Windows keeps its time by the HPET and does not get through a start there. A real host tells of the counter, so the machine was the odd one out. The part with the Linux guest has passed as it is and is what a run fails for, so it is not changed along the way |
| 2026-11-19 | One run for the Linux guest and Windows (flavor `allsim`); with `sim_strict: linux` it fails for a check with the Linux guest only | Fewer runs: one build instead of two. The parts with Windows are still for finding out, and one of them depends on a download and on the processor the runner happens to have; a run which fails for those would tell nothing about the code |
| 2026-11-19 | In the virtual machine standing in for a host the checks wait three times as long for the guest, and a VM which is slow is asked why instead of being failed for it | Run 231 failed for four waits in two VMs which were slow for a reason not yet known, with every check about what the code does passing. A check is about what happens, not how fast, at a depth of virtualization no user has. What made the VMs slow is still looked for: each VM reports its pace now |
| 2026-11-19 | After a change to the scripts of the simulation the next run is for finding out (`sim_strict: no`), also when it is to show something about the code. What it shows is in its list of checks either way | Run 231 was the first to fail by the simulation itself, on the first run of changed scripts and a new flavor. A failed run tells GitHub and everybody looking at the fork that the code is broken, which was not what it found |
| 2026-11-19 | The work goes on with what comes after the hardware test, without waiting for it. Nothing is recorded as tested on hardware. More runs than usual on days of such work, still one after the other and each read before the next | The person this is for has time for development now and will not have it later. What a positive hardware test would leave to do does not depend on its result: the code has to be had on a line of VirtualBox which is released and maintained, and the development line it was written on is neither |
| 2026-11-19 | The maintenance branch 7.2 first, 7.1 after it if at all | 7.2 has the passthrough device already, 7.1 has not; what 7.2 lacks of upstream is a list of commits, what 7.1 lacks is the device and what it needs in the VMM. Everything done for 7.2 is on the way to 7.1 |
| 2026-11-20 | The first build of a branch or a base nothing is known about yet is started with `explore: yes`: the run counts as passed whatever happens in it, and what happened is read from its log and written down here | Run 234, the first build of the 7.2 branch, failed in `configure` for options of the workflow. That is a finding about the workflow, and a failed run says the code is broken. The same thought as `sim_strict: no` for the simulation, one level up |
| 2026-11-20 | The KVM backend of the development line is not taken to the maintenance branches. The branches for 7.2 and 7.1 are kept as the work and its tests on those bases, building, and are not given an execution engine | What made that backend build and work is some forty changes to the VMM which upstream has not released. A maintenance branch with them is no longer the released line somebody chose it for, and nothing here can stand in for the testing upstream gives its VMM |
| 2026-11-20 | Running the device on the VirtualBox kernel driver, which is what the maintenance branches offer, is not started before it is known that it is wanted | It gives up a security requirement as it stands (S11) for another way of meeting what it is for, in the memory management of VirtualBox, and it takes a simulation with the kernel driver loaded. That is weeks of the kind of work the last ones were, and the right thing only if the hosts this is for cannot have KVM |
| 2026-11-20 | No further runs on 2026-11-20 unless asked for. Before the first run on a base which is new to the workflow, its `configure`, its checkout and its makefiles are read for what the workflow assumes, and all of it is put right in one go | Ten runs in one morning, six of them stopped by the workflow itself within two minutes, each for one thing which reading the scripts of 7.2 and 7.1 would have shown: the options of the older `configure`, the translations, files a branch deletes, kBuild without `.gitmodules`, an option only 7.2 has, the place of kBuild. One run per finding is what was not to happen again after 2026-11-15, and it did |
| 2026-11-20 | The KVM backend Cyberus Technology publishes for VirtualBox is put onto branches of their own (`pci-passthrough-hotplug-7.2-kvm`, `-7.1-kvm`), as one commit which is their patch as it is, and whatever this work changes in it comes as commits after that one | It is the one KVM backend there is for the released lines, it is maintained by somebody else, and it gives the device what it asks of an engine. Kept apart, it can be replaced by a newer version of theirs, and what is theirs and what is not stays plain to see. The branches without it remain what the merge request would be against upstream's maintenance branches |
| 2026-11-20 | On a build with that backend the Windows guest of the simulation is left at its default paravirtualization interface (Hyper-V), and Windows where the drive is is tried on every processor, not on AMD only | Run 248: without the interface Windows keeps its time by the HPET on that backend and does not get through setup on an Intel processor. The backend gives a guest the interface through KVM, and a VM somebody creates for Windows has it. The reason for trying AMD only was that clock |
| 2026-11-20 | The change to that backend after run 247 (a register handed to the kernel for a guest processor which has it only) stays on the branches although run 248 could not show it is what was missing; no run is started for the sake of getting an AMD processor | It is right by itself: a guest processor without the register has nothing to hand over. Which processor a runner has cannot be chosen, and every run on these branches tells when it has one. Should the VMs stop there again, the log now names the call. (They did, in run 249, and it did: see the next line) |
| 2026-11-20 | Whether the register is handed to the kernel is decided by asking the kernel once when the VM is set up, not by the features of the guest processor | Run 249: the kernel of a host can offer guests a feature which is in the register and still not take the register, so nothing which is known about the guest tells. Asking keeps the backend as it is published on every host where the kernel takes the register, which the first change did not |
| 2026-11-20 | A Windows guest which does not go on after a saved state was loaded, with the Hyper-V interface on that backend, is written down as found and not looked into further for now. The simulation switches such a VM off, starts Windows once more and goes on | It is the backend's handling of that interface across a saved state, in a version of the backend which is a year old, and has nothing to do with the slot: the Linux guest and Windows without the interface go through the same round. The parts of the simulation which come after it need a Windows which was shut down properly |
| 2026-11-20 | On the branches for 7.1 `Console::i_saveState` returns its result, which is one line taken from the lines after 7.1 and the one change there to code which is not about the device or the engine | Run 250: the device refuses to have the state of the VM saved while it is in the VM, and on 7.1 a save which does not work out leaves the machine in a state nothing gets it out of, and a snapshot of a running VM without its state. That is how 7.1 is with any failed save; with this work a failed save is something a user meets, so the work has to bring what makes it end well |
| 2026-11-20 | The simulation tries a snapshot of the running VM with the device in it next to the saved state, and it ends the process of a VM which does not let itself be powered off | The snapshot goes the same way through the device and another way through Main, and that other way is where 7.1 differed. A VM stuck in a state cost the three phases after it in run 250, which were the ones the run was for |
| 2026-11-20 | The work goes on under the assumption that the hosts it is meant for can run VMs with KVM. Running the device on the VirtualBox kernel driver stays unstarted, and the branches with the KVM backend of Cyberus Technology are the ones the next steps are for | Who this is for has no answer to that question yet and said to assume it. It is an assumption: should the answer be the other one, what was done for these branches stays right for hosts with KVM, and the decision of the same day about the kernel driver is taken up again |
| 2026-11-20 | The Windows guest which does not go on after a saved state is loaded, on the KVM backend of Cyberus Technology, is looked into after all, which takes back the decision of the same day | Reading the backend showed where it comes from in a few lines: it saves and loads the registers of the Hyper-V interface as the first virtual CPU has them, and the reference time starts anew in the VM a state is loaded into. A VM with an empty slot can be saved and loaded, which is part of this work, and Windows is the guest it is for |
| 2026-11-20 | The branches for 7.1 take over from the 7.2 branch what upstream put there and not on 7.1, as far as it is about code a Linux host runs, applies, and can be built here; what was not taken is listed with the reason | 7.1 is the line this was asked for, upstream has ended its support, and its security updates since April 2026 are for 7.2 only. Nobody outside upstream can make 7.1 the equal of 7.2 in that, and the notes say so; what can be had by taking over single changes is had |
| 2026-11-20 | The reworks which 7.2 has from 2025 and 7.1 never got (the buffers of USB requests, the command buffer interface of the TPM with the change to the VMM it needs, the E1000, the 3D graphics) are not ported to the branches for 7.1 | They are hundreds of lines each, depend on each other and on the VMM of 7.2, upstream itself did not take them to 7.1 while it supported it, and nothing here could show that a port of them is right. The notes name them as what separates 7.1 from 7.2, and who can choose takes 7.2 |
| 2026-11-20 | The branches for 7.1 are built without the 2D video acceleration for guests (`VBOX_WITH_VIDEOHWACCEL` empty in `LocalConfig.kmk`) | The lines after 7.1 no longer have that code at all, a guest with a current Windows does not use it, and leaving it out is a switch of the build and no change to the source. Corrected after run 259: a headless build, which is what is built here, never had it; the switch is for a build with the user interface |
| 2026-11-20 | The testcase of the guest control service, which cannot run as upstream has it, is rewritten on the branches for 7.1 instead of being taken off the list; and a testcase which is new to the workflow is tried for a run before it counts | The service is one of those the changes taken over from 7.2 touch, and nothing else here goes through it: no guest of the simulation has the Guest Additions. A testcase nobody has seen passing here may fail for the runner and not for the code, and a run which fails for that says the wrong thing |
| 2026-11-21 | The simulation gets a device with two functions, as a part of its own next to the one with a device of one function, which stays as it is | A graphics card in an enclosure is a device with several functions which the host has in one IOMMU group, and that is what this work is for. Several functions were gone through by the testcase with its made-up kernel interface only; a real kernel, Main, `VBoxManage` and a guest had not seen such a device. It is the largest thing left which can be shown without hardware. The part with one function stays unchanged so that a failure in the new part does not take the old checks with it |
| 2026-11-21 | The machine of the simulation which has the device of two functions gets a root port for 2.5 GT/s, which does not claim to report whether its link is up; the machine with the device of one function keeps the port it had | The port QEMU emulates by default claims it and never reports the link up, so the kernel calls every reset of the bus behind it failed after doing it (run 265). A port of a real host reports its link. Without the change the reset of the bus, which is the reset a graphics card gets, can only be seen failing; with it the kernel does what it does on hardware. The other machine does not reset a bus and stays as the runs so far had it |
| 2026-11-21 | Windows with the device of two functions has functions which the host cannot reset by themselves, from the start of its VM | That is the device this work is for: a graphics card and its audio function are mostly reset together, by the reset of their bus, and Windows switching the slot off is such a reset. The device of one function with Windows, and the device of two functions with the Linux guest in its other phases, keep the reset of a function |
| 2026-11-21 | A Windows which is stuck early in its start is given up on after 15 minutes without a read from its disk, looked at, and started once more; the first start counts as failed | Run 265 waited an hour for such a start and learned little from it, and everything the part is for comes after the start. A second start which works tells that the first was chance and lets the checks about the device run; a second which is stuck too tells the opposite. The failure stays in the count either way |
| 2026-11-22 | WP8 is taken up: a function which uses INTx gets every interrupt through to the guest. Each interrupt is a pulse on the line of the guest, and the device is asked for its next one after a time which grows while it keeps answering with one | Reading the device showed that the first INTx interrupt left the line of the guest raised for good, which is worse than the notes had it, and the audio function of a graphics card in a Windows guest is a device which uses INTx. The way taken stays inside the device, where the testcase goes through every step of it; a callback from the I/O APIC or trapping the memory of the device would have changed parts which every VM uses and which nothing here can test alone |
| 2026-11-22 | The longest time between two questions to a device about its INTx interrupt is 2 milliseconds, not the 8 of the first version | It is the pace of a device with many interrupts as much as the bound for one nobody deals with. 500 pulses a second cost little on a host which is not a virtual machine; 125 interrupts a second would have been little for a graphics card whose driver uses INTx. What the right number is can only be told with such a device |
| 2026-11-22 | The INTx change was read by somebody who had not written it before it goes to the branches of the merge requests, and what that found is a commit of its own on the working branch | The testcase and one phase of the simulation were all that had seen the change, and whoever writes a test and the code it tests makes the same mistake in both. The reading found a check which could not fail and a request to a device which was gone |
| 2026-11-22 | Runs may overlap: a build of ten minutes next to a simulation, and two simulations at a time at most | One run at a time made a day of what three lines need, most of it waiting. The runs are no more for it |
| 2026-11-22 | Requests from the public tracker of upstream are taken up where they can be done and tested here, each as a branch from the development branch of upstream and a merge request inside the fork | Asked for by whom this work is for. The tracker has no priorities, so what is small and can be tested without a graphics card comes first. Nothing goes upstream from here |
| 2026-11-22 | The branches for 7.2 and 7.1 get the INTx change when a run has shown Windows using the line interrupt, not before | Run 278 had Windows read from the drives after its controllers were told not to use MSI, and could not tell which interrupt they had. That is what a Windows guest with the audio function of a graphics card comes to, so it is worth one more run |
| 2026-11-22 | The three changes of 7.2 to the execution engine in the kernel driver stay left out of the 7.1 branches | Looked at again: they can be tried here in principle, the runners have the hardware virtualization for a VM on the kernel driver of VirtualBox, but that takes a part of the simulation of its own, about a day of work and several runs. A host which runs VMs with KVM does not run that code. It is worth doing if the hosts this is meant for turn out not to use KVM |
| 2026-11-22 | What is to run on a machine with real hardware is scripts which need nobody to think, and their results come back through a branch of the fork | The machine which is offered for it has an assistant with little to spend and is not to be given a free hand. Scripts cost nothing to run, can be read before they are run, and what they find is worked out where the work is done. Only fields which were chosen leave the machine, as the fork is public |
| 2026-11-22 | A run which is about Windows two levels of virtualization deep is cancelled at its start when its runner has an AMD processor | Run 284 took 163 minutes to tell nothing: Windows does not get through its start there on such a runner, which is known since run 268 and is not about the device. A run which is cancelled in its first minute costs next to nothing |
| 2026-11-22 | The VirtualBox built by the workflow is published as a file of a pre-release of the fork, for machines with real hardware to fetch | Building on such a machine takes a toolchain, an hour and somebody who knows what to do when it fails; fetching takes a command. A file of a release can be fetched without an account, an artifact of a run cannot. It is named by the commit it was built from and says of itself that it is no release of Oracle |
| 2026-11-22 | A script which is to run on a machine with real hardware runs in the simulation first, with the drive in the place of the card | Its first run would otherwise be the one somebody has made time for. Run 287 found the first script ending at its first step |
| 2026-11-22 | The INTx change is not put on the branches of the merge requests before the simulation has run it with a guest | The branches are marked ready for the hardware test with what was verified on them. A change to how interrupts reach the guest which only the testcase has seen is not that, and the runs for it could not be started that day |
| 2026-11-22 | The simulation gets what a graphics card has and its drive has not, piece by piece: a device with four gigabytes of memory, a ROM, and the VGA adapter of an emulator as the device, with its legacy ranges | The hardware which is offered for a test is one machine and somebody's evening, and a path of the passthrough device which first runs with a guest there fails there. These had run with the stand-in for the kernel of the unit tests only, or not at all |
| 2026-11-23 | Every script which is to run on a machine with real hardware runs in the simulation, the fetching of the published build and the publishing of results too, the latter to a repository made for the run | The build which is fetched is the one such a machine gets, so it is the one the first test has to have passed with. Publishing is the one step which leaves the machine and has to be seen to send what it says it sends |
| 2026-11-23 | The parts with the device of four gigabytes and with the VGA adapter belong to every simulation from now on (`sim` and `allsim`) | Together they take ten minutes, and they are the only checks of large memory, of the ROM and of the legacy VGA ranges with a guest |
| 2026-11-23 | What takes root on a machine with real hardware is one command, `hw/linux/tests.sh` | Each command somebody has to type there can be mistyped or left out, and the person is not to have to know which comes next. The scripts it runs stay as they are and can be read and run one by one |
| 2026-11-23 | The issue which makes the work known to upstream is filed now and not after the test on hardware | Whom this work is for said yes to it and wants the code to be used. The defects of `main` it lists are of use to upstream whatever becomes of the rest, and the test on hardware has no date. That nothing ran on hardware is said there in so many words, with the promise to add the result |
| 2026-11-23 | The workflow can build with 3D acceleration for VMs, and the simulation has a VM with it, rendered in software on the runner | Whom this work is for asked for the requests and reports about 3D to be taken up, and none of them can be without a VM which has 3D acceleration. A software renderer is no graphics card, but the code of the graphics adapter which a guest reaches is the same with both |
| 2026-11-23 | The simulation can be run with a build which has the sanitizers in all of it, and what the address sanitizer finds in upstream's code is put right on a branch of its own | The testcase with the sanitizers covers the passthrough device against a stand-in for the kernel; this covers whatever a guest of the simulation reaches, with the kernel as it is. The first run found a defect of upstream within minutes, which is what such a run is for. A finding which is not about this work does not go into the branches of this work |
| 2026-11-23 | The workflow keeps the packages of the kernel the simulation runs from one run to the next | Fetching them took close to an hour in two runs of one day (308 and 311) and seconds in the others. A run which waits an hour for a mirror tells nothing in that hour, and a runner is not to be kept for it |
| 2026-11-23 | The build which a machine with real hardware fetches is the one with 3D acceleration | Run 302 had it go through the parts with a passed through device as the build without goes through them. One build for the passthrough and for VMs with 3D acceleration spares the person at the machine fetching twice and knowing which is in place. The build without stays published and can be named |
| 2026-11-23 | A build with 3D acceleration is asked for with a word among the arguments for kmk, not with an input of its own | The workflow has ten inputs, which was the most a workflow could have when it was written, and the word has to be there for kmk anyway |
| 2026-11-24 | From the moment a function is out of service until the guest is told, a read of a register the device passes on is answered from what is kept of the register, not by asking and not with ones | Asking reaches what stands in for the device, which has no answer; and the function is to stay as it was for the guest until the PCI bus has taken its mappings away, which is why reads are not all ones before that |
| 2026-11-24 | The simulation is not run again on the 7.1 line for that change | It is the same lines on every line. The testcase passes with it on 7.1 (run 318) and the whole simulation does on 7.2 (run 317); 50 minutes more would tell nothing new |
| 2026-11-24 | The simulation has the host report an error of the device, with the error made from the runner through the monitor of the emulated machine | What VirtualBox does about such an error was only tried against a kernel the testcase makes up. The kernel of the runners cannot make one up itself (run 326), the emulator can, and what it does is nearer to hardware. The script on the runner takes that one request and no other, so that the machine gets no way to its own emulator |
| 2026-11-24 | That part is one a run of the simulation has by default | 17 minutes of which most is the build; a change to how the device goes out of service is what it is there to catch |
| 2026-11-24 | Upstream is told of the defect in the event queue of Main, as a comment on the issue which is there already and not as an issue of its own | Who this work is for said yes to telling. The issue lists the defects of `main` which were found on the way, and this is one more of them; a second issue would be one more thing for upstream to sort |
| 2026-11-24 | A run which is about Windows in the machine standing in for a host is cancelled at once when its runner has an AMD processor, and started again | Run 332 took two and a half hours to show nothing new: on such runners Windows does not get through its start at that depth (item 4d). A run tells its processor in its first minute, and a cancelled run costs that minute |
| 2026-11-24 | Where the emulated port has lost the width of its link, the simulation does not plug the device in again, and says so in its result | Runs 340 to 345: once the emulated port was made to report an error it tells of a link without a width, and the kernel of the machine takes no device from it (the emulator overwrites the link capabilities of the port when it injects the error; no reset has to do with it). Nothing which can be done from inside that machine brings the width back. A check which can only fail tells nothing; one which is left out with its reason does |
| 2026-11-25 | The simulation gets a part with the device behind a switch (`swplughost`): the root port reports the error, the port of the switch has the slot | The emulator spoils the link of the port it reports an error for (QEMU 8.2.2, read in its source, fixed in its current one), so a device could not be plugged in again after an error. With a switch the slot is at another port, and it is how an enclosure on a cable is connected. No other emulator is built or installed on the runner for this: the runner keeps what its distribution has. Run 346: the port of that switch has no link width at all in this emulator, so the part is kept for what it can show, the errors and the unplugging with a switch in between, and the plugging in after an error is left for the hardware. A root port behind a root port would have a slot which works, and is not done: no machine is built like that |
| 2026-11-25 | The checklist script does not count a port the device is behind as another device in its IOMMU group; it says that the port is there | Run 346: a port of a switch without access control services is in the group of what is behind it. The kernel permits such a group and keeps its driver on the port, nothing of the port goes to the VM, and VirtualBox had the device. Another device in the group is counted as before, and so is a bridge which is not on the way to the device |
| 2026-11-25 | Before it unplugs, a phase clears a completed command which is pending at the port, and says so in the log | Runs 347 and 348: after the reset of everything below the root port the port of the emulated switch is left with that bit, and the kernel of the machine hears of nothing from the slot until it is cleared. It is a matter between that kernel and the emulator; VirtualBox is not part of it, and without this the part could not unplug after an error |
| 2026-11-25 | The part with the drive behind a switch is one a run of the simulation has by default | Run 348: it passes, in under five minutes. It is the only part in which the port which reports an error is not the one the device is in, a switch is reset with the device behind it, and the device is two buses down |
| 2026-11-25 | The testcase waits for the slot to be told that it is empty; the device keeps its order, letting go of the functions before the port is told | Run 351: the other order would have a guest hear of an empty slot while something of the device can still be reached. What failed was a check which took the two for one moment |
| 2026-11-25 | No new build for a machine with real hardware for that commit | It changes the testcase only; the build of `585a8166` is what the head has, but for the test |
| 2026-11-25 | The four commits which were written after the review round and had no reader who had not written them get one each, and every point is checked against the code before anything is changed | The decisions of 2026-11-13 and 2026-11-22 were kept for the INTx change and not for these four; two of them are in the device a guest talks to |
| 2026-11-25 | `pciVfioFunGoneOnEmt` takes the PCI lock, after the lock of the slot; a test comes with it which fails without it | A reader found it changing the command register and the mappings of the BARs while another EMT can be in a write of the guest which does the same. The order of the two locks is the one the device has elsewhere |
| 2026-11-25 | A reset which the host refuses because it is busy with the device is not tried again; the log no longer says that the host resets the device later | What holds the lock of a device for a moment only is rare, and the unbinding and the recovery from an error hold it for longer than a second try would wait. Whether the host resets a device it gets back is its matter and was never looked at here |
| 2026-11-25 | The EMT for the timers is still notified again at the second tick and not at the first | It would halve a delay which only the KVM backend of upstream has, in code every VM with that backend runs through; that wants runs of its own. Noted under "Next" |
| 2026-11-25 | The scripts for a machine with real hardware get a third test, in which the device leaves the guest and comes back while a driver of the guest has it; the one script for all runs it last | It is what the work is for, and no script did it with a guest whose driver has the device. Last, as a driver which does not take being left may leave the card in a state the other tests would trip over |
| 2026-11-25 | A new build for a machine with real hardware, of the head with the lock | The one which is published does not have it, and a machine there may have its device in the VM for good |
| 2026-11-25 | The commits which came of the second reading get a reader too, after the fact for the first three and before it goes to the branches for the fourth | They had gone to the branches without one, which the decisions of 2026-11-13 and 2026-11-22 do not allow for either. From here on no commit to the device or to the VMM goes to a branch of a merge request unread |
| 2026-11-25 | `pciVfioSaveCheck` lets go of the lock of the slot before the VM is told that its state cannot be saved; a test comes with it which fails without it | The thread which saves a live state waits for an EMT to do the telling, and an EMT with work of the device waits for that lock: found by the reader of the change before |
| 2026-11-25 | A second new build for a machine with real hardware on one day | The one of run 362 has the PCI lock and not this; a build which stands still at a live snapshot is not one to hand out when the next is twelve minutes away |
| 2026-11-25 | The simulation asks for a live snapshot with the device in the slot | The snapshot it had pauses the VM first and is refused on an EMT; the live one is refused by the thread which saves, which is where the VM could stand still |
| 2026-11-25 | Main and the root port are read for who waits for whom, as the device was | Both things found on that day were of that kind, and neither Main nor the port had been read that way |
| 2026-11-25 | `Console::i_releaseVMCaller()`, code of upstream, counts down also when the Console is being uninitialised | A command which takes a device out holds on to the VM for as long as the guest is given, and the VM process did not end when the frontend was told to end meanwhile (run 370). Changing how long the command holds on would only make the moment shorter |
| 2026-11-25 | The simulation gets a phase with a guest which uses no MSI and the device in the slot; that such a guest is not told of the slot stays as decided on 2026-11-12 | INTx through the port of the slot had run nowhere. The guests this is for use MSI for the port; a port which tells with INTx as well would be new interrupt routing for a guest nobody has |
| 2026-11-25 | Left as they are: the device being reset with the PCI lock held when the guest switches the slot off; a link which is down counting as the guest having let go; the message for a VM which was started paused | Each was looked at: the first costs the guest its own time, the second takes a device from a guest which was asked and whose slot can lose its device at any time, the third is a wrong word in a refusal |
| 2026-11-26 | `Console::uninit()`, code of upstream, tells the server that the VM is off when the state was left at "stopping" | The service kept a machine without a session in a busy state, and nothing took the VM any more (runs 371 and 374). It is where `VBoxHeadless` ends up whenever it is told to end with a device of the host in the VM |
| 2026-11-26 | That change is made in `uninit()` and not in the function which sets the state | A first version let that function through for every thread and every state while the object is being uninitialised. Its reader found that this reaches the undoing of a state save which failed, where a VM process could come to stand. The narrow one does one thing on one thread |
| 2026-11-26 | Left as they are, both older than this work and in code of upstream: the call to the server from the thread which closes the session can meet a setter of the server which waits for that session; and a session which is closed while a state save of a paused VM that failed is being undone leaves the machine paused without a session | The first needs settings of the VM changed in the moment its session closes, and the call which says "stopping" has the same; the second is what was there before, and the first version of the change made it worse |
| 2026-11-26 | A new build for a machine with real hardware, of the head with the two changes to Main | With the one before it, telling the frontend to end leaves a VM with a device which VirtualBox takes for busy; whoever tries the build on hardware ends a VM that way sooner or later |
| 2026-11-26 | Upstream is told of the two defects in Main in an issue of its own (1009), and only after they were shown with its code alone | Who this work is for said yes. Seen with a device of the host in the VM they could be taken for something of this work; the phase `endpaused` has them with a paused VM and nothing else, and run 380 with `main` as it is |
| 2026-11-26 | The two suspicions about the interrupt code of upstream were looked into, and upstream is told of them (issue 1008) as read and not tried | Who this work is for said yes to both. Both hold by reading, somebody who was not told what was suspected came to the same answers, and there is no host here to try the first one on |
| 2026-11-26 | Nothing is changed in the branches for those two | They are in `main` only; the lines for the target do not have the code. A change to the I/O APIC and the APIC of the development line would have no test which reaches it |
| 2026-11-26 | The phase `endpaused` runs in every simulation with the part on the runner, and a tree without the two changes to Main has what goes wrong told as skipped | It costs two or three starts of the guest. The branches for requests of the upstream tracker are `main` with one change each and do not have the two changes; their runs are not to fail for what is upstream's |
| 2026-11-23 | No second run is started for a branch and a flavor while one is in flight | The workflow cancels the older one then, which is what happened to run 298. Two runs of the same code at a time take two names for the commit |
| 2026-11-20 | The simulation gets checks for what is not part of the work and a VM for it needs all the same: a network behind NAT, an appliance exported and imported, a TPM 2.0 in the VM with Windows; and the workflow runs the testcases of the host services | The branches for 7.1 carry changes to these parts taken over from 7.2, which no check went through; and a Windows 11 guest, which is what the hot-plug is for, does not run without a TPM |
| 2026-11-13 | Unbuilt code is reviewed by independent readers and every point is checked against the code before anything is changed | No compiler and no test run are to be had for now; a second pair of eyes finds what the author no longer sees, and a reviewer can be wrong (one point was: the lock of a getter in Main) |

## Test tiers (see PLAN.md section 6)

| Tier | State | Commit | Note |
|------|-------|--------|------|
| T1 unit | running in CI, 56 tests, ACPI table check | `f563e7f8` | all pass with ASan, LSan and UBSan (run 233) and without them (run 232) |
| T2 fuzz | running in CI | `f563e7f8` | per run 2000 device models, 400000 operations on the root port and 40000 on the device in the slot; with the sanitizers 20000 models, 2000000 and 100000 operations (run 233); 200000 models with another seed and sanitizers (run 70, before WP3) |
| T3 kernel simulation | not run | | The driver takes PCI devices behind an IOMMU and refuses everything else; the sample devices of the kernel are not such devices, and the runners have no IOMMU. What T3 was to show of the kernel interface is what the hardware test shows |
| T4 system simulation | passes | `f563e7f8` | Run 232, 80 checks with the Linux guest and 27 with Windows on the runner, all pass (a run for finding out; run 231 before it failed for four waits of the simulation, see there). For the head before the last two commits: run 224, 79 checks, a run which fails if one of them does, with a Linux guest and an emulated NVMe drive behind an emulated IOMMU: passthrough with memory access by the device and MSI-X interrupts, pause and resume, the refused saved state, the hot-plug capable slot with `unplugpci` and `plugpci` (the guest being asked, and forced), the host asking for the device back, a start without the device, saved states with the empty slot. One device with one function. Windows as the guest so far with the empty slot only (run 226, 40 of 41 checks, not part of the strict run). See "Installation and the simulation" above. Since run 264, on the branch for 7.2 with the KVM backend of Cyberus Technology: a device with two functions in one IOMMU group, 59 checks with a Linux guest; the same on the head of the merge request in run 267 (169 checks in all, with the reset of the bus for functions which have no reset of their own), and with Windows on the branches for 7.2 and 7.1 (runs 265 and 266) |
| T5 hardware | not run | | needs a human with hardware, see [HWTEST.md](HWTEST.md) |

## Security requirements and what shows them

The requirements are in PLAN.md, section 4.

| | Shown by |
|---|----------|
| S1 | Test "Device without IOMMU, device which is not PCI". The interface the driver uses by default (IOMMUFD) has no mode without an IOMMU |
| S2 | The tests for MSI vector counts, the MSI-X table, capability lists and the layout from the configuration; the random test with made-up device models, with the sanitizers; "Interrupts with a host which is short of vectors". The fake kernel counts every request which is out of bounds, and every test checks that count |
| S3 | "Guest memory can not be made available to the device", also with the legacy interface |
| S4 | "The host takes the device back ...", "Hot-plug slot: the device is detached", "... interrupts of a device which was taken out", and after every operation of the random test of the slot: nothing of an absent device mapped into the guest or answering, the device file closed, no file descriptor more than before. The mapping of the guest memory is not removed with the device: it stays for the life of the VM (S11) and for the next device, while the device is taken out of the address space when it is let go of |
| S5 | The tests with a device which stops answering on every guest access path; the random tests; "INTx: raised while the guest changes the interrupt setup" for the one wait an EMT has, which is bounded |
| S6 | "Host address which is not a PCI address" |
| S7 | "I/O port BAR, device which stops answering", "Legacy VGA ranges ...", "Trapped BAR accesses do not need the mapping of the BAR", the removal tests |
| S8 | "Saved state is refused", "Hot-plug slot: saved states which are refused" |
| S9 | Review; part 1 of the hardware test says what the user of the VM needs |
| S10 | Runs 217 and 218; no compiler warning in a file this work changes |
| S11 | "Execution engines without fixed guest memory" |

## Ready for the hardware test

By the definition in PLAN.md, section 7:

1. WP0 to WP7 are done. WP8 (INTx), which was allowed to be missing, is
   done since 2026-11-22, another way than planned, see "INTx (WP8)".
2. T1 and T2 are green on the head of the merge request, with and without
   the sanitizers (runs 277 and 276, on `scratch/intx` with the tree the
   head has; runs 232 and 233 for the head before the INTx commit).
3. T4 passes on the head of the merge request (run 277, the same way;
   run 232 for the head before the INTx commit, run 224 for the one
   before the two commits ahead of that). T3 was not run; the table above
   says why.
4. The security requirements are mapped to tests, see above. S9 is a
   matter of the hardware test.
5. The tree builds without a compiler warning in a file this work changes.
   The manual and the man pages are updated; the manual itself is not
   built by the workflow.
6. HWTEST.md has the procedure, with what to expect and what to look at.

What the simulation has shown since this was written: a Linux guest
using the slot, Main and `VBoxManage` doing what they were written to do,
the kernel interface of a real kernel. And since run 226: Windows taking the root bridge,
the firmware tables and the empty slot; since run 250 Windows with a
device in the slot, on the branches with the KVM backend of Cyberus
Technology; since run 264 a device with two functions, with a Linux
guest, and since run 267 functions which the host can only reset
together; since run 327 an error of the device as the kernel of the
host reports it, and since run 333 the device leaving the host and
coming back by the host's own hot-plug, both with a Linux guest and
with Windows; since run 346 a device behind a switch, with a Linux
guest. What the hardware test is still the first to show: a
real device, a graphics card with its driver in the guest,
Thunderbolt, a cable which is pulled without the host being asked, an
IOMMU of AMD, and what a host does which is not a virtual machine.

## CI runs

| Run | Ref | Flavor | Result | Note |
|-----|-----|--------|--------|------|
| 1 to 4 | `main` | baseline | failed | workflow problems (kBuild bootstrap not needed, libstdc++ header mix on the runner, script errors) |
| 5 | `main` | baseline | passed | reference: 12.5 min build, 871 warnings |
| 6 | code branch | tests | failed | build needs the product revision, which fork commits do not carry; now passed in by the workflow |
| 7 | `main` | baseline | passed | fills the build cache |
| 8 | code branch @ `7a9aba7f` | tests | failed | one compile error in the harness |
| 9 | code branch @ `5ee30cb8` | tests | passed | harness green against the unmodified device, 100 s |
| 10 | `scratch/irq-vectors` | tests | failed as intended | MSI vector count test against the unmodified device |
| 11 to 14 | `scratch/irq-vectors` | tests | mixed | development of the interrupt rework |
| 15 | `scratch/red-evidence` | tests | failed as intended | all new tests against the unmodified device |
| 16 | code branch @ `eb637e18` | tests | passed | 7 tests |
| 17 | `scratch/msix-shared-bar` | tests | failed as intended | MSI-X shared BAR test before the fix |
| 18, 19 | `scratch/msix-shared-bar` | tests | passed | development of the MSI-X and BAR rework |
| 20 | `scratch/red-evidence-2` | tests | failed as intended | three new tests against the driver before the rework, 13 failed checks |
| 21 | code branch @ `f31b5dda` | tests | passed | 10 tests |
| 22, 24 | `scratch/caps` | tests | failed | development of the capability list handling |
| 23 | `scratch/red-evidence-3` | tests | failed as intended | small MSI capability test against the driver before the change |
| 25 | `scratch/caps` | tests | passed | 13 tests |
| 26 | `scratch/red-evidence-4` | tests | failed as intended | capability list tests against the driver before the change; ran into the time limit |
| 27 | code branch @ `33ba49dc` | tests | passed | 13 tests |
| 28 | code branch @ `05617ea1` (as `scratch/msi`) | tests | passed | 16 tests |
| 29 | `scratch/red-evidence-5` | tests | failed as intended | three new tests against the driver before the MSI emulation, 14 failed checks |
| 30 | code branch @ `05617ea1` | tests | passed | 16 tests; first run with the watchdog for hanging tests |
| 31 | `scratch/red-evidence-4` | tests | failed as intended | hanging test stopped after three minutes with a backtrace |
| 32 | code branch @ `f3983a48` (as `scratch/dma`) | tests | passed | 18 tests |
| 33 | `scratch/red-evidence-6` | tests | failed as intended | DMA mapping tests against the driver before the change, 19 failed checks |
| 34 | code branch @ `b27efb27` (as `scratch/access`) | tests | passed | 22 tests |
| 35, 36 | `scratch/red-evidence-7`, `-8` | tests | cancelled | superseded by 37 and 38 |
| 37 | `scratch/red-evidence-7` | tests | failed as intended | access path tests against the driver before the change, quirk test last |
| 38 | `scratch/red-evidence-8` | tests | failed as intended | same with the read-only BAR test last |
| 39, 40 | `scratch/config`, `scratch/red-evidence-9` | tests | failed | a mistake in the fake kernel (copies of eventfds lost a flag) |
| 41 | code branch @ `cb26af23` (as `scratch/config`) | tests | passed | 26 tests |
| 42 | `scratch/red-evidence-9` | tests | failed as intended | configuration tests against the driver before the change, 12 failed checks |
| 43 | code branch @ `71037836` (as `scratch/reset`) | tests | passed | 29 tests |
| 44 | `scratch/red-evidence-10` | tests | failed as intended | reset and command register tests against the driver before the change |
| 45 | code branch @ `f9f95548` (as `scratch/docs`) | tests | passed | 29 tests |
| 46 | code branch @ `f9f95548` (as `scratch/docs`) | sanitize | passed | 29 tests, no reports from the sanitizers |
| 47 to 50 | `scratch/fuzz` | tests, sanitize | passed | first version of the fuzz test, up to 20000 models |
| 51 | `scratch/mutant-1` | sanitize | failed as intended | deliberate defect, caught by a unit test |
| 52, 55 | `scratch/mutant-2`, `-2b` | sanitize, tests | passed | deliberate defect not caught: tests improved |
| 53 | `scratch/fuzz` | sanitize | failed | configure ran into a time limit on the runner; it gets three attempts now |
| 54, 58 | `scratch/mutant-2c`, `-2d` | tests | failed as intended | the deliberate defect is caught by the unit test and by the fuzz test alone |
| 56 | `scratch/fuzz` | sanitize | passed | fuzz test aiming at edges |
| 57 | `scratch/fuzz` | sanitize | failed | the fuzz test found a leak with a capability being there twice |
| 59 to 61 | `scratch/dupcaps`, `scratch/red-evidence-12` | tests, sanitize | failed | compile error in the new test |
| 62, 63 | code branch @ `f4691957` (as `scratch/dupcaps`) | tests, sanitize | passed | 30 tests |
| 64 | `scratch/red-evidence-12` | sanitize | failed as intended | duplicate capability test against the driver before the change |
| 65 to 67 | `scratch/fuzz2` | tests, sanitize | failed, cancelled | a merge done wrong |
| 68, 69 | code branch @ `680a21c5` (as `scratch/fuzz2`) | tests, sanitize | passed | 30 tests and the fuzz test |
| 70 | code branch @ `680a21c5` | sanitize | passed | 200000 random device models with another seed |
| 71, 72 | code branch @ `7afa04ad` (as `scratch/legacy`) | tests, sanitize | passed | 31 tests and the fuzz test |
| 73 to 78 | `scratch/removal` | tests, sanitize | failed, cancelled | development of WP3 |
| 79, 80 | `scratch/removal` | tests, sanitize | passed | 34 tests and the fuzz test |
| 81 | `scratch/red-evidence-13` | tests | failed as intended | removal tests against the driver before the change |
| 82 | `scratch/mutant-3` | tests | failed as intended | deliberate defect (device memory mappings not replaced), caught |
| 83, 84 | code branch @ `24515f86` | tests, sanitize | passed | 34 tests and the fuzz test |
| 85, 86 | code branch @ `f6a1395f` (as `scratch/ssm`) | tests, sanitize | passed | 35 tests and the fuzz test |
| 87 | `scratch/red-evidence-14` | tests | failed as intended | saved state test against the driver before the change |
| 88 to 90 | `scratch/hotplug` | tests | failed | development of WP5 (test did not compile; a check of the new fuzz test was too strict; option of a CI tool) |
| 91, 100 | `scratch/hotplug` | tests, sanitize | passed | 39 tests, two fuzz tests, ACPI table check |
| 92 to 99 | `scratch/mutant-hp1` to `scratch/mutant-hp8` | tests | failed as intended | eight deliberate defects in the hot-plug slot, each one caught |
| 101, 102 | code branch @ `bd836fb5` | tests, sanitize | passed | 39 tests, two fuzz tests, ACPI table check |
| 103 | `scratch/slotclient` | tests | passed | first part of WP6, 42 tests |
| 104 | `scratch/slotclient` | sanitize | failed | a gap of the test harness: with the sanitizers the compiler uses a variant of `pread` the fake kernel did not catch |
| 105 to 109 | `scratch/mutant-sc1` to `scratch/mutant-sc5` | tests | failed as intended | five deliberate defects in the slot handling of the passthrough device, each one caught |
| 110, 111 | `scratch/mutant-sc6`, `scratch/mutant-sc7` | tests | failed to build | the two defects did not compile |
| 112, 113 | code branch @ `6176a66b` (as `scratch/slotclient`) | tests, sanitize | passed | 42 tests, two fuzz tests, ACPI table check |
| 114, 115 | `scratch/mutant-sc6b`, `scratch/mutant-sc7b` | tests | failed as intended | the two defects again, each one caught |
| 116 to 119 | `scratch/slotattach` | tests, sanitize | passed | second part of WP6, 43 tests |
| 120 to 126 | `scratch/mutant-sa1` to `scratch/mutant-sa7` | tests | failed as intended | seven deliberate defects in putting a device into the slot, each one caught |
| 127 | `scratch/mutant-sa8` | tests | passed | an eighth defect went unnoticed: the test did not have a device unplugged which was put in after an unplugging |
| 128, 129 | code branch @ `defa244c` (as `scratch/slotattach`) | tests, sanitize | passed | 43 tests with the missing case, two fuzz tests, ACPI table check |
| 130 | `scratch/mutant-sa8b` | tests | failed as intended | the eighth defect, caught now |
| 131, 132 | code branch @ `949a5c03` (as `scratch/slotempty`) | tests, sanitize | passed | third part of WP6, 44 tests, two fuzz tests, ACPI table check |
| 133 to 142 | `scratch/mutant-se1` to `scratch/mutant-se8`, `-se3b`, `-se8b` | tests | failed as intended | eight deliberate defects in starting with an empty slot, each one caught (runs 135 and 140 did not build, repeated as 141 and 142) |
| 143, 144 | `scratch/slotstate` | tests, sanitize | passed | fourth part of WP6, 46 tests |
| 145 to 147 | `scratch/slotstate` | tests, sanitize | passed | more cases: device memory which stays mapped, a state without the unit of the device |
| 148 to 160 | `scratch/mutant-st1` to `scratch/mutant-st13` | tests | failed as intended, 154 passed | thirteen deliberate defects in the saved state support. Twelve caught, two of these (149, 153) by a crash in the message of the PCI bus about another vendor ID, which is a defect of its own and got fixed. The seventh (154) changed nothing a guest or a state could tell and was replaced |
| 161, 162 | `scratch/slotstate` | tests, sanitize | passed | a device is let go of under the critical section of the slot, with a test |
| 163 | `scratch/mutant-st14` | tests | failed as intended | a fourteenth defect (a device is let go of without the critical section), caught |
| 164, 165 | `scratch/slotstate` | tests, sanitize | passed | with the message of the PCI bus fixed and a state with another vendor ID as a test; the tree of the code branch @ `fb6dca3a` |
| 166 to 179 | `scratch/mutant-su1` to `scratch/mutant-su14` | tests | failed as intended | all fourteen deliberate defects on the final tree, each one caught by a failing test, none by a crash |
| 181, 182 | code branch @ `fb6dca3a` | tests, sanitize | passed | 46 tests, two fuzz tests, ACPI table check |
| 180 | code branch @ `fb6dca3a` | baseline | passed | full build, the only kind which compiles the PGM change. 875 warning lines, none in a file the branch changes; `main` had 871 and 874 in runs 5 and 7, the number depends on how lines of parallel jobs fall together |
| 183, 184 | `scratch/slotifc` | tests, sanitize | passed | fifth part of WP6, 47 tests; the tree of the code branch @ `80a60fc9` |
| 185 to 192 | `scratch/mutant-if1` to `scratch/mutant-if8` | tests | failed as intended | eight deliberate defects in the interface for the management, each one caught |
| 193, 194 | `scratch/slotfuzz` | tests, sanitize | passed | first version of the third random test |
| 195, 196 | `scratch/slotfuzz` | tests, sanitize | failed | the random test with interrupts of the device in it: one did not arrive after the VM was suspended and resumed |
| 197 to 201 | `scratch/mutant-fz1` to `scratch/mutant-fz5` | tests | failed | of no use: started before run 195 was looked at, they fail for its reason |
| 202 | `scratch/red-evidence-15` | tests | failed as intended | the test for interrupts after suspend and resume against the driver without the fix |
| 203, 204 | `scratch/irqresume` | tests, sanitize | passed | with the fix, 48 tests; the tree of the code branch @ `07cbc95b` |
| 205, 206 | `scratch/slotfuzz2` | tests, sanitize | passed | the random test on top of the fix; 40000 operations in run 205: the device put in 1034 times, taken by the host 503 times, 837 states saved, 844 loaded, 713 interrupts of the device |
| 207 to 211 | `scratch/mutant-fy1` to `scratch/mutant-fy5` | tests | 207, 209, 210 failed as intended; 208, 211 passed | five deliberate defects which take a sequence of events to show. One is caught by the random test alone (207), two by other tests (209, 210). One changes nothing (208: the hold-off of the attention button is cleared when the link goes down as well). One went unnoticed (211: the watcher thread is not woken when a device is put back after a state was loaded), no test got to that |
| 212, 213 | `scratch/slotfuzz2` | tests, sanitize | passed | with a test for that case: 49 tests and three random tests; the tree of the code branch @ `c5cc9847` |
| 214, 215 | `scratch/mutant-fy5b`, `scratch/mutant-fy1b` | tests | failed as intended | the defect which went unnoticed is caught by the new test, the other one by the random test again |
| 216 | `scratch/mgmt` @ `6e85def2` | tests | failed | the first build of what was written from 13 to 15 November: one error in the tree (a status code `VBoxManage` does not have); the testcase was built and run nevertheless, 55 tests and three random tests passed |
| 217 | `scratch/mgmt` @ `a8da0d19` | tests | passed | the tree builds, 55 tests and three random tests pass; no compiler warning in a file this work changes |
| 218 | `scratch/mgmt` @ `a69d5a89` | sanitize | passed | the tree of the head of the merge request (`3d9ea801`); no reports from the sanitizers |
| 219 | `scratch/red-evidence-18` | tests, testcase expected to fail | failed | the testcase against root port and device as they were before the review: eight tests fail as they are to, then the random test of the device ran into the time limit, which a run cannot expect. The reason is the defect the INTx change is about, see the evidence table |
| 220 | `pci-passthrough-hotplug-v1` @ `3d9ea801` | sim, not strict | passed | first run of the simulation, 53 minutes. On the runner: 14 checks, all pass (VM with an empty hot-plug capable slot, the guest takes the slot, `plugpci` refused for a device which is not there, saved state). In the virtual machine standing in for the host: set-up passes (nested KVM, IOMMU, drive bound to `vfio-pci`), VMs with the drive start, their guest does not get through its start in three attempts of ten minutes each; the test setup is too slow at that depth of nesting, see above |
| 221 | `pci-passthrough-hotplug-v1` @ `3d9ea801` | sim, not strict | passed | 17 minutes. On the runner as in run 220, all pass. Host emulated in software: the VM of VirtualBox ends with a triple fault at once in all five phases. Host on KVM with slow timers in the VM: 15 checks, all pass: a guest reads from and writes to the passed through drive, MSI-X interrupts, pause and resume, saving the state refused |
| 222 | `pci-passthrough-hotplug-v1` @ `3d9ea801` | sim, not strict | passed | 24 minutes, a runner of another kind (Azure; 220 and 221 were on AWS). 67 checks, 65 pass: all of run 221 with the host on KVM, and the device in the hot-plug capable slot: `unplugpci`, `plugpci`, forced removal, the host asking for the device back, a start without the device, a saved state with the empty slot. The two which fail are one wrong expectation of the check: a device which is there when a state is loaded is put into the slot by itself |
| 223 | `pci-passthrough-hotplug-v1` @ `3d9ea801` | sim, not strict | passed | 20 minutes, AWS. 77 checks, all pass, none skipped: everything of run 222 with the corrected check, a state loaded with and without the device, and the script of the checklist of INSTALL.md on the runner and in the simulated host |
| 224 | `scratch/quiet-reset` @ `dd43ddf3` | sim, strict | passed | the head of the merge request with the commit about the log of a device on its way out: the tree builds without a warning in a file this work changes, 56 tests and three random tests pass, the simulation passes with 79 checks, 20 minutes, AWS |
| 225 | `scratch/quiet-reset` @ `dd43ddf3` | sanitize | passed | the same commit; no reports from the sanitizers |
| 226 | `pci-passthrough-hotplug-v1` @ `dd43ddf3` | winsim, not strict | passed | 31 minutes. The first Windows guest: Windows Server 2025 installed unattended into a VM with the empty hot-plug capable slot, on the runner. 41 checks, 40 pass: the root bridge is `PNP0A08` to Windows, the firmware hands native hot-plug over, the PCI driver of Windows runs the port, the slot registers show its hot-plug driver running the slot (the same check for the Linux guest passes too), saved state, second start. One fails: Windows did not shut down when the power button was pressed; not yet known why. See "A Windows guest" above |
| 227 | `pci-passthrough-hotplug-v1` @ `dd43ddf3` | winsim, not strict | passed | 74 minutes. The part with the Linux guest passes with its new checks: the slot registers read from the outside, and the power button, whose press arrives in the guest with and without the slot and after a saved state. The installation of Windows stood still 40 seconds after the VM was started and was given up after 50 minutes: the thread of the CPU had missed the notification of a timer, which is in VirtualBox as it is without this work (see run 227 under "Installation and the simulation"). Windows with the drive in the slot was not reached |
| 228 | `scratch/timer-wakeup` @ `20fd927d` | winsim, not strict | passed | 161 minutes, AWS. The head of the merge request plus the commit about the timer: builds, the 56 tests pass, the part with the Linux guest passes. Windows installs with no VM standing still, VirtualBox making up for a missed notification every ten to twenty seconds. Windows is seven times as slow as in run 226 (the paravirtualization interface, see run 228 under "Installation and the simulation"). The power button: Windows does not shut down in the VM with the slot and does in one without. Windows in the virtual machine standing in for a host did not get to its report in 50 minutes |
| 229 | `scratch/timer-wakeup` @ `20fd927d` | winsim, not strict | passed | 72 minutes, AWS. The Windows VM without a paravirtualization interface: setup in 569 seconds, a start in 63. The power button: Windows shuts down in the VM with the slot and in three VMs for comparison, and does not after a saved state was loaded, where the entry of the I/O APIC for the interrupt of the ACPI hardware has its remote IRR bit left set (see run 229 under "Installation and the simulation"). Windows in the virtual machine standing in for a host did not get to its report in 50 minutes again |
| 230 | `scratch/timer-wakeup` @ `f563e7f8` | winsim, not strict | passed | 106 minutes, Azure, an AMD processor. The commit about the I/O APIC on top: builds without a warning, the 56 tests pass, and Windows shuts down on the power button after a saved state was loaded. All 47 checks on the runner pass, with the Linux guest and with Windows (setup in 414 seconds, a start in 37). Windows in the virtual machine standing in for a host ran on an AMD processor for the first time and did not get to its report in 50 minutes: that machine did not tell of an invariant time stamp counter, so Windows read the time from the HPET (see run 230 under "Installation and the simulation") |
| 231 | `scratch/timer-wakeup` @ `f563e7f8` | allsim, strict for the Linux guest | **failed** | 93 minutes, Azure, an AMD processor. The first run of the flavor and of the changed scripts. Builds, the 56 tests pass, everything on the runner passes with the Linux guest and with Windows. Where the drive is, 60 of 64 checks with the Linux guest pass and four fail, all four waits which ended too early: two of the six VMs were three to eight times as slow as usual, with the emulated timers seconds behind, for a reason not yet known. Windows where the drive is no longer reads the HPET, shows its start screen and was not at its disk driver after 40 minutes (see run 231 under "Installation and the simulation") |
| 232 | `scratch/timer-wakeup` @ `f563e7f8` | allsim, not strict | passed | 43 minutes, AWS, an Intel processor. The simulation as changed after run 231. All 112 checks pass: 80 with the Linux guest (16 on the runner, 64 where the drive is, in 690 seconds), 27 with Windows on the runner (setup in 560 seconds, a start in 72). Windows where the drive is was skipped for the processor. The tree builds, the 56 tests pass |
| 233 | `scratch/timer-wakeup` @ `f563e7f8` | sanitize | passed | 9 minutes. The head with the two commits which are not about the device: the 56 tests and the random tests with 20000 device models, no reports from the sanitizers. The merge request was moved to this commit after it |
| 234 | `VBox-7.2` @ `edfe7955` | baseline | **failed** | 5 minutes. The first build of the maintenance branch 7.2 as upstream has it, for a build to compare with and to build on. It did not get to building: the `configure` of that branch does not know two options the workflow passes, which are of the `configure` of the development line. The workflow tells the two apart now |
| 235 | `VBox-7.2` @ `edfe7955` | baseline, exploring | passed (the build failed) | 5 minutes. `configure` passes with its own options. The build stops after a minute and a half at the translations of Main, which take a tool of Qt the runner does not have; the `configure` of the development line switches them off by itself in that case, the older one does not. The workflow does it for it now |
| 236 | `VBox-7.2` @ `edfe7955` | baseline, exploring | passed | 10 minutes. The maintenance branch 7.2 as upstream has it builds here, in 7 minutes 41 seconds; its output is kept for the builds of the branch with this work |
| 237 | `pci-passthrough-hotplug-7.2` @ `7374d28c` | tests, exploring | passed (stopped before the build) | 1 minute. The workflow marks the files which differ from the base as new and stumbled over the ones the branch deletes. Fixed in the workflow |
| 238 | `pci-passthrough-hotplug-7.2` @ `7374d28c` | tests, exploring | passed | 12 minutes. The first build of the work on the maintenance branch 7.2: the tree builds without a compiler warning in a file the work changes, the 56 tests and the three random tests pass, the ACPI table check passes |
| 239 | `pci-passthrough-hotplug-7.2` @ `7374d28c` | allsim, exploring | passed (no VM started) | 12 minutes. Builds, the tests pass. No VM of the simulation starts, on the runner or in the virtual machine standing in for a host: VirtualBox asks for its kernel driver. The 7.2 branch does not build its KVM backend for Linux on x86 and does not let a VM start without the support driver there; the development line does both since February 2026 (see "The maintenance branches") |
| 240 | `pci-passthrough-hotplug-7.2` @ `4b35fd22` | allsim, exploring | passed (the build failed) | 5 minutes. The same with the change of the development line which builds the KVM backend for Linux on x86: the backend of the 7.2 branch does not compile. It is the backend as it was before upstream made it build and work in January and February 2026. The change is taken back on the branch |
| 241 | `VBox-7.1` @ `e307f442` | baseline, exploring | passed (stopped at the checkout) | 1 minute. The 7.1 branch names the commit of kBuild in its tree and has no `.gitmodules` saying where to get it. The workflow fetches it itself now |
| 242 | `VBox-7.1` @ `e307f442` | baseline, exploring | passed (stopped in `configure`) | 2 minutes. kBuild is there. The `configure` of 7.1 does not know one of the options the one of 7.2 takes. The workflow asks the script which of them it knows now |
| 243 | `VBox-7.1` @ `e307f442` | baseline, exploring | passed (stopped in `configure`) | 2 minutes. The `configure` of 7.1 looks for kBuild one directory above where its repository has it. The workflow tells it where it is |
| 244 | `VBox-7.1` @ `e307f442` | baseline, exploring | passed | 13 minutes. The maintenance branch 7.1 as upstream has it builds here, in 10 minutes 55 seconds; its output is kept for the builds of the branch with this work |
| 245 | `pci-passthrough-hotplug-7.1` @ `ddd0b06f` | tests, exploring | passed (one file does not compile, one test fails) | 7 minutes. The first build of the work on the 7.1 branch. One file does not compile: the part taken from 7.2 uses a name for the page size of the host which 7.1 does not have. 58 of the 59 tests of the device pass; the one for a start without the device fails with a device whose MSI-X table has a BAR of its own, because two changes to the helpers of the device which the 7.2 branch has were not taken over (they do not touch the file of the device). Both put right on the branch |
| 246 | `pci-passthrough-hotplug-7.1` @ `bbf47bc0` | tests, exploring | passed | 7 minutes. The work on the 7.1 branch builds, the 56 tests and the three random tests pass, the ACPI table check passes |
| 247 | `pci-passthrough-hotplug-7.2-kvm` @ `2139a88e` | allsim, exploring | passed (seven checks of the simulation fail) | 27 minutes, AMD EPYC 9V74. The first build of the 7.2 branch with the KVM backend of Cyberus Technology: it builds with `--with-kvm` in 7 minutes 18 seconds and the tests of the device pass. On the runner every check with the Linux guest and with Windows passes on that backend (60 passed, 7 failed, none skipped). Every VM inside the virtual machine standing in for a host stops at its first step with `VERR_NEM_IPE_3` and nothing more in its log; see "Run 247" |
| 248 | `pci-passthrough-hotplug-7.2-kvm` @ `e45e74e1` | allsim, exploring | passed (one check of the simulation fails) | 74 minutes, Intel Xeon 6975P-C. With the change after run 247. Inside the virtual machine standing in for a host all 64 checks with the Linux guest pass on the backend of Cyberus Technology: the device passed through, in the slot, taken out and put in, a start without it, saved states with the empty slot. The 20 checks with the Linux guest on the runner pass. Windows does not get through its setup in 50 minutes there, keeping its time by the HPET (57 million reads), so Windows where the drive is was skipped. 87 passed, 1 failed, 1 skipped; see "Run 248" |
| 249 | `pci-passthrough-hotplug-7.1-kvm` @ `160d83a6` | allsim, exploring | passed (ten checks of the simulation fail) | 36 minutes, AMD EPYC 9V45. The first build of the 7.1 branch with the KVM backend of Cyberus Technology: it builds in 6 minutes 52 seconds, the tests of the device pass, the 19 checks with the Linux guest on the runner pass. Windows with the Hyper-V interface is installed in 276 seconds and does not go on after a saved state is loaded. Every VM inside the virtual machine standing in for a host stops at its first step, as in run 247, and the log names the register now. 67 passed, 10 failed; see "Run 249" |
| 250 | `pci-passthrough-hotplug-7.1-kvm` @ `2aaddf20` | allsim, exploring | passed (six checks of the simulation fail) | 54 minutes, Intel Xeon 6975P-C. Windows where the drive is passes for the first time, all 35 checks: Windows starts with the drive in the hot-plug capable slot, reads from it, lets go of it when asked, takes it again, has it taken by force and takes it once more. With the Linux guest the device runs passed through on the 7.1 line. After the refused save of the state the VM stays in the state Saving on that line and the three phases after it do not start (four checks); Windows on the runner says nothing after its saved state is loaded (two checks). 95 passed, 6 failed; see "Run 250" |
| 251 | `pci-passthrough-hotplug-7.1-kvm` @ `3b085eca` | allsim, exploring | passed (twelve checks of the simulation fail) | 124 minutes, Intel Xeon Platinum 8573C. With `Console::i_saveState` returning its result: all 67 checks with the Linux guest inside the virtual machine standing in for a host pass on the 7.1 line, the three new ones about a snapshot among them. Windows on the runner goes on after its saved state is loaded and does not shut down (two checks). Windows where the drive is starts, reads from the drive, lets go of it when asked and then says nothing any more (ten checks, 70 minutes of waiting); not understood. 148 passed, 12 failed; see "Run 251" |
| 252 | `scratch/71-hardening` @ `ec8bcc46` | tests, exploring | passed | 8 minutes. The 7.1 branch with what was taken over from the 7.2 branch (26 changes, see "The 7.1 line and what the 7.2 line received after it"): it builds, every file the changes touch is compiled for ring 3 and for the kernel driver, without a warning in one of them; the tests of the device pass, the ACPI table check passes. The 7.1 branch was moved to this commit after it |
| 253 | `pci-passthrough-hotplug-7.2-kvm` @ `e759aa6f` | allsim, exploring | passed | 39 minutes, Intel Xeon 6975P-C. With the change to the KVM backend of Cyberus Technology about the Hyper-V registers in a saved state. Every check passes, 150 of them: the Linux guest on the runner and inside the virtual machine standing in for a host (67 there), Windows on the runner, which now goes on after its saved state is loaded and shuts down on the power button, and Windows where the drive is (35), which takes the drive, lets go of it, takes it again, has it taken by force and takes it once more. See "Run 253" |
| 254 | `pci-passthrough-hotplug-7.1-kvm` @ `65feb364` | allsim, exploring | passed | 63 minutes, Intel Xeon 6975P-C. The 7.1 branch with the same change to the backend and with the 26 changes taken over from the 7.2 branch. Every check passes here too, 150 of them: what was taken over has run in VMs, Windows goes on after a loaded state and shuts down, Windows where the drive is passes. See "Run 254" |
| 255 | `backports-7.1` @ `0bf9d4e6` | baseline, exploring | cancelled in its first minute | By mistake: the new way of reading the processor of the runner through the API was tried on this run with the option which cancels a run on the wrong kind of processor. Nothing was built. It showed that the reading and the cancelling work |
| 256 | `backports-7.1` @ `0bf9d4e6` | baseline, exploring | passed | 10 minutes. The 7.1 branch with the five commits taken over from the 7.2 line and nothing of the passthrough work builds from nothing (7 minutes 22 seconds), every part is linked. The copy of libslirp in the tree is compiled, without a warning in the files which were changed. The first run of the workflow with the steps which tell the processor of the runner |
| 257 | `pci-passthrough-hotplug-7.1-kvm` @ `1bfd163b` | allsim, exploring | passed (nothing was built) | One minute. The workflow takes the revision of the product from the newest commit of upstream it finds among the newest 50 of the branch, and this branch has 51 of its own since the last ones were merged in. A limit of the workflow, raised to 300 after it; nothing about the code |
| 258 | `pci-passthrough-hotplug-7.1-kvm` @ `1bfd163b` | allsim, exploring | passed | 42 minutes, Intel Xeon Platinum 8573C. The same head as run 257. Every check passes, 155 of them: the five new ones with a VM behind NAT (address, ping, a file fetched through the NAT library with the fixes taken over from 7.2), the Linux guest with the device, Windows with the empty slot, and Windows where the drive is, which passes for the fourth time in five runs. See "Run 258" |
| 259 | `pci-passthrough-hotplug-7.1-kvm` @ `9e858a53` | allsim, exploring | passed; one testcase outside of the simulation did not run | 57 minutes, Intel Xeon 6975P-C. With the size check in the ACPI device. Every check of the simulation passes, 172 of them: new are an appliance exported and imported (8) and Windows in a VM with a TPM 2.0 (9); Windows where the drive is passes for the fifth time in six runs. Seven of the eight testcases of the host services pass; `tstGuestControlSvc` ends in its first connect, as it does on every branch of upstream, and was rewritten after the run. See "Run 259" |
| 260 | `pci-passthrough-hotplug-7.1-kvm` @ `7f324666` | tests, exploring | passed | 15 minutes, AMD EPYC 7763. The build and the testcases, no simulation. The first build of the rewritten `tstGuestControlSvc`: it passes. All 16 testcases of the tree which the workflow runs since this run pass (host services, image formats, the audio mixing buffer, the USB mouse, the switch of the internal network), and the device testcase. See "Run 260" |
| 261 | `pci-passthrough-hotplug-7.2-kvm` @ `e759aa6f` | allsim, exploring | passed | 38 minutes, AMD EPYC 9V45. Every check passes, 172 of them, the first time for this branch with the VM behind NAT, the appliance, the TPM 2.0 and the testcases of the tree (15 pass). The first run on an AMD processor in which the VMs inside the virtual machine standing in for a host run on this backend: the second change about the register does what it is for, and Windows where the drive is passes there too, for the sixth time in seven runs. See "Run 261" |
| 262 | `pci-passthrough-hotplug-7.1` @ `80d2f488` | sanitize, exploring | passed | 11 minutes, Intel Xeon 6975P-C. The device testcase with the sanitizers on the 7.1 line: the 56 tests and the three random tests in full length pass, no report. 16 testcases of the tree pass, the rewritten `tstGuestControlSvc` among them |
| 263 | `pci-passthrough-hotplug-7.2` @ `5d633ceb` | sanitize, exploring | passed | 13 minutes, Intel Xeon Platinum 8573C. The same on the 7.2 line: the 56 tests and the three random tests pass, no report. 15 testcases of the tree pass |
| 264 | `pci-passthrough-hotplug-7.2-kvm` @ `e759aa6f` | sim, exploring | passed | 34 minutes, Intel Xeon 6975P-C. The first run of the simulation with a device of two functions (two NVMe controllers at one address, in one IOMMU group). Every check passes, 160 of them; 59 are the new part: both functions passed through, the device in the slot taken out and put in by naming either function, a start without it, a device of which one function is missing refused, a saved state, the host taking both functions back. See "Run 264" |
| 265 | `pci-passthrough-hotplug-7.1-kvm` @ `7f324666` | allsim, exploring | passed (two checks of the simulation fail) | 128 minutes, AMD EPYC 7763. 253 checks pass. New and passing: the device of two functions on the 7.1 line (59 checks), and Windows with such a device (41 checks: both controllers with the driver of Windows, both drives read, taken out, put in by naming the second function, taken by force, put in again). Failing: the kernel of the simulated host calls a reset of the bus failed which it has done, because the emulated root port never reports its link up (the phase about functions without a reset of their own, 8 of 9 pass); and Windows with the device of one function was stuck early in its start, once, for the hour the run gave it. See "Run 265" |
| 266 | `pci-passthrough-hotplug-7.2-kvm` @ `e759aa6f` | allsim, exploring | passed | 68 minutes, Intel Xeon 6975P-C. Every check passes, 284 of them. With the changed root port of the simulated host the kernel does the reset of the bus which the device asks for and reports it done: with the Linux guest (9 checks) and with Windows, whose device of two functions can only be reset together from the start (43 checks). Windows with the device of one function passes again; no start was stuck. See "Run 266" |
| 267 | `pci-passthrough-hotplug-v1` @ `f563e7f8` | sim, exploring | passed | 38 minutes, Intel Xeon 6975P-C. The branch of the merge request with what the simulation has got since run 232. Every check passes, 169 of them: the device of two functions (68, with the reset of the bus), the device of one function (67), and on the runner the VM behind NAT and the appliance for the first time on this branch. 12 testcases of the tree pass. See "Run 267" |
| 268 | `pci-passthrough-hotplug-7.1-kvm` @ `7f324666` | allsim, exploring | passed (four checks of the simulation fail) | 145 minutes, AMD EPYC 9V45. 222 checks pass: everything with the Linux guest, with the reset of the bus now reported done on the 7.1 line too, and Windows on the runner. Windows in the VMs inside the virtual machine standing in for a host does not get through its start, three times out of three, which is the four failures. The counters of KVM tell what it is busy with: 6300 exits a second for 380 timer interrupts a second, which at that depth is all the time there is. See "Run 268" |
| 269 | `scratch/intx` @ `42d73998` | tests, exploring | passed | 10 minutes, Intel Xeon 6975P-C. The first build of the INTx change (WP8): it builds without a warning in a changed file, the tests pass, the rewritten INTx test among them. See "INTx (WP8)" |
| 270 | `scratch/intx` @ `42d73998` | sanitize, exploring | passed | 11 minutes. The same with the sanitizers and the random tests in full length: all pass, no report |
| 271 | `scratch/intx` @ `b2b6e1ee` | tests, exploring | passed | 10 minutes, AMD EPYC 7763. The second commit of the INTx change (the longest time between asking the device is 2 milliseconds): it builds, all tests pass |
| 272 | `scratch/intx` @ `b2b6e1ee` | sanitize, exploring | passed | 10 minutes, AMD EPYC 9V45. The same with the sanitizers and the random tests in full length: all pass, no report |
| 273 | `scratch/intx` @ `b2b6e1ee` | sim, exploring | passed | 45 minutes, Intel Xeon 6975P-C. 177 checks, none failed. The first run of the phase `intx`: a Linux guest which does not use MSI uses the drive with its line interrupt, through pause and resume and a reset of the VM. See "INTx (WP8)" |
| 274 | `scratch/intx-7.2` @ `601e7710` | tests, exploring | passed | 10 minutes, Intel Xeon 6975P-C. The INTx change on the 7.2 line: it builds, all tests pass |
| 275 | `scratch/intx-7.1` @ `97f447c7` | tests, exploring | passed | 8 minutes, AMD EPYC 9V45. The INTx change on the 7.1 line: it builds, all tests pass |
| 276 | `scratch/intx` @ `6fe456f0` | sanitize, exploring | passed | 13 minutes, Intel Xeon Platinum 8573C. The third commit, written after the change was read by a second reader: it builds, 60 tests pass with the sanitizers, no report |
| 277 | `scratch/intx` @ `6fe456f0` | sim, exploring | passed | 45 minutes, AMD EPYC 7763. 177 checks, none failed, the phase `intx` with the third commit among them. `pci-passthrough-hotplug-v1` has the INTx change as one commit after this run (`4add131b`) |
| 278 | `scratch/intx-7.2-kvm` @ `26e4045a` | allsim, exploring | passed | 133 minutes, AMD EPYC 7763. 264 checks pass, 5 fail, none for the device: Windows with one function stuck in its start twice (see "Run 268"), and the new step with Windows and INTx, which could not tell the kind of interrupt and after which the guest did not see the DVD telling it to shut down. Everything with the Linux guest passes, the phase `intx` with the KVM backend among it. See "Run 278" |
| 279 | `scratch/intx-7.1-kvm` @ `9f8f5cba` | allsim, exploring | passed | 110 minutes, Intel Xeon 6975P-C. 292 checks pass, 5 fail, the same five as in run 278 and for the step itself: the kind of interrupt not told, the DVD for shutting down not seen. Everything with the Linux guest passes, the phase `intx` among it, and both parts with Windows get through their steps with the slot. See "Run 279" |
| 280 | `feat/modifyvm-graphics-order` @ `ca3e5b3b` | tests, exploring | passed | 5 minutes. A branch from `main` without the passthrough work: it builds, the step for the testcase of the device ends well without one, and the two command line checks which need no 3D acceleration pass. See "Requests from the tracker of upstream" |
| 281 | `feat/modifyvm-graphics-order` @ `ca3e5b3b` | tests with `VBOX_WITH_3D_ACCELERATION=1`, exploring | passed | 7 minutes. All six command line checks for `modifyvm` pass |
| 282 | `scratch/intx-7.2-kvm` @ `26e4045a` | winsim, exploring | passed | 46 minutes, Intel Xeon 6975P-C. All 155 checks pass. Windows runs the controllers of both drives with a line interrupt, which the host shows as `vfio-intx` for both functions on one line, reads what is on the drives and shuts down when told to. See "Run 282" |
| 283 | `scratch/intx-7.1-kvm` @ `9f8f5cba` | winsim, exploring | passed | 42 minutes, Intel Xeon 6975P-C. All 155 checks pass: the same on the 7.1 line as run 282 on 7.2. Every branch of a merge request about the passthrough work has the INTx change after this run |
| 284 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | winsim, exploring | passed | 163 minutes, AMD EPYC 9V74. 93 checks pass, 5 fail, 1 skipped. The first run with the part `hdawinhost`, and on a runner on which Windows does not get through its start at that depth: all three parts with Windows in the simulated host waited for it in vain. See "Run 284" |
| 285 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | winsim with the parts runner, win and hdawinhost, exploring | passed | 60 minutes, Intel Xeon 6975P-C. 116 checks pass, 3 fail, 2 skipped. Windows goes through every step with the slot with a drive and an audio controller as the two functions of the device. The three which failed are about the driver of Windows for the audio controller, which the Windows of the simulation does not have. See "Run 285" |
| 286 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | package, exploring | passed | 11 minutes, AMD EPYC 9V74. The first package of a build, 110 MB; the release workflow published it in the release `hw-builds`. See "Machines with real hardware" |
| 287 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | sim, exploring | passed | 42 minutes, Intel Xeon 6975P-C. 177 checks pass, 2 fail: the first script for a machine with real hardware, run for the first time, ends at creating its VM. Everything else passes |
| 288 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | sim with the parts runner and simhost, exploring | passed | 29 minutes, Intel Xeon 6975P-C. All 109 checks pass, the script for a machine with real hardware among them with its 15 checks |
| 289 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | sim with the parts runner and simhost, exploring | passed | 33 minutes, Intel Xeon 6975P-C. 109 checks pass, 1 fails: the second script for a machine with real hardware, run for the first time, whose guest does not start. See "A second test" |
| 290 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | the same | passed | 48 minutes, AMD EPYC 7763. 109 pass, 1 fails: the guest of the second script is a unified kernel image now and says nothing in ten minutes |
| 291 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the part barhost, exploring | passed | 26 minutes, Intel Xeon 6975P-C. 38 pass, 2 fail. The first run with the device of four gigabytes: the first script goes through its 15 checks with it; the guest of the second says nothing, as in run 290 |
| 292 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the parts simhost and barhost, exploring | passed | 39 minutes, AMD EPYC 7763. 117 pass, 1 fails. With one CPU and slowed time the guest of the second script starts: 12 checks pass with the drive. With the device of four gigabytes the guest has the memory with its full size above four gigabytes, and what it wrote to both ends is in the file; the one check which failed expected a driver for a device which has none |
| 293 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the part barhost, exploring | passed | 19 minutes, Intel Xeon 6975P-C. All 40 pass. Both scripts for a machine with real hardware go through their checks with the device of four gigabytes, 15 and 12, and what the guest wrote to both ends of the memory is in the file |
| 294 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the parts barhost and vgahost, exploring | passed | 24 minutes, Intel Xeon Platinum 8573C. All 49 pass. The device of four gigabytes has a ROM, which the script binding the device and the guest read as the file is (13 checks of the second script). The new part with the VGA adapter of QEMU as the device: the first script 15, the second 16, the second with the legacy VGA ranges given to the guest 20. See "The ROM of the device" and "A graphics card, as far as the simulation has one" |
| 295 | `scratch/intx-7.2-kvm` @ `26e4045a` | the same | passed | 24 minutes, Intel Xeon 6975P-C. All 58 pass. The other scripts for a machine with real hardware run for the first time: the build as it is published is fetched and is the one listed, and the first script passes with it; the inventory, the checklist, and the publishing of a result to a repository made for the run. See "Every script once" |
| 296 | `pci-passthrough-hotplug-v1` @ `4add131b` | the same | passed | 24 minutes, Intel Xeon 6975P-C. All 58 pass: the development line, with its own KVM backend, does with the device of four gigabytes, its ROM and the VGA adapter what the branch for 7.2 does. The build which is fetched there is the published one, of the branch for 7.2 |
| 297 | `pci-passthrough-hotplug-7.1-kvm` @ `9f8f5cba` | the same | passed | 28 minutes, AMD EPYC 7763. All 59 pass: the branch for 7.1 with the KVM backend does the same with the device of four gigabytes, its ROM and the VGA adapter. New in the run: the script which binds the device gives it back to the host at the end (`bind.sh --undo`) |
| 298 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with its parts as they are by default now, exploring | cancelled | After 28 minutes, by the start of run 299: the workflow lets one run at a time be in flight for a branch and a flavor and cancels the older one. A mistake in starting the runs; it had got through the part on the runner. Run 300 is the same on another name of the same commit |
| 299 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the part barhost, exploring | passed | 24 minutes, Intel Xeon 6975P-C. All 50 pass. New: the one script for all which takes root (`hw/linux/tests.sh`) binds the device and has both tests pass, and the script with the checklist finds what the second test puts its guest together with |
| 300 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | sim with its parts as they are by default now (runner, simhost, mfhost, barhost, vgahost), exploring | passed | 62 minutes, Intel Xeon 6975P-C. All 211 pass: what the simulation had before, with the drive and with the device of two functions, and the two new parts after it, with the scripts as they are since run 299 |
| 301 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the part on the runner, built with 3D acceleration for VMs, exploring | passed | 20 minutes, AMD EPYC 7763. 37 pass, 2 fail. The first build with 3D acceleration and the first VM with it, see "3D acceleration for VMs". The two checks which fail were wrong themselves |
| 302 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the parts runner, barhost and vgahost, built with 3D acceleration, exploring | passed | 31 minutes, Intel Xeon Platinum 8573C. All 69 pass: the phase with 3D acceleration with its checks put right, the picture of the screen being what the guest wrote, and the two parts with a passed through device with this build |
| 303 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | package, built with 3D acceleration | passed | 15 minutes, AMD EPYC 9V74. The package `vbox-26e4045afd60-3d-ubuntu2404.tar.zst`, 155 MB, published in the pre-release next to the one without 3D acceleration |
| 304 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | sim with the part on the runner, the whole tree built with the sanitizers of GCC and with 3D acceleration, exploring | passed, the job having stopped | 25 minutes, AMD EPYC 9V74. The tree builds that way, in 22 minutes. The job stopped at the step after: the testcase of the passthrough device does not link with the sanitizers put in for the whole tree, as it puts functions of its own in the place of some of the C library, which the library of the address sanitizer wants too. The simulation did not run. A run of this kind leaves the steps with testcases out since |
| 305 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | the same | passed | 37 minutes, Intel Xeon 6975P-C. 35 pass, 2 fail. The first VMs with the sanitizers in the whole build: they run, the phase with 3D acceleration passes, and the address sanitizer has one finding, in the event queue of Main and not about a guest, which also is why a VM with a saved state was gone too early. See "VMs with the sanitizers" |
| 306 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the parts runner and win, built with 3D acceleration, exploring | passed | 47 minutes, Intel Xeon 6975P-C. All 86 pass. Windows with 3D acceleration for the first time: the Guest Additions installed in the guest, Windows started with 3D acceleration, the screen changed forty times, see "Windows with 3D acceleration" |
| 307 | `fix/event-queue-delete-type` @ `8e1ab048` | sim with the part on the runner, the whole tree built with the sanitizers, exploring | passed | 22 minutes, AMD EPYC 9V74. All 20 pass. The development line of upstream with the one correction to the event queue of Main: it builds with the sanitizers, its VMs run, and the address sanitizer has nothing to say where it had six reports without the correction (run 305) |
| 308 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the parts runner and win, the whole tree built with the sanitizers and with 3D acceleration, exploring | passed | 99 minutes, Intel Xeon 6975P-C; 58 of them the runner took to fetch the packages of a kernel. All 88 pass. Windows with 3D acceleration and everything else the part with Windows on the runner does, with the sanitizers: they wrote nothing |
| 309 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | sim with the parts runner, simhost and barhost, the whole tree built with the sanitizers and with 3D acceleration, exploring | passed | 61 minutes, AMD EPYC 7763. 135 pass, 2 fail. The parts with a passed through device with the sanitizers for the first time: the sanitizers wrote nothing. The two checks which fail are not about a finding of theirs, see "The parts with a passed through device, with the sanitizers" |
| 310 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | sim with the part barhost, built the same way, exploring | passed | 41 minutes, Intel Xeon 6975P-C. 57 pass, 1 fails. The part of run 309 once more: the test which counted twelve lines there passes, twice, and the sanitizers wrote nothing. The check which fails is the one which keeps names out of a result, set off by a path of the runner in what the fetched build says of itself |
| 311 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the part barhost, exploring | passed | 86 minutes, Intel Xeon 6975P-C; 53 of them the runner took to fetch the packages of a kernel, as in run 308. All 50 pass. The build which is fetched is the published one with 3D acceleration: it is fetched, is the one listed, and the first script for a machine with real hardware passes with it |
| 312 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the part vgahost, exploring | passed | 24 minutes, Intel Xeon 6975P-C. All 47 pass. The first run with the workflow keeping the packages of the kernel: none were kept yet, they were fetched in 5 seconds and are kept now |
| 313 | `pci-passthrough-hotplug-7.2-kvm` @ `26e4045a` | sim with the parts runner, mfhost and vgahost, the whole tree built with the sanitizers and with 3D acceleration, exploring | passed | 52 minutes, AMD EPYC 7763. 127 pass, 1 fails. The device of two functions and the VGA adapter with the sanitizers: they wrote nothing. The check which fails counted three lines of the device which say "Failed" and are the answer to a register read while a function was on its way out; they explain the twelve of run 309, see "The device of two functions and the VGA adapter, with the sanitizers" |
| 314 | `pci-passthrough-hotplug-v1` @ `4add131b` | sim with the parts runner, simhost and barhost, the whole tree built with the sanitizers, exploring | passed | 46 minutes, AMD EPYC 9V45. 130 pass, 1 fails. The development line with the sanitizers: they wrote nothing. The check which fails found a defect of the scripts for a machine with real hardware: a test started before the service of the test before it was gone, see "The development line, with the sanitizers" |
| 315 | `scratch/intx-7.2-kvm` @ `26e4045a` | sim with the part barhost, exploring | passed | 34 minutes, Intel Xeon 6975P-C. All 50 pass. The scripts for a machine with real hardware after their correction: the script which runs every test runs them one after the other, 15 and 14 checks. No line of a register read from a function on its way out was written, so nothing was there for the new count |
| 316 | `scratch/cfg-gone` @ `432ae816` | sanitize | passed | 12 minutes. The change which has the device not ask a function which is out of service, on the development line: the 57 tests, the one of a device taken back with its two new parts among them, and the three random tests in full length pass with the sanitizers, no report. AMD EPYC 7763 |
| 317 | `scratch/cfg-gone-7.2-kvm` @ `585a8166` | sim, not strict | passed | 49 minutes, AMD EPYC 9V45. The change on the 7.2 line with the KVM backend: the testcase passes, and all 211 checks of the simulation with a Linux guest do, in every part it has |
| 318 | `scratch/cfg-gone-7.1-kvm` @ `656e0e59` | tests | passed | 15 minutes, Intel Xeon 6975P-C. The change on the 7.1 line with the KVM backend: it builds, the 57 tests and the three random tests pass |
| 319 | `scratch/mutant-cg1` @ `9f19ab15` | tests | failed as intended | 8 minutes, AMD EPYC 9V45. A deliberate defect in the change: a read which fails is not looked at again for the function having gone out of service meanwhile. The part of the test which pulls the plug from inside a read fails, twice, and nothing else does |
| 320 | `scratch/mutant-cg2` @ `deaac27b` | tests | failed as intended | 12 minutes, Intel Xeon Platinum 8573C. A second deliberate defect: the device asks whether the function is in service or not. The read of the whole configuration space of a function which is out of service counts 65 reads which reach what stands in for the device, and the part with the plug pulled from inside a read counts two where one was under way. Nothing else fails |
| 321 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | package, built with 3D acceleration | passed | 37 minutes, Intel Xeon 6975P-C. The package `vbox-585a816662b0-3d-ubuntu2404.tar.zst`, 155 MB, with the change of 2026-11-24 to the device: the testcase passes on the head of the branch, and the release workflow published the package next to the two before it. `hw/builds.txt` has it as the build which is fetched |
| 322 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim with the part barhost, not strict | passed | 24 minutes, Intel Xeon Platinum 8573C. All 50 pass. The first run which fetches the package of run 321: the script gets the file which is listed, and the first test of the scripts for a machine with real hardware goes through its 15 checks with that build |
| 323 | `scratch/cfg-gone-7.2-kvm` @ `585a8166` | sim with the parts runner, win and winhost, the whole tree built with the sanitizers, exploring | passed | 62 minutes, Intel Xeon 6975P-C. All 110 pass. Windows Server 2025 with the drive in the hot-plug capable slot, VBoxSVC and the VM process built with both sanitizers: 32 checks with Windows and the drive, and the sanitizers wrote nothing in any part. See "Windows with a device in the slot, with the sanitizers" |
| 324 | `feat/nvram-firmware-setup` @ `83aa0765` | sim with the part runner, exploring | passed | 25 minutes, Intel Xeon 6975P-C. A branch from `main` for request 944 of the tracker of upstream. It builds; all 24 checks of the simulation pass, the new phase `fwsetup` among them: the firmware enters its setup when asked and takes the request back. One of the eight new command line checks passes, the others fail with a variable store which was just made. See "Requests from the tracker of upstream" |
| 325 | `feat/nvram-firmware-setup` @ `acd8fb87` | sim with the part runner, not strict | passed | 9 minutes, AMD EPYC 7763. The second commit of that branch: all eight command line checks pass, and all 24 of the simulation again |
| 326 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim with the part errhost, exploring | passed | 16 minutes. The first run of a part about the host reporting an error of the device. Its phase was skipped: the kernel of the runners has no module for making up such an error. See "An error of the device, in the simulation" |
| 327 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim with the part errhost, exploring | passed | 18 minutes, Intel Xeon Platinum 8573C. All 59 pass. The error is made from the runner, at the port in front of the device: 20 checks, with an error which is not fatal and with one which is. VirtualBox takes the device out of service, the guest notices, the VM runs on, the device is put back. After the fatal error the kernel of the machine called its own recovery failed, for what the emulated port does not tell |
| 328 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim with the part errhost, not strict | passed | 17 minutes, Intel Xeon 6975P-C. All 59 pass. The same with a port which lets the reset of the bus get through: the kernel's recovery succeeds after both errors, and the 20 checks pass again |
| 329 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim with the parts errhost and mferrhost, not strict | passed | 19 minutes, Intel Xeon 6975P-C. All 89 pass. The error of the device with a device of two functions: the kernel tells both drivers, each function is taken out of service once, the guest loses both drives and has them again after `plugpci`: 22 checks, and the 20 with one function again |
| 330 | `pci-passthrough-hotplug-v1` @ `432ae816` | sim with the parts errhost and mferrhost, not strict | passed | 15 minutes, AMD EPYC 9V45. All 89 pass. The two parts about an error of the device on the branch of the merge request: 20 and 22 checks |
| 331 | `pci-passthrough-hotplug-7.1-kvm` @ `656e0e59` | sim with the parts errhost and mferrhost, not strict | passed | 21 minutes, AMD EPYC 7763. All 89 pass. The same on the branch for 7.1 with the KVM backend |
| 332 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | winsim, exploring | passed (11 checks of the simulation fail) | 149 minutes, AMD EPYC 7763. 104 pass. Meant for the error of the device with Windows as the guest, which it did not get to: Windows did not get through its start in the machine standing in for a host with the drive and with the audio function, and stood still with the device of two functions, as on such runners before (item 4d under "Next"). Nothing of it is about the new step |
| 333 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim with the part plughost, exploring | passed | 14 minutes, AMD EPYC 9V45. All 55 pass. The device is unplugged from the host and plugged in again, by the hot-plug of the host's own kernel: 16 checks at their first run. See "The device leaves the host and comes back, in the simulation" |
| 334, 335 | `scratch/cfg-gone-7.2-kvm` @ `585a8166` | winsim, exploring | cancelled | Each within a minute, for having a runner of the kind run 332 had; the run after them is the one which was kept |
| 336 | `scratch/cfg-gone-7.2-kvm` @ `585a8166` | winsim, exploring | passed | 61 minutes, Intel Xeon 6975P-C. 258 pass, none fails, 1 is left out. The host reports an error of the device with Windows as the guest, in all three parts with Windows and a device: the device goes out of the VM, Windows runs on, takes it again after `plugpci` and reads from the drive, after an error which is not fatal and after one which is |
| 337 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim with the parts plughost and mfplughost, exploring | passed | 19 minutes, Intel Xeon 6975P-C. All 79 pass. The device of two functions is unplugged from the host and plugged in again: 16 checks at their first run, and the 16 with the drive again |
| 338 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim, not strict | passed | 72 minutes, Intel Xeon 6975P-C. All 315 pass. The first run of the simulation with its nine parts as they are by default now, the two about an error and the two about the device being unplugged among them; the testcase of the device passes in it |
| 339 | `scratch/cfg-gone-7.2-kvm` @ `585a8166` | winsim, exploring | cancelled | Within a minute, for its runner, as runs 334 and 335 |
| 340 | `scratch/cfg-gone-7.2-kvm` @ `585a8166` | winsim with the parts runner, win and mfwinhost, exploring | passed (1 check of the simulation fails) | 36 minutes, Intel Xeon 6975P-C. 147 pass. The device of two functions is unplugged from the host while Windows uses it: the host loses it, Windows finds it gone and runs on. Plugged in again it does not come back to the host, whose kernel cannot train the link of the port |
| 341 | `scratch/cfg-gone-7.2-kvm` @ `585a8166` | sim with the parts plughost and mfplughost, exploring | passed (2 checks of the simulation fail) | 32 minutes, Intel Xeon 6975P-C. 141 pass. The unplugging, an error, and the unplugging once more: the emulated port has lost the speed and width of its link after the phase with the errors (link status 2011 before, 2000 after), and a device which is plugged into it then is not taken. That is what failed in run 340, and it is the emulator |
| 342 | `scratch/cfg-gone-7.2-kvm` @ `585a8166` | sim with the parts plughost and mfplughost, exploring | passed | 29 minutes, Intel Xeon 6975P-C. 139 pass, none fails, 2 are left out with their reason: after the errors the device is unplugged once more, and is not plugged into the port which has no link width then. Both parts, the drive and the device of two functions |
| 343 | `scratch/cfg-gone-7.2-kvm` @ `585a8166` | winsim with the parts runner, win, winhost and mfwinhost, exploring | passed | 49 minutes, Intel Xeon 6975P-C. All 223 pass, none is left out. With Windows as the guest the device is unplugged from the host and plugged in again, with the drive (13 checks) and with the device of two functions (15): Windows finds it gone and runs on, and takes it and reads from the drive when it is back. The two errors after that pass as in run 336. The port has its link after the resets of the bus which the device of two functions needs |
| 344 | `pci-passthrough-hotplug-v1` @ `432ae816` | sim with the parts plughost and mfplughost, not strict | passed | 27 minutes, AMD EPYC 9V74. 139 pass, none fails, 2 are left out with their reason, as in run 342: the device is unplugged from the host and plugged in again, with the drive and with the device of two functions, on the development line |
| 345 | `pci-passthrough-hotplug-7.1-kvm` @ `656e0e59` | sim with the parts plughost and mfplughost, not strict | passed | 29 minutes, Intel Xeon 6975P-C. 139 pass, none fails, 2 are left out with their reason: the same on the 7.1 line. The first run which looks at the port before and after each error: link status 2011 before the error which is not fatal and 2000 after it, with nothing reset. It is the error injection of the emulator, see "What takes the width of the link" |
| 346 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim with the part swplughost, exploring | passed (3 checks of the simulation fail) | 17 minutes, Intel Xeon 6975P-C. 47 pass, 1 is left out. The first run with the drive behind a switch: the VM has it, the guest reads from it, and it is unplugged from the port of the switch (gone after 12 s). The port of the emulated switch has no link width from the start, so the drive is not plugged in again, and the two phases after that had no device. The third: the checklist script took the port in the IOMMU group of the drive for another device, which was the script |
| 347 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim with the part swplughost, exploring | passed (3 checks of the simulation fail) | 25 minutes, Intel Xeon 6975P-C. 63 pass, 1 is left out. With the drive behind a switch both errors go as without one: the root port reports, the driver of the drive two ports below is told, VirtualBox takes the drive out of service, and it is put back, also after the kernel reset the whole switch. The link capabilities of the root port are 00300604 before the first error and 00300000 after it: the defect of the emulator, seen. The unplugging after that fails: the port of the switch has a completed command pending since the reset and its kernel does not hear of the request |
| 348 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim with the part swplughost, exploring | passed | 19 minutes, Intel Xeon 6975P-C. All 68 pass, 1 is left out (the plugging in, which the port of the emulated switch cannot take). The drive behind a switch: both errors, the reset of the switch after the second, and the unplugging from the port of the switch after the phase cleared what the reset had left pending there. 37 checks in the part, the one which is left out among them |
| 349 | `pci-passthrough-hotplug-7.2-kvm` @ `585a8166` | sim, not strict | passed | 84 minutes, Intel Xeon 6975P-C. All 411 pass, 3 are left out with their reason. The first run of the simulation with its ten parts, the one with the drive behind a switch among them, and the first of all parts since the phases they share were changed for that one |
| 350 | `pci-passthrough-hotplug-v1` @ `432ae816` | sim with the part swplughost, not strict | passed | 19 minutes, Intel Xeon 6975P-C. All 68 pass, 1 is left out, as in run 348: the drive behind a switch on the development line |
| 351 | `pci-passthrough-hotplug-7.1-kvm` @ `656e0e59` | sim with the part swplughost, not strict | **failed** | 10 minutes, Intel Xeon 6975P-C. Not the simulation, which it did not get to: one test of `tstDevPciVfio` failed, "a state is loaded while the host waits for the device", and passed when it was run again in the same run. A check of the testcase which came too early, see the section of that name |
| 352 | `pci-passthrough-hotplug-7.1-kvm` @ `68c5b765` | sim with the part swplughost, not strict | passed | 22 minutes, AMD EPYC 7763. The head with the testcase waiting for the slot: the testcase passes, and all 68 checks of the simulation, 1 left out. The drive behind a switch on the 7.1 line |
| 353 | `pci-passthrough-hotplug-v1` @ `20438da2` | tests | passed | 10 minutes, AMD EPYC 7763. The same change of the testcase on the development line: it builds and `tstDevPciVfio` passes |
| 354 | `pci-passthrough-hotplug-7.2-kvm` @ `05f1d33a` | tests | passed | 11 minutes, AMD EPYC 9V74. The same change of the testcase on the 7.2 line with the KVM backend, which has it by a merge of the 7.2 line: it builds and `tstDevPciVfio` passes |
| 355 | `pci-passthrough-hotplug-v1` @ `20438da2` | sanitize | passed | 11 minutes, Intel Xeon 6975P-C. The head of the development line with the testcase waiting for the slot: the 57 tests and the three random tests in full length pass with the sanitizers, no report |
| 356 | `fix/doc-modifymedium-compact` @ `5476f2d7` | tests | passed | 5 minutes, AMD EPYC 7763. The correction of the text about `modifymedium --compact` (report 996 of the upstream tracker): it builds, and the help of `VBoxManage` has the corrected text, which a new command line check looks at |
| 357 | `pci-passthrough-hotplug-7.2-kvm` @ `05f1d33a` | sim with the parts simhost, mfhost, barhost and vgahost, not strict | passed | 67 minutes, AMD EPYC 9V74. All 215 pass. The first run with the third script for a machine with real hardware, in which the device leaves the guest and comes back while a driver of the guest has it: it passes with the drive (25 checks), with the device of two functions (27), with the device of four gigabytes (26, and once more from the one script for all) and with the graphics adapter of the emulator (31), whose driver takes the card again with its connector. See "Machines with real hardware" |
| 358 | `scratch/red-evidence-19` @ `c365019f` | tests, testcase expected to fail | failed as intended | 10 minutes, AMD EPYC 7763. The test for a function which is taken back during a write of the guest to its config space, against the device without the PCI lock at that place: the check it is about fails in both variants. Two more checks failed which expected an access that does not happen; they were taken out of the test afterwards. See "A second reader for four later commits" |
| 359 | `pci-passthrough-hotplug-v1` @ `2e4e6786` | sanitize | passed | 11 minutes, Intel Xeon 6975P-C. The head of the development line with the three commits which came of the second reading: the 58 tests and the three random tests pass with the sanitizers, without a report |
| 360 | `pci-passthrough-hotplug-7.1-kvm` @ `625a36ed` | tests | passed | 12 minutes, Intel Xeon 6975P-C. The two commits about the device on the 7.1 line with the KVM backend, which has them by a merge of the 7.1 line: it builds and `tstDevPciVfio` passes with its 58 tests |
| 361 | `pci-passthrough-hotplug-7.2-kvm` @ `572356d1` | sim, strict | passed | 103 minutes, Intel Xeon 6975P-C. 415 pass, none fails, 3 are left out with their reason. The simulation with its ten parts on the head which has the PCI lock where a function goes away for the guest, and the first run of all ten parts with the third script for a machine with real hardware |
| 362 | `pci-passthrough-hotplug-7.2-kvm` @ `572356d1` | package, built with 3D acceleration | passed | 12 minutes, AMD EPYC 9V45. The package `vbox-572356d18f9e-3d-ubuntu2404.tar.zst`, 155 MB, of the head with the PCI lock where a function goes away for the guest: the testcase passes in the run, the release workflow published the package next to the three before it, and `hw/builds.txt` had it as the build which is fetched until the one of run 366 |
| 363 | `scratch/red-evidence-20` @ `5a8518a8` | tests, testcase expected to fail | failed as intended | 11 minutes, AMD EPYC 7763. The test for a live state which is refused while an EMT has work of the device, against the device which holds the lock of its slot while the VM is told: the one check it is about fails, nothing else does |
| 364 | `pci-passthrough-hotplug-v1` @ `d85a8eed` | sanitize | passed | 11 minutes, Intel Xeon 6975P-C. The head of the development line with the change about a live state which is refused, and the texts: the 59 tests and the three random tests pass with the sanitizers, without a report |
| 365 | `pci-passthrough-hotplug-7.1-kvm` @ `4220a8d5` | tests | passed | 13 minutes, Intel Xeon 6975P-C. The same on the 7.1 line with the KVM backend: it builds and `tstDevPciVfio` passes with its 59 tests |
| 366 | `pci-passthrough-hotplug-7.2-kvm` @ `cc3dc981` | package, built with 3D acceleration | passed | 16 minutes, AMD EPYC 9V74. The package `vbox-cc3dc981fd82-3d-ubuntu2404.tar.zst`, 155 MB, of the head with both changes of the day: the testcase passes in the run with its 59 tests, the release workflow published the package, and `hw/builds.txt` had it as the build which is fetched until the one of run 378 |
| 367 | `pci-passthrough-hotplug-7.2-kvm` @ `cc3dc981` | sim with the parts runner, simhost and barhost, strict | passed | 49 minutes, AMD EPYC 9V74. All 130 pass. The simulation on the head with both changes of the day, and the first run which fetches the package of run 366: the script gets the file which is listed, and the first test of the scripts for a machine with real hardware goes through its 15 checks with that build |
| 368 | `pci-passthrough-hotplug-7.2-kvm` @ `cc3dc981` | sim with the part simhost, not strict | passed | 39 minutes, AMD EPYC 7763. All 114 pass. The first run in which the part with the device in the slot asks for a snapshot for which the VM is not paused: it is refused, nothing of it is left, and the VM runs on |
| 369 | `pci-passthrough-hotplug-7.2-kvm` @ `cc3dc981` | sim with the part simhost, exploring | passed (4 checks of the simulation fail) | 47 minutes, Intel Xeon 6975P-C. 122 pass. The first run of the phase with a guest which uses no MSI and the device in the hot-plug capable slot: the guest reads from the drive with INTx through the port, and a live snapshot is refused. The four are the phase expecting the guest to let go of the device when asked, which a guest without MSI is not told; the phase was wrong, see "Who waits for whom, in Main and in the root port" |
| 370 | `pci-passthrough-hotplug-7.2-kvm` @ `cc3dc981` | sim with the part simhost, exploring | passed (1 check of the simulation fails) | 44 minutes, AMD EPYC 7763. 125 pass. The phase as it is since, on the head without the change to Main: the VM process, told to end while a command waits for the guest to let go of a device, is still there 90 seconds later. That one is the check which fails, and what the change to Main is for |
| 371 | `pci-passthrough-hotplug-7.2-kvm` @ `d9083189` | sim with the part simhost, not strict | passed | 37 minutes, Intel Xeon 6975P-C. All 126 pass. The head with the change to Main: the VM process ends within a second, with the VM powered off in order. What the run also shows and no check looked at: the service takes the VM for one with a session afterwards, see the same section |
| 372 | `pci-passthrough-hotplug-v1` @ `d8d44eb9` | tests | passed | 10 minutes, AMD EPYC 7763. The development line with the change to Main: it builds and the tests pass |
| 373 | `pci-passthrough-hotplug-7.1-kvm` @ `2c7878c5` | tests | passed | 21 minutes, AMD EPYC 9V45. The same on the 7.1 line with the KVM backend |
| 374 | `pci-passthrough-hotplug-7.2-kvm` @ `d9083189` | sim with the part simhost, exploring | passed (2 checks of the simulation fail) | 41 minutes, Intel Xeon 6975P-C. 129 pass. The VM process is told to end twice, with nothing else going on and while a device is being taken out: it ends within a second both times, and both times the service does not know a minute later that the VM is off, which are the two checks. It is waiting for nothing; the state of the machine was left at "stopping". What the second change to Main is for |
| 375 | `pci-passthrough-hotplug-7.2-kvm` @ `b1136fa2` | sim with the part simhost, not strict | passed | 43 minutes, Intel Xeon 6975P-C. All 131 pass. The head with both changes to Main: the VM process ends within a second when it is told to, with nothing else going on and while a device is being taken out, and the service knows at once that the VM is off |
| 376 | `pci-passthrough-hotplug-v1` @ `2409b39e` | tests | passed | 9 minutes, Intel Xeon 6975P-C. The development line with both changes to Main: it builds and the tests pass |
| 377 | `pci-passthrough-hotplug-7.1-kvm` @ `6d552882` | tests | passed | 15 minutes, Intel Xeon 6975P-C. The same on the 7.1 line with the KVM backend |
| 378 | `pci-passthrough-hotplug-7.2-kvm` @ `b1136fa2` | package, built with 3D acceleration | passed | 20 minutes, Intel Xeon 6975P-C. The package `vbox-b1136fa26e96-3d-ubuntu2404.tar.zst`, 155 MB, of the head with the two changes to Main: the testcase passes in the run, the release workflow published the package, and `hw/builds.txt` has it as the build which is fetched. Run 379 fetched it |
| 379 | `pci-passthrough-hotplug-7.2-kvm` @ `b1136fa2` | sim with the parts runner and barhost, strict | passed | 29 minutes, AMD EPYC 9V74. All 51 pass. The first run which fetches the package of run 378: the script gets the file which is listed, its VBoxManage runs, and the first test of the scripts for a machine with real hardware goes through its 15 checks with that build |
| 380 | `main` @ `b6bea721` | sim with the part runner, exploring | passed | 10 minutes, AMD EPYC 7763. Upstream's code without any change, with the new phase `endpaused`: 23 checks pass and 2 are told as skipped, which are the two things in Main. With the VM paused and its process told to end, `showvminfo` fails afterwards until the service is started anew; with the memory of the guest being written to a file in that moment, the VM process is still there after 90 seconds, its main thread in `Console::i_powerDown()` |
| 381 | `pci-passthrough-hotplug-v1` @ `2409b39e` | sim with the parts runner and simhost | passed | 44 minutes, Intel Xeon 6975P-C. All 137 pass, none skipped. The same phase with the two changes to Main: the process ends within two seconds both times and the service has the VM as powered off at once. The first simulation of the development line with the two changes |

Run numbers are those GitHub shows for the `build` workflow.
