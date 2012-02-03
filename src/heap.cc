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

#include "heap.h"

#include <limits>
#include <vector>

#include "card_table.h"
#include "debugger.h"
#include "image.h"
#include "mark_sweep.h"
#include "object.h"
#include "object_utils.h"
#include "space.h"
#include "stl_util.h"
#include "thread_list.h"
#include "timing_logger.h"
#include "UniquePtr.h"

namespace art {

std::vector<Space*> Heap::spaces_;

AllocSpace* Heap::alloc_space_ = NULL;

size_t Heap::num_bytes_allocated_ = 0;

size_t Heap::num_objects_allocated_ = 0;

bool Heap::is_gc_running_ = false;

HeapBitmap* Heap::mark_bitmap_ = NULL;

HeapBitmap* Heap::live_bitmap_ = NULL;

CardTable* Heap::card_table_ = NULL;

bool Heap::card_marking_disabled_ = false;

Class* Heap::java_lang_ref_FinalizerReference_ = NULL;
Class* Heap::java_lang_ref_ReferenceQueue_ = NULL;

MemberOffset Heap::reference_referent_offset_ = MemberOffset(0);
MemberOffset Heap::reference_queue_offset_ = MemberOffset(0);
MemberOffset Heap::reference_queueNext_offset_ = MemberOffset(0);
MemberOffset Heap::reference_pendingNext_offset_ = MemberOffset(0);
MemberOffset Heap::finalizer_reference_zombie_offset_ = MemberOffset(0);

float Heap::target_utilization_ = 0.5;

Mutex* Heap::lock_ = NULL;

bool Heap::verify_objects_ = false;

static void UpdateFirstAndLastSpace(Space** first_space, Space** last_space, Space* space) {
  if (*first_space == NULL) {
    *first_space = space;
    *last_space = space;
  } else {
    if ((*first_space)->Begin() > space->Begin()) {
      *first_space = space;
    } else if (space->Begin() > (*last_space)->Begin()) {
      *last_space = space;
    }
  }
}

void Heap::Init(size_t initial_size, size_t growth_limit, size_t capacity,
                const std::vector<std::string>& image_file_names) {
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap::Init entering";
  }

  // Compute the bounds of all spaces for allocating live and mark bitmaps
  // there will be at least one space (the alloc space)
  Space* first_space = NULL;
  Space* last_space = NULL;

  // Requested begin for the alloc space, to follow the mapped image and oat files
  byte* requested_begin = NULL;
  std::vector<Space*> image_spaces;
  for (size_t i = 0; i < image_file_names.size(); i++) {
    ImageSpace* space = Space::CreateImageSpace(image_file_names[i]);
    if (space == NULL) {
      LOG(FATAL) << "Failed to create space from " << image_file_names[i];
    }
    image_spaces.push_back(space);
    AddSpace(space);
    UpdateFirstAndLastSpace(&first_space, &last_space, space);
    // Oat files referenced by image files immediately follow them in memory, ensure alloc space
    // isn't going to get in the middle
    byte* oat_end_addr = space->GetImageHeader().GetOatEnd();
    CHECK(oat_end_addr > space->End());
    if (oat_end_addr > requested_begin) {
      requested_begin = reinterpret_cast<byte*>(RoundUp(reinterpret_cast<uintptr_t>(oat_end_addr),
                                                        kPageSize));
    }
  }

  alloc_space_ = Space::CreateAllocSpace("alloc space", initial_size, growth_limit, capacity,
                                         requested_begin);
  if (alloc_space_ == NULL) {
    LOG(FATAL) << "Failed to create alloc space";
  }
  AddSpace(alloc_space_);
  UpdateFirstAndLastSpace(&first_space, &last_space, alloc_space_);
  byte* heap_begin = first_space->Begin();
  size_t heap_capacity = (last_space->Begin() - first_space->Begin()) + last_space->NonGrowthLimitCapacity();

