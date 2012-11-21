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
#include "method_codegen_driver.h"
#include "local_optimizations.h"
#include "codegen_util.h"
#include "ralloc_util.h"

static const char* kLabelFormat = "%c0x%x_%d";
static const char kInvalidBlock = 0xff;
static const char kNormalBlock = 'L';
static const char kCatchBlock = 'C';

namespace art {
// TODO: unify badLoc
const RegLocation badLoc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0,
                            INVALID_REG, INVALID_REG, INVALID_SREG,
                            INVALID_SREG};
RegLocation GetLoc(CompilationUnit* cUnit, llvm::Value* val);

llvm::BasicBlock* GetLLVMBlock(CompilationUnit* cUnit, int id)
{
  return cUnit->idToBlockMap.Get(id);
}

llvm::Value* GetLLVMValue(CompilationUnit* cUnit, int sReg)
{
  return reinterpret_cast<llvm::Value*>(GrowableListGetElement(&cUnit->llvmValues, sReg));
}

// Replace the placeholder value with the real definition
void DefineValue(CompilationUnit* cUnit, llvm::Value* val, int sReg)
{
  llvm::Value* placeholder = GetLLVMValue(cUnit, sReg);
  if (placeholder == NULL) {
    // This can happen on instruction rewrite on verification failure
    LOG(WARNING) << "Null placeholder";
    return;
  }
  placeholder->replaceAllUsesWith(val);
  val->takeName(placeholder);
  cUnit->llvmValues.elemList[sReg] = reinterpret_cast<uintptr_t>(val);
  llvm::Instruction* inst = llvm::dyn_cast<llvm::Instruction>(placeholder);
  DCHECK(inst != NULL);
  inst->eraseFromParent();

  // Set vreg for debugging
  if (!cUnit->compiler->IsDebuggingSupported()) {
    greenland::IntrinsicHelper::IntrinsicId id =
        greenland::IntrinsicHelper::SetVReg;
    llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
    int vReg = SRegToVReg(cUnit, sReg);
    llvm::Value* tableSlot = cUnit->irb->getInt32(vReg);
    llvm::Value* args[] = { tableSlot, val };
    cUnit->irb->CreateCall(func, args);
  }
}

llvm::Type* LlvmTypeFromLocRec(CompilationUnit* cUnit, RegLocation loc)
{
  llvm::Type* res = NULL;
  if (loc.wide) {
    if (loc.fp)
        res = cUnit->irb->getDoubleTy();
    else
        res = cUnit->irb->getInt64Ty();
  } else {
    if (loc.fp) {
      res = cUnit->irb->getFloatTy();
    } else {
      if (loc.ref)
        res = cUnit->irb->GetJObjectTy();
      else
        res = cUnit->irb->getInt32Ty();
    }
  }
  return res;
}

/* Create an in-memory RegLocation from an llvm Value. */
void CreateLocFromValue(CompilationUnit* cUnit, llvm::Value* val)
{
  // NOTE: llvm takes shortcuts with c_str() - get to std::string firstt
  std::string s(val->getName().str());
  const char* valName = s.c_str();
  SafeMap<llvm::Value*, RegLocation>::iterator it = cUnit->locMap.find(val);
  DCHECK(it == cUnit->locMap.end()) << " - already defined: " << valName;
  int baseSReg = INVALID_SREG;
  int subscript = -1;
  sscanf(valName, "v%d_%d", &baseSReg, &subscript);
  if ((baseSReg == INVALID_SREG) && (!strcmp(valName, "method"))) {
    baseSReg = SSA_METHOD_BASEREG;
    subscript = 0;
  }
  DCHECK_NE(baseSReg, INVALID_SREG);
  DCHECK_NE(subscript, -1);
  // TODO: redo during C++'ification
  RegLocation loc =  {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0, INVALID_REG,
                      INVALID_REG, INVALID_SREG, INVALID_SREG};
  llvm::Type* ty = val->getType();
  loc.wide = ((ty == cUnit->irb->getInt64Ty()) ||
              (ty == cUnit->irb->getDoubleTy()));
  loc.defined = true;
  loc.home = false;  // May change during promotion
  loc.sRegLow = baseSReg;
  loc.origSReg = cUnit->locMap.size();
  PromotionMap pMap = cUnit->promotionMap[baseSReg];
  if (ty == cUnit->irb->getFloatTy()) {
    loc.fp = true;
    if (pMap.fpLocation == kLocPhysReg) {
      loc.lowReg = pMap.FpReg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  } else if (ty == cUnit->irb->getDoubleTy()) {
    loc.fp = true;
    PromotionMap pMapHigh = cUnit->promotionMap[baseSReg + 1];
    if ((pMap.fpLocation == kLocPhysReg) &&
        (pMapHigh.fpLocation == kLocPhysReg) &&
        ((pMap.FpReg & 0x1) == 0) &&
        (pMap.FpReg + 1 == pMapHigh.FpReg)) {
      loc.lowReg = pMap.FpReg;
      loc.highReg = pMapHigh.FpReg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  } else if (ty == cUnit->irb->GetJObjectTy()) {
    loc.ref = true;
    if (pMap.coreLocation == kLocPhysReg) {
      loc.lowReg = pMap.coreReg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  } else if (ty == cUnit->irb->getInt64Ty()) {
    loc.core = true;
    PromotionMap pMapHigh = cUnit->promotionMap[baseSReg + 1];
    if ((pMap.coreLocation == kLocPhysReg) &&
        (pMapHigh.coreLocation == kLocPhysReg)) {
      loc.lowReg = pMap.coreReg;
      loc.highReg = pMapHigh.coreReg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  } else {
    loc.core = true;
    if (pMap.coreLocation == kLocPhysReg) {
      loc.lowReg = pMap.coreReg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  }

  if (cUnit->printMe && loc.home) {
    if (loc.wide) {
      LOG(INFO) << "Promoted wide " << s << " to regs " << loc.lowReg << "/" << loc.highReg;
    } else {
      LOG(INFO) << "Promoted " << s << " to reg " << loc.lowReg;
    }
  }
  cUnit->locMap.Put(val, loc);
}
void InitIR(CompilationUnit* cUnit)
{
  LLVMInfo* llvmInfo = cUnit->llvm_info;
  if (llvmInfo == NULL) {
    CompilerTls* tls = cUnit->compiler->GetTls();
    CHECK(tls != NULL);
    llvmInfo = static_cast<LLVMInfo*>(tls->GetLLVMInfo());
    if (llvmInfo == NULL) {
      llvmInfo = new LLVMInfo();
      tls->SetLLVMInfo(llvmInfo);
    }
  }
  cUnit->context = llvmInfo->GetLLVMContext();
  cUnit->module = llvmInfo->GetLLVMModule();
  cUnit->intrinsic_helper = llvmInfo->GetIntrinsicHelper();
  cUnit->irb = llvmInfo->GetIRBuilder();
}

const char* LlvmSSAName(CompilationUnit* cUnit, int ssaReg) {
  return GET_ELEM_N(cUnit->ssaStrings, char*, ssaReg);
}

llvm::BasicBlock* FindCaseTarget(CompilationUnit* cUnit, uint32_t vaddr)
{
  BasicBlock* bb = FindBlock(cUnit, vaddr);
  DCHECK(bb != NULL);
  return GetLLVMBlock(cUnit, bb->id);
}

void ConvertPackedSwitch(CompilationUnit* cUnit, BasicBlock* bb,
                         int32_t tableOffset, RegLocation rlSrc)
{
  const Instruction::PackedSwitchPayload* payload =
      reinterpret_cast<const Instruction::PackedSwitchPayload*>(
      cUnit->insns + cUnit->currentDalvikOffset + tableOffset);

  llvm::Value* value = GetLLVMValue(cUnit, rlSrc.origSReg);

  llvm::SwitchInst* sw =
    cUnit->irb->CreateSwitch(value, GetLLVMBlock(cUnit, bb->fallThrough->id),
                             payload->case_count);

  for (uint16_t i = 0; i < payload->case_count; ++i) {
    llvm::BasicBlock* llvmBB =
        FindCaseTarget(cUnit, cUnit->currentDalvikOffset + payload->targets[i]);
    sw->addCase(cUnit->irb->getInt32(payload->first_key + i), llvmBB);
  }
  llvm::MDNode* switchNode =
      llvm::MDNode::get(*cUnit->context, cUnit->irb->getInt32(tableOffset));
  sw->setMetadata("SwitchTable", switchNode);
  bb->taken = NULL;
  bb->fallThrough = NULL;
}

void ConvertSparseSwitch(CompilationUnit* cUnit, BasicBlock* bb,
                         int32_t tableOffset, RegLocation rlSrc)
{
  const Instruction::SparseSwitchPayload* payload =
      reinterpret_cast<const Instruction::SparseSwitchPayload*>(
      cUnit->insns + cUnit->currentDalvikOffset + tableOffset);

  const int32_t* keys = payload->GetKeys();
  const int32_t* targets = payload->GetTargets();

  llvm::Value* value = GetLLVMValue(cUnit, rlSrc.origSReg);

  llvm::SwitchInst* sw =
    cUnit->irb->CreateSwitch(value, GetLLVMBlock(cUnit, bb->fallThrough->id),
                             payload->case_count);

  for (size_t i = 0; i < payload->case_count; ++i) {
    llvm::BasicBlock* llvmBB =
        FindCaseTarget(cUnit, cUnit->currentDalvikOffset + targets[i]);
    sw->addCase(cUnit->irb->getInt32(keys[i]), llvmBB);
  }
  llvm::MDNode* switchNode =
      llvm::MDNode::get(*cUnit->context, cUnit->irb->getInt32(tableOffset));
  sw->setMetadata("SwitchTable", switchNode);
  bb->taken = NULL;
  bb->fallThrough = NULL;
}

void ConvertSget(CompilationUnit* cUnit, int32_t fieldIndex,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlDest)
{
  llvm::Constant* fieldIdx = cUnit->irb->getInt32(fieldIndex);
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cUnit->irb->CreateCall(intr, fieldIdx);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertSput(CompilationUnit* cUnit, int32_t fieldIndex,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlSrc)
{
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(fieldIndex));
  args.push_back(GetLLVMValue(cUnit, rlSrc.origSReg));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(intr, args);
}

void ConvertFillArrayData(CompilationUnit* cUnit, int32_t offset,
                          RegLocation rlArray)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::HLFillArrayData;
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(offset));
  args.push_back(GetLLVMValue(cUnit, rlArray.origSReg));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(intr, args);
}

llvm::Value* EmitConst(CompilationUnit* cUnit, llvm::ArrayRef<llvm::Value*> src,
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
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  return cUnit->irb->CreateCall(intr, src);
}

void EmitPopShadowFrame(CompilationUnit* cUnit)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::PopShadowFrame);
  cUnit->irb->CreateCall(intr);
}

llvm::Value* EmitCopy(CompilationUnit* cUnit, llvm::ArrayRef<llvm::Value*> src,
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
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  return cUnit->irb->CreateCall(intr, src);
}

void ConvertMoveException(CompilationUnit* cUnit, RegLocation rlDest)
{
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::GetException);
  llvm::Value* res = cUnit->irb->CreateCall(func);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertThrow(CompilationUnit* cUnit, RegLocation rlSrc)
{
  llvm::Value* src = GetLLVMValue(cUnit, rlSrc.origSReg);
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::HLThrowException);
  cUnit->irb->CreateCall(func, src);
}

void ConvertMonitorEnterExit(CompilationUnit* cUnit, int optFlags,
                             greenland::IntrinsicHelper::IntrinsicId id,
                             RegLocation rlSrc)
{
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(GetLLVMValue(cUnit, rlSrc.origSReg));
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(func, args);
}

void ConvertArrayLength(CompilationUnit* cUnit, int optFlags,
                        RegLocation rlDest, RegLocation rlSrc)
{
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(GetLLVMValue(cUnit, rlSrc.origSReg));
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::OptArrayLength);
  llvm::Value* res = cUnit->irb->CreateCall(func, args);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void EmitSuspendCheck(CompilationUnit* cUnit)
{
  greenland::IntrinsicHelper::IntrinsicId id =
      greenland::IntrinsicHelper::CheckSuspend;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(intr);
}

llvm::Value* ConvertCompare(CompilationUnit* cUnit, ConditionCode cc,
                            llvm::Value* src1, llvm::Value* src2)
{
  llvm::Value* res = NULL;
  DCHECK_EQ(src1->getType(), src2->getType());
  switch(cc) {
    case kCondEq: res = cUnit->irb->CreateICmpEQ(src1, src2); break;
    case kCondNe: res = cUnit->irb->CreateICmpNE(src1, src2); break;
    case kCondLt: res = cUnit->irb->CreateICmpSLT(src1, src2); break;
    case kCondGe: res = cUnit->irb->CreateICmpSGE(src1, src2); break;
    case kCondGt: res = cUnit->irb->CreateICmpSGT(src1, src2); break;
    case kCondLe: res = cUnit->irb->CreateICmpSLE(src1, src2); break;
    default: LOG(FATAL) << "Unexpected cc value " << cc;
  }
  return res;
}

void ConvertCompareAndBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                             ConditionCode cc, RegLocation rlSrc1,
                             RegLocation rlSrc2)
{
  if (bb->taken->startOffset <= mir->offset) {
    EmitSuspendCheck(cUnit);
  }
  llvm::Value* src1 = GetLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2 = GetLLVMValue(cUnit, rlSrc2.origSReg);
  llvm::Value* condValue = ConvertCompare(cUnit, cc, src1, src2);
  condValue->setName(StringPrintf("t%d", cUnit->tempName++));
  cUnit->irb->CreateCondBr(condValue, GetLLVMBlock(cUnit, bb->taken->id),
                           GetLLVMBlock(cUnit, bb->fallThrough->id));
  // Don't redo the fallthrough branch in the BB driver
  bb->fallThrough = NULL;
}

void ConvertCompareZeroAndBranch(CompilationUnit* cUnit, BasicBlock* bb,
                                 MIR* mir, ConditionCode cc, RegLocation rlSrc1)
{
  if (bb->taken->startOffset <= mir->offset) {
    EmitSuspendCheck(cUnit);
  }
  llvm::Value* src1 = GetLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2;
  if (rlSrc1.ref) {
    src2 = cUnit->irb->GetJNull();
  } else {
    src2 = cUnit->irb->getInt32(0);
  }
  llvm::Value* condValue = ConvertCompare(cUnit, cc, src1, src2);
  cUnit->irb->CreateCondBr(condValue, GetLLVMBlock(cUnit, bb->taken->id),
                           GetLLVMBlock(cUnit, bb->fallThrough->id));
  // Don't redo the fallthrough branch in the BB driver
  bb->fallThrough = NULL;
}

llvm::Value* GenDivModOp(CompilationUnit* cUnit, bool isDiv, bool isLong,
                         llvm::Value* src1, llvm::Value* src2)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  if (isLong) {
    if (isDiv) {
      id = greenland::IntrinsicHelper::DivLong;
    } else {
      id = greenland::IntrinsicHelper::RemLong;
    }
  } else {
    if (isDiv) {
      id = greenland::IntrinsicHelper::DivInt;
    } else {
      id = greenland::IntrinsicHelper::RemInt;
    }
  }
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2>args;
  args.push_back(src1);
  args.push_back(src2);
  return cUnit->irb->CreateCall(intr, args);
}

