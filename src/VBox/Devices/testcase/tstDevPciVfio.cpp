/* $Id$ */
/** @file
 * tstDevPciVfio - Testcase for the VFIO PCI passthrough device.
 */

/*
 * Copyright (C) 2026 popolupo
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

/** @page pg_tst_devpcivfio     tstDevPciVfio - Testcase for the VFIO PCI passthrough device
 *
 * Runs the real passthrough device (DevPciVfio.cpp) behind the real ICH9 PCI
 * bus (DevPciIch9.cpp) against a PCI device model served by a fake VFIO and
 * IOMMUFD kernel interface.  No hardware, no kernel support and no privileges
 * are required.
 *
 * See @ref pg_tst_devpcivfio_krnl and @ref pg_tst_devpcivfio_pdm for the two
 * halves of the environment.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <VBox/pci.h>
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/vmapi.h>
#include <VBox/err.h>
#include <iprt/assert.h>
#include <iprt/string.h>
#include <iprt/test.h>

#include <errno.h>

#include "VBoxDD.h"
#include "../Bus/DevPciVfio.h"
#include "tstDevPciVfio.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** Amount of guest RAM. */
#define TST_RAM_SIZE            _4M
/** The PCI device number the passthrough device is plugged into on bus 0. */
#define TST_PCI_DEV_NO          5
/** The device and function number of function 0 of the passthrough device. */
#define TST_PCI_DEVFN           (TST_PCI_DEV_NO << 3)
/** Vendor ID of the device model. */
#define TST_VENDOR_ID           UINT16_C(0x1234)
/** Device ID of the device model. */
#define TST_DEVICE_ID           UINT16_C(0x5678)
/** Size of BAR0 of the default device model. */
#define TST_BAR0_SIZE           _64K
/** Where the tests map BAR0. */
#define TST_BAR0_ADDR           UINT32_C(0xf0000000)
/** Size of the I/O port BAR of device models which have one. */
#define TST_IOBAR_SIZE          0x100
/** The first I/O port the tests assign to an I/O port BAR. */
#define TST_IOBAR_PORT          UINT16_C(0xd000)
/** Config space offset of the MSI capability of the default device model. */
#define TST_MSI_CAP_OFF         0x50
/** Config space offset of the PCI Express capability of the default device model. */
#define TST_EXP_CAP_OFF         0x70
/** Config space offset of the MSI-X capability of the device models which have one. */
#define TST_MSIX_CAP_OFF        0xb0
/** Where the tests map the BAR holding the MSI-X table. */
#define TST_MSIX_BAR_ADDR       UINT32_C(0xf1000000)
/** The node of the first function of the device model. */
#define TST_VFIO_NODE           "/dev/vfio/devices/vfio0"


/**
 * Stores a 16-bit value in the config space of a device model.
 *
 * @param   pModel              The device model.
 * @param   offReg              The register offset.
 * @param   u16Value            The value.
 */
static void tstModelSetU16(PTSTVFIODEVCFG pModel, uint32_t offReg, uint16_t u16Value)
{
    pModel->abCfg[offReg]     = RT_LO_U8(u16Value);
    pModel->abCfg[offReg + 1] = RT_HI_U8(u16Value);
}


/**
 * Initializes a device model with the standard header.
 *
 * @param   pModel              The device model.
 * @param   pszNode             The VFIO device node.
 * @param   bClassBase          The base class.
 * @param   bClassSub           The sub class.
 */
static void tstModelInit(PTSTVFIODEVCFG pModel, const char *pszNode, uint8_t bClassBase, uint8_t bClassSub)
{
    RT_ZERO(*pModel);
    RTStrCopy(pModel->szNode, sizeof(pModel->szNode), pszNode);
    pModel->cbCfg = 256;
    tstModelSetU16(pModel, VBOX_PCI_VENDOR_ID, TST_VENDOR_ID);
    tstModelSetU16(pModel, VBOX_PCI_DEVICE_ID, TST_DEVICE_ID);
    tstModelSetU16(pModel, VBOX_PCI_SUBSYSTEM_VENDOR_ID, TST_VENDOR_ID);
    tstModelSetU16(pModel, VBOX_PCI_SUBSYSTEM_ID, 0x0001);
    pModel->abCfg[VBOX_PCI_REVISION_ID]   = 0x01;
    pModel->abCfg[VBOX_PCI_CLASS_SUB]     = bClassSub;
    pModel->abCfg[VBOX_PCI_CLASS_BASE]    = bClassBase;
    pModel->abCfg[VBOX_PCI_INTERRUPT_PIN] = 0x01;
}


/**
 * Adds a BAR to a device model.
 *
 * @param   pModel              The device model.
 * @param   iBar                The BAR number.
 * @param   cbBar               The size of the BAR.
 * @param   fType               PCI_ADDRESS_SPACE_XXX; this is what the low bits of the BAR register read as.
 */
static void tstModelSetBar(PTSTVFIODEVCFG pModel, uint32_t iBar, uint64_t cbBar, uint32_t fType)
{
    pModel->aRegions[iBar].cb     = cbBar;
    pModel->aRegions[iBar].fFlags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
    if (!(fType & PCI_ADDRESS_SPACE_IO))
        pModel->aRegions[iBar].fFlags |= VFIO_REGION_INFO_FLAG_MMAP;
    pModel->abCfg[VBOX_PCI_BASE_ADDRESS_0 + iBar * 4] = (uint8_t)fType;
}


/**
 * Appends a capability to the capability list of a device model.
 *
 * @param   pModel              The device model.
 * @param   offCap              Where to place the capability.
 * @param   bCapId              The capability ID.
 * @param   pvBody              What follows the ID and the next pointer.
 * @param   cbBody              Size of the body.
 */
static void tstModelAddCap(PTSTVFIODEVCFG pModel, uint8_t offCap, uint8_t bCapId, const void *pvBody, size_t cbBody)
{
    AssertRelease(offCap >= 0x40 && offCap + 2 + cbBody <= 256);

    uint8_t *pbLink = &pModel->abCfg[VBOX_PCI_CAPABILITY_LIST];
    while (*pbLink)
        pbLink = &pModel->abCfg[*pbLink + 1];
    *pbLink = offCap;

    pModel->abCfg[offCap]     = bCapId;
    pModel->abCfg[offCap + 1] = 0;
    memcpy(&pModel->abCfg[offCap + 2], pvBody, cbBody);
    pModel->abCfg[VBOX_PCI_STATUS] |= VBOX_PCI_STATUS_CAP_LIST;
}


/**
 * Stores a 32-bit value in the config space of a device model.
 *
 * @param   pModel              The device model.
 * @param   offReg              The register offset.
 * @param   u32Value            The value.
 */
static void tstModelSetU32(PTSTVFIODEVCFG pModel, uint32_t offReg, uint32_t u32Value)
{
    pModel->abCfg[offReg]     = RT_BYTE1(u32Value);
    pModel->abCfg[offReg + 1] = RT_BYTE2(u32Value);
    pModel->abCfg[offReg + 2] = RT_BYTE3(u32Value);
    pModel->abCfg[offReg + 3] = RT_BYTE4(u32Value);
}


/**
 * Adds an MSI capability.
 *
 * @param   pModel              The device model.
 * @param   offCap              Where to place the capability.
 * @param   cVectorsLog2        Number of vectors the device supports as a power of two.
 * @param   f64Bit              Whether the capability has a 64-bit message address.
 * @param   fMaskable           Whether the capability has per-vector masking.
 */
static void tstModelAddMsiCap(PTSTVFIODEVCFG pModel, uint8_t offCap, uint8_t cVectorsLog2, bool f64Bit, bool fMaskable)
{
    uint8_t abBody[22];
    RT_ZERO(abBody);
    uint16_t const fMsgCtrl = (uint16_t)(  (f64Bit    ? VBOX_PCI_MSI_FLAGS_64BIT   : 0)
                                         | (fMaskable ? VBOX_PCI_MSI_FLAGS_MASKBIT : 0)
                                         | (cVectorsLog2 << 1));
    abBody[0] = RT_LO_U8(fMsgCtrl);
    abBody[1] = RT_HI_U8(fMsgCtrl);
    size_t const cbBody = 2 /* control */ + 4 /* address */ + (f64Bit ? 4 : 0) + 2 /* data */
                        + (fMaskable ? 2 /* reserved */ + 4 /* mask */ + 4 /* pending */ : 0);
    tstModelAddCap(pModel, offCap, VBOX_PCI_CAP_ID_MSI, abBody, cbBody);
    pModel->acIrqs[VFIO_PCI_MSI_IRQ_INDEX] = RT_BIT_32(cVectorsLog2);
}


/**
 * Adds an MSI-X capability and marks the BAR holding the table as such.
 *
 * @param   pModel              The device model.
 * @param   offCap              Where to place the capability.
 * @param   cVectors            The table size.
 * @param   iBar                The BAR holding both the table and the pending bit array.
 * @param   offTable            Offset of the table in the BAR.
 * @param   offPba              Offset of the pending bit array in the BAR.
 */
static void tstModelAddMsixCap(PTSTVFIODEVCFG pModel, uint8_t offCap, uint16_t cVectors, uint8_t iBar,
                               uint32_t offTable, uint32_t offPba)
{
    uint16_t const fMsgCtrl = (uint16_t)(cVectors - 1);
    uint32_t const u32Table = offTable | iBar;
    uint32_t const u32Pba   = offPba   | iBar;
    uint8_t const  abBody[10] =
    {
        RT_LO_U8(fMsgCtrl), RT_HI_U8(fMsgCtrl),
        RT_BYTE1(u32Table), RT_BYTE2(u32Table), RT_BYTE3(u32Table), RT_BYTE4(u32Table),
        RT_BYTE1(u32Pba),   RT_BYTE2(u32Pba),   RT_BYTE3(u32Pba),   RT_BYTE4(u32Pba)
    };
    tstModelAddCap(pModel, offCap, VBOX_PCI_CAP_ID_MSIX, abBody, sizeof(abBody));
    pModel->acIrqs[VFIO_PCI_MSIX_IRQ_INDEX] = cVectors;
    pModel->aRegions[iBar].fMsixMappable    = true;
}


/**
 * Adds a version 2 PCI Express capability for an endpoint.
 *
 * @param   pModel              The device model.
 * @param   offCap              Where to place the capability.
 */
static void tstModelAddExpCap(PTSTVFIODEVCFG pModel, uint8_t offCap)
{
    uint8_t abBody[58];
    RT_ZERO(abBody);
    abBody[0] = 0x02; /* Capability version 2, PCI Express endpoint. */
    tstModelAddCap(pModel, offCap, VBOX_PCI_CAP_ID_EXP, abBody, sizeof(abBody));
}


/**
 * Initializes the default device model: a network controller like single
 * function device with one 32-bit memory BAR, INTx, MSI with four vectors and
 * a PCI Express capability.
 *
 * @param   pModel              The device model.
 */
static void tstModelInitDefault(PTSTVFIODEVCFG pModel)
{
    tstModelInit(pModel, TST_VFIO_NODE, VBOX_PCI_CLASS_NETWORK, 0x00 /* Ethernet */);
    tstModelSetBar(pModel, 0, TST_BAR0_SIZE, PCI_ADDRESS_SPACE_MEM);
    pModel->acIrqs[VFIO_PCI_INTX_IRQ_INDEX] = 1;
    tstModelAddMsiCap(pModel, TST_MSI_CAP_OFF, 2 /*cVectorsLog2*/, true /*f64Bit*/, true /*fMaskable*/);
    tstModelAddExpCap(pModel, TST_EXP_CAP_OFF);
}


/**
 * Initializes a storage controller like device model without INTx and MSI,
 * with an MSI-X table and pending bit array in BAR2, which holds nothing else.
 *
 * @param   pModel              The device model.
 * @param   cVectors            The MSI-X table size.
 */
static void tstModelInitMsixDedicatedBar(PTSTVFIODEVCFG pModel, uint16_t cVectors)
{
    tstModelInit(pModel, TST_VFIO_NODE, 0x01 /* mass storage */, 0x08 /* non-volatile memory */);
    tstModelSetBar(pModel, 0, TST_BAR0_SIZE, PCI_ADDRESS_SPACE_MEM);
    tstModelSetBar(pModel, 2, _8K, PCI_ADDRESS_SPACE_MEM);
    tstModelAddExpCap(pModel, TST_EXP_CAP_OFF);
    tstModelAddMsixCap(pModel, TST_MSIX_CAP_OFF, cVectors, 2 /*iBar*/, 0 /*offTable*/, _4K /*offPba*/);
}


