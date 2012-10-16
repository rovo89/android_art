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
#include "dex_file.h"
#include "dex_instruction.h"
#include "greenland/backend_types.h"
#include "invoke_type.h"
#include "object_utils.h"
#include "runtime_support_func.h"

#include <llvm/Support/IRBuilder.h>

#include <vector>

#include <stdint.h>


namespace art {
  class ClassLinker;
  class ClassLoader;
  class CompiledMethod;
  class Compiler;
  class DexCache;
  class Field;
  class OatCompilationUnit;

  namespace greenland {
    class InferredRegCategoryMap;
  }
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

class CompilationUnit;
class CompilerLLVM;
class DalvikReg;
class IRBuilder;

class MethodCompiler {
 public:
  MethodCompiler(CompilationUnit* cunit,
                 Compiler* compiler,
                 OatCompilationUnit* oat_compilation_unit);

  ~MethodCompiler();

  CompiledMethod* Compile();


  // Code generation helper function

  IRBuilder& GetIRBuilder() const {
    return irb_;
  }


  // Register helper function

  llvm::Value* AllocDalvikReg(RegCategory cat, const std::string& name);

  llvm::Value* GetShadowFrameEntry(uint32_t reg_idx);


 private:
  void CreateFunction();
  void EmitPrologue();
  void EmitStackOverflowCheck();
  void EmitPrologueLastBranch();
  void EmitPrologueAllocShadowFrame();
  void EmitPrologueAssignArgRegister();
  void EmitInstructions();
  void EmitInstruction(uint32_t dex_pc, const Instruction* insn);

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
  };

  enum IntShiftArithmKind {
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

  enum InvokeArgFmt {
    kArgReg,
    kArgRange,
  };

#define GEN_INSN_ARGS uint32_t dex_pc, const Instruction* insn

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
  void EmitInsn_Invoke(GEN_INSN_ARGS,
                       InvokeType invoke_type,
                       InvokeArgFmt arg_fmt);

  llvm::Value* EmitLoadSDCalleeMethodObjectAddr(uint32_t callee_method_idx);

  llvm::Value* EmitLoadVirtualCalleeMethodObjectAddr(int vtable_idx,
                                                     llvm::Value* this_addr);

  llvm::Value* EmitCallRuntimeForCalleeMethodObjectAddr(uint32_t callee_method_idx,
                                                        InvokeType invoke_type,
                                                        llvm::Value* this_addr,
                                                        uint32_t dex_pc,
                                                        bool is_fast_path);

  // Unary instructions
  void EmitInsn_Neg(GEN_INSN_ARGS, JType op_jty);
  void EmitInsn_Not(GEN_INSN_ARGS, JType op_jty);
  void EmitInsn_SExt(GEN_INSN_ARGS);
  void EmitInsn_Trunc(GEN_INSN_ARGS);
  void EmitInsn_TruncAndSExt(GEN_INSN_ARGS, unsigned N);
  void EmitInsn_TruncAndZExt(GEN_INSN_ARGS, unsigned N);

  void EmitInsn_FNeg(GEN_INSN_ARGS, JType op_jty);
  void EmitInsn_IntToFP(GEN_INSN_ARGS, JType src_jty, JType dest_jty);
  void EmitInsn_FPToInt(GEN_INSN_ARGS, JType src_jty, JType dest_jty,
                        runtime_support::RuntimeId runtime_func_id);
  void EmitInsn_FExt(GEN_INSN_ARGS);
  void EmitInsn_FTrunc(GEN_INSN_ARGS);

  // Integer binary arithmetic instructions
  void EmitInsn_IntArithm(GEN_INSN_ARGS, IntArithmKind arithm,
                          JType op_jty, bool is_2addr);

  void EmitInsn_IntArithmImmediate(GEN_INSN_ARGS, IntArithmKind arithm);

  void EmitInsn_IntShiftArithm(GEN_INSN_ARGS, IntShiftArithmKind arithm,
                               JType op_jty, bool is_2addr);

