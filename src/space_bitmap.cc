/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "logging.h"
#include "UniquePtr.h"
#include "utils.h"

namespace art {

SpaceBitmap* SpaceBitmap::Create(const std::string& name, byte* heap_begin, size_t heap_capacity) {
  CHECK(heap_begin != NULL);
  // Round up since heap_capacity is not necessarily a multiple of kAlignment * kBitsPerWord.
  size_t bitmap_size = OffsetToIndex(RoundUp(heap_capacity, kAlignment * kBitsPerWord)) * kWordSize;
  UniquePtr<MemMap> mem_map(MemMap::MapAnonymous(name.c_str(), NULL, bitmap_size, PROT_READ | PROT_WRITE));
  if (mem_map.get() == NULL) {
    LOG(ERROR) << "Failed to allocate bitmap " << name;
    return NULL;
  }
  word* bitmap_begin = reinterpret_cast<word*>(mem_map->Begin());
  return new SpaceBitmap(name, mem_map.release(), bitmap_begin, bitmap_size, heap_begin);
}

// Clean up any resources associated with the bitmap.
SpaceBitmap::~SpaceBitmap() {}

void SpaceBitmap::Trim(size_t heap_capacity) {
  size_t new_size = OffsetToIndex(RoundUp(heap_capacity, kAlignment * kBitsPerWord)) * kWordSize;
  if (new_size < bitmap_size_) {
    bitmap_size_ = new_size;
  }
  // Not sure if doing this trim is necessary, since nothing past the end of the heap capacity
  // should be marked.
  // TODO: Fix this code is, it broken and causes rare heap corruption!
  // mem_map_->Trim(reinterpret_cast<byte*>(heap_begin_ + bitmap_size_));
}

// Fill the bitmap with zeroes.  Returns the bitmap's memory to the
// system as a side-effect.
void SpaceBitmap::Clear() {
  if (bitmap_begin_ != NULL) {
    // This returns the memory to the system.  Successive page faults
    // will return zeroed memory.
    int result = madvise(bitmap_begin_, bitmap_size_, MADV_DONTNEED);
    if (result == -1) {
      PLOG(WARNING) << "madvise failed";
    }
    heap_end_ = heap_begin_ - 1;
  }
}

// Return true iff <obj> is within the range of pointers that this bitmap could potentially cover,
// even if a bit has not been set for it.
bool SpaceBitmap::HasAddress(const void* obj) const {
  // If obj < heap_begin_ then offset underflows to some very large value past the end of the bitmap.
  const uintptr_t offset = (uintptr_t)obj - heap_begin_;
  const size_t index = OffsetToIndex(offset);
  return index < bitmap_size_ / kWordSize;
}

// Visits set bits in address order.  The callback is not permitted to
// change the bitmap bits or max during the traversal.
void SpaceBitmap::Walk(SpaceBitmap::Callback* callback, void* arg) {
  CHECK(bitmap_begin_ != NULL);
  CHECK(callback != NULL);
  if (heap_end_ < heap_begin_) {
    return;  // Bitmap is empty.
  }
  uintptr_t end = OffsetToIndex(heap_end_ - heap_begin_);
  for (uintptr_t i = 0; i <= end; ++i) {
    word w = bitmap_begin_[i];
    if (UNLIKELY(w != 0)) {
      word high_bit = 1 << (kBitsPerWord - 1);
      uintptr_t ptr_base = IndexToOffset(i) + heap_begin_;
      while (w != 0) {
        const int shift = CLZ(w);
        Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
        (*callback)(obj, arg);
        w &= ~(high_bit >> shift);
      }
    }
  }
}

// Similar to Walk but the callback routine is permitted to change the bitmap bits and end during
// traversal.  Used by the the root marking scan exclusively.
//
// The callback is invoked with a finger argument.  The finger is a pointer to an address not yet
// visited by the traversal.  If the callback sets a bit for an address at or above the finger, this
// address will be visited by the traversal.  If the callback sets a bit for an address below the
// finger, this address will not be visited (typiscally such an address would be placed on the
// marking stack).
void SpaceBitmap::ScanWalk(uintptr_t scan_begin, uintptr_t scan_end, ScanCallback* callback, void* arg) {
  CHECK(bitmap_begin_ != NULL);
  CHECK(callback != NULL);
  CHECK_LE(scan_begin, scan_end);
  CHECK_GE(scan_begin, heap_begin_);

  // This function doesn't support unaligned boundaries yet.
  size_t begin_offset = scan_begin - heap_begin_;
  size_t end_offset = scan_end - heap_begin_;
  DCHECK((begin_offset / kAlignment) % kBitsPerWord == 0)
      << "scan begin " << reinterpret_cast<const void*>(scan_begin)
      << " with offset " << begin_offset
      << " not aligned to word boundary";
  DCHECK((end_offset / kAlignment) % kBitsPerWord == 0)
      << "scan end " << reinterpret_cast<const void*>(scan_end)
      << " with offset " << end_offset
      << " not aligned to word boundary";

  size_t start = OffsetToIndex(begin_offset);
  if (scan_end < heap_end_) {
    // The end of the space we're looking at is before the current maximum bitmap PC, scan to that
    // and don't recompute end on each iteration
    size_t end = OffsetToIndex(end_offset - 1);
    for (size_t i = start; i <= end; i++) {
      word w = bitmap_begin_[i];
      if (UNLIKELY(w != 0)) {
        word high_bit = 1 << (kBitsPerWord - 1);
        uintptr_t ptr_base = IndexToOffset(i) + heap_begin_;
        void* finger = reinterpret_cast<void*>(IndexToOffset(i + 1) + heap_begin_);
        while (w != 0) {
          const int shift = CLZ(w);
          Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
          (*callback)(obj, finger, arg);
          w &= ~(high_bit >> shift);
        }
      }
    }
  } else {
    size_t end = OffsetToIndex(heap_end_ - heap_begin_);
    for (size_t i = start; i <= end; i++) {
      word w = bitmap_begin_[i];
      if (UNLIKELY(w != 0)) {
        word high_bit = 1 << (kBitsPerWord - 1);
        uintptr_t ptr_base = IndexToOffset(i) + heap_begin_;
        void* finger = reinterpret_cast<void*>(IndexToOffset(i + 1) + heap_begin_);
        while (w != 0) {
          const int shift = CLZ(w);
          Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
          (*callback)(obj, finger, arg);
          w &= ~(high_bit >> shift);
        }
      }
      // update 'end' in case callback modified bitmap
      end = OffsetToIndex(heap_end_ - heap_begin_);
    }
  }
}

// Walk through the bitmaps in increasing address order, and find the
// object pointers that correspond to garbage objects.  Call
// <callback> zero or more times with lists of these object pointers.
//
// The callback is not permitted to increase the max of either bitmap.
void SpaceBitmap::SweepWalk(const SpaceBitmap& live_bitmap,
                           const SpaceBitmap& mark_bitmap,
                           uintptr_t sweep_begin, uintptr_t sweep_end,
                           SpaceBitmap::SweepCallback* callback, void* arg) {
  CHECK(live_bitmap.bitmap_begin_ != NULL);
  CHECK(mark_bitmap.bitmap_begin_ != NULL);
  CHECK_EQ(live_bitmap.heap_begin_, mark_bitmap.heap_begin_);
  CHECK_EQ(live_bitmap.bitmap_size_, mark_bitmap.bitmap_size_);
  CHECK(callback != NULL);
  CHECK_LE(sweep_begin, sweep_end);
  CHECK_GE(sweep_begin, live_bitmap.heap_begin_);
  sweep_end = std::min(sweep_end - 1, live_bitmap.heap_end_);
  if (live_bitmap.heap_end_ < live_bitmap.heap_begin_) {
    // Easy case; both are obviously empty.
    // TODO: this should never happen
    return;
  }
  // TODO: rewrite the callbacks to accept a std::vector<Object*> rather than a Object**?
  std::vector<Object*> pointer_buf(4 * kBitsPerWord);
  Object** pb = &pointer_buf[0];
  size_t start = OffsetToIndex(sweep_begin - live_bitmap.heap_begin_);
  size_t end = OffsetToIndex(sweep_end - live_bitmap.heap_begin_);
  word* live = live_bitmap.bitmap_begin_;
  word* mark = mark_bitmap.bitmap_begin_;
  for (size_t i = start; i <= end; i++) {
    word garbage = live[i] & ~mark[i];
    if (UNLIKELY(garbage != 0)) {
      word high_bit = 1 << (kBitsPerWord - 1);
      uintptr_t ptr_base = IndexToOffset(i) + live_bitmap.heap_begin_;
      while (garbage != 0) {
        int shift = CLZ(garbage);
        garbage &= ~(high_bit >> shift);
        *pb++ = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
      }
      // Make sure that there are always enough slots available for an
      // entire word of one bits.
      if (pb >= &pointer_buf[pointer_buf.size() - kBitsPerWord]) {
        (*callback)(pb - &pointer_buf[0], &pointer_buf[0], arg);
        pb = &pointer_buf[0];
      }
    }
  }
  if (pb > &pointer_buf[0]) {
    (*callback)(pb - &pointer_buf[0], &pointer_buf[0], arg);
  }
}

}  // namespace art

