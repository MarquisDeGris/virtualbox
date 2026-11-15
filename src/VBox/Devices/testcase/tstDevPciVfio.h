/* $Id$ */
/** @file
 * tstDevPciVfio - Testcase for the VFIO PCI passthrough device, internal header.
 *
 * The testcase consists of three parts:
 *      - tstDevPciVfioKrnl.cpp: A fake VFIO/IOMMUFD kernel interface living in
 *        the test process, together with a configurable PCI device model.
 *      - tstDevPciVfioPdm.cpp: A minimal PDM environment for running the real
 *        ICH9 PCI bus and the VFIO passthrough device.
 *      - tstDevPciVfio.cpp: The tests.
 *
 * Include VBox/vmm/pdmdev.h before this header.
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

#ifndef VBOX_INCLUDED_SRC_testcase_tstDevPciVfio_h
#define VBOX_INCLUDED_SRC_testcase_tstDevPciVfio_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <iprt/types.h>
#include <VBox/types.h>

RT_C_DECLS_BEGIN


/** @name The fake VFIO/IOMMUFD kernel interface (tstDevPciVfioKrnl.cpp).
 * @{ */

/** Number of VFIO regions of a PCI device (BAR0..5, ROM, config space, VGA). */
#define TSTVFIO_REGION_COUNT        9
/** Number of VFIO interrupt indexes of a PCI device (INTx, MSI, MSI-X, ERR, REQ). */
#define TSTVFIO_IRQ_INDEX_COUNT     5

/** The path of the fake IOMMUFD device node. */
#define TSTVFIO_IOMMU_NODE          "/dev/iommu"
/** The path of the fake VFIO container node (legacy interface). */
#define TSTVFIO_CONTAINER_NODE      "/dev/vfio/vfio"
/** The path of the fake VFIO group node all functions are in (legacy interface). */
#define TSTVFIO_GROUP_NODE          "/dev/vfio/7"

/**
 * Description of a single VFIO region of the device model.
 */
typedef struct TSTVFIOREGION
{
    /** Size of the region in bytes, 0 if the region is not implemented. */
    uint64_t            cb;
    /** VFIO_REGION_INFO_FLAG_XXX, without VFIO_REGION_INFO_FLAG_CAPS. */
    uint32_t            fFlags;
    /** Whether the region reports the MSI-X mappable capability. */
    bool                fMsixMappable;
} TSTVFIOREGION;

/**
 * Description of a PCI function served by the fake kernel.
 *
 * The region for the config space is derived from cbCfg and abCfg, the entry
 * in aRegions is ignored.
 */
typedef struct TSTVFIODEVCFG
{
    /** The device node the function is reachable under. */
    char                szNode[64];
    /** Size of the config space, 256 or 4096. */
    uint32_t            cbCfg;
    /** Initial content of the config space. */
    uint8_t             abCfg[4096];
    /** The regions, indexed by VFIO_PCI_XXX_REGION_INDEX. */
    TSTVFIOREGION       aRegions[TSTVFIO_REGION_COUNT];
    /** Number of interrupts per VFIO_PCI_XXX_IRQ_INDEX. */
    uint32_t            acIrqs[TSTVFIO_IRQ_INDEX_COUNT];
    /** Whether the function is one without an IOMMU behind it: it can be opened and
     * bound, but there is no I/O address space it could be attached to. */
    bool                fNoIommu;
    /** Whether VFIO reports the function as something else than a PCI device. */
    bool                fNotPci;
    /** Whether the function can't be reset on its own. */
    bool                fNoFunctionReset;
    /** Whether the bus the function is on can't be reset by its user. */
    bool                fNoBusReset;
} TSTVFIODEVCFG;
/** Pointer to a device model description. */
typedef TSTVFIODEVCFG *PTSTVFIODEVCFG;
/** Pointer to a const device model description. */
typedef const TSTVFIODEVCFG *PCTSTVFIODEVCFG;

/** Opaque handle to a PCI function of the fake kernel. */
typedef struct TSTVFIODEV *PTSTVFIODEV;

/**
 * Callback for writes to the config space of a PCI function, called after the
 * write was done.
 *
 * @param   pDev                The PCI function.
 * @param   offReg              The config space offset written to.
 * @param   cb                  Number of bytes written.
 * @param   pvUser              The user argument.
 */
