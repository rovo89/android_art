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

#ifndef ART_RUNTIME_GC_ALLOCATION_RECORD_H_
#define ART_RUNTIME_GC_ALLOCATION_RECORD_H_

#include <list>

#include "base/mutex.h"
#include "object_callbacks.h"
#include "gc_root.h"

namespace art {

class ArtMethod;
class Thread;

namespace mirror {
  class Class;
  class Object;
}

namespace gc {

class AllocRecordStackTraceElement {
 public:
  AllocRecordStackTraceElement() : method_(nullptr), dex_pc_(0) {}

  int32_t ComputeLineNumber() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ArtMethod* GetMethod() const {
    return method_;
  }

  void SetMethod(ArtMethod* m) {
    method_ = m;
  }

  uint32_t GetDexPc() const {
    return dex_pc_;
  }

  void SetDexPc(uint32_t pc) {
    dex_pc_ = pc;
  }

  bool operator==(const AllocRecordStackTraceElement& other) const {
    if (this == &other) return true;
    return method_ == other.method_ && dex_pc_ == other.dex_pc_;
  }

 private:
  ArtMethod* method_;
  uint32_t dex_pc_;
};

class AllocRecordStackTrace {
 public:
  static constexpr size_t kHashMultiplier = 17;

  AllocRecordStackTrace(pid_t tid, size_t max_depth)
      : tid_(tid), depth_(0), stack_(new AllocRecordStackTraceElement[max_depth]) {}

  ~AllocRecordStackTrace() {
    delete[] stack_;
  }

  pid_t GetTid() const {
    return tid_;
  }

  size_t GetDepth() const {
    return depth_;
  }

  void SetDepth(size_t depth) {
    depth_ = depth;
  }

  const AllocRecordStackTraceElement& GetStackElement(size_t index) const {
    DCHECK_LT(index, depth_);
    return stack_[index];
  }

  void SetStackElementAt(size_t index, ArtMethod* m, uint32_t dex_pc) {
    stack_[index].SetMethod(m);
    stack_[index].SetDexPc(dex_pc);
  }

  bool operator==(const AllocRecordStackTrace& other) const {
    if (this == &other) return true;
    if (depth_ != other.depth_) return false;
    for (size_t i = 0; i < depth_; ++i) {
      if (!(stack_[i] == other.stack_[i])) return false;
    }
    return true;
  }

 private:
  const pid_t tid_;
  size_t depth_;
  AllocRecordStackTraceElement* const stack_;
};

struct HashAllocRecordTypes {
  size_t operator()(const AllocRecordStackTraceElement& r) const {
    return std::hash<void*>()(reinterpret_cast<void*>(r.GetMethod())) *
        AllocRecordStackTrace::kHashMultiplier + std::hash<uint32_t>()(r.GetDexPc());
  }

  size_t operator()(const AllocRecordStackTrace& r) const {
    size_t depth = r.GetDepth();
    size_t result = r.GetTid() * AllocRecordStackTrace::kHashMultiplier + depth;
    for (size_t i = 0; i < depth; ++i) {
      result = result * AllocRecordStackTrace::kHashMultiplier + (*this)(r.GetStackElement(i));
    }
    return result;
  }
};

template <typename T> struct HashAllocRecordTypesPtr {
  size_t operator()(const T* r) const {
    if (r == nullptr) return 0;
    return HashAllocRecordTypes()(*r);
  }
};

template <typename T> struct EqAllocRecordTypesPtr {
  bool operator()(const T* r1, const T* r2) const {
    if (r1 == r2) return true;
    if (r1 == nullptr || r2 == nullptr) return false;
    return *r1 == *r2;
  }
};

class AllocRecord {
 public:
  // All instances of AllocRecord should be managed by an instance of AllocRecordObjectMap.
  AllocRecord(size_t count, AllocRecordStackTrace* trace)
      : byte_count_(count), trace_(trace) {}

  ~AllocRecord() {
    delete trace_;
  }

  size_t GetDepth() const {
    return trace_->GetDepth();
  }

  const AllocRecordStackTrace* GetStackTrace() const {
    return trace_;
  }

  size_t ByteCount() const {
    return byte_count_;
  }

  pid_t GetTid() const {
    return trace_->GetTid();
  }

  const AllocRecordStackTraceElement& StackElement(size_t index) const {
    return trace_->GetStackElement(index);
  }

 private:
  const size_t byte_count_;
  // TODO: Currently trace_ is like a std::unique_ptr,
  // but in future with deduplication it could be a std::shared_ptr.
  const AllocRecordStackTrace* const trace_;
};

class AllocRecordObjectMap {
 public:
  // Since the entries contain weak roots, they need a read barrier. Do not directly access
  // the mirror::Object pointers in it. Use functions that contain read barriers.
  // No need for "const AllocRecord*" in the list, because all fields of AllocRecord are const.
  typedef std::list<std::pair<GcRoot<mirror::Object>, AllocRecord*>> EntryList;

  // "static" because it is part of double-checked locking. It needs to check a bool first,
  // in order to make sure the AllocRecordObjectMap object is not null.
  static void RecordAllocation(Thread* self, mirror::Object* obj, size_t byte_count)
      LOCKS_EXCLUDED(Locks::alloc_tracker_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void SetAllocTrackingEnabled(bool enabled) LOCKS_EXCLUDED(Locks::alloc_tracker_lock_);

  AllocRecordObjectMap() EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_)
      : alloc_record_max_(kDefaultNumAllocRecords),
        max_stack_depth_(kDefaultAllocStackDepth),
        alloc_ddm_thread_id_(0) {}

  ~AllocRecordObjectMap();

  void Put(mirror::Object* obj, AllocRecord* record)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_) {
    entries_.emplace_back(GcRoot<mirror::Object>(obj), record);
  }

  size_t Size() const SHARED_LOCKS_REQUIRED(Locks::alloc_tracker_lock_) {
    return entries_.size();
  }

  void SweepAllocationRecords(IsMarkedCallback* callback, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_);

  void RemoveOldest()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_) {
    DCHECK(!entries_.empty());
    delete entries_.front().second;
    entries_.pop_front();
  }

  // TODO: Is there a better way to hide the entries_'s type?
  EntryList::iterator Begin()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_) {
    return entries_.begin();
  }

  EntryList::iterator End()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_) {
    return entries_.end();
  }

  EntryList::reverse_iterator RBegin()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_) {
    return entries_.rbegin();
  }

  EntryList::reverse_iterator REnd()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_) {
    return entries_.rend();
  }

 private:
  static constexpr size_t kDefaultNumAllocRecords = 512 * 1024;
  static constexpr size_t kDefaultAllocStackDepth = 4;
  size_t alloc_record_max_ GUARDED_BY(Locks::alloc_tracker_lock_);
  // The implementation always allocates max_stack_depth_ number of frames for each stack trace.
  // As long as the max depth is not very large, this is not a waste of memory since most stack
  // traces will fill up the max depth number of the frames.
  size_t max_stack_depth_ GUARDED_BY(Locks::alloc_tracker_lock_);
  pid_t alloc_ddm_thread_id_ GUARDED_BY(Locks::alloc_tracker_lock_);
  EntryList entries_ GUARDED_BY(Locks::alloc_tracker_lock_);

  void SetProperties() EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_);
};

}  // namespace gc
}  // namespace art
#endif  // ART_RUNTIME_GC_ALLOCATION_RECORD_H_
