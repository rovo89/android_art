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

namespace art {

static jlong Unsafe_objectFieldOffset0(JNIEnv* env, jclass, jobject javaField) {
  // TODO: move to Java code
  jfieldID fid = env->FromReflectedField(javaField);
  Field* field = DecodeField(fid);
  return field->GetOffset().Int32Value();
}

static jint Unsafe_arrayBaseOffset0(JNIEnv* env, jclass, jclass javaArrayClass) {
  // TODO: move to Java code
  ScopedThreadStateChange tsc(Thread::Current(), kRunnable);
  Class* array_class = Decode<Class*>(env, javaArrayClass);
  return Array::DataOffset(array_class->GetComponentSize()).Int32Value();
}

static jint Unsafe_arrayIndexScale0(JNIEnv* env, jclass, jclass javaClass) {
  Class* c = Decode<Class*>(env, javaClass);
  return c->GetComponentSize();
}

static jboolean Unsafe_compareAndSwapInt(JNIEnv* env, jobject, jobject javaObj, jlong offset, jint expectedValue, jint newValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  byte* raw_addr = reinterpret_cast<byte*>(obj) + offset;
  volatile int32_t* address = reinterpret_cast<volatile int32_t*>(raw_addr);
  // Note: android_atomic_release_cas() returns 0 on success, not failure.
  int result = android_atomic_release_cas(expectedValue, newValue, address);
  return (result == 0);
}

static jboolean Unsafe_compareAndSwapLong(JNIEnv* env, jobject, jobject javaObj, jlong offset, jlong expectedValue, jlong newValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  byte* raw_addr = reinterpret_cast<byte*>(obj) + offset;
  volatile int64_t* address = reinterpret_cast<volatile int64_t*>(raw_addr);
  // Note: android_atomic_cmpxchg() returns 0 on success, not failure.
  int result = QuasiAtomic::Cas64(expectedValue, newValue, address);
  return (result == 0);
}

static jboolean Unsafe_compareAndSwapObject(JNIEnv* env, jobject, jobject javaObj, jlong offset, jobject javaExpectedValue, jobject javaNewValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  Object* expectedValue = Decode<Object*>(env, javaExpectedValue);
  Object* newValue = Decode<Object*>(env, javaNewValue);
  byte* raw_addr = reinterpret_cast<byte*>(obj) + offset;
  int32_t* address = reinterpret_cast<int32_t*>(raw_addr);
  // Note: android_atomic_cmpxchg() returns 0 on success, not failure.
  int result = android_atomic_release_cas(reinterpret_cast<int32_t>(expectedValue),
      reinterpret_cast<int32_t>(newValue), address);
  if (result == 0) {
    Runtime::Current()->GetHeap()->WriteBarrierField(obj, MemberOffset(offset), newValue);
  }
  return (result == 0);
}

static jint Unsafe_getInt(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  Object* obj = Decode<Object*>(env, javaObj);
  return obj->GetField32(MemberOffset(offset), false);
}

static jint Unsafe_getIntVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  Object* obj = Decode<Object*>(env, javaObj);
  byte* raw_addr = reinterpret_cast<byte*>(obj) + offset;
  volatile int32_t* address = reinterpret_cast<volatile int32_t*>(raw_addr);
  return android_atomic_acquire_load(address);
}

static void Unsafe_putInt(JNIEnv* env, jobject, jobject javaObj, jlong offset, jint newValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  obj->SetField32(MemberOffset(offset), newValue, false);
}

static void Unsafe_putIntVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset, jint newValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  byte* raw_addr = reinterpret_cast<byte*>(obj) + offset;
  volatile int32_t* address = reinterpret_cast<volatile int32_t*>(raw_addr);
  android_atomic_release_store(newValue, address);
}

static void Unsafe_putOrderedInt(JNIEnv* env, jobject, jobject javaObj, jlong offset, jint newValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  ANDROID_MEMBAR_STORE();
  obj->SetField32(MemberOffset(offset), newValue, false);
}

static jlong Unsafe_getLong(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  Object* obj = Decode<Object*>(env, javaObj);
  byte* raw_addr = reinterpret_cast<byte*>(obj) + offset;
  int64_t* address = reinterpret_cast<int64_t*>(raw_addr);
  return *address;
}

