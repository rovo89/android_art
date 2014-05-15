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

#include "base/mutex-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "rosalloc.h"

#include <map>
#include <list>
#include <vector>

namespace art {
namespace gc {
namespace allocator {

extern "C" void* art_heap_rosalloc_morecore(RosAlloc* rosalloc, intptr_t increment);

static constexpr bool kUsePrefetchDuringAllocRun = true;
static constexpr bool kPrefetchNewRunDataByZeroing = false;
static constexpr size_t kPrefetchStride = 64;

size_t RosAlloc::bracketSizes[kNumOfSizeBrackets];
size_t RosAlloc::numOfPages[kNumOfSizeBrackets];
size_t RosAlloc::numOfSlots[kNumOfSizeBrackets];
size_t RosAlloc::headerSizes[kNumOfSizeBrackets];
size_t RosAlloc::bulkFreeBitMapOffsets[kNumOfSizeBrackets];
size_t RosAlloc::threadLocalFreeBitMapOffsets[kNumOfSizeBrackets];
bool RosAlloc::initialized_ = false;
size_t RosAlloc::dedicated_full_run_storage_[kPageSize / sizeof(size_t)] = { 0 };
RosAlloc::Run* RosAlloc::dedicated_full_run_ =
    reinterpret_cast<RosAlloc::Run*>(dedicated_full_run_storage_);

RosAlloc::RosAlloc(void* base, size_t capacity, size_t max_capacity,
                   PageReleaseMode page_release_mode, size_t page_release_size_threshold)
    : base_(reinterpret_cast<byte*>(base)), footprint_(capacity),
      capacity_(capacity), max_capacity_(max_capacity),
      lock_("rosalloc global lock", kRosAllocGlobalLock),
      bulk_free_lock_("rosalloc bulk free lock", kRosAllocBulkFreeLock),
      page_release_mode_(page_release_mode),
      page_release_size_threshold_(page_release_size_threshold) {
  DCHECK_EQ(RoundUp(capacity, kPageSize), capacity);
  DCHECK_EQ(RoundUp(max_capacity, kPageSize), max_capacity);
  CHECK_LE(capacity, max_capacity);
  CHECK(IsAligned<kPageSize>(page_release_size_threshold_));
  if (!initialized_) {
    Initialize();
  }
  VLOG(heap) << "RosAlloc base="
             << std::hex << (intptr_t)base_ << ", end="
             << std::hex << (intptr_t)(base_ + capacity_)
             << ", capacity=" << std::dec << capacity_
             << ", max_capacity=" << std::dec << max_capacity_;
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    size_bracket_lock_names[i] =
        StringPrintf("an rosalloc size bracket %d lock", static_cast<int>(i));
    size_bracket_locks_[i] = new Mutex(size_bracket_lock_names[i].c_str(), kRosAllocBracketLock);
    current_runs_[i] = dedicated_full_run_;
  }
  DCHECK_EQ(footprint_, capacity_);
  size_t num_of_pages = footprint_ / kPageSize;
  size_t max_num_of_pages = max_capacity_ / kPageSize;
  std::string error_msg;
  page_map_mem_map_.reset(MemMap::MapAnonymous("rosalloc page map", NULL, RoundUp(max_num_of_pages, kPageSize),
                                               PROT_READ | PROT_WRITE, false, &error_msg));
  CHECK(page_map_mem_map_.get() != nullptr) << "Couldn't allocate the page map : " << error_msg;
  page_map_ = page_map_mem_map_->Begin();
  page_map_size_ = num_of_pages;
  max_page_map_size_ = max_num_of_pages;
  free_page_run_size_map_.resize(num_of_pages);
  FreePageRun* free_pages = reinterpret_cast<FreePageRun*>(base_);
  if (kIsDebugBuild) {
    free_pages->magic_num_ = kMagicNumFree;
  }
  free_pages->SetByteSize(this, capacity_);
  DCHECK_EQ(capacity_ % kPageSize, static_cast<size_t>(0));
  DCHECK(free_pages->IsFree());
  free_pages->ReleasePages(this);
  DCHECK(free_pages->IsFree());
  free_page_runs_.insert(free_pages);
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::RosAlloc() : Inserted run 0x" << std::hex
              << reinterpret_cast<intptr_t>(free_pages)
              << " into free_page_runs_";
  }
}

RosAlloc::~RosAlloc() {
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    delete size_bracket_locks_[i];
  }
}

void* RosAlloc::AllocPages(Thread* self, size_t num_pages, byte page_map_type) {
  lock_.AssertHeld(self);
  DCHECK(page_map_type == kPageMapRun || page_map_type == kPageMapLargeObject);
  FreePageRun* res = NULL;
  const size_t req_byte_size = num_pages * kPageSize;
  // Find the lowest address free page run that's large enough.
  for (auto it = free_page_runs_.begin(); it != free_page_runs_.end(); ) {
    FreePageRun* fpr = *it;
    DCHECK(fpr->IsFree());
    size_t fpr_byte_size = fpr->ByteSize(this);
    DCHECK_EQ(fpr_byte_size % kPageSize, static_cast<size_t>(0));
    if (req_byte_size <= fpr_byte_size) {
      // Found one.
      free_page_runs_.erase(it++);
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::AllocPages() : Erased run 0x"
                  << std::hex << reinterpret_cast<intptr_t>(fpr)
                  << " from free_page_runs_";
      }
      if (req_byte_size < fpr_byte_size) {
        // Split.
        FreePageRun* remainder = reinterpret_cast<FreePageRun*>(reinterpret_cast<byte*>(fpr) + req_byte_size);
        if (kIsDebugBuild) {
          remainder->magic_num_ = kMagicNumFree;
        }
        remainder->SetByteSize(this, fpr_byte_size - req_byte_size);
        DCHECK_EQ(remainder->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        // Don't need to call madvise on remainder here.
        free_page_runs_.insert(remainder);
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::AllocPages() : Inserted run 0x" << std::hex
                    << reinterpret_cast<intptr_t>(remainder)
                    << " into free_page_runs_";
        }
        fpr->SetByteSize(this, req_byte_size);
        DCHECK_EQ(fpr->ByteSize(this) % kPageSize, static_cast<size_t>(0));
      }
      res = fpr;
      break;
    } else {
      ++it;
    }
  }

  // Failed to allocate pages. Grow the footprint, if possible.
  if (UNLIKELY(res == NULL && capacity_ > footprint_)) {
    FreePageRun* last_free_page_run = NULL;
    size_t last_free_page_run_size;
    auto it = free_page_runs_.rbegin();
    if (it != free_page_runs_.rend() && (last_free_page_run = *it)->End(this) == base_ + footprint_) {
      // There is a free page run at the end.
      DCHECK(last_free_page_run->IsFree());
      DCHECK_EQ(page_map_[ToPageMapIndex(last_free_page_run)], kPageMapEmpty);
      last_free_page_run_size = last_free_page_run->ByteSize(this);
    } else {
      // There is no free page run at the end.
      last_free_page_run_size = 0;
    }
    DCHECK_LT(last_free_page_run_size, req_byte_size);
    if (capacity_ - footprint_ + last_free_page_run_size >= req_byte_size) {
      // If we grow the heap, we can allocate it.
      size_t increment = std::min(std::max(2 * MB, req_byte_size - last_free_page_run_size),
                                  capacity_ - footprint_);
      DCHECK_EQ(increment % kPageSize, static_cast<size_t>(0));
      size_t new_footprint = footprint_ + increment;
      size_t new_num_of_pages = new_footprint / kPageSize;
      DCHECK_LT(page_map_size_, new_num_of_pages);
      DCHECK_LT(free_page_run_size_map_.size(), new_num_of_pages);
      page_map_size_ = new_num_of_pages;
      DCHECK_LE(page_map_size_, max_page_map_size_);
      free_page_run_size_map_.resize(new_num_of_pages);
      art_heap_rosalloc_morecore(this, increment);
      if (last_free_page_run_size > 0) {
        // There was a free page run at the end. Expand its size.
        DCHECK_EQ(last_free_page_run_size, last_free_page_run->ByteSize(this));
        last_free_page_run->SetByteSize(this, last_free_page_run_size + increment);
        DCHECK_EQ(last_free_page_run->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        DCHECK_EQ(last_free_page_run->End(this), base_ + new_footprint);
      } else {
        // Otherwise, insert a new free page run at the end.
        FreePageRun* new_free_page_run = reinterpret_cast<FreePageRun*>(base_ + footprint_);
        if (kIsDebugBuild) {
          new_free_page_run->magic_num_ = kMagicNumFree;
        }
        new_free_page_run->SetByteSize(this, increment);
        DCHECK_EQ(new_free_page_run->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        free_page_runs_.insert(new_free_page_run);
        DCHECK_EQ(*free_page_runs_.rbegin(), new_free_page_run);
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::AlloPages() : Grew the heap by inserting run 0x"
                    << std::hex << reinterpret_cast<intptr_t>(new_free_page_run)
                    << " into free_page_runs_";
        }
      }
      DCHECK_LE(footprint_ + increment, capacity_);
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::AllocPages() : increased the footprint from "
                  << footprint_ << " to " << new_footprint;
      }
      footprint_ = new_footprint;

      // And retry the last free page run.
      it = free_page_runs_.rbegin();
      DCHECK(it != free_page_runs_.rend());
      FreePageRun* fpr = *it;
      if (kIsDebugBuild && last_free_page_run_size > 0) {
        DCHECK(last_free_page_run != NULL);
        DCHECK_EQ(last_free_page_run, fpr);
      }
      size_t fpr_byte_size = fpr->ByteSize(this);
      DCHECK_EQ(fpr_byte_size % kPageSize, static_cast<size_t>(0));
      DCHECK_LE(req_byte_size, fpr_byte_size);
      free_page_runs_.erase(fpr);
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::AllocPages() : Erased run 0x" << std::hex << reinterpret_cast<intptr_t>(fpr)
                  << " from free_page_runs_";
      }
      if (req_byte_size < fpr_byte_size) {
        // Split if there's a remainder.
        FreePageRun* remainder = reinterpret_cast<FreePageRun*>(reinterpret_cast<byte*>(fpr) + req_byte_size);
        if (kIsDebugBuild) {
          remainder->magic_num_ = kMagicNumFree;
        }
        remainder->SetByteSize(this, fpr_byte_size - req_byte_size);
        DCHECK_EQ(remainder->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        free_page_runs_.insert(remainder);
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::AllocPages() : Inserted run 0x" << std::hex
                    << reinterpret_cast<intptr_t>(remainder)
                    << " into free_page_runs_";
        }
        fpr->SetByteSize(this, req_byte_size);
        DCHECK_EQ(fpr->ByteSize(this) % kPageSize, static_cast<size_t>(0));
      }
      res = fpr;
    }
  }
  if (LIKELY(res != NULL)) {
    // Update the page map.
    size_t page_map_idx = ToPageMapIndex(res);
    for (size_t i = 0; i < num_pages; i++) {
      DCHECK_EQ(page_map_[page_map_idx + i], kPageMapEmpty);
    }
    switch (page_map_type) {
    case kPageMapRun:
      page_map_[page_map_idx] = kPageMapRun;
      for (size_t i = 1; i < num_pages; i++) {
        page_map_[page_map_idx + i] = kPageMapRunPart;
      }
      break;
    case kPageMapLargeObject:
      page_map_[page_map_idx] = kPageMapLargeObject;
      for (size_t i = 1; i < num_pages; i++) {
        page_map_[page_map_idx + i] = kPageMapLargeObjectPart;
      }
      break;
    default:
      LOG(FATAL) << "Unreachable - page map type: " << page_map_type;
      break;
    }
    if (kIsDebugBuild) {
      // Clear the first page since it is not madvised due to the magic number.
      memset(res, 0, kPageSize);
    }
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::AllocPages() : 0x" << std::hex << reinterpret_cast<intptr_t>(res)
                << "-0x" << (reinterpret_cast<intptr_t>(res) + num_pages * kPageSize)
                << "(" << std::dec << (num_pages * kPageSize) << ")";
    }
    return res;
  }

  // Fail.
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::AllocPages() : NULL";
  }
  return nullptr;
}

