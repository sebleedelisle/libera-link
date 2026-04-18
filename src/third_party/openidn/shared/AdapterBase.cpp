// -------------------------------------------------------------------------------------------------
//  File AdapterBase.cpp
//
//  05/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


// Standard libraries
#if __cplusplus >= 201103L
#include <atomic>
#endif
#include <cstdio>

// Module header
#include "AdapterBase.hpp"



// =================================================================================================
//  Class AdapterBase
//
// -------------------------------------------------------------------------------------------------
//  scope: protected
// -------------------------------------------------------------------------------------------------

AdapterBase::QUEUE_BUFFER *AdapterBase::allocQueue(unsigned queueSize)
{
    // Alloc the queue buffer
    unsigned allocSize = sizeof(QUEUE_BUFFER) + (queueSize * sizeof(uintptr_t));
    QUEUE_BUFFER *queue = (QUEUE_BUFFER *)malloc(allocSize);
    if(queue == (QUEUE_BUFFER *)0) return (QUEUE_BUFFER *)0;
    if(((uintptr_t)queue & 0x1) != 0) { free(queue); return (QUEUE_BUFFER *)0; }

    // Populate queue buffer
    queue->size = queueSize;
    queue->head = queue->caret = queue->tail = 0;

    return queue;
}


// -------------------------------------------------------------------------------------------------
//  scope: protected
// -------------------------------------------------------------------------------------------------

int AdapterBase::enable()
{
    // Note: Called from server context only !!
    // -------------------------------------------------------------------------

    return 1;
}


void AdapterBase::disable()
{
    // Note: Called from server context only !!
    // -------------------------------------------------------------------------
}


ODF_TAXI_BUFFER *AdapterBase::peekCaret()
{
    // Note: Called from adapter context only !!
    // -------------------------------------------------------------------------

    // This might be the first use of the caret queue pointer in this context (on this core) after
    // a start. The server context might have changed the caretQueue pointer and/or caret element
    // index - and the change might not be visible in the current thread yet.

    // Make all writes in other threads visible in the current thread (process cache invalidate queue)
#if __cplusplus >= 201103L
    atomic_thread_fence(std::memory_order_acquire);
#endif

    while(1)
    {
        if(caretQueue == (QUEUE_BUFFER *)0) return (ODF_TAXI_BUFFER *)0;
        if(caretQueue->caret == caretQueue->head) return (ODF_TAXI_BUFFER *)0;

        // Now, since the head changed (and the change of the head is done last), make sure, that
        // there is no stale data in the cache (neither queue nor buffer data)

        // Make all writes in other threads visible in the current thread (process cache invalidate queue)
#if __cplusplus >= 201103L
        atomic_thread_fence(std::memory_order_acquire);
#endif

        // Get the next item from the queue
        uintptr_t entry = ((uintptr_t *)&caretQueue[1])[caretQueue->caret];

        // In case of a regular entry - return the item
        if((entry & 0x1) == 0) return (ODF_TAXI_BUFFER *)entry;

        // The item is a new queue. Move (old queue) caret and replace the _caret_ queue
        caretQueue->caret = (caretQueue->caret + 1) % caretQueue->size;
        caretQueue = (QUEUE_BUFFER *)(entry & ~0x1);
    }
}