  void EmitInsn_IntShiftArithmImmediate(GEN_INSN_ARGS,
                                        IntShiftArithmKind arithm);

  void EmitInsn_RSubImmediate(GEN_INSN_ARGS);


  // Floating-point binary arithmetic instructions
  void EmitInsn_FPArithm(GEN_INSN_ARGS, FPArithmKind arithm,
                         JType op_jty, bool is_2addr);

#undef GEN_INSN_ARGS

  // GC card table helper function
  void EmitMarkGCCard(llvm::Value* value, llvm::Value* target_addr);

  // Shadow frame helper function
  void EmitPushShadowFrame(bool is_inline);
  void EmitPopShadowFrame();
  void EmitUpdateDexPC(uint32_t dex_pc);


  // Dex cache code generation helper function
  llvm::Value* EmitLoadDexCacheAddr(MemberOffset dex_cache_offset);

  llvm::Value* EmitLoadDexCacheStaticStorageFieldAddr(uint32_t type_idx);

  llvm::Value* EmitLoadDexCacheResolvedTypeFieldAddr(uint32_t type_idx);

  llvm::Value* EmitLoadDexCacheResolvedMethodFieldAddr(uint32_t method_idx);

  llvm::Value* EmitLoadDexCacheStringFieldAddr(uint32_t string_idx);


  // Code generation helper function

  llvm::Value* EmitLoadMethodObjectAddr();

  llvm::FunctionType* GetFunctionType(uint32_t method_idx, bool is_static);

  void EmitGuard_ExceptionLandingPad(uint32_t dex_pc, bool can_skip_unwind);

  void EmitBranchExceptionLandingPad(uint32_t dex_pc);

  void EmitGuard_GarbageCollectionSuspend();

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

  llvm::Value* EmitIntDivRemResultComputation(uint32_t dex_pc,
                                              llvm::Value* dividend,
                                              llvm::Value* divisor,
                                              IntArithmKind arithm,
                                              JType op_jty);

  llvm::Value* EmitIntShiftArithmResultComputation(uint32_t dex_pc,
                                                   llvm::Value* lhs,
                                                   llvm::Value* rhs,
                                                   IntShiftArithmKind arithm,
                                                   JType op_jty);

  llvm::Value* EmitFPArithmResultComputation(uint32_t dex_pc,
                                             llvm::Value* lhs,
                                             llvm::Value* rhs,
                                             FPArithmKind arithm);

  llvm::Value* EmitAllocNewArray(uint32_t dex_pc,
                                 int32_t length,
                                 uint32_t type_idx,
                                 bool is_filled_new_array);

  llvm::Value* EmitLoadClassObjectAddr(llvm::Value* this_addr);

  llvm::Value* EmitLoadVTableAddr(llvm::Value* class_object_addr);

  llvm::Value* EmitLoadMethodObjectAddrFromVTable(llvm::Value* vtable_addr,
                                                  uint16_t vtable_index);

  llvm::Value* EmitLoadCodeAddr(llvm::Value* method_object_addr,
                                uint32_t method_idx,
                                bool is_static);

  llvm::Value* EmitLoadArrayLength(llvm::Value* array);

  llvm::Value* EmitArrayGEP(llvm::Value* array_addr,
                            llvm::Value* index_value,
                            JType elem_jty);

  llvm::Value* EmitLoadConstantClass(uint32_t dex_pc, uint32_t type_idx);

  llvm::Value* EmitLoadStaticStorage(uint32_t dex_pc, uint32_t type_idx);

  void EmitLoadActualParameters(std::vector<llvm::Value*>& args,
                                uint32_t callee_method_idx,
                                DecodedInstruction const& di,
                                InvokeArgFmt arg_fmt,
                                bool is_static);

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

  greenland::RegCategory GetInferredRegCategory(uint32_t dex_pc, uint16_t reg);

  const greenland::InferredRegCategoryMap* GetInferredRegCategoryMap();

  bool IsRegCanBeObject(uint16_t reg_idx);


  // Basic block helper functions
  llvm::BasicBlock* GetBasicBlock(uint32_t dex_pc);

