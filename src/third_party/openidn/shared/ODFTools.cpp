// -------------------------------------------------------------------------------------------------
//  File ODFTools.cpp
//
//  OpenIDN DAC Framework tools
//
//  04/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


#include <stdio.h>
#include <stdarg.h>

// Module header
#include "ODFTools.hpp"



// =================================================================================================
//  Class TracePrinter
//
// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

TracePrinter::TracePrinter(ODF_ENV *env, const char *frameName)
{
    this->env = env;
}


TracePrinter::~TracePrinter()
{
}


void TracePrinter::logError(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);

    env->trace(ODF_TRACEOP_LOG_ERROR, format, ap);

    va_end(ap);
}


void TracePrinter::logWarn(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);

    env->trace(ODF_TRACEOP_LOG_WARN, format, ap);

    va_end(ap);
}


void TracePrinter::logInfo(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);

    env->trace(ODF_TRACEOP_LOG_INFO, format, ap);

    va_end(ap);
}


void TracePrinter::logDebug(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);

    env->trace(ODF_TRACEOP_LOG_DEBUG, format, ap);

    va_end(ap);
}

