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

#include "compiled_method.h"
#include "compiler.h"
#include "oat/utils/assembler.h"
#include "oat/utils/x86/assembler_x86.h"
#include "object.h"

namespace art {
namespace x86 {

// Creates a function which invokes a managed method with an array of
// arguments.
//
// Immediately after the call on X86, the environment looks like this:
//
// [SP+0 ] = Return address
// [SP+4 ] = method pointer
// [SP+8 ] = receiver pointer or NULL for static methods
// [SP+12] = (managed) thread pointer
// [SP+16] = argument array or NULL for no argument methods
// [SP+20] = JValue* result or NULL for void returns
//
// As the JNI call has already transitioned the thread into the
// "running" state the remaining responsibilities of this routine are
// to save the native registers and set up the managed registers. On
// return, the return value must be store into the result JValue.
CompiledInvokeStub* CreateInvokeStub(bool is_static, const char* shorty, uint32_t shorty_len) {
  UniquePtr<X86Assembler> assembler(down_cast<X86Assembler*>(Assembler::Create(kX86)));
#define __ assembler->
  size_t num_arg_array_bytes = NumArgArrayBytes(shorty, shorty_len);
  // Size of frame = return address + saved EBX + Method* + possible receiver + arg array size
  // Note, space is left in the frame to flush arguments in registers back to out locations.
  size_t frame_size = 3 * kPointerSize + (is_static ? 0 : kPointerSize) + num_arg_array_bytes;
  size_t pad_size = RoundUp(frame_size, kStackAlignment) - frame_size;

  Register rMethod = EAX;
  __ movl(rMethod,   Address(ESP, 4));     // EAX = method
  Register rReceiver = ECX;
  if (!is_static) {
    __ movl(rReceiver, Address(ESP, 8));   // ECX = receiver
  }
  // Save EBX
  __ pushl(EBX);
  Register rArgArray = EBX;
  __ movl(rArgArray, Address(ESP, 20));    // EBX = arg array

  // TODO: optimize the frame set up to avoid excessive SP math
  // Push padding
  if (pad_size != 0) {
    __ subl(ESP, Immediate(pad_size));
  }
  // Push/copy arguments.
  size_t arg_count = (shorty_len - 1);
  size_t dst_offset = num_arg_array_bytes;
  size_t src_offset = arg_count * sizeof(JValue);
  for (size_t i = shorty_len - 1; i > 0; --i) {
    switch (shorty[i]) {
      case 'D':
      case 'J':
        // Move both pointers 64 bits.
        dst_offset -= kPointerSize;
        src_offset -= sizeof(JValue) / 2;
        __ pushl(Address(rArgArray, src_offset));
        dst_offset -= kPointerSize;
        src_offset -= sizeof(JValue) / 2;
        __ pushl(Address(rArgArray, src_offset));
        break;
      default:
        // Move the source pointer sizeof(JValue) and the destination pointer 32 bits.
        dst_offset -= kPointerSize;
        src_offset -= sizeof(JValue);
        __ pushl(Address(rArgArray, src_offset));
        break;
    }
  }

  // Backing space for receiver.
  if (!is_static) {
    __ pushl(Immediate(0));
  }
  // Push 0 as NULL Method* thereby terminating managed stack crawls.
  __ pushl(Immediate(0));
  if (!is_static) {
    if (shorty_len > 1) {
      // Receiver already in ECX, pass remaining 2 args in EDX and EBX.
      __ movl(EDX, Address(rArgArray, 0));
      if (shorty[1] == 'D' || shorty[1] == 'J') {
        __ movl(EBX, Address(rArgArray, sizeof(JValue) / 2));
      } else if (shorty_len > 2) {
        __ movl(EBX, Address(rArgArray, sizeof(JValue)));
      }
    }
  } else {
    if (shorty_len > 1) {
      // Pass remaining 3 args in ECX, EDX and EBX.
      __ movl(ECX, Address(rArgArray, 0));
      if (shorty[1] == 'D' || shorty[1] == 'J') {
        __ movl(EDX, Address(rArgArray, sizeof(JValue) / 2));
        if (shorty_len > 2) {
           __ movl(EBX, Address(rArgArray, sizeof(JValue)));
        }
      } else if (shorty_len > 2) {
        __ movl(EDX, Address(rArgArray, sizeof(JValue)));
        if (shorty[2] == 'D' || shorty[2] == 'J') {
          __ movl(EBX, Address(rArgArray, sizeof(JValue) + (sizeof(JValue) / 2)));
        } else {
          __ movl(EBX, Address(rArgArray, sizeof(JValue) + sizeof(JValue)));
        }
      }
    }
  }

  __ call(Address(EAX, Method::GetCodeOffset()));  // Call code off of method

  // Pop arguments up to EBX and the return address.
  __ addl(ESP, Immediate(frame_size + pad_size - (2 * kPointerSize)));
  // Restore EBX.
  __ popl(EBX);
  char ch = shorty[0];
  if (ch != 'V') {
    // Load the result JValue pointer.
    __ movl(ECX, Address(ESP, 20));
    switch (ch) {
      case 'D':
        __ movsd(Address(ECX, 0), XMM0);
        break;
      case 'F':
        __ movss(Address(ECX, 0), XMM0);
        break;
      case 'J':
        __ movl(Address(ECX, 0), EAX);
        __ movl(Address(ECX, 4), EDX);
        break;
      default:
        __ movl(Address(ECX, 0), EAX);
        break;
    }
  }
  __ ret();
  // TODO: store native_entry in the stub table
  std::vector<uint8_t> code(assembler->CodeSize());
  MemoryRegion region(&code[0], code.size());
  assembler->FinalizeInstructions(region);
  return new CompiledInvokeStub(code);
#undef __
}

}  // namespace x86
}  // namespace art

extern "C" art::CompiledInvokeStub* ArtCreateInvokeStub(art::Compiler& /*compiler*/, bool is_static,
                                                        const char* shorty, uint32_t shorty_len) {
  return art::x86::CreateInvokeStub(is_static, shorty, shorty_len);
}
