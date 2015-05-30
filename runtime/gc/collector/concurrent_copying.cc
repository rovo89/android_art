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

#include "concurrent_copying.h"

#include "art_field-inl.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/space.h"
#include "intern_table.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "well_known_classes.h"

namespace art {
namespace gc {
namespace collector {

ConcurrentCopying::ConcurrentCopying(Heap* heap, const std::string& name_prefix)
    : GarbageCollector(heap,
                       name_prefix + (name_prefix.empty() ? "" : " ") +
                       "concurrent copying + mark sweep"),
      region_space_(nullptr), gc_barrier_(new Barrier(0)), mark_queue_(2 * MB),
      is_marking_(false), is_active_(false), is_asserting_to_space_invariant_(false),
      heap_mark_bitmap_(nullptr), live_stack_freeze_size_(0),
      skipped_blocks_lock_("concurrent copying bytes blocks lock", kMarkSweepMarkStackLock),
      rb_table_(heap_->GetReadBarrierTable()),
      force_evacuate_all_(false) {
  static_assert(space::RegionSpace::kRegionSize == accounting::ReadBarrierTable::kRegionSize,
                "The region space size and the read barrier table region size must match");
  cc_heap_bitmap_.reset(new accounting::HeapBitmap(heap));
  {
    Thread* self = Thread::Current();
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // Cache this so that we won't have to lock heap_bitmap_lock_ in
    // Mark() which could cause a nested lock on heap_bitmap_lock_
    // when GC causes a RB while doing GC or a lock order violation
    // (class_linker_lock_ and heap_bitmap_lock_).
    heap_mark_bitmap_ = heap->GetMarkBitmap();
  }
}

ConcurrentCopying::~ConcurrentCopying() {
}

void ConcurrentCopying::RunPhases() {
  CHECK(kUseBakerReadBarrier || kUseTableLookupReadBarrier);
  CHECK(!is_active_);
  is_active_ = true;
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  {
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    InitializePhase();
  }
  FlipThreadRoots();
  {
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    MarkingPhase();
  }
  // Verify no from space refs. This causes a pause.
  if (kEnableNoFromSpaceRefsVerification || kIsDebugBuild) {
    TimingLogger::ScopedTiming split("(Paused)VerifyNoFromSpaceReferences", GetTimings());
    ScopedPause pause(this);
    CheckEmptyMarkQueue();
    if (kVerboseMode) {
      LOG(INFO) << "Verifying no from-space refs";
    }
    VerifyNoFromSpaceReferences();
    if (kVerboseMode) {
      LOG(INFO) << "Done verifying no from-space refs";
    }
    CheckEmptyMarkQueue();
  }
  {
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    ReclaimPhase();
  }
  FinishPhase();
  CHECK(is_active_);
  is_active_ = false;
}

void ConcurrentCopying::BindBitmaps() {
  Thread* self = Thread::Current();
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  // Mark all of the spaces we never collect as immune.
  for (const auto& space : heap_->GetContinuousSpaces()) {
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyNeverCollect
        || space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect) {
      CHECK(space->IsZygoteSpace() || space->IsImageSpace());
      CHECK(immune_region_.AddContinuousSpace(space)) << "Failed to add space " << *space;
      const char* bitmap_name = space->IsImageSpace() ? "cc image space bitmap" :
          "cc zygote space bitmap";
      // TODO: try avoiding using bitmaps for image/zygote to save space.
      accounting::ContinuousSpaceBitmap* bitmap =
          accounting::ContinuousSpaceBitmap::Create(bitmap_name, space->Begin(), space->Capacity());
      cc_heap_bitmap_->AddContinuousSpaceBitmap(bitmap);
      cc_bitmaps_.push_back(bitmap);
    } else if (space == region_space_) {
      accounting::ContinuousSpaceBitmap* bitmap =
          accounting::ContinuousSpaceBitmap::Create("cc region space bitmap",
                                                    space->Begin(), space->Capacity());
      cc_heap_bitmap_->AddContinuousSpaceBitmap(bitmap);
      cc_bitmaps_.push_back(bitmap);
      region_space_bitmap_ = bitmap;
    }
  }
}

void ConcurrentCopying::InitializePhase() {
  TimingLogger::ScopedTiming split("InitializePhase", GetTimings());
  if (kVerboseMode) {
    LOG(INFO) << "GC InitializePhase";
    LOG(INFO) << "Region-space : " << reinterpret_cast<void*>(region_space_->Begin()) << "-"
              << reinterpret_cast<void*>(region_space_->Limit());
  }
  CHECK(mark_queue_.IsEmpty());
  immune_region_.Reset();
  bytes_moved_.StoreRelaxed(0);
  objects_moved_.StoreRelaxed(0);
  if (GetCurrentIteration()->GetGcCause() == kGcCauseExplicit ||
      GetCurrentIteration()->GetGcCause() == kGcCauseForNativeAlloc ||
      GetCurrentIteration()->GetClearSoftReferences()) {
    force_evacuate_all_ = true;
  } else {
    force_evacuate_all_ = false;
  }
  BindBitmaps();
  if (kVerboseMode) {
    LOG(INFO) << "force_evacuate_all=" << force_evacuate_all_;
    LOG(INFO) << "Immune region: " << immune_region_.Begin() << "-" << immune_region_.End();
    LOG(INFO) << "GC end of InitializePhase";
  }
}

// Used to switch the thread roots of a thread from from-space refs to to-space refs.
class ThreadFlipVisitor : public Closure {
 public:
  explicit ThreadFlipVisitor(ConcurrentCopying* concurrent_copying, bool use_tlab)
      : concurrent_copying_(concurrent_copying), use_tlab_(use_tlab) {
  }

  virtual void Run(Thread* thread) OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* self = Thread::Current();
    CHECK(thread == self || thread->IsSuspended() || thread->GetState() == kWaitingPerformingGc)
        << thread->GetState() << " thread " << thread << " self " << self;
    if (use_tlab_ && thread->HasTlab()) {
      if (ConcurrentCopying::kEnableFromSpaceAccountingCheck) {
        // This must come before the revoke.
        size_t thread_local_objects = thread->GetThreadLocalObjectsAllocated();
        concurrent_copying_->region_space_->RevokeThreadLocalBuffers(thread);
        reinterpret_cast<Atomic<size_t>*>(&concurrent_copying_->from_space_num_objects_at_first_pause_)->
            FetchAndAddSequentiallyConsistent(thread_local_objects);
      } else {
        concurrent_copying_->region_space_->RevokeThreadLocalBuffers(thread);
      }
    }
    if (kUseThreadLocalAllocationStack) {
      thread->RevokeThreadLocalAllocationStack();
    }
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    thread->VisitRoots(concurrent_copying_);
    concurrent_copying_->GetBarrier().Pass(self);
  }

 private:
  ConcurrentCopying* const concurrent_copying_;
  const bool use_tlab_;
};

// Called back from Runtime::FlipThreadRoots() during a pause.
class FlipCallback : public Closure {
 public:
  explicit FlipCallback(ConcurrentCopying* concurrent_copying)
      : concurrent_copying_(concurrent_copying) {
  }

  virtual void Run(Thread* thread) OVERRIDE EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ConcurrentCopying* cc = concurrent_copying_;
    TimingLogger::ScopedTiming split("(Paused)FlipCallback", cc->GetTimings());
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* self = Thread::Current();
    CHECK(thread == self);
    Locks::mutator_lock_->AssertExclusiveHeld(self);
    cc->region_space_->SetFromSpace(cc->rb_table_, cc->force_evacuate_all_);
    cc->SwapStacks(self);
    if (ConcurrentCopying::kEnableFromSpaceAccountingCheck) {
      cc->RecordLiveStackFreezeSize(self);
      cc->from_space_num_objects_at_first_pause_ = cc->region_space_->GetObjectsAllocated();
      cc->from_space_num_bytes_at_first_pause_ = cc->region_space_->GetBytesAllocated();
    }
    cc->is_marking_ = true;
    if (UNLIKELY(Runtime::Current()->IsActiveTransaction())) {
      CHECK(Runtime::Current()->IsAotCompiler());
      TimingLogger::ScopedTiming split2("(Paused)VisitTransactionRoots", cc->GetTimings());
      Runtime::Current()->VisitTransactionRoots(cc);
    }
  }

 private:
  ConcurrentCopying* const concurrent_copying_;
};

// Switch threads that from from-space to to-space refs. Forward/mark the thread roots.
void ConcurrentCopying::FlipThreadRoots() {
  TimingLogger::ScopedTiming split("FlipThreadRoots", GetTimings());
  if (kVerboseMode) {
    LOG(INFO) << "time=" << region_space_->Time();
    region_space_->DumpNonFreeRegions(LOG(INFO));
  }
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  gc_barrier_->Init(self, 0);
  ThreadFlipVisitor thread_flip_visitor(this, heap_->use_tlab_);
  FlipCallback flip_callback(this);
  size_t barrier_count = Runtime::Current()->FlipThreadRoots(
      &thread_flip_visitor, &flip_callback, this);
  {
    ScopedThreadStateChange tsc(self, kWaitingForCheckPointsToRun);
    gc_barrier_->Increment(self, barrier_count);
  }
  is_asserting_to_space_invariant_ = true;
  QuasiAtomic::ThreadFenceForConstructor();
  if (kVerboseMode) {
    LOG(INFO) << "time=" << region_space_->Time();
    region_space_->DumpNonFreeRegions(LOG(INFO));
    LOG(INFO) << "GC end of FlipThreadRoots";
  }
}

