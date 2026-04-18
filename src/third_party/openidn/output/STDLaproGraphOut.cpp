// -------------------------------------------------------------------------------------------------
//  File STDLaproGraphOut.cpp
//
//  STanDard Laser Projector Graphic Output
//
//  01/2025 Dirk Apitz, moved to new output architecture
//  06/2025 Dirk Apitz, migrated from version 1 compatibility layer
// -------------------------------------------------------------------------------------------------


// Standard libraries
#include <vector>
#include <string.h>

// Project headers
#include "../shared/ODFTools.hpp"

// Module header
#include "STDLaproGraphOut.hpp"



// =================================================================================================
//  Class STDLaproGraphicOutput
//
// -------------------------------------------------------------------------------------------------
//  scope: private
// -------------------------------------------------------------------------------------------------

void STDLaproGraphicOutput::gracefulStop()
{
    // Try a graceful stop
    if(adapter->stop(true) > 0)
    {
        // Adapter is stopped -> set output idle
        opMode = OPMODE_IDLE;
    }
    else
    {
        // Adapter busy -> set output flushing
        opMode = OPMODE_FLUSHING;
    }
}


unsigned STDLaproGraphicOutput::recycle()
{
    unsigned result = 0;
    while(1)
    {
        ODF_TAXI_BUFFER *taxiBuffer = adapter->getTrash();
        if(taxiBuffer == (ODF_TAXI_BUFFER *)0) break;

        // Get the pointer to the taxi buffer memo area
        LAPRO_CHUNK_MEMO *memo = (LAPRO_CHUNK_MEMO *)(taxiBuffer->getMemoPtr());

        // Decrement decoder reference count. Note: A reference count of 0 deletes the decoder
        if(memo->decoder != (DecoderBase *)0) (memo->decoder)->refDec();

        // Discard the taxi buffer
        taxiBuffer->discard();

        // Update number of recycled buffers
        result++;
    }

    return result;
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

STDLaproGraphicOutput::STDLaproGraphicOutput(LaproAdapter *adapter)
{
    this->adapter = adapter;

    opMode = OPMODE_IDLE;
}


STDLaproGraphicOutput::~STDLaproGraphicOutput()
{
}


void STDLaproGraphicOutput::getDeviceName(char *nameBufferPtr, unsigned nameBufferSize)
{
    adapter->getName(nameBufferPtr, nameBufferSize);
}


int STDLaproGraphicOutput::open(ODF_ENV *env, OPMODE opMode)
{
    TracePrinter tpr(env, "STDLaproGraphicOutput~open");

    // Error in case already active
    if(this->opMode > OPMODE_FLUSHING)
    {
        tpr.logError("LaproGraphicOutput: Duplicate open opMode=%d", opMode);
        return -1;
    }

    // Check the new mode
    if(opMode == OPMODE_WAVE)
    {
        tpr.logInfo("LaproGraphicOutput: Open in wave mode");
    }
    else if(opMode == OPMODE_FRAME)
    {
        tpr.logInfo("LaproGraphicOutput: Open in frame mode");
    }
    else
    {
        tpr.logError("LaproGraphicOutput: Invalid open mode parameter %d", opMode);
        return -1;
    }

    // Start the adapter (only) in case idle. Note: Flushing did not stop the adapter.
    if(this->opMode == OPMODE_IDLE)
    {
        adapter->start();
    }

    // Set the new operation mode
    this->opMode = opMode;

    // Success
    return 0;
}


void STDLaproGraphicOutput::close(ODF_ENV *env)
{
    TracePrinter tpr(env, "STDLaproGraphicOutput~close");

    // Error in case not active
    if(this->opMode <= OPMODE_FLUSHING)
    {
        tpr.logError("LaproGraphicOutput: Duplicate close");
        return;
    }

    // Try to stop the adapter and recycle trash
    gracefulStop();
    unsigned recycleCnt = recycle();

    if(opMode == OPMODE_IDLE) tpr.logInfo("LaproGraphicOutput: Closed (idle), recycleCnt=%u", recycleCnt);
    else tpr.logInfo("LaproGraphicOutput: Closed (flushing), recycleCnt=%u", recycleCnt);
}


void STDLaproGraphicOutput::process(ODF_ENV *env, CHUNKDATA &chunkData, ODF_TAXI_BUFFER *taxiBuffer)
{
    TracePrinter tpr(env, "STDLaproGraphicOutput~process");

    // Call parameter validation
    if(taxiBuffer == (ODF_TAXI_BUFFER *)0) return;

    // Check for enough space in taxi buffer memo area
    if(taxiBuffer->getMemoSize() < sizeof(LAPRO_CHUNK_MEMO))
    {
        // Note: Unlikely. For setup/debugging.
        tpr.logError("LaproGraphicOutput: Insufficient taxi buffer memo area");
        taxiBuffer->discard();
        return;
    }

    // Populate taxi buffer memo area
    LAPRO_CHUNK_MEMO *memo = (LAPRO_CHUNK_MEMO *)(taxiBuffer->getMemoPtr());
    memset(memo, 0, sizeof(LAPRO_CHUNK_MEMO));
    memo->duration = chunkData.chunkDuration;
    memo->sampleCount = chunkData.sampleCount;

    // Set decoder and increment reference count. Note: A reference count of 0 deletes the decoder
    memo->decoder = chunkData.decoder;
    if(memo->decoder != (DecoderBase *)0) (memo->decoder)->refInc();

    do
    {
        // Populate the chunk type
        if(opMode == OPMODE_WAVE)
        {
            memo->type = LAPRO_CHUNK_TYPE_WAVE;

            if (chunkData.modFlags & MODFLAG_DISCONTINUOUS)
                memo->isDiscontinuous = true;
            
        }
        else if(opMode == OPMODE_FRAME)
        {
            if(chunkData.modFlags & MODFLAG_SCAN_ONCE)
            {
                memo->type = LAPRO_CHUNK_TYPE_FRAME_ONCE;
            }
            else
            {
                memo->type = LAPRO_CHUNK_TYPE_FRAME_RPT;
            }
        }
        else
        {
            // Note: Unlikely. For setup/debugging.
            tpr.logError("LaproGraphicOutput: Invalid mode/type");
            break;
        }

        // Push buffer into adapter queue
        int rcPush = adapter->putBuffer(taxiBuffer);
        if(rcPush != 0)
        {
            // Note: Unlikely. Would be an out of memory condition or unaligned pointer.
            tpr.logError("LaproGraphicOutput: Adapter push error %d", rcPush);
            break;
        }

        // Everything OK, the taxi buffer has been passed to the adapter - no more access!!
        taxiBuffer = (ODF_TAXI_BUFFER *)0;
    }
    while(0);

    // Discard taxi buffer in case of errors
    if(taxiBuffer != (ODF_TAXI_BUFFER *)0)
    {
        // Decrement decoder reference count. Note: A reference count of 0 deletes the decoder
        if(memo->decoder != (DecoderBase *)0) (memo->decoder)->refDec();

        // Discard the taxi buffer
        taxiBuffer->discard();
    }

    // Recycle taxi buffers that were processed by the adapter
    recycle();
}


void STDLaproGraphicOutput::housekeeping(ODF_ENV *env, bool shutdownFlag)
{
    // Nothing to do in case idle
    if(opMode == OPMODE_IDLE) return;

    // Not idle: Check for adapter stop
    if(shutdownFlag)
    {
        // Shutdown: Force immediate stop
        adapter->stop(false);
    }
    else if(opMode == OPMODE_FLUSHING)
    {
        // Regular housekeeping: Retry graceful stop
        gracefulStop();
    }

    // Not idle: Recycle taxi buffers that were processed by the adapter
    recycle();
}

