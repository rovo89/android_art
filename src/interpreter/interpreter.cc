/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "interpreter.h"

#include <math.h>

#include "base/logging.h"
#include "class_linker-inl.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex_instruction.h"
#include "gc/card_table-inl.h"
#include "invoke_arg_array_builder.h"
#include "nth_caller_visitor.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/field-inl.h"
#include "mirror/abstract_method.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "object_utils.h"
#include "runtime_support.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

using namespace art::mirror;

namespace art {
namespace interpreter {

static const int32_t kMaxInt = std::numeric_limits<int32_t>::max();
static const int32_t kMinInt = std::numeric_limits<int32_t>::min();
static const int64_t kMaxLong = std::numeric_limits<int64_t>::max();
static const int64_t kMinLong = std::numeric_limits<int64_t>::min();

static JDWP::FrameId throw_frame_id_ = 0;
static AbstractMethod* throw_method_ = NULL;
static uint32_t throw_dex_pc_ = 0;

static void UnstartedRuntimeInvoke(Thread* self, AbstractMethod* target_method,
                                   Object* receiver, uint32_t* args, JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // In a runtime that's not started we intercept certain methods to avoid complicated dependency
  // problems in core libraries.
  std::string name(PrettyMethod(target_method));
  if (name == "java.lang.Class java.lang.Class.forName(java.lang.String)") {
    std::string descriptor(DotToDescriptor(reinterpret_cast<Object*>(args[0])->AsString()->ToModifiedUtf8().c_str()));
    ClassLoader* class_loader = NULL; // shadow_frame.GetMethod()->GetDeclaringClass()->GetClassLoader();
    Class* found = Runtime::Current()->GetClassLinker()->FindClass(descriptor.c_str(),
                                                                   class_loader);
    CHECK(found != NULL) << "Class.forName failed in un-started runtime for class: "
        << PrettyDescriptor(descriptor);
    result->SetL(found);
  } else if (name == "java.lang.Object java.lang.Class.newInstance()") {
    Class* klass = receiver->AsClass();
    AbstractMethod* c = klass->FindDeclaredDirectMethod("<init>", "()V");
    CHECK(c != NULL);
    Object* obj = klass->AllocObject(self);
    CHECK(obj != NULL);
    EnterInterpreterFromInvoke(self, c, obj, NULL, NULL, NULL);
    result->SetL(obj);
  } else if (name == "java.lang.reflect.Field java.lang.Class.getDeclaredField(java.lang.String)") {
    // Special managed code cut-out to allow field lookup in a un-started runtime that'd fail
    // going the reflective Dex way.
    Class* klass = receiver->AsClass();
    String* name = reinterpret_cast<Object*>(args[0])->AsString();
    Field* found = NULL;
    FieldHelper fh;
    ObjectArray<Field>* fields = klass->GetIFields();
    for (int32_t i = 0; i < fields->GetLength() && found == NULL; ++i) {
      Field* f = fields->Get(i);
      fh.ChangeField(f);
      if (name->Equals(fh.GetName())) {
        found = f;
      }
    }
    if (found == NULL) {
      fields = klass->GetSFields();
      for (int32_t i = 0; i < fields->GetLength() && found == NULL; ++i) {
        Field* f = fields->Get(i);
        fh.ChangeField(f);
        if (name->Equals(fh.GetName())) {
          found = f;
        }
      }
    }
    CHECK(found != NULL)
      << "Failed to find field in Class.getDeclaredField in un-started runtime. name="
      << name->ToModifiedUtf8() << " class=" << PrettyDescriptor(klass);
    // TODO: getDeclaredField calls GetType once the field is found to ensure a
    //       NoClassDefFoundError is thrown if the field's type cannot be resolved.
    result->SetL(found);
  } else if (name == "void java.lang.System.arraycopy(java.lang.Object, int, java.lang.Object, int, int)") {
    // Special case array copying without initializing System.
    Class* ctype = reinterpret_cast<Object*>(args[0])->GetClass()->GetComponentType();
    jint srcPos = args[1];
    jint dstPos = args[3];
    jint length = args[4];
    if (!ctype->IsPrimitive()) {
      ObjectArray<Object>* src = reinterpret_cast<Object*>(args[0])->AsObjectArray<Object>();
      ObjectArray<Object>* dst = reinterpret_cast<Object*>(args[2])->AsObjectArray<Object>();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else if (ctype->IsPrimitiveChar()) {
      CharArray* src = reinterpret_cast<Object*>(args[0])->AsCharArray();
      CharArray* dst = reinterpret_cast<Object*>(args[2])->AsCharArray();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else if (ctype->IsPrimitiveInt()) {
      IntArray* src = reinterpret_cast<Object*>(args[0])->AsIntArray();
      IntArray* dst = reinterpret_cast<Object*>(args[2])->AsIntArray();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else {
      UNIMPLEMENTED(FATAL) << "System.arraycopy of unexpected type: " << PrettyDescriptor(ctype);
    }
  } else {
    // Not special, continue with regular interpreter execution.
    EnterInterpreterFromInvoke(self, target_method, receiver, args, result, result);
  }
}

// Hand select a number of methods to be run in a not yet started runtime without using JNI.
static void UnstartedRuntimeJni(Thread* self, AbstractMethod* method,
                                Object* receiver, uint32_t* args, JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::string name(PrettyMethod(method));
  if (name == "java.lang.ClassLoader dalvik.system.VMStack.getCallingClassLoader()") {
    result->SetL(NULL);
  } else if (name == "java.lang.Class dalvik.system.VMStack.getStackClass2()") {
    NthCallerVisitor visitor(self, 3);
    visitor.WalkStack();
    result->SetL(visitor.caller->GetDeclaringClass());
  } else if (name == "double java.lang.Math.log(double)") {
    JValue value;
    value.SetJ((static_cast<uint64_t>(args[1]) << 32) | args[0]);
    result->SetD(log(value.GetD()));
  } else if (name == "java.lang.String java.lang.Class.getNameNative()") {
    result->SetL(receiver->AsClass()->ComputeName());
  } else if (name == "int java.lang.Float.floatToRawIntBits(float)") {
    result->SetI(args[0]);
  } else if (name == "float java.lang.Float.intBitsToFloat(int)") {
    result->SetI(args[0]);
  } else if (name == "double java.lang.Math.exp(double)") {
    JValue value;
    value.SetJ((static_cast<uint64_t>(args[1]) << 32) | args[0]);
    result->SetD(exp(value.GetD()));
  } else if (name == "java.lang.Object java.lang.Object.internalClone()") {
    result->SetL(receiver->Clone(self));
  } else if (name == "void java.lang.Object.notifyAll()") {
    receiver->NotifyAll(self);
  } else if (name == "int java.lang.String.compareTo(java.lang.String)") {
    String* rhs = reinterpret_cast<Object*>(args[0])->AsString();
    CHECK(rhs != NULL);
    result->SetI(receiver->AsString()->CompareTo(rhs));
  } else if (name == "java.lang.String java.lang.String.intern()") {
    result->SetL(receiver->AsString()->Intern());
  } else if (name == "int java.lang.String.fastIndexOf(int, int)") {
    result->SetI(receiver->AsString()->FastIndexOf(args[0], args[1]));
  } else if (name == "java.lang.Object java.lang.reflect.Array.createMultiArray(java.lang.Class, int[])") {
    result->SetL(Array::CreateMultiArray(self, reinterpret_cast<Object*>(args[0])->AsClass(), reinterpret_cast<Object*>(args[1])->AsIntArray()));
  } else if (name == "java.lang.Object java.lang.Throwable.nativeFillInStackTrace()") {
    ScopedObjectAccessUnchecked soa(self);
    result->SetL(soa.Decode<Object*>(self->CreateInternalStackTrace(soa)));
  } else if (name == "boolean java.nio.ByteOrder.isLittleEndian()") {
    result->SetJ(JNI_TRUE);
  } else if (name == "boolean sun.misc.Unsafe.compareAndSwapInt(java.lang.Object, long, int, int)") {
    Object* obj = reinterpret_cast<Object*>(args[0]);
    jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
    jint expectedValue = args[3];
    jint newValue = args[4];
    byte* raw_addr = reinterpret_cast<byte*>(obj) + offset;
    volatile int32_t* address = reinterpret_cast<volatile int32_t*>(raw_addr);
    // Note: android_atomic_release_cas() returns 0 on success, not failure.
    int r = android_atomic_release_cas(expectedValue, newValue, address);
    result->SetZ(r == 0);
  } else if (name == "void sun.misc.Unsafe.putObject(java.lang.Object, long, java.lang.Object)") {
    Object* obj = reinterpret_cast<Object*>(args[0]);
    Object* newValue = reinterpret_cast<Object*>(args[3]);
    obj->SetFieldObject(MemberOffset((static_cast<uint64_t>(args[2]) << 32) | args[1]), newValue, false);
  } else {
    LOG(FATAL) << "Attempt to invoke native method in non-started runtime: " << name;
  }
}

static void InterpreterJni(Thread* self, AbstractMethod* method, StringPiece shorty,
                           Object* receiver, uint32_t* args, JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // TODO: The following enters JNI code using a typedef-ed function rather than the JNI compiler,
  //       it should be removed and JNI compiled stubs used instead.
  ScopedObjectAccessUnchecked soa(self);
  if (method->IsStatic()) {
    if (shorty == "L") {
      typedef jobject (fnptr)(JNIEnv*, jclass);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jobject jresult;
      {
        ScopedThreadStateChange tsc(self, kNative);
        jresult = fn(soa.Env(), klass.get());
      }
      result->SetL(soa.Decode<Object*>(jresult));
    } else if (shorty == "V") {
      typedef void (fnptr)(JNIEnv*, jclass);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), klass.get());
    } else if (shorty == "Z") {
      typedef jboolean (fnptr)(JNIEnv*, jclass);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetZ(fn(soa.Env(), klass.get()));
    } else if (shorty == "BI") {
      typedef jbyte (fnptr)(JNIEnv*, jclass, jint);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetB(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "II") {
      typedef jint (fnptr)(JNIEnv*, jclass, jint);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetI(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "LL") {
      typedef jobject (fnptr)(JNIEnv*, jclass, jobject);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[0])));
      jobject jresult;
      {
        ScopedThreadStateChange tsc(self, kNative);
        jresult = fn(soa.Env(), klass.get(), arg0.get());
      }
      result->SetL(soa.Decode<Object*>(jresult));
    } else if (shorty == "IIZ") {
      typedef jint (fnptr)(JNIEnv*, jclass, jint, jboolean);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetI(fn(soa.Env(), klass.get(), args[0], args[1]));
    } else if (shorty == "ILI") {
      typedef jint (fnptr)(JNIEnv*, jclass, jobject, jint);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[0])));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetI(fn(soa.Env(), klass.get(), arg0.get(), args[1]));
    } else if (shorty == "SIZ") {
      typedef jshort (fnptr)(JNIEnv*, jclass, jint, jboolean);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetS(fn(soa.Env(), klass.get(), args[0], args[1]));
    } else if (shorty == "VIZ") {
      typedef void (fnptr)(JNIEnv*, jclass, jint, jboolean);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), klass.get(), args[0], args[1]);
    } else if (shorty == "ZLL") {
      typedef jboolean (fnptr)(JNIEnv*, jclass, jobject, jobject);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[0])));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[1])));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetZ(fn(soa.Env(), klass.get(), arg0.get(), arg1.get()));
    } else if (shorty == "ZILL") {
      typedef jboolean (fnptr)(JNIEnv*, jclass, jint, jobject, jobject);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[1])));
      ScopedLocalRef<jobject> arg2(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[2])));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetZ(fn(soa.Env(), klass.get(), args[0], arg1.get(), arg2.get()));
    } else if (shorty == "VILII") {
      typedef void (fnptr)(JNIEnv*, jclass, jint, jobject, jint, jint);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[1])));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), klass.get(), args[0], arg1.get(), args[2], args[3]);
    } else if (shorty == "VLILII") {
      typedef void (fnptr)(JNIEnv*, jclass, jobject, jint, jobject, jint, jint);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[0])));
      ScopedLocalRef<jobject> arg2(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[2])));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), klass.get(), arg0.get(), args[1], arg2.get(), args[3], args[4]);
    } else {
      LOG(FATAL) << "Do something with static native method: " << PrettyMethod(method)
          << " shorty: " << shorty;
    }
  } else {
    if (shorty == "L") {
      typedef jobject (fnptr)(JNIEnv*, jobject);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      jobject jresult;
      {
        ScopedThreadStateChange tsc(self, kNative);
        jresult = fn(soa.Env(), rcvr.get());
      }
      result->SetL(soa.Decode<Object*>(jresult));
    } else if (shorty == "LL") {
      typedef jobject (fnptr)(JNIEnv*, jobject, jobject);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[0])));
      jobject jresult;
      {
        ScopedThreadStateChange tsc(self, kNative);
        jresult = fn(soa.Env(), rcvr.get(), arg0.get());

      }
      result->SetL(soa.Decode<Object*>(jresult));
      ScopedThreadStateChange tsc(self, kNative);
    } else if (shorty == "III") {
      typedef jint (fnptr)(JNIEnv*, jobject, jint, jint);
      fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetI(fn(soa.Env(), rcvr.get(), args[0], args[1]));
    } else {
      LOG(FATAL) << "Do something with native method: " << PrettyMethod(method)
          << " shorty: " << shorty;
    }
  }
}

