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

#include "mark_sweep.h"

#include <functional>
#include <numeric>
#include <climits>
#include <vector>

#include "barrier.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex-inl.h"
#include "base/timing_logger.h"
#include "card_table.h"
#include "card_table-inl.h"
#include "heap.h"
#include "indirect_reference_table.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "large_object_space.h"
#include "monitor.h"
#include "mark_sweep-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "mirror/field.h"
#include "mirror/field-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array.h"
#include "mirror/object_array-inl.h"
#include "runtime.h"
#include "space.h"
#include "space_bitmap-inl.h"
#include "thread.h"
#include "thread_list.h"
#include "verifier/method_verifier.h"

using namespace art::mirror;

namespace art {

// Performance options.
static const bool kParallelMarkStack = true;
static const bool kDisableFinger = kParallelMarkStack;
static const bool kUseMarkStackPrefetch = true;

// Profiling and information flags.
static const bool kCountClassesMarked = false;
static const bool kProfileLargeObjects = false;
static const bool kMeasureOverhead = false;
static const bool kCountTasks = false;
static const bool kCountJavaLangRefs = false;

class SetFingerVisitor {
 public:
  SetFingerVisitor(MarkSweep* const mark_sweep) : mark_sweep_(mark_sweep) {

  }

  void operator ()(void* finger) const {
    mark_sweep_->SetFinger(reinterpret_cast<Object*>(finger));
  }

 private:
  MarkSweep* const mark_sweep_;
};

std::string MarkSweep::GetName() const {
  std::ostringstream ss;
  ss << (IsConcurrent() ? "Concurrent" : "") << GetGcType();
  return ss.str();
}

void MarkSweep::ImmuneSpace(ContinuousSpace* space) {
  // Bind live to mark bitmap if necessary.
  if (space->GetLiveBitmap() != space->GetMarkBitmap()) {
    BindLiveToMarkBitmap(space);
  }

  // Add the space to the immune region.
  if (immune_begin_ == NULL) {
    DCHECK(immune_end_ == NULL);
    SetImmuneRange(reinterpret_cast<Object*>(space->Begin()),
                   reinterpret_cast<Object*>(space->End()));
  } else {
      const Spaces& spaces = GetHeap()->GetSpaces();
      const ContinuousSpace* prev_space = NULL;
      // Find out if the previous space is immune.
      // TODO: C++0x
      for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
        if (*it == space) {
          break;
        }
        prev_space = *it;
      }

      // If previous space was immune, then extend the immune region.
      if (prev_space != NULL &&
          immune_begin_ <= reinterpret_cast<Object*>(prev_space->Begin()) &&
          immune_end_ >= reinterpret_cast<Object*>(prev_space->End())) {
      immune_begin_ = std::min(reinterpret_cast<Object*>(space->Begin()), immune_begin_);
      immune_end_ = std::max(reinterpret_cast<Object*>(space->End()), immune_end_);
    }
  }
}

// Bind the live bits to the mark bits of bitmaps based on the gc type.
void MarkSweep::BindBitmaps() {
  Spaces& spaces = GetHeap()->GetSpaces();
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);

  // Mark all of the spaces we never collect as immune.
  for (Spaces::iterator it = spaces.begin(); it != spaces.end(); ++it) {
    ContinuousSpace* space = *it;
    if (space->GetGcRetentionPolicy() == kGcRetentionPolicyNeverCollect) {
      ImmuneSpace(space);
    }
  }
}

MarkSweep::MarkSweep(Heap* heap, bool is_concurrent)
    : GarbageCollector(heap),
      gc_barrier_(new Barrier(0)),
      large_object_lock_("large object lock"),
      mark_stack_expand_lock_("mark stack expand lock"),
      timings_(GetName(), true),
      cumulative_timings_(GetName()),
      is_concurrent_(is_concurrent) {
  cumulative_timings_.SetName(GetName());
  ResetCumulativeStatistics();
}

void MarkSweep::InitializePhase() {
  mark_stack_ = GetHeap()->mark_stack_.get();
  DCHECK(mark_stack_ != NULL);
  finger_ = NULL;
  SetImmuneRange(NULL, NULL);
  soft_reference_list_ = NULL;
  weak_reference_list_ = NULL;
  finalizer_reference_list_ = NULL;
  phantom_reference_list_ = NULL;
  cleared_reference_list_ = NULL;
  freed_bytes_ = 0;
  freed_objects_ = 0;
  class_count_ = 0;
  array_count_ = 0;
  other_count_ = 0;
  large_object_test_ = 0;
  large_object_mark_ = 0;
  classes_marked_ = 0;
  overhead_time_ = 0;
  work_chunks_created_ = 0;
  work_chunks_deleted_ = 0;
  reference_count_ = 0;
  java_lang_Class_ = Class::GetJavaLangClass();
  CHECK(java_lang_Class_ != NULL);
  FindDefaultMarkBitmap();
  // Mark any concurrent roots as dirty since we need to scan them at least once during this GC.
  Runtime::Current()->DirtyRoots();
  timings_.Reset();
  // Do any pre GC verification.
  heap_->PreGcVerification(this);
}

void MarkSweep::ProcessReferences(Thread* self) {
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  ProcessReferences(&soft_reference_list_, clear_soft_references_, &weak_reference_list_,
                    &finalizer_reference_list_, &phantom_reference_list_);
  timings_.AddSplit("ProcessReferences");
}

bool MarkSweep::HandleDirtyObjectsPhase() {
  Thread* self = Thread::Current();
  ObjectStack* allocation_stack = GetHeap()->allocation_stack_.get();
  Locks::mutator_lock_->AssertExclusiveHeld(self);

  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);

    // Re-mark root set.
    ReMarkRoots();
    timings_.AddSplit("ReMarkRoots");

    // Scan dirty objects, this is only required if we are not doing concurrent GC.
    RecursiveMarkDirtyObjects(CardTable::kCardDirty);
  }

  ProcessReferences(self);

  // Only need to do this if we have the card mark verification on, and only during concurrent GC.
  if (GetHeap()->verify_missing_card_marks_) {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // This second sweep makes sure that we don't have any objects in the live stack which point to
    // freed objects. These cause problems since their references may be previously freed objects.
    SweepArray(timings_, allocation_stack, false);
  } else {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // We only sweep over the live stack, and the live stack should not intersect with the
    // allocation stack, so it should be safe to UnMark anything in the allocation stack as live.
    heap_->UnMarkAllocStack(GetHeap()->alloc_space_->GetMarkBitmap(),
                           GetHeap()->large_object_space_->GetMarkObjects(),
                           allocation_stack);
    timings_.AddSplit("UnMarkAllocStack");
  }
  return true;
}

bool MarkSweep::IsConcurrent() const {
  return is_concurrent_;
}

