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
// TODO: move this to a new file as a new garbage collector?
static constexpr bool kEnableSimplePromo = false;

// TODO: Unduplicate logic.
void SemiSpace::ImmuneSpace(space::ContinuousSpace* space) {
  // Bind live to mark bitmap if necessary.
  if (space->GetLiveBitmap() != space->GetMarkBitmap()) {
    BindLiveToMarkBitmap(space);
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
      immune_end_ = std::max(reinterpret_cast<Object*>(space->End()), immune_end_);
    }
  }
}

void SemiSpace::BindBitmaps() {
  timings_.StartSplit("BindBitmaps");
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  // Mark all of the spaces we never collect as immune.
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyNeverCollect
        || space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect) {
      ImmuneSpace(space);
    }
  }
  timings_.EndSplit();
}

SemiSpace::SemiSpace(Heap* heap, const std::string& name_prefix)
    : GarbageCollector(heap,
                       name_prefix + (name_prefix.empty() ? "" : " ") + "marksweep + semispace"),
      mark_stack_(nullptr),
      immune_begin_(nullptr),
      immune_end_(nullptr),
      to_space_(nullptr),
      from_space_(nullptr),
      soft_reference_list_(nullptr),
      weak_reference_list_(nullptr),
      finalizer_reference_list_(nullptr),
      phantom_reference_list_(nullptr),
      cleared_reference_list_(nullptr),
      self_(nullptr),
      last_gc_to_space_end_(nullptr),
      bytes_promoted_(0) {
}

void SemiSpace::InitializePhase() {
  timings_.Reset();
  TimingLogger::ScopedSplit split("InitializePhase", &timings_);
  mark_stack_ = heap_->mark_stack_.get();
  DCHECK(mark_stack_ != nullptr);
  immune_begin_ = nullptr;
  immune_end_ = nullptr;
  soft_reference_list_ = nullptr;
  weak_reference_list_ = nullptr;
  finalizer_reference_list_ = nullptr;
  phantom_reference_list_ = nullptr;
  cleared_reference_list_ = nullptr;
  self_ = Thread::Current();
  // Do any pre GC verification.
  timings_.NewSplit("PreGcVerification");
  heap_->PreGcVerification(this);
}

void SemiSpace::ProcessReferences(Thread* self) {
  TimingLogger::ScopedSplit split("ProcessReferences", &timings_);
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetHeap()->ProcessReferences(timings_, clear_soft_references_, &MarkedForwardingAddressCallback,
                               &RecursiveMarkObjectCallback, this);
}

void SemiSpace::MarkingPhase() {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  TimingLogger::ScopedSplit split("MarkingPhase", &timings_);
  // Need to do this with mutators paused so that somebody doesn't accidentally allocate into the
  // wrong space.
  heap_->SwapSemiSpaces();
  if (kEnableSimplePromo) {
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
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
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
      CHECK(table != nullptr);
      // TODO: Improve naming.
      TimingLogger::ScopedSplit split(
          space->IsZygoteSpace() ? "UpdateAndMarkZygoteModUnionTable" :
                                   "UpdateAndMarkImageModUnionTable",
                                   &timings_);
      table->UpdateAndMarkReferences(MarkRootCallback, this);
    }
  }
}

void SemiSpace::MarkReachableObjects() {
  timings_.StartSplit("MarkStackAsLive");
  accounting::ObjectStack* live_stack = heap_->GetLiveStack();
  heap_->MarkAllocStackAsLive(live_stack);
  live_stack->Reset();
  timings_.EndSplit();
  // Recursively process the mark stack.
  ProcessMarkStack(true);
}