void ConcurrentCopying::SwapStacks(Thread* self) {
  heap_->SwapStacks(self);
}

void ConcurrentCopying::RecordLiveStackFreezeSize(Thread* self) {
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  live_stack_freeze_size_ = heap_->GetLiveStack()->Size();
}

// Used to visit objects in the immune spaces.
class ConcurrentCopyingImmuneSpaceObjVisitor {
 public:
  explicit ConcurrentCopyingImmuneSpaceObjVisitor(ConcurrentCopying* cc)
      : collector_(cc) {}

  void operator()(mirror::Object* obj) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    DCHECK(obj != nullptr);
    DCHECK(collector_->immune_region_.ContainsObject(obj));
    accounting::ContinuousSpaceBitmap* cc_bitmap =
        collector_->cc_heap_bitmap_->GetContinuousSpaceBitmap(obj);
    DCHECK(cc_bitmap != nullptr)
        << "An immune space object must have a bitmap";
    if (kIsDebugBuild) {
      DCHECK(collector_->heap_->GetMarkBitmap()->Test(obj))
          << "Immune space object must be already marked";
    }
    // This may or may not succeed, which is ok.
    if (kUseBakerReadBarrier) {
      obj->AtomicSetReadBarrierPointer(ReadBarrier::WhitePtr(), ReadBarrier::GrayPtr());
    }
    if (cc_bitmap->AtomicTestAndSet(obj)) {
      // Already marked. Do nothing.
    } else {
      // Newly marked. Set the gray bit and push it onto the mark stack.
      CHECK(!kUseBakerReadBarrier || obj->GetReadBarrierPointer() == ReadBarrier::GrayPtr());
      collector_->PushOntoMarkStack<true>(obj);
    }
  }

 private:
  ConcurrentCopying* collector_;
};

class EmptyCheckpoint : public Closure {
 public:
  explicit EmptyCheckpoint(ConcurrentCopying* concurrent_copying)
      : concurrent_copying_(concurrent_copying) {
  }

  virtual void Run(Thread* thread) OVERRIDE NO_THREAD_SAFETY_ANALYSIS {
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* self = Thread::Current();
    CHECK(thread == self || thread->IsSuspended() || thread->GetState() == kWaitingPerformingGc)
        << thread->GetState() << " thread " << thread << " self " << self;
    // If thread is a running mutator, then act on behalf of the garbage collector.
    // See the code in ThreadList::RunCheckpoint.
    if (thread->GetState() == kRunnable) {
      concurrent_copying_->GetBarrier().Pass(self);
    }
  }

 private:
  ConcurrentCopying* const concurrent_copying_;
};

// Concurrently mark roots that are guarded by read barriers and process the mark stack.
void ConcurrentCopying::MarkingPhase() {
  TimingLogger::ScopedTiming split("MarkingPhase", GetTimings());
  if (kVerboseMode) {
    LOG(INFO) << "GC MarkingPhase";
  }
  {
    // Mark the image root. The WB-based collectors do not need to
    // scan the image objects from roots by relying on the card table,
    // but it's necessary for the RB to-space invariant to hold.
    TimingLogger::ScopedTiming split1("VisitImageRoots", GetTimings());
    gc::space::ImageSpace* image = heap_->GetImageSpace();
    if (image != nullptr) {
      mirror::ObjectArray<mirror::Object>* image_root = image->GetImageHeader().GetImageRoots();
      mirror::Object* marked_image_root = Mark(image_root);
      CHECK_EQ(image_root, marked_image_root) << "An image object does not move";
      if (ReadBarrier::kEnableToSpaceInvariantChecks) {
        AssertToSpaceInvariant(nullptr, MemberOffset(0), marked_image_root);
      }
    }
  }
  {
    TimingLogger::ScopedTiming split2("VisitConstantRoots", GetTimings());
    Runtime::Current()->VisitConstantRoots(this);
  }
  {
    TimingLogger::ScopedTiming split3("VisitInternTableRoots", GetTimings());
    Runtime::Current()->GetInternTable()->VisitRoots(this, kVisitRootFlagAllRoots);
  }
  {
    TimingLogger::ScopedTiming split4("VisitClassLinkerRoots", GetTimings());
    Runtime::Current()->GetClassLinker()->VisitRoots(this, kVisitRootFlagAllRoots);
  }
  {
    // TODO: don't visit the transaction roots if it's not active.
    TimingLogger::ScopedTiming split5("VisitNonThreadRoots", GetTimings());
    Runtime::Current()->VisitNonThreadRoots(this);
  }

  // Immune spaces.
  for (auto& space : heap_->GetContinuousSpaces()) {
    if (immune_region_.ContainsSpace(space)) {
      DCHECK(space->IsImageSpace() || space->IsZygoteSpace());
      accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
      ConcurrentCopyingImmuneSpaceObjVisitor visitor(this);
      live_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(space->Begin()),
                                    reinterpret_cast<uintptr_t>(space->Limit()),
                                    visitor);
    }
  }

  Thread* self = Thread::Current();
  {
    TimingLogger::ScopedTiming split6("ProcessMarkStack", GetTimings());
    // Process the mark stack and issue an empty check point. If the
    // mark stack is still empty after the check point, we're
    // done. Otherwise, repeat.
    ProcessMarkStack();
    size_t count = 0;
    while (!ProcessMarkStack()) {
      ++count;
      if (kVerboseMode) {
        LOG(INFO) << "Issue an empty check point. " << count;
      }
      IssueEmptyCheckpoint();
    }
    // Need to ensure the mark stack is empty before reference
    // processing to get rid of non-reference gray objects.
    CheckEmptyMarkQueue();
    // Enable the GetReference slow path and disallow access to the system weaks.
    GetHeap()->GetReferenceProcessor()->EnableSlowPath();
    Runtime::Current()->DisallowNewSystemWeaks();
    QuasiAtomic::ThreadFenceForConstructor();
    // Lock-unlock the system weak locks so that there's no thread in
    // the middle of accessing system weaks.
    Runtime::Current()->EnsureNewSystemWeaksDisallowed();
    // Note: Do not issue a checkpoint from here to the
    // SweepSystemWeaks call or else a deadlock due to
    // WaitHoldingLocks() would occur.
    if (kVerboseMode) {
      LOG(INFO) << "Enabled the ref proc slow path & disabled access to system weaks.";
      LOG(INFO) << "ProcessReferences";
    }
    ProcessReferences(self, true);
    CheckEmptyMarkQueue();
    if (kVerboseMode) {
      LOG(INFO) << "SweepSystemWeaks";
    }
    SweepSystemWeaks(self);
    if (kVerboseMode) {
      LOG(INFO) << "SweepSystemWeaks done";
    }
    // Because hash_set::Erase() can call the hash function for
    // arbitrary elements in the weak intern table in
    // InternTable::Table::SweepWeaks(), the above SweepSystemWeaks()
    // call may have marked some objects (strings) alive. So process
    // the mark stack here once again.
    ProcessMarkStack();
    CheckEmptyMarkQueue();
    // Disable marking.
    if (kUseTableLookupReadBarrier) {
      heap_->rb_table_->ClearAll();
      DCHECK(heap_->rb_table_->IsAllCleared());
    }
    is_mark_queue_push_disallowed_.StoreSequentiallyConsistent(1);
    is_marking_ = false;
    if (kVerboseMode) {
      LOG(INFO) << "AllowNewSystemWeaks";
    }
    Runtime::Current()->AllowNewSystemWeaks();
    CheckEmptyMarkQueue();
  }

  if (kVerboseMode) {
    LOG(INFO) << "GC end of MarkingPhase";
  }
}

void ConcurrentCopying::IssueEmptyCheckpoint() {
  Thread* self = Thread::Current();
  EmptyCheckpoint check_point(this);
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  gc_barrier_->Init(self, 0);
  size_t barrier_count = thread_list->RunCheckpoint(&check_point);
  // If there are no threads to wait which implys that all the checkpoint functions are finished,
  // then no need to release the mutator lock.
  if (barrier_count == 0) {
    return;
  }
  // Release locks then wait for all mutator threads to pass the barrier.
  Locks::mutator_lock_->SharedUnlock(self);
  {
    ScopedThreadStateChange tsc(self, kWaitingForCheckPointsToRun);
    gc_barrier_->Increment(self, barrier_count);
  }
  Locks::mutator_lock_->SharedLock(self);
}

mirror::Object* ConcurrentCopying::PopOffMarkStack() {
  return mark_queue_.Dequeue();
}

