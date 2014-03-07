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

size_t RosAlloc::bracketSizes[kNumOfSizeBrackets];
size_t RosAlloc::numOfPages[kNumOfSizeBrackets];
size_t RosAlloc::numOfSlots[kNumOfSizeBrackets];
size_t RosAlloc::headerSizes[kNumOfSizeBrackets];
size_t RosAlloc::bulkFreeBitMapOffsets[kNumOfSizeBrackets];
size_t RosAlloc::threadLocalFreeBitMapOffsets[kNumOfSizeBrackets];
bool RosAlloc::initialized_ = false;

RosAlloc::RosAlloc(void* base, size_t capacity,
                   PageReleaseMode page_release_mode, size_t page_release_size_threshold)
    : base_(reinterpret_cast<byte*>(base)), footprint_(capacity),
      capacity_(capacity),
      lock_("rosalloc global lock", kRosAllocGlobalLock),
      bulk_free_lock_("rosalloc bulk free lock", kRosAllocBulkFreeLock),
      page_release_mode_(page_release_mode),
      page_release_size_threshold_(page_release_size_threshold) {
  DCHECK(RoundUp(capacity, kPageSize) == capacity);
  CHECK(IsAligned<kPageSize>(page_release_size_threshold_));
  if (!initialized_) {
    Initialize();
  }
  VLOG(heap) << "RosAlloc base="
             << std::hex << (intptr_t)base_ << ", end="
             << std::hex << (intptr_t)(base_ + capacity_)
             << ", capacity=" << std::dec << capacity_;
  memset(current_runs_, 0, sizeof(current_runs_));
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    size_bracket_locks_[i] = new Mutex("an rosalloc size bracket lock",
                                       kRosAllocBracketLock);
  }
  size_t num_of_pages = capacity_ / kPageSize;
  page_map_.resize(num_of_pages);
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

