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

#include "thread_list.h"

#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

#include "debugger.h"
#include "timing_logger.h"
#include "utils.h"

namespace art {

ThreadList::ThreadList()
    : allocated_ids_lock_("allocated thread ids lock"),
      suspend_all_count_(0), debug_suspend_all_count_(0),
      thread_exit_cond_("thread exit condition variable") {
}

ThreadList::~ThreadList() {
  // Detach the current thread if necessary. If we failed to start, there might not be any threads.
  // We need to detach the current thread here in case there's another thread waiting to join with
  // us.
  if (Contains(Thread::Current())) {
    Runtime::Current()->DetachCurrentThread();
  }

  WaitForOtherNonDaemonThreadsToExit();
  // TODO: there's an unaddressed race here where a thread may attach during shutdown, see
  //       Thread::Init.
  SuspendAllDaemonThreads();
}

bool ThreadList::Contains(Thread* thread) {
  return find(list_.begin(), list_.end(), thread) != list_.end();
}

bool ThreadList::Contains(pid_t tid) {
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    if ((*it)->tid_ == tid) {
      return true;
    }
  }
  return false;
}

pid_t ThreadList::GetLockOwner() {
  return GlobalSynchronization::thread_list_lock_->GetExclusiveOwnerTid();
}

void ThreadList::DumpForSigQuit(std::ostream& os) {
  {
    MutexLock mu(*GlobalSynchronization::thread_list_lock_);
    DumpLocked(os);
  }
  DumpUnattachedThreads(os);
}

static void DumpUnattachedThread(std::ostream& os, pid_t tid) {
  Thread::DumpState(os, NULL, tid);
  DumpKernelStack(os, tid, "  kernel: ", false);
  // TODO: Reenable this when the native code in system_server can handle it.
  // Currently "adb shell kill -3 `pid system_server`" will cause it to exit.
  if (false) {
    DumpNativeStack(os, tid, "  native: ", false);
  }
  os << "\n";
}

void ThreadList::DumpUnattachedThreads(std::ostream& os) {
  DIR* d = opendir("/proc/self/task");
  if (!d) {
    return;
  }

  dirent de;
  dirent* e;
  while (!readdir_r(d, &de, &e) && e != NULL) {
    char* end;
    pid_t tid = strtol(de.d_name, &end, 10);
    if (!*end) {
      bool contains;
      {
        MutexLock mu(*GlobalSynchronization::thread_list_lock_);
        contains = Contains(tid);
      }
      if (!contains) {
        DumpUnattachedThread(os, tid);
      }
    }
  }
  closedir(d);
}

void ThreadList::DumpLocked(std::ostream& os) {
  GlobalSynchronization::thread_list_lock_->AssertHeld();
  os << "DALVIK THREADS (" << list_.size() << "):\n";
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    (*it)->Dump(os);
    os << "\n";
  }
}

void ThreadList::AssertThreadsAreSuspended() {
  MutexLock mu(*GlobalSynchronization::thread_list_lock_);
  MutexLock mu2(*GlobalSynchronization::thread_suspend_count_lock_);
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    Thread* thread = *it;
    CHECK_NE(thread->GetState(), kRunnable);
  }
}

// Attempt to rectify locks so that we dump thread list with required locks before exiting.
static void UnsafeLogFatalForThreadSuspendAllTimeout() NO_THREAD_SAFETY_ANALYSIS {
  Runtime* runtime = Runtime::Current();
  std::ostringstream ss;
  ss << "Thread suspend timeout\n";
  runtime->DumpLockHolders(ss);
  ss << "\n";
  GlobalSynchronization::mutator_lock_->SharedTryLock();
  if (!GlobalSynchronization::mutator_lock_->IsSharedHeld()) {
    LOG(WARNING) << "Dumping thread list without holding mutator_lock_";
  }
  GlobalSynchronization::thread_list_lock_->TryLock();
  if (!GlobalSynchronization::thread_list_lock_->IsExclusiveHeld()) {
    LOG(WARNING) << "Dumping thread list without holding thread_list_lock_";
  }
  runtime->GetThreadList()->DumpLocked(ss);
  LOG(FATAL) << ss.str();
}

