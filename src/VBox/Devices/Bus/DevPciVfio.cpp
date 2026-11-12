/* $Id: DevPciVfio.cpp 115383 2026-10-17 13:20:01Z aleksey.ilyushin@oracle.com $ */
/** @file
 * PCI passthrough device emulation using VFIO/IOMMUFD.
 */

/*
 * Copyright (C) 2026 Oracle and/or its affiliates.
 *
 * This file is part of VirtualBox base platform packages, as
 * available from https://www.virtualbox.org.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, in version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses>.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */


/** @page   pg_dev_vfio     DevPciVfio - VFIO based PCI passthrough
 *
 * This device emulation utilizes the VFIO framework on Linux and allows passing
 * through real PCI Express devices to the guest, including support for Graphics
 * Processing Units (GPUs).
 *
 * Not that this in early stages and has a few shortcomings currently.
 *
 * @section sec_dev_vfio_shortcomings
 *
 * The emulation supports MSI and MSI-X interrupts. The legacy INTx style
 * doesn't work currently because VFIO auto masks those after they fired to prevent
 * an interrupt storm on the host and requires explicit unmasking after the guest
 * handled it but we currently lack a callback mechanism from the I/O APIC to get notified
 * about EOI events from the guest for the device.
 * The passed through device therefore needs to support MSI or MSI-X.
 *
 * Saved states are not supported, and will never be as we can't capture the PCIe device state
 * and replay it when resuming. Saving the state of the VM and teleporting it are refused while
 * the device is part of the VM, and a saved state isn't loaded into a VM which has the device,
 * as it can only have been made without it.
 *
 * @section sec_dev_vfio_guest_view     What the guest gets to see
 *
 * The config space is assembled from what the device has: the identification registers, the BARs
 * with their sizes and types, and the capabilities this emulation knows how to deal with. Each
 * config space byte is either emulated, passed through to the device or not there for the guest;
 * capabilities which are not handled are left out of the capability list. A device whose
 * capability lists loop or overlap is refused.
 *
 * The MSI capability is emulated with the layout it has on the device. MSI-X is emulated with a
 * table and pending bit array of the guest's own at the place where the device has them; the
 * table of the device belongs to the host kernel and is never reachable by the guest. Interrupts
 * arrive through eventfds, one per vector, and are forwarded with what the guest has programmed,
 * masked ones are held back and marked pending.
 *
 * Memory BARs are mapped into the guest where VFIO allows it, except for the pages holding the
 * MSI-X table and pending bit array, which are trapped. Trapped accesses, I/O port BARs and the
 * legacy VGA ranges go through the device file descriptor. A device which doesn't answer reads as
 * all ones and ignores writes, the guest never gets an error for it.
 *
 * @section sec_dev_vfio_memory         Guest memory
 *
 * All guest RAM is made available to the device through the IOMMU when the VM is constructed, at
 * guest physical addresses. If this fails the VM doesn't start. The mappings stay as they are for
 * as long as the VM exists, which is why the guest memory has to stay at the same place in the VM
 * process, see the requirements. The guest can't enable bus mastering without them.
 *
 * @section sec_dev_vfio_reset          Reset
 *
 * A reset of the VM disables interrupts, bus mastering and decoding of each function and then
 * resets it. Functions which can't be reset on their own, as is common for devices with several
 * functions, are reset through the bus they are on, which the host kernel permits if everything on
 * that bus is passed through to the VM. If there is no way to reset a function it stays disabled
 * until the guest sets it up again, but keeps the rest of its state; this is noted in the release
 * log.
 *
 * @section sec_dev_vfio_removal        The host taking the device back
 *
 * The host can want the device back while the VM exists, when it gets unplugged above all, and it
 * can report an error of the device it can't recover from. A thread which doesn't depend on the
 * state of the VM watches out for both. The function is taken out of service then, see
 * pciVfioFunSetGone(): accesses of the guest are answered like those to hardware which isn't
 * there, the device memory mapped into the guest is replaced by ordinary memory in place, and the
 * device is let go of so the host isn't kept waiting. An EMT then removes the guest mappings of
 * the BARs, and from there on the guest finds nothing where the function was. Other functions of
 * the device stay in service.
 *
 * Unless the device is in a hot-plug capable slot, see below, the guest isn't asked and isn't told,
 * for it this is hardware which stops answering. There is no way back either: the function stays
 * out of service until the VM is powered off.
 *
 * @section sec_dev_vfio_hotplug        Hot-plug capable slot
 *
 * A device behind a PCI express root port with a hot-plug capable slot is the device in that slot,
 * with all its functions. The port has such a slot with the HotPlug setting of ich9pcibridge, and
 * the guest makes use of it if the ACPI device has the PciExpressNativeHotPlug setting. The guest
 * can switch the slot off and on then, which disables and resets the device like a reset of the VM
 * does.
 *
 * Before a device in such a slot is taken away from the guest, the guest is asked to let go of it
 * by the attention button of the slot and gets time for it; it shows that it is done by switching
 * the slot off. This is how it goes when the host wants the device back while the device still
 * answers, and when the device is detached on request, see pciVfioR3SlotDetach(). A guest which
 * doesn't react loses the device the hard way when the time is up, if it is the host which waits
 * or if the one who made the request insists. The slot is empty afterwards and the guest is told
 * so. An error of the device reported by the host and a device which no longer answers empty the
 * slot at once.
 *
 * @section sec_dev_vfio_requirements
 *
 * The following requirements have to be met in order to support PCIe passthrough:
 *     - Recent enough hardware with IOMMU support
 *     - Linux host with at least kernel 6.12
 *     - The VM needs to have ICH9 configured
 *     - The VM needs to have UEFI configured
 *     - The VM needs to use the native execution engine (KVM), as the guest memory has to stay
 *       at a fixed place in the VM process while a device can access it
 *     - The user running the VM needs to be allowed to lock all of the guest memory (RLIMIT_MEMLOCK),
 *       it is made available to the device when the VM starts
 *
 * @section sec_dev_vfio_configuration
 *
 * In order to be able to pass through a device it needs to be bound to the vfio-pci kernel module.
 * Depending on the complexity of the device this can be done after the original driver was loaded
 * or needs to be done before the driver initialized the device because unloading the driver will leave
 * it in a broken state only a host reset can cure. An example is the AMD Radeon 5700 XT which will serve
 * as the example throughout this section.
 *
 * First identify the device's bus, device and function number for every device you want to pass through using lspci:
 *     lspci
 *     [...]
 *     0a:00.0 VGA compatible controller: Advanced Micro Devices, Inc. [AMD/ATI] Navi 10 [Radeon RX 5600 OEM/5600 XT / 5700/5700 XT] (rev c1)
 *     0a:00.1 Audio device: Advanced Micro Devices, Inc. [AMD/ATI] Navi 10 HDMI Audio
 *     [...]
 *
 * Also retrieve the PCI IDs for the devices:
 *     lspci -n
 *     [...]
 *     0a:00.0 0300: 1002:731f (rev c1)
 *     0a:00.1 0403: 1002:ab38
 *     [...]
 *
 * As this is a multi function device all functions need to be passed through. Also ensure that the device being passed through
 * is in a single IOMMU group.
 *
 * In order to override the default driver directly on boot get at the modalias for all devices using
 *     cat /sys/bus/pci/devices/0000\\:0a\\:00.0/modalias
 *     pci:v00001002d0000731Fsv00001682sd00005701bc03sc00i00
 *     cat /sys/bus/pci/devices/0000\\:0a\\:00.1/modalias
 *     pci:v00001002d0000AB38sv00001002sd0000AB38bc04sc03i00
 *
 * Then edit /etc/modprobe.d/local.conf and add the following lines to override the kernel module
 * for the devices (insert your modalias output for the devices)
 *     alias pci:v00001002d0000AB38sv00001002sd0000AB38bc04sc03i00 vfio-pci
 *     alias pci:v00001002d0000731Fsv00001682sd00005701bc03sc00i00 vfio-pci
 *
 * Also add the PCI IDs from the devices as an option when loading vfio-pci
 *     options vfio-pci ids=1002:731f,1002:ab38
 *
 * On the next host reboot the devices should be bound to vfio-pci and /dev/vfio/devices should
 * have vfio0 and vfio1.
 *
 * In case the device to be passed through to the guest doesn't suffer from broken state after a driver unload the following
 * commands will unbind the driver from the device and bind it to vfio-pci
 *    echo 0000:09:00.0 > /sys/bus/pci/devices/0000\\:09\\:00.0/driver/unbind (the bus, device and function number is dependent on the device)
 *    modprobe vfio-pci
 *    echo 10ec 8126 > /sys/bus/pci/drivers/vfio-pci/new_id (the two hex numbers are the PCI vendor and device ID of the device being passed through)
 *
 * Make /dev/vfio/devices/vfio0, /dev/vfio/devices/vfio1 and /dev/iommu read/write accessible to the user running the VM.
 *
 * Create a VM and configure it to make use of the ICH9 chipset and enable EFI. Select the native execution engine with
 *    VBoxManage modifyvm vmname --vm-execution-engine native-api
 *
 * Then add the following extradata to the VM
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/ich9pcibridge/0/Config/ExpressEnabled" 1
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/ich9pcibridge/0/Config/ExpressPortType" "RootCmplxRootPort"
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/0/Config/Fun0/ExposeVga" 1
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/0/Config/Fun0/VfioPath" /dev/vfio/devices/vfio0
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/0/Config/Fun1/VfioPath" /dev/vfio/devices/vfio1
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/0/Config/IommuPath" /dev/iommu
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/0/PCIBusNo" 1
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/0/PCIDeviceNo" 0
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/0/PCIFunctionNo" 0
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/0/Trusted" 1
 *
 * If you intend to pass through multiple devices you have to change the pci-vfio instance number to 1, 2, etc.
 * and adjust the PCI device number accordingly
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/instance/..."
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/instance/PCIDeviceNo" instance
 *
 * The ExposeVga setting is only required for graphics card devices which expose the legacy VGA I/O ranges to the guest
 * so it can output early during boot. However you need to disable the VirtualBox emulated graphics controller or starting the VM
 * fails because both devices want to register those ranges. Disable the emulated graphics controller with
 *    VBoxManage modifyvm vmname --graphicscontroller none
 *
 * Instead of the VFIO device node with VfioPath a function can be given by its address on the host, the node is
 * looked up in sysfs then. The address has to have the form the host kernel uses for it
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/0/Config/Fun0/HostAddress" 0000:0a:00.0
 *
 * The InterceptMmio setting of a function makes all accesses to its memory BARs go through the device emulation
 * instead of mapping the BARs into the guest. This is slow and meant for debugging
 *    VBoxManage setextradata vmname "VBoxInternal/Devices/pci-vfio/0/Config/Fun0/InterceptMmio" 1
 *
 * After that you can start the VM with either the usual GUI or VBoxHeadless.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DEV_PCI_RAW
#define PDMPCIDEV_INCLUDE_PRIVATE  /* Hack to get pdmpcidevint.h included at the right point. */
#include <VBox/pci.h>
#include <VBox/log.h>
#include <VBox/msi.h>
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/stam.h>
#include <VBox/vmm/pdmpcidev.h>
#include <VBox/vmm/vmapi.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/ctype.h>
#include <iprt/dir.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/thread.h>
#include <iprt/time.h>
#include <iprt/errcore.h>

#include <iprt/linux/sysfs.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

# include "DevPciInternal.h"

#if 0 /* Allow building on older hosts. */
# include <linux/vfio.h>
# include <linux/iommufd.h>
#else
# include "DevPciVfio.h"
#endif


#include "VBoxDD.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** eventfd2() syscall for the interrupt handling. */
#define LNX_SYSCALL_EVENTFD2          290
/** Flag for eventfd2(): close the file descriptor on exec, same value as O_CLOEXEC. */
#define LNX_EFD_CLOEXEC               O_CLOEXEC
/** Flag for eventfd2(): reading doesn't block, same value as O_NONBLOCK. */
#define LNX_EFD_NONBLOCK              O_NONBLOCK

/** Invalid access. */
#define VFIO_PCI_CFG_SPACE_ACCESS_INVALID     0
/** Passthrough the access to VFIO. */
#define VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH 1
/** Just do the default action for the access. */
#define VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  2
/** Emulate the access using a special handler. */
#define VFIO_PCI_CFG_SPACE_ACCESS_EMULATE     3

/** Start of the legacy VGA memory range. */
#define VFIO_PCI_VGA_MEM_START                UINT32_C(0x000a0000)
/** Size of the legacy VGA memory range. */
#define VFIO_PCI_VGA_MEM_SIZE                 UINT32_C(0x00020000)

/** How long the attention button of a hot-plug capable slot is left alone after it was
 * pressed, in milliseconds.  A guest takes five seconds from the button being pressed
 * to doing something about it, and what it does takes time too.  Pressing the button
 * again meanwhile takes the request back or, once the slot is off, asks for the
 * opposite. */
#define VFIO_PCI_ATTN_BUTTON_HOLDOFF_MS       (10 * RT_MS_1SEC)


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/**
 * MSI-X table entry.
 */
typedef struct MSIXTBLENTRY
{
    volatile uint32_t u32MsgAddr;
    volatile uint32_t u32MsgAddrUpper;
    volatile uint32_t u32MsgData;
    volatile uint32_t u32VecCtrl;
} MSIXTBLENTRY;
typedef MSIXTBLENTRY *PMSIXTBLENTRY;


/** Maximum number of segments a memory BAR gets split into: the two page
 * ranges holding the MSI-X table and the pending bit array, and what is in
 * front of, between and behind them. */
#define VFIO_PCI_BAR_SEGS_MAX       5


/**
 * A part of a memory BAR which is either mapped into the guest or trapped.
 */
typedef struct VFIOPCIBARSEG
{
    /** The VFIO PCI function this segment belongs to. */
    uint8_t             iPciFun;
    /** The BAR this segment belongs to. */
    uint8_t             iBar;
    /** Flag whether accesses to the segment are trapped (true) or whether the
     * device memory is mapped into the guest (false). */
    bool                fTrapped;
    /** Offset of the segment in the BAR. */
    uint64_t            offBar;
    /** Size of the segment, a multiple of the guest page size. */
    uint64_t            cbSeg;
    /** Type dependent handle. */
    union
    {
        /** MMIO region of a trapped segment. */
        IOMMMIOHANDLE   hMmio;
        /** MMIO2 region of a mapped segment. */
        PGMMMIO2HANDLE  hMmio2;
    } hnd;
} VFIOPCIBARSEG;
typedef VFIOPCIBARSEG *PVFIOPCIBARSEG;
typedef const VFIOPCIBARSEG *PCVFIOPCIBARSEG;


/**
 * A single PCI BAR.
 */
typedef struct VFIOPCIBAR
{
    /** The VFIO PCI function this BAR belongs to. */
    uint8_t             iPciFun;
    /** Region type, 0 - disabled, 1 - PIO, 2 - MMIO. */
    uint8_t             bType;
    /** Flag whether all of an MMIO region is trapped and handled through regular
     * MMIO handlers, because it was configured that way or because the region
     * can't be mapped. */
    bool                fInterceptMmio;
    /** Number of segments of an MMIO region. */
    uint8_t             cSegs;
    /** The address space flags for an MMIO region. */
    PCIADDRESSSPACE     enmAddrSpace;
    /** Size of the region. */
    uint64_t            cbRegion;
    /** Start offset of the region in the VFIO device file. */
    uint64_t            offVfio;
    /** Start of the mapping of an MMIO region, NULL if the region is not mapped. */
    volatile void       *pvMmio;
    /** I/O port region handle of a PIO region. */
    IOMIOPORTHANDLE     hIoPort;
    /** The segments of an MMIO region, sorted by offset and without gaps. */
    VFIOPCIBARSEG       aSegs[VFIO_PCI_BAR_SEGS_MAX];
} VFIOPCIBAR;
typedef VFIOPCIBAR *PVFIOPCIBAR;
typedef const VFIOPCIBAR *PCVFIOPCIBAR;


/**
 * VFIO PCI function.
 */
typedef struct VFIOPCIFUN
{
    /** The function name. */
    char                 szName[16];
    /** The function index. */
    uint32_t             uPciFun;
    /** The vfio cdev file descriptor .*/
    int                  iFdVfio;
    /** Flag whether the function is attached to the I/O address space (IOMMUFD only). */
    bool                 fIoasAttached;
    /** Flag whether the function is out of service because the host took the device
     * back or reported an error of it.  iFdVfio refers to what iFdInert refers to then. */
    bool volatile        fGone;
    /** Flag whether the guest finds nothing where the function was.  This follows fGone
     * once the guest mappings of the BARs are removed, see pciVfioFunGoneOnEmt(). */
    bool volatile        fGoneForGuest;
    /** What takes the place of the device file when the function goes out of service:
     * a file descriptor which refuses everything the device file is used for. */
    int                  iFdInert;
    /** The eventfd the host signals when it wants the device back, -1 if it doesn't offer that. */
    int                  iFdEvtReq;
    /** The eventfd the host signals on an error of the device it can't recover from, -1 if it doesn't offer that. */
    int                  iFdEvtErr;

    /** The start offset of the PCI config space. */
    uint64_t             offPciCfg;
    /** Size of the PCI config space. */
    size_t               cbPciCfg;
    /** The access table indicating how to treat
     * config space accesses for each byte.. Each config space byte
     * requires 4bits (2bit each for read write) and the config space can be 4096 bytes large. */
    uint8_t              abPciCfgIntercept[(4096 * 4) / 8];
    /** The PCI BAR information. */
    VFIOPCIBAR           aBars[VBOX_PCI_NUM_REGIONS];
    /** The BAR containing MSI-X state if available. */
    uint8_t              iBarMsix;

    /** Flag whether MMIO regions are intercepted and handled through
     * regular MMIO handlers or are mapped into the guest. */
    bool                 fInterceptMmio;
    /** Flag whether VGA capabilities are exposed. */
    bool                 fVga;
    /** The start offset of the VGA region. */
    uint64_t             offVga;
    /** The legacy I/O port range from 0x3b0 - 0x3bb. */
    IOMIOPORTHANDLE      hVgaIoPort1;
    /** The legacy I/O port range from 0x3c0 - 0x3df. */
    IOMIOPORTHANDLE      hVgaIoPort2;
    /** The legacy MMIO range from 0xa0000 - 0xbffff*/
    IOMMMIOHANDLE        hVgaMmio;

    /** ROM region start offset. */
    uint64_t             offRom;
    /** Size of the ROM region. */
    size_t               cbRom;
    /** Pointer to the ROM region memory. */
    void                 *pvRom;
    /** ROM region handle. */
    PGMMMIO2HANDLE       hRom;
    /** The write protection layer for the ROM MMIO2 region. */
    PGMPHYSHANDLERTYPE   hRomAccHndType;
    /** The physical guest address the ROM is currently mapped at. */
    RTGCPHYS             GCPhysRom;

    /** The number of interrupt vectors for each interrupt type. */
    AssertCompile(   VFIO_PCI_INTX_IRQ_INDEX == 0
                  && VFIO_PCI_MSI_IRQ_INDEX  == 1
                  && VFIO_PCI_MSIX_IRQ_INDEX == 2);
    uint32_t             acIrqVectors[VFIO_PCI_MSIX_IRQ_INDEX + 1];

    /** The eventfd to wakeup the IRQ poller. */
    int                  iFdWakeup;

    /** The eventfds being pre-created, one for each vector of the interrupt
     * type with the most vectors. */
    int                  *paFdEvts;
    /** Number of entries in paFdEvts. */
    uint32_t             cFdEvts;
    /** The interrupt type currently set up with VFIO, UINT8_MAX if none.
     * This is what the thread changing the interrupt configuration goes by. */
    uint8_t              uIrqMode;
    /** The number of vectors set up with VFIO for uIrqMode. */
    uint32_t             cIrqVectors;
    /** The IRQ mode the poller currently works in. */
    uint8_t              uIrqModeCur;
    /** The IRQ mode the poller shall switch to. */
    volatile uint8_t     uIrqModeNew;
    /** Sequence number of the last mode switch requested from the poller. */
    volatile uint32_t    uIrqSeqReq;
    /** Sequence number of the last mode switch the poller has completed. */
    volatile uint32_t    uIrqSeqAck;

    /** The interrupt polling thread. */
    PPDMTHREAD           pThrdIrq;

    /** The MSI capability offset if enabled. */
    uint8_t              offMsiCtrl;
    /** Flag whether the MSI capability has a 64-bit message address. */
    bool                 fMsi64Bit;
    /** Flag whether the MSI capability has per-vector masking. */
    bool                 fMsiMaskable;

    /** The MSI-X capability offset if enabled. */
    uint8_t              offMsixCtrl;
    /** Offset of the MSI-X table in the BAR given by iBarMsix. */
    uint32_t             offMsixTbl;
    /** Size of the MSI-X table in bytes. */
    uint32_t             cbMsixTbl;
    /** Offset of the MSI-X pending bit array in the BAR given by iBarMsix. */
    uint32_t             offMsixPba;
    /** Size of the MSI-X pending bit array in bytes. */
    uint32_t             cbMsixPba;
    /** The MSI-X table the guest sees.  The table of the device is managed by
     * the host kernel and never exposed to the guest. */
    PMSIXTBLENTRY        paMsixTbl;
    /** The MSI-X pending bit array the guest sees. */
    volatile uint64_t    *pbmMsixPba;
} VFIOPCIFUN;
/** Pointer to the a VFIO PCI function. */
typedef VFIOPCIFUN *PVFIOPCIFUN;
/** Pointer to the a constant VFIO PCI function. */
typedef const VFIOPCIFUN *PCVFIOPCIFUN;


/**
 * Passed through VFIO PCI device instance.
 */
typedef struct VFIOPCI
{
    /** Pointer to the device instance. */
    PPDMDEVINSR3         pDevIns;
    /** The device instance. */
    int                  iInstance;

    /** Flag whether the legacy VFIO container API is used. */
    bool                 fVfioLegacy;

    /** Type dependent data. */
    union
    {
        /** IOMMUFD based data. */
        struct
        {
            /** The IOMMU file descriptor. */
            int          iFdIommu;
            /** The IOMMU page table object id. */
            uint32_t     idIommuHwpt;
        } IommuFd;
        /** VFIO container based data. */
        struct
        {
            /** The VFIO container file descriptor. */
            int          iFdVfioContainer;
            /** The VFIO group filedescriptor. */
            int          iFdVfioGroup;
        } VfioGroup;
    };

    /** The VFIO PCI functions. */
    VFIOPCIFUN           aPciFuns[8];

    /** Flag whether the guest RAM was mapped to the IOMMU. */
    bool                 fGuestRamMapped;
    /** Flag whether the I/O address space was allocated (IOMMUFD only). */
    bool                 fIoasAllocated;

    /** The thread watching for the host wanting a function back or reporting an error on it. */
    RTTHREAD             hThrdWatcher;
    /** The eventfd for waking up the watcher thread. */
    int                  iFdWatcherWakeup;
    /** Flag telling the watcher thread to terminate. */
    bool volatile        fWatcherStop;

    /** The root port with the hot-plug capable slot the device is in, NULL if the
     * device is somewhere else. */
    PPDMDEVINSR3         pDevInsPort;
    /** How long the guest gets for letting go of the device before it is taken away
     * from it because the host wants it back, in milliseconds. */
    uint32_t             cMsGuestRelease;
    /** When the attention button of the slot was pressed last to ask the guest to let
     * go of the device, 0 if it wasn't since the link to the slot went down last. */
    uint64_t volatile    msAttnPressed;
} VFIOPCI;
/** Pointer to the raw PCI instance data. */
typedef VFIOPCI *PVFIOPCI;


/** Pointer to a const PCI quirk. */
typedef const struct VFIOPCIQUIRK *PCVFIOPCIQUIRK;


/**
 * Applies quirks based on the detected vendir/device ID.
 *
 * @returns VBox status code.
 * @param   pThis               The VFIO PCI device instance.
 * @param   pFun                The function to apply the quirk to.
 * @param   pQuirk              The quirk to apply.
 */
typedef DECLCALLBACKTYPE(int, FNVFIOPCIQUIRKAPPLY, (PVFIOPCI pThis, PVFIOPCIFUN pFun, PCVFIOPCIQUIRK pQuirk));
/** Pointer to a quirk apply callback. */
typedef FNVFIOPCIQUIRKAPPLY *PFNVFIOPCIQUIRKAPPLY;


/**
 * A quirk entry.
 */
typedef struct VFIOPCIQUIRK
{
    /** The vendor ID. */
    uint16_t                idVendor;
    /** The device ID. */
    uint16_t                idDevice;
    /** Some flags for the quirk. */
    uint32_t                fQuirk;
    /** The quirk handler. */
    PFNVFIOPCIQUIRKAPPLY    pfnApply;
    /** The quirk description for logging. */
    const char              *pszDesc;
} VFIOPCIQUIRK;


#ifndef VBOX_DEVICE_STRUCT_TESTCASE

/**
 * eventfd2() syscall wrapper.
 *
 * @returns IPRT status code.
 * @param   uValInit            The initial value of the maintained counter.
 * @param   fFlags              Flags controlling the eventfd behavior.
 * @param   piFdEvt             Where to store the file descriptor of the eventfd object on success.
 */
DECLINLINE(int) pciVfioLnxEventfd2(uint32_t uValInit, uint32_t fFlags, int *piFdEvt)
{
    int rcLnx = syscall(LNX_SYSCALL_EVENTFD2, uValInit, fFlags);
    if (RT_UNLIKELY(rcLnx == -1))
        return RTErrConvertFromErrno(errno);

    *piFdEvt = rcLnx;
    return VINF_SUCCESS;
}


DECLINLINE(int) pciVfioCfgSpaceReadU8(PCVFIOPCIFUN pFun, uint32_t offReg, uint8_t *pb)
{
    ssize_t cb = pread(pFun->iFdVfio, pb, 1, pFun->offPciCfg + offReg);
    if (cb != 1)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}


DECLINLINE(int) pciVfioCfgSpaceReadU16(PCVFIOPCIFUN pFun, uint32_t offReg, uint16_t *pu16)
{
    ssize_t cb = pread(pFun->iFdVfio, pu16, 2, pFun->offPciCfg + offReg);
    if (cb != 2)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}


DECLINLINE(int) pciVfioCfgSpaceReadU32(PCVFIOPCIFUN pFun, uint32_t offReg, uint32_t *pu32)
{
    ssize_t cb = pread(pFun->iFdVfio, pu32, 4, pFun->offPciCfg + offReg);
    if (cb != 4)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}


#if 0 /* unused */
DECLINLINE(int) pciVfioCfgSpaceReadU64(PCVFIOPCIFUN pFun, uint32_t offReg, uint64_t *pu64)
{
    ssize_t cb = pread(pFun->iFdVfio, pu64, 8, pFun->offPciCfg + offReg);
    if (cb != 8)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}