void MarkSweep::MarkingPhase() {
  Heap* heap = GetHeap();
  Thread* self = Thread::Current();

  BindBitmaps();
  FindDefaultMarkBitmap();
  timings_.AddSplit("BindBitmaps");

  // Process dirty cards and add dirty cards to mod union tables.
  heap->ProcessCards(timings_);

  // Need to do this before the checkpoint since we don't want any threads to add references to
  // the live stack during the recursive mark.
  heap->SwapStacks();
  timings_.AddSplit("SwapStacks");

  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // If we exclusively hold the mutator lock, all threads must be suspended.
    MarkRoots();
    timings_.AddSplit("MarkConcurrentRoots");
  } else {
    MarkRootsCheckpoint();
    timings_.AddSplit("MarkRootsCheckpoint");
    MarkNonThreadRoots();
    timings_.AddSplit("MarkNonThreadRoots");
  }
  MarkConcurrentRoots();
  timings_.AddSplit("MarkConcurrentRoots");

  heap->UpdateAndMarkModUnion(this, timings_, GetGcType());
  MarkReachableObjects();
}

void MarkSweep::MarkReachableObjects() {
  // Mark everything allocated since the last as GC live so that we can sweep concurrently,
  // knowing that new allocations won't be marked as live.
  ObjectStack* live_stack = heap_->GetLiveStack();
  heap_->MarkAllocStack(heap_->alloc_space_->GetLiveBitmap(),
                       heap_->large_object_space_->GetLiveObjects(),
                       live_stack);
  live_stack->Reset();
  timings_.AddSplit("MarkStackAsLive");
  // Recursively mark all the non-image bits set in the mark bitmap.
  RecursiveMark();
  DisableFinger();
}

void MarkSweep::ReclaimPhase() {
  Thread* self = Thread::Current();

  if (!IsConcurrent()) {
    ProcessReferences(self);
  }

  // Before freeing anything, lets verify the heap.
  if (kIsDebugBuild) {
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    VerifyImageRoots();
  }
  heap_->PreSweepingGcVerification(this);

  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);

    // Reclaim unmarked objects.
    Sweep(timings_, false);

    // Swap the live and mark bitmaps for each space which we modified space. This is an
    // optimization that enables us to not clear live bits inside of the sweep. Only swaps unbound
    // bitmaps.
    SwapBitmaps();
    timings_.AddSplit("SwapBitmaps");

    // Unbind the live and mark bitmaps.
    UnBindBitmaps();
  }
}

void MarkSweep::SwapBitmaps() {
  // Swap the live and mark bitmaps for each alloc space. This is needed since sweep re-swaps
  // these bitmaps. The bitmap swapping is an optimization so that we do not need to clear the live
  // bits of dead objects in the live bitmap.
  const GcType gc_type = GetGcType();
  // TODO: C++0x
  Spaces& spaces = heap_->GetSpaces();
  for (Spaces::iterator it = spaces.begin(); it != spaces.end(); ++it) {
    ContinuousSpace* space = *it;
    // We never allocate into zygote spaces.
    if (space->GetGcRetentionPolicy() == kGcRetentionPolicyAlwaysCollect ||
        (gc_type == kGcTypeFull &&
            space->GetGcRetentionPolicy() == kGcRetentionPolicyFullCollect)) {
      SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
      if (live_bitmap != mark_bitmap) {
        heap_->GetLiveBitmap()->ReplaceBitmap(live_bitmap, mark_bitmap);
        heap_->GetMarkBitmap()->ReplaceBitmap(mark_bitmap, live_bitmap);
        space->AsAllocSpace()->SwapBitmaps();
      }
    }
  }
  SwapLargeObjects();
}

void MarkSweep::SwapLargeObjects() {
  LargeObjectSpace* large_object_space = heap_->GetLargeObjectsSpace();
  large_object_space->SwapBitmaps();
  heap_->GetLiveBitmap()->SetLargeObjects(large_object_space->GetLiveObjects());
  heap_->GetMarkBitmap()->SetLargeObjects(large_object_space->GetMarkObjects());
}

void MarkSweep::SetImmuneRange(Object* begin, Object* end) {
  immune_begin_ = begin;
  immune_end_ = end;
}

void MarkSweep::FindDefaultMarkBitmap() {
  const Spaces& spaces = heap_->GetSpaces();
  for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
    if ((*it)->GetGcRetentionPolicy() == kGcRetentionPolicyAlwaysCollect) {
      current_mark_bitmap_ = (*it)->GetMarkBitmap();
      CHECK(current_mark_bitmap_ != NULL);
      return;
    }
  }
  GetHeap()->DumpSpaces();
  LOG(FATAL) << "Could not find a default mark bitmap";
}

void MarkSweep::ExpandMarkStack() {
  // Rare case, no need to have Thread::Current be a parameter.
  MutexLock mu(Thread::Current(), mark_stack_expand_lock_);
  if (UNLIKELY(mark_stack_->Size() < mark_stack_->Capacity())) {
    // Someone else acquired the lock and expanded the mark stack before us.
    return;
  }
  std::vector<Object*> temp;
  temp.insert(temp.begin(), mark_stack_->Begin(), mark_stack_->End());
  mark_stack_->Resize(mark_stack_->Capacity() * 2);
  for (size_t i = 0; i < temp.size(); ++i) {
    mark_stack_->PushBack(temp[i]);
  }
}

inline void MarkSweep::MarkObjectNonNullParallel(const Object* obj, bool check_finger) {
  DCHECK(obj != NULL);
  if (MarkObjectParallel(obj)) {
    if (kDisableFinger || (check_finger && obj < finger_)) {
      while (UNLIKELY(!mark_stack_->AtomicPushBack(const_cast<Object*>(obj)))) {
        // Only reason a push can fail is that the mark stack is full.
        ExpandMarkStack();
      }
    }
  }
}

inline void MarkSweep::MarkObjectNonNull(const Object* obj, bool check_finger) {
  DCHECK(obj != NULL);

  if (obj >= immune_begin_ && obj < immune_end_) {
    DCHECK(IsMarked(obj));
    return;
  }

  // Try to take advantage of locality of references within a space, failing this find the space
  // the hard way.
  SpaceBitmap* object_bitmap = current_mark_bitmap_;
  if (UNLIKELY(!object_bitmap->HasAddress(obj))) {
    SpaceBitmap* new_bitmap = heap_->GetMarkBitmap()->GetSpaceBitmap(obj);
    if (new_bitmap != NULL) {
      object_bitmap = new_bitmap;
    } else {
      MarkLargeObject(obj);
      return;
    }
  }

  // This object was not previously marked.
  if (!object_bitmap->Test(obj)) {
    object_bitmap->Set(obj);
    if (kDisableFinger || (check_finger && obj < finger_)) {
      // Do we need to expand the mark stack?
      if (UNLIKELY(mark_stack_->Size() >= mark_stack_->Capacity())) {
        ExpandMarkStack();
      }
      // The object must be pushed on to the mark stack.
      mark_stack_->PushBack(const_cast<Object*>(obj));
    }
  }
}

