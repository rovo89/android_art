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

#include <algorithm>
#include <numeric>

#include "arena_allocator.h"
#include "base/logging.h"
#include "base/mutex.h"
#include "thread-inl.h"
#include <memcheck/memcheck.h>

namespace art {

// Memmap is a bit slower than malloc according to my measurements.
static constexpr bool kUseMemMap = false;
static constexpr bool kUseMemSet = true && kUseMemMap;
static constexpr size_t kValgrindRedZoneBytes = 8;
constexpr size_t Arena::kDefaultSize;

template <bool kCount>
const char* ArenaAllocatorStatsImpl<kCount>::kAllocNames[kNumArenaAllocKinds] = {
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
  "STL        ",
};

template <bool kCount>
ArenaAllocatorStatsImpl<kCount>::ArenaAllocatorStatsImpl()
    : num_allocations_(0u) {
  std::fill_n(alloc_stats_, arraysize(alloc_stats_), 0u);
}

template <bool kCount>
void ArenaAllocatorStatsImpl<kCount>::Copy(const ArenaAllocatorStatsImpl& other) {
  num_allocations_ = other.num_allocations_;
  std::copy(other.alloc_stats_, other.alloc_stats_ + arraysize(alloc_stats_), alloc_stats_);
}

template <bool kCount>
void ArenaAllocatorStatsImpl<kCount>::RecordAlloc(size_t bytes, ArenaAllocKind kind) {
  alloc_stats_[kind] += bytes;
  ++num_allocations_;
}

template <bool kCount>
size_t ArenaAllocatorStatsImpl<kCount>::NumAllocations() const {
  return num_allocations_;
}

template <bool kCount>
size_t ArenaAllocatorStatsImpl<kCount>::BytesAllocated() const {
  const size_t init = 0u;  // Initial value of the correct type.
  return std::accumulate(alloc_stats_, alloc_stats_ + arraysize(alloc_stats_), init);
}

template <bool kCount>
void ArenaAllocatorStatsImpl<kCount>::Dump(std::ostream& os, const Arena* first,
                                           ssize_t lost_bytes_adjustment) const {
  size_t malloc_bytes = 0u;
  size_t lost_bytes = 0u;
  size_t num_arenas = 0u;
  for (const Arena* arena = first; arena != nullptr; arena = arena->next_) {
    malloc_bytes += arena->Size();
    lost_bytes += arena->RemainingSpace();
    ++num_arenas;
  }
  // The lost_bytes_adjustment is used to make up for the fact that the current arena
  // may not have the bytes_allocated_ updated correctly.
  lost_bytes += lost_bytes_adjustment;
  const size_t bytes_allocated = BytesAllocated();
  os << " MEM: used: " << bytes_allocated << ", allocated: " << malloc_bytes
     << ", lost: " << lost_bytes << "\n";
  size_t num_allocations = NumAllocations();
  if (num_allocations != 0) {
    os << "Number of arenas allocated: " << num_arenas << ", Number of allocations: "
       << num_allocations << ", avg size: " << bytes_allocated / num_allocations << "\n";
  }
  os << "===== Allocation by kind\n";
  for (int i = 0; i < kNumArenaAllocKinds; i++) {
      os << kAllocNames[i] << std::setw(10) << alloc_stats_[i] << "\n";
  }
}

// Explicitly instantiate the used implementation.
template class ArenaAllocatorStatsImpl<kArenaAllocatorCountAllocations>;

Arena::Arena(size_t size)
    : bytes_allocated_(0),
      map_(nullptr),
      next_(nullptr) {
  if (kUseMemMap) {
    std::string error_msg;
    map_ = MemMap::MapAnonymous("dalvik-arena", NULL, size, PROT_READ | PROT_WRITE, false,
                                &error_msg);
    CHECK(map_ != nullptr) << error_msg;
    memory_ = map_->Begin();
    size_ = map_->Size();
  } else {
    memory_ = reinterpret_cast<uint8_t*>(calloc(1, size));
    size_ = size;
  }
}

Arena::~Arena() {
  if (kUseMemMap) {
    delete map_;
  } else {
    free(reinterpret_cast<void*>(memory_));
  }
}

void Arena::Reset() {
  if (bytes_allocated_) {
    if (kUseMemSet || !kUseMemMap) {
      memset(Begin(), 0, bytes_allocated_);
    } else {
      madvise(Begin(), bytes_allocated_, MADV_DONTNEED);
    }
    bytes_allocated_ = 0;
  }
}

ArenaPool::ArenaPool()
    : lock_("Arena pool lock"),
      free_arenas_(nullptr) {
}

ArenaPool::~ArenaPool() {
  while (free_arenas_ != nullptr) {
    auto* arena = free_arenas_;
    free_arenas_ = free_arenas_->next_;
    delete arena;
  }
}

Arena* ArenaPool::AllocArena(size_t size) {
  Thread* self = Thread::Current();
  Arena* ret = nullptr;
  {
    MutexLock lock(self, lock_);
    if (free_arenas_ != nullptr && LIKELY(free_arenas_->Size() >= size)) {
      ret = free_arenas_;
      free_arenas_ = free_arenas_->next_;
    }
  }
  if (ret == nullptr) {
    ret = new Arena(size);
  }
  ret->Reset();
  return ret;
}

void ArenaPool::FreeArenaChain(Arena* first) {
  if (UNLIKELY(RUNNING_ON_VALGRIND > 0)) {
    for (Arena* arena = first; arena != nullptr; arena = arena->next_) {
      VALGRIND_MAKE_MEM_UNDEFINED(arena->memory_, arena->bytes_allocated_);
    }
  }
  if (first != nullptr) {
    Arena* last = first;
    while (last->next_ != nullptr) {
      last = last->next_;
    }
    Thread* self = Thread::Current();
    MutexLock lock(self, lock_);
    last->next_ = free_arenas_;
    free_arenas_ = first;
  }
}

size_t ArenaAllocator::BytesAllocated() const {
  return ArenaAllocatorStats::BytesAllocated();
}

ArenaAllocator::ArenaAllocator(ArenaPool* pool)
  : pool_(pool),
    begin_(nullptr),
    end_(nullptr),
    ptr_(nullptr),
    arena_head_(nullptr),
    running_on_valgrind_(RUNNING_ON_VALGRIND > 0) {
}

void ArenaAllocator::UpdateBytesAllocated() {
  if (arena_head_ != nullptr) {
    // Update how many bytes we have allocated into the arena so that the arena pool knows how
    // much memory to zero out.
    arena_head_->bytes_allocated_ = ptr_ - begin_;
  }
}

void* ArenaAllocator::AllocValgrind(size_t bytes, ArenaAllocKind kind) {
  size_t rounded_bytes = (bytes + 3 + kValgrindRedZoneBytes) & ~3;
  if (UNLIKELY(ptr_ + rounded_bytes > end_)) {
    // Obtain a new block.
    ObtainNewArenaForAllocation(rounded_bytes);
    if (UNLIKELY(ptr_ == nullptr)) {
      return nullptr;
    }
  }
  ArenaAllocatorStats::RecordAlloc(rounded_bytes, kind);
  uint8_t* ret = ptr_;
  ptr_ += rounded_bytes;
  // Check that the memory is already zeroed out.
  for (uint8_t* ptr = ret; ptr < ptr_; ++ptr) {
    CHECK_EQ(*ptr, 0U);
  }
  VALGRIND_MAKE_MEM_NOACCESS(ret + bytes, rounded_bytes - bytes);
  return ret;
}

ArenaAllocator::~ArenaAllocator() {
  // Reclaim all the arenas by giving them back to the thread pool.
  UpdateBytesAllocated();
  pool_->FreeArenaChain(arena_head_);
}

void ArenaAllocator::ObtainNewArenaForAllocation(size_t allocation_size) {
  UpdateBytesAllocated();
  Arena* new_arena = pool_->AllocArena(std::max(Arena::kDefaultSize, allocation_size));
  new_arena->next_ = arena_head_;
  arena_head_ = new_arena;
  // Update our internal data structures.
  ptr_ = begin_ = new_arena->Begin();
  end_ = new_arena->End();
}

MemStats::MemStats(const char* name, const ArenaAllocatorStats* stats, const Arena* first_arena,
                   ssize_t lost_bytes_adjustment)
    : name_(name),
      stats_(stats),
      first_arena_(first_arena),
      lost_bytes_adjustment_(lost_bytes_adjustment) {
}

void MemStats::Dump(std::ostream& os) const {
  os << name_ << " stats:\n";
  stats_->Dump(os, first_arena_, lost_bytes_adjustment_);
}

// Dump memory usage stats.
MemStats ArenaAllocator::GetMemStats() const {
  ssize_t lost_bytes_adjustment =
      (arena_head_ == nullptr) ? 0 : (end_ - ptr_) - arena_head_->RemainingSpace();
  return MemStats("ArenaAllocator", this, arena_head_, lost_bytes_adjustment);
}

}  // namespace art
