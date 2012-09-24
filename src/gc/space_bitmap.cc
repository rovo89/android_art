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

std::string SpaceBitmap::GetName() const {
  return name_;
}

void SpaceBitmap::SetName(const std::string& name) {
  name_ = name;
}

void SpaceSetMap::Walk(SpaceBitmap::Callback* callback, void* arg) {
  for (Objects::iterator it = contained_.begin(); it != contained_.end(); ++it) {
    callback(const_cast<Object*>(*it), arg);
  }
}

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
SpaceBitmap::~SpaceBitmap() {

}

void SpaceBitmap::SetHeapLimit(uintptr_t new_end) {
  DCHECK(IsAligned<kBitsPerWord * kAlignment>(new_end));
  size_t new_size = OffsetToIndex(new_end - heap_begin_) * kWordSize;
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
      PLOG(FATAL) << "madvise failed";
    }
  }
}

void SpaceBitmap::CopyFrom(SpaceBitmap* source_bitmap) {
  DCHECK_EQ(Size(), source_bitmap->Size());
  std::copy(source_bitmap->Begin(), source_bitmap->Begin() + source_bitmap->Size() / kWordSize, Begin());
}

// Visits set bits in address order.  The callback is not permitted to
// change the bitmap bits or max during the traversal.
void SpaceBitmap::Walk(SpaceBitmap::Callback* callback, void* arg) {
  CHECK(bitmap_begin_ != NULL);
  CHECK(callback != NULL);

  uintptr_t end = OffsetToIndex(HeapLimit() - heap_begin_ - 1);
  word* bitmap_begin = bitmap_begin_;
  for (uintptr_t i = 0; i <= end; ++i) {
    word w = bitmap_begin[i];
    if (w != 0) {
      uintptr_t ptr_base = IndexToOffset(i) + heap_begin_;
      do {
        const size_t shift = CLZ(w);
        Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
        (*callback)(obj, arg);
        w ^= static_cast<size_t>(kWordHighBitMask) >> shift;
      } while (w != 0);
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

  if (sweep_end <= sweep_begin) {
    return;
  }

  // TODO: rewrite the callbacks to accept a std::vector<Object*> rather than a Object**?
  const size_t buffer_size = kWordSize * kBitsPerWord;
  Object* pointer_buf[buffer_size];
  Object** pb = &pointer_buf[0];
  size_t start = OffsetToIndex(sweep_begin - live_bitmap.heap_begin_);
  size_t end = OffsetToIndex(sweep_end - live_bitmap.heap_begin_ - 1);
  CHECK_LT(end, live_bitmap.Size() / kWordSize);
  word* live = live_bitmap.bitmap_begin_;
  word* mark = mark_bitmap.bitmap_begin_;
  for (size_t i = start; i <= end; i++) {
    word garbage = live[i] & ~mark[i];
    if (UNLIKELY(garbage != 0)) {
      uintptr_t ptr_base = IndexToOffset(i) + live_bitmap.heap_begin_;
      do {
        const size_t shift = CLZ(garbage);
        garbage ^= static_cast<size_t>(kWordHighBitMask) >> shift;
        *pb++ = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
      } while (garbage != 0);
      // Make sure that there are always enough slots available for an
      // entire word of one bits.
      if (pb >= &pointer_buf[buffer_size - kBitsPerWord]) {
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
                               Class* klass, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
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
      if (!fh.IsPrimitiveType()) {
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
                              void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
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
        if (!fh.IsPrimitiveType()) {
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
  uintptr_t end = Size() / kWordSize;
  for (uintptr_t i = 0; i < end; ++i) {
    word w = bitmap_begin_[i];
    if (UNLIKELY(w != 0)) {
      uintptr_t ptr_base = IndexToOffset(i) + heap_begin_;
      while (w != 0) {
        const size_t shift = CLZ(w);
        Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
        WalkFieldsInOrder(visited.get(), callback, obj, arg);
        w ^= static_cast<size_t>(kWordHighBitMask) >> shift;
      }
    }
  }
}

std::string SpaceSetMap::GetName() const {
  return name_;
}

void SpaceSetMap::SetName(const std::string& name) {
  name_ = name;
}

SpaceSetMap::SpaceSetMap(const std::string& name) : name_(name) {

}

void SpaceSetMap::CopyFrom(const SpaceSetMap& space_set) {
  contained_ = space_set.contained_;
}

std::ostream& operator << (std::ostream& stream, const SpaceBitmap& bitmap) {
  return stream
    << bitmap.GetName() << "["
    << "begin=" << reinterpret_cast<const void*>(bitmap.HeapBegin())
    << ",end=" << reinterpret_cast<const void*>(bitmap.HeapLimit())
    << "]";
  }

}  // namespace art
