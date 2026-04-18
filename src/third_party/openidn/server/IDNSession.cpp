// -------------------------------------------------------------------------------------------------
//  File IDNSession.cpp
//
//  Copyright (c) 2019-2024 DexLogic, Dirk Apitz. All Rights Reserved.
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
//  08/2019 Dirk Apitz, created
//  06/2021 Dirk Apitz, handle/conduit redesign
//  01/2024 Dirk Apitz, modifications and integration into OpenIDN
// -------------------------------------------------------------------------------------------------


// Standard libraries
#include <string.h>
#include <stdio.h>

// Project headers
#include "../shared/ODFTools.hpp"

// Module header
#include "IDNSession.hpp"



// =================================================================================================
//  Class IDNSession
//
// -------------------------------------------------------------------------------------------------
//  scope: private
// -------------------------------------------------------------------------------------------------

void IDNSession::releaseHandles(ODF_ENV *env, IDN_CHANNEL *channel, bool abortFlag)
{
    TracePrinter tpr(env, "IDNSession~releaseHandles");

    // Release service handle and conduit handle
    if(channel->serviceHnd != (SERVICE_HANDLE)0)
    {
        if(channel->conduitHnd != (CONDUIT_HANDLE)0)
        {
            releaseConduit(env, channel->channelID, channel->serviceHnd, channel->conduitHnd, abortFlag);
            channel->conduitHnd = (CONDUIT_HANDLE)0;
        }
        else
        {
            tpr.logError("%s: Service handle without conduit handle, channelID=%u", getLogIdent(), channel->channelID);
        }

        releaseService(env, channel->channelID, channel->serviceHnd);
        channel->serviceHnd = (SERVICE_HANDLE)0;
    }
    else if(channel->conduitHnd != (CONDUIT_HANDLE)0)
    {
        tpr.logError("%s: Conduit handle without service handle, channelID=%u", getLogIdent(), channel->channelID);

        releaseConduit(env, channel->channelID, channel->serviceHnd, channel->conduitHnd, abortFlag);
        channel->conduitHnd = (CONDUIT_HANDLE)0;
    }
}


// -------------------------------------------------------------------------------------------------
//  scope: protected
// -------------------------------------------------------------------------------------------------

IDNSession::IDNSession(char *logIdent)
{
    // Copy session name
    if((logIdent != (char *)0) && (*logIdent != '\0'))
    {
        snprintf(this->logIdent, sizeof(this->logIdent), "%s", logIdent);
    }
    else
    {
        snprintf(this->logIdent, sizeof(this->logIdent), "(N/A)");
    }

    // Initialize channel array
    memset(channels, 0, sizeof(channels));
    for(unsigned channelID = 0; channelID < IDNVAL_CHANNEL_COUNT; channelID++)
    {
        channels[channelID].channelID = (uint8_t)channelID;
    }
    openChannelCount = 0;
}


void IDNSession::openChannel(ODF_ENV *env, IDN_CHANNEL *channel)
{
    TracePrinter tpr(env, "IDNSession~openChannel");

    //tpr.logDebug("%s|Session: Open channelID %u", getLogIdent(), channel->channelID);

    // Set channel open
    openChannelCount++;
    channel->flags |= CHANNEL_FLAG_OPEN;
}


