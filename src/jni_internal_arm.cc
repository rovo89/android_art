// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include <algorithm>

#include "asm_support.h"
#include "assembler.h"
#include "compiled_method.h"
#include "object.h"

namespace art {
namespace arm {

// Creates a function which invokes a managed method with an array of
// arguments.
//
// At the time of call, the environment looks something like this:
//
// R0 = method pointer
// R1 = receiver pointer or NULL for static methods
// R2 = (managed) thread pointer
// R3 = argument array or NULL for no argument methods
// [SP] = JValue* result or NULL for void returns
//
// As the JNI call has already transitioned the thread into the
// "running" state the remaining responsibilities of this routine are
// to save the native register value and restore the managed thread
// register and transfer arguments from the array into register and on
// the stack, if needed.  On return, the thread register must be
// shuffled and the return value must be store into the result JValue.
CompiledInvokeStub* ArmCreateInvokeStub(const Method* method) {
  UniquePtr<ArmAssembler> assembler(
      down_cast<ArmAssembler*>(Assembler::Create(kArm)));
#define __ assembler->
  // Size of frame - spill of R4,R9/LR + Method* + possible receiver + arg array
  size_t unpadded_frame_size = (4 * kPointerSize) +
                               (method->IsStatic() ? 0 : kPointerSize) +
                               method->NumArgArrayBytes();
  size_t frame_size = RoundUp(unpadded_frame_size, kStackAlignment);

  // Spill R4,R9 and LR
  RegList save = (1 << R9) | (1 << R4);
  __ PushList(save | (1 << LR));

  // Move the managed thread pointer into R9.
  __ mov(R9, ShifterOperand(R2));

  // Reset R4 to suspend check intercal
  __ LoadImmediate(R4, SUSPEND_CHECK_INTERVAL);

  // Move frame down for arguments less 3 pushed values above
  __ AddConstant(SP, -frame_size + (3 * kPointerSize));

  // Can either get 3 or 2 arguments into registers
  size_t reg_bytes = (method->IsStatic() ? 3 : 2) * kPointerSize;
  // Bytes passed by stack
  size_t stack_bytes;
  if (method->NumArgArrayBytes() > reg_bytes) {
    stack_bytes = method->NumArgArrayBytes() - reg_bytes;
  } else {
    stack_bytes = 0;
    reg_bytes = method->NumArgArrayBytes();
  }

  // Method* at bottom of frame is null thereby terminating managed stack crawls
  __ LoadImmediate(IP, 0, AL);
  __ StoreToOffset(kStoreWord, IP, SP, 0);

  // Copy values by stack
  for (size_t off = 0; off < stack_bytes; off += kPointerSize) {
    // we're displaced off of r3 by bytes that'll go in registers
    int r3_offset = reg_bytes + off;
    __ LoadFromOffset(kLoadWord, IP, R3, r3_offset);

    // we're displaced off of the arguments by the spill space for the incoming
    // arguments, the Method* and possibly the receiver
    int sp_offset = reg_bytes + (method->IsStatic() ? 1 : 2) * kPointerSize + off;
    __ StoreToOffset(kStoreWord, IP, SP, sp_offset);
  }

  // Move all the register arguments into place.
  if (method->IsStatic()) {
    if (reg_bytes > 0) {
      __ LoadFromOffset(kLoadWord, R1, R3, 0);
      if (reg_bytes > 4) {
        __ LoadFromOffset(kLoadWord, R2, R3, 4);
        if (reg_bytes > 8) {
          __ LoadFromOffset(kLoadWord, R3, R3, 8);
        }
      }
    }
  } else {
    if (reg_bytes > 0) {
      __ LoadFromOffset(kLoadWord, R2, R3, 0);
      if (reg_bytes > 4) {
        __ LoadFromOffset(kLoadWord, R3, R3, 4);
      }
    }
  }

  // Load the code pointer we are about to call.
  __ LoadFromOffset(kLoadWord, IP, R0, method->GetCodeOffset().Int32Value());

  // Do the call.
  __ blx(IP);

  // If the method returns a value, store it to the result pointer.
  char ch = method->GetShorty()->CharAt(0);
  if (ch != 'V') {
    // Load the result JValue pointer of the stub caller's out args.
    __ LoadFromOffset(kLoadWord, IP, SP, frame_size);
    if (ch == 'D' || ch == 'J') {
      __ StoreToOffset(kStoreWordPair, R0, IP, 0);
    } else {
      __ StoreToOffset(kStoreWord, R0, IP, 0);
    }
  }

  // Remove the frame less the spilled R4, R9 and LR
  __ AddConstant(SP, frame_size - (3 * kPointerSize));

  // Pop R4, R9 and the LR into PC
  __ PopList(save | (1 << PC));
  // TODO: store native_entry in the stub table
  std::vector<uint8_t> code(assembler->CodeSize());
  MemoryRegion region(&code[0], code.size());
  assembler->FinalizeInstructions(region);
  return new CompiledInvokeStub(code);
#undef __
}

}  // namespace arm
}  // namespace art
