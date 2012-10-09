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

#include "../mutex.h"
#include "UniquePtr.h"
#include "globals.h"
#include "image.h"
#include "macros.h"
#include "dlmalloc.h"
#include "mem_map.h"

namespace art {

class AllocSpace;
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

  const std::string& GetName() const {
    return name_;
  }

  GcRetentionPolicy GetGcRetentionPolicy() const {
    return gc_retention_policy_;
  }

  void SetGcRetentionPolicy(GcRetentionPolicy gc_retention_policy) {
    gc_retention_policy_ = gc_retention_policy;
  }

  ImageSpace* AsImageSpace() {
    DCHECK_EQ(GetType(), kSpaceTypeImageSpace);
    return down_cast<ImageSpace*>(this);
  }

  AllocSpace* AsAllocSpace() {
    DCHECK_EQ(GetType(), kSpaceTypeAllocSpace);
    return down_cast<AllocSpace*>(this);
  }

  AllocSpace* AsZygoteSpace() {
    DCHECK_EQ(GetType(), kSpaceTypeZygoteSpace);
    return down_cast<AllocSpace*>(this);
  }

  LargeObjectSpace* AsLargeObjectSpace() {
    DCHECK_EQ(GetType(), kSpaceTypeLargeObjectSpace);
    return down_cast<LargeObjectSpace*>(this);
  }

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
  Space(const std::string& name, GcRetentionPolicy gc_retention_policy);

  // Name of the space.
  std::string name_;

  // Garbage collection retention policy, used to figure out when we should sweep over this space.
  GcRetentionPolicy gc_retention_policy_;

 private:
  DISALLOW_COPY_AND_ASSIGN(Space);
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

 protected:
  ContinuousSpace(const std::string& name, byte* begin, byte* end,
                  GcRetentionPolicy gc_retention_policy);

  // The beginning of the storage for fast access.
  byte* begin_;

  // Current end of the space.
  byte* end_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ContinuousSpace);
};

class DiscontinuousSpace : public Space {
 public:
  // Is object within this space?
  virtual bool Contains(const Object* obj) const = 0;

protected:
  DiscontinuousSpace(const std::string& name, GcRetentionPolicy gc_retention_policy);

private:
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
class AllocSpace : public MemMapSpace {
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
  static AllocSpace* Create(const std::string& name, size_t initial_size, size_t growth_limit,
                            size_t capacity, byte* requested_begin);

  // Allocate num_bytes without allowing the underlying mspace to grow.
  virtual Object* AllocWithGrowth(Thread* self, size_t num_bytes);

  // Allocate num_bytes allowing the underlying mspace to grow.
  virtual Object* AllocWithoutGrowth(Thread* self, size_t num_bytes);

  // Return the storage space required by obj.
  virtual size_t AllocationSize(const Object* obj);
  virtual void Free(Thread* self, Object* ptr);
  virtual void FreeList(Thread* self, size_t num_ptrs, Object** ptrs);

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
  AllocSpace* CreateZygoteSpace();

  size_t GetNumBytesAllocated() const {
    return num_bytes_allocated_;
  }

  size_t GetNumObjectsAllocated() const {
    return num_objects_allocated_;
  }

  size_t GetTotalBytesAllocated() const {
    return total_bytes_allocated_;
  }

  size_t GetTotalObjectsAllocated() const {
    return total_objects_allocated_;
  }

 private:
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

  AllocSpace(const std::string& name, MemMap* mem_map, void* mspace, byte* begin, byte* end,
             size_t growth_limit);

  bool Init(size_t initial_size, size_t maximum_size, size_t growth_size, byte* requested_base);

  static void* CreateMallocSpace(void* base, size_t morecore_start, size_t initial_size);

  // The boundary tag overhead.
  static const size_t kChunkOverhead = kWordSize;

  // Used to ensure mutual exclusion when the allocation spaces data structures are being modified.
  Mutex lock_;

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

  DISALLOW_COPY_AND_ASSIGN(AllocSpace);
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