void ThreadList::SuspendAll() {
  Thread* self = Thread::Current();

  VLOG(threads) << *self << " SuspendAll starting...";

  if (kIsDebugBuild) {
    GlobalSynchronization::mutator_lock_->AssertNotHeld();
    GlobalSynchronization::thread_list_lock_->AssertNotHeld();
    GlobalSynchronization::thread_suspend_count_lock_->AssertNotHeld();
    MutexLock mu(*GlobalSynchronization::thread_suspend_count_lock_);
    CHECK_NE(self->GetState(), kRunnable);
  }
  {
    MutexLock mu(*GlobalSynchronization::thread_list_lock_);
    {
      MutexLock mu2(*GlobalSynchronization::thread_suspend_count_lock_);
      // Update global suspend all state for attaching threads.
      ++suspend_all_count_;
      // Increment everybody's suspend count (except our own).
      for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
        Thread* thread = *it;
        if (thread == self) {
          continue;
        }
        VLOG(threads) << "requesting thread suspend: " << *thread;
        thread->ModifySuspendCount(+1, false);
      }
    }
  }

  // Block on the mutator lock until all Runnable threads release their share of access. Timeout
  // if we wait more than 30 seconds.
  timespec timeout;
  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_sec += 30;
  if (UNLIKELY(!GlobalSynchronization::mutator_lock_->ExclusiveLockWithTimeout(timeout))) {
    UnsafeLogFatalForThreadSuspendAllTimeout();
  }

  // Debug check that all threads are suspended.
  AssertThreadsAreSuspended();

  VLOG(threads) << *self << " SuspendAll complete";
}

void ThreadList::ResumeAll() {
  Thread* self = Thread::Current();

  VLOG(threads) << *self << " ResumeAll starting";
  {
    MutexLock mu(*GlobalSynchronization::thread_list_lock_);
    MutexLock mu2(*GlobalSynchronization::thread_suspend_count_lock_);
    // Update global suspend all state for attaching threads.
    --suspend_all_count_;
    // Decrement the suspend counts for all threads.
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      if (thread == self) {
        continue;
      }
      thread->ModifySuspendCount(-1, false);
    }

    // Broadcast a notification to all suspended threads, some or all of
    // which may choose to wake up.  No need to wait for them.
    VLOG(threads) << *self << " ResumeAll waking others";
    Thread::resume_cond_->Broadcast();
  }
  GlobalSynchronization::mutator_lock_->ExclusiveUnlock();
  VLOG(threads) << *self << " ResumeAll complete";
}

void ThreadList::Resume(Thread* thread, bool for_debugger) {
  DCHECK(thread != Thread::Current());
  VLOG(threads) << "Resume(" << *thread << ") starting..." << (for_debugger ? " (debugger)" : "");

  {
    // To check Contains.
    MutexLock mu(*GlobalSynchronization::thread_list_lock_);
    // To check IsSuspended.
    MutexLock mu2(*GlobalSynchronization::thread_suspend_count_lock_);
    CHECK(thread->IsSuspended());
    if (!Contains(thread)) {
      return;
    }
    thread->ModifySuspendCount(-1, for_debugger);
  }

  {
    VLOG(threads) << "Resume(" << *thread << ") waking others";
    MutexLock mu(*GlobalSynchronization::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast();
  }

  VLOG(threads) << "Resume(" << *thread << ") complete";
}

void ThreadList::SuspendAllForDebugger() {
  Thread* self = Thread::Current();
  Thread* debug_thread = Dbg::GetDebugThread();

  VLOG(threads) << *self << " SuspendAllForDebugger starting...";

  {
    MutexLock mu(*GlobalSynchronization::thread_list_lock_);
    {
      MutexLock mu(*GlobalSynchronization::thread_suspend_count_lock_);
      // Update global suspend all state for attaching threads.
      ++suspend_all_count_;
      ++debug_suspend_all_count_;
      // Increment everybody's suspend count (except our own).
      for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
        Thread* thread = *it;
        if (thread == self || thread == debug_thread) {
          continue;
        }
        VLOG(threads) << "requesting thread suspend: " << *thread;
        thread->ModifySuspendCount(+1, true);
      }
    }
  }

  // Block on the mutator lock until all Runnable threads release their share of access. Timeout
  // if we wait more than 30 seconds.
  timespec timeout;
  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_sec += 30;
  if (!GlobalSynchronization::mutator_lock_->ExclusiveLockWithTimeout(timeout)) {
    UnsafeLogFatalForThreadSuspendAllTimeout();
  } else {
    // Debugger suspends all threads but doesn't hold onto the mutator_lock_.
    GlobalSynchronization::mutator_lock_->ExclusiveUnlock();
  }

  AssertThreadsAreSuspended();

  VLOG(threads) << *self << " SuspendAll complete";
}

