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

#ifndef ART_SRC_MUTEX_H_
#define ART_SRC_MUTEX_H_

#include <pthread.h>
#include <stdint.h>

#include <iosfwd>
#include <string>

#include "globals.h"
#include "gtest/gtest.h"
#include "logging.h"
#include "macros.h"

// Currently Darwin doesn't support locks with timeouts.
#if !defined(__APPLE__)
#define HAVE_TIMED_RWLOCK 1
#else
#define HAVE_TIMED_RWLOCK 0
#endif

namespace art {

class LOCKABLE Mutex;
class LOCKABLE ReaderWriterMutex;

// MutexLevel is used to impose a lock hierarchy [1] where acquisition of a Mutex at a higher or
// equal level to a lock a thread holds is invalid. The lock hierarchy achieves a cycle free
// partial ordering and thereby cause deadlock situations to fail checks.
//
// [1] http://www.drdobbs.com/parallel/use-lock-hierarchies-to-avoid-deadlock/204801163
enum MutexLevel {
  kLoggingLock = 0,
  kUnexpectedSignalLock = 1,
  kThreadSuspendCountLock = 2,
  kAbortLock = 3,
  kDefaultMutexLevel = 4,
  kLoadLibraryLock = 5,
  kClassLinkerClassesLock = 6,
  kThreadListLock = 7,
  kHeapBitmapLock = 8,
  kZygoteCreationLock = 9,
  kMonitorLock = 10,
  kMutatorLock = 11,
  kMaxMutexLevel = kMutatorLock,
};
std::ostream& operator<<(std::ostream& os, const MutexLevel& rhs);

// Global mutexes corresponding to the levels above.
class GlobalSynchronization {
 public:
  static void Init();

  // The mutator_lock_ is used to allow mutators to execute in a shared (reader) mode or to block
  // mutators by having an exclusive (writer) owner. In normal execution each mutator thread holds
  // a share on the mutator_lock_. The garbage collector may also execute with shared access but
  // at times requires exclusive access to the heap (not to be confused with the heap meta-data
  // guarded by the heap_lock_ below). When the garbage collector requires exclusive access it asks
  // the mutators to suspend themselves which also involves usage of the thread_suspend_count_lock_
  // to cover weaknesses in using ReaderWriterMutexes with ConditionVariables. We use a condition
  // variable to wait upon in the suspension logic as releasing and then re-acquiring a share on
  // the mutator lock doesn't necessarily allow the exclusive user (e.g the garbage collector)
  // chance to acquire the lock.
  //
  // Thread suspension:
  // Shared users                                  | Exclusive user
  // (holding mutator lock and in kRunnable state) |   .. running ..
  //   .. running ..                               | Request thread suspension by:
  //   .. running ..                               |   - acquiring thread_suspend_count_lock_
  //   .. running ..                               |   - incrementing Thread::suspend_count_ on
  //   .. running ..                               |     all mutator threads
  //   .. running ..                               |   - releasing thread_suspend_count_lock_
  //   .. running ..                               | Block trying to acquire exclusive mutator lock
  // Poll Thread::suspend_count_ and enter full    |   .. blocked ..
  // suspend code.                                 |   .. blocked ..
  // Change state to kSuspended                    |   .. blocked ..
  // x: Release share on mutator_lock_             | Carry out exclusive access
  // Acquire thread_suspend_count_lock_            |   .. exclusive ..
  // while Thread::suspend_count_ > 0              |   .. exclusive ..
  //   - wait on Thread::resume_cond_              |   .. exclusive ..
  //     (releases thread_suspend_count_lock_)     |   .. exclusive ..
  //   .. waiting ..                               | Release mutator_lock_
  //   .. waiting ..                               | Request thread resumption by:
  //   .. waiting ..                               |   - acquiring thread_suspend_count_lock_
  //   .. waiting ..                               |   - decrementing Thread::suspend_count_ on
  //   .. waiting ..                               |     all mutator threads
  //   .. waiting ..                               |   - notifying on Thread::resume_cond_
  //    - re-acquire thread_suspend_count_lock_    |   - releasing thread_suspend_count_lock_
  // Release thread_suspend_count_lock_            |  .. running ..
  // Acquire share on mutator_lock_                |  .. running ..
  //  - This could block but the thread still      |  .. running ..
  //    has a state of kSuspended and so this      |  .. running ..
  //    isn't an issue.                            |  .. running ..
  // Acquire thread_suspend_count_lock_            |  .. running ..
  //  - we poll here as we're transitioning into   |  .. running ..
  //    kRunnable and an individual thread suspend |  .. running ..
  //    request (e.g for debugging) won't try      |  .. running ..
  //    to acquire the mutator lock (which would   |  .. running ..
  //    block as we hold the mutator lock). This   |  .. running ..
  //    poll ensures that if the suspender thought |  .. running ..
  //    we were suspended by incrementing our      |  .. running ..
  //    Thread::suspend_count_ and then reading    |  .. running ..
  //    our state we go back to waiting on         |  .. running ..
  //    Thread::resume_cond_.                      |  .. running ..
  // can_go_runnable = Thread::suspend_count_ == 0 |  .. running ..
  // Release thread_suspend_count_lock_            |  .. running ..
  // if can_go_runnable                            |  .. running ..
  //   Change state to kRunnable                   |  .. running ..
  // else                                          |  .. running ..
  //   Goto x                                      |  .. running ..
  //  .. running ..                                |  .. running ..
  static ReaderWriterMutex* mutator_lock_;

