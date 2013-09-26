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

#ifndef ART_RUNTIME_GC_HEAP_INL_H_
#define ART_RUNTIME_GC_HEAP_INL_H_

#include "heap.h"

#include "debugger.h"
#include "gc/space/bump_pointer_space-inl.h"
#include "gc/space/dlmalloc_space-inl.h"
#include "gc/space/large_object_space.h"
#include "gc/space/rosalloc_space-inl.h"
#include "object_utils.h"
#include "runtime.h"
#include "thread.h"
#include "thread-inl.h"

namespace art {
namespace gc {

inline mirror::Object* Heap::AllocNonMovableObjectUninstrumented(Thread* self, mirror::Class* c,
                                                                 size_t byte_count) {
  DebugCheckPreconditionsForAllocObject(c, byte_count);
  mirror::Object* obj;
  size_t bytes_allocated;
  AllocationTimer alloc_timer(this, &obj);
  bool large_object_allocation = TryAllocLargeObjectUninstrumented(self, c, byte_count,
                                                                   &obj, &bytes_allocated);
  if (LIKELY(!large_object_allocation)) {
    // Non-large object allocation.
    if (!kUseRosAlloc) {
      DCHECK(non_moving_space_->IsDlMallocSpace());
      obj = AllocateUninstrumented(self, reinterpret_cast<space::DlMallocSpace*>(non_moving_space_),
                                   byte_count, &bytes_allocated);
    } else {
      DCHECK(non_moving_space_->IsRosAllocSpace());
      obj = AllocateUninstrumented(self, reinterpret_cast<space::RosAllocSpace*>(non_moving_space_),
                                   byte_count, &bytes_allocated);
    }
    // Ensure that we did not allocate into a zygote space.
    DCHECK(obj == NULL || !have_zygote_space_ || !FindSpaceFromObject(obj, false)->IsZygoteSpace());
  }
  if (LIKELY(obj != NULL)) {
    obj->SetClass(c);
    // Record allocation after since we want to use the atomic add for the atomic fence to guard
    // the SetClass since we do not want the class to appear NULL in another thread.
    size_t new_num_bytes_allocated = RecordAllocationUninstrumented(bytes_allocated, obj);
    DCHECK(!Dbg::IsAllocTrackingEnabled());
    CheckConcurrentGC(self, new_num_bytes_allocated, obj);
    if (kDesiredHeapVerification > kNoHeapVerification) {
      VerifyObject(obj);
    }
  } else {
    ThrowOutOfMemoryError(self, byte_count, large_object_allocation);
  }
  if (kIsDebugBuild) {
    self->VerifyStack();
  }
  return obj;
}

inline mirror::Object* Heap::AllocMovableObjectUninstrumented(Thread* self, mirror::Class* c,
                                                              size_t byte_count) {
  DebugCheckPreconditionsForAllocObject(c, byte_count);
  mirror::Object* obj;
  AllocationTimer alloc_timer(this, &obj);
  byte_count = (byte_count + 7) & ~7;
  if (UNLIKELY(IsOutOfMemoryOnAllocation(byte_count, false))) {
    CollectGarbageInternal(collector::kGcTypeFull, kGcCauseForAlloc, false);
    if (UNLIKELY(IsOutOfMemoryOnAllocation(byte_count, true))) {
      CollectGarbageInternal(collector::kGcTypeFull, kGcCauseForAlloc, true);
    }
  }
  obj = bump_pointer_space_->AllocNonvirtual(byte_count);
  if (LIKELY(obj != NULL)) {
    obj->SetClass(c);
    DCHECK(!obj->IsClass());
    // Record allocation after since we want to use the atomic add for the atomic fence to guard
    // the SetClass since we do not want the class to appear NULL in another thread.
    num_bytes_allocated_.fetch_add(byte_count);
    DCHECK(!Dbg::IsAllocTrackingEnabled());
    if (kDesiredHeapVerification > kNoHeapVerification) {
      VerifyObject(obj);
    }
  } else {
    ThrowOutOfMemoryError(self, byte_count, false);
  }
  if (kIsDebugBuild) {
    self->VerifyStack();
  }
  return obj;
}

inline size_t Heap::RecordAllocationUninstrumented(size_t size, mirror::Object* obj) {
  DCHECK(obj != NULL);
  DCHECK_GT(size, 0u);
  size_t old_num_bytes_allocated = static_cast<size_t>(num_bytes_allocated_.fetch_add(size));

  DCHECK(!Runtime::Current()->HasStatsEnabled());

  // This is safe to do since the GC will never free objects which are neither in the allocation
  // stack or the live bitmap.
  while (!allocation_stack_->AtomicPushBack(obj)) {
    CollectGarbageInternal(collector::kGcTypeSticky, kGcCauseForAlloc, false);
  }

  return old_num_bytes_allocated + size;
}

inline mirror::Object* Heap::TryToAllocateUninstrumented(Thread* self, space::AllocSpace* space, size_t alloc_size,
                                                         bool grow, size_t* bytes_allocated) {
  if (UNLIKELY(IsOutOfMemoryOnAllocation(alloc_size, grow))) {
    return NULL;
  }
  DCHECK(!running_on_valgrind_);
  return space->Alloc(self, alloc_size, bytes_allocated);
}

// DlMallocSpace-specific version.
inline mirror::Object* Heap::TryToAllocateUninstrumented(Thread* self, space::DlMallocSpace* space, size_t alloc_size,
                                                         bool grow, size_t* bytes_allocated) {
  if (UNLIKELY(IsOutOfMemoryOnAllocation(alloc_size, grow))) {
    return NULL;
  }
  DCHECK(!running_on_valgrind_);
  return space->AllocNonvirtual(self, alloc_size, bytes_allocated);
}

// RosAllocSpace-specific version.
inline mirror::Object* Heap::TryToAllocateUninstrumented(Thread* self, space::RosAllocSpace* space, size_t alloc_size,
                                                         bool grow, size_t* bytes_allocated) {
  if (UNLIKELY(IsOutOfMemoryOnAllocation(alloc_size, grow))) {
    return NULL;
  }
  DCHECK(!running_on_valgrind_);
  return space->AllocNonvirtual(self, alloc_size, bytes_allocated);
}

template <class T>
inline mirror::Object* Heap::AllocateUninstrumented(Thread* self, T* space, size_t alloc_size,
                                                    size_t* bytes_allocated) {
  // Since allocation can cause a GC which will need to SuspendAll, make sure all allocations are
  // done in the runnable state where suspension is expected.
  DCHECK_EQ(self->GetState(), kRunnable);
  self->AssertThreadSuspensionIsAllowable();

  mirror::Object* ptr = TryToAllocateUninstrumented(self, space, alloc_size, false, bytes_allocated);
  if (LIKELY(ptr != NULL)) {
    return ptr;
  }
  return AllocateInternalWithGc(self, space, alloc_size, bytes_allocated);
}

inline bool Heap::TryAllocLargeObjectUninstrumented(Thread* self, mirror::Class* c, size_t byte_count,
                                                    mirror::Object** obj_ptr, size_t* bytes_allocated) {
  bool large_object_allocation = ShouldAllocLargeObject(c, byte_count);
  if (UNLIKELY(large_object_allocation)) {
    mirror::Object* obj = AllocateUninstrumented(self, large_object_space_, byte_count, bytes_allocated);
    // Make sure that our large object didn't get placed anywhere within the space interval or else
    // it breaks the immune range.
    DCHECK(obj == NULL ||
           reinterpret_cast<byte*>(obj) < continuous_spaces_.front()->Begin() ||
           reinterpret_cast<byte*>(obj) >= continuous_spaces_.back()->End());
    *obj_ptr = obj;
  }
  return large_object_allocation;
}

inline void Heap::DebugCheckPreconditionsForAllocObject(mirror::Class* c, size_t byte_count) {
  DCHECK(c == NULL || (c->IsClassClass() && byte_count >= sizeof(mirror::Class)) ||
         (c->IsVariableSize() || c->GetObjectSize() == byte_count) ||
         strlen(ClassHelper(c).GetDescriptor()) == 0);
  DCHECK_GE(byte_count, sizeof(mirror::Object));
}

inline Heap::AllocationTimer::AllocationTimer(Heap* heap, mirror::Object** allocated_obj_ptr)
    : heap_(heap), allocated_obj_ptr_(allocated_obj_ptr) {
  if (kMeasureAllocationTime) {
    allocation_start_time_ = NanoTime() / kTimeAdjust;
  }
}

inline Heap::AllocationTimer::~AllocationTimer() {
  if (kMeasureAllocationTime) {
    mirror::Object* allocated_obj = *allocated_obj_ptr_;
    // Only if the allocation succeeded, record the time.
    if (allocated_obj != NULL) {
      uint64_t allocation_end_time = NanoTime() / kTimeAdjust;
      heap_->total_allocation_time_.fetch_add(allocation_end_time - allocation_start_time_);
    }
  }
};

inline bool Heap::ShouldAllocLargeObject(mirror::Class* c, size_t byte_count) {
  // We need to have a zygote space or else our newly allocated large object can end up in the
  // Zygote resulting in it being prematurely freed.
  // We can only do this for primitive objects since large objects will not be within the card table
  // range. This also means that we rely on SetClass not dirtying the object's card.
  return byte_count >= kLargeObjectThreshold && have_zygote_space_ && c->IsPrimitiveArray();
}

inline bool Heap::IsOutOfMemoryOnAllocation(size_t alloc_size, bool grow) {
  size_t new_footprint = num_bytes_allocated_ + alloc_size;
  if (UNLIKELY(new_footprint > max_allowed_footprint_)) {
    if (UNLIKELY(new_footprint > growth_limit_)) {
      return true;
    }
    if (!concurrent_gc_) {
      if (!grow) {
        return true;
      } else {
        max_allowed_footprint_ = new_footprint;
      }
    }
  }
  return false;
}

inline void Heap::CheckConcurrentGC(Thread* self, size_t new_num_bytes_allocated, mirror::Object* obj) {
  if (UNLIKELY(new_num_bytes_allocated >= concurrent_start_bytes_)) {
    // The SirtRef is necessary since the calls in RequestConcurrentGC are a safepoint.
    SirtRef<mirror::Object> ref(self, obj);
    RequestConcurrentGC(self);
  }
}

}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_HEAP_INL_H_
