
#ifndef IDN_LAPRO_DECODER_HPP
#define IDN_LAPRO_DECODER_HPP


#include "../output/RTLaproGraphOut.hpp"



struct IDNDescriptorTag {
  uint8_t type; 
  uint8_t precision; 
  uint8_t scannerId; 
  uint16_t wavelength; 
  struct IDNDescriptorTag *next;
};


// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class IDNLaproDecoder: public RTLaproDecoder
{
    typedef RTLaproDecoder Inherited;

    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    IDNDescriptorTag *firstDescriptor;
    unsigned sampleSize;

    unsigned int buildDictionary(uint8_t *buf, unsigned int len, unsigned int offset, uint8_t scwc, IDNDescriptorTag** data);
    void deleteDescriptorList();


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    IDNLaproDecoder();
    virtual ~IDNLaproDecoder();

    virtual int buildFrom(uint8_t serviceMode, void *paramPtr, unsigned paramLen);

    // -- Inherited Members -------------
    virtual unsigned getSampleSize();
    virtual void decode(uint8_t *dstPtr, uint8_t *srcPtr);
    virtual void decode(uint8_t *dstPtr, uint8_t *srcPtr, unsigned sampleCount);
};


#endif

