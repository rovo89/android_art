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

#ifndef ART_SRC_GREENLAND_DEX_LANG_H_
#define ART_SRC_GREENLAND_DEX_LANG_H_

#include "backend_types.h"
#include "dalvik_reg.h"
#include "ir_builder.h"

#include "dex_file.h"
#include "dex_instruction.h"
#include "invoke_type.h"
#include "macros.h"

#include <vector>

#include <llvm/Module.h>
#include <llvm/ADT/ArrayRef.h>

namespace llvm {
  class BasicBlock;
  class Function;
  class LLVMContext;
  class Type;
}

namespace art {
  class Compiler;
  class OatCompilationUnit;
}

namespace art {
namespace greenland {

class DalvikReg;
class InferredRegCategoryMap;
class IntrinsicHelper;

class DexLang {
 public:
  class Context {
   private:
    llvm::Module& module_;
    IntrinsicHelper* intrinsic_helper_;

   public:
    Context(llvm::Module& module);
    ~Context();

    inline llvm::LLVMContext& GetLLVMContext()
    { return module_.getContext(); }

    inline llvm::Module& GetOutputModule()
    { return module_; }

    inline IntrinsicHelper& GetIntrinsicHelper()
    { return *intrinsic_helper_; }
    inline const IntrinsicHelper& GetIntrinsicHelper() const
    { return *intrinsic_helper_; }

   private:
    DISALLOW_COPY_AND_ASSIGN(Context);
  };

 public:
  DexLang(Context& context, Compiler& compiler, OatCompilationUnit& cunit);

  ~DexLang();

  llvm::Function* Build();

  inline IRBuilder& GetIRBuilder() {
    return irb_;
  }
  llvm::Value* AllocateDalvikReg(RegCategory cat, unsigned reg_idx);

 private:
  Context& dex_lang_ctx_;
  Compiler& compiler_;
  OatCompilationUnit& cunit_;

  const DexFile* dex_file_;
  const DexFile::CodeItem* code_item_;
  uint32_t method_idx_;

  llvm::LLVMContext& context_;
  llvm::Module& module_;
  IntrinsicHelper& intrinsic_helper_;

  IRBuilder irb_;
  llvm::Function* func_;

 private:
  //----------------------------------------------------------------------------
  // Basic Block Helper Functions
  //----------------------------------------------------------------------------
  llvm::BasicBlock* reg_alloc_bb_;
  llvm::BasicBlock* arg_reg_init_bb_;

  std::vector<llvm::BasicBlock*> basic_blocks_;

  llvm::BasicBlock* GetBasicBlock(unsigned dex_pc);
  llvm::BasicBlock* CreateBasicBlockWithDexPC(unsigned dex_pc,
                                              char const* postfix = NULL);
  llvm::BasicBlock* GetNextBasicBlock(unsigned dex_pc);

 private:
  //----------------------------------------------------------------------------
  // Register Helper Functions
  //----------------------------------------------------------------------------
  std::vector<DalvikReg*> regs_;

  llvm::Value* EmitLoadDalvikReg(unsigned reg_idx, JType jty, JTypeSpace space);

  llvm::Value* EmitLoadDalvikReg(unsigned reg_idx, char shorty, JTypeSpace space);

  void EmitStoreDalvikReg(unsigned reg_idx, JType jty,
                          JTypeSpace space, llvm::Value* new_value);

  void EmitStoreDalvikReg(unsigned reg_idx, char shorty,
                          JTypeSpace space, llvm::Value* new_value);

 private:
  //----------------------------------------------------------------------------
  // Return Value Related
  //----------------------------------------------------------------------------
  // Hold the return value returned from the lastest invoke-* instruction
  DalvikReg* retval_reg_;

  llvm::Value* EmitLoadDalvikRetValReg(JType jty, JTypeSpace space);

  llvm::Value* EmitLoadDalvikRetValReg(char shorty, JTypeSpace space);

  void EmitStoreDalvikRetValReg(JType jty, JTypeSpace space, llvm::Value* new_value);

  void EmitStoreDalvikRetValReg(char shorty, JTypeSpace space, llvm::Value* new_value);

 private:
  //----------------------------------------------------------------------------
  // Shadow Frame
  //----------------------------------------------------------------------------
  unsigned num_shadow_frame_entries_;
  std::vector<int32_t> reg_to_shadow_frame_index_;

  void EmitUpdateDexPC(unsigned dex_pc);

