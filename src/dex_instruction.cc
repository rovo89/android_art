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

#include "dex_instruction.h"

#include "dex_file.h"
#include <iomanip>

namespace art {

const char* const Instruction::kInstructionNames[] = {
#define INSTRUCTION_NAME(o, c, pname, f, r, i, a, v) pname,
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_NAME)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_NAME
};

Instruction::InstructionFormat const Instruction::kInstructionFormats[] = {
#define INSTRUCTION_FORMAT(o, c, p, format, r, i, a, v) format,
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_FORMAT)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_FORMAT
};

int const Instruction::kInstructionFlags[] = {
#define INSTRUCTION_FLAGS(o, c, p, f, r, i, flags, v) flags,
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_FLAGS)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_FLAGS
};

int const Instruction::kInstructionVerifyFlags[] = {
#define INSTRUCTION_VERIFY_FLAGS(o, c, p, f, r, i, a, vflags) vflags,
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_VERIFY_FLAGS)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_VERIFY_FLAGS
};

/*
 * Handy macros for helping decode instructions.
 */
#define FETCH(_offset)      (insns[(_offset)])
#define FETCH_u4(_offset)   (fetch_u4_impl((_offset), insns))
#define INST_A(_insn)       (((uint16_t)(_insn) >> 8) & 0x0f)
#define INST_B(_insn)       ((uint16_t)(_insn) >> 12)
#define INST_AA(_insn)      ((_insn) >> 8)

/* Helper for FETCH_u4, above. */
static inline uint32_t fetch_u4_impl(uint32_t offset, const uint16_t* insns) {
  return insns[offset] | ((uint32_t) insns[offset+1] << 16);
}

void Instruction::Decode(uint32_t &vA, uint32_t &vB, uint64_t &vB_wide, uint32_t &vC, uint32_t arg[]) const {
  const uint16_t* insns = reinterpret_cast<const uint16_t*>(this);
  uint16_t insn = *insns;
  int opcode = insn & 0xFF;

  switch (Format()) {
    case k10x:       // op
      /* nothing to do; copy the AA bits out for the verifier */
      vA = INST_AA(insn);
      break;
    case k12x:       // op vA, vB
      vA = INST_A(insn);
      vB = INST_B(insn);
      break;
    case k11n:       // op vA, #+B
      vA = INST_A(insn);
      vB = (int32_t) (INST_B(insn) << 28) >> 28; // sign extend 4-bit value
      break;
    case k11x:       // op vAA
      vA = INST_AA(insn);
      break;
    case k10t:       // op +AA
      vA = (int8_t) INST_AA(insn);              // sign-extend 8-bit value
      break;
    case k20bc:      // op AA, kind@BBBB
      break;
    case k20t:       // op +AAAA
      vA = (int16_t) FETCH(1);                   // sign-extend 16-bit value
      break;
    case k21c:       // op vAA, thing@BBBB
    case k22x:       // op vAA, vBBBB
      vA = INST_AA(insn);
      vB = FETCH(1);
      break;
    case k21s:       // op vAA, #+BBBB
    case k21t:       // op vAA, +BBBB
      vA = INST_AA(insn);
      vB = (int16_t) FETCH(1);                   // sign-extend 16-bit value
      break;
    case k21h:       // op vAA, #+BBBB0000[00000000]
      vA = INST_AA(insn);
      /*
       * The value should be treated as right-zero-extended, but we don't
       * actually do that here. Among other things, we don't know if it's
       * the top bits of a 32- or 64-bit value.
       */
      vB = FETCH(1);
      break;
    case k23x:       // op vAA, vBB, vCC
      vA = INST_AA(insn);
      vB = FETCH(1) & 0xff;
      vC = FETCH(1) >> 8;
      break;
    case k22b:       // op vAA, vBB, #+CC
      vA = INST_AA(insn);
      vB = FETCH(1) & 0xff;
      vC = (int8_t) (FETCH(1) >> 8);            // sign-extend 8-bit value
      break;
    case k22s:       // op vA, vB, #+CCCC
    case k22t:       // op vA, vB, +CCCC
      vA = INST_A(insn);
      vB = INST_B(insn);
      vC = (int16_t) FETCH(1);                   // sign-extend 16-bit value
      break;
    case k22c:       // op vA, vB, thing@CCCC
      vA = INST_A(insn);
      vB = INST_B(insn);
      vC = FETCH(1);
      break;
    case k30t:       // op +AAAAAAAA
      vA = FETCH_u4(1);                     // signed 32-bit value
      break;
    case k31t:       // op vAA, +BBBBBBBB
    case k31c:       // op vAA, string@BBBBBBBB
      vA = INST_AA(insn);
      vB = FETCH_u4(1);                     // 32-bit value
      break;
    case k32x:       // op vAAAA, vBBBB
      vA = FETCH(1);
      vB = FETCH(2);
      break;
    case k31i:       // op vAA, #+BBBBBBBB
      vA = INST_AA(insn);
      vB = FETCH_u4(1);                     // signed 32-bit value
      break;
    case k35c:       // op {vC, vD, vE, vF, vG}, thing@BBBB
      {
        /*
         * Note that the fields mentioned in the spec don't appear in
         * their "usual" positions here compared to most formats. This
         * was done so that the field names for the argument count and
         * reference index match between this format and the corresponding
         * range formats (3rc and friends).
         *
         * Bottom line: The argument count is always in vA, and the
         * method constant (or equivalent) is always in vB.
         */
        uint16_t regList;
        int count;

        vA = INST_B(insn); // This is labeled A in the spec.
        vB = FETCH(1);
        regList = FETCH(2);

        count = vA;

        /*
         * Copy the argument registers into the arg[] array, and
         * also copy the first argument (if any) into vC. (The
         * DecodedInstruction structure doesn't have separate
         * fields for {vD, vE, vF, vG}, so there's no need to make
         * copies of those.) Note that cases 5..2 fall through.
         */
        switch (count) {
        case 5: arg[4] = INST_A(insn);
        case 4: arg[3] = (regList >> 12) & 0x0f;
        case 3: arg[2] = (regList >> 8) & 0x0f;
        case 2: arg[1] = (regList >> 4) & 0x0f;
        case 1: vC = arg[0] = regList & 0x0f; break;
        case 0: break; // Valid, but no need to do anything.
        default:
          LOG(ERROR) << "Invalid arg count in 35c (" << count << ")";
          return;
        }
      }
      break;
    case k3rc:       // op {vCCCC .. v(CCCC+AA-1)}, meth@BBBB
      vA = INST_AA(insn);
      vB = FETCH(1);
      vC = FETCH(2);
        break;
    case k51l:       // op vAA, #+BBBBBBBBBBBBBBBB
      vA = INST_AA(insn);
      vB_wide = FETCH_u4(1) | ((uint64_t) FETCH_u4(3) << 32);
      break;
    default:
      LOG(ERROR) << "Can't decode unexpected format " << static_cast<int>(Format()) << " (op=" << opcode << ")";
      return;
  }
}

