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

#include "method_compiler.h"

#include "backend_types.h"
#include "compilation_unit.h"
#include "compiler.h"
#include "inferred_reg_category_map.h"
#include "ir_builder.h"
#include "logging.h"
#include "oat_compilation_unit.h"
#include "object.h"
#include "object_utils.h"
#include "runtime_support_func.h"
#include "stl_util.h"
#include "stringprintf.h"
#include "utils_llvm.h"

#include <iomanip>

#include <llvm/Analysis/Verifier.h>
#include <llvm/BasicBlock.h>
#include <llvm/Function.h>
#include <llvm/GlobalVariable.h>
#include <llvm/Intrinsics.h>

namespace art {
namespace compiler_llvm {

using namespace runtime_support;


MethodCompiler::MethodCompiler(CompilationUnit* cunit,
                               Compiler* compiler,
                               OatCompilationUnit* oat_compilation_unit)
  : cunit_(cunit), compiler_(compiler),
    class_linker_(oat_compilation_unit->class_linker_),
    class_loader_(oat_compilation_unit->class_loader_),
    dex_file_(oat_compilation_unit->dex_file_),
    dex_cache_(oat_compilation_unit->dex_cache_),
    code_item_(oat_compilation_unit->code_item_),
    oat_compilation_unit_(oat_compilation_unit),
    method_(dex_cache_->GetResolvedMethod(oat_compilation_unit->method_idx_)),
    method_helper_(method_),
    method_idx_(oat_compilation_unit->method_idx_),
    access_flags_(oat_compilation_unit->access_flags_),
    module_(cunit->GetModule()),
    context_(cunit->GetLLVMContext()),
    irb_(*cunit->GetIRBuilder()), func_(NULL), retval_reg_(NULL),
    basic_block_reg_alloca_(NULL), basic_block_shadow_frame_alloca_(NULL),
    basic_block_reg_zero_init_(NULL), basic_block_reg_arg_init_(NULL),
    basic_blocks_(code_item_->insns_size_in_code_units_),
    basic_block_landing_pads_(code_item_->tries_size_, NULL),
    basic_block_unwind_(NULL), basic_block_unreachable_(NULL),
    shadow_frame_(NULL) {
}


MethodCompiler::~MethodCompiler() {
  STLDeleteElements(&regs_);
}


void MethodCompiler::CreateFunction() {
  // LLVM function name
  std::string func_name(LLVMLongName(method_));

  // Get function type
  llvm::FunctionType* func_type =
    GetFunctionType(method_idx_, method_->IsStatic());

  // Create function
  func_ = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage,
                                 func_name, module_);

  // Set argument name
  llvm::Function::arg_iterator arg_iter(func_->arg_begin());
  llvm::Function::arg_iterator arg_end(func_->arg_end());

  DCHECK_NE(arg_iter, arg_end);
  arg_iter->setName("method");
  ++arg_iter;

  if (!method_->IsStatic()) {
    DCHECK_NE(arg_iter, arg_end);
    arg_iter->setName("this");
    ++arg_iter;
  }

  for (unsigned i = 0; arg_iter != arg_end; ++i, ++arg_iter) {
    arg_iter->setName(StringPrintf("a%u", i));
  }
}


llvm::FunctionType* MethodCompiler::GetFunctionType(uint32_t method_idx,
                                                    bool is_static) {
  // Get method signature
  DexFile::MethodId const& method_id = dex_file_->GetMethodId(method_idx);

  uint32_t shorty_size;
  char const* shorty = dex_file_->GetMethodShorty(method_id, &shorty_size);
  CHECK_GE(shorty_size, 1u);

  // Get return type
  llvm::Type* ret_type = irb_.getJType(shorty[0], kAccurate);

  // Get argument type
  std::vector<llvm::Type*> args_type;

  args_type.push_back(irb_.getJObjectTy()); // method object pointer

  if (!is_static) {
    args_type.push_back(irb_.getJType('L', kAccurate)); // "this" object pointer
  }

  for (uint32_t i = 1; i < shorty_size; ++i) {
    args_type.push_back(irb_.getJType(shorty[i], kAccurate));
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}


void MethodCompiler::EmitPrologue() {
  // Create basic blocks for prologue
  basic_block_reg_alloca_ =
    llvm::BasicBlock::Create(*context_, "prologue.alloca", func_);

  basic_block_shadow_frame_alloca_ =
    llvm::BasicBlock::Create(*context_, "prologue.shadowframe", func_);

  basic_block_reg_zero_init_ =
    llvm::BasicBlock::Create(*context_, "prologue.zeroinit", func_);

  basic_block_reg_arg_init_ =
    llvm::BasicBlock::Create(*context_, "prologue.arginit", func_);

  // Create register array
  for (uint16_t r = 0; r < code_item_->registers_size_; ++r) {
    regs_.push_back(DalvikReg::CreateLocalVarReg(*this, r));
  }

  retval_reg_.reset(DalvikReg::CreateRetValReg(*this));

  // Create Shadow Frame
  EmitPrologueAllocShadowFrame();

  // Store argument to dalvik register
  irb_.SetInsertPoint(basic_block_reg_arg_init_);
  EmitPrologueAssignArgRegister();

  // Branch to start address
  irb_.CreateBr(GetBasicBlock(0));
}


void MethodCompiler::EmitPrologueLastBranch() {
  irb_.SetInsertPoint(basic_block_reg_alloca_);
  irb_.CreateBr(basic_block_shadow_frame_alloca_);

  irb_.SetInsertPoint(basic_block_shadow_frame_alloca_);
  irb_.CreateBr(basic_block_reg_zero_init_);

  irb_.SetInsertPoint(basic_block_reg_zero_init_);
  irb_.CreateBr(basic_block_reg_arg_init_);
}


void MethodCompiler::EmitPrologueAllocShadowFrame() {
  irb_.SetInsertPoint(basic_block_shadow_frame_alloca_);

  // Allocate the shadow frame now!
  uint32_t sirt_size = code_item_->registers_size_;
  // TODO: registers_size_ is a bad approximation.  Compute a
  // tighter approximation at Dex verifier while performing data-flow
  // analysis.

  llvm::StructType* shadow_frame_type = irb_.getShadowFrameTy(sirt_size);
  shadow_frame_ = irb_.CreateAlloca(shadow_frame_type);

  // Zero-initialization of the shadow frame
  llvm::ConstantAggregateZero* zero_initializer =
    llvm::ConstantAggregateZero::get(shadow_frame_type);

  irb_.CreateStore(zero_initializer, shadow_frame_);

  // Variables for GetElementPtr
  llvm::Constant* zero = irb_.getInt32(0);

  llvm::Value* gep_index[] = {
    zero, // No displacement for shadow frame pointer
    zero, // Get the %ArtFrame data structure
    NULL,
  };

  // Store the method pointer
  gep_index[2] = irb_.getInt32(1);
  llvm::Value* method_field_addr = irb_.CreateGEP(shadow_frame_, gep_index);
  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();
  irb_.CreateStore(method_object_addr, method_field_addr);

  // Store the number of the pointer slots
  gep_index[2] = irb_.getInt32(3);
  llvm::Value* size_field_addr = irb_.CreateGEP(shadow_frame_, gep_index);
  llvm::ConstantInt* sirt_size_value = irb_.getInt32(sirt_size);
  irb_.CreateStore(sirt_size_value, size_field_addr);

  // Push the shadow frame
  llvm::Value* shadow_frame_upcast =
    irb_.CreateConstGEP2_32(shadow_frame_, 0, 0);

  irb_.CreateCall(irb_.GetRuntime(PushShadowFrame), shadow_frame_upcast);
}


void MethodCompiler::EmitPrologueAssignArgRegister() {
  uint16_t arg_reg = code_item_->registers_size_ - code_item_->ins_size_;

  llvm::Function::arg_iterator arg_iter(func_->arg_begin());
  llvm::Function::arg_iterator arg_end(func_->arg_end());

  char const* shorty = method_helper_.GetShorty();
  int32_t shorty_size = method_helper_.GetShortyLength();
  CHECK_LE(1, shorty_size);

  ++arg_iter; // skip method object

  if (!method_->IsStatic()) {
    EmitStoreDalvikReg(arg_reg, kObject, kAccurate, arg_iter);
    ++arg_iter;
    ++arg_reg;
  }

  for (int32_t i = 1; i < shorty_size; ++i, ++arg_iter) {
    EmitStoreDalvikReg(arg_reg, shorty[i], kAccurate, arg_iter);

    ++arg_reg;
    if (shorty[i] == 'J' || shorty[i] == 'D') {
      // Wide types, such as long and double, are using a pair of registers
      // to store the value, so we have to increase arg_reg again.
      ++arg_reg;
    }
  }

  DCHECK_EQ(arg_end, arg_iter);
}


void MethodCompiler::EmitInstructions() {
  uint32_t dex_pc = 0;
  while (dex_pc < code_item_->insns_size_in_code_units_) {
    Instruction const* insn = Instruction::At(code_item_->insns_ + dex_pc);
    EmitInstruction(dex_pc, insn);
    dex_pc += insn->SizeInCodeUnits();
  }
}


void MethodCompiler::EmitInstruction(uint32_t dex_pc,
                                     Instruction const* insn) {

  // Set the IRBuilder insertion point
  irb_.SetInsertPoint(GetBasicBlock(dex_pc));

#define ARGS dex_pc, insn

  // Dispatch the instruction
  switch (insn->Opcode()) {
  case Instruction::NOP:
    EmitInsn_Nop(ARGS);
    break;

  case Instruction::MOVE:
  case Instruction::MOVE_FROM16:
  case Instruction::MOVE_16:
    EmitInsn_Move(ARGS, kInt);
    break;

  case Instruction::MOVE_WIDE:
  case Instruction::MOVE_WIDE_FROM16:
  case Instruction::MOVE_WIDE_16:
    EmitInsn_Move(ARGS, kLong);
    break;

  case Instruction::MOVE_OBJECT:
  case Instruction::MOVE_OBJECT_FROM16:
  case Instruction::MOVE_OBJECT_16:
    EmitInsn_Move(ARGS, kObject);
    break;

  case Instruction::MOVE_RESULT:
    EmitInsn_MoveResult(ARGS, kInt);
    break;

  case Instruction::MOVE_RESULT_WIDE:
    EmitInsn_MoveResult(ARGS, kLong);
    break;

  case Instruction::MOVE_RESULT_OBJECT:
    EmitInsn_MoveResult(ARGS, kObject);
    break;

  case Instruction::MOVE_EXCEPTION:
    EmitInsn_MoveException(ARGS);
    break;

  case Instruction::RETURN_VOID:
    EmitInsn_ReturnVoid(ARGS);
    break;

  case Instruction::RETURN:
  case Instruction::RETURN_WIDE:
  case Instruction::RETURN_OBJECT:
    EmitInsn_Return(ARGS);
    break;

  case Instruction::CONST_4:
  case Instruction::CONST_16:
  case Instruction::CONST:
  case Instruction::CONST_HIGH16:
    EmitInsn_LoadConstant(ARGS, kInt);
    break;

  case Instruction::CONST_WIDE_16:
  case Instruction::CONST_WIDE_32:
  case Instruction::CONST_WIDE:
  case Instruction::CONST_WIDE_HIGH16:
    EmitInsn_LoadConstant(ARGS, kLong);
    break;

  case Instruction::CONST_STRING:
  case Instruction::CONST_STRING_JUMBO:
    EmitInsn_LoadConstantString(ARGS);
    break;

  case Instruction::CONST_CLASS:
    EmitInsn_LoadConstantClass(ARGS);
    break;

  case Instruction::MONITOR_ENTER:
    EmitInsn_MonitorEnter(ARGS);
    break;

  case Instruction::MONITOR_EXIT:
    EmitInsn_MonitorExit(ARGS);
    break;

  case Instruction::CHECK_CAST:
    EmitInsn_CheckCast(ARGS);
    break;

  case Instruction::INSTANCE_OF:
    EmitInsn_InstanceOf(ARGS);
    break;

  case Instruction::ARRAY_LENGTH:
    EmitInsn_ArrayLength(ARGS);
    break;

  case Instruction::NEW_INSTANCE:
    EmitInsn_NewInstance(ARGS);
    break;

  case Instruction::NEW_ARRAY:
    EmitInsn_NewArray(ARGS);
    break;

  case Instruction::FILLED_NEW_ARRAY:
    EmitInsn_FilledNewArray(ARGS, false);
    break;

  case Instruction::FILLED_NEW_ARRAY_RANGE:
    EmitInsn_FilledNewArray(ARGS, true);
    break;

  case Instruction::FILL_ARRAY_DATA:
    EmitInsn_FillArrayData(ARGS);
    break;

  case Instruction::THROW:
    EmitInsn_ThrowException(ARGS);
    break;

  case Instruction::GOTO:
  case Instruction::GOTO_16:
  case Instruction::GOTO_32:
    EmitInsn_UnconditionalBranch(ARGS);
    break;

  case Instruction::PACKED_SWITCH:
    EmitInsn_PackedSwitch(ARGS);
    break;

  case Instruction::SPARSE_SWITCH:
    EmitInsn_SparseSwitch(ARGS);
    break;

  case Instruction::CMPL_FLOAT:
    EmitInsn_FPCompare(ARGS, kFloat, false);
    break;

  case Instruction::CMPG_FLOAT:
    EmitInsn_FPCompare(ARGS, kFloat, true);
    break;

  case Instruction::CMPL_DOUBLE:
    EmitInsn_FPCompare(ARGS, kDouble, false);
    break;

  case Instruction::CMPG_DOUBLE:
    EmitInsn_FPCompare(ARGS, kDouble, true);
    break;

  case Instruction::CMP_LONG:
    EmitInsn_LongCompare(ARGS);
    break;

  case Instruction::IF_EQ:
    EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_EQ);
    break;

  case Instruction::IF_NE:
    EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_NE);
    break;

  case Instruction::IF_LT:
    EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_LT);
    break;

