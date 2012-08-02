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

#ifndef ART_SRC_SPACE_BITMAP_H_
#define ART_SRC_SPACE_BITMAP_H_

#include <limits.h>
#include <stdint.h>
#include <vector>

#include "UniquePtr.h"
#include "globals.h"
#include "logging.h"
#include "mem_map.h"
#include "utils.h"

namespace art {

class Object;

class SpaceBitmap {
 public:
  static const size_t kAlignment = 8;

  typedef void Callback(Object* obj, void* arg);

  typedef void ScanCallback(Object* obj, void* finger, void* arg);

  typedef void SweepCallback(size_t ptr_count, Object** ptrs, void* arg);

  // Initialize a HeapBitmap so that it points to a bitmap large enough to cover a heap at
  // heap_begin of heap_capacity bytes, where objects are guaranteed to be kAlignment-aligned.
  static SpaceBitmap* Create(const std::string& name, byte* heap_begin, size_t heap_capacity);

  ~SpaceBitmap();

  // <offset> is the difference from .base to a pointer address.
  // <index> is the index of .bits that contains the bit representing
  //         <offset>.
  static size_t OffsetToIndex(size_t offset) {
      return offset / kAlignment / kBitsPerWord;
  }

  static uintptr_t IndexToOffset(size_t index) {
    return static_cast<uintptr_t>(index * kAlignment * kBitsPerWord);
  }

  // Pack the bits in backwards so they come out in address order when using CLZ.
  static word OffsetToMask(uintptr_t offset_) {
    return static_cast<uintptr_t>(kWordHighBitMask) >> ((offset_ / kAlignment) % kBitsPerWord);
  }

  inline void Set(const Object* obj) {
    Modify(obj, true);
  }

  inline void Clear(const Object* obj) {
    Modify(obj, false);
  }

  void Clear();

  inline bool Test(const Object* obj) const {
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    DCHECK(HasAddress(obj)) << obj;
    DCHECK(bitmap_begin_ != NULL);
    DCHECK_GE(addr, heap_begin_);
    if (addr <= heap_end_) {
      const uintptr_t offset = addr - heap_begin_;
      return (bitmap_begin_[OffsetToIndex(offset)] & OffsetToMask(offset)) != 0;
    } else {
      return false;
    }
  }

  bool HasAddress(const void* addr) const;

  void VisitRange(uintptr_t base, uintptr_t max, Callback* visitor, void* arg) const;

  class ClearVisitor {
   public:
    explicit ClearVisitor(SpaceBitmap* const bitmap)
        : bitmap_(bitmap) {
    }

    void operator ()(Object* obj) const {
      bitmap_->Clear(obj);
    }
   private:
    SpaceBitmap* const bitmap_;
  };

  template <typename Visitor>
  void VisitRange(uintptr_t visit_begin, uintptr_t visit_end, const Visitor& visitor) const {
    for (; visit_begin < visit_end; visit_begin += kAlignment ) {
      visitor(reinterpret_cast<Object*>(visit_begin));
    }
  }

