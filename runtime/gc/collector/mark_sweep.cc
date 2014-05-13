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

#include "base/bounded_fifo.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex-inl.h"
#include "base/timing_logger.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/mod_union_table.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "gc/reference_processor.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "mark_sweep-inl.h"
#include "mirror/art_field-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"
#include "thread_list.h"

using ::art::mirror::ArtField;
using ::art::mirror::Class;
using ::art::mirror::Object;
using ::art::mirror::ObjectArray;

namespace art {
namespace gc {
namespace collector {

// Performance options.
static constexpr bool kUseRecursiveMark = false;
static constexpr bool kUseMarkStackPrefetch = true;
static constexpr size_t kSweepArrayChunkFreeSize = 1024;
static constexpr bool kPreCleanCards = true;

// Parallelism options.
static constexpr bool kParallelCardScan = true;
static constexpr bool kParallelRecursiveMark = true;
// Don't attempt to parallelize mark stack processing unless the mark stack is at least n
// elements. This is temporary until we reduce the overhead caused by allocating tasks, etc.. Not
// having this can add overhead in ProcessReferences since we may end up doing many calls of
// ProcessMarkStack with very small mark stacks.
static constexpr size_t kMinimumParallelMarkStackSize = 128;
static constexpr bool kParallelProcessMarkStack = true;

// Profiling and information flags.
static constexpr bool kProfileLargeObjects = false;
static constexpr bool kMeasureOverhead = false;
static constexpr bool kCountTasks = false;
static constexpr bool kCountJavaLangRefs = false;
static constexpr bool kCountMarkedObjects = false;

// Turn off kCheckLocks when profiling the GC since it slows the GC down by up to 40%.
static constexpr bool kCheckLocks = kDebugLocking;
static constexpr bool kVerifyRootsMarked = kIsDebugBuild;

// If true, revoke the rosalloc thread-local buffers at the
// checkpoint, as opposed to during the pause.
static constexpr bool kRevokeRosAllocThreadLocalBuffersAtCheckpoint = true;

void MarkSweep::BindBitmaps() {
  timings_.StartSplit("BindBitmaps");
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  // Mark all of the spaces we never collect as immune.
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyNeverCollect) {
      CHECK(immune_region_.AddContinuousSpace(space)) << "Failed to add space " << *space;
    }
  }
  timings_.EndSplit();
}

MarkSweep::MarkSweep(Heap* heap, bool is_concurrent, const std::string& name_prefix)
    : GarbageCollector(heap,
                       name_prefix +
                       (is_concurrent ? "concurrent mark sweep": "mark sweep")),
      gc_barrier_(new Barrier(0)),
      mark_stack_lock_("mark sweep mark stack lock", kMarkSweepMarkStackLock),
      is_concurrent_(is_concurrent) {
}

void MarkSweep::InitializePhase() {
  TimingLogger::ScopedSplit split("InitializePhase", &timings_);
  mark_stack_ = heap_->GetMarkStack();
  DCHECK(mark_stack_ != nullptr);
  immune_region_.Reset();
  class_count_ = 0;
  array_count_ = 0;
  other_count_ = 0;
  large_object_test_ = 0;
  large_object_mark_ = 0;
  overhead_time_ = 0;
  work_chunks_created_ = 0;
  work_chunks_deleted_ = 0;
  reference_count_ = 0;
  mark_null_count_ = 0;
  mark_immune_count_ = 0;
  mark_fastpath_count_ = 0;
  mark_slowpath_count_ = 0;
  {
    // TODO: I don't think we should need heap bitmap lock to Get the mark bitmap.
    ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    mark_bitmap_ = heap_->GetMarkBitmap();
  }
  if (!clear_soft_references_) {
    // Always clear soft references if a non-sticky collection.
    clear_soft_references_ = GetGcType() != collector::kGcTypeSticky;
  }
}

void MarkSweep::RunPhases() {
  Thread* self = Thread::Current();
  InitializePhase();
  Locks::mutator_lock_->AssertNotHeld(self);
  if (IsConcurrent()) {
    GetHeap()->PreGcVerification(this);
    {
      ReaderMutexLock mu(self, *Locks::mutator_lock_);
      MarkingPhase();
    }
    ScopedPause pause(this);
    GetHeap()->PrePauseRosAllocVerification(this);
    PausePhase();
    RevokeAllThreadLocalBuffers();
  } else {
    ScopedPause pause(this);
    GetHeap()->PreGcVerificationPaused(this);
    MarkingPhase();
    GetHeap()->PrePauseRosAllocVerification(this);
    PausePhase();
    RevokeAllThreadLocalBuffers();
  }
  {
    // Sweeping always done concurrently, even for non concurrent mark sweep.
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    ReclaimPhase();
  }
  GetHeap()->PostGcVerification(this);
  FinishPhase();
}

void MarkSweep::ProcessReferences(Thread* self) {
  TimingLogger::ScopedSplit split("ProcessReferences", &timings_);
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetHeap()->GetReferenceProcessor()->ProcessReferences(
      true, &timings_, clear_soft_references_, &IsMarkedCallback, &MarkObjectCallback,
      &ProcessMarkStackCallback, this);
}

void MarkSweep::PausePhase() {
  TimingLogger::ScopedSplit split("(Paused)PausePhase", &timings_);
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  if (IsConcurrent()) {
    // Handle the dirty objects if we are a concurrent GC.
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // Re-mark root set.
    ReMarkRoots();
    // Scan dirty objects, this is only required if we are not doing concurrent GC.
    RecursiveMarkDirtyObjects(true, accounting::CardTable::kCardDirty);
  }
  {
    TimingLogger::ScopedSplit split("SwapStacks", &timings_);
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    heap_->SwapStacks(self);
    live_stack_freeze_size_ = heap_->GetLiveStack()->Size();
    // Need to revoke all the thread local allocation stacks since we just swapped the allocation
    // stacks and don't want anybody to allocate into the live stack.
    RevokeAllThreadLocalAllocationStacks(self);
  }
  timings_.StartSplit("PreSweepingGcVerification");
  heap_->PreSweepingGcVerification(this);
  timings_.EndSplit();
  // Disallow new system weaks to prevent a race which occurs when someone adds a new system
  // weak before we sweep them. Since this new system weak may not be marked, the GC may
  // incorrectly sweep it. This also fixes a race where interning may attempt to return a strong
  // reference to a string that is about to be swept.
  Runtime::Current()->DisallowNewSystemWeaks();
  // Enable the reference processing slow path, needs to be done with mutators paused since there
  // is no lock in the GetReferent fast path.
  GetHeap()->GetReferenceProcessor()->EnableSlowPath();
}

void MarkSweep::PreCleanCards() {
  // Don't do this for non concurrent GCs since they don't have any dirty cards.
  if (kPreCleanCards && IsConcurrent()) {
    Thread* self = Thread::Current();
    CHECK(!Locks::mutator_lock_->IsExclusiveHeld(self));
    // Process dirty cards and add dirty cards to mod union tables, also ages cards.
    heap_->ProcessCards(timings_, false);
    // The checkpoint root marking is required to avoid a race condition which occurs if the
    // following happens during a reference write:
    // 1. mutator dirties the card (write barrier)
    // 2. GC ages the card (the above ProcessCards call)
    // 3. GC scans the object (the RecursiveMarkDirtyObjects call below)
    // 4. mutator writes the value (corresponding to the write barrier in 1.)
    // This causes the GC to age the card but not necessarily mark the reference which the mutator
    // wrote into the object stored in the card.
    // Having the checkpoint fixes this issue since it ensures that the card mark and the
    // reference write are visible to the GC before the card is scanned (this is due to locks being
    // acquired / released in the checkpoint code).
    // The other roots are also marked to help reduce the pause.
    MarkRootsCheckpoint(self, false);
    MarkNonThreadRoots();
    MarkConcurrentRoots(
        static_cast<VisitRootFlags>(kVisitRootFlagClearRootLog | kVisitRootFlagNewRoots));
    // Process the newly aged cards.
    RecursiveMarkDirtyObjects(false, accounting::CardTable::kCardDirty - 1);
    // TODO: Empty allocation stack to reduce the number of objects we need to test / mark as live
    // in the next GC.
  }
}

void MarkSweep::RevokeAllThreadLocalAllocationStacks(Thread* self) {
  if (kUseThreadLocalAllocationStack) {
    timings_.NewSplit("RevokeAllThreadLocalAllocationStacks");
    Locks::mutator_lock_->AssertExclusiveHeld(self);
    heap_->RevokeAllThreadLocalAllocationStacks(self);
  }
}

void MarkSweep::MarkingPhase() {
  TimingLogger::ScopedSplit split("MarkingPhase", &timings_);
  Thread* self = Thread::Current();

  BindBitmaps();
  FindDefaultSpaceBitmap();

  // Process dirty cards and add dirty cards to mod union tables.
  heap_->ProcessCards(timings_, false);

  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  MarkRoots(self);
  MarkReachableObjects();
  // Pre-clean dirtied cards to reduce pauses.
  PreCleanCards();
}

void MarkSweep::UpdateAndMarkModUnion() {
  for (const auto& space : heap_->GetContinuousSpaces()) {
    if (immune_region_.ContainsSpace(space)) {
      const char* name = space->IsZygoteSpace() ? "UpdateAndMarkZygoteModUnionTable" :
          "UpdateAndMarkImageModUnionTable";
      TimingLogger::ScopedSplit split(name, &timings_);
      accounting::ModUnionTable* mod_union_table = heap_->FindModUnionTableFromSpace(space);
      CHECK(mod_union_table != nullptr);
      mod_union_table->UpdateAndMarkReferences(MarkHeapReferenceCallback, this);
    }
  }
}

void MarkSweep::MarkReachableObjects() {
  UpdateAndMarkModUnion();
  // Recursively mark all the non-image bits set in the mark bitmap.
  RecursiveMark();
}

void MarkSweep::ReclaimPhase() {
  TimingLogger::ScopedSplit split("ReclaimPhase", &timings_);
  Thread* self = Thread::Current();
  // Process the references concurrently.
  ProcessReferences(self);
  SweepSystemWeaks(self);
  Runtime::Current()->AllowNewSystemWeaks();
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
    TimingLogger::ScopedSplit split("UnBindBitmaps", &timings_);
    GetHeap()->UnBindBitmaps();
  }
}

