// -------------------------------------------------------------------------------------------------
//  File IDNService.hpp
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


#ifndef IDN_SERVICE_HPP
#define IDN_SERVICE_HPP


// Standard libraries
#include <stdint.h>

// Project headers
#include "../shared/ODFEnvironment.hpp"
#include "../shared/ODFTaxiBuffer.hpp"
#include "LLNode.hpp"



// Node types (incomplete, not declared)
class ServiceNode;


// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class IDNInlet
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:

    unsigned pipelineEvents;                        // Processing flags (since last acknowledge)


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    IDNInlet();
    virtual ~IDNInlet();

    virtual uint8_t getServiceMode() = 0;

    virtual void reset();
    virtual unsigned clearPipelineEvents();

    virtual void process(ODF_ENV *env, ODF_TAXI_BUFFER *taxiBuffer) = 0;
};



class IDNConfigInlet: public IDNInlet
{
    typedef IDNInlet Inherited;

    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    bool configValid;
    uint8_t keptConfigFlags;
    uint8_t keptConfigWordCnt;
    uint32_t keptConfigWords[0x100];


    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:
    
    virtual int createDecoder(unsigned serviceMode, void *paramPtr, unsigned paramLen) = 0;

    virtual void invalidateConfig();
    virtual void readConfig(ODF_ENV *env, ODF_TAXI_BUFFER *taxiBuffer);

    // -- Inline Methods ----------------
    int isConfigValid() { return configValid; }
    unsigned getConfigFlags() { return keptConfigFlags; }


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    IDNConfigInlet();
    virtual ~IDNConfigInlet();

    // -- Inline Methods ----------------
    virtual void reset();
};



class IDNService: public LLNode<ServiceNode>
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    uint8_t serviceID;
    char *serviceName;
    bool defaultServiceFlag;


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    IDNService(uint8_t serviceID, char *serviceName, bool defaultServiceFlag);
    virtual ~IDNService();

    virtual uint8_t getServiceType() = 0;
    virtual int copyServiceName(char *bufferPtr, unsigned bufferSize);
    virtual int setServiceName(char *bufferPtr, unsigned bufferSize);

    virtual bool handlesMode(uint8_t serviceMode);
    virtual IDNInlet *requestInlet(ODF_ENV *env, uint8_t serviceMode) = 0;
    virtual void releaseInlet(ODF_ENV *env, IDNInlet *inlet) = 0;

    virtual void housekeeping(ODF_ENV *env, bool shutdownFlag);

    // -- Inline Methods ----------------
    uint8_t getServiceID() { return serviceID; }
    bool isDefaultService() { return defaultServiceFlag; }

    bool isActive;
};


#endif
