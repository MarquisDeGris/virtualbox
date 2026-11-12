/* $Id$ */
/** @file
 * tstDevPciVfio - Minimal PDM environment for the PCI bus and passthrough devices.
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

/** @page pg_tst_devpcivfio_pdm     tstDevPciVfio - The PDM mock
 *
 * This is the part of PDM, IOM, PGM and CFGM the ICH9 PCI bus, the PCI to PCI
 * bridge and the VFIO passthrough device need in ring-3, reduced to what is
 * observable by a device:
 *      - A configuration tree.
 *      - Device instances with shared and context specific instance data and
 *        the PCI device structures behind them.
 *      - Registries for I/O port, MMIO and MMIO2 ranges including the map
 *        state, and a dispatcher the tests use for guest accesses.
 *      - PCI bus registration and the PCI helpers; interrupts end in a log
 *        the tests can inspect.
 *      - PDM threads on top of IPRT threads.
 *
 * The mock enforces the restrictions of the real thing which matter for the
 * device design, most notably that ranges can only be created while the VM is
 * being constructed.
 *
 * Helpers which are not implemented trap, see tstPdmHlp_Unimplemented().
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define PDMPCIDEV_INCLUDE_PRIVATE  /* We are PDM here and need the bus private PCI device data. */
#include <VBox/types.h>
#include <VBox/param.h>

/* The PDM device instance internals as seen by this mock. They must be declared
   before pdmdev.h is included, the devices themselves only see the padding. */
typedef struct PDMDEVINSINTR3
{
    /** The device record of the mock. */
    struct TSTPDMDEV   *pDev;
} PDMDEVINSINTR3;
typedef struct PDMDEVINSINTR0
{
    /** Unused. */
    uint32_t            uDummy;
} PDMDEVINSINTR0;
typedef struct PDMDEVINSINTRC
{
    /** Unused. */
    uint32_t            uDummy;
} PDMDEVINSINTRC;
#define PDMDEVINSINT_DECLARED

#include <VBox/pci.h>
#include <VBox/msi.h>
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/pdmthread.h>
#include <VBox/vmm/pgm.h>
#include <VBox/vmm/vmapi.h>
#include <VBox/vmm/iom.h>
#include <VBox/err.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/critsect.h>
#include <iprt/mem.h>
#include <iprt/semaphore.h>
#include <iprt/string.h>
#include <iprt/thread.h>
#include <iprt/time.h>

#include "tstDevPciVfio.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** How long to wait for a PDM thread to change state. */
#define TSTPDM_THREAD_TIMEOUT_MS    RT_MS_30SEC
/** The number of MMIO2 ranges a VM can have (PGM_MAX_MMIO2_RANGES). */
#define TSTPDM_MAX_MMIO2_RANGES     32


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * A value in the configuration tree.
 */
typedef struct TSTCFGVALUE
{
    /** The next value of the node. */
    struct TSTCFGVALUE *pNext;
    /** The name. */
    char                szName[48];
    /** Whether this is a string (true) or an integer (false). */
    bool                fString;
    /** The integer value. */
    uint64_t            u64Value;
    /** The string value. */
    char               *pszValue;
} TSTCFGVALUE;
/** Pointer to a configuration value. */
typedef TSTCFGVALUE *PTSTCFGVALUE;

/**
 * A node in the configuration tree.
 */
struct CFGMNODE
{
    /** The parent node. */
    struct CFGMNODE    *pParent;
    /** The next sibling. */
    struct CFGMNODE    *pNext;
    /** The first child. */
    struct CFGMNODE    *pFirstChild;
    /** The first value. */
    PTSTCFGVALUE        pFirstValue;
    /** The name. */
    char                szName[48];
};

/**
 * A device instance of the mock.
 */
typedef struct TSTPDMDEV
{
    /** The device instance. */
    PPDMDEVINS          pDevIns;
    /** Size of the device instance allocation. */
    size_t              cbDevIns;
    /** The instance node of the configuration (the parent of "Config"). */
    PCFGMNODE           pCfgInst;
} TSTPDMDEV;
/** Pointer to a device instance record. */
typedef TSTPDMDEV *PTSTPDMDEV;

/**
 * A registered PCI bus.
 */
typedef struct TSTPDMPCIBUS
{
    /** The bus device instance, NULL if the entry is unused. */
    PPDMDEVINS          pDevIns;
    /** The callbacks of the bus. */
    PDMPCIBUSREGR3      Reg;
} TSTPDMPCIBUS;
/** Pointer to a registered PCI bus. */
typedef TSTPDMPCIBUS *PTSTPDMPCIBUS;

/**
 * An I/O port range.
 */
typedef struct TSTPDMIOPORT
{
    /** The owner. */
    PPDMDEVINS          pDevIns;
    /** Number of ports. */
    RTIOPORT            cPorts;
    /** The first port if mapped. */
    RTIOPORT            uPort;
    /** Whether the range is mapped. */
    bool                fMapped;
    /** IOM_IOPORT_F_XXX. */
    uint32_t            fFlags;
    /** The write handler. */
    PFNIOMIOPORTNEWOUT  pfnOut;
    /** The read handler. */
    PFNIOMIOPORTNEWIN   pfnIn;
    /** The user argument for the handlers. */
    void               *pvUser;
} TSTPDMIOPORT;

/**
 * An MMIO range.
 */
typedef struct TSTPDMMMIO
{
    /** The owner. */
    PPDMDEVINS          pDevIns;
    /** Size of the range. */
    RTGCPHYS            cbRegion;
    /** Where the range is mapped, NIL_RTGCPHYS if it is not. */
    RTGCPHYS            GCPhys;
    /** IOMMMIO_FLAGS_XXX. */
    uint32_t            fFlags;
    /** The write handler. */
    PFNIOMMMIONEWWRITE  pfnWrite;
    /** The read handler. */
    PFNIOMMMIONEWREAD   pfnRead;
    /** The fill handler, optional. */
    PFNIOMMMIONEWFILL   pfnFill;
    /** The user argument for the handlers. */
    void               *pvUser;
} TSTPDMMMIO;

/**
 * An MMIO2 range.
 */
typedef struct TSTPDMMMIO2
{
    /** The owner. */
    PPDMDEVINS          pDevIns;
    /** The PCI sub-device the range is registered for, 254 if none. */
    uint32_t            iSubDev;
    /** The region number, unique per owner and sub-device. */
    uint32_t            iRegion;
    /** Size of the range. */
    RTGCPHYS            cbRegion;
    /** Where the range is mapped, NIL_RTGCPHYS if it is not. */
    RTGCPHYS            GCPhys;
    /** The backing memory. */
    uint8_t            *pbBacking;
    /** Whether the backing memory was allocated by the mock. */
    bool                fOwned;
} TSTPDMMMIO2;

/**
 * A physical access handler type.
 */
typedef struct TSTPDMHNDTYPE
{
    /** The kind. */
    PGMPHYSHANDLERKIND  enmKind;
    /** The handler. */
    PFNPGMPHYSHANDLER   pfnHandler;
} TSTPDMHNDTYPE;

/**
 * A registered physical access handler.
 */
typedef struct TSTPDMHND
{
    /** Whether the entry is in use. */
    bool                fUsed;
    /** First address covered. */
    RTGCPHYS            GCPhys;
    /** Last address covered. */
    RTGCPHYS            GCPhysLast;
    /** Index into the type table. */
    uint32_t            idxType;
} TSTPDMHND;

/**
 * A PDM thread.
 */
typedef struct TSTPDMTHREAD
{
    /** What the device sees. */
    PDMTHREAD           Core;
    /** Signalled when the thread shall leave the suspended state. */
    RTSEMEVENT          hEvtRun;
    /** Signalled by the thread when it completed a state change. */
    RTSEMEVENT          hEvtState;
} TSTPDMTHREAD;
/** Pointer to a PDM thread. */
typedef TSTPDMTHREAD *PTSTPDMTHREAD;

/** @name The kinds of items in a saved state of the mock.
 * The real thing doesn't tell them apart.  Here it shows when what is read isn't
 * what was written.
 * @{ */
#define TSTPDM_SSM_ITEM_U8          UINT8_C(0x01)
#define TSTPDM_SSM_ITEM_U32         UINT8_C(0x02)
#define TSTPDM_SSM_ITEM_U64         UINT8_C(0x03)
#define TSTPDM_SSM_ITEM_S32         UINT8_C(0x04)
#define TSTPDM_SSM_ITEM_BOOL        UINT8_C(0x05)
#define TSTPDM_SSM_ITEM_MEM         UINT8_C(0x06)
/** @} */

/**
 * A saved state unit.
 */
typedef struct TSTPDMSSMUNIT
{
    /** The name, which is the name of the device. */
    char                szName[32];
    /** The instance. */
    uint32_t            uInstance;
    /** The version of the data. */
    uint32_t            uVersion;
    /** The device instance, NULL for the unit of the mock itself. */
    PPDMDEVINS          pDevIns;
    /** The callback preparing for a live save, optional. */
    PFNSSMDEVLIVEPREP   pfnLivePrep;
    /** The callback preparing for saving, optional. */
    PFNSSMDEVSAVEPREP   pfnSavePrep;
    /** The callback saving the data, optional. */
    PFNSSMDEVSAVEEXEC   pfnSaveExec;
    /** The callback completing the saving, optional. */
    PFNSSMDEVSAVEDONE   pfnSaveDone;
    /** The callback preparing for loading, optional. */
    PFNSSMDEVLOADPREP   pfnLoadPrep;
    /** The callback loading the data, optional. */
    PFNSSMDEVLOADEXEC   pfnLoadExec;
    /** The callback completing the loading, optional. */
    PFNSSMDEVLOADDONE   pfnLoadDone;
    /** Whether a callback of the unit was called during the current operation. */
    bool                fCalled;
} TSTPDMSSMUNIT;
/** Pointer to a saved state unit. */
typedef TSTPDMSSMUNIT *PTSTPDMSSMUNIT;

/**
 * The header of a unit in a saved state of the mock.
 */
typedef struct TSTPDMSSMUNITHDR
{
    /** The name of the unit, empty for the end of the state. */
    char                szName[32];
    /** The instance. */
    uint32_t            uInstance;
    /** The version of the data. */
    uint32_t            uVersion;
    /** Size of the data which follows. */
    uint32_t            cbData;
} TSTPDMSSMUNITHDR;

/**
 * A saved state which is being written or read.
 */
struct SSMHANDLE
{
    /** The data. */
    uint8_t            *pb;
    /** Size of the data. */
    size_t              cb;
    /** Size of the buffer when writing. */
    size_t              cbAlloc;
    /** The read position. */
    size_t              off;
    /** Where the data of the unit being read ends. */
    size_t              offUnitEnd;
    /** The status, a failure sticks. */
    int                 rc;
    /** Whether the state is being read. */
    bool                fLoading;
    /** Whether a unit is saving or loading its data right now, which is when data
     * can be put and got. */
    bool                fExec;
};

/**
 * The state of the mock.
 */
typedef struct TSTPDM
{
    /** Whether the mock is initialized. */
    bool                fInitialized;
    /** Whether the VM is still being constructed. */
    bool                fCreating;
    /** Whether the devices were told that the VM construction is complete. */
    bool                fInitCompleteDone;
    /** Whether the VM is powered on. */
    bool                fRunning;
    /** The main execution engine the devices get to see, VM_EXEC_ENGINE_XXX. */
    uint8_t             bExecEngine;
    /** The guest RAM, starting at guest physical address 0. */
    uint8_t            *pbRam;
    /** Size of the guest RAM. */
    uint32_t            cbRam;
    /** The device instances in creation order. */
    TSTPDMDEV           aDevs[8];
    /** Number of device instances. */
    uint32_t            cDevs;
    /** Device instances whose constructor failed; kept until termination because ranges may refer to them. */
    TSTPDMDEV           aZombies[8];
    /** Number of failed device instances. */
    uint32_t            cZombies;
    /** The PCI buses. */
    TSTPDMPCIBUS        aPciBuses[8];
    /** The I/O port ranges. */
    TSTPDMIOPORT        aIoPorts[32];
    /** Number of I/O port ranges. */
    uint32_t            cIoPorts;
    /** The MMIO ranges. */
    TSTPDMMMIO          aMmio[64];
    /** Number of MMIO ranges. */
    uint32_t            cMmio;
    /** The MMIO2 ranges. */
    TSTPDMMMIO2         aMmio2[96];
    /** Number of MMIO2 ranges. */
    uint32_t            cMmio2;
    /** The physical access handler types. */
    TSTPDMHNDTYPE       aHndTypes[32];
    /** Number of physical access handler types. */
    uint32_t            cHndTypes;
    /** The physical access handlers. */
    TSTPDMHND           aHnds[32];
    /** The PDM threads. */
    PTSTPDMTHREAD       apThreads[64];
    /** Number of PDM threads. */
    uint32_t            cThreads;
    /** The PCI lock, which is the PDM lock with the real thing. */
    RTCRITSECT          CritSectPci;
    /** Protects the interrupt log. */
    RTCRITSECT          CritSectIrq;
    /** Signalled whenever an MSI arrives. */
    RTSEMEVENT          hEvtMsi;
    /** The MSIs delivered to the interrupt controller. */
    TSTPDMMSI           aMsis[256];
    /** Number of MSIs delivered. */
    uint32_t            cMsis;
    /** The MSI which was delivered last. */
    TSTPDMMSI           LastMsi;
    /** Requests waiting for the thread playing the EMT, protected by CritSectIrq. */
    struct
    {
        /** The function to call. */
        PFNRT           pfn;
        /** Number of arguments. */
        unsigned        cArgs;
        /** The arguments. */
        uintptr_t       auArgs[4];
    }                   aReqs[16];
    /** Number of requests waiting. */
    uint32_t            cReqs;
    /** Number of requests made so far. */
    uint32_t volatile   cReqsTotal;
    /** Whether requests wait until the test asks for them to be done. */
    bool                fManualReqs;
    /** Number of blocks currently allocated from the MM heap. */
    uint32_t volatile   cHeapAllocs;
    /** The last VM error message. */
    char                szLastError[512];
    /** The ID of the last runtime error. */
    char                szLastRuntimeErrorId[64];
    /** The state of the VM the devices get to see. */
    VMSTATE             enmVmState;
    /** The saved state units in the order in which they are saved. */
    TSTPDMSSMUNIT       aSsmUnits[16];
    /** Number of saved state units. */
    uint32_t            cSsmUnits;
    /** Called between the preparations for saving or loading a state and doing it, optional. */
    PFNTSTPDMSTATEHOOK  pfnStateHook;
    /** The user argument of the hook. */
    void               *pvStateHookUser;
    /** The NOP critical section handed to the devices. */
    PDMCRITSECT         NopCritSect;
} TSTPDM;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/** The mock. */
static TSTPDM       g_Pdm;
/** The device helpers. */
static PDMDEVHLPR3  g_tstPdmDevHlpR3;
/** The PCI helpers. */
static PDMPCIHLPR3  g_tstPdmPciHlpR3;


/*
 *
 * Configuration tree.
 * Configuration tree.
 * Configuration tree.
 *
 */

/**
 * Finds a value of a node.
 *
 * @returns Pointer to the value or NULL if not found.
 * @param   pNode               The node, can be NULL.
 * @param   pszName             The name of the value.
 */
static PTSTCFGVALUE tstCfgFindValue(PCFGMNODE pNode, const char *pszName)
{
    if (pNode)
        for (PTSTCFGVALUE pValue = pNode->pFirstValue; pValue; pValue = pValue->pNext)
            if (!strcmp(pValue->szName, pszName))
                return pValue;
    return NULL;
}


/**
 * Finds a child of a node.
 *
 * @returns Pointer to the child or NULL if not found.
 * @param   pNode               The node, can be NULL.
 * @param   pszName             The name of the child.
 */
static PCFGMNODE tstCfgFindChild(PCFGMNODE pNode, const char *pszName)
{
    if (pNode)
        for (PCFGMNODE pChild = pNode->pFirstChild; pChild; pChild = pChild->pNext)
            if (!strcmp(pChild->szName, pszName))
                return pChild;
    return NULL;
}


/**
 * Queries an integer value of a node.
 *
 * @returns true if the value exists and is an integer, false otherwise.
 * @param   pNode               The node, can be NULL.
 * @param   pszName             The name of the value.
 * @param   pu64                Where to return the value.
 */
static bool tstCfgQueryInteger(PCFGMNODE pNode, const char *pszName, uint64_t *pu64)
{
    PTSTCFGVALUE pValue = tstCfgFindValue(pNode, pszName);
    if (!pValue || pValue->fString)
        return false;
    *pu64 = pValue->u64Value;
    return true;
}


/**
 * Creates or looks up a value of a node, releasing the old content.
 *
 * @returns Pointer to the value or NULL if out of memory.
 * @param   pNode               The node.
 * @param   pszName             The name of the value.
 */
static PTSTCFGVALUE tstCfgValueGet(PCFGMNODE pNode, const char *pszName)
{
    PTSTCFGVALUE pValue = tstCfgFindValue(pNode, pszName);
    if (pValue)
    {
        RTStrFree(pValue->pszValue);
        pValue->pszValue = NULL;
        return pValue;
    }

    AssertReturn(strlen(pszName) < sizeof(pValue->szName), NULL);
    pValue = (PTSTCFGVALUE)RTMemAllocZ(sizeof(*pValue));
    if (pValue)
    {
        RTStrCopy(pValue->szName, sizeof(pValue->szName), pszName);
        pValue->pNext      = pNode->pFirstValue;
        pNode->pFirstValue = pValue;
    }
    return pValue;
}


/**
 * Creates a configuration node.
 *
 * @returns The new node, NULL if out of memory.
 * @param   pParent             The parent, NULL for a root node.
 * @param   pszName             The name of the node.
 */
PCFGMNODE tstCfgNodeCreate(PCFGMNODE pParent, const char *pszName)
{
    PCFGMNODE pNode = NULL;
    AssertReturn(strlen(pszName) < sizeof(pNode->szName), NULL);

    pNode = (PCFGMNODE)RTMemAllocZ(sizeof(*pNode));
    if (pNode)
    {
        RTStrCopy(pNode->szName, sizeof(pNode->szName), pszName);
        pNode->pParent = pParent;
        if (pParent)
        {
            PCFGMNODE *ppLink = &pParent->pFirstChild;
            while (*ppLink)
                ppLink = &(*ppLink)->pNext;
            *ppLink = pNode;
        }
    }
    return pNode;
}


/**
 * Destroys a configuration node with everything below it.
 *
 * @param   pNode               The node, must be a root node or NULL.
 */
void tstCfgNodeDestroy(PCFGMNODE pNode)
{
    if (!pNode)
        return;

    while (pNode->pFirstChild)
    {
        PCFGMNODE pChild = pNode->pFirstChild;
        pNode->pFirstChild = pChild->pNext;
        pChild->pNext = NULL;
        tstCfgNodeDestroy(pChild);
    }
    while (pNode->pFirstValue)
    {
        PTSTCFGVALUE pValue = pNode->pFirstValue;
        pNode->pFirstValue = pValue->pNext;
        RTStrFree(pValue->pszValue);
        RTMemFree(pValue);
    }
    RTMemFree(pNode);
}


/**
 * Sets an integer value.
 *
 * @returns VBox status code.
 * @param   pNode               The node.
 * @param   pszName             The name of the value.
 * @param   u64Value            The value.
 */
