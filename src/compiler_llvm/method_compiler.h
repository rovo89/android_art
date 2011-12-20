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

#ifndef ART_SRC_COMPILER_LLVM_METHOD_COMPILER_H_
#define ART_SRC_COMPILER_LLVM_METHOD_COMPILER_H_

#include "backend_types.h"
#include "constants.h"
#include "dalvik_reg.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "object_utils.h"

#include <llvm/Support/IRBuilder.h>

#include <vector>

#include <stdint.h>


namespace art {
  class ClassLinker;
  class ClassLoader;
  class CompiledMethod;
  class Compiler;
  class DexCache;
}


namespace llvm {
  class AllocaInst;
  class BasicBlock;
  class Function;
  class FunctionType;
  class LLVMContext;
  class Module;
  class Type;
}


namespace art {
namespace compiler_llvm {

class CompilerLLVM;
class IRBuilder;

class MethodCompiler {
 private:
  InstructionSet insn_set_;
  Compiler* compiler_;
  compiler_llvm::CompilerLLVM* compiler_llvm_;

  ClassLinker* class_linker_;
  ClassLoader const* class_loader_;

  DexFile const* dex_file_;
  DexCache* dex_cache_;
  DexFile::CodeItem const* code_item_;

  Method* method_;
  MethodHelper method_helper_;

  uint32_t method_idx_;
  uint32_t access_flags_;

  llvm::Module* module_;
  llvm::LLVMContext* context_;
  IRBuilder& irb_;
  llvm::Function* func_;

  std::vector<DalvikReg*> regs_;
  UniquePtr<DalvikReg> retval_reg_;

  llvm::BasicBlock* basic_block_reg_alloca_;
  llvm::BasicBlock* basic_block_reg_zero_init_;
  std::vector<llvm::BasicBlock*> basic_blocks_;


 public:
  MethodCompiler(InstructionSet insn_set,
                 Compiler* compiler,
                 ClassLinker* class_linker,
                 ClassLoader const* class_loader,
                 DexFile const* dex_file,
                 DexCache* dex_cache,
                 DexFile::CodeItem const* code_item,
                 uint32_t method_idx,
                 uint32_t access_flags);

  ~MethodCompiler();

  CompiledMethod* Compile();


  // Code generation helper function

  IRBuilder& GetIRBuilder() const {
    return irb_;
  }


  // Register helper function

  llvm::Value* AllocDalvikLocalVarReg(RegCategory cat, uint32_t reg_idx);

  llvm::Value* AllocDalvikRetValReg(RegCategory cat);


 private:
  void CreateFunction();


  void EmitPrologue();
  void EmitPrologueLastBranch();
  void EmitInstructions();
  void EmitInstruction(uint32_t dex_pc, Instruction const* insn);


  // Code generation helper function

  llvm::Value* EmitLoadMethodObjectAddr();

  llvm::FunctionType* GetFunctionType(uint32_t method_idx, bool is_static);


  // Basic block helper functions
  llvm::BasicBlock* GetBasicBlock(uint32_t dex_pc);

  llvm::BasicBlock* GetNextBasicBlock(uint32_t dex_pc);

  llvm::BasicBlock* CreateBasicBlockWithDexPC(uint32_t dex_pc,
                                              char const* postfix = NULL);


  // Register helper function

  llvm::Value* EmitLoadDalvikReg(uint32_t reg_idx, JType jty,
                                 JTypeSpace space) {
    return regs_[reg_idx]->GetValue(jty, space);
  }

  llvm::Value* EmitLoadDalvikReg(uint32_t reg_idx, char shorty,
                                 JTypeSpace space) {
    return EmitLoadDalvikReg(reg_idx, GetJTypeFromShorty(shorty), space);
  }

  void EmitStoreDalvikReg(uint32_t reg_idx, JType jty,
                          JTypeSpace space, llvm::Value* new_value) {
    regs_[reg_idx]->SetValue(jty, space, new_value);
  }

  void EmitStoreDalvikReg(uint32_t reg_idx, char shorty,
                          JTypeSpace space, llvm::Value* new_value) {
    EmitStoreDalvikReg(reg_idx, GetJTypeFromShorty(shorty), space, new_value);
  }

  llvm::Value* EmitLoadDalvikRetValReg(JType jty, JTypeSpace space) {
    return retval_reg_->GetValue(jty, space);
  }

  llvm::Value* EmitLoadDalvikRetValReg(char shorty, JTypeSpace space) {
    return EmitLoadDalvikRetValReg(GetJTypeFromShorty(shorty), space);
  }

  void EmitStoreDalvikRetValReg(JType jty, JTypeSpace space,
                                llvm::Value* new_value) {
    retval_reg_->SetValue(jty, space, new_value);
  }

  void EmitStoreDalvikRetValReg(char shorty, JTypeSpace space,
                                llvm::Value* new_value) {
    EmitStoreDalvikRetValReg(GetJTypeFromShorty(shorty), space, new_value);
  }
};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_METHOD_COMPILER_H_
