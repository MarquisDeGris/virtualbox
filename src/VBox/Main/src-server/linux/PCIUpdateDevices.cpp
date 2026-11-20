/* $Id: PCIUpdateDevices.cpp 113434 2026-03-16 15:32:03Z alexander.eichner@oracle.com $ */
/** @file
 * VirtualBox host PCI device enumeration.
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


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include "PCIUpdateDevices.h"

#include <VBox/err.h>
#include <VBox/pci.h>

#include <iprt/linux/sysfs.h>
#include <iprt/cdefs.h>
#include <iprt/ctype.h>
#include <iprt/dir.h>
#include <iprt/file.h>
#include <iprt/fs.h>
#include <iprt/log.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/string.h>

#include <unistd.h>


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

#define LINUX_PCI_SYSFS_PATH "/sys/bus/pci/devices"

/**
 * Updates the given list of PCI devices, looking for any changes.
 */
DECLHIDDEN(int) PCIUpdateDevices(PRTLISTANCHOR pLst)
{
    RTLISTANCHOR LstNew;
    RTListInit(&LstNew);

    RTDIR hDir = NIL_RTDIR;
    int vrc = RTDirOpen(&hDir, LINUX_PCI_SYSFS_PATH);
    if (RT_SUCCESS(vrc))
    {
        uint32_t const offSysfsPath = sizeof(LINUX_PCI_SYSFS_PATH);
        char szPath[RTPATH_MAX];
        memcpy(&szPath[0], LINUX_PCI_SYSFS_PATH, offSysfsPath - 1);
        szPath[offSysfsPath - 1] = '/';

        for (;;)
        {
            RTDIRENTRY Entry; /* The default name size is more than enough for the format xxxx:xx:xx.x */
            vrc = RTDirRead(hDir, &Entry, NULL /*pcbDirEntry*/);
            if (vrc == VERR_NO_MORE_FILES)
            {
                vrc = VINF_SUCCESS;
                break;
            }
            if (RT_FAILURE(vrc))
                break;

            if (RTDirEntryIsStdDotLink(&Entry))
                continue;

            if (Entry.cbName != sizeof("xxxx:xx:xx.x") - 1)
            {
                vrc = VERR_BUFFER_OVERFLOW;
                break;
            }

            /* Resolve the host address of the device from the directory name. */
            if (Entry.szName[4] != ':' || Entry.szName[7] != ':' || Entry.szName[10] != '.')
            {
                vrc = VERR_INVALID_PARAMETER;
                break;
            }

            uint16_t u16PciDomain = 0;
            vrc = RTStrToUInt16Ex(Entry.szName, NULL /*ppszNext*/, 16 | (4 << 8), &u16PciDomain);
            if (RT_FAILURE(vrc)) break;

            uint8_t bPciBus = 0;
            vrc = RTStrToUInt8Ex(&Entry.szName[5], NULL /*ppszNext*/, 16 | (2 << 8), &bPciBus);
            if (RT_FAILURE(vrc)) break;

            uint8_t bPciDev = 0;
            vrc = RTStrToUInt8Ex(&Entry.szName[8], NULL /*ppszNext*/, 16 | (2 << 8), &bPciDev);
            if (RT_FAILURE(vrc)) break;

            uint8_t bPciFun = 0;
            vrc = RTStrToUInt8Ex(&Entry.szName[11], NULL /*ppszNext*/, 16 | (1 << 8), &bPciFun);
            if (RT_FAILURE(vrc)) break;

            memcpy(&szPath[offSysfsPath], &Entry.szName[0], Entry.cbName + 1);
            szPath[offSysfsPath + Entry.cbName] = '\0';

            /* Read the basic data which should be always available. */
            int64_t i64VendorId;
            vrc = RTLinuxSysFsReadIntFile(16, &i64VendorId, "%s/vendor", szPath);
            if (RT_FAILURE(vrc)) break;
            Assert(i64VendorId == (uint16_t)i64VendorId);

            int64_t i64DeviceId;
            vrc = RTLinuxSysFsReadIntFile(16, &i64DeviceId, "%s/device", szPath);
            if (RT_FAILURE(vrc)) break;
            Assert(i64DeviceId == (uint16_t)i64DeviceId);

            int64_t i64Class;
            vrc = RTLinuxSysFsReadIntFile(16, &i64Class, "%s/class", szPath);
            if (RT_FAILURE(vrc)) break;
            Assert(i64Class == (uint32_t)i64Class);

            int64_t i64Revision;
            vrc = RTLinuxSysFsReadIntFile(16, &i64Revision, "%s/revision", szPath);
            if (RT_FAILURE(vrc)) break;
            Assert(i64Revision == (uint16_t)i64Revision);

            int64_t i64SubsystemVendorId;
            vrc = RTLinuxSysFsReadIntFile(16, &i64SubsystemVendorId, "%s/subsystem_vendor", szPath);
            if (RT_FAILURE(vrc)) break;
            Assert(i64SubsystemVendorId == (uint16_t)i64SubsystemVendorId);

            int64_t i64SubsystemDeviceId;
            vrc = RTLinuxSysFsReadIntFile(16, &i64SubsystemDeviceId, "%s/subsystem_device", szPath);
            if (RT_FAILURE(vrc)) break;
            Assert(i64SubsystemDeviceId == (uint16_t)i64SubsystemDeviceId);

            char szDriver[64];
            szDriver[0] = '\0';
            size_t cchDriver = 0;
            vrc = RTLinuxSysFsGetLinkDest(&szDriver[0], sizeof(szDriver), &cchDriver, "%s/driver", szPath);
            if (RT_FAILURE(vrc) && vrc != VERR_FILE_NOT_FOUND) break;
            if (cchDriver)
                cchDriver++;

            char szIommuGroup[8];
            szIommuGroup[0] = '\0';
            size_t cchIommuGroup = 0;
            vrc = RTLinuxSysFsGetLinkDest(&szIommuGroup[0], sizeof(szIommuGroup), &cchIommuGroup, "%s/iommu_group", szPath);
            if (RT_FAILURE(vrc) && vrc != VERR_FILE_NOT_FOUND) break;

            uint32_t idIommuDomain = UINT32_MAX;
            if (RT_SUCCESS(vrc))
                idIommuDomain = RTStrToUInt32(szIommuGroup);
            vrc = VINF_SUCCESS;

            /* Go through the list and look for the device. */
            bool fFound = false;
            PPCIDEVICE pIt, pItNext;
            RTListForEachSafe(pLst, pIt, pItNext, PCIDEVICE, NdLst)
            {
                if (   u16PciDomain         == pIt->u16Domain
                    && bPciBus              == pIt->bBus
                    && bPciDev              == pIt->bDevice
                    && bPciFun              == pIt->bFunction
                    && i64VendorId          == pIt->idVendor
                    && i64DeviceId          == pIt->idDevice
                    && i64Class             == pIt->u32DeviceClass
                    && i64Revision          == pIt->u16Revision
                    && i64SubsystemVendorId == pIt->idSubsystemVendor
                    && i64SubsystemDeviceId == pIt->idSubsystem
                    && idIommuDomain        == pIt->idIommuDomain
                    && !strcmp(szDriver, pIt->pszDriver))
                {
                    fFound = true;
                    break;
                }
            }

            if (fFound)
                RTListNodeRemove(&pIt->NdLst);
            else
            {
                /* Create new device. */
                size_t const cbPath = offSysfsPath + Entry.cbName + 1;

                PPCIDEVICE pNew = (PPCIDEVICE)RTMemAlloc(sizeof(*pNew) + cbPath + cchDriver);
                if (!pNew)
                {
                    vrc = VERR_NO_MEMORY;
                    break;
                }

                pNew->idVendor          = (uint16_t)i64VendorId;
                pNew->idDevice          = (uint16_t)i64DeviceId;
                pNew->u32DeviceClass    = (uint32_t)i64Class;
                pNew->u16Revision       = (uint16_t)i64Revision;
                pNew->idSubsystem       = (uint16_t)i64SubsystemDeviceId;
                pNew->idSubsystemVendor = (uint16_t)i64SubsystemVendorId;
                pNew->u16Domain         = u16PciDomain;
                pNew->bBus              = bPciBus;
                pNew->bDevice           = bPciDev;
                pNew->bFunction         = bPciFun;
                pNew->idIommuDomain     = idIommuDomain;
                pNew->pszPath           = (const char *)(pNew + 1);
                memcpy((void *)pNew->pszPath, szPath, cbPath);
                if (cchDriver)
                {
                    pNew->pszDriver = pNew->pszPath + cbPath;
                    memcpy((void *)pNew->pszDriver, szDriver, cchDriver);
                }
                else
                    pNew->pszDriver = NULL;

                pIt = pNew;
            }

            /* Set the state. */
            PCIDEVICESTATE enmState = kPciDeviceState_Invalid;
            if (   (pIt->u32DeviceClass >> 16) == VBOX_PCI_CLASS_BRIDGE
                || pIt->idIommuDomain == UINT32_MAX)
                enmState = kPciDeviceState_NotSupported;
            else if (   !pIt->pszDriver
                     || strcmp(pIt->pszDriver, "vfio-pci"))
                enmState = kPciDeviceState_InUseByHost;
            else
            {
                /*
                 * Check that either the vfio device is accessible
                 * or we can access the vfio group.
                 */
                char szDevPath[128];
                vrc = RTPathJoin(szDevPath, sizeof(szDevPath), "/dev/vfio", szIommuGroup);
                AssertRC(vrc);

                if (!access(szDevPath, R_OK | W_OK))
                    enmState = kPciDeviceState_Available;
                else if (!access("/dev/iommu", R_OK | W_OK))
                {
                    vrc = RTPathAppend(szPath, sizeof(szPath), "vfio-dev");
                    AssertRC(vrc);

                    /* Get at the vfioN device and check the access. */
                    RTDIR hDirVfio = NIL_RTDIR;
                    vrc = RTDirOpen(&hDirVfio, szPath);
                    if (RT_SUCCESS(vrc))
                    {
                        enmState = kPciDeviceState_AccessDenied;
                        for (;;)
                        {
                            RTDIRENTRY VfioEntry; /* The default name size is more than enough for the format xxxx:xx:xx.x */
                            vrc = RTDirRead(hDirVfio, &VfioEntry, NULL /*pcbDirEntry*/);
                            if (vrc == VERR_NO_MORE_FILES)
                            {
                                vrc = VINF_SUCCESS;
                                break;
                            }
                            if (RT_FAILURE(vrc))
                                break;

                            if (RTDirEntryIsStdDotLink(&VfioEntry))
                                continue;

                            if (!strncmp(VfioEntry.szName, RT_STR_TUPLE("vfio")))
                            {
                                vrc = RTPathJoin(szDevPath, sizeof(szDevPath), "/dev/vfio/devices", VfioEntry.szName);
                                AssertRC(vrc);
                                if (!access(szDevPath, R_OK | W_OK))
                                    enmState = kPciDeviceState_Available;
                                break;
                            }
                        }

                        int vrc2 = RTDirClose(hDirVfio);
                        AssertRC(vrc2); RT_NOREF(vrc2);
                    }

                    vrc = VINF_SUCCESS;
                }
                else
                    enmState = kPciDeviceState_AccessDenied;
            }

            pIt->enmState = enmState;

            /* Create a sorted list. */
            PPCIDEVICE pDev;
            bool fInserted = false;
            RTListForEach(&LstNew, pDev, PCIDEVICE, NdLst)
            {
                uint64_t u64Addr1 =   ((uint64_t)pIt->u16Domain << 32)
                                    | ((uint64_t)pIt->bBus << 16)
                                    | ((uint64_t)pIt->bDevice << 8)
                                    | (uint64_t)pIt->bFunction;
                uint64_t u64Addr2 =   ((uint64_t)pDev->u16Domain << 32)
                                    | ((uint64_t)pDev->bBus << 16)
                                    | ((uint64_t)pDev->bDevice << 8)
                                    | (uint64_t)pDev->bFunction;

                if (u64Addr1 < u64Addr2)
                {
                    RTListNodeInsertBefore(&pDev->NdLst, &pIt->NdLst);
                    fInserted = true;
                    break;
                }
            }
            if (!fInserted)
                RTListAppend(&LstNew, &pIt->NdLst);
        }

        int vrc2 = RTDirClose(hDir);
        AssertRC(vrc2); RT_NOREF(vrc2);

        /* Any devices still on the old list got removed, so delete the entries. */
        PPCIDEVICE pIt, pItSafe;
        RTListForEachSafe(pLst, pIt, pItSafe, PCIDEVICE, NdLst)
        {
            RTListNodeRemove(&pIt->NdLst);
            RTMemFree(pIt);
        }
        Assert(RTListIsEmpty(pLst));
        RTListMove(pLst, &LstNew);
    }
    else
        LogRel(("PCI: Opening /sys/bus/pci/devices failed with %Rrc\n", vrc));

    return vrc;
}