// Rare case, probably not worth inlining since it will increase instruction cache miss rate.
bool MarkSweep::MarkLargeObject(const Object* obj) {
  LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  SpaceSetMap* large_objects = large_object_space->GetMarkObjects();
  if (kProfileLargeObjects) {
    ++large_object_test_;
  }
  if (UNLIKELY(!large_objects->Test(obj))) {
    if (!large_object_space->Contains(obj)) {
      LOG(ERROR) << "Tried to mark " << obj << " not contained by any spaces";
      LOG(ERROR) << "Attempting see if it's a bad root";
      VerifyRoots();
      LOG(FATAL) << "Can't mark bad root";
    }
    if (kProfileLargeObjects) {
      ++large_object_mark_;
    }
    large_objects->Set(obj);
    // Don't need to check finger since large objects never have any object references.
    return true;
  }
  return false;
}

inline bool MarkSweep::MarkObjectParallel(const Object* obj) {
  DCHECK(obj != NULL);

  if (obj >= immune_begin_ && obj < immune_end_) {
    DCHECK(IsMarked(obj));
    return false;
  }

  // Try to take advantage of locality of references within a space, failing this find the space
  // the hard way.
  SpaceBitmap* object_bitmap = current_mark_bitmap_;
  if (UNLIKELY(!object_bitmap->HasAddress(obj))) {
    SpaceBitmap* new_bitmap = heap_->GetMarkBitmap()->GetSpaceBitmap(obj);
    if (new_bitmap != NULL) {
      object_bitmap = new_bitmap;
    } else {
      // TODO: Remove the Thread::Current here?
      // TODO: Convert this to some kind of atomic marking?
      MutexLock mu(Thread::Current(), large_object_lock_);
      return MarkLargeObject(obj);
    }
  }

  // Return true if the object was not previously marked.
  return !object_bitmap->AtomicTestAndSet(obj);
}

// Used to mark objects when recursing.  Recursion is done by moving
// the finger across the bitmaps in address order and marking child
// objects.  Any newly-marked objects whose addresses are lower than
// the finger won't be visited by the bitmap scan, so those objects
// need to be added to the mark stack.
void MarkSweep::MarkObject(const Object* obj) {
  if (obj != NULL) {
    MarkObjectNonNull(obj, true);
  }
}

void MarkSweep::MarkRoot(const Object* obj) {
  if (obj != NULL) {
    MarkObjectNonNull(obj, false);
  }
}

void MarkSweep::MarkRootParallelCallback(const Object* root, void* arg) {
  DCHECK(root != NULL);
  DCHECK(arg != NULL);
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->MarkObjectNonNullParallel(root, false);
}

void MarkSweep::MarkObjectCallback(const Object* root, void* arg) {
  DCHECK(root != NULL);
  DCHECK(arg != NULL);
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->MarkObjectNonNull(root, false);
}

void MarkSweep::ReMarkObjectVisitor(const Object* root, void* arg) {
  DCHECK(root != NULL);
  DCHECK(arg != NULL);
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->MarkObjectNonNull(root, true);
}

void MarkSweep::VerifyRootCallback(const Object* root, void* arg, size_t vreg,
                                   const StackVisitor* visitor) {
  reinterpret_cast<MarkSweep*>(arg)->VerifyRoot(root, vreg, visitor);
}

void MarkSweep::VerifyRoot(const Object* root, size_t vreg, const StackVisitor* visitor) {
  // See if the root is on any space bitmap.
  if (GetHeap()->GetLiveBitmap()->GetSpaceBitmap(root) == NULL) {
    LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
    if (!large_object_space->Contains(root)) {
      LOG(ERROR) << "Found invalid root: " << root;
      if (visitor != NULL) {
        LOG(ERROR) << visitor->DescribeLocation() << " in VReg: " << vreg;
      }
    }
  }
}

void MarkSweep::VerifyRoots() {
  Runtime::Current()->GetThreadList()->VerifyRoots(VerifyRootCallback, this);
}

// Marks all objects in the root set.
void MarkSweep::MarkRoots() {
  Runtime::Current()->VisitNonConcurrentRoots(MarkObjectCallback, this);
}

void MarkSweep::MarkNonThreadRoots() {
  Runtime::Current()->VisitNonThreadRoots(MarkObjectCallback, this);
}

void MarkSweep::MarkConcurrentRoots() {
  Runtime::Current()->VisitConcurrentRoots(MarkObjectCallback, this);
}

class CheckObjectVisitor {
 public:
  CheckObjectVisitor(MarkSweep* const mark_sweep)
      : mark_sweep_(mark_sweep) {

  }

  void operator ()(const Object* obj, const Object* ref, MemberOffset offset, bool is_static) const
      NO_THREAD_SAFETY_ANALYSIS {
    if (kDebugLocking) {
      Locks::heap_bitmap_lock_->AssertSharedHeld(Thread::Current());
    }
    mark_sweep_->CheckReference(obj, ref, offset, is_static);
  }

 private:
  MarkSweep* const mark_sweep_;
};

void MarkSweep::CheckObject(const Object* obj) {
  DCHECK(obj != NULL);
  CheckObjectVisitor visitor(this);
  VisitObjectReferences(obj, visitor);
}

void MarkSweep::VerifyImageRootVisitor(Object* root, void* arg) {
  DCHECK(root != NULL);
  DCHECK(arg != NULL);
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  DCHECK(mark_sweep->heap_->GetMarkBitmap()->Test(root));
  mark_sweep->CheckObject(root);
}

void MarkSweep::BindLiveToMarkBitmap(ContinuousSpace* space) {
  CHECK(space->IsAllocSpace());
  DlMallocSpace* alloc_space = space->AsAllocSpace();
  SpaceBitmap* live_bitmap = space->GetLiveBitmap();
  SpaceBitmap* mark_bitmap = alloc_space->mark_bitmap_.release();
  GetHeap()->GetMarkBitmap()->ReplaceBitmap(mark_bitmap, live_bitmap);
  alloc_space->temp_bitmap_.reset(mark_bitmap);
  alloc_space->mark_bitmap_.reset(live_bitmap);
}

class ScanObjectVisitor {
 public:
  ScanObjectVisitor(MarkSweep* const mark_sweep) : mark_sweep_(mark_sweep) {

  }

  // TODO: Fixme when anotatalysis works with visitors.
  void operator ()(const Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    if (kDebugLocking) {
      Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    mark_sweep_->ScanObject(obj);
  }

 private:
  MarkSweep* const mark_sweep_;
};

void MarkSweep::ScanGrayObjects(byte minimum_age) {
  const Spaces& spaces = heap_->GetSpaces();
  CardTable* card_table = heap_->GetCardTable();
  ScanObjectVisitor visitor(this);
  SetFingerVisitor finger_visitor(this);
  // TODO: C++ 0x auto
  for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
    ContinuousSpace* space = *it;
    byte* begin = space->Begin();
    byte* end = space->End();
    // Image spaces are handled properly since live == marked for them.
    SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    card_table->Scan(mark_bitmap, begin, end, visitor, VoidFunctor(), minimum_age);
  }
}

