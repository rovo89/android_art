/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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

const int kWordSize = sizeof(word);
const int kPointerSize = sizeof(void*);

const int kBitsPerByte = 8;
const int kBitsPerByteLog2 = 3;
const int kBitsPerWord = kWordSize * kBitsPerByte;

// Required stack alignment
const int kStackAlignment = 16;

// Required object alignment
const int kObjectAlignment = 8;

// Required ARM instruction alignment
const int kArmAlignment = 4;

// Required X86 instruction alignment
const int kX86Alignment = 16;

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