void MarkSweep::FindDefaultSpaceBitmap() {
  TimingLogger::ScopedSplit split("FindDefaultMarkBitmap", &timings_);
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    accounting::ContinuousSpaceBitmap* bitmap = space->GetMarkBitmap();
    // We want to have the main space instead of non moving if possible.
    if (bitmap != nullptr &&
        space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect) {
      current_space_bitmap_ = bitmap;
      // If we are not the non moving space exit the loop early since this will be good enough.
      if (space != heap_->GetNonMovingSpace()) {
        break;
      }
    }
  }
  if (current_space_bitmap_ == nullptr) {
    heap_->DumpSpaces();
    LOG(FATAL) << "Could not find a default mark bitmap";
  }
}

void MarkSweep::ExpandMarkStack() {
  ResizeMarkStack(mark_stack_->Capacity() * 2);
}

void MarkSweep::ResizeMarkStack(size_t new_size) {
  // Rare case, no need to have Thread::Current be a parameter.
  if (UNLIKELY(mark_stack_->Size() < mark_stack_->Capacity())) {
    // Someone else acquired the lock and expanded the mark stack before us.
    return;
  }
  std::vector<Object*> temp(mark_stack_->Begin(), mark_stack_->End());
  CHECK_LE(mark_stack_->Size(), new_size);
  mark_stack_->Resize(new_size);
  for (const auto& obj : temp) {
    mark_stack_->PushBack(obj);
  }
}

inline void MarkSweep::MarkObjectNonNullParallel(Object* obj) {
  DCHECK(obj != nullptr);
  if (MarkObjectParallel(obj)) {
    MutexLock mu(Thread::Current(), mark_stack_lock_);
    if (UNLIKELY(mark_stack_->Size() >= mark_stack_->Capacity())) {
      ExpandMarkStack();
    }
    // The object must be pushed on to the mark stack.
    mark_stack_->PushBack(obj);
  }
}

mirror::Object* MarkSweep::MarkObjectCallback(mirror::Object* obj, void* arg) {
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->MarkObject(obj);
  return obj;
}

void MarkSweep::MarkHeapReferenceCallback(mirror::HeapReference<mirror::Object>* ref, void* arg) {
  reinterpret_cast<MarkSweep*>(arg)->MarkObject(ref->AsMirrorPtr());
}