int tstCfgSetInteger(PCFGMNODE pNode, const char *pszName, uint64_t u64Value)
{
    PTSTCFGVALUE pValue = tstCfgValueGet(pNode, pszName);
    AssertReturn(pValue, VERR_NO_MEMORY);
    pValue->fString  = false;
    pValue->u64Value = u64Value;
    return VINF_SUCCESS;
}


/**
 * Sets a string value.
 *
 * @returns VBox status code.
 * @param   pNode               The node.
 * @param   pszName             The name of the value.
 * @param   pszValue            The value.
 */
int tstCfgSetString(PCFGMNODE pNode, const char *pszName, const char *pszValue)
{
    PTSTCFGVALUE pValue = tstCfgValueGet(pNode, pszName);
    AssertReturn(pValue, VERR_NO_MEMORY);
    pValue->fString  = true;
    pValue->pszValue = RTStrDup(pszValue);
    return pValue->pszValue ? VINF_SUCCESS : VERR_NO_STR_MEMORY;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCFGMQueryString} */
static DECLCALLBACK(int) tstPdmHlp_CFGMQueryString(PCFGMNODE pNode, const char *pszName, char *pszString, size_t cchString)
{
    if (!pNode)
        return VERR_CFGM_NO_PARENT;
    PTSTCFGVALUE pValue = tstCfgFindValue(pNode, pszName);
    if (!pValue)
        return VERR_CFGM_VALUE_NOT_FOUND;
    if (!pValue->fString)
        return VERR_CFGM_NOT_STRING;
    size_t const cchValue = strlen(pValue->pszValue);
    if (cchValue >= cchString)
        return VERR_CFGM_NOT_ENOUGH_SPACE;
    memcpy(pszString, pValue->pszValue, cchValue + 1);
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCFGMQueryStringDef} */
static DECLCALLBACK(int) tstPdmHlp_CFGMQueryStringDef(PCFGMNODE pNode, const char *pszName, char *pszString, size_t cchString,
                                                      const char *pszDef)
{
    int rc = tstPdmHlp_CFGMQueryString(pNode, pszName, pszString, cchString);
    if (rc == VERR_CFGM_VALUE_NOT_FOUND || rc == VERR_CFGM_NO_PARENT)
    {
        size_t const cchDef = strlen(pszDef);
        if (cchDef >= cchString)
            return VERR_CFGM_NOT_ENOUGH_SPACE;
        memcpy(pszString, pszDef, cchDef + 1);
        rc = VINF_SUCCESS;
    }
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCFGMQueryU64Def} */
static DECLCALLBACK(int) tstPdmHlp_CFGMQueryU64Def(PCFGMNODE pNode, const char *pszName, uint64_t *pu64, uint64_t u64Def)
{
    PTSTCFGVALUE pValue = tstCfgFindValue(pNode, pszName);
    if (!pValue)
    {
        *pu64 = u64Def;
        return VINF_SUCCESS;
    }
    if (pValue->fString)
        return VERR_CFGM_NOT_INTEGER;
    *pu64 = pValue->u64Value;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCFGMQueryBoolDef} */
static DECLCALLBACK(int) tstPdmHlp_CFGMQueryBoolDef(PCFGMNODE pNode, const char *pszName, bool *pf, bool fDef)
{
    uint64_t u64Value = 0;
    int rc = tstPdmHlp_CFGMQueryU64Def(pNode, pszName, &u64Value, fDef);
    if (RT_SUCCESS(rc))
        *pf = u64Value != 0;
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCFGMGetChildFV} */
static DECLCALLBACK(PCFGMNODE) tstPdmHlp_CFGMGetChildFV(PCFGMNODE pNode, const char *pszPathFormat, va_list Args)
{
    char szName[64];
    ssize_t cch = RTStrPrintf2V(szName, sizeof(szName), pszPathFormat, Args);
    AssertReturn(cch > 0, NULL);
    return tstCfgFindChild(pNode, szName);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCFGMGetChildF} */
static DECLCALLBACK(PCFGMNODE) tstPdmHlp_CFGMGetChildF(PCFGMNODE pNode, const char *pszPathFormat, ...)
{
    va_list Args;
    va_start(Args, pszPathFormat);
    PCFGMNODE pChild = tstPdmHlp_CFGMGetChildFV(pNode, pszPathFormat, Args);
    va_end(Args);
    return pChild;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCFGMAreValuesValid} */
static DECLCALLBACK(bool) tstPdmHlp_CFGMAreValuesValid(PCFGMNODE pNode, const char *pszzValid)
{
    if (pNode)
        for (PTSTCFGVALUE pValue = pNode->pFirstValue; pValue; pValue = pValue->pNext)
        {
            bool fFound = false;
            for (const char *psz = pszzValid; *psz && !fFound; psz += strlen(psz) + 1)
                fFound = !strcmp(psz, pValue->szName);
            if (!fFound)
                return false;
        }
    return true;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCFGMValidateConfig} */
static DECLCALLBACK(int) tstPdmHlp_CFGMValidateConfig(PCFGMNODE pNode, const char *pszNode, const char *pszValidValues,
                                                      const char *pszValidNodes, const char *pszWho, uint32_t uInstance)
{
    RT_NOREF(pszNode, pszWho, uInstance);
    if (pNode)
    {
        for (PTSTCFGVALUE pValue = pNode->pFirstValue; pValue; pValue = pValue->pNext)
            if (!RTStrSimplePatternMultiMatch(pszValidValues, RTSTR_MAX, pValue->szName, RTSTR_MAX, NULL))
                return VERR_CFGM_CONFIG_UNKNOWN_VALUE;
        for (PCFGMNODE pChild = pNode->pFirstChild; pChild; pChild = pChild->pNext)
            if (!RTStrSimplePatternMultiMatch(pszValidNodes, RTSTR_MAX, pChild->szName, RTSTR_MAX, NULL))
                return VERR_CFGM_CONFIG_UNKNOWN_NODE;
    }
    return VINF_SUCCESS;
}


/*
 *
 * Device helpers: I/O ports, MMIO, MMIO2, memory.
 * Device helpers: I/O ports, MMIO, MMIO2, memory.
 * Device helpers: I/O ports, MMIO, MMIO2, memory.
 *
 */

/** @interface_method_impl{PDMDEVHLPR3,pfnIoPortCreateEx} */
static DECLCALLBACK(int) tstPdmHlp_IoPortCreateEx(PPDMDEVINS pDevIns, RTIOPORT cPorts, uint32_t fFlags, PPDMPCIDEV pPciDev,
                                                  uint32_t iPciRegion, PFNIOMIOPORTNEWOUT pfnOut, PFNIOMIOPORTNEWIN pfnIn,
                                                  PFNIOMIOPORTNEWOUTSTRING pfnOutStr, PFNIOMIOPORTNEWINSTRING pfnInStr,
                                                  RTR3PTR pvUser, const char *pszDesc, PCIOMIOPORTDESC paExtDescs,
                                                  PIOMIOPORTHANDLE phIoPorts)
{
    RT_NOREF(pPciDev, iPciRegion, pfnOutStr, pfnInStr, pszDesc, paExtDescs);
    *phIoPorts = NIL_IOMIOPORTHANDLE;
    AssertReturn(g_Pdm.fCreating, VERR_VM_INVALID_VM_STATE); /* Same restriction as IOMR3IoPortCreate. */
    AssertReturn(cPorts > 0, VERR_OUT_OF_RANGE);
    AssertReturn(g_Pdm.cIoPorts < RT_ELEMENTS(g_Pdm.aIoPorts), VERR_OUT_OF_RESOURCES);

    uint32_t const idx = g_Pdm.cIoPorts++;
    g_Pdm.aIoPorts[idx].pDevIns = pDevIns;
    g_Pdm.aIoPorts[idx].cPorts  = cPorts;
    g_Pdm.aIoPorts[idx].uPort   = 0;
    g_Pdm.aIoPorts[idx].fMapped = false;
    g_Pdm.aIoPorts[idx].fFlags  = fFlags;
    g_Pdm.aIoPorts[idx].pfnOut  = pfnOut;
    g_Pdm.aIoPorts[idx].pfnIn   = pfnIn;
    g_Pdm.aIoPorts[idx].pvUser  = pvUser;
    *phIoPorts = idx;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnIoPortMap} */
static DECLCALLBACK(int) tstPdmHlp_IoPortMap(PPDMDEVINS pDevIns, IOMIOPORTHANDLE hIoPorts, RTIOPORT Port)
{
    AssertReturn(hIoPorts < g_Pdm.cIoPorts, VERR_IOM_INVALID_IOPORT_HANDLE);
    AssertReturn(g_Pdm.aIoPorts[hIoPorts].pDevIns == pDevIns, VERR_IOM_INVALID_IOPORT_HANDLE);
    AssertReturn(!g_Pdm.aIoPorts[hIoPorts].fMapped, VERR_IOM_IOPORTS_ALREADY_MAPPED);
    g_Pdm.aIoPorts[hIoPorts].uPort   = Port;
    g_Pdm.aIoPorts[hIoPorts].fMapped = true;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnIoPortUnmap} */
static DECLCALLBACK(int) tstPdmHlp_IoPortUnmap(PPDMDEVINS pDevIns, IOMIOPORTHANDLE hIoPorts)
{
    AssertReturn(hIoPorts < g_Pdm.cIoPorts, VERR_IOM_INVALID_IOPORT_HANDLE);
    AssertReturn(g_Pdm.aIoPorts[hIoPorts].pDevIns == pDevIns, VERR_IOM_INVALID_IOPORT_HANDLE);
    AssertReturn(g_Pdm.aIoPorts[hIoPorts].fMapped, VERR_IOM_IOPORTS_NOT_MAPPED);
    g_Pdm.aIoPorts[hIoPorts].fMapped = false;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMmioCreateEx} */
static DECLCALLBACK(int) tstPdmHlp_MmioCreateEx(PPDMDEVINS pDevIns, RTGCPHYS cbRegion, uint32_t fFlags, PPDMPCIDEV pPciDev,
                                                uint32_t iPciRegion, PFNIOMMMIONEWWRITE pfnWrite, PFNIOMMMIONEWREAD pfnRead,
                                                PFNIOMMMIONEWFILL pfnFill, void *pvUser, const char *pszDesc,
                                                PIOMMMIOHANDLE phRegion)
{
    RT_NOREF(pPciDev, iPciRegion, pszDesc);
    *phRegion = NIL_IOMMMIOHANDLE;
    AssertReturn(g_Pdm.fCreating, VERR_VM_INVALID_VM_STATE); /* Same restrictions as IOMR3MmioCreate. */
    AssertReturn(cbRegion > 0, VERR_OUT_OF_RANGE);
    AssertReturn(!(cbRegion & GUEST_PAGE_OFFSET_MASK), VERR_UNSUPPORTED_ALIGNMENT);
    AssertReturn(g_Pdm.cMmio < RT_ELEMENTS(g_Pdm.aMmio), VERR_OUT_OF_RESOURCES);

    uint32_t const idx = g_Pdm.cMmio++;
    g_Pdm.aMmio[idx].pDevIns  = pDevIns;
    g_Pdm.aMmio[idx].cbRegion = cbRegion;
    g_Pdm.aMmio[idx].GCPhys   = NIL_RTGCPHYS;
    g_Pdm.aMmio[idx].fFlags   = fFlags;
    g_Pdm.aMmio[idx].pfnWrite = pfnWrite;
    g_Pdm.aMmio[idx].pfnRead  = pfnRead;
    g_Pdm.aMmio[idx].pfnFill  = pfnFill;
    g_Pdm.aMmio[idx].pvUser   = pvUser;
    *phRegion = idx;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMmioMap} */
static DECLCALLBACK(int) tstPdmHlp_MmioMap(PPDMDEVINS pDevIns, IOMMMIOHANDLE hRegion, RTGCPHYS GCPhys)
{
    AssertReturn(hRegion < g_Pdm.cMmio, VERR_IOM_INVALID_MMIO_HANDLE);
    AssertReturn(g_Pdm.aMmio[hRegion].pDevIns == pDevIns, VERR_IOM_INVALID_MMIO_HANDLE);
    AssertReturn(g_Pdm.aMmio[hRegion].GCPhys == NIL_RTGCPHYS, VERR_IOM_MMIO_REGION_ALREADY_MAPPED);
    g_Pdm.aMmio[hRegion].GCPhys = GCPhys;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMmioUnmap} */
static DECLCALLBACK(int) tstPdmHlp_MmioUnmap(PPDMDEVINS pDevIns, IOMMMIOHANDLE hRegion)
{
    AssertReturn(hRegion < g_Pdm.cMmio, VERR_IOM_INVALID_MMIO_HANDLE);
    AssertReturn(g_Pdm.aMmio[hRegion].pDevIns == pDevIns, VERR_IOM_INVALID_MMIO_HANDLE);
    AssertReturn(g_Pdm.aMmio[hRegion].GCPhys != NIL_RTGCPHYS, VERR_IOM_MMIO_REGION_NOT_MAPPED);
    g_Pdm.aMmio[hRegion].GCPhys = NIL_RTGCPHYS;
    return VINF_SUCCESS;
}


/**
 * Registers an MMIO2 range.
 *
 * @returns VBox status code.
 * @param   pDevIns             The owner.
 * @param   pPciDev             The PCI device the range is associated with, can be NULL.
 * @param   iPciRegion          The region number in the high word, the low word has to be zero.
 * @param   cbRegion            The size of the range.
 * @param   pvBacking           The existing backing memory, NULL to allocate it.
 * @param   ppvMapping          Where to return the backing memory, optional.
 * @param   phRegion            Where to return the handle.
 */
static int tstPdmMmio2Register(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t iPciRegion, RTGCPHYS cbRegion,
                               void *pvBacking, void **ppvMapping, PPGMMMIO2HANDLE phRegion)
{
    *phRegion = NIL_PGMMMIO2HANDLE;

    /* Same restrictions as pdmR3DevHlp_Mmio2Create and PGMR3PhysMmio2Register (the saved
       state loading exception does not apply here). */
    AssertReturn(!pPciDev || pPciDev->Int.s.pDevInsR3 == pDevIns, VERR_INVALID_PARAMETER);
    AssertReturn(!(iPciRegion & UINT16_MAX), VERR_INVALID_PARAMETER);
    uint32_t const iSubDev = pPciDev ? pPciDev->Int.s.idxDevCfg : 254;
    uint32_t const iRegion = iPciRegion >> 16;
    AssertReturn(iRegion <= UINT8_MAX, VERR_INVALID_PARAMETER);
    AssertReturn(g_Pdm.fCreating, VERR_VM_INVALID_VM_STATE);
    AssertReturn(cbRegion > 0, VERR_INVALID_PARAMETER);
    AssertReturn(!(cbRegion & GUEST_PAGE_OFFSET_MASK), VERR_INVALID_PARAMETER);
    for (uint32_t i = 0; i < g_Pdm.cMmio2; i++)
        AssertReturn(   g_Pdm.aMmio2[i].pDevIns != pDevIns
                     || g_Pdm.aMmio2[i].iSubDev != iSubDev
                     || g_Pdm.aMmio2[i].iRegion != iRegion,
                     VERR_ALREADY_EXISTS);
    AssertReturn(g_Pdm.cMmio2 < TSTPDM_MAX_MMIO2_RANGES, VERR_PGM_TOO_MANY_MMIO2_RANGES);

    bool const fOwned = pvBacking == NULL;
    if (fOwned)
    {
        pvBacking = RTMemPageAllocZ((size_t)cbRegion);
        if (!pvBacking)
            return VERR_NO_MEMORY;
    }

    uint32_t const idx = g_Pdm.cMmio2++;
    g_Pdm.aMmio2[idx].pDevIns   = pDevIns;
    g_Pdm.aMmio2[idx].iSubDev   = iSubDev;
    g_Pdm.aMmio2[idx].iRegion   = iRegion;
    g_Pdm.aMmio2[idx].cbRegion  = cbRegion;
    g_Pdm.aMmio2[idx].GCPhys    = NIL_RTGCPHYS;
    g_Pdm.aMmio2[idx].pbBacking = (uint8_t *)pvBacking;
    g_Pdm.aMmio2[idx].fOwned    = fOwned;
    if (ppvMapping)
        *ppvMapping = pvBacking;
    *phRegion = idx;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMmio2Create} */
static DECLCALLBACK(int) tstPdmHlp_Mmio2Create(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t iPciRegion, RTGCPHYS cbRegion,
                                               uint32_t fFlags, const char *pszDesc, void **ppvMapping, PPGMMMIO2HANDLE phRegion)
{
    RT_NOREF(pszDesc);
    AssertReturn(!(fFlags & ~PGMPHYS_MMIO2_FLAGS_TRACK_DIRTY_PAGES), VERR_INVALID_FLAGS);
    return tstPdmMmio2Register(pDevIns, pPciDev, iPciRegion, cbRegion, NULL /*pvBacking*/, ppvMapping, phRegion);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMmio2CreateFromExisting} */
static DECLCALLBACK(int) tstPdmHlp_Mmio2CreateFromExisting(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t iPciRegion,
                                                           RTGCPHYS cbRegion, uint32_t fFlags, const char *pszDesc,
                                                           void *pvBacking, PPGMMMIO2HANDLE phRegion)
{
    RT_NOREF(pszDesc);
    AssertReturn(!fFlags, VERR_INVALID_FLAGS);
    AssertPtrReturn(pvBacking, VERR_INVALID_POINTER);
    return tstPdmMmio2Register(pDevIns, pPciDev, iPciRegion, cbRegion, pvBacking, NULL /*ppvMapping*/, phRegion);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMmio2Map} */
static DECLCALLBACK(int) tstPdmHlp_Mmio2Map(PPDMDEVINS pDevIns, PGMMMIO2HANDLE hRegion, RTGCPHYS GCPhys)
{
    AssertReturn(hRegion < g_Pdm.cMmio2, VERR_INVALID_HANDLE);
    AssertReturn(g_Pdm.aMmio2[hRegion].pDevIns == pDevIns, VERR_INVALID_HANDLE);
    AssertReturn(g_Pdm.aMmio2[hRegion].GCPhys == NIL_RTGCPHYS, VERR_WRONG_ORDER);
    g_Pdm.aMmio2[hRegion].GCPhys = GCPhys;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMmio2Unmap} */
static DECLCALLBACK(int) tstPdmHlp_Mmio2Unmap(PPDMDEVINS pDevIns, PGMMMIO2HANDLE hRegion)
{
    AssertReturn(hRegion < g_Pdm.cMmio2, VERR_INVALID_HANDLE);
    AssertReturn(g_Pdm.aMmio2[hRegion].pDevIns == pDevIns, VERR_INVALID_HANDLE);
    AssertReturn(g_Pdm.aMmio2[hRegion].GCPhys != NIL_RTGCPHYS, VERR_WRONG_ORDER);
    g_Pdm.aMmio2[hRegion].GCPhys = NIL_RTGCPHYS;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPGMHandlerPhysicalTypeRegister} */
static DECLCALLBACK(int) tstPdmHlp_PGMHandlerPhysicalTypeRegister(PPDMDEVINS pDevIns, PGMPHYSHANDLERKIND enmKind,
                                                                  PFNPGMPHYSHANDLER pfnHandler, const char *pszDesc,
                                                                  PPGMPHYSHANDLERTYPE phType)
{
    RT_NOREF(pDevIns, pszDesc);
    AssertReturn(g_Pdm.fCreating, VERR_VM_INVALID_VM_STATE);
    AssertReturn(g_Pdm.cHndTypes < RT_ELEMENTS(g_Pdm.aHndTypes), VERR_OUT_OF_RESOURCES);
    uint32_t const idx = g_Pdm.cHndTypes++;
    g_Pdm.aHndTypes[idx].enmKind    = enmKind;
    g_Pdm.aHndTypes[idx].pfnHandler = pfnHandler;
    *phType = idx;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPGMHandlerPhysicalRegister} */
static DECLCALLBACK(int) tstPdmHlp_PGMHandlerPhysicalRegister(PPDMDEVINS pDevIns, RTGCPHYS GCPhys, RTGCPHYS GCPhysLast,
                                                              PGMPHYSHANDLERTYPE hType, R3PTRTYPE(const char *) pszDesc)
{
    RT_NOREF(pDevIns, pszDesc);
    AssertReturn(hType < g_Pdm.cHndTypes, VERR_INVALID_HANDLE);
    AssertReturn(GCPhysLast >= GCPhys, VERR_INVALID_PARAMETER);
    for (unsigned i = 0; i < RT_ELEMENTS(g_Pdm.aHnds); i++)
        if (!g_Pdm.aHnds[i].fUsed)
        {
            g_Pdm.aHnds[i].fUsed      = true;
            g_Pdm.aHnds[i].GCPhys     = GCPhys;
            g_Pdm.aHnds[i].GCPhysLast = GCPhysLast;
            g_Pdm.aHnds[i].idxType    = (uint32_t)hType;
            return VINF_SUCCESS;
        }
    return VERR_OUT_OF_RESOURCES;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPGMHandlerPhysicalDeregister} */
static DECLCALLBACK(int) tstPdmHlp_PGMHandlerPhysicalDeregister(PPDMDEVINS pDevIns, RTGCPHYS GCPhys)
{
    RT_NOREF(pDevIns);
    for (unsigned i = 0; i < RT_ELEMENTS(g_Pdm.aHnds); i++)
        if (g_Pdm.aHnds[i].fUsed && g_Pdm.aHnds[i].GCPhys == GCPhys)
        {
            g_Pdm.aHnds[i].fUsed = false;
            return VINF_SUCCESS;
        }
    return VERR_PGM_HANDLER_NOT_FOUND;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPhysGCPhys2CCPtr} */
static DECLCALLBACK(int) tstPdmHlp_PhysGCPhys2CCPtr(PPDMDEVINS pDevIns, RTGCPHYS GCPhys, uint32_t fFlags, void **ppv,
                                                    PPGMPAGEMAPLOCK pLock)
{
    RT_NOREF(pDevIns, fFlags, pLock);
    if (GCPhys >= g_Pdm.cbRam)
        return VERR_PGM_INVALID_GC_PHYSICAL_ADDRESS;
    /* Like in a PC there is no RAM behind the legacy video memory and the ROMs below 1 MiB. */
    if (GCPhys - TSTPDM_RAM_HOLE_START < TSTPDM_RAM_HOLE_SIZE)
        return VERR_PGM_PHYS_PAGE_RESERVED;
    *ppv = &g_Pdm.pbRam[GCPhys];
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPhysReleasePageMappingLock} */
static DECLCALLBACK(void) tstPdmHlp_PhysReleasePageMappingLock(PPDMDEVINS pDevIns, PPGMPAGEMAPLOCK pLock)
{
    RT_NOREF(pDevIns, pLock);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMMPhysGetRamSizeBelow4GB} */
static DECLCALLBACK(uint32_t) tstPdmHlp_MMPhysGetRamSizeBelow4GB(PPDMDEVINS pDevIns)
{
    RT_NOREF(pDevIns);
    return g_Pdm.cbRam;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMMPhysGetRamSizeAbove4GB} */
static DECLCALLBACK(uint64_t) tstPdmHlp_MMPhysGetRamSizeAbove4GB(PPDMDEVINS pDevIns)
{
    RT_NOREF(pDevIns);
    return 0;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMReqCallNoWaitV} */
static DECLCALLBACK(int) tstPdmHlp_VMReqCallNoWaitV(PPDMDEVINS pDevIns, VMCPUID idDstCpu, PFNRT pfnFunction, unsigned cArgs,
                                                    va_list Args)
{
    RT_NOREF(pDevIns, idDstCpu);
    AssertReturn(cArgs <= RT_ELEMENTS(g_Pdm.aReqs[0].auArgs), VERR_TOO_MUCH_DATA);

    /* It gets called by whoever plays the EMT when it gets around to it. */
    int rc = VINF_SUCCESS;
    RTCritSectEnter(&g_Pdm.CritSectIrq);
    if (g_Pdm.cReqs < RT_ELEMENTS(g_Pdm.aReqs))
    {
        g_Pdm.aReqs[g_Pdm.cReqs].pfn   = pfnFunction;
        g_Pdm.aReqs[g_Pdm.cReqs].cArgs = cArgs;
        for (unsigned i = 0; i < cArgs; i++)
            g_Pdm.aReqs[g_Pdm.cReqs].auArgs[i] = va_arg(Args, uintptr_t);
        g_Pdm.cReqs++;
        g_Pdm.cReqsTotal++;
    }
    else
        rc = VERR_NO_MEMORY;
    RTCritSectLeave(&g_Pdm.CritSectIrq);
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMReqPriorityCallWaitV} */
static DECLCALLBACK(int) tstPdmHlp_VMReqPriorityCallWaitV(PPDMDEVINS pDevIns, VMCPUID idDstCpu, PFNRT pfnFunction, unsigned cArgs,
                                                          va_list Args)
{
    RT_NOREF(pDevIns, idDstCpu);
    AssertReturn(cArgs <= 4, VERR_TOO_MUCH_DATA);
    uintptr_t auArgs[4] = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < cArgs; i++)
        auArgs[i] = va_arg(Args, uintptr_t);

    /* Whoever calls this plays the EMT.  A request like this one is done ahead of those
       which nobody waits for, so they stay where they are. */
    switch (cArgs)
    {
        case 0:  return ((int (*)(void))pfnFunction)();
        case 1:  return ((int (*)(uintptr_t))pfnFunction)(auArgs[0]);
        case 2:  return ((int (*)(uintptr_t, uintptr_t))pfnFunction)(auArgs[0], auArgs[1]);
        case 3:  return ((int (*)(uintptr_t, uintptr_t, uintptr_t))pfnFunction)(auArgs[0], auArgs[1], auArgs[2]);
        default: return ((int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))pfnFunction)(auArgs[0], auArgs[1], auArgs[2], auArgs[3]);
    }
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetMainExecutionEngine} */
static DECLCALLBACK(uint8_t) tstPdmHlp_GetMainExecutionEngine(PPDMDEVINS pDevIns)
{
    RT_NOREF(pDevIns);
    return g_Pdm.bExecEngine;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMMHeapAllocZ} */
static DECLCALLBACK(void *) tstPdmHlp_MMHeapAllocZ(PPDMDEVINS pDevIns, size_t cb)
{
    RT_NOREF(pDevIns);
    void *pv = RTMemAllocZ(cb);
    if (pv)
        ASMAtomicIncU32(&g_Pdm.cHeapAllocs);
    return pv;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMMHeapFree} */
static DECLCALLBACK(void) tstPdmHlp_MMHeapFree(PPDMDEVINS pDevIns, void *pv)
{
    RT_NOREF(pDevIns);
    if (pv)
    {
        ASMAtomicDecU32(&g_Pdm.cHeapAllocs);
        RTMemFree(pv);
    }
}


/*
 *
 * Device helpers: PCI.
 * Device helpers: PCI.
 * Device helpers: PCI.
 *
 */

/**
 * Gets the PCI bus a PCI device is registered with.
 *
 * @returns Pointer to the bus or NULL.
 * @param   pPciDev             The PCI device.
 */
static PTSTPDMPCIBUS tstPdmPciBusFromDev(PPDMPCIDEV pPciDev)
{
    size_t const idxBus = pPciDev->Int.s.idxPdmBus;
    AssertReturn(idxBus < RT_ELEMENTS(g_Pdm.aPciBuses), NULL);
    AssertReturn(g_Pdm.aPciBuses[idxBus].pDevIns, NULL);
    return &g_Pdm.aPciBuses[idxBus];
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIBusRegister} */
static DECLCALLBACK(int) tstPdmHlp_PCIBusRegister(PPDMDEVINS pDevIns, PPDMPCIBUSREGR3 pPciBusReg, PCPDMPCIHLPR3 *ppPciHlp,
                                                  uint32_t *piBus)
{
    AssertReturn(pPciBusReg->u32Version == PDM_PCIBUSREGR3_VERSION, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pPciBusReg->pfnRegisterR3, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pPciBusReg->pfnIORegionRegisterR3, VERR_INVALID_POINTER);
    AssertPtrReturn(pPciBusReg->pfnInterceptConfigAccesses, VERR_INVALID_POINTER);
    AssertPtrReturn(pPciBusReg->pfnConfigWrite, VERR_INVALID_POINTER);
    AssertPtrReturn(pPciBusReg->pfnConfigRead, VERR_INVALID_POINTER);
    AssertPtrReturn(pPciBusReg->pfnSetIrqR3, VERR_INVALID_POINTER);
    AssertReturn(pPciBusReg->u32EndVersion == PDM_PCIBUSREGR3_VERSION, VERR_INVALID_PARAMETER);
    AssertPtrReturn(ppPciHlp, VERR_INVALID_POINTER);
    AssertReturn(g_Pdm.fCreating, VERR_WRONG_ORDER);

    for (uint32_t iBus = 0; iBus < RT_ELEMENTS(g_Pdm.aPciBuses); iBus++)
        if (!g_Pdm.aPciBuses[iBus].pDevIns)
        {
            g_Pdm.aPciBuses[iBus].pDevIns = pDevIns;
            g_Pdm.aPciBuses[iBus].Reg     = *pPciBusReg;
            *ppPciHlp = &g_tstPdmPciHlpR3;
            if (piBus)
                *piBus = iBus;
            return VINF_SUCCESS;
        }
    return VERR_OUT_OF_RESOURCES;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIRegister} */
static DECLCALLBACK(int) tstPdmHlp_PCIRegister(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t fFlags,
                                               uint8_t uPciDevNo, uint8_t uPciFunNo, const char *pszName)
{
    /*
     * Validate input, mirroring pdmR3DevHlp_PCIRegister.
     */
    AssertReturn(pDevIns->pReg->cMaxPciDevices > 0, VERR_WRONG_ORDER);
    AssertPtrReturn(pPciDev, VERR_INVALID_POINTER);
    AssertReturn(PDMPciDevGetVendorId(pPciDev), VERR_INVALID_POINTER);
    AssertReturn(   uPciDevNo < 32
                 || uPciDevNo == PDMPCIDEVREG_DEV_NO_FIRST_UNUSED
                 || uPciDevNo == PDMPCIDEVREG_DEV_NO_SAME_AS_PREV,
                 VERR_INVALID_PARAMETER);
    AssertReturn(   uPciFunNo < 8
                 || uPciFunNo == PDMPCIDEVREG_FUN_NO_FIRST_UNUSED,
                 VERR_INVALID_PARAMETER);
    AssertReturn(!(fFlags & ~PDMPCIDEVREG_F_VALID_MASK), VERR_INVALID_FLAGS);
    if (!pszName)
        pszName = pDevIns->pReg->szName;
    AssertReturn(pPciDev->u32Magic == PDMPCIDEV_MAGIC, VERR_PDM_NOT_PCI_DEVICE);
    AssertReturn(!pPciDev->Int.s.fRegistered, VERR_PDM_NOT_PCI_DEVICE);

    /* The registration order must be following PDMDEVINSR3::apPciDevs. */
    uint16_t const idxSubDev = pPciDev->idxSubDev;
    AssertReturn(   idxSubDev < RT_ELEMENTS(pDevIns->apPciDevs)
                 && pDevIns->apPciDevs[idxSubDev] == pPciDev,
                 VERR_PDM_NOT_PCI_DEVICE);
    PPDMPCIDEV const pPrevPciDev = idxSubDev == 0 ? NULL : pDevIns->apPciDevs[idxSubDev - 1];
    AssertReturn(!pPrevPciDev || pPrevPciDev->Int.s.fRegistered, VERR_WRONG_ORDER);

    /*
     * Resolve the configuration node for the PCI device. The first one uses the
     * instance node of the device, the others the "PciCfg1..255" sub-nodes.
     */
    PCFGMNODE pCfg = pDevIns->Internal.s.pDev->pCfgInst;
    if (idxSubDev > 0)
    {
        char szName[32];
        RTStrPrintf(szName, sizeof(szName), "PciCfg%u", idxSubDev);
        pCfg = tstCfgFindChild(pCfg, szName);
    }

    /*
     * PDM resolves PDMPCIDEVREG_DEV_NO_SAME_AS_PREV, the bus does the rest.
     */
    uint8_t const uPciDevNoRaw = uPciDevNo;
    uint64_t      u64Bus       = 0;
    if (uPciDevNo == PDMPCIDEVREG_DEV_NO_SAME_AS_PREV)
    {
        AssertReturn(pPrevPciDev, VERR_WRONG_ORDER);
        uPciDevNo = (uint8_t)(pPrevPciDev->uDevFn >> 3);
        u64Bus    = pPrevPciDev->Int.s.idxPdmBus;
    }
    tstCfgQueryInteger(pCfg, "PCIBusNo", &u64Bus);
    AssertReturn(u64Bus < RT_ELEMENTS(g_Pdm.aPciBuses), VERR_PDM_NO_PCI_BUS);
    PTSTPDMPCIBUS pBus = &g_Pdm.aPciBuses[u64Bus];
    AssertReturn(pBus->pDevIns, VERR_PDM_NO_PCI_BUS);

    uint64_t u64Value = 0;
    if (tstCfgQueryInteger(pCfg, "PCIDeviceNo", &u64Value))
    {
        AssertReturn(u64Value <= 31, VERR_PDM_BAD_PCI_CONFIG);
        uPciDevNo = (uint8_t)u64Value;
    }
    if (tstCfgQueryInteger(pCfg, "PCIFunctionNo", &u64Value))
    {
        AssertReturn(u64Value <= 7, VERR_PDM_BAD_PCI_CONFIG);
        uPciFunNo = (uint8_t)u64Value;
    }

    /*
     * Initialize the internal data.  PDM wipes it and sets its own members,
     * the PCI bus does the rest in the registration call.
     */
    RT_ZERO(pPciDev->Int);
    pPciDev->Int.s.idxDevCfg          = (uint8_t)idxSubDev;
    pPciDev->Int.s.fReassignableDevNo = uPciDevNoRaw >= VBOX_PCI_MAX_DEVICES;
    pPciDev->Int.s.fReassignableFunNo = uPciFunNo >= VBOX_PCI_MAX_FUNCTIONS;
    pPciDev->Int.s.pDevInsR3          = pDevIns;
    pPciDev->Int.s.idxPdmBus          = (uint8_t)u64Bus;
    pPciDev->Int.s.fRegistered        = true;
    pPciDev->pszNameR3                = pszName;

    int rc = pBus->Reg.pfnRegisterR3(pBus->pDevIns, pPciDev, fFlags, uPciDevNo, uPciFunNo, pszName);
    if (RT_FAILURE(rc))
        pPciDev->Int.s.fRegistered = false;
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIRegisterMsi} */
static DECLCALLBACK(int) tstPdmHlp_PCIRegisterMsi(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, PPDMMSIREG pMsiReg)
{
    if (!pPciDev) /* NULL is an alias for the default PCI device. */
        pPciDev = pDevIns->apPciDevs[0];
    AssertReturn(pPciDev, VERR_PDM_NOT_PCI_DEVICE);
    AssertReturn(pMsiReg->cMsixVectors <= pDevIns->pReg->cMaxMsixVectors, VERR_INVALID_FLAGS);

    PTSTPDMPCIBUS pBus = tstPdmPciBusFromDev(pPciDev);
    AssertReturn(pBus, VERR_WRONG_ORDER);
    if (!pBus->Reg.pfnRegisterMsiR3)
        return VERR_NOT_IMPLEMENTED;
    return pBus->Reg.pfnRegisterMsiR3(pBus->pDevIns, pPciDev, pMsiReg);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIIORegionRegister} */
static DECLCALLBACK(int) tstPdmHlp_PCIIORegionRegister(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t iRegion,
                                                       RTGCPHYS cbRegion, PCIADDRESSSPACE enmType, uint32_t fFlags,
                                                       uint64_t hHandle, PFNPCIIOREGIONMAP pfnMapUnmap)
{
    if (!pPciDev) /* NULL is an alias for the default PCI device. */
        pPciDev = pDevIns->apPciDevs[0];
    AssertReturn(pPciDev, VERR_PDM_NOT_PCI_DEVICE);

    /*
     * Validate input, mirroring pdmR3DevHlp_PCIIORegionRegister.
     */
    AssertReturn(g_Pdm.fCreating, VERR_WRONG_ORDER);
    AssertReturn(iRegion < VBOX_PCI_NUM_REGIONS, VERR_INVALID_PARAMETER);
    switch ((int)enmType)
    {
        case PCI_ADDRESS_SPACE_IO:
            AssertReturn(cbRegion <= _32K, VERR_INVALID_PARAMETER);
            break;
        case PCI_ADDRESS_SPACE_MEM:
        case PCI_ADDRESS_SPACE_MEM_PREFETCH:
            AssertReturn(cbRegion <= MM_MMIO_32_MAX, VERR_OUT_OF_RANGE);
            break;
        case PCI_ADDRESS_SPACE_BAR64 | PCI_ADDRESS_SPACE_MEM:
        case PCI_ADDRESS_SPACE_BAR64 | PCI_ADDRESS_SPACE_MEM_PREFETCH:
            AssertReturn(cbRegion <= MM_MMIO_64_MAX, VERR_OUT_OF_RANGE);
            break;
        default:
            AssertFailedReturn(VERR_INVALID_PARAMETER);
    }
    AssertReturn(   pfnMapUnmap
                 || (   hHandle != UINT64_MAX
                     && (fFlags & PDMPCIDEV_IORGN_F_HANDLE_MASK) != PDMPCIDEV_IORGN_F_NO_HANDLE),
                 VERR_INVALID_PARAMETER);
    AssertReturn(!(fFlags & ~PDMPCIDEV_IORGN_F_VALID_MASK), VERR_INVALID_FLAGS);
    AssertReturn(fFlags & PDMPCIDEV_IORGN_F_NEW_STYLE, VERR_INVALID_FLAGS);
    AssertReturn(cbRegion > 0, VERR_INVALID_PARAMETER);

    /* MMIO regions are page aligned and all regions are a power of two in size. */
    if (   ((unsigned)enmType & ~(unsigned)(PCI_ADDRESS_SPACE_BAR64 | PCI_ADDRESS_SPACE_MEM_PREFETCH)) == (unsigned)PCI_ADDRESS_SPACE_MEM
        && cbRegion != RT_ALIGN_64(cbRegion, GUEST_PAGE_SIZE))
        cbRegion = RT_ALIGN_64(cbRegion, GUEST_PAGE_SIZE);
    uint64_t const cbRegionPow2 = RT_BIT_64(ASMBitLastSetU64(cbRegion) - 1);
    if (cbRegion > cbRegionPow2)
        cbRegion = cbRegionPow2 * 2;

    PTSTPDMPCIBUS pBus = tstPdmPciBusFromDev(pPciDev);
    AssertReturn(pBus, VERR_WRONG_ORDER);
    return pBus->Reg.pfnIORegionRegisterR3(pBus->pDevIns, pPciDev, iRegion, cbRegion, enmType, fFlags, hHandle, pfnMapUnmap);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIInterceptConfigAccesses} */
