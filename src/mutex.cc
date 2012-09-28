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

#include "mutex.h"

#include <errno.h>

#include "cutils/atomic.h"
#include "cutils/atomic-inline.h"
#include "logging.h"
#include "runtime.h"
#include "thread.h"
#include "utils.h"

#if defined(__APPLE__)
#include "AvailabilityMacros.h" // For MAC_OS_X_VERSION_MAX_ALLOWED
#endif

#define CHECK_MUTEX_CALL(call, args) CHECK_PTHREAD_CALL(call, args, name_)

extern int pthread_mutex_lock(pthread_mutex_t* mutex) EXCLUSIVE_LOCK_FUNCTION(mutex);
extern int pthread_mutex_unlock(pthread_mutex_t* mutex) UNLOCK_FUNCTION(1);
extern int pthread_mutex_trylock(pthread_mutex_t* mutex) EXCLUSIVE_TRYLOCK_FUNCTION(0, mutex);

#if ART_USE_FUTEXES
#include "linux/futex.h"
#include "sys/syscall.h"
#ifndef SYS_futex
#define SYS_futex __NR_futex
#endif
int futex(volatile int *uaddr, int op, int val, const struct timespec *timeout, int *, int ) {
  return syscall(SYS_futex, uaddr, op, val, timeout, NULL, NULL);
}
#endif  // ART_USE_FUTEXES

