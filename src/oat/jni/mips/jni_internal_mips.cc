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

#include <stdint.h>

#include <algorithm>

#include "asm_support.h"
#include "compiled_method.h"
#include "compiler.h"
#include "jni_internal.h"
#include "oat/utils/mips/assembler_mips.h"
#include "oat/utils/assembler.h"
#include "object.h"

namespace art {
namespace mips {
// Creates a function which invokes a managed method with an array of
// arguments.
//
// At the time of call, the environment looks something like this:
//
// A0 = method pointer
// A1 = receiver pointer or NULL for static methods
// A2 = (managed) thread pointer
// A3 = argument array or NULL for no argument methods
// [SP] = JValue* result or NULL for void returns
//
// As the JNI call has already transitioned the thread into the
// "running" state the remaining responsibilities of this routine are
// to save the native register value and restore the managed thread
// register and transfer arguments from the array into register and on
// the stack, if needed.  On return, the thread register must be
// shuffled and the return value must be store into the result JValue.
CompiledInvokeStub* CreateInvokeStub(bool is_static, const char* shorty, uint32_t shorty_len) {
  UniquePtr<MipsAssembler> assembler(down_cast<MipsAssembler*>(Assembler::Create(kMips)));
#define __ assembler->
  size_t num_arg_array_bytes = NumArgArrayBytes(shorty, shorty_len);
  // Size of frame = spill of R4,R9/LR + Method* + possible receiver + arg array size
  // Note, space is left in the frame to flush arguments in registers back to out locations.
  size_t unpadded_frame_size = (4 * kPointerSize) +
                               (is_static ? 0 : kPointerSize) +
                               num_arg_array_bytes;
  size_t frame_size = RoundUp(unpadded_frame_size, kStackAlignment);

  // Setup frame and spill S0 (rSUSPEND), S1 (rSELF), and RA
  __ AddConstant(SP, SP, -frame_size);
  __ StoreToOffset(kStoreWord, RA, SP, frame_size - 4);
  __ StoreToOffset(kStoreWord, S1, SP, frame_size - 8);
  __ StoreToOffset(kStoreWord, S0, SP, frame_size - 12);

  // Move the managed thread pointer into S1.
  __ Move(S1, A2);

  // Reset S0 to suspend check interval
  __ LoadImmediate(S0, SUSPEND_CHECK_INTERVAL);

  // Can either get 3 or 2 arguments into registers
  size_t reg_bytes = (is_static ? 3 : 2) * kPointerSize;
  if (num_arg_array_bytes <= reg_bytes) {
    reg_bytes = num_arg_array_bytes;
  }

  // Method* at bottom of frame is null thereby terminating managed stack crawls
  __ StoreToOffset(kStoreWord, ZERO, SP, 0);

  // Copy values onto the stack.
  size_t src_offset = 0;
  size_t dst_offset = (is_static ? 1 : 2) * kPointerSize;
  for (size_t i = 1; i < shorty_len; ++i) {
    switch (shorty[i]) {
      case 'D':
      case 'J':
        // Move both pointers 64 bits.
        __ LoadFromOffset(kLoadWord, T9, A3, src_offset);
        src_offset += kPointerSize;
        __ StoreToOffset(kStoreWord, T9, SP, dst_offset);
        dst_offset += kPointerSize;

        __ LoadFromOffset(kLoadWord, T9, A3, src_offset);
        src_offset += kPointerSize;
        __ StoreToOffset(kStoreWord, T9, SP, dst_offset);
        dst_offset += kPointerSize;
        break;
      default:
        // Move the source pointer sizeof(JValue) and the destination pointer 32 bits.
        __ LoadFromOffset(kLoadWord, T9, A3, src_offset);
        src_offset += sizeof(JValue);
        __ StoreToOffset(kStoreWord, T9, SP, dst_offset);
        dst_offset += kPointerSize;
        break;
    }
  }

  // Move all the register arguments into place.
  dst_offset = (is_static ? 1 : 2) * kPointerSize;
  if (is_static) {
    if (reg_bytes > 0 && num_arg_array_bytes > 0) {
      __ LoadFromOffset(kLoadWord, A1, SP, dst_offset + 0);
      if (reg_bytes > 4 && num_arg_array_bytes > 4) {
        __ LoadFromOffset(kLoadWord, A2, SP, dst_offset + 4);
        if (reg_bytes > 8 && num_arg_array_bytes > 8) {
          __ LoadFromOffset(kLoadWord, A3, SP, dst_offset + 8);
        }
      }
    }
  } else {
    if (reg_bytes > 0 && num_arg_array_bytes > 0) {
      __ LoadFromOffset(kLoadWord, A2, SP, dst_offset + 0);
      if (reg_bytes > 4 && num_arg_array_bytes > 4) {
        __ LoadFromOffset(kLoadWord, A3, SP, dst_offset + 4);
      }
    }
  }

  // Load the code pointer we are about to call.
  __ LoadFromOffset(kLoadWord, T9, A0, AbstractMethod::GetCodeOffset().Int32Value());

  // Do the call.
  __ Jalr(T9);

  // If the method returns a value, store it to the result pointer.
  if (shorty[0] != 'V') {
    // Load the result JValue pointer of the stub caller's out args.
    __ LoadFromOffset(kLoadWord, T9, SP, frame_size + 16);
    switch (shorty[0]) {
      case 'D':
        __ StoreDToOffset(D0, T9, 0);
        break;
      case 'F':
        __ StoreFToOffset(F0, T9, 0);
        break;
      case 'J':
        __ StoreToOffset(kStoreWord, V0, T9, 0);
        __ StoreToOffset(kStoreWord, V1, T9, 4);
        break;
      default:
        __ StoreToOffset(kStoreWord, V0, T9, 0);
    }
  }

  // Restore frame and spill regs
  __ LoadFromOffset(kLoadWord, S0, SP, frame_size - 12);
  __ LoadFromOffset(kLoadWord, S1, SP, frame_size - 8);
  __ LoadFromOffset(kLoadWord, RA, SP, frame_size - 4);
  __ AddConstant(SP, SP, frame_size);

  __ Jr(RA);

  // TODO: store native_entry in the stub table
  std::vector<uint8_t> code(assembler->CodeSize());
  MemoryRegion region(&code[0], code.size());
  assembler->FinalizeInstructions(region);
  return new CompiledInvokeStub(kMips, code);
#undef __
}
}  // namespace mips
}  // namespace art

extern "C" art::CompiledInvokeStub* ArtCreateInvokeStub(art::Compiler& /*compiler*/, bool is_static,
                                                        const char* shorty, uint32_t shorty_len) {
  return art::mips::CreateInvokeStub(is_static, shorty, shorty_len);
}
