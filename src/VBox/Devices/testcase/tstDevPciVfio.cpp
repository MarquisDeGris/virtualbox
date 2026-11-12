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
#include "../Bus/DevPciInternal.h" /* First, it has to get at the internals of the PCI devices. */
#include <VBox/msi.h>
#include <VBox/pci.h>
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/vmapi.h>
#include <VBox/err.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/env.h>
#include <iprt/rand.h>
#include <iprt/string.h>
#include <iprt/test.h>
#include <iprt/thread.h>
#include <iprt/time.h>

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
    pModel->acIrqs[VFIO_PCI_ERR_IRQ_INDEX] = 1;
    pModel->acIrqs[VFIO_PCI_REQ_IRQ_INDEX] = 1;
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
/** Whether tstFixtureSetup() sets things up for the legacy interface: the fake
 * kernel has a container and a group only, and the device is told about the group. */
static bool     g_fTstLegacy          = false;


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
    tstVfioKrnlSetLegacy(g_fTstLegacy);
    if (g_cTstFailIoasMapSkip != UINT32_MAX)
        tstVfioKrnlFailIoctl(g_fTstLegacy ? VFIO_IOMMU_MAP_DMA : IOMMU_IOAS_MAP, g_cTstFailIoasMapSkip, ENOMEM);
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
    if (g_fTstLegacy)
        RTTESTI_CHECK_RC(tstCfgSetString(pCfg, "VfioGroupPath", TSTVFIO_GROUP_NODE), VINF_SUCCESS);
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

        /*
         * Right behind the table and the pending bits: the first byte there belongs to the
         * device, and an access reaching across the end is split between the two.
         */
        uint32_t const offTableEnd = offTable + cVectors * 16;
        uint32_t const offPbaEnd   = offPba + 8;
        pbBar0[offTableEnd]     = 0x55;
        pbBar0[offTableEnd + 1] = 0x66;
        pbBar0[offPbaEnd]       = 0x77;
        pbBar0[offPbaEnd + 1]   = 0x88;
        uint8_t bEdge = 0;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offTableEnd, &bEdge, sizeof(bEdge)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(bEdge == 0x55, ("read %#x from the byte behind the table\n", bEdge));
        u32 = UINT32_MAX;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offTableEnd - 2, &u32, sizeof(u32)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u32 == UINT32_C(0x66550000), ("read %#RX32 across the end of the table\n", u32));
        bEdge = 0;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offPbaEnd, &bEdge, sizeof(bEdge)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(bEdge == 0x77, ("read %#x from the byte behind the pending bits\n", bEdge));
        u32 = UINT32_MAX;
        RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + offPbaEnd - 2, &u32, sizeof(u32)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(u32 == UINT32_C(0x88770000), ("read %#RX32 across the end of the pending bits\n", u32));
        static const uint8_t s_abAcross[4] = { 0xff, 0xff, 0xee, 0xdd };
        RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_BAR0_ADDR + offTableEnd - 2, s_abAcross, sizeof(s_abAcross)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(   pbBar0[offTableEnd - 2] == 0xa5 && pbBar0[offTableEnd - 1] == 0xa5
                          && pbBar0[offTableEnd]     == 0xee && pbBar0[offTableEnd + 1] == 0xdd,
                          ("the device has %.4Rhxs around the end of the table after a write across it\n",
                           &pbBar0[offTableEnd - 2]));
        RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_BAR0_ADDR + offPbaEnd - 2, s_abAcross, sizeof(s_abAcross)), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(pbBar0[offPbaEnd] == 0xee && pbBar0[offPbaEnd + 1] == 0xdd,
                          ("the device has %.2Rhxs behind the pending bits after a write across their end\n", &pbBar0[offPbaEnd]));

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
 * Legacy interface: construction, guest memory, a BAR, MSI, a reset, and
 * nothing being left for child processes.
 */
static void tstLegacyBasics(void)
{
    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        /* The guest RAM is mapped through the container. */
        RTTESTI_CHECK_MSG(tstVfioKrnlGetDmaMappedBytes() == TST_RAM_SIZE - TSTPDM_RAM_HOLE_SIZE,
                          ("%RU64 bytes mapped for DMA\n", tstVfioKrnlGetDmaMappedBytes()));
        RTTESTI_CHECK(tstVfioKrnlIsIovaRangeMapped(0, TSTPDM_RAM_HOLE_START));
        RTTESTI_CHECK(tstVfioKrnlDevGetOpenCount(pDev) == 1);
        tstPdmPowerOn();

        /* The BAR. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
        uint32_t *pau32Bar0 = (uint32_t *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_BAR0_REGION_INDEX);
        RTTESTI_CHECK(pau32Bar0 != NULL);
        if (pau32Bar0)
        {
            pau32Bar0[4] = UINT32_C(0xfeedface);
            uint32_t u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + 16, &u32, sizeof(u32)), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(u32 == UINT32_C(0xfeedface), ("read %#RX32 from BAR0\n", u32));
        }
        uint8_t const *pbCfg = (uint8_t const *)tstVfioKrnlDevGetRegionPtr(pDev, VFIO_PCI_CONFIG_REGION_INDEX);
        RTTESTI_CHECK(pbCfg != NULL);
        if (pbCfg)
            RTTESTI_CHECK_MSG(pbCfg[VBOX_PCI_COMMAND] & VBOX_PCI_COMMAND_MASTER,
                              ("command register of the device %#x\n", pbCfg[VBOX_PCI_COMMAND]));

        /* MSI. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 4,  UINT32_C(0xfee00000), 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 8,  0, 4);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 12, 0x4047, 2);
        uint32_t const fMsgCtrl =   tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2)
                                  & ~(uint32_t)(VBOX_PCI_MSI_FLAGS_QSIZE | VBOX_PCI_MSI_FLAGS_ENABLE);
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsgCtrl | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 1);
        uint32_t const cMsisBefore = tstPdmMsiGetCount();
        RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSI_IRQ_INDEX, 0), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("the MSI did not arrive\n"));
        TSTPDMMSI Msi;
        if (tstPdmMsiGet(cMsisBefore, &Msi))
            RTTESTI_CHECK_MSG(Msi.uData == 0x4047, ("data %#RX32\n", Msi.uData));

        /* Container, group, device and the eventfd are not for child processes. */
        RTTESTI_CHECK_MSG(tstVfioKrnlGetInheritableFdCount() == 0,
                          ("%u file descriptor(s) would be inherited\n", tstVfioKrnlGetInheritableFdCount()));

        /* A reset. */
        uint32_t const cResetsBefore = tstVfioKrnlDevGetResetCount(pDev);
        tstPdmReset();
        RTTESTI_CHECK_MSG(tstVfioKrnlDevGetResetCount(pDev) == cResetsBefore + 1,
                          ("%u device reset(s)\n", tstVfioKrnlDevGetResetCount(pDev) - cResetsBefore));
        RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 0);
        RTTESTI_CHECK_MSG(tstVfioKrnlGetDmaMappedBytes() == TST_RAM_SIZE - TSTPDM_RAM_HOLE_SIZE,
                          ("%RU64 bytes mapped for DMA after the reset\n", tstVfioKrnlGetDmaMappedBytes()));
    }
    tstFixtureTeardown();
}


/**
 * Legacy interface: a part of the guest memory can't be mapped, so the VM
 * doesn't start and nothing stays mapped.
 */
static void tstLegacyDmaMapFailure(void)
{
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
        tstFixtureTeardown();
    }
}


/**
 * Legacy interface: two functions which can't be reset on their own get reset
 * through their bus, with the group showing that they are ours.
 */
static void tstLegacyBusReset(void)
{
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
    }

    PTSTVFIODEV apDevs[2] = { NULL, NULL };
    int rc = tstFixtureSetup(aModels, RT_ELEMENTS(aModels), apDevs, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN + 1, VBOX_PCI_DEVICE_ID, 2) == TST_DEVICE_ID + 1);
        tstPdmReset();
        for (uint32_t i = 0; i < RT_ELEMENTS(aModels); i++)
            RTTESTI_CHECK_MSG(tstVfioKrnlDevGetBusResetCount(apDevs[i]) == 1,
                              ("function %u: %u bus reset(s)\n", i, tstVfioKrnlDevGetBusResetCount(apDevs[i])));
    }
    tstFixtureTeardown();
}


/**
 * The legacy VFIO interface with a container and a group, which is what gets
 * used on a host without IOMMUFD: things work the same way.
 */
static void tstLegacy(void)
{
    RTTestISub("Legacy interface with container and group");

    g_fTstLegacy = true;
    tstLegacyBasics();
    tstLegacyDmaMapFailure();
    tstLegacyBusReset();
    g_fTstLegacy = false;
}


/**
 * Waits for the device to have let go of a function of the fake kernel and to
 * have asked for the rest to be done on an EMT.
 *
 * @returns true if the function is no longer open, false if the time is up.
 * @param   pDev                The PCI function of the fake kernel.
 * @param   cReqsBefore         What tstPdmGetRequestCount() returned before the host started taking the function back.
 * @param   cMsTimeout          How long to wait at most.
 */
static bool tstWaitForRelease(PTSTVFIODEV pDev, uint32_t cReqsBefore, RTMSINTERVAL cMsTimeout)
{
    uint64_t const msStart = RTTimeMilliTS();
    while (   tstVfioKrnlDevGetOpenCount(pDev) != 0
           || tstPdmGetRequestCount() == cReqsBefore)
    {
        if (RTTimeMilliTS() - msStart >= cMsTimeout)
            return false;
        RTThreadSleep(1);
    }
    return true;
}


/**
 * Checks what the guest gets from a function the device has let go of: it
 * looks like there is nothing in the slot, and nothing the guest does changes
 * that or reaches the host.
 *
 * @param   pDev                The PCI function of the fake kernel.
 * @param   uDevFn              The device and function number of the function.
 * @param   GCPhysBar0          Where the guest had BAR0 mapped.
 * @param   pszWhat             What happened to the function, for messages.
 */
static void tstCheckFunctionIsGone(PTSTVFIODEV pDev, uint8_t uDevFn, RTGCPHYS GCPhysBar0, const char *pszWhat)
{
    /* The config space reads as all ones and ignores writes. */
    uint32_t u32 = tstPdmPciCfgRead(0, uDevFn, VBOX_PCI_VENDOR_ID, 2);
    RTTESTI_CHECK_MSG(u32 == UINT32_C(0xffff), ("%s: vendor ID %#RX32\n", pszWhat, u32));
    u32 = tstPdmPciCfgRead(0, uDevFn, VBOX_PCI_BASE_ADDRESS_0, 4);
    RTTESTI_CHECK_MSG(u32 == UINT32_MAX, ("%s: BAR0 reads %#RX32\n", pszWhat, u32));
    u32 = tstPdmPciCfgRead(0, uDevFn, VBOX_PCI_COMMAND, 1);
    RTTESTI_CHECK_MSG(u32 == UINT32_C(0xff), ("%s: the command register reads %#RX32\n", pszWhat, u32));

    /* The BARs are not mapped any more and the guest can't get them mapped again. */
    RTTESTI_CHECK_MSG(!tstPdmIsMmio2Mapped(GCPhysBar0), ("%s: BAR0 is mapped into the guest\n", pszWhat));
    tstPdmPciCfgWrite(0, uDevFn, VBOX_PCI_BASE_ADDRESS_0, (uint32_t)GCPhysBar0, 4);
    tstPdmPciCfgWrite(0, uDevFn, VBOX_PCI_COMMAND,
                      VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
    RTTESTI_CHECK_MSG(!tstPdmIsMmio2Mapped(GCPhysBar0), ("%s: the guest got BAR0 mapped again\n", pszWhat));
    u32 = 0;
    RTTESTI_CHECK_RC(tstPdmPhysRead(GCPhysBar0 + 16, &u32, sizeof(u32)), VINF_SUCCESS);

    /* A reset of the VM leaves it alone. */
    uint32_t const cResetsBefore    = tstVfioKrnlDevGetResetCount(pDev);
    uint32_t const cBusResetsBefore = tstVfioKrnlDevGetBusResetCount(pDev);
    tstPdmReset();
    RTTESTI_CHECK_MSG(   tstVfioKrnlDevGetResetCount(pDev)    == cResetsBefore
                      && tstVfioKrnlDevGetBusResetCount(pDev) == cBusResetsBefore,
                      ("%s: the function was reset\n", pszWhat));
    u32 = tstPdmPciCfgRead(0, uDevFn, VBOX_PCI_VENDOR_ID, 2);
    RTTESTI_CHECK_MSG(u32 == UINT32_C(0xffff), ("%s: vendor ID %#RX32 after a reset\n", pszWhat, u32));
    RTTESTI_CHECK(tstVfioKrnlDevGetOpenCount(pDev) == 0);
}


/**
 * The host takes the device back while the VM runs, like when its plug is
 * pulled: the device is let go of, what the guest had mapped of it stays
 * harmless until the guest mappings are gone, and from then on the guest finds
 * nothing where the function was.
 */
static void tstRemoval(void)
{
    RTTestISub("The host takes the device back while the VM runs");

    /*
     * One function with a memory and an I/O port BAR, MSI enabled.
     */
    {
        TSTVFIODEVCFG Model;
        tstModelInitDefault(&Model);
        tstModelSetBar(&Model, 1, TST_IOBAR_SIZE, PCI_ADDRESS_SPACE_IO);
        PTSTVFIODEV pDev = NULL;
        int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
        RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
        if (RT_SUCCESS(rc))
        {
            tstPdmPowerOn();
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_1, TST_IOBAR_PORT, 4);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND,
                              VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
            uint32_t const fMsgCtrl =   tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2)
                                      & ~(uint32_t)(VBOX_PCI_MSI_FLAGS_QSIZE | VBOX_PCI_MSI_FLAGS_ENABLE);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsgCtrl | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
            RTTESTI_CHECK(tstVfioKrnlDevGetIrqCount(pDev, VFIO_PCI_MSI_IRQ_INDEX) == 1);
            RTTESTI_CHECK(tstPdmIsMmio2Mapped(TST_BAR0_ADDR));
            uint64_t const cbMapped = tstVfioKrnlGetDmaMappedBytes();

            /* The plug gets pulled.  What is left for an EMT to do is held back for now. */
            uint32_t const cReqsBefore = tstPdmGetRequestCount();
            tstPdmSetRequestProcessing(false /*fAutomatic*/);
            tstVfioKrnlDevRemove(pDev);
            RTTESTI_CHECK_MSG(tstWaitForRelease(pDev, cReqsBefore, RT_MS_5SEC), ("the device has not let go of the function\n"));

            /*
             * The guest still has the BAR mapped.  What it finds there is memory which does
             * no harm: this works although the fake kernel has made the mappings of the
             * removed function unusable.
             */
            RTTESTI_CHECK(tstPdmIsMmio2Mapped(TST_BAR0_ADDR));
            uint32_t u32 = UINT32_C(0x5a5a5a5a);
            RTTESTI_CHECK_RC(tstPdmPhysWrite(TST_BAR0_ADDR + 64, &u32, sizeof(u32)), VINF_SUCCESS);
            u32 = 0;
            RTTESTI_CHECK_RC(tstPdmPhysRead(TST_BAR0_ADDR + 64, &u32, sizeof(u32)), VINF_SUCCESS);
            u32 = UINT32_MAX;
            RTTESTI_CHECK_RC(tstPdmIoPortRead(TST_IOBAR_PORT + 0x10, &u32, 2), VINF_SUCCESS);
            RTTESTI_CHECK_MSG((u32 & 0xffff) == 0xffff, ("read %#RX32 from a port of the removed function\n", u32));
            RTTESTI_CHECK_RC(tstPdmIoPortWrite(TST_IOBAR_PORT + 0x10, 0, 2), VINF_SUCCESS);

            /* An EMT gets around to it: the guest mappings go away. */
            tstPdmProcessRequests();
            tstPdmSetRequestProcessing(true /*fAutomatic*/);
            tstCheckFunctionIsGone(pDev, TST_PCI_DEVFN, TST_BAR0_ADDR, "removed function");

            /* No interrupt from it, and the guest memory stays mapped for whatever else uses it. */
            uint32_t const cMsisBefore = tstPdmMsiGetCount();
            RTTESTI_CHECK(RT_FAILURE(tstVfioKrnlDevTriggerIrq(pDev, VFIO_PCI_MSI_IRQ_INDEX, 0)));
            RTTESTI_CHECK_MSG(!tstPdmMsiWait(cMsisBefore + 1, 50 /*ms*/), ("an MSI of the removed function arrived\n"));
            RTTESTI_CHECK_MSG(tstVfioKrnlGetDmaMappedBytes() == cbMapped,
                              ("%RU64 bytes mapped for DMA, was %RU64\n", tstVfioKrnlGetDmaMappedBytes(), cbMapped));
        }
        tstFixtureTeardown();
    }

    /*
     * Two functions: one is taken back, the other one goes on working until it is taken back too.
     */
    {
        TSTVFIODEVCFG aModels[2];
        tstModelInitDefault(&aModels[0]);
        tstModelInit(&aModels[1], "/dev/vfio/devices/vfio1", 0x04 /* multimedia */, 0x03 /* audio device */);
        tstModelSetU16(&aModels[1], VBOX_PCI_DEVICE_ID, TST_DEVICE_ID + 1);
        tstModelSetBar(&aModels[1], 0, _16K, PCI_ADDRESS_SPACE_MEM);
        tstModelAddExpCap(&aModels[1], TST_EXP_CAP_OFF);

        PTSTVFIODEV apDevs[2] = { NULL, NULL };
        int rc = tstFixtureSetup(aModels, RT_ELEMENTS(aModels), apDevs, NULL);
        RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
        if (RT_SUCCESS(rc))
        {
            tstPdmPowerOn();
            for (uint8_t i = 0; i < RT_ELEMENTS(aModels); i++)
            {
                tstPdmPciCfgWrite(0, TST_PCI_DEVFN + i, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR + i * _1M, 4);
                tstPdmPciCfgWrite(0, TST_PCI_DEVFN + i, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
            }

            uint32_t cReqsBefore = tstPdmGetRequestCount();
            tstVfioKrnlDevRemove(apDevs[1]);
            RTTESTI_CHECK_MSG(tstWaitForRelease(apDevs[1], cReqsBefore, RT_MS_5SEC), ("the device has not let go of function 1\n"));
            tstCheckFunctionIsGone(apDevs[1], TST_PCI_DEVFN + 1, TST_BAR0_ADDR + _1M, "function 1");

            /* Function 0 is still there and its interrupts arrive. */
            RTTESTI_CHECK(tstVfioKrnlDevGetOpenCount(apDevs[0]) == 1);
            RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_VENDOR_ID, 2) == TST_VENDOR_ID);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
            RTTESTI_CHECK(tstPdmIsMmio2Mapped(TST_BAR0_ADDR));
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 4,  UINT32_C(0xfee00000), 4);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 8,  0, 4);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 12, 0x4048, 2);
            uint32_t const fMsgCtrl =   tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2)
                                      & ~(uint32_t)(VBOX_PCI_MSI_FLAGS_QSIZE | VBOX_PCI_MSI_FLAGS_ENABLE);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsgCtrl | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
            uint32_t const cMsisBefore = tstPdmMsiGetCount();
            RTTESTI_CHECK_RC(tstVfioKrnlDevTriggerIrq(apDevs[0], VFIO_PCI_MSI_IRQ_INDEX, 0), VINF_SUCCESS);
            RTTESTI_CHECK_MSG(tstPdmMsiWait(cMsisBefore + 1, RT_MS_5SEC), ("the MSI of function 0 did not arrive\n"));

            cReqsBefore = tstPdmGetRequestCount();
            tstVfioKrnlDevRemove(apDevs[0]);
            RTTESTI_CHECK_MSG(tstWaitForRelease(apDevs[0], cReqsBefore, RT_MS_5SEC), ("the device has not let go of function 0\n"));
            tstCheckFunctionIsGone(apDevs[0], TST_PCI_DEVFN, TST_BAR0_ADDR, "function 0");
        }
        tstFixtureTeardown();
    }
}


