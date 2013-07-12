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

#include "heap_bitmap.h"

#include "gc/space/space.h"

namespace art {
namespace gc {
namespace accounting {

void HeapBitmap::ReplaceBitmap(SpaceBitmap* old_bitmap, SpaceBitmap* new_bitmap) {
  // TODO: C++0x auto
  typedef std::vector<SpaceBitmap*>::iterator It;
  for (It it = continuous_space_bitmaps_.begin(), end = continuous_space_bitmaps_.end();
      it != end; ++it) {
    if (*it == old_bitmap) {
      *it = new_bitmap;
      return;
    }
  }
  LOG(FATAL) << "bitmap " << static_cast<const void*>(old_bitmap) << " not found";
}

void HeapBitmap::ReplaceObjectSet(SpaceSetMap* old_set, SpaceSetMap* new_set) {
  // TODO: C++0x auto
  typedef std::vector<SpaceSetMap*>::iterator It;
  for (It it = discontinuous_space_sets_.begin(), end = discontinuous_space_sets_.end();
      it != end; ++it) {
    if (*it == old_set) {
      *it = new_set;
      return;
    }
  }
  LOG(FATAL) << "object set " << static_cast<const void*>(old_set) << " not found";
}

void HeapBitmap::AddContinuousSpaceBitmap(accounting::SpaceBitmap* bitmap) {
  DCHECK(bitmap != NULL);

  // Check for interval overlap.
  typedef std::vector<SpaceBitmap*>::iterator It;
  for (It it = continuous_space_bitmaps_.begin(), end = continuous_space_bitmaps_.end();
      it != end; ++it) {
    SpaceBitmap* bitmap = *it;
    SpaceBitmap* cur_bitmap = *it;
    CHECK(bitmap->HeapBegin() < cur_bitmap->HeapLimit() &&
          bitmap->HeapLimit() > cur_bitmap->HeapBegin())
        << "Bitmap " << bitmap->Dump() << " overlaps with existing bitmap " << cur_bitmap->Dump();
  }
  continuous_space_bitmaps_.push_back(bitmap);
}

void HeapBitmap::AddDiscontinuousObjectSet(SpaceSetMap* set) {
  DCHECK(set != NULL);
  discontinuous_space_sets_.push_back(set);
}

void HeapBitmap::Walk(SpaceBitmap::Callback* callback, void* arg) {
  // TODO: C++0x auto
  typedef std::vector<SpaceBitmap*>::iterator It;
  for (It it = continuous_space_bitmaps_.begin(), end = continuous_space_bitmaps_.end();
      it != end; ++it) {
    SpaceBitmap* bitmap = *it;
    bitmap->Walk(callback, arg);
  }
  // TODO: C++0x auto
  typedef std::vector<SpaceSetMap*>::iterator It2;
  DCHECK(discontinuous_space_sets_.begin() !=  discontinuous_space_sets_.end());
  for (It2 it = discontinuous_space_sets_.begin(), end = discontinuous_space_sets_.end();
      it != end; ++it) {
    SpaceSetMap* set = *it;
    set->Walk(callback, arg);
  }
}

}  // namespace accounting
}  // namespace gc
}  // namespace art
