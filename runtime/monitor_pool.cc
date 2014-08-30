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

#include "monitor_pool.h"

#include "base/logging.h"
#include "base/mutex-inl.h"
#include "thread-inl.h"
#include "monitor.h"

namespace art {

namespace mirror {
  class Object;
}  // namespace mirror

MonitorPool::MonitorPool()
    : num_chunks_(0), capacity_(0), first_free_(nullptr) {
  AllocateChunk();  // Get our first chunk.
}

// Assumes locks are held appropriately when necessary.
// We do not need a lock in the constructor, but we need one when in CreateMonitorInPool.
void MonitorPool::AllocateChunk() {
  DCHECK(first_free_ == nullptr);

  // Do we need to resize?
  if (num_chunks_ == capacity_) {
    if (capacity_ == 0U) {
      // Initialization.
      capacity_ = kInitialChunkStorage;
      uintptr_t* new_backing = new uintptr_t[capacity_];
      monitor_chunks_.StoreRelaxed(new_backing);
    } else {
      size_t new_capacity = 2 * capacity_;
      uintptr_t* new_backing = new uintptr_t[new_capacity];
      uintptr_t* old_backing = monitor_chunks_.LoadRelaxed();
      memcpy(new_backing, old_backing, sizeof(uintptr_t) * capacity_);
      monitor_chunks_.StoreRelaxed(new_backing);
      capacity_ = new_capacity;
      old_chunk_arrays_.push_back(old_backing);
      VLOG(monitor) << "Resizing to capacity " << capacity_;
    }
  }

  // Allocate the chunk.
  void* chunk = allocator_.allocate(kChunkSize);
  // Check we allocated memory.
  CHECK_NE(reinterpret_cast<uintptr_t>(nullptr), reinterpret_cast<uintptr_t>(chunk));
  // Check it is aligned as we need it.
  CHECK_EQ(0U, reinterpret_cast<uintptr_t>(chunk) % kMonitorAlignment);

  // Add the chunk.
  *(monitor_chunks_.LoadRelaxed() + num_chunks_) = reinterpret_cast<uintptr_t>(chunk);
  num_chunks_++;

  // Set up the free list
  Monitor* last = reinterpret_cast<Monitor*>(reinterpret_cast<uintptr_t>(chunk) +
                                             (kChunkCapacity - 1) * kAlignedMonitorSize);
  last->next_free_ = nullptr;
  // Eagerly compute id.
  last->monitor_id_ = OffsetToMonitorId((num_chunks_ - 1) * kChunkSize +
                                        (kChunkCapacity - 1) * kAlignedMonitorSize);
  for (size_t i = 0; i < kChunkCapacity - 1; ++i) {
    Monitor* before = reinterpret_cast<Monitor*>(reinterpret_cast<uintptr_t>(last) -
                                                 kAlignedMonitorSize);
    before->next_free_ = last;
    // Derive monitor_id from last.
    before->monitor_id_ = OffsetToMonitorId(MonitorIdToOffset(last->monitor_id_) -
                                            kAlignedMonitorSize);

    last = before;
  }
  DCHECK(last == reinterpret_cast<Monitor*>(chunk));
  first_free_ = last;
}

Monitor* MonitorPool::CreateMonitorInPool(Thread* self, Thread* owner, mirror::Object* obj,
                                          int32_t hash_code)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // We are gonna allocate, so acquire the writer lock.
  MutexLock mu(self, *Locks::allocated_monitor_ids_lock_);

  // Enough space, or need to resize?
  if (first_free_ == nullptr) {
    VLOG(monitor) << "Allocating a new chunk.";
    AllocateChunk();
  }

  Monitor* mon_uninitialized = first_free_;
  first_free_ = first_free_->next_free_;

  // Pull out the id which was preinitialized.
  MonitorId id = mon_uninitialized->monitor_id_;

  // Initialize it.
  Monitor* monitor = new(mon_uninitialized) Monitor(self, owner, obj, hash_code, id);

  return monitor;
}

void MonitorPool::ReleaseMonitorToPool(Thread* self, Monitor* monitor) {
  // Might be racy with allocation, so acquire lock.
  MutexLock mu(self, *Locks::allocated_monitor_ids_lock_);

  // Keep the monitor id. Don't trust it's not cleared.
  MonitorId id = monitor->monitor_id_;

  // Call the destructor.
  // TODO: Exception safety?
  monitor->~Monitor();

  // Add to the head of the free list.
  monitor->next_free_ = first_free_;
  first_free_ = monitor;

  // Rewrite monitor id.
  monitor->monitor_id_ = id;
}

void MonitorPool::ReleaseMonitorsToPool(Thread* self, MonitorList::Monitors* monitors) {
  for (Monitor* mon : *monitors) {
    ReleaseMonitorToPool(self, mon);
  }
}

}  // namespace art
