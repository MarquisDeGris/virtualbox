/* $Id: tstGuestControlSvc.cpp 106320 2024-10-15 12:08:41Z klaus.espenlaub@oracle.com $ */
/** @file
 * Testcase for the guest control service.
 */

/*
 * Copyright (C) 2011-2024 Oracle and/or its affiliates.
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
#include <VBox/HostServices/GuestControlSvc.h>
#include <VBox/VMMDev.h>
#include <iprt/assert.h>
#include <iprt/initterm.h>
#include <iprt/mem.h>
#include <iprt/stream.h>
#include <iprt/string.h>
#include <iprt/test.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** The ID of the client which the tests connect. */
#define TST_CLIENT_ID       UINT32_C(1)
/** The context ID of the messages which the host queues. */
#define TST_CONTEXT_ID      UINT32_C(1000)
/** The 32-bit value in the message which the host queues. */
#define TST_VALUE_U32       UINT32_C(0x12345678)
/** The 64-bit value in the message which the host queues. */
#define TST_VALUE_U64       UINT64_C(0x1122334455667788)
/** What a 32-bit parameter of the client holds before a call. */
#define TST_UNTOUCHED_U32   UINT32_C(0xfeedface)
/** What a 64-bit parameter of the client holds before a call. */
#define TST_UNTOUCHED_U64   UINT64_C(0xfeedfacefeedface)
/** What the buffer of the client is filled with before a call. */
#define TST_UNTOUCHED_CH    '#'
/** The number of parameters of the message which the host queues. */
#define TST_MSG_PARMS       4


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
static RTTEST g_hTest = NIL_RTTEST;

using namespace guestControl;

extern "C" DECLCALLBACK(DECLEXPORT(int)) VBoxHGCMSvcLoad(VBOXHGCMSVCFNTABLE *pTable);

/** Simple call handle structure for the guest call completion callback */
struct VBOXHGCMCALLHANDLE_TYPEDEF
{
    /** Where to store the result code. */
    int32_t  rc;
    /** How many times the call was completed. */
    uint32_t cCompleted;
};

/** The handle of the call which the client is making.  It is not on the stack:
 *  the service keeps the handle of a call which it does not complete. */
static struct VBOXHGCMCALLHANDLE_TYPEDEF g_Call;

/** The buffer in the message which the host queues. */
static char g_szTstData[] = "foo.bar";


/** Call completion callback for guest calls. */
static DECLCALLBACK(int) callComplete(VBOXHGCMCALLHANDLE callHandle, int32_t rc)
{
    callHandle->rc = rc;
    callHandle->cCompleted++;
    return VINF_SUCCESS;
}

/** @interface_method_impl{VBOXHGCMSVCHELPERS,pfnIsCallRestored} */
static DECLCALLBACK(bool) isCallRestored(VBOXHGCMCALLHANDLE callHandle)
{
    RT_NOREF(callHandle);
    return false;
}

/** @interface_method_impl{VBOXHGCMSVCHELPERS,pfnIsCallCancelled} */
static DECLCALLBACK(bool) isCallCancelled(VBOXHGCMCALLHANDLE callHandle)
{
    RT_NOREF(callHandle);
    return false;
}

/** @interface_method_impl{VBOXHGCMSVCHELPERS,pfnGetRequestor} */
static DECLCALLBACK(uint32_t) getRequestor(VBOXHGCMCALLHANDLE hCall)
{
    RT_NOREF(hCall);
    return VMMDEV_REQUESTOR_LEGACY;
}

/** @interface_method_impl{VBOXHGCMSVCHELPERS,pfnGetVMMDevSessionId} */
static DECLCALLBACK(uint64_t) getVMMDevSessionId(void *pvInstance)
{
    RT_NOREF(pvInstance);
    return 42;
}

/**
 * Initialise the HGCM service table as much as we need to start the
 * service.
 *
 * @return IPRT status code.
 * @param  pTable the table to initialise
 */