static void DoMonitorEnter(Thread* self, Object* ref) NO_THREAD_SAFETY_ANALYSIS {
  ref->MonitorEnter(self);
}

static void DoMonitorExit(Thread* self, Object* ref) NO_THREAD_SAFETY_ANALYSIS {
  ref->MonitorExit(self);
}

static void DoInvoke(Thread* self, MethodHelper& mh, ShadowFrame& shadow_frame,
                     const DecodedInstruction& dec_insn, InvokeType type, bool is_range,
                     JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Object* receiver;
  if (type == kStatic) {
    receiver = NULL;
  } else {
    receiver = shadow_frame.GetVRegReference(dec_insn.vC);
  }
  uint32_t method_idx = dec_insn.vB;
  AbstractMethod* target_method = FindMethodFromCode(method_idx, receiver,
                                                     shadow_frame.GetMethod(), self, true,
                                                     type);
  if (UNLIKELY(target_method == NULL)) {
    CHECK(self->IsExceptionPending());
    result->SetJ(0);
    return;
  }
  mh.ChangeMethod(target_method);
  ArgArray arg_array(mh.GetShorty(), mh.GetShortyLength());
  if (is_range) {
    arg_array.BuildArgArray(shadow_frame, receiver, dec_insn.vC + (type != kStatic ? 1 : 0));
  } else {
    arg_array.BuildArgArray(shadow_frame, receiver, dec_insn.arg + (type != kStatic ? 1 : 0));
  }
  if (LIKELY(Runtime::Current()->IsStarted())) {
    JValue unused_result;
    if (mh.IsReturnFloatOrDouble()) {
      target_method->Invoke(self, arg_array.GetArray(), arg_array.GetNumBytes(),
                            &unused_result, result);
    } else {
      target_method->Invoke(self, arg_array.GetArray(), arg_array.GetNumBytes(),
                            result, &unused_result);
    }
  } else {
    uint32_t* args = arg_array.GetArray();
    if (type != kStatic) {
      args++;
    }
    UnstartedRuntimeInvoke(self, target_method, receiver, args, result);
  }
  mh.ChangeMethod(shadow_frame.GetMethod());
}

