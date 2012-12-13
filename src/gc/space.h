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

#ifndef ART_SRC_GC_SPACE_H_
#define ART_SRC_GC_SPACE_H_

#include <string>

#include "UniquePtr.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "globals.h"
#include "image.h"
#include "dlmalloc.h"
#include "mem_map.h"

namespace art {

static const bool kDebugSpaces = kIsDebugBuild;

class DlMallocSpace;
class ImageSpace;
class LargeObjectSpace;
class Object;
class SpaceBitmap;

enum GcRetentionPolicy {
  kGcRetentionPolicyNeverCollect,
  kGcRetentionPolicyAlwaysCollect,
  kGcRetentionPolicyFullCollect, // Collect only for full GC
};
std::ostream& operator<<(std::ostream& os, const GcRetentionPolicy& policy);

enum SpaceType {
  kSpaceTypeImageSpace,
  kSpaceTypeAllocSpace,
  kSpaceTypeZygoteSpace,
  kSpaceTypeLargeObjectSpace,
};
std::ostream& operator<<(std::ostream& os, const SpaceType& space_type);

// A space contains memory allocated for managed objects.
class Space {
 public:
  virtual bool CanAllocateInto() const = 0;
  virtual bool IsCompactible() const = 0;
  virtual bool Contains(const Object* obj) const = 0;
  virtual SpaceType GetType() const = 0;
  virtual GcRetentionPolicy GetGcRetentionPolicy() const = 0;
  virtual std::string GetName() const = 0;

  ImageSpace* AsImageSpace();
  DlMallocSpace* AsAllocSpace();
  DlMallocSpace* AsZygoteSpace();
  LargeObjectSpace* AsLargeObjectSpace();

  bool IsImageSpace() const {
    return GetType() == kSpaceTypeImageSpace;
  }

  bool IsAllocSpace() const {
    return GetType() == kSpaceTypeAllocSpace || GetType() == kSpaceTypeZygoteSpace;
  }

  bool IsZygoteSpace() const {
    return GetType() == kSpaceTypeZygoteSpace;
  }

  bool IsLargeObjectSpace() const {
    return GetType() == kSpaceTypeLargeObjectSpace;
  }

  virtual void Dump(std::ostream& /* os */) const { }

  virtual ~Space() {}

 protected:
  Space() { }

 private:
  DISALLOW_COPY_AND_ASSIGN(Space);
};

// AllocSpace interface.
class AllocSpace {
 public:
  virtual bool CanAllocateInto() const {
    return true;
  }

  // General statistics
  virtual uint64_t GetNumBytesAllocated() const = 0;
  virtual uint64_t GetNumObjectsAllocated() const = 0;
  virtual uint64_t GetTotalBytesAllocated() const = 0;
  virtual uint64_t GetTotalObjectsAllocated() const = 0;

  // Allocate num_bytes without allowing growth.
  virtual Object* Alloc(Thread* self, size_t num_bytes) = 0;

  // Return the storage space required by obj.
  virtual size_t AllocationSize(const Object* obj) = 0;

  // Returns how many bytes were freed.
  virtual size_t Free(Thread* self, Object* ptr) = 0;

  // Returns how many bytes were freed.
  virtual size_t FreeList(Thread* self, size_t num_ptrs, Object** ptrs) = 0;

 protected:
  AllocSpace() {}
  virtual ~AllocSpace() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(AllocSpace);
};

// Continuous spaces have bitmaps, and an address range.
class ContinuousSpace : public Space {
 public:
  // Address at which the space begins
  byte* Begin() const {
    return begin_;
  }

  // Address at which the space ends, which may vary as the space is filled.
  byte* End() const {
    return end_;
  }

  // Current size of space
  size_t Size() const {
    return End() - Begin();
  }

  virtual SpaceBitmap* GetLiveBitmap() const = 0;
  virtual SpaceBitmap* GetMarkBitmap() const = 0;

  // Is object within this space?
  bool HasAddress(const Object* obj) const {
    const byte* byte_ptr = reinterpret_cast<const byte*>(obj);
    return Begin() <= byte_ptr && byte_ptr < End();
  }

