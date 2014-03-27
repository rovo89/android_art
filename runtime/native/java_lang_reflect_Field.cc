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

#include "class_linker.h"
#include "class_linker-inl.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "jni_internal.h"
#include "mirror/art_field-inl.h"
#include "mirror/class-inl.h"
#include "object_utils.h"
#include "reflection.h"
#include "scoped_fast_native_object_access.h"

namespace art {

static bool GetFieldValue(const ScopedFastNativeObjectAccess& soa, mirror::Object* o,
                          mirror::ArtField* f, Primitive::Type field_type, bool allow_references,
                          JValue* value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK_EQ(value->GetJ(), INT64_C(0));
  DCHECK(f->GetDeclaringClass()->IsInitialized());
  switch (field_type) {
    case Primitive::kPrimBoolean:
      value->SetZ(f->GetBoolean(o));
      return true;
    case Primitive::kPrimByte:
      value->SetB(f->GetByte(o));
      return true;
    case Primitive::kPrimChar:
      value->SetC(f->GetChar(o));
      return true;
    case Primitive::kPrimDouble:
      value->SetD(f->GetDouble(o));
      return true;
    case Primitive::kPrimFloat:
      value->SetF(f->GetFloat(o));
      return true;
    case Primitive::kPrimInt:
      value->SetI(f->GetInt(o));
      return true;
    case Primitive::kPrimLong:
      value->SetJ(f->GetLong(o));
      return true;
    case Primitive::kPrimShort:
      value->SetS(f->GetShort(o));
      return true;
    case Primitive::kPrimNot:
      if (allow_references) {
        value->SetL(f->GetObject(o));
        return true;
      }
      // Else break to report an error.
      break;
    case Primitive::kPrimVoid:
      // Never okay.
      break;
  }
  ThrowIllegalArgumentException(nullptr, StringPrintf("Not a primitive field: %s",
                                                      PrettyField(f).c_str()).c_str());
  return false;
}

static bool CheckReceiver(const ScopedFastNativeObjectAccess& soa, jobject j_rcvr,
                          mirror::ArtField* f, mirror::Object** class_or_rcvr)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  soa.Self()->AssertThreadSuspensionIsAllowable();
  if (f->IsStatic()) {
    SirtRef<mirror::Class> sirt_klass(soa.Self(), f->GetDeclaringClass());
    if (UNLIKELY(!Runtime::Current()->GetClassLinker()->EnsureInitialized(sirt_klass, true, true))) {
      DCHECK(soa.Self()->IsExceptionPending());
      *class_or_rcvr = nullptr;
      return false;
    }
    *class_or_rcvr = sirt_klass.get();
    return true;
  }

  *class_or_rcvr = soa.Decode<mirror::Object*>(j_rcvr);
  mirror::Class* declaringClass = f->GetDeclaringClass();
  if (!VerifyObjectIsClass(*class_or_rcvr, declaringClass)) {
    DCHECK(soa.Self()->IsExceptionPending());
    *class_or_rcvr = nullptr;
    return false;
  }
  return true;
}

static jobject Field_get(JNIEnv* env, jobject javaField, jobject javaObj) {
  ScopedFastNativeObjectAccess soa(env);
  CHECK(!kMovingFields) << "CheckReceiver may trigger thread suspension for initialization";
  mirror::ArtField* f = mirror::ArtField::FromReflectedField(soa, javaField);
  mirror::Object* o = nullptr;
  if (!CheckReceiver(soa, javaObj, f, &o)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return nullptr;
  }
  // We now don't expect suspension unless an exception is thrown.
  // Get the field's value, boxing if necessary.
  Primitive::Type field_type = FieldHelper(f).GetTypeAsPrimitiveType();
  JValue value;
  if (!GetFieldValue(soa, o, f, field_type, true, &value)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(BoxPrimitive(field_type, value));
}

static JValue GetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj,
                                char dst_descriptor) {
  ScopedFastNativeObjectAccess soa(env);
  CHECK(!kMovingFields) << "CheckReceiver may trigger thread suspension for initialization";
  mirror::ArtField* f = mirror::ArtField::FromReflectedField(soa, javaField);
  mirror::Object* o = nullptr;
  if (!CheckReceiver(soa, javaObj, f, &o)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return JValue();
  }
  // We now don't expect suspension unless an exception is thrown.
  // Read the value.
  Primitive::Type field_type = FieldHelper(f).GetTypeAsPrimitiveType();
  JValue field_value;
  if (!GetFieldValue(soa, o, f, field_type, false, &field_value)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return JValue();
  }

  // Widen it if necessary (and possible).
  JValue wide_value;
  if (!ConvertPrimitiveValue(NULL, false, field_type, Primitive::GetType(dst_descriptor),
                             field_value, &wide_value)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return JValue();
  }
  return wide_value;
}

