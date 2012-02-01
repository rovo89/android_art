/*
 * Copyright 2011 Google Inc. All Rights Reserved.
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

#include "runtime_support.h"

#include "dex_cache.h"
#include "dex_verifier.h"
#include "macros.h"
#include "object.h"
#include "object_utils.h"
#include "reflection.h"
#include "trace.h"
#include "ScopedLocalRef.h"

namespace art {

// Place a special frame at the TOS that will save the callee saves for the given type
static void  FinishCalleeSaveFrameSetup(Thread* self, Method** sp, Runtime::CalleeSaveType type) {
  // Be aware the store below may well stomp on an incoming argument
  *sp = Runtime::Current()->GetCalleeSaveMethod(type);
  self->SetTopOfStack(sp, 0);
}

// Temporary debugging hook for compiler.
extern void DebugMe(Method* method, uint32_t info) {
  LOG(INFO) << "DebugMe";
  if (method != NULL) {
    LOG(INFO) << PrettyMethod(method);
  }
  LOG(INFO) << "Info: " << info;
}

extern "C" uint32_t artObjectInitFromCode(Object* o, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  Class* c = o->GetClass();
  if (UNLIKELY(c->IsFinalizable())) {
    Heap::AddFinalizerReference(self, o);
  }
  /*
   * NOTE: once debugger/profiler support is added, we'll need to check
   * here and branch to actual compiled object.<init> to handle any
   * breakpoint/logging activities if either is active.
   */
  return self->IsExceptionPending() ? -1 : 0;
}

// Return value helper for jobject return types
extern Object* DecodeJObjectInThread(Thread* thread, jobject obj) {
  if (thread->IsExceptionPending()) {
    return NULL;
  }
  return thread->DecodeJObject(obj);
}

extern void* FindNativeMethod(Thread* thread) {
  DCHECK(Thread::Current() == thread);

  Method* method = const_cast<Method*>(thread->GetCurrentMethod());
  DCHECK(method != NULL);

  // Lookup symbol address for method, on failure we'll return NULL with an
  // exception set, otherwise we return the address of the method we found.
  void* native_code = thread->GetJniEnv()->vm->FindCodeForNativeMethod(method);
  if (native_code == NULL) {
    DCHECK(thread->IsExceptionPending());
    return NULL;
  } else {
    // Register so that future calls don't come here
    method->RegisterNative(native_code);
    return native_code;
  }
}

// Called by generated call to throw an exception
extern "C" void artDeliverExceptionFromCode(Throwable* exception, Thread* thread, Method** sp) {
  /*
   * exception may be NULL, in which case this routine should
   * throw NPE.  NOTE: this is a convenience for generated code,
   * which previously did the null check inline and constructed
   * and threw a NPE if NULL.  This routine responsible for setting
   * exception_ in thread and delivering the exception.
   */
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  if (exception == NULL) {
    thread->ThrowNewException("Ljava/lang/NullPointerException;", "throw with null exception");
  } else {
    thread->SetException(exception);
  }
  thread->DeliverException();
}

// Deliver an exception that's pending on thread helping set up a callee save frame on the way
extern "C" void artDeliverPendingExceptionFromCode(Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->DeliverException();
}

// Called by generated call to throw a NPE exception
extern "C" void artThrowNullPointerExceptionFromCode(Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewException("Ljava/lang/NullPointerException;", NULL);
  thread->DeliverException();
}

// Called by generated call to throw an arithmetic divide by zero exception
extern "C" void artThrowDivZeroFromCode(Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewException("Ljava/lang/ArithmeticException;", "divide by zero");
  thread->DeliverException();
}

// Called by generated call to throw an arithmetic divide by zero exception
extern "C" void artThrowArrayBoundsFromCode(int index, int limit, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                             "length=%d; index=%d", limit, index);
  thread->DeliverException();
}

// Called by the AbstractMethodError stub (not runtime support)
extern void ThrowAbstractMethodErrorFromCode(Method* method, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewExceptionF("Ljava/lang/AbstractMethodError;",
                             "abstract method \"%s\"", PrettyMethod(method).c_str());
  thread->DeliverException();
}

extern "C" void artThrowStackOverflowFromCode(Method* method, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  // Remove extra entry pushed onto second stack during method tracing
  if (Runtime::Current()->IsMethodTracingActive()) {
    artTraceMethodUnwindFromCode(thread);
  }
  thread->SetStackEndForStackOverflow();  // Allow space on the stack for constructor to execute
  thread->ThrowNewExceptionF("Ljava/lang/StackOverflowError;",
      "stack size %zdkb; default stack size: %zdkb",
      thread->GetStackSize() / KB, Runtime::Current()->GetDefaultStackSize() / KB);
  thread->ResetDefaultStackEnd();  // Return to default stack size
  thread->DeliverException();
}

static std::string ClassNameFromIndex(Method* method, uint32_t ref,
                                      verifier::VerifyErrorRefType ref_type, bool access) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(method->GetDeclaringClass()->GetDexCache());

  uint16_t type_idx = 0;
  if (ref_type == verifier::VERIFY_ERROR_REF_FIELD) {
    const DexFile::FieldId& id = dex_file.GetFieldId(ref);
    type_idx = id.class_idx_;
  } else if (ref_type == verifier::VERIFY_ERROR_REF_METHOD) {
    const DexFile::MethodId& id = dex_file.GetMethodId(ref);
    type_idx = id.class_idx_;
  } else if (ref_type == verifier::VERIFY_ERROR_REF_CLASS) {
    type_idx = ref;
  } else {
    CHECK(false) << static_cast<int>(ref_type);
  }

  std::string class_name(PrettyDescriptor(dex_file.StringByTypeIdx(type_idx)));
  if (!access) {
    return class_name;
  }

  std::string result;
  result += "tried to access class ";
  result += class_name;
  result += " from class ";
  result += PrettyDescriptor(method->GetDeclaringClass());
  return result;
}