/** The main execution engine of the VMs set up by tstFixtureSetup(). */
static uint8_t  g_bTstExecEngine      = VM_EXEC_ENGINE_NATIVE_API;
/** Number of requests for mapping guest memory tstFixtureSetup() lets pass
 * before it makes one fail, UINT32_MAX for not making any fail. */
static uint32_t g_cTstFailIoasMapSkip = UINT32_MAX;
/** Whether tstFixtureSetup() configures function 0 to expose the legacy VGA ranges. */
static bool     g_fTstExposeVga       = false;
/** Whether tstFixtureSetup() configures function 0 to trap all memory BAR accesses. */
static bool     g_fTstInterceptMmio   = false;
/** The host address tstFixtureSetup() configures for function 0 instead of the
 * device node, NULL for configuring the device node. */
static const char *g_pszTstHostAddressFun0 = NULL;


/**
 * Sets up the fake kernel with the given PCI functions and a VM with the ICH9
 * PCI bus and one passthrough device using these functions.
 *
 * tstFixtureTeardown() has to be called regardless of the result.
 *
 * @returns Status code of the passthrough device constructor or of its init complete
 *          callback, or of whatever failed before.
 * @param   paModels            The PCI functions, the first one becomes function 0.
 * @param   cModels             Number of PCI functions.
 * @param   papDevs             Where to return the fake kernel handles of the functions.
 * @param   pszVfioPathFun0     The device node to configure for function 0, NULL to use the one of the model.
 */
static int tstFixtureSetup(PCTSTVFIODEVCFG paModels, uint32_t cModels, PTSTVFIODEV *papDevs, const char *pszVfioPathFun0)
{
    int rc = tstVfioKrnlInit();
    RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return rc;
    if (g_cTstFailIoasMapSkip != UINT32_MAX)
        tstVfioKrnlFailIoctl(IOMMU_IOAS_MAP, g_cTstFailIoasMapSkip, ENOMEM);
    for (uint32_t i = 0; i < cModels; i++)
    {
        rc = tstVfioKrnlDevAdd(&paModels[i], &papDevs[i]);
        RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
        if (RT_FAILURE(rc))
            return rc;
    }

    rc = tstPdmInit(TST_RAM_SIZE);
    RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return rc;
    tstPdmSetExecutionEngine(g_bTstExecEngine);

    /* The root bus with an ECAM window for config space accesses. */
    PCFGMNODE pCfgInst = tstCfgNodeCreate(NULL, "0");
    PCFGMNODE pCfg     = tstCfgNodeCreate(pCfgInst, "Config");
    RTTESTI_CHECK_RC(tstCfgSetInteger(pCfg, "IOAPIC", 1), VINF_SUCCESS);
    RTTESTI_CHECK_RC(tstCfgSetInteger(pCfg, "McfgBase", TSTPDM_MCFG_BASE), VINF_SUCCESS);
    RTTESTI_CHECK_RC(tstCfgSetInteger(pCfg, "McfgLength", TSTPDM_MCFG_SIZE), VINF_SUCCESS);
    rc = tstPdmDevCreate(&g_DevicePciIch9, 0 /*iInstance*/, pCfgInst, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("ich9pci: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_FAILURE(rc))
        return rc;

    /* The passthrough device. */
    pCfgInst = tstCfgNodeCreate(NULL, "0");
    RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgInst, "PCIBusNo", 0), VINF_SUCCESS);
    RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgInst, "PCIDeviceNo", TST_PCI_DEV_NO), VINF_SUCCESS);
    RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgInst, "PCIFunctionNo", 0), VINF_SUCCESS);
    pCfg = tstCfgNodeCreate(pCfgInst, "Config");
    for (uint32_t i = 0; i < cModels; i++)
    {
        char szName[16];
        RTStrPrintf(szName, sizeof(szName), "Fun%u", i);
        PCFGMNODE pCfgFun = tstCfgNodeCreate(pCfg, szName);
        const char *pszVfioPath = i == 0 && pszVfioPathFun0 ? pszVfioPathFun0 : paModels[i].szNode;
        if (i == 0 && g_pszTstHostAddressFun0)
            RTTESTI_CHECK_RC(tstCfgSetString(pCfgFun, "HostAddress", g_pszTstHostAddressFun0), VINF_SUCCESS);
        else
            RTTESTI_CHECK_RC(tstCfgSetString(pCfgFun, "VfioPath", pszVfioPath), VINF_SUCCESS);
        if (i == 0 && g_fTstExposeVga)
            RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgFun, "ExposeVga", 1), VINF_SUCCESS);
        if (i == 0 && g_fTstInterceptMmio)
            RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgFun, "InterceptMmio", 1), VINF_SUCCESS);
    }
    rc = tstPdmDevCreate(&g_DevicePciVfio, 0 /*iInstance*/, pCfgInst, NULL);
    if (RT_SUCCESS(rc))
        rc = tstPdmInitComplete();
    return rc;
}


/**
 * Destroys the VM and the fake kernel, checking that the passthrough device
 * has released everything it took from the kernel.
 */
static void tstFixtureTeardown(void)
{
    tstPdmTerm();
    RTTESTI_CHECK_MSG(tstVfioKrnlGetOpenFdCount() == 0,
                      ("%u file descriptor(s) still open after the VM is gone\n", tstVfioKrnlGetOpenFdCount()));
    RTTESTI_CHECK_MSG(tstVfioKrnlGetDmaMappedBytes() == 0,
                      ("%RU64 bytes still mapped for DMA after the VM is gone\n", tstVfioKrnlGetDmaMappedBytes()));
    tstVfioKrnlTerm();
}


/**
 * Construction, config space, BAR mapping, DMA mapping and destruction.
 */
static void tstBasics(void)
{
    RTTestISub("Construct, config space, BAR, DMA map, destruct");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* The guest sees the identity of the physical device, and nothing in the neighbouring function. */
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_VENDOR_ID, 2) == TST_VENDOR_ID);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_DEVICE_ID, 2) == TST_DEVICE_ID);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_CLASS_BASE, 1) == VBOX_PCI_CLASS_NETWORK);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN + 1, VBOX_PCI_VENDOR_ID, 2) == UINT16_MAX);

        /* The device is open exactly once. */
        RTTESTI_CHECK(tstVfioKrnlDevGetOpenCount(pDev) == 1);

        /* All guest RAM is available to the device at guest physical addresses, and nothing else is. */
        uint32_t const offRamHoleEnd = TSTPDM_RAM_HOLE_START + TSTPDM_RAM_HOLE_SIZE;
        RTTESTI_CHECK(tstVfioKrnlIsIovaRangeMapped(0, TSTPDM_RAM_HOLE_START));
        RTTESTI_CHECK(tstVfioKrnlIsIovaRangeMapped(offRamHoleEnd, TST_RAM_SIZE - offRamHoleEnd));
        RTTESTI_CHECK_MSG(tstVfioKrnlGetDmaMappedBytes() == TST_RAM_SIZE - TSTPDM_RAM_HOLE_SIZE,
                          ("%RU64 bytes mapped for DMA\n", tstVfioKrnlGetDmaMappedBytes()));

        /* Size BAR0, assign an address and enable memory decoding. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, UINT32_MAX, 4);
        uint32_t u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, 4);
        RTTESTI_CHECK_MSG(u32 == ~(uint32_t)(TST_BAR0_SIZE - 1), ("BAR0 sizing returned %#RX32\n", u32));
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);

        /* The guest reads what the device has in BAR0 and the device gets what the guest writes. */
        uint32_t *pau32Bar0 = (uint32_t *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_BAR0_REGION_INDEX);
        RTTESTI_CHECK(pau32Bar0 != NULL);
        if (pau32Bar0)
        {
            pau32Bar0[4] = UINT32_C(0xfeedface);
            u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + 16, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(u32 == UINT32_C(0xfeedface), ("read %#RX32 from BAR0\n", u32));

            u32 = UINT32_C(0x12345678);
            RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_BAR0_ADDR + 32, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(pau32Bar0[8] == UINT32_C(0x12345678), ("device sees %#RX32 in BAR0\n", pau32Bar0[8]));
        }

        /* The guest can enable bus mastering, which reaches the device. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, 2);
        RTTESTI_CHECK_MSG(u32 & VBOX_PCI_COMMAND_MASTER, ("command register %#RX32\n", u32));
        uint8_t const *pbCfg = (uint8_t const *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_CONFIG_REGION_INDEX);
        RTTESTI_CHECK(pbCfg != NULL);
        if (pbCfg)
            RTTESTI_CHECK_MSG(pbCfg[VBOX_PCI_COMMAND] & VBOX_PCI_COMMAND_MASTER,
                              ("command register of the device %#x\n", pbCfg[VBOX_PCI_COMMAND]));
    }
    tstFixtureTeardown();
}


/**
 * MSI: the guest enables it and an interrupt of the physical device reaches
 * the interrupt controller with the address and data the guest programmed.
 */
static void tstMsi(void)
{
    RTTestISub("MSI delivery");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* The capability is presented to the guest where the physical device has it. */
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_CAPABILITY_LIST, 1) == TST_MSI_CAP_OFF);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF, 1) == VBOX_PCI_CAP_ID_MSI);

        /* Nothing can be delivered while MSI is disabled. */
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 0);

        /* Program address and data, then enable MSI with a single vector. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 4,  UINT32_C(0xfee00000), 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 8,  0, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 12, 0x4041, 2);
        uint32_t const fMsgCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2,
                          (fMsgCtrl & ~(uint32_t)VBOX_PCI_MSI_FLAGS_QSIZE) | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
        RTTESTI_CHECK_MSG(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 1,
                          ("%u MSI vector(s) enabled on the device\n", tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX)));

        /* Let the device raise the interrupt. */
        RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSI_IRQ_INDEX, 0), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(tstPdmMsiWait(1, RT_MS_5SEC), ("the MSI did not arrive\n"));
        TSTPDMMSI Msi;
        if (tstPdmMsiGet(0, &Msi))
        {
            RTTESTI_CHECK_MSG(Msi.uAddr == UINT64_C(0xfee00000), ("address %#RX64\n", Msi.uAddr));
            RTTESTI_CHECK_MSG(Msi.uData == 0x4041, ("data %#RX32\n", Msi.uData));
            RTTESTI_CHECK_MSG(Msi.uBusDevFn == PCIBDF_MAKE(0, TST_PCI_DEVFN), ("sender %#RX16\n", Msi.uBusDevFn));
        }
    }
    tstFixtureTeardown();
}


/**
 * The device needs the guest memory to stay where it is, so it refuses to be
 * part of a VM using an execution engine where that isn't the case.
 */
static void tstExecEngine(void)
{
    RTTestISub("Execution engines without fixed guest memory");

    static const uint8_t s_abEngines[] = { VM_EXEC_ENGINE_HW_VIRT, VM_EXEC_ENGINE_IEM };
    for (uint32_t i = 0; i < RT_ELEMENTS(s_abEngines); i++)
    {
        TSTVFIODEVCFG Model;
        tstModelInitDefault(&Model);
        PTSTVFIODEV pDev = NULL;
        g_bTstExecEngine = s_abEngines[i];
        int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
        g_bTstExecEngine = VM_EXEC_ENGINE_NATIVE_API;
        RTTESTI_CHECK_MSG(RT_FAILURE(rc), ("engine %u: the VM construction succeeded: %Rrc\n", s_abEngines[i], rc));
        RTTESTI_CHECK_MSG(*tstPdmGetLastError() != '\0', ("engine %u: no VM error was set\n", s_abEngines[i]));
        if (pDev)
            RTTESTI_CHECK_MSG(tstVfioKrnlDevGetOpenCount(pDev) == 0, ("engine %u: the device was opened\n", s_abEngines[i]));
        RTTESTI_CHECK(tstVfioKrnlGetDmaMappedBytes() == 0);
        tstFixtureTeardown();
    }
}


