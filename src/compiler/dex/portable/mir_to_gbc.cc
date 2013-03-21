/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "object_utils.h"

#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Metadata.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/Instruction.h>
#include <llvm/Type.h>
#include <llvm/Instructions.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/InstIterator.h>

#include "compiler/dex/compiler_internals.h"
#include "compiler/dex/dataflow_iterator.h"

//TODO: move gbc_to_lir code into quick directory (if necessary).
#include "compiler/dex/quick/codegen_util.h"
#include "compiler/dex/quick/local_optimizations.h"
#include "compiler/dex/quick/ralloc_util.h"
#include "compiler/llvm/llvm_compilation_unit.h"
#include "compiler/llvm/utils_llvm.h"

static const char* kLabelFormat = "%c0x%x_%d";
static const char kInvalidBlock = 0xff;
static const char kNormalBlock = 'L';
static const char kCatchBlock = 'C';

namespace art {

static ::llvm::BasicBlock* GetLLVMBlock(CompilationUnit* cu, int id)
{
  return cu->id_to_block_map.Get(id);
}

static ::llvm::Value* GetLLVMValue(CompilationUnit* cu, int s_reg)
{
  return reinterpret_cast< ::llvm::Value*>(GrowableListGetElement(&cu->llvm_values, s_reg));
}

static void SetVregOnValue(CompilationUnit* cu, ::llvm::Value* val, int s_reg)
{
  // Set vreg for debugging
  art::llvm::IntrinsicHelper::IntrinsicId id = art::llvm::IntrinsicHelper::SetVReg;
  ::llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(id);
  int v_reg = cu->mir_graph->SRegToVReg(s_reg);
  ::llvm::Value* table_slot = cu->irb->getInt32(v_reg);
  ::llvm::Value* args[] = { table_slot, val };
  cu->irb->CreateCall(func, args);
}

// Replace the placeholder value with the real definition
static void DefineValueOnly(CompilationUnit* cu, ::llvm::Value* val, int s_reg)
{
  ::llvm::Value* placeholder = GetLLVMValue(cu, s_reg);
  if (placeholder == NULL) {
    // This can happen on instruction rewrite on verification failure
    LOG(WARNING) << "Null placeholder";
    return;
  }
  placeholder->replaceAllUsesWith(val);
  val->takeName(placeholder);
  cu->llvm_values.elem_list[s_reg] = reinterpret_cast<uintptr_t>(val);
  ::llvm::Instruction* inst = ::llvm::dyn_cast< ::llvm::Instruction>(placeholder);
  DCHECK(inst != NULL);
  inst->eraseFromParent();

}

static void DefineValue(CompilationUnit* cu, ::llvm::Value* val, int s_reg)
{
  DefineValueOnly(cu, val, s_reg);
  SetVregOnValue(cu, val, s_reg);
}

static ::llvm::Type* LlvmTypeFromLocRec(CompilationUnit* cu, RegLocation loc)
{
  ::llvm::Type* res = NULL;
  if (loc.wide) {
    if (loc.fp)
        res = cu->irb->getDoubleTy();
    else
        res = cu->irb->getInt64Ty();
  } else {
    if (loc.fp) {
      res = cu->irb->getFloatTy();
    } else {
      if (loc.ref)
        res = cu->irb->getJObjectTy();
      else
        res = cu->irb->getInt32Ty();
    }
  }
  return res;
}

static void InitIR(CompilationUnit* cu)
{
  LLVMInfo* llvm_info = cu->llvm_info;
  if (llvm_info == NULL) {
    CompilerTls* tls = cu->compiler_driver->GetTls();
    CHECK(tls != NULL);
    llvm_info = static_cast<LLVMInfo*>(tls->GetLLVMInfo());
    if (llvm_info == NULL) {
      llvm_info = new LLVMInfo();
      tls->SetLLVMInfo(llvm_info);
    }
  }
  cu->context = llvm_info->GetLLVMContext();
  cu->module = llvm_info->GetLLVMModule();
  cu->intrinsic_helper = llvm_info->GetIntrinsicHelper();
  cu->irb = llvm_info->GetIRBuilder();
}

::llvm::BasicBlock* FindCaseTarget(CompilationUnit* cu, uint32_t vaddr)
{
  BasicBlock* bb = cu->mir_graph.get()->FindBlock(vaddr);
  DCHECK(bb != NULL);
  return GetLLVMBlock(cu, bb->id);
}

static void ConvertPackedSwitch(CompilationUnit* cu, BasicBlock* bb,
                                int32_t table_offset, RegLocation rl_src)
{
  const Instruction::PackedSwitchPayload* payload =
      reinterpret_cast<const Instruction::PackedSwitchPayload*>(
      cu->insns + cu->current_dalvik_offset + table_offset);

  ::llvm::Value* value = GetLLVMValue(cu, rl_src.orig_sreg);

  ::llvm::SwitchInst* sw =
    cu->irb->CreateSwitch(value, GetLLVMBlock(cu, bb->fall_through->id),
                             payload->case_count);

  for (uint16_t i = 0; i < payload->case_count; ++i) {
    ::llvm::BasicBlock* llvm_bb =
        FindCaseTarget(cu, cu->current_dalvik_offset + payload->targets[i]);
    sw->addCase(cu->irb->getInt32(payload->first_key + i), llvm_bb);
  }
  ::llvm::MDNode* switch_node =
      ::llvm::MDNode::get(*cu->context, cu->irb->getInt32(table_offset));
  sw->setMetadata("SwitchTable", switch_node);
  bb->taken = NULL;
  bb->fall_through = NULL;
}

static void ConvertSparseSwitch(CompilationUnit* cu, BasicBlock* bb,
                                int32_t table_offset, RegLocation rl_src)
{
  const Instruction::SparseSwitchPayload* payload =
      reinterpret_cast<const Instruction::SparseSwitchPayload*>(
      cu->insns + cu->current_dalvik_offset + table_offset);

  const int32_t* keys = payload->GetKeys();
  const int32_t* targets = payload->GetTargets();

  ::llvm::Value* value = GetLLVMValue(cu, rl_src.orig_sreg);

  ::llvm::SwitchInst* sw =
    cu->irb->CreateSwitch(value, GetLLVMBlock(cu, bb->fall_through->id),
                             payload->case_count);

  for (size_t i = 0; i < payload->case_count; ++i) {
    ::llvm::BasicBlock* llvm_bb =
        FindCaseTarget(cu, cu->current_dalvik_offset + targets[i]);
    sw->addCase(cu->irb->getInt32(keys[i]), llvm_bb);
  }
  ::llvm::MDNode* switch_node =
      ::llvm::MDNode::get(*cu->context, cu->irb->getInt32(table_offset));
  sw->setMetadata("SwitchTable", switch_node);
  bb->taken = NULL;
  bb->fall_through = NULL;
}

static void ConvertSget(CompilationUnit* cu, int32_t field_index,
                        art::llvm::IntrinsicHelper::IntrinsicId id, RegLocation rl_dest)
{
  ::llvm::Constant* field_idx = cu->irb->getInt32(field_index);
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::Value* res = cu->irb->CreateCall(intr, field_idx);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertSput(CompilationUnit* cu, int32_t field_index,
                        art::llvm::IntrinsicHelper::IntrinsicId id, RegLocation rl_src)
{
  ::llvm::SmallVector< ::llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(field_index));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(intr, args);
}

static void ConvertFillArrayData(CompilationUnit* cu, int32_t offset, RegLocation rl_array)
{
  art::llvm::IntrinsicHelper::IntrinsicId id;
  id = art::llvm::IntrinsicHelper::HLFillArrayData;
  ::llvm::SmallVector< ::llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(offset));
  args.push_back(GetLLVMValue(cu, rl_array.orig_sreg));
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(intr, args);
}

static ::llvm::Value* EmitConst(CompilationUnit* cu, ::llvm::ArrayRef< ::llvm::Value*> src,
                              RegLocation loc)
{
  art::llvm::IntrinsicHelper::IntrinsicId id;
  if (loc.wide) {
    if (loc.fp) {
      id = art::llvm::IntrinsicHelper::ConstDouble;
    } else {
      id = art::llvm::IntrinsicHelper::ConstLong;
    }
  } else {
    if (loc.fp) {
      id = art::llvm::IntrinsicHelper::ConstFloat;
    } else if (loc.ref) {
      id = art::llvm::IntrinsicHelper::ConstObj;
    } else {
      id = art::llvm::IntrinsicHelper::ConstInt;
    }
  }
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  return cu->irb->CreateCall(intr, src);
}

static void EmitPopShadowFrame(CompilationUnit* cu)
{
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(
      art::llvm::IntrinsicHelper::PopShadowFrame);
  cu->irb->CreateCall(intr);
}

static ::llvm::Value* EmitCopy(CompilationUnit* cu, ::llvm::ArrayRef< ::llvm::Value*> src,
                             RegLocation loc)
{
  art::llvm::IntrinsicHelper::IntrinsicId id;
  if (loc.wide) {
    if (loc.fp) {
      id = art::llvm::IntrinsicHelper::CopyDouble;
    } else {
      id = art::llvm::IntrinsicHelper::CopyLong;
    }
  } else {
    if (loc.fp) {
      id = art::llvm::IntrinsicHelper::CopyFloat;
    } else if (loc.ref) {
      id = art::llvm::IntrinsicHelper::CopyObj;
    } else {
      id = art::llvm::IntrinsicHelper::CopyInt;
    }
  }
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  return cu->irb->CreateCall(intr, src);
}

