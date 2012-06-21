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

class TimingLogger;

class ThreadList {
 public:
  static const uint32_t kMaxThreadId = 0xFFFF;
  static const uint32_t kInvalidId = 0;
  static const uint32_t kMainId = 1;

  explicit ThreadList();
  ~ThreadList();

  void DumpForSigQuit(std::ostream& os);
  void DumpLocked(std::ostream& os); // For thread suspend timeout dumps.
  pid_t GetLockOwner(); // For SignalCatcher.

  // Thread suspension support.
  void FullSuspendCheck(Thread* thread);
  void ResumeAll(bool for_debugger = false);
  void Resume(Thread* thread, bool for_debugger = false);
  void RunWhileSuspended(Thread* thread, void (*callback)(void*), void* arg);  // NOLINT
  void SuspendAll(bool for_debugger = false);
  void SuspendSelfForDebugger();
  void Suspend(Thread* thread, bool for_debugger = false);
  void UndoDebuggerSuspensions();

  // Iterates over all the threads.
  void ForEach(void (*callback)(Thread*, void*), void* context);

  void Register();
  void Unregister();

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

  // Handshaking for new thread creation.
  void SignalGo(Thread* child);
  void WaitForGo();

 private:
  typedef std::list<Thread*>::const_iterator It; // TODO: C++0x auto

  uint32_t AllocThreadId();
  void ReleaseThreadId(uint32_t id);

  bool Contains(Thread* thread);
  bool Contains(pid_t tid);

  void DumpUnattachedThreads(std::ostream& os);

  bool AllOtherThreadsAreDaemons();
  void SuspendAllDaemonThreads();
  void WaitForOtherNonDaemonThreadsToExit();

  static void ModifySuspendCount(Thread* thread, int delta, bool for_debugger);

  mutable Mutex allocated_ids_lock_;
  std::bitset<kMaxThreadId> allocated_ids_ GUARDED_BY(allocated_ids_lock_);

  mutable Mutex thread_list_lock_;
  std::list<Thread*> list_; // TODO: GUARDED_BY(thread_list_lock_);

  ConditionVariable thread_start_cond_;
  ConditionVariable thread_exit_cond_;

  // This lock guards every thread's suspend_count_ field...
  mutable Mutex thread_suspend_count_lock_;
  // ...and is used in conjunction with this condition variable.
  ConditionVariable thread_suspend_count_cond_ GUARDED_BY(thread_suspend_count_lock_);

  friend class Thread;
  friend class ScopedThreadListLock;
  friend class ScopedThreadListLockReleaser;

  DISALLOW_COPY_AND_ASSIGN(ThreadList);
};

}  // namespace art

#endif  // ART_SRC_THREAD_LIST_H_