  case Instruction::IF_GE:
    EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_GE);
    break;

  case Instruction::IF_GT:
    EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_GT);
    break;

  case Instruction::IF_LE:
    EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_LE);
    break;

  case Instruction::IF_EQZ:
    EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_EQ);
    break;

  case Instruction::IF_NEZ:
    EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_NE);
    break;

  case Instruction::IF_LTZ:
    EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_LT);
    break;

  case Instruction::IF_GEZ:
    EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_GE);
    break;

  case Instruction::IF_GTZ:
    EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_GT);
    break;

  case Instruction::IF_LEZ:
    EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_LE);
    break;

  case Instruction::AGET:
    EmitInsn_AGet(ARGS, kInt);
    break;

  case Instruction::AGET_WIDE:
    EmitInsn_AGet(ARGS, kLong);
    break;

  case Instruction::AGET_OBJECT:
    EmitInsn_AGet(ARGS, kObject);
    break;

  case Instruction::AGET_BOOLEAN:
    EmitInsn_AGet(ARGS, kBoolean);
    break;

  case Instruction::AGET_BYTE:
    EmitInsn_AGet(ARGS, kByte);
    break;

  case Instruction::AGET_CHAR:
    EmitInsn_AGet(ARGS, kChar);
    break;

  case Instruction::AGET_SHORT:
    EmitInsn_AGet(ARGS, kShort);
    break;

  case Instruction::APUT:
    EmitInsn_APut(ARGS, kInt);
    break;

  case Instruction::APUT_WIDE:
    EmitInsn_APut(ARGS, kLong);
    break;

  case Instruction::APUT_OBJECT:
    EmitInsn_APut(ARGS, kObject);
    break;

  case Instruction::APUT_BOOLEAN:
    EmitInsn_APut(ARGS, kBoolean);
    break;

  case Instruction::APUT_BYTE:
    EmitInsn_APut(ARGS, kByte);
    break;

  case Instruction::APUT_CHAR:
    EmitInsn_APut(ARGS, kChar);
    break;

  case Instruction::APUT_SHORT:
    EmitInsn_APut(ARGS, kShort);
    break;

  case Instruction::IGET:
    EmitInsn_IGet(ARGS, kInt);
    break;

  case Instruction::IGET_WIDE:
    EmitInsn_IGet(ARGS, kLong);
    break;

  case Instruction::IGET_OBJECT:
    EmitInsn_IGet(ARGS, kObject);
    break;

  case Instruction::IGET_BOOLEAN:
    EmitInsn_IGet(ARGS, kBoolean);
    break;

  case Instruction::IGET_BYTE:
    EmitInsn_IGet(ARGS, kByte);
    break;

  case Instruction::IGET_CHAR:
    EmitInsn_IGet(ARGS, kChar);
    break;

  case Instruction::IGET_SHORT:
    EmitInsn_IGet(ARGS, kShort);
    break;

  case Instruction::IPUT:
    EmitInsn_IPut(ARGS, kInt);
    break;

  case Instruction::IPUT_WIDE:
    EmitInsn_IPut(ARGS, kLong);
    break;

  case Instruction::IPUT_OBJECT:
    EmitInsn_IPut(ARGS, kObject);
    break;

  case Instruction::IPUT_BOOLEAN:
    EmitInsn_IPut(ARGS, kBoolean);
    break;

  case Instruction::IPUT_BYTE:
    EmitInsn_IPut(ARGS, kByte);
    break;

  case Instruction::IPUT_CHAR:
    EmitInsn_IPut(ARGS, kChar);
    break;

  case Instruction::IPUT_SHORT:
    EmitInsn_IPut(ARGS, kShort);
    break;

  case Instruction::SGET:
    EmitInsn_SGet(ARGS, kInt);
    break;

  case Instruction::SGET_WIDE:
    EmitInsn_SGet(ARGS, kLong);
    break;

  case Instruction::SGET_OBJECT:
    EmitInsn_SGet(ARGS, kObject);
    break;

  case Instruction::SGET_BOOLEAN:
    EmitInsn_SGet(ARGS, kBoolean);
    break;

  case Instruction::SGET_BYTE:
    EmitInsn_SGet(ARGS, kByte);
    break;

  case Instruction::SGET_CHAR:
    EmitInsn_SGet(ARGS, kChar);
    break;

  case Instruction::SGET_SHORT:
    EmitInsn_SGet(ARGS, kShort);
    break;

  case Instruction::SPUT:
    EmitInsn_SPut(ARGS, kInt);
    break;

  case Instruction::SPUT_WIDE:
    EmitInsn_SPut(ARGS, kLong);
    break;

  case Instruction::SPUT_OBJECT:
    EmitInsn_SPut(ARGS, kObject);
    break;

  case Instruction::SPUT_BOOLEAN:
    EmitInsn_SPut(ARGS, kBoolean);
    break;

  case Instruction::SPUT_BYTE:
    EmitInsn_SPut(ARGS, kByte);
    break;

  case Instruction::SPUT_CHAR:
    EmitInsn_SPut(ARGS, kChar);
    break;

  case Instruction::SPUT_SHORT:
    EmitInsn_SPut(ARGS, kShort);
    break;


  case Instruction::INVOKE_VIRTUAL:
    EmitInsn_InvokeVirtual(ARGS, false);
    break;

  case Instruction::INVOKE_SUPER:
    EmitInsn_InvokeSuper(ARGS, false);
    break;

  case Instruction::INVOKE_DIRECT:
    EmitInsn_InvokeStaticDirect(ARGS, kDirect, /* is_range */ false);
    break;

  case Instruction::INVOKE_STATIC:
    EmitInsn_InvokeStaticDirect(ARGS, kStatic, /* is_range */ false);
    break;

  case Instruction::INVOKE_INTERFACE:
    EmitInsn_InvokeInterface(ARGS, false);
    break;

  case Instruction::INVOKE_VIRTUAL_RANGE:
    EmitInsn_InvokeVirtual(ARGS, true);
    break;

  case Instruction::INVOKE_SUPER_RANGE:
    EmitInsn_InvokeSuper(ARGS, true);
    break;

  case Instruction::INVOKE_DIRECT_RANGE:
    EmitInsn_InvokeStaticDirect(ARGS, kDirect, true);
    break;

  case Instruction::INVOKE_STATIC_RANGE:
    EmitInsn_InvokeStaticDirect(ARGS, kStatic, true);
    break;

  case Instruction::INVOKE_INTERFACE_RANGE:
    EmitInsn_InvokeInterface(ARGS, true);
    break;

  case Instruction::NEG_INT:
    EmitInsn_Neg(ARGS, kInt);
    break;

  case Instruction::NOT_INT:
    EmitInsn_Not(ARGS, kInt);
    break;

  case Instruction::NEG_LONG:
    EmitInsn_Neg(ARGS, kLong);
    break;

  case Instruction::NOT_LONG:
    EmitInsn_Not(ARGS, kLong);
    break;

  case Instruction::NEG_FLOAT:
    EmitInsn_FNeg(ARGS, kFloat);
    break;

  case Instruction::NEG_DOUBLE:
    EmitInsn_FNeg(ARGS, kDouble);
    break;

  case Instruction::INT_TO_LONG:
    EmitInsn_SExt(ARGS);
    break;

  case Instruction::INT_TO_FLOAT:
    EmitInsn_IntToFP(ARGS, kInt, kFloat);
    break;

  case Instruction::INT_TO_DOUBLE:
    EmitInsn_IntToFP(ARGS, kInt, kDouble);
    break;

  case Instruction::LONG_TO_INT:
    EmitInsn_Trunc(ARGS);
    break;

  case Instruction::LONG_TO_FLOAT:
    EmitInsn_IntToFP(ARGS, kLong, kFloat);
    break;

  case Instruction::LONG_TO_DOUBLE:
    EmitInsn_IntToFP(ARGS, kLong, kDouble);
    break;

  case Instruction::FLOAT_TO_INT:
    EmitInsn_FPToInt(ARGS, kFloat, kInt);
    break;

  case Instruction::FLOAT_TO_LONG:
    EmitInsn_FPToInt(ARGS, kFloat, kLong);
    break;

  case Instruction::FLOAT_TO_DOUBLE:
    EmitInsn_FExt(ARGS);
    break;

  case Instruction::DOUBLE_TO_INT:
    EmitInsn_FPToInt(ARGS, kDouble, kInt);
    break;

  case Instruction::DOUBLE_TO_LONG:
    EmitInsn_FPToInt(ARGS, kDouble, kLong);
    break;

  case Instruction::DOUBLE_TO_FLOAT:
    EmitInsn_FTrunc(ARGS);
    break;

  case Instruction::INT_TO_BYTE:
    EmitInsn_TruncAndSExt(ARGS, 8);
    break;

  case Instruction::INT_TO_CHAR:
    EmitInsn_TruncAndZExt(ARGS, 16);
    break;

  case Instruction::INT_TO_SHORT:
    EmitInsn_TruncAndSExt(ARGS, 16);
    break;

  case Instruction::ADD_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_Add, kInt, false);
    break;

  case Instruction::SUB_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_Sub, kInt, false);
    break;

  case Instruction::MUL_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_Mul, kInt, false);
    break;

  case Instruction::DIV_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_Div, kInt, false);
    break;

  case Instruction::REM_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_Rem, kInt, false);
    break;

  case Instruction::AND_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_And, kInt, false);
    break;

  case Instruction::OR_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_Or, kInt, false);
    break;

  case Instruction::XOR_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_Xor, kInt, false);
    break;

  case Instruction::SHL_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_Shl, kInt, false);
    break;

  case Instruction::SHR_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_Shr, kInt, false);
    break;

  case Instruction::USHR_INT:
    EmitInsn_IntArithm(ARGS, kIntArithm_UShr, kInt, false);
    break;

  case Instruction::ADD_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_Add, kLong, false);
    break;

  case Instruction::SUB_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_Sub, kLong, false);
    break;

  case Instruction::MUL_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_Mul, kLong, false);
    break;

  case Instruction::DIV_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_Div, kLong, false);
    break;

  case Instruction::REM_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_Rem, kLong, false);
    break;

  case Instruction::AND_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_And, kLong, false);
    break;

  case Instruction::OR_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_Or, kLong, false);
    break;

  case Instruction::XOR_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_Xor, kLong, false);
    break;

  case Instruction::SHL_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_Shl, kLong, false);
    break;

  case Instruction::SHR_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_Shr, kLong, false);
    break;

  case Instruction::USHR_LONG:
    EmitInsn_IntArithm(ARGS, kIntArithm_UShr, kLong, false);
    break;

  case Instruction::ADD_FLOAT:
    EmitInsn_FPArithm(ARGS, kFPArithm_Add, kFloat, false);
    break;

  case Instruction::SUB_FLOAT:
    EmitInsn_FPArithm(ARGS, kFPArithm_Sub, kFloat, false);
    break;

  case Instruction::MUL_FLOAT:
    EmitInsn_FPArithm(ARGS, kFPArithm_Mul, kFloat, false);
    break;

  case Instruction::DIV_FLOAT:
    EmitInsn_FPArithm(ARGS, kFPArithm_Div, kFloat, false);
    break;

  case Instruction::REM_FLOAT:
    EmitInsn_FPArithm(ARGS, kFPArithm_Rem, kFloat, false);
    break;

  case Instruction::ADD_DOUBLE:
    EmitInsn_FPArithm(ARGS, kFPArithm_Add, kDouble, false);
    break;

  case Instruction::SUB_DOUBLE:
    EmitInsn_FPArithm(ARGS, kFPArithm_Sub, kDouble, false);
    break;

  case Instruction::MUL_DOUBLE:
    EmitInsn_FPArithm(ARGS, kFPArithm_Mul, kDouble, false);
    break;

  case Instruction::DIV_DOUBLE:
    EmitInsn_FPArithm(ARGS, kFPArithm_Div, kDouble, false);
    break;

  case Instruction::REM_DOUBLE:
    EmitInsn_FPArithm(ARGS, kFPArithm_Rem, kDouble, false);
    break;

  case Instruction::ADD_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Add, kInt, true);
    break;

  case Instruction::SUB_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Sub, kInt, true);
    break;

  case Instruction::MUL_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Mul, kInt, true);
    break;

  case Instruction::DIV_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Div, kInt, true);
    break;

  case Instruction::REM_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Rem, kInt, true);
    break;

  case Instruction::AND_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_And, kInt, true);
    break;

  case Instruction::OR_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Or, kInt, true);
    break;

  case Instruction::XOR_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Xor, kInt, true);
    break;

  case Instruction::SHL_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Shl, kInt, true);
    break;

  case Instruction::SHR_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Shr, kInt, true);
    break;

  case Instruction::USHR_INT_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_UShr, kInt, true);
    break;

  case Instruction::ADD_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Add, kLong, true);
    break;

  case Instruction::SUB_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Sub, kLong, true);
    break;

  case Instruction::MUL_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Mul, kLong, true);
    break;

  case Instruction::DIV_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Div, kLong, true);
    break;

  case Instruction::REM_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Rem, kLong, true);
    break;

  case Instruction::AND_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_And, kLong, true);
    break;

  case Instruction::OR_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Or, kLong, true);
    break;

  case Instruction::XOR_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Xor, kLong, true);
    break;

  case Instruction::SHL_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Shl, kLong, true);
    break;

  case Instruction::SHR_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_Shr, kLong, true);
    break;

  case Instruction::USHR_LONG_2ADDR:
    EmitInsn_IntArithm(ARGS, kIntArithm_UShr, kLong, true);
    break;

  case Instruction::ADD_FLOAT_2ADDR:
    EmitInsn_FPArithm(ARGS, kFPArithm_Add, kFloat, true);
    break;

  case Instruction::SUB_FLOAT_2ADDR:
    EmitInsn_FPArithm(ARGS, kFPArithm_Sub, kFloat, true);
    break;

  case Instruction::MUL_FLOAT_2ADDR:
    EmitInsn_FPArithm(ARGS, kFPArithm_Mul, kFloat, true);
    break;

  case Instruction::DIV_FLOAT_2ADDR:
    EmitInsn_FPArithm(ARGS, kFPArithm_Div, kFloat, true);
    break;

  case Instruction::REM_FLOAT_2ADDR:
    EmitInsn_FPArithm(ARGS, kFPArithm_Rem, kFloat, true);
    break;

  case Instruction::ADD_DOUBLE_2ADDR:
    EmitInsn_FPArithm(ARGS, kFPArithm_Add, kDouble, true);
    break;

  case Instruction::SUB_DOUBLE_2ADDR:
    EmitInsn_FPArithm(ARGS, kFPArithm_Sub, kDouble, true);
    break;

  case Instruction::MUL_DOUBLE_2ADDR:
    EmitInsn_FPArithm(ARGS, kFPArithm_Mul, kDouble, true);
    break;

  case Instruction::DIV_DOUBLE_2ADDR:
    EmitInsn_FPArithm(ARGS, kFPArithm_Div, kDouble, true);
    break;

  case Instruction::REM_DOUBLE_2ADDR:
    EmitInsn_FPArithm(ARGS, kFPArithm_Rem, kDouble, true);
    break;

  case Instruction::ADD_INT_LIT16:
  case Instruction::ADD_INT_LIT8:
    EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Add);
    break;

  case Instruction::RSUB_INT:
  case Instruction::RSUB_INT_LIT8:
    EmitInsn_RSubImmediate(ARGS);
    break;

  case Instruction::MUL_INT_LIT16:
  case Instruction::MUL_INT_LIT8:
    EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Mul);
    break;

  case Instruction::DIV_INT_LIT16:
  case Instruction::DIV_INT_LIT8:
    EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Div);
    break;

  case Instruction::REM_INT_LIT16:
  case Instruction::REM_INT_LIT8:
    EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Rem);
    break;

  case Instruction::AND_INT_LIT16:
  case Instruction::AND_INT_LIT8:
    EmitInsn_IntArithmImmediate(ARGS, kIntArithm_And);
    break;

  case Instruction::OR_INT_LIT16:
  case Instruction::OR_INT_LIT8:
    EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Or);
    break;

  case Instruction::XOR_INT_LIT16:
  case Instruction::XOR_INT_LIT8:
    EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Xor);
    break;

  case Instruction::SHL_INT_LIT8:
    EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Shl);
    break;

  case Instruction::SHR_INT_LIT8:
    EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Shr);
    break;

  case Instruction::USHR_INT_LIT8:
    EmitInsn_IntArithmImmediate(ARGS, kIntArithm_UShr);
    break;

  case Instruction::UNUSED_3E:
  case Instruction::UNUSED_3F:
  case Instruction::UNUSED_40:
  case Instruction::UNUSED_41:
  case Instruction::UNUSED_42:
  case Instruction::UNUSED_43:
  case Instruction::UNUSED_73:
  case Instruction::UNUSED_79:
  case Instruction::UNUSED_7A:
  case Instruction::UNUSED_E3:
  case Instruction::UNUSED_E4:
  case Instruction::UNUSED_E5:
  case Instruction::UNUSED_E6:
  case Instruction::UNUSED_E7:
  case Instruction::UNUSED_E8:
  case Instruction::UNUSED_E9:
  case Instruction::UNUSED_EA:
  case Instruction::UNUSED_EB:
  case Instruction::UNUSED_EC:
  case Instruction::THROW_VERIFICATION_ERROR:
  case Instruction::UNUSED_EE:
  case Instruction::UNUSED_EF:
  case Instruction::UNUSED_F0:
  case Instruction::UNUSED_F1:
  case Instruction::UNUSED_F2:
  case Instruction::UNUSED_F3:
  case Instruction::UNUSED_F4:
  case Instruction::UNUSED_F5:
  case Instruction::UNUSED_F6:
  case Instruction::UNUSED_F7:
  case Instruction::UNUSED_F8:
  case Instruction::UNUSED_F9:
  case Instruction::UNUSED_FA:
  case Instruction::UNUSED_FB:
  case Instruction::UNUSED_FC:
  case Instruction::UNUSED_FD:
  case Instruction::UNUSED_FE:
  case Instruction::UNUSED_FF:
    LOG(FATAL) << "Dex file contains UNUSED bytecode: " << insn->Opcode();
    break;
  }

