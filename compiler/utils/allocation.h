/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_UTILS_ALLOCATION_H_
#define ART_COMPILER_UTILS_ALLOCATION_H_

#include "arena_allocator.h"
#include "base/logging.h"

namespace art {

class ArenaObject {
 public:
  // Allocate a new ArenaObject of 'size' bytes in the Arena.
  void* operator new(size_t size, ArenaAllocator* allocator) {
    return allocator->Alloc(size, kArenaAllocMisc);
  }

  void operator delete(void*, size_t) {
    LOG(FATAL) << "UNREACHABLE";
  }
};

class ValueObject {
 public:
  void* operator new(size_t size) {
    LOG(FATAL) << "UNREACHABLE";
    abort();
  }
  void operator delete(void*, size_t) {
    LOG(FATAL) << "UNREACHABLE";
  }
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_ALLOCATION_H_
