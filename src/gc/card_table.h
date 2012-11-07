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

#ifndef ART_SRC_GC_CARDTABLE_H_
#define ART_SRC_GC_CARDTABLE_H_

#include "globals.h"
#include "logging.h"
#include "mem_map.h"
#include "space_bitmap.h"
#include "UniquePtr.h"
#include "utils.h"

namespace art {

class Heap;
class ContinuousSpace;
class SpaceBitmap;
class Object;

// Maintain a card table from the the write barrier. All writes of
// non-NULL values to heap addresses should go through an entry in
// WriteBarrier, and from there to here.
class CardTable {
 public:
  static const size_t kCardShift = 7;
  static const size_t kCardSize = (1 << kCardShift);
  static const uint8_t kCardClean  = 0x0;
  static const uint8_t kCardDirty = 0x70;

  static CardTable* Create(const byte* heap_begin, size_t heap_capacity);

  // Set the card associated with the given address to GC_CARD_DIRTY.
  void MarkCard(const void *addr) {
    byte* card_addr = CardFromAddr(addr);
    *card_addr = kCardDirty;
  }

  // Is the object on a dirty card?
  bool IsDirty(const Object* obj) const {
    return *CardFromAddr(obj) == kCardDirty;
  }

  // Visit and clear cards within memory range, only visits dirty cards.
  template <typename Visitor>
  void VisitClear(const void* start, const void* end, const Visitor& visitor) {
    byte* card_start = CardFromAddr(start);
    byte* card_end = CardFromAddr(end);
    for (byte* it = card_start; it != card_end; ++it) {
      if (*it == kCardDirty) {
        *it = kCardClean;
        visitor(it);
      }
    }
  }

  // Returns a value that when added to a heap address >> GC_CARD_SHIFT will address the appropriate
  // card table byte. For convenience this value is cached in every Thread
  byte* GetBiasedBegin() const {
    return biased_begin_;
  }

  /*
   * Visitor is expected to take in a card and return the new value. When a value is modified, the
   * modify visitor is called.
   * visitor: The visitor which modifies the cards. Returns the new value for a card given an old
   * value.
   * modified: Whenever the visitor modifies a card, this visitor is called on the card. Enables
   * us to know which cards got cleared.
   */
  template <typename Visitor, typename ModifiedVisitor>
  void ModifyCardsAtomic(byte* scan_begin, byte* scan_end, const Visitor& visitor,
                      const ModifiedVisitor& modified = VoidFunctor()) {
    byte* card_cur = CardFromAddr(scan_begin);
    byte* card_end = CardFromAddr(scan_end);
    CheckCardValid(card_cur);
    CheckCardValid(card_end);

    // Handle any unaligned cards at the start.
    while (!IsAligned<sizeof(word)>(card_cur) && card_cur < card_end) {
      byte expected, new_value;
      do {
        expected = *card_cur;
        new_value = visitor(expected);
      } while (expected != new_value && UNLIKELY(byte_cas(expected, new_value, card_cur) != 0));
      if (expected != new_value) {
        modified(card_cur, expected, new_value);
      }
      ++card_cur;
    }

    // Handle unaligned cards at the end.
    while (!IsAligned<sizeof(word)>(card_end) && card_end > card_cur) {
      --card_end;
      byte expected, new_value;
      do {
        expected = *card_end;
        new_value = visitor(expected);
      } while (expected != new_value && UNLIKELY(byte_cas(expected, new_value, card_end) != 0));
      if (expected != new_value) {
        modified(card_cur, expected, new_value);
      }
    }

    // Now we have the words, we can process words in parallel.
    uintptr_t* word_cur = reinterpret_cast<uintptr_t*>(card_cur);
    uintptr_t* word_end = reinterpret_cast<uintptr_t*>(card_end);
    uintptr_t expected_word;
    uintptr_t new_word;

    // TODO: Parallelize.
    while (word_cur < word_end) {
      while ((expected_word = *word_cur) != 0) {
        new_word =
            (visitor((expected_word >> 0) & 0xFF) << 0) |
            (visitor((expected_word >> 8) & 0xFF) << 8) |
            (visitor((expected_word >> 16) & 0xFF) << 16) |
            (visitor((expected_word >> 24) & 0xFF) << 24);
        if (new_word == expected_word) {
          // No need to do a cas.
          break;
        }
        if (LIKELY(android_atomic_cas(expected_word, new_word,
                                      reinterpret_cast<int32_t*>(word_cur)) == 0)) {
          for (size_t i = 0; i < sizeof(uintptr_t); ++i) {
            const byte expected_byte = (expected_word >> (8 * i)) & 0xFF;
            const byte new_byte = (new_word >> (8 * i)) & 0xFF;
            if (expected_byte != new_byte) {
              modified(reinterpret_cast<byte*>(word_cur) + i, expected_byte, new_byte);
            }
          }
          break;
        }
      }
      ++word_cur;
    }
  }

  // For every dirty at least minumum age between begin and end invoke the visitor with the
  // specified argument.
  template <typename Visitor, typename FingerVisitor>
  void Scan(SpaceBitmap* bitmap, byte* scan_begin, byte* scan_end,
            const Visitor& visitor, const FingerVisitor& finger_visitor,
            const byte minimum_age = kCardDirty) const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(bitmap->HasAddress(scan_begin));
    DCHECK(bitmap->HasAddress(scan_end - 1));  // scan_end is the byte after the last byte we scan.
    byte* card_cur = CardFromAddr(scan_begin);
    byte* card_end = CardFromAddr(scan_end);
    CheckCardValid(card_cur);
    CheckCardValid(card_end);

