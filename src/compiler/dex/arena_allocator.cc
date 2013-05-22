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

#include "compiler_internals.h"
#include "dex_file-inl.h"
#include "arena_allocator.h"
#include "base/logging.h"

namespace art {

static const char* alloc_names[ArenaAllocator::kNumAllocKinds] = {
  "Misc       ",
  "BasicBlock ",
  "LIR        ",
  "MIR        ",
  "DataFlow   ",
  "GrowList   ",
  "GrowBitMap ",
  "Dalvik2SSA ",
  "DebugInfo  ",
  "Successor  ",
  "RegAlloc   ",
  "Data       ",
  "Preds      ",
};

ArenaAllocator::ArenaAllocator(size_t default_size)
  : default_size_(default_size),
    block_size_(default_size - sizeof(ArenaMemBlock)),
    arena_head_(NULL),
    current_block_(NULL),
    num_arena_blocks_(0),
    malloc_bytes_(0),
    lost_bytes_(0),
    num_allocations_(0) {
  memset(&alloc_stats_[0], 0, sizeof(alloc_stats_));
  // Start with an empty arena.
  arena_head_ = current_block_ = EmptyArenaBlock();
  num_arena_blocks_++;
}

ArenaAllocator::~ArenaAllocator() {
  // Reclaim all the arena blocks allocated so far.
  ArenaMemBlock* head = arena_head_;
  while (head != NULL) {
    ArenaMemBlock* p = head;
    head = head->next;
    free(p);
  }
  arena_head_ = NULL;
  num_arena_blocks_ = 0;
}

// Return an arena with no storage for use as a sentinal.
ArenaAllocator::ArenaMemBlock* ArenaAllocator::EmptyArenaBlock() {
  ArenaMemBlock* res = static_cast<ArenaMemBlock*>(malloc(sizeof(ArenaMemBlock)));
  malloc_bytes_ += sizeof(ArenaMemBlock);
  res->block_size = 0;
  res->bytes_allocated = 0;
  res->next = NULL;
  return res;
}

// Arena-based malloc for compilation tasks.
void* ArenaAllocator::NewMem(size_t size, bool zero, ArenaAllocKind kind) {
  DCHECK(current_block_ != NULL);
  DCHECK(arena_head_ != NULL);
  size = (size + 3) & ~3;
  alloc_stats_[kind] += size;
  ArenaMemBlock* allocation_block = current_block_;  // Assume we'll fit.
  size_t remaining_space = current_block_->block_size - current_block_->bytes_allocated;
  if (remaining_space < size) {
    /*
     * Time to allocate a new block.  If this is a large allocation or we have
     * significant space remaining in the current block then fulfill the allocation
     * request with a custom-sized malloc() - otherwise grab a new standard block.
     */
    size_t allocation_size = sizeof(ArenaMemBlock);
    if ((remaining_space >= ARENA_HIGH_WATER) || (size > block_size_)) {
      allocation_size += size;
    } else {
      allocation_size += block_size_;
    }
    ArenaMemBlock *new_block = static_cast<ArenaMemBlock*>(malloc(allocation_size));
    if (new_block == NULL) {
      LOG(FATAL) << "Arena allocation failure";
    }
    malloc_bytes_ += allocation_size;
    new_block->block_size = allocation_size - sizeof(ArenaMemBlock);
    new_block->bytes_allocated = 0;
    new_block->next = NULL;
    num_arena_blocks_++;
    /*
     * If the new block is completely full, insert it into the head of the list so we don't
     * bother trying to fit more and won't hide the potentially allocatable space on the
     * last (current_block_) block.  TUNING: if we move to a mark scheme, revisit
     * this code to keep allocation order intact.
     */
    if (new_block->block_size == size) {
      new_block->next = arena_head_;
      arena_head_ = new_block;
    } else {
      int lost = (current_block_->block_size - current_block_->bytes_allocated);
      lost_bytes_ += lost;
      current_block_->next = new_block;
      current_block_ = new_block;
    }
    allocation_block = new_block;
  }
  void* ptr = &allocation_block->ptr[allocation_block->bytes_allocated];
  allocation_block->bytes_allocated += size;
  if (zero) {
    memset(ptr, 0, size);
  }
  num_allocations_++;
  return ptr;
}

// Dump memory usage stats.
void ArenaAllocator::DumpMemStats(std::ostream& os) const {
  size_t total = 0;
  for (int i = 0; i < kNumAllocKinds; i++) {
    total += alloc_stats_[i];
  }
  os << " MEM: used: " << total << ", allocated: " << malloc_bytes_
     << ", lost: " << lost_bytes_ << "\n";
  os << "Number of blocks allocated: " << num_arena_blocks_ << ", Number of allocations: "
     << num_allocations_ << ", avg: " << total / num_allocations_ << "\n";
  os << "===== Allocation by kind\n";
  for (int i = 0; i < kNumAllocKinds; i++) {
      os << alloc_names[i] << std::setw(10) << alloc_stats_[i] << "\n";
  }
}

}  // namespace art
