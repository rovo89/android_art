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
#include "object_utils.h"

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

void InitBoxingMethods() {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  gBoolean_valueOf = class_linker->FindSystemClass("Ljava/lang/Boolean;")->FindDeclaredDirectMethod("valueOf", "(Z)Ljava/lang/Boolean;");
  gByte_valueOf = class_linker->FindSystemClass("Ljava/lang/Byte;")->FindDeclaredDirectMethod("valueOf", "(B)Ljava/lang/Byte;");
  gCharacter_valueOf = class_linker->FindSystemClass("Ljava/lang/Character;")->FindDeclaredDirectMethod("valueOf", "(C)Ljava/lang/Character;");
  gDouble_valueOf = class_linker->FindSystemClass("Ljava/lang/Double;")->FindDeclaredDirectMethod("valueOf", "(D)Ljava/lang/Double;");
  gFloat_valueOf = class_linker->FindSystemClass("Ljava/lang/Float;")->FindDeclaredDirectMethod("valueOf", "(F)Ljava/lang/Float;");
  gInteger_valueOf = class_linker->FindSystemClass("Ljava/lang/Integer;")->FindDeclaredDirectMethod("valueOf", "(I)Ljava/lang/Integer;");
  gLong_valueOf = class_linker->FindSystemClass("Ljava/lang/Long;")->FindDeclaredDirectMethod("valueOf", "(J)Ljava/lang/Long;");
  gShort_valueOf = class_linker->FindSystemClass("Ljava/lang/Short;")->FindDeclaredDirectMethod("valueOf", "(S)Ljava/lang/Short;");
}

jobject InvokeMethod(JNIEnv* env, jobject javaMethod, jobject javaReceiver, jobject javaArgs) {
  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, kRunnable);

  jmethodID mid = env->FromReflectedMethod(javaMethod);
  Method* m = reinterpret_cast<Method*>(mid);

  Class* declaring_class = m->GetDeclaringClass();
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(declaring_class, true, true)) {
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
    mid = reinterpret_cast<jmethodID>(m);
  }

  // Get our arrays of arguments and their types, and check they're the same size.
  ObjectArray<Object>* objects = Decode<ObjectArray<Object>*>(env, javaArgs);
  MethodHelper mh(m);
  const DexFile::TypeList* classes = mh.GetParameterTypeList();
  uint32_t classes_size = classes == NULL ? 0 : classes->Size();
  uint32_t arg_count = (objects != NULL) ? objects->GetLength() : 0;
  if (arg_count != classes_size) {
    self->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
        "wrong number of arguments; expected %d, got %d",
        classes_size, arg_count);
    return NULL;
  }

  // Translate javaArgs to a jvalue[].
  UniquePtr<jvalue[]> args(new jvalue[arg_count]);
  JValue* decoded_args = reinterpret_cast<JValue*>(args.get());
  for (uint32_t i = 0; i < arg_count; ++i) {
    Object* arg = objects->Get(i);
    Class* dst_class = mh.GetClassFromTypeIdx(classes->GetTypeItem(i).type_idx_);
    if (dst_class->IsPrimitive() || (arg != NULL && !arg->InstanceOf(dst_class))) {
      // We want to actually unbox primitives, but only reuse the error reporting for reference types.
      if (!UnboxPrimitiveForArgument(arg, dst_class, decoded_args[i], m, i)) {
        return NULL;
      }
    } else {
      // We already tested that these types are compatible.
      args[i].l = AddLocalReference<jobject>(env, arg);
    }
  }

  // Invoke the method.
  JValue value(InvokeWithJValues(env, javaReceiver, mid, args.get()));

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
  BoxPrimitive(mh.GetReturnType()->GetPrimitiveType(), value);
  return AddLocalReference<jobject>(env, value.GetL());
}

bool VerifyObjectInClass(JNIEnv* env, Object* o, Class* c) {
  const char* exception = NULL;
  if (o == NULL) {
    exception = "java/lang/NullPointerException";
  } else if (!o->InstanceOf(c)) {
    exception = "java/lang/IllegalArgumentException";
  }
  if (exception != NULL) {
    std::string expected_class_name(PrettyDescriptor(c));
    std::string actual_class_name(PrettyTypeOf(o));
    jniThrowExceptionFmt(env, exception, "expected receiver of type %s, but got %s",
                         expected_class_name.c_str(), actual_class_name.c_str());
    return false;
  }
  return true;
}

