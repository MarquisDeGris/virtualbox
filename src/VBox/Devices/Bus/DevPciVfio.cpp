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
 * The current emulation only supports MSI based interrupts. The legacy INTx style
 * doesn't work currently because VFIO auto masks those after they fired to prevent
 * an interrupt storm on the host and requires explicit unmasking after the guest
 * handled it but we currently lack a callback mechanism from the I/O APIC to get notified
 * about EOI events from the guest for the device.
 * MSI-X interrupt support is not implemented right now, so the passed through device needs
 * to support MSI in order to be supported.
 *
 * Saved states are not supported, and will never be as we can't capture the PCIe device state
 * and replay it when resuming.
 *
 * @section sec_dev_vfio_requirements
 *
 * The following requirements have to be met in order to support PCIe passthrough:
 *     - Recent enough hardware with IOMMU support
 *     - Linux host with at least kernel 6.12
 *     - The VM needs to have ICH9 configured
 *     - The VM needs to have UEFI configured
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
 * Create a VM and configure it to make use of the ICH9 chipset and enable EFI. Then add the following extradata to the VM
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
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/dir.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/string.h>
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

/** Invalid access. */
#define VFIO_PCI_CFG_SPACE_ACCESS_INVALID     0
/** Passthrough the access to VFIO. */
#define VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH 1
/** Just do the default action for the access. */
#define VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT  2
/** Emulate the access using a special handler. */
#define VFIO_PCI_CFG_SPACE_ACCESS_EMULATE     3


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
 * @callback_method_impl{FNIOMIOPORTNEWOUT}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioPioWrite(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT offPort, uint32_t u32, unsigned cb)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PCVFIOPCIBAR pBar = (PCVFIOPCIBAR)pvUser;
    PCVFIOPCIFUN pFun = &pThis->aPciFuns[pBar->iPciFun];
    ssize_t cbWritten = pwrite(pFun->iFdVfio, &u32, cb, pBar->offVfio + offPort);
    if (cbWritten != cb)
        return RTErrConvertFromErrno(errno);

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
    ssize_t cbRead = pread(pFun->iFdVfio, pu32, cb, pBar->offVfio + offPort);
    if (cbRead != cb)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
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
    /* Can't use PDMDevHlpPCISetIrqEx here as the PCI stuff is not aware of MSI-X support. */
    /** @todo This isn't pretty as it accesses stuff internal to the PCI subsystem. */
    PCPDMPCIHLPR3 pPciHlp = (PCPDMPCIHLPR3)pPciDev->Int.s.pvPciBusPtrR3;
    AssertPtrReturnVoid(pPciHlp);
    PPDMDEVINS pDevInsBus = pPciHlp->pfnGetBusByNo(pDevIns, pPciDev->Int.s.idxPdmBus);
    AssertPtrReturnVoid(pDevInsBus);
    PDEVPCIBUS pBus = PDMINS_2_DATA(pDevInsBus, PDEVPCIBUS);
    uint16_t const uBusDevFn = PCIBDF_MAKE(pBus->iBus, pPciDev->uDevFn);

    PMSIXTBLENTRY pVec = &pFun->paMsixTbl[idxVector];
    MSIMSG Msi;
    Msi.Addr.u64 = ((uint64_t)pVec->u32MsgAddrUpper << 32) | pVec->u32MsgAddr;
    Msi.Data.u32 = pVec->u32MsgData;

    pPciHlp->pfnIoApicSendMsi(pDevIns, uBusDevFn, &Msi, 0 /*uTagSrc*/);
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
    RT_NOREF(pThis);
    if (pBar->pvMmio)
    {
        /* Device registers have to be accessed with the width the guest uses. */
        volatile uint8_t *pb = (volatile uint8_t *)pBar->pvMmio + offBar;
        if (fWrite)
            switch (cb)
            {
                case 1: *pb                       = *(uint8_t const *)pv;  break;
                case 2: *(volatile uint16_t *)pb  = *(uint16_t const *)pv; break;
                case 4: *(volatile uint32_t *)pb  = *(uint32_t const *)pv; break;
                case 8: *(volatile uint64_t *)pb  = *(uint64_t const *)pv; break;
                default:
                    for (size_t i = 0; i < cb; i++)
                        pb[i] = ((uint8_t const *)pv)[i];
                    break;
            }
        else
            switch (cb)
            {
                case 1: *(uint8_t *)pv  = *pb;                      break;
                case 2: *(uint16_t *)pv = *(volatile uint16_t *)pb; break;
                case 4: *(uint32_t *)pv = *(volatile uint32_t *)pb; break;
                case 8: *(uint64_t *)pv = *(volatile uint64_t *)pb; break;
                default:
                    for (size_t i = 0; i < cb; i++)
                        ((uint8_t *)pv)[i] = pb[i];
                    break;
            }
    }
    else
    {
        /* The region is not mapped, go through the device file descriptor. */
        ssize_t const cbDone = fWrite
                             ? pwrite(pFun->iFdVfio, pv, cb, pBar->offVfio + offBar)
                             : pread(pFun->iFdVfio, pv, cb, pBar->offVfio + offBar);
        if (cbDone != (ssize_t)cb)
        {
            /* Behave like a device which doesn't respond: reads return all ones, writes get dropped. */
            if (!fWrite)
                memset(pv, 0xff, cb);
            LogRelMax(32, ("VFIO#%d.%u: %s %zu byte(s) at offset %#RX64 of a BAR failed with %d\n", pThis->iInstance,
                           pFun->uPciFun, fWrite ? "Writing" : "Reading", cb, offBar, errno));
        }
    }
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


