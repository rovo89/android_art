// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_VERIFY_H_
#define ART_SRC_DEX_VERIFY_H_

#include "macros.h"
#include "object.h"
#include "dex_instruction.h"

namespace art {

class DexVerify {
 public:
  enum {
    kInsnFlagWidthMask = 0x0000ffff,
    kInsnFlagInTry = (1 << 16),
    kInsnFlagBranchTarget = (1 << 17),
    kInsnFlagGcPoint = (1 << 18),
    kInsnFlagVisited = (1 << 30),
    kInsnFlagChanged = (1 << 31),
  };

  static bool VerifyClass(Class* klass);

 private:
  static bool VerifyMethod(Method* method);
  static bool VerifyInstructions(const DexFile* dex_file,
                                 const DexFile::CodeItem* code_item,
                                 uint32_t insn_flags[]);
  static bool VerifyInstruction(const DexFile* dex_file,
                                const Instruction* inst,
                                uint32_t code_offset,
                                const DexFile::CodeItem* code_item,
                                uint32_t insn_flags[]);

  DISALLOW_COPY_AND_ASSIGN(DexVerify);
};

}  // namespace art

#endif  // ART_SRC_DEX_VERIFY_H_
