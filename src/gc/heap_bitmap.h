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

#ifndef ART_SRC_HEAP_BITMAP_H_
#define ART_SRC_HEAP_BITMAP_H_

#include "space_bitmap.h"

namespace art {
  class Heap;
  class SpaceBitmap;

  class HeapBitmap {
   public:
    bool Test(const Object* obj) SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
      SpaceBitmap* bitmap = GetSpaceBitmap(obj);
      if (LIKELY(bitmap != NULL)) {
        return bitmap->Test(obj);
      } else {
        return large_objects_->Test(obj);
      }
    }

    void Clear(const Object* obj)
        EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
      SpaceBitmap* bitmap = GetSpaceBitmap(obj);
      if (LIKELY(bitmap != NULL)) {
        bitmap->Clear(obj);
      } else {
        large_objects_->Clear(obj);
      }
    }

    void Set(const Object* obj)
        EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
      SpaceBitmap* bitmap = GetSpaceBitmap(obj);
      if (LIKELY(bitmap != NULL)) {
        bitmap->Set(obj);
      } else {
        large_objects_->Set(obj);
      }
    }

    SpaceBitmap* GetSpaceBitmap(const Object* obj) {
      // TODO: C++0x auto
      for (Bitmaps::iterator it = bitmaps_.begin(); it != bitmaps_.end(); ++it) {
        if ((*it)->HasAddress(obj)) {
          return *it;
        }
      }
      return NULL;
    }

    void Walk(SpaceBitmap::Callback* callback, void* arg)
        SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

    template <typename Visitor>
    void Visit(const Visitor& visitor)
        EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
      // TODO: C++0x auto
      for (Bitmaps::iterator it = bitmaps_.begin(); it != bitmaps_.end(); ++it) {
        SpaceBitmap* bitmap = *it;
        bitmap->VisitMarkedRange(bitmap->HeapBegin(), bitmap->HeapLimit(), visitor,
                                 IdentityFunctor());
      }
      large_objects_->Visit(visitor);
    }

    // Find and replace a bitmap pointer, this is used by for the bitmap swapping in the GC.
    void ReplaceBitmap(SpaceBitmap* old_bitmap, SpaceBitmap* new_bitmap)
        EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

    HeapBitmap(Heap* heap);

    inline SpaceSetMap* GetLargeObjects() const {
      return large_objects_;
    }

    void SetLargeObjects(SpaceSetMap* large_objects);

   private:

    const Heap* const heap_;

    void AddSpaceBitmap(SpaceBitmap* bitmap);

    typedef std::vector<SpaceBitmap*> Bitmaps;
    Bitmaps bitmaps_;

    // Large object sets.
    SpaceSetMap* large_objects_;

    friend class Heap;
  };
}  // namespace art

#endif  // ART_SRC_HEAP_BITMAP_H_
