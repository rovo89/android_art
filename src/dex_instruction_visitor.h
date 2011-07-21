// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_INSTRUCTION_VISITOR_H_
#define ART_SRC_DEX_INSTRUCTION_VISITOR_H_

#include "dex_instruction.h"
#include "macros.h"

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
#define INSTRUCTION_CASE(o, cname, p, f, r, i, a)  \
        case Instruction::cname: {                 \
          derived->Do_ ## cname(inst);             \
          break;                                   \
        }
#include "dex_instruction_list.h"
        DEX_INSTRUCTION_LIST(INSTRUCTION_CASE)
#undef DEX_INSTRUCTION_LIST
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
#define INSTRUCTION_VISITOR(o, cname, p, f, r, i, a)    \
  void Do_ ## cname(Instruction* inst) {                \
    T* derived = static_cast<T*>(this);                 \
    derived->Do_Default(inst);                          \
  };
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_VISITOR)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_VISITOR

  // The default instruction handler.
  void Do_Default(Instruction* inst) {
    return;
  }
};

}  // namespace art

#endif  // ART_SRC_DEX_INSTRUCTION_VISITOR_H_
