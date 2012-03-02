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

static inline void CheckRank(MutexRank rank, bool is_locking) {
#ifndef NDEBUG
  if (rank == -1) {
    return;
  }
  Thread* self = Thread::Current();
  if (self != NULL) {
    self->CheckRank(rank, is_locking);
  }
#endif
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
    bool shutting_down = Runtime::Current()->IsShuttingDown();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_mutex_destroy failed for " << name_;
  }
}

void Mutex::Lock() {
  CHECK_MUTEX_CALL(pthread_mutex_lock, (&mutex_));
  CheckRank(rank_, true);
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
  CheckRank(rank_, true);
  AssertHeld();
  return true;
}

void Mutex::Unlock() {
  AssertHeld();
  CHECK_MUTEX_CALL(pthread_mutex_unlock, (&mutex_));
  CheckRank(rank_, false);
}

pid_t Mutex::GetOwner() {
#if defined(__BIONIC__)
  return static_cast<pid_t>((mutex_.value >> 16) & 0xffff);
#elif defined(__GLIBC__)
  struct __attribute__((__may_alias__)) glibc_pthread_t {
    int lock;
    unsigned int count;
    int owner;
    // ...other stuff we don't care about.
  };
  return reinterpret_cast<glibc_pthread_t*>(&mutex_)->owner;
#elif defined(__APPLE__)
  // We don't know a way to implement this for Mac OS.
  return 0;
#else
  UNIMPLEMENTED(FATAL);
  return 0;
#endif
}

uint32_t Mutex::GetDepth() {
  bool held = (GetOwner() == GetTid());
  if (!held) {
    return 0;
  }
  uint32_t depth;
#if defined(__BIONIC__)
  depth = static_cast<uint32_t>((mutex_.value >> 2) & 0x7ff) + 1;
#elif defined(__GLIBC__)
  struct __attribute__((__may_alias__)) glibc_pthread_t {
    int lock;
    unsigned int count;
    int owner;
    // ...other stuff we don't care about.
  };
  depth = reinterpret_cast<glibc_pthread_t*>(&mutex_)->count;
#elif defined(__APPLE__)
  // We don't know a way to implement this for Mac OS.
  return 0;
#else
  UNIMPLEMENTED(FATAL);
  return 0;
#endif
  CHECK_NE(0U, depth) << "owner=" << GetOwner() << " tid=" << GetTid();
  return depth;
}

pid_t Mutex::GetTid() {
  return ::art::GetTid();
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
  CHECK_MUTEX_CALL(pthread_cond_wait, (&cond_, mutex.GetImpl()));
}

void ConditionVariable::TimedWait(Mutex& mutex, const timespec& ts) {
#ifdef HAVE_TIMEDWAIT_MONOTONIC
#define TIMEDWAIT pthread_cond_timedwait_monotonic
#else
#define TIMEDWAIT pthread_cond_timedwait
#endif
  int rc = TIMEDWAIT(&cond_, mutex.GetImpl(), &ts);
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