static void DoFieldGet(Thread* self, ShadowFrame& shadow_frame,
                       const DecodedInstruction& dec_insn, FindFieldType find_type,
                       Primitive::Type field_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  bool is_static = (find_type == StaticObjectRead) || (find_type == StaticPrimitiveRead);
  uint32_t field_idx = is_static ? dec_insn.vB : dec_insn.vC;
  Field* f = FindFieldFromCode(field_idx, shadow_frame.GetMethod(), self,
                               find_type, Primitive::FieldSize(field_type));
  if (LIKELY(f != NULL)) {
    Object* obj;
    if (is_static) {
      obj = f->GetDeclaringClass();
    } else {
      obj = shadow_frame.GetVRegReference(dec_insn.vB);
      if (UNLIKELY(obj == NULL)) {
        ThrowNullPointerExceptionForFieldAccess(f, true);
        return;
      }
    }
    switch (field_type) {
      case Primitive::kPrimBoolean:
        shadow_frame.SetVReg(dec_insn.vA, f->GetBoolean(obj));
        break;
      case Primitive::kPrimByte:
        shadow_frame.SetVReg(dec_insn.vA, f->GetByte(obj));
        break;
      case Primitive::kPrimChar:
        shadow_frame.SetVReg(dec_insn.vA, f->GetChar(obj));
        break;
      case Primitive::kPrimShort:
        shadow_frame.SetVReg(dec_insn.vA, f->GetShort(obj));
        break;
      case Primitive::kPrimInt:
        shadow_frame.SetVReg(dec_insn.vA, f->GetInt(obj));
        break;
      case Primitive::kPrimLong:
        shadow_frame.SetVRegLong(dec_insn.vA, f->GetLong(obj));
        break;
      case Primitive::kPrimNot:
        shadow_frame.SetVRegReference(dec_insn.vA, f->GetObject(obj));
        break;
      default:
        LOG(FATAL) << "Unreachable: " << field_type;
    }
  }
}

static void DoFieldPut(Thread* self, ShadowFrame& shadow_frame,
                       const DecodedInstruction& dec_insn, FindFieldType find_type,
                       Primitive::Type field_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  bool is_static = (find_type == StaticObjectWrite) || (find_type == StaticPrimitiveWrite);
  uint32_t field_idx = is_static ? dec_insn.vB : dec_insn.vC;
  Field* f = FindFieldFromCode(field_idx, shadow_frame.GetMethod(), self,
                               find_type, Primitive::FieldSize(field_type));
  if (LIKELY(f != NULL)) {
    Object* obj;
    if (is_static) {
      obj = f->GetDeclaringClass();
    } else {
      obj = shadow_frame.GetVRegReference(dec_insn.vB);
      if (UNLIKELY(obj == NULL)) {
        ThrowNullPointerExceptionForFieldAccess(f, false);
        return;
      }
    }
    switch (field_type) {
      case Primitive::kPrimBoolean:
        f->SetBoolean(obj, shadow_frame.GetVReg(dec_insn.vA));
        break;
      case Primitive::kPrimByte:
        f->SetByte(obj, shadow_frame.GetVReg(dec_insn.vA));
        break;
      case Primitive::kPrimChar:
        f->SetChar(obj, shadow_frame.GetVReg(dec_insn.vA));
        break;
      case Primitive::kPrimShort:
        f->SetShort(obj, shadow_frame.GetVReg(dec_insn.vA));
        break;
      case Primitive::kPrimInt:
        f->SetInt(obj, shadow_frame.GetVReg(dec_insn.vA));
        break;
      case Primitive::kPrimLong:
        f->SetLong(obj, shadow_frame.GetVRegLong(dec_insn.vA));
        break;
      case Primitive::kPrimNot:
        f->SetObj(obj, shadow_frame.GetVRegReference(dec_insn.vA));
        break;
      default:
        LOG(FATAL) << "Unreachable: " << field_type;
    }
  }
}

static void DoIntDivide(Thread* self, ShadowFrame& shadow_frame, size_t result_reg,
    int32_t dividend, int32_t divisor) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(divisor == 0)) {
    self->ThrowNewException("Ljava/lang/ArithmeticException;", "divide by zero");
  } else if (UNLIKELY(dividend == kMinInt && divisor == -1)) {
    shadow_frame.SetVReg(result_reg, kMinInt);
  } else {
    shadow_frame.SetVReg(result_reg, dividend / divisor);
  }
}

static void DoIntRemainder(Thread* self, ShadowFrame& shadow_frame, size_t result_reg,
    int32_t dividend, int32_t divisor) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(divisor == 0)) {
    self->ThrowNewException("Ljava/lang/ArithmeticException;", "divide by zero");
  } else if (UNLIKELY(dividend == kMinInt && divisor == -1)) {
    shadow_frame.SetVReg(result_reg, 0);
  } else {
    shadow_frame.SetVReg(result_reg, dividend % divisor);
  }
}

static void DoLongDivide(Thread* self, ShadowFrame& shadow_frame, size_t result_reg,
    int64_t dividend, int64_t divisor) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(divisor == 0)) {
    self->ThrowNewException("Ljava/lang/ArithmeticException;", "divide by zero");
  } else if (UNLIKELY(dividend == kMinLong && divisor == -1)) {
    shadow_frame.SetVRegLong(result_reg, kMinLong);
  } else {
    shadow_frame.SetVRegLong(result_reg, dividend / divisor);
  }
}

static void DoLongRemainder(Thread* self, ShadowFrame& shadow_frame, size_t result_reg,
    int64_t dividend, int64_t divisor) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(divisor == 0)) {
    self->ThrowNewException("Ljava/lang/ArithmeticException;", "divide by zero");
  } else if (UNLIKELY(dividend == kMinLong && divisor == -1)) {
    shadow_frame.SetVRegLong(result_reg, 0);
  } else {
    shadow_frame.SetVRegLong(result_reg, dividend % divisor);
  }
}