/**
 * The host takes the device back before the VM got powered on, and the host
 * reports an error of the device it can't recover from: the device is let go
 * of in both cases.
 */
static void tstRemovalOtherCases(void)
{
    RTTestISub("Device taken back before power on, device error");

    for (uint32_t iVariant = 0; iVariant < 2; iVariant++)
    {
        const char * const pszWhat = iVariant == 0 ? "removed before power on" : "function with an error";
        TSTVFIODEVCFG Model;
        tstModelInitDefault(&Model);
        PTSTVFIODEV pDev = NULL;
        int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
        RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("%s: pci-vfio: %Rrc - %s\n", pszWhat, rc, tstPdmGetLastError()));
        if (RT_SUCCESS(rc))
        {
            uint32_t const cReqsBefore = tstPdmGetRequestCount();
            if (iVariant == 0)
            {
                /* The VM isn't running yet, which is no reason to keep the host waiting. */
                tstVfioKrnlDevRemove(pDev);
                RTTESTI_CHECK_MSG(tstWaitForRelease(pDev, cReqsBefore, RT_MS_5SEC), ("%s: the device has not let go of the function\n", pszWhat));
                tstPdmPowerOn();
            }
            else
            {
                tstPdmPowerOn();
                tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
                tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
                tstVfioKrnlDevSignalError(pDev);
                RTTESTI_CHECK_MSG(tstWaitForRelease(pDev, cReqsBefore, RT_MS_5SEC), ("%s: the device has not let go of the function\n", pszWhat));
            }
            tstCheckFunctionIsGone(pDev, TST_PCI_DEVFN, TST_BAR0_ADDR, pszWhat);
        }
        tstFixtureTeardown();
    }
}


/**
 * @callback_method_impl{FNRTTHREAD, Pulls the plug of a function after a short
 *      while which is different every time.}
 */
static DECLCALLBACK(int) tstRemovalUnderLoadThread(RTTHREAD hThreadSelf, void *pvUser)
{
    RT_NOREF(hThreadSelf);
    uint32_t const cSpins = RTRandU32Ex(0, 20000);
    for (uint32_t i = 0; i < cSpins; i++)
        ASMNopPause();
    if (RTRandU32Ex(0, 3) == 0)
        RTThreadSleep(RTRandU32Ex(0, 2));
    tstVfioKrnlDevRemove((PTSTVFIODEV)pvUser);
    return VINF_SUCCESS;
}


/**
 * The plug gets pulled while the guest is busy with the device: accessing the
 * config space and the BARs, switching interrupts on and off, resetting.  Done
 * many times as it is a matter of timing where it hits.
 */
static void tstRemovalUnderLoad(void)
{
    RTTestISub("The host takes the device back while the guest is busy with it");

    /* MSI-X table and pending bits in BAR0, so there are trapped and directly mapped parts. */
    uint16_t const cVectors = 8;
    uint32_t const offTable = _8K;
    uint32_t const offPba   = _8K + _4K;
    uint32_t const cErrorsBefore = RTTestIErrorCount();
    for (uint32_t iRound = 0; iRound < 150 && RTTestIErrorCount() == cErrorsBefore; iRound++)
    {
        TSTVFIODEVCFG Model;
        tstModelInit(&Model, TST_VFIO_NODE, 0x01 /* mass storage */, 0x08 /* non-volatile memory */);
        tstModelSetBar(&Model, 0, _16K, PCI_ADDRESS_SPACE_MEM);
        tstModelSetBar(&Model, 1, TST_IOBAR_SIZE, PCI_ADDRESS_SPACE_IO);
        Model.acIrqs[VFIO_PCI_INTX_IRQ_INDEX] = 1;
        tstModelAddMsiCap(&Model, TST_MSI_CAP_OFF, 0 /*cVectorsLog2*/, true /*f64Bit*/, false /*fMaskable*/);
        tstModelAddExpCap(&Model, TST_EXP_CAP_OFF);
        tstModelAddMsixCap(&Model, TST_MSIX_CAP_OFF, cVectors, 0 /*iBar*/, offTable, offPba);

        PTSTVFIODEV pDev = NULL;
        int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
        RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("round %u: pci-vfio: %Rrc - %s\n", iRound, rc, tstPdmGetLastError()));
        if (RT_SUCCESS(rc))
        {
            tstPdmPowerOn();
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0, TST_BAR0_ADDR, 4);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_1, TST_IOBAR_PORT, 4);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND,
                              VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
            uint32_t const fMsiCtrl  = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, 2);
            uint32_t const fMsixCtrl = tstPdmPciCfgRead(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, 2);

            uint32_t const cReqsBefore = tstPdmGetRequestCount();
            RTTHREAD       hThread     = NIL_RTTHREAD;
            rc = RTThreadCreate(&hThread, tstRemovalUnderLoadThread, pDev, 0 /*cbStack*/, RTTHREADTYPE_DEFAULT,
                                RTTHREADFLAGS_WAITABLE, "tstPull");
            RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
            if (RT_SUCCESS(rc))
            {
                /* Keep the guest busy until the function is let go of, and a little longer. */
                uint64_t const msStart = RTTimeMilliTS();
                uint32_t       cAfter  = 0;
                for (uint32_t iOp = 0; cAfter < 64 && RTTimeMilliTS() - msStart < RT_MS_5SEC; iOp++)
                {
                    uint32_t u32 = iOp;
                    switch (iOp % 12)
                    {
                        case 0:  tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsiCtrl | VBOX_PCI_MSI_FLAGS_ENABLE, 2);    break;
                        case 1:  tstPdmPhysRead(TST_BAR0_ADDR + 0x100, &u32, sizeof(u32));                                           break;
                        case 2:  tstPdmPhysWrite(TST_BAR0_ADDR + offTable + 0x100, &u32, sizeof(u32));                               break;
                        case 3:  tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSI_CAP_OFF + 2, fMsiCtrl, 2);                               break;
                        case 4:  tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, fMsixCtrl | VBOX_PCI_MSIX_FLAGS_ENABLE, 2); break;
                        case 5:  tstPdmPhysWrite(TST_BAR0_ADDR + offTable + 12, &u32, sizeof(u32));                                  break;
                        case 6:  tstPdmIoPortRead(TST_IOBAR_PORT + 4, &u32, 2);                                                      break;
                        case 7:  tstPdmPciCfgWrite(0, TST_PCI_DEVFN, TST_MSIX_CAP_OFF + 2, fMsixCtrl, 2);                             break;
                        case 8:  tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MASTER, 2);                   break;
                        case 9:  tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND,
                                                   VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);     break;
                        case 10: tstPdmPciCfgRead(0, TST_PCI_DEVFN, iOp & 0xfc, 4);                                                  break;
                        default:
                            if ((iOp % 96) == 95)
                                tstPdmReset();
                            break;
                    }
                    if (tstVfioKrnlDevGetOpenCount(pDev) == 0)
                        cAfter++;
                }
                RTTESTI_CHECK_RC(RTThreadWait(hThread, RT_MS_30SEC, NULL), VINF_SUCCESS);
                RTTESTI_CHECK_MSG(tstWaitForRelease(pDev, cReqsBefore, RT_MS_5SEC), ("round %u: the device has not let go of the function\n", iRound));
                uint32_t const idVendor = tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_VENDOR_ID, 2);
                RTTESTI_CHECK_MSG(idVendor == UINT32_C(0xffff), ("round %u: vendor ID %#RX32\n", iRound, idVendor));
                RTTESTI_CHECK_MSG(!tstPdmIsMmio2Mapped(TST_BAR0_ADDR), ("round %u: BAR0 is mapped into the guest\n", iRound));
            }
        }
        tstFixtureTeardown();
    }
}


/**
 * The state of a device of the host can't be saved: saving the state of the VM
 * and teleporting it are refused, and so is loading a saved state into a VM
 * which has the device.
 */
static void tstSavedState(void)
{
    RTTestISub("Saved state is refused");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev = NULL;
    int rc = tstFixtureSetup(&Model, 1, &pDev, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("pci-vfio: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        tstPdmPowerOn();

        /* Saving and teleporting: refused, with something to tell the user. */
        for (unsigned fLive = 0; fLive < 2; fLive++)
        {
            rc = tstPdmSaveStatePrep(RT_BOOL(fLive));
            RTTESTI_CHECK_MSG(rc == VERR_NOT_SUPPORTED, ("fLive=%u: %Rrc\n", fLive, rc));
            RTTESTI_CHECK_MSG(!strcmp(tstPdmGetLastRuntimeErrorId(), "PciVfioNoSavedState"),
                              ("fLive=%u: runtime error '%s'\n", fLive, tstPdmGetLastRuntimeErrorId()));
            RTTESTI_CHECK_MSG(*tstPdmGetLastError() != '\0', ("fLive=%u: no message\n", fLive));
        }

        /* Loading: the saved state doesn't go with the configuration of the VM. */
        rc = tstPdmLoadStatePrep();
        RTTESTI_CHECK_MSG(rc == VERR_SSM_LOAD_CONFIG_MISMATCH, ("loading: %Rrc\n", rc));
        RTTESTI_CHECK_MSG(*tstPdmGetLastError() != '\0', ("loading: no message\n"));

        /* None of this has done anything to the device. */
        RTTESTI_CHECK(tstVfioKrnlDevGetOpenCount(pDev) == 1);
        RTTESTI_CHECK(tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_VENDOR_ID, 2) == TST_VENDOR_ID);
    }
    tstFixtureTeardown();
}


/*
 *
 * Random device models and random guest accesses.
 *
 */

/** Where tstFuzz() makes the guest map a memory BAR: 32 MiB apart, above the RAM and the ECAM window. */
#define TST_FUZZ_MEM_ADDR(a_iBar)   (UINT32_C(0xe8000000) + (a_iBar) * UINT32_C(0x02000000))
/** Where tstFuzz() makes the guest map an I/O port BAR. */
#define TST_FUZZ_IO_PORT(a_iBar)    (UINT32_C(0xd000) + (a_iBar) * UINT32_C(0x100))

/**
 * What tstFuzzGuestOp() gets to know about a device model made up by
 * tstFuzzModelInit().
 */