static std::string FieldNameFromIndex(const Method* method, uint32_t ref,
                                      verifier::VerifyErrorRefType ref_type, bool access) {
  CHECK_EQ(static_cast<int>(ref_type), static_cast<int>(verifier::VERIFY_ERROR_REF_FIELD));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(method->GetDeclaringClass()->GetDexCache());

  const DexFile::FieldId& id = dex_file.GetFieldId(ref);
  std::string class_name(PrettyDescriptor(dex_file.GetFieldDeclaringClassDescriptor(id)));
  const char* field_name = dex_file.StringDataByIdx(id.name_idx_);
  if (!access) {
    return class_name + "." + field_name;
  }

  std::string result;
  result += "tried to access field ";
  result += class_name + "." + field_name;
  result += " from class ";
  result += PrettyDescriptor(method->GetDeclaringClass());
  return result;
}

static std::string MethodNameFromIndex(const Method* method, uint32_t ref,
                                verifier::VerifyErrorRefType ref_type, bool access) {
  CHECK_EQ(static_cast<int>(ref_type), static_cast<int>(verifier::VERIFY_ERROR_REF_METHOD));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(method->GetDeclaringClass()->GetDexCache());

  const DexFile::MethodId& id = dex_file.GetMethodId(ref);
  std::string class_name(PrettyDescriptor(dex_file.GetMethodDeclaringClassDescriptor(id)));
  const char* method_name = dex_file.StringDataByIdx(id.name_idx_);
  if (!access) {
    return class_name + "." + method_name;
  }

  std::string result;
  result += "tried to access method ";
  result += class_name + "." + method_name + ":" +
      dex_file.CreateMethodSignature(id.proto_idx_, NULL);
  result += " from class ";
  result += PrettyDescriptor(method->GetDeclaringClass());
  return result;
}

extern "C" void artThrowVerificationErrorFromCode(int32_t kind, int32_t ref, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kSaveAll);
  Frame frame = self->GetTopOfStack();  // We need the calling method as context to interpret 'ref'
  frame.Next();
  Method* method = frame.GetMethod();

  verifier::VerifyErrorRefType ref_type =
      static_cast<verifier::VerifyErrorRefType>(kind >> verifier::kVerifyErrorRefTypeShift);

  const char* exception_class = "Ljava/lang/VerifyError;";
  std::string msg;

  switch (static_cast<verifier::VerifyError>(kind & ~(0xff << verifier::kVerifyErrorRefTypeShift))) {
  case verifier::VERIFY_ERROR_NO_CLASS:
    exception_class = "Ljava/lang/NoClassDefFoundError;";
    msg = ClassNameFromIndex(method, ref, ref_type, false);
    break;
  case verifier::VERIFY_ERROR_NO_FIELD:
    exception_class = "Ljava/lang/NoSuchFieldError;";
    msg = FieldNameFromIndex(method, ref, ref_type, false);
    break;
  case verifier::VERIFY_ERROR_NO_METHOD:
    exception_class = "Ljava/lang/NoSuchMethodError;";
    msg = MethodNameFromIndex(method, ref, ref_type, false);
    break;
  case verifier::VERIFY_ERROR_ACCESS_CLASS:
    exception_class = "Ljava/lang/IllegalAccessError;";
    msg = ClassNameFromIndex(method, ref, ref_type, true);
    break;
  case verifier::VERIFY_ERROR_ACCESS_FIELD:
    exception_class = "Ljava/lang/IllegalAccessError;";
    msg = FieldNameFromIndex(method, ref, ref_type, true);
    break;
  case verifier::VERIFY_ERROR_ACCESS_METHOD:
    exception_class = "Ljava/lang/IllegalAccessError;";
    msg = MethodNameFromIndex(method, ref, ref_type, true);
    break;
  case verifier::VERIFY_ERROR_CLASS_CHANGE:
    exception_class = "Ljava/lang/IncompatibleClassChangeError;";
    msg = ClassNameFromIndex(method, ref, ref_type, false);
    break;
  case verifier::VERIFY_ERROR_INSTANTIATION:
    exception_class = "Ljava/lang/InstantiationError;";
    msg = ClassNameFromIndex(method, ref, ref_type, false);
    break;
  case verifier::VERIFY_ERROR_GENERIC:
    // Generic VerifyError; use default exception, no message.
    break;
  case verifier::VERIFY_ERROR_NONE:
    CHECK(false);
    break;
  }
  self->ThrowNewException(exception_class, msg.c_str());
  self->DeliverException();
}

extern "C" void artThrowInternalErrorFromCode(int32_t errnum, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  LOG(WARNING) << "TODO: internal error detail message. errnum=" << errnum;
  thread->ThrowNewExceptionF("Ljava/lang/InternalError;", "errnum=%d", errnum);
  thread->DeliverException();
}

extern "C" void artThrowRuntimeExceptionFromCode(int32_t errnum, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  LOG(WARNING) << "TODO: runtime exception detail message. errnum=" << errnum;
  thread->ThrowNewExceptionF("Ljava/lang/RuntimeException;", "errnum=%d", errnum);
  thread->DeliverException();
}

extern "C" void artThrowNoSuchMethodFromCode(int32_t method_idx, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kSaveAll);
  Frame frame = self->GetTopOfStack();  // We need the calling method as context for the method_idx
  frame.Next();
  Method* method = frame.GetMethod();
  self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
      MethodNameFromIndex(method, method_idx, verifier::VERIFY_ERROR_REF_METHOD, false).c_str());
  self->DeliverException();
}

extern "C" void artThrowNegArraySizeFromCode(int32_t size, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  LOG(WARNING) << "UNTESTED artThrowNegArraySizeFromCode";
  thread->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", size);
  thread->DeliverException();
}

