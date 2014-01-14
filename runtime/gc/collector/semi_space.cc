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

#include "semi_space.h"

#include <functional>
#include <numeric>
#include <climits>
#include <vector>

#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex-inl.h"
#include "base/timing_logger.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/accounting/mod_union_table.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "gc/space/bump_pointer_space.h"
#include "gc/space/bump_pointer_space-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "indirect_reference_table.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "mark_sweep-inl.h"
#include "monitor.h"
#include "mirror/art_field.h"
#include "mirror/art_field-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "mirror/object-inl.h"
#include "mirror/object_array.h"
#include "mirror/object_array-inl.h"
#include "runtime.h"
#include "semi_space-inl.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "verifier/method_verifier.h"

using ::art::mirror::Class;
using ::art::mirror::Object;

namespace art {
namespace gc {
namespace collector {

static constexpr bool kProtectFromSpace = true;
static constexpr bool kResetFromSpace = true;

// TODO: Unduplicate logic.
void SemiSpace::ImmuneSpace(space::ContinuousSpace* space) {
  // Bind live to mark bitmap if necessary.
  if (space->GetLiveBitmap() != space->GetMarkBitmap()) {
    CHECK(space->IsContinuousMemMapAllocSpace());
    space->AsContinuousMemMapAllocSpace()->BindLiveToMarkBitmap();
  }
  // Add the space to the immune region.
  if (immune_begin_ == nullptr) {
    DCHECK(immune_end_ == nullptr);
    immune_begin_ = reinterpret_cast<Object*>(space->Begin());
    immune_end_ = reinterpret_cast<Object*>(space->End());
  } else {
    const space::ContinuousSpace* prev_space = nullptr;
    // Find out if the previous space is immune.
    for (space::ContinuousSpace* cur_space : GetHeap()->GetContinuousSpaces()) {
      if (cur_space == space) {
        break;
      }
      prev_space = cur_space;
    }
    // If previous space was immune, then extend the immune region. Relies on continuous spaces
    // being sorted by Heap::AddContinuousSpace.
    if (prev_space != nullptr && IsImmuneSpace(prev_space)) {
      immune_begin_ = std::min(reinterpret_cast<Object*>(space->Begin()), immune_begin_);
      // Use Limit() instead of End() because otherwise if the
      // generational mode is enabled, the alloc space might expand
      // due to promotion and the sense of immunity may change in the
      // middle of a GC.
      immune_end_ = std::max(reinterpret_cast<Object*>(space->Limit()), immune_end_);
    }
  }
}

void SemiSpace::BindBitmaps() {
  timings_.StartSplit("BindBitmaps");
  WriterMutexLock mu(self_, *Locks::heap_bitmap_lock_);
  // Mark all of the spaces we never collect as immune.
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->GetLiveBitmap() != nullptr) {
      if (space == to_space_) {
        CHECK(to_space_->IsContinuousMemMapAllocSpace());
        to_space_->AsContinuousMemMapAllocSpace()->BindLiveToMarkBitmap();
      } else if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyNeverCollect
                 || space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect
                 // Add the main free list space and the non-moving
                 // space to the immune space if a bump pointer space
                 // only collection.
                 || (generational_ && !whole_heap_collection_ &&
                     (space == GetHeap()->GetNonMovingSpace() ||
                      space == GetHeap()->GetPrimaryFreeListSpace()))) {
        ImmuneSpace(space);
      }
    }
  }
  if (generational_ && !whole_heap_collection_) {
    // We won't collect the large object space if a bump pointer space only collection.
    is_large_object_space_immune_ = true;
    GetHeap()->GetLargeObjectsSpace()->CopyLiveToMarked();
  }
  timings_.EndSplit();
}

SemiSpace::SemiSpace(Heap* heap, bool generational, const std::string& name_prefix)
    : GarbageCollector(heap,
                       name_prefix + (name_prefix.empty() ? "" : " ") + "marksweep + semispace"),
      mark_stack_(nullptr),
      immune_begin_(nullptr),
      immune_end_(nullptr),
      is_large_object_space_immune_(false),
      to_space_(nullptr),
      from_space_(nullptr),
      self_(nullptr),
      generational_(generational),
      last_gc_to_space_end_(nullptr),
      bytes_promoted_(0),
      whole_heap_collection_(true),
      whole_heap_collection_interval_counter_(0) {
}

