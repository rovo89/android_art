/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "monitor.h"

#include <vector>

#include "base/mutex.h"
#include "base/stl_util.h"
#include "class_linker.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "lock_word-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "object_utils.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

namespace art {

/*
 * Every Object has a monitor associated with it, but not every Object is actually locked.  Even
 * the ones that are locked do not need a full-fledged monitor until a) there is actual contention
 * or b) wait() is called on the Object.
 *
 * For Android, we have implemented a scheme similar to the one described in Bacon et al.'s
 * "Thin locks: featherweight synchronization for Java" (ACM 1998).  Things are even easier for us,
 * though, because we have a full 32 bits to work with.
 *
 * The two states of an Object's lock are referred to as "thin" and "fat".  A lock may transition
 * from the "thin" state to the "fat" state and this transition is referred to as inflation. Once
 * a lock has been inflated it remains in the "fat" state indefinitely.
 *
 * The lock value itself is stored in mirror::Object::monitor_ and the representation is described
 * in the LockWord value type.
 *
 * Monitors provide:
 *  - mutually exclusive access to resources
 *  - a way for multiple threads to wait for notification
 *
 * In effect, they fill the role of both mutexes and condition variables.
 *
 * Only one thread can own the monitor at any time.  There may be several threads waiting on it
 * (the wait call unlocks it).  One or more waiting threads may be getting interrupted or notified
 * at any given time.
 */

bool (*Monitor::is_sensitive_thread_hook_)() = NULL;
uint32_t Monitor::lock_profiling_threshold_ = 0;

bool Monitor::IsSensitiveThread() {
  if (is_sensitive_thread_hook_ != NULL) {
    return (*is_sensitive_thread_hook_)();
  }
  return false;
}

void Monitor::Init(uint32_t lock_profiling_threshold, bool (*is_sensitive_thread_hook)()) {
  lock_profiling_threshold_ = lock_profiling_threshold;
  is_sensitive_thread_hook_ = is_sensitive_thread_hook;
}

Monitor::Monitor(Thread* self, Thread* owner, mirror::Object* obj, int32_t hash_code)
    : monitor_lock_("a monitor lock", kMonitorLock),
      monitor_contenders_("monitor contenders", monitor_lock_),
      num_waiters_(0),
      owner_(owner),
      lock_count_(0),
      obj_(obj),
      wait_set_(NULL),
      hash_code_(hash_code),
      locking_method_(NULL),
      locking_dex_pc_(0),
      monitor_id_(MonitorPool::CreateMonitorId(self, this)) {
  // We should only inflate a lock if the owner is ourselves or suspended. This avoids a race
  // with the owner unlocking the thin-lock.
  CHECK(owner == nullptr || owner == self || owner->IsSuspended());
  // The identity hash code is set for the life time of the monitor.
}

int32_t Monitor::GetHashCode() {
  while (!HasHashCode()) {
    if (hash_code_.CompareAndSwap(0, mirror::Object::GenerateIdentityHashCode())) {
      break;
    }
  }
  DCHECK(HasHashCode());
  return hash_code_.Load();
}

bool Monitor::Install(Thread* self) {
  MutexLock mu(self, monitor_lock_);  // Uncontended mutex acquisition as monitor isn't yet public.
  CHECK(owner_ == nullptr || owner_ == self || owner_->IsSuspended());
  // Propagate the lock state.
  LockWord lw(obj_->GetLockWord(false));
  switch (lw.GetState()) {
    case LockWord::kThinLocked: {
      CHECK_EQ(owner_->GetThreadId(), lw.ThinLockOwner());
      lock_count_ = lw.ThinLockCount();
      break;
    }
    case LockWord::kHashCode: {
      CHECK_EQ(hash_code_, static_cast<int32_t>(lw.GetHashCode()));
      break;
    }
    case LockWord::kFatLocked: {
      // The owner_ is suspended but another thread beat us to install a monitor.
      return false;
    }
    case LockWord::kUnlocked: {
      LOG(FATAL) << "Inflating unlocked lock word";
      break;
    }
    default: {
      LOG(FATAL) << "Invalid monitor state " << lw.GetState();
      return false;
    }
  }
  LockWord fat(this);
  // Publish the updated lock word, which may race with other threads.
  bool success = obj_->CasLockWord(lw, fat);
  // Lock profiling.
  if (success && owner_ != nullptr && lock_profiling_threshold_ != 0) {
    locking_method_ = owner_->GetCurrentMethod(&locking_dex_pc_);
  }
  return success;
}

Monitor::~Monitor() {
  MonitorPool::ReleaseMonitorId(monitor_id_);
  // Deflated monitors have a null object.
}

/*
 * Links a thread into a monitor's wait set.  The monitor lock must be
 * held by the caller of this routine.
 */
void Monitor::AppendToWaitSet(Thread* thread) {
  DCHECK(owner_ == Thread::Current());
  DCHECK(thread != NULL);
  DCHECK(thread->GetWaitNext() == nullptr) << thread->GetWaitNext();
  if (wait_set_ == NULL) {
    wait_set_ = thread;
    return;
  }

  // push_back.
  Thread* t = wait_set_;
  while (t->GetWaitNext() != nullptr) {
    t = t->GetWaitNext();
  }
  t->SetWaitNext(thread);
}

/*
 * Unlinks a thread from a monitor's wait set.  The monitor lock must
 * be held by the caller of this routine.
 */
void Monitor::RemoveFromWaitSet(Thread *thread) {
  DCHECK(owner_ == Thread::Current());
  DCHECK(thread != NULL);
  if (wait_set_ == NULL) {
    return;
  }
  if (wait_set_ == thread) {
    wait_set_ = thread->GetWaitNext();
    thread->SetWaitNext(nullptr);
    return;
  }

  Thread* t = wait_set_;
  while (t->GetWaitNext() != NULL) {
    if (t->GetWaitNext() == thread) {
      t->SetWaitNext(thread->GetWaitNext());
      thread->SetWaitNext(nullptr);
      return;
    }
    t = t->GetWaitNext();
  }
}

void Monitor::SetObject(mirror::Object* object) {
  obj_ = object;
}

void Monitor::Lock(Thread* self) {
  MutexLock mu(self, monitor_lock_);
  while (true) {
    if (owner_ == nullptr) {  // Unowned.
      owner_ = self;
      CHECK_EQ(lock_count_, 0);
      // When debugging, save the current monitor holder for future
      // acquisition failures to use in sampled logging.
      if (lock_profiling_threshold_ != 0) {
        locking_method_ = self->GetCurrentMethod(&locking_dex_pc_);
      }
      return;
    } else if (owner_ == self) {  // Recursive.
      lock_count_++;
      return;
    }
    // Contended.
    const bool log_contention = (lock_profiling_threshold_ != 0);
    uint64_t wait_start_ms = log_contention ? 0 : MilliTime();
    mirror::ArtMethod* owners_method = locking_method_;
    uint32_t owners_dex_pc = locking_dex_pc_;
    // Do this before releasing the lock so that we don't get deflated.
    ++num_waiters_;
    monitor_lock_.Unlock(self);  // Let go of locks in order.
    {
      ScopedThreadStateChange tsc(self, kBlocked);  // Change to blocked and give up mutator_lock_.
      self->SetMonitorEnterObject(obj_);
      MutexLock mu2(self, monitor_lock_);  // Reacquire monitor_lock_ without mutator_lock_ for Wait.
      if (owner_ != NULL) {  // Did the owner_ give the lock up?
        monitor_contenders_.Wait(self);  // Still contended so wait.
        // Woken from contention.
        if (log_contention) {
          uint64_t wait_ms = MilliTime() - wait_start_ms;
          uint32_t sample_percent;
          if (wait_ms >= lock_profiling_threshold_) {
            sample_percent = 100;
          } else {
            sample_percent = 100 * wait_ms / lock_profiling_threshold_;
          }
          if (sample_percent != 0 && (static_cast<uint32_t>(rand() % 100) < sample_percent)) {
            const char* owners_filename;
            uint32_t owners_line_number;
            TranslateLocation(owners_method, owners_dex_pc, &owners_filename, &owners_line_number);
            LogContentionEvent(self, wait_ms, sample_percent, owners_filename, owners_line_number);
          }
        }
      }
      self->SetMonitorEnterObject(nullptr);
    }
    monitor_lock_.Lock(self);  // Reacquire locks in order.
    --num_waiters_;
  }
}

static void ThrowIllegalMonitorStateExceptionF(const char* fmt, ...)
                                              __attribute__((format(printf, 1, 2)));

static void ThrowIllegalMonitorStateExceptionF(const char* fmt, ...)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  va_list args;
  va_start(args, fmt);
  Thread* self = Thread::Current();
  ThrowLocation throw_location = self->GetCurrentLocationForThrow();
  self->ThrowNewExceptionV(throw_location, "Ljava/lang/IllegalMonitorStateException;", fmt, args);
  if (!Runtime::Current()->IsStarted() || VLOG_IS_ON(monitor)) {
    std::ostringstream ss;
    self->Dump(ss);
    LOG(Runtime::Current()->IsStarted() ? INFO : ERROR)
        << self->GetException(NULL)->Dump() << "\n" << ss.str();
  }
  va_end(args);
}