void ThreadList::SuspendSelfForDebugger() {
  Thread* self = Thread::Current();

  // The debugger thread must not suspend itself due to debugger activity!
  Thread* debug_thread = Dbg::GetDebugThread();
  CHECK(debug_thread != NULL);
  CHECK(self != debug_thread);

  // Collisions with other suspends aren't really interesting. We want
  // to ensure that we're the only one fiddling with the suspend count
  // though.
  MutexLock mu(*GlobalSynchronization::thread_suspend_count_lock_);
  self->ModifySuspendCount(+1, true);

  // Suspend ourselves.
  CHECK_GT(self->suspend_count_, 0);
  self->SetState(kSuspended);
  VLOG(threads) << *self << " self-suspending (debugger)";

  // Tell JDWP that we've completed suspension. The JDWP thread can't
  // tell us to resume before we're fully asleep because we hold the
  // suspend count lock.
  Dbg::ClearWaitForEventThread();

  while (self->suspend_count_ != 0) {
    Thread::resume_cond_->Wait(*GlobalSynchronization::thread_suspend_count_lock_);
    if (self->suspend_count_ != 0) {
      // The condition was signaled but we're still suspended. This
      // can happen if the debugger lets go while a SIGQUIT thread
      // dump event is pending (assuming SignalCatcher was resumed for
      // just long enough to try to grab the thread-suspend lock).
      LOG(DEBUG) << *self << " still suspended after undo "
                 << "(suspend count=" << self->suspend_count_ << ")";
    }
  }
  CHECK_EQ(self->suspend_count_, 0);
  self->SetState(kRunnable);
  VLOG(threads) << *self << " self-reviving (debugger)";
}

void ThreadList::UndoDebuggerSuspensions() {
  Thread* self = Thread::Current();

  VLOG(threads) << *self << " UndoDebuggerSuspensions starting";

  {
    MutexLock mu(*GlobalSynchronization::thread_list_lock_);
    MutexLock mu2(*GlobalSynchronization::thread_suspend_count_lock_);
    // Update global suspend all state for attaching threads.
    suspend_all_count_ -= debug_suspend_all_count_;
    debug_suspend_all_count_ = 0;
    // Update running threads.
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      if (thread == self || thread->debug_suspend_count_ == 0) {
        continue;
      }
      thread->ModifySuspendCount(-thread->debug_suspend_count_, true);
    }
  }

  {
    MutexLock mu(*GlobalSynchronization::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast();
  }

  VLOG(threads) << "UndoDebuggerSuspensions(" << *self << ") complete";
}

void ThreadList::WaitForOtherNonDaemonThreadsToExit() {
  GlobalSynchronization::mutator_lock_->AssertNotHeld();
  MutexLock mu(*GlobalSynchronization::thread_list_lock_);
  bool all_threads_are_daemons;
  do {
    all_threads_are_daemons = true;
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      // TODO: there's a race here with thread exit that's being worked around by checking if the
      // thread has a peer.
      Thread* thread = *it;
      if (thread != Thread::Current() && thread->HasPeer() && !thread->IsDaemon()) {
        all_threads_are_daemons = false;
        break;
      }
    }
    if (!all_threads_are_daemons) {
      // Wait for another thread to exit before re-checking.
      thread_exit_cond_.Wait(*GlobalSynchronization::thread_list_lock_);
    }
  } while(!all_threads_are_daemons);
}

