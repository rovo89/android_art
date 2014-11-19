/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "disassembler_arm64.h"

#include <inttypes.h>

#include <ostream>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "thread.h"

namespace art {
namespace arm64 {

void CustomDisassembler::AppendRegisterNameToOutput(
    const vixl::Instruction* instr,
    const vixl::CPURegister& reg) {
  USE(instr);
  if (reg.IsRegister()) {
    // This enumeration should mirror the declarations in
    // runtime/arch/arm64/registers_arm64.h. We do not include that file to
    // avoid a dependency on libart.
    enum {
      TR  = 18,
      ETR = 21,
      IP0 = 16,
      IP1 = 17,
      FP  = 29,
      LR  = 30
    };
    switch (reg.code()) {
      case IP0: AppendToOutput(reg.Is64Bits() ? "ip0" : "wip0"); return;
      case IP1: AppendToOutput(reg.Is64Bits() ? "ip1" : "wip1"); return;
      case TR:  AppendToOutput(reg.Is64Bits() ? "tr"  :  "w18"); return;
      case ETR: AppendToOutput(reg.Is64Bits() ? "etr" :  "w21"); return;
      case FP:  AppendToOutput(reg.Is64Bits() ? "fp"  :  "w29"); return;
      case LR:  AppendToOutput(reg.Is64Bits() ? "lr"  :  "w30"); return;
      default:
        // Fall through.
        break;
    }
  }
  // Print other register names as usual.
  Disassembler::AppendRegisterNameToOutput(instr, reg);
}

void CustomDisassembler::VisitLoadLiteral(const vixl::Instruction* instr) {
  Disassembler::VisitLoadLiteral(instr);

  if (!read_literals_) {
    return;
  }

  char* buffer = buffer_;
  char* buffer_end = buffer_ + buffer_size_;

  // Find the end position in the buffer.
  while ((*buffer != 0) && (buffer < buffer_end)) {
    ++buffer;
  }

  void* data_address = instr->LiteralAddress();
  ptrdiff_t buf_size_remaining = buffer_end - buffer;
  vixl::Instr op = instr->Mask(vixl::LoadLiteralMask);

  switch (op) {
    case vixl::LDR_w_lit:
    case vixl::LDR_x_lit:
    case vixl::LDRSW_x_lit: {
      int64_t data = op == vixl::LDR_x_lit ? *reinterpret_cast<int64_t*>(data_address)
                                           : *reinterpret_cast<int32_t*>(data_address);
      snprintf(buffer, buf_size_remaining, " (0x%" PRIx64 " / %" PRId64 ")", data, data);
      break;
    }
    case vixl::LDR_s_lit:
    case vixl::LDR_d_lit: {
      double data = (op == vixl::LDR_s_lit) ? *reinterpret_cast<float*>(data_address)
                                            : *reinterpret_cast<double*>(data_address);
      snprintf(buffer, buf_size_remaining, " (%g)", data);
      break;
    }
    default:
      break;
  }
}

size_t DisassemblerArm64::Dump(std::ostream& os, const uint8_t* begin) {
  const vixl::Instruction* instr = reinterpret_cast<const vixl::Instruction*>(begin);
  decoder.Decode(instr);
  // TODO: Use FormatInstructionPointer() once VIXL provides the appropriate
  // features.
  // VIXL does not yet allow remapping addresses disassembled. Using
  // FormatInstructionPointer() would show incoherences between the instruction
  // location addresses and the target addresses disassembled by VIXL (eg. for
  // branch instructions).
  os << StringPrintf("%p", instr)
     << StringPrintf(": %08x\t%s\n", instr->InstructionBits(), disasm.GetOutput());
  return vixl::kInstructionSize;
}

void DisassemblerArm64::Dump(std::ostream& os, const uint8_t* begin, const uint8_t* end) {
  for (const uint8_t* cur = begin; cur < end; cur += vixl::kInstructionSize) {
    Dump(os, cur);
  }
}

}  // namespace arm64
}  // namespace art