class MarkSweepMarkObjectSlowPath {
 public:
  explicit MarkSweepMarkObjectSlowPath(MarkSweep* mark_sweep) : mark_sweep_(mark_sweep) {
  }

  void operator()(const Object* obj) const ALWAYS_INLINE {
    if (kProfileLargeObjects) {
      // TODO: Differentiate between marking and testing somehow.
      ++mark_sweep_->large_object_test_;
      ++mark_sweep_->large_object_mark_;
    }
    space::LargeObjectSpace* large_object_space = mark_sweep_->GetHeap()->GetLargeObjectsSpace();
    if (UNLIKELY(obj == nullptr || !IsAligned<kPageSize>(obj) ||
                 (kIsDebugBuild && !large_object_space->Contains(obj)))) {
      LOG(ERROR) << "Tried to mark " << obj << " not contained by any spaces";
      LOG(ERROR) << "Attempting see if it's a bad root";
      mark_sweep_->VerifyRoots();
      LOG(FATAL) << "Can't mark invalid object";
    }
  }

 private:
  MarkSweep* const mark_sweep_;
};

inline void MarkSweep::MarkObjectNonNull(Object* obj) {
  DCHECK(obj != nullptr);
  if (kUseBakerOrBrooksReadBarrier) {
    // Verify all the objects have the correct pointer installed.
    obj->AssertReadBarrierPointer();
  }
  if (immune_region_.ContainsObject(obj)) {
    if (kCountMarkedObjects) {
      ++mark_immune_count_;
    }
    DCHECK(mark_bitmap_->Test(obj));
  } else if (LIKELY(current_space_bitmap_->HasAddress(obj))) {
    if (kCountMarkedObjects) {
      ++mark_fastpath_count_;
    }
    if (UNLIKELY(!current_space_bitmap_->Set(obj))) {
      PushOnMarkStack(obj);  // This object was not previously marked.
    }
  } else {
    if (kCountMarkedObjects) {
      ++mark_slowpath_count_;
    }
    MarkSweepMarkObjectSlowPath visitor(this);
    // TODO: We already know that the object is not in the current_space_bitmap_ but MarkBitmap::Set
    // will check again.
    if (!mark_bitmap_->Set(obj, visitor)) {
      PushOnMarkStack(obj);  // Was not already marked, push.
    }
  }
}

inline void MarkSweep::PushOnMarkStack(Object* obj) {
  if (UNLIKELY(mark_stack_->Size() >= mark_stack_->Capacity())) {
    // Lock is not needed but is here anyways to please annotalysis.
    MutexLock mu(Thread::Current(), mark_stack_lock_);
    ExpandMarkStack();
  }
  // The object must be pushed on to the mark stack.
  mark_stack_->PushBack(obj);
}

inline bool MarkSweep::MarkObjectParallel(const Object* obj) {
  DCHECK(obj != nullptr);
  if (kUseBakerOrBrooksReadBarrier) {
    // Verify all the objects have the correct pointer installed.
    obj->AssertReadBarrierPointer();
  }
  if (immune_region_.ContainsObject(obj)) {
    DCHECK(IsMarked(obj));
    return false;
  }
  // Try to take advantage of locality of references within a space, failing this find the space
  // the hard way.
  accounting::ContinuousSpaceBitmap* object_bitmap = current_space_bitmap_;
  if (LIKELY(object_bitmap->HasAddress(obj))) {
    return !object_bitmap->AtomicTestAndSet(obj);
  }
  MarkSweepMarkObjectSlowPath visitor(this);
  return !mark_bitmap_->AtomicTestAndSet(obj, visitor);
}

// Used to mark objects when processing the mark stack. If an object is null, it is not marked.
inline void MarkSweep::MarkObject(Object* obj) {
  if (obj != nullptr) {
    MarkObjectNonNull(obj);
  } else if (kCountMarkedObjects) {
    ++mark_null_count_;
  }
}

void MarkSweep::MarkRootParallelCallback(Object** root, void* arg, uint32_t /*thread_id*/,
                                         RootType /*root_type*/) {
  reinterpret_cast<MarkSweep*>(arg)->MarkObjectNonNullParallel(*root);
}

void MarkSweep::VerifyRootMarked(Object** root, void* arg, uint32_t /*thread_id*/,
                                 RootType /*root_type*/) {
  CHECK(reinterpret_cast<MarkSweep*>(arg)->IsMarked(*root));
}

void MarkSweep::MarkRootCallback(Object** root, void* arg, uint32_t /*thread_id*/,
                                 RootType /*root_type*/) {
  reinterpret_cast<MarkSweep*>(arg)->MarkObjectNonNull(*root);
}

void MarkSweep::VerifyRootCallback(const Object* root, void* arg, size_t vreg,
                                   const StackVisitor* visitor, RootType root_type) {
  reinterpret_cast<MarkSweep*>(arg)->VerifyRoot(root, vreg, visitor, root_type);
}

void MarkSweep::VerifyRoot(const Object* root, size_t vreg, const StackVisitor* visitor,
                           RootType root_type) {
  // See if the root is on any space bitmap.
  if (heap_->GetLiveBitmap()->GetContinuousSpaceBitmap(root) == nullptr) {
    space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
    if (!large_object_space->Contains(root)) {
      LOG(ERROR) << "Found invalid root: " << root << " with type " << root_type;
      if (visitor != NULL) {
        LOG(ERROR) << visitor->DescribeLocation() << " in VReg: " << vreg;
      }
    }
  }
}

void MarkSweep::VerifyRoots() {
  Runtime::Current()->GetThreadList()->VerifyRoots(VerifyRootCallback, this);
}

void MarkSweep::MarkRoots(Thread* self) {
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // If we exclusively hold the mutator lock, all threads must be suspended.
    timings_.StartSplit("MarkRoots");
    Runtime::Current()->VisitRoots(MarkRootCallback, this);
    timings_.EndSplit();
    RevokeAllThreadLocalAllocationStacks(self);
  } else {
    MarkRootsCheckpoint(self, kRevokeRosAllocThreadLocalBuffersAtCheckpoint);
    // At this point the live stack should no longer have any mutators which push into it.
    MarkNonThreadRoots();
    MarkConcurrentRoots(
        static_cast<VisitRootFlags>(kVisitRootFlagAllRoots | kVisitRootFlagStartLoggingNewRoots));
  }
}