typedef struct TSTFUZZINFO
{
    /** Whether the device is configured to expose the legacy VGA ranges. */
    bool        fExposeVga;
    /** Offsets into the BARs where something starts or ends. */
    uint32_t    aaoffEdges[6][8];
    /** Number of such offsets per BAR. */
    uint32_t    acEdges[6];
} TSTFUZZINFO;
/** Pointer to the information about a made up device model. */
typedef TSTFUZZINFO *PTSTFUZZINFO;
/** Pointer to const information about a made up device model. */
typedef const TSTFUZZINFO *PCTSTFUZZINFO;

/** The random number generator of tstFuzz(). */
static RTRAND g_hTstRand = NIL_RTRAND;
/** Whether the host took the device of the current model of tstFuzz() back or reported an error of it. */
static bool   g_fTstFuzzRemoved = false;


/** Returns a random number in a range, both ends included. */
DECLINLINE(uint32_t) tstRnd(uint32_t uFirst, uint32_t uLast)
{
    return RTRandAdvU32Ex(g_hTstRand, uFirst, uLast);
}


/** Returns true in the given percentage of calls. */
DECLINLINE(bool) tstRndChance(uint32_t uPercent)
{
    return RTRandAdvU32Ex(g_hTstRand, 0, 99) < uPercent;
}


/** Returns a random value which tends to be at the edge of something. */
static uint32_t tstRndValue(void)
{
    switch (tstRnd(0, 5))
    {
        case 0:  return 0;
        case 1:  return UINT32_MAX;
        case 2:  return RT_BIT_32(tstRnd(0, 31));
        case 3:  return tstRnd(0, 0xff);
        default: return RTRandAdvU32(g_hTstRand);
    }
}


/**
 * Notes down an offset into a BAR where something starts or ends.
 *
 * @param   pInfo               The information about the device model.
 * @param   iBar                The BAR, ignored if there is no such BAR.
 * @param   off                 The offset.
 */
static void tstFuzzAddEdge(PTSTFUZZINFO pInfo, uint32_t iBar, uint32_t off)
{
    if (   iBar < RT_ELEMENTS(pInfo->acEdges)
        && pInfo->acEdges[iBar] < RT_ELEMENTS(pInfo->aaoffEdges[iBar]))
        pInfo->aaoffEdges[iBar][pInfo->acEdges[iBar]++] = off;
}


/**
 * Makes up a device model: mostly something a real device could look like,
 * with a part of it being off limits now and then.
 *
 * @param   pModel              The device model to fill in.
 * @param   pInfo               Where to return what the guest side of the test wants to know about it.
 */
static void tstFuzzModelInit(PTSTVFIODEVCFG pModel, PTSTFUZZINFO pInfo)
{
    RT_ZERO(*pInfo);
    static const uint8_t s_abClasses[] = { 0x01, 0x02, 0x03, 0x04, 0x0c };
    uint8_t const bClass = s_abClasses[tstRnd(0, RT_ELEMENTS(s_abClasses) - 1)];
    tstModelInit(pModel, TST_VFIO_NODE, bClass, bClass == 0x03 && tstRndChance(70) ? 0x00 : (uint8_t)tstRnd(0, 0x80));
    if (tstRndChance(30))
        pModel->cbCfg = 4096;
    if (tstRndChance(5))
    {
        /* The device there is a quirk for. */
        tstModelSetU16(pModel, VBOX_PCI_VENDOR_ID, 0x8086);
        tstModelSetU16(pModel, VBOX_PCI_DEVICE_ID, 0xe223);
    }
    if (tstRndChance(25))
        for (uint32_t off = 0x40; off < 0x100; off++)
            pModel->abCfg[off] = (uint8_t)tstRnd(0, 255);

    /*
     * The BARs, and what VFIO has to say about them.
     */
    for (uint32_t iBar = 0; iBar < 6; iBar++)
    {
        static const uint32_t s_acbMem[] = { 0x100, 0x1000, 0x2000, 0x4000, 0x10000, 0x100000, 0x1800, 0x10 };
        static const uint32_t s_acbIo[]  = { 0x8, 0x20, 0x100, 0x3 };
        uint32_t const uKind = tstRnd(0, 9);
        if (uKind < 5)
            continue;
        if (uKind < 8)
        {
            bool const f64Bit = iBar < 5 && tstRndChance(30);
            tstModelSetBar(pModel, iBar, s_acbMem[tstRnd(0, RT_ELEMENTS(s_acbMem) - 1)],
                             (uint32_t)PCI_ADDRESS_SPACE_MEM
                           | (f64Bit            ? (uint32_t)PCI_ADDRESS_SPACE_BAR64        : 0)
                           | (tstRndChance(30)  ? (uint32_t)PCI_ADDRESS_SPACE_MEM_PREFETCH : 0));
            pModel->aRegions[iBar].fMsixMappable = tstRndChance(30);
            if (f64Bit)
                iBar++;
        }
        else
            tstModelSetBar(pModel, iBar, s_acbIo[tstRnd(0, RT_ELEMENTS(s_acbIo) - 1)], PCI_ADDRESS_SPACE_IO);
    }
    for (uint32_t iBar = 0; iBar < 6; iBar++)
        if (pModel->aRegions[iBar].cb)
        {
            tstFuzzAddEdge(pInfo, iBar, 0);
            tstFuzzAddEdge(pInfo, iBar, (uint32_t)pModel->aRegions[iBar].cb);
            if (tstRndChance(15))
                pModel->aRegions[iBar].fFlags &= ~RT_BIT_32(tstRnd(0, 2)); /* read, write or mmap */
        }

    if (tstRndChance(15))
    {
        static const uint32_t s_acbRom[] = { _64K, _32K, 0x1800, _1M };
        pModel->aRegions[VFIO_PCI_ROM_REGION_INDEX].cb     = s_acbRom[tstRnd(0, RT_ELEMENTS(s_acbRom) - 1)];
        pModel->aRegions[VFIO_PCI_ROM_REGION_INDEX].fFlags = VFIO_REGION_INFO_FLAG_READ;
    }

    bool fExposeVga = false;
    if (bClass == 0x03 && tstRndChance(50))
    {
        fExposeVga = tstRndChance(80);
        pModel->aRegions[VFIO_PCI_VGA_REGION_INDEX].cb     = 0xc0000;
        pModel->aRegions[VFIO_PCI_VGA_REGION_INDEX].fFlags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
    }
    else
        fExposeVga = tstRndChance(3); /* Asked for without the device having them. */

    /*
     * The interrupts.  What VFIO reports doesn't always go with the capabilities.
     */
    pModel->acIrqs[VFIO_PCI_INTX_IRQ_INDEX] = tstRndChance(85) ? 1 : tstRnd(0, 3);
    if (!pModel->acIrqs[VFIO_PCI_INTX_IRQ_INDEX])
        pModel->abCfg[VBOX_PCI_INTERRUPT_PIN] = 0;

    /*
     * The standard capabilities.
     */
    uint8_t  aoffCaps[8];
    uint32_t cCaps     = 0;
    uint32_t offNext   = 0x40;
    uint32_t const cCapsWanted = tstRnd(0, 6);
    while (cCaps < cCapsWanted)
    {
        uint8_t abBody[64];
        size_t  cbBody = 0;
        uint8_t bCapId;
        RT_ZERO(abBody);
        switch (tstRnd(0, 6))
        {
            case 0:
            case 1:
            {
                bCapId = VBOX_PCI_CAP_ID_MSI;
                bool const     f64Bit    = tstRndChance(60);
                bool const     fMaskable = tstRndChance(50);
                uint32_t const cLog2     = tstRndChance(90) ? tstRnd(0, 5) : tstRnd(6, 7); /* 6 and 7 are reserved */
                uint16_t const fCtrl     = (uint16_t)(  (f64Bit    ? VBOX_PCI_MSI_FLAGS_64BIT   : 0)
                                                      | (fMaskable ? VBOX_PCI_MSI_FLAGS_MASKBIT : 0)
                                                      | (cLog2 << 1));
                abBody[0] = RT_LO_U8(fCtrl);
                abBody[1] = RT_HI_U8(fCtrl);
                cbBody = 2 + 4 + (f64Bit ? 4 : 0) + 2 + (fMaskable ? 2 + 4 + 4 : 0);
                pModel->acIrqs[VFIO_PCI_MSI_IRQ_INDEX] = tstRndChance(85) ? RT_BIT_32(RT_MIN(cLog2, 5)) : tstRnd(0, 200);
                break;
            }

            case 2:
            case 3:
            {
                bCapId = VBOX_PCI_CAP_ID_MSIX;
                uint32_t const cVectors = tstRndChance(80) ? tstRnd(1, 64) : tstRnd(1, 2048);
                uint32_t       iBarTbl;
                uint32_t       iBarPba;
                uint32_t       offTbl;
                uint32_t       offPba;
                if (tstRndChance(70))
                {
                    /* The way devices have it: table and pending bits inside a memory BAR which is big
                       enough for them, behind each other or in pages of their own. */
                    uint32_t const cbTbl = cVectors * 16;
                    uint32_t const cbPba = RT_ALIGN_32(cVectors, 64) / 8;
                    iBarTbl = iBarPba = tstRnd(0, 5);
                    for (uint32_t iBar = 0; iBar < 6; iBar++)
                        if (   pModel->aRegions[iBar].cb
                            && !(pModel->abCfg[VBOX_PCI_BASE_ADDRESS_0 + iBar * 4] & PCI_ADDRESS_SPACE_IO))
                        {
                            iBarTbl = iBarPba = iBar;
                            break;
                        }
                    offTbl = tstRnd(0, 3) * 0x1000;
                    offPba = tstRndChance(50) ? offTbl + RT_ALIGN_32(cbTbl, 8) : offTbl + RT_ALIGN_32(cbTbl, 0x1000);
                    uint32_t const cbNeeded = RT_ALIGN_32(offPba + cbPba, 0x1000) + (tstRndChance(50) ? 0x1000 : 0);
                    if (   pModel->aRegions[iBarTbl].cb < cbNeeded
                        || (pModel->abCfg[VBOX_PCI_BASE_ADDRESS_0 + iBarTbl * 4] & PCI_ADDRESS_SPACE_IO))
                    {
                        uint32_t cbBar = _4K;
                        while (cbBar < cbNeeded)
                            cbBar *= 2;
                        tstModelSetBar(pModel, iBarTbl, cbBar, PCI_ADDRESS_SPACE_MEM);
                        tstFuzzAddEdge(pInfo, iBarTbl, cbBar);
                    }
                    pModel->aRegions[iBarTbl].fMsixMappable = tstRndChance(85); /* VFIO has to agree, and mostly does. */
                }
                else
                {
                    iBarTbl = tstRndChance(90) ? tstRnd(0, 5) : tstRnd(6, 7);
                    iBarPba = tstRndChance(80) ? iBarTbl : tstRnd(0, 7);
                    offTbl  = tstRndChance(80) ? tstRnd(0, 16) * 0x1000 : tstRndValue() & ~UINT32_C(7);
                    offPba  = tstRndChance(80)
                            ? offTbl + RT_ALIGN_32(cVectors * 16, tstRndChance(50) ? 0x1000 : 8)
                            : tstRndValue() & ~UINT32_C(7);
                }
                uint16_t const fCtrl    = (uint16_t)(cVectors - 1);
                uint32_t const u32Tbl   = offTbl | iBarTbl;
                uint32_t const u32Pba   = offPba | iBarPba;
                abBody[0] = RT_LO_U8(fCtrl);
                abBody[1] = RT_HI_U8(fCtrl);
                abBody[2] = RT_BYTE1(u32Tbl);
                abBody[3] = RT_BYTE2(u32Tbl);
                abBody[4] = RT_BYTE3(u32Tbl);
                abBody[5] = RT_BYTE4(u32Tbl);
                abBody[6] = RT_BYTE1(u32Pba);
                abBody[7] = RT_BYTE2(u32Pba);
                abBody[8] = RT_BYTE3(u32Pba);
                abBody[9] = RT_BYTE4(u32Pba);
                cbBody = 10;
                pModel->acIrqs[VFIO_PCI_MSIX_IRQ_INDEX] = tstRndChance(85) ? cVectors : tstRnd(0, 4096);
                tstFuzzAddEdge(pInfo, iBarTbl, offTbl);
                tstFuzzAddEdge(pInfo, iBarTbl, offTbl + cVectors * 16);
                tstFuzzAddEdge(pInfo, iBarPba, offPba);
                tstFuzzAddEdge(pInfo, iBarPba, offPba + RT_ALIGN_32(cVectors, 64) / 8);
                break;
            }

            case 4:
                bCapId    = VBOX_PCI_CAP_ID_EXP;
                abBody[0] = (uint8_t)(tstRndChance(85) ? tstRnd(1, 2) : tstRnd(0, 15)); /* the version */
                cbBody    = tstRndChance(80) ? 58 : 18;
                break;

            case 5:
            {
                bCapId = VBOX_PCI_CAP_ID_VNDR;
                uint32_t const cbCap = tstRnd(3, 40);
                abBody[0] = (uint8_t)(tstRndChance(80) ? cbCap : tstRnd(0, 255)); /* the length */
                for (uint32_t i = 1; i < cbCap - 2; i++)
                    abBody[i] = (uint8_t)tstRnd(0, 255);
                cbBody = cbCap - 2;
                break;
            }

            default:
                bCapId = (uint8_t)tstRnd(0, 0x20);
                cbBody = tstRnd(0, 20);
                for (uint32_t i = 0; i < cbBody; i++)
                    abBody[i] = (uint8_t)tstRnd(0, 255);
                break;
        }

        /* Most of the time capabilities follow each other, now and then they end up anywhere. */
        uint32_t const offCap = tstRndChance(85) ? offNext : tstRnd(0x40, 0xfc) & ~UINT32_C(3);
        if (offCap + 2 + cbBody > 256)
            break;
        pModel->abCfg[offCap]     = bCapId;
        pModel->abCfg[offCap + 1] = 0;
        memcpy(&pModel->abCfg[offCap + 2], abBody, cbBody);
        aoffCaps[cCaps++] = (uint8_t)offCap;
        offNext = RT_ALIGN_32(offCap + 2 + (uint32_t)cbBody, 4);
        if (offNext > 0xfc)
            break;
    }
    if (cCaps)
    {
        /* Link them: in the order they were made up, the other way around or mixed up. */
        uint32_t const uOrder = tstRnd(0, 9);
        if (uOrder >= 7)
            for (uint32_t i = 0; i < cCaps / 2; i++)
            {
                uint32_t const iOther = uOrder == 7 ? cCaps - 1 - i : tstRnd(0, cCaps - 1);
                uint8_t const  offTmp = aoffCaps[i];
                aoffCaps[i]      = aoffCaps[iOther];
                aoffCaps[iOther] = offTmp;
            }
        pModel->abCfg[VBOX_PCI_CAPABILITY_LIST] = aoffCaps[0];
        for (uint32_t i = 0; i + 1 < cCaps; i++)
            pModel->abCfg[aoffCaps[i] + 1] = aoffCaps[i + 1];
        pModel->abCfg[VBOX_PCI_STATUS] |= VBOX_PCI_STATUS_CAP_LIST;

        /* A list which isn't one: looping, pointing into the middle of something or to nowhere. */
        if (tstRndChance(12))
        {
            uint8_t * const pbLink = tstRndChance(15)
                                   ? &pModel->abCfg[VBOX_PCI_CAPABILITY_LIST]
                                   : &pModel->abCfg[aoffCaps[tstRnd(0, cCaps - 1)] + 1];
            switch (tstRnd(0, 4))
            {
                case 0:  *pbLink = aoffCaps[tstRnd(0, cCaps - 1)];                       break;
                case 1:  *pbLink = (uint8_t)(aoffCaps[tstRnd(0, cCaps - 1)] + tstRnd(1, 3)); break;
                case 2:  *pbLink = (uint8_t)tstRnd(1, 0x3f);                             break;
                case 3:  *pbLink = 0xff;                                                 break;
                default: *pbLink = (uint8_t)tstRnd(0, 255);                              break;
            }
        }
    }
    else if (tstRndChance(5))
        pModel->abCfg[VBOX_PCI_STATUS] |= VBOX_PCI_STATUS_CAP_LIST; /* Says so and has none. */

    /*
     * The extended capabilities.
     */
    if (   pModel->cbCfg == 4096
        && tstRndChance(80))
    {
        uint32_t       off        = 0x100;
        uint32_t const cExtWanted = tstRnd(1, 8);
        for (uint32_t i = 0; i < cExtWanted; i++)
        {
            uint32_t offNextExt = i + 1 < cExtWanted ? off + tstRnd(1, 16) * 4 : 0;
            if (offNextExt > 0xffc)
                offNextExt = 0;
            if (tstRndChance(8))
                offNextExt = tstRndChance(50) ? 0x100 : tstRnd(0, 0xfff); /* looping or pointing to nowhere */
            tstModelSetU32(pModel, off, tstRnd(1, 0x30) | (tstRnd(1, 3) << 16) | (offNextExt << 20));
            if (off + 8 <= 4096)
                tstModelSetU32(pModel, off + 4, RTRandAdvU32(g_hTstRand));
            if (   offNextExt < 0x100
                || offNextExt > 0xffc
                || (offNextExt & 3))
                break;
            off = offNextExt;
        }
    }

    /*
     * Properties which make the device unusable or awkward.
     */
    pModel->acIrqs[VFIO_PCI_ERR_IRQ_INDEX] = tstRndChance(90) ? 1 : 0;
    pModel->acIrqs[VFIO_PCI_REQ_IRQ_INDEX] = tstRndChance(90) ? 1 : 0;
    pModel->fNoIommu         = tstRndChance(2);
    pModel->fNotPci          = tstRndChance(2);
    pModel->fNoFunctionReset = tstRndChance(20);
    pModel->fNoBusReset      = tstRndChance(30);
    pInfo->fExposeVga        = fExposeVga;
}


