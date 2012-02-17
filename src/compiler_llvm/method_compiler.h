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
  llvm::BasicBlock* basic_block_reg_arg_init_;
  std::vector<llvm::BasicBlock*> basic_blocks_;

  std::vector<llvm::BasicBlock*> basic_block_landing_pads_;
  llvm::BasicBlock* basic_block_unwind_;
  llvm::BasicBlock* basic_block_unreachable_;


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
  void EmitPrologueAssignArgRegister();
  void EmitInstructions();
  void EmitInstruction(uint32_t dex_pc, Instruction const* insn);

  enum CondBranchKind {
    kCondBranch_EQ,
    kCondBranch_NE,
    kCondBranch_LT,
    kCondBranch_GE,
    kCondBranch_GT,
    kCondBranch_LE,
  };

  enum IntArithmKind {
    kIntArithm_Add,
    kIntArithm_Sub,
    kIntArithm_Mul,
    kIntArithm_Div,
    kIntArithm_Rem,
    kIntArithm_And,
    kIntArithm_Or,
    kIntArithm_Xor,
    kIntArithm_Shl,
    kIntArithm_Shr,
    kIntArithm_UShr,
  };

  enum FPArithmKind {
    kFPArithm_Add,
    kFPArithm_Sub,
    kFPArithm_Mul,
    kFPArithm_Div,
    kFPArithm_Rem,
  };

#define GEN_INSN_ARGS uint32_t dex_pc, Instruction const* insn

  // NOP, PAYLOAD (unreachable) instructions
  void EmitInsn_Nop(GEN_INSN_ARGS);

  // MOVE, MOVE_RESULT instructions
  void EmitInsn_Move(GEN_INSN_ARGS, JType jty);
  void EmitInsn_MoveResult(GEN_INSN_ARGS, JType jty);

  // MOVE_EXCEPTION, THROW instructions
  void EmitInsn_MoveException(GEN_INSN_ARGS);
  void EmitInsn_ThrowException(GEN_INSN_ARGS);

  // RETURN instructions
  void EmitInsn_ReturnVoid(GEN_INSN_ARGS);
  void EmitInsn_Return(GEN_INSN_ARGS);

  // CONST, CONST_CLASS, CONST_STRING instructions
  void EmitInsn_LoadConstant(GEN_INSN_ARGS, JType imm_jty);
  void EmitInsn_LoadConstantString(GEN_INSN_ARGS);
  void EmitInsn_LoadConstantClass(GEN_INSN_ARGS);

  // MONITOR_ENTER, MONITOR_EXIT instructions
  void EmitInsn_MonitorEnter(GEN_INSN_ARGS);
  void EmitInsn_MonitorExit(GEN_INSN_ARGS);

  // CHECK_CAST, INSTANCE_OF instructions
  void EmitInsn_CheckCast(GEN_INSN_ARGS);
  void EmitInsn_InstanceOf(GEN_INSN_ARGS);

  // NEW_INSTANCE instructions
  void EmitInsn_NewInstance(GEN_INSN_ARGS);

  // ARRAY_LEN, NEW_ARRAY, FILLED_NEW_ARRAY, FILL_ARRAY_DATA instructions
  void EmitInsn_ArrayLength(GEN_INSN_ARGS);
  void EmitInsn_NewArray(GEN_INSN_ARGS);
  void EmitInsn_FilledNewArray(GEN_INSN_ARGS, bool is_range);
  void EmitInsn_FillArrayData(GEN_INSN_ARGS);

  // GOTO, IF_TEST, IF_TESTZ instructions
  void EmitInsn_UnconditionalBranch(GEN_INSN_ARGS);
  void EmitInsn_BinaryConditionalBranch(GEN_INSN_ARGS, CondBranchKind cond);
  void EmitInsn_UnaryConditionalBranch(GEN_INSN_ARGS, CondBranchKind cond);

  // PACKED_SWITCH, SPARSE_SWITCH instrutions
  void EmitInsn_PackedSwitch(GEN_INSN_ARGS);
  void EmitInsn_SparseSwitch(GEN_INSN_ARGS);

  // CMPX_FLOAT, CMPX_DOUBLE, CMP_LONG instructions
  void EmitInsn_FPCompare(GEN_INSN_ARGS, JType fp_jty, bool gt_bias);
  void EmitInsn_LongCompare(GEN_INSN_ARGS);

  // AGET, APUT instrutions
  void EmitInsn_AGet(GEN_INSN_ARGS, JType elem_jty);
  void EmitInsn_APut(GEN_INSN_ARGS, JType elem_jty);

  // IGET, IPUT instructions
  void EmitInsn_IGet(GEN_INSN_ARGS, JType field_jty);
  void EmitInsn_IPut(GEN_INSN_ARGS, JType field_jty);

  // SGET, SPUT instructions
  void EmitInsn_SGet(GEN_INSN_ARGS, JType field_jty);
  void EmitInsn_SPut(GEN_INSN_ARGS, JType field_jty);

  // INVOKE instructions
  void EmitInsn_InvokeVirtual(GEN_INSN_ARGS, bool is_range);
  void EmitInsn_InvokeSuper(GEN_INSN_ARGS, bool is_range);
  void EmitInsn_InvokeDirect(GEN_INSN_ARGS, bool is_range);
  void EmitInsn_InvokeStatic(GEN_INSN_ARGS, bool is_range);
  void EmitInsn_InvokeInterface(GEN_INSN_ARGS, bool is_range);

  // Unary instructions
  void EmitInsn_Neg(GEN_INSN_ARGS, JType op_jty);
  void EmitInsn_Not(GEN_INSN_ARGS, JType op_jty);
  void EmitInsn_SExt(GEN_INSN_ARGS);
  void EmitInsn_Trunc(GEN_INSN_ARGS);
  void EmitInsn_TruncAndSExt(GEN_INSN_ARGS, unsigned N);
  void EmitInsn_TruncAndZExt(GEN_INSN_ARGS, unsigned N);

  void EmitInsn_FNeg(GEN_INSN_ARGS, JType op_jty);
  void EmitInsn_IntToFP(GEN_INSN_ARGS, JType src_jty, JType dest_jty);
  void EmitInsn_FPToInt(GEN_INSN_ARGS, JType src_jty, JType dest_jty);
  void EmitInsn_FExt(GEN_INSN_ARGS);
  void EmitInsn_FTrunc(GEN_INSN_ARGS);

  // Integer binary arithmetic instructions
  void EmitInsn_IntArithm(GEN_INSN_ARGS, IntArithmKind arithm,
                          JType op_jty, bool is_2addr);

  void EmitInsn_IntArithmImmediate(GEN_INSN_ARGS, IntArithmKind arithm);

  void EmitInsn_RSubImmediate(GEN_INSN_ARGS);


  // Floating-point binary arithmetic instructions
  void EmitInsn_FPArithm(GEN_INSN_ARGS, FPArithmKind arithm,
                         JType op_jty, bool is_2addr);

