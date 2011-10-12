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

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "mutex.h"
#include "object.h"
#include "stl_util.h"
#include "thread.h"
#include "thread_list.h"

namespace art {

/*
 * Every Object has a monitor associated with it, but not every Object is
 * actually locked.  Even the ones that are locked do not need a
 * full-fledged monitor until a) there is actual contention or b) wait()
 * is called on the Object.
 *
 * For Android, we have implemented a scheme similar to the one described
 * in Bacon et al.'s "Thin locks: featherweight synchronization for Java"
 * (ACM 1998).  Things are even easier for us, though, because we have
 * a full 32 bits to work with.
 *
 * The two states of an Object's lock are referred to as "thin" and
 * "fat".  A lock may transition from the "thin" state to the "fat"
 * state and this transition is referred to as inflation.  Once a lock
 * has been inflated it remains in the "fat" state indefinitely.
 *
 * The lock value itself is stored in Object.lock.  The LSB of the
 * lock encodes its state.  When cleared, the lock is in the "thin"
 * state and its bits are formatted as follows:
 *
 *    [31 ---- 19] [18 ---- 3] [2 ---- 1] [0]
 *     lock count   thread id  hash state  0
 *
 * When set, the lock is in the "fat" state and its bits are formatted
 * as follows:
 *
 *    [31 ---- 3] [2 ---- 1] [0]
 *      pointer   hash state  1
 *
 * For an in-depth description of the mechanics of thin-vs-fat locking,
 * read the paper referred to above.
 *
 * Monitors provide:
 *  - mutually exclusive access to resources
 *  - a way for multiple threads to wait for notification
 *
 * In effect, they fill the role of both mutexes and condition variables.
 *
 * Only one thread can own the monitor at any time.  There may be several
 * threads waiting on it (the wait call unlocks it).  One or more waiting
 * threads may be getting interrupted or notified at any given time.
 *
 * TODO: the various members of monitor are not SMP-safe.
 */


/*
 * Monitor accessor.  Extracts a monitor structure pointer from a fat
 * lock.  Performs no error checking.
 */
#define LW_MONITOR(x) \
  ((Monitor*)((x) & ~((LW_HASH_STATE_MASK << LW_HASH_STATE_SHIFT) | LW_SHAPE_MASK)))

/*
 * Lock recursion count field.  Contains a count of the number of times
 * a lock has been recursively acquired.
 */
#define LW_LOCK_COUNT_MASK 0x1fff
#define LW_LOCK_COUNT_SHIFT 19
#define LW_LOCK_COUNT(x) (((x) >> LW_LOCK_COUNT_SHIFT) & LW_LOCK_COUNT_MASK)

bool Monitor::is_verbose_ = false;

bool Monitor::IsVerbose() {
  return is_verbose_;
}

void Monitor::SetVerbose(bool is_verbose) {
  is_verbose_ = is_verbose;
}

Monitor::Monitor(Object* obj)
    : owner_(NULL),
      lock_count_(0),
      obj_(obj),
      wait_set_(NULL),
      lock_("a monitor lock"),
      owner_filename_(NULL),
      owner_line_number_(0) {
}

Monitor::~Monitor() {
  DCHECK(obj_ != NULL);
  DCHECK_EQ(LW_SHAPE(*obj_->GetRawLockWordAddress()), LW_SHAPE_FAT);

#ifndef NDEBUG
  /* This lock is associated with an object
   * that's being swept.  The only possible way
   * anyone could be holding this lock would be
   * if some JNI code locked but didn't unlock
   * the object, in which case we've got some bad
   * native code somewhere.
   */
  DCHECK(lock_.TryLock());
  lock_.Unlock();
#endif
}

/*
 * Links a thread into a monitor's wait set.  The monitor lock must be
 * held by the caller of this routine.
 */
void Monitor::AppendToWaitSet(Thread* thread) {
  DCHECK(owner_ == Thread::Current());
  DCHECK(thread != NULL);
  DCHECK(thread->wait_next_ == NULL) << thread->wait_next_;
  if (wait_set_ == NULL) {
    wait_set_ = thread;
    return;
  }

  // push_back.
  Thread* t = wait_set_;
  while (t->wait_next_ != NULL) {
    t = t->wait_next_;
  }
  t->wait_next_ = thread;
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
    wait_set_ = thread->wait_next_;
    thread->wait_next_ = NULL;
    return;
  }

