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

#ifndef ART_COMPILER_UTILS_ARENA_BIT_VECTOR_H_
#define ART_COMPILER_UTILS_ARENA_BIT_VECTOR_H_

#include "base/bit_vector.h"
#include "utils/arena_allocator.h"
#include "utils/scoped_arena_allocator.h"

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
  kBitMapClInitCheck,
  kBitMapTmpBlockV,
  kBitMapPredecessors,
  kNumBitMapKinds
};

std::ostream& operator<<(std::ostream& os, const OatBitMapKind& kind);

/*
 * A BitVector implementation that uses Arena allocation.
 */
class ArenaBitVector : public BitVector {
  public:
    ArenaBitVector(ArenaAllocator* arena, uint32_t start_bits, bool expandable,
                   OatBitMapKind kind = kBitMapMisc);
    ArenaBitVector(ScopedArenaAllocator* arena, uint32_t start_bits, bool expandable,
                   OatBitMapKind kind = kBitMapMisc);
    ~ArenaBitVector() {}

  static void* operator new(size_t size, ArenaAllocator* arena) {
     return arena->Alloc(sizeof(ArenaBitVector), kArenaAllocGrowableBitMap);
  }
  static void* operator new(size_t size, ScopedArenaAllocator* arena) {
     return arena->Alloc(sizeof(ArenaBitVector), kArenaAllocGrowableBitMap);
  }
  static void operator delete(void* p) {}  // Nop.

  private:
    const OatBitMapKind kind_;      // for memory use tuning. TODO: currently unused.
};


}  // namespace art

#endif  // ART_COMPILER_UTILS_ARENA_BIT_VECTOR_H_