static int pciVfioBarContainsMsix(PVFIOPCI pThis, PVFIOPCIFUN pFun, uint32_t uRegion, uint32_t cbInfo, bool *pfMsix)
{
    struct vfio_region_info *pRegionInfo = (struct vfio_region_info *)RTMemTmpAllocZ(cbInfo);
    if (!pRegionInfo)
        return VERR_NO_TMP_MEMORY;

    pRegionInfo->argsz = cbInfo;
    pRegionInfo->index = uRegion;

    int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_GET_REGION_INFO, pRegionInfo);
    if (rcLnx == -1)
        return PDMDevHlpVMSetError(pThis->pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                N_("Getting information for region %u of opened VFIO device failed with %d"), uRegion, errno);

    AssertReturnStmt(pRegionInfo->cap_offset != 0, RTMemTmpFree(pRegionInfo), VERR_INVALID_STATE);
    uint32_t offCap = pRegionInfo->cap_offset;
    *pfMsix = false;
    while (offCap)
    {
        struct vfio_info_cap_header *pHdr = (struct vfio_info_cap_header *)((uint8_t *)pRegionInfo + offCap);
        if (pHdr->id == VFIO_REGION_INFO_CAP_MSIX_MAPPABLE)
        {
            *pfMsix = true;
            break;
        }

        offCap = pHdr->next;
    }

    RTMemTmpFree(pRegionInfo);
    return VINF_SUCCESS;
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
             * Map the region if VFIO allows it and if it consists of whole pages.  A region
             * which can't be mapped is accessed through the device file descriptor, which
             * means trapping every access of the guest.
             */
            PVFIOPCIBAR pBar = &pFun->aBars[uRegion];
            pBar->bType   = 2;
            pBar->offVfio = RegionInfo.offset;
            pBar->pvMmio  = NULL;
            if (   (RegionInfo.flags & VFIO_REGION_INFO_FLAG_MMAP)
                && !(RegionInfo.size & GUEST_PAGE_OFFSET_MASK))
            {
                int fProt =   ((RegionInfo.flags & VFIO_REGION_INFO_FLAG_READ)  ? PROT_READ  : 0)
                            | ((RegionInfo.flags & VFIO_REGION_INFO_FLAG_WRITE) ? PROT_WRITE : 0);
                void *pvMmio = mmap(NULL, RegionInfo.size, fProt, MAP_FILE | MAP_SHARED, pFun->iFdVfio, RegionInfo.offset);
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
    RT_NOREF(pDevIns);
    PCVFIOPCIFUN pFun = (PCVFIOPCIFUN)pvUser;

    ssize_t cbRead = pwrite(pFun->iFdVfio, &u32, cb, pFun->offVga + offPort);
    if (cbRead != cb)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMIOPORTNEWIN}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioVgaPioRead(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT offPort, uint32_t *pu32, unsigned cb)
{
    RT_NOREF(pDevIns);
    PCVFIOPCIFUN pFun = (PCVFIOPCIFUN)pvUser;

    ssize_t cbRead = pread(pFun->iFdVfio, pu32, cb, pFun->offVga + offPort);
    if (cbRead != cb)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMMMIONEWREAD}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioVgaMmioRead(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS off, void *pv, unsigned cb)
{
    RT_NOREF(pDevIns);
    PCVFIOPCIFUN pFun = (PCVFIOPCIFUN)pvUser;

    ssize_t cbRead = pread(pFun->iFdVfio, pv, cb, pFun->offVga + off);
    if (cbRead != cb)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMMMIONEWWRITE}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioVgaMmioWrite(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS off, void const *pv, unsigned cb)
{
    RT_NOREF(pDevIns);
    PCVFIOPCIFUN pFun = (PCVFIOPCIFUN)pvUser;

    ssize_t cbWritten = pwrite(pFun->iFdVfio, pv, cb, pFun->offVga + off);
    if (cbWritten != cb)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMMMIONEWFILL}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioVgaMmioFill(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS off, uint32_t u32Item, unsigned cbItem, unsigned cItems)
{
    RT_NOREF(pDevIns);
    PCVFIOPCIFUN pFun = (PCVFIOPCIFUN)pvUser;

    uint8_t abVal[4] = { 0 };
    for (uint8_t i = 0; i < RT_ELEMENTS(abVal); i++)
    {
        abVal[i] = u32Item & 0xff;
        u32Item >>= 8;
    }

    ssize_t const cb = (ssize_t)cbItem * cItems;
    uint8_t *pb = (uint8_t *)RTMemTmpAlloc(cb);
    if (!pb)
        return VERR_NO_MEMORY;

    uint8_t *pbCur = pb;

    switch (cbItem)
    {
        case 1:
            for (uint32_t i = 0; i < cItems; i++)
                *pbCur++ = abVal[0];
            break;
        case 2:
            for (uint32_t i = 0; i < cItems; i++)
            {
                pbCur[0] = abVal[0];
                pbCur[1] = abVal[1];
                pbCur += 2;
            }
            break;
        case 4:
            for (uint32_t i = 0; i < cItems; i++)
            {
                pbCur[0] = abVal[0];
                pbCur[1] = abVal[1];
                pbCur[2] = abVal[2];
                pbCur[3] = abVal[3];
                pbCur += 4;
            }
            break;
        default:
            AssertFailedReturn(VERR_NOT_SUPPORTED);
    }

    ssize_t cbWritten = pwrite(pFun->iFdVfio, pb, cb, pFun->offVga + off);
    RTMemTmpFree(pb);

    if (cbWritten != cb)
        return RTErrConvertFromErrno(errno);

    return VINF_SUCCESS;
}


