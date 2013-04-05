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
    current_arena_(NULL),
    num_arena_blocks_(0),
    malloc_bytes_(0) {
  memset(&alloc_stats_[0], 0, sizeof(alloc_stats_));
  // Start with an empty arena.
  arena_head_ = current_arena_ = EmptyArena();
  num_arena_blocks_++;
}

// Return an arena with no storage for use as a sentinal.
ArenaAllocator::ArenaMemBlock* ArenaAllocator::EmptyArena() {
  ArenaMemBlock* res = static_cast<ArenaMemBlock*>(malloc(sizeof(ArenaMemBlock)));
  malloc_bytes_ += sizeof(ArenaMemBlock);
  res->block_size = 0;
  res->bytes_allocated = 0;
  res->next = NULL;
  return res;
}

// Arena-based malloc for compilation tasks.
void* ArenaAllocator::NewMem(size_t size, bool zero, ArenaAllocKind kind) {
  DCHECK(current_arena_ != NULL);
  DCHECK(arena_head_ != NULL);
  size = (size + 3) & ~3;
  alloc_stats_[kind] += size;
  if (size + current_arena_->bytes_allocated > current_arena_->block_size) {
    size_t block_size = (size <= block_size_) ?  block_size_ : size;
    /* Time to allocate a new block */
    size_t allocation_size = sizeof(ArenaMemBlock) + block_size;
    ArenaMemBlock *new_arena =
        static_cast<ArenaMemBlock*>(malloc(allocation_size));
    if (new_arena == NULL) {
      LOG(FATAL) << "Arena allocation failure";
    }
    malloc_bytes_ += allocation_size;
    new_arena->block_size = block_size;
    new_arena->bytes_allocated = 0;
    new_arena->next = NULL;
    current_arena_->next = new_arena;
    current_arena_ = new_arena;
    num_arena_blocks_++;
  }
  void* ptr = &current_arena_->ptr[current_arena_->bytes_allocated];
  current_arena_->bytes_allocated += size;
  if (zero) {
    memset(ptr, 0, size);
  }
  return ptr;
}

// Reclaim all the arena blocks allocated so far.
void ArenaAllocator::ArenaReset() {
  ArenaMemBlock* head = arena_head_;
  while (head != NULL) {
    ArenaMemBlock* p = head;
    head = head->next;
    free(p);
  }
  // We must always have an arena.  Create a zero-length one.
  arena_head_ = current_arena_ = EmptyArena();
  num_arena_blocks_ = 1;
}

// Dump memory usage stats.
void ArenaAllocator::DumpMemStats(std::ostream& os) const {
  size_t total = 0;
  for (int i = 0; i < kNumAllocKinds; i++) {
    total += alloc_stats_[i];
  }
  os << " MEM: used: " << total << ", allocated: " << malloc_bytes_ << ", wasted: "
     << malloc_bytes_ - (total + (num_arena_blocks_ * sizeof(ArenaMemBlock))) << "\n";
  os << "Number of arenas allocated: " << num_arena_blocks_ << "\n";
  os << "===== Allocation by kind\n";
  for (int i = 0; i < kNumAllocKinds; i++) {
      os << alloc_names[i] << std::setw(10) << alloc_stats_[i] << "\n";
  }
}

}  // namespace art