  void EmitPopShadowFrame();

 private:
  //----------------------------------------------------------------------------
  // RegCategory
  //----------------------------------------------------------------------------
  RegCategory GetInferredRegCategory(unsigned dex_pc, unsigned reg_idx);

  InferredRegCategoryMap const* GetInferredRegCategoryMap();

  bool IsRegCanBeObject(unsigned reg_idx);

 private:
  //----------------------------------------------------------------------------
  // Exception Handling
  //----------------------------------------------------------------------------
  std::vector<llvm::BasicBlock*> landing_pads_bb_;
  llvm::BasicBlock* exception_unwind_bb_;

  // cur_try_item_offset caches the latest try item offset such that we don't
  // have to call DexFile::FindCatchHandlerOffset(...) (using binary search) for
  // every query of the try item for the given dex_pc.
  int32_t cur_try_item_offset;

  int32_t GetTryItemOffset(unsigned dex_pc);

  llvm::BasicBlock* GetLandingPadBasicBlock(unsigned dex_pc);

  llvm::BasicBlock* GetUnwindBasicBlock();

  void EmitBranchExceptionLandingPad(unsigned dex_pc);

  void EmitGuard_DivZeroException(unsigned dex_pc,
                                  llvm::Value* denominator,
                                  JType op_jty);

  void EmitGuard_NullPointerException(unsigned dex_pc, llvm::Value* object);

  void EmitGuard_ArrayIndexOutOfBoundsException(unsigned dex_pc,
                                                llvm::Value* array,
                                                llvm::Value* index);

  void EmitGuard_ArrayException(unsigned dex_pc,
                                llvm::Value* array,
                                llvm::Value* index);

  void EmitGuard_ExceptionLandingPad(unsigned dex_pc, bool can_skip_unwind);

 private:
  //----------------------------------------------------------------------------
  // Garbage Collection Safe Point
  //----------------------------------------------------------------------------
  void EmitGuard_GarbageCollectionSuspend();

 private:
  //----------------------------------------------------------------------------
  // Code Generation
  //----------------------------------------------------------------------------
  bool CreateFunction();
  llvm::FunctionType* GetFunctionType();

  bool EmitPrologue();
  bool EmitPrologueAssignArgRegister();
  bool EmitPrologueAllcaShadowFrame();
  bool EmitPrologueLinkBasicBlocks();
  bool PrettyLayoutExceptionBasicBlocks();
  bool VerifyFunction();
  bool OptimizeFunction();
  // Our optimization passes
  bool RemoveRedundantPendingExceptionChecks();

  //----------------------------------------------------------------------------
  // Emit* Helper Functions
  //----------------------------------------------------------------------------
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

  llvm::Value* EmitLoadMethodObjectAddr();

  llvm::Value* EmitGetCurrentThread();

  void EmitMarkGCCard(llvm::Value* value, llvm::Value* target_addr);

  llvm::Value* EmitInvokeIntrinsicNoThrow(IntrinsicHelper::IntrinsicId intr_id,
                                          llvm::ArrayRef<llvm::Value*> args
                                              = llvm::ArrayRef<llvm::Value*>());
  llvm::Value* EmitInvokeIntrinsic2NoThrow(IntrinsicHelper::IntrinsicId intr_id,
                                           llvm::Value* arg1,
                                           llvm::Value* arg2) {
    llvm::Value* args[] = { arg1, arg2 };
    return EmitInvokeIntrinsicNoThrow(intr_id, args);
  }
  llvm::Value* EmitInvokeIntrinsic3NoThrow(IntrinsicHelper::IntrinsicId intr_id,
                                           llvm::Value* arg1,
                                           llvm::Value* arg2,
                                           llvm::Value* arg3) {
    llvm::Value* args[] = { arg1, arg2, arg3 };
    return EmitInvokeIntrinsicNoThrow(intr_id, args);
  }
  llvm::Value* EmitInvokeIntrinsic4NoThrow(IntrinsicHelper::IntrinsicId intr_id,
                                           llvm::Value* arg1,
                                           llvm::Value* arg2,
                                           llvm::Value* arg3,
                                           llvm::Value* arg4) {
    llvm::Value* args[] = { arg1, arg2, arg3, arg4 };
    return EmitInvokeIntrinsicNoThrow(intr_id, args);
  }

