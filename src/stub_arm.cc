// Copyright 2011 Google Inc. All Rights Reserved.

#include "assembler_arm.h"
#include "jni_internal.h"
#include "object.h"

#define __ assembler->

namespace art {
namespace arm {

ByteArray* ArmCreateResolutionTrampoline(Runtime::TrampolineType type) {
  UniquePtr<ArmAssembler> assembler(static_cast<ArmAssembler*>(Assembler::Create(kArm)));
  // | Out args |
  // | Method*  | <- SP on entry
  // | LR       |    return address into caller
  // | R3       |    possible argument
  // | R2       |    possible argument
  // | R1       |    possible argument
  // | R0       |    method index (loaded from code and method array - will be converted to Method*)
  RegList save = (1 << R0) | (1 << R1) | (1 << R2) | (1 << R3) | (1 << LR);
  __ PushList(save);
  __ mov(R1, ShifterOperand(SP));  // Pass address of saved R0... in R1
  __ LoadFromOffset(kLoadWord, R12, TR,
                    OFFSETOF_MEMBER(Thread, pUnresolvedDirectMethodTrampolineFromCode));
  __ mov(R2, ShifterOperand(TR));  // Pass Thread::Current() in R2
  __ LoadImmediate(R3, type);
  __ IncreaseFrameSize(12);        // 3 words of space for alignment
  // Call to unresolved direct method trampoline (method_idx, sp, Thread*, is_static)
  __ blx(R12);
  __ mov(R12, ShifterOperand(R0));  // Save code address returned into R12
  // Restore registers which may have been modified by GC and R0 which will now hold the method*
  __ DecreaseFrameSize(12);
  __ PopList(save);
  __ bx(R12);  // Leaf call to method's code

  __ bkpt(0);

  assembler->EmitSlowPaths();
  size_t cs = assembler->CodeSize();
  ByteArray* resolution_trampoline = ByteArray::Alloc(cs);
  CHECK(resolution_trampoline != NULL);
  MemoryRegion code(resolution_trampoline->GetData(), resolution_trampoline->GetLength());
  assembler->FinalizeInstructions(code);

  return resolution_trampoline;
}

typedef void (*ThrowAme)(Method*, Thread*);

ByteArray* CreateAbstractMethodErrorStub() {
  UniquePtr<ArmAssembler> assembler(static_cast<ArmAssembler*>(Assembler::Create(kArm)));
  // Save callee saves and ready frame for exception delivery
  RegList save = (1 << R4) | (1 << R5) | (1 << R6) | (1 << R7) | (1 << R8) | (1 << R9) |
                 (1 << R10) | (1 << R11) | (1 << LR);
  __ PushList(save);         // push {r4-r11, lr} - 9 words of callee saves
  __ Emit(0xed2d0a20);       // vpush {s0-s31}
  __ IncreaseFrameSize(12);  // 3 words of space, bottom word will hold callee save Method*

  // R0 is the Method* already
  __ mov(R1, ShifterOperand(R9));  // Pass Thread::Current() in R1
  __ mov(R2, ShifterOperand(SP));  // Pass SP in R2
  // Call to throw AbstractMethodError
  __ LoadFromOffset(kLoadWord, R12, TR, OFFSETOF_MEMBER(Thread, pThrowAbstractMethodErrorFromCode));
  __ mov(PC, ShifterOperand(R12));  // Leaf call to routine that never returns

  __ bkpt(0);

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  ByteArray* abstract_stub = ByteArray::Alloc(cs);
  CHECK(abstract_stub != NULL);
  CHECK(abstract_stub->GetClass()->GetDescriptor());
  MemoryRegion code(abstract_stub->GetData(), abstract_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return abstract_stub;
}

ByteArray* CreateJniStub() {
  UniquePtr<ArmAssembler> assembler(static_cast<ArmAssembler*>(Assembler::Create(kArm)));
  // Build frame and save argument registers and LR.
  RegList save = (1 << R0) | (1 << R1) | (1 << R2) | (1 << R3) | (1 << LR);
  __ PushList(save);
  __ AddConstant(SP, -12);         // Ensure 16-byte alignment
  __ mov(R0, ShifterOperand(R9));  // Pass Thread::Current() in R0
  // Call FindNativeMethod
  __ LoadFromOffset(kLoadWord, R12, TR, OFFSETOF_MEMBER(Thread, pFindNativeMethod));
  __ blx(R12);
  __ mov(R12, ShifterOperand(R0));  // Save result of FindNativeMethod in R12
  __ AddConstant(SP, 12);           // Restore registers (including outgoing arguments)
  __ PopList(save);
  __ cmp(R12, ShifterOperand(0));
  __ bx(R12, NE);                   // If R12 != 0 tail call into native code
  __ bx(LR);                        // Return to caller to handle exception

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  ByteArray* jni_stub = ByteArray::Alloc(cs);
  CHECK(jni_stub != NULL);
  MemoryRegion code(jni_stub->GetData(), jni_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return jni_stub;
}

} // namespace arm
} // namespace art
