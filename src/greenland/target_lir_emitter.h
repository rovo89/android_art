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

#ifndef ART_SRC_GREENLAND_TARGET_LIR_EMITTER_H_
#define ART_SRC_GREENLAND_TARGET_LIR_EMITTER_H_

#include "dex_lang.h"

#include "lir_function.h"

#include <llvm/ADT/DenseMap.h>

namespace art {
  class OatCompilationUnit;
}

namespace llvm {
  class BasicBlock;
  class BranchInst;
  class CallInst;
  class Function;
  class ICmpInst;
  class Instruction;
  class IntToPtrInst;
  class ReturnInst;
}

namespace art {
namespace greenland {

class LIRFunction;
class TargetLIRInfo;

class TargetLIREmitter {
 private:
  const llvm::Function& func_;
  const OatCompilationUnit& cunit_;
  DexLang::Context& dex_lang_ctx_;
  TargetLIRInfo& info_;

 private:
  llvm::DenseMap<const llvm::BasicBlock*, LIR*> block_labels_;

 protected:
  LIRFunction lir_func_;

  TargetLIREmitter(const llvm::Function& func,
                   const OatCompilationUnit& cunit,
                   DexLang::Context& dex_lang_ctx,
                   TargetLIRInfo& target_lir_info);

 private:
  bool visitBasicBlock(const llvm::BasicBlock& bb);

  bool visitReturnInst(const llvm::ReturnInst& inst);
  bool visitBranchInst(const llvm::BranchInst& inst);
  //bool visitSwitchInst(SwitchInst &I)            { DELEGATE(TerminatorInst);}
  //bool visitIndirectBrInst(IndirectBrInst &I)    { DELEGATE(TerminatorInst);}
  //bool visitInvokeInst(InvokeInst &I)            { DELEGATE(TerminatorInst);}
  //bool visitResumeInst(ResumeInst &I)            { DELEGATE(TerminatorInst);}
  //bool visitUnreachableInst(UnreachableInst &I)  { DELEGATE(TerminatorInst);}
  bool visitICmpInst(const llvm::ICmpInst& inst);
  //bool visitFCmpInst(FCmpInst &I)                { DELEGATE(CmpInst);}
  //bool visitAllocaInst(AllocaInst &I)            { DELEGATE(UnaryInstruction);}
  //bool visitLoadInst(LoadInst     &I)            { DELEGATE(UnaryInstruction);}
  //bool visitStoreInst(StoreInst   &I)            { DELEGATE(Instruction);}
  //bool visitAtomicCmpXchgInst(AtomicCmpXchgInst &I) { DELEGATE(Instruction);}
  //bool visitAtomicRMWInst(AtomicRMWInst &I)      { DELEGATE(Instruction);}
  //bool visitFenceInst(FenceInst   &I)            { DELEGATE(Instruction);}
  //bool visitGetElementPtrInst(GetElementPtrInst &I){ DELEGATE(Instruction);}
  //bool visitPHINode(PHINode       &I)            { DELEGATE(Instruction);}
  //bool visitTruncInst(TruncInst &I)              { DELEGATE(CastInst);}
  //bool visitZExtInst(ZExtInst &I)                { DELEGATE(CastInst);}
  //bool visitSExtInst(SExtInst &I)                { DELEGATE(CastInst);}
  //bool visitFPTruncInst(FPTruncInst &I)          { DELEGATE(CastInst);}
  //bool visitFPExtInst(FPExtInst &I)              { DELEGATE(CastInst);}
  //bool visitFPToUIInst(FPToUIInst &I)            { DELEGATE(CastInst);}
  //bool visitFPToSIInst(FPToSIInst &I)            { DELEGATE(CastInst);}
  //bool visitUIToFPInst(UIToFPInst &I)            { DELEGATE(CastInst);}
  //bool visitSIToFPInst(SIToFPInst &I)            { DELEGATE(CastInst);}
  //bool visitPtrToIntInst(PtrToIntInst &I)        { DELEGATE(CastInst);}
  bool visitIntToPtrInst(const llvm::IntToPtrInst& inst);
  //bool visitBitCastInst(BitCastInst &I)          { DELEGATE(CastInst);}
  //bool visitSelectInst(SelectInst &I)            { DELEGATE(Instruction);}
  bool visitCallInst(const llvm::CallInst& inst);
  //bool visitVAArgInst(VAArgInst   &I)            { DELEGATE(UnaryInstruction);}
  //bool visitExtractElementInst(ExtractElementInst &I) { DELEGATE(Instruction);}
  //bool visitInsertElementInst(InsertElementInst &I) { DELEGATE(Instruction);}
  //bool visitShuffleVectorInst(ShuffleVectorInst &I) { DELEGATE(Instruction);}
  //bool visitExtractValueInst(ExtractValueInst &I){ DELEGATE(UnaryInstruction);}
  //bool visitInsertValueInst(InsertValueInst &I)  { DELEGATE(Instruction); }
  //bool visitLandingPadInst(LandingPadInst &I)    { DELEGATE(Instruction); }

  bool visitDexLangIntrinsics(const llvm::CallInst& inst);

 public:
  virtual ~TargetLIREmitter();

  LIRFunction* Emit();

 private:
  bool EmitBasicBlockLabels();
  bool EmitEntrySequence();
  bool EmitInstructions();
  bool EmitExitSequence();
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_TARGET_LIR_EMITTER_H_