  // Allocate the initial live bitmap.
  UniquePtr<HeapBitmap> live_bitmap(HeapBitmap::Create("dalvik-bitmap-1", heap_begin, heap_capacity));
  if (live_bitmap.get() == NULL) {
    LOG(FATAL) << "Failed to create live bitmap";
  }

  // Mark image objects in the live bitmap
  for (size_t i = 0; i < spaces_.size(); i++) {
    Space* space = spaces_[i];
    if (space->IsImageSpace()) {
      space->AsImageSpace()->RecordImageAllocations(live_bitmap.get());
    }
  }

  // Allocate the initial mark bitmap.
  UniquePtr<HeapBitmap> mark_bitmap(HeapBitmap::Create("dalvik-bitmap-2", heap_begin, heap_capacity));
  if (mark_bitmap.get() == NULL) {
    LOG(FATAL) << "Failed to create mark bitmap";
  }

  // Allocate the card table.
  UniquePtr<CardTable> card_table(CardTable::Create(heap_begin, heap_capacity));
  if (card_table.get() == NULL) {
    LOG(FATAL) << "Failed to create card table";
  }

  live_bitmap_ = live_bitmap.release();
  mark_bitmap_ = mark_bitmap.release();
  card_table_ = card_table.release();

  num_bytes_allocated_ = 0;
  num_objects_allocated_ = 0;

  // It's still to early to take a lock because there are no threads yet,
  // but we can create the heap lock now. We don't create it earlier to
  // make it clear that you can't use locks during heap initialization.
  lock_ = new Mutex("Heap lock");

  Heap::EnableObjectValidation();

  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap::Init exiting";
  }
}

void Heap::Destroy() {
  // We can't take the heap lock here because there might be a daemon thread suspended with the
  // heap lock held. We know though that no non-daemon threads are executing, and we know that
  // all daemon threads are suspended, and we also know that the threads list have been deleted, so
  // those threads can't resume. We're the only running thread, and we can do whatever we like...
  STLDeleteElements(&spaces_);
  delete mark_bitmap_;
  delete live_bitmap_;
  delete card_table_;
  delete lock_;
}

Object* Heap::AllocObject(Class* klass, size_t byte_count) {
  {
    ScopedHeapLock lock;
    DCHECK(klass == NULL || (klass->IsClassClass() && byte_count >= sizeof(Class)) ||
           (klass->IsVariableSize() || klass->GetObjectSize() == byte_count) ||
           strlen(ClassHelper(klass).GetDescriptor()) == 0);
    DCHECK_GE(byte_count, sizeof(Object));
    Object* obj = AllocateLocked(byte_count);
    if (obj != NULL) {
      obj->SetClass(klass);
      if (Dbg::IsAllocTrackingEnabled()) {
        Dbg::RecordAllocation(klass, byte_count);
      }
      return obj;
    }
  }

  Thread::Current()->ThrowOutOfMemoryError(klass, byte_count);
  return NULL;
}

bool Heap::IsHeapAddress(const Object* obj) {
  // Note: we deliberately don't take the lock here, and mustn't test anything that would
  // require taking the lock.
  if (obj == NULL || !IsAligned<kObjectAlignment>(obj)) {
    return false;
  }
  for (size_t i = 0; i < spaces_.size(); i++) {
    if (spaces_[i]->Contains(obj)) {
      return true;
    }
  }
  return false;
}

bool Heap::IsLiveObjectLocked(const Object* obj) {
  lock_->AssertHeld();
  return IsHeapAddress(obj) && live_bitmap_->Test(obj);
}

#if VERIFY_OBJECT_ENABLED
void Heap::VerifyObject(const Object* obj) {
  if (!verify_objects_) {
    return;
  }
  ScopedHeapLock lock;
  Heap::VerifyObjectLocked(obj);
}
#endif

