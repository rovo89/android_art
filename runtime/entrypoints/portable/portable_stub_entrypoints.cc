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

#include "dex_instruction-inl.h"
#include "entrypoints/entrypoint_utils.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/object-inl.h"

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
      {  // for lazy link.
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
        {  // for lazy link.
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
    DCHECK(code != GetResolutionTrampoline(linker));
    // Set up entry into main method
    *called_addr = called;
  }
  return code;
}

}  // namespace art