static std::string ThreadToString(Thread* thread) {
  if (thread == NULL) {
    return "NULL";
  }
  std::ostringstream oss;
  // TODO: alternatively, we could just return the thread's name.
  oss << *thread;
  return oss.str();
}

void Monitor::FailedUnlock(mirror::Object* o, Thread* expected_owner, Thread* found_owner,
                           Monitor* monitor) {
  Thread* current_owner = NULL;
  std::string current_owner_string;
  std::string expected_owner_string;
  std::string found_owner_string;
  {
    // TODO: isn't this too late to prevent threads from disappearing?
    // Acquire thread list lock so threads won't disappear from under us.
    MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
    // Re-read owner now that we hold lock.
    current_owner = (monitor != NULL) ? monitor->GetOwner() : NULL;
    // Get short descriptions of the threads involved.
    current_owner_string = ThreadToString(current_owner);
    expected_owner_string = ThreadToString(expected_owner);
    found_owner_string = ThreadToString(found_owner);
  }
  if (current_owner == NULL) {
    if (found_owner == NULL) {
      ThrowIllegalMonitorStateExceptionF("unlock of unowned monitor on object of type '%s'"
                                         " on thread '%s'",
                                         PrettyTypeOf(o).c_str(),
                                         expected_owner_string.c_str());
    } else {
      // Race: the original read found an owner but now there is none
      ThrowIllegalMonitorStateExceptionF("unlock of monitor owned by '%s' on object of type '%s'"
                                         " (where now the monitor appears unowned) on thread '%s'",
                                         found_owner_string.c_str(),
                                         PrettyTypeOf(o).c_str(),
                                         expected_owner_string.c_str());
    }
  } else {
    if (found_owner == NULL) {
      // Race: originally there was no owner, there is now
      ThrowIllegalMonitorStateExceptionF("unlock of monitor owned by '%s' on object of type '%s'"
                                         " (originally believed to be unowned) on thread '%s'",
                                         current_owner_string.c_str(),
                                         PrettyTypeOf(o).c_str(),
                                         expected_owner_string.c_str());
    } else {
      if (found_owner != current_owner) {
        // Race: originally found and current owner have changed
        ThrowIllegalMonitorStateExceptionF("unlock of monitor originally owned by '%s' (now"
                                           " owned by '%s') on object of type '%s' on thread '%s'",
                                           found_owner_string.c_str(),
                                           current_owner_string.c_str(),
                                           PrettyTypeOf(o).c_str(),
                                           expected_owner_string.c_str());
      } else {
        ThrowIllegalMonitorStateExceptionF("unlock of monitor owned by '%s' on object of type '%s'"
                                           " on thread '%s",
                                           current_owner_string.c_str(),
                                           PrettyTypeOf(o).c_str(),
                                           expected_owner_string.c_str());
      }
    }
  }
}