/**
 * The device gets all of the guest memory or the VM doesn't start: nothing is
 * left mapped when a part can't be mapped, and bus mastering can't be enabled
 * without the memory.
 */
static void tstDmaMapFailure(void)
{
    RTTestISub("Guest memory can't be made available to the device");

    /* The guest RAM of the VM is in two pieces, so there are two requests to fail. */
    for (uint32_t iFail = 0; iFail < 2; iFail++)
    {
        TSTVFIODEVCFG Model;
        tstModelInitDefault(&Model);
        PTSTVFIODEV pDev = NULL;
        g_cTstFailIoasMapSkip = iFail;
        int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
        g_cTstFailIoasMapSkip = UINT32_MAX;
        RTTESTI_CHECK_MSG(RT_FAILURE(rc), ("request %u failing: the VM construction succeeded: %Rrc\n", iFail, rc));
        RTTESTI_CHECK_MSG(*tstPdmGetLastError() != '\0', ("request %u failing: no VM error was set\n", iFail));
        RTTESTI_CHECK_MSG(tstVfioKrnlGetDmaMappedBytes() == 0,
                          ("request %u failing: %RU64 bytes left mapped for DMA\n", iFail, tstVfioKrnlGetDmaMappedBytes()));

        /*
         * Such a VM never runs.  If it did, the guest could not make the device do DMA:
         * the bus master bit does not stick and does not reach the device, and there is
         * no such thing as a part of the guest memory being mapped.
         */
        if (pDev && tstVfioKrnlDevGetOpenCount(pDev) == 1)
        {
            tstPdmPowerOn();
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
            uint64_t const cbMapped = tstVfioKrnlGetDmaMappedBytes();
            RTTESTI_CHECK_MSG(cbMapped == 0, ("request %u failing: %RU64 bytes mapped for DMA\n", iFail, cbMapped));
            uint32_t const u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, 2);
            RTTESTI_CHECK_MSG((u32 & (VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER)) == VBOX_PCI_COMMAND_MEMORY,
                              ("request %u failing: command register %#RX32\n", iFail, u32));
            uint8_t const *pbCfg = (uint8_t const *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_CONFIG_REGION_INDEX);
            RTTESTI_CHECK(pbCfg != NULL);
            if (pbCfg)
                RTTESTI_CHECK_MSG(!(pbCfg[VBOX_PCI_COMMAND] & VBOX_PCI_COMMAND_MASTER),
                                  ("request %u failing: command register of the device %#x\n", iFail, pbCfg[VBOX_PCI_COMMAND]));
        }
        else
            RTTestIFailed("request %u failing: the device is not there to check bus mastering", iFail);
        tstFixtureTeardown();
    }
}


/**
 * An I/O port BAR, and what the guest gets from a device which stops
 * answering: all ones when reading, and no errors.
 */
static void tstIoBarAndSilentDevice(void)
{
    RTTestISub("I/O port BAR, device which stops answering");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    tstModelSetBar(&Model, 1, TST_IOBAR_SIZE, PCI_ADDRESS_SPACE_IO);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* Assign the ports and enable I/O port decoding. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_1, TST_IOBAR_PORT, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_IO, 2);

        uint8_t *pbIoBar = (uint8_t *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_BAR1_REGION_INDEX);
        RTTESTI_CHECK(pbIoBar != NULL);
        if (pbIoBar)
        {
            /* The guest reads what the device has at its ports and the device gets what the guest writes. */
            static const uint8_t s_abWritten[4] = { 0x44, 0x33, 0x22, 0x11 };
            pbIoBar[0x10] = 0x5a;
            uint32_t u32 = 0;
            RTTESTI_CHECK_RC(tstPdmIoPortRead(TST_IOBAR_PORT + 0x10, &u32, 1), VINF_SUCCESS);
            RTTESTI_CHECK_MSG((u32 & 0xff) == 0x5a, ("read %#RX32 from the port\n", u32));
            RTTESTI_CHECK_RC(tstPdmIoPortWrite(TST_IOBAR_PORT + 0x20, UINT32_C(0x11223344), 4), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(!memcmp(&pbIoBar[0x20], s_abWritten, sizeof(s_abWritten)),
                              ("the device sees %.4Rhxs at the port\n", &pbIoBar[0x20]));

            /* The device stops answering: that is nothing the guest gets an error for. */
            tstVfioKrnlDevSetGone(pDev, true);
            u32 = 0;
            RTTESTI_CHECK_RC(tstPdmIoPortRead(TST_IOBAR_PORT + 0x10, &u32, 2), VINF_SUCCESS);
            RTTESTI_CHECK_MSG((u32 & 0xffff) == 0xffff, ("read %#RX32 from the port of the silent device\n", u32));
            RTTESTI_CHECK_RC(tstPdmIoPortWrite(TST_IOBAR_PORT + 0x20, 0, 4), VINF_SUCCESS);
            for (uint32_t off = 0; off < 256; off += 4)
                tstPdmPciCfgRead(0, TST_PCI_DEVFN, off, 4);
            tstVfioKrnlDevSetGone(pDev, false);
            RTTESTI_CHECK_MSG(!memcmp(&pbIoBar[0x20], s_abWritten, sizeof(s_abWritten)),
                              ("the silent device got written to: %.4Rhxs\n", &pbIoBar[0x20]));
        }
    }
    tstFixtureTeardown();
}


/**
 * The legacy VGA ranges of a graphics device: I/O ports, video memory, and a
 * memory fill which doesn't end where the video memory ends.
 */
static void tstVga(void)
{
    RTTestISub("Legacy VGA ranges of a graphics device");

    TSTVFIODEVCFG Model;
    tstModelInit(&Model, TST_VFIO_NODE, VBOX_PCI_CLASS_DISPLAY, VBOX_PCI_SUB_DISPLAY_VGA);
    tstModelSetBar(&Model, 0, TST_BAR0_SIZE, PCI_ADDRESS_SPACE_MEM);
    Model.acIrqs[VFIO_PCI_INTX_IRQ_INDEX] = 1;
    tstModelAddMsiCap(&Model, TST_MSI_CAP_OFF, 0 /*cVectorsLog2*/, true /*f64Bit*/, false /*fMaskable*/);
    tstModelAddExpCap(&Model, TST_EXP_CAP_OFF);
    /* The region is bigger than what the legacy ranges need, so that it shows when something goes beyond them. */
    Model.aRegions[VFIO_PCI_VGA_REGION_INDEX].cb     = _1M;
    Model.aRegions[VFIO_PCI_VGA_REGION_INDEX].fFlags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;

    PTSTVFIODEV pDev = NULL;
    g_fTstExposeVga = true;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    g_fTstExposeVga = false;
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        uint8_t *pbVga = (uint8_t *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_VGA_REGION_INDEX);
        RTTESTI_CHECK(pbVga != NULL);
        if (pbVga)
        {
            /* I/O ports: the port number is the offset into the VGA region. */
            pbVga[0x3c5] = 0xa5;
            uint32_t u32 = 0;
            RTTESTI_CHECK_RC(tstPdmIoPortRead(0x3c5, &u32, 1), VINF_SUCCESS);
            RTTESTI_CHECK_MSG((u32 & 0xff) == 0xa5, ("read %#RX32 from port 0x3c5\n", u32));
            RTTESTI_CHECK_RC(tstPdmIoPortWrite(0x3d4, 0x1234, 2), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(pbVga[0x3d4] == 0x34 && pbVga[0x3d5] == 0x12, ("the device sees %.2Rhxs at port 0x3d4\n", &pbVga[0x3d4]));

            /* Video memory: the guest physical address is the offset into the VGA region. */
            pbVga[0xb8000] = 0x41;
            uint8_t b = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(0xb8000, &b, 1), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(b == 0x41, ("read %#x from the video memory\n", b));
            static const uint8_t s_abWritten[4] = { 0x41, 0x0f, 0x42, 0x0f };
            RTTESTI_CHECK_RC(tstPdmPhysWrite(0xa0010, s_abWritten, sizeof(s_abWritten)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(!memcmp(&pbVga[0xa0010], s_abWritten, sizeof(s_abWritten)),
                              ("the device sees %.4Rhxs in the video memory\n", &pbVga[0xa0010]));

            /* A fill starting 16 bytes before the end of the video memory with 256 bytes to go ends with the video memory. */
            RTTESTI_CHECK_RC(tstPdmPhysFill(0xbfff0, UINT32_C(0xc3c2c1c0), 4, 64), VINF_SUCCESS);
            bool fFilled = true;
            for (uint32_t i = 0; i < 16; i++)
                if (pbVga[0xbfff0 + i] != 0xc0 + (i & 3))
                    fFilled = false;
            RTTESTI_CHECK_MSG(fFilled, ("the end of the video memory after the fill: %.16Rhxs\n", &pbVga[0xbfff0]));
            bool fBeyond = false;
            for (uint32_t i = 0; i < 256; i++)
                if (pbVga[0xc0000 + i] != 0)
                    fBeyond = true;
            RTTESTI_CHECK_MSG(!fBeyond, ("the fill went beyond the video memory: %.16Rhxs\n", &pbVga[0xc0000]));

            /* The device stops answering. */
            tstVfioKrnlDevSetGone(pDev, true);
            u32 = 0;
            RTTESTI_CHECK_RC(tstPdmIoPortRead(0x3c5, &u32, 1), VINF_SUCCESS);
            RTTESTI_CHECK_MSG((u32 & 0xff) == 0xff, ("read %#RX32 from port 0x3c5 of the silent device\n", u32));
            RTTESTI_CHECK_RC(tstPdmIoPortWrite(0x3d4, 0, 2), VINF_SUCCESS);
            b = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(0xb8000, &b, 1), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(b == 0xff, ("read %#x from the video memory of the silent device\n", b));
            RTTESTI_CHECK_RC(tstPdmPhysWrite(0xa0010, &b, 1), VINF_SUCCESS);
            RTTESTI_CHECK_RC(tstPdmPhysFill(0xa0000, 0, 1, 16), VINF_SUCCESS);
            tstVfioKrnlDevSetGone(pDev, false);
        }
    }
    tstFixtureTeardown();
}


/**
 * A device with a quirk making a config space range readable for the guest:
 * the range is passed through where the device has nothing else, and a
 * capability located there stays what it is.
 */
static void tstQuirkConfigRange(void)
{
    RTTestISub("Device quirk for a config space range, with and without a capability in it");

    /* The range is 0x50 to 0x5f, read-only for the guest. */
    for (uint32_t iVariant = 0; iVariant < 2; iVariant++)
    {
        bool const    fCapInRange = iVariant == 1;
        TSTVFIODEVCFG Model;
        tstModelInit(&Model, TST_VFIO_NODE, VBOX_PCI_CLASS_DISPLAY, 0x80 /* other */);
        tstModelSetU16(&Model, VBOX_PCI_VENDOR_ID, 0x8086);
        tstModelSetU16(&Model, VBOX_PCI_DEVICE_ID, 0xe223);
        tstModelSetBar(&Model, 0, TST_BAR0_SIZE, PCI_ADDRESS_SPACE_MEM);
        Model.acIrqs[VFIO_PCI_INTX_IRQ_INDEX] = 1;
        uint8_t const offMsiCap = fCapInRange ? 0x50 : 0x60;
        tstModelAddMsiCap(&Model, offMsiCap, 0 /*cVectorsLog2*/, false /*f64Bit*/, false /*fMaskable*/);
        tstModelAddExpCap(&Model, TST_EXP_CAP_OFF);
        if (!fCapInRange)
            Model.abCfg[0x52] = 0x77;

        PTSTVFIODEV pDev = NULL;
        int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
        RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("variant %u: pci-vfio: %Rrc - %s\n", iVariant, rc, tstPdmGetLastError()));
        if (RT_SUCCESS(rc))
        {
            tstPdmPowerOn();

            /* Reading through the config space works. */
            for (uint32_t off = 0; off < 256; off++)
                tstPdmPciCfgRead(0, TST_PCI_DEVFN, off, 1);

            uint8_t const *pbCfg = (uint8_t const *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_CONFIG_REGION_INDEX);
            RTTESTI_CHECK(pbCfg != NULL);
            if (!fCapInRange)
            {
                /* The guest reads what the device has there and can't change it. */
                uint32_t u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, 0x52, 1);
                RTTESTI_CHECK_MSG(u32 == 0x77, ("read %#RX32 from the range\n", u32));
                tstPdmPciCfgWrite(0, TST_PCI_DEVFN, 0x52, 0x11, 1);
                if (pbCfg)
                    RTTESTI_CHECK_MSG(pbCfg[0x52] == 0x77, ("the device has %#x in the range after a guest write\n", pbCfg[0x52]));
            }

            /* MSI works wherever the capability is. */
            RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, offMsiCap, 1) == VBOX_PCI_CAP_ID_MSI);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, offMsiCap + 4, UINT32_C(0xfee00000), 4);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, offMsiCap + 8, 0x4045, 2);
            uint32_t const fMsgCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, offMsiCap + 2, 2);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, offMsiCap + 2, fMsgCtrl | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
            RTTESTI_CHECK_MSG(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 1,
                              ("variant %u: %u MSI vector(s) enabled on the device\n", iVariant,
                               tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX)));
            uint32_t const cMsisBefore = tstPdmMsiGetCount();
            RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSI_IRQ_INDEX, 0), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("variant %u: the MSI did not arrive\n", iVariant));
            TSTPDMMSI Msi;
            if (tstPdmMsiGet(cMsisBefore, &Msi))
                RTTESTI_CHECK_MSG(Msi.uData == 0x4045, ("variant %u: data %#RX32\n", iVariant, Msi.uData));
        }
        tstFixtureTeardown();
    }
}