#endif


DECLINLINE(int) pciVfioCfgSpaceWriteU8(PCVFIOPCIFUN pFun, uint32_t offReg, uint8_t u8)
{
    ssize_t cb = pwrite(pFun->iFdVfio, &u8, 1, pFun->offPciCfg + offReg);
    if (cb != 1)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}


DECLINLINE(int) pciVfioCfgSpaceWriteU16(PCVFIOPCIFUN pFun, uint32_t offReg, uint16_t u16)
{
    ssize_t cb = pwrite(pFun->iFdVfio, &u16, 2, pFun->offPciCfg + offReg);
    if (cb != 2)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}

DECLINLINE(int) pciVfioCfgSpaceWriteU32(PCVFIOPCIFUN pFun, uint32_t offReg, uint32_t u32)
{
    ssize_t cb = pwrite(pFun->iFdVfio, &u32, 4, pFun->offPciCfg + offReg);
    if (cb != 4)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}


#if 0 /* unused */
DECLINLINE(int) pciVfioCfgSpaceWriteU64(PCVFIOPCIFUN pFun, uint32_t offReg, uint64_t u64)
{
    ssize_t cb = pwrite(pFun->iFdVfio, &u64, 8, pFun->offPciCfg + offReg);
    if (cb != 8)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}
#endif


/**
 * Reads from or writes to a region of the device through the device file
 * descriptor on behalf of the guest.
 *
 * A device which doesn't answer is treated like one which isn't there: reads
 * return all ones and writes are dropped.  There is no failure status, it is
 * nothing the guest could be blamed for.
 *
 * @param   pThis       The VFIO PCI device instance.
 * @param   pFun        The function.
 * @param   offVfio     Offset into the device file.
 * @param   pv          Where to store what was read, or what to write.
 * @param   cb          Number of bytes to access.
 * @param   fWrite      Flag whether this is a write (true) or a read (false).
 */
static void pciVfioFdAccess(PVFIOPCI pThis, PCVFIOPCIFUN pFun, uint64_t offVfio, void *pv, size_t cb, bool fWrite)
{
    if (pFun->fGone)
    {
        if (!fWrite)
            memset(pv, 0xff, cb);
        return;
    }

    ssize_t const cbDone = fWrite
                         ? pwrite(pFun->iFdVfio, pv, cb, offVfio)
                         : pread(pFun->iFdVfio, pv, cb, offVfio);
    if (cbDone != (ssize_t)cb)
    {
        int const iErrno = cbDone < 0 ? errno : 0;
        if (!fWrite)
            memset(pv, 0xff, cb);
        LogRelMax(32, ("VFIO#%d.%u: %s %zu byte(s) at offset %#RX64 of the device file failed with %d\n", pThis->iInstance,
                       pFun->uPciFun, fWrite ? "Writing" : "Reading", cb, offVfio, iErrno));
    }
}


/**
 * @callback_method_impl{FNIOMIOPORTNEWOUT}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioPioWrite(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT offPort, uint32_t u32, unsigned cb)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PCVFIOPCIBAR pBar = (PCVFIOPCIBAR)pvUser;
    PCVFIOPCIFUN pFun = &pThis->aPciFuns[pBar->iPciFun];
    AssertReturn(cb <= sizeof(u32), VINF_SUCCESS);

    pciVfioFdAccess(pThis, pFun, pBar->offVfio + offPort, &u32, cb, true /*fWrite*/);
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMIOPORTNEWIN}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioPioRead(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT offPort, uint32_t *pu32, unsigned cb)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PCVFIOPCIBAR pBar = (PCVFIOPCIBAR)pvUser;
    PCVFIOPCIFUN pFun = &pThis->aPciFuns[pBar->iPciFun];
    AssertReturn(cb <= sizeof(*pu32), VERR_IOM_IOPORT_UNUSED);

    uint32_t u32 = UINT32_MAX;
    pciVfioFdAccess(pThis, pFun, pBar->offVfio + offPort, &u32, cb, false /*fWrite*/);
    *pu32 = u32;
    return VINF_SUCCESS;
}


/**
 * Sends a message signalled interrupt of a function to the interrupt controller.
 *
 * @param   pDevIns     The device instance.
 * @param   pPciDev     The PCI device of the function raising the interrupt.
 * @param   uAddr       The message address.
 * @param   uData       The message data.
 */
static void pciVfioMsgSend(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint64_t uAddr, uint32_t uData)
{
    /* Can't use PDMDevHlpPCISetIrqEx here as the PCI stuff is not aware of the MSI and MSI-X support of this device. */
    /** @todo This isn't pretty as it accesses stuff internal to the PCI subsystem. */
    PCPDMPCIHLPR3 pPciHlp = (PCPDMPCIHLPR3)pPciDev->Int.s.pvPciBusPtrR3;
    AssertPtrReturnVoid(pPciHlp);
    PPDMDEVINS pDevInsBus = pPciHlp->pfnGetBusByNo(pDevIns, pPciDev->Int.s.idxPdmBus);
    AssertPtrReturnVoid(pDevInsBus);
    PDEVPCIBUS pBus = PDMINS_2_DATA(pDevInsBus, PDEVPCIBUS);
    uint16_t const uBusDevFn = PCIBDF_MAKE(pBus->iBus, pPciDev->uDevFn);

    MSIMSG Msi;
    Msi.Addr.u64 = uAddr;
    Msi.Data.u32 = uData;
    pPciHlp->pfnIoApicSendMsi(pDevIns, uBusDevFn, &Msi, 0 /*uTagSrc*/);
}


/**
 * Returns the config space offset of the MSI message data register.
 *
 * The mask bits follow at +4 and the pending bits at +8 if the capability has
 * per-vector masking.
 *
 * @returns Config space offset.
 * @param   pFun        The function, which has to have an MSI capability.
 */
DECLINLINE(uint32_t) pciVfioMsiGetDataOff(PCVFIOPCIFUN pFun)
{
    return pFun->offMsiCtrl - VBOX_MSI_CAP_MESSAGE_CONTROL
         + (pFun->fMsi64Bit ? VBOX_MSI_CAP_MESSAGE_DATA_64 : VBOX_MSI_CAP_MESSAGE_DATA_32);
}


/**
 * Checks whether an MSI vector can be delivered right now: MSI is enabled and
 * the vector is not masked.
 *
 * @returns true if the vector can be delivered, false if it has to be held back.
 * @param   pFun        The function.
 * @param   pPciDev     The PCI device of the function.
 * @param   idxVector   The vector.
 */
DECLINLINE(bool) pciVfioMsiIsVectorOpen(PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint32_t idxVector)
{
    if (!(PDMPciDevGetWord(pPciDev, pFun->offMsiCtrl) & VBOX_PCI_MSI_FLAGS_ENABLE))
        return false;
    return    !pFun->fMsiMaskable
           || !(PDMPciDevGetDWord(pPciDev, pciVfioMsiGetDataOff(pFun) + 4) & RT_BIT_32(idxVector));
}


/**
 * Sends the MSI message the guest has configured to the interrupt controller.
 *
 * @param   pDevIns     The device instance.
 * @param   pFun        The function raising the interrupt.
 * @param   pPciDev     The PCI device of the function.
 * @param   idxVector   The vector.
 */
static void pciVfioMsiSend(PPDMDEVINS pDevIns, PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint32_t idxVector)
{
    uint32_t const offCap = pFun->offMsiCtrl - VBOX_MSI_CAP_MESSAGE_CONTROL;
    uint64_t       uAddr  = PDMPciDevGetDWord(pPciDev, offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_LO);
    if (pFun->fMsi64Bit)
        uAddr |= (uint64_t)PDMPciDevGetDWord(pPciDev, offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_HI) << 32;

    /* The vector number replaces the low bits of the message data, as many as the guest has enabled vectors for. */
    uint32_t const cVectorsLog2 = (PDMPciDevGetWord(pPciDev, pFun->offMsiCtrl) & VBOX_PCI_MSI_FLAGS_QSIZE) >> 4;
    uint32_t const fVectorMask  = RT_BIT_32(RT_MIN(cVectorsLog2, 5)) - 1;
    uint32_t const uData        =   (PDMPciDevGetWord(pPciDev, pciVfioMsiGetDataOff(pFun)) & ~fVectorMask)
                                  | (idxVector & fVectorMask);

    pciVfioMsgSend(pDevIns, pPciDev, uAddr, uData);
}


/**
 * Delivers an MSI vector if it is pending and can be delivered now.
 *
 * @param   pDevIns     The device instance.
 * @param   pFun        The function, which has to have an MSI capability with per-vector masking.
 * @param   pPciDev     The PCI device of the function.
 * @param   idxVector   The vector.
 */
static void pciVfioMsiDeliverPending(PPDMDEVINS pDevIns, PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint32_t idxVector)
{
    uint32_t volatile *pfPending = (uint32_t volatile *)&pPciDev->abConfig[pciVfioMsiGetDataOff(pFun) + 8];
    if (   pciVfioMsiIsVectorOpen(pFun, pPciDev, idxVector)
        && ASMAtomicBitTestAndClear(pfPending, (int32_t)idxVector))
        pciVfioMsiSend(pDevIns, pFun, pPciDev, idxVector);
}


/**
 * Handles an MSI interrupt of the device: delivers it to the guest or records
 * it as pending if the guest has it masked.
 *
 * @param   pDevIns     The device instance.
 * @param   pFun        The function raising the interrupt.
 * @param   pPciDev     The PCI device of the function.
 * @param   idxVector   The vector.
 */
static void pciVfioMsiRaise(PPDMDEVINS pDevIns, PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint32_t idxVector)
{
    if (pciVfioMsiIsVectorOpen(pFun, pPciDev, idxVector))
        pciVfioMsiSend(pDevIns, pFun, pPciDev, idxVector);
    else if (pFun->fMsiMaskable)
    {
        /* The guest might be unmasking the vector right now, so look again after recording it. */
        ASMAtomicBitSet((uint32_t volatile *)&pPciDev->abConfig[pciVfioMsiGetDataOff(pFun) + 8], (int32_t)idxVector);
        pciVfioMsiDeliverPending(pDevIns, pFun, pPciDev, idxVector);
    }
    /* else: The guest has disabled MSI in the meantime, the interrupt is lost like on real hardware. */
}


/**
 * Sends the MSI-X message the guest has configured for a vector to the
 * interrupt controller.
 *
 * @param   pDevIns     The device instance.
 * @param   pFun        The function raising the interrupt.
 * @param   pPciDev     The PCI device of the function.
 * @param   idxVector   The vector.
 */
static void pciVfioMsixSend(PPDMDEVINS pDevIns, PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint32_t idxVector)
{
    PMSIXTBLENTRY pVec = &pFun->paMsixTbl[idxVector];
    pciVfioMsgSend(pDevIns, pPciDev, ((uint64_t)pVec->u32MsgAddrUpper << 32) | pVec->u32MsgAddr, pVec->u32MsgData);
}


/**
 * Checks whether an MSI-X vector can be delivered right now: MSI-X is enabled
 * and neither the function nor the vector is masked.
 *
 * @returns true if the vector can be delivered, false if it has to be held back.
 * @param   pFun        The function.
 * @param   pPciDev     The PCI device of the function.
 * @param   idxVector   The vector.
 */
DECLINLINE(bool) pciVfioMsixIsVectorOpen(PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint32_t idxVector)
{
    uint16_t const fMsixCtrl = PDMPciDevGetWord(pPciDev, pFun->offMsixCtrl);
    return    (fMsixCtrl & (VBOX_PCI_MSIX_FLAGS_ENABLE | VBOX_PCI_MSIX_FLAGS_FUNCMASK)) == VBOX_PCI_MSIX_FLAGS_ENABLE
           && !(pFun->paMsixTbl[idxVector].u32VecCtrl & RT_BIT_32(0));
}


/**
 * Delivers an MSI-X vector if it is pending and can be delivered now.
 *
 * @param   pDevIns     The device instance.
 * @param   pFun        The function.
 * @param   pPciDev     The PCI device of the function.
 * @param   idxVector   The vector.
 */
static void pciVfioMsixDeliverPending(PPDMDEVINS pDevIns, PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint32_t idxVector)
{
    if (   pciVfioMsixIsVectorOpen(pFun, pPciDev, idxVector)
        && ASMAtomicBitTestAndClear(pFun->pbmMsixPba, (int32_t)idxVector))
        pciVfioMsixSend(pDevIns, pFun, pPciDev, idxVector);
}


/**
 * Handles an MSI-X interrupt of the device: delivers it to the guest or
 * records it as pending if the guest has it masked.
 *
 * @param   pDevIns     The device instance.
 * @param   pFun        The function raising the interrupt.
 * @param   pPciDev     The PCI device of the function.
 * @param   idxVector   The vector.
 */
static void pciVfioMsixRaise(PPDMDEVINS pDevIns, PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint32_t idxVector)
{
    if (pciVfioMsixIsVectorOpen(pFun, pPciDev, idxVector))
        pciVfioMsixSend(pDevIns, pFun, pPciDev, idxVector);
    else
    {
        /* The guest might be unmasking the vector right now, so look again after recording it. */
        ASMAtomicBitSet(pFun->pbmMsixPba, (int32_t)idxVector);
        pciVfioMsixDeliverPending(pDevIns, pFun, pPciDev, idxVector);
    }
}


/**
 * Handles a guest write to the MSI-X table.
 *
 * @param   pDevIns     The device instance.
 * @param   pFun        The function.
 * @param   pPciDev     The PCI device of the function.
 * @param   offTbl      Offset into the table, the caller made sure the access is inside of it.
 * @param   pv          What to write.
 * @param   cb          Number of bytes to write.
 */
static void pciVfioMsixTblWrite(PPDMDEVINS pDevIns, PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint32_t offTbl,
                                void const *pv, size_t cb)
{
    memcpy((uint8_t *)pFun->paMsixTbl + offTbl, pv, cb);

    /* Only the mask bit of the vector control is implemented.  A vector which just
       got unmasked is sent what it has pending. */
    uint32_t const idxFirst = offTbl / sizeof(MSIXTBLENTRY);
    uint32_t const idxLast  = (uint32_t)((offTbl + cb - 1) / sizeof(MSIXTBLENTRY));
    for (uint32_t idxVector = idxFirst; idxVector <= idxLast; idxVector++)
    {
        pFun->paMsixTbl[idxVector].u32VecCtrl &= RT_BIT_32(0);
        pciVfioMsixDeliverPending(pDevIns, pFun, pPciDev, idxVector);
    }
}


/**
 * Reads from or writes to the device memory behind a memory BAR on behalf of
 * the guest.
 *
 * This always goes through the device file, also for a BAR which is mapped
 * into this process.  The host kernel takes the mapping away at times, while
 * the device doesn't decode memory for instance, and touching it then gets the
 * process a signal.  An access through the device file just fails.
 *
 * @param   pThis       The VFIO PCI device instance.
 * @param   pFun        The function.
 * @param   pBar        The BAR.
 * @param   offBar      Offset into the BAR, the caller made sure the access is inside of it.
 * @param   pv          Where to store what was read, or what to write.
 * @param   cb          Number of bytes to access.
 * @param   fWrite      Flag whether this is a write (true) or a read (false).
 */
static void pciVfioBarDevAccess(PVFIOPCI pThis, PCVFIOPCIFUN pFun, PCVFIOPCIBAR pBar, uint64_t offBar, void *pv, size_t cb,
                                bool fWrite)
{
    pciVfioFdAccess(pThis, pFun, pBar->offVfio + offBar, pv, cb, fWrite);
}


/**
 * Handles a guest access to a trapped segment of a memory BAR.
 *
 * The access is taken apart into what hits the MSI-X table, the MSI-X pending
 * bit array and the device memory around them.
 *
 * @param   pDevIns     The device instance.
 * @param   pSeg        The segment being accessed.
 * @param   off         Offset into the segment.
 * @param   pv          Where to store what was read, or what to write.
 * @param   cb          Number of bytes to access.
 * @param   fWrite      Flag whether this is a write (true) or a read (false).
 */
static void pciVfioBarSegAccess(PPDMDEVINS pDevIns, PCVFIOPCIBARSEG pSeg, RTGCPHYS off, void *pv, unsigned cb, bool fWrite)
{
    PVFIOPCI     pThis   = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PCVFIOPCIFUN pFun    = &pThis->aPciFuns[pSeg->iPciFun];
    PCVFIOPCIBAR pBar    = &pFun->aBars[pSeg->iBar];
    PPDMPCIDEV   pPciDev = pDevIns->apPciDevs[pSeg->iPciFun];
    bool const   fMsix   = pFun->offMsixCtrl && pFun->iBarMsix == pSeg->iBar;
    uint64_t     offBar  = pSeg->offBar + off;
    uint8_t     *pb      = (uint8_t *)pv;

    while (cb > 0)
    {
        /* Find out what the access hits first and how far that reaches. */
        enum { kNothing, kDevice, kMsixTbl, kMsixPba } enmWhat;
        uint64_t cbChunk = cb;
        if (offBar >= pBar->cbRegion)
            enmWhat = kNothing; /* The padding behind a BAR which doesn't end on a page boundary. */
        else
        {
            enmWhat = kDevice;
            cbChunk = RT_MIN(cbChunk, pBar->cbRegion - offBar);
            if (fMsix)
            {
                if (offBar - pFun->offMsixTbl < pFun->cbMsixTbl)
                {
                    enmWhat = kMsixTbl;
                    cbChunk = RT_MIN(cbChunk, (uint64_t)pFun->offMsixTbl + pFun->cbMsixTbl - offBar);
                }
                else if (offBar - pFun->offMsixPba < pFun->cbMsixPba)
                {
                    enmWhat = kMsixPba;
                    cbChunk = RT_MIN(cbChunk, (uint64_t)pFun->offMsixPba + pFun->cbMsixPba - offBar);
                }
                else
                {
                    if (offBar < pFun->offMsixTbl)
                        cbChunk = RT_MIN(cbChunk, pFun->offMsixTbl - offBar);
                    if (offBar < pFun->offMsixPba)
                        cbChunk = RT_MIN(cbChunk, pFun->offMsixPba - offBar);
                }
            }
        }

        switch (enmWhat)
        {
            case kDevice:
                pciVfioBarDevAccess(pThis, pFun, pBar, offBar, pb, (size_t)cbChunk, fWrite);
                break;
            case kMsixTbl:
                if (fWrite)
                    pciVfioMsixTblWrite(pDevIns, pFun, pPciDev, (uint32_t)(offBar - pFun->offMsixTbl), pb, (size_t)cbChunk);
                else
                    memcpy(pb, (uint8_t const *)pFun->paMsixTbl + (offBar - pFun->offMsixTbl), (size_t)cbChunk);
                break;
            case kMsixPba:
                /* Read-only for the guest. */
                if (!fWrite)
                    memcpy(pb, (uint8_t const *)pFun->pbmMsixPba + (offBar - pFun->offMsixPba), (size_t)cbChunk);
                break;
            default:
                if (!fWrite)
                    memset(pb, 0xff, (size_t)cbChunk);
                break;
        }

        offBar += cbChunk;
        pb     += cbChunk;
        cb     -= (unsigned)cbChunk;
    }
}


/**
 * @callback_method_impl{FNIOMMMIONEWREAD}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioMmioRead(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS off, void *pv, unsigned cb)
{
    pciVfioBarSegAccess(pDevIns, (PCVFIOPCIBARSEG)pvUser, off, pv, cb, false /*fWrite*/);
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMMMIONEWWRITE}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioMmioWrite(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS off, void const *pv, unsigned cb)
{
    pciVfioBarSegAccess(pDevIns, (PCVFIOPCIBARSEG)pvUser, off, (void *)(uintptr_t)pv, cb, true /*fWrite*/);
    return VINF_SUCCESS;
}


DECLINLINE(int) pciVfioQueryRegionInfo(PVFIOPCI pThis, PVFIOPCIFUN pFun, uint32_t uRegion, struct vfio_region_info *pRegionInfo)
{
    RT_ZERO(*pRegionInfo);
    pRegionInfo->argsz = sizeof(*pRegionInfo);
    pRegionInfo->index = uRegion;

    int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_GET_REGION_INFO, pRegionInfo);
    if (rcLnx == -1)
        return PDMDevHlpVMSetError(pThis->pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                N_("Getting information for region %u of opened VFIO device failed with %d"), uRegion, errno);

    const int iInstance = pThis->iInstance;
    LogRel(("VFIO#%d.%u: Region %u:\n"
            "VFIO#%d.%u:     flags:       %#RX32\n"
            "VFIO#%d.%u:     index:       %#RU32\n"
            "VFIO#%d.%u:     cap_offset:  %#RX32\n"
            "VFIO#%d.%u:     size:        %#RX64\n"
            "VFIO#%d.%u:     offset:      %#RX64\n",
            iInstance, pFun->uPciFun, uRegion,
            iInstance, pFun->uPciFun, pRegionInfo->flags,
            iInstance, pFun->uPciFun, pRegionInfo->index,
            iInstance, pFun->uPciFun, pRegionInfo->cap_offset,
            iInstance, pFun->uPciFun, pRegionInfo->size,
            iInstance, pFun->uPciFun, pRegionInfo->offset));

    return VINF_SUCCESS;
}


/**
 * Checks whether VFIO flags a region as one where the MSI-X table may be mapped.
 *
 * @returns VBox status code.
 * @param   pThis       The VFIO PCI device instance.
 * @param   pFun        The function.
 * @param   uRegion     The VFIO region.
 * @param   cbInfo      Size of the region information including its capabilities, as VFIO reported it.
 * @param   pfMsix      Where to return whether the region has the capability.
 */
static int pciVfioBarContainsMsix(PVFIOPCI pThis, PVFIOPCIFUN pFun, uint32_t uRegion, uint32_t cbInfo, bool *pfMsix)
{
    *pfMsix = false;
    if (   cbInfo < sizeof(struct vfio_region_info) + sizeof(struct vfio_info_cap_header)
        || cbInfo > _64K)
        return PDMDevHlpVMSetError(pThis->pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                   N_("The information for region %u of the VFIO device has an unexpected size of %u bytes"),
                                   uRegion, cbInfo);

    struct vfio_region_info *pRegionInfo = (struct vfio_region_info *)RTMemTmpAllocZ(cbInfo);
    if (!pRegionInfo)
        return VERR_NO_TMP_MEMORY;

    pRegionInfo->argsz = cbInfo;
    pRegionInfo->index = uRegion;

    int rc = VINF_SUCCESS;
    int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_GET_REGION_INFO, pRegionInfo);
    if (rcLnx != -1)
    {
        /* Walk the capabilities, they form a list linked by offsets into the buffer. */
        uint32_t offCap = pRegionInfo->cap_offset;
        for (unsigned cCapsLeft = 64; offCap != 0; cCapsLeft--)
        {
            if (   !cCapsLeft
                || offCap < sizeof(*pRegionInfo)
                || offCap > cbInfo - sizeof(struct vfio_info_cap_header))
            {
                rc = PDMDevHlpVMSetError(pThis->pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                         N_("The capabilities of region %u of the VFIO device are malformed"), uRegion);
                break;
            }

            struct vfio_info_cap_header const *pHdr = (struct vfio_info_cap_header const *)((uint8_t *)pRegionInfo + offCap);
            if (pHdr->id == VFIO_REGION_INFO_CAP_MSIX_MAPPABLE)
            {
                *pfMsix = true;
                break;
            }
            offCap = pHdr->next;
        }
    }
    else
        rc = PDMDevHlpVMSetError(pThis->pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                 N_("Getting information for region %u of opened VFIO device failed with %d"), uRegion, errno);

    RTMemTmpFree(pRegionInfo);
    return rc;
}