bool Monitor::Unlock(Thread* self) {
  DCHECK(self != NULL);
  MutexLock mu(self, monitor_lock_);
  Thread* owner = owner_;
  if (owner == self) {
    // We own the monitor, so nobody else can be in here.
    if (lock_count_ == 0) {
      owner_ = NULL;
      locking_method_ = NULL;
      locking_dex_pc_ = 0;
      // Wake a contender.
      monitor_contenders_.Signal(self);
    } else {
      --lock_count_;
    }
  } else {
    // We don't own this, so we're not allowed to unlock it.
    // The JNI spec says that we should throw IllegalMonitorStateException
    // in this case.
    FailedUnlock(obj_, self, owner, this);
    return false;
  }
  return true;
}

/*
 * Wait on a monitor until timeout, interrupt, or notification.  Used for
 * Object.wait() and (somewhat indirectly) Thread.sleep() and Thread.join().
 *
 * If another thread calls Thread.interrupt(), we throw InterruptedException
 * and return immediately if one of the following are true:
 *  - blocked in wait(), wait(long), or wait(long, int) methods of Object
 *  - blocked in join(), join(long), or join(long, int) methods of Thread
 *  - blocked in sleep(long), or sleep(long, int) methods of Thread
 * Otherwise, we set the "interrupted" flag.
 *
 * Checks to make sure that "ns" is in the range 0-999999
 * (i.e. fractions of a millisecond) and throws the appropriate
 * exception if it isn't.
 *
 * The spec allows "spurious wakeups", and recommends that all code using
 * Object.wait() do so in a loop.  This appears to derive from concerns
 * about pthread_cond_wait() on multiprocessor systems.  Some commentary
 * on the web casts doubt on whether these can/should occur.
 *
 * Since we're allowed to wake up "early", we clamp extremely long durations
 * to return at the end of the 32-bit time epoch.
 */
