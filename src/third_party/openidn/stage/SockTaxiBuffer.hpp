// -------------------------------------------------------------------------------------------------
//  File SockTaxiBufferLRaw.hpp
//
//  Taxi buffer for socket-type (threaded) network interfaces
//
//  04/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


#ifndef SOCK_TAXI_BUFFER_HPP
#define SOCK_TAXI_BUFFER_HPP


// Standard libraries
#include <stdint.h>
#if defined(__APPLE__)
#include <stdlib.h>
#else
#include <malloc.h>
#endif



// Forward declarations
struct _ODF_TAXI_BUFFER;


// -------------------------------------------------------------------------------------------------
//  Typedefs
// -------------------------------------------------------------------------------------------------

typedef struct _ODF_TAXI_SOURCE
{
    virtual ~_ODF_TAXI_SOURCE() { };

    virtual struct _ODF_TAXI_BUFFER *allocTaxiBuffer(uint16_t payloadLen) = 0;
    virtual void freeTaxiBuffer(struct _ODF_TAXI_BUFFER *taxiBuffer) = 0;

} ODF_TAXI_SOURCE;


// OpenIDN uses taxi buffers as an abstraction for the underlaying network interface. They allow for
// dynamic allocation, passing and storage. This is necessary for reassembly and latency Queues.
// Please note that this struct shall not have derivations or virtual functions because of casts.
typedef struct _ODF_TAXI_BUFFER
{
    friend class SockIDNServer;

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    ODF_TAXI_SOURCE *taxiSource;

    struct _ODF_TAXI_BUFFER *next;

    uint16_t payloadLen;                            // Length of the payload (payloadPtr points to)
    void *payloadPtr;                               // Pointer to the message payload

    uint32_t sourceRefTime;                         // Reference time of being sourced/created

    void *memoBuffer[10];                           // Memo area. Note: Keep pointer alignment


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    int concat(struct _ODF_TAXI_BUFFER *taxiBuffer)
    {
        int totalLen = 0;
        struct _ODF_TAXI_BUFFER *buf = this;
        while(buf->next) { totalLen += (unsigned)(buf->payloadLen); buf = buf->next; }

        buf->next = taxiBuffer;
        while(buf) { totalLen += (unsigned)(buf->payloadLen); buf = buf->next; }

        return totalLen;
    }

    struct _ODF_TAXI_BUFFER *getNext()
    {
        return next;
    }

    void discard()
    {
        while(next != (struct _ODF_TAXI_BUFFER *)0)
        {
            struct _ODF_TAXI_BUFFER *buf = next;
            next = buf->next;
            taxiSource->freeTaxiBuffer(buf);
        }
        taxiSource->freeTaxiBuffer(this);
    }

    int coalesce(unsigned len)
    {
        return (payloadLen < len) ? -1 : 0;
    }

    void *getPayloadPtr()
    {
        return payloadPtr;
    }

    unsigned getFragmentLen()
    {
        return payloadLen;
    }

    unsigned getTotalLen()
    {
        unsigned totalLen = 0;
        struct _ODF_TAXI_BUFFER *buf = this;
        while(buf) { totalLen += (unsigned)(buf->payloadLen); buf = buf->next; }

        return totalLen;
    }

    void adjustFront(int offset)
    {
        // Negative: Drop header; Positive: Add header
        payloadPtr = (void *)((uintptr_t)payloadPtr - offset);
        payloadLen += offset;
    }

    void cropPayload(unsigned len)
    {
        payloadLen = len;
    }

    unsigned getMemoSize()
    {
        return sizeof(memoBuffer);
    }

    void *getMemoPtr()
    {
        return (void *)memoBuffer;
    }

    uint32_t getSourceRefTime()
    {
        return sourceRefTime;
    }

} ODF_TAXI_BUFFER;


#endif