bool ConvertPrimitiveValue(Primitive::Type srcType, Primitive::Type dstType,
                           const JValue& src, JValue& dst) {
  CHECK(srcType != Primitive::kPrimNot && dstType != Primitive::kPrimNot);
  switch (dstType) {
  case Primitive::kPrimBoolean:
    if (srcType == Primitive::kPrimBoolean) {
      dst.SetZ(src.GetZ());
      return true;
    }
    break;
  case Primitive::kPrimChar:
    if (srcType == Primitive::kPrimChar) {
      dst.SetC(src.GetC());
      return true;
    }
    break;
  case Primitive::kPrimByte:
    if (srcType == Primitive::kPrimByte) {
      dst.SetB(src.GetB());
      return true;
    }
    break;
  case Primitive::kPrimShort:
    if (srcType == Primitive::kPrimByte || srcType == Primitive::kPrimShort) {
      dst.SetS(src.GetI());
      return true;
    }
    break;
  case Primitive::kPrimInt:
    if (srcType == Primitive::kPrimByte || srcType == Primitive::kPrimChar ||
        srcType == Primitive::kPrimShort || srcType == Primitive::kPrimInt) {
      dst.SetI(src.GetI());
      return true;
    }
    break;
  case Primitive::kPrimLong:
    if (srcType == Primitive::kPrimByte || srcType == Primitive::kPrimChar ||
        srcType == Primitive::kPrimShort || srcType == Primitive::kPrimInt) {
      dst.SetJ(src.GetI());
      return true;
    } else if (srcType == Primitive::kPrimLong) {
      dst.SetJ(src.GetJ());
      return true;
    }
    break;
  case Primitive::kPrimFloat:
    if (srcType == Primitive::kPrimByte || srcType == Primitive::kPrimChar ||
        srcType == Primitive::kPrimShort || srcType == Primitive::kPrimInt) {
      dst.SetF(src.GetI());
      return true;
    } else if (srcType == Primitive::kPrimLong) {
      dst.SetF(src.GetJ());
      return true;
    } else if (srcType == Primitive::kPrimFloat) {
      dst.SetF(src.GetF());
      return true;
    }
    break;
  case Primitive::kPrimDouble:
    if (srcType == Primitive::kPrimByte || srcType == Primitive::kPrimChar ||
        srcType == Primitive::kPrimShort || srcType == Primitive::kPrimInt) {
      dst.SetD(src.GetI());
      return true;
    } else if (srcType == Primitive::kPrimLong) {
      dst.SetD(src.GetJ());
      return true;
    } else if (srcType == Primitive::kPrimFloat) {
      dst.SetD(src.GetF());
      return true;
    } else if (srcType == Primitive::kPrimDouble) {
      dst.SetJ(src.GetJ());
      return true;
    }
    break;
  default:
    break;
  }
  Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
                                        "invalid primitive conversion from %s to %s",
                                        PrettyDescriptor(srcType).c_str(),
                                        PrettyDescriptor(dstType).c_str());
  return false;
}

void BoxPrimitive(Primitive::Type src_class, JValue& value) {
  if (src_class == Primitive::kPrimNot) {
    return;
  }

  Method* m = NULL;
  switch (src_class) {
  case Primitive::kPrimBoolean:
    m = gBoolean_valueOf;
    break;
  case Primitive::kPrimByte:
    m = gByte_valueOf;
    break;
  case Primitive::kPrimChar:
    m = gCharacter_valueOf;
    break;
  case Primitive::kPrimDouble:
    m = gDouble_valueOf;
    break;
  case Primitive::kPrimFloat:
    m = gFloat_valueOf;
    break;
  case Primitive::kPrimInt:
    m = gInteger_valueOf;
    break;
  case Primitive::kPrimLong:
    m = gLong_valueOf;
    break;
  case Primitive::kPrimShort:
    m = gShort_valueOf;
    break;
  case Primitive::kPrimVoid:
    // There's no such thing as a void field, and void methods invoked via reflection return null.
    value.SetL(NULL);
    return;
  default:
    LOG(FATAL) << static_cast<int>(src_class);
  }

  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, kRunnable);
  JValue args[1] = { value };
  m->Invoke(self, NULL, args, &value);
}

