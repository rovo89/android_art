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

namespace art {

// TODO: merge with ThreadListLock?
class ThreadListLocker {
 public:

  explicit ThreadListLocker(const ThreadList* thread_list) : thread_list_(thread_list) {
    // Avoid deadlock between two threads trying to SuspendAll
    // simultaneously by going to kVmWait if the lock cannot be
    // immediately acquired.
    if (!thread_list_->thread_list_lock_.TryLock()) {
      Thread* self = Thread::Current();
      if (self == NULL) {
          thread_list_->thread_list_lock_.Lock();
      } else {
          ScopedThreadStateChange tsc(self, Thread::kVmWait);
          thread_list_->thread_list_lock_.Lock();
      }
    }
  }

  ~ThreadListLocker() {
    thread_list_->thread_list_lock_.Unlock();
  }

 private:
  const ThreadList* thread_list_;
  DISALLOW_COPY_AND_ASSIGN(ThreadListLocker);
};

ThreadList::ThreadList(bool verbose)
    : verbose_(verbose),
      thread_list_lock_("thread list lock"),
      thread_start_cond_("thread_start_cond_"),
      thread_exit_cond_("thread_exit_cond_"),
      thread_suspend_count_lock_("thread suspend count lock"),
      thread_suspend_count_cond_("thread_suspend_count_cond_") {
}

ThreadList::~ThreadList() {
  // Detach the current thread if necessary.
  if (Contains(Thread::Current())) {
    Runtime::Current()->DetachCurrentThread();
  }

  WaitForNonDaemonThreadsToExit();
  SuspendAllDaemonThreads();
}

bool ThreadList::Contains(Thread* thread) {
  return find(list_.begin(), list_.end(), thread) != list_.end();
}

pid_t ThreadList::GetLockOwner() {
  return thread_list_lock_.GetOwner();
}

void ThreadList::Dump(std::ostream& os) {
  ThreadListLocker locker(this);
  os << "DALVIK THREADS (" << list_.size() << "):\n";
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    (*it)->Dump(os);
    os << "\n";
  }
}

void ThreadList::FullSuspendCheck(Thread* thread) {
  CHECK(thread != NULL);
  CHECK_GE(thread->suspend_count_, 0);

  MutexLock mu(thread_suspend_count_lock_);
  if (thread->suspend_count_ == 0) {
    return;
  }

  if (verbose_) {
    LOG(INFO) << *thread << " self-suspending";
  }
  {
    ScopedThreadStateChange tsc(thread, Thread::kSuspended);
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
  if (verbose_) {
    LOG(INFO) << *thread << " self-reviving";
  }
}

void ThreadList::SuspendAll(bool for_debugger) {
  Thread* self = Thread::Current();

  // TODO: add another thread_suspend_lock_ to avoid GC/debugger races.

  if (verbose_) {
    LOG(INFO) << *self << " SuspendAll starting..." << (for_debugger ? " (debugger)" : "");
  }

  ThreadListLocker locker(this);
  Thread* debug_thread = Dbg::GetDebugThread();

  {
    // Increment everybody's suspend count (except our own).
    MutexLock mu(thread_suspend_count_lock_);
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      if (thread == self || (for_debugger && thread == debug_thread)) {
        continue;
      }
      if (verbose_) {
        LOG(INFO) << "requesting thread suspend: " << *thread;
      }
      ++thread->suspend_count_;
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
    if (verbose_) {
      LOG(INFO) << "thread suspended: " << *thread;
    }
  }

  if (verbose_) {
    LOG(INFO) << *self << " SuspendAll complete";
  }
}

void ThreadList::Suspend(Thread* thread) {
  DCHECK(thread != Thread::Current());

  // TODO: add another thread_suspend_lock_ to avoid GC/debugger races.

  if (verbose_) {
    LOG(INFO) << "Suspend(" << *thread << ") starting...";
  }

  ThreadListLocker locker(this);
  if (!Contains(thread)) {
    return;
  }

  {
    MutexLock mu(thread_suspend_count_lock_);
    ++thread->suspend_count_;
  }

  thread->WaitUntilSuspended();

  if (verbose_) {
    LOG(INFO) << "Suspend(" << *thread << ") complete";
  }
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
  ThreadListLocker locker(this);
  MutexLock mu(thread_suspend_count_lock_);
  ++self->suspend_count_;

  // Suspend ourselves.
  CHECK_GT(self->suspend_count_, 0);
  self->SetState(Thread::kSuspended);
  if (verbose_) {
    LOG(INFO) << *self << " self-suspending (dbg)";
  }

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
  self->SetState(Thread::kRunnable);
  if (verbose_) {
    LOG(INFO) << *self << " self-reviving (dbg)";
  }
}

void ThreadList::ResumeAll(bool for_debugger) {
  Thread* self = Thread::Current();

  if (verbose_) {
    LOG(INFO) << *self << " ResumeAll starting" << (for_debugger ? " (debugger)" : "");
  }

  // Decrement the suspend counts for all threads.  No need for atomic
  // writes, since nobody should be moving until we decrement the count.
  // We do need to hold the thread list because of JNI attaches.
  {
    ThreadListLocker locker(this);
    Thread* debug_thread = Dbg::GetDebugThread();
    MutexLock mu(thread_suspend_count_lock_);
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      if (thread == self || (for_debugger && thread == debug_thread)) {
        continue;
      }
      if (thread->suspend_count_ > 0) {
        --thread->suspend_count_;
      } else {
        LOG(WARNING) << *thread << " suspend count already zero";
      }
    }
  }

  // Broadcast a notification to all suspended threads, some or all of
  // which may choose to wake up.  No need to wait for them.
  {
    if (verbose_) {
      LOG(INFO) << *self << " ResumeAll waking others";
    }
    MutexLock mu(thread_suspend_count_lock_);
    thread_suspend_count_cond_.Broadcast();
  }

  if (verbose_) {
    LOG(INFO) << *self << " ResumeAll complete";
  }
}