static jlong Unsafe_getLongVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  Object* obj = Decode<Object*>(env, javaObj);
  return obj->GetField64(MemberOffset(offset), true);
}

static void Unsafe_putLong(JNIEnv* env, jobject, jobject javaObj, jlong offset, jlong newValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  obj->SetField64(MemberOffset(offset), newValue, false);
}

static void Unsafe_putLongVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset, jlong newValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  obj->SetField64(MemberOffset(offset), newValue, true);
}

static void Unsafe_putOrderedLong(JNIEnv* env, jobject, jobject javaObj, jlong offset, jlong newValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  ANDROID_MEMBAR_STORE();
  obj->SetField64(MemberOffset(offset), newValue, false);
}

static jobject Unsafe_getObjectVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  Object* obj = Decode<Object*>(env, javaObj);
  Object* value = obj->GetFieldObject<Object*>(MemberOffset(offset), true);
  return AddLocalReference<jobject>(env, value);
}

static jobject Unsafe_getObject(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  Object* obj = Decode<Object*>(env, javaObj);
  Object* value = obj->GetFieldObject<Object*>(MemberOffset(offset), false);
  return AddLocalReference<jobject>(env, value);
}

static void Unsafe_putObject(JNIEnv* env, jobject, jobject javaObj, jlong offset, jobject javaNewValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  Object* newValue = Decode<Object*>(env, javaNewValue);
  obj->SetFieldObject(MemberOffset(offset), newValue, false);
}

static void Unsafe_putObjectVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset, jobject javaNewValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  Object* newValue = Decode<Object*>(env, javaNewValue);
  obj->SetFieldObject(MemberOffset(offset), newValue, true);
}

static void Unsafe_putOrderedObject(JNIEnv* env, jobject, jobject javaObj, jlong offset, jobject javaNewValue) {
  Object* obj = Decode<Object*>(env, javaObj);
  Object* newValue = Decode<Object*>(env, javaNewValue);
  ANDROID_MEMBAR_STORE();
  obj->SetFieldObject(MemberOffset(offset), newValue, false);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Unsafe, objectFieldOffset0, "(Ljava/lang/reflect/Field;)J"),
  NATIVE_METHOD(Unsafe, arrayBaseOffset0, "(Ljava/lang/Class;)I"),
  NATIVE_METHOD(Unsafe, arrayIndexScale0, "(Ljava/lang/Class;)I"),
  NATIVE_METHOD(Unsafe, compareAndSwapInt, "(Ljava/lang/Object;JII)Z"),
  NATIVE_METHOD(Unsafe, compareAndSwapLong, "(Ljava/lang/Object;JJJ)Z"),
  NATIVE_METHOD(Unsafe, compareAndSwapObject, "(Ljava/lang/Object;JLjava/lang/Object;Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Unsafe, getIntVolatile, "(Ljava/lang/Object;J)I"),
  NATIVE_METHOD(Unsafe, putIntVolatile, "(Ljava/lang/Object;JI)V"),
  NATIVE_METHOD(Unsafe, getLongVolatile, "(Ljava/lang/Object;J)J"),
  NATIVE_METHOD(Unsafe, putLongVolatile, "(Ljava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, getObjectVolatile, "(Ljava/lang/Object;J)Ljava/lang/Object;"),
  NATIVE_METHOD(Unsafe, putObjectVolatile, "(Ljava/lang/Object;JLjava/lang/Object;)V"),
  NATIVE_METHOD(Unsafe, getInt, "(Ljava/lang/Object;J)I"),
  NATIVE_METHOD(Unsafe, putInt, "(Ljava/lang/Object;JI)V"),
  NATIVE_METHOD(Unsafe, putOrderedInt, "(Ljava/lang/Object;JI)V"),
  NATIVE_METHOD(Unsafe, getLong, "(Ljava/lang/Object;J)J"),
  NATIVE_METHOD(Unsafe, putLong, "(Ljava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, putOrderedLong, "(Ljava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, getObject, "(Ljava/lang/Object;J)Ljava/lang/Object;"),
  NATIVE_METHOD(Unsafe, putObject, "(Ljava/lang/Object;JLjava/lang/Object;)V"),
  NATIVE_METHOD(Unsafe, putOrderedObject, "(Ljava/lang/Object;JLjava/lang/Object;)V"),
};

void register_sun_misc_Unsafe(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("sun/misc/Unsafe");
}

}  // namespace art
