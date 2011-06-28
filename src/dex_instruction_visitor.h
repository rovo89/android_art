// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_INSTRUCTION_VISITOR_H_
#define ART_SRC_DEX_INSTRUCTION_VISITOR_H_

#include "src/dex_instruction.h"
#include "src/macros.h"

namespace art {

template<typename T>
class DexInstructionVisitor {
 public:
  void Visit(uint16_t* code, size_t size) {
    T* derived = static_cast<T*>(this);
    byte* ptr = reinterpret_cast<byte*>(code);
    byte* end = ptr + size;
    while (ptr != end) {
      Instruction* inst = Instruction::At(ptr);
      switch (inst->Opcode()) {
#define INSTRUCTION_CASE(cname, value)             \
        case Instruction::cname: {                 \
          derived->Do_ ## cname(inst);             \
          break;                                   \
        }
        DEX_INSTRUCTION_LIST(INSTRUCTION_CASE)
#undef INSTRUCTION_CASE
        default:
          CHECK(true);
      }
      ptr += inst->Size();
      CHECK_LE(ptr, end);
    }
  }

 private:
  // Specific handlers for each instruction.
#define INSTRUCTION_VISITOR(cname, value) \
  void Do_ ## cname(Instruction* inst) {  \
    T* derived = static_cast<T*>(this);   \
    derived->Do_Default(inst);            \
  };
  DEX_INSTRUCTION_LIST(INSTRUCTION_VISITOR);
#undef INSTRUCTION_VISITOR

  // The default instruction handler.
  void Do_Default(Instruction* inst) {
    return;
  }
};

}  // namespace art

#endif  // ART_SRC_DEX_INSTRUCTION_VISITOR_H_