  template <typename Visitor>
  void VisitMarkedRange(uintptr_t visit_begin, uintptr_t visit_end, const Visitor& visitor) const {
    DCHECK_LT(visit_begin, visit_end);

    const size_t bit_index_start = (visit_begin - heap_begin_) / kAlignment;
    const size_t bit_index_end = (visit_end - heap_begin_ - 1) / kAlignment;

    size_t word_start = bit_index_start / kBitsPerWord;
    size_t word_end = bit_index_end / kBitsPerWord;
    DCHECK_LT(word_end * kWordSize, Size());

    // Trim off left_bits of left bits.
    size_t edge_word = bitmap_begin_[word_start];

    // Handle bits on the left first as a special case
    size_t left_bits = bit_index_start & (kBitsPerWord - 1);
    if (left_bits != 0) {
      edge_word &= (1 << (kBitsPerWord - left_bits)) - 1;
    }

    // If word_start == word_end then handle this case at the same place we handle the right edge.
    if (edge_word != 0 && word_start < word_end) {
      uintptr_t ptr_base = IndexToOffset(word_start) + heap_begin_;
      do {
        const size_t shift = CLZ(edge_word);
        Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
        visitor(obj);
        edge_word ^= static_cast<size_t>(kWordHighBitMask) >> shift;
      } while (edge_word != 0);
    }
    word_start++;

    for (size_t i = word_start; i < word_end; i++) {
      size_t w = bitmap_begin_[i];
      if (w != 0) {
        uintptr_t ptr_base = IndexToOffset(i) + heap_begin_;
        do {
          const size_t shift = CLZ(w);
          Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
          visitor(obj);
          w ^= static_cast<size_t>(kWordHighBitMask) >> shift;
        } while (w != 0);
      }
    }

    // Handle the right edge, and also the left edge if both edges are on the same word.
    size_t right_bits = bit_index_end & (kBitsPerWord - 1);

    // If word_start == word_end then we need to use the word which we removed the left bits.
    if (word_start <= word_end) {
      edge_word = bitmap_begin_[word_end];
    }

    // Bits that we trim off the right.
    const size_t trim_bits = kBitsPerWord - 1 - right_bits;
    edge_word &= ~((1 << trim_bits) - 1);
    uintptr_t ptr_base = IndexToOffset(word_end) + heap_begin_;
    while (edge_word != 0) {
      const size_t shift = CLZ(edge_word);
      Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
      visitor(obj);
      edge_word ^= static_cast<size_t>(kWordHighBitMask) >> shift;
    }
  }

  void Walk(Callback* callback, void* arg);

  void InOrderWalk(Callback* callback, void* arg);

  void ScanWalk(uintptr_t base, uintptr_t max, ScanCallback* thunk, void* arg);

  static void SweepWalk(const SpaceBitmap& live,
                        const SpaceBitmap& mark,
                        uintptr_t base, uintptr_t max,
                        SweepCallback* thunk, void* arg);

  // Starting address of our internal storage.
  word* Begin() {
    return bitmap_begin_;
  }

  // Size of our internal storage
  size_t Size() const {
    return bitmap_size_;
  }

  // Size in bytes of the memory that the bitmaps spans.
  size_t HeapSize() const {
    return IndexToOffset(Size() / kWordSize);
  }

  uintptr_t HeapBegin() const {
    return heap_begin_;
  }

  // The maximum address which the bitmap can span. (HeapBegin() <= object < HeapLimit()).
  uintptr_t HeapLimit() const {
    return HeapBegin() + static_cast<uintptr_t>(HeapSize());
  }

  // Set the max address which can covered by the bitmap.
  void SetHeapLimit(uintptr_t new_end);

 private:
  // TODO: heap_end_ is initialized so that the heap bitmap is empty, this doesn't require the -1,
  // however, we document that this is expected on heap_end_
  SpaceBitmap(const std::string& name, MemMap* mem_map, word* bitmap_begin, size_t bitmap_size, const void* heap_begin)
      : mem_map_(mem_map), bitmap_begin_(bitmap_begin), bitmap_size_(bitmap_size),
        heap_begin_(reinterpret_cast<uintptr_t>(heap_begin)), heap_end_(heap_begin_ - 1),
        name_(name) {}

  inline void Modify(const Object* obj, bool do_set) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    DCHECK_GE(addr, heap_begin_);
    const uintptr_t offset = addr - heap_begin_;
    const size_t index = OffsetToIndex(offset);
    const word mask = OffsetToMask(offset);
    DCHECK_LT(index, bitmap_size_ / kWordSize) << " bitmap_size_ = " << bitmap_size_;
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
  size_t bitmap_size_;

  // The base address of the heap, which corresponds to the word containing the first bit in the
  // bitmap.
  const uintptr_t heap_begin_;

  // The highest pointer value ever returned by an allocation from
  // this heap.  I.e., the highest address that may correspond to a
  // set bit.  If there are no bits set, (heap_end_ < heap_begin_).
  uintptr_t heap_end_;

  // Name of this bitmap.
  std::string name_;
};

}  // namespace art

#endif  // ART_SRC_SPACE_BITMAP_H_