  const std::string& GetImageFilename() const {
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

class LargeObjectSpace : public DiscontinuousSpace {
 public:
  virtual bool CanAllocateInto() const {
    return true;
  }

  virtual bool IsCompactible() const {
    return true;
  }

  virtual SpaceType GetType() const {
    return kSpaceTypeLargeObjectSpace;
  }

  virtual SpaceSetMap* GetLiveObjects() const {
    return live_objects_.get();
  }

  virtual SpaceSetMap* GetMarkObjects() const {
    return mark_objects_.get();
  }

  virtual void SwapBitmaps();
  virtual void CopyLiveToMarked();

  // Return the storage space required by obj.
  virtual size_t AllocationSize(const Object* obj) = 0;
  virtual Object* Alloc(Thread* self, size_t num_bytes) = 0;
  virtual void Free(Thread* self, Object* ptr) = 0;
  virtual void Walk(AllocSpace::WalkCallback, void* arg) = 0;

  virtual ~LargeObjectSpace() {}


  size_t GetNumBytesAllocated() const {
    return num_bytes_allocated_;
  }

  size_t GetNumObjectsAllocated() const {
    return num_objects_allocated_;
  }

  size_t GetTotalBytesAllocated() const {
    return total_bytes_allocated_;
  }

  size_t GetTotalObjectsAllocated() const {
    return total_objects_allocated_;
  }

 protected:

  LargeObjectSpace(const std::string& name);

  // Approximate number of bytes which have been allocated into the space.
  size_t num_bytes_allocated_;
  size_t num_objects_allocated_;
  size_t total_bytes_allocated_;
  size_t total_objects_allocated_;

  UniquePtr<SpaceSetMap> live_objects_;
  UniquePtr<SpaceSetMap> mark_objects_;

  friend class Space;
};

class LargeObjectMapSpace : public LargeObjectSpace {
 public:

  // Creates a large object space. Allocations into the large object space use memory maps instead
  // of malloc.
  static LargeObjectMapSpace* Create(const std::string& name);

  // Return the storage space required by obj.
  virtual size_t AllocationSize(const Object* obj);
  virtual Object* Alloc(Thread* self, size_t num_bytes);
  virtual void Free(Thread* self, Object* ptr);
  virtual void Walk(AllocSpace::WalkCallback, void* arg);
  virtual bool Contains(const Object* obj) const;
private:
  LargeObjectMapSpace(const std::string& name);
  virtual ~LargeObjectMapSpace() {}

  // Used to ensure mutual exclusion when the allocation spaces data structures are being modified.
  mutable Mutex lock_;
  std::vector<Object*> large_objects_;
  typedef SafeMap<Object*, MemMap*> MemMaps;
  MemMaps mem_maps_;
};

class FreeListSpace : public LargeObjectSpace {
 public:
  virtual ~FreeListSpace();
  static FreeListSpace* Create(const std::string& name, byte* requested_begin, size_t capacity);

  virtual size_t AllocationSize(const Object* obj);
  virtual Object* Alloc(Thread* self, size_t num_bytes);
  virtual void Free(Thread* self, Object* obj);
  virtual void FreeList(Thread* self, size_t num_ptrs, Object** ptrs);
  virtual bool Contains(const Object* obj) const;
  virtual void Walk(AllocSpace::WalkCallback callback, void* arg);

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
 private:
  static const size_t kAlignment = kPageSize;

  class Chunk {
   public:
    static const size_t kFreeFlag = 0x80000000;

    struct SortBySize {
      bool operator()(const Chunk* a, const Chunk* b) const {
        return a->GetSize() < b->GetSize();
      }
    };

    bool IsFree() const {
      return (m_size & kFreeFlag) != 0;
    }

    void SetSize(size_t size, bool is_free = false) {
      m_size = size | (is_free ? kFreeFlag : 0);
    }

    size_t GetSize() const {
      return m_size & (kFreeFlag - 1);
    }

    Chunk* GetPrevious() {
      return m_previous;
    }

    void SetPrevious(Chunk* previous) {
      m_previous = previous;
      DCHECK(m_previous == NULL ||
            (m_previous != NULL && m_previous + m_previous->GetSize() / kAlignment == this));
    }
   private:
    size_t m_size;
    Chunk* m_previous;
  };

  FreeListSpace(const std::string& name, MemMap* mem_map, byte* begin, byte* end);
  void AddFreeChunk(void* address, size_t size, Chunk* previous);
  Chunk* ChunkFromAddr(void* address);
  void* AddrFromChunk(Chunk* chunk);
  void RemoveFreeChunk(Chunk* chunk);
  Chunk* GetNextChunk(Chunk* chunk);

  typedef std::multiset<Chunk*, Chunk::SortBySize> FreeChunks;
  byte* begin_;
  byte* end_;
  UniquePtr<MemMap> mem_map_;
  Mutex lock_;
  std::vector<Chunk> chunks_;
  FreeChunks free_chunks_;
};

// Callback for dlmalloc_inspect_all or mspace_inspect_all that will madvise(2) unused
// pages back to the kernel.
void MspaceMadviseCallback(void* start, void* end, size_t used_bytes, void* /*arg*/);

}  // namespace art

#endif  // ART_SRC_SPACE_H_
