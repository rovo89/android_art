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

#if defined(__APPLE__)
#include "AvailabilityMacros.h" // For MAC_OS_X_VERSION_MAX_ALLOWED
#endif

#define CHECK_MUTEX_CALL(call, args) CHECK_PTHREAD_CALL(call, args, name_)

extern int pthread_mutex_lock(pthread_mutex_t* mutex) EXCLUSIVE_LOCK_FUNCTION(mutex);
extern int pthread_mutex_unlock(pthread_mutex_t* mutex) UNLOCK_FUNCTION(1);
extern int pthread_mutex_trylock(pthread_mutex_t* mutex) EXCLUSIVE_TRYLOCK_FUNCTION(0, mutex);

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

ReaderWriterMutex* GlobalSynchronization::mutator_lock_ = NULL;
Mutex* GlobalSynchronization::thread_list_lock_ = NULL;
Mutex* GlobalSynchronization::classlinker_classes_lock_ = NULL;
ReaderWriterMutex* GlobalSynchronization::heap_bitmap_lock_ = NULL;
Mutex* GlobalSynchronization::abort_lock_ = NULL;
Mutex* GlobalSynchronization::logging_lock_ = NULL;
Mutex* GlobalSynchronization::unexpected_signal_lock_ = NULL;
Mutex* GlobalSynchronization::thread_suspend_count_lock_ = NULL;

void GlobalSynchronization::Init() {
  if (logging_lock_ != NULL) {
    // Already initialized.
    DCHECK(mutator_lock_ != NULL);
    DCHECK(thread_list_lock_ != NULL);
    DCHECK(classlinker_classes_lock_ != NULL);
    DCHECK(heap_bitmap_lock_ != NULL);
    DCHECK(abort_lock_ != NULL);
    DCHECK(logging_lock_ != NULL);
    DCHECK(unexpected_signal_lock_ != NULL);
    DCHECK(thread_suspend_count_lock_ != NULL);
  } else {
    logging_lock_ = new Mutex("logging lock", kLoggingLock, true);
    abort_lock_ = new Mutex("abort lock", kAbortLock, true);
    DCHECK(mutator_lock_ == NULL);
    mutator_lock_ = new ReaderWriterMutex("mutator lock", kMutatorLock);
    DCHECK(thread_list_lock_ == NULL);
    thread_list_lock_ = new Mutex("thread list lock", kThreadListLock);
    DCHECK(classlinker_classes_lock_ == NULL);
    classlinker_classes_lock_ = new Mutex("ClassLinker classes lock", kClassLinkerClassesLock);
    DCHECK(heap_bitmap_lock_ == NULL);
    heap_bitmap_lock_ = new ReaderWriterMutex("heap bitmap lock", kHeapBitmapLock);
    DCHECK(unexpected_signal_lock_ == NULL);
    unexpected_signal_lock_ = new Mutex("unexpected signal lock", kUnexpectedSignalLock, true);
    DCHECK(thread_suspend_count_lock_ == NULL);
    thread_suspend_count_lock_ = new Mutex("thread suspend count lock", kThreadSuspendCountLock);
  }
}

BaseMutex::BaseMutex(const char* name, MutexLevel level) : level_(level), name_(name) {}

static void CheckUnattachedThread(MutexLevel level) {
  // The check below enumerates the cases where we expect not to be able to sanity check locks
  // on a thread. TODO: tighten this check.
  Runtime* runtime = Runtime::Current();
  CHECK(runtime == NULL || !runtime->IsStarted() || runtime->IsShuttingDown() ||
        level == kDefaultMutexLevel  || level == kThreadListLock ||
        level == kLoggingLock || level == kAbortLock);
}