static DECLCALLBACK(int) tstPdmHlp_PCIInterceptConfigAccesses(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev,
                                                              PFNPCICONFIGREAD pfnRead, PFNPCICONFIGWRITE pfnWrite)
{
    if (!pPciDev) /* NULL is an alias for the default PCI device. */
        pPciDev = pDevIns->apPciDevs[0];
    AssertReturn(pPciDev, VERR_PDM_NOT_PCI_DEVICE);
    AssertReturn(!g_Pdm.fRunning, VERR_WRONG_ORDER); /* PDM asserts this with a release assertion. */

    PTSTPDMPCIBUS pBus = tstPdmPciBusFromDev(pPciDev);
    AssertReturn(pBus, VERR_INTERNAL_ERROR_2);
    pBus->Reg.pfnInterceptConfigAccesses(pBus->pDevIns, pPciDev, pfnRead, pfnWrite);
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCISetIrq} */
static DECLCALLBACK(void) tstPdmHlp_PCISetIrq(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, int iIrq, int iLevel)
{
    if (!pPciDev) /* NULL is an alias for the default PCI device. */
        pPciDev = pDevIns->apPciDevs[0];
    AssertReturnVoid(pPciDev);

    PTSTPDMPCIBUS pBus = tstPdmPciBusFromDev(pPciDev);
    AssertReturnVoid(pBus);
    pBus->Reg.pfnSetIrqR3(pBus->pDevIns, pPciDev, iIrq, iLevel, 0 /*uTagSrc*/);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIConfigWrite} */
static DECLCALLBACK(VBOXSTRICTRC) tstPdmHlp_PCIConfigWrite(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t uAddress,
                                                          unsigned cb, uint32_t u32Value)
{
    if (!pPciDev) /* NULL is an alias for the default PCI device. */
        pPciDev = pDevIns->apPciDevs[0];
    AssertReturn(pPciDev, VERR_PDM_NOT_PCI_DEVICE);

    /* This is the default handling of the bus, the device interception is not involved. */
    PTSTPDMPCIBUS pBus = tstPdmPciBusFromDev(pPciDev);
    AssertReturn(pBus, VERR_INTERNAL_ERROR_2);
    return pBus->Reg.pfnConfigWrite(pBus->pDevIns, pPciDev, uAddress, cb, u32Value);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIConfigRead} */
static DECLCALLBACK(VBOXSTRICTRC) tstPdmHlp_PCIConfigRead(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t uAddress,
                                                         unsigned cb, uint32_t *pu32Value)
{
    if (!pPciDev) /* NULL is an alias for the default PCI device. */
        pPciDev = pDevIns->apPciDevs[0];
    AssertReturn(pPciDev, VERR_PDM_NOT_PCI_DEVICE);

    /* This is the default handling of the bus, the device interception is not involved. */
    PTSTPDMPCIBUS pBus = tstPdmPciBusFromDev(pPciDev);
    AssertReturn(pBus, VERR_INTERNAL_ERROR_2);
    return pBus->Reg.pfnConfigRead(pBus->pDevIns, pPciDev, uAddress, cb, pu32Value);
}


/** @interface_method_impl{PDMPCIHLPR3,pfnIsaSetIrq} */
static DECLCALLBACK(void) tstPdmPciHlp_IsaSetIrq(PPDMDEVINS pDevIns, int iIrq, int iLevel, uint32_t uTagSrc)
{
    RT_NOREF(pDevIns, iIrq, iLevel, uTagSrc);
}


/** @interface_method_impl{PDMPCIHLPR3,pfnIoApicSetIrq} */
static DECLCALLBACK(void) tstPdmPciHlp_IoApicSetIrq(PPDMDEVINS pDevIns, PCIBDF uBusDevFn, int iIrq, int iLevel, uint32_t uTagSrc)
{
    RT_NOREF(pDevIns, uBusDevFn, iIrq, iLevel, uTagSrc);
}


/** @interface_method_impl{PDMPCIHLPR3,pfnIoApicSendMsi} */
static DECLCALLBACK(void) tstPdmPciHlp_IoApicSendMsi(PPDMDEVINS pDevIns, PCIBDF uBusDevFn, PCMSIMSG pMsi, uint32_t uTagSrc)
{
    RT_NOREF(pDevIns, uTagSrc);
    RTCritSectEnter(&g_Pdm.CritSectIrq);
    if (g_Pdm.cMsis < RT_ELEMENTS(g_Pdm.aMsis))
    {
        g_Pdm.aMsis[g_Pdm.cMsis].uBusDevFn = uBusDevFn;
        g_Pdm.aMsis[g_Pdm.cMsis].uAddr     = pMsi->Addr.u64;
        g_Pdm.aMsis[g_Pdm.cMsis].uData     = pMsi->Data.u32;
    }
    g_Pdm.LastMsi.uBusDevFn = uBusDevFn;
    g_Pdm.LastMsi.uAddr     = pMsi->Addr.u64;
    g_Pdm.LastMsi.uData     = pMsi->Data.u32;
    g_Pdm.cMsis++;
    RTCritSectLeave(&g_Pdm.CritSectIrq);
    RTSemEventSignal(g_Pdm.hEvtMsi);
}


/** @interface_method_impl{PDMPCIHLPR3,pfnLock} */
static DECLCALLBACK(int) tstPdmPciHlp_Lock(PPDMDEVINS pDevIns, int rc)
{
    RT_NOREF(pDevIns, rc);
    return RTCritSectEnter(&g_Pdm.CritSectPci);
}


/** @interface_method_impl{PDMPCIHLPR3,pfnUnlock} */
static DECLCALLBACK(void) tstPdmPciHlp_Unlock(PPDMDEVINS pDevIns)
{
    RT_NOREF(pDevIns);
    RTCritSectLeave(&g_Pdm.CritSectPci);
}


/** @interface_method_impl{PDMPCIHLPR3,pfnGetBusByNo} */
static DECLCALLBACK(PPDMDEVINS) tstPdmPciHlp_GetBusByNo(PPDMDEVINS pDevIns, uint32_t idxPdmBus)
{
    RT_NOREF(pDevIns);
    AssertReturn(idxPdmBus < RT_ELEMENTS(g_Pdm.aPciBuses), NULL);
    return g_Pdm.aPciBuses[idxPdmBus].pDevIns;
}


/*
 *
 * Device helpers: Threads.
 * Device helpers: Threads.
 * Device helpers: Threads.
 *
 */

/**
 * The PDM thread procedure.
 *
 * @returns VBox status code.
 * @param   hThreadSelf         The thread handle.
 * @param   pvUser              The PDM thread.
 */
static DECLCALLBACK(int) tstPdmThreadMain(RTTHREAD hThreadSelf, void *pvUser)
{
    PTSTPDMTHREAD pThis   = (PTSTPDMTHREAD)pvUser;
    PPDMTHREAD    pThread = &pThis->Core;
    RT_NOREF(hThreadSelf);

    /* The initialization call. */
    int rc = pThread->u.Dev.pfnThread(pThread->u.Dev.pDevIns, pThread);
    pThread->enmState = PDMTHREADSTATE_SUSPENDED;
    RTSemEventSignal(pThis->hEvtState);

    for (;;)
    {
        RTSemEventWait(pThis->hEvtRun, RT_INDEFINITE_WAIT);
        if (pThread->enmState == PDMTHREADSTATE_TERMINATING)
            break;
        if (pThread->enmState == PDMTHREADSTATE_RESUMING)
        {
            pThread->enmState = PDMTHREADSTATE_RUNNING;
            RTSemEventSignal(pThis->hEvtState);

            rc = pThread->u.Dev.pfnThread(pThread->u.Dev.pDevIns, pThread);
            if (pThread->enmState == PDMTHREADSTATE_TERMINATING)
                break;

            pThread->enmState = PDMTHREADSTATE_SUSPENDED;
            RTSemEventSignal(pThis->hEvtState);
        }
    }

    pThread->enmState = PDMTHREADSTATE_TERMINATED;
    return rc;
}


/**
 * Resumes a suspended PDM thread.
 *
 * @param   pThis               The PDM thread.
 */
static void tstPdmThreadResume(PTSTPDMTHREAD pThis)
{
    if (pThis->Core.enmState == PDMTHREADSTATE_SUSPENDED)
    {
        pThis->Core.enmState = PDMTHREADSTATE_RESUMING;
        RTSemEventSignal(pThis->hEvtRun);
        int rc = RTSemEventWait(pThis->hEvtState, TSTPDM_THREAD_TIMEOUT_MS);
        AssertReleaseRC(rc);
    }
}


/**
 * Suspends a running PDM thread.
 *
 * @param   pThis               The PDM thread.
 */
static void tstPdmThreadSuspend(PTSTPDMTHREAD pThis)
{
    if (pThis->Core.enmState == PDMTHREADSTATE_RUNNING)
    {
        pThis->Core.enmState = PDMTHREADSTATE_SUSPENDING;
        pThis->Core.u.Dev.pfnWakeUp(pThis->Core.u.Dev.pDevIns, &pThis->Core);
        int rc = RTSemEventWait(pThis->hEvtState, TSTPDM_THREAD_TIMEOUT_MS);
        AssertReleaseRC(rc);
    }
}


/**
 * Terminates and frees a PDM thread.
 *
 * @param   pThis               The PDM thread.
 */
static void tstPdmThreadDestroy(PTSTPDMTHREAD pThis)
{
    if (pThis->Core.enmState == PDMTHREADSTATE_RUNNING)
    {
        pThis->Core.enmState = PDMTHREADSTATE_TERMINATING;
        pThis->Core.u.Dev.pfnWakeUp(pThis->Core.u.Dev.pDevIns, &pThis->Core);
    }
    else
    {
        pThis->Core.enmState = PDMTHREADSTATE_TERMINATING;
        RTSemEventSignal(pThis->hEvtRun);
    }
    int rc = RTThreadWait(pThis->Core.Thread, TSTPDM_THREAD_TIMEOUT_MS, NULL);
    AssertReleaseRC(rc);

    RTSemEventDestroy(pThis->hEvtRun);
    RTSemEventDestroy(pThis->hEvtState);
    RTMemFree(pThis);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnThreadCreate} */
static DECLCALLBACK(int) tstPdmHlp_ThreadCreate(PPDMDEVINS pDevIns, PPPDMTHREAD ppThread, void *pvUser,
                                                PFNPDMTHREADDEV pfnThread, PFNPDMTHREADWAKEUPDEV pfnWakeup, size_t cbStack,
                                                RTTHREADTYPE enmType, const char *pszName)
{
    *ppThread = NULL;
    AssertReturn(g_Pdm.cThreads < RT_ELEMENTS(g_Pdm.apThreads), VERR_OUT_OF_RESOURCES);

    PTSTPDMTHREAD pThis = (PTSTPDMTHREAD)RTMemAllocZ(sizeof(*pThis));
    if (!pThis)
        return VERR_NO_MEMORY;
    pThis->Core.u32Version      = PDMTHREAD_VERSION;
    pThis->Core.enmState        = PDMTHREADSTATE_INITIALIZING;
    pThis->Core.Thread          = NIL_RTTHREAD;
    pThis->Core.pvUser          = pvUser;
    pThis->Core.u.Dev.pDevIns   = pDevIns;
    pThis->Core.u.Dev.pfnThread = pfnThread;
    pThis->Core.u.Dev.pfnWakeUp = pfnWakeup;

    int rc = RTSemEventCreate(&pThis->hEvtRun);
    if (RT_SUCCESS(rc))
    {
        rc = RTSemEventCreate(&pThis->hEvtState);
        if (RT_SUCCESS(rc))
        {
            /* The device has to see the handle before the thread procedure is called. */
            *ppThread = &pThis->Core;
            rc = RTThreadCreate(&pThis->Core.Thread, tstPdmThreadMain, pThis, cbStack, enmType, RTTHREADFLAGS_WAITABLE, pszName);
            if (RT_SUCCESS(rc))
            {
                rc = RTSemEventWait(pThis->hEvtState, TSTPDM_THREAD_TIMEOUT_MS);
                AssertReleaseRC(rc);
                g_Pdm.apThreads[g_Pdm.cThreads++] = pThis;

                /* Threads created while the VM runs start running right away, like with PDM. */
                if (g_Pdm.fRunning)
                    tstPdmThreadResume(pThis);
                return VINF_SUCCESS;
            }
            *ppThread = NULL;
            RTSemEventDestroy(pThis->hEvtState);
        }
        RTSemEventDestroy(pThis->hEvtRun);
    }
    RTMemFree(pThis);
    return rc;
}


/*
 *
 * Device helpers: Miscellaneous.
 * Device helpers: Miscellaneous.
 * Device helpers: Miscellaneous.
 *
 */

/** @interface_method_impl{PDMDEVHLPR3,pfnVMSetErrorV} */
static DECLCALLBACK(int) tstPdmHlp_VMSetErrorV(PPDMDEVINS pDevIns, int rc, RT_SRC_POS_DECL, const char *pszFormat, va_list va)
{
    RT_NOREF(pDevIns);
    RT_SRC_POS_NOREF();
    RTStrPrintfV(g_Pdm.szLastError, sizeof(g_Pdm.szLastError), pszFormat, va);
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMSetRuntimeErrorV} */
static DECLCALLBACK(int) tstPdmHlp_VMSetRuntimeErrorV(PPDMDEVINS pDevIns, uint32_t fFlags, const char *pszErrorId,
                                                      const char *pszFormat, va_list va)
{
    RT_NOREF(pDevIns, fFlags);
    RTStrCopy(g_Pdm.szLastRuntimeErrorId, sizeof(g_Pdm.szLastRuntimeErrorId), pszErrorId);
    RTStrPrintfV(g_Pdm.szLastError, sizeof(g_Pdm.szLastError), pszFormat, va);
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMSetLoadErrorV} */
static DECLCALLBACK(int) tstPdmHlp_SSMSetLoadErrorV(PSSMHANDLE pSSM, int rc, RT_SRC_POS_DECL, const char *pszFormat, va_list va)
{
    RT_NOREF(pSSM);
    RT_SRC_POS_NOREF();
    /* The first message is the one which gets shown. */
    if (!g_Pdm.szLastError[0])
        RTStrPrintfV(g_Pdm.szLastError, sizeof(g_Pdm.szLastError), pszFormat, va);
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMSetLoadError} */
static DECLCALLBACK(int) tstPdmHlp_SSMSetLoadError(PSSMHANDLE pSSM, int rc, RT_SRC_POS_DECL, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    rc = tstPdmHlp_SSMSetLoadErrorV(pSSM, rc, RT_SRC_POS_ARGS, pszFormat, va);
    va_end(va);
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMHandleGetStatus} */
static DECLCALLBACK(int) tstPdmHlp_SSMHandleGetStatus(PSSMHANDLE pSSM)
{
    AssertPtrReturn(pSSM, VERR_INVALID_POINTER);
    return pSSM->rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMHandleGetAfter} */
static DECLCALLBACK(SSMAFTER) tstPdmHlp_SSMHandleGetAfter(PSSMHANDLE pSSM)
{
    RT_NOREF(pSSM);
    return SSMAFTER_RESUME;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMSetCfgError} */
static DECLCALLBACK(int) tstPdmHlp_SSMSetCfgError(PSSMHANDLE pSSM, RT_SRC_POS_DECL, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = tstPdmHlp_SSMSetLoadErrorV(pSSM, VERR_SSM_LOAD_CONFIG_MISMATCH, RT_SRC_POS_ARGS, pszFormat, va);
    va_end(va);
    return rc;
}


/**
 * Appends to a saved state.
 *
 * @returns VBox status code.
 * @param   pSSM                The saved state.
 * @param   pv                  What to append.
 * @param   cb                  Number of bytes.
 */
static int tstPdmSsmWrite(PSSMHANDLE pSSM, const void *pv, size_t cb)
{
    AssertPtrReturn(pSSM, VERR_INVALID_POINTER);
    AssertReturn(!pSSM->fLoading, VERR_SSM_INVALID_STATE);
    if (RT_FAILURE(pSSM->rc))
        return pSSM->rc;
    if (cb > pSSM->cbAlloc - pSSM->cb)
    {
        size_t const cbNew = RT_ALIGN_Z(pSSM->cb + cb + _1M, _1M);
        void        *pvNew = RTMemRealloc(pSSM->pb, cbNew);
        if (!pvNew)
            return pSSM->rc = VERR_NO_MEMORY;
        pSSM->pb      = (uint8_t *)pvNew;
        pSSM->cbAlloc = cbNew;
    }
    memcpy(&pSSM->pb[pSSM->cb], pv, cb);
    pSSM->cb += cb;
    return VINF_SUCCESS;
}


/**
 * Reads from a saved state, up to where the current unit ends.
 *
 * @returns VBox status code.
 * @param   pSSM                The saved state.
 * @param   pv                  Where to put what is read.
 * @param   cb                  Number of bytes.
 */
static int tstPdmSsmRead(PSSMHANDLE pSSM, void *pv, size_t cb)
{
    AssertPtrReturn(pSSM, VERR_INVALID_POINTER);
    AssertReturn(pSSM->fLoading, VERR_SSM_INVALID_STATE);
    if (RT_FAILURE(pSSM->rc))
        return pSSM->rc;
    if (cb > pSSM->offUnitEnd - pSSM->off)
        return pSSM->rc = VERR_SSM_LOADED_TOO_MUCH;
    memcpy(pv, &pSSM->pb[pSSM->off], cb);
    pSSM->off += cb;
    return VINF_SUCCESS;
}


/**
 * Puts an item of a unit into a saved state.
 *
 * @returns VBox status code.
 * @param   pSSM                The saved state.
 * @param   bKind               TSTPDM_SSM_ITEM_XXX.
 * @param   pv                  The item.
 * @param   cb                  Its size.
 */
static int tstPdmSsmPutItem(PSSMHANDLE pSSM, uint8_t bKind, const void *pv, size_t cb)
{
    AssertPtrReturn(pSSM, VERR_INVALID_POINTER);
    AssertReturn(pSSM->fExec, VERR_SSM_INVALID_STATE);
    tstPdmSsmWrite(pSSM, &bKind, sizeof(bKind));
    if (bKind == TSTPDM_SSM_ITEM_MEM)
    {
        uint32_t const cb32 = (uint32_t)cb;
        tstPdmSsmWrite(pSSM, &cb32, sizeof(cb32));
    }
    return tstPdmSsmWrite(pSSM, pv, cb);
}


/**
 * Gets an item of a unit from a saved state.
 *
 * @returns VBox status code.
 * @param   pSSM                The saved state.
 * @param   bKind               TSTPDM_SSM_ITEM_XXX, has to be what was put.
 * @param   pv                  Where to put the item.
 * @param   cb                  Its size, has to be what was put.
 */
