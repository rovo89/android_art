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

#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex-inl.h"
#include "base/timing_logger.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "indirect_reference_table.h"
#include "intern_table.h"
#include "jni_internal.h"
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
#include "thread-inl.h"
#include "thread_list.h"
#include "verifier/method_verifier.h"

using namespace art::mirror;

namespace art {
namespace gc {
namespace collector {

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

void MarkSweep::ImmuneSpace(space::ContinuousSpace* space) {
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
    const std::vector<space::ContinuousSpace*>& spaces = GetHeap()->GetContinuousSpaces();
    const space::ContinuousSpace* prev_space = NULL;
    // Find out if the previous space is immune.
    // TODO: C++0x
    typedef std::vector<space::ContinuousSpace*>::const_iterator It;
    for (It it = spaces.begin(), end = spaces.end(); it != end; ++it) {
      if (*it == space) {
        break;
      }
      prev_space = *it;
    }

    // If previous space was immune, then extend the immune region. Relies on continuous spaces
    // being sorted by Heap::AddContinuousSpace.
    if (prev_space != NULL &&
        immune_begin_ <= reinterpret_cast<Object*>(prev_space->Begin()) &&
        immune_end_ >= reinterpret_cast<Object*>(prev_space->End())) {
      immune_begin_ = std::min(reinterpret_cast<Object*>(space->Begin()), immune_begin_);
      immune_end_ = std::max(reinterpret_cast<Object*>(space->End()), immune_end_);
    }
  }
}

void MarkSweep::BindBitmaps() {
  const std::vector<space::ContinuousSpace*>& spaces = GetHeap()->GetContinuousSpaces();
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);

  // Mark all of the spaces we never collect as immune.
  typedef std::vector<space::ContinuousSpace*>::const_iterator It;
  for (It it = spaces.begin(), end = spaces.end(); it != end; ++it) {
    space::ContinuousSpace* space = *it;
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyNeverCollect) {
      ImmuneSpace(space);
    }
  }
}

MarkSweep::MarkSweep(Heap* heap, bool is_concurrent, const std::string& name_prefix)
    : GarbageCollector(heap,
                       name_prefix + (name_prefix.empty() ? "" : " ") +
                       (is_concurrent ? "concurrent mark sweep": "mark sweep")),
      current_mark_bitmap_(NULL),
      java_lang_Class_(NULL),
      mark_stack_(NULL),
      finger_(NULL),
      immune_begin_(NULL),
      immune_end_(NULL),
      soft_reference_list_(NULL),
      weak_reference_list_(NULL),
      finalizer_reference_list_(NULL),
      phantom_reference_list_(NULL),
      cleared_reference_list_(NULL),
      gc_barrier_(new Barrier(0)),
      large_object_lock_("mark sweep large object lock", kMarkSweepLargeObjectLock),
      mark_stack_expand_lock_("mark sweep mark stack expand lock"),
      is_concurrent_(is_concurrent),
      clear_soft_references_(false) {
}

void MarkSweep::InitializePhase() {
  timings_.Reset();
  timings_.StartSplit("InitializePhase");
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
  // Do any pre GC verification.
  heap_->PreGcVerification(this);
}

void MarkSweep::ProcessReferences(Thread* self) {
  timings_.NewSplit("ProcessReferences");
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  ProcessReferences(&soft_reference_list_, clear_soft_references_, &weak_reference_list_,
                    &finalizer_reference_list_, &phantom_reference_list_);
}

