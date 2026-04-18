#ifndef MYTYPES_H_
#define MYTYPES_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <deque>
#include <memory>
#include <string>

#define DRIVER_INACTIVE 0
#define DRIVER_WAVEMODE 1
#define DRIVER_FRAMEMODE 2

#ifndef NDEBUG
#define DEBUGOUTPUT
#endif
//#define DEBUGOUTPUT

/*
Laser configuration
*/

using SlicePrimitive = uint8_t;
using SliceType = std::vector<SlicePrimitive>;

//using a deque because the driver might want to use it as a ring buffer
struct TimeSlice {
	SliceType dataChunk;
	unsigned durationUs;
};
using SliceBuf = std::deque<std::shared_ptr<TimeSlice>>;

/*
End of Laser configuration
*/


 
#endif
