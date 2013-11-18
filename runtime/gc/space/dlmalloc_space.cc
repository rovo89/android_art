/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "dlmalloc_space.h"

#include "dlmalloc_space-inl.h"
#include "gc/accounting/card_table.h"
#include "gc/heap.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "thread.h"
#include "thread_list.h"
#include "utils.h"

#include <valgrind.h>
#include <memcheck/memcheck.h>

namespace art {
namespace gc {
namespace space {

static const bool kPrefetchDuringDlMallocFreeList = true;

// Number of bytes to use as a red zone (rdz). A red zone of this size will be placed before and
// after each allocation. 8 bytes provides long/double alignment.
const size_t kValgrindRedZoneBytes = 8;

// A specialization of DlMallocSpace that provides information to valgrind wrt allocations.
class ValgrindDlMallocSpace : public DlMallocSpace {
 public:
  virtual mirror::Object* AllocWithGrowth(Thread* self, size_t num_bytes, size_t* bytes_allocated) {
    void* obj_with_rdz = DlMallocSpace::AllocWithGrowth(self, num_bytes + 2 * kValgrindRedZoneBytes,
                                                        bytes_allocated);
    if (obj_with_rdz == NULL) {
      return NULL;
    }
    mirror::Object* result = reinterpret_cast<mirror::Object*>(
        reinterpret_cast<byte*>(obj_with_rdz) + kValgrindRedZoneBytes);
    // Make redzones as no access.
    VALGRIND_MAKE_MEM_NOACCESS(obj_with_rdz, kValgrindRedZoneBytes);
    VALGRIND_MAKE_MEM_NOACCESS(reinterpret_cast<byte*>(result) + num_bytes, kValgrindRedZoneBytes);
    return result;
  }

  virtual mirror::Object* Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated) {
    void* obj_with_rdz = DlMallocSpace::Alloc(self, num_bytes + 2 * kValgrindRedZoneBytes,
                                              bytes_allocated);
    if (obj_with_rdz == NULL) {
     return NULL;
    }
    mirror::Object* result = reinterpret_cast<mirror::Object*>(
        reinterpret_cast<byte*>(obj_with_rdz) + kValgrindRedZoneBytes);
    // Make redzones as no access.
    VALGRIND_MAKE_MEM_NOACCESS(obj_with_rdz, kValgrindRedZoneBytes);
    VALGRIND_MAKE_MEM_NOACCESS(reinterpret_cast<byte*>(result) + num_bytes, kValgrindRedZoneBytes);
    return result;
  }

  virtual size_t AllocationSize(const mirror::Object* obj) {
    size_t result = DlMallocSpace::AllocationSize(reinterpret_cast<const mirror::Object*>(
        reinterpret_cast<const byte*>(obj) - kValgrindRedZoneBytes));
    return result - 2 * kValgrindRedZoneBytes;
  }

  virtual size_t Free(Thread* self, mirror::Object* ptr) {
    void* obj_after_rdz = reinterpret_cast<void*>(ptr);
    void* obj_with_rdz = reinterpret_cast<byte*>(obj_after_rdz) - kValgrindRedZoneBytes;
    // Make redzones undefined.
    size_t allocation_size = DlMallocSpace::AllocationSize(
        reinterpret_cast<mirror::Object*>(obj_with_rdz));
    VALGRIND_MAKE_MEM_UNDEFINED(obj_with_rdz, allocation_size);
    size_t freed = DlMallocSpace::Free(self, reinterpret_cast<mirror::Object*>(obj_with_rdz));
    return freed - 2 * kValgrindRedZoneBytes;
  }

  virtual size_t FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs) {
    size_t freed = 0;
    for (size_t i = 0; i < num_ptrs; i++) {
      freed += Free(self, ptrs[i]);
    }
    return freed;
  }

  ValgrindDlMallocSpace(const std::string& name, MemMap* mem_map, void* mspace, byte* begin,
                        byte* end, byte* limit, size_t growth_limit, size_t initial_size) :
      DlMallocSpace(name, mem_map, mspace, begin, end, limit, growth_limit) {
    VALGRIND_MAKE_MEM_UNDEFINED(mem_map->Begin() + initial_size, mem_map->Size() - initial_size);
  }

  virtual ~ValgrindDlMallocSpace() {
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ValgrindDlMallocSpace);
};

DlMallocSpace::DlMallocSpace(const std::string& name, MemMap* mem_map, void* mspace, byte* begin,
                             byte* end, byte* limit, size_t growth_limit)
    : MallocSpace(name, mem_map, begin, end, limit, growth_limit),
      total_bytes_freed_(0), total_objects_freed_(0), mspace_(mspace), mspace_for_alloc_(mspace) {
  CHECK(mspace != NULL);
}

DlMallocSpace* DlMallocSpace::Create(const std::string& name, size_t initial_size, size_t growth_limit,
                                     size_t capacity, byte* requested_begin) {
  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    VLOG(startup) << "DlMallocSpace::Create entering " << name
                  << " initial_size=" << PrettySize(initial_size)
                  << " growth_limit=" << PrettySize(growth_limit)
                  << " capacity=" << PrettySize(capacity)
                  << " requested_begin=" << reinterpret_cast<void*>(requested_begin);
  }

