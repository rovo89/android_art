/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_RUNTIME_GC_SPACE_REGION_SPACE_H_
#define ART_RUNTIME_GC_SPACE_REGION_SPACE_H_

#include "object_callbacks.h"
#include "space.h"
#include "gc/accounting/read_barrier_table.h"

namespace art {
namespace gc {
namespace space {

// A space that consists of equal-sized regions.
class RegionSpace FINAL : public ContinuousMemMapAllocSpace {
 public:
  typedef void(*WalkCallback)(void *start, void *end, size_t num_bytes, void* callback_arg);

  SpaceType GetType() const OVERRIDE {
    return kSpaceTypeRegionSpace;
  }

  // Create a region space with the requested sizes. The requested base address is not
  // guaranteed to be granted, if it is required, the caller should call Begin on the returned
  // space to confirm the request was granted.
  static RegionSpace* Create(const std::string& name, size_t capacity, uint8_t* requested_begin);

  // Allocate num_bytes, returns nullptr if the space is full.
  mirror::Object* Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated,
                        size_t* usable_size) OVERRIDE;
  // Thread-unsafe allocation for when mutators are suspended, used by the semispace collector.
  mirror::Object* AllocThreadUnsafe(Thread* self, size_t num_bytes, size_t* bytes_allocated,
                                    size_t* usable_size)
      OVERRIDE EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  // The main allocation routine.
  template<bool kForEvac>
  ALWAYS_INLINE mirror::Object* AllocNonvirtual(size_t num_bytes, size_t* bytes_allocated,
                                                size_t* usable_size);
  // Allocate/free large objects (objects that are larger than the region size.)
  template<bool kForEvac>
  mirror::Object* AllocLarge(size_t num_bytes, size_t* bytes_allocated, size_t* usable_size);
  void FreeLarge(mirror::Object* large_obj, size_t bytes_allocated);

  // Return the storage space required by obj.
  size_t AllocationSize(mirror::Object* obj, size_t* usable_size) OVERRIDE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return AllocationSizeNonvirtual(obj, usable_size);
  }
  size_t AllocationSizeNonvirtual(mirror::Object* obj, size_t* usable_size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  size_t Free(Thread*, mirror::Object*) OVERRIDE {
    UNIMPLEMENTED(FATAL);
    return 0;
  }
  size_t FreeList(Thread*, size_t, mirror::Object**) OVERRIDE {
    UNIMPLEMENTED(FATAL);
    return 0;
  }
  accounting::ContinuousSpaceBitmap* GetLiveBitmap() const OVERRIDE {
    // No live bitmap.
    return nullptr;
  }
  accounting::ContinuousSpaceBitmap* GetMarkBitmap() const OVERRIDE {
    // No mark bitmap.
    return nullptr;
  }

  void Clear() OVERRIDE LOCKS_EXCLUDED(region_lock_);

  void Dump(std::ostream& os) const;
  void DumpRegions(std::ostream& os);
  void DumpNonFreeRegions(std::ostream& os);

  void RevokeThreadLocalBuffers(Thread* thread) LOCKS_EXCLUDED(region_lock_);
  void RevokeThreadLocalBuffersLocked(Thread* thread) EXCLUSIVE_LOCKS_REQUIRED(region_lock_);
  void RevokeAllThreadLocalBuffers() LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_,
                                                    Locks::thread_list_lock_);
  void AssertThreadLocalBuffersAreRevoked(Thread* thread) LOCKS_EXCLUDED(region_lock_);
  void AssertAllThreadLocalBuffersAreRevoked() LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_,
                                                              Locks::thread_list_lock_);

  enum SubSpaceType {
    kAllSpaces,        // All spaces.
    kFromSpace,        // From-space. To be evacuated.
    kUnevacFromSpace,  // Unevacuated from-space. Not to be evacuated.
    kToSpace,          // To-space.
  };

