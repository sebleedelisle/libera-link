// -------------------------------------------------------------------------------------------------
//  File IDNServer.cpp
//
//  Copyright (c) 2020-2025 DexLogic, Dirk Apitz. All Rights Reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//  SOFTWARE.
//
// -------------------------------------------------------------------------------------------------
//  Change History:
//
//  07/2017 Dirk Apitz, created
//  01/2024 Dirk Apitz, modifications and integration into OpenIDN
//  04/2025 Dirk Apitz, independence from network layer through derived classes (Linux/LwIP support)
// -------------------------------------------------------------------------------------------------


// Standard libraries
#include <string.h>
#include <stdio.h>

// Project headers
#include "idn.h"
#include "idn-stream.h"
#include "../shared/ODFTools.hpp"
#include "PEVFlags.h"
#if !defined(OPENIDN_BRIDGE_MODE)
#define OPENIDN_BRIDGE_MODE 0
#endif
#if !OPENIDN_BRIDGE_MODE
#include "../hardware/Helios/HeliosAdapter.hpp"
#endif

// Module header
#include "IDNServer.hpp"



// -------------------------------------------------------------------------------------------------
//  Defines
// -------------------------------------------------------------------------------------------------

#define LINK_TIMEOUT_US                     1000000     // Connection/Link inactivity (microseconds)
#define SESSION_TIMEOUT_US                  1000000     // Session inactivity (microseconds)

#define SESSIONSTATE_ATTACHED               1           // Attached to a connection
#define SESSIONSTATE_ABANDONED              0           // No more in use, to be deleted
#define SESSIONSTATE_CLOSING                -1          // No more in use, waiting for output to finish
#define SESSIONSTATE_DETACHED               -2          // In use but not attached to a connection


// Copy a uint8 array field into a uint8 array field, stop at '\0' and pad dst with '\0'
#define STRCPY_FIELD_FROM_FIELD(dstPtr, dstSize, srcPtr, srcSize)                           \
    {                                                                                       \
        unsigned cpyCount = dstSize;                                                        \
        if(srcSize < cpyCount) cpyCount = srcSize;                                          \
                                                                                            \
        unsigned i = 0;                                                                     \
        uint8_t *dst = dstPtr;                                                              \
        uint8_t *src = srcPtr;                                                              \
        for(; (i < cpyCount) && (*src != 0); i++) *dst++ = *src++;                          \
        for(; i < dstSize; i++) *dst = 0;                                                   \
    }

// Copy a unitID field
#define UNITID_FIELD_FROM_FIELD(dstPtr, dstSize, srcPtr, srcSize)                           \
    {                                                                                       \
        uint8_t *dst = dstPtr;                                                              \
        uint8_t *src = srcPtr;                                                              \
                                                                                            \
        unsigned idLen = src ? *src++ : 0;                                                  \
        if(srcSize > 0) { if(idLen >= srcSize) idLen = srcSize - 1; } else idLen = 0;       \
        if(dstSize > 0) { if(idLen >= dstSize) idLen = dstSize - 1; } else idLen = 0;       \
                                                                                            \
        if(dst) *dst++ = idLen;                                                             \
        if(dst && (idLen > 0)) memcpy(dst, src, idLen);                                     \
                                                                                            \
        unsigned trailCount = (dstSize > (1 + idLen)) ? (dstSize - (1 + idLen)) : 0;        \
        if(dst && (trailCount > 0)) memset(&dst[idLen], 0, trailCount);                     \
    }



// =================================================================================================
//  Class ODFSession
//
// -------------------------------------------------------------------------------------------------
//  scope: protected
// -------------------------------------------------------------------------------------------------

void ODFSession::openChannel(ODF_ENV *env, IDN_CHANNEL *channel)
{
    Inherited::openChannel(env, channel);

    // Initialize to 'routed' (discard previous flags) -- may be in error though
    pipelineEvents[channel->channelID] = IDN_PEVFLG_CHANNEL_ROUTED;
}


void ODFSession::closeChannel(ODF_ENV *env, IDN_CHANNEL *channel, bool abortFlag)
{
    Inherited::closeChannel(env, channel, abortFlag);

    // Finalize to 'closed' (discard previous flags)
    pipelineEvents[channel->channelID] = IDN_PEVFLG_CHANNEL_CLOSED;
}


SERVICE_HANDLE ODFSession::requestService(ODF_ENV *env, uint8_t channelID, uint8_t serviceID, uint8_t serviceMode)
{
    TracePrinter tpr(env, "ODFSession~requestService");

    // IDN session <-> Server nomenklature: serviceHandle <-> IDNService // conduitHandle <-> IDNInlet
    IDNService *service = (IDNService *)0;

    // Find service for serviceID
    if(serviceID != 0) service = idnServer->getService(serviceID);
    else service = idnServer->getDefaultService(serviceMode);

    // Check for valid service
    if(service == (IDNService *)0)
    {
        // Invalid serviceID or serviceMode
        pipelineEvents[channelID] |= IDN_PEVFLG_CHANNEL_SMERR;

        tpr.logWarn("%s|Attach<%u>: Muted, no service, serviceID=%u, serviceMode=%u", 
                    getLogIdent(), channelID, serviceID, serviceMode);

        return (SERVICE_HANDLE)0;
    }

    // Return the pointer to the service object as service handle
    return (SERVICE_HANDLE)service;
}


void ODFSession::releaseService(ODF_ENV *env, uint8_t channelID, SERVICE_HANDLE serviceHnd)
{
    TracePrinter tpr(env, "ODFSession~releaseService");

    // IDN session <-> Server nomenklature: serviceHandle <-> IDNService // conduitHandle <-> IDNInlet
    IDNService *service = (IDNService *)serviceHnd;

    // Check for a valid service object (unlikely, invalid invocation)
    if(service == (IDNService *)0)
    {
        tpr.logError("%s|Detach<%u>: Invalid service", getLogIdent(), channelID);
        return;
    }

    // Nothing to do since the service list is static
}


CONDUIT_HANDLE ODFSession::requestConduit(ODF_ENV *env, uint8_t channelID, SERVICE_HANDLE serviceHnd, uint8_t serviceMode)
{
    TracePrinter tpr(env, "ODFSession~requestConduit");

    // IDN session <-> Server nomenklature: serviceHandle <-> IDNService // conduitHandle <-> IDNInlet
    IDNService *service = (IDNService *)serviceHnd;
    IDNInlet *inlet = (IDNInlet *)0;

    // Check for a valid service object (unlikely, invalid invocation)
    if(service == (IDNService *)0)
    {
        tpr.logError("%s|Attach<%u>: Invalid service", getLogIdent(), channelID);
        return (CONDUIT_HANDLE)0;
    }

    // Check for service mode handling
    if(service->handlesMode(serviceMode) == false)
    {
        // Invalid serviceID or serviceMode
        pipelineEvents[channelID] |= IDN_PEVFLG_CHANNEL_SMERR;

        tpr.logWarn("%s|Attach<%u>: Invalid mode for service, serviceMode=%u", getLogIdent(), channelID, serviceMode);

        return (CONDUIT_HANDLE)0;
    }

    // Request inlet from the service for the mode
    inlet = service->requestInlet(env, serviceMode);
    if(inlet == (IDNInlet *)0)
    {
        // Service or mode busy
        pipelineEvents[channelID] |= IDN_PEVFLG_CHANNEL_BSYERR;

        tpr.logWarn("%s|Attach<%u>: Failed to request inlet, serviceMode=%u", getLogIdent(), channelID, serviceMode);

        return (CONDUIT_HANDLE)0;
    }

    // Return the pointer to the service inlet object as conduit handle
    return (CONDUIT_HANDLE)inlet;
}


