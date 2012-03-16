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

#include "debugger.h"
#include "dex_cache.h"
#include "dex_verifier.h"
#include "macros.h"
#include "object.h"
#include "object_utils.h"
#include "reflection.h"
#include "runtime_support_common.h"
#include "trace.h"
#include "ScopedLocalRef.h"

namespace art {

// Place a special frame at the TOS that will save the callee saves for the given type
static void  FinishCalleeSaveFrameSetup(Thread* self, Method** sp, Runtime::CalleeSaveType type) {
  // Be aware the store below may well stomp on an incoming argument
  *sp = Runtime::Current()->GetCalleeSaveMethod(type);
  self->SetTopOfStack(sp, 0);
}

/*
 * Report location to debugger.  Note: dex_pc is the current offset within
 * the method.  However, because the offset alone cannot distinguish between
 * method entry and offset 0 within the method, we'll use an offset of -1
 * to denote method entry.
 */
extern "C" void artUpdateDebuggerFromCode(int32_t dex_pc, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp,  Runtime::kRefsAndArgs);
  Dbg::UpdateDebugger(dex_pc, self, sp);
}

// Temporary debugging hook for compiler.
extern void DebugMe(Method* method, uint32_t info) {
  LOG(INFO) << "DebugMe";
  if (method != NULL) {
    LOG(INFO) << PrettyMethod(method);
  }
  LOG(INFO) << "Info: " << info;
}

// Return value helper for jobject return types
extern Object* DecodeJObjectInThread(Thread* thread, jobject obj) {
  if (thread->IsExceptionPending()) {
    return NULL;
  }
  return thread->DecodeJObject(obj);
}