static int pciVfioSetupBar(PVFIOPCI pThis, PVFIOPCIFUN pFun, PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t uRegion, uint32_t uVfioRegion)
{
    struct vfio_region_info RegionInfo; RT_ZERO(RegionInfo);
    int rc = pciVfioQueryRegionInfo(pThis, pFun, uVfioRegion, &RegionInfo);
    if (RT_FAILURE(rc))
        return rc;

    pFun->aBars[uRegion].iPciFun = pFun->uPciFun;

    if (   RegionInfo.flags
        && RegionInfo.size)
    {
        uint32_t u32PciBar;
        rc = pciVfioCfgSpaceReadU32(pFun, VBOX_PCI_BASE_ADDRESS_0 + (uRegion * sizeof(uint32_t)), &u32PciBar);
        if (RT_FAILURE(rc))
            return rc;

        pFun->aBars[uRegion].cbRegion  = RegionInfo.size;
        if (u32PciBar & RT_BIT_32(0))
        {
            /* PIO. */
            pFun->aBars[uRegion].bType    = 1;
            pFun->aBars[uRegion].offVfio = RegionInfo.offset;

            rc = PDMDevHlpPCIIORegionCreateIoEx(pDevIns, pPciDev, uRegion, RegionInfo.size,
                                                pciVfioPioWrite, pciVfioPioRead, &pFun->aBars[uRegion],
                                                "PIO", NULL /*paExtDescs*/, &pFun->aBars[uRegion].hIoPort);
            AssertRCReturn(rc, PDMDEV_SET_ERROR(pDevIns, rc, N_("Cannot register PCI I/O region")));
        }
        else
        {
            /* Check whether this might be a region containing MSI-X state. */
            if (RegionInfo.flags & VFIO_REGION_INFO_FLAG_CAPS)
            {
                bool fMsix = false;

                rc = pciVfioBarContainsMsix(pThis, pFun, uVfioRegion, RegionInfo.argsz, &fMsix);
                if (RT_FAILURE(rc))
                    return rc;

                if (fMsix)
                    pFun->iBarMsix = uRegion;
            }

            /*
             * Map the region if VFIO allows it, if it can be read and written and if it
             * consists of whole pages.  A region which can't be mapped is accessed through
             * the device file descriptor, which means trapping every access of the guest.
             */
            PVFIOPCIBAR    pBar          = &pFun->aBars[uRegion];
            uint32_t const fVfioMappable = VFIO_REGION_INFO_FLAG_MMAP | VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
            pBar->bType   = 2;
            pBar->offVfio = RegionInfo.offset;
            pBar->pvMmio  = NULL;
            if (   (RegionInfo.flags & fVfioMappable) == fVfioMappable
                && !(RegionInfo.size & GUEST_PAGE_OFFSET_MASK))
            {
                void *pvMmio = mmap(NULL, RegionInfo.size, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, pFun->iFdVfio,
                                    RegionInfo.offset);
                if (pvMmio == MAP_FAILED)
                    return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                               N_("Mapping BAR%u at offset %#RX64 with size %RX64 failed with %d"),
                                               uRegion, RegionInfo.offset, RegionInfo.size, errno);
                pBar->pvMmio = pvMmio;
            }
            else
                LogRel(("VFIO#%d.%u: BAR%u can't be mapped (flags %#RX32, size %#RX64), trapping all accesses to it\n",
                        pThis->iInstance, pFun->uPciFun, uRegion, RegionInfo.flags, RegionInfo.size));

            uint32_t enmAddrSpace = PCI_ADDRESS_SPACE_MEM;
            if ((u32PciBar & (RT_BIT_32(2) | RT_BIT_32(1))) == PCI_ADDRESS_SPACE_BAR64)
                enmAddrSpace |= PCI_ADDRESS_SPACE_BAR64;
            if (u32PciBar & PCI_ADDRESS_SPACE_MEM_PREFETCH)
                enmAddrSpace |= PCI_ADDRESS_SPACE_MEM_PREFETCH;

            pBar->fInterceptMmio = pFun->fInterceptMmio || !pBar->pvMmio;
            pBar->enmAddrSpace   = (PCIADDRESSSPACE)enmAddrSpace;
            /* The MMIO regions get created at a later stage. */
        }
    }
    else /* Not available. */
        Assert(RegionInfo.size == 0);

    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMIOPORTNEWOUT}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioVgaPioWrite(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT offPort, uint32_t u32, unsigned cb)
{
    PVFIOPCI     pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PCVFIOPCIFUN pFun  = (PCVFIOPCIFUN)pvUser;
    AssertReturn(cb <= sizeof(u32), VINF_SUCCESS);

    pciVfioFdAccess(pThis, pFun, pFun->offVga + offPort, &u32, cb, true /*fWrite*/);
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMIOPORTNEWIN}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioVgaPioRead(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT offPort, uint32_t *pu32, unsigned cb)
{
    PVFIOPCI     pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PCVFIOPCIFUN pFun  = (PCVFIOPCIFUN)pvUser;
    AssertReturn(cb <= sizeof(*pu32), VERR_IOM_IOPORT_UNUSED);

    uint32_t u32 = UINT32_MAX;
    pciVfioFdAccess(pThis, pFun, pFun->offVga + offPort, &u32, cb, false /*fWrite*/);
    *pu32 = u32;
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMMMIONEWREAD}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioVgaMmioRead(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS off, void *pv, unsigned cb)
{
    PVFIOPCI     pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PCVFIOPCIFUN pFun  = (PCVFIOPCIFUN)pvUser;

    pciVfioFdAccess(pThis, pFun, pFun->offVga + off, pv, cb, false /*fWrite*/);
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMMMIONEWWRITE}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioVgaMmioWrite(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS off, void const *pv, unsigned cb)
{
    PVFIOPCI     pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PCVFIOPCIFUN pFun  = (PCVFIOPCIFUN)pvUser;

    pciVfioFdAccess(pThis, pFun, pFun->offVga + off, (void *)(uintptr_t)pv, cb, true /*fWrite*/);
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMMMIONEWFILL}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioVgaMmioFill(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS off, uint32_t u32Item, unsigned cbItem, unsigned cItems)
{
    PVFIOPCI     pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PCVFIOPCIFUN pFun  = (PCVFIOPCIFUN)pvUser;
    AssertReturn(cbItem == 1 || cbItem == 2 || cbItem == 4, VINF_SUCCESS);

    /* What is filled has to be inside the legacy video memory range, the offset is the absolute address. */
    if (   off <  VFIO_PCI_VGA_MEM_START
        || off >= VFIO_PCI_VGA_MEM_START + VFIO_PCI_VGA_MEM_SIZE)
        return VINF_SUCCESS;
    uint64_t cbLeft = RT_MIN((uint64_t)cbItem * cItems, VFIO_PCI_VGA_MEM_START + VFIO_PCI_VGA_MEM_SIZE - off);

    /* Write it in pieces from a buffer full of items; the buffer size is a multiple of all item sizes. */
    uint8_t abBuf[_1K];
    for (unsigned i = 0; i < sizeof(abBuf); i++)
        abBuf[i] = (uint8_t)(u32Item >> ((i % cbItem) * 8));
    while (cbLeft > 0)
    {
        size_t const cbChunk = (size_t)RT_MIN(cbLeft, sizeof(abBuf));
        pciVfioFdAccess(pThis, pFun, pFun->offVga + off, abBuf, cbChunk, true /*fWrite*/);
        off    += cbChunk;
        cbLeft -= cbChunk;
    }
    return VINF_SUCCESS;
}


static int pciVfioSetupVga(PVFIOPCI pThis, PVFIOPCIFUN pFun, PPDMDEVINS pDevIns)
{
    struct vfio_region_info RegionInfo; RT_ZERO(RegionInfo);
    int rc = pciVfioQueryRegionInfo(pThis, pFun, VFIO_PCI_VGA_REGION_INDEX, &RegionInfo);
    if (RT_FAILURE(rc))
        return rc;

    if (   !RegionInfo.flags
        || !RegionInfo.size)
        return PDMDevHlpVMSetError(pDevIns, VERR_NOT_SUPPORTED, RT_SRC_POS,
                                   N_("The device has no legacy VGA ranges which could be exposed to the guest"));

    pFun->offVga = RegionInfo.offset;

    /* Register the legacy VGA I/O port and MMIO ranges. */
    rc = PDMDevHlpIoPortCreateExAndMap(pDevIns, 0x3b0, 0x3bb - 0x3b0 + 1, IOM_IOPORT_F_ABS,
                                       pciVfioVgaPioWrite, pciVfioVgaPioRead, NULL, NULL, pFun, /** @todo String I/O */
                                       "VFIO VGA #1", NULL /*paExtDescs*/, &pFun->hVgaIoPort1);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, "Mapping legacy VGA ports 0x3b0 - 0x3bb failed");

    rc = PDMDevHlpIoPortCreateExAndMap(pDevIns, 0x3c0, 0x3df - 0x3c0 + 1, IOM_IOPORT_F_ABS,
                                       pciVfioVgaPioWrite, pciVfioVgaPioRead, NULL, NULL, pFun, /** @todo String I/O */
                                       "VFIO VGA #2", NULL /*paExtDescs*/, &pFun->hVgaIoPort2);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, "Mapping legacy VGA ports 0x3c0 - 0x3df failed");

    /*
     * The MDA/CGA/EGA/VGA/whatever fixed MMIO area.
     */
    rc = PDMDevHlpMmioCreateExAndMap(pDevIns, VFIO_PCI_VGA_MEM_START, VFIO_PCI_VGA_MEM_SIZE,
                                     IOMMMIO_FLAGS_READ_PASSTHRU | IOMMMIO_FLAGS_WRITE_PASSTHRU | IOMMMIO_FLAGS_ABS,
                                     NULL /*pPciDev*/, UINT32_MAX /*iPciRegion*/,
                                     pciVfioVgaMmioWrite, pciVfioVgaMmioRead, pciVfioVgaMmioFill, pFun,
                                     "VFIO VGA - VGA Video Buffer", &pFun->hVgaMmio);
    AssertRCReturn(rc, rc);

    return VINF_SUCCESS;
}


/**
 * HC access handler for the ROM.
 *
 * @returns VINF_SUCCESS if the handler have carried out the operation.
 * @returns VINF_PGM_HANDLER_DO_DEFAULT if the caller should carry out the access operation.
 * @param   pVM             VM Handle.
 * @param   pVCpu           The cross context CPU structure for the calling EMT.
 * @param   GCPhys          The physical address the guest is writing to.
 * @param   pvPhys          The HC mapping of that address.
 * @param   pvBuf           What the guest is reading/writing.
 * @param   cbBuf           How much it's reading/writing.
 * @param   enmAccessType   The access type.
 * @param   fOrigin         Who is making the access.
 * @param   uUser           User argument.
 */
static DECLCALLBACK(VBOXSTRICTRC)
pciVfioRomWriteHandler(PVM pVM, PVMCPU pVCpu, RTGCPHYS GCPhys, void *pvPhys, void *pvBuf, size_t cbBuf,
                          PGMACCESSTYPE enmAccessType, PGMACCESSORIGIN fOrigin, uint64_t uUser)
{
    RT_NOREF(pVM, pVCpu, GCPhys, pvPhys, pvBuf, cbBuf, enmAccessType, fOrigin, uUser);

    LogRelMax(10, ("VFIO: Attempted write access to ROM region\n"));
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNPCIIOREGIONMAP}
 */
DECLCALLBACK(int) pciVfioRomRegionMapUnmap(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t iRegion,
                                           RTGCPHYS GCPhysAddress, RTGCPHYS cb, PCIADDRESSSPACE enmType)
{
    RT_NOREF(iRegion, enmType);
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    int      rc;
    Assert(pPciDev == pDevIns->apPciDevs[pPciDev->uDevFn & 0x7]);
    PVFIOPCIFUN pFun = &pThis->aPciFuns[pPciDev->uDevFn & 0x7];

    Log(("pciVfioRomRegionMapUnmap: iRegion=%d GCPhysAddress=%RGp cb=%RGp enmType=%d\n", iRegion, GCPhysAddress, cb, enmType));
    if (GCPhysAddress != NIL_RTGCPHYS)
    {
        /* Map the ROM. */
        rc = PDMDevHlpMmio2Map(pDevIns, pFun->hRom, GCPhysAddress);
        AssertRC(rc);

        if (RT_SUCCESS(rc))
        {
            rc = PDMDevHlpPGMHandlerPhysicalRegister(pDevIns, GCPhysAddress, GCPhysAddress + cb - 1,
                                                     pFun->hRomAccHndType, "VFIO ROM");
            AssertRC(rc);
        }

        if (RT_SUCCESS(rc))
        {
            pFun->GCPhysRom = GCPhysAddress;
            Log(("pciVfioRomRegionMapUnmap: GCPhysRom=%RGp cb=%RGp\n", GCPhysAddress, cb));
        }
        rc = VINF_PCI_MAPPING_DONE; /* caller only cares about this status, so it is okay that we overwrite errors here. */
    }
    else
    {
        rc = PDMDevHlpPGMHandlerPhysicalDeregister(pDevIns, pFun->GCPhysRom);
        AssertRC(rc);
        pFun->GCPhysRom = NIL_RTGCPHYS;
    }
    return rc;
}


static int pciVfioSetupRom(PVFIOPCI pThis, PVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, PPDMDEVINS pDevIns)
{
    struct vfio_region_info RegionInfo; RT_ZERO(RegionInfo);
    int rc = pciVfioQueryRegionInfo(pThis, pFun, VFIO_PCI_ROM_REGION_INDEX, &RegionInfo);
    if (RT_FAILURE(rc))
        return rc;

    /* No ROM, nothing to do. */
    if (   RegionInfo.flags == 0
        && RegionInfo.size == 0)
        return VINF_SUCCESS;

    /** @todo Currently we will map the ROM as MMIO2 region as we lack the necessary
     * infrastructure to register ROMs for PCI BARs. This is wrong because MMIO2 regions
     * are mapped read/write. To protect against writes we put an access handler over the region,
     * which will discard (and log) all writes.
     */

    pFun->offRom = RegionInfo.offset;
    pFun->cbRom  = RegionInfo.size;

    rc = PDMDevHlpPCIIORegionCreateMmio2Ex(pDevIns, pPciDev, VBOX_PCI_ROM_SLOT, pFun->cbRom,
                                           PCI_ADDRESS_SPACE_MEM_PREFETCH, 0 /*fFlags*/,
                                           pciVfioRomRegionMapUnmap, "ROM",
                                           &pFun->pvRom, &pFun->hRom);
    AssertLogRelRCReturn(rc, PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                                 N_("Failed to allocate %zu bytes of ROM"), pFun->cbRom));

    ssize_t cbRead = pread(pFun->iFdVfio, pFun->pvRom, pFun->cbRom, pFun->offRom);
    if (cbRead != (ssize_t)pFun->cbRom)
        return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                   N_("Failed to read %zu bytes of ROM from the device"), pFun->cbRom);

    rc = PDMDevHlpPGMHandlerPhysicalTypeRegister(pThis->pDevIns, PGMPHYSHANDLERKIND_WRITE,
                                                 pciVfioRomWriteHandler, "VFIO ROM", &pFun->hRomAccHndType);
    AssertRCReturn(rc, rc);

    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNPCIIOREGIONMAP}
 */
DECLCALLBACK(int) pciVfioMmioMapUnmap(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t iRegion,
                                      RTGCPHYS GCPhysAddress, RTGCPHYS cb, PCIADDRESSSPACE enmType)
{
    RT_NOREF(enmType, cb);
    PVFIOPCI     pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    Assert(pPciDev == pDevIns->apPciDevs[pPciDev->idxSubDev]);
    PVFIOPCIFUN  pFun  = &pThis->aPciFuns[pPciDev->idxSubDev];
    PCVFIOPCIBAR pBar  = &pFun->aBars[iRegion];

    Log(("pciVfioMmioMapUnmap: iRegion=%d GCPhysAddress=%RGp cb=%RGp enmType=%d\n", iRegion, GCPhysAddress, cb, enmType));

    /* The segments of the BAR follow each other without gaps. */
    for (uint8_t iSeg = 0; iSeg < pBar->cSegs; iSeg++)
    {
        PCVFIOPCIBARSEG pSeg = &pBar->aSegs[iSeg];
        int             rc;
        if (GCPhysAddress != NIL_RTGCPHYS)
        {
            if (pSeg->fTrapped)
                rc = PDMDevHlpMmioMap(pDevIns, pSeg->hnd.hMmio, GCPhysAddress + pSeg->offBar);
            else
                rc = PDMDevHlpMmio2Map(pDevIns, pSeg->hnd.hMmio2, GCPhysAddress + pSeg->offBar);
        }
        else if (pSeg->fTrapped)
            rc = PDMDevHlpMmioUnmap(pDevIns, pSeg->hnd.hMmio);
        else
            rc = PDMDevHlpMmio2Unmap(pDevIns, pSeg->hnd.hMmio2);
        AssertRC(rc);
    }

    return VINF_PCI_MAPPING_DONE;
}


/**
 * Creates the MMIO and MMIO2 regions for a memory BAR and registers the BAR
 * with the PCI bus.
 *
 * The guest gets to see the BAR with the size it has on the device.  The device
 * memory is mapped into the guest, except for the pages holding the MSI-X table
 * and the MSI-X pending bit array, which are trapped.  All of the BAR is
 * trapped if it can't be mapped or if this was requested in the configuration.
 *
 * @returns VBox status code.
 * @param   pThis       The VFIO PCI device instance.
 * @param   pFun        The function the BAR belongs to.
 * @param   pPciDev     The PCI device of the function.
 * @param   iBar        The BAR.
 */
static int pciVfioBarSetupMmio(PVFIOPCI pThis, PVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint8_t iBar)
{
    PPDMDEVINS     pDevIns      = pThis->pDevIns;
    PVFIOPCIBAR    pBar         = &pFun->aBars[iBar];
    uint64_t const cbBarAligned = RT_ALIGN_64(pBar->cbRegion, GUEST_PAGE_SIZE);

    /*
     * Work out the page ranges to trap, sorted by offset.
     */
    struct
    {
        uint64_t offStart;
        uint64_t offEnd;
    }        aTraps[2];
    unsigned cTraps = 0;
    RT_ZERO(aTraps);
    if (pBar->fInterceptMmio)
    {
        aTraps[0].offStart = 0;
        aTraps[0].offEnd   = cbBarAligned;
        cTraps = 1;
    }
    else if (   pFun->offMsixCtrl
             && pFun->iBarMsix == iBar)
    {
        uint64_t const offTblStart = pFun->offMsixTbl & ~(uint64_t)GUEST_PAGE_OFFSET_MASK;
        uint64_t const offPbaStart = pFun->offMsixPba & ~(uint64_t)GUEST_PAGE_OFFSET_MASK;
        unsigned const iTbl        = offTblStart <= offPbaStart ? 0 : 1;
        aTraps[iTbl].offStart     = offTblStart;
        aTraps[iTbl].offEnd       = RT_ALIGN_64((uint64_t)pFun->offMsixTbl + pFun->cbMsixTbl, GUEST_PAGE_SIZE);
        aTraps[iTbl ^ 1].offStart = offPbaStart;
        aTraps[iTbl ^ 1].offEnd   = RT_ALIGN_64((uint64_t)pFun->offMsixPba + pFun->cbMsixPba, GUEST_PAGE_SIZE);
        cTraps = 2;
        if (aTraps[1].offStart <= aTraps[0].offEnd)
        {
            /* The same or neighbouring pages, make it one range. */
            aTraps[0].offEnd = RT_MAX(aTraps[0].offEnd, aTraps[1].offEnd);
            cTraps = 1;
        }
    }

    /*
     * Split the BAR into segments accordingly.
     */
    uint64_t offBar = 0;
    pBar->cSegs = 0;
    for (unsigned i = 0; i <= cTraps; i++)
    {
        uint64_t const offEndMapped = i < cTraps ? aTraps[i].offStart : cbBarAligned;
        if (offEndMapped > offBar)
        {
            AssertReturn(pBar->cSegs < RT_ELEMENTS(pBar->aSegs), VERR_INTERNAL_ERROR_3);
            PVFIOPCIBARSEG pSeg = &pBar->aSegs[pBar->cSegs++];
            pSeg->iPciFun  = (uint8_t)pFun->uPciFun;
            pSeg->iBar     = iBar;
            pSeg->fTrapped = false;
            pSeg->offBar   = offBar;
            pSeg->cbSeg    = offEndMapped - offBar;
        }
        if (i < cTraps)
        {
            AssertReturn(pBar->cSegs < RT_ELEMENTS(pBar->aSegs), VERR_INTERNAL_ERROR_3);
            PVFIOPCIBARSEG pSeg = &pBar->aSegs[pBar->cSegs++];
            pSeg->iPciFun  = (uint8_t)pFun->uPciFun;
            pSeg->iBar     = iBar;
            pSeg->fTrapped = true;
            pSeg->offBar   = aTraps[i].offStart;
            pSeg->cbSeg    = aTraps[i].offEnd - aTraps[i].offStart;
            offBar = aTraps[i].offEnd;
        }
    }

    /*
     * Create the regions.
     */
    for (uint8_t iSeg = 0; iSeg < pBar->cSegs; iSeg++)
    {
        PVFIOPCIBARSEG pSeg = &pBar->aSegs[iSeg];
        int            rc;
        if (pSeg->fTrapped)
            rc = PDMDevHlpMmioCreate(pDevIns, pSeg->cbSeg, pPciDev, iBar /*iPciRegion*/,
                                     pciVfioMmioWrite, pciVfioMmioRead, pSeg,
                                     IOMMMIO_FLAGS_READ_PASSTHRU | IOMMMIO_FLAGS_WRITE_PASSTHRU, "MMIO",
                                     &pSeg->hnd.hMmio);
        else
            rc = PDMDevHlpMmio2CreateFromExisting(pDevIns, pPciDev, iBar + iSeg * 0x10 /* has to be unique for the PCI device */,
                                                  pSeg->cbSeg, 0 /*fFlags*/, "MMIO",
                                                  (uint8_t *)pBar->pvMmio + pSeg->offBar, &pSeg->hnd.hMmio2);
        AssertLogRelRCReturn(rc, rc);
    }

    /* The mapping is done by us as it depends on the segments. */
    return pDevIns->pHlpR3->pfnPCIIORegionRegister(pDevIns, pPciDev, iBar, pBar->cbRegion, pBar->enmAddrSpace,
                                                   PDMPCIDEV_IORGN_F_NO_HANDLE | PDMPCIDEV_IORGN_F_NEW_STYLE,
                                                   UINT64_MAX, pciVfioMmioMapUnmap);
}


/**
 * The IRQ poller thread.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pThread     The command thread.
 */
static DECLCALLBACK(int) pciVfioIrqPoller(PPDMDEVINS pDevIns, PPDMTHREAD pThread)
{
    PVFIOPCI    pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PVFIOPCIFUN pFun = (PVFIOPCIFUN)pThread->pvUser;
    PPDMPCIDEV  pPciDev = pDevIns->apPciDevs[pFun->uPciFun];

    if (pThread->enmState == PDMTHREADSTATE_INITIALIZING)
        return VINF_SUCCESS;

    /* The poll structure for the interrupts, big enough for the wakeup eventfd and every vector we have an eventfd for. */
    uint32_t const  cEntriesAlloc = pFun->cFdEvts + 1;
    struct pollfd   *paIrqFds = (struct pollfd *)RTMemAllocZ(cEntriesAlloc * sizeof(*paIrqFds));
    if (!paIrqFds)
    {
        LogRel(("VFIO#%d.%u: Failed to allocate memory for %u interrupt polling entries",
                pThis->iInstance, pFun->uPciFun, cEntriesAlloc));
        return VERR_NO_MEMORY;
    }

    paIrqFds[0].fd = pFun->iFdWakeup;
    paIrqFds[0].events = POLLIN | POLLRDNORM | POLLRDBAND | POLLPRI | POLLERR;

    uint32_t cIrqs = 0;
    while (pThread->enmState == PDMTHREADSTATE_RUNNING)
    {
        /* The sequence number has to be read before the mode, see pciVfioIrqPollerSwitchMode(). */
        uint32_t const uIrqSeqReq  = ASMAtomicReadU32(&pFun->uIrqSeqReq);
        uint8_t const  uIrqModeNew = ASMAtomicReadU8(&pFun->uIrqModeNew);
        if (pFun->uIrqModeCur != uIrqModeNew)
        {
            if (uIrqModeNew < RT_ELEMENTS(pFun->acIrqVectors))
                cIrqs = RT_MIN(pFun->acIrqVectors[uIrqModeNew], pFun->cFdEvts);
            else
                cIrqs = 0;

            for (uint32_t i = 0; i < cIrqs; i++)
            {
                paIrqFds[i + 1].fd      = pFun->paFdEvts[i];
                paIrqFds[i + 1].events  = POLLIN | POLLRDNORM | POLLRDBAND | POLLPRI | POLLERR;
                paIrqFds[i + 1].revents = 0;
            }

            ASMAtomicWriteU8(&pFun->uIrqModeCur, uIrqModeNew);
        }

        /* Inform the initiator of a mode switch that we are done with it. */
        if (ASMAtomicReadU32(&pFun->uIrqSeqAck) != uIrqSeqReq)
        {
            ASMAtomicWriteU32(&pFun->uIrqSeqAck, uIrqSeqReq);
            RTThreadUserSignal(pThread->Thread);
        }

        int rcPsx = poll(&paIrqFds[0], 1 + cIrqs, -1);
        if (rcPsx > 0)
        {
            if (paIrqFds[0].revents)
            {
                /* We got woken up externally. */
                paIrqFds[0].revents = 0;
                uint64_t u64;
                ssize_t cb = read(paIrqFds[0].fd, &u64, sizeof(u64));
                Assert(cb == sizeof(u64)); RT_NOREF(cb);
                if (pThread->enmState != PDMTHREADSTATE_RUNNING)
                    break;
            }

            for (uint32_t i = 1; i < cIrqs + 1; i++)
            {
                if (paIrqFds[i].revents)
                {
                    paIrqFds[i].revents = 0;
                    uint64_t u64;
                    ssize_t cb = read(paIrqFds[i].fd, &u64, sizeof(u64));
                    Assert(cb == sizeof(u64)); RT_NOREF(cb);

                    uint16_t const uVec = i - 1;
                    if (pFun->uIrqModeCur == VFIO_PCI_MSIX_IRQ_INDEX)
                        pciVfioMsixRaise(pDevIns, pFun, pPciDev, uVec);
                    else if (pFun->uIrqModeCur == VFIO_PCI_MSI_IRQ_INDEX)
                        pciVfioMsiRaise(pDevIns, pFun, pPciDev, uVec);
                    else
                        PDMDevHlpPCISetIrqEx(pDevIns, pPciDev, uVec, 1);

                    if (pFun->uIrqModeCur == VFIO_PCI_INTX_IRQ_INDEX)
                    {
                        /** @todo The interrupt seems to be masked and we would need a mechanism
                         * to get notified when the interrupt is de-asserted in the interrupt controller
                         * (through an EOI for example) so we can unmask them. With KVM this is supported
                         * when KVM_CAP_IRQFD_RESAMPLE is available.
                         */
#if 0
                        union
                        {
                            struct vfio_irq_set IrqSet;
                            uint32_t            au32[sizeof(struct vfio_irq_set) / sizeof(uint32_t) + 1];
                        } uBuf;

                        uBuf.IrqSet.argsz = sizeof(uBuf);
                        uBuf.IrqSet.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_UNMASK;
                        uBuf.IrqSet.index = i;
                        uBuf.IrqSet.start = 0;
                        uBuf.IrqSet.count = 1;
                        uBuf.au32[sizeof(struct vfio_irq_set) / sizeof(uint32_t)] = pThis->aIrqFds[i].fd;

                        int rcLnx = ioctl(pThis->iFdVfio, VFIO_DEVICE_SET_IRQS, &uBuf);
                        if (rcLnx == -1)
                            LogRel(("Unmasking one INTX interrupt failed with %d", errno));
#endif
                    }
                }
            }
        }
        else
            AssertFailed();
    }

    RTMemFree(paIrqFds);
    LogFlowFunc(("Poller thread terminating\n"));
    return VINF_SUCCESS;
}


/**
 * Wakes up the IRQ poller to respond to state changes.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pThread     The command thread.
 */
static DECLCALLBACK(int) pciVfioIrqPollerWakeup(PPDMDEVINS pDevIns, PPDMTHREAD pThread)
{
    RT_NOREF(pDevIns);
    Log4Func(("\n"));
    PVFIOPCIFUN pFun = (PVFIOPCIFUN)pThread->pvUser;

    uint64_t u64 = 1;
    ssize_t cb = write(pFun->iFdWakeup, &u64, sizeof(u64));
    Assert(cb == sizeof(u64)); RT_NOREF(cb);

    return VINF_SUCCESS;
}


/**
 * Makes the IRQ poller switch to the given mode.
 *
 * If the poller is running this waits until it did, so the caller can go on
 * changing the interrupt setup of the device without the poller looking at the
 * eventfds with a wrong idea of what they mean.  A poller which is not running
 * (the VM is being constructed, is suspended or is powering off) does not look
 * at the eventfds at all and picks up the mode when it gets going.
 *
 * @returns VBox status code.
 * @retval  VERR_TIMEOUT if the poller did not react in time.
 * @param   pThis       The VFIO PCI device instance.
 * @param   pFun        The function the poller belongs to.
 * @param   uVfioIrq    The VFIO interrupt index to switch to, UINT8_MAX for none.
 */
