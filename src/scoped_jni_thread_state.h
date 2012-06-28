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

#include "casts.h"
#include "jni_internal.h"
#include "thread.h"

namespace art {

// Entry/exit processing for transitions from Native to Runnable (ie within JNI functions).
//
// This class performs the necessary thread state switching to and from Runnable and lets us
// amortize the cost of working out the current thread. Additionally it lets us check (and repair)
// apps that are using a JNIEnv on the wrong thread. The class also decodes and encodes Objects
// into jobjects via methods of this class. Performing this here enforces the Runnable thread state
// for use of Object, thereby inhibiting the Object being modified by GC whilst native or VM code
// is also manipulating the Object.
//
// The destructor transitions back to the previous thread state, typically Native. In this case
// GC and thread suspension may occur.
class ScopedJniThreadState {
 public:
  explicit ScopedJniThreadState(JNIEnv* env, ThreadState new_state = kRunnable)
      : env_(reinterpret_cast<JNIEnvExt*>(env)), vm_(env_->vm), self_(ThreadForEnv(env)),
        old_thread_state_(self_->SetState(new_state)), thread_state_(new_state) {
    self_->VerifyStack();
  }

  explicit ScopedJniThreadState(Thread* self, ThreadState new_state = kRunnable)
      : env_(reinterpret_cast<JNIEnvExt*>(self->GetJniEnv())), vm_(env_->vm), self_(self),
        old_thread_state_(self_->SetState(new_state)), thread_state_(new_state) {
    if (!Vm()->work_around_app_jni_bugs && self != Thread::Current()) {
      UnexpectedThreads(self, Thread::Current());
    }
    self_->VerifyStack();
  }

  // Used when we want a scoped JNI thread state but have no thread/JNIEnv.
  explicit ScopedJniThreadState(JavaVM* vm)
      : env_(NULL), vm_(reinterpret_cast<JavaVMExt*>(vm)), self_(NULL),
        old_thread_state_(kTerminated), thread_state_(kTerminated) {}

  ~ScopedJniThreadState() {
    if (self_ != NULL) {
      self_->SetState(old_thread_state_);
    }
  }

  JNIEnvExt* Env() const {
    return env_;
  }

  Thread* Self() const {
    return self_;
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
  T AddLocalReference(Object* obj) const {
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
  T Decode(jobject obj) const {
    DCHECK_EQ(thread_state_, kRunnable);  // Don't work with raw objects in non-runnable states.
    return down_cast<T>(Self()->DecodeJObject(obj));
  }

  Field* DecodeField(jfieldID fid) const {
    DCHECK_EQ(thread_state_, kRunnable);  // Don't work with raw objects in non-runnable states.
  #ifdef MOVING_GARBAGE_COLLECTOR
    // TODO: we should make these unique weak globals if Field instances can ever move.
    UNIMPLEMENTED(WARNING);
  #endif
    return reinterpret_cast<Field*>(fid);
  }

  jfieldID EncodeField(Field* field) const {
    DCHECK_EQ(thread_state_, kRunnable);  // Don't work with raw objects in non-runnable states.
  #ifdef MOVING_GARBAGE_COLLECTOR
    UNIMPLEMENTED(WARNING);
  #endif
    return reinterpret_cast<jfieldID>(field);
  }

  Method* DecodeMethod(jmethodID mid) const {
    DCHECK_EQ(thread_state_, kRunnable);  // Don't work with raw objects in non-runnable states.
  #ifdef MOVING_GARBAGE_COLLECTOR
    // TODO: we should make these unique weak globals if Method instances can ever move.
    UNIMPLEMENTED(WARNING);
  #endif
    return reinterpret_cast<Method*>(mid);
  }

  jmethodID EncodeMethod(Method* method) const {
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
  // Cached current thread derived from the JNIEnv.
  Thread* const self_;
  // Previous thread state, most likely kNative.
  const ThreadState old_thread_state_;
  // Local cache of thread state to enable quick sanity checks.
  const ThreadState thread_state_;
  DISALLOW_COPY_AND_ASSIGN(ScopedJniThreadState);
};

}  // namespace art
