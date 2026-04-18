// -------------------------------------------------------------------------------------------------
//  File ODFTools.hpp
//
//  OpenIDN DAC Framework tools
//
//  04/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------



#ifndef ODF_TOOLS_HPP
#define ODF_TOOLS_HPP


// Standard libraries
#include <stdint.h>

// Project headers
#include "ODFEnvironment.hpp"



// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class TracePrinter
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    ODF_ENV *env;


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:
    TracePrinter(ODF_ENV *env, const char *frameName);
    ~TracePrinter();

    void logError(const char *format, ...);
    void logWarn(const char *format, ...);
    void logInfo(const char *format, ...);
    void logDebug(const char *format, ...);
};


// -------------------------------------------------------------------------------------------------
//  Private inline functions
// -------------------------------------------------------------------------------------------------

static inline uint32_t btoh32(uint32_t value)
{
    uint32_t result = 0;
    uint8_t *p = (uint8_t *)&value;

    result |= (uint32_t)(*p++) << 24;
    result |= (uint32_t)(*p++) << 16;
    result |= (uint32_t)(*p++) << 8;
    result |= (uint32_t)(*p++);

    return result;
}


static inline uint16_t btoh16(uint16_t value)
{
    uint16_t result = 0;
    uint8_t *p = (uint8_t *)&value;

    result |= (uint16_t)(*p++) << 8;
    result |= (uint16_t)(*p++);

    return result;
}

// ----

static inline uint32_t htob32(uint32_t value)
{
    uint32_t result = 0;
    uint8_t *p = (uint8_t *)&result;

    *p++ = (uint8_t)(value >> 24);
    *p++ = (uint8_t)(value >> 16);
    *p++ = (uint8_t)(value >> 8);
    *p++ = (uint8_t)value;

    return result;
}


static inline uint16_t htob16(uint16_t value)
{
    uint16_t result = 0;
    uint8_t *p = (uint8_t *)&result;

    *p++ = (uint8_t)(value >> 8);
    *p++ = (uint8_t)value;

    return result;
}


#endif