#undef GEN_INSN_ARGS


  // Dex cache code generation helper function
  llvm::Value* EmitLoadDexCacheAddr(MemberOffset dex_cache_offset);

  void EmitLoadDexCacheCodeAndDirectMethodFieldAddr(
                                          llvm::Value*& code_addr_field_addr,
                                          llvm::Value*& method_field_addr,
                                          uint32_t method_idx);

  llvm::Value* EmitLoadDexCacheStaticStorageFieldAddr(uint32_t type_idx);

  llvm::Value* EmitLoadDexCacheResolvedTypeFieldAddr(uint32_t type_idx);

  llvm::Value* EmitLoadDexCacheStringFieldAddr(uint32_t string_idx);


  // Code generation helper function

  llvm::Value* EmitLoadMethodObjectAddr();

  llvm::FunctionType* GetFunctionType(uint32_t method_idx, bool is_static);

  void EmitGuard_ExceptionLandingPad(uint32_t dex_pc);

  void EmitBranchExceptionLandingPad(uint32_t dex_pc);

  void EmitGuard_GarbageCollectionSuspend(uint32_t dex_pc);

  llvm::Value* EmitCompareResultSelection(llvm::Value* cmp_eq,
                                          llvm::Value* cmp_lt);

  llvm::Value* EmitConditionResult(llvm::Value* lhs,
                                   llvm::Value* rhs,
                                   CondBranchKind cond);

  llvm::Value* EmitIntArithmResultComputation(uint32_t dex_pc,
                                              llvm::Value* lhs,
                                              llvm::Value* rhs,
                                              IntArithmKind arithm,
                                              JType op_jty);

  llvm::Value* EmitFPArithmResultComputation(uint32_t dex_pc,
                                             llvm::Value* lhs,
                                             llvm::Value* rhs,
                                             FPArithmKind arithm);

  llvm::Value* EmitLoadArrayLength(llvm::Value* array);

  llvm::Value* EmitArrayGEP(llvm::Value* array_addr,
                            llvm::Value* index_value,
                            llvm::Type* elem_type);

  void EmitGuard_DivZeroException(uint32_t dex_pc,
                                  llvm::Value* denominator,
                                  JType op_jty);

  void EmitGuard_NullPointerException(uint32_t dex_pc,
                                      llvm::Value* object);

  void EmitGuard_ArrayIndexOutOfBoundsException(uint32_t dex_pc,
                                                llvm::Value* array,
                                                llvm::Value* index);

  void EmitGuard_ArrayException(uint32_t dex_pc,
                                llvm::Value* array,
                                llvm::Value* index);

  RegCategory GetInferredRegCategory(uint32_t dex_pc, uint16_t reg);


  // Diagnostics helper function
  void PrintUnresolvedFieldWarning(int32_t field_idx);


  // Basic block helper functions
  llvm::BasicBlock* GetBasicBlock(uint32_t dex_pc);

  llvm::BasicBlock* GetNextBasicBlock(uint32_t dex_pc);

  llvm::BasicBlock* CreateBasicBlockWithDexPC(uint32_t dex_pc,
                                              char const* postfix = NULL);

  int32_t GetTryItemOffset(uint32_t dex_pc);

  llvm::BasicBlock* GetLandingPadBasicBlock(uint32_t dex_pc);

  llvm::BasicBlock* GetUnwindBasicBlock();


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