    // Handle any unaligned cards at the start.
    while (!IsAligned<sizeof(word)>(card_cur) && card_cur < card_end) {
      if (*card_cur >= minimum_age) {
        uintptr_t start = reinterpret_cast<uintptr_t>(AddrFromCard(card_cur));
        uintptr_t end = start + kCardSize;
        bitmap->VisitMarkedRange(start, end, visitor, finger_visitor);
      }
      ++card_cur;
    }

    byte* aligned_end = card_end -
        (reinterpret_cast<uintptr_t>(card_end) & (sizeof(uintptr_t) - 1));

    // Now we have the words, we can send these to be processed in parallel.
    uintptr_t* word_cur = reinterpret_cast<uintptr_t*>(card_cur);
    uintptr_t* word_end = reinterpret_cast<uintptr_t*>(aligned_end);

    // TODO: Parallelize
    while (word_cur < word_end) {
      // Find the first dirty card.
      while (*word_cur == 0 && word_cur < word_end) {
        word_cur++;
      }
      if (word_cur >= word_end) {
        break;
      }
      uintptr_t start_word = *word_cur;
      for (size_t i = 0; i < sizeof(uintptr_t); ++i) {
        if ((start_word & 0xFF) == minimum_age) {
          byte* card = reinterpret_cast<byte*>(word_cur) + i;
          DCHECK_EQ(*card, start_word & 0xFF);
          uintptr_t start = reinterpret_cast<uintptr_t>(AddrFromCard(card));
          uintptr_t end = start + kCardSize;
          bitmap->VisitMarkedRange(start, end, visitor, finger_visitor);
        }
        start_word >>= 8;
      }
      ++word_cur;
    }

    // Handle any unaligned cards at the end.
    card_cur = reinterpret_cast<byte*>(word_end);
    while (card_cur < card_end) {
      if (*card_cur >= minimum_age) {
        uintptr_t start = reinterpret_cast<uintptr_t>(AddrFromCard(card_cur));
        uintptr_t end = start + kCardSize;
        bitmap->VisitMarkedRange(start, end, visitor, finger_visitor);
      }
      ++card_cur;
    }
  }

  // Assertion used to check the given address is covered by the card table
  void CheckAddrIsInCardTable(const byte* addr) const;

  // Resets all of the bytes in the card table to clean.
  void ClearCardTable();

  // Resets all of the bytes in the card table which do not map to the image space.
  void ClearSpaceCards(ContinuousSpace* space);

  // Returns the first address in the heap which maps to this card.
  void* AddrFromCard(const byte *card_addr) const {
    DCHECK(IsValidCard(card_addr))
      << " card_addr: " << reinterpret_cast<const void*>(card_addr)
      << " begin: " << reinterpret_cast<void*>(mem_map_->Begin() + offset_)
      << " end: " << reinterpret_cast<void*>(mem_map_->End());
    uintptr_t offset = card_addr - biased_begin_;
    return reinterpret_cast<void*>(offset << kCardShift);
  }

  // Returns the address of the relevant byte in the card table, given an address on the heap.
  byte* CardFromAddr(const void *addr) const {
    byte *card_addr = biased_begin_ + ((uintptr_t)addr >> kCardShift);
    // Sanity check the caller was asking for address covered by the card table
    DCHECK(IsValidCard(card_addr)) << "addr: " << addr
        << " card_addr: " << reinterpret_cast<void*>(card_addr);
    return card_addr;
  }

  bool AddrIsInCardTable(const void* addr) const;

 private:
  static int byte_cas(byte old_value, byte new_value, byte* address) {
    // Little endian means most significant byte is on the left.
    const size_t shift = reinterpret_cast<uintptr_t>(address) % sizeof(uintptr_t);
    // Align the address down.
    address -= shift;
    int32_t* word_address = reinterpret_cast<int32_t*>(address);
    // Word with the byte we are trying to cas cleared.
    const int32_t cur_word = *word_address & ~(0xFF << shift);
    const int32_t old_word = cur_word | (static_cast<int32_t>(old_value) << shift);
    const int32_t new_word = cur_word | (static_cast<int32_t>(new_value) << shift);
    return android_atomic_cas(old_word, new_word, word_address);
  }

  CardTable(MemMap* begin, byte* biased_begin, size_t offset);

  // Returns true iff the card table address is within the bounds of the card table.
  bool IsValidCard(const byte* card_addr) const {
    byte* begin = mem_map_->Begin() + offset_;
    byte* end = mem_map_->End();
    return card_addr >= begin && card_addr < end;
  }

  void CheckCardValid(byte* card) const {
    DCHECK(IsValidCard(card))
        << " card_addr: " << reinterpret_cast<const void*>(card)
        << " begin: " << reinterpret_cast<void*>(mem_map_->Begin() + offset_)
        << " end: " << reinterpret_cast<void*>(mem_map_->End());
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
#endif  // ART_SRC_GC_CARDTABLE_H_