void* RosAlloc::AllocPages(Thread* self, size_t num_pages, byte page_map_type) {
  lock_.AssertHeld(self);
  DCHECK(page_map_type == kPageMapRun || page_map_type == kPageMapLargeObject);
  FreePageRun* res = NULL;
  size_t req_byte_size = num_pages * kPageSize;
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
      DCHECK(page_map_[ToPageMapIndex(last_free_page_run)] == kPageMapEmpty);
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
      DCHECK_LT(page_map_.size(), new_num_of_pages);
      DCHECK_LT(free_page_run_size_map_.size(), new_num_of_pages);
      page_map_.resize(new_num_of_pages);
      free_page_run_size_map_.resize(new_num_of_pages);
      art_heap_rosalloc_morecore(this, increment);
      if (last_free_page_run_size > 0) {
        // There was a free page run at the end. Expand its size.
        DCHECK_EQ(last_free_page_run_size, last_free_page_run->ByteSize(this));
        last_free_page_run->SetByteSize(this, last_free_page_run_size + increment);
        DCHECK_EQ(last_free_page_run->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        DCHECK(last_free_page_run->End(this) == base_ + new_footprint);
      } else {
        // Otherwise, insert a new free page run at the end.
        FreePageRun* new_free_page_run = reinterpret_cast<FreePageRun*>(base_ + footprint_);
        if (kIsDebugBuild) {
          new_free_page_run->magic_num_ = kMagicNumFree;
        }
        new_free_page_run->SetByteSize(this, increment);
        DCHECK_EQ(new_free_page_run->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        free_page_runs_.insert(new_free_page_run);
        DCHECK(*free_page_runs_.rbegin() == new_free_page_run);
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
      DCHECK(page_map_[page_map_idx + i] == kPageMapEmpty);
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
      // Clear the first page which isn't madvised away in the debug
      // build for the magic number.
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

void RosAlloc::FreePages(Thread* self, void* ptr) {
  lock_.AssertHeld(self);
  size_t pm_idx = ToPageMapIndex(ptr);
  DCHECK(pm_idx < page_map_.size());
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
    return;
  }
  // Update the page map and count the number of pages.
  size_t num_pages = 1;
  page_map_[pm_idx] = kPageMapEmpty;
  size_t idx = pm_idx + 1;
  size_t end = page_map_.size();
  while (idx < end && page_map_[idx] == pm_part_type) {
    page_map_[idx] = kPageMapEmpty;
    num_pages++;
    idx++;
  }

  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::FreePages() : 0x" << std::hex << reinterpret_cast<intptr_t>(ptr)
              << "-0x" << (reinterpret_cast<intptr_t>(ptr) + num_pages * kPageSize)
              << "(" << std::dec << (num_pages * kPageSize) << ")";
  }

  // Turn it into a free run.
  FreePageRun* fpr = reinterpret_cast<FreePageRun*>(ptr);
  if (kIsDebugBuild) {
    fpr->magic_num_ = kMagicNumFree;
  }
  fpr->SetByteSize(this, num_pages * kPageSize);
  DCHECK_EQ(fpr->ByteSize(this) % kPageSize, static_cast<size_t>(0));

  DCHECK(free_page_runs_.find(fpr) == free_page_runs_.end());
  if (!free_page_runs_.empty()) {
    // Try to coalesce in the higher address direction.
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::FreePages() : trying to coalesce a free page run 0x"
                << std::hex << reinterpret_cast<uintptr_t>(fpr) << " [" << std::dec << pm_idx << "] -0x"
                << std::hex << reinterpret_cast<uintptr_t>(fpr->End(this)) << " [" << std::dec
                << (fpr->End(this) == End() ? page_map_.size() : ToPageMapIndex(fpr->End(this))) << "]";
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
                    << (h->End(this) == End() ? page_map_.size() : ToPageMapIndex(h->End(this))) << "]";
        }
        if (fpr->End(this) == h->Begin()) {
          if (kTraceRosAlloc) {
            LOG(INFO) << "Success";
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
                    << (l->End(this) == End() ? page_map_.size() : ToPageMapIndex(l->End(this))) << "]";
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
}

void* RosAlloc::AllocLargeObject(Thread* self, size_t size, size_t* bytes_allocated) {
  DCHECK(size > kLargeSizeThreshold);
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
  if (bytes_allocated != NULL) {
    *bytes_allocated = num_pages * kPageSize;
  }
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::AllocLargeObject() : 0x" << std::hex << reinterpret_cast<intptr_t>(r)
              << "-0x" << (reinterpret_cast<intptr_t>(r) + num_pages * kPageSize)
              << "(" << std::dec << (num_pages * kPageSize) << ")";
  }
  if (!DoesReleaseAllPages()) {
    // If it does not release all pages, pages may not be zeroed out.
    memset(r, 0, size);
  }
  // Check if the returned memory is really all zero.
  if (kCheckZeroMemory) {
    byte* bytes = reinterpret_cast<byte*>(r);
    for (size_t i = 0; i < size; ++i) {
      DCHECK_EQ(bytes[i], 0);
    }
  }
  return r;
}

void RosAlloc::FreeInternal(Thread* self, void* ptr) {
  DCHECK(base_ <= ptr && ptr < base_ + footprint_);
  size_t pm_idx = RoundDownToPageMapIndex(ptr);
  bool free_from_run = false;
  Run* run = NULL;
  {
    MutexLock mu(self, lock_);
    DCHECK(pm_idx < page_map_.size());
    byte page_map_entry = page_map_[pm_idx];
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::FreeInternal() : " << std::hex << ptr << ", pm_idx=" << std::dec << pm_idx
                << ", page_map_entry=" << static_cast<int>(page_map_entry);
    }
    switch (page_map_[pm_idx]) {
      case kPageMapEmpty:
        LOG(FATAL) << "Unreachable - page map type: " << page_map_[pm_idx];
        return;
      case kPageMapLargeObject:
        FreePages(self, ptr);
        return;
      case kPageMapLargeObjectPart:
        LOG(FATAL) << "Unreachable - page map type: " << page_map_[pm_idx];
        return;
      case kPageMapRun:
      case kPageMapRunPart: {
        free_from_run = true;
        size_t pi = pm_idx;
        DCHECK(page_map_[pi] == kPageMapRun || page_map_[pi] == kPageMapRunPart);
        // Find the beginning of the run.
        while (page_map_[pi] != kPageMapRun) {
          pi--;
          DCHECK(pi < capacity_ / kPageSize);
        }
        DCHECK(page_map_[pi] == kPageMapRun);
        run = reinterpret_cast<Run*>(base_ + pi * kPageSize);
        DCHECK(run->magic_num_ == kMagicNum);
        break;
      }
      default:
        LOG(FATAL) << "Unreachable - page map type: " << page_map_[pm_idx];
        return;
    }
  }
  if (LIKELY(free_from_run)) {
    DCHECK(run != NULL);
    FreeFromRun(self, ptr, run);
  }
}

void RosAlloc::Free(Thread* self, void* ptr) {
  ReaderMutexLock rmu(self, bulk_free_lock_);
  FreeInternal(self, ptr);
}

RosAlloc::Run* RosAlloc::RefillRun(Thread* self, size_t idx) {
  Run* new_run;
  size_t num_pages = numOfPages[idx];
  // Get the lowest address non-full run from the binary tree.
  Run* temp = NULL;
  std::set<Run*>* bt = &non_full_runs_[idx];
  std::set<Run*>::iterator found = bt->lower_bound(temp);
  if (found != bt->end()) {
    // If there's one, use it as the current run.
    Run* non_full_run = *found;
    DCHECK(non_full_run != NULL);
    new_run = non_full_run;
    DCHECK_EQ(new_run->is_thread_local_, 0);
    bt->erase(found);
    DCHECK_EQ(non_full_run->is_thread_local_, 0);
  } else {
    // If there's none, allocate a new run and use it as the
    // current run.
    {
      MutexLock mu(self, lock_);
      new_run = reinterpret_cast<Run*>(AllocPages(self, num_pages, kPageMapRun));
    }
    if (new_run == NULL) {
      return NULL;
    }
    if (kIsDebugBuild) {
      new_run->magic_num_ = kMagicNum;
    }
    new_run->size_bracket_idx_ = idx;
    new_run->top_slot_idx_ = 0;
    new_run->ClearBitMaps();
    new_run->to_be_bulk_freed_ = false;
  }
  return new_run;
}

void* RosAlloc::AllocFromRun(Thread* self, size_t size, size_t* bytes_allocated) {
  DCHECK(size <= kLargeSizeThreshold);
  size_t bracket_size;
  size_t idx = SizeToIndexAndBracketSize(size, &bracket_size);
  DCHECK_EQ(idx, SizeToIndex(size));
  DCHECK_EQ(bracket_size, IndexToBracketSize(idx));
  DCHECK_EQ(bracket_size, bracketSizes[idx]);
  DCHECK(size <= bracket_size);
  DCHECK(size > 512 || bracket_size - size < 16);

  void* slot_addr;

  if (LIKELY(idx <= kMaxThreadLocalSizeBracketIdx)) {
    // Use a thread-local run.
    Run* thread_local_run = reinterpret_cast<Run*>(self->rosalloc_runs_[idx]);
    if (UNLIKELY(thread_local_run == NULL)) {
      MutexLock mu(self, *size_bracket_locks_[idx]);
      thread_local_run = RefillRun(self, idx);
      if (UNLIKELY(thread_local_run == NULL)) {
        return NULL;
      }
      DCHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(thread_local_run) == full_runs_[idx].end());
      thread_local_run->is_thread_local_ = 1;
      self->rosalloc_runs_[idx] = thread_local_run;
      DCHECK(!thread_local_run->IsFull());
    }

    DCHECK(thread_local_run != NULL);
    DCHECK_NE(thread_local_run->is_thread_local_, 0);
    slot_addr = thread_local_run->AllocSlot();

    if (UNLIKELY(slot_addr == NULL)) {
      // The run got full. Try to free slots.
      DCHECK(thread_local_run->IsFull());
      MutexLock mu(self, *size_bracket_locks_[idx]);
      bool is_all_free_after_merge;
      if (thread_local_run->MergeThreadLocalFreeBitMapToAllocBitMap(&is_all_free_after_merge)) {
        // Some slot got freed. Keep it.
        DCHECK(!thread_local_run->IsFull());
        DCHECK_EQ(is_all_free_after_merge, thread_local_run->IsAllFree());
        if (is_all_free_after_merge) {
          // Reinstate the bump index mode if it's all free.
          DCHECK_EQ(thread_local_run->top_slot_idx_, numOfSlots[idx]);
          thread_local_run->top_slot_idx_ = 0;
        }
      } else {
        // No slots got freed. Try to refill the thread-local run.
        DCHECK(thread_local_run->IsFull());
        self->rosalloc_runs_[idx] = NULL;
        thread_local_run->is_thread_local_ = 0;
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
        thread_local_run = RefillRun(self, idx);
        if (UNLIKELY(thread_local_run == NULL)) {
          return NULL;
        }
        DCHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
        DCHECK(full_runs_[idx].find(thread_local_run) == full_runs_[idx].end());
        thread_local_run->is_thread_local_ = 1;
        self->rosalloc_runs_[idx] = thread_local_run;
        DCHECK(!thread_local_run->IsFull());
      }

      DCHECK(thread_local_run != NULL);
      DCHECK(!thread_local_run->IsFull());
      DCHECK_NE(thread_local_run->is_thread_local_, 0);
      slot_addr = thread_local_run->AllocSlot();
      // Must succeed now with a new run.
      DCHECK(slot_addr != NULL);
    }
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::AllocFromRun() thread-local : 0x" << std::hex << reinterpret_cast<intptr_t>(slot_addr)
                << "-0x" << (reinterpret_cast<intptr_t>(slot_addr) + bracket_size)
                << "(" << std::dec << (bracket_size) << ")";
    }
  } else {
    // Use the (shared) current run.
    MutexLock mu(self, *size_bracket_locks_[idx]);
    Run* current_run = current_runs_[idx];
    if (UNLIKELY(current_run == NULL)) {
      current_run = RefillRun(self, idx);
      if (UNLIKELY(current_run == NULL)) {
        return NULL;
      }
      DCHECK(non_full_runs_[idx].find(current_run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(current_run) == full_runs_[idx].end());
      current_run->is_thread_local_ = 0;
      current_runs_[idx] = current_run;
      DCHECK(!current_run->IsFull());
    }
    DCHECK(current_run != NULL);
    slot_addr = current_run->AllocSlot();
    if (UNLIKELY(slot_addr == NULL)) {
      // The current run got full. Try to refill it.
      DCHECK(current_run->IsFull());
      current_runs_[idx] = NULL;
      if (kIsDebugBuild) {
        // Insert it into full_runs and set the current run to NULL.
        full_runs_[idx].insert(current_run);
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::AllocFromRun() : Inserted run 0x" << std::hex << reinterpret_cast<intptr_t>(current_run)
                    << " into full_runs_[" << std::dec << idx << "]";
        }
      }
      DCHECK(non_full_runs_[idx].find(current_run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(current_run) != full_runs_[idx].end());
      current_run = RefillRun(self, idx);
      if (UNLIKELY(current_run == NULL)) {
        return NULL;
      }
      DCHECK(current_run != NULL);
      DCHECK(non_full_runs_[idx].find(current_run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(current_run) == full_runs_[idx].end());
      current_run->is_thread_local_ = 0;
      current_runs_[idx] = current_run;
      DCHECK(!current_run->IsFull());
      slot_addr = current_run->AllocSlot();
      // Must succeed now with a new run.
      DCHECK(slot_addr != NULL);
    }
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::AllocFromRun() : 0x" << std::hex << reinterpret_cast<intptr_t>(slot_addr)
                << "-0x" << (reinterpret_cast<intptr_t>(slot_addr) + bracket_size)
                << "(" << std::dec << (bracket_size) << ")";
    }
  }
  if (LIKELY(bytes_allocated != NULL)) {
    *bytes_allocated = bracket_size;
  }
  memset(slot_addr, 0, size);
  return slot_addr;
}

void RosAlloc::FreeFromRun(Thread* self, void* ptr, Run* run) {
  DCHECK(run->magic_num_ == kMagicNum);
  DCHECK(run < ptr && ptr < run->End());
  size_t idx = run->size_bracket_idx_;
  MutexLock mu(self, *size_bracket_locks_[idx]);
  bool run_was_full = false;
  if (kIsDebugBuild) {
    run_was_full = run->IsFull();
  }
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::FreeFromRun() : 0x" << std::hex << reinterpret_cast<intptr_t>(ptr);
  }
  if (LIKELY(run->is_thread_local_ != 0)) {
    // It's a thread-local run. Just mark the thread-local free bit map and return.
    DCHECK_LE(run->size_bracket_idx_, kMaxThreadLocalSizeBracketIdx);
    DCHECK(non_full_runs_[idx].find(run) == non_full_runs_[idx].end());
    DCHECK(full_runs_[idx].find(run) == full_runs_[idx].end());
    run->MarkThreadLocalFreeBitMap(ptr);
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::FreeFromRun() : Freed a slot in a thread local run 0x" << std::hex
                << reinterpret_cast<intptr_t>(run);
    }
    // A thread local run will be kept as a thread local even if it's become all free.
    return;
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
      current_runs_[idx] = NULL;
    }
    DCHECK(non_full_runs_[idx].find(run) == non_full_runs_[idx].end());
    DCHECK(full_runs_[idx].find(run) == full_runs_[idx].end());
    {
      MutexLock mu(self, lock_);
      FreePages(self, run);
    }
  } else {
    // It is not completely free. If it wasn't the current run or
    // already in the non-full run set (i.e., it was full) insert it
    // into the non-full run set.
    if (run != current_runs_[idx]) {
      hash_set<Run*, hash_run, eq_run>* full_runs =
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
         << " top_slot_idx=" << top_slot_idx_
         << " alloc_bit_map=" << BitMapToStr(alloc_bit_map_, num_vec)
         << " bulk_free_bit_map=" << BitMapToStr(BulkFreeBitMap(), num_vec)
         << " thread_local_bit_map=" << BitMapToStr(ThreadLocalFreeBitMap(), num_vec)
         << " }" << std::endl;
  return stream.str();
}

void* RosAlloc::Run::AllocSlot() {
  size_t idx = size_bracket_idx_;
  size_t num_slots = numOfSlots[idx];
  DCHECK_LE(top_slot_idx_, num_slots);
  if (LIKELY(top_slot_idx_ < num_slots)) {
    // If it's in bump index mode, grab the top slot and increment the top index.
    size_t slot_idx = top_slot_idx_;
    byte* slot_addr = reinterpret_cast<byte*>(this) + headerSizes[idx] + slot_idx * bracketSizes[idx];
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::Run::AllocSlot() : 0x" << std::hex << reinterpret_cast<intptr_t>(slot_addr)
                << ", bracket_size=" << std::dec << bracketSizes[idx] << ", slot_idx=" << slot_idx;
    }
    top_slot_idx_++;
    size_t vec_idx = slot_idx / 32;
    size_t vec_off = slot_idx % 32;
    uint32_t* vec = &alloc_bit_map_[vec_idx];
    DCHECK_EQ((*vec & (1 << vec_off)), static_cast<uint32_t>(0));
    *vec |= 1 << vec_off;
    DCHECK_NE((*vec & (1 << vec_off)), static_cast<uint32_t>(0));
    return slot_addr;
  }
  // Not in bump index mode. Search the alloc bit map for an empty slot.
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  size_t slot_idx = 0;
  bool found_slot = false;
  for (size_t v = 0; v < num_vec; v++) {
    uint32_t *vecp = &alloc_bit_map_[v];
    uint32_t ffz1 = __builtin_ffs(~*vecp);
    uint32_t ffz;
    // TODO: Use LIKELY or UNLIKELY here?
    if (LIKELY(ffz1 > 0 && (ffz = ffz1 - 1) + v * 32 < num_slots)) {
      // Found an empty slot. Set the bit.
      DCHECK_EQ((*vecp & (1 << ffz)), static_cast<uint32_t>(0));
      *vecp |= (1 << ffz);
      DCHECK_NE((*vecp & (1 << ffz)), static_cast<uint32_t>(0));
      slot_idx = ffz + v * 32;
      found_slot = true;
      break;
    }
  }
  if (LIKELY(found_slot)) {
    byte* slot_addr = reinterpret_cast<byte*>(this) + headerSizes[idx] + slot_idx * bracketSizes[idx];
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::Run::AllocSlot() : 0x" << std::hex << reinterpret_cast<intptr_t>(slot_addr)
                << ", bracket_size=" << std::dec << bracketSizes[idx] << ", slot_idx=" << slot_idx;
    }
    return slot_addr;
  }
  return NULL;
}

