// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_INSTRUCTION_H_
#define ART_SRC_DEX_INSTRUCTION_H_

#include "src/globals.h"
#include "src/logging.h"
#include "src/macros.h"

namespace art {

class Instruction {
 public:
  enum Code {
#define INSTRUCTION_ENUM(cname, opcode) cname = opcode,
#include "src/dex_instruction_list.h"
    DEX_INSTRUCTION_LIST(INSTRUCTION_ENUM)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_ENUM
  };

  // Returns the size in bytes of this instruction.
  size_t Size();

  // Returns a pointer to the next instruction in the stream.
  const Instruction* Next();

  // Returns the opcode field of the instruction.
  Code Opcode();

  // Reads an instruction out of the stream at the specified address.
  static Instruction* At(byte* code) {
    CHECK(code != NULL);
    return reinterpret_cast<Instruction*>(code);
  }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Instruction);
};

}  // namespace art

#endif  // ART_SRC_DEX_INSTRUCTION_H_
