

// Standard libraries
#include <stdio.h>
#if defined(__APPLE__)
#include <stdlib.h>
#else
#include <malloc.h>
#endif

// Project headers
#include "../shared/ISPDB25Point.h"

// Module header
#include "IDNLaproDecoder.hpp"





#define IDN_DESCRIPTOR_NOP 0x00
#define IDN_DESCRIPTOR_DRAW_CONTROL_0 0x01
#define IDN_DESCRIPTOR_DRAW_CONTROL_1 0x02
#define IDN_DESCRIPTOR_X 0x03
#define IDN_DESCRIPTOR_Y 0x04
#define IDN_DESCRIPTOR_Z 0x05
#define IDN_DESCRIPTOR_COLOR 0x06
#define IDN_DESCRIPTOR_WAVELENGTH 0x07
#define IDN_DESCRIPTOR_INTENSITY 0x08
#define IDN_DESCRIPTOR_BEAM_BRUSH 0x09

#define IDN_TAG_CAT_BMASK 0xF000
#define IDN_TAG_CAT_OFFSET 12
#define IDN_TAG_SUB_BMASK 0x0F00
#define IDN_TAG_SUB_OFFSET 8
#define IDN_TAG_ID_BMASK 0x00F0
#define IDN_TAG_ID_OFFSET 4
#define IDN_TAG_PRM_BMASK 0x000F
#define IDN_TAG_WL_BMASK 0x03FF


/*
Laser configuration
*/

#define ISP_DB25_RED_WAVELENGTH  0x27E
#define ISP_DB25_GREEN_WAVELENGTH 0x214
#define ISP_DB25_BLUE_WAVELENGTH 0x1CC

#define ISP_DB25_USE_U1 0
#define ISP_DB25_USE_U2 0
#define ISP_DB25_USE_U3 0
#define ISP_DB25_USE_U4 0
#define ISP_DB25_U1_WAVELENGTH 0x1BD
#define ISP_DB25_U2_WAVELENGTH 0x241
#define ISP_DB25_U3_WAVELENGTH 0x1E8


// -------------------------------------------------------------------------------------------------
//  Tools
// -------------------------------------------------------------------------------------------------
/*
static void printDescrTag(struct IDNDescriptorTag *tag) {
  printf("TAG: %x, %x, %x, %x\n", tag->type, tag->precision, tag->scannerId, tag->wavelength);
}


static void print_hex_memory(void *mem, int len) {
  int i;
  unsigned char *p = (unsigned char *)mem;
  for (i=0;i<len;i++) {
    printf("0x%02x ", p[i]);
    if ((i%16==0) && i)
      printf("\n");
  }
  printf("\n");
}
*/

static unsigned int read_uint8(uint8_t *buf, unsigned int len, unsigned int offset, uint8_t* data) {
  if (len >= offset + 1) {
    *data = buf[offset];
    return offset + 1;
  }
  printf("[WAR] read err uint8, offset: %i, len: %i\n", offset, len);
  return offset;
}


static unsigned int read_uint16(uint8_t *buf, unsigned int len, unsigned int offset, uint16_t* data) {
  if (len >= offset + 2) {
    *data = ((buf[offset] << 8) & 0xff00) | (buf[offset+1] & 0xff);
    return offset + 2;
  }
  printf("[WAR] read err uint16, offset: %i, len: %i\n", offset, len);
  return offset;
}



// =================================================================================================
//  Class IDNLaproDecoder
//
// -------------------------------------------------------------------------------------------------
//  scope: private
// -------------------------------------------------------------------------------------------------