void* UnresolvedDirectMethodTrampolineFromCode(int32_t method_idx, Method** sp, Thread* thread,
                                               Runtime::TrampolineType type) {
  // TODO: this code is specific to ARM
  // On entry the stack pointed by sp is:
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | LR         |
  // | ...        |    callee saves
  // | R3         |    arg3
  // | R2         |    arg2
  // | R1         |    arg1
  // | R0         |
  // | Method*    |  <- sp
  uintptr_t* regs = reinterpret_cast<uintptr_t*>(reinterpret_cast<byte*>(sp) + kPointerSize);
  DCHECK_EQ(48U, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
  Method** caller_sp = reinterpret_cast<Method**>(reinterpret_cast<byte*>(sp) + 48);
  uintptr_t caller_pc = regs[10];
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kRefsAndArgs);
  // Start new JNI local reference state
  JNIEnvExt* env = thread->GetJniEnv();
  ScopedJniEnvLocalRefState env_state(env);
  // Discover shorty (avoid GCs)
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  const char* shorty = linker->MethodShorty(method_idx, *caller_sp);
  size_t shorty_len = strlen(shorty);
  size_t args_in_regs = 0;
  for (size_t i = 1; i < shorty_len; i++) {
    char c = shorty[i];
    args_in_regs = args_in_regs + (c == 'J' || c == 'D' ? 2 : 1);
    if (args_in_regs > 3) {
      args_in_regs = 3;
      break;
    }
  }
  bool is_static;
  if (type == Runtime::kUnknownMethod) {
    Method* caller = *caller_sp;
    // less two as return address may span into next dex instruction
    uint32_t dex_pc = caller->ToDexPC(caller_pc - 2);
    const DexFile::CodeItem* code = MethodHelper(caller).GetCodeItem();
    CHECK_LT(dex_pc, code->insns_size_in_code_units_);
    const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
    Instruction::Code instr_code = instr->Opcode();
    is_static = (instr_code == Instruction::INVOKE_STATIC) ||
                (instr_code == Instruction::INVOKE_STATIC_RANGE);
    DCHECK(is_static || (instr_code == Instruction::INVOKE_DIRECT) ||
           (instr_code == Instruction::INVOKE_DIRECT_RANGE));
  } else {
    is_static = type == Runtime::kStaticMethod;
  }
  // Place into local references incoming arguments from the caller's register arguments
  size_t cur_arg = 1;   // skip method_idx in R0, first arg is in R1
  if (!is_static) {
    Object* obj = reinterpret_cast<Object*>(regs[cur_arg]);
    cur_arg++;
    if (args_in_regs < 3) {
      // If we thought we had fewer than 3 arguments in registers, account for the receiver
      args_in_regs++;
    }
    AddLocalReference<jobject>(env, obj);
  }
  size_t shorty_index = 1;  // skip return value
  // Iterate while arguments and arguments in registers (less 1 from cur_arg which is offset to skip
  // R0)
  while ((cur_arg - 1) < args_in_regs && shorty_index < shorty_len) {
    char c = shorty[shorty_index];
    shorty_index++;
    if (c == 'L') {
      Object* obj = reinterpret_cast<Object*>(regs[cur_arg]);
      AddLocalReference<jobject>(env, obj);
    }
    cur_arg = cur_arg + (c == 'J' || c == 'D' ? 2 : 1);
  }
  // Place into local references incoming arguments from the caller's stack arguments
  cur_arg += 11;  // skip LR, Method* and spills for R1 to R3 and callee saves
  while (shorty_index < shorty_len) {
    char c = shorty[shorty_index];
    shorty_index++;
    if (c == 'L') {
      Object* obj = reinterpret_cast<Object*>(regs[cur_arg]);
      AddLocalReference<jobject>(env, obj);
    }
    cur_arg = cur_arg + (c == 'J' || c == 'D' ? 2 : 1);
  }
  // Resolve method filling in dex cache
  Method* called = linker->ResolveMethod(method_idx, *caller_sp, true);
  if (LIKELY(!thread->IsExceptionPending())) {
    if (LIKELY(called->IsDirect())) {
      // Ensure that the called method's class is initialized
      Class* called_class = called->GetDeclaringClass();
      linker->EnsureInitialized(called_class, true);
      if (LIKELY(called_class->IsInitialized())) {
        // Update CodeAndDirectMethod table and avoid the trampoline when we know the called class
        // is initialized (see test 084-class-init SlowInit)
        Method* caller = *caller_sp;
        DexCache* dex_cache = caller->GetDeclaringClass()->GetDexCache();
        dex_cache->GetCodeAndDirectMethods()->SetResolvedDirectMethod(method_idx, called);
        // We got this far, ensure that the declaring class is initialized
        linker->EnsureInitialized(called->GetDeclaringClass(), true);
      }
    } else {
      // Direct method has been made virtual
      thread->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
                                 "Expected direct method but found virtual: %s",
                                 PrettyMethod(called, true).c_str());
    }
  }
  void* code;
  if (UNLIKELY(thread->IsExceptionPending())) {
    // Something went wrong in ResolveMethod or EnsureInitialized,
    // go into deliver exception with the pending exception in r0
    code = reinterpret_cast<void*>(art_deliver_exception_from_code);
    regs[0] = reinterpret_cast<uintptr_t>(thread->GetException());
    thread->ClearException();
  } else {
    // Expect class to at least be initializing
    DCHECK(called->GetDeclaringClass()->IsInitializing());
    // Set up entry into main method
    regs[0] = reinterpret_cast<uintptr_t>(called);
    code = const_cast<void*>(called->GetCode());
  }
  return code;
}

// Fast path field resolution that can't throw exceptions
static Field* FindFieldFast(uint32_t field_idx, const Method* referrer) {
  Field* resolved_field = referrer->GetDexCacheResolvedFields()->Get(field_idx);
  if (UNLIKELY(resolved_field == NULL)) {
    return NULL;
  }
  Class* fields_class = resolved_field->GetDeclaringClass();
  // Check class is initilaized or initializing
  if (UNLIKELY(!fields_class->IsInitializing())) {
    return NULL;
  }
  return resolved_field;
}