template<bool kThreadSafe>
void ConcurrentCopying::PushOntoMarkStack(mirror::Object* to_ref) {
  CHECK_EQ(is_mark_queue_push_disallowed_.LoadRelaxed(), 0)
      << " " << to_ref << " " << PrettyTypeOf(to_ref);
  if (kThreadSafe) {
    CHECK(mark_queue_.Enqueue(to_ref)) << "Mark queue overflow";
  } else {
    CHECK(mark_queue_.EnqueueThreadUnsafe(to_ref)) << "Mark queue overflow";
  }
}

accounting::ObjectStack* ConcurrentCopying::GetAllocationStack() {
  return heap_->allocation_stack_.get();
}

accounting::ObjectStack* ConcurrentCopying::GetLiveStack() {
  return heap_->live_stack_.get();
}

inline mirror::Object* ConcurrentCopying::GetFwdPtr(mirror::Object* from_ref) {
  DCHECK(region_space_->IsInFromSpace(from_ref));
  LockWord lw = from_ref->GetLockWord(false);
  if (lw.GetState() == LockWord::kForwardingAddress) {
    mirror::Object* fwd_ptr = reinterpret_cast<mirror::Object*>(lw.ForwardingAddress());
    CHECK(fwd_ptr != nullptr);
    return fwd_ptr;
  } else {
    return nullptr;
  }
}

// The following visitors are that used to verify that there's no
// references to the from-space left after marking.
class ConcurrentCopyingVerifyNoFromSpaceRefsVisitor : public SingleRootVisitor {
 public:
  explicit ConcurrentCopyingVerifyNoFromSpaceRefsVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}

  void operator()(mirror::Object* ref) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    if (ref == nullptr) {
      // OK.
      return;
    }
    collector_->AssertToSpaceInvariant(nullptr, MemberOffset(0), ref);
    if (kUseBakerReadBarrier) {
      if (collector_->RegionSpace()->IsInToSpace(ref)) {
        CHECK(ref->GetReadBarrierPointer() == nullptr)
            << "To-space ref " << ref << " " << PrettyTypeOf(ref)
            << " has non-white rb_ptr " << ref->GetReadBarrierPointer();
      } else {
        CHECK(ref->GetReadBarrierPointer() == ReadBarrier::BlackPtr() ||
              (ref->GetReadBarrierPointer() == ReadBarrier::WhitePtr() &&
               collector_->IsOnAllocStack(ref)))
            << "Non-moving/unevac from space ref " << ref << " " << PrettyTypeOf(ref)
            << " has non-black rb_ptr " << ref->GetReadBarrierPointer()
            << " but isn't on the alloc stack (and has white rb_ptr)."
            << " Is it in the non-moving space="
            << (collector_->GetHeap()->GetNonMovingSpace()->HasAddress(ref));
      }
    }
  }

  void VisitRoot(mirror::Object* root, const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(root != nullptr);
    operator()(root);
  }

 private:
  ConcurrentCopying* const collector_;
};

class ConcurrentCopyingVerifyNoFromSpaceRefsFieldVisitor {
 public:
  explicit ConcurrentCopyingVerifyNoFromSpaceRefsFieldVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}

  void operator()(mirror::Object* obj, MemberOffset offset, bool /* is_static */) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    mirror::Object* ref =
        obj->GetFieldObject<mirror::Object, kDefaultVerifyFlags, kWithoutReadBarrier>(offset);
    ConcurrentCopyingVerifyNoFromSpaceRefsVisitor visitor(collector_);
    visitor(ref);
  }
  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    CHECK(klass->IsTypeOfReferenceClass());
    this->operator()(ref, mirror::Reference::ReferentOffset(), false);
  }

 private:
  ConcurrentCopying* collector_;
};

class ConcurrentCopyingVerifyNoFromSpaceRefsObjectVisitor {
 public:
  explicit ConcurrentCopyingVerifyNoFromSpaceRefsObjectVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}
  void operator()(mirror::Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ObjectCallback(obj, collector_);
  }
  static void ObjectCallback(mirror::Object* obj, void *arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(obj != nullptr);
    ConcurrentCopying* collector = reinterpret_cast<ConcurrentCopying*>(arg);
    space::RegionSpace* region_space = collector->RegionSpace();
    CHECK(!region_space->IsInFromSpace(obj)) << "Scanning object " << obj << " in from space";
    ConcurrentCopyingVerifyNoFromSpaceRefsFieldVisitor visitor(collector);
    obj->VisitReferences<true>(visitor, visitor);
    if (kUseBakerReadBarrier) {
      if (collector->RegionSpace()->IsInToSpace(obj)) {
        CHECK(obj->GetReadBarrierPointer() == nullptr)
            << "obj=" << obj << " non-white rb_ptr " << obj->GetReadBarrierPointer();
      } else {
        CHECK(obj->GetReadBarrierPointer() == ReadBarrier::BlackPtr() ||
              (obj->GetReadBarrierPointer() == ReadBarrier::WhitePtr() &&
               collector->IsOnAllocStack(obj)))
            << "Non-moving space/unevac from space ref " << obj << " " << PrettyTypeOf(obj)
            << " has non-black rb_ptr " << obj->GetReadBarrierPointer()
            << " but isn't on the alloc stack (and has white rb_ptr). Is it in the non-moving space="
            << (collector->GetHeap()->GetNonMovingSpace()->HasAddress(obj));
      }
    }
  }

 private:
  ConcurrentCopying* const collector_;
};

// Verify there's no from-space references left after the marking phase.
void ConcurrentCopying::VerifyNoFromSpaceReferences() {
  Thread* self = Thread::Current();
  DCHECK(Locks::mutator_lock_->IsExclusiveHeld(self));
  ConcurrentCopyingVerifyNoFromSpaceRefsObjectVisitor visitor(this);
  // Roots.
  {
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    ConcurrentCopyingVerifyNoFromSpaceRefsVisitor ref_visitor(this);
    Runtime::Current()->VisitRoots(&ref_visitor);
  }
  // The to-space.
  region_space_->WalkToSpace(ConcurrentCopyingVerifyNoFromSpaceRefsObjectVisitor::ObjectCallback,
                             this);
  // Non-moving spaces.
  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    heap_->GetMarkBitmap()->Visit(visitor);
  }
  // The alloc stack.
  {
    ConcurrentCopyingVerifyNoFromSpaceRefsVisitor ref_visitor(this);
    for (auto* it = heap_->allocation_stack_->Begin(), *end = heap_->allocation_stack_->End();
        it < end; ++it) {
      mirror::Object* const obj = it->AsMirrorPtr();
      if (obj != nullptr && obj->GetClass() != nullptr) {
        // TODO: need to call this only if obj is alive?
        ref_visitor(obj);
        visitor(obj);
      }
    }
  }
  // TODO: LOS. But only refs in LOS are classes.
}

// The following visitors are used to assert the to-space invariant.
class ConcurrentCopyingAssertToSpaceInvariantRefsVisitor {
 public:
  explicit ConcurrentCopyingAssertToSpaceInvariantRefsVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}

  void operator()(mirror::Object* ref) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    if (ref == nullptr) {
      // OK.
      return;
    }
    collector_->AssertToSpaceInvariant(nullptr, MemberOffset(0), ref);
  }
  static void RootCallback(mirror::Object** root, void *arg, const RootInfo& /*root_info*/)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ConcurrentCopying* collector = reinterpret_cast<ConcurrentCopying*>(arg);
    ConcurrentCopyingAssertToSpaceInvariantRefsVisitor visitor(collector);
    DCHECK(root != nullptr);
    visitor(*root);
  }

 private:
  ConcurrentCopying* collector_;
};

class ConcurrentCopyingAssertToSpaceInvariantFieldVisitor {
 public:
  explicit ConcurrentCopyingAssertToSpaceInvariantFieldVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}

  void operator()(mirror::Object* obj, MemberOffset offset, bool /* is_static */) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    mirror::Object* ref =
        obj->GetFieldObject<mirror::Object, kDefaultVerifyFlags, kWithoutReadBarrier>(offset);
    ConcurrentCopyingAssertToSpaceInvariantRefsVisitor visitor(collector_);
    visitor(ref);
  }
  void operator()(mirror::Class* klass, mirror::Reference* /* ref */) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    CHECK(klass->IsTypeOfReferenceClass());
  }

 private:
  ConcurrentCopying* collector_;
};

class ConcurrentCopyingAssertToSpaceInvariantObjectVisitor {
 public:
  explicit ConcurrentCopyingAssertToSpaceInvariantObjectVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}
  void operator()(mirror::Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ObjectCallback(obj, collector_);
  }
  static void ObjectCallback(mirror::Object* obj, void *arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(obj != nullptr);
    ConcurrentCopying* collector = reinterpret_cast<ConcurrentCopying*>(arg);
    space::RegionSpace* region_space = collector->RegionSpace();
    CHECK(!region_space->IsInFromSpace(obj)) << "Scanning object " << obj << " in from space";
    collector->AssertToSpaceInvariant(nullptr, MemberOffset(0), obj);
    ConcurrentCopyingAssertToSpaceInvariantFieldVisitor visitor(collector);
    obj->VisitReferences<true>(visitor, visitor);
  }

 private:
  ConcurrentCopying* collector_;
};