static int tstPdmSsmGetItem(PSSMHANDLE pSSM, uint8_t bKind, void *pv, size_t cb)
{
    AssertPtrReturn(pSSM, VERR_INVALID_POINTER);
    AssertReturn(pSSM->fExec, VERR_SSM_INVALID_STATE);
    uint8_t bKindSaved = 0;
    int rc = tstPdmSsmRead(pSSM, &bKindSaved, sizeof(bKindSaved));
    if (RT_SUCCESS(rc) && bKindSaved != bKind)
        rc = pSSM->rc = VERR_SSM_DATA_UNIT_FORMAT_CHANGED;
    if (RT_SUCCESS(rc) && bKind == TSTPDM_SSM_ITEM_MEM)
    {
        uint32_t cb32 = 0;
        rc = tstPdmSsmRead(pSSM, &cb32, sizeof(cb32));
        if (RT_SUCCESS(rc) && cb32 != cb)
            rc = pSSM->rc = VERR_SSM_DATA_UNIT_FORMAT_CHANGED;
    }
    if (RT_SUCCESS(rc))
        rc = tstPdmSsmRead(pSSM, pv, cb);
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMPutU8} */
static DECLCALLBACK(int) tstPdmHlp_SSMPutU8(PSSMHANDLE pSSM, uint8_t u8)
{
    return tstPdmSsmPutItem(pSSM, TSTPDM_SSM_ITEM_U8, &u8, sizeof(u8));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMPutU32} */
static DECLCALLBACK(int) tstPdmHlp_SSMPutU32(PSSMHANDLE pSSM, uint32_t u32)
{
    return tstPdmSsmPutItem(pSSM, TSTPDM_SSM_ITEM_U32, &u32, sizeof(u32));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMPutU64} */
static DECLCALLBACK(int) tstPdmHlp_SSMPutU64(PSSMHANDLE pSSM, uint64_t u64)
{
    return tstPdmSsmPutItem(pSSM, TSTPDM_SSM_ITEM_U64, &u64, sizeof(u64));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMPutS32} */
static DECLCALLBACK(int) tstPdmHlp_SSMPutS32(PSSMHANDLE pSSM, int32_t i32)
{
    return tstPdmSsmPutItem(pSSM, TSTPDM_SSM_ITEM_S32, &i32, sizeof(i32));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMPutBool} */
static DECLCALLBACK(int) tstPdmHlp_SSMPutBool(PSSMHANDLE pSSM, bool fBool)
{
    uint8_t const u8 = fBool;
    return tstPdmSsmPutItem(pSSM, TSTPDM_SSM_ITEM_BOOL, &u8, sizeof(u8));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMPutMem} */
static DECLCALLBACK(int) tstPdmHlp_SSMPutMem(PSSMHANDLE pSSM, const void *pv, size_t cb)
{
    return tstPdmSsmPutItem(pSSM, TSTPDM_SSM_ITEM_MEM, pv, cb);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMGetU8} */
static DECLCALLBACK(int) tstPdmHlp_SSMGetU8(PSSMHANDLE pSSM, uint8_t *pu8)
{
    return tstPdmSsmGetItem(pSSM, TSTPDM_SSM_ITEM_U8, pu8, sizeof(*pu8));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMGetU32} */
static DECLCALLBACK(int) tstPdmHlp_SSMGetU32(PSSMHANDLE pSSM, uint32_t *pu32)
{
    return tstPdmSsmGetItem(pSSM, TSTPDM_SSM_ITEM_U32, pu32, sizeof(*pu32));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMGetU32V} */
static DECLCALLBACK(int) tstPdmHlp_SSMGetU32V(PSSMHANDLE pSSM, uint32_t volatile *pu32)
{
    return tstPdmSsmGetItem(pSSM, TSTPDM_SSM_ITEM_U32, (void *)pu32, sizeof(*pu32));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMGetU64} */
static DECLCALLBACK(int) tstPdmHlp_SSMGetU64(PSSMHANDLE pSSM, uint64_t *pu64)
{
    return tstPdmSsmGetItem(pSSM, TSTPDM_SSM_ITEM_U64, pu64, sizeof(*pu64));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMGetS32} */
static DECLCALLBACK(int) tstPdmHlp_SSMGetS32(PSSMHANDLE pSSM, int32_t *pi32)
{
    return tstPdmSsmGetItem(pSSM, TSTPDM_SSM_ITEM_S32, pi32, sizeof(*pi32));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMGetBool} */
static DECLCALLBACK(int) tstPdmHlp_SSMGetBool(PSSMHANDLE pSSM, bool *pfBool)
{
    uint8_t u8 = 0;
    int rc = tstPdmSsmGetItem(pSSM, TSTPDM_SSM_ITEM_BOOL, &u8, sizeof(u8));
    if (RT_SUCCESS(rc))
        *pfBool = u8 != 0;
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMGetMem} */
static DECLCALLBACK(int) tstPdmHlp_SSMGetMem(PSSMHANDLE pSSM, void *pv, size_t cb)
{
    return tstPdmSsmGetItem(pSSM, TSTPDM_SSM_ITEM_MEM, pv, cb);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMState} */
static DECLCALLBACK(VMSTATE) tstPdmHlp_VMState(PPDMDEVINS pDevIns)
{
    RT_NOREF(pDevIns);
    return g_Pdm.enmVmState;
}


/**
 * @callback_method_impl{FNSSMDEVSAVEEXEC,
 *      Plays the part of PGM: each MMIO2 range is saved with what identifies it and
 *      page by page\, which reads all of it.}
 */
static DECLCALLBACK(int) tstPdmPgmSaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    RT_NOREF(pDevIns);
    for (uint32_t i = 0; i < g_Pdm.cMmio2; i++)
    {
        char szName[32];
        RT_ZERO(szName);
        RTStrCopy(szName, sizeof(szName), g_Pdm.aMmio2[i].pDevIns->pReg->szName);
        tstPdmHlp_SSMPutU32(pSSM, i);
        tstPdmHlp_SSMPutMem(pSSM, szName, sizeof(szName));
        tstPdmHlp_SSMPutU32(pSSM, g_Pdm.aMmio2[i].pDevIns->iInstance);
        tstPdmHlp_SSMPutU32(pSSM, g_Pdm.aMmio2[i].iSubDev);
        tstPdmHlp_SSMPutU32(pSSM, g_Pdm.aMmio2[i].iRegion);
        tstPdmHlp_SSMPutU64(pSSM, g_Pdm.aMmio2[i].cbRegion);
        for (RTGCPHYS off = 0; off < g_Pdm.aMmio2[i].cbRegion; off += GUEST_PAGE_SIZE)
        {
            uint8_t const *pbPage = &g_Pdm.aMmio2[i].pbBacking[off];
            bool const     fZero  = ASMMemIsZero(pbPage, GUEST_PAGE_SIZE);
            tstPdmHlp_SSMPutBool(pSSM, fZero);
            if (!fZero)
                tstPdmHlp_SSMPutMem(pSSM, pbPage, GUEST_PAGE_SIZE);
        }
    }
    return tstPdmHlp_SSMPutU32(pSSM, UINT32_MAX);
}


/**
 * @callback_method_impl{FNSSMDEVLOADEXEC,
 *      Plays the part of PGM: each saved MMIO2 range has to be there\, and each of
 *      its pages gets written to\, the ones which were zero included.}
 */
static DECLCALLBACK(int) tstPdmPgmLoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    RT_NOREF(pDevIns, uVersion, uPass);
    for (;;)
    {
        uint32_t idSaved = 0;
        int rc = tstPdmHlp_SSMGetU32(pSSM, &idSaved);
        if (RT_FAILURE(rc))
            return rc;
        if (idSaved == UINT32_MAX)
            return VINF_SUCCESS;

        char     szName[32];
        uint32_t uInstance = 0;
        uint32_t iSubDev   = 0;
        uint32_t iRegion   = 0;
        uint64_t cbRegion  = 0;
        RT_ZERO(szName);
        tstPdmHlp_SSMGetMem(pSSM, szName, sizeof(szName));
        tstPdmHlp_SSMGetU32(pSSM, &uInstance);
        tstPdmHlp_SSMGetU32(pSSM, &iSubDev);
        tstPdmHlp_SSMGetU32(pSSM, &iRegion);
        rc = tstPdmHlp_SSMGetU64(pSSM, &cbRegion);
        if (RT_FAILURE(rc))
            return rc;
        szName[sizeof(szName) - 1] = '\0';

        uint32_t idx;
        for (idx = 0; idx < g_Pdm.cMmio2; idx++)
            if (   g_Pdm.aMmio2[idx].iSubDev == iSubDev
                && g_Pdm.aMmio2[idx].iRegion == iRegion
                && g_Pdm.aMmio2[idx].pDevIns->iInstance == uInstance
                && !strcmp(g_Pdm.aMmio2[idx].pDevIns->pReg->szName, szName))
                break;
        if (idx >= g_Pdm.cMmio2)
            return tstPdmHlp_SSMSetCfgError(pSSM, RT_SRC_POS, "Failed to locate a MMIO2 range owned by %s/%u, region %u/%u",
                                            szName, uInstance, iSubDev, iRegion);
        if (cbRegion != g_Pdm.aMmio2[idx].cbRegion)
            return tstPdmHlp_SSMSetCfgError(pSSM, RT_SRC_POS, "MMIO2 region %u/%u of %s/%u size mismatch: saved=%RX64 config=%RGp",
                                            iSubDev, iRegion, szName, uInstance, cbRegion, g_Pdm.aMmio2[idx].cbRegion);

        for (RTGCPHYS off = 0; off < g_Pdm.aMmio2[idx].cbRegion; off += GUEST_PAGE_SIZE)
        {
            uint8_t *pbPage = &g_Pdm.aMmio2[idx].pbBacking[off];
            bool     fZero  = false;
            rc = tstPdmHlp_SSMGetBool(pSSM, &fZero);
            if (RT_FAILURE(rc))
                return rc;
            if (fZero)
                RT_BZERO(pbPage, GUEST_PAGE_SIZE);
            else
            {
                rc = tstPdmHlp_SSMGetMem(pSSM, pbPage, GUEST_PAGE_SIZE);
                if (RT_FAILURE(rc))
                    return rc;
            }
        }
    }
}


/**
 * Registers a saved state unit.
 *
 * @returns VBox status code.
 * @param   pUnit               The unit.
 * @param   pszBefore           The name of the unit to put it in front of, NULL for putting
 *                              it last.  It goes last too if there is no such unit.
 */
static int tstPdmSsmRegisterUnit(TSTPDMSSMUNIT const *pUnit, const char *pszBefore)
{
    AssertReturn(g_Pdm.cSsmUnits < RT_ELEMENTS(g_Pdm.aSsmUnits), VERR_OUT_OF_RESOURCES);
    uint32_t idx = g_Pdm.cSsmUnits;
    for (uint32_t i = 0; i < g_Pdm.cSsmUnits; i++)
    {
        AssertReturn(   g_Pdm.aSsmUnits[i].uInstance != pUnit->uInstance
                     || strcmp(g_Pdm.aSsmUnits[i].szName, pUnit->szName),
                     VERR_SSM_UNIT_EXISTS);
        if (   pszBefore
            && idx == g_Pdm.cSsmUnits
            && !strcmp(g_Pdm.aSsmUnits[i].szName, pszBefore))
            idx = i;
    }
    memmove(&g_Pdm.aSsmUnits[idx + 1], &g_Pdm.aSsmUnits[idx], (g_Pdm.cSsmUnits - idx) * sizeof(g_Pdm.aSsmUnits[0]));
    g_Pdm.aSsmUnits[idx] = *pUnit;
    g_Pdm.cSsmUnits++;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCritSectGetNop} */
static DECLCALLBACK(PPDMCRITSECT) tstPdmHlp_CritSectGetNop(PPDMDEVINS pDevIns)
{
    RT_NOREF(pDevIns);
    return &g_Pdm.NopCritSect;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSetDeviceCritSect} */
static DECLCALLBACK(int) tstPdmHlp_SetDeviceCritSect(PPDMDEVINS pDevIns, PPDMCRITSECT pCritSect)
{
    AssertPtrReturn(pCritSect, VERR_INVALID_POINTER);
    pDevIns->pCritSectRoR3 = pCritSect;
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMRegister} */
static DECLCALLBACK(int) tstPdmHlp_SSMRegister(PPDMDEVINS pDevIns, uint32_t uVersion, size_t cbGuess, const char *pszBefore,
                                               PFNSSMDEVLIVEPREP pfnLivePrep, PFNSSMDEVLIVEEXEC pfnLiveExec,
                                               PFNSSMDEVLIVEVOTE pfnLiveVote, PFNSSMDEVSAVEPREP pfnSavePrep,
                                               PFNSSMDEVSAVEEXEC pfnSaveExec, PFNSSMDEVSAVEDONE pfnSaveDone,
                                               PFNSSMDEVLOADPREP pfnLoadPrep, PFNSSMDEVLOADEXEC pfnLoadExec,
                                               PFNSSMDEVLOADDONE pfnLoadDone)
{
    /* The passes of a live save while the VM runs are not played through. */
    RT_NOREF(cbGuess, pfnLiveExec, pfnLiveVote);
    TSTPDMSSMUNIT Unit;
    RT_ZERO(Unit);
    RTStrCopy(Unit.szName, sizeof(Unit.szName), pDevIns->pReg->szName);
    Unit.uInstance   = pDevIns->iInstance;
    Unit.uVersion    = uVersion;
    Unit.pDevIns     = pDevIns;
    Unit.pfnLivePrep = pfnLivePrep;
    Unit.pfnSavePrep = pfnSavePrep;
    Unit.pfnSaveExec = pfnSaveExec;
    Unit.pfnSaveDone = pfnSaveDone;
    Unit.pfnLoadPrep = pfnLoadPrep;
    Unit.pfnLoadExec = pfnLoadExec;
    Unit.pfnLoadDone = pfnLoadDone;
    return tstPdmSsmRegisterUnit(&Unit, pszBefore);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDBGFInfoRegister} */
static DECLCALLBACK(int) tstPdmHlp_DBGFInfoRegister(PPDMDEVINS pDevIns, const char *pszName, const char *pszDesc,
                                                    PFNDBGFHANDLERDEV pfnHandler)
{
    RT_NOREF(pDevIns, pszName, pszDesc, pfnHandler);
    return VINF_SUCCESS;
}


/**
 * Catches calls to device helpers which are not implemented by the mock.
 *
 * Run the testcase in a debugger to find out which helper the device called;
 * the caller is the frame above this function.
 */
static void tstPdmHlp_Unimplemented(void)
{
    AssertReleaseMsgFailed(("tstDevPciVfio: The device called a PDM helper the mock does not implement\n"));
}


/**
 * Sets up the helper tables.
 */
static void tstPdmHlpInit(void)
{
    /* All members but the version fields at the start and at the end are function
       pointers.  Let everything we don't implement end up in a trap instead of at NULL. */
    uintptr_t *pauSlots = (uintptr_t *)&g_tstPdmDevHlpR3;
    for (size_t i = 0; i < sizeof(g_tstPdmDevHlpR3) / sizeof(uintptr_t); i++)
        pauSlots[i] = (uintptr_t)tstPdmHlp_Unimplemented;

    g_tstPdmDevHlpR3.u32Version                         = PDM_DEVHLPR3_VERSION;
    g_tstPdmDevHlpR3.pfnIoPortCreateEx                  = tstPdmHlp_IoPortCreateEx;
    g_tstPdmDevHlpR3.pfnIoPortMap                       = tstPdmHlp_IoPortMap;
    g_tstPdmDevHlpR3.pfnIoPortUnmap                     = tstPdmHlp_IoPortUnmap;
    g_tstPdmDevHlpR3.pfnMmioCreateEx                    = tstPdmHlp_MmioCreateEx;
    g_tstPdmDevHlpR3.pfnMmioMap                         = tstPdmHlp_MmioMap;
    g_tstPdmDevHlpR3.pfnMmioUnmap                       = tstPdmHlp_MmioUnmap;
    g_tstPdmDevHlpR3.pfnMmio2Create                     = tstPdmHlp_Mmio2Create;
    g_tstPdmDevHlpR3.pfnMmio2CreateFromExisting         = tstPdmHlp_Mmio2CreateFromExisting;
    g_tstPdmDevHlpR3.pfnMmio2Map                        = tstPdmHlp_Mmio2Map;
    g_tstPdmDevHlpR3.pfnMmio2Unmap                      = tstPdmHlp_Mmio2Unmap;
    g_tstPdmDevHlpR3.pfnPGMHandlerPhysicalTypeRegister  = tstPdmHlp_PGMHandlerPhysicalTypeRegister;
    g_tstPdmDevHlpR3.pfnPGMHandlerPhysicalRegister      = tstPdmHlp_PGMHandlerPhysicalRegister;
    g_tstPdmDevHlpR3.pfnPGMHandlerPhysicalDeregister    = tstPdmHlp_PGMHandlerPhysicalDeregister;
    g_tstPdmDevHlpR3.pfnPhysGCPhys2CCPtr                = tstPdmHlp_PhysGCPhys2CCPtr;
    g_tstPdmDevHlpR3.pfnPhysReleasePageMappingLock      = tstPdmHlp_PhysReleasePageMappingLock;
    g_tstPdmDevHlpR3.pfnMMPhysGetRamSizeBelow4GB        = tstPdmHlp_MMPhysGetRamSizeBelow4GB;
    g_tstPdmDevHlpR3.pfnMMPhysGetRamSizeAbove4GB        = tstPdmHlp_MMPhysGetRamSizeAbove4GB;
    g_tstPdmDevHlpR3.pfnGetMainExecutionEngine          = tstPdmHlp_GetMainExecutionEngine;
    g_tstPdmDevHlpR3.pfnVMReqCallNoWaitV                = tstPdmHlp_VMReqCallNoWaitV;
    g_tstPdmDevHlpR3.pfnVMReqPriorityCallWaitV          = tstPdmHlp_VMReqPriorityCallWaitV;
    g_tstPdmDevHlpR3.pfnMMHeapAllocZ                    = tstPdmHlp_MMHeapAllocZ;
    g_tstPdmDevHlpR3.pfnMMHeapFree                      = tstPdmHlp_MMHeapFree;
    g_tstPdmDevHlpR3.pfnPCIBusRegister                  = tstPdmHlp_PCIBusRegister;
    g_tstPdmDevHlpR3.pfnPCIRegister                     = tstPdmHlp_PCIRegister;
    g_tstPdmDevHlpR3.pfnPCIRegisterMsi                  = tstPdmHlp_PCIRegisterMsi;
    g_tstPdmDevHlpR3.pfnPCIIORegionRegister             = tstPdmHlp_PCIIORegionRegister;
    g_tstPdmDevHlpR3.pfnPCIInterceptConfigAccesses      = tstPdmHlp_PCIInterceptConfigAccesses;
    g_tstPdmDevHlpR3.pfnPCIConfigWrite                  = tstPdmHlp_PCIConfigWrite;
    g_tstPdmDevHlpR3.pfnPCIConfigRead                   = tstPdmHlp_PCIConfigRead;
    g_tstPdmDevHlpR3.pfnPCISetIrq                       = tstPdmHlp_PCISetIrq;
    g_tstPdmDevHlpR3.pfnPCISetIrqNoWait                 = tstPdmHlp_PCISetIrq;
    g_tstPdmDevHlpR3.pfnThreadCreate                    = tstPdmHlp_ThreadCreate;
    g_tstPdmDevHlpR3.pfnVMSetErrorV                     = tstPdmHlp_VMSetErrorV;
    g_tstPdmDevHlpR3.pfnVMSetRuntimeErrorV              = tstPdmHlp_VMSetRuntimeErrorV;
    g_tstPdmDevHlpR3.pfnSSMSetLoadError                 = tstPdmHlp_SSMSetLoadError;
    g_tstPdmDevHlpR3.pfnSSMSetLoadErrorV                = tstPdmHlp_SSMSetLoadErrorV;
    g_tstPdmDevHlpR3.pfnSSMHandleGetStatus              = tstPdmHlp_SSMHandleGetStatus;
    g_tstPdmDevHlpR3.pfnSSMHandleGetAfter               = tstPdmHlp_SSMHandleGetAfter;
    g_tstPdmDevHlpR3.pfnSSMSetCfgError                  = tstPdmHlp_SSMSetCfgError;
    g_tstPdmDevHlpR3.pfnSSMPutU8                        = tstPdmHlp_SSMPutU8;
    g_tstPdmDevHlpR3.pfnSSMPutU32                       = tstPdmHlp_SSMPutU32;
    g_tstPdmDevHlpR3.pfnSSMPutU64                       = tstPdmHlp_SSMPutU64;
    g_tstPdmDevHlpR3.pfnSSMPutS32                       = tstPdmHlp_SSMPutS32;
    g_tstPdmDevHlpR3.pfnSSMPutBool                      = tstPdmHlp_SSMPutBool;
    g_tstPdmDevHlpR3.pfnSSMPutMem                       = tstPdmHlp_SSMPutMem;
    g_tstPdmDevHlpR3.pfnSSMGetU8                        = tstPdmHlp_SSMGetU8;
    g_tstPdmDevHlpR3.pfnSSMGetU32                       = tstPdmHlp_SSMGetU32;
    g_tstPdmDevHlpR3.pfnSSMGetU32V                      = tstPdmHlp_SSMGetU32V;
    g_tstPdmDevHlpR3.pfnSSMGetU64                       = tstPdmHlp_SSMGetU64;
    g_tstPdmDevHlpR3.pfnSSMGetS32                       = tstPdmHlp_SSMGetS32;
    g_tstPdmDevHlpR3.pfnSSMGetBool                      = tstPdmHlp_SSMGetBool;
    g_tstPdmDevHlpR3.pfnSSMGetMem                       = tstPdmHlp_SSMGetMem;
    g_tstPdmDevHlpR3.pfnVMState                         = tstPdmHlp_VMState;
    g_tstPdmDevHlpR3.pfnCritSectGetNop                  = tstPdmHlp_CritSectGetNop;
    g_tstPdmDevHlpR3.pfnSetDeviceCritSect               = tstPdmHlp_SetDeviceCritSect;
    g_tstPdmDevHlpR3.pfnSSMRegister                     = tstPdmHlp_SSMRegister;
    g_tstPdmDevHlpR3.pfnDBGFInfoRegister                = tstPdmHlp_DBGFInfoRegister;
    g_tstPdmDevHlpR3.pfnCFGMQueryString                 = tstPdmHlp_CFGMQueryString;
    g_tstPdmDevHlpR3.pfnCFGMQueryStringDef              = tstPdmHlp_CFGMQueryStringDef;
    g_tstPdmDevHlpR3.pfnCFGMQueryU64Def                 = tstPdmHlp_CFGMQueryU64Def;
    g_tstPdmDevHlpR3.pfnCFGMQueryBoolDef                = tstPdmHlp_CFGMQueryBoolDef;
    g_tstPdmDevHlpR3.pfnCFGMGetChildF                   = tstPdmHlp_CFGMGetChildF;
    g_tstPdmDevHlpR3.pfnCFGMGetChildFV                  = tstPdmHlp_CFGMGetChildFV;
    g_tstPdmDevHlpR3.pfnCFGMAreValuesValid              = tstPdmHlp_CFGMAreValuesValid;
    g_tstPdmDevHlpR3.pfnCFGMValidateConfig              = tstPdmHlp_CFGMValidateConfig;
    g_tstPdmDevHlpR3.u32TheEnd                          = PDM_DEVHLPR3_VERSION;

    g_tstPdmPciHlpR3.u32Version                         = PDM_PCIHLPR3_VERSION;
    g_tstPdmPciHlpR3.pfnIsaSetIrq                       = tstPdmPciHlp_IsaSetIrq;
    g_tstPdmPciHlpR3.pfnIoApicSetIrq                    = tstPdmPciHlp_IoApicSetIrq;
    g_tstPdmPciHlpR3.pfnIoApicSendMsi                   = tstPdmPciHlp_IoApicSendMsi;
    g_tstPdmPciHlpR3.pfnLock                            = tstPdmPciHlp_Lock;
    g_tstPdmPciHlpR3.pfnUnlock                          = tstPdmPciHlp_Unlock;
    g_tstPdmPciHlpR3.pfnGetBusByNo                      = tstPdmPciHlp_GetBusByNo;
    g_tstPdmPciHlpR3.u32TheEnd                          = PDM_PCIHLPR3_VERSION;
}


