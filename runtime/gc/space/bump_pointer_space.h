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

#ifndef ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_H_
#define ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_H_

#include "space.h"

namespace art {
namespace gc {

namespace collector {
  class MarkSweep;
}  // namespace collector

namespace space {

// A bump pointer space is a space where objects may be allocated and garbage collected.
class BumpPointerSpace : public ContinuousMemMapAllocSpace {
 public:
  typedef void(*WalkCallback)(void *start, void *end, size_t num_bytes, void* callback_arg);

  SpaceType GetType() const {
    return kSpaceTypeBumpPointerSpace;
  }

  // Create a bump pointer space with the requested sizes. The requested base address is not
  // guaranteed to be granted, if it is required, the caller should call Begin on the returned
  // space to confirm the request was granted.
  static BumpPointerSpace* Create(const std::string& name, size_t capacity, byte* requested_begin);

  // Allocate num_bytes, returns nullptr if the space is full.
  virtual mirror::Object* Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated);
  mirror::Object* AllocNonvirtual(size_t num_bytes);

  // Return the storage space required by obj.
  virtual size_t AllocationSize(const mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Nos unless we support free lists.
  virtual size_t Free(Thread*, mirror::Object*) {
    return 0;
  }
  virtual size_t FreeList(Thread*, size_t, mirror::Object**) {
    return 0;
  }

  size_t AllocationSizeNonvirtual(const mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return obj->SizeOf();
  }

  // Removes the fork time growth limit on capacity, allowing the application to allocate up to the
  // maximum reserved size of the heap.
  void ClearGrowthLimit() {
    growth_end_ = Limit();
  }

  // Override capacity so that we only return the possibly limited capacity
  size_t Capacity() const {
    return growth_end_ - begin_;
  }

  // The total amount of memory reserved for the space.
  size_t NonGrowthLimitCapacity() const {
    return GetMemMap()->Size();
  }

  accounting::SpaceBitmap* GetLiveBitmap() const {
    return nullptr;
  }

  accounting::SpaceBitmap* GetMarkBitmap() const {
    return nullptr;
  }

  // Clear the memory and reset the pointer to the start of the space.
  void Clear();

  void Dump(std::ostream& os) const;

  uint64_t GetBytesAllocated() {
    return Size();
  }

  uint64_t GetObjectsAllocated() {
    return num_objects_allocated_;
  }

  uint64_t GetTotalBytesAllocated() {
    return total_bytes_allocated_;
  }

  uint64_t GetTotalObjectsAllocated() {
    return total_objects_allocated_;
  }

  bool Contains(const mirror::Object* obj) const {
    const byte* byte_obj = reinterpret_cast<const byte*>(obj);
    return byte_obj >= Begin() && byte_obj < End();
  }

  // TODO: Change this? Mainly used for compacting to a particular region of memory.
  BumpPointerSpace(const std::string& name, byte* begin, byte* limit);

  // Return the object which comes after obj, while ensuring alignment.
  static mirror::Object* GetNextObject(mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  virtual BumpPointerSpace* AsBumpPointerSpace() {
    return this;
  }

  // Object alignment within the space.
  static constexpr size_t kAlignment = 8;

 protected:
  BumpPointerSpace(const std::string& name, MemMap* mem_map);

  size_t InternalAllocationSize(const mirror::Object* obj);
  mirror::Object* AllocWithoutGrowthLocked(size_t num_bytes, size_t* bytes_allocated)
      EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Approximate number of bytes which have been allocated into the space.
  AtomicInteger num_objects_allocated_;
  AtomicInteger total_bytes_allocated_;
  AtomicInteger total_objects_allocated_;

  byte* growth_end_;

 private:
  friend class collector::MarkSweep;
  DISALLOW_COPY_AND_ASSIGN(BumpPointerSpace);
};

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_H_