static JValue Execute(Thread* self, MethodHelper& mh, const DexFile::CodeItem* code_item,
                      ShadowFrame& shadow_frame, JValue result_register)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  const uint16_t* insns = code_item->insns_;
  const Instruction* inst = Instruction::At(insns + shadow_frame.GetDexPC());
  bool entry = (inst->GetDexPc(insns) == 0);
  while (true) {
    CheckSuspend(self);
    uint32_t dex_pc = inst->GetDexPc(insns);
    shadow_frame.SetDexPC(dex_pc);
    if (entry) {
      Dbg::UpdateDebugger(-1, self);
    }
    entry = false;
    Dbg::UpdateDebugger(dex_pc, self);
    DecodedInstruction dec_insn(inst);
    const bool kTracing = false;
    if (kTracing) {
      LOG(INFO) << PrettyMethod(shadow_frame.GetMethod())
                << StringPrintf("\n0x%x: %s\nReferences:",
                                inst->GetDexPc(insns), inst->DumpString(&mh.GetDexFile()).c_str());
      for (size_t i = 0; i < shadow_frame.NumberOfVRegs(); ++i) {
        Object* o = shadow_frame.GetVRegReference(i);
        if (o != NULL) {
          if (o->GetClass()->IsStringClass() && o->AsString()->GetCharArray() != NULL) {
            LOG(INFO) << i << ": java.lang.String " << static_cast<void*>(o)
                  << " \"" << o->AsString()->ToModifiedUtf8() << "\"";
          } else {
            LOG(INFO) << i << ": " << PrettyTypeOf(o) << " " << static_cast<void*>(o);
          }
        } else {
          LOG(INFO) << i << ": null";
        }
      }
      LOG(INFO) << "vregs:";
      for (size_t i = 0; i < shadow_frame.NumberOfVRegs(); ++i) {
        LOG(INFO) << StringPrintf("%d: %08x", i, shadow_frame.GetVReg(i));
      }
    }
    const Instruction* next_inst = inst->Next();
    switch (dec_insn.opcode) {
      case Instruction::NOP:
        break;
      case Instruction::MOVE:
      case Instruction::MOVE_FROM16:
      case Instruction::MOVE_16:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::MOVE_WIDE:
      case Instruction::MOVE_WIDE_FROM16:
      case Instruction::MOVE_WIDE_16:
        shadow_frame.SetVRegLong(dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::MOVE_OBJECT:
      case Instruction::MOVE_OBJECT_FROM16:
      case Instruction::MOVE_OBJECT_16:
        shadow_frame.SetVRegReference(dec_insn.vA, shadow_frame.GetVRegReference(dec_insn.vB));
        break;
      case Instruction::MOVE_RESULT:
        shadow_frame.SetVReg(dec_insn.vA, result_register.GetI());
        break;
      case Instruction::MOVE_RESULT_WIDE:
        shadow_frame.SetVRegLong(dec_insn.vA, result_register.GetJ());
        break;
      case Instruction::MOVE_RESULT_OBJECT:
        shadow_frame.SetVRegReference(dec_insn.vA, result_register.GetL());
        break;
      case Instruction::MOVE_EXCEPTION: {
        Throwable* exception = self->GetException();
        self->ClearException();
        shadow_frame.SetVRegReference(dec_insn.vA, exception);
        break;
      }
      case Instruction::RETURN_VOID: {
        JValue result;
        result.SetJ(0);
        return result;
      }
      case Instruction::RETURN: {
        JValue result;
        result.SetJ(0);
        result.SetI(shadow_frame.GetVReg(dec_insn.vA));
        return result;
      }
      case Instruction::RETURN_WIDE: {
        JValue result;
        result.SetJ(shadow_frame.GetVRegLong(dec_insn.vA));
        return result;
      }
      case Instruction::RETURN_OBJECT: {
        JValue result;
        result.SetJ(0);
        result.SetL(shadow_frame.GetVRegReference(dec_insn.vA));
        return result;
      }
      case Instruction::CONST_4: {
        int32_t val = static_cast<int32_t>(dec_insn.vB << 28) >> 28;
        shadow_frame.SetVReg(dec_insn.vA, val);
        if (val == 0) {
          shadow_frame.SetVRegReference(dec_insn.vA, NULL);
        }
        break;
      }
      case Instruction::CONST_16: {
        int32_t val = static_cast<int16_t>(dec_insn.vB);
        shadow_frame.SetVReg(dec_insn.vA, val);
        if (val == 0) {
          shadow_frame.SetVRegReference(dec_insn.vA, NULL);
        }
        break;
      }
      case Instruction::CONST: {
        int32_t val = dec_insn.vB;
        shadow_frame.SetVReg(dec_insn.vA, val);
        if (val == 0) {
          shadow_frame.SetVRegReference(dec_insn.vA, NULL);
        }
        break;
      }
      case Instruction::CONST_HIGH16: {
        int32_t val = dec_insn.vB << 16;
        shadow_frame.SetVReg(dec_insn.vA, val);
        if (val == 0) {
          shadow_frame.SetVRegReference(dec_insn.vA, NULL);
        }
        break;
      }
      case Instruction::CONST_WIDE_16:
        shadow_frame.SetVRegLong(dec_insn.vA, static_cast<int16_t>(dec_insn.vB));
        break;
      case Instruction::CONST_WIDE_32:
        shadow_frame.SetVRegLong(dec_insn.vA, static_cast<int32_t>(dec_insn.vB));
        break;
      case Instruction::CONST_WIDE:
        shadow_frame.SetVRegLong(dec_insn.vA, dec_insn.vB_wide);
        break;
      case Instruction::CONST_WIDE_HIGH16:
        shadow_frame.SetVRegLong(dec_insn.vA, static_cast<uint64_t>(dec_insn.vB) << 48);
        break;
      case Instruction::CONST_STRING:
      case Instruction::CONST_STRING_JUMBO: {
        if (UNLIKELY(!String::GetJavaLangString()->IsInitialized())) {
          Runtime::Current()->GetClassLinker()->EnsureInitialized(String::GetJavaLangString(),
                                                                  true, true);
        }
        String* s = mh.ResolveString(dec_insn.vB);
        shadow_frame.SetVRegReference(dec_insn.vA, s);
        break;
      }
      case Instruction::CONST_CLASS: {
        Class* c = ResolveVerifyAndClinit(dec_insn.vB, shadow_frame.GetMethod(), self, false, true);
        shadow_frame.SetVRegReference(dec_insn.vA, c);
        break;
      }
      case Instruction::MONITOR_ENTER: {
        Object* obj = shadow_frame.GetVRegReference(dec_insn.vA);
        if (UNLIKELY(obj == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
        } else {
          DoMonitorEnter(self, obj);
        }
        break;
      }
      case Instruction::MONITOR_EXIT: {
        Object* obj = shadow_frame.GetVRegReference(dec_insn.vA);
        if (UNLIKELY(obj == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
        } else {
          DoMonitorExit(self, obj);
        }
        break;
      }
      case Instruction::CHECK_CAST: {
        Class* c = ResolveVerifyAndClinit(dec_insn.vB, shadow_frame.GetMethod(), self, false, true);
        if (UNLIKELY(c == NULL)) {
          CHECK(self->IsExceptionPending());
        } else {
          Object* obj = shadow_frame.GetVRegReference(dec_insn.vA);
          if (UNLIKELY(obj != NULL && !obj->InstanceOf(c))) {
            self->ThrowNewExceptionF("Ljava/lang/ClassCastException;",
                "%s cannot be cast to %s",
                PrettyDescriptor(obj->GetClass()).c_str(),
                PrettyDescriptor(c).c_str());
          }
        }
        break;
      }
      case Instruction::INSTANCE_OF: {
        Class* c = ResolveVerifyAndClinit(dec_insn.vC, shadow_frame.GetMethod(), self, false, true);
        if (UNLIKELY(c == NULL)) {
          CHECK(self->IsExceptionPending());
        } else {
          Object* obj = shadow_frame.GetVRegReference(dec_insn.vB);
          shadow_frame.SetVReg(dec_insn.vA, (obj != NULL && obj->InstanceOf(c)) ? 1 : 0);
        }
        break;
      }
      case Instruction::ARRAY_LENGTH:  {
        Object* array = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(array == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        shadow_frame.SetVReg(dec_insn.vA, array->AsArray()->GetLength());
        break;
      }
      case Instruction::NEW_INSTANCE: {
        Object* obj = AllocObjectFromCode(dec_insn.vB, shadow_frame.GetMethod(), self, true);
        shadow_frame.SetVRegReference(dec_insn.vA, obj);
        break;
      }
      case Instruction::NEW_ARRAY: {
        int32_t length = shadow_frame.GetVReg(dec_insn.vB);
        Object* obj = AllocArrayFromCode(dec_insn.vC, shadow_frame.GetMethod(), length, self, true);
        shadow_frame.SetVRegReference(dec_insn.vA, obj);
        break;
      }
      case Instruction::FILLED_NEW_ARRAY:
      case Instruction::FILLED_NEW_ARRAY_RANGE: {
        bool is_range = (dec_insn.opcode == Instruction::FILLED_NEW_ARRAY_RANGE);
        int32_t length = dec_insn.vA;
        CHECK(is_range || length <= 5);
        if (UNLIKELY(length < 0)) {
          self->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", length);
          break;
        }
        Class* arrayClass = ResolveVerifyAndClinit(dec_insn.vB, shadow_frame.GetMethod(), self, false, true);
        if (UNLIKELY(arrayClass == NULL)) {
          CHECK(self->IsExceptionPending());
          break;
        }
        CHECK(arrayClass->IsArrayClass());
        Class* componentClass = arrayClass->GetComponentType();
        if (UNLIKELY(componentClass->IsPrimitive() && !componentClass->IsPrimitiveInt())) {
          if (componentClass->IsPrimitiveLong() || componentClass->IsPrimitiveDouble()) {
            self->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
                                     "Bad filled array request for type %s",
                                     PrettyDescriptor(componentClass).c_str());
          } else {
            self->ThrowNewExceptionF("Ljava/lang/InternalError;",
                                     "Found type %s; filled-new-array not implemented for anything but \'int\'",
                                     PrettyDescriptor(componentClass).c_str());
          }
          break;
        }
        Object* newArray = Array::Alloc(self, arrayClass, length);
        if (newArray != NULL) {
          for (int32_t i = 0; i < length; ++i) {
            if (is_range) {
              if (componentClass->IsPrimitiveInt()) {
                newArray->AsIntArray()->Set(i, shadow_frame.GetVReg(dec_insn.vC + i));
              } else {
                newArray->AsObjectArray<Object>()->Set(i, shadow_frame.GetVRegReference(dec_insn.vC + i));
              }
            } else {
              if (componentClass->IsPrimitiveInt()) {
                newArray->AsIntArray()->Set(i, shadow_frame.GetVReg(dec_insn.arg[i]));
              } else {
                newArray->AsObjectArray<Object>()->Set(i, shadow_frame.GetVRegReference(dec_insn.arg[i]));
              }
            }
          }
        }
        result_register.SetL(newArray);
        break;
      }
      case Instruction::CMPL_FLOAT: {
        float val1 = shadow_frame.GetVRegFloat(dec_insn.vB);
        float val2 = shadow_frame.GetVRegFloat(dec_insn.vC);
        int32_t result;
        if (val1 == val2) {
          result = 0;
        } else if (val1 > val2) {
          result = 1;
        } else {
          result = -1;
        }
        shadow_frame.SetVReg(dec_insn.vA, result);
        break;
      }
      case Instruction::CMPG_FLOAT: {
        float val1 = shadow_frame.GetVRegFloat(dec_insn.vB);
        float val2 = shadow_frame.GetVRegFloat(dec_insn.vC);
        int32_t result;
        if (val1 == val2) {
          result = 0;
        } else if (val1 < val2) {
          result = -1;
        } else {
          result = 1;
        }
        shadow_frame.SetVReg(dec_insn.vA, result);
        break;
      }
      case Instruction::CMPL_DOUBLE: {
        double val1 = shadow_frame.GetVRegDouble(dec_insn.vB);
        double val2 = shadow_frame.GetVRegDouble(dec_insn.vC);
        int32_t result;
        if (val1 == val2) {
          result = 0;
        } else if (val1 > val2) {
          result = 1;
        } else {
          result = -1;
        }
        shadow_frame.SetVReg(dec_insn.vA, result);
        break;
      }

      case Instruction::CMPG_DOUBLE: {
        double val1 = shadow_frame.GetVRegDouble(dec_insn.vB);
        double val2 = shadow_frame.GetVRegDouble(dec_insn.vC);
        int32_t result;
        if (val1 == val2) {
          result = 0;
        } else if (val1 < val2) {
          result = -1;
        } else {
          result = 1;
        }
        shadow_frame.SetVReg(dec_insn.vA, result);
        break;
      }
      case Instruction::CMP_LONG: {
        int64_t val1 = shadow_frame.GetVRegLong(dec_insn.vB);
        int64_t val2 = shadow_frame.GetVRegLong(dec_insn.vC);
        int32_t result;
        if (val1 > val2) {
          result = 1;
        } else if (val1 == val2) {
          result = 0;
        } else {
          result = -1;
        }
        shadow_frame.SetVReg(dec_insn.vA, result);
        break;
      }
      case Instruction::THROW: {
        Object* o = shadow_frame.GetVRegReference(dec_insn.vA);
        Throwable* t = (o == NULL) ? NULL : o->AsThrowable();
        self->DeliverException(t);
        break;
      }
      case Instruction::GOTO:
      case Instruction::GOTO_16:
      case Instruction::GOTO_32: {
        uint32_t dex_pc = inst->GetDexPc(insns);
        next_inst = Instruction::At(insns + dex_pc + dec_insn.vA);
        break;
      }
      case Instruction::PACKED_SWITCH: {
        uint32_t dex_pc = inst->GetDexPc(insns);
        const uint16_t* switch_data = insns + dex_pc + dec_insn.vB;
        int32_t test_val = shadow_frame.GetVReg(dec_insn.vA);
        CHECK_EQ(switch_data[0], static_cast<uint16_t>(Instruction::kPackedSwitchSignature));
        uint16_t size = switch_data[1];
        CHECK_GT(size, 0);
        const int32_t* keys = reinterpret_cast<const int32_t*>(&switch_data[2]);
        CHECK(IsAligned<4>(keys));
        int32_t first_key = keys[0];
        const int32_t* targets = reinterpret_cast<const int32_t*>(&switch_data[4]);
        CHECK(IsAligned<4>(targets));
        int32_t index = test_val - first_key;
        if (index >= 0 && index < size) {
          next_inst = Instruction::At(insns + dex_pc + targets[index]);
        }
        break;
      }
      case Instruction::SPARSE_SWITCH: {
        uint32_t dex_pc = inst->GetDexPc(insns);
        const uint16_t* switch_data = insns + dex_pc + dec_insn.vB;
        int32_t test_val = shadow_frame.GetVReg(dec_insn.vA);
        CHECK_EQ(switch_data[0], static_cast<uint16_t>(Instruction::kSparseSwitchSignature));
        uint16_t size = switch_data[1];
        CHECK_GT(size, 0);
        const int32_t* keys = reinterpret_cast<const int32_t*>(&switch_data[2]);
        CHECK(IsAligned<4>(keys));
        const int32_t* entries = keys + size;
        CHECK(IsAligned<4>(entries));
        int lo = 0;
        int hi = size - 1;
        while (lo <= hi) {
          int mid = (lo + hi) / 2;
          int32_t foundVal = keys[mid];
          if (test_val < foundVal) {
            hi = mid - 1;
          } else if (test_val > foundVal) {
            lo = mid + 1;
          } else {
            next_inst = Instruction::At(insns + dex_pc + entries[mid]);
            break;
          }
        }
        break;
      }
      case Instruction::FILL_ARRAY_DATA: {
        Object* obj = shadow_frame.GetVRegReference(dec_insn.vA);
        if (UNLIKELY(obj == NULL)) {
          Thread::Current()->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
              "null array in FILL_ARRAY_DATA");
          break;
        }
        Array* array = obj->AsArray();
        DCHECK(array->IsArrayInstance() && !array->IsObjectArray());
        uint32_t dex_pc = inst->GetDexPc(insns);
        const Instruction::ArrayDataPayload* payload =
            reinterpret_cast<const Instruction::ArrayDataPayload*>(insns + dex_pc + dec_insn.vB);
        if (UNLIKELY(static_cast<int32_t>(payload->element_count) > array->GetLength())) {
          Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                                                "failed FILL_ARRAY_DATA; length=%d, index=%d",
                                                array->GetLength(), payload->element_count);
          break;
        }
        uint32_t size_in_bytes = payload->element_count * payload->element_width;
        memcpy(array->GetRawData(payload->element_width), payload->data, size_in_bytes);
        break;
      }
      case Instruction::IF_EQ: {
        if (shadow_frame.GetVReg(dec_insn.vA) == shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_NE: {
        if (shadow_frame.GetVReg(dec_insn.vA) != shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_LT: {
        if (shadow_frame.GetVReg(dec_insn.vA) < shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_GE: {
        if (shadow_frame.GetVReg(dec_insn.vA) >= shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_GT: {
        if (shadow_frame.GetVReg(dec_insn.vA) > shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_LE: {
        if (shadow_frame.GetVReg(dec_insn.vA) <= shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_EQZ: {
        if (shadow_frame.GetVReg(dec_insn.vA) == 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::IF_NEZ: {
        if (shadow_frame.GetVReg(dec_insn.vA) != 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::IF_LTZ: {
        if (shadow_frame.GetVReg(dec_insn.vA) < 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::IF_GEZ: {
        if (shadow_frame.GetVReg(dec_insn.vA) >= 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::IF_GTZ: {
        if (shadow_frame.GetVReg(dec_insn.vA) > 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::IF_LEZ:  {
        if (shadow_frame.GetVReg(dec_insn.vA) <= 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::AGET_BOOLEAN: {
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVReg(dec_insn.vA, a->AsBooleanArray()->Get(index));
        break;
      }
      case Instruction::AGET_BYTE: {
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVReg(dec_insn.vA, a->AsByteArray()->Get(index));
        break;
      }
      case Instruction::AGET_CHAR: {
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVReg(dec_insn.vA, a->AsCharArray()->Get(index));
        break;
      }
      case Instruction::AGET_SHORT: {
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVReg(dec_insn.vA, a->AsShortArray()->Get(index));
        break;
      }
      case Instruction::AGET: {
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVReg(dec_insn.vA, a->AsIntArray()->Get(index));
        break;
      }
      case Instruction::AGET_WIDE:  {
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVRegLong(dec_insn.vA, a->AsLongArray()->Get(index));
        break;
      }
      case Instruction::AGET_OBJECT: {
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVRegReference(dec_insn.vA, a->AsObjectArray<Object>()->Get(index));
        break;
      }
      case Instruction::APUT_BOOLEAN: {
        uint8_t val = shadow_frame.GetVReg(dec_insn.vA);
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->AsBooleanArray()->Set(index, val);
        break;
      }
      case Instruction::APUT_BYTE: {
        int8_t val = shadow_frame.GetVReg(dec_insn.vA);
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->AsByteArray()->Set(index, val);
        break;
      }
      case Instruction::APUT_CHAR: {
        uint16_t val = shadow_frame.GetVReg(dec_insn.vA);
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->AsCharArray()->Set(index, val);
        break;
      }
      case Instruction::APUT_SHORT: {
        int16_t val = shadow_frame.GetVReg(dec_insn.vA);
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->AsShortArray()->Set(index, val);
        break;
      }
      case Instruction::APUT: {
        int32_t val = shadow_frame.GetVReg(dec_insn.vA);
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->AsIntArray()->Set(index, val);
        break;
      }
      case Instruction::APUT_WIDE: {
        int64_t val = shadow_frame.GetVRegLong(dec_insn.vA);
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->AsLongArray()->Set(index, val);
        break;
      }
      case Instruction::APUT_OBJECT: {
        Object* val = shadow_frame.GetVRegReference(dec_insn.vA);
        Object* a = shadow_frame.GetVRegReference(dec_insn.vB);
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->AsObjectArray<Object>()->Set(index, val);
        break;
      }
      case Instruction::IGET_BOOLEAN:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimBoolean);
        break;
      case Instruction::IGET_BYTE:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimByte);
        break;
      case Instruction::IGET_CHAR:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimChar);
        break;
      case Instruction::IGET_SHORT:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimShort);
        break;
      case Instruction::IGET:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimInt);
        break;
      case Instruction::IGET_WIDE:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimLong);
        break;
      case Instruction::IGET_OBJECT:
        DoFieldGet(self, shadow_frame, dec_insn, InstanceObjectRead, Primitive::kPrimNot);
        break;
      case Instruction::SGET_BOOLEAN:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimBoolean);
        break;
      case Instruction::SGET_BYTE:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimByte);
        break;
      case Instruction::SGET_CHAR:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimChar);
        break;
      case Instruction::SGET_SHORT:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimShort);
        break;
      case Instruction::SGET:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimInt);
        break;
      case Instruction::SGET_WIDE:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimLong);
        break;
      case Instruction::SGET_OBJECT:
        DoFieldGet(self, shadow_frame, dec_insn, StaticObjectRead, Primitive::kPrimNot);
        break;
      case Instruction::IPUT_BOOLEAN:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimBoolean);
        break;
      case Instruction::IPUT_BYTE:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimByte);
        break;
      case Instruction::IPUT_CHAR:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimChar);
        break;
      case Instruction::IPUT_SHORT:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimShort);
        break;
      case Instruction::IPUT:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimInt);
        break;
      case Instruction::IPUT_WIDE:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimLong);
        break;
      case Instruction::IPUT_OBJECT:
        DoFieldPut(self, shadow_frame, dec_insn, InstanceObjectWrite, Primitive::kPrimNot);
        break;
      case Instruction::SPUT_BOOLEAN:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimBoolean);
        break;
      case Instruction::SPUT_BYTE:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimByte);
        break;
      case Instruction::SPUT_CHAR:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimChar);
        break;
      case Instruction::SPUT_SHORT:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimShort);
        break;
      case Instruction::SPUT:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimInt);
        break;
      case Instruction::SPUT_WIDE:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimLong);
        break;
      case Instruction::SPUT_OBJECT:
        DoFieldPut(self, shadow_frame, dec_insn, StaticObjectWrite, Primitive::kPrimNot);
        break;
      case Instruction::INVOKE_VIRTUAL:
        DoInvoke(self, mh, shadow_frame, dec_insn, kVirtual, false, &result_register);
        break;
      case Instruction::INVOKE_VIRTUAL_RANGE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kVirtual, true, &result_register);
        break;
      case Instruction::INVOKE_SUPER:
        DoInvoke(self, mh, shadow_frame, dec_insn, kSuper, false, &result_register);
        break;
      case Instruction::INVOKE_SUPER_RANGE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kSuper, true, &result_register);
        break;
      case Instruction::INVOKE_DIRECT:
        DoInvoke(self, mh, shadow_frame, dec_insn, kDirect, false, &result_register);
        break;
      case Instruction::INVOKE_DIRECT_RANGE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kDirect, true, &result_register);
        break;
      case Instruction::INVOKE_INTERFACE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kInterface, false, &result_register);
        break;
      case Instruction::INVOKE_INTERFACE_RANGE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kInterface, true, &result_register);
        break;
      case Instruction::INVOKE_STATIC:
        DoInvoke(self, mh, shadow_frame, dec_insn, kStatic, false, &result_register);
        break;
      case Instruction::INVOKE_STATIC_RANGE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kStatic, true, &result_register);
        break;
      case Instruction::NEG_INT:
        shadow_frame.SetVReg(dec_insn.vA, -shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::NOT_INT:
        shadow_frame.SetVReg(dec_insn.vA, ~shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::NEG_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA, -shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::NOT_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA, ~shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::NEG_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA, -shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::NEG_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA, -shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::INT_TO_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::INT_TO_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::INT_TO_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::LONG_TO_INT:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::LONG_TO_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::LONG_TO_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::FLOAT_TO_INT: {
        float val = shadow_frame.GetVRegFloat(dec_insn.vB);
        if (val != val) {
          shadow_frame.SetVReg(dec_insn.vA, 0);
        } else if (val > static_cast<float>(kMaxInt)) {
          shadow_frame.SetVReg(dec_insn.vA, kMaxInt);
        } else if (val < static_cast<float>(kMinInt)) {
          shadow_frame.SetVReg(dec_insn.vA, kMinInt);
        } else {
          shadow_frame.SetVReg(dec_insn.vA, val);
        }
        break;
      }
      case Instruction::FLOAT_TO_LONG: {
        float val = shadow_frame.GetVRegFloat(dec_insn.vB);
        if (val != val) {
          shadow_frame.SetVRegLong(dec_insn.vA, 0);
        } else if (val > static_cast<float>(kMaxLong)) {
          shadow_frame.SetVRegLong(dec_insn.vA, kMaxLong);
        } else if (val < static_cast<float>(kMinLong)) {
          shadow_frame.SetVRegLong(dec_insn.vA, kMinLong);
        } else {
          shadow_frame.SetVRegLong(dec_insn.vA, val);
        }
        break;
      }
      case Instruction::FLOAT_TO_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA, shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::DOUBLE_TO_INT: {
        double val = shadow_frame.GetVRegDouble(dec_insn.vB);
        if (val != val) {
          shadow_frame.SetVReg(dec_insn.vA, 0);
        } else if (val > static_cast<double>(kMaxInt)) {
          shadow_frame.SetVReg(dec_insn.vA, kMaxInt);
        } else if (val < static_cast<double>(kMinInt)) {
          shadow_frame.SetVReg(dec_insn.vA, kMinInt);
        } else {
          shadow_frame.SetVReg(dec_insn.vA, val);
        }
        break;
      }
      case Instruction::DOUBLE_TO_LONG: {
        double val = shadow_frame.GetVRegDouble(dec_insn.vB);
        if (val != val) {
          shadow_frame.SetVRegLong(dec_insn.vA, 0);
        } else if (val > static_cast<double>(kMaxLong)) {
          shadow_frame.SetVRegLong(dec_insn.vA, kMaxLong);
        } else if (val < static_cast<double>(kMinLong)) {
          shadow_frame.SetVRegLong(dec_insn.vA, kMinLong);
        } else {
          shadow_frame.SetVRegLong(dec_insn.vA, val);
        }
        break;
      }
      case Instruction::DOUBLE_TO_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA, shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::INT_TO_BYTE:
        shadow_frame.SetVReg(dec_insn.vA, static_cast<int8_t>(shadow_frame.GetVReg(dec_insn.vB)));
        break;
      case Instruction::INT_TO_CHAR:
        shadow_frame.SetVReg(dec_insn.vA, static_cast<uint16_t>(shadow_frame.GetVReg(dec_insn.vB)));
        break;
      case Instruction::INT_TO_SHORT:
        shadow_frame.SetVReg(dec_insn.vA, static_cast<int16_t>(shadow_frame.GetVReg(dec_insn.vB)));
        break;
      case Instruction::ADD_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) + shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::SUB_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) - shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::MUL_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) * shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::REM_INT:
        DoIntRemainder(self, shadow_frame, dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB),
                       shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::DIV_INT:
        DoIntDivide(self, shadow_frame, dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB),
                    shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::SHL_INT:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) <<
                             (shadow_frame.GetVReg(dec_insn.vC) & 0x1f));
        break;
      case Instruction::SHR_INT:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) >>
                             (shadow_frame.GetVReg(dec_insn.vC) & 0x1f));
        break;
      case Instruction::USHR_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             static_cast<uint32_t>(shadow_frame.GetVReg(dec_insn.vB)) >>
                             (shadow_frame.GetVReg(dec_insn.vC) & 0x1f));
        break;
      case Instruction::AND_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) & shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::OR_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) | shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::XOR_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) ^ shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::ADD_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) +
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::SUB_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) -
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::MUL_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) *
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::DIV_LONG:
        DoLongDivide(self, shadow_frame, dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vB),
                    shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::REM_LONG:
        DoLongRemainder(self, shadow_frame, dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vB),
                        shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::AND_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) &
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::OR_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) |
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::XOR_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) ^
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::SHL_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) <<
                                 (shadow_frame.GetVReg(dec_insn.vC) & 0x3f));
        break;
      case Instruction::SHR_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) >>
                                 (shadow_frame.GetVReg(dec_insn.vC) & 0x3f));
        break;
      case Instruction::USHR_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 static_cast<uint64_t>(shadow_frame.GetVRegLong(dec_insn.vB)) >>
                                 (shadow_frame.GetVReg(dec_insn.vC) & 0x3f));
        break;
      case Instruction::ADD_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vB) +
                                  shadow_frame.GetVRegFloat(dec_insn.vC));
        break;
      case Instruction::SUB_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vB) -
                                  shadow_frame.GetVRegFloat(dec_insn.vC));
        break;
      case Instruction::MUL_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vB) *
                                  shadow_frame.GetVRegFloat(dec_insn.vC));
        break;
      case Instruction::DIV_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vB) /
                                  shadow_frame.GetVRegFloat(dec_insn.vC));
        break;
      case Instruction::REM_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  fmodf(shadow_frame.GetVRegFloat(dec_insn.vB),
                                        shadow_frame.GetVRegFloat(dec_insn.vC)));
        break;
      case Instruction::ADD_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vB) +
                                   shadow_frame.GetVRegDouble(dec_insn.vC));
        break;
      case Instruction::SUB_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vB) -
                                   shadow_frame.GetVRegDouble(dec_insn.vC));
        break;
      case Instruction::MUL_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vB) *
                                   shadow_frame.GetVRegDouble(dec_insn.vC));
        break;
      case Instruction::DIV_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vB) /
                                   shadow_frame.GetVRegDouble(dec_insn.vC));
        break;
      case Instruction::REM_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   fmod(shadow_frame.GetVRegDouble(dec_insn.vB),
                                        shadow_frame.GetVRegDouble(dec_insn.vC)));
        break;
      case Instruction::ADD_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) + shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::SUB_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) - shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::MUL_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) * shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::REM_INT_2ADDR:
        DoIntRemainder(self, shadow_frame, dec_insn.vA, shadow_frame.GetVReg(dec_insn.vA),
                       shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::SHL_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vA) <<
                             (shadow_frame.GetVReg(dec_insn.vB) & 0x1f));
        break;
      case Instruction::SHR_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vA) >>
                             (shadow_frame.GetVReg(dec_insn.vB) & 0x1f));
        break;
      case Instruction::USHR_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             static_cast<uint32_t>(shadow_frame.GetVReg(dec_insn.vA)) >>
                             (shadow_frame.GetVReg(dec_insn.vB) & 0x1f));
        break;
      case Instruction::AND_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) & shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::OR_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) | shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::XOR_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) ^ shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::DIV_INT_2ADDR:
        DoIntDivide(self, shadow_frame, dec_insn.vA, shadow_frame.GetVReg(dec_insn.vA),
                    shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::ADD_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) +
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::SUB_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) -
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::MUL_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) *
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::DIV_LONG_2ADDR:
        DoLongDivide(self, shadow_frame, dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vA),
                    shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::REM_LONG_2ADDR:
        DoLongRemainder(self, shadow_frame, dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vA),
                        shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::AND_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) &
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::OR_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) |
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::XOR_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) ^
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::SHL_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) <<
                                 (shadow_frame.GetVReg(dec_insn.vB) & 0x3f));
        break;
      case Instruction::SHR_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) >>
                                 (shadow_frame.GetVReg(dec_insn.vB) & 0x3f));
        break;
      case Instruction::USHR_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 static_cast<uint64_t>(shadow_frame.GetVRegLong(dec_insn.vA)) >>
                                 (shadow_frame.GetVReg(dec_insn.vB) & 0x3f));
        break;
      case Instruction::ADD_FLOAT_2ADDR:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vA) +
                                  shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::SUB_FLOAT_2ADDR:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vA) -
                                  shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::MUL_FLOAT_2ADDR:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vA) *
                                  shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::DIV_FLOAT_2ADDR:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vA) /
                                  shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::REM_FLOAT_2ADDR:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  fmodf(shadow_frame.GetVRegFloat(dec_insn.vA),
                                        shadow_frame.GetVRegFloat(dec_insn.vB)));
        break;
      case Instruction::ADD_DOUBLE_2ADDR:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vA) +
                                   shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::SUB_DOUBLE_2ADDR:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vA) -
                                   shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::MUL_DOUBLE_2ADDR:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vA) *
                                   shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::DIV_DOUBLE_2ADDR:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vA) /
                                   shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::REM_DOUBLE_2ADDR:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   fmod(shadow_frame.GetVRegDouble(dec_insn.vA),
                                        shadow_frame.GetVRegDouble(dec_insn.vB)));
        break;
      case Instruction::ADD_INT_LIT16:
      case Instruction::ADD_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) + dec_insn.vC);
        break;
      case Instruction::RSUB_INT:
      case Instruction::RSUB_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, dec_insn.vC - shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::MUL_INT_LIT16:
      case Instruction::MUL_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) * dec_insn.vC);
        break;
      case Instruction::DIV_INT_LIT16:
      case Instruction::DIV_INT_LIT8:
        DoIntDivide(self, shadow_frame, dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB),
                    dec_insn.vC);
        break;
      case Instruction::REM_INT_LIT16:
      case Instruction::REM_INT_LIT8:
        DoIntRemainder(self, shadow_frame, dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB),
                       dec_insn.vC);
        break;
      case Instruction::AND_INT_LIT16:
      case Instruction::AND_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) & dec_insn.vC);
        break;
      case Instruction::OR_INT_LIT16:
      case Instruction::OR_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) | dec_insn.vC);
        break;
      case Instruction::XOR_INT_LIT16:
      case Instruction::XOR_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) ^ dec_insn.vC);
        break;
      case Instruction::SHL_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) <<
                             (dec_insn.vC & 0x1f));
        break;
      case Instruction::SHR_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) >>
                             (dec_insn.vC & 0x1f));
        break;
      case Instruction::USHR_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA,
                             static_cast<uint32_t>(shadow_frame.GetVReg(dec_insn.vB)) >>
                             (dec_insn.vC & 0x1f));
        break;
      default:
        LOG(FATAL) << "Unexpected instruction: " << inst->DumpString(&mh.GetDexFile());
        break;
    }
    if (UNLIKELY(self->IsExceptionPending())) {
      if (throw_frame_id_ == 0) {
        throw_method_ = shadow_frame.GetMethod();
        throw_dex_pc_ = dex_pc;
      }
      throw_frame_id_++;
      uint32_t found_dex_pc =
          shadow_frame.GetMethod()->FindCatchBlock(self->GetException()->GetClass(),
                                                   inst->GetDexPc(insns));
      if (found_dex_pc == DexFile::kDexNoIndex) {
        JValue result;
        result.SetJ(0);
        return result;  // Handler in caller.
      } else {
        Dbg::PostException(self, throw_frame_id_, throw_method_, throw_dex_pc_,
                           shadow_frame.GetMethod(), found_dex_pc, self->GetException());
        throw_frame_id_ = 0;
        next_inst = Instruction::At(insns + found_dex_pc);
      }
    }
    inst = next_inst;
  }
}