  template<SubSpaceType kSubSpaceType> uint64_t GetBytesAllocatedInternal();
  template<SubSpaceType kSubSpaceType> uint64_t GetObjectsAllocatedInternal();
  uint64_t GetBytesAllocated() {
    return GetBytesAllocatedInternal<kAllSpaces>();
  }
  uint64_t GetObjectsAllocated() {
    return GetObjectsAllocatedInternal<kAllSpaces>();
  }
  uint64_t GetBytesAllocatedInFromSpace() {
    return GetBytesAllocatedInternal<kFromSpace>();
  }
  uint64_t GetObjectsAllocatedInFromSpace() {
    return GetObjectsAllocatedInternal<kFromSpace>();
  }
  uint64_t GetBytesAllocatedInUnevacFromSpace() {
    return GetBytesAllocatedInternal<kUnevacFromSpace>();
  }
  uint64_t GetObjectsAllocatedInUnevacFromSpace() {
    return GetObjectsAllocatedInternal<kUnevacFromSpace>();
  }

  bool CanMoveObjects() const OVERRIDE {
    return true;
  }

  bool Contains(const mirror::Object* obj) const {
    const uint8_t* byte_obj = reinterpret_cast<const uint8_t*>(obj);
    return byte_obj >= Begin() && byte_obj < Limit();
  }

  RegionSpace* AsRegionSpace() OVERRIDE {
    return this;
  }