size_t RosAlloc::FreePages(Thread* self, void* ptr, bool already_zero) {
  lock_.AssertHeld(self);
  size_t pm_idx = ToPageMapIndex(ptr);
  DCHECK_LT(pm_idx, page_map_size_);
  byte pm_type = page_map_[pm_idx];
  DCHECK(pm_type == kPageMapRun || pm_type == kPageMapLargeObject);
  byte pm_part_type;
  switch (pm_type) {
  case kPageMapRun:
    pm_part_type = kPageMapRunPart;
    break;
  case kPageMapLargeObject:
    pm_part_type = kPageMapLargeObjectPart;
    break;
  default:
    pm_part_type = kPageMapEmpty;
    LOG(FATAL) << "Unreachable - RosAlloc::FreePages() : " << "pm_idx=" << pm_idx << ", pm_type="
               << static_cast<int>(pm_type) << ", ptr=" << std::hex
               << reinterpret_cast<intptr_t>(ptr);
    return 0;
  }
  // Update the page map and count the number of pages.
  size_t num_pages = 1;
  page_map_[pm_idx] = kPageMapEmpty;
  size_t idx = pm_idx + 1;
  size_t end = page_map_size_;
  while (idx < end && page_map_[idx] == pm_part_type) {
    page_map_[idx] = kPageMapEmpty;
    num_pages++;
    idx++;
  }
  const size_t byte_size = num_pages * kPageSize;
  if (already_zero) {
    if (kCheckZeroMemory) {
      const uword* word_ptr = reinterpret_cast<uword*>(ptr);
      for (size_t i = 0; i < byte_size / sizeof(uword); ++i) {
        CHECK_EQ(word_ptr[i], 0U) << "words don't match at index " << i;
      }
    }
  } else if (!DoesReleaseAllPages()) {
    memset(ptr, 0, byte_size);
  }

  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::FreePages() : 0x" << std::hex << reinterpret_cast<intptr_t>(ptr)
              << "-0x" << (reinterpret_cast<intptr_t>(ptr) + byte_size)
              << "(" << std::dec << (num_pages * kPageSize) << ")";
  }

  // Turn it into a free run.
  FreePageRun* fpr = reinterpret_cast<FreePageRun*>(ptr);
  if (kIsDebugBuild) {
    fpr->magic_num_ = kMagicNumFree;
  }
  fpr->SetByteSize(this, byte_size);
  DCHECK(IsAligned<kPageSize>(fpr->ByteSize(this)));

  DCHECK(free_page_runs_.find(fpr) == free_page_runs_.end());
  if (!free_page_runs_.empty()) {
    // Try to coalesce in the higher address direction.
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::FreePages() : trying to coalesce a free page run 0x"
                << std::hex << reinterpret_cast<uintptr_t>(fpr) << " [" << std::dec << pm_idx << "] -0x"
                << std::hex << reinterpret_cast<uintptr_t>(fpr->End(this)) << " [" << std::dec
                << (fpr->End(this) == End() ? page_map_size_ : ToPageMapIndex(fpr->End(this))) << "]";
    }
    auto higher_it = free_page_runs_.upper_bound(fpr);
    if (higher_it != free_page_runs_.end()) {
      for (auto it = higher_it; it != free_page_runs_.end(); ) {
        FreePageRun* h = *it;
        DCHECK_EQ(h->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::FreePages() : trying to coalesce with a higher free page run 0x"
                    << std::hex << reinterpret_cast<uintptr_t>(h) << " [" << std::dec << ToPageMapIndex(h) << "] -0x"
                    << std::hex << reinterpret_cast<uintptr_t>(h->End(this)) << " [" << std::dec
                    << (h->End(this) == End() ? page_map_size_ : ToPageMapIndex(h->End(this))) << "]";
        }
        if (fpr->End(this) == h->Begin()) {
          if (kTraceRosAlloc) {
            LOG(INFO) << "Success";
          }
          // Clear magic num since this is no longer the start of a free page run.
          if (kIsDebugBuild) {
            h->magic_num_ = 0;
          }
          free_page_runs_.erase(it++);
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::FreePages() : (coalesce) Erased run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(h)
                      << " from free_page_runs_";
          }
          fpr->SetByteSize(this, fpr->ByteSize(this) + h->ByteSize(this));
          DCHECK_EQ(fpr->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        } else {
          // Not adjacent. Stop.
          if (kTraceRosAlloc) {
            LOG(INFO) << "Fail";
          }
          break;
        }
      }
    }
    // Try to coalesce in the lower address direction.
    auto lower_it = free_page_runs_.upper_bound(fpr);
    if (lower_it != free_page_runs_.begin()) {
      --lower_it;
      for (auto it = lower_it; ; ) {
        // We want to try to coalesce with the first element but
        // there's no "<=" operator for the iterator.
        bool to_exit_loop = it == free_page_runs_.begin();

        FreePageRun* l = *it;
        DCHECK_EQ(l->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::FreePages() : trying to coalesce with a lower free page run 0x"
                    << std::hex << reinterpret_cast<uintptr_t>(l) << " [" << std::dec << ToPageMapIndex(l) << "] -0x"
                    << std::hex << reinterpret_cast<uintptr_t>(l->End(this)) << " [" << std::dec
                    << (l->End(this) == End() ? page_map_size_ : ToPageMapIndex(l->End(this))) << "]";
        }
        if (l->End(this) == fpr->Begin()) {
          if (kTraceRosAlloc) {
            LOG(INFO) << "Success";
          }
          free_page_runs_.erase(it--);
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::FreePages() : (coalesce) Erased run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(l)
                      << " from free_page_runs_";
          }
          l->SetByteSize(this, l->ByteSize(this) + fpr->ByteSize(this));
          DCHECK_EQ(l->ByteSize(this) % kPageSize, static_cast<size_t>(0));
          // Clear magic num since this is no longer the start of a free page run.
          if (kIsDebugBuild) {
            fpr->magic_num_ = 0;
          }
          fpr = l;
        } else {
          // Not adjacent. Stop.
          if (kTraceRosAlloc) {
            LOG(INFO) << "Fail";
          }
          break;
        }
        if (to_exit_loop) {
          break;
        }
      }
    }
  }

  // Insert it.
  DCHECK_EQ(fpr->ByteSize(this) % kPageSize, static_cast<size_t>(0));
  DCHECK(free_page_runs_.find(fpr) == free_page_runs_.end());
  DCHECK(fpr->IsFree());
  fpr->ReleasePages(this);
  DCHECK(fpr->IsFree());
  free_page_runs_.insert(fpr);
  DCHECK(free_page_runs_.find(fpr) != free_page_runs_.end());
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::FreePages() : Inserted run 0x" << std::hex << reinterpret_cast<intptr_t>(fpr)
              << " into free_page_runs_";
  }
  return byte_size;
}

void* RosAlloc::AllocLargeObject(Thread* self, size_t size, size_t* bytes_allocated) {
  DCHECK_GT(size, kLargeSizeThreshold);
  size_t num_pages = RoundUp(size, kPageSize) / kPageSize;
  void* r;
  {
    MutexLock mu(self, lock_);
    r = AllocPages(self, num_pages, kPageMapLargeObject);
  }
  if (UNLIKELY(r == nullptr)) {
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::AllocLargeObject() : NULL";
    }
    return nullptr;
  }
  const size_t total_bytes = num_pages * kPageSize;
  *bytes_allocated = total_bytes;
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::AllocLargeObject() : 0x" << std::hex << reinterpret_cast<intptr_t>(r)
              << "-0x" << (reinterpret_cast<intptr_t>(r) + num_pages * kPageSize)
              << "(" << std::dec << (num_pages * kPageSize) << ")";
  }
  // Check if the returned memory is really all zero.
  if (kCheckZeroMemory) {
    CHECK_EQ(total_bytes % sizeof(uword), 0U);
    const uword* words = reinterpret_cast<uword*>(r);
    for (size_t i = 0; i < total_bytes / sizeof(uword); ++i) {
      CHECK_EQ(words[i], 0U);
    }
  }
  return r;
}

size_t RosAlloc::FreeInternal(Thread* self, void* ptr) {
  DCHECK_LE(base_, ptr);
  DCHECK_LT(ptr, base_ + footprint_);
  size_t pm_idx = RoundDownToPageMapIndex(ptr);
  Run* run = nullptr;
  {
    MutexLock mu(self, lock_);
    DCHECK_LT(pm_idx, page_map_size_);
    byte page_map_entry = page_map_[pm_idx];
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::FreeInternal() : " << std::hex << ptr << ", pm_idx=" << std::dec << pm_idx
                << ", page_map_entry=" << static_cast<int>(page_map_entry);
    }
    switch (page_map_[pm_idx]) {
      case kPageMapEmpty:
        LOG(FATAL) << "Unreachable - page map type: " << page_map_[pm_idx];
        return 0;
      case kPageMapLargeObject:
        return FreePages(self, ptr, false);
      case kPageMapLargeObjectPart:
        LOG(FATAL) << "Unreachable - page map type: " << page_map_[pm_idx];
        return 0;
      case kPageMapRun:
      case kPageMapRunPart: {
        size_t pi = pm_idx;
        DCHECK(page_map_[pi] == kPageMapRun || page_map_[pi] == kPageMapRunPart);
        // Find the beginning of the run.
        while (page_map_[pi] != kPageMapRun) {
          pi--;
          DCHECK_LT(pi, capacity_ / kPageSize);
        }
        DCHECK_EQ(page_map_[pi], kPageMapRun);
        run = reinterpret_cast<Run*>(base_ + pi * kPageSize);
        DCHECK_EQ(run->magic_num_, kMagicNum);
        break;
      }
      default:
        LOG(FATAL) << "Unreachable - page map type: " << page_map_[pm_idx];
        return 0;
    }
  }
  DCHECK(run != nullptr);
  return FreeFromRun(self, ptr, run);
}

size_t RosAlloc::Free(Thread* self, void* ptr) {
  ReaderMutexLock rmu(self, bulk_free_lock_);
  return FreeInternal(self, ptr);
}

RosAlloc::Run* RosAlloc::AllocRun(Thread* self, size_t idx) {
  RosAlloc::Run* new_run = nullptr;
  {
    MutexLock mu(self, lock_);
    new_run = reinterpret_cast<Run*>(AllocPages(self, numOfPages[idx], kPageMapRun));
  }
  if (LIKELY(new_run != nullptr)) {
    if (kIsDebugBuild) {
      new_run->magic_num_ = kMagicNum;
    }
    new_run->size_bracket_idx_ = idx;
    new_run->SetAllocBitMapBitsForInvalidSlots();
    DCHECK(!new_run->IsThreadLocal());
    DCHECK_EQ(new_run->first_search_vec_idx_, 0U);
    DCHECK(!new_run->to_be_bulk_freed_);
    if (kUsePrefetchDuringAllocRun && idx < kNumThreadLocalSizeBrackets) {
      // Take ownership of the cache lines if we are likely to be thread local run.
      if (kPrefetchNewRunDataByZeroing) {
        // Zeroing the data is sometimes faster than prefetching but it increases memory usage
        // since we end up dirtying zero pages which may have been madvised.
        new_run->ZeroData();
      } else {
        const size_t num_of_slots = numOfSlots[idx];
        const size_t bracket_size = bracketSizes[idx];
        const size_t num_of_bytes = num_of_slots * bracket_size;
        byte* begin = reinterpret_cast<byte*>(new_run) + headerSizes[idx];
        for (size_t i = 0; i < num_of_bytes; i += kPrefetchStride) {
          __builtin_prefetch(begin + i);
        }
      }
    }
  }
  return new_run;
}