class CheckBitmapVisitor {
 public:
  CheckBitmapVisitor(MarkSweep* mark_sweep) : mark_sweep_(mark_sweep) {

  }

  void operator ()(const Object* obj) const
      NO_THREAD_SAFETY_ANALYSIS {
    if (kDebugLocking) {
      Locks::heap_bitmap_lock_->AssertSharedHeld(Thread::Current());
    }
    DCHECK(obj != NULL);
    mark_sweep_->CheckObject(obj);
  }

 private:
  MarkSweep* mark_sweep_;
};

void MarkSweep::VerifyImageRoots() {
  // Verify roots ensures that all the references inside the image space point
  // objects which are either in the image space or marked objects in the alloc
  // space
  CheckBitmapVisitor visitor(this);
  const Spaces& spaces = heap_->GetSpaces();
  for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
    if ((*it)->IsImageSpace()) {
      ImageSpace* space = (*it)->AsImageSpace();
      uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
      uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
      SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      DCHECK(live_bitmap != NULL);
      live_bitmap->VisitMarkedRange(begin, end, visitor, VoidFunctor());
    }
  }
}

// Populates the mark stack based on the set of marked objects and
// recursively marks until the mark stack is emptied.
void MarkSweep::RecursiveMark() {
  // RecursiveMark will build the lists of known instances of the Reference classes.
  // See DelayReferenceReferent for details.
  CHECK(soft_reference_list_ == NULL);
  CHECK(weak_reference_list_ == NULL);
  CHECK(finalizer_reference_list_ == NULL);
  CHECK(phantom_reference_list_ == NULL);
  CHECK(cleared_reference_list_ == NULL);

  const bool partial = GetGcType() == kGcTypePartial;
  const Spaces& spaces = heap_->GetSpaces();
  SetFingerVisitor set_finger_visitor(this);
  ScanObjectVisitor scan_visitor(this);
  if (!kDisableFinger) {
    finger_ = NULL;
    for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
      ContinuousSpace* space = *it;
      if ((space->GetGcRetentionPolicy() == kGcRetentionPolicyAlwaysCollect) ||
          (!partial && space->GetGcRetentionPolicy() == kGcRetentionPolicyFullCollect)
          ) {
        current_mark_bitmap_ = space->GetMarkBitmap();
        if (current_mark_bitmap_ == NULL) {
          GetHeap()->DumpSpaces();
          LOG(FATAL) << "invalid bitmap";
        }
        // This function does not handle heap end increasing, so we must use the space end.
        uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
        uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
        current_mark_bitmap_->VisitMarkedRange(begin, end, scan_visitor, set_finger_visitor);
      }
    }
  }
  DisableFinger();
  timings_.AddSplit("RecursiveMark");
  ProcessMarkStack();
  timings_.AddSplit("ProcessMarkStack");
}

bool MarkSweep::IsMarkedCallback(const Object* object, void* arg) {
  return
      reinterpret_cast<MarkSweep*>(arg)->IsMarked(object) ||
      !reinterpret_cast<MarkSweep*>(arg)->GetHeap()->GetLiveBitmap()->Test(object);
}

void MarkSweep::RecursiveMarkDirtyObjects(byte minimum_age) {
  ScanGrayObjects(minimum_age);
  timings_.AddSplit("ScanGrayObjects");
  ProcessMarkStack();
  timings_.AddSplit("ProcessMarkStack");
}

void MarkSweep::ReMarkRoots() {
  Runtime::Current()->VisitRoots(ReMarkObjectVisitor, this);
}

void MarkSweep::SweepJniWeakGlobals(IsMarkedTester is_marked, void* arg) {
  JavaVMExt* vm = Runtime::Current()->GetJavaVM();
  MutexLock mu(Thread::Current(), vm->weak_globals_lock);
  IndirectReferenceTable* table = &vm->weak_globals;
  typedef IndirectReferenceTable::iterator It;  // TODO: C++0x auto
  for (It it = table->begin(), end = table->end(); it != end; ++it) {
    const Object** entry = *it;
    if (!is_marked(*entry, arg)) {
      *entry = kClearedJniWeakGlobal;
    }
  }
}

struct ArrayMarkedCheck {
  ObjectStack* live_stack;
  MarkSweep* mark_sweep;
};

// Either marked or not live.
bool MarkSweep::IsMarkedArrayCallback(const Object* object, void* arg) {
  ArrayMarkedCheck* array_check = reinterpret_cast<ArrayMarkedCheck*>(arg);
  if (array_check->mark_sweep->IsMarked(object)) {
    return true;
  }
  ObjectStack* live_stack = array_check->live_stack;
  return std::find(live_stack->Begin(), live_stack->End(), object) == live_stack->End();
}

void MarkSweep::SweepSystemWeaksArray(ObjectStack* allocations) {
  Runtime* runtime = Runtime::Current();
  // The callbacks check
  // !is_marked where is_marked is the callback but we want
  // !IsMarked && IsLive
  // So compute !(!IsMarked && IsLive) which is equal to (IsMarked || !IsLive).
  // Or for swapped (IsLive || !IsMarked).

  ArrayMarkedCheck visitor;
  visitor.live_stack = allocations;
  visitor.mark_sweep = this;
  runtime->GetInternTable()->SweepInternTableWeaks(IsMarkedArrayCallback, &visitor);
  runtime->GetMonitorList()->SweepMonitorList(IsMarkedArrayCallback, &visitor);
  SweepJniWeakGlobals(IsMarkedArrayCallback, &visitor);
}

void MarkSweep::SweepSystemWeaks() {
  Runtime* runtime = Runtime::Current();
  // The callbacks check
  // !is_marked where is_marked is the callback but we want
  // !IsMarked && IsLive
  // So compute !(!IsMarked && IsLive) which is equal to (IsMarked || !IsLive).
  // Or for swapped (IsLive || !IsMarked).
  runtime->GetInternTable()->SweepInternTableWeaks(IsMarkedCallback, this);
  runtime->GetMonitorList()->SweepMonitorList(IsMarkedCallback, this);
  SweepJniWeakGlobals(IsMarkedCallback, this);
}

bool MarkSweep::VerifyIsLiveCallback(const Object* obj, void* arg) {
  reinterpret_cast<MarkSweep*>(arg)->VerifyIsLive(obj);
  // We don't actually want to sweep the object, so lets return "marked"
  return true;
}

void MarkSweep::VerifyIsLive(const Object* obj) {
  Heap* heap = GetHeap();
  if (!heap->GetLiveBitmap()->Test(obj)) {
    LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
    if (!large_object_space->GetLiveObjects()->Test(obj)) {
      if (std::find(heap->allocation_stack_->Begin(), heap->allocation_stack_->End(), obj) ==
          heap->allocation_stack_->End()) {
        // Object not found!
        heap->DumpSpaces();
        LOG(FATAL) << "Found dead object " << obj;
      }
    }
  }
}

