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

#include <dynamic_annotations.h>

#include "heap.h"
#include "heap_bitmap.h"
#include "logging.h"
#include "runtime.h"
#include "space.h"
#include "utils.h"

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
 * byte is equal to GC_DIRTY_CARD. See CardTable::Create for details.
 */

CardTable* CardTable::Create(const byte* heap_begin, size_t heap_capacity) {
  /* Set up the card table */
  size_t capacity = heap_capacity / GC_CARD_SIZE;
  /* Allocate an extra 256 bytes to allow fixed low-byte of base */
  UniquePtr<MemMap> mem_map(MemMap::MapAnonymous("dalvik-card-table", NULL,
                                                 capacity + 256, PROT_READ | PROT_WRITE));
  if (mem_map.get() == NULL) {
    std::string maps;
    ReadFileToString("/proc/self/maps", &maps);
    LOG(FATAL) << "couldn't allocate card table\n" << maps;
  }
  // All zeros is the correct initial value; all clean. Anonymous mmaps are initialized to zero, we
  // don't clear the card table to avoid unnecessary pages being allocated
  CHECK_EQ(GC_CARD_CLEAN, 0);

  byte* cardtable_begin = mem_map->Begin();
  CHECK(cardtable_begin != NULL);

  // We allocated up to a bytes worth of extra space to allow biased_begin's byte value to equal
  // GC_CARD_DIRTY, compute a offset value to make this the case
  size_t offset = 0;
  byte* biased_begin = reinterpret_cast<byte*>(reinterpret_cast<uintptr_t>(cardtable_begin) -
      (reinterpret_cast<uintptr_t>(heap_begin) >> GC_CARD_SHIFT));
  if (((uintptr_t)biased_begin & 0xff) != GC_CARD_DIRTY) {
    int delta = GC_CARD_DIRTY - (reinterpret_cast<int>(biased_begin) & 0xff);
    offset = delta + (delta < 0 ? 0x100 : 0);
    biased_begin += offset;
  }
  CHECK_EQ(reinterpret_cast<int>(biased_begin) & 0xff, GC_CARD_DIRTY);

  return new CardTable(mem_map.release(), biased_begin, offset);
}

CardTable::CardTable(MemMap* mem_map, byte* biased_begin, size_t offset)
    : mem_map_(mem_map), biased_begin_(biased_begin), offset_(offset) {
  byte* __attribute__((unused)) begin = mem_map_->Begin() + offset_;
  byte* __attribute__((unused)) end = mem_map_->End();
  ANNOTATE_BENIGN_RACE_SIZED(begin, (end - begin), "writes to GC card table");
}

void CardTable::ClearNonImageSpaceCards(Heap* heap) {
  // TODO: clear just the range of the table that has been modified
  const std::vector<Space*>& spaces = heap->GetSpaces();
  for (size_t i = 0; i < spaces.size(); ++i) {
    if (!spaces[i]->IsImageSpace()) {
      byte* card_start = CardFromAddr(spaces[i]->Begin());
      byte* card_end = CardFromAddr(spaces[i]->End());
      memset(reinterpret_cast<void*>(card_start), GC_CARD_CLEAN, card_end - card_start);
    }
  }
}

void CardTable::ClearCardTable() {
  // TODO: clear just the range of the table that has been modified
  memset(mem_map_->Begin(), GC_CARD_CLEAN, mem_map_->Size());
}

void CardTable::CheckAddrIsInCardTable(const byte* addr) const {
  byte* card_addr = biased_begin_ + ((uintptr_t)addr >> GC_CARD_SHIFT);
  if (!IsValidCard(card_addr)) {
    byte* begin = mem_map_->Begin() + offset_;
    byte* end = mem_map_->End();
    LOG(FATAL) << "Cardtable - begin: " << reinterpret_cast<void*>(begin)
               << " end: " << reinterpret_cast<void*>(end)
               << " addr: " << reinterpret_cast<const void*>(addr)
               << " card_addr: " << reinterpret_cast<void*>(card_addr);
  }
}

void CardTable::Scan(HeapBitmap* bitmap, byte* heap_begin, byte* heap_end, Callback* visitor, void* arg) const {
  byte* card_cur = CardFromAddr(heap_begin);
  byte* card_end = CardFromAddr(heap_end);
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
      bitmap->VisitRange(reinterpret_cast<uintptr_t>(AddrFromCard(run_start)),
                                      reinterpret_cast<uintptr_t>(AddrFromCard(run_end)),
                                      visitor, arg);
    }
  }
}

void CardTable::VerifyCardTable() {
  UNIMPLEMENTED(WARNING) << "Card table verification";
}

}  // namespace art