RosAlloc::Run* RosAlloc::RefillRun(Thread* self, size_t idx) {
  // Get the lowest address non-full run from the binary tree.
  std::set<Run*>* const bt = &non_full_runs_[idx];
  if (!bt->empty()) {
    // If there's one, use it as the current run.
    auto it = bt->begin();
    Run* non_full_run = *it;
    DCHECK(non_full_run != nullptr);
    DCHECK(!non_full_run->IsThreadLocal());
    bt->erase(it);
    return non_full_run;
  }
  // If there's none, allocate a new run and use it as the current run.
  return AllocRun(self, idx);
}

inline void* RosAlloc::AllocFromCurrentRunUnlocked(Thread* self, size_t idx) {
  Run* current_run = current_runs_[idx];
  DCHECK(current_run != nullptr);
  void* slot_addr = current_run->AllocSlot();
  if (UNLIKELY(slot_addr == nullptr)) {
    // The current run got full. Try to refill it.
    DCHECK(current_run->IsFull());
    if (kIsDebugBuild && current_run != dedicated_full_run_) {
      full_runs_[idx].insert(current_run);
      if (kTraceRosAlloc) {
        LOG(INFO) << __FUNCTION__ << " : Inserted run 0x" << std::hex << reinterpret_cast<intptr_t>(current_run)
                  << " into full_runs_[" << std::dec << idx << "]";
      }
      DCHECK(non_full_runs_[idx].find(current_run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(current_run) != full_runs_[idx].end());
    }
    current_run = RefillRun(self, idx);
    if (UNLIKELY(current_run == nullptr)) {
      // Failed to allocate a new run, make sure that it is the dedicated full run.
      current_runs_[idx] = dedicated_full_run_;
      return nullptr;
    }
    DCHECK(current_run != nullptr);
    DCHECK(non_full_runs_[idx].find(current_run) == non_full_runs_[idx].end());
    DCHECK(full_runs_[idx].find(current_run) == full_runs_[idx].end());
    current_run->SetIsThreadLocal(false);
    current_runs_[idx] = current_run;
    DCHECK(!current_run->IsFull());
    slot_addr = current_run->AllocSlot();
    // Must succeed now with a new run.
    DCHECK(slot_addr != nullptr);
  }
  return slot_addr;
}

void* RosAlloc::AllocFromRunThreadUnsafe(Thread* self, size_t size, size_t* bytes_allocated) {
  DCHECK_LE(size, kLargeSizeThreshold);
  size_t bracket_size;
  size_t idx = SizeToIndexAndBracketSize(size, &bracket_size);
  DCHECK_EQ(idx, SizeToIndex(size));
  DCHECK_EQ(bracket_size, IndexToBracketSize(idx));
  DCHECK_EQ(bracket_size, bracketSizes[idx]);
  DCHECK_LE(size, bracket_size);
  DCHECK(size > 512 || bracket_size - size < 16);
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  void* slot_addr = AllocFromCurrentRunUnlocked(self, idx);
  if (LIKELY(slot_addr != nullptr)) {
    DCHECK(bytes_allocated != nullptr);
    *bytes_allocated = bracket_size;
    // Caller verifies that it is all 0.
  }
  return slot_addr;
}

void* RosAlloc::AllocFromRun(Thread* self, size_t size, size_t* bytes_allocated) {
  DCHECK_LE(size, kLargeSizeThreshold);
  size_t bracket_size;
  size_t idx = SizeToIndexAndBracketSize(size, &bracket_size);
  DCHECK_EQ(idx, SizeToIndex(size));
  DCHECK_EQ(bracket_size, IndexToBracketSize(idx));
  DCHECK_EQ(bracket_size, bracketSizes[idx]);
  DCHECK_LE(size, bracket_size);
  DCHECK(size > 512 || bracket_size - size < 16);

  void* slot_addr;

  if (LIKELY(idx < kNumThreadLocalSizeBrackets)) {
    // Use a thread-local run.
    Run* thread_local_run = reinterpret_cast<Run*>(self->GetRosAllocRun(idx));
    // Allow invalid since this will always fail the allocation.
    if (kIsDebugBuild) {
      // Need the lock to prevent race conditions.
      MutexLock mu(self, *size_bracket_locks_[idx]);
      CHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
      CHECK(full_runs_[idx].find(thread_local_run) == full_runs_[idx].end());
    }
    DCHECK(thread_local_run != nullptr);
    DCHECK(thread_local_run->IsThreadLocal() || thread_local_run == dedicated_full_run_);
    slot_addr = thread_local_run->AllocSlot();
    // The allocation must fail if the run is invalid.
    DCHECK(thread_local_run != dedicated_full_run_ || slot_addr == nullptr)
        << "allocated from an invalid run";
    if (UNLIKELY(slot_addr == nullptr)) {
      // The run got full. Try to free slots.
      DCHECK(thread_local_run->IsFull());
      MutexLock mu(self, *size_bracket_locks_[idx]);
      bool is_all_free_after_merge;
      // This is safe to do for the dedicated_full_run_ since the bitmaps are empty.
      if (thread_local_run->MergeThreadLocalFreeBitMapToAllocBitMap(&is_all_free_after_merge)) {
        DCHECK_NE(thread_local_run, dedicated_full_run_);
        // Some slot got freed. Keep it.
        DCHECK(!thread_local_run->IsFull());
        DCHECK_EQ(is_all_free_after_merge, thread_local_run->IsAllFree());
        if (is_all_free_after_merge) {
          // Check that the bitmap idx is back at 0 if it's all free.
          DCHECK_EQ(thread_local_run->first_search_vec_idx_, 0U);
        }
      } else {
        // No slots got freed. Try to refill the thread-local run.
        DCHECK(thread_local_run->IsFull());
        if (thread_local_run != dedicated_full_run_) {
          thread_local_run->SetIsThreadLocal(false);
          if (kIsDebugBuild) {
            full_runs_[idx].insert(thread_local_run);
            if (kTraceRosAlloc) {
              LOG(INFO) << "RosAlloc::AllocFromRun() : Inserted run 0x" << std::hex
                        << reinterpret_cast<intptr_t>(thread_local_run)
                        << " into full_runs_[" << std::dec << idx << "]";
            }
          }
          DCHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
          DCHECK(full_runs_[idx].find(thread_local_run) != full_runs_[idx].end());
        }

        thread_local_run = RefillRun(self, idx);
        if (UNLIKELY(thread_local_run == nullptr)) {
          self->SetRosAllocRun(idx, dedicated_full_run_);
          return nullptr;
        }
        DCHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
        DCHECK(full_runs_[idx].find(thread_local_run) == full_runs_[idx].end());
        thread_local_run->SetIsThreadLocal(true);
        self->SetRosAllocRun(idx, thread_local_run);
        DCHECK(!thread_local_run->IsFull());
      }

      DCHECK(thread_local_run != nullptr);
      DCHECK(!thread_local_run->IsFull());
      DCHECK(thread_local_run->IsThreadLocal());
      slot_addr = thread_local_run->AllocSlot();
      // Must succeed now with a new run.
      DCHECK(slot_addr != nullptr);
    }
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::AllocFromRun() thread-local : 0x" << std::hex << reinterpret_cast<intptr_t>(slot_addr)
                << "-0x" << (reinterpret_cast<intptr_t>(slot_addr) + bracket_size)
                << "(" << std::dec << (bracket_size) << ")";
    }
  } else {
    // Use the (shared) current run.
    MutexLock mu(self, *size_bracket_locks_[idx]);
    slot_addr = AllocFromCurrentRunUnlocked(self, idx);
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::AllocFromRun() : 0x" << std::hex << reinterpret_cast<intptr_t>(slot_addr)
                << "-0x" << (reinterpret_cast<intptr_t>(slot_addr) + bracket_size)
                << "(" << std::dec << (bracket_size) << ")";
    }
  }
  DCHECK(bytes_allocated != nullptr);
  *bytes_allocated = bracket_size;
  // Caller verifies that it is all 0.
  return slot_addr;
}

size_t RosAlloc::FreeFromRun(Thread* self, void* ptr, Run* run) {
  DCHECK_EQ(run->magic_num_, kMagicNum);
  DCHECK_LT(run, ptr);
  DCHECK_LT(ptr, run->End());
  const size_t idx = run->size_bracket_idx_;
  const size_t bracket_size = bracketSizes[idx];
  bool run_was_full = false;
  MutexLock mu(self, *size_bracket_locks_[idx]);
  if (kIsDebugBuild) {
    run_was_full = run->IsFull();
  }
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::FreeFromRun() : 0x" << std::hex << reinterpret_cast<intptr_t>(ptr);
  }
  if (LIKELY(run->IsThreadLocal())) {
    // It's a thread-local run. Just mark the thread-local free bit map and return.
    DCHECK_LT(run->size_bracket_idx_, kNumThreadLocalSizeBrackets);
    DCHECK(non_full_runs_[idx].find(run) == non_full_runs_[idx].end());
    DCHECK(full_runs_[idx].find(run) == full_runs_[idx].end());
    run->MarkThreadLocalFreeBitMap(ptr);
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::FreeFromRun() : Freed a slot in a thread local run 0x" << std::hex
                << reinterpret_cast<intptr_t>(run);
    }
    // A thread local run will be kept as a thread local even if it's become all free.
    return bracket_size;
  }
  // Free the slot in the run.
  run->FreeSlot(ptr);
  std::set<Run*>* non_full_runs = &non_full_runs_[idx];
  if (run->IsAllFree()) {
    // It has just become completely free. Free the pages of this run.
    std::set<Run*>::iterator pos = non_full_runs->find(run);
    if (pos != non_full_runs->end()) {
      non_full_runs->erase(pos);
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::FreeFromRun() : Erased run 0x" << std::hex
                  << reinterpret_cast<intptr_t>(run) << " from non_full_runs_";
      }
    }
    if (run == current_runs_[idx]) {
      current_runs_[idx] = dedicated_full_run_;
    }
    DCHECK(non_full_runs_[idx].find(run) == non_full_runs_[idx].end());
    DCHECK(full_runs_[idx].find(run) == full_runs_[idx].end());
    run->ZeroHeader();
    {
      MutexLock mu(self, lock_);
      FreePages(self, run, true);
    }
  } else {
    // It is not completely free. If it wasn't the current run or
    // already in the non-full run set (i.e., it was full) insert it
    // into the non-full run set.
    if (run != current_runs_[idx]) {
      unordered_set<Run*, hash_run, eq_run>* full_runs =
          kIsDebugBuild ? &full_runs_[idx] : NULL;
      std::set<Run*>::iterator pos = non_full_runs->find(run);
      if (pos == non_full_runs->end()) {
        DCHECK(run_was_full);
        DCHECK(full_runs->find(run) != full_runs->end());
        if (kIsDebugBuild) {
          full_runs->erase(run);
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::FreeFromRun() : Erased run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(run) << " from full_runs_";
          }
        }
        non_full_runs->insert(run);
        DCHECK(!run->IsFull());
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::FreeFromRun() : Inserted run 0x" << std::hex
                    << reinterpret_cast<intptr_t>(run)
                    << " into non_full_runs_[" << std::dec << idx << "]";
        }
      }
    }
  }
  return bracket_size;
}

std::string RosAlloc::Run::BitMapToStr(uint32_t* bit_map_base, size_t num_vec) {
  std::string bit_map_str;
  for (size_t v = 0; v < num_vec; v++) {
    uint32_t vec = bit_map_base[v];
    if (v != num_vec - 1) {
      bit_map_str.append(StringPrintf("%x-", vec));
    } else {
      bit_map_str.append(StringPrintf("%x", vec));
    }
  }
  return bit_map_str.c_str();
}

