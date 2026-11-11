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
#include <VBox/err.h>
#include <iprt/assert.h>
#include <iprt/string.h>
#include <iprt/test.h>

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
 * Adds an MSI capability with 64-bit addressing and per-vector masking.
 *
 * @param   pModel              The device model.
 * @param   offCap              Where to place the capability.
 * @param   cVectorsLog2        Number of vectors the device supports as a power of two.
 */
static void tstModelAddMsiCap(PTSTVFIODEVCFG pModel, uint8_t offCap, uint8_t cVectorsLog2)
{
    uint8_t abBody[22];
    RT_ZERO(abBody);
    uint16_t const fMsgCtrl = VBOX_PCI_MSI_FLAGS_64BIT | VBOX_PCI_MSI_FLAGS_MASKBIT | (uint16_t)(cVectorsLog2 << 1);
    abBody[0] = RT_LO_U8(fMsgCtrl);
    abBody[1] = RT_HI_U8(fMsgCtrl);
    tstModelAddCap(pModel, offCap, VBOX_PCI_CAP_ID_MSI, abBody, sizeof(abBody));
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
    tstModelAddMsiCap(pModel, TST_MSI_CAP_OFF, 2 /*cVectorsLog2*/);
    tstModelAddExpCap(pModel, TST_EXP_CAP_OFF);
}


/**
 * Sets up the fake kernel with the given PCI functions and a VM with the ICH9
 * PCI bus and one passthrough device using these functions.
 *
 * tstFixtureTeardown() has to be called regardless of the result.
 *
 * @returns Status code of the passthrough device constructor, or of whatever failed before.
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
        RTTESTI_CHECK_RC(tstCfgSetString(pCfgFun, "VfioPath", pszVfioPath), VINF_SUCCESS);
    }
    return tstPdmDevCreate(&g_DevicePciVfio, 0 /*iInstance*/, pCfgInst, NULL);
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

        /* The device is open exactly once and nothing is mapped for DMA before the guest enables bus mastering. */
        RTTESTI_CHECK(tstVfioKrnlDevGetOpenCount(pDev) == 1);
        RTTESTI_CHECK(tstVfioKrnlGetDmaMappedBytes() == 0);

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

        /* Enabling bus mastering makes all guest RAM available to the device, at guest physical addresses. */
        tstPdmPciCfgWrite(0, TST_PCI_DEVFN, VBOX_PCI_COMMAND, VBOX_PCI_COMMAND_MEMORY | VBOX_PCI_COMMAND_MASTER, 2);
        RTTESTI_CHECK(tstVfioKrnlIsIovaRangeMapped(0, TST_RAM_SIZE));
        RTTESTI_CHECK_MSG(tstVfioKrnlGetDmaMappedBytes() == TST_RAM_SIZE,
                          ("%RU64 bytes mapped for DMA\n", tstVfioKrnlGetDmaMappedBytes()));
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
    tstModelInit(&Model, TST_VFIO_NODE, 0x01 /* mass storage */, 0x08 /* non-volatile memory */);
    tstModelSetBar(&Model, 0, TST_BAR0_SIZE, PCI_ADDRESS_SPACE_MEM);
    tstModelSetBar(&Model, 2, _8K, PCI_ADDRESS_SPACE_MEM);
    tstModelAddMsixCap(&Model, TST_MSIX_CAP_OFF, cVectors, 2 /*iBar*/, 0 /*offTable*/, _4K /*offPba*/);
    tstModelAddExpCap(&Model, TST_EXP_CAP_OFF);

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
    tstUnknownNode();

    return RTTestSummaryAndDestroy(hTest);
}
