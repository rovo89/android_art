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

#ifndef ART_SRC_COMPILER_DEX_COMPILER_ARENA_ALLOCATOR_H_
#define ART_SRC_COMPILER_DEX_COMPILER_ARENA_ALLOCATOR_H_

#include <stdint.h>
#include <stddef.h>
#include "compiler_enums.h"

namespace art {

#define ARENA_DEFAULT_BLOCK_SIZE (256 * 1024)
#define ARENA_HIGH_WATER (16 * 1024)

class ArenaAllocator {
  public:

    // Type of allocation for memory tuning.
    enum ArenaAllocKind {
      kAllocMisc,
      kAllocBB,
      kAllocLIR,
      kAllocMIR,
      kAllocDFInfo,
      kAllocGrowableArray,
      kAllocGrowableBitMap,
      kAllocDalvikToSSAMap,
      kAllocDebugInfo,
      kAllocSuccessor,
      kAllocRegAlloc,
      kAllocData,
      kAllocPredecessors,
      kNumAllocKinds
    };

  ArenaAllocator(size_t default_size = ARENA_DEFAULT_BLOCK_SIZE);
  ~ArenaAllocator();
  void* NewMem(size_t size, bool zero, ArenaAllocKind kind);
  size_t BytesAllocated() {
    return malloc_bytes_;
  }

  void DumpMemStats(std::ostream& os) const;

  private:

    // Variable-length allocation block.
    struct ArenaMemBlock {
      size_t block_size;
      size_t bytes_allocated;
      ArenaMemBlock *next;
      char ptr[0];
    };

    ArenaMemBlock* EmptyArenaBlock();

    size_t default_size_;                    // Smallest size of new allocation block.
    size_t block_size_;                      // Amount of allocatable bytes on a default block.
    ArenaMemBlock* arena_head_;              // Head of linked list of allocation blocks.
    ArenaMemBlock* current_block_;           // NOTE: code assumes there's always at least 1 block.
    int num_arena_blocks_;
    uint32_t malloc_bytes_;                  // Number of actual bytes malloc'd
    uint32_t alloc_stats_[kNumAllocKinds];   // Bytes used by various allocation kinds.
    uint32_t lost_bytes_;                    // Lost memory at end of too-small region
    uint32_t num_allocations_;

};  // ArenaAllocator


struct MemStats {
   public:
     void Dump(std::ostream& os) const {
       arena_.DumpMemStats(os);
     }
     MemStats(const ArenaAllocator &arena) : arena_(arena){};
  private:
    const ArenaAllocator &arena_;
}; // MemStats

}  // namespace art

#endif  // ART_SRC_COMPILER_DEX_COMPILER_ARENA_ALLOCATOR_H_
