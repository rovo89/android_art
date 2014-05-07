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

#ifndef ART_COMPILER_UTILS_ARENA_ALLOCATOR_H_
#define ART_COMPILER_UTILS_ARENA_ALLOCATOR_H_

#include <stdint.h>
#include <stddef.h>

#include "base/macros.h"
#include "base/mutex.h"
#include "mem_map.h"
#include "utils.h"

namespace art {

class Arena;
class ArenaPool;
class ArenaAllocator;
class ArenaStack;
class ScopedArenaAllocator;
class MemStats;

static constexpr bool kArenaAllocatorCountAllocations = false;

// Type of allocation for memory tuning.
enum ArenaAllocKind {
  kArenaAllocMisc,
  kArenaAllocBB,
  kArenaAllocLIR,
  kArenaAllocMIR,
  kArenaAllocDFInfo,
  kArenaAllocGrowableArray,
  kArenaAllocGrowableBitMap,
  kArenaAllocDalvikToSSAMap,
  kArenaAllocDebugInfo,
  kArenaAllocSuccessor,
  kArenaAllocRegAlloc,
  kArenaAllocData,
  kArenaAllocPredecessors,
  kArenaAllocSTL,
  kNumArenaAllocKinds
};

template <bool kCount>
class ArenaAllocatorStatsImpl;

template <>
class ArenaAllocatorStatsImpl<false> {
 public:
  ArenaAllocatorStatsImpl() = default;
  ArenaAllocatorStatsImpl(const ArenaAllocatorStatsImpl& other) = default;
  ArenaAllocatorStatsImpl& operator = (const ArenaAllocatorStatsImpl& other) = delete;

  void Copy(const ArenaAllocatorStatsImpl& other) { UNUSED(other); }
  void RecordAlloc(size_t bytes, ArenaAllocKind kind) { UNUSED(bytes); UNUSED(kind); }
  size_t NumAllocations() const { return 0u; }
  size_t BytesAllocated() const { return 0u; }
  void Dump(std::ostream& os, const Arena* first, ssize_t lost_bytes_adjustment) const {
    UNUSED(os); UNUSED(first); UNUSED(lost_bytes_adjustment);
  }
};

template <bool kCount>
class ArenaAllocatorStatsImpl {
 public:
  ArenaAllocatorStatsImpl();
  ArenaAllocatorStatsImpl(const ArenaAllocatorStatsImpl& other) = default;
  ArenaAllocatorStatsImpl& operator = (const ArenaAllocatorStatsImpl& other) = delete;

  void Copy(const ArenaAllocatorStatsImpl& other);
  void RecordAlloc(size_t bytes, ArenaAllocKind kind);
  size_t NumAllocations() const;
  size_t BytesAllocated() const;
  void Dump(std::ostream& os, const Arena* first, ssize_t lost_bytes_adjustment) const;

 private:
  size_t num_allocations_;
  // TODO: Use std::array<size_t, kNumArenaAllocKinds> from C++11 when we upgrade the STL.
  size_t alloc_stats_[kNumArenaAllocKinds];  // Bytes used by various allocation kinds.

  static const char* kAllocNames[kNumArenaAllocKinds];
};

typedef ArenaAllocatorStatsImpl<kArenaAllocatorCountAllocations> ArenaAllocatorStats;

class Arena {
 public:
  static constexpr size_t kDefaultSize = 128 * KB;
  explicit Arena(size_t size = kDefaultSize);
  ~Arena();
  void Reset();
  uint8_t* Begin() {
    return memory_;
  }

  uint8_t* End() {
    return memory_ + size_;
  }

  size_t Size() const {
    return size_;
  }

  size_t RemainingSpace() const {
    return Size() - bytes_allocated_;
  }

 private:
  size_t bytes_allocated_;
  uint8_t* memory_;
  size_t size_;
  MemMap* map_;
  Arena* next_;
  friend class ArenaPool;
  friend class ArenaAllocator;
  friend class ArenaStack;
  friend class ScopedArenaAllocator;
  template <bool kCount> friend class ArenaAllocatorStatsImpl;
  DISALLOW_COPY_AND_ASSIGN(Arena);
};

class ArenaPool {
 public:
  ArenaPool();
  ~ArenaPool();
  Arena* AllocArena(size_t size);
  void FreeArenaChain(Arena* first);

 private:
  Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  Arena* free_arenas_ GUARDED_BY(lock_);
  DISALLOW_COPY_AND_ASSIGN(ArenaPool);
};

class ArenaAllocator : private ArenaAllocatorStats {
 public:
  explicit ArenaAllocator(ArenaPool* pool);
  ~ArenaAllocator();

  // Returns zeroed memory.
  void* Alloc(size_t bytes, ArenaAllocKind kind) ALWAYS_INLINE {
    if (UNLIKELY(running_on_valgrind_)) {
      return AllocValgrind(bytes, kind);
    }
    bytes = RoundUp(bytes, 4);
    if (UNLIKELY(ptr_ + bytes > end_)) {
      // Obtain a new block.
      ObtainNewArenaForAllocation(bytes);
      if (UNLIKELY(ptr_ == nullptr)) {
        return nullptr;
      }
    }
    ArenaAllocatorStats::RecordAlloc(bytes, kind);
    uint8_t* ret = ptr_;
    ptr_ += bytes;
    return ret;
  }

  void* AllocValgrind(size_t bytes, ArenaAllocKind kind);
  void ObtainNewArenaForAllocation(size_t allocation_size);
  size_t BytesAllocated() const;
  MemStats GetMemStats() const;

 private:
  void UpdateBytesAllocated();

  ArenaPool* pool_;
  uint8_t* begin_;
  uint8_t* end_;
  uint8_t* ptr_;
  Arena* arena_head_;
  bool running_on_valgrind_;

  DISALLOW_COPY_AND_ASSIGN(ArenaAllocator);
};  // ArenaAllocator

class MemStats {
 public:
  MemStats(const char* name, const ArenaAllocatorStats* stats, const Arena* first_arena,
           ssize_t lost_bytes_adjustment = 0);
  void Dump(std::ostream& os) const;

 private:
  const char* const name_;
  const ArenaAllocatorStats* const stats_;
  const Arena* const first_arena_;
  const ssize_t lost_bytes_adjustment_;
};  // MemStats

}  // namespace art

#endif  // ART_COMPILER_UTILS_ARENA_ALLOCATOR_H_
