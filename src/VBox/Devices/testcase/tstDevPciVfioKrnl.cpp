/* $Id$ */
/** @file
 * tstDevPciVfio - Fake VFIO/IOMMUFD kernel interface with a PCI device model.
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

/** @page pg_tst_devpcivfio_krnl    tstDevPciVfio - The fake kernel
 *
 * The VFIO passthrough device talks to the host kernel through open(),
 * close(), ioctl(), pread(), pwrite(), mmap(), poll() and eventfds.  The
 * testcase binary is linked with the GNU ld --wrap option for all but the last
 * two, which routes these calls from the device code to the wrappers at the end
 * of this file.  Calls concerning file descriptors or paths not owned by the fake
 * kernel are forwarded to the C library unchanged.
 *
 * A PCI function is modelled by a memory file (memfd).  Every VFIO region
 * lives at a fixed offset of that file, exactly like with the real vfio-pci
 * driver, which means pread(), pwrite() and mmap() on the device file
 * descriptor work without any emulation.  Interrupts are real eventfds, so
 * the interrupt poller of the device runs unmodified.
 *
 * The model does not emulate device behaviour (register write masks, BAR
 * sizing and the like); the config space is plain storage initialised from
 * the description the test supplies.  It does emulate the VFIO and IOMMUFD
 * object life cycles and argument validation closely enough to catch misuse.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <VBox/vmm/pdmdev.h>
#include <VBox/err.h>
#include <iprt/assert.h>
#include <iprt/critsect.h>
#include <iprt/mem.h>
#include <iprt/string.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "../Bus/DevPciVfio.h"
#include "tstDevPciVfio.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** Maximum number of PCI functions the fake kernel serves. */
#define TSTVFIO_MAX_DEVS            8
/** Maximum number of file descriptors the fake kernel hands out. */
#define TSTVFIO_MAX_FDS             64
/** Maximum number of DMA mappings. */
#define TSTVFIO_MAX_DMA             256
/** Maximum number of I/O address spaces. */
#define TSTVFIO_MAX_IOAS            16
/** Each region gets 16 GiB of file offset space. */
#define TSTVFIO_REGION_SHIFT        34
/** The page size DMA mappings have to be aligned to. */
#define TSTVFIO_PAGE_SIZE           UINT64_C(4096)


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * File descriptor type.
 */
typedef enum TSTVFIOFDTYPE
{
    /** The entry is unused. */
    TSTVFIOFDTYPE_FREE = 0,
    /** An IOMMUFD context. */
    TSTVFIOFDTYPE_IOMMU,
    /** A VFIO device. */
    TSTVFIOFDTYPE_DEVICE
} TSTVFIOFDTYPE;

/**
 * A PCI function of the fake kernel.
 */
typedef struct TSTVFIODEV
{
    /** The description supplied by the test. */
    TSTVFIODEVCFG       Cfg;
    /** The memory file backing all regions. */
    int                 iFdBacking;
    /** File offset of each region. */
    uint64_t            aoffRegions[TSTVFIO_REGION_COUNT];
    /** Size of each region, 0 if not implemented. */
    uint64_t            acbRegions[TSTVFIO_REGION_COUNT];
    /** Mappings of the regions for the test, created on demand. */
    void               *apvRegions[TSTVFIO_REGION_COUNT];
    /** The eventfds registered for each interrupt index, -1 if not set. */
    int                *apaiIrqFds[TSTVFIO_IRQ_INDEX_COUNT];
    /** Number of open file descriptors referring to this function. */
    uint32_t            cOpens;
    /** Number of resets requested. */
    uint32_t            cResets;
    /** Number of interrupts triggered per interrupt index. */
    uint32_t            acIrqsTriggered[TSTVFIO_IRQ_INDEX_COUNT];
    /** The I/O address space the function is attached to, 0 if none. */
    uint32_t            idIoas;
    /** Whether the function is bound to an IOMMUFD context. */
    bool                fBound;
    /** Whether the function was removed from the host. */
    bool                fGone;
    /** Whether INTx is masked. */
    bool                fIntxMasked;
} TSTVFIODEV;

/**
 * A file descriptor handed out by the fake kernel.
 */
typedef struct TSTVFIOFD
{
    /** The type. */
    TSTVFIOFDTYPE       enmType;
    /** The file descriptor number. */
    int                 iFd;
    /** The PCI function for TSTVFIOFDTYPE_DEVICE. */
    PTSTVFIODEV         pDev;
} TSTVFIOFD;
/** Pointer to a file descriptor entry. */
typedef TSTVFIOFD *PTSTVFIOFD;

/**
 * A DMA mapping.
 */
typedef struct TSTVFIODMA
{
    /** The I/O address space the mapping belongs to, 0 if the entry is unused. */
    uint32_t            idIoas;
    /** The I/O virtual address. */
    uint64_t            uIova;
    /** The size of the mapping. */
    uint64_t            cb;
    /** The process address backing the mapping. */
    uint64_t            uVirt;
} TSTVFIODMA;

/**
 * The fake kernel state.
 */
typedef struct TSTVFIOKRNL
{
    /** Whether the fake kernel is active. */
    bool                fInitialized;
    /** Serialises everything. */
    RTCRITSECT          CritSect;
    /** The PCI functions. */
    TSTVFIODEV          aDevs[TSTVFIO_MAX_DEVS];
    /** Number of PCI functions. */
    uint32_t            cDevs;
    /** The file descriptors. */
    TSTVFIOFD           aFds[TSTVFIO_MAX_FDS];
    /** The DMA mappings. */
    TSTVFIODMA          aDma[TSTVFIO_MAX_DMA];
    /** The I/O address spaces, 0 if the entry is unused. */
    uint32_t            aidIoas[TSTVFIO_MAX_IOAS];
    /** The next I/O address space ID. */
    uint32_t            idIoasNext;
    /** Fault injection: whether a failure is armed. */
    bool                fFailArmed;
    /** Fault injection: the ioctl request to fail. */
    unsigned long       uFailReq;
    /** Fault injection: number of matching requests to let pass first. */
    uint32_t            cFailSkip;
    /** Fault injection: the errno to fail with. */
    int                 iFailErrno;
} TSTVFIOKRNL;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
RT_C_DECLS_BEGIN
/* The C library functions behind the wrappers, see GNU ld --wrap. */
int     __real_open(const char *pszPath, int fFlags, ...);
int     __real_open64(const char *pszPath, int fFlags, ...);
int     __real_close(int iFd);
int     __real_ioctl(int iFd, unsigned long uReq, ...);
ssize_t __real_pread(int iFd, void *pvBuf, size_t cbBuf, off_t off);
ssize_t __real_pread64(int iFd, void *pvBuf, size_t cbBuf, off_t off);
ssize_t __real_pwrite(int iFd, const void *pvBuf, size_t cbBuf, off_t off);
ssize_t __real_pwrite64(int iFd, const void *pvBuf, size_t cbBuf, off_t off);
void   *__real_mmap(void *pvAddr, size_t cb, int fProt, int fFlags, int iFd, off_t off);
void   *__real_mmap64(void *pvAddr, size_t cb, int fProt, int fFlags, int iFd, off_t off);

