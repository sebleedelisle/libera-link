// -------------------------------------------------------------------------------------------------
//  File RTOutput.hpp
//
//  01/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


#ifndef RT_OUTPUT_HPP
#define RT_OUTPUT_HPP


// Project headers
#include "../shared/ODFEnvironment.hpp"
#include "../shared/IDNRealtimeStatus.hpp"



// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class RTOutput
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:

    unsigned pipelineEvents;

    virtual void reset();


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    RTOutput();
    virtual ~RTOutput();

    virtual void getDeviceName(char *nameBufferPtr, unsigned nameBufferSize) = 0;
    virtual unsigned clearPipelineEvents();
    virtual void getRealtimeStatus(IDNRealtimeStatus &status) const;

    virtual void housekeeping(ODF_ENV *env, bool shutdownFlag);
};


#endif