void ThreadList::SuspendAllDaemonThreads() {
  MutexLock mu(*GlobalSynchronization::thread_list_lock_);
  { // Tell all the daemons it's time to suspend.
    MutexLock mu2(*GlobalSynchronization::thread_suspend_count_lock_);
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      // This is only run after all non-daemon threads have exited, so the remainder should all be
      // daemons.
      CHECK(thread->IsDaemon());
      if (thread != Thread::Current()) {
        ++thread->suspend_count_;
      }
    }
  }
  // Give the threads a chance to suspend, complaining if they're slow.
  bool have_complained = false;
  for (int i = 0; i < 10; ++i) {
    usleep(200 * 1000);
    bool all_suspended = true;
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      MutexLock mu2(*GlobalSynchronization::thread_suspend_count_lock_);
      if (thread != Thread::Current() && thread->GetState() == kRunnable) {
        if (!have_complained) {
          LOG(WARNING) << "daemon thread not yet suspended: " << *thread;
          have_complained = true;
        }
        all_suspended = false;
      }
    }
    if (all_suspended) {
      return;
    }
  }
  LOG(ERROR) << "suspend all daemons failed";
}
void ThreadList::Register(Thread* self) {
  DCHECK_EQ(self, Thread::Current());

  if (VLOG_IS_ON(threads)) {
    std::ostringstream oss;
    self->ShortDump(oss);  // We don't hold the mutator_lock_ yet and so cannot call Dump.
    LOG(INFO) << "ThreadList::Register() " << *self  << "\n" << oss;
  }

  // Atomically add self to the thread list and make its thread_suspend_count_ reflect ongoing
  // SuspendAll requests.
  MutexLock mu(*GlobalSynchronization::thread_list_lock_);
  MutexLock mu2(*GlobalSynchronization::thread_suspend_count_lock_);
  self->suspend_count_ = suspend_all_count_;
  self->debug_suspend_count_ = debug_suspend_all_count_;
  CHECK(!Contains(self));
  list_.push_back(self);
}

void ThreadList::Unregister(Thread* self) {
  DCHECK_EQ(self, Thread::Current());

  VLOG(threads) << "ThreadList::Unregister() " << *self;

  // Any time-consuming destruction, plus anything that can call back into managed code or
  // suspend and so on, must happen at this point, and not in ~Thread.
  self->Destroy();

  {
    // Remove this thread from the list.
    MutexLock mu(*GlobalSynchronization::thread_list_lock_);
    CHECK(Contains(self));
    list_.remove(self);
  }

  // Delete the Thread* and release the thin lock id.
  uint32_t thin_lock_id = self->thin_lock_id_;
  ReleaseThreadId(thin_lock_id);
  delete self;

  // Clear the TLS data, so that the underlying native thread is recognizably detached.
  // (It may wish to reattach later.)
  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, NULL), "detach self");

  // Signal that a thread just detached.
  MutexLock mu(*GlobalSynchronization::thread_list_lock_);
  thread_exit_cond_.Signal();
}

void ThreadList::ForEach(void (*callback)(Thread*, void*), void* context) {
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    callback(*it, context);
  }
}

void ThreadList::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  MutexLock mu(*GlobalSynchronization::thread_list_lock_);
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    (*it)->VisitRoots(visitor, arg);
  }
}

uint32_t ThreadList::AllocThreadId() {
  MutexLock mu(allocated_ids_lock_);
  for (size_t i = 0; i < allocated_ids_.size(); ++i) {
    if (!allocated_ids_[i]) {
      allocated_ids_.set(i);
      return i + 1; // Zero is reserved to mean "invalid".
    }
  }
  LOG(FATAL) << "Out of internal thread ids";
  return 0;
}

void ThreadList::ReleaseThreadId(uint32_t id) {
  MutexLock mu(allocated_ids_lock_);
  --id; // Zero is reserved to mean "invalid".
  DCHECK(allocated_ids_[id]) << id;
  allocated_ids_.reset(id);
}

}  // namespace art
