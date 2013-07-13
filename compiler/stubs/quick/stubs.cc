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

#include "stubs/stubs.h"

#include "jni_internal.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "oat/utils/arm/assembler_arm.h"
#include "oat/utils/mips/assembler_mips.h"
#include "oat/utils/x86/assembler_x86.h"
#include "sirt_ref.h"
#include "stack_indirect_reference_table.h"

#define __ assembler->

namespace art {

namespace arm {
const std::vector<uint8_t>* CreateQuickResolutionTrampoline() {
  UniquePtr<ArmAssembler> assembler(static_cast<ArmAssembler*>(Assembler::Create(kArm)));
  // | Out args |
  // | Method*  | <- SP on entry
  // | LR       |    return address into caller
  // | ...      |    callee saves
  // | R3       |    possible argument
  // | R2       |    possible argument
  // | R1       |    possible argument
  // | R0       |    junk on call to QuickResolutionTrampolineFromCode, holds result Method*
  // | Method*  |    Callee save Method* set up by QuickResoltuionTrampolineFromCode
  // Save callee saves and ready frame for exception delivery
  RegList save = (1 << R1) | (1 << R2) | (1 << R3) | (1 << R5) | (1 << R6) | (1 << R7) | (1 << R8) |
                 (1 << R10) | (1 << R11) | (1 << LR);
  // TODO: enable when GetCalleeSaveMethod is available at stub generation time
  // DCHECK_EQ(save, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetCoreSpillMask());
  __ PushList(save);
  __ LoadFromOffset(kLoadWord, R12, TR, ENTRYPOINT_OFFSET(pQuickResolutionTrampolineFromCode));
  __ mov(R3, ShifterOperand(TR));  // Pass Thread::Current() in R3
  __ IncreaseFrameSize(8);         // 2 words of space for alignment
  __ mov(R2, ShifterOperand(SP));  // Pass SP
  // Call to resolution trampoline (method_idx, receiver, sp, Thread*)
  __ blx(R12);
  __ mov(R12, ShifterOperand(R0));  // Save code address returned into R12
  // Restore registers which may have been modified by GC, "R0" will hold the Method*
  __ DecreaseFrameSize(4);
  __ PopList((1 << R0) | save);
  __ bx(R12);  // Leaf call to method's code
  __ bkpt(0);

  assembler->EmitSlowPaths();
  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > resolution_trampoline(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*resolution_trampoline)[0], resolution_trampoline->size());
  assembler->FinalizeInstructions(code);

  return resolution_trampoline.release();
}

const std::vector<uint8_t>* CreateInterpreterToInterpreterEntry() {
  UniquePtr<ArmAssembler> assembler(static_cast<ArmAssembler*>(Assembler::Create(kArm)));

  __ LoadFromOffset(kLoadWord, PC, R0, ENTRYPOINT_OFFSET(pInterpreterToInterpreterEntry));
  __ bkpt(0);

  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*entry_stub)[0], entry_stub->size());
  assembler->FinalizeInstructions(code);

  return entry_stub.release();
}

const std::vector<uint8_t>* CreateInterpreterToQuickEntry() {
  UniquePtr<ArmAssembler> assembler(static_cast<ArmAssembler*>(Assembler::Create(kArm)));

  __ LoadFromOffset(kLoadWord, PC, R0, ENTRYPOINT_OFFSET(pInterpreterToQuickEntry));
  __ bkpt(0);

  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*entry_stub)[0], entry_stub->size());
  assembler->FinalizeInstructions(code);

  return entry_stub.release();
}
} // namespace arm

namespace mips {
const std::vector<uint8_t>* CreateQuickResolutionTrampoline() {
  UniquePtr<MipsAssembler> assembler(static_cast<MipsAssembler*>(Assembler::Create(kMips)));
  // | Out args   |
  // | Method*    | <- SP on entry
  // | RA         |    return address into caller
  // | ...        |    callee saves
  // | A3         |    possible argument
  // | A2         |    possible argument
  // | A1         |    possible argument
  // | A0/Method* |    Callee save Method* set up by UnresolvedDirectMethodTrampolineFromCode
  // Save callee saves and ready frame for exception delivery
  __ AddConstant(SP, SP, -64);
  __ StoreToOffset(kStoreWord, RA, SP, 60);
  __ StoreToOffset(kStoreWord, FP, SP, 56);
  __ StoreToOffset(kStoreWord, GP, SP, 52);
  __ StoreToOffset(kStoreWord, S7, SP, 48);
  __ StoreToOffset(kStoreWord, S6, SP, 44);
  __ StoreToOffset(kStoreWord, S5, SP, 40);
  __ StoreToOffset(kStoreWord, S4, SP, 36);
  __ StoreToOffset(kStoreWord, S3, SP, 32);
  __ StoreToOffset(kStoreWord, S2, SP, 28);
  __ StoreToOffset(kStoreWord, A3, SP, 12);
  __ StoreToOffset(kStoreWord, A2, SP, 8);
  __ StoreToOffset(kStoreWord, A1, SP, 4);

  __ LoadFromOffset(kLoadWord, T9, S1, ENTRYPOINT_OFFSET(pQuickResolutionTrampolineFromCode));
  __ Move(A3, S1);  // Pass Thread::Current() in A3
  __ Move(A2, SP);  // Pass SP for Method** callee_addr
  __ Jalr(T9); // Call to resolution trampoline (method_idx, receiver, sp, Thread*)

  // Restore registers which may have been modified by GC
  __ LoadFromOffset(kLoadWord, A0, SP, 0);
  __ LoadFromOffset(kLoadWord, A1, SP, 4);
  __ LoadFromOffset(kLoadWord, A2, SP, 8);
  __ LoadFromOffset(kLoadWord, A3, SP, 12);
  __ LoadFromOffset(kLoadWord, S2, SP, 28);
  __ LoadFromOffset(kLoadWord, S3, SP, 32);
  __ LoadFromOffset(kLoadWord, S4, SP, 36);
  __ LoadFromOffset(kLoadWord, S5, SP, 40);
  __ LoadFromOffset(kLoadWord, S6, SP, 44);
  __ LoadFromOffset(kLoadWord, S7, SP, 48);
  __ LoadFromOffset(kLoadWord, GP, SP, 52);
  __ LoadFromOffset(kLoadWord, FP, SP, 56);
  __ LoadFromOffset(kLoadWord, RA, SP, 60);
  __ AddConstant(SP, SP, 64);

  __ Move(T9, V0); // Put method's code in T9
  __ Jr(T9);  // Leaf call to method's code

  __ Break();

  assembler->EmitSlowPaths();
  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > resolution_trampoline(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*resolution_trampoline)[0], resolution_trampoline->size());
  assembler->FinalizeInstructions(code);

