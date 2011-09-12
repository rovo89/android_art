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

#include "jni_internal.h"
#include "object.h"
#include "thread.h"
#include "thread_list.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

jobject Thread_currentThread(JNIEnv* env, jclass) {
  return AddLocalReference<jobject>(env, Thread::Current()->GetPeer());
}

jboolean Thread_interrupted(JNIEnv* env, jclass) {
  return Thread::Current()->Interrupted();
}

jboolean Thread_isInterrupted(JNIEnv* env, jobject javaThread) {
  ThreadListLock lock;
  Thread* thread = Thread::FromManagedThread(env, javaThread);
  return (thread != NULL) ? thread->IsInterrupted() : JNI_FALSE;
}

void Thread_nativeCreate(JNIEnv* env, jclass, jobject javaThread, jlong stackSize) {
  Object* managedThread = Decode<Object*>(env, javaThread);
  Thread::Create(managedThread, stackSize);
}

jint Thread_nativeGetStatus(JNIEnv* env, jobject javaThread) {
  ThreadListLock lock;
  Thread* thread = Thread::FromManagedThread(env, javaThread);
  Thread::State state = (thread != NULL) ? thread->GetState() : Thread::kUnknown;
  return static_cast<jint>(state);
}

jboolean Thread_nativeHoldsLock(JNIEnv* env, jobject javaThread, jobject javaObject) {
  ThreadListLock lock;
  //Thread* thread = Thread::FromManagedThread(env, javaThread);
  //Object* object = dvmDecodeIndirectRef(env, javaObject);
  //if (object == NULL) {
    //dvmThrowNullPointerException("object == null");
    //return JNI_FALSE;
  //}
  //Thread* thread = Thread::FromManagedThread(env, javaThread);
  //int result = dvmHoldsLock(thread, object);
  //return result;
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

void Thread_nativeInterrupt(JNIEnv* env, jobject javaThread) {
  ThreadListLock lock;
  UNIMPLEMENTED(FATAL);
  //Thread* thread = Thread::FromManagedThread(env, javaThread);
  //if (thread != NULL) {
    //dvmThreadInterrupt(thread);
  //}
}

void Thread_nativeSetName(JNIEnv* env, jobject javaThread, jstring javaName) {
  ThreadListLock lock;
  UNIMPLEMENTED(WARNING);
  //Thread* thread = Thread::FromManagedThread(env, javaThread);
  //StringObject* nameStr = (StringObject*) dvmDecodeIndirectRef(env, javaName);
  //int threadId = (thread != NULL) ? thread->threadId : -1;
  //dvmDdmSendThreadNameChange(threadId, nameStr);
}

/*
 * Alter the priority of the specified thread.  "newPriority" will range
 * from Thread.MIN_PRIORITY to Thread.MAX_PRIORITY (1-10), with "normal"
 * threads at Thread.NORM_PRIORITY (5).
 */
void Thread_nativeSetPriority(JNIEnv* env, jobject javaThread, jint newPriority) {
  ThreadListLock lock;
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
void Thread_yield(JNIEnv*, jobject) {
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

}  // namespace

void register_java_lang_Thread(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/Thread", gMethods, NELEM(gMethods));
}

}  // namespace art
