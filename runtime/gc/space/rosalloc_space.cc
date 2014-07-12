
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

#include "rosalloc_space-inl.h"

#include "gc/accounting/card_table.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "thread.h"
#include "thread_list.h"
#include "utils.h"
#include "valgrind_malloc_space-inl.h"

namespace art {
namespace gc {
namespace space {

static constexpr bool kPrefetchDuringRosAllocFreeList = false;
static constexpr size_t kPrefetchLookAhead = 8;
// Use this only for verification, it is not safe to use since the class of the object may have
// been freed.
static constexpr bool kVerifyFreedBytes = false;

// TODO: Fix
// template class ValgrindMallocSpace<RosAllocSpace, allocator::RosAlloc*>;

RosAllocSpace::RosAllocSpace(const std::string& name, MemMap* mem_map,
                             art::gc::allocator::RosAlloc* rosalloc, byte* begin, byte* end,
                             byte* limit, size_t growth_limit, bool can_move_objects,
                             size_t starting_size, size_t initial_size, bool low_memory_mode)
    : MallocSpace(name, mem_map, begin, end, limit, growth_limit, true, can_move_objects,
                  starting_size, initial_size),
      rosalloc_(rosalloc), low_memory_mode_(low_memory_mode) {
  CHECK(rosalloc != nullptr);
}

RosAllocSpace* RosAllocSpace::CreateFromMemMap(MemMap* mem_map, const std::string& name,
                                               size_t starting_size, size_t initial_size,
                                               size_t growth_limit, size_t capacity,
                                               bool low_memory_mode, bool can_move_objects) {
  DCHECK(mem_map != nullptr);
  allocator::RosAlloc* rosalloc = CreateRosAlloc(mem_map->Begin(), starting_size, initial_size,
                                                 capacity, low_memory_mode);
  if (rosalloc == NULL) {
    LOG(ERROR) << "Failed to initialize rosalloc for alloc space (" << name << ")";
    return NULL;
  }

  // Protect memory beyond the starting size. MoreCore will add r/w permissions when necessory
  byte* end = mem_map->Begin() + starting_size;
  if (capacity - starting_size > 0) {
    CHECK_MEMORY_CALL(mprotect, (end, capacity - starting_size, PROT_NONE), name);
  }

  // Everything is set so record in immutable structure and leave
  byte* begin = mem_map->Begin();
  // TODO: Fix RosAllocSpace to support valgrind. There is currently some issues with
  // AllocationSize caused by redzones. b/12944686
  if (false && Runtime::Current()->GetHeap()->RunningOnValgrind()) {
    LOG(FATAL) << "Unimplemented";
  } else {
    return new RosAllocSpace(name, mem_map, rosalloc, begin, end, begin + capacity, growth_limit,
                             can_move_objects, starting_size, initial_size, low_memory_mode);
  }
}

RosAllocSpace::~RosAllocSpace() {
  delete rosalloc_;
}

RosAllocSpace* RosAllocSpace::Create(const std::string& name, size_t initial_size,
                                     size_t growth_limit, size_t capacity, byte* requested_begin,
                                     bool low_memory_mode, bool can_move_objects) {
  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    VLOG(startup) << "RosAllocSpace::Create entering " << name
                  << " initial_size=" << PrettySize(initial_size)
                  << " growth_limit=" << PrettySize(growth_limit)
                  << " capacity=" << PrettySize(capacity)
                  << " requested_begin=" << reinterpret_cast<void*>(requested_begin);
  }

  // Memory we promise to rosalloc before it asks for morecore.
  // Note: making this value large means that large allocations are unlikely to succeed as rosalloc
  // will ask for this memory from sys_alloc which will fail as the footprint (this value plus the
  // size of the large allocation) will be greater than the footprint limit.
  size_t starting_size = Heap::kDefaultStartingSize;
  MemMap* mem_map = CreateMemMap(name, starting_size, &initial_size, &growth_limit, &capacity,
                                 requested_begin);
  if (mem_map == NULL) {
    LOG(ERROR) << "Failed to create mem map for alloc space (" << name << ") of size "
               << PrettySize(capacity);
    return NULL;
  }

