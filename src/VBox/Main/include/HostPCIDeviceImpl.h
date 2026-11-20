/* $Id: HostPCIDeviceImpl.h 113422 2026-03-16 14:28:47Z alexander.eichner@oracle.com $ */
/** @file
 * VirtualBox Main - IHostPCIDevice implementation, VBoxSVC.
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

#ifndef MAIN_INCLUDED_HostPCIDeviceImpl_h
#define MAIN_INCLUDED_HostPCIDeviceImpl_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include "HostPCIDeviceWrap.h"

#include "PCIUpdateDevices.h"

class ATL_NO_VTABLE HostPCIDevice
    : public HostPCIDeviceWrap
{
public:
    DECLARE_COMMON_CLASS_METHODS(HostPCIDevice)

    HRESULT FinalConstruct();
    void FinalRelease();

    /** @name Public initializer/uninitializer for internal purposes only.
     * @{ */
    HRESULT initFromDevice(PPCIDEVICE pDev);
    void uninit() RT_OVERRIDE;
    /** @} */

private:
    /** @name wrapped IHostPCIDevice properties
     * @{ */
    HRESULT getState(PCIDeviceState_T *aState) RT_OVERRIDE;
    HRESULT getVendorId(USHORT *aVendorId) RT_OVERRIDE;
    HRESULT getDeviceId(USHORT *aDeviceId) RT_OVERRIDE;
    HRESULT getRevisionId(USHORT *aRevisionId) RT_OVERRIDE;
    HRESULT getDeviceClass(ULONG *aDeviceClass) RT_OVERRIDE;
    HRESULT getManufacturer(com::Utf8Str &aManufacturer) RT_OVERRIDE;
    HRESULT getProduct(com::Utf8Str &aProduct) RT_OVERRIDE;
    HRESULT getDomain(USHORT *aDomain) RT_OVERRIDE;
    HRESULT getBus(USHORT *aBus) RT_OVERRIDE;
    HRESULT getDevice(USHORT *aDevice) RT_OVERRIDE;
    HRESULT getDevFunction(USHORT *aFunction) RT_OVERRIDE;
    HRESULT getAddress(com::Utf8Str &aAddress) RT_OVERRIDE;
    /** @} */

    struct Data;
    Data *m;
};

#endif /* !MAIN_INCLUDED_HostPCIDeviceImpl_h */