bool ConcurrentCopying::ProcessMarkStack() {
  if (kVerboseMode) {
    LOG(INFO) << "ProcessMarkStack. ";
  }
  size_t count = 0;
  mirror::Object* to_ref;
  while ((to_ref = PopOffMarkStack()) != nullptr) {
    ++count;
    DCHECK(!region_space_->IsInFromSpace(to_ref));
    if (kUseBakerReadBarrier) {
      DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr())
          << " " << to_ref << " " << to_ref->GetReadBarrierPointer()
          << " is_marked=" << IsMarked(to_ref);
    }
    // Scan ref fields.
    Scan(to_ref);
    // Mark the gray ref as white or black.
    if (kUseBakerReadBarrier) {
      DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr())
          << " " << to_ref << " " << to_ref->GetReadBarrierPointer()
          << " is_marked=" << IsMarked(to_ref);
    }
    if (to_ref->GetClass<kVerifyNone, kWithoutReadBarrier>()->IsTypeOfReferenceClass() &&
        to_ref->AsReference()->GetReferent<kWithoutReadBarrier>() != nullptr &&
        !IsInToSpace(to_ref->AsReference()->GetReferent<kWithoutReadBarrier>())) {
      // Leave References gray so that GetReferent() will trigger RB.
      CHECK(to_ref->AsReference()->IsEnqueued()) << "Left unenqueued ref gray " << to_ref;
    } else {
#ifdef USE_BAKER_OR_BROOKS_READ_BARRIER
      if (kUseBakerReadBarrier) {
        if (region_space_->IsInToSpace(to_ref)) {
          // If to-space, change from gray to white.
          bool success = to_ref->AtomicSetReadBarrierPointer(ReadBarrier::GrayPtr(),
                                                             ReadBarrier::WhitePtr());
          CHECK(success) << "Must succeed as we won the race.";
          CHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::WhitePtr());
        } else {
          // If non-moving space/unevac from space, change from gray
          // to black. We can't change gray to white because it's not
          // safe to use CAS if two threads change values in opposite
          // directions (A->B and B->A). So, we change it to black to
          // indicate non-moving objects that have been marked
          // through. Note we'd need to change from black to white
          // later (concurrently).
          bool success = to_ref->AtomicSetReadBarrierPointer(ReadBarrier::GrayPtr(),
                                                             ReadBarrier::BlackPtr());
          CHECK(success) << "Must succeed as we won the race.";
          CHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::BlackPtr());
        }
      }
#else
      DCHECK(!kUseBakerReadBarrier);
#endif
    }
    if (ReadBarrier::kEnableToSpaceInvariantChecks || kIsDebugBuild) {
      ConcurrentCopyingAssertToSpaceInvariantObjectVisitor visitor(this);
      visitor(to_ref);
    }
  }
  // Return true if the stack was empty.
  return count == 0;
}

void ConcurrentCopying::CheckEmptyMarkQueue() {
  if (!mark_queue_.IsEmpty()) {
    while (!mark_queue_.IsEmpty()) {
      mirror::Object* obj = mark_queue_.Dequeue();
      if (kUseBakerReadBarrier) {
        mirror::Object* rb_ptr = obj->GetReadBarrierPointer();
        LOG(INFO) << "On mark queue : " << obj << " " << PrettyTypeOf(obj) << " rb_ptr=" << rb_ptr
                  << " is_marked=" << IsMarked(obj);
      } else {
        LOG(INFO) << "On mark queue : " << obj << " " << PrettyTypeOf(obj)
                  << " is_marked=" << IsMarked(obj);
      }
    }
    LOG(FATAL) << "mark queue is not empty";
  }
}

void ConcurrentCopying::SweepSystemWeaks(Thread* self) {
  TimingLogger::ScopedTiming split("SweepSystemWeaks", GetTimings());
  ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
  Runtime::Current()->SweepSystemWeaks(IsMarkedCallback, this);
}

void ConcurrentCopying::Sweep(bool swap_bitmaps) {
  {
    TimingLogger::ScopedTiming t("MarkStackAsLive", GetTimings());
    accounting::ObjectStack* live_stack = heap_->GetLiveStack();
    if (kEnableFromSpaceAccountingCheck) {
      CHECK_GE(live_stack_freeze_size_, live_stack->Size());
    }
    heap_->MarkAllocStackAsLive(live_stack);
    live_stack->Reset();
  }
  CHECK(mark_queue_.IsEmpty());
  TimingLogger::ScopedTiming split("Sweep", GetTimings());
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      if (space == region_space_ || immune_region_.ContainsSpace(space)) {
        continue;
      }
      TimingLogger::ScopedTiming split2(
          alloc_space->IsZygoteSpace() ? "SweepZygoteSpace" : "SweepAllocSpace", GetTimings());
      RecordFree(alloc_space->Sweep(swap_bitmaps));
    }
  }
  SweepLargeObjects(swap_bitmaps);
}

void ConcurrentCopying::SweepLargeObjects(bool swap_bitmaps) {
  TimingLogger::ScopedTiming split("SweepLargeObjects", GetTimings());
  RecordFreeLOS(heap_->GetLargeObjectsSpace()->Sweep(swap_bitmaps));
}

class ConcurrentCopyingClearBlackPtrsVisitor {
 public:
  explicit ConcurrentCopyingClearBlackPtrsVisitor(ConcurrentCopying* cc)
      : collector_(cc) {}
#ifndef USE_BAKER_OR_BROOKS_READ_BARRIER
  NO_RETURN
#endif
  void operator()(mirror::Object* obj) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    DCHECK(obj != nullptr);
    DCHECK(collector_->heap_->GetMarkBitmap()->Test(obj)) << obj;
    DCHECK_EQ(obj->GetReadBarrierPointer(), ReadBarrier::BlackPtr()) << obj;
    obj->AtomicSetReadBarrierPointer(ReadBarrier::BlackPtr(), ReadBarrier::WhitePtr());
    DCHECK_EQ(obj->GetReadBarrierPointer(), ReadBarrier::WhitePtr()) << obj;
  }

 private:
  ConcurrentCopying* const collector_;
};

// Clear the black ptrs in non-moving objects back to white.
void ConcurrentCopying::ClearBlackPtrs() {
  CHECK(kUseBakerReadBarrier);
  TimingLogger::ScopedTiming split("ClearBlackPtrs", GetTimings());
  ConcurrentCopyingClearBlackPtrsVisitor visitor(this);
  for (auto& space : heap_->GetContinuousSpaces()) {
    if (space == region_space_) {
      continue;
    }
    accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    if (kVerboseMode) {
      LOG(INFO) << "ClearBlackPtrs: " << *space << " bitmap: " << *mark_bitmap;
    }
    mark_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(space->Begin()),
                                  reinterpret_cast<uintptr_t>(space->Limit()),
                                  visitor);
  }
  space::LargeObjectSpace* large_object_space = heap_->GetLargeObjectsSpace();
  large_object_space->GetMarkBitmap()->VisitMarkedRange(
      reinterpret_cast<uintptr_t>(large_object_space->Begin()),
      reinterpret_cast<uintptr_t>(large_object_space->End()),
      visitor);
  // Objects on the allocation stack?
  if (ReadBarrier::kEnableReadBarrierInvariantChecks || kIsDebugBuild) {
    size_t count = GetAllocationStack()->Size();
    auto* it = GetAllocationStack()->Begin();
    auto* end = GetAllocationStack()->End();
    for (size_t i = 0; i < count; ++i, ++it) {
      CHECK_LT(it, end);
      mirror::Object* obj = it->AsMirrorPtr();
      if (obj != nullptr) {
        // Must have been cleared above.
        CHECK_EQ(obj->GetReadBarrierPointer(), ReadBarrier::WhitePtr()) << obj;
      }
    }
  }
}

