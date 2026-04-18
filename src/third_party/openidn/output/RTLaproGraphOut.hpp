// -------------------------------------------------------------------------------------------------
//  File RTLaproGraphOut.hpp
//
//  01/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


#ifndef RT_LAPRO_GRAPHIC_OUTPUT_HPP
#define RT_LAPRO_GRAPHIC_OUTPUT_HPP


// Standard libraries
#include <stdint.h>

// Project headers
#include "../shared/ODFTaxiBuffer.hpp"
#include "../shared/DecoderBase.hpp"
#include "RTOutput.hpp"



// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class RTLaproDecoder: public DecoderBase
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    RTLaproDecoder();
    virtual ~RTLaproDecoder();

    virtual unsigned getSampleSize() = 0;
    virtual void decode(uint8_t *dstPtr, uint8_t *srcPtr) = 0;
    virtual void decode(uint8_t *dstPtr, uint8_t *srcPtr, unsigned sampleCount) = 0;
};


class RTLaproGraphicOutput: public RTOutput
{
    public:

    enum OPMODE  { OPMODE_IDLE = 0, OPMODE_FLUSHING = 1, OPMODE_WAVE = 2, OPMODE_FRAME = 3 };
    enum MODFLAG { MODFLAG_SCAN_ONCE = 1, MODFLAG_DISCONTINUOUS = 2 };

    typedef struct
    {
        RTLaproDecoder *decoder;

        uint32_t chunkDuration;
        uint32_t sampleCount;

        uint8_t modFlags;

    } CHUNKDATA;


    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    RTLaproGraphicOutput();
    virtual ~RTLaproGraphicOutput();

    virtual int open(ODF_ENV *env, OPMODE opMode) = 0;
    virtual void close(ODF_ENV *env) = 0;

    virtual RTLaproDecoder *createIDNDecoder(uint8_t serviceMode, void *paramPtr, unsigned paramLen);
    virtual void deleteDecoder(RTLaproDecoder *decoder);

    virtual void process(ODF_ENV *env, CHUNKDATA &chunkData, ODF_TAXI_BUFFER *taxiBuffer) = 0;
};


#endif