static jboolean Field_getBoolean(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'Z').GetZ();
}

static jbyte Field_getByte(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'B').GetB();
}

static jchar Field_getChar(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'C').GetC();
}

static jdouble Field_getDouble(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'D').GetD();
}

static jfloat Field_getFloat(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'F').GetF();
}

static jint Field_getInt(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'I').GetI();
}

static jlong Field_getLong(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'J').GetJ();
}

static jshort Field_getShort(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'S').GetS();
}

static void SetFieldValue(ScopedFastNativeObjectAccess& soa, mirror::Object* o,
                          mirror::ArtField* f, Primitive::Type field_type, bool allow_references,
                          const JValue& new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(f->GetDeclaringClass()->IsInitialized());
  switch (field_type) {
  case Primitive::kPrimBoolean:
    f->SetBoolean<false>(o, new_value.GetZ());
    break;
  case Primitive::kPrimByte:
    f->SetByte<false>(o, new_value.GetB());
    break;
  case Primitive::kPrimChar:
    f->SetChar<false>(o, new_value.GetC());
    break;
  case Primitive::kPrimDouble:
    f->SetDouble<false>(o, new_value.GetD());
    break;
  case Primitive::kPrimFloat:
    f->SetFloat<false>(o, new_value.GetF());
    break;
  case Primitive::kPrimInt:
    f->SetInt<false>(o, new_value.GetI());
    break;
  case Primitive::kPrimLong:
    f->SetLong<false>(o, new_value.GetJ());
    break;
  case Primitive::kPrimShort:
    f->SetShort<false>(o, new_value.GetS());
    break;
  case Primitive::kPrimNot:
    if (allow_references) {
      f->SetObject<false>(o, new_value.GetL());
      break;
    }
    // Else fall through to report an error.
  case Primitive::kPrimVoid:
    // Never okay.
    ThrowIllegalArgumentException(nullptr, StringPrintf("Not a primitive field: %s",
                                                        PrettyField(f).c_str()).c_str());
    return;
  }
}

static void Field_set(JNIEnv* env, jobject javaField, jobject javaObj, jobject javaValue) {
  ScopedFastNativeObjectAccess soa(env);
  CHECK(!kMovingFields) << "CheckReceiver may trigger thread suspension for initialization";
  mirror::ArtField* f = mirror::ArtField::FromReflectedField(soa, javaField);
  // Check that the receiver is non-null and an instance of the field's declaring class.
  mirror::Object* o = nullptr;
  if (!CheckReceiver(soa, javaObj, f, &o)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return;
  }
  Primitive::Type field_prim_type;
  mirror::Class* field_type;
  {
    FieldHelper fh(f);
    const char* field_type_desciptor = fh.GetTypeDescriptor();
    field_prim_type = Primitive::GetType(field_type_desciptor[0]);
    if (field_prim_type == Primitive::kPrimNot) {
      SirtRef<mirror::Object> sirt_obj(soa.Self(), o);
      // May cause resolution.
      CHECK(!kMovingFields) << "Resolution may trigger thread suspension";
      field_type = fh.GetType(true);
      if (field_type == nullptr) {
        DCHECK(soa.Self()->IsExceptionPending());
        return;
      }
    } else {
      field_type = Runtime::Current()->GetClassLinker()->FindPrimitiveClass(field_type_desciptor[0]);
    }
  }
  // We now don't expect suspension unless an exception is thrown.
  // Unbox the value, if necessary.
  mirror::Object* boxed_value = soa.Decode<mirror::Object*>(javaValue);
  JValue unboxed_value;
  if (!UnboxPrimitiveForField(boxed_value, field_type, f, &unboxed_value)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return;
  }
  SetFieldValue(soa, o, f, field_prim_type, true, unboxed_value);
}