void ConcurrentCopying::ReclaimPhase() {
  TimingLogger::ScopedTiming split("ReclaimPhase", GetTimings());
  if (kVerboseMode) {
    LOG(INFO) << "GC ReclaimPhase";
  }
  Thread* self = Thread::Current();

  {
    // Double-check that the mark stack is empty.
    // Note: need to set this after VerifyNoFromSpaceRef().
    is_asserting_to_space_invariant_ = false;
    QuasiAtomic::ThreadFenceForConstructor();
    if (kVerboseMode) {
      LOG(INFO) << "Issue an empty check point. ";
    }
    IssueEmptyCheckpoint();
    // Disable the check.
    is_mark_queue_push_disallowed_.StoreSequentiallyConsistent(0);
    CheckEmptyMarkQueue();
  }

  {
    // Record freed objects.
    TimingLogger::ScopedTiming split2("RecordFree", GetTimings());
    // Don't include thread-locals that are in the to-space.
    uint64_t from_bytes = region_space_->GetBytesAllocatedInFromSpace();
    uint64_t from_objects = region_space_->GetObjectsAllocatedInFromSpace();
    uint64_t unevac_from_bytes = region_space_->GetBytesAllocatedInUnevacFromSpace();
    uint64_t unevac_from_objects = region_space_->GetObjectsAllocatedInUnevacFromSpace();
    uint64_t to_bytes = bytes_moved_.LoadSequentiallyConsistent();
    uint64_t to_objects = objects_moved_.LoadSequentiallyConsistent();
    if (kEnableFromSpaceAccountingCheck) {
      CHECK_EQ(from_space_num_objects_at_first_pause_, from_objects + unevac_from_objects);
      CHECK_EQ(from_space_num_bytes_at_first_pause_, from_bytes + unevac_from_bytes);
    }
    CHECK_LE(to_objects, from_objects);
    CHECK_LE(to_bytes, from_bytes);
    int64_t freed_bytes = from_bytes - to_bytes;
    int64_t freed_objects = from_objects - to_objects;
    if (kVerboseMode) {
      LOG(INFO) << "RecordFree:"
                << " from_bytes=" << from_bytes << " from_objects=" << from_objects
                << " unevac_from_bytes=" << unevac_from_bytes << " unevac_from_objects=" << unevac_from_objects
                << " to_bytes=" << to_bytes << " to_objects=" << to_objects
                << " freed_bytes=" << freed_bytes << " freed_objects=" << freed_objects
                << " from_space size=" << region_space_->FromSpaceSize()
                << " unevac_from_space size=" << region_space_->UnevacFromSpaceSize()
                << " to_space size=" << region_space_->ToSpaceSize();
      LOG(INFO) << "(before) num_bytes_allocated=" << heap_->num_bytes_allocated_.LoadSequentiallyConsistent();
    }
    RecordFree(ObjectBytePair(freed_objects, freed_bytes));
    if (kVerboseMode) {
      LOG(INFO) << "(after) num_bytes_allocated=" << heap_->num_bytes_allocated_.LoadSequentiallyConsistent();
    }
  }

  {
    TimingLogger::ScopedTiming split3("ComputeUnevacFromSpaceLiveRatio", GetTimings());
    ComputeUnevacFromSpaceLiveRatio();
  }

  {
    TimingLogger::ScopedTiming split4("ClearFromSpace", GetTimings());
    region_space_->ClearFromSpace();
  }

  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    if (kUseBakerReadBarrier) {
      ClearBlackPtrs();
    }
    Sweep(false);
    SwapBitmaps();
    heap_->UnBindBitmaps();

    // Remove bitmaps for the immune spaces.
    while (!cc_bitmaps_.empty()) {
      accounting::ContinuousSpaceBitmap* cc_bitmap = cc_bitmaps_.back();
      cc_heap_bitmap_->RemoveContinuousSpaceBitmap(cc_bitmap);
      delete cc_bitmap;
      cc_bitmaps_.pop_back();
    }
    region_space_bitmap_ = nullptr;
  }

  if (kVerboseMode) {
    LOG(INFO) << "GC end of ReclaimPhase";
  }
}

class ConcurrentCopyingComputeUnevacFromSpaceLiveRatioVisitor {
 public:
  explicit ConcurrentCopyingComputeUnevacFromSpaceLiveRatioVisitor(ConcurrentCopying* cc)
      : collector_(cc) {}
  void operator()(mirror::Object* ref) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    DCHECK(ref != nullptr);
    DCHECK(collector_->region_space_bitmap_->Test(ref)) << ref;
    DCHECK(collector_->region_space_->IsInUnevacFromSpace(ref)) << ref;
    if (kUseBakerReadBarrier) {
      DCHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::BlackPtr()) << ref;
      // Clear the black ptr.
      ref->AtomicSetReadBarrierPointer(ReadBarrier::BlackPtr(), ReadBarrier::WhitePtr());
      DCHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::WhitePtr()) << ref;
    }
    size_t obj_size = ref->SizeOf();
    size_t alloc_size = RoundUp(obj_size, space::RegionSpace::kAlignment);
    collector_->region_space_->AddLiveBytes(ref, alloc_size);
  }

 private:
  ConcurrentCopying* collector_;
};

// Compute how much live objects are left in regions.
void ConcurrentCopying::ComputeUnevacFromSpaceLiveRatio() {
  region_space_->AssertAllRegionLiveBytesZeroOrCleared();
  ConcurrentCopyingComputeUnevacFromSpaceLiveRatioVisitor visitor(this);
  region_space_bitmap_->VisitMarkedRange(reinterpret_cast<uintptr_t>(region_space_->Begin()),
                                         reinterpret_cast<uintptr_t>(region_space_->Limit()),
                                         visitor);
}

// Assert the to-space invariant.
void ConcurrentCopying::AssertToSpaceInvariant(mirror::Object* obj, MemberOffset offset,
                                               mirror::Object* ref) {
  CHECK(heap_->collector_type_ == kCollectorTypeCC) << static_cast<size_t>(heap_->collector_type_);
  if (is_asserting_to_space_invariant_) {
    if (region_space_->IsInToSpace(ref)) {
      // OK.
      return;
    } else if (region_space_->IsInUnevacFromSpace(ref)) {
      CHECK(region_space_bitmap_->Test(ref)) << ref;
    } else if (region_space_->IsInFromSpace(ref)) {
      // Not OK. Do extra logging.
      if (obj != nullptr) {
        if (kUseBakerReadBarrier) {
          LOG(INFO) << "holder=" << obj << " " << PrettyTypeOf(obj)
                    << " holder rb_ptr=" << obj->GetReadBarrierPointer();
        } else {
          LOG(INFO) << "holder=" << obj << " " << PrettyTypeOf(obj);
        }
        if (region_space_->IsInFromSpace(obj)) {
          LOG(INFO) << "holder is in the from-space.";
        } else if (region_space_->IsInToSpace(obj)) {
          LOG(INFO) << "holder is in the to-space.";
        } else if (region_space_->IsInUnevacFromSpace(obj)) {
          LOG(INFO) << "holder is in the unevac from-space.";
          if (region_space_bitmap_->Test(obj)) {
            LOG(INFO) << "holder is marked in the region space bitmap.";
          } else {
            LOG(INFO) << "holder is not marked in the region space bitmap.";
          }
        } else {
          // In a non-moving space.
          if (immune_region_.ContainsObject(obj)) {
            LOG(INFO) << "holder is in the image or the zygote space.";
            accounting::ContinuousSpaceBitmap* cc_bitmap =
                cc_heap_bitmap_->GetContinuousSpaceBitmap(obj);
            CHECK(cc_bitmap != nullptr)
                << "An immune space object must have a bitmap.";
            if (cc_bitmap->Test(obj)) {
              LOG(INFO) << "holder is marked in the bit map.";
            } else {
              LOG(INFO) << "holder is NOT marked in the bit map.";
            }
          } else {
            LOG(INFO) << "holder is in a non-moving (or main) space.";
            accounting::ContinuousSpaceBitmap* mark_bitmap =
                heap_mark_bitmap_->GetContinuousSpaceBitmap(obj);
            accounting::LargeObjectBitmap* los_bitmap =
                heap_mark_bitmap_->GetLargeObjectBitmap(obj);
            CHECK(los_bitmap != nullptr) << "LOS bitmap covers the entire address range";
            bool is_los = mark_bitmap == nullptr;
            if (!is_los && mark_bitmap->Test(obj)) {
              LOG(INFO) << "holder is marked in the mark bit map.";
            } else if (is_los && los_bitmap->Test(obj)) {
              LOG(INFO) << "holder is marked in the los bit map.";
            } else {
              // If ref is on the allocation stack, then it is considered
              // mark/alive (but not necessarily on the live stack.)
              if (IsOnAllocStack(obj)) {
                LOG(INFO) << "holder is on the alloc stack.";
              } else {
                LOG(INFO) << "holder is not marked or on the alloc stack.";
              }
            }
          }
        }
        LOG(INFO) << "offset=" << offset.SizeValue();
      }
      CHECK(false) << "Found from-space ref " << ref << " " << PrettyTypeOf(ref);
    } else {
      // In a non-moving spaces. Check that the ref is marked.
      if (immune_region_.ContainsObject(ref)) {
        accounting::ContinuousSpaceBitmap* cc_bitmap =
            cc_heap_bitmap_->GetContinuousSpaceBitmap(ref);
        CHECK(cc_bitmap != nullptr)
            << "An immune space ref must have a bitmap. " << ref;
        if (kUseBakerReadBarrier) {
          CHECK(cc_bitmap->Test(ref))
              << "Unmarked immune space ref. obj=" << obj << " rb_ptr="
              << obj->GetReadBarrierPointer() << " ref=" << ref;
        } else {
          CHECK(cc_bitmap->Test(ref))
              << "Unmarked immune space ref. obj=" << obj << " ref=" << ref;
        }
      } else {
        accounting::ContinuousSpaceBitmap* mark_bitmap =
            heap_mark_bitmap_->GetContinuousSpaceBitmap(ref);
        accounting::LargeObjectBitmap* los_bitmap =
            heap_mark_bitmap_->GetLargeObjectBitmap(ref);
        CHECK(los_bitmap != nullptr) << "LOS bitmap covers the entire address range";
        bool is_los = mark_bitmap == nullptr;
        if ((!is_los && mark_bitmap->Test(ref)) ||
            (is_los && los_bitmap->Test(ref))) {
          // OK.
        } else {
          // If ref is on the allocation stack, then it may not be
          // marked live, but considered marked/alive (but not
          // necessarily on the live stack).
          CHECK(IsOnAllocStack(ref)) << "Unmarked ref that's not on the allocation stack. "
                                     << "obj=" << obj << " ref=" << ref;
        }
      }
    }
  }
}