void MarkSweep::MarkNonThreadRoots() {
  timings_.StartSplit("MarkNonThreadRoots");
  Runtime::Current()->VisitNonThreadRoots(MarkRootCallback, this);
  timings_.EndSplit();
}

void MarkSweep::MarkConcurrentRoots(VisitRootFlags flags) {
  timings_.StartSplit("MarkConcurrentRoots");
  // Visit all runtime roots and clear dirty flags.
  Runtime::Current()->VisitConcurrentRoots(MarkRootCallback, this, flags);
  timings_.EndSplit();
}

class ScanObjectVisitor {
 public:
  explicit ScanObjectVisitor(MarkSweep* const mark_sweep) ALWAYS_INLINE
      : mark_sweep_(mark_sweep) {}

  void operator()(Object* obj) const ALWAYS_INLINE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    if (kCheckLocks) {
      Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    mark_sweep_->ScanObject(obj);
  }

 private:
  MarkSweep* const mark_sweep_;
};

class DelayReferenceReferentVisitor {
 public:
  explicit DelayReferenceReferentVisitor(MarkSweep* collector) : collector_(collector) {
  }

  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    collector_->DelayReferenceReferent(klass, ref);
  }

 private:
  MarkSweep* const collector_;
};

template <bool kUseFinger = false>
class MarkStackTask : public Task {
 public:
  MarkStackTask(ThreadPool* thread_pool, MarkSweep* mark_sweep, size_t mark_stack_size,
                Object** mark_stack)
      : mark_sweep_(mark_sweep),
        thread_pool_(thread_pool),
        mark_stack_pos_(mark_stack_size) {
    // We may have to copy part of an existing mark stack when another mark stack overflows.
    if (mark_stack_size != 0) {
      DCHECK(mark_stack != NULL);
      // TODO: Check performance?
      std::copy(mark_stack, mark_stack + mark_stack_size, mark_stack_);
    }
    if (kCountTasks) {
      ++mark_sweep_->work_chunks_created_;
    }
  }

  static const size_t kMaxSize = 1 * KB;

 protected:
  class MarkObjectParallelVisitor {
   public:
    explicit MarkObjectParallelVisitor(MarkStackTask<kUseFinger>* chunk_task,
                                       MarkSweep* mark_sweep) ALWAYS_INLINE
            : chunk_task_(chunk_task), mark_sweep_(mark_sweep) {}

    void operator()(Object* obj, MemberOffset offset, bool /* static */) const ALWAYS_INLINE
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      mirror::Object* ref = obj->GetFieldObject<mirror::Object>(offset);
      if (ref != nullptr && mark_sweep_->MarkObjectParallel(ref)) {
        if (kUseFinger) {
          android_memory_barrier();
          if (reinterpret_cast<uintptr_t>(ref) >=
              static_cast<uintptr_t>(mark_sweep_->atomic_finger_)) {
            return;
          }
        }
        chunk_task_->MarkStackPush(ref);
      }
    }

   private:
    MarkStackTask<kUseFinger>* const chunk_task_;
    MarkSweep* const mark_sweep_;
  };

  class ScanObjectParallelVisitor {
   public:
    explicit ScanObjectParallelVisitor(MarkStackTask<kUseFinger>* chunk_task) ALWAYS_INLINE
        : chunk_task_(chunk_task) {}

    // No thread safety analysis since multiple threads will use this visitor.
    void operator()(Object* obj) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
        EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
      MarkSweep* const mark_sweep = chunk_task_->mark_sweep_;
      MarkObjectParallelVisitor mark_visitor(chunk_task_, mark_sweep);
      DelayReferenceReferentVisitor ref_visitor(mark_sweep);
      mark_sweep->ScanObjectVisit(obj, mark_visitor, ref_visitor);
    }

   private:
    MarkStackTask<kUseFinger>* const chunk_task_;
  };

  virtual ~MarkStackTask() {
    // Make sure that we have cleared our mark stack.
    DCHECK_EQ(mark_stack_pos_, 0U);
    if (kCountTasks) {
      ++mark_sweep_->work_chunks_deleted_;
    }
  }

  MarkSweep* const mark_sweep_;
  ThreadPool* const thread_pool_;
  // Thread local mark stack for this task.
  Object* mark_stack_[kMaxSize];
  // Mark stack position.
  size_t mark_stack_pos_;

  void MarkStackPush(Object* obj) ALWAYS_INLINE {
    if (UNLIKELY(mark_stack_pos_ == kMaxSize)) {
      // Mark stack overflow, give 1/2 the stack to the thread pool as a new work task.
      mark_stack_pos_ /= 2;
      auto* task = new MarkStackTask(thread_pool_, mark_sweep_, kMaxSize - mark_stack_pos_,
                                     mark_stack_ + mark_stack_pos_);
      thread_pool_->AddTask(Thread::Current(), task);
    }
    DCHECK(obj != nullptr);
    DCHECK_LT(mark_stack_pos_, kMaxSize);
    mark_stack_[mark_stack_pos_++] = obj;
  }

  virtual void Finalize() {
    delete this;
  }

  // Scans all of the objects
  virtual void Run(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    ScanObjectParallelVisitor visitor(this);
    // TODO: Tune this.
    static const size_t kFifoSize = 4;
    BoundedFifoPowerOfTwo<Object*, kFifoSize> prefetch_fifo;
    for (;;) {
      Object* obj = nullptr;
      if (kUseMarkStackPrefetch) {
        while (mark_stack_pos_ != 0 && prefetch_fifo.size() < kFifoSize) {
          Object* obj = mark_stack_[--mark_stack_pos_];
          DCHECK(obj != nullptr);
          __builtin_prefetch(obj);
          prefetch_fifo.push_back(obj);
        }
        if (UNLIKELY(prefetch_fifo.empty())) {
          break;
        }
        obj = prefetch_fifo.front();
        prefetch_fifo.pop_front();
      } else {
        if (UNLIKELY(mark_stack_pos_ == 0)) {
          break;
        }
        obj = mark_stack_[--mark_stack_pos_];
      }
      DCHECK(obj != nullptr);
      visitor(obj);
    }
  }
};

