// -------------------------------------------------------------------------------------------------
//  File DecoderBase.cpp
//
//  05/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


// Module header
#include "DecoderBase.hpp"



// =================================================================================================
//  Class DecoderBase
//
// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

DecoderBase::DecoderBase()
{
    refCount = 1;
}


DecoderBase::~DecoderBase()
{
}


void DecoderBase::refInc()
{
    if(refCount < 0xFFFFFFFF)
    {
        refCount++;
    }
    else
    {
        // Error: Overrun
    }
}


void DecoderBase::refDec()
{
    if(refCount > 1)
    {
        refCount--;
    }
    else
    {
        // Last reference - delete the instance
        delete this;
    }
}

