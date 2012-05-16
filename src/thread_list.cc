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

#include <unistd.h>

#include "debugger.h"
#include "scoped_heap_lock.h"
#include "scoped_thread_list_lock.h"

namespace art {

ThreadList::ThreadList()
    : allocated_ids_lock_("allocated thread ids lock"),
      thread_list_lock_("thread list lock", kThreadListLock),
      thread_start_cond_("thread start condition variable"),
      thread_exit_cond_("thread exit condition variable"),
      thread_suspend_count_lock_("thread suspend count lock", kThreadSuspendCountLock),
      thread_suspend_count_cond_("thread suspend count condition variable") {
  VLOG(threads) << "Default stack size: " << PrettySize(Runtime::Current()->GetDefaultStackSize());
}

ThreadList::~ThreadList() {
  // Detach the current thread if necessary. If we failed to start, there might not be any threads.
  // We need to detach the current thread here in case there's another thread waiting to join with
  // us.
  if (Contains(Thread::Current())) {
    Runtime::Current()->DetachCurrentThread();
  }

  WaitForOtherNonDaemonThreadsToExit();
  SuspendAllDaemonThreads();
}

bool ThreadList::Contains(Thread* thread) {
  return find(list_.begin(), list_.end(), thread) != list_.end();
}

pid_t ThreadList::GetLockOwner() {
  return thread_list_lock_.GetOwner();
}

void ThreadList::DumpForSigQuit(std::ostream& os) {
  ScopedThreadListLock thread_list_lock;
  DumpLocked(os);
}

void ThreadList::DumpLocked(std::ostream& os) {
  os << "DALVIK THREADS (" << list_.size() << "):\n";
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    (*it)->Dump(os);
    os << "\n";
  }
}

void ThreadList::ModifySuspendCount(Thread* thread, int delta, bool for_debugger) {
#ifndef NDEBUG
  DCHECK(delta == -1 || delta == +1 || delta == -thread->debug_suspend_count_)
      << delta << " " << thread->debug_suspend_count_ << " " << *thread;
  DCHECK_GE(thread->suspend_count_, thread->debug_suspend_count_) << *thread;
#endif
  if (delta == -1 && thread->suspend_count_ <= 0) {
    // This is expected if you attach a thread during a GC.
    if (!thread->IsStillStarting()) {
      LOG(FATAL) << *thread << " suspend count already zero";
    }
    return;
  }
  thread->suspend_count_ += delta;
  if (for_debugger) {
    thread->debug_suspend_count_ += delta;
  }
}

void ThreadList::FullSuspendCheck(Thread* thread) {
  CHECK(thread != NULL);
  CHECK_GE(thread->suspend_count_, 0);

  MutexLock mu(thread_suspend_count_lock_);
  if (thread->suspend_count_ == 0) {
    return;
  }

  VLOG(threads) << *thread << " self-suspending";
  {
    ScopedThreadStateChange tsc(thread, kSuspended);
    while (thread->suspend_count_ != 0) {
      /*
       * Wait for wakeup signal, releasing lock.  The act of releasing
       * and re-acquiring the lock provides the memory barriers we
       * need for correct behavior on SMP.
       */
      thread_suspend_count_cond_.Wait(thread_suspend_count_lock_);
    }
    CHECK_EQ(thread->suspend_count_, 0);
  }
  VLOG(threads) << *thread << " self-reviving";
}

void ThreadList::SuspendAll(bool for_debugger) {
  Thread* self = Thread::Current();

  VLOG(threads) << *self << " SuspendAll starting..." << (for_debugger ? " (debugger)" : "");

  CHECK_EQ(self->GetState(), kRunnable);
  ScopedThreadListLock thread_list_lock;
  Thread* debug_thread = Dbg::GetDebugThread();

  {
    // Increment everybody's suspend count (except our own).
    MutexLock mu(thread_suspend_count_lock_);
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      if (thread == self || (for_debugger && thread == debug_thread)) {
        continue;
      }
      VLOG(threads) << "requesting thread suspend: " << *thread;
      ModifySuspendCount(thread, +1, for_debugger);
    }
  }

  /*
   * Wait for everybody in kRunnable state to stop.  Other states
   * indicate the code is either running natively or sleeping quietly.
   * Any attempt to transition back to kRunnable will cause a check
   * for suspension, so it should be impossible for anything to execute
   * interpreted code or modify objects (assuming native code plays nicely).
   *
   * It's also okay if the thread transitions to a non-kRunnable state.
   *
   * Note we released the thread_suspend_count_lock_ before getting here,
   * so if another thread is fiddling with its suspend count (perhaps
   * self-suspending for the debugger) it won't block while we're waiting
   * in here.
   */
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    Thread* thread = *it;
    if (thread == self || (for_debugger && thread == debug_thread)) {
      continue;
    }
    thread->WaitUntilSuspended();
    VLOG(threads) << "thread suspended: " << *thread;
  }

  VLOG(threads) << *self << " SuspendAll complete";
}

