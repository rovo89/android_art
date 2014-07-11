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

#include "gc/accounting/card_table-inl.h"
#include "jni_internal.h"
#include "mirror/array.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "scoped_fast_native_object_access.h"

namespace art {

static jboolean Unsafe_compareAndSwapInt(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                         jint expectedValue, jint newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  bool success = obj->CasFieldStrongSequentiallyConsistent32<false>(MemberOffset(offset),
                                                                    expectedValue, newValue);
  return success ? JNI_TRUE : JNI_FALSE;
}

static jboolean Unsafe_compareAndSwapLong(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                          jlong expectedValue, jlong newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  bool success = obj->CasFieldStrongSequentiallyConsistent64<false>(MemberOffset(offset),
                                                                    expectedValue, newValue);
  return success ? JNI_TRUE : JNI_FALSE;
}

static jboolean Unsafe_compareAndSwapObject(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                            jobject javaExpectedValue, jobject javaNewValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* expectedValue = soa.Decode<mirror::Object*>(javaExpectedValue);
  mirror::Object* newValue = soa.Decode<mirror::Object*>(javaNewValue);
  // JNI must use non transactional mode.
  bool success = obj->CasFieldStrongSequentiallyConsistentObject<false>(MemberOffset(offset),
                                                                        expectedValue, newValue);
  return success ? JNI_TRUE : JNI_FALSE;
}

static jint Unsafe_getInt(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  return obj->GetField32(MemberOffset(offset));
}

static jint Unsafe_getIntVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  return obj->GetField32Volatile(MemberOffset(offset));
}

static void Unsafe_putInt(JNIEnv* env, jobject, jobject javaObj, jlong offset, jint newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  obj->SetField32<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putIntVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                  jint newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  obj->SetField32Volatile<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putOrderedInt(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                 jint newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  QuasiAtomic::ThreadFenceRelease();
  // JNI must use non transactional mode.
  obj->SetField32<false>(MemberOffset(offset), newValue);
}

static jlong Unsafe_getLong(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  return obj->GetField64(MemberOffset(offset));
}

static jlong Unsafe_getLongVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  return obj->GetField64Volatile(MemberOffset(offset));
}

static void Unsafe_putLong(JNIEnv* env, jobject, jobject javaObj, jlong offset, jlong newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  obj->SetField64<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putLongVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                   jlong newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  obj->SetField64Volatile<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putOrderedLong(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                  jlong newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  QuasiAtomic::ThreadFenceRelease();
  // JNI must use non transactional mode.
  obj->SetField64<false>(MemberOffset(offset), newValue);
}

static jobject Unsafe_getObjectVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* value = obj->GetFieldObjectVolatile<mirror::Object>(MemberOffset(offset));
  return soa.AddLocalReference<jobject>(value);
}

static jobject Unsafe_getObject(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* value = obj->GetFieldObject<mirror::Object>(MemberOffset(offset));
  return soa.AddLocalReference<jobject>(value);
}

static void Unsafe_putObject(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                             jobject javaNewValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* newValue = soa.Decode<mirror::Object*>(javaNewValue);
  // JNI must use non transactional mode.
  obj->SetFieldObject<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putObjectVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                     jobject javaNewValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* newValue = soa.Decode<mirror::Object*>(javaNewValue);
  // JNI must use non transactional mode.
  obj->SetFieldObjectVolatile<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putOrderedObject(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                    jobject javaNewValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* newValue = soa.Decode<mirror::Object*>(javaNewValue);
  QuasiAtomic::ThreadFenceRelease();
  // JNI must use non transactional mode.
  obj->SetFieldObject<false>(MemberOffset(offset), newValue);
}

static jint Unsafe_getArrayBaseOffsetForComponentType(JNIEnv* env, jclass, jobject component_class) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Class* component = soa.Decode<mirror::Class*>(component_class);
  Primitive::Type primitive_type = component->GetPrimitiveType();
  return mirror::Array::DataOffset(Primitive::ComponentSize(primitive_type)).Int32Value();
}

static jint Unsafe_getArrayIndexScaleForComponentType(JNIEnv* env, jclass, jobject component_class) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Class* component = soa.Decode<mirror::Class*>(component_class);
  Primitive::Type primitive_type = component->GetPrimitiveType();
  return Primitive::ComponentSize(primitive_type);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Unsafe, compareAndSwapInt, "!(Ljava/lang/Object;JII)Z"),
  NATIVE_METHOD(Unsafe, compareAndSwapLong, "!(Ljava/lang/Object;JJJ)Z"),
  NATIVE_METHOD(Unsafe, compareAndSwapObject, "!(Ljava/lang/Object;JLjava/lang/Object;Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Unsafe, getIntVolatile, "!(Ljava/lang/Object;J)I"),
  NATIVE_METHOD(Unsafe, putIntVolatile, "!(Ljava/lang/Object;JI)V"),
  NATIVE_METHOD(Unsafe, getLongVolatile, "!(Ljava/lang/Object;J)J"),
  NATIVE_METHOD(Unsafe, putLongVolatile, "!(Ljava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, getObjectVolatile, "!(Ljava/lang/Object;J)Ljava/lang/Object;"),
  NATIVE_METHOD(Unsafe, putObjectVolatile, "!(Ljava/lang/Object;JLjava/lang/Object;)V"),
  NATIVE_METHOD(Unsafe, getInt, "!(Ljava/lang/Object;J)I"),
  NATIVE_METHOD(Unsafe, putInt, "!(Ljava/lang/Object;JI)V"),
  NATIVE_METHOD(Unsafe, putOrderedInt, "!(Ljava/lang/Object;JI)V"),
  NATIVE_METHOD(Unsafe, getLong, "!(Ljava/lang/Object;J)J"),
  NATIVE_METHOD(Unsafe, putLong, "!(Ljava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, putOrderedLong, "!(Ljava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, getObject, "!(Ljava/lang/Object;J)Ljava/lang/Object;"),
  NATIVE_METHOD(Unsafe, putObject, "!(Ljava/lang/Object;JLjava/lang/Object;)V"),
  NATIVE_METHOD(Unsafe, putOrderedObject, "!(Ljava/lang/Object;JLjava/lang/Object;)V"),
  NATIVE_METHOD(Unsafe, getArrayBaseOffsetForComponentType, "!(Ljava/lang/Class;)I"),
  NATIVE_METHOD(Unsafe, getArrayIndexScaleForComponentType, "!(Ljava/lang/Class;)I"),
};

void register_sun_misc_Unsafe(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("sun/misc/Unsafe");
}

}  // namespace art