  RosAllocSpace* space = CreateFromMemMap(mem_map, name, starting_size, initial_size,
                                          growth_limit, capacity, low_memory_mode,
                                          can_move_objects);
  // We start out with only the initial size possibly containing objects.
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "RosAllocSpace::Create exiting (" << PrettyDuration(NanoTime() - start_time)
        << " ) " << *space;
  }
  return space;
}

allocator::RosAlloc* RosAllocSpace::CreateRosAlloc(void* begin, size_t morecore_start,
                                                   size_t initial_size,
                                                   size_t maximum_size, bool low_memory_mode) {
  // clear errno to allow PLOG on error
  errno = 0;
  // create rosalloc using our backing storage starting at begin and
  // with a footprint of morecore_start. When morecore_start bytes of
  // memory is exhaused morecore will be called.
  allocator::RosAlloc* rosalloc = new art::gc::allocator::RosAlloc(
      begin, morecore_start, maximum_size,
      low_memory_mode ?
          art::gc::allocator::RosAlloc::kPageReleaseModeAll :
          art::gc::allocator::RosAlloc::kPageReleaseModeSizeAndEnd);
  if (rosalloc != NULL) {
    rosalloc->SetFootprintLimit(initial_size);
  } else {
    PLOG(ERROR) << "RosAlloc::Create failed";
  }
  return rosalloc;
}

mirror::Object* RosAllocSpace::AllocWithGrowth(Thread* self, size_t num_bytes,
                                               size_t* bytes_allocated, size_t* usable_size) {
  mirror::Object* result;
  {
    MutexLock mu(self, lock_);
    // Grow as much as possible within the space.
    size_t max_allowed = Capacity();
    rosalloc_->SetFootprintLimit(max_allowed);
    // Try the allocation.
    result = AllocCommon(self, num_bytes, bytes_allocated, usable_size);
    // Shrink back down as small as possible.
    size_t footprint = rosalloc_->Footprint();
    rosalloc_->SetFootprintLimit(footprint);
  }
  // Note RosAlloc zeroes memory internally.
  // Return the new allocation or NULL.
  CHECK(!kDebugSpaces || result == nullptr || Contains(result));
  return result;
}

MallocSpace* RosAllocSpace::CreateInstance(const std::string& name, MemMap* mem_map, void* allocator,
                                           byte* begin, byte* end, byte* limit, size_t growth_limit,
                                           bool can_move_objects) {
  return new RosAllocSpace(name, mem_map, reinterpret_cast<allocator::RosAlloc*>(allocator),
                           begin, end, limit, growth_limit, can_move_objects, starting_size_,
                           initial_size_, low_memory_mode_);
}

size_t RosAllocSpace::Free(Thread* self, mirror::Object* ptr) {
  if (kDebugSpaces) {
    CHECK(ptr != NULL);
    CHECK(Contains(ptr)) << "Free (" << ptr << ") not in bounds of heap " << *this;
  }
  if (kRecentFreeCount > 0) {
    MutexLock mu(self, lock_);
    RegisterRecentFree(ptr);
  }
  return rosalloc_->Free(self, ptr);
}

size_t RosAllocSpace::FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs) {
  DCHECK(ptrs != nullptr);

  size_t verify_bytes = 0;
  for (size_t i = 0; i < num_ptrs; i++) {
    if (kPrefetchDuringRosAllocFreeList && i + kPrefetchLookAhead < num_ptrs) {
      __builtin_prefetch(reinterpret_cast<char*>(ptrs[i + kPrefetchLookAhead]));
    }
    if (kVerifyFreedBytes) {
      verify_bytes += AllocationSizeNonvirtual(ptrs[i], nullptr);
    }
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
        size_t size = rosalloc_->UsableSize(ptrs[i]);
        memset(ptrs[i], 0xEF, size);
      }
    }
    CHECK_EQ(num_broken_ptrs, 0u);
  }

  const size_t bytes_freed = rosalloc_->BulkFree(self, reinterpret_cast<void**>(ptrs), num_ptrs);
  if (kVerifyFreedBytes) {
    CHECK_EQ(verify_bytes, bytes_freed);
  }
  return bytes_freed;
}

// Callback from rosalloc when it needs to increase the footprint
extern "C" void* art_heap_rosalloc_morecore(allocator::RosAlloc* rosalloc, intptr_t increment) {
  Heap* heap = Runtime::Current()->GetHeap();
  RosAllocSpace* rosalloc_space = heap->GetRosAllocSpace(rosalloc);
  DCHECK(rosalloc_space != nullptr);
  DCHECK_EQ(rosalloc_space->GetRosAlloc(), rosalloc);
  return rosalloc_space->MoreCore(increment);
}

size_t RosAllocSpace::Trim() {
  VLOG(heap) << "RosAllocSpace::Trim() ";
  {
    MutexLock mu(Thread::Current(), lock_);
    // Trim to release memory at the end of the space.
    rosalloc_->Trim();
  }
  // Attempt to release pages if it does not release all empty pages.
  if (!rosalloc_->DoesReleaseAllPages()) {
    return rosalloc_->ReleasePages();
  }
  return 0;
}