inline void RosAlloc::Run::FreeSlot(void* ptr) {
  DCHECK_EQ(is_thread_local_, 0);
  byte idx = size_bracket_idx_;
  size_t offset_from_slot_base = reinterpret_cast<byte*>(ptr)
      - (reinterpret_cast<byte*>(this) + headerSizes[idx]);
  DCHECK_EQ(offset_from_slot_base % bracketSizes[idx], static_cast<size_t>(0));
  size_t slot_idx = offset_from_slot_base / bracketSizes[idx];
  DCHECK(slot_idx < numOfSlots[idx]);
  size_t vec_idx = slot_idx / 32;
  if (kIsDebugBuild) {
    size_t num_vec = RoundUp(numOfSlots[idx], 32) / 32;
    DCHECK(vec_idx < num_vec);
  }
  size_t vec_off = slot_idx % 32;
  uint32_t* vec = &alloc_bit_map_[vec_idx];
  DCHECK_NE((*vec & (1 << vec_off)), static_cast<uint32_t>(0));
  *vec &= ~(1 << vec_off);
  DCHECK_EQ((*vec & (1 << vec_off)), static_cast<uint32_t>(0));
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::Run::FreeSlot() : 0x" << std::hex << reinterpret_cast<intptr_t>(ptr)
              << ", bracket_size=" << std::dec << bracketSizes[idx] << ", slot_idx=" << slot_idx;
  }
}

