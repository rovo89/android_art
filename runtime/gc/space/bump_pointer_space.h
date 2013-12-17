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

#include "root_visitor.h"
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
  mirror::Object* AllocNonvirtualWithoutAccounting(size_t num_bytes);

  // Return the storage space required by obj.
  virtual size_t AllocationSize(const mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // NOPS unless we support free lists.
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

  void RevokeThreadLocalBuffers(Thread* thread);
  void RevokeAllThreadLocalBuffers();

  uint64_t GetBytesAllocated() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  uint64_t GetObjectsAllocated() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsEmpty() const;

  bool Contains(const mirror::Object* obj) const {
    const byte* byte_obj = reinterpret_cast<const byte*>(obj);
    return byte_obj >= Begin() && byte_obj < End();
  }

  // TODO: Change this? Mainly used for compacting to a particular region of memory.
  BumpPointerSpace(const std::string& name, byte* begin, byte* limit);

  // Return the object which comes after obj, while ensuring alignment.
  static mirror::Object* GetNextObject(mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Allocate a new TLAB, returns false if the allocation failed.
  bool AllocNewTLAB(Thread* self, size_t bytes);

  virtual BumpPointerSpace* AsBumpPointerSpace() {
    return this;
  }

  // Go through all of the blocks and visit the continuous objects.
  void Walk(ObjectVisitorCallback callback, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Object alignment within the space.
  static constexpr size_t kAlignment = 8;

 protected:
  BumpPointerSpace(const std::string& name, MemMap* mem_map);

  // Allocate a raw block of bytes.
  byte* AllocBlock(size_t bytes) EXCLUSIVE_LOCKS_REQUIRED(block_lock_);
  void RevokeThreadLocalBuffersLocked(Thread* thread) EXCLUSIVE_LOCKS_REQUIRED(block_lock_);

  size_t InternalAllocationSize(const mirror::Object* obj);
  mirror::Object* AllocWithoutGrowthLocked(size_t num_bytes, size_t* bytes_allocated)
      EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // The main block is an unbounded block where objects go when there are no other blocks. This
  // enables us to maintain tightly packed objects when you are not using thread local buffers for
  // allocation.
  // The main block is also the block which starts at address 0.
  void UpdateMainBlock() EXCLUSIVE_LOCKS_REQUIRED(block_lock_);

  byte* growth_end_;
  AtomicInteger objects_allocated_;  // Accumulated from revoked thread local regions.
  AtomicInteger bytes_allocated_;  // Accumulated from revoked thread local regions.
  Mutex block_lock_;

  // The number of blocks in the space, if it is 0 then the space has one long continuous block
  // which doesn't have an updated header.
  size_t num_blocks_ GUARDED_BY(block_lock_);

 private:
  struct BlockHeader {
    size_t size_;  // Size of the block in bytes, does not include the header.
    size_t unused_;  // Ensures alignment of kAlignment.
  };

  COMPILE_ASSERT(sizeof(BlockHeader) % kAlignment == 0,
                 continuous_block_must_be_kAlignment_aligned);

  friend class collector::MarkSweep;
  DISALLOW_COPY_AND_ASSIGN(BumpPointerSpace);
};

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_H_