namespace art {

// This works on Mac OS 10.6 but hasn't been tested on older releases.
struct __attribute__((__may_alias__)) darwin_pthread_mutex_t {
  long padding0;
  int padding1;
  uint32_t padding2;
  int16_t padding3;
  int16_t padding4;
  uint32_t padding5;
  pthread_t darwin_pthread_mutex_owner;
  // ...other stuff we don't care about.
};

struct __attribute__((__may_alias__)) darwin_pthread_rwlock_t {
  long padding0;
  pthread_mutex_t padding1;
  int padding2;
  pthread_cond_t padding3;
  pthread_cond_t padding4;
  int padding5;
  int padding6;
  pthread_t darwin_pthread_rwlock_owner;
  // ...other stuff we don't care about.
};

struct __attribute__((__may_alias__)) glibc_pthread_mutex_t {
  int32_t padding0[2];
  int owner;
  // ...other stuff we don't care about.
};

struct __attribute__((__may_alias__)) glibc_pthread_rwlock_t {
#ifdef __LP64__
  int32_t padding0[6];
#else
  int32_t padding0[7];
#endif
  int writer;
  // ...other stuff we don't care about.
};

static uint64_t SafeGetTid(const Thread* self) {
  if (self != NULL) {
    return static_cast<uint64_t>(self->GetTid());
  } else {
    return static_cast<uint64_t>(GetTid());
  }
}

BaseMutex::BaseMutex(const char* name, LockLevel level) : level_(level), name_(name) {}

static void CheckUnattachedThread(LockLevel level) {
  // The check below enumerates the cases where we expect not to be able to sanity check locks
  // on a thread. TODO: tighten this check.
  if (kDebugLocking) {
    Runtime* runtime = Runtime::Current();
    CHECK(runtime == NULL || !runtime->IsStarted() || runtime->IsShuttingDown() ||
          level == kDefaultMutexLevel  || level == kThreadListLock ||
          level == kLoggingLock || level == kAbortLock);
  }
}

void BaseMutex::RegisterAsLocked(Thread* self) {
  if (UNLIKELY(self == NULL)) {
    CheckUnattachedThread(level_);
    return;
  }
  if (kDebugLocking) {
    // Check if a bad Mutex of this level or lower is held.
    bool bad_mutexes_held = false;
    for (int i = level_; i >= 0; --i) {
      BaseMutex* held_mutex = self->GetHeldMutex(static_cast<LockLevel>(i));
      if (UNLIKELY(held_mutex != NULL)) {
        LOG(ERROR) << "Lock level violation: holding \"" << held_mutex->name_ << "\" (level " << i
            << ") while locking \"" << name_ << "\" (level " << static_cast<int>(level_) << ")";
        if (i > kAbortLock) {
          // Only abort in the check below if this is more than abort level lock.
          bad_mutexes_held = true;
        }
      }
    }
    CHECK(!bad_mutexes_held);
  }
  // Don't record monitors as they are outside the scope of analysis. They may be inspected off of
  // the monitor list.
  if (level_ != kMonitorLock) {
    self->SetHeldMutex(level_, this);
  }
}

void BaseMutex::RegisterAsUnlocked(Thread* self) {
  if (UNLIKELY(self == NULL)) {
    CheckUnattachedThread(level_);
    return;
  }
  if (level_ != kMonitorLock) {
    if (kDebugLocking) {
      CHECK(self->GetHeldMutex(level_) == this) << "Unlocking on unacquired mutex: " << name_;
    }
    self->SetHeldMutex(level_, NULL);
  }
}

void BaseMutex::CheckSafeToWait(Thread* self) {
  if (self == NULL) {
    CheckUnattachedThread(level_);
    return;
  }
  if (kDebugLocking) {
    CHECK(self->GetHeldMutex(level_) == this) << "Waiting on unacquired mutex: " << name_;
    bool bad_mutexes_held = false;
    for (int i = kMaxMutexLevel; i >= 0; --i) {
      if (i != level_) {
        BaseMutex* held_mutex = self->GetHeldMutex(static_cast<LockLevel>(i));
        if (held_mutex != NULL) {
          LOG(ERROR) << "Holding " << held_mutex->name_ << " (level " << i
              << ") while performing wait on: "
              << name_ << " (level " << static_cast<int>(level_) << ")";
          bad_mutexes_held = true;
        }
      }
    }
    CHECK(!bad_mutexes_held);
  }
}

Mutex::Mutex(const char* name, LockLevel level, bool recursive)
    : BaseMutex(name, level), recursive_(recursive), recursion_count_(0) {
#if defined(__BIONIC__) || defined(__APPLE__)
  // Use recursive mutexes for bionic and Apple otherwise the
  // non-recursive mutexes don't have TIDs to check lock ownership of.
  pthread_mutexattr_t attributes;
  CHECK_MUTEX_CALL(pthread_mutexattr_init, (&attributes));
  CHECK_MUTEX_CALL(pthread_mutexattr_settype, (&attributes, PTHREAD_MUTEX_RECURSIVE));
  CHECK_MUTEX_CALL(pthread_mutex_init, (&mutex_, &attributes));
  CHECK_MUTEX_CALL(pthread_mutexattr_destroy, (&attributes));
#else
  CHECK_MUTEX_CALL(pthread_mutex_init, (&mutex_, NULL));
#endif
}

Mutex::~Mutex() {
  // We can't use CHECK_MUTEX_CALL here because on shutdown a suspended daemon thread
  // may still be using locks.
  int rc = pthread_mutex_destroy(&mutex_);
  if (rc != 0) {
    errno = rc;
    // TODO: should we just not log at all if shutting down? this could be the logging mutex!
    bool shutting_down = Runtime::Current()->IsShuttingDown();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_mutex_destroy failed for " << name_;
  }
}

void Mutex::ExclusiveLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  if (kDebugLocking && !recursive_) {
    AssertNotHeld(self);
  }
  if (!recursive_ || !IsExclusiveHeld(self)) {
    CHECK_MUTEX_CALL(pthread_mutex_lock, (&mutex_));
    RegisterAsLocked(self);
  }
  recursion_count_++;
  if (kDebugLocking) {
    CHECK(recursion_count_ == 1 || recursive_) << "Unexpected recursion count on mutex: "
        << name_ << " " << recursion_count_;
    AssertHeld(self);
  }
}

bool Mutex::ExclusiveTryLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  if (kDebugLocking && !recursive_) {
    AssertNotHeld(self);
  }
  if (!recursive_ || !IsExclusiveHeld(self)) {
    int result = pthread_mutex_trylock(&mutex_);
    if (result == EBUSY) {
      return false;
    }
    if (result != 0) {
      errno = result;
      PLOG(FATAL) << "pthread_mutex_trylock failed for " << name_;
    }
    RegisterAsLocked(self);
  }
  recursion_count_++;
  if (kDebugLocking) {
    CHECK(recursion_count_ == 1 || recursive_) << "Unexpected recursion count on mutex: "
        << name_ << " " << recursion_count_;
    AssertHeld(self);
  }
  return true;
}

void Mutex::ExclusiveUnlock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  AssertHeld(self);
  recursion_count_--;
  if (!recursive_ || recursion_count_ == 0) {
    if (kDebugLocking) {
      CHECK(recursion_count_ == 0 || recursive_) << "Unexpected recursion count on mutex: "
          << name_ << " " << recursion_count_;
    }
    RegisterAsUnlocked(self);
    CHECK_MUTEX_CALL(pthread_mutex_unlock, (&mutex_));
  }
}