#ifdef STANDALONE_TESTCASE
#include <iprt/initterm.h>
#include <iprt/stream.h>

/**
 * This file can optionally be compiled into a testcase, this is the main function.
 * To build:
 *      g++ -I ../../../../include -D STANDALONE_TESTCASE -D IN_RING3 PCIUpdateDevices.cpp
 *          ../../../../out/linux.amd64/debug/lib/RuntimeR3.a
 *          ../../../../out/linux.amd64/debug/lib/SUPR3.a
 */
int main(int argc, char **argv)
{
    RTR3InitExe(argc, &argv, 0);

    /*
     * Get and display the PCI devices.
     */
    RTPrintf("PCI devices:\n");
    RTLISTANCHOR LstDevs;
    RTListInit(&LstDevs);
    int vrc = PCIUpdateDevices(&LstDevs);
    if (RT_SUCCESS(vrc))
    {
        PPCIDEVICE pIt, pItNext;
        RTListForEachSafe(&LstDevs, pIt, pItNext, PCIDEVICE, NdLst)
        {
            RTListNodeRemove(&pIt->NdLst);
            RTPrintf("%04x:%02x:%02x.%01x:\n", pIt->u16Domain, pIt->bBus, pIt->bDevice, pIt->bFunction);
            RTPrintf("    VendorId:          %#04x\n", pIt->idVendor);
            RTPrintf("    DeviceId:          %#04x\n", pIt->idDevice);
            RTPrintf("    Class:             %#06x\n", pIt->u32DeviceClass);
            RTPrintf("    Revision:          %#04x\n", pIt->u16Revision);
            RTPrintf("    SubsystemVendorId: %#04x\n", pIt->idSubsystemVendor);
            RTPrintf("    SubsystemId:       %#04x\n", pIt->idSubsystem);
            RTPrintf("    IOMMU Group:       %u\n",    pIt->idIommuDomain);
            RTPrintf("    Path:              %s\n",    pIt->pszPath);
            RTPrintf("    Driver:            %s\n",    pIt->pszDriver ? pIt->pszDriver : "");

            RTMemFree(pIt);
        }
    }
    else
        RTPrintf("Building the host PCI device list failed: %Rrc\n", vrc);

    return 0;
}
#endif