static int initTable(VBOXHGCMSVCFNTABLE *pTable, VBOXHGCMSVCHELPERS *pHelpers)
{
    RT_ZERO(*pTable);
    RT_ZERO(*pHelpers);
    pTable->cbSize = sizeof (VBOXHGCMSVCFNTABLE);
    pTable->u32Version = VBOX_HGCM_SVC_VERSION;
    pHelpers->pfnCallComplete       = callComplete;
    pHelpers->pfnIsCallRestored     = isCallRestored;
    pHelpers->pfnIsCallCancelled    = isCallCancelled;
    pHelpers->pfnGetRequestor       = getRequestor;
    pHelpers->pfnGetVMMDevSessionId = getVMMDevSessionId;
    pTable->pHelpers = pHelpers;

    return VINF_SUCCESS;
}

/**
 * Connects the client of the tests.
 *
 * The service keeps the state of a client in memory which HGCM hands to it:
 * VBOXHGCMSVCFNTABLE::cbClient bytes, zeroed.  The first client which connects
 * the way an old guest does is the one messages for the root service go to.
 *
 * @returns The memory with the state of the client, NULL on failure.
 * @param   pTable      The service table.
 */
static void *tstClientConnect(const VBOXHGCMSVCFNTABLE *pTable)
{
    void *pvClient = RTMemAllocZ(pTable->cbClient);
    if (!pvClient)
    {
        RTTestIFailed("no memory for the state of the client (%u bytes)", pTable->cbClient);
        return NULL;
    }
    int rc = pTable->pfnConnect(pTable->pvService, TST_CLIENT_ID, pvClient, VMMDEV_REQUESTOR_LEGACY, false /* fRestoring */);
    if (RT_FAILURE(rc))
    {
        RTTestIFailed("connecting the client failed with %Rrc", rc);
        RTMemFree(pvClient);
        return NULL;
    }
    return pvClient;
}

/**
 * Disconnects the client of the tests.
 *
 * @param   pTable      The service table.
 * @param   pvClient    What tstClientConnect returned.
 */
static void tstClientDisconnect(const VBOXHGCMSVCFNTABLE *pTable, void *pvClient)
{
    RTTESTI_CHECK_RC(pTable->pfnDisconnect(pTable->pvService, TST_CLIENT_ID, pvClient), VINF_SUCCESS);
    RTMemFree(pvClient);
}

/**
 * Makes a call of the client and returns what the service completed it with.
 *
 * No call the tests make is one the service keeps for later.
 *
 * @returns What the call was completed with, VERR_IPE_UNEXPECTED_STATUS if it
 *          was not completed exactly once.
 * @param   pTable      The service table.
 * @param   pvClient    The state of the client.
 * @param   idFunction  The function to call.
 * @param   cParms      The number of parameters.
 * @param   paParms     The parameters.
 */
static int tstClientCall(const VBOXHGCMSVCFNTABLE *pTable, void *pvClient, uint32_t idFunction,
                         uint32_t cParms, PVBOXHGCMSVCPARM paParms)
{
    g_Call.rc         = VERR_IPE_UNINITIALIZED_STATUS;
    g_Call.cCompleted = 0;
    pTable->pfnCall(pTable->pvService, &g_Call, TST_CLIENT_ID, pvClient, idFunction, cParms, paParms, 0 /* tsArrival */);
    if (g_Call.cCompleted != 1)
    {
        RTTestIFailed("function %u with %u parameters was completed %u times", idFunction, cParms, g_Call.cCompleted);
        return VERR_IPE_UNEXPECTED_STATUS;
    }
    return g_Call.rc;
}

/**
 * Lets the host queue the message which the client tests fetch: a context
 * ID, a 32-bit value, a 64-bit value and a buffer.
 *
 * @returns What the service returned.
 * @param   pTable      The service table.
 */
static int tstHostQueueMsg(const VBOXHGCMSVCFNTABLE *pTable)
{
    VBOXHGCMSVCPARM aParms[TST_MSG_PARMS];
    HGCMSvcSetU64(&aParms[0], VBOX_GUESTCTRL_DST_ROOT_SVC | TST_CONTEXT_ID);
    HGCMSvcSetU32(&aParms[1], TST_VALUE_U32);
    HGCMSvcSetU64(&aParms[2], TST_VALUE_U64);
    HGCMSvcSetPv(&aParms[3], g_szTstData, sizeof(g_szTstData));
    return pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, RT_ELEMENTS(aParms), aParms);
}