static int pciVfioIrqPollerSwitchMode(PVFIOPCI pThis, PVFIOPCIFUN pFun, uint8_t uVfioIrq)
{
    PPDMTHREAD const pThread = pFun->pThrdIrq;
    if (!pThread)
    {
        /* No poller yet, it starts out in the mode set here. */
        ASMAtomicWriteU8(&pFun->uIrqModeNew, uVfioIrq);
        return VINF_SUCCESS;
    }

    /* The mode has to be written before the sequence number, the poller reads them the other way around. */
    RTThreadUserReset(pThread->Thread);
    ASMAtomicWriteU8(&pFun->uIrqModeNew, uVfioIrq);
    uint32_t const uIrqSeqReq = ASMAtomicIncU32(&pFun->uIrqSeqReq);
    pciVfioIrqPollerWakeup(pThis->pDevIns, pThread);

    if (pThread->enmState != PDMTHREADSTATE_RUNNING)
        return VINF_SUCCESS;

    /* Wait until it got confirmed. */
    uint64_t const msStart = RTTimeMilliTS();
    for (;;)
    {
        if (ASMAtomicReadU32(&pFun->uIrqSeqAck) == uIrqSeqReq)
            return VINF_SUCCESS;

        uint64_t const cMsElapsed = RTTimeMilliTS() - msStart;
        if (cMsElapsed >= 5 * RT_MS_1SEC)
            return VERR_TIMEOUT;
        RTThreadUserWait(pThread->Thread, (RTMSINTERVAL)(5 * RT_MS_1SEC - cMsElapsed));
        RTThreadUserReset(pThread->Thread);
    }
}


DECLINLINE(int) pciVfioQueryIrqInfo(PVFIOPCI pThis, PCVFIOPCIFUN pFun, uint32_t uIrq, uint32_t *pcVectors)
{
    struct vfio_irq_info IrqInfo;
    RT_ZERO(IrqInfo);
    IrqInfo.argsz = sizeof(IrqInfo);
    IrqInfo.index = uIrq;

    int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_GET_IRQ_INFO, &IrqInfo);
    if (rcLnx == -1)
        return PDMDevHlpVMSetError(pThis->pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                N_("Getting information for irq %u of opened VFIO device failed with %d"), uIrq, errno);

    const int iInstance = pThis->iInstance;
    LogRel(("VFIO#%d.%u: Irq %u:\n"
            "VFIO#%d.%u:     flags:       %#RX32\n"
            "VFIO#%d.%u:     index:       %#RU32\n"
            "VFIO#%d.%u:     count:       %#RU32\n",
            iInstance, pFun->uPciFun, uIrq,
            iInstance, pFun->uPciFun, IrqInfo.flags,
            iInstance, pFun->uPciFun, IrqInfo.index,
            iInstance, pFun->uPciFun, IrqInfo.count));

    *pcVectors = IrqInfo.count;
    return VINF_SUCCESS;
}


/**
 * Sets up the given interrupt type with VFIO, tearing down what is currently
 * set up if it differs.
 *
 * @returns VBox status code.
 * @param   pThis       The VFIO PCI device instance.
 * @param   pFun        The function to configure.
 * @param   uVfioIrq    The VFIO interrupt index to set up, UINT8_MAX for none.
 * @param   cVectors    The number of vectors to enable; none is set up if this is 0.
 */
static int pciVfioIrqReconfigure(PVFIOPCI pThis, PVFIOPCIFUN pFun, uint8_t uVfioIrq, uint32_t cVectors)
{
    /* There are no interrupts to come from a function which is out of service. */
    if (pFun->fGone)
        return VINF_SUCCESS;

    if (!cVectors)
        uVfioIrq = UINT8_MAX;
    else if (uVfioIrq == UINT8_MAX)
        cVectors = 0;

    if (   pFun->uIrqMode    == uVfioIrq
        && pFun->cIrqVectors == cVectors)
        return VINF_SUCCESS;

    /*
     * Tear down the current setup.  The poller has to stop looking at the
     * eventfds before the meaning of them changes.
     */
    if (pFun->uIrqMode != UINT8_MAX)
    {
        int rc = pciVfioIrqPollerSwitchMode(pThis, pFun, UINT8_MAX);
        if (RT_FAILURE(rc))
            return rc;

        struct vfio_irq_set IrqSet;
        IrqSet.argsz = sizeof(IrqSet);
        IrqSet.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
        IrqSet.index = pFun->uIrqMode;
        IrqSet.start = 0;
        IrqSet.count = 0;

        int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_SET_IRQS, &IrqSet);
        if (rcLnx == -1)
            return RTErrConvertFromErrno(errno);

        pFun->uIrqMode    = UINT8_MAX;
        pFun->cIrqVectors = 0;
    }

    if (uVfioIrq == UINT8_MAX)
        return VINF_SUCCESS;

    /*
     * Set up the new one.  The number of vectors is limited by what the device
     * has and by the number of eventfds created for it.
     */
    AssertReturn(uVfioIrq < RT_ELEMENTS(pFun->acIrqVectors), VERR_INVALID_PARAMETER);
    AssertReturn(   cVectors <= pFun->acIrqVectors[uVfioIrq]
                 && cVectors <= pFun->cFdEvts,
                 VERR_OUT_OF_RANGE);

    size_t const         cbIrqSet = sizeof(struct vfio_irq_set) + cVectors * sizeof(int32_t);
    struct vfio_irq_set *pIrqSet  = (struct vfio_irq_set *)RTMemTmpAllocZ(cbIrqSet);
    if (!pIrqSet)
        return VERR_NO_TMP_MEMORY;

    pIrqSet->argsz = (uint32_t)cbIrqSet;
    pIrqSet->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    pIrqSet->index = uVfioIrq;
    pIrqSet->start = 0;
    pIrqSet->count = cVectors;

    int32_t *paiFds = (int32_t *)(pIrqSet + 1);
    for (uint32_t i = 0; i < cVectors; i++)
        paiFds[i] = pFun->paFdEvts[i];

    int       rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_SET_IRQS, pIrqSet);
    int const rc    = rcLnx != -1 ? VINF_SUCCESS : RTErrConvertFromErrno(errno);
    RTMemTmpFree(pIrqSet);
    if (RT_FAILURE(rc))
        return rc;

    pFun->uIrqMode    = uVfioIrq;
    pFun->cIrqVectors = cVectors;
    return pciVfioIrqPollerSwitchMode(pThis, pFun, uVfioIrq);
}


/**
 * Brings the interrupt setup of the device in line with what the guest has
 * configured.
 *
 * MSI-X takes precedence over MSI, which takes precedence over INTx.
 *
 * @returns VBox status code.
 * @param   pThis       The VFIO PCI device instance.
 * @param   pFun        The function to configure.
 * @param   u16Cmd      The PCI command register.
 * @param   u16MsiCtrl  The MSI message control register, ignored if the function has no MSI capability.
 * @param   u16MsixCtrl The MSI-X message control register, ignored if the function has no MSI-X capability.
 */
static int pciVfioIrqUpdate(PVFIOPCI pThis, PVFIOPCIFUN pFun, uint16_t u16Cmd, uint16_t u16MsiCtrl, uint16_t u16MsixCtrl)
{
    uint8_t  uVfioIrq = UINT8_MAX;
    uint32_t cVectors = 0;
    if (   pFun->offMsixCtrl
        && (u16MsixCtrl & VBOX_PCI_MSIX_FLAGS_ENABLE))
    {
        uVfioIrq = VFIO_PCI_MSIX_IRQ_INDEX;
        cVectors = pFun->acIrqVectors[VFIO_PCI_MSIX_IRQ_INDEX];
    }
    else if (   pFun->offMsiCtrl
             && (u16MsiCtrl & VBOX_PCI_MSI_FLAGS_ENABLE))
    {
        /*
         * The guest picks how many of the vectors it wants to use with the
         * Multiple Message Enable field.  Whatever it writes there, it can't
         * get more than the device has.
         */
        uVfioIrq = VFIO_PCI_MSI_IRQ_INDEX;
        cVectors = RT_MIN(RT_BIT_32((u16MsiCtrl & VBOX_PCI_MSI_FLAGS_QSIZE) >> 4), pFun->acIrqVectors[VFIO_PCI_MSI_IRQ_INDEX]);
    }
    else if (   pFun->acIrqVectors[VFIO_PCI_INTX_IRQ_INDEX]
             && !(u16Cmd & VBOX_PCI_COMMAND_INTX_DISABLE))
    {
        uVfioIrq = VFIO_PCI_INTX_IRQ_INDEX;
        cVectors = 1;
    }

    int rc = pciVfioIrqReconfigure(pThis, pFun, uVfioIrq, cVectors);
    if (RT_FAILURE(rc))
        LogRelMax(32, ("VFIO#%d.%u: Failed to set up interrupt type %u with %u vector(s), expect a broken device (%Rrc)\n",
                       pThis->iInstance, pFun->uPciFun, uVfioIrq, cVectors, rc));
    return rc;
}


/**
 * Sets how reads and writes of a config space byte are handled, replacing what
 * was set for it before.
 *
 * @param   pFun        The function.
 * @param   off         The config space offset of the byte.
 * @param   fRd         How to handle reads, VFIO_PCI_CFG_SPACE_ACCESS_XXX.
 * @param   fWr         How to handle writes, VFIO_PCI_CFG_SPACE_ACCESS_XXX.
 */
DECLINLINE(void) pciVfioCfgSpaceSetInterceptU8(PVFIOPCIFUN pFun, uint32_t off, uint8_t fRd, uint8_t fWr)
{
    AssertReturnVoid(off < sizeof(pFun->abPciCfgIntercept) * 8 / 4);
    uint32_t offByte = off >> 1;
    uint8_t  cShift  = (off & 0x1) ? 4 : 0;

    pFun->abPciCfgIntercept[offByte] = (uint8_t)(  (pFun->abPciCfgIntercept[offByte] & ~(0xf << cShift))
                                                 | ((((fWr & 0x3) << 2) | (fRd & 0x3)) << cShift));
}


DECLINLINE(void) pciVfioCfgSpaceSetInterceptRoU8(PVFIOPCIFUN pFun, uint32_t off, uint8_t fRd)
{
    pciVfioCfgSpaceSetInterceptU8(pFun, off,     fRd, VFIO_PCI_CFG_SPACE_ACCESS_INVALID);
}


DECLINLINE(void) pciVfioCfgSpaceSetInterceptU16(PVFIOPCIFUN pFun, uint32_t off, uint8_t fRd, uint8_t fWr)
{
    pciVfioCfgSpaceSetInterceptU8(pFun, off,     fRd, fWr);
    pciVfioCfgSpaceSetInterceptU8(pFun, off + 1, fRd, fWr);
}


DECLINLINE(void) pciVfioCfgSpaceSetInterceptRoU16(PVFIOPCIFUN pFun, uint32_t off, uint8_t fRd)
{
    pciVfioCfgSpaceSetInterceptU8(pFun, off,     fRd, VFIO_PCI_CFG_SPACE_ACCESS_INVALID);
    pciVfioCfgSpaceSetInterceptU8(pFun, off + 1, fRd, VFIO_PCI_CFG_SPACE_ACCESS_INVALID);
}


DECLINLINE(void) pciVfioCfgSpaceSetInterceptU32(PVFIOPCIFUN pFun, uint32_t off, uint8_t fRd, uint8_t fWr)
{
    pciVfioCfgSpaceSetInterceptU16(pFun, off,     fRd, fWr);
    pciVfioCfgSpaceSetInterceptU16(pFun, off + 2, fRd, fWr);
}


DECLINLINE(void) pciVfioCfgSpaceSetInterceptRoU32(PVFIOPCIFUN pFun, uint32_t off, uint8_t fRd)
{
    pciVfioCfgSpaceSetInterceptRoU16(pFun, off,     fRd);
    pciVfioCfgSpaceSetInterceptRoU16(pFun, off + 2, fRd);
}


DECLINLINE(uint8_t) pciVfioCfgSpaceGetInterceptRd(PVFIOPCIFUN pFun, uint32_t off)
{
    AssertReturn(off < sizeof(pFun->abPciCfgIntercept) * 8 / 4, VFIO_PCI_CFG_SPACE_ACCESS_INVALID);

    uint32_t offByte = off >> 1;
    uint8_t  cShift  = (off & 0x1) ? 4 : 0;

    return (pFun->abPciCfgIntercept[offByte] >> cShift) & 0x3;
}


DECLINLINE(uint8_t) pciVfioCfgSpaceGetInterceptWr(PVFIOPCIFUN pFun, uint32_t off)
{
    AssertReturn(off < sizeof(pFun->abPciCfgIntercept) * 8 / 4, VFIO_PCI_CFG_SPACE_ACCESS_INVALID);

    uint32_t offByte = off >> 1;
    uint8_t  cShift  = (off & 0x1) ? 4 + 2 : 2;

    return (pFun->abPciCfgIntercept[offByte] >> cShift) & 0x3;
}


static int pciVfioTrySetupMsix(PVFIOPCI pThis, PVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint8_t offCap)
{
    PPDMDEVINS pDevIns = pThis->pDevIns;

    /*
     * The MSI-X table and the pending bit array of the device belong to the host
     * kernel, which programs the table for delivering the interrupts to our
     * eventfds.  The guest gets a table and a pending bit array of its own at the
     * place where the device has them, see pciVfioBarSetupMmio() and
     * pciVfioBarSegAccess().
     */

    /* First get at the information from the capability. */
    uint16_t u16Mmc = 0;
    int rc = pciVfioCfgSpaceReadU16(pFun, offCap + 2, &u16Mmc);
    if (RT_FAILURE(rc))
        return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                   N_("Failed to read MSI-X message control register with %Rrc"), rc);

    uint32_t u32TblCfg = 0;
    rc = pciVfioCfgSpaceReadU32(pFun, offCap + 4, &u32TblCfg);
    if (RT_FAILURE(rc))
        return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                   N_("Failed to read MSI-X table config register with %Rrc"), rc);

    uint32_t u32PbaCfg = 0;
    rc = pciVfioCfgSpaceReadU32(pFun, offCap + 8, &u32PbaCfg);
    if (RT_FAILURE(rc))
        return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                   N_("Failed to read MSI-X PBA config register with %Rrc"), rc);

    uint32_t const cVectors = (u16Mmc & 0x7ff) + 1;
    uint8_t  const uTblBir  = u32TblCfg & 0x7;
    uint8_t  const uPbaBir  = u32PbaCfg & 0x7;
    uint32_t const offTbl   = u32TblCfg & UINT32_C(0xfffffff8);
    uint32_t const offPba   = u32PbaCfg & UINT32_C(0xfffffff8);
    uint32_t const cbTbl    = cVectors * sizeof(MSIXTBLENTRY);
    uint32_t const cbPba    = RT_ALIGN_32(cVectors, 64) / 8;
    uint64_t const cbBar    = uTblBir < 6 ? pFun->aBars[uTblBir].cbRegion : 0;

    if (   uTblBir >= 6                                            /* 6 and 7 are reserved, indicates a broken device. */
        || uTblBir != uPbaBir                                      /* Same region */
        || uTblBir != pFun->iBarMsix                               /* BAR doesn't match what VFIO presented to us. */
        || pFun->aBars[uTblBir].bType != 2                         /* Has to be a memory BAR. */
        || cVectors != pFun->acIrqVectors[VFIO_PCI_MSIX_IRQ_INDEX] /* The indicated table size must match what VFIO reported as number of IRQ vectors. */
        || (uint64_t)offTbl + cbTbl > cbBar                        /* The table has to be inside the BAR ... */
        || (uint64_t)offPba + cbPba > cbBar                        /* ... and so does the PBA. */
        || (   offTbl < (uint64_t)offPba + cbPba                   /* PBA must not intersect table. */
            && offPba < (uint64_t)offTbl + cbTbl)
       )
    {
        /* Just log and don't set an error. */
        LogRel(("VFIO#%d.%u: MSI-X not supported for this device as it doesn't meet the requirements: uTblBir=%u uPbaBir=%u offTbl=%#x offPba=%#x cVectors=%u cVfioVectors=%u cbBar=%#RX64\n",
                pThis->iInstance, pFun->uPciFun, uTblBir, uPbaBir, offTbl, offPba, cVectors,
                pFun->acIrqVectors[VFIO_PCI_MSIX_IRQ_INDEX], cbBar));
        return VERR_NOT_SUPPORTED;
    }

    pFun->paMsixTbl  = (PMSIXTBLENTRY)RTMemAllocZ(cbTbl);
    pFun->pbmMsixPba = (volatile uint64_t *)RTMemAllocZ(cbPba);
    if (!pFun->paMsixTbl || !pFun->pbmMsixPba)
    {
        LogRel(("VFIO#%d.%u: Failed to allocate the MSI-X state for %u vectors, MSI-X not supported for this device\n",
                pThis->iInstance, pFun->uPciFun, cVectors));
        RTMemFree(pFun->paMsixTbl);
        RTMemFree((void *)pFun->pbmMsixPba);
        pFun->paMsixTbl  = NULL;
        pFun->pbmMsixPba = NULL;
        return VERR_NO_MEMORY;
    }

    /* Each vector is masked by default. */
    for (uint32_t i = 0; i < cVectors; i++)
        pFun->paMsixTbl[i].u32VecCtrl = RT_BIT_32(0);

    pFun->offMsixTbl = offTbl;
    pFun->cbMsixTbl  = cbTbl;
    pFun->offMsixPba = offPba;
    pFun->cbMsixPba  = cbPba;

    /* Configure the capability if everything succeeded.  MSI-X starts out disabled for the guest. */
    pFun->offMsixCtrl = offCap + 2;
    PDMPciDevSetWord( pPciDev, offCap + 2, u16Mmc & ~(VBOX_PCI_MSIX_FLAGS_ENABLE | VBOX_PCI_MSIX_FLAGS_FUNCMASK)); /* Message control */
    PDMPciDevSetDWord(pPciDev, offCap + 4, offTbl | uTblBir); /* Table config */
    PDMPciDevSetDWord(pPciDev, offCap + 8, offPba | uPbaBir); /* PBA config   */
    pciVfioCfgSpaceSetInterceptU16(  pFun, offCap + 2, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_EMULATE);    /* Message Control */
    pciVfioCfgSpaceSetInterceptRoU32(pFun, offCap + 4, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT);                                       /* Table config */
    pciVfioCfgSpaceSetInterceptRoU32(pFun, offCap + 8, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT);                                       /* PBA config */

    return VINF_SUCCESS;
}


