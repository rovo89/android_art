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

#ifdef HAVE_ANDROID_OS
#include "cutils/properties.h"
#endif

namespace art {
namespace gc {

int32_t AllocRecordStackTraceElement::ComputeLineNumber() const {
  DCHECK(method_ != nullptr);
  return method_->GetLineNumFromDexPC(dex_pc_);
}

void AllocRecordObjectMap::SetProperties() {
#ifdef HAVE_ANDROID_OS
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
    }
  }
  // Check whether there's a system property overriding the max depth of stack trace.
  propertyName = "dalvik.vm.allocStackDepth";
  char stackDepthString[PROPERTY_VALUE_MAX];
  if (property_get(propertyName, stackDepthString, "") > 0) {
    char* end;
    size_t value = strtoul(stackDepthString, &end, 10);
    if (*end != '\0') {
      LOG(ERROR) << "Ignoring  " << propertyName << " '" << stackDepthString
                 << "' --- invalid";
    } else {
      max_stack_depth_ = value;
    }
  }
#endif
}

AllocRecordObjectMap::~AllocRecordObjectMap() {
  STLDeleteValues(&entries_);
}

void AllocRecordObjectMap::SweepAllocationRecords(IsMarkedCallback* callback, void* arg) {
  VLOG(heap) << "Start SweepAllocationRecords()";
  size_t count_deleted = 0, count_moved = 0;
  for (auto it = entries_.begin(), end = entries_.end(); it != end;) {
    // This does not need a read barrier because this is called by GC.
    mirror::Object* old_object = it->first.Read<kWithoutReadBarrier>();
    AllocRecord* record = it->second;
    mirror::Object* new_object = callback(old_object, arg);
    if (new_object == nullptr) {
      delete record;
      it = entries_.erase(it);
      ++count_deleted;
    } else {
      if (old_object != new_object) {
        it->first = GcRoot<mirror::Object>(new_object);
        ++count_moved;
      }
      ++it;
    }
  }
  VLOG(heap) << "Deleted " << count_deleted << " allocation records";
  VLOG(heap) << "Updated " << count_moved << " allocation records";
}

struct AllocRecordStackVisitor : public StackVisitor {
  AllocRecordStackVisitor(Thread* thread, AllocRecordStackTrace* trace_in, size_t max)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        trace(trace_in),
        depth(0),
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
  size_t depth;
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
      AllocRecordObjectMap* records = new AllocRecordObjectMap();
      CHECK(records != nullptr);
      records->SetProperties();
      std::string self_name;
      self->GetThreadName(self_name);
      if (self_name == "JDWP") {
        records->alloc_ddm_thread_id_ = self->GetTid();
      }
      size_t sz = sizeof(AllocRecordStackTraceElement) * records->max_stack_depth_ +
                  sizeof(AllocRecord) + sizeof(AllocRecordStackTrace);
      LOG(INFO) << "Enabling alloc tracker (" << records->alloc_record_max_ << " entries of "
                << records->max_stack_depth_ << " frames, taking up to "
                << PrettySize(sz * records->alloc_record_max_) << ")";
      heap->SetAllocationRecords(records);
      heap->SetAllocTrackingEnabled(true);
    }
    Runtime::Current()->GetInstrumentation()->InstrumentQuickAllocEntryPoints();
  } else {
    {
      MutexLock mu(self, *Locks::alloc_tracker_lock_);
      if (!heap->IsAllocTrackingEnabled()) {
        return;  // Already disabled, bail.
      }
      heap->SetAllocTrackingEnabled(false);
      LOG(INFO) << "Disabling alloc tracker";
      heap->SetAllocationRecords(nullptr);
    }
    // If an allocation comes in before we uninstrument, we will safely drop it on the floor.
    Runtime::Current()->GetInstrumentation()->UninstrumentQuickAllocEntryPoints();
  }
}

void AllocRecordObjectMap::RecordAllocation(Thread* self, mirror::Object* obj, size_t byte_count) {
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

  DCHECK_LE(records->Size(), records->alloc_record_max_);

  // Remove oldest record.
  if (records->Size() == records->alloc_record_max_) {
    records->RemoveOldest();
  }

  // Get stack trace.
  const size_t max_depth = records->max_stack_depth_;
  AllocRecordStackTrace* trace = new AllocRecordStackTrace(self->GetTid(), max_depth);
  // add scope to make "visitor" destroyed promptly, in order to set the trace->depth_
  {
    AllocRecordStackVisitor visitor(self, trace, max_depth);
    visitor.WalkStack();
  }

  // Fill in the basics.
  AllocRecord* record = new AllocRecord(byte_count, trace);

  records->Put(obj, record);
  DCHECK_LE(records->Size(), records->alloc_record_max_);
}

}  // namespace gc
}  // namespace art
