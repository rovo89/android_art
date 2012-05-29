/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "target_lir_emitter.h"

#include "target_lir_info.h"
#include "target_lir_opcodes.h"

#include "intrinsic_helper.h"
#include "lir_function.h"

#include <llvm/Function.h>

namespace art {
namespace greenland {

TargetLIREmitter::TargetLIREmitter(const llvm::Function& func,
                                   const OatCompilationUnit& cunit,
                                   DexLang::Context& dex_lang_ctx,
                                   TargetLIRInfo& target_lir_info)
    : func_(func), cunit_(cunit), dex_lang_ctx_(dex_lang_ctx.IncRef()),
      info_(target_lir_info), lir_func_() {
  return;
}

TargetLIREmitter::~TargetLIREmitter() {
  dex_lang_ctx_.DecRef();
  return;
}

bool TargetLIREmitter::visitBasicBlock(const llvm::BasicBlock& bb) {
  // Place the corresponding block label to the output
  llvm::DenseMap<const llvm::BasicBlock*, LIR*>::const_iterator label_iter =
      block_labels_.find(&bb);

  DCHECK(label_iter != block_labels_.end());
  lir_func_.push_back(label_iter->second);

  // Now, iterate over and process all instructions within the basic block
  for (llvm::BasicBlock::const_iterator inst_iter = bb.begin(),
          inst_end = bb.end(); inst_iter != inst_end; inst_iter++) {
    const llvm::Instruction& inst = *inst_iter;
    switch (inst.getOpcode()) {
#define VISIT(OPCODE, CLASS)                                          \
      case llvm::Instruction::OPCODE: {                               \
        if (!visit ## CLASS(static_cast<const llvm::CLASS&>(inst))) { \
          return false;                                               \
        }                                                             \
        break;                                                        \
      }

      VISIT(Ret,      ReturnInst);
      VISIT(Br,       BranchInst);
      VISIT(ICmp,     ICmpInst);
      VISIT(IntToPtr, IntToPtrInst);
      VISIT(Call,     CallInst);

#undef VISIT
      default : {
        LOG(INFO) << "Unhandled instruction hit!";
        inst.dump();
        return false;
      }
    }
  }

  return true;
}

bool TargetLIREmitter::visitReturnInst(const llvm::ReturnInst& inst) {
  inst.dump();
  return true;
}

bool TargetLIREmitter::visitBranchInst(const llvm::BranchInst& inst) {
  inst.dump();
  return true;
}

bool TargetLIREmitter::visitICmpInst(const llvm::ICmpInst& inst) {
  inst.dump();
  return true;
}

bool TargetLIREmitter::visitIntToPtrInst(const llvm::IntToPtrInst& inst) {
  inst.dump();
  return true;
}

bool TargetLIREmitter::visitCallInst(const llvm::CallInst& inst) {
  // The callee must be a DexLang intrinsic
  return visitDexLangIntrinsics(inst);
}

bool TargetLIREmitter::visitDexLangIntrinsics(const llvm::CallInst& inst) {
  const llvm::Function* callee = inst.getCalledFunction();
  IntrinsicHelper::IntrinsicId intr_id =
      dex_lang_ctx_.GetIntrinsicHelper().GetIntrinsicId(callee);

  if (intr_id == IntrinsicHelper::UnknownId) {
    LOG(INFO) << "Unexpected call instruction to '"
              << callee->getName().str() << "'";
    return false;
  }

  //const IntrinsicHelper::IntrinsicInfo& intr_info =
  //    IntrinsicHelper::GetInfo(intr_id);


  return true;
}

LIRFunction* TargetLIREmitter::Emit() {
  if (EmitBasicBlockLabels() &&
      EmitEntrySequence() &&
      EmitInstructions() &&
      EmitExitSequence()) {
    return &lir_func_;
  }

  return NULL;
}

bool TargetLIREmitter::EmitBasicBlockLabels() {
  for (llvm::Function::const_iterator bb_iter = func_.begin(),
          bb_end = func_.end(); bb_iter != bb_end; bb_iter++) {
    LIR* lir = lir_func_.CreateLIR(info_.GetLIRDesc(opcode::kBlockLabel));
    CHECK(block_labels_.insert(std::make_pair(bb_iter, lir)).second);
  }
  return true;
}

bool TargetLIREmitter::EmitEntrySequence() {
  // Flush all function arguments to the virtual registers
  return true;
}

bool TargetLIREmitter::EmitInstructions() {
  // Iterator over all basic blocks
  for (llvm::Function::const_iterator bb_iter = func_.begin(),
          bb_end = func_.end(); bb_iter != bb_end; bb_iter++) {
    if (!visitBasicBlock(*bb_iter)) {
      return false;
    }
  }
  return true;
}

bool TargetLIREmitter::EmitExitSequence() {
  return true;
}

} // namespace greenland
} // namespace art