/* The wrappers. */
int     __wrap_open(const char *pszPath, int fFlags, ...);
int     __wrap_open64(const char *pszPath, int fFlags, ...);
int     __wrap_close(int iFd);
int     __wrap_ioctl(int iFd, unsigned long uReq, ...);
ssize_t __wrap_pread(int iFd, void *pvBuf, size_t cbBuf, off_t off);
ssize_t __wrap_pread64(int iFd, void *pvBuf, size_t cbBuf, off_t off);
ssize_t __wrap_pwrite(int iFd, const void *pvBuf, size_t cbBuf, off_t off);
ssize_t __wrap_pwrite64(int iFd, const void *pvBuf, size_t cbBuf, off_t off);
void   *__wrap_mmap(void *pvAddr, size_t cb, int fProt, int fFlags, int iFd, off_t off);
void   *__wrap_mmap64(void *pvAddr, size_t cb, int fProt, int fFlags, int iFd, off_t off);
RT_C_DECLS_END


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/** The fake kernel. */
static TSTVFIOKRNL g_Krnl;


/**
 * Creates an anonymous memory file.
 *
 * @returns File descriptor or -1 with errno set.
 */
static int tstVfioKrnlMemFdCreate(void)
{
    return (int)syscall(SYS_memfd_create, "tstDevPciVfio", 0);
}


/**
 * Looks up a file descriptor.
 *
 * @returns Pointer to the entry or NULL if the descriptor is not ours.
 * @param   iFd                 The file descriptor.
 */
static PTSTVFIOFD tstVfioKrnlFdLookup(int iFd)
{
    if (iFd >= 0)
        for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aFds); i++)
            if (   g_Krnl.aFds[i].enmType != TSTVFIOFDTYPE_FREE
                && g_Krnl.aFds[i].iFd == iFd)
                return &g_Krnl.aFds[i];
    return NULL;
}


/**
 * Registers a new file descriptor.
 *
 * @returns Pointer to the entry or NULL if the table is full.
 * @param   iFd                 The file descriptor.
 * @param   enmType             The type.
 * @param   pDev                The PCI function for device descriptors.
 */
static PTSTVFIOFD tstVfioKrnlFdRegister(int iFd, TSTVFIOFDTYPE enmType, PTSTVFIODEV pDev)
{
    for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aFds); i++)
        if (g_Krnl.aFds[i].enmType == TSTVFIOFDTYPE_FREE)
        {
            g_Krnl.aFds[i].enmType = enmType;
            g_Krnl.aFds[i].iFd     = iFd;
            g_Krnl.aFds[i].pDev    = pDev;
            return &g_Krnl.aFds[i];
        }
    return NULL;
}


/**
 * Checks whether the given I/O address space exists.
 *
 * @returns true if it exists, false otherwise.
 * @param   idIoas              The I/O address space ID.
 */
static bool tstVfioKrnlIoasExists(uint32_t idIoas)
{
    if (idIoas)
        for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aidIoas); i++)
            if (g_Krnl.aidIoas[i] == idIoas)
                return true;
    return false;
}


/**
 * Removes an I/O address space and all its mappings.
 *
 * @param   idIoas              The I/O address space ID.
 */
static void tstVfioKrnlIoasDestroy(uint32_t idIoas)
{
    for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aDma); i++)
        if (g_Krnl.aDma[i].idIoas == idIoas)
            g_Krnl.aDma[i].idIoas = 0;
    for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aidIoas); i++)
        if (g_Krnl.aidIoas[i] == idIoas)
            g_Krnl.aidIoas[i] = 0;
}


/**
 * Drops the eventfds of one interrupt index.
 *
 * @param   pDev                The PCI function.
 * @param   uIrqIndex           The interrupt index.
 */
static void tstVfioKrnlDevIrqDisable(PTSTVFIODEV pDev, uint32_t uIrqIndex)
{
    for (uint32_t i = 0; i < pDev->Cfg.acIrqs[uIrqIndex]; i++)
        if (pDev->apaiIrqFds[uIrqIndex][i] != -1)
        {
            __real_close(pDev->apaiIrqFds[uIrqIndex][i]);
            pDev->apaiIrqFds[uIrqIndex][i] = -1;
        }
}


/**
 * Releases everything a user of the PCI function has set up, called when the
 * last file descriptor goes away.
 *
 * @param   pDev                The PCI function.
 */
static void tstVfioKrnlDevRelease(PTSTVFIODEV pDev)
{
    for (uint32_t i = 0; i < RT_ELEMENTS(pDev->apaiIrqFds); i++)
        tstVfioKrnlDevIrqDisable(pDev, i);
    pDev->fBound      = false;
    pDev->idIoas      = 0;
    pDev->fIntxMasked = false;
}


/**
 * Handles an ioctl on an IOMMUFD context.
 *
 * @returns 0 on success, -1 with errno set on failure.
 * @param   uReq                The request.
 * @param   pvArg               The argument.
 */
