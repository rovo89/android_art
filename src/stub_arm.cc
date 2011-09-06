// Copyright 2011 Google Inc. All Rights Reserved.

#include "assembler_arm.h"
#include "jni_internal.h"
#include "object.h"

#define __ assembler->

namespace art {
namespace arm {

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
  __ LoadImmediate(R12, reinterpret_cast<int32_t>(&FindNativeMethod));
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
