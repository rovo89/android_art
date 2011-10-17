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

  explicit ThreadList(bool verbose);
  ~ThreadList();

  void Dump(std::ostream& os);
  uint32_t GetLockOwner(); // For SignalCatcher.

  // Thread suspension support.
  void FullSuspendCheck(Thread* thread);
  void ResumeAll();
  void SuspendAll();
  void RunWhileSuspended(Thread* thread, void (*callback)(void*), void* arg);

  void Register();
  void Unregister();

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

  // Handshaking for new thread creation.
  void SignalGo(Thread* child);
  void WaitForGo();

 private:
  typedef std::list<Thread*>::const_iterator It; // TODO: C++0x auto

  bool AllThreadsAreDaemons();
  uint32_t AllocThreadId();
  bool Contains(Thread* thread);
  void ReleaseThreadId(uint32_t id);
  void Resume(Thread* thread);
  void Suspend(Thread* thread);
  void SuspendAllDaemonThreads();
  void WaitForNonDaemonThreadsToExit();

  bool verbose_;

  mutable Mutex thread_list_lock_;
  std::bitset<kMaxThreadId> allocated_ids_;
  std::list<Thread*> list_;

  ConditionVariable thread_start_cond_;
  ConditionVariable thread_exit_cond_;

  // This lock guards every thread's suspend_count_ field...
  mutable Mutex thread_suspend_count_lock_;
  // ...and is used in conjunction with this condition variable.
  ConditionVariable thread_suspend_count_cond_;

  friend class Thread;
  friend class ThreadListLock;

  DISALLOW_COPY_AND_ASSIGN(ThreadList);
};

class ThreadListLock {
 public:
  explicit ThreadListLock(Thread* self = NULL) {
    if (self == NULL) {
      // Try to get it from TLS.
      self = Thread::Current();
    }
    // self may still be NULL during VM shutdown.
    Thread::State old_state;
    if (self != NULL) {
      old_state = self->SetState(Thread::kVmWait);
    }
    Runtime::Current()->GetThreadList()->thread_list_lock_.Lock();
    if (self != NULL) {
      self->SetState(old_state);
    }
  }

  ~ThreadListLock() {
    Runtime::Current()->GetThreadList()->thread_list_lock_.Unlock();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ThreadListLock);
};

}  // namespace art

#endif  // ART_SRC_THREAD_LIST_H_
