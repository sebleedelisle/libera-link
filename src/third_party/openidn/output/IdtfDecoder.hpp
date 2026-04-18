#pragma once

#include "../shared/ISPDB25Point.h"
#include "RTLaproGraphOut.hpp"

class IdtfDecoder : public RTLaproDecoder
{
private:

    const int sampleSize = 8;

public:

    // -- Inherited Members -------------
    virtual unsigned getSampleSize();
    virtual void decode(uint8_t* dstPtr, uint8_t* srcPtr);
    virtual void decode(uint8_t* dstPtr, uint8_t* srcPtr, unsigned sampleCount);

};