void ThreadList::Suspend(Thread* thread, bool for_debugger) {
  DCHECK(thread != Thread::Current());
  thread_list_lock_.AssertHeld();

  // TODO: add another thread_suspend_lock_ to avoid GC/debugger races.

  VLOG(threads) << "Suspend(" << *thread << ") starting..." << (for_debugger ? " (debugger)" : "");

  if (!Contains(thread)) {
    return;
  }

  {
    MutexLock mu(thread_suspend_count_lock_);
    ModifySuspendCount(thread, +1, for_debugger);
  }

  thread->WaitUntilSuspended();

  VLOG(threads) << "Suspend(" << *thread << ") complete";
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
  MutexLock mu(thread_suspend_count_lock_);
  ModifySuspendCount(self, +1, true);

  // Suspend ourselves.
  CHECK_GT(self->suspend_count_, 0);
  self->SetState(kSuspended);
  VLOG(threads) << *self << " self-suspending (debugger)";

  // Tell JDWP that we've completed suspension. The JDWP thread can't
  // tell us to resume before we're fully asleep because we hold the
  // suspend count lock.
  Dbg::ClearWaitForEventThread();

  while (self->suspend_count_ != 0) {
    thread_suspend_count_cond_.Wait(thread_suspend_count_lock_);
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

void ThreadList::ResumeAll(bool for_debugger) {
  Thread* self = Thread::Current();

  VLOG(threads) << *self << " ResumeAll starting" << (for_debugger ? " (debugger)" : "");

  // Decrement the suspend counts for all threads.  No need for atomic
  // writes, since nobody should be moving until we decrement the count.
  // We do need to hold the thread list because of JNI attaches.
  {
    ScopedThreadListLock thread_list_lock;
    Thread* debug_thread = Dbg::GetDebugThread();
    MutexLock mu(thread_suspend_count_lock_);
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      if (thread == self || (for_debugger && thread == debug_thread)) {
        continue;
      }
      ModifySuspendCount(thread, -1, for_debugger);
    }
  }

  // Broadcast a notification to all suspended threads, some or all of
  // which may choose to wake up.  No need to wait for them.
  {
    VLOG(threads) << *self << " ResumeAll waking others";
    MutexLock mu(thread_suspend_count_lock_);
    thread_suspend_count_cond_.Broadcast();
  }

  VLOG(threads) << *self << " ResumeAll complete";
}

void ThreadList::Resume(Thread* thread, bool for_debugger) {
  DCHECK(thread != Thread::Current());

  if (!for_debugger) { // The debugger is very naughty. See Dbg::InvokeMethod.
    thread_list_lock_.AssertHeld();
  }

  VLOG(threads) << "Resume(" << *thread << ") starting..." << (for_debugger ? " (debugger)" : "");

  {
    MutexLock mu(thread_suspend_count_lock_);
    if (!Contains(thread)) {
      return;
    }
    ModifySuspendCount(thread, -1, for_debugger);
  }

  {
    VLOG(threads) << "Resume(" << *thread << ") waking others";
    MutexLock mu(thread_suspend_count_lock_);
    thread_suspend_count_cond_.Broadcast();
  }

  VLOG(threads) << "Resume(" << *thread << ") complete";
}

void ThreadList::RunWhileSuspended(Thread* thread, void (*callback)(void*), void* arg) {  // NOLINT
  DCHECK(thread != NULL);
  Thread* self = Thread::Current();
  if (thread != self) {
    Suspend(thread);
  }
  callback(arg);
  if (thread != self) {
    Resume(thread);
  }
}

void ThreadList::UndoDebuggerSuspensions() {
  Thread* self = Thread::Current();

  VLOG(threads) << *self << " UndoDebuggerSuspensions starting";

  {
    ScopedThreadListLock thread_list_lock;
    MutexLock mu(thread_suspend_count_lock_);
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      if (thread == self || thread->debug_suspend_count_ == 0) {
        continue;
      }
      ModifySuspendCount(thread, -thread->debug_suspend_count_, true);
    }
  }

  {
    MutexLock mu(thread_suspend_count_lock_);
    thread_suspend_count_cond_.Broadcast();
  }

  VLOG(threads) << "UndoDebuggerSuspensions(" << *self << ") complete";
}

void ThreadList::Register() {
  Thread* self = Thread::Current();

  VLOG(threads) << "ThreadList::Register() " << *self << "\n" << Dumpable<Thread>(*self);

  ScopedThreadListLock thread_list_lock;
  CHECK(!Contains(self));
  list_.push_back(self);
}