class CardScanTask : public MarkStackTask<false> {
 public:
  CardScanTask(ThreadPool* thread_pool, MarkSweep* mark_sweep,
               accounting::ContinuousSpaceBitmap* bitmap,
               byte* begin, byte* end, byte minimum_age, size_t mark_stack_size,
               Object** mark_stack_obj)
      : MarkStackTask<false>(thread_pool, mark_sweep, mark_stack_size, mark_stack_obj),
        bitmap_(bitmap),
        begin_(begin),
        end_(end),
        minimum_age_(minimum_age) {
  }

 protected:
  accounting::ContinuousSpaceBitmap* const bitmap_;
  byte* const begin_;
  byte* const end_;
  const byte minimum_age_;

  virtual void Finalize() {
    delete this;
  }

  virtual void Run(Thread* self) NO_THREAD_SAFETY_ANALYSIS {
    ScanObjectParallelVisitor visitor(this);
    accounting::CardTable* card_table = mark_sweep_->GetHeap()->GetCardTable();
    size_t cards_scanned = card_table->Scan(bitmap_, begin_, end_, visitor, minimum_age_);
    VLOG(heap) << "Parallel scanning cards " << reinterpret_cast<void*>(begin_) << " - "
        << reinterpret_cast<void*>(end_) << " = " << cards_scanned;
    // Finish by emptying our local mark stack.
    MarkStackTask::Run(self);
  }
};

size_t MarkSweep::GetThreadCount(bool paused) const {
  if (heap_->GetThreadPool() == nullptr || !heap_->CareAboutPauseTimes()) {
    return 1;
  }
  if (paused) {
    return heap_->GetParallelGCThreadCount() + 1;
  } else {
    return heap_->GetConcGCThreadCount() + 1;
  }
}

void MarkSweep::ScanGrayObjects(bool paused, byte minimum_age) {
  accounting::CardTable* card_table = GetHeap()->GetCardTable();
  ThreadPool* thread_pool = GetHeap()->GetThreadPool();
  size_t thread_count = GetThreadCount(paused);
  // The parallel version with only one thread is faster for card scanning, TODO: fix.
  if (kParallelCardScan && thread_count > 1) {
    Thread* self = Thread::Current();
    // Can't have a different split for each space since multiple spaces can have their cards being
    // scanned at the same time.
    timings_.StartSplit(paused ? "(Paused)ScanGrayObjects" : "ScanGrayObjects");
    // Try to take some of the mark stack since we can pass this off to the worker tasks.
    Object** mark_stack_begin = mark_stack_->Begin();
    Object** mark_stack_end = mark_stack_->End();
    const size_t mark_stack_size = mark_stack_end - mark_stack_begin;
    // Estimated number of work tasks we will create.
    const size_t mark_stack_tasks = GetHeap()->GetContinuousSpaces().size() * thread_count;
    DCHECK_NE(mark_stack_tasks, 0U);
    const size_t mark_stack_delta = std::min(CardScanTask::kMaxSize / 2,
                                             mark_stack_size / mark_stack_tasks + 1);
    for (const auto& space : GetHeap()->GetContinuousSpaces()) {
      if (space->GetMarkBitmap() == nullptr) {
        continue;
      }
      byte* card_begin = space->Begin();
      byte* card_end = space->End();
      // Align up the end address. For example, the image space's end
      // may not be card-size-aligned.
      card_end = AlignUp(card_end, accounting::CardTable::kCardSize);
      DCHECK(IsAligned<accounting::CardTable::kCardSize>(card_begin));
      DCHECK(IsAligned<accounting::CardTable::kCardSize>(card_end));
      // Calculate how many bytes of heap we will scan,
      const size_t address_range = card_end - card_begin;
      // Calculate how much address range each task gets.
      const size_t card_delta = RoundUp(address_range / thread_count + 1,
                                        accounting::CardTable::kCardSize);
      // Create the worker tasks for this space.
      while (card_begin != card_end) {
        // Add a range of cards.
        size_t addr_remaining = card_end - card_begin;
        size_t card_increment = std::min(card_delta, addr_remaining);
        // Take from the back of the mark stack.
        size_t mark_stack_remaining = mark_stack_end - mark_stack_begin;
        size_t mark_stack_increment = std::min(mark_stack_delta, mark_stack_remaining);
        mark_stack_end -= mark_stack_increment;
        mark_stack_->PopBackCount(static_cast<int32_t>(mark_stack_increment));
        DCHECK_EQ(mark_stack_end, mark_stack_->End());
        // Add the new task to the thread pool.
        auto* task = new CardScanTask(thread_pool, this, space->GetMarkBitmap(), card_begin,
                                      card_begin + card_increment, minimum_age,
                                      mark_stack_increment, mark_stack_end);
        thread_pool->AddTask(self, task);
        card_begin += card_increment;
      }
    }

    // Note: the card scan below may dirty new cards (and scan them)
    // as a side effect when a Reference object is encountered and
    // queued during the marking. See b/11465268.
    thread_pool->SetMaxActiveWorkers(thread_count - 1);
    thread_pool->StartWorkers(self);
    thread_pool->Wait(self, true, true);
    thread_pool->StopWorkers(self);
    timings_.EndSplit();
  } else {
    for (const auto& space : GetHeap()->GetContinuousSpaces()) {
      if (space->GetMarkBitmap() != nullptr) {
        // Image spaces are handled properly since live == marked for them.
        switch (space->GetGcRetentionPolicy()) {
          case space::kGcRetentionPolicyNeverCollect:
            timings_.StartSplit(paused ? "(Paused)ScanGrayImageSpaceObjects" :
                "ScanGrayImageSpaceObjects");
            break;
          case space::kGcRetentionPolicyFullCollect:
            timings_.StartSplit(paused ? "(Paused)ScanGrayZygoteSpaceObjects" :
                "ScanGrayZygoteSpaceObjects");
            break;
          case space::kGcRetentionPolicyAlwaysCollect:
            timings_.StartSplit(paused ? "(Paused)ScanGrayAllocSpaceObjects" :
                "ScanGrayAllocSpaceObjects");
            break;
          }
        ScanObjectVisitor visitor(this);
        card_table->Scan(space->GetMarkBitmap(), space->Begin(), space->End(), visitor, minimum_age);
        timings_.EndSplit();
      }
    }
  }
}

class RecursiveMarkTask : public MarkStackTask<false> {
 public:
  RecursiveMarkTask(ThreadPool* thread_pool, MarkSweep* mark_sweep,
                    accounting::ContinuousSpaceBitmap* bitmap, uintptr_t begin, uintptr_t end)
      : MarkStackTask<false>(thread_pool, mark_sweep, 0, NULL),
        bitmap_(bitmap),
        begin_(begin),
        end_(end) {
  }

