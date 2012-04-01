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

#include "callee_save_frame.h"
#include "dex_instruction.h"
#include "object.h"
#include "object_utils.h"

// Architecture specific assembler helper to deliver exception.
extern "C" void art_deliver_exception_from_code(void*);

namespace art {

// Lazily resolve a method. Called by stub code.
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
                 (instr_code == Instruction::INVOKE_VIRTUAL_RANGE) ||
                 (instr_code == Instruction::INVOKE_SUPER) ||
                 (instr_code == Instruction::INVOKE_SUPER_RANGE);
    DCHECK(is_static || is_virtual || (instr_code == Instruction::INVOKE_DIRECT) ||
           (instr_code == Instruction::INVOKE_DIRECT_RANGE));
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
      linker->EnsureInitialized(called_class, true, true);
      if (LIKELY(called_class->IsInitialized())) {
        code = called->GetCode();
      } else if (called_class->IsInitializing()) {
        if (is_static) {
          // Class is still initializing, go to oat and grab code (trampoline must be left in place
          // until class is initialized to stop races between threads).
          code = linker->GetOatCodeFor(called);
        } else {
          // No trampoline for non-static methods.
          code = called->GetCode();
        }
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

// Called by the AbstractMethodError. Called by stub code.
extern void ThrowAbstractMethodErrorFromCode(Method* method, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewExceptionF("Ljava/lang/AbstractMethodError;",
                             "abstract method \"%s\"", PrettyMethod(method).c_str());
  thread->DeliverException();
}

}  // namespace art
