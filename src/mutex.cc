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

#include "logging.h"
#include "runtime.h"
#include "thread.h"
#include "utils.h"

#define CHECK_MUTEX_CALL(call, args) CHECK_PTHREAD_CALL(call, args, name_)

namespace art {

// This works on Mac OS 10.7, but hasn't been tested on older releases.
struct __attribute__((__may_alias__)) darwin_pthread_mutex_t {
  uint32_t padding0[2];
  uint32_t value;
  uint32_t padding1[5];
  uint64_t owner_tid;
  // ...other stuff we don't care about.
};

struct __attribute__((__may_alias__)) glibc_pthread_mutex_t {
  int lock;
  unsigned int count;
  int owner;
  // ...other stuff we don't care about.
};

static inline void CheckSafeToLockOrUnlock(MutexRank rank, bool is_locking) {
  if (!kIsDebugBuild) {
    return;
  }
  if (rank == -1) {
    return;
  }
  Thread* self = Thread::Current();
  if (self != NULL) {
    self->CheckSafeToLockOrUnlock(rank, is_locking);
  }
}

static inline void CheckSafeToWait(MutexRank rank) {
  if (!kIsDebugBuild) {
    return;
  }
  Thread* self = Thread::Current();
  if (self != NULL) {
    self->CheckSafeToWait(rank);
  }
}

Mutex::Mutex(const char* name, MutexRank rank) : name_(name), rank_(rank) {
  // Like Java, we use recursive mutexes.
  pthread_mutexattr_t attributes;
  CHECK_MUTEX_CALL(pthread_mutexattr_init, (&attributes));
  CHECK_MUTEX_CALL(pthread_mutexattr_settype, (&attributes, PTHREAD_MUTEX_RECURSIVE));
  CHECK_MUTEX_CALL(pthread_mutex_init, (&mutex_, &attributes));
  CHECK_MUTEX_CALL(pthread_mutexattr_destroy, (&attributes));
}

Mutex::~Mutex() {
  int rc = pthread_mutex_destroy(&mutex_);
  if (rc != 0) {
    errno = rc;
    // TODO: should we just not log at all if shutting down? this could be the logging mutex!
    bool shutting_down = Runtime::Current()->IsShuttingDown();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_mutex_destroy failed for " << name_;
  }
}

void Mutex::Lock() {
  CheckSafeToLockOrUnlock(rank_, true);
  CHECK_MUTEX_CALL(pthread_mutex_lock, (&mutex_));
  AssertHeld();
}

bool Mutex::TryLock() {
  int result = pthread_mutex_trylock(&mutex_);
  if (result == EBUSY) {
    return false;
  }
  if (result != 0) {
    errno = result;
    PLOG(FATAL) << "pthread_mutex_trylock failed for " << name_;
  }
  CheckSafeToLockOrUnlock(rank_, true);
  AssertHeld();
  return true;
}

void Mutex::Unlock() {
  AssertHeld();
  CheckSafeToLockOrUnlock(rank_, false);
  CHECK_MUTEX_CALL(pthread_mutex_unlock, (&mutex_));
}

#if !defined(NDEBUG)
#if defined(__APPLE__) && MAC_OS_X_VERSION_MAX_ALLOWED < 1060
// Mac OS 10.5 didn't have anything we could implement GetTid() with. One thing we could try would
// be using pthread_t instead of the actual tid; this would be acceptable in most places, and more
// portable. 10.5 is already obsolete, though, so doing so would probably be all pain for no gain.
void Mutex::AssertHeld() {}
void Mutex::AssertNotHeld() {}
#else
void Mutex::AssertHeld() {
  DCHECK_EQ(GetOwner(), static_cast<uint64_t>(GetTid()));
}

void Mutex::AssertNotHeld() {
  DCHECK_NE(GetOwner(), static_cast<uint64_t>(GetTid()));
}
#endif
#endif

uint64_t Mutex::GetOwner() {
#if defined(__BIONIC__)
  return static_cast<uint64_t>((mutex_.value >> 16) & 0xffff);
#elif defined(__GLIBC__)
  return reinterpret_cast<glibc_pthread_mutex_t*>(&mutex_)->owner;
#elif defined(__APPLE__)
  return reinterpret_cast<darwin_pthread_mutex_t*>(&mutex_)->owner_tid;
#else
#error unsupported C library
#endif
}

uint32_t Mutex::GetDepth() {
  bool held = (GetOwner() == static_cast<uint64_t>(GetTid()));
  if (!held) {
    return 0;
  }
  uint32_t depth;
#if defined(__BIONIC__)
  depth = static_cast<uint32_t>((mutex_.value >> 2) & 0x7ff) + 1;
#elif defined(__GLIBC__)
  depth = reinterpret_cast<glibc_pthread_mutex_t*>(&mutex_)->count;
#elif defined(__APPLE__)
  darwin_pthread_mutex_t* darwin_mutex = reinterpret_cast<darwin_pthread_mutex_t*>(&mutex_);
  depth = ((darwin_mutex->value >> 16) & 0xffff);
#else
#error unsupported C library
#endif
  CHECK_NE(depth, 0U) << "owner=" << GetOwner() << " tid=" << GetTid();
  return depth;
}

ConditionVariable::ConditionVariable(const std::string& name) : name_(name) {
  CHECK_MUTEX_CALL(pthread_cond_init, (&cond_, NULL));
}

ConditionVariable::~ConditionVariable() {
  CHECK_MUTEX_CALL(pthread_cond_destroy, (&cond_));
}

void ConditionVariable::Broadcast() {
  CHECK_MUTEX_CALL(pthread_cond_broadcast, (&cond_));
}

void ConditionVariable::Signal() {
  CHECK_MUTEX_CALL(pthread_cond_signal, (&cond_));
}

void ConditionVariable::Wait(Mutex& mutex) {
  CheckSafeToWait(mutex.rank_);
  CHECK_MUTEX_CALL(pthread_cond_wait, (&cond_, &mutex.mutex_));
}

void ConditionVariable::TimedWait(Mutex& mutex, const timespec& ts) {
#ifdef HAVE_TIMEDWAIT_MONOTONIC
#define TIMEDWAIT pthread_cond_timedwait_monotonic
#else
#define TIMEDWAIT pthread_cond_timedwait
#endif
  CheckSafeToWait(mutex.rank_);
  int rc = TIMEDWAIT(&cond_, &mutex.mutex_, &ts);
  if (rc != 0 && rc != ETIMEDOUT) {
    errno = rc;
    PLOG(FATAL) << "TimedWait failed for " << name_;
  }
}

std::ostream& operator<<(std::ostream& os, const MutexRank& rhs) {
  switch (rhs) {
    case kHeapLock: os << "HeapLock"; break;
    case kThreadListLock: os << "ThreadListLock"; break;
    case kThreadSuspendCountLock: os << "ThreadSuspendCountLock"; break;
    default: os << "MutexRank[" << static_cast<int>(rhs) << "]"; break;
  }
  return os;
}

}  // namespace
