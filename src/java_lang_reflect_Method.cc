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

jint Method_getMethodModifiers(JNIEnv* env, jclass, jclass javaDeclaringClass, jobject jmethod, jint slot) {
  Method* m = Decode<Object*>(env, jmethod)->AsMethod();
  jint access_flags = m->GetAccessFlags();
  // We move the DECLARED_SYNCHRONIZED flag into the SYNCHRONIZED
  // position, because the callers of this function are trying to convey
  // the "traditional" meaning of the flags to their callers.
  access_flags &= ~kAccSynchronized;
  if ((access_flags & kAccDeclaredSynchronized) != 0) {
    access_flags |= kAccSynchronized;
  }
  return access_flags & kAccMethodFlagsMask;
}

jobject Method_invokeNative(JNIEnv* env, jobject javaMethod, jobject javaReceiver, jobject javaArgs, jclass, jobject javaParams, jclass, jint, jboolean) {
  return InvokeMethod(env, javaMethod, javaReceiver, javaArgs, javaParams);
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