size_t Instruction::SizeInCodeUnits() const {
  const uint16_t* insns = reinterpret_cast<const uint16_t*>(this);
  if (*insns == kPackedSwitchSignature) {
    return (4 + insns[1] * 2);
  } else if (*insns == kSparseSwitchSignature) {
    return (2 + insns[1] * 4);
  } else if (*insns == kArrayDataSignature) {
    uint16_t element_size = insns[1];
    uint32_t length = insns[2] | (((uint32_t)insns[3]) << 16);
    // The plus 1 is to round up for odd size and width.
    return (4 + (element_size * length + 1) / 2);
  } else {
    switch (Format()) {
      case k10x:
      case k12x:
      case k11n:
      case k11x:
      case k10t:
        return 1;
      case k20bc:
      case k20t:
      case k22x:
      case k21t:
      case k21s:
      case k21h:
      case k21c:
      case k23x:
      case k22b:
      case k22t:
      case k22s:
      case k22c:
        return 2;
      case k32x:
      case k30t:
      case k31t:
      case k31i:
      case k31c:
      case k35c:
      case k3rc:
        return 3;
      case k51l:
        return 5;
      default:
        LOG(FATAL) << "Unreachable";
    }
  }
  return 0;
}

Instruction::Code Instruction::Opcode() const {
  const uint16_t* insns = reinterpret_cast<const uint16_t*>(this);
  int opcode = *insns & 0xFF;
  return static_cast<Code>(opcode);
}

const Instruction* Instruction::Next() const {
  size_t current_size_in_bytes = SizeInCodeUnits() * sizeof(uint16_t);
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(this);
  return reinterpret_cast<const Instruction*>(ptr + current_size_in_bytes);
}