static int tstVfioKrnlIommuIoctl(unsigned long uReq, void *pvArg)
{
    switch (uReq)
    {
        case IOMMU_IOAS_ALLOC:
        {
            struct iommu_ioas_alloc *pAlloc = (struct iommu_ioas_alloc *)pvArg;
            if (pAlloc->size < sizeof(*pAlloc) || pAlloc->flags)
                return errno = EINVAL, -1;
            for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aidIoas); i++)
                if (!g_Krnl.aidIoas[i])
                {
                    g_Krnl.aidIoas[i]   = g_Krnl.idIoasNext++;
                    pAlloc->out_ioas_id = g_Krnl.aidIoas[i];
                    return 0;
                }
            return errno = ENOMEM, -1;
        }

        case IOMMU_IOAS_MAP:
        {
            struct iommu_ioas_map *pMap = (struct iommu_ioas_map *)pvArg;
            if (   pMap->size < sizeof(*pMap)
                || !(pMap->flags & IOMMU_IOAS_MAP_FIXED_IOVA) /* The model does not allocate IOVAs. */
                || (pMap->flags & ~(IOMMU_IOAS_MAP_FIXED_IOVA | IOMMU_IOAS_MAP_WRITEABLE | IOMMU_IOAS_MAP_READABLE))
                || !pMap->length
                || (pMap->length  & (TSTVFIO_PAGE_SIZE - 1))
                || (pMap->iova    & (TSTVFIO_PAGE_SIZE - 1))
                || (pMap->user_va & (TSTVFIO_PAGE_SIZE - 1))
                || pMap->iova + pMap->length < pMap->iova)
                return errno = EINVAL, -1;
            if (!tstVfioKrnlIoasExists(pMap->ioas_id))
                return errno = ENOENT, -1;

            unsigned iFree = RT_ELEMENTS(g_Krnl.aDma);
            for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aDma); i++)
                if (g_Krnl.aDma[i].idIoas != pMap->ioas_id)
                {
                    if (!g_Krnl.aDma[i].idIoas && iFree == RT_ELEMENTS(g_Krnl.aDma))
                        iFree = i;
                }
                else if (   pMap->iova                < g_Krnl.aDma[i].uIova + g_Krnl.aDma[i].cb
                         && pMap->iova + pMap->length > g_Krnl.aDma[i].uIova)
                    return errno = EEXIST, -1;
            if (iFree == RT_ELEMENTS(g_Krnl.aDma))
                return errno = ENOMEM, -1;

            g_Krnl.aDma[iFree].idIoas = pMap->ioas_id;
            g_Krnl.aDma[iFree].uIova  = pMap->iova;
            g_Krnl.aDma[iFree].cb     = pMap->length;
            g_Krnl.aDma[iFree].uVirt  = pMap->user_va;
            return 0;
        }

        case IOMMU_DESTROY:
        {
            struct iommu_destroy *pDestroy = (struct iommu_destroy *)pvArg;
            if (pDestroy->size < sizeof(*pDestroy))
                return errno = EINVAL, -1;
            if (!tstVfioKrnlIoasExists(pDestroy->id))
                return errno = ENOENT, -1;
            for (uint32_t i = 0; i < g_Krnl.cDevs; i++)
                if (g_Krnl.aDevs[i].idIoas == pDestroy->id)
                    return errno = EBUSY, -1;
            tstVfioKrnlIoasDestroy(pDestroy->id);
            return 0;
        }

        default:
            return errno = ENOTTY, -1;
    }
}


/**
 * Handles VFIO_DEVICE_GET_REGION_INFO.
 *
 * @returns 0 on success, -1 with errno set on failure.
 * @param   pDev                The PCI function.
 * @param   pInfo               The region info structure.
 */
static int tstVfioKrnlDevGetRegionInfo(PTSTVFIODEV pDev, struct vfio_region_info *pInfo)
{
    if (   pInfo->argsz < sizeof(*pInfo)
        || pInfo->index >= TSTVFIO_REGION_COUNT)
        return errno = EINVAL, -1;

    uint32_t const idxRegion = pInfo->index;
    uint64_t const cbRegion  = pDev->acbRegions[idxRegion];
    uint32_t       fFlags    = 0;
    if (cbRegion)
        fFlags = idxRegion == VFIO_PCI_CONFIG_REGION_INDEX
               ? VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE
               : pDev->Cfg.aRegions[idxRegion].fFlags & ~VFIO_REGION_INFO_FLAG_CAPS;

    pInfo->size       = cbRegion;
    pInfo->offset     = pDev->aoffRegions[idxRegion];
    pInfo->cap_offset = 0;
    if (   cbRegion
        && idxRegion != VFIO_PCI_CONFIG_REGION_INDEX
        && pDev->Cfg.aRegions[idxRegion].fMsixMappable)
    {
        /* Like the real thing: Report the size required if the capability doesn't fit. */
        uint32_t const cbNeeded = sizeof(*pInfo) + sizeof(struct vfio_info_cap_header);
        fFlags |= VFIO_REGION_INFO_FLAG_CAPS;
        if (pInfo->argsz >= cbNeeded)
        {
            struct vfio_info_cap_header *pCap = (struct vfio_info_cap_header *)(pInfo + 1);
            pCap->id          = VFIO_REGION_INFO_CAP_MSIX_MAPPABLE;
            pCap->version     = 1;
            pCap->next        = 0;
            pInfo->cap_offset = sizeof(*pInfo);
        }
        else
            pInfo->argsz = cbNeeded;
    }
    pInfo->flags = fFlags;
    return 0;
}


/**
 * Handles VFIO_DEVICE_SET_IRQS.
 *
 * @returns 0 on success, -1 with errno set on failure.
 * @param   pDev                The PCI function.
 * @param   pSet                The request, followed by the data.
 */