/*
 *
 * Interface for the tests.
 * Interface for the tests.
 * Interface for the tests.
 *
 */

/**
 * Initializes the PDM mock.
 *
 * @returns VBox status code.
 * @param   cbRam               Amount of guest RAM, all of it below 4 GiB.
 */
int tstPdmInit(uint32_t cbRam)
{
    AssertReturn(!g_Pdm.fInitialized, VERR_WRONG_ORDER);
    AssertReturn(cbRam && !(cbRam & GUEST_PAGE_OFFSET_MASK), VERR_INVALID_PARAMETER);

    RT_ZERO(g_Pdm);
    tstPdmHlpInit();

    int rc = RTCritSectInit(&g_Pdm.CritSectPci);
    if (RT_FAILURE(rc))
        return rc;
    rc = RTCritSectInit(&g_Pdm.CritSectIrq);
    if (RT_SUCCESS(rc))
    {
        rc = RTSemEventCreate(&g_Pdm.hEvtMsi);
        if (RT_SUCCESS(rc))
        {
            g_Pdm.pbRam = (uint8_t *)RTMemPageAllocZ(cbRam);
            if (g_Pdm.pbRam)
            {
                g_Pdm.cbRam        = cbRam;
                g_Pdm.bExecEngine  = VM_EXEC_ENGINE_NATIVE_API;
                g_Pdm.fCreating    = true;
                g_Pdm.fInitialized = true;
                g_Pdm.enmVmState   = VMSTATE_CREATING;

                /* What takes care of the guest memory has its saved state unit before any device gets created. */
                TSTPDMSSMUNIT Unit;
                RT_ZERO(Unit);
                RTStrCopy(Unit.szName, sizeof(Unit.szName), "pgm");
                Unit.uVersion    = 1;
                Unit.pfnSaveExec = tstPdmPgmSaveExec;
                Unit.pfnLoadExec = tstPdmPgmLoadExec;
                return tstPdmSsmRegisterUnit(&Unit, NULL /*pszBefore*/);
            }
            rc = VERR_NO_MEMORY;
            RTSemEventDestroy(g_Pdm.hEvtMsi);
        }
        RTCritSectDelete(&g_Pdm.CritSectIrq);
    }
    RTCritSectDelete(&g_Pdm.CritSectPci);
    return rc;
}


/**
 * Powers off the VM if necessary, destroys all devices and terminates the mock.
 */
void tstPdmTerm(void)
{
    if (!g_Pdm.fInitialized)
        return;
    tstPdmProcessRequests();

    tstPdmPowerOff();

    /* PDM destructs the devices in reverse order and takes their threads down afterwards. */
    for (uint32_t i = g_Pdm.cDevs; i-- > 0;)
    {
        PPDMDEVINS pDevIns = g_Pdm.aDevs[i].pDevIns;
        if (pDevIns->pReg->pfnDestruct)
            pDevIns->pReg->pfnDestruct(pDevIns);
    }
    for (uint32_t i = 0; i < g_Pdm.cThreads; i++)
        tstPdmThreadDestroy(g_Pdm.apThreads[i]);

    for (uint32_t i = 0; i < g_Pdm.cMmio2; i++)
        if (g_Pdm.aMmio2[i].fOwned)
            RTMemPageFree(g_Pdm.aMmio2[i].pbBacking, (size_t)g_Pdm.aMmio2[i].cbRegion);

    for (uint32_t i = 0; i < g_Pdm.cDevs; i++)
    {
        tstCfgNodeDestroy(g_Pdm.aDevs[i].pCfgInst);
        RTMemPageFree(g_Pdm.aDevs[i].pDevIns, g_Pdm.aDevs[i].cbDevIns);
    }
    for (uint32_t i = 0; i < g_Pdm.cZombies; i++)
    {
        tstCfgNodeDestroy(g_Pdm.aZombies[i].pCfgInst);
        RTMemPageFree(g_Pdm.aZombies[i].pDevIns, g_Pdm.aZombies[i].cbDevIns);
    }

    RTMemPageFree(g_Pdm.pbRam, g_Pdm.cbRam);
    RTSemEventDestroy(g_Pdm.hEvtMsi);
    RTCritSectDelete(&g_Pdm.CritSectIrq);
    RTCritSectDelete(&g_Pdm.CritSectPci);
    RT_ZERO(g_Pdm);
}


/**
 * Creates and constructs a device instance.
 *
 * The mock takes ownership of the configuration, also on failure.
 *
 * @returns VBox status code, the status of the constructor if it was reached.
 * @param   pReg                The device registration structure.
 * @param   iInstance           The instance number.
 * @param   pCfgInst            The instance node of the configuration; holds the PCI address
 *                              assignment and the "Config" child the device gets to see.
 * @param   ppDevIns            Where to return the device instance, optional.
 */
int tstPdmDevCreate(PCPDMDEVREG pReg, uint32_t iInstance, PCFGMNODE pCfgInst, PPDMDEVINS *ppDevIns)
{
    if (ppDevIns)
        *ppDevIns = NULL;
    AssertReturnStmt(g_Pdm.fInitialized && g_Pdm.fCreating, tstCfgNodeDestroy(pCfgInst), VERR_WRONG_ORDER);
    AssertReturnStmt(g_Pdm.cDevs < RT_ELEMENTS(g_Pdm.aDevs), tstCfgNodeDestroy(pCfgInst), VERR_OUT_OF_RESOURCES);

    PCFGMNODE pCfg = tstCfgFindChild(pCfgInst, "Config");
    if (!pCfg)
        pCfg = tstCfgNodeCreate(pCfgInst, "Config");
    AssertReturnStmt(pCfg, tstCfgNodeDestroy(pCfgInst), VERR_NO_MEMORY);

    /*
     * Lay out the instance like PDM does: context specific data, shared data,
     * critical section and the PCI devices.
     */
    uint32_t cb = RT_UOFFSETOF_DYN(PDMDEVINS, achInstanceData[pReg->cbInstanceCC]);
    cb = RT_ALIGN_32(cb, 64);
    uint32_t const offShared   = cb;
    cb += RT_ALIGN_32(pReg->cbInstanceShared, 64);
    uint32_t const offCritSect = cb;
    cb += RT_ALIGN_32(sizeof(PDMCRITSECT), 64);
    uint32_t const offPciDevs  = cb;
    uint32_t const cbMsixState = RT_ALIGN_32(pReg->cMaxMsixVectors * 16 + (pReg->cMaxMsixVectors + 7) / 8, _4K);
    uint32_t const cbPciDev    = RT_ALIGN_32(RT_UOFFSETOF_DYN(PDMPCIDEV, abMsixState[cbMsixState]), 64);
    uint32_t const cPciDevs    = RT_MIN(pReg->cMaxPciDevices, (uint32_t)RT_ELEMENTS(((PPDMDEVINS)NULL)->apPciDevs));
    cb += cbPciDev * cPciDevs;

    PPDMDEVINS pDevIns = (PPDMDEVINS)RTMemPageAllocZ(cb);
    AssertReturnStmt(pDevIns, tstCfgNodeDestroy(pCfgInst), VERR_NO_MEMORY);

    PTSTPDMDEV pDev = &g_Pdm.aDevs[g_Pdm.cDevs];
    pDev->pDevIns  = pDevIns;
    pDev->cbDevIns = cb;
    pDev->pCfgInst = pCfgInst;

    pDevIns->u32Version          = PDM_DEVINS_VERSION;
    pDevIns->iInstance           = iInstance;
    pDevIns->cbRing3             = cb;
    pDevIns->fR0Enabled          = false;
    pDevIns->fRCEnabled          = false;
    pDevIns->pHlpR3              = &g_tstPdmDevHlpR3;
    pDevIns->pvInstanceDataR3    = (uint8_t *)pDevIns + offShared;
    pDevIns->pvInstanceDataForR3 = &pDevIns->achInstanceData[0];
    pDevIns->pCritSectRoR3       = (PPDMCRITSECT)((uint8_t *)pDevIns + offCritSect);
    pDevIns->pReg                = pReg;
    pDevIns->pCfg                = pCfg;
    pDevIns->cbPciDev            = cbPciDev;
    pDevIns->cPciDevs            = cPciDevs;
    pDevIns->Internal.s.pDev     = pDev;
    for (uint32_t iPciDev = 0; iPciDev < cPciDevs; iPciDev++)
    {
        PPDMPCIDEV pPciDev = (PPDMPCIDEV)((uint8_t *)pDevIns + offPciDevs + cbPciDev * iPciDev);
        pDevIns->apPciDevs[iPciDev] = pPciDev;
        pPciDev->cbConfig           = _4K;
        pPciDev->cbMsixState        = (uint16_t)cbMsixState;
        pPciDev->idxSubDev          = (uint16_t)iPciDev;
        pPciDev->Int.s.idxSubDev    = (uint16_t)iPciDev;
        pPciDev->u32Magic           = PDMPCIDEV_MAGIC;
    }

    /* The device is in the list while it is being constructed, like with PDM. */
    g_Pdm.cDevs++;
    int rc = pReg->pfnConstruct(pDevIns, (int)iInstance, pCfg);
    if (RT_SUCCESS(rc))
    {
        if (ppDevIns)
            *ppDevIns = pDevIns;
        return rc;
    }

    /* PDM calls the destructor also when the constructor failed. */
    if (pReg->pfnDestruct)
        pReg->pfnDestruct(pDevIns);
    for (uint32_t i = 0; i < g_Pdm.cThreads;)
        if (g_Pdm.apThreads[i]->Core.u.Dev.pDevIns == pDevIns)
        {
            tstPdmThreadDestroy(g_Pdm.apThreads[i]);
            g_Pdm.apThreads[i] = g_Pdm.apThreads[--g_Pdm.cThreads];
        }
        else
            i++;
    for (unsigned i = 0; i < RT_ELEMENTS(g_Pdm.aPciBuses); i++)
        if (g_Pdm.aPciBuses[i].pDevIns == pDevIns)
            g_Pdm.aPciBuses[i].pDevIns = NULL;
    for (uint32_t i = 0; i < g_Pdm.cSsmUnits;)
        if (g_Pdm.aSsmUnits[i].pDevIns == pDevIns)
        {
            g_Pdm.cSsmUnits--;
            memmove(&g_Pdm.aSsmUnits[i], &g_Pdm.aSsmUnits[i + 1], (g_Pdm.cSsmUnits - i) * sizeof(g_Pdm.aSsmUnits[0]));
        }
        else
            i++;
    g_Pdm.cDevs--;

    /* Ranges the device has registered still refer to the instance, so keep it until termination. */
    AssertRelease(g_Pdm.cZombies < RT_ELEMENTS(g_Pdm.aZombies));
    g_Pdm.aZombies[g_Pdm.cZombies++] = *pDev;
    RT_ZERO(*pDev);
    return rc;
}


/**
 * Sets the main execution engine the devices get to see.
 *
 * @param   bExecEngine         VM_EXEC_ENGINE_XXX.
 */
void tstPdmSetExecutionEngine(uint8_t bExecEngine)
{
    g_Pdm.bExecEngine = bExecEngine;
}


/**
 * Tells the devices that the VM construction is complete, which is the last
 * thing that can fail it.
 *
 * @returns VBox status code, the first failure a device reports.
 */
int tstPdmInitComplete(void)
{
    AssertReturn(g_Pdm.fInitialized && g_Pdm.fCreating && !g_Pdm.fInitCompleteDone, VERR_WRONG_ORDER);

    g_Pdm.fInitCompleteDone = true;
    for (uint32_t i = 0; i < g_Pdm.cDevs; i++)
    {
        PPDMDEVINS pDevIns = g_Pdm.aDevs[i].pDevIns;
        if (pDevIns->pReg->pfnInitComplete)
        {
            int rc = pDevIns->pReg->pfnInitComplete(pDevIns);
            if (RT_FAILURE(rc))
                return rc;
        }
    }
    g_Pdm.enmVmState = VMSTATE_CREATED;
    return VINF_SUCCESS;
}


/**
 * Completes the VM construction if that wasn't done yet and powers the VM on.
 *
 * PDM doesn't run a VM whose construction failed.  The mock does, so tests can
 * check how a device behaves which was left behind half set up.
 */
void tstPdmPowerOn(void)
{
    AssertReturnVoid(g_Pdm.fInitialized && !g_Pdm.fRunning);

    if (g_Pdm.fCreating && !g_Pdm.fInitCompleteDone)
    {
        int rc = tstPdmInitComplete();
        AssertReleaseRC(rc);
    }
    g_Pdm.fCreating = false;

    for (uint32_t i = 0; i < g_Pdm.cDevs; i++)
    {
        PPDMDEVINS pDevIns = g_Pdm.aDevs[i].pDevIns;
        if (pDevIns->pReg->pfnPowerOn)
            pDevIns->pReg->pfnPowerOn(pDevIns);
    }
    g_Pdm.fRunning   = true;
    g_Pdm.enmVmState = VMSTATE_RUNNING;
    for (uint32_t i = 0; i < g_Pdm.cThreads; i++)
        tstPdmThreadResume(g_Pdm.apThreads[i]);
}


/**
 * Powers the VM off.
 */
void tstPdmPowerOff(void)
{
    if (   !g_Pdm.fInitialized
        || (   !g_Pdm.fRunning
            && g_Pdm.enmVmState != VMSTATE_SUSPENDED))
        return;

    for (uint32_t i = 0; i < g_Pdm.cThreads; i++)
        tstPdmThreadSuspend(g_Pdm.apThreads[i]);
    g_Pdm.fRunning = false;
    for (uint32_t i = g_Pdm.cDevs; i-- > 0;)
    {
        PPDMDEVINS pDevIns = g_Pdm.aDevs[i].pDevIns;
        if (pDevIns->pReg->pfnPowerOff)
            pDevIns->pReg->pfnPowerOff(pDevIns);
    }
    g_Pdm.enmVmState = VMSTATE_OFF;
}


/**
 * Suspends the running VM.  Requests for the EMT which are waiting keep waiting.
 */
void tstPdmSuspend(void)
{
    AssertReturnVoid(g_Pdm.fInitialized && g_Pdm.fRunning);

    for (uint32_t i = 0; i < g_Pdm.cDevs; i++)
    {
        PPDMDEVINS pDevIns = g_Pdm.aDevs[i].pDevIns;
        if (pDevIns->pReg->pfnSuspend)
            pDevIns->pReg->pfnSuspend(pDevIns);
    }
    for (uint32_t i = 0; i < g_Pdm.cThreads; i++)
        tstPdmThreadSuspend(g_Pdm.apThreads[i]);
    g_Pdm.fRunning   = false;
    g_Pdm.enmVmState = VMSTATE_SUSPENDED;
}


/**
 * Resumes the suspended VM, which is also how a VM gets going after a saved
 * state was loaded into it.
 */