// Slow path field resolution and declaring class initialization
Field* FindFieldFromCode(uint32_t field_idx, const Method* referrer, bool is_static) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Field* resolved_field = class_linker->ResolveField(field_idx, referrer, is_static);
  if (LIKELY(resolved_field != NULL)) {
    Class* fields_class = resolved_field->GetDeclaringClass();
    // If the class is already initializing, we must be inside <clinit>, or
    // we'd still be waiting for the lock.
    if (fields_class->IsInitializing()) {
      return resolved_field;
    }
    if (Runtime::Current()->GetClassLinker()->EnsureInitialized(fields_class, true)) {
      return resolved_field;
    }
  }
  DCHECK(Thread::Current()->IsExceptionPending()); // Throw exception and unwind
  return NULL;
}

extern "C" Field* artFindInstanceFieldFromCode(uint32_t field_idx, const Method* referrer,
                                               Thread* self, Method** sp) {
  Field* resolved_field = FindFieldFast(field_idx, referrer);
  if (UNLIKELY(resolved_field == NULL)) {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
    resolved_field = FindFieldFromCode(field_idx, referrer, false);
  }
  return resolved_field;
}

extern "C" uint32_t artGet32StaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer);
  if (LIKELY(field != NULL)) {
    FieldHelper fh(field);
    if (LIKELY(fh.IsPrimitiveType() && fh.FieldSize() == sizeof(int32_t))) {
      return field->Get32(NULL);
    }
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    FieldHelper fh(field);
    if (!fh.IsPrimitiveType() || fh.FieldSize() != sizeof(int32_t)) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted read of 32-bit primitive on field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      return field->Get32(NULL);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" uint64_t artGet64StaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer);
  if (LIKELY(field != NULL)) {
    FieldHelper fh(field);
    if (LIKELY(fh.IsPrimitiveType() && fh.FieldSize() == sizeof(int64_t))) {
      return field->Get64(NULL);
    }
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    FieldHelper fh(field);
    if (!fh.IsPrimitiveType() || fh.FieldSize() != sizeof(int64_t)) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted read of 64-bit primitive on field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      return field->Get64(NULL);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" Object* artGetObjStaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer);
  if (LIKELY(field != NULL)) {
    FieldHelper fh(field);
    if (LIKELY(!fh.IsPrimitiveType())) {
      return field->GetObj(NULL);
    }
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    FieldHelper fh(field);
    if (fh.IsPrimitiveType()) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted read of reference on primitive field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      return field->GetObj(NULL);
    }
  }
  return NULL;  // Will throw exception by checking with Thread::Current
}

extern "C" int artSet32StaticFromCode(uint32_t field_idx, const Method* referrer,
                                       uint32_t new_value, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer);
  if (LIKELY(field != NULL)) {
    FieldHelper fh(field);
    if (LIKELY(fh.IsPrimitiveType() && fh.FieldSize() == sizeof(int32_t))) {
      field->Set32(NULL, new_value);
      return 0;  // success
    }
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    FieldHelper fh(field);
    if (!fh.IsPrimitiveType() || fh.FieldSize() != sizeof(int32_t)) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted write of 32-bit primitive to field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      field->Set32(NULL, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSet64StaticFromCode(uint32_t field_idx, const Method* referrer,
                                      uint64_t new_value, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer);
  if (LIKELY(field != NULL)) {
    FieldHelper fh(field);
    if (LIKELY(fh.IsPrimitiveType() && fh.FieldSize() == sizeof(int64_t))) {
      field->Set64(NULL, new_value);
      return 0;  // success
    }
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, true);
  if (LIKELY(field != NULL)) {
    FieldHelper fh(field);
    if (UNLIKELY(!fh.IsPrimitiveType() || fh.FieldSize() != sizeof(int64_t))) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted write of 64-bit primitive to field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      field->Set64(NULL, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSetObjStaticFromCode(uint32_t field_idx, const Method* referrer,
                                       Object* new_value, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer);
  if (LIKELY(field != NULL)) {
    if (LIKELY(!FieldHelper(field).IsPrimitiveType())) {
      field->SetObj(NULL, new_value);
      return 0;  // success
    }
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    if (FieldHelper(field).IsPrimitiveType()) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted write of reference to primitive field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      field->SetObj(NULL, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

// Given the context of a calling Method, use its DexCache to resolve a type to a Class. If it
// cannot be resolved, throw an error. If it can, use it to create an instance.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
static Object* AllocObjectFromCode(uint32_t type_idx, Method* method, Thread* self,
                                   bool access_check) {
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  Runtime* runtime = Runtime::Current();
  if (UNLIKELY(klass == NULL)) {
    klass = runtime->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {
      DCHECK(self->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (access_check) {
    Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                               "illegal class access: '%s' -> '%s'",
                               PrettyDescriptor(referrer).c_str(),
                               PrettyDescriptor(klass).c_str());
      return NULL;  // Failure
    }
  }
  if (!runtime->GetClassLinker()->EnsureInitialized(klass, true)) {
    DCHECK(self->IsExceptionPending());
    return NULL;  // Failure
  }
  return klass->AllocObject();
}

extern "C" Object* artAllocObjectFromCode(uint32_t type_idx, Method* method,
                                          Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return AllocObjectFromCode(type_idx, method, self, false);
}

extern "C" Object* artAllocObjectFromCodeWithAccessCheck(uint32_t type_idx, Method* method,
                                                         Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return AllocObjectFromCode(type_idx, method, self, true);
}

// Given the context of a calling Method, use its DexCache to resolve a type to an array Class. If
// it cannot be resolved, throw an error. If it can, use it to create an array.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
static Array* AllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
                                  Thread* self, bool access_check) {
  if (UNLIKELY(component_count < 0)) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d",
                                         component_count);
    return NULL;  // Failure
  }
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (UNLIKELY(klass == NULL)) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
    CHECK(klass->IsArrayClass()) << PrettyClass(klass);
  }
  if (access_check) {
    Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                               "illegal class access: '%s' -> '%s'",
                               PrettyDescriptor(referrer).c_str(),
                               PrettyDescriptor(klass).c_str());
      return NULL;  // Failure
    }
  }
  return Array::Alloc(klass, component_count);
}

extern "C" Array* artAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
                                        Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return AllocArrayFromCode(type_idx, method, component_count, self, false);
}

