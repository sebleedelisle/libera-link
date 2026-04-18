

#include <stdio.h>
#include <math.h>

#include "../output/RTLaproGraphOut.hpp"
#include "DACHWInterface.hpp"


void DACHWInterface::commitChunk(TransformEnv &tfEnv, std::shared_ptr<SliceBuf> &sliceBuf)
{
    if(tfEnv.db25Accu.size() != 0) 
    {
        //if current slice is full, convert it to bytes, reset and reset the currentSliceTime
        std::shared_ptr<TimeSlice> newSlice(new TimeSlice);
        newSlice->dataChunk = convertPoints(tfEnv.db25Accu);
        newSlice->durationUs = tfEnv.usPerSlice - tfEnv.currentSliceTime;
        sliceBuf->push_back(newSlice);
        tfEnv.db25Accu.clear();
        tfEnv.currentSliceTime = tfEnv.usPerSlice;
    }
}


int DACHWInterface::enable()
{
    // Note: Called from server context !!
    // -------------------------------------------------------------------------

    cmdMutex.lock();
    enabledFlag = true;
    cmdMutex.unlock();

    return 1;
}


void DACHWInterface::disable()
{
    // Note: Called from server context !!
    // -------------------------------------------------------------------------

    cmdMutex.lock();
    enabledFlag = false;
    cmdMutex.unlock();
}


int DACHWInterface::putBuffer(ODF_TAXI_BUFFER *taxiBuffer)
{
    // Note: Called from server context !!
    // -------------------------------------------------------------------------

    if(enabledFlag == false)
    {
        return -1;
    }
    else
    {
        return Inherited::putBuffer(taxiBuffer);
    }
}