static int pciVfioSetupVga(PVFIOPCI pThis, PVFIOPCIFUN pFun, PPDMDEVINS pDevIns)
{
    struct vfio_region_info RegionInfo; RT_ZERO(RegionInfo);
    int rc = pciVfioQueryRegionInfo(pThis, pFun, VFIO_PCI_VGA_REGION_INDEX, &RegionInfo);
    if (RT_FAILURE(rc))
        return rc;

    AssertLogRelMsgReturn(   RegionInfo.flags
                          && RegionInfo.size,
                          ("VGA/GPU does not support VFIO_PCI_VGA_REGION_INDEX\n"),
                          VERR_NOT_SUPPORTED);

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
        return PDMDEV_SET_ERROR(pDevIns, rc, "Mapping legacy VGA ports 0x3b0 - 0x3bb failed");

    /*
     * The MDA/CGA/EGA/VGA/whatever fixed MMIO area.
     */
    rc = PDMDevHlpMmioCreateExAndMap(pDevIns, 0x000a0000, 0x00020000,
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


DECLINLINE(void) pciVfioCfgSpaceSetInterceptU8(PVFIOPCIFUN pFun, uint32_t off, uint8_t fRd, uint8_t fWr)
{
    AssertReturnVoid(off < sizeof(pFun->abPciCfgIntercept) * 8 / 4);
    uint32_t offByte = off >> 1;
    uint8_t  cShift  = (off & 0x1) ? 4 : 0;

    pFun->abPciCfgIntercept[offByte] |= ((fWr << 2) | fRd) << cShift;
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
    if (!offCap)
    {
        /* No capabilities, return early. */
        return VINF_SUCCESS;
    }

    /* This ASSUMES that the cpabilities are not going backwards when pointing to the next one. */
    uint8_t offCapNextPrev = VBOX_PCI_CAPABILITY_LIST;
    for (;;)
    {
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
                LogRel(("VFIO#%d.%u: Cap[%#x]: Message Signaled Interrupts -> emulate\n", pThis->iInstance, pFun->uPciFun, offCap));
                fSupported = true;

                /* Read the message control from the device. */
                uint16_t u16Mmc = 0;
                rc = pciVfioCfgSpaceReadU16(pFun, offCap + 2, &u16Mmc);
                if (RT_FAILURE(rc))
                    return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                               N_("Failed to read MSI message control register with %Rrc"), rc);

                bool f64Bit = RT_BOOL(u16Mmc & VBOX_PCI_MSI_FLAGS_64BIT);
                uint8_t cVectors = RT_BIT((u16Mmc & VBOX_PCI_MSI_FLAGS_QMASK) >> 1);

                /* Add capability. */
                PDMMSIREG MsiReg;
                RT_ZERO(MsiReg);
                MsiReg.cMsiVectors     = cVectors;
                MsiReg.iMsiCapOffset   = offCap;
                MsiReg.iMsiNextOffset  = 0; /* Gets updated later. */
                MsiReg.fMsi64bit       = f64Bit;
                rc = PDMDevHlpPCIRegisterMsiEx(pDevIns, pPciDev, &MsiReg);
                if (RT_FAILURE(rc))
                    AssertFailed();

                pciVfioCfgSpaceSetInterceptU16(pFun, offCap +  2, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_EMULATE);    /* Message Control */
                pciVfioCfgSpaceSetInterceptU32(pFun, offCap +  4, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT); /* Message Address [Low] */
                if (f64Bit)
                {
                    pciVfioCfgSpaceSetInterceptU32(pFun, offCap +  8, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT); /* Message Address High */
                    pciVfioCfgSpaceSetInterceptU32(pFun, offCap + 12, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT); /* Message Data */

                    /* We always implement MSI with per-vector masking support. */
                    pciVfioCfgSpaceSetInterceptU32(pFun, offCap + 16, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT); /* Mask */
                    pciVfioCfgSpaceSetInterceptU32(pFun, offCap + 20, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT); /* Pending */
                }
                else
                {
                    pciVfioCfgSpaceSetInterceptU32(pFun, offCap + 8, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT); /* Message Data */

                    /* We always implement MSI with per-vector masking support. */
                    pciVfioCfgSpaceSetInterceptU32(pFun, offCap + 12, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT); /* Mask */
                    pciVfioCfgSpaceSetInterceptU32(pFun, offCap + 16, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT, VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT); /* Pending */
                }

                pFun->offMsiCtrl = offCap + 2;
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

                for (uint16_t i = offCap + 2; i < (uint16_t)offCap + cbCap2; i += 2)
                    pciVfioCfgSpaceSetInterceptU16(pFun, i, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH);
                cbCap = cbCap2;
#endif

                break;
            }
            case VBOX_PCI_CAP_ID_MSIX:
            {
                LogRel(("VFIO#%d.%u: Cap[%#x]: MSI-X -> emulate\n", pThis->iInstance, pFun->uPciFun, offCap));
                rc = pciVfioTrySetupMsix(pThis, pFun, pPciDev, offCap);
                if (RT_SUCCESS(rc))
                {
                    fSupported = true;
                    cbCap      = 12;
                }
                break;
            }
            case VBOX_PCI_CAP_ID_SATA: LogRel(("VFIO#%d.%u: Cap[%#x]: Serial ATA HBA -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap));        break;
            case VBOX_PCI_CAP_ID_AF:   LogRel(("VFIO#%d.%u: Cap[%#x]: PCI Advanced Features -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap)); break;
            default:
                LogRel(("VFIO#%d.%u: Cap[%#x]: Unknown capability ID %#x encountered -> unsupported\n", pThis->iInstance, pFun->uPciFun, offCap, bCapId));
                break;
        }

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

        if (   offCapNext < offCap
            || offCapNext < offCap + cbCap)
            return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                       N_("Next capability pointer points backwards or inside the current capability (next offset %#x )"), offCapNext);

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

    /* This ASSUMES that the cpabilities are not going backwards when pointing to the next one. */
    for (;;)
    {
        uint32_t u32CapHdr = 0;
        int rc = pciVfioCfgSpaceReadU32(pFun, offCap, &u32CapHdr);
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Failed to read extended capabilitiy header at offset %#x with %Rrc"), offCap, rc);

        if (!u32CapHdr)
            break;

        uint16_t const u16CapId   = u32CapHdr & UINT16_C(0xffff);
        uint8_t  const bCapVers   = (u32CapHdr >> 16) & 0xf; RT_NOREF(bCapVers);
        uint16_t const offCapNext = (u32CapHdr >> 20) & 0xfff;
        uint16_t       cbCap      = sizeof(uint32_t);
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

        /*
         * Don't allow pointing inside the current capability, pointing backwards is allowed as that was seen on
         * an Intel ARC Pro B70 GPU and there is nothing in the PCIe specification prohibiting that.
         */
        /** @todo Maybe check whether the new capability was visited already to avoid endless loops, which would
         * indicate a broken device. */
        if (   offCapNext >= offCap
            && offCapNext < offCap + cbCap)
            return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_STATE, RT_SRC_POS,
                                       N_("Next capability pointer points inside the current capability (next offset %#x, current [%#x:%#x[)"), offCapNext, offCap, cbCap);

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


static int pciVfioMapRegion(PVFIOPCI pThis, RTGCPHYS GCPhysStart, uintptr_t uPtrMapping, size_t cbMapping)
{
    if (pThis->fVfioLegacy)
    {
        struct vfio_iommu_type1_dma_map Map;
        Map.argsz      = sizeof(Map);
        Map.flags      = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
        Map.vaddr      = uPtrMapping;
        Map.size       = cbMapping;
        Map.iova       = GCPhysStart;

        int rcLnx = ioctl(pThis->VfioGroup.iFdVfioContainer, VFIO_IOMMU_MAP_DMA, &Map);
        if (rcLnx == -1)
        {
            LogRel(("errno=%d\n", errno));
            return RTErrConvertFromErrno(errno);
        }
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

        int rcLnx = ioctl(pThis->IommuFd.iFdIommu, IOMMU_IOAS_MAP, &Map);
        if (rcLnx == -1)
        {
            LogRel(("errno=%d\n", errno));
            return RTErrConvertFromErrno(errno);
        }
    }

    return VINF_SUCCESS;
}


static void pciVfioIommuGuestRamMapRange(PVFIOPCI pThis, PPDMDEVINS pDevIns, RTGCPHYS GCPhysStart, RTGCPHYS cbMap)
{
    /** @todo This is a really gross hack because we currently lack
     * a dedicated interface to get knowledge about guest RAM mappings.
     * This might also return mappings for stuff not being guest RAM.
     * Also this will allocate all guest RAM instantly but there is no way
     * around that anyway in order for the IOMMU to work correctly.
     */
    uintptr_t uPtrMapping = 0;
    size_t    cbMapping   = 0;
    for (RTGCPHYS GCPhys = GCPhysStart; GCPhys < GCPhysStart + cbMap; GCPhys += _4K)
    {
        void *pv = NULL;
        PGMPAGEMAPLOCK Lock;
        int rc = PDMDevHlpPhysGCPhys2CCPtr(pDevIns, GCPhys, 0 /*fFlags*/, &pv, &Lock);
        if (RT_SUCCESS(rc))
        {
            if (cbMapping)
            {
                if (uPtrMapping + cbMapping == (uintptr_t)pv)
                    cbMapping += _4K;
                else
                {
                    rc = pciVfioMapRegion(pThis, GCPhysStart, uPtrMapping, cbMapping);
                    if (RT_FAILURE(rc))
                        LogRel(("Mapping %RGp/%zu failed with %Rrc\n", GCPhysStart, cbMapping, rc));

                    GCPhysStart = GCPhys;
                    uPtrMapping = (uintptr_t)pv;
                    cbMapping = _4K;
                }
            }
            else
            {
                Assert(!uPtrMapping);
                GCPhysStart = GCPhys;
                uPtrMapping = (uintptr_t)pv;
                cbMapping = _4K;
            }
            PDMDevHlpPhysReleasePageMappingLock(pDevIns, &Lock);
        }
        else if (cbMapping)
        {
            LogRel(("PDMDevHlpPhysGCPhys2CCPtr(,%RGp) -> %Rrc\n", GCPhys, rc));
            /* Map what we currently have. */
            Assert(uPtrMapping);
            rc = pciVfioMapRegion(pThis, GCPhysStart, uPtrMapping, cbMapping);
            if (RT_FAILURE(rc))
                LogRel(("Mapping %RGp/%zu failed with %Rrc\n", GCPhysStart, cbMapping, rc));
            cbMapping   = 0;
            uPtrMapping = 0;
            GCPhysStart = GCPhys;
        }
    }

    if (cbMapping)
    {
        /* Map what we currently have. */
        Assert(uPtrMapping);
        int rc = pciVfioMapRegion(pThis, GCPhysStart, uPtrMapping, cbMapping);
        if (RT_FAILURE(rc))
            LogRel(("Mapping %RGp/%zu failed with %Rrc\n", GCPhysStart, cbMapping, rc));
        cbMapping   = 0;
        uPtrMapping = 0;
    }
}


static int pciVfioIommuGuestRamMap(PVFIOPCI pThis, PPDMDEVINS pDevIns)
{
    if (!pThis->fGuestRamMapped)
    {
        pciVfioIommuGuestRamMapRange(pThis, pDevIns,   0 /*GCPhysStart*/, PDMDevHlpMMPhysGetRamSizeBelow4GB(pDevIns));
        pciVfioIommuGuestRamMapRange(pThis, pDevIns, _4G /*GCPhysStart*/, PDMDevHlpMMPhysGetRamSizeAbove4GB(pDevIns));
        pThis->fGuestRamMapped = true;
    }

    return VINF_SUCCESS;
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
                LogRel(("VFIO#%d.%u: Failed to passthrough PCI config read at offset %#x with %Rrc, returning 0\n",
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
                    default:
                        AssertReleaseFailed();
                }
                break;
            }
            default:
                AssertReleaseFailed();
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
 * @callback_method_impl{FNPCICONFIGWRITE}
 */
static DECLCALLBACK(VBOXSTRICTRC) pciVfioConfigWrite(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev,
                                                     uint32_t uAddress, unsigned cb, uint32_t u32Value)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);
    PVFIOPCIFUN pFun = &pThis->aPciFuns[pPciDev->idxSubDev];

    /** @todo Only handles accesses with the same intercept config for now. */
    uint8_t fWr = pciVfioCfgSpaceGetInterceptWr(pFun, uAddress);
    for (uint8_t i = 1; i < cb; i++)
    {
        uint8_t fWrU8 = pciVfioCfgSpaceGetInterceptWr(pFun, uAddress + i);
        if (fWr != fWrU8)
        {
            LogRel(("VFIO#%d.%u: Complicated PCI config space write at offset %#x (+ %u) intercept %u vs %u\n",
                    pThis->iInstance, pFun->uPciFun, uAddress, i, fWr, fWrU8));
            return VINF_SUCCESS;
        }
    }

    int rc = VINF_SUCCESS;
    switch (fWr)
    {
        case VFIO_PCI_CFG_SPACE_ACCESS_INVALID:
        {
            LogRel(("VFIO#%d.%u: Invalid PCI config write at offset %#x, ignoring\n",
                    pThis->iInstance, pFun->uPciFun, uAddress));
            break;
        }
        case VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH:
        {
            rc = pciVfioConfigPassthroughWrite(pFun, uAddress, cb, u32Value);
            break;
        }
        case VFIO_PCI_CFG_SPACE_ACCESS_DO_DEFAULT:
            return VINF_PDM_PCI_DO_DEFAULT;
        case VFIO_PCI_CFG_SPACE_ACCESS_EMULATE:
        {
            /* Map all the guest memory into the IOMMU as soon as the BUSMASTER bit is enabled. */
            if (uAddress == VBOX_PCI_COMMAND)
            {
                if (u32Value & RT_BIT(2))
                {
                    rc = pciVfioIommuGuestRamMap(pThis, pDevIns);
                    if (RT_FAILURE(rc))
                        LogRel(("VFIO#%d.%u: Failed to map guest RAM into IOMMU for device, expect broken device (%Rrc)\n",
                                pThis->iInstance, pFun->uPciFun, rc));
                }

                /* The INTx disable bit decides whether INTx gets used when neither MSI nor MSI-X is enabled. */
                uint16_t u16Cmd = (uint16_t)u32Value;
                if (cb < sizeof(uint16_t))
                    u16Cmd = (uint16_t)((PDMPciDevGetCommand(pPciDev) & UINT16_C(0xff00)) | (u16Cmd & UINT16_C(0x00ff)));
                pciVfioIrqUpdate(pThis, pFun, u16Cmd, pciVfioGetMsiCtrl(pFun, pPciDev), pciVfioGetMsixCtrl(pFun, pPciDev));

                /* Now write to the device. */
                rc = pciVfioConfigPassthroughWrite(pFun, uAddress, cb, u32Value);
                if (RT_FAILURE(rc))
                    LogRel(("VFIO#%d.%u: Failed to update command register in device (%Rrc)", pThis->iInstance, pFun->uPciFun, rc));
                rc = VINF_PDM_PCI_DO_DEFAULT; /* Need to update BAR mappings. */
            }
            else if (   pFun->offMsiCtrl == uAddress
                     && cb == sizeof(uint16_t))
            {
                pciVfioIrqUpdate(pThis, pFun, PDMPciDevGetCommand(pPciDev), (uint16_t)u32Value, pciVfioGetMsixCtrl(pFun, pPciDev));

                /* Now write to the device. */
                rc = pciVfioConfigPassthroughWrite(pFun, uAddress, cb, u32Value);
                if (RT_FAILURE(rc))
                    LogRel(("VFIO#%d.%u: Failed to update MSI control register in device (%Rrc)", pThis->iInstance, pFun->uPciFun, rc));
                rc = VINF_PDM_PCI_DO_DEFAULT; /* Need to update our internal MSI state. */
            }
            else if (   pFun->offMsixCtrl == uAddress
                     && cb == sizeof(uint16_t))
            {
                /* Only the enable and the function mask bit are writable, the rest describes the device. */
                uint16_t const fMsixCtrlRw = VBOX_PCI_MSIX_FLAGS_ENABLE | VBOX_PCI_MSIX_FLAGS_FUNCMASK;
                uint16_t const u16MsixCtrl = (uint16_t)(  (PDMPciDevGetWord(pPciDev, uAddress) & ~fMsixCtrlRw)
                                                        | ((uint16_t)u32Value & fMsixCtrlRw));
                pciVfioIrqUpdate(pThis, pFun, PDMPciDevGetCommand(pPciDev), pciVfioGetMsiCtrl(pFun, pPciDev), u16MsixCtrl);

                /* Now write to the device. */
                rc = pciVfioConfigPassthroughWrite(pFun, uAddress, cb, u16MsixCtrl);
                if (RT_FAILURE(rc))
                    LogRel(("VFIO#%d.%u: Failed to update MSI-X control register in device (%Rrc)", pThis->iInstance, pFun->uPciFun, rc));
                PDMPciDevSetWord(pPciDev, uAddress, u16MsixCtrl);

                /* Vectors held back by the function mask can go out now. */
                for (uint32_t i = 0; i < pFun->acIrqVectors[VFIO_PCI_MSIX_IRQ_INDEX]; i++)
                    pciVfioMsixDeliverPending(pDevIns, pFun, pPciDev, i);
                rc = VINF_SUCCESS;
            }
            else
                AssertFailed();
            break;
        }
        default:
            AssertReleaseFailed();
    }

    return rc;
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

    for (uint32_t i = 0x50; i < 0x50 + 16; i += 4)
        pciVfioCfgSpaceSetInterceptRoU32(pFun, i, VFIO_PCI_CFG_SPACE_ACCESS_PASSTHROUGH);

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


static int pciVfioConfigureAccess(PPDMDEVINS pDevIns, PVFIOPCI pThis, PCPDMDEVHLPR3 pHlp, PCFGMNODE pCfg)
{
    /* Query configuration, configuration using the IOMMUFD takes precedence over the legacy VFIO group style. */
    char szPath[RTPATH_MAX];
    int rc = pHlp->pfnCFGMQueryStringDef(pCfg, "IommuPath", &szPath[0], sizeof(szPath), "/dev/iommu");
    if (RT_FAILURE(rc))
        return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                   N_("Configuration error: Querying \"IommuPath\" failed"));

    pThis->IommuFd.iFdIommu = open(szPath, O_RDWR);
    if (pThis->IommuFd.iFdIommu == -1)
    {
        pThis->fVfioLegacy = true;
        pThis->VfioGroup.iFdVfioGroup     = -1;
        pThis->VfioGroup.iFdVfioContainer = -1;

        rc = pHlp->pfnCFGMQueryStringDef(pCfg, "VfioPath", &szPath[0], sizeof(szPath), "/dev/vfio/vfio");
        if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Configuration error: Querying \"VfioPath\" failed, no access method configured"));

        pThis->VfioGroup.iFdVfioContainer = open(szPath, O_RDWR);
        if (pThis->VfioGroup.iFdVfioContainer == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Opening VFIO container path \"%s\" failed with %d"), szPath, errno);

        int rcLnx = ioctl(pThis->VfioGroup.iFdVfioContainer, VFIO_GET_API_VERSION);
        if (rcLnx == -1)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Querying VFIO API version failed"));
        if (rcLnx != VFIO_API_VERSION)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("Returned VFIO API version %d doesn't match expected version %d\n"),
                                       rcLnx, VFIO_API_VERSION);

        rcLnx = ioctl(pThis->VfioGroup.iFdVfioContainer, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU);
        if (rcLnx == 0)
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
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

            char szIommuGroup[8];
            szIommuGroup[0] = '\0';
            size_t cchIommuGroup = 0;
            rc = RTLinuxSysFsGetLinkDest(&szIommuGroup[0], sizeof(szIommuGroup), &cchIommuGroup, "/sys/bus/pci/devices/%s/iommu_group", szHostAddress);
            if (RT_FAILURE(rc))
                return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                           N_("Configuration error: Resolving the IOMMU group of the PCI device failed"));

            rc = RTPathJoin(szPath, sizeof(szPath), "/dev/vfio", szIommuGroup);
            AssertRC(rc);
        }
        else  if (RT_FAILURE(rc))
            return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                       N_("Configuration error: Querying \"VfioGroupPath\" failed"));

        pThis->VfioGroup.iFdVfioGroup = open(szPath, O_RDWR);
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
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
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
    }
    else
    {
        if (fHostAddress)
        {
            /* Try determining the VFIO device from sysfs. */
            char szSysfsPath[128];
            rc = RTPathJoin(szSysfsPath, sizeof(szSysfsPath), "/sys/bus/pci/devices", szPath);
            AssertRC(rc);
            rc = RTPathAppend(szSysfsPath, sizeof(szSysfsPath), "vfio-dev");
            AssertRC(rc);

            RTDIR hDir = NIL_RTDIR;
            rc = RTDirOpen(&hDir, szSysfsPath);
            if (RT_SUCCESS(rc))
            {
                for (;;)
                {
                    RTDIRENTRY Entry; /* The default name size is more than enough for the format xxxx:xx:xx.x */
                    rc = RTDirRead(hDir, &Entry, NULL /*pcbDirEntry*/);
                    if (RT_FAILURE(rc))
                        break;

                    if (RTDirEntryIsStdDotLink(&Entry))
                        continue;

                    if (!strncmp(Entry.szName, RT_STR_TUPLE("vfio")))
                    {
                        rc = RTPathJoin(szPath, sizeof(szPath), "/dev/vfio/devices", Entry.szName);
                        AssertRC(rc);
                        break;
                    }
                }

                int rc2 = RTDirClose(hDir);
                AssertRC(rc2); RT_NOREF(rc2);

                if (RT_FAILURE(rc))
                    return PDMDevHlpVMSetError(pDevIns, rc, RT_SRC_POS,
                                               N_("Configuration error: Resolving the VFIO device failed"));
            }
        }

        pFun->iFdVfio = open(szPath, O_RDWR);
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
    }

    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMDEVREG,pfnReset}
 */