static int pciVfioCfgSpaceParseCapabilities(PVFIOPCI pThis, PVFIOPCIFUN pFun, PPDMPCIDEV pPciDev)
{
    PPDMDEVINS pDevIns = pThis->pDevIns;

    /* Initialize with 0. */
    PDMPciDevSetCapabilityList(pPciDev, 0);

    /*
     * Try building a 1:1 mapping of the capabilities, punching holes for capabilities
     * currently not being supported.
     */
    uint8_t offCap = 0;
    int rc = pciVfioCfgSpaceReadU8(pFun, VBOX_PCI_CAPABILITY_LIST, &offCap);
    if (RT_FAILURE(rc))
        return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                   N_("Failed to read capabilities list pointer with %Rrc"), rc);

    pciVfioCfgSpaceSetInterceptRoU8(pFun, VBOX_PCI_CAPABILITY_LIST, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT);
    offCap &= 0xfc; /* The bottom two bits are reserved. */
    if (!offCap)
    {
        /* No capabilities, return early. */
        return VINF_SUCCESS;
    }

    /*
     * The capabilities can come in any order.  Keep track of the config space
     * bytes claimed by the capabilities seen so far, in order to catch lists
     * which loop and capabilities which overlap.
     */
    uint64_t bmClaimed[256 / 64];
    RT_ZERO(bmClaimed);
    uint8_t offCapNextPrev = VBOX_PCI_CAPABILITY_LIST;
    for (;;)
    {
        if (offCap < 0x40)
            return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                       N_("The capability pointer %#x points into the PCI header"), offCap);

        uint8_t bCapId = 0;
        rc = pciVfioCfgSpaceReadU8(pFun, offCap, &bCapId);
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Failed to read capabilitiy ID at offset %#x with %Rrc"), offCap, rc);

        uint8_t offCapNext = 0;
        rc = pciVfioCfgSpaceReadU8(pFun, offCap + 1, &offCapNext);
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Failed to read next capability pointer at offset %#x with %Rrc"), offCap, rc);
        offCapNext &= 0xfc; /* The bottom two bits are reserved. */

        uint8_t cbCap = 2;
        bool    fSupported = false;
        switch (bCapId)
        {
            case VBOX_PCI_CAP_ID_PM:  LogRel(("VFIO#%d.%u: Cap[%#x]: PCI Power Management Interface -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap)); break;
            case VBOX_PCI_CAP_ID_AGP: LogRel(("VFIO#%d.%u: Cap[%#x]: AGP -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap));                            break;
            case VBOX_PCI_CAP_ID_VPD:
            {
                LogRel(("VFIO#%d.%u: Cap[%#x]: VPD -> passthrough\n", pThis->iInstance, pFun->uPciFun, offCap));
                pciVfioCfgSpaceSetInterceptU16(pFun, offCap + 2, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH);
                pciVfioCfgSpaceSetInterceptU32(pFun, offCap + 4, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH);
                cbCap      = 2 + 2 + 4;
                fSupported = true;
                break;
            }
            case VBOX_PCI_CAP_ID_SLOTID: LogRel(("VFIO#%d.%u: Cap[%#x]: Slot Identification -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap)); break;
            case VBOX_PCI_CAP_ID_MSI:
            {
                /* There is one per function.  Whatever a second one is, the guest doesn't get to see it. */
                if (pFun->offMsiCtrl)
                {
                    LogRel(("VFIO#%d.%u: Cap[%#x]: Message Signaled Interrupts -> unsupported, the function has one already\n",
                            pThis->iInstance, pFun->uPciFun, offCap));
                    break;
                }

                /* Read the message control from the device. */
                uint16_t u16Mmc = 0;
                rc = pciVfioCfgSpaceReadU16(pFun, offCap + 2, &u16Mmc);
                if (RT_FAILURE(rc))
                    return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                               N_("Failed to read MSI message control register with %Rrc"), rc);

                uint32_t const cVectors = pFun->acIrqVectors[VFIO_PCI_MSI_IRQ_INDEX];
                if (!cVectors)
                {
                    LogRel(("VFIO#%d.%u: Cap[%#x]: Message Signaled Interrupts -> unsupported, VFIO has no vectors for it\n",
                            pThis->iInstance, pFun->uPciFun, offCap));
                    break;
                }

                LogRel(("VFIO#%d.%u: Cap[%#x]: Message Signaled Interrupts -> emulate\n", pThis->iInstance, pFun->uPciFun, offCap));
                fSupported = true;

                /*
                 * The guest gets the capability with the layout it has on the device, so it
                 * never extends into whatever follows in the config space.  The registers are
                 * all emulated here, see pciVfioCfgEmulatedRegWrite() and pciVfioMsiRaise();
                 * the MSI support of the PCI bus is not used as it knows one layout only.
                 */
                bool const    f64Bit    = RT_BOOL(u16Mmc & VBOX_PCI_MSI_FLAGS_64BIT);
                bool const    fMaskable = RT_BOOL(u16Mmc & VBOX_PCI_MSI_FLAGS_MASKBIT);
                uint8_t const offData   = offCap + (f64Bit ? VBOX_MSI_CAP_MESSAGE_DATA_64 : VBOX_MSI_CAP_MESSAGE_DATA_32);
                cbCap = (uint8_t)(offData - offCap) + (fMaskable ? 2 + 2 + 4 + 4 : 2);
                if ((uint32_t)offCap + cbCap > 256)
                    return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                               N_("The MSI capability at offset %#x doesn't fit into the PCI config space"), offCap);

                /* The message control register describes the device: the layout and as many vectors
                   as VFIO has for us.  MSI starts out disabled. */
                uint32_t cVectorsLog2 = (u16Mmc & VBOX_PCI_MSI_FLAGS_QMASK) >> 1;
                while (cVectorsLog2 > 0 && RT_BIT_32(cVectorsLog2) > cVectors)
                    cVectorsLog2--;
                PDMPciDevSetWord(pPciDev, offCap + VBOX_MSI_CAP_MESSAGE_CONTROL,
                                   (u16Mmc & (VBOX_PCI_MSI_FLAGS_64BIT | VBOX_PCI_MSI_FLAGS_MASKBIT))
                                 | (uint16_t)(cVectorsLog2 << 1));
                for (uint32_t off = offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_LO; off < (uint32_t)offCap + cbCap; off++)
                    PDMPciDevSetByte(pPciDev, off, 0);

                pciVfioCfgSpaceSetInterceptU16(pFun, offCap + VBOX_MSI_CAP_MESSAGE_CONTROL,    VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_EMULATE); /* Message Control */
                pciVfioCfgSpaceSetInterceptU32(pFun, offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_LO, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_EMULATE); /* Message Address [Low] */
                if (f64Bit)
                    pciVfioCfgSpaceSetInterceptU32(pFun, offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_HI, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_EMULATE); /* Message Address High */
                pciVfioCfgSpaceSetInterceptU16(pFun, offData, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_EMULATE);          /* Message Data */
                if (fMaskable)
                {
                    pciVfioCfgSpaceSetInterceptRoU16(pFun, offData + 2, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT);                                  /* Reserved */
                    pciVfioCfgSpaceSetInterceptU32(  pFun, offData + 4, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_EMULATE); /* Mask */
                    pciVfioCfgSpaceSetInterceptRoU32(pFun, offData + 8, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT);                                  /* Pending */
                }

                pFun->offMsiCtrl   = offCap + 2;
                pFun->fMsi64Bit    = f64Bit;
                pFun->fMsiMaskable = fMaskable;
                break;
            }
            case VBOX_PCI_CAP_ID_CHSWP: LogRel(("VFIO#%d.%u: Cap[%#x]: CompactPCI Hot Swap -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap)); break;
            case VBOX_PCI_CAP_ID_PCIX:  LogRel(("VFIO#%d.%u: Cap[%#x]: PCI-X -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap));               break;
            case VBOX_PCI_CAP_ID_HT:    LogRel(("VFIO#%d.%u: Cap[%#x]: HyperTransport -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap));      break;
            case VBOX_PCI_CAP_ID_VNDR:
            {
                LogRel(("VFIO#%d.%u: Cap[%#x]: Vendor Specific -> passthrough\n", pThis->iInstance, pFun->uPciFun, offCap));

                /* The next byte after the header is a length field. */
                uint8_t cbVendor = 0;
                rc = pciVfioCfgSpaceReadU8(pFun, offCap + 2, &cbVendor);
                if (RT_FAILURE(rc))
                    return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                               N_("Failed to read vendor length field at offset %#x with %Rrc"), offCap + 2, rc);
                if (cbVendor < 2 || cbVendor > 256 - offCap)
                    return PDMDevHlpVMSetError(pDevIns, VERR_BUFFER_OVERFLOW, RT_SRC_POS,
                                               N_("Invalid vendor length field %#x"), cbVendor);

                for (uint8_t i = 0; i < cbVendor - 2; i++)
                    pciVfioCfgSpaceSetInterceptU8(pFun, offCap + 2 + i, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH);

                cbCap      = cbVendor;
                fSupported = true;
                break;
            }
            case VBOX_PCI_CAP_ID_DBG:    LogRel(("VFIO#%d.%u: Cap[%#x]: Debug port -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap));                          break;
            case VBOX_PCI_CAP_ID_CCRC:   LogRel(("VFIO#%d.%u: Cap[%#x]: CompactPCI central resource control -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap)); break;
            case VBOX_PCI_CAP_ID_SHPC:   LogRel(("VFIO#%d.%u: Cap[%#x]: Standard PCI Hot-Plug Controller -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap));    break;
            case VBOX_PCI_CAP_ID_SSVID:  LogRel(("VFIO#%d.%u: Cap[%#x]: SPCI Bridge Subsystem Vendor ID -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap));     break;
            case VBOX_PCI_CAP_ID_AGP3:   LogRel(("VFIO#%d.%u: Cap[%#x]: AGP 8x -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap));                              break;
            case VBOX_PCI_CAP_ID_SECURE: LogRel(("VFIO#%d.%u: Cap[%#x]: Secure Device -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap));                       break;
            case VBOX_PCI_CAP_ID_EXP:
            {
                LogRel(("VFIO#%d.%u: Cap[%#x]: PCI Express -> emulate\n", pThis->iInstance, pFun->uPciFun, offCap));
                fSupported = true;

#if 1 /** @todo Proper support. */
                uint16_t u16ExpReg = 0;
                rc = pciVfioCfgSpaceReadU16(pFun, offCap + 2, &u16ExpReg);
                if (RT_FAILURE(rc))
                    return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                               N_("Failed to read PCI Express Capabilities Register at offset %#x with %Rrc"), offCap + 2, rc);

                uint8_t const bVers = u16ExpReg & 0xf;
                uint8_t cbCap2 = 0;
                if (bVers == 1)
                    cbCap2 = 36;
                else if (bVers == 2)
                    cbCap2 = 60;

                if (!cbCap2)
                {
                    LogRel(("VFIO#%d.%u: Cap[%#x]: PCI Express capability version %u is unknown -> unsupported\n",
                            pThis->iInstance, pFun->uPciFun, offCap, bVers));
                    fSupported = false;
                    break;
                }
                if ((uint32_t)offCap + cbCap2 > 256)
                    return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                               N_("The PCI Express capability at offset %#x doesn't fit into the PCI config space"), offCap);

                for (uint16_t i = offCap + 2; i < (uint16_t)offCap + cbCap2; i += 2)
                    pciVfioCfgSpaceSetInterceptU16(pFun, i, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH);
                cbCap = cbCap2;
#endif

                break;
            }
            case VBOX_PCI_CAP_ID_MSIX:
            {
                /* There is one per function.  Whatever a second one is, the guest doesn't get to see it. */
                if (pFun->offMsixCtrl)
                {
                    LogRel(("VFIO#%d.%u: Cap[%#x]: MSI-X -> unsupported, the function has one already\n",
                            pThis->iInstance, pFun->uPciFun, offCap));
                    break;
                }
                if ((uint32_t)offCap + 12 > 256)
                    return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                               N_("The MSI-X capability at offset %#x doesn't fit into the PCI config space"), offCap);

                LogRel(("VFIO#%d.%u: Cap[%#x]: MSI-X -> emulate\n", pThis->iInstance, pFun->uPciFun, offCap));
                rc = pciVfioTrySetupMsix(pThis, pFun, pPciDev, offCap);
                if (RT_SUCCESS(rc))
                {
                    fSupported = true;
                    cbCap      = 12;
                }
                else if (rc != VERR_NOT_SUPPORTED) /* Not supported means doing without it, anything else is a failure. */
                    return rc;
                break;
            }
            case VBOX_PCI_CAP_ID_SATA: LogRel(("VFIO#%d.%u: Cap[%#x]: Serial ATA HBA -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap));        break;
            case VBOX_PCI_CAP_ID_AF:   LogRel(("VFIO#%d.%u: Cap[%#x]: PCI Advanced Features -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap)); break;
            default:
                LogRel(("VFIO#%d.%u: Cap[%#x]: Unknown capability ID %#x encountered -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapId));
                break;
        }

        /*
         * Whatever the capability is, it has to be inside the config space and must
         * not touch what a capability seen earlier has claimed.  The latter also
         * catches a list which loops.
         */
        if ((uint32_t)offCap + cbCap > 256)
            return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                       N_("The capability at offset %#x with %u bytes doesn't fit into the PCI config space"),
                                       offCap, cbCap);
        for (uint32_t off = offCap; off < (uint32_t)offCap + cbCap; off++)
            if (ASMBitTestAndSet(&bmClaimed[0], (int32_t)off))
                return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                           N_("The capability at offset %#x overlaps with another one or the capability list loops"),
                                           offCap);

        if (fSupported)
        {
            /* Accesses to capability ID and pointer to the next capability are never passed through. */
            pciVfioCfgSpaceSetInterceptRoU16(pFun, offCap, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT);
            PDMPciDevSetByte(pPciDev, offCap, bCapId);
            PDMPciDevSetByte(pPciDev, offCapNextPrev, offCap);
            offCapNextPrev = offCap + 1;
        }

        if (!offCapNext)
            break;

        offCap = offCapNext;
    }

    /* Mark the end of the list. */
    PDMPciDevSetByte(pPciDev, offCapNextPrev, 0);

    return VINF_SUCCESS;
}


static int pciVfioCfgSpaceParseExtCapabilities(PVFIOPCI pThis, PVFIOPCIFUN pFun, PPDMPCIDEV pPciDev)
{
    PPDMDEVINS pDevIns = pThis->pDevIns;
    RT_NOREF(pPciDev);

    uint16_t offCap = 256;

    /*
     * The capabilities can come in any order.  Keep track of the capability
     * headers seen so far in order to catch lists which loop.
     */
    uint64_t bmVisited[4096 / sizeof(uint32_t) / 64];
    RT_ZERO(bmVisited);
    for (;;)
    {
        if (   offCap < 256
            || offCap > 4096 - sizeof(uint32_t))
            return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                       N_("The extended capability pointer %#x is invalid"), offCap);
        if (ASMBitTestAndSet(&bmVisited[0], (int32_t)(offCap / sizeof(uint32_t))))
            return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                       N_("The extended capability list loops at offset %#x"), offCap);

        uint32_t u32CapHdr = 0;
        int rc = pciVfioCfgSpaceReadU32(pFun, offCap, &u32CapHdr);
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Failed to read extended capabilitiy header at offset %#x with %Rrc"), offCap, rc);

        if (   !u32CapHdr
            || u32CapHdr == UINT32_MAX)
            break;

        uint16_t const u16CapId   = u32CapHdr & UINT16_C(0xffff);
        uint8_t  const bCapVers   = (u32CapHdr >> 16) & 0xf; RT_NOREF(bCapVers);
        uint16_t const offCapNext = (u32CapHdr >> 20) & 0xffc; /* The bottom two bits are reserved. */
        bool fSupported = false;
        switch (u16CapId)
        {
            case VBOX_PCI_EXT_CAP_ID_ERR:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Advanced Error Reporting -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));               break;
            case VBOX_PCI_EXT_CAP_ID_VC:      LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Virtual Channel -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                        break;
            case VBOX_PCI_EXT_CAP_ID_DSN:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Device Serial Number -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                   break;
            case VBOX_PCI_EXT_CAP_ID_PWR:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Power Budgeting -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                        break;
            case VBOX_PCI_EXT_CAP_ID_RCLINK:  LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Root Complex Link Declaration -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));          break;
            case VBOX_PCI_EXT_CAP_ID_RCILINK: LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Coot Complex Internal Link Declaration -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers)); break;
            case VBOX_PCI_EXT_CAP_ID_RCECOLL: LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Root Complex Event Collector -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));           break;
            case VBOX_PCI_EXT_CAP_ID_MFVC:    LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Multi-Function Virtual Channel -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));         break;
            case VBOX_PCI_EXT_CAP_ID_RBCB:    LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Root Bridge Control Block -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));              break;
            case VBOX_PCI_EXT_CAP_ID_VNDR:    LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Vendor Specific -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                        break;
            case VBOX_PCI_EXT_CAP_ID_ACS:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Access Controls -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                        break;
            case VBOX_PCI_EXT_CAP_ID_ARI:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Alternative Routing ID -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                 break;
            case VBOX_PCI_EXT_CAP_ID_ATS:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Address Translation Service -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));            break;
            case VBOX_PCI_EXT_CAP_ID_SRIOV:   LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Single Root I/O Virtualization -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));         break;
            case VBOX_PCI_EXT_CAP_ID_MCAST:   LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Multicast -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                              break;
            case VBOX_PCI_EXT_CAP_ID_RESZBAR: LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Resizable BAR -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                          break;
            case VBOX_PCI_EXT_CAP_ID_DPA:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Dynamic Power Allocation -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));               break;
            case VBOX_PCI_EXT_CAP_ID_TPH:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: TPH Requester -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                          break;
            case VBOX_PCI_EXT_CAP_ID_LTR:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Latency Tolerance Reporting -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));            break;
            case VBOX_PCI_EXT_CAP_ID_SECPCIE: LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Secondary PCI Express -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                  break;
            case VBOX_PCI_EXT_CAP_ID_PASID:   LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Process Address Space Identifier -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));       break;
            case VBOX_PCI_EXT_CAP_ID_LNR:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: LN Requester -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                           break;
            case VBOX_PCI_EXT_CAP_ID_DPC:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Downstream Port Containment -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));            break;
            case VBOX_PCI_EXT_CAP_ID_L1PM:    LogRel(("VFIO#%d.%u: Cap[%#x v%u]: L1 PM Substates -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                        break;
            case VBOX_PCI_EXT_CAP_ID_PTM:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Precision Time Management -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));              break;
            case VBOX_PCI_EXT_CAP_ID_MPCIE:   LogRel(("VFIO#%d.%u: Cap[%#x v%u]: M-PCI Express -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));                          break;
            case VBOX_PCI_EXT_CAP_ID_FRS:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Function Readiness Status -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));              break;
            case VBOX_PCI_EXT_CAP_ID_RTR:     LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Readiness Time Reporting -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));               break;
            case VBOX_PCI_EXT_CAP_ID_DVSEC:   LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Desginated Vendor-Specific -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers));             break;
            default:
                LogRel(("VFIO#%d.%u: Cap[%#x v%u]: Unknown capability ID %#x encountered -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapVers, u16CapId));
                break;
        }

        if (fSupported)
        {
            /** @todo */
        }

        if (!offCapNext)
            break;

        offCap = offCapNext;
    }

#if 1 /** @todo Proper support. */
    for (uint32_t i = 256; i < 4096; i += 4)
        pciVfioCfgSpaceSetInterceptU32(pFun, i, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH);
#endif

    return VINF_SUCCESS;
}


/**
 * The descriptors for the standard PCI config space.
 */
static const struct
{
    uint8_t offReg;
    uint8_t cbReg;
    bool    fInitFromDev;
    uint8_t fRd;
    uint8_t fWr;
} s_aCfgSpaceDesc[] =
{
    { VBOX_PCI_VENDOR_ID,           sizeof(uint16_t), true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_DEVICE_ID,           sizeof(uint16_t), true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_COMMAND,             sizeof(uint16_t), true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_EMULATE     },
    { VBOX_PCI_STATUS,              sizeof(uint16_t), true,  VFIO_PCI_CFG_SPACE_ACCESS_EMULATE,     VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH },
    { VBOX_PCI_REVISION_ID,         sizeof(uint8_t),  true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_CLASS_PROG,          sizeof(uint8_t),  true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_CLASS_SUB,           sizeof(uint8_t),  true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_CLASS_BASE,          sizeof(uint8_t),  true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_CACHE_LINE_SIZE,     sizeof(uint8_t),  false, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH },
    { VBOX_PCI_LATENCY_TIMER,       sizeof(uint8_t),  false, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH },
    { VBOX_PCI_HEADER_TYPE,         sizeof(uint8_t),  false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_BIST,                sizeof(uint8_t),  false, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH },
    { VBOX_PCI_BASE_ADDRESS_0,      sizeof(uint32_t), false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  },
    { VBOX_PCI_BASE_ADDRESS_1,      sizeof(uint32_t), false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  },
    { VBOX_PCI_BASE_ADDRESS_2,      sizeof(uint32_t), false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  },
    { VBOX_PCI_BASE_ADDRESS_3,      sizeof(uint32_t), false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  },
    { VBOX_PCI_BASE_ADDRESS_4,      sizeof(uint32_t), false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  },
    { VBOX_PCI_BASE_ADDRESS_5,      sizeof(uint32_t), false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  },
    { VBOX_PCI_CARDBUS_CIS,         sizeof(uint32_t), false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  },
    { VBOX_PCI_SUBSYSTEM_VENDOR_ID, sizeof(uint16_t), true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_SUBSYSTEM_ID,        sizeof(uint16_t), true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_ROM_ADDRESS,         sizeof(uint32_t), false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  },
    { VBOX_PCI_INTERRUPT_LINE,      sizeof(uint8_t),  false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  },
    { VBOX_PCI_INTERRUPT_PIN,       sizeof(uint8_t),  false, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_MIN_GNT,             sizeof(uint8_t),  true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
    { VBOX_PCI_MAX_LAT,             sizeof(uint8_t),  true,  VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT,  VFIO_PCI_CFG_SPACE_ACCESS_INVALID     },
};

static int pciVfioCfgSpaceSetup(PVFIOPCIFUN pFun, PPDMPCIDEV pPciDev)
{
    for (uint32_t i = 0; i < RT_ELEMENTS(s_aCfgSpaceDesc); i++)
    {
        switch (s_aCfgSpaceDesc[i].cbReg)
        {
            case 1:
            {
                if (s_aCfgSpaceDesc[i].fInitFromDev)
                {
                    uint8_t u8;
                    int rc = pciVfioCfgSpaceReadU8(pFun, s_aCfgSpaceDesc[i].offReg, &u8);
                    if (RT_FAILURE(rc)) return rc;
                    PDMPciDevSetByte(pPciDev, s_aCfgSpaceDesc[i].offReg, u8);
                }
                pciVfioCfgSpaceSetInterceptU8(pFun, s_aCfgSpaceDesc[i].offReg, s_aCfgSpaceDesc[i].fRd, s_aCfgSpaceDesc[i].fWr);
                break;
            }
            case 2:
            {
                if (s_aCfgSpaceDesc[i].fInitFromDev)
                {
                    uint16_t u16;
                    int rc = pciVfioCfgSpaceReadU16(pFun, s_aCfgSpaceDesc[i].offReg, &u16);
                    if (RT_FAILURE(rc)) return rc;
                    PDMPciDevSetWord(pPciDev, s_aCfgSpaceDesc[i].offReg, u16);
                }
                pciVfioCfgSpaceSetInterceptU16(pFun, s_aCfgSpaceDesc[i].offReg, s_aCfgSpaceDesc[i].fRd, s_aCfgSpaceDesc[i].fWr);
                break;
            }
            case 4:
            {
                if (s_aCfgSpaceDesc[i].fInitFromDev)
                {
                    uint32_t u32;
                    int rc = pciVfioCfgSpaceReadU32(pFun, s_aCfgSpaceDesc[i].offReg, &u32);
                    if (RT_FAILURE(rc)) return rc;
                    PDMPciDevSetDWord(pPciDev, s_aCfgSpaceDesc[i].offReg, u32);
                }
                pciVfioCfgSpaceSetInterceptU32(pFun, s_aCfgSpaceDesc[i].offReg, s_aCfgSpaceDesc[i].fRd, s_aCfgSpaceDesc[i].fWr);
                break;
            }
            default:
                AssertReleaseFailed();
        }
    }

    return VINF_SUCCESS;
}


/**
 * Maps a piece of guest memory for DMA by the device.
 *
 * @returns VBox status code.
 * @param   pThis           The VFIO PCI device instance.
 * @param   GCPhysStart     The guest physical address of the memory, which is the address the device uses for it.
 * @param   uPtrMapping     Where the memory is in this process.
 * @param   cbMapping       Size of the piece.
 */
static int pciVfioMapRegion(PVFIOPCI pThis, RTGCPHYS GCPhysStart, uintptr_t uPtrMapping, size_t cbMapping)
{
    int rcLnx;
    if (pThis->fVfioLegacy)
    {
        struct vfio_iommu_type1_dma_map Map;
        Map.argsz      = sizeof(Map);
        Map.flags      = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
        Map.vaddr      = uPtrMapping;
        Map.size       = cbMapping;
        Map.iova       = GCPhysStart;

        rcLnx = ioctl(pThis->VfioGroup.iFdVfioContainer, VFIO_IOMMU_MAP_DMA, &Map);
    }
    else
    {
        struct iommu_ioas_map Map;
        Map.size       = sizeof(Map);
        Map.flags      = IOMMU_IOAS_MAP_FIXED_IOVA | IOMMU_IOAS_MAP_WRITEABLE | IOMMU_IOAS_MAP_READABLE;
        Map.ioas_id    = pThis->IommuFd.idIommuHwpt;
        Map.__reserved = 0;
        Map.user_va    = uPtrMapping;
        Map.length     = cbMapping;
        Map.iova       = GCPhysStart;

        rcLnx = ioctl(pThis->IommuFd.iFdIommu, IOMMU_IOAS_MAP, &Map);
    }
    if (rcLnx == -1)
    {
        int const iErrno = errno;
        LogRel(("VFIO#%d: Mapping %zu bytes of guest memory at %RGp for the device failed with %d\n",
                pThis->iInstance, cbMapping, GCPhysStart, iErrno));
        return RTErrConvertFromErrno(iErrno);
    }

    return VINF_SUCCESS;
}


/**
 * Removes everything which is mapped for DMA by the device.
 *
 * @param   pThis           The VFIO PCI device instance.
 */
static void pciVfioIommuUnmapAll(PVFIOPCI pThis)
{
    int rcLnx;
    if (pThis->fVfioLegacy)
    {
        struct vfio_iommu_type1_dma_unmap Unmap;
        Unmap.argsz   = sizeof(Unmap);
        Unmap.flags   = VFIO_DMA_UNMAP_FLAG_ALL;
        Unmap.iova    = 0;
        Unmap.size    = 0;

        rcLnx = ioctl(pThis->VfioGroup.iFdVfioContainer, VFIO_IOMMU_UNMAP_DMA, &Unmap);
    }
    else
    {
        struct iommu_ioas_unmap Unmap;
        Unmap.size    = sizeof(Unmap);
        Unmap.ioas_id = pThis->IommuFd.idIommuHwpt;
        Unmap.iova    = 0;
        Unmap.length  = UINT64_MAX; /* Everything. */

        rcLnx = ioctl(pThis->IommuFd.iFdIommu, IOMMU_IOAS_UNMAP, &Unmap);
    }
    if (rcLnx == -1)
        LogRel(("VFIO#%d: Removing the guest memory mappings of the device failed with %d\n", pThis->iInstance, errno));
    pThis->fGuestRamMapped = false;
}


/**
 * Maps the guest RAM in a guest physical address range for DMA by the device.
 *
 * @returns VBox status code.  A part of the range may be mapped on failure.
 * @param   pThis           The VFIO PCI device instance.
 * @param   pDevIns         The device instance.
 * @param   GCPhysStart     Start of the range.
 * @param   cbMap           Size of the range.
 */
static int pciVfioIommuGuestRamMapRange(PVFIOPCI pThis, PPDMDEVINS pDevIns, RTGCPHYS GCPhysStart, RTGCPHYS cbMap)
{
    /** @todo This is a really gross hack because we currently lack
     * a dedicated interface to get knowledge about guest RAM mappings.
     * This might also return mappings for stuff not being guest RAM.
     * Also this will allocate all guest RAM instantly but there is no way
     * around that anyway in order for the IOMMU to work correctly.
     */
    int       rc          = VINF_SUCCESS;
    RTGCPHYS  GCPhysRun   = GCPhysStart;
    uintptr_t uPtrMapping = 0;
    size_t    cbMapping   = 0;
    for (RTGCPHYS GCPhys = GCPhysStart; GCPhys < GCPhysStart + cbMap && RT_SUCCESS(rc); GCPhys += GUEST_PAGE_SIZE)
    {
        void          *pv = NULL;
        PGMPAGEMAPLOCK Lock;
        int rc2 = PDMDevHlpPhysGCPhys2CCPtr(pDevIns, GCPhys, 0 /*fFlags*/, &pv, &Lock);
        if (RT_SUCCESS(rc2))
        {
            PDMDevHlpPhysReleasePageMappingLock(pDevIns, &Lock);
            if (   cbMapping
                && uPtrMapping + cbMapping == (uintptr_t)pv)
                cbMapping += GUEST_PAGE_SIZE;
            else
            {
                /* The memory continues elsewhere in this process: map what we have and start over. */
                if (cbMapping)
                    rc = pciVfioMapRegion(pThis, GCPhysRun, uPtrMapping, cbMapping);
                GCPhysRun   = GCPhys;
                uPtrMapping = (uintptr_t)pv;
                cbMapping   = GUEST_PAGE_SIZE;
            }
        }
        else if (cbMapping)
        {
            /* Not RAM: map what we have. */
            rc = pciVfioMapRegion(pThis, GCPhysRun, uPtrMapping, cbMapping);
            cbMapping = 0;
        }
    }

    if (   RT_SUCCESS(rc)
        && cbMapping)
        rc = pciVfioMapRegion(pThis, GCPhysRun, uPtrMapping, cbMapping);
    return rc;
}


/**
 * Maps all of the guest RAM for DMA by the device.
 *
 * Nothing is left mapped if this fails.
 *
 * @returns VBox status code.
 * @param   pThis           The VFIO PCI device instance.
 * @param   pDevIns         The device instance.
 */
static int pciVfioIommuGuestRamMap(PVFIOPCI pThis, PPDMDEVINS pDevIns)
{
    if (pThis->fGuestRamMapped)
        return VINF_SUCCESS;

    int rc = pciVfioIommuGuestRamMapRange(pThis, pDevIns,   0 /*GCPhysStart*/, PDMDevHlpMMPhysGetRamSizeBelow4GB(pDevIns));
    if (RT_SUCCESS(rc))
        rc = pciVfioIommuGuestRamMapRange(pThis, pDevIns, _4G /*GCPhysStart*/, PDMDevHlpMMPhysGetRamSizeAbove4GB(pDevIns));
    if (RT_SUCCESS(rc))
        pThis->fGuestRamMapped = true;
    else
        pciVfioIommuUnmapAll(pThis);
    return rc;
}


static int pciVfioConfigPassthroughRead(PVFIOPCIFUN pFun, uint32_t uAddress, unsigned cb, uint32_t *pu32Value)
{
    switch (cb)
    {
        case 1:
        {
            uint8_t u8;
            int rc = pciVfioCfgSpaceReadU8(pFun, uAddress, &u8);
            if (RT_FAILURE(rc)) return rc;
            *pu32Value = u8;
            break;
        }
        case 2:
        {
            uint16_t u16;
            int rc = pciVfioCfgSpaceReadU16(pFun, uAddress, &u16);
            if (RT_FAILURE(rc)) return rc;
            *pu32Value = u16;
            break;
        }
        case 4:
        {
            uint32_t u32;
            int rc = pciVfioCfgSpaceReadU32(pFun, uAddress, &u32);
            if (RT_FAILURE(rc)) return rc;
            *pu32Value = u32;
            break;
        }
        default:
            AssertFailedReturn(VERR_INVALID_PARAMETER);
    }

    return VINF_SUCCESS;
}


static int pciVfioConfigPassthroughWrite(PVFIOPCIFUN pFun, uint32_t uAddress, unsigned cb, uint32_t u32Value)
{
    switch (cb)
    {
        case 1:
        {
            int rc = pciVfioCfgSpaceWriteU8(pFun, uAddress, (uint8_t)u32Value);
            if (RT_FAILURE(rc)) return rc;
            break;
        }
        case 2:
        {
            int rc = pciVfioCfgSpaceWriteU16(pFun, uAddress, (uint16_t)u32Value);
            if (RT_FAILURE(rc)) return rc;
            break;
        }
        case 4:
        {
            int rc = pciVfioCfgSpaceWriteU32(pFun, uAddress, (uint32_t)u32Value);
            if (RT_FAILURE(rc)) return rc;
            break;
        }
        default:
            AssertFailedReturn(VERR_INVALID_PARAMETER);
    }

    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNPCICONFIGREAD}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioConfigRead(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev,
                                                    uint32_t uAddress, unsigned cb, uint32_t *pu32Value)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PVFIOPCIFUN pFun = &pThis->aPciFuns[pPciDev->idxSubDev];

    /*
     * A function which is out of service reads like one which isn't there.  Not
     * before the guest mappings of its BARs are removed though: the PCI bus gets
     * what it needs to know for that through here too.
     */
    if (pFun->fGoneForGuest)
    {
        *pu32Value = cb < sizeof(uint32_t) ? RT_BIT_32(cb * 8) - 1 : UINT32_MAX;
        return VINF_SUCCESS;
    }

    uint32_t u32 = 0;
    for (uint8_t i = 0; i < cb; i++)
    {
        uint32_t bRead = 0;
        uint8_t fRd = pciVfioCfgSpaceGetInterceptRd(pFun, uAddress + i);
        switch (fRd)
        {
            case VFIO_PCI_CFG_SPACE_ACCESS_INVALID:
            {
                Log(("VFIO#%d.%u: Invalid PCI config read at offset %#x, returning 0\n", pThis->iInstance, pFun->uPciFun, uAddress + i));
                break;
            }
            case VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH:
            {
                int rc = pciVfioConfigPassthroughRead(pFun, uAddress + i, 1, &bRead);
                if (RT_FAILURE(rc))
                    LogRelMax(32, ("VFIO#%d.%u: Failed to passthrough PCI config read at offset %#x with %Rrc, returning 0\n",
                                   pThis->iInstance, pFun->uPciFun, uAddress + i, rc));
                break;
            }
            case VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT:
            {
                bRead = PDMPciDevGetByte(pPciDev, uAddress + i);
                break;
            }
            case VFIO_PCI_CFG_SPACE_ACCESS_EMULATE:
            {
                switch (uAddress + i)
                {
                    case VBOX_PCI_STATUS:
                    case VBOX_PCI_STATUS + 1:
                    {
                        uint16_t u16;
                        int rc = pciVfioCfgSpaceReadU16(pFun, VBOX_PCI_STATUS, &u16);
                        if (RT_SUCCESS(rc))
                        {
                            /* Reflect proper capabilities bit. */
                            uint8_t offCap = PDMPciDevGetCapabilityList(pPciDev);
                            if (offCap)
                                u16 |= RT_BIT(4);
                            else
                                u16 &= ~RT_BIT(4);
                            bRead = (u16 >> ((uAddress + i) == VBOX_PCI_STATUS ? 0 : 8)) & 0xff;
                        }
                        break;
                    }
                    default: /* Nothing else is emulated for reading, reads as zero. */
                        AssertFailed();
                        break;
                }
                break;
            }
            default:
                AssertFailed();
                break;
        }

        u32 |= (uint32_t)bRead << (i * 8);
    }

    *pu32Value = u32;
    return VINF_SUCCESS;
}


