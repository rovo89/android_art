/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef DALVIK_ALLOC_CARDTABLE_H_
#define DALVIK_ALLOC_CARDTABLE_H_

#include "globals.h"
#include "logging.h"
#include "mem_map.h"
#include "space_bitmap.h"
#include "UniquePtr.h"

namespace art {

class Heap;
class Space;
class SpaceBitmap;
class Object;

#define GC_CARD_SHIFT 7
#define GC_CARD_SIZE (1 << GC_CARD_SHIFT)
#define GC_CARD_CLEAN 0
#define GC_CARD_DIRTY 0x70

// Maintain a card table from the the write barrier. All writes of
// non-NULL values to heap addresses should go through an entry in
// WriteBarrier, and from there to here.
class CardTable {
 public:
  static CardTable* Create(const byte* heap_begin, size_t heap_capacity);

  // Set the card associated with the given address to GC_CARD_DIRTY.
  void MarkCard(const void *addr) {
    byte* card_addr = CardFromAddr(addr);
    *card_addr = GC_CARD_DIRTY;
  }

  // Is the object on a dirty card?
  bool IsDirty(const Object* obj) const {
    return *CardFromAddr(obj) == GC_CARD_DIRTY;
  }

  // Visit cards within memory range.
  template <typename Visitor>
  void VisitClear(const void* start, const void* end, const Visitor& visitor) {
    byte* card_start = CardFromAddr(start);
    byte* card_end = CardFromAddr(end);
    for (byte* cur = card_start; cur != card_end; ++cur) {
      if (*cur == GC_CARD_DIRTY) {
        *cur = GC_CARD_CLEAN;
        visitor(cur);
      }
    }
  }

  // Returns a value that when added to a heap address >> GC_CARD_SHIFT will address the appropriate
  // card table byte. For convenience this value is cached in every Thread
  byte* GetBiasedBegin() const {
    return biased_begin_;
  }

  // For every dirty card between begin and end invoke the visitor with the specified argument.
  template <typename Visitor>
  void Scan(SpaceBitmap* bitmap, byte* scan_begin, byte* scan_end, const Visitor& visitor) const
      EXCLUSIVE_LOCKS_REQUIRED(GlobalSynchronization::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    DCHECK(bitmap->HasAddress(scan_begin));
    DCHECK(bitmap->HasAddress(scan_end - 1));  // scan_end is the byte after the last byte we scan.
    byte* card_cur = CardFromAddr(scan_begin);
    byte* card_end = CardFromAddr(scan_end);
    while (card_cur < card_end) {
      while (card_cur < card_end && *card_cur == GC_CARD_CLEAN) {
        card_cur++;
      }
      byte* run_start = card_cur;

      while (card_cur < card_end && *card_cur == GC_CARD_DIRTY) {
        card_cur++;
      }
      byte* run_end = card_cur;

      if (run_start != run_end) {
        uintptr_t start = reinterpret_cast<uintptr_t>(AddrFromCard(run_start));
        uintptr_t end = reinterpret_cast<uintptr_t>(AddrFromCard(run_end));
        bitmap->VisitMarkedRange(start, end, visitor);
      }
    }
  }

  // Assertion used to check the given address is covered by the card table
  void CheckAddrIsInCardTable(const byte* addr) const;

  // Resets all of the bytes in the card table to clean.
  void ClearCardTable();

  // Resets all of the bytes in the card table which do not map to the image space.
  void ClearSpaceCards(Space* space);

  // Returns the first address in the heap which maps to this card.
  void* AddrFromCard(const byte *card_addr) const {
    DCHECK(IsValidCard(card_addr))
      << " card_addr: " << reinterpret_cast<const void*>(card_addr)
      << " begin: " << reinterpret_cast<void*>(mem_map_->Begin() + offset_)
      << " end: " << reinterpret_cast<void*>(mem_map_->End());
    uintptr_t offset = card_addr - biased_begin_;
    return reinterpret_cast<void*>(offset << GC_CARD_SHIFT);
  }

  // Returns the address of the relevant byte in the card table, given an address on the heap.
  byte* CardFromAddr(const void *addr) const {
    byte *card_addr = biased_begin_ + ((uintptr_t)addr >> GC_CARD_SHIFT);
    // Sanity check the caller was asking for address covered by the card table
    DCHECK(IsValidCard(card_addr)) << "addr: " << addr
        << " card_addr: " << reinterpret_cast<void*>(card_addr);
    return card_addr;
  }

 private:
  CardTable(MemMap* begin, byte* biased_begin, size_t offset);

  // Returns true iff the card table address is within the bounds of the card table.
  bool IsValidCard(const byte* card_addr) const {
    byte* begin = mem_map_->Begin() + offset_;
    byte* end = mem_map_->End();
    return card_addr >= begin && card_addr < end;
  }

  // Verifies that all gray objects are on a dirty card.
  void VerifyCardTable();

  // Mmapped pages for the card table
  UniquePtr<MemMap> mem_map_;
  // Value used to compute card table addresses from object addresses, see GetBiasedBegin
  byte* const biased_begin_;
  // Card table doesn't begin at the beginning of the mem_map_, instead it is displaced by offset
  // to allow the byte value of biased_begin_ to equal GC_CARD_DIRTY
  const size_t offset_;
};

}  // namespace art
#endif  // DALVIK_ALLOC_CARDTABLE_H_