std::string RosAlloc::Run::Dump() {
  size_t idx = size_bracket_idx_;
  size_t num_slots = numOfSlots[idx];
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  std::ostringstream stream;
  stream << "RosAlloc Run = " << reinterpret_cast<void*>(this)
         << "{ magic_num=" << static_cast<int>(magic_num_)
         << " size_bracket_idx=" << idx
         << " is_thread_local=" << static_cast<int>(is_thread_local_)
         << " to_be_bulk_freed=" << static_cast<int>(to_be_bulk_freed_)
         << " first_search_vec_idx=" << first_search_vec_idx_
         << " alloc_bit_map=" << BitMapToStr(alloc_bit_map_, num_vec)
         << " bulk_free_bit_map=" << BitMapToStr(BulkFreeBitMap(), num_vec)
         << " thread_local_bit_map=" << BitMapToStr(ThreadLocalFreeBitMap(), num_vec)
         << " }" << std::endl;
  return stream.str();
}

inline void* RosAlloc::Run::AllocSlot() {
  const size_t idx = size_bracket_idx_;
  while (true) {
    if (kIsDebugBuild) {
      // Make sure that no slots leaked, the bitmap should be full for all previous vectors.
      for (size_t i = 0; i < first_search_vec_idx_; ++i) {
        CHECK_EQ(~alloc_bit_map_[i], 0U);
      }
    }
    uint32_t* const alloc_bitmap_ptr = &alloc_bit_map_[first_search_vec_idx_];
    uint32_t ffz1 = __builtin_ffs(~*alloc_bitmap_ptr);
    if (LIKELY(ffz1 != 0)) {
      const uint32_t ffz = ffz1 - 1;
      const uint32_t slot_idx = ffz + first_search_vec_idx_ * sizeof(*alloc_bitmap_ptr) * kBitsPerByte;
      const uint32_t mask = 1U << ffz;
      DCHECK_LT(slot_idx, numOfSlots[idx]) << "out of range";
      // Found an empty slot. Set the bit.
      DCHECK_EQ(*alloc_bitmap_ptr & mask, 0U);
      *alloc_bitmap_ptr |= mask;
      DCHECK_NE(*alloc_bitmap_ptr & mask, 0U);
      byte* slot_addr = reinterpret_cast<byte*>(this) + headerSizes[idx] + slot_idx * bracketSizes[idx];
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::Run::AllocSlot() : 0x" << std::hex << reinterpret_cast<intptr_t>(slot_addr)
                  << ", bracket_size=" << std::dec << bracketSizes[idx] << ", slot_idx=" << slot_idx;
      }
      return slot_addr;
    }
    const size_t num_words = RoundUp(numOfSlots[idx], 32) / 32;
    if (first_search_vec_idx_ + 1 >= num_words) {
      DCHECK(IsFull());
      // Already at the last word, return null.
      return nullptr;
    }
    // Increase the index to the next word and try again.
    ++first_search_vec_idx_;
  }
}

void RosAlloc::Run::FreeSlot(void* ptr) {
  DCHECK(!IsThreadLocal());
  const byte idx = size_bracket_idx_;
  const size_t bracket_size = bracketSizes[idx];
  const size_t offset_from_slot_base = reinterpret_cast<byte*>(ptr)
      - (reinterpret_cast<byte*>(this) + headerSizes[idx]);
  DCHECK_EQ(offset_from_slot_base % bracket_size, static_cast<size_t>(0));
  size_t slot_idx = offset_from_slot_base / bracket_size;
  DCHECK_LT(slot_idx, numOfSlots[idx]);
  size_t vec_idx = slot_idx / 32;
  if (kIsDebugBuild) {
    size_t num_vec = RoundUp(numOfSlots[idx], 32) / 32;
    DCHECK_LT(vec_idx, num_vec);
  }
  size_t vec_off = slot_idx % 32;
  uint32_t* vec = &alloc_bit_map_[vec_idx];
  first_search_vec_idx_ = std::min(first_search_vec_idx_, static_cast<uint32_t>(vec_idx));
  const uint32_t mask = 1U << vec_off;
  DCHECK_NE(*vec & mask, 0U);
  *vec &= ~mask;
  DCHECK_EQ(*vec & mask, 0U);
  // Zero out the memory.
  // TODO: Investigate alternate memset since ptr is guaranteed to be aligned to 16.
  memset(ptr, 0, bracket_size);
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::Run::FreeSlot() : 0x" << std::hex << reinterpret_cast<intptr_t>(ptr)
              << ", bracket_size=" << std::dec << bracketSizes[idx] << ", slot_idx=" << slot_idx;
  }
}

inline bool RosAlloc::Run::MergeThreadLocalFreeBitMapToAllocBitMap(bool* is_all_free_after_out) {
  DCHECK(IsThreadLocal());
  // Free slots in the alloc bit map based on the thread local free bit map.
  const size_t idx = size_bracket_idx_;
  const size_t num_of_slots = numOfSlots[idx];
  const size_t num_vec = RoundUp(num_of_slots, 32) / 32;
  bool changed = false;
  uint32_t* vecp = &alloc_bit_map_[0];
  uint32_t* tl_free_vecp = &ThreadLocalFreeBitMap()[0];
  bool is_all_free_after = true;
  for (size_t v = 0; v < num_vec; v++, vecp++, tl_free_vecp++) {
    uint32_t tl_free_vec = *tl_free_vecp;
    uint32_t vec_before = *vecp;
    uint32_t vec_after;
    if (tl_free_vec != 0) {
      first_search_vec_idx_ = std::min(first_search_vec_idx_, static_cast<uint32_t>(v));
      vec_after = vec_before & ~tl_free_vec;
      *vecp = vec_after;
      changed = true;
      *tl_free_vecp = 0;  // clear the thread local free bit map.
    } else {
      vec_after = vec_before;
    }
    if (vec_after != 0) {
      if (v == num_vec - 1) {
        // Only not all free if a bit other than the mask bits are set.
        is_all_free_after =
            is_all_free_after && GetBitmapLastVectorMask(num_of_slots, num_vec) == vec_after;
      } else {
        is_all_free_after = false;
      }
    }
    DCHECK_EQ(*tl_free_vecp, static_cast<uint32_t>(0));
  }
  *is_all_free_after_out = is_all_free_after;
  // Return true if there was at least a bit set in the thread-local
  // free bit map and at least a bit in the alloc bit map changed.
  return changed;
}

inline void RosAlloc::Run::MergeBulkFreeBitMapIntoAllocBitMap() {
  DCHECK(!IsThreadLocal());
  // Free slots in the alloc bit map based on the bulk free bit map.
  const size_t num_vec = NumberOfBitmapVectors();
  uint32_t* vecp = &alloc_bit_map_[0];
  uint32_t* free_vecp = &BulkFreeBitMap()[0];
  for (size_t v = 0; v < num_vec; v++, vecp++, free_vecp++) {
    uint32_t free_vec = *free_vecp;
    if (free_vec != 0) {
      first_search_vec_idx_ = std::min(first_search_vec_idx_, static_cast<uint32_t>(v));
      *vecp &= ~free_vec;
      *free_vecp = 0;  // clear the bulk free bit map.
    }
    DCHECK_EQ(*free_vecp, static_cast<uint32_t>(0));
  }
}

inline void RosAlloc::Run::UnionBulkFreeBitMapToThreadLocalFreeBitMap() {
  DCHECK(IsThreadLocal());
  // Union the thread local bit map with the bulk free bit map.
  size_t num_vec = NumberOfBitmapVectors();
  uint32_t* to_vecp = &ThreadLocalFreeBitMap()[0];
  uint32_t* from_vecp = &BulkFreeBitMap()[0];
  for (size_t v = 0; v < num_vec; v++, to_vecp++, from_vecp++) {
    uint32_t from_vec = *from_vecp;
    if (from_vec != 0) {
      *to_vecp |= from_vec;
      *from_vecp = 0;  // clear the bulk free bit map.
    }
    DCHECK_EQ(*from_vecp, static_cast<uint32_t>(0));
  }
}

inline void RosAlloc::Run::MarkThreadLocalFreeBitMap(void* ptr) {
  DCHECK(IsThreadLocal());
  MarkFreeBitMapShared(ptr, ThreadLocalFreeBitMap(), "MarkThreadLocalFreeBitMap");
}

inline size_t RosAlloc::Run::MarkBulkFreeBitMap(void* ptr) {
  return MarkFreeBitMapShared(ptr, BulkFreeBitMap(), "MarkFreeBitMap");
}

inline size_t RosAlloc::Run::MarkFreeBitMapShared(void* ptr, uint32_t* free_bit_map_base,
                                                  const char* caller_name) {
  const byte idx = size_bracket_idx_;
  const size_t offset_from_slot_base = reinterpret_cast<byte*>(ptr)
      - (reinterpret_cast<byte*>(this) + headerSizes[idx]);
  const size_t bracket_size = bracketSizes[idx];
  memset(ptr, 0, bracket_size);
  DCHECK_EQ(offset_from_slot_base % bracket_size, static_cast<size_t>(0));
  size_t slot_idx = offset_from_slot_base / bracket_size;
  DCHECK_LT(slot_idx, numOfSlots[idx]);
  size_t vec_idx = slot_idx / 32;
  if (kIsDebugBuild) {
    size_t num_vec = NumberOfBitmapVectors();
    DCHECK_LT(vec_idx, num_vec);
  }
  size_t vec_off = slot_idx % 32;
  uint32_t* vec = &free_bit_map_base[vec_idx];
  const uint32_t mask = 1U << vec_off;
  DCHECK_EQ(*vec & mask, 0U);
  *vec |= mask;
  DCHECK_NE(*vec & mask, 0U);
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::Run::" << caller_name << "() : 0x" << std::hex
              << reinterpret_cast<intptr_t>(ptr)
              << ", bracket_size=" << std::dec << bracketSizes[idx] << ", slot_idx=" << slot_idx;
  }
  return bracket_size;
}

inline uint32_t RosAlloc::Run::GetBitmapLastVectorMask(size_t num_slots, size_t num_vec) {
  const size_t kBitsPerVec = 32;
  DCHECK_GE(num_slots * kBitsPerVec, num_vec);
  size_t remain = num_vec * kBitsPerVec - num_slots;
  DCHECK_NE(remain, kBitsPerVec);
  return ((1U << remain) - 1) << (kBitsPerVec - remain);
}

inline bool RosAlloc::Run::IsAllFree() {
  const byte idx = size_bracket_idx_;
  const size_t num_slots = numOfSlots[idx];
  const size_t num_vec = NumberOfBitmapVectors();
  DCHECK_NE(num_vec, 0U);
  // Check the last vector after the loop since it uses a special case for the masked bits.
  for (size_t v = 0; v < num_vec - 1; v++) {
    uint32_t vec = alloc_bit_map_[v];
    if (vec != 0) {
      return false;
    }
  }
  // Make sure the last word is equal to the mask, all other bits must be 0.
  return alloc_bit_map_[num_vec - 1] == GetBitmapLastVectorMask(num_slots, num_vec);
}

inline bool RosAlloc::Run::IsFull() {
  const size_t num_vec = NumberOfBitmapVectors();
  for (size_t v = 0; v < num_vec; ++v) {
    if (~alloc_bit_map_[v] != 0) {
      return false;
    }
  }
  return true;
}

inline bool RosAlloc::Run::IsBulkFreeBitmapClean() {
  const size_t num_vec = NumberOfBitmapVectors();
  for (size_t v = 0; v < num_vec; v++) {
    uint32_t vec = BulkFreeBitMap()[v];
    if (vec != 0) {
      return false;
    }
  }
  return true;
}