bool MarkSweep::HandleDirtyObjectsPhase() {
  Thread* self = Thread::Current();
  accounting::ObjectStack* allocation_stack = GetHeap()->allocation_stack_.get();
  Locks::mutator_lock_->AssertExclusiveHeld(self);

  {
    timings_.NewSplit("ReMarkRoots");
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);

    // Re-mark root set.
    ReMarkRoots();

    // Scan dirty objects, this is only required if we are not doing concurrent GC.
    RecursiveMarkDirtyObjects(accounting::CardTable::kCardDirty);
  }

  ProcessReferences(self);

  // Only need to do this if we have the card mark verification on, and only during concurrent GC.
  if (GetHeap()->verify_missing_card_marks_) {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // This second sweep makes sure that we don't have any objects in the live stack which point to
    // freed objects. These cause problems since their references may be previously freed objects.
    SweepArray(allocation_stack, false);
  } else {
    timings_.NewSplit("UnMarkAllocStack");
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // The allocation stack contains things allocated since the start of the GC. These may have been
    // marked during this GC meaning they won't be eligible for reclaiming in the next sticky GC.
    // Remove these objects from the mark bitmaps so that they will be eligible for sticky
    // collection.
    heap_->UnMarkAllocStack(GetHeap()->alloc_space_->GetMarkBitmap(),
                            GetHeap()->large_object_space_->GetMarkObjects(),
                            allocation_stack);
  }
  return true;
}

bool MarkSweep::IsConcurrent() const {
  return is_concurrent_;
}

void MarkSweep::MarkingPhase() {
  Heap* heap = GetHeap();
  Thread* self = Thread::Current();

  timings_.NewSplit("BindBitmaps");
  BindBitmaps();
  FindDefaultMarkBitmap();
  // Process dirty cards and add dirty cards to mod union tables.
  heap->ProcessCards(timings_);

  // Need to do this before the checkpoint since we don't want any threads to add references to
  // the live stack during the recursive mark.
  timings_.NewSplit("SwapStacks");
  heap->SwapStacks();

  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // If we exclusively hold the mutator lock, all threads must be suspended.
    timings_.NewSplit("MarkRoots");
    MarkRoots();
  } else {
    timings_.NewSplit("MarkRootsCheckpoint");
    MarkRootsCheckpoint(self);
    timings_.NewSplit("MarkNonThreadRoots");
    MarkNonThreadRoots();
  }
  timings_.NewSplit("MarkConcurrentRoots");
  MarkConcurrentRoots();

  heap->UpdateAndMarkModUnion(this, timings_, GetGcType());
  MarkReachableObjects();
}

void MarkSweep::MarkReachableObjects() {
  // Mark everything allocated since the last as GC live so that we can sweep concurrently,
  // knowing that new allocations won't be marked as live.
  timings_.NewSplit("MarkStackAsLive");
  accounting::ObjectStack* live_stack = heap_->GetLiveStack();
  heap_->MarkAllocStack(heap_->alloc_space_->GetLiveBitmap(),
                       heap_->large_object_space_->GetLiveObjects(),
                       live_stack);
  live_stack->Reset();
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
    Sweep(false);

    // Swap the live and mark bitmaps for each space which we modified space. This is an
    // optimization that enables us to not clear live bits inside of the sweep. Only swaps unbound
    // bitmaps.
    timings_.NewSplit("SwapBitmaps");
    SwapBitmaps();

    // Unbind the live and mark bitmaps.
    UnBindBitmaps();
  }
}

void MarkSweep::SetImmuneRange(Object* begin, Object* end) {
  immune_begin_ = begin;
  immune_end_ = end;
}

