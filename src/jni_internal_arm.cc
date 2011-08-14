// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include <algorithm>

#include "assembler.h"
#include "object.h"

#define __ assembler->

namespace art {

// Creates a function which invokes a managed method with an array of
// arguments.
//
// At the time of call, the environment looks something like this:
//
// R0 = method pointer
// R1 = receiver pointer or NULL for static methods
// R2 = (managed) thread pointer
// R3 = argument array or NULL for void arugment methods
// [SP] = JValue result or NULL for void returns
//
// As the JNI call has already transitioned the thread into the
// "running" state the remaining responsibilities of this routine are
// to save the native register value and restore the managed thread
// register and transfer arguments from the array into register and on
// the stack, if needed.  On return, the thread register must be
// shuffled and the return value must be store into the result JValue.
void CreateInvokeStub(Assembler* assembler, Method* method) {
  size_t num_arg_words = method->NumArgArrayBytes() / kWordSize;
  // TODO: the incoming argument array should have implicit arguments.
  size_t max_register_words = method->IsStatic() ? 3 : 2;
  size_t num_register_words = std::min(num_arg_words, max_register_words);
  size_t num_stack_words = num_arg_words - num_register_words;

  // For now we allocate stack space for stacked outgoing arguments.
  size_t stack_parameters_size = num_stack_words*kWordSize;
  if (num_arg_words != RoundUp(num_arg_words,8)) {
    // Ensure 8-byte alignment.
    stack_parameters_size += kWordSize;
  }

  RegList save = (1 << R9);
  __ PushList(save | (1 << LR));

  // Allocate a frame large enough for the stacked arguments.
  __ AddConstant(SP, -stack_parameters_size);

  // Move the managed thread pointer into R9.
  __ mov(R9, ShifterOperand(R2));

  // Move all stacked arguments into place.
  size_t first_stack_word = num_register_words;
  if (num_arg_words > max_register_words) {
    for (size_t i = first_stack_word, j = 0; i < num_arg_words; ++i, ++j) {
      int r3_offset = i * kWordSize;
      int sp_offset = j * kWordSize;
      __ LoadFromOffset(kLoadWord, IP, R3, r3_offset);
      __ StoreToOffset(kStoreWord, IP, SP, sp_offset);
    }
  }

  // Move all the register arguments into place.
  if (method->IsStatic()) {
    if (num_register_words > 0) {
      __ LoadFromOffset(kLoadWord, R1, R3, 0);
    }
    if (num_register_words > 1) {
      __ LoadFromOffset(kLoadWord, R2, R3, 4);
    }
    if (num_register_words > 2) {
      __ LoadFromOffset(kLoadWord, R3, R3, 8);
    }
  } else {
    if (num_register_words > 0) {
      __ LoadFromOffset(kLoadWord, R2, R3, 0);
    }
    if (num_register_words > 1) {
      __ LoadFromOffset(kLoadWord, R3, R3, 4);
    }
  }

  // Allocate the spill area for outgoing arguments.
  __ AddConstant(SP, -((num_register_words+1)*kWordSize));

  // Load the code pointer we are about to call.
  __ LoadFromOffset(kLoadWord, IP, R0, method->GetCodeOffset());

  // Do the call.
  __ blx(IP);

  // Deallocate the spill area for outgoing arguments.
  __ AddConstant(SP, ((num_register_words+1)*kWordSize));

  // If the method returns a value, store it to the result pointer.
  char ch = method->GetShorty()[0];
  if (ch != 'V') {
    // Load the result JValue pointer.  It is the first stacked
    // argument so it is stored above the stacked R9 and LR values.
    __ LoadFromOffset(kLoadWord, IP, SP, stack_parameters_size + 2*kWordSize);
    if (ch == 'D' || ch == 'J') {
      __ StoreToOffset(kStoreWordPair, R0, IP, 0);
    } else {
      __ StoreToOffset(kStoreWord, R0, IP, 0);
    }
  }

  __ AddConstant(SP, stack_parameters_size);
  __ PopList(save | (1 << PC));
}

}  // namespace art
