// -------------------------------------------------------------------------------------------------
//  File V1LaproGraphOut.cpp
//
//  Version 1 compatibility layer
//
//  01/2025 Dirk Apitz, moved to new output architecture
// -------------------------------------------------------------------------------------------------


// Module header
#include "V1LaproGraphOut.hpp"



// =================================================================================================
//  Class V1LaproGraphicOutput
//
// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

V1LaproGraphicOutput::V1LaproGraphicOutput(std::shared_ptr<LaproAdapter> adapter):
    STDLaproGraphicOutput(adapter.get())
{
}


V1LaproGraphicOutput::~V1LaproGraphicOutput()
{
}