void Heap::VerifyObjectLocked(const Object* obj) {
  lock_->AssertHeld();
  if (obj != NULL) {
    if (!IsAligned<kObjectAlignment>(obj)) {
      LOG(FATAL) << "Object isn't aligned: " << obj;
    } else if (!live_bitmap_->Test(obj)) {
      LOG(FATAL) << "Object is dead: " << obj;
    }
    // Ignore early dawn of the universe verifications
    if (num_objects_allocated_ > 10) {
      const byte* raw_addr = reinterpret_cast<const byte*>(obj) +
          Object::ClassOffset().Int32Value();
      const Class* c = *reinterpret_cast<Class* const *>(raw_addr);
      if (c == NULL) {
        LOG(FATAL) << "Null class in object: " << obj;
      } else if (!IsAligned<kObjectAlignment>(c)) {
        LOG(FATAL) << "Class isn't aligned: " << c << " in object: " << obj;
      } else if (!live_bitmap_->Test(c)) {
        LOG(FATAL) << "Class of object is dead: " << c << " in object: " << obj;
      }
      // Check obj.getClass().getClass() == obj.getClass().getClass().getClass()
      // Note: we don't use the accessors here as they have internal sanity checks
      // that we don't want to run
      raw_addr = reinterpret_cast<const byte*>(c) + Object::ClassOffset().Int32Value();
      const Class* c_c = *reinterpret_cast<Class* const *>(raw_addr);
      raw_addr = reinterpret_cast<const byte*>(c_c) + Object::ClassOffset().Int32Value();
      const Class* c_c_c = *reinterpret_cast<Class* const *>(raw_addr);
      CHECK_EQ(c_c, c_c_c);
    }
  }
}

void Heap::VerificationCallback(Object* obj, void* arg) {
  DCHECK(obj != NULL);
  Heap::VerifyObjectLocked(obj);
}

void Heap::VerifyHeap() {
  ScopedHeapLock lock;
  live_bitmap_->Walk(Heap::VerificationCallback, NULL);
}

void Heap::RecordAllocationLocked(AllocSpace* space, const Object* obj) {
#ifndef NDEBUG
  if (Runtime::Current()->IsStarted()) {
    lock_->AssertHeld();
  }
#endif
  size_t size = space->AllocationSize(obj);
  DCHECK_GT(size, 0u);
  num_bytes_allocated_ += size;
  num_objects_allocated_ += 1;

  if (Runtime::Current()->HasStatsEnabled()) {
    RuntimeStats* global_stats = Runtime::Current()->GetStats();
    RuntimeStats* thread_stats = Thread::Current()->GetStats();
    ++global_stats->allocated_objects;
    ++thread_stats->allocated_objects;
    global_stats->allocated_bytes += size;
    thread_stats->allocated_bytes += size;
  }

  live_bitmap_->Set(obj);
}

void Heap::RecordFreeLocked(size_t freed_objects, size_t freed_bytes) {
  lock_->AssertHeld();

  if (freed_objects < num_objects_allocated_) {
    num_objects_allocated_ -= freed_objects;
  } else {
    num_objects_allocated_ = 0;
  }
  if (freed_bytes < num_bytes_allocated_) {
    num_bytes_allocated_ -= freed_bytes;
  } else {
    num_bytes_allocated_ = 0;
  }

  if (Runtime::Current()->HasStatsEnabled()) {
    RuntimeStats* global_stats = Runtime::Current()->GetStats();
    RuntimeStats* thread_stats = Thread::Current()->GetStats();
    ++global_stats->freed_objects;
    ++thread_stats->freed_objects;
    global_stats->freed_bytes += freed_bytes;
    thread_stats->freed_bytes += freed_bytes;
  }
}

Object* Heap::AllocateLocked(size_t size) {
  lock_->AssertHeld();
  DCHECK(alloc_space_ != NULL);
  AllocSpace* space = alloc_space_;
  Object* obj = AllocateLocked(space, size);
  if (obj != NULL) {
    RecordAllocationLocked(space, obj);
  }
  return obj;
}

