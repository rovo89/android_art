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
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "mirror/class-inl.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "object_utils.h"
#include "scoped_thread_state_change.h"

// Architecture specific assembler helper to deliver exception.
extern "C" void art_quick_deliver_exception_from_code(void*);

namespace art {

// Lazily resolve a method for portable. Called by stub code.
extern "C" const void* artPortableResolutionTrampoline(mirror::AbstractMethod* called,
                                                       mirror::Object* receiver,
                                                       mirror::AbstractMethod** called_addr,
                                                       Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  uint32_t dex_pc;
  mirror::AbstractMethod* caller = thread->GetCurrentMethod(&dex_pc);

  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  InvokeType invoke_type;
  bool is_range;
  if (called->IsRuntimeMethod()) {
    const DexFile::CodeItem* code = MethodHelper(caller).GetCodeItem();
    CHECK_LT(dex_pc, code->insns_size_in_code_units_);
    const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
    Instruction::Code instr_code = instr->Opcode();
    switch (instr_code) {
      case Instruction::INVOKE_DIRECT:
        invoke_type = kDirect;
        is_range = false;
        break;
      case Instruction::INVOKE_DIRECT_RANGE:
        invoke_type = kDirect;
        is_range = true;
        break;
      case Instruction::INVOKE_STATIC:
        invoke_type = kStatic;
        is_range = false;
        break;
      case Instruction::INVOKE_STATIC_RANGE:
        invoke_type = kStatic;
        is_range = true;
        break;
      case Instruction::INVOKE_SUPER:
        invoke_type = kSuper;
        is_range = false;
        break;
      case Instruction::INVOKE_SUPER_RANGE:
        invoke_type = kSuper;
        is_range = true;
        break;
      case Instruction::INVOKE_VIRTUAL:
        invoke_type = kVirtual;
        is_range = false;
        break;
      case Instruction::INVOKE_VIRTUAL_RANGE:
        invoke_type = kVirtual;
        is_range = true;
        break;
      case Instruction::INVOKE_INTERFACE:
        invoke_type = kInterface;
        is_range = false;
        break;
      case Instruction::INVOKE_INTERFACE_RANGE:
        invoke_type = kInterface;
        is_range = true;
        break;
      default:
        LOG(FATAL) << "Unexpected call into trampoline: " << instr->DumpString(NULL);
        // Avoid used uninitialized warnings.
        invoke_type = kDirect;
        is_range = true;
    }
    uint32_t dex_method_idx = (is_range) ? instr->VRegB_3rc() : instr->VRegB_35c();
    called = linker->ResolveMethod(dex_method_idx, caller, invoke_type);
    // Refine called method based on receiver.
    if (invoke_type == kVirtual) {
      called = receiver->GetClass()->FindVirtualMethodForVirtual(called);
    } else if (invoke_type == kInterface) {
      called = receiver->GetClass()->FindVirtualMethodForInterface(called);
    }
  } else {
    CHECK(called->IsStatic()) << PrettyMethod(called);
    invoke_type = kStatic;
  }
  const void* code = NULL;
  if (LIKELY(!thread->IsExceptionPending())) {
    // Incompatible class change should have been handled in resolve method.
    CHECK(!called->CheckIncompatibleClassChange(invoke_type));
    // Ensure that the called method's class is initialized.
    mirror::Class* called_class = called->GetDeclaringClass();
    linker->EnsureInitialized(called_class, true, true);
    if (LIKELY(called_class->IsInitialized())) {
      code = called->GetEntryPointFromCompiledCode();
      // TODO: remove this after we solve the link issue.
      { // for lazy link.
        if (code == NULL) {
          code = linker->GetOatCodeFor(called);
        }
      }
    } else if (called_class->IsInitializing()) {
      if (invoke_type == kStatic) {
        // Class is still initializing, go to oat and grab code (trampoline must be left in place
        // until class is initialized to stop races between threads).
        code = linker->GetOatCodeFor(called);
      } else {
        // No trampoline for non-static methods.
        code = called->GetEntryPointFromCompiledCode();
        // TODO: remove this after we solve the link issue.
        { // for lazy link.
          if (code == NULL) {
            code = linker->GetOatCodeFor(called);
          }
        }
      }
    } else {
      DCHECK(called_class->IsErroneous());
    }
  }
  if (LIKELY(code != NULL)) {
    // Expect class to at least be initializing.
    DCHECK(called->GetDeclaringClass()->IsInitializing());
    // Don't want infinite recursion.
    DCHECK(code != GetResolutionTrampoline());
    // Set up entry into main method
    *called_addr = called;
  }
  return code;
}

// Lazily resolve a method for quick. Called by stub code.
extern "C" const void* artQuickResolutionTrampoline(mirror::AbstractMethod* called,
                                                    mirror::Object* receiver,
                                                    mirror::AbstractMethod** sp, Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
#if defined(__arm__)
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
  DCHECK_EQ(48U, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
  mirror::AbstractMethod** caller_sp = reinterpret_cast<mirror::AbstractMethod**>(reinterpret_cast<byte*>(sp) + 48);
  uintptr_t* regs = reinterpret_cast<uintptr_t*>(reinterpret_cast<byte*>(sp) + kPointerSize);
  uint32_t pc_offset = 10;
  uintptr_t caller_pc = regs[pc_offset];
#elif defined(__i386__)
  // On entry the stack pointed by sp is:
  // | argN        |  |
  // | ...         |  |
  // | arg4        |  |
  // | arg3 spill  |  |  Caller's frame
  // | arg2 spill  |  |
  // | arg1 spill  |  |
  // | Method*     | ---
  // | Return      |
  // | EBP,ESI,EDI |    callee saves
  // | EBX         |    arg3
  // | EDX         |    arg2
  // | ECX         |    arg1
  // | EAX/Method* |  <- sp
  DCHECK_EQ(32U, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
  mirror::AbstractMethod** caller_sp = reinterpret_cast<mirror::AbstractMethod**>(reinterpret_cast<byte*>(sp) + 32);
  uintptr_t* regs = reinterpret_cast<uintptr_t*>(reinterpret_cast<byte*>(sp));
  uintptr_t caller_pc = regs[7];
#elif defined(__mips__)
  // On entry the stack pointed by sp is:
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | RA         |
  // | ...        |    callee saves
  // | A3         |    arg3
  // | A2         |    arg2
  // | A1         |    arg1
  // | A0/Method* |  <- sp
  DCHECK_EQ(64U, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
  mirror::AbstractMethod** caller_sp = reinterpret_cast<mirror::AbstractMethod**>(reinterpret_cast<byte*>(sp) + 64);
  uintptr_t* regs = reinterpret_cast<uintptr_t*>(reinterpret_cast<byte*>(sp));
  uint32_t pc_offset = 15;
  uintptr_t caller_pc = regs[pc_offset];
#else
  UNIMPLEMENTED(FATAL);
  mirror::AbstractMethod** caller_sp = NULL;
  uintptr_t* regs = NULL;
  uintptr_t caller_pc = 0;
#endif
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kRefsAndArgs);
  // Start new JNI local reference state
  JNIEnvExt* env = thread->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);

  // Compute details about the called method (avoid GCs)
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  mirror::AbstractMethod* caller = *caller_sp;
  InvokeType invoke_type;
  uint32_t dex_method_idx;
#if !defined(__i386__)
  const char* shorty;
  uint32_t shorty_len;
#endif
  if (called->IsRuntimeMethod()) {
    uint32_t dex_pc = caller->ToDexPc(caller_pc);
    const DexFile::CodeItem* code = MethodHelper(caller).GetCodeItem();
    CHECK_LT(dex_pc, code->insns_size_in_code_units_);
    const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
    Instruction::Code instr_code = instr->Opcode();
    bool is_range;
    switch (instr_code) {
      case Instruction::INVOKE_DIRECT:
        invoke_type = kDirect;
        is_range = false;
        break;
      case Instruction::INVOKE_DIRECT_RANGE:
        invoke_type = kDirect;
        is_range = true;
        break;
      case Instruction::INVOKE_STATIC:
        invoke_type = kStatic;
        is_range = false;
        break;
      case Instruction::INVOKE_STATIC_RANGE:
        invoke_type = kStatic;
        is_range = true;
        break;
      case Instruction::INVOKE_SUPER:
        invoke_type = kSuper;
        is_range = false;
        break;
      case Instruction::INVOKE_SUPER_RANGE:
        invoke_type = kSuper;
        is_range = true;
        break;
      case Instruction::INVOKE_VIRTUAL:
        invoke_type = kVirtual;
        is_range = false;
        break;
      case Instruction::INVOKE_VIRTUAL_RANGE:
        invoke_type = kVirtual;
        is_range = true;
        break;
      case Instruction::INVOKE_INTERFACE:
        invoke_type = kInterface;
        is_range = false;
        break;
      case Instruction::INVOKE_INTERFACE_RANGE:
        invoke_type = kInterface;
        is_range = true;
        break;
      default:
        LOG(FATAL) << "Unexpected call into trampoline: " << instr->DumpString(NULL);
        // Avoid used uninitialized warnings.
        invoke_type = kDirect;
        is_range = false;
    }
    dex_method_idx = (is_range) ? instr->VRegB_3rc() : instr->VRegB_35c();
#if !defined(__i386__)
    shorty = linker->MethodShorty(dex_method_idx, caller, &shorty_len);
#endif
  } else {
    invoke_type = kStatic;
    dex_method_idx = called->GetDexMethodIndex();
#if !defined(__i386__)
    MethodHelper mh(called);
    shorty = mh.GetShorty();
    shorty_len = mh.GetShortyLength();
#endif
  }
#if !defined(__i386__)
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
  if (invoke_type != kStatic) {
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(regs[cur_arg]);
    cur_arg++;
    if (args_in_regs < 3) {
      // If we thought we had fewer than 3 arguments in registers, account for the receiver
      args_in_regs++;
    }
    soa.AddLocalReference<jobject>(obj);
  }
  size_t shorty_index = 1;  // skip return value
  // Iterate while arguments and arguments in registers (less 1 from cur_arg which is offset to skip
  // R0)
  while ((cur_arg - 1) < args_in_regs && shorty_index < shorty_len) {
    char c = shorty[shorty_index];
    shorty_index++;
    if (c == 'L') {
      mirror::Object* obj = reinterpret_cast<mirror::Object*>(regs[cur_arg]);
      soa.AddLocalReference<jobject>(obj);
    }
    cur_arg = cur_arg + (c == 'J' || c == 'D' ? 2 : 1);
  }
  // Place into local references incoming arguments from the caller's stack arguments
  cur_arg += pc_offset + 1;  // skip LR/RA, Method* and spills for R1-R3/A1-A3 and callee saves
  while (shorty_index < shorty_len) {
    char c = shorty[shorty_index];
    shorty_index++;
    if (c == 'L') {
      mirror::Object* obj = reinterpret_cast<mirror::Object*>(regs[cur_arg]);
      soa.AddLocalReference<jobject>(obj);
    }
    cur_arg = cur_arg + (c == 'J' || c == 'D' ? 2 : 1);
  }
#endif
  // Resolve method filling in dex cache
  if (called->IsRuntimeMethod()) {
    called = linker->ResolveMethod(dex_method_idx, caller, invoke_type);
  }
  const void* code = NULL;
  if (LIKELY(!thread->IsExceptionPending())) {
    // Incompatible class change should have been handled in resolve method.
    CHECK(!called->CheckIncompatibleClassChange(invoke_type));
    // Refine called method based on receiver.
    if (invoke_type == kVirtual) {
      called = receiver->GetClass()->FindVirtualMethodForVirtual(called);
    } else if (invoke_type == kInterface) {
      called = receiver->GetClass()->FindVirtualMethodForInterface(called);
    }
    // Ensure that the called method's class is initialized.
    mirror::Class* called_class = called->GetDeclaringClass();
    linker->EnsureInitialized(called_class, true, true);
    if (LIKELY(called_class->IsInitialized())) {
      code = called->GetEntryPointFromCompiledCode();
    } else if (called_class->IsInitializing()) {
      if (invoke_type == kStatic) {
        // Class is still initializing, go to oat and grab code (trampoline must be left in place
        // until class is initialized to stop races between threads).
        code = linker->GetOatCodeFor(called);
      } else {
        // No trampoline for non-static methods.
        code = called->GetEntryPointFromCompiledCode();
      }
    } else {
      DCHECK(called_class->IsErroneous());
    }
  }
  if (UNLIKELY(code == NULL)) {
    // Something went wrong in ResolveMethod or EnsureInitialized,
    // go into deliver exception with the pending exception in r0
    CHECK(thread->IsExceptionPending());
    code = reinterpret_cast<void*>(art_quick_deliver_exception_from_code);
    regs[0] = reinterpret_cast<uintptr_t>(thread->GetException(NULL));
    thread->ClearException();
  } else {
    // Expect class to at least be initializing.
    DCHECK(called->GetDeclaringClass()->IsInitializing());
    // Don't want infinite recursion.
    DCHECK(code != GetResolutionTrampoline());
    // Set up entry into main method
    regs[0] = reinterpret_cast<uintptr_t>(called);
  }
  return code;
}

// Called by the abstract method error stub.
extern "C" void artThrowAbstractMethodErrorFromCode(mirror::AbstractMethod* method, Thread* self,
                                                    mirror::AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
#if !defined(ART_USE_PORTABLE_COMPILER)
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kSaveAll);
#else
  UNUSED(sp);
#endif
  ThrowLocation throw_location = self->GetCurrentLocationForThrow();
  self->ThrowNewExceptionF(throw_location, "Ljava/lang/AbstractMethodError;",
                           "abstract method \"%s\"", PrettyMethod(method).c_str());
  self->QuickDeliverException();
}

// Used by the JNI dlsym stub to find the native method to invoke if none is registered.
extern "C" void* artFindNativeMethod(Thread* self) {
  Locks::mutator_lock_->AssertNotHeld(self);  // We come here as Native.
  DCHECK(Thread::Current() == self);
  ScopedObjectAccess soa(self);

  mirror::AbstractMethod* method = self->GetCurrentMethod(NULL);
  DCHECK(method != NULL);

  // Lookup symbol address for method, on failure we'll return NULL with an
  // exception set, otherwise we return the address of the method we found.
  void* native_code = soa.Vm()->FindCodeForNativeMethod(method);
  if (native_code == NULL) {
    DCHECK(self->IsExceptionPending());
    return NULL;
  } else {
    // Register so that future calls don't come here
    method->RegisterNative(self, native_code);
    return native_code;
  }
}

}  // namespace art