bool Mutex::IsExclusiveHeld(const Thread* self) const {
  DCHECK(self == NULL || self == Thread::Current());
  bool result = (GetExclusiveOwnerTid() == SafeGetTid(self));
  if (kDebugLocking) {
    // Sanity debug check that if we think it is locked we have it in our held mutexes.
    if (result && self != NULL && level_ != kMonitorLock) {
      CHECK_EQ(self->GetHeldMutex(level_), this);
    }
  }
  return result;
}

uint64_t Mutex::GetExclusiveOwnerTid() const {
#if defined(__BIONIC__)
  return static_cast<uint64_t>((mutex_.value >> 16) & 0xffff);
#elif defined(__GLIBC__)
  return reinterpret_cast<const glibc_pthread_mutex_t*>(&mutex_)->owner;
#elif defined(__APPLE__)
  const darwin_pthread_mutex_t* dpmutex = reinterpret_cast<const darwin_pthread_mutex_t*>(&mutex_);
  pthread_t owner = dpmutex->darwin_pthread_mutex_owner;
  // 0 for unowned, -1 for PTHREAD_MTX_TID_SWITCHING
  // TODO: should we make darwin_pthread_mutex_owner volatile and recheck until not -1?
  if ((owner == (pthread_t)0) || (owner == (pthread_t)-1)) {
    return 0;
  }
  uint64_t tid;
  CHECK_PTHREAD_CALL(pthread_threadid_np, (owner, &tid), __FUNCTION__);  // Requires Mac OS 10.6
  return tid;
#else
#error unsupported C library
#endif
}

std::string Mutex::Dump() const {
  return StringPrintf("%s %s level=%d count=%d owner=%llx",
                      recursive_ ? "recursive" : "non-recursive",
                      name_.c_str(),
                      static_cast<int>(level_),
                      recursion_count_,
                      GetExclusiveOwnerTid());
}

std::ostream& operator<<(std::ostream& os, const Mutex& mu) {
  return os << mu.Dump();
}

ReaderWriterMutex::ReaderWriterMutex(const char* name, LockLevel level) :
    BaseMutex(name, level)
#if ART_USE_FUTEXES
    , state_(0), exclusive_owner_(0), num_pending_readers_(0), num_pending_writers_(0)
#endif
{
#if !ART_USE_FUTEXES
  CHECK_MUTEX_CALL(pthread_rwlock_init, (&rwlock_, NULL));
#endif
}

ReaderWriterMutex::~ReaderWriterMutex() {
#if ART_USE_FUTEXES
  CHECK_EQ(state_, 0);
  CHECK_EQ(exclusive_owner_, 0U);
  CHECK_EQ(num_pending_readers_, 0);
  CHECK_EQ(num_pending_writers_, 0);
#else
  // We can't use CHECK_MUTEX_CALL here because on shutdown a suspended daemon thread
  // may still be using locks.
  int rc = pthread_rwlock_destroy(&rwlock_);
  if (rc != 0) {
    errno = rc;
    // TODO: should we just not log at all if shutting down? this could be the logging mutex!
    bool shutting_down = Runtime::Current()->IsShuttingDown();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_mutex_destroy failed for " << name_;
  }
#endif
}

void ReaderWriterMutex::ExclusiveLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  AssertNotExclusiveHeld(self);
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (cur_state == 0) {
      // Change state from 0 to -1.
      done = android_atomic_cmpxchg(0, -1, &state_) == 0;
    } else {
      // Failed to acquire, hang up.
      android_atomic_inc(&num_pending_writers_);
      if (futex(&state_, FUTEX_WAIT, cur_state, NULL, NULL, 0) != 0) {
        if (errno != EAGAIN) {
          PLOG(FATAL) << "futex wait failed for " << name_;
        }
      }
      android_atomic_dec(&num_pending_writers_);
    }
  } while(!done);
  exclusive_owner_ = static_cast<uint64_t>(self->GetTid());
#else
  CHECK_MUTEX_CALL(pthread_rwlock_wrlock, (&rwlock_));
#endif
  RegisterAsLocked(self);
  AssertExclusiveHeld(self);
}