void SemiSpace::InitializePhase() {
  timings_.Reset();
  TimingLogger::ScopedSplit split("InitializePhase", &timings_);
  mark_stack_ = heap_->mark_stack_.get();
  DCHECK(mark_stack_ != nullptr);
  immune_begin_ = nullptr;
  immune_end_ = nullptr;
  is_large_object_space_immune_ = false;
  self_ = Thread::Current();
  // Do any pre GC verification.
  timings_.NewSplit("PreGcVerification");
  heap_->PreGcVerification(this);
  // Set the initial bitmap.
  to_space_live_bitmap_ = to_space_->GetLiveBitmap();
}

void SemiSpace::ProcessReferences(Thread* self) {
  TimingLogger::ScopedSplit split("ProcessReferences", &timings_);
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetHeap()->ProcessReferences(timings_, clear_soft_references_, &MarkedForwardingAddressCallback,
                               &RecursiveMarkObjectCallback, this);
}

void SemiSpace::MarkingPhase() {
  if (generational_) {
    if (gc_cause_ == kGcCauseExplicit || gc_cause_ == kGcCauseForNativeAlloc ||
        clear_soft_references_) {
      // If an explicit, native allocation-triggered, or last attempt
      // collection, collect the whole heap (and reset the interval
      // counter to be consistent.)
      whole_heap_collection_ = true;
      whole_heap_collection_interval_counter_ = 0;
    }
    if (whole_heap_collection_) {
      VLOG(heap) << "Whole heap collection";
    } else {
      VLOG(heap) << "Bump pointer space only collection";
    }
  }
  Locks::mutator_lock_->AssertExclusiveHeld(self_);
  TimingLogger::ScopedSplit split("MarkingPhase", &timings_);
  // Need to do this with mutators paused so that somebody doesn't accidentally allocate into the
  // wrong space.
  heap_->SwapSemiSpaces();
  if (generational_) {
    // If last_gc_to_space_end_ is out of the bounds of the from-space
    // (the to-space from last GC), then point it to the beginning of
    // the from-space. For example, the very first GC or the
    // pre-zygote compaction.
    if (!from_space_->HasAddress(reinterpret_cast<mirror::Object*>(last_gc_to_space_end_))) {
      last_gc_to_space_end_ = from_space_->Begin();
    }
    // Reset this before the marking starts below.
    bytes_promoted_ = 0;
  }
  // Assume the cleared space is already empty.
  BindBitmaps();
  // Process dirty cards and add dirty cards to mod-union tables.
  heap_->ProcessCards(timings_);
  // Clear the whole card table since we can not get any additional dirty cards during the
  // paused GC. This saves memory but only works for pause the world collectors.
  timings_.NewSplit("ClearCardTable");
  heap_->GetCardTable()->ClearCardTable();
  // Need to do this before the checkpoint since we don't want any threads to add references to
  // the live stack during the recursive mark.
  timings_.NewSplit("SwapStacks");
  heap_->SwapStacks();
  WriterMutexLock mu(self_, *Locks::heap_bitmap_lock_);
  MarkRoots();
  // Mark roots of immune spaces.
  UpdateAndMarkModUnion();
  // Recursively mark remaining objects.
  MarkReachableObjects();
}

bool SemiSpace::IsImmuneSpace(const space::ContinuousSpace* space) const {
  return
    immune_begin_ <= reinterpret_cast<Object*>(space->Begin()) &&
    immune_end_ >= reinterpret_cast<Object*>(space->End());
}

void SemiSpace::UpdateAndMarkModUnion() {
  for (auto& space : heap_->GetContinuousSpaces()) {
    // If the space is immune then we need to mark the references to other spaces.
    if (IsImmuneSpace(space)) {
      accounting::ModUnionTable* table = heap_->FindModUnionTableFromSpace(space);
      if (table != nullptr) {
        // TODO: Improve naming.
        TimingLogger::ScopedSplit split(
            space->IsZygoteSpace() ? "UpdateAndMarkZygoteModUnionTable" :
                                     "UpdateAndMarkImageModUnionTable",
                                     &timings_);
        table->UpdateAndMarkReferences(MarkRootCallback, this);
      } else {
        // If a bump pointer space only collection, the non-moving
        // space is added to the immune space. But the non-moving
        // space doesn't have a mod union table. Instead, its live
        // bitmap will be scanned later in MarkReachableObjects().
        DCHECK(generational_ && !whole_heap_collection_ &&
               (space == heap_->GetNonMovingSpace() || space == heap_->GetPrimaryFreeListSpace()));
      }
    }
  }
}