  // Memory we promise to dlmalloc before it asks for morecore.
  // Note: making this value large means that large allocations are unlikely to succeed as dlmalloc
  // will ask for this memory from sys_alloc which will fail as the footprint (this value plus the
  // size of the large allocation) will be greater than the footprint limit.
  size_t starting_size = kPageSize;
  MemMap* mem_map = CreateMemMap(name, starting_size, &initial_size, &growth_limit, &capacity,
                                 requested_begin);
  if (mem_map == NULL) {
    LOG(ERROR) << "Failed to create mem map for alloc space (" << name << ") of size "
               << PrettySize(capacity);
    return NULL;
  }
  void* mspace = CreateMspace(mem_map->Begin(), starting_size, initial_size);
  if (mspace == NULL) {
    LOG(ERROR) << "Failed to initialize mspace for alloc space (" << name << ")";
    return NULL;
  }

  // Protect memory beyond the initial size.
  byte* end = mem_map->Begin() + starting_size;
  if (capacity - initial_size > 0) {
    CHECK_MEMORY_CALL(mprotect, (end, capacity - initial_size, PROT_NONE), name);
  }

  // Everything is set so record in immutable structure and leave
  DlMallocSpace* space;
  byte* begin = mem_map->Begin();
  if (RUNNING_ON_VALGRIND > 0) {
    space = new ValgrindDlMallocSpace(name, mem_map, mspace, begin, end, begin + capacity,
                                      growth_limit, initial_size);
  } else {
    space = new DlMallocSpace(name, mem_map, mspace, begin, end, begin + capacity, growth_limit);
  }
  // We start out with only the initial size possibly containing objects.
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "DlMallocSpace::Create exiting (" << PrettyDuration(NanoTime() - start_time)
        << " ) " << *space;
  }
  return space;
}

void* DlMallocSpace::CreateMspace(void* begin, size_t morecore_start, size_t initial_size) {
  // clear errno to allow PLOG on error
  errno = 0;
  // create mspace using our backing storage starting at begin and with a footprint of
  // morecore_start. Don't use an internal dlmalloc lock (as we already hold heap lock). When
  // morecore_start bytes of memory is exhaused morecore will be called.
  void* msp = create_mspace_with_base(begin, morecore_start, false /*locked*/);
  if (msp != NULL) {
    // Do not allow morecore requests to succeed beyond the initial size of the heap
    mspace_set_footprint_limit(msp, initial_size);
  } else {
    PLOG(ERROR) << "create_mspace_with_base failed";
  }
  return msp;
}

mirror::Object* DlMallocSpace::Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated) {
  return AllocNonvirtual(self, num_bytes, bytes_allocated);
}

mirror::Object* DlMallocSpace::AllocWithGrowth(Thread* self, size_t num_bytes, size_t* bytes_allocated) {
  mirror::Object* result;
  {
    MutexLock mu(self, lock_);
    // Grow as much as possible within the space.
    size_t max_allowed = Capacity();
    mspace_set_footprint_limit(mspace_, max_allowed);
    // Try the allocation.
    result = AllocWithoutGrowthLocked(self, num_bytes, bytes_allocated);
    // Shrink back down as small as possible.
    size_t footprint = mspace_footprint(mspace_);
    mspace_set_footprint_limit(mspace_, footprint);
  }
  if (result != NULL) {
    // Zero freshly allocated memory, done while not holding the space's lock.
    memset(result, 0, num_bytes);
  }
  // Return the new allocation or NULL.
  CHECK(!kDebugSpaces || result == NULL || Contains(result));
  return result;
}

MallocSpace* DlMallocSpace::CreateInstance(const std::string& name, MemMap* mem_map, void* allocator, byte* begin, byte* end,
                                           byte* limit, size_t growth_limit) {
  return new DlMallocSpace(name, mem_map, allocator, begin, end, limit, growth_limit);
}

size_t DlMallocSpace::Free(Thread* self, mirror::Object* ptr) {
  MutexLock mu(self, lock_);
  if (kDebugSpaces) {
    CHECK(ptr != NULL);
    CHECK(Contains(ptr)) << "Free (" << ptr << ") not in bounds of heap " << *this;
  }
  const size_t bytes_freed = InternalAllocationSize(ptr);
  total_bytes_freed_ += bytes_freed;
  ++total_objects_freed_;
  if (kRecentFreeCount > 0) {
    RegisterRecentFree(ptr);
  }
  mspace_free(mspace_, ptr);
  return bytes_freed;
}