typedef DECLCALLBACKTYPE(void, FNTSTVFIOCFGWRITE,(PTSTVFIODEV pDev, uint32_t offReg, size_t cb, void *pvUser));
/** Pointer to a config space write callback. */
typedef FNTSTVFIOCFGWRITE *PFNTSTVFIOCFGWRITE;

/**
 * Callback before ordinary memory gets mapped at a given address.
 *
 * @param   pvUser              The user argument.
 */
typedef DECLCALLBACKTYPE(void, FNTSTVFIOMMAPHOOK,(void *pvUser));
/** Pointer to a callback before ordinary memory gets mapped at a given address. */
typedef FNTSTVFIOMMAPHOOK *PFNTSTVFIOMMAPHOOK;

int         tstVfioKrnlInit(void);
void        tstVfioKrnlTerm(void);
void        tstVfioKrnlSetLegacy(bool fLegacy);
int         tstVfioKrnlDevAdd(PCTSTVFIODEVCFG pCfg, PTSTVFIODEV *ppDev);
int         tstVfioKrnlDevReplace(PTSTVFIODEV pDev, PCTSTVFIODEVCFG pCfg);
void       *tstVfioKrnlDevGetRegionPtr(PTSTVFIODEV pDev, uint32_t iRegion);
int         tstVfioKrnlDevTriggerIrq(PTSTVFIODEV pDev, uint32_t uIrqIndex, uint32_t uVector);
uint32_t    tstVfioKrnlDevGetIrqCount(PTSTVFIODEV pDev, uint32_t uIrqIndex);
void        tstVfioKrnlDevSetIrqsAvailable(PTSTVFIODEV pDev, uint32_t uIrqIndex, uint32_t cIrqs);
uint32_t    tstVfioKrnlDevGetResetCount(PTSTVFIODEV pDev);
uint32_t    tstVfioKrnlDevGetBusResetCount(PTSTVFIODEV pDev);
void        tstVfioKrnlDevProtectMappings(PTSTVFIODEV pDev, bool fProtect);
void        tstVfioKrnlSetCfgWriteHook(PFNTSTVFIOCFGWRITE pfnHook, void *pvUser);
uint32_t    tstVfioKrnlDevGetOpenCount(PTSTVFIODEV pDev);
void        tstVfioKrnlDevSetGone(PTSTVFIODEV pDev, bool fGone);
void        tstVfioKrnlDevRequest(PTSTVFIODEV pDev);
void        tstVfioKrnlDevRemove(PTSTVFIODEV pDev);
void        tstVfioKrnlDevSignalError(PTSTVFIODEV pDev);
uint32_t    tstVfioKrnlGetOpenFdCount(void);
uint32_t    tstVfioKrnlGetInheritableFdCount(void);
uint32_t    tstVfioKrnlGetViolationCount(void);
uint64_t    tstVfioKrnlGetDmaMappedBytes(void);
bool        tstVfioKrnlIsIovaRangeMapped(uint64_t uIova, uint64_t cb);
void        tstVfioKrnlFailIoctl(unsigned long uReq, uint32_t cSkip, int iErrno);
void        tstVfioKrnlFailPlaceholderMmap(uint32_t cTimes);
void        tstVfioKrnlSetPlaceholderMmapHook(PFNTSTVFIOMMAPHOOK pfnHook, void *pvUser);
/** @} */


/** @name The configuration tree mock (tstDevPciVfioPdm.cpp).
 * @{ */
PCFGMNODE   tstCfgNodeCreate(PCFGMNODE pParent, const char *pszName);
void        tstCfgNodeDestroy(PCFGMNODE pNode);
int         tstCfgSetInteger(PCFGMNODE pNode, const char *pszName, uint64_t u64Value);
int         tstCfgSetString(PCFGMNODE pNode, const char *pszName, const char *pszValue);
/** @} */


/** @name The PDM mock (tstDevPciVfioPdm.cpp).
 * @{ */

/** Guest physical address of the PCI ECAM (MCFG) window used by the tests. */
#define TSTPDM_MCFG_BASE            UINT64_C(0xe0000000)
/** Size of the PCI ECAM (MCFG) window used by the tests (64 buses). */
#define TSTPDM_MCFG_SIZE            UINT64_C(0x04000000)
/** Start of the hole in the guest RAM: legacy video memory and ROMs, like in a PC. */
#define TSTPDM_RAM_HOLE_START       UINT32_C(0x000a0000)
/** Size of the hole in the guest RAM. */
#define TSTPDM_RAM_HOLE_SIZE        UINT32_C(0x00060000)

/**
 * A message signalled interrupt seen by the interrupt controller mock.
 */