void SemiSpace::ReclaimPhase() {
  TimingLogger::ScopedSplit split("ReclaimPhase", &timings_);
  Thread* self = Thread::Current();
  ProcessReferences(self);
  {
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    SweepSystemWeaks();
  }
  // Record freed memory.
  int from_bytes = from_space_->GetBytesAllocated();
  int to_bytes = to_space_->GetBytesAllocated();
  int from_objects = from_space_->GetObjectsAllocated();
  int to_objects = to_space_->GetObjectsAllocated();
  int freed_bytes = from_bytes - to_bytes;
  int freed_objects = from_objects - to_objects;
  CHECK_GE(freed_bytes, 0);
  freed_bytes_.fetch_add(freed_bytes);
  freed_objects_.fetch_add(freed_objects);
  heap_->RecordFree(static_cast<size_t>(freed_objects), static_cast<size_t>(freed_bytes));

  timings_.StartSplit("PreSweepingGcVerification");
  heap_->PreSweepingGcVerification(this);
  timings_.EndSplit();

  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // Reclaim unmarked objects.
    Sweep(false);
    // Swap the live and mark bitmaps for each space which we modified space. This is an
    // optimization that enables us to not clear live bits inside of the sweep. Only swaps unbound
    // bitmaps.
    timings_.StartSplit("SwapBitmaps");
    SwapBitmaps();
    timings_.EndSplit();
    // Unbind the live and mark bitmaps.
    UnBindBitmaps();
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

  if (kEnableSimplePromo) {
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
  accounting::SpaceSetMap* large_objects = large_object_space->GetMarkObjects();
  if (UNLIKELY(!large_objects->Test(obj))) {
    large_objects->Set(obj);
    return true;
  }
  return false;
}

// Used to mark and copy objects. Any newly-marked objects who are in the from space get moved to
// the to-space and have their forward address updated. Objects which have been newly marked are
// pushed on the mark stack.
Object* SemiSpace::MarkObject(Object* obj) {
  Object* ret = obj;
  if (obj != nullptr && !IsImmune(obj)) {
    if (from_space_->HasAddress(obj)) {
      mirror::Object* forward_address = GetForwardingAddressInFromSpace(obj);
      // If the object has already been moved, return the new forward address.
      if (forward_address == nullptr) {
        // Otherwise, we need to move the object and add it to the markstack for processing.
        size_t object_size = obj->SizeOf();
        size_t bytes_allocated = 0;
        if (kEnableSimplePromo && reinterpret_cast<byte*>(obj) < last_gc_to_space_end_) {
          // If it's allocated before the last GC (older), move (pseudo-promote) it to
          // the non-moving space (as sort of an old generation.)
          size_t bytes_promoted;
          space::MallocSpace* non_moving_space = GetHeap()->GetNonMovingSpace();
          forward_address = non_moving_space->Alloc(self_, object_size, &bytes_promoted);
          if (forward_address == nullptr) {
            // If out of space, fall back to the to-space.
            forward_address = to_space_->Alloc(self_, object_size, &bytes_allocated);
          } else {
            GetHeap()->num_bytes_allocated_.fetch_add(bytes_promoted);
            bytes_promoted_ += bytes_promoted;
            // Mark forward_address on the live bit map.
            accounting::SpaceBitmap* live_bitmap = non_moving_space->GetLiveBitmap();
            DCHECK(live_bitmap != nullptr);
            DCHECK(!live_bitmap->Test(forward_address));
            live_bitmap->Set(forward_address);
            // Mark forward_address on the mark bit map.
            accounting::SpaceBitmap* mark_bitmap = non_moving_space->GetMarkBitmap();
            DCHECK(mark_bitmap != nullptr);
            DCHECK(!mark_bitmap->Test(forward_address));
            mark_bitmap->Set(forward_address);
          }
          DCHECK(forward_address != nullptr);
        } else {
          // If it's allocated after the last GC (younger), copy it to the to-space.
          forward_address = to_space_->Alloc(self_, object_size, &bytes_allocated);
        }
        // Copy over the object and add it to the mark stack since we still need to update it's
        // references.
        memcpy(reinterpret_cast<void*>(forward_address), obj, object_size);
        // Make sure to only update the forwarding address AFTER you copy the object so that the
        // monitor word doesn't get stomped over.
        obj->SetLockWord(LockWord::FromForwardingAddress(reinterpret_cast<size_t>(forward_address)));
        MarkStackPush(forward_address);
      } else {
        DCHECK(to_space_->HasAddress(forward_address) ||
               (kEnableSimplePromo && GetHeap()->GetNonMovingSpace()->HasAddress(forward_address)));
      }
      ret = forward_address;
      // TODO: Do we need this if in the else statement?
    } else {
      accounting::SpaceBitmap* object_bitmap = heap_->GetMarkBitmap()->GetContinuousSpaceBitmap(obj);
      if (LIKELY(object_bitmap != nullptr)) {
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
  return ret;
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

void SemiSpace::BindLiveToMarkBitmap(space::ContinuousSpace* space) {
  CHECK(space->IsMallocSpace());
  space::MallocSpace* alloc_space = space->AsMallocSpace();
  accounting::SpaceBitmap* live_bitmap = space->GetLiveBitmap();
  accounting::SpaceBitmap* mark_bitmap = alloc_space->BindLiveToMarkBitmap();
  GetHeap()->GetMarkBitmap()->ReplaceBitmap(mark_bitmap, live_bitmap);
}

mirror::Object* SemiSpace::GetForwardingAddress(mirror::Object* obj) {
  if (from_space_->HasAddress(obj)) {
    LOG(FATAL) << "Shouldn't happen!";
    return GetForwardingAddressInFromSpace(obj);
  }
  return obj;
}

mirror::Object* SemiSpace::MarkedForwardingAddressCallback(Object* object, void* arg) {
  return reinterpret_cast<SemiSpace*>(arg)->GetMarkedForwardAddress(object);
}

void SemiSpace::SweepSystemWeaks() {
  timings_.StartSplit("SweepSystemWeaks");
  Runtime::Current()->SweepSystemWeaks(MarkedForwardingAddressCallback, this);
  timings_.EndSplit();
}

struct SweepCallbackContext {
  SemiSpace* mark_sweep;
  space::AllocSpace* space;
  Thread* self;
};

void SemiSpace::SweepCallback(size_t num_ptrs, Object** ptrs, void* arg) {
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  SemiSpace* gc = context->mark_sweep;
  Heap* heap = gc->GetHeap();
  space::AllocSpace* space = context->space;
  Thread* self = context->self;
  Locks::heap_bitmap_lock_->AssertExclusiveHeld(self);
  size_t freed_bytes = space->FreeList(self, num_ptrs, ptrs);
  heap->RecordFree(num_ptrs, freed_bytes);
  gc->freed_objects_.fetch_add(num_ptrs);
  gc->freed_bytes_.fetch_add(freed_bytes);
}

void SemiSpace::ZygoteSweepCallback(size_t num_ptrs, Object** ptrs, void* arg) {
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  Locks::heap_bitmap_lock_->AssertExclusiveHeld(context->self);
  Heap* heap = context->mark_sweep->GetHeap();
  // We don't free any actual memory to avoid dirtying the shared zygote pages.
  for (size_t i = 0; i < num_ptrs; ++i) {
    Object* obj = static_cast<Object*>(ptrs[i]);
    heap->GetLiveBitmap()->Clear(obj);
    heap->GetCardTable()->MarkCard(obj);
  }
}

void SemiSpace::Sweep(bool swap_bitmaps) {
  DCHECK(mark_stack_->IsEmpty());
  TimingLogger::ScopedSplit("Sweep", &timings_);

  const bool partial = (GetGcType() == kGcTypePartial);
  SweepCallbackContext scc;
  scc.mark_sweep = this;
  scc.self = Thread::Current();
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (!space->IsMallocSpace()) {
      continue;
    }
    // We always sweep always collect spaces.
    bool sweep_space = (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect);
    if (!partial && !sweep_space) {
      // We sweep full collect spaces when the GC isn't a partial GC (ie its full).
      sweep_space = (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect);
    }
    if (sweep_space && space->IsMallocSpace()) {
      uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
      uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
      scc.space = space->AsMallocSpace();
      accounting::SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      accounting::SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
      if (swap_bitmaps) {
        std::swap(live_bitmap, mark_bitmap);
      }
      if (!space->IsZygoteSpace()) {
        TimingLogger::ScopedSplit split("SweepAllocSpace", &timings_);
        // Bitmaps are pre-swapped for optimization which enables sweeping with the heap unlocked.
        accounting::SpaceBitmap::SweepWalk(*live_bitmap, *mark_bitmap, begin, end,
                                           &SweepCallback, reinterpret_cast<void*>(&scc));
      } else {
        TimingLogger::ScopedSplit split("SweepZygote", &timings_);
        // Zygote sweep takes care of dirtying cards and clearing live bits, does not free actual
        // memory.
        accounting::SpaceBitmap::SweepWalk(*live_bitmap, *mark_bitmap, begin, end,
                                           &ZygoteSweepCallback, reinterpret_cast<void*>(&scc));
      }
    }
  }

  SweepLargeObjects(swap_bitmaps);
}

void SemiSpace::SweepLargeObjects(bool swap_bitmaps) {
  TimingLogger::ScopedSplit("SweepLargeObjects", &timings_);
  // Sweep large objects
  space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  accounting::SpaceSetMap* large_live_objects = large_object_space->GetLiveObjects();
  accounting::SpaceSetMap* large_mark_objects = large_object_space->GetMarkObjects();
  if (swap_bitmaps) {
    std::swap(large_live_objects, large_mark_objects);
  }
  // O(n*log(n)) but hopefully there are not too many large objects.
  size_t freed_objects = 0;
  size_t freed_bytes = 0;
  Thread* self = Thread::Current();
  for (const Object* obj : large_live_objects->GetObjects()) {
    if (!large_mark_objects->Test(obj)) {
      freed_bytes += large_object_space->Free(self, const_cast<Object*>(obj));
      ++freed_objects;
    }
  }
  freed_large_objects_.fetch_add(freed_objects);
  freed_large_object_bytes_.fetch_add(freed_bytes);
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
  timings_.StartSplit(paused ? "(paused)ProcessMarkStack" : "ProcessMarkStack");
  while (!mark_stack_->IsEmpty()) {
    ScanObject(mark_stack_->PopBack());
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
    // If the object is forwarded then it MUST be marked.
    DCHECK(forwarding_address == nullptr || to_space_->HasAddress(forwarding_address) ||
           (kEnableSimplePromo && GetHeap()->GetNonMovingSpace()->HasAddress(forwarding_address)));
    if (forwarding_address != nullptr) {
      return forwarding_address;
    }
    // Must not be marked, return nullptr;
    return nullptr;
  } else if (to_space_->HasAddress(obj)) {
    // Already forwarded, must be marked.
    return obj;
  }
  return heap_->GetMarkBitmap()->Test(obj) ? obj : nullptr;
}

void SemiSpace::UnBindBitmaps() {
  TimingLogger::ScopedSplit split("UnBindBitmaps", &timings_);
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsMallocSpace()) {
      space::MallocSpace* alloc_space = space->AsMallocSpace();
      if (alloc_space->HasBoundBitmaps()) {
        alloc_space->UnBindBitmaps();
        heap_->GetMarkBitmap()->ReplaceBitmap(alloc_space->GetLiveBitmap(),
                                              alloc_space->GetMarkBitmap());
      }
    }
  }
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
  // Can't enqueue references if we hold the mutator lock.
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
}

}  // namespace collector
}  // namespace gc
}  // namespace art