void ReaderWriterMutex::ExclusiveUnlock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  AssertExclusiveHeld(self);
  RegisterAsUnlocked(self);
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (cur_state == -1) {
      // We're no longer the owner.
      exclusive_owner_ = 0;
      // Change state from -1 to 0.
      done = android_atomic_cmpxchg(-1, 0, &state_) == 0;
      if (done) { // cmpxchg may fail due to noise?
        // Wake any waiters.
        if (num_pending_readers_ > 0 || num_pending_writers_ > 0) {
          futex(&state_, FUTEX_WAKE, -1, NULL, NULL, 0);
        }
      }
    } else {
      LOG(FATAL) << "Unexpected state_:" << cur_state << " for " << name_;
    }
  } while(!done);
#else
  CHECK_MUTEX_CALL(pthread_rwlock_unlock, (&rwlock_));
#endif
}

#if HAVE_TIMED_RWLOCK
bool ReaderWriterMutex::ExclusiveLockWithTimeout(Thread* self, const timespec& abs_timeout) {
  DCHECK(self == NULL || self == Thread::Current());
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (cur_state == 0) {
      // Change state from 0 to -1.
      done = android_atomic_cmpxchg(0, -1, &state_) == 0;
    } else {
      // Failed to acquire, hang up.
      android_atomic_inc(&num_pending_writers_);
      if (futex(&state_, FUTEX_WAIT, cur_state, &abs_timeout, NULL, 0) != 0) {
        if (errno == ETIMEDOUT) {
          android_atomic_dec(&num_pending_writers_);
          return false;
        } else if (errno != EAGAIN) {
          PLOG(FATAL) << "timed futex wait failed for " << name_;
        }
      }
      android_atomic_dec(&num_pending_writers_);
    }
  } while(!done);
  exclusive_owner_ = SafeGetTid(self);
#else
  int result = pthread_rwlock_timedwrlock(&rwlock_, &abs_timeout);
  if (result == ETIMEDOUT) {
    return false;
  }
  if (result != 0) {
    errno = result;
    PLOG(FATAL) << "pthread_rwlock_timedwrlock failed for " << name_;
  }
#endif
  RegisterAsLocked(self);
  AssertSharedHeld(self);
  return true;
}
#endif

void ReaderWriterMutex::SharedLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (cur_state >= 0) {
      // Add as an extra reader.
      done = android_atomic_cmpxchg(cur_state, cur_state + 1, &state_) == 0;
    } else {
      // Owner holds it exclusively, hang up.
      android_atomic_inc(&num_pending_readers_);
      if (futex(&state_, FUTEX_WAIT, cur_state, NULL, NULL, 0) != 0) {
        if (errno != EAGAIN) {
          PLOG(FATAL) << "futex wait failed for " << name_;
        }
      }
      android_atomic_dec(&num_pending_readers_);
    }
  } while(!done);
#else
  CHECK_MUTEX_CALL(pthread_rwlock_rdlock, (&rwlock_));
#endif
  RegisterAsLocked(self);
  AssertSharedHeld(self);
}

bool ReaderWriterMutex::SharedTryLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (cur_state >= 0) {
      // Add as an extra reader.
      done = android_atomic_cmpxchg(cur_state, cur_state + 1, &state_) == 0;
    } else {
      // Owner holds it exclusively.
      return false;
    }
  } while(!done);
#else
  int result = pthread_rwlock_tryrdlock(&rwlock_);
  if (result == EBUSY) {
    return false;
  }
  if (result != 0) {
    errno = result;
    PLOG(FATAL) << "pthread_mutex_trylock failed for " << name_;
  }
#endif
  RegisterAsLocked(self);
  AssertSharedHeld(self);
  return true;
}

void ReaderWriterMutex::SharedUnlock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  AssertSharedHeld(self);
  RegisterAsUnlocked(self);
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (LIKELY(cur_state > 0)) {
      // Reduce state by 1.
      done = android_atomic_cmpxchg(cur_state, cur_state - 1, &state_) == 0;
      if (done && (cur_state - 1) == 0) { // cmpxchg may fail due to noise?
        if (num_pending_writers_ > 0 || num_pending_readers_ > 0) {
          // Wake any exclusive waiters as there are now no readers.
          futex(&state_, FUTEX_WAKE, -1, NULL, NULL, 0);
        }
      }
    } else {
      LOG(FATAL) << "Unexpected state_:" << cur_state << " for " << name_;
    }
  } while(!done);