typedef struct TSTPDMMSI
{
    /** The bus:device:function of the sender. */
    uint16_t            uBusDevFn;
    /** The message address. */
    uint64_t            uAddr;
    /** The message data. */
    uint32_t            uData;
} TSTPDMMSI;
/** Pointer to a message signalled interrupt record. */
typedef TSTPDMMSI *PTSTPDMMSI;

/**
 * Callback between the preparations for saving or loading a state and doing it.
 *
 * @param   pvUser              The user argument.
 */
typedef DECLCALLBACKTYPE(void, FNTSTPDMSTATEHOOK,(void *pvUser));
/** Pointer to a callback between the preparations for saving or loading a state and doing it. */
typedef FNTSTPDMSTATEHOOK *PFNTSTPDMSTATEHOOK;

int             tstPdmInit(uint32_t cbRam);
void            tstPdmTerm(void);
int             tstPdmDevCreate(PCPDMDEVREG pReg, uint32_t iInstance, PCFGMNODE pCfgInst, PPDMDEVINS *ppDevIns);
int             tstPdmQueryDeviceLun(const char *pszDevice, uint32_t iInstance, uint32_t iLun, PPDMIBASE *ppBase);
void            tstPdmSetExecutionEngine(uint8_t bExecEngine);
int             tstPdmInitComplete(void);
void            tstPdmPowerOn(void);
void            tstPdmPowerOff(void);
void            tstPdmSuspend(void);
void            tstPdmResume(void);
void            tstPdmReset(void);
void            tstPdmProcessRequests(void);
uint32_t        tstPdmGetRequestCount(void);
void            tstPdmSetRequestProcessing(bool fAutomatic);
int             tstPdmIoPortRead(RTIOPORT uPort, uint32_t *pu32, unsigned cb);
int             tstPdmIoPortWrite(RTIOPORT uPort, uint32_t u32, unsigned cb);
int             tstPdmPhysRead(RTGCPHYS GCPhys, void *pv, unsigned cb);
int             tstPdmPhysWrite(RTGCPHYS GCPhys, const void *pv, unsigned cb);
int             tstPdmPhysFill(RTGCPHYS GCPhys, uint32_t u32Item, unsigned cbItem, unsigned cItems);
bool            tstPdmIsMmio2Mapped(RTGCPHYS GCPhys);
uint32_t        tstPdmGetMmio2MappedCount(void);
bool            tstPdmIsRunning(void);
uint32_t        tstPdmPciCfgRead(uint8_t iBus, uint8_t uDevFn, uint32_t offReg, unsigned cb);
void            tstPdmPciCfgWrite(uint8_t iBus, uint8_t uDevFn, uint32_t offReg, uint32_t u32, unsigned cb);
bool            tstPdmPciLockIsOwner(void);
void            tstPdmPciLock(void);
void            tstPdmPciUnlock(void);
bool            tstPdmIntxIsHigh(void);
uint32_t        tstPdmIntxGetRaisedCount(void);
const char     *tstPdmGetLastError(void);
const char     *tstPdmGetLastRuntimeErrorId(void);
int             tstPdmSaveStatePrep(bool fLive);
int             tstPdmLoadStatePrep(void);
int             tstPdmLoadStateDone(int rcLoad);
void            tstPdmSetStateHook(PFNTSTPDMSTATEHOOK pfnHook, void *pvUser);
int             tstPdmSaveState(bool fLive, void **ppvState, size_t *pcbState);
void            tstPdmFreeState(void *pvState);
int             tstPdmLoadState(const void *pvState, size_t cbState);
int             tstPdmStateDropUnit(void *pvState, size_t *pcbState, const char *pszUnit, uint32_t uInstance);
int             tstPdmStatePatchItem(void *pvState, size_t cbState, const char *pszUnit, uint32_t uInstance, uint32_t idxItem,
                                     size_t offItem, const void *pvNew, size_t cbNew);
uint32_t        tstPdmMsiGetCount(void);
bool            tstPdmMsiWait(uint32_t cExpected, RTMSINTERVAL cMsTimeout);
bool            tstPdmMsiGet(uint32_t idxMsi, PTSTPDMMSI pMsi);
bool            tstPdmMsiGetLast(PTSTPDMMSI pMsi);
uint32_t        tstPdmGetHeapAllocCount(void);
/** @} */

RT_C_DECLS_END

#endif /* !VBOX_INCLUDED_SRC_testcase_tstDevPciVfio_h */