llvm::Value* GenArithOp(CompilationUnit* cUnit, OpKind op, bool isLong,
                        llvm::Value* src1, llvm::Value* src2)
{
  llvm::Value* res = NULL;
  switch(op) {
    case kOpAdd: res = cUnit->irb->CreateAdd(src1, src2); break;
    case kOpSub: res = cUnit->irb->CreateSub(src1, src2); break;
    case kOpRsub: res = cUnit->irb->CreateSub(src2, src1); break;
    case kOpMul: res = cUnit->irb->CreateMul(src1, src2); break;
    case kOpOr: res = cUnit->irb->CreateOr(src1, src2); break;
    case kOpAnd: res = cUnit->irb->CreateAnd(src1, src2); break;
    case kOpXor: res = cUnit->irb->CreateXor(src1, src2); break;
    case kOpDiv: res = GenDivModOp(cUnit, true, isLong, src1, src2); break;
    case kOpRem: res = GenDivModOp(cUnit, false, isLong, src1, src2); break;
    case kOpLsl: res = cUnit->irb->CreateShl(src1, src2); break;
    case kOpLsr: res = cUnit->irb->CreateLShr(src1, src2); break;
    case kOpAsr: res = cUnit->irb->CreateAShr(src1, src2); break;
    default:
      LOG(FATAL) << "Invalid op " << op;
  }
  return res;
}