static int tstVfioKrnlDevSetIrqs(PTSTVFIODEV pDev, struct vfio_irq_set *pSet)
{
    uint32_t const fData   = pSet->flags & (VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_DATA_BOOL | VFIO_IRQ_SET_DATA_EVENTFD);
    uint32_t const fAction = pSet->flags & (VFIO_IRQ_SET_ACTION_MASK | VFIO_IRQ_SET_ACTION_UNMASK | VFIO_IRQ_SET_ACTION_TRIGGER);
    if (   pSet->argsz < sizeof(*pSet)
        || pSet->index >= TSTVFIO_IRQ_INDEX_COUNT
        || pSet->count >= UINT32_MAX - pSet->start
        || (pSet->flags & ~(fData | fAction))
        || !fData   || (fData   & (fData   - 1))   /* exactly one data type */
        || !fAction || (fAction & (fAction - 1)))  /* exactly one action */
        return errno = EINVAL, -1;

    uint32_t const cIrqs = pDev->Cfg.acIrqs[pSet->index];
    if (   pSet->start >= cIrqs
        || pSet->start + pSet->count > cIrqs)
        return errno = EINVAL, -1;

    size_t cbItem = 0;
    if (fData == VFIO_IRQ_SET_DATA_BOOL)
        cbItem = sizeof(uint8_t);
    else if (fData == VFIO_IRQ_SET_DATA_EVENTFD)
        cbItem = sizeof(int32_t);
    if (cbItem && pSet->argsz - sizeof(*pSet) < pSet->count * cbItem)
        return errno = EINVAL, -1;

    int * const paiFds = pDev->apaiIrqFds[pSet->index];
    if (fAction == VFIO_IRQ_SET_ACTION_TRIGGER)
    {
        if (fData == VFIO_IRQ_SET_DATA_EVENTFD)
        {
            int32_t const *paiNew = (int32_t const *)(pSet + 1);
            for (uint32_t i = 0; i < pSet->count; i++)
            {
                int iFdNew = -1;
                if (paiNew[i] != -1)
                {
                    iFdNew = dup(paiNew[i]);
                    if (iFdNew == -1)
                        return -1;
                }
                if (paiFds[pSet->start + i] != -1)
                    __real_close(paiFds[pSet->start + i]);
                paiFds[pSet->start + i] = iFdNew;
            }
        }
        else if (fData == VFIO_IRQ_SET_DATA_NONE && !pSet->count)
            tstVfioKrnlDevIrqDisable(pDev, pSet->index);
        else
        {
            /* Loopback trigger. */
            uint8_t const *pafFire = (uint8_t const *)(pSet + 1);
            for (uint32_t i = 0; i < pSet->count; i++)
                if (   (fData == VFIO_IRQ_SET_DATA_NONE || pafFire[i])
                    && paiFds[pSet->start + i] != -1)
                {
                    uint64_t const uOne = 1;
                    ssize_t cbWritten = write(paiFds[pSet->start + i], &uOne, sizeof(uOne));
                    RT_NOREF(cbWritten);
                }
        }
    }
    else if (pSet->index == VFIO_PCI_INTX_IRQ_INDEX)
        pDev->fIntxMasked = fAction == VFIO_IRQ_SET_ACTION_MASK;
    else
        return errno = EINVAL, -1; /* vfio-pci has no mask/unmask for MSI and MSI-X either. */

    return 0;
}


/**
 * Handles an ioctl on a VFIO device.
 *
 * @returns 0 on success, -1 with errno set on failure.
 * @param   pDev                The PCI function.
 * @param   uReq                The request.
 * @param   pvArg               The argument.
 */
static int tstVfioKrnlDevIoctl(PTSTVFIODEV pDev, unsigned long uReq, void *pvArg)
{
    if (pDev->fGone)
        return errno = ENODEV, -1;

    if (uReq == VFIO_DEVICE_BIND_IOMMUFD)
    {
        struct vfio_device_bind_iommufd *pBind = (struct vfio_device_bind_iommufd *)pvArg;
        PTSTVFIOFD pFdIommu = tstVfioKrnlFdLookup((int)pBind->iommufd);
        if (   pBind->argsz < RT_UOFFSETOF(struct vfio_device_bind_iommufd, token_uuid_ptr)
            || pBind->flags
            || pDev->fBound)
            return errno = EINVAL, -1;
        if (!pFdIommu || pFdIommu->enmType != TSTVFIOFDTYPE_IOMMU)
            return errno = EBADF, -1;
        pDev->fBound     = true;
        pBind->out_devid = (uint32_t)(pDev - &g_Krnl.aDevs[0]) + 1;
        return 0;
    }

    /* Nothing else works on a device which isn't bound yet. */
    if (!pDev->fBound)
        return errno = EINVAL, -1;

    switch (uReq)
    {
        case VFIO_DEVICE_ATTACH_IOMMUFD_PT:
        {
            struct vfio_device_attach_iommufd_pt *pAttach = (struct vfio_device_attach_iommufd_pt *)pvArg;
            if (pAttach->argsz < RT_UOFFSETOF(struct vfio_device_attach_iommufd_pt, pasid) || pAttach->flags)
                return errno = EINVAL, -1;
            if (!tstVfioKrnlIoasExists(pAttach->pt_id))
                return errno = ENOENT, -1;
            pDev->idIoas = pAttach->pt_id;
            return 0;
        }

        case VFIO_DEVICE_DETACH_IOMMUFD_PT:
        {
            struct vfio_device_detach_iommufd_pt *pDetach = (struct vfio_device_detach_iommufd_pt *)pvArg;
            if (pDetach->argsz < RT_UOFFSETOF(struct vfio_device_detach_iommufd_pt, pasid) || pDetach->flags)
                return errno = EINVAL, -1;
            pDev->idIoas = 0;
            return 0;
        }

        case VFIO_DEVICE_GET_INFO:
        {
            struct vfio_device_info *pInfo = (struct vfio_device_info *)pvArg;
            if (pInfo->argsz < RT_UOFFSETOF(struct vfio_device_info, cap_offset))
                return errno = EINVAL, -1;
            pInfo->flags       = VFIO_DEVICE_FLAGS_PCI | VFIO_DEVICE_FLAGS_RESET;
            pInfo->num_regions = TSTVFIO_REGION_COUNT;
            pInfo->num_irqs    = TSTVFIO_IRQ_INDEX_COUNT;
            if (pInfo->argsz >= sizeof(*pInfo))
            {
                pInfo->cap_offset = 0;
                pInfo->pad        = 0;
            }
            return 0;
        }

        case VFIO_DEVICE_GET_REGION_INFO:
            return tstVfioKrnlDevGetRegionInfo(pDev, (struct vfio_region_info *)pvArg);

        case VFIO_DEVICE_GET_IRQ_INFO:
        {
            struct vfio_irq_info *pInfo = (struct vfio_irq_info *)pvArg;
            if (   pInfo->argsz < sizeof(*pInfo)
                || pInfo->index >= TSTVFIO_IRQ_INDEX_COUNT)
                return errno = EINVAL, -1;
            pInfo->count = pDev->Cfg.acIrqs[pInfo->index];
            pInfo->flags = VFIO_IRQ_INFO_EVENTFD;
            if (pInfo->index == VFIO_PCI_INTX_IRQ_INDEX)
                pInfo->flags |= VFIO_IRQ_INFO_MASKABLE | VFIO_IRQ_INFO_AUTOMASKED;
            else
                pInfo->flags |= VFIO_IRQ_INFO_NORESIZE;
            return 0;
        }

        case VFIO_DEVICE_SET_IRQS:
            return tstVfioKrnlDevSetIrqs(pDev, (struct vfio_irq_set *)pvArg);

        case VFIO_DEVICE_RESET:
        {
            /* A function level reset brings the config space back to its power-on content. */
            ssize_t cbWritten = __real_pwrite(pDev->iFdBacking, pDev->Cfg.abCfg, pDev->Cfg.cbCfg,
                                              (off_t)pDev->aoffRegions[VFIO_PCI_CONFIG_REGION_INDEX]);
            if (cbWritten != (ssize_t)pDev->Cfg.cbCfg)
                return errno = EIO, -1;
            pDev->cResets++;
            return 0;
        }

        default:
            return errno = ENOTTY, -1;
    }
}


