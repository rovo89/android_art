/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_GC_LARGE_OBJECT_SPACE_H_
#define ART_SRC_GC_LARGE_OBJECT_SPACE_H_

#include "space.h"
#include "safe_map.h"

#include <set>
#include <vector>

namespace art {
class SpaceSetMap;

class LargeObjectSpace : public DiscontinuousSpace, public AllocSpace {
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
  virtual void Walk(DlMallocSpace::WalkCallback, void* arg) = 0;
  virtual ~LargeObjectSpace() {}

  uint64_t GetNumBytesAllocated() const {
    return num_bytes_allocated_;
  }

  uint64_t GetNumObjectsAllocated() const {
    return num_objects_allocated_;
  }

  uint64_t GetTotalBytesAllocated() const {
    return total_bytes_allocated_;
  }

  uint64_t GetTotalObjectsAllocated() const {
    return total_objects_allocated_;
  }

  size_t FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs);

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
  virtual size_t AllocationSize(const mirror::Object* obj);
  virtual mirror::Object* Alloc(Thread* self, size_t num_bytes);
  size_t Free(Thread* self, mirror::Object* ptr);
  virtual void Walk(DlMallocSpace::WalkCallback, void* arg);
  virtual bool Contains(const mirror::Object* obj) const;
private:
  LargeObjectMapSpace(const std::string& name);
  virtual ~LargeObjectMapSpace() {}

  // Used to ensure mutual exclusion when the allocation spaces data structures are being modified.
  mutable Mutex lock_;
  std::vector<mirror::Object*> large_objects_;
  typedef SafeMap<mirror::Object*, MemMap*> MemMaps;
  MemMaps mem_maps_;
};

class FreeListSpace : public LargeObjectSpace {
 public:
  virtual ~FreeListSpace();
  static FreeListSpace* Create(const std::string& name, byte* requested_begin, size_t capacity);

  size_t AllocationSize(const mirror::Object* obj);
  mirror::Object* Alloc(Thread* self, size_t num_bytes);
  size_t Free(Thread* self, mirror::Object* obj);
  bool Contains(const mirror::Object* obj) const;
  void Walk(DlMallocSpace::WalkCallback callback, void* arg);

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

  virtual void Dump(std::ostream& os) const;

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

}

#endif  // ART_SRC_GC_LARGE_OBJECT_SPACE_H_
