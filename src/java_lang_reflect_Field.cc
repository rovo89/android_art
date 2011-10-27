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

jint Field_getFieldModifiers(JNIEnv* env, jobject jfield, jclass javaDeclaringClass, jint slot) {
  return Decode<Object*>(env, jfield)->AsField()->GetAccessFlags() & kAccJavaFlagsMask;
}

bool GetFieldValue(Object* o, Field* f, JValue& value, bool allow_references) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  switch (f->GetPrimitiveType()) {
  case Primitive::kPrimBoolean:
    value.z = f->GetBoolean(o);
    return true;
  case Primitive::kPrimByte:
    value.b = f->GetByte(o);
    return true;
  case Primitive::kPrimChar:
    value.c = f->GetChar(o);
    return true;
  case Primitive::kPrimDouble:
    value.d = f->GetDouble(o);
    return true;
  case Primitive::kPrimFloat:
    value.f = f->GetFloat(o);
    return true;
  case Primitive::kPrimInt:
    value.i = f->GetInt(o);
    return true;
  case Primitive::kPrimLong:
    value.j = f->GetLong(o);
    return true;
  case Primitive::kPrimShort:
    value.s = f->GetShort(o);
    return true;
  case Primitive::kPrimNot:
    if (allow_references) {
      value.l = f->GetObject(o);
      return true;
    }
    // Else break to report an error.
    break;
  case Primitive::kPrimVoid:
    // Never okay.
    break;
  }
  Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
      "Not a primitive field: %s", PrettyField(f).c_str());
  return false;
}

bool CheckReceiver(JNIEnv* env, jobject javaObj, jclass javaDeclaringClass, Field* f, Object*& o) {
  if (f->IsStatic()) {
    o = NULL;
    return true;
  }

  o = Decode<Object*>(env, javaObj);
  Class* declaringClass = Decode<Class*>(env, javaDeclaringClass);
  if (!VerifyObjectInClass(env, o, declaringClass)) {
    return false;
  }
  return true;
}

JValue GetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jchar dst_descriptor) {
  Field* f = DecodeField(env->FromReflectedField(javaField));
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, javaDeclaringClass, f, o)) {
    return JValue();
  }

  // Read the value.
  JValue field_value;
  if (!GetFieldValue(o, f, field_value, false)) {
    return JValue();
  }

  // Widen it if necessary (and possible).
  JValue wide_value;
  Class* dst_type = Runtime::Current()->GetClassLinker()->FindPrimitiveClass(dst_descriptor);
  if (!ConvertPrimitiveValue(f->GetPrimitiveType(), dst_type->GetPrimitiveType(),
                             field_value, wide_value)) {
    return JValue();
  }
  return wide_value;
}

jbyte Field_getBField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar dst_descriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, dst_descriptor).b;
}

jchar Field_getCField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar dst_descriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, dst_descriptor).c;
}

jdouble Field_getDField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar dst_descriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, dst_descriptor).d;
}

jfloat Field_getFField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar dst_descriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, dst_descriptor).f;
}

jint Field_getIField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar dst_descriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, dst_descriptor).i;
}

jlong Field_getJField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar dst_descriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, dst_descriptor).j;
}

jshort Field_getSField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar dst_descriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, dst_descriptor).s;
}

jboolean Field_getZField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar dst_descriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, dst_descriptor).z;
}

void SetFieldValue(Object* o, Field* f, const JValue& new_value, bool allow_references) {
  switch (f->GetPrimitiveType()) {
  case Primitive::kPrimBoolean:
    f->SetBoolean(o, new_value.z);
    break;
  case Primitive::kPrimByte:
    f->SetByte(o, new_value.b);
    break;
  case Primitive::kPrimChar:
    f->SetChar(o, new_value.c);
    break;
  case Primitive::kPrimDouble:
    f->SetDouble(o, new_value.d);
    break;
  case Primitive::kPrimFloat:
    f->SetFloat(o, new_value.f);
    break;
  case Primitive::kPrimInt:
    f->SetInt(o, new_value.i);
    break;
  case Primitive::kPrimLong:
    f->SetLong(o, new_value.j);
    break;
  case Primitive::kPrimShort:
    f->SetShort(o, new_value.s);
    break;
  case Primitive::kPrimNot:
    if (allow_references) {
      f->SetObject(o, new_value.l);
      break;
    }
    // Else fall through to report an error.
  case Primitive::kPrimVoid:
    // Never okay.
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
        "Not a primitive field: %s", PrettyField(f).c_str());
    return;
  }

  // Special handling for final fields on SMP systems.
  // We need a store/store barrier here (JMM requirement).
  if (f->IsFinal()) {
    ANDROID_MEMBAR_STORE();
  }
}

void SetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jchar src_descriptor, const JValue& new_value) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  Field* f = DecodeField(env->FromReflectedField(javaField));
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, javaDeclaringClass, f, o)) {
    return;
  }

  // Widen the value if necessary (and possible).
  JValue wide_value;
  Class* src_type = Runtime::Current()->GetClassLinker()->FindPrimitiveClass(src_descriptor);
  if (!ConvertPrimitiveValue(src_type->GetPrimitiveType(), f->GetPrimitiveType(),
                             new_value, wide_value)) {
    return;
  }

  // Write the value.
  SetFieldValue(o, f, wide_value, false);
}

void Field_setBField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar src_descriptor, jbyte value) {
  JValue v = { 0 };
  v.b = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, src_descriptor, v);
}

void Field_setCField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar src_descriptor, jchar value) {
  JValue v = { 0 };
  v.c = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, src_descriptor, v);
}

void Field_setDField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar src_descriptor, jdouble value) {
  JValue v = { 0 };
  v.d = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, src_descriptor, v);
}

void Field_setFField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar src_descriptor, jfloat value) {
  JValue v = { 0 };
  v.f = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, src_descriptor, v);
}

void Field_setIField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar src_descriptor, jint value) {
  JValue v = { 0 };
  v.i = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, src_descriptor, v);
}

void Field_setJField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar src_descriptor, jlong value) {
  JValue v = { 0 };
  v.j = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, src_descriptor, v);
}

void Field_setSField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar src_descriptor, jshort value) {
  JValue v = { 0 };
  v.s = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, src_descriptor, v);
}

void Field_setZField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar src_descriptor, jboolean value) {
  JValue v = { 0 };
  v.z = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, src_descriptor, v);
}

void Field_setField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jobject javaValue) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  Field* f = DecodeField(env->FromReflectedField(javaField));

  // Unbox the value, if necessary.
  Object* boxed_value = Decode<Object*>(env, javaValue);
  JValue unboxed_value;
  if (!UnboxPrimitive(env, boxed_value, f->GetType(), unboxed_value)) {
    return;
  }

  // Check that the receiver is non-null and an instance of the field's declaring class.
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, javaDeclaringClass, f, o)) {
    return;
  }

  SetFieldValue(o, f, unboxed_value, true);
}

jobject Field_getField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean) {
  Field* f = DecodeField(env->FromReflectedField(javaField));
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, javaDeclaringClass, f, o)) {
    return NULL;
  }

  // Get the field's value, boxing if necessary.
  JValue value;
  if (!GetFieldValue(o, f, value, true)) {
    return NULL;
  }
  BoxPrimitive(env, f->GetPrimitiveType(), value);

  return AddLocalReference<jobject>(env, value.l);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Field, getFieldModifiers, "(Ljava/lang/Class;I)I"),

  NATIVE_METHOD(Field, getBField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)B"),
  NATIVE_METHOD(Field, getCField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)C"),
  NATIVE_METHOD(Field, getDField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)D"),
  NATIVE_METHOD(Field, getFField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)F"),
  NATIVE_METHOD(Field, getField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZ)Ljava/lang/Object;"),
  NATIVE_METHOD(Field, getIField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)I"),
  NATIVE_METHOD(Field, getJField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)J"),
  NATIVE_METHOD(Field, getSField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)S"),
  NATIVE_METHOD(Field, getZField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZC)Z"),
  NATIVE_METHOD(Field, setBField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCB)V"),
  NATIVE_METHOD(Field, setCField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCC)V"),
  NATIVE_METHOD(Field, setDField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCD)V"),
  NATIVE_METHOD(Field, setFField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCF)V"),
  NATIVE_METHOD(Field, setField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZLjava/lang/Object;)V"),
  NATIVE_METHOD(Field, setIField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCI)V"),
  NATIVE_METHOD(Field, setJField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCJ)V"),
  NATIVE_METHOD(Field, setSField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCS)V"),
  NATIVE_METHOD(Field, setZField, "(Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/Class;IZCZ)V"),
};

}  // namespace

void register_java_lang_reflect_Field(JNIEnv* env) {
  InitBoxingMethods(env); // TODO: move to Runtime?
  jniRegisterNativeMethods(env, "java/lang/reflect/Field", gMethods, NELEM(gMethods));
}

}  // namespace art