inline bool RosAlloc::Run::IsThreadLocalFreeBitmapClean() {
  const size_t num_vec = NumberOfBitmapVectors();
  for (size_t v = 0; v < num_vec; v++) {
    uint32_t vec = ThreadLocalFreeBitMap()[v];
    if (vec != 0) {
      return false;
    }
  }
  return true;
}

inline void RosAlloc::Run::SetAllocBitMapBitsForInvalidSlots() {
  const size_t idx = size_bracket_idx_;
  const size_t num_slots = numOfSlots[idx];
  const size_t num_vec = RoundUp(num_slots, 32) / 32;
  DCHECK_NE(num_vec, 0U);
  // Make sure to set the bits at the end of the bitmap so that we don't allocate there since they
  // don't represent valid slots.
  alloc_bit_map_[num_vec - 1] |= GetBitmapLastVectorMask(num_slots, num_vec);
}

inline void RosAlloc::Run::ZeroHeader() {
  const byte idx = size_bracket_idx_;
  memset(this, 0, headerSizes[idx]);
}

inline void RosAlloc::Run::ZeroData() {
  const byte idx = size_bracket_idx_;
  byte* slot_begin = reinterpret_cast<byte*>(this) + headerSizes[idx];
  memset(slot_begin, 0, numOfSlots[idx] * bracketSizes[idx]);
}

inline void RosAlloc::Run::FillAllocBitMap() {
  size_t num_vec = NumberOfBitmapVectors();
  memset(alloc_bit_map_, 0xFF, sizeof(uint32_t) * num_vec);
  first_search_vec_idx_ = num_vec - 1;  // No free bits in any of the bitmap words.
}

void RosAlloc::Run::InspectAllSlots(void (*handler)(void* start, void* end, size_t used_bytes, void* callback_arg),
                                    void* arg) {
  size_t idx = size_bracket_idx_;
  byte* slot_base = reinterpret_cast<byte*>(this) + headerSizes[idx];
  size_t num_slots = numOfSlots[idx];
  size_t bracket_size = IndexToBracketSize(idx);
  DCHECK_EQ(slot_base + num_slots * bracket_size, reinterpret_cast<byte*>(this) + numOfPages[idx] * kPageSize);
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  size_t slots = 0;
  for (size_t v = 0; v < num_vec; v++, slots += 32) {
    DCHECK_GE(num_slots, slots);
    uint32_t vec = alloc_bit_map_[v];
    size_t end = std::min(num_slots - slots, static_cast<size_t>(32));
    for (size_t i = 0; i < end; ++i) {
      bool is_allocated = ((vec >> i) & 0x1) != 0;
      byte* slot_addr = slot_base + (slots + i) * bracket_size;
      if (is_allocated) {
        handler(slot_addr, slot_addr + bracket_size, bracket_size, arg);
      } else {
        handler(slot_addr, slot_addr + bracket_size, 0, arg);
      }
    }
  }
}

// If true, read the page map entries in BulkFree() without using the
// lock for better performance, assuming that the existence of an
// allocated chunk/pointer being freed in BulkFree() guarantees that
// the page map entry won't change. Disabled for now.
static constexpr bool kReadPageMapEntryWithoutLockInBulkFree = true;

size_t RosAlloc::BulkFree(Thread* self, void** ptrs, size_t num_ptrs) {
  size_t freed_bytes = 0;
  if (false) {
    // Used only to test Free() as GC uses only BulkFree().
    for (size_t i = 0; i < num_ptrs; ++i) {
      freed_bytes += FreeInternal(self, ptrs[i]);
    }
    return freed_bytes;
  }

  WriterMutexLock wmu(self, bulk_free_lock_);

  // First mark slots to free in the bulk free bit map without locking the
  // size bracket locks. On host, unordered_set is faster than vector + flag.
#ifdef HAVE_ANDROID_OS
  std::vector<Run*> runs;
#else
  unordered_set<Run*, hash_run, eq_run> runs;
#endif
  for (size_t i = 0; i < num_ptrs; i++) {
    void* ptr = ptrs[i];
    DCHECK_LE(base_, ptr);
    DCHECK_LT(ptr, base_ + footprint_);
    size_t pm_idx = RoundDownToPageMapIndex(ptr);
    Run* run = nullptr;
    if (kReadPageMapEntryWithoutLockInBulkFree) {
      // Read the page map entries without locking the lock.
      byte page_map_entry = page_map_[pm_idx];
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::BulkFree() : " << std::hex << ptr << ", pm_idx="
                  << std::dec << pm_idx
                  << ", page_map_entry=" << static_cast<int>(page_map_entry);
      }
      if (LIKELY(page_map_entry == kPageMapRun)) {
        run = reinterpret_cast<Run*>(base_ + pm_idx * kPageSize);
      } else if (LIKELY(page_map_entry == kPageMapRunPart)) {
        size_t pi = pm_idx;
        // Find the beginning of the run.
        do {
          --pi;
          DCHECK_LT(pi, capacity_ / kPageSize);
        } while (page_map_[pi] != kPageMapRun);
        run = reinterpret_cast<Run*>(base_ + pi * kPageSize);
      } else if (page_map_entry == kPageMapLargeObject) {
        MutexLock mu(self, lock_);
        freed_bytes += FreePages(self, ptr, false);
        continue;
      } else {
        LOG(FATAL) << "Unreachable - page map type: " << page_map_entry;
      }
    } else {
      // Read the page map entries with a lock.
      MutexLock mu(self, lock_);
      DCHECK_LT(pm_idx, page_map_size_);
      byte page_map_entry = page_map_[pm_idx];
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::BulkFree() : " << std::hex << ptr << ", pm_idx="
                  << std::dec << pm_idx
                  << ", page_map_entry=" << static_cast<int>(page_map_entry);
      }
      if (LIKELY(page_map_entry == kPageMapRun)) {
        run = reinterpret_cast<Run*>(base_ + pm_idx * kPageSize);
      } else if (LIKELY(page_map_entry == kPageMapRunPart)) {
        size_t pi = pm_idx;
        // Find the beginning of the run.
        do {
          --pi;
          DCHECK_LT(pi, capacity_ / kPageSize);
        } while (page_map_[pi] != kPageMapRun);
        run = reinterpret_cast<Run*>(base_ + pi * kPageSize);
      } else if (page_map_entry == kPageMapLargeObject) {
        freed_bytes += FreePages(self, ptr, false);
        continue;
      } else {
        LOG(FATAL) << "Unreachable - page map type: " << page_map_entry;
      }
    }
    DCHECK(run != nullptr);
    DCHECK_EQ(run->magic_num_, kMagicNum);
    // Set the bit in the bulk free bit map.
    freed_bytes += run->MarkBulkFreeBitMap(ptr);
#ifdef HAVE_ANDROID_OS
    if (!run->to_be_bulk_freed_) {
      run->to_be_bulk_freed_ = true;
      runs.push_back(run);
    }
#else
    runs.insert(run);
#endif
  }

  // Now, iterate over the affected runs and update the alloc bit map
  // based on the bulk free bit map (for non-thread-local runs) and
  // union the bulk free bit map into the thread-local free bit map
  // (for thread-local runs.)
  for (Run* run : runs) {
#ifdef HAVE_ANDROID_OS
    DCHECK(run->to_be_bulk_freed_);
    run->to_be_bulk_freed_ = false;
#endif
    size_t idx = run->size_bracket_idx_;
    MutexLock mu(self, *size_bracket_locks_[idx]);
    if (run->IsThreadLocal()) {
      DCHECK_LT(run->size_bracket_idx_, kNumThreadLocalSizeBrackets);
      DCHECK(non_full_runs_[idx].find(run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(run) == full_runs_[idx].end());
      run->UnionBulkFreeBitMapToThreadLocalFreeBitMap();
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::BulkFree() : Freed slot(s) in a thread local run 0x"
                  << std::hex << reinterpret_cast<intptr_t>(run);
      }
      DCHECK(run->IsThreadLocal());
      // A thread local run will be kept as a thread local even if
      // it's become all free.
    } else {
      bool run_was_full = run->IsFull();
      run->MergeBulkFreeBitMapIntoAllocBitMap();
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::BulkFree() : Freed slot(s) in a run 0x" << std::hex
                  << reinterpret_cast<intptr_t>(run);
      }
      // Check if the run should be moved to non_full_runs_ or
      // free_page_runs_.
      std::set<Run*>* non_full_runs = &non_full_runs_[idx];
      unordered_set<Run*, hash_run, eq_run>* full_runs =
          kIsDebugBuild ? &full_runs_[idx] : NULL;
      if (run->IsAllFree()) {
        // It has just become completely free. Free the pages of the
        // run.
        bool run_was_current = run == current_runs_[idx];
        if (run_was_current) {
          DCHECK(full_runs->find(run) == full_runs->end());
          DCHECK(non_full_runs->find(run) == non_full_runs->end());
          // If it was a current run, reuse it.
        } else if (run_was_full) {
          // If it was full, remove it from the full run set (debug
          // only.)
          if (kIsDebugBuild) {
            unordered_set<Run*, hash_run, eq_run>::iterator pos = full_runs->find(run);
            DCHECK(pos != full_runs->end());
            full_runs->erase(pos);
            if (kTraceRosAlloc) {
              LOG(INFO) << "RosAlloc::BulkFree() : Erased run 0x" << std::hex
                        << reinterpret_cast<intptr_t>(run)
                        << " from full_runs_";
            }
            DCHECK(full_runs->find(run) == full_runs->end());
          }
        } else {
          // If it was in a non full run set, remove it from the set.
          DCHECK(full_runs->find(run) == full_runs->end());
          DCHECK(non_full_runs->find(run) != non_full_runs->end());
          non_full_runs->erase(run);
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::BulkFree() : Erased run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(run)
                      << " from non_full_runs_";
          }
          DCHECK(non_full_runs->find(run) == non_full_runs->end());
        }
        if (!run_was_current) {
          run->ZeroHeader();
          MutexLock mu(self, lock_);
          FreePages(self, run, true);
        }
      } else {
        // It is not completely free. If it wasn't the current run or
        // already in the non-full run set (i.e., it was full) insert
        // it into the non-full run set.
        if (run == current_runs_[idx]) {
          DCHECK(non_full_runs->find(run) == non_full_runs->end());
          DCHECK(full_runs->find(run) == full_runs->end());
          // If it was a current run, keep it.
        } else if (run_was_full) {
          // If it was full, remove it from the full run set (debug
          // only) and insert into the non-full run set.
          DCHECK(full_runs->find(run) != full_runs->end());
          DCHECK(non_full_runs->find(run) == non_full_runs->end());
          if (kIsDebugBuild) {
            full_runs->erase(run);
            if (kTraceRosAlloc) {
              LOG(INFO) << "RosAlloc::BulkFree() : Erased run 0x" << std::hex
                        << reinterpret_cast<intptr_t>(run)
                        << " from full_runs_";
            }
          }
          non_full_runs->insert(run);
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::BulkFree() : Inserted run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(run)
                      << " into non_full_runs_[" << std::dec << idx;
          }
        } else {
          // If it was not full, so leave it in the non full run set.
          DCHECK(full_runs->find(run) == full_runs->end());
          DCHECK(non_full_runs->find(run) != non_full_runs->end());
        }
      }
    }
  }
  return freed_bytes;
}