/**
 * Opens a node of the fake kernel.
 *
 * @returns File descriptor, -1 with errno set on failure, -2 if the path is not ours.
 * @param   pszPath             The path being opened.
 */
static int tstVfioKrnlOpen(const char *pszPath)
{
    if (!g_Krnl.fInitialized || !pszPath)
        return -2;

    bool const fIommu = !strcmp(pszPath, TSTVFIO_IOMMU_NODE);
    if (!fIommu && strncmp(pszPath, RT_STR_TUPLE("/dev/vfio/")))
        return -2;

    int iFd = -1;
    RTCritSectEnter(&g_Krnl.CritSect);
    if (fIommu)
    {
        iFd = tstVfioKrnlMemFdCreate();
        if (iFd != -1 && !tstVfioKrnlFdRegister(iFd, TSTVFIOFDTYPE_IOMMU, NULL))
        {
            __real_close(iFd);
            iFd   = -1;
            errno = EMFILE;
        }
    }
    else
    {
        /* Never let an unknown VFIO path through to the real host. */
        errno = ENOENT;
        for (uint32_t i = 0; i < g_Krnl.cDevs; i++)
            if (!strcmp(pszPath, g_Krnl.aDevs[i].Cfg.szNode))
            {
                PTSTVFIODEV pDev = &g_Krnl.aDevs[i];
                if (pDev->fGone)
                    errno = ENODEV;
                else
                {
                    iFd = dup(pDev->iFdBacking);
                    if (iFd != -1)
                    {
                        if (tstVfioKrnlFdRegister(iFd, TSTVFIOFDTYPE_DEVICE, pDev))
                            pDev->cOpens++;
                        else
                        {
                            __real_close(iFd);
                            iFd   = -1;
                            errno = EMFILE;
                        }
                    }
                }
                break;
            }
    }
    RTCritSectLeave(&g_Krnl.CritSect);
    return iFd;
}


/**
 * Checks whether the file descriptor refers to a PCI function which is gone.
 *
 * @returns true if I/O has to fail, false if it can go ahead.
 * @param   iFd                 The file descriptor.
 */
static bool tstVfioKrnlIsFdOfGoneDev(int iFd)
{
    bool fGone = false;
    if (g_Krnl.fInitialized)
    {
        RTCritSectEnter(&g_Krnl.CritSect);
        PTSTVFIOFD pFd = tstVfioKrnlFdLookup(iFd);
        fGone = pFd && pFd->enmType == TSTVFIOFDTYPE_DEVICE && pFd->pDev->fGone;
        RTCritSectLeave(&g_Krnl.CritSect);
    }
    return fGone;
}


/*
 *
 * Interface for the tests.
 * Interface for the tests.
 * Interface for the tests.
 *
 */

/**
 * Initializes the fake kernel.
 *
 * @returns VBox status code.
 */
int tstVfioKrnlInit(void)
{
    AssertReturn(!g_Krnl.fInitialized, VERR_WRONG_ORDER);

    RT_ZERO(g_Krnl);
    for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aFds); i++)
        g_Krnl.aFds[i].iFd = -1;
    g_Krnl.idIoasNext = 1;

    int rc = RTCritSectInit(&g_Krnl.CritSect);
    if (RT_SUCCESS(rc))
        g_Krnl.fInitialized = true;
    return rc;
}


/**
 * Terminates the fake kernel, releasing all PCI functions.
 *
 * File descriptors the device under test has leaked are closed here; use
 * tstVfioKrnlGetOpenFdCount() before calling this to detect leaks.
 */
void tstVfioKrnlTerm(void)
{
    if (!g_Krnl.fInitialized)
        return;

    g_Krnl.fInitialized = false;
    for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aFds); i++)
        if (g_Krnl.aFds[i].enmType != TSTVFIOFDTYPE_FREE)
            __real_close(g_Krnl.aFds[i].iFd);

    for (uint32_t i = 0; i < g_Krnl.cDevs; i++)
    {
        PTSTVFIODEV pDev = &g_Krnl.aDevs[i];
        for (uint32_t iRegion = 0; iRegion < RT_ELEMENTS(pDev->apvRegions); iRegion++)
            if (pDev->apvRegions[iRegion])
                munmap(pDev->apvRegions[iRegion], pDev->acbRegions[iRegion]);
        for (uint32_t iIrq = 0; iIrq < RT_ELEMENTS(pDev->apaiIrqFds); iIrq++)
            if (pDev->apaiIrqFds[iIrq])
            {
                tstVfioKrnlDevIrqDisable(pDev, iIrq);
                RTMemFree(pDev->apaiIrqFds[iIrq]);
            }
        __real_close(pDev->iFdBacking);
    }

    RTCritSectDelete(&g_Krnl.CritSect);
    RT_ZERO(g_Krnl);
}