void Monitor::Wait(Thread* self, int64_t ms, int32_t ns,
                   bool interruptShouldThrow, ThreadState why) {
  DCHECK(self != NULL);
  DCHECK(why == kTimedWaiting || why == kWaiting || why == kSleeping);

  monitor_lock_.Lock(self);

  // Make sure that we hold the lock.
  if (owner_ != self) {
    ThrowIllegalMonitorStateExceptionF("object not locked by thread before wait()");
    monitor_lock_.Unlock(self);
    return;
  }

  // We need to turn a zero-length timed wait into a regular wait because
  // Object.wait(0, 0) is defined as Object.wait(0), which is defined as Object.wait().
  if (why == kTimedWaiting && (ms == 0 && ns == 0)) {
    why = kWaiting;
  }

  // Enforce the timeout range.
  if (ms < 0 || ns < 0 || ns > 999999) {
    ThrowLocation throw_location = self->GetCurrentLocationForThrow();
    self->ThrowNewExceptionF(throw_location, "Ljava/lang/IllegalArgumentException;",
                             "timeout arguments out of range: ms=%" PRId64 " ns=%d", ms, ns);
    monitor_lock_.Unlock(self);
    return;
  }

  /*
   * Add ourselves to the set of threads waiting on this monitor, and
   * release our hold.  We need to let it go even if we're a few levels
   * deep in a recursive lock, and we need to restore that later.
   *
   * We append to the wait set ahead of clearing the count and owner
   * fields so the subroutine can check that the calling thread owns
   * the monitor.  Aside from that, the order of member updates is
   * not order sensitive as we hold the pthread mutex.
   */
  AppendToWaitSet(self);
  ++num_waiters_;
  int prev_lock_count = lock_count_;
  lock_count_ = 0;
  owner_ = NULL;
  mirror::ArtMethod* saved_method = locking_method_;
  locking_method_ = NULL;
  uintptr_t saved_dex_pc = locking_dex_pc_;
  locking_dex_pc_ = 0;

  /*
   * Update thread state. If the GC wakes up, it'll ignore us, knowing
   * that we won't touch any references in this state, and we'll check
   * our suspend mode before we transition out.
   */
  self->TransitionFromRunnableToSuspended(why);

  bool was_interrupted = false;
  {
    // Pseudo-atomically wait on self's wait_cond_ and release the monitor lock.
    MutexLock mu(self, *self->GetWaitMutex());

    // Set wait_monitor_ to the monitor object we will be waiting on. When wait_monitor_ is
    // non-NULL a notifying or interrupting thread must signal the thread's wait_cond_ to wake it
    // up.
    DCHECK(self->GetWaitMonitor() == nullptr);
    self->SetWaitMonitor(this);

    // Release the monitor lock.
    monitor_contenders_.Signal(self);
    monitor_lock_.Unlock(self);

    // Handle the case where the thread was interrupted before we called wait().
    if (self->IsInterruptedLocked()) {
      was_interrupted = true;
    } else {
      // Wait for a notification or a timeout to occur.
      if (why == kWaiting) {
        self->GetWaitConditionVariable()->Wait(self);
      } else {
        DCHECK(why == kTimedWaiting || why == kSleeping) << why;
        self->GetWaitConditionVariable()->TimedWait(self, ms, ns);
      }
      if (self->IsInterruptedLocked()) {
        was_interrupted = true;
      }
      self->SetInterruptedLocked(false);
    }
  }

  // Set self->status back to kRunnable, and self-suspend if needed.
  self->TransitionFromSuspendedToRunnable();

  {
    // We reset the thread's wait_monitor_ field after transitioning back to runnable so
    // that a thread in a waiting/sleeping state has a non-null wait_monitor_ for debugging
    // and diagnostic purposes. (If you reset this earlier, stack dumps will claim that threads
    // are waiting on "null".)
    MutexLock mu(self, *self->GetWaitMutex());
    DCHECK(self->GetWaitMonitor() != nullptr);
    self->SetWaitMonitor(nullptr);
  }

  // Re-acquire the monitor and lock.
  Lock(self);
  monitor_lock_.Lock(self);
  self->GetWaitMutex()->AssertNotHeld(self);

  /*
   * We remove our thread from wait set after restoring the count
   * and owner fields so the subroutine can check that the calling
   * thread owns the monitor. Aside from that, the order of member
   * updates is not order sensitive as we hold the pthread mutex.
   */
  owner_ = self;
  lock_count_ = prev_lock_count;
  locking_method_ = saved_method;
  locking_dex_pc_ = saved_dex_pc;
  --num_waiters_;
  RemoveFromWaitSet(self);

  if (was_interrupted) {
    /*
     * We were interrupted while waiting, or somebody interrupted an
     * un-interruptible thread earlier and we're bailing out immediately.
     *
     * The doc sayeth: "The interrupted status of the current thread is
     * cleared when this exception is thrown."
     */
    {
      MutexLock mu(self, *self->GetWaitMutex());
      self->SetInterruptedLocked(false);
    }
    if (interruptShouldThrow) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      self->ThrowNewException(throw_location, "Ljava/lang/InterruptedException;", NULL);
    }
  }
  monitor_lock_.Unlock(self);
}

void Monitor::Notify(Thread* self) {
  DCHECK(self != NULL);
  MutexLock mu(self, monitor_lock_);
  // Make sure that we hold the lock.
  if (owner_ != self) {
    ThrowIllegalMonitorStateExceptionF("object not locked by thread before notify()");
    return;
  }
  // Signal the first waiting thread in the wait set.
  while (wait_set_ != NULL) {
    Thread* thread = wait_set_;
    wait_set_ = thread->GetWaitNext();
    thread->SetWaitNext(nullptr);

    // Check to see if the thread is still waiting.
    MutexLock mu(self, *thread->GetWaitMutex());
    if (thread->GetWaitMonitor() != nullptr) {
      thread->GetWaitConditionVariable()->Signal(self);
      return;
    }
  }
}

void Monitor::NotifyAll(Thread* self) {
  DCHECK(self != NULL);
  MutexLock mu(self, monitor_lock_);
  // Make sure that we hold the lock.
  if (owner_ != self) {
    ThrowIllegalMonitorStateExceptionF("object not locked by thread before notifyAll()");
    return;
  }
  // Signal all threads in the wait set.
  while (wait_set_ != NULL) {
    Thread* thread = wait_set_;
    wait_set_ = thread->GetWaitNext();
    thread->SetWaitNext(nullptr);
    thread->Notify();
  }
}

bool Monitor::Deflate(Thread* self, mirror::Object* obj) {
  DCHECK(obj != nullptr);
  // Don't need volatile since we only deflate with mutators suspended.
  LockWord lw(obj->GetLockWord(false));
  // If the lock isn't an inflated monitor, then we don't need to deflate anything.
  if (lw.GetState() == LockWord::kFatLocked) {
    Monitor* monitor = lw.FatLockMonitor();
    DCHECK(monitor != nullptr);
    MutexLock mu(self, monitor->monitor_lock_);
    // Can't deflate if we have anybody waiting on the CV.
    if (monitor->num_waiters_ > 0) {
      return false;
    }
    Thread* owner = monitor->owner_;
    if (owner != nullptr) {
      // Can't deflate if we are locked and have a hash code.
      if (monitor->HasHashCode()) {
        return false;
      }
      // Can't deflate if our lock count is too high.
      if (monitor->lock_count_ > LockWord::kThinLockMaxCount) {
        return false;
      }
      // Deflate to a thin lock.
      obj->SetLockWord(LockWord::FromThinLockId(owner->GetThreadId(), monitor->lock_count_), false);
      VLOG(monitor) << "Deflated " << obj << " to thin lock " << owner->GetTid() << " / "
          << monitor->lock_count_;
    } else if (monitor->HasHashCode()) {
      obj->SetLockWord(LockWord::FromHashCode(monitor->GetHashCode()), false);
      VLOG(monitor) << "Deflated " << obj << " to hash monitor " << monitor->GetHashCode();
    } else {
      // No lock and no hash, just put an empty lock word inside the object.
      obj->SetLockWord(LockWord(), false);
      VLOG(monitor) << "Deflated" << obj << " to empty lock word";
    }
    // The monitor is deflated, mark the object as nullptr so that we know to delete it during the
    // next GC.
    monitor->obj_ = nullptr;
  }
  return true;
}

