/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "allocation_record.h"

#include "art_method-inl.h"
#include "base/stl_util.h"
#include "stack.h"

#ifdef __ANDROID__
#include "cutils/properties.h"
#endif

namespace art {
namespace gc {

int32_t AllocRecordStackTraceElement::ComputeLineNumber() const {
  DCHECK(method_ != nullptr);
  return method_->GetLineNumFromDexPC(dex_pc_);
}

const char* AllocRecord::GetClassDescriptor(std::string* storage) const {
  // klass_ could contain null only if we implement class unloading.
  return klass_.IsNull() ? "null" : klass_.Read()->GetDescriptor(storage);
}

void AllocRecordObjectMap::SetProperties() {
#ifdef __ANDROID__
  // Check whether there's a system property overriding the max number of records.
  const char* propertyName = "dalvik.vm.allocTrackerMax";
  char allocMaxString[PROPERTY_VALUE_MAX];
  if (property_get(propertyName, allocMaxString, "") > 0) {
    char* end;
    size_t value = strtoul(allocMaxString, &end, 10);
    if (*end != '\0') {
      LOG(ERROR) << "Ignoring  " << propertyName << " '" << allocMaxString
                 << "' --- invalid";
    } else {
      alloc_record_max_ = value;
      if (recent_record_max_ > value) {
        recent_record_max_ = value;
      }
    }
  }
  // Check whether there's a system property overriding the number of recent records.
  propertyName = "dalvik.vm.recentAllocMax";
  char recentAllocMaxString[PROPERTY_VALUE_MAX];
  if (property_get(propertyName, recentAllocMaxString, "") > 0) {
    char* end;
    size_t value = strtoul(recentAllocMaxString, &end, 10);
    if (*end != '\0') {
      LOG(ERROR) << "Ignoring  " << propertyName << " '" << recentAllocMaxString
                 << "' --- invalid";
    } else if (value > alloc_record_max_) {
      LOG(ERROR) << "Ignoring  " << propertyName << " '" << recentAllocMaxString
                 << "' --- should be less than " << alloc_record_max_;
    } else {
      recent_record_max_ = value;
    }
  }
  // Check whether there's a system property overriding the max depth of stack trace.
  propertyName = "debug.allocTracker.stackDepth";
  char stackDepthString[PROPERTY_VALUE_MAX];
  if (property_get(propertyName, stackDepthString, "") > 0) {
    char* end;
    size_t value = strtoul(stackDepthString, &end, 10);
    if (*end != '\0') {
      LOG(ERROR) << "Ignoring  " << propertyName << " '" << stackDepthString
                 << "' --- invalid";
    } else if (value > kMaxSupportedStackDepth) {
      LOG(WARNING) << propertyName << " '" << stackDepthString << "' too large, using "
                   << kMaxSupportedStackDepth;
      max_stack_depth_ = kMaxSupportedStackDepth;
    } else {
      max_stack_depth_ = value;
    }
  }
#endif
}

AllocRecordObjectMap::~AllocRecordObjectMap() {
  Clear();
}

void AllocRecordObjectMap::VisitRoots(RootVisitor* visitor) {
  CHECK_LE(recent_record_max_, alloc_record_max_);
  BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(visitor, RootInfo(kRootDebugger));
  size_t count = recent_record_max_;
  // Only visit the last recent_record_max_ number of allocation records in entries_ and mark the
  // klass_ fields as strong roots.
  for (auto it = entries_.rbegin(), end = entries_.rend(); it != end; ++it) {
    AllocRecord* record = it->second;
    if (count > 0) {
      buffered_visitor.VisitRootIfNonNull(record->GetClassGcRoot());
      --count;
    }
    // Visit all of the stack frames to make sure no methods in the stack traces get unloaded by
    // class unloading.
    for (size_t i = 0, depth = record->GetDepth(); i < depth; ++i) {
      const AllocRecordStackTraceElement& element = record->StackElement(i);
      DCHECK(element.GetMethod() != nullptr);
      element.GetMethod()->VisitRoots(buffered_visitor, sizeof(void*));
    }
  }
}

static inline void SweepClassObject(AllocRecord* record, IsMarkedVisitor* visitor)
    SHARED_REQUIRES(Locks::mutator_lock_)
    REQUIRES(Locks::alloc_tracker_lock_) {
  GcRoot<mirror::Class>& klass = record->GetClassGcRoot();
  // This does not need a read barrier because this is called by GC.
  mirror::Object* old_object = klass.Read<kWithoutReadBarrier>();
  if (old_object != nullptr) {
    // The class object can become null if we implement class unloading.
    // In that case we might still want to keep the class name string (not implemented).
    mirror::Object* new_object = visitor->IsMarked(old_object);
    DCHECK(new_object != nullptr);
    if (UNLIKELY(old_object != new_object)) {
      klass = GcRoot<mirror::Class>(new_object->AsClass());
    }
  }
}

void AllocRecordObjectMap::SweepAllocationRecords(IsMarkedVisitor* visitor) {
  VLOG(heap) << "Start SweepAllocationRecords()";
  size_t count_deleted = 0, count_moved = 0, count = 0;
  // Only the first (size - recent_record_max_) number of records can be deleted.
  const size_t delete_bound = std::max(entries_.size(), recent_record_max_) - recent_record_max_;
  for (auto it = entries_.begin(), end = entries_.end(); it != end;) {
    ++count;
    // This does not need a read barrier because this is called by GC.
    mirror::Object* old_object = it->first.Read<kWithoutReadBarrier>();
    AllocRecord* record = it->second;
    mirror::Object* new_object = old_object == nullptr ? nullptr : visitor->IsMarked(old_object);
    if (new_object == nullptr) {
      if (count > delete_bound) {
        it->first = GcRoot<mirror::Object>(nullptr);
        SweepClassObject(record, visitor);
        ++it;
      } else {
        delete record;
        it = entries_.erase(it);
        ++count_deleted;
      }
    } else {
      if (old_object != new_object) {
        it->first = GcRoot<mirror::Object>(new_object);
        ++count_moved;
      }
      SweepClassObject(record, visitor);
      ++it;
    }
  }
  VLOG(heap) << "Deleted " << count_deleted << " allocation records";
  VLOG(heap) << "Updated " << count_moved << " allocation records";
}

void AllocRecordObjectMap::AllowNewAllocationRecords() {
  CHECK(!kUseReadBarrier);
  allow_new_record_ = true;
  new_record_condition_.Broadcast(Thread::Current());
}

void AllocRecordObjectMap::DisallowNewAllocationRecords() {
  CHECK(!kUseReadBarrier);
  allow_new_record_ = false;
}

void AllocRecordObjectMap::BroadcastForNewAllocationRecords() {
  CHECK(kUseReadBarrier);
  new_record_condition_.Broadcast(Thread::Current());
}

struct AllocRecordStackVisitor : public StackVisitor {
  AllocRecordStackVisitor(Thread* thread, AllocRecordStackTrace* trace_in, size_t max)
      SHARED_REQUIRES(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        trace(trace_in),
        max_depth(max) {}

  // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
  // annotalysis.
  bool VisitFrame() OVERRIDE NO_THREAD_SAFETY_ANALYSIS {
    if (depth >= max_depth) {
      return false;
    }
    ArtMethod* m = GetMethod();
    if (!m->IsRuntimeMethod()) {
      trace->SetStackElementAt(depth, m, GetDexPc());
      ++depth;
    }
    return true;
  }

  ~AllocRecordStackVisitor() {
    trace->SetDepth(depth);
  }

  AllocRecordStackTrace* trace;
  size_t depth = 0u;
  const size_t max_depth;
};

void AllocRecordObjectMap::SetAllocTrackingEnabled(bool enable) {
  Thread* self = Thread::Current();
  Heap* heap = Runtime::Current()->GetHeap();
  if (enable) {
    {
      MutexLock mu(self, *Locks::alloc_tracker_lock_);
      if (heap->IsAllocTrackingEnabled()) {
        return;  // Already enabled, bail.
      }
      AllocRecordObjectMap* records = heap->GetAllocationRecords();
      if (records == nullptr) {
        records = new AllocRecordObjectMap;
        heap->SetAllocationRecords(records);
      }
      CHECK(records != nullptr);
      records->SetProperties();
      std::string self_name;
      self->GetThreadName(self_name);
      if (self_name == "JDWP") {
        records->alloc_ddm_thread_id_ = self->GetTid();
      }
      records->scratch_trace_.SetDepth(records->max_stack_depth_);
      size_t sz = sizeof(AllocRecordStackTraceElement) * records->max_stack_depth_ +
                  sizeof(AllocRecord) + sizeof(AllocRecordStackTrace);
      LOG(INFO) << "Enabling alloc tracker (" << records->alloc_record_max_ << " entries of "
                << records->max_stack_depth_ << " frames, taking up to "
                << PrettySize(sz * records->alloc_record_max_) << ")";
    }
    Runtime::Current()->GetInstrumentation()->InstrumentQuickAllocEntryPoints();
    {
      MutexLock mu(self, *Locks::alloc_tracker_lock_);
      heap->SetAllocTrackingEnabled(true);
    }
  } else {
    // Delete outside of the critical section to avoid possible lock violations like the runtime
    // shutdown lock.
    {
      MutexLock mu(self, *Locks::alloc_tracker_lock_);
      if (!heap->IsAllocTrackingEnabled()) {
        return;  // Already disabled, bail.
      }
      heap->SetAllocTrackingEnabled(false);
      LOG(INFO) << "Disabling alloc tracker";
      AllocRecordObjectMap* records = heap->GetAllocationRecords();
      records->Clear();
    }
    // If an allocation comes in before we uninstrument, we will safely drop it on the floor.
    Runtime::Current()->GetInstrumentation()->UninstrumentQuickAllocEntryPoints();
  }
}

void AllocRecordObjectMap::RecordAllocation(Thread* self, mirror::Object* obj, mirror::Class* klass,
                                            size_t byte_count) {
  MutexLock mu(self, *Locks::alloc_tracker_lock_);
  Heap* heap = Runtime::Current()->GetHeap();
  if (!heap->IsAllocTrackingEnabled()) {
    // In the process of shutting down recording, bail.
    return;
  }

  AllocRecordObjectMap* records = heap->GetAllocationRecords();
  DCHECK(records != nullptr);

  // Do not record for DDM thread
  if (records->alloc_ddm_thread_id_ == self->GetTid()) {
    return;
  }

  // Wait for GC's sweeping to complete and allow new records
  while (UNLIKELY((!kUseReadBarrier && !records->allow_new_record_) ||
                  (kUseReadBarrier && !self->GetWeakRefAccessEnabled()))) {
    records->new_record_condition_.WaitHoldingLocks(self);
  }

  DCHECK_LE(records->Size(), records->alloc_record_max_);

  // Get stack trace.
  // add scope to make "visitor" destroyed promptly, in order to set the scratch_trace_->depth_
  {
    AllocRecordStackVisitor visitor(self, &records->scratch_trace_, records->max_stack_depth_);
    visitor.WalkStack();
  }
  records->scratch_trace_.SetTid(self->GetTid());
  AllocRecordStackTrace* trace = new AllocRecordStackTrace(records->scratch_trace_);

  // Fill in the basics.
  AllocRecord* record = new AllocRecord(byte_count, klass, trace);

  records->Put(obj, record);
  DCHECK_LE(records->Size(), records->alloc_record_max_);
}

void AllocRecordObjectMap::Clear() {
  STLDeleteValues(&entries_);
  entries_.clear();
}

}  // namespace gc
}  // namespace art
