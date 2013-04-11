/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_DEX_COMPILER_ARENA_BIT_VECTOR_H_
#define ART_SRC_COMPILER_DEX_COMPILER_ARENA_BIT_VECTOR_H_

#include <stdint.h>
#include <stddef.h>
#include "compiler_enums.h"
#include "arena_allocator.h"

namespace art {

// Type of growable bitmap for memory tuning.
enum OatBitMapKind {
  kBitMapMisc = 0,
  kBitMapUse,
  kBitMapDef,
  kBitMapLiveIn,
  kBitMapBMatrix,
  kBitMapDominators,
  kBitMapIDominated,
  kBitMapDomFrontier,
  kBitMapPhi,
  kBitMapTmpBlocks,
  kBitMapInputBlocks,
  kBitMapRegisterV,
  kBitMapTempSSARegisterV,
  kBitMapNullCheck,
  kBitMapTmpBlockV,
  kBitMapPredecessors,
  kNumBitMapKinds
};

/*
 * Expanding bitmap, used for tracking resources.  Bits are numbered starting
 * from zero.  All operations on a BitVector are unsynchronized.
 */
class ArenaBitVector {
  public:

    class Iterator {
      public:
        Iterator(ArenaBitVector* bit_vector)
          : p_bits_(bit_vector),
            bit_storage_(bit_vector->GetRawStorage()),
            bit_index_(0),
            bit_size_(p_bits_->storage_size_ * sizeof(uint32_t) * 8) {};

        int Next();  // Returns -1 when no next.

        static void* operator new(size_t size, ArenaAllocator* arena) {
          return arena->NewMem(sizeof(ArenaBitVector::Iterator), true,
                               ArenaAllocator::kAllocGrowableBitMap);
        };
        static void operator delete(void* p) {};  // Nop.

      private:
        ArenaBitVector* const p_bits_;
        uint32_t* const bit_storage_;
        uint32_t bit_index_;              // Current index (size in bits).
        const uint32_t bit_size_;       // Size of vector in bits.
    };

    ArenaBitVector(ArenaAllocator* arena, unsigned int start_bits, bool expandable,
                   OatBitMapKind kind = kBitMapMisc);
    ~ArenaBitVector() {};

    static void* operator new( size_t size, ArenaAllocator* arena) {
      return arena->NewMem(sizeof(ArenaBitVector), true, ArenaAllocator::kAllocGrowableBitMap);
    }
    static void operator delete(void* p) {};  // Nop.

    void SetBit(unsigned int num);
    void ClearBit(unsigned int num);
    void MarkAllBits(bool set);
    void DebugBitVector(char* msg, int length);
    bool IsBitSet(unsigned int num);
    void ClearAllBits();
    void SetInitialBits(unsigned int num_bits);
    void Copy(ArenaBitVector* src);
    void Intersect(const ArenaBitVector* src2);
    void Union(const ArenaBitVector* src);
    bool Equal(const ArenaBitVector* src);
    int NumSetBits();

    uint32_t GetStorageSize() const { return storage_size_; }
    bool IsExpandable() const { return expandable_; }
    uint32_t GetRawStorageWord(size_t idx) const { return storage_[idx]; }
    uint32_t* GetRawStorage() { return storage_; }

  private:
    ArenaAllocator* const arena_;
    const bool expandable_;         // expand bitmap if we run out?
    const OatBitMapKind kind_;      // for memory use tuning.
    uint32_t   storage_size_;       // current size, in 32-bit words.
    uint32_t*  storage_;
};


}  // namespace art

#endif  // ART_SRC_COMPILER_DEX_COMPILER_ARENA_BIT_VECTOR_H_