/*
 * Changes the shape of a monitor from thin to fat, preserving the internal lock state. The calling
 * thread must own the lock or the owner must be suspended. There's a race with other threads
 * inflating the lock and so the caller should read the monitor following the call.
 */
void Monitor::Inflate(Thread* self, Thread* owner, mirror::Object* obj, int32_t hash_code) {
  DCHECK(self != NULL);
  DCHECK(obj != NULL);
  // Allocate and acquire a new monitor.
  UniquePtr<Monitor> m(new Monitor(self, owner, obj, hash_code));
  if (m->Install(self)) {
    VLOG(monitor) << "monitor: thread " << owner->GetThreadId()
                    << " created monitor " << m.get() << " for object " << obj;
    Runtime::Current()->GetMonitorList()->Add(m.release());
    CHECK_EQ(obj->GetLockWord(true).GetState(), LockWord::kFatLocked);
  }
}

void Monitor::InflateThinLocked(Thread* self, Handle<mirror::Object>& obj, LockWord lock_word,
                                uint32_t hash_code) {
  DCHECK_EQ(lock_word.GetState(), LockWord::kThinLocked);
  uint32_t owner_thread_id = lock_word.ThinLockOwner();
  if (owner_thread_id == self->GetThreadId()) {
    // We own the monitor, we can easily inflate it.
    Inflate(self, self, obj.Get(), hash_code);
  } else {
    ThreadList* thread_list = Runtime::Current()->GetThreadList();
    // Suspend the owner, inflate. First change to blocked and give up mutator_lock_.
    self->SetMonitorEnterObject(obj.Get());
    bool timed_out;
    Thread* owner;
    {
      ScopedThreadStateChange tsc(self, kBlocked);
      owner = thread_list->SuspendThreadByThreadId(owner_thread_id, false, &timed_out);
    }
    if (owner != nullptr) {
      // We succeeded in suspending the thread, check the lock's status didn't change.
      lock_word = obj->GetLockWord(true);
      if (lock_word.GetState() == LockWord::kThinLocked &&
          lock_word.ThinLockOwner() == owner_thread_id) {
        // Go ahead and inflate the lock.
        Inflate(self, owner, obj.Get(), hash_code);
      }
      thread_list->Resume(owner, false);
    }
    self->SetMonitorEnterObject(nullptr);
  }
}

// Fool annotalysis into thinking that the lock on obj is acquired.
static mirror::Object* FakeLock(mirror::Object* obj)
    EXCLUSIVE_LOCK_FUNCTION(obj) NO_THREAD_SAFETY_ANALYSIS {
  return obj;
}

// Fool annotalysis into thinking that the lock on obj is release.
static mirror::Object* FakeUnlock(mirror::Object* obj)
    UNLOCK_FUNCTION(obj) NO_THREAD_SAFETY_ANALYSIS {
  return obj;
}

mirror::Object* Monitor::MonitorEnter(Thread* self, mirror::Object* obj) {
  DCHECK(self != NULL);
  DCHECK(obj != NULL);
  obj = FakeLock(obj);
  uint32_t thread_id = self->GetThreadId();
  size_t contention_count = 0;
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> h_obj(hs.NewHandle(obj));
  while (true) {
    LockWord lock_word = h_obj->GetLockWord(true);
    switch (lock_word.GetState()) {
      case LockWord::kUnlocked: {
        LockWord thin_locked(LockWord::FromThinLockId(thread_id, 0));
        if (h_obj->CasLockWord(lock_word, thin_locked)) {
          QuasiAtomic::MembarLoadLoad();
          return h_obj.Get();  // Success!
        }
        continue;  // Go again.
      }
      case LockWord::kThinLocked: {
        uint32_t owner_thread_id = lock_word.ThinLockOwner();
        if (owner_thread_id == thread_id) {
          // We own the lock, increase the recursion count.
          uint32_t new_count = lock_word.ThinLockCount() + 1;
          if (LIKELY(new_count <= LockWord::kThinLockMaxCount)) {
            LockWord thin_locked(LockWord::FromThinLockId(thread_id, new_count));
            h_obj->SetLockWord(thin_locked, true);
            return h_obj.Get();  // Success!
          } else {
            // We'd overflow the recursion count, so inflate the monitor.
            InflateThinLocked(self, h_obj, lock_word, 0);
          }
        } else {
          // Contention.
          contention_count++;
          Runtime* runtime = Runtime::Current();
          if (contention_count <= runtime->GetMaxSpinsBeforeThinkLockInflation()) {
            NanoSleep(1000);  // Sleep for 1us and re-attempt.
          } else {
            contention_count = 0;
            InflateThinLocked(self, h_obj, lock_word, 0);
          }
        }
        continue;  // Start from the beginning.
      }
      case LockWord::kFatLocked: {
        Monitor* mon = lock_word.FatLockMonitor();
        mon->Lock(self);
        return h_obj.Get();  // Success!
      }
      case LockWord::kHashCode:
        // Inflate with the existing hashcode.
        Inflate(self, nullptr, h_obj.Get(), lock_word.GetHashCode());
        continue;  // Start from the beginning.
      default: {
        LOG(FATAL) << "Invalid monitor state " << lock_word.GetState();
        return h_obj.Get();
      }
    }
  }
}