size_t DlMallocSpace::FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs) {
  DCHECK(ptrs != NULL);

  // Don't need the lock to calculate the size of the freed pointers.
  size_t bytes_freed = 0;
  for (size_t i = 0; i < num_ptrs; i++) {
    mirror::Object* ptr = ptrs[i];
    const size_t look_ahead = 8;
    if (kPrefetchDuringDlMallocFreeList && i + look_ahead < num_ptrs) {
      // The head of chunk for the allocation is sizeof(size_t) behind the allocation.
      __builtin_prefetch(reinterpret_cast<char*>(ptrs[i + look_ahead]) - sizeof(size_t));
    }
    bytes_freed += InternalAllocationSize(ptr);
  }

  if (kRecentFreeCount > 0) {
    MutexLock mu(self, lock_);
    for (size_t i = 0; i < num_ptrs; i++) {
      RegisterRecentFree(ptrs[i]);
    }
  }

  if (kDebugSpaces) {
    size_t num_broken_ptrs = 0;
    for (size_t i = 0; i < num_ptrs; i++) {
      if (!Contains(ptrs[i])) {
        num_broken_ptrs++;
        LOG(ERROR) << "FreeList[" << i << "] (" << ptrs[i] << ") not in bounds of heap " << *this;
      } else {
        size_t size = mspace_usable_size(ptrs[i]);
        memset(ptrs[i], 0xEF, size);
      }
    }
    CHECK_EQ(num_broken_ptrs, 0u);
  }

  {
    MutexLock mu(self, lock_);
    total_bytes_freed_ += bytes_freed;
    total_objects_freed_ += num_ptrs;
    mspace_bulk_free(mspace_, reinterpret_cast<void**>(ptrs), num_ptrs);
    return bytes_freed;
  }
}

// Callback from dlmalloc when it needs to increase the footprint
extern "C" void* art_heap_morecore(void* mspace, intptr_t increment) {
  Heap* heap = Runtime::Current()->GetHeap();
  DCHECK(heap->GetNonMovingSpace()->IsDlMallocSpace());
  DCHECK_EQ(heap->GetNonMovingSpace()->AsDlMallocSpace()->GetMspace(), mspace);
  return heap->GetNonMovingSpace()->MoreCore(increment);
}

// Virtual functions can't get inlined.
inline size_t DlMallocSpace::InternalAllocationSize(const mirror::Object* obj) {
  return AllocationSizeNonvirtual(obj);
}

size_t DlMallocSpace::AllocationSize(const mirror::Object* obj) {
  return InternalAllocationSize(obj);
}

size_t DlMallocSpace::Trim() {
  MutexLock mu(Thread::Current(), lock_);
  // Trim to release memory at the end of the space.
  mspace_trim(mspace_, 0);
  // Visit space looking for page-sized holes to advise the kernel we don't need.
  size_t reclaimed = 0;
  mspace_inspect_all(mspace_, DlmallocMadviseCallback, &reclaimed);
  return reclaimed;
}

void DlMallocSpace::Walk(void(*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
                      void* arg) {
  MutexLock mu(Thread::Current(), lock_);
  mspace_inspect_all(mspace_, callback, arg);
  callback(NULL, NULL, 0, arg);  // Indicate end of a space.
}

size_t DlMallocSpace::GetFootprint() {
  MutexLock mu(Thread::Current(), lock_);
  return mspace_footprint(mspace_);
}

size_t DlMallocSpace::GetFootprintLimit() {
  MutexLock mu(Thread::Current(), lock_);
  return mspace_footprint_limit(mspace_);
}

void DlMallocSpace::SetFootprintLimit(size_t new_size) {
  MutexLock mu(Thread::Current(), lock_);
  VLOG(heap) << "DlMallocSpace::SetFootprintLimit " << PrettySize(new_size);
  // Compare against the actual footprint, rather than the Size(), because the heap may not have
  // grown all the way to the allowed size yet.
  size_t current_space_size = mspace_footprint(mspace_);
  if (new_size < current_space_size) {
    // Don't let the space grow any more.
    new_size = current_space_size;
  }
  mspace_set_footprint_limit(mspace_, new_size);
}

uint64_t DlMallocSpace::GetBytesAllocated() {
  MutexLock mu(Thread::Current(), lock_);
  size_t bytes_allocated = 0;
  mspace_inspect_all(mspace_, DlmallocBytesAllocatedCallback, &bytes_allocated);
  return bytes_allocated;
}

uint64_t DlMallocSpace::GetObjectsAllocated() {
  MutexLock mu(Thread::Current(), lock_);
  size_t objects_allocated = 0;
  mspace_inspect_all(mspace_, DlmallocObjectsAllocatedCallback, &objects_allocated);
  return objects_allocated;
}

#ifndef NDEBUG
void DlMallocSpace::CheckMoreCoreForPrecondition() {
  lock_.AssertHeld(Thread::Current());
}
#endif

}  // namespace space
}  // namespace gc
}  // namespace art