  // Allow reader-writer mutual exclusion on the mark and live bitmaps of the heap.
  static ReaderWriterMutex* heap_bitmap_lock_ ACQUIRED_AFTER(mutator_lock_);

  // The thread_list_lock_ guards ThreadList::list_. It is also commonly held to stop threads
  // attaching and detaching.
  static Mutex* thread_list_lock_ ACQUIRED_AFTER(heap_bitmap_lock_);

  // Guards lists of classes within the class linker.
  static Mutex* classlinker_classes_lock_ ACQUIRED_AFTER(thread_list_lock_);

  // When declaring any Mutex add DEFAULT_MUTEX_ACQUIRED_AFTER to use annotalysis to check the code
  // doesn't try to hold a higher level Mutex.
  #define DEFAULT_MUTEX_ACQUIRED_AFTER ACQUIRED_AFTER(classlinker_classes_lock_)

  // Have an exclusive aborting thread.
  static Mutex* abort_lock_ ACQUIRED_AFTER(classlinker_classes_lock_);

  // Allow mutual exclusion when manipulating Thread::suspend_count_.
  // TODO: Does the trade-off of a per-thread lock make sense?
  static Mutex* thread_suspend_count_lock_ ACQUIRED_AFTER(abort_lock_);

  // One unexpected signal at a time lock.
  static Mutex* unexpected_signal_lock_ ACQUIRED_AFTER(thread_suspend_count_lock_);

  // Have an exclusive logging thread.
  static Mutex* logging_lock_ ACQUIRED_AFTER(unexpected_signal_lock_);
};

// Base class for all Mutex implementations
class BaseMutex {
 public:
  const std::string& GetName() const {
    return name_;
  }

  virtual bool IsMutex() const { return false; }
  virtual bool IsReaderWriterMutex() const { return false; }

 protected:
  friend class ConditionVariable;

  BaseMutex(const char* name, MutexLevel level);
  virtual ~BaseMutex() {}
  void RegisterAsLockedWithCurrentThread();
  void RegisterAsUnlockedWithCurrentThread();
  void CheckSafeToWait();

  const MutexLevel level_;  // Support for lock hierarchy.
  const std::string name_;
};

// A Mutex is used to achieve mutual exclusion between threads. A Mutex can be used to gain
// exclusive access to what it guards. A Mutex can be in one of two states:
// - Free - not owned by any thread,
// - Exclusive - owned by a single thread.
//
// The effect of locking and unlocking operations on the state is:
// State     | ExclusiveLock | ExclusiveUnlock
// -------------------------------------------
// Free      | Exclusive     | error
// Exclusive | Block*        | Free
// * Mutex is not reentrant and so an attempt to ExclusiveLock on the same thread will result in
//   an error. Being non-reentrant simplifies Waiting on ConditionVariables.
class LOCKABLE Mutex : public BaseMutex {
 public:
  explicit Mutex(const char* name, MutexLevel level = kDefaultMutexLevel, bool recursive = false);
  ~Mutex();

  virtual bool IsMutex() const { return true; }

  // Block until mutex is free then acquire exclusive access.
  void ExclusiveLock() EXCLUSIVE_LOCK_FUNCTION();
  void Lock() EXCLUSIVE_LOCK_FUNCTION() {  ExclusiveLock(); }

  // Returns true if acquires exclusive access, false otherwise.
  bool ExclusiveTryLock() EXCLUSIVE_TRYLOCK_FUNCTION(true);
  bool TryLock() EXCLUSIVE_TRYLOCK_FUNCTION(true) { return ExclusiveTryLock(); }

  // Release exclusive access.
  void ExclusiveUnlock() UNLOCK_FUNCTION();
  void Unlock() UNLOCK_FUNCTION() {  ExclusiveUnlock(); }

  // Is the current thread the exclusive holder of the Mutex.
  bool IsExclusiveHeld() const;

