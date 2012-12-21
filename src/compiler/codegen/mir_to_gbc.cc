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

#include "../compiler_internals.h"
#include "local_optimizations.h"
#include "codegen_util.h"
#include "ralloc_util.h"

static const char* kLabelFormat = "%c0x%x_%d";
static const char kInvalidBlock = 0xff;
static const char kNormalBlock = 'L';
static const char kCatchBlock = 'C';

namespace art {
static RegLocation GetLoc(CompilationUnit* cu, llvm::Value* val);

static llvm::BasicBlock* GetLLVMBlock(CompilationUnit* cu, int id)
{
  return cu->id_to_block_map.Get(id);
}

static llvm::Value* GetLLVMValue(CompilationUnit* cu, int s_reg)
{
  return reinterpret_cast<llvm::Value*>(GrowableListGetElement(&cu->llvm_values, s_reg));
}

static void SetVregOnValue(CompilationUnit* cu, llvm::Value* val, int s_reg)
{
  // Set vreg for debugging
  if (cu->compiler->IsDebuggingSupported()) {
    greenland::IntrinsicHelper::IntrinsicId id =
        greenland::IntrinsicHelper::SetVReg;
    llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(id);
    int v_reg = SRegToVReg(cu, s_reg);
    llvm::Value* table_slot = cu->irb->getInt32(v_reg);
    llvm::Value* args[] = { table_slot, val };
    cu->irb->CreateCall(func, args);
  }
}

// Replace the placeholder value with the real definition
static void DefineValueOnly(CompilationUnit* cu, llvm::Value* val, int s_reg)
{
  llvm::Value* placeholder = GetLLVMValue(cu, s_reg);
  if (placeholder == NULL) {
    // This can happen on instruction rewrite on verification failure
    LOG(WARNING) << "Null placeholder";
    return;
  }
  placeholder->replaceAllUsesWith(val);
  val->takeName(placeholder);
  cu->llvm_values.elem_list[s_reg] = reinterpret_cast<uintptr_t>(val);
  llvm::Instruction* inst = llvm::dyn_cast<llvm::Instruction>(placeholder);
  DCHECK(inst != NULL);
  inst->eraseFromParent();

}

static void DefineValue(CompilationUnit* cu, llvm::Value* val, int s_reg)
{
  DefineValueOnly(cu, val, s_reg);
  SetVregOnValue(cu, val, s_reg);
}

static llvm::Type* LlvmTypeFromLocRec(CompilationUnit* cu, RegLocation loc)
{
  llvm::Type* res = NULL;
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
        res = cu->irb->GetJObjectTy();
      else
        res = cu->irb->getInt32Ty();
    }
  }
  return res;
}

/* Create an in-memory RegLocation from an llvm Value. */
static void CreateLocFromValue(CompilationUnit* cu, llvm::Value* val)
{
  // NOTE: llvm takes shortcuts with c_str() - get to std::string firstt
  std::string s(val->getName().str());
  const char* val_name = s.c_str();
  SafeMap<llvm::Value*, RegLocation>::iterator it = cu->loc_map.find(val);
  DCHECK(it == cu->loc_map.end()) << " - already defined: " << val_name;
  int base_sreg = INVALID_SREG;
  int subscript = -1;
  sscanf(val_name, "v%d_%d", &base_sreg, &subscript);
  if ((base_sreg == INVALID_SREG) && (!strcmp(val_name, "method"))) {
    base_sreg = SSA_METHOD_BASEREG;
    subscript = 0;
  }
  DCHECK_NE(base_sreg, INVALID_SREG);
  DCHECK_NE(subscript, -1);
  // TODO: redo during C++'ification
  RegLocation loc =  {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0, INVALID_REG,
                      INVALID_REG, INVALID_SREG, INVALID_SREG};
  llvm::Type* ty = val->getType();
  loc.wide = ((ty == cu->irb->getInt64Ty()) ||
              (ty == cu->irb->getDoubleTy()));
  loc.defined = true;
  loc.home = false;  // May change during promotion
  loc.s_reg_low = base_sreg;
  loc.orig_sreg = cu->loc_map.size();
  PromotionMap p_map = cu->promotion_map[base_sreg];
  if (ty == cu->irb->getFloatTy()) {
    loc.fp = true;
    if (p_map.fp_location == kLocPhysReg) {
      loc.low_reg = p_map.FpReg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  } else if (ty == cu->irb->getDoubleTy()) {
    loc.fp = true;
    PromotionMap p_map_high = cu->promotion_map[base_sreg + 1];
    if ((p_map.fp_location == kLocPhysReg) &&
        (p_map_high.fp_location == kLocPhysReg) &&
        ((p_map.FpReg & 0x1) == 0) &&
        (p_map.FpReg + 1 == p_map_high.FpReg)) {
      loc.low_reg = p_map.FpReg;
      loc.high_reg = p_map_high.FpReg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  } else if (ty == cu->irb->GetJObjectTy()) {
    loc.ref = true;
    if (p_map.core_location == kLocPhysReg) {
      loc.low_reg = p_map.core_reg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  } else if (ty == cu->irb->getInt64Ty()) {
    loc.core = true;
    PromotionMap p_map_high = cu->promotion_map[base_sreg + 1];
    if ((p_map.core_location == kLocPhysReg) &&
        (p_map_high.core_location == kLocPhysReg)) {
      loc.low_reg = p_map.core_reg;
      loc.high_reg = p_map_high.core_reg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  } else {
    loc.core = true;
    if (p_map.core_location == kLocPhysReg) {
      loc.low_reg = p_map.core_reg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  }

  if (cu->verbose && loc.home) {
    if (loc.wide) {
      LOG(INFO) << "Promoted wide " << s << " to regs " << loc.low_reg << "/" << loc.high_reg;
    } else {
      LOG(INFO) << "Promoted " << s << " to reg " << loc.low_reg;
    }
  }
  cu->loc_map.Put(val, loc);
}

static void InitIR(CompilationUnit* cu)
{
  LLVMInfo* llvm_info = cu->llvm_info;
  if (llvm_info == NULL) {
    CompilerTls* tls = cu->compiler->GetTls();
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

static const char* LlvmSSAName(CompilationUnit* cu, int ssa_reg) {
  return GET_ELEM_N(cu->ssa_strings, char*, ssa_reg);
}

llvm::BasicBlock* FindCaseTarget(CompilationUnit* cu, uint32_t vaddr)
{
  BasicBlock* bb = FindBlock(cu, vaddr);
  DCHECK(bb != NULL);
  return GetLLVMBlock(cu, bb->id);
}

static void ConvertPackedSwitch(CompilationUnit* cu, BasicBlock* bb,
                                int32_t table_offset, RegLocation rl_src)
{
  const Instruction::PackedSwitchPayload* payload =
      reinterpret_cast<const Instruction::PackedSwitchPayload*>(
      cu->insns + cu->current_dalvik_offset + table_offset);

  llvm::Value* value = GetLLVMValue(cu, rl_src.orig_sreg);

  llvm::SwitchInst* sw =
    cu->irb->CreateSwitch(value, GetLLVMBlock(cu, bb->fall_through->id),
                             payload->case_count);

  for (uint16_t i = 0; i < payload->case_count; ++i) {
    llvm::BasicBlock* llvm_bb =
        FindCaseTarget(cu, cu->current_dalvik_offset + payload->targets[i]);
    sw->addCase(cu->irb->getInt32(payload->first_key + i), llvm_bb);
  }
  llvm::MDNode* switch_node =
      llvm::MDNode::get(*cu->context, cu->irb->getInt32(table_offset));
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

  llvm::Value* value = GetLLVMValue(cu, rl_src.orig_sreg);

  llvm::SwitchInst* sw =
    cu->irb->CreateSwitch(value, GetLLVMBlock(cu, bb->fall_through->id),
                             payload->case_count);

  for (size_t i = 0; i < payload->case_count; ++i) {
    llvm::BasicBlock* llvm_bb =
        FindCaseTarget(cu, cu->current_dalvik_offset + targets[i]);
    sw->addCase(cu->irb->getInt32(keys[i]), llvm_bb);
  }
  llvm::MDNode* switch_node =
      llvm::MDNode::get(*cu->context, cu->irb->getInt32(table_offset));
  sw->setMetadata("SwitchTable", switch_node);
  bb->taken = NULL;
  bb->fall_through = NULL;
}

static void ConvertSget(CompilationUnit* cu, int32_t field_index,
                        greenland::IntrinsicHelper::IntrinsicId id, RegLocation rl_dest)
{
  llvm::Constant* field_idx = cu->irb->getInt32(field_index);
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cu->irb->CreateCall(intr, field_idx);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertSput(CompilationUnit* cu, int32_t field_index,
                        greenland::IntrinsicHelper::IntrinsicId id, RegLocation rl_src)
{
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(field_index));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(intr, args);
}

static void ConvertFillArrayData(CompilationUnit* cu, int32_t offset, RegLocation rl_array)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::HLFillArrayData;
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(offset));
  args.push_back(GetLLVMValue(cu, rl_array.orig_sreg));
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(intr, args);
}

static llvm::Value* EmitConst(CompilationUnit* cu, llvm::ArrayRef<llvm::Value*> src,
                              RegLocation loc)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  if (loc.wide) {
    if (loc.fp) {
      id = greenland::IntrinsicHelper::ConstDouble;
    } else {
      id = greenland::IntrinsicHelper::ConstLong;
    }
  } else {
    if (loc.fp) {
      id = greenland::IntrinsicHelper::ConstFloat;
    } else if (loc.ref) {
      id = greenland::IntrinsicHelper::ConstObj;
    } else {
      id = greenland::IntrinsicHelper::ConstInt;
    }
  }
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  return cu->irb->CreateCall(intr, src);
}

static void EmitPopShadowFrame(CompilationUnit* cu)
{
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::PopShadowFrame);
  cu->irb->CreateCall(intr);
}

static llvm::Value* EmitCopy(CompilationUnit* cu, llvm::ArrayRef<llvm::Value*> src,
                             RegLocation loc)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  if (loc.wide) {
    if (loc.fp) {
      id = greenland::IntrinsicHelper::CopyDouble;
    } else {
      id = greenland::IntrinsicHelper::CopyLong;
    }
  } else {
    if (loc.fp) {
      id = greenland::IntrinsicHelper::CopyFloat;
    } else if (loc.ref) {
      id = greenland::IntrinsicHelper::CopyObj;
    } else {
      id = greenland::IntrinsicHelper::CopyInt;
    }
  }
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  return cu->irb->CreateCall(intr, src);
}

static void ConvertMoveException(CompilationUnit* cu, RegLocation rl_dest)
{
  llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::GetException);
  llvm::Value* res = cu->irb->CreateCall(func);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertThrow(CompilationUnit* cu, RegLocation rl_src)
{
  llvm::Value* src = GetLLVMValue(cu, rl_src.orig_sreg);
  llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::HLThrowException);
  cu->irb->CreateCall(func, src);
}

static void ConvertMonitorEnterExit(CompilationUnit* cu, int opt_flags,
                                    greenland::IntrinsicHelper::IntrinsicId id,
                                    RegLocation rl_src)
{
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(func, args);
}

static void ConvertArrayLength(CompilationUnit* cu, int opt_flags,
                               RegLocation rl_dest, RegLocation rl_src)
{
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::OptArrayLength);
  llvm::Value* res = cu->irb->CreateCall(func, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void EmitSuspendCheck(CompilationUnit* cu)
{
  greenland::IntrinsicHelper::IntrinsicId id =
      greenland::IntrinsicHelper::CheckSuspend;
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(intr);
}

static llvm::Value* ConvertCompare(CompilationUnit* cu, ConditionCode cc,
                                   llvm::Value* src1, llvm::Value* src2)
{
  llvm::Value* res = NULL;
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
  llvm::Value* src1 = GetLLVMValue(cu, rl_src1.orig_sreg);
  llvm::Value* src2 = GetLLVMValue(cu, rl_src2.orig_sreg);
  llvm::Value* cond_value = ConvertCompare(cu, cc, src1, src2);
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
  llvm::Value* src1 = GetLLVMValue(cu, rl_src1.orig_sreg);
  llvm::Value* src2;
  if (rl_src1.ref) {
    src2 = cu->irb->GetJNull();
  } else {
    src2 = cu->irb->getInt32(0);
  }
  llvm::Value* cond_value = ConvertCompare(cu, cc, src1, src2);
  cu->irb->CreateCondBr(cond_value, GetLLVMBlock(cu, bb->taken->id),
                           GetLLVMBlock(cu, bb->fall_through->id));
  // Don't redo the fallthrough branch in the BB driver
  bb->fall_through = NULL;
}

static llvm::Value* GenDivModOp(CompilationUnit* cu, bool is_div, bool is_long,
                                llvm::Value* src1, llvm::Value* src2)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  if (is_long) {
    if (is_div) {
      id = greenland::IntrinsicHelper::DivLong;
    } else {
      id = greenland::IntrinsicHelper::RemLong;
    }
  } else {
    if (is_div) {
      id = greenland::IntrinsicHelper::DivInt;
    } else {
      id = greenland::IntrinsicHelper::RemInt;
    }
  }
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2>args;
  args.push_back(src1);
  args.push_back(src2);
  return cu->irb->CreateCall(intr, args);
}

static llvm::Value* GenArithOp(CompilationUnit* cu, OpKind op, bool is_long,
                               llvm::Value* src1, llvm::Value* src2)
{
  llvm::Value* res = NULL;
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
  llvm::Value* src1 = GetLLVMValue(cu, rl_src1.orig_sreg);
  llvm::Value* src2 = GetLLVMValue(cu, rl_src2.orig_sreg);
  llvm::Value* res = NULL;
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

static void ConvertShift(CompilationUnit* cu, greenland::IntrinsicHelper::IntrinsicId id,
                         RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2)
{
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2>args;
  args.push_back(GetLLVMValue(cu, rl_src1.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_src2.orig_sreg));
  llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertShiftLit(CompilationUnit* cu, greenland::IntrinsicHelper::IntrinsicId id,
                            RegLocation rl_dest, RegLocation rl_src, int shift_amount)
{
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2>args;
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  args.push_back(cu->irb->getInt32(shift_amount));
  llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertArithOp(CompilationUnit* cu, OpKind op, RegLocation rl_dest,
                           RegLocation rl_src1, RegLocation rl_src2)
{
  llvm::Value* src1 = GetLLVMValue(cu, rl_src1.orig_sreg);
  llvm::Value* src2 = GetLLVMValue(cu, rl_src2.orig_sreg);
  DCHECK_EQ(src1->getType(), src2->getType());
  llvm::Value* res = GenArithOp(cu, op, rl_dest.wide, src1, src2);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertArithOpLit(CompilationUnit* cu, OpKind op, RegLocation rl_dest,
                              RegLocation rl_src1, int32_t imm)
{
  llvm::Value* src1 = GetLLVMValue(cu, rl_src1.orig_sreg);
  llvm::Value* src2 = cu->irb->getInt32(imm);
  llvm::Value* res = GenArithOp(cu, op, rl_dest.wide, src1, src2);
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
  llvm::SmallVector<llvm::Value*, 10> args;
  // Insert the invoke_type
  args.push_back(cu->irb->getInt32(static_cast<int>(invoke_type)));
  // Insert the method_idx
  args.push_back(cu->irb->getInt32(info->index));
  // Insert the optimization flags
  args.push_back(cu->irb->getInt32(info->opt_flags));
  // Now, insert the actual arguments
  for (int i = 0; i < info->num_arg_words;) {
    llvm::Value* val = GetLLVMValue(cu, info->args[i].orig_sreg);
    args.push_back(val);
    i += info->args[i].wide ? 2 : 1;
  }
  /*
   * Choose the invoke return type based on actual usage.  Note: may
   * be different than shorty.  For example, if a function return value
   * is not used, we'll treat this as a void invoke.
   */
  greenland::IntrinsicHelper::IntrinsicId id;
  if (is_filled_new_array) {
    id = greenland::IntrinsicHelper::HLFilledNewArray;
  } else if (info->result.location == kLocInvalid) {
    id = greenland::IntrinsicHelper::HLInvokeVoid;
  } else {
    if (info->result.wide) {
      if (info->result.fp) {
        id = greenland::IntrinsicHelper::HLInvokeDouble;
      } else {
        id = greenland::IntrinsicHelper::HLInvokeLong;
      }
    } else if (info->result.ref) {
        id = greenland::IntrinsicHelper::HLInvokeObj;
    } else if (info->result.fp) {
        id = greenland::IntrinsicHelper::HLInvokeFloat;
    } else {
        id = greenland::IntrinsicHelper::HLInvokeInt;
    }
  }
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cu->irb->CreateCall(intr, args);
  if (info->result.location != kLocInvalid) {
    DefineValue(cu, res, info->result.orig_sreg);
  }
}

static void ConvertConstObject(CompilationUnit* cu, uint32_t idx,
                               greenland::IntrinsicHelper::IntrinsicId id, RegLocation rl_dest)
{
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* index = cu->irb->getInt32(idx);
  llvm::Value* res = cu->irb->CreateCall(intr, index);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertCheckCast(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_src)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::HLCheckCast;
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(type_idx));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  cu->irb->CreateCall(intr, args);
}

static void ConvertNewInstance(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::NewInstance;
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* index = cu->irb->getInt32(type_idx);
  llvm::Value* res = cu->irb->CreateCall(intr, index);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertNewArray(CompilationUnit* cu, uint32_t type_idx,
                            RegLocation rl_dest, RegLocation rl_src)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::NewArray;
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(type_idx));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertAget(CompilationUnit* cu, int opt_flags,
                        greenland::IntrinsicHelper::IntrinsicId id,
                        RegLocation rl_dest, RegLocation rl_array, RegLocation rl_index)
{
  llvm::SmallVector<llvm::Value*, 3> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_array.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_index.orig_sreg));
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertAput(CompilationUnit* cu, int opt_flags,
                        greenland::IntrinsicHelper::IntrinsicId id,
                        RegLocation rl_src, RegLocation rl_array, RegLocation rl_index)
{
  llvm::SmallVector<llvm::Value*, 4> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_array.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_index.orig_sreg));
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(intr, args);
}

static void ConvertIget(CompilationUnit* cu, int opt_flags,
                        greenland::IntrinsicHelper::IntrinsicId id,
                        RegLocation rl_dest, RegLocation rl_obj, int field_index)
{
  llvm::SmallVector<llvm::Value*, 3> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_obj.orig_sreg));
  args.push_back(cu->irb->getInt32(field_index));
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertIput(CompilationUnit* cu, int opt_flags,
                        greenland::IntrinsicHelper::IntrinsicId id,
                        RegLocation rl_src, RegLocation rl_obj, int field_index)
{
  llvm::SmallVector<llvm::Value*, 4> args;
  args.push_back(cu->irb->getInt32(opt_flags));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_obj.orig_sreg));
  args.push_back(cu->irb->getInt32(field_index));
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  cu->irb->CreateCall(intr, args);
}