  return resolution_trampoline.release();
}

const std::vector<uint8_t>* CreateInterpreterToInterpreterEntry() {
  UniquePtr<MipsAssembler> assembler(static_cast<MipsAssembler*>(Assembler::Create(kMips)));

  __ LoadFromOffset(kLoadWord, T9, A0, ENTRYPOINT_OFFSET(pInterpreterToInterpreterEntry));
  __ Jr(T9);
  __ Break();

  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*entry_stub)[0], entry_stub->size());
  assembler->FinalizeInstructions(code);

  return entry_stub.release();
}

const std::vector<uint8_t>* CreateInterpreterToQuickEntry() {
  UniquePtr<MipsAssembler> assembler(static_cast<MipsAssembler*>(Assembler::Create(kMips)));

  __ LoadFromOffset(kLoadWord, T9, A0, ENTRYPOINT_OFFSET(pInterpreterToInterpreterEntry));
  __ Jr(T9);
  __ Break();

  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*entry_stub)[0], entry_stub->size());
  assembler->FinalizeInstructions(code);

  return entry_stub.release();
}
} // namespace mips

namespace x86 {
const std::vector<uint8_t>* CreateQuickResolutionTrampoline() {
  UniquePtr<X86Assembler> assembler(static_cast<X86Assembler*>(Assembler::Create(kX86)));
  // Set up the callee save frame to conform with Runtime::CreateCalleeSaveMethod(kRefsAndArgs)
  // return address
  __ pushl(EDI);
  __ pushl(ESI);
  __ pushl(EBP);
  __ pushl(EBX);
  __ pushl(EDX);
  __ pushl(ECX);
  __ pushl(EAX);  // <-- callee save Method* to go here
  __ movl(EDX, ESP);          // save ESP
  __ fs()->pushl(Address::Absolute(Thread::SelfOffset()));  // pass Thread*
  __ pushl(EDX);              // pass ESP for Method*
  __ pushl(ECX);              // pass receiver
  __ pushl(EAX);              // pass Method*

  // Call to resolve method.
  __ Call(ThreadOffset(ENTRYPOINT_OFFSET(pQuickResolutionTrampolineFromCode)),
          X86ManagedRegister::FromCpuRegister(ECX));

  __ movl(EDI, EAX);  // save code pointer in EDI
  __ addl(ESP, Immediate(16));  // Pop arguments
  __ popl(EAX);  // Restore args.
  __ popl(ECX);
  __ popl(EDX);
  __ popl(EBX);
  __ popl(EBP);  // Restore callee saves.
  __ popl(ESI);
  // Swap EDI callee save with code pointer
  __ xchgl(EDI, Address(ESP, 0));
  // Tail call to intended method.
  __ ret();

  assembler->EmitSlowPaths();
  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > resolution_trampoline(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*resolution_trampoline)[0], resolution_trampoline->size());
  assembler->FinalizeInstructions(code);

  return resolution_trampoline.release();
}

const std::vector<uint8_t>* CreateInterpreterToInterpreterEntry() {
  UniquePtr<X86Assembler> assembler(static_cast<X86Assembler*>(Assembler::Create(kX86)));

  __ fs()->jmp(Address::Absolute(ThreadOffset(ENTRYPOINT_OFFSET(pInterpreterToInterpreterEntry))));

  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*entry_stub)[0], entry_stub->size());
  assembler->FinalizeInstructions(code);

  return entry_stub.release();
}

const std::vector<uint8_t>* CreateInterpreterToQuickEntry() {
  UniquePtr<X86Assembler> assembler(static_cast<X86Assembler*>(Assembler::Create(kX86)));

  __ fs()->jmp(Address::Absolute(ThreadOffset(ENTRYPOINT_OFFSET(pInterpreterToQuickEntry))));

  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*entry_stub)[0], entry_stub->size());
  assembler->FinalizeInstructions(code);

  return entry_stub.release();
}
} // namespace x86

} // namespace art