Object* Heap::AllocateLocked(AllocSpace* space, size_t alloc_size) {
  lock_->AssertHeld();

  // Since allocation can cause a GC which will need to SuspendAll,
  // make sure all allocators are in the kRunnable state.
  DCHECK_EQ(Thread::Current()->GetState(), Thread::kRunnable);

  // Fail impossible allocations
  if (alloc_size > space->Capacity()) {
    // On failure collect soft references
    CollectGarbageInternal(true);
    return NULL;
  }

  Object* ptr = space->AllocWithoutGrowth(alloc_size);
  if (ptr != NULL) {
    return ptr;
  }

  // The allocation failed.  If the GC is running, block until it completes and retry.
  if (is_gc_running_) {
    // The GC is concurrently tracing the heap.  Release the heap lock, wait for the GC to
    // complete, and retrying allocating.
    WaitForConcurrentGcToComplete();
    ptr = space->AllocWithoutGrowth(alloc_size);
    if (ptr != NULL) {
      return ptr;
    }
  }

  // Another failure.  Our thread was starved or there may be too many
  // live objects.  Try a foreground GC.  This will have no effect if
  // the concurrent GC is already running.
  if (Runtime::Current()->HasStatsEnabled()) {
    ++Runtime::Current()->GetStats()->gc_for_alloc_count;
    ++Thread::Current()->GetStats()->gc_for_alloc_count;
  }
  CollectGarbageInternal(false);
  ptr = space->AllocWithoutGrowth(alloc_size);
  if (ptr != NULL) {
    return ptr;
  }

  // Even that didn't work;  this is an exceptional state.
  // Try harder, growing the heap if necessary.
  ptr = space->AllocWithGrowth(alloc_size);
  if (ptr != NULL) {
    //size_t new_footprint = dvmHeapSourceGetIdealFootprint();
    size_t new_footprint = space->GetFootprintLimit();
    // OLD-TODO: may want to grow a little bit more so that the amount of
    //       free space is equal to the old free space + the
    //       utilization slop for the new allocation.
    VLOG(gc) << "Grow heap (frag case) to " << PrettySize(new_footprint)
             << " for a " << PrettySize(alloc_size) << " allocation";
    return ptr;
  }

  // Most allocations should have succeeded by now, so the heap is
  // really full, really fragmented, or the requested size is really
  // big.  Do another GC, collecting SoftReferences this time.  The VM
  // spec requires that all SoftReferences have been collected and
  // cleared before throwing an OOME.

  // OLD-TODO: wait for the finalizers from the previous GC to finish
  VLOG(gc) << "Forcing collection of SoftReferences for " << PrettySize(alloc_size) << " allocation";
  CollectGarbageInternal(true);
  ptr = space->AllocWithGrowth(alloc_size);
  if (ptr != NULL) {
    return ptr;
  }

  LOG(ERROR) << "Out of memory on a " << PrettySize(alloc_size) << " allocation";

  // TODO: tell the HeapSource to dump its state
  // TODO: dump stack traces for all threads

  return NULL;
}

int64_t Heap::GetMaxMemory() {
  return alloc_space_->Capacity();
}

int64_t Heap::GetTotalMemory() {
  return alloc_space_->Capacity();
}

int64_t Heap::GetFreeMemory() {
  return alloc_space_->Capacity() - num_bytes_allocated_;
}

class InstanceCounter {
 public:
  InstanceCounter(Class* c, bool count_assignable)
      : class_(c), count_assignable_(count_assignable), count_(0) {
  }

  size_t GetCount() {
    return count_;
  }

  static void Callback(Object* o, void* arg) {
    reinterpret_cast<InstanceCounter*>(arg)->VisitInstance(o);
  }

 private:
  void VisitInstance(Object* o) {
    Class* instance_class = o->GetClass();
    if (count_assignable_) {
      if (instance_class == class_) {
        ++count_;
      }
    } else {
      if (instance_class != NULL && class_->IsAssignableFrom(instance_class)) {
        ++count_;
      }
    }
  }

  Class* class_;
  bool count_assignable_;
  size_t count_;
};