inline bool RosAlloc::Run::MergeThreadLocalFreeBitMapToAllocBitMap(bool* is_all_free_after_out) {
  DCHECK_NE(is_thread_local_, 0);
  // Free slots in the alloc bit map based on the thread local free bit map.
  byte idx = size_bracket_idx_;
  size_t num_slots = numOfSlots[idx];
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  bool changed = false;
  uint32_t* vecp = &alloc_bit_map_[0];
  uint32_t* tl_free_vecp = &ThreadLocalFreeBitMap()[0];
  bool is_all_free_after = true;
  for (size_t v = 0; v < num_vec; v++, vecp++, tl_free_vecp++) {
    uint32_t tl_free_vec = *tl_free_vecp;
    uint32_t vec_before = *vecp;
    uint32_t vec_after;
    if (tl_free_vec != 0) {
      vec_after = vec_before & ~tl_free_vec;
      *vecp = vec_after;
      changed = true;
      *tl_free_vecp = 0;  // clear the thread local free bit map.
    } else {
      vec_after = vec_before;
    }
    if (vec_after != 0) {
      is_all_free_after = false;
    }
    DCHECK_EQ(*tl_free_vecp, static_cast<uint32_t>(0));
  }
  *is_all_free_after_out = is_all_free_after;
  // Return true if there was at least a bit set in the thread-local
  // free bit map and at least a bit in the alloc bit map changed.
  return changed;
}