void IDNSession::closeChannel(ODF_ENV *env, IDN_CHANNEL *channel, bool abortFlag)
{
    TracePrinter tpr(env, "IDNSession~closeChannel");

    //tpr.logDebug("%s|Session: Close%s channelID %u", getLogIdent(), abortFlag ? "(abort)" : "", channel->channelID);

    // Release handles held by the channel
    releaseHandles(env, channel, abortFlag);

    // Reset channel members, set channel closed
    channel->serviceID = 0;
    channel->serviceMode = 0;
    channel->flags = 0;
    openChannelCount--;
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

IDNSession::~IDNSession()
{
    // Check for open channels - just issue an error. Session shall be reset before destruction!
/*
    bool openChannels = false;
    for(unsigned i = 0; i < IDNVAL_CHANNEL_COUNT; i++)
    {
        IDN_CHANNEL *channel = &channels[i];
        if((channel->flags & CHANNEL_FLAG_OPEN) != 0)
        {
            openChannels = true;
        }
    }
// FIXME
//    if(openChannels) logError("IDNSession|dtor: Allocated channels");
*/
}


void IDNSession::cancel(ODF_ENV *env, uint8_t channelID)
{
    if(channelID >= IDNVAL_CHANNEL_COUNT) return;

    IDN_CHANNEL *channel = &channels[channelID];
    if((channel->flags & CHANNEL_FLAG_OPEN) != 0)
    {
        // Release handles and close/free the channel
        const bool abortFlag = false;
        closeChannel(env, channel, abortFlag);
    }
}


void IDNSession::reset(ODF_ENV *env)
{
    // Close all channels. Note: Can't be done in destructor because of call to virtual function
    for(unsigned i = 0; i < IDNVAL_CHANNEL_COUNT; i++)
    {
        IDN_CHANNEL *channel = &channels[i];
        if((channel->flags & CHANNEL_FLAG_OPEN) != 0)
        {
            // Release handles and close/free the channel
            const bool abortFlag = true;
            closeChannel(env, channel, abortFlag);
        }
    }
}


int IDNSession::processChannelMessage(ODF_ENV *env, ODF_TAXI_BUFFER *taxiBuffer)
{
    TracePrinter tpr(env, "IDNSession~processChannelMessage");

    // Get/Check channelID
    IDNHDR_CHANNEL_MESSAGE *channelMessage = (IDNHDR_CHANNEL_MESSAGE *)taxiBuffer->getPayloadPtr();
    uint16_t contentID = btoh16(channelMessage->contentID);
    unsigned channelID = (contentID & IDNMSK_CONTENTID_CHANNELID) >> 8;
    if(channelID >= IDNVAL_CHANNEL_COUNT)
    {
        tpr.logWarn("%s|Session: Invalid channelID %d, dropped", getLogIdent(), channelID);
        return -1;
    }

    // Get config header and config flags (if header is contained in message)
    IDNHDR_CHANNEL_CONFIG *channelConfig = (IDNHDR_CHANNEL_CONFIG *)0;
    unsigned configFlags = 0;
    unsigned chunkType = (contentID & IDNMSK_CONTENTID_CNKTYPE);
    if((chunkType <= 0xBF) && (contentID & IDNFLG_CONTENTID_CONFIG_LSTFRG))
    {
        // Check message to include a channel config header
        uint16_t totalSize = btoh16(channelMessage->totalSize);
        if(totalSize < (sizeof(IDNHDR_CHANNEL_MESSAGE) + sizeof(IDNHDR_CHANNEL_CONFIG)))
        {
            tpr.logWarn("%s|Session: Premature message end (%u, channel config)", getLogIdent(), totalSize);
            return -1;
        }

        // Message (entire chunk or first chunk fragment) contains channel configuration
        channelConfig = (IDNHDR_CHANNEL_CONFIG *)&channelMessage[1];
        configFlags = channelConfig->flags;
    }

    // Check for the channel not yet open
    IDN_CHANNEL *channel = &channels[channelID];
    if((channel->flags & CHANNEL_FLAG_OPEN) == 0)
    {
        // Message must contain routing information
        if((configFlags & IDNFLG_CHNCFG_ROUTING) == 0)
        {
            tpr.logWarn("%s|Session: Message on closed channel, channelID=%u", getLogIdent(), channelID);
            return -1;
        }

        // Open/Allocate the channel
        openChannel(env, channel);
    }

    // Pass buffer. Note: Do not abort/return in this section since configFlags might contain a close
    int result = 0;
    do
    {
        // In case routing flag is set - (Re-)Connect channel
        if((configFlags & IDNFLG_CHNCFG_ROUTING) != 0)
        {
            // Check for channel being connected to the service
            if(((channel->flags & CHANNEL_MASK_CONNSTATE) != CHANNEL_FLAG_CS_CONNECTED) ||
               (channel->serviceID != channelConfig->serviceID) ||
               (channel->serviceMode != channelConfig->serviceMode))
            {
                // If connected to another service - disconnect
                if((channel->flags & CHANNEL_MASK_CONNSTATE) == CHANNEL_FLAG_CS_CONNECTED)
                {
                    // Release handles held by the channel
                    const bool abortFlag = false;
                    releaseHandles(env, channel, abortFlag);

                    // Reset channel members (keep channel open)
                    channel->serviceID = 0;
                    channel->serviceMode = 0;
                    channel->flags &= ~CHANNEL_MASK_CONNSTATE;
                }

                // Connect to the requested service
                SERVICE_HANDLE serviceHnd = (SERVICE_HANDLE)0;
                CONDUIT_HANDLE conduitHnd = (CONDUIT_HANDLE)0;
                uint8_t reqServiceID = channel->serviceID = channelConfig->serviceID;
                uint8_t reqServiceMode = channel->serviceMode = channelConfig->serviceMode;
                if(reqServiceMode == IDNVAL_SMOD_VOID)
                {
                    // Request for NULL device/mode - set connected
                    channel->serviceID = reqServiceID;
                    channel->serviceMode = reqServiceMode;
                    channel->flags &= ~CHANNEL_MASK_CONNSTATE;
                    channel->flags |= CHANNEL_FLAG_CS_CONNECTED;
                }
                else if((serviceHnd = requestService(env, channelID, reqServiceID, reqServiceMode)) == (SERVICE_HANDLE)0)
                {
                    // Service not found - do not connect
                    channel->flags &= ~CHANNEL_MASK_CONNSTATE;
                    channel->flags |= CHANNEL_FLAG_CS_SERVICE_ERROR;
                }
                else if((conduitHnd = requestConduit(env, channelID, serviceHnd, reqServiceMode)) == (CONDUIT_HANDLE)0)
                {
                    // No conduit available - release service and do not connect
                    releaseService(env, channelID, serviceHnd);
                    channel->flags &= ~CHANNEL_MASK_CONNSTATE;
                    channel->flags |= CHANNEL_FLAG_CS_CONDUIT_ERROR;
                }
                else
                {
                    // Request for service succeeded - store conduit and set connected
                    channel->serviceID = reqServiceID;
                    channel->serviceMode = reqServiceMode;
                    channel->serviceHnd = serviceHnd;
                    channel->conduitHnd = conduitHnd;
                    channel->flags &= ~CHANNEL_MASK_CONNSTATE;
                    channel->flags |= CHANNEL_FLAG_CS_CONNECTED;
                }
            }
        }

        // -----------------------------------------------------------------------------------------

        // Pass the message in case of valid routing. Note: Do not access netBuffer hereafter!!
        if(channel->serviceHnd != (SERVICE_HANDLE)0) input(env, channel->serviceHnd, channel->conduitHnd, taxiBuffer);
        else result = -1;

        // No more buffer/message access!!
        taxiBuffer = (ODF_TAXI_BUFFER *)0;
        channelMessage = (IDNHDR_CHANNEL_MESSAGE *)0;
        channelConfig = (IDNHDR_CHANNEL_CONFIG *)0;
    }
    while(0);

    // Check for close request
    if((configFlags & IDNFLG_CHNCFG_CLOSE) != 0)
    {
        // Release handles and close/free the channel
        const bool abortFlag = false;
        closeChannel(env, channel, abortFlag);
    }

    // Note: Result of <0 indicates that the buffer was not passed and must be freed by the caller
    return result;
}

