/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_SCOPED_THREAD_STATE_CHANGE_H_
#define ART_SRC_SCOPED_THREAD_STATE_CHANGE_H_

#include "base/casts.h"
#include "jni_internal.h"
#include "thread.h"

namespace art {

// Scoped change into and out of a particular state. Handles Runnable transitions that require
// more complicated suspension checking. The subclasses ScopedObjectAccessUnchecked and
// ScopedObjectAccess are used to handle the change into Runnable to get direct access to objects,
// the unchecked variant doesn't aid annotalysis.
class ScopedThreadStateChange {
 public:
  ScopedThreadStateChange(Thread* self, ThreadState new_thread_state)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      : self_(self), thread_state_(new_thread_state), expected_has_no_thread_(false) {
    if (self_ == NULL) {
      // Value chosen arbitrarily and won't be used in the destructor since thread_ == NULL.
      old_thread_state_ = kTerminated;
      MutexLock mu(NULL, *Locks::runtime_shutdown_lock_);
      Runtime* runtime = Runtime::Current();
      CHECK(runtime == NULL || !runtime->IsStarted() || runtime->IsShuttingDown());
    } else {
      bool runnable_transition;
      DCHECK_EQ(self, Thread::Current());
      // Read state without locks, ok as state is effectively thread local and we're not interested
      // in the suspend count (this will be handled in the runnable transitions).
      old_thread_state_ = self->GetState();
      runnable_transition = old_thread_state_ == kRunnable || new_thread_state == kRunnable;
      if (!runnable_transition) {
        // A suspended transition to another effectively suspended transition, ok to use Unsafe.
        self_->SetState(new_thread_state);
      }

      if (runnable_transition && old_thread_state_ != new_thread_state) {
        if (new_thread_state == kRunnable) {
          self_->TransitionFromSuspendedToRunnable();
        } else {
          DCHECK_EQ(old_thread_state_, kRunnable);
          self_->TransitionFromRunnableToSuspended(new_thread_state);
        }
      }
    }
  }

  ~ScopedThreadStateChange() LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_) {
    if (self_ == NULL) {
      if (!expected_has_no_thread_) {
        MutexLock mu(NULL, *Locks::runtime_shutdown_lock_);
        Runtime* runtime = Runtime::Current();
        bool shutting_down = (runtime == NULL) || runtime->IsShuttingDown();
        CHECK(shutting_down);
      }
    } else {
      if (old_thread_state_ != thread_state_) {
        if (old_thread_state_ == kRunnable) {
          self_->TransitionFromSuspendedToRunnable();
        } else if (thread_state_ == kRunnable) {
          self_->TransitionFromRunnableToSuspended(old_thread_state_);
        } else {
          // A suspended transition to another effectively suspended transition, ok to use Unsafe.
          self_->SetState(old_thread_state_);
        }
      }
    }
  }

  Thread* Self() const {
    return self_;
  }

 protected:
  // Constructor used by ScopedJniThreadState for an unattached thread that has access to the VM*.
  ScopedThreadStateChange()
      : self_(NULL), thread_state_(kTerminated), old_thread_state_(kTerminated),
        expected_has_no_thread_(true) {}

  Thread* const self_;
  const ThreadState thread_state_;

 private:
  ThreadState old_thread_state_;
  const bool expected_has_no_thread_;

  DISALLOW_COPY_AND_ASSIGN(ScopedThreadStateChange);
};

// Entry/exit processing for transitions from Native to Runnable (ie within JNI functions).
//
// This class performs the necessary thread state switching to and from Runnable and lets us
// amortize the cost of working out the current thread. Additionally it lets us check (and repair)
// apps that are using a JNIEnv on the wrong thread. The class also decodes and encodes Objects
// into jobjects via methods of this class. Performing this here enforces the Runnable thread state
// for use of Object, thereby inhibiting the Object being modified by GC whilst native or VM code
// is also manipulating the Object.
//
// The destructor transitions back to the previous thread state, typically Native. In this state
// GC and thread suspension may occur.
//
// For annotalysis the subclass ScopedObjectAccess (below) makes it explicit that a shared of
// the mutator_lock_ will be acquired on construction.
class ScopedObjectAccessUnchecked : public ScopedThreadStateChange {
 public:
  explicit ScopedObjectAccessUnchecked(JNIEnv* env)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      : ScopedThreadStateChange(ThreadForEnv(env), kRunnable),
        env_(reinterpret_cast<JNIEnvExt*>(env)), vm_(env_->vm) {
    self_->VerifyStack();
  }

