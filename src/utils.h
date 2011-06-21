// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_UTILS_H_
#define ART_SRC_UTILS_H_

#include "src/globals.h"

namespace art {

// Check whether an N-bit two's-complement representation can hold value.
static inline bool IsInt(int N, word value) {
  CHECK_LT(0, N);
  CHECK_LT(N, kBitsPerWord);
  word limit = static_cast<word>(1) << (N - 1);
  return (-limit <= value) && (value < limit);
}


template<typename T>
static inline bool IsPowerOfTwo(T x) {
  return (x & (x - 1)) == 0;
}


static inline bool IsUint(int N, word value) {
  CHECK_LT(0, N);
  CHECK_LT(N, kBitsPerWord);
  word limit = static_cast<word>(1) << N;
  return (0 <= value) && (value < limit);
}


static inline int32_t Low32Bits(int64_t value) {
  return static_cast<int32_t>(value);
}


static inline int32_t High32Bits(int64_t value) {
  return static_cast<int32_t>(value >> 32);
}

}  // namespace art

#endif  // ART_SRC_UTILS_H_