void ThreadList::Unregister() {
  Thread* self = Thread::Current();

  VLOG(threads) << "ThreadList::Unregister() " << *self;

  // Any time-consuming destruction, plus anything that can call back into managed code or
  // suspend and so on, must happen at this point, and not in ~Thread.
  self->Destroy();

  {
    // Remove this thread from the list.
    ScopedThreadListLock thread_list_lock;
    CHECK(Contains(self));
    list_.remove(self);
  }

  // Delete the Thread* and release the thin lock id.
  uint32_t thin_lock_id = self->thin_lock_id_;
  delete self;
  ReleaseThreadId(thin_lock_id);

  // Clear the TLS data, so that the underlying native thread is recognizably detached.
  // (It may wish to reattach later.)
  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, NULL), "detach self");

  // Signal that a thread just detached.
  thread_exit_cond_.Signal();
}

void ThreadList::ForEach(void (*callback)(Thread*, void*), void* context) {
  thread_list_lock_.AssertHeld();
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    callback(*it, context);
  }
}

void ThreadList::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  ScopedThreadListLock thread_list_lock;
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    (*it)->VisitRoots(visitor, arg);
  }
}

/*
 * Tell a new thread it's safe to start.
 *
 * We must hold the thread list lock before messing with another thread.
 * In the general case we would also need to verify that the new thread was
 * still in the thread list, but in our case the thread has not started
 * executing user code and therefore has not had a chance to exit.
 *
 * We move it to kVmWait, and it then shifts itself to kRunning, which
 * comes with a suspend-pending check. We do this after
 */
void ThreadList::SignalGo(Thread* child) {
  Thread* self = Thread::Current();
  CHECK(child != self);

  {
    ScopedThreadListLock thread_list_lock;
    VLOG(threads) << *self << " waiting for child " << *child << " to be in thread list...";

    // We wait for the child to tell us that it's in the thread list.
    while (child->GetState() != kStarting) {
      thread_start_cond_.Wait(thread_list_lock_);
    }
  }

  // If we switch out of runnable and then back in, we know there's no pending suspend.
  self->SetState(kVmWait);
  self->SetState(kRunnable);

  // Tell the child that it's safe: it will see any future suspend request.
  ScopedThreadListLock thread_list_lock;
  VLOG(threads) << *self << " telling child " << *child << " it's safe to proceed...";
  child->SetState(kVmWait);
  thread_start_cond_.Broadcast();
}

void ThreadList::WaitForGo() {
  Thread* self = Thread::Current();
  DCHECK(Contains(self));

  {
    ScopedThreadListLock thread_list_lock;

    // Tell our parent that we're in the thread list.
    VLOG(threads) << *self << " telling parent that we're now in thread list...";
    self->SetState(kStarting);
    thread_start_cond_.Broadcast();

    // Wait until our parent tells us there's no suspend still pending
    // from before we were on the thread list.
    VLOG(threads) << *self << " waiting for parent's go-ahead...";
    while (self->GetState() != kVmWait) {
      thread_start_cond_.Wait(thread_list_lock_);
    }
  }

  // Enter the runnable state. We know that any pending suspend will affect us now.
  VLOG(threads) << *self << " entering runnable state...";
  // Lock and unlock the heap lock. This ensures that if there was a GC in progress when we
  // started, we wait until it's over. Which means that if there's now another GC pending, our
  // suspend count is non-zero, so switching to the runnable state will suspend us.
  // TODO: find a better solution!
  {
    ScopedHeapLock heap_lock;
  }
  self->SetState(kRunnable);
}

bool ThreadList::AllOtherThreadsAreDaemons() {
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    // TODO: there's a race here with thread exit that's being worked around by checking if the peer
    // is null.
    Thread* thread = *it;
    if (thread != Thread::Current() && thread->GetPeer() != NULL && !thread->IsDaemon()) {
      return false;
    }
  }
  return true;
}

void ThreadList::WaitForOtherNonDaemonThreadsToExit() {
  ScopedThreadListLock thread_list_lock;
  while (!AllOtherThreadsAreDaemons()) {
    thread_exit_cond_.Wait(thread_list_lock_);
  }
}

void ThreadList::SuspendAllDaemonThreads() {
  ScopedThreadListLock thread_list_lock;

  // Tell all the daemons it's time to suspend.
  {
    MutexLock mu(thread_suspend_count_lock_);
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
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
}

uint32_t ThreadList::AllocThreadId() {
  MutexLock mu(allocated_ids_lock_);
  //ScopedThreadListLock thread_list_lock;
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