void BaseMutex::RegisterAsLockedWithCurrentThread() {
  Thread* self = Thread::Current();
  if (self == NULL) {
    CheckUnattachedThread(level_);
    return;
  }
  // Check if a bad Mutex of this level or lower is held.
  bool bad_mutexes_held = false;
  for (int i = level_; i >= 0; --i) {
    BaseMutex* held_mutex = self->GetHeldMutex(static_cast<MutexLevel>(i));
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
  // Don't record monitors as they are outside the scope of analysis. They may be inspected off of
  // the monitor list.
  if (level_ != kMonitorLock) {
    self->SetHeldMutex(level_, this);
  }
}

void BaseMutex::RegisterAsUnlockedWithCurrentThread() {
  Thread* self = Thread::Current();
  if (self == NULL) {
    CheckUnattachedThread(level_);
    return;
  }
  if (level_ != kMonitorLock) {
    CHECK(self->GetHeldMutex(level_) == this) << "Unlocking on unacquired mutex: " << name_;
    self->SetHeldMutex(level_, NULL);
  }
}

void BaseMutex::CheckSafeToWait() {
  Thread* self = Thread::Current();
  if (self == NULL) {
    CheckUnattachedThread(level_);
    return;
  }
  CHECK(self->GetHeldMutex(level_) == this) << "Waiting on unacquired mutex: " << name_;
  bool bad_mutexes_held = false;
  for (int i = kMaxMutexLevel; i >= 0; --i) {
    if (i != level_) {
      BaseMutex* held_mutex = self->GetHeldMutex(static_cast<MutexLevel>(i));
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

Mutex::Mutex(const char* name, MutexLevel level, bool recursive)
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

void Mutex::ExclusiveLock() {
  bool is_held = IsExclusiveHeld();
  CHECK(recursive_ || !is_held)
      << "Error attempt to recursively lock non-recursive lock \"" << name_ << "\"";
  if (!is_held) {
    CHECK_MUTEX_CALL(pthread_mutex_lock, (&mutex_));
    RegisterAsLockedWithCurrentThread();
  }
  recursion_count_++;
  DCHECK(recursion_count_ == 1 || recursive_) << "Unexpected recursion count on mutex: "
      << name_ << " " << recursion_count_;
  AssertHeld();
}

bool Mutex::ExclusiveTryLock() {
  bool is_held = IsExclusiveHeld();
  CHECK(recursive_ || !is_held)
      << "Error attempt to recursively lock non-recursive lock \"" << name_ << "\"";
  if (!is_held) {
    int result = pthread_mutex_trylock(&mutex_);
    if (result == EBUSY) {
      return false;
    }
    if (result != 0) {
      errno = result;
      PLOG(FATAL) << "pthread_mutex_trylock failed for " << name_;
    }
    RegisterAsLockedWithCurrentThread();
  }
  recursion_count_++;
  AssertHeld();
  return true;
}

void Mutex::ExclusiveUnlock() {
  AssertHeld();
  recursion_count_--;
  if (!recursive_ || recursion_count_ == 0) {
    DCHECK(recursion_count_ == 0 || recursive_) << "Unexpected recursion count on mutex: "
        << name_ << " " << recursion_count_;
    RegisterAsUnlockedWithCurrentThread();
    CHECK_MUTEX_CALL(pthread_mutex_unlock, (&mutex_));
  }
}

bool Mutex::IsExclusiveHeld() const {
  Thread* self = Thread::Current();
  bool result;
  if (self == NULL || level_ == kMonitorLock) {  // Handle unattached threads and monitors.
    result = (GetExclusiveOwnerTid() == static_cast<uint64_t>(GetTid()));
  } else {
    result = (self->GetHeldMutex(level_) == this);
    // Sanity debug check that if we think it is locked, so does the pthread.
    DCHECK(result == (GetExclusiveOwnerTid() == static_cast<uint64_t>(GetTid())));
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

ReaderWriterMutex::ReaderWriterMutex(const char* name, MutexLevel level) : BaseMutex(name, level) {
  CHECK_MUTEX_CALL(pthread_rwlock_init, (&rwlock_, NULL));
}

ReaderWriterMutex::~ReaderWriterMutex() {
  // We can't use CHECK_MUTEX_CALL here because on shutdown a suspended daemon thread
  // may still be using locks.
  int rc = pthread_rwlock_destroy(&rwlock_);
  if (rc != 0) {
    errno = rc;
    // TODO: should we just not log at all if shutting down? this could be the logging mutex!
    bool shutting_down = Runtime::Current()->IsShuttingDown();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_mutex_destroy failed for " << name_;
  }
}

void ReaderWriterMutex::ExclusiveLock() {
  AssertNotExclusiveHeld();
  CHECK_MUTEX_CALL(pthread_rwlock_wrlock, (&rwlock_));
  RegisterAsLockedWithCurrentThread();
  AssertExclusiveHeld();
}

void ReaderWriterMutex::ExclusiveUnlock() {
  AssertExclusiveHeld();
  RegisterAsUnlockedWithCurrentThread();
  CHECK_MUTEX_CALL(pthread_rwlock_unlock, (&rwlock_));
}

#if HAVE_TIMED_RWLOCK
bool ReaderWriterMutex::ExclusiveLockWithTimeout(const timespec& abs_timeout) {
  int result = pthread_rwlock_timedwrlock(&rwlock_, &abs_timeout);
  if (result == ETIMEDOUT) {
    return false;
  }
  if (result != 0) {
    errno = result;
    PLOG(FATAL) << "pthread_rwlock_timedwrlock failed for " << name_;
  }
  RegisterAsLockedWithCurrentThread();
  AssertSharedHeld();
  return true;
}
#endif

void ReaderWriterMutex::SharedLock() {
  CHECK_MUTEX_CALL(pthread_rwlock_rdlock, (&rwlock_));
  RegisterAsLockedWithCurrentThread();
  AssertSharedHeld();
}

bool ReaderWriterMutex::SharedTryLock() {
  int result = pthread_rwlock_tryrdlock(&rwlock_);
  if (result == EBUSY) {
    return false;
  }
  if (result != 0) {
    errno = result;
    PLOG(FATAL) << "pthread_mutex_trylock failed for " << name_;
  }
  RegisterAsLockedWithCurrentThread();
  AssertSharedHeld();
  return true;
}

void ReaderWriterMutex::SharedUnlock() {
  AssertSharedHeld();
  RegisterAsUnlockedWithCurrentThread();
  CHECK_MUTEX_CALL(pthread_rwlock_unlock, (&rwlock_));
}

bool ReaderWriterMutex::IsExclusiveHeld() const {
  bool result = (GetExclusiveOwnerTid() == static_cast<uint64_t>(GetTid()));
  // Sanity that if the pthread thinks we own the lock the Thread agrees.
  Thread* self = Thread::Current();
  DCHECK((self == NULL) || !result || (self->GetHeldMutex(level_) == this));
  return result;
}

bool ReaderWriterMutex::IsSharedHeld() const {
  Thread* self = Thread::Current();
  bool result;
  if (UNLIKELY(self == NULL)) {  // Handle unattached threads.
    result = IsExclusiveHeld(); // TODO: a better best effort here.
  } else {
    result = (self->GetHeldMutex(level_) == this);
  }
  return result;
}

uint64_t ReaderWriterMutex::GetExclusiveOwnerTid() const {
#if defined(__BIONIC__)
  return rwlock_.writerThreadId;
#elif defined(__GLIBC__)
  return reinterpret_cast<const glibc_pthread_rwlock_t*>(&rwlock_)->writer;
#elif defined(__APPLE__)
  const darwin_pthread_rwlock_t* dprwlock = reinterpret_cast<const darwin_pthread_rwlock_t*>(&rwlock_);
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

void ConditionVariable::Wait(Mutex& mutex) {
  mutex.CheckSafeToWait();
  unsigned int old_recursion_count = mutex.recursion_count_;
  mutex.recursion_count_ = 0;
  CHECK_MUTEX_CALL(pthread_cond_wait, (&cond_, &mutex.mutex_));
  mutex.recursion_count_ = old_recursion_count;
}

void ConditionVariable::TimedWait(Mutex& mutex, const timespec& ts) {
#ifdef HAVE_TIMEDWAIT_MONOTONIC
#define TIMEDWAIT pthread_cond_timedwait_monotonic
#else
#define TIMEDWAIT pthread_cond_timedwait
#endif
  mutex.CheckSafeToWait();
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