extern void* FindNativeMethod(Thread* self) {
  DCHECK(Thread::Current() == self);

  Method* method = const_cast<Method*>(self->GetCurrentMethod());
  DCHECK(method != NULL);

  // Lookup symbol address for method, on failure we'll return NULL with an
  // exception set, otherwise we return the address of the method we found.
  void* native_code = self->GetJniEnv()->vm->FindCodeForNativeMethod(method);
  if (native_code == NULL) {
    DCHECK(self->IsExceptionPending());
    return NULL;
  } else {
    // Register so that future calls don't come here
    method->RegisterNative(self, native_code);
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
extern "C" void artThrowNullPointerExceptionFromCode(Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kSaveAll);
  Frame fr = self->GetTopOfStack();
  uintptr_t throw_native_pc = fr.GetReturnPC();
  fr.Next();
  Method* throw_method = fr.GetMethod();
  uint32_t dex_pc = throw_method->ToDexPC(throw_native_pc - 2);
  const DexFile::CodeItem* code = MethodHelper(throw_method).GetCodeItem();
  CHECK_LT(dex_pc, code->insns_size_in_code_units_);
  const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
  DecodedInstruction dec_insn(instr);
  switch (instr->Opcode()) {
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
      ThrowNullPointerExceptionForMethodAccess(self, throw_method, dec_insn.vB, kDirect);
      break;
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
      ThrowNullPointerExceptionForMethodAccess(self, throw_method, dec_insn.vB, kVirtual);
      break;
    case Instruction::IGET:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_OBJECT:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT: {
      Field* field =
          Runtime::Current()->GetClassLinker()->ResolveField(dec_insn.vC, throw_method, false);
      ThrowNullPointerExceptionForFieldAccess(self, field, true /* read */);
      break;
    }
    case Instruction::IPUT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT: {
      Field* field =
          Runtime::Current()->GetClassLinker()->ResolveField(dec_insn.vC, throw_method, false);
      ThrowNullPointerExceptionForFieldAccess(self, field, false /* write */);
      break;
    }
    case Instruction::AGET:
    case Instruction::AGET_WIDE:
    case Instruction::AGET_OBJECT:
    case Instruction::AGET_BOOLEAN:
    case Instruction::AGET_BYTE:
    case Instruction::AGET_CHAR:
    case Instruction::AGET_SHORT:
      self->ThrowNewException("Ljava/lang/NullPointerException;",
                              "Attempt to read from null array");
      break;
    case Instruction::APUT:
    case Instruction::APUT_WIDE:
    case Instruction::APUT_OBJECT:
    case Instruction::APUT_BOOLEAN:
    case Instruction::APUT_BYTE:
    case Instruction::APUT_CHAR:
    case Instruction::APUT_SHORT:
      self->ThrowNewException("Ljava/lang/NullPointerException;",
                              "Attempt to write to null array");
      break;
    default: {
      const DexFile& dex_file = Runtime::Current()->GetClassLinker()
          ->FindDexFile(throw_method->GetDeclaringClass()->GetDexCache());
      std::string message("Null pointer exception during instruction '");
      message += instr->DumpString(&dex_file);
      message += "'";
      self->ThrowNewException("Ljava/lang/NullPointerException;", message.c_str());
      break;
    }
  }
  self->DeliverException();
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
    TraceMethodUnwindFromCode(thread);
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

const void* UnresolvedDirectMethodTrampolineFromCode(Method* called, Method** sp, Thread* thread,
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

  // Compute details about the called method (avoid GCs)
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  Method* caller = *caller_sp;
  bool is_static;
  bool is_virtual;
  uint32_t dex_method_idx;
  const char* shorty;
  uint32_t shorty_len;
  if (type == Runtime::kUnknownMethod) {
    DCHECK(called->IsRuntimeMethod());
    // less two as return address may span into next dex instruction
    uint32_t dex_pc = caller->ToDexPC(caller_pc - 2);
    const DexFile::CodeItem* code = MethodHelper(caller).GetCodeItem();
    CHECK_LT(dex_pc, code->insns_size_in_code_units_);
    const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
    Instruction::Code instr_code = instr->Opcode();
    is_static = (instr_code == Instruction::INVOKE_STATIC) ||
                (instr_code == Instruction::INVOKE_STATIC_RANGE);
    is_virtual = (instr_code == Instruction::INVOKE_VIRTUAL) ||
                 (instr_code == Instruction::INVOKE_VIRTUAL_RANGE);
    DCHECK(is_static || (instr_code == Instruction::INVOKE_DIRECT) ||
           (instr_code == Instruction::INVOKE_DIRECT_RANGE) ||
           (instr_code == Instruction::INVOKE_VIRTUAL) ||
           (instr_code == Instruction::INVOKE_VIRTUAL_RANGE));
    DecodedInstruction dec_insn(instr);
    dex_method_idx = dec_insn.vB;
    shorty = linker->MethodShorty(dex_method_idx, caller, &shorty_len);
  } else {
    DCHECK(!called->IsRuntimeMethod());
    is_static = type == Runtime::kStaticMethod;
    is_virtual = false;
    dex_method_idx = called->GetDexMethodIndex();
    MethodHelper mh(called);
    shorty = mh.GetShorty();
    shorty_len = mh.GetShortyLength();
  }
  // Discover shorty (avoid GCs)
  size_t args_in_regs = 0;
  for (size_t i = 1; i < shorty_len; i++) {
    char c = shorty[i];
    args_in_regs = args_in_regs + (c == 'J' || c == 'D' ? 2 : 1);
    if (args_in_regs > 3) {
      args_in_regs = 3;
      break;
    }
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
  if (type == Runtime::kUnknownMethod) {
    called = linker->ResolveMethod(dex_method_idx, caller, !is_virtual);
  }
  const void* code = NULL;
  if (LIKELY(!thread->IsExceptionPending())) {
    if (LIKELY(called->IsDirect() == !is_virtual)) {
      // Ensure that the called method's class is initialized.
      Class* called_class = called->GetDeclaringClass();
      linker->EnsureInitialized(called_class, true);
      if (LIKELY(called_class->IsInitialized())) {
        code = called->GetCode();
      } else if (called_class->IsInitializing()) {
        // Class is still initializing, go to oat and grab code (trampoline must be left in place
        // until class is initialized to stop races between threads).
        code = linker->GetOatCodeFor(called);
      } else {
        DCHECK(called_class->IsErroneous());
      }
    } else {
      // Direct method has been made virtual
      thread->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
                                 "Expected direct method but found virtual: %s",
                                 PrettyMethod(called, true).c_str());
    }
  }
  if (UNLIKELY(code == NULL)) {
    // Something went wrong in ResolveMethod or EnsureInitialized,
    // go into deliver exception with the pending exception in r0
    code = reinterpret_cast<void*>(art_deliver_exception_from_code);
    regs[0] = reinterpret_cast<uintptr_t>(thread->GetException());
    thread->ClearException();
  } else {
    // Expect class to at least be initializing.
    DCHECK(called->GetDeclaringClass()->IsInitializing());
    // Don't want infinite recursion.
    DCHECK(code != Runtime::Current()->GetResolutionStubArray(Runtime::kUnknownMethod)->GetData());
    // Set up entry into main method
    regs[0] = reinterpret_cast<uintptr_t>(called);
  }
  return code;
}

static void WorkAroundJniBugsForJobject(intptr_t* arg_ptr) {
  intptr_t value = *arg_ptr;
  Object** value_as_jni_rep = reinterpret_cast<Object**>(value);
  Object* value_as_work_around_rep = value_as_jni_rep != NULL ? *value_as_jni_rep : NULL;
  CHECK(Runtime::Current()->GetHeap()->IsHeapAddress(value_as_work_around_rep)) << value_as_work_around_rep;
  *arg_ptr = reinterpret_cast<intptr_t>(value_as_work_around_rep);
}

extern "C" const void* artWorkAroundAppJniBugs(Thread* self, intptr_t* sp) {
  DCHECK(Thread::Current() == self);
  // TODO: this code is specific to ARM
  // On entry the stack pointed by sp is:
  // | arg3   | <- Calling JNI method's frame (and extra bit for out args)
  // | LR     |
  // | R3     |    arg2
  // | R2     |    arg1
  // | R1     |    jclass/jobject
  // | R0     |    JNIEnv
  // | unused |
  // | unused |
  // | unused | <- sp
  Method* jni_method = self->GetTopOfStack().GetMethod();
  DCHECK(jni_method->IsNative()) << PrettyMethod(jni_method);
  intptr_t* arg_ptr = sp + 4;  // pointer to r1 on stack
  // Fix up this/jclass argument
  WorkAroundJniBugsForJobject(arg_ptr);
  arg_ptr++;
  // Fix up jobject arguments
  MethodHelper mh(jni_method);
  int reg_num = 2;  // Current register being processed, -1 for stack arguments.
  for (uint32_t i = 1; i < mh.GetShortyLength(); i++) {
    char shorty_char = mh.GetShorty()[i];
    if (shorty_char == 'L') {
      WorkAroundJniBugsForJobject(arg_ptr);
    }
    if (shorty_char == 'J' || shorty_char == 'D') {
      if (reg_num == 2) {
        arg_ptr = sp + 8;  // skip to out arguments
        reg_num = -1;
      } else if (reg_num == 3) {
        arg_ptr = sp + 10;  // skip to out arguments plus 2 slots as long must be aligned
        reg_num = -1;
      } else {
        DCHECK(reg_num == -1);
        if ((reinterpret_cast<intptr_t>(arg_ptr) & 7) == 4) {
          arg_ptr += 3;  // unaligned, pad and move through stack arguments
        } else {
          arg_ptr += 2;  // aligned, move through stack arguments
        }
      }
    } else {
      if (reg_num == 2) {
        arg_ptr++; // move through register arguments
        reg_num++;
      } else if (reg_num == 3) {
        arg_ptr = sp + 8;  // skip to outgoing stack arguments
        reg_num = -1;
      } else {
        DCHECK(reg_num == -1);
        arg_ptr++;  // move through stack arguments
      }
    }
  }
  // Load expected destination, see Method::RegisterNative
  const void* code = reinterpret_cast<const void*>(jni_method->GetGcMapRaw());
  if (UNLIKELY(code == NULL)) {
    code = Runtime::Current()->GetJniDlsymLookupStub()->GetData();
    jni_method->RegisterNative(self, code);
  }
  return code;
}


extern "C" uint32_t artGet32StaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(NULL);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, true, false, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(NULL);
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" uint64_t artGet64StaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(NULL);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, true, false, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(NULL);
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" Object* artGetObjStaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(NULL);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(NULL);
  }
  return NULL;  // Will throw exception by checking with Thread::Current
}

