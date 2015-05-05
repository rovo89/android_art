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
static jint Unsafe_addressSize(JNIEnv* env, jobject) {
  return sizeof(void*);
}

static jint Unsafe_pageSize(JNIEnv* env, jobject) {
  return sysconf(_SC_PAGESIZE);
}

static jlong Unsafe_allocateMemory(JNIEnv* env, jobject, jlong bytes) {
  ScopedFastNativeObjectAccess soa(env);
  // bytes is nonnegative and fits into size_t
  if (bytes < 0 || bytes != (jlong)(size_t) bytes) {
    ThrowIllegalAccessException(nullptr, "wrong number of bytes");
    return 0;
  }
  void* mem = malloc(bytes);
  if (mem == nullptr) {
    soa.Self()->ThrowOutOfMemoryError("native alloc");
    return 0;
  }
  return (uintptr_t) mem;
}

static void Unsafe_freeMemory(JNIEnv* env, jobject, jlong address) {
  free((void*)(uintptr_t)address);
}

static void Unsafe_setMemory(JNIEnv* env, jobject, jlong address, jlong bytes, jbyte value) {
  memset((void*)(uintptr_t)address, value, bytes);
}

static jbyte Unsafe_getByte$(JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jbyte*>(address);
}

static void Unsafe_putByte$(JNIEnv* env, jobject, jlong address, jbyte value) {
  *reinterpret_cast<jbyte*>(address) = value;
}

static jshort Unsafe_getShort$(JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jshort*>(address);
}

static void Unsafe_putShort$(JNIEnv* env, jobject, jlong address, jshort value) {
  *reinterpret_cast<jshort*>(address) = value;
}

static jchar Unsafe_getChar$(JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jchar*>(address);
}

static void Unsafe_putChar$(JNIEnv* env, jobject, jlong address, jchar value) {
  *reinterpret_cast<jchar*>(address) = value;
}

static jint Unsafe_getInt$(JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jint*>(address);
}

static void Unsafe_putInt$(JNIEnv* env, jobject, jlong address, jint value) {
  *reinterpret_cast<jint*>(address) = value;
}

static jlong Unsafe_getLong$(JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jlong*>(address);
}

static void Unsafe_putLong$(JNIEnv* env, jobject, jlong address, jlong value) {
  *reinterpret_cast<jlong*>(address) = value;
}

static jfloat Unsafe_getFloat$(JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jfloat*>(address);
}

static void Unsafe_putFloat$(JNIEnv* env, jobject, jlong address, jfloat value) {
  *reinterpret_cast<jfloat*>(address) = value;
}
static jdouble Unsafe_getDouble$(JNIEnv* env, jobject, jlong address) {
  return *reinterpret_cast<jdouble*>(address);
}

static void Unsafe_putDouble$(JNIEnv* env, jobject, jlong address, jdouble value) {
  *reinterpret_cast<jdouble*>(address) = value;
}

static jlong Unsafe_getAddress(JNIEnv* env, jobject, jlong address) {
  void* p = (void*)(uintptr_t)address;
  return (uintptr_t)(*(void**)p);
}

static void Unsafe_copyMemory(JNIEnv *env, jobject unsafe, jlong src, jlong dst, jlong size) {
    if (size == 0) {
        return;
    }
    // size is nonnegative and fits into size_t
    if (size < 0 || size != (jlong)(size_t) size) {
        ScopedFastNativeObjectAccess soa(env);
        ThrowIllegalAccessException(nullptr, "wrong number of bytes");
    }
    size_t sz = (size_t)size;
    memcpy(reinterpret_cast<void *>(dst), reinterpret_cast<void *>(src), sz);
}

template<typename T>
static void copyToArray(jlong srcAddr, mirror::PrimitiveArray<T>* array, size_t size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const T* src = reinterpret_cast<T*>(srcAddr);
    size_t sz = size / sizeof(T);
    for (size_t i = 0; i < sz; ++i) {
        array->Set(i, *(src + i));
    }
}

template<typename T>
static void copyFromArray(jlong dstAddr, mirror::PrimitiveArray<T>* array, size_t size)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    T* dst = reinterpret_cast<T*>(dstAddr);
    size_t sz = size / sizeof(T);
    for (size_t i = 0; i < sz; ++i) {
        *(dst + i) = array->Get(i);
    }
}

