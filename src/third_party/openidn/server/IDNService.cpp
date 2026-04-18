// -------------------------------------------------------------------------------------------------
//  File IDNService.cpp
//
//  Copyright (c) 2013-2025 DexLogic, Dirk Apitz. All Rights Reserved.
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
//
//  IDN service base classes
//
// -------------------------------------------------------------------------------------------------
//  Change History:
//
//  07/2013 Dirk Apitz, created
//  01/2025 Dirk Apitz, modifications and integration into OpenIDN
// -------------------------------------------------------------------------------------------------


// Standard libraries
#include <string.h>
#if defined(__APPLE__)
#include <stdlib.h>
#else
#include <malloc.h>
#endif

// Project headers
#include "idn-stream.h"
#include "../shared/ODFTools.hpp"
#include "PEVFlags.h"

// Module header
#include "IDNService.hpp"



// =================================================================================================
//  Class IDNInlet
//
// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

IDNInlet::IDNInlet()
{
    pipelineEvents = 0;
}


IDNInlet::~IDNInlet()
{
}


void IDNInlet::reset()
{
    pipelineEvents = 0;
}


unsigned IDNInlet::clearPipelineEvents()
{
    unsigned result = pipelineEvents;
    pipelineEvents = 0;

    return result;
}



// =================================================================================================
//  Class IDNConfigInlet
//
// -------------------------------------------------------------------------------------------------
//  scope: protected
// -------------------------------------------------------------------------------------------------

void IDNConfigInlet::invalidateConfig()
{
    configValid = false;
    keptConfigFlags = 0;
    keptConfigWordCnt = 0;
}


void IDNConfigInlet::readConfig(ODF_ENV *env, ODF_TAXI_BUFFER *taxiBuffer)
{
    IDNHDR_CHANNEL_MESSAGE *channelMessageHdr = (IDNHDR_CHANNEL_MESSAGE *)taxiBuffer->getPayloadPtr();
    uint16_t contentID = btoh16(channelMessageHdr->contentID);
    uint16_t chunkType = (contentID & IDNMSK_CONTENTID_CNKTYPE);

    IDNHDR_CHANNEL_CONFIG *channelConfigHdr = (IDNHDR_CHANNEL_CONFIG *)0;
    uint8_t configFlags = 0;
    if((chunkType <= 0xBF) && (contentID & IDNFLG_CONTENTID_CONFIG_LSTFRG))
    {
        // Message (entire chunk or first chunk fragment) contains channel configuration
        channelConfigHdr = (IDNHDR_CHANNEL_CONFIG *)&channelMessageHdr[1];
        configFlags = channelConfigHdr->flags;
    }

    // Create decoder in case the message has a config header and routing flag set
    if(configFlags & (uint8_t)IDNFLG_CHNCFG_ROUTING)
    {
        // In case configuration is valid: Check message with cache
        if(configValid)
        {
            // Check cached config word count
            bool changed = (channelConfigHdr->wordCount != keptConfigWordCnt);

            // Check cached config words
            if(!changed)
            {
                // Note: Upper layer guarantees config data to be contiguous.
                uint32_t *p1 = keptConfigWords, *p2 = (uint32_t *)&channelConfigHdr[1];
                for(uint32_t i = keptConfigWordCnt; i > 0; i--)
                {
                    if(*p1++ != *p2++) { changed = true; break; }
                }
            }

            // Has new config data: Invalidate configuration, delete decoder
            if(changed) invalidateConfig();
        }

        // In case configuration is invalid: Try to create a decoder and update the cache
        if(!configValid)
        {
            // Create a decoder in case the new configuration would fit into the cache
            unsigned wordCount = channelConfigHdr->wordCount;
            if(wordCount <= (sizeof(keptConfigWords) / sizeof(uint32_t)))
            {
                void *paramPtr = (void *)&channelConfigHdr[1];
                unsigned paramLen = channelConfigHdr->wordCount * 4;
                if(createDecoder(getServiceMode(), paramPtr, paramLen) >= 0)
                {
                    // Update cache. Note: Upper layer guarantees config data to be contiguous.
                    keptConfigWordCnt = channelConfigHdr->wordCount;
                    uint32_t *dst = keptConfigWords, *src = (uint32_t *)&channelConfigHdr[1];
                    for(unsigned i = keptConfigWordCnt; i > 0; i--) *dst++ = *src++;

                    configValid = true;
                }
                else
                {
                    pipelineEvents |= IDN_PEVFLG_INLET_CFGERR;
                }
            }
        }

        // Check for valid configuration
        if(configValid)
        {
            // Store service config flags (to be compared with data data match bits).
            keptConfigFlags = configFlags;
        }
    }
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

IDNConfigInlet::IDNConfigInlet()
{
    configValid = false;
    keptConfigFlags = 0;
    keptConfigWordCnt = 0;
}


IDNConfigInlet::~IDNConfigInlet()
{
}


void IDNConfigInlet::reset()
{
    invalidateConfig();
    Inherited::reset();
}



// =================================================================================================
//  Class IDNService
//
// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

IDNService::IDNService(uint8_t serviceID, char *serviceName, bool defaultServiceFlag)
{
    this->serviceID = serviceID;
    this->serviceName = (serviceName == (char *)0) ? (char *)0 : strdup(serviceName);
    this->defaultServiceFlag = defaultServiceFlag;
    this->isActive = true;
}


IDNService::~IDNService()
{
    if(serviceName != (char *)0) free(serviceName);
}


int IDNService::copyServiceName(char *bufferPtr, unsigned bufferSize)
{
    if(serviceName == (char *)0) return -1;

    // Populate destination buffer. Note: Not '\0' terminated.
    unsigned len = 0;
    uint8_t *dst = (uint8_t *)bufferPtr;
    uint8_t *src = (uint8_t *)serviceName;
    for(; (len < bufferSize) && (*src != 0); len++) *dst++ = *src++;
    if(len < bufferSize) *dst = 0;

    return len;
}

int IDNService::setServiceName(char* bufferPtr, unsigned bufferSize)
{
    // Populate destination buffer. Note: Not '\0' terminated.
    unsigned len = 0;
    uint8_t* dst = (uint8_t*)serviceName;
    uint8_t* src = (uint8_t*)bufferPtr;
    for (; (len < bufferSize) && (*src != 0); len++) *dst++ = *src++;
    if (len < bufferSize) *dst = 0;

    return len;
}


bool IDNService::handlesMode(uint8_t serviceMode)
{
    return false;
}


void IDNService::housekeeping(ODF_ENV *env, bool shutdownFlag)
{
}