#undef ARGS
}


void MethodCompiler::EmitInsn_Nop(uint32_t dex_pc,
                                  Instruction const* insn) {

  uint16_t insn_signature = code_item_->insns_[dex_pc];

  if (insn_signature == Instruction::kPackedSwitchSignature ||
      insn_signature == Instruction::kSparseSwitchSignature ||
      insn_signature == Instruction::kArrayDataSignature) {
    irb_.CreateUnreachable();
  } else {
    irb_.CreateBr(GetNextBasicBlock(dex_pc));
  }
}


void MethodCompiler::EmitInsn_Move(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType jty) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, jty, kReg);
  EmitStoreDalvikReg(dec_insn.vA, jty, kReg, src_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_MoveResult(uint32_t dex_pc,
                                         Instruction const* insn,
                                         JType jty) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikRetValReg(jty, kReg);
  EmitStoreDalvikReg(dec_insn.vA, jty, kReg, src_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_MoveException(uint32_t dex_pc,
                                            Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  // Get thread-local exception field address
  llvm::Constant* exception_field_offset =
    irb_.getPtrEquivInt(Thread::ExceptionOffset().Int32Value());

  llvm::Value* thread_object_addr =
    irb_.CreateCall(irb_.GetRuntime(GetCurrentThread));

  llvm::Value* exception_field_addr =
    irb_.CreatePtrDisp(thread_object_addr, exception_field_offset,
                       irb_.getJObjectTy()->getPointerTo());

  // Get exception object address
  llvm::Value* exception_object_addr = irb_.CreateLoad(exception_field_addr);

  // Set thread-local exception field address to NULL
  irb_.CreateStore(irb_.getJNull(), exception_field_addr);

  // Keep the exception object in the Dalvik register
  EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, exception_object_addr);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_ThrowException(uint32_t dex_pc,
                                             Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* exception_addr =
    EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);

  EmitUpdateLineNumFromDexPC(dex_pc);

  irb_.CreateCall(irb_.GetRuntime(ThrowException), exception_addr);

  EmitBranchExceptionLandingPad(dex_pc);
}


void MethodCompiler::EmitInsn_ReturnVoid(uint32_t dex_pc,
                                         Instruction const* insn) {
  // Garbage collection safe-point
  EmitGuard_GarbageCollectionSuspend(dex_pc);

  // Pop the shadow frame
  EmitPopShadowFrame();

  // Return!
  irb_.CreateRetVoid();
}


void MethodCompiler::EmitInsn_Return(uint32_t dex_pc,
                                     Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  // Garbage collection safe-point
  EmitGuard_GarbageCollectionSuspend(dex_pc);

  // Pop the shadow frame
  EmitPopShadowFrame();
  // NOTE: It is important to keep this AFTER the GC safe-point.  Otherwise,
  // the return value might be collected since the shadow stack is popped.

  // Return!
  char ret_shorty = method_helper_.GetShorty()[0];
  llvm::Value* retval = EmitLoadDalvikReg(dec_insn.vA, ret_shorty, kAccurate);

  irb_.CreateRet(retval);
}


void MethodCompiler::EmitInsn_LoadConstant(uint32_t dex_pc,
                                           Instruction const* insn,
                                           JType imm_jty) {

  DecodedInstruction dec_insn(insn);

  DCHECK(imm_jty == kInt || imm_jty == kLong) << imm_jty;

  int64_t imm = 0;

  switch (insn->Opcode()) {
  // 32-bit Immediate
  case Instruction::CONST_4:
  case Instruction::CONST_16:
  case Instruction::CONST:
  case Instruction::CONST_WIDE_16:
  case Instruction::CONST_WIDE_32:
    imm = static_cast<int64_t>(static_cast<int32_t>(dec_insn.vB));
    break;

  case Instruction::CONST_HIGH16:
    imm = static_cast<int64_t>(static_cast<int32_t>(
          static_cast<uint32_t>(static_cast<uint16_t>(dec_insn.vB)) << 16));
    break;

  // 64-bit Immediate
  case Instruction::CONST_WIDE:
    imm = static_cast<int64_t>(dec_insn.vB_wide);
    break;

  case Instruction::CONST_WIDE_HIGH16:
    imm = static_cast<int64_t>(
          static_cast<uint64_t>(static_cast<uint16_t>(dec_insn.vB)) << 48);
    break;

  // Unknown opcode for load constant (unreachable)
  default:
    LOG(FATAL) << "Unknown opcode for load constant: " << insn->Opcode();
    break;
  }

  // Store the non-object register
  llvm::Type* imm_type = irb_.getJType(imm_jty, kAccurate);
  llvm::Constant* imm_value = llvm::ConstantInt::getSigned(imm_type, imm);
  EmitStoreDalvikReg(dec_insn.vA, imm_jty, kAccurate, imm_value);

  // Store the object register if it is possible to be null.
  if (imm_jty == kInt && imm == 0) {
    EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, irb_.getJNull());
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_LoadConstantString(uint32_t dex_pc,
                                                 Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  uint32_t string_idx = dec_insn.vB;

  llvm::Value* string_field_addr = EmitLoadDexCacheStringFieldAddr(string_idx);

  llvm::Value* string_addr = irb_.CreateLoad(string_field_addr);

  if (!compiler_->CanAssumeStringIsPresentInDexCache(dex_cache_, string_idx)) {
    llvm::BasicBlock* block_str_exist =
      CreateBasicBlockWithDexPC(dex_pc, "str_exist");

    llvm::BasicBlock* block_str_resolve =
      CreateBasicBlockWithDexPC(dex_pc, "str_resolve");

    // Test: Is the string resolved and in the dex cache?
    llvm::Value* equal_null = irb_.CreateICmpEQ(string_addr, irb_.getJNull());

    irb_.CreateCondBr(equal_null, block_str_exist, block_str_resolve);

    // String is resolved, go to next basic block.
    irb_.SetInsertPoint(block_str_exist);
    EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, string_addr);
    irb_.CreateBr(GetNextBasicBlock(dex_pc));

    // String is not resolved yet, resolve it now.
    irb_.SetInsertPoint(block_str_resolve);

    llvm::Function* runtime_func = irb_.GetRuntime(ResolveString);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    llvm::Value* string_idx_value = irb_.getInt32(string_idx);

    EmitUpdateLineNumFromDexPC(dex_pc);

    string_addr = irb_.CreateCall2(runtime_func, method_object_addr,
                                   string_idx_value);

    EmitGuard_ExceptionLandingPad(dex_pc);
  }

  // Store the string object to the Dalvik register
  EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, string_addr);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


llvm::Value* MethodCompiler::EmitLoadConstantClass(uint32_t dex_pc,
                                                   uint32_t type_idx) {
  if (!compiler_->CanAccessTypeWithoutChecks(method_idx_, dex_cache_,
                                             *dex_file_, type_idx)) {
    llvm::Value* type_idx_value = irb_.getInt32(type_idx);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    llvm::Function* runtime_func =
      irb_.GetRuntime(InitializeTypeAndVerifyAccess);

    EmitUpdateLineNumFromDexPC(dex_pc);

    llvm::Value* type_object_addr =
      irb_.CreateCall2(runtime_func, type_idx_value, method_object_addr);

    EmitGuard_ExceptionLandingPad(dex_pc);

    return type_object_addr;

  } else {
    // Try to load the class (type) object from the test cache.
    llvm::Value* type_field_addr =
      EmitLoadDexCacheResolvedTypeFieldAddr(type_idx);

    llvm::Value* type_object_addr = irb_.CreateLoad(type_field_addr);

    if (compiler_->CanAssumeTypeIsPresentInDexCache(dex_cache_, type_idx)) {
      return type_object_addr;
    }

    llvm::BasicBlock* block_original = irb_.GetInsertBlock();

    // Test whether class (type) object is in the dex cache or not
    llvm::Value* equal_null =
      irb_.CreateICmpEQ(type_object_addr, irb_.getJNull());

    llvm::BasicBlock* block_cont =
      CreateBasicBlockWithDexPC(dex_pc, "cont");

    llvm::BasicBlock* block_load_class =
      CreateBasicBlockWithDexPC(dex_pc, "load_class");

    irb_.CreateCondBr(equal_null, block_load_class, block_cont);

    // Failback routine to load the class object
    irb_.SetInsertPoint(block_load_class);

    llvm::Function* runtime_func = irb_.GetRuntime(InitializeType);

    llvm::Constant* type_idx_value = irb_.getInt32(type_idx);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitUpdateLineNumFromDexPC(dex_pc);

    llvm::Value* loaded_type_object_addr =
      irb_.CreateCall2(runtime_func, type_idx_value, method_object_addr);

    EmitGuard_ExceptionLandingPad(dex_pc);

    llvm::BasicBlock* block_after_load_class = irb_.GetInsertBlock();

    irb_.CreateBr(block_cont);

    // Now the class object must be loaded
    irb_.SetInsertPoint(block_cont);

    llvm::PHINode* phi = irb_.CreatePHI(irb_.getJObjectTy(), 2);

    phi->addIncoming(type_object_addr, block_original);
    phi->addIncoming(loaded_type_object_addr, block_after_load_class);

    return phi;
  }
}


void MethodCompiler::EmitInsn_LoadConstantClass(uint32_t dex_pc,
                                                Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* type_object_addr = EmitLoadConstantClass(dex_pc, dec_insn.vB);
  EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, type_object_addr);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_MonitorEnter(uint32_t dex_pc,
                                           Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* object_addr =
    EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);

  // TODO: Slow path always. May not need NullPointerException check.
  EmitGuard_NullPointerException(dex_pc, object_addr);

  EmitUpdateLineNumFromDexPC(dex_pc);

  irb_.CreateCall(irb_.GetRuntime(LockObject), object_addr);
  EmitGuard_ExceptionLandingPad(dex_pc);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_MonitorExit(uint32_t dex_pc,
                                          Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* object_addr =
    EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);

  EmitGuard_NullPointerException(dex_pc, object_addr);

  EmitUpdateLineNumFromDexPC(dex_pc);

  irb_.CreateCall(irb_.GetRuntime(UnlockObject), object_addr);
  EmitGuard_ExceptionLandingPad(dex_pc);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_CheckCast(uint32_t dex_pc,
                                        Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::BasicBlock* block_test_class =
    CreateBasicBlockWithDexPC(dex_pc, "test_class");

  llvm::BasicBlock* block_test_sub_class =
    CreateBasicBlockWithDexPC(dex_pc, "test_sub_class");

  llvm::Value* object_addr =
    EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);

  // Test: Is the reference equal to null?  Act as no-op when it is null.
  llvm::Value* equal_null = irb_.CreateICmpEQ(object_addr, irb_.getJNull());

  irb_.CreateCondBr(equal_null,
                    GetNextBasicBlock(dex_pc),
                    block_test_class);

  // Test: Is the object instantiated from the given class?
  irb_.SetInsertPoint(block_test_class);
  llvm::Value* type_object_addr = EmitLoadConstantClass(dex_pc, dec_insn.vB);
  DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);

  llvm::PointerType* jobject_ptr_ty = irb_.getJObjectTy();

  llvm::Value* object_type_field_addr =
    irb_.CreateBitCast(object_addr, jobject_ptr_ty->getPointerTo());

  llvm::Value* object_type_object_addr =
    irb_.CreateLoad(object_type_field_addr);

  llvm::Value* equal_class =
    irb_.CreateICmpEQ(type_object_addr, object_type_object_addr);

  irb_.CreateCondBr(equal_class,
                    GetNextBasicBlock(dex_pc),
                    block_test_sub_class);

  // Test: Is the object instantiated from the subclass of the given class?
  irb_.SetInsertPoint(block_test_sub_class);

  EmitUpdateLineNumFromDexPC(dex_pc);

  irb_.CreateCall2(irb_.GetRuntime(CheckCast),
                   type_object_addr, object_type_object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_InstanceOf(uint32_t dex_pc,
                                         Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Constant* zero = irb_.getJInt(0);
  llvm::Constant* one = irb_.getJInt(1);

  llvm::BasicBlock* block_nullp = CreateBasicBlockWithDexPC(dex_pc, "nullp");

  llvm::BasicBlock* block_test_class =
    CreateBasicBlockWithDexPC(dex_pc, "test_class");

  llvm::BasicBlock* block_class_equals =
    CreateBasicBlockWithDexPC(dex_pc, "class_eq");

  llvm::BasicBlock* block_test_sub_class =
    CreateBasicBlockWithDexPC(dex_pc, "test_sub_class");

  llvm::Value* object_addr =
    EmitLoadDalvikReg(dec_insn.vB, kObject, kAccurate);

  // Overview of the following code :
  // We check for null, if so, then false, otherwise check for class == . If so
  // then true, otherwise do callout slowpath.
  //
  // Test: Is the reference equal to null?  Set 0 when it is null.
  llvm::Value* equal_null = irb_.CreateICmpEQ(object_addr, irb_.getJNull());

  irb_.CreateCondBr(equal_null, block_nullp, block_test_class);

  irb_.SetInsertPoint(block_nullp);
  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, zero);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));

  // Test: Is the object instantiated from the given class?
  irb_.SetInsertPoint(block_test_class);
  llvm::Value* type_object_addr = EmitLoadConstantClass(dex_pc, dec_insn.vC);
  DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);

  llvm::PointerType* jobject_ptr_ty = irb_.getJObjectTy();

  llvm::Value* object_type_field_addr =
    irb_.CreateBitCast(object_addr, jobject_ptr_ty->getPointerTo());

  llvm::Value* object_type_object_addr =
    irb_.CreateLoad(object_type_field_addr);

  llvm::Value* equal_class =
    irb_.CreateICmpEQ(type_object_addr, object_type_object_addr);

  irb_.CreateCondBr(equal_class, block_class_equals, block_test_sub_class);

  irb_.SetInsertPoint(block_class_equals);
  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, one);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));

  // Test: Is the object instantiated from the subclass of the given class?
  irb_.SetInsertPoint(block_test_sub_class);

  llvm::Value* result =
    irb_.CreateCall2(irb_.GetRuntime(IsAssignable),
                     type_object_addr, object_type_object_addr);

  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, result);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