void MarkSweep::VerifySystemWeaks() {
  Runtime* runtime = Runtime::Current();
  // Verify system weaks, uses a special IsMarked callback which always returns true.
  runtime->GetInternTable()->SweepInternTableWeaks(VerifyIsLiveCallback, this);
  runtime->GetMonitorList()->SweepMonitorList(VerifyIsLiveCallback, this);

  JavaVMExt* vm = runtime->GetJavaVM();
  MutexLock mu(Thread::Current(), vm->weak_globals_lock);
  IndirectReferenceTable* table = &vm->weak_globals;
  typedef IndirectReferenceTable::iterator It;  // TODO: C++0x auto
  for (It it = table->begin(), end = table->end(); it != end; ++it) {
    const Object** entry = *it;
    VerifyIsLive(*entry);
  }
}

struct SweepCallbackContext {
  MarkSweep* mark_sweep;
  AllocSpace* space;
  Thread* self;
};

class CheckpointMarkThreadRoots : public Closure {
 public:
  CheckpointMarkThreadRoots(MarkSweep* mark_sweep) : mark_sweep_(mark_sweep) {

  }

  virtual void Run(Thread* thread) NO_THREAD_SAFETY_ANALYSIS {
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* self = Thread::Current();
    CHECK(thread == self || thread->IsSuspended() || thread->GetState() == kWaitingPerformingGc)
        << thread->GetState() << " thread " << thread << " self " << self;
    thread->VisitRoots(MarkSweep::MarkRootParallelCallback, mark_sweep_);
    mark_sweep_->GetBarrier().Pass(self);
  }

 private:
  MarkSweep* mark_sweep_;
};

void MarkSweep::ResetCumulativeStatistics() {
  cumulative_timings_.Reset();
  total_time_ = 0;
  total_paused_time_ = 0;
  total_freed_objects_ = 0;
  total_freed_bytes_ = 0;
}

void MarkSweep::MarkRootsCheckpoint() {
  CheckpointMarkThreadRoots check_point(this);
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  // Increment the count of the barrier. If all of the checkpoints have already been finished then
  // will hit 0 and continue. Otherwise we are still waiting for some checkpoints, so the counter
  // will go positive and we will unblock when it hits zero.
  gc_barrier_->Increment(Thread::Current(), thread_list->RunCheckpoint(&check_point));
}

void MarkSweep::SweepCallback(size_t num_ptrs, Object** ptrs, void* arg) {
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  MarkSweep* mark_sweep = context->mark_sweep;
  Heap* heap = mark_sweep->GetHeap();
  AllocSpace* space = context->space;
  Thread* self = context->self;
  Locks::heap_bitmap_lock_->AssertExclusiveHeld(self);
  // Use a bulk free, that merges consecutive objects before freeing or free per object?
  // Documentation suggests better free performance with merging, but this may be at the expensive
  // of allocation.
  size_t freed_objects = num_ptrs;
  // AllocSpace::FreeList clears the value in ptrs, so perform after clearing the live bit
  size_t freed_bytes = space->FreeList(self, num_ptrs, ptrs);
  heap->RecordFree(freed_objects, freed_bytes);
  mark_sweep->freed_objects_ += freed_objects;
  mark_sweep->freed_bytes_ += freed_bytes;
}

void MarkSweep::ZygoteSweepCallback(size_t num_ptrs, Object** ptrs, void* arg) {
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

void MarkSweep::SweepArray(TimingLogger& logger, ObjectStack* allocations, bool swap_bitmaps) {
  size_t freed_bytes = 0;
  DlMallocSpace* space = heap_->GetAllocSpace();

  // If we don't swap bitmaps then newly allocated Weaks go into the live bitmap but not mark
  // bitmap, resulting in occasional frees of Weaks which are still in use.
  SweepSystemWeaksArray(allocations);
  logger.AddSplit("SweepSystemWeaks");

  // Newly allocated objects MUST be in the alloc space and those are the only objects which we are
  // going to free.
  SpaceBitmap* live_bitmap = space->GetLiveBitmap();
  SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
  LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  SpaceSetMap* large_live_objects = large_object_space->GetLiveObjects();
  SpaceSetMap* large_mark_objects = large_object_space->GetMarkObjects();
  if (swap_bitmaps) {
    std::swap(live_bitmap, mark_bitmap);
    std::swap(large_live_objects, large_mark_objects);
  }

  size_t freed_large_objects = 0;
  size_t count = allocations->Size();
  Object** objects = const_cast<Object**>(allocations->Begin());
  Object** out = objects;

  // Empty the allocation stack.
  Thread* self = Thread::Current();
  for (size_t i = 0;i < count;++i) {
    Object* obj = objects[i];
    // There should only be objects in the AllocSpace/LargeObjectSpace in the allocation stack.
    if (LIKELY(mark_bitmap->HasAddress(obj))) {
      if (!mark_bitmap->Test(obj)) {
        // Don't bother un-marking since we clear the mark bitmap anyways.
        *(out++) = obj;
      }
    } else if (!large_mark_objects->Test(obj)) {
      ++freed_large_objects;
      freed_bytes += large_object_space->Free(self, obj);
    }
  }
  logger.AddSplit("Process allocation stack");

  size_t freed_objects = out - objects;
  freed_bytes += space->FreeList(self, freed_objects, objects);
  VLOG(heap) << "Freed " << freed_objects << "/" << count
             << " objects with size " << PrettySize(freed_bytes);
  heap_->RecordFree(freed_objects + freed_large_objects, freed_bytes);
  freed_objects_ += freed_objects;
  freed_bytes_ += freed_bytes;
  logger.AddSplit("FreeList");
  allocations->Reset();
  logger.AddSplit("ResetStack");
}

void MarkSweep::Sweep(TimingLogger& timings, bool swap_bitmaps) {
  DCHECK(mark_stack_->IsEmpty());

  // If we don't swap bitmaps then newly allocated Weaks go into the live bitmap but not mark
  // bitmap, resulting in occasional frees of Weaks which are still in use.
  SweepSystemWeaks();
  timings.AddSplit("SweepSystemWeaks");

  const bool partial = GetGcType() == kGcTypePartial;
  const Spaces& spaces = heap_->GetSpaces();
  SweepCallbackContext scc;
  scc.mark_sweep = this;
  scc.self = Thread::Current();
  // TODO: C++0x auto
  for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
    ContinuousSpace* space = *it;
    if (
        space->GetGcRetentionPolicy() == kGcRetentionPolicyAlwaysCollect ||
        (!partial && space->GetGcRetentionPolicy() == kGcRetentionPolicyFullCollect)
        ) {
      uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
      uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
      scc.space = space->AsAllocSpace();
      SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
      if (swap_bitmaps) {
        std::swap(live_bitmap, mark_bitmap);
      }
      if (space->GetGcRetentionPolicy() == kGcRetentionPolicyAlwaysCollect) {
        // Bitmaps are pre-swapped for optimization which enables sweeping with the heap unlocked.
        SpaceBitmap::SweepWalk(*live_bitmap, *mark_bitmap, begin, end,
                               &SweepCallback, reinterpret_cast<void*>(&scc));
      } else {
        // Zygote sweep takes care of dirtying cards and clearing live bits, does not free actual memory.
        SpaceBitmap::SweepWalk(*live_bitmap, *mark_bitmap, begin, end,
                               &ZygoteSweepCallback, reinterpret_cast<void*>(&scc));
      }
    }
  }
  timings.AddSplit("Sweep");

  SweepLargeObjects(swap_bitmaps);
  timings.AddSplit("SweepLargeObjects");
}

