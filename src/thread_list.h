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

#ifndef ART_SRC_THREAD_LIST_H_
#define ART_SRC_THREAD_LIST_H_

#include "mutex.h"
#include "thread.h"

namespace art {

class ThreadList {
 public:
  static const uint32_t kMaxThreadId = 0xFFFF;
  static const uint32_t kInvalidId = 0;
  static const uint32_t kMainId = 1;

  ThreadList();
  ~ThreadList();

  void Dump(std::ostream& os);

  void Register(Thread* thread);

  void Unregister();

  bool Contains(Thread* thread);

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

 private:
  uint32_t AllocThreadId();
  void ReleaseThreadId(uint32_t id);

  mutable Mutex lock_;
  std::bitset<kMaxThreadId> allocated_ids_;
  std::list<Thread*> list_;

  friend class Thread;
  friend class ThreadListLock;

  DISALLOW_COPY_AND_ASSIGN(ThreadList);
};

class ThreadListLock {
 public:
  ThreadListLock(Thread* self = NULL) {
    if (self == NULL) {
      // Try to get it from TLS.
      self = Thread::Current();
    }
    Thread::State old_state;
    if (self != NULL) {
      old_state = self->SetState(Thread::kWaiting);  // TODO: VMWAIT
    } else {
      // This happens during VM shutdown.
      old_state = Thread::kUnknown;
    }
    Runtime::Current()->GetThreadList()->lock_.Lock();
    if (self != NULL) {
      self->SetState(old_state);
    }
  }

  ~ThreadListLock() {
    Runtime::Current()->GetThreadList()->lock_.Unlock();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ThreadListLock);
};

}  // namespace art

#endif  // ART_SRC_THREAD_LIST_H_
