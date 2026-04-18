// -------------------------------------------------------------------------------------------------
//  File AdapterBase.hpp
//
//  05/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


#ifndef ADAPTER_BASE_HPP
#define ADAPTER_BASE_HPP


// Standard libraries
#include <stdint.h>

// Project headers
#include "ODFTaxiBuffer.hpp"



// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class AdapterBase
{
    typedef struct
    {
        uintptr_t size;
        uintptr_t head;
        uintptr_t caret;
        uintptr_t tail;

        // Followed by the queue entries of type uintptr_t

    } QUEUE_BUFFER;


    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    QUEUE_BUFFER *headQueue;
    QUEUE_BUFFER *caretQueue;
    QUEUE_BUFFER *tailQueue;

    QUEUE_BUFFER *allocQueue(unsigned queueSize);


    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:

    // Called from server context only, implemented by derived class
    virtual int enable();
    virtual void disable();

    // Called from adapter context only, used by derived class
    virtual ODF_TAXI_BUFFER *peekCaret();
    virtual ODF_TAXI_BUFFER *readCaret();


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    AdapterBase();
    virtual ~AdapterBase();

    // Called from server context only
    virtual int start();
    virtual int stop(bool gracefulFlag);
    virtual int putBuffer(ODF_TAXI_BUFFER *taxiBuffer);
    virtual ODF_TAXI_BUFFER *getTrash();
    virtual void getName(char *nameBufferPtr, unsigned nameBufferSize);
};


#endif


/* 
 
---> atomic available since C++11 !!!
 
// Standard libraries
#include <atomic>
 
    std::atomic<uint32_t> latency;

    // Called from server and/or adapter context
    virtual int updateLatency(int32_t delta);
    uint32_t getLatency() { return latency.load(); }
 
*/
