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

#ifndef ART_RUNTIME_GC_SPACE_DLMALLOC_SPACE_H_
#define ART_RUNTIME_GC_SPACE_DLMALLOC_SPACE_H_

#include "gc/allocator/dlmalloc.h"
#include "malloc_space.h"
#include "space.h"

namespace art {
namespace gc {

namespace collector {
  class MarkSweep;
}  // namespace collector

namespace space {

// An alloc space is a space where objects may be allocated and garbage collected.
class DlMallocSpace : public MallocSpace {
 public:
  // Create a DlMallocSpace with the requested sizes. The requested
  // base address is not guaranteed to be granted, if it is required,
  // the caller should call Begin on the returned space to confirm the
  // request was granted.
  static DlMallocSpace* Create(const std::string& name, size_t initial_size, size_t growth_limit,
                               size_t capacity, byte* requested_begin);

  virtual mirror::Object* AllocWithGrowth(Thread* self, size_t num_bytes,
                                          size_t* bytes_allocated) LOCKS_EXCLUDED(lock_);
  virtual mirror::Object* Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated);
  virtual size_t AllocationSize(const mirror::Object* obj);
  virtual size_t Free(Thread* self, mirror::Object* ptr);
  virtual size_t FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs);

  mirror::Object* AllocNonvirtual(Thread* self, size_t num_bytes, size_t* bytes_allocated);

  size_t AllocationSizeNonvirtual(const mirror::Object* obj) {
    void* obj_ptr = const_cast<void*>(reinterpret_cast<const void*>(obj));
    return mspace_usable_size(obj_ptr) + kChunkOverhead;
  }

#ifndef NDEBUG
  // Override only in the debug build.
  void CheckMoreCoreForPrecondition();
#endif

  void* GetMspace() const {
    return mspace_;
  }

  size_t Trim();

  // Perform a mspace_inspect_all which calls back for each allocation chunk. The chunk may not be
  // in use, indicated by num_bytes equaling zero.
  void Walk(WalkCallback callback, void* arg) LOCKS_EXCLUDED(lock_);

  // Returns the number of bytes that the space has currently obtained from the system. This is
  // greater or equal to the amount of live data in the space.
  size_t GetFootprint();

  // Returns the number of bytes that the heap is allowed to obtain from the system via MoreCore.
  size_t GetFootprintLimit();

  // Set the maximum number of bytes that the heap is allowed to obtain from the system via
  // MoreCore. Note this is used to stop the mspace growing beyond the limit to Capacity. When
  // allocations fail we GC before increasing the footprint limit and allowing the mspace to grow.
  void SetFootprintLimit(size_t limit);

  MallocSpace* CreateInstance(const std::string& name, MemMap* mem_map, void* allocator,
                              byte* begin, byte* end, byte* limit, size_t growth_limit);

  uint64_t GetBytesAllocated();
  uint64_t GetObjectsAllocated();

  // Returns the class of a recently freed object.
  mirror::Class* FindRecentFreedObject(const mirror::Object* obj);

  virtual void InvalidateAllocator() {
    mspace_for_alloc_ = nullptr;
  }

  virtual bool IsDlMallocSpace() const {
    return true;
  }
  virtual DlMallocSpace* AsDlMallocSpace() {
    return this;
  }

 protected:
  DlMallocSpace(const std::string& name, MemMap* mem_map, void* mspace, byte* begin, byte* end,
                byte* limit, size_t growth_limit);

 private:
  mirror::Object* AllocWithoutGrowthLocked(Thread* self, size_t num_bytes, size_t* bytes_allocated)
      EXCLUSIVE_LOCKS_REQUIRED(lock_);

  void* CreateAllocator(void* base, size_t morecore_start, size_t initial_size, bool /*low_memory_mode*/) {
    return CreateMspace(base, morecore_start, initial_size);
  }
  static void* CreateMspace(void* base, size_t morecore_start, size_t initial_size);

  // The boundary tag overhead.
  static const size_t kChunkOverhead = kWordSize;

  // Underlying malloc space
  void* const mspace_;

  // A mspace pointer used for allocation. Equals to what mspace_
  // points to or nullptr after InvalidateAllocator() is called.
  void* mspace_for_alloc_;

  friend class collector::MarkSweep;

  DISALLOW_COPY_AND_ASSIGN(DlMallocSpace);
};

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_DLMALLOC_SPACE_H_