void RosAllocSpace::Walk(void(*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
                         void* arg) {
  InspectAllRosAlloc(callback, arg, true);
}

size_t RosAllocSpace::GetFootprint() {
  MutexLock mu(Thread::Current(), lock_);
  return rosalloc_->Footprint();
}

size_t RosAllocSpace::GetFootprintLimit() {
  MutexLock mu(Thread::Current(), lock_);
  return rosalloc_->FootprintLimit();
}

void RosAllocSpace::SetFootprintLimit(size_t new_size) {
  MutexLock mu(Thread::Current(), lock_);
  VLOG(heap) << "RosAllocSpace::SetFootprintLimit " << PrettySize(new_size);
  // Compare against the actual footprint, rather than the Size(), because the heap may not have
  // grown all the way to the allowed size yet.
  size_t current_space_size = rosalloc_->Footprint();
  if (new_size < current_space_size) {
    // Don't let the space grow any more.
    new_size = current_space_size;
  }
  rosalloc_->SetFootprintLimit(new_size);
}

uint64_t RosAllocSpace::GetBytesAllocated() {
  size_t bytes_allocated = 0;
  InspectAllRosAlloc(art::gc::allocator::RosAlloc::BytesAllocatedCallback, &bytes_allocated, false);
  return bytes_allocated;
}

uint64_t RosAllocSpace::GetObjectsAllocated() {
  size_t objects_allocated = 0;
  InspectAllRosAlloc(art::gc::allocator::RosAlloc::ObjectsAllocatedCallback, &objects_allocated, false);
  return objects_allocated;
}

void RosAllocSpace::InspectAllRosAllocWithSuspendAll(
    void (*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
    void* arg, bool do_null_callback_at_end) NO_THREAD_SAFETY_ANALYSIS {
  // TODO: NO_THREAD_SAFETY_ANALYSIS.
  Thread* self = Thread::Current();
  ThreadList* tl = Runtime::Current()->GetThreadList();
  tl->SuspendAll();
  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    MutexLock mu2(self, *Locks::thread_list_lock_);
    rosalloc_->InspectAll(callback, arg);
    if (do_null_callback_at_end) {
      callback(NULL, NULL, 0, arg);  // Indicate end of a space.
    }
  }
  tl->ResumeAll();
}

void RosAllocSpace::InspectAllRosAlloc(void (*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
                                       void* arg, bool do_null_callback_at_end) NO_THREAD_SAFETY_ANALYSIS {
  // TODO: NO_THREAD_SAFETY_ANALYSIS.
  Thread* self = Thread::Current();
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // The mutators are already suspended. For example, a call path
    // from SignalCatcher::HandleSigQuit().
    rosalloc_->InspectAll(callback, arg);
    if (do_null_callback_at_end) {
      callback(NULL, NULL, 0, arg);  // Indicate end of a space.
    }
  } else if (Locks::mutator_lock_->IsSharedHeld(self)) {
    // The mutators are not suspended yet and we have a shared access
    // to the mutator lock. Temporarily release the shared access by
    // transitioning to the suspend state, and suspend the mutators.
    self->TransitionFromRunnableToSuspended(kSuspended);
    InspectAllRosAllocWithSuspendAll(callback, arg, do_null_callback_at_end);
    self->TransitionFromSuspendedToRunnable();
    Locks::mutator_lock_->AssertSharedHeld(self);
  } else {
    // The mutators are not suspended yet. Suspend the mutators.
    InspectAllRosAllocWithSuspendAll(callback, arg, do_null_callback_at_end);
  }
}

void RosAllocSpace::RevokeThreadLocalBuffers(Thread* thread) {
  rosalloc_->RevokeThreadLocalRuns(thread);
}

void RosAllocSpace::RevokeAllThreadLocalBuffers() {
  rosalloc_->RevokeAllThreadLocalRuns();
}

void RosAllocSpace::AssertAllThreadLocalBuffersAreRevoked() {
  if (kIsDebugBuild) {
    rosalloc_->AssertAllThreadLocalRunsAreRevoked();
  }
}

void RosAllocSpace::Clear() {
  size_t footprint_limit = GetFootprintLimit();
  madvise(GetMemMap()->Begin(), GetMemMap()->Size(), MADV_DONTNEED);
  live_bitmap_->Clear();
  mark_bitmap_->Clear();
  SetEnd(begin_ + starting_size_);
  delete rosalloc_;
  rosalloc_ = CreateRosAlloc(mem_map_->Begin(), starting_size_, initial_size_, Capacity(),
                             low_memory_mode_);
  SetFootprintLimit(footprint_limit);
}

}  // namespace space
}  // namespace gc
}  // namespace art
