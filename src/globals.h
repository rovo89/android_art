// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_GLOBALS_H_
#define ART_SRC_GLOBALS_H_

#include <stddef.h>
#include <stdint.h>

namespace art {

typedef uint8_t byte;
typedef intptr_t word;
typedef uintptr_t uword;

const size_t KB = 1024;
const size_t MB = KB * KB;
const size_t GB = KB * KB * KB;
const int kMaxInt = 0x7FFFFFFF;
const int kMinInt = -kMaxInt - 1;

const int kCharSize = sizeof(char);
const int kShortSize = sizeof(short);
const int kIntSize = sizeof(int);
const int kDoubleSize = sizeof(double);
const int kIntptrSize = sizeof(intptr_t);
const int kWordSize = sizeof(word);
const int kPointerSize = sizeof(void*);

const int kBitsPerByte = 8;
const int kBitsPerByteLog2 = 3;
const int kBitsPerPointer = kPointerSize * kBitsPerByte;
const int kBitsPerWord = kWordSize * kBitsPerByte;
const int kBitsPerInt = kIntSize * kBitsPerByte;

// Required stack alignment
const int kStackAlignment = 16;

// Required object alignment
const int kObjectAlignment = 8;

// Required ARM instruction alignment
const int kArmAlignment = 4;

// System page size.  Normally you're expected to get this from
// sysconf(_SC_PAGESIZE) or some system-specific define (usually
// PAGESIZE or PAGE_SIZE).  If we use a simple compile-time constant
// the compiler can generate appropriate masks directly, so we define
// it here and verify it as the runtime is starting up.
//
// Must be a power of 2.
const int kPageSize = 4096;

}  // namespace art

#endif  // ART_SRC_GLOBALS_H_