/**
 * A memory BAR which VFIO allows to read and to map but not to write: the
 * guest can read it and what it writes is dropped.
 */
static void tstReadOnlyBar(void)
{
    RTTestISub("BAR which can't be written");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    tstModelSetBar(&Model, 2, _64K, PCI_ADDRESS_SPACE_MEM);
    Model.aRegions[2].fFlags &= ~(uint32_t)VFIO_REGION_INFO_FLAG_WRITE;
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_2, TST_MSIX_BAR_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);

        uint32_t *pau32Bar2 = (uint32_t *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_BAR2_REGION_INDEX);
        RTTESTI_CHECK(pau32Bar2 != NULL);
        if (pau32Bar2)
        {
            pau32Bar2[1] = UINT32_C(0xcafe0001);
            uint32_t u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(TST_MSIX_BAR_ADDR + 4, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(u32 == UINT32_C(0xcafe0001), ("read %#RX32 from the BAR\n", u32));

            u32 = UINT32_C(0x55aa55aa);
            RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_MSIX_BAR_ADDR + 8, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(pau32Bar2[2] == 0, ("the device sees %#RX32 in the BAR which can't be written\n", pau32Bar2[2]));
        }
    }
    tstFixtureTeardown();
}


/**
 * None of the file descriptors the device uses are handed on to processes
 * started by the VM process.
 */
static void tstFdsNotInherited(void)
{
    RTTestISub("File descriptors are not inherited by child processes");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* With MSI enabled the kernel has the eventfds of all four vectors. */
        uint32_t const fMsgCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsgCtrl | (2 << 4) | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 4);

        RTTESTI_CHECK_MSG(tstVfioKrnlGetInheritableFdCount() == 0,
                          ("%u file descriptor(s) would be inherited\n", tstVfioKrnlGetInheritableFdCount()));
    }
    tstFixtureTeardown();
}


/**
 * The per function option for trapping all memory BAR accesses: the guest
 * gets no direct mapping and the BAR works all the same.
 */
static void tstInterceptMmio(void)
{
    RTTestISub("Memory BAR with all accesses trapped on request");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    g_fTstInterceptMmio = true;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    g_fTstInterceptMmio = false;
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);
        RTTESTI_CHECK_MSG(!tstPdmIsMmio2Mapped(TST_BAR0_ADDR), ("the BAR is mapped into the guest\n"));

        uint32_t *pau32Bar0 = (uint32_t *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_BAR0_REGION_INDEX);
        RTTESTI_CHECK(pau32Bar0 != NULL);
        if (pau32Bar0)
        {
            pau32Bar0[4] = UINT32_C(0xfeedface);
            uint32_t u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + 16, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(u32 == UINT32_C(0xfeedface), ("read %#RX32 from BAR0\n", u32));

            u32 = UINT32_C(0x12345678);
            RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_BAR0_ADDR + 32, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(pau32Bar0[8] == UINT32_C(0x12345678), ("device sees %#RX32 in BAR0\n", pau32Bar0[8]));
        }
    }
    tstFixtureTeardown();
}


/**
 * Devices which must be refused: one without an IOMMU behind it, and one
 * which VFIO doesn't report as a PCI device.
 */
static void tstUnsuitableDevices(void)
{
    RTTestISub("Device without IOMMU, device which is not PCI");

    for (uint32_t iVariant = 0; iVariant < 2; iVariant++)
    {
        TSTVFIODEVCFG Model;
        tstModelInitDefault(&Model);
        Model.fNoIommu = iVariant == 0;
        Model.fNotPci  = iVariant == 1;
        PTSTVFIODEV pDev = NULL;
        int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
        RTTESTI_CHECK_MSG(RT_FAILURE(rc), ("variant %u: the VM construction succeeded: %Rrc\n", iVariant, rc));
        RTTESTI_CHECK_MSG(*tstPdmGetLastError() != '\0', ("variant %u: no VM error was set\n", iVariant));
        RTTESTI_CHECK_MSG(tstVfioKrnlGetDmaMappedBytes() == 0,
                          ("variant %u: %RU64 bytes mapped for DMA\n", iVariant, tstVfioKrnlGetDmaMappedBytes()));
        tstFixtureTeardown();
    }
}


/**
 * The host address of a function ends up in paths, so only a PCI address is
 * accepted for it.
 */
static void tstHostAddress(void)
{
    RTTestISub("Host address which is not a PCI address");

    static const char * const s_apszAddresses[] =
    {
        "",
        "0000:0a:00",
        "0000:0a:00.8",
        "0000:0a:00.1x",
        "000g:0a:00.1",
        "0000-0a-00.1",
        "../../../../tmp",
        "0000:0a:00.1/../../../../../tmp",
        "/dev/vfio/devices/vfio0"
    };
    for (uint32_t i = 0; i < RT_ELEMENTS(s_apszAddresses); i++)
    {
        TSTVFIODEVCFG Model;
        tstModelInitDefault(&Model);
        PTSTVFIODEV pDev = NULL;
        g_pszTstHostAddressFun0 = s_apszAddresses[i];
        int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
        g_pszTstHostAddressFun0 = NULL;
        RTTESTI_CHECK_MSG(rc == VERR_INVALID_PARAMETER, ("'%s': %Rrc - %s\n", s_apszAddresses[i], rc, tstPdmGetLastError()));
        if (pDev)
            RTTESTI_CHECK_MSG(tstVfioKrnlDevGetOpenCount(pDev) == 0, ("'%s': the device was opened\n", s_apszAddresses[i]));
        tstFixtureTeardown();
    }
}


/** Number of times the device stopped decoding memory while the guest still had
 * BAR0 mapped, counted by tstCmdRegOrderHook(). */
static uint32_t g_cTstDecodeOffWhileMapped = 0;

/**
 * @callback_method_impl{FNTSTVFIOCFGWRITE, Checks the state of the guest
 *      mapping of BAR0 whenever the command register of the device is written.}
 */
static DECLCALLBACK(void) tstCmdRegOrderHook(PTSTVFIODEV pDev, uint32_t offReg, size_t cb, void *pvUser)
{
    RT_NOREF(pvUser);
    if (   offReg <= VBOX_PCI_COMMAND
        && offReg + cb > VBOX_PCI_COMMAND)
    {
        uint8_t const *pbCfg = (uint8_t const *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_CONFIG_REGION_INDEX);
        if (   pbCfg
            && !(pbCfg[VBOX_PCI_COMMAND] & VBOX_PCI_COMMAND_MEMORY)
            && tstPdmIsMmio2Mapped(TST_BAR0_ADDR))
            g_cTstDecodeOffWhileMapped++;
    }
}


/**
 * The host kernel makes the mapping of a BAR unusable while the device doesn't
 * decode memory, so the guest has to have lost its mapping by the time the
 * device is told to stop decoding.
 */
static void tstCmdRegOrder(void)
{
    RTTestISub("Guest mappings are gone before the device stops decoding");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();
        g_cTstDecodeOffWhileMapped = 0;
        tstVfioKrnlSetCfgWriteHook(tstCmdRegOrderHook, NULL);

        /* Enable and disable memory decoding with a word write. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);
        RTTESTI_CHECK(tstPdmIsMmio2Mapped(TST_BAR0_ADDR));
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, 0, 2);
        RTTESTI_CHECK(!tstPdmIsMmio2Mapped(TST_BAR0_ADDR));
        RTTESTI_CHECK_MSG(g_cTstDecodeOffWhileMapped == 0,
                          ("word write: the device stopped decoding %u time(s) with the BAR mapped\n", g_cTstDecodeOffWhileMapped));

        /* The same with a byte write, and with other bits changing along. */
        g_cTstDecodeOffWhileMapped = 0;
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 1);
        RTTESTI_CHECK(tstPdmIsMmio2Mapped(TST_BAR0_ADDR));
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MASTER, 1);
        RTTESTI_CHECK(!tstPdmIsMmio2Mapped(TST_BAR0_ADDR));
        RTTESTI_CHECK_MSG(g_cTstDecodeOffWhileMapped == 0,
                          ("byte write: the device stopped decoding %u time(s) with the BAR mapped\n", g_cTstDecodeOffWhileMapped));

        /* A reset of the VM with the BAR mapped. */
        g_cTstDecodeOffWhileMapped = 0;
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
        RTTESTI_CHECK(tstPdmIsMmio2Mapped(TST_BAR0_ADDR));
        tstPdmReset();
        RTTESTI_CHECK(!tstPdmIsMmio2Mapped(TST_BAR0_ADDR));
        RTTESTI_CHECK_MSG(g_cTstDecodeOffWhileMapped == 0,
                          ("reset: the device stopped decoding %u time(s) with the BAR mapped\n", g_cTstDecodeOffWhileMapped));

        tstVfioKrnlSetCfgWriteHook(NULL, NULL);
    }
    tstFixtureTeardown();
}


/**
 * A device with two functions which can't be reset on their own, like many
 * graphics cards: a reset of the VM resets the bus they are on.  If that isn't
 * possible either the functions are left disabled.
 */
