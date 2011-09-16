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

jint Field_getFieldModifiers(JNIEnv* env, jobject jfield, jclass javaDeclaringClass, jint slot) {
  return Decode<Object*>(env, jfield)->AsField()->GetAccessFlags() & kAccFieldFlagsMask;
}

static JNINativeMethod gMethods[] = {
  //NATIVE_METHOD(Field, getAnnotation, "(Ljava/lang/Class;ILjava/lang/Class;)Ljava/lang/annotation/Annotation;"),
  //NATIVE_METHOD(Field, getBField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)B"),
  //NATIVE_METHOD(Field, getCField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)C"),
  //NATIVE_METHOD(Field, getDField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)D"),
  //NATIVE_METHOD(Field, getDeclaredAnnotations, "(Ljava/lang/Class;I)[Ljava/lang/annotation/Annotation;"),
  //NATIVE_METHOD(Field, getFField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)F"),
  //NATIVE_METHOD(Field, getField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZ)Ljava/lang/Object;"),
  NATIVE_METHOD(Field, getFieldModifiers, "(Ljava/lang/Class;I)I"),
  //NATIVE_METHOD(Field, getIField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)I"),
  //NATIVE_METHOD(Field, getJField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)J"),
  //NATIVE_METHOD(Field, getSField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)S"),
  //NATIVE_METHOD(Field, getSignatureAnnotation, "(Ljava/lang/Class;I)[Ljava/lang/Object;"),
  //NATIVE_METHOD(Field, getZField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)Z"),
  //NATIVE_METHOD(Field, isAnnotationPresent, "(Ljava/lang/Class;ILjava/lang/Class;)Z"),
  //NATIVE_METHOD(Field, setBField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCB)V"),
  //NATIVE_METHOD(Field, setCField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCC)V"),
  //NATIVE_METHOD(Field, setDField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCD)V"),
  //NATIVE_METHOD(Field, setFField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCF)V"),
  //NATIVE_METHOD(Field, setField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZLjava/lang/Object;)V"),
  //NATIVE_METHOD(Field, setIField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCI)V"),
  //NATIVE_METHOD(Field, setJField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCJ)V"),
  //NATIVE_METHOD(Field, setSField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCS)V"),
  //NATIVE_METHOD(Field, setZField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCZ)V"),
};

}  // namespace

void register_java_lang_reflect_Field(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/reflect/Field", gMethods, NELEM(gMethods));
}

}  // namespace art