std::string RosAlloc::DumpPageMap() {
  std::ostringstream stream;
  stream << "RosAlloc PageMap: " << std::endl;
  lock_.AssertHeld(Thread::Current());
  size_t end = page_map_size_;
  FreePageRun* curr_fpr = NULL;
  size_t curr_fpr_size = 0;
  size_t remaining_curr_fpr_size = 0;
  size_t num_running_empty_pages = 0;
  for (size_t i = 0; i < end; ++i) {
    byte pm = page_map_[i];
    switch (pm) {
      case kPageMapEmpty: {
        FreePageRun* fpr = reinterpret_cast<FreePageRun*>(base_ + i * kPageSize);
        if (free_page_runs_.find(fpr) != free_page_runs_.end()) {
          // Encountered a fresh free page run.
          DCHECK_EQ(remaining_curr_fpr_size, static_cast<size_t>(0));
          DCHECK(fpr->IsFree());
          DCHECK(curr_fpr == NULL);
          DCHECK_EQ(curr_fpr_size, static_cast<size_t>(0));
          curr_fpr = fpr;
          curr_fpr_size = fpr->ByteSize(this);
          DCHECK_EQ(curr_fpr_size % kPageSize, static_cast<size_t>(0));
          remaining_curr_fpr_size = curr_fpr_size - kPageSize;
          stream << "[" << i << "]=Empty (FPR start)"
                 << " fpr_size=" << curr_fpr_size
                 << " remaining_fpr_size=" << remaining_curr_fpr_size << std::endl;
          if (remaining_curr_fpr_size == 0) {
            // Reset at the end of the current free page run.
            curr_fpr = NULL;
            curr_fpr_size = 0;
          }
          stream << "curr_fpr=0x" << std::hex << reinterpret_cast<intptr_t>(curr_fpr) << std::endl;
          DCHECK_EQ(num_running_empty_pages, static_cast<size_t>(0));
        } else {
          // Still part of the current free page run.
          DCHECK_NE(num_running_empty_pages, static_cast<size_t>(0));
          DCHECK(curr_fpr != NULL && curr_fpr_size > 0 && remaining_curr_fpr_size > 0);
          DCHECK_EQ(remaining_curr_fpr_size % kPageSize, static_cast<size_t>(0));
          DCHECK_GE(remaining_curr_fpr_size, static_cast<size_t>(kPageSize));
          remaining_curr_fpr_size -= kPageSize;
          stream << "[" << i << "]=Empty (FPR part)"
                 << " remaining_fpr_size=" << remaining_curr_fpr_size << std::endl;
          if (remaining_curr_fpr_size == 0) {
            // Reset at the end of the current free page run.
            curr_fpr = NULL;
            curr_fpr_size = 0;
          }
        }
        num_running_empty_pages++;
        break;
      }
      case kPageMapLargeObject: {
        DCHECK_EQ(remaining_curr_fpr_size, static_cast<size_t>(0));
        num_running_empty_pages = 0;
        stream << "[" << i << "]=Large (start)" << std::endl;
        break;
      }
      case kPageMapLargeObjectPart:
        DCHECK_EQ(remaining_curr_fpr_size, static_cast<size_t>(0));
        num_running_empty_pages = 0;
        stream << "[" << i << "]=Large (part)" << std::endl;
        break;
      case kPageMapRun: {
        DCHECK_EQ(remaining_curr_fpr_size, static_cast<size_t>(0));
        num_running_empty_pages = 0;
        Run* run = reinterpret_cast<Run*>(base_ + i * kPageSize);
        size_t idx = run->size_bracket_idx_;
        stream << "[" << i << "]=Run (start)"
               << " idx=" << idx
               << " numOfPages=" << numOfPages[idx]
               << " is_thread_local=" << run->is_thread_local_
               << " is_all_free=" << (run->IsAllFree() ? 1 : 0)
               << std::endl;
        break;
      }
      case kPageMapRunPart:
        DCHECK_EQ(remaining_curr_fpr_size, static_cast<size_t>(0));
        num_running_empty_pages = 0;
        stream << "[" << i << "]=Run (part)" << std::endl;
        break;
      default:
        stream << "[" << i << "]=Unrecognizable page map type: " << pm;
        break;
    }
  }
  return stream.str();
}

size_t RosAlloc::UsableSize(void* ptr) {
  DCHECK_LE(base_, ptr);
  DCHECK_LT(ptr, base_ + footprint_);
  size_t pm_idx = RoundDownToPageMapIndex(ptr);
  MutexLock mu(Thread::Current(), lock_);
  switch (page_map_[pm_idx]) {
  case kPageMapEmpty:
    LOG(FATAL) << "Unreachable - RosAlloc::UsableSize(): pm_idx=" << pm_idx << ", ptr=" << std::hex
               << reinterpret_cast<intptr_t>(ptr);
    break;
  case kPageMapLargeObject: {
    size_t num_pages = 1;
    size_t idx = pm_idx + 1;
    size_t end = page_map_size_;
    while (idx < end && page_map_[idx] == kPageMapLargeObjectPart) {
      num_pages++;
      idx++;
    }
    return num_pages * kPageSize;
  }
  case kPageMapLargeObjectPart:
    LOG(FATAL) << "Unreachable - RosAlloc::UsableSize(): pm_idx=" << pm_idx << ", ptr=" << std::hex
               << reinterpret_cast<intptr_t>(ptr);
    break;
  case kPageMapRun:
  case kPageMapRunPart: {
    // Find the beginning of the run.
    while (page_map_[pm_idx] != kPageMapRun) {
      pm_idx--;
      DCHECK_LT(pm_idx, capacity_ / kPageSize);
    }
    DCHECK_EQ(page_map_[pm_idx], kPageMapRun);
    Run* run = reinterpret_cast<Run*>(base_ + pm_idx * kPageSize);
    DCHECK_EQ(run->magic_num_, kMagicNum);
    size_t idx = run->size_bracket_idx_;
    size_t offset_from_slot_base = reinterpret_cast<byte*>(ptr)
        - (reinterpret_cast<byte*>(run) + headerSizes[idx]);
    DCHECK_EQ(offset_from_slot_base % bracketSizes[idx], static_cast<size_t>(0));
    return IndexToBracketSize(idx);
  }
  default:
    LOG(FATAL) << "Unreachable - page map type: " << page_map_[pm_idx];
    break;
  }
  return 0;
}

bool RosAlloc::Trim() {
  MutexLock mu(Thread::Current(), lock_);
  FreePageRun* last_free_page_run;
  DCHECK_EQ(footprint_ % kPageSize, static_cast<size_t>(0));
  auto it = free_page_runs_.rbegin();
  if (it != free_page_runs_.rend() && (last_free_page_run = *it)->End(this) == base_ + footprint_) {
    // Remove the last free page run, if any.
    DCHECK(last_free_page_run->IsFree());
    DCHECK_EQ(page_map_[ToPageMapIndex(last_free_page_run)], kPageMapEmpty);
    DCHECK_EQ(last_free_page_run->ByteSize(this) % kPageSize, static_cast<size_t>(0));
    DCHECK_EQ(last_free_page_run->End(this), base_ + footprint_);
    free_page_runs_.erase(last_free_page_run);
    size_t decrement = last_free_page_run->ByteSize(this);
    size_t new_footprint = footprint_ - decrement;
    DCHECK_EQ(new_footprint % kPageSize, static_cast<size_t>(0));
    size_t new_num_of_pages = new_footprint / kPageSize;
    DCHECK_GE(page_map_size_, new_num_of_pages);
    // Zero out the tail of the page map.
    byte* zero_begin = page_map_ + new_num_of_pages;
    byte* madvise_begin = AlignUp(zero_begin, kPageSize);
    DCHECK_LE(madvise_begin, page_map_mem_map_->End());
    size_t madvise_size = page_map_mem_map_->End() - madvise_begin;
    if (madvise_size > 0) {
      DCHECK_ALIGNED(madvise_begin, kPageSize);
      DCHECK_EQ(RoundUp(madvise_size, kPageSize), madvise_size);
      CHECK_EQ(madvise(madvise_begin, madvise_size, MADV_DONTNEED), 0);
    }
    if (madvise_begin - zero_begin) {
      memset(zero_begin, 0, madvise_begin - zero_begin);
    }
    page_map_size_ = new_num_of_pages;
    free_page_run_size_map_.resize(new_num_of_pages);
    DCHECK_EQ(free_page_run_size_map_.size(), new_num_of_pages);
    art_heap_rosalloc_morecore(this, -(static_cast<intptr_t>(decrement)));
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::Trim() : decreased the footprint from "
                << footprint_ << " to " << new_footprint;
    }
    DCHECK_LT(new_footprint, footprint_);
    DCHECK_LT(new_footprint, capacity_);
    footprint_ = new_footprint;
    return true;
  }
  return false;
}

void RosAlloc::InspectAll(void (*handler)(void* start, void* end, size_t used_bytes, void* callback_arg),
                          void* arg) {
  // Note: no need to use this to release pages as we already do so in FreePages().
  if (handler == NULL) {
    return;
  }
  MutexLock mu(Thread::Current(), lock_);
  size_t pm_end = page_map_size_;
  size_t i = 0;
  while (i < pm_end) {
    byte pm = page_map_[i];
    switch (pm) {
      case kPageMapEmpty: {
        // The start of a free page run.
        FreePageRun* fpr = reinterpret_cast<FreePageRun*>(base_ + i * kPageSize);
        DCHECK(free_page_runs_.find(fpr) != free_page_runs_.end());
        size_t fpr_size = fpr->ByteSize(this);
        DCHECK(IsAligned<kPageSize>(fpr_size));
        void* start = fpr;
        if (kIsDebugBuild) {
          // In the debug build, the first page of a free page run
          // contains a magic number for debugging. Exclude it.
          start = reinterpret_cast<byte*>(fpr) + kPageSize;
        }
        void* end = reinterpret_cast<byte*>(fpr) + fpr_size;
        handler(start, end, 0, arg);
        size_t num_pages = fpr_size / kPageSize;
        if (kIsDebugBuild) {
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            DCHECK_EQ(page_map_[j], kPageMapEmpty);
          }
        }
        i += fpr_size / kPageSize;
        DCHECK_LE(i, pm_end);
        break;
      }
      case kPageMapLargeObject: {
        // The start of a large object.
        size_t num_pages = 1;
        size_t idx = i + 1;
        while (idx < pm_end && page_map_[idx] == kPageMapLargeObjectPart) {
          num_pages++;
          idx++;
        }
        void* start = base_ + i * kPageSize;
        void* end = base_ + (i + num_pages) * kPageSize;
        size_t used_bytes = num_pages * kPageSize;
        handler(start, end, used_bytes, arg);
        if (kIsDebugBuild) {
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            DCHECK_EQ(page_map_[j], kPageMapLargeObjectPart);
          }
        }
        i += num_pages;
        DCHECK_LE(i, pm_end);
        break;
      }
      case kPageMapLargeObjectPart:
        LOG(FATAL) << "Unreachable - page map type: " << pm;
        break;
      case kPageMapRun: {
        // The start of a run.
        Run* run = reinterpret_cast<Run*>(base_ + i * kPageSize);
        DCHECK_EQ(run->magic_num_, kMagicNum);
        // The dedicated full run doesn't contain any real allocations, don't visit the slots in
        // there.
        run->InspectAllSlots(handler, arg);
        size_t num_pages = numOfPages[run->size_bracket_idx_];
        if (kIsDebugBuild) {
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            DCHECK_EQ(page_map_[j], kPageMapRunPart);
          }
        }
        i += num_pages;
        DCHECK_LE(i, pm_end);
        break;
      }
      case kPageMapRunPart:
        LOG(FATAL) << "Unreachable - page map type: " << pm;
        break;
      default:
        LOG(FATAL) << "Unreachable - page map type: " << pm;
        break;
    }
  }
}

size_t RosAlloc::Footprint() {
  MutexLock mu(Thread::Current(), lock_);
  return footprint_;
}

size_t RosAlloc::FootprintLimit() {
  MutexLock mu(Thread::Current(), lock_);
  return capacity_;
}

