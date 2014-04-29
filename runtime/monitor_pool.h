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

#ifndef ART_RUNTIME_MONITOR_POOL_H_
#define ART_RUNTIME_MONITOR_POOL_H_

#include "monitor.h"

#ifdef __LP64__
#include <bitset>
#include <stdint.h>

#include "runtime.h"
#include "safe_map.h"
#endif

namespace art {

// Abstraction to keep monitors small enough to fit in a lock word (32bits). On 32bit systems the
// monitor id loses the alignment bits of the Monitor*.
class MonitorPool {
 public:
  static MonitorPool* Create() {
#ifndef __LP64__
    return nullptr;
#else
    return new MonitorPool();
#endif
  }

  static Monitor* MonitorFromMonitorId(MonitorId mon_id) {
#ifndef __LP64__
    return reinterpret_cast<Monitor*>(mon_id << 3);
#else
    return Runtime::Current()->GetMonitorPool()->LookupMonitorFromTable(mon_id);
#endif
  }

  static MonitorId MonitorIdFromMonitor(Monitor* mon) {
#ifndef __LP64__
    return reinterpret_cast<MonitorId>(mon) >> 3;
#else
    return mon->GetMonitorId();
#endif
  }

  static MonitorId CreateMonitorId(Thread* self, Monitor* mon) {
#ifndef __LP64__
    UNUSED(self);
    return MonitorIdFromMonitor(mon);
#else
    return Runtime::Current()->GetMonitorPool()->AllocMonitorIdFromTable(self, mon);
#endif
  }

  static void ReleaseMonitorId(MonitorId mon_id) {
#ifndef __LP64__
    UNUSED(mon_id);
#else
    Runtime::Current()->GetMonitorPool()->ReleaseMonitorIdFromTable(mon_id);
#endif
  }

 private:
#ifdef __LP64__
  MonitorPool();

  Monitor* LookupMonitorFromTable(MonitorId mon_id);

  MonitorId LookupMonitorIdFromTable(Monitor* mon);

  MonitorId AllocMonitorIdFromTable(Thread* self, Monitor* mon);

  void ReleaseMonitorIdFromTable(MonitorId mon_id);

  ReaderWriterMutex allocated_ids_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  static constexpr uint32_t kMaxMonitorId = 0xFFFF;
  std::bitset<kMaxMonitorId> allocated_ids_ GUARDED_BY(allocated_ids_lock_);
  SafeMap<MonitorId, Monitor*> table_ GUARDED_BY(allocated_ids_lock_);
#endif
};

}  // namespace art

#endif  // ART_RUNTIME_MONITOR_POOL_H_
