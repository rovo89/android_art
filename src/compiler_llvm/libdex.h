/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_LLVM_LIBDEX_H_
#define ART_SRC_COMPILER_LLVM_LIBDEX_H_

#include <assert.h>

// From Common.h
// TODO: remove all these and just use the long names
typedef uint8_t             u1;
typedef uint16_t            u2;
typedef uint32_t            u4;
typedef uint64_t            u8;
typedef int8_t              s1;
typedef int16_t             s2;
typedef int32_t             s4;
typedef int64_t             s8;
typedef unsigned long long  u8;

// Skip old DexFile.h
#define LIBDEX_DEXFILE_H_

// Skip old vm/Common.h
#define DALVIK_COMMON_H_

// Make inlines inline
#define DEX_INLINE inline
#include "DexOpcodes.h"
#include "InstrUtils.h"


#endif // ART_SRC_COMPILER_LLVM_LIBDEX_H_