static std::string UnboxingFailureKind(Method* m, int index, Field* f) {
  if (m != NULL && index != -1) {
    ++index; // Humans count from 1.
    return StringPrintf("method %s argument %d", PrettyMethod(m, false).c_str(), index);
  }
  if (f != NULL) {
    return "field " + PrettyField(f, false);
  }
  return "result";
}

static bool UnboxPrimitive(Object* o, Class* dst_class, JValue& unboxed_value, Method* m, int index, Field* f) {
  if (!dst_class->IsPrimitive()) {
    if (o != NULL && !o->InstanceOf(dst_class)) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
                                            "%s has type %s, got %s",
                                            UnboxingFailureKind(m, index, f).c_str(),
                                            PrettyDescriptor(dst_class).c_str(),
                                            PrettyTypeOf(o).c_str());
      return false;
    }
    unboxed_value.SetL(o);
    return true;
  } else if (dst_class->GetPrimitiveType() == Primitive::kPrimVoid) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
                                          "can't unbox %s to void",
                                          UnboxingFailureKind(m, index, f).c_str());
    return false;
  }

  if (o == NULL) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
                                          "%s has type %s, got null",
                                          UnboxingFailureKind(m, index, f).c_str(),
                                          PrettyDescriptor(dst_class).c_str());
    return false;
  }

  JValue boxed_value;
  std::string src_descriptor(ClassHelper(o->GetClass()).GetDescriptor());
  Class* src_class = NULL;
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Field* primitive_field = o->GetClass()->GetIFields()->Get(0);
  if (src_descriptor == "Ljava/lang/Boolean;") {
    src_class = class_linker->FindPrimitiveClass('Z');
    boxed_value.SetZ(primitive_field->GetBoolean(o));
  } else if (src_descriptor == "Ljava/lang/Byte;") {
    src_class = class_linker->FindPrimitiveClass('B');
    boxed_value.SetB(primitive_field->GetByte(o));
  } else if (src_descriptor == "Ljava/lang/Character;") {
    src_class = class_linker->FindPrimitiveClass('C');
    boxed_value.SetC(primitive_field->GetChar(o));
  } else if (src_descriptor == "Ljava/lang/Float;") {
    src_class = class_linker->FindPrimitiveClass('F');
    boxed_value.SetF(primitive_field->GetFloat(o));
  } else if (src_descriptor == "Ljava/lang/Double;") {
    src_class = class_linker->FindPrimitiveClass('D');
    boxed_value.SetD(primitive_field->GetDouble(o));
  } else if (src_descriptor == "Ljava/lang/Integer;") {
    src_class = class_linker->FindPrimitiveClass('I');
    boxed_value.SetI(primitive_field->GetInt(o));
  } else if (src_descriptor == "Ljava/lang/Long;") {
    src_class = class_linker->FindPrimitiveClass('J');
    boxed_value.SetJ(primitive_field->GetLong(o));
  } else if (src_descriptor == "Ljava/lang/Short;") {
    src_class = class_linker->FindPrimitiveClass('S');
    boxed_value.SetS(primitive_field->GetShort(o));
  } else {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
                                          "%s has type %s, got %s",
                                          UnboxingFailureKind(m, index, f).c_str(),
                                          PrettyDescriptor(dst_class).c_str(),
                                          PrettyDescriptor(src_descriptor.c_str()).c_str());
    return false;
  }

  return ConvertPrimitiveValue(src_class->GetPrimitiveType(), dst_class->GetPrimitiveType(),
                               boxed_value, unboxed_value);
}

bool UnboxPrimitiveForArgument(Object* o, Class* dst_class, JValue& unboxed_value, Method* m, size_t index) {
  CHECK(m != NULL);
  return UnboxPrimitive(o, dst_class, unboxed_value, m, index, NULL);
}

bool UnboxPrimitiveForField(Object* o, Class* dst_class, JValue& unboxed_value, Field* f) {
  CHECK(f != NULL);
  return UnboxPrimitive(o, dst_class, unboxed_value, NULL, -1, f);
}

bool UnboxPrimitiveForResult(Object* o, Class* dst_class, JValue& unboxed_value) {
  return UnboxPrimitive(o, dst_class, unboxed_value, NULL, -1, NULL);
}

}  // namespace art