/**
 * Adds a PCI function to the fake kernel.
 *
 * @returns VBox status code.
 * @param   pCfg                The description of the function, copied.
 * @param   ppDev               Where to return the handle.
 */
int tstVfioKrnlDevAdd(PCTSTVFIODEVCFG pCfg, PTSTVFIODEV *ppDev)
{
    AssertReturn(g_Krnl.fInitialized, VERR_WRONG_ORDER);
    AssertReturn(g_Krnl.cDevs < RT_ELEMENTS(g_Krnl.aDevs), VERR_OUT_OF_RESOURCES);
    AssertReturn(pCfg->cbCfg == 256 || pCfg->cbCfg == 4096, VERR_INVALID_PARAMETER);
    AssertReturn(RTStrEnd(pCfg->szNode, sizeof(pCfg->szNode)), VERR_INVALID_PARAMETER);

    PTSTVFIODEV pDev = &g_Krnl.aDevs[g_Krnl.cDevs];
    RT_ZERO(*pDev);
    pDev->Cfg = *pCfg;
    for (uint32_t i = 0; i < TSTVFIO_REGION_COUNT; i++)
    {
        pDev->aoffRegions[i] = (uint64_t)i << TSTVFIO_REGION_SHIFT;
        pDev->acbRegions[i]  = i == VFIO_PCI_CONFIG_REGION_INDEX ? pCfg->cbCfg : pCfg->aRegions[i].cb;
        AssertReturn(pDev->acbRegions[i] <= RT_BIT_64(TSTVFIO_REGION_SHIFT), VERR_INVALID_PARAMETER);
    }

    int rc = VINF_SUCCESS;
    pDev->iFdBacking = tstVfioKrnlMemFdCreate();
    if (pDev->iFdBacking == -1)
        return RTErrConvertFromErrno(errno);
    if (ftruncate(pDev->iFdBacking, (off_t)((uint64_t)TSTVFIO_REGION_COUNT << TSTVFIO_REGION_SHIFT)) != 0)
        rc = RTErrConvertFromErrno(errno);
    else if (   __real_pwrite(pDev->iFdBacking, pCfg->abCfg, pCfg->cbCfg, (off_t)pDev->aoffRegions[VFIO_PCI_CONFIG_REGION_INDEX])
             != (ssize_t)pCfg->cbCfg)
        rc = RTErrConvertFromErrno(errno);

    for (uint32_t i = 0; i < TSTVFIO_IRQ_INDEX_COUNT && RT_SUCCESS(rc); i++)
        if (pCfg->acIrqs[i])
        {
            pDev->apaiIrqFds[i] = (int *)RTMemAlloc(pCfg->acIrqs[i] * sizeof(int));
            if (pDev->apaiIrqFds[i])
                for (uint32_t iIrq = 0; iIrq < pCfg->acIrqs[i]; iIrq++)
                    pDev->apaiIrqFds[i][iIrq] = -1;
            else
                rc = VERR_NO_MEMORY;
        }

    if (RT_SUCCESS(rc))
    {
        g_Krnl.cDevs++;
        *ppDev = pDev;
    }
    else
    {
        for (uint32_t i = 0; i < TSTVFIO_IRQ_INDEX_COUNT; i++)
            RTMemFree(pDev->apaiIrqFds[i]);
        __real_close(pDev->iFdBacking);
        RT_ZERO(*pDev);
    }
    return rc;
}


/**
 * Returns a pointer to the memory of a region of the PCI function, for
 * inspecting what the guest wrote and for preparing what it will read.
 *
 * @returns Pointer to the start of the region, NULL on failure.
 * @param   pDev                The PCI function.
 * @param   iRegion             The VFIO region index.
 */
void *tstVfioKrnlDevGetRegionPtr(PTSTVFIODEV pDev, uint32_t iRegion)
{
    AssertReturn(iRegion < TSTVFIO_REGION_COUNT, NULL);
    AssertReturn(pDev->acbRegions[iRegion], NULL);

    if (!pDev->apvRegions[iRegion])
    {
        void *pv = mmap(NULL, pDev->acbRegions[iRegion], PROT_READ | PROT_WRITE, MAP_SHARED,
                        pDev->iFdBacking, (off_t)pDev->aoffRegions[iRegion]);
        AssertReturn(pv != MAP_FAILED, NULL);
        pDev->apvRegions[iRegion] = pv;
    }
    return pDev->apvRegions[iRegion];
}


/**
 * Lets the PCI function raise an interrupt.
 *
 * @returns VBox status code.
 * @retval  VERR_NOT_FOUND if no eventfd is registered for the vector.
 * @retval  VERR_INVALID_STATE if the interrupt is INTx and it is masked.
 * @param   pDev                The PCI function.
 * @param   uIrqIndex           The VFIO interrupt index.
 * @param   uVector             The vector within the index.
 */
int tstVfioKrnlDevTriggerIrq(PTSTVFIODEV pDev, uint32_t uIrqIndex, uint32_t uVector)
{
    AssertReturn(uIrqIndex < TSTVFIO_IRQ_INDEX_COUNT, VERR_INVALID_PARAMETER);
    AssertReturn(uVector < pDev->Cfg.acIrqs[uIrqIndex], VERR_INVALID_PARAMETER);

    int rc = VINF_SUCCESS;
    RTCritSectEnter(&g_Krnl.CritSect);
    if (pDev->apaiIrqFds[uIrqIndex][uVector] == -1)
        rc = VERR_NOT_FOUND;
    else if (uIrqIndex == VFIO_PCI_INTX_IRQ_INDEX && pDev->fIntxMasked)
        rc = VERR_INVALID_STATE;
    else
    {
        uint64_t const uOne = 1;
        if (write(pDev->apaiIrqFds[uIrqIndex][uVector], &uOne, sizeof(uOne)) != (ssize_t)sizeof(uOne))
            rc = RTErrConvertFromErrno(errno);
        else
        {
            pDev->acIrqsTriggered[uIrqIndex]++;
            /* INTx is level triggered and masked by vfio-pci until the user unmasks it. */
            if (uIrqIndex == VFIO_PCI_INTX_IRQ_INDEX)
                pDev->fIntxMasked = true;
        }
    }
    RTCritSectLeave(&g_Krnl.CritSect);
    return rc;
}