class SemiSpaceScanObjectVisitor {
 public:
  explicit SemiSpaceScanObjectVisitor(SemiSpace* ss) : semi_space_(ss) {}
  void operator()(Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    // TODO: fix NO_THREAD_SAFETY_ANALYSIS. ScanObject() requires an
    // exclusive lock on the mutator lock, but
    // SpaceBitmap::VisitMarkedRange() only requires the shared lock.
    DCHECK(obj != nullptr);
    semi_space_->ScanObject(obj);
  }
 private:
  SemiSpace* semi_space_;
};

void SemiSpace::MarkReachableObjects() {
  timings_.StartSplit("MarkStackAsLive");
  accounting::ObjectStack* live_stack = heap_->GetLiveStack();
  heap_->MarkAllocStackAsLive(live_stack);
  live_stack->Reset();
  timings_.EndSplit();

  for (auto& space : heap_->GetContinuousSpaces()) {
    // If the space is immune and has no mod union table (the
    // non-moving space when the bump pointer space only collection is
    // enabled,) then we need to scan its live bitmap as roots
    // (including the objects on the live stack which have just marked
    // in the live bitmap above in MarkAllocStackAsLive().)
    if (IsImmuneSpace(space) && heap_->FindModUnionTableFromSpace(space) == nullptr) {
      DCHECK(generational_ && !whole_heap_collection_ &&
             (space == GetHeap()->GetNonMovingSpace() || space == GetHeap()->GetPrimaryFreeListSpace()));
      accounting::SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      SemiSpaceScanObjectVisitor visitor(this);
      live_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(space->Begin()),
                                    reinterpret_cast<uintptr_t>(space->End()),
                                    visitor);
    }
  }

  if (is_large_object_space_immune_) {
    DCHECK(generational_ && !whole_heap_collection_);
    // When the large object space is immune, we need to scan the
    // large object space as roots as they contain references to their
    // classes (primitive array classes) that could move though they
    // don't contain any other references.
    space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
    accounting::ObjectSet* large_live_objects = large_object_space->GetLiveObjects();
    SemiSpaceScanObjectVisitor visitor(this);
    for (const Object* obj : large_live_objects->GetObjects()) {
      visitor(const_cast<Object*>(obj));
    }
  }

  // Recursively process the mark stack.
  ProcessMarkStack(true);
}

void SemiSpace::ReclaimPhase() {
  TimingLogger::ScopedSplit split("ReclaimPhase", &timings_);
  ProcessReferences(self_);
  {
    ReaderMutexLock mu(self_, *Locks::heap_bitmap_lock_);
    SweepSystemWeaks();
  }
  // Record freed memory.
  uint64_t from_bytes = from_space_->GetBytesAllocated();
  uint64_t to_bytes = to_space_->GetBytesAllocated();
  uint64_t from_objects = from_space_->GetObjectsAllocated();
  uint64_t to_objects = to_space_->GetObjectsAllocated();
  CHECK_LE(to_objects, from_objects);
  int64_t freed_bytes = from_bytes - to_bytes;
  int64_t freed_objects = from_objects - to_objects;
  freed_bytes_.FetchAndAdd(freed_bytes);
  freed_objects_.FetchAndAdd(freed_objects);
  // Note: Freed bytes can be negative if we copy form a compacted space to a free-list backed
  // space.
  heap_->RecordFree(freed_objects, freed_bytes);
  timings_.StartSplit("PreSweepingGcVerification");
  heap_->PreSweepingGcVerification(this);
  timings_.EndSplit();

  {
    WriterMutexLock mu(self_, *Locks::heap_bitmap_lock_);
    // Reclaim unmarked objects.
    Sweep(false);
    // Swap the live and mark bitmaps for each space which we modified space. This is an
    // optimization that enables us to not clear live bits inside of the sweep. Only swaps unbound
    // bitmaps.
    timings_.StartSplit("SwapBitmaps");
    SwapBitmaps();
    timings_.EndSplit();
    // Unbind the live and mark bitmaps.
    TimingLogger::ScopedSplit split("UnBindBitmaps", &timings_);
    GetHeap()->UnBindBitmaps();
  }
  // Release the memory used by the from space.
  if (kResetFromSpace) {
    // Clearing from space.
    from_space_->Clear();
  }
  // Protect the from space.
  VLOG(heap)
      << "mprotect region " << reinterpret_cast<void*>(from_space_->Begin()) << " - "
      << reinterpret_cast<void*>(from_space_->Limit());
  if (kProtectFromSpace) {
    mprotect(from_space_->Begin(), from_space_->Capacity(), PROT_NONE);
  } else {
    mprotect(from_space_->Begin(), from_space_->Capacity(), PROT_READ);
  }

  if (generational_) {
    // Record the end (top) of the to space so we can distinguish
    // between objects that were allocated since the last GC and the
    // older objects.
    last_gc_to_space_end_ = to_space_->End();
  }
}

