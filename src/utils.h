// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_UTILS_H_
#define ART_SRC_UTILS_H_

#include "globals.h"
#include "logging.h"
#include "stringpiece.h"
#include "stringprintf.h"

namespace art {

class Object;

template<typename T>
static inline bool IsPowerOfTwo(T x) {
  return (x & (x - 1)) == 0;
}

template<typename T>
static inline bool IsAligned(T x, int n) {
  CHECK(IsPowerOfTwo(n));
  return (x & (n - 1)) == 0;
}

template<typename T>
static inline bool IsAligned(T* x, int n) {
  return IsAligned(reinterpret_cast<uintptr_t>(x), n);
}

// Check whether an N-bit two's-complement representation can hold value.
static inline bool IsInt(int N, word value) {
  CHECK_LT(0, N);
  CHECK_LT(N, kBitsPerWord);
  word limit = static_cast<word>(1) << (N - 1);
  return (-limit <= value) && (value < limit);
}

static inline bool IsUint(int N, word value) {
  CHECK_LT(0, N);
  CHECK_LT(N, kBitsPerWord);
  word limit = static_cast<word>(1) << N;
  return (0 <= value) && (value < limit);
}

static inline bool IsAbsoluteUint(int N, word value) {
  CHECK_LT(0, N);
  CHECK_LT(N, kBitsPerWord);
  if (value < 0) value = -value;
  return IsUint(N, value);
}

static inline int32_t Low16Bits(int32_t value) {
  return static_cast<int32_t>(value & 0xffff);
}

static inline int32_t High16Bits(int32_t value) {
  return static_cast<int32_t>(value >> 16);
}

static inline int32_t Low32Bits(int64_t value) {
  return static_cast<int32_t>(value);
}

static inline int32_t High32Bits(int64_t value) {
  return static_cast<int32_t>(value >> 32);
}

template<typename T>
static inline T RoundDown(T x, int n) {
  CHECK(IsPowerOfTwo(n));
  return (x & -n);
}

template<typename T>
static inline T RoundUp(T x, int n) {
  return RoundDown(x + n - 1, n);
}

// Implementation is from "Hacker's Delight" by Henry S. Warren, Jr.,
// figure 3-3, page 48, where the function is called clp2.
static inline uint32_t RoundUpToPowerOfTwo(uint32_t x) {
  x = x - 1;
  x = x | (x >> 1);
  x = x | (x >> 2);
  x = x | (x >> 4);
  x = x | (x >> 8);
  x = x | (x >> 16);
  return x + 1;
}

// Implementation is from "Hacker's Delight" by Henry S. Warren, Jr.,
// figure 5-2, page 66, where the function is called pop.
static inline int CountOneBits(uint32_t x) {
  x = x - ((x >> 1) & 0x55555555);
  x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
  x = (x + (x >> 4)) & 0x0F0F0F0F;
  x = x + (x >> 8);
  x = x + (x >> 16);
  return static_cast<int>(x & 0x0000003F);
}

#define CLZ(x) __builtin_clz(x)

static inline bool NeedsEscaping(uint16_t ch) {
  return (ch < ' ' || ch > '~');
}

static inline std::string PrintableChar(uint16_t ch) {
  std::string result;
  result += '\'';
  if (NeedsEscaping(ch)) {
    StringAppendF(&result, "\\u%04x", ch);
  } else {
    result += ch;
  }
  result += '\'';
  return result;
}

// TODO: assume the content is UTF-8, and show code point escapes?
template<typename StringT>
static inline std::string PrintableString(const StringT& s) {
  std::string result;
  result += '"';
  for (typename StringT::iterator it = s.begin(); it != s.end(); ++it) {
    char ch = *it;
    if (NeedsEscaping(ch)) {
      StringAppendF(&result, "\\x%02x", ch & 0xff);
    } else {
      result += ch;
    }
  }
  result += '"';
  return result;
}

// Return a newly-allocated string containing a human-readable equivalent
// of 'descriptor'. So "I" would be "int", "[[I" would be "int[][]",
// "[Ljava/lang/String;" would be "java.lang.String[]", and so forth.
std::string PrettyDescriptor(const StringPiece& descriptor);

// Returns a human-readable string form of the name of the class of
// the given object. So given a java.lang.String, the output would
// be "java.lang.String". Given an array of int, the output would be "int[]".
// Given String.class, the output would be "java.lang.Class<java.lang.String>".
std::string PrettyType(const Object* obj);

}  // namespace art

#endif  // ART_SRC_UTILS_H_