// Used to scan ref fields of an object.
class ConcurrentCopyingRefFieldsVisitor {
 public:
  explicit ConcurrentCopyingRefFieldsVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}

  void operator()(mirror::Object* obj, MemberOffset offset, bool /* is_static */)
      const ALWAYS_INLINE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    collector_->Process(obj, offset);
  }

  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    CHECK(klass->IsTypeOfReferenceClass());
    collector_->DelayReferenceReferent(klass, ref);
  }

 private:
  ConcurrentCopying* const collector_;
};

// Scan ref fields of an object.
void ConcurrentCopying::Scan(mirror::Object* to_ref) {
  DCHECK(!region_space_->IsInFromSpace(to_ref));
  ConcurrentCopyingRefFieldsVisitor visitor(this);
  to_ref->VisitReferences<true>(visitor, visitor);
}

// Process a field.
inline void ConcurrentCopying::Process(mirror::Object* obj, MemberOffset offset) {
  mirror::Object* ref = obj->GetFieldObject<mirror::Object, kVerifyNone, kWithoutReadBarrier, false>(offset);
  if (ref == nullptr || region_space_->IsInToSpace(ref)) {
    return;
  }
  mirror::Object* to_ref = Mark(ref);
  if (to_ref == ref) {
    return;
  }
  // This may fail if the mutator writes to the field at the same time. But it's ok.
  mirror::Object* expected_ref = ref;
  mirror::Object* new_ref = to_ref;
  do {
    if (expected_ref !=
        obj->GetFieldObject<mirror::Object, kVerifyNone, kWithoutReadBarrier, false>(offset)) {
      // It was updated by the mutator.
      break;
    }
  } while (!obj->CasFieldWeakSequentiallyConsistentObjectWithoutWriteBarrier<false, false, kVerifyNone>(
      offset, expected_ref, new_ref));
}

// Process some roots.
void ConcurrentCopying::VisitRoots(
    mirror::Object*** roots, size_t count, const RootInfo& info ATTRIBUTE_UNUSED) {
  for (size_t i = 0; i < count; ++i) {
    mirror::Object** root = roots[i];
    mirror::Object* ref = *root;
    if (ref == nullptr || region_space_->IsInToSpace(ref)) {
      continue;
    }
    mirror::Object* to_ref = Mark(ref);
    if (to_ref == ref) {
      continue;
    }
    Atomic<mirror::Object*>* addr = reinterpret_cast<Atomic<mirror::Object*>*>(root);
    mirror::Object* expected_ref = ref;
    mirror::Object* new_ref = to_ref;
    do {
      if (expected_ref != addr->LoadRelaxed()) {
        // It was updated by the mutator.
        break;
      }
    } while (!addr->CompareExchangeWeakSequentiallyConsistent(expected_ref, new_ref));
  }
}

void ConcurrentCopying::VisitRoots(
    mirror::CompressedReference<mirror::Object>** roots, size_t count,
    const RootInfo& info ATTRIBUTE_UNUSED) {
  for (size_t i = 0; i < count; ++i) {
    mirror::CompressedReference<mirror::Object>* root = roots[i];
    mirror::Object* ref = root->AsMirrorPtr();
    if (ref == nullptr || region_space_->IsInToSpace(ref)) {
      continue;
    }
    mirror::Object* to_ref = Mark(ref);
    if (to_ref == ref) {
      continue;
    }
    auto* addr = reinterpret_cast<Atomic<mirror::CompressedReference<mirror::Object>>*>(root);
    auto expected_ref = mirror::CompressedReference<mirror::Object>::FromMirrorPtr(ref);
    auto new_ref = mirror::CompressedReference<mirror::Object>::FromMirrorPtr(to_ref);
    do {
      if (ref != addr->LoadRelaxed().AsMirrorPtr()) {
        // It was updated by the mutator.
        break;
      }
    } while (!addr->CompareExchangeWeakSequentiallyConsistent(expected_ref, new_ref));
  }
}

// Fill the given memory block with a dummy object. Used to fill in a
// copy of objects that was lost in race.
void ConcurrentCopying::FillWithDummyObject(mirror::Object* dummy_obj, size_t byte_size) {
  CHECK(IsAligned<kObjectAlignment>(byte_size));
  memset(dummy_obj, 0, byte_size);
  mirror::Class* int_array_class = mirror::IntArray::GetArrayClass();
  CHECK(int_array_class != nullptr);
  AssertToSpaceInvariant(nullptr, MemberOffset(0), int_array_class);
  size_t component_size = int_array_class->GetComponentSize();
  CHECK_EQ(component_size, sizeof(int32_t));
  size_t data_offset = mirror::Array::DataOffset(component_size).SizeValue();
  if (data_offset > byte_size) {
    // An int array is too big. Use java.lang.Object.
    mirror::Class* java_lang_Object = WellKnownClasses::ToClass(WellKnownClasses::java_lang_Object);
    AssertToSpaceInvariant(nullptr, MemberOffset(0), java_lang_Object);
    CHECK_EQ(byte_size, java_lang_Object->GetObjectSize());
    dummy_obj->SetClass(java_lang_Object);
    CHECK_EQ(byte_size, dummy_obj->SizeOf());
  } else {
    // Use an int array.
    dummy_obj->SetClass(int_array_class);
    CHECK(dummy_obj->IsArrayInstance());
    int32_t length = (byte_size - data_offset) / component_size;
    dummy_obj->AsArray()->SetLength(length);
    CHECK_EQ(dummy_obj->AsArray()->GetLength(), length)
        << "byte_size=" << byte_size << " length=" << length
        << " component_size=" << component_size << " data_offset=" << data_offset;
    CHECK_EQ(byte_size, dummy_obj->SizeOf())
        << "byte_size=" << byte_size << " length=" << length
        << " component_size=" << component_size << " data_offset=" << data_offset;
  }
}

// Reuse the memory blocks that were copy of objects that were lost in race.
mirror::Object* ConcurrentCopying::AllocateInSkippedBlock(size_t alloc_size) {
  // Try to reuse the blocks that were unused due to CAS failures.
  CHECK(IsAligned<space::RegionSpace::kAlignment>(alloc_size));
  Thread* self = Thread::Current();
  size_t min_object_size = RoundUp(sizeof(mirror::Object), space::RegionSpace::kAlignment);
  MutexLock mu(self, skipped_blocks_lock_);
  auto it = skipped_blocks_map_.lower_bound(alloc_size);
  if (it == skipped_blocks_map_.end()) {
    // Not found.
    return nullptr;
  }
  {
    size_t byte_size = it->first;
    CHECK_GE(byte_size, alloc_size);
    if (byte_size > alloc_size && byte_size - alloc_size < min_object_size) {
      // If remainder would be too small for a dummy object, retry with a larger request size.
      it = skipped_blocks_map_.lower_bound(alloc_size + min_object_size);
      if (it == skipped_blocks_map_.end()) {
        // Not found.
        return nullptr;
      }
      CHECK(IsAligned<space::RegionSpace::kAlignment>(it->first - alloc_size));
      CHECK_GE(it->first - alloc_size, min_object_size)
          << "byte_size=" << byte_size << " it->first=" << it->first << " alloc_size=" << alloc_size;
    }
  }
  // Found a block.
  CHECK(it != skipped_blocks_map_.end());
  size_t byte_size = it->first;
  uint8_t* addr = it->second;
  CHECK_GE(byte_size, alloc_size);
  CHECK(region_space_->IsInToSpace(reinterpret_cast<mirror::Object*>(addr)));
  CHECK(IsAligned<space::RegionSpace::kAlignment>(byte_size));
  if (kVerboseMode) {
    LOG(INFO) << "Reusing skipped bytes : " << reinterpret_cast<void*>(addr) << ", " << byte_size;
  }
  skipped_blocks_map_.erase(it);
  memset(addr, 0, byte_size);
  if (byte_size > alloc_size) {
    // Return the remainder to the map.
    CHECK(IsAligned<space::RegionSpace::kAlignment>(byte_size - alloc_size));
    CHECK_GE(byte_size - alloc_size, min_object_size);
    FillWithDummyObject(reinterpret_cast<mirror::Object*>(addr + alloc_size),
                        byte_size - alloc_size);
    CHECK(region_space_->IsInToSpace(reinterpret_cast<mirror::Object*>(addr + alloc_size)));
    skipped_blocks_map_.insert(std::make_pair(byte_size - alloc_size, addr + alloc_size));
  }
  return reinterpret_cast<mirror::Object*>(addr);
}