/**
 * Sets up the parameters with which a client fetches the message of
 * tstHostQueueMsg, each holding what tells that it was not written to.
 *
 * @param   paParms     The parameters, TST_MSG_PARMS of them.
 * @param   pchBuf      The buffer of the client.
 * @param   cbBuf       Its size.
 */
static void tstSetFetchParms(PVBOXHGCMSVCPARM paParms, char *pchBuf, uint32_t cbBuf)
{
    /* The first parameter names the message the client expects, and gets the context ID. */
    HGCMSvcSetU32(&paParms[0], (uint32_t)HOST_MSG_EXEC_CMD);
    HGCMSvcSetU32(&paParms[1], TST_UNTOUCHED_U32);
    HGCMSvcSetU64(&paParms[2], TST_UNTOUCHED_U64);
    memset(pchBuf, TST_UNTOUCHED_CH, cbBuf);
    HGCMSvcSetPv(&paParms[3], pchBuf, cbBuf);
}

/**
 * Tells whether a buffer of the client was not written to.
 *
 * @returns true if every byte is what tstSetFetchParms put there.
 * @param   pchBuf      The buffer.
 * @param   cbBuf       Its size.
 */
static bool tstIsBufUntouched(const char *pchBuf, size_t cbBuf)
{
    for (size_t off = 0; off < cbBuf; off++)
        if (pchBuf[off] != TST_UNTOUCHED_CH)
            return false;
    return true;
}

/**
 * Checks that the client has got the message of tstHostQueueMsg.
 *
 * @param   paParms     The parameters after the call.
 * @param   pchBuf      The buffer of the client.
 * @param   cbBuf       Its size.
 */
static void tstCheckFetched(PVBOXHGCMSVCPARM paParms, const char *pchBuf, size_t cbBuf)
{
    RTTESTI_CHECK_MSG(paParms[0].u.uint32 == TST_CONTEXT_ID, ("context ID %#x\n", paParms[0].u.uint32));
    RTTESTI_CHECK_MSG(paParms[1].u.uint32 == TST_VALUE_U32, ("32-bit value %#x\n", paParms[1].u.uint32));
    RTTESTI_CHECK_MSG(paParms[2].u.uint64 == TST_VALUE_U64, ("64-bit value %#RX64\n", paParms[2].u.uint64));
    RTTESTI_CHECK(memcmp(pchBuf, g_szTstData, sizeof(g_szTstData)) == 0);
    /* Nothing was written behind the data. */
    RTTESTI_CHECK(tstIsBufUntouched(pchBuf + sizeof(g_szTstData), cbBuf - sizeof(g_szTstData)));
}

/**
 * Messages of the host which the service has to refuse, and one it takes.
 *
 * @param   pTable      The service table.
 */
