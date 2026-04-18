// -------------------------------------------------------------------------------------------------
//  File ODFEnvironment.hpp
//
//  OpenIDN DAC Framework environment
//
//  04/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


#ifndef ODF_ENVIRONMENT_HPP
#define ODF_ENVIRONMENT_HPP


// Standard libraries
#include <stdint.h>
#include <stdarg.h>



// -------------------------------------------------------------------------------------------------
//  Defines
// -------------------------------------------------------------------------------------------------

#define ODF_TRACEOP_LOG_FATAL               0x04    // Severe error, will prevent from continuing
#define ODF_TRACEOP_LOG_ERROR               0x06    // Error from API invocation (usually resolved)
#define ODF_TRACEOP_LOG_WARN                0x08    // Anomalies/Conflicts in processing/stream/structures
#define ODF_TRACEOP_LOG_INFO                0x0A    // Information that highlight the progress
#define ODF_TRACEOP_LOG_DEBUG               0x0C    // Fine-Grained info (timing impact - use with care)


// -------------------------------------------------------------------------------------------------
//  Typedefs
// -------------------------------------------------------------------------------------------------

typedef struct _ODF_ENV
{
    virtual ~_ODF_ENV() { };

    virtual void trace(int traceOp, const char *format, va_list ap) = 0;

    virtual uint32_t getClockUS() = 0;

} ODF_ENV;


#endif