extern "C" Array* artAllocArrayFromCodeWithAccessCheck(uint32_t type_idx, Method* method,
                                                       int32_t component_count,
                                                       Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return AllocArrayFromCode(type_idx, method, component_count, self, true);
}

// Helper function to alloc array for OP_FILLED_NEW_ARRAY
Array* CheckAndAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
                                  Thread* self, bool access_check) {
  if (UNLIKELY(component_count < 0)) {
    self->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", component_count);
    return NULL;  // Failure
  }
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (UNLIKELY(klass == NULL)) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (UNLIKELY(klass->IsPrimitive() && !klass->IsPrimitiveInt())) {
    if (klass->IsPrimitiveLong() || klass->IsPrimitiveDouble()) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
          "Bad filled array request for type %s",
          PrettyDescriptor(klass).c_str());
    } else {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/InternalError;",
          "Found type %s; filled-new-array not implemented for anything but \'int\'",
          PrettyDescriptor(klass).c_str());
    }
    return NULL;  // Failure
  } else {
    if (access_check) {
      Class* referrer = method->GetDeclaringClass();
      if (UNLIKELY(!referrer->CanAccess(klass))) {
        self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                                 "illegal class access: '%s' -> '%s'",
                                 PrettyDescriptor(referrer).c_str(),
                                 PrettyDescriptor(klass).c_str());
        return NULL;  // Failure
      }
    }
    DCHECK(klass->IsArrayClass()) << PrettyClass(klass);
    return Array::Alloc(klass, component_count);
  }
}

extern "C" Array* artCheckAndAllocArrayFromCode(uint32_t type_idx, Method* method,
                                               int32_t component_count, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return CheckAndAllocArrayFromCode(type_idx, method, component_count, self, false);
}

extern "C" Array* artCheckAndAllocArrayFromCodeWithAccessCheck(uint32_t type_idx, Method* method,
                                                               int32_t component_count,
                                                               Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return CheckAndAllocArrayFromCode(type_idx, method, component_count, self, true);
}

// Assignable test for code, won't throw.  Null and equality tests already performed
uint32_t IsAssignableFromCode(const Class* klass, const Class* ref_class) {
  DCHECK(klass != NULL);
  DCHECK(ref_class != NULL);
  return klass->IsAssignableFrom(ref_class) ? 1 : 0;
}

// Check whether it is safe to cast one class to the other, throw exception and return -1 on failure
extern "C" int artCheckCastFromCode(const Class* a, const Class* b, Thread* self, Method** sp) {
  DCHECK(a->IsClass()) << PrettyClass(a);
  DCHECK(b->IsClass()) << PrettyClass(b);
  if (LIKELY(b->IsAssignableFrom(a))) {
    return 0;  // Success
  } else {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ClassCastException;",
        "%s cannot be cast to %s",
        PrettyDescriptor(a).c_str(),
        PrettyDescriptor(b).c_str());
    return -1;  // Failure
  }
}

// Tests whether 'element' can be assigned into an array of type 'array_class'.
// Returns 0 on success and -1 if an exception is pending.
extern "C" int artCanPutArrayElementFromCode(const Object* element, const Class* array_class,
                                             Thread* self, Method** sp) {
  DCHECK(array_class != NULL);
  // element can't be NULL as we catch this is screened in runtime_support
  Class* element_class = element->GetClass();
  Class* component_type = array_class->GetComponentType();
  if (LIKELY(component_type->IsAssignableFrom(element_class))) {
    return 0;  // Success
  } else {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
        "%s cannot be stored in an array of type %s",
        PrettyDescriptor(element_class).c_str(),
        PrettyDescriptor(array_class).c_str());
    return -1;  // Failure
  }
}

Class* ResolveVerifyAndClinit(uint32_t type_idx, const Method* referrer, Thread* self,
                               bool can_run_clinit, bool verify_access) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* klass = class_linker->ResolveType(type_idx, referrer);
  if (UNLIKELY(klass == NULL)) {
    CHECK(self->IsExceptionPending());
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  // Perform access check if necessary.
  if (verify_access && !referrer->GetDeclaringClass()->CanAccess(klass)) {
    self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                             "Class %s is inaccessible to method %s",
                             PrettyDescriptor(klass).c_str(),
                             PrettyMethod(referrer, true).c_str());
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  // If we're just implementing const-class, we shouldn't call <clinit>.
  if (!can_run_clinit) {
    return klass;
  }
  // If we are the <clinit> of this class, just return our storage.
  //
  // Do not set the DexCache InitializedStaticStorage, since that implies <clinit> has finished
  // running.
  if (klass == referrer->GetDeclaringClass() && MethodHelper(referrer).IsClassInitializer()) {
    return klass;
  }
  if (!class_linker->EnsureInitialized(klass, true)) {
    CHECK(self->IsExceptionPending());
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  referrer->GetDexCacheInitializedStaticStorage()->Set(type_idx, klass);
  return klass;
}

extern "C" Class* artInitializeStaticStorageFromCode(uint32_t type_idx, const Method* referrer,
                                                     Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return ResolveVerifyAndClinit(type_idx, referrer, self, true, true);
}

extern "C" Class* artInitializeTypeFromCode(uint32_t type_idx, const Method* referrer, Thread* self,
                                            Method** sp) {
  // Called when method->dex_cache_resolved_types_[] misses
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return ResolveVerifyAndClinit(type_idx, referrer, self, false, false);
}

extern "C" Class* artInitializeTypeAndVerifyAccessFromCode(uint32_t type_idx,
                                                           const Method* referrer, Thread* self,
                                                           Method** sp) {
  // Called when caller isn't guaranteed to have access to a type and the dex cache may be
  // unpopulated
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return ResolveVerifyAndClinit(type_idx, referrer, self, false, true);
}

// Helper function to resolve virtual method
extern "C" Method* artResolveMethodFromCode(Method* referrer,
                                            uint32_t method_idx,
                                            bool is_direct,
                                            Thread* self,
                                            Method** sp) {
    /*
     * Slow-path handler on invoke virtual method path in which
     * base method is unresolved at compile-time.  Caller will
     * unwind if can't resolve.
     */
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    Method* method = class_linker->ResolveMethod(method_idx, referrer, is_direct);
    referrer->GetDexCacheResolvedMethods()->Set(method_idx, method);
    return method;
}

String* ResolveStringFromCode(const Method* referrer, uint32_t string_idx) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->ResolveString(string_idx, referrer);
}