static void testHost(const VBOXHGCMSVCFNTABLE *pTable)
{
    RTTestISub("Messages of the host");

    VBOXHGCMSVCPARM aParms[2];
    HGCMSvcSetU64(&aParms[0], VBOX_GUESTCTRL_DST_ROOT_SVC | TST_CONTEXT_ID);
    HGCMSvcSetU32(&aParms[1], TST_VALUE_U32);

    /* No client is connected: there is nobody the message could go to. */
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, RT_ELEMENTS(aParms), aParms), VERR_NOT_FOUND);

    void *pvClient = tstClientConnect(pTable);
    if (!pvClient)
        return;

    /* A number of parameters which is out of range, with parameters and without. */
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, 0, NULL), VERR_WRONG_PARAMETER_COUNT);
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, 0, aParms), VERR_WRONG_PARAMETER_COUNT);
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, VMMDEV_MAX_HGCM_PARMS, NULL),
                     VERR_WRONG_PARAMETER_COUNT);
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, UINT32_MAX, NULL), VERR_WRONG_PARAMETER_COUNT);

    /* No parameters where there have to be some. */
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, 1, NULL), VERR_INVALID_POINTER);

    /* A first parameter which is no context ID. */
    VBOXHGCMSVCPARM aParmsBad[2];
    HGCMSvcSetPv(&aParmsBad[0], g_szTstData, sizeof(g_szTstData));
    HGCMSvcSetU32(&aParmsBad[1], TST_VALUE_U32);
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, RT_ELEMENTS(aParmsBad), aParmsBad),
                     VERR_WRONG_PARAMETER_TYPE);

    /* A context ID which names no destination. */
    HGCMSvcSetU64(&aParmsBad[0], TST_CONTEXT_ID);
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, RT_ELEMENTS(aParmsBad), aParmsBad),
                     VERR_INTERNAL_ERROR_3);

    /* A session as the destination which no client has. */
    HGCMSvcSetU64(&aParmsBad[0], VBOX_GUESTCTRL_DST_SESSION | TST_CONTEXT_ID);
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, RT_ELEMENTS(aParmsBad), aParmsBad),
                     VERR_NOT_FOUND);

    /* A parameter of a type which a message cannot have. */
    HGCMSvcSetU64(&aParmsBad[0], VBOX_GUESTCTRL_DST_ROOT_SVC | TST_CONTEXT_ID);
    aParmsBad[1].type = VBOX_HGCM_SVC_PARM_INVALID;
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, RT_ELEMENTS(aParmsBad), aParmsBad),
                     VERR_WRONG_PARAMETER_TYPE);

    /* None of these was queued. */
    VBOXHGCMSVCPARM aParmsPeek[2];
    HGCMSvcSetU32(&aParmsPeek[0], 0);
    HGCMSvcSetU32(&aParmsPeek[1], 0);
    RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_PEEK_NOWAIT, RT_ELEMENTS(aParmsPeek), aParmsPeek), VERR_TRY_AGAIN);

    /* A message as it has to be.  The client disconnects with it in its queue. */
    RTTESTI_CHECK_RC(pTable->pfnHostCall(pTable->pvService, HOST_MSG_EXEC_CMD, RT_ELEMENTS(aParms), aParms), VINF_SUCCESS);
    HGCMSvcSetU32(&aParmsPeek[0], 0);
    HGCMSvcSetU32(&aParmsPeek[1], 0);
    RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_PEEK_NOWAIT, RT_ELEMENTS(aParmsPeek), aParmsPeek), VINF_SUCCESS);
    RTTESTI_CHECK_MSG(aParmsPeek[0].u.uint32 == (uint32_t)HOST_MSG_EXEC_CMD, ("message %u\n", aParmsPeek[0].u.uint32));
    RTTESTI_CHECK_MSG(aParmsPeek[1].u.uint32 == RT_ELEMENTS(aParms), ("%u parameters\n", aParmsPeek[1].u.uint32));

    tstClientDisconnect(pTable, pvClient);
}

/**
 * A client fetches a message with GUEST_MSG_PEEK_NOWAIT and GUEST_MSG_GET,
 * and is refused when it does not ask for what is queued.
 *
 * @param   pTable      The service table.
 */
