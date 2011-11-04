// Copyright 2011 Google Inc. All Rights Reserved.

/*
 * Maintain a card table from the the write barrier. All writes of
 * non-NULL values to heap addresses should go through an entry in
 * WriteBarrier, and from there to here.
 */

#ifndef DALVIK_ALLOC_CARDTABLE_H_
#define DALVIK_ALLOC_CARDTABLE_H_

#include "globals.h"
#include "logging.h"
#include "mem_map.h"
#include "UniquePtr.h"

namespace art {

class Object;

#define GC_CARD_SHIFT 7
#define GC_CARD_SIZE (1 << GC_CARD_SHIFT)
#define GC_CARD_CLEAN 0
#define GC_CARD_DIRTY 0x70

class CardTable {
 public:
  typedef void Callback(Object* obj, void* arg);

  static CardTable* Create(const byte* heap_base, size_t heap_max_size, size_t growth_size);

  /*
   * Set the card associated with the given address to GC_CARD_DIRTY.
   */
  void MarkCard(const void *addr) {
    byte* cardAddr = CardFromAddr(addr);
    *cardAddr = GC_CARD_DIRTY;
  }

  byte* GetBiasedBase() {
    return biased_base_;
  }

  void Scan(byte* base, byte* limit, Callback* visitor, void* arg) const;

  bool IsDirty(const Object* obj) const {
    return *CardFromAddr(obj) == GC_CARD_DIRTY;
  }

  void ClearGrowthLimit() {
    CHECK_GE(max_length_, length_);
    length_ = max_length_;
  }

 private:

  CardTable() {}

  /*
   * Initializes the card table; must be called before any other
   * CardTable functions.
   */
  bool Init(const byte* heap_base, size_t heap_max_size, size_t growth_size);

  /*
   * Resets all of the bytes in the card table to clean.
   */
  void ClearCardTable();

  /*
   * Returns the address of the relevant byte in the card table, given
   * an address on the heap.
   */
  byte* CardFromAddr(const void *addr) const {
    byte *cardAddr = biased_base_ + ((uintptr_t)addr >> GC_CARD_SHIFT);
    CHECK(IsValidCard(cardAddr));
    return cardAddr;
  }

  /*
   * Returns the first address in the heap which maps to this card.
   */
  void* AddrFromCard(const byte *card) const;

  /*
   * Returns true iff the address is within the bounds of the card table.
   */
  bool IsValidCard(const byte* cardAddr) const {
    byte* begin = mem_map_->GetAddress() + offset_;
    byte* end = &begin[length_];
    return cardAddr >= begin && cardAddr < end;
  }

  /*
   * Verifies that all gray objects are on a dirty card.
   */
  void VerifyCardTable();


  UniquePtr<MemMap> mem_map_;
  byte* base_;
  byte* biased_base_;
  size_t length_;
  size_t max_length_;
  size_t offset_;
};

}  // namespace art
#endif  // DALVIK_ALLOC_CARDTABLE_H_