/**
 * Lets the guest do one random thing to the device.
 *
 * @param   pModel              The device model.
 * @param   pDev                The PCI function of the fake kernel.
 * @param   pInfo               What is known about the device model.
 */
static void tstFuzzGuestOp(PCTSTVFIODEVCFG pModel, PTSTVFIODEV pDev, PCTSTFUZZINFO pInfo)
{
    static const uint8_t s_acbAccess[] = { 1, 2, 4 };
    uint32_t const       uOp           = tstRnd(0, 99);
    if (uOp < 40)
    {
        /* A config space write. */
        unsigned const cb  = s_acbAccess[tstRnd(0, RT_ELEMENTS(s_acbAccess) - 1)];
        uint32_t       off;
        uint32_t       u32 = tstRndValue();
        switch (tstRnd(0, 9))
        {
            case 0:
            case 1:
            case 2:
                /* The command register, often turning on what makes the device do things. */
                off = VBOX_PCI_COMMAND;
                if (tstRndChance(70))
                    u32 = tstRnd(0, 7) | (tstRndChance(20) ? VBOX_PCI_COMMAND_INTX_DISABLE : 0);
                break;

            case 3:
            case 4:
            {
                /* A BAR: sizing it, disabling it or giving it the address reserved for it here. */
                uint32_t const iBar = tstRnd(0, 5);
                off = VBOX_PCI_BASE_ADDRESS_0 + iBar * 4;
                if (tstRndChance(85))
                    u32 = tstRndChance(20)                                ? UINT32_MAX
                        : tstRndChance(15)                                ? UINT32_C(0)
                        : (pModel->abCfg[off] & PCI_ADDRESS_SPACE_IO)     ? TST_FUZZ_IO_PORT(iBar)
                        :                                                   TST_FUZZ_MEM_ADDR(iBar);
                break;
            }

            case 5:
            case 6:
            case 7:
                /* Where the capabilities are. */
                off = tstRnd(0x34, 0xff);
                break;

            default:
                off = tstRnd(0, pModel->cbCfg - 1);
                break;
        }
        if (tstRndChance(90))
            off &= ~(uint32_t)(cb - 1);
        if (off + cb <= 4096)
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, off, u32, cb);
    }
    else if (uOp < 60)
    {
        /* A config space read. */
        unsigned const cb  = s_acbAccess[tstRnd(0, RT_ELEMENTS(s_acbAccess) - 1)];
        uint32_t       off = tstRnd(0, tstRndChance(50) ? 0xff : 4095);
        if (tstRndChance(90))
            off &= ~(uint32_t)(cb - 1);
        if (off + cb <= 4096)
            tstPdmPciCfgRead(0, TST_PCI_DEVFN, off, cb);
    }
    else if (uOp < 82)
    {
        /*
         * An access to where a BAR might be mapped: around where something starts or
         * ends in it, around page boundaries or just anywhere.
         */
        uint32_t const iBar   = tstRnd(0, 5);
        uint64_t const cbBar  = RT_MAX(pModel->aRegions[iBar].cb, 16);
        bool const     fEdge  = pInfo->acEdges[iBar] && tstRndChance(45);
        uint32_t const offBar = fEdge
                              ? pInfo->aaoffEdges[iBar][tstRnd(0, pInfo->acEdges[iBar] - 1)] + tstRnd(0, 16) - 8
                              : tstRndChance(50)
                              ? (uint32_t)(tstRnd(0, 16) * 0x1000 + tstRnd(0, 0x40) - 0x20) % (uint32_t)cbBar
                              : tstRnd(0, (uint32_t)cbBar - 1);
        if (pModel->abCfg[VBOX_PCI_BASE_ADDRESS_0 + iBar * 4] & PCI_ADDRESS_SPACE_IO)
        {
            unsigned const cb  = s_acbAccess[tstRnd(0, RT_ELEMENTS(s_acbAccess) - 1)];
            uint32_t       u32 = 0;
            if (tstRndChance(50))
                tstPdmIoPortRead((RTIOPORT)(TST_FUZZ_IO_PORT(iBar) + (offBar & 0xff)), &u32, cb);
            else
                tstPdmIoPortWrite((RTIOPORT)(TST_FUZZ_IO_PORT(iBar) + (offBar & 0xff)), tstRndValue(), cb);
        }
        else
        {
            static const uint8_t s_acbMmio[] = { 1, 2, 4, 8 };
            unsigned const cb = s_acbMmio[tstRnd(0, RT_ELEMENTS(s_acbMmio) - 1)];
            uint64_t       u64 = RT_MAKE_U64(tstRndValue(), tstRndValue());
            RTGCPHYS const GCPhys = TST_FUZZ_MEM_ADDR(iBar)
                                  + (tstRndChance(fEdge ? 40 : 90) ? (offBar & ~(uint32_t)(cb - 1)) : offBar);
            if (tstRndChance(50))
                tstPdmPhysRead(GCPhys, &u64, cb);
            else
                tstPdmPhysWrite(GCPhys, &u64, cb);
        }
    }
    else if (uOp < 90)
    {
        /* The device raises an interrupt.  Which fails if it isn't set up, and that is fine. */
        uint32_t const uIrqIndex = tstRnd(VFIO_PCI_INTX_IRQ_INDEX, VFIO_PCI_MSIX_IRQ_INDEX);
        if (pModel->acIrqs[uIrqIndex])
            tstVfioKrnlDevTriggerIrq(pDev, uIrqIndex, tstRnd(0, RT_MIN(pModel->acIrqs[uIrqIndex], 2048) - 1));
    }
    else if (uOp < 93)
    {
        if (pInfo->fExposeVga)
        {
            /* The legacy VGA ranges. */
            uint64_t u64 = RT_MAKE_U64(tstRndValue(), tstRndValue());
            uint32_t u32 = 0;
            switch (tstRnd(0, 4))
            {
                case 0:  tstPdmIoPortRead((RTIOPORT)tstRnd(0x3b0, 0x3df), &u32, 1);                            break;
                case 1:  tstPdmIoPortWrite((RTIOPORT)tstRnd(0x3b0, 0x3df), tstRndValue(), 1);                  break;
                case 2:  tstPdmPhysRead(tstRnd(0xa0000, 0xbfff8), &u64, 4);                                    break;
                case 3:  tstPdmPhysWrite(tstRnd(0xa0000, 0xbfff8), &u64, 4);                                   break;
                default: tstPdmPhysFill(tstRnd(0xa0000, 0xbffff), tstRndValue(), 1U << tstRnd(0, 2), tstRnd(0, 0x10000)); break;
            }
        }
    }
    else if (uOp < 96)
        tstPdmReset();
    else if (uOp == 99 && tstRndChance(30))
    {
        /* The host takes the device back for good or reports an error of it.  Whatever comes after
           this races with the device letting go of it. */
        if (tstRndChance(60))
            tstVfioKrnlDevRemove(pDev);
        else
            tstVfioKrnlDevSignalError(pDev);
        g_fTstFuzzRemoved = true;
    }
    else if (!g_fTstFuzzRemoved)
    {
        /* The device stops answering for a while. */
        tstVfioKrnlDevSetGone(pDev, true);
        for (uint32_t i = 0, cOps = tstRnd(1, 8); i < cOps; i++)
        {
            uint32_t u32 = 0;
            tstPdmPciCfgRead(0, TST_PCI_DEVFN, tstRnd(0, 0xff) & ~UINT32_C(3), 4);
            tstPdmPciCfgWrite(0, TST_PCI_DEVFN, tstRnd(0, 0xff) & ~UINT32_C(3), tstRndValue(), 4);
            tstPdmPhysRead(TST_FUZZ_MEM_ADDR(tstRnd(0, 5)) + (tstRnd(0, 0xfff) & ~UINT32_C(3)), &u32, 4);
        }
        tstVfioKrnlDevSetGone(pDev, false);
    }
}


/**
 * Random device models with random guest accesses.
 *
 * Whatever the device looks like, the VM is either constructed or not, with
 * nothing left behind in the latter case, and whatever the guest does, the
 * VM process survives it and the host kernel sees no request it would refuse
 * for being out of bounds.  This is of most use with the sanitizers.
 *
 * The number of models and the seed can be set with the environment variables
 * TSTDEVPCIVFIO_FUZZ_MODELS and TSTDEVPCIVFIO_FUZZ_SEED, the seed of a model
 * which fails is given in the output.
 */
static void tstFuzz(void)
{
    RTTestISub("Random device models and guest accesses");

    uint32_t    cModels = 150;
    uint32_t    uSeed   = UINT32_C(0x20261111);
    const char *psz     = RTEnvGet("TSTDEVPCIVFIO_FUZZ_MODELS");
    if (psz)
        cModels = RTStrToUInt32(psz);
    psz = RTEnvGet("TSTDEVPCIVFIO_FUZZ_SEED");
    if (psz)
        uSeed = RTStrToUInt32(psz);

    int rc = RTRandAdvCreateParkMiller(&g_hTstRand);
    RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return;

    uint32_t cConstructed = 0;
    for (uint32_t iModel = 0; iModel < cModels; iModel++)
    {
        uint32_t const uModelSeed    = uSeed + iModel;
        uint32_t const cErrorsBefore = RTTestIErrorCount();
        RTRandAdvSeed(g_hTstRand, uModelSeed);
        g_fTstFuzzRemoved = false;

        static TSTVFIODEVCFG s_Model; /* Too big for the stack. */
        TSTFUZZINFO Info;
        tstFuzzModelInit(&s_Model, &Info);
        PTSTVFIODEV pDev = NULL;
        g_fTstExposeVga     = Info.fExposeVga;
        g_fTstInterceptMmio = tstRndChance(10);
        g_fTstLegacy        = tstRndChance(10);
        rc = tstFixtureSetup(&s_Model, 1, &pDev, NULL);
        g_fTstExposeVga     = false;
        g_fTstInterceptMmio = false;
        g_fTstLegacy        = false;
        if (RT_SUCCESS(rc))
        {
            cConstructed++;
            uint64_t const cbMapped = tstVfioKrnlGetDmaMappedBytes();
            RTTESTI_CHECK_MSG(cbMapped == TST_RAM_SIZE - TSTPDM_RAM_HOLE_SIZE, ("%RU64 bytes mapped for DMA\n", cbMapped));
            tstPdmPowerOn();

            /* Most guests start out with setting the device up the way a firmware does it. */
            if (tstRndChance(80))
            {
                for (uint32_t iBar = 0; iBar < 6; iBar++)
                    tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_BASE_ADDRESS_0 + iBar * 4,
                                        (s_Model.abCfg[VBOX_PCI_BASE_ADDRESS_0 + iBar * 4] & PCI_ADDRESS_SPACE_IO)
                                      ? TST_FUZZ_IO_PORT(iBar) : TST_FUZZ_MEM_ADDR(iBar), 4);
                tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND,
                                  VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
            }

            for (uint32_t iOp = 0, cOps = tstRnd(20, 400); iOp < cOps; iOp++)
                tstFuzzGuestOp(&s_Model, pDev, &Info);

            /* The guest never changes what is mapped for DMA, and the identity of the device stays what it is. */
            RTTESTI_CHECK_MSG(tstVfioKrnlGetDmaMappedBytes() == cbMapped,
                              ("%RU64 bytes mapped for DMA, was %RU64\n", tstVfioKrnlGetDmaMappedBytes(), cbMapped));
            uint32_t const idVendor = tstPdmPciCfgRead(0, TST_PCI_DEVFN, VBOX_PCI_VENDOR_ID, 2);
            RTTESTI_CHECK_MSG(   idVendor == RT_MAKE_U16(s_Model.abCfg[VBOX_PCI_VENDOR_ID], s_Model.abCfg[VBOX_PCI_VENDOR_ID + 1])
                              || (g_fTstFuzzRemoved && idVendor == UINT32_C(0xffff)),
                              ("vendor ID %#RX32\n", idVendor));
        }
        else
            RTTESTI_CHECK_MSG(tstVfioKrnlGetDmaMappedBytes() == 0,
                              ("%RU64 bytes mapped for DMA with the VM construction having failed\n",
                               tstVfioKrnlGetDmaMappedBytes()));

        /* Nothing the host kernel would refuse for being out of bounds. */
        RTTESTI_CHECK_MSG(tstVfioKrnlGetViolationCount() == 0,
                          ("%u request(s) to the kernel which are out of bounds\n", tstVfioKrnlGetViolationCount()));
        tstFixtureTeardown();

        if (RTTestIErrorCount() != cErrorsBefore)
        {
            RTTestIFailureDetails("the model with seed %#RX32 failed (construction: %Rrc); "
                                  "run with TSTDEVPCIVFIO_FUZZ_SEED=%u TSTDEVPCIVFIO_FUZZ_MODELS=1 to get it again\n",
                                  uModelSeed, rc, uModelSeed);
            break;
        }
    }
    RTTestIPrintf(RTTESTLVL_ALWAYS, "%u device model(s) starting with seed %#RX32, %u of them made it into a VM\n",
                  cModels, uSeed, cConstructed);

    RTRandAdvDestroy(g_hTstRand);
    g_hTstRand = NIL_RTRAND;
}


