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

#include "heap_bitmap.h"

#include <sys/mman.h>

#include "UniquePtr.h"
#include "logging.h"
#include "utils.h"

namespace art {

HeapBitmap* HeapBitmap::Create(byte* base, size_t length) {
  UniquePtr<HeapBitmap> bitmap(new HeapBitmap(base, length));
  if (!bitmap->Init(base, length)) {
    return NULL;
  } else {
    return bitmap.release();
  }
}

// Initialize a HeapBitmap so that it points to a bitmap large enough
// to cover a heap at <base> of <max_size> bytes, where objects are
// guaranteed to be kAlignment-aligned.
bool HeapBitmap::Init(const byte* base, size_t max_size) {
  CHECK(base != NULL);
  size_t length = HB_OFFSET_TO_INDEX(max_size) * kWordSize;
  mem_map_.reset(MemMap::Map(length, PROT_READ | PROT_WRITE));
  if (mem_map_.get() == NULL) {
    return false;
  }
  words_ = reinterpret_cast<word*>(mem_map_->GetAddress());
  num_bytes_ = length;
  base_ = reinterpret_cast<uintptr_t>(base);
  max_ = base_ - 1;
  return true;
}

// Clean up any resources associated with the bitmap.
HeapBitmap::~HeapBitmap() {}

// Fill the bitmap with zeroes.  Returns the bitmap's memory to the
// system as a side-effect.
void HeapBitmap::Clear() {
  if (words_ != NULL) {
    // This returns the memory to the system.  Successive page faults
    // will return zeroed memory.
    int result = madvise(words_, num_bytes_, MADV_DONTNEED);
    if (result == -1) {
      PLOG(WARNING) << "madvise failed";
    }
    max_ = base_ - 1;
  }
}

// Return true iff <obj> is within the range of pointers that this
// bitmap could potentially cover, even if a bit has not been set for
// it.
bool HeapBitmap::HasAddress(const void* obj) const {
  if (obj != NULL) {
    const uintptr_t offset = (uintptr_t)obj - base_;
    const size_t index = HB_OFFSET_TO_INDEX(offset);
    return index < num_bytes_ / kWordSize;
  }
  return false;
}

void HeapBitmap::VisitRange(uintptr_t base, uintptr_t max, Callback* visitor, void* arg) const {
  size_t start = HB_OFFSET_TO_INDEX(base - base_);
  size_t end = HB_OFFSET_TO_INDEX(max - base_ - 1);
  for (size_t i = start; i <= end; i++) {
    word w = words_[i];
    if (w != 0) {
      word high_bit = 1 << (kBitsPerWord - 1);
      uintptr_t ptr_base = HB_INDEX_TO_OFFSET(i) + base_;
      while (w != 0) {
        const int shift = CLZ(w);
        Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
        (*visitor)(obj, arg);
        w &= ~(high_bit >> shift);
      }
    }
  }
}

// Visits set bits in address order.  The callback is not permitted to
// change the bitmap bits or max during the traversal.
void HeapBitmap::Walk(HeapBitmap::Callback* callback, void* arg) {
  CHECK(words_ != NULL);
  CHECK(callback != NULL);
  uintptr_t end = HB_OFFSET_TO_INDEX(max_ - base_);
  for (uintptr_t i = 0; i <= end; ++i) {
    word w = words_[i];
    if (UNLIKELY(w != 0)) {
      word high_bit = 1 << (kBitsPerWord - 1);
      uintptr_t ptr_base = HB_INDEX_TO_OFFSET(i) + base_;
      while (w != 0) {
        const int shift = CLZ(w);
        Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
        (*callback)(obj, arg);
        w &= ~(high_bit >> shift);
      }
    }
  }
}

// Similar to Walk but the callback routine is permitted to change the
// bitmap bits and max during traversal.  Used by the the root marking
// scan exclusively.
//
// The callback is invoked with a finger argument.  The finger is a
// pointer to an address not yet visited by the traversal.  If the
// callback sets a bit for an address at or above the finger, this
// address will be visited by the traversal.  If the callback sets a
// bit for an address below the finger, this address will not be
// visited.
void HeapBitmap::ScanWalk(uintptr_t base, uintptr_t max, ScanCallback* callback, void* arg) {
  CHECK(words_ != NULL);
  CHECK(callback != NULL);
  CHECK_LE(base, max);
  CHECK_GE(base, base_);
  size_t start = HB_OFFSET_TO_INDEX(base - base_);
  if (max < max_) {
    // The end of the space we're looking at is before the current maximum bitmap PC, scan to that
    // and don't recompute end on each iteration
    size_t end = HB_OFFSET_TO_INDEX(max - base_ - 1);
    for (size_t i = start; i <= end; i++) {
      word w = words_[i];
      if (UNLIKELY(w != 0)) {
        word high_bit = 1 << (kBitsPerWord - 1);
        uintptr_t ptr_base = HB_INDEX_TO_OFFSET(i) + base_;
        void* finger = reinterpret_cast<void*>(HB_INDEX_TO_OFFSET(i + 1) + base_);
        while (w != 0) {
          const int shift = CLZ(w);
          Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
          (*callback)(obj, finger, arg);
          w &= ~(high_bit >> shift);
        }
      }
    }
  } else {
    size_t end = HB_OFFSET_TO_INDEX(max_ - base_);
    for (size_t i = start; i <= end; i++) {
      word w = words_[i];
      if (UNLIKELY(w != 0)) {
        word high_bit = 1 << (kBitsPerWord - 1);
        uintptr_t ptr_base = HB_INDEX_TO_OFFSET(i) + base_;
        void* finger = reinterpret_cast<void*>(HB_INDEX_TO_OFFSET(i + 1) + base_);
        while (w != 0) {
          const int shift = CLZ(w);
          Object* obj = reinterpret_cast<Object*>(ptr_base + shift * kAlignment);
          (*callback)(obj, finger, arg);
          w &= ~(high_bit >> shift);
        }
      }
      end = HB_OFFSET_TO_INDEX(max_ - base_);
    }
  }
}

// Walk through the bitmaps in increasing address order, and find the
// object pointers that correspond to garbage objects.  Call
// <callback> zero or more times with lists of these object pointers.
//
// The callback is not permitted to increase the max of either bitmap.
void HeapBitmap::SweepWalk(const HeapBitmap& live_bitmap,
                           const HeapBitmap& mark_bitmap,
                           uintptr_t base, uintptr_t max,
                           HeapBitmap::SweepCallback* callback, void* arg) {
  CHECK(live_bitmap.words_ != NULL);
  CHECK(mark_bitmap.words_ != NULL);
  CHECK_EQ(live_bitmap.base_, mark_bitmap.base_);
  CHECK_EQ(live_bitmap.num_bytes_, mark_bitmap.num_bytes_);
  CHECK(callback != NULL);
  CHECK_LE(base, max);
  CHECK_GE(base, live_bitmap.base_);
  max = std::min(max - 1, live_bitmap.max_);
  if (live_bitmap.max_ < live_bitmap.base_) {
    // Easy case; both are obviously empty.
    // TODO: this should never happen
    return;
  }
  // TODO: rewrite the callbacks to accept a std::vector<void*> rather than a void**?
  std::vector<void*> pointer_buf(4 * kBitsPerWord);
  void** pb = &pointer_buf[0];
  size_t start = HB_OFFSET_TO_INDEX(base - live_bitmap.base_);
  size_t end = HB_OFFSET_TO_INDEX(max - live_bitmap.base_);
  word* live = live_bitmap.words_;
  word* mark = mark_bitmap.words_;
  for (size_t i = start; i <= end; i++) {
    word garbage = live[i] & ~mark[i];
    if (UNLIKELY(garbage != 0)) {
      word high_bit = 1 << (kBitsPerWord - 1);
      uintptr_t ptr_base = HB_INDEX_TO_OFFSET(i) + live_bitmap.base_;
      while (garbage != 0) {
        int shift = CLZ(garbage);
        garbage &= ~(high_bit >> shift);
        *pb++ = reinterpret_cast<void*>(ptr_base + shift * kAlignment);
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
