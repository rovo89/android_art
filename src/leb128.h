// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_LEB128_H_
#define ART_SRC_LEB128_H_

#include "globals.h"

namespace art {

// Reads an unsigned LEB128 value, updating the given pointer to point
// just past the end of the read value. This function tolerates
// non-zero high-order bits in the fifth encoded byte.
static inline uint32_t DecodeUnsignedLeb128(const byte** data) {
  const byte* ptr = *data;
  int result = *(ptr++);
  if (result > 0x7f) {
    int cur = *(ptr++);
    result = (result & 0x7f) | ((cur & 0x7f) << 7);
    if (cur > 0x7f) {
      cur = *(ptr++);
      result |= (cur & 0x7f) << 14;
      if (cur > 0x7f) {
        cur = *(ptr++);
        result |= (cur & 0x7f) << 21;
        if (cur > 0x7f) {
          // Note: We don't check to see if cur is out of range here,
          // meaning we tolerate garbage in the four high-order bits.
          cur = *(ptr++);
          result |= cur << 28;
        }
      }
    }
  }
  *data = ptr;
  return (uint32_t)result;
}

// Reads an unsigned LEB128 + 1 value. updating the given pointer to point
// just past the end of the read value. This function tolerates
// non-zero high-order bits in the fifth encoded byte.
// It is possible for this function to return -1.
static inline int32_t DecodeUnsignedLeb128P1(const byte** data) {
  return DecodeUnsignedLeb128(data) - 1;
}

// Reads a signed LEB128 value, updating the given pointer to point
// just past the end of the read value. This function tolerates
// non-zero high-order bits in the fifth encoded byte.
static inline int32_t DecodeSignedLeb128(const byte** data) {
  const byte* ptr = *data;
  int32_t result = *(ptr++);
  if (result <= 0x7f) {
    result = (result << 25) >> 25;
  } else {
    int cur = *(ptr++);
    result = (result & 0x7f) | ((cur & 0x7f) << 7);
    if (cur <= 0x7f) {
      result = (result << 18) >> 18;
    } else {
      cur = *(ptr++);
      result |= (cur & 0x7f) << 14;
      if (cur <= 0x7f) {
        result = (result << 11) >> 11;
      } else {
        cur = *(ptr++);
        result |= (cur & 0x7f) << 21;
        if (cur <= 0x7f) {
          result = (result << 4) >> 4;
        } else {
          // Note: We don't check to see if cur is out of range here,
          // meaning we tolerate garbage in the four high-order bits.
          cur = *(ptr++);
          result |= cur << 28;
        }
      }
    }
  }
  *data = ptr;
  return result;
}

// Returns the number of bytes needed to encode the value in unsigned LEB128.
static inline uint32_t UnsignedLeb128Size(uint32_t data) {
  uint32_t count = 0;
  do {
    data >>= 7;
    count++;
  } while (data != 0);
  return count;
}

// Writes a 32-bit value in unsigned ULEB128 format.
// Returns the updated pointer.
static inline uint8_t* WriteUnsignedLeb128(uint8_t* ptr, uint32_t data) {
  while (true) {
    uint8_t out = data & 0x7f;
    if (out != data) {
      *ptr++ = out | 0x80;
      data >>= 7;
    } else {
      *ptr++ = out;
      break;
    }
  }
  return ptr;
}

}  // namespace art

#endif  // ART_SRC_LEB128_H_