/*
 *
 * The PCI express root port with a hot-plug capable slot.
 *
 */

/** The device number of the root port on the root bus. */
#define TSTPORT_DEV_NO          24
/** The device and function number of the root port. */
#define TSTPORT_DEVFN           (TSTPORT_DEV_NO << 3)
/** The bus number the guest gives to the secondary bus of the root port. */
#define TSTPORT_SEC_BUS         1
/** Config space offset of the MSI capability of the root port. */
#define TSTPORT_MSI_CAP         0x80
/** Config space offset of the PCI express capability of the root port. */
#define TSTPORT_EXP_CAP         0xa0
/** Size of the PCI express capability of the root port. */
#define TSTPORT_EXP_CAP_SIZE    0x3c
/** @name Registers of the PCI express capability, as config space offsets of the root port.
 * @{ */
#define TSTPORT_EXP_FLAGS       (TSTPORT_EXP_CAP + 0x02)
#define TSTPORT_LNKCAP          (TSTPORT_EXP_CAP + 0x0c)
#define TSTPORT_LNKCTL          (TSTPORT_EXP_CAP + 0x10)
#define TSTPORT_LNKSTA          (TSTPORT_EXP_CAP + 0x12)
#define TSTPORT_SLTCAP          (TSTPORT_EXP_CAP + 0x14)
#define TSTPORT_SLTCTL          (TSTPORT_EXP_CAP + 0x18)
#define TSTPORT_SLTSTA          (TSTPORT_EXP_CAP + 0x1a)
/** @} */
/** Slot control of an empty slot after a reset: no power, both indicators off. */
#define TSTPORT_SLTCTL_EMPTY    UINT16_C(0x07c0)
/** Slot control of an occupied slot after a reset: power and power indicator on, attention indicator off. */
#define TSTPORT_SLTCTL_OCCUPIED UINT16_C(0x01c0)
/** What a guest adds to the slot control to get told about everything the slot does. */
#define TSTPORT_SLTCTL_ENABLES  (  VBOX_PCI_EXP_SLTCTL_ATNB | VBOX_PCI_EXP_SLTCTL_PRSD | VBOX_PCI_EXP_SLTCTL_CMDC \
                                 | VBOX_PCI_EXP_SLTCTL_HPIE | VBOX_PCI_EXP_SLTCTL_LLCHG)
/** The message address the guest gives the root port for its interrupt. */
#define TSTPORT_MSI_ADDR        UINT32_C(0xfee01000)
/** The message data the guest gives the root port for its interrupt. */
#define TSTPORT_MSI_DATA        UINT16_C(0x0051)

/**
 * The registers of the PCI express capability of a root port with a hot-plug
 * capable slot and the bits in them the guest can do something about.
 */
static const struct
{
    /** Offset into the capability. */
    uint8_t     off;
    /** Size of the register in bytes. */
    uint8_t     cb;
    /** Bits the guest can set and clear. */
    uint32_t    fRw;
    /** Bits the guest clears by writing a one, none of which ever gets set but those of the slot status. */
    uint32_t    fRw1c;
    /** Read-only bits which go with what happens to the slot. */
    uint32_t    fDynamic;
} g_aTstPortRegs[] =
{
    { 0x00, 2, 0,      0,                    0 }, /* Capability ID, next capability. */
    { 0x02, 2, 0,      0,                    0 }, /* PCI express capabilities. */
    { 0x04, 4, 0,      0,                    0 }, /* Device capabilities. */
    { 0x08, 2, 0x78ff, 0,                    0 }, /* Device control. */
    { 0x0a, 2, 0,      0x000f,               0 }, /* Device status. */
    { 0x0c, 4, 0,      0,                    0 }, /* Link capabilities. */
    { 0x10, 2, 0x0cd3, 0,                    0 }, /* Link control. */
    { 0x12, 2, 0,      0xc000,               VBOX_PCI_EXP_LNKSTA_DL_ACT }, /* Link status. */
    { 0x14, 4, 0,      0,                    0 }, /* Slot capabilities. */
    { 0x18, 2, 0x17fb, 0,                    0 }, /* Slot control. */
    { 0x1a, 2, 0,      0x011f,               VBOX_PCI_EXP_SLTSTA_PRES }, /* Slot status. */
    { 0x1c, 2, 0x000f, 0,                    0 }, /* Root control. */
    { 0x1e, 2, 0,      0,                    0 }, /* Root capabilities. */
    { 0x20, 4, 0,      UINT32_C(0x00010000), 0 }, /* Root status. */
    { 0x24, 4, 0,      0,                    0 }, /* Device capabilities 2. */
    { 0x28, 2, 0,      0,                    0 }, /* Device control 2. */
    { 0x2a, 2, 0,      0,                    0 }, /* Device status 2. */
    { 0x2c, 4, 0,      0,                    0 }, /* Link capabilities 2. */
    { 0x30, 2, 0xffbf, 0,                    0 }, /* Link control 2. */
    { 0x32, 2, 0,      0,                    0 }, /* Link status 2. */
    { 0x34, 4, 0,      0,                    0 }, /* Slot capabilities 2. */
    { 0x38, 2, 0,      0,                    0 }, /* Slot control 2. */
    { 0x3a, 2, 0,      0,                    0 }  /* Slot status 2. */
};

/** What the tests playing the device in the hot-plug capable slot got told about the link. */
static struct
{
    /** Number of calls. */
    uint32_t    cCalls;
    /** The state of the link according to the last call, down before the first one. */
    bool        fLinkUp;
    /** The device instance the last call came with. */
    PPDMDEVINS  pDevIns;
} g_TstPortLink;


/** @callback_method_impl{FNDEVPCIHPSLOTLINK} */
static DECLCALLBACK(void) tstPortLinkChanged(PPDMDEVINS pDevIns, bool fLinkUp)
{
    g_TstPortLink.cCalls++;
    g_TstPortLink.fLinkUp = fLinkUp;
    g_TstPortLink.pDevIns = pDevIns;
}


/**
 * Sets up a VM with the ICH9 PCI bus and a bridge on it, a PCI express root
 * port usually, and optionally the fake kernel with a PCI function and a
 * passthrough device for it behind the bridge.
 *
 * Completing the VM construction is left to the caller.  tstFixtureTeardown()
 * has to be called regardless of the result.
 *
 * @returns Status code of the constructor which failed, or of whatever failed before.
 * @param   pszPortType         The "ExpressPortType" of the bridge, NULL for a bridge which is not PCI express.
 * @param   fHotPlug            Whether the bridge is configured to have a hot-plug capable slot.
 * @param   pModel              The PCI function to put behind the bridge, NULL for none.
 * @param   ppDev               Where to return the fake kernel handle of the function, NULL if there is none.
 * @param   ppDevInsPort        Where to return the device instance of the bridge.
 * @param   ppDevInsDev         Where to return the device instance of the passthrough device, optional.
 */
static int tstPortFixtureSetup(const char *pszPortType, bool fHotPlug, PCTSTVFIODEVCFG pModel, PTSTVFIODEV *ppDev,
                               PPDMDEVINS *ppDevInsPort, PPDMDEVINS *ppDevInsDev)
{
    RT_ZERO(g_TstPortLink);
    *ppDevInsPort = NULL;
    if (ppDevInsDev)
        *ppDevInsDev = NULL;

    int rc = tstVfioKrnlInit();
    RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return rc;
    if (pModel)
    {
        rc = tstVfioKrnlDevAdd(pModel, ppDev);
        RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
        if (RT_FAILURE(rc))
            return rc;
    }

    rc = tstPdmInit(TST_RAM_SIZE);
    RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return rc;

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

    /* The bridge. */
    pCfgInst = tstCfgNodeCreate(NULL, "0");
    RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgInst, "PCIBusNo", 0), VINF_SUCCESS);
    RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgInst, "PCIDeviceNo", TSTPORT_DEV_NO), VINF_SUCCESS);
    RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgInst, "PCIFunctionNo", 0), VINF_SUCCESS);
    pCfg = tstCfgNodeCreate(pCfgInst, "Config");
    if (pszPortType)
    {
        RTTESTI_CHECK_RC(tstCfgSetInteger(pCfg, "ExpressEnabled", 1), VINF_SUCCESS);
        RTTESTI_CHECK_RC(tstCfgSetString(pCfg, "ExpressPortType", pszPortType), VINF_SUCCESS);
    }
    if (fHotPlug)
        RTTESTI_CHECK_RC(tstCfgSetInteger(pCfg, "HotPlug", 1), VINF_SUCCESS);
    rc = tstPdmDevCreate(&g_DevicePciIch9Bridge, 0 /*iInstance*/, pCfgInst, ppDevInsPort);
    if (RT_FAILURE(rc))
        return rc;

    /* The passthrough device: the one thing on the secondary bus, which is the second PCI bus of the VM. */
    if (pModel)
    {
        pCfgInst = tstCfgNodeCreate(NULL, "0");
        RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgInst, "PCIBusNo", 1), VINF_SUCCESS);
        RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgInst, "PCIDeviceNo", 0), VINF_SUCCESS);
        RTTESTI_CHECK_RC(tstCfgSetInteger(pCfgInst, "PCIFunctionNo", 0), VINF_SUCCESS);
        pCfg = tstCfgNodeCreate(pCfgInst, "Config");
        PCFGMNODE pCfgFun = tstCfgNodeCreate(pCfg, "Fun0");
        RTTESTI_CHECK_RC(tstCfgSetString(pCfgFun, "VfioPath", pModel->szNode), VINF_SUCCESS);
        rc = tstPdmDevCreate(&g_DevicePciVfio, 0 /*iInstance*/, pCfgInst, ppDevInsDev);
    }
    return rc;
}


/** Reads from the config space of the root port like the guest does. */
static uint32_t tstPortCfgRead(uint32_t offReg, unsigned cb)
{
    return tstPdmPciCfgRead(0, TSTPORT_DEVFN, offReg, cb);
}


/** Writes to the config space of the root port like the guest does. */
static void tstPortCfgWrite(uint32_t offReg, uint32_t u32Value, unsigned cb)
{
    tstPdmPciCfgWrite(0, TSTPORT_DEVFN, offReg, u32Value, cb);
}


/** Gives the root port its bus numbers, which is what a firmware does first. */
static void tstPortGuestSetBusNumbers(void)
{
    tstPortCfgWrite(VBOX_PCI_PRIMARY_BUS, 0, 1);
    tstPortCfgWrite(VBOX_PCI_SECONDARY_BUS, TSTPORT_SEC_BUS, 1);
    tstPortCfgWrite(VBOX_PCI_SUBORDINATE_BUS, TSTPORT_SEC_BUS, 1);
}


/** Sets up and enables MSI of the root port, which is what a guest does to get interrupts from it. */
static void tstPortGuestEnableMsi(void)
{
    tstPortCfgWrite(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_ADDRESS_LO, TSTPORT_MSI_ADDR, 4);
    tstPortCfgWrite(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_ADDRESS_HI, 0, 4);
    tstPortCfgWrite(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_DATA_64, TSTPORT_MSI_DATA, 2);
    tstPortCfgWrite(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_CONTROL,
                    tstPortCfgRead(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_CONTROL, 2) | VBOX_PCI_MSI_FLAGS_ENABLE, 2);
}


/**
 * Checks what the guest and the device in the slot see of the hot-plug capable
 * slot, and how many interrupts the root port has sent so far.
 *
 * @param   uLine               The line of the caller.
 * @param   uSltStaExpected     The expected slot status.
 * @param   fLinkUpExpected     Whether the link is expected to be up.
 * @param   cLinkCallsExpected  How often the device in the slot is expected to have been told about the link.
 * @param   cMsisExpected       The expected number of interrupts.
 */
static void tstPortCheck(unsigned uLine, uint16_t uSltStaExpected, bool fLinkUpExpected, uint32_t cLinkCallsExpected,
                         uint32_t cMsisExpected)
{
    uint32_t const uSltSta = tstPortCfgRead(TSTPORT_SLTSTA, 2);
    if (uSltSta != uSltStaExpected)
        RTTestIFailed("line %u: slot status %#RX32, expected %#RX16\n", uLine, uSltSta, uSltStaExpected);

    bool const fLinkUp = RT_BOOL(tstPortCfgRead(TSTPORT_LNKSTA, 2) & VBOX_PCI_EXP_LNKSTA_DL_ACT);
    if (fLinkUp != fLinkUpExpected)
        RTTestIFailed("line %u: the guest sees the link %s\n", uLine, fLinkUp ? "up" : "down");
    if (g_TstPortLink.cCalls != cLinkCallsExpected)
        RTTestIFailed("line %u: the device in the slot was told %u time(s) about the link, expected %u\n",
                      uLine, g_TstPortLink.cCalls, cLinkCallsExpected);
    else if (g_TstPortLink.fLinkUp != fLinkUpExpected)
        RTTestIFailed("line %u: the device in the slot was told that the link is %s\n",
                      uLine, g_TstPortLink.fLinkUp ? "up" : "down");

    uint32_t const cMsis = tstPdmMsiGetCount();
    if (cMsis != cMsisExpected)
        RTTestIFailed("line %u: %u interrupt(s) from the root port, expected %u\n", uLine, cMsis, cMsisExpected);
    else if (cMsis)
    {
        TSTPDMMSI Msi;
        if (   !tstPdmMsiGet(cMsis - 1, &Msi)
            || Msi.uBusDevFn != PCIBDF_MAKE(0, TSTPORT_DEVFN)
            || Msi.uAddr != TSTPORT_MSI_ADDR
            || Msi.uData != TSTPORT_MSI_DATA)
            RTTestIFailed("line %u: the last interrupt is not what the guest asked for: sender %#RX16 address %#RX64 data %#RX32\n",
                          uLine, Msi.uBusDevFn, Msi.uAddr, Msi.uData);
    }
}


/**
 * The bridge without and with a hot-plug capable slot: what it tells the guest
 * about itself and what the guest can change of that.
 */