int64_t Heap::CountInstances(Class* c, bool count_assignable) {
  ScopedHeapLock lock;
  InstanceCounter counter(c, count_assignable);
  live_bitmap_->Walk(InstanceCounter::Callback, &counter);
  return counter.GetCount();
}

void Heap::CollectGarbage(bool clear_soft_references) {
  ScopedHeapLock lock;
  CollectGarbageInternal(clear_soft_references);
}

void Heap::CollectGarbageInternal(bool clear_soft_references) {
  lock_->AssertHeld();

  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  thread_list->SuspendAll();

  size_t initial_size = num_bytes_allocated_;
  TimingLogger timings("CollectGarbageInternal");
  uint64_t t0 = NanoTime();
  Object* cleared_references = NULL;
  {
    MarkSweep mark_sweep;
    timings.AddSplit("ctor");

    mark_sweep.Init();
    timings.AddSplit("Init");

    mark_sweep.MarkRoots();
    timings.AddSplit("MarkRoots");

    mark_sweep.ScanDirtyImageRoots();
    timings.AddSplit("DirtyImageRoots");

    // Roots are marked on the bitmap and the mark_stack is empty
    DCHECK(mark_sweep.IsMarkStackEmpty());

    // TODO: if concurrent
    //   unlock heap
    //   thread_list->ResumeAll();

    // Recursively mark all bits set in the non-image mark bitmap
    mark_sweep.RecursiveMark();
    timings.AddSplit("RecursiveMark");

    // TODO: if concurrent
    //   lock heap
    //   thread_list->SuspendAll();
    //   re-mark root set
    //   scan dirty objects

    mark_sweep.ProcessReferences(clear_soft_references);
    timings.AddSplit("ProcessReferences");

    // TODO: if concurrent
    //    swap bitmaps

    mark_sweep.Sweep();
    timings.AddSplit("Sweep");

    cleared_references = mark_sweep.GetClearedReferences();
  }

  GrowForUtilization();
  timings.AddSplit("GrowForUtilization");
  uint64_t t1 = NanoTime();
  thread_list->ResumeAll();

  EnqueueClearedReferences(&cleared_references);
  RequestHeapTrim();

  uint64_t duration_ns = t1 - t0;
  bool gc_was_particularly_slow = duration_ns > MsToNs(50); // TODO: crank this down for concurrent.
  if (VLOG_IS_ON(gc) || gc_was_particularly_slow) {
    // TODO: somehow make the specific GC implementation (here MarkSweep) responsible for logging.
    size_t bytes_freed = initial_size - num_bytes_allocated_;
    if (bytes_freed > KB) {  // ignore freed bytes in output if > 1KB
      bytes_freed = RoundDown(bytes_freed, KB);
    }
    size_t bytes_allocated = RoundUp(num_bytes_allocated_, KB);
    // lose low nanoseconds in duration. TODO: make this part of PrettyDuration
    duration_ns = (duration_ns / 1000) * 1000;
    size_t total = GetTotalMemory();
    size_t percentFree = 100 - static_cast<size_t>(100.0f * static_cast<float>(num_bytes_allocated_) / total);
    LOG(INFO) << "GC freed " << PrettySize(bytes_freed) << ", " << percentFree << "% free, "
              << PrettySize(bytes_allocated) << "/" << PrettySize(total) << ", "
              << "paused " << PrettyDuration(duration_ns);
  }
  Dbg::GcDidFinish();
  if (VLOG_IS_ON(heap)) {
    timings.Dump();
  }
}

void Heap::WaitForConcurrentGcToComplete() {
  lock_->AssertHeld();
}

void Heap::SetIdealFootprint(size_t max_allowed_footprint) {
  size_t alloc_space_capacity = alloc_space_->Capacity();
  if (max_allowed_footprint > alloc_space_capacity) {
    VLOG(gc) << "Clamp target GC heap from " << PrettySize(max_allowed_footprint)
             << " to " << PrettySize(alloc_space_capacity);
    max_allowed_footprint = alloc_space_capacity;
  }
  alloc_space_->SetFootprintLimit(max_allowed_footprint);
}