void MarkSweep::SweepLargeObjects(bool swap_bitmaps) {
  // Sweep large objects
  LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  SpaceSetMap* large_live_objects = large_object_space->GetLiveObjects();
  SpaceSetMap* large_mark_objects = large_object_space->GetMarkObjects();
  if (swap_bitmaps) {
    std::swap(large_live_objects, large_mark_objects);
  }
  SpaceSetMap::Objects& live_objects = large_live_objects->GetObjects();
  // O(n*log(n)) but hopefully there are not too many large objects.
  size_t freed_objects = 0;
  size_t freed_bytes = 0;
  // TODO: C++0x
  Thread* self = Thread::Current();
  for (SpaceSetMap::Objects::iterator it = live_objects.begin(); it != live_objects.end(); ++it) {
    if (!large_mark_objects->Test(*it)) {
      freed_bytes += large_object_space->Free(self, const_cast<Object*>(*it));
      ++freed_objects;
    }
  }
  freed_objects_ += freed_objects;
  freed_bytes_ += freed_bytes;
  // Large objects don't count towards bytes_allocated.
  GetHeap()->RecordFree(freed_objects, freed_bytes);
}

void MarkSweep::CheckReference(const Object* obj, const Object* ref, MemberOffset offset, bool is_static) {
  const Spaces& spaces = heap_->GetSpaces();
  // TODO: C++0x auto
  for (Spaces::const_iterator cur = spaces.begin(); cur != spaces.end(); ++cur) {
    if ((*cur)->IsAllocSpace() && (*cur)->Contains(ref)) {
      DCHECK(IsMarked(obj));

      bool is_marked = IsMarked(ref);
      if (!is_marked) {
        LOG(INFO) << **cur;
        LOG(WARNING) << (is_static ? "Static ref'" : "Instance ref'") << PrettyTypeOf(ref)
                     << "' (" << reinterpret_cast<const void*>(ref) << ") in '" << PrettyTypeOf(obj)
                     << "' (" << reinterpret_cast<const void*>(obj) << ") at offset "
                     << reinterpret_cast<void*>(offset.Int32Value()) << " wasn't marked";

        const Class* klass = is_static ? obj->AsClass() : obj->GetClass();
        DCHECK(klass != NULL);
        const ObjectArray<Field>* fields = is_static ? klass->GetSFields() : klass->GetIFields();
        DCHECK(fields != NULL);
        bool found = false;
        for (int32_t i = 0; i < fields->GetLength(); ++i) {
          const Field* cur = fields->Get(i);
          if (cur->GetOffset().Int32Value() == offset.Int32Value()) {
            LOG(WARNING) << "Field referencing the alloc space was " << PrettyField(cur);
            found = true;
            break;
          }
        }
        if (!found) {
          LOG(WARNING) << "Could not find field in object alloc space with offset " << offset.Int32Value();
        }

        bool obj_marked = heap_->GetCardTable()->IsDirty(obj);
        if (!obj_marked) {
          LOG(WARNING) << "Object '" << PrettyTypeOf(obj) << "' "
                       << "(" << reinterpret_cast<const void*>(obj) << ") contains references to "
                       << "the alloc space, but wasn't card marked";
        }
      }
    }
    break;
  }
}

// Process the "referent" field in a java.lang.ref.Reference.  If the
// referent has not yet been marked, put it on the appropriate list in
// the gcHeap for later processing.
void MarkSweep::DelayReferenceReferent(Object* obj) {
  DCHECK(obj != NULL);
  Class* klass = obj->GetClass();
  DCHECK(klass != NULL);
  DCHECK(klass->IsReferenceClass());
  Object* pending = obj->GetFieldObject<Object*>(heap_->GetReferencePendingNextOffset(), false);
  Object* referent = heap_->GetReferenceReferent(obj);
  if (kCountJavaLangRefs) {
    ++reference_count_;
  }
  if (pending == NULL && referent != NULL && !IsMarked(referent)) {
    Object** list = NULL;
    if (klass->IsSoftReferenceClass()) {
      list = &soft_reference_list_;
    } else if (klass->IsWeakReferenceClass()) {
      list = &weak_reference_list_;
    } else if (klass->IsFinalizerReferenceClass()) {
      list = &finalizer_reference_list_;
    } else if (klass->IsPhantomReferenceClass()) {
      list = &phantom_reference_list_;
    }
    DCHECK(list != NULL) << PrettyClass(klass) << " " << std::hex << klass->GetAccessFlags();
    // TODO: One lock per list?
    heap_->EnqueuePendingReference(obj, list);
  }
}

void MarkSweep::ScanRoot(const Object* obj) {
  ScanObject(obj);
}

class MarkObjectVisitor {
 public:
  MarkObjectVisitor(MarkSweep* const mark_sweep) : mark_sweep_(mark_sweep) {
  }

  // TODO: Fixme when anotatalysis works with visitors.
  void operator ()(const Object* /* obj */, const Object* ref, const MemberOffset& /* offset */,
                   bool /* is_static */) const
      NO_THREAD_SAFETY_ANALYSIS {
    if (kDebugLocking) {
      Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    mark_sweep_->MarkObject(ref);
  }

 private:
  MarkSweep* const mark_sweep_;
};

// Scans an object reference.  Determines the type of the reference
// and dispatches to a specialized scanning routine.
void MarkSweep::ScanObject(const Object* obj) {
  MarkObjectVisitor visitor(this);
  ScanObjectVisit(obj, visitor);
}

class MarkStackChunk : public Task {
public:
  MarkStackChunk(ThreadPool* thread_pool, MarkSweep* mark_sweep, Object** begin, Object** end)
      : mark_sweep_(mark_sweep),
        thread_pool_(thread_pool),
        index_(0),
        length_(0),
        output_(NULL) {
    length_ = end - begin;
    if (begin != end) {
      // Cost not significant since we only do this for the initial set of mark stack chunks.
      memcpy(data_, begin, length_ * sizeof(*begin));
    }
    if (kCountTasks) {
      ++mark_sweep_->work_chunks_created_;
    }
  }

  ~MarkStackChunk() {
    DCHECK(output_ == NULL || output_->length_ == 0);
    DCHECK_GE(index_, length_);
    delete output_;
    if (kCountTasks) {
      ++mark_sweep_->work_chunks_deleted_;
    }
  }