static void ConvertInstanceOf(CompilationUnit* cu, uint32_t type_idx,
                              RegLocation rl_dest, RegLocation rl_src)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::InstanceOf;
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cu->irb->getInt32(type_idx));
  args.push_back(GetLLVMValue(cu, rl_src.orig_sreg));
  llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertIntToLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  llvm::Value* res = cu->irb->CreateSExt(GetLLVMValue(cu, rl_src.orig_sreg),
                                            cu->irb->getInt64Ty());
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertLongToInt(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  llvm::Value* src = GetLLVMValue(cu, rl_src.orig_sreg);
  llvm::Value* res = cu->irb->CreateTrunc(src, cu->irb->getInt32Ty());
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertFloatToDouble(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  llvm::Value* src = GetLLVMValue(cu, rl_src.orig_sreg);
  llvm::Value* res = cu->irb->CreateFPExt(src, cu->irb->getDoubleTy());
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertDoubleToFloat(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  llvm::Value* src = GetLLVMValue(cu, rl_src.orig_sreg);
  llvm::Value* res = cu->irb->CreateFPTrunc(src, cu->irb->getFloatTy());
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertWideComparison(CompilationUnit* cu,
                                  greenland::IntrinsicHelper::IntrinsicId id,
                                  RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_src2)
{
  DCHECK_EQ(rl_src1.fp, rl_src2.fp);
  DCHECK_EQ(rl_src1.wide, rl_src2.wide);
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(GetLLVMValue(cu, rl_src1.orig_sreg));
  args.push_back(GetLLVMValue(cu, rl_src2.orig_sreg));
  llvm::Value* res = cu->irb->CreateCall(intr, args);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertIntNarrowing(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src,
                                greenland::IntrinsicHelper::IntrinsicId id)
{
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res =
      cu->irb->CreateCall(intr, GetLLVMValue(cu, rl_src.orig_sreg));
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertNeg(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  llvm::Value* res = cu->irb->CreateNeg(GetLLVMValue(cu, rl_src.orig_sreg));
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertIntToFP(CompilationUnit* cu, llvm::Type* ty, RegLocation rl_dest,
                           RegLocation rl_src)
{
  llvm::Value* res =
      cu->irb->CreateSIToFP(GetLLVMValue(cu, rl_src.orig_sreg), ty);
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertFPToInt(CompilationUnit* cu, greenland::IntrinsicHelper::IntrinsicId id,
                           RegLocation rl_dest,
                    RegLocation rl_src)
{
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cu->irb->CreateCall(intr, GetLLVMValue(cu, rl_src.orig_sreg));
  DefineValue(cu, res, rl_dest.orig_sreg);
}


static void ConvertNegFP(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  llvm::Value* res =
      cu->irb->CreateFNeg(GetLLVMValue(cu, rl_src.orig_sreg));
  DefineValue(cu, res, rl_dest.orig_sreg);
}

static void ConvertNot(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  llvm::Value* src = GetLLVMValue(cu, rl_src.orig_sreg);
  llvm::Value* res = cu->irb->CreateXor(src, static_cast<uint64_t>(-1));
  DefineValue(cu, res, rl_dest.orig_sreg);
}

/*
 * Target-independent code generation.  Use only high-level
 * load/store utilities here, or target-dependent genXX() handlers
 * when necessary.
 */
static bool ConvertMIRNode(CompilationUnit* cu, MIR* mir, BasicBlock* bb,
                           llvm::BasicBlock* llvm_bb, LIR* label_list)
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
        llvm::Value* src = GetLLVMValue(cu, rl_src[0].orig_sreg);
        llvm::Value* res = EmitCopy(cu, src, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;

    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16: {
        llvm::Constant* imm_value = cu->irb->GetJInt(vB);
        llvm::Value* res = EmitConst(cu, imm_value, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;

    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32: {
        // Sign extend to 64 bits
        int64_t imm = static_cast<int32_t>(vB);
        llvm::Constant* imm_value = cu->irb->GetJLong(imm);
        llvm::Value* res = EmitConst(cu, imm_value, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;

    case Instruction::CONST_HIGH16: {
        llvm::Constant* imm_value = cu->irb->GetJInt(vB << 16);
        llvm::Value* res = EmitConst(cu, imm_value, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;

    case Instruction::CONST_WIDE: {
        llvm::Constant* imm_value =
            cu->irb->GetJLong(mir->dalvikInsn.vB_wide);
        llvm::Value* res = EmitConst(cu, imm_value, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;
    case Instruction::CONST_WIDE_HIGH16: {
        int64_t imm = static_cast<int64_t>(vB) << 48;
        llvm::Constant* imm_value = cu->irb->GetJLong(imm);
        llvm::Value* res = EmitConst(cu, imm_value, rl_dest);
        DefineValue(cu, res, rl_dest.orig_sreg);
      }
      break;

    case Instruction::SPUT_OBJECT:
      ConvertSput(cu, vB, greenland::IntrinsicHelper::HLSputObject,
                  rl_src[0]);
      break;
    case Instruction::SPUT:
      if (rl_src[0].fp) {
        ConvertSput(cu, vB, greenland::IntrinsicHelper::HLSputFloat,
                    rl_src[0]);
      } else {
        ConvertSput(cu, vB, greenland::IntrinsicHelper::HLSput, rl_src[0]);
      }
      break;
    case Instruction::SPUT_BOOLEAN:
      ConvertSput(cu, vB, greenland::IntrinsicHelper::HLSputBoolean,
                  rl_src[0]);
      break;
    case Instruction::SPUT_BYTE:
      ConvertSput(cu, vB, greenland::IntrinsicHelper::HLSputByte, rl_src[0]);
      break;
    case Instruction::SPUT_CHAR:
      ConvertSput(cu, vB, greenland::IntrinsicHelper::HLSputChar, rl_src[0]);
      break;
    case Instruction::SPUT_SHORT:
      ConvertSput(cu, vB, greenland::IntrinsicHelper::HLSputShort, rl_src[0]);
      break;
    case Instruction::SPUT_WIDE:
      if (rl_src[0].fp) {
        ConvertSput(cu, vB, greenland::IntrinsicHelper::HLSputDouble,
                    rl_src[0]);
      } else {
        ConvertSput(cu, vB, greenland::IntrinsicHelper::HLSputWide,
                    rl_src[0]);
      }
      break;

    case Instruction::SGET_OBJECT:
      ConvertSget(cu, vB, greenland::IntrinsicHelper::HLSgetObject, rl_dest);
      break;
    case Instruction::SGET:
      if (rl_dest.fp) {
        ConvertSget(cu, vB, greenland::IntrinsicHelper::HLSgetFloat, rl_dest);
      } else {
        ConvertSget(cu, vB, greenland::IntrinsicHelper::HLSget, rl_dest);
      }
      break;
    case Instruction::SGET_BOOLEAN:
      ConvertSget(cu, vB, greenland::IntrinsicHelper::HLSgetBoolean, rl_dest);
      break;
    case Instruction::SGET_BYTE:
      ConvertSget(cu, vB, greenland::IntrinsicHelper::HLSgetByte, rl_dest);
      break;
    case Instruction::SGET_CHAR:
      ConvertSget(cu, vB, greenland::IntrinsicHelper::HLSgetChar, rl_dest);
      break;
    case Instruction::SGET_SHORT:
      ConvertSget(cu, vB, greenland::IntrinsicHelper::HLSgetShort, rl_dest);
      break;
    case Instruction::SGET_WIDE:
      if (rl_dest.fp) {
        ConvertSget(cu, vB, greenland::IntrinsicHelper::HLSgetDouble,
                    rl_dest);
      } else {
        ConvertSget(cu, vB, greenland::IntrinsicHelper::HLSgetWide, rl_dest);
      }
      break;

    case Instruction::RETURN_WIDE:
    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT: {
        if (!(cu->attrs & METHOD_IS_LEAF)) {
          EmitSuspendCheck(cu);
        }
        EmitPopShadowFrame(cu);
        cu->irb->CreateRet(GetLLVMValue(cu, rl_src[0].orig_sreg));
        bb->has_return = true;
      }
      break;

    case Instruction::RETURN_VOID: {
        if (!(cu->attrs & METHOD_IS_LEAF)) {
          EmitSuspendCheck(cu);
        }
        EmitPopShadowFrame(cu);
        cu->irb->CreateRetVoid();
        bb->has_return = true;
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
      ConvertShift(cu, greenland::IntrinsicHelper::SHLLong,
                    rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      ConvertShift(cu, greenland::IntrinsicHelper::SHLInt,
                   rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      ConvertShift(cu, greenland::IntrinsicHelper::SHRLong,
                   rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      ConvertShift(cu, greenland::IntrinsicHelper::SHRInt,
                   rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      ConvertShift(cu, greenland::IntrinsicHelper::USHRLong,
                   rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      ConvertShift(cu, greenland::IntrinsicHelper::USHRInt,
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
      ConvertShiftLit(cu, greenland::IntrinsicHelper::SHLInt,
                      rl_dest, rl_src[0], vC & 0x1f);
      break;
    case Instruction::SHR_INT_LIT8:
      ConvertShiftLit(cu, greenland::IntrinsicHelper::SHRInt,
                      rl_dest, rl_src[0], vC & 0x1f);
      break;
    case Instruction::USHR_INT_LIT8:
      ConvertShiftLit(cu, greenland::IntrinsicHelper::USHRInt,
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
      ConvertConstObject(cu, vB, greenland::IntrinsicHelper::ConstString,
                         rl_dest);
      break;

    case Instruction::CONST_CLASS:
      ConvertConstObject(cu, vB, greenland::IntrinsicHelper::ConstClass,
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
                              greenland::IntrinsicHelper::MonitorEnter,
                              rl_src[0]);
      break;

    case Instruction::MONITOR_EXIT:
      ConvertMonitorEnterExit(cu, opt_flags,
                              greenland::IntrinsicHelper::MonitorExit,
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
                    greenland::IntrinsicHelper::HLArrayGetFloat,
                    rl_dest, rl_src[0], rl_src[1]);
      } else {
        ConvertAget(cu, opt_flags, greenland::IntrinsicHelper::HLArrayGet,
                    rl_dest, rl_src[0], rl_src[1]);
      }
      break;
    case Instruction::AGET_OBJECT:
      ConvertAget(cu, opt_flags, greenland::IntrinsicHelper::HLArrayGetObject,
                  rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AGET_BOOLEAN:
      ConvertAget(cu, opt_flags,
                  greenland::IntrinsicHelper::HLArrayGetBoolean,
                  rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AGET_BYTE:
      ConvertAget(cu, opt_flags, greenland::IntrinsicHelper::HLArrayGetByte,
                  rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AGET_CHAR:
      ConvertAget(cu, opt_flags, greenland::IntrinsicHelper::HLArrayGetChar,
                  rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AGET_SHORT:
      ConvertAget(cu, opt_flags, greenland::IntrinsicHelper::HLArrayGetShort,
                  rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::AGET_WIDE:
      if (rl_dest.fp) {
        ConvertAget(cu, opt_flags,
                    greenland::IntrinsicHelper::HLArrayGetDouble,
                    rl_dest, rl_src[0], rl_src[1]);
      } else {
        ConvertAget(cu, opt_flags, greenland::IntrinsicHelper::HLArrayGetWide,
                    rl_dest, rl_src[0], rl_src[1]);
      }
      break;

    case Instruction::APUT:
      if (rl_src[0].fp) {
        ConvertAput(cu, opt_flags,
                    greenland::IntrinsicHelper::HLArrayPutFloat,
                    rl_src[0], rl_src[1], rl_src[2]);
      } else {
        ConvertAput(cu, opt_flags, greenland::IntrinsicHelper::HLArrayPut,
                    rl_src[0], rl_src[1], rl_src[2]);
      }
      break;
    case Instruction::APUT_OBJECT:
      ConvertAput(cu, opt_flags, greenland::IntrinsicHelper::HLArrayPutObject,
                    rl_src[0], rl_src[1], rl_src[2]);
      break;
    case Instruction::APUT_BOOLEAN:
      ConvertAput(cu, opt_flags,
                  greenland::IntrinsicHelper::HLArrayPutBoolean,
                    rl_src[0], rl_src[1], rl_src[2]);
      break;
    case Instruction::APUT_BYTE:
      ConvertAput(cu, opt_flags, greenland::IntrinsicHelper::HLArrayPutByte,
                    rl_src[0], rl_src[1], rl_src[2]);
      break;
    case Instruction::APUT_CHAR:
      ConvertAput(cu, opt_flags, greenland::IntrinsicHelper::HLArrayPutChar,
                    rl_src[0], rl_src[1], rl_src[2]);
      break;
    case Instruction::APUT_SHORT:
      ConvertAput(cu, opt_flags, greenland::IntrinsicHelper::HLArrayPutShort,
                    rl_src[0], rl_src[1], rl_src[2]);
      break;
    case Instruction::APUT_WIDE:
      if (rl_src[0].fp) {
        ConvertAput(cu, opt_flags,
                    greenland::IntrinsicHelper::HLArrayPutDouble,
                    rl_src[0], rl_src[1], rl_src[2]);
      } else {
        ConvertAput(cu, opt_flags, greenland::IntrinsicHelper::HLArrayPutWide,
                    rl_src[0], rl_src[1], rl_src[2]);
      }
      break;

    case Instruction::IGET:
      if (rl_dest.fp) {
        ConvertIget(cu, opt_flags, greenland::IntrinsicHelper::HLIGetFloat,
                    rl_dest, rl_src[0], vC);
      } else {
        ConvertIget(cu, opt_flags, greenland::IntrinsicHelper::HLIGet,
                    rl_dest, rl_src[0], vC);
      }
      break;
    case Instruction::IGET_OBJECT:
      ConvertIget(cu, opt_flags, greenland::IntrinsicHelper::HLIGetObject,
                  rl_dest, rl_src[0], vC);
      break;
    case Instruction::IGET_BOOLEAN:
      ConvertIget(cu, opt_flags, greenland::IntrinsicHelper::HLIGetBoolean,
                  rl_dest, rl_src[0], vC);
      break;
    case Instruction::IGET_BYTE:
      ConvertIget(cu, opt_flags, greenland::IntrinsicHelper::HLIGetByte,
                  rl_dest, rl_src[0], vC);
      break;
    case Instruction::IGET_CHAR:
      ConvertIget(cu, opt_flags, greenland::IntrinsicHelper::HLIGetChar,
                  rl_dest, rl_src[0], vC);
      break;
    case Instruction::IGET_SHORT:
      ConvertIget(cu, opt_flags, greenland::IntrinsicHelper::HLIGetShort,
                  rl_dest, rl_src[0], vC);
      break;
    case Instruction::IGET_WIDE:
      if (rl_dest.fp) {
        ConvertIget(cu, opt_flags, greenland::IntrinsicHelper::HLIGetDouble,
                    rl_dest, rl_src[0], vC);
      } else {
        ConvertIget(cu, opt_flags, greenland::IntrinsicHelper::HLIGetWide,
                    rl_dest, rl_src[0], vC);
      }
      break;
    case Instruction::IPUT:
      if (rl_src[0].fp) {
        ConvertIput(cu, opt_flags, greenland::IntrinsicHelper::HLIPutFloat,
                    rl_src[0], rl_src[1], vC);
      } else {
        ConvertIput(cu, opt_flags, greenland::IntrinsicHelper::HLIPut,
                    rl_src[0], rl_src[1], vC);
      }
      break;
    case Instruction::IPUT_OBJECT:
      ConvertIput(cu, opt_flags, greenland::IntrinsicHelper::HLIPutObject,
                  rl_src[0], rl_src[1], vC);
      break;
    case Instruction::IPUT_BOOLEAN:
      ConvertIput(cu, opt_flags, greenland::IntrinsicHelper::HLIPutBoolean,
                  rl_src[0], rl_src[1], vC);
      break;
    case Instruction::IPUT_BYTE:
      ConvertIput(cu, opt_flags, greenland::IntrinsicHelper::HLIPutByte,
                  rl_src[0], rl_src[1], vC);
      break;
    case Instruction::IPUT_CHAR:
      ConvertIput(cu, opt_flags, greenland::IntrinsicHelper::HLIPutChar,
                  rl_src[0], rl_src[1], vC);
      break;
    case Instruction::IPUT_SHORT:
      ConvertIput(cu, opt_flags, greenland::IntrinsicHelper::HLIPutShort,
                  rl_src[0], rl_src[1], vC);
      break;
    case Instruction::IPUT_WIDE:
      if (rl_src[0].fp) {
        ConvertIput(cu, opt_flags, greenland::IntrinsicHelper::HLIPutDouble,
                    rl_src[0], rl_src[1], vC);
      } else {
        ConvertIput(cu, opt_flags, greenland::IntrinsicHelper::HLIPutWide,
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
                          greenland::IntrinsicHelper::IntToChar);
      break;
    case Instruction::INT_TO_BYTE:
      ConvertIntNarrowing(cu, rl_dest, rl_src[0],
                          greenland::IntrinsicHelper::IntToByte);
      break;
    case Instruction::INT_TO_SHORT:
      ConvertIntNarrowing(cu, rl_dest, rl_src[0],
                          greenland::IntrinsicHelper::IntToShort);
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
      ConvertFPToInt(cu, greenland::IntrinsicHelper::F2I, rl_dest, rl_src[0]);
      break;

    case Instruction::DOUBLE_TO_INT:
      ConvertFPToInt(cu, greenland::IntrinsicHelper::D2I, rl_dest, rl_src[0]);
      break;

    case Instruction::FLOAT_TO_LONG:
      ConvertFPToInt(cu, greenland::IntrinsicHelper::F2L, rl_dest, rl_src[0]);
      break;

    case Instruction::DOUBLE_TO_LONG:
      ConvertFPToInt(cu, greenland::IntrinsicHelper::D2L, rl_dest, rl_src[0]);
      break;

    case Instruction::CMPL_FLOAT:
      ConvertWideComparison(cu, greenland::IntrinsicHelper::CmplFloat,
                            rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::CMPG_FLOAT:
      ConvertWideComparison(cu, greenland::IntrinsicHelper::CmpgFloat,
                            rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::CMPL_DOUBLE:
      ConvertWideComparison(cu, greenland::IntrinsicHelper::CmplDouble,
                            rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::CMPG_DOUBLE:
      ConvertWideComparison(cu, greenland::IntrinsicHelper::CmpgDouble,
                            rl_dest, rl_src[0], rl_src[1]);
      break;
    case Instruction::CMP_LONG:
      ConvertWideComparison(cu, greenland::IntrinsicHelper::CmpLong,
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
  llvm::SmallVector<llvm::Value*, 1> array_ref;
  array_ref.push_back(cu->irb->getInt32(offset));
  llvm::MDNode* node = llvm::MDNode::get(*cu->context, array_ref);
  cu->irb->SetDexOffset(node);
}

// Attach method info as metadata to special intrinsic
static void SetMethodInfo(CompilationUnit* cu)
{
  // We don't want dex offset on this
  cu->irb->SetDexOffset(NULL);
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::MethodInfo;
  llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Instruction* inst = cu->irb->CreateCall(intr);
  llvm::SmallVector<llvm::Value*, 2> reg_info;
  reg_info.push_back(cu->irb->getInt32(cu->num_ins));
  reg_info.push_back(cu->irb->getInt32(cu->num_regs));
  reg_info.push_back(cu->irb->getInt32(cu->num_outs));
  reg_info.push_back(cu->irb->getInt32(cu->num_compiler_temps));
  reg_info.push_back(cu->irb->getInt32(cu->num_ssa_regs));
  llvm::MDNode* reg_info_node = llvm::MDNode::get(*cu->context, reg_info);
  inst->setMetadata("RegInfo", reg_info_node);
  int promo_size = cu->num_dalvik_registers + cu->num_compiler_temps + 1;
  llvm::SmallVector<llvm::Value*, 50> pmap;
  for (int i = 0; i < promo_size; i++) {
    PromotionMap* p = &cu->promotion_map[i];
    int32_t map_data = ((p->first_in_pair & 0xff) << 24) |
                      ((p->FpReg & 0xff) << 16) |
                      ((p->core_reg & 0xff) << 8) |
                      ((p->fp_location & 0xf) << 4) |
                      (p->core_location & 0xf);
    pmap.push_back(cu->irb->getInt32(map_data));
  }
  llvm::MDNode* map_node = llvm::MDNode::get(*cu->context, pmap);
  inst->setMetadata("PromotionMap", map_node);
  SetDexOffset(cu, cu->current_dalvik_offset);
}

static void HandlePhiNodes(CompilationUnit* cu, BasicBlock* bb, llvm::BasicBlock* llvm_bb)
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
    llvm::Type* phi_type =
        LlvmTypeFromLocRec(cu, rl_dest);
    llvm::PHINode* phi = cu->irb->CreatePHI(phi_type, mir->ssa_rep->num_uses);
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
                               llvm::BasicBlock* llvm_bb)
{

  switch (static_cast<ExtendedMIROpcode>(mir->dalvikInsn.opcode)) {
    case kMirOpPhi: {
      // The llvm Phi node already emitted - just DefineValue() here.
      RegLocation rl_dest = cu->reg_location[mir->ssa_rep->defs[0]];
      if (!rl_dest.high_word) {
        // Only consider low word of pairs.
        DCHECK(GetLLVMValue(cu, rl_dest.orig_sreg) != NULL);
        llvm::Value* phi = GetLLVMValue(cu, rl_dest.orig_sreg);
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
  llvm::BasicBlock* llvm_bb = GetLLVMBlock(cu, bb->id);
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
    bool *can_be_ref = static_cast<bool*>(NewMem(cu, sizeof(bool) * cu->num_dalvik_registers,
                                               true, kAllocMisc));
    for (int i = 0; i < cu->num_ssa_regs; i++) {
      int v_reg = SRegToVReg(cu, i);
      if (v_reg > SSA_METHOD_BASEREG) {
        can_be_ref[SRegToVReg(cu, i)] |= cu->reg_location[i].ref;
      }
    }
    for (int i = 0; i < cu->num_dalvik_registers; i++) {
      if (can_be_ref[i]) {
        cu->num_shadow_frame_entries++;
      }
    }
    if (cu->num_shadow_frame_entries > 0) {
      cu->shadow_map = static_cast<int*>(NewMem(cu, sizeof(int) * cu->num_shadow_frame_entries,
                                                  true, kAllocMisc));
      for (int i = 0, j = 0; i < cu->num_dalvik_registers; i++) {
        if (can_be_ref[i]) {
          cu->shadow_map[j++] = i;
        }
      }
    }
    greenland::IntrinsicHelper::IntrinsicId id =
            greenland::IntrinsicHelper::AllocaShadowFrame;
    llvm::Function* func = cu->intrinsic_helper->GetIntrinsicFunction(id);
    llvm::Value* entries = cu->irb->getInt32(cu->num_shadow_frame_entries);
    cu->irb->CreateCall(func, entries);
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
        llvm::Function* intr = cu->intrinsic_helper->GetIntrinsicFunction(
            greenland::IntrinsicHelper::CatchTargets);
        llvm::Value* switch_key =
            cu->irb->CreateCall(intr, cu->irb->getInt32(mir->offset));
        GrowableListIterator iter;
        GrowableListIteratorInit(&bb->successor_block_list.blocks, &iter);
        // New basic block to use for work half
        llvm::BasicBlock* work_bb =
            llvm::BasicBlock::Create(*cu->context, "", cu->func);
        llvm::SwitchInst* sw =
            cu->irb->CreateSwitch(switch_key, work_bb,
                                     bb->successor_block_list.blocks.num_used);
        while (true) {
          SuccessorBlockInfo *successor_block_info =
              reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iter));
          if (successor_block_info == NULL) break;
          llvm::BasicBlock *target =
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

    bool not_handled = ConvertMIRNode(cu, mir, bb, llvm_bb,
                                     NULL /* label_list */);
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
  } else if ((bb->fall_through != NULL) && !bb->has_return) {
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

static llvm::FunctionType* GetFunctionType(CompilationUnit* cu) {

  // Get return type
  llvm::Type* ret_type = cu->irb->GetJType(RemapShorty(cu->shorty[0]),
                                              greenland::kAccurate);

  // Get argument type
  std::vector<llvm::Type*> args_type;

  // method object
  args_type.push_back(cu->irb->GetJMethodTy());

  // Do we have  a "this"?
  if ((cu->access_flags & kAccStatic) == 0) {
    args_type.push_back(cu->irb->GetJObjectTy());
  }

  for (uint32_t i = 1; i < strlen(cu->shorty); ++i) {
    args_type.push_back(cu->irb->GetJType(RemapShorty(cu->shorty[i]),
                                             greenland::kAccurate));
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}

static bool CreateFunction(CompilationUnit* cu) {
  std::string func_name(PrettyMethod(cu->method_idx, *cu->dex_file,
                                     /* with_signature */ false));
  llvm::FunctionType* func_type = GetFunctionType(cu);

  if (func_type == NULL) {
    return false;
  }

  cu->func = llvm::Function::Create(func_type,
                                       llvm::Function::ExternalLinkage,
                                       func_name, cu->module);

  llvm::Function::arg_iterator arg_iter(cu->func->arg_begin());
  llvm::Function::arg_iterator arg_end(cu->func->arg_end());

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
    llvm::BasicBlock* llvm_bb =
        llvm::BasicBlock::Create(*cu->context, entry_block ? "entry" :
                                 StringPrintf(kLabelFormat, bb->catch_entry ? kCatchBlock :
                                              kNormalBlock, offset, bb->id), cu->func);
    if (entry_block) {
        cu->entry_bb = llvm_bb;
        cu->placeholder_bb =
            llvm::BasicBlock::Create(*cu->context, "placeholder",
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
  CompilerInitGrowableList(cu, &cu->llvm_values, cu->num_ssa_regs);

  // Create the function
  CreateFunction(cu);

  // Create an LLVM basic block for each MIR block in dfs preorder
  DataFlowAnalysisDispatcher(cu, CreateLLVMBasicBlock,
                                kPreOrderDFSTraversal, false /* is_iterative */);
  /*
   * Create an llvm named value for each MIR SSA name.  Note: we'll use
   * placeholders for all non-argument values (because we haven't seen
   * the definition yet).
   */
  cu->irb->SetInsertPoint(cu->placeholder_bb);
  llvm::Function::arg_iterator arg_iter(cu->func->arg_begin());
  arg_iter++;  /* Skip path method */
  for (int i = 0; i < cu->num_ssa_regs; i++) {
    llvm::Value* val;
    RegLocation rl_temp = cu->reg_location[i];
    if ((SRegToVReg(cu, i) < 0) || rl_temp.high_word) {
      InsertGrowableList(cu, &cu->llvm_values, 0);
    } else if ((i < cu->num_regs) ||
               (i >= (cu->num_regs + cu->num_ins))) {
      llvm::Constant* imm_value = cu->reg_location[i].wide ?
         cu->irb->GetJLong(0) : cu->irb->GetJInt(0);
      val = EmitConst(cu, imm_value, cu->reg_location[i]);
      val->setName(LlvmSSAName(cu, i));
      InsertGrowableList(cu, &cu->llvm_values, reinterpret_cast<uintptr_t>(val));
    } else {
      // Recover previously-created argument values
      llvm::Value* arg_val = arg_iter++;
      InsertGrowableList(cu, &cu->llvm_values, reinterpret_cast<uintptr_t>(arg_val));
    }
  }

  DataFlowAnalysisDispatcher(cu, BlockBitcodeConversion,
                                kPreOrderDFSTraversal, false /* Iterative */);

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
  for (llvm::BasicBlock::iterator it = cu->placeholder_bb->begin(),
       it_end = cu->placeholder_bb->end(); it != it_end;) {
    llvm::Instruction* inst = llvm::dyn_cast<llvm::Instruction>(it++);
    DCHECK(inst != NULL);
    llvm::Value* val = llvm::dyn_cast<llvm::Value>(inst);
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
     if (llvm::verifyFunction(*cu->func, llvm::PrintMessageAction)) {
       LOG(INFO) << "Bitcode verification FAILED for "
                 << PrettyMethod(cu->method_idx, *cu->dex_file)
                 << " of size " << cu->insns_size;
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

    llvm::OwningPtr<llvm::tool_output_file> out_file(
        new llvm::tool_output_file(fname.c_str(), errmsg,
                                   llvm::raw_fd_ostream::F_Binary));

    if (!errmsg.empty()) {
      LOG(ERROR) << "Failed to create bitcode output file: " << errmsg;
    }

    llvm::WriteBitcodeToFile(cu->module, out_file->os());
    out_file->keep();
  }
}

static RegLocation GetLoc(CompilationUnit* cu, llvm::Value* val) {
  RegLocation res;
  DCHECK(val != NULL);
  SafeMap<llvm::Value*, RegLocation>::iterator it = cu->loc_map.find(val);
  if (it == cu->loc_map.end()) {
    std::string val_name = val->getName().str();
    if (val_name.empty()) {
      // FIXME: need to be more robust, handle FP and be in a position to
      // manage unnamed temps whose lifetimes span basic block boundaries
      UNIMPLEMENTED(WARNING) << "Need to handle unnamed llvm temps";
      memset(&res, 0, sizeof(res));
      res.location = kLocPhysReg;
      res.low_reg = AllocTemp(cu);
      res.home = true;
      res.s_reg_low = INVALID_SREG;
      res.orig_sreg = INVALID_SREG;
      llvm::Type* ty = val->getType();
      res.wide = ((ty == cu->irb->getInt64Ty()) ||
                  (ty == cu->irb->getDoubleTy()));
      if (res.wide) {
        res.high_reg = AllocTemp(cu);
      }
      cu->loc_map.Put(val, res);
    } else {
      DCHECK_EQ(val_name[0], 'v');
      int base_sreg = INVALID_SREG;
      sscanf(val_name.c_str(), "v%d_", &base_sreg);
      res = cu->reg_location[base_sreg];
      cu->loc_map.Put(val, res);
    }
  } else {
    res = it->second;
  }
  return res;
}

static Instruction::Code GetDalvikOpcode(OpKind op, bool is_const, bool is_wide)
{
  Instruction::Code res = Instruction::NOP;
  if (is_wide) {
    switch(op) {
      case kOpAdd: res = Instruction::ADD_LONG; break;
      case kOpSub: res = Instruction::SUB_LONG; break;
      case kOpMul: res = Instruction::MUL_LONG; break;
      case kOpDiv: res = Instruction::DIV_LONG; break;
      case kOpRem: res = Instruction::REM_LONG; break;
      case kOpAnd: res = Instruction::AND_LONG; break;
      case kOpOr: res = Instruction::OR_LONG; break;
      case kOpXor: res = Instruction::XOR_LONG; break;
      case kOpLsl: res = Instruction::SHL_LONG; break;
      case kOpLsr: res = Instruction::USHR_LONG; break;
      case kOpAsr: res = Instruction::SHR_LONG; break;
      default: LOG(FATAL) << "Unexpected OpKind " << op;
    }
  } else if (is_const){
    switch(op) {
      case kOpAdd: res = Instruction::ADD_INT_LIT16; break;
      case kOpSub: res = Instruction::RSUB_INT_LIT8; break;
      case kOpMul: res = Instruction::MUL_INT_LIT16; break;
      case kOpDiv: res = Instruction::DIV_INT_LIT16; break;
      case kOpRem: res = Instruction::REM_INT_LIT16; break;
      case kOpAnd: res = Instruction::AND_INT_LIT16; break;
      case kOpOr: res = Instruction::OR_INT_LIT16; break;
      case kOpXor: res = Instruction::XOR_INT_LIT16; break;
      case kOpLsl: res = Instruction::SHL_INT_LIT8; break;
      case kOpLsr: res = Instruction::USHR_INT_LIT8; break;
      case kOpAsr: res = Instruction::SHR_INT_LIT8; break;
      default: LOG(FATAL) << "Unexpected OpKind " << op;
    }
  } else {
    switch(op) {
      case kOpAdd: res = Instruction::ADD_INT; break;
      case kOpSub: res = Instruction::SUB_INT; break;
      case kOpMul: res = Instruction::MUL_INT; break;
      case kOpDiv: res = Instruction::DIV_INT; break;
      case kOpRem: res = Instruction::REM_INT; break;
      case kOpAnd: res = Instruction::AND_INT; break;
      case kOpOr: res = Instruction::OR_INT; break;
      case kOpXor: res = Instruction::XOR_INT; break;
      case kOpLsl: res = Instruction::SHL_INT; break;
      case kOpLsr: res = Instruction::USHR_INT; break;
      case kOpAsr: res = Instruction::SHR_INT; break;
      default: LOG(FATAL) << "Unexpected OpKind " << op;
    }
  }
  return res;
}

static Instruction::Code GetDalvikFPOpcode(OpKind op, bool is_const, bool is_wide)
{
  Instruction::Code res = Instruction::NOP;
  if (is_wide) {
    switch(op) {
      case kOpAdd: res = Instruction::ADD_DOUBLE; break;
      case kOpSub: res = Instruction::SUB_DOUBLE; break;
      case kOpMul: res = Instruction::MUL_DOUBLE; break;
      case kOpDiv: res = Instruction::DIV_DOUBLE; break;
      case kOpRem: res = Instruction::REM_DOUBLE; break;
      default: LOG(FATAL) << "Unexpected OpKind " << op;
    }
  } else {
    switch(op) {
      case kOpAdd: res = Instruction::ADD_FLOAT; break;
      case kOpSub: res = Instruction::SUB_FLOAT; break;
      case kOpMul: res = Instruction::MUL_FLOAT; break;
      case kOpDiv: res = Instruction::DIV_FLOAT; break;
      case kOpRem: res = Instruction::REM_FLOAT; break;
      default: LOG(FATAL) << "Unexpected OpKind " << op;
    }
  }
  return res;
}

static void CvtBinFPOp(CompilationUnit* cu, OpKind op, llvm::Instruction* inst)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_dest = GetLoc(cu, inst);
  /*
   * Normally, we won't ever generate an FP operation with an immediate
   * operand (not supported in Dex instruction set).  However, the IR builder
   * may insert them - in particular for create_neg_fp.  Recognize this case
   * and deal with it.
   */
  llvm::ConstantFP* op1C = llvm::dyn_cast<llvm::ConstantFP>(inst->getOperand(0));
  llvm::ConstantFP* op2C = llvm::dyn_cast<llvm::ConstantFP>(inst->getOperand(1));
  DCHECK(op2C == NULL);
  if ((op1C != NULL) && (op == kOpSub)) {
    RegLocation rl_src = GetLoc(cu, inst->getOperand(1));
    if (rl_dest.wide) {
      cg->GenArithOpDouble(cu, Instruction::NEG_DOUBLE, rl_dest, rl_src, rl_src);
    } else {
      cg->GenArithOpFloat(cu, Instruction::NEG_FLOAT, rl_dest, rl_src, rl_src);
    }
  } else {
    DCHECK(op1C == NULL);
    RegLocation rl_src1 = GetLoc(cu, inst->getOperand(0));
    RegLocation rl_src2 = GetLoc(cu, inst->getOperand(1));
    Instruction::Code dalvik_op = GetDalvikFPOpcode(op, false, rl_dest.wide);
    if (rl_dest.wide) {
      cg->GenArithOpDouble(cu, dalvik_op, rl_dest, rl_src1, rl_src2);
    } else {
      cg->GenArithOpFloat(cu, dalvik_op, rl_dest, rl_src1, rl_src2);
    }
  }
}

static void CvtIntNarrowing(CompilationUnit* cu, llvm::Instruction* inst,
                     Instruction::Code opcode)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_dest = GetLoc(cu, inst);
  RegLocation rl_src = GetLoc(cu, inst->getOperand(0));
  cg->GenIntNarrowing(cu, opcode, rl_dest, rl_src);
}

static void CvtIntToFP(CompilationUnit* cu, llvm::Instruction* inst)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_dest = GetLoc(cu, inst);
  RegLocation rl_src = GetLoc(cu, inst->getOperand(0));
  Instruction::Code opcode;
  if (rl_dest.wide) {
    if (rl_src.wide) {
      opcode = Instruction::LONG_TO_DOUBLE;
    } else {
      opcode = Instruction::INT_TO_DOUBLE;
    }
  } else {
    if (rl_src.wide) {
      opcode = Instruction::LONG_TO_FLOAT;
    } else {
      opcode = Instruction::INT_TO_FLOAT;
    }
  }
  cg->GenConversion(cu, opcode, rl_dest, rl_src);
}

static void CvtFPToInt(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_dest = GetLoc(cu, call_inst);
  RegLocation rl_src = GetLoc(cu, call_inst->getOperand(0));
  Instruction::Code opcode;
  if (rl_dest.wide) {
    if (rl_src.wide) {
      opcode = Instruction::DOUBLE_TO_LONG;
    } else {
      opcode = Instruction::FLOAT_TO_LONG;
    }
  } else {
    if (rl_src.wide) {
      opcode = Instruction::DOUBLE_TO_INT;
    } else {
      opcode = Instruction::FLOAT_TO_INT;
    }
  }
  cg->GenConversion(cu, opcode, rl_dest, rl_src);
}

static void CvtFloatToDouble(CompilationUnit* cu, llvm::Instruction* inst)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_dest = GetLoc(cu, inst);
  RegLocation rl_src = GetLoc(cu, inst->getOperand(0));
  cg->GenConversion(cu, Instruction::FLOAT_TO_DOUBLE, rl_dest, rl_src);
}

static void CvtTrunc(CompilationUnit* cu, llvm::Instruction* inst)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_dest = GetLoc(cu, inst);
  RegLocation rl_src = GetLoc(cu, inst->getOperand(0));
  rl_src = UpdateLocWide(cu, rl_src);
  rl_src = WideToNarrow(cu, rl_src);
  cg->StoreValue(cu, rl_dest, rl_src);
}

static void CvtDoubleToFloat(CompilationUnit* cu, llvm::Instruction* inst)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_dest = GetLoc(cu, inst);
  RegLocation rl_src = GetLoc(cu, inst->getOperand(0));
  cg->GenConversion(cu, Instruction::DOUBLE_TO_FLOAT, rl_dest, rl_src);
}


static void CvtIntExt(CompilationUnit* cu, llvm::Instruction* inst, bool is_signed)
{
  Codegen* cg = cu->cg.get();
  // TODO: evaluate src/tgt types and add general support for more than int to long
  RegLocation rl_dest = GetLoc(cu, inst);
  RegLocation rl_src = GetLoc(cu, inst->getOperand(0));
  DCHECK(rl_dest.wide);
  DCHECK(!rl_src.wide);
  DCHECK(!rl_dest.fp);
  DCHECK(!rl_src.fp);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  if (rl_src.location == kLocPhysReg) {
    cg->OpRegCopy(cu, rl_result.low_reg, rl_src.low_reg);
  } else {
    cg->LoadValueDirect(cu, rl_src, rl_result.low_reg);
  }
  if (is_signed) {
    cg->OpRegRegImm(cu, kOpAsr, rl_result.high_reg, rl_result.low_reg, 31);
  } else {
    cg->LoadConstant(cu, rl_result.high_reg, 0);
  }
  cg->StoreValueWide(cu, rl_dest, rl_result);
}

static void CvtBinOp(CompilationUnit* cu, OpKind op, llvm::Instruction* inst)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_dest = GetLoc(cu, inst);
  llvm::Value* lhs = inst->getOperand(0);
  // Special-case RSUB/NEG
  llvm::ConstantInt* lhs_imm = llvm::dyn_cast<llvm::ConstantInt>(lhs);
  if ((op == kOpSub) && (lhs_imm != NULL)) {
    RegLocation rl_src1 = GetLoc(cu, inst->getOperand(1));
    if (rl_src1.wide) {
      DCHECK_EQ(lhs_imm->getSExtValue(), 0);
      cg->GenArithOpLong(cu, Instruction::NEG_LONG, rl_dest, rl_src1, rl_src1);
    } else {
      cg->GenArithOpIntLit(cu, Instruction::RSUB_INT, rl_dest, rl_src1,
                       lhs_imm->getSExtValue());
    }
    return;
  }
  DCHECK(lhs_imm == NULL);
  RegLocation rl_src1 = GetLoc(cu, inst->getOperand(0));
  llvm::Value* rhs = inst->getOperand(1);
  llvm::ConstantInt* const_rhs = llvm::dyn_cast<llvm::ConstantInt>(rhs);
  if (!rl_dest.wide && (const_rhs != NULL)) {
    Instruction::Code dalvik_op = GetDalvikOpcode(op, true, false);
    cg->GenArithOpIntLit(cu, dalvik_op, rl_dest, rl_src1, const_rhs->getSExtValue());
  } else {
    Instruction::Code dalvik_op = GetDalvikOpcode(op, false, rl_dest.wide);
    RegLocation rl_src2;
    if (const_rhs != NULL) {
      // ir_builder converts NOT_LONG to xor src, -1.  Restore
      DCHECK_EQ(dalvik_op, Instruction::XOR_LONG);
      DCHECK_EQ(-1L, const_rhs->getSExtValue());
      dalvik_op = Instruction::NOT_LONG;
      rl_src2 = rl_src1;
    } else {
      rl_src2 = GetLoc(cu, rhs);
    }
    if (rl_dest.wide) {
      cg->GenArithOpLong(cu, dalvik_op, rl_dest, rl_src1, rl_src2);
    } else {
      cg->GenArithOpInt(cu, dalvik_op, rl_dest, rl_src1, rl_src2);
    }
  }
}

static void CvtShiftOp(CompilationUnit* cu, Instruction::Code opcode, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 2U);
  RegLocation rl_dest = GetLoc(cu, call_inst);
  RegLocation rl_src = GetLoc(cu, call_inst->getArgOperand(0));
  llvm::Value* rhs = call_inst->getArgOperand(1);
  if (llvm::ConstantInt* src2 = llvm::dyn_cast<llvm::ConstantInt>(rhs)) {
    DCHECK(!rl_dest.wide);
    cg->GenArithOpIntLit(cu, opcode, rl_dest, rl_src, src2->getSExtValue());
  } else {
    RegLocation rl_shift = GetLoc(cu, rhs);
    if (call_inst->getType() == cu->irb->getInt64Ty()) {
      cg->GenShiftOpLong(cu, opcode, rl_dest, rl_src, rl_shift);
    } else {
      cg->GenArithOpInt(cu, opcode, rl_dest, rl_src, rl_shift);
    }
  }
}

static void CvtBr(CompilationUnit* cu, llvm::Instruction* inst)
{
  Codegen* cg = cu->cg.get();
  llvm::BranchInst* br_inst = llvm::dyn_cast<llvm::BranchInst>(inst);
  DCHECK(br_inst != NULL);
  DCHECK(br_inst->isUnconditional());  // May change - but this is all we use now
  llvm::BasicBlock* target_bb = br_inst->getSuccessor(0);
  cg->OpUnconditionalBranch(cu, cu->block_to_label_map.Get(target_bb));
}

static void CvtPhi(CompilationUnit* cu, llvm::Instruction* inst)
{
  // Nop - these have already been processed
}

static void CvtRet(CompilationUnit* cu, llvm::Instruction* inst)
{
  Codegen* cg = cu->cg.get();
  llvm::ReturnInst* ret_inst = llvm::dyn_cast<llvm::ReturnInst>(inst);
  llvm::Value* ret_val = ret_inst->getReturnValue();
  if (ret_val != NULL) {
    RegLocation rl_src = GetLoc(cu, ret_val);
    if (rl_src.wide) {
      cg->StoreValueWide(cu, GetReturnWide(cu, rl_src.fp), rl_src);
    } else {
      cg->StoreValue(cu, GetReturn(cu, rl_src.fp), rl_src);
    }
  }
  cg->GenExitSequence(cu);
}

static ConditionCode GetCond(llvm::ICmpInst::Predicate llvm_cond)
{
  ConditionCode res = kCondAl;
  switch(llvm_cond) {
    case llvm::ICmpInst::ICMP_EQ: res = kCondEq; break;
    case llvm::ICmpInst::ICMP_NE: res = kCondNe; break;
    case llvm::ICmpInst::ICMP_SLT: res = kCondLt; break;
    case llvm::ICmpInst::ICMP_SGE: res = kCondGe; break;
    case llvm::ICmpInst::ICMP_SGT: res = kCondGt; break;
    case llvm::ICmpInst::ICMP_SLE: res = kCondLe; break;
    default: LOG(FATAL) << "Unexpected llvm condition";
  }
  return res;
}

static void CvtICmp(CompilationUnit* cu, llvm::Instruction* inst)
{
  // cg->GenCmpLong(cu, rl_dest, rl_src1, rl_src2)
  UNIMPLEMENTED(FATAL);
}

static void CvtICmpBr(CompilationUnit* cu, llvm::Instruction* inst,
               llvm::BranchInst* br_inst)
{
  Codegen* cg = cu->cg.get();
  // Get targets
  llvm::BasicBlock* taken_bb = br_inst->getSuccessor(0);
  LIR* taken = cu->block_to_label_map.Get(taken_bb);
  llvm::BasicBlock* fallthrough_bb = br_inst->getSuccessor(1);
  LIR* fall_through = cu->block_to_label_map.Get(fallthrough_bb);
  // Get comparison operands
  llvm::ICmpInst* i_cmp_inst = llvm::dyn_cast<llvm::ICmpInst>(inst);
  ConditionCode cond = GetCond(i_cmp_inst->getPredicate());
  llvm::Value* lhs = i_cmp_inst->getOperand(0);
  // Not expecting a constant as 1st operand
  DCHECK(llvm::dyn_cast<llvm::ConstantInt>(lhs) == NULL);
  RegLocation rl_src1 = GetLoc(cu, inst->getOperand(0));
  rl_src1 = cg->LoadValue(cu, rl_src1, kCoreReg);
  llvm::Value* rhs = inst->getOperand(1);
  if (cu->instruction_set == kMips) {
    // Compare and branch in one shot
    UNIMPLEMENTED(FATAL);
  }
  //Compare, then branch
  // TODO: handle fused CMP_LONG/IF_xxZ case
  if (llvm::ConstantInt* src2 = llvm::dyn_cast<llvm::ConstantInt>(rhs)) {
    cg->OpRegImm(cu, kOpCmp, rl_src1.low_reg, src2->getSExtValue());
  } else if (llvm::dyn_cast<llvm::ConstantPointerNull>(rhs) != NULL) {
    cg->OpRegImm(cu, kOpCmp, rl_src1.low_reg, 0);
  } else {
    RegLocation rl_src2 = GetLoc(cu, rhs);
    rl_src2 = cg->LoadValue(cu, rl_src2, kCoreReg);
    cg->OpRegReg(cu, kOpCmp, rl_src1.low_reg, rl_src2.low_reg);
  }
  cg->OpCondBranch(cu, cond, taken);
  // Fallthrough
  cg->OpUnconditionalBranch(cu, fall_through);
}

static void CvtCopy(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 1U);
  RegLocation rl_src = GetLoc(cu, call_inst->getArgOperand(0));
  RegLocation rl_dest = GetLoc(cu, call_inst);
  DCHECK_EQ(rl_src.wide, rl_dest.wide);
  DCHECK_EQ(rl_src.fp, rl_dest.fp);
  if (rl_src.wide) {
    cg->StoreValueWide(cu, rl_dest, rl_src);
  } else {
    cg->StoreValue(cu, rl_dest, rl_src);
  }
}

// Note: Immediate arg is a ConstantInt regardless of result type
static void CvtConst(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 1U);
  llvm::ConstantInt* src =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  uint64_t immval = src->getZExtValue();
  RegLocation rl_dest = GetLoc(cu, call_inst);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kAnyReg, true);
  if (rl_dest.wide) {
    cg->LoadConstantValueWide(cu, rl_result.low_reg, rl_result.high_reg,
                          (immval) & 0xffffffff, (immval >> 32) & 0xffffffff);
    cg->StoreValueWide(cu, rl_dest, rl_result);
  } else {
    int immediate = immval & 0xffffffff;
    cg->LoadConstantNoClobber(cu, rl_result.low_reg, immediate);
    cg->StoreValue(cu, rl_dest, rl_result);
    if (immediate == 0) {
      cg->Workaround7250540(cu, rl_dest, rl_result.low_reg);
    }
  }
}

static void CvtConstObject(CompilationUnit* cu, llvm::CallInst* call_inst, bool is_string)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 1U);
  llvm::ConstantInt* idx_val =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  uint32_t index = idx_val->getZExtValue();
  RegLocation rl_dest = GetLoc(cu, call_inst);
  if (is_string) {
    cg->GenConstString(cu, index, rl_dest);
  } else {
    cg->GenConstClass(cu, index, rl_dest);
  }
}

static void CvtFillArrayData(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 2U);
  llvm::ConstantInt* offset_val =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  RegLocation rl_src = GetLoc(cu, call_inst->getArgOperand(1));
  cg->GenFillArrayData(cu, offset_val->getSExtValue(), rl_src);
}

static void CvtNewInstance(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 1U);
  llvm::ConstantInt* type_idx_val =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  uint32_t type_idx = type_idx_val->getZExtValue();
  RegLocation rl_dest = GetLoc(cu, call_inst);
  cg->GenNewInstance(cu, type_idx, rl_dest);
}

static void CvtNewArray(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 2U);
  llvm::ConstantInt* type_idx_val =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  uint32_t type_idx = type_idx_val->getZExtValue();
  llvm::Value* len = call_inst->getArgOperand(1);
  RegLocation rl_len = GetLoc(cu, len);
  RegLocation rl_dest = GetLoc(cu, call_inst);
  cg->GenNewArray(cu, type_idx, rl_dest, rl_len);
}

static void CvtInstanceOf(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 2U);
  llvm::ConstantInt* type_idx_val =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  uint32_t type_idx = type_idx_val->getZExtValue();
  llvm::Value* src = call_inst->getArgOperand(1);
  RegLocation rl_src = GetLoc(cu, src);
  RegLocation rl_dest = GetLoc(cu, call_inst);
  cg->GenInstanceof(cu, type_idx, rl_dest, rl_src);
}

static void CvtThrow(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 1U);
  llvm::Value* src = call_inst->getArgOperand(0);
  RegLocation rl_src = GetLoc(cu, src);
  cg->GenThrow(cu, rl_src);
}

static void CvtMonitorEnterExit(CompilationUnit* cu, bool is_enter,
                         llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 2U);
  llvm::ConstantInt* opt_flags =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  llvm::Value* src = call_inst->getArgOperand(1);
  RegLocation rl_src = GetLoc(cu, src);
  if (is_enter) {
    cg->GenMonitorEnter(cu, opt_flags->getZExtValue(), rl_src);
  } else {
    cg->GenMonitorExit(cu, opt_flags->getZExtValue(), rl_src);
  }
}

static void CvtArrayLength(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 2U);
  llvm::ConstantInt* opt_flags =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  llvm::Value* src = call_inst->getArgOperand(1);
  RegLocation rl_src = GetLoc(cu, src);
  rl_src = cg->LoadValue(cu, rl_src, kCoreReg);
  cg->GenNullCheck(cu, rl_src.s_reg_low, rl_src.low_reg, opt_flags->getZExtValue());
  RegLocation rl_dest = GetLoc(cu, call_inst);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  int len_offset = Array::LengthOffset().Int32Value();
  cg->LoadWordDisp(cu, rl_src.low_reg, len_offset, rl_result.low_reg);
  cg->StoreValue(cu, rl_dest, rl_result);
}

static void CvtMoveException(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_dest = GetLoc(cu, call_inst);
  cg->GenMoveException(cu, rl_dest);
}

static void CvtSget(CompilationUnit* cu, llvm::CallInst* call_inst, bool is_wide, bool is_object)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 1U);
  llvm::ConstantInt* type_idx_val =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  uint32_t type_idx = type_idx_val->getZExtValue();
  RegLocation rl_dest = GetLoc(cu, call_inst);
  cg->GenSget(cu, type_idx, rl_dest, is_wide, is_object);
}

static void CvtSput(CompilationUnit* cu, llvm::CallInst* call_inst, bool is_wide, bool is_object)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 2U);
  llvm::ConstantInt* type_idx_val =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  uint32_t type_idx = type_idx_val->getZExtValue();
  llvm::Value* src = call_inst->getArgOperand(1);
  RegLocation rl_src = GetLoc(cu, src);
  cg->GenSput(cu, type_idx, rl_src, is_wide, is_object);
}

static void CvtAget(CompilationUnit* cu, llvm::CallInst* call_inst, OpSize size, int scale)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 3U);
  llvm::ConstantInt* opt_flags =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  RegLocation rl_array = GetLoc(cu, call_inst->getArgOperand(1));
  RegLocation rl_index = GetLoc(cu, call_inst->getArgOperand(2));
  RegLocation rl_dest = GetLoc(cu, call_inst);
  cg->GenArrayGet(cu, opt_flags->getZExtValue(), size, rl_array, rl_index,
              rl_dest, scale);
}

static void CvtAput(CompilationUnit* cu, llvm::CallInst* call_inst, OpSize size,
                    int scale, bool is_object)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 4U);
  llvm::ConstantInt* opt_flags =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  RegLocation rl_src = GetLoc(cu, call_inst->getArgOperand(1));
  RegLocation rl_array = GetLoc(cu, call_inst->getArgOperand(2));
  RegLocation rl_index = GetLoc(cu, call_inst->getArgOperand(3));
  if (is_object) {
    cg->GenArrayObjPut(cu, opt_flags->getZExtValue(), rl_array, rl_index,
                   rl_src, scale);
  } else {
    cg->GenArrayPut(cu, opt_flags->getZExtValue(), size, rl_array, rl_index,
                rl_src, scale);
  }
}

static void CvtAputObj(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  CvtAput(cu, call_inst, kWord, 2, true /* is_object */);
}

static void CvtAputPrimitive(CompilationUnit* cu, llvm::CallInst* call_inst,
                      OpSize size, int scale)
{
  CvtAput(cu, call_inst, size, scale, false /* is_object */);
}

static void CvtIget(CompilationUnit* cu, llvm::CallInst* call_inst, OpSize size,
                    bool is_wide, bool is_obj)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 3U);
  llvm::ConstantInt* opt_flags =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  RegLocation rl_obj = GetLoc(cu, call_inst->getArgOperand(1));
  llvm::ConstantInt* field_idx =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(2));
  RegLocation rl_dest = GetLoc(cu, call_inst);
  cg->GenIGet(cu, field_idx->getZExtValue(), opt_flags->getZExtValue(),
          size, rl_dest, rl_obj, is_wide, is_obj);
}

static void CvtIput(CompilationUnit* cu, llvm::CallInst* call_inst, OpSize size,
                    bool is_wide, bool is_obj)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 4U);
  llvm::ConstantInt* opt_flags =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  RegLocation rl_src = GetLoc(cu, call_inst->getArgOperand(1));
  RegLocation rl_obj = GetLoc(cu, call_inst->getArgOperand(2));
  llvm::ConstantInt* field_idx =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(3));
  cg->GenIPut(cu, field_idx->getZExtValue(), opt_flags->getZExtValue(),
          size, rl_src, rl_obj, is_wide, is_obj);
}

static void CvtCheckCast(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  DCHECK_EQ(call_inst->getNumArgOperands(), 2U);
  llvm::ConstantInt* type_idx =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  RegLocation rl_src = GetLoc(cu, call_inst->getArgOperand(1));
  cg->GenCheckCast(cu, type_idx->getZExtValue(), rl_src);
}

static void CvtFPCompare(CompilationUnit* cu, llvm::CallInst* call_inst,
                         Instruction::Code opcode)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_src1 = GetLoc(cu, call_inst->getArgOperand(0));
  RegLocation rl_src2 = GetLoc(cu, call_inst->getArgOperand(1));
  RegLocation rl_dest = GetLoc(cu, call_inst);
  cg->GenCmpFP(cu, opcode, rl_dest, rl_src1, rl_src2);
}

static void CvtLongCompare(CompilationUnit* cu, llvm::CallInst* call_inst)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_src1 = GetLoc(cu, call_inst->getArgOperand(0));
  RegLocation rl_src2 = GetLoc(cu, call_inst->getArgOperand(1));
  RegLocation rl_dest = GetLoc(cu, call_inst);
  cg->GenCmpLong(cu, rl_dest, rl_src1, rl_src2);
}

static void CvtSwitch(CompilationUnit* cu, llvm::Instruction* inst)
{
  Codegen* cg = cu->cg.get();
  llvm::SwitchInst* sw_inst = llvm::dyn_cast<llvm::SwitchInst>(inst);
  DCHECK(sw_inst != NULL);
  llvm::Value* test_val = sw_inst->getCondition();
  llvm::MDNode* table_offset_node = sw_inst->getMetadata("SwitchTable");
  DCHECK(table_offset_node != NULL);
  llvm::ConstantInt* table_offset_value =
          static_cast<llvm::ConstantInt*>(table_offset_node->getOperand(0));
  int32_t table_offset = table_offset_value->getSExtValue();
  RegLocation rl_src = GetLoc(cu, test_val);
  const uint16_t* table = cu->insns + cu->current_dalvik_offset + table_offset;
  uint16_t table_magic = *table;
  if (table_magic == 0x100) {
    cg->GenPackedSwitch(cu, table_offset, rl_src);
  } else {
    DCHECK_EQ(table_magic, 0x200);
    cg->GenSparseSwitch(cu, table_offset, rl_src);
  }
}

static void CvtInvoke(CompilationUnit* cu, llvm::CallInst* call_inst, bool is_void,
                      bool is_filled_new_array)
{
  Codegen* cg = cu->cg.get();
  CallInfo* info = static_cast<CallInfo*>(NewMem(cu, sizeof(CallInfo), true, kAllocMisc));
  if (is_void) {
    info->result.location = kLocInvalid;
  } else {
    info->result = GetLoc(cu, call_inst);
  }
  llvm::ConstantInt* invoke_type_val =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(0));
  llvm::ConstantInt* method_index_val =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(1));
  llvm::ConstantInt* opt_flags_val =
      llvm::dyn_cast<llvm::ConstantInt>(call_inst->getArgOperand(2));
  info->type = static_cast<InvokeType>(invoke_type_val->getZExtValue());
  info->index = method_index_val->getZExtValue();
  info->opt_flags = opt_flags_val->getZExtValue();
  info->offset = cu->current_dalvik_offset;

  // Count the argument words, and then build argument array.
  info->num_arg_words = 0;
  for (unsigned int i = 3; i < call_inst->getNumArgOperands(); i++) {
    RegLocation t_loc = GetLoc(cu, call_inst->getArgOperand(i));
    info->num_arg_words += t_loc.wide ? 2 : 1;
  }
  info->args = (info->num_arg_words == 0) ? NULL : static_cast<RegLocation*>
      (NewMem(cu, sizeof(RegLocation) * info->num_arg_words, false, kAllocMisc));
  // Now, fill in the location records, synthesizing high loc of wide vals
  for (int i = 3, next = 0; next < info->num_arg_words;) {
    info->args[next] = GetLoc(cu, call_inst->getArgOperand(i++));
    if (info->args[next].wide) {
      next++;
      // TODO: Might make sense to mark this as an invalid loc
      info->args[next].orig_sreg = info->args[next-1].orig_sreg+1;
      info->args[next].s_reg_low = info->args[next-1].s_reg_low+1;
    }
    next++;
  }
  // TODO - rework such that we no longer need is_range
  info->is_range = (info->num_arg_words > 5);

  if (is_filled_new_array) {
    cg->GenFilledNewArray(cu, info);
  } else {
    cg->GenInvoke(cu, info);
  }
}