// kHeapIdealFree is the ideal maximum free size, when we grow the heap for utilization.
static const size_t kHeapIdealFree = 2 * MB;
// kHeapMinFree guarantees that you always have at least 512 KB free, when you grow for utilization,
// regardless of target utilization ratio.
static const size_t kHeapMinFree = kHeapIdealFree / 4;

void Heap::GrowForUtilization() {
  lock_->AssertHeld();

  // We know what our utilization is at this moment.
  // This doesn't actually resize any memory. It just lets the heap grow more
  // when necessary.
  size_t target_size(num_bytes_allocated_ / Heap::GetTargetHeapUtilization());

  if (target_size > num_bytes_allocated_ + kHeapIdealFree) {
    target_size = num_bytes_allocated_ + kHeapIdealFree;
  } else if (target_size < num_bytes_allocated_ + kHeapMinFree) {
    target_size = num_bytes_allocated_ + kHeapMinFree;
  }

  SetIdealFootprint(target_size);
}

void Heap::ClearGrowthLimit() {
  ScopedHeapLock lock;
  WaitForConcurrentGcToComplete();
  alloc_space_->ClearGrowthLimit();
}

pid_t Heap::GetLockOwner() {
  return lock_->GetOwner();
}

void Heap::Lock() {
  // Grab the lock, but put ourselves into Thread::kVmWait if it looks
  // like we're going to have to wait on the mutex. This prevents
  // deadlock if another thread is calling CollectGarbageInternal,
  // since they will have the heap lock and be waiting for mutators to
  // suspend.
  if (!lock_->TryLock()) {
    ScopedThreadStateChange tsc(Thread::Current(), Thread::kVmWait);
    lock_->Lock();
  }
}

void Heap::Unlock() {
  lock_->Unlock();
}

void Heap::SetWellKnownClasses(Class* java_lang_ref_FinalizerReference,
    Class* java_lang_ref_ReferenceQueue) {
  java_lang_ref_FinalizerReference_ = java_lang_ref_FinalizerReference;
  java_lang_ref_ReferenceQueue_ = java_lang_ref_ReferenceQueue;
  CHECK(java_lang_ref_FinalizerReference_ != NULL);
  CHECK(java_lang_ref_ReferenceQueue_ != NULL);
}

void Heap::SetReferenceOffsets(MemberOffset reference_referent_offset,
    MemberOffset reference_queue_offset,
    MemberOffset reference_queueNext_offset,
    MemberOffset reference_pendingNext_offset,
    MemberOffset finalizer_reference_zombie_offset) {
  reference_referent_offset_ = reference_referent_offset;
  reference_queue_offset_ = reference_queue_offset;
  reference_queueNext_offset_ = reference_queueNext_offset;
  reference_pendingNext_offset_ = reference_pendingNext_offset;
  finalizer_reference_zombie_offset_ = finalizer_reference_zombie_offset;
  CHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  CHECK_NE(reference_queue_offset_.Uint32Value(), 0U);
  CHECK_NE(reference_queueNext_offset_.Uint32Value(), 0U);
  CHECK_NE(reference_pendingNext_offset_.Uint32Value(), 0U);
  CHECK_NE(finalizer_reference_zombie_offset_.Uint32Value(), 0U);
}

Object* Heap::GetReferenceReferent(Object* reference) {
  DCHECK(reference != NULL);
  DCHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  return reference->GetFieldObject<Object*>(reference_referent_offset_, true);
}

void Heap::ClearReferenceReferent(Object* reference) {
  DCHECK(reference != NULL);
  DCHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  reference->SetFieldObject(reference_referent_offset_, NULL, true);
}

// Returns true if the reference object has not yet been enqueued.
bool Heap::IsEnqueuable(const Object* ref) {
  DCHECK(ref != NULL);
  const Object* queue = ref->GetFieldObject<Object*>(reference_queue_offset_, false);
  const Object* queue_next = ref->GetFieldObject<Object*>(reference_queueNext_offset_, false);
  return (queue != NULL) && (queue_next == NULL);
}

