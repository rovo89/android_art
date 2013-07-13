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
#include "oat/utils/arm/assembler_arm.h"
#include "oat/utils/mips/assembler_mips.h"
#include "oat/utils/x86/assembler_x86.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "stack_indirect_reference_table.h"
#include "sirt_ref.h"

#define __ assembler->

namespace art {

namespace arm {
const std::vector<uint8_t>* CreatePortableResolutionTrampoline() {
  UniquePtr<ArmAssembler> assembler(static_cast<ArmAssembler*>(Assembler::Create(kArm)));
  RegList save = (1 << R0) | (1 << R1) | (1 << R2) | (1 << R3) | (1 << LR);

  __ PushList(save);
  __ LoadFromOffset(kLoadWord, R12, TR, ENTRYPOINT_OFFSET(pPortableResolutionTrampolineFromCode));
  __ mov(R3, ShifterOperand(TR));  // Pass Thread::Current() in R3
  __ mov(R2, ShifterOperand(SP));  // Pass sp for Method** callee_addr
  __ IncreaseFrameSize(12);         // 3 words of space for alignment
  // Call to resolution trampoline (callee, receiver, callee_addr, Thread*)
  __ blx(R12);
  __ mov(R12, ShifterOperand(R0));  // Save code address returned into R12
  __ DecreaseFrameSize(12);
  __ PopList(save);
  __ cmp(R12, ShifterOperand(0));
  __ bx(R12, NE);                   // If R12 != 0 tail call method's code
  __ bx(LR);                        // Return to caller to handle exception

  assembler->EmitSlowPaths();
  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > resolution_trampoline(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*resolution_trampoline)[0], resolution_trampoline->size());
  assembler->FinalizeInstructions(code);

  return resolution_trampoline.release();
}
} // namespace arm

namespace mips {
const std::vector<uint8_t>* CreatePortableResolutionTrampoline() {
  UniquePtr<MipsAssembler> assembler(static_cast<MipsAssembler*>(Assembler::Create(kMips)));
  // Build frame and save argument registers and RA.
  __ AddConstant(SP, SP, -32);
  __ StoreToOffset(kStoreWord, RA, SP, 28);
  __ StoreToOffset(kStoreWord, A3, SP, 12);
  __ StoreToOffset(kStoreWord, A2, SP, 8);
  __ StoreToOffset(kStoreWord, A1, SP, 4);
  __ StoreToOffset(kStoreWord, A0, SP, 0);

  __ LoadFromOffset(kLoadWord, T9, S1,
                    ENTRYPOINT_OFFSET(pPortableResolutionTrampolineFromCode));
  __ Move(A3, S1);  // Pass Thread::Current() in A3
  __ Move(A2, SP);  // Pass SP for Method** callee_addr
  __ Jalr(T9); // Call to resolution trampoline (callee, receiver, callee_addr, Thread*)

  // Restore frame, argument registers, and RA.
  __ LoadFromOffset(kLoadWord, A0, SP, 0);
  __ LoadFromOffset(kLoadWord, A1, SP, 4);
  __ LoadFromOffset(kLoadWord, A2, SP, 8);
  __ LoadFromOffset(kLoadWord, A3, SP, 12);
  __ LoadFromOffset(kLoadWord, RA, SP, 28);
  __ AddConstant(SP, SP, 32);

  Label resolve_fail;
  __ EmitBranch(V0, ZERO, &resolve_fail, true);
  __ Jr(V0); // If V0 != 0 tail call method's code
  __ Bind(&resolve_fail, false);
  __ Jr(RA); // Return to caller to handle exception

  assembler->EmitSlowPaths();
  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > resolution_trampoline(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*resolution_trampoline)[0], resolution_trampoline->size());
  assembler->FinalizeInstructions(code);

  return resolution_trampoline.release();
}
} // namespace mips

namespace x86 {
const std::vector<uint8_t>* CreatePortableResolutionTrampoline() {
  UniquePtr<X86Assembler> assembler(static_cast<X86Assembler*>(Assembler::Create(kX86)));

  __ pushl(EBP);
  __ movl(EBP, ESP);          // save ESP
  __ subl(ESP, Immediate(8));  // Align stack
  __ movl(EAX, Address(EBP, 8));  // Method* called
  __ leal(EDX, Address(EBP, 8));  // Method** called_addr
  __ fs()->pushl(Address::Absolute(Thread::SelfOffset()));  // pass thread
  __ pushl(EDX);  // pass called_addr
  __ pushl(ECX);  // pass receiver
  __ pushl(EAX);  // pass called
  // Call to resolve method.
  __ Call(ThreadOffset(ENTRYPOINT_OFFSET(pPortableResolutionTrampolineFromCode)),
          X86ManagedRegister::FromCpuRegister(ECX));
  __ leave();

  Label resolve_fail;  // forward declaration
  __ cmpl(EAX, Immediate(0));
  __ j(kEqual, &resolve_fail);
  __ jmp(EAX);
  // Tail call to intended method.
  __ Bind(&resolve_fail);
  __ ret();

  assembler->EmitSlowPaths();
  size_t cs = assembler->CodeSize();
  UniquePtr<std::vector<uint8_t> > resolution_trampoline(new std::vector<uint8_t>(cs));
  MemoryRegion code(&(*resolution_trampoline)[0], resolution_trampoline->size());
  assembler->FinalizeInstructions(code);

  return resolution_trampoline.release();
}
} // namespace x86

} // namespace art
