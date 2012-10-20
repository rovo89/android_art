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
#include <sys/time.h>

#include "cutils/atomic.h"
#include "cutils/atomic-inline.h"
#include "logging.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "utils.h"

#if defined(__APPLE__)
#include "AvailabilityMacros.h" // For MAC_OS_X_VERSION_MAX_ALLOWED
// No clocks to specify on OS/X, fake value to pass to routines that require a clock.
#define CLOCK_REALTIME 0xEBADFOOD
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
int futex(volatile int *uaddr, int op, int val, const struct timespec *timeout, volatile int *uaddr2, int val3) {
  return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
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

// Initialize a timespec to either an absolute or relative time.
static void InitTimeSpec(Thread* self, bool absolute, int clock, int64_t ms, int32_t ns,
                         timespec* ts) {
  int64_t endSec;

  if (absolute) {
#if !defined(__APPLE__)
    clock_gettime(clock, ts);
#else
    UNUSED(clock);
    timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000;
#endif
  } else {
    ts->tv_sec = 0;
    ts->tv_nsec = 0;
  }
  endSec = ts->tv_sec + ms / 1000;
  if (UNLIKELY(endSec >= 0x7fffffff)) {
    std::ostringstream ss;
    ScopedObjectAccess soa(self);
    self->Dump(ss);
    LOG(INFO) << "Note: end time exceeds epoch: " << ss.str();
    endSec = 0x7ffffffe;
  }
  ts->tv_sec = endSec;
  ts->tv_nsec = (ts->tv_nsec + (ms % 1000) * 1000000) + ns;

  // Catch rollover.
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

#if ART_USE_FUTEXES
static bool ComputeRelativeTimeSpec(timespec* result_ts, const timespec& lhs, const timespec& rhs) {
  const long int one_sec = 1000 * 1000 * 1000;  // one second in nanoseconds.
  result_ts->tv_sec = lhs.tv_sec - rhs.tv_sec;
  result_ts->tv_nsec = lhs.tv_nsec - rhs.tv_nsec;
  if (result_ts->tv_nsec < 0) {
    result_ts->tv_sec--;
    result_ts->tv_nsec += one_sec;
  } else if (result_ts->tv_nsec > one_sec) {
    result_ts->tv_sec++;
    result_ts->tv_nsec -= one_sec;
  }
  return result_ts->tv_sec < 0;
}
#endif

BaseMutex::BaseMutex(const char* name, LockLevel level) : level_(level), name_(name) {}

static void CheckUnattachedThread(LockLevel level) NO_THREAD_SAFETY_ANALYSIS {
  // The check below enumerates the cases where we expect not to be able to sanity check locks
  // on a thread. Lock checking is disabled to avoid deadlock when checking shutdown lock.
  // TODO: tighten this check.
  if (kDebugLocking) {
    Runtime* runtime = Runtime::Current();
    CHECK(runtime == NULL || !runtime->IsStarted() || runtime->IsShuttingDown() ||
          level == kDefaultMutexLevel  || level == kRuntimeShutdownLock ||
          level == kThreadListLock || level == kLoggingLock || level == kAbortLock);
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
#if ART_USE_FUTEXES
  state_ = 0;
  exclusive_owner_ = 0;
  num_contenders_ = 0;
#elif defined(__BIONIC__) || defined(__APPLE__)
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
#if ART_USE_FUTEXES
  if (state_ != 0) {
    MutexLock mu(Thread::Current(), *Locks::runtime_shutdown_lock_);
    Runtime* runtime = Runtime::Current();
    bool shutting_down = (runtime == NULL) || runtime->IsShuttingDown();
    LOG(shutting_down ? WARNING : FATAL) << "destroying mutex with owner: " << exclusive_owner_;
  } else {
    CHECK_EQ(exclusive_owner_, 0U)  << "unexpectedly found an owner on unlocked mutex " << name_;
    CHECK_EQ(num_contenders_, 0) << "unexpectedly found a contender on mutex " << name_;
  }
#else
  // We can't use CHECK_MUTEX_CALL here because on shutdown a suspended daemon thread
  // may still be using locks.
  int rc = pthread_mutex_destroy(&mutex_);
  if (rc != 0) {
    errno = rc;
    // TODO: should we just not log at all if shutting down? this could be the logging mutex!
    MutexLock mu(Thread::Current(), *Locks::runtime_shutdown_lock_);
    Runtime* runtime = Runtime::Current();
    bool shutting_down = (runtime == NULL) || runtime->IsShuttingDown();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_mutex_destroy failed for " << name_;
  }
#endif
}

void Mutex::ExclusiveLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  if (kDebugLocking && !recursive_) {
    AssertNotHeld(self);
  }
  if (!recursive_ || !IsExclusiveHeld(self)) {
#if ART_USE_FUTEXES
    bool done = false;
    do {
      int32_t cur_state = state_;
      if (cur_state == 0) {
        // Change state from 0 to 1.
        done = android_atomic_cmpxchg(0, 1, &state_) == 0;
      } else {
        // Failed to acquire, hang up.
        android_atomic_inc(&num_contenders_);
        if (futex(&state_, FUTEX_WAIT, 1, NULL, NULL, 0) != 0) {
          if (errno != EAGAIN) {
            PLOG(FATAL) << "futex wait failed for " << name_;
          }
        }
        android_atomic_dec(&num_contenders_);
      }
    } while(!done);
    DCHECK_EQ(state_, 1);
    exclusive_owner_ = SafeGetTid(self);
#else
    CHECK_MUTEX_CALL(pthread_mutex_lock, (&mutex_));
#endif
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
#if ART_USE_FUTEXES
    bool done = false;
    do {
      int32_t cur_state = state_;
      if (cur_state == 0) {
        // Change state from 0 to 1.
        done = android_atomic_cmpxchg(0, 1, &state_) == 0;
      } else {
        return false;
      }
    } while(!done);
    DCHECK_EQ(state_, 1);
    exclusive_owner_ = SafeGetTid(self);
#else
    int result = pthread_mutex_trylock(&mutex_);
    if (result == EBUSY) {
      return false;
    }
    if (result != 0) {
      errno = result;
      PLOG(FATAL) << "pthread_mutex_trylock failed for " << name_;
    }
#endif
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
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (cur_state == 1) {
      // We're no longer the owner.
      exclusive_owner_ = 0;
      // Change state to 0.
      done = android_atomic_cmpxchg(cur_state, 0, &state_) == 0;
      if (done) { // Spurious fail?
        // Wake a contender
        if (num_contenders_ > 0) {
          futex(&state_, FUTEX_WAKE, 1, NULL, NULL, 0);
        }
      }
    } else {
      LOG(FATAL) << "Unexpected state_:" << cur_state << " for " << name_;
    }
  } while(!done);
#else
    CHECK_MUTEX_CALL(pthread_mutex_unlock, (&mutex_));
#endif
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
#if ART_USE_FUTEXES
  return exclusive_owner_;
#elif defined(__BIONIC__)
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
    MutexLock mu(Thread::Current(), *Locks::runtime_shutdown_lock_);
    Runtime* runtime = Runtime::Current();
    bool shutting_down = runtime == NULL || runtime->IsShuttingDown();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_rwlock_destroy failed for " << name_;
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
  DCHECK_EQ(state_, -1);
  exclusive_owner_ = SafeGetTid(self);
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
bool ReaderWriterMutex::ExclusiveLockWithTimeout(Thread* self, int64_t ms, int32_t ns) {
  DCHECK(self == NULL || self == Thread::Current());
#if ART_USE_FUTEXES
  bool done = false;
  timespec end_abs_ts;
  InitTimeSpec(self, true, CLOCK_REALTIME, ms, ns, &end_abs_ts);
  do {
    int32_t cur_state = state_;
    if (cur_state == 0) {
      // Change state from 0 to -1.
      done = android_atomic_cmpxchg(0, -1, &state_) == 0;
    } else {
      // Failed to acquire, hang up.
      timespec now_abs_ts;
      InitTimeSpec(self, true, CLOCK_REALTIME, 0, 0, &now_abs_ts);
      timespec rel_ts;
      if (ComputeRelativeTimeSpec(&rel_ts, end_abs_ts, now_abs_ts)) {
        return false;  // Timed out.
      }
      android_atomic_inc(&num_pending_writers_);
      if (futex(&state_, FUTEX_WAIT, cur_state, &rel_ts, NULL, 0) != 0) {
        if (errno == ETIMEDOUT) {
          android_atomic_dec(&num_pending_writers_);
          return false;  // Timed out.
        } else if (errno != EAGAIN && errno != EINTR) {
          PLOG(FATAL) << "timed futex wait failed for " << name_;
        }
      }
      android_atomic_dec(&num_pending_writers_);
    }
  } while(!done);
  exclusive_owner_ = SafeGetTid(self);
#else
  timespec ts;
  InitTimeSpec(self, true, CLOCK_REALTIME, ms, ns, &ts);
  int result = pthread_rwlock_timedwrlock(&rwlock_, &ts);
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

ConditionVariable::ConditionVariable(const std::string& name, Mutex& guard)
    : name_(name), guard_(guard) {
#if ART_USE_FUTEXES
  state_ = 0;
  num_waiters_ = 0;
  num_awoken_ = 0;
#else
  CHECK_MUTEX_CALL(pthread_cond_init, (&cond_, NULL));
#endif
}

ConditionVariable::~ConditionVariable() {
#if !ART_USE_FUTEXES
  // We can't use CHECK_MUTEX_CALL here because on shutdown a suspended daemon thread
  // may still be using condition variables.
  int rc = pthread_cond_destroy(&cond_);
  if (rc != 0) {
    errno = rc;
    MutexLock mu(Thread::Current(), *Locks::runtime_shutdown_lock_);
    Runtime* runtime = Runtime::Current();
    bool shutting_down = (runtime == NULL) || runtime->IsShuttingDown();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_cond_destroy failed for " << name_;
  }
#endif
}

void ConditionVariable::Broadcast(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  // TODO: enable below, there's a race in thread creation that causes false failures currently.
  // guard_.AssertExclusiveHeld(self);
#if ART_USE_FUTEXES
  if (num_waiters_ > 0) {
    android_atomic_inc(&state_); // Indicate a wake has occurred to waiters coming in.
    bool done = false;
    do {
       int32_t cur_state = state_;
       // Compute number of waiters requeued and add to mutex contenders.
       int32_t num_requeued = num_waiters_ - num_awoken_;
       android_atomic_add(num_requeued, &guard_.num_contenders_);
       // Requeue waiters onto contenders.
       done = futex(&state_, FUTEX_CMP_REQUEUE, 0,
                    reinterpret_cast<const timespec*>(std::numeric_limits<int32_t>::max()),
                    &guard_.state_, cur_state) != -1;
       if (!done) {
         if (errno != EAGAIN) {
           PLOG(FATAL) << "futex cmp requeue failed for " << name_;
         }
       } else {
         num_awoken_ = num_waiters_;
       }
    } while (!done);
  }
#else
  CHECK_MUTEX_CALL(pthread_cond_broadcast, (&cond_));
#endif
}

void ConditionVariable::Signal(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  guard_.AssertExclusiveHeld(self);
#if ART_USE_FUTEXES
  if (num_waiters_ > 0) {
    android_atomic_inc(&state_); // Indicate a wake has occurred to waiters coming in.
    // Futex wake 1 waiter who will then come and in contend on mutex. It'd be nice to requeue them
    // to avoid this, however, requeueing can only move all waiters.
    if (futex(&state_, FUTEX_WAKE, 1, NULL, NULL, 0) == 1) {
      // Wake success.
      // We weren't requeued, however, to make accounting simpler in the Wait code, increment the
      // number of contenders on the mutex.
      num_awoken_++;
      android_atomic_inc(&guard_.num_contenders_);
    }
  }
#else
  CHECK_MUTEX_CALL(pthread_cond_signal, (&cond_));
#endif
}

void ConditionVariable::Wait(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  guard_.AssertExclusiveHeld(self);
  guard_.CheckSafeToWait(self);
  unsigned int old_recursion_count = guard_.recursion_count_;
#if ART_USE_FUTEXES
  int32_t cur_state = state_;
  num_waiters_++;
  guard_.recursion_count_ = 1;
  guard_.ExclusiveUnlock(self);
  bool woken = true;
  while (futex(&state_, FUTEX_WAIT, cur_state, NULL, NULL, 0) != 0) {
    if (errno == EINTR || errno == EAGAIN) {
      if (state_ != cur_state) {
        // We failed and a signal has come in.
        woken = false;
        break;
      }
    } else {
      PLOG(FATAL) << "futex wait failed for " << name_;
    }
  }
  guard_.ExclusiveLock(self);
  num_waiters_--;
  if (woken) {
    // If we were woken we were requeued on the mutex, decrement the mutex's contender count.
    android_atomic_dec(&guard_.num_contenders_);
    num_awoken_--;
  }
#else
  guard_.recursion_count_ = 0;
  CHECK_MUTEX_CALL(pthread_cond_wait, (&cond_, &guard_.mutex_));
#endif
  guard_.recursion_count_ = old_recursion_count;
}

void ConditionVariable::TimedWait(Thread* self, int64_t ms, int32_t ns) {
  DCHECK(self == NULL || self == Thread::Current());
  guard_.AssertExclusiveHeld(self);
  guard_.CheckSafeToWait(self);
  unsigned int old_recursion_count = guard_.recursion_count_;
#if ART_USE_FUTEXES
  // Record the original end time so that if the futex call fails we can recompute the appropriate
  // relative time.
  timespec end_abs_ts;
  InitTimeSpec(self, true, CLOCK_REALTIME, ms, ns, &end_abs_ts);
  timespec rel_ts;
  InitTimeSpec(self, false, CLOCK_REALTIME, ms, ns, &rel_ts);
  // Read state so that we can know if a signal comes in during before we sleep.
  int32_t cur_state = state_;
  num_waiters_++;
  guard_.recursion_count_ = 1;
  guard_.ExclusiveUnlock(self);
  bool woken = true;  // Did the futex wait end because we were awoken?
  while (futex(&state_, FUTEX_WAIT, cur_state, &rel_ts, NULL, 0) != 0) {
    if (errno == ETIMEDOUT) {
      woken = false;
      break;
    }
    if ((errno == EINTR) || (errno == EAGAIN)) {
      if (state_ != cur_state) {
        // We failed and a signal has come in.
        woken = false;
        break;
      }
      timespec now_abs_ts;
      InitTimeSpec(self, true, CLOCK_REALTIME, 0, 0, &now_abs_ts);
      if (ComputeRelativeTimeSpec(&rel_ts, end_abs_ts, now_abs_ts)) {
        // futex failed and we timed out in the meantime.
        woken = false;
        break;
      }
    } else {
      PLOG(FATAL) << "timed futex wait failed for " << name_;
    }
  }
  guard_.ExclusiveLock(self);
  num_waiters_--;
  if (woken) {
    // If we were woken we were requeued on the mutex, decrement the mutex's contender count.
    android_atomic_dec(&guard_.num_contenders_);
    num_awoken_--;
  }
#else
#ifdef HAVE_TIMEDWAIT_MONOTONIC
#define TIMEDWAIT pthread_cond_timedwait_monotonic
  int clock = CLOCK_MONOTONIC;
#else
#define TIMEDWAIT pthread_cond_timedwait
  int clock = CLOCK_REALTIME;
#endif
  guard_.recursion_count_ = 0;
  timespec ts;
  InitTimeSpec(self, true, clock, ms, ns, &ts);
  int rc = TIMEDWAIT(&cond_, &guard_.mutex_, &ts);
  if (rc != 0 && rc != ETIMEDOUT) {
    errno = rc;
    PLOG(FATAL) << "TimedWait failed for " << name_;
  }
#endif
  guard_.recursion_count_ = old_recursion_count;
}

}  // namespace art
