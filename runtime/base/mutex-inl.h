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

#ifndef ART_RUNTIME_BASE_MUTEX_INL_H_
#define ART_RUNTIME_BASE_MUTEX_INL_H_

#include <inttypes.h>

#include "mutex.h"

#define ATRACE_TAG ATRACE_TAG_DALVIK

#include "cutils/atomic-inline.h"
#include "cutils/trace.h"
#include "runtime.h"
#include "thread.h"

namespace art {

#define CHECK_MUTEX_CALL(call, args) CHECK_PTHREAD_CALL(call, args, name_)

#if ART_USE_FUTEXES
#include "linux/futex.h"
#include "sys/syscall.h"
#ifndef SYS_futex
#define SYS_futex __NR_futex
#endif
static inline int futex(volatile int *uaddr, int op, int val, const struct timespec *timeout, volatile int *uaddr2, int val3) {
  return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
}
#endif  // ART_USE_FUTEXES

#if defined(__APPLE__)

// This works on Mac OS 10.6 but hasn't been tested on older releases.
struct __attribute__((__may_alias__)) darwin_pthread_mutex_t {
  long padding0;  // NOLINT(runtime/int) exact match to darwin type
  int padding1;
  uint32_t padding2;
  int16_t padding3;
  int16_t padding4;
  uint32_t padding5;
  pthread_t darwin_pthread_mutex_owner;
  // ...other stuff we don't care about.
};

struct __attribute__((__may_alias__)) darwin_pthread_rwlock_t {
  long padding0;  // NOLINT(runtime/int) exact match to darwin type
  pthread_mutex_t padding1;
  int padding2;
  pthread_cond_t padding3;
  pthread_cond_t padding4;
  int padding5;
  int padding6;
  pthread_t darwin_pthread_rwlock_owner;
  // ...other stuff we don't care about.
};

#endif  // __APPLE__

#if defined(__GLIBC__)

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

#endif  // __GLIBC__

class ScopedContentionRecorder {
 public:
  ScopedContentionRecorder(BaseMutex* mutex, uint64_t blocked_tid, uint64_t owner_tid)
      : mutex_(kLogLockContentions ? mutex : NULL),
        blocked_tid_(kLogLockContentions ? blocked_tid : 0),
        owner_tid_(kLogLockContentions ? owner_tid : 0),
        start_nano_time_(kLogLockContentions ? NanoTime() : 0) {
    std::string msg = StringPrintf("Lock contention on %s (owner tid: %" PRIu64 ")",
                                   mutex->GetName(), owner_tid);
    ATRACE_BEGIN(msg.c_str());
  }

  ~ScopedContentionRecorder() {
    ATRACE_END();
    if (kLogLockContentions) {
      uint64_t end_nano_time = NanoTime();
      mutex_->RecordContention(blocked_tid_, owner_tid_, end_nano_time - start_nano_time_);
    }
  }

 private:
  BaseMutex* const mutex_;
  const uint64_t blocked_tid_;
  const uint64_t owner_tid_;
  const uint64_t start_nano_time_;
};

static inline uint64_t SafeGetTid(const Thread* self) {
  if (self != NULL) {
    return static_cast<uint64_t>(self->GetTid());
  } else {
    return static_cast<uint64_t>(GetTid());
  }
}

static inline void CheckUnattachedThread(LockLevel level) NO_THREAD_SAFETY_ANALYSIS {
  // The check below enumerates the cases where we expect not to be able to sanity check locks
  // on a thread. Lock checking is disabled to avoid deadlock when checking shutdown lock.
  // TODO: tighten this check.
  if (kDebugLocking) {
    Runtime* runtime = Runtime::Current();
    CHECK(runtime == NULL || !runtime->IsStarted() || runtime->IsShuttingDownLocked() ||
          level == kDefaultMutexLevel  || level == kRuntimeShutdownLock ||
          level == kThreadListLock || level == kLoggingLock || level == kAbortLock);
  }
}

inline void BaseMutex::RegisterAsLocked(Thread* self) {
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
        LOG(ERROR) << "Lock level violation: holding \"" << held_mutex->name_ << "\" "
                   << "(level " << LockLevel(i) << " - " << i
                   << ") while locking \"" << name_ << "\" "
                   << "(level " << level_ << " - " << static_cast<int>(level_) << ")";
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

inline void BaseMutex::RegisterAsUnlocked(Thread* self) {
  if (UNLIKELY(self == NULL)) {
    CheckUnattachedThread(level_);
    return;
  }
  if (level_ != kMonitorLock) {
    if (kDebugLocking && !gAborting) {
      CHECK(self->GetHeldMutex(level_) == this) << "Unlocking on unacquired mutex: " << name_;
    }
    self->SetHeldMutex(level_, NULL);
  }
}

inline void ReaderWriterMutex::SharedLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (LIKELY(cur_state >= 0)) {
      // Add as an extra reader.
      done = android_atomic_acquire_cas(cur_state, cur_state + 1, &state_) == 0;
    } else {
      // Owner holds it exclusively, hang up.
      ScopedContentionRecorder scr(this, GetExclusiveOwnerTid(), SafeGetTid(self));
      android_atomic_inc(&num_pending_readers_);
      if (futex(&state_, FUTEX_WAIT, cur_state, NULL, NULL, 0) != 0) {
        if (errno != EAGAIN) {
          PLOG(FATAL) << "futex wait failed for " << name_;
        }
      }
      android_atomic_dec(&num_pending_readers_);
    }
  } while (!done);
#else
  CHECK_MUTEX_CALL(pthread_rwlock_rdlock, (&rwlock_));
#endif
  RegisterAsLocked(self);
  AssertSharedHeld(self);
}

inline void ReaderWriterMutex::SharedUnlock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  AssertSharedHeld(self);
  RegisterAsUnlocked(self);
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (LIKELY(cur_state > 0)) {
      // Reduce state by 1.
      done = android_atomic_release_cas(cur_state, cur_state - 1, &state_) == 0;
      if (done && (cur_state - 1) == 0) {  // cas may fail due to noise?
        if (num_pending_writers_ > 0 || num_pending_readers_ > 0) {
          // Wake any exclusive waiters as there are now no readers.
          futex(&state_, FUTEX_WAKE, -1, NULL, NULL, 0);
        }
      }
    } else {
      LOG(FATAL) << "Unexpected state_:" << cur_state << " for " << name_;
    }
  } while (!done);
#else
  CHECK_MUTEX_CALL(pthread_rwlock_unlock, (&rwlock_));
#endif
}

inline bool Mutex::IsExclusiveHeld(const Thread* self) const {
  DCHECK(self == NULL || self == Thread::Current());
  bool result = (GetExclusiveOwnerTid() == SafeGetTid(self));
  if (kDebugLocking) {
    // Sanity debug check that if we think it is locked we have it in our held mutexes.
    if (result && self != NULL && level_ != kMonitorLock && !gAborting) {
      CHECK_EQ(self->GetHeldMutex(level_), this);
    }
  }
  return result;
}

inline uint64_t Mutex::GetExclusiveOwnerTid() const {
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

inline bool ReaderWriterMutex::IsExclusiveHeld(const Thread* self) const {
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

inline uint64_t ReaderWriterMutex::GetExclusiveOwnerTid() const {
#if ART_USE_FUTEXES
  int32_t state = state_;
  if (state == 0) {
    return 0;  // No owner.
  } else if (state > 0) {
    return -1;  // Shared.
  } else {
    return exclusive_owner_;
  }
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

}  // namespace art

#endif  // ART_RUNTIME_BASE_MUTEX_INL_H_