  explicit ScopedObjectAccessUnchecked(Thread* self)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      : ScopedThreadStateChange(self, kRunnable),
        env_(reinterpret_cast<JNIEnvExt*>(self->GetJniEnv())),
        vm_(env_ != NULL ? env_->vm : NULL) {
    if (Vm() != NULL && !Vm()->work_around_app_jni_bugs && self != Thread::Current()) {
      UnexpectedThreads(self, Thread::Current());
    }
    self_->VerifyStack();
  }

  // Used when we want a scoped JNI thread state but have no thread/JNIEnv. Consequently doesn't
  // change into Runnable or acquire a share on the mutator_lock_.
  explicit ScopedObjectAccessUnchecked(JavaVM* vm)
      : ScopedThreadStateChange(), env_(NULL), vm_(reinterpret_cast<JavaVMExt*>(vm)) {}

  JNIEnvExt* Env() const {
    return env_;
  }

  JavaVMExt* Vm() const {
    return vm_;
  }

  /*
   * Add a local reference for an object to the indirect reference table associated with the
   * current stack frame.  When the native function returns, the reference will be discarded.
   * Part of the ScopedJniThreadState as native code shouldn't be working on raw Object* without
   * having transitioned its state.
   *
   * We need to allow the same reference to be added multiple times.
   *
   * This will be called on otherwise unreferenced objects.  We cannot do GC allocations here, and
   * it's best if we don't grab a mutex.
   *
   * Returns the local reference (currently just the same pointer that was
   * passed in), or NULL on failure.
   */
  template<typename T>
  T AddLocalReference(Object* obj) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK_EQ(thread_state_, kRunnable);  // Don't work with raw objects in non-runnable states.
    if (obj == NULL) {
      return NULL;
    }

    DCHECK_NE((reinterpret_cast<uintptr_t>(obj) & 0xffff0000), 0xebad0000);

    IndirectReferenceTable& locals = Env()->locals;

    uint32_t cookie = Env()->local_ref_cookie;
    IndirectRef ref = locals.Add(cookie, obj);

#if 0 // TODO: fix this to understand PushLocalFrame, so we can turn it on.
    if (Env()->check_jni) {
      size_t entry_count = locals.Capacity();
      if (entry_count > 16) {
        LOG(WARNING) << "Warning: more than 16 JNI local references: "
                     << entry_count << " (most recent was a " << PrettyTypeOf(obj) << ")\n"
                     << Dumpable<IndirectReferenceTable>(locals);
        // TODO: LOG(FATAL) in a later release?
      }
    }
#endif

    if (Vm()->work_around_app_jni_bugs) {
      // Hand out direct pointers to support broken old apps.
      return reinterpret_cast<T>(obj);
    }

    return reinterpret_cast<T>(ref);
  }

  template<typename T>
  T Decode(jobject obj) const
      LOCKS_EXCLUDED(JavaVMExt::globals_lock,
                     JavaVMExt::weak_globals_lock)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
    DCHECK_EQ(thread_state_, kRunnable);  // Don't work with raw objects in non-runnable states.
    return down_cast<T>(Self()->DecodeJObject(obj));
  }

  Field* DecodeField(jfieldID fid) const
      LOCKS_EXCLUDED(JavaVMExt::globals_lock,
                     JavaVMExt::weak_globals_lock)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
    DCHECK_EQ(thread_state_, kRunnable);  // Don't work with raw objects in non-runnable states.
