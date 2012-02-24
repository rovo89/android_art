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

#ifndef ART_SRC_HEAP_BITMAP_H_
#define ART_SRC_HEAP_BITMAP_H_

#include <limits.h>
#include <stdint.h>

#include "UniquePtr.h"
#include "globals.h"
#include "logging.h"
#include "mem_map.h"

namespace art {

class Object;

// <offset> is the difference from .base to a pointer address.
// <index> is the index of .bits that contains the bit representing
//         <offset>.
#define HB_OFFSET_TO_INDEX(offset_) \
    ((offset_) / kAlignment / kBitsPerWord)
#define HB_INDEX_TO_OFFSET(index_) \
    ((index_) * kAlignment * kBitsPerWord)

#define HB_OFFSET_TO_BYTE_INDEX(offset_) \
  (HB_OFFSET_TO_INDEX(offset_) * sizeof(*(reinterpret_cast<HeapBitmap*>(0))->words_))

// Pack the bits in backwards so they come out in address order
// when using CLZ.
#define HB_OFFSET_TO_MASK(offset_) \
    (1 << (31-(((uintptr_t)(offset_) / kAlignment) % kBitsPerWord)))

class HeapBitmap {
 public:
  static const size_t kAlignment = 8;

  typedef void Callback(Object* obj, void* arg);

  typedef void ScanCallback(Object* obj, void* finger, void* arg);

  typedef void SweepCallback(size_t numPtrs, Object** ptrs, void* arg);

  // Initialize a HeapBitmap so that it points to a bitmap large enough to cover a heap at
  // heap_begin of heap_capacity bytes, where objects are guaranteed to be kAlignment-aligned.
  static HeapBitmap* Create(const char* name, byte* heap_begin, size_t heap_capacity);

  ~HeapBitmap();

  inline void Set(const Object* obj) {
    Modify(obj, true);
  }

  inline void Clear(const Object* obj) {
    Modify(obj, false);
  }

  void Clear();

  inline bool Test(const Object* obj) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    DCHECK(HasAddress(obj)) << obj;
    DCHECK(bitmap_begin_ != NULL);
    DCHECK_GE(addr, heap_begin_);
    if (addr <= heap_end_) {
      const uintptr_t offset = addr - heap_begin_;
      return (bitmap_begin_[HB_OFFSET_TO_INDEX(offset)] & HB_OFFSET_TO_MASK(offset)) != 0;
    } else {
      return false;
    }
  }

  bool HasAddress(const void* addr) const;

  void VisitRange(uintptr_t base, uintptr_t max, Callback* visitor, void* arg) const;

  void Walk(Callback* callback, void* arg);

  void InOrderWalk(HeapBitmap::Callback* callback, void* arg);

  void ScanWalk(uintptr_t base, uintptr_t max, ScanCallback* thunk, void* arg);

  static void SweepWalk(const HeapBitmap& live,
                        const HeapBitmap& mark,
                        uintptr_t base, uintptr_t max,
                        SweepCallback* thunk, void* arg);

 private:
  // TODO: heap_end_ is initialized so that the heap bitmap is empty, this doesn't require the -1,
  // however, we document that this is expected on heap_end_
  HeapBitmap(const char* name, MemMap* mem_map, word* bitmap_begin, size_t bitmap_size, const void* heap_begin)
      : mem_map_(mem_map), bitmap_begin_(bitmap_begin), bitmap_size_(bitmap_size),
        heap_begin_(reinterpret_cast<uintptr_t>(heap_begin)), heap_end_(heap_begin_ - 1),
        name_(name) {}

  inline void Modify(const Object* obj, bool do_set) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    DCHECK_GE(addr, heap_begin_);
    const uintptr_t offset = addr - heap_begin_;
    const size_t index = HB_OFFSET_TO_INDEX(offset);
    const word mask = HB_OFFSET_TO_MASK(offset);
    DCHECK_LT(index, bitmap_size_ / kWordSize);
    if (do_set) {
      if (addr > heap_end_) {
        heap_end_ = addr;
      }
      bitmap_begin_[index] |= mask;
    } else {
      bitmap_begin_[index] &= ~mask;
    }
  }

  // Backing storage for bitmap.
  UniquePtr<MemMap> mem_map_;

  // This bitmap itself, word sized for efficiency in scanning.
  word* const bitmap_begin_;

  // Size of this bitmap.
  const size_t bitmap_size_;

  // The base address of the heap, which corresponds to the word containing the first bit in the
  // bitmap.
  const uintptr_t heap_begin_;

  // The highest pointer value ever returned by an allocation from
  // this heap.  I.e., the highest address that may correspond to a
  // set bit.  If there are no bits set, (heap_end_ < heap_begin_).
  uintptr_t heap_end_;

  // Name of this bitmap.
  const char* const name_;
};

}  // namespace art

#endif  // ART_SRC_HEAP_BITMAP_H_