llvm::Value* MethodCompiler::EmitLoadArrayLength(llvm::Value* array) {
  // Load array length field address
  llvm::Constant* array_len_field_offset =
    irb_.getPtrEquivInt(Array::LengthOffset().Int32Value());

  llvm::Value* array_len_field_addr =
    irb_.CreatePtrDisp(array, array_len_field_offset,
                       irb_.getJIntTy()->getPointerTo());

  // Load array length
  return irb_.CreateLoad(array_len_field_addr);
}


void MethodCompiler::EmitInsn_ArrayLength(uint32_t dex_pc,
                                          Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  // Get the array object address
  llvm::Value* array_addr = EmitLoadDalvikReg(dec_insn.vB, kObject, kAccurate);
  EmitGuard_NullPointerException(dex_pc, array_addr);

  // Get the array length and store it to the register
  llvm::Value* array_len = EmitLoadArrayLength(array_addr);
  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, array_len);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_NewInstance(uint32_t dex_pc,
                                          Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Function* runtime_func;
  if (compiler_->CanAccessTypeWithoutChecks(method_idx_, dex_cache_,
                                            *dex_file_, dec_insn.vB)) {
    runtime_func = irb_.GetRuntime(AllocObject);
  } else {
    runtime_func = irb_.GetRuntime(AllocObjectWithAccessCheck);
  }

  llvm::Constant* type_index_value = irb_.getInt32(dec_insn.vB);

  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  EmitUpdateLineNumFromDexPC(dex_pc);

  llvm::Value* object_addr =
    irb_.CreateCall2(runtime_func, type_index_value, method_object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, object_addr);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


llvm::Value* MethodCompiler::EmitAllocNewArray(uint32_t dex_pc,
                                               int32_t length,
                                               uint32_t type_idx,
                                               bool is_filled_new_array) {
  llvm::Function* runtime_func;

  bool skip_access_check =
    compiler_->CanAccessTypeWithoutChecks(method_idx_, dex_cache_,
                                          *dex_file_, type_idx);

  if (is_filled_new_array) {
    runtime_func = skip_access_check ?
      irb_.GetRuntime(CheckAndAllocArray) :
      irb_.GetRuntime(CheckAndAllocArrayWithAccessCheck);
  } else {
    runtime_func = skip_access_check ?
      irb_.GetRuntime(AllocArray) :
      irb_.GetRuntime(AllocArrayWithAccessCheck);
  }

  llvm::Constant* type_index_value = irb_.getInt32(type_idx);

  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  llvm::Value* array_length_value = irb_.getInt32(length);

  EmitUpdateLineNumFromDexPC(dex_pc);

  llvm::Value* object_addr =
    irb_.CreateCall3(runtime_func, type_index_value, method_object_addr,
                     array_length_value);

  EmitGuard_ExceptionLandingPad(dex_pc);

  return object_addr;
}


void MethodCompiler::EmitInsn_NewArray(uint32_t dex_pc,
                                       Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* object_addr =
    EmitAllocNewArray(dex_pc, dec_insn.vB, dec_insn.vC, false);

  EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, object_addr);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FilledNewArray(uint32_t dex_pc,
                                             Instruction const* insn,
                                             bool is_range) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* object_addr =
    EmitAllocNewArray(dex_pc, dec_insn.vA, dec_insn.vB, true);

  if (dec_insn.vA > 0) {
    llvm::Value* object_addr_int =
      irb_.CreatePtrToInt(object_addr, irb_.getPtrEquivIntTy());

    // TODO: currently FilledNewArray doesn't support I, J, D and L, [ so computing the component
    // size using int alignment is safe. This code should determine the width of the FilledNewArray
    // component.
    llvm::Value* data_field_offset =
      irb_.getPtrEquivInt(Array::DataOffset(sizeof(int32_t)).Int32Value());

    llvm::Value* data_field_addr_int =
      irb_.CreateAdd(object_addr_int, data_field_offset);

    Class* klass = method_->GetDexCacheResolvedTypes()->Get(dec_insn.vB);
    CHECK_NE(klass, static_cast<Class*>(NULL));
    // Moved this below already: CHECK(!klass->IsPrimitive() || klass->IsPrimitiveInt());

    llvm::Constant* word_size = irb_.getSizeOfPtrEquivIntValue();

    llvm::Type* field_type;
    if (klass->IsPrimitiveInt()) {
      field_type = irb_.getJIntTy()->getPointerTo();
    } else {
      CHECK(!klass->IsPrimitive());
      field_type = irb_.getJObjectTy()->getPointerTo();
    }

    // TODO: Tune this code.  Currently we are generating one instruction for
    // one element which may be very space consuming.  Maybe changing to use
    // memcpy may help; however, since we can't guarantee that the alloca of
    // dalvik register are continuous, we can't perform such optimization yet.
    for (uint32_t i = 0; i < dec_insn.vA; ++i) {
      llvm::Value* data_field_addr =
        irb_.CreateIntToPtr(data_field_addr_int, field_type);

      int reg_index;
      if (is_range) {
        reg_index = dec_insn.vC + i;
      } else {
        reg_index = dec_insn.arg[i];
      }

      llvm::Value* reg_value;
      if (klass->IsPrimitiveInt()) {
        reg_value = EmitLoadDalvikReg(reg_index, kInt, kAccurate);
      } else {
        reg_value = EmitLoadDalvikReg(reg_index, kObject, kAccurate);
      }

      irb_.CreateStore(reg_value, data_field_addr);

      data_field_addr_int = irb_.CreateAdd(data_field_addr_int, word_size);
    }
  }

  EmitStoreDalvikRetValReg(kObject, kAccurate, object_addr);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FillArrayData(uint32_t dex_pc,
                                            Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  // Read the payload
  struct PACKED Payload {
    uint16_t ident_;
    uint16_t elem_width_;
    uint32_t num_elems_;
    uint8_t data_[];
  };

  int32_t payload_offset = static_cast<int32_t>(dex_pc) +
                           static_cast<int32_t>(dec_insn.vB);

  Payload const* payload =
    reinterpret_cast<Payload const*>(code_item_->insns_ + payload_offset);

  uint32_t size_in_bytes = payload->elem_width_ * payload->num_elems_;

  // Load and check the array
  llvm::Value* array_addr = EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);

  EmitGuard_NullPointerException(dex_pc, array_addr);

  if (payload->num_elems_ > 0) {
    // Test: Is array length big enough?
    llvm::Constant* last_index = irb_.getJInt(payload->num_elems_ - 1);

    EmitGuard_ArrayIndexOutOfBoundsException(dex_pc, array_addr, last_index);

    // TODO: currently FillArray doesn't support I, J, D and L, [ so computing the component
    // size using int alignment is safe. This code should determine the width of the FillArray
    // component.
    // Get array data field
    llvm::Value* data_field_offset_value =
      irb_.getPtrEquivInt(Array::DataOffset(sizeof(int32_t)).Int32Value());

    llvm::Value* data_field_addr =
      irb_.CreatePtrDisp(array_addr, data_field_offset_value,
                         irb_.getInt8Ty()->getPointerTo());

    // Emit payload to bitcode constant pool
    std::vector<llvm::Constant*> const_pool_data;
    for (uint32_t i = 0; i < size_in_bytes; ++i) {
      const_pool_data.push_back(irb_.getInt8(payload->data_[i]));
    }

    llvm::Constant* const_pool_data_array_value = llvm::ConstantArray::get(
      llvm::ArrayType::get(irb_.getInt8Ty(), size_in_bytes), const_pool_data);

    llvm::Value* const_pool_data_array_addr =
      new llvm::GlobalVariable(*module_,
                               const_pool_data_array_value->getType(),
                               false, llvm::GlobalVariable::InternalLinkage,
                               const_pool_data_array_value,
                               "array_data_payload");

    // Find the memcpy intrinsic
    llvm::Type* memcpy_arg_types[] = {
      llvm::Type::getInt8Ty(*context_)->getPointerTo(),
      llvm::Type::getInt8Ty(*context_)->getPointerTo(),
      llvm::Type::getInt32Ty(*context_)
    };

    llvm::Function* memcpy_intrinsic =
      llvm::Intrinsic::getDeclaration(module_,
                                      llvm::Intrinsic::memcpy,
                                      memcpy_arg_types);

    // Copy now!
    llvm::Value *args[] = {
      data_field_addr,
      irb_.CreateConstGEP2_32(const_pool_data_array_addr, 0, 0),
      irb_.getInt32(size_in_bytes),
      irb_.getInt32(0), // alignment: no guarantee
      irb_.getFalse() // is_volatile: false
    };

    irb_.CreateCall(memcpy_intrinsic, args);
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_UnconditionalBranch(uint32_t dex_pc,
                                                  Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  int32_t branch_offset = dec_insn.vA;

  if (branch_offset <= 0) {
    // Garbage collection safe-point on backward branch
    EmitGuard_GarbageCollectionSuspend(dex_pc);
  }

  irb_.CreateBr(GetBasicBlock(dex_pc + branch_offset));
}


void MethodCompiler::EmitInsn_PackedSwitch(uint32_t dex_pc,
                                           Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  struct PACKED Payload {
    uint16_t ident_;
    uint16_t num_cases_;
    int32_t first_key_;
    int32_t targets_[];
  };

  int32_t payload_offset = static_cast<int32_t>(dex_pc) +
                           static_cast<int32_t>(dec_insn.vB);

  Payload const* payload =
    reinterpret_cast<Payload const*>(code_item_->insns_ + payload_offset);

  llvm::Value* value = EmitLoadDalvikReg(dec_insn.vA, kInt, kAccurate);

  llvm::SwitchInst* sw =
    irb_.CreateSwitch(value, GetNextBasicBlock(dex_pc), payload->num_cases_);

  for (uint16_t i = 0; i < payload->num_cases_; ++i) {
    sw->addCase(irb_.getInt32(payload->first_key_ + i),
                GetBasicBlock(dex_pc + payload->targets_[i]));
  }
}


void MethodCompiler::EmitInsn_SparseSwitch(uint32_t dex_pc,
                                           Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  struct PACKED Payload {
    uint16_t ident_;
    uint16_t num_cases_;
    int32_t keys_and_targets_[];
  };

  int32_t payload_offset = static_cast<int32_t>(dex_pc) +
                           static_cast<int32_t>(dec_insn.vB);

  Payload const* payload =
    reinterpret_cast<Payload const*>(code_item_->insns_ + payload_offset);

  int32_t const* keys = payload->keys_and_targets_;
  int32_t const* targets = payload->keys_and_targets_ + payload->num_cases_;

  llvm::Value* value = EmitLoadDalvikReg(dec_insn.vA, kInt, kAccurate);

  llvm::SwitchInst* sw =
    irb_.CreateSwitch(value, GetNextBasicBlock(dex_pc), payload->num_cases_);

  for (size_t i = 0; i < payload->num_cases_; ++i) {
    sw->addCase(irb_.getInt32(keys[i]), GetBasicBlock(dex_pc + targets[i]));
  }
}


void MethodCompiler::EmitInsn_FPCompare(uint32_t dex_pc,
                                        Instruction const* insn,
                                        JType fp_jty,
                                        bool gt_bias) {

  DecodedInstruction dec_insn(insn);

  DCHECK(fp_jty == kFloat || fp_jty == kDouble) << "JType: " << fp_jty;

  llvm::Value* src1_value = EmitLoadDalvikReg(dec_insn.vB, fp_jty, kAccurate);
  llvm::Value* src2_value = EmitLoadDalvikReg(dec_insn.vC, fp_jty, kAccurate);

  llvm::Value* cmp_eq = irb_.CreateFCmpOEQ(src1_value, src2_value);
  llvm::Value* cmp_lt;

  if (gt_bias) {
    cmp_lt = irb_.CreateFCmpOLT(src1_value, src2_value);
  } else {
    cmp_lt = irb_.CreateFCmpULT(src1_value, src2_value);
  }

  llvm::Value* result = EmitCompareResultSelection(cmp_eq, cmp_lt);
  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, result);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_LongCompare(uint32_t dex_pc,
                                          Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src1_value = EmitLoadDalvikReg(dec_insn.vB, kLong, kAccurate);
  llvm::Value* src2_value = EmitLoadDalvikReg(dec_insn.vC, kLong, kAccurate);

  llvm::Value* cmp_eq = irb_.CreateICmpEQ(src1_value, src2_value);
  llvm::Value* cmp_lt = irb_.CreateICmpSLT(src1_value, src2_value);

  llvm::Value* result = EmitCompareResultSelection(cmp_eq, cmp_lt);
  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, result);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


llvm::Value* MethodCompiler::EmitCompareResultSelection(llvm::Value* cmp_eq,
                                                        llvm::Value* cmp_lt) {

  llvm::Constant* zero = irb_.getJInt(0);
  llvm::Constant* pos1 = irb_.getJInt(1);
  llvm::Constant* neg1 = irb_.getJInt(-1);

  llvm::Value* result_lt = irb_.CreateSelect(cmp_lt, neg1, pos1);
  llvm::Value* result_eq = irb_.CreateSelect(cmp_eq, zero, result_lt);

  return result_eq;
}


void MethodCompiler::EmitInsn_BinaryConditionalBranch(uint32_t dex_pc,
                                                      Instruction const* insn,
                                                      CondBranchKind cond) {

  DecodedInstruction dec_insn(insn);

  int8_t src1_reg_cat = GetInferredRegCategory(dex_pc, dec_insn.vA);
  int8_t src2_reg_cat = GetInferredRegCategory(dex_pc, dec_insn.vB);

  DCHECK_NE(kRegUnknown, src1_reg_cat);
  DCHECK_NE(kRegUnknown, src2_reg_cat);
  DCHECK_NE(kRegCat2, src1_reg_cat);
  DCHECK_NE(kRegCat2, src2_reg_cat);

  int32_t branch_offset = dec_insn.vC;

  if (branch_offset <= 0) {
    // Garbage collection safe-point on backward branch
    EmitGuard_GarbageCollectionSuspend(dex_pc);
  }

  if (src1_reg_cat == kRegZero && src2_reg_cat == kRegZero) {
    irb_.CreateBr(GetBasicBlock(dex_pc + branch_offset));
    return;
  }

  llvm::Value* src1_value;
  llvm::Value* src2_value;

  if (src1_reg_cat != kRegZero && src2_reg_cat != kRegZero) {
    CHECK_EQ(src1_reg_cat, src2_reg_cat);

    if (src1_reg_cat == kRegCat1nr) {
      src1_value = EmitLoadDalvikReg(dec_insn.vA, kInt, kAccurate);
      src2_value = EmitLoadDalvikReg(dec_insn.vB, kInt, kAccurate);
    } else {
      src1_value = EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);
      src2_value = EmitLoadDalvikReg(dec_insn.vB, kObject, kAccurate);
    }
  } else {
    DCHECK(src1_reg_cat == kRegZero ||
           src2_reg_cat == kRegZero);

    if (src1_reg_cat == kRegZero) {
      if (src2_reg_cat == kRegCat1nr) {
        src1_value = irb_.getJInt(0);
        src2_value = EmitLoadDalvikReg(dec_insn.vA, kInt, kAccurate);
      } else {
        src1_value = irb_.getJNull();
        src2_value = EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);
      }
    } else { // src2_reg_cat == kRegZero
      if (src2_reg_cat == kRegCat1nr) {
        src1_value = EmitLoadDalvikReg(dec_insn.vA, kInt, kAccurate);
        src2_value = irb_.getJInt(0);
      } else {
        src1_value = EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);
        src2_value = irb_.getJNull();
      }
    }
  }

  llvm::Value* cond_value =
    EmitConditionResult(src1_value, src2_value, cond);

  irb_.CreateCondBr(cond_value,
                    GetBasicBlock(dex_pc + branch_offset),
                    GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_UnaryConditionalBranch(uint32_t dex_pc,
                                                     Instruction const* insn,
                                                     CondBranchKind cond) {

  DecodedInstruction dec_insn(insn);

  int8_t src_reg_cat = GetInferredRegCategory(dex_pc, dec_insn.vA);

  DCHECK_NE(kRegUnknown, src_reg_cat);
  DCHECK_NE(kRegCat2, src_reg_cat);

  int32_t branch_offset = dec_insn.vB;

  if (branch_offset <= 0) {
    // Garbage collection safe-point on backward branch
    EmitGuard_GarbageCollectionSuspend(dex_pc);
  }

  if (src_reg_cat == kRegZero) {
    irb_.CreateBr(GetBasicBlock(dex_pc + branch_offset));
    return;
  }

  llvm::Value* src1_value;
  llvm::Value* src2_value;

  if (src_reg_cat == kRegCat1nr) {
    src1_value = EmitLoadDalvikReg(dec_insn.vA, kInt, kAccurate);
    src2_value = irb_.getInt32(0);
  } else {
    src1_value = EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);
    src2_value = irb_.getJNull();
  }

  llvm::Value* cond_value =
    EmitConditionResult(src1_value, src2_value, cond);

  irb_.CreateCondBr(cond_value,
                    GetBasicBlock(dex_pc + branch_offset),
                    GetNextBasicBlock(dex_pc));
}


