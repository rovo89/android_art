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

// TODO: we'll need this for Method too.
bool VerifyObjectInClass(JNIEnv* env, Object* o, Class* c) {
  if (o == NULL) {
    jniThrowNullPointerException(env, "receiver for non-static field access was null");
    return false;
  }
  if (!o->InstanceOf(c)) {
    std::string expectedClassName(PrettyDescriptor(c->GetDescriptor()));
    std::string actualClassName(PrettyTypeOf(o));
    jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException",
        "expected receiver of type %s, but got %s",
        expectedClassName.c_str(), actualClassName.c_str());
    return false;
  }
  return true;
}

/*
 * Convert primitive, boxed data from "srcPtr" to "dstPtr".
 *
 * Section v2 2.6 lists the various conversions and promotions.  We
 * allow the "widening" and "identity" conversions, but don't allow the
 * "narrowing" conversions.
 *
 * Allowed:
 *  byte to short, int, long, float, double
 *  short to int, long, float double
 *  char to int, long, float, double
 *  int to long, float, double
 *  long to float, double
 *  float to double
 * Values of types byte, char, and short are "internally" widened to int.
 *
 * Returns the width in 32-bit words of the destination primitive, or
 * -1 if the conversion is not allowed.
 */
bool ConvertPrimitiveValue(Class* src_class, Class* dst_class, const JValue& src, JValue& dst) {
  Class::PrimitiveType srcType = src_class->GetPrimitiveType();
  Class::PrimitiveType dstType = dst_class->GetPrimitiveType();
  switch (dstType) {
  case Class::kPrimBoolean:
  case Class::kPrimChar:
  case Class::kPrimByte:
    if (srcType == dstType) {
      dst.i = src.i;
      return true;
    }
    break;
  case Class::kPrimShort:
    if (srcType == Class::kPrimByte || srcType == Class::kPrimShort) {
      dst.i = src.i;
      return true;
    }
    break;
  case Class::kPrimInt:
    if (srcType == Class::kPrimByte || srcType == Class::kPrimChar ||
        srcType == Class::kPrimShort || srcType == Class::kPrimInt) {
      dst.i = src.i;
      return true;
    }
    break;
  case Class::kPrimLong:
    if (srcType == Class::kPrimByte || srcType == Class::kPrimChar ||
        srcType == Class::kPrimShort || srcType == Class::kPrimInt) {
      dst.j = src.i;
      return true;
    } else if (srcType == Class::kPrimLong) {
      dst.j = src.j;
      return true;
    }
    break;
  case Class::kPrimFloat:
    if (srcType == Class::kPrimByte || srcType == Class::kPrimChar ||
        srcType == Class::kPrimShort || srcType == Class::kPrimInt) {
      dst.f = src.i;
      return true;
    } else if (srcType == Class::kPrimLong) {
      dst.f = src.j;
      return true;
    } else if (srcType == Class::kPrimFloat) {
      dst.i = src.i;
      return true;
    }
    break;
  case Class::kPrimDouble:
    if (srcType == Class::kPrimByte || srcType == Class::kPrimChar ||
        srcType == Class::kPrimShort || srcType == Class::kPrimInt) {
      dst.d = src.i;
      return true;
    } else if (srcType == Class::kPrimLong) {
      dst.d = src.j;
      return true;
    } else if (srcType == Class::kPrimFloat) {
      dst.d = src.f;
      return true;
    } else if (srcType == Class::kPrimDouble) {
      dst.j = src.j;
      return true;
    }
    break;
  default:
    break;
  }
  Thread::Current()->ThrowNewException("Ljava/lang/IllegalArgumentException;",
      "invalid primitive conversion from %s to %s",
      PrettyDescriptor(src_class->GetDescriptor()).c_str(),
      PrettyDescriptor(dst_class->GetDescriptor()).c_str());
  return false;
}

