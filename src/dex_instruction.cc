// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/dex_instruction.h"

namespace art {

const char* const Instruction::kInstructionNames[] = {
#define INSTRUCTION_NAME(o, c, pname, f, r, i, a) pname,
#include "src/dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_NAME)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_NAME
};

Instruction::InstructionFormat const Instruction::kInstructionFormats[] = {
#define INSTRUCTION_FORMAT(o, c, p, format, r, i, a) format,
#include "src/dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_FORMAT)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_FORMAT
};

int const Instruction::kInstructionFlags[] = {
#define INSTRUCTION_FLAGS(o, c, p, f, r, i, flags) flags,
#include "src/dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_FLAGS)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_FLAGS
};

size_t Instruction::Size() const {
  const uint16_t* insns = reinterpret_cast<const uint16_t*>(this);
  size_t size = 0;
  if (*insns == kPackedSwitchSignature) {
    size = 4 + insns[1] * 2;
  } else if (*insns == kSparseSwitchSignature) {
    size = 2 + insns[1] * 4;
  } else if (*insns == kArrayDataSignature) {
    uint16_t element_size = insns[1];
    uint32_t length = insns[2] | (((uint32_t)insns[3]) << 16);
    // The plus 1 is to round up for odd size and width.
    return 4 + (element_size * length + 1) / 2;
  } else {
    switch (Format()) {
      case k10x:
      case k12x:
      case k11n:
      case k11x:
      case k10t:
        size = 1;
        break;
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
        size = 2;
        break;
      case k32x:
      case k30t:
      case k31t:
      case k31i:
      case k31c:
      case k35c:
      case k3rc:
        size = 3;
        break;
      case k51l:
        size = 5;
        break;
      default:
        LOG(FATAL) << "Unreachable";
    }
  }
  size *= sizeof(uint16_t);
  return size;
}

Instruction::Code Instruction::Opcode() const {
  const uint16_t* insns = reinterpret_cast<const uint16_t*>(this);
  int opcode = *insns & 0xFF;
  return static_cast<Code>(opcode);
}

const Instruction* Instruction::Next() const {
  size_t current_size = Size();
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(this);
  return reinterpret_cast<const Instruction*>(ptr + current_size);
}

}  // namespace art