static void testClientGet(const VBOXHGCMSVCFNTABLE *pTable)
{
    RTTestISub("A client fetches a message");

    void *pvClient = tstClientConnect(pTable);
    if (!pvClient)
        return;

    VBOXHGCMSVCPARM aParms[TST_MSG_PARMS + 2];
    char            achBuf[32];
    do
    {
        /*
         * Nothing is queued.
         */
        HGCMSvcSetU32(&aParms[0], 0);
        HGCMSvcSetU32(&aParms[1], 0);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_PEEK_NOWAIT, 2, aParms), VERR_TRY_AGAIN);
        HGCMSvcSetU32(&aParms[0], 0);
        HGCMSvcSetU32(&aParms[1], 0);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_GET, 2, aParms), VERR_TRY_AGAIN);

        /*
         * The host queues a message, and the client asks what it is.
         */
        RTTESTI_CHECK_RC_BREAK(tstHostQueueMsg(pTable), VINF_SUCCESS);

        for (unsigned i = 0; i < RT_ELEMENTS(aParms); i++)
            HGCMSvcSetU32(&aParms[i], TST_UNTOUCHED_U32);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_PEEK_NOWAIT, RT_ELEMENTS(aParms), aParms), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(aParms[0].u.uint32 == (uint32_t)HOST_MSG_EXEC_CMD, ("message %u\n", aParms[0].u.uint32));
        RTTESTI_CHECK_MSG(aParms[1].u.uint32 == TST_MSG_PARMS, ("%u parameters\n", aParms[1].u.uint32));
        /* A value is told as the inverted size of its type, a buffer by its size. */
        RTTESTI_CHECK_MSG(aParms[2].u.uint32 == ~(uint32_t)sizeof(uint32_t), ("%#x\n", aParms[2].u.uint32));
        RTTESTI_CHECK_MSG(aParms[3].u.uint32 == ~(uint32_t)sizeof(uint32_t), ("%#x\n", aParms[3].u.uint32));
        RTTESTI_CHECK_MSG(aParms[4].u.uint32 == ~(uint32_t)sizeof(uint64_t), ("%#x\n", aParms[4].u.uint32));
        RTTESTI_CHECK_MSG(aParms[5].u.uint32 == sizeof(g_szTstData), ("%#x\n", aParms[5].u.uint32));

        /*
         * The client expects another message than the one which is queued.
         */
        tstSetFetchParms(aParms, achBuf, sizeof(achBuf));
        HGCMSvcSetU32(&aParms[0], (uint32_t)HOST_MSG_EXEC_CMD + 1);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_GET, TST_MSG_PARMS, aParms), VERR_MISMATCH);
        RTTESTI_CHECK(aParms[1].u.uint32 == TST_UNTOUCHED_U32);
        RTTESTI_CHECK(aParms[2].u.uint64 == TST_UNTOUCHED_U64);
        RTTESTI_CHECK(tstIsBufUntouched(achBuf, sizeof(achBuf)));

        /*
         * The client has one parameter less than the message, and one more.
         */
        tstSetFetchParms(aParms, achBuf, sizeof(achBuf));
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_GET, TST_MSG_PARMS - 1, aParms), VERR_WRONG_PARAMETER_COUNT);
        RTTESTI_CHECK(aParms[1].u.uint32 == TST_UNTOUCHED_U32);
        RTTESTI_CHECK(aParms[2].u.uint64 == TST_UNTOUCHED_U64);

        tstSetFetchParms(aParms, achBuf, sizeof(achBuf));
        HGCMSvcSetU32(&aParms[TST_MSG_PARMS], TST_UNTOUCHED_U32);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_GET, TST_MSG_PARMS + 1, aParms), VERR_WRONG_PARAMETER_COUNT);
        RTTESTI_CHECK(aParms[1].u.uint32 == TST_UNTOUCHED_U32);
        RTTESTI_CHECK(aParms[2].u.uint64 == TST_UNTOUCHED_U64);
        RTTESTI_CHECK(tstIsBufUntouched(achBuf, sizeof(achBuf)));

        /*
         * One parameter of the client is of another type than the one of the
         * message, for each of the parameters after the first in turn.  The
         * client gets nothing then, also not the parameters before that one.
         */
        for (unsigned iBad = 1; iBad < TST_MSG_PARMS; iBad++)
        {
            tstSetFetchParms(aParms, achBuf, sizeof(achBuf));
            if (iBad == 1)
                HGCMSvcSetU64(&aParms[iBad], TST_UNTOUCHED_U64);
            else
                HGCMSvcSetU32(&aParms[iBad], TST_UNTOUCHED_U32);
            int rc = tstClientCall(pTable, pvClient, GUEST_MSG_GET, TST_MSG_PARMS, aParms);
            RTTESTI_CHECK_MSG(rc == VERR_WRONG_PARAMETER_TYPE, ("parameter %u: %Rrc\n", iBad, rc));
            RTTESTI_CHECK_MSG(aParms[0].u.uint32 == (uint32_t)HOST_MSG_EXEC_CMD, ("parameter %u\n", iBad));
            if (iBad == 1)
                RTTESTI_CHECK_MSG(aParms[1].u.uint64 == TST_UNTOUCHED_U64, ("parameter %u\n", iBad));
            else
                RTTESTI_CHECK_MSG(aParms[1].u.uint32 == TST_UNTOUCHED_U32, ("parameter %u\n", iBad));
            if (iBad == 2)
                RTTESTI_CHECK_MSG(aParms[2].u.uint32 == TST_UNTOUCHED_U32, ("parameter %u\n", iBad));
            else
                RTTESTI_CHECK_MSG(aParms[2].u.uint64 == TST_UNTOUCHED_U64, ("parameter %u\n", iBad));
            if (iBad == 3)
                RTTESTI_CHECK_MSG(aParms[3].u.uint32 == TST_UNTOUCHED_U32, ("parameter %u\n", iBad));
            RTTESTI_CHECK_MSG(tstIsBufUntouched(achBuf, sizeof(achBuf)), ("parameter %u\n", iBad));
        }

        /* The same with a first parameter which is no value. */
        tstSetFetchParms(aParms, achBuf, sizeof(achBuf));
        char achBuf0[8];
        HGCMSvcSetPv(&aParms[0], achBuf0, sizeof(achBuf0));
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_GET, TST_MSG_PARMS, aParms), VERR_WRONG_PARAMETER_TYPE);
        RTTESTI_CHECK(aParms[1].u.uint32 == TST_UNTOUCHED_U32);
        RTTESTI_CHECK(aParms[2].u.uint64 == TST_UNTOUCHED_U64);
        RTTESTI_CHECK(tstIsBufUntouched(achBuf, sizeof(achBuf)));

        /*
         * The buffer of the client is one byte too small: it is told the size
         * which is needed and nothing is written to the buffer.
         */
        tstSetFetchParms(aParms, achBuf, sizeof(g_szTstData) - 1);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_GET, TST_MSG_PARMS, aParms), VERR_BUFFER_OVERFLOW);
        RTTESTI_CHECK_MSG(aParms[3].u.pointer.size == sizeof(g_szTstData), ("size %#x\n", aParms[3].u.pointer.size));
        RTTESTI_CHECK(tstIsBufUntouched(achBuf, sizeof(g_szTstData) - 1));

        /*
         * After all of that the message is still the first in the queue.
         */
        HGCMSvcSetU32(&aParms[0], 0);
        HGCMSvcSetU32(&aParms[1], 0);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_PEEK_NOWAIT, 2, aParms), VINF_SUCCESS);
        RTTESTI_CHECK_MSG(aParms[0].u.uint32 == (uint32_t)HOST_MSG_EXEC_CMD, ("message %u\n", aParms[0].u.uint32));
        RTTESTI_CHECK_MSG(aParms[1].u.uint32 == TST_MSG_PARMS, ("%u parameters\n", aParms[1].u.uint32));

        /*
         * The client asks for what is queued and gets it.
         */
        tstSetFetchParms(aParms, achBuf, sizeof(achBuf));
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_GET, TST_MSG_PARMS, aParms), VINF_SUCCESS);
        tstCheckFetched(aParms, achBuf, sizeof(achBuf));
        RTTESTI_CHECK_MSG(aParms[3].u.pointer.size == sizeof(g_szTstData), ("size %#x\n", aParms[3].u.pointer.size));

        /*
         * The queue is empty again.
         */
        HGCMSvcSetU32(&aParms[0], 0);
        HGCMSvcSetU32(&aParms[1], 0);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_PEEK_NOWAIT, 2, aParms), VERR_TRY_AGAIN);
    } while (0);

    tstClientDisconnect(pTable, pvClient);
}

