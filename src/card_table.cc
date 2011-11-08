/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include "card_table.h"

#include <sys/mman.h>  /* for PROT_* */

#include "heap.h"
#include "heap_bitmap.h"
#include "logging.h"

namespace art {
/*
 * Maintain a card table from the write barrier. All writes of
 * non-NULL values to heap addresses should go through an entry in
 * WriteBarrier, and from there to here.
 *
 * The heap is divided into "cards" of GC_CARD_SIZE bytes, as
 * determined by GC_CARD_SHIFT. The card table contains one byte of
 * data per card, to be used by the GC. The value of the byte will be
 * one of GC_CARD_CLEAN or GC_CARD_DIRTY.
 *
 * After any store of a non-NULL object pointer into a heap object,
 * code is obliged to mark the card dirty. The setters in
 * object.h [such as SetFieldObject] do this for you. The
 * compiler also contains code to mark cards as dirty.
 *
 * The card table's base [the "biased card table"] gets set to a
 * rather strange value.  In order to keep the JIT from having to
 * fabricate or load GC_DIRTY_CARD to store into the card table,
 * biased base is within the mmap allocation at a point where its low
 * byte is equal to GC_DIRTY_CARD. See CardTable::Init for details.
 */

CardTable* CardTable::Create(const byte* heap_base, size_t heap_max_size, size_t growth_size) {
  UniquePtr<CardTable> bitmap(new CardTable);
  if (!bitmap->Init(heap_base, heap_max_size, growth_size)) {
    return NULL;
  } else {
    return bitmap.release();
  }
}

/*
 * Initializes the card table; must be called before any other
 * CardTable functions.
 */
bool CardTable::Init(const byte* heap_base, size_t heap_max_size, size_t growth_size) {
  /* Set up the card table */
  size_t length = heap_max_size / GC_CARD_SIZE;
  /* Allocate an extra 256 bytes to allow fixed low-byte of base */
  mem_map_.reset(MemMap::Map("dalvik-card-table", NULL, length + 256, PROT_READ | PROT_WRITE));
  byte* alloc_base = mem_map_->GetAddress();
  if (alloc_base == NULL) {
    return false;
  }
  base_ = alloc_base;
  length_ = growth_size / GC_CARD_SIZE;
  max_length_ = length;
  offset_ = 0;
  /* All zeros is the correct initial value; all clean. */
  CHECK_EQ(GC_CARD_CLEAN, 0);
  biased_base_ = (byte *)((uintptr_t)alloc_base -((uintptr_t)heap_base >> GC_CARD_SHIFT));
  if (((uintptr_t)biased_base_ & 0xff) != GC_CARD_DIRTY) {
    int offset = GC_CARD_DIRTY - (reinterpret_cast<int>(biased_base_) & 0xff);
    offset_ = offset + (offset < 0 ? 0x100 : 0);
    biased_base_ += offset_;
  }
  CHECK_EQ(reinterpret_cast<int>(biased_base_) & 0xff, GC_CARD_DIRTY);
  ClearCardTable();
  return true;
}

void CardTable::ClearCardTable() {
  CHECK(mem_map_->GetAddress() != NULL);
  memset(mem_map_->GetAddress(), GC_CARD_CLEAN, length_);
}

/*
 * Returns the first address in the heap which maps to this card.
 */
void* CardTable::AddrFromCard(const byte *cardAddr) const {
  CHECK(IsValidCard(cardAddr));
  uintptr_t offset = cardAddr - biased_base_;
  return (void *)(offset << GC_CARD_SHIFT);
}

void CardTable::Scan(byte* base, byte* limit, Callback* visitor, void* arg) const {
  byte* cur = CardFromAddr(base);
  byte* end = CardFromAddr(limit);
  while (cur < end) {
    while (cur < end && *cur == GC_CARD_CLEAN) {
      cur++;
    }
    byte* run_start = cur;
    size_t run = 0;
    while (cur < end && *cur == GC_CARD_DIRTY) {
      run++;
      cur++;
    }
    if (run > 0) {
      byte* run_end = &cur[run];
      Heap::GetLiveBits()->VisitRange(reinterpret_cast<uintptr_t>(AddrFromCard(run_start)),
                                      reinterpret_cast<uintptr_t>(AddrFromCard(run_end)),
                                      visitor, arg);
    }
  }
}

/*
 * Verifies that gray objects are on a dirty card.
 */
void CardTable::VerifyCardTable() {
  UNIMPLEMENTED(WARNING) << "Card table verification";
}

}  // namespace art
