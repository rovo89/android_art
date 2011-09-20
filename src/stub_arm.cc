// Copyright 2011 Google Inc. All Rights Reserved.

#include "assembler_arm.h"
#include "jni_internal.h"
#include "object.h"

#define __ assembler->

namespace art {
namespace arm {

typedef void (*ThrowAme)(Method*, Thread*);

ByteArray* CreateAbstractMethodErrorStub() {
  UniquePtr<ArmAssembler> assembler( static_cast<ArmAssembler*>(Assembler::Create(kArm)) );

  // Save callee saves and ready frame for exception delivery
  RegList save = (1 << R1) | (1 << R2) | (1 << R3) | (1 << R4) | (1 << R5) | (1 << R6) | (1 << R7) |
                 (1 << R8) | (1 << R9) | (1 << R10) | (1 << R11) | (1 << LR);
  __ PushList(save);
  __ IncreaseFrameSize(16);  // 4 words of space, bottom word will hold callee save Method*

  // R0 is the Method* already
  __ mov(R1, ShifterOperand(R9));  // Pass Thread::Current() in R1
  __ mov(R2, ShifterOperand(SP));  // Pass SP in R2
  // Call to throw AbstractMethodError
  __ LoadFromOffset(kLoadWord, R12, TR, OFFSETOF_MEMBER(Thread, pThrowAbstractMethodErrorFromCode));
  // Leaf call to routine that never returns
  __ mov(PC, ShifterOperand(R12));

  __ bkpt(0);

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  ByteArray* abstract_stub = ByteArray::Alloc(cs);
  CHECK(abstract_stub != NULL);
  MemoryRegion code(abstract_stub->GetData(), abstract_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return abstract_stub;
}

ByteArray* CreateJniStub() {
  UniquePtr<ArmAssembler> assembler( static_cast<ArmAssembler*>(Assembler::Create(kArm)) );

  RegList save = (1 << R0) | (1 << R1) | (1 << R2) | (1 << R3) | (1 << LR);

  // Build frame and save registers. Save 5 registers.
  __ PushList(save);
  // Ensure 16-byte alignment
  __ AddConstant(SP, -12);

  // Pass Thread::Current() in R0
  __ mov(R0, ShifterOperand(R9));

  // Call FindNativeMethod
  __ LoadFromOffset(kLoadWord, R12, TR, OFFSETOF_MEMBER(Thread, pFindNativeMethod));
  __ blx(R12);

  // Save result of FindNativeMethod in R12
  __ mov(R12, ShifterOperand(R0));

  // Restore registers (including outgoing arguments)
  __ AddConstant(SP, 12);
  __ PopList(save);

  __ cmp(R12, ShifterOperand(0));

  // If R12 != 0 tail call into native code
  __ mov(PC, ShifterOperand(R12), NE);

  // Return to caller to handle exception
  __ mov(PC, ShifterOperand(LR));

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
