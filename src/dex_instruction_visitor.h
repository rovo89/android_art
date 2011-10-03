// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_INSTRUCTION_VISITOR_H_
#define ART_SRC_DEX_INSTRUCTION_VISITOR_H_

#include "dex_instruction.h"
#include "macros.h"

namespace art {

template<typename T>
class DexInstructionVisitor {
 public:
  void Visit(const uint16_t* code, size_t size_in_bytes) {
    T* derived = static_cast<T*>(this);
    size_t size_in_code_units = size_in_bytes / sizeof(uint16_t);
    size_t i = 0;
    while (i < size_in_code_units) {
      const Instruction* inst = Instruction::At(&code[i]);
      switch (inst->Opcode()) {
#define INSTRUCTION_CASE(o, cname, p, f, r, i, a, v)  \
        case Instruction::cname: {                    \
          derived->Do_ ## cname(inst);                \
          break;                                      \
        }
#include "dex_instruction_list.h"
        DEX_INSTRUCTION_LIST(INSTRUCTION_CASE)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_CASE
        default:
          CHECK(false);
      }
      i += inst->SizeInCodeUnits();
    }
  }

 private:
  // Specific handlers for each instruction.
#define INSTRUCTION_VISITOR(o, cname, p, f, r, i, a, v)    \
  void Do_ ## cname(const Instruction* inst) {             \
    T* derived = static_cast<T*>(this);                    \
    derived->Do_Default(inst);                             \
  }
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_VISITOR)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_VISITOR

  // The default instruction handler.
  void Do_Default(const Instruction* inst) {
    return;
  }
};


}  // namespace art

#endif  // ART_SRC_DEX_INSTRUCTION_VISITOR_H_
