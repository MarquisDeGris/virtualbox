# Hardware test

Nothing in here has been run on hardware yet. It is the list of what a
person with the hardware has to try, what should happen, and what to look at
when it does not. See [STATUS.md](STATUS.md) for what was verified without
hardware and what was not.

All of it is in the branch of the merge request. A simulation has been
through most of these steps with a Linux guest, an emulated NVMe drive and
an emulated IOMMU of the Intel kind, with the kernel, Main and `VBoxManage`
as they are, and a Windows guest has met the hot-plug capable slot while it
was empty: its PCI driver runs the port and the slot, a saved state is
loaded, the power button works before and after. On the branches for
the 7.1 and 7.2 lines with the KVM backend of Cyberus Technology the
simulation has had Windows with the emulated drive in the slot as well,
seven times. Six times Windows started with it, read from it, let go
of it when asked, took it again, had it taken by force and took it once
more (runs 250, 253, 254, 258, 259 and 261 in STATUS.md). Once, with the
same device code, Windows let go of it when asked and then did nothing
any more, with the events of the slot left unanswered (run 251); why is
not known yet, so parts F to H with a Windows guest are where to look
closely. On the branch of the merge request no Windows has had a device
in the slot yet.

A device with two functions has been in the simulation, on the branches
for 7.2 and 7.1 with that backend (runs 264 to 266): two emulated NVMe
controllers at one address, which the host has in one IOMMU group as it
has a graphics card and its audio function. With a Linux guest and with
Windows both were passed through, the device went out of the slot and
into it as one, whichever function the command named, and with the
Linux guest a device of which one function was missing was refused.
Also with functions which the host cannot reset by themselves, as it is
with many graphics cards: the device then asks for a reset of its bus,
which the kernel did, with the Linux guest and each time Windows
switched the slot off (run 266). Part C is about that reset on
hardware.

On AMD processors Windows often does not get through its start in the
simulation where the VM is two levels of virtualization deep (runs 265
and 268, four starts of six). It crawls there: its timer interrupts,
380 a second, cost it 6300 exits a second, and at that depth an exit
costs a hundred times what it costs on hardware. One level less deep
on the same processors Windows starts in 25 seconds. Nothing of that
is expected on a host which is not a virtual machine; should a Windows
guest stay at its start screen with its disk nearly idle all the same,
note down the processor of the host and keep the log.

What a graphics card has and a drive has not has been in the simulation
as far as an emulator has it (runs 292 to 296): a device with four
gigabytes of memory, which the guest reached to its last byte; a ROM,
which the guest read as the host has it; and the VGA adapter of QEMU as
the device, which the driver of the guest took, also in a VM without a
graphics adapter of its own which was given the legacy VGA ranges of
the adapter. That is not the graphics card of a real machine. Nothing
was tried anywhere with one, with Thunderbolt or with an IOMMU of AMD.

For the first steps on a machine there are scripts which need nobody
to decide anything, see [hw/README.md](hw/README.md): they tell what
the machine has, fetch a build, bind the card, and run three tests
which write down what they find; in the third the device leaves a guest
whose driver has it and comes back. The steps below are for a person, after the
scripts and with what the scripts have not got: Thunderbolt, a monitor
to look at, Windows as the guest.

## 1. What is needed

- A PC with an IOMMU (Intel VT-d or AMD-Vi), switched on in the firmware and
  in the kernel. `dmesg | grep -i -e DMAR -e IOMMU` shows it.
- Linux with kernel 6.12 or newer, `/dev/iommu` and
  `/dev/vfio/devices/vfio*` present (`CONFIG_IOMMUFD`,
  `CONFIG_VFIO_DEVICE_CDEV`). The older interface with `/dev/vfio/vfio` and a
  group works too and should be tried once.
- The device to pass through: ideally an enclosure on Thunderbolt or USB4
  with a graphics card, and one simple device such as a network card or an
  NVMe drive. All functions of the device, and nothing else, in one IOMMU
  group: `ls /sys/bus/pci/devices/<address>/iommu_group/devices`.
- The device bound to `vfio-pci` on the host, see the description at the top
  of `src/VBox/Devices/Bus/DevPciVfio.cpp`.
- The user who runs the VM may open `/dev/iommu` and the `vfio` device
  nodes, and may lock all of the guest memory (`ulimit -l`, at least the RAM
  of the VM).
- VirtualBox built from the branch of the merge request,
  `pci-passthrough-hotplug-v1`, see [INSTALL.md](INSTALL.md) for building it
  and for preparing the host, with KVM as execution engine:
  `VBoxManage modifyvm <vm> --vm-execution-engine native-api`.
