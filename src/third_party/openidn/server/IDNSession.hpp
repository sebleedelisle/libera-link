// -------------------------------------------------------------------------------------------------
//  File IDNSession.hpp
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


#ifndef IDNSESSION_HPP
#define IDNSESSION_HPP


// Standard libraries
#include <stdint.h>

// Project headers
#include "idn-stream.h"
#include "../shared/ODFEnvironment.hpp"
#include "../shared/ODFTaxiBuffer.hpp"



// -------------------------------------------------------------------------------------------------
//  Defines
// -------------------------------------------------------------------------------------------------

#define CHANNEL_FLAG_OPEN                   0x01        // The channel is allocated/open
#define CHANNEL_FLAG_CS_SERVICE_ERROR       0x10        // The service resolution failed
#define CHANNEL_FLAG_CS_CONDUIT_ERROR       0x20        // The request for a conduit failed
#define CHANNEL_FLAG_CS_CONNECTED           0x30        // The channel is connected

#define CHANNEL_MASK_CONNSTATE              0x30        // Mask for the connection state


// -------------------------------------------------------------------------------------------------
//  Typedefs
// -------------------------------------------------------------------------------------------------

struct SERVICE_HANDLE_  { int unused; };    typedef struct SERVICE_HANDLE_ *    SERVICE_HANDLE;
struct CONDUIT_HANDLE_  { int unused; };    typedef struct CONDUIT_HANDLE_ *    CONDUIT_HANDLE;

typedef struct
{
    uint8_t channelID;
    uint8_t flags;
    uint8_t serviceID;
    uint8_t serviceMode;

    SERVICE_HANDLE serviceHnd;
    CONDUIT_HANDLE conduitHnd;

} IDN_CHANNEL;


// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class IDNSession
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    char logIdent[64];                              // A diagnostic ident string for logging

    void releaseHandles(ODF_ENV *env, IDN_CHANNEL *channel, bool abortFlag);


    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:

    IDN_CHANNEL channels[IDNVAL_CHANNEL_COUNT];
    unsigned openChannelCount;


    IDNSession(char *logIdent);

    virtual void openChannel(ODF_ENV *env, IDN_CHANNEL *channel);
    virtual void closeChannel(ODF_ENV *env, IDN_CHANNEL *channel, bool abortFlag);

    virtual SERVICE_HANDLE requestService(ODF_ENV *env, uint8_t channelID, uint8_t serviceID, uint8_t serviceMode) = 0;
    virtual void releaseService(ODF_ENV *env, uint8_t channelID, SERVICE_HANDLE serviceHnd) = 0;

    virtual CONDUIT_HANDLE requestConduit(ODF_ENV *env, uint8_t channelID, SERVICE_HANDLE serviceHnd, uint8_t serviceMode) = 0;
    virtual void releaseConduit(ODF_ENV *env, uint8_t channelID, SERVICE_HANDLE serviceHnd, CONDUIT_HANDLE conduitHnd, bool abortFlag) = 0;

    virtual void input(ODF_ENV *env, SERVICE_HANDLE serviceHnd, CONDUIT_HANDLE conduitHnd, ODF_TAXI_BUFFER *taxiBuffer) = 0;


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    virtual ~IDNSession();

    virtual void cancel(ODF_ENV *env, uint8_t channelID);
    virtual void reset(ODF_ENV *env);

    virtual int processChannelMessage(ODF_ENV *env, ODF_TAXI_BUFFER *taxiBuffer);

    // -- Inline Methods ----------------
    char *getLogIdent() { return logIdent; }
    bool hasOpenChannels() { return openChannelCount != 0; }
};


#endif