#else
  CHECK_MUTEX_CALL(pthread_rwlock_unlock, (&rwlock_));
#endif
}

bool ReaderWriterMutex::IsExclusiveHeld(const Thread* self) const {
  DCHECK(self == NULL || self == Thread::Current());
  bool result = (GetExclusiveOwnerTid() == SafeGetTid(self));
  if (kDebugLocking) {
    // Sanity that if the pthread thinks we own the lock the Thread agrees.
    if (self != NULL && result)  {
      CHECK_EQ(self->GetHeldMutex(level_), this);
    }
  }
  return result;
}

bool ReaderWriterMutex::IsSharedHeld(const Thread* self) const {
  DCHECK(self == NULL || self == Thread::Current());
  bool result;
  if (UNLIKELY(self == NULL)) {  // Handle unattached threads.
    result = IsExclusiveHeld(self);  // TODO: a better best effort here.
  } else {
    result = (self->GetHeldMutex(level_) == this);
  }
  return result;
}

uint64_t ReaderWriterMutex::GetExclusiveOwnerTid() const {
#if ART_USE_FUTEXES
  return exclusive_owner_;
#else
#if defined(__BIONIC__)
  return rwlock_.writerThreadId;
#elif defined(__GLIBC__)
  return reinterpret_cast<const glibc_pthread_rwlock_t*>(&rwlock_)->writer;
#elif defined(__APPLE__)
  const darwin_pthread_rwlock_t*
      dprwlock = reinterpret_cast<const darwin_pthread_rwlock_t*>(&rwlock_);
  pthread_t owner = dprwlock->darwin_pthread_rwlock_owner;
  if (owner == (pthread_t)0) {
    return 0;
  }
  uint64_t tid;
  CHECK_PTHREAD_CALL(pthread_threadid_np, (owner, &tid), __FUNCTION__);  // Requires Mac OS 10.6
  return tid;
#else
#error unsupported C library
#endif
#endif
}

std::string ReaderWriterMutex::Dump() const {
  return StringPrintf("%s level=%d owner=%llx",
                      name_.c_str(),
                      static_cast<int>(level_),
                      GetExclusiveOwnerTid());
}

std::ostream& operator<<(std::ostream& os, const ReaderWriterMutex& mu) {
  return os << mu.Dump();
}

ConditionVariable::ConditionVariable(const std::string& name) : name_(name) {
  CHECK_MUTEX_CALL(pthread_cond_init, (&cond_, NULL));
}

ConditionVariable::~ConditionVariable() {
  // We can't use CHECK_MUTEX_CALL here because on shutdown a suspended daemon thread
  // may still be using condition variables.
  int rc = pthread_cond_destroy(&cond_);
  if (rc != 0) {
    errno = rc;
    bool shutting_down = Runtime::Current()->IsShuttingDown();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_cond_destroy failed for " << name_;
  }
}

void ConditionVariable::Broadcast() {
  CHECK_MUTEX_CALL(pthread_cond_broadcast, (&cond_));
}

void ConditionVariable::Signal() {
  CHECK_MUTEX_CALL(pthread_cond_signal, (&cond_));
}

void ConditionVariable::Wait(Thread* self, Mutex& mutex) {
  mutex.CheckSafeToWait(self);
  unsigned int old_recursion_count = mutex.recursion_count_;
  mutex.recursion_count_ = 0;
  CHECK_MUTEX_CALL(pthread_cond_wait, (&cond_, &mutex.mutex_));
  mutex.recursion_count_ = old_recursion_count;
}

void ConditionVariable::TimedWait(Thread* self, Mutex& mutex, const timespec& ts) {
#ifdef HAVE_TIMEDWAIT_MONOTONIC
#define TIMEDWAIT pthread_cond_timedwait_monotonic
#else
#define TIMEDWAIT pthread_cond_timedwait
#endif
  mutex.CheckSafeToWait(self);
  unsigned int old_recursion_count = mutex.recursion_count_;
  mutex.recursion_count_ = 0;
  int rc = TIMEDWAIT(&cond_, &mutex.mutex_, &ts);
  mutex.recursion_count_ = old_recursion_count;
  if (rc != 0 && rc != ETIMEDOUT) {
    errno = rc;
    PLOG(FATAL) << "TimedWait failed for " << name_;
  }
}

}  // namespace art