  llvm::BasicBlock* GetNextBasicBlock(uint32_t dex_pc);

  llvm::BasicBlock* CreateBasicBlockWithDexPC(uint32_t dex_pc,
                                              const char* postfix = NULL);

  int32_t GetTryItemOffset(uint32_t dex_pc);

  llvm::BasicBlock* GetLandingPadBasicBlock(uint32_t dex_pc);

  llvm::BasicBlock* GetUnwindBasicBlock();


  // Register helper function

  llvm::Value* EmitLoadDalvikReg(uint32_t reg_idx, JType jty, JTypeSpace space);

  llvm::Value* EmitLoadDalvikReg(uint32_t reg_idx, char shorty, JTypeSpace space);

  void EmitStoreDalvikReg(uint32_t reg_idx, JType jty,
                          JTypeSpace space, llvm::Value* new_value);

  void EmitStoreDalvikReg(uint32_t reg_idx, char shorty,
                          JTypeSpace space, llvm::Value* new_value);

  llvm::Value* EmitLoadDalvikRetValReg(JType jty, JTypeSpace space);

  llvm::Value* EmitLoadDalvikRetValReg(char shorty, JTypeSpace space);

  void EmitStoreDalvikRetValReg(JType jty, JTypeSpace space, llvm::Value* new_value);

  void EmitStoreDalvikRetValReg(char shorty, JTypeSpace space, llvm::Value* new_value);

  // TODO: Use high-level IR to do this
  bool EmitInlineJavaIntrinsic(const std::string& callee_method_name,
                               const std::vector<llvm::Value*>& args,
                               llvm::BasicBlock* after_invoke);

  bool EmitInlinedStringCharAt(const std::vector<llvm::Value*>& args,
                               llvm::BasicBlock* after_invoke);

  bool EmitInlinedStringLength(const std::vector<llvm::Value*>& args,
                               llvm::BasicBlock* after_invoke);

  bool EmitInlinedStringIndexOf(const std::vector<llvm::Value*>& args,
                                llvm::BasicBlock* after_invoke,
                                bool zero_based);

  bool EmitInlinedStringCompareTo(const std::vector<llvm::Value*>& args,
                                  llvm::BasicBlock* after_invoke);

  bool IsInstructionDirectToReturn(uint32_t dex_pc);

  struct MethodInfo {
    int64_t this_reg_idx;
    bool this_will_not_be_null;
    bool has_invoke;
    bool need_shadow_frame_entry;
    bool need_shadow_frame;
    bool lazy_push_shadow_frame;
    std::vector<bool> set_to_another_object;
  };
  MethodInfo method_info_;

  void ComputeMethodInfo();

 private:
  CompilationUnit* cunit_;
  Compiler* compiler_;

  const DexFile* dex_file_;
  const DexFile::CodeItem* code_item_;

  OatCompilationUnit* oat_compilation_unit_;

  uint32_t method_idx_;
  uint32_t access_flags_;

  llvm::Module* module_;
  llvm::LLVMContext* context_;
  IRBuilder& irb_;
  llvm::Function* func_;

  std::vector<DalvikReg*> regs_;
  std::vector<llvm::Value*> shadow_frame_entries_;
  std::vector<int32_t> reg_to_shadow_frame_index_;
  UniquePtr<DalvikReg> retval_reg_;

  llvm::BasicBlock* basic_block_alloca_;
  llvm::BasicBlock* basic_block_shadow_frame_;
  llvm::BasicBlock* basic_block_reg_arg_init_;
  std::vector<llvm::BasicBlock*> basic_blocks_;

  std::vector<llvm::BasicBlock*> basic_block_landing_pads_;
  llvm::BasicBlock* basic_block_unwind_;

  llvm::AllocaInst* shadow_frame_;
  llvm::Value* old_shadow_frame_;

  llvm::Value* already_pushed_shadow_frame_;
  uint16_t num_shadow_frame_refs_;

  uint16_t elf_func_idx_;
};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_METHOD_COMPILER_H_
