// -------------------------------------------------------------------------------------------------
//  File RTLaproGraphOut.cpp
//
//  01/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


// Project headers
#include "IDNLaproDecoder.hpp"

// Module header
#include "RTLaproGraphOut.hpp"



// =================================================================================================
//  Class RTLaproDecoder
//
// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

RTLaproDecoder::RTLaproDecoder()
{
}


RTLaproDecoder::~RTLaproDecoder()
{
}



// =================================================================================================
//  Class RTLaproGraphicOutput
//
// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

RTLaproGraphicOutput::RTLaproGraphicOutput()
{
}


RTLaproGraphicOutput::~RTLaproGraphicOutput()
{
}


RTLaproDecoder *RTLaproGraphicOutput::createIDNDecoder(uint8_t serviceMode, void *paramPtr, unsigned paramLen)
{
    IDNLaproDecoder *idnDecoder = new IDNLaproDecoder();
    if (idnDecoder->buildFrom(serviceMode, paramPtr, paramLen) < 0)
    {
        delete idnDecoder;
        return (RTLaproDecoder *)0;
    }

    return idnDecoder;
}


void RTLaproGraphicOutput::deleteDecoder(RTLaproDecoder *decoder)
{
    decoder->refDec();
}