// Support needed for in order traversal
#include "object.h"
#include "object_utils.h"

namespace art {

static void WalkFieldsInOrder(SpaceBitmap* visited, SpaceBitmap::Callback* callback, Object* obj,
                              void* arg);

// Walk instance fields of the given Class. Separate function to allow recursion on the super
// class.
static void WalkInstanceFields(SpaceBitmap* visited, SpaceBitmap::Callback* callback, Object* obj,
                               Class* klass, void* arg) {
  // Visit fields of parent classes first.
  Class* super = klass->GetSuperClass();
  if (super != NULL) {
    WalkInstanceFields(visited, callback, obj, super, arg);
  }
  // Walk instance fields
  ObjectArray<Field>* fields = klass->GetIFields();
  if (fields != NULL) {
    for (int32_t i = 0; i < fields->GetLength(); i++) {
      Field* field = fields->Get(i);
      FieldHelper fh(field);
      if (!fh.GetType()->IsPrimitive()) {
        Object* value = field->GetObj(obj);
        if (value != NULL) {
          WalkFieldsInOrder(visited, callback, value,  arg);
        }
      }
    }
  }
}

// For an unvisited object, visit it then all its children found via fields.
static void WalkFieldsInOrder(SpaceBitmap* visited, SpaceBitmap::Callback* callback, Object* obj,
                              void* arg) {
  if (visited->Test(obj)) {
    return;
  }
  // visit the object itself
  (*callback)(obj, arg);
  visited->Set(obj);
  // Walk instance fields of all objects
  Class* klass = obj->GetClass();
  WalkInstanceFields(visited, callback, obj, klass, arg);
  // Walk static fields of a Class
  if (obj->IsClass()) {
    ObjectArray<Field>* fields = klass->GetSFields();
    if (fields != NULL) {
      for (int32_t i = 0; i < fields->GetLength(); i++) {
        Field* field = fields->Get(i);
        FieldHelper fh(field);
        if (!fh.GetType()->IsPrimitive()) {
          Object* value = field->GetObj(NULL);
          if (value != NULL) {
            WalkFieldsInOrder(visited, callback, value, arg);
          }
        }
      }
    }
  } else if (obj->IsObjectArray()) {
    // Walk elements of an object array
    ObjectArray<Object>* obj_array = obj->AsObjectArray<Object>();
    int32_t length = obj_array->GetLength();
    for (int32_t i = 0; i < length; i++) {
      Object* value = obj_array->Get(i);
      if (value != NULL) {
        WalkFieldsInOrder(visited, callback, value, arg);
      }
    }
  }
}

// Visits set bits with an in order traversal.  The callback is not permitted to change the bitmap
// bits or max during the traversal.
void SpaceBitmap::InOrderWalk(SpaceBitmap::Callback* callback, void* arg) {
  UniquePtr<SpaceBitmap> visited(Create("bitmap for in-order walk",
                                       reinterpret_cast<byte*>(heap_begin_),
                                       IndexToOffset(bitmap_size_ / kWordSize)));
  CHECK(bitmap_begin_ != NULL);
  CHECK(callback != NULL);
  uintptr_t end = OffsetToIndex(heap_end_ - heap_begin_);
  for (uintptr_t i = 0; i <= end; ++i) {
    word w = bitmap_begin_[i];
    if (UNLIKELY(w != 0)) {
      word high_bit = 1 << (kBitsPerWord - 1);
      uintptr_t ptr_base = IndexToOffset(i) + heap_begin_;
      while (w != 0) {
        const int shift = CLZ(w);
        Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
        WalkFieldsInOrder(visited.get(), callback, obj, arg);
        w &= ~(high_bit >> shift);
      }
    }
  }
}

}  // namespace art
