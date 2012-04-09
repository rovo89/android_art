/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "scoped_thread_list_lock.h"

#include "runtime.h"
#include "thread_list.h"

namespace art {

ScopedThreadListLock::ScopedThreadListLock() {
  // Avoid deadlock between two threads trying to SuspendAll
  // simultaneously by going to kVmWait if the lock cannot be
  // immediately acquired.
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  if (!thread_list->thread_list_lock_.TryLock()) {
    Thread* self = Thread::Current();
    if (self == NULL) {
      // Self may be null during shutdown, but in that case there's no point going to kVmWait.
      thread_list->thread_list_lock_.Lock();
    } else {
      ThreadState old_thread_state = self->SetState(kVmWait);
      thread_list->thread_list_lock_.Lock();
      // If we have the lock, by definition there's no GC in progress (though we
      // might be taking the lock in order to start one). We avoid the suspend
      // check here so we don't risk going to sleep on the thread suspend count lock
      // while holding the thread list lock.
      self->SetStateWithoutSuspendCheck(old_thread_state);
    }
  }
}

ScopedThreadListLock::~ScopedThreadListLock() {
  Runtime::Current()->GetThreadList()->thread_list_lock_.Unlock();
}

}  // namespace art
