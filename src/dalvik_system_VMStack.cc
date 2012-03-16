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
#include "scoped_heap_lock.h"
#include "scoped_thread_list_lock.h"
#include "thread_list.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

static jobject GetThreadStack(JNIEnv* env, jobject javaThread) {
  ScopedHeapLock heap_lock;
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(env, javaThread);
  return (thread != NULL) ? GetThreadStack(env, thread) : NULL;
}

static jint VMStack_fillStackTraceElements(JNIEnv* env, jclass, jobject javaThread, jobjectArray javaSteArray) {
  jobject trace = GetThreadStack(env, javaThread);
  if (trace == NULL) {
    return 0;
  }
  int32_t depth;
  Thread::InternalStackTraceToStackTraceElementArray(env, trace, javaSteArray, &depth);
  return depth;
}

static jobject VMStack_getCallingClassLoader(JNIEnv* env, jclass) {
  // Returns the defining class loader of the caller's caller.
  // TODO: need SmartFrame (Thread::WalkStack-like iterator).
  Frame frame = Thread::Current()->GetTopOfStack();
  frame.Next();
  frame.Next();
  Method* callerCaller = frame.GetMethod();
  DCHECK(callerCaller != NULL);
  const Object* cl = callerCaller->GetDeclaringClass()->GetClassLoader();
  return AddLocalReference<jobject>(env, cl);
}

static jobject VMStack_getClosestUserClassLoader(JNIEnv* env, jclass, jobject javaBootstrap, jobject javaSystem) {
  struct ClosestUserClassLoaderVisitor : public Thread::StackVisitor {
    ClosestUserClassLoaderVisitor(Object* bootstrap, Object* system)
      : bootstrap(bootstrap), system(system), class_loader(NULL) {}
    bool VisitFrame(const Frame& f, uintptr_t) {
      DCHECK(class_loader == NULL);
      Class* c = f.GetMethod()->GetDeclaringClass();
      Object* cl = c->GetClassLoader();
      if (cl != NULL && cl != bootstrap && cl != system) {
        class_loader = cl;
        return false;
      }
      return true;
    }
    Object* bootstrap;
    Object* system;
    Object* class_loader;
  };
  Object* bootstrap = Decode<Object*>(env, javaBootstrap);
  Object* system = Decode<Object*>(env, javaSystem);
  ClosestUserClassLoaderVisitor visitor(bootstrap, system);
  Thread::Current()->WalkStack(&visitor);
  return AddLocalReference<jobject>(env, visitor.class_loader);
}

static jclass VMStack_getStackClass2(JNIEnv* env, jclass) {
  // Returns the class of the caller's caller's caller.
  // TODO: need SmartFrame (Thread::WalkStack-like iterator).
  Frame frame = Thread::Current()->GetTopOfStack();
  frame.Next();
  frame.Next();
  frame.Next();
  Method* callerCallerCaller = frame.GetMethod();
  DCHECK(callerCallerCaller != NULL);
  Class* c = callerCallerCaller->GetDeclaringClass();
  return AddLocalReference<jclass>(env, c);
}

static jobjectArray VMStack_getThreadStackTrace(JNIEnv* env, jclass, jobject javaThread) {
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

void register_dalvik_system_VMStack(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/VMStack", gMethods, NELEM(gMethods));
}

}  // namespace art