void ThreadList::Resume(Thread* thread) {
  DCHECK(thread != Thread::Current());

  if (verbose_) {
    LOG(INFO) << "Resume(" << *thread << ") starting...";
  }

  {
    ThreadListLocker locker(this);
    MutexLock mu(thread_suspend_count_lock_);
    if (!Contains(thread)) {
      return;
    }
    if (thread->suspend_count_ > 0) {
      --thread->suspend_count_;
    } else {
      LOG(WARNING) << *thread << " suspend count already zero";
    }
  }

  {
    if (verbose_) {
      LOG(INFO) << "Resume(" << *thread << ") waking others";
    }
    MutexLock mu(thread_suspend_count_lock_);
    thread_suspend_count_cond_.Broadcast();
  }

  if (verbose_) {
    LOG(INFO) << "Resume(" << *thread << ") complete";
  }
}

void ThreadList::RunWhileSuspended(Thread* thread, void (*callback)(void*), void* arg) {
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

void ThreadList::Register() {
  Thread* self = Thread::Current();

  if (verbose_) {
    LOG(INFO) << "ThreadList::Register() " << *self;
    self->Dump(std::cerr);
  }

  ThreadListLocker locker(this);
  CHECK(!Contains(self));
  list_.push_back(self);
}

void ThreadList::Unregister() {
  Thread* self = Thread::Current();

  if (verbose_) {
    LOG(INFO) << "ThreadList::Unregister() " << *self;
  }

  if (self->GetPeer() != NULL) {
      self->SetState(Thread::kRunnable);

      // This may need to call user-supplied managed code. Make sure we do this before we start tearing
      // down the Thread* and removing it from the thread list (or start taking any locks).
      self->HandleUncaughtExceptions();

      // Make sure we remove from ThreadGroup before taking the
      // thread_list_lock_ since it allocates an Iterator which can cause
      // a GC which will want to suspend.
      self->RemoveFromThreadGroup();
  }

  ThreadListLocker locker(this);

  // Remove this thread from the list.
  CHECK(Contains(self));
  list_.remove(self);

  // Delete the Thread* and release the thin lock id.
  uint32_t thin_lock_id = self->thin_lock_id_;
  delete self;
  ReleaseThreadId(thin_lock_id);

  // Clear the TLS data, so that thread is recognizably detached.
  // (It may wish to reattach later.)
  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, NULL), "detach self");

  // Signal that a thread just detached.
  thread_exit_cond_.Signal();
}

void ThreadList::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  ThreadListLocker locker(this);
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
    ThreadListLocker locker(this);

    // We wait for the child to tell us that it's in the thread list.
    while (child->GetState() != Thread::kStarting) {
      thread_start_cond_.Wait(thread_list_lock_);
    }
  }

  // If we switch out of runnable and then back in, we know there's no pending suspend.
  self->SetState(Thread::kVmWait);
  self->SetState(Thread::kRunnable);

  // Tell the child that it's safe: it will see any future suspend request.
  child->SetState(Thread::kVmWait);
  thread_start_cond_.Broadcast();
}

void ThreadList::WaitForGo() {
  Thread* self = Thread::Current();
  DCHECK(Contains(self));

  {
    ThreadListLocker locker(this);

    // Tell our parent that we're in the thread list.
    self->SetState(Thread::kStarting);
    thread_start_cond_.Broadcast();

    // Wait until our parent tells us there's no suspend still pending
    // from before we were on the thread list.
    while (self->GetState() != Thread::kVmWait) {
      thread_start_cond_.Wait(thread_list_lock_);
    }
  }

  // Enter the runnable state. We know that any pending suspend will affect us now.
  self->SetState(Thread::kRunnable);
}

bool ThreadList::AllThreadsAreDaemons() {
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    // TODO: there's a race here with thread exit that's being worked around by checking if the peer
    // is null.
    if ((*it)->GetPeer() != NULL && !(*it)->IsDaemon()) {
      return false;
    }
  }
  return true;
}

void ThreadList::WaitForNonDaemonThreadsToExit() {
  ThreadListLocker locker(this);
  while (!AllThreadsAreDaemons()) {
    thread_exit_cond_.Wait(thread_list_lock_);
  }
}

void ThreadList::SuspendAllDaemonThreads() {
  ThreadListLocker locker(this);

  // Tell all the daemons it's time to suspend. (At this point, we know
  // all threads are daemons.)
  {
    MutexLock mu(thread_suspend_count_lock_);
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      ++thread->suspend_count_;
    }
  }

  // Give the threads a chance to suspend, complaining if they're slow.
  bool have_complained = false;
  for (int i = 0; i < 10; ++i) {
    usleep(200 * 1000);
    bool all_suspended = true;
    for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
      Thread* thread = *it;
      if (thread->GetState() == Thread::kRunnable) {
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
  ThreadListLocker locker(this);
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
  thread_list_lock_.AssertHeld();
  --id; // Zero is reserved to mean "invalid".
  DCHECK(allocated_ids_[id]) << id;
  allocated_ids_.reset(id);
}

}  // namespace art