inline void RosAlloc::Run::MergeBulkFreeBitMapIntoAllocBitMap() {
  DCHECK_EQ(is_thread_local_, 0);
  // Free slots in the alloc bit map based on the bulk free bit map.
  byte idx = size_bracket_idx_;
  size_t num_slots = numOfSlots[idx];
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  uint32_t* vecp = &alloc_bit_map_[0];
  uint32_t* free_vecp = &BulkFreeBitMap()[0];
  for (size_t v = 0; v < num_vec; v++, vecp++, free_vecp++) {
    uint32_t free_vec = *free_vecp;
    if (free_vec != 0) {
      *vecp &= ~free_vec;
      *free_vecp = 0;  // clear the bulk free bit map.
    }
    DCHECK_EQ(*free_vecp, static_cast<uint32_t>(0));
  }
}

inline void RosAlloc::Run::UnionBulkFreeBitMapToThreadLocalFreeBitMap() {
  DCHECK_NE(is_thread_local_, 0);
  // Union the thread local bit map with the bulk free bit map.
  byte idx = size_bracket_idx_;
  size_t num_slots = numOfSlots[idx];
  size_t num_vec = RoundUp(num_slots, 32) / 32;
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
  DCHECK_NE(is_thread_local_, 0);
  MarkFreeBitMapShared(ptr, ThreadLocalFreeBitMap(), "MarkThreadLocalFreeBitMap");
}

inline void RosAlloc::Run::MarkBulkFreeBitMap(void* ptr) {
  MarkFreeBitMapShared(ptr, BulkFreeBitMap(), "MarkFreeBitMap");
}

inline void RosAlloc::Run::MarkFreeBitMapShared(void* ptr, uint32_t* free_bit_map_base,
                                              const char* caller_name) {
  byte idx = size_bracket_idx_;
  size_t offset_from_slot_base = reinterpret_cast<byte*>(ptr)
      - (reinterpret_cast<byte*>(this) + headerSizes[idx]);
  DCHECK_EQ(offset_from_slot_base % bracketSizes[idx], static_cast<size_t>(0));
  size_t slot_idx = offset_from_slot_base / bracketSizes[idx];
  DCHECK(slot_idx < numOfSlots[idx]);
  size_t vec_idx = slot_idx / 32;
  if (kIsDebugBuild) {
    size_t num_vec = RoundUp(numOfSlots[idx], 32) / 32;
    DCHECK(vec_idx < num_vec);
  }
  size_t vec_off = slot_idx % 32;
  uint32_t* vec = &free_bit_map_base[vec_idx];
  DCHECK_EQ((*vec & (1 << vec_off)), static_cast<uint32_t>(0));
  *vec |= 1 << vec_off;
  DCHECK_NE((*vec & (1 << vec_off)), static_cast<uint32_t>(0));
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::Run::" << caller_name << "() : 0x" << std::hex
              << reinterpret_cast<intptr_t>(ptr)
              << ", bracket_size=" << std::dec << bracketSizes[idx] << ", slot_idx=" << slot_idx;
  }
}

inline bool RosAlloc::Run::IsAllFree() {
  byte idx = size_bracket_idx_;
  size_t num_slots = numOfSlots[idx];
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  for (size_t v = 0; v < num_vec; v++) {
    uint32_t vec = alloc_bit_map_[v];
    if (vec != 0) {
      return false;
    }
  }
  return true;
}