/**
 * A client fetches a message with GUEST_MSG_WAIT, the way guests did before
 * there was GUEST_MSG_PEEK_NOWAIT and GUEST_MSG_GET.
 *
 * @param   pTable      The service table.
 */
static void testClientWait(const VBOXHGCMSVCFNTABLE *pTable)
{
    RTTestISub("A client fetches a message the old way");

    void *pvClient = tstClientConnect(pTable);
    if (!pvClient)
        return;

    VBOXHGCMSVCPARM aParms[TST_MSG_PARMS];
    char            achBuf[32];
    do
    {
        /*
         * The message is queued first: a client which waits with nothing in
         * the queue is answered when a message comes, not now.
         */
        RTTESTI_CHECK_RC_BREAK(tstHostQueueMsg(pTable), VINF_SUCCESS);

        /*
         * The first call is told what the message is and how many parameters
         * it has, with the status which says that there is more to fetch.
         */
        HGCMSvcSetU32(&aParms[0], 0);
        HGCMSvcSetU32(&aParms[1], 0);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_WAIT, 2, aParms), VERR_TOO_MUCH_DATA);
        RTTESTI_CHECK_MSG(aParms[0].u.uint32 == (uint32_t)HOST_MSG_EXEC_CMD, ("message %u\n", aParms[0].u.uint32));
        RTTESTI_CHECK_MSG(aParms[1].u.uint32 == TST_MSG_PARMS, ("%u parameters\n", aParms[1].u.uint32));

        /*
         * The client has the right number of parameters and the last is of
         * another type than the one of the message.  It is told again what
         * the message is, in its first two parameters, and gets nothing of
         * the message: the parameter before the one which does not fit holds
         * what it held.
         */
        tstSetFetchParms(aParms, achBuf, sizeof(achBuf));
        HGCMSvcSetU32(&aParms[3], TST_UNTOUCHED_U32);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_WAIT, TST_MSG_PARMS, aParms), VERR_TOO_MUCH_DATA);
        RTTESTI_CHECK_MSG(aParms[0].u.uint32 == (uint32_t)HOST_MSG_EXEC_CMD, ("message %u\n", aParms[0].u.uint32));
        RTTESTI_CHECK_MSG(aParms[1].u.uint32 == TST_MSG_PARMS, ("%u parameters\n", aParms[1].u.uint32));
        RTTESTI_CHECK_MSG(aParms[2].u.uint64 == TST_UNTOUCHED_U64, ("64-bit value %#RX64\n", aParms[2].u.uint64));
        RTTESTI_CHECK_MSG(aParms[3].u.uint32 == TST_UNTOUCHED_U32, ("%#x\n", aParms[3].u.uint32));
        RTTESTI_CHECK(tstIsBufUntouched(achBuf, sizeof(achBuf)));

        /*
         * The client has the parameters of the message and gets it.
         */
        tstSetFetchParms(aParms, achBuf, sizeof(achBuf));
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_WAIT, TST_MSG_PARMS, aParms), VINF_SUCCESS);
        tstCheckFetched(aParms, achBuf, sizeof(achBuf));

        /*
         * The queue is empty after that.
         */
        HGCMSvcSetU32(&aParms[0], 0);
        HGCMSvcSetU32(&aParms[1], 0);
        RTTESTI_CHECK_RC(tstClientCall(pTable, pvClient, GUEST_MSG_PEEK_NOWAIT, 2, aParms), VERR_TRY_AGAIN);
    } while (0);

    tstClientDisconnect(pTable, pvClient);
}