RegCategory MethodCompiler::GetInferredRegCategory(uint32_t dex_pc,
                                                   uint16_t reg_idx) {
  InferredRegCategoryMap const* map = method_->GetInferredRegCategoryMap();
  CHECK_NE(map, static_cast<InferredRegCategoryMap*>(NULL));

  return map->GetRegCategory(dex_pc, reg_idx);
}


llvm::Value* MethodCompiler::EmitConditionResult(llvm::Value* lhs,
                                                 llvm::Value* rhs,
                                                 CondBranchKind cond) {
  switch (cond) {
  case kCondBranch_EQ:
    return irb_.CreateICmpEQ(lhs, rhs);

  case kCondBranch_NE:
    return irb_.CreateICmpNE(lhs, rhs);

  case kCondBranch_LT:
    return irb_.CreateICmpSLT(lhs, rhs);

  case kCondBranch_GE:
    return irb_.CreateICmpSGE(lhs, rhs);

  case kCondBranch_GT:
    return irb_.CreateICmpSGT(lhs, rhs);

  case kCondBranch_LE:
    return irb_.CreateICmpSLE(lhs, rhs);

  default: // Unreachable
    LOG(FATAL) << "Unknown conditional branch kind: " << cond;
    return NULL;
  }
}


void
MethodCompiler::EmitGuard_ArrayIndexOutOfBoundsException(uint32_t dex_pc,
                                                         llvm::Value* array,
                                                         llvm::Value* index) {
  llvm::Value* array_len = EmitLoadArrayLength(array);

  llvm::Value* cmp = irb_.CreateICmpUGE(index, array_len);

  llvm::BasicBlock* block_exception =
    CreateBasicBlockWithDexPC(dex_pc, "overflow");

  llvm::BasicBlock* block_continue =
    CreateBasicBlockWithDexPC(dex_pc, "cont");

  irb_.CreateCondBr(cmp, block_exception, block_continue);

  irb_.SetInsertPoint(block_exception);

  EmitUpdateLineNumFromDexPC(dex_pc);
  irb_.CreateCall2(irb_.GetRuntime(ThrowIndexOutOfBounds), index, array_len);
  EmitBranchExceptionLandingPad(dex_pc);

  irb_.SetInsertPoint(block_continue);
}


void MethodCompiler::EmitGuard_ArrayException(uint32_t dex_pc,
                                              llvm::Value* array,
                                              llvm::Value* index) {
  EmitGuard_NullPointerException(dex_pc, array);
  EmitGuard_ArrayIndexOutOfBoundsException(dex_pc, array, index);
}