unsigned int IDNLaproDecoder::buildDictionary(uint8_t *buf, unsigned int len, unsigned int offset, uint8_t scwc, IDNDescriptorTag** data)
{
    IDNDescriptorTag *result = NULL;
    IDNDescriptorTag *last = NULL, *currentDescTag = NULL;

    int i;
    for (i = 0; i < scwc*4; i += 2) 
    {
        uint16_t tag;
        offset = read_uint16(buf, len, offset, &tag);

        uint16_t category = (tag & IDN_TAG_CAT_BMASK) >> IDN_TAG_CAT_OFFSET;
        uint16_t sub = (tag & IDN_TAG_SUB_BMASK) >> IDN_TAG_SUB_OFFSET;
        uint16_t id = (tag & IDN_TAG_ID_BMASK) >> IDN_TAG_ID_OFFSET;
        uint16_t prm = (tag & IDN_TAG_PRM_BMASK);
        uint16_t wl = (tag & IDN_TAG_WL_BMASK);

        switch(category) 
        {
            case 0: // 0
            offset += prm*2; //skip over prm 16-bit words
            i += prm*2;
            break;

            case 1: // 1
            if (sub == 0) 
            {
                //1.0
                //BREAK TAG
            } 
            else if (sub == 1)
            { 
                //1.1
                //COORDINATE AND COLOR SPACE MODIFIERS
            }
            break;

            case 4: //4
            if (sub == 0)
            { 
                //4.0
                if (id == 0) 
                { 
                    //4.0.0 - NOP
                    currentDescTag = (IDNDescriptorTag*)calloc(1, sizeof(IDNDescriptorTag));
                    currentDescTag->type = IDN_DESCRIPTOR_NOP;

                    //append Tag
                    if (last != NULL) last->next = currentDescTag;
                    last = currentDescTag;
                    if (result == NULL) result = last;
                    //end append
                } 
                else if (id == 1)
                { 
                    //4.0.1 - Precision Tag
                    if (last != NULL) last->precision++;
                }
            } 
            else if (sub == 1)
            {
                //4.1
                currentDescTag = (IDNDescriptorTag*)calloc(1, sizeof(IDNDescriptorTag));
                if (prm == 0) 
                {
                    currentDescTag->type = IDN_DESCRIPTOR_DRAW_CONTROL_0;
                } 
                else if (prm == 1) 
                {
                    currentDescTag->type = IDN_DESCRIPTOR_DRAW_CONTROL_1;
                }

                //apppend Tag
                if (last != NULL) last->next = currentDescTag;
                last = currentDescTag;
                if (result == NULL) result = last;
                //end append
            } 
            else if (sub == 2)
            { //4.2
                currentDescTag = (IDNDescriptorTag*)calloc(1, sizeof(IDNDescriptorTag));
                if (id == 0)
                { 
                    //4.2.0
                    currentDescTag->type = IDN_DESCRIPTOR_X;
                } 
                else if (id == 1) 
                {
                    //4.2.1
                    currentDescTag->type = IDN_DESCRIPTOR_Y;
                } 
                else if (id == 2) 
                { 
                    //4.2.2
                    currentDescTag->type = IDN_DESCRIPTOR_Z;
                }

                currentDescTag->scannerId = prm;

                //append Tag
                if (last != NULL) last->next = currentDescTag;
                last = currentDescTag;
                if (result == NULL) result = last;
                //end append
            }
            break;

            case 5: //5
            if (sub <= 3)
            { 
                //5.0 - 5.3
                currentDescTag = (IDNDescriptorTag*)calloc(1, sizeof(IDNDescriptorTag));
                currentDescTag->type = IDN_DESCRIPTOR_COLOR;
                currentDescTag->wavelength = wl;

                //append Tag
                if (last != NULL) last->next = currentDescTag;
                last = currentDescTag;
                if (result == NULL) result = last;
                //end append
            } 
            else if (sub == 12)
            { 
                //5.12
                currentDescTag = (IDNDescriptorTag*)calloc(1, sizeof(IDNDescriptorTag));

                //5.12.0
                if (id == 0) currentDescTag->type = IDN_DESCRIPTOR_WAVELENGTH;

                //5.12.1
                if (id == 1) currentDescTag->type = IDN_DESCRIPTOR_INTENSITY;

                //5.12.2
                if (id == 2) currentDescTag->type = IDN_DESCRIPTOR_BEAM_BRUSH;

                //append Tag
                if (last != NULL) last->next = currentDescTag;
                last = currentDescTag;
                if (result == NULL) result = last;
                //end append
            }
            break;
        }
    }

    last->next = NULL;
    *data = result;
    return offset;
}