  virtual bool Contains(const Object* obj) const {
    return HasAddress(obj);
  }

  virtual ~ContinuousSpace() {}

  virtual std::string GetName() const {
    return name_;
  }

  virtual GcRetentionPolicy GetGcRetentionPolicy() const {
    return gc_retention_policy_;
  }

 protected:
  ContinuousSpace(const std::string& name, byte* begin, byte* end,
                  GcRetentionPolicy gc_retention_policy);

  std::string name_;
  GcRetentionPolicy gc_retention_policy_;

  // The beginning of the storage for fast access.
  byte* begin_;

  // Current end of the space.
  byte* end_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ContinuousSpace);
};

class DiscontinuousSpace : public virtual Space {
 public:
  // Is object within this space?
  virtual bool Contains(const Object* obj) const = 0;

  virtual std::string GetName() const {
    return name_;
  }

  virtual GcRetentionPolicy GetGcRetentionPolicy() const {
    return gc_retention_policy_;
  }

protected:
  DiscontinuousSpace(const std::string& name, GcRetentionPolicy gc_retention_policy);

private:
  std::string name_;
  GcRetentionPolicy gc_retention_policy_;

  DISALLOW_COPY_AND_ASSIGN(DiscontinuousSpace);
};

std::ostream& operator<<(std::ostream& os, const Space& space);

class MemMapSpace : public ContinuousSpace {
 public:
  // Maximum which the mapped space can grow to.
  virtual size_t Capacity() const {
    return mem_map_->Size();
  }

  // Size of the space without a limit on its growth. By default this is just the Capacity, but
  // for the allocation space we support starting with a small heap and then extending it.
  virtual size_t NonGrowthLimitCapacity() const {
    return Capacity();
  }

 protected:
  MemMapSpace(const std::string& name, MemMap* mem_map, size_t initial_size,
              GcRetentionPolicy gc_retention_policy);

  MemMap* GetMemMap() {
    return mem_map_.get();
  }

  const MemMap* GetMemMap() const {
    return mem_map_.get();
  }

 private:
  // Underlying storage of the space
  UniquePtr<MemMap> mem_map_;

  DISALLOW_COPY_AND_ASSIGN(MemMapSpace);
};

// An alloc space is a space where objects may be allocated and garbage collected.
class DlMallocSpace : public MemMapSpace, public AllocSpace {
 public:
  typedef void(*WalkCallback)(void *start, void *end, size_t num_bytes, void* callback_arg);

  virtual bool CanAllocateInto() const {
    return true;
  }

  virtual bool IsCompactible() const {
    return false;
  }

  virtual SpaceType GetType() const {
    return kSpaceTypeAllocSpace;
  }

  // Create a AllocSpace with the requested sizes. The requested
  // base address is not guaranteed to be granted, if it is required,
  // the caller should call Begin on the returned space to confirm
  // the request was granted.
  static DlMallocSpace* Create(const std::string& name, size_t initial_size, size_t growth_limit,
                            size_t capacity, byte* requested_begin);

  // Allocate num_bytes without allowing the underlying mspace to grow.
  virtual Object* AllocWithGrowth(Thread* self, size_t num_bytes);

  // Allocate num_bytes allowing the underlying mspace to grow.
  virtual Object* Alloc(Thread* self, size_t num_bytes);

  // Return the storage space required by obj.
  virtual size_t AllocationSize(const Object* obj);
  virtual size_t Free(Thread* self, Object* ptr);
  virtual size_t FreeList(Thread* self, size_t num_ptrs, Object** ptrs);

  void* MoreCore(intptr_t increment);

  void* GetMspace() const {
    return mspace_;
  }

  // Hands unused pages back to the system.
  void Trim();