/**
 * Returns the MSI message control register as the guest sees it.
 *
 * @returns The register value, 0 if the function has no MSI capability.
 * @param   pFun        The function.
 * @param   pPciDev     The PCI device of the function.
 */
DECLINLINE(uint16_t) pciVfioGetMsiCtrl(PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev)
{
    return pFun->offMsiCtrl ? PDMPciDevGetWord(pPciDev, pFun->offMsiCtrl) : 0;
}


/**
 * Returns the MSI-X message control register as the guest sees it.
 *
 * @returns The register value, 0 if the function has no MSI-X capability.
 * @param   pFun        The function.
 * @param   pPciDev     The PCI device of the function.
 */
DECLINLINE(uint16_t) pciVfioGetMsixCtrl(PCVFIOPCIFUN pFun, PPDMPCIDEV pPciDev)
{
    return pFun->offMsixCtrl ? PDMPciDevGetWord(pPciDev, pFun->offMsixCtrl) : 0;
}


/**
 * Finds the emulated config space register a byte belongs to.
 *
 * @returns true if the byte belongs to an emulated register, false if not.
 * @param   pFun        The function.
 * @param   off         The config space offset of the byte.
 * @param   poffReg     Where to return the config space offset of the register.
 * @param   pcbReg      Where to return the size of the register, 2 or 4.
 */
static bool pciVfioCfgEmulatedRegLookup(PCVFIOPCIFUN pFun, uint32_t off, uint32_t *poffReg, uint32_t *pcbReg)
{
    uint32_t const cbU16 = sizeof(uint16_t);
    uint32_t const cbU32 = sizeof(uint32_t);

    uint32_t offReg = VBOX_PCI_COMMAND;
    uint32_t cbReg  = cbU16;
    if (off - offReg >= cbReg)
    {
        offReg = UINT32_MAX;
        if (pFun->offMsiCtrl)
        {
            uint32_t const offCap  = pFun->offMsiCtrl - VBOX_MSI_CAP_MESSAGE_CONTROL;
            uint32_t const offData = pciVfioMsiGetDataOff(pFun);
            if (off - pFun->offMsiCtrl < cbU16)
            {
                offReg = pFun->offMsiCtrl;
                cbReg  = cbU16;
            }
            else if (off - (offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_LO) < cbU32)
            {
                offReg = offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_LO;
                cbReg  = cbU32;
            }
            else if (   pFun->fMsi64Bit
                     && off - (offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_HI) < cbU32)
            {
                offReg = offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_HI;
                cbReg  = cbU32;
            }
            else if (off - offData < cbU16)
            {
                offReg = offData;
                cbReg  = cbU16;
            }
            else if (   pFun->fMsiMaskable
                     && off - (offData + 4) < cbU32)
            {
                offReg = offData + 4;
                cbReg  = cbU32;
            }
        }
        if (   offReg == UINT32_MAX
            && pFun->offMsixCtrl
            && off - pFun->offMsixCtrl < cbU16)
        {
            offReg = pFun->offMsixCtrl;
            cbReg  = cbU16;
        }
        if (offReg == UINT32_MAX)
            return false;
    }

    *poffReg = offReg;
    *pcbReg  = cbReg;
    return true;
}


/**
 * Handles the guest writing an emulated config space register.
 *
 * @param   pThis       The VFIO PCI device instance.
 * @param   pFun        The function.
 * @param   pPciDev     The PCI device of the function.
 * @param   offReg      The config space offset of the register as returned by pciVfioCfgEmulatedRegLookup().
 * @param   uValue      The new value of the whole register; what the guest did not write is the current content.
 */
static void pciVfioCfgEmulatedRegWrite(PVFIOPCI pThis, PVFIOPCIFUN pFun, PPDMPCIDEV pPciDev, uint32_t offReg, uint32_t uValue)
{
    PPDMDEVINS pDevIns = pThis->pDevIns;

    if (offReg == VBOX_PCI_COMMAND)
    {
        /*
         * The device gets to do DMA only with the guest memory mapped for it, which
         * is done before the VM starts running, see pciVfioInitComplete().
         */
        if (   (uValue & VBOX_PCI_COMMAND_MASTER)
            && !pThis->fGuestRamMapped)
        {
            LogRelMax(8, ("VFIO#%d.%u: Not enabling bus mastering, the guest memory is not mapped for the device\n",
                          pThis->iInstance, pFun->uPciFun));
            uValue &= ~(uint32_t)VBOX_PCI_COMMAND_MASTER;
        }

        /* The INTx disable bit decides whether INTx gets used when neither MSI nor MSI-X is enabled. */
        pciVfioIrqUpdate(pThis, pFun, (uint16_t)uValue, pciVfioGetMsiCtrl(pFun, pPciDev), pciVfioGetMsixCtrl(pFun, pPciDev));

        /*
         * The host kernel takes the mappings of the BARs away while the device doesn't
         * decode its I/O ports or memory.  The guest must not be left with a mapping
         * which got unusable, so the order matters: the PCI bus removes the guest
         * mappings of what gets disabled, then the device is written to, and then the
         * PCI bus does the rest, which includes creating the guest mappings of what
         * got enabled.
         */
        uint16_t const fDecode  = VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MEMORY;
        uint16_t const u16Old   = PDMPciDevGetCommand(pPciDev);
        uint16_t const u16First = (uint16_t)((u16Old & ~fDecode) | (u16Old & uValue & fDecode));
        VBOXSTRICTRC rcStrict;
        if (u16First != u16Old)
        {
            rcStrict = PDMDevHlpPCIConfigWrite(pDevIns, pPciDev, VBOX_PCI_COMMAND, sizeof(uint16_t), u16First);
            Assert(rcStrict == VINF_SUCCESS);
        }

        int rc = pciVfioCfgSpaceWriteU16(pFun, VBOX_PCI_COMMAND, (uint16_t)uValue);
        if (RT_FAILURE(rc))
            LogRel(("VFIO#%d.%u: Failed to update command register in device (%Rrc)\n", pThis->iInstance, pFun->uPciFun, rc));

        rcStrict = PDMDevHlpPCIConfigWrite(pDevIns, pPciDev, VBOX_PCI_COMMAND, sizeof(uint16_t), uValue);
        Assert(rcStrict == VINF_SUCCESS); RT_NOREF(rcStrict);
    }
    else if (offReg == pFun->offMsixCtrl)
    {
        /* Only the enable and the function mask bit are writable, the rest describes the device. */
        uint16_t const fMsixCtrlRw = VBOX_PCI_MSIX_FLAGS_ENABLE | VBOX_PCI_MSIX_FLAGS_FUNCMASK;
        uint16_t const u16MsixCtrl = (uint16_t)(  (PDMPciDevGetWord(pPciDev, offReg) & ~fMsixCtrlRw)
                                                | ((uint16_t)uValue & fMsixCtrlRw));
        pciVfioIrqUpdate(pThis, pFun, PDMPciDevGetCommand(pPciDev), pciVfioGetMsiCtrl(pFun, pPciDev), u16MsixCtrl);

        /* Now write to the device. */
        int rc = pciVfioCfgSpaceWriteU16(pFun, offReg, u16MsixCtrl);
        if (RT_FAILURE(rc))
            LogRel(("VFIO#%d.%u: Failed to update MSI-X control register in device (%Rrc)\n", pThis->iInstance, pFun->uPciFun, rc));
        PDMPciDevSetWord(pPciDev, offReg, u16MsixCtrl);

        /* Vectors held back by the function mask can go out now. */
        for (uint32_t i = 0; i < pFun->acIrqVectors[VFIO_PCI_MSIX_IRQ_INDEX]; i++)
            pciVfioMsixDeliverPending(pDevIns, pFun, pPciDev, i);
    }
    else if (offReg == pFun->offMsiCtrl)
    {
        /* Only the enable bit and the number of vectors to use are writable, the rest describes the device. */
        uint16_t const fMsiCtrlRw = VBOX_PCI_MSI_FLAGS_ENABLE | VBOX_PCI_MSI_FLAGS_QSIZE;
        uint16_t const u16MsiCtrl = (uint16_t)(  (PDMPciDevGetWord(pPciDev, offReg) & ~fMsiCtrlRw)
                                               | ((uint16_t)uValue & fMsiCtrlRw));
        pciVfioIrqUpdate(pThis, pFun, PDMPciDevGetCommand(pPciDev), u16MsiCtrl, pciVfioGetMsixCtrl(pFun, pPciDev));

        /* Now write to the device. */
        int rc = pciVfioCfgSpaceWriteU16(pFun, offReg, u16MsiCtrl);
        if (RT_FAILURE(rc))
            LogRel(("VFIO#%d.%u: Failed to update MSI control register in device (%Rrc)\n", pThis->iInstance, pFun->uPciFun, rc));
        PDMPciDevSetWord(pPciDev, offReg, u16MsiCtrl);
    }
    else
    {
        uint32_t const offCap  = pFun->offMsiCtrl - VBOX_MSI_CAP_MESSAGE_CONTROL;
        uint32_t const offData = pciVfioMsiGetDataOff(pFun);
        if (offReg == offData)
            PDMPciDevSetWord(pPciDev, offReg, (uint16_t)uValue);
        else if (offReg == offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_LO)
            PDMPciDevSetDWord(pPciDev, offReg, uValue & ~UINT32_C(3)); /* The bottom two bits are always zero. */
        else if (   pFun->fMsi64Bit
                 && offReg == offCap + VBOX_MSI_CAP_MESSAGE_ADDRESS_HI)
            PDMPciDevSetDWord(pPciDev, offReg, uValue);
        else
        {
            /* The mask bits: one for each vector of the device.  Vectors which got unmasked are sent what they have pending. */
            Assert(pFun->fMsiMaskable && offReg == offData + 4);
            uint32_t const cVectors = pFun->acIrqVectors[VFIO_PCI_MSI_IRQ_INDEX];
            PDMPciDevSetDWord(pPciDev, offReg, uValue & (uint32_t)(RT_BIT_64(cVectors) - 1));
            for (uint32_t i = 0; i < cVectors; i++)
                pciVfioMsiDeliverPending(pDevIns, pFun, pPciDev, i);
        }
    }
}


/**
 * @callback_method_impl{FNPCICONFIGWRITE}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioConfigWrite(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev,
                                                     uint32_t uAddress, unsigned cb, uint32_t u32Value)
{
    PVFIOPCI    pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PVFIOPCIFUN pFun  = &pThis->aPciFuns[pPciDev->idxSubDev];
    AssertReturn(cb <= sizeof(u32Value), VINF_SUCCESS);

    /* Nothing can be written to a function which is out of service. */
    if (pFun->fGone)
        return VINF_SUCCESS;

    /*
     * The bytes of an access can be subject to different handling.  Take the access
     * apart into runs of bytes which are handled the same way and, in the case of
     * emulated registers, belong to the same register.
     */
    uint32_t offAccess = 0;
    while (offAccess < cb)
    {
        uint32_t const offCfg = uAddress + offAccess;
        uint8_t        fWr    = pciVfioCfgSpaceGetInterceptWr(pFun, offCfg);
        uint32_t       offReg = 0;
        uint32_t       cbReg  = 0;
        if (   fWr == VFIO_PCI_CFG_SPACE_ACCESS_EMULATE
            && !pciVfioCfgEmulatedRegLookup(pFun, offCfg, &offReg, &cbReg))
        {
            AssertFailed();
            fWr = VFIO_PCI_CFG_SPACE_ACCESS_INVALID;
        }

        uint32_t cbRun = 1;
        while (   offAccess + cbRun < cb
               && pciVfioCfgSpaceGetInterceptWr(pFun, offCfg + cbRun) == fWr
               && (   fWr != VFIO_PCI_CFG_SPACE_ACCESS_EMULATE
                   || offCfg + cbRun < offReg + cbReg))
            cbRun++;
        uint32_t const fRunMask = cbRun < sizeof(uint32_t) ? RT_BIT_32(cbRun * 8) - 1 : UINT32_MAX;
        uint32_t const uRun     = (u32Value >> (offAccess * 8)) & fRunMask;

        switch (fWr)
        {
            case VFIO_PCI_CFG_SPACE_ACCESS_INVALID:
                Log(("VFIO#%d.%u: Invalid PCI config write at offset %#x LB %u, ignoring\n",
                     pThis->iInstance, pFun->uPciFun, offCfg, cbRun));
                break;

            case VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH:
            case VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT:
            {
                /* Hand it on in naturally aligned pieces of one, two or four bytes. */
                uint32_t offRun = 0;
                while (offRun < cbRun)
                {
                    uint32_t const offPiece = offCfg + offRun;
                    uint32_t const cbLeft   = cbRun - offRun;
                    uint32_t const cbPiece  = (offPiece & 1) || cbLeft == 1       ? 1
                                            : (offPiece & 2) || cbLeft < 4        ? 2
                                            :                                       4;
                    uint32_t const uPiece   = (uRun >> (offRun * 8)) & (cbPiece < sizeof(uint32_t) ? RT_BIT_32(cbPiece * 8) - 1 : UINT32_MAX);
                    if (fWr == VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH)
                    {
                        int rc = pciVfioConfigPassthroughWrite(pFun, offPiece, cbPiece, uPiece);
                        if (RT_FAILURE(rc))
                            LogRelMax(32, ("VFIO#%d.%u: Failed to passthrough PCI config write at offset %#x LB %u (%Rrc)\n",
                                           pThis->iInstance, pFun->uPciFun, offPiece, cbPiece, rc));
                    }
                    else
                    {
                        VBOXSTRICTRC rcStrict = PDMDevHlpPCIConfigWrite(pDevIns, pPciDev, offPiece, cbPiece, uPiece);
                        Assert(rcStrict == VINF_SUCCESS); RT_NOREF(rcStrict);
                    }
                    offRun += cbPiece;
                }
                break;
            }

            case VFIO_PCI_CFG_SPACE_ACCESS_EMULATE:
            {
                /* Merge what is written into the current content of the register. */
                uint32_t const cShift = (offCfg - offReg) * 8;
                uint32_t       uReg   = cbReg == sizeof(uint16_t) ? PDMPciDevGetWord(pPciDev, offReg) : PDMPciDevGetDWord(pPciDev, offReg);
                uReg = (uReg & ~(fRunMask << cShift)) | (uRun << cShift);
                pciVfioCfgEmulatedRegWrite(pThis, pFun, pPciDev, offReg, uReg);
                break;
            }

            default:
                AssertFailed();
                break;
        }

        offAccess += cbRun;
    }

    return VINF_SUCCESS;
}


/**
 * Quirk handler for Intel Battlemage GPUs where the Windows driver reads from PCI config space 0x50
 * when no hypervisor is detected and the physical device is passed through (does not apply to virtual functions in the SR-IOV case).
 * The driver uses this to determine how much bytes to map for a PCI bar. However the 0x50 offset is not passed through because
 * it is not part of the standard config space and not part of a capability and therefore returns 0.
 * This results in the driver trying to map 0 bytes when calling DxgkcbMapMemory() which fails with NTSTATUS_INVALID_PARAMETER.
 * The quirk just statically passes through the 0x50 offset readonly.
 */
static DECLCALLBACK(int) pciVfioQuirkIntelBattlemageApply(PVFIOPCI pThis, PVFIOPCIFUN pFun, PCVFIOPCIQUIRK pQuirk)
{
    RT_NOREF(pThis, pQuirk);

    /* Only bytes which are not handled otherwise, like being part of a capability. */
    for (uint32_t off = 0x50; off < 0x50 + 16; off++)
        if (   pciVfioCfgSpaceGetInterceptRd(pFun, off) == VFIO_PCI_CFG_SPACE_ACCESS_INVALID
            && pciVfioCfgSpaceGetInterceptWr(pFun, off) == VFIO_PCI_CFG_SPACE_ACCESS_INVALID)
            pciVfioCfgSpaceSetInterceptRoU8(pFun, off, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH);

    return VINF_SUCCESS;
}


/*
 * Quirk handling.
 */
static const VFIOPCIQUIRK g_aQuirks[] =
{
    /** Intel */
    { 0x8086, 0xe223, 0, pciVfioQuirkIntelBattlemageApply, "Intel Arc Pro B70 config space passthrough" } /**< Intel Corporation Battlemage G31 [Arc Pro B70] */
};


/**
 * Applies quirks for the given function.
 *
 * @returns VBox status code.
 * @param   pThis               Pointer to the PCI VFIO device instance.
 * @param   pFun                The function to apply a possible quirk for.
 */
static int pciVfioQuirkApply(PVFIOPCI pThis, PVFIOPCIFUN pFun)
{
    uint16_t idVendor = 0;
    uint16_t idDevice = 0;
    int rc = pciVfioCfgSpaceReadU16(pFun, VBOX_PCI_VENDOR_ID, &idVendor);
    if (RT_SUCCESS(rc))
        rc = pciVfioCfgSpaceReadU16(pFun, VBOX_PCI_DEVICE_ID, &idDevice);
    if (RT_SUCCESS(rc))
    {
        for (uint32_t i = 0; i < RT_ELEMENTS(g_aQuirks); i++)
        {
            PCVFIOPCIQUIRK pQuirk = &g_aQuirks[i];

            if (   pQuirk->idVendor == idVendor
                && pQuirk->idDevice == idDevice)
            {
                LogRel(("VFIO#%d.%u: Applying quirk \"%s\" for device %#RX16:%#RX16\n", pThis->iInstance, pFun->uPciFun,
                        pQuirk->pszDesc, idVendor, idDevice));
                return pQuirk->pfnApply(pThis, pFun, pQuirk);
            }
        }
    }

    return rc;
}


/**
 * Checks that a string is the address of a PCI function the way the host
 * kernel writes it: domain:bus:device.function, like 0000:0a:00.1.
 *
 * The address becomes a part of sysfs paths and of device names handed to the
 * kernel, so nothing but this format is let through.
 *
 * @returns true if the string is a PCI function address, false if not.
 * @param   pszAddress  The string to check.
 */
static bool pciVfioIsHostAddressValid(const char *pszAddress)
{
    static const char s_szFormat[] = "xxxx:xx:xx.f"; /* x: hex digit, f: function number */
    size_t off = 0;
    for (; off < sizeof(s_szFormat) - 1; off++)
    {
        char const ch = pszAddress[off];
        if (s_szFormat[off] == 'x')
        {
            if (!RT_C_IS_XDIGIT(ch))
                return false;
        }
        else if (s_szFormat[off] == 'f')
        {
            if (ch < '0' || ch > '7')
                return false;
        }
        else if (ch != s_szFormat[off])
            return false;
    }
    return pszAddress[off] == '\0';
}


static int pciVfioConfigureAccess(PPDMDEVINS pDevIns, PVFIOPCI pThis, PCPDMDEVHLPR3 pHlp, PCFGMNODE pCfg)
{
    /* Query configuration, configuration using the IOMMUFD takes precedence over the legacy VFIO group style. */
    char szPath[RTPATH_MAX];
    int rc = pHlp->pfnCFGMQueryStringDef(pCfg, "IommuPath", &szPath[0], sizeof(szPath), "/dev/iommu");
    if (RT_FAILURE(rc))
        return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                   N_("Configuration error: Querying \"IommuPath\" failed"));

    pThis->IommuFd.iFdIommu = open(szPath, O_RDWR | O_CLOEXEC);
    if (pThis->IommuFd.iFdIommu == -1)
    {
        pThis->fVfioLegacy = true;
        pThis->VfioGroup.iFdVfioGroup     = -1;
        pThis->VfioGroup.iFdVfioContainer = -1;

        rc = pHlp->pfnCFGMQueryStringDef(pCfg, "VfioPath", &szPath[0], sizeof(szPath), "/dev/vfio/vfio");
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Configuration error: Querying \"VfioPath\" failed, no access method configured"));

        pThis->VfioGroup.iFdVfioContainer = open(szPath, O_RDWR | O_CLOEXEC);
        if (pThis->VfioGroup.iFdVfioContainer == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Opening VFIO container path \"%s\" failed with %d"), szPath, errno);

        int rcLnx = ioctl(pThis->VfioGroup.iFdVfioContainer, VFIO_GET_API_VERSION);
        if (rcLnx == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Querying VFIO API version failed"));
        if (rcLnx != VFIO_API_VERSION)
            return PDMDevHlpVMSetError(pDevIns, VERR_VERSION_MISMATCH, RT_SRC_POS,
                                       N_("Returned VFIO API version %d doesn't match expected version %d"),
                                       rcLnx, VFIO_API_VERSION);

        rcLnx = ioctl(pThis->VfioGroup.iFdVfioContainer, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU);
        if (rcLnx <= 0)
            return PDMDevHlpVMSetError(pDevIns, VERR_NOT_SUPPORTED, RT_SRC_POS,
                                       N_("VFIO_TYPE1_IOMMU extension not supported"));

        /* Open the group. */
        rc = pHlp->pfnCFGMQueryString(pCfg, "VfioGroupPath", &szPath[0], sizeof(szPath));
        if (rc == VERR_CFGM_VALUE_NOT_FOUND)
        {
            /* Try to determine the IOMMU group from the first device being used. */
            PCFGMNODE pCfgFun = pHlp->pfnCFGMGetChildF(pCfg, "Fun%u", 0);
            if (!pCfgFun)
                return PDMDevHlpVMSetError(pDevIns, VERR_NOT_FOUND, RT_SRC_POS,
                                           N_("Configuration error: The device configuration misses function 0"));

            char szHostAddress[32]; /* Format is xxxx:xx:xx.x */
            rc = pHlp->pfnCFGMQueryString(pCfgFun, "HostAddress", &szHostAddress[0], sizeof(szHostAddress));
            if (RT_FAILURE(rc))
                return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                           N_("Configuration error: Querying \"HostAddress\" failed, no access method configured"));
            if (!pciVfioIsHostAddressValid(szHostAddress))
                return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_PARAMETER, RT_SRC_POS,
                                           N_("Configuration error: \"HostAddress\" is not a PCI address of the form 0000:00:00.0"));

            char szIommuGroup[8];
            szIommuGroup[0] = '\0';
            size_t cchIommuGroup = 0;
            rc = RTLinuxSysFsGetLinkDest(&szIommuGroup[0], sizeof(szIommuGroup), &cchIommuGroup, "/sys/bus/pci/devices/%s/iommu_group", szHostAddress);
            if (RT_FAILURE(rc))
                return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                           N_("Configuration error: Resolving the IOMMU group of the PCI device failed"));

            /* The group is a number, anything else isn't a group this can work with. */
            for (size_t off = 0; off < cchIommuGroup || off == 0; off++)
                if (!RT_C_IS_DIGIT(szIommuGroup[off]))
                    return PDMDevHlpVMSetError(pDevIns, VERR_NOT_SUPPORTED, RT_SRC_POS,
                                               N_("The IOMMU group of the PCI device is not usable for PCI passthrough"));

            rc = RTPathJoin(szPath, sizeof(szPath), "/dev/vfio", szIommuGroup);
            AssertRCReturn(rc, rc);
        }
        else  if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Configuration error: Querying \"VfioGroupPath\" failed"));

        pThis->VfioGroup.iFdVfioGroup = open(szPath, O_RDWR | O_CLOEXEC);
        if (pThis->VfioGroup.iFdVfioGroup == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Opening VFIO group path \"%s\" failed with %d"), szPath, errno);

        /* Check whether the group is viable. */
        struct vfio_group_status GroupStatus;
        GroupStatus.argsz = sizeof(vfio_group_status);
        GroupStatus.flags = 0;
        rcLnx = ioctl(pThis->VfioGroup.iFdVfioGroup, VFIO_GROUP_GET_STATUS, &GroupStatus);
        if (rcLnx == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Querying VFIO status failed"));

        if (!(GroupStatus.flags & VFIO_GROUP_FLAGS_VIABLE))
            return PDMDevHlpVMSetError(pDevIns, VERR_NOT_SUPPORTED, RT_SRC_POS,
                                       N_("The VFIO group is not usable for PCI passthrough (all devices in the group bound to VFIO?)"));

        rcLnx = ioctl(pThis->VfioGroup.iFdVfioGroup, VFIO_GROUP_SET_CONTAINER, &pThis->VfioGroup.iFdVfioContainer);
        if (rcLnx == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Attaching VFIO group to container failed"));

        rcLnx = ioctl(pThis->VfioGroup.iFdVfioContainer, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
        if (rcLnx == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Enabling IOMMU for container failed"));
    }
    else
    {
        /* Allocate a new I/O address space on the IOMMU. */
        struct iommu_ioas_alloc IoasAlloc;
        IoasAlloc.size  = sizeof(IoasAlloc);
        IoasAlloc.flags = 0;
        int rcLnx = ioctl(pThis->IommuFd.iFdIommu, IOMMU_IOAS_ALLOC, &IoasAlloc);
        if (rcLnx == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Allocating I/O address space for VFIO device failed with %d"), errno);

        pThis->IommuFd.idIommuHwpt = IoasAlloc.out_ioas_id;
        pThis->fIoasAllocated      = true;
    }

    return VINF_SUCCESS;
}


static int pciVfioFunConfigureAccess(PPDMDEVINS pDevIns, PVFIOPCI pThis, PVFIOPCIFUN pFun, PCPDMDEVHLPR3 pHlp, PCFGMNODE pCfg)
{
    bool fHostAddress = false;
    char szPath[RTPATH_MAX];
    int rc = pHlp->pfnCFGMQueryString(pCfg, "VfioPath", &szPath[0], sizeof(szPath));
    if (rc == VERR_CFGM_VALUE_NOT_FOUND)
    {
        rc = pHlp->pfnCFGMQueryString(pCfg, "HostAddress", &szPath[0], sizeof(szPath));
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Configuration error: Querying \"HostAddress\" failed, no access method configured"));
        if (!pciVfioIsHostAddressValid(szPath))
            return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_PARAMETER, RT_SRC_POS,
                                       N_("Configuration error: \"HostAddress\" is not a PCI address of the form 0000:00:00.0"));
        fHostAddress = true;
    }
    else if (RT_FAILURE(rc))
        return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                   N_("Configuration error: Querying \"VfioPath\" failed"));

    if (pThis->fVfioLegacy)
    {
        pFun->iFdVfio = ioctl(pThis->VfioGroup.iFdVfioGroup, VFIO_GROUP_GET_DEVICE_FD, &szPath[0]);
        if (pFun->iFdVfio == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Failed to get device for VFIO device \"%s\" -> %d"), szPath, errno);

        /* Not for processes started by this one. */
        int const fFdFlags = fcntl(pFun->iFdVfio, F_GETFD);
        if (   fFdFlags == -1
            || fcntl(pFun->iFdVfio, F_SETFD, fFdFlags | FD_CLOEXEC) == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Failed to set up the file descriptor of VFIO device \"%s\" -> %d"), szPath, errno);
    }
    else
    {
        if (fHostAddress)
        {
            /* Determine the VFIO device from sysfs. */
            char szSysfsPath[128];
            rc = RTPathJoin(szSysfsPath, sizeof(szSysfsPath), "/sys/bus/pci/devices", szPath);
            AssertRCReturn(rc, rc);
            rc = RTPathAppend(szSysfsPath, sizeof(szSysfsPath), "vfio-dev");
            AssertRCReturn(rc, rc);

            RTDIR hDir = NIL_RTDIR;
            rc = RTDirOpen(&hDir, szSysfsPath);
            if (RT_FAILURE(rc))
                return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                           N_("Configuration error: The PCI device %s is not bound to a VFIO driver"), szPath);
            for (;;)
            {
                RTDIRENTRY Entry; /* The default name size is more than enough for the format vfioN */
                rc = RTDirRead(hDir, &Entry, NULL /*pcbDirEntry*/);
                if (RT_FAILURE(rc))
                    break;

                if (RTDirEntryIsStdDotLink(&Entry))
                    continue;

                if (!strncmp(Entry.szName, RT_STR_TUPLE("vfio")))
                {
                    rc = RTPathJoin(szPath, sizeof(szPath), "/dev/vfio/devices", Entry.szName);
                    break;
                }
            }

            int rc2 = RTDirClose(hDir);
            AssertRC(rc2); RT_NOREF(rc2);

            if (RT_FAILURE(rc))
                return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                           N_("Configuration error: Resolving the VFIO device failed"));
        }

        pFun->iFdVfio = open(szPath, O_RDWR | O_CLOEXEC);
        if (pFun->iFdVfio == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Opening VFIO device \"%s\" failed with %d"), szPath, errno);

        /* Bind the IOMMU to the device. */
        struct vfio_device_bind_iommufd VfioBind; RT_ZERO(VfioBind);
        VfioBind.argsz   = sizeof(VfioBind);
        VfioBind.flags   = 0;
        VfioBind.iommufd = pThis->IommuFd.iFdIommu;
        int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_BIND_IOMMUFD, &VfioBind);
        if (rcLnx == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Binding IOMMU device to opened VFIO device failed with %d"), errno);

        /* And bind it to the device. */
        struct vfio_device_attach_iommufd_pt VfioAttachIommu;
        VfioAttachIommu.argsz = sizeof(VfioAttachIommu);
        VfioAttachIommu.flags = 0;
        VfioAttachIommu.pt_id = pThis->IommuFd.idIommuHwpt;
        rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &VfioAttachIommu);
        if (rcLnx == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Attaching IO address space to opened VFIO device failed with %d"), errno);
        pFun->fIoasAttached = true;
    }

    return VINF_SUCCESS;
}