static void ConvertMoveException(CompilationUnit* cu, RegLocation rl_dest)
{
  ::llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(
      art::llvm::IntrinsicHelper::GetException);
  ::llvm::Value* res = cu->irb->CreateCall(func);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertThrow(CompilationUnit* cu, RegLocation rl_src)
{
  ::llvm::Value* src = GetLLVMValue(cu, rl_src.orig_sreg);
  ::llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(
      art::llvm::IntrinsicHelper::HLThrowException);
  cu->irb->CreateCall(func, src);
}

static void ConvertMonitorEnterExit(CompilationUnit* cu, int opt_flags,
                                    art::llvm::IntrinsicHelper::IntrinsicId id,
                                    RegLocation rl_src)
{
  ::llvm::SmallVector< ::llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  ::llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(func, args);
}

static void ConvertArrayLength(CompilationUnit* cu, int opt_flags,
                               RegLocation rl_dest, RegLocation rl_src)
{
  ::llvm::SmallVector< ::llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  ::llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(
      art::llvm::IntrinsicHelper::OptArrayLength);
  ::llvm::Value* res = cu->irb->CreateCall(func, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void EmitSuspendCheck(CompilationUnit* cu)
{
  art::llvm::IntrinsicHelper::IntrinsicId id =
      art::llvm::IntrinsicHelper::CheckSuspend;
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(intr);
}

static ::llvm::Value* ConvertCompare(CompilationUnit* cu, ConditionCode cc,
                                   ::llvm::Value* src1, ::llvm::Value* src2)
{
  ::llvm::Value* res = NULL;
  DCHECK_EQ(src1->getType(), src2->getType());
  switch(cc) {
    case kCondEq: res = cu->irb->CreateICmpEQ(src1, src2); break;
    case kCondNe: res = cu->irb->CreateICmpNE(src1, src2); break;
    case kCondLt: res = cu->irb->CreateICmpSLT(src1, src2); break;
    case kCondGe: res = cu->irb->CreateICmpSGE(src1, src2); break;
    case kCondGt: res = cu->irb->CreateICmpSGT(src1, src2); break;
    case kCondLe: res = cu->irb->CreateICmpSLE(src1, src2); break;
    default: LOG(FATAL) << "Unexpected cc value " << cc;
  }
  return res;
}

static void ConvertCompareAndBranch(CompilationUnit* cu, BasicBlock* bb, MIR* mir,
                                    ConditionCode cc, RegLocation rl_src1, RegLocation rl_src2)
{
  if (bb->taken->start_offset <= mir->offset) {
    EmitSuspendCheck(cu);
  }
  ::llvm::Value* src1 = GetLLVMValue(cu, rl_src1.orig_sreg);
  ::llvm::Value* src2 = GetLLVMValue(cu, rl_src2.orig_sreg);
  ::llvm::Value* cond_value = ConvertCompare(cu, cc, src1, src2);
  cond_value->setName(StringPrintf("t%d", cu->temp_name++));
  cu->irb->CreateCondBr(cond_value, GetLLVMBlock(cu, bb->taken->id),
                           GetLLVMBlock(cu, bb->fall_through->id));
  // Don't redo the fallthrough branch in the BB driver
  bb->fall_through = NULL;
}

static void ConvertCompareZeroAndBranch(CompilationUnit* cu, BasicBlock* bb,
                                        MIR* mir, ConditionCode cc, RegLocation rl_src1)
{
  if (bb->taken->start_offset <= mir->offset) {
    EmitSuspendCheck(cu);
  }
  ::llvm::Value* src1 = GetLLVMValue(cu, rl_src1.orig_sreg);
  ::llvm::Value* src2;
  if (rl_src1.ref) {
    src2 = cu->irb->getJNull();
  } else {
    src2 = cu->irb->getInt32(0);
  }
  ::llvm::Value* cond_value = ConvertCompare(cu, cc, src1, src2);
  cu->irb->CreateCondBr(cond_value, GetLLVMBlock(cu, bb->taken->id),
                           GetLLVMBlock(cu, bb->fall_through->id));
  // Don't redo the fallthrough branch in the BB driver
  bb->fall_through = NULL;
}

static ::llvm::Value* GenDivModOp(CompilationUnit* cu, bool is_div, bool is_long,
                                ::llvm::Value* src1, ::llvm::Value* src2)
{
  art::llvm::IntrinsicHelper::IntrinsicId id;
  if (is_long) {
    if (is_div) {
      id = art::llvm::IntrinsicHelper::DivLong;
    } else {
      id = art::llvm::IntrinsicHelper::RemLong;
    }
  } else {
    if (is_div) {
      id = art::llvm::IntrinsicHelper::DivInt;
    } else {
      id = art::llvm::IntrinsicHelper::RemInt;
    }
  }
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::SmallVector< ::llvm::Value*, 2>args;
  args.push_back(src1);
  args.push_back(src2);
  return cu->irb->CreateCall(intr, args);
}

static ::llvm::Value* GenArithOp(CompilationUnit* cu, OpKind op, bool is_long,
                               ::llvm::Value* src1, ::llvm::Value* src2)
{
  ::llvm::Value* res = NULL;
  switch(op) {
    case kOpAdd: res = cu->irb->CreateAdd(src1, src2); break;
    case kOpSub: res = cu->irb->CreateSub(src1, src2); break;
    case kOpRsub: res = cu->irb->CreateSub(src2, src1); break;
    case kOpMul: res = cu->irb->CreateMul(src1, src2); break;
    case kOpOr: res = cu->irb->CreateOr(src1, src2); break;
    case kOpAnd: res = cu->irb->CreateAnd(src1, src2); break;
    case kOpXor: res = cu->irb->CreateXor(src1, src2); break;
    case kOpDiv: res = GenDivModOp(cu, true, is_long, src1, src2); break;
    case kOpRem: res = GenDivModOp(cu, false, is_long, src1, src2); break;
    case kOpLsl: res = cu->irb->CreateShl(src1, src2); break;
    case kOpLsr: res = cu->irb->CreateLShr(src1, src2); break;
    case kOpAsr: res = cu->irb->CreateAShr(src1, src2); break;
    default:
      LOG(FATAL) << "Invalid op " << op;
  }
  return res;
}

static void ConvertFPArithOp(CompilationUnit* cu, OpKind op, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_src2)
{
  ::llvm::Value* src1 = GetLLVMValue(cu, rl_src1.orig_sreg);
  ::llvm::Value* src2 = GetLLVMValue(cu, rl_src2.orig_sreg);
  ::llvm::Value* res = NULL;
  switch(op) {
    case kOpAdd: res = cu->irb->CreateFAdd(src1, src2); break;
    case kOpSub: res = cu->irb->CreateFSub(src1, src2); break;
    case kOpMul: res = cu->irb->CreateFMul(src1, src2); break;
    case kOpDiv: res = cu->irb->CreateFDiv(src1, src2); break;
    case kOpRem: res = cu->irb->CreateFRem(src1, src2); break;
    default:
      LOG(FATAL) << "Invalid op " << op;
  }
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertShift(CompilationUnit* cu, art::llvm::IntrinsicHelper::IntrinsicId id,
                         RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2)
{
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::SmallVector< ::llvm::Value*, 2>args;
  args.push_back(GetLLVMValue(cu, rl_src1.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_src2.orig_sreg));
  ::llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertShiftLit(CompilationUnit* cu, art::llvm::IntrinsicHelper::IntrinsicId id,
                            RegLocation rl_dest, RegLocation rl_src, int shift_amount)
{
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::SmallVector< ::llvm::Value*, 2>args;
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  args.push_back(cu->irb->getInt32(shift_amount));
  ::llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertArithOp(CompilationUnit* cu, OpKind op, RegLocation rl_dest,
                           RegLocation rl_src1, RegLocation rl_src2)
{
  ::llvm::Value* src1 = GetLLVMValue(cu, rl_src1.orig_sreg);
  ::llvm::Value* src2 = GetLLVMValue(cu, rl_src2.orig_sreg);
  DCHECK_EQ(src1->getType(), src2->getType());
  ::llvm::Value* res = GenArithOp(cu, op, rl_dest.wide, src1, src2);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertArithOpLit(CompilationUnit* cu, OpKind op, RegLocation rl_dest,
                              RegLocation rl_src1, int32_t imm)
{
  ::llvm::Value* src1 = GetLLVMValue(cu, rl_src1.orig_sreg);
  ::llvm::Value* src2 = cu->irb->getInt32(imm);
  ::llvm::Value* res = GenArithOp(cu, op, rl_dest.wide, src1, src2);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

/*
 * Process arguments for invoke.  Note: this code is also used to
 * collect and process arguments for NEW_FILLED_ARRAY and NEW_FILLED_ARRAY_RANGE.
 * The requirements are similar.
 */
static void ConvertInvoke(CompilationUnit* cu, BasicBlock* bb, MIR* mir,
                          InvokeType invoke_type, bool is_range, bool is_filled_new_array)
{
  Codegen* cg = cu->cg.get();
  CallInfo* info = cg->NewMemCallInfo(cu, bb, mir, invoke_type, is_range);
  ::llvm::SmallVector< ::llvm::Value*, 10> args;
  // Insert the invoke_type
  args.push_back(cu->irb->getInt32(static_cast<int>(invoke_type)));
  // Insert the method_idx
  args.push_back(cu->irb->getInt32(info->index));
  // Insert the optimization flags
  args.push_back(cu->irb->getInt32(info->opt_flags));
  // Now, insert the actual arguments
  for (int i = 0; i < info->num_arg_words;) {
    ::llvm::Value* val = GetLLVMValue(cu, info->args[i].orig_sreg);
    args.push_back(val);
    i += info->args[i].wide ? 2 : 1;
  }
  /*
   * Choose the invoke return type based on actual usage.  Note: may
   * be different than shorty.  For example, if a function return value
   * is not used, we'll treat this as a void invoke.
   */
  art::llvm::IntrinsicHelper::IntrinsicId id;
  if (is_filled_new_array) {
    id = art::llvm::IntrinsicHelper::HLFilledNewArray;
  } else if (info->result.location == kLocInvalid) {
    id = art::llvm::IntrinsicHelper::HLInvokeVoid;
  } else {
    if (info->result.wide) {
      if (info->result.fp) {
        id = art::llvm::IntrinsicHelper::HLInvokeDouble;
      } else {
        id = art::llvm::IntrinsicHelper::HLInvokeLong;
      }
    } else if (info->result.ref) {
        id = art::llvm::IntrinsicHelper::HLInvokeObj;
    } else if (info->result.fp) {
        id = art::llvm::IntrinsicHelper::HLInvokeFloat;
    } else {
        id = art::llvm::IntrinsicHelper::HLInvokeInt;
    }
  }
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::Value* res = cu->irb->CreateCall(intr, args);
  if (info->result.location != kLocInvalid) {
    DefineValue(cu, res, info->result.orig_sreg);
  }
}

static void ConvertConstObject(CompilationUnit* cu, uint32_t idx,
                               art::llvm::IntrinsicHelper::IntrinsicId id, RegLocation rl_dest)
{
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::Value* index = cu->irb->getInt32(idx);
  ::llvm::Value* res = cu->irb->CreateCall(intr, index);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertCheckCast(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_src)
{
  art::llvm::IntrinsicHelper::IntrinsicId id;
  id = art::llvm::IntrinsicHelper::HLCheckCast;
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::SmallVector< ::llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(type_idx));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  cu->irb->CreateCall(intr, args);
}

static void ConvertNewInstance(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest)
{
  art::llvm::IntrinsicHelper::IntrinsicId id;
  id = art::llvm::IntrinsicHelper::NewInstance;
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::Value* index = cu->irb->getInt32(type_idx);
  ::llvm::Value* res = cu->irb->CreateCall(intr, index);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertNewArray(CompilationUnit* cu, uint32_t type_idx,
                            RegLocation rl_dest, RegLocation rl_src)
{
  art::llvm::IntrinsicHelper::IntrinsicId id;
  id = art::llvm::IntrinsicHelper::NewArray;
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::SmallVector< ::llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(type_idx));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  ::llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertAget(CompilationUnit* cu, int opt_flags,
                        art::llvm::IntrinsicHelper::IntrinsicId id,
                        RegLocation rl_dest, RegLocation rl_array, RegLocation rl_index)
{
  ::llvm::SmallVector< ::llvm::Value*, 3> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_array.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_index.orig_sreg));
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertAput(CompilationUnit* cu, int opt_flags,
                        art::llvm::IntrinsicHelper::IntrinsicId id,
                        RegLocation rl_src, RegLocation rl_array, RegLocation rl_index)
{
  ::llvm::SmallVector< ::llvm::Value*, 4> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_array.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_index.orig_sreg));
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(intr, args);
}

static void ConvertIget(CompilationUnit* cu, int opt_flags,
                        art::llvm::IntrinsicHelper::IntrinsicId id,
                        RegLocation rl_dest, RegLocation rl_obj, int field_index)
{
  ::llvm::SmallVector< ::llvm::Value*, 3> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_obj.orig_sreg));
  args.push_back(cu->irb->getInt32(field_index));
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertIput(CompilationUnit* cu, int opt_flags,
                        art::llvm::IntrinsicHelper::IntrinsicId id,
                        RegLocation rl_src, RegLocation rl_obj, int field_index)
{
  ::llvm::SmallVector< ::llvm::Value*, 4> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_obj.orig_sreg));
  args.push_back(cu->irb->getInt32(field_index));
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(intr, args);
}

static void ConvertInstanceOf(CompilationUnit* cu, uint32_t type_idx,
                              RegLocation rl_dest, RegLocation rl_src)
{
  art::llvm::IntrinsicHelper::IntrinsicId id;
  id = art::llvm::IntrinsicHelper::InstanceOf;
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::SmallVector< ::llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(type_idx));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  ::llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertIntToLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  ::llvm::Value* res = cu->irb->CreateSExt(GetLLVMValue(cu, rl_src.orig_sreg),
                                            cu->irb->getInt64Ty());
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertLongToInt(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  ::llvm::Value* src = GetLLVMValue(cu, rl_src.orig_sreg);
  ::llvm::Value* res = cu->irb->CreateTrunc(src, cu->irb->getInt32Ty());
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertFloatToDouble(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  ::llvm::Value* src = GetLLVMValue(cu, rl_src.orig_sreg);
  ::llvm::Value* res = cu->irb->CreateFPExt(src, cu->irb->getDoubleTy());
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertDoubleToFloat(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  ::llvm::Value* src = GetLLVMValue(cu, rl_src.orig_sreg);
  ::llvm::Value* res = cu->irb->CreateFPTrunc(src, cu->irb->getFloatTy());
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertWideComparison(CompilationUnit* cu,
                                  art::llvm::IntrinsicHelper::IntrinsicId id,
                                  RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_src2)
{
  DCHECK_EQ(rl_src1.fp, rl_src2.fp);
  DCHECK_EQ(rl_src1.wide, rl_src2.wide);
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::SmallVector< ::llvm::Value*, 2> args;
  args.push_back(GetLLVMValue(cu, rl_src1.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_src2.orig_sreg));
  ::llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertIntNarrowing(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src,
                                art::llvm::IntrinsicHelper::IntrinsicId id)
{
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::Value* res =
      cu->irb->CreateCall(intr, GetLLVMValue(cu, rl_src.orig_sreg));
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertNeg(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  ::llvm::Value* res = cu->irb->CreateNeg(GetLLVMValue(cu, rl_src.orig_sreg));
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertIntToFP(CompilationUnit* cu, ::llvm::Type* ty, RegLocation rl_dest,
                           RegLocation rl_src)
{
  ::llvm::Value* res =
      cu->irb->CreateSIToFP(GetLLVMValue(cu, rl_src.orig_sreg), ty);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertFPToInt(CompilationUnit* cu, art::llvm::IntrinsicHelper::IntrinsicId id,
                           RegLocation rl_dest,
                    RegLocation rl_src)
{
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::Value* res = cu->irb->CreateCall(intr, GetLLVMValue(cu, rl_src.orig_sreg));
  DefineValue(cu, res, rl_dest.orig_sreg);
}


static void ConvertNegFP(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  ::llvm::Value* res =
      cu->irb->CreateFNeg(GetLLVMValue(cu, rl_src.orig_sreg));
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertNot(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  ::llvm::Value* src = GetLLVMValue(cu, rl_src.orig_sreg);
  ::llvm::Value* res = cu->irb->CreateXor(src, static_cast<uint64_t>(-1));
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void EmitConstructorBarrier(CompilationUnit* cu) {
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(
      art::llvm::IntrinsicHelper::ConstructorBarrier);
  cu->irb->CreateCall(intr);
}

/*
 * Target-independent code generation.  Use only high-level
 * load/store utilities here, or target-dependent genXX() handlers
 * when necessary.
 */
static bool ConvertMIRNode(CompilationUnit* cu, MIR* mir, BasicBlock* bb,
                           ::llvm::BasicBlock* llvm_bb)
{
  bool res = false;   // Assume success
  RegLocation rl_src[3];
  RegLocation rl_dest = GetBadLoc();
  Instruction::Code opcode = mir->dalvikInsn.opcode;
  int op_val = opcode;
  uint32_t vB = mir->dalvikInsn.vB;
  uint32_t vC = mir->dalvikInsn.vC;
  int opt_flags = mir->optimization_flags;

  if (cu->verbose) {
    if (op_val < kMirOpFirst) {
      LOG(INFO) << ".. " << Instruction::Name(opcode) << " 0x" << std::hex << op_val;
    } else {
      LOG(INFO) << extended_mir_op_names[op_val - kMirOpFirst] << " 0x" << std::hex << op_val;
    }
  }

  /* Prep Src and Dest locations */
  int next_sreg = 0;
  int next_loc = 0;
  int attrs = oat_data_flow_attributes[opcode];
  rl_src[0] = rl_src[1] = rl_src[2] = GetBadLoc();
  if (attrs & DF_UA) {
    if (attrs & DF_A_WIDE) {
      rl_src[next_loc++] = GetSrcWide(cu, mir, next_sreg);
      next_sreg+= 2;
    } else {
      rl_src[next_loc++] = GetSrc(cu, mir, next_sreg);
      next_sreg++;
    }
  }
  if (attrs & DF_UB) {
    if (attrs & DF_B_WIDE) {
      rl_src[next_loc++] = GetSrcWide(cu, mir, next_sreg);
      next_sreg+= 2;
    } else {
      rl_src[next_loc++] = GetSrc(cu, mir, next_sreg);
      next_sreg++;
    }
  }
  if (attrs & DF_UC) {
    if (attrs & DF_C_WIDE) {
      rl_src[next_loc++] = GetSrcWide(cu, mir, next_sreg);
    } else {
      rl_src[next_loc++] = GetSrc(cu, mir, next_sreg);
    }
  }
  if (attrs & DF_DA) {
    if (attrs & DF_A_WIDE) {
      rl_dest = GetDestWide(cu, mir);
    } else {
      rl_dest = GetDest(cu, mir);
    }
  }

  switch (opcode) {
    case Instruction::NOP:
      break;

    case Instruction::MOVE:
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_16:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_OBJECT_FROM16:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_16:
    case Instruction::MOVE_WIDE_FROM16: {
        /*
         * Moves/copies are meaningless in pure SSA register form,
         * but we need to preserve them for the conversion back into
         * MIR (at least until we stop using the Dalvik register maps).
         * Insert a dummy intrinsic copy call, which will be recognized
         * by the quick path and removed by the portable path.
         */
        ::llvm::Value* src = GetLLVMValue(cu, rl_src[0].orig_sreg);
        ::llvm::Value* res = EmitCopy(cu, src, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;

    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16: {
        ::llvm::Constant* imm_value = cu->irb->getJInt(vB);
        ::llvm::Value* res = EmitConst(cu, imm_value, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;

    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32: {
        // Sign extend to 64 bits
        int64_t imm = static_cast<int32_t>(vB);
        ::llvm::Constant* imm_value = cu->irb->getJLong(imm);
        ::llvm::Value* res = EmitConst(cu, imm_value, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;

    case Instruction::CONST_HIGH16: {
        ::llvm::Constant* imm_value = cu->irb->getJInt(vB << 16);
        ::llvm::Value* res = EmitConst(cu, imm_value, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;

    case Instruction::CONST_WIDE: {
        ::llvm::Constant* imm_value =
            cu->irb->getJLong(mir->dalvikInsn.vB_wide);
        ::llvm::Value* res = EmitConst(cu, imm_value, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;
    case Instruction::CONST_WIDE_HIGH16: {
        int64_t imm = static_cast<int64_t>(vB) << 48;
        ::llvm::Constant* imm_value = cu->irb->getJLong(imm);
        ::llvm::Value* res = EmitConst(cu, imm_value, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;

    case Instruction::SPUT_OBJECT:
      ConvertSput(cu, vB, art::llvm::IntrinsicHelper::HLSputObject,
                  rl_src[0]);
      break;
    case Instruction::SPUT:
      if (rl_src[0].fp) {
        ConvertSput(cu, vB, art::llvm::IntrinsicHelper::HLSputFloat,
                    rl_src[0]);
      } else {
        ConvertSput(cu, vB, art::llvm::IntrinsicHelper::HLSput, rl_src[0]);
      }
      break;
    case Instruction::SPUT_BOOLEAN:
      ConvertSput(cu, vB, art::llvm::IntrinsicHelper::HLSputBoolean,
                  rl_src[0]);
      break;
    case Instruction::SPUT_BYTE:
      ConvertSput(cu, vB, art::llvm::IntrinsicHelper::HLSputByte, rl_src[0]);
      break;
    case Instruction::SPUT_CHAR:
      ConvertSput(cu, vB, art::llvm::IntrinsicHelper::HLSputChar, rl_src[0]);
      break;
    case Instruction::SPUT_SHORT:
      ConvertSput(cu, vB, art::llvm::IntrinsicHelper::HLSputShort, rl_src[0]);
      break;
    case Instruction::SPUT_WIDE:
      if (rl_src[0].fp) {
        ConvertSput(cu, vB, art::llvm::IntrinsicHelper::HLSputDouble,
                    rl_src[0]);
      } else {
        ConvertSput(cu, vB, art::llvm::IntrinsicHelper::HLSputWide,
                    rl_src[0]);
      }
      break;

    case Instruction::SGET_OBJECT:
      ConvertSget(cu, vB, art::llvm::IntrinsicHelper::HLSgetObject, rl_dest);
      break;
    case Instruction::SGET:
      if (rl_dest.fp) {
        ConvertSget(cu, vB, art::llvm::IntrinsicHelper::HLSgetFloat, rl_dest);
      } else {
        ConvertSget(cu, vB, art::llvm::IntrinsicHelper::HLSget, rl_dest);
      }
      break;
    case Instruction::SGET_BOOLEAN:
      ConvertSget(cu, vB, art::llvm::IntrinsicHelper::HLSgetBoolean, rl_dest);
      break;
    case Instruction::SGET_BYTE:
      ConvertSget(cu, vB, art::llvm::IntrinsicHelper::HLSgetByte, rl_dest);
      break;
    case Instruction::SGET_CHAR:
      ConvertSget(cu, vB, art::llvm::IntrinsicHelper::HLSgetChar, rl_dest);
      break;
    case Instruction::SGET_SHORT:
      ConvertSget(cu, vB, art::llvm::IntrinsicHelper::HLSgetShort, rl_dest);
      break;
    case Instruction::SGET_WIDE:
      if (rl_dest.fp) {
        ConvertSget(cu, vB, art::llvm::IntrinsicHelper::HLSgetDouble,
                    rl_dest);
      } else {
        ConvertSget(cu, vB, art::llvm::IntrinsicHelper::HLSgetWide, rl_dest);
      }
      break;

    case Instruction::RETURN_WIDE:
    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT: {
        if (!(cu->attributes & METHOD_IS_LEAF)) {
          EmitSuspendCheck(cu);
        }
        EmitPopShadowFrame(cu);
        cu->irb->CreateRet(GetLLVMValue(cu, rl_src[0].orig_sreg));
        DCHECK(bb->terminated_by_return);
      }
      break;

    case Instruction::RETURN_VOID: {
        if (((cu->access_flags & kAccConstructor) != 0) &&
            cu->compiler_driver->RequiresConstructorBarrier(Thread::Current(),
                                                            cu->dex_file,
                                                            cu->class_def_idx)) {
          EmitConstructorBarrier(cu);
        }
        if (!(cu->attributes & METHOD_IS_LEAF)) {
          EmitSuspendCheck(cu);
        }
        EmitPopShadowFrame(cu);
        cu->irb->CreateRetVoid();
        DCHECK(bb->terminated_by_return);
      }
      break;

    case Instruction::IF_EQ:
      ConvertCompareAndBranch(cu, bb, mir, kCondEq, rl_src[0], rl_src[1]);
      break;
    case Instruction::IF_NE:
      ConvertCompareAndBranch(cu, bb, mir, kCondNe, rl_src[0], rl_src[1]);
      break;
    case Instruction::IF_LT:
      ConvertCompareAndBranch(cu, bb, mir, kCondLt, rl_src[0], rl_src[1]);
      break;
    case Instruction::IF_GE:
      ConvertCompareAndBranch(cu, bb, mir, kCondGe, rl_src[0], rl_src[1]);
      break;
    case Instruction::IF_GT:
      ConvertCompareAndBranch(cu, bb, mir, kCondGt, rl_src[0], rl_src[1]);
      break;
    case Instruction::IF_LE:
      ConvertCompareAndBranch(cu, bb, mir, kCondLe, rl_src[0], rl_src[1]);
      break;
    case Instruction::IF_EQZ:
      ConvertCompareZeroAndBranch(cu, bb, mir, kCondEq, rl_src[0]);
      break;
    case Instruction::IF_NEZ:
      ConvertCompareZeroAndBranch(cu, bb, mir, kCondNe, rl_src[0]);
      break;
    case Instruction::IF_LTZ:
      ConvertCompareZeroAndBranch(cu, bb, mir, kCondLt, rl_src[0]);
      break;
    case Instruction::IF_GEZ:
      ConvertCompareZeroAndBranch(cu, bb, mir, kCondGe, rl_src[0]);
      break;
    case Instruction::IF_GTZ:
      ConvertCompareZeroAndBranch(cu, bb, mir, kCondGt, rl_src[0]);
      break;
    case Instruction::IF_LEZ:
      ConvertCompareZeroAndBranch(cu, bb, mir, kCondLe, rl_src[0]);
      break;

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32: {
        if (bb->taken->start_offset <= bb->start_offset) {
          EmitSuspendCheck(cu);
        }
        cu->irb->CreateBr(GetLLVMBlock(cu, bb->taken->id));
      }
      break;

    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
      ConvertArithOp(cu, kOpAdd, rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      ConvertArithOp(cu, kOpSub, rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::MUL_LONG:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
      ConvertArithOp(cu, kOpMul, rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::DIV_LONG:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
      ConvertArithOp(cu, kOpDiv, rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::REM_LONG:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
      ConvertArithOp(cu, kOpRem, rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AND_LONG:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
      ConvertArithOp(cu, kOpAnd, rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
      ConvertArithOp(cu, kOpOr, rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
      ConvertArithOp(cu, kOpXor, rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      ConvertShift(cu, art::llvm::IntrinsicHelper::SHLLong,
                    rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      ConvertShift(cu, art::llvm::IntrinsicHelper::SHLInt,
                   rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      ConvertShift(cu, art::llvm::IntrinsicHelper::SHRLong,
                   rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      ConvertShift(cu, art::llvm::IntrinsicHelper::SHRInt,
                   rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      ConvertShift(cu, art::llvm::IntrinsicHelper::USHRLong,
                   rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      ConvertShift(cu, art::llvm::IntrinsicHelper::USHRInt,
                   rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::ADD_INT_LIT16:
    case Instruction::ADD_INT_LIT8:
      ConvertArithOpLit(cu, kOpAdd, rl_dest, rl_src[0], vC);
      break;
    case Instruction::RSUB_INT:
    case Instruction::RSUB_INT_LIT8:
      ConvertArithOpLit(cu, kOpRsub, rl_dest, rl_src[0], vC);
      break;
    case Instruction::MUL_INT_LIT16:
    case Instruction::MUL_INT_LIT8:
      ConvertArithOpLit(cu, kOpMul, rl_dest, rl_src[0], vC);
      break;
    case Instruction::DIV_INT_LIT16:
    case Instruction::DIV_INT_LIT8:
      ConvertArithOpLit(cu, kOpDiv, rl_dest, rl_src[0], vC);
      break;
    case Instruction::REM_INT_LIT16:
    case Instruction::REM_INT_LIT8:
      ConvertArithOpLit(cu, kOpRem, rl_dest, rl_src[0], vC);
      break;
    case Instruction::AND_INT_LIT16:
    case Instruction::AND_INT_LIT8:
      ConvertArithOpLit(cu, kOpAnd, rl_dest, rl_src[0], vC);
      break;
    case Instruction::OR_INT_LIT16:
    case Instruction::OR_INT_LIT8:
      ConvertArithOpLit(cu, kOpOr, rl_dest, rl_src[0], vC);
      break;
    case Instruction::XOR_INT_LIT16:
    case Instruction::XOR_INT_LIT8:
      ConvertArithOpLit(cu, kOpXor, rl_dest, rl_src[0], vC);
      break;
    case Instruction::SHL_INT_LIT8:
      ConvertShiftLit(cu, art::llvm::IntrinsicHelper::SHLInt,
                      rl_dest, rl_src[0], vC & 0x1f);
      break;
    case Instruction::SHR_INT_LIT8:
      ConvertShiftLit(cu, art::llvm::IntrinsicHelper::SHRInt,
                      rl_dest, rl_src[0], vC & 0x1f);
      break;
    case Instruction::USHR_INT_LIT8:
      ConvertShiftLit(cu, art::llvm::IntrinsicHelper::USHRInt,
                      rl_dest, rl_src[0], vC & 0x1f);
      break;

    case Instruction::ADD_FLOAT:
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::ADD_DOUBLE:
    case Instruction::ADD_DOUBLE_2ADDR:
      ConvertFPArithOp(cu, kOpAdd, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::SUB_FLOAT:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::SUB_DOUBLE:
    case Instruction::SUB_DOUBLE_2ADDR:
      ConvertFPArithOp(cu, kOpSub, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::MUL_FLOAT:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::MUL_DOUBLE:
    case Instruction::MUL_DOUBLE_2ADDR:
      ConvertFPArithOp(cu, kOpMul, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::DIV_FLOAT:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::DIV_DOUBLE:
    case Instruction::DIV_DOUBLE_2ADDR:
      ConvertFPArithOp(cu, kOpDiv, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::REM_FLOAT:
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::REM_DOUBLE:
    case Instruction::REM_DOUBLE_2ADDR:
      ConvertFPArithOp(cu, kOpRem, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::INVOKE_STATIC:
      ConvertInvoke(cu, bb, mir, kStatic, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_STATIC_RANGE:
      ConvertInvoke(cu, bb, mir, kStatic, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_DIRECT:
      ConvertInvoke(cu, bb,  mir, kDirect, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_DIRECT_RANGE:
      ConvertInvoke(cu, bb, mir, kDirect, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_VIRTUAL:
      ConvertInvoke(cu, bb, mir, kVirtual, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_VIRTUAL_RANGE:
      ConvertInvoke(cu, bb, mir, kVirtual, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_SUPER:
      ConvertInvoke(cu, bb, mir, kSuper, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_SUPER_RANGE:
      ConvertInvoke(cu, bb, mir, kSuper, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_INTERFACE:
      ConvertInvoke(cu, bb, mir, kInterface, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_INTERFACE_RANGE:
      ConvertInvoke(cu, bb, mir, kInterface, true /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::FILLED_NEW_ARRAY:
      ConvertInvoke(cu, bb, mir, kInterface, false /*range*/,
                    true /* NewFilledArray */);
      break;
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      ConvertInvoke(cu, bb, mir, kInterface, true /*range*/,
                    true /* NewFilledArray */);
      break;

    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      ConvertConstObject(cu, vB, art::llvm::IntrinsicHelper::ConstString,
                         rl_dest);
      break;

    case Instruction::CONST_CLASS:
      ConvertConstObject(cu, vB, art::llvm::IntrinsicHelper::ConstClass,
                         rl_dest);
      break;

    case Instruction::CHECK_CAST:
      ConvertCheckCast(cu, vB, rl_src[0]);
      break;

    case Instruction::NEW_INSTANCE:
      ConvertNewInstance(cu, vB, rl_dest);
      break;

   case Instruction::MOVE_EXCEPTION:
      ConvertMoveException(cu, rl_dest);
      break;

   case Instruction::THROW:
      ConvertThrow(cu, rl_src[0]);
      /*
       * If this throw is standalone, terminate.
       * If it might rethrow, force termination
       * of the following block.
       */
      if (bb->fall_through == NULL) {
        cu->irb->CreateUnreachable();
      } else {
        bb->fall_through->fall_through = NULL;
        bb->fall_through->taken = NULL;
      }
      break;

    case Instruction::MOVE_RESULT_WIDE:
    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_OBJECT:
      /*
       * All move_results should have been folded into the preceeding invoke.
       */
      LOG(FATAL) << "Unexpected move_result";
      break;

    case Instruction::MONITOR_ENTER:
      ConvertMonitorEnterExit(cu, opt_flags,
                              art::llvm::IntrinsicHelper::MonitorEnter,
                              rl_src[0]);
      break;

    case Instruction::MONITOR_EXIT:
      ConvertMonitorEnterExit(cu, opt_flags,
                              art::llvm::IntrinsicHelper::MonitorExit,
                              rl_src[0]);
      break;

    case Instruction::ARRAY_LENGTH:
      ConvertArrayLength(cu, opt_flags, rl_dest, rl_src[0]);
      break;

    case Instruction::NEW_ARRAY:
      ConvertNewArray(cu, vC, rl_dest, rl_src[0]);
      break;

    case Instruction::INSTANCE_OF:
      ConvertInstanceOf(cu, vC, rl_dest, rl_src[0]);
      break;

    case Instruction::AGET:
      if (rl_dest.fp) {
        ConvertAget(cu, opt_flags,
                    art::llvm::IntrinsicHelper::HLArrayGetFloat,
                    rl_dest, rl_src[0], rl_src[1]);
      } else {
        ConvertAget(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayGet,
                    rl_dest, rl_src[0], rl_src[1]);
      }
      break;
    case Instruction::AGET_OBJECT:
      ConvertAget(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayGetObject,
                  rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AGET_BOOLEAN:
      ConvertAget(cu, opt_flags,
                  art::llvm::IntrinsicHelper::HLArrayGetBoolean,
                  rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AGET_BYTE:
      ConvertAget(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayGetByte,
                  rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AGET_CHAR:
      ConvertAget(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayGetChar,
                  rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AGET_SHORT:
      ConvertAget(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayGetShort,
                  rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AGET_WIDE:
      if (rl_dest.fp) {
        ConvertAget(cu, opt_flags,
                    art::llvm::IntrinsicHelper::HLArrayGetDouble,
                    rl_dest, rl_src[0], rl_src[1]);
      } else {
        ConvertAget(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayGetWide,
                    rl_dest, rl_src[0], rl_src[1]);
      }
      break;

    case Instruction::APUT:
      if (rl_src[0].fp) {
        ConvertAput(cu, opt_flags,
                    art::llvm::IntrinsicHelper::HLArrayPutFloat,
                    rl_src[0], rl_src[1], rl_src[2]);
      } else {
        ConvertAput(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayPut,
                    rl_src[0], rl_src[1], rl_src[2]);
      }
      break;
    case Instruction::APUT_OBJECT:
      ConvertAput(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayPutObject,
                    rl_src[0], rl_src[1], rl_src[2]);
      break;
    case Instruction::APUT_BOOLEAN:
      ConvertAput(cu, opt_flags,
                  art::llvm::IntrinsicHelper::HLArrayPutBoolean,
                    rl_src[0], rl_src[1], rl_src[2]);
      break;
    case Instruction::APUT_BYTE:
      ConvertAput(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayPutByte,
                    rl_src[0], rl_src[1], rl_src[2]);
      break;
    case Instruction::APUT_CHAR:
      ConvertAput(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayPutChar,
                    rl_src[0], rl_src[1], rl_src[2]);
      break;
    case Instruction::APUT_SHORT:
      ConvertAput(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayPutShort,
                    rl_src[0], rl_src[1], rl_src[2]);
      break;
    case Instruction::APUT_WIDE:
      if (rl_src[0].fp) {
        ConvertAput(cu, opt_flags,
                    art::llvm::IntrinsicHelper::HLArrayPutDouble,
                    rl_src[0], rl_src[1], rl_src[2]);
      } else {
        ConvertAput(cu, opt_flags, art::llvm::IntrinsicHelper::HLArrayPutWide,
                    rl_src[0], rl_src[1], rl_src[2]);
      }
      break;

    case Instruction::IGET:
      if (rl_dest.fp) {
        ConvertIget(cu, opt_flags, art::llvm::IntrinsicHelper::HLIGetFloat,
                    rl_dest, rl_src[0], vC);
      } else {
        ConvertIget(cu, opt_flags, art::llvm::IntrinsicHelper::HLIGet,
                    rl_dest, rl_src[0], vC);
      }
      break;
    case Instruction::IGET_OBJECT:
      ConvertIget(cu, opt_flags, art::llvm::IntrinsicHelper::HLIGetObject,
                  rl_dest, rl_src[0], vC);
      break;
    case Instruction::IGET_BOOLEAN:
      ConvertIget(cu, opt_flags, art::llvm::IntrinsicHelper::HLIGetBoolean,
                  rl_dest, rl_src[0], vC);
      break;
    case Instruction::IGET_BYTE:
      ConvertIget(cu, opt_flags, art::llvm::IntrinsicHelper::HLIGetByte,
                  rl_dest, rl_src[0], vC);
      break;
    case Instruction::IGET_CHAR:
      ConvertIget(cu, opt_flags, art::llvm::IntrinsicHelper::HLIGetChar,
                  rl_dest, rl_src[0], vC);
      break;
    case Instruction::IGET_SHORT:
      ConvertIget(cu, opt_flags, art::llvm::IntrinsicHelper::HLIGetShort,
                  rl_dest, rl_src[0], vC);
      break;
    case Instruction::IGET_WIDE:
      if (rl_dest.fp) {
        ConvertIget(cu, opt_flags, art::llvm::IntrinsicHelper::HLIGetDouble,
                    rl_dest, rl_src[0], vC);
      } else {
        ConvertIget(cu, opt_flags, art::llvm::IntrinsicHelper::HLIGetWide,
                    rl_dest, rl_src[0], vC);
      }
      break;
    case Instruction::IPUT:
      if (rl_src[0].fp) {
        ConvertIput(cu, opt_flags, art::llvm::IntrinsicHelper::HLIPutFloat,
                    rl_src[0], rl_src[1], vC);
      } else {
        ConvertIput(cu, opt_flags, art::llvm::IntrinsicHelper::HLIPut,
                    rl_src[0], rl_src[1], vC);
      }
      break;
    case Instruction::IPUT_OBJECT:
      ConvertIput(cu, opt_flags, art::llvm::IntrinsicHelper::HLIPutObject,
                  rl_src[0], rl_src[1], vC);
      break;
    case Instruction::IPUT_BOOLEAN:
      ConvertIput(cu, opt_flags, art::llvm::IntrinsicHelper::HLIPutBoolean,
                  rl_src[0], rl_src[1], vC);
      break;
    case Instruction::IPUT_BYTE:
      ConvertIput(cu, opt_flags, art::llvm::IntrinsicHelper::HLIPutByte,
                  rl_src[0], rl_src[1], vC);
      break;
    case Instruction::IPUT_CHAR:
      ConvertIput(cu, opt_flags, art::llvm::IntrinsicHelper::HLIPutChar,
                  rl_src[0], rl_src[1], vC);
      break;
    case Instruction::IPUT_SHORT:
      ConvertIput(cu, opt_flags, art::llvm::IntrinsicHelper::HLIPutShort,
                  rl_src[0], rl_src[1], vC);
      break;
    case Instruction::IPUT_WIDE:
      if (rl_src[0].fp) {
        ConvertIput(cu, opt_flags, art::llvm::IntrinsicHelper::HLIPutDouble,
                    rl_src[0], rl_src[1], vC);
      } else {
        ConvertIput(cu, opt_flags, art::llvm::IntrinsicHelper::HLIPutWide,
                    rl_src[0], rl_src[1], vC);
      }
      break;

    case Instruction::FILL_ARRAY_DATA:
      ConvertFillArrayData(cu, vB, rl_src[0]);
      break;

    case Instruction::LONG_TO_INT:
      ConvertLongToInt(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::INT_TO_LONG:
      ConvertIntToLong(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::INT_TO_CHAR:
      ConvertIntNarrowing(cu, rl_dest, rl_src[0],
                          art::llvm::IntrinsicHelper::IntToChar);
      break;
    case Instruction::INT_TO_BYTE:
      ConvertIntNarrowing(cu, rl_dest, rl_src[0],
                          art::llvm::IntrinsicHelper::IntToByte);
      break;
    case Instruction::INT_TO_SHORT:
      ConvertIntNarrowing(cu, rl_dest, rl_src[0],
                          art::llvm::IntrinsicHelper::IntToShort);
      break;

    case Instruction::INT_TO_FLOAT:
    case Instruction::LONG_TO_FLOAT:
      ConvertIntToFP(cu, cu->irb->getFloatTy(), rl_dest, rl_src[0]);
      break;

    case Instruction::INT_TO_DOUBLE:
    case Instruction::LONG_TO_DOUBLE:
      ConvertIntToFP(cu, cu->irb->getDoubleTy(), rl_dest, rl_src[0]);
      break;

    case Instruction::FLOAT_TO_DOUBLE:
      ConvertFloatToDouble(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::DOUBLE_TO_FLOAT:
      ConvertDoubleToFloat(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::NEG_LONG:
    case Instruction::NEG_INT:
      ConvertNeg(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::NEG_FLOAT:
    case Instruction::NEG_DOUBLE:
      ConvertNegFP(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::NOT_LONG:
    case Instruction::NOT_INT:
      ConvertNot(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::FLOAT_TO_INT:
      ConvertFPToInt(cu, art::llvm::IntrinsicHelper::F2I, rl_dest, rl_src[0]);
      break;

    case Instruction::DOUBLE_TO_INT:
      ConvertFPToInt(cu, art::llvm::IntrinsicHelper::D2I, rl_dest, rl_src[0]);
      break;

    case Instruction::FLOAT_TO_LONG:
      ConvertFPToInt(cu, art::llvm::IntrinsicHelper::F2L, rl_dest, rl_src[0]);
      break;

    case Instruction::DOUBLE_TO_LONG:
      ConvertFPToInt(cu, art::llvm::IntrinsicHelper::D2L, rl_dest, rl_src[0]);
      break;

    case Instruction::CMPL_FLOAT:
      ConvertWideComparison(cu, art::llvm::IntrinsicHelper::CmplFloat,
                            rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::CMPG_FLOAT:
      ConvertWideComparison(cu, art::llvm::IntrinsicHelper::CmpgFloat,
                            rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::CMPL_DOUBLE:
      ConvertWideComparison(cu, art::llvm::IntrinsicHelper::CmplDouble,
                            rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::CMPG_DOUBLE:
      ConvertWideComparison(cu, art::llvm::IntrinsicHelper::CmpgDouble,
                            rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::CMP_LONG:
      ConvertWideComparison(cu, art::llvm::IntrinsicHelper::CmpLong,
                            rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::PACKED_SWITCH:
      ConvertPackedSwitch(cu, bb, vB, rl_src[0]);
      break;

    case Instruction::SPARSE_SWITCH:
      ConvertSparseSwitch(cu, bb, vB, rl_src[0]);
      break;

    default:
      UNIMPLEMENTED(FATAL) << "Unsupported Dex opcode 0x" << std::hex << opcode;
      res = true;
  }
  return res;
}

static void SetDexOffset(CompilationUnit* cu, int32_t offset)
{
  cu->current_dalvik_offset = offset;
  ::llvm::SmallVector< ::llvm::Value*, 1> array_ref;
  array_ref.push_back(cu->irb->getInt32(offset));
  ::llvm::MDNode* node = ::llvm::MDNode::get(*cu->context, array_ref);
  cu->irb->SetDexOffset(node);
}

// Attach method info as metadata to special intrinsic
static void SetMethodInfo(CompilationUnit* cu)
{
  // We don't want dex offset on this
  cu->irb->SetDexOffset(NULL);
  art::llvm::IntrinsicHelper::IntrinsicId id;
  id = art::llvm::IntrinsicHelper::MethodInfo;
  ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  ::llvm::Instruction* inst = cu->irb->CreateCall(intr);
  ::llvm::SmallVector< ::llvm::Value*, 2> reg_info;
  reg_info.push_back(cu->irb->getInt32(cu->num_ins));
  reg_info.push_back(cu->irb->getInt32(cu->num_regs));
  reg_info.push_back(cu->irb->getInt32(cu->num_outs));
  reg_info.push_back(cu->irb->getInt32(cu->num_compiler_temps));
  reg_info.push_back(cu->irb->getInt32(cu->mir_graph->GetNumSSARegs()));
  ::llvm::MDNode* reg_info_node = ::llvm::MDNode::get(*cu->context, reg_info);
  inst->setMetadata("RegInfo", reg_info_node);
  int promo_size = cu->num_dalvik_registers + cu->num_compiler_temps + 1;
  ::llvm::SmallVector< ::llvm::Value*, 50> pmap;
  for (int i = 0; i < promo_size; i++) {
    PromotionMap* p = &cu->promotion_map[i];
    int32_t map_data = ((p->first_in_pair & 0xff) << 24) |
                      ((p->FpReg & 0xff) << 16) |
                      ((p->core_reg & 0xff) << 8) |
                      ((p->fp_location & 0xf) << 4) |
                      (p->core_location & 0xf);
    pmap.push_back(cu->irb->getInt32(map_data));
  }
  ::llvm::MDNode* map_node = ::llvm::MDNode::get(*cu->context, pmap);
  inst->setMetadata("PromotionMap", map_node);
  SetDexOffset(cu, cu->current_dalvik_offset);
}

static void HandlePhiNodes(CompilationUnit* cu, BasicBlock* bb, ::llvm::BasicBlock* llvm_bb)
{
  SetDexOffset(cu, bb->start_offset);
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    int opcode = mir->dalvikInsn.opcode;
    if (opcode < kMirOpFirst) {
      // Stop after first non-pseudo MIR op.
      continue;
    }
    if (opcode != kMirOpPhi) {
      // Skip other mir Pseudos.
      continue;
    }
    RegLocation rl_dest = cu->reg_location[mir->ssa_rep->defs[0]];
    /*
     * The Art compiler's Phi nodes only handle 32-bit operands,
     * representing wide values using a matched set of Phi nodes
     * for the lower and upper halves.  In the llvm world, we only
     * want a single Phi for wides.  Here we will simply discard
     * the Phi node representing the high word.
     */
    if (rl_dest.high_word) {
      continue;  // No Phi node - handled via low word
    }
    int* incoming = reinterpret_cast<int*>(mir->dalvikInsn.vB);
    ::llvm::Type* phi_type =
        LlvmTypeFromLocRec(cu, rl_dest);
    ::llvm::PHINode* phi = cu->irb->CreatePHI(phi_type, mir->ssa_rep->num_uses);
    for (int i = 0; i < mir->ssa_rep->num_uses; i++) {
      RegLocation loc;
      // Don't check width here.
      loc = GetRawSrc(cu, mir, i);
      DCHECK_EQ(rl_dest.wide, loc.wide);
      DCHECK_EQ(rl_dest.wide & rl_dest.high_word, loc.wide & loc.high_word);
      DCHECK_EQ(rl_dest.fp, loc.fp);
      DCHECK_EQ(rl_dest.core, loc.core);
      DCHECK_EQ(rl_dest.ref, loc.ref);
      SafeMap<unsigned int, unsigned int>::iterator it;
      it = cu->block_id_map.find(incoming[i]);
      DCHECK(it != cu->block_id_map.end());
      DCHECK(GetLLVMValue(cu, loc.orig_sreg) != NULL);
      DCHECK(GetLLVMBlock(cu, it->second) != NULL);
      phi->addIncoming(GetLLVMValue(cu, loc.orig_sreg),
                       GetLLVMBlock(cu, it->second));
    }
    DefineValueOnly(cu, phi, rl_dest.orig_sreg);
  }
}

/* Extended MIR instructions like PHI */
static void ConvertExtendedMIR(CompilationUnit* cu, BasicBlock* bb, MIR* mir,
                               ::llvm::BasicBlock* llvm_bb)
{

  switch (static_cast<ExtendedMIROpcode>(mir->dalvikInsn.opcode)) {
    case kMirOpPhi: {
      // The llvm Phi node already emitted - just DefineValue() here.
      RegLocation rl_dest = cu->reg_location[mir->ssa_rep->defs[0]];
      if (!rl_dest.high_word) {
        // Only consider low word of pairs.
        DCHECK(GetLLVMValue(cu, rl_dest.orig_sreg) != NULL);
        ::llvm::Value* phi = GetLLVMValue(cu, rl_dest.orig_sreg);
        if (1) SetVregOnValue(cu, phi, rl_dest.orig_sreg);
      }
      break;
    }
    case kMirOpCopy: {
      UNIMPLEMENTED(WARNING) << "unimp kMirOpPhi";
      break;
    }
    case kMirOpNop:
      if ((mir == bb->last_mir_insn) && (bb->taken == NULL) &&
          (bb->fall_through == NULL)) {
        cu->irb->CreateUnreachable();
      }
      break;

    // TODO: need GBC intrinsic to take advantage of fused operations
    case kMirOpFusedCmplFloat:
      UNIMPLEMENTED(FATAL) << "kMirOpFusedCmpFloat unsupported";
      break;
    case kMirOpFusedCmpgFloat:
      UNIMPLEMENTED(FATAL) << "kMirOpFusedCmgFloat unsupported";
      break;
    case kMirOpFusedCmplDouble:
      UNIMPLEMENTED(FATAL) << "kMirOpFusedCmplDouble unsupported";
      break;
    case kMirOpFusedCmpgDouble:
      UNIMPLEMENTED(FATAL) << "kMirOpFusedCmpgDouble unsupported";
      break;
    case kMirOpFusedCmpLong:
      UNIMPLEMENTED(FATAL) << "kMirOpLongCmpBranch unsupported";
      break;
    default:
      break;
  }
}

/* Handle the content in each basic block */
static bool BlockBitcodeConversion(CompilationUnit* cu, BasicBlock* bb)
{
  if (bb->block_type == kDead) return false;
  ::llvm::BasicBlock* llvm_bb = GetLLVMBlock(cu, bb->id);
  if (llvm_bb == NULL) {
    CHECK(bb->block_type == kExitBlock);
  } else {
    cu->irb->SetInsertPoint(llvm_bb);
    SetDexOffset(cu, bb->start_offset);
  }

  if (cu->verbose) {
    LOG(INFO) << "................................";
    LOG(INFO) << "Block id " << bb->id;
    if (llvm_bb != NULL) {
      LOG(INFO) << "label " << llvm_bb->getName().str().c_str();
    } else {
      LOG(INFO) << "llvm_bb is NULL";
    }
  }

  if (bb->block_type == kEntryBlock) {
    SetMethodInfo(cu);

    { // Allocate shadowframe.
      art::llvm::IntrinsicHelper::IntrinsicId id =
              art::llvm::IntrinsicHelper::AllocaShadowFrame;
      ::llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(id);
      ::llvm::Value* entries = cu->irb->getInt32(cu->num_dalvik_registers);
      cu->irb->CreateCall(func, entries);
    }

    { // Store arguments to vregs.
      uint16_t arg_reg = cu->num_regs;

      ::llvm::Function::arg_iterator arg_iter(cu->func->arg_begin());
      ::llvm::Function::arg_iterator arg_end(cu->func->arg_end());

      const char* shorty = cu->shorty;
      uint32_t shorty_size = strlen(shorty);
      CHECK_GE(shorty_size, 1u);

      ++arg_iter; // skip method object

      if ((cu->access_flags & kAccStatic) == 0) {
        SetVregOnValue(cu, arg_iter, arg_reg);
        ++arg_iter;
        ++arg_reg;
      }

      for (uint32_t i = 1; i < shorty_size; ++i, ++arg_iter) {
        SetVregOnValue(cu, arg_iter, arg_reg);

        ++arg_reg;
        if (shorty[i] == 'J' || shorty[i] == 'D') {
          // Wide types, such as long and double, are using a pair of registers
          // to store the value, so we have to increase arg_reg again.
          ++arg_reg;
        }
      }
    }
  } else if (bb->block_type == kExitBlock) {
    /*
     * Because of the differences between how MIR/LIR and llvm handle exit
     * blocks, we won't explicitly covert them.  On the llvm-to-lir
     * path, it will need to be regenereated.
     */
    return false;
  } else if (bb->block_type == kExceptionHandling) {
    /*
     * Because we're deferring null checking, delete the associated empty
     * exception block.
     */
    llvm_bb->eraseFromParent();
    return false;
  }

  HandlePhiNodes(cu, bb, llvm_bb);

  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {

    SetDexOffset(cu, mir->offset);

    int opcode = mir->dalvikInsn.opcode;
    Instruction::Format dalvik_format =
        Instruction::FormatOf(mir->dalvikInsn.opcode);

    if (opcode == kMirOpCheck) {
      // Combine check and work halves of throwing instruction.
      MIR* work_half = mir->meta.throw_insn;
      mir->dalvikInsn.opcode = work_half->dalvikInsn.opcode;
      opcode = mir->dalvikInsn.opcode;
      SSARepresentation* ssa_rep = work_half->ssa_rep;
      work_half->ssa_rep = mir->ssa_rep;
      mir->ssa_rep = ssa_rep;
      work_half->meta.original_opcode = work_half->dalvikInsn.opcode;
      work_half->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
      if (bb->successor_block_list.block_list_type == kCatch) {
        ::llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(
            art::llvm::IntrinsicHelper::CatchTargets);
        ::llvm::Value* switch_key =
            cu->irb->CreateCall(intr, cu->irb->getInt32(mir->offset));
        GrowableListIterator iter;
        GrowableListIteratorInit(&bb->successor_block_list.blocks, &iter);
        // New basic block to use for work half
        ::llvm::BasicBlock* work_bb =
            ::llvm::BasicBlock::Create(*cu->context, "", cu->func);
        ::llvm::SwitchInst* sw =
            cu->irb->CreateSwitch(switch_key, work_bb,
                                     bb->successor_block_list.blocks.num_used);
        while (true) {
          SuccessorBlockInfo *successor_block_info =
              reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iter));
          if (successor_block_info == NULL) break;
          ::llvm::BasicBlock *target =
              GetLLVMBlock(cu, successor_block_info->block->id);
          int type_index = successor_block_info->key;
          sw->addCase(cu->irb->getInt32(type_index), target);
        }
        llvm_bb = work_bb;
        cu->irb->SetInsertPoint(llvm_bb);
      }
    }

    if (opcode >= kMirOpFirst) {
      ConvertExtendedMIR(cu, bb, mir, llvm_bb);
      continue;
    }

    bool not_handled = ConvertMIRNode(cu, mir, bb, llvm_bb);
    if (not_handled) {
      Instruction::Code dalvik_opcode = static_cast<Instruction::Code>(opcode);
      LOG(WARNING) << StringPrintf("%#06x: Op %#x (%s) / Fmt %d not handled",
                                   mir->offset, opcode,
                                   Instruction::Name(dalvik_opcode),
                                   dalvik_format);
    }
  }

  if (bb->block_type == kEntryBlock) {
    cu->entryTarget_bb = GetLLVMBlock(cu, bb->fall_through->id);
  } else if ((bb->fall_through != NULL) && !bb->terminated_by_return) {
    cu->irb->CreateBr(GetLLVMBlock(cu, bb->fall_through->id));
  }

  return false;
}

char RemapShorty(char shorty_type) {
  /*
   * TODO: might want to revisit this.  Dalvik registers are 32-bits wide,
   * and longs/doubles are represented as a pair of registers.  When sub-word
   * arguments (and method results) are passed, they are extended to Dalvik
   * virtual register containers.  Because llvm is picky about type consistency,
   * we must either cast the "real" type to 32-bit container multiple Dalvik
   * register types, or always use the expanded values.
   * Here, we're doing the latter.  We map the shorty signature to container
   * types (which is valid so long as we always do a real expansion of passed
   * arguments and field loads).
   */
  switch(shorty_type) {
    case 'Z' : shorty_type = 'I'; break;
    case 'B' : shorty_type = 'I'; break;
    case 'S' : shorty_type = 'I'; break;
    case 'C' : shorty_type = 'I'; break;
    default: break;
  }
  return shorty_type;
}

static ::llvm::FunctionType* GetFunctionType(CompilationUnit* cu) {

  // Get return type
  ::llvm::Type* ret_type = cu->irb->getJType(RemapShorty(cu->shorty[0]));

  // Get argument type
  std::vector< ::llvm::Type*> args_type;

  // method object
  args_type.push_back(cu->irb->getJMethodTy());

  // Do we have  a "this"?
  if ((cu->access_flags & kAccStatic) == 0) {
    args_type.push_back(cu->irb->getJObjectTy());
  }

  for (uint32_t i = 1; i < strlen(cu->shorty); ++i) {
    args_type.push_back(cu->irb->getJType(RemapShorty(cu->shorty[i])));
  }

  return ::llvm::FunctionType::get(ret_type, args_type, false);
}

static bool CreateFunction(CompilationUnit* cu) {
  ::llvm::FunctionType* func_type = GetFunctionType(cu);
  if (func_type == NULL) {
    return false;
  }

  cu->func = ::llvm::Function::Create(func_type,
                                      ::llvm::Function::InternalLinkage,
                                      cu->symbol, cu->module);

  ::llvm::Function::arg_iterator arg_iter(cu->func->arg_begin());
  ::llvm::Function::arg_iterator arg_end(cu->func->arg_end());

  arg_iter->setName("method");
  ++arg_iter;

  int start_sreg = cu->num_regs;

  for (unsigned i = 0; arg_iter != arg_end; ++i, ++arg_iter) {
    arg_iter->setName(StringPrintf("v%i_0", start_sreg));
    start_sreg += cu->reg_location[start_sreg].wide ? 2 : 1;
  }

  return true;
}

static bool CreateLLVMBasicBlock(CompilationUnit* cu, BasicBlock* bb)
{
  // Skip the exit block
  if ((bb->block_type == kDead) ||(bb->block_type == kExitBlock)) {
    cu->id_to_block_map.Put(bb->id, NULL);
  } else {
    int offset = bb->start_offset;
    bool entry_block = (bb->block_type == kEntryBlock);
    ::llvm::BasicBlock* llvm_bb =
        ::llvm::BasicBlock::Create(*cu->context, entry_block ? "entry" :
                                 StringPrintf(kLabelFormat, bb->catch_entry ? kCatchBlock :
                                              kNormalBlock, offset, bb->id), cu->func);
    if (entry_block) {
        cu->entry_bb = llvm_bb;
        cu->placeholder_bb =
            ::llvm::BasicBlock::Create(*cu->context, "placeholder",
                                     cu->func);
    }
    cu->id_to_block_map.Put(bb->id, llvm_bb);
  }
  return false;
}


/*
 * Convert MIR to LLVM_IR
 *  o For each ssa name, create LLVM named value.  Type these
 *    appropriately, and ignore high half of wide and double operands.
 *  o For each MIR basic block, create an LLVM basic block.
 *  o Iterate through the MIR a basic block at a time, setting arguments
 *    to recovered ssa name.
 */
void MethodMIR2Bitcode(CompilationUnit* cu)
{
  InitIR(cu);
  CompilerInitGrowableList(cu, &cu->llvm_values, cu->mir_graph->GetNumSSARegs());

  // Create the function
  CreateFunction(cu);

  // Create an LLVM basic block for each MIR block in dfs preorder
  PreOrderDfsIterator iter(cu->mir_graph.get(), false /* not iterative */);
  for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
    CreateLLVMBasicBlock(cu, bb);
  }

  /*
   * Create an llvm named value for each MIR SSA name.  Note: we'll use
   * placeholders for all non-argument values (because we haven't seen
   * the definition yet).
   */
  cu->irb->SetInsertPoint(cu->placeholder_bb);
  ::llvm::Function::arg_iterator arg_iter(cu->func->arg_begin());
  arg_iter++;  /* Skip path method */
  for (int i = 0; i < cu->mir_graph->GetNumSSARegs(); i++) {
    ::llvm::Value* val;
    RegLocation rl_temp = cu->reg_location[i];
    if ((cu->mir_graph->SRegToVReg(i) < 0) || rl_temp.high_word) {
      InsertGrowableList(cu, &cu->llvm_values, 0);
    } else if ((i < cu->num_regs) ||
               (i >= (cu->num_regs + cu->num_ins))) {
      ::llvm::Constant* imm_value = cu->reg_location[i].wide ?
         cu->irb->getJLong(0) : cu->irb->getJInt(0);
      val = EmitConst(cu, imm_value, cu->reg_location[i]);
      val->setName(cu->mir_graph->GetSSAString(i));
      InsertGrowableList(cu, &cu->llvm_values, reinterpret_cast<uintptr_t>(val));
    } else {
      // Recover previously-created argument values
      ::llvm::Value* arg_val = arg_iter++;
      InsertGrowableList(cu, &cu->llvm_values, reinterpret_cast<uintptr_t>(arg_val));
    }
  }

  PreOrderDfsIterator iter2(cu->mir_graph.get(), false /* not iterative */);
  for (BasicBlock* bb = iter2.Next(); bb != NULL; bb = iter2.Next()) {
    BlockBitcodeConversion(cu, bb);
  }

  /*
   * In a few rare cases of verification failure, the verifier will
   * replace one or more Dalvik opcodes with the special
   * throw-verification-failure opcode.  This can leave the SSA graph
   * in an invalid state, as definitions may be lost, while uses retained.
   * To work around this problem, we insert placeholder definitions for
   * all Dalvik SSA regs in the "placeholder" block.  Here, after
   * bitcode conversion is complete, we examine those placeholder definitions
   * and delete any with no references (which normally is all of them).
   *
   * If any definitions remain, we link the placeholder block into the
   * CFG.  Otherwise, it is deleted.
   */
  for (::llvm::BasicBlock::iterator it = cu->placeholder_bb->begin(),
       it_end = cu->placeholder_bb->end(); it != it_end;) {
    ::llvm::Instruction* inst = ::llvm::dyn_cast< ::llvm::Instruction>(it++);
    DCHECK(inst != NULL);
    ::llvm::Value* val = ::llvm::dyn_cast< ::llvm::Value>(inst);
    DCHECK(val != NULL);
    if (val->getNumUses() == 0) {
      inst->eraseFromParent();
    }
  }
  SetDexOffset(cu, 0);
  if (cu->placeholder_bb->empty()) {
    cu->placeholder_bb->eraseFromParent();
  } else {
    cu->irb->SetInsertPoint(cu->placeholder_bb);
    cu->irb->CreateBr(cu->entryTarget_bb);
    cu->entryTarget_bb = cu->placeholder_bb;
  }
  cu->irb->SetInsertPoint(cu->entry_bb);
  cu->irb->CreateBr(cu->entryTarget_bb);

  if (cu->enable_debug & (1 << kDebugVerifyBitcode)) {
     if (::llvm::verifyFunction(*cu->func, ::llvm::PrintMessageAction)) {
       LOG(INFO) << "Bitcode verification FAILED for "
                 << PrettyMethod(cu->method_idx, *cu->dex_file)
                 << " of size " << cu->code_item->insns_size_in_code_units_;
       cu->enable_debug |= (1 << kDebugDumpBitcodeFile);
     }
  }

  if (cu->enable_debug & (1 << kDebugDumpBitcodeFile)) {
    // Write bitcode to file
    std::string errmsg;
    std::string fname(PrettyMethod(cu->method_idx, *cu->dex_file));
    ReplaceSpecialChars(fname);
    // TODO: make configurable change naming mechanism to avoid fname length issues.
    fname = StringPrintf("/sdcard/Bitcode/%s.bc", fname.c_str());

    if (fname.size() > 240) {
      LOG(INFO) << "Warning: bitcode filename too long. Truncated.";
      fname.resize(240);
    }

    ::llvm::OwningPtr< ::llvm::tool_output_file> out_file(
        new ::llvm::tool_output_file(fname.c_str(), errmsg,
                                   ::llvm::raw_fd_ostream::F_Binary));

    if (!errmsg.empty()) {
      LOG(ERROR) << "Failed to create bitcode output file: " << errmsg;
    }

    ::llvm::WriteBitcodeToFile(cu->module, out_file->os());
    out_file->keep();
  }
}

}  // namespace art