  MarkSweep* const mark_sweep_;
  ThreadPool* const thread_pool_;
  static const size_t max_size = 1 * KB;
  // Index of which object we are scanning. Only needs to be atomic if we are doing work stealing.
  size_t index_;
  // Input / output mark stack. We add newly marked references to data_ until length reaches
  // max_size. This is an optimization so that less tasks are created.
  // TODO: Investigate using a bounded buffer FIFO.
  Object* data_[max_size];
  // How many elements in data_ we need to scan.
  size_t length_;
  // Output block, newly marked references get added to the ouput block so that another thread can
  // scan them.
  MarkStackChunk* output_;

  class MarkObjectParallelVisitor {
   public:
    MarkObjectParallelVisitor(MarkStackChunk* chunk_task) : chunk_task_(chunk_task) {

    }

    void operator ()(const Object* /* obj */, const Object* ref,
                     const MemberOffset& /* offset */, bool /* is_static */) const {
      if (ref != NULL && chunk_task_->mark_sweep_->MarkObjectParallel(ref)) {
        chunk_task_->MarkStackPush(ref);
      }
    }

   private:
    MarkStackChunk* const chunk_task_;
  };

  // Push an object into the block.
  // Don't need to use atomic ++ since we only one thread is writing to an output block at any
  // given time.
  void Push(Object* obj) {
    data_[length_++] = obj;
  }

  void MarkStackPush(const Object* obj) {
    if (static_cast<size_t>(length_) < max_size) {
      Push(const_cast<Object*>(obj));
    } else {
      // Internal buffer is full, push to a new buffer instead.
      if (UNLIKELY(output_ == NULL)) {
        AllocateOutputChunk();
      } else if (UNLIKELY(static_cast<size_t>(output_->length_) == max_size)) {
        // Output block is full, queue it up for processing and obtain a new block.
        EnqueueOutput();
        AllocateOutputChunk();
      }
      output_->Push(const_cast<Object*>(obj));
    }
  }

  void ScanObject(Object* obj) {
    mark_sweep_->ScanObjectVisit(obj, MarkObjectParallelVisitor(this));
  }

  void EnqueueOutput() {
    if (output_ != NULL) {
      uint64_t start = 0;
      if (kMeasureOverhead) {
        start = NanoTime();
      }
      thread_pool_->AddTask(Thread::Current(), output_);
      output_ = NULL;
      if (kMeasureOverhead) {
        mark_sweep_->overhead_time_ += NanoTime() - start;
      }
    }
  }

  void AllocateOutputChunk() {
    uint64_t start = 0;
    if (kMeasureOverhead) {
      start = NanoTime();
    }
    output_ = new MarkStackChunk(thread_pool_, mark_sweep_, NULL, NULL);
    if (kMeasureOverhead) {
      mark_sweep_->overhead_time_ += NanoTime() - start;
    }
  }

  void Finalize() {
    EnqueueOutput();
    delete this;
  }

  // Scans all of the objects
  virtual void Run(Thread* self) {
    int index;
    while ((index = index_++) < length_) {
      if (kUseMarkStackPrefetch) {
        static const size_t prefetch_look_ahead = 1;
        __builtin_prefetch(data_[std::min(index + prefetch_look_ahead, length_ - 1)]);
      }
      Object* obj = data_[index];
      DCHECK(obj != NULL);
      ScanObject(obj);
    }
  }
};

void MarkSweep::ProcessMarkStackParallel() {
  CHECK(kDisableFinger) << "parallel mark stack processing cannot work when finger is enabled";
  Thread* self = Thread::Current();
  ThreadPool* thread_pool = GetHeap()->GetThreadPool();
  // Split the current mark stack up into work tasks.
  const size_t num_threads = thread_pool->GetThreadCount();
  const size_t stack_size = mark_stack_->Size();
  const size_t chunk_size =
      std::min((stack_size + num_threads - 1) / num_threads,
               static_cast<size_t>(MarkStackChunk::max_size));
  size_t index = 0;
  for (size_t i = 0; i < num_threads || index < stack_size; ++i) {
    Object** begin = &mark_stack_->Begin()[std::min(stack_size, index)];
    Object** end = &mark_stack_->Begin()[std::min(stack_size, index + chunk_size)];
    index += chunk_size;
    thread_pool->AddTask(self, new MarkStackChunk(thread_pool, this, begin, end));
  }
  thread_pool->StartWorkers(self);
  mark_stack_->Reset();
  thread_pool->Wait(self, true);
  //LOG(INFO) << "Idle wait time " << PrettyDuration(thread_pool->GetWaitTime());
  CHECK_EQ(work_chunks_created_, work_chunks_deleted_) << " some of the work chunks were leaked";
}

// Scan anything that's on the mark stack.
void MarkSweep::ProcessMarkStack() {
  ThreadPool* thread_pool = GetHeap()->GetThreadPool();
  if (kParallelMarkStack && thread_pool != NULL && thread_pool->GetThreadCount() > 0) {
    ProcessMarkStackParallel();
    return;
  }

  if (kUseMarkStackPrefetch) {
    const size_t fifo_size = 4;
    const size_t fifo_mask = fifo_size - 1;
    const Object* fifo[fifo_size];
    for (size_t i = 0;i < fifo_size;++i) {
      fifo[i] = NULL;
    }
    size_t fifo_pos = 0;
    size_t fifo_count = 0;
    for (;;) {
      const Object* obj = fifo[fifo_pos & fifo_mask];
      if (obj != NULL) {
        ScanObject(obj);
        fifo[fifo_pos & fifo_mask] = NULL;
        --fifo_count;
      }

      if (!mark_stack_->IsEmpty()) {
        const Object* obj = mark_stack_->PopBack();
        DCHECK(obj != NULL);
        fifo[fifo_pos & fifo_mask] = obj;
        __builtin_prefetch(obj);
        fifo_count++;
      }
      fifo_pos++;

      if (!fifo_count) {
        CHECK(mark_stack_->IsEmpty()) << mark_stack_->Size();
        break;
      }
    }
  } else {
    while (!mark_stack_->IsEmpty()) {
      const Object* obj = mark_stack_->PopBack();
      DCHECK(obj != NULL);
      ScanObject(obj);
    }
  }
}

// Walks the reference list marking any references subject to the
// reference clearing policy.  References with a black referent are
// removed from the list.  References with white referents biased
// toward saving are blackened and also removed from the list.
void MarkSweep::PreserveSomeSoftReferences(Object** list) {
  DCHECK(list != NULL);
  Object* clear = NULL;
  size_t counter = 0;

  DCHECK(mark_stack_->IsEmpty());

  while (*list != NULL) {
    Object* ref = heap_->DequeuePendingReference(list);
    Object* referent = heap_->GetReferenceReferent(ref);
    if (referent == NULL) {
      // Referent was cleared by the user during marking.
      continue;
    }
    bool is_marked = IsMarked(referent);
    if (!is_marked && ((++counter) & 1)) {
      // Referent is white and biased toward saving, mark it.
      MarkObject(referent);
      is_marked = true;
    }
    if (!is_marked) {
      // Referent is white, queue it for clearing.
      heap_->EnqueuePendingReference(ref, &clear);
    }
  }
  *list = clear;
  // Restart the mark with the newly black references added to the
  // root set.
  ProcessMarkStack();
}

inline bool MarkSweep::IsMarked(const Object* object) const
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
  if (object >= immune_begin_ && object < immune_end_) {
    return true;
  }
  DCHECK(current_mark_bitmap_ != NULL);
  if (current_mark_bitmap_->HasAddress(object)) {
    return current_mark_bitmap_->Test(object);
  }
  return heap_->GetMarkBitmap()->Test(object);
}