extern "C" String* artResolveStringFromCode(Method* referrer, int32_t string_idx,
                                            Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return ResolveStringFromCode(referrer, string_idx);
}

extern "C" int artUnlockObjectFromCode(Object* obj, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  DCHECK(obj != NULL);  // Assumed to have been checked before entry
  // MonitorExit may throw exception
  return obj->MonitorExit(self) ? 0 /* Success */ : -1 /* Failure */;
}

extern "C" void artLockObjectFromCode(Object* obj, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kRefsOnly);
  DCHECK(obj != NULL);        // Assumed to have been checked before entry
  obj->MonitorEnter(thread);  // May block
  DCHECK(thread->HoldsLock(obj));
  // Only possible exception is NPE and is handled before entry
  DCHECK(!thread->IsExceptionPending());
}

void CheckSuspendFromCode(Thread* thread) {
  // Called when thread->suspend_count_ != 0
  Runtime::Current()->GetThreadList()->FullSuspendCheck(thread);
}

extern "C" void artTestSuspendFromCode(Thread* thread, Method** sp) {
  // Called when suspend count check value is 0 and thread->suspend_count_ != 0
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kRefsOnly);
  Runtime::Current()->GetThreadList()->FullSuspendCheck(thread);
}

/*
 * Fill the array with predefined constant values, throwing exceptions if the array is null or
 * not of sufficient length.
 *
 * NOTE: When dealing with a raw dex file, the data to be copied uses
 * little-endian ordering.  Require that oat2dex do any required swapping
 * so this routine can get by with a memcpy().
 *
 * Format of the data:
 *  ushort ident = 0x0300   magic value
 *  ushort width            width of each element in the table
 *  uint   size             number of elements in the table
 *  ubyte  data[size*width] table of data values (may contain a single-byte
 *                          padding at the end)
 */
extern "C" int artHandleFillArrayDataFromCode(Array* array, const uint16_t* table,
                                              Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  DCHECK_EQ(table[0], 0x0300);
  if (UNLIKELY(array == NULL)) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
        "null array in fill array");
    return -1;  // Error
  }
  DCHECK(array->IsArrayInstance() && !array->IsObjectArray());
  uint32_t size = (uint32_t)table[2] | (((uint32_t)table[3]) << 16);
  if (UNLIKELY(static_cast<int32_t>(size) > array->GetLength())) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
        "failed array fill. length=%d; index=%d", array->GetLength(), size);
    return -1;  // Error
  }
  uint16_t width = table[1];
  uint32_t size_in_bytes = size * width;
  memcpy((char*)array + Array::DataOffset().Int32Value(), (char*)&table[4], size_in_bytes);
  return 0;  // Success
}

// See comments in runtime_support_asm.S
extern "C" uint64_t artFindInterfaceMethodInCacheFromCode(uint32_t method_idx,
                                                          Object* this_object,
                                                          Method* caller_method,
                                                          Thread* thread, Method** sp) {
  Method* interface_method = caller_method->GetDexCacheResolvedMethods()->Get(method_idx);
  Method* found_method = NULL;  // The found method
  if (LIKELY(interface_method != NULL && this_object != NULL)) {
    found_method = this_object->GetClass()->FindVirtualMethodForInterface(interface_method, false);
  }
  if (UNLIKELY(found_method == NULL)) {
    FinishCalleeSaveFrameSetup(thread, sp, Runtime::kRefsAndArgs);
    if (this_object == NULL) {
      thread->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
          "null receiver during interface dispatch");
      return 0;
    }
    if (interface_method == NULL) {
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      interface_method = class_linker->ResolveMethod(method_idx, caller_method, false);
      if (interface_method == NULL) {
        // Could not resolve interface method. Throw error and unwind
        CHECK(thread->IsExceptionPending());
        return 0;
      }
    }
    found_method = this_object->GetClass()->FindVirtualMethodForInterface(interface_method, true);
    if (found_method == NULL) {
      CHECK(thread->IsExceptionPending());
      return 0;
    }
  }
  const void* code = found_method->GetCode();

  uint32_t method_uint = reinterpret_cast<uint32_t>(found_method);
  uint64_t code_uint = reinterpret_cast<uint32_t>(code);
  uint64_t result = ((code_uint << 32) | method_uint);
  return result;
}

static void ThrowNewUndeclaredThrowableException(Thread* self, JNIEnv* env, Throwable* exception) {
  ScopedLocalRef<jclass> jlr_UTE_class(env,
      env->FindClass("java/lang/reflect/UndeclaredThrowableException"));
  if (jlr_UTE_class.get() == NULL) {
    LOG(ERROR) << "Couldn't throw new \"java/lang/reflect/UndeclaredThrowableException\"";
  } else {
    jmethodID jlre_UTE_constructor = env->GetMethodID(jlr_UTE_class.get(), "<init>",
                                                      "(Ljava/lang/Throwable;)V");
    jthrowable jexception = AddLocalReference<jthrowable>(env, exception);
    ScopedLocalRef<jthrowable> jlr_UTE(env,
        reinterpret_cast<jthrowable>(env->NewObject(jlr_UTE_class.get(), jlre_UTE_constructor,
                                                    jexception)));
    int rc = env->Throw(jlr_UTE.get());
    if (rc != JNI_OK) {
      LOG(ERROR) << "Couldn't throw new \"java/lang/reflect/UndeclaredThrowableException\"";
    }
  }
  CHECK(self->IsExceptionPending());
}