static void tstResetFallback(void)
{
    RTTestISub("Reset of functions which can't be reset on their own");

    for (uint32_t iVariant = 0; iVariant < 2; iVariant++)
    {
        bool const    fBusResetWorks = iVariant == 0;
        uint16_t const fCmdActive    = VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER;
        TSTVFIODEVCFG aModels[2];
        tstModelInit(&aModels[0], "/dev/vfio/devices/vfio0", 0x03 /* display */, 0x80 /* other */);
        tstModelInit(&aModels[1], "/dev/vfio/devices/vfio1", 0x04 /* multimedia */, 0x03 /* audio device */);
        tstModelSetU16(&aModels[1], VBOX_PCI_DEVICE_ID, TST_DEVICE_ID + 1);
        for (uint32_t i = 0; i < RT_ELEMENTS(aModels); i++)
        {
            tstModelSetBar(&aModels[i], 0, _64K, PCI_ADDRESS_SPACE_MEM);
            aModels[i].acIrqs[VFIO_PCI_INTX_IRQ_INDEX] = 1;
            tstModelAddExpCap(&aModels[i], TST_EXP_CAP_OFF);
            aModels[i].fNoFunctionReset = true;
            aModels[i].fNoBusReset      = !fBusResetWorks;
        }

        PTSTVFIODEV apDevs[2] = { NULL, NULL };
        int rc = tstFixtureSetup(aModels, RT_ELEMENTS(aModels), apDevs, NULL);
        RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("variant %u: pci-vfio: %Rrc - %s\n", iVariant, rc, tstPdmGetLastError()));
        if (RT_SUCCESS(rc))
        {
            tstPdmPowerOn();

            /* The guest sets both functions up. */
            for (uint8_t i = 0; i < RT_ELEMENTS(aModels); i++)
            {
                tstPdmPciCfgWrite(0, TST_PCI_DEVFN + i, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR + i * _1M, 4);
                tstPdmPciCfgWrite(0, TST_PCI_DEVFN + i, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
                RTTESTI_CHECK(tstPdmIsMmio2Mapped(TST_BAR0_ADDR + i * _1M));
            }

            tstPdmReset();

            for (uint8_t i = 0; i < RT_ELEMENTS(aModels); i++)
            {
                RTTESTI_CHECK_MSG(tstVfioKrnlDevGetResetCount(apDevs[i]) == 0,
                                  ("variant %u, function %u: %u function reset(s)\n", iVariant, i,
                                   tstVfioKrnlDevGetResetCount(apDevs[i])));
                RTTESTI_CHECK_MSG(tstVfioKrnlDevGetBusResetCount(apDevs[i]) == (fBusResetWorks ? 1U : 0U),
                                  ("variant %u, function %u: %u bus reset(s)\n", iVariant, i,
                                   tstVfioKrnlDevGetBusResetCount(apDevs[i])));

                /* Reset or not, the function keeps quiet until the guest sets it up again. */
                uint8_t const *pbCfg = (uint8_t const *)tstVfioKrnlDevGetRegionPtr(apDevs[i], VFIO_PCI_CONFIG_REGION_INDEX);
                RTTESTI_CHECK(pbCfg != NULL);
                if (pbCfg)
                    RTTESTI_CHECK_MSG(!(pbCfg[VBOX_PCI_COMMAND] & fCmdActive),
                                      ("variant %u, function %u: command register of the device %#x\n", iVariant, i,
                                       pbCfg[VBOX_PCI_COMMAND]));
                uint32_t const u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN + i, VBOX_PCI_COMMAND, 2);
                RTTESTI_CHECK_MSG(!(u32 & fCmdActive), ("variant %u, function %u: command register %#RX32\n", iVariant, i, u32));
                RTTESTI_CHECK(!tstPdmIsMmio2Mapped(TST_BAR0_ADDR + i * _1M));
                RTTESTI_CHECK_MSG(tstVfioKrnlDevGetIrqCount(apDevs[i], VFIO_PCI_INTX_IRQ_INDEX) == 1,
                                  ("variant %u, function %u: INTx is not set up after the reset\n", iVariant, i));
            }

            /* Which works. */
            for (uint8_t i = 0; i < RT_ELEMENTS(aModels); i++)
            {
                tstPdmPciCfgWrite(0, TST_PCI_DEVFN + i, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR + i * _1M, 4);
                tstPdmPciCfgWrite(0, TST_PCI_DEVFN + i, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);
                RTTESTI_CHECK(tstPdmIsMmio2Mapped(TST_BAR0_ADDR + i * _1M));
            }
        }
        tstFixtureTeardown();
    }
}


/**
 * Accesses to the trapped part of a memory BAR work without the mapping of the
 * BAR, which the host kernel takes away at times.
 */
static void tstTrappedAccessMappingGone(void)
{
    RTTestISub("Trapped BAR accesses don't need the mapping of the BAR");

    /* MSI-X table and pending bits in the BAR: their pages are trapped, the rest is mapped. */
    uint16_t const cVectors = 8;
    uint32_t const offTable = _8K;
    uint32_t const offPba   = _8K + _4K;
    uint32_t const offReg   = offTable + 0x100;
    TSTVFIODEVCFG Model;
    tstModelInit(&Model, TST_VFIO_NODE, 0x01 /* mass storage */, 0x08 /* non-volatile memory */);
    tstModelSetBar(&Model, 0, _16K, PCI_ADDRESS_SPACE_MEM);
    tstModelAddExpCap(&Model, TST_EXP_CAP_OFF);
    tstModelAddMsixCap(&Model, TST_MSIX_CAP_OFF, cVectors, 0 /*iBar*/, offTable, offPba);

    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);

        uint8_t *pbBar0 = (uint8_t *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_BAR0_REGION_INDEX);
        RTTESTI_CHECK(pbBar0 != NULL);
        if (pbBar0)
        {
            static const uint8_t s_abDevice[4]  = { 0x22, 0x22, 0x22, 0x22 };
            static const uint8_t s_abWritten[4] = { 0x33, 0x33, 0x33, 0x33 };
            memcpy(&pbBar0[offReg], s_abDevice, sizeof(s_abDevice));

            tstVfioKrnlDevProtectMappings(pDev, true);

            /* A device register in the page of the MSI-X table. */
            uint32_t u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offReg, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(u32 == UINT32_C(0x22222222), ("read %#RX32 from the register behind the table\n", u32));
            RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_BAR0_ADDR + offReg + 4, s_abWritten, sizeof(s_abWritten)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(!memcmp(&pbBar0[offReg + 4], s_abWritten, sizeof(s_abWritten)),
                              ("the device sees %.4Rhxs in the register behind the table\n", &pbBar0[offReg + 4]));

            /* The MSI-X table of the guest. */
            u32 = UINT32_C(0xfee00000);
            RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_BAR0_ADDR + offTable, &u32, sizeof(u32)), VINF_SUCCESS);
            u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offTable, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(u32 == UINT32_C(0xfee00000), ("read %#RX32 from the MSI-X table\n", u32));

            tstVfioKrnlDevProtectMappings(pDev, false);
        }
    }
    tstFixtureTeardown();
}


/**
 * A device which has the MSI and the MSI-X capability twice, which it must not:
 * the guest gets each of them once and they work.
 */
static void tstDuplicateCaps(void)
{
    RTTestISub("MSI and MSI-X capability more than once");

    uint16_t const cVectors = 8;
    TSTVFIODEVCFG  Model;
    tstModelInit(&Model, TST_VFIO_NODE, 0x01 /* mass storage */, 0x08 /* non-volatile memory */);
    tstModelSetBar(&Model, 0, TST_BAR0_SIZE, PCI_ADDRESS_SPACE_MEM);
    tstModelSetBar(&Model, 2, _8K, PCI_ADDRESS_SPACE_MEM);
    Model.acIrqs[VFIO_PCI_INTX_IRQ_INDEX] = 1;
    tstModelAddMsiCap(&Model, 0x50, 0 /*cVectorsLog2*/, false /*f64Bit*/, false /*fMaskable*/);
    tstModelAddMsiCap(&Model, 0x60, 0 /*cVectorsLog2*/, false /*f64Bit*/, false /*fMaskable*/);
    tstModelAddExpCap(&Model, TST_EXP_CAP_OFF);
    tstModelAddMsixCap(&Model, 0xb0, cVectors, 2 /*iBar*/, 0 /*offTable*/, _4K /*offPba*/);
    tstModelAddMsixCap(&Model, 0xc0, cVectors, 2 /*iBar*/, 0 /*offTable*/, _4K /*offPba*/);

    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* Walk the capability list the guest gets to see. */
        uint32_t cMsi   = 0;
        uint32_t cMsix  = 0;
        uint32_t cOther = 0;
        uint32_t offCap = tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_CAPABILITY_LIST, 1);
        for (uint32_t cLeft = 48; offCap >= 0x40 && cLeft > 0; cLeft--)
        {
            uint32_t const bCapId = tstPdmPciCfgRead(0, TST_PCI_DEVFN, offCap, 1);
            if (bCapId == VBOX_PCI_CAP_ID_MSI)
            {
                cMsi++;
                RTTESTI_CHECK_MSG(offCap == 0x50, ("the MSI capability is at %#RX32\n", offCap));
            }
            else if (bCapId == VBOX_PCI_CAP_ID_MSIX)
            {
                cMsix++;
                RTTESTI_CHECK_MSG(offCap == 0xb0, ("the MSI-X capability is at %#RX32\n", offCap));
            }
            else
                cOther++;
            offCap = tstPdmPciCfgRead(0, TST_PCI_DEVFN, offCap + 1, 1);
        }
        RTTESTI_CHECK_MSG(cMsi == 1 && cMsix == 1 && cOther == 1,
                          ("the guest sees %u MSI, %u MSI-X and %u other capabilities\n", cMsi, cMsix, cOther));

        /* MSI-X works. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_2, TST_MSIX_BAR_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);
        tstGuestWriteMsixEntry(TST_MSIX_BAR_ADDR, 3, UINT32_C(0x4003), false /*fMasked*/);
        uint32_t const fMsgCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, 0xb0 + 2, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, 0xb0 + 2, fMsgCtrl | VBOX_PCI_MSIX_FLAGS_ENABLE, 2);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSIX_IRQ_INDEX) == cVectors);
        tstMsixTriggerAndCheck(pDev, 3, UINT32_C(0x4003));

        /* So does MSI when MSI-X is off again. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, 0xb0 + 2, fMsgCtrl & ~(uint32_t)VBOX_PCI_MSIX_FLAGS_ENABLE, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, 0x50 + 4, UINT32_C(0xfee00000), 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, 0x50 + 8, 0x4046, 2);
        uint32_t const fMsiCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, 0x50 + 2, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, 0x50 + 2, fMsiCtrl | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 1);
        uint32_t const cMsisBefore = tstPdmMsiGetCount();
        RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSI_IRQ_INDEX, 0), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("the MSI did not arrive\n"));
        TSTPDMMSI Msi;
        if (tstPdmMsiGet(cMsisBefore, &Msi))
            RTTESTI_CHECK_MSG(Msi.uData == 0x4046, ("data %#RX32\n", Msi.uData));
    }
    tstFixtureTeardown();
}


/**
 * A device node which does not exist fails the VM construction cleanly.
 */
static void tstUnknownNode(void)
{
    RTTestISub("Unknown device node");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, "/dev/vfio/devices/vfio99");
    RTTESTI_CHECK_MSG(RT_FAILURE(rc), ("the constructor succeeded: %Rrc\n", rc));
    RTTESTI_CHECK_MSG(*tstPdmGetLastError() != '\0', ("no VM error was set\n"));
    if (pDev)
        RTTESTI_CHECK(tstVfioKrnlDevGetOpenCount(pDev) == 0);
    tstFixtureTeardown();
}


/**
 * MSI: the guest can ask for any number of vectors, the device only ever gets
 * as many as it has.
 */
static void tstMsiVectorCount(void)
{
    RTTestISub("MSI vector count");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        uint32_t const cDevVectors = Model.acIrqs[VFIO_PCI_MSI_IRQ_INDEX];
        uint32_t const fMsgCtrl    =   tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2)
                                     & ~(uint32_t)(VBOX_PCI_MSI_FLAGS_QSIZE | VBOX_PCI_MSI_FLAGS_ENABLE);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 4,  UINT32_C(0xfee00000), 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 8,  0, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 12, 0x4040, 2);

        /* Go through all encodings of the Multiple Message Enable field, including the reserved ones. */
        for (uint32_t uMme = 0; uMme < 8; uMme++)
        {
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsgCtrl, 2);
            uint32_t cEnabled = tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX);
            RTTESTI_CHECK_MSG(cEnabled == 0, ("uMme=%u: %u vector(s) enabled after disabling MSI\n", uMme, cEnabled));

            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsgCtrl | (uMme << 4) | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
            uint32_t const cExpected = RT_MIN(RT_BIT_32(uMme), cDevVectors);
            cEnabled = tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX);
            RTTESTI_CHECK_MSG(cEnabled == cExpected, ("uMme=%u: %u vector(s) enabled, expected %u\n", uMme, cEnabled, cExpected));
        }

        /* Interrupts still arrive. */
        uint32_t const cMsisBefore = tstPdmMsiGetCount();
        RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSI_IRQ_INDEX, 0), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("the MSI did not arrive\n"));
    }
    tstFixtureTeardown();
}


/**
 * MSI-X: a device with more vectors than MSI allows, table and PBA in a BAR
 * of their own.  Every vector reaches the interrupt controller with the
 * address and data the guest has put into the table.
 */
