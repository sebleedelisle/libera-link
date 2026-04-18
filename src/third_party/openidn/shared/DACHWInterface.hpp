#ifndef DACADAPTER_H_
#define DACADAPTER_H_

#include "types.h"

#include "ISPDB25Point.h"

#include "LaproAdapter.hpp"


#include <mutex>


class TransformEnv
{
    public:

    double usPerSlice;

    double currentSliceTime;

    std::vector<ISPDB25Point> db25Accu;
    double skipCounter = 0;
};



class DACHWInterface: public LaproAdapter
{
    public:

	//writes byte data to a hardware interface
	virtual int writeFrame(const TimeSlice& slice, double duration) = 0;

	//converts ISPDB25 Points to bytes in a way that the hardware expects
	//points the outputBuf pointer to a vector and returns the total
	//number of bytes.
	virtual SliceType convertPoints(const std::vector<ISPDB25Point>& points) = 0;

	//size of converted points in bytes
	virtual unsigned bytesPerPoint() = 0;

	//maximum possible amount of bytes per transmission
	//without fragmentation
	virtual unsigned maxBytesPerTransmission() = 0;

	//maximum pointrate that the device if capable of
	//in pps
	virtual unsigned maxPointrate() = 0;
	virtual void setMaxPointrate(unsigned) = 0;


    // -----------------------------------------------------


    private:
    typedef LaproAdapter Inherited;
    std::mutex cmdMutex;
    bool enabledFlag = false;
    void commitChunk(TransformEnv &tfEnv, std::shared_ptr<SliceBuf> &sliceBuf);

    // For repairing discontinuities
    uint16_t previousX;
    uint16_t previousY;

    protected:
    virtual int enable();
    virtual void disable();

    public:
    virtual int putBuffer(ODF_TAXI_BUFFER *taxiBuffer);
    virtual std::shared_ptr<SliceBuf> getNextBuffer(TransformEnv &tfEnv, unsigned &driverMode);
};

#endif