void SemiSpace::ResizeMarkStack(size_t new_size) {
  std::vector<Object*> temp(mark_stack_->Begin(), mark_stack_->End());
  CHECK_LE(mark_stack_->Size(), new_size);
  mark_stack_->Resize(new_size);
  for (const auto& obj : temp) {
    mark_stack_->PushBack(obj);
  }
}

inline void SemiSpace::MarkStackPush(Object* obj) {
  if (UNLIKELY(mark_stack_->Size() >= mark_stack_->Capacity())) {
    ResizeMarkStack(mark_stack_->Capacity() * 2);
  }
  // The object must be pushed on to the mark stack.
  mark_stack_->PushBack(obj);
}

// Rare case, probably not worth inlining since it will increase instruction cache miss rate.
bool SemiSpace::MarkLargeObject(const Object* obj) {
  // TODO: support >1 discontinuous space.
  space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  DCHECK(large_object_space->Contains(obj));
  accounting::ObjectSet* large_objects = large_object_space->GetMarkObjects();
  if (UNLIKELY(!large_objects->Test(obj))) {
    large_objects->Set(obj);
    return true;
  }
  return false;
}

mirror::Object* SemiSpace::MarkNonForwardedObject(mirror::Object* obj) {
  size_t object_size = obj->SizeOf();
  size_t bytes_allocated;
  mirror::Object* forward_address = nullptr;
  if (generational_ && reinterpret_cast<byte*>(obj) < last_gc_to_space_end_) {
    // If it's allocated before the last GC (older), move
    // (pseudo-promote) it to the main free list space (as sort
    // of an old generation.)
    size_t bytes_promoted;
    space::MallocSpace* promo_dest_space = GetHeap()->GetPrimaryFreeListSpace();
    forward_address = promo_dest_space->Alloc(self_, object_size, &bytes_promoted);
    if (forward_address == nullptr) {
      // If out of space, fall back to the to-space.
      forward_address = to_space_->Alloc(self_, object_size, &bytes_allocated);
    } else {
      GetHeap()->num_bytes_allocated_.FetchAndAdd(bytes_promoted);
      bytes_promoted_ += bytes_promoted;
      // Handle the bitmaps marking.
      accounting::SpaceBitmap* live_bitmap = promo_dest_space->GetLiveBitmap();
      DCHECK(live_bitmap != nullptr);
      accounting::SpaceBitmap* mark_bitmap = promo_dest_space->GetMarkBitmap();
      DCHECK(mark_bitmap != nullptr);
      DCHECK(!live_bitmap->Test(forward_address));
      if (!whole_heap_collection_) {
        // If collecting the bump pointer spaces only, live_bitmap == mark_bitmap.
        DCHECK_EQ(live_bitmap, mark_bitmap);

        // If a bump pointer space only collection, delay the live
        // bitmap marking of the promoted object until it's popped off
        // the mark stack (ProcessMarkStack()). The rationale: we may
        // be in the middle of scanning the objects in the promo
        // destination space for
        // non-moving-space-to-bump-pointer-space references by
        // iterating over the marked bits of the live bitmap
        // (MarkReachableObjects()). If we don't delay it (and instead
        // mark the promoted object here), the above promo destination
        // space scan could encounter the just-promoted object and
        // forward the references in the promoted object's fields even
        // through it is pushed onto the mark stack. If this happens,
        // the promoted object would be in an inconsistent state, that
        // is, it's on the mark stack (gray) but its fields are
        // already forwarded (black), which would cause a
        // DCHECK(!to_space_->HasAddress(obj)) failure below.
      } else {
        // Mark forward_address on the live bit map.
        live_bitmap->Set(forward_address);
        // Mark forward_address on the mark bit map.
        DCHECK(!mark_bitmap->Test(forward_address));
        mark_bitmap->Set(forward_address);
      }
    }
    DCHECK(forward_address != nullptr);
  } else {
    // If it's allocated after the last GC (younger), copy it to the to-space.
    forward_address = to_space_->Alloc(self_, object_size, &bytes_allocated);
  }
  // Copy over the object and add it to the mark stack since we still need to update its
  // references.
  memcpy(reinterpret_cast<void*>(forward_address), obj, object_size);
  if (to_space_live_bitmap_ != nullptr) {
    to_space_live_bitmap_->Set(forward_address);
  }
  DCHECK(to_space_->HasAddress(forward_address) ||
         (generational_ && GetHeap()->GetPrimaryFreeListSpace()->HasAddress(forward_address)));
  return forward_address;
}

