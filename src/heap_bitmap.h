// Copyright (C) 2008 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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

  typedef void SweepCallback(size_t numPtrs, void** ptrs, void* arg);

  static HeapBitmap* Create(byte* base, size_t length);

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
    DCHECK(words_ != NULL);
    DCHECK_GE(addr, base_);
    if (addr <= max_) {
      const uintptr_t offset = addr - base_;
      return (words_[HB_OFFSET_TO_INDEX(offset)] & HB_OFFSET_TO_MASK(offset)) != 0;
    } else {
      return false;
    }
  }

  bool HasAddress(const void* addr) const;

  void Walk(Callback* callback, void* arg);

  void ScanWalk(uintptr_t base, ScanCallback* thunk, void* arg);

  static void SweepWalk(const HeapBitmap& live,
                        const HeapBitmap& mark,
                        uintptr_t base, uintptr_t max,
                        SweepCallback* thunk, void* arg);

 private:
  HeapBitmap(const void* base, size_t length)
      : words_(NULL),
        num_bytes_(length),
        base_(reinterpret_cast<uintptr_t>(base)) {
  };

  inline void Modify(const Object* obj, bool do_set) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    DCHECK_GE(addr, base_);
    const uintptr_t offset = addr - base_;
    const size_t index = HB_OFFSET_TO_INDEX(offset);
    const word mask = HB_OFFSET_TO_MASK(offset);
    DCHECK_LT(index, num_bytes_ / kWordSize);
    if (do_set) {
      if (addr > max_) {
        max_ = addr;
      }
      words_[index] |= mask;
    } else {
      words_[index] &= ~mask;
    }
  }

  bool Init(const byte* base, size_t length);

  UniquePtr<MemMap> mem_map_;

  word* words_;

  size_t num_bytes_;

  // The base address, which corresponds to the word containing the
  // first bit in the bitmap.
  uintptr_t base_;

  // The highest pointer value ever returned by an allocation from
  // this heap.  I.e., the highest address that may correspond to a
  // set bit.  If there are no bits set, (max_ < base_).
  uintptr_t max_;

  const char* name_;
};

}  // namespace art

#endif  // ART_SRC_HEAP_BITMAP_H_
