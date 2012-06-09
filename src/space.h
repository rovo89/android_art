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

#ifndef ART_SRC_SPACE_H_
#define ART_SRC_SPACE_H_

#include <string>

#include "UniquePtr.h"
#include "globals.h"
#include "image.h"
#include "macros.h"
#include "dlmalloc.h"
#include "mem_map.h"

namespace art {

class AllocSpace;
class ImageSpace;
class Object;

// A space contains memory allocated for managed objects.
class Space {
 public:
  // Create a AllocSpace with the requested sizes. The requested
  // base address is not guaranteed to be granted, if it is required,
  // the caller should call Begin on the returned space to confirm
  // the request was granted.
  static AllocSpace* CreateAllocSpace(const std::string& name, size_t initial_size,
                                      size_t growth_limit, size_t capacity,
                                      byte* requested_begin);

  // create a Space from an image file. cannot be used for future allocation or collected.
  static ImageSpace* CreateImageSpace(const std::string& image);

  virtual ~Space() {}

  const std::string& GetSpaceName() const {
    return name_;
  }

  // Address at which the space begins
  byte* Begin() const {
    return begin_;
  }

  // Address at which the space ends, which may vary as the space is filled
  byte* End() const {
    return end_;
  }

  // Is object within this space?
  bool Contains(const Object* obj) const {
    const byte* byte_ptr = reinterpret_cast<const byte*>(obj);
    return Begin() <= byte_ptr && byte_ptr < End();
  }

  // Current size of space
  size_t Size() const {
    return End() - Begin();
  }

  // Maximum size of space
  virtual size_t Capacity() const {
    return mem_map_->Size();
  }

  // Size of the space without a limit on its growth. By default this is just the Capacity, but
  // for the allocation space we support starting with a small heap and then extending it.
  virtual size_t NonGrowthLimitCapacity() const {
    return Capacity();
  }

  ImageSpace* AsImageSpace() {
    DCHECK(IsImageSpace());
    return down_cast<ImageSpace*>(this);
  }

  AllocSpace* AsAllocSpace() {
    DCHECK(IsAllocSpace());
    return down_cast<AllocSpace*>(this);
  }

  virtual bool IsAllocSpace() const = 0;
  virtual bool IsImageSpace() const = 0;

 protected:
  Space(const std::string& name, MemMap* mem_map, byte* end) : name_(name), mem_map_(mem_map),
      begin_(mem_map->Begin()), end_(end) {}

  std::string name_;
  // Underlying storage of the space
  UniquePtr<MemMap> mem_map_;

  // The beginning of the storage for fast access (always equals mem_map_->GetAddress())
  byte* const begin_;

  // Current end of the space
  byte* end_;

  DISALLOW_COPY_AND_ASSIGN(Space);
};

std::ostream& operator<<(std::ostream& os, const Space& space);

// An alloc space is a space where objects may be allocated and garbage collected.
class AllocSpace : public Space {
 public:
  // Allocate num_bytes without allowing the underlying mspace to grow.
  Object* AllocWithGrowth(size_t num_bytes);

  // Allocate num_bytes allowing the underlying mspace to grow.
  Object* AllocWithoutGrowth(size_t num_bytes);

  // Return the storage space required by obj.
  size_t AllocationSize(const Object* obj);

  void Free(Object* ptr);

  void FreeList(size_t num_ptrs, Object** ptrs);

  void* MoreCore(intptr_t increment);

  void* GetMspace() const {
    return mspace_;
  }

  // Hands unused pages back to the system.
  void Trim();

  // Perform a mspace_inspect_all which calls back for each allocation chunk. The chunk may not be
  // in use, indicated by num_bytes equaling zero.
  void Walk(void(*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
            void* arg);

  // Returns the number of bytes that the heap is allowed to obtain from the system via MoreCore.
  size_t GetFootprintLimit();

  // Set the maximum number of bytes that the heap is allowed to obtain from the system via
  // MoreCore. Note this is used to stop the mspace growing beyond the limit to Capacity. When
  // allocations fail we GC before increasing the footprint limit and allowing the mspace to grow.
  void SetFootprintLimit(size_t limit);

  // Removes the fork time growth limit on capacity, allowing the application to allocate up to the
  // maximum reserved size of the heap.
  void ClearGrowthLimit() {
    growth_limit_ = NonGrowthLimitCapacity();
  }

  // Override capacity so that we only return the possibly limited capacity
  virtual size_t Capacity() const {
    return growth_limit_;
  }

  // The total amount of memory reserved for the alloc space
  virtual size_t NonGrowthLimitCapacity() const {
    return mem_map_->End() - mem_map_->Begin();
  }

  virtual bool IsAllocSpace() const {
    return true;
  }

  virtual bool IsImageSpace() const {
    return false;
  }

 private:
  friend class Space;

  AllocSpace(const std::string& name, MemMap* mem_map, void* mspace, byte* end,
             size_t growth_limit) :
    Space(name, mem_map, end), mspace_(mspace), growth_limit_(growth_limit) {
    CHECK(mspace != NULL);
  }

  bool Init(size_t initial_size, size_t maximum_size, size_t growth_size, byte* requested_base);

  static void* CreateMallocSpace(void* base, size_t morecore_start, size_t initial_size);

  // The boundary tag overhead.
  static const size_t kChunkOverhead = kWordSize;

  // Underlying malloc space
  void* const mspace_;

  // The capacity of the alloc space until such time that ClearGrowthLimit is called.
  // The underlying mem_map_ controls the maximum size we allow the heap to grow to. The growth
  // limit is a value <= to the mem_map_ capacity used for ergonomic reasons because of the zygote.
  // Prior to forking the zygote the heap will have a maximally sized mem_map_ but the growth_limit_
  // will be set to a lower value. The growth_limit_ is used as the capacity of the alloc_space_,
  // however, capacity normally can't vary. In the case of the growth_limit_ it can be cleared
  // one time by a call to ClearGrowthLimit.
  size_t growth_limit_;

  DISALLOW_COPY_AND_ASSIGN(AllocSpace);
};

// An image space is a space backed with a memory mapped image
class ImageSpace : public Space {
 public:
  const ImageHeader& GetImageHeader() const {
    return *reinterpret_cast<ImageHeader*>(Begin());
  }

  const std::string& GetImageFilename() const {
    return name_;
  }

  // Mark the objects defined in this space in the given live bitmap
  void RecordImageAllocations(HeapBitmap* live_bitmap) const;

  virtual bool IsAllocSpace() const {
    return false;
  }

  virtual bool IsImageSpace() const {
    return true;
  }

 private:
  friend class Space;

  ImageSpace(const std::string& name, MemMap* mem_map) :
      Space(name, mem_map, mem_map->End()) {}

  DISALLOW_COPY_AND_ASSIGN(ImageSpace);
};

// Callback for dlmalloc_inspect_all or mspace_inspect_all that will madvise(2) unused
// pages back to the kernel.
void MspaceMadviseCallback(void* start, void* end, size_t used_bytes, void* /*arg*/);
// Callback for the obsolete dlmalloc_walk_free_pages.
void MspaceMadviseCallback(void* start, void* end, void* /*arg*/);

}  // namespace art

#endif  // ART_SRC_SPACE_H_