void MarkSweep::FindDefaultMarkBitmap() {
  const std::vector<space::ContinuousSpace*>& spaces = GetHeap()->GetContinuousSpaces();
  // TODO: C++0x
  typedef std::vector<space::ContinuousSpace*>::const_iterator It;
  for (It it = spaces.begin(), end = spaces.end(); it != end; ++it) {
    space::ContinuousSpace* space = *it;
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect) {
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
  accounting::SpaceBitmap* object_bitmap = current_mark_bitmap_;
  if (UNLIKELY(!object_bitmap->HasAddress(obj))) {
    accounting::SpaceBitmap* new_bitmap = heap_->GetMarkBitmap()->GetContinuousSpaceBitmap(obj);
    if (LIKELY(new_bitmap != NULL)) {
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
  // TODO: support >1 discontinuous space.
  space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  accounting::SpaceSetMap* large_objects = large_object_space->GetMarkObjects();
  if (kProfileLargeObjects) {
    ++large_object_test_;
  }
  if (UNLIKELY(!large_objects->Test(obj))) {
    // TODO: mark may be called holding the JNI global references lock, Contains will hold the
    // large object space lock causing a lock level violation. Bug: 9414652;
    if (!kDebugLocking && !large_object_space->Contains(obj)) {
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
  accounting::SpaceBitmap* object_bitmap = current_mark_bitmap_;
  if (UNLIKELY(!object_bitmap->HasAddress(obj))) {
    accounting::SpaceBitmap* new_bitmap = heap_->GetMarkBitmap()->GetContinuousSpaceBitmap(obj);
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
  if (GetHeap()->GetLiveBitmap()->GetContinuousSpaceBitmap(root) == NULL) {
    space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
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
  // Visit all runtime roots and clear dirty flags.
  Runtime::Current()->VisitConcurrentRoots(MarkObjectCallback, this, false, true);
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

void MarkSweep::BindLiveToMarkBitmap(space::ContinuousSpace* space) {
  CHECK(space->IsDlMallocSpace());
  space::DlMallocSpace* alloc_space = space->AsDlMallocSpace();
  accounting::SpaceBitmap* live_bitmap = space->GetLiveBitmap();
  accounting::SpaceBitmap* mark_bitmap = alloc_space->mark_bitmap_.release();
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
  accounting::CardTable* card_table = GetHeap()->GetCardTable();
  const std::vector<space::ContinuousSpace*>& spaces = GetHeap()->GetContinuousSpaces();
  ScanObjectVisitor visitor(this);
  SetFingerVisitor finger_visitor(this);
  // TODO: C++0x
  typedef std::vector<space::ContinuousSpace*>::const_iterator It;
  for (It it = spaces.begin(), space_end = spaces.end(); it != space_end; ++it) {
    space::ContinuousSpace* space = *it;
    switch (space->GetGcRetentionPolicy()) {
      case space::kGcRetentionPolicyNeverCollect:
        timings_.NewSplit("ScanGrayImageSpaceObjects");
        break;
      case space::kGcRetentionPolicyFullCollect:
        timings_.NewSplit("ScanGrayZygoteSpaceObjects");
        break;
      case space::kGcRetentionPolicyAlwaysCollect:
        timings_.NewSplit("ScanGrayAllocSpaceObjects");
        break;
    }
    byte* begin = space->Begin();
    byte* end = space->End();
    // Image spaces are handled properly since live == marked for them.
    accounting::SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    card_table->Scan(mark_bitmap, begin, end, visitor, finger_visitor, minimum_age);
  }
}

class CheckBitmapVisitor {
 public:
  CheckBitmapVisitor(MarkSweep* mark_sweep) : mark_sweep_(mark_sweep) {
  }

  void operator ()(const Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
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
  const std::vector<space::ContinuousSpace*>& spaces = GetHeap()->GetContinuousSpaces();
  // TODO: C++0x
  typedef std::vector<space::ContinuousSpace*>::const_iterator It;
  for (It it = spaces.begin(), end = spaces.end(); it != end; ++it) {
    if ((*it)->IsImageSpace()) {
      space::ImageSpace* space = (*it)->AsImageSpace();
      uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
      uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
      accounting::SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      DCHECK(live_bitmap != NULL);
      live_bitmap->VisitMarkedRange(begin, end, visitor, VoidFunctor());
    }
  }
}

// Populates the mark stack based on the set of marked objects and
// recursively marks until the mark stack is emptied.
void MarkSweep::RecursiveMark() {
  timings_.NewSplit("RecursiveMark");
  // RecursiveMark will build the lists of known instances of the Reference classes.
  // See DelayReferenceReferent for details.
  CHECK(soft_reference_list_ == NULL);
  CHECK(weak_reference_list_ == NULL);
  CHECK(finalizer_reference_list_ == NULL);
  CHECK(phantom_reference_list_ == NULL);
  CHECK(cleared_reference_list_ == NULL);

  const bool partial = GetGcType() == kGcTypePartial;
  SetFingerVisitor set_finger_visitor(this);
  ScanObjectVisitor scan_visitor(this);
  if (!kDisableFinger) {
    finger_ = NULL;
    const std::vector<space::ContinuousSpace*>& spaces = GetHeap()->GetContinuousSpaces();
    // TODO: C++0x
    typedef std::vector<space::ContinuousSpace*>::const_iterator It;
    for (It it = spaces.begin(), end = spaces.end(); it != end; ++it) {
      space::ContinuousSpace* space = *it;
      if ((space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect) ||
          (!partial && space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect)) {
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
  timings_.NewSplit("ProcessMarkStack");
  ProcessMarkStack();
}

bool MarkSweep::IsMarkedCallback(const Object* object, void* arg) {
  return
      reinterpret_cast<MarkSweep*>(arg)->IsMarked(object) ||
      !reinterpret_cast<MarkSweep*>(arg)->GetHeap()->GetLiveBitmap()->Test(object);
}

void MarkSweep::RecursiveMarkDirtyObjects(byte minimum_age) {
  ScanGrayObjects(minimum_age);
  timings_.NewSplit("ProcessMarkStack");
  ProcessMarkStack();
}

void MarkSweep::ReMarkRoots() {
  Runtime::Current()->VisitRoots(ReMarkObjectVisitor, this, true, true);
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
  accounting::ObjectStack* live_stack;
  MarkSweep* mark_sweep;
};

// Either marked or not live.
bool MarkSweep::IsMarkedArrayCallback(const Object* object, void* arg) {
  ArrayMarkedCheck* array_check = reinterpret_cast<ArrayMarkedCheck*>(arg);
  if (array_check->mark_sweep->IsMarked(object)) {
    return true;
  }
  accounting::ObjectStack* live_stack = array_check->live_stack;
  return std::find(live_stack->Begin(), live_stack->End(), object) == live_stack->End();
}

void MarkSweep::SweepSystemWeaksArray(accounting::ObjectStack* allocations) {
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
    space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
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
  space::AllocSpace* space;
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

void MarkSweep::MarkRootsCheckpoint(Thread* self) {
  CheckpointMarkThreadRoots check_point(this);
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  // Request the check point is run on all threads returning a count of the threads that must
  // run through the barrier including self.
  size_t barrier_count = thread_list->RunCheckpoint(&check_point);
  // Release locks then wait for all mutator threads to pass the barrier.
  // TODO: optimize to not release locks when there are no threads to wait for.
  Locks::heap_bitmap_lock_->ExclusiveUnlock(self);
  Locks::mutator_lock_->SharedUnlock(self);
  ThreadState old_state = self->SetState(kWaitingForCheckPointsToRun);
  CHECK_EQ(old_state, kWaitingPerformingGc);
  gc_barrier_->Increment(self, barrier_count);
  self->SetState(kWaitingPerformingGc);
  Locks::mutator_lock_->SharedLock(self);
  Locks::heap_bitmap_lock_->ExclusiveLock(self);
}

void MarkSweep::SweepCallback(size_t num_ptrs, Object** ptrs, void* arg) {
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  MarkSweep* mark_sweep = context->mark_sweep;
  Heap* heap = mark_sweep->GetHeap();
  space::AllocSpace* space = context->space;
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

void MarkSweep::SweepArray(accounting::ObjectStack* allocations, bool swap_bitmaps) {
  size_t freed_bytes = 0;
  space::DlMallocSpace* space = heap_->GetAllocSpace();

  // If we don't swap bitmaps then newly allocated Weaks go into the live bitmap but not mark
  // bitmap, resulting in occasional frees of Weaks which are still in use.
  timings_.NewSplit("SweepSystemWeaks");
  SweepSystemWeaksArray(allocations);

  timings_.NewSplit("Process allocation stack");
  // Newly allocated objects MUST be in the alloc space and those are the only objects which we are
  // going to free.
  accounting::SpaceBitmap* live_bitmap = space->GetLiveBitmap();
  accounting::SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
  space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  accounting::SpaceSetMap* large_live_objects = large_object_space->GetLiveObjects();
  accounting::SpaceSetMap* large_mark_objects = large_object_space->GetMarkObjects();
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
  CHECK_EQ(count, allocations->Size());
  timings_.NewSplit("FreeList");

  size_t freed_objects = out - objects;
  freed_bytes += space->FreeList(self, freed_objects, objects);
  VLOG(heap) << "Freed " << freed_objects << "/" << count
             << " objects with size " << PrettySize(freed_bytes);
  heap_->RecordFree(freed_objects + freed_large_objects, freed_bytes);
  freed_objects_ += freed_objects;
  freed_bytes_ += freed_bytes;

  timings_.NewSplit("ResetStack");
  allocations->Reset();
}

void MarkSweep::Sweep(bool swap_bitmaps) {
  DCHECK(mark_stack_->IsEmpty());

  // If we don't swap bitmaps then newly allocated Weaks go into the live bitmap but not mark
  // bitmap, resulting in occasional frees of Weaks which are still in use.
  timings_.NewSplit("SweepSystemWeaks");
  SweepSystemWeaks();

  const bool partial = (GetGcType() == kGcTypePartial);
  SweepCallbackContext scc;
  scc.mark_sweep = this;
  scc.self = Thread::Current();
  const std::vector<space::ContinuousSpace*>& spaces = GetHeap()->GetContinuousSpaces();
  // TODO: C++0x
  typedef std::vector<space::ContinuousSpace*>::const_iterator It;
  for (It it = spaces.begin(), end = spaces.end(); it != end; ++it) {
    space::ContinuousSpace* space = *it;
    // We always sweep always collect spaces.
    bool sweep_space = (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect);
    if (!partial && !sweep_space) {
      // We sweep full collect spaces when the GC isn't a partial GC (ie its full).
      sweep_space = (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect);
    }
    if (sweep_space) {
      uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
      uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
      scc.space = space->AsDlMallocSpace();
      accounting::SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      accounting::SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
      if (swap_bitmaps) {
        std::swap(live_bitmap, mark_bitmap);
      }
      if (!space->IsZygoteSpace()) {
        timings_.NewSplit("SweepAllocSpace");
        // Bitmaps are pre-swapped for optimization which enables sweeping with the heap unlocked.
        accounting::SpaceBitmap::SweepWalk(*live_bitmap, *mark_bitmap, begin, end,
                                           &SweepCallback, reinterpret_cast<void*>(&scc));
      } else {
        timings_.NewSplit("SweepZygote");
        // Zygote sweep takes care of dirtying cards and clearing live bits, does not free actual
        // memory.
        accounting::SpaceBitmap::SweepWalk(*live_bitmap, *mark_bitmap, begin, end,
                                           &ZygoteSweepCallback, reinterpret_cast<void*>(&scc));
      }
    }
  }

  timings_.NewSplit("SweepLargeObjects");
  SweepLargeObjects(swap_bitmaps);
}

void MarkSweep::SweepLargeObjects(bool swap_bitmaps) {
  // Sweep large objects
  space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  accounting::SpaceSetMap* large_live_objects = large_object_space->GetLiveObjects();
  accounting::SpaceSetMap* large_mark_objects = large_object_space->GetMarkObjects();
  if (swap_bitmaps) {
    std::swap(large_live_objects, large_mark_objects);
  }
  accounting::SpaceSetMap::Objects& live_objects = large_live_objects->GetObjects();
  // O(n*log(n)) but hopefully there are not too many large objects.
  size_t freed_objects = 0;
  size_t freed_bytes = 0;
  Thread* self = Thread::Current();
  // TODO: C++0x
  typedef accounting::SpaceSetMap::Objects::iterator It;
  for (It it = live_objects.begin(), end = live_objects.end(); it != end; ++it) {
    if (!large_mark_objects->Test(*it)) {
      freed_bytes += large_object_space->Free(self, const_cast<Object*>(*it));
      ++freed_objects;
    }
  }
  freed_objects_ += freed_objects;
  freed_bytes_ += freed_bytes;
  GetHeap()->RecordFree(freed_objects, freed_bytes);
}

void MarkSweep::CheckReference(const Object* obj, const Object* ref, MemberOffset offset, bool is_static) {
  const std::vector<space::ContinuousSpace*>& spaces = GetHeap()->GetContinuousSpaces();
  // TODO: C++0x
  typedef std::vector<space::ContinuousSpace*>::const_iterator It;
  for (It it = spaces.begin(), end = spaces.end(); it != end; ++it) {
    space::ContinuousSpace* space = *it;
    if (space->IsDlMallocSpace() && space->Contains(ref)) {
      DCHECK(IsMarked(obj));

      bool is_marked = IsMarked(ref);
      if (!is_marked) {
        LOG(INFO) << *space;
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
    CHECK(obj != NULL);
    data_[length_++] = obj;
  }

  void MarkStackPush(const Object* obj) {
    if (static_cast<size_t>(length_) < max_size) {
      Push(const_cast<Object*>(obj));
    } else {
      // Internal (thread-local) buffer is full, push to a new buffer instead.
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
    size_t index;
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
  thread_pool->Wait(self, true, true);
  mark_stack_->Reset();
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
  const std::vector<space::ContinuousSpace*>& spaces = GetHeap()->GetContinuousSpaces();
  // TODO: C++0x
  typedef std::vector<space::ContinuousSpace*>::const_iterator It;
  for (It it = spaces.begin(), end = spaces.end(); it != end; ++it) {
    space::ContinuousSpace* space = *it;
    if (space->IsDlMallocSpace()) {
      space::DlMallocSpace* alloc_space = space->AsDlMallocSpace();
      if (alloc_space->temp_bitmap_.get() != NULL) {
        // At this point, the temp_bitmap holds our old mark bitmap.
        accounting::SpaceBitmap* new_bitmap = alloc_space->temp_bitmap_.release();
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
  Heap* heap = GetHeap();
  heap->EnqueueClearedReferences(&cleared_references);

  heap->PostGcVerification(this);

  timings_.NewSplit("GrowForUtilization");
  heap->GrowForUtilization(GetDurationNs());

  timings_.NewSplit("RequestHeapTrim");
  heap->RequestHeapTrim();

  // Update the cumulative statistics
  total_time_ns_ += GetDurationNs();
  total_paused_time_ns_ += std::accumulate(GetPauseTimes().begin(), GetPauseTimes().end(), 0,
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
  cumulative_timings_.AddNewLogger(timings_);
  cumulative_timings_.End();

  // Clear all of the spaces' mark bitmaps.
  const std::vector<space::ContinuousSpace*>& spaces = GetHeap()->GetContinuousSpaces();
  // TODO: C++0x
  typedef std::vector<space::ContinuousSpace*>::const_iterator It;
  for (It it = spaces.begin(), end = spaces.end(); it != end; ++it) {
    space::ContinuousSpace* space = *it;
    if (space->GetGcRetentionPolicy() != space::kGcRetentionPolicyNeverCollect) {
      space->GetMarkBitmap()->Clear();
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