// Unlink the reference list clearing references objects with white
// referents.  Cleared references registered to a reference queue are
// scheduled for appending by the heap worker thread.
void MarkSweep::ClearWhiteReferences(Object** list) {
  DCHECK(list != NULL);
  while (*list != NULL) {
    Object* ref = heap_->DequeuePendingReference(list);
    Object* referent = heap_->GetReferenceReferent(ref);
    if (referent != NULL && !IsMarked(referent)) {
      // Referent is white, clear it.
      heap_->ClearReferenceReferent(ref);
      if (heap_->IsEnqueuable(ref)) {
        heap_->EnqueueReference(ref, &cleared_reference_list_);
      }
    }
  }
  DCHECK(*list == NULL);
}

// Enqueues finalizer references with white referents.  White
// referents are blackened, moved to the zombie field, and the
// referent field is cleared.
void MarkSweep::EnqueueFinalizerReferences(Object** list) {
  DCHECK(list != NULL);
  MemberOffset zombie_offset = heap_->GetFinalizerReferenceZombieOffset();
  bool has_enqueued = false;
  while (*list != NULL) {
    Object* ref = heap_->DequeuePendingReference(list);
    Object* referent = heap_->GetReferenceReferent(ref);
    if (referent != NULL && !IsMarked(referent)) {
      MarkObject(referent);
      // If the referent is non-null the reference must queuable.
      DCHECK(heap_->IsEnqueuable(ref));
      ref->SetFieldObject(zombie_offset, referent, false);
      heap_->ClearReferenceReferent(ref);
      heap_->EnqueueReference(ref, &cleared_reference_list_);
      has_enqueued = true;
    }
  }
  if (has_enqueued) {
    ProcessMarkStack();
  }
  DCHECK(*list == NULL);
}

// Process reference class instances and schedule finalizations.
void MarkSweep::ProcessReferences(Object** soft_references, bool clear_soft,
                                  Object** weak_references,
                                  Object** finalizer_references,
                                  Object** phantom_references) {
  DCHECK(soft_references != NULL);
  DCHECK(weak_references != NULL);
  DCHECK(finalizer_references != NULL);
  DCHECK(phantom_references != NULL);

  // Unless we are in the zygote or required to clear soft references
  // with white references, preserve some white referents.
  if (!clear_soft && !Runtime::Current()->IsZygote()) {
    PreserveSomeSoftReferences(soft_references);
  }

  // Clear all remaining soft and weak references with white
  // referents.
  ClearWhiteReferences(soft_references);
  ClearWhiteReferences(weak_references);

  // Preserve all white objects with finalize methods and schedule
  // them for finalization.
  EnqueueFinalizerReferences(finalizer_references);

  // Clear all f-reachable soft and weak references with white
  // referents.
  ClearWhiteReferences(soft_references);
  ClearWhiteReferences(weak_references);

  // Clear all phantom references with white referents.
  ClearWhiteReferences(phantom_references);

  // At this point all reference lists should be empty.
  DCHECK(*soft_references == NULL);
  DCHECK(*weak_references == NULL);
  DCHECK(*finalizer_references == NULL);
  DCHECK(*phantom_references == NULL);
}

void MarkSweep::UnBindBitmaps() {
  const Spaces& spaces = heap_->GetSpaces();
  // TODO: C++0x auto
  for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
    Space* space = *it;
    if (space->IsAllocSpace()) {
      DlMallocSpace* alloc_space = space->AsAllocSpace();
      if (alloc_space->temp_bitmap_.get() != NULL) {
        // At this point, the temp_bitmap holds our old mark bitmap.
        SpaceBitmap* new_bitmap = alloc_space->temp_bitmap_.release();
        GetHeap()->GetMarkBitmap()->ReplaceBitmap(alloc_space->mark_bitmap_.get(), new_bitmap);
        CHECK_EQ(alloc_space->mark_bitmap_.release(), alloc_space->live_bitmap_.get());
        alloc_space->mark_bitmap_.reset(new_bitmap);
        DCHECK(alloc_space->temp_bitmap_.get() == NULL);
      }
    }
  }
}

void MarkSweep::FinishPhase() {
  // Can't enqueue referneces if we hold the mutator lock.
  Object* cleared_references = GetClearedReferences();
  heap_->EnqueueClearedReferences(&cleared_references);

  heap_->PostGcVerification(this);

  heap_->GrowForUtilization(GetDuration());
  timings_.AddSplit("GrowForUtilization");

  heap_->RequestHeapTrim();
  timings_.AddSplit("RequestHeapTrim");

  // Update the cumulative statistics
  total_time_ += GetDuration();
  total_paused_time_ += std::accumulate(GetPauseTimes().begin(), GetPauseTimes().end(), 0,
                                        std::plus<uint64_t>());
  total_freed_objects_ += GetFreedObjects();
  total_freed_bytes_ += GetFreedBytes();

  // Ensure that the mark stack is empty.
  CHECK(mark_stack_->IsEmpty());

  if (kCountScannedTypes) {
    VLOG(gc) << "MarkSweep scanned classes=" << class_count_ << " arrays=" << array_count_
             << " other=" << other_count_;
  }

  if (kCountTasks) {
    VLOG(gc) << "Total number of work chunks allocated: " << work_chunks_created_;
  }

  if (kMeasureOverhead) {
    VLOG(gc) << "Overhead time " << PrettyDuration(overhead_time_);
  }

  if (kProfileLargeObjects) {
    VLOG(gc) << "Large objects tested " << large_object_test_ << " marked " << large_object_mark_;
  }

  if (kCountClassesMarked) {
    VLOG(gc) << "Classes marked " << classes_marked_;
  }

  if (kCountJavaLangRefs) {
    VLOG(gc) << "References scanned " << reference_count_;
  }

  // Update the cumulative loggers.
  cumulative_timings_.Start();
  cumulative_timings_.AddLogger(timings_);
  cumulative_timings_.End();

  // Clear all of the spaces' mark bitmaps.
  const Spaces& spaces = heap_->GetSpaces();
  // TODO: C++0x auto
  for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
    ContinuousSpace* space = *it;
    if (space->GetGcRetentionPolicy() != kGcRetentionPolicyNeverCollect) {
      space->GetMarkBitmap()->Clear();
    }
  }
  mark_stack_->Reset();

  // Reset the marked large objects.
  LargeObjectSpace* large_objects = GetHeap()->GetLargeObjectsSpace();
  large_objects->GetMarkObjects()->Clear();
}

MarkSweep::~MarkSweep() {

}

}  // namespace art