/**
 * Returns the number of vectors the user has registered eventfds for.
 *
 * @returns Number of vectors with an eventfd.
 * @param   pDev                The PCI function.
 * @param   uIrqIndex           The VFIO interrupt index.
 */
uint32_t tstVfioKrnlDevGetIrqCount(PTSTVFIODEV pDev, uint32_t uIrqIndex)
{
    AssertReturn(uIrqIndex < TSTVFIO_IRQ_INDEX_COUNT, 0);

    uint32_t cIrqs = 0;
    RTCritSectEnter(&g_Krnl.CritSect);
    for (uint32_t i = 0; i < pDev->Cfg.acIrqs[uIrqIndex]; i++)
        if (pDev->apaiIrqFds[uIrqIndex][i] != -1)
            cIrqs++;
    RTCritSectLeave(&g_Krnl.CritSect);
    return cIrqs;
}


/**
 * Returns the number of resets the user has requested for the PCI function.
 *
 * @returns Number of resets.
 * @param   pDev                The PCI function.
 */
uint32_t tstVfioKrnlDevGetResetCount(PTSTVFIODEV pDev)
{
    return pDev->cResets;
}


/**
 * Returns the number of file descriptors open for the PCI function.
 *
 * @returns Number of open file descriptors.
 * @param   pDev                The PCI function.
 */
uint32_t tstVfioKrnlDevGetOpenCount(PTSTVFIODEV pDev)
{
    return pDev->cOpens;
}


/**
 * Removes the PCI function from the host or brings it back.
 *
 * While the function is gone, every ioctl, read and write on its file
 * descriptors fails and it cannot be opened.
 *
 * @param   pDev                The PCI function.
 * @param   fGone               The new state.
 */
void tstVfioKrnlDevSetGone(PTSTVFIODEV pDev, bool fGone)
{
    RTCritSectEnter(&g_Krnl.CritSect);
    pDev->fGone = fGone;
    RTCritSectLeave(&g_Krnl.CritSect);
}


/**
 * Returns the number of file descriptors of the fake kernel which are open.
 *
 * @returns Number of open file descriptors.
 */
uint32_t tstVfioKrnlGetOpenFdCount(void)
{
    uint32_t cFds = 0;
    RTCritSectEnter(&g_Krnl.CritSect);
    for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aFds); i++)
        if (g_Krnl.aFds[i].enmType != TSTVFIOFDTYPE_FREE)
            cFds++;
    RTCritSectLeave(&g_Krnl.CritSect);
    return cFds;
}


/**
 * Returns the number of bytes mapped for DMA in all I/O address spaces.
 *
 * @returns Number of bytes.
 */
uint64_t tstVfioKrnlGetDmaMappedBytes(void)
{
    uint64_t cb = 0;
    RTCritSectEnter(&g_Krnl.CritSect);
    for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aDma); i++)
        if (g_Krnl.aDma[i].idIoas)
            cb += g_Krnl.aDma[i].cb;
    RTCritSectLeave(&g_Krnl.CritSect);
    return cb;
}


/**
 * Checks whether an I/O virtual address range is completely mapped for DMA
 * in any I/O address space.
 *
 * @returns true if every byte of the range is mapped, false otherwise.
 * @param   uIova               Start of the range.
 * @param   cb                  Size of the range.
 */
bool tstVfioKrnlIsIovaRangeMapped(uint64_t uIova, uint64_t cb)
{
    uint64_t const uEnd = uIova + cb;
    RTCritSectEnter(&g_Krnl.CritSect);
    bool fProgress = true;
    while (uIova < uEnd && fProgress)
    {
        fProgress = false;
        for (unsigned i = 0; i < RT_ELEMENTS(g_Krnl.aDma); i++)
            if (   g_Krnl.aDma[i].idIoas
                && uIova >= g_Krnl.aDma[i].uIova
                && uIova <  g_Krnl.aDma[i].uIova + g_Krnl.aDma[i].cb)
            {
                uIova     = g_Krnl.aDma[i].uIova + g_Krnl.aDma[i].cb;
                fProgress = true;
                break;
            }
    }
    RTCritSectLeave(&g_Krnl.CritSect);
    return uIova >= uEnd;
}


/**
 * Arms a single ioctl failure.
 *
 * @param   uReq                The ioctl request to fail.
 * @param   cSkip               Number of matching requests to let pass before failing one.
 * @param   iErrno              The errno to fail with, 0 to disarm.
 */
void tstVfioKrnlFailIoctl(unsigned long uReq, uint32_t cSkip, int iErrno)
{
    RTCritSectEnter(&g_Krnl.CritSect);
    g_Krnl.fFailArmed = iErrno != 0;
    g_Krnl.uFailReq   = uReq;
    g_Krnl.cFailSkip  = cSkip;
    g_Krnl.iFailErrno = iErrno;
    RTCritSectLeave(&g_Krnl.CritSect);
}


/*
 *
 * The wrappers, see GNU ld --wrap.
 * The wrappers, see GNU ld --wrap.
 * The wrappers, see GNU ld --wrap.
 *
 */

int __wrap_open(const char *pszPath, int fFlags, ...)
{
    int iFd = tstVfioKrnlOpen(pszPath);
    if (iFd != -2)
        return iFd;

    int fMode = 0;
    if ((fFlags & O_CREAT) || (fFlags & O_TMPFILE) == O_TMPFILE)
    {
        va_list va;
        va_start(va, fFlags);
        fMode = va_arg(va, int);
        va_end(va);
    }
    return __real_open(pszPath, fFlags, fMode);
}


int __wrap_open64(const char *pszPath, int fFlags, ...)
{
    int iFd = tstVfioKrnlOpen(pszPath);
    if (iFd != -2)
        return iFd;

    int fMode = 0;
    if ((fFlags & O_CREAT) || (fFlags & O_TMPFILE) == O_TMPFILE)
    {
        va_list va;
        va_start(va, fFlags);
        fMode = va_arg(va, int);
        va_end(va);
    }
    return __real_open64(pszPath, fFlags, fMode);
}


