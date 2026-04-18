// -------------------------------------------------------------------------------------------------
//  File STDLaproGraphOut.hpp
//
//  STanDard Laser Projector Graphic Output
//
//  01/2025 Dirk Apitz, moved to new output architecture
//  06/2025 Dirk Apitz, migrated from version 1 compatibility layer
// -------------------------------------------------------------------------------------------------


#ifndef STD_LAPRO_GRAPHIC_OUTPUT_HPP
#define STD_LAPRO_GRAPHIC_OUTPUT_HPP


// Project headers
#include "../shared/LaproAdapter.hpp"
#include "RTLaproGraphOut.hpp"



// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class STDLaproGraphicOutput: public RTLaproGraphicOutput
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    LaproAdapter *adapter;
    OPMODE opMode;

    void gracefulStop();
    unsigned recycle();


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    STDLaproGraphicOutput(LaproAdapter *adapter);
    virtual ~STDLaproGraphicOutput();

    // -- Inherited Members -------------
    virtual void getDeviceName(char *nameBufferPtr, unsigned nameBufferSize);
    virtual int open(ODF_ENV *env, OPMODE opMode);
    virtual void close(ODF_ENV *env);
    virtual void process(ODF_ENV *env, CHUNKDATA &chunkData, ODF_TAXI_BUFFER *taxiBuffer);
    virtual void housekeeping(ODF_ENV *env, bool shutdownFlag);
};


#endif