static DECLCALLBACK(void) pciVfioReset(PPDMDEVINS pDevIns)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);

    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
    {
        PVFIOPCIFUN pFun = &pThis->aPciFuns[iPciFun];

        if (pFun->iFdVfio != -1)
        {
            PPDMPCIDEV pPciDev = pDevIns->apPciDevs[iPciFun];

            int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_RESET, NULL);
            AssertLogRelMsg(!rcLnx, ("VFIO#%d.%u: Failed to reset device %d\n", pThis->iInstance, pFun->uPciFun, errno));

            /*
             * The device comes out of the reset with MSI-X disabled and all vectors masked.
             * The PCI bus takes care of the command register and of the MSI capability,
             * the MSI-X state is ours.
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

            /* Bring the interrupt setup of the device in line with that. */
            pciVfioIrqUpdate(pThis, pFun, PDMPciDevGetCommand(pPciDev),
                             pciVfioGetMsiCtrl(pFun, pPciDev), pciVfioGetMsixCtrl(pFun, pPciDev));
        }
    }
}


/**
 * @interface_method_impl{PDMDEVREG,pfnDestruct}
 */
static DECLCALLBACK(int) pciVfioDestruct(PPDMDEVINS pDevIns)
{
    PVFIOPCI pThis = PDMDEVINS_2_DATA(pDevIns, PVFIOPCI);

    for (uint8_t iPciFun = 0; iPciFun < RT_ELEMENTS(pThis->aPciFuns); iPciFun++)
    {
        PVFIOPCIFUN pFun = &pThis->aPciFuns[iPciFun];

        /* Detach address space. */
        if (pFun->iFdVfio != -1)
        {
            struct vfio_device_detach_iommufd_pt VfioDetach;
            VfioDetach.argsz   = sizeof(VfioDetach);
            VfioDetach.flags   = 0;
            int rcLnx = ioctl(pFun->iFdVfio, VFIO_DEVICE_DETACH_IOMMUFD_PT, &VfioDetach);
            AssertLogRelMsg(!rcLnx, ("VFIO#%d.%u: Failed to detach IOMMU page table with %d\n", pThis->iInstance, pFun->uPciFun, errno));

            close(pFun->iFdVfio);
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
    }

    if (pThis->fVfioLegacy)
    {
        if (pThis->VfioGroup.iFdVfioContainer != -1)
            close(pThis->VfioGroup.iFdVfioContainer);
        if (pThis->VfioGroup.iFdVfioGroup != -1)
            close(pThis->VfioGroup.iFdVfioGroup);
    }
    else
    {
        if (pThis->IommuFd.iFdIommu != -1)
        {
            struct iommu_destroy HwptDestroy;
            HwptDestroy.size = sizeof(HwptDestroy);
            HwptDestroy.id   = pThis->IommuFd.idIommuHwpt;
            int rcLnx = ioctl(pThis->IommuFd.iFdIommu, IOMMU_DESTROY, &HwptDestroy);
            AssertLogRelMsg(!rcLnx, ("VFIO#%d: Failed to destroy I/O address space with %d\n", pThis->iInstance, errno));

            close(pThis->IommuFd.iFdIommu);
        }
    }

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

    for (uint8_t i = 0; i < RT_ELEMENTS(pThis->aPciFuns); i++)
    {
        PVFIOPCIFUN pFun = &pThis->aPciFuns[i];

        pFun->uPciFun        = UINT32_MAX;
        pFun->iFdVfio        = -1;
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

    /*
     * Validate configuration.
     */
    if (!pHlp->pfnCFGMAreValuesValid(pCfg,
                                     "IommuPath\0"
                                     "VfioPath\0"
                                     "VfioGroupPath\0"
                                     "Fun*\0"
                                    ))
        return VERR_PDM_DEVINS_UNKNOWN_CFG_VALUES;

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

        rc = pHlp->pfnCFGMQueryBoolDef(pCfg, "InterceptMmio", &pFun->fInterceptMmio, false);
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
            return PDMDevHlpVMSetError(pDevIns, RTErrConvertFromErrno(errno), RT_SRC_POS,
                                       N_("VFIO device is not a PCI/PCI Express device. This configuration is not supported"));

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
            rc = pciVfioLnxEventfd2(0 /*uValInit*/, 0 /*fFlags*/, &pFun->paFdEvts[i]);
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
        rc = pciVfioLnxEventfd2(0 /*uValInit*/, 0 /*fFlags*/, &pFun->iFdWakeup);
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

        /* Subsequent function should use the same major as the previous one. */
        iPciDevNo = PDMPCIDEVREG_DEV_NO_SAME_AS_PREV;
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
    /* .pfnInitComplete = */        NULL,
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

