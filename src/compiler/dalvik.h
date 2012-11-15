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

/*
 * Common defines for all Dalvik code.
 */
#ifndef DALVIK_COMMON_H_
#define DALVIK_COMMON_H_

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "gc/card_table.h"
#include "class_linker.h"
#include "compiler.h"
#include "dex_cache.h"
#include "logging.h"
#include "monitor.h"
#include "object.h"
#include "thread.h"
#include "utils.h"

// From Common.h
typedef uint8_t             u1;
typedef uint16_t            u2;
typedef uint32_t            u4;
typedef uint64_t            u8;
typedef int8_t              s1;
typedef int16_t             s2;
typedef int32_t             s4;
typedef int64_t             s8;
typedef unsigned long long  u8;

#include "frontend.h"

#endif