// Emit Array GetElementPtr
llvm::Value* MethodCompiler::EmitArrayGEP(llvm::Value* array_addr,
                                          llvm::Value* index_value,
                                          llvm::Type* elem_type,
                                          JType elem_jty) {

  int data_offset;
  if (elem_jty == kLong || elem_jty == kDouble ||
      (elem_jty == kObject && sizeof(uint64_t) == sizeof(Object*))) {
    data_offset = Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  llvm::Constant* data_offset_value =
    irb_.getPtrEquivInt(data_offset);

  llvm::Value* array_data_addr =
    irb_.CreatePtrDisp(array_addr, data_offset_value,
                       elem_type->getPointerTo());

  return irb_.CreateGEP(array_data_addr, index_value);
}


void MethodCompiler::EmitInsn_AGet(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType elem_jty) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* array_addr = EmitLoadDalvikReg(dec_insn.vB, kObject, kAccurate);
  llvm::Value* index_value = EmitLoadDalvikReg(dec_insn.vC, kInt, kAccurate);

  EmitGuard_ArrayException(dex_pc, array_addr, index_value);

  llvm::Type* elem_type = irb_.getJType(elem_jty, kArray);

  llvm::Value* array_elem_addr =
    EmitArrayGEP(array_addr, index_value, elem_type, elem_jty);

  llvm::Value* array_elem_value = irb_.CreateLoad(array_elem_addr);

  EmitStoreDalvikReg(dec_insn.vA, elem_jty, kArray, array_elem_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_APut(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType elem_jty) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* array_addr = EmitLoadDalvikReg(dec_insn.vB, kObject, kAccurate);
  llvm::Value* index_value = EmitLoadDalvikReg(dec_insn.vC, kInt, kAccurate);

  EmitGuard_ArrayException(dex_pc, array_addr, index_value);

  llvm::Type* elem_type = irb_.getJType(elem_jty, kArray);

  llvm::Value* array_elem_addr =
    EmitArrayGEP(array_addr, index_value, elem_type, elem_jty);

  llvm::Value* new_value = EmitLoadDalvikReg(dec_insn.vA, elem_jty, kArray);

  irb_.CreateStore(new_value, array_elem_addr);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::PrintUnresolvedFieldWarning(int32_t field_idx) {
  DexFile const& dex_file = method_helper_.GetDexFile();
  DexFile::FieldId const& field_id = dex_file.GetFieldId(field_idx);

  LOG(WARNING) << "unable to resolve static field " << field_idx << " ("
               << dex_file.GetFieldName(field_id) << ") in "
               << dex_file.GetFieldDeclaringClassDescriptor(field_id);
}


void MethodCompiler::EmitInsn_IGet(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType field_jty) {

  DecodedInstruction dec_insn(insn);

  uint32_t reg_idx = dec_insn.vB;
  uint32_t field_idx = dec_insn.vC;

  Field* field = dex_cache_->GetResolvedField(field_idx);

  llvm::Value* object_addr = EmitLoadDalvikReg(reg_idx, kObject, kAccurate);

  EmitGuard_NullPointerException(dex_pc, object_addr);

  llvm::Value* field_value;

  if (field == NULL) {
    PrintUnresolvedFieldWarning(field_idx);

    llvm::Function* runtime_func;

    if (field_jty == kObject) {
      runtime_func = irb_.GetRuntime(GetObjectInstance);
    } else if (field_jty == kLong || field_jty == kDouble) {
      runtime_func = irb_.GetRuntime(Get64Instance);
    } else {
      runtime_func = irb_.GetRuntime(Get32Instance);
    }

    llvm::ConstantInt* field_idx_value = irb_.getInt32(field_idx);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitUpdateLineNumFromDexPC(dex_pc);

    field_value = irb_.CreateCall3(runtime_func, field_idx_value,
                                   method_object_addr, object_addr);

    EmitGuard_ExceptionLandingPad(dex_pc);

  } else {
    llvm::PointerType* field_type =
      irb_.getJType(field_jty, kField)->getPointerTo();

    llvm::ConstantInt* field_offset =
      irb_.getPtrEquivInt(field->GetOffset().Int32Value());

    llvm::Value* field_addr =
      irb_.CreatePtrDisp(object_addr, field_offset, field_type);

    field_value = irb_.CreateLoad(field_addr);
  }

  EmitStoreDalvikReg(dec_insn.vA, field_jty, kField, field_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_IPut(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType field_jty) {

  DecodedInstruction dec_insn(insn);

  uint32_t reg_idx = dec_insn.vB;
  uint32_t field_idx = dec_insn.vC;

  Field* field = dex_cache_->GetResolvedField(field_idx);

  llvm::Value* object_addr = EmitLoadDalvikReg(reg_idx, kObject, kAccurate);

  EmitGuard_NullPointerException(dex_pc, object_addr);

  llvm::Value* new_value = EmitLoadDalvikReg(dec_insn.vA, field_jty, kField);

  if (field == NULL) {
    PrintUnresolvedFieldWarning(field_idx);

    llvm::Function* runtime_func;

    if (field_jty == kObject) {
      runtime_func = irb_.GetRuntime(SetObjectInstance);
    } else if (field_jty == kLong || field_jty == kDouble) {
      runtime_func = irb_.GetRuntime(Set64Instance);
    } else {
      runtime_func = irb_.GetRuntime(Set32Instance);
    }

    llvm::Value* field_idx_value = irb_.getInt32(field_idx);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitUpdateLineNumFromDexPC(dex_pc);

    irb_.CreateCall4(runtime_func, field_idx_value,
                     method_object_addr, object_addr, new_value);

    EmitGuard_ExceptionLandingPad(dex_pc);

  } else {
    llvm::PointerType* field_type =
      irb_.getJType(field_jty, kField)->getPointerTo();

    llvm::Value* field_offset =
      irb_.getPtrEquivInt(field->GetOffset().Int32Value());

    llvm::Value* field_addr =
      irb_.CreatePtrDisp(object_addr, field_offset, field_type);

    irb_.CreateStore(new_value, field_addr);
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


Method* MethodCompiler::ResolveMethod(uint32_t method_idx) {
  Thread* thread = Thread::Current();

  // Save the exception state
  Throwable* old_exception = NULL;
  if (thread->IsExceptionPending()) {
    old_exception = thread->GetException();
    thread->ClearException();
  }

  // Resolve the method through the class linker
  Method* method = class_linker_->ResolveMethod(*dex_file_, method_idx,
                                                dex_cache_, class_loader_,
                                                /* is_direct= */ false);

  if (method == NULL) {
    // Ignore the exception raised during the method resolution
    thread->ClearException();
  }

  // Restore the exception state
  if (old_exception != NULL) {
    thread->SetException(old_exception);
  }

  return method;
}


Field* MethodCompiler::ResolveField(uint32_t field_idx) {
  Thread* thread = Thread::Current();

  // Save the exception state
  Throwable* old_exception = NULL;
  if (thread->IsExceptionPending()) {
    old_exception = thread->GetException();
    thread->ClearException();
  }

  // Resolve the fields through the class linker
  Field* field = class_linker_->ResolveField(*dex_file_, field_idx,
                                             dex_cache_, class_loader_,
                                             /* is_static= */ true);

  if (field == NULL) {
    // Ignore the exception raised during the field resolution
    thread->ClearException();
  }

  // Restore the exception state
  if (old_exception != NULL) {
    thread->SetException(old_exception);
  }

  return field;
}


Field* MethodCompiler::
FindFieldAndDeclaringTypeIdx(uint32_t field_idx,
                             uint32_t &resolved_type_idx) {

  // Resolve the field through the class linker
  Field* field = ResolveField(field_idx);
  if (field == NULL) {
    return NULL;
  }

  DexFile::FieldId const& field_id = dex_file_->GetFieldId(field_idx);

  // Search for the type_idx of the declaring class
  uint16_t type_idx = field_id.class_idx_;
  Class* klass = dex_cache_->GetResolvedTypes()->Get(type_idx);

  // Test: Is referring_class equal to declaring_class?

  // If true, then return the type_idx; otherwise, this field must be declared
  // in super class or interfaces, we have to search for declaring class.

  if (field->GetDeclaringClass() == klass) {
    resolved_type_idx = type_idx;
    return field;
  }

  // Search for the type_idx of the super class or interfaces
  std::string desc(FieldHelper(field).GetDeclaringClassDescriptor());

  DexFile::StringId const* string_id = dex_file_->FindStringId(desc);

  if (string_id == NULL) {
    return NULL;
  }

  DexFile::TypeId const* type_id =
    dex_file_->FindTypeId(dex_file_->GetIndexForStringId(*string_id));

  if (type_id == NULL) {
    return NULL;
  }

  resolved_type_idx = dex_file_->GetIndexForTypeId(*type_id);
  return field;
}


llvm::Value* MethodCompiler::EmitLoadStaticStorage(uint32_t dex_pc,
                                                   uint32_t type_idx) {
  llvm::BasicBlock* block_load_static =
    CreateBasicBlockWithDexPC(dex_pc, "load_static");

  llvm::BasicBlock* block_cont = CreateBasicBlockWithDexPC(dex_pc, "cont");

  // Load static storage from dex cache
  llvm::Value* storage_field_addr =
    EmitLoadDexCacheStaticStorageFieldAddr(type_idx);

  llvm::Value* storage_object_addr = irb_.CreateLoad(storage_field_addr);

  llvm::BasicBlock* block_original = irb_.GetInsertBlock();

  // Test: Is the static storage of this class initialized?
  llvm::Value* equal_null =
    irb_.CreateICmpEQ(storage_object_addr, irb_.getJNull());

  irb_.CreateCondBr(equal_null, block_load_static, block_cont);

  // Failback routine to load the class object
  irb_.SetInsertPoint(block_load_static);

  llvm::Function* runtime_func =
    irb_.GetRuntime(InitializeStaticStorage);

  llvm::Constant* type_idx_value = irb_.getInt32(type_idx);

  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  EmitUpdateLineNumFromDexPC(dex_pc);

  llvm::Value* loaded_storage_object_addr =
    irb_.CreateCall2(runtime_func, type_idx_value, method_object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  llvm::BasicBlock* block_after_load_static = irb_.GetInsertBlock();

  irb_.CreateBr(block_cont);

  // Now the class object must be loaded
  irb_.SetInsertPoint(block_cont);

  llvm::PHINode* phi = irb_.CreatePHI(irb_.getJObjectTy(), 2);

  phi->addIncoming(storage_object_addr, block_original);
  phi->addIncoming(loaded_storage_object_addr, block_after_load_static);

  return phi;
}


void MethodCompiler::EmitInsn_SGet(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType field_jty) {

  DecodedInstruction dec_insn(insn);

  uint32_t declaring_type_idx = DexFile::kDexNoIndex;

  Field* field = FindFieldAndDeclaringTypeIdx(dec_insn.vB, declaring_type_idx);

  llvm::Value* static_field_value;

  if (field == NULL) {
    llvm::Function* runtime_func;

    if (field_jty == kObject) {
      runtime_func = irb_.GetRuntime(GetObjectStatic);
    } else if (field_jty == kLong || field_jty == kDouble) {
      runtime_func = irb_.GetRuntime(Get64Static);
    } else {
      runtime_func = irb_.GetRuntime(Get32Static);
    }

    llvm::Constant* field_idx_value = irb_.getInt32(dec_insn.vB);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitUpdateLineNumFromDexPC(dex_pc);

    static_field_value =
      irb_.CreateCall2(runtime_func, field_idx_value, method_object_addr);

    EmitGuard_ExceptionLandingPad(dex_pc);

  } else {
    llvm::Value* static_storage_addr =
      EmitLoadStaticStorage(dex_pc, declaring_type_idx);

    llvm::Value* static_field_offset_value =
      irb_.getPtrEquivInt(field->GetOffset().Int32Value());

    llvm::Value* static_field_addr =
      irb_.CreatePtrDisp(static_storage_addr, static_field_offset_value,
                         irb_.getJType(field_jty, kField)->getPointerTo());

    static_field_value = irb_.CreateLoad(static_field_addr);
  }

  EmitStoreDalvikReg(dec_insn.vA, field_jty, kField, static_field_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_SPut(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType field_jty) {

  DecodedInstruction dec_insn(insn);

  uint32_t declaring_type_idx = DexFile::kDexNoIndex;

  Field* field = FindFieldAndDeclaringTypeIdx(dec_insn.vB, declaring_type_idx);

  llvm::Value* new_value = EmitLoadDalvikReg(dec_insn.vA, field_jty, kField);

  if (field == NULL) {
    llvm::Function* runtime_func;

    if (field_jty == kObject) {
      runtime_func = irb_.GetRuntime(SetObjectStatic);
    } else if (field_jty == kLong || field_jty == kDouble) {
      runtime_func = irb_.GetRuntime(Set64Static);
    } else {
      runtime_func = irb_.GetRuntime(Set32Static);
    }

    llvm::Constant* field_idx_value = irb_.getInt32(dec_insn.vB);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitUpdateLineNumFromDexPC(dex_pc);

    irb_.CreateCall3(runtime_func, field_idx_value,
                     method_object_addr, new_value);

    EmitGuard_ExceptionLandingPad(dex_pc);

  } else {
    llvm::Value* static_storage_addr =
      EmitLoadStaticStorage(dex_pc, declaring_type_idx);

    llvm::Value* static_field_offset_value =
      irb_.getPtrEquivInt(field->GetOffset().Int32Value());

    llvm::Value* static_field_addr =
      irb_.CreatePtrDisp(static_storage_addr, static_field_offset_value,
                         irb_.getJType(field_jty, kField)->getPointerTo());

    irb_.CreateStore(new_value, static_field_addr);
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


llvm::Value* MethodCompiler::EmitLoadCalleeThis(DecodedInstruction const& dec_insn, bool is_range) {
  if (is_range) {
    return EmitLoadDalvikReg(dec_insn.vC, kObject, kAccurate);
  } else {
    return EmitLoadDalvikReg(dec_insn.arg[0], kObject, kAccurate);
  }
}


void MethodCompiler::
EmitLoadActualParameters(std::vector<llvm::Value*>& args,
                         uint32_t callee_method_idx,
                         DecodedInstruction const& dec_insn,
                         bool is_range,
                         bool is_static) {

  // Get method signature
  DexFile::MethodId const& method_id =
    dex_file_->GetMethodId(callee_method_idx);

  uint32_t shorty_size;
  char const* shorty = dex_file_->GetMethodShorty(method_id, &shorty_size);
  CHECK_GE(shorty_size, 1u);

  // Load argument values according to the shorty (without "this")
  uint16_t reg_count = 0;

  if (!is_static) {
    ++reg_count; // skip the "this" pointer
  }

  for (uint32_t i = 1; i < shorty_size; ++i) {
    uint32_t reg_idx = (is_range) ? (dec_insn.vC + reg_count)
                                  : (dec_insn.arg[reg_count]);

    args.push_back(EmitLoadDalvikReg(reg_idx, shorty[i], kAccurate));

    ++reg_count;
    if (shorty[i] == 'J' || shorty[i] == 'D') {
      // Wide types, such as long and double, are using a pair of registers
      // to store the value, so we have to increase arg_reg again.
      ++reg_count;
    }
  }

  DCHECK_EQ(reg_count, dec_insn.vA)
    << "Actual argument mismatch for callee: "
    << PrettyMethod(callee_method_idx, *dex_file_);
}


void MethodCompiler::EmitInsn_InvokeVirtualSuperSlow(uint32_t dex_pc,
                                                     DecodedInstruction &dec_insn,
                                                     bool is_range,
                                                     uint32_t callee_method_idx,
                                                     bool is_virtual) {
  // Callee method can't be resolved at compile time.  Emit the runtime
  // method resolution code.

  // Test: Is "this" pointer equal to null?
  llvm::Value* this_addr = EmitLoadCalleeThis(dec_insn, is_range);
  EmitGuard_NullPointerException(dex_pc, this_addr);

  // Resolve the callee method object address at runtime
  llvm::Value* runtime_func;
  if (is_virtual) {
    runtime_func = irb_.GetRuntime(FindVirtualMethod);
  } else {
    runtime_func = irb_.GetRuntime(FindSuperMethod);
  }

  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  llvm::Value* callee_method_idx_value = irb_.getInt32(callee_method_idx);

  EmitUpdateLineNumFromDexPC(dex_pc);

  llvm::Value* callee_method_object_addr =
      irb_.CreateCall3(runtime_func,
                       callee_method_idx_value, this_addr, method_object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  llvm::Value* code_addr =
      EmitLoadCodeAddr(callee_method_object_addr, callee_method_idx, false);
  // Load the actual parameter
  std::vector<llvm::Value*> args;
  args.push_back(callee_method_object_addr);
  args.push_back(this_addr);
  EmitLoadActualParameters(args, callee_method_idx, dec_insn, is_range, false);

  // Invoke callee
  EmitUpdateLineNumFromDexPC(dex_pc);
  llvm::Value* retval = irb_.CreateCall(code_addr, args);
  EmitGuard_ExceptionLandingPad(dex_pc);

  UniquePtr<OatCompilationUnit>
      callee(oat_compilation_unit_->GetCallee(callee_method_idx, /* access flags */ 0));
  char ret_shorty = callee->GetShorty()[0];
  if (ret_shorty != 'V') {
    EmitStoreDalvikRetValReg(ret_shorty, kAccurate, retval);
  }
}

void MethodCompiler::EmitInsn_InvokeVirtual(uint32_t dex_pc,
                                            Instruction const* insn,
                                            bool is_range) {

  DecodedInstruction dec_insn(insn);

  uint32_t callee_method_idx = dec_insn.vB;

  // Find the method object at compile time
  Method* callee_method = ResolveMethod(callee_method_idx);

  if (callee_method == NULL) {
    EmitInsn_InvokeVirtualSuperSlow(dex_pc, dec_insn, is_range,
                                    callee_method_idx, /*is_virtual*/ true);
  } else {
    // Test: Is *this* parameter equal to null?
    llvm::Value* this_addr = EmitLoadCalleeThis(dec_insn, is_range);
    EmitGuard_NullPointerException(dex_pc, this_addr);

    // Load class object of *this* pointer
    llvm::Value* class_object_addr = EmitLoadClassObjectAddr(this_addr);

    // Load callee method code address (branch destination)
    llvm::Value* vtable_addr = EmitLoadVTableAddr(class_object_addr);

    llvm::Value* method_object_addr =
      EmitLoadMethodObjectAddrFromVTable(vtable_addr,
                                         callee_method->GetMethodIndex());

    llvm::Value* code_addr =
      EmitLoadCodeAddr(method_object_addr, callee_method_idx, false);

    // Load actual parameters
    std::vector<llvm::Value*> args;
    args.push_back(method_object_addr);
    args.push_back(this_addr);
    EmitLoadActualParameters(args, callee_method_idx, dec_insn,
                             is_range, false);

    // Invoke callee
    EmitUpdateLineNumFromDexPC(dex_pc);
    llvm::Value* retval = irb_.CreateCall(code_addr, args);
    EmitGuard_ExceptionLandingPad(dex_pc);

    MethodHelper method_helper(callee_method);
    char ret_shorty = method_helper.GetShorty()[0];
    if (ret_shorty != 'V') {
      EmitStoreDalvikRetValReg(ret_shorty, kAccurate, retval);
    }
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_InvokeSuper(uint32_t dex_pc,
                                          Instruction const* insn,
                                          bool is_range) {

  DecodedInstruction dec_insn(insn);

  uint32_t callee_method_idx = dec_insn.vB;

  // Find the method object at compile time
  Method* callee_overiding_method = ResolveMethod(callee_method_idx);

  if (callee_overiding_method == NULL) {
    EmitInsn_InvokeVirtualSuperSlow(dex_pc, dec_insn, is_range,
                                    callee_method_idx, /*is_virtual*/ false);
  } else {
    // CHECK: Is the instruction well-encoded or is the class hierarchy correct?
    Class* declaring_class = method_->GetDeclaringClass();
    CHECK_NE(declaring_class, static_cast<Class*>(NULL));

    Class* super_class = declaring_class->GetSuperClass();
    CHECK_NE(super_class, static_cast<Class*>(NULL));

    uint16_t callee_method_index = callee_overiding_method->GetMethodIndex();
    CHECK_GT(super_class->GetVTable()->GetLength(), callee_method_index);

    Method* callee_method = super_class->GetVTable()->Get(callee_method_index);
    CHECK_NE(callee_method, static_cast<Method*>(NULL));

    // Test: Is *this* parameter equal to null?
    llvm::Value* this_addr = EmitLoadCalleeThis(dec_insn, is_range);
    EmitGuard_NullPointerException(dex_pc, this_addr);

    // Load declaring class of the caller method
    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    llvm::ConstantInt* declaring_class_offset_value =
      irb_.getPtrEquivInt(Method::DeclaringClassOffset().Int32Value());

    llvm::Value* declaring_class_field_addr =
      irb_.CreatePtrDisp(method_object_addr, declaring_class_offset_value,
                         irb_.getJObjectTy()->getPointerTo());

    llvm::Value* declaring_class_addr =
      irb_.CreateLoad(declaring_class_field_addr);

    // Load the super class of the declaring class
    llvm::Value* super_class_offset_value =
      irb_.getPtrEquivInt(Class::SuperClassOffset().Int32Value());

    llvm::Value* super_class_field_addr =
      irb_.CreatePtrDisp(declaring_class_addr, super_class_offset_value,
                         irb_.getJObjectTy()->getPointerTo());

    llvm::Value* super_class_addr =
      irb_.CreateLoad(super_class_field_addr);

    // Load method object from virtual table
    llvm::Value* vtable_addr = EmitLoadVTableAddr(super_class_addr);

    llvm::Value* callee_method_object_addr =
      EmitLoadMethodObjectAddrFromVTable(vtable_addr,
                                         callee_method->GetMethodIndex());

    llvm::Value* code_addr =
      EmitLoadCodeAddr(callee_method_object_addr,
                       callee_method_idx, false);

    // Load actual parameters
    std::vector<llvm::Value*> args;
    args.push_back(callee_method_object_addr);
    args.push_back(this_addr);
    EmitLoadActualParameters(args, callee_method_idx, dec_insn,
                             is_range, false);

    // Invoke callee
    EmitUpdateLineNumFromDexPC(dex_pc);
    llvm::Value* retval = irb_.CreateCall(code_addr, args);
    EmitGuard_ExceptionLandingPad(dex_pc);

    MethodHelper method_helper(callee_method);
    char ret_shorty = method_helper.GetShorty()[0];
    if (ret_shorty != 'V') {
      EmitStoreDalvikRetValReg(ret_shorty, kAccurate, retval);
    }
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_InvokeStaticDirect(uint32_t dex_pc,
                                                 Instruction const* insn,
                                                 InvokeType invoke_type,
                                                 bool is_range) {

  DecodedInstruction dec_insn(insn);

  uint32_t callee_method_idx = dec_insn.vB;

  bool is_static = (invoke_type == kStatic);

  UniquePtr<OatCompilationUnit> callee_oatcompilation_unit(
      oat_compilation_unit_->GetCallee(callee_method_idx, is_static ? kAccStatic : 0));

  int vtable_idx = -1; // Currently unused
  bool is_fast_path = compiler_->
    ComputeInvokeInfo(callee_method_idx, callee_oatcompilation_unit.get(),
                      invoke_type, vtable_idx);

  CHECK(is_fast_path) << "Slow path for invoke static/direct is unimplemented";

  llvm::Value* this_addr = NULL;

  if (!is_static) {
    // Test: Is *this* parameter equal to null?
    this_addr = EmitLoadCalleeThis(dec_insn, is_range);
    EmitGuard_NullPointerException(dex_pc, this_addr);
  }

  // Load method function pointers
  llvm::Value* callee_method_object_field_addr =
    EmitLoadDexCacheResolvedMethodFieldAddr(callee_method_idx);

  llvm::Value* callee_method_object_addr =
    irb_.CreateLoad(callee_method_object_field_addr);

  llvm::Value* code_addr =
    EmitLoadCodeAddr(callee_method_object_addr, callee_method_idx, is_static);

  // Load the actual parameter
  std::vector<llvm::Value*> args;

  args.push_back(callee_method_object_addr); // method object for callee

  if (!is_static) {
    args.push_back(this_addr); // "this" object for callee
  }

  EmitLoadActualParameters(args, callee_method_idx, dec_insn,
                           is_range, is_static);

  // Invoke callee
  EmitUpdateLineNumFromDexPC(dex_pc);
  llvm::Value* retval = irb_.CreateCall(code_addr, args);
  EmitGuard_ExceptionLandingPad(dex_pc);

  char ret_shorty = callee_oatcompilation_unit->GetShorty()[0];
  if (ret_shorty != 'V') {
    EmitStoreDalvikRetValReg(ret_shorty, kAccurate, retval);
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_InvokeInterface(uint32_t dex_pc,
                                              Instruction const* insn,
                                              bool is_range) {

  DecodedInstruction dec_insn(insn);

  uint32_t callee_method_idx = dec_insn.vB;

  // Resolve callee method
  Method* callee_method = ResolveMethod(callee_method_idx);

  // Test: Is "this" pointer equal to null?
  llvm::Value* this_addr = EmitLoadCalleeThis(dec_insn, is_range);
  EmitGuard_NullPointerException(dex_pc, this_addr);

  // Resolve the callee method object address at runtime
  llvm::Value* runtime_func = irb_.GetRuntime(FindInterfaceMethod);

  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  llvm::Value* callee_method_idx_value = irb_.getInt32(callee_method_idx);

  EmitUpdateLineNumFromDexPC(dex_pc);

  llvm::Value* callee_method_object_addr =
    irb_.CreateCall3(runtime_func,
                     callee_method_idx_value, this_addr, method_object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  llvm::Value* code_addr =
    EmitLoadCodeAddr(callee_method_object_addr, callee_method_idx, false);

  // Load the actual parameter
  std::vector<llvm::Value*> args;
  args.push_back(callee_method_object_addr);
  args.push_back(this_addr);
  EmitLoadActualParameters(args, callee_method_idx, dec_insn, is_range, false);

  // Invoke callee
  EmitUpdateLineNumFromDexPC(dex_pc);
  llvm::Value* retval = irb_.CreateCall(code_addr, args);
  EmitGuard_ExceptionLandingPad(dex_pc);

  MethodHelper method_helper(callee_method);
  char ret_shorty = method_helper.GetShorty()[0];
  if (ret_shorty != 'V') {
    EmitStoreDalvikRetValReg(ret_shorty, kAccurate, retval);
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_Neg(uint32_t dex_pc,
                                  Instruction const* insn,
                                  JType op_jty) {

  DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
  llvm::Value* result_value = irb_.CreateNeg(src_value);
  EmitStoreDalvikReg(dec_insn.vA, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_Not(uint32_t dex_pc,
                                  Instruction const* insn,
                                  JType op_jty) {

  DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
  llvm::Value* result_value =
    irb_.CreateXor(src_value, static_cast<uint64_t>(-1));

  EmitStoreDalvikReg(dec_insn.vA, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_SExt(uint32_t dex_pc,
                                   Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, kInt, kAccurate);
  llvm::Value* result_value = irb_.CreateSExt(src_value, irb_.getJLongTy());
  EmitStoreDalvikReg(dec_insn.vA, kLong, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_Trunc(uint32_t dex_pc,
                                    Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, kLong, kAccurate);
  llvm::Value* result_value = irb_.CreateTrunc(src_value, irb_.getJIntTy());
  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_TruncAndSExt(uint32_t dex_pc,
                                           Instruction const* insn,
                                           unsigned N) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, kInt, kAccurate);

  llvm::Value* trunc_value =
    irb_.CreateTrunc(src_value, llvm::Type::getIntNTy(*context_, N));

  llvm::Value* result_value = irb_.CreateSExt(trunc_value, irb_.getJIntTy());

  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_TruncAndZExt(uint32_t dex_pc,
                                           Instruction const* insn,
                                           unsigned N) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, kInt, kAccurate);

  llvm::Value* trunc_value =
    irb_.CreateTrunc(src_value, llvm::Type::getIntNTy(*context_, N));

  llvm::Value* result_value = irb_.CreateZExt(trunc_value, irb_.getJIntTy());

  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FNeg(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType op_jty) {

  DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kFloat || op_jty == kDouble) << op_jty;

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
  llvm::Value* result_value = irb_.CreateFNeg(src_value);
  EmitStoreDalvikReg(dec_insn.vA, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_IntToFP(uint32_t dex_pc,
                                      Instruction const* insn,
                                      JType src_jty,
                                      JType dest_jty) {

  DecodedInstruction dec_insn(insn);

  DCHECK(src_jty == kInt || src_jty == kLong) << src_jty;
  DCHECK(dest_jty == kFloat || dest_jty == kDouble) << dest_jty;

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, src_jty, kAccurate);
  llvm::Type* dest_type = irb_.getJType(dest_jty, kAccurate);
  llvm::Value* dest_value = irb_.CreateSIToFP(src_value, dest_type);
  EmitStoreDalvikReg(dec_insn.vA, dest_jty, kAccurate, dest_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FPToInt(uint32_t dex_pc,
                                      Instruction const* insn,
                                      JType src_jty,
                                      JType dest_jty) {

  DecodedInstruction dec_insn(insn);

  DCHECK(src_jty == kFloat || src_jty == kDouble) << src_jty;
  DCHECK(dest_jty == kInt || dest_jty == kLong) << dest_jty;

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, src_jty, kAccurate);
  llvm::Type* dest_type = irb_.getJType(dest_jty, kAccurate);
  llvm::Value* dest_value = irb_.CreateFPToSI(src_value, dest_type);
  EmitStoreDalvikReg(dec_insn.vA, dest_jty, kAccurate, dest_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FExt(uint32_t dex_pc,
                                   Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, kFloat, kAccurate);
  llvm::Value* result_value = irb_.CreateFPExt(src_value, irb_.getJDoubleTy());
  EmitStoreDalvikReg(dec_insn.vA, kDouble, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FTrunc(uint32_t dex_pc,
                                     Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, kDouble, kAccurate);
  llvm::Value* result_value = irb_.CreateFPTrunc(src_value, irb_.getJFloatTy());
  EmitStoreDalvikReg(dec_insn.vA, kFloat, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_IntArithm(uint32_t dex_pc,
                                        Instruction const* insn,
                                        IntArithmKind arithm,
                                        JType op_jty,
                                        bool is_2addr) {

  DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  llvm::Value* src1_value;
  llvm::Value* src2_value;

  if (is_2addr) {
    src1_value = EmitLoadDalvikReg(dec_insn.vA, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
  } else {
    src1_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vC, op_jty, kAccurate);
  }

  llvm::Value* result_value =
    EmitIntArithmResultComputation(dex_pc, src1_value, src2_value,
                                   arithm, op_jty);

  EmitStoreDalvikReg(dec_insn.vA, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_IntArithmImmediate(uint32_t dex_pc,
                                                 Instruction const* insn,
                                                 IntArithmKind arithm) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, kInt, kAccurate);

  llvm::Value* imm_value = irb_.getInt32(dec_insn.vC);

  llvm::Value* result_value =
    EmitIntArithmResultComputation(dex_pc, src_value, imm_value, arithm, kInt);

  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


llvm::Value*
MethodCompiler::EmitIntArithmResultComputation(uint32_t dex_pc,
                                               llvm::Value* lhs,
                                               llvm::Value* rhs,
                                               IntArithmKind arithm,
                                               JType op_jty) {
  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  switch (arithm) {
  case kIntArithm_Add:
    return irb_.CreateAdd(lhs, rhs);

  case kIntArithm_Sub:
    return irb_.CreateSub(lhs, rhs);

  case kIntArithm_Mul:
    return irb_.CreateMul(lhs, rhs);

  case kIntArithm_Div:
    EmitGuard_DivZeroException(dex_pc, rhs, op_jty);
    return irb_.CreateSDiv(lhs, rhs);

  case kIntArithm_Rem:
    EmitGuard_DivZeroException(dex_pc, rhs, op_jty);
    return irb_.CreateSRem(lhs, rhs);

  case kIntArithm_And:
    return irb_.CreateAnd(lhs, rhs);

  case kIntArithm_Or:
    return irb_.CreateOr(lhs, rhs);

  case kIntArithm_Xor:
    return irb_.CreateXor(lhs, rhs);

  case kIntArithm_Shl:
    if (op_jty == kLong) {
      return irb_.CreateShl(lhs, irb_.CreateAnd(rhs, 0x3f));
    } else {
      return irb_.CreateShl(lhs, irb_.CreateAnd(rhs, 0x1f));
    }

  case kIntArithm_Shr:
    if (op_jty == kLong) {
      return irb_.CreateAShr(lhs, irb_.CreateAnd(rhs, 0x3f));
    } else {
      return irb_.CreateAShr(lhs, irb_.CreateAnd(rhs, 0x1f));
    }

  case kIntArithm_UShr:
    if (op_jty == kLong) {
      return irb_.CreateLShr(lhs, irb_.CreateAnd(rhs, 0x3f));
    } else {
      return irb_.CreateLShr(lhs, irb_.CreateAnd(rhs, 0x1f));
    }

  default:
    LOG(FATAL) << "Unknown integer arithmetic kind: " << arithm;
    return NULL;
  }
}


void MethodCompiler::EmitInsn_RSubImmediate(uint32_t dex_pc,
                                            Instruction const* insn) {

  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, kInt, kAccurate);
  llvm::Value* imm_value = irb_.getInt32(dec_insn.vC);
  llvm::Value* result_value = irb_.CreateSub(imm_value, src_value);
  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FPArithm(uint32_t dex_pc,
                                       Instruction const* insn,
                                       FPArithmKind arithm,
                                       JType op_jty,
                                       bool is_2addr) {

  DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kFloat || op_jty == kDouble) << op_jty;

  llvm::Value* src1_value;
  llvm::Value* src2_value;

  if (is_2addr) {
    src1_value = EmitLoadDalvikReg(dec_insn.vA, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
  } else {
    src1_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vC, op_jty, kAccurate);
  }

  llvm::Value* result_value =
    EmitFPArithmResultComputation(dex_pc, src1_value, src2_value, arithm);

  EmitStoreDalvikReg(dec_insn.vA, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


llvm::Value*
MethodCompiler::EmitFPArithmResultComputation(uint32_t dex_pc,
                                              llvm::Value *lhs,
                                              llvm::Value *rhs,
                                              FPArithmKind arithm) {
  switch (arithm) {
  case kFPArithm_Add:
    return irb_.CreateFAdd(lhs, rhs);

  case kFPArithm_Sub:
    return irb_.CreateFSub(lhs, rhs);

  case kFPArithm_Mul:
    return irb_.CreateFMul(lhs, rhs);

  case kFPArithm_Div:
    return irb_.CreateFDiv(lhs, rhs);

  case kFPArithm_Rem:
    return irb_.CreateFRem(lhs, rhs);

  default:
    LOG(FATAL) << "Unknown floating-point arithmetic kind: " << arithm;
    return NULL;
  }
}


void MethodCompiler::EmitGuard_DivZeroException(uint32_t dex_pc,
                                                llvm::Value* denominator,
                                                JType op_jty) {
  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  llvm::Constant* zero = irb_.getJZero(op_jty);

  llvm::Value* equal_zero = irb_.CreateICmpEQ(denominator, zero);

  llvm::BasicBlock* block_exception = CreateBasicBlockWithDexPC(dex_pc, "div0");

  llvm::BasicBlock* block_continue = CreateBasicBlockWithDexPC(dex_pc, "cont");

  irb_.CreateCondBr(equal_zero, block_exception, block_continue);

  irb_.SetInsertPoint(block_exception);
  EmitUpdateLineNumFromDexPC(dex_pc);
  irb_.CreateCall(irb_.GetRuntime(ThrowDivZeroException));
  EmitBranchExceptionLandingPad(dex_pc);

  irb_.SetInsertPoint(block_continue);
}


llvm::Value* MethodCompiler::EmitLoadClassObjectAddr(llvm::Value* this_addr) {
  llvm::Constant* class_field_offset_value =
    irb_.getPtrEquivInt(Object::ClassOffset().Int32Value());

  llvm::Value* class_field_addr =
    irb_.CreatePtrDisp(this_addr, class_field_offset_value,
                       irb_.getJObjectTy()->getPointerTo());

  return irb_.CreateLoad(class_field_addr);
}


llvm::Value*
MethodCompiler::EmitLoadVTableAddr(llvm::Value* class_object_addr) {
  llvm::Constant* vtable_offset_value =
    irb_.getPtrEquivInt(Class::VTableOffset().Int32Value());

  llvm::Value* vtable_field_addr =
    irb_.CreatePtrDisp(class_object_addr, vtable_offset_value,
                       irb_.getJObjectTy()->getPointerTo());

  return irb_.CreateLoad(vtable_field_addr);
}


llvm::Value* MethodCompiler::
EmitLoadMethodObjectAddrFromVTable(llvm::Value* vtable_addr,
                                   uint16_t vtable_index) {
  llvm::Value* vtable_index_value =
    irb_.getPtrEquivInt(static_cast<uint64_t>(vtable_index));

  llvm::Value* method_field_addr =
    EmitArrayGEP(vtable_addr, vtable_index_value, irb_.getJObjectTy(), kObject);

  return irb_.CreateLoad(method_field_addr);
}


llvm::Value* MethodCompiler::EmitLoadCodeAddr(llvm::Value* method_object_addr,
                                              uint32_t method_idx,
                                              bool is_static) {

  llvm::Value* code_field_offset_value =
    irb_.getPtrEquivInt(Method::GetCodeOffset().Int32Value());

  llvm::FunctionType* method_type = GetFunctionType(method_idx, is_static);

  llvm::Value* code_field_addr =
    irb_.CreatePtrDisp(method_object_addr, code_field_offset_value,
                       method_type->getPointerTo()->getPointerTo());

  return irb_.CreateLoad(code_field_addr);
}


void MethodCompiler::EmitGuard_NullPointerException(uint32_t dex_pc,
                                                    llvm::Value* object) {
  llvm::Value* equal_null = irb_.CreateICmpEQ(object, irb_.getJNull());

  llvm::BasicBlock* block_exception =
    CreateBasicBlockWithDexPC(dex_pc, "nullp");

  llvm::BasicBlock* block_continue =
    CreateBasicBlockWithDexPC(dex_pc, "cont");

  irb_.CreateCondBr(equal_null, block_exception, block_continue);

  irb_.SetInsertPoint(block_exception);
  EmitUpdateLineNumFromDexPC(dex_pc);
  irb_.CreateCall(irb_.GetRuntime(ThrowNullPointerException));
  EmitBranchExceptionLandingPad(dex_pc);

  irb_.SetInsertPoint(block_continue);
}


llvm::Value* MethodCompiler::EmitLoadDexCacheAddr(MemberOffset offset) {
  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  llvm::Value* dex_cache_offset_value =
    irb_.getPtrEquivInt(offset.Int32Value());

  llvm::Value* dex_cache_field_addr =
    irb_.CreatePtrDisp(method_object_addr, dex_cache_offset_value,
                       irb_.getJObjectTy()->getPointerTo());

  return irb_.CreateLoad(dex_cache_field_addr);
}


llvm::Value* MethodCompiler::
EmitLoadDexCacheStaticStorageFieldAddr(uint32_t type_idx) {
  llvm::Value* static_storage_dex_cache_addr =
    EmitLoadDexCacheAddr(Method::DexCacheInitializedStaticStorageOffset());

  llvm::Value* type_idx_value = irb_.getPtrEquivInt(type_idx);

  return EmitArrayGEP(static_storage_dex_cache_addr, type_idx_value,
                      irb_.getJObjectTy(), kObject);
}


llvm::Value* MethodCompiler::
EmitLoadDexCacheResolvedTypeFieldAddr(uint32_t type_idx) {
  llvm::Value* resolved_type_dex_cache_addr =
    EmitLoadDexCacheAddr(Method::DexCacheResolvedTypesOffset());

  llvm::Value* type_idx_value = irb_.getPtrEquivInt(type_idx);

  return EmitArrayGEP(resolved_type_dex_cache_addr, type_idx_value,
                      irb_.getJObjectTy(), kObject);
}


llvm::Value* MethodCompiler::
EmitLoadDexCacheResolvedMethodFieldAddr(uint32_t method_idx) {
  llvm::Value* resolved_method_dex_cache_addr =
    EmitLoadDexCacheAddr(Method::DexCacheResolvedMethodsOffset());

  llvm::Value* method_idx_value = irb_.getPtrEquivInt(method_idx);

  return EmitArrayGEP(resolved_method_dex_cache_addr, method_idx_value,
                      irb_.getJObjectTy(), kObject);
}


llvm::Value* MethodCompiler::
EmitLoadDexCacheStringFieldAddr(uint32_t string_idx) {
  llvm::Value* string_dex_cache_addr =
    EmitLoadDexCacheAddr(Method::DexCacheStringsOffset());

  llvm::Value* string_idx_value = irb_.getPtrEquivInt(string_idx);

  return EmitArrayGEP(string_dex_cache_addr, string_idx_value,
                      irb_.getJObjectTy(), kObject);
}


CompiledMethod *MethodCompiler::Compile() {
  // Code generation
  CreateFunction();

  EmitPrologue();
  EmitInstructions();
  EmitPrologueLastBranch();

  // Verify the generated bitcode
  llvm::verifyFunction(*func_, llvm::PrintMessageAction);

  // Delete the inferred register category map (won't be used anymore)
  method_->ResetInferredRegCategoryMap();

  // Add the memory usage approximation of the compilation unit
  cunit_->AddMemUsageApproximation(code_item_->insns_size_in_code_units_ * 900);
  // NOTE: From statistic, the bitcode size is 4.5 times bigger than the
  // Dex file.  Besides, we have to convert the code unit into bytes.
  // Thus, we got our magic number 9.

  return new CompiledMethod(cunit_->GetInstructionSet(), func_);
}


llvm::Value* MethodCompiler::EmitLoadMethodObjectAddr() {
  return func_->arg_begin();
}


void MethodCompiler::EmitBranchExceptionLandingPad(uint32_t dex_pc) {
  if (llvm::BasicBlock* lpad = GetLandingPadBasicBlock(dex_pc)) {
    irb_.CreateBr(lpad);
  } else {
    irb_.CreateBr(GetUnwindBasicBlock());
  }
}


void MethodCompiler::EmitGuard_ExceptionLandingPad(uint32_t dex_pc) {
  llvm::Value* exception_pending =
    irb_.CreateCall(irb_.GetRuntime(IsExceptionPending));

  llvm::BasicBlock* block_cont = CreateBasicBlockWithDexPC(dex_pc, "cont");

  if (llvm::BasicBlock* lpad = GetLandingPadBasicBlock(dex_pc)) {
    irb_.CreateCondBr(exception_pending, lpad, block_cont);
  } else {
    irb_.CreateCondBr(exception_pending, GetUnwindBasicBlock(), block_cont);
  }

  irb_.SetInsertPoint(block_cont);
}


void MethodCompiler::EmitGuard_GarbageCollectionSuspend(uint32_t dex_pc) {
  llvm::Value* runtime_func = irb_.GetRuntime(TestSuspend);
  EmitUpdateLineNumFromDexPC(dex_pc);
  irb_.CreateCall(runtime_func);

  EmitGuard_ExceptionLandingPad(dex_pc);
}


llvm::BasicBlock* MethodCompiler::
CreateBasicBlockWithDexPC(uint32_t dex_pc, char const* postfix) {
  std::string name;

  if (postfix) {
    StringAppendF(&name, "B%u.%s", dex_pc, postfix);
  } else {
    StringAppendF(&name, "B%u", dex_pc);
  }

  return llvm::BasicBlock::Create(*context_, name, func_);
}


llvm::BasicBlock* MethodCompiler::GetBasicBlock(uint32_t dex_pc) {
  DCHECK(dex_pc < code_item_->insns_size_in_code_units_);

  llvm::BasicBlock* basic_block = basic_blocks_[dex_pc];

  if (!basic_block) {
    basic_block = CreateBasicBlockWithDexPC(dex_pc);
    basic_blocks_[dex_pc] = basic_block;
  }

  return basic_block;
}


llvm::BasicBlock*
MethodCompiler::GetNextBasicBlock(uint32_t dex_pc) {
  Instruction const* insn = Instruction::At(code_item_->insns_ + dex_pc);
  return GetBasicBlock(dex_pc + insn->SizeInCodeUnits());
}


int32_t MethodCompiler::GetTryItemOffset(uint32_t dex_pc) {
  // TODO: Since we are emitting the dex instructions in ascending order
  // w.r.t.  address, we can cache the lastest try item offset so that we
  // don't have to do binary search for every query.

  int32_t min = 0;
  int32_t max = code_item_->tries_size_ - 1;

  while (min <= max) {
    int32_t mid = min + (max - min) / 2;

    DexFile::TryItem const* ti = DexFile::GetTryItems(*code_item_, mid);
    uint32_t start = ti->start_addr_;
    uint32_t end = start + ti->insn_count_;

    if (dex_pc < start) {
      max = mid - 1;
    } else if (dex_pc >= end) {
      min = mid + 1;
    } else {
      return mid; // found
    }
  }

  return -1; // not found
}


llvm::BasicBlock* MethodCompiler::GetLandingPadBasicBlock(uint32_t dex_pc) {
  // Find the try item for this address in this method
  int32_t ti_offset = GetTryItemOffset(dex_pc);

  if (ti_offset == -1) {
    return NULL; // No landing pad is available for this address.
  }

  // Check for the existing landing pad basic block
  DCHECK_GT(basic_block_landing_pads_.size(), static_cast<size_t>(ti_offset));
  llvm::BasicBlock* block_lpad = basic_block_landing_pads_[ti_offset];

  if (block_lpad) {
    // We have generated landing pad for this try item already.  Return the
    // same basic block.
    return block_lpad;
  }

  // Get try item from code item
  DexFile::TryItem const* ti = DexFile::GetTryItems(*code_item_, ti_offset);

  // Create landing pad basic block
  block_lpad = llvm::BasicBlock::Create(*context_,
                                        StringPrintf("lpad%d", ti_offset),
                                        func_);

  // Change IRBuilder insert point
  llvm::IRBuilderBase::InsertPoint irb_ip_original = irb_.saveIP();
  irb_.SetInsertPoint(block_lpad);

  // Find catch block with matching type
  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  // TODO: Maybe passing try item offset will be a better idea?  For now,
  // we are passing dex_pc, so that we can use existing runtime support
  // function directly.  However, in the runtime supporting function we
  // have to search for try item with binary search which can be
  // eliminated.
  llvm::Value* dex_pc_value = irb_.getInt32(ti->start_addr_);

  llvm::Value* catch_handler_index_value =
    irb_.CreateCall2(irb_.GetRuntime(FindCatchBlock),
                     method_object_addr, dex_pc_value);

  // Switch instruction (Go to unwind basic block by default)
  llvm::SwitchInst* sw =
    irb_.CreateSwitch(catch_handler_index_value, GetUnwindBasicBlock());

  // Cases with matched catch block
  CatchHandlerIterator iter(*code_item_, ti->start_addr_);

  for (uint32_t c = 0; iter.HasNext(); iter.Next(), ++c) {
    sw->addCase(irb_.getInt32(c), GetBasicBlock(iter.GetHandlerAddress()));
  }

  // Restore the orignal insert point for IRBuilder
  irb_.restoreIP(irb_ip_original);

  // Cache this landing pad
  DCHECK_GT(basic_block_landing_pads_.size(), static_cast<size_t>(ti_offset));
  basic_block_landing_pads_[ti_offset] = block_lpad;

  return block_lpad;
}


llvm::BasicBlock* MethodCompiler::GetUnwindBasicBlock() {
  // Check the existing unwinding baisc block block
  if (basic_block_unwind_ != NULL) {
    return basic_block_unwind_;
  }

  // Create new basic block for unwinding
  basic_block_unwind_ =
    llvm::BasicBlock::Create(*context_, "exception_unwind", func_);

  // Change IRBuilder insert point
  llvm::IRBuilderBase::InsertPoint irb_ip_original = irb_.saveIP();
  irb_.SetInsertPoint(basic_block_unwind_);

  // Pop the shadow frame
  EmitPopShadowFrame();

  // Emit the code to return default value (zero) for the given return type.
  char ret_shorty = method_helper_.GetShorty()[0];
  if (ret_shorty == 'V') {
    irb_.CreateRetVoid();
  } else {
    irb_.CreateRet(irb_.getJZero(ret_shorty));
  }

  // Restore the orignal insert point for IRBuilder
  irb_.restoreIP(irb_ip_original);

  return basic_block_unwind_;
}


llvm::Value* MethodCompiler::AllocDalvikLocalVarReg(RegCategory cat,
                                                    uint32_t reg_idx) {

  // Save current IR builder insert point
  llvm::IRBuilderBase::InsertPoint irb_ip_original = irb_.saveIP();

  // Alloca
  llvm::Value* reg_addr = NULL;

  switch (cat) {
  case kRegCat1nr:
    irb_.SetInsertPoint(basic_block_reg_alloca_);
    reg_addr = irb_.CreateAlloca(irb_.getJIntTy(), 0,
                                 StringPrintf("r%u", reg_idx));

    irb_.SetInsertPoint(basic_block_reg_zero_init_);
    irb_.CreateStore(irb_.getJInt(0), reg_addr);
    break;

  case kRegCat2:
    irb_.SetInsertPoint(basic_block_reg_alloca_);
    reg_addr = irb_.CreateAlloca(irb_.getJLongTy(), 0,
                                 StringPrintf("w%u", reg_idx));

    irb_.SetInsertPoint(basic_block_reg_zero_init_);
    irb_.CreateStore(irb_.getJLong(0), reg_addr);
    break;

  case kRegObject:
    {
      irb_.SetInsertPoint(basic_block_shadow_frame_alloca_);

      llvm::Value* gep_index[] = {
        irb_.getInt32(0), // No pointer displacement
        irb_.getInt32(1), // SIRT
        irb_.getInt32(reg_idx) // Pointer field
      };

      reg_addr = irb_.CreateGEP(shadow_frame_, gep_index,
                                StringPrintf("p%u", reg_idx));

      irb_.SetInsertPoint(basic_block_reg_zero_init_);
      irb_.CreateStore(irb_.getJNull(), reg_addr);
    }
    break;

  default:
    LOG(FATAL) << "Unknown register category for allocation: " << cat;
  }

  // Restore IRBuilder insert point
  irb_.restoreIP(irb_ip_original);

  DCHECK_NE(reg_addr, static_cast<llvm::Value*>(NULL));
  return reg_addr;
}


llvm::Value* MethodCompiler::AllocDalvikRetValReg(RegCategory cat) {
  // Save current IR builder insert point
  llvm::IRBuilderBase::InsertPoint irb_ip_original = irb_.saveIP();

  // Alloca
  llvm::Value* reg_addr = NULL;

  switch (cat) {
  case kRegCat1nr:
    irb_.SetInsertPoint(basic_block_reg_alloca_);
    reg_addr = irb_.CreateAlloca(irb_.getJIntTy(), 0, "r_res");
    break;

  case kRegCat2:
    irb_.SetInsertPoint(basic_block_reg_alloca_);
    reg_addr = irb_.CreateAlloca(irb_.getJLongTy(), 0, "w_res");
    break;

  case kRegObject:
    irb_.SetInsertPoint(basic_block_reg_alloca_);
    reg_addr = irb_.CreateAlloca(irb_.getJObjectTy(), 0, "p_res");
    break;

  default:
    LOG(FATAL) << "Unknown register category for allocation: " << cat;
  }

  // Restore IRBuilder insert point
  irb_.restoreIP(irb_ip_original);

  DCHECK_NE(reg_addr, static_cast<llvm::Value*>(NULL));
  return reg_addr;
}


void MethodCompiler::EmitPopShadowFrame() {
  irb_.CreateCall(irb_.GetRuntime(PopShadowFrame));
}


void MethodCompiler::EmitUpdateLineNum(int32_t line_num) {
  llvm::Constant* zero = irb_.getInt32(0);

  llvm::Value* gep_index[] = {
    zero, // No displacement for shadow frame pointer
    zero, // Get the %ArtFrame data structure
    irb_.getInt32(2),
  };

  llvm::Value* line_num_field_addr = irb_.CreateGEP(shadow_frame_, gep_index);
  llvm::ConstantInt* line_num_value = irb_.getInt32(line_num);
  irb_.CreateStore(line_num_value, line_num_field_addr);
}


void MethodCompiler::EmitUpdateLineNumFromDexPC(uint32_t dex_pc) {
  EmitUpdateLineNum(dex_file_->GetLineNumFromPC(method_, dex_pc));
}


} // namespace compiler_llvm
} // namespace art