bool Monitor::MonitorExit(Thread* self, mirror::Object* obj) {
  DCHECK(self != NULL);
  DCHECK(obj != NULL);
  obj = FakeUnlock(obj);
  LockWord lock_word = obj->GetLockWord(true);
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> h_obj(hs.NewHandle(obj));
  switch (lock_word.GetState()) {
    case LockWord::kHashCode:
      // Fall-through.
    case LockWord::kUnlocked:
      FailedUnlock(h_obj.Get(), self, nullptr, nullptr);
      return false;  // Failure.
    case LockWord::kThinLocked: {
      uint32_t thread_id = self->GetThreadId();
      uint32_t owner_thread_id = lock_word.ThinLockOwner();
      if (owner_thread_id != thread_id) {
        // TODO: there's a race here with the owner dying while we unlock.
        Thread* owner =
            Runtime::Current()->GetThreadList()->FindThreadByThreadId(lock_word.ThinLockOwner());
        FailedUnlock(h_obj.Get(), self, owner, nullptr);
        return false;  // Failure.
      } else {
        // We own the lock, decrease the recursion count.
        if (lock_word.ThinLockCount() != 0) {
          uint32_t new_count = lock_word.ThinLockCount() - 1;
          LockWord thin_locked(LockWord::FromThinLockId(thread_id, new_count));
          h_obj->SetLockWord(thin_locked, true);
        } else {
          h_obj->SetLockWord(LockWord(), true);
        }
        return true;  // Success!
      }
    }
    case LockWord::kFatLocked: {
      Monitor* mon = lock_word.FatLockMonitor();
      return mon->Unlock(self);
    }
    default: {
      LOG(FATAL) << "Invalid monitor state " << lock_word.GetState();
      return false;
    }
  }
}

/*
 * Object.wait().  Also called for class init.
 */
void Monitor::Wait(Thread* self, mirror::Object *obj, int64_t ms, int32_t ns,
                   bool interruptShouldThrow, ThreadState why) {
  DCHECK(self != nullptr);
  DCHECK(obj != nullptr);
  LockWord lock_word = obj->GetLockWord(true);
  switch (lock_word.GetState()) {
    case LockWord::kHashCode:
      // Fall-through.
    case LockWord::kUnlocked:
      ThrowIllegalMonitorStateExceptionF("object not locked by thread before wait()");
      return;  // Failure.
    case LockWord::kThinLocked: {
      uint32_t thread_id = self->GetThreadId();
      uint32_t owner_thread_id = lock_word.ThinLockOwner();
      if (owner_thread_id != thread_id) {
        ThrowIllegalMonitorStateExceptionF("object not locked by thread before wait()");
        return;  // Failure.
      } else {
        // We own the lock, inflate to enqueue ourself on the Monitor.
        Inflate(self, self, obj, 0);
        lock_word = obj->GetLockWord(true);
      }
      break;
    }
    case LockWord::kFatLocked:
      break;  // Already set for a wait.
    default: {
      LOG(FATAL) << "Invalid monitor state " << lock_word.GetState();
      return;
    }
  }
  Monitor* mon = lock_word.FatLockMonitor();
  mon->Wait(self, ms, ns, interruptShouldThrow, why);
}

void Monitor::DoNotify(Thread* self, mirror::Object* obj, bool notify_all) {
  DCHECK(self != nullptr);
  DCHECK(obj != nullptr);
  LockWord lock_word = obj->GetLockWord(true);
  switch (lock_word.GetState()) {
    case LockWord::kHashCode:
      // Fall-through.
    case LockWord::kUnlocked:
      ThrowIllegalMonitorStateExceptionF("object not locked by thread before notify()");
      return;  // Failure.
    case LockWord::kThinLocked: {
      uint32_t thread_id = self->GetThreadId();
      uint32_t owner_thread_id = lock_word.ThinLockOwner();
      if (owner_thread_id != thread_id) {
        ThrowIllegalMonitorStateExceptionF("object not locked by thread before notify()");
        return;  // Failure.
      } else {
        // We own the lock but there's no Monitor and therefore no waiters.
        return;  // Success.
      }
    }
    case LockWord::kFatLocked: {
      Monitor* mon = lock_word.FatLockMonitor();
      if (notify_all) {
        mon->NotifyAll(self);
      } else {
        mon->Notify(self);
      }
      return;  // Success.
    }
    default: {
      LOG(FATAL) << "Invalid monitor state " << lock_word.GetState();
      return;
    }
  }
}

uint32_t Monitor::GetLockOwnerThreadId(mirror::Object* obj) {
  DCHECK(obj != nullptr);
  LockWord lock_word = obj->GetLockWord(true);
  switch (lock_word.GetState()) {
    case LockWord::kHashCode:
      // Fall-through.
    case LockWord::kUnlocked:
      return ThreadList::kInvalidThreadId;
    case LockWord::kThinLocked:
      return lock_word.ThinLockOwner();
    case LockWord::kFatLocked: {
      Monitor* mon = lock_word.FatLockMonitor();
      return mon->GetOwnerThreadId();
    }
    default: {
      LOG(FATAL) << "Unreachable";
      return ThreadList::kInvalidThreadId;
    }
  }
}

