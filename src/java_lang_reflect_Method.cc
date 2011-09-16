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

static JNINativeMethod gMethods[] = {
  //NATIVE_METHOD(Method, getAnnotation, "(Ljava/lang/Class;ILjava/lang/Class;)Ljava/lang/annotation/Annotation;"),
  //NATIVE_METHOD(Method, getDeclaredAnnotations, "(Ljava/lang/Class;I)[Ljava/lang/annotation/Annotation;"),
  //NATIVE_METHOD(Method, getDefaultValue, "(Ljava/lang/Class;I)Ljava/lang/Object;"),
  NATIVE_METHOD(Method, getMethodModifiers, "(Ljava/lang/Class;Ljava/lang/reflect/AccessibleObject;I)I"),
  //NATIVE_METHOD(Method, getParameterAnnotations, "(Ljava/lang/Class;I)[[Ljava/lang/annotation/Annotation;"),
  //NATIVE_METHOD(Method, getSignatureAnnotation, "(Ljava/lang/Class;I)[Ljava/lang/Object;"),
  //NATIVE_METHOD(Method, invokeNative, "(Ljava/lang/Object;[Ljava/lang/Object;Ljava/lang/Class;[Ljava/lang/Class;Ljava/lang/Class;IZ)Ljava/lang/Object;"),
  //NATIVE_METHOD(Method, isAnnotationPresent, "(Ljava/lang/Class;ILjava/lang/Class;)Z"),
};

}  // namespace

void register_java_lang_reflect_Method(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/reflect/Method", gMethods, NELEM(gMethods));
}

}  // namespace art
