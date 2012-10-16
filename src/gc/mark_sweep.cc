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

#include <climits>
#include <vector>

#include "card_table.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "heap.h"
#include "indirect_reference_table.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "large_object_space.h"
#include "logging.h"
#include "macros.h"
#include "monitor.h"
#include "object.h"
#include "runtime.h"
#include "space.h"
#include "timing_logger.h"
#include "thread.h"
#include "thread_list.h"

static const bool kUseMarkStackPrefetch = true;

namespace art {

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

MarkSweep::MarkSweep(ObjectStack* mark_stack)
    : current_mark_bitmap_(NULL),
      mark_stack_(mark_stack),
      heap_(NULL),
      finger_(NULL),
      immune_begin_(NULL),
      immune_end_(NULL),
      soft_reference_list_(NULL),
      weak_reference_list_(NULL),
      finalizer_reference_list_(NULL),
      phantom_reference_list_(NULL),
      cleared_reference_list_(NULL),
      freed_bytes_(0), freed_objects_(0),
      class_count_(0), array_count_(0), other_count_(0) {
  DCHECK(mark_stack_ != NULL);
}

void MarkSweep::Init() {
  heap_ = Runtime::Current()->GetHeap();
  mark_stack_->Reset();
  // TODO: C++0x auto
  FindDefaultMarkBitmap();
  // TODO: if concurrent, enable card marking in compiler
  // TODO: check that the mark bitmap is entirely clear.
  // Mark any concurrent roots as dirty since we need to scan them at least once during this GC.
  Runtime::Current()->DirtyRoots();
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

inline void MarkSweep::MarkObject0(const Object* obj, bool check_finger) {
  DCHECK(obj != NULL);

  if (obj >= immune_begin_ && obj < immune_end_) {
    DCHECK(IsMarked(obj));
    return;
  }

  // Try to take advantage of locality of references within a space, failing this find the space
  // the hard way.
  if (UNLIKELY(!current_mark_bitmap_->HasAddress(obj))) {
    SpaceBitmap* new_bitmap = heap_->GetMarkBitmap()->GetSpaceBitmap(obj);
    if (new_bitmap != NULL) {
      current_mark_bitmap_ = new_bitmap;
    } else {
      LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
      SpaceSetMap* large_objects = large_object_space->GetMarkObjects();
      if (!large_objects->Test(obj)) {
        if (!large_object_space->Contains(obj)) {
          LOG(ERROR) << "Tried to mark " << obj << " not contained by any spaces";
          LOG(ERROR) << "Attempting see if it's a bad root";
          VerifyRoots();
          LOG(FATAL) << "Can't mark bad root";
        }
        large_objects->Set(obj);
        // Don't need to check finger since large objects never have any object references.
      }
      // TODO: Improve clarity of control flow in this function?
      return;
    }
  }

  // This object was not previously marked.
  if (!current_mark_bitmap_->Test(obj)) {
    current_mark_bitmap_->Set(obj);
    if (check_finger && obj < finger_) {
      // Do we need to expand the mark stack?
      if (UNLIKELY(mark_stack_->Size() >= mark_stack_->Capacity())) {
        std::vector<Object*> temp;
        temp.insert(temp.begin(), mark_stack_->Begin(), mark_stack_->End());
        mark_stack_->Resize(mark_stack_->Capacity() * 2);
        for (size_t i = 0; i < temp.size(); ++i) {
          mark_stack_->PushBack(temp[i]);
        }
      }
      // The object must be pushed on to the mark stack.
      mark_stack_->PushBack(const_cast<Object*>(obj));
    }
  }
}

// Used to mark objects when recursing.  Recursion is done by moving
// the finger across the bitmaps in address order and marking child
// objects.  Any newly-marked objects whose addresses are lower than
// the finger won't be visited by the bitmap scan, so those objects
// need to be added to the mark stack.
void MarkSweep::MarkObject(const Object* obj) {
  if (obj != NULL) {
    MarkObject0(obj, true);
  }
}

void MarkSweep::MarkObjectVisitor(const Object* root, void* arg) {
  DCHECK(root != NULL);
  DCHECK(arg != NULL);
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->MarkObject0(root, false);
}

void MarkSweep::ReMarkObjectVisitor(const Object* root, void* arg) {
  DCHECK(root != NULL);
  DCHECK(arg != NULL);
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->MarkObject0(root, true);
}

void MarkSweep::VerifyRootCallback(const Object* root, void* arg, size_t vreg,
                                   const AbstractMethod* method) {
  reinterpret_cast<MarkSweep*>(arg)->VerifyRoot(root, vreg, method);
}

void MarkSweep::VerifyRoot(const Object* root, size_t vreg, const AbstractMethod* method) {
  // See if the root is on any space bitmap.
  if (heap_->FindSpaceFromObject(root) == NULL) {
    LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
    if (large_object_space->Contains(root)) {
      LOG(ERROR) << "Found invalid root: " << root;
      LOG(ERROR) << "VReg / Shadow frame offset: " << vreg;
      if (method != NULL) {
        LOG(ERROR) << "In method " << PrettyMethod(method, true);
      }
    }
  }
}

void MarkSweep::VerifyRoots() {
  Runtime::Current()->GetThreadList()->VerifyRoots(VerifyRootCallback, this);
}

// Marks all objects in the root set.
void MarkSweep::MarkRoots() {
  Runtime::Current()->VisitNonConcurrentRoots(MarkObjectVisitor, this);
}

void MarkSweep::MarkConcurrentRoots() {
  Runtime::Current()->VisitConcurrentRoots(MarkObjectVisitor, this);
}

class CheckObjectVisitor {
 public:
  CheckObjectVisitor(MarkSweep* const mark_sweep)
      : mark_sweep_(mark_sweep) {

  }

  void operator ()(const Object* obj, const Object* ref, MemberOffset offset, bool is_static) const
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_) {
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

void MarkSweep::CopyMarkBits(ContinuousSpace* space) {
  SpaceBitmap* live_bitmap = space->GetLiveBitmap();
  SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
  mark_bitmap->CopyFrom(live_bitmap);
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

class ScanImageRootVisitor {
 public:
  ScanImageRootVisitor(MarkSweep* const mark_sweep) : mark_sweep_(mark_sweep) {
  }

  void operator ()(const Object* root) const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(root != NULL);
    mark_sweep_->ScanObject(root);
  }

 private:
  MarkSweep* const mark_sweep_;
};

void MarkSweep::ScanGrayObjects(bool update_finger) {
  const Spaces& spaces = heap_->GetSpaces();
  CardTable* card_table = heap_->GetCardTable();
  ScanImageRootVisitor image_root_visitor(this);
  SetFingerVisitor finger_visitor(this);
  // TODO: C++ 0x auto
  for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
    ContinuousSpace* space = *it;
    byte* begin = space->Begin();
    byte* end = space->End();
    // Image spaces are handled properly since live == marked for them.
    SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    if (update_finger) {
      card_table->Scan(mark_bitmap, begin, end, image_root_visitor, finger_visitor);
    } else {
      card_table->Scan(mark_bitmap, begin, end, image_root_visitor, IdentityFunctor());
    }
  }
}

class CheckBitmapVisitor {
 public:
  CheckBitmapVisitor(MarkSweep* mark_sweep) : mark_sweep_(mark_sweep) {

  }

  void operator ()(const Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_) {
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
      live_bitmap->VisitMarkedRange(begin, end, visitor, IdentityFunctor());
    }
  }
}

class ScanObjectVisitor {
 public:
  ScanObjectVisitor(MarkSweep* const mark_sweep) : mark_sweep_(mark_sweep) {

  }

