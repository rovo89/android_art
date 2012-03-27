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

#include "jni_internal.h"
#include "oat/utils/arm/assembler_arm.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "object.h"
#include "stack_indirect_reference_table.h"

#define __ assembler->

namespace art {
namespace arm {

ByteArray* ArmCreateResolutionTrampoline(Runtime::TrampolineType type) {
  UniquePtr<ArmAssembler> assembler(static_cast<ArmAssembler*>(Assembler::Create(kArm)));
  // | Out args |
  // | Method*  | <- SP on entry
  // | LR       |    return address into caller
  // | ...      |    callee saves
  // | R3       |    possible argument
  // | R2       |    possible argument
  // | R1       |    possible argument
  // | R0       |    junk on call to UnresolvedDirectMethodTrampolineFromCode, holds result Method*
  // | Method*  |    Callee save Method* set up by UnresolvedDirectMethodTrampolineFromCode
  // Save callee saves and ready frame for exception delivery
  RegList save = (1 << R1) | (1 << R2) | (1 << R3) | (1 << R5) | (1 << R6) | (1 << R7) | (1 << R8) |
                 (1 << R10) | (1 << R11) | (1 << LR);
  // TODO: enable when GetCalleeSaveMethod is available at stub generation time
  // DCHECK_EQ(save, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetCoreSpillMask());
  __ PushList(save);
  __ LoadFromOffset(kLoadWord, R12, TR,
                    ENTRYPOINT_OFFSET(pUnresolvedDirectMethodTrampolineFromCode));
  __ mov(R2, ShifterOperand(TR));  // Pass Thread::Current() in R2
  __ LoadImmediate(R3, type);
  __ IncreaseFrameSize(8);         // 2 words of space for alignment
  __ mov(R1, ShifterOperand(SP));  // Pass SP
  // Call to unresolved direct method trampoline (method_idx, sp, Thread*, is_static)
  __ blx(R12);
  __ mov(R12, ShifterOperand(R0));  // Save code address returned into R12
  // Restore registers which may have been modified by GC, "R0" will hold the Method*
  __ DecreaseFrameSize(4);
  __ PopList((1 << R0) | save);
  __ bx(R12);  // Leaf call to method's code

  __ bkpt(0);

  assembler->EmitSlowPaths();
  size_t cs = assembler->CodeSize();
  SirtRef<ByteArray> resolution_trampoline(ByteArray::Alloc(cs));
  CHECK(resolution_trampoline.get() != NULL);
  MemoryRegion code(resolution_trampoline->GetData(), resolution_trampoline->GetLength());
  assembler->FinalizeInstructions(code);

  return resolution_trampoline.get();
}

typedef void (*ThrowAme)(Method*, Thread*);

ByteArray* CreateAbstractMethodErrorStub() {
  UniquePtr<ArmAssembler> assembler(static_cast<ArmAssembler*>(Assembler::Create(kArm)));
  // Save callee saves and ready frame for exception delivery
  RegList save = (1 << R4) | (1 << R5) | (1 << R6) | (1 << R7) | (1 << R8) | (1 << R9) |
                 (1 << R10) | (1 << R11) | (1 << LR);
  // TODO: enable when GetCalleeSaveMethod is available at stub generation time
  // DCHECK_EQ(save, Runtime::Current()->GetCalleeSaveMethod(Runtime::kSaveAll)->GetCoreSpillMask());
  __ PushList(save);         // push {r4-r11, lr} - 9 words of callee saves
  // TODO: enable when GetCalleeSaveMethod is available at stub generation time
  // DCHECK_EQ(Runtime::Current()->GetCalleeSaveMethod(Runtime::kSaveAll)->GetFpSpillMask(), 0xFFFFU);
  __ Emit(0xed2d0a20);       // vpush {s0-s31}

  __ IncreaseFrameSize(12);  // 3 words of space, bottom word will hold callee save Method*

  // R0 is the Method* already
  __ mov(R1, ShifterOperand(R9));  // Pass Thread::Current() in R1
  __ mov(R2, ShifterOperand(SP));  // Pass SP in R2
  // Call to throw AbstractMethodError
  __ LoadFromOffset(kLoadWord, R12, TR, ENTRYPOINT_OFFSET(pThrowAbstractMethodErrorFromCode));
  __ mov(PC, ShifterOperand(R12));  // Leaf call to routine that never returns

  __ bkpt(0);

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  SirtRef<ByteArray> abstract_stub(ByteArray::Alloc(cs));
  CHECK(abstract_stub.get() != NULL);
  MemoryRegion code(abstract_stub->GetData(), abstract_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return abstract_stub.get();
}

ByteArray* CreateJniDlsymLookupStub() {
  UniquePtr<ArmAssembler> assembler(static_cast<ArmAssembler*>(Assembler::Create(kArm)));
  // Build frame and save argument registers and LR.
  RegList save = (1 << R0) | (1 << R1) | (1 << R2) | (1 << R3) | (1 << LR);
  __ PushList(save);
  __ AddConstant(SP, -12);         // Ensure 16-byte alignment
  __ mov(R0, ShifterOperand(R9));  // Pass Thread::Current() in R0
  // Call FindNativeMethod
  __ LoadFromOffset(kLoadWord, R12, TR, ENTRYPOINT_OFFSET(pFindNativeMethod));
  __ blx(R12);
  __ mov(R12, ShifterOperand(R0));  // Save result of FindNativeMethod in R12
  __ AddConstant(SP, 12);           // Restore registers (including outgoing arguments)
  __ PopList(save);
  __ cmp(R12, ShifterOperand(0));
  __ bx(R12, NE);                   // If R12 != 0 tail call into native code
  __ bx(LR);                        // Return to caller to handle exception

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  SirtRef<ByteArray> jni_stub(ByteArray::Alloc(cs));
  CHECK(jni_stub.get() != NULL);
  MemoryRegion code(jni_stub->GetData(), jni_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return jni_stub.get();
}

} // namespace arm
} // namespace art