  // Perform a mspace_inspect_all which calls back for each allocation chunk. The chunk may not be
  // in use, indicated by num_bytes equaling zero.
  void Walk(WalkCallback callback, void* arg);

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
    return GetMemMap()->Size();
  }

  virtual SpaceBitmap* GetLiveBitmap() const {
    return live_bitmap_.get();
  }

  virtual SpaceBitmap* GetMarkBitmap() const {
    return mark_bitmap_.get();
  }

  virtual void Dump(std::ostream& os) const;

  void SetGrowthLimit(size_t growth_limit);

  // Swap the live and mark bitmaps of this space. This is used by the GC for concurrent sweeping.
  virtual void SwapBitmaps();

  // Turn ourself into a zygote space and return a new alloc space which has our unused memory.
  DlMallocSpace* CreateZygoteSpace();

  void SetGcRetentionPolicy(GcRetentionPolicy gc_retention_policy) {
    gc_retention_policy_ = gc_retention_policy;
  }

  virtual uint64_t GetNumBytesAllocated() const {
    return num_bytes_allocated_;
  }

  virtual uint64_t GetNumObjectsAllocated() const {
    return num_objects_allocated_;
  }

  virtual uint64_t GetTotalBytesAllocated() const {
    return total_bytes_allocated_;
  }

  virtual uint64_t GetTotalObjectsAllocated() const {
    return total_objects_allocated_;
  }

 private:
  size_t InternalAllocationSize(const Object* obj);
  Object* AllocWithoutGrowthLocked(size_t num_bytes) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  UniquePtr<SpaceBitmap> live_bitmap_;
  UniquePtr<SpaceBitmap> mark_bitmap_;
  UniquePtr<SpaceBitmap> temp_bitmap_;

  // Approximate number of bytes which have been allocated into the space.
  size_t num_bytes_allocated_;
  size_t num_objects_allocated_;
  size_t total_bytes_allocated_;
  size_t total_objects_allocated_;

  static size_t bitmap_index_;

  DlMallocSpace(const std::string& name, MemMap* mem_map, void* mspace, byte* begin, byte* end,
             size_t growth_limit);

  bool Init(size_t initial_size, size_t maximum_size, size_t growth_size, byte* requested_base);

  static void* CreateMallocSpace(void* base, size_t morecore_start, size_t initial_size);

  // The boundary tag overhead.
  static const size_t kChunkOverhead = kWordSize;

  // Used to ensure mutual exclusion when the allocation spaces data structures are being modified.
  Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;

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

  friend class MarkSweep;

  DISALLOW_COPY_AND_ASSIGN(DlMallocSpace);
};

// An image space is a space backed with a memory mapped image
class ImageSpace : public MemMapSpace {
 public:
  virtual bool CanAllocateInto() const {
    return false;
  }

  virtual bool IsCompactible() const {
    return false;
  }

  virtual SpaceType GetType() const {
    return kSpaceTypeImageSpace;
  }

  // create a Space from an image file. cannot be used for future allocation or collected.
  static ImageSpace* Create(const std::string& image)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  const ImageHeader& GetImageHeader() const {
    return *reinterpret_cast<ImageHeader*>(Begin());
  }

  const std::string GetImageFilename() const {
    return GetName();
  }

  // Mark the objects defined in this space in the given live bitmap
  void RecordImageAllocations(SpaceBitmap* live_bitmap) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  virtual SpaceBitmap* GetLiveBitmap() const {
    return live_bitmap_.get();
  }

  virtual SpaceBitmap* GetMarkBitmap() const {
    // ImageSpaces have the same bitmap for both live and marked. This helps reduce the number of
    // special cases to test against.
    return live_bitmap_.get();
  }

  virtual void Dump(std::ostream& os) const;

 private:
  friend class Space;

  UniquePtr<SpaceBitmap> live_bitmap_;
  static size_t bitmap_index_;

  ImageSpace(const std::string& name, MemMap* mem_map);

  DISALLOW_COPY_AND_ASSIGN(ImageSpace);
};

// Callback for dlmalloc_inspect_all or mspace_inspect_all that will madvise(2) unused
// pages back to the kernel.
void MspaceMadviseCallback(void* start, void* end, size_t used_bytes, void* /*arg*/);

}  // namespace art

#endif  // ART_SRC_GC_SPACE_H_