extern "C" uint32_t artGet32InstanceFromCode(uint32_t field_idx, Object* obj,
                                             const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(int32_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->Get32(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, false, true, false, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, true);
    } else {
      return field->Get32(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" uint64_t artGet64InstanceFromCode(uint32_t field_idx, Object* obj,
                                             const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(int64_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->Get64(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, false, true, false, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, true);
    } else {
      return field->Get64(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" Object* artGetObjInstanceFromCode(uint32_t field_idx, Object* obj,
                                              const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, false, false, sizeof(Object*));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->GetObj(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, false, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, true);
    } else {
      return field->GetObj(obj);
    }
  }
  return NULL;  // Will throw exception by checking with Thread::Current
}

extern "C" int artSet32StaticFromCode(uint32_t field_idx, uint32_t new_value,
                                      const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(NULL, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, true, true, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(NULL, new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet64StaticFromCode(uint32_t field_idx, const Method* referrer,
                                      uint64_t new_value, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(NULL, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, true, true, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(NULL, new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSetObjStaticFromCode(uint32_t field_idx, Object* new_value,
                                       const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, false, true, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    if (LIKELY(!FieldHelper(field).IsPrimitiveType())) {
      field->SetObj(NULL, new_value);
      return 0;  // success
    }
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, false, true, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(NULL, new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet32InstanceFromCode(uint32_t field_idx, Object* obj, uint32_t new_value,
                                        const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(int32_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    field->Set32(obj, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, false, true, true, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, false);
    } else {
      field->Set32(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSet64InstanceFromCode(uint32_t field_idx, Object* obj, uint64_t new_value,
                                        Thread* self, Method** sp) {
  Method* callee_save = Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsOnly);
  Method* referrer = sp[callee_save->GetFrameSizeInBytes() / sizeof(Method*)];
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(int64_t));
  if (LIKELY(field != NULL  && obj != NULL)) {
    field->Set64(obj, new_value);
    return 0;  // success
  }
  *sp = callee_save;
  self->SetTopOfStack(sp, 0);
  field = FindFieldFromCode(field_idx, referrer, self, false, true, true, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, false);
    } else {
      field->Set64(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSetObjInstanceFromCode(uint32_t field_idx, Object* obj, Object* new_value,
                                         const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, false, true, sizeof(Object*));
  if (LIKELY(field != NULL && obj != NULL)) {
    field->SetObj(obj, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, false, false, true, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, false);
    } else {
      field->SetObj(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
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
  memcpy((char*)array + Array::DataOffset(width).Int32Value(), (char*)&table[4], size_in_bytes);
  return 0;  // Success
}

static uint64_t artInvokeCommon(uint32_t method_idx, Object* this_object, Method* caller_method,
                                Thread* self, Method** sp, bool access_check, InvokeType type){
  Method* method = FindMethodFast(method_idx, this_object, caller_method, access_check, type);
  if (UNLIKELY(method == NULL)) {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);
    if (UNLIKELY(this_object == NULL && type != kDirect && type != kStatic)) {
      ThrowNullPointerExceptionForMethodAccess(self, caller_method, method_idx, type);
      return 0;  // failure
    }
    method = FindMethodFromCode(method_idx, this_object, caller_method, self, access_check, type);
    if (UNLIKELY(method == NULL)) {
      CHECK(self->IsExceptionPending());
      return 0;  // failure
    }
  }
  DCHECK(!self->IsExceptionPending());
  const void* code = method->GetCode();

  uint32_t method_uint = reinterpret_cast<uint32_t>(method);
  uint64_t code_uint = reinterpret_cast<uint32_t>(code);
  uint64_t result = ((code_uint << 32) | method_uint);
  return result;
}

// See comments in runtime_support_asm.S
extern "C" uint64_t artInvokeInterfaceTrampoline(uint32_t method_idx, Object* this_object,
                                                 Method* caller_method, Thread* self,
                                                 Method** sp) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, false, kInterface);
}

extern "C" uint64_t artInvokeInterfaceTrampolineWithAccessCheck(uint32_t method_idx,
                                                                Object* this_object,
                                                                Method* caller_method, Thread* self,
                                                                Method** sp) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, true, kInterface);
}


extern "C" uint64_t artInvokeDirectTrampolineWithAccessCheck(uint32_t method_idx,
                                                             Object* this_object,
                                                             Method* caller_method, Thread* self,
                                                             Method** sp) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, true, kDirect);
}

extern "C" uint64_t artInvokeStaticTrampolineWithAccessCheck(uint32_t method_idx,
                                                            Object* this_object,
                                                            Method* caller_method, Thread* self,
                                                            Method** sp) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, true, kStatic);
}

extern "C" uint64_t artInvokeSuperTrampolineWithAccessCheck(uint32_t method_idx,
                                                            Object* this_object,
                                                            Method* caller_method, Thread* self,
                                                            Method** sp) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, true, kSuper);
}

extern "C" uint64_t artInvokeVirtualTrampolineWithAccessCheck(uint32_t method_idx,
                                                              Object* this_object,
                                                              Method* caller_method, Thread* self,
                                                              Method** sp) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, true, kVirtual);
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
  DCHECK(interface_method != NULL);
  DCHECK(!interface_method->IsProxyMethod()) << PrettyMethod(interface_method);
  args_jobj[1].l = AddLocalReference<jobject>(env, interface_method);
  // Box arguments
  cur_arg = 0;  // reset stack location to read to start
  // reset index, will index into param type array which doesn't include the receiver
  param_index = 0;
  ObjectArray<Class>* param_types = proxy_mh.GetParameterTypes();
  DCHECK(param_types != NULL);
  // Check number of parameter types agrees with number from the Method - less 1 for the receiver.
  DCHECK_EQ(static_cast<size_t>(param_types->GetLength()), num_params - 1);
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
      bool unboxed_okay = UnboxPrimitive(env, result_ref, proxy_mh.GetReturnType(), result_unboxed, "result");
      CHECK(unboxed_okay);
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

uint32_t TraceMethodUnwindFromCode(Thread* self) {
  Trace* tracer = Runtime::Current()->GetTracer();
  TraceStackFrame trace_frame = self->PopTraceStackFrame();
  Method* method = trace_frame.method_;
  uint32_t lr = trace_frame.return_pc_;

  tracer->LogMethodTraceEvent(self, method, Trace::kMethodTraceUnwind);

  return lr;
}

int CmplFloat(float a, float b) {
  if (a == b) {
    return 0;
  } else if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  }
  return -1;
}