inline bool RosAlloc::Run::IsFull() {
  byte idx = size_bracket_idx_;
  size_t num_slots = numOfSlots[idx];
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  size_t slots = 0;
  for (size_t v = 0; v < num_vec; v++, slots += 32) {
    DCHECK(num_slots >= slots);
    uint32_t vec = alloc_bit_map_[v];
    uint32_t mask = (num_slots - slots >= 32) ? static_cast<uint32_t>(-1)
        : (1 << (num_slots - slots)) - 1;
    DCHECK(num_slots - slots >= 32 ? mask == static_cast<uint32_t>(-1) : true);
    if (vec != mask) {
      return false;
    }
  }
  return true;
}

inline bool RosAlloc::Run::IsBulkFreeBitmapClean() {
  byte idx = size_bracket_idx_;
  size_t num_slots = numOfSlots[idx];
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  for (size_t v = 0; v < num_vec; v++) {
    uint32_t vec = BulkFreeBitMap()[v];
    if (vec != 0) {
      return false;
    }
  }
  return true;
}

inline bool RosAlloc::Run::IsThreadLocalFreeBitmapClean() {
  byte idx = size_bracket_idx_;
  size_t num_slots = numOfSlots[idx];
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  for (size_t v = 0; v < num_vec; v++) {
    uint32_t vec = ThreadLocalFreeBitMap()[v];
    if (vec != 0) {
      return false;
    }
  }
  return true;
}

inline void RosAlloc::Run::ClearBitMaps() {
  byte idx = size_bracket_idx_;
  size_t num_slots = numOfSlots[idx];
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  memset(alloc_bit_map_, 0, sizeof(uint32_t) * num_vec * 3);
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
    DCHECK(num_slots >= slots);
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

void RosAlloc::BulkFree(Thread* self, void** ptrs, size_t num_ptrs) {
  if (false) {
    // Used only to test Free() as GC uses only BulkFree().
    for (size_t i = 0; i < num_ptrs; ++i) {
      FreeInternal(self, ptrs[i]);
    }
    return;
  }

  WriterMutexLock wmu(self, bulk_free_lock_);

  // First mark slots to free in the bulk free bit map without locking the
  // size bracket locks. On host, hash_set is faster than vector + flag.
#ifdef HAVE_ANDROID_OS
  std::vector<Run*> runs;
#else
  hash_set<Run*, hash_run, eq_run> runs;
#endif
  {
    for (size_t i = 0; i < num_ptrs; i++) {
      void* ptr = ptrs[i];
      ptrs[i] = NULL;
      DCHECK(base_ <= ptr && ptr < base_ + footprint_);
      size_t pm_idx = RoundDownToPageMapIndex(ptr);
      bool free_from_run = false;
      Run* run = NULL;
      {
        MutexLock mu(self, lock_);
        DCHECK(pm_idx < page_map_.size());
        byte page_map_entry = page_map_[pm_idx];
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::BulkFree() : " << std::hex << ptr << ", pm_idx="
                    << std::dec << pm_idx
                    << ", page_map_entry=" << static_cast<int>(page_map_entry);
        }
        if (LIKELY(page_map_entry == kPageMapRun)) {
          free_from_run = true;
          run = reinterpret_cast<Run*>(base_ + pm_idx * kPageSize);
          DCHECK(run->magic_num_ == kMagicNum);
        } else if (LIKELY(page_map_entry == kPageMapRunPart)) {
          free_from_run = true;
          size_t pi = pm_idx;
          DCHECK(page_map_[pi] == kPageMapRun || page_map_[pi] == kPageMapRunPart);
          // Find the beginning of the run.
          while (page_map_[pi] != kPageMapRun) {
            pi--;
            DCHECK(pi < capacity_ / kPageSize);
          }
          DCHECK(page_map_[pi] == kPageMapRun);
          run = reinterpret_cast<Run*>(base_ + pi * kPageSize);
          DCHECK(run->magic_num_ == kMagicNum);
        } else if (page_map_entry == kPageMapLargeObject) {
          FreePages(self, ptr);
        } else {
          LOG(FATAL) << "Unreachable - page map type: " << page_map_entry;
        }
      }
      if (LIKELY(free_from_run)) {
        DCHECK(run != NULL);
        // Set the bit in the bulk free bit map.
        run->MarkBulkFreeBitMap(ptr);
#ifdef HAVE_ANDROID_OS
        if (!run->to_be_bulk_freed_) {
          run->to_be_bulk_freed_ = true;
          runs.push_back(run);
        }
#else
        runs.insert(run);
#endif
      }
    }
  }

  // Now, iterate over the affected runs and update the alloc bit map
  // based on the bulk free bit map (for non-thread-local runs) and
  // union the bulk free bit map into the thread-local free bit map
  // (for thread-local runs.)
#ifdef HAVE_ANDROID_OS
  typedef std::vector<Run*>::iterator It;
#else
  typedef hash_set<Run*, hash_run, eq_run>::iterator It;
#endif
  for (It it = runs.begin(); it != runs.end(); ++it) {
    Run* run = *it;
#ifdef HAVE_ANDROID_OS
    DCHECK(run->to_be_bulk_freed_);
    run->to_be_bulk_freed_ = false;
#endif
    size_t idx = run->size_bracket_idx_;
    MutexLock mu(self, *size_bracket_locks_[idx]);
    if (run->is_thread_local_ != 0) {
      DCHECK_LE(run->size_bracket_idx_, kMaxThreadLocalSizeBracketIdx);
      DCHECK(non_full_runs_[idx].find(run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(run) == full_runs_[idx].end());
      run->UnionBulkFreeBitMapToThreadLocalFreeBitMap();
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::BulkFree() : Freed slot(s) in a thread local run 0x"
                  << std::hex << reinterpret_cast<intptr_t>(run);
      }
      DCHECK_NE(run->is_thread_local_, 0);
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
      hash_set<Run*, hash_run, eq_run>* full_runs =
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
            hash_set<Run*, hash_run, eq_run>::iterator pos = full_runs->find(run);
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
          MutexLock mu(self, lock_);
          FreePages(self, run);
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
}

std::string RosAlloc::DumpPageMap() {
  std::ostringstream stream;
  stream << "RosAlloc PageMap: " << std::endl;
  lock_.AssertHeld(Thread::Current());
  size_t end = page_map_.size();
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
               << " thread_local=" << static_cast<int>(run->is_thread_local_)
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
  DCHECK(base_ <= ptr && ptr < base_ + footprint_);
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
    size_t end = page_map_.size();
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
      DCHECK(pm_idx < capacity_ / kPageSize);
    }
    DCHECK(page_map_[pm_idx] == kPageMapRun);
    Run* run = reinterpret_cast<Run*>(base_ + pm_idx * kPageSize);
    DCHECK(run->magic_num_ == kMagicNum);
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
    DCHECK(page_map_[ToPageMapIndex(last_free_page_run)] == kPageMapEmpty);
    DCHECK_EQ(last_free_page_run->ByteSize(this) % kPageSize, static_cast<size_t>(0));
    DCHECK_EQ(last_free_page_run->End(this), base_ + footprint_);
    free_page_runs_.erase(last_free_page_run);
    size_t decrement = last_free_page_run->ByteSize(this);
    size_t new_footprint = footprint_ - decrement;
    DCHECK_EQ(new_footprint % kPageSize, static_cast<size_t>(0));
    size_t new_num_of_pages = new_footprint / kPageSize;
    DCHECK_GE(page_map_.size(), new_num_of_pages);
    page_map_.resize(new_num_of_pages);
    DCHECK_EQ(page_map_.size(), new_num_of_pages);
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
  size_t pm_end = page_map_.size();
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
        DCHECK(run->magic_num_ == kMagicNum);
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
    capacity_ = new_capacity;
    VLOG(heap) << "new capacity=" << capacity_;
  }
}