std::string Instruction::DumpHex(size_t code_units) const {
  size_t inst_length = SizeInCodeUnits();
  if (inst_length > code_units) {
    inst_length = code_units;
  }
  std::ostringstream os;
  const uint16_t* insn = reinterpret_cast<const uint16_t*>(this);
  for (size_t i = 0; i < inst_length; i++) {
    os << StringPrintf("0x%04x", insn[i]) << " ";
  }
  for (size_t i = inst_length; i < code_units; i++) {
    os << "       ";
  }
  return os.str();
}

std::string Instruction::DumpString(const DexFile* file) const {
  DecodedInstruction insn(this);
  std::ostringstream os;
  const char* opcode = kInstructionNames[insn.opcode_];
  switch (Format()) {
    case k10x: os << opcode; break;
    case k12x: os << opcode << " v" << insn.vA_ << ", v" << insn.vB_; break;
    case k11n: os << opcode << " v" << insn.vA_ << ", #+" << insn.vB_; break;
    case k11x: os << opcode << " v" << insn.vA_; break;
    case k10t: os << opcode << " +" << (int)insn.vA_; break;
    case k20bc: os << opcode << " " << insn.vA_ << ", kind@" << insn.vB_; break;
    case k20t: os << opcode << " +" << (int)insn.vA_; break;
    case k22x: os << opcode << " v" << insn.vA_ << ", v" << insn.vB_; break;
    case k21t: os << opcode << " v" << insn.vA_ << ", +" << insn.vB_; break;
    case k21s: os << opcode << " v" << insn.vA_ << ", #+" << insn.vB_; break;
    case k21h: os << opcode << " v" << insn.vA_ << ", #+" << insn.vB_ << "00000[00000000]"; break;
    case k21c: os << opcode << " " << insn.vA_ << ", thing@" << insn.vB_; break;
    case k23x: os << opcode << " v" << insn.vA_ << ", v" << insn.vB_ << ", v" << insn.vC_; break;
    case k22b: os << opcode << " v" << insn.vA_ << ", v" << insn.vB_ << ", #+" << insn.vC_; break;
    case k22t: os << opcode << " v" << insn.vA_ << ", v" << insn.vB_ << ", +" << insn.vC_; break;
    case k22s: os << opcode << " v" << insn.vA_ << ", v" << insn.vB_ << ", #+" << insn.vC_; break;
    case k22c: os << opcode << " v" << insn.vA_ << ", v" << insn.vB_ << ", thing@" << insn.vC_; break;
    case k32x: os << opcode << " v" << insn.vA_ << ", v" << insn.vB_; break;
    case k30t: os << opcode << " +" << (int)insn.vA_; break;
    case k31t: os << opcode << " v" << insn.vA_ << ", +" << insn.vB_; break;
    case k31i: os << opcode << " v" << insn.vA_ << ", #+" << insn.vB_; break;
    case k31c: os << opcode << " v" << insn.vA_ << ", thing@" << insn.vB_; break;
    case k35c: {
      switch (insn.opcode_) {
        case INVOKE_VIRTUAL:
        case INVOKE_SUPER:
        case INVOKE_DIRECT:
        case INVOKE_STATIC:
        case INVOKE_INTERFACE:
          if (file != NULL) {
            const DexFile::MethodId& meth_id = file->GetMethodId(insn.vB_);
            os << opcode << " {v" << insn.arg_[0] << ", v" << insn.arg_[1] << ", v" << insn.arg_[2]
                         << ", v" << insn.arg_[3] << ", v" << insn.arg_[4] << "}, "
                         << file->GetMethodDeclaringClassDescriptor(meth_id) << "."
                         << file->GetMethodName(meth_id) << file->GetMethodSignature(meth_id)
                         << " // method@" << insn.vB_;
            break;
          }  // else fall-through
        default:
          os << opcode << " {v" << insn.arg_[0] << ", v" << insn.arg_[1] << ", v" << insn.arg_[2]
                       << ", v" << insn.arg_[3] << ", v" << insn.arg_[4] << "}, thing@" << insn.vB_;
          break;
      }
      break;
    }
    case k3rc: os << opcode << " {v" << insn.vC_ << " .. v" << (insn.vC_+ insn.vA_ - 1) << "}, method@" << insn.vB_; break;
    case k51l: os << opcode << " v" << insn.vA_ << ", #+" << insn.vB_; break;
    default: os << " unknown format (" << DumpHex(5) << ")"; break;
  }
  return os.str();
}

}  // namespace art