  llvm::Value* EmitInvokeIntrinsic(unsigned dex_pc, bool can_skip_unwind,
                                   IntrinsicHelper::IntrinsicId intr_id,
                                   llvm::ArrayRef<llvm::Value*> args
                                        = llvm::ArrayRef<llvm::Value*>());
  llvm::Value* EmitInvokeIntrinsic2(unsigned dex_pc, bool can_skip_unwind,
                                    IntrinsicHelper::IntrinsicId intr_id,
                                    llvm::Value* arg1,
                                    llvm::Value* arg2) {
    llvm::Value* args[] = { arg1, arg2 };
    return EmitInvokeIntrinsic(dex_pc, can_skip_unwind, intr_id, args);
  }
  llvm::Value* EmitInvokeIntrinsic3(unsigned dex_pc, bool can_skip_unwind,
                                    IntrinsicHelper::IntrinsicId intr_id,
                                    llvm::Value* arg1,
                                    llvm::Value* arg2,
                                    llvm::Value* arg3) {
    llvm::Value* args[] = { arg1, arg2, arg3 };
    return EmitInvokeIntrinsic(dex_pc, can_skip_unwind, intr_id, args);
  }
  llvm::Value* EmitInvokeIntrinsic4(unsigned dex_pc, bool can_skip_unwind,
                                    IntrinsicHelper::IntrinsicId intr_id,
                                    llvm::Value* arg1,
                                    llvm::Value* arg2,
                                    llvm::Value* arg3,
                                    llvm::Value* arg4) {
    llvm::Value* args[] = { arg1, arg2, arg3, arg4 };
    return EmitInvokeIntrinsic(dex_pc, can_skip_unwind, intr_id, args);
  }
  llvm::Value* EmitInvokeIntrinsic5(unsigned dex_pc, bool can_skip_unwind,
                                    IntrinsicHelper::IntrinsicId intr_id,
                                    llvm::Value* arg1,
                                    llvm::Value* arg2,
                                    llvm::Value* arg3,
                                    llvm::Value* arg4,
                                    llvm::Value* arg5) {
    llvm::Value* args[] = { arg1, arg2, arg3, arg4, arg5 };
    return EmitInvokeIntrinsic(dex_pc, can_skip_unwind, intr_id, args);
  }

  llvm::Value* EmitLoadConstantClass(unsigned dex_pc, uint32_t type_idx);

  llvm::Value* EmitLoadArrayLength(llvm::Value* array);

  llvm::Value* EmitAllocNewArray(unsigned dex_pc, int32_t length,
                                 uint32_t type_idx, bool is_filled_new_array);

  llvm::Value* EmitCompareResultSelection(llvm::Value* cmp_eq,
                                          llvm::Value* cmp_lt);

  llvm::Value* EmitLoadStaticStorage(unsigned dex_pc, unsigned type_idx);

  llvm::Value* EmitConditionResult(llvm::Value* lhs, llvm::Value* rhs,
                                   CondBranchKind cond);

  llvm::Value* EmitIntArithmResultComputation(unsigned dex_pc,
                                              llvm::Value* lhs,
                                              llvm::Value* rhs,
                                              IntArithmKind arithm,
                                              JType op_jty);

  llvm::Value* EmitIntShiftArithmResultComputation(uint32_t dex_pc,
                                                   llvm::Value* lhs,
                                                   llvm::Value* rhs,
                                                   IntShiftArithmKind arithm,
                                                   JType op_jty);

  llvm::Value* EmitIntDivRemResultComputation(unsigned dex_pc,
                                              llvm::Value* dividend,
                                              llvm::Value* divisor,
                                              IntArithmKind arithm,
                                              JType op_jty);

#define GEN_INSN_ARGS unsigned dex_pc, const Instruction* insn
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
  void EmitInsn_UnaryConditionalBranch(GEN_INSN_ARGS, CondBranchKind cond);
  void EmitInsn_BinaryConditionalBranch(GEN_INSN_ARGS, CondBranchKind cond);

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

  void EmitInsn_Invoke(GEN_INSN_ARGS,
                       InvokeType invoke_type,
                       InvokeArgFmt arg_fmt);

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
                        IntrinsicHelper::IntrinsicId intr_id);
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


  bool EmitInstructions();
  bool EmitInstruction(unsigned dex_pc, const Instruction* insn);


  // TODO: Use high-level IR to do this
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

  DISALLOW_COPY_AND_ASSIGN(DexLang);
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_DEX_LANG_H_
