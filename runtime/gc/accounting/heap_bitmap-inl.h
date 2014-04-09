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

#ifndef ART_RUNTIME_GC_ACCOUNTING_HEAP_BITMAP_INL_H_
#define ART_RUNTIME_GC_ACCOUNTING_HEAP_BITMAP_INL_H_

#include "heap_bitmap.h"

#include "space_bitmap-inl.h"

namespace art {
namespace gc {
namespace accounting {

template <typename Visitor>
inline void HeapBitmap::Visit(const Visitor& visitor) {
  for (const auto& bitmap : continuous_space_bitmaps_) {
    bitmap->VisitMarkedRange(bitmap->HeapBegin(), bitmap->HeapLimit(), visitor);
  }
  DCHECK(!discontinuous_space_sets_.empty());
  for (const auto& space_set : discontinuous_space_sets_) {
    space_set->Visit(visitor);
  }
}

inline bool HeapBitmap::Test(const mirror::Object* obj) {
  ContinuousSpaceBitmap* bitmap = GetContinuousSpaceBitmap(obj);
  if (LIKELY(bitmap != nullptr)) {
    return bitmap->Test(obj);
  } else {
    return GetDiscontinuousSpaceObjectSet(obj) != nullptr;
  }
}

inline void HeapBitmap::Clear(const mirror::Object* obj) {
  ContinuousSpaceBitmap* bitmap = GetContinuousSpaceBitmap(obj);
  if (LIKELY(bitmap != nullptr)) {
    bitmap->Clear(obj);
  } else {
    ObjectSet* set = GetDiscontinuousSpaceObjectSet(obj);
    DCHECK(set != NULL);
    set->Clear(obj);
  }
}

inline void HeapBitmap::Set(const mirror::Object* obj) {
  ContinuousSpaceBitmap* bitmap = GetContinuousSpaceBitmap(obj);
  if (LIKELY(bitmap != NULL)) {
    bitmap->Set(obj);
  } else {
    ObjectSet* set = GetDiscontinuousSpaceObjectSet(obj);
    DCHECK(set != NULL);
    set->Set(obj);
  }
}

inline ContinuousSpaceBitmap* HeapBitmap::GetContinuousSpaceBitmap(const mirror::Object* obj) const {
  for (const auto& bitmap : continuous_space_bitmaps_) {
    if (bitmap->HasAddress(obj)) {
      return bitmap;
    }
  }
  return nullptr;
}

inline ObjectSet* HeapBitmap::GetDiscontinuousSpaceObjectSet(const mirror::Object* obj) const {
  for (const auto& space_set : discontinuous_space_sets_) {
    if (space_set->Test(obj)) {
      return space_set;
    }
  }
  return nullptr;
}

}  // namespace accounting
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_ACCOUNTING_HEAP_BITMAP_INL_H_