static void tstPortRegisters(void)
{
    RTTestISub("Root port: registers of the hot-plug slot");

    /*
     * Not asked for: the port is what it always was.
     */
    PPDMDEVINS pDevInsPort = NULL;
    int rc = tstPortFixtureSetup("RootCmplxRootPort", false /*fHotPlug*/, NULL, NULL, &pDevInsPort, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("ich9pcibridge: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        RTTESTI_CHECK_RC(tstPdmInitComplete(), VINF_SUCCESS);
        tstPdmPowerOn();
        RTTESTI_CHECK(tstPortCfgRead(VBOX_PCI_CAPABILITY_LIST, 1) == TSTPORT_EXP_CAP);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_EXP_CAP, 2) == VBOX_PCI_CAP_ID_EXP);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_MSI_CAP, 4) == 0);
        RTTESTI_CHECK(!(tstPortCfgRead(TSTPORT_EXP_FLAGS, 2) & VBOX_PCI_EXP_FLAGS_SLOT));
        RTTESTI_CHECK(!(tstPortCfgRead(TSTPORT_LNKCAP, 4) & VBOX_PCI_EXP_LNKCAP_DLLA));
        RTTESTI_CHECK_MSG(tstPortCfgRead(TSTPORT_SLTCAP, 4) == (75 << 7), ("%#RX32\n", tstPortCfgRead(TSTPORT_SLTCAP, 4)));
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTCTL, 4) == 0);
    }
    tstFixtureTeardown();

    /*
     * Asked for with a bridge which can't have it.
     */
    static const char * const s_apszPortTypes[] = { NULL, "ExpressSwDownstream", "RootCmplxIntEp" };
    for (unsigned i = 0; i < RT_ELEMENTS(s_apszPortTypes); i++)
    {
        rc = tstPortFixtureSetup(s_apszPortTypes[i], true /*fHotPlug*/, NULL, NULL, &pDevInsPort, NULL);
        RTTESTI_CHECK_MSG(rc == VERR_NOT_SUPPORTED, ("variant %u: %Rrc\n", i, rc));
        RTTESTI_CHECK_MSG(strstr(tstPdmGetLastError(), "HotPlug") != NULL, ("variant %u: '%s'\n", i, tstPdmGetLastError()));
        tstFixtureTeardown();
    }

    /*
     * A root port with a hot-plug capable slot.
     */
    rc = tstPortFixtureSetup("RootCmplxRootPort", true /*fHotPlug*/, NULL, NULL, &pDevInsPort, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("ich9pcibridge with hot-plug: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        RTTESTI_CHECK_RC(tstPdmInitComplete(), VINF_SUCCESS);
        tstPdmPowerOn();
        tstPortGuestSetBusNumbers();

        /* The capabilities: MSI, then PCI express.  MSI is the only interrupt the port has. */
        RTTESTI_CHECK(tstPortCfgRead(VBOX_PCI_STATUS, 2) & VBOX_PCI_STATUS_CAP_LIST);
        RTTESTI_CHECK(tstPortCfgRead(VBOX_PCI_CAPABILITY_LIST, 1) == TSTPORT_MSI_CAP);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_MSI_CAP, 2) == (VBOX_PCI_CAP_ID_MSI | (TSTPORT_EXP_CAP << 8)));
        RTTESTI_CHECK(   (tstPortCfgRead(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_CONTROL, 2) & (VBOX_PCI_MSI_FLAGS_64BIT | VBOX_PCI_MSI_FLAGS_ENABLE))
                      == VBOX_PCI_MSI_FLAGS_64BIT);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_EXP_CAP, 2) == VBOX_PCI_CAP_ID_EXP);
        RTTESTI_CHECK(tstPortCfgRead(VBOX_PCI_INTERRUPT_PIN, 1) == 0);

        /* A root port with a slot the guest can do native hot-plug with, the slot numbers start at one. */
        uint32_t u32 = tstPortCfgRead(TSTPORT_EXP_FLAGS, 2);
        RTTESTI_CHECK_MSG(u32 == (2 | (VBOX_PCI_EXP_TYPE_ROOT_PORT << 4) | VBOX_PCI_EXP_FLAGS_SLOT), ("%#RX32\n", u32));
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_LNKCAP, 4) & VBOX_PCI_EXP_LNKCAP_DLLA);
        u32 = tstPortCfgRead(TSTPORT_SLTCAP, 4);
        RTTESTI_CHECK_MSG(u32 == (  VBOX_PCI_EXP_SLTCAP_ATNB | VBOX_PCI_EXP_SLTCAP_PWRC | VBOX_PCI_EXP_SLTCAP_ATNI
                                  | VBOX_PCI_EXP_SLTCAP_PWRI | VBOX_PCI_EXP_SLTCAP_HPS  | VBOX_PCI_EXP_SLTCAP_HPC
                                  | (75 << 7) | (1 << 19)),
                          ("slot capabilities %#RX32\n", u32));

        /* Nothing was configured to be behind the port: the slot is empty and has no power. */
        RTTESTI_CHECK_MSG(tstPortCfgRead(TSTPORT_SLTCTL, 2) == TSTPORT_SLTCTL_EMPTY, ("%#RX32\n", tstPortCfgRead(TSTPORT_SLTCTL, 2)));
        tstPortCheck(__LINE__, 0 /*uSltSta*/, false /*fLinkUp*/, 0 /*cLinkCalls*/, 0 /*cMsis*/);
        RTTESTI_CHECK(!devpciR3HotPlugSlotIsLinkUp(pDevInsPort));

        /*
         * What the guest can change: ones into everything, then zeros.  The slot
         * status is written after the slot control, which takes the command
         * completed event away the first time and leaves it there the second time.
         */
        uint32_t au32Initial[RT_ELEMENTS(g_aTstPortRegs)];
        for (unsigned i = 0; i < RT_ELEMENTS(g_aTstPortRegs); i++)
            au32Initial[i] = tstPortCfgRead(TSTPORT_EXP_CAP + g_aTstPortRegs[i].off, g_aTstPortRegs[i].cb);
        for (unsigned iPass = 0; iPass < 2; iPass++)
        {
            for (unsigned i = 0; i < RT_ELEMENTS(g_aTstPortRegs); i++)
                tstPortCfgWrite(TSTPORT_EXP_CAP + g_aTstPortRegs[i].off, iPass == 0 ? UINT32_MAX : 0, g_aTstPortRegs[i].cb);
            for (unsigned i = 0; i < RT_ELEMENTS(g_aTstPortRegs); i++)
            {
                uint32_t u32Expected = (au32Initial[i] & ~g_aTstPortRegs[i].fRw) | (iPass == 0 ? g_aTstPortRegs[i].fRw : 0);
                if (iPass == 1 && g_aTstPortRegs[i].off == 0x1a)
                    u32Expected |= VBOX_PCI_EXP_SLTSTA_CMDC;
                u32 = tstPortCfgRead(TSTPORT_EXP_CAP + g_aTstPortRegs[i].off, g_aTstPortRegs[i].cb);
                RTTESTI_CHECK_MSG(u32 == u32Expected,
                                  ("pass %u, register %#x: %#RX32, expected %#RX32\n", iPass, g_aTstPortRegs[i].off, u32, u32Expected));
            }
        }
        RTTESTI_CHECK(tstPdmMsiGetCount() == 0);

        /* An event goes away by writing a one to it. */
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_CMDC, 2);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTSTA, 2) == 0);

        /* A byte of the slot control is a command as well, and a byte of the status does for the event. */
        tstPortCfgWrite(TSTPORT_SLTCTL + 1, TSTPORT_SLTCTL_EMPTY >> 8, 1);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTCTL, 2) == (TSTPORT_SLTCTL_EMPTY & 0xff00));
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTSTA, 2) == VBOX_PCI_EXP_SLTSTA_CMDC);
        tstPortCfgWrite(TSTPORT_SLTSTA + 1, 0xff, 1);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTSTA, 2) == VBOX_PCI_EXP_SLTSTA_CMDC);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_CMDC, 1);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTSTA, 2) == 0);

        /* Control and status in one go: the event of before is gone, the one of this command is there. */
        tstPortCfgWrite(TSTPORT_SLTCTL, TSTPORT_SLTCTL_EMPTY, 2);
        tstPortCfgWrite(TSTPORT_SLTCTL, TSTPORT_SLTCTL_EMPTY | ((uint32_t)VBOX_PCI_EXP_SLTSTA_CMDC << 16), 4);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTCTL, 4) == (TSTPORT_SLTCTL_EMPTY | ((uint32_t)VBOX_PCI_EXP_SLTSTA_CMDC << 16)));
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_CMDC, 2);

        /* Accesses which are not aligned: across the start and across the end of the capability. */
        tstPortCfgWrite(TSTPORT_EXP_CAP + TSTPORT_EXP_CAP_SIZE - 2, UINT32_MAX, 4);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_EXP_CAP + TSTPORT_EXP_CAP_SIZE - 2, 2) == 0);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_EXP_CAP + TSTPORT_EXP_CAP_SIZE, 2) == 0xffff);
        tstPortCfgWrite(TSTPORT_EXP_CAP - 2, UINT32_C(0x5aa5c33c), 4);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_EXP_CAP - 2, 2) == 0xc33c);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_EXP_CAP, 2) == VBOX_PCI_CAP_ID_EXP);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTSTA, 2) == 0);

        /* The rest of the config space works like before. */
        tstPortCfgWrite(VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
        RTTESTI_CHECK(   (tstPortCfgRead(VBOX_PCI_COMMAND, 2) & (VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER))
                      == (VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER));
        RTTESTI_CHECK(tstPortCfgRead(VBOX_PCI_SECONDARY_BUS, 1) == TSTPORT_SEC_BUS);
        RTTESTI_CHECK(tstPortCfgRead(VBOX_PCI_VENDOR_ID, 2) == 0x8086);
        RTTESTI_CHECK(tstPdmMsiGetCount() == 0);
    }
    tstFixtureTeardown();
}


/**
 * A device being put into the hot-plug capable slot and taken out of it, the
 * attention button, the guest switching the power and disabling the link: what
 * the guest sees, when it is interrupted and what the device gets told.
 */
static void tstPortSlotEvents(void)
{
    RTTestISub("Root port: slot events and interrupts");

    PPDMDEVINS pDevInsPort = NULL;
    int rc = tstPortFixtureSetup("RootCmplxRootPort", true /*fHotPlug*/, NULL, NULL, &pDevInsPort, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("ich9pcibridge with hot-plug: %Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        /* The test is the device which comes and goes, there can only be one. */
        RTTESTI_CHECK_RC(devpciR3HotPlugSlotRegisterClient(pDevInsPort, pDevInsPort, tstPortLinkChanged, false /*fPresent*/),
                         VINF_SUCCESS);
        RTTESTI_CHECK_RC(devpciR3HotPlugSlotRegisterClient(pDevInsPort, pDevInsPort, tstPortLinkChanged, false /*fPresent*/),
                         VERR_ALREADY_EXISTS);
        RTTESTI_CHECK_RC(tstPdmInitComplete(), VINF_SUCCESS);
        tstPdmPowerOn();
        tstPortGuestSetBusNumbers();

        uint32_t cMsis  = 0;
        uint32_t cCalls = 0;
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTCTL, 2) == TSTPORT_SLTCTL_EMPTY);
        tstPortCheck(__LINE__, 0, false, cCalls, cMsis);

        /* The guest gets ready: MSI, then the slot.  Writing the slot control is a command,
           and the guest asked to be told when one is done. */
        uint16_t const uSltCtlOff = TSTPORT_SLTCTL_EMPTY | TSTPORT_SLTCTL_ENABLES;
        uint16_t const uSltCtlOn  = TSTPORT_SLTCTL_OCCUPIED | TSTPORT_SLTCTL_ENABLES;
        tstPortGuestEnableMsi();
        tstPortCheck(__LINE__, 0, false, cCalls, cMsis);
        tstPortCfgWrite(TSTPORT_SLTCTL, uSltCtlOff, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_CMDC, false, cCalls, ++cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_CMDC, 2);
        tstPortCheck(__LINE__, 0, false, cCalls, cMsis);

        /* A device is put into the slot: present, but without power there is no link. */
        devpciR3HotPlugSlotSetPresence(pDevInsPort, true);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_PRSD, false, cCalls, ++cMsis);
        devpciR3HotPlugSlotSetPresence(pDevInsPort, true);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_PRSD, false, cCalls, cMsis);

        /* One more event while the guest has not dealt with the first: no further interrupt,
           not before the guest has cleared everything. */
        devpciR3HotPlugSlotPressAttentionButton(pDevInsPort);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_PRSD | VBOX_PCI_EXP_SLTSTA_ATNB, false, cCalls, cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_PRSD, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_ATNB, false, cCalls, cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_ATNB, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES, false, cCalls, cMsis);
        devpciR3HotPlugSlotPressAttentionButton(pDevInsPort);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_ATNB, false, cCalls, ++cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_ATNB, 2);

        /* The guest switches the power on: the link comes up, the device is told.  Command completed and
           the link state change are one interrupt. */
        tstPortCfgWrite(TSTPORT_SLTCTL, uSltCtlOn, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_CMDC | VBOX_PCI_EXP_SLTSTA_LLCHG, true, ++cCalls, ++cMsis);
        RTTESTI_CHECK(g_TstPortLink.pDevIns == pDevInsPort);
        RTTESTI_CHECK(devpciR3HotPlugSlotIsLinkUp(pDevInsPort));
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_CMDC | VBOX_PCI_EXP_SLTSTA_LLCHG, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES, true, cCalls, cMsis);

        /* The guest disables the link and enables it again. */
        uint16_t const uLnkCtl = (uint16_t)tstPortCfgRead(TSTPORT_LNKCTL, 2);
        tstPortCfgWrite(TSTPORT_LNKCTL, uLnkCtl | VBOX_PCI_EXP_LNKCTL_DISABLE, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_LLCHG, false, ++cCalls, ++cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_LLCHG, 2);
        tstPortCfgWrite(TSTPORT_LNKCTL, uLnkCtl, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_LLCHG, true, ++cCalls, ++cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_LLCHG, 2);

        /* With the hot-plug interrupt disabled the events wait, and they are reported when it gets enabled. */
        tstPortCfgWrite(TSTPORT_SLTCTL, uSltCtlOn & ~VBOX_PCI_EXP_SLTCTL_HPIE, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_CMDC, true, cCalls, cMsis);
        devpciR3HotPlugSlotPressAttentionButton(pDevInsPort);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_CMDC | VBOX_PCI_EXP_SLTSTA_ATNB, true, cCalls, cMsis);
        tstPortCfgWrite(TSTPORT_SLTCTL, uSltCtlOn, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_CMDC | VBOX_PCI_EXP_SLTSTA_ATNB, true, cCalls, ++cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_CMDC | VBOX_PCI_EXP_SLTSTA_ATNB, 2);

        /* An event the guest has not enabled is noted down without an interrupt. */
        tstPortCfgWrite(TSTPORT_SLTCTL, uSltCtlOn & ~VBOX_PCI_EXP_SLTCTL_ATNB, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_CMDC, true, cCalls, ++cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_CMDC, 2);
        devpciR3HotPlugSlotPressAttentionButton(pDevInsPort);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_ATNB, true, cCalls, cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_ATNB, 2);
        tstPortCfgWrite(TSTPORT_SLTCTL, uSltCtlOn, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_CMDC, true, cCalls, ++cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_CMDC, 2);

        /* Without MSI enabled there is no interrupt, the events are reported when it gets enabled. */
        uint16_t const fMsgCtrl = (uint16_t)tstPortCfgRead(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_CONTROL, 2);
        RTTESTI_CHECK(fMsgCtrl & VBOX_PCI_MSI_FLAGS_ENABLE);
        tstPortCfgWrite(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_CONTROL, fMsgCtrl & ~VBOX_PCI_MSI_FLAGS_ENABLE, 2);
        devpciR3HotPlugSlotPressAttentionButton(pDevInsPort);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_ATNB, true, cCalls, cMsis);
        tstPortCfgWrite(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_CONTROL, fMsgCtrl, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_ATNB, true, cCalls, ++cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_ATNB, 2);

        /* The device is gone all of a sudden, and another one is there with the power still on. */
        devpciR3HotPlugSlotSetPresence(pDevInsPort, false);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRSD | VBOX_PCI_EXP_SLTSTA_LLCHG, false, ++cCalls, ++cMsis);
        RTTESTI_CHECK(!devpciR3HotPlugSlotIsLinkUp(pDevInsPort));
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_PRSD | VBOX_PCI_EXP_SLTSTA_LLCHG, 2);
        devpciR3HotPlugSlotSetPresence(pDevInsPort, true);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_PRSD | VBOX_PCI_EXP_SLTSTA_LLCHG, true, ++cCalls, ++cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_PRSD | VBOX_PCI_EXP_SLTSTA_LLCHG, 2);

        /* The orderly way: the guest switches the power off, then the device is taken out. */
        tstPortCfgWrite(TSTPORT_SLTCTL, uSltCtlOff, 2);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_CMDC | VBOX_PCI_EXP_SLTSTA_LLCHG, false, ++cCalls, ++cMsis);
        tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_CMDC | VBOX_PCI_EXP_SLTSTA_LLCHG, 2);
        devpciR3HotPlugSlotSetPresence(pDevInsPort, false);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRSD, false, cCalls, ++cMsis);

        /* A reset of the VM: no events, no interrupts enabled, the empty slot is without power. */
        tstPdmReset();
        tstPortGuestSetBusNumbers();
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTCTL, 2) == TSTPORT_SLTCTL_EMPTY);
        RTTESTI_CHECK(!(tstPortCfgRead(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_CONTROL, 2) & VBOX_PCI_MSI_FLAGS_ENABLE));
        tstPortCheck(__LINE__, 0, false, cCalls, cMsis);

        /* A device put in while the guest doesn't listen is found by looking, and a reset
           switches the power on for it. */
        devpciR3HotPlugSlotSetPresence(pDevInsPort, true);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_PRSD, false, cCalls, cMsis);
        tstPdmReset();
        tstPortGuestSetBusNumbers();
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTCTL, 2) == TSTPORT_SLTCTL_OCCUPIED);
        tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES, true, ++cCalls, cMsis);
    }
    tstFixtureTeardown();
}