int __wrap_close(int iFd)
{
    if (g_Krnl.fInitialized)
    {
        RTCritSectEnter(&g_Krnl.CritSect);
        PTSTVFIOFD pFd = tstVfioKrnlFdLookup(iFd);
        if (pFd)
        {
            if (pFd->enmType == TSTVFIOFDTYPE_DEVICE)
            {
                PTSTVFIODEV pDev = pFd->pDev;
                Assert(pDev->cOpens > 0);
                if (--pDev->cOpens == 0)
                    tstVfioKrnlDevRelease(pDev);
            }
            else
            {
                /* Closing the IOMMUFD context destroys everything which lives in it. */
                for (uint32_t i = 0; i < g_Krnl.cDevs; i++)
                    g_Krnl.aDevs[i].idIoas = 0;
                RT_ZERO(g_Krnl.aDma);
                RT_ZERO(g_Krnl.aidIoas);
            }
            pFd->enmType = TSTVFIOFDTYPE_FREE;
            pFd->iFd     = -1;
            pFd->pDev    = NULL;
        }
        RTCritSectLeave(&g_Krnl.CritSect);
    }
    return __real_close(iFd);
}


int __wrap_ioctl(int iFd, unsigned long uReq, ...)
{
    va_list va;
    va_start(va, uReq);
    void * const pvArg = va_arg(va, void *);
    va_end(va);

    if (g_Krnl.fInitialized)
    {
        RTCritSectEnter(&g_Krnl.CritSect);
        PTSTVFIOFD pFd = tstVfioKrnlFdLookup(iFd);
        if (pFd)
        {
            int rcLnx;
            int iErrno = 0;
            if (g_Krnl.fFailArmed && g_Krnl.uFailReq == uReq && !g_Krnl.cFailSkip)
            {
                g_Krnl.fFailArmed = false;
                rcLnx  = -1;
                iErrno = g_Krnl.iFailErrno;
            }
            else
            {
                if (g_Krnl.fFailArmed && g_Krnl.uFailReq == uReq)
                    g_Krnl.cFailSkip--;
                if (pFd->enmType == TSTVFIOFDTYPE_IOMMU)
                    rcLnx = tstVfioKrnlIommuIoctl(uReq, pvArg);
                else
                    rcLnx = tstVfioKrnlDevIoctl(pFd->pDev, uReq, pvArg);
                if (rcLnx == -1)
                    iErrno = errno;
            }
            RTCritSectLeave(&g_Krnl.CritSect);
            if (rcLnx == -1)
                errno = iErrno;
            return rcLnx;
        }
        RTCritSectLeave(&g_Krnl.CritSect);
    }
    return __real_ioctl(iFd, uReq, pvArg);
}


ssize_t __wrap_pread(int iFd, void *pvBuf, size_t cbBuf, off_t off)
{
    if (tstVfioKrnlIsFdOfGoneDev(iFd))
        return errno = ENODEV, -1;
    return __real_pread(iFd, pvBuf, cbBuf, off);
}


ssize_t __wrap_pread64(int iFd, void *pvBuf, size_t cbBuf, off_t off)
{
    if (tstVfioKrnlIsFdOfGoneDev(iFd))
        return errno = ENODEV, -1;
    return __real_pread64(iFd, pvBuf, cbBuf, off);
}


ssize_t __wrap_pwrite(int iFd, const void *pvBuf, size_t cbBuf, off_t off)
{
    if (tstVfioKrnlIsFdOfGoneDev(iFd))
        return errno = ENODEV, -1;
    return __real_pwrite(iFd, pvBuf, cbBuf, off);
}


ssize_t __wrap_pwrite64(int iFd, const void *pvBuf, size_t cbBuf, off_t off)
{
    if (tstVfioKrnlIsFdOfGoneDev(iFd))
        return errno = ENODEV, -1;
    return __real_pwrite64(iFd, pvBuf, cbBuf, off);
}


/**
 * Checks whether the user may map the given part of a file of the fake kernel.
 *
 * Like vfio-pci this only allows mapping regions which are flagged as mappable.
 *
 * @returns true if the mapping can go ahead, false if it has to be refused.
 * @param   iFd                 The file descriptor.
 * @param   cb                  Size of the mapping.
 * @param   off                 Offset of the mapping.
 */
static bool tstVfioKrnlIsMmapAllowed(int iFd, size_t cb, off_t off)
{
    bool fAllowed = true;
    if (g_Krnl.fInitialized)
    {
        RTCritSectEnter(&g_Krnl.CritSect);
        PTSTVFIOFD pFd = tstVfioKrnlFdLookup(iFd);
        if (pFd)
        {
            fAllowed = false;
            if (pFd->enmType == TSTVFIOFDTYPE_DEVICE && !pFd->pDev->fGone)
            {
                PTSTVFIODEV pDev      = pFd->pDev;
                uint64_t    idxRegion = (uint64_t)off >> TSTVFIO_REGION_SHIFT;
                uint64_t    offRegion = (uint64_t)off & (RT_BIT_64(TSTVFIO_REGION_SHIFT) - 1);
                fAllowed =    idxRegion < TSTVFIO_REGION_COUNT
                           && idxRegion != VFIO_PCI_CONFIG_REGION_INDEX
                           && (pDev->Cfg.aRegions[idxRegion].fFlags & VFIO_REGION_INFO_FLAG_MMAP)
                           && offRegion + cb <= RT_ALIGN_64(pDev->acbRegions[idxRegion], TSTVFIO_PAGE_SIZE);
            }
        }
        RTCritSectLeave(&g_Krnl.CritSect);
    }
    return fAllowed;
}


void *__wrap_mmap(void *pvAddr, size_t cb, int fProt, int fFlags, int iFd, off_t off)
{
    if (!tstVfioKrnlIsMmapAllowed(iFd, cb, off))
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    return __real_mmap(pvAddr, cb, fProt, fFlags, iFd, off);
}


void *__wrap_mmap64(void *pvAddr, size_t cb, int fProt, int fFlags, int iFd, off_t off)
{
    if (!tstVfioKrnlIsMmapAllowed(iFd, cb, off))
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    return __real_mmap64(pvAddr, cb, fProt, fFlags, iFd, off);
}