mirror::Object* ConcurrentCopying::Copy(mirror::Object* from_ref) {
  DCHECK(region_space_->IsInFromSpace(from_ref));
  // No read barrier to avoid nested RB that might violate the to-space
  // invariant. Note that from_ref is a from space ref so the SizeOf()
  // call will access the from-space meta objects, but it's ok and necessary.
  size_t obj_size = from_ref->SizeOf<kDefaultVerifyFlags, kWithoutReadBarrier>();
  size_t region_space_alloc_size = RoundUp(obj_size, space::RegionSpace::kAlignment);
  size_t region_space_bytes_allocated = 0U;
  size_t non_moving_space_bytes_allocated = 0U;
  size_t bytes_allocated = 0U;
  size_t dummy;
  mirror::Object* to_ref = region_space_->AllocNonvirtual<true>(
      region_space_alloc_size, &region_space_bytes_allocated, nullptr, &dummy);
  bytes_allocated = region_space_bytes_allocated;
  if (to_ref != nullptr) {
    DCHECK_EQ(region_space_alloc_size, region_space_bytes_allocated);
  }
  bool fall_back_to_non_moving = false;
  if (UNLIKELY(to_ref == nullptr)) {
    // Failed to allocate in the region space. Try the skipped blocks.
    to_ref = AllocateInSkippedBlock(region_space_alloc_size);
    if (to_ref != nullptr) {
      // Succeeded to allocate in a skipped block.
      if (heap_->use_tlab_) {
        // This is necessary for the tlab case as it's not accounted in the space.
        region_space_->RecordAlloc(to_ref);
      }
      bytes_allocated = region_space_alloc_size;
    } else {
      // Fall back to the non-moving space.
      fall_back_to_non_moving = true;
      if (kVerboseMode) {
        LOG(INFO) << "Out of memory in the to-space. Fall back to non-moving. skipped_bytes="
                  << to_space_bytes_skipped_.LoadSequentiallyConsistent()
                  << " skipped_objects=" << to_space_objects_skipped_.LoadSequentiallyConsistent();
      }
      fall_back_to_non_moving = true;
      to_ref = heap_->non_moving_space_->Alloc(Thread::Current(), obj_size,
                                               &non_moving_space_bytes_allocated, nullptr, &dummy);
      CHECK(to_ref != nullptr) << "Fall-back non-moving space allocation failed";
      bytes_allocated = non_moving_space_bytes_allocated;
      // Mark it in the mark bitmap.
      accounting::ContinuousSpaceBitmap* mark_bitmap =
          heap_mark_bitmap_->GetContinuousSpaceBitmap(to_ref);
      CHECK(mark_bitmap != nullptr);
      CHECK(!mark_bitmap->AtomicTestAndSet(to_ref));
    }
  }
  DCHECK(to_ref != nullptr);

  // Attempt to install the forward pointer. This is in a loop as the
  // lock word atomic write can fail.
  while (true) {
    // Copy the object. TODO: copy only the lockword in the second iteration and on?
    memcpy(to_ref, from_ref, obj_size);

    LockWord old_lock_word = to_ref->GetLockWord(false);

    if (old_lock_word.GetState() == LockWord::kForwardingAddress) {
      // Lost the race. Another thread (either GC or mutator) stored
      // the forwarding pointer first. Make the lost copy (to_ref)
      // look like a valid but dead (dummy) object and keep it for
      // future reuse.
      FillWithDummyObject(to_ref, bytes_allocated);
      if (!fall_back_to_non_moving) {
        DCHECK(region_space_->IsInToSpace(to_ref));
        if (bytes_allocated > space::RegionSpace::kRegionSize) {
          // Free the large alloc.
          region_space_->FreeLarge(to_ref, bytes_allocated);
        } else {
          // Record the lost copy for later reuse.
          heap_->num_bytes_allocated_.FetchAndAddSequentiallyConsistent(bytes_allocated);
          to_space_bytes_skipped_.FetchAndAddSequentiallyConsistent(bytes_allocated);
          to_space_objects_skipped_.FetchAndAddSequentiallyConsistent(1);
          MutexLock mu(Thread::Current(), skipped_blocks_lock_);
          skipped_blocks_map_.insert(std::make_pair(bytes_allocated,
                                                    reinterpret_cast<uint8_t*>(to_ref)));
        }
      } else {
        DCHECK(heap_->non_moving_space_->HasAddress(to_ref));
        DCHECK_EQ(bytes_allocated, non_moving_space_bytes_allocated);
        // Free the non-moving-space chunk.
        accounting::ContinuousSpaceBitmap* mark_bitmap =
            heap_mark_bitmap_->GetContinuousSpaceBitmap(to_ref);
        CHECK(mark_bitmap != nullptr);
        CHECK(mark_bitmap->Clear(to_ref));
        heap_->non_moving_space_->Free(Thread::Current(), to_ref);
      }

      // Get the winner's forward ptr.
      mirror::Object* lost_fwd_ptr = to_ref;
      to_ref = reinterpret_cast<mirror::Object*>(old_lock_word.ForwardingAddress());
      CHECK(to_ref != nullptr);
      CHECK_NE(to_ref, lost_fwd_ptr);
      CHECK(region_space_->IsInToSpace(to_ref) || heap_->non_moving_space_->HasAddress(to_ref));
      CHECK_NE(to_ref->GetLockWord(false).GetState(), LockWord::kForwardingAddress);
      return to_ref;
    }

    // Set the gray ptr.
    if (kUseBakerReadBarrier) {
      to_ref->SetReadBarrierPointer(ReadBarrier::GrayPtr());
    }

    LockWord new_lock_word = LockWord::FromForwardingAddress(reinterpret_cast<size_t>(to_ref));

    // Try to atomically write the fwd ptr.
    bool success = from_ref->CasLockWordWeakSequentiallyConsistent(old_lock_word, new_lock_word);
    if (LIKELY(success)) {
      // The CAS succeeded.
      objects_moved_.FetchAndAddSequentiallyConsistent(1);
      bytes_moved_.FetchAndAddSequentiallyConsistent(region_space_alloc_size);
      if (LIKELY(!fall_back_to_non_moving)) {
        DCHECK(region_space_->IsInToSpace(to_ref));
      } else {
        DCHECK(heap_->non_moving_space_->HasAddress(to_ref));
        DCHECK_EQ(bytes_allocated, non_moving_space_bytes_allocated);
      }
      if (kUseBakerReadBarrier) {
        DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr());
      }
      DCHECK(GetFwdPtr(from_ref) == to_ref);
      CHECK_NE(to_ref->GetLockWord(false).GetState(), LockWord::kForwardingAddress);
      PushOntoMarkStack<true>(to_ref);
      return to_ref;
    } else {
      // The CAS failed. It may have lost the race or may have failed
      // due to monitor/hashcode ops. Either way, retry.
    }
  }
}

mirror::Object* ConcurrentCopying::IsMarked(mirror::Object* from_ref) {
  DCHECK(from_ref != nullptr);
  space::RegionSpace::RegionType rtype = region_space_->GetRegionType(from_ref);
  if (rtype == space::RegionSpace::RegionType::kRegionTypeToSpace) {
    // It's already marked.
    return from_ref;
  }
  mirror::Object* to_ref;
  if (rtype == space::RegionSpace::RegionType::kRegionTypeFromSpace) {
    to_ref = GetFwdPtr(from_ref);
    DCHECK(to_ref == nullptr || region_space_->IsInToSpace(to_ref) ||
           heap_->non_moving_space_->HasAddress(to_ref))
        << "from_ref=" << from_ref << " to_ref=" << to_ref;
  } else if (rtype == space::RegionSpace::RegionType::kRegionTypeUnevacFromSpace) {
    if (region_space_bitmap_->Test(from_ref)) {
      to_ref = from_ref;
    } else {
      to_ref = nullptr;
    }
  } else {
    // from_ref is in a non-moving space.
    if (immune_region_.ContainsObject(from_ref)) {
      accounting::ContinuousSpaceBitmap* cc_bitmap =
          cc_heap_bitmap_->GetContinuousSpaceBitmap(from_ref);
      DCHECK(cc_bitmap != nullptr)
          << "An immune space object must have a bitmap";
      if (kIsDebugBuild) {
        DCHECK(heap_mark_bitmap_->GetContinuousSpaceBitmap(from_ref)->Test(from_ref))
            << "Immune space object must be already marked";
      }
      if (cc_bitmap->Test(from_ref)) {
        // Already marked.
        to_ref = from_ref;
      } else {
        // Newly marked.
        to_ref = nullptr;
      }
    } else {
      // Non-immune non-moving space. Use the mark bitmap.
      accounting::ContinuousSpaceBitmap* mark_bitmap =
          heap_mark_bitmap_->GetContinuousSpaceBitmap(from_ref);
      accounting::LargeObjectBitmap* los_bitmap =
          heap_mark_bitmap_->GetLargeObjectBitmap(from_ref);
      CHECK(los_bitmap != nullptr) << "LOS bitmap covers the entire address range";
      bool is_los = mark_bitmap == nullptr;
      if (!is_los && mark_bitmap->Test(from_ref)) {
        // Already marked.
        to_ref = from_ref;
      } else if (is_los && los_bitmap->Test(from_ref)) {
        // Already marked in LOS.
        to_ref = from_ref;
      } else {
        // Not marked.
        if (IsOnAllocStack(from_ref)) {
          // If on the allocation stack, it's considered marked.
          to_ref = from_ref;
        } else {
          // Not marked.
          to_ref = nullptr;
        }
      }
    }
  }
  return to_ref;
}

