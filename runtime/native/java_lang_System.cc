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

#include "common_throws.h"
#include "gc/accounting/card_table-inl.h"
#include "jni_internal.h"
#include "mirror/array.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "scoped_fast_native_object_access.h"

namespace art {

// Template to convert general array to that of its specific primitive type.
template <typename T>
inline T* AsPrimitiveArray(mirror::Array* array) {
  return down_cast<T*>(array);
}

template <typename T, Primitive::Type kPrimType>
inline void System_arraycopyTUnchecked(JNIEnv* env, jobject javaSrc, jint srcPos,
                                       jobject javaDst, jint dstPos, jint count) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* srcObject = soa.Decode<mirror::Object*>(javaSrc);
  mirror::Object* dstObject = soa.Decode<mirror::Object*>(javaDst);
  DCHECK(dstObject != nullptr);
  mirror::Array* srcArray = srcObject->AsArray();
  mirror::Array* dstArray = dstObject->AsArray();
  DCHECK_GE(count, 0);
  DCHECK_EQ(srcArray->GetClass(), dstArray->GetClass());
  DCHECK_EQ(srcArray->GetClass()->GetComponentType()->GetPrimitiveType(), kPrimType);
  AsPrimitiveArray<T>(dstArray)->Memmove(dstPos, AsPrimitiveArray<T>(srcArray), srcPos, count);
}

static void System_arraycopyCharUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                          jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::CharArray, Primitive::kPrimChar>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyByteUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                          jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::ByteArray, Primitive::kPrimByte>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyShortUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                           jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::ShortArray, Primitive::kPrimShort>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyIntUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                         jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::IntArray, Primitive::kPrimInt>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyLongUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                          jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::LongArray, Primitive::kPrimLong>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyFloatUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                           jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::FloatArray, Primitive::kPrimFloat>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyDoubleUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                            jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::DoubleArray, Primitive::kPrimDouble>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyBooleanUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                             jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::BooleanArray, Primitive::kPrimBoolean>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(System, arraycopyCharUnchecked, "!([CI[CII)V"),
  NATIVE_METHOD(System, arraycopyByteUnchecked, "!([BI[BII)V"),
  NATIVE_METHOD(System, arraycopyShortUnchecked, "!([SI[SII)V"),
  NATIVE_METHOD(System, arraycopyIntUnchecked, "!([II[III)V"),
  NATIVE_METHOD(System, arraycopyLongUnchecked, "!([JI[JII)V"),
  NATIVE_METHOD(System, arraycopyFloatUnchecked, "!([FI[FII)V"),
  NATIVE_METHOD(System, arraycopyDoubleUnchecked, "!([DI[DII)V"),
  NATIVE_METHOD(System, arraycopyBooleanUnchecked, "!([ZI[ZII)V"),
};

void register_java_lang_System(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/System");
}

}  // namespace art