void Monitor::DescribeWait(std::ostream& os, const Thread* thread) {
  // Determine the wait message and object we're waiting or blocked upon.
  mirror::Object* pretty_object = nullptr;
  const char* wait_message = nullptr;
  uint32_t lock_owner = ThreadList::kInvalidThreadId;
  ThreadState state = thread->GetState();
  if (state == kWaiting || state == kTimedWaiting || state == kSleeping) {
    wait_message = (state == kSleeping) ? "  - sleeping on " : "  - waiting on ";
    Thread* self = Thread::Current();
    MutexLock mu(self, *thread->GetWaitMutex());
    Monitor* monitor = thread->GetWaitMonitor();
    if (monitor != nullptr) {
      pretty_object = monitor->obj_;
    }
  } else if (state == kBlocked) {
    wait_message = "  - waiting to lock ";
    pretty_object = thread->GetMonitorEnterObject();
    if (pretty_object != nullptr) {
      lock_owner = pretty_object->GetLockOwnerThreadId();
    }
  }

  if (wait_message != nullptr) {
    if (pretty_object == nullptr) {
      os << wait_message << "an unknown object";
    } else {
      if ((pretty_object->GetLockWord(true).GetState() == LockWord::kThinLocked) &&
          Locks::mutator_lock_->IsExclusiveHeld(Thread::Current())) {
        // Getting the identity hashcode here would result in lock inflation and suspension of the
        // current thread, which isn't safe if this is the only runnable thread.
        os << wait_message << StringPrintf("<@addr=0x%" PRIxPTR "> (a %s)",
                                           reinterpret_cast<intptr_t>(pretty_object),
                                           PrettyTypeOf(pretty_object).c_str());
      } else {
        // - waiting on <0x6008c468> (a java.lang.Class<java.lang.ref.ReferenceQueue>)
        os << wait_message << StringPrintf("<0x%08x> (a %s)", pretty_object->IdentityHashCode(),
                                           PrettyTypeOf(pretty_object).c_str());
      }
    }
    // - waiting to lock <0x613f83d8> (a java.lang.Object) held by thread 5
    if (lock_owner != ThreadList::kInvalidThreadId) {
      os << " held by thread " << lock_owner;
    }
    os << "\n";
  }
}

mirror::Object* Monitor::GetContendedMonitor(Thread* thread) {
  // This is used to implement JDWP's ThreadReference.CurrentContendedMonitor, and has a bizarre
  // definition of contended that includes a monitor a thread is trying to enter...
  mirror::Object* result = thread->GetMonitorEnterObject();
  if (result == NULL) {
    // ...but also a monitor that the thread is waiting on.
    MutexLock mu(Thread::Current(), *thread->GetWaitMutex());
    Monitor* monitor = thread->GetWaitMonitor();
    if (monitor != NULL) {
      result = monitor->GetObject();
    }
  }
  return result;
}

void Monitor::VisitLocks(StackVisitor* stack_visitor, void (*callback)(mirror::Object*, void*),
                         void* callback_context) {
  mirror::ArtMethod* m = stack_visitor->GetMethod();
  CHECK(m != NULL);

  // Native methods are an easy special case.
  // TODO: use the JNI implementation's table of explicit MonitorEnter calls and dump those too.
  if (m->IsNative()) {
    if (m->IsSynchronized()) {
      mirror::Object* jni_this = stack_visitor->GetCurrentHandleScope()->GetReference(0);
      callback(jni_this, callback_context);
    }
    return;
  }

  // Proxy methods should not be synchronized.
  if (m->IsProxyMethod()) {
    CHECK(!m->IsSynchronized());
    return;
  }

  // <clinit> is another special case. The runtime holds the class lock while calling <clinit>.
  MethodHelper mh(m);
  if (mh.IsClassInitializer()) {
    callback(m->GetDeclaringClass(), callback_context);
    // Fall through because there might be synchronization in the user code too.
  }

  // Is there any reason to believe there's any synchronization in this method?
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  CHECK(code_item != NULL) << PrettyMethod(m);
  if (code_item->tries_size_ == 0) {
    return;  // No "tries" implies no synchronization, so no held locks to report.
  }

  // Ask the verifier for the dex pcs of all the monitor-enter instructions corresponding to
  // the locks held in this stack frame.
  std::vector<uint32_t> monitor_enter_dex_pcs;
  verifier::MethodVerifier::FindLocksAtDexPc(m, stack_visitor->GetDexPc(), monitor_enter_dex_pcs);
  if (monitor_enter_dex_pcs.empty()) {
    return;
  }

  for (size_t i = 0; i < monitor_enter_dex_pcs.size(); ++i) {
    // The verifier works in terms of the dex pcs of the monitor-enter instructions.
    // We want the registers used by those instructions (so we can read the values out of them).
    uint32_t dex_pc = monitor_enter_dex_pcs[i];
    uint16_t monitor_enter_instruction = code_item->insns_[dex_pc];

    // Quick sanity check.
    if ((monitor_enter_instruction & 0xff) != Instruction::MONITOR_ENTER) {
      LOG(FATAL) << "expected monitor-enter @" << dex_pc << "; was "
                 << reinterpret_cast<void*>(monitor_enter_instruction);
    }

    uint16_t monitor_register = ((monitor_enter_instruction >> 8) & 0xff);
    mirror::Object* o = reinterpret_cast<mirror::Object*>(stack_visitor->GetVReg(m, monitor_register,
                                                                                 kReferenceVReg));
    callback(o, callback_context);
  }
}