ODF_TAXI_BUFFER *AdapterBase::readCaret()
{
    // Note: Called from adapter context only !!
    // -------------------------------------------------------------------------

    ODF_TAXI_BUFFER *result = peekCaret();

    // In case not empty: Move the caret to the next item
    if(result != (ODF_TAXI_BUFFER *)0)
    {
        caretQueue->caret = (caretQueue->caret + 1) % caretQueue->size;

        // All modifications are done and in correct sequence. Making the queue update visible
        // in other threads explicitly is optional but prevents from delays.

        // Make all writes in the current thread visible in other threads
#if __cplusplus >= 201103L
        atomic_thread_fence(std::memory_order_release);
#endif
    }

    return result;
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

AdapterBase::AdapterBase()
{
    headQueue = caretQueue = tailQueue = allocQueue(16);
}


AdapterBase::~AdapterBase()
{
    // Just free the head queue (should be the same as caret/tail queue and should be empty).
    // Note: The user is responsible for removing all items from the queue !!
    if(headQueue != (QUEUE_BUFFER *)0) free(headQueue);
}


int AdapterBase::start()
{
    // Note: Called from server context only !!
    // -------------------------------------------------------------------------

    // Enable adapter handling (add to scheduling)
    return enable();
}


int AdapterBase::stop(bool gracefulFlag)
{
    // Note: Called from server context only !!
    // -------------------------------------------------------------------------

    // In case of graceful stop: Check for non-empty queue
    if((gracefulFlag == true) && (caretQueue != (QUEUE_BUFFER *)0))
    {
        if(caretQueue != headQueue) return 0;
        if(headQueue->caret != headQueue->head) return 0;
    }

    // Disable adapter handling (remove from scheduling)
    disable();

    // Note: Adapter is stopped and not accessing the queue any more!

    // Empty the queue. Just discard the caret (by replacing it with the head)
    caretQueue = headQueue;
    if(caretQueue != (QUEUE_BUFFER *)0) caretQueue->caret = caretQueue->head;

    // Make all writes in the current thread visible in other threads
#if __cplusplus >= 201103L
    atomic_thread_fence(std::memory_order_release);
#endif

    // Success
    return 1;
}


int AdapterBase::putBuffer(ODF_TAXI_BUFFER *taxiBuffer)
{
    // Note: Called from server context only !!
    // -------------------------------------------------------------------------

    // Check for valid parameter. Note: Assuming aligment - LSB used to mark a queue pointer
    if(taxiBuffer == (ODF_TAXI_BUFFER *)0) return -1;
    if(((uintptr_t)taxiBuffer & 0x1) != 0) return -1;

    // Check for valid queue
    if(headQueue == (QUEUE_BUFFER *)0) return -1;

    // In case full, allocate new queue and set as last item.
    // Note: A ring buffer has always an empty slot to mark head == tail as empty
    if(((headQueue->head + 2) % headQueue->size) == headQueue->tail)
    {
        QUEUE_BUFFER *queue = allocQueue(headQueue->size * 2);
        if(queue == (QUEUE_BUFFER *)0) return -1;

        // Set new queue as last item.
        ((uintptr_t *)&headQueue[1])[headQueue->head] = (uintptr_t)queue | 0x1;

        // Make all writes in the current thread visible in other threads
#if __cplusplus >= 201103L
        atomic_thread_fence(std::memory_order_release);
#endif

        // Finally: Move the head of the old queue (to the the pointer to the next queue).
        headQueue->head = (headQueue->head + 1) % headQueue->size;

        // Replace the _head_ (write) queue
        headQueue = queue;
    }

    // Add the item to the queue
    ((uintptr_t *)&headQueue[1])[headQueue->head] = (uintptr_t)taxiBuffer;

    // Make all writes in the current thread visible in other threads
#if __cplusplus >= 201103L
    atomic_thread_fence(std::memory_order_release);
#endif

    // Finally: Move the head.
    headQueue->head = (headQueue->head + 1) % headQueue->size;

    // All modifications are done and in correct sequence. Making the queue update visible
    // in other threads explicitly is optional but prevents from delays.

    // Make all writes in the current thread visible in other threads
#if __cplusplus >= 201103L
    atomic_thread_fence(std::memory_order_release);
#endif

    // Success
    return 0;
}


ODF_TAXI_BUFFER *AdapterBase::getTrash()
{
    // Note: Called from server context only !!
    // -------------------------------------------------------------------------

    while(1)
    {
        // The caret might have been mofified in adapter context. Making the queue update visible
        // in the currebt thread explicitly is optional but prevents from delays.

        // Make all writes in other threads visible in the current thread (process cache invalidate queue)
#if __cplusplus >= 201103L
        atomic_thread_fence(std::memory_order_acquire);
#endif

        if(tailQueue == (QUEUE_BUFFER *)0) return (ODF_TAXI_BUFFER *)0;
        if(tailQueue->tail == tailQueue->caret) return (ODF_TAXI_BUFFER *)0;

        // Note: No fence needed. Neither queue content nor buffer data is modified by the adapter.
        // Only the caret is modified - and only the caret is used. When the caret change becomes
        // visible in server context, the tail buffer can be removed from the queue.

        // Get the next item from the queue
        uintptr_t entry = ((uintptr_t *)&tailQueue[1])[tailQueue->tail];
        tailQueue->tail = (tailQueue->tail + 1) % tailQueue->size;

        // In case of a regular entry - return the item
        if((entry & 0x1) == 0) return (ODF_TAXI_BUFFER *)entry;

        // The item is a new queue. Free/Replace the _tail_ (read) queue
        free(tailQueue);
        tailQueue = (QUEUE_BUFFER *)(entry & ~0x1);
    }
}


void AdapterBase::getName(char *nameBufferPtr, unsigned nameBufferSize)
{
    snprintf(nameBufferPtr, nameBufferSize, "%s", "");
}


/* 
 
---> atomic available since C++11 !!!
 
    latency.store(0);
 
     // Reset members
    latency.store(0);



int AdapterBase::updateLatency(int32_t delta)
{
    int result = 0;

    while(1)
    {
        uint32_t expected = latency.load();

        uint32_t desired = expected + delta;
        if((delta >= 0) && (desired < expected))
        {
            // Error: Overrun
            desired = 0xFFFFFFFF;
            result = -1;
        }
        else if((delta < 0) && (desired > expected))
        {
            // Error: Underrun
            desired = 0;
            result = -1;
        }

        if(latency.compare_exchange_strong(expected, desired)) break;
    }

    return result;
}

*/