void RosAlloc::SetFootprintLimit(size_t new_capacity) {
  MutexLock mu(Thread::Current(), lock_);
  DCHECK_EQ(RoundUp(new_capacity, kPageSize), new_capacity);
  // Only growing is supported here. But Trim() is supported.
  if (capacity_ < new_capacity) {
    CHECK_LE(new_capacity, max_capacity_);
    capacity_ = new_capacity;
    VLOG(heap) << "new capacity=" << capacity_;
  }
}

void RosAlloc::RevokeThreadLocalRuns(Thread* thread) {
  Thread* self = Thread::Current();
  // Avoid race conditions on the bulk free bit maps with BulkFree() (GC).
  WriterMutexLock wmu(self, bulk_free_lock_);
  for (size_t idx = 0; idx < kNumThreadLocalSizeBrackets; idx++) {
    MutexLock mu(self, *size_bracket_locks_[idx]);
    Run* thread_local_run = reinterpret_cast<Run*>(thread->GetRosAllocRun(idx));
    CHECK(thread_local_run != nullptr);
    // Invalid means already revoked.
    DCHECK(thread_local_run->IsThreadLocal());
    if (thread_local_run != dedicated_full_run_) {
      thread->SetRosAllocRun(idx, dedicated_full_run_);
      DCHECK_EQ(thread_local_run->magic_num_, kMagicNum);
      // Note the thread local run may not be full here.
      bool dont_care;
      thread_local_run->MergeThreadLocalFreeBitMapToAllocBitMap(&dont_care);
      thread_local_run->SetIsThreadLocal(false);
      thread_local_run->MergeBulkFreeBitMapIntoAllocBitMap();
      DCHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(thread_local_run) == full_runs_[idx].end());
      RevokeRun(self, idx, thread_local_run);
    }
  }
}

void RosAlloc::RevokeRun(Thread* self, size_t idx, Run* run) {
  size_bracket_locks_[idx]->AssertHeld(self);
  DCHECK(run != dedicated_full_run_);
  if (run->IsFull()) {
    if (kIsDebugBuild) {
      full_runs_[idx].insert(run);
      DCHECK(full_runs_[idx].find(run) != full_runs_[idx].end());
      if (kTraceRosAlloc) {
        LOG(INFO) << __FUNCTION__  << " : Inserted run 0x" << std::hex
                  << reinterpret_cast<intptr_t>(run)
                  << " into full_runs_[" << std::dec << idx << "]";
      }
    }
  } else if (run->IsAllFree()) {
    run->ZeroHeader();
    MutexLock mu(self, lock_);
    FreePages(self, run, true);
  } else {
    non_full_runs_[idx].insert(run);
    DCHECK(non_full_runs_[idx].find(run) != non_full_runs_[idx].end());
    if (kTraceRosAlloc) {
      LOG(INFO) << __FUNCTION__ << " : Inserted run 0x" << std::hex
                << reinterpret_cast<intptr_t>(run)
                << " into non_full_runs_[" << std::dec << idx << "]";
    }
  }
}

void RosAlloc::RevokeThreadUnsafeCurrentRuns() {
  // Revoke the current runs which share the same idx as thread local runs.
  Thread* self = Thread::Current();
  for (size_t idx = 0; idx < kNumThreadLocalSizeBrackets; ++idx) {
    MutexLock mu(self, *size_bracket_locks_[idx]);
    if (current_runs_[idx] != dedicated_full_run_) {
      RevokeRun(self, idx, current_runs_[idx]);
      current_runs_[idx] = dedicated_full_run_;
    }
  }
}

void RosAlloc::RevokeAllThreadLocalRuns() {
  // This is called when a mutator thread won't allocate such as at
  // the Zygote creation time or during the GC pause.
  MutexLock mu(Thread::Current(), *Locks::runtime_shutdown_lock_);
  MutexLock mu2(Thread::Current(), *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  for (Thread* thread : thread_list) {
    RevokeThreadLocalRuns(thread);
  }
  RevokeThreadUnsafeCurrentRuns();
}

void RosAlloc::AssertThreadLocalRunsAreRevoked(Thread* thread) {
  if (kIsDebugBuild) {
    Thread* self = Thread::Current();
    // Avoid race conditions on the bulk free bit maps with BulkFree() (GC).
    WriterMutexLock wmu(self, bulk_free_lock_);
    for (size_t idx = 0; idx < kNumThreadLocalSizeBrackets; idx++) {
      MutexLock mu(self, *size_bracket_locks_[idx]);
      Run* thread_local_run = reinterpret_cast<Run*>(thread->GetRosAllocRun(idx));
      DCHECK(thread_local_run == nullptr || thread_local_run == dedicated_full_run_);
    }
  }
}

void RosAlloc::AssertAllThreadLocalRunsAreRevoked() {
  if (kIsDebugBuild) {
    Thread* self = Thread::Current();
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    MutexLock mu2(self, *Locks::thread_list_lock_);
    std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
    for (Thread* t : thread_list) {
      AssertThreadLocalRunsAreRevoked(t);
    }
    for (size_t idx = 0; idx < kNumThreadLocalSizeBrackets; ++idx) {
      MutexLock mu(self, *size_bracket_locks_[idx]);
      CHECK_EQ(current_runs_[idx], dedicated_full_run_);
    }
  }
}

void RosAlloc::Initialize() {
  // bracketSizes.
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    if (i < kNumOfSizeBrackets - 2) {
      bracketSizes[i] = 16 * (i + 1);
    } else if (i == kNumOfSizeBrackets - 2) {
      bracketSizes[i] = 1 * KB;
    } else {
      DCHECK_EQ(i, kNumOfSizeBrackets - 1);
      bracketSizes[i] = 2 * KB;
    }
    if (kTraceRosAlloc) {
      LOG(INFO) << "bracketSizes[" << i << "]=" << bracketSizes[i];
    }
  }
  // numOfPages.
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    if (i < 4) {
      numOfPages[i] = 1;
    } else if (i < 8) {
      numOfPages[i] = 2;
    } else if (i < 16) {
      numOfPages[i] = 4;
    } else if (i < 32) {
      numOfPages[i] = 8;
    } else if (i == 32) {
      DCHECK_EQ(i, kNumOfSizeBrackets - 2);
      numOfPages[i] = 16;
    } else {
      DCHECK_EQ(i, kNumOfSizeBrackets - 1);
      numOfPages[i] = 32;
    }
    if (kTraceRosAlloc) {
      LOG(INFO) << "numOfPages[" << i << "]=" << numOfPages[i];
    }
  }
  // Compute numOfSlots and slotOffsets.
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    size_t bracket_size = bracketSizes[i];
    size_t run_size = kPageSize * numOfPages[i];
    size_t max_num_of_slots = run_size / bracket_size;
    // Compute the actual number of slots by taking the header and
    // alignment into account.
    size_t fixed_header_size = RoundUp(Run::fixed_header_size(), sizeof(uint32_t));
    DCHECK_EQ(fixed_header_size, static_cast<size_t>(8));
    size_t header_size = 0;
    size_t bulk_free_bit_map_offset = 0;
    size_t thread_local_free_bit_map_offset = 0;
    size_t num_of_slots = 0;
    // Search for the maximum number of slots that allows enough space
    // for the header (including the bit maps.)
    for (int s = max_num_of_slots; s >= 0; s--) {
      size_t tmp_slots_size = bracket_size * s;
      size_t tmp_bit_map_size = RoundUp(s, sizeof(uint32_t) * kBitsPerByte) / kBitsPerByte;
      size_t tmp_bulk_free_bit_map_size = tmp_bit_map_size;
      size_t tmp_bulk_free_bit_map_off = fixed_header_size + tmp_bit_map_size;
      size_t tmp_thread_local_free_bit_map_size = tmp_bit_map_size;
      size_t tmp_thread_local_free_bit_map_off = tmp_bulk_free_bit_map_off + tmp_bulk_free_bit_map_size;
      size_t tmp_unaligned_header_size = tmp_thread_local_free_bit_map_off + tmp_thread_local_free_bit_map_size;
      // Align up the unaligned header size. bracket_size may not be a power of two.
      size_t tmp_header_size = (tmp_unaligned_header_size % bracket_size == 0) ?
          tmp_unaligned_header_size :
          tmp_unaligned_header_size + (bracket_size - tmp_unaligned_header_size % bracket_size);
      DCHECK_EQ(tmp_header_size % bracket_size, static_cast<size_t>(0));
      DCHECK_EQ(tmp_header_size % 8, static_cast<size_t>(0));
      if (tmp_slots_size + tmp_header_size <= run_size) {
        // Found the right number of slots, that is, there was enough
        // space for the header (including the bit maps.)
        num_of_slots = s;
        header_size = tmp_header_size;
        bulk_free_bit_map_offset = tmp_bulk_free_bit_map_off;
        thread_local_free_bit_map_offset = tmp_thread_local_free_bit_map_off;
        break;
      }
    }
    DCHECK(num_of_slots > 0 && header_size > 0 && bulk_free_bit_map_offset > 0);
    // Add the padding for the alignment remainder.
    header_size += run_size % bracket_size;
    DCHECK_EQ(header_size + num_of_slots * bracket_size, run_size);
    numOfSlots[i] = num_of_slots;
    headerSizes[i] = header_size;
    bulkFreeBitMapOffsets[i] = bulk_free_bit_map_offset;
    threadLocalFreeBitMapOffsets[i] = thread_local_free_bit_map_offset;
    if (kTraceRosAlloc) {
      LOG(INFO) << "numOfSlots[" << i << "]=" << numOfSlots[i]
                << ", headerSizes[" << i << "]=" << headerSizes[i]
                << ", bulkFreeBitMapOffsets[" << i << "]=" << bulkFreeBitMapOffsets[i]
                << ", threadLocalFreeBitMapOffsets[" << i << "]=" << threadLocalFreeBitMapOffsets[i];;
    }
  }
  // Fill the alloc bitmap so nobody can successfully allocate from it.
  if (kIsDebugBuild) {
    dedicated_full_run_->magic_num_ = kMagicNum;
  }
  // It doesn't matter which size bracket we use since the main goal is to have the allocation
  // fail 100% of the time you attempt to allocate into the dedicated full run.
  dedicated_full_run_->size_bracket_idx_ = 0;
  dedicated_full_run_->FillAllocBitMap();
  dedicated_full_run_->SetIsThreadLocal(true);
}

void RosAlloc::BytesAllocatedCallback(void* start, void* end, size_t used_bytes, void* arg) {
  if (used_bytes == 0) {
    return;
  }
  size_t* bytes_allocated = reinterpret_cast<size_t*>(arg);
  *bytes_allocated += used_bytes;
}

void RosAlloc::ObjectsAllocatedCallback(void* start, void* end, size_t used_bytes, void* arg) {
  if (used_bytes == 0) {
    return;
  }
  size_t* objects_allocated = reinterpret_cast<size_t*>(arg);
  ++(*objects_allocated);
}