#ifdef MOVING_GARBAGE_COLLECTOR
    // TODO: we should make these unique weak globals if Field instances can ever move.
    UNIMPLEMENTED(WARNING);
#endif
    return reinterpret_cast<Field*>(fid);
  }

  jfieldID EncodeField(Field* field) const
      LOCKS_EXCLUDED(JavaVMExt::globals_lock,
                     JavaVMExt::weak_globals_lock)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
    DCHECK_EQ(thread_state_, kRunnable);  // Don't work with raw objects in non-runnable states.
#ifdef MOVING_GARBAGE_COLLECTOR
    UNIMPLEMENTED(WARNING);
#endif
    return reinterpret_cast<jfieldID>(field);
  }

  AbstractMethod* DecodeMethod(jmethodID mid) const
      LOCKS_EXCLUDED(JavaVMExt::globals_lock,
                     JavaVMExt::weak_globals_lock)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
    DCHECK_EQ(thread_state_, kRunnable);  // Don't work with raw objects in non-runnable states.
#ifdef MOVING_GARBAGE_COLLECTOR
    // TODO: we should make these unique weak globals if Method instances can ever move.
    UNIMPLEMENTED(WARNING);
#endif
    return reinterpret_cast<AbstractMethod*>(mid);
  }

  jmethodID EncodeMethod(AbstractMethod* method) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
    DCHECK_EQ(thread_state_, kRunnable);  // Don't work with raw objects in non-runnable states.
#ifdef MOVING_GARBAGE_COLLECTOR
    UNIMPLEMENTED(WARNING);
#endif
    return reinterpret_cast<jmethodID>(method);
  }

 private:
  static Thread* ThreadForEnv(JNIEnv* env) {
    JNIEnvExt* full_env(reinterpret_cast<JNIEnvExt*>(env));
    bool work_around_app_jni_bugs = full_env->vm->work_around_app_jni_bugs;
    Thread* env_self = full_env->self;
    Thread* self = work_around_app_jni_bugs ? Thread::Current() : env_self;
    if (!work_around_app_jni_bugs && self != env_self) {
      UnexpectedThreads(env_self, self);
    }
    return self;
  }

  static void UnexpectedThreads(Thread* found_self, Thread* expected_self) {
    // TODO: pass through function name so we can use it here instead of NULL...
    JniAbortF(NULL, "JNIEnv for %s used on %s",
             found_self != NULL ? ToStr<Thread>(*found_self).c_str() : "NULL",
             expected_self != NULL ? ToStr<Thread>(*expected_self).c_str() : "NULL");

  }

  // The full JNIEnv.
  JNIEnvExt* const env_;
  // The full JavaVM.
  JavaVMExt* const vm_;

  DISALLOW_COPY_AND_ASSIGN(ScopedObjectAccessUnchecked);
};

// Annotalysis helping variant of the above.
class ScopedObjectAccess : public ScopedObjectAccessUnchecked {
 public:
  explicit ScopedObjectAccess(JNIEnv* env)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCK_FUNCTION(Locks::mutator_lock_)
      : ScopedObjectAccessUnchecked(env) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
  }

  explicit ScopedObjectAccess(Thread* self)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCK_FUNCTION(Locks::mutator_lock_)
      : ScopedObjectAccessUnchecked(self) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
  }

  ~ScopedObjectAccess() UNLOCK_FUNCTION(Locks::mutator_lock_) {
    // Base class will release share of lock. Invoked after this destructor.
  }

 private:
  // TODO: remove this constructor. It is used by check JNI's ScopedCheck to make it believe that
  //       routines operating with just a VM are sound, they are not, but when you have just a VM
  //       you cannot call the unsound routines.
  explicit ScopedObjectAccess(JavaVM* vm)
      SHARED_LOCK_FUNCTION(Locks::mutator_lock_)
      : ScopedObjectAccessUnchecked(vm) {}

  friend class ScopedCheck;
  DISALLOW_COPY_AND_ASSIGN(ScopedObjectAccess);
};

}  // namespace art

#endif  // ART_SRC_SCOPED_THREAD_STATE_CHANGE_H_