  Thread* t = wait_set_;
  while (t->wait_next_ != NULL) {
    if (t->wait_next_ == thread) {
      t->wait_next_ = thread->wait_next_;
      thread->wait_next_ = NULL;
      return;
    }
    t = t->wait_next_;
  }
}

Object* Monitor::GetObject() {
  return obj_;
}

/*
static char *logWriteInt(char *dst, int value) {
  *dst++ = EVENT_TYPE_INT;
  set4LE((uint8_t *)dst, value);
  return dst + 4;
}

static char *logWriteString(char *dst, const char *value, size_t len) {
  *dst++ = EVENT_TYPE_STRING;
  len = len < 32 ? len : 32;
  set4LE((uint8_t *)dst, len);
  dst += 4;
  memcpy(dst, value, len);
  return dst + len;
}

#define EVENT_LOG_TAG_dvm_lock_sample 20003

static void logContentionEvent(Thread *self, uint32_t waitMs, uint32_t samplePercent,
                               const char *ownerFileName, uint32_t ownerLineNumber)
{
    const StackSaveArea *saveArea;
    const Method *meth;
    uint32_t relativePc;
    char eventBuffer[174];
    const char *fileName;
    char procName[33];
    char *cp;
    size_t len;
    int fd;

    saveArea = SAVEAREA_FROM_FP(self->interpSave.curFrame);
    meth = saveArea->method;
    cp = eventBuffer;

    // Emit the event list length, 1 byte.
    *cp++ = 9;

    // Emit the process name, <= 37 bytes.
    fd = open("/proc/self/cmdline", O_RDONLY);
    memset(procName, 0, sizeof(procName));
    read(fd, procName, sizeof(procName) - 1);
    close(fd);
    len = strlen(procName);
    cp = logWriteString(cp, procName, len);

    // Emit the sensitive thread ("main thread") status, 5 bytes.
    bool isSensitive = false;
    if (gDvm.isSensitiveThreadHook != NULL) {
        isSensitive = gDvm.isSensitiveThreadHook();
    }
    cp = logWriteInt(cp, isSensitive);

    // Emit self thread name string, <= 37 bytes.
    std::string selfName = dvmGetThreadName(self);
    cp = logWriteString(cp, selfName.c_str(), selfName.size());

    // Emit the wait time, 5 bytes.
    cp = logWriteInt(cp, waitMs);

    // Emit the source code file name, <= 37 bytes.
    fileName = dvmGetMethodSourceFile(meth);
    if (fileName == NULL) fileName = "";
    cp = logWriteString(cp, fileName, strlen(fileName));

    // Emit the source code line number, 5 bytes.
    relativePc = saveArea->xtra.currentPc - saveArea->method->insns;
    cp = logWriteInt(cp, dvmLineNumFromPC(meth, relativePc));

    // Emit the lock owner source code file name, <= 37 bytes.
    if (ownerFileName == NULL) {
        ownerFileName = "";
    } else if (strcmp(fileName, ownerFileName) == 0) {
        // Common case, so save on log space.
        ownerFileName = "-";
    }
    cp = logWriteString(cp, ownerFileName, strlen(ownerFileName));

    // Emit the source code line number, 5 bytes.
    cp = logWriteInt(cp, ownerLineNumber);

    // Emit the sample percentage, 5 bytes.
    cp = logWriteInt(cp, samplePercent);

    assert((size_t)(cp - eventBuffer) <= sizeof(eventBuffer));
    android_btWriteLog(EVENT_LOG_TAG_dvm_lock_sample,
                       EVENT_TYPE_LIST,
                       eventBuffer,
                       (size_t)(cp - eventBuffer));
}
*/