void tstPdmResume(void)
{
    AssertReturnVoid(g_Pdm.fInitialized && !g_Pdm.fRunning && g_Pdm.enmVmState == VMSTATE_SUSPENDED);

    g_Pdm.fCreating = false;
    for (uint32_t i = 0; i < g_Pdm.cDevs; i++)
    {
        PPDMDEVINS pDevIns = g_Pdm.aDevs[i].pDevIns;
        if (pDevIns->pReg->pfnResume)
            pDevIns->pReg->pfnResume(pDevIns);
    }
    g_Pdm.fRunning   = true;
    g_Pdm.enmVmState = VMSTATE_RUNNING;
    for (uint32_t i = 0; i < g_Pdm.cThreads; i++)
        tstPdmThreadResume(g_Pdm.apThreads[i]);
}


/**
 * Does what devices have asked to be done on an EMT.  The functions through
 * which a test plays the guest do this before anything else, like an EMT
 * getting around to it before it goes back to running guest code.
 */
void tstPdmProcessRequests(void)
{
    if (!g_Pdm.fInitialized)
        return;
    for (;;)
    {
        PFNRT     pfn       = NULL;
        unsigned  cArgs     = 0;
        uintptr_t auArgs[4] = { 0, 0, 0, 0 };
        RTCritSectEnter(&g_Pdm.CritSectIrq);
        if (g_Pdm.cReqs)
        {
            pfn   = g_Pdm.aReqs[0].pfn;
            cArgs = g_Pdm.aReqs[0].cArgs;
            memcpy(auArgs, g_Pdm.aReqs[0].auArgs, sizeof(auArgs));
            g_Pdm.cReqs--;
            memmove(&g_Pdm.aReqs[0], &g_Pdm.aReqs[1], g_Pdm.cReqs * sizeof(g_Pdm.aReqs[0]));
        }
        RTCritSectLeave(&g_Pdm.CritSectIrq);
        if (!pfn)
            break;
        switch (cArgs)
        {
            case 0:  ((int (*)(void))pfn)(); break;
            case 1:  ((int (*)(uintptr_t))pfn)(auArgs[0]); break;
            case 2:  ((int (*)(uintptr_t, uintptr_t))pfn)(auArgs[0], auArgs[1]); break;
            case 3:  ((int (*)(uintptr_t, uintptr_t, uintptr_t))pfn)(auArgs[0], auArgs[1], auArgs[2]); break;
            default: ((int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))pfn)(auArgs[0], auArgs[1], auArgs[2], auArgs[3]); break;
        }
    }
}


/**
 * Returns the number of requests for the EMT devices have made so far.
 *
 * @returns Number of requests, those which are done included.
 */
uint32_t tstPdmGetRequestCount(void)
{
    return ASMAtomicReadU32(&g_Pdm.cReqsTotal);
}


/**
 * Selects whether the functions through which a test plays the guest do the
 * requests for the EMT first, or whether these wait for tstPdmProcessRequests().
 *
 * @param   fAutomatic          Whether the requests are done automatically.
 */
void tstPdmSetRequestProcessing(bool fAutomatic)
{
    g_Pdm.fManualReqs = !fAutomatic;
}


/**
 * Does the requests for the EMT unless the test wants them to wait.
 */
static void tstPdmAutoProcessRequests(void)
{
    if (!g_Pdm.fManualReqs)
        tstPdmProcessRequests();
}


/**
 * Resets the VM.
 */
void tstPdmReset(void)
{
    AssertReturnVoid(g_Pdm.fInitialized);
    tstPdmAutoProcessRequests();
    for (uint32_t i = 0; i < g_Pdm.cDevs; i++)
    {
        PPDMDEVINS pDevIns = g_Pdm.aDevs[i].pDevIns;
        if (pDevIns->pReg->pfnReset)
            pDevIns->pReg->pfnReset(pDevIns);
    }
}


/**
 * Performs a guest I/O port read.
 *
 * @returns VBox status code; ports nobody owns read as all ones.
 * @param   uPort               The port.
 * @param   pu32                Where to return the value.
 * @param   cb                  Number of bytes to read: 1, 2 or 4.
 */
int tstPdmIoPortRead(RTIOPORT uPort, uint32_t *pu32, unsigned cb)
{
    tstPdmAutoProcessRequests();
    for (uint32_t i = 0; i < g_Pdm.cIoPorts; i++)
        if (   g_Pdm.aIoPorts[i].fMapped
            && g_Pdm.aIoPorts[i].pfnIn
            && uPort >= g_Pdm.aIoPorts[i].uPort
            && (uint32_t)uPort + cb <= (uint32_t)g_Pdm.aIoPorts[i].uPort + g_Pdm.aIoPorts[i].cPorts)
        {
            RTIOPORT const offPort = g_Pdm.aIoPorts[i].fFlags & IOM_IOPORT_F_ABS ? uPort : uPort - g_Pdm.aIoPorts[i].uPort;
            *pu32 = 0;
            VBOXSTRICTRC rcStrict = g_Pdm.aIoPorts[i].pfnIn(g_Pdm.aIoPorts[i].pDevIns, g_Pdm.aIoPorts[i].pvUser, offPort,
                                                            pu32, cb);
            return VBOXSTRICTRC_VAL(rcStrict);
        }
    *pu32 = UINT32_MAX >> ((4 - cb) * 8);
    return VINF_SUCCESS;
}


/**
 * Performs a guest I/O port write.
 *
 * @returns VBox status code; writes to ports nobody owns are dropped.
 * @param   uPort               The port.
 * @param   u32                 The value.
 * @param   cb                  Number of bytes to write: 1, 2 or 4.
 */
int tstPdmIoPortWrite(RTIOPORT uPort, uint32_t u32, unsigned cb)
{
    tstPdmAutoProcessRequests();
    for (uint32_t i = 0; i < g_Pdm.cIoPorts; i++)
        if (   g_Pdm.aIoPorts[i].fMapped
            && g_Pdm.aIoPorts[i].pfnOut
            && uPort >= g_Pdm.aIoPorts[i].uPort
            && (uint32_t)uPort + cb <= (uint32_t)g_Pdm.aIoPorts[i].uPort + g_Pdm.aIoPorts[i].cPorts)
        {
            RTIOPORT const offPort = g_Pdm.aIoPorts[i].fFlags & IOM_IOPORT_F_ABS ? uPort : uPort - g_Pdm.aIoPorts[i].uPort;
            VBOXSTRICTRC rcStrict = g_Pdm.aIoPorts[i].pfnOut(g_Pdm.aIoPorts[i].pDevIns, g_Pdm.aIoPorts[i].pvUser, offPort,
                                                             u32, cb);
            return VBOXSTRICTRC_VAL(rcStrict);
        }
    return VINF_SUCCESS;
}


/**
 * Performs a guest physical memory read.
 *
 * MMIO ranges take precedence over MMIO2 ranges, which take precedence over RAM.
 *
 * @returns VBox status code; memory nobody owns reads as all ones.
 * @param   GCPhys              The guest physical address.
 * @param   pv                  Where to return the data.
 * @param   cb                  Number of bytes to read.
 */
int tstPdmPhysRead(RTGCPHYS GCPhys, void *pv, unsigned cb)
{
    tstPdmAutoProcessRequests();
    for (uint32_t i = 0; i < g_Pdm.cMmio; i++)
        if (   g_Pdm.aMmio[i].GCPhys != NIL_RTGCPHYS
            && g_Pdm.aMmio[i].pfnRead
            && GCPhys >= g_Pdm.aMmio[i].GCPhys
            && GCPhys + cb <= g_Pdm.aMmio[i].GCPhys + g_Pdm.aMmio[i].cbRegion)
        {
            RTGCPHYS const off = g_Pdm.aMmio[i].fFlags & IOMMMIO_FLAGS_ABS ? GCPhys : GCPhys - g_Pdm.aMmio[i].GCPhys;
            VBOXSTRICTRC rcStrict = g_Pdm.aMmio[i].pfnRead(g_Pdm.aMmio[i].pDevIns, g_Pdm.aMmio[i].pvUser, off, pv, cb);
            return VBOXSTRICTRC_VAL(rcStrict);
        }

    for (uint32_t i = 0; i < g_Pdm.cMmio2; i++)
        if (   g_Pdm.aMmio2[i].GCPhys != NIL_RTGCPHYS
            && GCPhys >= g_Pdm.aMmio2[i].GCPhys
            && GCPhys + cb <= g_Pdm.aMmio2[i].GCPhys + g_Pdm.aMmio2[i].cbRegion)
        {
            memcpy(pv, &g_Pdm.aMmio2[i].pbBacking[GCPhys - g_Pdm.aMmio2[i].GCPhys], cb);
            return VINF_SUCCESS;
        }

    if (GCPhys + cb <= g_Pdm.cbRam)
        memcpy(pv, &g_Pdm.pbRam[GCPhys], cb);
    else
        memset(pv, 0xff, cb);
    return VINF_SUCCESS;
}


/**
 * Performs a guest physical memory write.
 *
 * MMIO ranges take precedence over MMIO2 ranges, which take precedence over
 * RAM.  Write access handlers are consulted for MMIO2 ranges and RAM.
 *
 * @returns VBox status code; writes to memory nobody owns are dropped.
 * @param   GCPhys              The guest physical address.
 * @param   pv                  The data to write.
 * @param   cb                  Number of bytes to write.
 */
int tstPdmPhysWrite(RTGCPHYS GCPhys, const void *pv, unsigned cb)
{
    tstPdmAutoProcessRequests();
    for (uint32_t i = 0; i < g_Pdm.cMmio; i++)
        if (   g_Pdm.aMmio[i].GCPhys != NIL_RTGCPHYS
            && g_Pdm.aMmio[i].pfnWrite
            && GCPhys >= g_Pdm.aMmio[i].GCPhys
            && GCPhys + cb <= g_Pdm.aMmio[i].GCPhys + g_Pdm.aMmio[i].cbRegion)
        {
            RTGCPHYS const off = g_Pdm.aMmio[i].fFlags & IOMMMIO_FLAGS_ABS ? GCPhys : GCPhys - g_Pdm.aMmio[i].GCPhys;
            VBOXSTRICTRC rcStrict = g_Pdm.aMmio[i].pfnWrite(g_Pdm.aMmio[i].pDevIns, g_Pdm.aMmio[i].pvUser, off, pv, cb);
            return VBOXSTRICTRC_VAL(rcStrict);
        }

    uint8_t *pbDst = NULL;
    for (uint32_t i = 0; i < g_Pdm.cMmio2 && !pbDst; i++)
        if (   g_Pdm.aMmio2[i].GCPhys != NIL_RTGCPHYS
            && GCPhys >= g_Pdm.aMmio2[i].GCPhys
            && GCPhys + cb <= g_Pdm.aMmio2[i].GCPhys + g_Pdm.aMmio2[i].cbRegion)
            pbDst = &g_Pdm.aMmio2[i].pbBacking[GCPhys - g_Pdm.aMmio2[i].GCPhys];
    if (!pbDst && GCPhys + cb <= g_Pdm.cbRam)
        pbDst = &g_Pdm.pbRam[GCPhys];
    if (!pbDst)
        return VINF_SUCCESS;

    for (unsigned i = 0; i < RT_ELEMENTS(g_Pdm.aHnds); i++)
        if (   g_Pdm.aHnds[i].fUsed
            && GCPhys + cb - 1 >= g_Pdm.aHnds[i].GCPhys
            && GCPhys          <= g_Pdm.aHnds[i].GCPhysLast)
        {
            VBOXSTRICTRC rcStrict = g_Pdm.aHndTypes[g_Pdm.aHnds[i].idxType].pfnHandler(NULL /*pVM*/, NULL /*pVCpu*/, GCPhys,
                                                                                       pbDst, (void *)(uintptr_t)pv, cb,
                                                                                       PGMACCESSTYPE_WRITE,
                                                                                       PGMACCESSORIGIN_IEM, 0 /*uUser*/);
            if (rcStrict != VINF_PGM_HANDLER_DO_DEFAULT)
                return VBOXSTRICTRC_VAL(rcStrict);
            break;
        }

    memcpy(pbDst, pv, cb);
    return VINF_SUCCESS;
}


/**
 * Performs a guest memory fill hitting an MMIO range which has a fill handler.
 *
 * The handler gets what the guest asks for, it is up to it to stay inside its
 * range.
 *
 * @returns VBox status code, VERR_NOT_FOUND if there is no range with a fill handler at the address.
 * @param   GCPhys              Where to start filling.
 * @param   u32Item             The item to fill with.
 * @param   cbItem              Size of an item: 1, 2 or 4.
 * @param   cItems              Number of items.
 */
int tstPdmPhysFill(RTGCPHYS GCPhys, uint32_t u32Item, unsigned cbItem, unsigned cItems)
{
    tstPdmAutoProcessRequests();
    for (uint32_t i = 0; i < g_Pdm.cMmio; i++)
        if (   g_Pdm.aMmio[i].GCPhys != NIL_RTGCPHYS
            && g_Pdm.aMmio[i].pfnFill
            && GCPhys - g_Pdm.aMmio[i].GCPhys < g_Pdm.aMmio[i].cbRegion)
        {
            RTGCPHYS const off = g_Pdm.aMmio[i].fFlags & IOMMMIO_FLAGS_ABS ? GCPhys : GCPhys - g_Pdm.aMmio[i].GCPhys;
            VBOXSTRICTRC rcStrict = g_Pdm.aMmio[i].pfnFill(g_Pdm.aMmio[i].pDevIns, g_Pdm.aMmio[i].pvUser, off, u32Item,
                                                           cbItem, cItems);
            return VBOXSTRICTRC_VAL(rcStrict);
        }
    return VERR_NOT_FOUND;
}


/**
 * Checks whether a guest physical address is backed by a mapped MMIO2 range,
 * which means the guest accesses it without the device being involved.
 *
 * @returns true if there is an MMIO2 range mapped at the address, false if not.
 * @param   GCPhys              The guest physical address.
 */
bool tstPdmIsMmio2Mapped(RTGCPHYS GCPhys)
{
    for (uint32_t i = 0; i < g_Pdm.cMmio2; i++)
        if (   g_Pdm.aMmio2[i].GCPhys != NIL_RTGCPHYS
            && GCPhys - g_Pdm.aMmio2[i].GCPhys < g_Pdm.aMmio2[i].cbRegion)
            return true;
    return false;
}


/**
 * Returns how many MMIO2 ranges are mapped into the guest.
 *
 * @returns Number of mapped MMIO2 ranges.
 */
uint32_t tstPdmGetMmio2MappedCount(void)
{
    uint32_t cMapped = 0;
    for (uint32_t i = 0; i < g_Pdm.cMmio2; i++)
        if (g_Pdm.aMmio2[i].GCPhys != NIL_RTGCPHYS)
            cMapped++;
    return cMapped;
}


/**
 * Checks whether the VM is running.
 *
 * @returns true if the VM is running, false if it was not powered on yet, is suspended or off.
 */
bool tstPdmIsRunning(void)
{
    return g_Pdm.fRunning;
}


/**
 * Reads from the config space of a PCI device through the ECAM window.
 *
 * @returns The value, all ones if the access failed.
 * @param   iBus                The bus number.
 * @param   uDevFn              The device and function number.
 * @param   offReg              The register offset.
 * @param   cb                  Number of bytes to read: 1, 2 or 4.
 */
uint32_t tstPdmPciCfgRead(uint8_t iBus, uint8_t uDevFn, uint32_t offReg, unsigned cb)
{
    uint32_t u32Value = 0;
    int rc = tstPdmPhysRead(TSTPDM_MCFG_BASE + ((RTGCPHYS)iBus << 20) + ((RTGCPHYS)uDevFn << 12) + offReg, &u32Value, cb);
    return RT_SUCCESS(rc) ? u32Value : UINT32_MAX >> ((4 - cb) * 8);
}


/**
 * Writes to the config space of a PCI device through the ECAM window.
 *
 * @param   iBus                The bus number.
 * @param   uDevFn              The device and function number.
 * @param   offReg              The register offset.
 * @param   u32                 The value.
 * @param   cb                  Number of bytes to write: 1, 2 or 4.
 */
void tstPdmPciCfgWrite(uint8_t iBus, uint8_t uDevFn, uint32_t offReg, uint32_t u32, unsigned cb)
{
    tstPdmPhysWrite(TSTPDM_MCFG_BASE + ((RTGCPHYS)iBus << 20) + ((RTGCPHYS)uDevFn << 12) + offReg, &u32, cb);
}


/**
 * Returns the last VM error message a device has set.
 *
 * @returns The message, empty if none was set.
 */
const char *tstPdmGetLastError(void)
{
    return g_Pdm.szLastError;
}


/**
 * Returns the ID of the last runtime error a device has raised.
 *
 * @returns The ID, an empty string if there was none.
 */
const char *tstPdmGetLastRuntimeErrorId(void)
{
    return g_Pdm.szLastRuntimeErrorId;
}


/**
 * Asks the devices whether the state of the VM can be saved, the way it is
 * done before anything gets saved.  Nothing else of saving a state is done.
 *
 * @returns VBox status code, the first failure a device reports.
 * @param   fLive               Whether this is for a live save (teleportation) or an ordinary one.
 */
int tstPdmSaveStatePrep(bool fLive)
{
    AssertReturn(g_Pdm.fInitialized, VERR_WRONG_ORDER);
    g_Pdm.szLastError[0]          = '\0';
    g_Pdm.szLastRuntimeErrorId[0] = '\0';
    struct SSMHANDLE Handle;
    RT_ZERO(Handle);
    for (uint32_t i = 0; i < g_Pdm.cSsmUnits; i++)
    {
        PTSTPDMSSMUNIT pUnit = &g_Pdm.aSsmUnits[i];
        int rc = VINF_SUCCESS;
        if (fLive && pUnit->pfnLivePrep)
            rc = pUnit->pfnLivePrep(pUnit->pDevIns, &Handle);
        else if (!fLive && pUnit->pfnSavePrep)
            rc = pUnit->pfnSavePrep(pUnit->pDevIns, &Handle);
        if (RT_FAILURE(rc))
            return rc;
    }
    return VINF_SUCCESS;
}


/**
 * Asks the devices whether a saved state can be loaded, the way it is done
 * before anything gets loaded.  Nothing else of loading a state is done.
 *
 * @returns VBox status code, the first failure a device reports.
 */
int tstPdmLoadStatePrep(void)
{
    AssertReturn(g_Pdm.fInitialized, VERR_WRONG_ORDER);
    g_Pdm.szLastError[0] = '\0';
    struct SSMHANDLE Handle;
    RT_ZERO(Handle);
    Handle.fLoading = true;
    for (uint32_t i = 0; i < g_Pdm.cSsmUnits; i++)
        if (g_Pdm.aSsmUnits[i].pfnLoadPrep)
        {
            int rc = g_Pdm.aSsmUnits[i].pfnLoadPrep(g_Pdm.aSsmUnits[i].pDevIns, &Handle);
            if (RT_FAILURE(rc))
                return rc;
        }
    return VINF_SUCCESS;
}


/**
 * Tells the devices that loading a saved state is over, the way it is done
 * after everything was loaded or loading has failed.  Nothing else of loading a
 * state is done, the test puts what it wants to be loaded where it belongs.
 *
 * @returns VBox status code, the first failure a device reports.
 * @param   rcLoad              How loading went up to here, which the devices can ask for.
 */