  void operator ()(const Object* obj) const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mark_sweep_->ScanObject(obj);
  }

 private:
  MarkSweep* const mark_sweep_;
};

// Populates the mark stack based on the set of marked objects and
// recursively marks until the mark stack is emptied.
void MarkSweep::RecursiveMark(bool partial, TimingLogger& timings) {
  // RecursiveMark will build the lists of known instances of the Reference classes.
  // See DelayReferenceReferent for details.
  CHECK(soft_reference_list_ == NULL);
  CHECK(weak_reference_list_ == NULL);
  CHECK(finalizer_reference_list_ == NULL);
  CHECK(phantom_reference_list_ == NULL);
  CHECK(cleared_reference_list_ == NULL);

  const Spaces& spaces = heap_->GetSpaces();

  SetFingerVisitor set_finger_visitor(this);
  ScanObjectVisitor scan_visitor(this);
  for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
    ContinuousSpace* space = *it;
    if (space->GetGcRetentionPolicy() == kGcRetentionPolicyAlwaysCollect ||
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
  finger_ = reinterpret_cast<Object*>(~0);
  timings.AddSplit("RecursiveMark");
  // TODO: tune the frequency of emptying the mark stack
  ProcessMarkStack();
  timings.AddSplit("ProcessMarkStack");
}

void MarkSweep::RecursiveMarkCards(CardTable* card_table, const std::vector<byte*>& cards,
                                   TimingLogger& timings) {
  ScanImageRootVisitor image_root_visitor(this);
  SetFingerVisitor finger_visitor(this);
  const size_t card_count = cards.size();
  SpaceBitmap* active_bitmap = NULL;
  for (size_t i = 0;i < card_count;) {
    Object* start_obj = reinterpret_cast<Object*>(card_table->AddrFromCard(cards[i]));
    uintptr_t begin = reinterpret_cast<uintptr_t>(start_obj);
    uintptr_t end = begin + CardTable::kCardSize;
    for (++i; reinterpret_cast<uintptr_t>(cards[i]) == end && i < card_count; ++i) {
      end += CardTable::kCardSize;
    }
    if (active_bitmap == NULL || !active_bitmap->HasAddress(start_obj)) {
      active_bitmap = heap_->GetMarkBitmap()->GetSpaceBitmap(start_obj);
#ifndef NDEBUG
      if (active_bitmap == NULL) {
        GetHeap()->DumpSpaces();
        LOG(FATAL) << "Object " << reinterpret_cast<const void*>(start_obj);
      }
#endif
    }
    active_bitmap->VisitMarkedRange(begin, end, image_root_visitor, finger_visitor);
  }
  timings.AddSplit("RecursiveMarkCards");
  ProcessMarkStack();
  timings.AddSplit("ProcessMarkStack");
}

bool MarkSweep::IsMarkedCallback(const Object* object, void* arg) {
  return
      reinterpret_cast<MarkSweep*>(arg)->IsMarked(object) ||
      !reinterpret_cast<MarkSweep*>(arg)->GetHeap()->GetLiveBitmap()->Test(object);
}

void MarkSweep::RecursiveMarkDirtyObjects(bool update_finger) {
  ScanGrayObjects(update_finger);
  ProcessMarkStack();
}

void MarkSweep::ReMarkRoots() {
  Runtime::Current()->VisitRoots(ReMarkObjectVisitor, this);
}

void MarkSweep::SweepJniWeakGlobals(Heap::IsMarkedTester is_marked, void* arg) {
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

void MarkSweep::SweepCallback(size_t num_ptrs, Object** ptrs, void* arg) {
  size_t freed_objects = num_ptrs;
  size_t freed_bytes = 0;
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  MarkSweep* mark_sweep = context->mark_sweep;
  Heap* heap = mark_sweep->GetHeap();
  AllocSpace* space = context->space;
  Thread* self = context->self;
  Locks::heap_bitmap_lock_->AssertExclusiveHeld(self);
  // Use a bulk free, that merges consecutive objects before freeing or free per object?
  // Documentation suggests better free performance with merging, but this may be at the expensive
  // of allocation.
  // TODO: investigate performance
  static const bool kUseFreeList = true;
  if (kUseFreeList) {
    // AllocSpace::FreeList clears the value in ptrs, so perform after clearing the live bit
    freed_bytes += space->FreeList(self, num_ptrs, ptrs);
  } else {
    for (size_t i = 0; i < num_ptrs; ++i) {
      Object* obj = static_cast<Object*>(ptrs[i]);
      freed_bytes += space->Free(self, obj);
    }
  }

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
  // TODO: Fix when sweeping weaks works properly with mutators unpaused + allocation list.
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
  logger.AddSplit("Reset stack");
}

void MarkSweep::Sweep(bool partial, bool swap_bitmaps) {
  DCHECK(mark_stack_->IsEmpty());

  // If we don't swap bitmaps then newly allocated Weaks go into the live bitmap but not mark
  // bitmap, resulting in occasional frees of Weaks which are still in use.
  SweepSystemWeaks();

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

// Scans instance fields.
inline void MarkSweep::ScanInstanceFields(const Object* obj) {
  DCHECK(obj != NULL);
  Class* klass = obj->GetClass();
  DCHECK(klass != NULL);
  ScanFields(obj, klass->GetReferenceInstanceOffsets(), false);
}

// Scans static storage on a Class.
inline void MarkSweep::ScanStaticFields(const Class* klass) {
  DCHECK(klass != NULL);
  ScanFields(klass, klass->GetReferenceStaticOffsets(), true);
}

inline void MarkSweep::ScanFields(const Object* obj, uint32_t ref_offsets, bool is_static) {
  if (ref_offsets != CLASS_WALK_SUPER) {
    // Found a reference offset bitmap.  Mark the specified offsets.
    while (ref_offsets != 0) {
      const size_t right_shift = CLZ(ref_offsets);
      MemberOffset byte_offset = CLASS_OFFSET_FROM_CLZ(right_shift);
      const Object* ref = obj->GetFieldObject<const Object*>(byte_offset, false);
      MarkObject(ref);
      ref_offsets ^= CLASS_HIGH_BIT >> right_shift;
    }
  } else {
    // There is no reference offset bitmap.  In the non-static case,
    // walk up the class inheritance hierarchy and find reference
    // offsets the hard way. In the static case, just consider this
    // class.
    for (const Class* klass = is_static ? obj->AsClass() : obj->GetClass();
         klass != NULL;
         klass = is_static ? NULL : klass->GetSuperClass()) {
      size_t num_reference_fields = (is_static
                                     ? klass->NumReferenceStaticFields()
                                     : klass->NumReferenceInstanceFields());
      for (size_t i = 0; i < num_reference_fields; ++i) {
        Field* field = (is_static
                        ? klass->GetStaticField(i)
                        : klass->GetInstanceField(i));
        MemberOffset field_offset = field->GetOffset();
        const Object* ref = obj->GetFieldObject<const Object*>(field_offset, false);
        MarkObject(ref);
      }
    }
  }
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

// Scans the header, static field references, and interface pointers
// of a class object.
inline void MarkSweep::ScanClass(const Object* obj) {
#ifndef NDEBUG
  ++class_count_;
#endif
  ScanInstanceFields(obj);
  ScanStaticFields(obj->AsClass());
}

// Scans the header of all array objects.  If the array object is
// specialized to a reference type, scans the array data as well.
inline void MarkSweep::ScanArray(const Object* obj) {
#ifndef NDEBUG
  ++array_count_;
#endif
  MarkObject(obj->GetClass());
  if (obj->IsObjectArray()) {
    const ObjectArray<Object>* array = obj->AsObjectArray<Object>();
    for (int32_t i = 0; i < array->GetLength(); ++i) {
      const Object* element = array->GetWithoutChecks(i);
      MarkObject(element);
    }
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
    heap_->EnqueuePendingReference(obj, list);
  }
}

// Scans the header and field references of a data object.  If the
// scanned object is a reference subclass, it is scheduled for later
// processing.
inline void MarkSweep::ScanOther(const Object* obj) {
#ifndef NDEBUG
  ++other_count_;
#endif
  ScanInstanceFields(obj);
  if (obj->GetClass()->IsReferenceClass()) {
    DelayReferenceReferent(const_cast<Object*>(obj));
  }
}

void MarkSweep::ScanRoot(const Object* obj) {
  ScanObject(obj);
}

// Scans an object reference.  Determines the type of the reference
// and dispatches to a specialized scanning routine.
void MarkSweep::ScanObject(const Object* obj) {
  DCHECK(obj != NULL);
  DCHECK(obj->GetClass() != NULL);
#ifndef NDEBUG
  if (!IsMarked(obj)) {
    heap_->DumpSpaces();
    LOG(FATAL) << "Scanning unmarked object " << reinterpret_cast<const void*>(obj);
  }
#endif
  if (obj->IsClass()) {
    ScanClass(obj);
  } else if (obj->IsArrayInstance()) {
    ScanArray(obj);
  } else {
    ScanOther(obj);
  }
}

// Scan anything that's on the mark stack.
void MarkSweep::ProcessMarkStack() {
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

MarkSweep::~MarkSweep() {
#ifndef NDEBUG
  VLOG(heap) << "MarkSweep scanned classes=" << class_count_ << " arrays=" << array_count_ << " other=" << other_count_;
#endif
  // Ensure that the mark stack is empty.
  CHECK(mark_stack_->IsEmpty());

  // Clear all of the alloc spaces' mark bitmaps.
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

}  // namespace art
