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

#include "scoped_thread_list_lock_releaser.h"

#include "runtime.h"
#include "thread_list.h"

namespace art {

ScopedThreadListLockReleaser::ScopedThreadListLockReleaser() : unlocked_(false) {
  if (Thread::Current() == NULL) {
    CHECK(Runtime::Current()->IsShuttingDown());
    return;
  }

  if (Thread::Current()->held_mutexes_[kThreadListLock] > 0) {
    Runtime::Current()->GetThreadList()->thread_list_lock_.Unlock();
    unlocked_ = true;
  }
}

ScopedThreadListLockReleaser::~ScopedThreadListLockReleaser() {
  if (unlocked_) {
    Runtime::Current()->GetThreadList()->thread_list_lock_.Lock();
  }
}

}  // namespace art
