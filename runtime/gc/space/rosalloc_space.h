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

#ifndef ART_RUNTIME_GC_SPACE_ROSALLOC_SPACE_H_
#define ART_RUNTIME_GC_SPACE_ROSALLOC_SPACE_H_

#include "gc/allocator/rosalloc.h"
#include "malloc_space.h"
#include "space.h"

namespace art {
namespace gc {

namespace collector {
  class MarkSweep;
}  // namespace collector

namespace space {

// An alloc space is a space where objects may be allocated and garbage collected.
class RosAllocSpace : public MallocSpace {
 public:
  // Create a RosAllocSpace with the requested sizes. The requested
  // base address is not guaranteed to be granted, if it is required,
  // the caller should call Begin on the returned space to confirm the
  // request was granted.
  static RosAllocSpace* Create(const std::string& name, size_t initial_size, size_t growth_limit,
                               size_t capacity, byte* requested_begin, bool low_memory_mode);
  static RosAllocSpace* CreateFromMemMap(MemMap* mem_map, const std::string& name,
                                         size_t starting_size, size_t initial_size,
                                         size_t growth_limit, size_t capacity,
                                         bool low_memory_mode);

  virtual mirror::Object* AllocWithGrowth(Thread* self, size_t num_bytes,
                                          size_t* bytes_allocated) LOCKS_EXCLUDED(lock_);
  virtual mirror::Object* Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated);
  virtual size_t AllocationSize(mirror::Object* obj);
  virtual size_t Free(Thread* self, mirror::Object* ptr)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual size_t FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  mirror::Object* AllocNonvirtual(Thread* self, size_t num_bytes, size_t* bytes_allocated);

  size_t AllocationSizeNonvirtual(mirror::Object* obj)
      NO_THREAD_SAFETY_ANALYSIS {
    // TODO: NO_THREAD_SAFETY_ANALYSIS because SizeOf() requires that mutator_lock is held.
    void* obj_ptr = const_cast<void*>(reinterpret_cast<const void*>(obj));
    // obj is a valid object. Use its class in the header to get the size.
    // Don't use verification since the object may be dead if we are sweeping.
    size_t size = obj->SizeOf<kVerifyNone>();
    size_t size_by_size = rosalloc_->UsableSize(size);
    if (kIsDebugBuild) {
      size_t size_by_ptr = rosalloc_->UsableSize(obj_ptr);
      if (size_by_size != size_by_ptr) {
        LOG(INFO) << "Found a bad sized obj of size " << size
                  << " at " << std::hex << reinterpret_cast<intptr_t>(obj_ptr) << std::dec
                  << " size_by_size=" << size_by_size << " size_by_ptr=" << size_by_ptr;
      }
      DCHECK_EQ(size_by_size, size_by_ptr);
    }
    return size_by_size;
  }

  art::gc::allocator::RosAlloc* GetRosAlloc() {
    return rosalloc_;
  }

  size_t Trim();
  void Walk(WalkCallback callback, void* arg) LOCKS_EXCLUDED(lock_);
  size_t GetFootprint();
  size_t GetFootprintLimit();
  void SetFootprintLimit(size_t limit);

  virtual void Clear();
  MallocSpace* CreateInstance(const std::string& name, MemMap* mem_map, void* allocator,
                              byte* begin, byte* end, byte* limit, size_t growth_limit);

  uint64_t GetBytesAllocated();
  uint64_t GetObjectsAllocated();

  void RevokeThreadLocalBuffers(Thread* thread);
  void RevokeAllThreadLocalBuffers();

  // Returns the class of a recently freed object.
  mirror::Class* FindRecentFreedObject(const mirror::Object* obj);

  virtual bool IsRosAllocSpace() const {
    return true;
  }
  virtual RosAllocSpace* AsRosAllocSpace() {
    return this;
  }

  void Verify() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_) {
    rosalloc_->Verify();
  }

 protected:
  RosAllocSpace(const std::string& name, MemMap* mem_map, allocator::RosAlloc* rosalloc,
                byte* begin, byte* end, byte* limit, size_t growth_limit);

 private:
  mirror::Object* AllocWithoutGrowthLocked(Thread* self, size_t num_bytes, size_t* bytes_allocated);

  void* CreateAllocator(void* base, size_t morecore_start, size_t initial_size, bool low_memory_mode) {
    return CreateRosAlloc(base, morecore_start, initial_size, low_memory_mode);
  }
  static allocator::RosAlloc* CreateRosAlloc(void* base, size_t morecore_start, size_t initial_size,
                                             bool low_memory_mode);

  void InspectAllRosAlloc(void (*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
                          void* arg)
      LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_, Locks::thread_list_lock_);

  // Underlying rosalloc.
  art::gc::allocator::RosAlloc* const rosalloc_;

  // A rosalloc pointer used for allocation. Equals to what rosalloc_
  // points to or nullptr after InvalidateAllocator() is called.
  art::gc::allocator::RosAlloc* rosalloc_for_alloc_;

  friend class collector::MarkSweep;

  DISALLOW_COPY_AND_ASSIGN(RosAllocSpace);
};

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_ROSALLOC_SPACE_H_