/**
 * A passthrough device behind a root port with a hot-plug capable slot, as
 * configured when the VM starts: the slot has power, and the guest reaches the
 * device only while the link is up.
 */
static void tstPortDevice(void)
{
    RTTestISub("Root port: device in the hot-plug slot");

    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev        = NULL;
    PPDMDEVINS  pDevInsPort = NULL;
    PPDMDEVINS  pDevInsDev  = NULL;
    int rc = tstPortFixtureSetup("RootCmplxRootPort", true /*fHotPlug*/, &Model, &pDev, &pDevInsPort, &pDevInsDev);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("%Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        RTTESTI_CHECK_RC(tstPdmInitComplete(), VINF_SUCCESS);
        tstPdmPowerOn();
        tstPortGuestSetBusNumbers();

        /* The device finds the port it is behind, the port itself is behind none. */
        RTTESTI_CHECK(devpciR3HotPlugSlotFind(pDevInsDev, pDevInsDev->apPciDevs[0]) == pDevInsPort);
        RTTESTI_CHECK(devpciR3HotPlugSlotFind(pDevInsPort, pDevInsPort->apPciDevs[0]) == NULL);

        /* Occupied and with power from the start. */
        RTTESTI_CHECK_MSG(tstPortCfgRead(TSTPORT_SLTCTL, 2) == TSTPORT_SLTCTL_OCCUPIED, ("%#RX32\n", tstPortCfgRead(TSTPORT_SLTCTL, 2)));
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTSTA, 2) == VBOX_PCI_EXP_SLTSTA_PRES);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_LNKSTA, 2) & VBOX_PCI_EXP_LNKSTA_DL_ACT);
        RTTESTI_CHECK(devpciR3HotPlugSlotIsLinkUp(pDevInsPort));
        RTTESTI_CHECK(tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, VBOX_PCI_VENDOR_ID, 2) == TST_VENDOR_ID);
        RTTESTI_CHECK(tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, VBOX_PCI_DEVICE_ID, 2) == TST_DEVICE_ID);

        /* The guest switches the power off or disables the link: nothing is there, nothing gets through. */
        uint32_t const uCmdBefore = tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, VBOX_PCI_COMMAND, 2);
        uint16_t const uLnkCtl    = (uint16_t)tstPortCfgRead(TSTPORT_LNKCTL, 2);
        for (unsigned iVariant = 0; iVariant < 2; iVariant++)
        {
            if (iVariant == 0)
                tstPortCfgWrite(TSTPORT_SLTCTL, TSTPORT_SLTCTL_EMPTY, 2);
            else
                tstPortCfgWrite(TSTPORT_LNKCTL, uLnkCtl | VBOX_PCI_EXP_LNKCTL_DISABLE, 2);
            RTTESTI_CHECK_MSG(tstPortCfgRead(TSTPORT_SLTSTA, 2) & VBOX_PCI_EXP_SLTSTA_PRES, ("variant %u\n", iVariant));
            RTTESTI_CHECK_MSG(!(tstPortCfgRead(TSTPORT_LNKSTA, 2) & VBOX_PCI_EXP_LNKSTA_DL_ACT), ("variant %u\n", iVariant));
            RTTESTI_CHECK_MSG(tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, VBOX_PCI_VENDOR_ID, 2) == 0xffff, ("variant %u\n", iVariant));
            RTTESTI_CHECK_MSG(tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, VBOX_PCI_VENDOR_ID, 4) == UINT32_MAX, ("variant %u\n", iVariant));
            RTTESTI_CHECK_MSG(tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, 0x100, 4) == UINT32_MAX, ("variant %u\n", iVariant));
            tstPdmPciCfgWrite(TSTPORT_SEC_BUS, 0, VBOX_PCI_COMMAND, uCmdBefore ^ VBOX_PCI_COMMAND_MEMORY, 2);

            if (iVariant == 0)
                tstPortCfgWrite(TSTPORT_SLTCTL, TSTPORT_SLTCTL_OCCUPIED, 2);
            else
                tstPortCfgWrite(TSTPORT_LNKCTL, uLnkCtl, 2);
            RTTESTI_CHECK_MSG(tstPortCfgRead(TSTPORT_LNKSTA, 2) & VBOX_PCI_EXP_LNKSTA_DL_ACT, ("variant %u\n", iVariant));
            RTTESTI_CHECK_MSG(tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, VBOX_PCI_VENDOR_ID, 2) == TST_VENDOR_ID, ("variant %u\n", iVariant));
            RTTESTI_CHECK_MSG(tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, VBOX_PCI_COMMAND, 2) == uCmdBefore,
                              ("variant %u: command %#RX32, was %#RX32\n", iVariant,
                               tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, VBOX_PCI_COMMAND, 2), uCmdBefore));
        }

        /* A reset of the VM with the power off: on again. */
        tstPortCfgWrite(TSTPORT_SLTCTL, TSTPORT_SLTCTL_EMPTY, 2);
        RTTESTI_CHECK(!devpciR3HotPlugSlotIsLinkUp(pDevInsPort));
        tstPdmReset();
        tstPortGuestSetBusNumbers();
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTCTL, 2) == TSTPORT_SLTCTL_OCCUPIED);
        RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTSTA, 2) == VBOX_PCI_EXP_SLTSTA_PRES);
        RTTESTI_CHECK(devpciR3HotPlugSlotIsLinkUp(pDevInsPort));
        RTTESTI_CHECK(tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, VBOX_PCI_VENDOR_ID, 2) == TST_VENDOR_ID);

        /* The guest did not ask for interrupts and got none. */
        RTTESTI_CHECK(tstPdmMsiGetCount() == 0);
        RTTESTI_CHECK(tstVfioKrnlDevGetOpenCount(pDev) == 1);
    }
    tstFixtureTeardown();
}


/**
 * Puts a 16-bit value into the config space of the root port the way loading a
 * saved state does it.
 *
 * @param   pDevInsPort         The device instance of the root port.
 * @param   offReg              The config space offset.
 * @param   u16Value            The value.
 */
static void tstPortLoadU16(PPDMDEVINS pDevInsPort, uint32_t offReg, uint16_t u16Value)
{
    pDevInsPort->apPciDevs[0]->abConfig[offReg]     = RT_LO_U8(u16Value);
    pDevInsPort->apPciDevs[0]->abConfig[offReg + 1] = RT_HI_U8(u16Value);
}


/**
 * Puts the config space of a root port into the place the way loading a saved
 * state does it: as the guest left it with interrupts set up and nothing left
 * to be dealt with.
 *
 * @param   pDevInsPort         The device instance of the root port.
 * @param   fOccupied           Whether the state was saved with a device in the slot and the power on.
 */
static void tstPortLoadState(PPDMDEVINS pDevInsPort, bool fOccupied)
{
    uint8_t * const pbCfg = &pDevInsPort->apPciDevs[0]->abConfig[0];
    tstPortLoadU16(pDevInsPort, TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_ADDRESS_LO,     RT_LO_U16(TSTPORT_MSI_ADDR));
    tstPortLoadU16(pDevInsPort, TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_ADDRESS_LO + 2, RT_HI_U16(TSTPORT_MSI_ADDR));
    tstPortLoadU16(pDevInsPort, TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_DATA_64, TSTPORT_MSI_DATA);
    pbCfg[TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_CONTROL] |= VBOX_PCI_MSI_FLAGS_ENABLE;
    tstPortLoadU16(pDevInsPort, TSTPORT_SLTCTL, (fOccupied ? TSTPORT_SLTCTL_OCCUPIED : TSTPORT_SLTCTL_EMPTY) | TSTPORT_SLTCTL_ENABLES);
    tstPortLoadU16(pDevInsPort, TSTPORT_SLTSTA, fOccupied ? VBOX_PCI_EXP_SLTSTA_PRES : 0);
    if (fOccupied)
        pbCfg[TSTPORT_LNKSTA + 1] |= (uint8_t)(VBOX_PCI_EXP_LNKSTA_DL_ACT >> 8);
    else
        pbCfg[TSTPORT_LNKSTA + 1] &= (uint8_t)~(VBOX_PCI_EXP_LNKSTA_DL_ACT >> 8);
}


/**
 * Loading a saved state into a VM with a root port with a hot-plug capable
 * slot: the guest is told what happened to the slot since the state was saved,
 * and a state which doesn't go with the port is refused.
 */
static void tstPortSavedState(void)
{
    RTTestISub("Root port: hot-plug slot and saved state");

    /*
     * Two rounds: the state was saved with an empty slot and is loaded with a
     * device in it, and the other way around.
     */
    for (unsigned iVariant = 0; iVariant < 2; iVariant++)
    {
        bool const fPresentNow = iVariant == 0;
        PPDMDEVINS pDevInsPort = NULL;
        int rc = tstPortFixtureSetup("RootCmplxRootPort", true /*fHotPlug*/, NULL, NULL, &pDevInsPort, NULL);
        RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("ich9pcibridge with hot-plug: %Rrc - %s\n", rc, tstPdmGetLastError()));
        if (RT_SUCCESS(rc))
        {
            RTTESTI_CHECK_RC(devpciR3HotPlugSlotRegisterClient(pDevInsPort, pDevInsPort, tstPortLinkChanged, fPresentNow),
                             VINF_SUCCESS);
            RTTESTI_CHECK_RC(tstPdmInitComplete(), VINF_SUCCESS);
            tstPdmPowerOn();

            /* Before anything is loaded, the slot is what it is when a VM starts. */
            uint32_t cCalls = fPresentNow ? 1 : 0;
            tstPortCheck(__LINE__, fPresentNow ? VBOX_PCI_EXP_SLTSTA_PRES : 0, fPresentNow, cCalls, 0);

            /* Loading went wrong before it got to the port: nothing happens. */
            RTTESTI_CHECK_RC(tstPdmLoadStateDone(VERR_SSM_INTEGRITY_CRC), VINF_SUCCESS);
            tstPortCheck(__LINE__, fPresentNow ? VBOX_PCI_EXP_SLTSTA_PRES : 0, fPresentNow, cCalls, 0);

            /* The state of a port which had no hot-plug capable slot, or of another kind of slot. */
            uint8_t * const pbCfg = &pDevInsPort->apPciDevs[0]->abConfig[0];
            uint8_t const   bSaved0 = pbCfg[TSTPORT_SLTCAP];
            pbCfg[TSTPORT_SLTCAP] &= ~VBOX_PCI_EXP_SLTCAP_HPC;
            RTTESTI_CHECK_RC(tstPdmLoadStateDone(VINF_SUCCESS), VERR_SSM_LOAD_CONFIG_MISMATCH);
            RTTESTI_CHECK_MSG(*tstPdmGetLastError() != '\0', ("no message\n"));
            pbCfg[TSTPORT_SLTCAP] = bSaved0 | VBOX_PCI_EXP_SLTCAP_MRL;
            RTTESTI_CHECK_RC(tstPdmLoadStateDone(VINF_SUCCESS), VERR_SSM_LOAD_CONFIG_MISMATCH);
            pbCfg[TSTPORT_SLTCAP] = bSaved0;
            uint8_t const   bSaved1 = pbCfg[TSTPORT_EXP_FLAGS + 1];
            pbCfg[TSTPORT_EXP_FLAGS + 1] &= (uint8_t)~(VBOX_PCI_EXP_FLAGS_SLOT >> 8);
            RTTESTI_CHECK_RC(tstPdmLoadStateDone(VINF_SUCCESS), VERR_SSM_LOAD_CONFIG_MISMATCH);
            pbCfg[TSTPORT_EXP_FLAGS + 1] = bSaved1;
            tstPortCheck(__LINE__, fPresentNow ? VBOX_PCI_EXP_SLTSTA_PRES : 0, fPresentNow, cCalls, 0);

            /* The state of the same port, with the slot the other way around. */
            tstPortLoadState(pDevInsPort, !fPresentNow);
            RTTESTI_CHECK_RC(tstPdmLoadStateDone(VINF_SUCCESS), VINF_SUCCESS);
            if (fPresentNow)
            {
                /* A device was put in: present, the power is off like the guest left it, which the device is told. */
                RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTCTL, 2) == (TSTPORT_SLTCTL_EMPTY | TSTPORT_SLTCTL_ENABLES));
                tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRES | VBOX_PCI_EXP_SLTSTA_PRSD, false, ++cCalls, 1);
            }
            else
            {
                /* The device was taken out: not present, the link is down.  The device in the slot,
                   which there is none, was not told as the link was down for it all along. */
                RTTESTI_CHECK(tstPortCfgRead(TSTPORT_SLTCTL, 2) == (TSTPORT_SLTCTL_OCCUPIED | TSTPORT_SLTCTL_ENABLES));
                tstPortCheck(__LINE__, VBOX_PCI_EXP_SLTSTA_PRSD | VBOX_PCI_EXP_SLTSTA_LLCHG, false, cCalls, 1);
            }

            /* The state as it is now loaded once more: nothing new, no interrupt. */
            tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_PRSD | VBOX_PCI_EXP_SLTSTA_LLCHG, 2);
            RTTESTI_CHECK_RC(tstPdmLoadStateDone(VINF_SUCCESS), VINF_SUCCESS);
            tstPortCheck(__LINE__, fPresentNow ? VBOX_PCI_EXP_SLTSTA_PRES : 0, false, cCalls, 1);

            /* An event the guest has not dealt with when the state was saved does not interrupt it
               a second time, the next one does. */
            devpciR3HotPlugSlotPressAttentionButton(pDevInsPort);
            tstPortCheck(__LINE__, (fPresentNow ? VBOX_PCI_EXP_SLTSTA_PRES : 0) | VBOX_PCI_EXP_SLTSTA_ATNB, false, cCalls, 2);
            RTTESTI_CHECK_RC(tstPdmLoadStateDone(VINF_SUCCESS), VINF_SUCCESS);
            tstPortCheck(__LINE__, (fPresentNow ? VBOX_PCI_EXP_SLTSTA_PRES : 0) | VBOX_PCI_EXP_SLTSTA_ATNB, false, cCalls, 2);
            tstPortCfgWrite(TSTPORT_SLTSTA, VBOX_PCI_EXP_SLTSTA_ATNB, 2);
            devpciR3HotPlugSlotPressAttentionButton(pDevInsPort);
            tstPortCheck(__LINE__, (fPresentNow ? VBOX_PCI_EXP_SLTSTA_PRES : 0) | VBOX_PCI_EXP_SLTSTA_ATNB, false, cCalls, 3);
        }
        tstFixtureTeardown();
    }
}