 protected:
  accounting::ContinuousSpaceBitmap* const bitmap_;
  const uintptr_t begin_;
  const uintptr_t end_;

  virtual void Finalize() {
    delete this;
  }

  // Scans all of the objects
  virtual void Run(Thread* self) NO_THREAD_SAFETY_ANALYSIS {
    ScanObjectParallelVisitor visitor(this);
    bitmap_->VisitMarkedRange(begin_, end_, visitor);
    // Finish by emptying our local mark stack.
    MarkStackTask::Run(self);
  }
};

// Populates the mark stack based on the set of marked objects and
// recursively marks until the mark stack is emptied.
void MarkSweep::RecursiveMark() {
  TimingLogger::ScopedSplit split("RecursiveMark", &timings_);
  // RecursiveMark will build the lists of known instances of the Reference classes. See
  // DelayReferenceReferent for details.
  if (kUseRecursiveMark) {
    const bool partial = GetGcType() == kGcTypePartial;
    ScanObjectVisitor scan_visitor(this);
    auto* self = Thread::Current();
    ThreadPool* thread_pool = heap_->GetThreadPool();
    size_t thread_count = GetThreadCount(false);
    const bool parallel = kParallelRecursiveMark && thread_count > 1;
    mark_stack_->Reset();
    for (const auto& space : GetHeap()->GetContinuousSpaces()) {
      if ((space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect) ||
          (!partial && space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect)) {
        current_space_bitmap_ = space->GetMarkBitmap();
        if (current_space_bitmap_ == nullptr) {
          continue;
        }
        if (parallel) {
          // We will use the mark stack the future.
          // CHECK(mark_stack_->IsEmpty());
          // This function does not handle heap end increasing, so we must use the space end.
          uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
          uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
          atomic_finger_ = static_cast<int32_t>(0xFFFFFFFF);

          // Create a few worker tasks.
          const size_t n = thread_count * 2;
          while (begin != end) {
            uintptr_t start = begin;
            uintptr_t delta = (end - begin) / n;
            delta = RoundUp(delta, KB);
            if (delta < 16 * KB) delta = end - begin;
            begin += delta;
            auto* task = new RecursiveMarkTask(thread_pool, this, current_space_bitmap_, start,
                                               begin);
            thread_pool->AddTask(self, task);
          }
          thread_pool->SetMaxActiveWorkers(thread_count - 1);
          thread_pool->StartWorkers(self);
          thread_pool->Wait(self, true, true);
          thread_pool->StopWorkers(self);
        } else {
          // This function does not handle heap end increasing, so we must use the space end.
          uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
          uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
          current_space_bitmap_->VisitMarkedRange(begin, end, scan_visitor);
        }
      }
    }
  }
  ProcessMarkStack(false);
}

mirror::Object* MarkSweep::IsMarkedCallback(mirror::Object* object, void* arg) {
  if (reinterpret_cast<MarkSweep*>(arg)->IsMarked(object)) {
    return object;
  }
  return nullptr;
}

void MarkSweep::RecursiveMarkDirtyObjects(bool paused, byte minimum_age) {
  ScanGrayObjects(paused, minimum_age);
  ProcessMarkStack(paused);
}

void MarkSweep::ReMarkRoots() {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  timings_.StartSplit("(Paused)ReMarkRoots");
  Runtime::Current()->VisitRoots(
      MarkRootCallback, this, static_cast<VisitRootFlags>(kVisitRootFlagNewRoots |
                                                          kVisitRootFlagStopLoggingNewRoots |
                                                          kVisitRootFlagClearRootLog));
  timings_.EndSplit();
  if (kVerifyRootsMarked) {
    timings_.StartSplit("(Paused)VerifyRoots");
    Runtime::Current()->VisitRoots(VerifyRootMarked, this);
    timings_.EndSplit();
  }
}

void MarkSweep::SweepSystemWeaks(Thread* self) {
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  timings_.StartSplit("SweepSystemWeaks");
  Runtime::Current()->SweepSystemWeaks(IsMarkedCallback, this);
  timings_.EndSplit();
}

mirror::Object* MarkSweep::VerifySystemWeakIsLiveCallback(Object* obj, void* arg) {
  reinterpret_cast<MarkSweep*>(arg)->VerifyIsLive(obj);
  // We don't actually want to sweep the object, so lets return "marked"
  return obj;
}

void MarkSweep::VerifyIsLive(const Object* obj) {
  if (!heap_->GetLiveBitmap()->Test(obj)) {
    if (std::find(heap_->allocation_stack_->Begin(), heap_->allocation_stack_->End(), obj) ==
        heap_->allocation_stack_->End()) {
      // Object not found!
      heap_->DumpSpaces();
      LOG(FATAL) << "Found dead object " << obj;
    }
  }
}

void MarkSweep::VerifySystemWeaks() {
  // Verify system weaks, uses a special object visitor which returns the input object.
  Runtime::Current()->SweepSystemWeaks(VerifySystemWeakIsLiveCallback, this);
}

class CheckpointMarkThreadRoots : public Closure {
 public:
  explicit CheckpointMarkThreadRoots(MarkSweep* mark_sweep,
                                     bool revoke_ros_alloc_thread_local_buffers_at_checkpoint)
      : mark_sweep_(mark_sweep),
        revoke_ros_alloc_thread_local_buffers_at_checkpoint_(
            revoke_ros_alloc_thread_local_buffers_at_checkpoint) {
  }

  virtual void Run(Thread* thread) OVERRIDE NO_THREAD_SAFETY_ANALYSIS {
    ATRACE_BEGIN("Marking thread roots");
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* self = Thread::Current();
    CHECK(thread == self || thread->IsSuspended() || thread->GetState() == kWaitingPerformingGc)
        << thread->GetState() << " thread " << thread << " self " << self;
    thread->VisitRoots(MarkSweep::MarkRootParallelCallback, mark_sweep_);
    ATRACE_END();
    if (revoke_ros_alloc_thread_local_buffers_at_checkpoint_) {
      ATRACE_BEGIN("RevokeRosAllocThreadLocalBuffers");
      mark_sweep_->GetHeap()->RevokeRosAllocThreadLocalBuffers(thread);
      ATRACE_END();
    }
    mark_sweep_->GetBarrier().Pass(self);
  }