bool UnboxPrimitive(JNIEnv* env, Object* o, Class* dst_class, JValue& unboxed_value) {
  if (dst_class->GetPrimitiveType() == Class::kPrimNot) {
    if (o != NULL && !o->InstanceOf(dst_class)) {
      jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException",
          "expected object of type %s, but got %s",
          PrettyDescriptor(dst_class->GetDescriptor()).c_str(),
          PrettyTypeOf(o).c_str());
      return false;
    }
    unboxed_value.l = o;
    return true;
  } else if (dst_class->GetPrimitiveType() == Class::kPrimVoid) {
    Thread::Current()->ThrowNewException("Ljava/lang/IllegalArgumentException;",
        "can't unbox to void");
    return false;
  }

  if (o == NULL) {
    Thread::Current()->ThrowNewException("Ljava/lang/IllegalArgumentException;",
        "null passed for boxed primitive type");
    return false;
  }

  JValue boxed_value = { 0 };
  const String* src_descriptor = o->GetClass()->GetDescriptor();
  Class* src_class = NULL;
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Field* primitive_field = o->GetClass()->GetIFields()->Get(0);
  if (src_descriptor->Equals("Ljava/lang/Boolean;")) {
    src_class = class_linker->FindPrimitiveClass('Z');
    boxed_value.z = primitive_field->GetBoolean(o);
  } else if (src_descriptor->Equals("Ljava/lang/Byte;")) {
    src_class = class_linker->FindPrimitiveClass('B');
    boxed_value.b = primitive_field->GetByte(o);
  } else if (src_descriptor->Equals("Ljava/lang/Character;")) {
    src_class = class_linker->FindPrimitiveClass('C');
    boxed_value.c = primitive_field->GetChar(o);
  } else if (src_descriptor->Equals("Ljava/lang/Float;")) {
    src_class = class_linker->FindPrimitiveClass('F');
    boxed_value.f = primitive_field->GetFloat(o);
  } else if (src_descriptor->Equals("Ljava/lang/Double;")) {
    src_class = class_linker->FindPrimitiveClass('D');
    boxed_value.d = primitive_field->GetDouble(o);
  } else if (src_descriptor->Equals("Ljava/lang/Integer;")) {
    src_class = class_linker->FindPrimitiveClass('I');
    boxed_value.i = primitive_field->GetInt(o);
  } else if (src_descriptor->Equals("Ljava/lang/Long;")) {
    src_class = class_linker->FindPrimitiveClass('J');
    boxed_value.j = primitive_field->GetLong(o);
  } else if (src_descriptor->Equals("Ljava/lang/Short;")) {
    src_class = class_linker->FindPrimitiveClass('S');
    boxed_value.s = primitive_field->GetShort(o);
  } else {
    Thread::Current()->ThrowNewException("Ljava/lang/IllegalArgumentException;",
        "%s is not a boxed primitive type", PrettyDescriptor(src_descriptor).c_str());
    return false;
  }

  return ConvertPrimitiveValue(src_class, dst_class, boxed_value, unboxed_value);
}

Method* gBoolean_valueOf;
Method* gByte_valueOf;
Method* gCharacter_valueOf;
Method* gDouble_valueOf;
Method* gFloat_valueOf;
Method* gInteger_valueOf;
Method* gLong_valueOf;
Method* gShort_valueOf;

void InitBoxingMethod(Method*& m, const char* class_descriptor, const char* method_signature) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* c = class_linker->FindSystemClass(class_descriptor);
  CHECK(c != NULL) << "Couldn't find boxing class: " << class_descriptor;
  m = c->FindDirectMethod("valueOf", method_signature);
  CHECK(m != NULL) << "Couldn't find boxing method: " << method_signature;
}

