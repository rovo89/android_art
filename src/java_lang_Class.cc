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

jclass Class_getComponentType(JNIEnv* env, jobject javaThis) {
  Class* c = Decode<Class*>(env, javaThis);
  if (!c->IsArrayClass()) {
    return NULL;
  }

  /*
   * We can't just return c->GetComponentType(), because that gives
   * us the base type (e.g. X[][][] returns X).  If this is a multi-
   * dimensional array, we have to do the lookup by name.
   */
  Class* result;
  std::string descriptor(c->GetDescriptor()->ToModifiedUtf8());
  if (descriptor[1] == '[') {
    result = Runtime::Current()->GetClassLinker()->FindClass(descriptor.c_str() + 1, c->GetClassLoader());
  } else {
    result = c->GetComponentType();
  }
  return AddLocalReference<jclass>(env, result);

}

jobjectArray Class_getDeclaredClasses(JNIEnv* env, jclass java_lang_Class_class, jclass c, jboolean publicOnly) {
  UNIMPLEMENTED(WARNING);
  return env->NewObjectArray(0, java_lang_Class_class, NULL);
}

static JNINativeMethod gMethods[] = {
  //NATIVE_METHOD(Class, classForName, "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;"),
  //NATIVE_METHOD(Class, desiredAssertionStatus, "()Z"),
  //NATIVE_METHOD(Class, getClassLoader, "(Ljava/lang/Class;)Ljava/lang/ClassLoader;"),
  NATIVE_METHOD(Class, getComponentType, "()Ljava/lang/Class;"),
  //NATIVE_METHOD(Class, getDeclaredAnnotation, "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;"),
  //NATIVE_METHOD(Class, getDeclaredAnnotations, "()[Ljava/lang/annotation/Annotation;"),
  NATIVE_METHOD(Class, getDeclaredClasses, "(Ljava/lang/Class;Z)[Ljava/lang/Class;"),
  //NATIVE_METHOD(Class, getDeclaredConstructorOrMethod, "(Ljava/lang/Class;Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Member;"),
  //NATIVE_METHOD(Class, getDeclaredConstructors, "(Ljava/lang/Class;Z)[Ljava/lang/reflect/Constructor;"),
  //NATIVE_METHOD(Class, getDeclaredField, "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Field;"),
  //NATIVE_METHOD(Class, getDeclaredFields, "(Ljava/lang/Class;Z)[Ljava/lang/reflect/Field;"),
  //NATIVE_METHOD(Class, getDeclaredMethods, "(Ljava/lang/Class;Z)[Ljava/lang/reflect/Method;"),
  //NATIVE_METHOD(Class, getDeclaringClass, "()Ljava/lang/Class;"),
  //NATIVE_METHOD(Class, getEnclosingClass, "()Ljava/lang/Class;"),
  //NATIVE_METHOD(Class, getEnclosingConstructor, "()Ljava/lang/reflect/Constructor;"),
  //NATIVE_METHOD(Class, getEnclosingMethod, "()Ljava/lang/reflect/Method;"),
  //NATIVE_METHOD(Class, getInnerClassName, "()Ljava/lang/String;"),
  //NATIVE_METHOD(Class, getInterfaces, "()[Ljava/lang/Class;"),
  //NATIVE_METHOD(Class, getModifiers, "(Ljava/lang/Class;Z)I"),
  //NATIVE_METHOD(Class, getNameNative, "()Ljava/lang/String;"),
  //NATIVE_METHOD(Class, getSignatureAnnotation, "()[Ljava/lang/Object;"),
  //NATIVE_METHOD(Class, getSuperclass, "()Ljava/lang/Class;"),
  //NATIVE_METHOD(Class, isAnonymousClass, "()Z"),
  //NATIVE_METHOD(Class, isAssignableFrom, "(Ljava/lang/Class;)Z"),
  //NATIVE_METHOD(Class, isDeclaredAnnotationPresent, "(Ljava/lang/Class;)Z"),
  //NATIVE_METHOD(Class, isInstance, "(Ljava/lang/Object;)Z"),
  //NATIVE_METHOD(Class, isInterface, "()Z"),
  //NATIVE_METHOD(Class, isPrimitive, "()Z"),
  //NATIVE_METHOD(Class, newInstanceImpl, "()Ljava/lang/Object;"),
};

}  // namespace

void register_java_lang_Class(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/Class", gMethods, NELEM(gMethods));
}

}  // namespace art
