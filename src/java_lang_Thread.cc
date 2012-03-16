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
#include "scoped_thread_list_lock.h"
#include "ScopedUtfChars.h"
#include "thread.h"
#include "thread_list.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

static jobject Thread_currentThread(JNIEnv* env, jclass) {
  return AddLocalReference<jobject>(env, Thread::Current()->GetPeer());
}

static jboolean Thread_interrupted(JNIEnv*, jclass) {
  return Thread::Current()->Interrupted();
}

static jboolean Thread_isInterrupted(JNIEnv* env, jobject javaThread) {
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(env, javaThread);
  return (thread != NULL) ? thread->IsInterrupted() : JNI_FALSE;
}

static void Thread_nativeCreate(JNIEnv* env, jclass, jobject javaThread, jlong stackSize) {
  Object* managedThread = Decode<Object*>(env, javaThread);
  Thread::Create(managedThread, stackSize);
}

static jint Thread_nativeGetStatus(JNIEnv* env, jobject javaThread) {
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(env, javaThread);
  if (thread == NULL) {
    return -1;
  }
  return static_cast<jint>(thread->GetState());
}

static jboolean Thread_nativeHoldsLock(JNIEnv* env, jobject javaThread, jobject javaObject) {
  Object* object = Decode<Object*>(env, javaObject);
  if (object == NULL) {
    ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
    Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;", "object == null");
    return JNI_FALSE;
  }
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(env, javaThread);
  return thread->HoldsLock(object);
}

static void Thread_nativeInterrupt(JNIEnv* env, jobject javaThread) {
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(env, javaThread);
  if (thread != NULL) {
    thread->Interrupt();
  }
}

static void Thread_nativeSetName(JNIEnv* env, jobject javaThread, jstring javaName) {
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(env, javaThread);
  if (thread == NULL) {
    return;
  }
  ScopedUtfChars name(env, javaName);
  if (name.c_str() == NULL) {
    return;
  }
  thread->SetThreadName(name.c_str());
}

/*
 * Alter the priority of the specified thread.  "newPriority" will range
 * from Thread.MIN_PRIORITY to Thread.MAX_PRIORITY (1-10), with "normal"
 * threads at Thread.NORM_PRIORITY (5).
 */
static void Thread_nativeSetPriority(JNIEnv* env, jobject javaThread, jint newPriority) {
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(env, javaThread);
  if (thread != NULL) {
    thread->SetNativePriority(newPriority);
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
  NATIVE_METHOD(Thread, nativeCreate, "(Ljava/lang/Thread;J)V"),
  NATIVE_METHOD(Thread, nativeGetStatus, "()I"),
  NATIVE_METHOD(Thread, nativeHoldsLock, "(Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Thread, nativeInterrupt, "()V"),
  NATIVE_METHOD(Thread, nativeSetName, "(Ljava/lang/String;)V"),
  NATIVE_METHOD(Thread, nativeSetPriority, "(I)V"),
  NATIVE_METHOD(Thread, yield, "()V"),
};

void register_java_lang_Thread(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/Thread", gMethods, NELEM(gMethods));
}

}  // namespace art