// Used to mark and copy objects. Any newly-marked objects who are in the from space get moved to
// the to-space and have their forward address updated. Objects which have been newly marked are
// pushed on the mark stack.
Object* SemiSpace::MarkObject(Object* obj) {
  Object* forward_address = obj;
  if (obj != nullptr && !IsImmune(obj)) {
    if (from_space_->HasAddress(obj)) {
      forward_address = GetForwardingAddressInFromSpace(obj);
      // If the object has already been moved, return the new forward address.
      if (forward_address == nullptr) {
        forward_address = MarkNonForwardedObject(obj);
        DCHECK(forward_address != nullptr);
        // Make sure to only update the forwarding address AFTER you copy the object so that the
        // monitor word doesn't get stomped over.
        obj->SetLockWord(LockWord::FromForwardingAddress(
            reinterpret_cast<size_t>(forward_address)));
        // Push the object onto the mark stack for later processing.
        MarkStackPush(forward_address);
      }
      // TODO: Do we need this if in the else statement?
    } else {
      accounting::SpaceBitmap* object_bitmap = heap_->GetMarkBitmap()->GetContinuousSpaceBitmap(obj);
      if (LIKELY(object_bitmap != nullptr)) {
        if (generational_) {
          // If a bump pointer space only collection, we should not
          // reach here as we don't/won't mark the objects in the
          // non-moving space (except for the promoted objects.)  Note
          // the non-moving space is added to the immune space.
          DCHECK(whole_heap_collection_);
        }
        // This object was not previously marked.
        if (!object_bitmap->Test(obj)) {
          object_bitmap->Set(obj);
          MarkStackPush(obj);
        }
      } else {
        DCHECK(!to_space_->HasAddress(obj)) << "Marking object in to_space_";
        if (MarkLargeObject(obj)) {
          MarkStackPush(obj);
        }
      }
    }
  }
  return forward_address;
}

Object* SemiSpace::RecursiveMarkObjectCallback(Object* root, void* arg) {
  DCHECK(root != nullptr);
  DCHECK(arg != nullptr);
  SemiSpace* semi_space = reinterpret_cast<SemiSpace*>(arg);
  mirror::Object* ret = semi_space->MarkObject(root);
  semi_space->ProcessMarkStack(true);
  return ret;
}

Object* SemiSpace::MarkRootCallback(Object* root, void* arg) {
  DCHECK(root != nullptr);
  DCHECK(arg != nullptr);
  return reinterpret_cast<SemiSpace*>(arg)->MarkObject(root);
}

// Marks all objects in the root set.
void SemiSpace::MarkRoots() {
  timings_.StartSplit("MarkRoots");
  // TODO: Visit up image roots as well?
  Runtime::Current()->VisitRoots(MarkRootCallback, this, false, true);
  timings_.EndSplit();
}