- A VM with the ICH9 chipset and EFI. Guests to try: a current Linux, and
  Windows 10 or 11.
- Windows 11 asks for a TPM 2.0: `VBoxManage modifyvm <vm> --tpm-type 2.0`.
  In the simulation Windows Server 2025 ran in a VM with the hot-plug
  capable slot and that TPM: it used the TPM with its own driver, went
  on after a saved state was loaded and shut down (run 259, on the
  branch for 7.1 with the KVM backend, the slot being empty). A TPM in a
  VM which has a device in the slot has not been tried anywhere, and
  neither has Secure Boot, which Windows 11 asks for as well.
- A function whose driver uses INTx, the old kind of PCI interrupt, gets
  each of its interrupts, on all branches since 2026-11-22
  ([STATUS.md](STATUS.md), "INTx (WP8)"). It is slower with INTx than a
  function with MSI or MSI-X: the device is asked for its next interrupt
  a moment after each one, at most 2 milliseconds later, so a function
  which has interrupts all the time gets about 500 a second. Seen with
  the testcase and in the simulation: with a Linux guest told not to use
  MSI, and with Windows told to run the controllers of two drives
  without MSI, the two sharing one interrupt line (runs 277, 282 and
  283); with no real device.
  - Which kind of interrupt a function has is told by the host while
    the VM runs: `grep vfio /proc/interrupts` has a line
    `vfio-intx(<address>)` for a function with INTx, and lines
    `vfio-msix[...](<address>)` or `vfio-msi[...](<address>)` for one
    with messages. The numbers in front count.
  - Windows uses INTx for a High Definition Audio controller unless it
    is told otherwise, and that is what the audio function of a graphics
    card is to it. For a first test with a graphics card and a Windows
    guest, leave the audio function out of the VM: attach function 0
    only. The audio function has to be bound to `vfio-pci` on the host
    all the same, being in the IOMMU group of the card.
  - Then try with it: whether sound over the card plays without breaks
    is one of the things only the hardware can tell. If it does not,
    Windows can be told to use MSI for the function, which is the
    simulation's way of having it use INTx the other way around: the
    value `MSISupported` (1 for MSI) in the registry of the guest, under
    `Device Parameters\Interrupt Management\MessageSignaledInterruptProperties`
    of the device below `HKLM\SYSTEM\CurrentControlSet\Enum`, and the
    device or the guest started anew. Whether the audio function of a
    given card works with MSI is for its driver to say; nothing here
    has tried it.
- For a Windows guest: `VBoxManage modifyvm <vm> --paravirt-provider none`.
  With KVM VirtualBox cannot show a Windows guest the interface of Hyper-V,
  which is what it shows such a guest otherwise. It shows a minimal one
  instead (the log says `NEM: Adjusting GIM configuration from HyperV to
  Minimal mode`), and Windows then keeps its time by the emulated HPET. In
  the simulation setup of Windows took five times as long that way and a
  start three times.

The test can be done with one of the branches for a released line
instead, `pci-passthrough-hotplug-7.2-kvm` or `-7.1-kvm`
([INSTALL.md](INSTALL.md), part 0): the list is the same. What differs
there:

- A Windows guest is left at its default paravirtualization interface,
  the line above about `--paravirt-provider none` does not apply. That
  backend shows the guest the interface of Hyper-V.
- Part J with a Windows guest: the backend as it is published does not
  get a Windows guest with more than one CPU through a saved state (the
  screen stays as it was, the guest does nothing any more, or does not
  shut down). These branches have a change to the backend for it, with
  which Windows with two CPUs goes on after its state was loaded and
  shuts down when the power button is pressed, in the simulation on both
  (runs 253 and 254). It is the one place where the branches change
  what the backend does for every VM, so try it: save and load the state
  of the Windows guest a few times, also after it has been running for
  an hour, and note down if it ever does not go on.
- When a state is saved or loaded the log has lines about eleven
  registers which could not be read or written (`NEMR3KvmGetMsr`,
  `NEMR3KvmSetMsr`). They are registers of the Hyper-V interface which
  the kernel does not have; the lines are in order.
- The readme of the backend calls AMD processors experimental and has a
  note about split lock detection on Intel processors; the log of the VM
  says when the second applies to the host. In the simulation the VMs
  with the device, Linux and Windows, have run on an AMD processor on
  the branch for 7.2 (run 261); the IOMMU there was an emulated one of
  Intel, so a host with an IOMMU of AMD is new ground all the same.

The branch has two commits which are not about the device. Every VM with
KVM as execution engine on a Linux host gets them, and a Windows guest in
the simulation ran into what they are for:

