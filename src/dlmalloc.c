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

#define FOR_DLMALLOC_C  // Avoid inclusion of src/malloc.h
#include "dlmalloc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Disable GCC diagnostics so that -Werror won't fail
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wempty-body"

// ART specific morecore implementation
#define MORECORE(x) art_heap_morecore(m, x)
extern void* art_heap_morecore(void* m, intptr_t increment);

// Ugly inclusion of C file so that ART specific #defines configure dlmalloc
#include "dlmalloc/malloc.c"