static void Unsafe_copyMemoryToPrimitiveArray(JNIEnv *env,
                                              jobject unsafe,
                                              jlong srcAddr,
                                              jobject dstObj,
                                              jlong dstOffset,
                                              jlong size) {
    ScopedObjectAccess soa(env);
    if (size == 0) {
        return;
    }
    // size is nonnegative and fits into size_t
    if (size < 0 || size != (jlong)(size_t) size) {
        ThrowIllegalAccessException(nullptr, "wrong number of bytes");
    }
    size_t sz = (size_t)size;
    mirror::Object* dst = soa.Decode<mirror::Object*>(dstObj);
    mirror::Class* component_type = dst->GetClass()->GetComponentType();
    if (component_type->IsPrimitiveByte() || component_type->IsPrimitiveBoolean()) {
        copyToArray(srcAddr, dst->AsByteSizedArray(), sz);
    } else if (component_type->IsPrimitiveShort() || component_type->IsPrimitiveChar()) {
        copyToArray(srcAddr, dst->AsShortSizedArray(), sz);
    } else if (component_type->IsPrimitiveInt() || component_type->IsPrimitiveFloat()) {
        copyToArray(srcAddr, dst->AsIntArray(), sz);
    } else if (component_type->IsPrimitiveLong() || component_type->IsPrimitiveDouble()) {
        copyToArray(srcAddr, dst->AsLongArray(), sz);
    } else {
        ThrowIllegalAccessException(nullptr, "not a primitive array");
    }
}

static void Unsafe_copyMemoryFromPrimitiveArray(JNIEnv *env,
                                                jobject unsafe,
                                                jobject srcObj,
                                                jlong srcOffset,
                                                jlong dstAddr,
                                                jlong size) {
    ScopedObjectAccess soa(env);
    if (size == 0) {
        return;
    }
    // size is nonnegative and fits into size_t
    if (size < 0 || size != (jlong)(size_t) size) {
        ThrowIllegalAccessException(nullptr, "wrong number of bytes");
    }
    size_t sz = (size_t)size;
    mirror::Object* src = soa.Decode<mirror::Object*>(srcObj);
    mirror::Class* component_type = src->GetClass()->GetComponentType();
    if (component_type->IsPrimitiveByte() || component_type->IsPrimitiveBoolean()) {
        copyFromArray(dstAddr, src->AsByteSizedArray(), sz);
    } else if (component_type->IsPrimitiveShort() || component_type->IsPrimitiveChar()) {
        copyFromArray(dstAddr, src->AsShortSizedArray(), sz);
    } else if (component_type->IsPrimitiveInt() || component_type->IsPrimitiveFloat()) {
        copyFromArray(dstAddr, src->AsIntArray(), sz);
    } else if (component_type->IsPrimitiveLong() || component_type->IsPrimitiveDouble()) {
        copyFromArray(dstAddr, src->AsLongArray(), sz);
    } else {
        ThrowIllegalAccessException(nullptr, "not a primitive array");
    }
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
  NATIVE_METHOD(Unsafe, addressSize, "!()I"),
  NATIVE_METHOD(Unsafe, pageSize, "!()I"),
  NATIVE_METHOD(Unsafe, allocateMemory, "!(J)J"),
  NATIVE_METHOD(Unsafe, freeMemory, "!(J)V"),
  NATIVE_METHOD(Unsafe, setMemory, "!(JJB)V"),
  NATIVE_METHOD(Unsafe, getByte$, "!(J)B"),
  NATIVE_METHOD(Unsafe, putByte$, "!(JB)V"),
  NATIVE_METHOD(Unsafe, getShort$, "!(J)S"),
  NATIVE_METHOD(Unsafe, putShort$, "!(JS)V"),
  NATIVE_METHOD(Unsafe, getChar$, "!(J)C"),
  NATIVE_METHOD(Unsafe, putChar$, "!(JC)V"),
  NATIVE_METHOD(Unsafe, getInt$, "!(J)I"),
  NATIVE_METHOD(Unsafe, putInt$, "!(JI)V"),
  NATIVE_METHOD(Unsafe, getLong$, "!(J)J"),
  NATIVE_METHOD(Unsafe, putLong$, "!(JJ)V"),
  NATIVE_METHOD(Unsafe, getFloat$, "!(J)F"),
  NATIVE_METHOD(Unsafe, putFloat$, "!(JF)V"),
  NATIVE_METHOD(Unsafe, getDouble$, "!(J)D"),
  NATIVE_METHOD(Unsafe, putDouble$, "!(JD)V"),
  NATIVE_METHOD(Unsafe, getAddress, "!(J)J"),
  NATIVE_METHOD(Unsafe, copyMemory, "!(JJJ)V"),
  NATIVE_METHOD(Unsafe, copyMemoryToPrimitiveArray, "!(JLjava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, copyMemoryFromPrimitiveArray, "!(Ljava/lang/Object;JJJ)V"),
};

void register_sun_misc_Unsafe(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("sun/misc/Unsafe");
}

}  // namespace art