static void SetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj, char src_descriptor,
                              const JValue& new_value) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::ArtField* f = mirror::ArtField::FromReflectedField(soa, javaField);
  mirror::Object* o = nullptr;
  if (!CheckReceiver(soa, javaObj, f, &o)) {
    return;
  }
  Primitive::Type field_type = FieldHelper(f).GetTypeAsPrimitiveType();
  if (UNLIKELY(field_type == Primitive::kPrimNot)) {
    ThrowIllegalArgumentException(nullptr, StringPrintf("Not a primitive field: %s",
                                                        PrettyField(f).c_str()).c_str());
    return;
  }

  // Widen the value if necessary (and possible).
  JValue wide_value;
  if (!ConvertPrimitiveValue(nullptr, false, Primitive::GetType(src_descriptor),
                             field_type, new_value, &wide_value)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return;
  }

  // Write the value.
  SetFieldValue(soa, o, f, field_type, false, wide_value);
}

static void Field_setBoolean(JNIEnv* env, jobject javaField, jobject javaObj, jboolean z) {
  JValue value;
  value.SetZ(z);
  SetPrimitiveField(env, javaField, javaObj, 'Z', value);
}

static void Field_setByte(JNIEnv* env, jobject javaField, jobject javaObj, jbyte b) {
  JValue value;
  value.SetB(b);
  SetPrimitiveField(env, javaField, javaObj, 'B', value);
}

static void Field_setChar(JNIEnv* env, jobject javaField, jobject javaObj, jchar c) {
  JValue value;
  value.SetC(c);
  SetPrimitiveField(env, javaField, javaObj, 'C', value);
}

static void Field_setDouble(JNIEnv* env, jobject javaField, jobject javaObj, jdouble d) {
  JValue value;
  value.SetD(d);
  SetPrimitiveField(env, javaField, javaObj, 'D', value);
}

static void Field_setFloat(JNIEnv* env, jobject javaField, jobject javaObj, jfloat f) {
  JValue value;
  value.SetF(f);
  SetPrimitiveField(env, javaField, javaObj, 'F', value);
}

static void Field_setInt(JNIEnv* env, jobject javaField, jobject javaObj, jint i) {
  JValue value;
  value.SetI(i);
  SetPrimitiveField(env, javaField, javaObj, 'I', value);
}

static void Field_setLong(JNIEnv* env, jobject javaField, jobject javaObj, jlong j) {
  JValue value;
  value.SetJ(j);
  SetPrimitiveField(env, javaField, javaObj, 'J', value);
}

static void Field_setShort(JNIEnv* env, jobject javaField, jobject javaObj, jshort s) {
  JValue value;
  value.SetS(s);
  SetPrimitiveField(env, javaField, javaObj, 'S', value);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Field, get,        "!(Ljava/lang/Object;)Ljava/lang/Object;"),
  NATIVE_METHOD(Field, getBoolean, "!(Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Field, getByte,    "!(Ljava/lang/Object;)B"),
  NATIVE_METHOD(Field, getChar,    "!(Ljava/lang/Object;)C"),
  NATIVE_METHOD(Field, getDouble,  "!(Ljava/lang/Object;)D"),
  NATIVE_METHOD(Field, getFloat,   "!(Ljava/lang/Object;)F"),
  NATIVE_METHOD(Field, getInt,     "!(Ljava/lang/Object;)I"),
  NATIVE_METHOD(Field, getLong,    "!(Ljava/lang/Object;)J"),
  NATIVE_METHOD(Field, getShort,   "!(Ljava/lang/Object;)S"),
  NATIVE_METHOD(Field, set,        "!(Ljava/lang/Object;Ljava/lang/Object;)V"),
  NATIVE_METHOD(Field, setBoolean, "!(Ljava/lang/Object;Z)V"),
  NATIVE_METHOD(Field, setByte,    "!(Ljava/lang/Object;B)V"),
  NATIVE_METHOD(Field, setChar,    "!(Ljava/lang/Object;C)V"),
  NATIVE_METHOD(Field, setDouble,  "!(Ljava/lang/Object;D)V"),
  NATIVE_METHOD(Field, setFloat,   "!(Ljava/lang/Object;F)V"),
  NATIVE_METHOD(Field, setInt,     "!(Ljava/lang/Object;I)V"),
  NATIVE_METHOD(Field, setLong,    "!(Ljava/lang/Object;J)V"),
  NATIVE_METHOD(Field, setShort,   "!(Ljava/lang/Object;S)V"),
};

void register_java_lang_reflect_Field(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/reflect/Field");
}

}  // namespace art