  // Assert that the Mutex is exclusively held by the current thread.
  void AssertExclusiveHeld() {
    if (kIsDebugBuild) {
      CHECK(IsExclusiveHeld());
    }
  }
  void AssertHeld() { AssertExclusiveHeld(); }

  // Assert that the Mutex is not held by the current thread.
  void AssertNotHeldExclusive() {
    if (kIsDebugBuild) {
      CHECK(!IsExclusiveHeld());
    }
  }
  void AssertNotHeld() { AssertNotHeldExclusive(); }

  // Id associated with exclusive owner.
  uint64_t GetExclusiveOwnerTid() const;

  // Returns how many times this Mutex has been locked, it is better to use AssertHeld/NotHeld.
  unsigned int GetDepth() const {
    return recursion_count_;
  }

 private:
  pthread_mutex_t mutex_;
  const bool recursive_;  // Can the lock be recursively held?
  unsigned int recursion_count_;
  friend class ConditionVariable;
  friend class MutexTester;
  DISALLOW_COPY_AND_ASSIGN(Mutex);
};

// A ReaderWriterMutex is used to achieve mutual exclusion between threads, similar to a Mutex.
// Unlike a Mutex a ReaderWriterMutex can be used to gain exclusive (writer) or shared (reader)
// access to what it guards. A flaw in relation to a Mutex is that it cannot be used with a
// condition variable. A ReaderWriterMutex can be in one of three states:
// - Free - not owned by any thread,
// - Exclusive - owned by a single thread,
// - Shared(n) - shared amongst n threads.
//
// The effect of locking and unlocking operations on the state is:
//
// State     | ExclusiveLock | ExclusiveUnlock | SharedLock       | SharedUnlock
// ----------------------------------------------------------------------------
// Free      | Exclusive     | error           | SharedLock(1)    | error
// Exclusive | Block         | Free            | Block            | error
// Shared(n) | Block         | error           | SharedLock(n+1)* | Shared(n-1) or Free
// * for large values of n the SharedLock may block.
class LOCKABLE ReaderWriterMutex : public BaseMutex {
 public:
  explicit ReaderWriterMutex(const char* name, MutexLevel level = kDefaultMutexLevel);
  ~ReaderWriterMutex();

  virtual bool IsReaderWriterMutex() const { return true; }

  // Block until ReaderWriterMutex is free then acquire exclusive access.
  void ExclusiveLock() EXCLUSIVE_LOCK_FUNCTION();
  void WriterLock() EXCLUSIVE_LOCK_FUNCTION() {  ExclusiveLock(); }

  // Release exclusive access.
  void ExclusiveUnlock() UNLOCK_FUNCTION();
  void WriterUnlock() UNLOCK_FUNCTION() {  ExclusiveUnlock(); }

  // Block until ReaderWriterMutex is free and acquire exclusive access. Returns true on success
  // or false if timeout is reached.
#if HAVE_TIMED_RWLOCK
  bool ExclusiveLockWithTimeout(const timespec& abs_timeout) EXCLUSIVE_TRYLOCK_FUNCTION(true);
#endif

  // Block until ReaderWriterMutex is shared or free then acquire a share on the access.
  void SharedLock() SHARED_LOCK_FUNCTION();
  void ReaderLock() SHARED_LOCK_FUNCTION() { SharedLock(); }

  // Try to acquire share of ReaderWriterMutex.
  bool SharedTryLock() EXCLUSIVE_TRYLOCK_FUNCTION(true);

  // Release a share of the access.
  void SharedUnlock() UNLOCK_FUNCTION();
  void ReaderUnlock() UNLOCK_FUNCTION() { SharedUnlock(); }

  // Is the current thread the exclusive holder of the ReaderWriterMutex.
  bool IsExclusiveHeld() const;

  // Assert the current thread has exclusive access to the ReaderWriterMutex.
  void AssertExclusiveHeld() {
    if (kIsDebugBuild) {
      CHECK(IsExclusiveHeld());
    }
  }
  void AssertWriterHeld() { AssertExclusiveHeld(); }

  // Assert the current thread doesn't have exclusive access to the ReaderWriterMutex.
  void AssertNotExclusiveHeld() {
    if (kIsDebugBuild) {
      CHECK(!IsExclusiveHeld());
    }
  }
  void AssertNotWriterHeld() { AssertNotExclusiveHeld(); }

  // Is the current thread a shared holder of the ReaderWriterMutex.
  bool IsSharedHeld() const;

  // Assert the current thread has shared access to the ReaderWriterMutex.
  void AssertSharedHeld() {
    if (kIsDebugBuild) {
      CHECK(IsSharedHeld());
    }
  }
  void AssertReaderHeld() { AssertSharedHeld(); }