void Monitor::Lock(Thread* self) {
//  uint32_t waitThreshold, samplePercent;
//  uint64_t waitStart, waitEnd, waitMs;

  if (owner_ == self) {
    lock_count_++;
    return;
  }
  if (!lock_.TryLock()) {
    {
      ScopedThreadStateChange tsc(self, Thread::kBlocked);
//      waitThreshold = gDvm.lockProfThreshold;
//      if (waitThreshold) {
//        waitStart = dvmGetRelativeTimeUsec();
//      }
//      const char* currentOwnerFileName = mon->ownerFileName;
//      uint32_t currentOwnerLineNumber = mon->ownerLineNumber;

      lock_.Lock();
//      if (waitThreshold) {
//        waitEnd = dvmGetRelativeTimeUsec();
//      }
    }
//    if (waitThreshold) {
//      waitMs = (waitEnd - waitStart) / 1000;
//      if (waitMs >= waitThreshold) {
//        samplePercent = 100;
//      } else {
//        samplePercent = 100 * waitMs / waitThreshold;
//      }
//      if (samplePercent != 0 && ((uint32_t)rand() % 100 < samplePercent)) {
//        logContentionEvent(self, waitMs, samplePercent, currentOwnerFileName, currentOwnerLineNumber);
//      }
//    }
  }
  owner_ = self;
  DCHECK_EQ(lock_count_, 0);

  // When debugging, save the current monitor holder for future
  // acquisition failures to use in sampled logging.
//  if (gDvm.lockProfThreshold > 0) {
//    const StackSaveArea *saveArea;
//    const Method *meth;
//    mon->ownerLineNumber = 0;
//    if (self->interpSave.curFrame == NULL) {
//      mon->ownerFileName = "no_frame";
//    } else if ((saveArea = SAVEAREA_FROM_FP(self->interpSave.curFrame)) == NULL) {
//      mon->ownerFileName = "no_save_area";
//    } else if ((meth = saveArea->method) == NULL) {
//      mon->ownerFileName = "no_method";
//    } else {
//      uint32_t relativePc = saveArea->xtra.currentPc - saveArea->method->insns;
//      mon->ownerFileName = (char*) dvmGetMethodSourceFile(meth);
//      if (mon->ownerFileName == NULL) {
//        mon->ownerFileName = "no_method_file";
//      } else {
//        mon->ownerLineNumber = dvmLineNumFromPC(meth, relativePc);
//      }
//    }
//  }
}

void ThrowIllegalMonitorStateException(const char* msg) {
  Thread::Current()->ThrowNewException("Ljava/lang/IllegalMonitorStateException;", msg);
}

bool Monitor::Unlock(Thread* self) {
  DCHECK(self != NULL);
  if (owner_ == self) {
    // We own the monitor, so nobody else can be in here.
    if (lock_count_ == 0) {
      owner_ = NULL;
      owner_filename_ = "unlocked";
      owner_line_number_ = 0;
      lock_.Unlock();
    } else {
      --lock_count_;
    }
  } else {
    // We don't own this, so we're not allowed to unlock it.
    // The JNI spec says that we should throw IllegalMonitorStateException
    // in this case.
    ThrowIllegalMonitorStateException("unlock of unowned monitor");
    return false;
  }
  return true;
}

/*
 * Converts the given relative waiting time into an absolute time.
 */