void IDNLaproDecoder::deleteDescriptorList()
{
    IDNDescriptorTag *descriptor = firstDescriptor;
    while (descriptor != (IDNDescriptorTag *)0)
    {
        IDNDescriptorTag *nextDescriptor = descriptor->next;
        free(descriptor);
        descriptor = nextDescriptor;
    }

    firstDescriptor = (IDNDescriptorTag *)0;
    sampleSize = 0;
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

IDNLaproDecoder::IDNLaproDecoder()
{
    firstDescriptor = (IDNDescriptorTag *)0;
    sampleSize = 0;
}


IDNLaproDecoder::~IDNLaproDecoder()
{
    deleteDescriptorList();
}


int IDNLaproDecoder::buildFrom(uint8_t serviceMode, void *paramPtr, unsigned paramLen)
{
    deleteDescriptorList();
    buildDictionary((uint8_t *)paramPtr, paramLen, 0, paramLen / 4, &firstDescriptor);
    if (firstDescriptor == (IDNDescriptorTag *)0) return -1;

    IDNDescriptorTag *descriptor = firstDescriptor;
    while (descriptor != (IDNDescriptorTag *)0)
    {
        sampleSize += 1 + descriptor->precision;
        descriptor = descriptor->next;
    }

    return 0;
}


unsigned IDNLaproDecoder::getSampleSize()
{
    return sampleSize;
}


void IDNLaproDecoder::decode(uint8_t *dstPtr, uint8_t *srcPtr)
{
    unsigned srcLen = sampleSize;
    unsigned offset = 0;

    ISPDB25Point *dstPoint = (ISPDB25Point *)dstPtr;
    dstPoint->intensity = 0xFFFF; // Default lit, otherwise might not work if frame only specifies RGB

    uint8_t point_cscl = 0;
    uint8_t point_iscl = 0;

    //parse sample
    IDNDescriptorTag* tag = firstDescriptor;
    while (tag != NULL)
    {
        if (tag->type == IDN_DESCRIPTOR_NOP)
        {
            offset++;
        }
        if (tag->type == IDN_DESCRIPTOR_INTENSITY)
        {
            offset++;
			if (tag->precision == 1) offset++;
        }
		if (tag->type == IDN_DESCRIPTOR_BEAM_BRUSH)
        {
            offset++;
			if (tag->precision == 1) offset++;
        }
        if (tag->type == IDN_DESCRIPTOR_DRAW_CONTROL_0 || tag->type == IDN_DESCRIPTOR_DRAW_CONTROL_1)
        {
            uint8_t hint;
            offset = read_uint8(srcPtr, srcLen, offset, (uint8_t*)&(hint));
            point_cscl = (hint & 0xc0) >> 6;
            point_iscl = (hint & 0x30) >> 4;
        }
        if (tag->precision == 1)
        {
            if (tag->type == IDN_DESCRIPTOR_X)
            {
                if (tag->scannerId != 0)
                {
                    offset += 2;
                }
                else
                {
                    offset = read_uint16(srcPtr, srcLen, offset, &(dstPoint->x));
                    dstPoint->x += 0x8000;
                }
            }
            else if (tag->type == IDN_DESCRIPTOR_Y)
            {
                if (tag->scannerId != 0)
                {
                    offset += 2;
                }
                else
                {
                    offset = read_uint16(srcPtr, srcLen, offset, &(dstPoint->y));
                    dstPoint->y += 0x8000;
                }
            }
            else if (tag->type == IDN_DESCRIPTOR_COLOR)
            {
                if (tag->wavelength == ISP_DB25_RED_WAVELENGTH)
                {
                    offset = read_uint16(srcPtr, srcLen, offset, &(dstPoint->r));
                }
                else if (tag->wavelength == ISP_DB25_GREEN_WAVELENGTH)
                {
                    offset = read_uint16(srcPtr, srcLen, offset, &(dstPoint->g));
                }
                else if (tag->wavelength == ISP_DB25_BLUE_WAVELENGTH)
                {
                    offset = read_uint16(srcPtr, srcLen, offset, &(dstPoint->b));
                }
                else if (tag->wavelength == ISP_DB25_U1_WAVELENGTH)
                {
                    offset = read_uint16(srcPtr, srcLen, offset, &(dstPoint->u1));
                }
                else if (tag->wavelength == ISP_DB25_U2_WAVELENGTH)
                {
                    offset = read_uint16(srcPtr, srcLen, offset, &(dstPoint->u2));
                }
                else if (tag->wavelength == ISP_DB25_U3_WAVELENGTH)
                {
                    offset = read_uint16(srcPtr, srcLen, offset, &(dstPoint->u3));
                }
                else
                {
                    offset += 2;
                }
            }
            else if (tag->type == IDN_DESCRIPTOR_Z)
            {
                offset += 2;
            }
        }// end precision 1

        if (tag->precision == 0)
        {
            if (tag->type == IDN_DESCRIPTOR_X)
            {
                if (tag->scannerId != 0)
                {
                    offset++;
                }
                else
                {
                    offset = read_uint8(srcPtr, srcLen, offset, (uint8_t*)&(dstPoint->x));
                    dstPoint->x += 0x80;
                    dstPoint->x = ((dstPoint->x << 8) & 0xff00) | (dstPoint->x & 0x00ff);
                }
            }
            else if (tag->type == IDN_DESCRIPTOR_Y)
            {
                if (tag->scannerId != 0)
                {
                    offset++;
                }
                else
                {
                    offset = read_uint8(srcPtr, srcLen, offset, (uint8_t*)&(dstPoint->y));
                    dstPoint->y += 0x80;
                    dstPoint->y = ((dstPoint->y << 8) & 0xff00) | (dstPoint->y & 0x00ff);
                }
            }
            else if (tag->type == IDN_DESCRIPTOR_COLOR)
            {
                if (tag->wavelength == ISP_DB25_RED_WAVELENGTH)
                {
                    offset = read_uint8(srcPtr, srcLen, offset, (uint8_t*)&(dstPoint->r));
                    dstPoint->r = ((dstPoint->r << 8) & 0xff00) | (dstPoint->r & 0x00ff);
                }
                else if (tag->wavelength == ISP_DB25_GREEN_WAVELENGTH)
                {
                    offset = read_uint8(srcPtr, srcLen, offset, (uint8_t*)&(dstPoint->g));
                    dstPoint->g = ((dstPoint->g << 8) & 0xff00) | (dstPoint->g & 0x00ff);
                }
                else if (tag->wavelength == ISP_DB25_BLUE_WAVELENGTH)
                {
                    offset = read_uint8(srcPtr, srcLen, offset, (uint8_t*)&(dstPoint->b));
                    dstPoint->b = ((dstPoint->b << 8) & 0xff00) | (dstPoint->b & 0x00ff);
                }
                else if (tag->wavelength == ISP_DB25_U1_WAVELENGTH)
                {
                    offset = read_uint8(srcPtr, srcLen, offset, (uint8_t*)&(dstPoint->u1));
                    dstPoint->u1 = ((dstPoint->u1 << 8) & 0xff00) | (dstPoint->u1 & 0x00ff);
                }
                else if (tag->wavelength == ISP_DB25_U2_WAVELENGTH)
                {
                    offset = read_uint8(srcPtr, srcLen, offset, (uint8_t*)&(dstPoint->u2));
                    dstPoint->u2 = ((dstPoint->u2 << 8) & 0xff00) | (dstPoint->u2 & 0x00ff);
                }
                else if (tag->wavelength == ISP_DB25_U3_WAVELENGTH)
                {
                    offset = read_uint8(srcPtr, srcLen, offset, (uint8_t*)&(dstPoint->u3));
                    dstPoint->u3 = ((dstPoint->u3 << 8) & 0xff00) | (dstPoint->u3 & 0x00ff);
                }
                else
                {
                    offset++;
                }
            }
            else if (tag->type == IDN_DESCRIPTOR_Z)
            {
                offset++;
            }
        } //end precision 0

        tag = tag->next;

    } // end while tag

    // Scale colors
    if (point_cscl > 0)
    {
        dstPoint->r >>= 2 * point_cscl;
        dstPoint->g >>= 2 * point_cscl;
        dstPoint->b >>= 2 * point_cscl;
    }

    // Scale intensity
    if (point_iscl > 0)
    {
        dstPoint->intensity >>= 2 * point_iscl;
    }
}


void IDNLaproDecoder::decode(uint8_t *dstPtr, uint8_t *srcPtr, unsigned sampleCount)
{
    // Copy sample data
    for(; sampleCount > 0; sampleCount--)
    {
        decode(dstPtr, srcPtr);

        // Next point
        dstPtr = &dstPtr[sizeof(ISPDB25Point)];
        srcPtr = &srcPtr[sampleSize];
    }
}
