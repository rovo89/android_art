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

#include "class_loader.h"
#include "jni_internal.h"
#include "nth_caller_visitor.h"
#include "object.h"
#include "scoped_thread_state_change.h"
#include "thread_list.h"

namespace art {

static jobject GetThreadStack(JNIEnv* env, jobject peer) {
  bool timeout;
  Thread* self = Thread::Current();
  if (env->IsSameObject(peer, self->GetPeer())) {
    ScopedObjectAccess soa(env);
    return self->CreateInternalStackTrace(soa);
  }
  // Suspend thread to build stack trace.
  Thread* thread = Thread::SuspendForDebugger(peer, true, &timeout);
  if (thread != NULL) {
    jobject trace;
    {
      ScopedObjectAccess soa(env);
      trace = thread->CreateInternalStackTrace(soa);
    }
    // Restart suspended thread.
    Runtime::Current()->GetThreadList()->Resume(thread, true);
    return trace;
  } else {
    if (timeout) {
      LOG(ERROR) << "Trying to get thread's stack failed as the thread failed to suspend within a "
          "generous timeout.";
    }
    return NULL;
  }
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

// Returns the defining class loader of the caller's caller.
static jobject VMStack_getCallingClassLoader(JNIEnv* env, jclass) {
  ScopedObjectAccess soa(env);
  NthCallerVisitor visitor(soa.Self()->GetManagedStack(), soa.Self()->GetInstrumentationStack(), 2);
  visitor.WalkStack();
  return soa.AddLocalReference<jobject>(visitor.caller->GetDeclaringClass()->GetClassLoader());
}

static jobject VMStack_getClosestUserClassLoader(JNIEnv* env, jclass, jobject javaBootstrap, jobject javaSystem) {
  struct ClosestUserClassLoaderVisitor : public StackVisitor {
    ClosestUserClassLoaderVisitor(const ManagedStack* stack,
                                  const std::vector<InstrumentationStackFrame>* instrumentation_stack,
                                  Object* bootstrap, Object* system)
      : StackVisitor(stack, instrumentation_stack, NULL),
        bootstrap(bootstrap), system(system), class_loader(NULL) {}

    bool VisitFrame() {
      DCHECK(class_loader == NULL);
      Class* c = GetMethod()->GetDeclaringClass();
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
  ScopedObjectAccess soa(env);
  Object* bootstrap = soa.Decode<Object*>(javaBootstrap);
  Object* system = soa.Decode<Object*>(javaSystem);
  ClosestUserClassLoaderVisitor visitor(soa.Self()->GetManagedStack(), soa.Self()->GetInstrumentationStack(),
                                        bootstrap, system);
  visitor.WalkStack();
  return soa.AddLocalReference<jobject>(visitor.class_loader);
}

// Returns the class of the caller's caller's caller.
static jclass VMStack_getStackClass2(JNIEnv* env, jclass) {
  ScopedObjectAccess soa(env);
  NthCallerVisitor visitor(soa.Self()->GetManagedStack(), soa.Self()->GetInstrumentationStack(), 3);
  visitor.WalkStack();
  return soa.AddLocalReference<jclass>(visitor.caller->GetDeclaringClass());
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
  REGISTER_NATIVE_METHODS("dalvik/system/VMStack");
}

}  // namespace art
