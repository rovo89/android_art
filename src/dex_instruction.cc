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

Instruction::Format const Instruction::kInstructionFormats[] = {
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

  switch (FormatOf(Opcode())) {
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
    case k20t:       // op +AAAA
      vA = (int16_t) FETCH(1);                   // sign-extend 16-bit value
      break;
    case k20bc:      // op AA, kind@BBBB
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
      LOG(ERROR) << "Can't decode unexpected format " << static_cast<int>(FormatOf(Opcode())) << " (op=" << opcode << ")";
      return;
  }
}

size_t Instruction::SizeInCodeUnits() const {
  const uint16_t* insns = reinterpret_cast<const uint16_t*>(this);
  if (*insns == Instruction::kPackedSwitchSignature) {
    return (4 + insns[1] * 2);
  } else if (*insns == Instruction::kSparseSwitchSignature) {
    return (2 + insns[1] * 4);
  } else if (*insns == kArrayDataSignature) {
    uint16_t element_size = insns[1];
    uint32_t length = insns[2] | (((uint32_t)insns[3]) << 16);
    // The plus 1 is to round up for odd size and width.
    return (4 + (element_size * length + 1) / 2);
  } else {
    switch (FormatOf(Opcode())) {
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
  const char* opcode = kInstructionNames[insn.opcode];
  switch (FormatOf(Opcode())) {
    case k10x:  os << opcode; break;
    case k12x:  os << StringPrintf("%s v%d, v%d", opcode, insn.vA, insn.vB); break;
    case k11n:  os << StringPrintf("%s v%d, #%+d", opcode, insn.vA, insn.vB); break;
    case k11x:  os << StringPrintf("%s v%d", opcode, insn.vA); break;
    case k10t:  os << StringPrintf("%s %+d", opcode, insn.vA); break;
    case k20bc: os << StringPrintf("%s %d, kind@%d", opcode, insn.vA, insn.vB); break;
    case k20t:  os << StringPrintf("%s %+d", opcode, insn.vA); break;
    case k22x:  os << StringPrintf("%s v%d, v%d", opcode, insn.vA, insn.vB); break;
    case k21t:  os << StringPrintf("%s v%d, %+d", opcode, insn.vA, insn.vB); break;
    case k21s:  os << StringPrintf("%s v%d, #%+d", opcode, insn.vA, insn.vB); break;
    case k21h: {
        // op vAA, #+BBBB0000[00000000]
        if (insn.opcode == CONST_HIGH16) {
          uint32_t value = insn.vB << 16;
          os << StringPrintf("%s v%d, #int %+d // 0x%x", opcode, insn.vA, value, value);
        } else {
          uint64_t value = static_cast<uint64_t>(insn.vB) << 48;
          os << StringPrintf("%s v%d, #long %+lld // 0x%llx", opcode, insn.vA, value, value);
        }
      }
      break;
    case k21c: {
      switch (insn.opcode) {
        case CHECK_CAST:
          if (file != NULL) {
            os << StringPrintf("check-cast v%d, %s // type@%d", insn.vA,
                               file->StringByTypeIdx(insn.vB), insn.vB);
            break;
          }  // else fall-through
        case CONST_CLASS:
          if (file != NULL) {
            os << StringPrintf("const-class v%d, %s // type@%d", insn.vA,
                               file->StringByTypeIdx(insn.vB), insn.vB);
            break;
          }  // else fall-through
        case CONST_STRING:
          if (file != NULL) {
            os << StringPrintf("const-string v%d, \"%s\" // string@%d", insn.vA,
                               file->StringDataByIdx(insn.vB), insn.vB);
            break;
          }  // else fall-through
        case NEW_INSTANCE:
          if (file != NULL) {
            os << StringPrintf("new-instance v%d, %s // type@%d", insn.vA,
                               file->StringByTypeIdx(insn.vB), insn.vB);
            break;
          }  // else fall-through
        case SGET:
        case SGET_WIDE:
        case SGET_OBJECT:
        case SGET_BOOLEAN:
        case SGET_BYTE:
        case SGET_CHAR:
        case SGET_SHORT:
          if (file != NULL) {
            const DexFile::FieldId& field_id = file->GetFieldId(insn.vB);
            os << StringPrintf("%s v%d, (%s) %s.%s // field@%d", opcode, insn.vA,
                               file->GetFieldTypeDescriptor(field_id),
                               file->GetFieldDeclaringClassDescriptor(field_id),
                               file->GetFieldName(field_id), insn.vB);
            break;
          }  // else fall-through
        case SPUT:
        case SPUT_WIDE:
        case SPUT_OBJECT:
        case SPUT_BOOLEAN:
        case SPUT_BYTE:
        case SPUT_CHAR:
        case SPUT_SHORT:
          if (file != NULL) {
            const DexFile::FieldId& field_id = file->GetFieldId(insn.vB);
            os << StringPrintf("%s (%s) %s.%s, v%d // field@%d", opcode,
                               file->GetFieldTypeDescriptor(field_id),
                               file->GetFieldDeclaringClassDescriptor(field_id),
                               file->GetFieldName(field_id), insn.vA, insn.vB);
            break;
          }  // else fall-through
        default:
          os << StringPrintf("%s v%d, thing@%d", opcode, insn.vA, insn.vB);
          break;
      }
      break;
    }
    case k23x:  os << StringPrintf("%s v%d, v%d, v%d", opcode, insn.vA, insn.vB, insn.vC); break;
    case k22b:  os << StringPrintf("%s v%d, v%d, #%+d", opcode, insn.vA, insn.vB, insn.vC); break;
    case k22t:  os << StringPrintf("%s v%d, v%d, %+d", opcode, insn.vA, insn.vB, insn.vC); break;
    case k22s:  os << StringPrintf("%s v%d, v%d, #%+d", opcode, insn.vA, insn.vB, insn.vC); break;
    case k22c: {
      switch (insn.opcode) {
        case IGET:
        case IGET_WIDE:
        case IGET_OBJECT:
        case IGET_BOOLEAN:
        case IGET_BYTE:
        case IGET_CHAR:
        case IGET_SHORT:
          if (file != NULL) {
            const DexFile::FieldId& field_id = file->GetFieldId(insn.vC);
            os << StringPrintf("%s v%d, v%d, (%s) %s.%s // field@%d", opcode, insn.vA, insn.vB,
                               file->GetFieldTypeDescriptor(field_id),
                               file->GetFieldDeclaringClassDescriptor(field_id),
                               file->GetFieldName(field_id), insn.vC);
            break;
          }  // else fall-through
        case IPUT:
        case IPUT_WIDE:
        case IPUT_OBJECT:
        case IPUT_BOOLEAN:
        case IPUT_BYTE:
        case IPUT_CHAR:
        case IPUT_SHORT:
          if (file != NULL) {
            const DexFile::FieldId& field_id = file->GetFieldId(insn.vC);
            os << StringPrintf("%s v%d, (%s) %s.%s, v%d // field@%d", opcode, insn.vA,
                               file->GetFieldTypeDescriptor(field_id),
                               file->GetFieldDeclaringClassDescriptor(field_id),
                               file->GetFieldName(field_id), insn.vB, insn.vC);
            break;
          }  // else fall-through
        case INSTANCE_OF:
          if (file != NULL) {
            os << StringPrintf("instance-of v%d, v%d, %s // type@%d", insn.vA, insn.vB,
                               file->StringByTypeIdx(insn.vC), insn.vC);
            break;
          }  // else fall-through
        default:
          os << StringPrintf("%s v%d, v%d, thing@%d", opcode, insn.vA, insn.vB, insn.vC);
          break;
      }
      break;
    }
    case k32x:  os << StringPrintf("%s v%d, v%d", opcode, insn.vA, insn.vB); break;
    case k30t:  os << StringPrintf("%s %+d", opcode, insn.vA); break;
    case k31t:  os << StringPrintf("%s v%d, %+d", opcode, insn.vA, insn.vB); break;
    case k31i:  os << StringPrintf("%s v%d, #%+d", opcode, insn.vA, insn.vB); break;
    case k31c:  os << StringPrintf("%s v%d, thing@%d", opcode, insn.vA, insn.vB); break;
    case k35c: {
      switch (insn.opcode) {
        case INVOKE_VIRTUAL:
        case INVOKE_SUPER:
        case INVOKE_DIRECT:
        case INVOKE_STATIC:
        case INVOKE_INTERFACE:
          if (file != NULL) {
            const DexFile::MethodId& meth_id = file->GetMethodId(insn.vB);
            os << opcode << " {";
            for (size_t i = 0; i < insn.vA; ++i) {
              if (i != 0) {
                os << ", ";
              }
              os << "v" << insn.arg[i];
            }
            os << "}, "
               << file->GetMethodDeclaringClassDescriptor(meth_id) << "."
               << file->GetMethodName(meth_id) << file->GetMethodSignature(meth_id)
               << " // method@" << insn.vB;
            break;
          }  // else fall-through
        default:
          os << opcode << " {v" << insn.arg[0] << ", v" << insn.arg[1] << ", v" << insn.arg[2]
                       << ", v" << insn.arg[3] << ", v" << insn.arg[4] << "}, thing@" << insn.vB;
          break;
      }
      break;
    }
    case k3rc: os << StringPrintf("%s, {v%d .. v%d}, method@%d", opcode, insn.vC, (insn.vC + insn.vA - 1), insn.vB); break;
    case k51l: os << StringPrintf("%s v%d, #%+d", opcode, insn.vA, insn.vB); break;
    default: os << " unknown format (" << DumpHex(5) << ")"; break;
  }
  return os.str();
}

DecodedInstruction::DecodedInstruction(const Instruction* inst) {
  inst->Decode(vA, vB, vB_wide, vC, arg);
  opcode = inst->Opcode();
}

}  // namespace art