static void tstMsixDedicatedBar(void)
{
    RTTestISub("MSI-X, 64 vectors, dedicated BAR");

    static const uint16_t s_aidxVectors[] = { 0, 1, 31, 32, 33, 40, 63 };
    uint16_t const        cVectors        = 64;

    TSTVFIODEVCFG Model;
    tstModelInitMsixDedicatedBar(&Model, cVectors);

    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* The capability is presented to the guest with the table size of the device. */
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF, 1) == VBOX_PCI_CAP_ID_MSIX);
        uint32_t const fMsgCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, 2);
        RTTESTI_CHECK_MSG((fMsgCtrl & 0x7ff) == (uint32_t)cVectors - 1, ("message control %#RX32\n", fMsgCtrl));

        /* Map the BAR with the table and fill in all entries, unmasked. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0 + 2 * 4, TST_MSIX_BAR_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);
        for (uint32_t i = 0; i < cVectors; i++)
        {
            uint32_t const au32Entry[4] = { UINT32_C(0xfee00000), 0, UINT32_C(0x4000) + i, 0 };
            for (uint32_t j = 0; j < RT_ELEMENTS(au32Entry); j++)
                RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_MSIX_BAR_ADDR + i * 16 + j * 4, &au32Entry[j], 4), VINF_SUCCESS);
        }

        /* Enable MSI-X. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, fMsgCtrl | RT_BIT_32(15), 2);
        uint32_t const cEnabled = tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSIX_IRQ_INDEX);
        RTTESTI_CHECK_MSG(cEnabled == cVectors, ("%u vector(s) enabled on the device\n", cEnabled));

        /* Raise a few of them, on both sides of the MSI vector limit. */
        for (uint32_t i = 0; i < RT_ELEMENTS(s_aidxVectors); i++)
        {
            uint32_t const cMsisBefore = tstPdmMsiGetCount();
            rc = tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSIX_IRQ_INDEX, s_aidxVectors[i]);
            RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("vector %u: trigger failed with %Rrc\n", s_aidxVectors[i], rc));
            if (RT_FAILURE(rc))
                continue;
            RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("vector %u did not arrive\n", s_aidxVectors[i]));
            TSTPDMMSI Msi;
            if (tstPdmMsiGet(cMsisBefore, &Msi))
            {
                RTTESTI_CHECK_MSG(Msi.uAddr == UINT64_C(0xfee00000), ("vector %u: address %#RX64\n", s_aidxVectors[i], Msi.uAddr));
                RTTESTI_CHECK_MSG(Msi.uData == UINT32_C(0x4000) + s_aidxVectors[i],
                                  ("vector %u: data %#RX32\n", s_aidxVectors[i], Msi.uData));
            }
        }
    }
    tstFixtureTeardown();
}


/**
 * Writes an MSI-X table entry the way a guest does, with four 32-bit accesses.
 *
 * @param   GCPhysTable         Where the table is mapped.
 * @param   idxVector           The entry to write.
 * @param   uData               The message data.
 * @param   fMasked             Whether the vector is masked.
 */
static void tstGuestWriteMsixEntry(RTGCPHYS GCPhysTable, uint32_t idxVector, uint32_t uData, bool fMasked)
{
    uint32_t const au32Entry[4] = { UINT32_C(0xfee00000), 0, uData, fMasked ? UINT32_C(1) : UINT32_C(0) };
    for (uint32_t i = 0; i < RT_ELEMENTS(au32Entry); i++)
        RTTESTI_CHECK_RC(tstPdmPhysWrite(GCPhysTable + idxVector * 16 + i * 4, &au32Entry[i], sizeof(uint32_t)), VINF_SUCCESS);
}


/**
 * Lets the device raise an MSI-X interrupt and checks that it arrives with the
 * expected message data.
 *
 * @param   pDev                The PCI function of the fake kernel.
 * @param   idxVector           The vector to raise.
 * @param   uDataExpected       The message data expected.
 */
static void tstMsixTriggerAndCheck(PTSTVFIODEV pDev, uint32_t idxVector, uint32_t uDataExpected)
{
    uint32_t const cMsisBefore = tstPdmMsiGetCount();
    int rc = tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSIX_IRQ_INDEX, idxVector);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("vector %u: trigger failed with %Rrc\n", idxVector, rc));
    if (RT_FAILURE(rc))
        return;
    RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("vector %u did not arrive\n", idxVector));
    TSTPDMMSI Msi;
    if (tstPdmMsiGet(cMsisBefore, &Msi))
    {
        RTTESTI_CHECK_MSG(Msi.uAddr == UINT64_C(0xfee00000), ("vector %u: address %#RX64\n", idxVector, Msi.uAddr));
        RTTESTI_CHECK_MSG(Msi.uData == uDataExpected, ("vector %u: data %#RX32, expected %#RX32\n", idxVector, Msi.uData, uDataExpected));
    }
}


/**
 * Lets the device raise an MSI-X interrupt which must not reach the guest now.
 *
 * @param   pDev                The PCI function of the fake kernel.
 * @param   idxVector           The vector to raise.
 */
static void tstMsixTriggerAndCheckHeldBack(PTSTVFIODEV pDev, uint32_t idxVector)
{
    uint32_t const cMsisBefore = tstPdmMsiGetCount();
    RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSIX_IRQ_INDEX, idxVector), VINF_SUCCESS);
    RTTESTI_CHECK_MSG(!tstPdmMsiWait(cMsisBefore + 1, 250 /*ms*/), ("vector %u was delivered although it is masked\n", idxVector));
}


/**
 * MSI-X with the table and the pending bit array inside a BAR which also holds
 * device registers, as most devices have it:
 *      - The guest sees the BAR with the size and the layout of the device.
 *      - The table the guest writes is not the one of the physical device.
 *      - Interrupts arrive with what the guest has put into its table.
 *      - Device registers next to the table and elsewhere in the BAR work.
 *      - Masked vectors are held back, show up in the pending bit array and
 *        get delivered when unmasked; same for the function mask.
 */
static void tstMsixSharedBar(void)
{
    RTTestISub("MSI-X, table and PBA inside a BAR with device registers");

    uint16_t const cVectors = 8;
    uint32_t const cbBar    = _16K;
    uint32_t const offTable = _8K;
    uint32_t const offPba   = _8K + _4K;

    TSTVFIODEVCFG Model;
    tstModelInit(&Model, TST_VFIO_NODE, 0x01 /* mass storage */, 0x08 /* non-volatile memory */);
    tstModelSetBar(&Model, 0, cbBar, PCI_ADDRESS_SPACE_MEM);
    tstModelAddExpCap(&Model, TST_EXP_CAP_OFF);
    tstModelAddMsixCap(&Model, TST_MSIX_CAP_OFF, cVectors, 0 /*iBar*/, offTable, offPba);

    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* What the physical device has in its table and next to it. */
        uint8_t *pbBar0 = (uint8_t *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_BAR0_REGION_INDEX);
        if (!pbBar0)
        {
            RTTestIFailed("no access to BAR0 of the device model");
            tstFixtureTeardown();
            return;
        }
        memset(&pbBar0[offTable], 0xa5, cVectors * 16);
        *(uint32_t *)&pbBar0[0x10]             = UINT32_C(0x11111111);
        *(uint32_t *)&pbBar0[offTable + 0x100] = UINT32_C(0x22222222);

        /* The capability and the BAR look like on the physical device. */
        uint32_t u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 4, 4);
        RTTESTI_CHECK_MSG(u32 == offTable, ("table offset/BIR %#RX32\n", u32));
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 8, 4);
        RTTESTI_CHECK_MSG(u32 == offPba, ("PBA offset/BIR %#RX32\n", u32));
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, UINT32_MAX, 4);
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, 4);
        RTTESTI_CHECK_MSG(u32 == ~(cbBar - 1), ("BAR0 sizing returned %#RX32\n", u32));
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);

        /* Device registers: far away from the table and in the same page as the table. */
        u32 = 0;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + 0x10, &u32, sizeof(u32)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u32 == UINT32_C(0x11111111), ("read %#RX32 from the register at 0x10\n", u32));
        u32 = 0;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offTable + 0x100, &u32, sizeof(u32)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u32 == UINT32_C(0x22222222), ("read %#RX32 from the register behind the table\n", u32));
        u32 = UINT32_C(0x33333333);
        RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_BAR0_ADDR + offTable + 0x104, &u32, sizeof(u32)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(*(uint32_t *)&pbBar0[offTable + 0x104] == UINT32_C(0x33333333),
                          ("the device sees %#RX32 in the register behind the table\n", *(uint32_t *)&pbBar0[offTable + 0x104]));

        /* All vectors start out masked.  Fill in the table, leaving vector 2 masked. */
        u32 = 0;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offTable + 12, &u32, sizeof(u32)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u32 == 1, ("vector control of entry 0 is %#RX32 initially\n", u32));
        for (uint32_t i = 0; i < cVectors; i++)
            tstGuestWriteMsixEntry(TST_BAR0_ADDR + offTable, i, UINT32_C(0x4000) + i, i == 2 /*fMasked*/);

        /* The guest reads back what it wrote, and the table of the physical device is untouched. */
        u32 = 0;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offTable + 3 * 16 + 8, &u32, sizeof(u32)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u32 == UINT32_C(0x4003), ("read %#RX32 as message data of entry 3\n", u32));
        bool fTouched = false;
        for (uint32_t off = 0; off < (uint32_t)cVectors * 16; off++)
            fTouched |= pbBar0[offTable + off] != 0xa5;
        RTTESTI_CHECK_MSG(!fTouched, ("the guest wrote to the MSI-X table of the physical device\n"));

        /* Enable MSI-X; unmasked vectors arrive with the guest's message. */
        uint32_t const fMsgCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, fMsgCtrl | VBOX_PCI_MSIX_FLAGS_ENABLE, 2);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSIX_IRQ_INDEX) == cVectors);
        tstMsixTriggerAndCheck(pDev, 0, UINT32_C(0x4000));
        tstMsixTriggerAndCheck(pDev, 7, UINT32_C(0x4007));

        /* The masked vector is held back, shows up as pending and gets delivered when the guest unmasks it. */
        uint64_t u64Pba = UINT64_MAX;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offPba, &u64Pba, sizeof(u64Pba)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u64Pba == 0, ("pending bits %#RX64 before anything is pending\n", u64Pba));
        tstMsixTriggerAndCheckHeldBack(pDev, 2);
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offPba, &u64Pba, sizeof(u64Pba)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u64Pba == RT_BIT_64(2), ("pending bits %#RX64 with vector 2 pending\n", u64Pba));
        uint32_t cMsisBefore = tstPdmMsiGetCount();
        u32 = 0;
        RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_BAR0_ADDR + offTable + 2 * 16 + 12, &u32, sizeof(u32)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("vector 2 was not delivered when unmasked\n"));
        TSTPDMMSI Msi;
        if (tstPdmMsiGet(cMsisBefore, &Msi))
            RTTESTI_CHECK_MSG(Msi.uData == UINT32_C(0x4002), ("vector 2: data %#RX32\n", Msi.uData));
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offPba, &u64Pba, sizeof(u64Pba)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u64Pba == 0, ("pending bits %#RX64 after the delivery\n", u64Pba));

        /* Same with the function mask. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2,
                          fMsgCtrl | VBOX_PCI_MSIX_FLAGS_ENABLE | VBOX_PCI_MSIX_FLAGS_FUNCMASK, 2);
        tstMsixTriggerAndCheckHeldBack(pDev, 5);
        cMsisBefore = tstPdmMsiGetCount();
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, fMsgCtrl | VBOX_PCI_MSIX_FLAGS_ENABLE, 2);
        RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("vector 5 was not delivered when the function got unmasked\n"));
        if (tstPdmMsiGet(cMsisBefore, &Msi))
            RTTESTI_CHECK_MSG(Msi.uData == UINT32_C(0x4005), ("vector 5: data %#RX32\n", Msi.uData));

        /* The guest can't change the pending bits. */
        u64Pba = UINT64_MAX;
        RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_BAR0_ADDR + offPba, &u64Pba, sizeof(u64Pba)), VINF_SUCCESS);
        u64Pba = UINT64_MAX;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offPba, &u64Pba, sizeof(u64Pba)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u64Pba == 0, ("pending bits %#RX64 after the guest wrote to them\n", u64Pba));
    }
    tstFixtureTeardown();
}


/**
 * Capabilities chained from high to low offsets: the guest sees them in the
 * order the device has them.
 */