void RosAlloc::Verify() {
  Thread* self = Thread::Current();
  CHECK(Locks::mutator_lock_->IsExclusiveHeld(self))
      << "The mutator locks isn't exclusively locked at RosAlloc::Verify()";
  MutexLock mu(self, *Locks::thread_list_lock_);
  WriterMutexLock wmu(self, bulk_free_lock_);
  std::vector<Run*> runs;
  {
    MutexLock mu(self, lock_);
    size_t pm_end = page_map_size_;
    size_t i = 0;
    while (i < pm_end) {
      byte pm = page_map_[i];
      switch (pm) {
        case kPageMapEmpty: {
          // The start of a free page run.
          FreePageRun* fpr = reinterpret_cast<FreePageRun*>(base_ + i * kPageSize);
          DCHECK_EQ(fpr->magic_num_, kMagicNumFree);
          CHECK(free_page_runs_.find(fpr) != free_page_runs_.end())
              << "An empty page must belong to the free page run set";
          size_t fpr_size = fpr->ByteSize(this);
          CHECK(IsAligned<kPageSize>(fpr_size))
              << "A free page run size isn't page-aligned : " << fpr_size;
          size_t num_pages = fpr_size / kPageSize;
          CHECK_GT(num_pages, static_cast<uintptr_t>(0))
              << "A free page run size must be > 0 : " << fpr_size;
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            CHECK_EQ(page_map_[j], kPageMapEmpty)
                << "A mismatch between the page map table for kPageMapEmpty "
                << " at page index " << j
                << " and the free page run size : page index range : "
                << i << " to " << (i + num_pages) << std::endl << DumpPageMap();
          }
          i += num_pages;
          CHECK_LE(i, pm_end) << "Page map index " << i << " out of range < " << pm_end
                              << std::endl << DumpPageMap();
          break;
        }
        case kPageMapLargeObject: {
          // The start of a large object.
          size_t num_pages = 1;
          size_t idx = i + 1;
          while (idx < pm_end && page_map_[idx] == kPageMapLargeObjectPart) {
            num_pages++;
            idx++;
          }
          void* start = base_ + i * kPageSize;
          mirror::Object* obj = reinterpret_cast<mirror::Object*>(start);
          size_t obj_size = obj->SizeOf();
          CHECK_GT(obj_size, kLargeSizeThreshold)
              << "A rosalloc large object size must be > " << kLargeSizeThreshold;
          CHECK_EQ(num_pages, RoundUp(obj_size, kPageSize) / kPageSize)
              << "A rosalloc large object size " << obj_size
              << " does not match the page map table " << (num_pages * kPageSize)
              << std::endl << DumpPageMap();
          i += num_pages;
          CHECK_LE(i, pm_end) << "Page map index " << i << " out of range < " << pm_end
                              << std::endl << DumpPageMap();
          break;
        }
        case kPageMapLargeObjectPart:
          LOG(FATAL) << "Unreachable - page map type: " << pm << std::endl << DumpPageMap();
          break;
        case kPageMapRun: {
          // The start of a run.
          Run* run = reinterpret_cast<Run*>(base_ + i * kPageSize);
          DCHECK_EQ(run->magic_num_, kMagicNum);
          size_t idx = run->size_bracket_idx_;
          CHECK_LT(idx, kNumOfSizeBrackets) << "Out of range size bracket index : " << idx;
          size_t num_pages = numOfPages[idx];
          CHECK_GT(num_pages, static_cast<uintptr_t>(0))
              << "Run size must be > 0 : " << num_pages;
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            CHECK_EQ(page_map_[j], kPageMapRunPart)
                << "A mismatch between the page map table for kPageMapRunPart "
                << " at page index " << j
                << " and the run size : page index range " << i << " to " << (i + num_pages)
                << std::endl << DumpPageMap();
          }
          // Don't verify the dedicated_full_run_ since it doesn't have any real allocations.
          runs.push_back(run);
          i += num_pages;
          CHECK_LE(i, pm_end) << "Page map index " << i << " out of range < " << pm_end
                              << std::endl << DumpPageMap();
          break;
        }
        case kPageMapRunPart:
          // Fall-through.
        default:
          LOG(FATAL) << "Unreachable - page map type: " << pm << std::endl << DumpPageMap();
          break;
      }
    }
  }
  std::list<Thread*> threads = Runtime::Current()->GetThreadList()->GetList();
  for (Thread* thread : threads) {
    for (size_t i = 0; i < kNumThreadLocalSizeBrackets; ++i) {
      MutexLock mu(self, *size_bracket_locks_[i]);
      Run* thread_local_run = reinterpret_cast<Run*>(thread->GetRosAllocRun(i));
      CHECK(thread_local_run != nullptr);
      CHECK(thread_local_run->IsThreadLocal());
      CHECK(thread_local_run == dedicated_full_run_ ||
            thread_local_run->size_bracket_idx_ == i);
    }
  }
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    MutexLock mu(self, *size_bracket_locks_[i]);
    Run* current_run = current_runs_[i];
    CHECK(current_run != nullptr);
    if (current_run != dedicated_full_run_) {
      // The dedicated full run is currently marked as thread local.
      CHECK(!current_run->IsThreadLocal());
      CHECK_EQ(current_run->size_bracket_idx_, i);
    }
  }
  // Call Verify() here for the lock order.
  for (auto& run : runs) {
    run->Verify(self, this);
  }
}

void RosAlloc::Run::Verify(Thread* self, RosAlloc* rosalloc) {
  DCHECK_EQ(magic_num_, kMagicNum) << "Bad magic number : " << Dump();
  const size_t idx = size_bracket_idx_;
  CHECK_LT(idx, kNumOfSizeBrackets) << "Out of range size bracket index : " << Dump();
  byte* slot_base = reinterpret_cast<byte*>(this) + headerSizes[idx];
  const size_t num_slots = numOfSlots[idx];
  const size_t num_vec = RoundUp(num_slots, 32) / 32;
  CHECK_GT(num_vec, 0U);
  size_t bracket_size = IndexToBracketSize(idx);
  CHECK_EQ(slot_base + num_slots * bracket_size,
           reinterpret_cast<byte*>(this) + numOfPages[idx] * kPageSize)
      << "Mismatch in the end address of the run " << Dump();
  // Check that the bulk free bitmap is clean. It's only used during BulkFree().
  CHECK(IsBulkFreeBitmapClean()) << "The bulk free bit map isn't clean " << Dump();
  uint32_t last_word_mask = GetBitmapLastVectorMask(num_slots, num_vec);
  // Make sure all the bits at the end of the run are set so that we don't allocate there.
  CHECK_EQ(alloc_bit_map_[num_vec - 1] & last_word_mask, last_word_mask);
  // Ensure that the first bitmap index is valid.
  CHECK_LT(first_search_vec_idx_, num_vec);
  // Check the thread local runs, the current runs, and the run sets.
  if (IsThreadLocal()) {
    // If it's a thread local run, then it must be pointed to by an owner thread.
    bool owner_found = false;
    std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
    for (auto it = thread_list.begin(); it != thread_list.end(); ++it) {
      Thread* thread = *it;
      for (size_t i = 0; i < kNumThreadLocalSizeBrackets; i++) {
        MutexLock mu(self, *rosalloc->size_bracket_locks_[i]);
        Run* thread_local_run = reinterpret_cast<Run*>(thread->GetRosAllocRun(i));
        if (thread_local_run == this) {
          CHECK(!owner_found)
              << "A thread local run has more than one owner thread " << Dump();
          CHECK_EQ(i, idx)
              << "A mismatching size bracket index in a thread local run " << Dump();
          owner_found = true;
        }
      }
    }
    CHECK(owner_found) << "A thread local run has no owner thread " << Dump();
  } else {
    // If it's not thread local, check that the thread local free bitmap is clean.
    CHECK(IsThreadLocalFreeBitmapClean())
        << "A non-thread-local run's thread local free bitmap isn't clean "
        << Dump();
    // Check if it's a current run for the size bucket.
    bool is_current_run = false;
    for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
      MutexLock mu(self, *rosalloc->size_bracket_locks_[i]);
      Run* current_run = rosalloc->current_runs_[i];
      if (idx == i) {
        if (this == current_run) {
          is_current_run = true;
        }
      } else {
        // If the size bucket index does not match, then it must not
        // be a current run.
        CHECK_NE(this, current_run)
            << "A current run points to a run with a wrong size bracket index " << Dump();
      }
    }
    // If it's neither a thread local or current run, then it must be
    // in a run set.
    if (!is_current_run) {
      MutexLock mu(self, rosalloc->lock_);
      std::set<Run*>& non_full_runs = rosalloc->non_full_runs_[idx];
      // If it's all free, it must be a free page run rather than a run.
      CHECK(!IsAllFree()) << "A free run must be in a free page run set " << Dump();
      if (!IsFull()) {
        // If it's not full, it must in the non-full run set.
        CHECK(non_full_runs.find(this) != non_full_runs.end())
            << "A non-full run isn't in the non-full run set " << Dump();
      } else {
        // If it's full, it must in the full run set (debug build only.)
        if (kIsDebugBuild) {
          unordered_set<Run*, hash_run, eq_run>& full_runs = rosalloc->full_runs_[idx];
          CHECK(full_runs.find(this) != full_runs.end())
              << " A full run isn't in the full run set " << Dump();
        }
      }
    }
  }
  // Check each slot.
  size_t slots = 0;
  for (size_t v = 0; v < num_vec; v++, slots += 32) {
    DCHECK_GE(num_slots, slots) << "Out of bounds";
    uint32_t vec = alloc_bit_map_[v];
    uint32_t thread_local_free_vec = ThreadLocalFreeBitMap()[v];
    size_t end = std::min(num_slots - slots, static_cast<size_t>(32));
    for (size_t i = 0; i < end; ++i) {
      bool is_allocated = ((vec >> i) & 0x1) != 0;
      // If a thread local run, slots may be marked freed in the
      // thread local free bitmap.
      bool is_thread_local_freed = IsThreadLocal() && ((thread_local_free_vec >> i) & 0x1) != 0;
      if (is_allocated && !is_thread_local_freed) {
        byte* slot_addr = slot_base + (slots + i) * bracket_size;
        mirror::Object* obj = reinterpret_cast<mirror::Object*>(slot_addr);
        size_t obj_size = obj->SizeOf();
        CHECK_LE(obj_size, kLargeSizeThreshold)
            << "A run slot contains a large object " << Dump();
        CHECK_EQ(SizeToIndex(obj_size), idx)
            << PrettyTypeOf(obj) << " "
            << "obj_size=" << obj_size << ", idx=" << idx << " "
            << "A run slot contains an object with wrong size " << Dump();
      }
    }
  }
}

size_t RosAlloc::ReleasePages() {
  VLOG(heap) << "RosAlloc::ReleasePages()";
  DCHECK(!DoesReleaseAllPages());
  Thread* self = Thread::Current();
  size_t reclaimed_bytes = 0;
  size_t i = 0;
  while (true) {
    MutexLock mu(self, lock_);
    // Check the page map size which might have changed due to grow/shrink.
    size_t pm_end = page_map_size_;
    if (i >= pm_end) {
      // Reached the end.
      break;
    }
    byte pm = page_map_[i];
    switch (pm) {
      case kPageMapEmpty: {
        // The start of a free page run. Release pages.
        FreePageRun* fpr = reinterpret_cast<FreePageRun*>(base_ + i * kPageSize);
        DCHECK(free_page_runs_.find(fpr) != free_page_runs_.end());
        size_t fpr_size = fpr->ByteSize(this);
        DCHECK(IsAligned<kPageSize>(fpr_size));
        byte* start = reinterpret_cast<byte*>(fpr);
        if (kIsDebugBuild) {
          // In the debug build, the first page of a free page run
          // contains a magic number for debugging. Exclude it.
          start = reinterpret_cast<byte*>(fpr) + kPageSize;
        }
        byte* end = reinterpret_cast<byte*>(fpr) + fpr_size;
        CHECK_EQ(madvise(start, end - start, MADV_DONTNEED), 0);
        reclaimed_bytes += fpr_size;
        size_t num_pages = fpr_size / kPageSize;
        if (kIsDebugBuild) {
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            DCHECK_EQ(page_map_[j], kPageMapEmpty);
          }
        }
        i += num_pages;
        DCHECK_LE(i, pm_end);
        break;
      }
      case kPageMapLargeObject:      // Fall through.
      case kPageMapLargeObjectPart:  // Fall through.
      case kPageMapRun:              // Fall through.
      case kPageMapRunPart:          // Fall through.
        ++i;
        break;  // Skip.
      default:
        LOG(FATAL) << "Unreachable - page map type: " << pm;
        break;
    }
  }
  return reclaimed_bytes;
}

}  // namespace allocator
}  // namespace gc
}  // namespace art