bool ConcurrentCopying::IsOnAllocStack(mirror::Object* ref) {
  QuasiAtomic::ThreadFenceAcquire();
  accounting::ObjectStack* alloc_stack = GetAllocationStack();
  return alloc_stack->Contains(ref);
}

mirror::Object* ConcurrentCopying::Mark(mirror::Object* from_ref) {
  if (from_ref == nullptr) {
    return nullptr;
  }
  DCHECK(from_ref != nullptr);
  DCHECK(heap_->collector_type_ == kCollectorTypeCC);
  if (kUseBakerReadBarrier && !is_active_) {
    // In the lock word forward address state, the read barrier bits
    // in the lock word are part of the stored forwarding address and
    // invalid. This is usually OK as the from-space copy of objects
    // aren't accessed by mutators due to the to-space
    // invariant. However, during the dex2oat image writing relocation
    // and the zygote compaction, objects can be in the forward
    // address state (to store the forward/relocation addresses) and
    // they can still be accessed and the invalid read barrier bits
    // are consulted. If they look like gray but aren't really, the
    // read barriers slow path can trigger when it shouldn't. To guard
    // against this, return here if the CC collector isn't running.
    return from_ref;
  }
  DCHECK(region_space_ != nullptr) << "Read barrier slow path taken when CC isn't running?";
  space::RegionSpace::RegionType rtype = region_space_->GetRegionType(from_ref);
  if (rtype == space::RegionSpace::RegionType::kRegionTypeToSpace) {
    // It's already marked.
    return from_ref;
  }
  mirror::Object* to_ref;
  if (rtype == space::RegionSpace::RegionType::kRegionTypeFromSpace) {
    to_ref = GetFwdPtr(from_ref);
    if (kUseBakerReadBarrier) {
      DCHECK(to_ref != ReadBarrier::GrayPtr()) << "from_ref=" << from_ref << " to_ref=" << to_ref;
    }
    if (to_ref == nullptr) {
      // It isn't marked yet. Mark it by copying it to the to-space.
      to_ref = Copy(from_ref);
    }
    DCHECK(region_space_->IsInToSpace(to_ref) || heap_->non_moving_space_->HasAddress(to_ref))
        << "from_ref=" << from_ref << " to_ref=" << to_ref;
  } else if (rtype == space::RegionSpace::RegionType::kRegionTypeUnevacFromSpace) {
    // This may or may not succeed, which is ok.
    if (kUseBakerReadBarrier) {
      from_ref->AtomicSetReadBarrierPointer(ReadBarrier::WhitePtr(), ReadBarrier::GrayPtr());
    }
    if (region_space_bitmap_->AtomicTestAndSet(from_ref)) {
      // Already marked.
      to_ref = from_ref;
    } else {
      // Newly marked.
      to_ref = from_ref;
      if (kUseBakerReadBarrier) {
        DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr());
      }
      PushOntoMarkStack<true>(to_ref);
    }
  } else {
    // from_ref is in a non-moving space.
    DCHECK(!region_space_->HasAddress(from_ref)) << from_ref;
    if (immune_region_.ContainsObject(from_ref)) {
      accounting::ContinuousSpaceBitmap* cc_bitmap =
          cc_heap_bitmap_->GetContinuousSpaceBitmap(from_ref);
      DCHECK(cc_bitmap != nullptr)
          << "An immune space object must have a bitmap";
      if (kIsDebugBuild) {
        DCHECK(heap_mark_bitmap_->GetContinuousSpaceBitmap(from_ref)->Test(from_ref))
            << "Immune space object must be already marked";
      }
      // This may or may not succeed, which is ok.
      if (kUseBakerReadBarrier) {
        from_ref->AtomicSetReadBarrierPointer(ReadBarrier::WhitePtr(), ReadBarrier::GrayPtr());
      }
      if (cc_bitmap->AtomicTestAndSet(from_ref)) {
        // Already marked.
        to_ref = from_ref;
      } else {
        // Newly marked.
        to_ref = from_ref;
        if (kUseBakerReadBarrier) {
          DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr());
        }
        PushOntoMarkStack<true>(to_ref);
      }
    } else {
      // Use the mark bitmap.
      accounting::ContinuousSpaceBitmap* mark_bitmap =
          heap_mark_bitmap_->GetContinuousSpaceBitmap(from_ref);
      accounting::LargeObjectBitmap* los_bitmap =
          heap_mark_bitmap_->GetLargeObjectBitmap(from_ref);
      CHECK(los_bitmap != nullptr) << "LOS bitmap covers the entire address range";
      bool is_los = mark_bitmap == nullptr;
      if (!is_los && mark_bitmap->Test(from_ref)) {
        // Already marked.
        to_ref = from_ref;
        if (kUseBakerReadBarrier) {
          DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr() ||
                 to_ref->GetReadBarrierPointer() == ReadBarrier::BlackPtr());
        }
      } else if (is_los && los_bitmap->Test(from_ref)) {
        // Already marked in LOS.
        to_ref = from_ref;
        if (kUseBakerReadBarrier) {
          DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr() ||
                 to_ref->GetReadBarrierPointer() == ReadBarrier::BlackPtr());
        }
      } else {
        // Not marked.
        if (IsOnAllocStack(from_ref)) {
          // If it's on the allocation stack, it's considered marked. Keep it white.
          to_ref = from_ref;
          // Objects on the allocation stack need not be marked.
          if (!is_los) {
            DCHECK(!mark_bitmap->Test(to_ref));
          } else {
            DCHECK(!los_bitmap->Test(to_ref));
          }
          if (kUseBakerReadBarrier) {
            DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::WhitePtr());
          }
        } else {
          // Not marked or on the allocation stack. Try to mark it.
          // This may or may not succeed, which is ok.
          if (kUseBakerReadBarrier) {
            from_ref->AtomicSetReadBarrierPointer(ReadBarrier::WhitePtr(), ReadBarrier::GrayPtr());
          }
          if (!is_los && mark_bitmap->AtomicTestAndSet(from_ref)) {
            // Already marked.
            to_ref = from_ref;
          } else if (is_los && los_bitmap->AtomicTestAndSet(from_ref)) {
            // Already marked in LOS.
            to_ref = from_ref;
          } else {
            // Newly marked.
            to_ref = from_ref;
            if (kUseBakerReadBarrier) {
              DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr());
            }
            PushOntoMarkStack<true>(to_ref);
          }
        }
      }
    }
  }
  return to_ref;
}

void ConcurrentCopying::FinishPhase() {
  region_space_ = nullptr;
  CHECK(mark_queue_.IsEmpty());
  mark_queue_.Clear();
  {
    MutexLock mu(Thread::Current(), skipped_blocks_lock_);
    skipped_blocks_map_.clear();
  }
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  heap_->ClearMarkedObjects();
}

mirror::Object* ConcurrentCopying::IsMarkedCallback(mirror::Object* from_ref, void* arg) {
  return reinterpret_cast<ConcurrentCopying*>(arg)->IsMarked(from_ref);
}

bool ConcurrentCopying::IsHeapReferenceMarkedCallback(
    mirror::HeapReference<mirror::Object>* field, void* arg) {
  mirror::Object* from_ref = field->AsMirrorPtr();
  mirror::Object* to_ref = reinterpret_cast<ConcurrentCopying*>(arg)->IsMarked(from_ref);
  if (to_ref == nullptr) {
    return false;
  }
  if (from_ref != to_ref) {
    QuasiAtomic::ThreadFenceRelease();
    field->Assign(to_ref);
    QuasiAtomic::ThreadFenceSequentiallyConsistent();
  }
  return true;
}

mirror::Object* ConcurrentCopying::MarkCallback(mirror::Object* from_ref, void* arg) {
  return reinterpret_cast<ConcurrentCopying*>(arg)->Mark(from_ref);
}

void ConcurrentCopying::ProcessMarkStackCallback(void* arg) {
  reinterpret_cast<ConcurrentCopying*>(arg)->ProcessMarkStack();
}

void ConcurrentCopying::DelayReferenceReferent(mirror::Class* klass, mirror::Reference* reference) {
  heap_->GetReferenceProcessor()->DelayReferenceReferent(
      klass, reference, &IsHeapReferenceMarkedCallback, this);
}

void ConcurrentCopying::ProcessReferences(Thread* self, bool concurrent) {
  TimingLogger::ScopedTiming split("ProcessReferences", GetTimings());
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetHeap()->GetReferenceProcessor()->ProcessReferences(
      concurrent, GetTimings(), GetCurrentIteration()->GetClearSoftReferences(),
      &IsHeapReferenceMarkedCallback, &MarkCallback, &ProcessMarkStackCallback, this);
}

void ConcurrentCopying::RevokeAllThreadLocalBuffers() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  region_space_->RevokeAllThreadLocalBuffers();
}

}  // namespace collector
}  // namespace gc
}  // namespace art