 private:
  MarkSweep* const mark_sweep_;
  const bool revoke_ros_alloc_thread_local_buffers_at_checkpoint_;
};

void MarkSweep::MarkRootsCheckpoint(Thread* self,
                                    bool revoke_ros_alloc_thread_local_buffers_at_checkpoint) {
  CheckpointMarkThreadRoots check_point(this, revoke_ros_alloc_thread_local_buffers_at_checkpoint);
  timings_.StartSplit("MarkRootsCheckpoint");
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  // Request the check point is run on all threads returning a count of the threads that must
  // run through the barrier including self.
  size_t barrier_count = thread_list->RunCheckpoint(&check_point);
  // Release locks then wait for all mutator threads to pass the barrier.
  // TODO: optimize to not release locks when there are no threads to wait for.
  Locks::heap_bitmap_lock_->ExclusiveUnlock(self);
  Locks::mutator_lock_->SharedUnlock(self);
  {
    ScopedThreadStateChange tsc(self, kWaitingForCheckPointsToRun);
    gc_barrier_->Increment(self, barrier_count);
  }
  Locks::mutator_lock_->SharedLock(self);
  Locks::heap_bitmap_lock_->ExclusiveLock(self);
  timings_.EndSplit();
}

void MarkSweep::SweepArray(accounting::ObjectStack* allocations, bool swap_bitmaps) {
  timings_.StartSplit("SweepArray");
  Thread* self = Thread::Current();
  mirror::Object* chunk_free_buffer[kSweepArrayChunkFreeSize];
  size_t chunk_free_pos = 0;
  size_t freed_bytes = 0;
  size_t freed_large_object_bytes = 0;
  size_t freed_objects = 0;
  size_t freed_large_objects = 0;
  // How many objects are left in the array, modified after each space is swept.
  Object** objects = allocations->Begin();
  size_t count = allocations->Size();
  // Change the order to ensure that the non-moving space last swept as an optimization.
  std::vector<space::ContinuousSpace*> sweep_spaces;
  space::ContinuousSpace* non_moving_space = nullptr;
  for (space::ContinuousSpace* space : heap_->GetContinuousSpaces()) {
    if (space->IsAllocSpace() && !immune_region_.ContainsSpace(space) &&
        space->GetLiveBitmap() != nullptr) {
      if (space == heap_->GetNonMovingSpace()) {
        non_moving_space = space;
      } else {
        sweep_spaces.push_back(space);
      }
    }
  }
  // Unlikely to sweep a significant amount of non_movable objects, so we do these after the after
  // the other alloc spaces as an optimization.
  if (non_moving_space != nullptr) {
    sweep_spaces.push_back(non_moving_space);
  }
  // Start by sweeping the continuous spaces.
  for (space::ContinuousSpace* space : sweep_spaces) {
    space::AllocSpace* alloc_space = space->AsAllocSpace();
    accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    if (swap_bitmaps) {
      std::swap(live_bitmap, mark_bitmap);
    }
    Object** out = objects;
    for (size_t i = 0; i < count; ++i) {
      Object* obj = objects[i];
      if (kUseThreadLocalAllocationStack && obj == nullptr) {
        continue;
      }
      if (space->HasAddress(obj)) {
        // This object is in the space, remove it from the array and add it to the sweep buffer
        // if needed.
        if (!mark_bitmap->Test(obj)) {
          if (chunk_free_pos >= kSweepArrayChunkFreeSize) {
            timings_.StartSplit("FreeList");
            freed_objects += chunk_free_pos;
            freed_bytes += alloc_space->FreeList(self, chunk_free_pos, chunk_free_buffer);
            timings_.EndSplit();
            chunk_free_pos = 0;
          }
          chunk_free_buffer[chunk_free_pos++] = obj;
        }
      } else {
        *(out++) = obj;
      }
    }
    if (chunk_free_pos > 0) {
      timings_.StartSplit("FreeList");
      freed_objects += chunk_free_pos;
      freed_bytes += alloc_space->FreeList(self, chunk_free_pos, chunk_free_buffer);
      timings_.EndSplit();
      chunk_free_pos = 0;
    }
    // All of the references which space contained are no longer in the allocation stack, update
    // the count.
    count = out - objects;
  }
  // Handle the large object space.
  space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  accounting::LargeObjectBitmap* large_live_objects = large_object_space->GetLiveBitmap();
  accounting::LargeObjectBitmap* large_mark_objects = large_object_space->GetMarkBitmap();
  if (swap_bitmaps) {
    std::swap(large_live_objects, large_mark_objects);
  }
  for (size_t i = 0; i < count; ++i) {
    Object* obj = objects[i];
    // Handle large objects.
    if (kUseThreadLocalAllocationStack && obj == nullptr) {
      continue;
    }
    if (!large_mark_objects->Test(obj)) {
      ++freed_large_objects;
      freed_large_object_bytes += large_object_space->Free(self, obj);
    }
  }
  timings_.EndSplit();

  timings_.StartSplit("RecordFree");
  VLOG(heap) << "Freed " << freed_objects << "/" << count << " objects with size "
             << PrettySize(freed_bytes);
  RecordFree(freed_objects, freed_bytes);
  RecordFreeLargeObjects(freed_large_objects, freed_large_object_bytes);
  timings_.EndSplit();

  timings_.StartSplit("ResetStack");
  allocations->Reset();
  timings_.EndSplit();
}

void MarkSweep::Sweep(bool swap_bitmaps) {
  // Ensure that nobody inserted items in the live stack after we swapped the stacks.
  CHECK_GE(live_stack_freeze_size_, GetHeap()->GetLiveStack()->Size());
  // Mark everything allocated since the last as GC live so that we can sweep concurrently,
  // knowing that new allocations won't be marked as live.
  timings_.StartSplit("MarkStackAsLive");
  accounting::ObjectStack* live_stack = heap_->GetLiveStack();
  heap_->MarkAllocStackAsLive(live_stack);
  live_stack->Reset();
  timings_.EndSplit();

  DCHECK(mark_stack_->IsEmpty());
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      TimingLogger::ScopedSplit split(
          alloc_space->IsZygoteSpace() ? "SweepZygoteSpace" : "SweepMallocSpace", &timings_);
      size_t freed_objects = 0;
      size_t freed_bytes = 0;
      alloc_space->Sweep(swap_bitmaps, &freed_objects, &freed_bytes);
      RecordFree(freed_objects, freed_bytes);
    }
  }
  SweepLargeObjects(swap_bitmaps);
}

