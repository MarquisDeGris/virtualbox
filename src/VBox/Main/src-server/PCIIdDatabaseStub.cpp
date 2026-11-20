/* $Id: PCIIdDatabaseStub.cpp 113422 2026-03-16 14:28:47Z alexander.eichner@oracle.com $ */
/** @file
 * PCI device vendor and product ID database - stub.
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

#include "PCIIdDatabase.h"

const RTBLDPROGSTRTAB   PCIIdDatabase::s_StrTab          =  { "", 0, 0, NULL };

const size_t            PCIIdDatabase::s_cVendors        = 0;
const PCIIDDBVENDOR     PCIIdDatabase::s_aVendors[]      = { {0,0,0} };
const RTBLDPROGSTRREF   PCIIdDatabase::s_aVendorNames[]  = { {0,0} };

const size_t            PCIIdDatabase::s_cProducts       = 0;
const PCIIDDBPROD       PCIIdDatabase::s_aProducts[]     = { {0} };
const RTBLDPROGSTRREF   PCIIdDatabase::s_aProductNames[] = { {0,0} };

