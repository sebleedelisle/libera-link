#ifndef IDN_REALTIME_STATUS_HPP
#define IDN_REALTIME_STATUS_HPP


// Standard libraries
#include <stdint.h>


struct IDNRealtimeStatus
{
    bool sessionHasMessages = false;
    bool devicesOccupyBuffers = false;
    bool latencyValid = false;
    uint32_t latencyUS = 0;
};


#endif