  // Go through all of the blocks and visit the continuous objects.
  void Walk(ObjectCallback* callback, void* arg)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_) {
    WalkInternal<false>(callback, arg);
  }

  void WalkToSpace(ObjectCallback* callback, void* arg)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_) {
    WalkInternal<true>(callback, arg);
  }

  accounting::ContinuousSpaceBitmap::SweepCallback* GetSweepCallback() OVERRIDE {
    return nullptr;
  }
  void LogFragmentationAllocFailure(std::ostream& os, size_t failed_alloc_bytes) OVERRIDE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Object alignment within the space.
  static constexpr size_t kAlignment = kObjectAlignment;
  // The region size.
  static constexpr size_t kRegionSize = 1 * MB;

  bool IsInFromSpace(mirror::Object* ref) {
    if (HasAddress(ref)) {
      Region* r = RefToRegionUnlocked(ref);
      return r->IsInFromSpace();
    }
    return false;
  }

  bool IsInUnevacFromSpace(mirror::Object* ref) {
    if (HasAddress(ref)) {
      Region* r = RefToRegionUnlocked(ref);
      return r->IsInUnevacFromSpace();
    }
    return false;
  }

  bool IsInToSpace(mirror::Object* ref) {
    if (HasAddress(ref)) {
      Region* r = RefToRegionUnlocked(ref);
      return r->IsInToSpace();
    }
    return false;
  }

  void SetFromSpace(accounting::ReadBarrierTable* rb_table, bool force_evacuate_all)
      LOCKS_EXCLUDED(region_lock_);

  size_t FromSpaceSize();
  size_t UnevacFromSpaceSize();
  size_t ToSpaceSize();
  void ClearFromSpace();

  void AddLiveBytes(mirror::Object* ref, size_t alloc_size) {
    Region* reg = RefToRegion(ref);
    reg->AddLiveBytes(alloc_size);
  }

  void AssertAllRegionLiveBytesZeroOrCleared();

  void RecordAlloc(mirror::Object* ref);
  bool AllocNewTlab(Thread* self);

  uint32_t Time() {
    return time_;
  }

 private:
  RegionSpace(const std::string& name, MemMap* mem_map);

  template<bool kToSpaceOnly>
  void WalkInternal(ObjectCallback* callback, void* arg) NO_THREAD_SAFETY_ANALYSIS;

  enum RegionState {
    kRegionFree,                      // Free region.
    kRegionToSpace,                   // To-space region.
    kRegionFromSpace,                 // From-space region. To be evacuated.
    kRegionUnevacFromSpace,           // Unevacuated from-space region. Not to be evacuated.
    kRegionLargeToSpace,              // Large (allocation larger than the region size) to-space.
    kRegionLargeFromSpace,            // Large from-space. To be evacuated.
    kRegionLargeUnevacFromSpace,      // Large unevacuated from-space.
    kRegionLargeTailToSpace,          // Large tail (non-first regions of a large allocation).
    kRegionLargeTailFromSpace,        // Large tail from-space.
    kRegionLargeTailUnevacFromSpace,  // Large tail unevacuated from-space.
  };

  class Region {
   public:
    Region()
        : idx_(static_cast<size_t>(-1)),
          begin_(nullptr), top_(nullptr), end_(nullptr), state_(kRegionToSpace),
          objects_allocated_(0), alloc_time_(0), live_bytes_(static_cast<size_t>(-1)),
          is_newly_allocated_(false), is_a_tlab_(false), thread_(nullptr) {}

    Region(size_t idx, uint8_t* begin, uint8_t* end)
        : idx_(idx), begin_(begin), top_(begin), end_(end), state_(kRegionFree),
          objects_allocated_(0), alloc_time_(0), live_bytes_(static_cast<size_t>(-1)),
          is_newly_allocated_(false), is_a_tlab_(false), thread_(nullptr) {
      DCHECK_LT(begin, end);
      DCHECK_EQ(static_cast<size_t>(end - begin), kRegionSize);
    }

    void Clear() {
      top_ = begin_;
      state_ = kRegionFree;
      objects_allocated_ = 0;
      alloc_time_ = 0;
      live_bytes_ = static_cast<size_t>(-1);
      if (!kMadviseZeroes) {
        memset(begin_, 0, end_ - begin_);
      }
      madvise(begin_, end_ - begin_, MADV_DONTNEED);
      is_newly_allocated_ = false;
      is_a_tlab_ = false;
      thread_ = nullptr;
    }

    ALWAYS_INLINE mirror::Object* Alloc(size_t num_bytes, size_t* bytes_allocated,
                                        size_t* usable_size);

    bool IsFree() const {
      bool is_free = state_ == kRegionFree;
      if (is_free) {
        DCHECK_EQ(begin_, top_);
        DCHECK_EQ(objects_allocated_, 0U);
      }
      return is_free;
    }

    // Given a free region, declare it non-free (allocated).
    void Unfree(uint32_t alloc_time) {
      DCHECK(IsFree());
      state_ = kRegionToSpace;
      alloc_time_ = alloc_time;
    }

    void UnfreeLarge(uint32_t alloc_time) {
      DCHECK(IsFree());
      state_ = kRegionLargeToSpace;
      alloc_time_ = alloc_time;
    }

    void UnfreeLargeTail(uint32_t alloc_time) {
      DCHECK(IsFree());
      state_ = kRegionLargeTailToSpace;
      alloc_time_ = alloc_time;
    }

    void SetNewlyAllocated() {
      is_newly_allocated_ = true;
    }

    // Non-large, non-large-tail.
    bool IsNormal() const {
      return state_ == kRegionToSpace || state_ == kRegionFromSpace ||
          state_ == kRegionUnevacFromSpace;
    }

    bool IsLarge() const {
      bool is_large = state_ == kRegionLargeToSpace || state_ == kRegionLargeFromSpace ||
          state_ == kRegionLargeUnevacFromSpace;
      if (is_large) {
        DCHECK_LT(begin_ + 1 * MB, top_);
      }
      return is_large;
    }

    bool IsLargeTail() const {
      bool is_large_tail = state_ == kRegionLargeTailToSpace ||
          state_ == kRegionLargeTailFromSpace ||
          state_ == kRegionLargeTailUnevacFromSpace;
      if (is_large_tail) {
        DCHECK_EQ(begin_, top_);
      }
      return is_large_tail;
    }

    size_t Idx() const {
      return idx_;
    }

    bool IsInFromSpace() const {
      return state_ == kRegionFromSpace || state_ == kRegionLargeFromSpace ||
          state_ == kRegionLargeTailFromSpace;
    }

    bool IsInToSpace() const {
      return state_ == kRegionToSpace || state_ == kRegionLargeToSpace ||
          state_ == kRegionLargeTailToSpace;
    }

    bool IsInUnevacFromSpace() const {
      return state_ == kRegionUnevacFromSpace || state_ == kRegionLargeUnevacFromSpace ||
          state_ == kRegionLargeTailUnevacFromSpace;
    }

    void SetAsFromSpace() {
      switch (state_) {
        case kRegionToSpace:
          state_ = kRegionFromSpace;
          break;
        case kRegionLargeToSpace:
          state_ = kRegionLargeFromSpace;
          break;
        case kRegionLargeTailToSpace:
          state_ = kRegionLargeTailFromSpace;
          break;
        default:
          LOG(FATAL) << "Unexpected region state : " << static_cast<uint>(state_)
                     << " idx=" << idx_;
      }
      live_bytes_ = static_cast<size_t>(-1);
    }

    void SetAsUnevacFromSpace() {
      switch (state_) {
        case kRegionToSpace:
          state_ = kRegionUnevacFromSpace;
          break;
        case kRegionLargeToSpace:
          state_ = kRegionLargeUnevacFromSpace;
          break;
        case kRegionLargeTailToSpace:
          state_ = kRegionLargeTailUnevacFromSpace;
          break;
        default:
          LOG(FATAL) << "Unexpected region state : " << static_cast<uint>(state_)
                     << " idx=" << idx_;
      }
      live_bytes_ = 0U;
    }

    void SetUnevacFromSpaceAsToSpace() {
      switch (state_) {
        case kRegionUnevacFromSpace:
          state_ = kRegionToSpace;
          break;
        case kRegionLargeUnevacFromSpace:
          state_ = kRegionLargeToSpace;
          break;
        case kRegionLargeTailUnevacFromSpace:
          state_ = kRegionLargeTailToSpace;
          break;
        default:
          LOG(FATAL) << "Unexpected region state : " << static_cast<uint>(state_)
                     << " idx=" << idx_;
      }
    }

    ALWAYS_INLINE bool ShouldBeEvacuated();

    void AddLiveBytes(size_t live_bytes) {
      DCHECK(IsInUnevacFromSpace());
      DCHECK(!IsLargeTail());
      DCHECK_NE(live_bytes_, static_cast<size_t>(-1));
      live_bytes_ += live_bytes;
      DCHECK_LE(live_bytes_, BytesAllocated());
    }

    size_t LiveBytes() const {
      return live_bytes_;
    }

    uint GetLivePercent() const {
      DCHECK(IsInToSpace());
      DCHECK(!IsLargeTail());
      DCHECK_NE(live_bytes_, static_cast<size_t>(-1));
      DCHECK_LE(live_bytes_, BytesAllocated());
      size_t bytes_allocated = RoundUp(BytesAllocated(), kRegionSize);
      DCHECK_GE(bytes_allocated, 0U);
      uint result = (live_bytes_ * 100U) / bytes_allocated;
      DCHECK_LE(result, 100U);
      return result;
    }

    size_t BytesAllocated() const {
      if (IsLarge()) {
        DCHECK_LT(begin_ + kRegionSize, top_);
        return static_cast<size_t>(top_ - begin_);
      } else if (IsLargeTail()) {
        DCHECK_EQ(begin_, top_);
        return 0;
      } else {
        DCHECK(IsNormal()) << static_cast<uint>(state_);
        DCHECK_LE(begin_, top_);
        size_t bytes = static_cast<size_t>(top_ - begin_);
        DCHECK_LE(bytes, kRegionSize);
        return bytes;
      }
    }

    size_t ObjectsAllocated() const {
      if (IsLarge()) {
        DCHECK_LT(begin_ + 1 * MB, top_);
        DCHECK_EQ(objects_allocated_, 0U);
        return 1;
      } else if (IsLargeTail()) {
        DCHECK_EQ(begin_, top_);
        DCHECK_EQ(objects_allocated_, 0U);
        return 0;
      } else {
        DCHECK(IsNormal()) << static_cast<uint>(state_);
        return objects_allocated_;
      }
    }

    uint8_t* Begin() const {
      return begin_;
    }

    uint8_t* Top() const {
      return top_;
    }

    void SetTop(uint8_t* new_top) {
      top_ = new_top;
    }

    uint8_t* End() const {
      return end_;
    }

    bool Contains(mirror::Object* ref) const {
      return begin_ <= reinterpret_cast<uint8_t*>(ref) && reinterpret_cast<uint8_t*>(ref) < end_;
    }

    void Dump(std::ostream& os) const;

    void RecordThreadLocalAllocations(size_t num_objects, size_t num_bytes) {
      DCHECK(IsNormal());
      DCHECK_EQ(objects_allocated_, 0U);
      DCHECK_EQ(top_, end_);
      objects_allocated_ = num_objects;
      top_ = begin_ + num_bytes;
      DCHECK_EQ(top_, end_);
    }

   private:
    size_t idx_;                   // The region's index in the region space.
    uint8_t* begin_;               // The begin address of the region.
    // Can't use Atomic<uint8_t*> as Atomic's copy operator is implicitly deleted.
    uint8_t* top_;                 // The current position of the allocation.
    uint8_t* end_;                 // The end address of the region.
    uint8_t state_;                // The region state (see RegionState).
    uint64_t objects_allocated_;   // The number of objects allocated.
    uint32_t alloc_time_;          // The allocation time of the region.
    size_t live_bytes_;            // The live bytes. Used to compute the live percent.
    bool is_newly_allocated_;      // True if it's allocated after the last collection.
    bool is_a_tlab_;               // True if it's a tlab.
    Thread* thread_;               // The owning thread if it's a tlab.

    friend class RegionSpace;
  };

  Region* RefToRegion(mirror::Object* ref) LOCKS_EXCLUDED(region_lock_) {
    MutexLock mu(Thread::Current(), region_lock_);
    return RefToRegionLocked(ref);
  }

  Region* RefToRegionUnlocked(mirror::Object* ref) NO_THREAD_SAFETY_ANALYSIS {
    // For a performance reason (this is frequently called via
    // IsInFromSpace() etc.) we avoid taking a lock here. Note that
    // since we only change a region from to-space to from-space only
    // during a pause (SetFromSpace()) and from from-space to free
    // (after GC is done) as long as ref is a valid reference into an
    // allocated region, it's safe to access the region state without
    // the lock.
    return RefToRegionLocked(ref);
  }

  Region* RefToRegionLocked(mirror::Object* ref) EXCLUSIVE_LOCKS_REQUIRED(region_lock_) {
    DCHECK(HasAddress(ref));
    uintptr_t offset = reinterpret_cast<uintptr_t>(ref) - reinterpret_cast<uintptr_t>(Begin());
    size_t reg_idx = offset / kRegionSize;
    DCHECK_LT(reg_idx, num_regions_);
    Region* reg = &regions_[reg_idx];
    DCHECK_EQ(reg->Idx(), reg_idx);
    DCHECK(reg->Contains(ref));
    return reg;
  }

  mirror::Object* GetNextObject(mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Mutex region_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;

  uint32_t time_;                  // The time as the number of collections since the startup.
  size_t num_regions_;             // The number of regions in this space.
  size_t num_non_free_regions_;    // The number of non-free regions in this space.
  std::unique_ptr<Region[]> regions_ GUARDED_BY(region_lock_);
                                   // The pointer to the region array.
  Region* current_region_;         // The region that's being allocated currently.
  Region* evac_region_;            // The region that's being evacuated to currently.
  Region full_region_;             // The dummy/sentinel region that looks full.

  DISALLOW_COPY_AND_ASSIGN(RegionSpace);
};

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_REGION_SPACE_H_
