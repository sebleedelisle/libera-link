

#ifndef ISP_DB25_POINT_H
#define ISP_DB25_POINT_H

// Standard libraries
#include <stdint.h>


struct ISPDB25Point {
  uint16_t x;
  uint16_t y;
  uint16_t r;
  uint16_t g;
  uint16_t b;
  uint16_t intensity;
  uint16_t shutter;
  uint16_t u1;
  uint16_t u2;
  uint16_t u3;
  uint16_t u4;

  bool operator==(const ISPDB25Point& rhs) {
	  if(this->x != rhs.x)
		  return false;

	  if(this->y != rhs.y)
		  return false;

	  if(this->r != rhs.r)
		  return false;

	  if(this->g != rhs.g)
		  return false;

	  if(this->b != rhs.b)
		  return false;

	  if(this->intensity != rhs.intensity)
		  return false;

	  if(this->shutter != rhs.shutter)
		  return false;

	  if(this->u1 != rhs.u1)
		  return false;

	  if(this->u2 != rhs.u2)
		  return false;

	  if(this->u3 != rhs.u3)
		  return false;

	  if(this->u4 != rhs.u4)
		  return false;

	  return true;
  }
};


#endif