/**
 * Checks what has to hold for a root port with a hot-plug capable slot
 * whatever the guest and the device in the slot do.
 *
 * @returns true if all is well, false if not (test failure reported).
 * @param   pDevInsPort         The device instance of the root port.
 * @param   pabInitial          The PCI express capability as it was before anything was done to it.
 * @param   fPresent            Whether there is a device in the slot.
 * @param   fBusNumbersSet      Whether the secondary bus of the port has its number.
 */
static bool tstPortFuzzCheck(PPDMDEVINS pDevInsPort, uint8_t const *pabInitial, bool fPresent, bool fBusNumbersSet)
{
    uint32_t const cErrorsBefore = RTTestIErrorCount();

    /* Nothing changes which the guest has no business changing, and no event shows up which there is none of. */
    for (unsigned i = 0; i < RT_ELEMENTS(g_aTstPortRegs); i++)
    {
        uint32_t u32Initial = 0;
        for (unsigned iByte = 0; iByte < g_aTstPortRegs[i].cb; iByte++)
            u32Initial |= (uint32_t)pabInitial[g_aTstPortRegs[i].off + iByte] << (iByte * 8);
        uint32_t const u32      = tstPortCfgRead(TSTPORT_EXP_CAP + g_aTstPortRegs[i].off, g_aTstPortRegs[i].cb);
        uint32_t const fIgnore  = g_aTstPortRegs[i].fRw | g_aTstPortRegs[i].fDynamic
                                | (g_aTstPortRegs[i].off == 0x1a ? g_aTstPortRegs[i].fRw1c : 0);
        if ((u32 ^ u32Initial) & ~fIgnore)
            RTTestIFailed("register %#x of the PCI express capability is %#RX32, was %#RX32\n",
                          g_aTstPortRegs[i].off, u32, u32Initial);
    }
    uint32_t const uSltSta = tstPortCfgRead(TSTPORT_SLTSTA, 2);
    RTTESTI_CHECK_MSG(!(uSltSta & (VBOX_PCI_EXP_SLTSTA_PWRF | VBOX_PCI_EXP_SLTSTA_MRLS)), ("slot status %#RX32\n", uSltSta));

    /* Presence, link and what the device in the slot knows about it go together. */
    RTTESTI_CHECK_MSG(RT_BOOL(uSltSta & VBOX_PCI_EXP_SLTSTA_PRES) == fPresent, ("slot status %#RX32, fPresent=%d\n", uSltSta, fPresent));
    bool const fLinkUp =    fPresent
                         && !(tstPortCfgRead(TSTPORT_SLTCTL, 2) & VBOX_PCI_EXP_SLTCTL_PWRC)
                         && !(tstPortCfgRead(TSTPORT_LNKCTL, 2) & VBOX_PCI_EXP_LNKCTL_DISABLE);
    RTTESTI_CHECK_MSG(RT_BOOL(tstPortCfgRead(TSTPORT_LNKSTA, 2) & VBOX_PCI_EXP_LNKSTA_DL_ACT) == fLinkUp,
                      ("link status %#RX32, expected the link to be %s\n", tstPortCfgRead(TSTPORT_LNKSTA, 2), fLinkUp ? "up" : "down"));
    RTTESTI_CHECK_MSG(g_TstPortLink.fLinkUp == fLinkUp, ("the device in the slot was told last that the link is %s\n",
                                                         g_TstPortLink.fLinkUp ? "up" : "down"));
    RTTESTI_CHECK(devpciR3HotPlugSlotIsLinkUp(pDevInsPort) == fLinkUp);

    /* The guest reaches the device while the link is up and at no other time. */
    if (fBusNumbersSet)
    {
        uint32_t const idVendor = tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, VBOX_PCI_VENDOR_ID, 2);
        RTTESTI_CHECK_MSG(idVendor == (fLinkUp ? TST_VENDOR_ID : UINT32_C(0xffff)),
                          ("vendor ID %#RX32 with the link %s\n", idVendor, fLinkUp ? "up" : "down"));
    }

    return RTTestIErrorCount() == cErrorsBefore;
}


/**
 * Random guest accesses to a root port with a hot-plug capable slot and to the
 * device behind it, mixed with a device coming and going, the attention button,
 * resets of the VM and saved states being loaded.
 *
 * The number of operations follows TSTDEVPCIVFIO_FUZZ_MODELS, the seed is
 * TSTDEVPCIVFIO_FUZZ_SEED.
 */
static void tstPortFuzz(void)
{
    RTTestISub("Root port: random guest accesses and slot events");

    uint32_t    cOps  = 150 * 200;
    uint32_t    uSeed = UINT32_C(0x20261112);
    const char *psz   = RTEnvGet("TSTDEVPCIVFIO_FUZZ_MODELS");
    if (psz)
        cOps = RT_MIN(RTStrToUInt32(psz), 10000) * 200;
    psz = RTEnvGet("TSTDEVPCIVFIO_FUZZ_SEED");
    if (psz)
        uSeed = RTStrToUInt32(psz);

    int rc = RTRandAdvCreateParkMiller(&g_hTstRand);
    RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return;
    RTRandAdvSeed(g_hTstRand, uSeed);

    /* The passthrough device is on the secondary bus, the test says whether it is in the slot. */
    TSTVFIODEVCFG Model;
    tstModelInitDefault(&Model);
    PTSTVFIODEV pDev        = NULL;
    PPDMDEVINS  pDevInsPort = NULL;
    rc = tstPortFixtureSetup("RootCmplxRootPort", true /*fHotPlug*/, &Model, &pDev, &pDevInsPort, NULL);
    RTTESTI_CHECK_MSG(RT_SUCCESS(rc), ("%Rrc - %s\n", rc, tstPdmGetLastError()));
    if (RT_SUCCESS(rc))
    {
        bool fPresent = tstRndChance(50);
        RTTESTI_CHECK_RC(devpciR3HotPlugSlotRegisterClient(pDevInsPort, pDevInsPort, tstPortLinkChanged, fPresent), VINF_SUCCESS);
        RTTESTI_CHECK_RC(tstPdmInitComplete(), VINF_SUCCESS);
        tstPdmPowerOn();
        tstPortGuestSetBusNumbers();

        uint8_t abInitial[TSTPORT_EXP_CAP_SIZE];
        for (unsigned off = 0; off < sizeof(abInitial); off++)
            abInitial[off] = (uint8_t)tstPortCfgRead(TSTPORT_EXP_CAP + off, 1);

        bool fBusNumbersSet = true;
        for (uint32_t iOp = 0; iOp < cOps; iOp++)
        {
            /* What decides about interrupts, before the operation. */
            uint32_t const cErrorsBefore = RTTestIErrorCount();
            uint32_t const cMsisBefore   = tstPdmMsiGetCount();
            bool const     fCanIrqBefore =    (tstPortCfgRead(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_CONTROL, 2) & VBOX_PCI_MSI_FLAGS_ENABLE)
                                           && (tstPortCfgRead(TSTPORT_SLTCTL, 2) & VBOX_PCI_EXP_SLTCTL_HPIE);
            bool           fMsiMaskWrite = false;

            uint32_t const uOp = tstRnd(0, 99);
            if (uOp < 50)
            {
                /* The guest writes to the port: mostly at and around the PCI express capability,
                   also to the MSI capability and anywhere else, aligned or not.  The bus numbers
                   are left alone, the checks need the secondary bus to be reachable. */
                static const unsigned s_acb[] = { 1, 2, 4 };
                unsigned const cb  = s_acb[tstRnd(0, 2)];
                uint32_t       off;
                uint32_t const uWhere = tstRnd(0, 9);
                if (uWhere < 6)
                    off = tstRnd(TSTPORT_EXP_CAP - 4, TSTPORT_EXP_CAP + TSTPORT_EXP_CAP_SIZE + 3);
                else if (uWhere < 8)
                    off = tstRnd(TSTPORT_MSI_CAP - 2, TSTPORT_MSI_CAP + VBOX_MSI_CAP_SIZE_64 + 1);
                else
                    off = tstRnd(0, 0x100 - cb);
                if (tstRndChance(80))
                    off &= ~(uint32_t)(cb - 1);
                if (   off + cb > VBOX_PCI_PRIMARY_BUS
                    && off <= VBOX_PCI_SUBORDINATE_BUS)
                    continue;
                fMsiMaskWrite =    off + cb > TSTPORT_MSI_CAP + VBOX_MSI_CAP_MASK_BITS_64
                                && off < TSTPORT_MSI_CAP + VBOX_MSI_CAP_PENDING_BITS_64;
                tstPortCfgWrite(off, tstRndValue(), cb);
            }
            else if (uOp < 58)
                /* The guest does what guests do to get interrupts. */
                switch (tstRnd(0, 2))
                {
                    case 0:
                        tstPortGuestEnableMsi();
                        break;
                    case 1:
                        tstPortCfgWrite(TSTPORT_SLTCTL, tstPortCfgRead(TSTPORT_SLTCTL, 2) | TSTPORT_SLTCTL_ENABLES, 2);
                        break;
                    default:
                        tstPortCfgWrite(TSTPORT_SLTSTA, tstPortCfgRead(TSTPORT_SLTSTA, 2), 2);
                        break;
                }
            else if (uOp < 66)
                /* The guest switches the power of the slot. */
                tstPortCfgWrite(TSTPORT_SLTCTL, tstPortCfgRead(TSTPORT_SLTCTL, 2) ^ VBOX_PCI_EXP_SLTCTL_PWRC, 2);
            else if (uOp < 76)
            {
                /* The guest accesses the device behind the port. */
                static const unsigned s_acb[] = { 1, 2, 4 };
                unsigned const cb  = s_acb[tstRnd(0, 2)];
                uint32_t const off = tstRnd(0, 0x1000 - cb) & ~(uint32_t)(cb - 1);
                if (tstRndChance(50))
                    tstPdmPciCfgRead(TSTPORT_SEC_BUS, 0, off, cb);
                else
                    tstPdmPciCfgWrite(TSTPORT_SEC_BUS, 0, off, tstRndValue(), cb);
            }
            else if (uOp < 88)
            {
                /* The device comes or goes. */
                fPresent = tstRndChance(50);
                devpciR3HotPlugSlotSetPresence(pDevInsPort, fPresent);
            }
            else if (uOp < 94)
                devpciR3HotPlugSlotPressAttentionButton(pDevInsPort);
            else if (uOp < 97)
                /* A saved state which is what is there already, so nothing changes. */
                RTTESTI_CHECK_RC(tstPdmLoadStateDone(VINF_SUCCESS), VINF_SUCCESS);
            else
            {
                /* A reset of the VM, usually followed by the guest setting the port up again. */
                tstPdmReset();
                fBusNumbersSet = tstRndChance(90);
                if (fBusNumbersSet)
                    tstPortGuestSetBusNumbers();
            }

            /* No interrupt unless the guest has asked for it, before or by what it just did, and one
               at most.  What MSI does when the guest clears mask bits is none of the port's business:
               there are pending bits for more interrupts than the port has, and the guest can set them. */
            bool const fCanIrqAfter =    (tstPortCfgRead(TSTPORT_MSI_CAP + VBOX_MSI_CAP_MESSAGE_CONTROL, 2) & VBOX_PCI_MSI_FLAGS_ENABLE)
                                      && (tstPortCfgRead(TSTPORT_SLTCTL, 2) & VBOX_PCI_EXP_SLTCTL_HPIE);
            uint32_t const cMsis = tstPdmMsiGetCount();
            if (!fMsiMaskWrite)
            {
                RTTESTI_CHECK_MSG(cMsis == cMsisBefore || fCanIrqBefore || fCanIrqAfter,
                                  ("%u interrupt(s) the guest has not asked for\n", cMsis - cMsisBefore));
                RTTESTI_CHECK_MSG(cMsis - cMsisBefore <= 1, ("%u interrupts for one operation\n", cMsis - cMsisBefore));
            }

            if (   !tstPortFuzzCheck(pDevInsPort, abInitial, fPresent, fBusNumbersSet)
                || RTTestIErrorCount() != cErrorsBefore)
            {
                RTTestIFailureDetails("operation %u (kind %u) with seed %#RX32 failed; run with TSTDEVPCIVFIO_FUZZ_SEED=%u to get it again\n",
                                      iOp, uOp, uSeed, uSeed);
                break;
            }
        }
        RTTestIPrintf(RTTESTLVL_ALWAYS, "%u operation(s) with seed %#RX32, %u interrupt(s)\n", cOps, uSeed, tstPdmMsiGetCount());
        RTTESTI_CHECK_MSG(tstVfioKrnlGetViolationCount() == 0,
                          ("%u request(s) to the kernel which are out of bounds\n", tstVfioKrnlGetViolationCount()));
    }
    tstFixtureTeardown();

    RTRandAdvDestroy(g_hTstRand);
    g_hTstRand = NIL_RTRAND;
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
    tstLegacy();
    tstRemoval();
    tstRemovalOtherCases();
    tstRemovalUnderLoad();
    tstSavedState();
    tstPortRegisters();
    tstPortSlotEvents();
    tstPortDevice();
    tstPortSavedState();
    tstFuzz();
    tstPortFuzz();

    return RTTestSummaryAndDestroy(hTest);
}