void MarkSweep::SweepLargeObjects(bool swap_bitmaps) {
  TimingLogger::ScopedSplit split("SweepLargeObjects", &timings_);
  size_t freed_objects = 0;
  size_t freed_bytes = 0;
  heap_->GetLargeObjectsSpace()->Sweep(swap_bitmaps, &freed_objects, &freed_bytes);
  RecordFreeLargeObjects(freed_objects, freed_bytes);
}

// Process the "referent" field in a java.lang.ref.Reference.  If the referent has not yet been
// marked, put it on the appropriate list in the heap for later processing.
void MarkSweep::DelayReferenceReferent(mirror::Class* klass, mirror::Reference* ref) {
  DCHECK(klass != nullptr);
  if (kCountJavaLangRefs) {
    ++reference_count_;
  }
  heap_->GetReferenceProcessor()->DelayReferenceReferent(klass, ref, IsMarkedCallback, this);
}

class MarkObjectVisitor {
 public:
  explicit MarkObjectVisitor(MarkSweep* const mark_sweep) ALWAYS_INLINE : mark_sweep_(mark_sweep) {
  }

  void operator()(Object* obj, MemberOffset offset, bool /* is_static */) const
      ALWAYS_INLINE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    if (kCheckLocks) {
      Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    mark_sweep_->MarkObject(obj->GetFieldObject<mirror::Object>(offset));
  }

 private:
  MarkSweep* const mark_sweep_;
};

// Scans an object reference.  Determines the type of the reference
// and dispatches to a specialized scanning routine.
void MarkSweep::ScanObject(Object* obj) {
  MarkObjectVisitor mark_visitor(this);
  DelayReferenceReferentVisitor ref_visitor(this);
  ScanObjectVisit(obj, mark_visitor, ref_visitor);
}

void MarkSweep::ProcessMarkStackCallback(void* arg) {
  reinterpret_cast<MarkSweep*>(arg)->ProcessMarkStack(false);
}

void MarkSweep::ProcessMarkStackParallel(size_t thread_count) {
  Thread* self = Thread::Current();
  ThreadPool* thread_pool = GetHeap()->GetThreadPool();
  const size_t chunk_size = std::min(mark_stack_->Size() / thread_count + 1,
                                     static_cast<size_t>(MarkStackTask<false>::kMaxSize));
  CHECK_GT(chunk_size, 0U);
  // Split the current mark stack up into work tasks.
  for (mirror::Object **it = mark_stack_->Begin(), **end = mark_stack_->End(); it < end; ) {
    const size_t delta = std::min(static_cast<size_t>(end - it), chunk_size);
    thread_pool->AddTask(self, new MarkStackTask<false>(thread_pool, this, delta, it));
    it += delta;
  }
  thread_pool->SetMaxActiveWorkers(thread_count - 1);
  thread_pool->StartWorkers(self);
  thread_pool->Wait(self, true, true);
  thread_pool->StopWorkers(self);
  mark_stack_->Reset();
  CHECK_EQ(work_chunks_created_, work_chunks_deleted_) << " some of the work chunks were leaked";
}

// Scan anything that's on the mark stack.
void MarkSweep::ProcessMarkStack(bool paused) {
  timings_.StartSplit(paused ? "(Paused)ProcessMarkStack" : "ProcessMarkStack");
  size_t thread_count = GetThreadCount(paused);
  if (kParallelProcessMarkStack && thread_count > 1 &&
      mark_stack_->Size() >= kMinimumParallelMarkStackSize) {
    ProcessMarkStackParallel(thread_count);
  } else {
    // TODO: Tune this.
    static const size_t kFifoSize = 4;
    BoundedFifoPowerOfTwo<Object*, kFifoSize> prefetch_fifo;
    for (;;) {
      Object* obj = NULL;
      if (kUseMarkStackPrefetch) {
        while (!mark_stack_->IsEmpty() && prefetch_fifo.size() < kFifoSize) {
          Object* obj = mark_stack_->PopBack();
          DCHECK(obj != NULL);
          __builtin_prefetch(obj);
          prefetch_fifo.push_back(obj);
        }
        if (prefetch_fifo.empty()) {
          break;
        }
        obj = prefetch_fifo.front();
        prefetch_fifo.pop_front();
      } else {
        if (mark_stack_->IsEmpty()) {
          break;
        }
        obj = mark_stack_->PopBack();
      }
      DCHECK(obj != nullptr);
      ScanObject(obj);
    }
  }
  timings_.EndSplit();
}

inline bool MarkSweep::IsMarked(const Object* object) const
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
  if (immune_region_.ContainsObject(object)) {
    return true;
  }
  if (current_space_bitmap_->HasAddress(object)) {
    return current_space_bitmap_->Test(object);
  }
  return mark_bitmap_->Test(object);
}

void MarkSweep::FinishPhase() {
  TimingLogger::ScopedSplit split("FinishPhase", &timings_);
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
  if (kCountJavaLangRefs) {
    VLOG(gc) << "References scanned " << reference_count_;
  }
  if (kCountMarkedObjects) {
    VLOG(gc) << "Marked: null=" << mark_null_count_ << " immune=" <<  mark_immune_count_
        << " fastpath=" << mark_fastpath_count_ << " slowpath=" << mark_slowpath_count_;
  }
  CHECK(mark_stack_->IsEmpty());  // Ensure that the mark stack is empty.
  mark_stack_->Reset();
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  heap_->ClearMarkedObjects();
}

void MarkSweep::RevokeAllThreadLocalBuffers() {
  if (kRevokeRosAllocThreadLocalBuffersAtCheckpoint && IsConcurrent()) {
    // If concurrent, rosalloc thread-local buffers are revoked at the
    // thread checkpoint. Bump pointer space thread-local buffers must
    // not be in use.
    GetHeap()->AssertAllBumpPointerSpaceThreadLocalBuffersAreRevoked();
  } else {
    timings_.StartSplit("(Paused)RevokeAllThreadLocalBuffers");
    GetHeap()->RevokeAllThreadLocalBuffers();
    timings_.EndSplit();
  }
}

}  // namespace collector
}  // namespace gc
}  // namespace art
