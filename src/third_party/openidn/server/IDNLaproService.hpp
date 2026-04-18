// -------------------------------------------------------------------------------------------------
//  File IDNLaproService.hpp
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
//  Laser projector service/inlet base classes
//
// -------------------------------------------------------------------------------------------------
//  Change History:
//
//  07/2013 Dirk Apitz, created
//  01/2025 Dirk Apitz, modifications and integration into OpenIDN
// -------------------------------------------------------------------------------------------------


#ifndef IDN_LAPRO_SERVICE_HPP
#define IDN_LAPRO_SERVICE_HPP


// Standard libraries
#include <memory>

// Project headers
#include "../output/RTLaproGraphOut.hpp"
#include "IDNService.hpp"



// Forward declarations
class IDNLaproGraConInlet;
class IDNLaproGraDisInlet;


// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class IDNLaproGraphInlet: public IDNConfigInlet
{
    typedef IDNConfigInlet Inherited;

    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:

    RTLaproGraphicOutput *rtOutput;
    RTLaproDecoder *decoder;

    // -- Inherited Members -------------
    virtual int createDecoder(unsigned serviceMode, void *paramPtr, unsigned paramLen);
    virtual void invalidateConfig();


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    IDNLaproGraphInlet(RTLaproGraphicOutput *rtOutput);
    virtual ~IDNLaproGraphInlet();

    // -- Inherited Members -------------
    virtual void reset();
};



class IDNLaproService: public IDNService
{
    typedef IDNService Inherited;

    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    RTLaproGraphicOutput *rtOutput;
    IDNLaproGraConInlet *graConInlet;
    IDNLaproGraDisInlet *graDisInlet;

    IDNInlet *currentInlet;


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    IDNLaproService(uint8_t serviceID, char *serviceName, bool defaultServiceFlag, RTLaproGraphicOutput *rtOutput);
    virtual ~IDNLaproService();

    // -- Inherited Members -------------
    virtual uint8_t getServiceType();
    virtual int copyServiceName(char *bufferPtr, unsigned bufferSize);
    virtual bool handlesMode(uint8_t serviceMode);
    virtual IDNInlet *requestInlet(ODF_ENV *env, uint8_t serviceMode);
    virtual void releaseInlet(ODF_ENV *env, IDNInlet *inlet);
    virtual void housekeeping(ODF_ENV *env, bool shutdownFlag);
};


#endif