/*
 * Set environment variable "IPRT_TEST_MAX_LEVEL=all" to get more debug output!
 */
int main()
{
    RTEXITCODE rcExit  = RTTestInitAndCreate("tstGuestControlSvc", &g_hTest);
    if (rcExit != RTEXITCODE_SUCCESS)
        return rcExit;
    RTTestBanner(g_hTest);

    /* The service is handed what it has to refuse; in a build with assertions
       it asserts on that before it does. */
    RTAssertSetMayPanic(false);
    RTAssertSetQuiet(true);

    /* Some host info. */
    RTTestIPrintf(RTTESTLVL_ALWAYS, "sizeof(void*)=%d\n", sizeof(void*));

    /* Do the tests, each with a service of its own. */
    VBOXHGCMSVCFNTABLE svcTable;
    VBOXHGCMSVCHELPERS svcHelpers;
    RTTEST_CHECK_RC_RET(g_hTest, initTable(&svcTable, &svcHelpers), VINF_SUCCESS, 1);

    typedef void FNTEST(const VBOXHGCMSVCFNTABLE *pTable);
    static FNTEST * const s_apfnTests[] =
    {
        testHost,
        testClientGet,
        testClientWait
    };
    for (unsigned i = 0; i < RT_ELEMENTS(s_apfnTests); i++)
    {
        RTTESTI_CHECK_RC_BREAK(VBoxHGCMSvcLoad(&svcTable), VINF_SUCCESS);
        s_apfnTests[i](&svcTable);
        RTTESTI_CHECK_RC_BREAK(svcTable.pfnUnload(svcTable.pvService), VINF_SUCCESS);
    }

    return RTTestSummaryAndDestroy(g_hTest);
}
