// Copyright 2011 Google Inc. All Rights Reserved.

#include "libdex/InstrUtils.h"
#undef LOG
#undef LOG_FATAL

#include "src/dex_instruction.h"

namespace art {

size_t Instruction::Size() {
  const uint16_t* insns = reinterpret_cast<const uint16_t*>(this);
  return dexGetWidthFromInstruction(insns) * sizeof(uint16_t);
}

Instruction::Code Instruction::Opcode() {
  const uint16_t* insns = reinterpret_cast<const uint16_t*>(this);
  return (Instruction::Code)dexOpcodeFromCodeUnit(*insns);
}

const Instruction* Instruction::Next() {
  size_t current_size = Size();
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(this);
  return reinterpret_cast<const Instruction*>(ptr + current_size);
}

}  // namespace art