void ConvertFPArithOp(CompilationUnit* cUnit, OpKind op, RegLocation rlDest,
                      RegLocation rlSrc1, RegLocation rlSrc2)
{
  llvm::Value* src1 = GetLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2 = GetLLVMValue(cUnit, rlSrc2.origSReg);
  llvm::Value* res = NULL;
  switch(op) {
    case kOpAdd: res = cUnit->irb->CreateFAdd(src1, src2); break;
    case kOpSub: res = cUnit->irb->CreateFSub(src1, src2); break;
    case kOpMul: res = cUnit->irb->CreateFMul(src1, src2); break;
    case kOpDiv: res = cUnit->irb->CreateFDiv(src1, src2); break;
    case kOpRem: res = cUnit->irb->CreateFRem(src1, src2); break;
    default:
      LOG(FATAL) << "Invalid op " << op;
  }
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertShift(CompilationUnit* cUnit,
                  greenland::IntrinsicHelper::IntrinsicId id,
                  RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2>args;
  args.push_back(GetLLVMValue(cUnit, rlSrc1.origSReg));
  args.push_back(GetLLVMValue(cUnit, rlSrc2.origSReg));
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertShiftLit(CompilationUnit* cUnit,
                     greenland::IntrinsicHelper::IntrinsicId id,
                     RegLocation rlDest, RegLocation rlSrc, int shiftAmount)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2>args;
  args.push_back(GetLLVMValue(cUnit, rlSrc.origSReg));
  args.push_back(cUnit->irb->getInt32(shiftAmount));
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertArithOp(CompilationUnit* cUnit, OpKind op, RegLocation rlDest,
                    RegLocation rlSrc1, RegLocation rlSrc2)
{
  llvm::Value* src1 = GetLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2 = GetLLVMValue(cUnit, rlSrc2.origSReg);
  DCHECK_EQ(src1->getType(), src2->getType());
  llvm::Value* res = GenArithOp(cUnit, op, rlDest.wide, src1, src2);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void SetShadowFrameEntry(CompilationUnit* cUnit, llvm::Value* newVal)
{
  int index = -1;
  DCHECK(newVal != NULL);
  int vReg = SRegToVReg(cUnit, GetLoc(cUnit, newVal).origSReg);
  for (int i = 0; i < cUnit->numShadowFrameEntries; i++) {
    if (cUnit->shadowMap[i] == vReg) {
      index = i;
      break;
    }
  }
  if (index == -1) {
    return;
  }
  llvm::Type* ty = newVal->getType();
  greenland::IntrinsicHelper::IntrinsicId id =
      greenland::IntrinsicHelper::SetShadowFrameEntry;
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* tableSlot = cUnit->irb->getInt32(index);
  // If newVal is a Null pointer, we'll see it here as a const int.  Replace
  if (!ty->isPointerTy()) {
    // TODO: assert newVal created w/ dex_lang_const_int(0) or dex_lang_const_float(0)
    newVal = cUnit->irb->GetJNull();
  }
  llvm::Value* args[] = { newVal, tableSlot };
  cUnit->irb->CreateCall(func, args);
}

void ConvertArithOpLit(CompilationUnit* cUnit, OpKind op, RegLocation rlDest,
                       RegLocation rlSrc1, int32_t imm)
{
  llvm::Value* src1 = GetLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2 = cUnit->irb->getInt32(imm);
  llvm::Value* res = GenArithOp(cUnit, op, rlDest.wide, src1, src2);
  DefineValue(cUnit, res, rlDest.origSReg);
}

/*
 * Process arguments for invoke.  Note: this code is also used to
 * collect and process arguments for NEW_FILLED_ARRAY and NEW_FILLED_ARRAY_RANGE.
 * The requirements are similar.
 */
void ConvertInvoke(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                   InvokeType invokeType, bool isRange, bool isFilledNewArray)
{
  CallInfo* info = NewMemCallInfo(cUnit, bb, mir, invokeType, isRange);
  llvm::SmallVector<llvm::Value*, 10> args;
  // Insert the invokeType
  args.push_back(cUnit->irb->getInt32(static_cast<int>(invokeType)));
  // Insert the method_idx
  args.push_back(cUnit->irb->getInt32(info->index));
  // Insert the optimization flags
  args.push_back(cUnit->irb->getInt32(info->optFlags));
  // Now, insert the actual arguments
  for (int i = 0; i < info->numArgWords;) {
    llvm::Value* val = GetLLVMValue(cUnit, info->args[i].origSReg);
    args.push_back(val);
    i += info->args[i].wide ? 2 : 1;
  }
  /*
   * Choose the invoke return type based on actual usage.  Note: may
   * be different than shorty.  For example, if a function return value
   * is not used, we'll treat this as a void invoke.
   */
  greenland::IntrinsicHelper::IntrinsicId id;
  if (isFilledNewArray) {
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
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  if (info->result.location != kLocInvalid) {
    DefineValue(cUnit, res, info->result.origSReg);
    if (info->result.ref) {
      SetShadowFrameEntry(cUnit, reinterpret_cast<llvm::Value*>
                          (cUnit->llvmValues.elemList[info->result.origSReg]));
    }
  }
}

void ConvertConstObject(CompilationUnit* cUnit, uint32_t idx,
                        greenland::IntrinsicHelper::IntrinsicId id,
                        RegLocation rlDest)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* index = cUnit->irb->getInt32(idx);
  llvm::Value* res = cUnit->irb->CreateCall(intr, index);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertCheckCast(CompilationUnit* cUnit, uint32_t type_idx,
                      RegLocation rlSrc)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::HLCheckCast;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(type_idx));
  args.push_back(GetLLVMValue(cUnit, rlSrc.origSReg));
  cUnit->irb->CreateCall(intr, args);
}

void ConvertNewInstance(CompilationUnit* cUnit, uint32_t type_idx,
                        RegLocation rlDest)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::NewInstance;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* index = cUnit->irb->getInt32(type_idx);
  llvm::Value* res = cUnit->irb->CreateCall(intr, index);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertNewArray(CompilationUnit* cUnit, uint32_t type_idx,
                     RegLocation rlDest, RegLocation rlSrc)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::NewArray;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(type_idx));
  args.push_back(GetLLVMValue(cUnit, rlSrc.origSReg));
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertAget(CompilationUnit* cUnit, int optFlags,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlDest, RegLocation rlArray, RegLocation rlIndex)
{
  llvm::SmallVector<llvm::Value*, 3> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(GetLLVMValue(cUnit, rlArray.origSReg));
  args.push_back(GetLLVMValue(cUnit, rlIndex.origSReg));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertAput(CompilationUnit* cUnit, int optFlags,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlSrc, RegLocation rlArray, RegLocation rlIndex)
{
  llvm::SmallVector<llvm::Value*, 4> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(GetLLVMValue(cUnit, rlSrc.origSReg));
  args.push_back(GetLLVMValue(cUnit, rlArray.origSReg));
  args.push_back(GetLLVMValue(cUnit, rlIndex.origSReg));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(intr, args);
}

void ConvertIget(CompilationUnit* cUnit, int optFlags,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlDest, RegLocation rlObj, int fieldIndex)
{
  llvm::SmallVector<llvm::Value*, 3> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(GetLLVMValue(cUnit, rlObj.origSReg));
  args.push_back(cUnit->irb->getInt32(fieldIndex));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertIput(CompilationUnit* cUnit, int optFlags,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlSrc, RegLocation rlObj, int fieldIndex)
{
  llvm::SmallVector<llvm::Value*, 4> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(GetLLVMValue(cUnit, rlSrc.origSReg));
  args.push_back(GetLLVMValue(cUnit, rlObj.origSReg));
  args.push_back(cUnit->irb->getInt32(fieldIndex));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(intr, args);
}

void ConvertInstanceOf(CompilationUnit* cUnit, uint32_t type_idx,
                       RegLocation rlDest, RegLocation rlSrc)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::InstanceOf;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(type_idx));
  args.push_back(GetLLVMValue(cUnit, rlSrc.origSReg));
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertIntToLong(CompilationUnit* cUnit, RegLocation rlDest,
                      RegLocation rlSrc)
{
  llvm::Value* res = cUnit->irb->CreateSExt(GetLLVMValue(cUnit, rlSrc.origSReg),
                                            cUnit->irb->getInt64Ty());
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertLongToInt(CompilationUnit* cUnit, RegLocation rlDest,
                      RegLocation rlSrc)
{
  llvm::Value* src = GetLLVMValue(cUnit, rlSrc.origSReg);
  llvm::Value* res = cUnit->irb->CreateTrunc(src, cUnit->irb->getInt32Ty());
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertFloatToDouble(CompilationUnit* cUnit, RegLocation rlDest,
                          RegLocation rlSrc)
{
  llvm::Value* src = GetLLVMValue(cUnit, rlSrc.origSReg);
  llvm::Value* res = cUnit->irb->CreateFPExt(src, cUnit->irb->getDoubleTy());
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertDoubleToFloat(CompilationUnit* cUnit, RegLocation rlDest,
                          RegLocation rlSrc)
{
  llvm::Value* src = GetLLVMValue(cUnit, rlSrc.origSReg);
  llvm::Value* res = cUnit->irb->CreateFPTrunc(src, cUnit->irb->getFloatTy());
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertWideComparison(CompilationUnit* cUnit,
                           greenland::IntrinsicHelper::IntrinsicId id,
                           RegLocation rlDest, RegLocation rlSrc1,
                           RegLocation rlSrc2)
{
  DCHECK_EQ(rlSrc1.fp, rlSrc2.fp);
  DCHECK_EQ(rlSrc1.wide, rlSrc2.wide);
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(GetLLVMValue(cUnit, rlSrc1.origSReg));
  args.push_back(GetLLVMValue(cUnit, rlSrc2.origSReg));
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertIntNarrowing(CompilationUnit* cUnit, RegLocation rlDest,
                         RegLocation rlSrc,
                         greenland::IntrinsicHelper::IntrinsicId id)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res =
      cUnit->irb->CreateCall(intr, GetLLVMValue(cUnit, rlSrc.origSReg));
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertNeg(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  llvm::Value* res = cUnit->irb->CreateNeg(GetLLVMValue(cUnit, rlSrc.origSReg));
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertIntToFP(CompilationUnit* cUnit, llvm::Type* ty, RegLocation rlDest,
                    RegLocation rlSrc)
{
  llvm::Value* res =
      cUnit->irb->CreateSIToFP(GetLLVMValue(cUnit, rlSrc.origSReg), ty);
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertFPToInt(CompilationUnit* cUnit,
                    greenland::IntrinsicHelper::IntrinsicId id,
                    RegLocation rlDest,
                    RegLocation rlSrc)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cUnit->irb->CreateCall(intr, GetLLVMValue(cUnit, rlSrc.origSReg));
  DefineValue(cUnit, res, rlDest.origSReg);
}


void ConvertNegFP(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  llvm::Value* res =
      cUnit->irb->CreateFNeg(GetLLVMValue(cUnit, rlSrc.origSReg));
  DefineValue(cUnit, res, rlDest.origSReg);
}

void ConvertNot(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  llvm::Value* src = GetLLVMValue(cUnit, rlSrc.origSReg);
  llvm::Value* res = cUnit->irb->CreateXor(src, static_cast<uint64_t>(-1));
  DefineValue(cUnit, res, rlDest.origSReg);
}

/*
 * Target-independent code generation.  Use only high-level
 * load/store utilities here, or target-dependent genXX() handlers
 * when necessary.
 */
bool ConvertMIRNode(CompilationUnit* cUnit, MIR* mir, BasicBlock* bb,
                    llvm::BasicBlock* llvmBB, LIR* labelList)
{
  bool res = false;   // Assume success
  RegLocation rlSrc[3];
  RegLocation rlDest = badLoc;
  Instruction::Code opcode = mir->dalvikInsn.opcode;
  int opVal = opcode;
  uint32_t vB = mir->dalvikInsn.vB;
  uint32_t vC = mir->dalvikInsn.vC;
  int optFlags = mir->optimizationFlags;

  bool objectDefinition = false;

  if (cUnit->printMe) {
    if (opVal < kMirOpFirst) {
      LOG(INFO) << ".. " << Instruction::Name(opcode) << " 0x" << std::hex << opVal;
    } else {
      LOG(INFO) << extendedMIROpNames[opVal - kMirOpFirst] << " 0x" << std::hex << opVal;
    }
  }

  /* Prep Src and Dest locations */
  int nextSreg = 0;
  int nextLoc = 0;
  int attrs = oatDataFlowAttributes[opcode];
  rlSrc[0] = rlSrc[1] = rlSrc[2] = badLoc;
  if (attrs & DF_UA) {
    if (attrs & DF_A_WIDE) {
      rlSrc[nextLoc++] = GetSrcWide(cUnit, mir, nextSreg);
      nextSreg+= 2;
    } else {
      rlSrc[nextLoc++] = GetSrc(cUnit, mir, nextSreg);
      nextSreg++;
    }
  }
  if (attrs & DF_UB) {
    if (attrs & DF_B_WIDE) {
      rlSrc[nextLoc++] = GetSrcWide(cUnit, mir, nextSreg);
      nextSreg+= 2;
    } else {
      rlSrc[nextLoc++] = GetSrc(cUnit, mir, nextSreg);
      nextSreg++;
    }
  }
  if (attrs & DF_UC) {
    if (attrs & DF_C_WIDE) {
      rlSrc[nextLoc++] = GetSrcWide(cUnit, mir, nextSreg);
    } else {
      rlSrc[nextLoc++] = GetSrc(cUnit, mir, nextSreg);
    }
  }
  if (attrs & DF_DA) {
    if (attrs & DF_A_WIDE) {
      rlDest = GetDestWide(cUnit, mir);
    } else {
      rlDest = GetDest(cUnit, mir);
      if (rlDest.ref) {
        objectDefinition = true;
      }
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
        llvm::Value* src = GetLLVMValue(cUnit, rlSrc[0].origSReg);
        llvm::Value* res = EmitCopy(cUnit, src, rlDest);
        DefineValue(cUnit, res, rlDest.origSReg);
      }
      break;

    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16: {
        if (vB == 0) {
          objectDefinition = true;
        }
        llvm::Constant* immValue = cUnit->irb->GetJInt(vB);
        llvm::Value* res = EmitConst(cUnit, immValue, rlDest);
        DefineValue(cUnit, res, rlDest.origSReg);
      }
      break;

    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32: {
        // Sign extend to 64 bits
        int64_t imm = static_cast<int32_t>(vB);
        llvm::Constant* immValue = cUnit->irb->GetJLong(imm);
        llvm::Value* res = EmitConst(cUnit, immValue, rlDest);
        DefineValue(cUnit, res, rlDest.origSReg);
      }
      break;

    case Instruction::CONST_HIGH16: {
        llvm::Constant* immValue = cUnit->irb->GetJInt(vB << 16);
        llvm::Value* res = EmitConst(cUnit, immValue, rlDest);
        DefineValue(cUnit, res, rlDest.origSReg);
      }
      break;

    case Instruction::CONST_WIDE: {
        llvm::Constant* immValue =
            cUnit->irb->GetJLong(mir->dalvikInsn.vB_wide);
        llvm::Value* res = EmitConst(cUnit, immValue, rlDest);
        DefineValue(cUnit, res, rlDest.origSReg);
      }
      break;
    case Instruction::CONST_WIDE_HIGH16: {
        int64_t imm = static_cast<int64_t>(vB) << 48;
        llvm::Constant* immValue = cUnit->irb->GetJLong(imm);
        llvm::Value* res = EmitConst(cUnit, immValue, rlDest);
        DefineValue(cUnit, res, rlDest.origSReg);
      }
      break;

    case Instruction::SPUT_OBJECT:
      ConvertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputObject,
                  rlSrc[0]);
      break;
    case Instruction::SPUT:
      if (rlSrc[0].fp) {
        ConvertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputFloat,
                    rlSrc[0]);
      } else {
        ConvertSput(cUnit, vB, greenland::IntrinsicHelper::HLSput, rlSrc[0]);
      }
      break;
    case Instruction::SPUT_BOOLEAN:
      ConvertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputBoolean,
                  rlSrc[0]);
      break;
    case Instruction::SPUT_BYTE:
      ConvertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputByte, rlSrc[0]);
      break;
    case Instruction::SPUT_CHAR:
      ConvertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputChar, rlSrc[0]);
      break;
    case Instruction::SPUT_SHORT:
      ConvertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputShort, rlSrc[0]);
      break;
    case Instruction::SPUT_WIDE:
      if (rlSrc[0].fp) {
        ConvertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputDouble,
                    rlSrc[0]);
      } else {
        ConvertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputWide,
                    rlSrc[0]);
      }
      break;

    case Instruction::SGET_OBJECT:
      ConvertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetObject, rlDest);
      break;
    case Instruction::SGET:
      if (rlDest.fp) {
        ConvertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetFloat, rlDest);
      } else {
        ConvertSget(cUnit, vB, greenland::IntrinsicHelper::HLSget, rlDest);
      }
      break;
    case Instruction::SGET_BOOLEAN:
      ConvertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetBoolean, rlDest);
      break;
    case Instruction::SGET_BYTE:
      ConvertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetByte, rlDest);
      break;
    case Instruction::SGET_CHAR:
      ConvertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetChar, rlDest);
      break;
    case Instruction::SGET_SHORT:
      ConvertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetShort, rlDest);
      break;
    case Instruction::SGET_WIDE:
      if (rlDest.fp) {
        ConvertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetDouble,
                    rlDest);
      } else {
        ConvertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetWide, rlDest);
      }
      break;

    case Instruction::RETURN_WIDE:
    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT: {
        if (!(cUnit->attrs & METHOD_IS_LEAF)) {
          EmitSuspendCheck(cUnit);
        }
        EmitPopShadowFrame(cUnit);
        cUnit->irb->CreateRet(GetLLVMValue(cUnit, rlSrc[0].origSReg));
        bb->hasReturn = true;
      }
      break;

    case Instruction::RETURN_VOID: {
        if (!(cUnit->attrs & METHOD_IS_LEAF)) {
          EmitSuspendCheck(cUnit);
        }
        EmitPopShadowFrame(cUnit);
        cUnit->irb->CreateRetVoid();
        bb->hasReturn = true;
      }
      break;

    case Instruction::IF_EQ:
      ConvertCompareAndBranch(cUnit, bb, mir, kCondEq, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_NE:
      ConvertCompareAndBranch(cUnit, bb, mir, kCondNe, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_LT:
      ConvertCompareAndBranch(cUnit, bb, mir, kCondLt, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_GE:
      ConvertCompareAndBranch(cUnit, bb, mir, kCondGe, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_GT:
      ConvertCompareAndBranch(cUnit, bb, mir, kCondGt, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_LE:
      ConvertCompareAndBranch(cUnit, bb, mir, kCondLe, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_EQZ:
      ConvertCompareZeroAndBranch(cUnit, bb, mir, kCondEq, rlSrc[0]);
      break;
    case Instruction::IF_NEZ:
      ConvertCompareZeroAndBranch(cUnit, bb, mir, kCondNe, rlSrc[0]);
      break;
    case Instruction::IF_LTZ:
      ConvertCompareZeroAndBranch(cUnit, bb, mir, kCondLt, rlSrc[0]);
      break;
    case Instruction::IF_GEZ:
      ConvertCompareZeroAndBranch(cUnit, bb, mir, kCondGe, rlSrc[0]);
      break;
    case Instruction::IF_GTZ:
      ConvertCompareZeroAndBranch(cUnit, bb, mir, kCondGt, rlSrc[0]);
      break;
    case Instruction::IF_LEZ:
      ConvertCompareZeroAndBranch(cUnit, bb, mir, kCondLe, rlSrc[0]);
      break;

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32: {
        if (bb->taken->startOffset <= bb->startOffset) {
          EmitSuspendCheck(cUnit);
        }
        cUnit->irb->CreateBr(GetLLVMBlock(cUnit, bb->taken->id));
      }
      break;

    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
      ConvertArithOp(cUnit, kOpAdd, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      ConvertArithOp(cUnit, kOpSub, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::MUL_LONG:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
      ConvertArithOp(cUnit, kOpMul, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::DIV_LONG:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
      ConvertArithOp(cUnit, kOpDiv, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::REM_LONG:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
      ConvertArithOp(cUnit, kOpRem, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AND_LONG:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
      ConvertArithOp(cUnit, kOpAnd, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
      ConvertArithOp(cUnit, kOpOr, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
      ConvertArithOp(cUnit, kOpXor, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      ConvertShift(cUnit, greenland::IntrinsicHelper::SHLLong,
                    rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      ConvertShift(cUnit, greenland::IntrinsicHelper::SHLInt,
                   rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      ConvertShift(cUnit, greenland::IntrinsicHelper::SHRLong,
                   rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      ConvertShift(cUnit, greenland::IntrinsicHelper::SHRInt,
                   rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      ConvertShift(cUnit, greenland::IntrinsicHelper::USHRLong,
                   rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      ConvertShift(cUnit, greenland::IntrinsicHelper::USHRInt,
                   rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::ADD_INT_LIT16:
    case Instruction::ADD_INT_LIT8:
      ConvertArithOpLit(cUnit, kOpAdd, rlDest, rlSrc[0], vC);
      break;
    case Instruction::RSUB_INT:
    case Instruction::RSUB_INT_LIT8:
      ConvertArithOpLit(cUnit, kOpRsub, rlDest, rlSrc[0], vC);
      break;
    case Instruction::MUL_INT_LIT16:
    case Instruction::MUL_INT_LIT8:
      ConvertArithOpLit(cUnit, kOpMul, rlDest, rlSrc[0], vC);
      break;
    case Instruction::DIV_INT_LIT16:
    case Instruction::DIV_INT_LIT8:
      ConvertArithOpLit(cUnit, kOpDiv, rlDest, rlSrc[0], vC);
      break;
    case Instruction::REM_INT_LIT16:
    case Instruction::REM_INT_LIT8:
      ConvertArithOpLit(cUnit, kOpRem, rlDest, rlSrc[0], vC);
      break;
    case Instruction::AND_INT_LIT16:
    case Instruction::AND_INT_LIT8:
      ConvertArithOpLit(cUnit, kOpAnd, rlDest, rlSrc[0], vC);
      break;
    case Instruction::OR_INT_LIT16:
    case Instruction::OR_INT_LIT8:
      ConvertArithOpLit(cUnit, kOpOr, rlDest, rlSrc[0], vC);
      break;
    case Instruction::XOR_INT_LIT16:
    case Instruction::XOR_INT_LIT8:
      ConvertArithOpLit(cUnit, kOpXor, rlDest, rlSrc[0], vC);
      break;
    case Instruction::SHL_INT_LIT8:
      ConvertShiftLit(cUnit, greenland::IntrinsicHelper::SHLInt,
                      rlDest, rlSrc[0], vC & 0x1f);
      break;
    case Instruction::SHR_INT_LIT8:
      ConvertShiftLit(cUnit, greenland::IntrinsicHelper::SHRInt,
                      rlDest, rlSrc[0], vC & 0x1f);
      break;
    case Instruction::USHR_INT_LIT8:
      ConvertShiftLit(cUnit, greenland::IntrinsicHelper::USHRInt,
                      rlDest, rlSrc[0], vC & 0x1f);
      break;

    case Instruction::ADD_FLOAT:
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::ADD_DOUBLE:
    case Instruction::ADD_DOUBLE_2ADDR:
      ConvertFPArithOp(cUnit, kOpAdd, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::SUB_FLOAT:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::SUB_DOUBLE:
    case Instruction::SUB_DOUBLE_2ADDR:
      ConvertFPArithOp(cUnit, kOpSub, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::MUL_FLOAT:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::MUL_DOUBLE:
    case Instruction::MUL_DOUBLE_2ADDR:
      ConvertFPArithOp(cUnit, kOpMul, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::DIV_FLOAT:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::DIV_DOUBLE:
    case Instruction::DIV_DOUBLE_2ADDR:
      ConvertFPArithOp(cUnit, kOpDiv, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::REM_FLOAT:
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::REM_DOUBLE:
    case Instruction::REM_DOUBLE_2ADDR:
      ConvertFPArithOp(cUnit, kOpRem, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::INVOKE_STATIC:
      ConvertInvoke(cUnit, bb, mir, kStatic, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_STATIC_RANGE:
      ConvertInvoke(cUnit, bb, mir, kStatic, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_DIRECT:
      ConvertInvoke(cUnit, bb,  mir, kDirect, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_DIRECT_RANGE:
      ConvertInvoke(cUnit, bb, mir, kDirect, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_VIRTUAL:
      ConvertInvoke(cUnit, bb, mir, kVirtual, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_VIRTUAL_RANGE:
      ConvertInvoke(cUnit, bb, mir, kVirtual, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_SUPER:
      ConvertInvoke(cUnit, bb, mir, kSuper, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_SUPER_RANGE:
      ConvertInvoke(cUnit, bb, mir, kSuper, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_INTERFACE:
      ConvertInvoke(cUnit, bb, mir, kInterface, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_INTERFACE_RANGE:
      ConvertInvoke(cUnit, bb, mir, kInterface, true /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::FILLED_NEW_ARRAY:
      ConvertInvoke(cUnit, bb, mir, kInterface, false /*range*/,
                    true /* NewFilledArray */);
      break;
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      ConvertInvoke(cUnit, bb, mir, kInterface, true /*range*/,
                    true /* NewFilledArray */);
      break;

    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      ConvertConstObject(cUnit, vB, greenland::IntrinsicHelper::ConstString,
                         rlDest);
      break;

    case Instruction::CONST_CLASS:
      ConvertConstObject(cUnit, vB, greenland::IntrinsicHelper::ConstClass,
                         rlDest);
      break;

    case Instruction::CHECK_CAST:
      ConvertCheckCast(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::NEW_INSTANCE:
      ConvertNewInstance(cUnit, vB, rlDest);
      break;

   case Instruction::MOVE_EXCEPTION:
      ConvertMoveException(cUnit, rlDest);
      break;

   case Instruction::THROW:
      ConvertThrow(cUnit, rlSrc[0]);
      /*
       * If this throw is standalone, terminate.
       * If it might rethrow, force termination
       * of the following block.
       */
      if (bb->fallThrough == NULL) {
        cUnit->irb->CreateUnreachable();
      } else {
        bb->fallThrough->fallThrough = NULL;
        bb->fallThrough->taken = NULL;
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
      ConvertMonitorEnterExit(cUnit, optFlags,
                              greenland::IntrinsicHelper::MonitorEnter,
                              rlSrc[0]);
      break;

    case Instruction::MONITOR_EXIT:
      ConvertMonitorEnterExit(cUnit, optFlags,
                              greenland::IntrinsicHelper::MonitorExit,
                              rlSrc[0]);
      break;

    case Instruction::ARRAY_LENGTH:
      ConvertArrayLength(cUnit, optFlags, rlDest, rlSrc[0]);
      break;

    case Instruction::NEW_ARRAY:
      ConvertNewArray(cUnit, vC, rlDest, rlSrc[0]);
      break;

    case Instruction::INSTANCE_OF:
      ConvertInstanceOf(cUnit, vC, rlDest, rlSrc[0]);
      break;

    case Instruction::AGET:
      if (rlDest.fp) {
        ConvertAget(cUnit, optFlags,
                    greenland::IntrinsicHelper::HLArrayGetFloat,
                    rlDest, rlSrc[0], rlSrc[1]);
      } else {
        ConvertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGet,
                    rlDest, rlSrc[0], rlSrc[1]);
      }
      break;
    case Instruction::AGET_OBJECT:
      ConvertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGetObject,
                  rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AGET_BOOLEAN:
      ConvertAget(cUnit, optFlags,
                  greenland::IntrinsicHelper::HLArrayGetBoolean,
                  rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AGET_BYTE:
      ConvertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGetByte,
                  rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AGET_CHAR:
      ConvertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGetChar,
                  rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AGET_SHORT:
      ConvertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGetShort,
                  rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AGET_WIDE:
      if (rlDest.fp) {
        ConvertAget(cUnit, optFlags,
                    greenland::IntrinsicHelper::HLArrayGetDouble,
                    rlDest, rlSrc[0], rlSrc[1]);
      } else {
        ConvertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGetWide,
                    rlDest, rlSrc[0], rlSrc[1]);
      }
      break;

    case Instruction::APUT:
      if (rlSrc[0].fp) {
        ConvertAput(cUnit, optFlags,
                    greenland::IntrinsicHelper::HLArrayPutFloat,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      } else {
        ConvertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPut,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      }
      break;
    case Instruction::APUT_OBJECT:
      ConvertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPutObject,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      break;
    case Instruction::APUT_BOOLEAN:
      ConvertAput(cUnit, optFlags,
                  greenland::IntrinsicHelper::HLArrayPutBoolean,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      break;
    case Instruction::APUT_BYTE:
      ConvertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPutByte,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      break;
    case Instruction::APUT_CHAR:
      ConvertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPutChar,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      break;
    case Instruction::APUT_SHORT:
      ConvertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPutShort,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      break;
    case Instruction::APUT_WIDE:
      if (rlSrc[0].fp) {
        ConvertAput(cUnit, optFlags,
                    greenland::IntrinsicHelper::HLArrayPutDouble,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      } else {
        ConvertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPutWide,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      }
      break;

    case Instruction::IGET:
      if (rlDest.fp) {
        ConvertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetFloat,
                    rlDest, rlSrc[0], vC);
      } else {
        ConvertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGet,
                    rlDest, rlSrc[0], vC);
      }
      break;
    case Instruction::IGET_OBJECT:
      ConvertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetObject,
                  rlDest, rlSrc[0], vC);
      break;
    case Instruction::IGET_BOOLEAN:
      ConvertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetBoolean,
                  rlDest, rlSrc[0], vC);
      break;
    case Instruction::IGET_BYTE:
      ConvertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetByte,
                  rlDest, rlSrc[0], vC);
      break;
    case Instruction::IGET_CHAR:
      ConvertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetChar,
                  rlDest, rlSrc[0], vC);
      break;
    case Instruction::IGET_SHORT:
      ConvertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetShort,
                  rlDest, rlSrc[0], vC);
      break;
    case Instruction::IGET_WIDE:
      if (rlDest.fp) {
        ConvertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetDouble,
                    rlDest, rlSrc[0], vC);
      } else {
        ConvertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetWide,
                    rlDest, rlSrc[0], vC);
      }
      break;
    case Instruction::IPUT:
      if (rlSrc[0].fp) {
        ConvertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutFloat,
                    rlSrc[0], rlSrc[1], vC);
      } else {
        ConvertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPut,
                    rlSrc[0], rlSrc[1], vC);
      }
      break;
    case Instruction::IPUT_OBJECT:
      ConvertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutObject,
                  rlSrc[0], rlSrc[1], vC);
      break;
    case Instruction::IPUT_BOOLEAN:
      ConvertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutBoolean,
                  rlSrc[0], rlSrc[1], vC);
      break;
    case Instruction::IPUT_BYTE:
      ConvertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutByte,
                  rlSrc[0], rlSrc[1], vC);
      break;
    case Instruction::IPUT_CHAR:
      ConvertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutChar,
                  rlSrc[0], rlSrc[1], vC);
      break;
    case Instruction::IPUT_SHORT:
      ConvertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutShort,
                  rlSrc[0], rlSrc[1], vC);
      break;
    case Instruction::IPUT_WIDE:
      if (rlSrc[0].fp) {
        ConvertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutDouble,
                    rlSrc[0], rlSrc[1], vC);
      } else {
        ConvertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutWide,
                    rlSrc[0], rlSrc[1], vC);
      }
      break;

    case Instruction::FILL_ARRAY_DATA:
      ConvertFillArrayData(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::LONG_TO_INT:
      ConvertLongToInt(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_LONG:
      ConvertIntToLong(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_CHAR:
      ConvertIntNarrowing(cUnit, rlDest, rlSrc[0],
                          greenland::IntrinsicHelper::IntToChar);
      break;
    case Instruction::INT_TO_BYTE:
      ConvertIntNarrowing(cUnit, rlDest, rlSrc[0],
                          greenland::IntrinsicHelper::IntToByte);
      break;
    case Instruction::INT_TO_SHORT:
      ConvertIntNarrowing(cUnit, rlDest, rlSrc[0],
                          greenland::IntrinsicHelper::IntToShort);
      break;

    case Instruction::INT_TO_FLOAT:
    case Instruction::LONG_TO_FLOAT:
      ConvertIntToFP(cUnit, cUnit->irb->getFloatTy(), rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_DOUBLE:
    case Instruction::LONG_TO_DOUBLE:
      ConvertIntToFP(cUnit, cUnit->irb->getDoubleTy(), rlDest, rlSrc[0]);
      break;

    case Instruction::FLOAT_TO_DOUBLE:
      ConvertFloatToDouble(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::DOUBLE_TO_FLOAT:
      ConvertDoubleToFloat(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::NEG_LONG:
    case Instruction::NEG_INT:
      ConvertNeg(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::NEG_FLOAT:
    case Instruction::NEG_DOUBLE:
      ConvertNegFP(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::NOT_LONG:
    case Instruction::NOT_INT:
      ConvertNot(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::FLOAT_TO_INT:
      ConvertFPToInt(cUnit, greenland::IntrinsicHelper::F2I, rlDest, rlSrc[0]);
      break;

    case Instruction::DOUBLE_TO_INT:
      ConvertFPToInt(cUnit, greenland::IntrinsicHelper::D2I, rlDest, rlSrc[0]);
      break;

    case Instruction::FLOAT_TO_LONG:
      ConvertFPToInt(cUnit, greenland::IntrinsicHelper::F2L, rlDest, rlSrc[0]);
      break;

    case Instruction::DOUBLE_TO_LONG:
      ConvertFPToInt(cUnit, greenland::IntrinsicHelper::D2L, rlDest, rlSrc[0]);
      break;

    case Instruction::CMPL_FLOAT:
      ConvertWideComparison(cUnit, greenland::IntrinsicHelper::CmplFloat,
                            rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::CMPG_FLOAT:
      ConvertWideComparison(cUnit, greenland::IntrinsicHelper::CmpgFloat,
                            rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::CMPL_DOUBLE:
      ConvertWideComparison(cUnit, greenland::IntrinsicHelper::CmplDouble,
                            rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::CMPG_DOUBLE:
      ConvertWideComparison(cUnit, greenland::IntrinsicHelper::CmpgDouble,
                            rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::CMP_LONG:
      ConvertWideComparison(cUnit, greenland::IntrinsicHelper::CmpLong,
                            rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::PACKED_SWITCH:
      ConvertPackedSwitch(cUnit, bb, vB, rlSrc[0]);
      break;

    case Instruction::SPARSE_SWITCH:
      ConvertSparseSwitch(cUnit, bb, vB, rlSrc[0]);
      break;

    default:
      UNIMPLEMENTED(FATAL) << "Unsupported Dex opcode 0x" << std::hex << opcode;
      res = true;
  }
  if (objectDefinition) {
    SetShadowFrameEntry(cUnit, reinterpret_cast<llvm::Value*>
                        (cUnit->llvmValues.elemList[rlDest.origSReg]));
  }
  return res;
}

/* Extended MIR instructions like PHI */
void ConvertExtendedMIR(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                        llvm::BasicBlock* llvmBB)
{

  switch (static_cast<ExtendedMIROpcode>(mir->dalvikInsn.opcode)) {
    case kMirOpPhi: {
      RegLocation rlDest = cUnit->regLocation[mir->ssaRep->defs[0]];
      /*
       * The Art compiler's Phi nodes only handle 32-bit operands,
       * representing wide values using a matched set of Phi nodes
       * for the lower and upper halves.  In the llvm world, we only
       * want a single Phi for wides.  Here we will simply discard
       * the Phi node representing the high word.
       */
      if (rlDest.highWord) {
        return;  // No Phi node - handled via low word
      }
      int* incoming = reinterpret_cast<int*>(mir->dalvikInsn.vB);
      llvm::Type* phiType =
          LlvmTypeFromLocRec(cUnit, rlDest);
      llvm::PHINode* phi = cUnit->irb->CreatePHI(phiType, mir->ssaRep->numUses);
      for (int i = 0; i < mir->ssaRep->numUses; i++) {
        RegLocation loc;
        // Don't check width here.
        loc = GetRawSrc(cUnit, mir, i);
        DCHECK_EQ(rlDest.wide, loc.wide);
        DCHECK_EQ(rlDest.wide & rlDest.highWord, loc.wide & loc.highWord);
        DCHECK_EQ(rlDest.fp, loc.fp);
        DCHECK_EQ(rlDest.core, loc.core);
        DCHECK_EQ(rlDest.ref, loc.ref);
        SafeMap<unsigned int, unsigned int>::iterator it;
        it = cUnit->blockIdMap.find(incoming[i]);
        DCHECK(it != cUnit->blockIdMap.end());
        phi->addIncoming(GetLLVMValue(cUnit, loc.origSReg),
                         GetLLVMBlock(cUnit, it->second));
      }
      DefineValue(cUnit, phi, rlDest.origSReg);
      break;
    }
    case kMirOpCopy: {
      UNIMPLEMENTED(WARNING) << "unimp kMirOpPhi";
      break;
    }
    case kMirOpNop:
      if ((mir == bb->lastMIRInsn) && (bb->taken == NULL) &&
          (bb->fallThrough == NULL)) {
        cUnit->irb->CreateUnreachable();
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

void SetDexOffset(CompilationUnit* cUnit, int32_t offset)
{
  cUnit->currentDalvikOffset = offset;
  llvm::SmallVector<llvm::Value*, 1> arrayRef;
  arrayRef.push_back(cUnit->irb->getInt32(offset));
  llvm::MDNode* node = llvm::MDNode::get(*cUnit->context, arrayRef);
  cUnit->irb->SetDexOffset(node);
}

// Attach method info as metadata to special intrinsic
void SetMethodInfo(CompilationUnit* cUnit)
{
  // We don't want dex offset on this
  cUnit->irb->SetDexOffset(NULL);
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::MethodInfo;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Instruction* inst = cUnit->irb->CreateCall(intr);
  llvm::SmallVector<llvm::Value*, 2> regInfo;
  regInfo.push_back(cUnit->irb->getInt32(cUnit->numIns));
  regInfo.push_back(cUnit->irb->getInt32(cUnit->numRegs));
  regInfo.push_back(cUnit->irb->getInt32(cUnit->numOuts));
  regInfo.push_back(cUnit->irb->getInt32(cUnit->numCompilerTemps));
  regInfo.push_back(cUnit->irb->getInt32(cUnit->numSSARegs));
  llvm::MDNode* regInfoNode = llvm::MDNode::get(*cUnit->context, regInfo);
  inst->setMetadata("RegInfo", regInfoNode);
  int promoSize = cUnit->numDalvikRegisters + cUnit->numCompilerTemps + 1;
  llvm::SmallVector<llvm::Value*, 50> pmap;
  for (int i = 0; i < promoSize; i++) {
    PromotionMap* p = &cUnit->promotionMap[i];
    int32_t mapData = ((p->firstInPair & 0xff) << 24) |
                      ((p->FpReg & 0xff) << 16) |
                      ((p->coreReg & 0xff) << 8) |
                      ((p->fpLocation & 0xf) << 4) |
                      (p->coreLocation & 0xf);
    pmap.push_back(cUnit->irb->getInt32(mapData));
  }
  llvm::MDNode* mapNode = llvm::MDNode::get(*cUnit->context, pmap);
  inst->setMetadata("PromotionMap", mapNode);
  SetDexOffset(cUnit, cUnit->currentDalvikOffset);
}

/* Handle the content in each basic block */
bool MethodBlockBitcodeConversion(CompilationUnit* cUnit, BasicBlock* bb)
{
  if (bb->blockType == kDead) return false;
  llvm::BasicBlock* llvmBB = GetLLVMBlock(cUnit, bb->id);
  if (llvmBB == NULL) {
    CHECK(bb->blockType == kExitBlock);
  } else {
    cUnit->irb->SetInsertPoint(llvmBB);
    SetDexOffset(cUnit, bb->startOffset);
  }

  if (cUnit->printMe) {
    LOG(INFO) << "................................";
    LOG(INFO) << "Block id " << bb->id;
    if (llvmBB != NULL) {
      LOG(INFO) << "label " << llvmBB->getName().str().c_str();
    } else {
      LOG(INFO) << "llvmBB is NULL";
    }
  }

  if (bb->blockType == kEntryBlock) {
    SetMethodInfo(cUnit);
    bool *canBeRef = static_cast<bool*>(NewMem(cUnit, sizeof(bool) * cUnit->numDalvikRegisters,
                                               true, kAllocMisc));
    for (int i = 0; i < cUnit->numSSARegs; i++) {
      int vReg = SRegToVReg(cUnit, i);
      if (vReg > SSA_METHOD_BASEREG) {
        canBeRef[SRegToVReg(cUnit, i)] |= cUnit->regLocation[i].ref;
      }
    }
    for (int i = 0; i < cUnit->numDalvikRegisters; i++) {
      if (canBeRef[i]) {
        cUnit->numShadowFrameEntries++;
      }
    }
    if (cUnit->numShadowFrameEntries > 0) {
      cUnit->shadowMap = static_cast<int*>(NewMem(cUnit, sizeof(int) * cUnit->numShadowFrameEntries,
                                                  true, kAllocMisc));
      for (int i = 0, j = 0; i < cUnit->numDalvikRegisters; i++) {
        if (canBeRef[i]) {
          cUnit->shadowMap[j++] = i;
        }
      }
    }
    greenland::IntrinsicHelper::IntrinsicId id =
            greenland::IntrinsicHelper::AllocaShadowFrame;
    llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
    llvm::Value* entries = cUnit->irb->getInt32(cUnit->numShadowFrameEntries);
    llvm::Value* dalvikRegs = cUnit->irb->getInt32(cUnit->numDalvikRegisters);
    llvm::Value* args[] = { entries, dalvikRegs };
    cUnit->irb->CreateCall(func, args);
  } else if (bb->blockType == kExitBlock) {
    /*
     * Because of the differences between how MIR/LIR and llvm handle exit
     * blocks, we won't explicitly covert them.  On the llvm-to-lir
     * path, it will need to be regenereated.
     */
    return false;
  } else if (bb->blockType == kExceptionHandling) {
    /*
     * Because we're deferring null checking, delete the associated empty
     * exception block.
     */
    llvmBB->eraseFromParent();
    return false;
  }

  for (MIR* mir = bb->firstMIRInsn; mir; mir = mir->next) {

    SetDexOffset(cUnit, mir->offset);

    int opcode = mir->dalvikInsn.opcode;
    Instruction::Format dalvikFormat =
        Instruction::FormatOf(mir->dalvikInsn.opcode);

    if (opcode == kMirOpCheck) {
      // Combine check and work halves of throwing instruction.
      MIR* workHalf = mir->meta.throwInsn;
      mir->dalvikInsn.opcode = workHalf->dalvikInsn.opcode;
      opcode = mir->dalvikInsn.opcode;
      SSARepresentation* ssaRep = workHalf->ssaRep;
      workHalf->ssaRep = mir->ssaRep;
      mir->ssaRep = ssaRep;
      workHalf->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
      if (bb->successorBlockList.blockListType == kCatch) {
        llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(
            greenland::IntrinsicHelper::CatchTargets);
        llvm::Value* switchKey =
            cUnit->irb->CreateCall(intr, cUnit->irb->getInt32(mir->offset));
        GrowableListIterator iter;
        GrowableListIteratorInit(&bb->successorBlockList.blocks, &iter);
        // New basic block to use for work half
        llvm::BasicBlock* workBB =
            llvm::BasicBlock::Create(*cUnit->context, "", cUnit->func);
        llvm::SwitchInst* sw =
            cUnit->irb->CreateSwitch(switchKey, workBB,
                                     bb->successorBlockList.blocks.numUsed);
        while (true) {
          SuccessorBlockInfo *successorBlockInfo =
              reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iter));
          if (successorBlockInfo == NULL) break;
          llvm::BasicBlock *target =
              GetLLVMBlock(cUnit, successorBlockInfo->block->id);
          int typeIndex = successorBlockInfo->key;
          sw->addCase(cUnit->irb->getInt32(typeIndex), target);
        }
        llvmBB = workBB;
        cUnit->irb->SetInsertPoint(llvmBB);
      }
    }

    if (opcode >= kMirOpFirst) {
      ConvertExtendedMIR(cUnit, bb, mir, llvmBB);
      continue;
    }

    bool notHandled = ConvertMIRNode(cUnit, mir, bb, llvmBB,
                                     NULL /* labelList */);
    if (notHandled) {
      Instruction::Code dalvikOpcode = static_cast<Instruction::Code>(opcode);
      LOG(WARNING) << StringPrintf("%#06x: Op %#x (%s) / Fmt %d not handled",
                                   mir->offset, opcode,
                                   Instruction::Name(dalvikOpcode),
                                   dalvikFormat);
    }
  }

  if (bb->blockType == kEntryBlock) {
    cUnit->entryTargetBB = GetLLVMBlock(cUnit, bb->fallThrough->id);
  } else if ((bb->fallThrough != NULL) && !bb->hasReturn) {
    cUnit->irb->CreateBr(GetLLVMBlock(cUnit, bb->fallThrough->id));
  }

  return false;
}

char RemapShorty(char shortyType) {
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
  switch(shortyType) {
    case 'Z' : shortyType = 'I'; break;
    case 'B' : shortyType = 'I'; break;
    case 'S' : shortyType = 'I'; break;
    case 'C' : shortyType = 'I'; break;
    default: break;
  }
  return shortyType;
}

llvm::FunctionType* GetFunctionType(CompilationUnit* cUnit) {

  // Get return type
  llvm::Type* ret_type = cUnit->irb->GetJType(RemapShorty(cUnit->shorty[0]),
                                              greenland::kAccurate);

  // Get argument type
  std::vector<llvm::Type*> args_type;

  // method object
  args_type.push_back(cUnit->irb->GetJMethodTy());

  // Do we have  a "this"?
  if ((cUnit->access_flags & kAccStatic) == 0) {
    args_type.push_back(cUnit->irb->GetJObjectTy());
  }

  for (uint32_t i = 1; i < strlen(cUnit->shorty); ++i) {
    args_type.push_back(cUnit->irb->GetJType(RemapShorty(cUnit->shorty[i]),
                                             greenland::kAccurate));
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}

bool CreateFunction(CompilationUnit* cUnit) {
  std::string func_name(PrettyMethod(cUnit->method_idx, *cUnit->dex_file,
                                     /* with_signature */ false));
  llvm::FunctionType* func_type = GetFunctionType(cUnit);

  if (func_type == NULL) {
    return false;
  }

  cUnit->func = llvm::Function::Create(func_type,
                                       llvm::Function::ExternalLinkage,
                                       func_name, cUnit->module);

  llvm::Function::arg_iterator arg_iter(cUnit->func->arg_begin());
  llvm::Function::arg_iterator arg_end(cUnit->func->arg_end());

  arg_iter->setName("method");
  ++arg_iter;

  int startSReg = cUnit->numRegs;

  for (unsigned i = 0; arg_iter != arg_end; ++i, ++arg_iter) {
    arg_iter->setName(StringPrintf("v%i_0", startSReg));
    startSReg += cUnit->regLocation[startSReg].wide ? 2 : 1;
  }

  return true;
}

bool CreateLLVMBasicBlock(CompilationUnit* cUnit, BasicBlock* bb)
{
  // Skip the exit block
  if ((bb->blockType == kDead) ||(bb->blockType == kExitBlock)) {
    cUnit->idToBlockMap.Put(bb->id, NULL);
  } else {
    int offset = bb->startOffset;
    bool entryBlock = (bb->blockType == kEntryBlock);
    llvm::BasicBlock* llvmBB =
        llvm::BasicBlock::Create(*cUnit->context, entryBlock ? "entry" :
                                 StringPrintf(kLabelFormat, bb->catchEntry ? kCatchBlock :
                                              kNormalBlock, offset, bb->id), cUnit->func);
    if (entryBlock) {
        cUnit->entryBB = llvmBB;
        cUnit->placeholderBB =
            llvm::BasicBlock::Create(*cUnit->context, "placeholder",
                                     cUnit->func);
    }
    cUnit->idToBlockMap.Put(bb->id, llvmBB);
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
void MethodMIR2Bitcode(CompilationUnit* cUnit)
{
  InitIR(cUnit);
  CompilerInitGrowableList(cUnit, &cUnit->llvmValues, cUnit->numSSARegs);

  // Create the function
  CreateFunction(cUnit);

  // Create an LLVM basic block for each MIR block in dfs preorder
  DataFlowAnalysisDispatcher(cUnit, CreateLLVMBasicBlock,
                                kPreOrderDFSTraversal, false /* isIterative */);
  /*
   * Create an llvm named value for each MIR SSA name.  Note: we'll use
   * placeholders for all non-argument values (because we haven't seen
   * the definition yet).
   */
  cUnit->irb->SetInsertPoint(cUnit->placeholderBB);
  llvm::Function::arg_iterator arg_iter(cUnit->func->arg_begin());
  arg_iter++;  /* Skip path method */
  for (int i = 0; i < cUnit->numSSARegs; i++) {
    llvm::Value* val;
    RegLocation rlTemp = cUnit->regLocation[i];
    if ((SRegToVReg(cUnit, i) < 0) || rlTemp.highWord) {
      InsertGrowableList(cUnit, &cUnit->llvmValues, 0);
    } else if ((i < cUnit->numRegs) ||
               (i >= (cUnit->numRegs + cUnit->numIns))) {
      llvm::Constant* immValue = cUnit->regLocation[i].wide ?
         cUnit->irb->GetJLong(0) : cUnit->irb->GetJInt(0);
      val = EmitConst(cUnit, immValue, cUnit->regLocation[i]);
      val->setName(LlvmSSAName(cUnit, i));
      InsertGrowableList(cUnit, &cUnit->llvmValues, reinterpret_cast<uintptr_t>(val));
    } else {
      // Recover previously-created argument values
      llvm::Value* argVal = arg_iter++;
      InsertGrowableList(cUnit, &cUnit->llvmValues, reinterpret_cast<uintptr_t>(argVal));
    }
  }

  DataFlowAnalysisDispatcher(cUnit, MethodBlockBitcodeConversion,
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
  for (llvm::BasicBlock::iterator it = cUnit->placeholderBB->begin(),
       itEnd = cUnit->placeholderBB->end(); it != itEnd;) {
    llvm::Instruction* inst = llvm::dyn_cast<llvm::Instruction>(it++);
    DCHECK(inst != NULL);
    llvm::Value* val = llvm::dyn_cast<llvm::Value>(inst);
    DCHECK(val != NULL);
    if (val->getNumUses() == 0) {
      inst->eraseFromParent();
    }
  }
  SetDexOffset(cUnit, 0);
  if (cUnit->placeholderBB->empty()) {
    cUnit->placeholderBB->eraseFromParent();
  } else {
    cUnit->irb->SetInsertPoint(cUnit->placeholderBB);
    cUnit->irb->CreateBr(cUnit->entryTargetBB);
    cUnit->entryTargetBB = cUnit->placeholderBB;
  }
  cUnit->irb->SetInsertPoint(cUnit->entryBB);
  cUnit->irb->CreateBr(cUnit->entryTargetBB);

  if (cUnit->enableDebug & (1 << kDebugVerifyBitcode)) {
     if (llvm::verifyFunction(*cUnit->func, llvm::PrintMessageAction)) {
       LOG(INFO) << "Bitcode verification FAILED for "
                 << PrettyMethod(cUnit->method_idx, *cUnit->dex_file)
                 << " of size " << cUnit->insnsSize;
       cUnit->enableDebug |= (1 << kDebugDumpBitcodeFile);
     }
  }

  if (cUnit->enableDebug & (1 << kDebugDumpBitcodeFile)) {
    // Write bitcode to file
    std::string errmsg;
    std::string fname(PrettyMethod(cUnit->method_idx, *cUnit->dex_file));
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

    llvm::WriteBitcodeToFile(cUnit->module, out_file->os());
    out_file->keep();
  }
}

RegLocation GetLoc(CompilationUnit* cUnit, llvm::Value* val) {
  RegLocation res;
  DCHECK(val != NULL);
  SafeMap<llvm::Value*, RegLocation>::iterator it = cUnit->locMap.find(val);
  if (it == cUnit->locMap.end()) {
    std::string valName = val->getName().str();
    if (valName.empty()) {
      // FIXME: need to be more robust, handle FP and be in a position to
      // manage unnamed temps whose lifetimes span basic block boundaries
      UNIMPLEMENTED(WARNING) << "Need to handle unnamed llvm temps";
      memset(&res, 0, sizeof(res));
      res.location = kLocPhysReg;
      res.lowReg = AllocTemp(cUnit);
      res.home = true;
      res.sRegLow = INVALID_SREG;
      res.origSReg = INVALID_SREG;
      llvm::Type* ty = val->getType();
      res.wide = ((ty == cUnit->irb->getInt64Ty()) ||
                  (ty == cUnit->irb->getDoubleTy()));
      if (res.wide) {
        res.highReg = AllocTemp(cUnit);
      }
      cUnit->locMap.Put(val, res);
    } else {
      DCHECK_EQ(valName[0], 'v');
      int baseSReg = INVALID_SREG;
      sscanf(valName.c_str(), "v%d_", &baseSReg);
      res = cUnit->regLocation[baseSReg];
      cUnit->locMap.Put(val, res);
    }
  } else {
    res = it->second;
  }
  return res;
}

Instruction::Code GetDalvikOpcode(OpKind op, bool isConst, bool isWide)
{
  Instruction::Code res = Instruction::NOP;
  if (isWide) {
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
  } else if (isConst){
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

Instruction::Code GetDalvikFPOpcode(OpKind op, bool isConst, bool isWide)
{
  Instruction::Code res = Instruction::NOP;
  if (isWide) {
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

void CvtBinFPOp(CompilationUnit* cUnit, OpKind op, llvm::Instruction* inst)
{
  RegLocation rlDest = GetLoc(cUnit, inst);
  /*
   * Normally, we won't ever generate an FP operation with an immediate
   * operand (not supported in Dex instruction set).  However, the IR builder
   * may insert them - in particular for createNegFP.  Recognize this case
   * and deal with it.
   */
  llvm::ConstantFP* op1C = llvm::dyn_cast<llvm::ConstantFP>(inst->getOperand(0));
  llvm::ConstantFP* op2C = llvm::dyn_cast<llvm::ConstantFP>(inst->getOperand(1));
  DCHECK(op2C == NULL);
  if ((op1C != NULL) && (op == kOpSub)) {
    RegLocation rlSrc = GetLoc(cUnit, inst->getOperand(1));
    if (rlDest.wide) {
      GenArithOpDouble(cUnit, Instruction::NEG_DOUBLE, rlDest, rlSrc, rlSrc);
    } else {
      GenArithOpFloat(cUnit, Instruction::NEG_FLOAT, rlDest, rlSrc, rlSrc);
    }
  } else {
    DCHECK(op1C == NULL);
    RegLocation rlSrc1 = GetLoc(cUnit, inst->getOperand(0));
    RegLocation rlSrc2 = GetLoc(cUnit, inst->getOperand(1));
    Instruction::Code dalvikOp = GetDalvikFPOpcode(op, false, rlDest.wide);
    if (rlDest.wide) {
      GenArithOpDouble(cUnit, dalvikOp, rlDest, rlSrc1, rlSrc2);
    } else {
      GenArithOpFloat(cUnit, dalvikOp, rlDest, rlSrc1, rlSrc2);
    }
  }
}

void CvtIntNarrowing(CompilationUnit* cUnit, llvm::Instruction* inst,
                     Instruction::Code opcode)
{
  RegLocation rlDest = GetLoc(cUnit, inst);
  RegLocation rlSrc = GetLoc(cUnit, inst->getOperand(0));
  GenIntNarrowing(cUnit, opcode, rlDest, rlSrc);
}

void CvtIntToFP(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  RegLocation rlDest = GetLoc(cUnit, inst);
  RegLocation rlSrc = GetLoc(cUnit, inst->getOperand(0));
  Instruction::Code opcode;
  if (rlDest.wide) {
    if (rlSrc.wide) {
      opcode = Instruction::LONG_TO_DOUBLE;
    } else {
      opcode = Instruction::INT_TO_DOUBLE;
    }
  } else {
    if (rlSrc.wide) {
      opcode = Instruction::LONG_TO_FLOAT;
    } else {
      opcode = Instruction::INT_TO_FLOAT;
    }
  }
  GenConversion(cUnit, opcode, rlDest, rlSrc);
}

void CvtFPToInt(CompilationUnit* cUnit, llvm::CallInst* call_inst)
{
  RegLocation rlDest = GetLoc(cUnit, call_inst);
  RegLocation rlSrc = GetLoc(cUnit, call_inst->getOperand(0));
  Instruction::Code opcode;
  if (rlDest.wide) {
    if (rlSrc.wide) {
      opcode = Instruction::DOUBLE_TO_LONG;
    } else {
      opcode = Instruction::FLOAT_TO_LONG;
    }
  } else {
    if (rlSrc.wide) {
      opcode = Instruction::DOUBLE_TO_INT;
    } else {
      opcode = Instruction::FLOAT_TO_INT;
    }
  }
  GenConversion(cUnit, opcode, rlDest, rlSrc);
}

void CvtFloatToDouble(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  RegLocation rlDest = GetLoc(cUnit, inst);
  RegLocation rlSrc = GetLoc(cUnit, inst->getOperand(0));
  GenConversion(cUnit, Instruction::FLOAT_TO_DOUBLE, rlDest, rlSrc);
}

void CvtTrunc(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  RegLocation rlDest = GetLoc(cUnit, inst);
  RegLocation rlSrc = GetLoc(cUnit, inst->getOperand(0));
  rlSrc = UpdateLocWide(cUnit, rlSrc);
  rlSrc = WideToNarrow(cUnit, rlSrc);
  StoreValue(cUnit, rlDest, rlSrc);
}

void CvtDoubleToFloat(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  RegLocation rlDest = GetLoc(cUnit, inst);
  RegLocation rlSrc = GetLoc(cUnit, inst->getOperand(0));
  GenConversion(cUnit, Instruction::DOUBLE_TO_FLOAT, rlDest, rlSrc);
}


void CvtIntExt(CompilationUnit* cUnit, llvm::Instruction* inst, bool isSigned)
{
  // TODO: evaluate src/tgt types and add general support for more than int to long
  RegLocation rlDest = GetLoc(cUnit, inst);
  RegLocation rlSrc = GetLoc(cUnit, inst->getOperand(0));
  DCHECK(rlDest.wide);
  DCHECK(!rlSrc.wide);
  DCHECK(!rlDest.fp);
  DCHECK(!rlSrc.fp);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  if (rlSrc.location == kLocPhysReg) {
    OpRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
  } else {
    LoadValueDirect(cUnit, rlSrc, rlResult.lowReg);
  }
  if (isSigned) {
    OpRegRegImm(cUnit, kOpAsr, rlResult.highReg, rlResult.lowReg, 31);
  } else {
    LoadConstant(cUnit, rlResult.highReg, 0);
  }
  StoreValueWide(cUnit, rlDest, rlResult);
}

void CvtBinOp(CompilationUnit* cUnit, OpKind op, llvm::Instruction* inst)
{
  RegLocation rlDest = GetLoc(cUnit, inst);
  llvm::Value* lhs = inst->getOperand(0);
  // Special-case RSUB/NEG
  llvm::ConstantInt* lhsImm = llvm::dyn_cast<llvm::ConstantInt>(lhs);
  if ((op == kOpSub) && (lhsImm != NULL)) {
    RegLocation rlSrc1 = GetLoc(cUnit, inst->getOperand(1));
    if (rlSrc1.wide) {
      DCHECK_EQ(lhsImm->getSExtValue(), 0);
      GenArithOpLong(cUnit, Instruction::NEG_LONG, rlDest, rlSrc1, rlSrc1);
    } else {
      GenArithOpIntLit(cUnit, Instruction::RSUB_INT, rlDest, rlSrc1,
                       lhsImm->getSExtValue());
    }
    return;
  }
  DCHECK(lhsImm == NULL);
  RegLocation rlSrc1 = GetLoc(cUnit, inst->getOperand(0));
  llvm::Value* rhs = inst->getOperand(1);
  llvm::ConstantInt* constRhs = llvm::dyn_cast<llvm::ConstantInt>(rhs);
  if (!rlDest.wide && (constRhs != NULL)) {
    Instruction::Code dalvikOp = GetDalvikOpcode(op, true, false);
    GenArithOpIntLit(cUnit, dalvikOp, rlDest, rlSrc1, constRhs->getSExtValue());
  } else {
    Instruction::Code dalvikOp = GetDalvikOpcode(op, false, rlDest.wide);
    RegLocation rlSrc2;
    if (constRhs != NULL) {
      // ir_builder converts NOT_LONG to xor src, -1.  Restore
      DCHECK_EQ(dalvikOp, Instruction::XOR_LONG);
      DCHECK_EQ(-1L, constRhs->getSExtValue());
      dalvikOp = Instruction::NOT_LONG;
      rlSrc2 = rlSrc1;
    } else {
      rlSrc2 = GetLoc(cUnit, rhs);
    }
    if (rlDest.wide) {
      GenArithOpLong(cUnit, dalvikOp, rlDest, rlSrc1, rlSrc2);
    } else {
      GenArithOpInt(cUnit, dalvikOp, rlDest, rlSrc1, rlSrc2);
    }
  }
}

void CvtShiftOp(CompilationUnit* cUnit, Instruction::Code opcode,
                llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  RegLocation rlDest = GetLoc(cUnit, callInst);
  RegLocation rlSrc = GetLoc(cUnit, callInst->getArgOperand(0));
  llvm::Value* rhs = callInst->getArgOperand(1);
  if (llvm::ConstantInt* src2 = llvm::dyn_cast<llvm::ConstantInt>(rhs)) {
    DCHECK(!rlDest.wide);
    GenArithOpIntLit(cUnit, opcode, rlDest, rlSrc, src2->getSExtValue());
  } else {
    RegLocation rlShift = GetLoc(cUnit, rhs);
    if (callInst->getType() == cUnit->irb->getInt64Ty()) {
      GenShiftOpLong(cUnit, opcode, rlDest, rlSrc, rlShift);
    } else {
      GenArithOpInt(cUnit, opcode, rlDest, rlSrc, rlShift);
    }
  }
}

void CvtBr(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  llvm::BranchInst* brInst = llvm::dyn_cast<llvm::BranchInst>(inst);
  DCHECK(brInst != NULL);
  DCHECK(brInst->isUnconditional());  // May change - but this is all we use now
  llvm::BasicBlock* targetBB = brInst->getSuccessor(0);
  OpUnconditionalBranch(cUnit, cUnit->blockToLabelMap.Get(targetBB));
}

void CvtPhi(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  // Nop - these have already been processed
}

void CvtRet(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  llvm::ReturnInst* retInst = llvm::dyn_cast<llvm::ReturnInst>(inst);
  llvm::Value* retVal = retInst->getReturnValue();
  if (retVal != NULL) {
    RegLocation rlSrc = GetLoc(cUnit, retVal);
    if (rlSrc.wide) {
      StoreValueWide(cUnit, GetReturnWide(cUnit, rlSrc.fp), rlSrc);
    } else {
      StoreValue(cUnit, GetReturn(cUnit, rlSrc.fp), rlSrc);
    }
  }
  GenExitSequence(cUnit);
}

ConditionCode GetCond(llvm::ICmpInst::Predicate llvmCond)
{
  ConditionCode res = kCondAl;
  switch(llvmCond) {
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

void CvtICmp(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  // GenCmpLong(cUnit, rlDest, rlSrc1, rlSrc2)
  UNIMPLEMENTED(FATAL);
}

void CvtICmpBr(CompilationUnit* cUnit, llvm::Instruction* inst,
               llvm::BranchInst* brInst)
{
  // Get targets
  llvm::BasicBlock* takenBB = brInst->getSuccessor(0);
  LIR* taken = cUnit->blockToLabelMap.Get(takenBB);
  llvm::BasicBlock* fallThroughBB = brInst->getSuccessor(1);
  LIR* fallThrough = cUnit->blockToLabelMap.Get(fallThroughBB);
  // Get comparison operands
  llvm::ICmpInst* iCmpInst = llvm::dyn_cast<llvm::ICmpInst>(inst);
  ConditionCode cond = GetCond(iCmpInst->getPredicate());
  llvm::Value* lhs = iCmpInst->getOperand(0);
  // Not expecting a constant as 1st operand
  DCHECK(llvm::dyn_cast<llvm::ConstantInt>(lhs) == NULL);
  RegLocation rlSrc1 = GetLoc(cUnit, inst->getOperand(0));
  rlSrc1 = LoadValue(cUnit, rlSrc1, kCoreReg);
  llvm::Value* rhs = inst->getOperand(1);
  if (cUnit->instructionSet == kMips) {
    // Compare and branch in one shot
    UNIMPLEMENTED(FATAL);
  }
  //Compare, then branch
  // TODO: handle fused CMP_LONG/IF_xxZ case
  if (llvm::ConstantInt* src2 = llvm::dyn_cast<llvm::ConstantInt>(rhs)) {
    OpRegImm(cUnit, kOpCmp, rlSrc1.lowReg, src2->getSExtValue());
  } else if (llvm::dyn_cast<llvm::ConstantPointerNull>(rhs) != NULL) {
    OpRegImm(cUnit, kOpCmp, rlSrc1.lowReg, 0);
  } else {
    RegLocation rlSrc2 = GetLoc(cUnit, rhs);
    rlSrc2 = LoadValue(cUnit, rlSrc2, kCoreReg);
    OpRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
  }
  OpCondBranch(cUnit, cond, taken);
  // Fallthrough
  OpUnconditionalBranch(cUnit, fallThrough);
}

void CvtCall(CompilationUnit* cUnit, llvm::CallInst* callInst,
             llvm::Function* callee)
{
  UNIMPLEMENTED(FATAL);
}

void CvtCopy(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  RegLocation rlSrc = GetLoc(cUnit, callInst->getArgOperand(0));
  RegLocation rlDest = GetLoc(cUnit, callInst);
  DCHECK_EQ(rlSrc.wide, rlDest.wide);
  DCHECK_EQ(rlSrc.fp, rlDest.fp);
  if (rlSrc.wide) {
    StoreValueWide(cUnit, rlDest, rlSrc);
  } else {
    StoreValue(cUnit, rlDest, rlSrc);
  }
}

// Note: Immediate arg is a ConstantInt regardless of result type
void CvtConst(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  llvm::ConstantInt* src =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint64_t immval = src->getZExtValue();
  RegLocation rlDest = GetLoc(cUnit, callInst);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kAnyReg, true);
  if (rlDest.wide) {
    LoadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                          (immval) & 0xffffffff, (immval >> 32) & 0xffffffff);
    StoreValueWide(cUnit, rlDest, rlResult);
  } else {
    LoadConstantNoClobber(cUnit, rlResult.lowReg, immval & 0xffffffff);
    StoreValue(cUnit, rlDest, rlResult);
  }
}

void CvtConstObject(CompilationUnit* cUnit, llvm::CallInst* callInst,
                    bool isString)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  llvm::ConstantInt* idxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t index = idxVal->getZExtValue();
  RegLocation rlDest = GetLoc(cUnit, callInst);
  if (isString) {
    GenConstString(cUnit, index, rlDest);
  } else {
    GenConstClass(cUnit, index, rlDest);
  }
}

void CvtFillArrayData(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* offsetVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlSrc = GetLoc(cUnit, callInst->getArgOperand(1));
  GenFillArrayData(cUnit, offsetVal->getSExtValue(), rlSrc);
}

void CvtNewInstance(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  llvm::ConstantInt* typeIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t typeIdx = typeIdxVal->getZExtValue();
  RegLocation rlDest = GetLoc(cUnit, callInst);
  GenNewInstance(cUnit, typeIdx, rlDest);
}

void CvtNewArray(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* typeIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t typeIdx = typeIdxVal->getZExtValue();
  llvm::Value* len = callInst->getArgOperand(1);
  RegLocation rlLen = GetLoc(cUnit, len);
  RegLocation rlDest = GetLoc(cUnit, callInst);
  GenNewArray(cUnit, typeIdx, rlDest, rlLen);
}

void CvtInstanceOf(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* typeIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t typeIdx = typeIdxVal->getZExtValue();
  llvm::Value* src = callInst->getArgOperand(1);
  RegLocation rlSrc = GetLoc(cUnit, src);
  RegLocation rlDest = GetLoc(cUnit, callInst);
  GenInstanceof(cUnit, typeIdx, rlDest, rlSrc);
}

void CvtThrow(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  llvm::Value* src = callInst->getArgOperand(0);
  RegLocation rlSrc = GetLoc(cUnit, src);
  GenThrow(cUnit, rlSrc);
}

void CvtMonitorEnterExit(CompilationUnit* cUnit, bool isEnter,
                         llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  llvm::Value* src = callInst->getArgOperand(1);
  RegLocation rlSrc = GetLoc(cUnit, src);
  if (isEnter) {
    GenMonitorEnter(cUnit, optFlags->getZExtValue(), rlSrc);
  } else {
    GenMonitorExit(cUnit, optFlags->getZExtValue(), rlSrc);
  }
}

void CvtArrayLength(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  llvm::Value* src = callInst->getArgOperand(1);
  RegLocation rlSrc = GetLoc(cUnit, src);
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  GenNullCheck(cUnit, rlSrc.sRegLow, rlSrc.lowReg, optFlags->getZExtValue());
  RegLocation rlDest = GetLoc(cUnit, callInst);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  int lenOffset = Array::LengthOffset().Int32Value();
  LoadWordDisp(cUnit, rlSrc.lowReg, lenOffset, rlResult.lowReg);
  StoreValue(cUnit, rlDest, rlResult);
}

void CvtMoveException(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  RegLocation rlDest = GetLoc(cUnit, callInst);
  GenMoveException(cUnit, rlDest);
}

void CvtSget(CompilationUnit* cUnit, llvm::CallInst* callInst, bool isWide,
             bool isObject)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  llvm::ConstantInt* typeIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t typeIdx = typeIdxVal->getZExtValue();
  RegLocation rlDest = GetLoc(cUnit, callInst);
  GenSget(cUnit, typeIdx, rlDest, isWide, isObject);
}

void CvtSput(CompilationUnit* cUnit, llvm::CallInst* callInst, bool isWide,
             bool isObject)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* typeIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t typeIdx = typeIdxVal->getZExtValue();
  llvm::Value* src = callInst->getArgOperand(1);
  RegLocation rlSrc = GetLoc(cUnit, src);
  GenSput(cUnit, typeIdx, rlSrc, isWide, isObject);
}

void CvtAget(CompilationUnit* cUnit, llvm::CallInst* callInst, OpSize size,
             int scale)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 3U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlArray = GetLoc(cUnit, callInst->getArgOperand(1));
  RegLocation rlIndex = GetLoc(cUnit, callInst->getArgOperand(2));
  RegLocation rlDest = GetLoc(cUnit, callInst);
  GenArrayGet(cUnit, optFlags->getZExtValue(), size, rlArray, rlIndex,
              rlDest, scale);
}

void CvtAput(CompilationUnit* cUnit, llvm::CallInst* callInst, OpSize size,
             int scale, bool isObject)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 4U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlSrc = GetLoc(cUnit, callInst->getArgOperand(1));
  RegLocation rlArray = GetLoc(cUnit, callInst->getArgOperand(2));
  RegLocation rlIndex = GetLoc(cUnit, callInst->getArgOperand(3));
  if (isObject) {
    GenArrayObjPut(cUnit, optFlags->getZExtValue(), rlArray, rlIndex,
                   rlSrc, scale);
  } else {
    GenArrayPut(cUnit, optFlags->getZExtValue(), size, rlArray, rlIndex,
                rlSrc, scale);
  }
}

void CvtAputObj(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  CvtAput(cUnit, callInst, kWord, 2, true /* isObject */);
}

void CvtAputPrimitive(CompilationUnit* cUnit, llvm::CallInst* callInst,
                      OpSize size, int scale)
{
  CvtAput(cUnit, callInst, size, scale, false /* isObject */);
}

void CvtIget(CompilationUnit* cUnit, llvm::CallInst* callInst, OpSize size,
             bool isWide, bool isObj)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 3U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlObj = GetLoc(cUnit, callInst->getArgOperand(1));
  llvm::ConstantInt* fieldIdx =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(2));
  RegLocation rlDest = GetLoc(cUnit, callInst);
  GenIGet(cUnit, fieldIdx->getZExtValue(), optFlags->getZExtValue(),
          size, rlDest, rlObj, isWide, isObj);
}

void CvtIput(CompilationUnit* cUnit, llvm::CallInst* callInst, OpSize size,
             bool isWide, bool isObj)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 4U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlSrc = GetLoc(cUnit, callInst->getArgOperand(1));
  RegLocation rlObj = GetLoc(cUnit, callInst->getArgOperand(2));
  llvm::ConstantInt* fieldIdx =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(3));
  GenIPut(cUnit, fieldIdx->getZExtValue(), optFlags->getZExtValue(),
          size, rlSrc, rlObj, isWide, isObj);
}

void CvtCheckCast(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* typeIdx =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlSrc = GetLoc(cUnit, callInst->getArgOperand(1));
  GenCheckCast(cUnit, typeIdx->getZExtValue(), rlSrc);
}

void CvtFPCompare(CompilationUnit* cUnit, llvm::CallInst* callInst,
                  Instruction::Code opcode)
{
  RegLocation rlSrc1 = GetLoc(cUnit, callInst->getArgOperand(0));
  RegLocation rlSrc2 = GetLoc(cUnit, callInst->getArgOperand(1));
  RegLocation rlDest = GetLoc(cUnit, callInst);
  GenCmpFP(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
}

void CvtLongCompare(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  RegLocation rlSrc1 = GetLoc(cUnit, callInst->getArgOperand(0));
  RegLocation rlSrc2 = GetLoc(cUnit, callInst->getArgOperand(1));
  RegLocation rlDest = GetLoc(cUnit, callInst);
  GenCmpLong(cUnit, rlDest, rlSrc1, rlSrc2);
}

void CvtSwitch(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  llvm::SwitchInst* swInst = llvm::dyn_cast<llvm::SwitchInst>(inst);
  DCHECK(swInst != NULL);
  llvm::Value* testVal = swInst->getCondition();
  llvm::MDNode* tableOffsetNode = swInst->getMetadata("SwitchTable");
  DCHECK(tableOffsetNode != NULL);
  llvm::ConstantInt* tableOffsetValue =
          static_cast<llvm::ConstantInt*>(tableOffsetNode->getOperand(0));
  int32_t tableOffset = tableOffsetValue->getSExtValue();
  RegLocation rlSrc = GetLoc(cUnit, testVal);
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  uint16_t tableMagic = *table;
  if (tableMagic == 0x100) {
    GenPackedSwitch(cUnit, tableOffset, rlSrc);
  } else {
    DCHECK_EQ(tableMagic, 0x200);
    GenSparseSwitch(cUnit, tableOffset, rlSrc);
  }
}

void CvtInvoke(CompilationUnit* cUnit, llvm::CallInst* callInst,
               bool isVoid, bool isFilledNewArray)
{
  CallInfo* info = static_cast<CallInfo*>(NewMem(cUnit, sizeof(CallInfo), true, kAllocMisc));
  if (isVoid) {
    info->result.location = kLocInvalid;
  } else {
    info->result = GetLoc(cUnit, callInst);
  }
  llvm::ConstantInt* invokeTypeVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  llvm::ConstantInt* methodIndexVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(1));
  llvm::ConstantInt* optFlagsVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(2));
  info->type = static_cast<InvokeType>(invokeTypeVal->getZExtValue());
  info->index = methodIndexVal->getZExtValue();
  info->optFlags = optFlagsVal->getZExtValue();
  info->offset = cUnit->currentDalvikOffset;

  // Count the argument words, and then build argument array.
  info->numArgWords = 0;
  for (unsigned int i = 3; i < callInst->getNumArgOperands(); i++) {
    RegLocation tLoc = GetLoc(cUnit, callInst->getArgOperand(i));
    info->numArgWords += tLoc.wide ? 2 : 1;
  }
  info->args = (info->numArgWords == 0) ? NULL : static_cast<RegLocation*>
      (NewMem(cUnit, sizeof(RegLocation) * info->numArgWords, false, kAllocMisc));
  // Now, fill in the location records, synthesizing high loc of wide vals
  for (int i = 3, next = 0; next < info->numArgWords;) {
    info->args[next] = GetLoc(cUnit, callInst->getArgOperand(i++));
    if (info->args[next].wide) {
      next++;
      // TODO: Might make sense to mark this as an invalid loc
      info->args[next].origSReg = info->args[next-1].origSReg+1;
      info->args[next].sRegLow = info->args[next-1].sRegLow+1;
    }
    next++;
  }
  // TODO - rework such that we no longer need isRange
  info->isRange = (info->numArgWords > 5);

  if (isFilledNewArray) {
    GenFilledNewArray(cUnit, info);
  } else {
    GenInvoke(cUnit, info);
  }
}

/* Look up the RegLocation associated with a Value.  Must already be defined */
RegLocation ValToLoc(CompilationUnit* cUnit, llvm::Value* val)
{
  SafeMap<llvm::Value*, RegLocation>::iterator it = cUnit->locMap.find(val);
  DCHECK(it != cUnit->locMap.end()) << "Missing definition";
  return it->second;
}

bool MethodBitcodeBlockCodeGen(CompilationUnit* cUnit, llvm::BasicBlock* bb)
{
  while (cUnit->llvmBlocks.find(bb) == cUnit->llvmBlocks.end()) {
    llvm::BasicBlock* nextBB = NULL;
    cUnit->llvmBlocks.insert(bb);
    bool isEntry = (bb == &cUnit->func->getEntryBlock());
    // Define the starting label
    LIR* blockLabel = cUnit->blockToLabelMap.Get(bb);
    // Extract the type and starting offset from the block's name
    char blockType = kInvalidBlock;
    if (isEntry) {
      blockType = kNormalBlock;
      blockLabel->operands[0] = 0;
    } else if (!bb->hasName()) {
      blockType = kNormalBlock;
      blockLabel->operands[0] = DexFile::kDexNoIndex;
    } else {
      std::string blockName = bb->getName().str();
      int dummy;
      sscanf(blockName.c_str(), kLabelFormat, &blockType, &blockLabel->operands[0], &dummy);
      cUnit->currentDalvikOffset = blockLabel->operands[0];
    }
    DCHECK((blockType == kNormalBlock) || (blockType == kCatchBlock));
    cUnit->currentDalvikOffset = blockLabel->operands[0];
    // Set the label kind
    blockLabel->opcode = kPseudoNormalBlockLabel;
    // Insert the label
    AppendLIR(cUnit, blockLabel);

    LIR* headLIR = NULL;

    if (blockType == kCatchBlock) {
      headLIR = NewLIR0(cUnit, kPseudoExportedPC);
    }

    // Free temp registers and reset redundant store tracking */
    ResetRegPool(cUnit);
    ResetDefTracking(cUnit);

    //TODO: restore oat incoming liveness optimization
    ClobberAllRegs(cUnit);

    if (isEntry) {
      RegLocation* ArgLocs = static_cast<RegLocation*>
          (NewMem(cUnit, sizeof(RegLocation) * cUnit->numIns, true, kAllocMisc));
      llvm::Function::arg_iterator it(cUnit->func->arg_begin());
      llvm::Function::arg_iterator it_end(cUnit->func->arg_end());
      // Skip past Method*
      it++;
      for (unsigned i = 0; it != it_end; ++it) {
        llvm::Value* val = it;
        ArgLocs[i++] = ValToLoc(cUnit, val);
        llvm::Type* ty = val->getType();
        if ((ty == cUnit->irb->getInt64Ty()) || (ty == cUnit->irb->getDoubleTy())) {
          ArgLocs[i] = ArgLocs[i-1];
          ArgLocs[i].lowReg = ArgLocs[i].highReg;
          ArgLocs[i].origSReg++;
          ArgLocs[i].sRegLow = INVALID_SREG;
          ArgLocs[i].highWord = true;
          i++;
        }
      }
      GenEntrySequence(cUnit, ArgLocs, cUnit->methodLoc);
    }

    // Visit all of the instructions in the block
    for (llvm::BasicBlock::iterator it = bb->begin(), e = bb->end(); it != e;) {
      llvm::Instruction* inst = it;
      llvm::BasicBlock::iterator nextIt = ++it;
      // Extract the Dalvik offset from the instruction
      uint32_t opcode = inst->getOpcode();
      llvm::MDNode* dexOffsetNode = inst->getMetadata("DexOff");
      if (dexOffsetNode != NULL) {
        llvm::ConstantInt* dexOffsetValue =
            static_cast<llvm::ConstantInt*>(dexOffsetNode->getOperand(0));
        cUnit->currentDalvikOffset = dexOffsetValue->getZExtValue();
      }

      ResetRegPool(cUnit);
      if (cUnit->disableOpt & (1 << kTrackLiveTemps)) {
        ClobberAllRegs(cUnit);
      }

      if (cUnit->disableOpt & (1 << kSuppressLoads)) {
        ResetDefTracking(cUnit);
      }

  #ifndef NDEBUG
      /* Reset temp tracking sanity check */
      cUnit->liveSReg = INVALID_SREG;
  #endif

      // TODO: use llvm opcode name here instead of "boundary" if verbose
      LIR* boundaryLIR = MarkBoundary(cUnit, cUnit->currentDalvikOffset, "boundary");

      /* Remember the first LIR for thisl block*/
      if (headLIR == NULL) {
        headLIR = boundaryLIR;
        headLIR->defMask = ENCODE_ALL;
      }

      switch(opcode) {

        case llvm::Instruction::ICmp: {
            llvm::Instruction* nextInst = nextIt;
            llvm::BranchInst* brInst = llvm::dyn_cast<llvm::BranchInst>(nextInst);
            if (brInst != NULL /* and... */) {
              CvtICmpBr(cUnit, inst, brInst);
              ++it;
            } else {
              CvtICmp(cUnit, inst);
            }
          }
          break;

        case llvm::Instruction::Call: {
            llvm::CallInst* callInst = llvm::dyn_cast<llvm::CallInst>(inst);
            llvm::Function* callee = callInst->getCalledFunction();
            greenland::IntrinsicHelper::IntrinsicId id =
                cUnit->intrinsic_helper->GetIntrinsicId(callee);
            switch (id) {
              case greenland::IntrinsicHelper::AllocaShadowFrame:
              case greenland::IntrinsicHelper::SetShadowFrameEntry:
              case greenland::IntrinsicHelper::PopShadowFrame:
              case greenland::IntrinsicHelper::SetVReg:
                // Ignore shadow frame stuff for quick compiler
                break;
              case greenland::IntrinsicHelper::CopyInt:
              case greenland::IntrinsicHelper::CopyObj:
              case greenland::IntrinsicHelper::CopyFloat:
              case greenland::IntrinsicHelper::CopyLong:
              case greenland::IntrinsicHelper::CopyDouble:
                CvtCopy(cUnit, callInst);
                break;
              case greenland::IntrinsicHelper::ConstInt:
              case greenland::IntrinsicHelper::ConstObj:
              case greenland::IntrinsicHelper::ConstLong:
              case greenland::IntrinsicHelper::ConstFloat:
              case greenland::IntrinsicHelper::ConstDouble:
                CvtConst(cUnit, callInst);
                break;
              case greenland::IntrinsicHelper::DivInt:
              case greenland::IntrinsicHelper::DivLong:
                CvtBinOp(cUnit, kOpDiv, inst);
                break;
              case greenland::IntrinsicHelper::RemInt:
              case greenland::IntrinsicHelper::RemLong:
                CvtBinOp(cUnit, kOpRem, inst);
                break;
              case greenland::IntrinsicHelper::MethodInfo:
                // Already dealt with - just ignore it here.
                break;
              case greenland::IntrinsicHelper::CheckSuspend:
                GenSuspendTest(cUnit, 0 /* optFlags already applied */);
                break;
              case greenland::IntrinsicHelper::HLInvokeObj:
              case greenland::IntrinsicHelper::HLInvokeFloat:
              case greenland::IntrinsicHelper::HLInvokeDouble:
              case greenland::IntrinsicHelper::HLInvokeLong:
              case greenland::IntrinsicHelper::HLInvokeInt:
                CvtInvoke(cUnit, callInst, false /* isVoid */, false /* newArray */);
                break;
              case greenland::IntrinsicHelper::HLInvokeVoid:
                CvtInvoke(cUnit, callInst, true /* isVoid */, false /* newArray */);
                break;
              case greenland::IntrinsicHelper::HLFilledNewArray:
                CvtInvoke(cUnit, callInst, false /* isVoid */, true /* newArray */);
                break;
              case greenland::IntrinsicHelper::HLFillArrayData:
                CvtFillArrayData(cUnit, callInst);
                break;
              case greenland::IntrinsicHelper::ConstString:
                CvtConstObject(cUnit, callInst, true /* isString */);
                break;
              case greenland::IntrinsicHelper::ConstClass:
                CvtConstObject(cUnit, callInst, false /* isString */);
                break;
              case greenland::IntrinsicHelper::HLCheckCast:
                CvtCheckCast(cUnit, callInst);
                break;
              case greenland::IntrinsicHelper::NewInstance:
                CvtNewInstance(cUnit, callInst);
                break;
              case greenland::IntrinsicHelper::HLSgetObject:
                CvtSget(cUnit, callInst, false /* wide */, true /* Object */);
                break;
              case greenland::IntrinsicHelper::HLSget:
              case greenland::IntrinsicHelper::HLSgetFloat:
              case greenland::IntrinsicHelper::HLSgetBoolean:
              case greenland::IntrinsicHelper::HLSgetByte:
              case greenland::IntrinsicHelper::HLSgetChar:
              case greenland::IntrinsicHelper::HLSgetShort:
                CvtSget(cUnit, callInst, false /* wide */, false /* Object */);
                break;
              case greenland::IntrinsicHelper::HLSgetWide:
              case greenland::IntrinsicHelper::HLSgetDouble:
                CvtSget(cUnit, callInst, true /* wide */, false /* Object */);
                break;
              case greenland::IntrinsicHelper::HLSput:
              case greenland::IntrinsicHelper::HLSputFloat:
              case greenland::IntrinsicHelper::HLSputBoolean:
              case greenland::IntrinsicHelper::HLSputByte:
              case greenland::IntrinsicHelper::HLSputChar:
              case greenland::IntrinsicHelper::HLSputShort:
                CvtSput(cUnit, callInst, false /* wide */, false /* Object */);
                break;
              case greenland::IntrinsicHelper::HLSputWide:
              case greenland::IntrinsicHelper::HLSputDouble:
                CvtSput(cUnit, callInst, true /* wide */, false /* Object */);
                break;
              case greenland::IntrinsicHelper::HLSputObject:
                CvtSput(cUnit, callInst, false /* wide */, true /* Object */);
                break;
              case greenland::IntrinsicHelper::GetException:
                CvtMoveException(cUnit, callInst);
                break;
              case greenland::IntrinsicHelper::HLThrowException:
                CvtThrow(cUnit, callInst);
                break;
              case greenland::IntrinsicHelper::MonitorEnter:
                CvtMonitorEnterExit(cUnit, true /* isEnter */, callInst);
                break;
              case greenland::IntrinsicHelper::MonitorExit:
                CvtMonitorEnterExit(cUnit, false /* isEnter */, callInst);
                break;
              case greenland::IntrinsicHelper::OptArrayLength:
                CvtArrayLength(cUnit, callInst);
                break;
              case greenland::IntrinsicHelper::NewArray:
                CvtNewArray(cUnit, callInst);
                break;
              case greenland::IntrinsicHelper::InstanceOf:
                CvtInstanceOf(cUnit, callInst);
                break;

              case greenland::IntrinsicHelper::HLArrayGet:
              case greenland::IntrinsicHelper::HLArrayGetObject:
              case greenland::IntrinsicHelper::HLArrayGetFloat:
                CvtAget(cUnit, callInst, kWord, 2);
                break;
              case greenland::IntrinsicHelper::HLArrayGetWide:
              case greenland::IntrinsicHelper::HLArrayGetDouble:
                CvtAget(cUnit, callInst, kLong, 3);
                break;
              case greenland::IntrinsicHelper::HLArrayGetBoolean:
                CvtAget(cUnit, callInst, kUnsignedByte, 0);
                break;
              case greenland::IntrinsicHelper::HLArrayGetByte:
                CvtAget(cUnit, callInst, kSignedByte, 0);
                break;
              case greenland::IntrinsicHelper::HLArrayGetChar:
                CvtAget(cUnit, callInst, kUnsignedHalf, 1);
                break;
              case greenland::IntrinsicHelper::HLArrayGetShort:
                CvtAget(cUnit, callInst, kSignedHalf, 1);
                break;

              case greenland::IntrinsicHelper::HLArrayPut:
              case greenland::IntrinsicHelper::HLArrayPutFloat:
                CvtAputPrimitive(cUnit, callInst, kWord, 2);
                break;
              case greenland::IntrinsicHelper::HLArrayPutObject:
                CvtAputObj(cUnit, callInst);
                break;
              case greenland::IntrinsicHelper::HLArrayPutWide:
              case greenland::IntrinsicHelper::HLArrayPutDouble:
                CvtAputPrimitive(cUnit, callInst, kLong, 3);
                break;
              case greenland::IntrinsicHelper::HLArrayPutBoolean:
                CvtAputPrimitive(cUnit, callInst, kUnsignedByte, 0);
                break;
              case greenland::IntrinsicHelper::HLArrayPutByte:
                CvtAputPrimitive(cUnit, callInst, kSignedByte, 0);
                break;
              case greenland::IntrinsicHelper::HLArrayPutChar:
                CvtAputPrimitive(cUnit, callInst, kUnsignedHalf, 1);
                break;
              case greenland::IntrinsicHelper::HLArrayPutShort:
                CvtAputPrimitive(cUnit, callInst, kSignedHalf, 1);
                break;

              case greenland::IntrinsicHelper::HLIGet:
              case greenland::IntrinsicHelper::HLIGetFloat:
                CvtIget(cUnit, callInst, kWord, false /* isWide */, false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetObject:
                CvtIget(cUnit, callInst, kWord, false /* isWide */, true /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetWide:
              case greenland::IntrinsicHelper::HLIGetDouble:
                CvtIget(cUnit, callInst, kLong, true /* isWide */, false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetBoolean:
                CvtIget(cUnit, callInst, kUnsignedByte, false /* isWide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetByte:
                CvtIget(cUnit, callInst, kSignedByte, false /* isWide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetChar:
                CvtIget(cUnit, callInst, kUnsignedHalf, false /* isWide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIGetShort:
                CvtIget(cUnit, callInst, kSignedHalf, false /* isWide */,
                        false /* obj */);
                break;

              case greenland::IntrinsicHelper::HLIPut:
              case greenland::IntrinsicHelper::HLIPutFloat:
                CvtIput(cUnit, callInst, kWord, false /* isWide */, false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutObject:
                CvtIput(cUnit, callInst, kWord, false /* isWide */, true /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutWide:
              case greenland::IntrinsicHelper::HLIPutDouble:
                CvtIput(cUnit, callInst, kLong, true /* isWide */, false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutBoolean:
                CvtIput(cUnit, callInst, kUnsignedByte, false /* isWide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutByte:
                CvtIput(cUnit, callInst, kSignedByte, false /* isWide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutChar:
                CvtIput(cUnit, callInst, kUnsignedHalf, false /* isWide */,
                        false /* obj */);
                break;
              case greenland::IntrinsicHelper::HLIPutShort:
                CvtIput(cUnit, callInst, kSignedHalf, false /* isWide */,
                        false /* obj */);
                break;

              case greenland::IntrinsicHelper::IntToChar:
                CvtIntNarrowing(cUnit, callInst, Instruction::INT_TO_CHAR);
                break;
              case greenland::IntrinsicHelper::IntToShort:
                CvtIntNarrowing(cUnit, callInst, Instruction::INT_TO_SHORT);
                break;
              case greenland::IntrinsicHelper::IntToByte:
                CvtIntNarrowing(cUnit, callInst, Instruction::INT_TO_BYTE);
                break;

              case greenland::IntrinsicHelper::F2I:
              case greenland::IntrinsicHelper::D2I:
              case greenland::IntrinsicHelper::F2L:
              case greenland::IntrinsicHelper::D2L:
                CvtFPToInt(cUnit, callInst);
                break;

              case greenland::IntrinsicHelper::CmplFloat:
                CvtFPCompare(cUnit, callInst, Instruction::CMPL_FLOAT);
                break;
              case greenland::IntrinsicHelper::CmpgFloat:
                CvtFPCompare(cUnit, callInst, Instruction::CMPG_FLOAT);
                break;
              case greenland::IntrinsicHelper::CmplDouble:
                CvtFPCompare(cUnit, callInst, Instruction::CMPL_DOUBLE);
                break;
              case greenland::IntrinsicHelper::CmpgDouble:
                CvtFPCompare(cUnit, callInst, Instruction::CMPG_DOUBLE);
                break;

              case greenland::IntrinsicHelper::CmpLong:
                CvtLongCompare(cUnit, callInst);
                break;

              case greenland::IntrinsicHelper::SHLLong:
                CvtShiftOp(cUnit, Instruction::SHL_LONG, callInst);
                break;
              case greenland::IntrinsicHelper::SHRLong:
                CvtShiftOp(cUnit, Instruction::SHR_LONG, callInst);
                break;
              case greenland::IntrinsicHelper::USHRLong:
                CvtShiftOp(cUnit, Instruction::USHR_LONG, callInst);
                break;
              case greenland::IntrinsicHelper::SHLInt:
                CvtShiftOp(cUnit, Instruction::SHL_INT, callInst);
                break;
              case greenland::IntrinsicHelper::SHRInt:
                CvtShiftOp(cUnit, Instruction::SHR_INT, callInst);
                break;
              case greenland::IntrinsicHelper::USHRInt:
                CvtShiftOp(cUnit, Instruction::USHR_INT, callInst);
                break;

              case greenland::IntrinsicHelper::CatchTargets: {
                  llvm::SwitchInst* swInst =
                      llvm::dyn_cast<llvm::SwitchInst>(nextIt);
                  DCHECK(swInst != NULL);
                  /*
                   * Discard the edges and the following conditional branch.
                   * Do a direct branch to the default target (which is the
                   * "work" portion of the pair.
                   * TODO: awful code layout - rework
                   */
                   llvm::BasicBlock* targetBB = swInst->getDefaultDest();
                   DCHECK(targetBB != NULL);
                   OpUnconditionalBranch(cUnit,
                                         cUnit->blockToLabelMap.Get(targetBB));
                   ++it;
                   // Set next bb to default target - improves code layout
                   nextBB = targetBB;
                }
                break;

              default:
                LOG(FATAL) << "Unexpected intrinsic " << cUnit->intrinsic_helper->GetName(id);
            }
          }
          break;

        case llvm::Instruction::Br: CvtBr(cUnit, inst); break;
        case llvm::Instruction::Add: CvtBinOp(cUnit, kOpAdd, inst); break;
        case llvm::Instruction::Sub: CvtBinOp(cUnit, kOpSub, inst); break;
        case llvm::Instruction::Mul: CvtBinOp(cUnit, kOpMul, inst); break;
        case llvm::Instruction::SDiv: CvtBinOp(cUnit, kOpDiv, inst); break;
        case llvm::Instruction::SRem: CvtBinOp(cUnit, kOpRem, inst); break;
        case llvm::Instruction::And: CvtBinOp(cUnit, kOpAnd, inst); break;
        case llvm::Instruction::Or: CvtBinOp(cUnit, kOpOr, inst); break;
        case llvm::Instruction::Xor: CvtBinOp(cUnit, kOpXor, inst); break;
        case llvm::Instruction::PHI: CvtPhi(cUnit, inst); break;
        case llvm::Instruction::Ret: CvtRet(cUnit, inst); break;
        case llvm::Instruction::FAdd: CvtBinFPOp(cUnit, kOpAdd, inst); break;
        case llvm::Instruction::FSub: CvtBinFPOp(cUnit, kOpSub, inst); break;
        case llvm::Instruction::FMul: CvtBinFPOp(cUnit, kOpMul, inst); break;
        case llvm::Instruction::FDiv: CvtBinFPOp(cUnit, kOpDiv, inst); break;
        case llvm::Instruction::FRem: CvtBinFPOp(cUnit, kOpRem, inst); break;
        case llvm::Instruction::SIToFP: CvtIntToFP(cUnit, inst); break;
        case llvm::Instruction::FPTrunc: CvtDoubleToFloat(cUnit, inst); break;
        case llvm::Instruction::FPExt: CvtFloatToDouble(cUnit, inst); break;
        case llvm::Instruction::Trunc: CvtTrunc(cUnit, inst); break;

        case llvm::Instruction::ZExt: CvtIntExt(cUnit, inst, false /* signed */);
          break;
        case llvm::Instruction::SExt: CvtIntExt(cUnit, inst, true /* signed */);
          break;

        case llvm::Instruction::Switch: CvtSwitch(cUnit, inst); break;

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

    if (headLIR != NULL) {
      ApplyLocalOptimizations(cUnit, headLIR, cUnit->lastLIRInsn);
    }
    if (nextBB != NULL) {
      bb = nextBB;
      nextBB = NULL;
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
 *   o Create ssaRep def/use operand arrays for each converted LLVM opcode
 *   o Perform register promotion
 *   o Iterate through the graph a basic block at a time, generating
 *     LIR.
 *   o Assemble LIR as usual.
 *   o Profit.
 */
void MethodBitcode2LIR(CompilationUnit* cUnit)
{
  llvm::Function* func = cUnit->func;
  int numBasicBlocks = func->getBasicBlockList().size();
  // Allocate a list for LIR basic block labels
  cUnit->blockLabelList =
    static_cast<LIR*>(NewMem(cUnit, sizeof(LIR) * numBasicBlocks, true, kAllocLIR));
  LIR* labelList = cUnit->blockLabelList;
  int nextLabel = 0;
  for (llvm::Function::iterator i = func->begin(),
       e = func->end(); i != e; ++i) {
    cUnit->blockToLabelMap.Put(static_cast<llvm::BasicBlock*>(i),
                               &labelList[nextLabel++]);
  }

  /*
   * Keep honest - clear regLocations, Value => RegLocation,
   * promotion map and VmapTables.
   */
  cUnit->locMap.clear();  // Start fresh
  cUnit->regLocation = NULL;
  for (int i = 0; i < cUnit->numDalvikRegisters + cUnit->numCompilerTemps + 1;
       i++) {
    cUnit->promotionMap[i].coreLocation = kLocDalvikFrame;
    cUnit->promotionMap[i].fpLocation = kLocDalvikFrame;
  }
  cUnit->coreSpillMask = 0;
  cUnit->numCoreSpills = 0;
  cUnit->fpSpillMask = 0;
  cUnit->numFPSpills = 0;
  cUnit->coreVmapTable.clear();
  cUnit->fpVmapTable.clear();

  /*
   * At this point, we've lost all knowledge of register promotion.
   * Rebuild that info from the MethodInfo intrinsic (if it
   * exists - not required for correctness).  Normally, this will
   * be the first instruction we encounter, so we won't have to iterate
   * through everything.
   */
  for (llvm::inst_iterator i = llvm::inst_begin(func),
       e = llvm::inst_end(func); i != e; ++i) {
    llvm::CallInst* callInst = llvm::dyn_cast<llvm::CallInst>(&*i);
    if (callInst != NULL) {
      llvm::Function* callee = callInst->getCalledFunction();
      greenland::IntrinsicHelper::IntrinsicId id =
          cUnit->intrinsic_helper->GetIntrinsicId(callee);
      if (id == greenland::IntrinsicHelper::MethodInfo) {
        if (cUnit->printMe) {
          LOG(INFO) << "Found MethodInfo";
        }
        llvm::MDNode* regInfoNode = callInst->getMetadata("RegInfo");
        if (regInfoNode != NULL) {
          llvm::ConstantInt* numInsValue =
            static_cast<llvm::ConstantInt*>(regInfoNode->getOperand(0));
          llvm::ConstantInt* numRegsValue =
            static_cast<llvm::ConstantInt*>(regInfoNode->getOperand(1));
          llvm::ConstantInt* numOutsValue =
            static_cast<llvm::ConstantInt*>(regInfoNode->getOperand(2));
          llvm::ConstantInt* numCompilerTempsValue =
            static_cast<llvm::ConstantInt*>(regInfoNode->getOperand(3));
          llvm::ConstantInt* numSSARegsValue =
            static_cast<llvm::ConstantInt*>(regInfoNode->getOperand(4));
          if (cUnit->printMe) {
             LOG(INFO) << "RegInfo - Ins:" << numInsValue->getZExtValue()
                       << ", Regs:" << numRegsValue->getZExtValue()
                       << ", Outs:" << numOutsValue->getZExtValue()
                       << ", CTemps:" << numCompilerTempsValue->getZExtValue()
                       << ", SSARegs:" << numSSARegsValue->getZExtValue();
            }
          }
        llvm::MDNode* pmapInfoNode = callInst->getMetadata("PromotionMap");
        if (pmapInfoNode != NULL) {
          int elems = pmapInfoNode->getNumOperands();
          if (cUnit->printMe) {
            LOG(INFO) << "PMap size: " << elems;
          }
          for (int i = 0; i < elems; i++) {
            llvm::ConstantInt* rawMapData =
                static_cast<llvm::ConstantInt*>(pmapInfoNode->getOperand(i));
            uint32_t mapData = rawMapData->getZExtValue();
            PromotionMap* p = &cUnit->promotionMap[i];
            p->firstInPair = (mapData >> 24) & 0xff;
            p->FpReg = (mapData >> 16) & 0xff;
            p->coreReg = (mapData >> 8) & 0xff;
            p->fpLocation = static_cast<RegLocationType>((mapData >> 4) & 0xf);
            if (p->fpLocation == kLocPhysReg) {
              RecordFpPromotion(cUnit, p->FpReg, i);
            }
            p->coreLocation = static_cast<RegLocationType>(mapData & 0xf);
            if (p->coreLocation == kLocPhysReg) {
              RecordCorePromotion(cUnit, p->coreReg, i);
            }
          }
          if (cUnit->printMe) {
            DumpPromotionMap(cUnit);
          }
        }
        break;
      }
    }
  }
  AdjustSpillMask(cUnit);
  cUnit->frameSize = ComputeFrameSize(cUnit);

  // Create RegLocations for arguments
  llvm::Function::arg_iterator it(cUnit->func->arg_begin());
  llvm::Function::arg_iterator it_end(cUnit->func->arg_end());
  for (; it != it_end; ++it) {
    llvm::Value* val = it;
    CreateLocFromValue(cUnit, val);
  }
  // Create RegLocations for all non-argument defintions
  for (llvm::inst_iterator i = llvm::inst_begin(func),
       e = llvm::inst_end(func); i != e; ++i) {
    llvm::Value* val = &*i;
    if (val->hasName() && (val->getName().str().c_str()[0] == 'v')) {
      CreateLocFromValue(cUnit, val);
    }
  }

  // Walk the blocks, generating code.
  for (llvm::Function::iterator i = cUnit->func->begin(),
       e = cUnit->func->end(); i != e; ++i) {
    MethodBitcodeBlockCodeGen(cUnit, static_cast<llvm::BasicBlock*>(i));
  }

  HandleSuspendLaunchPads(cUnit);

  HandleThrowLaunchPads(cUnit);

  HandleIntrinsicLaunchPads(cUnit);

  cUnit->func->eraseFromParent();
  cUnit->func = NULL;
}


}  // namespace art
