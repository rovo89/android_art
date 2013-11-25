/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_GC_SPACE_ROSALLOC_SPACE_INL_H_
#define ART_RUNTIME_GC_SPACE_ROSALLOC_SPACE_INL_H_

#include "gc/allocator/rosalloc-inl.h"
#include "rosalloc_space.h"
#include "thread.h"

namespace art {
namespace gc {
namespace space {

inline mirror::Object* RosAllocSpace::AllocNonvirtual(Thread* self, size_t num_bytes,
                                                      size_t* bytes_allocated) {
  mirror::Object* obj;
  obj = AllocWithoutGrowthLocked(self, num_bytes, bytes_allocated);
  // RosAlloc zeroes memory internally.
  return obj;
}

inline mirror::Object* RosAllocSpace::AllocWithoutGrowthLocked(Thread* self, size_t num_bytes,
                                                               size_t* bytes_allocated) {
  size_t rosalloc_size = 0;
  mirror::Object* result = reinterpret_cast<mirror::Object*>(
      rosalloc_for_alloc_->Alloc(self, num_bytes,
                                 &rosalloc_size));
  if (LIKELY(result != NULL)) {
    if (kDebugSpaces) {
      CHECK(Contains(result)) << "Allocation (" << reinterpret_cast<void*>(result)
            << ") not in bounds of allocation space " << *this;
    }
    DCHECK(bytes_allocated != NULL);
    *bytes_allocated = rosalloc_size;
  }
  return result;
}

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_ROSALLOC_SPACE_INL_H_
