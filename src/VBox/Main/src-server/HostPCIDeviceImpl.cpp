/* $Id: HostPCIDeviceImpl.cpp 113422 2026-03-16 14:28:47Z alexander.eichner@oracle.com $ */
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

#define LOG_GROUP LOG_GROUP_MAIN_HOSTPCIDEVICE
#include "Global.h"
#include "HostPCIDeviceImpl.h"
#include "PCIIdDatabase.h"
#include "LoggingNew.h"
#include "VirtualBoxImpl.h"

//////////////////////////////////////////////////////////////////////////////////
//
// HostPCIDevice private data definition
//
//////////////////////////////////////////////////////////////////////////////////

struct HostPCIDevice::Data
{
    Data()
        : pDev(NULL)
    { }

    PPCIDEVICE pDev;
};


// constructor / destructor
/////////////////////////////////////////////////////////////////////////////
DEFINE_EMPTY_CTOR_DTOR(HostPCIDevice)

HRESULT HostPCIDevice::FinalConstruct()
{
    return BaseFinalConstruct();
}

void HostPCIDevice::FinalRelease()
{
    uninit();

    BaseFinalRelease();
}


// public initializer/uninitializer for internal purposes only
/////////////////////////////////////////////////////////////////////////////
HRESULT HostPCIDevice::initFromDevice(PPCIDEVICE pDev)
{
    LogFlowThisFunc(("\n"));

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    m = new Data();
    m->pDev = pDev;

    /* Confirm a successful initialization */
    autoInitSpan.setSucceeded();
    return S_OK;
}


/**
 * Uninitializes the instance.
 * Called either from FinalRelease() or by the parent when it gets destroyed.
 */
void HostPCIDevice::uninit()
{
    LogFlowThisFunc(("\n"));

    /* Enclose the state transition Ready->InUninit->NotReady */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    RTMemFree(m->pDev);
    m->pDev = NULL;

    delete m;
    m = NULL;
}


// IHostPCIDevice properties
/////////////////////////////////////////////////////////////////////////////

HRESULT HostPCIDevice::getState(PCIDeviceState_T *aState)
{
    switch (m->pDev->enmState)
    {
        case kPciDeviceState_Invalid:      *aState = PCIDeviceState_NotSupported; break;
        case kPciDeviceState_NotSupported: *aState = PCIDeviceState_NotSupported; break;
        case kPciDeviceState_InUseByHost:  *aState = PCIDeviceState_Unavailable;  break;
        case kPciDeviceState_AccessDenied: *aState = PCIDeviceState_AccessDenied; break;
        case kPciDeviceState_Available:    *aState = PCIDeviceState_Available;    break;
        default:
            AssertFailed();
            *aState = PCIDeviceState_NotSupported;
            break;
    }
    return S_OK;
}


HRESULT HostPCIDevice::getVendorId(USHORT *aVendorId)
{
    *aVendorId = m->pDev->idVendor;
    return S_OK;
}


HRESULT HostPCIDevice::getDeviceId(USHORT *aDeviceId)
{
    *aDeviceId = m->pDev->idDevice;
    return S_OK;
}


HRESULT HostPCIDevice::getRevisionId(USHORT *aRevisionId)
{
    *aRevisionId = m->pDev->u16Revision;
    return S_OK;
}


HRESULT HostPCIDevice::getDeviceClass(ULONG *aDeviceClass)
{
    *aDeviceClass = m->pDev->u32DeviceClass;
    return S_OK;
}


HRESULT HostPCIDevice::getManufacturer(com::Utf8Str &aManufacturer)
{
    Utf8Str strVendor = PCIIdDatabase::findVendor(m->pDev->idVendor);
    if (strVendor.isNotEmpty())
        aManufacturer = strVendor;
    else
    {
        Assert(strVendor.isEmpty());
        aManufacturer = Utf8Str("<unknown>");
    }

    return S_OK;
}


HRESULT HostPCIDevice::getProduct(com::Utf8Str &aProduct)
{
    Utf8Str strProduct = PCIIdDatabase::findProduct(m->pDev->idVendor, m->pDev->idDevice);
    if (strProduct.isNotEmpty())
        aProduct = strProduct;
    else
    {
        Assert(strProduct.isEmpty());
        aProduct = Utf8Str("<unknown>");
    }

    return S_OK;
}


HRESULT HostPCIDevice::getDomain(USHORT *aDomain)
{
    *aDomain = m->pDev->u16Domain;
    return S_OK;
}


HRESULT HostPCIDevice::getBus(USHORT *aBus)
{
    *aBus = m->pDev->bBus;
    return S_OK;
}


HRESULT HostPCIDevice::getDevice(USHORT *aDevice)
{
    *aDevice = m->pDev->bDevice;
    return S_OK;
}


HRESULT HostPCIDevice::getDevFunction(USHORT *aFunction)
{
    *aFunction = m->pDev->bFunction;
    return S_OK;
}


HRESULT HostPCIDevice::getAddress(com::Utf8Str &aAddress)
{
    aAddress = Utf8Str(m->pDev->pszPath);
    return S_OK;
}