void BoxPrimitive(JNIEnv* env, Class* src_class, JValue& value) {
  if (!src_class->IsPrimitive()) {
    return;
  }

  Method* m = NULL;
  UniquePtr<byte[]> args(new byte[8]);
  memset(&args[0], 0, 8);
  switch (src_class->GetPrimitiveType()) {
  case Class::kPrimBoolean:
    m = gBoolean_valueOf;
    *reinterpret_cast<uint32_t*>(&args[0]) = value.z;
    break;
  case Class::kPrimByte:
    m = gByte_valueOf;
    *reinterpret_cast<uint32_t*>(&args[0]) = value.b;
    break;
  case Class::kPrimChar:
    m = gCharacter_valueOf;
    *reinterpret_cast<uint32_t*>(&args[0]) = value.c;
    break;
  case Class::kPrimDouble:
    m = gDouble_valueOf;
    *reinterpret_cast<double*>(&args[0]) = value.d;
    break;
  case Class::kPrimFloat:
    m = gFloat_valueOf;
    *reinterpret_cast<float*>(&args[0]) = value.f;
    break;
  case Class::kPrimInt:
    m = gInteger_valueOf;
    *reinterpret_cast<uint32_t*>(&args[0]) = value.i;
    break;
  case Class::kPrimLong:
    m = gLong_valueOf;
    *reinterpret_cast<uint64_t*>(&args[0]) = value.j;
    break;
  case Class::kPrimShort:
    m = gShort_valueOf;
    *reinterpret_cast<uint32_t*>(&args[0]) = value.s;
    break;
  default:
    LOG(FATAL) << PrettyClass(src_class);
  }

  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, Thread::kRunnable);
  m->Invoke(self, NULL, args.get(), &value);
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
  Field* f = reinterpret_cast<Field*>(env->FromReflectedField(javaField));

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
    return;
  case Class::kPrimByte:
    f->SetByte(o, new_value.b);
    return;
  case Class::kPrimChar:
    f->SetChar(o, new_value.c);
    return;
  case Class::kPrimDouble:
    f->SetDouble(o, new_value.d);
    return;
  case Class::kPrimFloat:
    f->SetFloat(o, new_value.f);
    return;
  case Class::kPrimInt:
    f->SetInt(o, new_value.i);
    return;
  case Class::kPrimLong:
    f->SetLong(o, new_value.j);
    return;
  case Class::kPrimShort:
    f->SetShort(o, new_value.s);
    return;
  case Class::kPrimNot:
    if (allow_references) {
      f->SetObject(o, new_value.l);
      return;
    }
    // Else break to report an error.
    break;
  case Class::kPrimVoid:
    // Never okay.
    break;
  }
  Thread::Current()->ThrowNewException("Ljava/lang/IllegalArgumentException;",
      "Not a primitive field: %s", PrettyField(f).c_str());
}

void SetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj, jclass javaDeclaringClass, jchar targetDescriptor, const JValue& new_value) {
  Field* f = reinterpret_cast<Field*>(env->FromReflectedField(javaField));

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
  Field* f = reinterpret_cast<Field*>(env->FromReflectedField(javaField));

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
  Field* f = reinterpret_cast<Field*>(env->FromReflectedField(javaField));

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
  //NATIVE_METHOD(Field, getSignatureAnnotation, "(Ljava/lang/Class;I)[Ljava/lang/Object;"),
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
  InitBoxingMethod(gBoolean_valueOf, "Ljava/lang/Boolean;", "(Z)Ljava/lang/Boolean;");
  InitBoxingMethod(gByte_valueOf, "Ljava/lang/Byte;", "(B)Ljava/lang/Byte;");
  InitBoxingMethod(gCharacter_valueOf, "Ljava/lang/Character;", "(C)Ljava/lang/Character;");
  InitBoxingMethod(gDouble_valueOf, "Ljava/lang/Double;", "(D)Ljava/lang/Double;");
  InitBoxingMethod(gFloat_valueOf, "Ljava/lang/Float;", "(F)Ljava/lang/Float;");
  InitBoxingMethod(gInteger_valueOf, "Ljava/lang/Integer;", "(I)Ljava/lang/Integer;");
  InitBoxingMethod(gLong_valueOf, "Ljava/lang/Long;", "(J)Ljava/lang/Long;");
  InitBoxingMethod(gShort_valueOf, "Ljava/lang/Short;", "(S)Ljava/lang/Short;");
  jniRegisterNativeMethods(env, "java/lang/reflect/Field", gMethods, NELEM(gMethods));
}

}  // namespace art