mirror::Object* SemiSpace::MarkedForwardingAddressCallback(Object* object, void* arg) {
  return reinterpret_cast<SemiSpace*>(arg)->GetMarkedForwardAddress(object);
}

void SemiSpace::SweepSystemWeaks() {
  timings_.StartSplit("SweepSystemWeaks");
  Runtime::Current()->SweepSystemWeaks(MarkedForwardingAddressCallback, this);
  timings_.EndSplit();
}

bool SemiSpace::ShouldSweepSpace(space::ContinuousSpace* space) const {
  return space != from_space_ && space != to_space_ && !IsImmuneSpace(space);
}

void SemiSpace::Sweep(bool swap_bitmaps) {
  DCHECK(mark_stack_->IsEmpty());
  TimingLogger::ScopedSplit("Sweep", &timings_);
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      if (!ShouldSweepSpace(alloc_space)) {
        continue;
      }
      TimingLogger::ScopedSplit split(
          alloc_space->IsZygoteSpace() ? "SweepZygoteSpace" : "SweepAllocSpace", &timings_);
      size_t freed_objects = 0;
      size_t freed_bytes = 0;
      alloc_space->Sweep(swap_bitmaps, &freed_objects, &freed_bytes);
      heap_->RecordFree(freed_objects, freed_bytes);
      freed_objects_.FetchAndAdd(freed_objects);
      freed_bytes_.FetchAndAdd(freed_bytes);
    }
  }
  if (!is_large_object_space_immune_) {
    SweepLargeObjects(swap_bitmaps);
  }
}

void SemiSpace::SweepLargeObjects(bool swap_bitmaps) {
  DCHECK(!is_large_object_space_immune_);
  TimingLogger::ScopedSplit("SweepLargeObjects", &timings_);
  size_t freed_objects = 0;
  size_t freed_bytes = 0;
  GetHeap()->GetLargeObjectsSpace()->Sweep(swap_bitmaps, &freed_objects, &freed_bytes);
  freed_large_objects_.FetchAndAdd(freed_objects);
  freed_large_object_bytes_.FetchAndAdd(freed_bytes);
  GetHeap()->RecordFree(freed_objects, freed_bytes);
}

// Process the "referent" field in a java.lang.ref.Reference.  If the referent has not yet been
// marked, put it on the appropriate list in the heap for later processing.
void SemiSpace::DelayReferenceReferent(mirror::Class* klass, Object* obj) {
  heap_->DelayReferenceReferent(klass, obj, MarkedForwardingAddressCallback, this);
}

// Visit all of the references of an object and update.
void SemiSpace::ScanObject(Object* obj) {
  DCHECK(obj != NULL);
  DCHECK(!from_space_->HasAddress(obj)) << "Scanning object " << obj << " in from space";
  MarkSweep::VisitObjectReferences(obj, [this](Object* obj, Object* ref, const MemberOffset& offset,
     bool /* is_static */) ALWAYS_INLINE_LAMBDA NO_THREAD_SAFETY_ANALYSIS {
    mirror::Object* new_address = MarkObject(ref);
    if (new_address != ref) {
      DCHECK(new_address != nullptr);
      // Don't need to mark the card since we updating the object address and not changing the
      // actual objects its pointing to. Using SetFieldPtr is better in this case since it does not
      // dirty cards and use additional memory.
      obj->SetFieldPtr(offset, new_address, false);
    }
  }, kMovingClasses);
  mirror::Class* klass = obj->GetClass();
  if (UNLIKELY(klass->IsReferenceClass())) {
    DelayReferenceReferent(klass, obj);
  }
}

