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

#ifndef ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_INL_H_
#define ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_INL_H_

#include "bump_pointer_space.h"

namespace art {
namespace gc {
namespace space {

inline mirror::Object* BumpPointerSpace::AllocNonvirtualWithoutAccounting(size_t num_bytes) {
  DCHECK(IsAligned<kAlignment>(num_bytes));
  byte* old_end;
  byte* new_end;
  do {
    old_end = end_;
    new_end = old_end + num_bytes;
    // If there is no more room in the region, we are out of memory.
    if (UNLIKELY(new_end > growth_end_)) {
      return nullptr;
    }
    // TODO: Use a cas which always equals the size of pointers.
  } while (android_atomic_cas(reinterpret_cast<int32_t>(old_end),
                              reinterpret_cast<int32_t>(new_end),
                              reinterpret_cast<volatile int32_t*>(&end_)) != 0);
  return reinterpret_cast<mirror::Object*>(old_end);
}

inline mirror::Object* BumpPointerSpace::AllocNonvirtual(size_t num_bytes) {
  mirror::Object* ret = AllocNonvirtualWithoutAccounting(num_bytes);
  if (ret != nullptr) {
    objects_allocated_.fetch_add(1);
    bytes_allocated_.fetch_add(num_bytes);
  }
  return ret;
}

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_INL_H_