/**
 * Does on an EMT what is left to do for a function which went out of service:
 * the guest loses its mappings of the BARs and finds nothing where the function
 * was from then on.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   iPciFun     The function.
 */
static DECLCALLBACK(int) pciVfioFunGoneOnEmt(PPDMDEVINS pDevIns, uintptr_t iPciFun)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    AssertReturn(iPciFun < RT_ELEMENTS(pThis->aPciFuns), VERR_INVALID_PARAMETER);
    PVFIOPCIFUN pFun    = &pThis->aPciFuns[iPciFun];
    PPDMPCIDEV  pPciDev = pDevIns->apPciDevs[iPciFun];

    /* The PCI bus removes the mappings when the decoding gets disabled.  The guest doesn't get to enable it again. */
    uint16_t const fCmdActive = VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER;
    uint16_t const u16Cmd     = PDMPciDevGetCommand(pPciDev);
    if (u16Cmd & fCmdActive)
    {
        VBOXSTRICTRC rcStrict = PDMDevHlpPCIConfigWrite(pDevIns, pPciDev, VBOX_PCI_COMMAND, sizeof(uint16_t), u16Cmd & ~fCmdActive);
        Assert(rcStrict == VINF_SUCCESS); RT_NOREF(rcStrict);
    }

    /* From here on the guest finds nothing where the function was. */
    ASMAtomicWriteBool(&pFun->fGoneForGuest, true);

    /* Whatever the guest managed to write into the memory which took the place of the BARs is of no use to anybody. */
    for (uint32_t iBar = 0; iBar < RT_ELEMENTS(pFun->aBars); iBar++)
        if (pFun->aBars[iBar].pvMmio)
            madvise((void *)pFun->aBars[iBar].pvMmio, pFun->aBars[iBar].cbRegion, MADV_DONTNEED);

    return VINF_SUCCESS;
}


/**
 * Takes a function out of service and lets go of the device, because the host
 * wants it back or because it reported an error it can't recover from.
 *
 * This can be called on any thread and in any VM state.  When it returns
 * nothing of the device is reachable any more: accesses of the guest are
 * answered like those to a device which isn't there, and the BARs mapped into
 * the guest are backed by ordinary memory, at the place where the device
 * memory used to be mapped in this process so everybody referring to it stays
 * valid.  The file descriptor of the device is replaced by one which refuses
 * everything, which leaves its number valid for whoever is using it right now.
 *
 * @param   pThis       The VFIO PCI device instance.
 * @param   pFun        The function.
 * @param   pszWhy      The reason, for the log.
 */
static void pciVfioFunSetGone(PVFIOPCI pThis, PVFIOPCIFUN pFun, const char *pszWhy)
{
    if (ASMAtomicXchgBool(&pFun->fGone, true))
        return;
    LogRel(("VFIO#%d.%u: The function is taken out of service: %s\n", pThis->iInstance, pFun->uPciFun, pszWhy));

    /* The device memory mapped into this process and through that into the guest. */
    for (uint32_t iBar = 0; iBar < RT_ELEMENTS(pFun->aBars); iBar++)
    {
        PCVFIOPCIBAR pBar = &pFun->aBars[iBar];
        if (pBar->pvMmio)
        {
            void *pv = mmap((void *)pBar->pvMmio, pBar->cbRegion, PROT_READ | PROT_WRITE,
                            MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1 /*fd*/, 0 /*offset*/);
            if (pv == MAP_FAILED)
                LogRel(("VFIO#%d.%u: Failed to replace the mapping of BAR%u (%d), the device stays in use by it\n",
                        pThis->iInstance, pFun->uPciFun, iBar, errno));
        }
    }

    /* The device itself.  This also takes it out of the I/O address space and ends its interrupts. */
    if (dup3(pFun->iFdInert, pFun->iFdVfio, O_CLOEXEC) == -1)
        LogRel(("VFIO#%d.%u: Failed to let go of the device (%d)\n", pThis->iInstance, pFun->uPciFun, errno));
    pFun->fIoasAttached = false;

    /* The rest is for an EMT to do. */
    int rc = PDMDevHlpVMReqCallNoWait(pThis->pDevIns, VMCPUID_ANY, (PFNRT)pciVfioFunGoneOnEmt, 2,
                                      pThis->pDevIns, (uintptr_t)pFun->uPciFun);
    if (RT_FAILURE(rc))
        LogRel(("VFIO#%d.%u: Failed to schedule the removal of the guest mappings (%Rrc)\n",
                pThis->iInstance, pFun->uPciFun, rc));
}


/**
 * Checks whether nothing of the device is in service.
 *
 * @returns true if all functions are out of service, false if one is in service at least.
 * @param   pThis       The VFIO PCI device instance.
 */
static bool pciVfioIsOutOfService(PVFIOPCI pThis)
{
    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
        if (   pThis->aPciFuns[iPciFun].iFdVfio != -1
            && !pThis->aPciFuns[iPciFun].fGone)
            return false;
    return true;
}


/**
 * Checks whether a function still answers, which it doesn't after it was unplugged.
 *
 * @returns true if the function answers, false if not.
 * @param   pFun        The function.
 */
static bool pciVfioFunAnswers(PCVFIOPCIFUN pFun)
{
    uint16_t idVendor = UINT16_MAX;
    int rc = pciVfioCfgSpaceReadU16(pFun, VBOX_PCI_VENDOR_ID, &idVendor);
    return RT_SUCCESS(rc) && idVendor != UINT16_MAX;
}


/**
 * Asks the guest to let go of the device in the hot-plug capable slot by pressing
 * the attention button of the slot, unless that was done a moment ago.
 *
 * @param   pThis       The VFIO PCI device instance, in a hot-plug capable slot.
 */
static void pciVfioSlotAskGuest(PVFIOPCI pThis)
{
    uint64_t const msNow  = RT_MAX(RTTimeMilliTS(), 1);
    uint64_t const msLast = ASMAtomicReadU64(&pThis->msAttnPressed);
    if (   (   !msLast
            || msNow - msLast >= VFIO_PCI_ATTN_BUTTON_HOLDOFF_MS)
        && ASMAtomicCmpXchgU64(&pThis->msAttnPressed, msNow, msLast))
        devpciR3HotPlugSlotPressAttentionButton(pThis->pDevInsPort);
}


/**
 * Takes all functions of the device out of service and the device out of the
 * hot-plug capable slot it is in, see pciVfioFunSetGone().  The guest is told that
 * the slot is empty.
 *
 * @param   pThis       The VFIO PCI device instance, in a hot-plug capable slot.
 * @param   pszWhy      The reason, for the log.
 */
static void pciVfioSlotEmpty(PVFIOPCI pThis, const char *pszWhy)
{
    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
        if (pThis->aPciFuns[iPciFun].iFdVfio != -1)
            pciVfioFunSetGone(pThis, &pThis->aPciFuns[iPciFun], pszWhy);
    devpciR3HotPlugSlotSetPresence(pThis->pDevInsPort, false /*fPresent*/);
}


/**
 * The thread watching out for the host wanting a function back or reporting an
 * error on it.
 *
 * This is not a PDM thread as it has to do its job whatever state the VM is in:
 * the host waits for the device to be let go of.
 *
 * @returns VBox status code.
 * @param   hThreadSelf The thread handle.
 * @param   pvUser      The VFIO PCI device instance.
 */
static DECLCALLBACK(int) pciVfioWatcherThread(RTTHREAD hThreadSelf, void *pvUser)
{
    PVFIOPCI pThis = (PVFIOPCI)pvUser;
    RT_NOREF(hThreadSelf);

    /* When the time the guest got for letting go of the device is up, 0 if it wasn't asked. */
    uint64_t msLetGoDeadline = 0;

    while (!ASMAtomicReadBool(&pThis->fWatcherStop))
    {
        /* The wakeup eventfd and what there is to watch of the functions which are still in service. */
        struct pollfd aFds[1 + RT_ELEMENTS(pThis->aPciFuns) * 2];
        uint8_t       aiPciFuns[RT_ELEMENTS(aFds)];
        bool          afRequest[RT_ELEMENTS(aFds)];
        nfds_t        cFds = 0;
        aFds[cFds].fd      = pThis->iFdWatcherWakeup;
        aFds[cFds].events  = POLLIN;
        aFds[cFds].revents = 0;
        aiPciFuns[cFds]    = 0;
        afRequest[cFds]    = false;
        cFds++;
        for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
        {
            PCVFIOPCIFUN pFun = &pThis->aPciFuns[iPciFun];
            if (pFun->fGone)
                continue;
            if (pFun->iFdEvtReq != -1)
            {
                aFds[cFds].fd      = pFun->iFdEvtReq;
                aFds[cFds].events  = POLLIN;
                aFds[cFds].revents = 0;
                aiPciFuns[cFds]    = iPciFun;
                afRequest[cFds]    = true;
                cFds++;
            }
            if (pFun->iFdEvtErr != -1)
            {
                aFds[cFds].fd      = pFun->iFdEvtErr;
                aFds[cFds].events  = POLLIN;
                aFds[cFds].revents = 0;
                aiPciFuns[cFds]    = iPciFun;
                afRequest[cFds]    = false;
                cFds++;
            }
        }

        /* While the guest is given time, the link to the slot is looked at now and then. */
        int rcLnx = poll(&aFds[0], cFds, msLetGoDeadline ? 50 : -1 /*timeout*/);
        if (rcLnx == -1)
        {
            if (errno == EINTR)
                continue;
            LogRel(("VFIO#%d: Watching for the host wanting the device back failed with %d\n", pThis->iInstance, errno));
            break;
        }

        for (nfds_t i = 1; i < cFds; i++)
            if (aFds[i].revents & POLLIN)
            {
                uint64_t      u64Ign = 0;
                ssize_t const cbRead = read(aFds[i].fd, &u64Ign, sizeof(u64Ign));
                RT_NOREF(cbRead);
                PVFIOPCIFUN const pFun = &pThis->aPciFuns[aiPciFuns[i]];
                if (!pThis->pDevInsPort)
                    /* Nobody to ask and nobody to tell. */
                    pciVfioFunSetGone(pThis, pFun,
                                      afRequest[i] ? "the host wants the device back" : "the host reported an error of the device");
                else if (!afRequest[i])
                    pciVfioSlotEmpty(pThis, "the host reported an error of the device");
                else if (   !pciVfioFunAnswers(pFun)
                         || !devpciR3HotPlugSlotIsLinkUp(pThis->pDevInsPort))
                    /* Unplugged, or nothing the guest would have to let go of. */
                    pciVfioSlotEmpty(pThis, "the host wants the device back");
                else
                {
                    /* The guest gets asked, and time to answer.  The host asks again and again meanwhile. */
                    pciVfioSlotAskGuest(pThis);
                    if (!msLetGoDeadline)
                        msLetGoDeadline = RT_MAX(RTTimeMilliTS() + pThis->cMsGuestRelease, 1);
                }
            }

        /* The guest has let go of the device when the link to the slot is down. */
        if (   msLetGoDeadline
            && (   !devpciR3HotPlugSlotIsLinkUp(pThis->pDevInsPort)
                || RTTimeMilliTS() >= msLetGoDeadline))
        {
            pciVfioSlotEmpty(pThis, "the host wants the device back");
            msLetGoDeadline = 0;
        }
    }

    return VINF_SUCCESS;
}


/**
 * Sets up an eventfd for one of the notifications of the host about a function:
 * the request to give the device back or an error of the device.
 *
 * These are optional, the function is used without if the host doesn't offer one.
 *
 * @returns VBox status code.
 * @param   pThis       The VFIO PCI device instance.
 * @param   pFun        The function.
 * @param   uIrqIndex   VFIO_PCI_REQ_IRQ_INDEX or VFIO_PCI_ERR_IRQ_INDEX.
 * @param   piFdEvt     Where to return the eventfd, -1 if the host doesn't offer the notification.
 */
static int pciVfioNotifierSetup(PVFIOPCI pThis, PCVFIOPCIFUN pFun, uint32_t uIrqIndex, int *piFdEvt)
{
    *piFdEvt = -1;

    struct vfio_irq_info IrqInfo;
    RT_ZERO(IrqInfo);
    IrqInfo.argsz = sizeof(IrqInfo);
    IrqInfo.index = uIrqIndex;
    int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_GET_IRQ_INFO, &IrqInfo);
    if (   rcLnx == -1
        || IrqInfo.count < 1
        || !(IrqInfo.flags & VFIO_IRQ_INFO_EVENTFD))
    {
        LogRel(("VFIO#%d.%u: The host doesn't offer notification %u for the function\n",
                pThis->iInstance, pFun->uPciFun, uIrqIndex));
        return VINF_SUCCESS;
    }

    int iFdEvt = -1;
    int rc = pciVfioLnxEventfd2(0 /*uValInit*/, LNX_EFD_CLOEXEC | LNX_EFD_NONBLOCK, &iFdEvt);
    if (RT_FAILURE(rc))
        return PDMDevHlpVMSetError(pThis->pDevIns, rc, RT_SRC_POS,
                                   N_("Failed to create the event file descriptor for notification %u"), uIrqIndex);

    struct
    {
        struct vfio_irq_set IrqSet;
        int32_t             iFdEvt;
    } Req;
    AssertCompile(sizeof(Req) == sizeof(struct vfio_irq_set) + sizeof(int32_t));
    Req.IrqSet.argsz = sizeof(Req);
    Req.IrqSet.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    Req.IrqSet.index = uIrqIndex;
    Req.IrqSet.start = 0;
    Req.IrqSet.count = 1;
    Req.iFdEvt       = iFdEvt;
    rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_SET_IRQS, &Req);
    if (rcLnx == -1)
    {
        rc = RTErrConvertFromErrno(errno);
        close(iFdEvt);
        return PDMDevHlpVMSetError(pThis->pDevIns, rc, RT_SRC_POS,
                                   N_("Failed to set up notification %u of the VFIO device (%Rrc)"), uIrqIndex, rc);
    }

    *piFdEvt = iFdEvt;
    return VINF_SUCCESS;
}


/**
 * Resets all functions of the device by resetting the bus they are on.
 *
 * The host kernel only does this if everything affected by it is ours.
 *
 * @returns VBox status code.
 * @param   pThis       The VFIO PCI device instance.
 */
static int pciVfioBusReset(PVFIOPCI pThis)
{
    /* Any function will do for asking. */
    PCVFIOPCIFUN pFun = NULL;
    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns) && !pFun; iPciFun++)
        if (   pThis->aPciFuns[iPciFun].iFdVfio != -1
            && !pThis->aPciFuns[iPciFun].fGone)
            pFun = &pThis->aPciFuns[iPciFun];
    if (!pFun) /* All of them went out of service meanwhile. */
        return VERR_INVALID_STATE;

    struct
    {
        struct vfio_pci_hot_reset   Hdr;
        int32_t                     iFdGroup;
    } Req;
    AssertCompile(sizeof(Req) == sizeof(struct vfio_pci_hot_reset) + sizeof(int32_t));
    RT_ZERO(Req);
    if (pThis->fVfioLegacy)
    {
        /* The group is the proof of owning what is affected. */
        Req.Hdr.argsz = sizeof(Req);
        Req.Hdr.count = 1;
        Req.iFdGroup  = pThis->VfioGroup.iFdVfioGroup;
    }
    else
    {
        /* The kernel goes by the IOMMUFD context the functions are bound to. */
        Req.Hdr.argsz = sizeof(Req.Hdr);
        Req.Hdr.count = 0;
    }

    int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_PCI_HOT_RESET, &Req);
    if (rcLnx == -1)
        return RTErrConvertFromErrno(errno);
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMDEVREG,pfnReset}
 */
static DECLCALLBACK(void) pciVfioReset(PPDMDEVINS pDevIns)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);

    /*
     * Make the functions keep quiet first: no bus mastering, no decoding and no
     * interrupts.  This is what they are left with should the reset not work.
     */
    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
    {
        PVFIOPCIFUN pFun = &pThis->aPciFuns[iPciFun];
        if (   pFun->iFdVfio == -1
            || pFun->fGone)
            continue;
        PPDMPCIDEV pPciDev = pDevIns->apPciDevs[iPciFun];

        /*
         * The device comes out of the reset with MSI and MSI-X disabled and all MSI-X
         * vectors masked, this is the state the guest gets to see.
         */
        if (pFun->offMsixCtrl)
        {
            PDMPciDevSetWord(pPciDev, pFun->offMsixCtrl,
                               PDMPciDevGetWord(pPciDev, pFun->offMsixCtrl)
                             & ~(VBOX_PCI_MSIX_FLAGS_ENABLE | VBOX_PCI_MSIX_FLAGS_FUNCMASK));
            for (uint32_t i = 0; i < pFun->acIrqVectors[VFIO_PCI_MSIX_IRQ_INDEX]; i++)
                pFun->paMsixTbl[i].u32VecCtrl = RT_BIT_32(0);
            RT_BZERO((void *)pFun->pbmMsixPba, pFun->cbMsixPba);
        }
        if (pFun->offMsiCtrl)
        {
            PDMPciDevSetWord(pPciDev, pFun->offMsiCtrl,
                               PDMPciDevGetWord(pPciDev, pFun->offMsiCtrl)
                             & ~(VBOX_PCI_MSI_FLAGS_ENABLE | VBOX_PCI_MSI_FLAGS_QSIZE));
            if (pFun->fMsiMaskable)
            {
                PDMPciDevSetDWord(pPciDev, pciVfioMsiGetDataOff(pFun) + 4, 0); /* Mask */
                PDMPciDevSetDWord(pPciDev, pciVfioMsiGetDataOff(pFun) + 8, 0); /* Pending */
            }
        }

        /* The command register: the guest loses its mappings of the BARs, then the device stops decoding. */
        pciVfioCfgEmulatedRegWrite(pThis, pFun, pPciDev, VBOX_PCI_COMMAND, 0);

        /* No interrupts while the device is being reset. */
        pciVfioIrqReconfigure(pThis, pFun, UINT8_MAX /*uVfioIrq*/, 0 /*cVectors*/);
    }

    /*
     * Reset the functions.  Where a function can't be reset on its own, which is
     * common for devices with several functions, the bus they are on gets reset.
     */
    bool fBusReset = false;
    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
    {
        PVFIOPCIFUN pFun = &pThis->aPciFuns[iPciFun];
        if (   pFun->iFdVfio != -1
            && !pFun->fGone
            && ioctl(pFun->iFdVfio, VFIO_DEVICE_RESET, NULL) == -1)
        {
            LogRelMax(16, ("VFIO#%d.%u: The function can't be reset on its own (%d)\n", pThis->iInstance, pFun->uPciFun, errno));
            fBusReset = true;
        }
    }
    if (fBusReset)
    {
        int rc = pciVfioBusReset(pThis);
        if (RT_FAILURE(rc))
            LogRelMax(16, ("VFIO#%d: The device could not be reset (%Rrc).  It keeps its state apart from being disabled "
                           "until the guest sets it up again\n", pThis->iInstance, rc));
    }

    /*
     * Whatever the reset has done to the command register of a function, the guest
     * sees a cleared one and that is what the device is to have.  Interrupts start
     * out as INTx.
     */
    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
    {
        PVFIOPCIFUN pFun = &pThis->aPciFuns[iPciFun];
        if (   pFun->iFdVfio == -1
            || pFun->fGone)
            continue;
        PPDMPCIDEV pPciDev = pDevIns->apPciDevs[iPciFun];

        int rc = pciVfioCfgSpaceWriteU16(pFun, VBOX_PCI_COMMAND, 0);
        if (RT_FAILURE(rc))
            LogRelMax(16, ("VFIO#%d.%u: Failed to clear the command register of the device after the reset (%Rrc)\n",
                           pThis->iInstance, pFun->uPciFun, rc));

        pciVfioIrqUpdate(pThis, pFun, PDMPciDevGetCommand(pPciDev),
                         pciVfioGetMsiCtrl(pFun, pPciDev), pciVfioGetMsixCtrl(pFun, pPciDev));
    }
}


/**
 * @callback_method_impl{FNDEVPCIHPSLOTLINK,
 *      The link between the root port and the hot-plug capable slot the device is
 *      in changed its state.}
 */
static DECLCALLBACK(void) pciVfioSlotLinkChanged(PPDMDEVINS pDevIns, bool fLinkUp)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);

    if (!fLinkUp)
    {
        /*
         * The guest switched the slot off or disabled the link, or the device was
         * taken out of the slot.  The guest doesn't get at the device until the link
         * is up again, and it expects a device which was reset then.  So what is in
         * service of the device, which is nothing if it was taken out, is disabled
         * and reset now.
         */
        pciVfioReset(pDevIns);
        ASMAtomicWriteU64(&pThis->msAttnPressed, 0);
    }
    LogRelMax(256, ("VFIO#%d: The link to the device is %s\n", pThis->iInstance, fLinkUp ? "up" : "down"));
}


/**
 * Takes the device out of the hot-plug capable slot it is in.
 *
 * The guest is asked to let go of the device first, by the attention button of
 * the slot, if it has the slot switched on.  It shows that it did by switching
 * the slot off, which a guest takes five seconds and more for.
 *
 * @returns VBox status code.
 * @retval  VERR_NOT_SUPPORTED if the device is not in a hot-plug capable slot.
 * @retval  VERR_TIMEOUT if the guest didn't let go of the device in time and
 *          fForce is clear.  The device is where it was.  The guest was asked and
 *          can still switch the slot off, which leaves the device in the slot.
 * @param   pDevIns     The device instance.
 * @param   cMsWait     How long to wait for the guest to let go of the device.
 * @param   fForce      Whether the device is taken away from the guest if it didn't
 *                      let go of it in time.
 *
 * @thread  Any but an EMT, which is needed for the guest to do its part.
 */
DECLHIDDEN(int) pciVfioR3SlotDetach(PPDMDEVINS pDevIns, RTMSINTERVAL cMsWait, bool fForce)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    if (!pThis->pDevInsPort)
        return VERR_NOT_SUPPORTED;

    uint64_t const msStart = RTTimeMilliTS();
    while (   devpciR3HotPlugSlotIsLinkUp(pThis->pDevInsPort)
           && !pciVfioIsOutOfService(pThis))
    {
        pciVfioSlotAskGuest(pThis);
        if (RTTimeMilliTS() - msStart >= cMsWait)
        {
            break;
        }
        RTThreadSleep(10);
    }

    pciVfioSlotEmpty(pThis, "it was detached");
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMDEVREG,pfnDestruct}
 */