// Scan anything that's on the mark stack.
void SemiSpace::ProcessMarkStack(bool paused) {
  space::MallocSpace* promo_dest_space = NULL;
  accounting::SpaceBitmap* live_bitmap = NULL;
  if (generational_ && !whole_heap_collection_) {
    // If a bump pointer space only collection (and the promotion is
    // enabled,) we delay the live-bitmap marking of promoted objects
    // from MarkObject() until this function.
    promo_dest_space = GetHeap()->GetPrimaryFreeListSpace();
    live_bitmap = promo_dest_space->GetLiveBitmap();
    DCHECK(live_bitmap != nullptr);
    accounting::SpaceBitmap* mark_bitmap = promo_dest_space->GetMarkBitmap();
    DCHECK(mark_bitmap != nullptr);
    DCHECK_EQ(live_bitmap, mark_bitmap);
  }
  timings_.StartSplit(paused ? "(paused)ProcessMarkStack" : "ProcessMarkStack");
  while (!mark_stack_->IsEmpty()) {
    Object* obj = mark_stack_->PopBack();
    if (generational_ && !whole_heap_collection_ && promo_dest_space->HasAddress(obj)) {
      // obj has just been promoted. Mark the live bitmap for it,
      // which is delayed from MarkObject().
      DCHECK(!live_bitmap->Test(obj));
      live_bitmap->Set(obj);
    }
    ScanObject(obj);
  }
  timings_.EndSplit();
}

inline Object* SemiSpace::GetMarkedForwardAddress(mirror::Object* obj) const
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
  // All immune objects are assumed marked.
  if (IsImmune(obj)) {
    return obj;
  }
  if (from_space_->HasAddress(obj)) {
    mirror::Object* forwarding_address = GetForwardingAddressInFromSpace(const_cast<Object*>(obj));
    return forwarding_address;  // Returns either the forwarding address or nullptr.
  } else if (to_space_->HasAddress(obj)) {
    // Should be unlikely.
    // Already forwarded, must be marked.
    return obj;
  }
  return heap_->GetMarkBitmap()->Test(obj) ? obj : nullptr;
}

void SemiSpace::SetToSpace(space::ContinuousMemMapAllocSpace* to_space) {
  DCHECK(to_space != nullptr);
  to_space_ = to_space;
}

void SemiSpace::SetFromSpace(space::ContinuousMemMapAllocSpace* from_space) {
  DCHECK(from_space != nullptr);
  from_space_ = from_space;
}

void SemiSpace::FinishPhase() {
  TimingLogger::ScopedSplit split("FinishPhase", &timings_);
  Heap* heap = GetHeap();
  timings_.NewSplit("PostGcVerification");
  heap->PostGcVerification(this);

  // Null the "to" and "from" spaces since compacting from one to the other isn't valid until
  // further action is done by the heap.
  to_space_ = nullptr;
  from_space_ = nullptr;

  // Update the cumulative statistics
  total_freed_objects_ += GetFreedObjects() + GetFreedLargeObjects();
  total_freed_bytes_ += GetFreedBytes() + GetFreedLargeObjectBytes();

  // Ensure that the mark stack is empty.
  CHECK(mark_stack_->IsEmpty());

  // Update the cumulative loggers.
  cumulative_timings_.Start();
  cumulative_timings_.AddLogger(timings_);
  cumulative_timings_.End();

  // Clear all of the spaces' mark bitmaps.
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    accounting::SpaceBitmap* bitmap = space->GetMarkBitmap();
    if (bitmap != nullptr &&
        space->GetGcRetentionPolicy() != space::kGcRetentionPolicyNeverCollect) {
      bitmap->Clear();
    }
  }
  mark_stack_->Reset();

  // Reset the marked large objects.
  space::LargeObjectSpace* large_objects = GetHeap()->GetLargeObjectsSpace();
  large_objects->GetMarkObjects()->Clear();

  if (generational_) {
    // Decide whether to do a whole heap collection or a bump pointer
    // only space collection at the next collection by updating
    // whole_heap_collection. Enable whole_heap_collection once every
    // kDefaultWholeHeapCollectionInterval collections.
    if (!whole_heap_collection_) {
      --whole_heap_collection_interval_counter_;
      DCHECK_GE(whole_heap_collection_interval_counter_, 0);
      if (whole_heap_collection_interval_counter_ == 0) {
        whole_heap_collection_ = true;
      }
    } else {
      DCHECK_EQ(whole_heap_collection_interval_counter_, 0);
      whole_heap_collection_interval_counter_ = kDefaultWholeHeapCollectionInterval;
      whole_heap_collection_ = false;
    }
  }
}

}  // namespace collector
}  // namespace gc
}  // namespace art
