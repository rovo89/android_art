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
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
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

static void UnstartedRuntimeInvoke(Thread* self, MethodHelper& mh,
                                   const DexFile::CodeItem* code_item, ShadowFrame* shadow_frame,
                                   JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // In a runtime that's not started we intercept certain methods to avoid complicated dependency
  // problems in core libraries.
  std::string name(PrettyMethod(shadow_frame->GetMethod()));
  if (name == "java.lang.Class java.lang.Class.forName(java.lang.String)") {
    std::string descriptor(DotToDescriptor(shadow_frame->GetVRegReference(arg_offset)->AsString()->ToModifiedUtf8().c_str()));
    ClassLoader* class_loader = NULL; // shadow_frame.GetMethod()->GetDeclaringClass()->GetClassLoader();
    Class* found = Runtime::Current()->GetClassLinker()->FindClass(descriptor.c_str(),
                                                                   class_loader);
    CHECK(found != NULL) << "Class.forName failed in un-started runtime for class: "
        << PrettyDescriptor(descriptor);
    result->SetL(found);
  } else if (name == "java.lang.Object java.lang.Class.newInstance()") {
    Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
    AbstractMethod* c = klass->FindDeclaredDirectMethod("<init>", "()V");
    CHECK(c != NULL);
    Object* obj = klass->AllocObject(self);
    CHECK(obj != NULL);
    EnterInterpreterFromInvoke(self, c, obj, NULL, NULL);
    result->SetL(obj);
  } else if (name == "java.lang.reflect.Field java.lang.Class.getDeclaredField(java.lang.String)") {
    // Special managed code cut-out to allow field lookup in a un-started runtime that'd fail
    // going the reflective Dex way.
    Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
    String* name = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
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
    Class* ctype = shadow_frame->GetVRegReference(arg_offset)->GetClass()->GetComponentType();
    jint srcPos = shadow_frame->GetVReg(arg_offset + 1);
    jint dstPos = shadow_frame->GetVReg(arg_offset + 3);
    jint length = shadow_frame->GetVReg(arg_offset + 4);
    if (!ctype->IsPrimitive()) {
      ObjectArray<Object>* src = shadow_frame->GetVRegReference(arg_offset)->AsObjectArray<Object>();
      ObjectArray<Object>* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsObjectArray<Object>();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else if (ctype->IsPrimitiveChar()) {
      CharArray* src = shadow_frame->GetVRegReference(arg_offset)->AsCharArray();
      CharArray* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsCharArray();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else if (ctype->IsPrimitiveInt()) {
      IntArray* src = shadow_frame->GetVRegReference(arg_offset)->AsIntArray();
      IntArray* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsIntArray();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else {
      UNIMPLEMENTED(FATAL) << "System.arraycopy of unexpected type: " << PrettyDescriptor(ctype);
    }
  } else {
    // Not special, continue with regular interpreter execution.
    artInterpreterToInterpreterEntry(self, mh, code_item, shadow_frame, result);
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
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
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
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), klass.get());
    } else if (shorty == "Z") {
      typedef jboolean (fnptr)(JNIEnv*, jclass);
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetZ(fn(soa.Env(), klass.get()));
    } else if (shorty == "BI") {
      typedef jbyte (fnptr)(JNIEnv*, jclass, jint);
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetB(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "II") {
      typedef jint (fnptr)(JNIEnv*, jclass, jint);
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetI(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "LL") {
      typedef jobject (fnptr)(JNIEnv*, jclass, jobject);
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
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
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetI(fn(soa.Env(), klass.get(), args[0], args[1]));
    } else if (shorty == "ILI") {
      typedef jint (fnptr)(JNIEnv*, jclass, jobject, jint);
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[0])));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetI(fn(soa.Env(), klass.get(), arg0.get(), args[1]));
    } else if (shorty == "SIZ") {
      typedef jshort (fnptr)(JNIEnv*, jclass, jint, jboolean);
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetS(fn(soa.Env(), klass.get(), args[0], args[1]));
    } else if (shorty == "VIZ") {
      typedef void (fnptr)(JNIEnv*, jclass, jint, jboolean);
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), klass.get(), args[0], args[1]);
    } else if (shorty == "ZLL") {
      typedef jboolean (fnptr)(JNIEnv*, jclass, jobject, jobject);
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
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
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
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
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[1])));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), klass.get(), args[0], arg1.get(), args[2], args[3]);
    } else if (shorty == "VLILII") {
      typedef void (fnptr)(JNIEnv*, jclass, jobject, jint, jobject, jint, jint);
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
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
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
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
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
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
      const fnptr* fn = reinterpret_cast<const fnptr*>(method->GetNativeMethod());
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

// TODO: should be SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) which is failing due to template
// specialization.
template<InvokeType type, bool is_range>
static void DoInvoke(Thread* self, ShadowFrame& shadow_frame,
                     const Instruction* inst, JValue* result) NO_THREAD_SAFETY_ANALYSIS;

template<InvokeType type, bool is_range>
static void DoInvoke(Thread* self, ShadowFrame& shadow_frame,
                     const Instruction* inst, JValue* result) {
  uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
  uint32_t vregC = (is_range) ? inst->VRegC_3rc() : inst->VRegC_35c();
  Object* receiver = (type == kStatic) ? NULL : shadow_frame.GetVRegReference(vregC);
  AbstractMethod* method = FindMethodFromCode(method_idx, receiver, shadow_frame.GetMethod(), self,
                                              true, type);
  if (UNLIKELY(method == NULL)) {
    CHECK(self->IsExceptionPending());
    result->SetJ(0);
    return;
  }

  MethodHelper mh(method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  uint16_t num_regs;
  uint16_t num_ins;
  if (LIKELY(code_item != NULL)) {
    num_regs = code_item->registers_size_;
    num_ins = code_item->ins_size_;
  } else if (method->IsAbstract()) {
    ThrowLocation throw_location = self->GetCurrentLocationForThrow();
    self->ThrowNewExceptionF(throw_location, "Ljava/lang/AbstractMethodError;",
                             "abstract method \"%s\"", PrettyMethod(method).c_str());
    return;
  } else {
    DCHECK(method->IsNative() || method->IsProxyMethod());
    num_regs = num_ins = AbstractMethod::NumArgRegisters(mh.GetShorty());
    if (!method->IsStatic()) {
      num_regs++;
      num_ins++;
    }
  }

  void* memory = alloca(ShadowFrame::ComputeSize(num_regs));
  ShadowFrame* new_shadow_frame(ShadowFrame::Create(num_regs, &shadow_frame, method, 0, memory));
  size_t cur_reg = num_regs - num_ins;
  if (receiver != NULL) {
    new_shadow_frame->SetVRegReference(cur_reg, receiver);
    ++cur_reg;
  }

  size_t arg_offset = (receiver == NULL) ? 0 : 1;
  const char* shorty = mh.GetShorty();
  uint32_t arg[5];
  if (!is_range) {
    inst->GetArgs(arg);
  }
  for (size_t shorty_pos = 0; cur_reg < num_regs; ++shorty_pos, cur_reg++, arg_offset++) {
    DCHECK_LT(shorty_pos + 1, mh.GetShortyLength());
    size_t arg_pos = is_range ? vregC + arg_offset : arg[arg_offset];
    switch (shorty[shorty_pos + 1]) {
      case 'L': {
        Object* o = shadow_frame.GetVRegReference(arg_pos);
        new_shadow_frame->SetVRegReference(cur_reg, o);
        break;
      }
      case 'J': case 'D': {
        uint64_t wide_value = (static_cast<uint64_t>(shadow_frame.GetVReg(arg_pos + 1)) << 32) |
                              static_cast<uint32_t>(shadow_frame.GetVReg(arg_pos));
        new_shadow_frame->SetVRegLong(cur_reg, wide_value);
        cur_reg++;
        arg_offset++;
        break;
      }
      default:
        new_shadow_frame->SetVReg(cur_reg, shadow_frame.GetVReg(arg_pos));
        break;
    }
  }

  if (LIKELY(Runtime::Current()->IsStarted())) {
    (method->GetEntryPointFromInterpreter())(self, mh, code_item, new_shadow_frame, result);
  } else {
    UnstartedRuntimeInvoke(self, mh, code_item, new_shadow_frame, result, num_regs - num_ins);
  }
}

// We use template functions to optimize compiler inlining process. Otherwise,
// some parts of the code (like a switch statement) which depend on a constant
// parameter would not be inlined while it should be. These constant parameters
// are now part of the template arguments.
// Note these template functions are static and inlined so they should not be
// part of the final object file.
// TODO: should be SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) which is failing due to template
// specialization.
template<FindFieldType find_type, Primitive::Type field_type>
static void DoFieldGet(Thread* self, ShadowFrame& shadow_frame,
                       const Instruction* inst)
    NO_THREAD_SAFETY_ANALYSIS ALWAYS_INLINE;

template<FindFieldType find_type, Primitive::Type field_type>
static inline void DoFieldGet(Thread* self, ShadowFrame& shadow_frame,
                              const Instruction* inst) {
  bool is_static = (find_type == StaticObjectRead) || (find_type == StaticPrimitiveRead);
  uint32_t field_idx = is_static ? inst->VRegB_21c() : inst->VRegC_22c();
  Field* f = FindFieldFromCode(field_idx, shadow_frame.GetMethod(), self,
                               find_type, Primitive::FieldSize(field_type));
  if (UNLIKELY(f == NULL)) {
    CHECK(self->IsExceptionPending());
    return;
  }
  Object* obj;
  if (is_static) {
    obj = f->GetDeclaringClass();
  } else {
    obj = shadow_frame.GetVRegReference(inst->VRegB_22c());
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(shadow_frame.GetCurrentLocationForThrow(), f, true);
      return;
    }
  }
  uint32_t vregA = is_static ? inst->VRegA_21c() : inst->VRegA_22c();
  switch (field_type) {
    case Primitive::kPrimBoolean:
      shadow_frame.SetVReg(vregA, f->GetBoolean(obj));
      break;
    case Primitive::kPrimByte:
      shadow_frame.SetVReg(vregA, f->GetByte(obj));
      break;
    case Primitive::kPrimChar:
      shadow_frame.SetVReg(vregA, f->GetChar(obj));
      break;
    case Primitive::kPrimShort:
      shadow_frame.SetVReg(vregA, f->GetShort(obj));
      break;
    case Primitive::kPrimInt:
      shadow_frame.SetVReg(vregA, f->GetInt(obj));
      break;
    case Primitive::kPrimLong:
      shadow_frame.SetVRegLong(vregA, f->GetLong(obj));
      break;
    case Primitive::kPrimNot:
      shadow_frame.SetVRegReference(vregA, f->GetObject(obj));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
  }
}

// TODO: should be SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) which is failing due to template
// specialization.
template<FindFieldType find_type, Primitive::Type field_type>
static void DoFieldPut(Thread* self, const ShadowFrame& shadow_frame,
                       const Instruction* inst)
    NO_THREAD_SAFETY_ANALYSIS ALWAYS_INLINE;

template<FindFieldType find_type, Primitive::Type field_type>
static inline void DoFieldPut(Thread* self, const ShadowFrame& shadow_frame,
                              const Instruction* inst) {
  bool is_static = (find_type == StaticObjectWrite) || (find_type == StaticPrimitiveWrite);
  uint32_t field_idx = is_static ? inst->VRegB_21c() : inst->VRegC_22c();
  Field* f = FindFieldFromCode(field_idx, shadow_frame.GetMethod(), self,
                               find_type, Primitive::FieldSize(field_type));
  if (UNLIKELY(f == NULL)) {
    CHECK(self->IsExceptionPending());
    return;
  }
  Object* obj;
  if (is_static) {
    obj = f->GetDeclaringClass();
  } else {
    obj = shadow_frame.GetVRegReference(inst->VRegB_22c());
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(shadow_frame.GetCurrentLocationForThrow(),
                                              f, false);
      return;
    }
  }
  uint32_t vregA = is_static ? inst->VRegA_21c() : inst->VRegA_22c();
  switch (field_type) {
    case Primitive::kPrimBoolean:
      f->SetBoolean(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimByte:
      f->SetByte(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimChar:
      f->SetChar(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimShort:
      f->SetShort(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimInt:
      f->SetInt(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimLong:
      f->SetLong(obj, shadow_frame.GetVRegLong(vregA));
      break;
    case Primitive::kPrimNot:
      f->SetObj(obj, shadow_frame.GetVRegReference(vregA));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
  }
}

static inline String* ResolveString(Thread* self, MethodHelper& mh, uint32_t string_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Class* java_lang_string_class = String::GetJavaLangString();
  if (UNLIKELY(!java_lang_string_class->IsInitialized())) {
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    if (UNLIKELY(!class_linker->EnsureInitialized(java_lang_string_class,
                                                  true, true))) {
      DCHECK(self->IsExceptionPending());
      return NULL;
    }
  }
  return mh.ResolveString(string_idx);
}

static inline void DoIntDivide(Thread* self, ShadowFrame& shadow_frame, size_t result_reg,
    int32_t dividend, int32_t divisor) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero(self);
  } else if (UNLIKELY(dividend == kMinInt && divisor == -1)) {
    shadow_frame.SetVReg(result_reg, kMinInt);
  } else {
    shadow_frame.SetVReg(result_reg, dividend / divisor);
  }
}

static inline void DoIntRemainder(Thread* self, ShadowFrame& shadow_frame, size_t result_reg,
    int32_t dividend, int32_t divisor) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero(self);
  } else if (UNLIKELY(dividend == kMinInt && divisor == -1)) {
    shadow_frame.SetVReg(result_reg, 0);
  } else {
    shadow_frame.SetVReg(result_reg, dividend % divisor);
  }
}

static inline void DoLongDivide(Thread* self, ShadowFrame& shadow_frame, size_t result_reg,
    int64_t dividend, int64_t divisor) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero(self);
  } else if (UNLIKELY(dividend == kMinLong && divisor == -1)) {
    shadow_frame.SetVRegLong(result_reg, kMinLong);
  } else {
    shadow_frame.SetVRegLong(result_reg, dividend / divisor);
  }
}