static void tstCapsDescending(void)
{
    RTTestISub("Capability list from high to low offsets");

    TSTVFIODEVCFG Model;
    tstModelInit(&Model, TST_VFIO_NODE, VBOX_PCI_CLASS_NETWORK, 0x00 /* Ethernet */);
    tstModelSetBar(&Model, 0, TST_BAR0_SIZE, PCI_ADDRESS_SPACE_MEM);
    Model.acIrqs[VFIO_PCI_INTX_IRQ_INDEX] = 1;
    tstModelAddExpCap(&Model, 0xb0);
    tstModelAddMsiCap(&Model, TST_MSI_CAP_OFF, 0 /*cVectorsLog2*/, true /*f64Bit*/, false /*fMaskable*/);

    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_CAPABILITY_LIST, 1) == 0xb0);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, 0xb0, 1) == VBOX_PCI_CAP_ID_EXP);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, 0xb1, 1) == TST_MSI_CAP_OFF);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF, 1) == VBOX_PCI_CAP_ID_MSI);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 1, 1) == 0);
    }
    tstFixtureTeardown();
}


/**
 * A capability list which loops fails the VM construction instead of hanging it.
 */
static void tstCapsLoop(void)
{
    RTTestISub("Capability list which loops");

    static const uint8_t s_abPmBody[6] = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 };
    TSTVFIODEVCFG Model;
    tstModelInit(&Model, TST_VFIO_NODE, VBOX_PCI_CLASS_NETWORK, 0x00 /* Ethernet */);
    tstModelSetBar(&Model, 0, TST_BAR0_SIZE, PCI_ADDRESS_SPACE_MEM);
    Model.acIrqs[VFIO_PCI_INTX_IRQ_INDEX] = 1;
    tstModelAddCap(&Model, 0x40, 0x01 /* power management */, s_abPmBody, sizeof(s_abPmBody));
    tstModelAddExpCap(&Model, TST_EXP_CAP_OFF);
    Model.abCfg[TST_EXP_CAP_OFF + 1] = 0x40; /* back to the first one */

    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_FAILURE(rc), ("the constructor succeeded: %Rrc\n", rc));
    RTTESTI_CHECK_MSG(*tstPdmGetLastError() != '\0', ("no VM error was set\n"));
    tstFixtureTeardown();
}


/**
 * An extended capability list which loops fails the VM construction instead
 * of hanging it.
 */
static void tstExtCapsLoop(void)
{
    RTTestISub("Extended capability list which loops");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    Model.cbCfg = 4096;
    tstModelSetU32(&Model, 0x100, UINT32_C(0x0001) | RT_BIT_32(16) | (UINT32_C(0x140) << 20)); /* advanced error reporting */
    tstModelSetU32(&Model, 0x140, UINT32_C(0x0003) | RT_BIT_32(16) | (UINT32_C(0x100) << 20)); /* device serial number */

    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_FAILURE(rc), ("the constructor succeeded: %Rrc\n", rc));
    RTTESTI_CHECK_MSG(*tstPdmGetLastError() != '\0', ("no VM error was set\n"));
    tstFixtureTeardown();
}


/**
 * The smallest MSI capability there is, 32-bit address and no masking, with
 * another capability right behind it: the guest gets MSI with that layout and
 * the neighbour stays what it is.
 */
static void tstMsiSmallLayout(void)
{
    RTTestISub("32-bit MSI without masking, followed by another capability");

    uint8_t const        offVendorCap      = TST_MSI_CAP_OFF + 12;
    static const uint8_t s_abVendorBody[6] = { 8 /* length */, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5 };
    TSTVFIODEVCFG Model;
    tstModelInit(&Model, TST_VFIO_NODE, VBOX_PCI_CLASS_NETWORK, 0x00 /* Ethernet */);
    tstModelSetBar(&Model, 0, TST_BAR0_SIZE, PCI_ADDRESS_SPACE_MEM);
    Model.acIrqs[VFIO_PCI_INTX_IRQ_INDEX] = 1;
    tstModelAddMsiCap(&Model, TST_MSI_CAP_OFF, 0 /*cVectorsLog2*/, false /*f64Bit*/, false /*fMaskable*/);
    tstModelAddCap(&Model, offVendorCap, 0x09 /* vendor specific */, s_abVendorBody, sizeof(s_abVendorBody));
    tstModelAddExpCap(&Model, TST_EXP_CAP_OFF);

    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* The guest sees the layout of the device. */
        uint32_t const fMsgCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2);
        RTTESTI_CHECK_MSG(!(fMsgCtrl & (VBOX_PCI_MSI_FLAGS_64BIT | VBOX_PCI_MSI_FLAGS_MASKBIT | VBOX_PCI_MSI_FLAGS_ENABLE)),
                          ("message control %#RX32\n", fMsgCtrl));
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 1, 1) == offVendorCap);

        /* The vendor capability behind it is the one of the device. */
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, offVendorCap, 1) == 0x09);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, offVendorCap + 1, 1) == TST_EXP_CAP_OFF);
        uint32_t u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, offVendorCap + 4, 4);
        RTTESTI_CHECK_MSG(u32 == UINT32_C(0xa5a4a3a2), ("read %#RX32 from the vendor capability\n", u32));

        /* Reading through the whole config space with every access width works. */
        for (uint32_t off = 0; off < 256; off++)
        {
            tstPdmPciCfgRead(0, TST_PCI_DEVFN, off, 1);
            if (!(off & 1))
                tstPdmPciCfgRead(0, TST_PCI_DEVFN, off, 2);
            if (!(off & 3))
                tstPdmPciCfgRead(0, TST_PCI_DEVFN, off, 4);
        }

        /* MSI works with this layout: address at +4, data at +8. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 4, UINT32_C(0xfee00000), 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 8, 0x4043, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsgCtrl | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 1);
        uint32_t const cMsisBefore = tstPdmMsiGetCount();
        RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSI_IRQ_INDEX, 0), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("the MSI did not arrive\n"));
        TSTPDMMSI Msi;
        if (tstPdmMsiGet(cMsisBefore, &Msi))
        {
            RTTESTI_CHECK_MSG(Msi.uAddr == UINT64_C(0xfee00000), ("address %#RX64\n", Msi.uAddr));
            RTTESTI_CHECK_MSG(Msi.uData == 0x4043, ("data %#RX32\n", Msi.uData));
        }

        /* None of this has changed the vendor capability of the device. */
        uint8_t const *pbCfg = (uint8_t const *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_CONFIG_REGION_INDEX);
        RTTESTI_CHECK(pbCfg != NULL);
        if (pbCfg)
            RTTESTI_CHECK_MSG(!memcmp(&pbCfg[offVendorCap + 2], s_abVendorBody, sizeof(s_abVendorBody)),
                              ("the vendor capability of the device was changed: %.6Rhxs\n", &pbCfg[offVendorCap + 2]));
    }
    tstFixtureTeardown();
}


/**
 * MSI with per-vector masking and several vectors: the vector number ends up
 * in the message data, masked vectors are held back, show up in the pending
 * bits and get delivered when unmasked.
 */
static void tstMsiMasking(void)
{
    RTTestISub("MSI with several vectors and per-vector masking");

    /* The default model has a 64-bit MSI capability with masking: mask bits at +16, pending bits at +20. */
    uint32_t const offMask    = TST_MSI_CAP_OFF + 16;
    uint32_t const offPending = TST_MSI_CAP_OFF + 20;
    TSTVFIODEVCFG  Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* The guest is offered the four vectors of the device. */
        uint32_t const fMsgCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2);
        RTTESTI_CHECK_MSG(   (fMsgCtrl & (VBOX_PCI_MSI_FLAGS_QMASK | VBOX_PCI_MSI_FLAGS_QSIZE | VBOX_PCI_MSI_FLAGS_ENABLE))
                          == (2 << 1), ("message control %#RX32\n", fMsgCtrl));

        /* Program the message, mask vector 1 and enable MSI with all four vectors. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 4,  UINT32_C(0xfee00000), 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 8,  0, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 12, 0x4040, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, offMask, RT_BIT_32(1), 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsgCtrl | (2 << 4) | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 4);

        /* Unmasked vectors arrive with the vector number in the low bits of the message data. */
        static const uint8_t s_aidxVectors[] = { 0, 3, 2 };
        TSTPDMMSI Msi;
        for (uint32_t i = 0; i < RT_ELEMENTS(s_aidxVectors); i++)
        {
            uint32_t const cMsisBefore = tstPdmMsiGetCount();
            RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSI_IRQ_INDEX, s_aidxVectors[i]), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("vector %u did not arrive\n", s_aidxVectors[i]));
            if (tstPdmMsiGet(cMsisBefore, &Msi))
                RTTESTI_CHECK_MSG(Msi.uData == UINT32_C(0x4040) + s_aidxVectors[i],
                                  ("vector %u: data %#RX32\n", s_aidxVectors[i], Msi.uData));
        }

        /* The masked one is held back and shows up as pending, which the guest can't change. */
        uint32_t cMsisBefore = tstPdmMsiGetCount();
        RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSI_IRQ_INDEX, 1), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(!tstPdmMsiWait(cMsisBefore + 1, 250 /*ms*/), ("vector 1 was delivered although it is masked\n"));
        uint32_t u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, offPending, 4);
        RTTESTI_CHECK_MSG(u32 == RT_BIT_32(1), ("pending bits %#RX32 with vector 1 pending\n", u32));
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, offPending, 0, 4);
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, offPending, 4);
        RTTESTI_CHECK_MSG(u32 == RT_BIT_32(1), ("pending bits %#RX32 after the guest wrote to them\n", u32));

        /* Unmasking delivers it. */
        cMsisBefore = tstPdmMsiGetCount();
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, offMask, 0, 4);
        RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("vector 1 was not delivered when unmasked\n"));
        if (tstPdmMsiGet(cMsisBefore, &Msi))
            RTTESTI_CHECK_MSG(Msi.uData == UINT32_C(0x4041), ("vector 1: data %#RX32\n", Msi.uData));
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, offPending, 4);
        RTTESTI_CHECK_MSG(u32 == 0, ("pending bits %#RX32 after the delivery\n", u32));

        /* There are only mask bits for the vectors the device has. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, offMask, UINT32_MAX, 4);
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, offMask, 4);
        RTTESTI_CHECK_MSG(u32 == UINT32_C(0xf), ("mask bits %#RX32\n", u32));
    }
    tstFixtureTeardown();
}


/**
 * Config space writes of every width do what they should to the registers
 * they cover, also when they cover several registers or only a part of one.
 */
static void tstCfgWriteWidths(void)
{
    RTTestISub("Config space writes of all widths");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* A dword write covering the command and the status register enables memory decoding. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 4);
        uint32_t u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, 2);
        RTTESTI_CHECK_MSG(u32 & VBOX_PCI_COMMAND_MEMORY, ("command register %#RX32\n", u32));
        uint32_t *pau32Bar0 = (uint32_t *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_BAR0_REGION_INDEX);
        RTTESTI_CHECK(pau32Bar0 != NULL);
        if (pau32Bar0)
        {
            pau32Bar0[0] = UINT32_C(0x88888888);
            u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(u32 == UINT32_C(0x88888888), ("read %#RX32 from BAR0\n", u32));
        }

        /* A byte write to the upper half of the command register disables INTx and leaves the lower half alone. */
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_INTX_IRQ_INDEX) == 1);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND + 1, VBOX_PCI_COMMAND_INTX_DISABLE >> 8, 1);
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, 2);
        RTTESTI_CHECK_MSG(   (u32 & (VBOX_PCI_COMMAND_INTX_DISABLE | VBOX_PCI_COMMAND_MEMORY))
                          == (VBOX_PCI_COMMAND_INTX_DISABLE | VBOX_PCI_COMMAND_MEMORY), ("command register %#RX32\n", u32));
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_INTX_IRQ_INDEX) == 0);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND + 1, 0, 1);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_INTX_IRQ_INDEX) == 1);

        /* A byte write to the lower half of the MSI message control register enables MSI. */
        uint32_t const fMsgCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, (fMsgCtrl & 0xff) | VBOX_PCI_MSI_FLAGS_ENABLE, 1);
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2);
        RTTESTI_CHECK_MSG(u32 == (fMsgCtrl | VBOX_PCI_MSI_FLAGS_ENABLE), ("message control %#RX32\n", u32));
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 1);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_INTX_IRQ_INDEX) == 0);

        /* A dword write covering the capability header and the message control register disables MSI
           again and changes nothing else. */
        uint32_t const u32CapHdr = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF, UINT32_MAX & ~((uint32_t)VBOX_PCI_MSI_FLAGS_ENABLE << 16), 4);
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF, 4);
        RTTESTI_CHECK_MSG(   (u32 & UINT32_C(0xffff)) == (u32CapHdr & UINT32_C(0xffff))
                          && !(u32 & ((uint32_t)VBOX_PCI_MSI_FLAGS_ENABLE << 16))
                          && ((u32 >> 16) & (VBOX_PCI_MSI_FLAGS_QMASK | VBOX_PCI_MSI_FLAGS_64BIT | VBOX_PCI_MSI_FLAGS_MASKBIT))
                             == (fMsgCtrl & (VBOX_PCI_MSI_FLAGS_QMASK | VBOX_PCI_MSI_FLAGS_64BIT | VBOX_PCI_MSI_FLAGS_MASKBIT)),
                          ("capability header and message control %#RX32, was %#RX32\n", u32, u32CapHdr));
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 0);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_INTX_IRQ_INDEX) == 1);

        /* A dword write covering the message data and the reserved word behind it sets the message data. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 12, UINT32_C(0xffff4044), 4);
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 12, 4);
        RTTESTI_CHECK_MSG(u32 == UINT32_C(0x4044), ("message data and reserved word %#RX32\n", u32));

        /* The interrupt line register can be written on its own and together with its read-only neighbours. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_INTERRUPT_LINE, 0x0b, 1);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_INTERRUPT_LINE, 1) == 0x0b);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_INTERRUPT_LINE, UINT32_C(0xffffff05), 4);
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_INTERRUPT_LINE, 2);
        RTTESTI_CHECK_MSG(u32 == UINT32_C(0x0105), ("interrupt line and pin %#RX32\n", u32));
    }
    tstFixtureTeardown();
}


/**
 * A device with two functions: both show up with their own identity and each
 * BAR belongs to the function which has it.
 */