void RosAlloc::RevokeThreadLocalRuns(Thread* thread) {
  Thread* self = Thread::Current();
  // Avoid race conditions on the bulk free bit maps with BulkFree() (GC).
  WriterMutexLock wmu(self, bulk_free_lock_);
  for (size_t idx = 0; idx < kNumOfSizeBrackets; idx++) {
    MutexLock mu(self, *size_bracket_locks_[idx]);
    Run* thread_local_run = reinterpret_cast<Run*>(thread->rosalloc_runs_[idx]);
    if (thread_local_run != NULL) {
      DCHECK_EQ(thread_local_run->magic_num_, kMagicNum);
      DCHECK_NE(thread_local_run->is_thread_local_, 0);
      thread->rosalloc_runs_[idx] = NULL;
      // Note the thread local run may not be full here.
      bool dont_care;
      thread_local_run->MergeThreadLocalFreeBitMapToAllocBitMap(&dont_care);
      thread_local_run->is_thread_local_ = 0;
      thread_local_run->MergeBulkFreeBitMapIntoAllocBitMap();
      DCHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(thread_local_run) == full_runs_[idx].end());
      if (thread_local_run->IsFull()) {
        if (kIsDebugBuild) {
          full_runs_[idx].insert(thread_local_run);
          DCHECK(full_runs_[idx].find(thread_local_run) != full_runs_[idx].end());
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::RevokeThreadLocalRuns() : Inserted run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(thread_local_run)
                      << " into full_runs_[" << std::dec << idx << "]";
          }
        }
      } else if (thread_local_run->IsAllFree()) {
        MutexLock mu(self, lock_);
        FreePages(self, thread_local_run);
      } else {
        non_full_runs_[idx].insert(thread_local_run);
        DCHECK(non_full_runs_[idx].find(thread_local_run) != non_full_runs_[idx].end());
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::RevokeThreadLocalRuns() : Inserted run 0x" << std::hex
                    << reinterpret_cast<intptr_t>(thread_local_run)
                    << " into non_full_runs_[" << std::dec << idx << "]";
        }
      }
    }
  }
}

void RosAlloc::RevokeAllThreadLocalRuns() {
  // This is called when a mutator thread won't allocate such as at
  // the Zygote creation time or during the GC pause.
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  for (auto it = thread_list.begin(); it != thread_list.end(); ++it) {
    Thread* t = *it;
    RevokeThreadLocalRuns(t);
  }
}