  // Assert the current thread doesn't hold this ReaderWriterMutex either in shared or exclusive
  // mode.
  void AssertNotHeld() {
    if (kIsDebugBuild) {
      CHECK(!IsSharedHeld());
    }
  }

  // Id associated with exclusive owner.
  uint64_t GetExclusiveOwnerTid() const;
 private:
  pthread_rwlock_t rwlock_;

  friend class MutexTester;
  DISALLOW_COPY_AND_ASSIGN(ReaderWriterMutex);
};

// ConditionVariables allow threads to queue and sleep. Threads may then be resumed individually
// (Signal) or all at once (Broadcast).
class ConditionVariable {
 public:
  explicit ConditionVariable(const std::string& name);
  ~ConditionVariable();

  void Broadcast();
  void Signal();
  void Wait(Mutex& mutex);
  void TimedWait(Mutex& mutex, const timespec& ts);

 private:
  pthread_cond_t cond_;
  std::string name_;
  DISALLOW_COPY_AND_ASSIGN(ConditionVariable);
};

// Scoped locker/unlocker for a regular Mutex that acquires mu upon construction and releases it
// upon destruction.
class SCOPED_LOCKABLE MutexLock {
 public:
  explicit MutexLock(Mutex& mu) EXCLUSIVE_LOCK_FUNCTION(mu) : mu_(mu) {
    mu_.ExclusiveLock();
  }

  ~MutexLock() UNLOCK_FUNCTION() {
    mu_.ExclusiveUnlock();
  }

 private:
  Mutex& mu_;
  DISALLOW_COPY_AND_ASSIGN(MutexLock);
};
// Catch bug where variable name is omitted. "MutexLock (lock);" instead of "MutexLock mu(lock)".
#define MutexLock(x) COMPILE_ASSERT(0, mutex_lock_declaration_missing_variable_name)

// Scoped locker/unlocker for a ReaderWriterMutex that acquires read access to mu upon
// construction and releases it upon destruction.
class SCOPED_LOCKABLE ReaderMutexLock {
 public:
  explicit ReaderMutexLock(ReaderWriterMutex& mu) EXCLUSIVE_LOCK_FUNCTION(mu) : mu_(mu) {
    mu_.SharedLock();
  }

  ~ReaderMutexLock() UNLOCK_FUNCTION() {
    mu_.SharedUnlock();
  }

 private:
  ReaderWriterMutex& mu_;
  DISALLOW_COPY_AND_ASSIGN(ReaderMutexLock);
};
// Catch bug where variable name is omitted. "ReaderMutexLock (lock);" instead of
// "ReaderMutexLock mu(lock)".
#define ReaderMutexLock(x) COMPILE_ASSERT(0, reader_mutex_lock_declaration_missing_variable_name)

// Scoped locker/unlocker for a ReaderWriterMutex that acquires write access to mu upon
// construction and releases it upon destruction.
class SCOPED_LOCKABLE WriterMutexLock {
 public:
  explicit WriterMutexLock(ReaderWriterMutex& mu) EXCLUSIVE_LOCK_FUNCTION(mu) : mu_(mu) {
    mu_.ExclusiveLock();
  }

  ~WriterMutexLock() UNLOCK_FUNCTION() {
    mu_.ExclusiveUnlock();
  }

 private:
  ReaderWriterMutex& mu_;
  DISALLOW_COPY_AND_ASSIGN(WriterMutexLock);
};
// Catch bug where variable name is omitted. "WriterMutexLock (lock);" instead of
// "WriterMutexLock mu(lock)".
#define WriterMutexLock(x) COMPILE_ASSERT(0, writer_mutex_lock_declaration_missing_variable_name)

// Scoped unlocker/locker for a ReaderWriterMutex that releases read access to mu upon
// construction and acquires it again upon destruction.
class ReaderMutexUnlock {
 public:
  explicit ReaderMutexUnlock(ReaderWriterMutex& mu) UNLOCK_FUNCTION(mu) : mu_(mu) {
    mu_.SharedUnlock();
  }

  ~ReaderMutexUnlock() SHARED_LOCK_FUNCTION(mu_) {
    mu_.SharedLock();
  }

 private:
  ReaderWriterMutex& mu_;
  DISALLOW_COPY_AND_ASSIGN(ReaderMutexUnlock);
};
// Catch bug where variable name is omitted. "ReaderMutexUnlock (lock);" instead of
// "ReaderMutexUnlock mu(lock)".
#define ReaderMutexUnlock(x) \
    COMPILE_ASSERT(0, reader_mutex_unlock_declaration_missing_variable_name)

}  // namespace art

#endif  // ART_SRC_MUTEX_H_