// Handler for invocation on proxy methods. On entry a frame will exist for the proxy object method
// which is responsible for recording callee save registers. We explicitly handlerize incoming
// reference arguments (so they survive GC) and create a boxed argument array. Finally we invoke
// the invocation handler which is a field within the proxy object receiver.
extern "C" void artProxyInvokeHandler(Method* proxy_method, Object* receiver,
                                      Thread* self, byte* stack_args) {
  // Register the top of the managed stack
  Method** proxy_sp = reinterpret_cast<Method**>(stack_args - 12);
  DCHECK_EQ(*proxy_sp, proxy_method);
  self->SetTopOfStack(proxy_sp, 0);
  // TODO: ARM specific
  DCHECK_EQ(proxy_method->GetFrameSizeInBytes(), 48u);
  // Start new JNI local reference state
  JNIEnvExt* env = self->GetJniEnv();
  ScopedJniEnvLocalRefState env_state(env);
  // Create local ref. copies of proxy method and the receiver
  jobject rcvr_jobj = AddLocalReference<jobject>(env, receiver);
  jobject proxy_method_jobj = AddLocalReference<jobject>(env, proxy_method);

  // Placing into local references incoming arguments from the caller's register arguments,
  // replacing original Object* with jobject
  MethodHelper proxy_mh(proxy_method);
  const size_t num_params = proxy_mh.NumArgs();
  size_t args_in_regs = 0;
  for (size_t i = 1; i < num_params; i++) {  // skip receiver
    args_in_regs = args_in_regs + (proxy_mh.IsParamALongOrDouble(i) ? 2 : 1);
    if (args_in_regs > 2) {
      args_in_regs = 2;
      break;
    }
  }
  size_t cur_arg = 0;  // current stack location to read
  size_t param_index = 1;  // skip receiver
  while (cur_arg < args_in_regs && param_index < num_params) {
    if (proxy_mh.IsParamAReference(param_index)) {
      Object* obj = *reinterpret_cast<Object**>(stack_args + (cur_arg * kPointerSize));
      jobject jobj = AddLocalReference<jobject>(env, obj);
      *reinterpret_cast<jobject*>(stack_args + (cur_arg * kPointerSize)) = jobj;
    }
    cur_arg = cur_arg + (proxy_mh.IsParamALongOrDouble(param_index) ? 2 : 1);
    param_index++;
  }
  // Placing into local references incoming arguments from the caller's stack arguments
  cur_arg += 11;  // skip callee saves, LR, Method* and out arg spills for R1 to R3
  while (param_index < num_params) {
    if (proxy_mh.IsParamAReference(param_index)) {
      Object* obj = *reinterpret_cast<Object**>(stack_args + (cur_arg * kPointerSize));
      jobject jobj = AddLocalReference<jobject>(env, obj);
      *reinterpret_cast<jobject*>(stack_args + (cur_arg * kPointerSize)) = jobj;
    }
    cur_arg = cur_arg + (proxy_mh.IsParamALongOrDouble(param_index) ? 2 : 1);
    param_index++;
  }
  // Set up arguments array and place in local IRT during boxing (which may allocate/GC)
  jvalue args_jobj[3];
  args_jobj[0].l = rcvr_jobj;
  args_jobj[1].l = proxy_method_jobj;
  // Args array, if no arguments then NULL (don't include receiver in argument count)
  args_jobj[2].l = NULL;
  ObjectArray<Object>* args = NULL;
  if ((num_params - 1) > 0) {
    args = Runtime::Current()->GetClassLinker()->AllocObjectArray<Object>(num_params - 1);
    if (args == NULL) {
      CHECK(self->IsExceptionPending());
      return;
    }
    args_jobj[2].l = AddLocalReference<jobjectArray>(env, args);
  }
  // Convert proxy method into expected interface method
  Method* interface_method = proxy_method->FindOverriddenMethod();
  CHECK(interface_method != NULL);
  CHECK(!interface_method->IsProxyMethod()) << PrettyMethod(interface_method);
  args_jobj[1].l = AddLocalReference<jobject>(env, interface_method);
  // Box arguments
  cur_arg = 0;  // reset stack location to read to start
  // reset index, will index into param type array which doesn't include the receiver
  param_index = 0;
  ObjectArray<Class>* param_types = proxy_mh.GetParameterTypes();
  CHECK(param_types != NULL);
  // Check number of parameter types agrees with number from the Method - less 1 for the receiver.
  CHECK_EQ(static_cast<size_t>(param_types->GetLength()), num_params - 1);
  while (cur_arg < args_in_regs && param_index < (num_params - 1)) {
    Class* param_type = param_types->Get(param_index);
    Object* obj;
    if (!param_type->IsPrimitive()) {
      obj = self->DecodeJObject(*reinterpret_cast<jobject*>(stack_args + (cur_arg * kPointerSize)));
    } else {
      JValue val = *reinterpret_cast<JValue*>(stack_args + (cur_arg * kPointerSize));
      if (cur_arg == 1 && (param_type->IsPrimitiveLong() || param_type->IsPrimitiveDouble())) {
        // long/double split over regs and stack, mask in high half from stack arguments
        uint64_t high_half = *reinterpret_cast<uint32_t*>(stack_args + (13 * kPointerSize));
        val.j = (val.j & 0xffffffffULL) | (high_half << 32);
      }
      BoxPrimitive(env, param_type->GetPrimitiveType(), val);
      if (self->IsExceptionPending()) {
        return;
      }
      obj = val.l;
    }
    args->Set(param_index, obj);
    cur_arg = cur_arg + (param_type->IsPrimitiveLong() || param_type->IsPrimitiveDouble() ? 2 : 1);
    param_index++;
  }
  // Placing into local references incoming arguments from the caller's stack arguments
  cur_arg += 11;  // skip callee saves, LR, Method* and out arg spills for R1 to R3
  while (param_index < (num_params - 1)) {
    Class* param_type = param_types->Get(param_index);
    Object* obj;
    if (!param_type->IsPrimitive()) {
      obj = self->DecodeJObject(*reinterpret_cast<jobject*>(stack_args + (cur_arg * kPointerSize)));
    } else {
      JValue val = *reinterpret_cast<JValue*>(stack_args + (cur_arg * kPointerSize));
      BoxPrimitive(env, param_type->GetPrimitiveType(), val);
      if (self->IsExceptionPending()) {
        return;
      }
      obj = val.l;
    }
    args->Set(param_index, obj);
    cur_arg = cur_arg + (param_type->IsPrimitiveLong() || param_type->IsPrimitiveDouble() ? 2 : 1);
    param_index++;
  }
  // Get the InvocationHandler method and the field that holds it within the Proxy object
  static jmethodID inv_hand_invoke_mid = NULL;
  static jfieldID proxy_inv_hand_fid = NULL;
  if (proxy_inv_hand_fid == NULL) {
    ScopedLocalRef<jclass> proxy(env, env->FindClass("java/lang/reflect/Proxy"));
    proxy_inv_hand_fid = env->GetFieldID(proxy.get(), "h", "Ljava/lang/reflect/InvocationHandler;");
    ScopedLocalRef<jclass> inv_hand_class(env, env->FindClass("java/lang/reflect/InvocationHandler"));
    inv_hand_invoke_mid = env->GetMethodID(inv_hand_class.get(), "invoke",
        "(Ljava/lang/Object;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;");
  }
  DCHECK(env->IsInstanceOf(rcvr_jobj, env->FindClass("java/lang/reflect/Proxy")));
  jobject inv_hand = env->GetObjectField(rcvr_jobj, proxy_inv_hand_fid);
  // Call InvocationHandler.invoke
  jobject result = env->CallObjectMethodA(inv_hand, inv_hand_invoke_mid, args_jobj);
  // Place result in stack args
  if (!self->IsExceptionPending()) {
    Object* result_ref = self->DecodeJObject(result);
    if (result_ref != NULL) {
      JValue result_unboxed;
      UnboxPrimitive(env, result_ref, proxy_mh.GetReturnType(), result_unboxed);
      *reinterpret_cast<JValue*>(stack_args) = result_unboxed;
    } else {
      *reinterpret_cast<jobject*>(stack_args) = NULL;
    }
  } else {
    // In the case of checked exceptions that aren't declared, the exception must be wrapped by
    // a UndeclaredThrowableException.
    Throwable* exception = self->GetException();
    self->ClearException();
    if (!exception->IsCheckedException()) {
      self->SetException(exception);
    } else {
      SynthesizedProxyClass* proxy_class =
          down_cast<SynthesizedProxyClass*>(proxy_method->GetDeclaringClass());
      int throws_index = -1;
      size_t num_virt_methods = proxy_class->NumVirtualMethods();
      for (size_t i = 0; i < num_virt_methods; i++) {
        if (proxy_class->GetVirtualMethod(i) == proxy_method) {
          throws_index = i;
          break;
        }
      }
      CHECK_NE(throws_index, -1);
      ObjectArray<Class>* declared_exceptions = proxy_class->GetThrows()->Get(throws_index);
      Class* exception_class = exception->GetClass();
      bool declares_exception = false;
      for (int i = 0; i < declared_exceptions->GetLength() && !declares_exception; i++) {
        Class* declared_exception = declared_exceptions->Get(i);
        declares_exception = declared_exception->IsAssignableFrom(exception_class);
      }
      if (declares_exception) {
        self->SetException(exception);
      } else {
        ThrowNewUndeclaredThrowableException(self, env, exception);
      }
    }
  }
}

