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

#include "interpreter_common.h"

#include <limits>

#include "mirror/string-inl.h"

namespace art {
namespace interpreter {

// Hand select a number of methods to be run in a not yet started runtime without using JNI.
static void UnstartedRuntimeJni(Thread* self, ArtMethod* method,
                                Object* receiver, uint32_t* args, JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::string name(PrettyMethod(method));
  if (name == "java.lang.Object dalvik.system.VMRuntime.newUnpaddedArray(java.lang.Class, int)") {
    int32_t length = args[1];
    DCHECK_GE(length, 0);
    mirror::Class* element_class = reinterpret_cast<Object*>(args[0])->AsClass();
    Runtime* runtime = Runtime::Current();
    mirror::Class* array_class = runtime->GetClassLinker()->FindArrayClass(self, element_class);
    DCHECK(array_class != nullptr);
    gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
    result->SetL(mirror::Array::Alloc<true>(self, array_class, length,
                                            array_class->GetComponentSize(), allocator, true));
  } else if (name == "java.lang.ClassLoader dalvik.system.VMStack.getCallingClassLoader()") {
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
    StackHandleScope<1> hs(self);
    result->SetL(mirror::Class::ComputeName(hs.NewHandle(receiver->AsClass())));
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
    StackHandleScope<2> hs(self);
    auto h_class(hs.NewHandle(reinterpret_cast<mirror::Class*>(args[0])->AsClass()));
    auto h_dimensions(hs.NewHandle(reinterpret_cast<mirror::IntArray*>(args[1])->AsIntArray()));
    result->SetL(Array::CreateMultiArray(self, h_class, h_dimensions));
  } else if (name == "java.lang.Object java.lang.Throwable.nativeFillInStackTrace()") {
    ScopedObjectAccessUnchecked soa(self);
    if (Runtime::Current()->IsActiveTransaction()) {
      result->SetL(soa.Decode<Object*>(self->CreateInternalStackTrace<true>(soa)));
    } else {
      result->SetL(soa.Decode<Object*>(self->CreateInternalStackTrace<false>(soa)));
    }
  } else if (name == "int java.lang.System.identityHashCode(java.lang.Object)") {
    mirror::Object* obj = reinterpret_cast<Object*>(args[0]);
    result->SetI((obj != nullptr) ? obj->IdentityHashCode() : 0);
  } else if (name == "boolean java.nio.ByteOrder.isLittleEndian()") {
    result->SetZ(JNI_TRUE);
  } else if (name == "boolean sun.misc.Unsafe.compareAndSwapInt(java.lang.Object, long, int, int)") {
    Object* obj = reinterpret_cast<Object*>(args[0]);
    jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
    jint expectedValue = args[3];
    jint newValue = args[4];
    bool success;
    if (Runtime::Current()->IsActiveTransaction()) {
      success = obj->CasField32<true>(MemberOffset(offset), expectedValue, newValue);
    } else {
      success = obj->CasField32<false>(MemberOffset(offset), expectedValue, newValue);
    }
    result->SetZ(success ? JNI_TRUE : JNI_FALSE);
  } else if (name == "void sun.misc.Unsafe.putObject(java.lang.Object, long, java.lang.Object)") {
    Object* obj = reinterpret_cast<Object*>(args[0]);
    jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
    Object* newValue = reinterpret_cast<Object*>(args[3]);
    if (Runtime::Current()->IsActiveTransaction()) {
      obj->SetFieldObject<true>(MemberOffset(offset), newValue);
    } else {
      obj->SetFieldObject<false>(MemberOffset(offset), newValue);
    }
  } else if (name == "int sun.misc.Unsafe.getArrayBaseOffsetForComponentType(java.lang.Class)") {
    mirror::Class* component = reinterpret_cast<Object*>(args[0])->AsClass();
    Primitive::Type primitive_type = component->GetPrimitiveType();
    result->SetI(mirror::Array::DataOffset(Primitive::ComponentSize(primitive_type)).Int32Value());
  } else if (name == "int sun.misc.Unsafe.getArrayIndexScaleForComponentType(java.lang.Class)") {
    mirror::Class* component = reinterpret_cast<Object*>(args[0])->AsClass();
    Primitive::Type primitive_type = component->GetPrimitiveType();
    result->SetI(Primitive::ComponentSize(primitive_type));
  } else if (Runtime::Current()->IsActiveTransaction()) {
    AbortTransaction(self, "Attempt to invoke native method in non-started runtime: %s",
                     name.c_str());

  } else {
    LOG(FATAL) << "Calling native method " << PrettyMethod(method) << " in an unstarted "
        "non-transactional runtime";
  }
}

static void InterpreterJni(Thread* self, ArtMethod* method, const StringPiece& shorty,
                           Object* receiver, uint32_t* args, JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // TODO: The following enters JNI code using a typedef-ed function rather than the JNI compiler,
  //       it should be removed and JNI compiled stubs used instead.
  ScopedObjectAccessUnchecked soa(self);
  if (method->IsStatic()) {
    if (shorty == "L") {
      typedef jobject (fntype)(JNIEnv*, jclass);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      jobject jresult;
      {
        ScopedThreadStateChange tsc(self, kNative);
        jresult = fn(soa.Env(), klass.get());
      }
      result->SetL(soa.Decode<Object*>(jresult));
    } else if (shorty == "V") {
      typedef void (fntype)(JNIEnv*, jclass);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), klass.get());
    } else if (shorty == "Z") {
      typedef jboolean (fntype)(JNIEnv*, jclass);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetZ(fn(soa.Env(), klass.get()));
    } else if (shorty == "BI") {
      typedef jbyte (fntype)(JNIEnv*, jclass, jint);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetB(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "II") {
      typedef jint (fntype)(JNIEnv*, jclass, jint);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetI(fn(soa.Env(), klass.get(), args[0]));
    } else if (shorty == "LL") {
      typedef jobject (fntype)(JNIEnv*, jclass, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
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
      typedef jint (fntype)(JNIEnv*, jclass, jint, jboolean);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetI(fn(soa.Env(), klass.get(), args[0], args[1]));
    } else if (shorty == "ILI") {
      typedef jint (fntype)(JNIEnv*, jclass, jobject, jint);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[0])));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetI(fn(soa.Env(), klass.get(), arg0.get(), args[1]));
    } else if (shorty == "SIZ") {
      typedef jshort (fntype)(JNIEnv*, jclass, jint, jboolean);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetS(fn(soa.Env(), klass.get(), args[0], args[1]));
    } else if (shorty == "VIZ") {
      typedef void (fntype)(JNIEnv*, jclass, jint, jboolean);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), klass.get(), args[0], args[1]);
    } else if (shorty == "ZLL") {
      typedef jboolean (fntype)(JNIEnv*, jclass, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg0(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[0])));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[1])));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetZ(fn(soa.Env(), klass.get(), arg0.get(), arg1.get()));
    } else if (shorty == "ZILL") {
      typedef jboolean (fntype)(JNIEnv*, jclass, jint, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[1])));
      ScopedLocalRef<jobject> arg2(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[2])));
      ScopedThreadStateChange tsc(self, kNative);
      result->SetZ(fn(soa.Env(), klass.get(), args[0], arg1.get(), arg2.get()));
    } else if (shorty == "VILII") {
      typedef void (fntype)(JNIEnv*, jclass, jint, jobject, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jclass> klass(soa.Env(),
                                   soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
      ScopedLocalRef<jobject> arg1(soa.Env(),
                                   soa.AddLocalReference<jobject>(reinterpret_cast<Object*>(args[1])));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), klass.get(), args[0], arg1.get(), args[2], args[3]);
    } else if (shorty == "VLILII") {
      typedef void (fntype)(JNIEnv*, jclass, jobject, jint, jobject, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
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
      typedef jobject (fntype)(JNIEnv*, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      jobject jresult;
      {
        ScopedThreadStateChange tsc(self, kNative);
        jresult = fn(soa.Env(), rcvr.get());
      }
      result->SetL(soa.Decode<Object*>(jresult));
    } else if (shorty == "V") {
      typedef void (fntype)(JNIEnv*, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
      ScopedLocalRef<jobject> rcvr(soa.Env(),
                                   soa.AddLocalReference<jobject>(receiver));
      ScopedThreadStateChange tsc(self, kNative);
      fn(soa.Env(), rcvr.get());
    } else if (shorty == "LL") {
      typedef jobject (fntype)(JNIEnv*, jobject, jobject);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
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
      typedef jint (fntype)(JNIEnv*, jobject, jint, jint);
      fntype* const fn = reinterpret_cast<fntype*>(const_cast<void*>(method->GetNativeMethod()));
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

enum InterpreterImplKind {
  kSwitchImpl,            // Switch-based interpreter implementation.
  kComputedGotoImplKind   // Computed-goto-based interpreter implementation.
};

#if !defined(__clang__)
static constexpr InterpreterImplKind kInterpreterImplKind = kComputedGotoImplKind;
#else
// Clang 3.4 fails to build the goto interpreter implementation.
static constexpr InterpreterImplKind kInterpreterImplKind = kSwitchImpl;
template<bool do_access_check, bool transaction_active>
JValue ExecuteGotoImpl(Thread* self, MethodHelper& mh, const DexFile::CodeItem* code_item,
                       ShadowFrame& shadow_frame, JValue result_register) {
  LOG(FATAL) << "UNREACHABLE";
  exit(0);
}
// Explicit definitions of ExecuteGotoImpl.
template<> SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
JValue ExecuteGotoImpl<true, false>(Thread* self, MethodHelper& mh,
                                    const DexFile::CodeItem* code_item,
                                    ShadowFrame& shadow_frame, JValue result_register);
template<> SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
JValue ExecuteGotoImpl<false, false>(Thread* self, MethodHelper& mh,
                                     const DexFile::CodeItem* code_item,
                                     ShadowFrame& shadow_frame, JValue result_register);
template<> SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
JValue ExecuteGotoImpl<true, true>(Thread* self, MethodHelper& mh,
                                    const DexFile::CodeItem* code_item,
                                    ShadowFrame& shadow_frame, JValue result_register);
template<> SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
JValue ExecuteGotoImpl<false, true>(Thread* self, MethodHelper& mh,
                                     const DexFile::CodeItem* code_item,
                                     ShadowFrame& shadow_frame, JValue result_register);
#endif

static JValue Execute(Thread* self, MethodHelper& mh, const DexFile::CodeItem* code_item,
                      ShadowFrame& shadow_frame, JValue result_register)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

static inline JValue Execute(Thread* self, MethodHelper& mh, const DexFile::CodeItem* code_item,
                             ShadowFrame& shadow_frame, JValue result_register) {
  DCHECK(shadow_frame.GetMethod() == mh.GetMethod() ||
         shadow_frame.GetMethod()->GetDeclaringClass()->IsProxyClass());
  DCHECK(!shadow_frame.GetMethod()->IsAbstract());
  DCHECK(!shadow_frame.GetMethod()->IsNative());

  bool transaction_active = Runtime::Current()->IsActiveTransaction();
  if (LIKELY(shadow_frame.GetMethod()->IsPreverified())) {
    // Enter the "without access check" interpreter.
    if (kInterpreterImplKind == kSwitchImpl) {
      if (transaction_active) {
        return ExecuteSwitchImpl<false, true>(self, mh, code_item, shadow_frame, result_register);
      } else {
        return ExecuteSwitchImpl<false, false>(self, mh, code_item, shadow_frame, result_register);
      }
    } else {
      DCHECK_EQ(kInterpreterImplKind, kComputedGotoImplKind);
      if (transaction_active) {
        return ExecuteGotoImpl<false, true>(self, mh, code_item, shadow_frame, result_register);
      } else {
        return ExecuteGotoImpl<false, false>(self, mh, code_item, shadow_frame, result_register);
      }
    }
  } else {
    // Enter the "with access check" interpreter.
    if (kInterpreterImplKind == kSwitchImpl) {
      if (transaction_active) {
        return ExecuteSwitchImpl<true, true>(self, mh, code_item, shadow_frame, result_register);
      } else {
        return ExecuteSwitchImpl<true, false>(self, mh, code_item, shadow_frame, result_register);
      }
    } else {
      DCHECK_EQ(kInterpreterImplKind, kComputedGotoImplKind);
      if (transaction_active) {
        return ExecuteGotoImpl<true, true>(self, mh, code_item, shadow_frame, result_register);
      } else {
        return ExecuteGotoImpl<true, false>(self, mh, code_item, shadow_frame, result_register);
      }
    }
  }
}

void EnterInterpreterFromInvoke(Thread* self, ArtMethod* method, Object* receiver,
                                uint32_t* args, JValue* result) {
  DCHECK_EQ(self, Thread::Current());
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEnd())) {
    ThrowStackOverflowError(self);
    return;
  }

  const char* old_cause = self->StartAssertNoThreadSuspension("EnterInterpreterFromInvoke");
  MethodHelper mh(method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  uint16_t num_regs;
  uint16_t num_ins;
  if (code_item != NULL) {
    num_regs =  code_item->registers_size_;
    num_ins = code_item->ins_size_;
  } else if (method->IsAbstract()) {
    self->EndAssertNoThreadSuspension(old_cause);
    ThrowAbstractMethodError(method);
    return;
  } else {
    DCHECK(method->IsNative());
    num_regs = num_ins = ArtMethod::NumArgRegisters(mh.GetShorty());
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
  }
  const char* shorty = mh.GetShorty();
  for (size_t shorty_pos = 0, arg_pos = 0; cur_reg < num_regs; ++shorty_pos, ++arg_pos, cur_reg++) {
    DCHECK_LT(shorty_pos + 1, mh.GetShortyLength());
    switch (shorty[shorty_pos + 1]) {
      case 'L': {
        Object* o = reinterpret_cast<StackReference<Object>*>(&args[arg_pos])->AsMirrorPtr();
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
  self->EndAssertNoThreadSuspension(old_cause);
  // Do this after populating the shadow frame in case EnsureInitialized causes a GC.
  if (method->IsStatic() && UNLIKELY(!method->GetDeclaringClass()->IsInitializing())) {
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(method->GetDeclaringClass()));
    if (UNLIKELY(!class_linker->EnsureInitialized(h_class, true, true))) {
      CHECK(self->IsExceptionPending());
      self->PopShadowFrame();
      return;
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
    // Update args to be the args in the shadow frame since the input ones could hold stale
    // references pointers due to moving GC.
    args = shadow_frame->GetVRegArgs(method->IsStatic() ? 0 : 1);
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
                                ShadowFrame& shadow_frame) {
  DCHECK_EQ(self, Thread::Current());
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEnd())) {
    ThrowStackOverflowError(self);
    return JValue();
  }

  return Execute(self, mh, code_item, shadow_frame, JValue());
}

extern "C" void artInterpreterToInterpreterBridge(Thread* self, MethodHelper& mh,
                                                  const DexFile::CodeItem* code_item,
                                                  ShadowFrame* shadow_frame, JValue* result) {
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEnd())) {
    ThrowStackOverflowError(self);
    return;
  }

  self->PushShadowFrame(shadow_frame);
  ArtMethod* method = shadow_frame->GetMethod();
  // Ensure static methods are initialized.
  if (method->IsStatic()) {
    StackHandleScope<1> hs(self);
    Handle<Class> declaringClass(hs.NewHandle(method->GetDeclaringClass()));
    if (UNLIKELY(!declaringClass->IsInitializing())) {
      if (UNLIKELY(!Runtime::Current()->GetClassLinker()->EnsureInitialized(declaringClass, true,
                                                                            true))) {
        DCHECK(Thread::Current()->IsExceptionPending());
        self->PopShadowFrame();
        return;
      }
      CHECK(declaringClass->IsInitializing());
    }
  }

  if (LIKELY(!method->IsNative())) {
    result->SetJ(Execute(self, mh, code_item, *shadow_frame, JValue()).GetJ());
  } else {
    // We don't expect to be asked to interpret native code (which is entered via a JNI compiler
    // generated stub) except during testing and image writing.
    CHECK(!Runtime::Current()->IsStarted());
    Object* receiver = method->IsStatic() ? nullptr : shadow_frame->GetVRegReference(0);
    uint32_t* args = shadow_frame->GetVRegArgs(method->IsStatic() ? 0 : 1);
    UnstartedRuntimeJni(self, method, receiver, args, result);
  }

  self->PopShadowFrame();
}

}  // namespace interpreter
}  // namespace art
