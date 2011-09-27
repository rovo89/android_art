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
  return Decode<Object*>(env, jfield)->AsField()->GetAccessFlags() & kAccFieldFlagsMask;
}

bool GetFieldValue(Object* o, Field* f, JValue& value, bool allow_references) {
  switch (f->GetType()->GetPrimitiveType()) {
  case Class::kPrimBoolean:
    value.z = f->GetBoolean(o);
    return true;
  case Class::kPrimByte:
    value.b = f->GetByte(o);
    return true;
  case Class::kPrimChar:
    value.c = f->GetChar(o);
    return true;
  case Class::kPrimDouble:
    value.d = f->GetDouble(o);
    return true;
  case Class::kPrimFloat:
    value.f = f->GetFloat(o);
    return true;
  case Class::kPrimInt:
    value.i = f->GetInt(o);
    return true;
  case Class::kPrimLong:
    value.j = f->GetLong(o);
    return true;
  case Class::kPrimShort:
    value.s = f->GetShort(o);
    return true;
  case Class::kPrimNot:
    if (allow_references) {
      value.l = f->GetObject(o);
      return true;
    }
    // Else break to report an error.
    break;
  case Class::kPrimVoid:
    // Never okay.
    break;
  }
  Thread::Current()->ThrowNewException("Ljava/lang/IllegalArgumentException;",
      "Not a primitive field: %s", PrettyField(f).c_str());
  return false;
}

JValue GetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jchar targetDescriptor) {
  Field* f = DecodeField(env->FromReflectedField(javaField));

  // Check that the receiver is non-null and an instance of the field's declaring class.
  Object* o = Decode<Object*>(env, javaObj);
  bool isStatic = (javaObj == NULL);
  if (!isStatic) {
    Class* declaringClass = Decode<Class*>(env, javaDeclaringClass);
    if (!VerifyObjectInClass(env, o, declaringClass)) {
      return JValue();
    }
  }

  // Read the value.
  JValue field_value;
  if (!GetFieldValue(o, f, field_value, false)) {
    return JValue();
  }

  // Widen it if necessary (and possible).
  JValue wide_value;
  Class* targetType = Runtime::Current()->GetClassLinker()->FindPrimitiveClass(targetDescriptor);
  if (!ConvertPrimitiveValue(f->GetType(), targetType, field_value, wide_value)) {
    return JValue();
  }
  return wide_value;
}

jbyte Field_getBField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor).b;
}

jchar Field_getCField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor).c;
}

jdouble Field_getDField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor).d;
}

jfloat Field_getFField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor).f;
}

jint Field_getIField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor).i;
}

jlong Field_getJField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor).j;
}

jshort Field_getSField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor).s;
}

jboolean Field_getZField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor) {
  return GetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor).z;
}

void SetFieldValue(Object* o, Field* f, const JValue& new_value, bool allow_references) {
  switch (f->GetType()->GetPrimitiveType()) {
  case Class::kPrimBoolean:
    f->SetBoolean(o, new_value.z);
    break;
  case Class::kPrimByte:
    f->SetByte(o, new_value.b);
    break;
  case Class::kPrimChar:
    f->SetChar(o, new_value.c);
    break;
  case Class::kPrimDouble:
    f->SetDouble(o, new_value.d);
    break;
  case Class::kPrimFloat:
    f->SetFloat(o, new_value.f);
    break;
  case Class::kPrimInt:
    f->SetInt(o, new_value.i);
    break;
  case Class::kPrimLong:
    f->SetLong(o, new_value.j);
    break;
  case Class::kPrimShort:
    f->SetShort(o, new_value.s);
    break;
  case Class::kPrimNot:
    if (allow_references) {
      f->SetObject(o, new_value.l);
      break;
    }
    // Else fall through to report an error.
  case Class::kPrimVoid:
    // Never okay.
    Thread::Current()->ThrowNewException("Ljava/lang/IllegalArgumentException;",
        "Not a primitive field: %s", PrettyField(f).c_str());
    return;
  }

  // Special handling for final fields on SMP systems.
  // We need a store/store barrier here (JMM requirement).
  if (f->IsFinal()) {
    ANDROID_MEMBAR_STORE();
  }
}

void SetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jchar targetDescriptor, const JValue& new_value) {
  Field* f = DecodeField(env->FromReflectedField(javaField));

  // Check that the receiver is non-null and an instance of the field's declaring class.
  Object* o = Decode<Object*>(env, javaObj);
  bool isStatic = (javaObj == NULL);
  if (!isStatic) {
    Class* declaringClass = Decode<Class*>(env, javaDeclaringClass);
    if (!VerifyObjectInClass(env, o, declaringClass)) {
      return;
    }
  }

  // Widen the value if necessary (and possible).
  JValue wide_value;
  Class* targetType = Runtime::Current()->GetClassLinker()->FindPrimitiveClass(targetDescriptor);
  if (!ConvertPrimitiveValue(f->GetType(), targetType, new_value, wide_value)) {
    return;
  }

  // Write the value.
  SetFieldValue(o, f, wide_value, false);
}

void Field_setBField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor, jbyte value) {
  JValue v = { 0 };
  v.b = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor, v);
}

void Field_setCField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor, jchar value) {
  JValue v = { 0 };
  v.c = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor, v);
}

void Field_setDField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor, jdouble value) {
  JValue v = { 0 };
  v.d = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor, v);
}

void Field_setFField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor, jfloat value) {
  JValue v = { 0 };
  v.f = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor, v);
}

void Field_setIField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor, jint value) {
  JValue v = { 0 };
  v.i = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor, v);
}

void Field_setJField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor, jlong value) {
  JValue v = { 0 };
  v.j = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor, v);
}

void Field_setSField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor, jshort value) {
  JValue v = { 0 };
  v.s = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor, v);
}

void Field_setZField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jchar targetDescriptor, jboolean value) {
  JValue v = { 0 };
  v.z = value;
  SetPrimitiveField(env, javaField, javaObj, javaDeclaringClass, targetDescriptor, v);
}

void Field_setField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean, jobject javaValue) {
  Field* f = DecodeField(env->FromReflectedField(javaField));

  // Unbox the value, if necessary.
  Object* boxed_value = Decode<Object*>(env, javaValue);
  JValue unboxed_value;
  if (!UnboxPrimitive(env, boxed_value, f->GetType(), unboxed_value)) {
    return;
  }

  // Check that the receiver is non-null and an instance of the field's declaring class.
  Object* o = Decode<Object*>(env, javaObj);
  bool isStatic = (javaObj == NULL);
  if (!isStatic) {
    Class* declaringClass = Decode<Class*>(env, javaDeclaringClass);
    if (!VerifyObjectInClass(env, o, declaringClass)) {
      return;
    }
  }

  SetFieldValue(o, f, unboxed_value, true);
}

jobject Field_getField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jclass, jint, jboolean) {
  Field* f = DecodeField(env->FromReflectedField(javaField));

  // Check that the receiver is non-null and an instance of the field's declaring class.
  Object* o = Decode<Object*>(env, javaObj);
  bool isStatic = (javaObj == NULL);
  if (!isStatic) {
    Class* declaringClass = Decode<Class*>(env, javaDeclaringClass);
    if (!VerifyObjectInClass(env, o, declaringClass)) {
      return NULL;
    }
  }

  // Get the field's value, boxing if necessary.
  JValue value;
  if (!GetFieldValue(o, f, value, true)) {
    return NULL;
  }
  BoxPrimitive(env, f->GetType(), value);

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
