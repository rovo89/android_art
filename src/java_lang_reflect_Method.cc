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
#include "class_linker.h"
#include "object.h"
#include "reflection.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

// We move the DECLARED_SYNCHRONIZED flag into the SYNCHRONIZED
// position, because the callers of this function are trying to convey
// the "traditional" meaning of the flags to their callers.
uint32_t FixupMethodFlags(uint32_t access_flags) {
  access_flags &= ~kAccSynchronized;
  if ((access_flags & kAccDeclaredSynchronized) != 0) {
    access_flags |= kAccSynchronized;
  }
  return access_flags & kAccMethodFlagsMask;
}

jint Method_getMethodModifiers(JNIEnv* env, jclass, jclass javaDeclaringClass, jobject jmethod, jint slot) {
  return FixupMethodFlags(Decode<Object*>(env, jmethod)->AsMethod()->GetAccessFlags());
}

jobject Method_invokeNative(JNIEnv* env, jobject javaMethod, jobject javaReceiver, jobject javaArgs, jclass javaDeclaringClass, jobject javaParams, jclass, jint, jboolean) {
  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, Thread::kRunnable);

  jmethodID mid = env->FromReflectedMethod(javaMethod);
  Method* m = reinterpret_cast<Method*>(mid);
  Object* receiver = NULL;
  if (!m->IsStatic()) {
    // Check that the receiver is non-null and an instance of the field's declaring class.
    receiver = Decode<Object*>(env, javaReceiver);
    Class* declaringClass = Decode<Class*>(env, javaDeclaringClass);
    if (!VerifyObjectInClass(env, receiver, declaringClass)) {
      return NULL;
    }

    // Find the actual implementation of the virtual method.
    m = receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(m);
  }

  // Get our arrays of arguments and their types, and check they're the same size.
  ObjectArray<Object>* objects = Decode<ObjectArray<Object>*>(env, javaArgs);
  ObjectArray<Class>* classes = Decode<ObjectArray<Class>*>(env, javaParams);
  int32_t arg_count = (objects != NULL) ? objects->GetLength() : 0;
  if (arg_count != classes->GetLength()) {
    self->ThrowNewException("Ljava/lang/IllegalArgumentException;",
        "wrong number of arguments; expected %d, got %d",
        classes->GetLength(), arg_count);
    return NULL;
  }

  // Translate javaArgs to a jvalue[].
  UniquePtr<jvalue[]> args(new jvalue[arg_count]);
  JValue* decoded_args = reinterpret_cast<JValue*>(args.get());
  for (int32_t i = 0; i < arg_count; ++i) {
    Object* arg = objects->Get(i);
    Class* dst_class = classes->Get(i);
    if (dst_class->IsPrimitive()) {
      if (!UnboxPrimitive(env, arg, dst_class, decoded_args[i])) {
        return NULL;
      }
    } else {
      args[i].l = AddLocalReference<jobject>(env, arg);
    }
  }

  // Invoke the method.
  JValue value = InvokeWithJValues(env, javaReceiver, mid, args.get());

  // Wrap any exception with "Ljava/lang/reflect/InvocationTargetException;" and return early.
  if (self->IsExceptionPending()) {
    jthrowable th = env->ExceptionOccurred();
    env->ExceptionClear();
    jclass exception_class = env->FindClass("java/lang/reflect/InvocationTargetException");
    jmethodID mid = env->GetMethodID(exception_class, "<init>", "(Ljava/lang/Throwable;)V");
    jobject exception_instance = env->NewObject(exception_class, mid, th);
    env->Throw(reinterpret_cast<jthrowable>(exception_instance));
    return NULL;
  }

  // Box if necessary and return.
  BoxPrimitive(env, m->GetReturnType(), value);
  return AddLocalReference<jobject>(env, value.l);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Method, getMethodModifiers, "(Ljava/lang/Class;Ljava/lang/reflect/AccessibleObject;I)I"),
  NATIVE_METHOD(Method, invokeNative, "(Ljava/lang/Object;[Ljava/lang/Object;Ljava/lang/Class;[Ljava/lang/Class;Ljava/lang/Class;IZ)Ljava/lang/Object;"),
};

}  // namespace

void register_java_lang_reflect_Method(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/reflect/Method", gMethods, NELEM(gMethods));
}

}  // namespace art
