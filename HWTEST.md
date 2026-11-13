# Hardware test

Draft. Nothing in here has been run by anybody yet. It is the list of what a
person with the hardware has to try, what should happen, and what to look at
when it does not. See [STATUS.md](STATUS.md) for what was verified without
hardware and what was not.

The commands marked **(WP7)** exist in code which has not been built yet, see
the status page. Until that is built, a device can only be taken away from
the VM by the host (parts D and H), not put in.

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
  `pci-passthrough-hotplug-v1`, with KVM as execution engine:
  `VBoxManage modifyvm <vm> --vm-execution-engine native-api`.
- A VM with the ICH9 chipset and EFI. Guests to try: a current Linux, and
  Windows 10 or 11.

For a graphics card switch the emulated graphics controller off and set
`ExposeVga`, see the description in `DevPciVfio.cpp`.

## 2. Setting up the VM

Host address of the device from `lspci -D`, here `0000:03:00.0` and
`0000:03:00.1`. Each function is attached, the device goes to device 0 of
guest bus 1:

    VBoxManage modifyvm <vm> --pci-attach 0000:03:00.0@0000:01:00.0
    VBoxManage modifyvm <vm> --pci-attach 0000:03:00.1@0000:01:00.1

For the hot-plug tests (parts E to K) in addition **(WP7)**:

    VBoxManage setextradata <vm> VBoxInternal2/HostPCIHotPlug 1

With hot-plug the device has to be alone on its guest bus. Some storage
controllers and network adapters of a VM get their addresses on guest bus 1
or 2 as well; the VM does not start then and the message names the device
which is in the way. Attach the PCI device at `0000:02:00.0` instead, or
take the controller out of the VM.

Until WP7 is built the same is reached with extra data which goes straight
into the configuration of the devices:

    VBoxManage setextradata <vm> VBoxInternal/Devices/ich9pcibridge/0/Config/HotPlug 1
    VBoxManage setextradata <vm> VBoxInternal/Devices/acpi/0/Config/PciExpressNativeHotPlug 1

The release log of the VM (`VBox.log` in the `Logs` folder of the VM) has
lines starting with `VFIO#0`. With the hot-plug slot there is

    VFIO#0: The device is in the hot-plug capable slot of PCI bridge #0

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

Note: devices which only have INTx interrupts do not work, that is known.

### B. Pause, resume, snapshot

1. With the device in use by the guest: `VBoxManage controlvm <vm> pause`,
   wait ten seconds, `VBoxManage controlvm <vm> resume`.
2. The device keeps working, its interrupt counters keep counting. This
   was broken in the code this work started from and is fixed; it has only
   been seen working in the test without hardware.
3. Taking a snapshot of the running VM and saving its state are refused
   while the device is in the VM, with a message which says so. The VM
   keeps running.

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
3. Linux guest, a reset of the device through its bus. Not built yet, see
   the status page. Unbind the driver of the device in the guest, then

       echo bus > /sys/bus/pci/devices/0000:01:00.0/reset_method
       echo 1 > /sys/bus/pci/devices/0000:01:00.0/reset

   The log of the VM has `The link to the hot-plug slot is down` and, right
   after it, `up`, and the device was reset on the host in between. The
   guest does not take this for the device having been unplugged. Bind the
   driver again: the device works.

### F. Taking the device out **(WP7)**

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

### G. Putting the device in **(WP7)**

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
   that, bind it to `vfio-pci`, then `plugpci` as in part G **(WP7)**. The
   device works in the guest again.
4. Repeat five times, also pulling the cable while the device is under
   load (a large copy, a 3D benchmark).

### I. Starting without the device **(WP7)**

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

### K. What must not work

1. Saving the state and taking a snapshot with the device in the slot:
   refused, as in part B.
2. `plugpci` while the state of the VM is being saved: refused. This is
   hard to hit by hand; a VM with much memory gives the time for it.
3. Loading a saved state after the device was detached from the VM
   configuration, or attached to it, or after another kind of device was
   passed through when the state was saved: refused with a message, the VM
   does not start from that state.
4. **(WP7)** A VM with hot-plug switched on and something else on the guest
   bus of the PCI device, for example a second SATA controller with the
   device at `0000:01:00.0`: the VM does not start, the message names the
   controller.
5. **(WP7)** A host address with a number which is out of range, such as
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