- A VM whose guest waited for a timer of VirtualBox with its CPUs halted
  could stand still until something else woke it. VirtualBox now notices
  within about 20 milliseconds and tells the thread of the CPU again. The
  log has a line when it did, 64 times at most:

      TM: VMCPU_FF_TIMER set at two ticks in a row, 10 ms apart, while EMT 0 executes the guest, notified it

  (A build of before 2026-11-25 says "pending for more than 10 ms while
  EMT 0 executes the guest, notified it again" for the same.)

  The line is in order. A VM which stops doing anything without being
  paused is not: note it down, with the log.
- After a saved state was loaded, a guest got a level-triggered interrupt
  of an emulated device once and never again; the power button did nothing
  for Windows then. Part J has a step for it.

For a graphics card switch the emulated graphics controller off and set
`ExposeVga`, see the description in `DevPciVfio.cpp`. The kernel of the
host has to give the legacy VGA ranges of the card away for that
(`CONFIG_VFIO_PCI_VGA`, and `vfio-pci` not loaded with `disable_vga`); the
checklist tells whether it does, and a VM does not start where it does
not.

## 2. Setting up the VM

Host address of the device from `lspci -D`, here `0000:03:00.0` and
`0000:03:00.1`. Each function is attached, the device goes to device 0 of
guest bus 1:

    VBoxManage modifyvm <vm> --pci-attach 0000:03:00.0@0000:01:00.0
    VBoxManage modifyvm <vm> --pci-attach 0000:03:00.1@0000:01:00.1

For the hot-plug tests (parts E to K) in addition:

    VBoxManage setextradata <vm> VBoxInternal2/HostPCIHotPlug 1

With hot-plug the device has to be alone on its guest bus. Some storage
controllers and network adapters of a VM get their addresses on guest bus 1
or 2 as well; the VM does not start then and the message names the device
which is in the way. Attach the PCI device at `0000:02:00.0` instead, or
take the controller out of the VM.

Should that not do what it is to do, the same can be reached with extra data
which goes straight into the configuration of the devices, without the
checks and without the VM starting when the device is not there:

    VBoxManage setextradata <vm> VBoxInternal/Devices/ich9pcibridge/0/Config/HotPlug 1
    VBoxManage setextradata <vm> VBoxInternal/Devices/acpi/0/Config/PciExpressNativeHotPlug 1

The release log of the VM (`VBox.log` in the `Logs` folder of the VM) has
lines starting with `VFIO#0`. With the hot-plug slot there is

    VFIO#0: The device is in the hot-plug capable slot of PCI bridge #0

What the log has when a device is taken out and put in, as the simulation
shows it with a Linux guest (see [STATUS.md](STATUS.md)):

    PCI: Bridge #0: The attention button of the hot-plug slot was pressed
    PCI: Bridge #0: The link to the hot-plug slot is down
    VFIO#0.0: The function is taken out of service: it was detached
    VFIO#0: The link to the device is down
    PCI: Bridge #0: A device was taken out of the hot-plug slot
    ...
    VFIO#0: A device was put into the slot
    PCI: Bridge #0: A device was put into the hot-plug slot
    PCI: Bridge #0: The link to the hot-plug slot is up
    VFIO#0: The link to the device is up

When it is the host which wants the device back, the guest switching the
slot off cannot reset the device, as the host holds on to it while it
waits. The log says so, and that is as it should be:

    VFIO#0: The host is busy with the device and refuses a reset.  What was not reset of it keeps its state apart from being disabled

(Builds of before the evening of 2026-11-25 say "The device is not reset
now, the host is busy with it and refuses", and the older of them go on
with "The host resets it when it is let go of". Whether the host does is
the host's matter and was never looked at here; the line no longer says
it.)

## 3. What to try

Each part says what to do, what should happen, and what to note down. Keep
`VBox.log` and the output of `dmesg` on the host for each part which does
not go as described.

### A. The device works in the guest

1. Start the VM with the device there. No hot-plug settings.
2. Linux guest: `lspci -vv` shows the device with its real IDs, the driver
   loads, the device works (network traffic, read and write on the drive,
   picture on the monitor of the graphics card). `cat /proc/interrupts`
   shows MSI or MSI-X interrupts of the device counting.
3. Windows guest: the device shows in the device manager without a warning
   sign once its driver is installed, and works.

Note: a function whose driver uses INTx shows an interrupt of the I/O
APIC there instead (`IO-APIC`), counting as well; the list of what is
needed, further up, says what to expect of such a function.

### B. Pause, resume, snapshot

1. With the device in use by the guest: `VBoxManage controlvm <vm> pause`,
   wait ten seconds, `VBoxManage controlvm <vm> resume`.
2. The device keeps working, its interrupt counters keep counting. This
   was broken in the code this work started from and is fixed; it has only
   been seen working in the test without hardware.
3. Taking a snapshot of the running VM and saving its state are refused
   while the device is in the VM, with a message which says so. The VM
   keeps running. So it does at a snapshot for which the VM is not
   paused (`VBoxManage snapshot <vm> take <name> --live`); with a build
   of before the evening of 2026-11-25 and the device in the hot-plug
   capable slot, a VM with one CPU could stand still at that. Note it
   down if a VM does.

### C. Reset and power off

1. Reset the VM (`VBoxManage controlvm <vm> reset`) while the guest uses
   the device. The guest boots and the device works again.
2. For a device with several functions the log may have

       VFIO#0.0: The function can't be reset on its own, a reset is going to affect everything on its bus

   which is fine.
3. Power the VM off, start it again: the device works. On the host,
   `dmesg` shows no complaints of the IOMMU or of `vfio-pci`.

### D. The host takes the device back, no hot-plug slot

1. With the device in use by the guest, unbind it on the host:
   `echo 0000:03:00.0 > /sys/bus/pci/drivers/vfio-pci/unbind`
   The command returns within about a second.
2. The log has `VFIO#0.0: The function is taken out of service: the host
   wants the device back`. The VM keeps running. For the guest the device
   has stopped answering; its driver reports errors, the guest does not
   crash or hang as a whole.
3. Do the same by pulling the cable of the enclosure instead of unbinding.
4. The device does not come back for this VM. Power the VM off and on for
   that.

### E. The hot-plug slot as the guest sees it

With the hot-plug settings of part 2.

1. Linux guest: `lspci -vv -s 00:18.0` (the root port) shows `Slot+`, and
   under `SltCap` `HotPlug+ Surprise+`, `AttnBtn+ PwrCtrl+`. `dmesg | grep
   pciehp` shows the port being taken over by the hot-plug driver of the
   guest. The device behind it works as in part A.
2. Windows guest: the device works as in part A. In the device manager,
   "View, Devices by connection" shows it below a PCI Express root port.
   Note down what the root bridge is called. **This has never been tried.**
   With these settings the virtual firmware hands PCI Express native
   control to the guest, which Windows has not been given by VirtualBox
   before. If Windows does not boot or loses other devices with the
   `PciExpressNativeHotPlug` setting, that is the most important thing to
   report, with the VM log and, if it gets that far, a screenshot of the
   device manager.
3. Linux guest, a reset of the device through its bus. Unbind the driver of
   the device in the guest, then

       echo bus > /sys/bus/pci/devices/0000:01:00.0/reset_method
       echo 1 > /sys/bus/pci/devices/0000:01:00.0/reset

   The log of the VM has `The link to the hot-plug slot is down` and, right
   after it, `up`, and the device was reset on the host in between. The
   guest does not take this for the device having been unplugged. Bind the
   driver again: the device works.

### F. Taking the device out

1. Guest uses the device. `VBoxManage controlvm <vm> unplugpci
   0000:03:00.0`
2. The guest is asked first: a Linux guest logs `pciehp: Slot(...):
   Attention button pressed` and `Powering off due to button press`, and
   after about five seconds has removed the device in an orderly way.
   Windows shows the device as removed. The command returns successfully.
   The log has `VFIO#0.0: The function is taken out of service: it was
   detached`.
3. `VBoxManage showvminfo <vm>` shows the device as not being in the VM.
   On the host the device can be bound to its own driver again.
4. With a guest which does not react (stop it in a boot loader, or use
   `--timeout 2000`): the command fails with a message that the guest did
   not let go, the device stays. With `--force` the device is taken away
   regardless and the guest finds the slot empty.
5. With a guest which does not react and `--timeout 60000`: power the VM
   off from another terminal while the command waits. The VM powers off at
   once, the command ends with a message.

### G. Putting the device in

1. After part F, with the device bound to `vfio-pci` again:
   `VBoxManage controlvm <vm> plugpci 0000:03:00.0`
2. The log has `VFIO#0: A device was put into the slot`. A Linux guest
   logs `pciehp: Slot(...): Card present` and `Link Up`, finds the device
   and loads its driver; Windows shows the device again. The device works.
   The guest is told by the attention button of the slot along with the
   device being there, so a line about the button in the log of the guest
   is fine, and a guest may take five seconds before it switches the slot
   on. **Note down what Windows does here and how long it takes**: this is
   the place where guests differ, and none has been tried.
3. Repeat parts F and G ten times. Afterwards the number of open files of
   the VM process is what it was before (`ls /proc/<pid>/fd | wc -l`), and
   the guest still has all its memory.
4. With another kind of device at that host address (another card in the
   enclosure): the command fails, the message says that the device is of
   another kind, the log says what differs. The slot stays empty.

### H. Pulling the plug

1. Guest uses the device. Pull the cable of the enclosure.
2. On the host the kernel removes the device, which returns promptly. The
   log has `the host wants the device back`. The VM keeps running. A Linux
   guest logs `pciehp: Slot(...): Link Down` and `Card not present` and
   removes the device; Windows shows it as removed.
3. Plug the cable in. Authorize the device on the host if it asks for
   that, bind it to `vfio-pci`, then `plugpci` as in part G. The
   device works in the guest again.
4. Repeat five times, also pulling the cable while the device is under
   load (a large copy, a 3D benchmark).
5. Nothing is to be done for this one, it is for when it happens: the
   kernel log of the host has a line about an error of the device or of a
   port above it (`AER:` with `Uncorrectable`), which a cable can cause.
   The log of the VM then has

       VFIO#0.0: The function is taken out of service: the host reported an error of the device

   for each function, the device is gone from the guest as in step 2, and
   the VM keeps running. Once the host has the device again (bind it to
   `vfio-pci` again if the host removed it), `plugpci` puts it back. Keep
   the lines of both logs with the result. The simulation goes through
   this with an error which is made up (STATUS.md, "An error of the
   device, in the simulation"); what a real device is like after a real
   error it cannot show.

### I. Starting without the device

1. Start the VM once with the device, power it off. `VBoxManage
   getextradata <vm> enumerate` now has one `VBoxInternal2/HostPCILayout/...`
   item per function.
2. Unplug the device from the host. Start the VM. It starts; the log has

       VFIO#0.0: The device isn't there, the VM is built for it from the configured layout

   The guest sees an empty slot.
3. Plug the device in, bind it to `vfio-pci`, `plugpci`. The guest finds
   the device and it works.

### J. Saved state with the slot being empty

Steps 2 and 3 start the VM without the device, which needs part I to work.

1. Take the device out (part F or H). Save the state of the VM:
   `VBoxManage controlvm <vm> savestate`. This works.
2. Start the VM from the saved state without the device being there. The
   guest runs on, the slot is empty.
3. Save the state again, plug the device in and bind it to `vfio-pci`,
   start the VM from the saved state. The guest runs on and gets the device
   as if it had just been plugged in.
4. With a graphics card with a big BAR (use `lspci -vv` on the host; 8 GB
   and more is what matters): watch the memory use of the VM process while
   the state is saved and loaded. It should not grow by the size of the
   BAR. Note down how long saving and loading take.
5. Taking a snapshot of the running VM with the slot empty works too; note
   down how long it takes with a big BAR, this is expected to be slow.
6. After a state was loaded in step 2 or 3:
   `VBoxManage controlvm <vm> acpipowerbutton`. The guest reacts as it
   does in a VM whose state was not loaded: Windows shuts down or asks,
   a Linux with a desktop asks.

### K. What must not work

1. Saving the state and taking a snapshot with the device in the slot:
   refused, as in part B.
2. `plugpci` while the state of the VM is being saved: refused. This is
   hard to hit by hand; a VM with much memory gives the time for it.
3. Loading a saved state after the device was detached from the VM
   configuration, or attached to it, or after another kind of device was
   passed through when the state was saved: refused with a message, the VM
   does not start from that state.
4. A VM with hot-plug switched on and something else on the guest
   bus of the PCI device, for example a second SATA controller with the
   device at `0000:01:00.0`: the VM does not start, the message names the
   controller.
5. A host address with a number which is out of range, such as
   `0000:03:00.8`, is refused by `VBoxManage` and does not stand for
   another device.

### L. Sleep and resume of the host

1. With the device in the VM, suspend the host to RAM and wake it up.
   Note down what happens: this is the business of the host kernel first
   (a device behind Thunderbolt usually goes away and comes back), and the
   VM should see it as in part H.

## 4. What to report

For each part: worked as described, or what happened instead. For anything
which did not work as described:

- `VBox.log` of that run,
- `dmesg` of the host from that time,
- for a Linux guest its `dmesg`, for Windows a screenshot of the device
  manager and, if there was one, the stop code,
- the device: `lspci -vvnn -s <address>` from the host,
- host kernel version and distribution.

Please do not put details of anything which looks like a way for a guest to
harm the host into a public issue; see `SECURITY.md` of VirtualBox for
where that goes.
