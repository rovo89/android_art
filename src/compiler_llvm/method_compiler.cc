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
#include "compiler.h"
#include "inferred_reg_category_map.h"
#include "ir_builder.h"
#include "logging.h"
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

namespace art {
namespace compiler_llvm {

using namespace runtime_support;


MethodCompiler::MethodCompiler(InstructionSet insn_set,
                               Compiler* compiler,
                               ClassLinker* class_linker,
                               ClassLoader const* class_loader,
                               DexFile const* dex_file,
                               DexCache* dex_cache,
                               DexFile::CodeItem const* code_item,
                               uint32_t method_idx,
                               uint32_t access_flags)
: insn_set_(insn_set),
  compiler_(compiler), compiler_llvm_(compiler->GetCompilerLLVM()),
  class_linker_(class_linker), class_loader_(class_loader),
  dex_file_(dex_file), dex_cache_(dex_cache), code_item_(code_item),
  method_(dex_cache->GetResolvedMethod(method_idx)),
  method_helper_(method_), method_idx_(method_idx),
  access_flags_(access_flags), module_(compiler_llvm_->GetModule()),
  context_(compiler_llvm_->GetLLVMContext()),
  irb_(*compiler_llvm_->GetIRBuilder()), func_(NULL), retval_reg_(NULL),
  basic_block_reg_alloca_(NULL),
  basic_block_reg_zero_init_(NULL), basic_block_reg_arg_init_(NULL),
  basic_blocks_(code_item->insns_size_in_code_units_),
  basic_block_landing_pads_(code_item->tries_size_, NULL),
  basic_block_unwind_(NULL), basic_block_unreachable_(NULL) {
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

  int32_t shorty_size;
  char const* shorty = dex_file_->GetMethodShorty(method_id, &shorty_size);
  CHECK_GE(shorty_size, 1);

  // Get return type
  llvm::Type* ret_type = irb_.getJType(shorty[0], kAccurate);

  // Get argument type
  std::vector<llvm::Type*> args_type;

  args_type.push_back(irb_.getJObjectTy()); // method object pointer

  if (!is_static) {
    args_type.push_back(irb_.getJType('L', kAccurate)); // "this" object pointer
  }

  for (int32_t i = 1; i < shorty_size; ++i) {
    args_type.push_back(irb_.getJType(shorty[i], kAccurate));
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}


void MethodCompiler::EmitPrologue() {
  // Create basic blocks for prologue
  basic_block_reg_alloca_ =
    llvm::BasicBlock::Create(*context_, "prologue.alloca", func_);

  basic_block_reg_zero_init_ =
    llvm::BasicBlock::Create(*context_, "prologue.zeroinit", func_);

  basic_block_reg_arg_init_ =
    llvm::BasicBlock::Create(*context_, "prologue.arginit", func_);

  // Create register array
  for (uint16_t r = 0; r < code_item_->registers_size_; ++r) {
    regs_.push_back(DalvikReg::CreateLocalVarReg(*this, r));
  }

  retval_reg_.reset(DalvikReg::CreateRetValReg(*this));

  // Store argument to dalvik register
  irb_.SetInsertPoint(basic_block_reg_arg_init_);
  EmitPrologueAssignArgRegister();

  // Branch to start address
  irb_.CreateBr(GetBasicBlock(0));
}


void MethodCompiler::EmitPrologueLastBranch() {
  irb_.SetInsertPoint(basic_block_reg_alloca_);
  irb_.CreateBr(basic_block_reg_zero_init_);

  irb_.SetInsertPoint(basic_block_reg_zero_init_);
  irb_.CreateBr(basic_block_reg_arg_init_);
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
    EmitInsn_InvokeDirect(ARGS, false);
    break;

  case Instruction::INVOKE_STATIC:
    EmitInsn_InvokeStatic(ARGS, false);
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
    EmitInsn_InvokeDirect(ARGS, true);
    break;

  case Instruction::INVOKE_STATIC_RANGE:
    EmitInsn_InvokeStatic(ARGS, true);
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
  } else{
    irb_.CreateBr(GetNextBasicBlock(dex_pc));
  }
}


void MethodCompiler::EmitInsn_Move(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType jty) {

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, jty, kReg);
  EmitStoreDalvikReg(dec_insn.vA_, jty, kReg, src_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_MoveResult(uint32_t dex_pc,
                                         Instruction const* insn,
                                         JType jty) {

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikRetValReg(jty, kReg);
  EmitStoreDalvikReg(dec_insn.vA_, jty, kReg, src_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_MoveException(uint32_t dex_pc,
                                            Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_ThrowException(uint32_t dex_pc,
                                             Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateUnreachable();
}


void MethodCompiler::EmitInsn_ReturnVoid(uint32_t dex_pc,
                                         Instruction const* insn) {
  // Garbage collection safe-point
  EmitGuard_GarbageCollectionSuspend(dex_pc);

  // Return!
  irb_.CreateRetVoid();
}


void MethodCompiler::EmitInsn_Return(uint32_t dex_pc,
                                     Instruction const* insn) {

  Instruction::DecodedInstruction dec_insn(insn);

  // Garbage collection safe-point
  EmitGuard_GarbageCollectionSuspend(dex_pc);

  // Return!
  char ret_shorty = method_helper_.GetShorty()[0];
  llvm::Value* retval = EmitLoadDalvikReg(dec_insn.vA_, ret_shorty, kAccurate);

  irb_.CreateRet(retval);
}


void MethodCompiler::EmitInsn_LoadConstant(uint32_t dex_pc,
                                           Instruction const* insn,
                                           JType imm_jty) {

  Instruction::DecodedInstruction dec_insn(insn);

  DCHECK(imm_jty == kInt || imm_jty == kLong) << imm_jty;

  int64_t imm = 0;

  switch (insn->Opcode()) {
  // 32-bit Immediate
  case Instruction::CONST_4:
  case Instruction::CONST_16:
  case Instruction::CONST:
  case Instruction::CONST_WIDE_16:
  case Instruction::CONST_WIDE_32:
    imm = static_cast<int64_t>(static_cast<int32_t>(dec_insn.vB_));
    break;

  case Instruction::CONST_HIGH16:
    imm = static_cast<int64_t>(static_cast<int32_t>(
          static_cast<uint32_t>(static_cast<uint16_t>(dec_insn.vB_)) << 16));
    break;

  // 64-bit Immediate
  case Instruction::CONST_WIDE:
    imm = static_cast<int64_t>(dec_insn.vB_wide_);
    break;

  case Instruction::CONST_WIDE_HIGH16:
    imm = static_cast<int64_t>(
          static_cast<uint64_t>(static_cast<uint16_t>(dec_insn.vB_)) << 48);
    break;

  // Unknown opcode for load constant (unreachable)
  default:
    LOG(FATAL) << "Unknown opcode for load constant: " << insn->Opcode();
    break;
  }

  // Store the non-object register
  llvm::Type* imm_type = irb_.getJType(imm_jty, kAccurate);
  llvm::Constant* imm_value = llvm::ConstantInt::getSigned(imm_type, imm);
  EmitStoreDalvikReg(dec_insn.vA_, imm_jty, kAccurate, imm_value);

  // Store the object register if it is possible to be null.
  if (imm_jty == kInt && imm == 0) {
    EmitStoreDalvikReg(dec_insn.vA_, kObject, kAccurate, irb_.getJNull());
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_LoadConstantString(uint32_t dex_pc,
                                                 Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_LoadConstantClass(uint32_t dex_pc,
                                                Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_MonitorEnter(uint32_t dex_pc,
                                           Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_MonitorExit(uint32_t dex_pc,
                                          Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_CheckCast(uint32_t dex_pc,
                                        Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_InstanceOf(uint32_t dex_pc,
                                         Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_ArrayLength(uint32_t dex_pc,
                                          Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_NewInstance(uint32_t dex_pc,
                                          Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_NewArray(uint32_t dex_pc,
                                       Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FilledNewArray(uint32_t dex_pc,
                                             Instruction const* insn,
                                             bool is_range) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FillArrayData(uint32_t dex_pc,
                                            Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_UnconditionalBranch(uint32_t dex_pc,
                                                  Instruction const* insn) {

  Instruction::DecodedInstruction dec_insn(insn);

  int32_t branch_offset = dec_insn.vA_;

  if (branch_offset <= 0) {
    // Garbage collection safe-point on backward branch
    EmitGuard_GarbageCollectionSuspend(dex_pc);
  }

  irb_.CreateBr(GetBasicBlock(dex_pc + branch_offset));
}


void MethodCompiler::EmitInsn_PackedSwitch(uint32_t dex_pc,
                                           Instruction const* insn) {

  Instruction::DecodedInstruction dec_insn(insn);

  struct PACKED Payload {
    uint16_t ident_;
    uint16_t num_cases_;
    int32_t first_key_;
    int32_t targets_[];
  };

  int32_t payload_offset = static_cast<int32_t>(dex_pc) +
                           static_cast<int32_t>(dec_insn.vB_);

  Payload const* payload =
    reinterpret_cast<Payload const*>(code_item_->insns_ + payload_offset);

  llvm::Value* value = EmitLoadDalvikReg(dec_insn.vA_, kInt, kAccurate);

  llvm::SwitchInst* sw =
    irb_.CreateSwitch(value, GetNextBasicBlock(dex_pc), payload->num_cases_);

  for (uint16_t i = 0; i < payload->num_cases_; ++i) {
    sw->addCase(irb_.getInt32(payload->first_key_ + i),
                GetBasicBlock(dex_pc + payload->targets_[i]));
  }
}


void MethodCompiler::EmitInsn_SparseSwitch(uint32_t dex_pc,
                                           Instruction const* insn) {

  Instruction::DecodedInstruction dec_insn(insn);

  struct PACKED Payload {
    uint16_t ident_;
    uint16_t num_cases_;
    int32_t keys_and_targets_[];
  };

  int32_t payload_offset = static_cast<int32_t>(dex_pc) +
                           static_cast<int32_t>(dec_insn.vB_);

  Payload const* payload =
    reinterpret_cast<Payload const*>(code_item_->insns_ + payload_offset);

  int32_t const* keys = payload->keys_and_targets_;
  int32_t const* targets = payload->keys_and_targets_ + payload->num_cases_;

  llvm::Value* value = EmitLoadDalvikReg(dec_insn.vA_, kInt, kAccurate);

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

  Instruction::DecodedInstruction dec_insn(insn);

  DCHECK(fp_jty == kFloat || fp_jty == kDouble) << "JType: " << fp_jty;

  llvm::Value* src1_value = EmitLoadDalvikReg(dec_insn.vB_, fp_jty, kAccurate);
  llvm::Value* src2_value = EmitLoadDalvikReg(dec_insn.vC_, fp_jty, kAccurate);

  llvm::Value* cmp_eq = irb_.CreateFCmpOEQ(src1_value, src2_value);
  llvm::Value* cmp_lt;

  if (gt_bias) {
    cmp_lt = irb_.CreateFCmpOLT(src1_value, src2_value);
  } else {
    cmp_lt = irb_.CreateFCmpULT(src1_value, src2_value);
  }

  llvm::Value* result = EmitCompareResultSelection(cmp_eq, cmp_lt);
  EmitStoreDalvikReg(dec_insn.vA_, kInt, kAccurate, result);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_LongCompare(uint32_t dex_pc,
                                          Instruction const* insn) {

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src1_value = EmitLoadDalvikReg(dec_insn.vB_, kLong, kAccurate);
  llvm::Value* src2_value = EmitLoadDalvikReg(dec_insn.vC_, kLong, kAccurate);

  llvm::Value* cmp_eq = irb_.CreateICmpEQ(src1_value, src2_value);
  llvm::Value* cmp_lt = irb_.CreateICmpSLT(src1_value, src2_value);

  llvm::Value* result = EmitCompareResultSelection(cmp_eq, cmp_lt);
  EmitStoreDalvikReg(dec_insn.vA_, kInt, kAccurate, result);

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

  Instruction::DecodedInstruction dec_insn(insn);

  int8_t src1_reg_cat = GetInferredRegCategory(dex_pc, dec_insn.vA_);
  int8_t src2_reg_cat = GetInferredRegCategory(dex_pc, dec_insn.vB_);

  DCHECK_NE(kRegUnknown, src1_reg_cat);
  DCHECK_NE(kRegUnknown, src2_reg_cat);
  DCHECK_NE(kRegCat2, src1_reg_cat);
  DCHECK_NE(kRegCat2, src2_reg_cat);

  int32_t branch_offset = dec_insn.vC_;

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
      src1_value = EmitLoadDalvikReg(dec_insn.vA_, kInt, kAccurate);
      src2_value = EmitLoadDalvikReg(dec_insn.vB_, kInt, kAccurate);
    } else {
      src1_value = EmitLoadDalvikReg(dec_insn.vA_, kObject, kAccurate);
      src2_value = EmitLoadDalvikReg(dec_insn.vB_, kObject, kAccurate);
    }
  } else {
    DCHECK(src1_reg_cat == kRegZero ||
           src2_reg_cat == kRegZero);

    if (src1_reg_cat == kRegZero) {
      if (src2_reg_cat == kRegCat1nr) {
        src1_value = irb_.getJInt(0);
        src2_value = EmitLoadDalvikReg(dec_insn.vA_, kInt, kAccurate);
      } else {
        src1_value = irb_.getJNull();
        src2_value = EmitLoadDalvikReg(dec_insn.vA_, kObject, kAccurate);
      }
    } else { // src2_reg_cat == kRegZero
      if (src2_reg_cat == kRegCat1nr) {
        src1_value = EmitLoadDalvikReg(dec_insn.vA_, kInt, kAccurate);
        src2_value = irb_.getJInt(0);
      } else {
        src1_value = EmitLoadDalvikReg(dec_insn.vA_, kObject, kAccurate);
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

  Instruction::DecodedInstruction dec_insn(insn);

  int8_t src_reg_cat = GetInferredRegCategory(dex_pc, dec_insn.vA_);

  DCHECK_NE(kRegUnknown, src_reg_cat);
  DCHECK_NE(kRegCat2, src_reg_cat);

  int32_t branch_offset = dec_insn.vB_;

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
    src1_value = EmitLoadDalvikReg(dec_insn.vA_, kInt, kAccurate);
    src2_value = irb_.getInt32(0);
  } else {
    src1_value = EmitLoadDalvikReg(dec_insn.vA_, kObject, kAccurate);
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


void MethodCompiler::EmitInsn_AGet(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType elem_jty) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_APut(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType elem_jty) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_IGet(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType field_jty) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_IPut(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType field_jty) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_SGet(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType field_jty) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_SPut(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType field_jty) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_InvokeVirtual(uint32_t dex_pc,
                                            Instruction const* insn,
                                            bool is_range) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_InvokeSuper(uint32_t dex_pc,
                                          Instruction const* insn,
                                          bool is_range) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_InvokeDirect(uint32_t dex_pc,
                                           Instruction const* insn,
                                           bool is_range) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_InvokeStatic(uint32_t dex_pc,
                                           Instruction const* insn,
                                           bool is_range) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_InvokeInterface(uint32_t dex_pc,
                                              Instruction const* insn,
                                              bool is_range) {
  // UNIMPLEMENTED(WARNING);
  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_Neg(uint32_t dex_pc,
                                  Instruction const* insn,
                                  JType op_jty) {

  Instruction::DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, op_jty, kAccurate);
  llvm::Value* result_value = irb_.CreateNeg(src_value);
  EmitStoreDalvikReg(dec_insn.vA_, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_Not(uint32_t dex_pc,
                                  Instruction const* insn,
                                  JType op_jty) {

  Instruction::DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, op_jty, kAccurate);
  llvm::Value* result_value =
    irb_.CreateXor(src_value, static_cast<uint64_t>(-1));

  EmitStoreDalvikReg(dec_insn.vA_, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_SExt(uint32_t dex_pc,
                                   Instruction const* insn) {

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, kInt, kAccurate);
  llvm::Value* result_value = irb_.CreateSExt(src_value, irb_.getJLongTy());
  EmitStoreDalvikReg(dec_insn.vA_, kLong, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_Trunc(uint32_t dex_pc,
                                    Instruction const* insn) {

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, kLong, kAccurate);
  llvm::Value* result_value = irb_.CreateTrunc(src_value, irb_.getJIntTy());
  EmitStoreDalvikReg(dec_insn.vA_, kInt, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_TruncAndSExt(uint32_t dex_pc,
                                           Instruction const* insn,
                                           unsigned N) {

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, kInt, kAccurate);

  llvm::Value* trunc_value =
    irb_.CreateTrunc(src_value, llvm::Type::getIntNTy(*context_, N));

  llvm::Value* result_value = irb_.CreateSExt(trunc_value, irb_.getJIntTy());

  EmitStoreDalvikReg(dec_insn.vA_, kInt, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_TruncAndZExt(uint32_t dex_pc,
                                           Instruction const* insn,
                                           unsigned N) {

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, kInt, kAccurate);

  llvm::Value* trunc_value =
    irb_.CreateTrunc(src_value, llvm::Type::getIntNTy(*context_, N));

  llvm::Value* result_value = irb_.CreateZExt(trunc_value, irb_.getJIntTy());

  EmitStoreDalvikReg(dec_insn.vA_, kInt, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FNeg(uint32_t dex_pc,
                                   Instruction const* insn,
                                   JType op_jty) {

  Instruction::DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kFloat || op_jty == kDouble) << op_jty;

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, op_jty, kAccurate);
  llvm::Value* result_value = irb_.CreateFNeg(src_value);
  EmitStoreDalvikReg(dec_insn.vA_, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_IntToFP(uint32_t dex_pc,
                                      Instruction const* insn,
                                      JType src_jty,
                                      JType dest_jty) {

  Instruction::DecodedInstruction dec_insn(insn);

  DCHECK(src_jty == kInt || src_jty == kLong) << src_jty;
  DCHECK(dest_jty == kFloat || dest_jty == kDouble) << dest_jty;

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, src_jty, kAccurate);
  llvm::Type* dest_type = irb_.getJType(dest_jty, kAccurate);
  llvm::Value* dest_value = irb_.CreateSIToFP(src_value, dest_type);
  EmitStoreDalvikReg(dec_insn.vA_, dest_jty, kAccurate, dest_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FPToInt(uint32_t dex_pc,
                                      Instruction const* insn,
                                      JType src_jty,
                                      JType dest_jty) {

  Instruction::DecodedInstruction dec_insn(insn);

  DCHECK(src_jty == kFloat || src_jty == kDouble) << src_jty;
  DCHECK(dest_jty == kInt || dest_jty == kLong) << dest_jty;

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, src_jty, kAccurate);
  llvm::Type* dest_type = irb_.getJType(dest_jty, kAccurate);
  llvm::Value* dest_value = irb_.CreateFPToSI(src_value, dest_type);
  EmitStoreDalvikReg(dec_insn.vA_, dest_jty, kAccurate, dest_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FExt(uint32_t dex_pc,
                                   Instruction const* insn) {

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, kFloat, kAccurate);
  llvm::Value* result_value = irb_.CreateFPExt(src_value, irb_.getJDoubleTy());
  EmitStoreDalvikReg(dec_insn.vA_, kDouble, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FTrunc(uint32_t dex_pc,
                                     Instruction const* insn) {

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, kDouble, kAccurate);
  llvm::Value* result_value = irb_.CreateFPTrunc(src_value, irb_.getJFloatTy());
  EmitStoreDalvikReg(dec_insn.vA_, kFloat, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_IntArithm(uint32_t dex_pc,
                                        Instruction const* insn,
                                        IntArithmKind arithm,
                                        JType op_jty,
                                        bool is_2addr) {

  Instruction::DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  llvm::Value* src1_value;
  llvm::Value* src2_value;

  if (is_2addr) {
    src1_value = EmitLoadDalvikReg(dec_insn.vA_, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vB_, op_jty, kAccurate);
  } else {
    src1_value = EmitLoadDalvikReg(dec_insn.vB_, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vC_, op_jty, kAccurate);
  }

  llvm::Value* result_value =
    EmitIntArithmResultComputation(dex_pc, src1_value, src2_value,
                                   arithm, op_jty);

  EmitStoreDalvikReg(dec_insn.vA_, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_IntArithmImmediate(uint32_t dex_pc,
                                                 Instruction const* insn,
                                                 IntArithmKind arithm) {

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, kInt, kAccurate);

  llvm::Value* imm_value = irb_.getInt32(dec_insn.vC_);

  llvm::Value* result_value =
    EmitIntArithmResultComputation(dex_pc, src_value, imm_value, arithm, kInt);

  EmitStoreDalvikReg(dec_insn.vA_, kInt, kAccurate, result_value);

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

  Instruction::DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB_, kInt, kAccurate);
  llvm::Value* imm_value = irb_.getInt32(dec_insn.vC_);
  llvm::Value* result_value = irb_.CreateSub(imm_value, src_value);
  EmitStoreDalvikReg(dec_insn.vA_, kInt, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
}


void MethodCompiler::EmitInsn_FPArithm(uint32_t dex_pc,
                                       Instruction const* insn,
                                       FPArithmKind arithm,
                                       JType op_jty,
                                       bool is_2addr) {

  Instruction::DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kFloat || op_jty == kDouble) << op_jty;

  llvm::Value* src1_value;
  llvm::Value* src2_value;

  if (is_2addr) {
    src1_value = EmitLoadDalvikReg(dec_insn.vA_, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vB_, op_jty, kAccurate);
  } else {
    src1_value = EmitLoadDalvikReg(dec_insn.vB_, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vC_, op_jty, kAccurate);
  }

  llvm::Value* result_value =
    EmitFPArithmResultComputation(dex_pc, src1_value, src2_value, arithm);

  EmitStoreDalvikReg(dec_insn.vA_, op_jty, kAccurate, result_value);

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
  irb_.CreateCall(irb_.GetRuntime(ThrowDivZeroException));
  EmitBranchExceptionLandingPad(dex_pc);

  irb_.SetInsertPoint(block_continue);
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

  return new CompiledMethod(insn_set_, func_);
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
    irb_.SetInsertPoint(basic_block_reg_alloca_);
    reg_addr = irb_.CreateAlloca(irb_.getJObjectTy(), 0,
                                 StringPrintf("p%u", reg_idx));

    irb_.SetInsertPoint(basic_block_reg_zero_init_);
    irb_.CreateStore(irb_.getJNull(), reg_addr);
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


} // namespace compiler_llvm
} // namespace art
