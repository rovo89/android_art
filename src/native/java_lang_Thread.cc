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

#include "debugger.h"
#include "jni_internal.h"
#include "object.h"
#include "scoped_jni_thread_state.h"
#include "scoped_thread_list_lock.h"
#include "ScopedUtfChars.h"
#include "thread.h"
#include "thread_list.h"

namespace art {

static jobject Thread_currentThread(JNIEnv* env, jclass) {
  ScopedJniThreadState ts(env);
  return ts.AddLocalReference<jobject>(ts.Self()->GetPeer());
}

static jboolean Thread_interrupted(JNIEnv* env, jclass) {
  ScopedJniThreadState ts(env, kNative);  // Doesn't touch objects, so keep in native state.
  return ts.Self()->Interrupted();
}

static jboolean Thread_isInterrupted(JNIEnv* env, jobject java_thread) {
  ScopedJniThreadState ts(env);
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(ts, java_thread);
  return (thread != NULL) ? thread->IsInterrupted() : JNI_FALSE;
}

static void Thread_nativeCreate(JNIEnv* env, jclass, jobject java_thread, jlong stack_size,
                                jboolean daemon) {
  Thread::CreateNativeThread(env, java_thread, stack_size, daemon == JNI_TRUE);
}

static jint Thread_nativeGetStatus(JNIEnv* env, jobject java_thread, jboolean has_been_started) {
  // Ordinals from Java's Thread.State.
  const jint kJavaNew = 0;
  const jint kJavaRunnable = 1;
  const jint kJavaBlocked = 2;
  const jint kJavaWaiting = 3;
  const jint kJavaTimedWaiting = 4;
  const jint kJavaTerminated = 5;

  ScopedJniThreadState ts(env);
  ThreadState internal_thread_state = (has_been_started ? kTerminated : kStarting);
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(ts, java_thread);
  if (thread != NULL) {
    internal_thread_state = thread->GetState();
  }
  switch (internal_thread_state) {
    case kTerminated:   return kJavaTerminated;
    case kRunnable:     return kJavaRunnable;
    case kTimedWaiting: return kJavaTimedWaiting;
    case kBlocked:      return kJavaBlocked;
    case kWaiting:      return kJavaWaiting;
    case kStarting:     return kJavaNew;
    case kNative:       return kJavaRunnable;
    case kVmWait:       return kJavaWaiting;
    case kSuspended:    return kJavaRunnable;
    // Don't add a 'default' here so the compiler can spot incompatible enum changes.
  }
  return -1; // Unreachable.
}

static jboolean Thread_nativeHoldsLock(JNIEnv* env, jobject java_thread, jobject java_object) {
  ScopedJniThreadState ts(env);
  Object* object = ts.Decode<Object*>(java_object);
  if (object == NULL) {
    Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;", "object == null");
    return JNI_FALSE;
  }
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(ts, java_thread);
  return thread->HoldsLock(object);
}

static void Thread_nativeInterrupt(JNIEnv* env, jobject java_thread) {
  ScopedJniThreadState ts(env);
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(ts, java_thread);
  if (thread != NULL) {
    thread->Interrupt();
  }
}

static void Thread_nativeSetName(JNIEnv* env, jobject java_thread, jstring java_name) {
  ScopedJniThreadState ts(env);
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(ts, java_thread);
  if (thread == NULL) {
    return;
  }
  ScopedUtfChars name(env, java_name);
  if (name.c_str() == NULL) {
    return;
  }
  thread->SetThreadName(name.c_str());
}

/*
 * Alter the priority of the specified thread.  "new_priority" will range
 * from Thread.MIN_PRIORITY to Thread.MAX_PRIORITY (1-10), with "normal"
 * threads at Thread.NORM_PRIORITY (5).
 */
static void Thread_nativeSetPriority(JNIEnv* env, jobject java_thread, jint new_priority) {
  ScopedJniThreadState ts(env);
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(ts, java_thread);
  if (thread != NULL) {
    thread->SetNativePriority(new_priority);
  }
}

/*
 * Causes the thread to temporarily pause and allow other threads to execute.
 *
 * The exact behavior is poorly defined.  Some discussion here:
 *   http://www.cs.umd.edu/~pugh/java/memoryModel/archive/0944.html
 */
static void Thread_yield(JNIEnv*, jobject) {
  sched_yield();
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Thread, currentThread, "()Ljava/lang/Thread;"),
  NATIVE_METHOD(Thread, interrupted, "()Z"),
  NATIVE_METHOD(Thread, isInterrupted, "()Z"),
  NATIVE_METHOD(Thread, nativeCreate, "(Ljava/lang/Thread;JZ)V"),
  NATIVE_METHOD(Thread, nativeGetStatus, "(Z)I"),
  NATIVE_METHOD(Thread, nativeHoldsLock, "(Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Thread, nativeInterrupt, "()V"),
  NATIVE_METHOD(Thread, nativeSetName, "(Ljava/lang/String;)V"),
  NATIVE_METHOD(Thread, nativeSetPriority, "(I)V"),
  NATIVE_METHOD(Thread, yield, "()V"),
};

void register_java_lang_Thread(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Thread");
}

}  // namespace art