void ODFSession::releaseConduit(ODF_ENV *env, uint8_t channelID, SERVICE_HANDLE serviceHnd, CONDUIT_HANDLE conduitHnd, bool abortFlag)
{
    TracePrinter tpr(env, "ODFSession~releaseConduit");

    // IDN session <-> Server nomenklature: serviceHandle <-> IDNService // conduitHandle <-> IDNInlet
    IDNService *service = (IDNService *)serviceHnd;
    IDNInlet *inlet = (IDNInlet *)conduitHnd;

    // Check for a valid service object (unlikely, invalid invocation)
    if(service == (IDNService *)0)
    {
        tpr.logError("%s|Detach<%u>: Invalid service", getLogIdent(), channelID);
        return;
    }

    // Check for a valid inlet object (unlikely, invalid invocation)
    if(inlet == (IDNInlet *)0)
    {
        tpr.logError("%s|Detach<%u>: Invalid inlet", getLogIdent(), channelID);
        return;
    }

    // Release the inlet
    service->releaseInlet(env, inlet);
}


void ODFSession::input(ODF_ENV *env, SERVICE_HANDLE serviceHnd, CONDUIT_HANDLE conduitHnd, ODF_TAXI_BUFFER *taxiBuffer)
{
    //IDNService *service = (IDNService *)serviceHnd;
    IDNInlet *inlet = (IDNInlet *)conduitHnd;

    if(inlet != (IDNInlet *)0)
    {
        inlet->process(env, taxiBuffer);
    }
    else
    {
        taxiBuffer->discard();
    }
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

ODFSession::ODFSession(char *logIdent, IDNServer *idnServer): IDNSession(logIdent)
{
    this->idnServer = idnServer;

    // Session must have been created by a connection
    sessionState = SESSIONSTATE_ATTACHED;

    // No input yet
    inputTimeValid = false;
    inputTimeUS = 0;

    // Set all channels closed
    for(unsigned channelID = 0; channelID < IDNVAL_CHANNEL_COUNT; channelID++)
    {
        pipelineEvents[channelID] = IDN_PEVFLG_CHANNEL_CLOSED;
    }
}


ODFSession::~ODFSession()
{
}


uint16_t ODFSession::clearPipelineEvents(unsigned channelID)
{
    if(channelID >= IDNVAL_CHANNEL_COUNT) return 0;

    // Get current channel/routing event flags
    uint16_t result = pipelineEvents[channelID];

    // Get current Processing/Device event flags
    IDNInlet *inlet = (IDNInlet *)(channels[channelID].conduitHnd);
    if(inlet != (IDNInlet *)0) result |= inlet->clearPipelineEvents();

    // Reset events. Note: 'closed' flag is sticky
    pipelineEvents[channelID] &= IDN_PEVFLG_CHANNEL_CLOSED;

    return result;
}


void ODFSession::cancelGracefully(ODF_ENV *env)
{
    // Graceful session stop: Process buffered messages, finish active output
    // ---------------------------------------------------------------------------------------------

    TracePrinter tpr(env, "ODFSession~cancelGracefully");

    do
    {

// FIXME: Code for gracefully flushing input and closing services should go here
// In case there is a backlog, the close of the session should be delayed and the
// session cleaned up with the timeout handling

    }
    while(0);

    const char *danglingNote = "";
    if(hasOpenChannels()) danglingNote = ", dangling channels";
    tpr.logDebug("%s|Session: Regular close%s", getLogIdent(), danglingNote);

    // No gracefully closing sink inputs (or error). Abandon (still) open inputs/channels.
    reset(env);

    // No more in use, to be removed/deleted
    sessionState = SESSIONSTATE_ABANDONED;
}


void ODFSession::cancelImmediately(ODF_ENV *env)
{
    // Immediate session stop: Discard buffered messages, abort output
    // ---------------------------------------------------------------------------------------------

    TracePrinter tpr(env, "ODFSession~cancelImmediately");

    tpr.logDebug("%s|Session: Immediate close", getLogIdent());

    // Abandon/Close all inputs/channels
    reset(env);

    // No more in use, to be removed/deleted
    sessionState = SESSIONSTATE_ABANDONED;
}


bool ODFSession::checkTeardown(ODF_ENV *env, uint32_t envTimeUS)
{
    TracePrinter tpr(env, "ODFSession~checkTeardown");

    // Check for already being abandoned
    if(sessionState == SESSIONSTATE_ABANDONED)
    {
        return true;
    }

    if(sessionState == SESSIONSTATE_CLOSING)
    {

// FIXME: Periodic cleanup - invoke closing processing on services for
// Check for timeout and eventually reset

    }

    // In case session had input - check for timeout
    if(inputTimeValid == true)
    {
        int32_t usDiffTime = (int32_t)((uint32_t)envTimeUS - (uint32_t)inputTimeUS);
        if(usDiffTime > SESSION_TIMEOUT_US)
        {
            // Diagnostics
            tpr.logDebug("%s|Session: Inactivity timeout", getLogIdent());

            // Abandon/Close all inputs/channels, invalidate input time
            reset(env);
        }
    }

    // Re-Check session input after timeout handling
    // Note: No further action (just reset / close channels) in case SESSIONSTATE_ATTACHED
    if(inputTimeValid == false)
    {
        // No input, not attached to a connection: Remove/Delete session
        if(sessionState == SESSIONSTATE_DETACHED) return true;
    }

    // Keep session
    return false;
}


void ODFSession::reset(ODF_ENV *env)
{
    Inherited::reset(env);

    inputTimeValid = false;
}


int ODFSession::processChannelMessage(ODF_ENV *env, ODF_TAXI_BUFFER *taxiBuffer)
{
    // Update input time with buffer reception time
    inputTimeUS = taxiBuffer->getSourceRefTime();
    inputTimeValid = true;


// FIXME: Latency queue, timestamp sequence check / discontinuity / overrun/underrun/recovery


    // Pass message to base class
    return Inherited::processChannelMessage(env, taxiBuffer);
}



// =================================================================================================
//  Class IDNHelloConnection
//
// -------------------------------------------------------------------------------------------------
//  scope: protected
// -------------------------------------------------------------------------------------------------

void IDNHelloConnection::cleanupClose(ODF_ENV *env)
{
    if(session != (ODFSession *)0)
    {
        // Graceful session stop - flush data, close channels
        session->cancelGracefully(env);
        session = (ODFSession *)0;
    }
}


void IDNHelloConnection::cleanupAbort(ODF_ENV *env)
{
    if(session != (ODFSession *)0)
    {
        // Immediate session stop - flush data, close channels
        session->cancelImmediately(env);
        session = (ODFSession *)0;
    }
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

IDNHelloConnection::IDNHelloConnection(uint8_t clientGroup, char *logIdent)
{
    // Copy client group (as part of the address)
    this->clientGroup = clientGroup;

    // Populate link identification (for log/diagnostics)
    if((logIdent != (char *)0) && (*logIdent != '\0'))
    {
        snprintf(this->logIdent, sizeof(this->logIdent), "%s", logIdent);
    }
    else
    {
        snprintf(this->logIdent, sizeof(this->logIdent), "(N/A)");
    }

    // Note: Attribute inputTimeUS initialized with the first invocation of updateInputTime()
    inputTimeUS = 0;

    session = (ODFSession *)0;

    inputEvents = IDNVAL_RTACK_IEVFLG_NEW;
    sequenceValid = 0;
    nextSequence = 0;
    errCntSequence = 0;
}


IDNHelloConnection::~IDNHelloConnection()
{
}


int IDNHelloConnection::clientMatchIDNHello(RECV_COOKIE *cookie, uint8_t clientGroup)
{
    if(clientGroup != this->clientGroup) return 1;

    // Success
    return 0;
}


void IDNHelloConnection::handleLinkClose(ODF_ENV *env)
{
    // Handle a IDN-RT connection close request
    // ---------------------------------------------------------------------------------------------

    TracePrinter tpr(env, "IDNHelloConnection~handleLinkClose");

    tpr.logInfo("%s|Link: Connection close", getLogIdent());

    cleanupClose(env);
}


void IDNHelloConnection::handleTimeout(ODF_ENV *env, int32_t usDiffTime)
{
    // Handle a connection timeout
    // ---------------------------------------------------------------------------------------------

    TracePrinter tpr(env, "IDNHelloConnection~handleTimeout");

    tpr.logInfo("%s|Link: Connection timeout, dt=%d", getLogIdent(), usDiffTime);

    cleanupAbort(env);
}


void IDNHelloConnection::handleAbort(ODF_ENV *env)
{
    // Handle abandoning the connection due to state change
    // ---------------------------------------------------------------------------------------------

    TracePrinter tpr(env, "IDNHelloConnection~handleAbort");

    tpr.logInfo("%s|Link: Connection stop", getLogIdent());

    cleanupAbort(env);
}


bool IDNHelloConnection::checkTeardown(ODF_ENV *env, uint32_t envTimeUS)
{
    // Check for reception timeout.
    int32_t usDiffTime = (int32_t)((uint32_t)envTimeUS - (uint32_t)inputTimeUS);
    if(usDiffTime > LINK_TIMEOUT_US)
    {
        // Connection timeout: Forward to session depending on connection type
        handleTimeout(env, usDiffTime);

        // Delete connection
        return true;
    }

    // Keep connection
    return false;
}



// =================================================================================================
//  Class IDNServer
//
// -------------------------------------------------------------------------------------------------
//  scope: private
// -------------------------------------------------------------------------------------------------

void IDNServer::processStreamMessage(ODF_ENV *env, IDNHelloConnection *connection, ODF_TAXI_BUFFER *taxiBuffer)
{
    TracePrinter tpr(env, "IDNServer~processStreamMessage");

    do
    {
// FIXME: Check/Modify buffer ref (->latency queue)

        // Get IDN channel message header. Note: IDNHDR_CHANNEL_MESSAGE coalesce checked earlier
        IDNHDR_CHANNEL_MESSAGE *channelMessageHdr = (IDNHDR_CHANNEL_MESSAGE *)taxiBuffer->getPayloadPtr();

        // Check message length
        uint16_t totalSize = btoh16(channelMessageHdr->totalSize);
        if(totalSize != taxiBuffer->getTotalLen())
        {
            connection->inputEvents |= IDNVAL_RTACK_IEVFLG_MVERR;

            tpr.logWarn("%s|IDN-Stream: Payload length (%u) / message size mismatch (%u)",
                        connection->getLogIdent(), taxiBuffer->getTotalLen(), totalSize);
            break;
        }

        // Check contentID (expect channel message)
        uint16_t contentID = btoh16(channelMessageHdr->contentID);
        if((contentID & IDNFLG_CONTENTID_CHANNELMSG) == 0)
        {
            connection->inputEvents |= IDNVAL_RTACK_IEVFLG_MVERR;

            tpr.logWarn("%s|IDN-Stream: Message type mismatch (expected channel message)",
                        connection->getLogIdent());
            break;
        }

        // Check for channel configuration
        unsigned char chunkType = (contentID & IDNMSK_CONTENTID_CNKTYPE);
        if((chunkType <= 0xBF) && (contentID & IDNFLG_CONTENTID_CONFIG_LSTFRG))
        {
            // Message (entire chunk or first chunk fragment) contains channel configuration

            // Ensure channel configuration in first buffer (coalesce in case scattered)
            if(taxiBuffer->coalesce(sizeof(IDNHDR_CHANNEL_MESSAGE) + sizeof(IDNHDR_CHANNEL_CONFIG)) < 0)
            {
                connection->inputEvents |= IDNVAL_RTACK_IEVFLG_IRAERR;
                break;
            }
            IDNHDR_CHANNEL_CONFIG *configHdr = (IDNHDR_CHANNEL_CONFIG *)&channelMessageHdr[1];

            // Ensure configuration words in first buffer (coalesce in case scattered)
            unsigned configLen = (unsigned)(configHdr->wordCount) * 4;
            if(taxiBuffer->coalesce(sizeof(IDNHDR_CHANNEL_MESSAGE) + sizeof(IDNHDR_CHANNEL_CONFIG) + configLen) < 0)
            {
                connection->inputEvents |= IDNVAL_RTACK_IEVFLG_IRAERR;
                break;
            }
        }

        // Pass the channel message. Note: IDNHDR_CHANNEL_MESSAGE coalesce checked earlier
        ODFSession *session = connection->getSession();
        if(session != (ODFSession *)0)
        {
            // Pass to IDN session for open/connect/close and sink forwarding. Discard in case of error
            int rcSession = session->processChannelMessage(env, taxiBuffer);
            if(rcSession < 0) taxiBuffer->discard();

            // No access to the buffer hereafter !!!
            taxiBuffer = (ODF_TAXI_BUFFER *)0;
        }
    }
    while(0);

    // In case the taxi buffer was not passed - free memory. No more access!
    if(taxiBuffer != (ODF_TAXI_BUFFER *)0) taxiBuffer->discard();
}


void IDNServer::processRtPacket(ODF_ENV *env, IDNHelloConnection *connection, ODF_TAXI_BUFFER *taxiBuffer)
{
    TracePrinter tpr(env, "IDNServer~processRtPacket");

    // Get packet header and sequence number
    IDNHDR_PACKET *recvPacketHdr = (IDNHDR_PACKET *)taxiBuffer->getPayloadPtr();
    unsigned short recvSequence = btoh16(recvPacketHdr->sequence);

    // Check sequence number
    if(connection->sequenceValid == false)
    {
        // Sequence init
        connection->sequenceValid = true;
        connection->nextSequence = recvSequence + 1;
    }
    else
    {
        // FIXME: Find/Drop duplicates!!!
        // FIXME: Find/Report missing!!!

        // Check for strict monotonic increment
        short seqDiff = (short)((unsigned short)recvSequence - (unsigned short)connection->nextSequence);
        if(seqDiff != 0)
        {
            // Store sequence error
            connection->inputEvents |= IDNVAL_RTACK_IEVFLG_SEQERR_TYPE1;
            if(connection->errCntSequence != (unsigned)-1) connection->errCntSequence++;

            if(seqDiff > 0)
            {
                // Lost messages - possibly either in slow receive or slow processing
                tpr.logWarn("%s|IDN-RT: Out of sequence (%d lost), expected %04X, got %04X",
                            connection->getLogIdent(), seqDiff, connection->nextSequence, recvSequence);
            }
            else
            {
                // Message sequence messed up
                tpr.logWarn("%s|IDN-RT: Out of sequence (%d early), expected %04X, got %04X",
                           connection->getLogIdent(), -seqDiff, connection->nextSequence, recvSequence);
            }
        }

        // Assumption for the next sequence number
        connection->nextSequence = recvSequence + 1;
    }

    // Unwarp/Process stream message
    if(taxiBuffer->getTotalLen() <= sizeof(IDNHDR_PACKET))
    {
        // In case the packet doesn't contain a message - do not pass to the session
        taxiBuffer->discard();
    }
    else
    {
        // Adjust buffer (drop IDN-RT packet header)
        taxiBuffer->adjustFront(-1 * (int)sizeof(IDNHDR_PACKET));

        // Pass the stream message to the session input
        processStreamMessage(env, connection, taxiBuffer);
    }
}


void IDNServer::destroyConnection(IDNHelloConnection *connection)
{
    connection->LLNode<ConnectionNode>::linkout();
    delete connection;
}


void IDNServer::destroySession(ODFSession *session)
{
    session->LLNode<SessionNode>::linkout();
    delete session;
}


IDNHelloConnection *IDNServer::findConnection(ODF_ENV *env, RECV_COOKIE *cookie, uint8_t clientGroup)
{
    TracePrinter tpr(env, "IDNServer~findConnection");

    // Find existing connection
    for(LLNode<ConnectionNode> *node = firstConnection; node != (LLNode<ConnectionNode> *)0; node = node->getNextNode())
    {
        IDNHelloConnection *connection = static_cast<IDNHelloConnection *>(node);

        int rcCmp = connection->clientMatchIDNHello(cookie, clientGroup);
        if(rcCmp == 0)
        {
            // Found / Known
            return connection;
        }
        else if(rcCmp < 0)
        {
            // Match function failed
            tpr.logError("Socket address compare failed");
        }
    }

    return (IDNHelloConnection *)0;
}


int IDNServer::processRtConnection(ODF_ENV *env, RECV_COOKIE *cookie, IDNHDR_RT_ACKNOWLEDGE *ackRspHdr, int cmd, ODF_TAXI_BUFFER *taxiBuffer)
{
    TracePrinter tpr(env, "IDNServer~processRtConnection");

    // Note: Assert packetlen >= sizeof(IDNHDR_PACKET) --- already checked, would not be here

    // Assume success
    int8_t result = IDNVAL_RTACK_SUCCESS;

    // -----------------------------------------------------

    // Find/Create the connection for the client
    IDNHelloConnection *connection = (IDNHelloConnection *)0;
    do
    {
        // Find existing connection
        uint8_t clientGroup = ((IDNHDR_PACKET *)taxiBuffer->getPayloadPtr())->flags & IDNMSK_PKTFLAGS_GROUP;
        connection = findConnection(env, cookie, clientGroup);
        if(connection != (IDNHelloConnection *)0)
        {
            // Connection exists
            break;
        }

        // Check for empty close command from unconnected client
        if((cmd == IDNCMD_RT_CNLMSG_CLOSE) && (taxiBuffer->getTotalLen() == sizeof(IDNHDR_PACKET)))
        {
            // Discard with error
            result = IDNVAL_RTACK_ERR_NOT_CONNECTED;
            break;
        }

        // Message command or close command with valid channel message payload - create/open connection
        connection = createConnection(cookie, clientGroup, cookie->diagString);
        if(connection == (IDNHelloConnection *)0)
        {
            // No memory or all connections occupied - discard with error
            tpr.logError("Connection object creation failed (IDNHelloConnection)");
            result = IDNVAL_RTACK_ERR_OCCUPIED;
            break;
        }

        // Add to the list of connections. Note: Receive time initialized right before packet processing
        connection->LLNode<ConnectionNode>::linkin(&firstConnection);

        // -----------------------------------------------------------------------------------------

        // Create/Open a session. Note: IDN-RT has a 1:1 correspondence of connection/session
        ODFSession *session = createSession(cookie->diagString, this);
        if(session != (ODFSession *)0)
        {
            // Add to the list of sessions
            session->LLNode<SessionNode>::linkin(&firstSession);
            connection->setSession(session);
        }

        // Diagnostic log
        tpr.logInfo("%s|Link: Connection open, session=%p", connection->getLogIdent(), session);
    }
    while(0);

    // In case of error (no connection): Discard the taxi buffer and return
    if(result != IDNVAL_RTACK_SUCCESS)
    {
        taxiBuffer->discard();
        return result;
    }

    // -----------------------------------------------------

    // Check message payload
    if(taxiBuffer->getTotalLen() == sizeof(IDNHDR_PACKET))
    {
        // OK, empty packet
    }
    else if(taxiBuffer->getTotalLen() < (sizeof(IDNHDR_PACKET) + sizeof(IDNHDR_CHANNEL_MESSAGE)))
    {
        // Payload must not be less than a channel message header
        result = IDNVAL_RTACK_ERR_PAYLOAD;
    }
    else if(taxiBuffer->coalesce(sizeof(IDNHDR_PACKET) + sizeof(IDNHDR_CHANNEL_MESSAGE)) < 0)
    {
        // Failed to coalesce headers - should not happen.
        result = IDNVAL_RTACK_ERR_GENERIC;
    }
    else
    {
        IDNHDR_PACKET *recvPacketHdr = (IDNHDR_PACKET *)taxiBuffer->getPayloadPtr();
        IDNHDR_MESSAGE *messageHdr = (IDNHDR_MESSAGE *)&recvPacketHdr[1];
        if(taxiBuffer->getTotalLen() < (sizeof(IDNHDR_PACKET) + btoh16(messageHdr->totalSize)))
        {
            // Datagram length does not match total message length
            result = IDNVAL_RTACK_ERR_PAYLOAD;
        }
    }

    // Prefetch channel message channelID (after packet input, header is not valid any more)
    int channelID = -1;
    if(result == IDNVAL_RTACK_SUCCESS)
    {
        IDNHDR_PACKET *recvPacketHdr = (IDNHDR_PACKET *)taxiBuffer->getPayloadPtr();
        IDNHDR_CHANNEL_MESSAGE *channelMessageHdr = (IDNHDR_CHANNEL_MESSAGE *)&recvPacketHdr[1];
        uint16_t contentID = btoh16(channelMessageHdr->contentID);
        channelID = (contentID & IDNMSK_CONTENTID_CHANNELID) >> 8;
    }
    else
    {
        // Message payload error: Discard payload (but process packet for sequence checks)
        taxiBuffer->cropPayload(sizeof(IDNHDR_PACKET));
    }

    // Update connection timeout, check/pass packet. Note: No access to the buffer hereafter !!!
    connection->updateInputTime(taxiBuffer->getSourceRefTime());
    processRtPacket(env, connection, taxiBuffer);
    taxiBuffer = (ODF_TAXI_BUFFER *)0;

    // ---------------------------------------------------------------------------------------------

    // For acknowledgements: Get/Clear event flags since last acknowledge
    if(ackRspHdr != (IDNHDR_RT_ACKNOWLEDGE *)0)
    {
        ODFSession *session = (ODFSession *)connection->getSession();

        // -------------------

        // Get the link events
        uint16_t inputFlags = connection->inputEvents;
        connection->inputEvents = 0;
/*
        // Get the session input events
        if(session != (ODFSession *)0)
        {
            inputFlags |= session->clearInputEvents();
        }
*/
        ackRspHdr->inputEventFlags = htob16(inputFlags);

        // -------------------

        // Get the pipeline event flags
        uint16_t plFlagsSrc = 0;
        if((session != (ODFSession *)0) && (channelID >= 0))
        {
            plFlagsSrc = session->clearPipelineEvents(channelID);
        }

        // Translate to IDN-RT acknowledge values
        uint16_t plFlagsDst = 0;

        // Channel routing events
        if(plFlagsSrc & IDN_PEVFLG_CHANNEL_ROUTED) plFlagsDst |= IDNVAL_RTACK_PEVFLG_ROUTED;
        if(plFlagsSrc & IDN_PEVFLG_CHANNEL_CLOSED) plFlagsDst |= IDNVAL_RTACK_PEVFLG_CLOSED;
        if(plFlagsSrc & IDN_PEVFLG_CHANNEL_SMERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_SMERR;
        if(plFlagsSrc & IDN_PEVFLG_CHANNEL_BSYERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_BSYERR;

        // Inlet processing events
        if(plFlagsSrc & IDN_PEVFLG_INLET_FRGERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_FRGERR;
        if(plFlagsSrc & IDN_PEVFLG_INLET_CFGERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_CFGERR;
        if(plFlagsSrc & IDN_PEVFLG_INLET_CKTERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_CKTERR;
        if(plFlagsSrc & IDN_PEVFLG_INLET_DCMERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_DCMERR;
        if(plFlagsSrc & IDN_PEVFLG_INLET_CTYERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_CTYERR;
        if(plFlagsSrc & IDN_PEVFLG_INLET_MCLERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_MCLERR;
        if(plFlagsSrc & IDN_PEVFLG_INLET_IAPERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_IAPERR;

        // Output and device events
        if(plFlagsSrc & IDN_PEVFLG_OUTPUT_RGUERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_RGUERR;
        if(plFlagsSrc & IDN_PEVFLG_OUTPUT_PVLERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_PVLERR;
        if(plFlagsSrc & IDN_PEVFLG_OUTPUT_DVIERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_DVIERR;
        if(plFlagsSrc & IDN_PEVFLG_OUTPUT_IAPERR) plFlagsDst |= IDNVAL_RTACK_PEVFLG_IAPERR;

        ackRspHdr->pipelineEventFlags = htob16(plFlagsDst);

        // -------------------

        uint8_t stsFlagsOut = 0;

        if(session != (ODFSession *)0)
        {
            if(session->hasOpenChannels()) stsFlagsOut |= IDNVAL_RTACK_STSFLG_SOCNL;
        }

        // IDNVAL_RTACK_STSFLG_DOBUF -> Devices occupy buffers
        // IDNVAL_RTACK_STSFLG_SNMSG -> Session has messages

        ackRspHdr->statusFlags = stsFlagsOut;

        // -------------------

        // ackRspHdr->linkQuality =

        // -------------------

        // ackRspHdr->latency =
    }

    // Check for close command (disconnect and graceful session close after message processing)
    if(cmd == IDNCMD_RT_CNLMSG_CLOSE)
    {
        // Connection close: Forward to session and delete the connection.
        connection->handleLinkClose(env);
        destroyConnection(connection);
    }

    // Report successfully passed. Note: Buffer is passed and will not be freed by caller.
    return result;
}


// -------------------------------------------------------------------------------------------------
//  scope: protected
// -------------------------------------------------------------------------------------------------

int IDNServer::checkExcluded(uint8_t flags)
{
    // Check group mask
    if((clientGroupMask & (0x0001 << (flags & IDNMSK_PKTFLAGS_GROUP))) == 0) return 1;

    return 0;
}


void IDNServer::processCommand(ODF_ENV *env, RECV_COOKIE *cookie, ODF_TAXI_BUFFER *taxiBuffer)
{
    TracePrinter tpr(env, "IDNServer~processCommand");

    // Check minimum packet size
    if(taxiBuffer->getTotalLen() < sizeof(IDNHDR_PACKET))
    {
        tpr.logWarn("%s|cmd: Invalid packet size %u", cookie->diagString, taxiBuffer->getTotalLen());
        taxiBuffer->discard();
        return;
    }

    // Ensure complete IDN packet struct in first buffer (coalesce in case scattered)
    if(taxiBuffer->coalesce(sizeof(IDNHDR_PACKET)) < 0)
    {
        tpr.logError("%s|cmd: Coalesce(%u) failed", cookie->diagString, sizeof(IDNHDR_PACKET));
        taxiBuffer->discard();
        return;
    }

    // Get packet header and payload length
    IDNHDR_PACKET *recvPacketHdr = (IDNHDR_PACKET *)taxiBuffer->getPayloadPtr();
    unsigned recvPayloadLen = taxiBuffer->getTotalLen() - sizeof(IDNHDR_PACKET);

    // Append client group to remote address (for diagnostics)
    uint8_t clientGroup = recvPacketHdr->flags & IDNMSK_PKTFLAGS_GROUP;
    unsigned diagLen = strlen(cookie->diagString);
    char *diagPtr = &cookie->diagString[diagLen];
    snprintf(diagPtr, sizeof(cookie->diagString) - diagLen, ",cg<%u>", clientGroup);

    // Dispatch command
    int command = recvPacketHdr->command;
    switch(command)
    {
        case IDNCMD_PING_REQUEST:
        {
            // Ensure for contiguous request packet
            if(taxiBuffer->coalesce(taxiBuffer->getTotalLen()) < 0)
            {
                tpr.logError("%s|Ping: Coalesce(%u) failed", cookie->diagString, taxiBuffer->getTotalLen());
                break;
            }

            // Get/Allocate send buffer
            unsigned sendLen = sizeof(IDNHDR_PACKET) + recvPayloadLen;
            uint8_t *sendBufferPtr = cookie->getSendBuffer(env, sendLen);
            if(sendBufferPtr == (uint8_t *)0)
            {
                tpr.logError("%s|Ping: No send buffer, %u", cookie->diagString, sendLen);
                break;
            }

            // IDN-Hello packet header
            IDNHDR_PACKET *sendPacketHdr = (IDNHDR_PACKET *)(sendBufferPtr);
            sendPacketHdr->command = IDNCMD_PING_RESPONSE;
            sendPacketHdr->flags = recvPacketHdr->flags & IDNMSK_PKTFLAGS_GROUP;
            sendPacketHdr->sequence = recvPacketHdr->sequence;

            // Copy additional ping data
            memcpy(&sendPacketHdr[1], &recvPacketHdr[1], recvPayloadLen);

            // Send response back to requester, no more buffer access hereafter!
            cookie->sendResponse(sendLen);
        }
        break;


        case IDNCMD_GROUP_REQUEST:
        {
            // Ensure for contiguous request packet
            if(taxiBuffer->coalesce(taxiBuffer->getTotalLen()) < 0)
            {
                tpr.logError("%s|Group: Coalesce(%u) failed", cookie->diagString, taxiBuffer->getTotalLen());
                break;
            }

            // Get/Allocate send buffer
            unsigned sendLen = sizeof(IDNHDR_PACKET) + sizeof(IDNHDR_GROUP_RESPONSE);
            uint8_t *sendBufferPtr = cookie->getSendBuffer(env, sendLen);
            if(sendBufferPtr == (uint8_t *)0)
            {
                tpr.logError("%s|Group: No send buffer, %u", cookie->diagString, sendLen);
                break;
            }

            // IDN-Hello packet header
            IDNHDR_PACKET *sendPacketHdr = (IDNHDR_PACKET *)(sendBufferPtr);
            sendPacketHdr->command = IDNCMD_GROUP_RESPONSE;
            sendPacketHdr->flags = recvPacketHdr->flags & IDNMSK_PKTFLAGS_GROUP;
            sendPacketHdr->sequence = recvPacketHdr->sequence;

            // Group response header
            IDNHDR_GROUP_RESPONSE *groupRspHdr = (IDNHDR_GROUP_RESPONSE *)&sendPacketHdr[1];
            memset(groupRspHdr, 0, sizeof(IDNHDR_GROUP_RESPONSE));
            groupRspHdr->structSize = sizeof(IDNHDR_GROUP_RESPONSE);

            // Validate group request header
            IDNHDR_GROUP_REQUEST *groupReqHdr = (IDNHDR_GROUP_REQUEST *)&recvPacketHdr[1];
            if((recvPayloadLen < 1) || (recvPayloadLen < groupReqHdr->structSize))
            {
                groupRspHdr->opCode = IDNVAL_GROUPOP_ERR_REQUEST;
                groupReqHdr = (IDNHDR_GROUP_REQUEST *)0;
            }
            else if(groupReqHdr->structSize != sizeof(IDNHDR_GROUP_REQUEST))
            {
                groupRspHdr->opCode = IDNVAL_GROUPOP_ERR_REQUEST;
                groupReqHdr = (IDNHDR_GROUP_REQUEST *)0;
            }

            // Process operation
            if(groupReqHdr == (IDNHDR_GROUP_REQUEST *)0)
            {
                // Invalid request
            }
            else if(groupReqHdr->opCode == IDNVAL_GROUPOP_GETMASK)
            {
                groupRspHdr->opCode = IDNVAL_GROUPOP_SUCCESS;
            }
/*
// FIXME: Setting a group mask to exclude/include client groups
 
            else if(groupReqHdr->opCode == IDNVAL_GROUPOP_SETMASK)
            {
                if(strcmp((char *)(groupReqHdr->authCode), "GroupAuth"))
                {
                    groupRspHdr->opCode = IDNVAL_GROUPOP_ERR_AUTH;
                }
                else
                {
                    clientGroupMask = ntohs(groupReqHdr->groupMask);
                    groupRspHdr->opCode = IDNVAL_GROUPOP_SUCCESS;

                    // Close sessions in case excluded by the client group mask
                    for(int i = 0; i < SESSION_COUNT; i++)
                    {
                        if(checkExcluded(sessions[i].getClientGroup()) &&
                           (sessions[i].isEstablished()))
                        {
                            // Immediate disconnect/reset
                            sessions[i].cancel("group excluded");
                        }
                    }
                }
            }
*/
            else
            {
                // No support for other operations
                groupRspHdr->opCode = IDNVAL_GROUPOP_ERR_OPERATION;
            }

            // Populate response group mask field
            groupRspHdr->groupMask = htob16(clientGroupMask);

            // Send response back to requester, no more buffer access hereafter!
            cookie->sendResponse(sendLen);
        }
        break;


        case IDNCMD_SCAN_REQUEST:
        {
            // Get/Allocate send buffer
            unsigned sendLen = sizeof(IDNHDR_PACKET) + sizeof(IDNHDR_SCAN_RESPONSE);
            uint8_t *sendBufferPtr = cookie->getSendBuffer(env, sendLen);
            if(sendBufferPtr == (uint8_t *)0)
            {
                tpr.logError("%s|Scan: No send buffer, %u", cookie->diagString, sendLen);
                break;
            }

            // IDN-Hello packet header
            IDNHDR_PACKET *sendPacketHdr = (IDNHDR_PACKET *)(sendBufferPtr);
            sendPacketHdr->command = IDNCMD_SCAN_RESPONSE;
            sendPacketHdr->flags = recvPacketHdr->flags & IDNMSK_PKTFLAGS_GROUP;
            sendPacketHdr->sequence = recvPacketHdr->sequence;

            // Scan response header
            IDNHDR_SCAN_RESPONSE *scanRspHdr = (IDNHDR_SCAN_RESPONSE *)&sendPacketHdr[1];
            memset(scanRspHdr, 0, sizeof(IDNHDR_SCAN_RESPONSE));
            scanRspHdr->structSize = sizeof(IDNHDR_SCAN_RESPONSE);
            scanRspHdr->protocolVersion = (unsigned char)((0 << 4) | 1);

            // Populate status
            scanRspHdr->status = 0;
            scanRspHdr->status |= IDNFLG_SCAN_STATUS_REALTIME;  // Offers IDN-RT reatime streaming
            if(checkExcluded(recvPacketHdr->flags)) scanRspHdr->status |= IDNFLG_SCAN_STATUS_EXCLUDED;
// FIXME: unit issues -> IDNFLG_SCAN_STATUS_OFFLINE;

            // Populate unitID and host name
            getUnitID(scanRspHdr->unitID, sizeof(scanRspHdr->unitID));
            getHostName(scanRspHdr->hostName, sizeof(scanRspHdr->hostName));

            // Send response back to requester, no more buffer access hereafter!
            cookie->sendResponse(sendLen);
        }
        break;


        case IDNCMD_SERVICEMAP_REQUEST:
        {
//FIXME: Too much stack use
            // Build tables
            unsigned relayCount = 0, serviceCount = 0;
            IDNHDR_SERVICEMAP_ENTRY relayTable[0xFF];
            IDNHDR_SERVICEMAP_ENTRY serviceTable[0xFF];

#if !OPENIDN_BRIDGE_MODE
            HeliosAdapter::updateDeviceList();
#endif

            for(LLNode<ServiceNode> *node = firstService; node != (LLNode<ServiceNode> *)0; node = node->getNextNode())
            {
                IDNService *service = static_cast<IDNService *>(node);

                if (!service->isActive)
                    continue;

                if(serviceCount >= 0xFF)
                {
                    tpr.logError("%s|Map: Excess service count");
                    relayCount = serviceCount = 0;
                    break;
                }

                uint8_t flags = 0;
                if(service->isDefaultService()) flags |= IDNFLG_MAPENTRY_DEFAULT_SERVICE;

                // Populate the entry. Note: Name field not terminated, padded with '\0'
                IDNHDR_SERVICEMAP_ENTRY *mapEntry = &serviceTable[serviceCount];
                mapEntry->serviceID = service->getServiceID();
                mapEntry->serviceType = service->getServiceType();
                mapEntry->flags = flags;
                mapEntry->relayNumber = 0;
                memset(mapEntry->name, 0, sizeof(mapEntry->name));
                service->copyServiceName((char *)(mapEntry->name), sizeof(mapEntry->name));

                // Next entry
                serviceCount++;
            }

            // ---------------------------------------------

            // Get/Allocate send buffer (add memory needed for the tables)
            unsigned sendLen = sizeof(IDNHDR_PACKET) + sizeof(IDNHDR_SERVICEMAP_RESPONSE);
            sendLen += relayCount * sizeof(IDNHDR_SERVICEMAP_ENTRY);
            sendLen += serviceCount * sizeof(IDNHDR_SERVICEMAP_ENTRY);
            uint8_t *sendBufferPtr = cookie->getSendBuffer(env, sendLen);
            if(sendBufferPtr == (uint8_t *)0)
            {
                tpr.logError("%s|Map: No send buffer, %u", cookie->diagString, sendLen);
                break;
            }

            // IDN-Hello packet header
            IDNHDR_PACKET *sendPacketHdr = (IDNHDR_PACKET *)(sendBufferPtr);
            sendPacketHdr->command = IDNCMD_SERVICEMAP_RESPONSE;
            sendPacketHdr->flags = recvPacketHdr->flags & IDNMSK_PKTFLAGS_GROUP;
            sendPacketHdr->sequence = recvPacketHdr->sequence;

            // Service map header setup
            IDNHDR_SERVICEMAP_RESPONSE *mapRspHdr = (IDNHDR_SERVICEMAP_RESPONSE *)&sendPacketHdr[1];
            memset(mapRspHdr, 0, sizeof(IDNHDR_SERVICEMAP_RESPONSE));
            mapRspHdr->structSize = sizeof(IDNHDR_SERVICEMAP_RESPONSE);
            mapRspHdr->entrySize = sizeof(IDNHDR_SERVICEMAP_ENTRY);
            mapRspHdr->relayEntryCount = (uint8_t)relayCount;
            mapRspHdr->serviceEntryCount = (uint8_t)serviceCount;

            // Copy tables
            IDNHDR_SERVICEMAP_ENTRY *mapEntry = (IDNHDR_SERVICEMAP_ENTRY *)&mapRspHdr[1];
            memcpy(mapEntry, relayTable, relayCount * sizeof(IDNHDR_SERVICEMAP_ENTRY));
            mapEntry = &mapEntry[relayCount];
            memcpy(mapEntry, serviceTable, serviceCount * sizeof(IDNHDR_SERVICEMAP_ENTRY));

            // ---------------------------------------------

            // Send response back to requester, no more buffer access hereafter!
            cookie->sendResponse(sendLen);
        }
        break;


        case IDNCMD_RT_CNLMSG:
        {
            if(checkExcluded(recvPacketHdr->flags)) break;

            // Process message. Note: The buffer is passed. No more access!
            processRtConnection(env, cookie, (IDNHDR_RT_ACKNOWLEDGE *)0, IDNCMD_RT_CNLMSG, taxiBuffer);
            taxiBuffer = (ODF_TAXI_BUFFER *)0;
        }
        break;


        case IDNCMD_RT_CNLMSG_ACKREQ:
        {
            IDNHDR_PACKET *sendPacketHdr = (IDNHDR_PACKET *)0;
            IDNHDR_RT_ACKNOWLEDGE *ackRspHdr = (IDNHDR_RT_ACKNOWLEDGE *)0;

            // Get/Allocate send buffer
            unsigned sendLen = sizeof(IDNHDR_PACKET) + sizeof(IDNHDR_RT_ACKNOWLEDGE);
            uint8_t *sendBufferPtr = cookie->getSendBuffer(env, sendLen);
            if(sendBufferPtr == (uint8_t *)0)
            {
                tpr.logError("%s|Ack: No send buffer, %u", cookie->diagString, sendLen);
            }
            else
            {
                // IDN-Hello packet header
                sendPacketHdr = (IDNHDR_PACKET *)(sendBufferPtr);
                sendPacketHdr->command = IDNCMD_RT_ACKNOWLEDGE;
                sendPacketHdr->flags = recvPacketHdr->flags & IDNMSK_PKTFLAGS_GROUP;
                sendPacketHdr->sequence = recvPacketHdr->sequence;

                // Acknowledgement response header
                ackRspHdr = (IDNHDR_RT_ACKNOWLEDGE *)&sendPacketHdr[1];
                memset(ackRspHdr, 0, sizeof(IDNHDR_RT_ACKNOWLEDGE));
                ackRspHdr->structSize = sizeof(IDNHDR_RT_ACKNOWLEDGE);
            }

            // Process message command
            int resultCode;
            if(checkExcluded(recvPacketHdr->flags))
            {
                resultCode = IDNVAL_RTACK_ERR_EXCLUDED;
            }
            else
            {
                // Process message. Note: The buffer is passed. No more access!
                resultCode = processRtConnection(env, cookie, ackRspHdr, IDNCMD_RT_CNLMSG, taxiBuffer);
                taxiBuffer = (ODF_TAXI_BUFFER *)0;
            }

            if(ackRspHdr != (IDNHDR_RT_ACKNOWLEDGE *)0)
            {
                // Populate acknowledgement result code
                ackRspHdr->resultCode = resultCode;

                // Send response back to requester, no more buffer access hereafter!
                cookie->sendResponse(sendLen);
            }
        }
        break;


        case IDNCMD_RT_CNLMSG_CLOSE:
        {
            if(checkExcluded(recvPacketHdr->flags)) break;

            // Process message. Note: The buffer is passed. No more access!
            processRtConnection(env, cookie, (IDNHDR_RT_ACKNOWLEDGE *)0, IDNCMD_RT_CNLMSG_CLOSE, taxiBuffer);
            taxiBuffer = (ODF_TAXI_BUFFER *)0;
        }
        break;


        case IDNCMD_RT_CNLMSG_CLOSE_ACKREQ:
        {
            IDNHDR_PACKET *sendPacketHdr = (IDNHDR_PACKET *)0;
            IDNHDR_RT_ACKNOWLEDGE *ackRspHdr = (IDNHDR_RT_ACKNOWLEDGE *)0;

            // Get/Allocate send buffer
            unsigned sendLen = sizeof(IDNHDR_PACKET) + sizeof(IDNHDR_RT_ACKNOWLEDGE);
            uint8_t *sendBufferPtr = cookie->getSendBuffer(env, sendLen);
            if(sendBufferPtr == (uint8_t *)0)
            {
                tpr.logError("%s|Ack: No send buffer, %u", cookie->diagString, sendLen);
            }
            else
            {
                // IDN-Hello packet header
                sendPacketHdr = (IDNHDR_PACKET *)(sendBufferPtr);
                sendPacketHdr->command = IDNCMD_RT_ACKNOWLEDGE;
                sendPacketHdr->flags = recvPacketHdr->flags & IDNMSK_PKTFLAGS_GROUP;
                sendPacketHdr->sequence = recvPacketHdr->sequence;

                // Acknowledgement response header
                ackRspHdr = (IDNHDR_RT_ACKNOWLEDGE *)&sendPacketHdr[1];
                memset(ackRspHdr, 0, sizeof(IDNHDR_RT_ACKNOWLEDGE));
                ackRspHdr->structSize = sizeof(IDNHDR_RT_ACKNOWLEDGE);
            }

            // Process message / close command
            int resultCode;
            if(checkExcluded(recvPacketHdr->flags))
            {
                resultCode = IDNVAL_RTACK_ERR_EXCLUDED;
            }
            else
            {
                // Process message. Note: The buffer is passed. No more access!
                resultCode = processRtConnection(env, cookie, ackRspHdr, IDNCMD_RT_CNLMSG_CLOSE, taxiBuffer);
                taxiBuffer = (ODF_TAXI_BUFFER *)0;
            }

            if(ackRspHdr != (IDNHDR_RT_ACKNOWLEDGE *)0)
            {
                // Populate acknowledgement result code
                ackRspHdr->resultCode = resultCode;

                // Send response back to requester, no more buffer access hereafter!
                cookie->sendResponse(sendLen);
            }
        }
        break;


        case IDNCMD_RT_ABORT:
        {
            IDNHelloConnection *connection = findConnection(env, cookie, clientGroup);

            // In case there is a connection - abort
            if(connection != (IDNHelloConnection *)0)
            {
                connection->handleAbort(env);
                destroyConnection(connection);
            }
        }
        break;


        default:
        {
            tpr.logWarn("%s|IDN-Hello: Unknown command %02X", cookie->diagString, command);
        }
        break;
    }

    // In case the taxi buffer was not passed - free memory. No more access!
    if(taxiBuffer != (ODF_TAXI_BUFFER *)0) taxiBuffer->discard();
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

IDNServer::IDNServer(LLNode<ServiceNode> *firstService):
    firstService(firstService)
{
    firstConnection = (LLNode<ConnectionNode> *)0;
    firstSession = (LLNode<SessionNode> *)0;

    // Per default allow all client groups
    clientGroupMask = 0xFFFF;
}


IDNServer::~IDNServer()
{
// FIXME
/*
    // Sanity check
    if(firstConnection != (LLNode<ConnectionNode> *)0)
    {
        logError("IDNServer|dtor: Unfreed connection list %p", firstConnection);
    }
    if(firstSession != (LLNode<SessionNode> *)0)
    {
        logError("IDNServer|dtor: Unfreed session list %p", firstSession);
    }
*/
}


void IDNServer::setUnitID(uint8_t *fieldPtr, unsigned fieldSize)
{
    UNITID_FIELD_FROM_FIELD(cfgUnitID, sizeof(cfgUnitID), fieldPtr, fieldSize);
}


void IDNServer::getUnitID(uint8_t *fieldPtr, unsigned fieldSize)
{
    UNITID_FIELD_FROM_FIELD(fieldPtr, fieldSize, cfgUnitID, sizeof(cfgUnitID));
}


void IDNServer::setHostName(uint8_t *fieldPtr, unsigned fieldSize)
{
    STRCPY_FIELD_FROM_FIELD(cfgHostName, sizeof(cfgHostName), fieldPtr, fieldSize);
}


void IDNServer::getHostName(uint8_t *fieldPtr, unsigned fieldSize)
{
    STRCPY_FIELD_FROM_FIELD(fieldPtr, fieldSize, cfgHostName, sizeof(cfgHostName));
}


IDNService *IDNServer::getService(uint8_t serviceID)
{
    for(LLNode<ServiceNode> *node = firstService; node != (LLNode<ServiceNode> *)0; node = node->getNextNode())
    {
        IDNService *service = static_cast<IDNService *>(node);
        if(service->getServiceID() == serviceID) return service;
    }

    return (IDNService *)0;
}


IDNService *IDNServer::getDefaultService(uint8_t serviceMode)
{
    for(LLNode<ServiceNode> *node = firstService; node != (LLNode<ServiceNode> *)0; node = node->getNextNode())
    {
        IDNService *service = static_cast<IDNService *>(node);
        if(service->isDefaultService() && service->handlesMode(serviceMode))
        {
            return service;
        }
    }

    return (IDNService *)0;
}


void IDNServer::housekeeping(ODF_ENV *env, uint32_t envTimeUS)
{
    // Note: Shutdown in case envTimeUS == 0 !!

    if(envTimeUS == 0)
    {
        // Unconditionally abandon all connections
        while(firstConnection)
        {
            LLNode<ConnectionNode> *node = firstConnection;
            IDNHelloConnection *connection = static_cast<IDNHelloConnection *>(node);

            // Immediate connection stop, linkout and delete the connection
            connection->handleAbort(env);
            destroyConnection(connection);
        }

        // Unconditionally abandon all sessions
        while(firstSession)
        {
            LLNode<SessionNode> *node = firstSession;
            ODFSession *session = static_cast<ODFSession *>(node);

            // Immediate session stop, linkout and delete the session
/*            if(session->getSessionState() != SESSIONSTATE_ABANDONED)*/ session->cancelImmediately(env);
            destroySession(session);
        }

        // Housekeeping for all services (and outputs/adapters)
        for(LLNode<ServiceNode> *node = firstService; node != (LLNode<ServiceNode> *)0; )
        {
            IDNService *service = static_cast<IDNService *>(node);
            node = node->getNextNode();

            service->housekeeping(env, true);
        }
    }
    else
    {
        // First, check all connections. Teardown may leave orphan sessions
        for(LLNode<ConnectionNode> *node = firstConnection; node != (LLNode<ConnectionNode> *)0; )
        {
            IDNHelloConnection *connection = static_cast<IDNHelloConnection *>(node);
            node = node->getNextNode();

            // Check for destruction
            if(connection->checkTeardown(env, envTimeUS)) destroyConnection(connection);
        }

        // Check all sessions
        for(LLNode<SessionNode> *node = firstSession; node != (LLNode<SessionNode> *)0; )
        {
            ODFSession *session = static_cast<ODFSession *>(node);
            node = node->getNextNode();

            // Check for destruction
            if(session->checkTeardown(env, envTimeUS)) destroySession(session);
        }

        // Housekeeping for all services (and outputs/adapters)
        for(LLNode<ServiceNode> *node = firstService; node != (LLNode<ServiceNode> *)0; )
        {
            IDNService *service = static_cast<IDNService *>(node);
            node = node->getNextNode();

            service->housekeeping(env, false);
        }
    }
}