std::shared_ptr<SliceBuf> DACHWInterface::getNextBuffer(TransformEnv &tfEnv, unsigned &driverMode)
{
    // Note: Called from adapter context !!
    // -------------------------------------------------------------------------

    std::shared_ptr<SliceBuf> sliceBuf(new SliceBuf);

    while(1)
    {
        std::vector<ISPDB25Point> db25Samples;
        uint32_t duration;
        bool isWave = false;
        bool scanOnce = false;
        bool isDiscontinuous = false;

        // Get the next taxi buffer. Keep the mutex for as long as the pointer is valid !
        cmdMutex.lock();
        do
        {
            // In case not enabled: Reset processing environment and set driver inactive
            if(!enabledFlag)
            {
                tfEnv.db25Accu.clear();
                driverMode = DRIVER_INACTIVE;
            }

            // Get the next buffer. Just peek - Do not move the caret (yet) !!
            ODF_TAXI_BUFFER *taxiBuffer = peekCaret();
            if(taxiBuffer == (ODF_TAXI_BUFFER *)0) break;

            // Get the pointer to the taxi buffer memo area
            LAPRO_CHUNK_MEMO *memo = (LAPRO_CHUNK_MEMO *)(taxiBuffer->getMemoPtr());
            RTLaproDecoder *decoder = (RTLaproDecoder *)memo->decoder;
            duration = memo->duration;
            isWave = (memo->type == LAPRO_CHUNK_TYPE_WAVE);
            scanOnce = (memo->type == LAPRO_CHUNK_TYPE_FRAME_ONCE);
            isDiscontinuous = memo->isDiscontinuous;

            // Decode the input samples into the internally used struct
            unsigned sampleSize = decoder->getSampleSize();
            db25Samples.resize(memo->sampleCount);
            ODF_TAXI_BUFFER *fragBuf = taxiBuffer;
            uint8_t *srcPtr = (uint8_t *)fragBuf->getPayloadPtr();
            unsigned srcLen = fragBuf->getFragmentLen();
            uint8_t *dstPtr = (uint8_t *)db25Samples.data();
            while(fragBuf != (ODF_TAXI_BUFFER *)0)
            {
                unsigned fragSampleCount = srcLen / sampleSize;
                decoder->decode(dstPtr, srcPtr, fragSampleCount);
                srcPtr = &srcPtr[fragSampleCount * sampleSize];
                srcLen -= fragSampleCount * sampleSize;
                dstPtr = &dstPtr[fragSampleCount * sizeof(ISPDB25Point)];

                // Check for last sample in the fragment spanning across two data fragments
                if(srcLen != 0)
                {
                    // Copy the remainder of the fragment
                    uint8_t sample[sampleSize];
                    memcpy(sample, srcPtr, srcLen);
                    uint8_t *contPtr = &sample[srcLen];
                    unsigned leftover = sampleSize - srcLen;

                    // Get the next fragment, abort in case of short data
                    fragBuf = fragBuf->getNext();
                    if(fragBuf == (ODF_TAXI_BUFFER *)0) break;

                    // Check for the rest of the sample, abort in case of short data
                    srcPtr = (uint8_t *)fragBuf->getPayloadPtr();
                    srcLen = fragBuf->getFragmentLen();
                    if(srcLen < leftover) break;

                    // Copy the remainder of the sample
                    memcpy(contPtr, srcPtr, leftover);
                    srcPtr = &srcPtr[leftover];
                    srcLen -= leftover;

                    // Decode the sample
                    decoder->decode(dstPtr, sample);
                    dstPtr = &dstPtr[sizeof(ISPDB25Point)];
                }
                else
                {
                    fragBuf = fragBuf->getNext();
                    if(fragBuf != (ODF_TAXI_BUFFER *)0)
                    {
                        srcPtr = (uint8_t *)fragBuf->getPayloadPtr();
                        srcLen = fragBuf->getFragmentLen();
                    }
                }
            }

            // Now, move the caret and log error in case of mismatch (Unlikely, would be a bug).
            // No more taxi buffer access! The tail follows the caret and the buffer will be discarded.
            if(readCaret() != taxiBuffer)
            {
                printf("peekCaret()/readCaret() mismatch\n");
                db25Samples.clear();
            }
            taxiBuffer = (ODF_TAXI_BUFFER *)0;

            // Log error in case of short data (Unlikely, would be a bug)
            if(dstPtr != &((uint8_t *)db25Samples.data())[memo->sampleCount * sizeof(ISPDB25Point)])
            {
                printf("Short data: Buffer length / sample count mismatch\n");
                db25Samples.clear();
            }
        }
        while(0);
        cmdMutex.unlock();

        // Done in case of no more input buffers
        //printf("Found %d samples\n", db25Samples.size());
        if (db25Samples.empty())
            break;

        // -----------------------------------------------------------------------------------------
        // There are samples and the input chunk is in DB25 format in vector db25Samples now
        // -----------------------------------------------------------------------------------------

        // The driver may stay in the current mode, change mode or become active.
        driverMode = isWave ? DRIVER_WAVEMODE : DRIVER_FRAMEMODE;

        // When in frame mode - clear all current data (since new data came in - overrun)
        if (driverMode == DRIVER_FRAMEMODE)
        {
            sliceBuf->clear();
            tfEnv.db25Accu.clear();
        }

        unsigned sampleCount = db25Samples.size();

        // Repair any discontinuity
        std::vector<ISPDB25Point> repairDb25Samples;
        if (sampleCount > 1 && isWave)
        {
            if (isDiscontinuous)
            {
                uint32_t timeskip = duration * 0.9; // A guess, since chunks should be roughly uniformly sized
                if (timeskip > 10 && timeskip < 1000000)
                {
                    uint16_t newX = db25Samples.front().x;
                    uint16_t newY = db25Samples.front().y;

                    if (newX != previousX && newY != previousY)
                    {
                        unsigned int numInterpolatedPoints = sampleCount * timeskip / duration;
                        if (numInterpolatedPoints == 0)
                            numInterpolatedPoints = 1;
                        repairDb25Samples.reserve(numInterpolatedPoints);
                        for (unsigned int i = 1; i < numInterpolatedPoints; i++)
                        {
                            ISPDB25Point point;
                            memset(&point, 0, sizeof(point));
                            point.x = previousX + (newX - previousX) * (i / (double)numInterpolatedPoints);
                            point.y = previousY + (newY - previousY) * (i / (double)numInterpolatedPoints);
                            repairDb25Samples.push_back(point);
                            sampleCount++;
                        }
                        printf("[WAR] wave timestamp mismatch. inserting %u points, %u us\n", numInterpolatedPoints, timeskip);
                    }
                }
            }
            previousX = db25Samples.back().x;
            previousY = db25Samples.back().y;
        }

        tfEnv.db25Accu.reserve(tfEnv.db25Accu.size() + sampleCount);

        double pointDuration = (double)duration / sampleCount;
        double targetPointRate = ((1000000.0 * (double)sampleCount) / (double)duration);
        double rateRatio = maxPointrate() / targetPointRate;

        for (auto combinedVector : { &repairDb25Samples, &db25Samples })
        {
            for (auto& sample : *combinedVector)
            {
                //start downsampling if the maximum device pointrate is exceeded
                if (rateRatio < 1.0)
                {
                    //skip point, but act like it was added
                    //this keeps rateRatio% of points
                    if (tfEnv.skipCounter >= rateRatio)
                    {
                        tfEnv.skipCounter += rateRatio;
                        tfEnv.skipCounter -= (int)tfEnv.skipCounter;
                        tfEnv.currentSliceTime -= pointDuration;
                        continue;
                    }
                    tfEnv.skipCounter += rateRatio;
                    tfEnv.skipCounter -= (int)tfEnv.skipCounter;
                }

                //here the current slice has enough space,
                //so append the point and decrease
                //the currentSliceTime by the point duration
                tfEnv.db25Accu.push_back(sample);
                tfEnv.currentSliceTime -= pointDuration;

                //chunk the points
                if (isWave)
                {
                    //wave mode chunks into chunks of x ms that don't exceed the maximum amount of
                    //bytes that the adapter accepts
                    if (tfEnv.currentSliceTime < 0 ||
                        bytesPerPoint() * (tfEnv.db25Accu.size() + 1) > maxBytesPerTransmission())
                    {
                        commitChunk(tfEnv, sliceBuf);
                    }
                }
                else if (!isWave && maxBytesPerTransmission() != -1)
                {
                    //frame mode does not need to chunk based on duration, but it still needs evenly sized chunks if
                    //the device has a point limit, so it tries to equalize them
                    unsigned numChunksOfBlockSize = ceil(((double)sampleCount * (double)bytesPerPoint()) / (double)maxBytesPerTransmission());
                    unsigned equalizedSize = ceil((double)sampleCount / (double)numChunksOfBlockSize);

                    if (tfEnv.db25Accu.size() + 1 > equalizedSize)
                    {
                        commitChunk(tfEnv, sliceBuf);
                    }
                }
            }
        }

        if (driverMode == DRIVER_FRAMEMODE)
        {
            commitChunk(tfEnv, sliceBuf);
        }
    }
    return sliceBuf;
}