static inline void DoLongRemainder(Thread* self, ShadowFrame& shadow_frame, size_t result_reg,
    int64_t dividend, int64_t divisor) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero(self);
  } else if (UNLIKELY(dividend == kMinLong && divisor == -1)) {
    shadow_frame.SetVRegLong(result_reg, 0);
  } else {
    shadow_frame.SetVRegLong(result_reg, dividend % divisor);
  }
}

static inline const Instruction* FindNextInstructionFollowingException(Thread* self,
                                                                       ShadowFrame& shadow_frame,
                                                                       uint32_t dex_pc,
                                                                       const uint16_t* insns,
                                                                       SirtRef<Object>& this_object_ref,
                                                                       instrumentation::Instrumentation* instrumentation)
    ALWAYS_INLINE;

static inline const Instruction* FindNextInstructionFollowingException(Thread* self,
                                                                       ShadowFrame& shadow_frame,
                                                                       uint32_t dex_pc,
                                                                       const uint16_t* insns,
                                                                       SirtRef<Object>& this_object_ref,
                                                                       instrumentation::Instrumentation* instrumentation)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  self->VerifyStack();
  ThrowLocation throw_location;
  mirror::Throwable* exception = self->GetException(&throw_location);
  uint32_t found_dex_pc = shadow_frame.GetMethod()->FindCatchBlock(exception->GetClass(), dex_pc);
  if (found_dex_pc == DexFile::kDexNoIndex) {
    instrumentation->MethodUnwindEvent(self, this_object_ref.get(),
                                       shadow_frame.GetMethod(), dex_pc);
    return NULL;
  } else {
    instrumentation->ExceptionCaughtEvent(self, throw_location,
                                          shadow_frame.GetMethod(),
                                          found_dex_pc, exception);
    return Instruction::At(insns + found_dex_pc);
  }
}

#define HANDLE_PENDING_EXCEPTION() \
  CHECK(self->IsExceptionPending()); \
  inst = FindNextInstructionFollowingException(self, shadow_frame, inst->GetDexPc(insns), insns, \
                                               this_object_ref, instrumentation); \
  if (inst == NULL) { \
    return JValue(); /* Handled in caller. */ \
  }

#define POSSIBLY_HANDLE_PENDING_EXCEPTION(next_function) \
  if (UNLIKELY(self->IsExceptionPending())) { \
    inst = FindNextInstructionFollowingException(self, shadow_frame, inst->GetDexPc(insns), insns, \
                                                 this_object_ref, instrumentation); \
    if (inst == NULL) { \
      return JValue(); /* Handled in caller. */ \
    } \
  } else { \
    inst = inst-> next_function (); \
  }

static void UnexpectedOpcode(const Instruction* inst, MethodHelper& mh)
  __attribute__ ((cold, noreturn, noinline));

static void UnexpectedOpcode(const Instruction* inst, MethodHelper& mh)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  LOG(FATAL) << "Unexpected instruction: " << inst->DumpString(&mh.GetDexFile());
  exit(0);  // Unreachable, keep GCC happy.
}

// Code to run before each dex instruction.
#define PREAMBLE()

static JValue Execute(Thread* self, MethodHelper& mh, const DexFile::CodeItem* code_item,
                      ShadowFrame& shadow_frame, JValue result_register)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) __attribute__ ((hot));