int tstPdmLoadStateDone(int rcLoad)
{
    AssertReturn(g_Pdm.fInitialized, VERR_WRONG_ORDER);
    g_Pdm.szLastError[0] = '\0';
    struct SSMHANDLE Handle;
    RT_ZERO(Handle);
    Handle.fLoading = true;
    Handle.rc       = rcLoad;
    int rc = VINF_SUCCESS;
    for (uint32_t i = 0; i < g_Pdm.cSsmUnits; i++)
        if (g_Pdm.aSsmUnits[i].pfnLoadDone)
        {
            int rc2 = g_Pdm.aSsmUnits[i].pfnLoadDone(g_Pdm.aSsmUnits[i].pDevIns, &Handle);
            if (RT_FAILURE(rc2) && RT_SUCCESS(rc))
                rc = rc2;
        }
    return rc;
}


/**
 * Sets a function which is called between the preparations for saving or
 * loading a state and saving or loading the first unit, for doing what would
 * come from another thread at that time.
 *
 * @param   pfnHook             The function, NULL for none.
 * @param   pvUser              The user argument.
 */
void tstPdmSetStateHook(PFNTSTPDMSTATEHOOK pfnHook, void *pvUser)
{
    g_Pdm.pfnStateHook    = pfnHook;
    g_Pdm.pvStateHookUser = pvUser;
}


/**
 * Saves the state of the VM the way SSM does it: all units are asked to prepare,
 * then each saves its data, then all are told that it is over, also if it
 * failed.  A VM which runs is suspended on the way, a live save (teleportation)
 * requires one; the passes of a live save while the VM runs are left out.
 *
 * @returns VBox status code.
 * @param   fLive               Whether this is a live save or an ordinary one.
 * @param   ppvState            Where to return the saved state on success, to be freed
 *                              with tstPdmFreeState().
 * @param   pcbState            Where to return its size.
 */
int tstPdmSaveState(bool fLive, void **ppvState, size_t *pcbState)
{
    *ppvState = NULL;
    *pcbState = 0;
    AssertReturn(g_Pdm.fInitialized, VERR_WRONG_ORDER);
    AssertReturn(   g_Pdm.enmVmState == (fLive ? VMSTATE_RUNNING : VMSTATE_SUSPENDED)
                 || (!fLive && g_Pdm.enmVmState == VMSTATE_RUNNING),
                 VERR_VM_INVALID_VM_STATE);
    g_Pdm.szLastError[0]          = '\0';
    g_Pdm.szLastRuntimeErrorId[0] = '\0';
    for (uint32_t i = 0; i < g_Pdm.cSsmUnits; i++)
        g_Pdm.aSsmUnits[i].fCalled = false;

    struct SSMHANDLE Handle;
    RT_ZERO(Handle);
    int rc = VINF_SUCCESS;

    /* A live save starts while the VM runs.  The VM gets suspended for the rest. */
    if (fLive)
    {
        g_Pdm.enmVmState = VMSTATE_RUNNING_LS;
        for (uint32_t i = 0; i < g_Pdm.cSsmUnits && RT_SUCCESS(rc); i++)
        {
            PTSTPDMSSMUNIT pUnit = &g_Pdm.aSsmUnits[i];
            if (pUnit->pfnLivePrep)
            {
                pUnit->fCalled = true;
                rc = pUnit->pfnLivePrep(pUnit->pDevIns, &Handle);
                if (RT_FAILURE(rc) && RT_SUCCESS(Handle.rc))
                    Handle.rc = rc;
                else
                    rc = Handle.rc;
            }
        }
    }
    bool const fWasRunning = g_Pdm.fRunning;
    if (RT_SUCCESS(rc))
    {
        if (fWasRunning)
            tstPdmSuspend();
        g_Pdm.enmVmState = fLive ? VMSTATE_SUSPENDED_LS : VMSTATE_SAVING;

        for (uint32_t i = 0; i < g_Pdm.cSsmUnits && RT_SUCCESS(rc); i++)
        {
            PTSTPDMSSMUNIT pUnit = &g_Pdm.aSsmUnits[i];
            if (pUnit->pfnSavePrep)
            {
                pUnit->fCalled = true;
                rc = pUnit->pfnSavePrep(pUnit->pDevIns, &Handle);
                if (RT_FAILURE(rc) && RT_SUCCESS(Handle.rc))
                    Handle.rc = rc;
                else
                    rc = Handle.rc;
            }
        }

        if (RT_SUCCESS(rc) && g_Pdm.pfnStateHook)
            g_Pdm.pfnStateHook(g_Pdm.pvStateHookUser);

        TSTPDMSSMUNITHDR Hdr;
        for (uint32_t i = 0; i < g_Pdm.cSsmUnits && RT_SUCCESS(rc); i++)
        {
            PTSTPDMSSMUNIT pUnit = &g_Pdm.aSsmUnits[i];
            if (!pUnit->pfnSaveExec)
                continue;

            RT_ZERO(Hdr);
            RTStrCopy(Hdr.szName, sizeof(Hdr.szName), pUnit->szName);
            Hdr.uInstance = pUnit->uInstance;
            Hdr.uVersion  = pUnit->uVersion;
            size_t const offHdr = Handle.cb;
            tstPdmSsmWrite(&Handle, &Hdr, sizeof(Hdr));

            pUnit->fCalled = true;
            Handle.fExec = true;
            rc = pUnit->pfnSaveExec(pUnit->pDevIns, &Handle);
            Handle.fExec = false;
            if (RT_FAILURE(rc) && RT_SUCCESS(Handle.rc))
                Handle.rc = rc;
            else
                rc = Handle.rc;
            if (RT_SUCCESS(rc))
            {
                Hdr.cbData = (uint32_t)(Handle.cb - offHdr - sizeof(Hdr));
                memcpy(&Handle.pb[offHdr], &Hdr, sizeof(Hdr));
            }
        }
        if (RT_SUCCESS(rc))
        {
            RT_ZERO(Hdr);
            rc = tstPdmSsmWrite(&Handle, &Hdr, sizeof(Hdr));
        }
    }

    Handle.rc = rc;
    for (uint32_t i = 0; i < g_Pdm.cSsmUnits; i++)
    {
        PTSTPDMSSMUNIT pUnit = &g_Pdm.aSsmUnits[i];
        if (   pUnit->pfnSaveDone
            && (   pUnit->fCalled
                || (!pUnit->pfnSavePrep && !pUnit->pfnSaveExec)))
        {
            int rc2 = pUnit->pfnSaveDone(pUnit->pDevIns, &Handle);
            if (RT_FAILURE(rc2) && RT_SUCCESS(rc))
                rc = rc2;
        }
    }

    g_Pdm.enmVmState = g_Pdm.fRunning ? VMSTATE_RUNNING : VMSTATE_SUSPENDED;
    if (RT_SUCCESS(rc))
    {
        *ppvState = Handle.pb;
        *pcbState = Handle.cb;
    }
    else
        RTMemFree(Handle.pb);
    return rc;
}


/**
 * Frees a saved state.
 *
 * @param   pvState             The saved state, NULL is fine.
 */
void tstPdmFreeState(void *pvState)
{
    RTMemFree(pvState);
}


/**
 * Loads a saved state into the VM the way SSM does it: all units are asked to
 * prepare, then each unit of the state loads its data, then all are told that
 * it is over, also if it failed.  The VM has to be constructed and not powered
 * on yet, or suspended.  It is suspended afterwards, see tstPdmResume(), unless
 * loading failed, which leaves it good for nothing but being destroyed.
 *
 * @returns VBox status code.
 * @param   pvState             The saved state.
 * @param   cbState             Its size.
 */
int tstPdmLoadState(const void *pvState, size_t cbState)
{
    AssertReturn(g_Pdm.fInitialized, VERR_WRONG_ORDER);
    AssertReturn(   g_Pdm.enmVmState == VMSTATE_CREATED
                 || g_Pdm.enmVmState == VMSTATE_SUSPENDED,
                 VERR_VM_INVALID_VM_STATE);
    g_Pdm.szLastError[0] = '\0';
    for (uint32_t i = 0; i < g_Pdm.cSsmUnits; i++)
        g_Pdm.aSsmUnits[i].fCalled = false;

    struct SSMHANDLE Handle;
    RT_ZERO(Handle);
    Handle.pb       = (uint8_t *)(uintptr_t)pvState;
    Handle.cb       = cbState;
    Handle.fLoading = true;
    g_Pdm.enmVmState = VMSTATE_LOADING;

    int rc = VINF_SUCCESS;
    for (uint32_t i = 0; i < g_Pdm.cSsmUnits && RT_SUCCESS(rc); i++)
    {
        PTSTPDMSSMUNIT pUnit = &g_Pdm.aSsmUnits[i];
        if (pUnit->pfnLoadPrep)
        {
            pUnit->fCalled = true;
            rc = pUnit->pfnLoadPrep(pUnit->pDevIns, &Handle);
            if (RT_FAILURE(rc) && RT_SUCCESS(Handle.rc))
                Handle.rc = rc;
            else
                rc = Handle.rc;
        }
    }

    if (RT_SUCCESS(rc) && g_Pdm.pfnStateHook)
        g_Pdm.pfnStateHook(g_Pdm.pvStateHookUser);

    while (RT_SUCCESS(rc))
    {
        TSTPDMSSMUNITHDR Hdr;
        Handle.offUnitEnd = Handle.cb;
        rc = tstPdmSsmRead(&Handle, &Hdr, sizeof(Hdr));
        if (RT_FAILURE(rc) || !Hdr.szName[0])
            break;
        Hdr.szName[sizeof(Hdr.szName) - 1] = '\0';
        if (Hdr.cbData > Handle.cb - Handle.off)
        {
            rc = Handle.rc = VERR_SSM_INTEGRITY_UNIT;
            break;
        }

        PTSTPDMSSMUNIT pUnit = NULL;
        for (uint32_t i = 0; i < g_Pdm.cSsmUnits && !pUnit; i++)
            if (   g_Pdm.aSsmUnits[i].uInstance == Hdr.uInstance
                && !strcmp(g_Pdm.aSsmUnits[i].szName, Hdr.szName))
                pUnit = &g_Pdm.aSsmUnits[i];
        if (   !pUnit
            || !pUnit->pfnLoadExec)
        {
            if (!g_Pdm.szLastError[0])
                RTStrPrintf(g_Pdm.szLastError, sizeof(g_Pdm.szLastError), "Found no handler for unit '%s' instance #%u",
                            Hdr.szName, Hdr.uInstance);
            rc = Handle.rc = VERR_SSM_INTEGRITY_UNIT_NOT_FOUND;
            break;
        }

        Handle.offUnitEnd = Handle.off + Hdr.cbData;
        pUnit->fCalled = true;
        Handle.fExec = true;
        rc = pUnit->pfnLoadExec(pUnit->pDevIns, &Handle, Hdr.uVersion, SSM_PASS_FINAL);
        Handle.fExec = false;
        if (RT_FAILURE(rc) && RT_SUCCESS(Handle.rc))
            Handle.rc = rc;
        else
            rc = Handle.rc;
        if (RT_SUCCESS(rc) && Handle.off != Handle.offUnitEnd)
            rc = Handle.rc = VERR_SSM_LOADED_TOO_LITTLE;
    }

    Handle.rc = rc;
    for (uint32_t i = 0; i < g_Pdm.cSsmUnits; i++)
    {
        PTSTPDMSSMUNIT pUnit = &g_Pdm.aSsmUnits[i];
        if (   pUnit->pfnLoadDone
            && (   pUnit->fCalled
                || (!pUnit->pfnLoadPrep && !pUnit->pfnLoadExec)))
        {
            int const rcOld = Handle.rc;
            int       rc2   = pUnit->pfnLoadDone(pUnit->pDevIns, &Handle);
            if (RT_SUCCESS(rc2) && Handle.rc != rcOld)
                rc2 = Handle.rc;
            if (RT_FAILURE(rc2) && RT_SUCCESS(Handle.rc))
                Handle.rc = rc2;
        }
    }
    rc = Handle.rc;

    g_Pdm.enmVmState = RT_SUCCESS(rc) ? VMSTATE_SUSPENDED : VMSTATE_LOAD_FAILURE;
    return rc;
}


/**
 * Takes a unit out of a saved state, which makes it one of a VM which didn't
 * have the device.
 *
 * @returns VBox status code.
 * @retval  VERR_NOT_FOUND if the state has no such unit.
 * @param   pvState             The saved state.
 * @param   pcbState            Its size, updated.
 * @param   pszUnit             The name of the unit.
 * @param   uInstance           The instance of the unit.
 */
int tstPdmStateDropUnit(void *pvState, size_t *pcbState, const char *pszUnit, uint32_t uInstance)
{
    uint8_t * const pb  = (uint8_t *)pvState;
    size_t          off = 0;
    for (;;)
    {
        TSTPDMSSMUNITHDR Hdr;
        if (sizeof(Hdr) > *pcbState - off)
            return VERR_NOT_FOUND;
        memcpy(&Hdr, &pb[off], sizeof(Hdr));
        Hdr.szName[sizeof(Hdr.szName) - 1] = '\0';
        if (   !Hdr.szName[0]
            || Hdr.cbData > *pcbState - off - sizeof(Hdr))
            return VERR_NOT_FOUND;
        size_t const cbUnit = sizeof(Hdr) + Hdr.cbData;
        if (   Hdr.uInstance == uInstance
            && !strcmp(Hdr.szName, pszUnit))
        {
            memmove(&pb[off], &pb[off + cbUnit], *pcbState - off - cbUnit);
            *pcbState -= cbUnit;
            return VINF_SUCCESS;
        }
        off += cbUnit;
    }
}


/**
 * Changes an item in a saved state, for trying out states which don't go with
 * the VM they are loaded into or which make no sense.
 *
 * @returns VBox status code.
 * @retval  VERR_NOT_FOUND if the state has no such unit or the unit no such item.
 * @retval  VERR_MISMATCH if the item has another size.
 * @param   pvState             The saved state.
 * @param   cbState             Its size.
 * @param   pszUnit             The name of the unit.
 * @param   uInstance           The instance of the unit.
 * @param   idxItem             The item, counting what the unit has put from 0.
 * @param   offItem             Where in the item the new value goes, which has to be 0 unless
 *                              the item is a piece of memory.
 * @param   pvNew               The new value.
 * @param   cbNew               Its size, which has to be the size of the item unless the item
 *                              is a piece of memory.
 */
int tstPdmStatePatchItem(void *pvState, size_t cbState, const char *pszUnit, uint32_t uInstance, uint32_t idxItem,
                         size_t offItem, const void *pvNew, size_t cbNew)
{
    uint8_t * const pb  = (uint8_t *)pvState;
    size_t          off = 0;
    for (;;)
    {
        TSTPDMSSMUNITHDR Hdr;
        if (sizeof(Hdr) > cbState - off)
            return VERR_NOT_FOUND;
        memcpy(&Hdr, &pb[off], sizeof(Hdr));
        off += sizeof(Hdr);
        Hdr.szName[sizeof(Hdr.szName) - 1] = '\0';
        if (   !Hdr.szName[0]
            || Hdr.cbData > cbState - off)
            return VERR_NOT_FOUND;
        if (   Hdr.uInstance != uInstance
            || strcmp(Hdr.szName, pszUnit))
        {
            off += Hdr.cbData;
            continue;
        }

        size_t const offEnd = off + Hdr.cbData;
        for (uint32_t i = 0; off < offEnd; i++)
        {
            uint8_t const bKind  = pb[off++];
            size_t        cbItem = 0;
            switch (bKind)
            {
                case TSTPDM_SSM_ITEM_U8:
                case TSTPDM_SSM_ITEM_BOOL:
                    cbItem = sizeof(uint8_t);
                    break;
                case TSTPDM_SSM_ITEM_U32:
                case TSTPDM_SSM_ITEM_S32:
                    cbItem = sizeof(uint32_t);
                    break;
                case TSTPDM_SSM_ITEM_U64:
                    cbItem = sizeof(uint64_t);
                    break;
                case TSTPDM_SSM_ITEM_MEM:
                {
                    uint32_t cb32 = 0;
                    AssertReturn(sizeof(cb32) <= offEnd - off, VERR_INTERNAL_ERROR_2);
                    memcpy(&cb32, &pb[off], sizeof(cb32));
                    off   += sizeof(cb32);
                    cbItem = cb32;
                    break;
                }
                default:
                    AssertFailedReturn(VERR_INTERNAL_ERROR_3);
            }
            AssertReturn(cbItem <= offEnd - off, VERR_INTERNAL_ERROR_4);
            if (i == idxItem)
            {
                if (  bKind == TSTPDM_SSM_ITEM_MEM
                    ? offItem > cbItem || cbNew > cbItem - offItem
                    : offItem != 0 || cbNew != cbItem)
                    return VERR_MISMATCH;
                memcpy(&pb[off + offItem], pvNew, cbNew);
                return VINF_SUCCESS;
            }
            off += cbItem;
        }
        return VERR_NOT_FOUND;
    }
}


/**
 * Returns the number of MSIs which reached the interrupt controller.
 *
 * @returns Number of MSIs.
 */
uint32_t tstPdmMsiGetCount(void)
{
    RTCritSectEnter(&g_Pdm.CritSectIrq);
    uint32_t const cMsis = g_Pdm.cMsis;
    RTCritSectLeave(&g_Pdm.CritSectIrq);
    return cMsis;
}


/**
 * Waits until at least the given number of MSIs reached the interrupt controller.
 *
 * @returns true if the number was reached, false on timeout.
 * @param   cExpected           The number of MSIs to wait for.
 * @param   cMsTimeout          How long to wait.
 */
bool tstPdmMsiWait(uint32_t cExpected, RTMSINTERVAL cMsTimeout)
{
    uint64_t const msStart = RTTimeMilliTS();
    for (;;)
    {
        if (tstPdmMsiGetCount() >= cExpected)
            return true;
        uint64_t const cMsElapsed = RTTimeMilliTS() - msStart;
        if (cMsElapsed >= cMsTimeout)
            return false;
        RTSemEventWait(g_Pdm.hEvtMsi, (RTMSINTERVAL)RT_MIN(cMsTimeout - cMsElapsed, UINT64_C(100)));
    }
}


/**
 * Returns an MSI from the interrupt log.
 *
 * @returns true if the MSI exists, false if not.
 * @param   idxMsi              The index of the MSI, 0 being the first one delivered.
 * @param   pMsi                Where to return the MSI.
 */
bool tstPdmMsiGet(uint32_t idxMsi, PTSTPDMMSI pMsi)
{
    bool fRc = false;
    RTCritSectEnter(&g_Pdm.CritSectIrq);
    if (idxMsi < g_Pdm.cMsis && idxMsi < RT_ELEMENTS(g_Pdm.aMsis))
    {
        *pMsi = g_Pdm.aMsis[idxMsi];
        fRc   = true;
    }
    RTCritSectLeave(&g_Pdm.CritSectIrq);
    return fRc;
}


/**
 * Returns the MSI which reached the interrupt controller last.
 *
 * @returns true if there was one, false if not.
 * @param   pMsi                Where to return the MSI.
 */
bool tstPdmMsiGetLast(PTSTPDMMSI pMsi)
{
    RTCritSectEnter(&g_Pdm.CritSectIrq);
    bool const fRc = g_Pdm.cMsis != 0;
    *pMsi = g_Pdm.LastMsi;
    RTCritSectLeave(&g_Pdm.CritSectIrq);
    return fRc;
}


/**
 * Returns the number of blocks the devices currently hold from the MM heap.
 *
 * @returns Number of blocks.
 */
uint32_t tstPdmGetHeapAllocCount(void)
{
    return ASMAtomicReadU32(&g_Pdm.cHeapAllocs);
}