/* Look up the RegLocation associated with a Value.  Must already be defined */
static RegLocation ValToLoc(CompilationUnit* cu, llvm::Value* val)
{
  SafeMap<llvm::Value*, RegLocation>::iterator it = cu->loc_map.find(val);
  DCHECK(it != cu->loc_map.end()) << "Missing definition";
  return it->second;
}

static bool BitcodeBlockCodeGen(CompilationUnit* cu, llvm::BasicBlock* bb)
{
  Codegen* cg = cu->cg.get();
  while (cu->llvm_blocks.find(bb) == cu->llvm_blocks.end()) {
    llvm::BasicBlock* next_bb = NULL;
    cu->llvm_blocks.insert(bb);
    bool is_entry = (bb == &cu->func->getEntryBlock());
    // Define the starting label
    LIR* block_label = cu->block_to_label_map.Get(bb);
    // Extract the type and starting offset from the block's name
    char block_type = kInvalidBlock;
    if (is_entry) {
      block_type = kNormalBlock;
      block_label->operands[0] = 0;
    } else if (!bb->hasName()) {
      block_type = kNormalBlock;
      block_label->operands[0] = DexFile::kDexNoIndex;
    } else {
      std::string block_name = bb->getName().str();
      int dummy;
      sscanf(block_name.c_str(), kLabelFormat, &block_type, &block_label->operands[0], &dummy);
      cu->current_dalvik_offset = block_label->operands[0];
    }
    DCHECK((block_type == kNormalBlock) || (block_type == kCatchBlock));
    cu->current_dalvik_offset = block_label->operands[0];
    // Set the label kind
    block_label->opcode = kPseudoNormalBlockLabel;
    // Insert the label
    AppendLIR(cu, block_label);

    LIR* head_lir = NULL;

    if (block_type == kCatchBlock) {
      head_lir = NewLIR0(cu, kPseudoExportedPC);
    }

    // Free temp registers and reset redundant store tracking */
    ResetRegPool(cu);
    ResetDefTracking(cu);

    //TODO: restore oat incoming liveness optimization
    ClobberAllRegs(cu);

    if (is_entry) {
      RegLocation* ArgLocs = static_cast<RegLocation*>
          (NewMem(cu, sizeof(RegLocation) * cu->num_ins, true, kAllocMisc));
      llvm::Function::arg_iterator it(cu->func->arg_begin());
      llvm::Function::arg_iterator it_end(cu->func->arg_end());
      // Skip past Method*
      it++;
      for (unsigned i = 0; it != it_end; ++it) {
        llvm::Value* val = it;
        ArgLocs[i++] = ValToLoc(cu, val);
        llvm::Type* ty = val->getType();
        if ((ty == cu->irb->getInt64Ty()) || (ty == cu->irb->getDoubleTy())) {
          ArgLocs[i] = ArgLocs[i-1];
          ArgLocs[i].low_reg = ArgLocs[i].high_reg;
          ArgLocs[i].orig_sreg++;
          ArgLocs[i].s_reg_low = INVALID_SREG;
          ArgLocs[i].high_word = true;
          i++;
        }
      }
      cg->GenEntrySequence(cu, ArgLocs, cu->method_loc);
    }

    // Visit all of the instructions in the block
    for (llvm::BasicBlock::iterator it = bb->begin(), e = bb->end(); it != e;) {
      llvm::Instruction* inst = it;
      llvm::BasicBlock::iterator next_it = ++it;
      // Extract the Dalvik offset from the instruction
      uint32_t opcode = inst->getOpcode();
      llvm::MDNode* dex_offset_node = inst->getMetadata("DexOff");
      if (dex_offset_node != NULL) {
        llvm::ConstantInt* dex_offset_value =
            static_cast<llvm::ConstantInt*>(dex_offset_node->getOperand(0));
        cu->current_dalvik_offset = dex_offset_value->getZExtValue();
      }

      ResetRegPool(cu);
      if (cu->disable_opt & (1 << kTrackLiveTemps)) {
        ClobberAllRegs(cu);
      }

      if (cu->disable_opt & (1 << kSuppressLoads)) {
        ResetDefTracking(cu);
      }

  #ifndef NDEBUG
      /* Reset temp tracking sanity check */
      cu->live_sreg = INVALID_SREG;
  #endif

      // TODO: use llvm opcode name here instead of "boundary" if verbose
      LIR* boundary_lir = MarkBoundary(cu, cu->current_dalvik_offset, "boundary");

      /* Remember the first LIR for thisl block*/
      if (head_lir == NULL) {
        head_lir = boundary_lir;
        head_lir->def_mask = ENCODE_ALL;
      }

      switch(opcode) {

        case llvm::Instruction::ICmp: {
            llvm::Instruction* next_inst = next_it;
            llvm::BranchInst* br_inst = llvm::dyn_cast<llvm::BranchInst>(next_inst);
            if (br_inst != NULL /* and... */) {
              CvtICmpBr(cu, inst, br_inst);
              ++it;
            } else {
              CvtICmp(cu, inst);
            }
          }
          break;

        case llvm::Instruction::Call: {
            llvm::CallInst* call_inst = llvm::dyn_cast<llvm::CallInst>(inst);
            llvm::Function* callee = call_inst->getCalledFunction();
            greenland::IntrinsicHelper::IntrinsicId id =
                cu->intrinsic_helper->GetIntrinsicId(callee);
            switch (id) {
              case greenland::IntrinsicHelper::AllocaShadowFrame:
              case greenland::IntrinsicHelper::PopShadowFrame:
              case greenland::IntrinsicHelper::SetVReg:
                // Ignore shadow frame stuff for quick compiler
                break;
              case greenland::IntrinsicHelper::CopyInt:
              case greenland::IntrinsicHelper::CopyObj:
              case greenland::IntrinsicHelper::CopyFloat:
              case greenland::IntrinsicHelper::CopyLong:
              case greenland::IntrinsicHelper::CopyDouble:
                CvtCopy(cu, call_inst);
                break;
              case greenland::IntrinsicHelper::ConstInt:
              case greenland::IntrinsicHelper::ConstObj:
              case greenland::IntrinsicHelper::ConstLong:
              case greenland::IntrinsicHelper::ConstFloat:
              case greenland::IntrinsicHelper::ConstDouble:
                CvtConst(cu, call_inst);
                break;
              case greenland::IntrinsicHelper::DivInt:
              case greenland::IntrinsicHelper::DivLong:
                CvtBinOp(cu, kOpDiv, inst);
                break;
              case greenland::IntrinsicHelper::RemInt:
              case greenland::IntrinsicHelper::RemLong:
                CvtBinOp(cu, kOpRem, inst);
                break;
              case greenland::IntrinsicHelper::MethodInfo:
                // Already dealt with - just ignore it here.
                break;
              case greenland::IntrinsicHelper::CheckSuspend:
                cg->GenSuspendTest(cu, 0 /* opt_flags already applied */);
                break;
              case greenland::IntrinsicHelper::HLInvokeObj:
              case greenland::IntrinsicHelper::HLInvokeFloat:
              case greenland::IntrinsicHelper::HLInvokeDouble:
              case greenland::IntrinsicHelper::HLInvokeLong:
              case greenland::IntrinsicHelper::HLInvokeInt:
                CvtInvoke(cu, call_inst, false /* is_void */, false /* new_array */);
                break;
              case greenland::IntrinsicHelper::HLInvokeVoid:
                CvtInvoke(cu, call_inst, true /* is_void */, false /* new_array */);
                break;
              case greenland::IntrinsicHelper::HLFilledNewArray:
                CvtInvoke(cu, call_inst, false /* is_void */, true /* new_array */);
                break;
              case greenland::IntrinsicHelper::HLFillArrayData:
                CvtFillArrayData(cu, call_inst);
                break;
              case greenland::IntrinsicHelper::ConstString:
                CvtConstObject(cu, call_inst, true /* is_string */);
                break;
              case greenland::IntrinsicHelper::ConstClass:
                CvtConstObject(cu, call_inst, false /* is_string */);
                break;
              case greenland::IntrinsicHelper::HLCheckCast:
                CvtCheckCast(cu, call_inst);
                break;
              case greenland::IntrinsicHelper::NewInstance:
                CvtNewInstance(cu, call_inst);
                break;
              case greenland::IntrinsicHelper::HLSgetObject:
                CvtSget(cu, call_inst, false /* wide */, true /* Object */);
                break;
              case greenland::IntrinsicHelper::HLSget:
              case greenland::IntrinsicHelper::HLSgetFloat:
              case greenland::IntrinsicHelper::HLSgetBoolean:
              case greenland::IntrinsicHelper::HLSgetByte:
              case greenland::IntrinsicHelper::HLSgetChar:
              case greenland::IntrinsicHelper::HLSgetShort:
                CvtSget(cu, call_inst, false /* wide */, false /* Object */);
                break;
              case greenland::IntrinsicHelper::HLSgetWide:
              case greenland::IntrinsicHelper::HLSgetDouble:
                CvtSget(cu, call_inst, true /* wide */, false /* Object */);
                break;
              case greenland::IntrinsicHelper::HLSput:
              case greenland::IntrinsicHelper::HLSputFloat:
              case greenland::IntrinsicHelper::HLSputBoolean:
              case greenland::IntrinsicHelper::HLSputByte:
              case greenland::IntrinsicHelper::HLSputChar:
              case greenland::IntrinsicHelper::HLSputShort:
                CvtSput(cu, call_inst, false /* wide */, false /* Object */);
                break;
              case greenland::IntrinsicHelper::HLSputWide:
              case greenland::IntrinsicHelper::HLSputDouble:
                CvtSput(cu, call_inst, true /* wide */, false /* Object */);
                break;
              case greenland::IntrinsicHelper::HLSputObject:
                CvtSput(cu, call_inst, false /* wide */, true /* Object */);
                break;
              case greenland::IntrinsicHelper::GetException:
                CvtMoveException(cu, call_inst);
                break;
              case greenland::IntrinsicHelper::HLThrowException:
                CvtThrow(cu, call_inst);
                break;
              case greenland::IntrinsicHelper::MonitorEnter:
                CvtMonitorEnterExit(cu, true /* is_enter */, call_inst);
                break;
              case greenland::IntrinsicHelper::MonitorExit:
                CvtMonitorEnterExit(cu, false /* is_enter */, call_inst);
                break;
              case greenland::IntrinsicHelper::OptArrayLength:
                CvtArrayLength(cu, call_inst);
                break;
              case greenland::IntrinsicHelper::NewArray:
                CvtNewArray(cu, call_inst);
                break;
              case greenland::IntrinsicHelper::InstanceOf:
                CvtInstanceOf(cu, call_inst);
                break;

              case greenland::IntrinsicHelper::HLArrayGet:
              case greenland::IntrinsicHelper::HLArrayGetObject:
              case greenland::IntrinsicHelper::HLArrayGetFloat:
                CvtAget(cu, call_inst, kWord, 2);
                break;
              case greenland::IntrinsicHelper::HLArrayGetWide:
              case greenland::IntrinsicHelper::HLArrayGetDouble:
                CvtAget(cu, call_inst, kLong, 3);
                break;
              case greenland::IntrinsicHelper::HLArrayGetBoolean:
                CvtAget(cu, call_inst, kUnsignedByte, 0);
                break;
              case greenland::IntrinsicHelper::HLArrayGetByte:
                CvtAget(cu, call_inst, kSignedByte, 0);
                break;
              case greenland::IntrinsicHelper::HLArrayGetChar:
                CvtAget(cu, call_inst, kUnsignedHalf, 1);
                break;
              case greenland::IntrinsicHelper::HLArrayGetShort:
                CvtAget(cu, call_inst, kSignedHalf, 1);
                break;

              case greenland::IntrinsicHelper::HLArrayPut:
              case greenland::IntrinsicHelper::HLArrayPutFloat:
                CvtAputPrimitive(cu, call_inst, kWord, 2);
                break;
              case greenland::IntrinsicHelper::HLArrayPutObject:
                CvtAputObj(cu, call_inst);
                break;
              case greenland::IntrinsicHelper::HLArrayPutWide:
              case greenland::IntrinsicHelper::HLArrayPutDouble:
                CvtAputPrimitive(cu, call_inst, kLong, 3);
                break;
              case greenland::IntrinsicHelper::HLArrayPutBoolean:
                CvtAputPrimitive(cu, call_inst, kUnsignedByte, 0);
                break;
              case greenland::IntrinsicHelper::HLArrayPutByte:
                CvtAputPrimitive(cu, call_inst, kSignedByte, 0);
                break;
              case greenland::IntrinsicHelper::HLArrayPutChar:
                CvtAputPrimitive(cu, call_inst, kUnsignedHalf, 1);
                break;
              case greenland::IntrinsicHelper::HLArrayPutShort:
                CvtAputPrimitive(cu, call_inst, kSignedHalf, 1);
                break;

              case greenland::IntrinsicHelper::HLIGet:
              case greenland::IntrinsicHelper::HLIGetFloat:
                CvtIget(cu, call_inst, kWord, false /* is_wide */, false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetObject:
                CvtIget(cu, call_inst, kWord, false /* is_wide */, true /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetWide:
              case greenland::IntrinsicHelper::HLIGetDouble:
                CvtIget(cu, call_inst, kLong, true /* is_wide */, false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetBoolean:
                CvtIget(cu, call_inst, kUnsignedByte, false /* is_wide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetByte:
                CvtIget(cu, call_inst, kSignedByte, false /* is_wide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetChar:
                CvtIget(cu, call_inst, kUnsignedHalf, false /* is_wide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetShort:
                CvtIget(cu, call_inst, kSignedHalf, false /* is_wide */,
                        false /* obj */);
                break;

              case greenland::IntrinsicHelper::HLIPut:
              case greenland::IntrinsicHelper::HLIPutFloat:
                CvtIput(cu, call_inst, kWord, false /* is_wide */, false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutObject:
                CvtIput(cu, call_inst, kWord, false /* is_wide */, true /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutWide:
              case greenland::IntrinsicHelper::HLIPutDouble:
                CvtIput(cu, call_inst, kLong, true /* is_wide */, false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutBoolean:
                CvtIput(cu, call_inst, kUnsignedByte, false /* is_wide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutByte:
                CvtIput(cu, call_inst, kSignedByte, false /* is_wide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutChar:
                CvtIput(cu, call_inst, kUnsignedHalf, false /* is_wide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutShort:
                CvtIput(cu, call_inst, kSignedHalf, false /* is_wide */,
                        false /* obj */);
                break;

              case greenland::IntrinsicHelper::IntToChar:
                CvtIntNarrowing(cu, call_inst, Instruction::INT_TO_CHAR);
                break;
              case greenland::IntrinsicHelper::IntToShort:
                CvtIntNarrowing(cu, call_inst, Instruction::INT_TO_SHORT);
                break;
              case greenland::IntrinsicHelper::IntToByte:
                CvtIntNarrowing(cu, call_inst, Instruction::INT_TO_BYTE);
                break;

              case greenland::IntrinsicHelper::F2I:
              case greenland::IntrinsicHelper::D2I:
              case greenland::IntrinsicHelper::F2L:
              case greenland::IntrinsicHelper::D2L:
                CvtFPToInt(cu, call_inst);
                break;

              case greenland::IntrinsicHelper::CmplFloat:
                CvtFPCompare(cu, call_inst, Instruction::CMPL_FLOAT);
                break;
              case greenland::IntrinsicHelper::CmpgFloat:
                CvtFPCompare(cu, call_inst, Instruction::CMPG_FLOAT);
                break;
              case greenland::IntrinsicHelper::CmplDouble:
                CvtFPCompare(cu, call_inst, Instruction::CMPL_DOUBLE);
                break;
              case greenland::IntrinsicHelper::CmpgDouble:
                CvtFPCompare(cu, call_inst, Instruction::CMPG_DOUBLE);
                break;

              case greenland::IntrinsicHelper::CmpLong:
                CvtLongCompare(cu, call_inst);
                break;

              case greenland::IntrinsicHelper::SHLLong:
                CvtShiftOp(cu, Instruction::SHL_LONG, call_inst);
                break;
              case greenland::IntrinsicHelper::SHRLong:
                CvtShiftOp(cu, Instruction::SHR_LONG, call_inst);
                break;
              case greenland::IntrinsicHelper::USHRLong:
                CvtShiftOp(cu, Instruction::USHR_LONG, call_inst);
                break;
              case greenland::IntrinsicHelper::SHLInt:
                CvtShiftOp(cu, Instruction::SHL_INT, call_inst);
                break;
              case greenland::IntrinsicHelper::SHRInt:
                CvtShiftOp(cu, Instruction::SHR_INT, call_inst);
                break;
              case greenland::IntrinsicHelper::USHRInt:
                CvtShiftOp(cu, Instruction::USHR_INT, call_inst);
                break;

              case greenland::IntrinsicHelper::CatchTargets: {
                  llvm::SwitchInst* sw_inst =
                      llvm::dyn_cast<llvm::SwitchInst>(next_it);
                  DCHECK(sw_inst != NULL);
                  /*
                   * Discard the edges and the following conditional branch.
                   * Do a direct branch to the default target (which is the
                   * "work" portion of the pair.
                   * TODO: awful code layout - rework
                   */
                   llvm::BasicBlock* target_bb = sw_inst->getDefaultDest();
                   DCHECK(target_bb != NULL);
                   cg->OpUnconditionalBranch(cu, cu->block_to_label_map.Get(target_bb));
                   ++it;
                   // Set next bb to default target - improves code layout
                   next_bb = target_bb;
                }
                break;

              default:
                LOG(FATAL) << "Unexpected intrinsic " << cu->intrinsic_helper->GetName(id);
            }
          }
          break;

        case llvm::Instruction::Br: CvtBr(cu, inst); break;
        case llvm::Instruction::Add: CvtBinOp(cu, kOpAdd, inst); break;
        case llvm::Instruction::Sub: CvtBinOp(cu, kOpSub, inst); break;
        case llvm::Instruction::Mul: CvtBinOp(cu, kOpMul, inst); break;
        case llvm::Instruction::SDiv: CvtBinOp(cu, kOpDiv, inst); break;
        case llvm::Instruction::SRem: CvtBinOp(cu, kOpRem, inst); break;
        case llvm::Instruction::And: CvtBinOp(cu, kOpAnd, inst); break;
        case llvm::Instruction::Or: CvtBinOp(cu, kOpOr, inst); break;
        case llvm::Instruction::Xor: CvtBinOp(cu, kOpXor, inst); break;
        case llvm::Instruction::PHI: CvtPhi(cu, inst); break;
        case llvm::Instruction::Ret: CvtRet(cu, inst); break;
        case llvm::Instruction::FAdd: CvtBinFPOp(cu, kOpAdd, inst); break;
        case llvm::Instruction::FSub: CvtBinFPOp(cu, kOpSub, inst); break;
        case llvm::Instruction::FMul: CvtBinFPOp(cu, kOpMul, inst); break;
        case llvm::Instruction::FDiv: CvtBinFPOp(cu, kOpDiv, inst); break;
        case llvm::Instruction::FRem: CvtBinFPOp(cu, kOpRem, inst); break;
        case llvm::Instruction::SIToFP: CvtIntToFP(cu, inst); break;
        case llvm::Instruction::FPTrunc: CvtDoubleToFloat(cu, inst); break;
        case llvm::Instruction::FPExt: CvtFloatToDouble(cu, inst); break;
        case llvm::Instruction::Trunc: CvtTrunc(cu, inst); break;

        case llvm::Instruction::ZExt: CvtIntExt(cu, inst, false /* signed */);
          break;
        case llvm::Instruction::SExt: CvtIntExt(cu, inst, true /* signed */);
          break;

        case llvm::Instruction::Switch: CvtSwitch(cu, inst); break;

        case llvm::Instruction::Unreachable:
          break;  // FIXME: can we really ignore these?

        case llvm::Instruction::Shl:
        case llvm::Instruction::LShr:
        case llvm::Instruction::AShr:
        case llvm::Instruction::Invoke:
        case llvm::Instruction::FPToUI:
        case llvm::Instruction::FPToSI:
        case llvm::Instruction::UIToFP:
        case llvm::Instruction::PtrToInt:
        case llvm::Instruction::IntToPtr:
        case llvm::Instruction::FCmp:
        case llvm::Instruction::URem:
        case llvm::Instruction::UDiv:
        case llvm::Instruction::Resume:
        case llvm::Instruction::Alloca:
        case llvm::Instruction::GetElementPtr:
        case llvm::Instruction::Fence:
        case llvm::Instruction::AtomicCmpXchg:
        case llvm::Instruction::AtomicRMW:
        case llvm::Instruction::BitCast:
        case llvm::Instruction::VAArg:
        case llvm::Instruction::Select:
        case llvm::Instruction::UserOp1:
        case llvm::Instruction::UserOp2:
        case llvm::Instruction::ExtractElement:
        case llvm::Instruction::InsertElement:
        case llvm::Instruction::ShuffleVector:
        case llvm::Instruction::ExtractValue:
        case llvm::Instruction::InsertValue:
        case llvm::Instruction::LandingPad:
        case llvm::Instruction::IndirectBr:
        case llvm::Instruction::Load:
        case llvm::Instruction::Store:
          LOG(FATAL) << "Unexpected llvm opcode: " << opcode; break;

        default:
          LOG(FATAL) << "Unknown llvm opcode: " << inst->getOpcodeName();
          break;
      }
    }

    if (head_lir != NULL) {
      ApplyLocalOptimizations(cu, head_lir, cu->last_lir_insn);
    }
    if (next_bb != NULL) {
      bb = next_bb;
      next_bb = NULL;
    }
  }
  return false;
}

/*
 * Convert LLVM_IR to MIR:
 *   o Iterate through the LLVM_IR and construct a graph using
 *     standard MIR building blocks.
 *   o Perform a basic-block optimization pass to remove unnecessary
 *     store/load sequences.
 *   o Convert the LLVM Value operands into RegLocations where applicable.
 *   o Create ssa_rep def/use operand arrays for each converted LLVM opcode
 *   o Perform register promotion
 *   o Iterate through the graph a basic block at a time, generating
 *     LIR.
 *   o Assemble LIR as usual.
 *   o Profit.
 */
void MethodBitcode2LIR(CompilationUnit* cu)
{
  Codegen* cg = cu->cg.get();
  llvm::Function* func = cu->func;
  int num_basic_blocks = func->getBasicBlockList().size();
  // Allocate a list for LIR basic block labels
  cu->block_label_list =
    static_cast<LIR*>(NewMem(cu, sizeof(LIR) * num_basic_blocks, true, kAllocLIR));
  LIR* label_list = cu->block_label_list;
  int next_label = 0;
  for (llvm::Function::iterator i = func->begin(), e = func->end(); i != e; ++i) {
    cu->block_to_label_map.Put(static_cast<llvm::BasicBlock*>(i),
                               &label_list[next_label++]);
  }

  /*
   * Keep honest - clear reg_locations, Value => RegLocation,
   * promotion map and VmapTables.
   */
  cu->loc_map.clear();  // Start fresh
  cu->reg_location = NULL;
  for (int i = 0; i < cu->num_dalvik_registers + cu->num_compiler_temps + 1; i++) {
    cu->promotion_map[i].core_location = kLocDalvikFrame;
    cu->promotion_map[i].fp_location = kLocDalvikFrame;
  }
  cu->core_spill_mask = 0;
  cu->num_core_spills = 0;
  cu->fp_spill_mask = 0;
  cu->num_fp_spills = 0;
  cu->core_vmap_table.clear();
  cu->fp_vmap_table.clear();

  /*
   * At this point, we've lost all knowledge of register promotion.
   * Rebuild that info from the MethodInfo intrinsic (if it
   * exists - not required for correctness).  Normally, this will
   * be the first instruction we encounter, so we won't have to iterate
   * through everything.
   */
  for (llvm::inst_iterator i = llvm::inst_begin(func), e = llvm::inst_end(func); i != e; ++i) {
    llvm::CallInst* call_inst = llvm::dyn_cast<llvm::CallInst>(&*i);
    if (call_inst != NULL) {
      llvm::Function* callee = call_inst->getCalledFunction();
      greenland::IntrinsicHelper::IntrinsicId id =
          cu->intrinsic_helper->GetIntrinsicId(callee);
      if (id == greenland::IntrinsicHelper::MethodInfo) {
        if (cu->verbose) {
          LOG(INFO) << "Found MethodInfo";
        }
        llvm::MDNode* reg_info_node = call_inst->getMetadata("RegInfo");
        if (reg_info_node != NULL) {
          llvm::ConstantInt* num_ins_value =
            static_cast<llvm::ConstantInt*>(reg_info_node->getOperand(0));
          llvm::ConstantInt* num_regs_value =
            static_cast<llvm::ConstantInt*>(reg_info_node->getOperand(1));
          llvm::ConstantInt* num_outs_value =
            static_cast<llvm::ConstantInt*>(reg_info_node->getOperand(2));
          llvm::ConstantInt* num_compiler_temps_value =
            static_cast<llvm::ConstantInt*>(reg_info_node->getOperand(3));
          llvm::ConstantInt* num_ssa_regs_value =
            static_cast<llvm::ConstantInt*>(reg_info_node->getOperand(4));
          if (cu->verbose) {
             LOG(INFO) << "RegInfo - Ins:" << num_ins_value->getZExtValue()
                       << ", Regs:" << num_regs_value->getZExtValue()
                       << ", Outs:" << num_outs_value->getZExtValue()
                       << ", CTemps:" << num_compiler_temps_value->getZExtValue()
                       << ", SSARegs:" << num_ssa_regs_value->getZExtValue();
            }
          }
        llvm::MDNode* pmap_info_node = call_inst->getMetadata("PromotionMap");
        if (pmap_info_node != NULL) {
          int elems = pmap_info_node->getNumOperands();
          if (cu->verbose) {
            LOG(INFO) << "PMap size: " << elems;
          }
          for (int i = 0; i < elems; i++) {
            llvm::ConstantInt* raw_map_data =
                static_cast<llvm::ConstantInt*>(pmap_info_node->getOperand(i));
            uint32_t map_data = raw_map_data->getZExtValue();
            PromotionMap* p = &cu->promotion_map[i];
            p->first_in_pair = (map_data >> 24) & 0xff;
            p->FpReg = (map_data >> 16) & 0xff;
            p->core_reg = (map_data >> 8) & 0xff;
            p->fp_location = static_cast<RegLocationType>((map_data >> 4) & 0xf);
            if (p->fp_location == kLocPhysReg) {
              RecordFpPromotion(cu, p->FpReg, i);
            }
            p->core_location = static_cast<RegLocationType>(map_data & 0xf);
            if (p->core_location == kLocPhysReg) {
              RecordCorePromotion(cu, p->core_reg, i);
            }
          }
          if (cu->verbose) {
            DumpPromotionMap(cu);
          }
        }
        break;
      }
    }
  }
  cg->AdjustSpillMask(cu);
  cu->frame_size = ComputeFrameSize(cu);

  // Create RegLocations for arguments
  llvm::Function::arg_iterator it(cu->func->arg_begin());
  llvm::Function::arg_iterator it_end(cu->func->arg_end());
  for (; it != it_end; ++it) {
    llvm::Value* val = it;
    CreateLocFromValue(cu, val);
  }
  // Create RegLocations for all non-argument defintions
  for (llvm::inst_iterator i = llvm::inst_begin(func), e = llvm::inst_end(func); i != e; ++i) {
    llvm::Value* val = &*i;
    if (val->hasName() && (val->getName().str().c_str()[0] == 'v')) {
      CreateLocFromValue(cu, val);
    }
  }

  // Walk the blocks, generating code.
  for (llvm::Function::iterator i = cu->func->begin(), e = cu->func->end(); i != e; ++i) {
    BitcodeBlockCodeGen(cu, static_cast<llvm::BasicBlock*>(i));
  }

  cg->HandleSuspendLaunchPads(cu);

  cg->HandleThrowLaunchPads(cu);

  cg->HandleIntrinsicLaunchPads(cu);

  cu->func->eraseFromParent();
  cu->func = NULL;
}


}  // namespace art