static JValue Execute(Thread* self, MethodHelper& mh, const DexFile::CodeItem* code_item,
                      ShadowFrame& shadow_frame, JValue result_register) {
  if (UNLIKELY(!shadow_frame.HasReferenceArray())) {
    LOG(FATAL) << "Invalid shadow frame for interpreter use";
    return JValue();
  }
  self->VerifyStack();
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  const uint16_t* const insns = code_item->insns_;

  // As the 'this' object won't change during the execution of current code, we
  // want to cache it in local variables. Nevertheless, in order to let the
  // garbage collector access it, we store it into sirt references.
  SirtRef<Object> this_object_ref(self, shadow_frame.GetThisObject(code_item->ins_size_));

  const Instruction* inst = Instruction::At(insns + shadow_frame.GetDexPC());
  if (inst->GetDexPc(insns) == 0) {  // We are entering the method as opposed to deoptimizing..
    if (UNLIKELY(instrumentation->HasMethodEntryListeners())) {
      instrumentation->MethodEnterEvent(self, this_object_ref.get(),
                                        shadow_frame.GetMethod(), 0);
    }
  }
  while (true) {
    if (UNLIKELY(self->TestAllFlags())) {
      CheckSuspend(self);
    }
    const uint32_t dex_pc = inst->GetDexPc(insns);
    shadow_frame.SetDexPC(dex_pc);
    if (instrumentation->HasDexPcListeners()) {
      instrumentation->DexPcMovedEvent(self, this_object_ref.get(),
                                       shadow_frame.GetMethod(), dex_pc);
    }
    const bool kTracing = false;
    if (kTracing) {
#define TRACE_LOG std::cerr
      TRACE_LOG << PrettyMethod(shadow_frame.GetMethod())
                << StringPrintf("\n0x%x: ", dex_pc)
                << inst->DumpString(&mh.GetDexFile()) << "\n";
      for (size_t i = 0; i < shadow_frame.NumberOfVRegs(); ++i) {
        uint32_t raw_value = shadow_frame.GetVReg(i);
        Object* ref_value = shadow_frame.GetVRegReference(i);
        TRACE_LOG << StringPrintf(" vreg%d=0x%08X", i, raw_value);
        if (ref_value != NULL) {
          if (ref_value->GetClass()->IsStringClass() &&
              ref_value->AsString()->GetCharArray() != NULL) {
            TRACE_LOG << "/java.lang.String \"" << ref_value->AsString()->ToModifiedUtf8() << "\"";
          } else {
            TRACE_LOG << "/" << PrettyTypeOf(ref_value);
          }
        }
      }
      TRACE_LOG << "\n";
#undef TRACE_LOG
    }
    switch (inst->Opcode()) {
      case Instruction::NOP:
        PREAMBLE();
        inst = inst->Next_1xx();
        break;
      case Instruction::MOVE:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_12x(),
                             shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::MOVE_FROM16:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22x(),
                             shadow_frame.GetVReg(inst->VRegB_22x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::MOVE_16:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_32x(),
                             shadow_frame.GetVReg(inst->VRegB_32x()));
        inst = inst->Next_3xx();
        break;
      case Instruction::MOVE_WIDE:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_12x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::MOVE_WIDE_FROM16:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_22x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_22x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::MOVE_WIDE_16:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_32x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_32x()));
        inst = inst->Next_3xx();
        break;
      case Instruction::MOVE_OBJECT:
        PREAMBLE();
        shadow_frame.SetVRegReference(inst->VRegA_12x(),
                                      shadow_frame.GetVRegReference(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::MOVE_OBJECT_FROM16:
        PREAMBLE();
        shadow_frame.SetVRegReference(inst->VRegA_22x(),
                                      shadow_frame.GetVRegReference(inst->VRegB_22x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::MOVE_OBJECT_16:
        PREAMBLE();
        shadow_frame.SetVRegReference(inst->VRegA_32x(),
                                      shadow_frame.GetVRegReference(inst->VRegB_32x()));
        inst = inst->Next_3xx();
        break;
      case Instruction::MOVE_RESULT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_11x(), result_register.GetI());
        inst = inst->Next_1xx();
        break;
      case Instruction::MOVE_RESULT_WIDE:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_11x(), result_register.GetJ());
        inst = inst->Next_1xx();
        break;
      case Instruction::MOVE_RESULT_OBJECT:
        PREAMBLE();
        shadow_frame.SetVRegReference(inst->VRegA_11x(), result_register.GetL());
        inst = inst->Next_1xx();
        break;
      case Instruction::MOVE_EXCEPTION: {
        PREAMBLE();
        Throwable* exception = self->GetException(NULL);
        self->ClearException();
        shadow_frame.SetVRegReference(inst->VRegA_11x(), exception);
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::RETURN_VOID: {
        PREAMBLE();
        JValue result;
        if (UNLIKELY(instrumentation->HasMethodExitListeners())) {
          instrumentation->MethodExitEvent(self, this_object_ref.get(),
                                           shadow_frame.GetMethod(), inst->GetDexPc(insns),
                                           result);
        }
        return result;
      }
      case Instruction::RETURN: {
        PREAMBLE();
        JValue result;
        result.SetJ(0);
        result.SetI(shadow_frame.GetVReg(inst->VRegA_11x()));
        if (UNLIKELY(instrumentation->HasMethodExitListeners())) {
          instrumentation->MethodExitEvent(self, this_object_ref.get(),
                                           shadow_frame.GetMethod(), inst->GetDexPc(insns),
                                           result);
        }
        return result;
      }
      case Instruction::RETURN_WIDE: {
        PREAMBLE();
        JValue result;
        result.SetJ(shadow_frame.GetVRegLong(inst->VRegA_11x()));
        if (UNLIKELY(instrumentation->HasMethodExitListeners())) {
          instrumentation->MethodExitEvent(self, this_object_ref.get(),
                                           shadow_frame.GetMethod(), inst->GetDexPc(insns),
                                           result);
        }
        return result;
      }
      case Instruction::RETURN_OBJECT: {
        PREAMBLE();
        JValue result;
        result.SetJ(0);
        result.SetL(shadow_frame.GetVRegReference(inst->VRegA_11x()));
        if (UNLIKELY(instrumentation->HasMethodExitListeners())) {
          instrumentation->MethodExitEvent(self, this_object_ref.get(),
                                           shadow_frame.GetMethod(), inst->GetDexPc(insns),
                                           result);
        }
        return result;
      }
      case Instruction::CONST_4: {
        PREAMBLE();
        uint32_t dst = inst->VRegA_11n();
        int32_t val = static_cast<int32_t>(inst->VRegB_11n() << 28) >> 28;
        shadow_frame.SetVReg(dst, val);
        if (val == 0) {
          shadow_frame.SetVRegReference(dst, NULL);
        }
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::CONST_16: {
        PREAMBLE();
        uint32_t dst = inst->VRegA_21s();
        int32_t val = static_cast<int16_t>(inst->VRegB_21s());
        shadow_frame.SetVReg(dst, val);
        if (val == 0) {
          shadow_frame.SetVRegReference(dst, NULL);
        }
        inst = inst->Next_2xx();
        break;
      }
      case Instruction::CONST: {
        PREAMBLE();
        uint32_t dst = inst->VRegA_31i();
        int32_t val = inst->VRegB_31i();
        shadow_frame.SetVReg(dst, val);
        if (val == 0) {
          shadow_frame.SetVRegReference(dst, NULL);
        }
        inst = inst->Next_3xx();
        break;
      }
      case Instruction::CONST_HIGH16: {
        PREAMBLE();
        uint32_t dst = inst->VRegA_21h();
        int32_t val = inst->VRegB_21h() << 16;
        shadow_frame.SetVReg(dst, val);
        if (val == 0) {
          shadow_frame.SetVRegReference(dst, NULL);
        }
        inst = inst->Next_2xx();
        break;
      }
      case Instruction::CONST_WIDE_16:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_21s(),
                                 static_cast<int16_t>(inst->VRegB_21s()));
        inst = inst->Next_2xx();
        break;
      case Instruction::CONST_WIDE_32:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_31i(),
                                 static_cast<int32_t>(inst->VRegB_31i()));
        inst = inst->Next_3xx();
        break;
      case Instruction::CONST_WIDE:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_51l(), inst->VRegB_51l());
        inst = inst->Next_51l();
        break;
      case Instruction::CONST_WIDE_HIGH16:
        shadow_frame.SetVRegLong(inst->VRegA_21h(),
                                 static_cast<uint64_t>(inst->VRegB_21h()) << 48);
        inst = inst->Next_2xx();
        break;
      case Instruction::CONST_STRING: {
        PREAMBLE();
        String* s = ResolveString(self, mh,  inst->VRegB_21c());
        if (UNLIKELY(s == NULL)) {
          HANDLE_PENDING_EXCEPTION();
        } else {
          shadow_frame.SetVRegReference( inst->VRegA_21c(), s);
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::CONST_STRING_JUMBO: {
        PREAMBLE();
        String* s = ResolveString(self, mh,  inst->VRegB_31c());
        if (UNLIKELY(s == NULL)) {
          HANDLE_PENDING_EXCEPTION();
        } else {
          shadow_frame.SetVRegReference( inst->VRegA_31c(), s);
          inst = inst->Next_3xx();
        }
        break;
      }
      case Instruction::CONST_CLASS: {
        PREAMBLE();
        Class* c = ResolveVerifyAndClinit(inst->VRegB_21c(), shadow_frame.GetMethod(),
                                          self, false, true);
        if (UNLIKELY(c == NULL)) {
          HANDLE_PENDING_EXCEPTION();
        } else {
          shadow_frame.SetVRegReference(inst->VRegA_21c(), c);
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::MONITOR_ENTER: {
        PREAMBLE();
        Object* obj = shadow_frame.GetVRegReference(inst->VRegA_11x());
        if (UNLIKELY(obj == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
        } else {
          DoMonitorEnter(self, obj);
          POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_1xx);
        }
        break;
      }
      case Instruction::MONITOR_EXIT: {
        PREAMBLE();
        Object* obj = shadow_frame.GetVRegReference(inst->VRegA_11x());
        if (UNLIKELY(obj == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
        } else {
          DoMonitorExit(self, obj);
          POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_1xx);
        }
        break;
      }
      case Instruction::CHECK_CAST: {
        PREAMBLE();
        Class* c = ResolveVerifyAndClinit(inst->VRegB_21c(), shadow_frame.GetMethod(),
                                          self, false, true);
        if (UNLIKELY(c == NULL)) {
          HANDLE_PENDING_EXCEPTION();
        } else {
          Object* obj = shadow_frame.GetVRegReference(inst->VRegA_21c());
          if (UNLIKELY(obj != NULL && !obj->InstanceOf(c))) {
            ThrowClassCastException(c, obj->GetClass());
            HANDLE_PENDING_EXCEPTION();
          } else {
            inst = inst->Next_2xx();
          }
        }
        break;
      }
      case Instruction::INSTANCE_OF: {
        PREAMBLE();
        Class* c = ResolveVerifyAndClinit(inst->VRegC_22c(), shadow_frame.GetMethod(),
                                          self, false, true);
        if (UNLIKELY(c == NULL)) {
          HANDLE_PENDING_EXCEPTION();
        } else {
          Object* obj = shadow_frame.GetVRegReference(inst->VRegB_22c());
          shadow_frame.SetVReg(inst->VRegA_22c(), (obj != NULL && obj->InstanceOf(c)) ? 1 : 0);
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::ARRAY_LENGTH:  {
        PREAMBLE();
        Object* array = shadow_frame.GetVRegReference(inst->VRegB_12x());
        if (UNLIKELY(array == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        shadow_frame.SetVReg(inst->VRegA_12x(), array->AsArray()->GetLength());
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::NEW_INSTANCE: {
        PREAMBLE();
        Object* obj = AllocObjectFromCode(inst->VRegB_21c(), shadow_frame.GetMethod(),
                                          self, true);
        if (UNLIKELY(obj == NULL)) {
          HANDLE_PENDING_EXCEPTION();
        } else {
          shadow_frame.SetVRegReference(inst->VRegA_21c(), obj);
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::NEW_ARRAY: {
        PREAMBLE();
        int32_t length = shadow_frame.GetVReg(inst->VRegB_22c());
        Object* obj = AllocArrayFromCode(inst->VRegC_22c(), shadow_frame.GetMethod(),
                                         length, self, true);
        if (UNLIKELY(obj == NULL)) {
          HANDLE_PENDING_EXCEPTION();
        } else {
          shadow_frame.SetVRegReference(inst->VRegA_22c(), obj);
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::FILLED_NEW_ARRAY: {
        PREAMBLE();
        const int32_t length = inst->VRegA_35c();
        CHECK(length <= 5);
        if (UNLIKELY(length < 0)) {
          ThrowNegativeArraySizeException(length);
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        Class* arrayClass = ResolveVerifyAndClinit(inst->VRegB_35c(), shadow_frame.GetMethod(),
                                                   self, false, true);
        if (UNLIKELY(arrayClass == NULL)) {
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        CHECK(arrayClass->IsArrayClass());
        Class* componentClass = arrayClass->GetComponentType();
        if (UNLIKELY(componentClass->IsPrimitive() && !componentClass->IsPrimitiveInt())) {
          if (componentClass->IsPrimitiveLong() || componentClass->IsPrimitiveDouble()) {
            ThrowRuntimeException("Bad filled array request for type %s",
                                  PrettyDescriptor(componentClass).c_str());
          } else {
            self->ThrowNewExceptionF(shadow_frame.GetCurrentLocationForThrow(),
                                     "Ljava/lang/InternalError;",
                                     "Found type %s; filled-new-array not implemented for anything but \'int\'",
                                     PrettyDescriptor(componentClass).c_str());
          }
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        Object* newArray = Array::Alloc(self, arrayClass, length);
        if (UNLIKELY(newArray == NULL)) {
          HANDLE_PENDING_EXCEPTION();
        } else {
          uint32_t arg[5];
          inst->GetArgs(arg);
          const bool is_primitive_int_component = componentClass->IsPrimitiveInt();
          for (int32_t i = 0; i < length; ++i) {
            if (is_primitive_int_component) {
              newArray->AsIntArray()->Set(i, shadow_frame.GetVReg(arg[i]));
            } else {
              newArray->AsObjectArray<Object>()->Set(i, shadow_frame.GetVRegReference(arg[i]));
            }
          }
          result_register.SetL(newArray);
          inst = inst->Next_3xx();
        }
        break;
      }
      case Instruction::FILLED_NEW_ARRAY_RANGE: {
        PREAMBLE();
        int32_t length = inst->VRegA_3rc();
        if (UNLIKELY(length < 0)) {
          ThrowNegativeArraySizeException(length);
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        Class* arrayClass = ResolveVerifyAndClinit(inst->VRegB_3rc(), shadow_frame.GetMethod(),
                                                   self, false, true);
        if (UNLIKELY(arrayClass == NULL)) {
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        CHECK(arrayClass->IsArrayClass());
        Class* componentClass = arrayClass->GetComponentType();
        if (UNLIKELY(componentClass->IsPrimitive() && !componentClass->IsPrimitiveInt())) {
          if (componentClass->IsPrimitiveLong() || componentClass->IsPrimitiveDouble()) {
            ThrowRuntimeException("Bad filled array request for type %s",
                                  PrettyDescriptor(componentClass).c_str());
          } else {
            self->ThrowNewExceptionF(shadow_frame.GetCurrentLocationForThrow(),
                                     "Ljava/lang/InternalError;",
                                     "Found type %s; filled-new-array not implemented for anything but \'int\'",
                                     PrettyDescriptor(componentClass).c_str());
          }
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        Object* newArray = Array::Alloc(self, arrayClass, length);
        if (UNLIKELY(newArray == NULL)) {
          HANDLE_PENDING_EXCEPTION();
        } else {
          uint32_t vregC = inst->VRegC_3rc();
          const bool is_primitive_int_component = componentClass->IsPrimitiveInt();
          for (int32_t i = 0; i < length; ++i) {
            if (is_primitive_int_component) {
              newArray->AsIntArray()->Set(i, shadow_frame.GetVReg(vregC + i));
            } else {
              newArray->AsObjectArray<Object>()->Set(i, shadow_frame.GetVRegReference(vregC + i));
            }
          }
          result_register.SetL(newArray);
          inst = inst->Next_3xx();
        }
        break;
      }
      case Instruction::FILL_ARRAY_DATA: {
        PREAMBLE();
        Object* obj = shadow_frame.GetVRegReference(inst->VRegA_31t());
        if (UNLIKELY(obj == NULL)) {
          ThrowNullPointerException(NULL, "null array in FILL_ARRAY_DATA");
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        Array* array = obj->AsArray();
        DCHECK(array->IsArrayInstance() && !array->IsObjectArray());
        const Instruction::ArrayDataPayload* payload =
            reinterpret_cast<const Instruction::ArrayDataPayload*>(insns + inst->GetDexPc(insns) + inst->VRegB_31t());
        if (UNLIKELY(static_cast<int32_t>(payload->element_count) > array->GetLength())) {
          self->ThrowNewExceptionF(shadow_frame.GetCurrentLocationForThrow(),
                                   "Ljava/lang/ArrayIndexOutOfBoundsException;",
                                   "failed FILL_ARRAY_DATA; length=%d, index=%d",
                                   array->GetLength(), payload->element_count);
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        uint32_t size_in_bytes = payload->element_count * payload->element_width;
        memcpy(array->GetRawData(payload->element_width), payload->data, size_in_bytes);
        inst = inst->Next_3xx();
        break;
      }
      case Instruction::THROW: {
        PREAMBLE();
        Object* exception = shadow_frame.GetVRegReference(inst->VRegA_11x());
        if (UNLIKELY(exception == NULL)) {
          ThrowNullPointerException(NULL, "throw with null exception");
        } else {
          self->SetException(shadow_frame.GetCurrentLocationForThrow(), exception->AsThrowable());
        }
        HANDLE_PENDING_EXCEPTION();
        break;
      }
      case Instruction::GOTO: {
        PREAMBLE();
        inst = inst->RelativeAt(inst->VRegA_10t());
        break;
      }
      case Instruction::GOTO_16: {
        PREAMBLE();
        inst = inst->RelativeAt(inst->VRegA_20t());
        break;
      }
      case Instruction::GOTO_32: {
        PREAMBLE();
        inst = inst->RelativeAt(inst->VRegA_30t());
        break;
      }
      case Instruction::PACKED_SWITCH: {
        PREAMBLE();
        const uint16_t* switch_data = reinterpret_cast<const uint16_t*>(inst) + inst->VRegB_31t();
        int32_t test_val = shadow_frame.GetVReg(inst->VRegA_31t());
        DCHECK_EQ(switch_data[0], static_cast<uint16_t>(Instruction::kPackedSwitchSignature));
        uint16_t size = switch_data[1];
        DCHECK_GT(size, 0);
        const int32_t* keys = reinterpret_cast<const int32_t*>(&switch_data[2]);
        DCHECK(IsAligned<4>(keys));
        int32_t first_key = keys[0];
        const int32_t* targets = reinterpret_cast<const int32_t*>(&switch_data[4]);
        DCHECK(IsAligned<4>(targets));
        int32_t index = test_val - first_key;
        if (index >= 0 && index < size) {
          inst = inst->RelativeAt(targets[index]);
        } else {
          inst = inst->Next_3xx();
        }
        break;
      }
      case Instruction::SPARSE_SWITCH: {
        PREAMBLE();
        const uint16_t* switch_data = reinterpret_cast<const uint16_t*>(inst) + inst->VRegB_31t();
        int32_t test_val = shadow_frame.GetVReg(inst->VRegA_31t());
        DCHECK_EQ(switch_data[0], static_cast<uint16_t>(Instruction::kSparseSwitchSignature));
        uint16_t size = switch_data[1];
        DCHECK_GT(size, 0);
        const int32_t* keys = reinterpret_cast<const int32_t*>(&switch_data[2]);
        DCHECK(IsAligned<4>(keys));
        const int32_t* entries = keys + size;
        DCHECK(IsAligned<4>(entries));
        int lo = 0;
        int hi = size - 1;
        const Instruction* current_inst = inst;
        inst = inst->Next_3xx();
        while (lo <= hi) {
          int mid = (lo + hi) / 2;
          int32_t foundVal = keys[mid];
          if (test_val < foundVal) {
            hi = mid - 1;
          } else if (test_val > foundVal) {
            lo = mid + 1;
          } else {
            inst = current_inst->RelativeAt(entries[mid]);
            break;
          }
        }
        break;
      }
      case Instruction::CMPL_FLOAT: {
        PREAMBLE();
        float val1 = shadow_frame.GetVRegFloat(inst->VRegB_23x());
        float val2 = shadow_frame.GetVRegFloat(inst->VRegC_23x());
        int32_t result;
        if (val1 > val2) {
          result = 1;
        } else if (val1 == val2) {
          result = 0;
        } else {
          result = -1;
        }
        shadow_frame.SetVReg(inst->VRegA_23x(), result);
        inst = inst->Next_2xx();
        break;
      }
      case Instruction::CMPG_FLOAT: {
        PREAMBLE();
        float val1 = shadow_frame.GetVRegFloat(inst->VRegB_23x());
        float val2 = shadow_frame.GetVRegFloat(inst->VRegC_23x());
        int32_t result;
        if (val1 < val2) {
          result = -1;
        } else if (val1 == val2) {
          result = 0;
        } else {
          result = 1;
        }
        shadow_frame.SetVReg(inst->VRegA_23x(), result);
        inst = inst->Next_2xx();
        break;
      }
      case Instruction::CMPL_DOUBLE: {
        PREAMBLE();
        double val1 = shadow_frame.GetVRegDouble(inst->VRegB_23x());
        double val2 = shadow_frame.GetVRegDouble(inst->VRegC_23x());
        int32_t result;
        if (val1 > val2) {
          result = 1;
        } else if (val1 == val2) {
          result = 0;
        } else {
          result = -1;
        }
        shadow_frame.SetVReg(inst->VRegA_23x(), result);
        inst = inst->Next_2xx();
        break;
      }

      case Instruction::CMPG_DOUBLE: {
        PREAMBLE();
        double val1 = shadow_frame.GetVRegDouble(inst->VRegB_23x());
        double val2 = shadow_frame.GetVRegDouble(inst->VRegC_23x());
        int32_t result;
        if (val1 < val2) {
          result = -1;
        } else if (val1 == val2) {
          result = 0;
        } else {
          result = 1;
        }
        shadow_frame.SetVReg(inst->VRegA_23x(), result);
        inst = inst->Next_2xx();
        break;
      }
      case Instruction::CMP_LONG: {
        PREAMBLE();
        int64_t val1 = shadow_frame.GetVRegLong(inst->VRegB_23x());
        int64_t val2 = shadow_frame.GetVRegLong(inst->VRegC_23x());
        int32_t result;
        if (val1 > val2) {
          result = 1;
        } else if (val1 == val2) {
          result = 0;
        } else {
          result = -1;
        }
        shadow_frame.SetVReg(inst->VRegA_23x(), result);
        inst = inst->Next_2xx();
        break;
      }
      case Instruction::IF_EQ: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_22t()) == shadow_frame.GetVReg(inst->VRegB_22t())) {
          inst = inst->RelativeAt(inst->VRegC_22t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_NE: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_22t()) != shadow_frame.GetVReg(inst->VRegB_22t())) {
          inst = inst->RelativeAt(inst->VRegC_22t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_LT: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_22t()) < shadow_frame.GetVReg(inst->VRegB_22t())) {
          inst = inst->RelativeAt(inst->VRegC_22t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_GE: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_22t()) >= shadow_frame.GetVReg(inst->VRegB_22t())) {
          inst = inst->RelativeAt(inst->VRegC_22t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_GT: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_22t()) > shadow_frame.GetVReg(inst->VRegB_22t())) {
          inst = inst->RelativeAt(inst->VRegC_22t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_LE: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_22t()) <= shadow_frame.GetVReg(inst->VRegB_22t())) {
          inst = inst->RelativeAt(inst->VRegC_22t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_EQZ: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_21t()) == 0) {
          inst = inst->RelativeAt(inst->VRegB_21t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_NEZ: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_21t()) != 0) {
          inst = inst->RelativeAt(inst->VRegB_21t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_LTZ: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_21t()) < 0) {
          inst = inst->RelativeAt(inst->VRegB_21t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_GEZ: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_21t()) >= 0) {
          inst = inst->RelativeAt(inst->VRegB_21t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_GTZ: {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_21t()) > 0) {
          inst = inst->RelativeAt(inst->VRegB_21t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::IF_LEZ:  {
        PREAMBLE();
        if (shadow_frame.GetVReg(inst->VRegA_21t()) <= 0) {
          inst = inst->RelativeAt(inst->VRegB_21t());
        } else {
          inst = inst->Next_2xx();
        }
        break;
      }
      case Instruction::AGET_BOOLEAN: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        BooleanArray* array = a->AsBooleanArray();
        if (LIKELY(array->IsValidIndex(index))) {
          shadow_frame.SetVReg(inst->VRegA_23x(), array->GetData()[index]);
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::AGET_BYTE: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        ByteArray* array = a->AsByteArray();
        if (LIKELY(array->IsValidIndex(index))) {
          shadow_frame.SetVReg(inst->VRegA_23x(), array->GetData()[index]);
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::AGET_CHAR: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        CharArray* array = a->AsCharArray();
        if (LIKELY(array->IsValidIndex(index))) {
          shadow_frame.SetVReg(inst->VRegA_23x(), array->GetData()[index]);
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::AGET_SHORT: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        ShortArray* array = a->AsShortArray();
        if (LIKELY(array->IsValidIndex(index))) {
          shadow_frame.SetVReg(inst->VRegA_23x(), array->GetData()[index]);
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::AGET: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        IntArray* array = a->AsIntArray();
        if (LIKELY(array->IsValidIndex(index))) {
          shadow_frame.SetVReg(inst->VRegA_23x(), array->GetData()[index]);
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::AGET_WIDE:  {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        LongArray* array = a->AsLongArray();
        if (LIKELY(array->IsValidIndex(index))) {
          shadow_frame.SetVRegLong(inst->VRegA_23x(), array->GetData()[index]);
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::AGET_OBJECT: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        ObjectArray<Object>* array = a->AsObjectArray<Object>();
        if (LIKELY(array->IsValidIndex(index))) {
          shadow_frame.SetVRegReference(inst->VRegA_23x(), array->GetWithoutChecks(index));
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::APUT_BOOLEAN: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        uint8_t val = shadow_frame.GetVReg(inst->VRegA_23x());
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        BooleanArray* array = a->AsBooleanArray();
        if (LIKELY(array->IsValidIndex(index))) {
          array->GetData()[index] = val;
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::APUT_BYTE: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int8_t val = shadow_frame.GetVReg(inst->VRegA_23x());
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        ByteArray* array = a->AsByteArray();
        if (LIKELY(array->IsValidIndex(index))) {
          array->GetData()[index] = val;
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::APUT_CHAR: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        uint16_t val = shadow_frame.GetVReg(inst->VRegA_23x());
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        CharArray* array = a->AsCharArray();
        if (LIKELY(array->IsValidIndex(index))) {
          array->GetData()[index] = val;
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::APUT_SHORT: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int16_t val = shadow_frame.GetVReg(inst->VRegA_23x());
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        ShortArray* array = a->AsShortArray();
        if (LIKELY(array->IsValidIndex(index))) {
          array->GetData()[index] = val;
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::APUT: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int32_t val = shadow_frame.GetVReg(inst->VRegA_23x());
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        IntArray* array = a->AsIntArray();
        if (LIKELY(array->IsValidIndex(index))) {
          array->GetData()[index] = val;
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::APUT_WIDE: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int64_t val = shadow_frame.GetVRegLong(inst->VRegA_23x());
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        LongArray* array = a->AsLongArray();
        if (LIKELY(array->IsValidIndex(index))) {
          array->GetData()[index] = val;
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::APUT_OBJECT: {
        PREAMBLE();
        Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
          HANDLE_PENDING_EXCEPTION();
          break;
        }
        int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
        Object* val = shadow_frame.GetVRegReference(inst->VRegA_23x());
        ObjectArray<Object>* array = a->AsObjectArray<Object>();
        if (LIKELY(array->IsValidIndex(index) && array->CheckAssignable(val))) {
          array->SetWithoutChecks(index, val);
          inst = inst->Next_2xx();
        } else {
          HANDLE_PENDING_EXCEPTION();
        }
        break;
      }
      case Instruction::IGET_BOOLEAN:
        PREAMBLE();
        DoFieldGet<InstancePrimitiveRead, Primitive::kPrimBoolean>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IGET_BYTE:
        PREAMBLE();
        DoFieldGet<InstancePrimitiveRead, Primitive::kPrimByte>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IGET_CHAR:
        PREAMBLE();
        DoFieldGet<InstancePrimitiveRead, Primitive::kPrimChar>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IGET_SHORT:
        PREAMBLE();
        DoFieldGet<InstancePrimitiveRead, Primitive::kPrimShort>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IGET:
        PREAMBLE();
        DoFieldGet<InstancePrimitiveRead, Primitive::kPrimInt>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IGET_WIDE:
        PREAMBLE();
        DoFieldGet<InstancePrimitiveRead, Primitive::kPrimLong>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IGET_OBJECT:
        PREAMBLE();
        DoFieldGet<InstanceObjectRead, Primitive::kPrimNot>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SGET_BOOLEAN:
        PREAMBLE();
        DoFieldGet<StaticPrimitiveRead, Primitive::kPrimBoolean>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SGET_BYTE:
        PREAMBLE();
        DoFieldGet<StaticPrimitiveRead, Primitive::kPrimByte>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SGET_CHAR:
        PREAMBLE();
        DoFieldGet<StaticPrimitiveRead, Primitive::kPrimChar>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SGET_SHORT:
        PREAMBLE();
        DoFieldGet<StaticPrimitiveRead, Primitive::kPrimShort>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SGET:
        PREAMBLE();
        DoFieldGet<StaticPrimitiveRead, Primitive::kPrimInt>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SGET_WIDE:
        PREAMBLE();
        DoFieldGet<StaticPrimitiveRead, Primitive::kPrimLong>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SGET_OBJECT:
        PREAMBLE();
        DoFieldGet<StaticObjectRead, Primitive::kPrimNot>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IPUT_BOOLEAN:
        PREAMBLE();
        DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimBoolean>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IPUT_BYTE:
        PREAMBLE();
        DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimByte>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IPUT_CHAR:
        PREAMBLE();
        DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimChar>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IPUT_SHORT:
        PREAMBLE();
        DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimShort>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IPUT:
        PREAMBLE();
        DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimInt>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IPUT_WIDE:
        PREAMBLE();
        DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimLong>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::IPUT_OBJECT:
        PREAMBLE();
        DoFieldPut<InstanceObjectWrite, Primitive::kPrimNot>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SPUT_BOOLEAN:
        PREAMBLE();
        DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimBoolean>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SPUT_BYTE:
        PREAMBLE();
        DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimByte>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SPUT_CHAR:
        PREAMBLE();
        DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimChar>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SPUT_SHORT:
        PREAMBLE();
        DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimShort>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SPUT:
        PREAMBLE();
        DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimInt>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SPUT_WIDE:
        PREAMBLE();
        DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimLong>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SPUT_OBJECT:
        PREAMBLE();
        DoFieldPut<StaticObjectWrite, Primitive::kPrimNot>(self, shadow_frame, inst);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::INVOKE_VIRTUAL:
        PREAMBLE();
        DoInvoke<kVirtual, false>(self, shadow_frame, inst, &result_register);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_3xx);
        break;
      case Instruction::INVOKE_VIRTUAL_RANGE:
        PREAMBLE();
        DoInvoke<kVirtual, true>(self, shadow_frame, inst, &result_register);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_3xx);
        break;
      case Instruction::INVOKE_SUPER:
        PREAMBLE();
        DoInvoke<kSuper, false>(self, shadow_frame, inst, &result_register);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_3xx);
        break;
      case Instruction::INVOKE_SUPER_RANGE:
        PREAMBLE();
        DoInvoke<kSuper, true>(self, shadow_frame, inst, &result_register);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_3xx);
        break;
      case Instruction::INVOKE_DIRECT:
        PREAMBLE();
        DoInvoke<kDirect, false>(self, shadow_frame, inst, &result_register);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_3xx);
        break;
      case Instruction::INVOKE_DIRECT_RANGE:
        PREAMBLE();
        DoInvoke<kDirect, true>(self, shadow_frame, inst, &result_register);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_3xx);
        break;
      case Instruction::INVOKE_INTERFACE:
        PREAMBLE();
        DoInvoke<kInterface, false>(self, shadow_frame, inst, &result_register);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_3xx);
        break;
      case Instruction::INVOKE_INTERFACE_RANGE:
        PREAMBLE();
        DoInvoke<kInterface, true>(self, shadow_frame, inst, &result_register);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_3xx);
        break;
      case Instruction::INVOKE_STATIC:
        PREAMBLE();
        DoInvoke<kStatic, false>(self, shadow_frame, inst, &result_register);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_3xx);
        break;
      case Instruction::INVOKE_STATIC_RANGE:
        PREAMBLE();
        DoInvoke<kStatic, true>(self, shadow_frame, inst, &result_register);
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_3xx);
        break;
      case Instruction::NEG_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_12x(), -shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::NOT_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_12x(), ~shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::NEG_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_12x(), -shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::NOT_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_12x(), ~shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::NEG_FLOAT:
        PREAMBLE();
        shadow_frame.SetVRegFloat(inst->VRegA_12x(), -shadow_frame.GetVRegFloat(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::NEG_DOUBLE:
        PREAMBLE();
        shadow_frame.SetVRegDouble(inst->VRegA_12x(), -shadow_frame.GetVRegDouble(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::INT_TO_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_12x(), shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::INT_TO_FLOAT:
        PREAMBLE();
        shadow_frame.SetVRegFloat(inst->VRegA_12x(), shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::INT_TO_DOUBLE:
        PREAMBLE();
        shadow_frame.SetVRegDouble(inst->VRegA_12x(), shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::LONG_TO_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_12x(), shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::LONG_TO_FLOAT:
        PREAMBLE();
        shadow_frame.SetVRegFloat(inst->VRegA_12x(), shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::LONG_TO_DOUBLE:
        PREAMBLE();
        shadow_frame.SetVRegDouble(inst->VRegA_12x(), shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::FLOAT_TO_INT: {
        PREAMBLE();
        uint32_t dst = inst->VRegA_12x();
        float val = shadow_frame.GetVRegFloat(inst->VRegB_12x());
        if (val != val) {
          shadow_frame.SetVReg(dst, 0);
        } else if (val > static_cast<float>(kMaxInt)) {
          shadow_frame.SetVReg(dst, kMaxInt);
        } else if (val < static_cast<float>(kMinInt)) {
          shadow_frame.SetVReg(dst, kMinInt);
        } else {
          shadow_frame.SetVReg(dst, val);
        }
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::FLOAT_TO_LONG: {
        PREAMBLE();
        uint32_t dst = inst->VRegA_12x();
        float val = shadow_frame.GetVRegFloat(inst->VRegB_12x());
        if (val != val) {
          shadow_frame.SetVRegLong(dst, 0);
        } else if (val > static_cast<float>(kMaxLong)) {
          shadow_frame.SetVRegLong(dst, kMaxLong);
        } else if (val < static_cast<float>(kMinLong)) {
          shadow_frame.SetVRegLong(dst, kMinLong);
        } else {
          shadow_frame.SetVRegLong(dst, val);
        }
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::FLOAT_TO_DOUBLE:
        PREAMBLE();
        shadow_frame.SetVRegDouble(inst->VRegA_12x(), shadow_frame.GetVRegFloat(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::DOUBLE_TO_INT: {
        PREAMBLE();
        uint32_t dst = inst->VRegA_12x();
        double val = shadow_frame.GetVRegDouble(inst->VRegB_12x());
        if (val != val) {
          shadow_frame.SetVReg(dst, 0);
        } else if (val > static_cast<double>(kMaxInt)) {
          shadow_frame.SetVReg(dst, kMaxInt);
        } else if (val < static_cast<double>(kMinInt)) {
          shadow_frame.SetVReg(dst, kMinInt);
        } else {
          shadow_frame.SetVReg(dst, val);
        }
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::DOUBLE_TO_LONG: {
        PREAMBLE();
        uint32_t dst = inst->VRegA_12x();
        double val = shadow_frame.GetVRegDouble(inst->VRegB_12x());
        if (val != val) {
          shadow_frame.SetVRegLong(dst, 0);
        } else if (val > static_cast<double>(kMaxLong)) {
          shadow_frame.SetVRegLong(dst, kMaxLong);
        } else if (val < static_cast<double>(kMinLong)) {
          shadow_frame.SetVRegLong(dst, kMinLong);
        } else {
          shadow_frame.SetVRegLong(dst, val);
        }
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::DOUBLE_TO_FLOAT:
        PREAMBLE();
        shadow_frame.SetVRegFloat(inst->VRegA_12x(), shadow_frame.GetVRegDouble(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      case Instruction::INT_TO_BYTE:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_12x(),
                             static_cast<int8_t>(shadow_frame.GetVReg(inst->VRegB_12x())));
        inst = inst->Next_1xx();
        break;
      case Instruction::INT_TO_CHAR:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_12x(),
                             static_cast<uint16_t>(shadow_frame.GetVReg(inst->VRegB_12x())));
        inst = inst->Next_1xx();
        break;
      case Instruction::INT_TO_SHORT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_12x(),
                             static_cast<int16_t>(shadow_frame.GetVReg(inst->VRegB_12x())));
        inst = inst->Next_1xx();
        break;
      case Instruction::ADD_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_23x(),
                             shadow_frame.GetVReg(inst->VRegB_23x()) +
                             shadow_frame.GetVReg(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::SUB_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_23x(),
                             shadow_frame.GetVReg(inst->VRegB_23x()) -
                             shadow_frame.GetVReg(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::MUL_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_23x(),
                             shadow_frame.GetVReg(inst->VRegB_23x()) *
                             shadow_frame.GetVReg(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::DIV_INT:
        PREAMBLE();
        DoIntDivide(self, shadow_frame, inst->VRegA_23x(),
                    shadow_frame.GetVReg(inst->VRegB_23x()),
                    shadow_frame.GetVReg(inst->VRegC_23x()));
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::REM_INT:
        PREAMBLE();
        DoIntRemainder(self, shadow_frame, inst->VRegA_23x(),
                       shadow_frame.GetVReg(inst->VRegB_23x()),
                       shadow_frame.GetVReg(inst->VRegC_23x()));
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::SHL_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_23x(),
                             shadow_frame.GetVReg(inst->VRegB_23x()) <<
                             (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x1f));
        inst = inst->Next_2xx();
        break;
      case Instruction::SHR_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_23x(),
                             shadow_frame.GetVReg(inst->VRegB_23x()) >>
                             (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x1f));
        inst = inst->Next_2xx();
        break;
      case Instruction::USHR_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_23x(),
                             static_cast<uint32_t>(shadow_frame.GetVReg(inst->VRegB_23x())) >>
                             (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x1f));
        inst = inst->Next_2xx();
        break;
      case Instruction::AND_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_23x(),
                             shadow_frame.GetVReg(inst->VRegB_23x()) &
                             shadow_frame.GetVReg(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::OR_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_23x(),
                             shadow_frame.GetVReg(inst->VRegB_23x()) |
                             shadow_frame.GetVReg(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::XOR_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_23x(),
                             shadow_frame.GetVReg(inst->VRegB_23x()) ^
                             shadow_frame.GetVReg(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::ADD_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_23x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_23x()) +
                                 shadow_frame.GetVRegLong(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::SUB_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_23x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_23x()) -
                                 shadow_frame.GetVRegLong(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::MUL_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_23x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_23x()) *
                                 shadow_frame.GetVRegLong(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::DIV_LONG:
        PREAMBLE();
        DoLongDivide(self, shadow_frame, inst->VRegA_23x(),
                     shadow_frame.GetVRegLong(inst->VRegB_23x()),
                    shadow_frame.GetVRegLong(inst->VRegC_23x()));
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::REM_LONG:
        PREAMBLE();
        DoLongRemainder(self, shadow_frame, inst->VRegA_23x(),
                        shadow_frame.GetVRegLong(inst->VRegB_23x()),
                        shadow_frame.GetVRegLong(inst->VRegC_23x()));
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::AND_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_23x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_23x()) &
                                 shadow_frame.GetVRegLong(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::OR_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_23x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_23x()) |
                                 shadow_frame.GetVRegLong(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::XOR_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_23x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_23x()) ^
                                 shadow_frame.GetVRegLong(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::SHL_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_23x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_23x()) <<
                                 (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x3f));
        inst = inst->Next_2xx();
        break;
      case Instruction::SHR_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_23x(),
                                 shadow_frame.GetVRegLong(inst->VRegB_23x()) >>
                                 (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x3f));
        inst = inst->Next_2xx();
        break;
      case Instruction::USHR_LONG:
        PREAMBLE();
        shadow_frame.SetVRegLong(inst->VRegA_23x(),
                                 static_cast<uint64_t>(shadow_frame.GetVRegLong(inst->VRegB_23x())) >>
                                 (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x3f));
        inst = inst->Next_2xx();
        break;
      case Instruction::ADD_FLOAT:
        PREAMBLE();
        shadow_frame.SetVRegFloat(inst->VRegA_23x(),
                                  shadow_frame.GetVRegFloat(inst->VRegB_23x()) +
                                  shadow_frame.GetVRegFloat(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::SUB_FLOAT:
        PREAMBLE();
        shadow_frame.SetVRegFloat(inst->VRegA_23x(),
                                  shadow_frame.GetVRegFloat(inst->VRegB_23x()) -
                                  shadow_frame.GetVRegFloat(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::MUL_FLOAT:
        PREAMBLE();
        shadow_frame.SetVRegFloat(inst->VRegA_23x(),
                                  shadow_frame.GetVRegFloat(inst->VRegB_23x()) *
                                  shadow_frame.GetVRegFloat(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::DIV_FLOAT:
        PREAMBLE();
        shadow_frame.SetVRegFloat(inst->VRegA_23x(),
                                  shadow_frame.GetVRegFloat(inst->VRegB_23x()) /
                                  shadow_frame.GetVRegFloat(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::REM_FLOAT:
        PREAMBLE();
        shadow_frame.SetVRegFloat(inst->VRegA_23x(),
                                  fmodf(shadow_frame.GetVRegFloat(inst->VRegB_23x()),
                                        shadow_frame.GetVRegFloat(inst->VRegC_23x())));
        inst = inst->Next_2xx();
        break;
      case Instruction::ADD_DOUBLE:
        PREAMBLE();
        shadow_frame.SetVRegDouble(inst->VRegA_23x(),
                                   shadow_frame.GetVRegDouble(inst->VRegB_23x()) +
                                   shadow_frame.GetVRegDouble(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::SUB_DOUBLE:
        PREAMBLE();
        shadow_frame.SetVRegDouble(inst->VRegA_23x(),
                                   shadow_frame.GetVRegDouble(inst->VRegB_23x()) -
                                   shadow_frame.GetVRegDouble(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::MUL_DOUBLE:
        PREAMBLE();
        shadow_frame.SetVRegDouble(inst->VRegA_23x(),
                                   shadow_frame.GetVRegDouble(inst->VRegB_23x()) *
                                   shadow_frame.GetVRegDouble(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::DIV_DOUBLE:
        PREAMBLE();
        shadow_frame.SetVRegDouble(inst->VRegA_23x(),
                                   shadow_frame.GetVRegDouble(inst->VRegB_23x()) /
                                   shadow_frame.GetVRegDouble(inst->VRegC_23x()));
        inst = inst->Next_2xx();
        break;
      case Instruction::REM_DOUBLE:
        PREAMBLE();
        shadow_frame.SetVRegDouble(inst->VRegA_23x(),
                                   fmod(shadow_frame.GetVRegDouble(inst->VRegB_23x()),
                                        shadow_frame.GetVRegDouble(inst->VRegC_23x())));
        inst = inst->Next_2xx();
        break;
      case Instruction::ADD_INT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVReg(vregA,
                             shadow_frame.GetVReg(vregA) +
                             shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::SUB_INT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVReg(vregA,
                             shadow_frame.GetVReg(vregA) -
                             shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::MUL_INT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVReg(vregA,
                             shadow_frame.GetVReg(vregA) *
                             shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::DIV_INT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        DoIntDivide(self, shadow_frame, vregA, shadow_frame.GetVReg(vregA),
                    shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::REM_INT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        DoIntRemainder(self, shadow_frame, vregA, shadow_frame.GetVReg(vregA),
                       shadow_frame.GetVReg(inst->VRegB_12x()));
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_1xx);
        break;
      }
      case Instruction::SHL_INT_2ADDR:{
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVReg(vregA,
                             shadow_frame.GetVReg(vregA) <<
                             (shadow_frame.GetVReg(inst->VRegB_12x()) & 0x1f));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::SHR_INT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVReg(vregA,
                             shadow_frame.GetVReg(vregA) >>
                             (shadow_frame.GetVReg(inst->VRegB_12x()) & 0x1f));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::USHR_INT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVReg(vregA,
                             static_cast<uint32_t>(shadow_frame.GetVReg(vregA)) >>
                             (shadow_frame.GetVReg(inst->VRegB_12x()) & 0x1f));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::AND_INT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVReg(vregA,
                             shadow_frame.GetVReg(vregA) &
                             shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::OR_INT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVReg(vregA,
                             shadow_frame.GetVReg(vregA) |
                             shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::XOR_INT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVReg(vregA,
                             shadow_frame.GetVReg(vregA) ^
                             shadow_frame.GetVReg(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::ADD_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegLong(vregA,
                                 shadow_frame.GetVRegLong(vregA) +
                                 shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::SUB_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegLong(vregA,
                                 shadow_frame.GetVRegLong(vregA) -
                                 shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::MUL_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegLong(vregA,
                                 shadow_frame.GetVRegLong(vregA) *
                                 shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::DIV_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        DoLongDivide(self, shadow_frame, vregA, shadow_frame.GetVRegLong(vregA),
                    shadow_frame.GetVRegLong(inst->VRegB_12x()));
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_1xx);
        break;
      }
      case Instruction::REM_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        DoLongRemainder(self, shadow_frame, vregA, shadow_frame.GetVRegLong(vregA),
                        shadow_frame.GetVRegLong(inst->VRegB_12x()));
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_1xx);
        break;
      }
      case Instruction::AND_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegLong(vregA,
                                 shadow_frame.GetVRegLong(vregA) &
                                 shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::OR_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegLong(vregA,
                                 shadow_frame.GetVRegLong(vregA) |
                                 shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::XOR_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegLong(vregA,
                                 shadow_frame.GetVRegLong(vregA) ^
                                 shadow_frame.GetVRegLong(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::SHL_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegLong(vregA,
                                 shadow_frame.GetVRegLong(vregA) <<
                                 (shadow_frame.GetVReg(inst->VRegB_12x()) & 0x3f));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::SHR_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegLong(vregA,
                                 shadow_frame.GetVRegLong(vregA) >>
                                 (shadow_frame.GetVReg(inst->VRegB_12x()) & 0x3f));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::USHR_LONG_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegLong(vregA,
                                 static_cast<uint64_t>(shadow_frame.GetVRegLong(vregA)) >>
                                 (shadow_frame.GetVReg(inst->VRegB_12x()) & 0x3f));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::ADD_FLOAT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegFloat(vregA,
                                  shadow_frame.GetVRegFloat(vregA) +
                                  shadow_frame.GetVRegFloat(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::SUB_FLOAT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegFloat(vregA,
                                  shadow_frame.GetVRegFloat(vregA) -
                                  shadow_frame.GetVRegFloat(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::MUL_FLOAT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegFloat(vregA,
                                  shadow_frame.GetVRegFloat(vregA) *
                                  shadow_frame.GetVRegFloat(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::DIV_FLOAT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegFloat(vregA,
                                  shadow_frame.GetVRegFloat(vregA) /
                                  shadow_frame.GetVRegFloat(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::REM_FLOAT_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegFloat(vregA,
                                  fmodf(shadow_frame.GetVRegFloat(vregA),
                                        shadow_frame.GetVRegFloat(inst->VRegB_12x())));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::ADD_DOUBLE_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegDouble(vregA,
                                   shadow_frame.GetVRegDouble(vregA) +
                                   shadow_frame.GetVRegDouble(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::SUB_DOUBLE_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegDouble(vregA,
                                   shadow_frame.GetVRegDouble(vregA) -
                                   shadow_frame.GetVRegDouble(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::MUL_DOUBLE_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegDouble(vregA,
                                   shadow_frame.GetVRegDouble(vregA) *
                                   shadow_frame.GetVRegDouble(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::DIV_DOUBLE_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegDouble(vregA,
                                   shadow_frame.GetVRegDouble(vregA) /
                                   shadow_frame.GetVRegDouble(inst->VRegB_12x()));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::REM_DOUBLE_2ADDR: {
        PREAMBLE();
        uint32_t vregA = inst->VRegA_12x();
        shadow_frame.SetVRegDouble(vregA,
                                   fmod(shadow_frame.GetVRegDouble(vregA),
                                        shadow_frame.GetVRegDouble(inst->VRegB_12x())));
        inst = inst->Next_1xx();
        break;
      }
      case Instruction::ADD_INT_LIT16:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22s(),
                             shadow_frame.GetVReg(inst->VRegB_22s()) +
                             inst->VRegC_22s());
        inst = inst->Next_2xx();
        break;
      case Instruction::RSUB_INT:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22s(),
                             inst->VRegC_22s() -
                             shadow_frame.GetVReg(inst->VRegB_22s()));
        inst = inst->Next_2xx();
        break;
      case Instruction::MUL_INT_LIT16:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22s(),
                             shadow_frame.GetVReg(inst->VRegB_22s()) *
                             inst->VRegC_22s());
        inst = inst->Next_2xx();
        break;
      case Instruction::DIV_INT_LIT16:
        PREAMBLE();
        DoIntDivide(self, shadow_frame, inst->VRegA_22s(),
                    shadow_frame.GetVReg(inst->VRegB_22s()), inst->VRegC_22s());
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::REM_INT_LIT16:
        PREAMBLE();
        DoIntRemainder(self, shadow_frame, inst->VRegA_22s(),
                       shadow_frame.GetVReg(inst->VRegB_22s()), inst->VRegC_22s());
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::AND_INT_LIT16:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22s(),
                             shadow_frame.GetVReg(inst->VRegB_22s()) &
                             inst->VRegC_22s());
        inst = inst->Next_2xx();
        break;
      case Instruction::OR_INT_LIT16:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22s(),
                             shadow_frame.GetVReg(inst->VRegB_22s()) |
                             inst->VRegC_22s());
        inst = inst->Next_2xx();
        break;
      case Instruction::XOR_INT_LIT16:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22s(),
                             shadow_frame.GetVReg(inst->VRegB_22s()) ^
                             inst->VRegC_22s());
        inst = inst->Next_2xx();
        break;
      case Instruction::ADD_INT_LIT8:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22b(),
                             shadow_frame.GetVReg(inst->VRegB_22b()) +
                             inst->VRegC_22b());
        inst = inst->Next_2xx();
        break;
      case Instruction::RSUB_INT_LIT8:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22b(),
                             inst->VRegC_22b() -
                             shadow_frame.GetVReg(inst->VRegB_22b()));
        inst = inst->Next_2xx();
        break;
      case Instruction::MUL_INT_LIT8:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22b(),
                             shadow_frame.GetVReg(inst->VRegB_22b()) *
                             inst->VRegC_22b());
        inst = inst->Next_2xx();
        break;
      case Instruction::DIV_INT_LIT8:
        PREAMBLE();
        DoIntDivide(self, shadow_frame, inst->VRegA_22b(),
                    shadow_frame.GetVReg(inst->VRegB_22b()), inst->VRegC_22b());
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::REM_INT_LIT8:
        PREAMBLE();
        DoIntRemainder(self, shadow_frame, inst->VRegA_22b(),
                       shadow_frame.GetVReg(inst->VRegB_22b()), inst->VRegC_22b());
        POSSIBLY_HANDLE_PENDING_EXCEPTION(Next_2xx);
        break;
      case Instruction::AND_INT_LIT8:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22b(),
                             shadow_frame.GetVReg(inst->VRegB_22b()) &
                             inst->VRegC_22b());
        inst = inst->Next_2xx();
        break;
      case Instruction::OR_INT_LIT8:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22b(),
                             shadow_frame.GetVReg(inst->VRegB_22b()) |
                             inst->VRegC_22b());
        inst = inst->Next_2xx();
        break;
      case Instruction::XOR_INT_LIT8:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22b(),
                             shadow_frame.GetVReg(inst->VRegB_22b()) ^
                             inst->VRegC_22b());
        inst = inst->Next_2xx();
        break;
      case Instruction::SHL_INT_LIT8:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22b(),
                             shadow_frame.GetVReg(inst->VRegB_22b()) <<
                             (inst->VRegC_22b() & 0x1f));
        inst = inst->Next_2xx();
        break;
      case Instruction::SHR_INT_LIT8:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22b(),
                             shadow_frame.GetVReg(inst->VRegB_22b()) >>
                             (inst->VRegC_22b() & 0x1f));
        inst = inst->Next_2xx();
        break;
      case Instruction::USHR_INT_LIT8:
        PREAMBLE();
        shadow_frame.SetVReg(inst->VRegA_22b(),
                             static_cast<uint32_t>(shadow_frame.GetVReg(inst->VRegB_22b())) >>
                             (inst->VRegC_22b() & 0x1f));
        inst = inst->Next_2xx();
        break;
      case Instruction::UNUSED_3E ... Instruction::UNUSED_43:
      case Instruction::UNUSED_E3 ... Instruction::UNUSED_FF:
      case Instruction::UNUSED_73:
      case Instruction::UNUSED_79:
      case Instruction::UNUSED_7A:
        UnexpectedOpcode(inst, mh);
    }
  }
}

void EnterInterpreterFromInvoke(Thread* self, AbstractMethod* method, Object* receiver,
                                uint32_t* args, JValue* result) {
  DCHECK_EQ(self, Thread::Current());
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEnd())) {
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
    ThrowLocation throw_location = self->GetCurrentLocationForThrow();
    self->ThrowNewExceptionF(throw_location, "Ljava/lang/AbstractMethodError;",
                             "abstract method \"%s\"", PrettyMethod(method).c_str());
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
  void* memory = alloca(ShadowFrame::ComputeSize(num_regs));
  ShadowFrame* shadow_frame(ShadowFrame::Create(num_regs, last_shadow_frame, method, 0, memory));
  self->PushShadowFrame(shadow_frame);
  size_t cur_reg = num_regs - num_ins;
  if (!method->IsStatic()) {
    CHECK(receiver != NULL);
    shadow_frame->SetVRegReference(cur_reg, receiver);
    ++cur_reg;
  } else if (UNLIKELY(!method->GetDeclaringClass()->IsInitializing())) {
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    if (UNLIKELY(!class_linker->EnsureInitialized(method->GetDeclaringClass(),
                                                  true, true))) {
      CHECK(self->IsExceptionPending());
      self->PopShadowFrame();
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
    JValue r = Execute(self, mh, code_item, *shadow_frame, JValue());
    if (result != NULL) {
      *result = r;
    }
  } else {
    // We don't expect to be asked to interpret native code (which is entered via a JNI compiler
    // generated stub) except during testing and image writing.
    if (!Runtime::Current()->IsStarted()) {
      UnstartedRuntimeJni(self, method, receiver, args, result);
    } else {
      InterpreterJni(self, method, shorty, receiver, args, result);
    }
  }
  self->PopShadowFrame();
}

void EnterInterpreterFromDeoptimize(Thread* self, ShadowFrame* shadow_frame, JValue* ret_val)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  JValue value;
  value.SetJ(ret_val->GetJ());  // Set value to last known result in case the shadow frame chain is empty.
  MethodHelper mh;
  while (shadow_frame != NULL) {
    self->SetTopOfShadowStack(shadow_frame);
    mh.ChangeMethod(shadow_frame->GetMethod());
    const DexFile::CodeItem* code_item = mh.GetCodeItem();
    value = Execute(self, mh, code_item, *shadow_frame, value);
    ShadowFrame* old_frame = shadow_frame;
    shadow_frame = shadow_frame->GetLink();
    delete old_frame;
  }
  ret_val->SetJ(value.GetJ());
}

JValue EnterInterpreterFromStub(Thread* self, MethodHelper& mh, const DexFile::CodeItem* code_item,
                                ShadowFrame& shadow_frame)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK_EQ(self, Thread::Current());
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEnd())) {
    ThrowStackOverflowError(self);
    return JValue();
  }

  return Execute(self, mh, code_item, shadow_frame, JValue());
}

void artInterpreterToInterpreterEntry(Thread* self, MethodHelper& mh,
                                      const DexFile::CodeItem* code_item,
                                      ShadowFrame* shadow_frame, JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEnd())) {
    ThrowStackOverflowError(self);
    return;
  }

  AbstractMethod* method = shadow_frame->GetMethod();
  if (method->IsStatic() && !method->GetDeclaringClass()->IsInitializing()) {
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(method->GetDeclaringClass(),
                                                                 true, true)) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return;
    }
    CHECK(method->GetDeclaringClass()->IsInitializing());
  }

  self->PushShadowFrame(shadow_frame);

  if (LIKELY(!method->IsNative())) {
    result->SetJ(Execute(self, mh, code_item, *shadow_frame, JValue()).GetJ());
  } else {
    // We don't expect to be asked to interpret native code (which is entered via a JNI compiler
    // generated stub) except during testing and image writing.
    CHECK(!Runtime::Current()->IsStarted());
    Object* receiver = method->IsStatic() ? NULL : shadow_frame->GetVRegReference(0);
    uint32_t* args = shadow_frame->GetVRegArgs(method->IsStatic() ? 0 : 1);
    UnstartedRuntimeJni(self, method, receiver, args, result);
  }

  self->PopShadowFrame();
  return;
}

}  // namespace interpreter
}  // namespace art
