/* $Id: PCIUpdateDevices.h 113422 2026-03-16 14:28:47Z alexander.eichner@oracle.com $ */
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

#ifndef MAIN_INCLUDED_PCIUpdateDevices_h
#define MAIN_INCLUDED_PCIUpdateDevices_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <iprt/mem.h>
#include <iprt/list.h>
#include <iprt/string.h>


/**
 * PCI device state.
 */
typedef enum PCIDEVICESTATE
{
    kPciDeviceState_Invalid = 0,
    kPciDeviceState_NotSupported,
    kPciDeviceState_InUseByHost,
    kPciDeviceState_AccessDenied,
    kPciDeviceState_Available,
    kPciDeviceState_32Bit_Hack = 0x7fffffff
} PCIDEVICESTATE;


/**
 * PCI host device description.
 * Used for enumeration of PCI devices.
 */
typedef struct PCIDEVICE
{
    /** List node. */
    RTLISTNODE      NdLst;

    /** The device state. */
    PCIDEVICESTATE  enmState;
    /** Vendor ID. */
    uint16_t        idVendor;
    /** Device ID. */
    uint16_t        idDevice;
    /** Device base class, sub-class and programming interface. */
    uint32_t        u32DeviceClass;
    /** Device revision. */
    uint16_t        u16Revision;
    /** The subsystem ID. */
    uint16_t        idSubsystem;
    /** The subsystem vendor ID. */
    uint16_t        idSubsystemVendor;
    /** The PCI domain number (usually 0). */
    uint16_t        u16Domain;
    /** The PCI Bus number. */
    uint8_t         bBus;
    /** The PCI device number. */
    uint8_t         bDevice;
    /** The PCI function number. */
    uint8_t         bFunction;
    /** The host specific path to access the device. */
    const char      *pszPath;
    /** The driver responsible for the device. */
    const char      *pszDriver;
    /** The IOMMU group/domain for the device if any. */
    uint32_t        idIommuDomain;
} PCIDEVICE;
/** Pointer to a PCI device. */
typedef PCIDEVICE *PPCIDEVICE;
/** Pointer to a const PCI device. */
typedef PCIDEVICE *PCPCIDEVICE;

RT_C_DECLS_BEGIN


DECLHIDDEN(int) PCIUpdateDevices(PRTLISTANCHOR pLst);

RT_C_DECLS_END

#endif /* !MAIN_INCLUDED_PCIUpdateDevices_h */

