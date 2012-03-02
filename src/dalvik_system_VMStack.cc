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
#include "class_loader.h"
#include "object.h"
#include "thread_list.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

static jobject GetThreadStack(JNIEnv* env, jobject javaThread) {
  ScopedHeapLock heap_lock;
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(env, javaThread);
  return (thread != NULL) ? GetThreadStack(env, thread) : NULL;
}

jint VMStack_fillStackTraceElements(JNIEnv* env, jclass, jobject javaThread, jobjectArray javaSteArray) {
  jobject trace = GetThreadStack(env, javaThread);
  if (trace == NULL) {
    return 0;
  }
  int32_t depth;
  Thread::InternalStackTraceToStackTraceElementArray(env, trace, javaSteArray, &depth);
  return depth;
}

jobject VMStack_getCallingClassLoader(JNIEnv* env, jclass) {
  // Returns the defining class loader of the caller's caller.
  // TODO: need SmartFrame (Thread::WalkStack-like iterator).
  Frame frame = Thread::Current()->GetTopOfStack();
  frame.Next();
  frame.Next();
  Method* callerCaller = frame.GetMethod();
  const Object* cl = callerCaller->GetDeclaringClass()->GetClassLoader();
  return AddLocalReference<jobject>(env, cl);
}

jobject VMStack_getClosestUserClassLoader(JNIEnv* env, jclass, jobject javaBootstrap, jobject javaSystem) {
  Thread* self = Thread::Current();
  Object* bootstrap = Decode<Object*>(env, javaBootstrap);
  Object* system = Decode<Object*>(env, javaSystem);
  // TODO: need SmartFrame (Thread::WalkStack-like iterator).
  for (Frame frame = self->GetTopOfStack(); frame.HasNext(); frame.Next()) {
    Class* c = frame.GetMethod()->GetDeclaringClass();
    Object* cl = c->GetClassLoader();
    if (cl != NULL && cl != bootstrap && cl != system) {
      return AddLocalReference<jobject>(env, cl);
    }
  }
  return NULL;
}

jclass VMStack_getStackClass2(JNIEnv* env, jclass) {
  // Returns the class of the caller's caller's caller.
  // TODO: need SmartFrame (Thread::WalkStack-like iterator).
  Frame frame = Thread::Current()->GetTopOfStack();
  frame.Next();
  frame.Next();
  frame.Next();
  Method* callerCallerCaller = frame.GetMethod();
  Class* c = callerCallerCaller->GetDeclaringClass();
  return AddLocalReference<jclass>(env, c);
}

jobjectArray VMStack_getThreadStackTrace(JNIEnv* env, jclass, jobject javaThread) {
  jobject trace = GetThreadStack(env, javaThread);
  if (trace == NULL) {
    return NULL;
  }
  return Thread::InternalStackTraceToStackTraceElementArray(env, trace);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(VMStack, fillStackTraceElements, "(Ljava/lang/Thread;[Ljava/lang/StackTraceElement;)I"),
  NATIVE_METHOD(VMStack, getCallingClassLoader, "()Ljava/lang/ClassLoader;"),
  NATIVE_METHOD(VMStack, getClosestUserClassLoader, "(Ljava/lang/ClassLoader;Ljava/lang/ClassLoader;)Ljava/lang/ClassLoader;"),
  NATIVE_METHOD(VMStack, getStackClass2, "()Ljava/lang/Class;"),
  NATIVE_METHOD(VMStack, getThreadStackTrace, "(Ljava/lang/Thread;)[Ljava/lang/StackTraceElement;"),
};

}  // namespace

void register_dalvik_system_VMStack(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/VMStack", gMethods, NELEM(gMethods));
}

}  // namespace art
