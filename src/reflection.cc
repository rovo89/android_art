/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "reflection.h"

#include "class_linker.h"
#include "jni_internal.h"
#include "object.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

Method* gBoolean_valueOf;
Method* gByte_valueOf;
Method* gCharacter_valueOf;
Method* gDouble_valueOf;
Method* gFloat_valueOf;
Method* gInteger_valueOf;
Method* gLong_valueOf;
Method* gShort_valueOf;

void InitBoxingMethod(JNIEnv* env, Method*& m, jclass c, const char* method_signature) {
  m = DecodeMethod(env->GetStaticMethodID(c, "valueOf", method_signature));
}

void InitBoxingMethods(JNIEnv* env) {
  InitBoxingMethod(env, gBoolean_valueOf, JniConstants::booleanClass, "(Z)Ljava/lang/Boolean;");
  InitBoxingMethod(env, gByte_valueOf, JniConstants::byteClass, "(B)Ljava/lang/Byte;");
  InitBoxingMethod(env, gCharacter_valueOf, JniConstants::characterClass, "(C)Ljava/lang/Character;");
  InitBoxingMethod(env, gDouble_valueOf, JniConstants::doubleClass, "(D)Ljava/lang/Double;");
  InitBoxingMethod(env, gFloat_valueOf, JniConstants::floatClass, "(F)Ljava/lang/Float;");
  InitBoxingMethod(env, gInteger_valueOf, JniConstants::integerClass, "(I)Ljava/lang/Integer;");
  InitBoxingMethod(env, gLong_valueOf, JniConstants::longClass, "(J)Ljava/lang/Long;");
  InitBoxingMethod(env, gShort_valueOf, JniConstants::shortClass, "(S)Ljava/lang/Short;");
}

jobject InvokeMethod(JNIEnv* env, jobject javaMethod, jobject javaReceiver, jobject javaArgs, jobject javaParams) {
  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, Thread::kRunnable);

  jmethodID mid = env->FromReflectedMethod(javaMethod);
  Method* m = reinterpret_cast<Method*>(mid);

  Class* declaring_class = m->GetDeclaringClass();
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(declaring_class, true)) {
    return NULL;
  }

  Object* receiver = NULL;
  if (!m->IsStatic()) {
    // Check that the receiver is non-null and an instance of the field's declaring class.
    receiver = Decode<Object*>(env, javaReceiver);
    if (!VerifyObjectInClass(env, receiver, declaring_class)) {
      return NULL;
    }

    // Find the actual implementation of the virtual method.
    m = receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(m);
  }

  // Get our arrays of arguments and their types, and check they're the same size.
  ObjectArray<Object>* objects = Decode<ObjectArray<Object>*>(env, javaArgs);
  ObjectArray<Class>* classes = Decode<ObjectArray<Class>*>(env, javaParams);
  int32_t arg_count = (objects != NULL) ? objects->GetLength() : 0;
  if (arg_count != classes->GetLength()) {
    self->ThrowNewException("Ljava/lang/IllegalArgumentException;",
        "wrong number of arguments; expected %d, got %d",
        classes->GetLength(), arg_count);
    return NULL;
  }

  // Translate javaArgs to a jvalue[].
  UniquePtr<jvalue[]> args(new jvalue[arg_count]);
  JValue* decoded_args = reinterpret_cast<JValue*>(args.get());
  for (int32_t i = 0; i < arg_count; ++i) {
    Object* arg = objects->Get(i);
    Class* dst_class = classes->Get(i);
    if (dst_class->IsPrimitive()) {
      if (!UnboxPrimitive(env, arg, dst_class, decoded_args[i])) {
        return NULL;
      }
    } else {
      args[i].l = AddLocalReference<jobject>(env, arg);
    }
  }

  // Invoke the method.
  JValue value = InvokeWithJValues(env, javaReceiver, mid, args.get());

  // Wrap any exception with "Ljava/lang/reflect/InvocationTargetException;" and return early.
  if (self->IsExceptionPending()) {
    jthrowable th = env->ExceptionOccurred();
    env->ExceptionClear();
    jclass exception_class = env->FindClass("java/lang/reflect/InvocationTargetException");
    jmethodID mid = env->GetMethodID(exception_class, "<init>", "(Ljava/lang/Throwable;)V");
    jobject exception_instance = env->NewObject(exception_class, mid, th);
    env->Throw(reinterpret_cast<jthrowable>(exception_instance));
    return NULL;
  }

  // Box if necessary and return.
  BoxPrimitive(env, m->GetReturnType(), value);
  return AddLocalReference<jobject>(env, value.l);
}

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
  case Class::kPrimVoid:
    // There's no such thing as a void field, and void methods invoked via reflection return null.
    value.l = NULL;
    return;
  default:
    LOG(FATAL) << PrettyClass(src_class);
  }

  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, Thread::kRunnable);
  m->Invoke(self, NULL, args.get(), &value);
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

}  // namespace art
