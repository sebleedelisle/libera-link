// -------------------------------------------------------------------------------------------------
//  File V1LaproGraphOut.hpp
//
//  Version 1 compatibility layer
//
//  01/2025 Dirk Apitz, moved to new output architecture
// -------------------------------------------------------------------------------------------------


#ifndef V1_LAPRO_GRAPHIC_OUTPUT_HPP
#define V1_LAPRO_GRAPHIC_OUTPUT_HPP


// Standard libraries
#include <memory>

// Project headers
#include "STDLaproGraphOut.hpp"



// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class V1LaproGraphicOutput: public STDLaproGraphicOutput
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    std::shared_ptr<LaproAdapter> adapterPtr = nullptr;


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    V1LaproGraphicOutput(std::shared_ptr<LaproAdapter> adapter);
    virtual ~V1LaproGraphicOutput();
};


#endif