extern "C" const void* artTraceMethodEntryFromCode(Method* method, Thread* self, uintptr_t lr) {
  Trace* tracer = Runtime::Current()->GetTracer();
  TraceStackFrame trace_frame = TraceStackFrame(method, lr);
  self->PushTraceStackFrame(trace_frame);

  tracer->LogMethodTraceEvent(self, method, Trace::kMethodTraceEnter);

  return tracer->GetSavedCodeFromMap(method);
}

extern "C" uintptr_t artTraceMethodExitFromCode() {
  Trace* tracer = Runtime::Current()->GetTracer();
  TraceStackFrame trace_frame = Thread::Current()->PopTraceStackFrame();
  Method* method = trace_frame.method_;
  uintptr_t lr = trace_frame.return_pc_;

  tracer->LogMethodTraceEvent(Thread::Current(), method, Trace::kMethodTraceExit);

  return lr;
}

uint32_t artTraceMethodUnwindFromCode(Thread* self) {
  Trace* tracer = Runtime::Current()->GetTracer();
  TraceStackFrame trace_frame = self->PopTraceStackFrame();
  Method* method = trace_frame.method_;
  uint32_t lr = trace_frame.return_pc_;

  tracer->LogMethodTraceEvent(self, method, Trace::kMethodTraceUnwind);

  return lr;
}

/*
 * Float/double conversion requires clamping to min and max of integer form.  If
 * target doesn't support this normally, use these.
 */
int64_t D2L(double d) {
    static const double kMaxLong = (double)(int64_t)0x7fffffffffffffffULL;
    static const double kMinLong = (double)(int64_t)0x8000000000000000ULL;
    if (d >= kMaxLong)
        return (int64_t)0x7fffffffffffffffULL;
    else if (d <= kMinLong)
        return (int64_t)0x8000000000000000ULL;
    else if (d != d) // NaN case
        return 0;
    else
        return (int64_t)d;
}

int64_t F2L(float f) {
    static const float kMaxLong = (float)(int64_t)0x7fffffffffffffffULL;
    static const float kMinLong = (float)(int64_t)0x8000000000000000ULL;
    if (f >= kMaxLong)
        return (int64_t)0x7fffffffffffffffULL;
    else if (f <= kMinLong)
        return (int64_t)0x8000000000000000ULL;
    else if (f != f) // NaN case
        return 0;
    else
        return (int64_t)f;
}

}  // namespace art