void ToAbsoluteTime(int64_t ms, int32_t ns, struct timespec *ts) {
  int64_t endSec;

#ifdef HAVE_TIMEDWAIT_MONOTONIC
  clock_gettime(CLOCK_MONOTONIC, ts);
#else
  {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000;
  }
#endif
  endSec = ts->tv_sec + ms / 1000;
  if (endSec >= 0x7fffffff) {
    LOG(INFO) << "Note: end time exceeds epoch";
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

int dvmRelativeCondWait(pthread_cond_t* cond, pthread_mutex_t* mutex, int64_t ms, int32_t ns) {
  struct timespec ts;
  ToAbsoluteTime(ms, ns, &ts);
#if defined(HAVE_TIMEDWAIT_MONOTONIC)
  int rc = pthread_cond_timedwait_monotonic(cond, mutex, &ts);
#else
  int rc = pthread_cond_timedwait(cond, mutex, &ts);
#endif
  DCHECK(rc == 0 || rc == ETIMEDOUT);
  return rc;
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
void Monitor::Wait(Thread* self, int64_t ms, int32_t ns, bool interruptShouldThrow) {
  DCHECK(self != NULL);

  // Make sure that we hold the lock.
  if (owner_ != self) {
    ThrowIllegalMonitorStateException("object not locked by thread before wait()");
    return;
  }

  // Enforce the timeout range.
  if (ms < 0 || ns < 0 || ns > 999999) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
        "timeout arguments out of range: ms=%lld ns=%d", ms, ns);
    return;
  }

  // Compute absolute wakeup time, if necessary.
  struct timespec ts;
  bool timed = false;
  if (ms != 0 || ns != 0) {
    ToAbsoluteTime(ms, ns, &ts);
    timed = true;
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
  int prevLockCount = lock_count_;
  lock_count_ = 0;
  owner_ = NULL;
  const char* savedFileName = owner_filename_;
  owner_filename_ = NULL;
  uint32_t savedLineNumber = owner_line_number_;
  owner_line_number_ = 0;

  /*
   * Update thread status.  If the GC wakes up, it'll ignore us, knowing
   * that we won't touch any references in this state, and we'll check
   * our suspend mode before we transition out.
   */
  if (timed) {
    self->SetState(Thread::kTimedWaiting);
  } else {
    self->SetState(Thread::kWaiting);
  }

  self->wait_mutex_->Lock();

  /*
   * Set wait_monitor_ to the monitor object we will be waiting on.
   * When wait_monitor_ is non-NULL a notifying or interrupting thread
   * must signal the thread's wait_cond_ to wake it up.
   */
  DCHECK(self->wait_monitor_ == NULL);
  self->wait_monitor_ = this;

  /*
   * Handle the case where the thread was interrupted before we called
   * wait().
   */
  bool wasInterrupted = false;
  if (self->interrupted_) {
    wasInterrupted = true;
    self->wait_monitor_ = NULL;
    self->wait_mutex_->Unlock();
    goto done;
  }

  /*
   * Release the monitor lock and wait for a notification or
   * a timeout to occur.
   */
  lock_.Unlock();

  if (!timed) {
    self->wait_cond_->Wait(*self->wait_mutex_);
  } else {
    self->wait_cond_->TimedWait(*self->wait_mutex_, ts);
  }
  if (self->interrupted_) {
    wasInterrupted = true;
  }

  self->interrupted_ = false;
  self->wait_monitor_ = NULL;
  self->wait_mutex_->Unlock();

  // Reacquire the monitor lock.
  Lock(self);

done:
  /*
   * We remove our thread from wait set after restoring the count
   * and owner fields so the subroutine can check that the calling
   * thread owns the monitor. Aside from that, the order of member
   * updates is not order sensitive as we hold the pthread mutex.
   */
  owner_ = self;
  lock_count_ = prevLockCount;
  owner_filename_ = savedFileName;
  owner_line_number_ = savedLineNumber;
  RemoveFromWaitSet(self);

  /* set self->status back to Thread::kRunnable, and self-suspend if needed */
  self->SetState(Thread::kRunnable);

  if (wasInterrupted) {
    /*
     * We were interrupted while waiting, or somebody interrupted an
     * un-interruptible thread earlier and we're bailing out immediately.
     *
     * The doc sayeth: "The interrupted status of the current thread is
     * cleared when this exception is thrown."
     */
    self->interrupted_ = false;
    if (interruptShouldThrow) {
      Thread::Current()->ThrowNewException("Ljava/lang/InterruptedException;", NULL);
    }
  }
}

void Monitor::Notify(Thread* self) {
  DCHECK(self != NULL);

  // Make sure that we hold the lock.
  if (owner_ != self) {
    ThrowIllegalMonitorStateException("object not locked by thread before notify()");
    return;
  }
  // Signal the first waiting thread in the wait set.
  while (wait_set_ != NULL) {
    Thread* thread = wait_set_;
    wait_set_ = thread->wait_next_;
    thread->wait_next_ = NULL;

    // Check to see if the thread is still waiting.
    MutexLock mu(*thread->wait_mutex_);
    if (thread->wait_monitor_ != NULL) {
      thread->wait_cond_->Signal();
      return;
    }
  }
}

void Monitor::NotifyAll(Thread* self) {
  DCHECK(self != NULL);

  // Make sure that we hold the lock.
  if (owner_ != self) {
    ThrowIllegalMonitorStateException("object not locked by thread before notifyAll()");
    return;
  }
  // Signal all threads in the wait set.
  while (wait_set_ != NULL) {
    Thread* thread = wait_set_;
    wait_set_ = thread->wait_next_;
    thread->wait_next_ = NULL;
    thread->Notify();
  }
}

/*
 * Changes the shape of a monitor from thin to fat, preserving the
 * internal lock state. The calling thread must own the lock.
 */
void Monitor::Inflate(Thread* self, Object* obj) {
  DCHECK(self != NULL);
  DCHECK(obj != NULL);
  DCHECK_EQ(LW_SHAPE(*obj->GetRawLockWordAddress()), LW_SHAPE_THIN);
  DCHECK_EQ(LW_LOCK_OWNER(*obj->GetRawLockWordAddress()), static_cast<int32_t>(self->thin_lock_id_));

  // Allocate and acquire a new monitor.
  Monitor* m = new Monitor(obj);
  if (is_verbose_) {
    LOG(INFO) << "monitor: created monitor " << m << " for object " << obj;
  }
  Runtime::Current()->GetMonitorList()->Add(m);
  m->Lock(self);
  // Propagate the lock state.
  uint32_t thin = *obj->GetRawLockWordAddress();
  m->lock_count_ = LW_LOCK_COUNT(thin);
  thin &= LW_HASH_STATE_MASK << LW_HASH_STATE_SHIFT;
  thin |= reinterpret_cast<uint32_t>(m) | LW_SHAPE_FAT;
  // Publish the updated lock word.
  android_atomic_release_store(thin, obj->GetRawLockWordAddress());
}

void Monitor::MonitorEnter(Thread* self, Object* obj) {
  volatile int32_t* thinp = obj->GetRawLockWordAddress();
  struct timespec tm;
  long sleepDelayNs;
  long minSleepDelayNs = 1000000;  /* 1 millisecond */
  long maxSleepDelayNs = 1000000000;  /* 1 second */
  uint32_t thin, newThin, threadId;

  DCHECK(self != NULL);
  DCHECK(obj != NULL);
  threadId = self->thin_lock_id_;
retry:
  thin = *thinp;
  if (LW_SHAPE(thin) == LW_SHAPE_THIN) {
    /*
     * The lock is a thin lock.  The owner field is used to
     * determine the acquire method, ordered by cost.
     */
    if (LW_LOCK_OWNER(thin) == threadId) {
      /*
       * The calling thread owns the lock.  Increment the
       * value of the recursion count field.
       */
      *thinp += 1 << LW_LOCK_COUNT_SHIFT;
      if (LW_LOCK_COUNT(*thinp) == LW_LOCK_COUNT_MASK) {
        /*
         * The reacquisition limit has been reached.  Inflate
         * the lock so the next acquire will not overflow the
         * recursion count field.
         */
        Inflate(self, obj);
      }
    } else if (LW_LOCK_OWNER(thin) == 0) {
      /*
       * The lock is unowned.  Install the thread id of the
       * calling thread into the owner field.  This is the
       * common case.  In performance critical code the JIT
       * will have tried this before calling out to the VM.
       */
      newThin = thin | (threadId << LW_LOCK_OWNER_SHIFT);
      if (android_atomic_acquire_cas(thin, newThin, thinp) != 0) {
        // The acquire failed. Try again.
        goto retry;
      }
    } else {
      if (is_verbose_) {
        LOG(INFO) << StringPrintf("monitor: (%d) spin on lock %p: %#x (%#x) %#x", threadId, thinp, 0, *thinp, thin);
      }
      // The lock is owned by another thread. Notify the VM that we are about to wait.
      self->monitor_enter_object_ = obj;
      Thread::State oldStatus = self->SetState(Thread::kBlocked);
      // Spin until the thin lock is released or inflated.
      sleepDelayNs = 0;
      for (;;) {
        thin = *thinp;
        // Check the shape of the lock word. Another thread
        // may have inflated the lock while we were waiting.
        if (LW_SHAPE(thin) == LW_SHAPE_THIN) {
          if (LW_LOCK_OWNER(thin) == 0) {
            // The lock has been released. Install the thread id of the
            // calling thread into the owner field.
            newThin = thin | (threadId << LW_LOCK_OWNER_SHIFT);
            if (android_atomic_acquire_cas(thin, newThin, thinp) == 0) {
              // The acquire succeed. Break out of the loop and proceed to inflate the lock.
              break;
            }
          } else {
            // The lock has not been released. Yield so the owning thread can run.
            if (sleepDelayNs == 0) {
              sched_yield();
              sleepDelayNs = minSleepDelayNs;
            } else {
              tm.tv_sec = 0;
              tm.tv_nsec = sleepDelayNs;
              nanosleep(&tm, NULL);
              // Prepare the next delay value. Wrap to avoid once a second polls for eternity.
              if (sleepDelayNs < maxSleepDelayNs / 2) {
                sleepDelayNs *= 2;
              } else {
                sleepDelayNs = minSleepDelayNs;
              }
            }
          }
        } else {
          // The thin lock was inflated by another thread. Let the VM know we are no longer
          // waiting and try again.
          if (is_verbose_) {
            LOG(INFO) << "monitor: (" << threadId << ") lock " << (void*) thinp << " surprise-fattened";
          }
          self->monitor_enter_object_ = NULL;
          self->SetState(oldStatus);
          goto retry;
        }
      }
      if (is_verbose_) {
        LOG(INFO) << StringPrintf("monitor: (%d) spin on lock done %p: %#x (%#x) %#x", threadId, thinp, 0, *thinp, thin);
      }
      // We have acquired the thin lock. Let the VM know that we are no longer waiting.
      self->monitor_enter_object_ = NULL;
      self->SetState(oldStatus);
      // Fatten the lock.
      Inflate(self, obj);
      if (is_verbose_) {
        LOG(INFO) << StringPrintf("monitor: (%d) lock %p fattened", threadId, thinp);
      }
    }
  } else {
    // The lock is a fat lock.
    if (is_verbose_) {
      LOG(INFO) << StringPrintf("monitor: (%d) locking fat lock %p (%p) %p on a %s", threadId, thinp, LW_MONITOR(*thinp), (void*)*thinp, PrettyTypeOf(obj).c_str());
    }
    DCHECK(LW_MONITOR(*thinp) != NULL);
    LW_MONITOR(*thinp)->Lock(self);
  }
}

bool Monitor::MonitorExit(Thread* self, Object* obj) {
  volatile int32_t* thinp = obj->GetRawLockWordAddress();

  DCHECK(self != NULL);
  //DCHECK_EQ(self->GetState(), Thread::kRunnable);
  DCHECK(obj != NULL);

  /*
   * Cache the lock word as its value can change while we are
   * examining its state.
   */
  uint32_t thin = *thinp;
  if (LW_SHAPE(thin) == LW_SHAPE_THIN) {
    /*
     * The lock is thin.  We must ensure that the lock is owned
     * by the given thread before unlocking it.
     */
    if (LW_LOCK_OWNER(thin) == self->thin_lock_id_) {
      /*
       * We are the lock owner.  It is safe to update the lock
       * without CAS as lock ownership guards the lock itself.
       */
      if (LW_LOCK_COUNT(thin) == 0) {
        /*
         * The lock was not recursively acquired, the common
         * case.  Unlock by clearing all bits except for the
         * hash state.
         */
        thin &= (LW_HASH_STATE_MASK << LW_HASH_STATE_SHIFT);
        android_atomic_release_store(thin, thinp);
      } else {
        /*
         * The object was recursively acquired.  Decrement the
         * lock recursion count field.
         */
        *thinp -= 1 << LW_LOCK_COUNT_SHIFT;
      }
    } else {
      /*
       * We do not own the lock.  The JVM spec requires that we
       * throw an exception in this case.
       */
      ThrowIllegalMonitorStateException("unlock of unowned monitor");
      return false;
    }
  } else {
    /*
     * The lock is fat.  We must check to see if Unlock has
     * raised any exceptions before continuing.
     */
    DCHECK(LW_MONITOR(*thinp) != NULL);
    if (!LW_MONITOR(*thinp)->Unlock(self)) {
      // An exception has been raised.  Do not fall through.
      return false;
    }
  }
  return true;
}

/*
 * Object.wait().  Also called for class init.
 */
void Monitor::Wait(Thread* self, Object *obj, int64_t ms, int32_t ns, bool interruptShouldThrow) {
  volatile int32_t* thinp = obj->GetRawLockWordAddress();

  // If the lock is still thin, we need to fatten it.
  uint32_t thin = *thinp;
  if (LW_SHAPE(thin) == LW_SHAPE_THIN) {
    // Make sure that 'self' holds the lock.
    if (LW_LOCK_OWNER(thin) != self->thin_lock_id_) {
      ThrowIllegalMonitorStateException("object not locked by thread before wait()");
      return;
    }

    /* This thread holds the lock.  We need to fatten the lock
     * so 'self' can block on it.  Don't update the object lock
     * field yet, because 'self' needs to acquire the lock before
     * any other thread gets a chance.
     */
    Inflate(self, obj);
    if (is_verbose_) {
      LOG(INFO) << StringPrintf("monitor: (%d) lock %p fattened by wait()", self->thin_lock_id_, thinp);
    }
  }
  LW_MONITOR(*thinp)->Wait(self, ms, ns, interruptShouldThrow);
}

void Monitor::Notify(Thread* self, Object *obj) {
  uint32_t thin = *obj->GetRawLockWordAddress();

  // If the lock is still thin, there aren't any waiters;
  // waiting on an object forces lock fattening.
  if (LW_SHAPE(thin) == LW_SHAPE_THIN) {
    // Make sure that 'self' holds the lock.
    if (LW_LOCK_OWNER(thin) != self->thin_lock_id_) {
      ThrowIllegalMonitorStateException("object not locked by thread before notify()");
      return;
    }
    // no-op;  there are no waiters to notify.
  } else {
    // It's a fat lock.
    LW_MONITOR(thin)->Notify(self);
  }
}

void Monitor::NotifyAll(Thread* self, Object *obj) {
  uint32_t thin = *obj->GetRawLockWordAddress();

  // If the lock is still thin, there aren't any waiters;
  // waiting on an object forces lock fattening.
  if (LW_SHAPE(thin) == LW_SHAPE_THIN) {
    // Make sure that 'self' holds the lock.
    if (LW_LOCK_OWNER(thin) != self->thin_lock_id_) {
      ThrowIllegalMonitorStateException("object not locked by thread before notifyAll()");
      return;
    }
    // no-op;  there are no waiters to notify.
  } else {
    // It's a fat lock.
    LW_MONITOR(thin)->NotifyAll(self);
  }
}

uint32_t Monitor::GetLockOwner(uint32_t raw_lock_word) {
  if (LW_SHAPE(raw_lock_word) == LW_SHAPE_THIN) {
    return LW_LOCK_OWNER(raw_lock_word);
  } else {
    Thread* owner = LW_MONITOR(raw_lock_word)->owner_;
    return owner ? owner->GetThinLockId() : 0;
  }
}

void Monitor::DescribeWait(std::ostream& os, const Thread* thread) {
  Thread::State state = thread->GetState();

  Object* object = NULL;
  uint32_t lock_owner = ThreadList::kInvalidId;
  if (state == Thread::kWaiting || state == Thread::kTimedWaiting) {
    os << "  - waiting on ";
    Monitor* monitor = thread->wait_monitor_;
    if (monitor != NULL) {
      object = monitor->obj_;
    }
    lock_owner = Thread::LockOwnerFromThreadLock(object);
  } else if (state == Thread::kBlocked) {
    os << "  - waiting to lock ";
    object = thread->monitor_enter_object_;
    if (object != NULL) {
      lock_owner = object->GetLockOwner();
    }
  } else {
    // We're not waiting on anything.
    return;
  }
  os << "<" << object << ">";

  // - waiting on <0x613f83d8> (a java.lang.ThreadLock) held by thread 5
  // - waiting on <0x6008c468> (a java.lang.Class<java.lang.ref.ReferenceQueue>)
  os << " (a " << PrettyTypeOf(object) << ")";

  if (lock_owner != ThreadList::kInvalidId) {
    os << " held by thread " << lock_owner;
  }

  os << "\n";
}

MonitorList::MonitorList() : lock_("MonitorList lock") {
}

MonitorList::~MonitorList() {
  MutexLock mu(lock_);
  STLDeleteElements(&list_);
}

void MonitorList::Add(Monitor* m) {
  MutexLock mu(lock_);
  list_.push_front(m);
}

void MonitorList::SweepMonitorList(Heap::IsMarkedTester is_marked, void* arg) {
  MutexLock mu(lock_);
  typedef std::list<Monitor*>::iterator It; // TODO: C++0x auto
  It it = list_.begin();
  while (it != list_.end()) {
    Monitor* m = *it;
    if (!is_marked(m->GetObject(), arg)) {
      if (Monitor::IsVerbose()) {
        LOG(INFO) << "freeing monitor " << m << " belonging to unmarked object " << m->GetObject();
      }
      delete m;
      it = list_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace art