bool Monitor::IsValidLockWord(LockWord lock_word) {
  switch (lock_word.GetState()) {
    case LockWord::kUnlocked:
      // Nothing to check.
      return true;
    case LockWord::kThinLocked:
      // Basic sanity check of owner.
      return lock_word.ThinLockOwner() != ThreadList::kInvalidThreadId;
    case LockWord::kFatLocked: {
      // Check the  monitor appears in the monitor list.
      Monitor* mon = lock_word.FatLockMonitor();
      MonitorList* list = Runtime::Current()->GetMonitorList();
      MutexLock mu(Thread::Current(), list->monitor_list_lock_);
      for (Monitor* list_mon : list->list_) {
        if (mon == list_mon) {
          return true;  // Found our monitor.
        }
      }
      return false;  // Fail - unowned monitor in an object.
    }
    case LockWord::kHashCode:
      return true;
    default:
      LOG(FATAL) << "Unreachable";
      return false;
  }
}

bool Monitor::IsLocked() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  MutexLock mu(Thread::Current(), monitor_lock_);
  return owner_ != nullptr;
}

void Monitor::TranslateLocation(mirror::ArtMethod* method, uint32_t dex_pc,
                                const char** source_file, uint32_t* line_number) const {
  // If method is null, location is unknown
  if (method == NULL) {
    *source_file = "";
    *line_number = 0;
    return;
  }
  MethodHelper mh(method);
  *source_file = mh.GetDeclaringClassSourceFile();
  if (*source_file == NULL) {
    *source_file = "";
  }
  *line_number = mh.GetLineNumFromDexPC(dex_pc);
}

uint32_t Monitor::GetOwnerThreadId() {
  MutexLock mu(Thread::Current(), monitor_lock_);
  Thread* owner = owner_;
  if (owner != NULL) {
    return owner->GetThreadId();
  } else {
    return ThreadList::kInvalidThreadId;
  }
}

MonitorList::MonitorList()
    : allow_new_monitors_(true), monitor_list_lock_("MonitorList lock", kMonitorListLock),
      monitor_add_condition_("MonitorList disallow condition", monitor_list_lock_) {
}

MonitorList::~MonitorList() {
  MutexLock mu(Thread::Current(), monitor_list_lock_);
  STLDeleteElements(&list_);
}

void MonitorList::DisallowNewMonitors() {
  MutexLock mu(Thread::Current(), monitor_list_lock_);
  allow_new_monitors_ = false;
}

void MonitorList::AllowNewMonitors() {
  Thread* self = Thread::Current();
  MutexLock mu(self, monitor_list_lock_);
  allow_new_monitors_ = true;
  monitor_add_condition_.Broadcast(self);
}

void MonitorList::Add(Monitor* m) {
  Thread* self = Thread::Current();
  MutexLock mu(self, monitor_list_lock_);
  while (UNLIKELY(!allow_new_monitors_)) {
    monitor_add_condition_.WaitHoldingLocks(self);
  }
  list_.push_front(m);
}

void MonitorList::SweepMonitorList(IsMarkedCallback* callback, void* arg) {
  MutexLock mu(Thread::Current(), monitor_list_lock_);
  for (auto it = list_.begin(); it != list_.end(); ) {
    Monitor* m = *it;
    mirror::Object* obj = m->GetObject();
    // The object of a monitor can be null if we have deflated it.
    mirror::Object* new_obj = obj != nullptr ? callback(obj, arg) : nullptr;
    if (new_obj == nullptr) {
      VLOG(monitor) << "freeing monitor " << m << " belonging to unmarked object "
                    << m->GetObject();
      delete m;
      it = list_.erase(it);
    } else {
      m->SetObject(new_obj);
      ++it;
    }
  }
}

static mirror::Object* MonitorDeflateCallback(mirror::Object* object, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (Monitor::Deflate(reinterpret_cast<Thread*>(arg), object)) {
    DCHECK_NE(object->GetLockWord(true).GetState(), LockWord::kFatLocked);
    // If we deflated, return nullptr so that the monitor gets removed from the array.
    return nullptr;
  }
  return object;  // Monitor was not deflated.
}

void MonitorList::DeflateMonitors() {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  SweepMonitorList(MonitorDeflateCallback, reinterpret_cast<Thread*>(self));
}

MonitorInfo::MonitorInfo(mirror::Object* obj) : owner_(NULL), entry_count_(0) {
  DCHECK(obj != nullptr);
  LockWord lock_word = obj->GetLockWord(true);
  switch (lock_word.GetState()) {
    case LockWord::kUnlocked:
      // Fall-through.
    case LockWord::kForwardingAddress:
      // Fall-through.
    case LockWord::kHashCode:
      break;
    case LockWord::kThinLocked:
      owner_ = Runtime::Current()->GetThreadList()->FindThreadByThreadId(lock_word.ThinLockOwner());
      entry_count_ = 1 + lock_word.ThinLockCount();
      // Thin locks have no waiters.
      break;
    case LockWord::kFatLocked: {
      Monitor* mon = lock_word.FatLockMonitor();
      owner_ = mon->owner_;
      entry_count_ = 1 + mon->lock_count_;
      for (Thread* waiter = mon->wait_set_; waiter != NULL; waiter = waiter->GetWaitNext()) {
        waiters_.push_back(waiter);
      }
      break;
    }
  }
}

}  // namespace art