void Heap::EnqueueReference(Object* ref, Object** cleared_reference_list) {
  DCHECK(ref != NULL);
  CHECK(ref->GetFieldObject<Object*>(reference_queue_offset_, false) != NULL);
  CHECK(ref->GetFieldObject<Object*>(reference_queueNext_offset_, false) == NULL);
  EnqueuePendingReference(ref, cleared_reference_list);
}

void Heap::EnqueuePendingReference(Object* ref, Object** list) {
  DCHECK(ref != NULL);
  DCHECK(list != NULL);

  if (*list == NULL) {
    ref->SetFieldObject(reference_pendingNext_offset_, ref, false);
    *list = ref;
  } else {
    Object* head = (*list)->GetFieldObject<Object*>(reference_pendingNext_offset_, false);
    ref->SetFieldObject(reference_pendingNext_offset_, head, false);
    (*list)->SetFieldObject(reference_pendingNext_offset_, ref, false);
  }
}

Object* Heap::DequeuePendingReference(Object** list) {
  DCHECK(list != NULL);
  DCHECK(*list != NULL);
  Object* head = (*list)->GetFieldObject<Object*>(reference_pendingNext_offset_, false);
  Object* ref;
  if (*list == head) {
    ref = *list;
    *list = NULL;
  } else {
    Object* next = head->GetFieldObject<Object*>(reference_pendingNext_offset_, false);
    (*list)->SetFieldObject(reference_pendingNext_offset_, next, false);
    ref = head;
  }
  ref->SetFieldObject(reference_pendingNext_offset_, NULL, false);
  return ref;
}

void Heap::AddFinalizerReference(Thread* self, Object* object) {
  ScopedThreadStateChange tsc(self, Thread::kRunnable);
  static Method* FinalizerReference_add =
      java_lang_ref_FinalizerReference_->FindDirectMethod("add", "(Ljava/lang/Object;)V");
  DCHECK(FinalizerReference_add != NULL);
  Object* args[] = { object };
  FinalizerReference_add->Invoke(self, NULL, reinterpret_cast<byte*>(&args), NULL);
}

void Heap::EnqueueClearedReferences(Object** cleared) {
  DCHECK(cleared != NULL);
  if (*cleared != NULL) {
    static Method* ReferenceQueue_add =
        java_lang_ref_ReferenceQueue_->FindDirectMethod("add", "(Ljava/lang/ref/Reference;)V");
    DCHECK(ReferenceQueue_add != NULL);

    Thread* self = Thread::Current();
    ScopedThreadStateChange tsc(self, Thread::kRunnable);
    Object* args[] = { *cleared };
    ReferenceQueue_add->Invoke(self, NULL, reinterpret_cast<byte*>(&args), NULL);
    *cleared = NULL;
  }
}

void Heap::RequestHeapTrim() {
  // We don't have a good measure of how worthwhile a trim might be. We can't use the live bitmap
  // because that only marks object heads, so a large array looks like lots of empty space. We
  // don't just call dlmalloc all the time, because the cost of an _attempted_ trim is proportional
  // to utilization (which is probably inversely proportional to how much benefit we can expect).
  // We could try mincore(2) but that's only a measure of how many pages we haven't given away,
  // not how much use we're making of those pages.
  float utilization = static_cast<float>(num_bytes_allocated_) / alloc_space_->Size();
  if (utilization > 0.75f) {
    // Don't bother trimming the heap if it's more than 75% utilized.
    // (This percentage was picked arbitrarily.)
    return;
  }
  JNIEnv* env = Thread::Current()->GetJniEnv();
  static jclass Daemons_class = CacheClass(env, "java/lang/Daemons");
  static jmethodID Daemons_requestHeapTrim = env->GetStaticMethodID(Daemons_class, "requestHeapTrim", "()V");
  env->CallStaticVoidMethod(Daemons_class, Daemons_requestHeapTrim);
  CHECK(!env->ExceptionCheck());
}

}  // namespace art