static DECLCALLBACK(int) pciVfioDestruct(PPDMDEVINS pDevIns)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);

    /* Nobody is to take functions out of service from here on. */
    if (pThis->hThrdWatcher != NIL_RTTHREAD)
    {
        ASMAtomicWriteBool(&pThis->fWatcherStop, true);
        uint64_t const uOne      = 1;
        ssize_t const  cbWritten = write(pThis->iFdWatcherWakeup, &uOne, sizeof(uOne));
        RT_NOREF(cbWritten);
        int rc = RTThreadWait(pThis->hThrdWatcher, RT_MS_30SEC, NULL);
        AssertLogRelRC(rc);
        pThis->hThrdWatcher = NIL_RTTHREAD;
    }
    if (pThis->iFdWatcherWakeup != -1)
        close(pThis->iFdWatcherWakeup);
    pThis->iFdWatcherWakeup = -1;

    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
    {
        PVFIOPCIFUN pFun = &pThis->aPciFuns[iPciFun];

        if (pFun->iFdVfio != -1)
        {
            /* Detach the address space if the function got attached to one. */
            if (pFun->fIoasAttached)
            {
                struct vfio_device_detach_iommufd_pt VfioDetach;
                VfioDetach.argsz   = sizeof(VfioDetach);
                VfioDetach.flags   = 0;
                int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_DETACH_IOMMUFD_PT, &VfioDetach);
                if (rcLnx == -1)
                    LogRel(("VFIO#%d.%u: Failed to detach IOMMU page table with %d\n", pThis->iInstance, pFun->uPciFun, errno));
                pFun->fIoasAttached = false;
            }

            close(pFun->iFdVfio);
            pFun->iFdVfio = -1;
        }

        for (uint32_t i = 0; i < pFun->cFdEvts; i++)
            if (pFun->paFdEvts[i] != -1)
                close(pFun->paFdEvts[i]);
        RTMemFree(pFun->paFdEvts);
        pFun->paFdEvts = NULL;
        pFun->cFdEvts  = 0;

        RTMemFree(pFun->paMsixTbl);
        pFun->paMsixTbl = NULL;
        RTMemFree((void *)pFun->pbmMsixPba);
        pFun->pbmMsixPba = NULL;

        if (pFun->iFdWakeup != -1)
            close(pFun->iFdWakeup);
        if (pFun->iFdEvtReq != -1)
            close(pFun->iFdEvtReq);
        pFun->iFdEvtReq = -1;
        if (pFun->iFdEvtErr != -1)
            close(pFun->iFdEvtErr);
        pFun->iFdEvtErr = -1;
        if (pFun->iFdInert != -1)
            close(pFun->iFdInert);
        pFun->iFdInert = -1;
    }

    /* No device is left which could use the guest memory. */
    if (pThis->fGuestRamMapped)
        pciVfioIommuUnmapAll(pThis);

    if (pThis->fVfioLegacy)
    {
        if (pThis->VfioGroup.iFdVfioContainer != -1)
            close(pThis->VfioGroup.iFdVfioContainer);
        pThis->VfioGroup.iFdVfioContainer = -1;
        if (pThis->VfioGroup.iFdVfioGroup != -1)
            close(pThis->VfioGroup.iFdVfioGroup);
        pThis->VfioGroup.iFdVfioGroup = -1;
    }
    else
    {
        if (pThis->IommuFd.iFdIommu != -1)
        {
            if (pThis->fIoasAllocated)
            {
                struct iommu_destroy HwptDestroy;
                HwptDestroy.size = sizeof(HwptDestroy);
                HwptDestroy.id   = pThis->IommuFd.idIommuHwpt;
                int rcLnx = ioctl(pThis->IommuFd.iFdIommu, IOMMU_DESTROY, &HwptDestroy);
                if (rcLnx == -1)
                    LogRel(("VFIO#%d: Failed to destroy I/O address space with %d\n", pThis->iInstance, errno));
                pThis->fIoasAllocated = false;
            }

            close(pThis->IommuFd.iFdIommu);
            pThis->IommuFd.iFdIommu = -1;
        }
    }

    return VINF_SUCCESS;
}


/**
 * Refuses to have the state of the VM saved or teleported.
 *
 * @returns VERR_NOT_SUPPORTED.
 * @param   pDevIns     The device instance.
 */
static int pciVfioRefuseSavingState(PPDMDEVINS pDevIns)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    LogRel(("VFIO#%d: The state of the VM can't be saved with a PCI device of the host passed through to it\n",
            pThis->iInstance));
    int rc = PDMDevHlpVMSetRuntimeError(pDevIns, 0 /*fFlags*/, "PciVfioNoSavedState",
                                        N_("The state of the VM can't be saved or teleported while a PCI device of the "
                                           "host is passed through to it. Power the VM off instead, or remove the device "
                                           "from the VM first"));
    AssertRC(rc); RT_NOREF(rc);
    return VERR_NOT_SUPPORTED;
}


/**
 * @callback_method_impl{FNSSMDEVLIVEPREP, The state of a device of the host can't be moved to another host.}
 */
static DECLCALLBACK(int) pciVfioLivePrep(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    RT_NOREF(pSSM);
    return pciVfioRefuseSavingState(pDevIns);
}


/**
 * @callback_method_impl{FNSSMDEVSAVEPREP, The state of a device of the host can't be saved.}
 */
static DECLCALLBACK(int) pciVfioSavePrep(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    RT_NOREF(pSSM);
    return pciVfioRefuseSavingState(pDevIns);
}


/**
 * @callback_method_impl{FNSSMDEVLOADPREP,
 *      A saved state can only have been made without the device\, so the device was added afterwards.}
 */
static DECLCALLBACK(int) pciVfioLoadPrep(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    return pDevIns->pHlpR3->pfnSSMSetLoadError(pSSM, VERR_SSM_LOAD_CONFIG_MISMATCH, RT_SRC_POS,
                                               N_("The saved state was made without the PCI device of the host which is "
                                                  "passed through to the VM now. Remove the device from the VM or discard "
                                                  "the saved state"));
}


/**
 * @interface_method_impl{PDMDEVREG,pfnInitComplete}
 */
static DECLCALLBACK(int) pciVfioInitComplete(PPDMDEVINS pDevIns)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);

    /*
     * Make the guest memory available to the device.  This is done once and before
     * the guest runs: all of it or the VM doesn't start.  The guest can't enable
     * bus mastering without it, see pciVfioCfgEmulatedRegWrite().
     */
    int rc = pciVfioIommuGuestRamMap(pThis, pDevIns);
    if (RT_FAILURE(rc))
        return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                   N_("Failed to make the guest memory available to the passed through PCI device. "
                                      "The amount of memory the user running the VM may lock might be too low"));
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMDEVREG,pfnConstruct}
 */
static DECLCALLBACK(int) pciVfioConstruct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE pCfg)
{
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);

    PVFIOPCI      pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PCPDMDEVHLPR3 pHlp  = pDevIns->pHlpR3;

    pThis->pDevIns          = pDevIns;
    pThis->iInstance        = iInstance;
    pThis->IommuFd.iFdIommu = -1;
    pThis->fVfioLegacy      = false;
    pThis->fGuestRamMapped  = false;
    pThis->fIoasAllocated   = false;
    pThis->hThrdWatcher     = NIL_RTTHREAD;
    pThis->iFdWatcherWakeup = -1;
    pThis->fWatcherStop     = false;
    pThis->pDevInsPort      = NULL;
    pThis->cMsGuestRelease  = 15 * RT_MS_1SEC;
    pThis->msAttnPressed    = 0;

    for (uint8_t i = 0; i < RT_ELEMENTS(pThis->aPciFuns); i++)
    {
        PVFIOPCIFUN pFun = &pThis->aPciFuns[i];

        pFun->uPciFun        = UINT32_MAX;
        pFun->iFdVfio        = -1;
        pFun->fIoasAttached  = false;
        pFun->fGone          = false;
        pFun->fGoneForGuest  = false;
        pFun->iFdInert       = -1;
        pFun->iFdEvtReq      = -1;
        pFun->iFdEvtErr      = -1;
        pFun->iFdWakeup      = -1;
        pFun->fVga           = false;
        pFun->fInterceptMmio = false;
        pFun->iBarMsix       = UINT8_MAX;
        pFun->offMsiCtrl     = 0;
        pFun->uIrqMode       = UINT8_MAX;
        pFun->uIrqModeCur    = UINT8_MAX;
        pFun->uIrqModeNew    = UINT8_MAX;
    }

    int rc = PDMDevHlpSetDeviceCritSect(pDevIns, PDMDevHlpCritSectGetNop(pDevIns));
    if (RT_FAILURE(rc))
        return rc;

    /* There is no state to save, the unit is there for refusing to. */
    rc = PDMDevHlpSSMRegisterEx(pDevIns, 1 /*uVersion*/, 0 /*cbGuess*/, NULL /*pszBefore*/,
                                pciVfioLivePrep, NULL /*pfnLiveExec*/, NULL /*pfnLiveVote*/,
                                pciVfioSavePrep, NULL /*pfnSaveExec*/, NULL /*pfnSaveDone*/,
                                pciVfioLoadPrep, NULL /*pfnLoadExec*/, NULL /*pfnLoadDone*/);
    if (RT_FAILURE(rc))
        return rc;

    /*
     * Validate configuration.
     */
    if (!pHlp->pfnCFGMAreValuesValid(pCfg,
                                     "IommuPath\0"
                                     "VfioPath\0"
                                     "VfioGroupPath\0"
                                     "GuestReleaseTimeoutMs\0"
                                     "Fun*\0"
                                    ))
        return VERR_PDM_DEVINS_UNKNOWN_CFG_VALUES;

    /*
     * The guest memory gets mapped for the device once and has to stay where it is
     * for as long as the VM exists.  This is the case with the native execution
     * engine (KVM), where each RAM range is allocated in one piece and kept, and it
     * is not with the others, where pages are allocated and freed as the VM runs.
     */
    if (PDMDevHlpGetMainExecutionEngine(pDevIns) != VM_EXEC_ENGINE_NATIVE_API)
        return PDMDevHlpVMSetError(pDevIns, VERR_NOT_SUPPORTED, RT_SRC_POS,
                                   N_("PCI passthrough requires the native execution engine (KVM), which this VM doesn't use. "
                                      "Select it with 'VBoxManage modifyvm --vm-execution-engine native-api'"));

    /* How long the guest gets for letting go of the device when the host wants it back. */
    uint64_t cMsGuestRelease = 0;
    rc = pHlp->pfnCFGMQueryU64Def(pCfg, "GuestReleaseTimeoutMs", &cMsGuestRelease, pThis->cMsGuestRelease);
    if (   RT_FAILURE(rc)
        || cMsGuestRelease > 5 * RT_MS_1MIN)
        return PDMDevHlpVMSetError(pDevIns, RT_FAILURE(rc) ? rc : VERR_OUT_OF_RANGE, RT_SRC_POS,
                                   N_("Configuration error: \"GuestReleaseTimeoutMs\" is not a number of milliseconds up to five minutes"));
    pThis->cMsGuestRelease = (uint32_t)cMsGuestRelease;

    rc = pciVfioConfigureAccess(pDevIns, pThis, pHlp, pCfg);
    if (RT_FAILURE(rc))
        return rc;

    /* Initialize available functions. */
    uint32_t iPciDevNo = PDMPCIDEVREG_DEV_NO_FIRST_UNUSED;
    bool fMultiFn = false;
    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
    {
        PVFIOPCIFUN pFun    = &pThis->aPciFuns[iPciFun];
        PPDMPCIDEV  pPciDev = pDevIns->apPciDevs[iPciFun];
        PDMPCIDEV_ASSERT_VALID(pDevIns, pPciDev);

        /* Query per port configuration options if available. */
        PCFGMNODE pCfgFun = pHlp->pfnCFGMGetChildF(pCfg, "Fun%u", iPciFun);
        if (!pCfgFun)
            continue;

        if (iPciFun > 0)
            fMultiFn = true;

        pFun->uPciFun = iPciFun;
        RTStrPrintf(pFun->szName, sizeof(pFun->szName), "VFIO#%u.%u", pThis->iInstance, iPciFun);

        /*
         * Validate configuration.
         */
        if (!pHlp->pfnCFGMAreValuesValid(pCfgFun,
                                         "VfioPath\0"
                                         "ExposeVga\0"
                                         "InterceptMmio\0"
                                         "HostAddress\0"
                                        ))
            return VERR_PDM_DEVINS_UNKNOWN_CFG_VALUES;

        rc = pHlp->pfnCFGMQueryBoolDef(pCfgFun, "InterceptMmio", &pFun->fInterceptMmio, false);
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Configuration error: Querying \"InterceptMmio\" failed"));

        bool fVga = false;
        rc = pHlp->pfnCFGMQueryBoolDef(pCfgFun, "ExposeVga", &fVga, false);
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Configuration error: Querying \"ExposeVga\" failed"));

        rc = pciVfioFunConfigureAccess(pDevIns, pThis, pFun, pHlp, pCfgFun);
        if (RT_FAILURE(rc))
            return rc;

        struct vfio_device_info DevInfo;
        DevInfo.argsz = sizeof(DevInfo);
        int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_GET_INFO, &DevInfo);
        if (rcLnx == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Getting device information of opened VFIO device failed with %d"), errno);

        LogRel(("VFIO#%d.%u: Info flags:       %#RX32\n"
                "VFIO#%d.%u: Info num_regions: %#RU32\n"
                "VFIO#%d.%u: Info num_irqs:    %#RU32\n"
                "VFIO#%d.%u: Info cap_offset:  %#RX32\n",
                iInstance, iPciFun, DevInfo.flags,
                iInstance, iPciFun, DevInfo.num_regions,
                iInstance, iPciFun, DevInfo.num_irqs,
                iInstance, iPciFun, DevInfo.cap_offset));

        if (!(DevInfo.flags & VFIO_DEVICE_FLAGS_PCI))
            return PDMDevHlpVMSetError(pDevIns, VERR_NOT_SUPPORTED, RT_SRC_POS,
                                       N_("VFIO device is not a PCI/PCI Express device. This configuration is not supported"));
        if (!(DevInfo.flags & VFIO_DEVICE_FLAGS_RESET))
            LogRel(("VFIO#%d.%u: The function can't be reset on its own, a reset is going to affect everything on its bus\n",
                    iInstance, iPciFun));

        /* Setup access to the PCI config space first. */
        struct vfio_region_info RegionInfo;
        rc = pciVfioQueryRegionInfo(pThis, pFun, VFIO_PCI_CONFIG_REGION_INDEX, &RegionInfo);
        if (RT_FAILURE(rc))
            return rc;

        if (   (RegionInfo.flags & (VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE))
            != (VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE))
            return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                       N_("The PCI config region is not marked as read/write as expected"));

        pFun->offPciCfg = RegionInfo.offset;
        pFun->cbPciCfg  = RegionInfo.size;

        /* Setup the PCI config space. */
        rc = pciVfioCfgSpaceSetup(pFun, pPciDev);
        if (RT_FAILURE(rc))
            return rc;

        LogRel(("VFIO#%d.%u: Attached PCI device %04x:%04x\n", iInstance, iPciFun,
                PDMPciDevGetVendorId(pPciDev), PDMPciDevGetDeviceId(pPciDev)));

        /* Attach the device. */
        rc = PDMDevHlpPCIRegisterEx(pDevIns, pPciDev, 0 /*fFlags*/, iPciDevNo, iPciFun, pFun->szName);
        if (RT_FAILURE(rc))
            return rc;

        rc = PDMDevHlpPCIInterceptConfigAccesses(pDevIns, pPciDev, pciVfioConfigRead, pciVfioConfigWrite);
        if (RT_FAILURE(rc))
            return rc;

        /* Set up BAR0 through BAR5. */
        static uint32_t s_aVfioPciRegions[] =
        {
            VFIO_PCI_BAR0_REGION_INDEX,
            VFIO_PCI_BAR1_REGION_INDEX,
            VFIO_PCI_BAR2_REGION_INDEX,
            VFIO_PCI_BAR3_REGION_INDEX,
            VFIO_PCI_BAR4_REGION_INDEX,
            VFIO_PCI_BAR5_REGION_INDEX
        };

        for (uint32_t i = 0; i < RT_ELEMENTS(s_aVfioPciRegions); i++)
        {
            rc = pciVfioSetupBar(pThis, pFun, pDevIns, pPciDev, i, s_aVfioPciRegions[i]);
            if (RT_FAILURE(rc))
                return rc;
        }

        uint8_t bClassBase = PDMPciDevGetByte(pPciDev, VBOX_PCI_CLASS_BASE);
        uint8_t bClassSub  = PDMPciDevGetByte(pPciDev, VBOX_PCI_CLASS_SUB);
        if (   fVga
            && bClassBase == VBOX_PCI_CLASS_DISPLAY
            && bClassSub  == VBOX_PCI_SUB_DISPLAY_VGA)
        {
            rc = pciVfioSetupVga(pThis, pFun, pDevIns);
            if (RT_FAILURE(rc))
                return rc;
        }
        else
            pFun->fVga = false;

        rc = pciVfioSetupRom(pThis, pFun, pPciDev, pDevIns);
        if (RT_FAILURE(rc))
            return rc;

        /* Query the supported interrupt types now. */
        static uint32_t s_aVfioPciIrqs[] =
        {
            VFIO_PCI_INTX_IRQ_INDEX,
            VFIO_PCI_MSI_IRQ_INDEX,
            VFIO_PCI_MSIX_IRQ_INDEX
        };

        for (uint32_t i = 0; i < RT_ELEMENTS(s_aVfioPciIrqs); i++)
        {
            uint32_t const uIrq = s_aVfioPciIrqs[i];
            rc = pciVfioQueryIrqInfo(pThis, pFun, uIrq, &pFun->acIrqVectors[i]);
            if (RT_FAILURE(rc))
                return rc;
        }

        /* The numbers come from the device, make sure they are something we can deal with. */
        if (   pFun->acIrqVectors[VFIO_PCI_INTX_IRQ_INDEX] > 1
            || pFun->acIrqVectors[VFIO_PCI_MSI_IRQ_INDEX]  > VBOX_MSI_MAX_ENTRIES
            || pFun->acIrqVectors[VFIO_PCI_MSIX_IRQ_INDEX] > VBOX_MSIX_MAX_ENTRIES)
            return PDMDevHlpVMSetError(pDevIns, VERR_NOT_SUPPORTED, RT_SRC_POS,
                                       N_("The device has more interrupt vectors than supported (INTx: %u, MSI: %u, MSI-X: %u)"),
                                       pFun->acIrqVectors[VFIO_PCI_INTX_IRQ_INDEX], pFun->acIrqVectors[VFIO_PCI_MSI_IRQ_INDEX],
                                       pFun->acIrqVectors[VFIO_PCI_MSIX_IRQ_INDEX]);

        /*
         * Pre create the event file descriptors to not risk running out of file descriptors
         * while the VM is running.
         */
        uint32_t const cMaxVectors = RT_MAX(RT_MAX(pFun->acIrqVectors[VFIO_PCI_MSI_IRQ_INDEX],
                                                   pFun->acIrqVectors[VFIO_PCI_MSIX_IRQ_INDEX]),
                                            pFun->acIrqVectors[VFIO_PCI_INTX_IRQ_INDEX]);
        if (cMaxVectors)
        {
            pFun->paFdEvts = (int *)RTMemAlloc(cMaxVectors * sizeof(pFun->paFdEvts[0]));
            if (!pFun->paFdEvts)
                return PDMDevHlpVMSetError(pDevIns, VERR_NO_MEMORY, RT_SRC_POS,
                                           N_("Failed to allocate memory for %u event file descriptors for interrupts"),
                                           cMaxVectors);
            for (uint32_t i = 0; i < cMaxVectors; i++)
                pFun->paFdEvts[i] = -1;
            pFun->cFdEvts = cMaxVectors;
        }

        for (uint32_t i = 0; i < cMaxVectors; i++)
        {
            rc = pciVfioLnxEventfd2(0 /*uValInit*/, LNX_EFD_CLOEXEC, &pFun->paFdEvts[i]);
            if (RT_FAILURE(rc))
                return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                           N_("Failed to create event file descriptor %u out of %u for interrupts"),
                                           i, cMaxVectors);
        }

        /* Parse the capability lists now. */
        rc = pciVfioCfgSpaceParseCapabilities(pThis, pFun, pPciDev);
        if (RT_FAILURE(rc))
            return rc;

        if (pFun->cbPciCfg == 4096)
        {
            rc = pciVfioCfgSpaceParseExtCapabilities(pThis, pFun, pPciDev);
            if (RT_FAILURE(rc))
                return rc;
        }

        /* Create the MMIO region mappings now. */
        for (uint8_t i = 0; i < RT_ELEMENTS(pFun->aBars); i++)
        {
            /* PIO regions were created before. */
            if (pFun->aBars[i].bType == 2)
            {
                rc = pciVfioBarSetupMmio(pThis, pFun, pPciDev, i);
                if (RT_FAILURE(rc))
                    return rc;
            }
        }

        /* Create wakeup eventfd for IRQ poller. */
        rc = pciVfioLnxEventfd2(0 /*uValInit*/, LNX_EFD_CLOEXEC, &pFun->iFdWakeup);
        if (RT_FAILURE(rc))
            return rc;

        /* Spin up the interrupt poller. */
        char szDev[64];
        RT_ZERO(szDev);
        RTStrPrintf(szDev, sizeof(szDev), "VFIO#%u.%u", iInstance, iPciFun);
        rc = PDMDevHlpThreadCreate(pDevIns, &pFun->pThrdIrq, pFun, pciVfioIrqPoller, pciVfioIrqPollerWakeup,
                                   0 /* cbStack */, RTTHREADTYPE_IO, szDev);
        AssertLogRelRCReturn(rc, rc);

        /* SR-IOV devices don't offer INTx. */
        if (pFun->acIrqVectors[VFIO_PCI_INTX_IRQ_INDEX])
        {
            PDMPciDevSetInterruptPin(pPciDev, 0x01); /* A */

            rc = pciVfioIrqReconfigure(pThis, pFun, VFIO_PCI_INTX_IRQ_INDEX, 1 /*cVectors*/);
            if (RT_FAILURE(rc))
                return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                           N_("Failed to set initial interrupt mode to INTx"));
        }
        else
            PDMPciDevSetInterruptPin(pPciDev, 0);

        /* Apply any quirks for the function. */
        rc = pciVfioQuirkApply(pThis, pFun);
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS, N_("Applying quirk for device failed"));

        /*
         * For the host taking the device back while the VM exists: what takes the place
         * of the device file then, and the notifications telling that it is time.
         */
        rc = pciVfioLnxEventfd2(0 /*uValInit*/, LNX_EFD_CLOEXEC, &pFun->iFdInert);
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Failed to create the file descriptor standing in for a removed device"));
        rc = pciVfioNotifierSetup(pThis, pFun, VFIO_PCI_REQ_IRQ_INDEX, &pFun->iFdEvtReq);
        if (RT_SUCCESS(rc))
            rc = pciVfioNotifierSetup(pThis, pFun, VFIO_PCI_ERR_IRQ_INDEX, &pFun->iFdEvtErr);
        if (RT_FAILURE(rc))
            return rc;
        if (pFun->iFdEvtReq == -1)
            LogRel(("VFIO#%d.%u: The host can't tell when it wants the device back, removing it while the VM exists is going to block on the host\n",
                    iInstance, iPciFun));

        /* Subsequent function should use the same major as the previous one. */
        iPciDevNo = PDMPCIDEVREG_DEV_NO_SAME_AS_PREV;
    }

    /* The thread watching the notifications of the host about the functions. */
    rc = pciVfioLnxEventfd2(0 /*uValInit*/, LNX_EFD_CLOEXEC, &pThis->iFdWatcherWakeup);
    if (RT_FAILURE(rc))
        return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS, N_("Failed to create the event file descriptor of the watcher thread"));
    rc = RTThreadCreateF(&pThis->hThrdWatcher, pciVfioWatcherThread, pThis, 0 /*cbStack*/, RTTHREADTYPE_IO,
                         RTTHREADFLAGS_WAITABLE, "VFIO#%uW", iInstance);
    if (RT_FAILURE(rc))
    {
        pThis->hThrdWatcher = NIL_RTTHREAD;
        return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS, N_("Failed to create the watcher thread"));
    }

    /*
     * Behind a root port with a hot-plug capable slot the device is what is in the
     * slot.  This is the last thing which can fail, as the port keeps what it is told.
     */
    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
        if (pThis->aPciFuns[iPciFun].iFdVfio != -1)
        {
            PPDMDEVINS const pDevInsPort = devpciR3HotPlugSlotFind(pDevIns, pDevIns->apPciDevs[iPciFun]);
            if (pDevInsPort)
            {
                rc = devpciR3HotPlugSlotRegisterClient(pDevInsPort, pDevIns, pciVfioSlotLinkChanged, true /*fPresent*/);
                if (RT_FAILURE(rc))
                    return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                               N_("The hot-plug capable slot the passed through PCI device is configured "
                                                  "to be in is taken by another device"));
                pThis->pDevInsPort = pDevInsPort;
                LogRel(("VFIO#%d: The device is in the hot-plug capable slot of PCI bridge #%u\n", iInstance, pDevInsPort->iInstance));
            }
            break;
        }

    /* Need to fixup the header type for all functions. */
    if (fMultiFn)
    {
        for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
        {
            PPDMPCIDEV  pPciDev = pDevIns->apPciDevs[iPciFun];

            uint8_t const u8HdrType = PDMPciDevGetHeaderType(pPciDev);
            PDMPciDevSetHeaderType(pPciDev, u8HdrType | RT_BIT(7));
        }
    }

    return rc;
}


/**
 * The device registration structure.
 */
const PDMDEVREG g_DevicePciVfio =
{
    /* .u32Version = */             PDM_DEVREG_VERSION,
    /* .uReserved0 = */             0,
    /* .szName = */                 "pci-vfio",
    /* .fFlags = */                 PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_NEW_STYLE,
    /* .fClass = */                 PDM_DEVREG_CLASS_HOST_DEV,
    /* .cMaxInstances = */          ~0U,
    /* .uSharedVersion = */         42,
    /* .cbInstanceShared = */       sizeof(VFIOPCI),
    /* .cbInstanceCC = */           0,
    /* .cbInstanceRC = */           0,
    /* .cMaxPciDevices = */         8,
    /* .cMaxMsixVectors = */        VBOX_MSIX_MAX_ENTRIES,
    /* .pszDescription = */         "PCI passthrough through VFIO",
#if defined(IN_RING3)
    /* .pszRCMod = */               "",
    /* .pszR0Mod = */               "",
    /* .pfnConstruct = */           pciVfioConstruct,
    /* .pfnDestruct = */            pciVfioDestruct,
    /* .pfnRelocate = */            NULL,
    /* .pfnMemSetup = */            NULL,
    /* .pfnPowerOn = */             NULL,
    /* .pfnReset = */               pciVfioReset,
    /* .pfnSuspend = */             NULL,
    /* .pfnResume = */              NULL,
    /* .pfnAttach = */              NULL,
    /* .pfnDetach = */              NULL,
    /* .pfnQueryInterface = */      NULL,
    /* .pfnInitComplete = */        pciVfioInitComplete,
    /* .pfnPowerOff = */            NULL,
    /* .pfnSoftReset = */           NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
#elif defined(IN_RING0)
    /* .pfnEarlyConstruct = */      NULL,
    /* .pfnConstruct = */           NULL,
    /* .pfnDestruct = */            NULL,
    /* .pfnFinalDestruct = */       NULL,
    /* .pfnRequest = */             NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
#elif defined(IN_RC)
    /* .pfnConstruct = */           NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
#else
# error "Not in IN_RING3, IN_RING0 or IN_RC!"
#endif
    /* .u32VersionEnd = */          PDM_DEVREG_VERSION
};

#endif /* !VBOX_DEVICE_STRUCT_TESTCASE */

