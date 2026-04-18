// -------------------------------------------------------------------------------------------------
//  File IDNLaproService.cpp
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


// Standard libraries
#include <string.h>

// Project headers
#include "idn-stream.h"
#include "../shared/ODFTools.hpp"
#include "PEVFlags.h"
#include "IDNLaproGraConInlet.hpp"
#include "IDNLaproGraDisInlet.hpp"

// Module header
#include "IDNLaproService.hpp"



// =================================================================================================
//  Class IDNLaproGraphInlet
//
// -------------------------------------------------------------------------------------------------
//  scope: protected
// -------------------------------------------------------------------------------------------------

int IDNLaproGraphInlet::createDecoder(unsigned serviceMode, void *paramPtr, unsigned paramLen)
{
    if(decoder != (RTLaproDecoder *)0) rtOutput->deleteDecoder(decoder);
    decoder = (RTLaproDecoder *)0; 

    // For laser projector graphic, empty configuration is invalid
    if(paramLen == 0) return -1;

    // Create the decoder
    decoder = rtOutput->createIDNDecoder(serviceMode, paramPtr, paramLen);
    if((decoder == (RTLaproDecoder *)0)) return -1;

    return 0;
}


void IDNLaproGraphInlet::invalidateConfig()
{
    Inherited::invalidateConfig();

    if(decoder != (RTLaproDecoder *)0) rtOutput->deleteDecoder(decoder);
    decoder = (RTLaproDecoder *)0; 
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

IDNLaproGraphInlet::IDNLaproGraphInlet(RTLaproGraphicOutput *rtOutput):
    rtOutput(rtOutput)
{
    decoder = (RTLaproDecoder *)0;
}


IDNLaproGraphInlet::~IDNLaproGraphInlet()
{
}


void IDNLaproGraphInlet::reset()
{
    Inherited::reset();

    if(decoder != (RTLaproDecoder *)0) rtOutput->deleteDecoder(decoder);
    decoder = (RTLaproDecoder *)0; 
}



// =================================================================================================
//  Class IDNLaproService
//
// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

IDNLaproService::IDNLaproService(uint8_t serviceID, char *serviceName, bool defaultServiceFlag, RTLaproGraphicOutput *rtOutput):
    IDNService(serviceID, serviceName, defaultServiceFlag),
    rtOutput(rtOutput)
{

// FIXME: Static allocation option
    graConInlet = new IDNLaproGraConInlet(rtOutput);
    graDisInlet = new IDNLaproGraDisInlet(rtOutput);

    currentInlet = (IDNInlet *)0;
}


IDNLaproService::~IDNLaproService()
{
}


uint8_t IDNLaproService::getServiceType()
{
    return (uint8_t)IDNVAL_STYPE_LAPRO;
}


int IDNLaproService::copyServiceName(char *bufferPtr, unsigned bufferSize)
{
    int nameLen = Inherited::copyServiceName(bufferPtr, bufferSize);
    if(nameLen >= 0) return nameLen;

    rtOutput->getDeviceName(bufferPtr, bufferSize);
    return strlen(bufferPtr);
}


bool IDNLaproService::handlesMode(uint8_t serviceMode)
{
    if(serviceMode == IDNVAL_SMOD_LPGRF_CONTINUOUS) return true;
    if(serviceMode == IDNVAL_SMOD_LPGRF_DISCRETE) return true;
    if(serviceMode == IDNVAL_SMOD_LPEFX_CONTINUOUS) return true;
    if(serviceMode == IDNVAL_SMOD_LPEFX_DISCRETE) return true;

    return Inherited::handlesMode(serviceMode);
}


IDNInlet *IDNLaproService::requestInlet(ODF_ENV *env, uint8_t serviceMode)
{
    if(currentInlet != (IDNInlet *)0) return (IDNInlet *)0;

    if(serviceMode == IDNVAL_SMOD_LPGRF_CONTINUOUS)
    {
        // Open the hardware output in continuous mode
        if(rtOutput->open(env, RTLaproGraphicOutput::OPMODE_WAVE) == 0)
        {
            currentInlet = graConInlet;
        }
    }
    else if(serviceMode == IDNVAL_SMOD_LPGRF_DISCRETE)
    {
        // Open the hardware output in discrete mode
        if(rtOutput->open(env, RTLaproGraphicOutput::OPMODE_FRAME) == 0)
        {
            currentInlet = graDisInlet;
        }
    }

    return currentInlet;
}


void IDNLaproService::releaseInlet(ODF_ENV *env, IDNInlet *inlet)
{
    TracePrinter tpr(env, "IDNLaproService~releaseInlet");

    if(inlet == (IDNInlet *)0)
    {
        tpr.logError("Invalid invocation (inlet == 0)");
        return;
    }
    else if(inlet != currentInlet)
    {
        tpr.logError("Invalid invocation (inlet pointer mismatch)");
        return;
    }

    // Reset the inlet
    inlet->reset();
    currentInlet = (IDNInlet *)0;

    // Close the hardware output
    rtOutput->close(env);
}


void IDNLaproService::housekeeping(ODF_ENV *env, bool shutdownFlag)
{
    rtOutput->housekeeping(env, shutdownFlag);
}