int CmpgFloat(float a, float b) {
  if (a == b) {
    return 0;
  } else if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  }
  return 1;
}

int CmpgDouble(double a, double b) {
  if (a == b) {
    return 0;
  } else if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  }
  return 1;
}

int CmplDouble(double a, double b) {
  if (a == b) {
    return 0;
  } else if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  }
  return -1;
}

/*
 * Float/double conversion requires clamping to min and max of integer form.  If
 * target doesn't support this normally, use these.
 */
int64_t D2L(double d) {
  static const double kMaxLong = (double) (int64_t) 0x7fffffffffffffffULL;
  static const double kMinLong = (double) (int64_t) 0x8000000000000000ULL;
  if (d >= kMaxLong) {
    return (int64_t) 0x7fffffffffffffffULL;
  } else if (d <= kMinLong) {
    return (int64_t) 0x8000000000000000ULL;
  } else if (d != d)  { // NaN case
    return 0;
  } else {
    return (int64_t) d;
  }
}

int64_t F2L(float f) {
  static const float kMaxLong = (float) (int64_t) 0x7fffffffffffffffULL;
  static const float kMinLong = (float) (int64_t) 0x8000000000000000ULL;
  if (f >= kMaxLong) {
    return (int64_t) 0x7fffffffffffffffULL;
  } else if (f <= kMinLong) {
    return (int64_t) 0x8000000000000000ULL;
  } else if (f != f) { // NaN case
    return 0;
  } else {
    return (int64_t) f;
  }
}

}  // namespace art