void RosAlloc::Initialize() {
  // Check the consistency of the number of size brackets.
  DCHECK_EQ(Thread::kRosAllocNumOfSizeBrackets, kNumOfSizeBrackets);
  // bracketSizes.
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    if (i < kNumOfSizeBrackets - 2) {
      bracketSizes[i] = 16 * (i + 1);
    } else if (i == kNumOfSizeBrackets - 2) {
      bracketSizes[i] = 1 * KB;
    } else {
      DCHECK(i == kNumOfSizeBrackets - 1);
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
      DCHECK(i = kNumOfSizeBrackets - 2);
      numOfPages[i] = 16;
    } else {
      DCHECK(i = kNumOfSizeBrackets - 1);
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
    DCHECK(header_size + num_of_slots * bracket_size == run_size);
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
    size_t pm_end = page_map_.size();
    size_t i = 0;
    while (i < pm_end) {
      byte pm = page_map_[i];
      switch (pm) {
        case kPageMapEmpty: {
          // The start of a free page run.
          FreePageRun* fpr = reinterpret_cast<FreePageRun*>(base_ + i * kPageSize);
          DCHECK(fpr->magic_num_ == kMagicNumFree) << "Bad magic number : " << fpr->magic_num_;
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
          CHECK(obj_size > kLargeSizeThreshold)
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
          DCHECK(run->magic_num_ == kMagicNum) << "Bad magic number" << run->magic_num_;
          size_t idx = run->size_bracket_idx_;
          CHECK(idx < kNumOfSizeBrackets) << "Out of range size bracket index : " << idx;
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
          runs.push_back(run);
          i += num_pages;
          CHECK_LE(i, pm_end) << "Page map index " << i << " out of range < " << pm_end
                              << std::endl << DumpPageMap();
          break;
        }
        case kPageMapRunPart:
          LOG(FATAL) << "Unreachable - page map type: " << pm << std::endl << DumpPageMap();
          break;
        default:
          LOG(FATAL) << "Unreachable - page map type: " << pm << std::endl << DumpPageMap();
          break;
      }
    }
  }

  // Call Verify() here for the lock order.
  for (auto& run : runs) {
    run->Verify(self, this);
  }
}

void RosAlloc::Run::Verify(Thread* self, RosAlloc* rosalloc) {
  DCHECK(magic_num_ == kMagicNum) << "Bad magic number : " << Dump();
  size_t idx = size_bracket_idx_;
  CHECK(idx < kNumOfSizeBrackets) << "Out of range size bracket index : " << Dump();
  byte* slot_base = reinterpret_cast<byte*>(this) + headerSizes[idx];
  size_t num_slots = numOfSlots[idx];
  size_t bracket_size = IndexToBracketSize(idx);
  CHECK_EQ(slot_base + num_slots * bracket_size,
           reinterpret_cast<byte*>(this) + numOfPages[idx] * kPageSize)
      << "Mismatch in the end address of the run " << Dump();
  // Check that the bulk free bitmap is clean. It's only used during BulkFree().
  CHECK(IsBulkFreeBitmapClean()) << "The bulk free bit map isn't clean " << Dump();
  // Check the bump index mode, if it's on.
  if (top_slot_idx_ < num_slots) {
    // If the bump index mode is on (top_slot_idx_ < num_slots), then
    // all of the slots after the top index must be free.
    for (size_t i = top_slot_idx_; i < num_slots; ++i) {
      size_t vec_idx = i / 32;
      size_t vec_off = i % 32;
      uint32_t vec = alloc_bit_map_[vec_idx];
      CHECK_EQ((vec & (1 << vec_off)), static_cast<uint32_t>(0))
          << "A slot >= top_slot_idx_ isn't free " << Dump();
    }
  } else {
    CHECK_EQ(top_slot_idx_, num_slots)
        << "If the bump index mode is off, the top index == the number of slots "
        << Dump();
  }
  // Check the thread local runs, the current runs, and the run sets.
  if (is_thread_local_) {
    // If it's a thread local run, then it must be pointed to by an owner thread.
    bool owner_found = false;
    std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
    for (auto it = thread_list.begin(); it != thread_list.end(); ++it) {
      Thread* thread = *it;
      for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
        MutexLock mu(self, *rosalloc->size_bracket_locks_[i]);
        Run* thread_local_run = reinterpret_cast<Run*>(thread->rosalloc_runs_[i]);
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
          hash_set<Run*, hash_run, eq_run>& full_runs = rosalloc->full_runs_[idx];
          CHECK(full_runs.find(this) != full_runs.end())
              << " A full run isn't in the full run set " << Dump();
        }
      }
    }
  }
  // Check each slot.
  size_t num_vec = RoundUp(num_slots, 32) / 32;
  size_t slots = 0;
  for (size_t v = 0; v < num_vec; v++, slots += 32) {
    DCHECK(num_slots >= slots) << "Out of bounds";
    uint32_t vec = alloc_bit_map_[v];
    uint32_t thread_local_free_vec = ThreadLocalFreeBitMap()[v];
    size_t end = std::min(num_slots - slots, static_cast<size_t>(32));
    for (size_t i = 0; i < end; ++i) {
      bool is_allocated = ((vec >> i) & 0x1) != 0;
      // If a thread local run, slots may be marked freed in the
      // thread local free bitmap.
      bool is_thread_local_freed = is_thread_local_ && ((thread_local_free_vec >> i) & 0x1) != 0;
      if (is_allocated && !is_thread_local_freed) {
        byte* slot_addr = slot_base + (slots + i) * bracket_size;
        mirror::Object* obj = reinterpret_cast<mirror::Object*>(slot_addr);
        size_t obj_size = obj->SizeOf();
        CHECK_LE(obj_size, kLargeSizeThreshold)
            << "A run slot contains a large object " << Dump();
        CHECK_EQ(SizeToIndex(obj_size), idx)
            << "A run slot contains an object with wrong size " << Dump();
      }
    }
  }
}

}  // namespace allocator
}  // namespace gc
}  // namespace art
