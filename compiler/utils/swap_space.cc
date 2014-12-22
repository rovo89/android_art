/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "swap_space.h"

#include <algorithm>
#include <numeric>

#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "thread-inl.h"

namespace art {

// The chunk size by which the swap file is increased and mapped.
static constexpr size_t kMininumMapSize = 16 * MB;

static constexpr bool kCheckFreeMaps = false;

template <typename FreeBySizeSet>
static void DumpFreeMap(const FreeBySizeSet& free_by_size) {
  size_t last_size = static_cast<size_t>(-1);
  for (const auto& entry : free_by_size) {
    if (last_size != entry.first) {
      last_size = entry.first;
      LOG(INFO) << "Size " << last_size;
    }
    LOG(INFO) << "  0x" << std::hex << entry.second->Start()
        << " size=" << std::dec << entry.second->size;
  }
}

template <typename FreeByStartSet, typename FreeBySizeSet>
static void RemoveChunk(FreeByStartSet* free_by_start,
                        FreeBySizeSet* free_by_size,
                        typename FreeBySizeSet::const_iterator free_by_size_pos) {
  auto free_by_start_pos = free_by_size_pos->second;
  free_by_size->erase(free_by_size_pos);
  free_by_start->erase(free_by_start_pos);
}

template <typename FreeByStartSet, typename FreeBySizeSet>
static void InsertChunk(FreeByStartSet* free_by_start,
                        FreeBySizeSet* free_by_size,
                        const SpaceChunk& chunk) {
  DCHECK_NE(chunk.size, 0u);
  auto insert_result = free_by_start->insert(chunk);
  DCHECK(insert_result.second);
  free_by_size->emplace(chunk.size, insert_result.first);
}

SwapSpace::SwapSpace(int fd, size_t initial_size)
    : fd_(fd),
      size_(0),
      lock_("SwapSpace lock", static_cast<LockLevel>(LockLevel::kDefaultMutexLevel - 1)) {
  // Assume that the file is unlinked.

  InsertChunk(&free_by_start_, &free_by_size_, NewFileChunk(initial_size));
}

SwapSpace::~SwapSpace() {
  // All arenas are backed by the same file. Just close the descriptor.
  close(fd_);
}

template <typename FreeByStartSet, typename FreeBySizeSet>
static size_t CollectFree(const FreeByStartSet& free_by_start, const FreeBySizeSet& free_by_size) {
  if (free_by_start.size() != free_by_size.size()) {
    LOG(FATAL) << "Size: " << free_by_start.size() << " vs " << free_by_size.size();
  }

  // Calculate over free_by_size.
  size_t sum1 = 0;
  for (const auto& entry : free_by_size) {
    sum1 += entry.second->size;
  }

  // Calculate over free_by_start.
  size_t sum2 = 0;
  for (const auto& entry : free_by_start) {
    sum2 += entry.size;
  }

  if (sum1 != sum2) {
    LOG(FATAL) << "Sum: " << sum1 << " vs " << sum2;
  }
  return sum1;
}

void* SwapSpace::Alloc(size_t size) {
  MutexLock lock(Thread::Current(), lock_);
  size = RoundUp(size, 8U);

  // Check the free list for something that fits.
  // TODO: Smarter implementation. Global biggest chunk, ...
  SpaceChunk old_chunk;
  auto it = free_by_start_.empty()
      ? free_by_size_.end()
      : free_by_size_.lower_bound(FreeBySizeEntry { size, free_by_start_.begin() });
  if (it != free_by_size_.end()) {
    old_chunk = *it->second;
    RemoveChunk(&free_by_start_, &free_by_size_, it);
  } else {
    // Not a big enough free chunk, need to increase file size.
    old_chunk = NewFileChunk(size);
  }

  void* ret = old_chunk.ptr;

  if (old_chunk.size != size) {
    // Insert the remainder.
    SpaceChunk new_chunk = { old_chunk.ptr + size, old_chunk.size - size };
    InsertChunk(&free_by_start_, &free_by_size_, new_chunk);
  }

  return ret;
}

SpaceChunk SwapSpace::NewFileChunk(size_t min_size) {
#if !defined(__APPLE__)
  size_t next_part = std::max(RoundUp(min_size, kPageSize), RoundUp(kMininumMapSize, kPageSize));
  int result = TEMP_FAILURE_RETRY(ftruncate64(fd_, size_ + next_part));
  if (result != 0) {
    PLOG(FATAL) << "Unable to increase swap file.";
  }
  uint8_t* ptr = reinterpret_cast<uint8_t*>(
      mmap(nullptr, next_part, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, size_));
  if (ptr == MAP_FAILED) {
    LOG(ERROR) << "Unable to mmap new swap file chunk.";
    LOG(ERROR) << "Current size: " << size_ << " requested: " << next_part << "/" << min_size;
    LOG(ERROR) << "Free list:";
    MutexLock lock(Thread::Current(), lock_);
    DumpFreeMap(free_by_size_);
    LOG(ERROR) << "In free list: " << CollectFree(free_by_start_, free_by_size_);
    LOG(FATAL) << "Aborting...";
  }
  size_ += next_part;
  SpaceChunk new_chunk = {ptr, next_part};
  maps_.push_back(new_chunk);
  return new_chunk;
#else
  UNUSED(kMininumMapSize);
  LOG(FATAL) << "No swap file support on the Mac.";
  return {nullptr, 0U};  // NOLINT [readability/braces] [4]
#endif
}

// TODO: Full coalescing.
void SwapSpace::Free(void* ptrV, size_t size) {
  MutexLock lock(Thread::Current(), lock_);
  size = RoundUp(size, 8U);

  size_t free_before = 0;
  if (kCheckFreeMaps) {
    free_before = CollectFree(free_by_start_, free_by_size_);
  }

  SpaceChunk chunk = { reinterpret_cast<uint8_t*>(ptrV), size };
  auto it = free_by_start_.lower_bound(chunk);
  if (it != free_by_start_.begin()) {
    auto prev = it;
    --prev;
    CHECK_LE(prev->End(), chunk.Start());
    if (prev->End() == chunk.Start()) {
      // Merge *prev with this chunk.
      chunk.size += prev->size;
      chunk.ptr -= prev->size;
      auto erase_pos = free_by_size_.find(FreeBySizeEntry { prev->size, prev });
      DCHECK(erase_pos != free_by_size_.end());
      RemoveChunk(&free_by_start_, &free_by_size_, erase_pos);
      // "prev" is invalidated but "it" remains valid.
    }
  }
  if (it != free_by_start_.end()) {
    CHECK_LE(chunk.End(), it->Start());
    if (chunk.End() == it->Start()) {
      // Merge *it with this chunk.
      chunk.size += it->size;
      auto erase_pos = free_by_size_.find(FreeBySizeEntry { it->size, it });
      DCHECK(erase_pos != free_by_size_.end());
      RemoveChunk(&free_by_start_, &free_by_size_, erase_pos);
      // "it" is invalidated but we don't need it anymore.
    }
  }
  InsertChunk(&free_by_start_, &free_by_size_, chunk);

  if (kCheckFreeMaps) {
    size_t free_after = CollectFree(free_by_start_, free_by_size_);

    if (free_after != free_before + size) {
      DumpFreeMap(free_by_size_);
      CHECK_EQ(free_after, free_before + size) << "Should be " << size << " difference from " << free_before;
    }
  }
}

}  // namespace art
