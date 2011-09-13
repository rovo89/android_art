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

namespace art {

pthread_cond_t ThreadList::thread_start_cond_ = PTHREAD_COND_INITIALIZER;

ThreadList::ThreadList() : lock_("ThreadList lock") {
}

ThreadList::~ThreadList() {
  if (Contains(Thread::Current())) {
    Runtime::Current()->DetachCurrentThread();
  }

  // All threads should have exited and unregistered when we
  // reach this point. This means that all daemon threads had been
  // shutdown cleanly.
  // TODO: dump ThreadList if non-empty.
  CHECK_EQ(list_.size(), 0U);
}

bool ThreadList::Contains(Thread* thread) {
  return find(list_.begin(), list_.end(), thread) != list_.end();
}

void ThreadList::Dump(std::ostream& os) {
  MutexLock mu(lock_);
  os << "DALVIK THREADS (" << list_.size() << "):\n";
  typedef std::list<Thread*>::const_iterator It; // TODO: C++0x auto
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    (*it)->Dump(os);
    os << "\n";
  }
}

void ThreadList::Register(Thread* thread) {
  //LOG(INFO) << "ThreadList::Register() " << *thread;
  MutexLock mu(lock_);
  CHECK(!Contains(thread));
  list_.push_back(thread);
}

void ThreadList::Unregister() {
  Thread* self = Thread::Current();

  //LOG(INFO) << "ThreadList::Unregister() " << *self;
  MutexLock mu(lock_);

  // Remove this thread from the list.
  CHECK(Contains(self));
  list_.remove(self);

  // Delete the Thread* and release the thin lock id.
  uint32_t thin_lock_id = self->thin_lock_id_;
  delete self;
  ReleaseThreadId(thin_lock_id);

  // Clear the TLS data, so that thread is recognizably detached.
  // (It may wish to reattach later.)
  errno = pthread_setspecific(Thread::pthread_key_self_, NULL);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_setspecific failed";
  }
}

void ThreadList::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  MutexLock mu(lock_);
  typedef std::list<Thread*>::const_iterator It; // TODO: C++0x auto
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
    MutexLock mu(lock_);

    // We wait for the child to tell us that it's in the thread list.
    while (child->GetState() != Thread::kStarting) {
      pthread_cond_wait(&thread_start_cond_, lock_.GetImpl());
    }
  }

  // If we switch out of runnable and then back in, we know there's no pending suspend.
  self->SetState(Thread::kVmWait);
  self->SetState(Thread::kRunnable);

  // Tell the child that it's safe: it will see any future suspend request.
  child->SetState(Thread::kVmWait);
  pthread_cond_broadcast(&thread_start_cond_);
}

void ThreadList::WaitForGo() {
  Thread* self = Thread::Current();
  DCHECK(Contains(self));

  MutexLock mu(lock_);

  // Tell our parent that we're in the thread list.
  self->SetState(Thread::kStarting);
  pthread_cond_broadcast(&thread_start_cond_);

  // Wait until our parent tells us there's no suspend still pending
  // from before we were on the thread list.
  while (self->GetState() != Thread::kVmWait) {
    pthread_cond_wait(&thread_start_cond_, lock_.GetImpl());
  }

  // Enter the runnable state. We know that any pending suspend will affect us now.
  self->SetState(Thread::kRunnable);
}

uint32_t ThreadList::AllocThreadId() {
  MutexLock mu(lock_);
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
  lock_.AssertHeld();
  --id; // Zero is reserved to mean "invalid".
  DCHECK(allocated_ids_[id]) << id;
  allocated_ids_.reset(id);
}

}  // namespace art
