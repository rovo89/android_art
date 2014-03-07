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

MonitorPool::MonitorPool() : allocated_ids_lock_("allocated monitor ids lock",
                                                 LockLevel::kMonitorPoolLock) {
}

Monitor* MonitorPool::LookupMonitorFromTable(MonitorId mon_id) {
  ReaderMutexLock mu(Thread::Current(), allocated_ids_lock_);
  return table_.Get(mon_id);
}

MonitorId MonitorPool::AllocMonitorIdFromTable(Thread* self, Monitor* mon) {
  WriterMutexLock mu(self, allocated_ids_lock_);
  for (size_t i = 0; i < allocated_ids_.size(); ++i) {
    if (!allocated_ids_[i]) {
      allocated_ids_.set(i);
      MonitorId mon_id = i + 1;  // Zero is reserved to mean "invalid".
      table_.Put(mon_id, mon);
      return mon_id;
    }
  }
  LOG(FATAL) << "Out of internal monitor ids";
  return 0;
}

void MonitorPool::ReleaseMonitorIdFromTable(MonitorId mon_id) {
  WriterMutexLock mu(Thread::Current(), allocated_ids_lock_);
  DCHECK(table_.Get(mon_id) != nullptr);
  table_.erase(mon_id);
  --mon_id;  // Zero is reserved to mean "invalid".
  DCHECK(allocated_ids_[mon_id]) << mon_id;
  allocated_ids_.reset(mon_id);
}

}  // namespace art
