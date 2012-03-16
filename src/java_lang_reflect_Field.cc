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
#include "object_utils.h"
#include "reflection.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

static bool GetFieldValue(Object* o, Field* f, JValue& value, bool allow_references) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  switch (FieldHelper(f).GetTypeAsPrimitiveType()) {
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

static bool CheckReceiver(JNIEnv* env, jobject javaObj, Field* f, Object*& o) {
  if (f->IsStatic()) {
    o = NULL;
    return true;
  }

  o = Decode<Object*>(env, javaObj);
  Class* declaringClass = f->GetDeclaringClass();
  if (!VerifyObjectInClass(env, o, declaringClass)) {
    return false;
  }
  return true;
}

static jobject Field_get(JNIEnv* env, jobject javaField, jobject javaObj) {
  Field* f = DecodeField(env->FromReflectedField(javaField));
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, f, o)) {
    return NULL;
  }

  // Get the field's value, boxing if necessary.
  JValue value;
  if (!GetFieldValue(o, f, value, true)) {
    return NULL;
  }
  BoxPrimitive(env, FieldHelper(f).GetTypeAsPrimitiveType(), value);

  return AddLocalReference<jobject>(env, value.l);
}

static JValue GetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj, char dst_descriptor) {
  Field* f = DecodeField(env->FromReflectedField(javaField));
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, f, o)) {
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
  if (!ConvertPrimitiveValue(FieldHelper(f).GetTypeAsPrimitiveType(), dst_type->GetPrimitiveType(),
                             field_value, wide_value)) {
    return JValue();
  }
  return wide_value;
}

static jboolean Field_getBoolean(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'Z').z;
}

static jbyte Field_getByte(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'B').b;
}

static jchar Field_getChar(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'C').c;
}

static jdouble Field_getDouble(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'D').d;
}

static jfloat Field_getFloat(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'F').f;
}

static jint Field_getInt(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'I').i;
}

static jlong Field_getLong(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'J').j;
}

static jshort Field_getShort(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'S').s;
}

static void SetFieldValue(Object* o, Field* f, const JValue& new_value, bool allow_references) {
  switch (FieldHelper(f).GetTypeAsPrimitiveType()) {
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

static void Field_set(JNIEnv* env, jobject javaField, jobject javaObj, jobject javaValue) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  Field* f = DecodeField(env->FromReflectedField(javaField));

  // Unbox the value, if necessary.
  Object* boxed_value = Decode<Object*>(env, javaValue);
  JValue unboxed_value;
  if (!UnboxPrimitive(env, boxed_value, FieldHelper(f).GetType(), unboxed_value, "field")) {
    return;
  }

  // Check that the receiver is non-null and an instance of the field's declaring class.
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, f, o)) {
    return;
  }

  SetFieldValue(o, f, unboxed_value, true);
}

static void SetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj, char src_descriptor,
                              const JValue& new_value) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  Field* f = DecodeField(env->FromReflectedField(javaField));
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, f, o)) {
    return;
  }
  FieldHelper fh(f);
  if (!fh.IsPrimitiveType()) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
        "Not a primitive field: %s", PrettyField(f).c_str());
    return;
  }

  // Widen the value if necessary (and possible).
  JValue wide_value;
  Class* src_type = Runtime::Current()->GetClassLinker()->FindPrimitiveClass(src_descriptor);
  if (!ConvertPrimitiveValue(src_type->GetPrimitiveType(), fh.GetTypeAsPrimitiveType(),
                             new_value, wide_value)) {
    return;
  }

  // Write the value.
  SetFieldValue(o, f, wide_value, false);
}

static void Field_setBoolean(JNIEnv* env, jobject javaField, jobject javaObj, jboolean value) {
  JValue v = { 0 };
  v.z = value;
  SetPrimitiveField(env, javaField, javaObj, 'Z', v);
}

static void Field_setByte(JNIEnv* env, jobject javaField, jobject javaObj, jbyte value) {
  JValue v = { 0 };
  v.b = value;
  SetPrimitiveField(env, javaField, javaObj, 'B', v);
}

static void Field_setChar(JNIEnv* env, jobject javaField, jobject javaObj, jchar value) {
  JValue v = { 0 };
  v.c = value;
  SetPrimitiveField(env, javaField, javaObj, 'C', v);
}

static void Field_setDouble(JNIEnv* env, jobject javaField, jobject javaObj, jdouble value) {
  JValue v = { 0 };
  v.d = value;
  SetPrimitiveField(env, javaField, javaObj, 'D', v);
}

static void Field_setFloat(JNIEnv* env, jobject javaField, jobject javaObj, jfloat value) {
  JValue v = { 0 };
  v.f = value;
  SetPrimitiveField(env, javaField, javaObj, 'F', v);
}

static void Field_setInt(JNIEnv* env, jobject javaField, jobject javaObj, jint value) {
  JValue v = { 0 };
  v.i = value;
  SetPrimitiveField(env, javaField, javaObj, 'I', v);
}

static void Field_setLong(JNIEnv* env, jobject javaField, jobject javaObj, jlong value) {
  JValue v = { 0 };
  v.j = value;
  SetPrimitiveField(env, javaField, javaObj, 'J', v);
}

static void Field_setShort(JNIEnv* env, jobject javaField, jobject javaObj, jshort value) {
  JValue v = { 0 };
  v.s = value;
  SetPrimitiveField(env, javaField, javaObj, 'S', v);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Field, get,        "(Ljava/lang/Object;)Ljava/lang/Object;"),
  NATIVE_METHOD(Field, getBoolean, "(Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Field, getByte,    "(Ljava/lang/Object;)B"),
  NATIVE_METHOD(Field, getChar,    "(Ljava/lang/Object;)C"),
  NATIVE_METHOD(Field, getDouble,  "(Ljava/lang/Object;)D"),
  NATIVE_METHOD(Field, getFloat,   "(Ljava/lang/Object;)F"),
  NATIVE_METHOD(Field, getInt,     "(Ljava/lang/Object;)I"),
  NATIVE_METHOD(Field, getLong,    "(Ljava/lang/Object;)J"),
  NATIVE_METHOD(Field, getShort,   "(Ljava/lang/Object;)S"),
  NATIVE_METHOD(Field, set,        "(Ljava/lang/Object;Ljava/lang/Object;)V"),
  NATIVE_METHOD(Field, setBoolean, "(Ljava/lang/Object;Z)V"),
  NATIVE_METHOD(Field, setByte,    "(Ljava/lang/Object;B)V"),
  NATIVE_METHOD(Field, setChar,    "(Ljava/lang/Object;C)V"),
  NATIVE_METHOD(Field, setDouble,  "(Ljava/lang/Object;D)V"),
  NATIVE_METHOD(Field, setFloat,   "(Ljava/lang/Object;F)V"),
  NATIVE_METHOD(Field, setInt,     "(Ljava/lang/Object;I)V"),
  NATIVE_METHOD(Field, setLong,    "(Ljava/lang/Object;J)V"),
  NATIVE_METHOD(Field, setShort,   "(Ljava/lang/Object;S)V"),
};

void register_java_lang_reflect_Field(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/reflect/Field", gMethods, NELEM(gMethods));
}

}  // namespace art