void EnterInterpreterFromInvoke(Thread* self, AbstractMethod* method, Object* receiver,
                                uint32_t* args, JValue* result, JValue* float_result) {
  DCHECK_EQ(self, Thread::Current());
  if (__builtin_frame_address(0) < self->GetStackEnd()) {
    ThrowStackOverflowError(self);
    return;
  }

  MethodHelper mh(method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  uint16_t num_regs;
  uint16_t num_ins;
  if (code_item != NULL) {
    num_regs =  code_item->registers_size_;
    num_ins = code_item->ins_size_;
  } else if (method->IsAbstract()) {
    self->ThrowNewExceptionF("Ljava/lang/AbstractMethodError;", "abstract method \"%s\"",
                             PrettyMethod(method).c_str());
    return;
  } else {
    DCHECK(method->IsNative());
    num_regs = num_ins = AbstractMethod::NumArgRegisters(mh.GetShorty());
    if (!method->IsStatic()) {
      num_regs++;
      num_ins++;
    }
  }
  // Set up shadow frame with matching number of reference slots to vregs.
  ShadowFrame* last_shadow_frame = self->GetManagedStack()->GetTopShadowFrame();
  UniquePtr<ShadowFrame> shadow_frame(ShadowFrame::Create(num_regs,
                                                          last_shadow_frame,
                                                          method, 0));
  self->PushShadowFrame(shadow_frame.get());
  size_t cur_reg = num_regs - num_ins;
  if (!method->IsStatic()) {
    CHECK(receiver != NULL);
    shadow_frame->SetVRegReference(cur_reg, receiver);
    ++cur_reg;
  } else if (!method->GetDeclaringClass()->IsInitializing()) {
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(method->GetDeclaringClass(),
                                                                 true, true)) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return;
    }
    CHECK(method->GetDeclaringClass()->IsInitializing());
  }
  const char* shorty = mh.GetShorty();
  for (size_t shorty_pos = 0, arg_pos = 0; cur_reg < num_regs; ++shorty_pos, ++arg_pos, cur_reg++) {
    DCHECK_LT(shorty_pos + 1, mh.GetShortyLength());
    switch (shorty[shorty_pos + 1]) {
      case 'L': {
        Object* o = reinterpret_cast<Object*>(args[arg_pos]);
        shadow_frame->SetVRegReference(cur_reg, o);
        break;
      }
      case 'J': case 'D': {
        uint64_t wide_value = (static_cast<uint64_t>(args[arg_pos + 1]) << 32) | args[arg_pos];
        shadow_frame->SetVRegLong(cur_reg, wide_value);
        cur_reg++;
        arg_pos++;
        break;
      }
      default:
        shadow_frame->SetVReg(cur_reg, args[arg_pos]);
        break;
    }
  }
  if (LIKELY(!method->IsNative())) {
    JValue r = Execute(self, mh, code_item, *shadow_frame.get(), JValue());
    if (result != NULL && float_result != NULL) {
      if (mh.IsReturnFloatOrDouble()) {
        *float_result = r;
      } else {
        *result = r;
      }
    }
  } else {
    // We don't expect to be asked to interpret native code (which is entered via a JNI compiler
    // generated stub) except during testing and image writing.
    if (!Runtime::Current()->IsStarted()) {
      if (mh.IsReturnFloatOrDouble()) {
        UnstartedRuntimeJni(self, method, receiver, args, float_result);
      } else {
        UnstartedRuntimeJni(self, method, receiver, args, result);
      }
    } else {
      if (mh.IsReturnFloatOrDouble()) {
        InterpreterJni(self, method, shorty, receiver, args, float_result);
      } else {
        InterpreterJni(self, method, shorty, receiver, args, result);
      }
    }
  }
  self->PopShadowFrame();
}

JValue EnterInterpreterFromDeoptimize(Thread* self, ShadowFrame& shadow_frame, JValue ret_val)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  MethodHelper mh(shadow_frame.GetMethod());
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  return Execute(self, mh, code_item, shadow_frame, ret_val);
}

void EnterInterpreterFromLLVM(Thread* self, ShadowFrame* shadow_frame, JValue* ret_val)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  JValue value;
  MethodHelper mh(shadow_frame->GetMethod());
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  while (shadow_frame != NULL) {
    value = Execute(self, mh, code_item, *shadow_frame, value);
    ShadowFrame* old_frame = shadow_frame;
    shadow_frame = shadow_frame->GetLink();
    mh.ChangeMethod(shadow_frame->GetMethod());
    delete old_frame;
  }
  ret_val->SetJ(value.GetJ());
}

JValue EnterInterpreterFromStub(Thread* self, MethodHelper& mh, const DexFile::CodeItem* code_item,
                                ShadowFrame& shadow_frame)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return Execute(self, mh, code_item, shadow_frame, JValue());
}

}  // namespace interpreter
}  // namespace art