static void tstMultiFunction(void)
{
    RTTestISub("Two functions, each with a memory BAR");

    uint32_t const    acbBar0[2]      = { _64K, _16K };
    uint32_t const    aGCPhysBar0[2]  = { TST_BAR0_ADDR, TST_BAR0_ADDR + _1M };
    uint32_t const    au32Pattern[2]  = { UINT32_C(0xf00d0000), UINT32_C(0xf00d0001) };
    TSTVFIODEVCFG     aModels[2];
    tstModelInit(&aModels[0], "/dev/vfio/devices/vfio0", 0x03 /* display */, 0x80 /* other */);
    tstModelInit(&aModels[1], "/dev/vfio/devices/vfio1", 0x04 /* multimedia */, 0x03 /* audio device */);
    tstModelSetU16(&aModels[1], VBOX_PCI_DEVICE_ID, TST_DEVICE_ID + 1);
    for (uint32_t i = 0; i < RT_ELEMENTS(aModels); i++)
    {
        tstModelSetBar(&aModels[i], 0, acbBar0[i], PCI_ADDRESS_SPACE_MEM);
        tstModelAddExpCap(&aModels[i], TST_EXP_CAP_OFF);
    }

    PTSTVFIODEV apDevs[2] = { NULL, NULL };
    int rc = tstFixtureSetup(aModels, RT_ELEMENTS(aModels), apDevs, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* Both functions are there and function 0 tells the guest to look for more. */
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN,     VBOX_PCI_DEVICE_ID, 2) == TST_DEVICE_ID);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN + 1, VBOX_PCI_DEVICE_ID, 2) == TST_DEVICE_ID + 1);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN,     VBOX_PCI_HEADER_TYPE, 1) & 0x80);

        for (uint8_t i = 0; i < RT_ELEMENTS(aModels); i++)
        {
            /* The BAR has the size it has on this function ... */
            uint8_t const uDevFn = TST_PCI_DEVFN + i;
            tstPdmPciCfgWrite(0, uDevFn, VBOX_PCI_BASE_ADDRESS_0, UINT32_MAX, 4);
            uint32_t u32 = tstPdmPciCfgRead(0, uDevFn, VBOX_PCI_BASE_ADDRESS_0, 4);
            RTTESTI_CHECK_MSG(u32 == ~(acbBar0[i] - 1), ("function %u: BAR0 sizing returned %#RX32\n", i, u32));
            tstPdmPciCfgWrite(0, uDevFn, VBOX_PCI_BASE_ADDRESS_0, aGCPhysBar0[i], 4);
            tstPdmPciCfgWrite(0, uDevFn, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);

            /* ... and shows the memory of this function. */
            uint32_t *pau32Bar0 = (uint32_t *)tstVfioKrnlDevGetRegionPtr(apDevs[i], VFIO_PCI_BAR0_REGION_INDEX);
            RTTESTI_CHECK(pau32Bar0 != NULL);
            if (pau32Bar0)
            {
                pau32Bar0[1] = au32Pattern[i];
                u32 = 0;
                RTTESTI_CHECK_RC(tstPdmPhysRead(aGCPhysBar0[i] + 4, &u32, sizeof(u32)), VINF_SUCCESS);
                RTTESTI_CHECK_MSG(u32 == au32Pattern[i], ("function %u: read %#RX32 from BAR0\n", i, u32));
            }
        }
    }
    tstFixtureTeardown();
}


/**
 * A BAR which VFIO does not allow to map, a small one at that: the guest
 * reaches the device registers nevertheless, and nothing behind the BAR.
 */
static void tstUnmappableBar(void)
{
    RTTestISub("BAR which can't be mapped");

    uint32_t const cbBar1     = 256;
    uint32_t const GCPhysBar1 = TST_BAR0_ADDR + _1M;
    TSTVFIODEVCFG  Model;
    tstModelInitDefault(&Model);
    tstModelSetBar(&Model, 1, cbBar1, PCI_ADDRESS_SPACE_MEM);
    Model.aRegions[1].fFlags &= ~(uint32_t)VFIO_REGION_INFO_FLAG_MMAP;

    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0 + 4, GCPhysBar1, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);

        /* The test maps a whole page of the model, of which the first 256 bytes are the BAR. */
        uint32_t *pau32Bar1 = (uint32_t *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_BAR1_REGION_INDEX);
        RTTESTI_CHECK(pau32Bar1 != NULL);
        if (pau32Bar1)
        {
            /* Reads and writes reach the device with the width the guest uses. */
            pau32Bar1[4] = UINT32_C(0x44444444);
            uint32_t u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(GCPhysBar1 + 16, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(u32 == UINT32_C(0x44444444), ("read %#RX32 from BAR1\n", u32));
            u32 = UINT32_C(0x55555555);
            RTTESTI_CHECK_RC(tstPdmPhysWrite(GCPhysBar1 + 32, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(pau32Bar1[8] == UINT32_C(0x55555555), ("the device sees %#RX32 in BAR1\n", pau32Bar1[8]));
            uint8_t b = 0x66;
            RTTESTI_CHECK_RC(tstPdmPhysWrite(GCPhysBar1 + 37, &b, sizeof(b)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(pau32Bar1[9] == UINT32_C(0x00006600), ("the device sees %#RX32 after a byte write\n", pau32Bar1[9]));

            /* The guest sees a page, but there is nothing behind the BAR and nothing it could change there. */
            pau32Bar1[cbBar1 / 4] = UINT32_C(0x77777777);
            u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(GCPhysBar1 + cbBar1, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(u32 == UINT32_MAX, ("read %#RX32 from behind BAR1\n", u32));
            u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysWrite(GCPhysBar1 + cbBar1, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(pau32Bar1[cbBar1 / 4] == UINT32_C(0x77777777),
                              ("a guest write behind BAR1 changed the device memory to %#RX32\n", pau32Bar1[cbBar1 / 4]));
        }
    }
    tstFixtureTeardown();
}


/**
 * Reset with MSI enabled: the device gets reset, MSI is off afterwards on both
 * sides and works again when the guest enables it anew.
 */
static void tstResetMsi(void)
{
    RTTestISub("Reset with MSI enabled");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        uint32_t const fMsgCtrl =   tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2)
                                  & ~(uint32_t)(VBOX_PCI_MSI_FLAGS_QSIZE | VBOX_PCI_MSI_FLAGS_ENABLE);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsgCtrl | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 1);

        uint32_t const cResetsBefore = tstVfioKrnlDevGetResetCount(pDev);
        tstPdmReset();
        RTTESTI_CHECK_MSG(tstVfioKrnlDevGetResetCount(pDev) == cResetsBefore + 1,
                          ("%u device reset(s)\n", tstVfioKrnlDevGetResetCount(pDev) - cResetsBefore));

        /* The guest sees MSI disabled and the device does not have it set up any more. */
        RTTESTI_CHECK(!(tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2) & VBOX_PCI_MSI_FLAGS_ENABLE));
        uint32_t const cEnabled = tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX);
        RTTESTI_CHECK_MSG(cEnabled == 0, ("%u MSI vector(s) enabled on the device after the reset\n", cEnabled));

        /* Enabling it again works and interrupts arrive. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 4,  UINT32_C(0xfee00000), 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 8,  0, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 12, 0x4042, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsgCtrl | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 1);
        uint32_t const cMsisBefore = tstPdmMsiGetCount();
        RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSI_IRQ_INDEX, 0), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("the MSI did not arrive\n"));
    }
    tstFixtureTeardown();
}


/**
 * Reset with MSI-X enabled: MSI-X is off afterwards on both sides and all
 * vectors are masked.
 */
static void tstResetMsix(void)
{
    RTTestISub("Reset with MSI-X enabled");

    uint16_t const cVectors = 8;
    TSTVFIODEVCFG  Model;
    tstModelInitMsixDedicatedBar(&Model, cVectors);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* Map the table, unmask vector 0 and enable MSI-X. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0 + 2 * 4, TST_MSIX_BAR_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);
        uint32_t u32 = 0;
        RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_MSIX_BAR_ADDR + 12, &u32, sizeof(u32)), VINF_SUCCESS);
        uint32_t const fMsgCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, fMsgCtrl | VBOX_PCI_MSIX_FLAGS_ENABLE, 2);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSIX_IRQ_INDEX) == cVectors);

        tstPdmReset();

        /* The guest sees MSI-X disabled with the table size intact, and the device does not have it set up any more. */
        u32 = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, 2);
        RTTESTI_CHECK_MSG(u32 == (uint32_t)cVectors - 1, ("message control %#RX32 after the reset\n", u32));
        uint32_t const cEnabled = tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSIX_IRQ_INDEX);
        RTTESTI_CHECK_MSG(cEnabled == 0, ("%u MSI-X vector(s) enabled on the device after the reset\n", cEnabled));

        /* The vector unmasked before the reset is masked again; map the table where the tests want it to look. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, 0, 2);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0 + 2 * 4, TST_MSIX_BAR_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY, 2);
        u32 = 0;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_MSIX_BAR_ADDR + 12, &u32, sizeof(u32)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u32 & RT_BIT_32(0), ("vector control of entry 0 is %#RX32 after the reset\n", u32));
    }
    tstFixtureTeardown();
}


int main()
{
    RTTEST hTest;
    RTEXITCODE rcExit = RTTestInitAndCreate("tstDevPciVfio", &hTest);
    if (rcExit != RTEXITCODE_SUCCESS)
        return rcExit;
    RTTestBanner(hTest);

    tstBasics();
    tstMsi();
    tstMsiVectorCount();
    tstMsixDedicatedBar();
    tstMsixSharedBar();
    tstMsiSmallLayout();
    tstMsiMasking();
    tstCfgWriteWidths();
    tstCapsDescending();
    tstCapsLoop();
    tstExtCapsLoop();
    tstMultiFunction();
    tstUnmappableBar();
    tstResetMsi();
    tstResetMsix();
    tstUnknownNode();
    tstExecEngine();
    tstDmaMapFailure();
    tstIoBarAndSilentDevice();
    tstVga();
    tstQuirkConfigRange();
    tstReadOnlyBar();
    tstFdsNotInherited();
    tstInterceptMmio();
    tstUnsuitableDevices();
    tstHostAddress();
    tstCmdRegOrder();
    tstResetFallback();
    tstTrappedAccessMappingGone();
    tstDuplicateCaps();

    return RTTestSummaryAndDestroy(hTest);
}
