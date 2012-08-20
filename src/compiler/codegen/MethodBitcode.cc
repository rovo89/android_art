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

#if defined(ART_USE_QUICK_COMPILER)

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

static const char* kLabelFormat = "L0x%x_%d";

namespace art {
extern const RegLocation badLoc;
RegLocation getLoc(CompilationUnit* cUnit, llvm::Value* val);

llvm::BasicBlock* getLLVMBlock(CompilationUnit* cUnit, int id)
{
  return cUnit->idToBlockMap.Get(id);
}

llvm::Value* getLLVMValue(CompilationUnit* cUnit, int sReg)
{
  return (llvm::Value*)oatGrowableListGetElement(&cUnit->llvmValues, sReg);
}

// Replace the placeholder value with the real definition
void defineValue(CompilationUnit* cUnit, llvm::Value* val, int sReg)
{
  llvm::Value* placeholder = getLLVMValue(cUnit, sReg);
  if (placeholder == NULL) {
    // This can happen on instruction rewrite on verification failure
    LOG(WARNING) << "Null placeholder";
    return;
  }
  placeholder->replaceAllUsesWith(val);
  val->takeName(placeholder);
  cUnit->llvmValues.elemList[sReg] = (intptr_t)val;
  llvm::Instruction* inst = llvm::dyn_cast<llvm::Instruction>(placeholder);
  DCHECK(inst != NULL);
  inst->eraseFromParent();
}

llvm::Type* llvmTypeFromLocRec(CompilationUnit* cUnit, RegLocation loc)
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
void createLocFromValue(CompilationUnit* cUnit, llvm::Value* val)
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
      loc.lowReg = pMap.fpReg;
      loc.location = kLocPhysReg;
      loc.home = true;
    }
  } else if (ty == cUnit->irb->getDoubleTy()) {
    loc.fp = true;
    PromotionMap pMapHigh = cUnit->promotionMap[baseSReg + 1];
    if ((pMap.fpLocation == kLocPhysReg) &&
        (pMapHigh.fpLocation == kLocPhysReg) &&
        ((pMap.fpReg & 0x1) == 0) &&
        (pMap.fpReg + 1 == pMapHigh.fpReg)) {
      loc.lowReg = pMap.fpReg;
      loc.highReg = pMapHigh.fpReg;
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
      LOG(INFO) << "Promoted wide " << s << " to regs " << loc.lowReg
                << "/" << loc.highReg;
    } else {
      LOG(INFO) << "Promoted " << s << " to reg " << loc.lowReg;
    }
  }
  cUnit->locMap.Put(val, loc);
}

void initIR(CompilationUnit* cUnit)
{
  cUnit->context = new llvm::LLVMContext();
  cUnit->module = new llvm::Module("art", *cUnit->context);
  llvm::StructType::create(*cUnit->context, "JavaObject");
  llvm::StructType::create(*cUnit->context, "Method");
  llvm::StructType::create(*cUnit->context, "Thread");
  cUnit->intrinsic_helper =
      new greenland::IntrinsicHelper(*cUnit->context, *cUnit->module);
  cUnit->irb =
      new greenland::IRBuilder(*cUnit->context, *cUnit->module,
                               *cUnit->intrinsic_helper);
}

void freeIR(CompilationUnit* cUnit)
{
  delete cUnit->irb;
  delete cUnit->intrinsic_helper;
  delete cUnit->module;
  delete cUnit->context;
}

const char* llvmSSAName(CompilationUnit* cUnit, int ssaReg) {
  return GET_ELEM_N(cUnit->ssaStrings, char*, ssaReg);
}

llvm::BasicBlock* findCaseTarget(CompilationUnit* cUnit, uint32_t vaddr)
{
  BasicBlock* bb = oatFindBlock(cUnit, vaddr);
  DCHECK(bb != NULL);
  return getLLVMBlock(cUnit, bb->id);
}

void convertPackedSwitch(CompilationUnit* cUnit, BasicBlock* bb,
                         int32_t tableOffset, RegLocation rlSrc)
{
  const Instruction::PackedSwitchPayload* payload =
      reinterpret_cast<const Instruction::PackedSwitchPayload*>(
      cUnit->insns + cUnit->currentDalvikOffset + tableOffset);

  llvm::Value* value = getLLVMValue(cUnit, rlSrc.origSReg);

  llvm::SwitchInst* sw =
    cUnit->irb->CreateSwitch(value, getLLVMBlock(cUnit, bb->fallThrough->id),
                             payload->case_count);

  for (uint16_t i = 0; i < payload->case_count; ++i) {
    llvm::BasicBlock* llvmBB =
        findCaseTarget(cUnit, cUnit->currentDalvikOffset + payload->targets[i]);
    sw->addCase(cUnit->irb->getInt32(payload->first_key + i), llvmBB);
  }
  llvm::MDNode* switchNode =
      llvm::MDNode::get(*cUnit->context, cUnit->irb->getInt32(tableOffset));
  sw->setMetadata("SwitchTable", switchNode);
  bb->taken = NULL;
  bb->fallThrough = NULL;
}

void convertSparseSwitch(CompilationUnit* cUnit, BasicBlock* bb,
                         int32_t tableOffset, RegLocation rlSrc)
{
  const Instruction::SparseSwitchPayload* payload =
      reinterpret_cast<const Instruction::SparseSwitchPayload*>(
      cUnit->insns + cUnit->currentDalvikOffset + tableOffset);

  const int32_t* keys = payload->GetKeys();
  const int32_t* targets = payload->GetTargets();

  llvm::Value* value = getLLVMValue(cUnit, rlSrc.origSReg);

  llvm::SwitchInst* sw =
    cUnit->irb->CreateSwitch(value, getLLVMBlock(cUnit, bb->fallThrough->id),
                             payload->case_count);

  for (size_t i = 0; i < payload->case_count; ++i) {
    llvm::BasicBlock* llvmBB =
        findCaseTarget(cUnit, cUnit->currentDalvikOffset + targets[i]);
    sw->addCase(cUnit->irb->getInt32(keys[i]), llvmBB);
  }
  llvm::MDNode* switchNode =
      llvm::MDNode::get(*cUnit->context, cUnit->irb->getInt32(tableOffset));
  sw->setMetadata("SwitchTable", switchNode);
  bb->taken = NULL;
  bb->fallThrough = NULL;
}

void convertSget(CompilationUnit* cUnit, int32_t fieldIndex,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlDest)
{
  llvm::Constant* fieldIdx = cUnit->irb->getInt32(fieldIndex);
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cUnit->irb->CreateCall(intr, fieldIdx);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertSput(CompilationUnit* cUnit, int32_t fieldIndex,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlSrc)
{
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(fieldIndex));
  args.push_back(getLLVMValue(cUnit, rlSrc.origSReg));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(intr, args);
}

void convertFillArrayData(CompilationUnit* cUnit, int32_t offset,
                          RegLocation rlArray)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::FillArrayData;
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(offset));
  args.push_back(getLLVMValue(cUnit, rlArray.origSReg));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(intr, args);
}

llvm::Value* emitConst(CompilationUnit* cUnit, llvm::ArrayRef<llvm::Value*> src,
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

void emitPopShadowFrame(CompilationUnit* cUnit)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::PopShadowFrame);
  cUnit->irb->CreateCall(intr);
}

llvm::Value* emitCopy(CompilationUnit* cUnit, llvm::ArrayRef<llvm::Value*> src,
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

void convertMoveException(CompilationUnit* cUnit, RegLocation rlDest)
{
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::GetException);
  llvm::Value* res = cUnit->irb->CreateCall(func);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertThrow(CompilationUnit* cUnit, RegLocation rlSrc)
{
  llvm::Value* src = getLLVMValue(cUnit, rlSrc.origSReg);
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::Throw);
  cUnit->irb->CreateCall(func, src);
}

void convertMonitorEnterExit(CompilationUnit* cUnit, int optFlags,
                             greenland::IntrinsicHelper::IntrinsicId id,
                             RegLocation rlSrc)
{
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(getLLVMValue(cUnit, rlSrc.origSReg));
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(func, args);
}

void convertArrayLength(CompilationUnit* cUnit, int optFlags,
                        RegLocation rlDest, RegLocation rlSrc)
{
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(getLLVMValue(cUnit, rlSrc.origSReg));
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::ArrayLength);
  llvm::Value* res = cUnit->irb->CreateCall(func, args);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertThrowVerificationError(CompilationUnit* cUnit, int info1, int info2)
{
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(
      greenland::IntrinsicHelper::ThrowVerificationError);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(info1));
  args.push_back(cUnit->irb->getInt32(info2));
  cUnit->irb->CreateCall(func, args);
}

void emitSuspendCheck(CompilationUnit* cUnit)
{
  greenland::IntrinsicHelper::IntrinsicId id =
      greenland::IntrinsicHelper::CheckSuspend;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(intr);
}

llvm::Value* convertCompare(CompilationUnit* cUnit, ConditionCode cc,
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

void convertCompareAndBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                             ConditionCode cc, RegLocation rlSrc1,
                             RegLocation rlSrc2)
{
  if (bb->taken->startOffset <= mir->offset) {
    emitSuspendCheck(cUnit);
  }
  llvm::Value* src1 = getLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2 = getLLVMValue(cUnit, rlSrc2.origSReg);
  llvm::Value* condValue = convertCompare(cUnit, cc, src1, src2);
  condValue->setName(StringPrintf("t%d", cUnit->tempName++));
  cUnit->irb->CreateCondBr(condValue, getLLVMBlock(cUnit, bb->taken->id),
                           getLLVMBlock(cUnit, bb->fallThrough->id));
  // Don't redo the fallthrough branch in the BB driver
  bb->fallThrough = NULL;
}

void convertCompareZeroAndBranch(CompilationUnit* cUnit, BasicBlock* bb,
                                 MIR* mir, ConditionCode cc, RegLocation rlSrc1)
{
  if (bb->taken->startOffset <= mir->offset) {
    emitSuspendCheck(cUnit);
  }
  llvm::Value* src1 = getLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2;
  if (rlSrc1.ref) {
    src2 = cUnit->irb->GetJNull();
  } else {
    src2 = cUnit->irb->getInt32(0);
  }
  llvm::Value* condValue = convertCompare(cUnit, cc, src1, src2);
  cUnit->irb->CreateCondBr(condValue, getLLVMBlock(cUnit, bb->taken->id),
                           getLLVMBlock(cUnit, bb->fallThrough->id));
  // Don't redo the fallthrough branch in the BB driver
  bb->fallThrough = NULL;
}

llvm::Value* genDivModOp(CompilationUnit* cUnit, bool isDiv, bool isLong,
                         llvm::Value* src1, llvm::Value* src2)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  if (isLong) {
    if (isDiv) {
      id = greenland::IntrinsicHelper::DivLong;
    } else {
      id = greenland::IntrinsicHelper::RemLong;
    }
  } else if (isDiv) {
      id = greenland::IntrinsicHelper::DivInt;
    } else {
      id = greenland::IntrinsicHelper::RemInt;
  }
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2>args;
  args.push_back(src1);
  args.push_back(src2);
  return cUnit->irb->CreateCall(intr, args);
}

llvm::Value* genArithOp(CompilationUnit* cUnit, OpKind op, bool isLong,
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
    case kOpDiv: res = genDivModOp(cUnit, true, isLong, src1, src2); break;
    case kOpRem: res = genDivModOp(cUnit, false, isLong, src1, src2); break;
    case kOpLsl: res = cUnit->irb->CreateShl(src1, src2); break;
    case kOpLsr: res = cUnit->irb->CreateLShr(src1, src2); break;
    case kOpAsr: res = cUnit->irb->CreateAShr(src1, src2); break;
    default:
      LOG(FATAL) << "Invalid op " << op;
  }
  return res;
}

void convertFPArithOp(CompilationUnit* cUnit, OpKind op, RegLocation rlDest,
                      RegLocation rlSrc1, RegLocation rlSrc2)
{
  llvm::Value* src1 = getLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2 = getLLVMValue(cUnit, rlSrc2.origSReg);
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
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertShift(CompilationUnit* cUnit,
                  greenland::IntrinsicHelper::IntrinsicId id,
                  RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2>args;
  args.push_back(getLLVMValue(cUnit, rlSrc1.origSReg));
  args.push_back(getLLVMValue(cUnit, rlSrc2.origSReg));
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertShiftLit(CompilationUnit* cUnit,
                     greenland::IntrinsicHelper::IntrinsicId id,
                     RegLocation rlDest, RegLocation rlSrc, int shiftAmount)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2>args;
  args.push_back(getLLVMValue(cUnit, rlSrc.origSReg));
  args.push_back(cUnit->irb->getInt32(shiftAmount));
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertArithOp(CompilationUnit* cUnit, OpKind op, RegLocation rlDest,
                    RegLocation rlSrc1, RegLocation rlSrc2)
{
  llvm::Value* src1 = getLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2 = getLLVMValue(cUnit, rlSrc2.origSReg);
  DCHECK_EQ(src1->getType(), src2->getType());
  llvm::Value* res = genArithOp(cUnit, op, rlDest.wide, src1, src2);
  defineValue(cUnit, res, rlDest.origSReg);
}

void setShadowFrameEntry(CompilationUnit* cUnit, llvm::Value* newVal)
{
  int index = -1;
  DCHECK(newVal != NULL);
  int vReg = SRegToVReg(cUnit, getLoc(cUnit, newVal).origSReg);
  for (int i = 0; i < cUnit->numShadowFrameEntries; i++) {
    if (cUnit->shadowMap[i] == vReg) {
      index = i;
      break;
    }
  }
  DCHECK_NE(index, -1) << "Corrupt shadowMap";
  greenland::IntrinsicHelper::IntrinsicId id =
      greenland::IntrinsicHelper::SetShadowFrameEntry;
  llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* tableSlot = cUnit->irb->getInt32(index);
  llvm::Value* args[] = { newVal, tableSlot };
  cUnit->irb->CreateCall(func, args);
}

void convertArithOpLit(CompilationUnit* cUnit, OpKind op, RegLocation rlDest,
                       RegLocation rlSrc1, int32_t imm)
{
  llvm::Value* src1 = getLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2 = cUnit->irb->getInt32(imm);
  llvm::Value* res = genArithOp(cUnit, op, rlDest.wide, src1, src2);
  defineValue(cUnit, res, rlDest.origSReg);
}

/*
 * Process arguments for invoke.  Note: this code is also used to
 * collect and process arguments for NEW_FILLED_ARRAY and NEW_FILLED_ARRAY_RANGE.
 * The requirements are similar.
 */
void convertInvoke(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                   InvokeType invokeType, bool isRange, bool isFilledNewArray)
{
  CallInfo* info = oatNewCallInfo(cUnit, bb, mir, invokeType, isRange);
  llvm::SmallVector<llvm::Value*, 10> args;
  // Insert the invokeType
  args.push_back(cUnit->irb->getInt32(static_cast<int>(invokeType)));
  // Insert the method_idx
  args.push_back(cUnit->irb->getInt32(info->index));
  // Insert the optimization flags
  args.push_back(cUnit->irb->getInt32(info->optFlags));
  // Now, insert the actual arguments
  for (int i = 0; i < info->numArgWords;) {
    llvm::Value* val = getLLVMValue(cUnit, info->args[i].origSReg);
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
    id = greenland::IntrinsicHelper::FilledNewArray;
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
    defineValue(cUnit, res, info->result.origSReg);
  }
}

void convertConstObject(CompilationUnit* cUnit, uint32_t idx,
                        greenland::IntrinsicHelper::IntrinsicId id,
                        RegLocation rlDest)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* index = cUnit->irb->getInt32(idx);
  llvm::Value* res = cUnit->irb->CreateCall(intr, index);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertCheckCast(CompilationUnit* cUnit, uint32_t type_idx,
                      RegLocation rlSrc)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::CheckCast;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(type_idx));
  args.push_back(getLLVMValue(cUnit, rlSrc.origSReg));
  cUnit->irb->CreateCall(intr, args);
}

void convertNewInstance(CompilationUnit* cUnit, uint32_t type_idx,
                        RegLocation rlDest)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::NewInstance;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* index = cUnit->irb->getInt32(type_idx);
  llvm::Value* res = cUnit->irb->CreateCall(intr, index);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertNewArray(CompilationUnit* cUnit, uint32_t type_idx,
                     RegLocation rlDest, RegLocation rlSrc)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::NewArray;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(type_idx));
  args.push_back(getLLVMValue(cUnit, rlSrc.origSReg));
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertAget(CompilationUnit* cUnit, int optFlags,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlDest, RegLocation rlArray, RegLocation rlIndex)
{
  llvm::SmallVector<llvm::Value*, 3> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(getLLVMValue(cUnit, rlArray.origSReg));
  args.push_back(getLLVMValue(cUnit, rlIndex.origSReg));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertAput(CompilationUnit* cUnit, int optFlags,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlSrc, RegLocation rlArray, RegLocation rlIndex)
{
  llvm::SmallVector<llvm::Value*, 4> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(getLLVMValue(cUnit, rlSrc.origSReg));
  args.push_back(getLLVMValue(cUnit, rlArray.origSReg));
  args.push_back(getLLVMValue(cUnit, rlIndex.origSReg));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(intr, args);
}

void convertIget(CompilationUnit* cUnit, int optFlags,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlDest, RegLocation rlObj, int fieldIndex)
{
  llvm::SmallVector<llvm::Value*, 3> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(getLLVMValue(cUnit, rlObj.origSReg));
  args.push_back(cUnit->irb->getInt32(fieldIndex));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertIput(CompilationUnit* cUnit, int optFlags,
                 greenland::IntrinsicHelper::IntrinsicId id,
                 RegLocation rlSrc, RegLocation rlObj, int fieldIndex)
{
  llvm::SmallVector<llvm::Value*, 4> args;
  args.push_back(cUnit->irb->getInt32(optFlags));
  args.push_back(getLLVMValue(cUnit, rlSrc.origSReg));
  args.push_back(getLLVMValue(cUnit, rlObj.origSReg));
  args.push_back(cUnit->irb->getInt32(fieldIndex));
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  cUnit->irb->CreateCall(intr, args);
}

void convertInstanceOf(CompilationUnit* cUnit, uint32_t type_idx,
                       RegLocation rlDest, RegLocation rlSrc)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::InstanceOf;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(cUnit->irb->getInt32(type_idx));
  args.push_back(getLLVMValue(cUnit, rlSrc.origSReg));
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertIntToLong(CompilationUnit* cUnit, RegLocation rlDest,
                      RegLocation rlSrc)
{
  llvm::Value* res = cUnit->irb->CreateSExt(getLLVMValue(cUnit, rlSrc.origSReg),
                                            cUnit->irb->getInt64Ty());
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertLongToInt(CompilationUnit* cUnit, RegLocation rlDest,
                      RegLocation rlSrc)
{
  llvm::Value* src = getLLVMValue(cUnit, rlSrc.origSReg);
  llvm::Value* res = cUnit->irb->CreateTrunc(src, cUnit->irb->getInt32Ty());
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertFloatToDouble(CompilationUnit* cUnit, RegLocation rlDest,
                          RegLocation rlSrc)
{
  llvm::Value* src = getLLVMValue(cUnit, rlSrc.origSReg);
  llvm::Value* res = cUnit->irb->CreateFPExt(src, cUnit->irb->getDoubleTy());
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertDoubleToFloat(CompilationUnit* cUnit, RegLocation rlDest,
                          RegLocation rlSrc)
{
  llvm::Value* src = getLLVMValue(cUnit, rlSrc.origSReg);
  llvm::Value* res = cUnit->irb->CreateFPTrunc(src, cUnit->irb->getFloatTy());
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertWideComparison(CompilationUnit* cUnit,
                           greenland::IntrinsicHelper::IntrinsicId id,
                           RegLocation rlDest, RegLocation rlSrc1,
                           RegLocation rlSrc2)
{
  DCHECK_EQ(rlSrc1.fp, rlSrc2.fp);
  DCHECK_EQ(rlSrc1.wide, rlSrc2.wide);
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::SmallVector<llvm::Value*, 2> args;
  args.push_back(getLLVMValue(cUnit, rlSrc1.origSReg));
  args.push_back(getLLVMValue(cUnit, rlSrc2.origSReg));
  llvm::Value* res = cUnit->irb->CreateCall(intr, args);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertIntNarrowing(CompilationUnit* cUnit, RegLocation rlDest,
                         RegLocation rlSrc,
                         greenland::IntrinsicHelper::IntrinsicId id)
{
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* res =
      cUnit->irb->CreateCall(intr, getLLVMValue(cUnit, rlSrc.origSReg));
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertNeg(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  llvm::Value* res = cUnit->irb->CreateNeg(getLLVMValue(cUnit, rlSrc.origSReg));
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertIntToFP(CompilationUnit* cUnit, llvm::Type* ty, RegLocation rlDest,
                    RegLocation rlSrc)
{
  llvm::Value* res =
      cUnit->irb->CreateSIToFP(getLLVMValue(cUnit, rlSrc.origSReg), ty);
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertFPToInt(CompilationUnit* cUnit, llvm::Type* ty, RegLocation rlDest,
                    RegLocation rlSrc)
{
  llvm::Value* res =
      cUnit->irb->CreateFPToSI(getLLVMValue(cUnit, rlSrc.origSReg), ty);
  defineValue(cUnit, res, rlDest.origSReg);
}


void convertNegFP(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  llvm::Value* res =
      cUnit->irb->CreateFNeg(getLLVMValue(cUnit, rlSrc.origSReg));
  defineValue(cUnit, res, rlDest.origSReg);
}

void convertNot(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  llvm::Value* src = getLLVMValue(cUnit, rlSrc.origSReg);
  llvm::Value* res = cUnit->irb->CreateXor(src, static_cast<uint64_t>(-1));
  defineValue(cUnit, res, rlDest.origSReg);
}

/*
 * Target-independent code generation.  Use only high-level
 * load/store utilities here, or target-dependent genXX() handlers
 * when necessary.
 */
bool convertMIRNode(CompilationUnit* cUnit, MIR* mir, BasicBlock* bb,
                    llvm::BasicBlock* llvmBB, LIR* labelList)
{
  bool res = false;   // Assume success
  RegLocation rlSrc[3];
  RegLocation rlDest = badLoc;
  Instruction::Code opcode = mir->dalvikInsn.opcode;
  uint32_t vA = mir->dalvikInsn.vA;
  uint32_t vB = mir->dalvikInsn.vB;
  uint32_t vC = mir->dalvikInsn.vC;
  int optFlags = mir->optimizationFlags;

  bool objectDefinition = false;

  if (cUnit->printMe) {
    if ((int)opcode < kMirOpFirst) {
      LOG(INFO) << ".. " << Instruction::Name(opcode) << " 0x"
                << std::hex << (int)opcode;
    } else {
      LOG(INFO) << ".. opcode 0x" << std::hex << (int)opcode;
    }
  }

  /* Prep Src and Dest locations */
  int nextSreg = 0;
  int nextLoc = 0;
  int attrs = oatDataFlowAttributes[opcode];
  rlSrc[0] = rlSrc[1] = rlSrc[2] = badLoc;
  if (attrs & DF_UA) {
    if (attrs & DF_A_WIDE) {
      rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg);
      nextSreg+= 2;
    } else {
      rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
      nextSreg++;
    }
  }
  if (attrs & DF_UB) {
    if (attrs & DF_B_WIDE) {
      rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg);
      nextSreg+= 2;
    } else {
      rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
      nextSreg++;
    }
  }
  if (attrs & DF_UC) {
    if (attrs & DF_C_WIDE) {
      rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg);
    } else {
      rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
    }
  }
  if (attrs & DF_DA) {
    if (attrs & DF_A_WIDE) {
      rlDest = oatGetDestWide(cUnit, mir);
    } else {
      rlDest = oatGetDest(cUnit, mir);
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
        llvm::Value* src = getLLVMValue(cUnit, rlSrc[0].origSReg);
        llvm::Value* res = emitCopy(cUnit, src, rlDest);
        defineValue(cUnit, res, rlDest.origSReg);
      }
      break;

    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16: {
        llvm::Constant* immValue = cUnit->irb->GetJInt(vB);
        llvm::Value* res = emitConst(cUnit, immValue, rlDest);
        defineValue(cUnit, res, rlDest.origSReg);
      }
      break;

    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32: {
        // Sign extend to 64 bits
        int64_t imm = static_cast<int32_t>(vB);
        llvm::Constant* immValue = cUnit->irb->GetJLong(imm);
        llvm::Value* res = emitConst(cUnit, immValue, rlDest);
        defineValue(cUnit, res, rlDest.origSReg);
      }
      break;

    case Instruction::CONST_HIGH16: {
        llvm::Constant* immValue = cUnit->irb->GetJInt(vB << 16);
        llvm::Value* res = emitConst(cUnit, immValue, rlDest);
        defineValue(cUnit, res, rlDest.origSReg);
      }
      break;

    case Instruction::CONST_WIDE: {
        llvm::Constant* immValue =
            cUnit->irb->GetJLong(mir->dalvikInsn.vB_wide);
        llvm::Value* res = emitConst(cUnit, immValue, rlDest);
        defineValue(cUnit, res, rlDest.origSReg);
      }
      break;
    case Instruction::CONST_WIDE_HIGH16: {
        int64_t imm = static_cast<int64_t>(vB) << 48;
        llvm::Constant* immValue = cUnit->irb->GetJLong(imm);
        llvm::Value* res = emitConst(cUnit, immValue, rlDest);
        defineValue(cUnit, res, rlDest.origSReg);
      }
      break;

    case Instruction::SPUT_OBJECT:
      convertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputObject,
                  rlSrc[0]);
      break;
    case Instruction::SPUT:
      if (rlSrc[0].fp) {
        convertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputFloat,
                    rlSrc[0]);
      } else {
        convertSput(cUnit, vB, greenland::IntrinsicHelper::HLSput, rlSrc[0]);
      }
      break;
    case Instruction::SPUT_BOOLEAN:
      convertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputBoolean,
                  rlSrc[0]);
      break;
    case Instruction::SPUT_BYTE:
      convertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputByte, rlSrc[0]);
      break;
    case Instruction::SPUT_CHAR:
      convertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputChar, rlSrc[0]);
      break;
    case Instruction::SPUT_SHORT:
      convertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputShort, rlSrc[0]);
      break;
    case Instruction::SPUT_WIDE:
      if (rlSrc[0].fp) {
        convertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputDouble,
                    rlSrc[0]);
      } else {
        convertSput(cUnit, vB, greenland::IntrinsicHelper::HLSputWide,
                    rlSrc[0]);
      }
      break;

    case Instruction::SGET_OBJECT:
      convertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetObject, rlDest);
      break;
    case Instruction::SGET:
      if (rlDest.fp) {
        convertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetFloat, rlDest);
      } else {
        convertSget(cUnit, vB, greenland::IntrinsicHelper::HLSget, rlDest);
      }
      break;
    case Instruction::SGET_BOOLEAN:
      convertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetBoolean, rlDest);
      break;
    case Instruction::SGET_BYTE:
      convertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetByte, rlDest);
      break;
    case Instruction::SGET_CHAR:
      convertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetChar, rlDest);
      break;
    case Instruction::SGET_SHORT:
      convertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetShort, rlDest);
      break;
    case Instruction::SGET_WIDE:
      if (rlDest.fp) {
        convertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetDouble,
                    rlDest);
      } else {
        convertSget(cUnit, vB, greenland::IntrinsicHelper::HLSgetWide, rlDest);
      }
      break;

    case Instruction::RETURN_WIDE:
    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT: {
        if (!(cUnit->attrs & METHOD_IS_LEAF)) {
          emitSuspendCheck(cUnit);
        }
        emitPopShadowFrame(cUnit);
        cUnit->irb->CreateRet(getLLVMValue(cUnit, rlSrc[0].origSReg));
        bb->hasReturn = true;
      }
      break;

    case Instruction::RETURN_VOID: {
        if (!(cUnit->attrs & METHOD_IS_LEAF)) {
          emitSuspendCheck(cUnit);
        }
        emitPopShadowFrame(cUnit);
        cUnit->irb->CreateRetVoid();
        bb->hasReturn = true;
      }
      break;

    case Instruction::IF_EQ:
      convertCompareAndBranch(cUnit, bb, mir, kCondEq, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_NE:
      convertCompareAndBranch(cUnit, bb, mir, kCondNe, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_LT:
      convertCompareAndBranch(cUnit, bb, mir, kCondLt, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_GE:
      convertCompareAndBranch(cUnit, bb, mir, kCondGe, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_GT:
      convertCompareAndBranch(cUnit, bb, mir, kCondGt, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_LE:
      convertCompareAndBranch(cUnit, bb, mir, kCondLe, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::IF_EQZ:
      convertCompareZeroAndBranch(cUnit, bb, mir, kCondEq, rlSrc[0]);
      break;
    case Instruction::IF_NEZ:
      convertCompareZeroAndBranch(cUnit, bb, mir, kCondNe, rlSrc[0]);
      break;
    case Instruction::IF_LTZ:
      convertCompareZeroAndBranch(cUnit, bb, mir, kCondLt, rlSrc[0]);
      break;
    case Instruction::IF_GEZ:
      convertCompareZeroAndBranch(cUnit, bb, mir, kCondGe, rlSrc[0]);
      break;
    case Instruction::IF_GTZ:
      convertCompareZeroAndBranch(cUnit, bb, mir, kCondGt, rlSrc[0]);
      break;
    case Instruction::IF_LEZ:
      convertCompareZeroAndBranch(cUnit, bb, mir, kCondLe, rlSrc[0]);
      break;

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32: {
        if (bb->taken->startOffset <= bb->startOffset) {
          emitSuspendCheck(cUnit);
        }
        cUnit->irb->CreateBr(getLLVMBlock(cUnit, bb->taken->id));
      }
      break;

    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
      convertArithOp(cUnit, kOpAdd, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      convertArithOp(cUnit, kOpSub, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::MUL_LONG:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
      convertArithOp(cUnit, kOpMul, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::DIV_LONG:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
      convertArithOp(cUnit, kOpDiv, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::REM_LONG:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
      convertArithOp(cUnit, kOpRem, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AND_LONG:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
      convertArithOp(cUnit, kOpAnd, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
      convertArithOp(cUnit, kOpOr, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
      convertArithOp(cUnit, kOpXor, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      convertShift(cUnit, greenland::IntrinsicHelper::SHLLong,
                    rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      convertShift(cUnit, greenland::IntrinsicHelper::SHLInt,
                   rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      convertShift(cUnit, greenland::IntrinsicHelper::SHRLong,
                   rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      convertShift(cUnit, greenland::IntrinsicHelper::SHRInt,
                   rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      convertShift(cUnit, greenland::IntrinsicHelper::USHRLong,
                   rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      convertShift(cUnit, greenland::IntrinsicHelper::USHRInt,
                   rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::ADD_INT_LIT16:
    case Instruction::ADD_INT_LIT8:
      convertArithOpLit(cUnit, kOpAdd, rlDest, rlSrc[0], vC);
      break;
    case Instruction::RSUB_INT:
    case Instruction::RSUB_INT_LIT8:
      convertArithOpLit(cUnit, kOpRsub, rlDest, rlSrc[0], vC);
      break;
    case Instruction::MUL_INT_LIT16:
    case Instruction::MUL_INT_LIT8:
      convertArithOpLit(cUnit, kOpMul, rlDest, rlSrc[0], vC);
      break;
    case Instruction::DIV_INT_LIT16:
    case Instruction::DIV_INT_LIT8:
      convertArithOpLit(cUnit, kOpDiv, rlDest, rlSrc[0], vC);
      break;
    case Instruction::REM_INT_LIT16:
    case Instruction::REM_INT_LIT8:
      convertArithOpLit(cUnit, kOpRem, rlDest, rlSrc[0], vC);
      break;
    case Instruction::AND_INT_LIT16:
    case Instruction::AND_INT_LIT8:
      convertArithOpLit(cUnit, kOpAnd, rlDest, rlSrc[0], vC);
      break;
    case Instruction::OR_INT_LIT16:
    case Instruction::OR_INT_LIT8:
      convertArithOpLit(cUnit, kOpOr, rlDest, rlSrc[0], vC);
      break;
    case Instruction::XOR_INT_LIT16:
    case Instruction::XOR_INT_LIT8:
      convertArithOpLit(cUnit, kOpXor, rlDest, rlSrc[0], vC);
      break;
    case Instruction::SHL_INT_LIT8:
      convertShiftLit(cUnit, greenland::IntrinsicHelper::SHLInt,
                      rlDest, rlSrc[0], vC & 0x1f);
      break;
    case Instruction::SHR_INT_LIT8:
      convertShiftLit(cUnit, greenland::IntrinsicHelper::SHRInt,
                      rlDest, rlSrc[0], vC & 0x1f);
      break;
    case Instruction::USHR_INT_LIT8:
      convertShiftLit(cUnit, greenland::IntrinsicHelper::USHRInt,
                      rlDest, rlSrc[0], vC & 0x1f);
      break;

    case Instruction::ADD_FLOAT:
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::ADD_DOUBLE:
    case Instruction::ADD_DOUBLE_2ADDR:
      convertFPArithOp(cUnit, kOpAdd, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::SUB_FLOAT:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::SUB_DOUBLE:
    case Instruction::SUB_DOUBLE_2ADDR:
      convertFPArithOp(cUnit, kOpSub, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::MUL_FLOAT:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::MUL_DOUBLE:
    case Instruction::MUL_DOUBLE_2ADDR:
      convertFPArithOp(cUnit, kOpMul, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::DIV_FLOAT:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::DIV_DOUBLE:
    case Instruction::DIV_DOUBLE_2ADDR:
      convertFPArithOp(cUnit, kOpDiv, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::REM_FLOAT:
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::REM_DOUBLE:
    case Instruction::REM_DOUBLE_2ADDR:
      convertFPArithOp(cUnit, kOpRem, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::INVOKE_STATIC:
      convertInvoke(cUnit, bb, mir, kStatic, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_STATIC_RANGE:
      convertInvoke(cUnit, bb, mir, kStatic, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_DIRECT:
      convertInvoke(cUnit, bb,  mir, kDirect, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_DIRECT_RANGE:
      convertInvoke(cUnit, bb, mir, kDirect, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_VIRTUAL:
      convertInvoke(cUnit, bb, mir, kVirtual, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_VIRTUAL_RANGE:
      convertInvoke(cUnit, bb, mir, kVirtual, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_SUPER:
      convertInvoke(cUnit, bb, mir, kSuper, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_SUPER_RANGE:
      convertInvoke(cUnit, bb, mir, kSuper, true /*range*/,
                    false /* NewFilledArray */);
      break;

    case Instruction::INVOKE_INTERFACE:
      convertInvoke(cUnit, bb, mir, kInterface, false /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::INVOKE_INTERFACE_RANGE:
      convertInvoke(cUnit, bb, mir, kInterface, true /*range*/,
                    false /* NewFilledArray */);
      break;
    case Instruction::FILLED_NEW_ARRAY:
      convertInvoke(cUnit, bb, mir, kInterface, false /*range*/,
                    true /* NewFilledArray */);
      break;
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      convertInvoke(cUnit, bb, mir, kInterface, true /*range*/,
                    true /* NewFilledArray */);
      break;

    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      convertConstObject(cUnit, vB, greenland::IntrinsicHelper::ConstString,
                         rlDest);
      break;

    case Instruction::CONST_CLASS:
      convertConstObject(cUnit, vB, greenland::IntrinsicHelper::ConstClass,
                         rlDest);
      break;

    case Instruction::CHECK_CAST:
      convertCheckCast(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::NEW_INSTANCE:
      convertNewInstance(cUnit, vB, rlDest);
      break;

   case Instruction::MOVE_EXCEPTION:
      convertMoveException(cUnit, rlDest);
      break;

   case Instruction::THROW:
      convertThrow(cUnit, rlSrc[0]);
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

   case Instruction::THROW_VERIFICATION_ERROR:
      convertThrowVerificationError(cUnit, vA, vB);
      UNIMPLEMENTED(WARNING) << "Need dead code elimination pass"
                             << " - disabling bitcode verification";
      cUnit->enableDebug &= ~(1 << kDebugVerifyBitcode);
      break;

    case Instruction::MOVE_RESULT_WIDE:
    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_OBJECT:
#if defined(TARGET_ARM)
      /*
       * Instruction rewriting on verification failure can eliminate
       * the invoke that feeds this move0result.  It won't ever be reached,
       * so we can ignore it.
       * TODO: verify that previous instruction is THROW_VERIFICATION_ERROR,
       * or better, add dead-code elimination.
       */
      UNIMPLEMENTED(WARNING) << "Need to verify previous inst was rewritten";
#else
      UNIMPLEMENTED(WARNING) << "need x86 move-result fusing";
#endif

      break;

    case Instruction::MONITOR_ENTER:
      convertMonitorEnterExit(cUnit, optFlags,
                              greenland::IntrinsicHelper::MonitorEnter,
                              rlSrc[0]);
      break;

    case Instruction::MONITOR_EXIT:
      convertMonitorEnterExit(cUnit, optFlags,
                              greenland::IntrinsicHelper::MonitorExit,
                              rlSrc[0]);
      break;

    case Instruction::ARRAY_LENGTH:
      convertArrayLength(cUnit, optFlags, rlDest, rlSrc[0]);
      break;

    case Instruction::NEW_ARRAY:
      convertNewArray(cUnit, vC, rlDest, rlSrc[0]);
      break;

    case Instruction::INSTANCE_OF:
      convertInstanceOf(cUnit, vC, rlDest, rlSrc[0]);
      break;

    case Instruction::AGET:
      if (rlDest.fp) {
        convertAget(cUnit, optFlags,
                    greenland::IntrinsicHelper::HLArrayGetFloat,
                    rlDest, rlSrc[0], rlSrc[1]);
      } else {
        convertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGet,
                    rlDest, rlSrc[0], rlSrc[1]);
      }
      break;
    case Instruction::AGET_OBJECT:
      convertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGetObject,
                  rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AGET_BOOLEAN:
      convertAget(cUnit, optFlags,
                  greenland::IntrinsicHelper::HLArrayGetBoolean,
                  rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AGET_BYTE:
      convertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGetByte,
                  rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AGET_CHAR:
      convertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGetChar,
                  rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AGET_SHORT:
      convertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGetShort,
                  rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::AGET_WIDE:
      if (rlDest.fp) {
        convertAget(cUnit, optFlags,
                    greenland::IntrinsicHelper::HLArrayGetDouble,
                    rlDest, rlSrc[0], rlSrc[1]);
      } else {
        convertAget(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayGetWide,
                    rlDest, rlSrc[0], rlSrc[1]);
      }
      break;

    case Instruction::APUT:
      if (rlSrc[0].fp) {
        convertAput(cUnit, optFlags,
                    greenland::IntrinsicHelper::HLArrayPutFloat,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      } else {
        convertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPut,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      }
      break;
    case Instruction::APUT_OBJECT:
      convertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPutObject,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      break;
    case Instruction::APUT_BOOLEAN:
      convertAput(cUnit, optFlags,
                  greenland::IntrinsicHelper::HLArrayPutBoolean,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      break;
    case Instruction::APUT_BYTE:
      convertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPutByte,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      break;
    case Instruction::APUT_CHAR:
      convertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPutChar,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      break;
    case Instruction::APUT_SHORT:
      convertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPutShort,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      break;
    case Instruction::APUT_WIDE:
      if (rlSrc[0].fp) {
        convertAput(cUnit, optFlags,
                    greenland::IntrinsicHelper::HLArrayPutDouble,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      } else {
        convertAput(cUnit, optFlags, greenland::IntrinsicHelper::HLArrayPutWide,
                    rlSrc[0], rlSrc[1], rlSrc[2]);
      }
      break;

    case Instruction::IGET:
      if (rlDest.fp) {
        convertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetFloat,
                    rlDest, rlSrc[0], vC);
      } else {
        convertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGet,
                    rlDest, rlSrc[0], vC);
      }
      break;
    case Instruction::IGET_OBJECT:
      convertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetObject,
                  rlDest, rlSrc[0], vC);
      break;
    case Instruction::IGET_BOOLEAN:
      convertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetBoolean,
                  rlDest, rlSrc[0], vC);
      break;
    case Instruction::IGET_BYTE:
      convertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetByte,
                  rlDest, rlSrc[0], vC);
      break;
    case Instruction::IGET_CHAR:
      convertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetChar,
                  rlDest, rlSrc[0], vC);
      break;
    case Instruction::IGET_SHORT:
      convertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetShort,
                  rlDest, rlSrc[0], vC);
      break;
    case Instruction::IGET_WIDE:
      if (rlDest.fp) {
        convertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetDouble,
                    rlDest, rlSrc[0], vC);
      } else {
        convertIget(cUnit, optFlags, greenland::IntrinsicHelper::HLIGetWide,
                    rlDest, rlSrc[0], vC);
      }
      break;
    case Instruction::IPUT:
      if (rlSrc[0].fp) {
        convertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutFloat,
                    rlSrc[0], rlSrc[1], vC);
      } else {
        convertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPut,
                    rlSrc[0], rlSrc[1], vC);
      }
      break;
    case Instruction::IPUT_OBJECT:
      convertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutObject,
                  rlSrc[0], rlSrc[1], vC);
      break;
    case Instruction::IPUT_BOOLEAN:
      convertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutBoolean,
                  rlSrc[0], rlSrc[1], vC);
      break;
    case Instruction::IPUT_BYTE:
      convertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutByte,
                  rlSrc[0], rlSrc[1], vC);
      break;
    case Instruction::IPUT_CHAR:
      convertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutChar,
                  rlSrc[0], rlSrc[1], vC);
      break;
    case Instruction::IPUT_SHORT:
      convertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutShort,
                  rlSrc[0], rlSrc[1], vC);
      break;
    case Instruction::IPUT_WIDE:
      if (rlSrc[0].fp) {
        convertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutDouble,
                    rlSrc[0], rlSrc[1], vC);
      } else {
        convertIput(cUnit, optFlags, greenland::IntrinsicHelper::HLIPutWide,
                    rlSrc[0], rlSrc[1], vC);
      }
      break;

    case Instruction::FILL_ARRAY_DATA:
      convertFillArrayData(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::LONG_TO_INT:
      convertLongToInt(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_LONG:
      convertIntToLong(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_CHAR:
      convertIntNarrowing(cUnit, rlDest, rlSrc[0],
                          greenland::IntrinsicHelper::IntToChar);
      break;
    case Instruction::INT_TO_BYTE:
      convertIntNarrowing(cUnit, rlDest, rlSrc[0],
                          greenland::IntrinsicHelper::IntToByte);
      break;
    case Instruction::INT_TO_SHORT:
      convertIntNarrowing(cUnit, rlDest, rlSrc[0],
                          greenland::IntrinsicHelper::IntToShort);
      break;

    case Instruction::INT_TO_FLOAT:
    case Instruction::LONG_TO_FLOAT:
      convertIntToFP(cUnit, cUnit->irb->getFloatTy(), rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_DOUBLE:
    case Instruction::LONG_TO_DOUBLE:
      convertIntToFP(cUnit, cUnit->irb->getDoubleTy(), rlDest, rlSrc[0]);
      break;

    case Instruction::FLOAT_TO_DOUBLE:
      convertFloatToDouble(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::DOUBLE_TO_FLOAT:
      convertDoubleToFloat(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::NEG_LONG:
    case Instruction::NEG_INT:
      convertNeg(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::NEG_FLOAT:
    case Instruction::NEG_DOUBLE:
      convertNegFP(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::NOT_LONG:
    case Instruction::NOT_INT:
      convertNot(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::FLOAT_TO_INT:
    case Instruction::DOUBLE_TO_INT:
      convertFPToInt(cUnit, cUnit->irb->getInt32Ty(), rlDest, rlSrc[0]);
      break;

    case Instruction::FLOAT_TO_LONG:
    case Instruction::DOUBLE_TO_LONG:
      convertFPToInt(cUnit, cUnit->irb->getInt64Ty(), rlDest, rlSrc[0]);
      break;

    case Instruction::CMPL_FLOAT:
      convertWideComparison(cUnit, greenland::IntrinsicHelper::CmplFloat,
                            rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::CMPG_FLOAT:
      convertWideComparison(cUnit, greenland::IntrinsicHelper::CmpgFloat,
                            rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::CMPL_DOUBLE:
      convertWideComparison(cUnit, greenland::IntrinsicHelper::CmplDouble,
                            rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::CMPG_DOUBLE:
      convertWideComparison(cUnit, greenland::IntrinsicHelper::CmpgDouble,
                            rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::CMP_LONG:
      convertWideComparison(cUnit, greenland::IntrinsicHelper::CmpLong,
                            rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::PACKED_SWITCH:
      convertPackedSwitch(cUnit, bb, vB, rlSrc[0]);
      break;

    case Instruction::SPARSE_SWITCH:
      convertSparseSwitch(cUnit, bb, vB, rlSrc[0]);
      break;

    default:
      UNIMPLEMENTED(FATAL) << "Unsupported Dex opcode 0x" << std::hex << opcode;
      res = true;
  }
  if (objectDefinition) {
    setShadowFrameEntry(cUnit, (llvm::Value*)
                        cUnit->llvmValues.elemList[rlDest.origSReg]);
  }
  return res;
}

/* Extended MIR instructions like PHI */
void convertExtendedMIR(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                        llvm::BasicBlock* llvmBB)
{

  switch ((ExtendedMIROpcode)mir->dalvikInsn.opcode) {
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
      int* incoming = (int*)mir->dalvikInsn.vB;
      llvm::Type* phiType =
          llvmTypeFromLocRec(cUnit, rlDest);
      llvm::PHINode* phi = cUnit->irb->CreatePHI(phiType, mir->ssaRep->numUses);
      for (int i = 0; i < mir->ssaRep->numUses; i++) {
        RegLocation loc;
        // Don't check width here.
        loc = oatGetRawSrc(cUnit, mir, i);
        DCHECK_EQ(rlDest.wide, loc.wide);
        DCHECK_EQ(rlDest.wide & rlDest.highWord, loc.wide & loc.highWord);
        DCHECK_EQ(rlDest.fp, loc.fp);
        DCHECK_EQ(rlDest.core, loc.core);
        DCHECK_EQ(rlDest.ref, loc.ref);
        phi->addIncoming(getLLVMValue(cUnit, loc.origSReg),
                         getLLVMBlock(cUnit, incoming[i]));
      }
      defineValue(cUnit, phi, rlDest.origSReg);
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

#if defined(TARGET_ARM)
    case kMirOpFusedCmplFloat:
      UNIMPLEMENTED(WARNING) << "unimp kMirOpFusedCmpFloat";
      break;
    case kMirOpFusedCmpgFloat:
      UNIMPLEMENTED(WARNING) << "unimp kMirOpFusedCmgFloat";
      break;
    case kMirOpFusedCmplDouble:
      UNIMPLEMENTED(WARNING) << "unimp kMirOpFusedCmplDouble";
      break;
    case kMirOpFusedCmpgDouble:
      UNIMPLEMENTED(WARNING) << "unimp kMirOpFusedCmpgDouble";
      break;
    case kMirOpFusedCmpLong:
      UNIMPLEMENTED(WARNING) << "unimp kMirOpLongCmpBranch";
      break;
#endif
    default:
      break;
  }
}

void setDexOffset(CompilationUnit* cUnit, int32_t offset)
{
  cUnit->currentDalvikOffset = offset;
  llvm::SmallVector<llvm::Value*, 1> arrayRef;
  arrayRef.push_back(cUnit->irb->getInt32(offset));
  llvm::MDNode* node = llvm::MDNode::get(*cUnit->context, arrayRef);
  cUnit->irb->SetDexOffset(node);
}

// Attach method info as metadata to special intrinsic
void setMethodInfo(CompilationUnit* cUnit)
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
                      ((p->fpReg & 0xff) << 16) |
                      ((p->coreReg & 0xff) << 8) |
                      ((p->fpLocation & 0xf) << 4) |
                      (p->coreLocation & 0xf);
    pmap.push_back(cUnit->irb->getInt32(mapData));
  }
  llvm::MDNode* mapNode = llvm::MDNode::get(*cUnit->context, pmap);
  inst->setMetadata("PromotionMap", mapNode);
  setDexOffset(cUnit, cUnit->currentDalvikOffset);
}

/* Handle the content in each basic block */
bool methodBlockBitcodeConversion(CompilationUnit* cUnit, BasicBlock* bb)
{
  llvm::BasicBlock* llvmBB = getLLVMBlock(cUnit, bb->id);
  cUnit->irb->SetInsertPoint(llvmBB);
  setDexOffset(cUnit, bb->startOffset);

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
    setMethodInfo(cUnit);
    bool *canBeRef = (bool*)  oatNew(cUnit, sizeof(bool) *
                                     cUnit->numDalvikRegisters, true,
                                     kAllocMisc);
    for (int i = 0; i < cUnit->numSSARegs; i++) {
      canBeRef[SRegToVReg(cUnit, i)] |= cUnit->regLocation[i].ref;
    }
    for (int i = 0; i < cUnit->numDalvikRegisters; i++) {
      if (canBeRef[i]) {
        cUnit->numShadowFrameEntries++;
      }
    }
    if (cUnit->numShadowFrameEntries > 0) {
      cUnit->shadowMap = (int*) oatNew(cUnit, sizeof(int) *
                                       cUnit->numShadowFrameEntries, true,
                                       kAllocMisc);
      for (int i = 0, j = 0; i < cUnit->numDalvikRegisters; i++) {
        if (canBeRef[i]) {
          cUnit->shadowMap[j++] = i;
        }
      }
      greenland::IntrinsicHelper::IntrinsicId id =
              greenland::IntrinsicHelper::AllocaShadowFrame;
      llvm::Function* func = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
      llvm::Value* entries = cUnit->irb->getInt32(cUnit->numShadowFrameEntries);
      cUnit->irb->CreateCall(func, entries);
    }
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

    setDexOffset(cUnit, mir->offset);

    int opcode = mir->dalvikInsn.opcode;
    Instruction::Format dalvikFormat =
        Instruction::FormatOf(mir->dalvikInsn.opcode);

    /* If we're compiling for the debugger, generate an update callout */
    if (cUnit->genDebugger) {
      UNIMPLEMENTED(FATAL) << "Need debug codegen";
      //genDebuggerUpdate(cUnit, mir->offset);
    }

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
        oatGrowableListIteratorInit(&bb->successorBlockList.blocks, &iter);
        // New basic block to use for work half
        llvm::BasicBlock* workBB =
            llvm::BasicBlock::Create(*cUnit->context, "", cUnit->func);
        llvm::SwitchInst* sw =
            cUnit->irb->CreateSwitch(switchKey, workBB,
                                     bb->successorBlockList.blocks.numUsed);
        while (true) {
          SuccessorBlockInfo *successorBlockInfo =
              (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iter);
          if (successorBlockInfo == NULL) break;
          llvm::BasicBlock *target =
              getLLVMBlock(cUnit, successorBlockInfo->block->id);
          int typeIndex = successorBlockInfo->key;
          sw->addCase(cUnit->irb->getInt32(typeIndex), target);
        }
        llvmBB = workBB;
        cUnit->irb->SetInsertPoint(llvmBB);
      }
    }

    if (opcode >= kMirOpFirst) {
      convertExtendedMIR(cUnit, bb, mir, llvmBB);
      continue;
    }

    bool notHandled = convertMIRNode(cUnit, mir, bb, llvmBB,
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
    cUnit->entryTargetBB = getLLVMBlock(cUnit, bb->fallThrough->id);
  } else if ((bb->fallThrough != NULL) && !bb->hasReturn) {
    cUnit->irb->CreateBr(getLLVMBlock(cUnit, bb->fallThrough->id));
  }

  return false;
}

char remapShorty(char shortyType) {
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

llvm::FunctionType* getFunctionType(CompilationUnit* cUnit) {

  // Get return type
  llvm::Type* ret_type = cUnit->irb->GetJType(remapShorty(cUnit->shorty[0]),
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
    args_type.push_back(cUnit->irb->GetJType(remapShorty(cUnit->shorty[i]),
                                             greenland::kAccurate));
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}

bool createFunction(CompilationUnit* cUnit) {
  std::string func_name(PrettyMethod(cUnit->method_idx, *cUnit->dex_file,
                                     /* with_signature */ false));
  llvm::FunctionType* func_type = getFunctionType(cUnit);

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

bool createLLVMBasicBlock(CompilationUnit* cUnit, BasicBlock* bb)
{
  // Skip the exit block
  if (bb->blockType == kExitBlock) {
    cUnit->idToBlockMap.Put(bb->id, NULL);
  } else {
    int offset = bb->startOffset;
    bool entryBlock = (bb->blockType == kEntryBlock);
    llvm::BasicBlock* llvmBB =
        llvm::BasicBlock::Create(*cUnit->context, entryBlock ? "entry" :
                                 StringPrintf(kLabelFormat, offset, bb->id),
                                 cUnit->func);
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
void oatMethodMIR2Bitcode(CompilationUnit* cUnit)
{
  initIR(cUnit);
  oatInitGrowableList(cUnit, &cUnit->llvmValues, cUnit->numSSARegs);

  // Create the function
  createFunction(cUnit);

  // Create an LLVM basic block for each MIR block in dfs preorder
  oatDataFlowAnalysisDispatcher(cUnit, createLLVMBasicBlock,
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
      oatInsertGrowableList(cUnit, &cUnit->llvmValues, 0);
    } else if ((i < cUnit->numRegs) ||
               (i >= (cUnit->numRegs + cUnit->numIns))) {
      llvm::Constant* immValue = cUnit->regLocation[i].wide ?
         cUnit->irb->GetJLong(0) : cUnit->irb->GetJInt(0);
      val = emitConst(cUnit, immValue, cUnit->regLocation[i]);
      val->setName(llvmSSAName(cUnit, i));
      oatInsertGrowableList(cUnit, &cUnit->llvmValues, (intptr_t)val);
    } else {
      // Recover previously-created argument values
      llvm::Value* argVal = arg_iter++;
      oatInsertGrowableList(cUnit, &cUnit->llvmValues, (intptr_t)argVal);
    }
  }

  oatDataFlowAnalysisDispatcher(cUnit, methodBlockBitcodeConversion,
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
  setDexOffset(cUnit, 0);
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
    oatReplaceSpecialChars(fname);
    // TODO: make configurable
    fname = StringPrintf("/sdcard/Bitcode/%s.bc", fname.c_str());

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

RegLocation getLoc(CompilationUnit* cUnit, llvm::Value* val) {
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
      res.lowReg = oatAllocTemp(cUnit);
      res.home = true;
      res.sRegLow = INVALID_SREG;
      res.origSReg = INVALID_SREG;
      llvm::Type* ty = val->getType();
      res.wide = ((ty == cUnit->irb->getInt64Ty()) ||
                  (ty == cUnit->irb->getDoubleTy()));
      if (res.wide) {
        res.highReg = oatAllocTemp(cUnit);
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

Instruction::Code getDalvikOpcode(OpKind op, bool isConst, bool isWide)
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

Instruction::Code getDalvikFPOpcode(OpKind op, bool isConst, bool isWide)
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

void cvtBinFPOp(CompilationUnit* cUnit, OpKind op, llvm::Instruction* inst)
{
  RegLocation rlDest = getLoc(cUnit, inst);
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
    RegLocation rlSrc = getLoc(cUnit, inst->getOperand(1));
    if (rlDest.wide) {
      genArithOpDouble(cUnit, Instruction::NEG_DOUBLE, rlDest, rlSrc, rlSrc);
    } else {
      genArithOpFloat(cUnit, Instruction::NEG_FLOAT, rlDest, rlSrc, rlSrc);
    }
  } else {
    DCHECK(op1C == NULL);
    RegLocation rlSrc1 = getLoc(cUnit, inst->getOperand(0));
    RegLocation rlSrc2 = getLoc(cUnit, inst->getOperand(1));
    Instruction::Code dalvikOp = getDalvikFPOpcode(op, false, rlDest.wide);
    if (rlDest.wide) {
      genArithOpDouble(cUnit, dalvikOp, rlDest, rlSrc1, rlSrc2);
    } else {
      genArithOpFloat(cUnit, dalvikOp, rlDest, rlSrc1, rlSrc2);
    }
  }
}

void cvtIntNarrowing(CompilationUnit* cUnit, llvm::Instruction* inst,
                     Instruction::Code opcode)
{
  RegLocation rlDest = getLoc(cUnit, inst);
  RegLocation rlSrc = getLoc(cUnit, inst->getOperand(0));
  genIntNarrowing(cUnit, opcode, rlDest, rlSrc);
}

void cvtIntToFP(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  RegLocation rlDest = getLoc(cUnit, inst);
  RegLocation rlSrc = getLoc(cUnit, inst->getOperand(0));
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
  genConversion(cUnit, opcode, rlDest, rlSrc);
}

void cvtFPToInt(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  RegLocation rlDest = getLoc(cUnit, inst);
  RegLocation rlSrc = getLoc(cUnit, inst->getOperand(0));
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
  genConversion(cUnit, opcode, rlDest, rlSrc);
}

void cvtFloatToDouble(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  RegLocation rlDest = getLoc(cUnit, inst);
  RegLocation rlSrc = getLoc(cUnit, inst->getOperand(0));
  genConversion(cUnit, Instruction::FLOAT_TO_DOUBLE, rlDest, rlSrc);
}

void cvtTrunc(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  RegLocation rlDest = getLoc(cUnit, inst);
  RegLocation rlSrc = getLoc(cUnit, inst->getOperand(0));
  rlSrc = oatUpdateLocWide(cUnit, rlSrc);
  rlSrc = oatWideToNarrow(cUnit, rlSrc);
  storeValue(cUnit, rlDest, rlSrc);
}

void cvtDoubleToFloat(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  RegLocation rlDest = getLoc(cUnit, inst);
  RegLocation rlSrc = getLoc(cUnit, inst->getOperand(0));
  genConversion(cUnit, Instruction::DOUBLE_TO_FLOAT, rlDest, rlSrc);
}


void cvtIntExt(CompilationUnit* cUnit, llvm::Instruction* inst, bool isSigned)
{
  // TODO: evaluate src/tgt types and add general support for more than int to long
  RegLocation rlDest = getLoc(cUnit, inst);
  RegLocation rlSrc = getLoc(cUnit, inst->getOperand(0));
  DCHECK(rlDest.wide);
  DCHECK(!rlSrc.wide);
  DCHECK(!rlDest.fp);
  DCHECK(!rlSrc.fp);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  if (rlSrc.location == kLocPhysReg) {
    opRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
  } else {
    loadValueDirect(cUnit, rlSrc, rlResult.lowReg);
  }
  if (isSigned) {
    opRegRegImm(cUnit, kOpAsr, rlResult.highReg, rlResult.lowReg, 31);
  } else {
    loadConstant(cUnit, rlResult.highReg, 0);
  }
  storeValueWide(cUnit, rlDest, rlResult);
}

void cvtBinOp(CompilationUnit* cUnit, OpKind op, llvm::Instruction* inst)
{
  RegLocation rlDest = getLoc(cUnit, inst);
  llvm::Value* lhs = inst->getOperand(0);
  // Special-case RSUB/NEG
  llvm::ConstantInt* lhsImm = llvm::dyn_cast<llvm::ConstantInt>(lhs);
  if ((op == kOpSub) && (lhsImm != NULL)) {
    RegLocation rlSrc1 = getLoc(cUnit, inst->getOperand(1));
    if (rlSrc1.wide) {
      DCHECK_EQ(lhsImm->getSExtValue(), 0);
      genArithOpLong(cUnit, Instruction::NEG_LONG, rlDest, rlSrc1, rlSrc1);
    } else {
      genArithOpIntLit(cUnit, Instruction::RSUB_INT, rlDest, rlSrc1,
                       lhsImm->getSExtValue());
    }
    return;
  }
  DCHECK(lhsImm == NULL);
  RegLocation rlSrc1 = getLoc(cUnit, inst->getOperand(0));
  llvm::Value* rhs = inst->getOperand(1);
  llvm::ConstantInt* constRhs = llvm::dyn_cast<llvm::ConstantInt>(rhs);
  if (!rlDest.wide && (constRhs != NULL)) {
    Instruction::Code dalvikOp = getDalvikOpcode(op, true, false);
    genArithOpIntLit(cUnit, dalvikOp, rlDest, rlSrc1, constRhs->getSExtValue());
  } else {
    Instruction::Code dalvikOp = getDalvikOpcode(op, false, rlDest.wide);
    RegLocation rlSrc2;
    if (constRhs != NULL) {
      // ir_builder converts NOT_LONG to xor src, -1.  Restore
      DCHECK_EQ(dalvikOp, Instruction::XOR_LONG);
      DCHECK_EQ(-1L, constRhs->getSExtValue());
      dalvikOp = Instruction::NOT_LONG;
      rlSrc2 = rlSrc1;
    } else {
      rlSrc2 = getLoc(cUnit, rhs);
    }
    if (rlDest.wide) {
      genArithOpLong(cUnit, dalvikOp, rlDest, rlSrc1, rlSrc2);
    } else {
      genArithOpInt(cUnit, dalvikOp, rlDest, rlSrc1, rlSrc2);
    }
  }
}

void cvtShiftOp(CompilationUnit* cUnit, Instruction::Code opcode,
                llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  RegLocation rlDest = getLoc(cUnit, callInst);
  RegLocation rlSrc = getLoc(cUnit, callInst->getArgOperand(0));
  llvm::Value* rhs = callInst->getArgOperand(1);
  if (llvm::ConstantInt* src2 = llvm::dyn_cast<llvm::ConstantInt>(rhs)) {
    DCHECK(!rlDest.wide);
    genArithOpIntLit(cUnit, opcode, rlDest, rlSrc, src2->getSExtValue());
  } else {
    RegLocation rlShift = getLoc(cUnit, rhs);
    if (callInst->getType() == cUnit->irb->getInt64Ty()) {
      genShiftOpLong(cUnit, opcode, rlDest, rlSrc, rlShift);
    } else {
      genArithOpInt(cUnit, opcode, rlDest, rlSrc, rlShift);
    }
  }
}

void cvtBr(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  llvm::BranchInst* brInst = llvm::dyn_cast<llvm::BranchInst>(inst);
  DCHECK(brInst != NULL);
  DCHECK(brInst->isUnconditional());  // May change - but this is all we use now
  llvm::BasicBlock* targetBB = brInst->getSuccessor(0);
  opUnconditionalBranch(cUnit, cUnit->blockToLabelMap.Get(targetBB));
}

void cvtPhi(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  // Nop - these have already been processed
}

void cvtRet(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  llvm::ReturnInst* retInst = llvm::dyn_cast<llvm::ReturnInst>(inst);
  llvm::Value* retVal = retInst->getReturnValue();
  if (retVal != NULL) {
    RegLocation rlSrc = getLoc(cUnit, retVal);
    if (rlSrc.wide) {
      storeValueWide(cUnit, oatGetReturnWide(cUnit, rlSrc.fp), rlSrc);
    } else {
      storeValue(cUnit, oatGetReturn(cUnit, rlSrc.fp), rlSrc);
    }
  }
  genExitSequence(cUnit);
}

ConditionCode getCond(llvm::ICmpInst::Predicate llvmCond)
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

void cvtICmp(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  // genCmpLong(cUnit, rlDest, rlSrc1, rlSrc2)
  UNIMPLEMENTED(FATAL);
}

void cvtICmpBr(CompilationUnit* cUnit, llvm::Instruction* inst,
               llvm::BranchInst* brInst)
{
  // Get targets
  llvm::BasicBlock* takenBB = brInst->getSuccessor(0);
  LIR* taken = cUnit->blockToLabelMap.Get(takenBB);
  llvm::BasicBlock* fallThroughBB = brInst->getSuccessor(1);
  LIR* fallThrough = cUnit->blockToLabelMap.Get(fallThroughBB);
  // Get comparison operands
  llvm::ICmpInst* iCmpInst = llvm::dyn_cast<llvm::ICmpInst>(inst);
  ConditionCode cond = getCond(iCmpInst->getPredicate());
  llvm::Value* lhs = iCmpInst->getOperand(0);
  // Not expecting a constant as 1st operand
  DCHECK(llvm::dyn_cast<llvm::ConstantInt>(lhs) == NULL);
  RegLocation rlSrc1 = getLoc(cUnit, inst->getOperand(0));
  rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
  llvm::Value* rhs = inst->getOperand(1);
#if defined(TARGET_MIPS)
  // Compare and branch in one shot
  (void)taken;
  (void)cond;
  (void)rhs;
  UNIMPLEMENTED(FATAL);
#else
  //Compare, then branch
  // TODO: handle fused CMP_LONG/IF_xxZ case
  if (llvm::ConstantInt* src2 = llvm::dyn_cast<llvm::ConstantInt>(rhs)) {
    opRegImm(cUnit, kOpCmp, rlSrc1.lowReg, src2->getSExtValue());
  } else if (llvm::dyn_cast<llvm::ConstantPointerNull>(rhs) != NULL) {
    opRegImm(cUnit, kOpCmp, rlSrc1.lowReg, 0);
  } else {
    RegLocation rlSrc2 = getLoc(cUnit, rhs);
    rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
    opRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
  }
  opCondBranch(cUnit, cond, taken);
#endif
  // Fallthrough
  opUnconditionalBranch(cUnit, fallThrough);
}

void cvtCall(CompilationUnit* cUnit, llvm::CallInst* callInst,
             llvm::Function* callee)
{
  UNIMPLEMENTED(FATAL);
}

void cvtCopy(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  RegLocation rlSrc = getLoc(cUnit, callInst->getArgOperand(0));
  RegLocation rlDest = getLoc(cUnit, callInst);
  DCHECK_EQ(rlSrc.wide, rlDest.wide);
  DCHECK_EQ(rlSrc.fp, rlDest.fp);
  if (rlSrc.wide) {
    storeValueWide(cUnit, rlDest, rlSrc);
  } else {
    storeValue(cUnit, rlDest, rlSrc);
  }
}

// Note: Immediate arg is a ConstantInt regardless of result type
void cvtConst(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  llvm::ConstantInt* src =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint64_t immval = src->getZExtValue();
  RegLocation rlDest = getLoc(cUnit, callInst);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
  if (rlDest.wide) {
    loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                          (immval) & 0xffffffff, (immval >> 32) & 0xffffffff);
    storeValueWide(cUnit, rlDest, rlResult);
  } else {
    loadConstantNoClobber(cUnit, rlResult.lowReg, immval & 0xffffffff);
    storeValue(cUnit, rlDest, rlResult);
  }
}

void cvtConstObject(CompilationUnit* cUnit, llvm::CallInst* callInst,
                    bool isString)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  llvm::ConstantInt* idxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t index = idxVal->getZExtValue();
  RegLocation rlDest = getLoc(cUnit, callInst);
  if (isString) {
    genConstString(cUnit, index, rlDest);
  } else {
    genConstClass(cUnit, index, rlDest);
  }
}

void cvtFillArrayData(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* offsetVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlSrc = getLoc(cUnit, callInst->getArgOperand(1));
  genFillArrayData(cUnit, offsetVal->getSExtValue(), rlSrc);
}

void cvtNewInstance(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  llvm::ConstantInt* typeIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t typeIdx = typeIdxVal->getZExtValue();
  RegLocation rlDest = getLoc(cUnit, callInst);
  genNewInstance(cUnit, typeIdx, rlDest);
}

void cvtNewArray(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* typeIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t typeIdx = typeIdxVal->getZExtValue();
  llvm::Value* len = callInst->getArgOperand(1);
  RegLocation rlLen = getLoc(cUnit, len);
  RegLocation rlDest = getLoc(cUnit, callInst);
  genNewArray(cUnit, typeIdx, rlDest, rlLen);
}

void cvtInstanceOf(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* typeIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t typeIdx = typeIdxVal->getZExtValue();
  llvm::Value* src = callInst->getArgOperand(1);
  RegLocation rlSrc = getLoc(cUnit, src);
  RegLocation rlDest = getLoc(cUnit, callInst);
  genInstanceof(cUnit, typeIdx, rlDest, rlSrc);
}

void cvtThrowVerificationError(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* info1 =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  llvm::ConstantInt* info2 =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(1));
  genThrowVerificationError(cUnit, info1->getZExtValue(), info2->getZExtValue());
}

void cvtThrow(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  llvm::Value* src = callInst->getArgOperand(0);
  RegLocation rlSrc = getLoc(cUnit, src);
  genThrow(cUnit, rlSrc);
}

void cvtMonitorEnterExit(CompilationUnit* cUnit, bool isEnter,
                         llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  llvm::Value* src = callInst->getArgOperand(1);
  RegLocation rlSrc = getLoc(cUnit, src);
  if (isEnter) {
    genMonitorEnter(cUnit, optFlags->getZExtValue(), rlSrc);
  } else {
    genMonitorExit(cUnit, optFlags->getZExtValue(), rlSrc);
  }
}

void cvtArrayLength(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  llvm::Value* src = callInst->getArgOperand(1);
  RegLocation rlSrc = getLoc(cUnit, src);
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  genNullCheck(cUnit, rlSrc.sRegLow, rlSrc.lowReg, optFlags->getZExtValue());
  RegLocation rlDest = getLoc(cUnit, callInst);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  int lenOffset = Array::LengthOffset().Int32Value();
  loadWordDisp(cUnit, rlSrc.lowReg, lenOffset, rlResult.lowReg);
  storeValue(cUnit, rlDest, rlResult);
}

void cvtMoveException(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 0U);
  int exOffset = Thread::ExceptionOffset().Int32Value();
  RegLocation rlDest = getLoc(cUnit, callInst);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
#if defined(TARGET_X86)
  newLIR2(cUnit, kX86Mov32RT, rlResult.lowReg, exOffset);
  newLIR2(cUnit, kX86Mov32TI, exOffset, 0);
#else
  int resetReg = oatAllocTemp(cUnit);
  loadWordDisp(cUnit, rSELF, exOffset, rlResult.lowReg);
  loadConstant(cUnit, resetReg, 0);
  storeWordDisp(cUnit, rSELF, exOffset, resetReg);
  oatFreeTemp(cUnit, resetReg);
#endif
  storeValue(cUnit, rlDest, rlResult);
}

void cvtSget(CompilationUnit* cUnit, llvm::CallInst* callInst, bool isWide,
             bool isObject)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1U);
  llvm::ConstantInt* typeIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t typeIdx = typeIdxVal->getZExtValue();
  RegLocation rlDest = getLoc(cUnit, callInst);
  genSget(cUnit, typeIdx, rlDest, isWide, isObject);
}

void cvtSput(CompilationUnit* cUnit, llvm::CallInst* callInst, bool isWide,
             bool isObject)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* typeIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t typeIdx = typeIdxVal->getZExtValue();
  llvm::Value* src = callInst->getArgOperand(1);
  RegLocation rlSrc = getLoc(cUnit, src);
  genSput(cUnit, typeIdx, rlSrc, isWide, isObject);
}

void cvtAget(CompilationUnit* cUnit, llvm::CallInst* callInst, OpSize size,
             int scale)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 3U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlArray = getLoc(cUnit, callInst->getArgOperand(1));
  RegLocation rlIndex = getLoc(cUnit, callInst->getArgOperand(2));
  RegLocation rlDest = getLoc(cUnit, callInst);
  genArrayGet(cUnit, optFlags->getZExtValue(), size, rlArray, rlIndex,
              rlDest, scale);
}

void cvtAput(CompilationUnit* cUnit, llvm::CallInst* callInst, OpSize size,
             int scale, bool isObject)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 4U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlSrc = getLoc(cUnit, callInst->getArgOperand(1));
  RegLocation rlArray = getLoc(cUnit, callInst->getArgOperand(2));
  RegLocation rlIndex = getLoc(cUnit, callInst->getArgOperand(3));
  if (isObject) {
    genArrayObjPut(cUnit, optFlags->getZExtValue(), rlArray, rlIndex,
                   rlSrc, scale);
  } else {
    genArrayPut(cUnit, optFlags->getZExtValue(), size, rlArray, rlIndex,
                rlSrc, scale);
  }
}

void cvtAputObj(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  cvtAput(cUnit, callInst, kWord, 2, true /* isObject */);
}

void cvtAputPrimitive(CompilationUnit* cUnit, llvm::CallInst* callInst,
                      OpSize size, int scale)
{
  cvtAput(cUnit, callInst, size, scale, false /* isObject */);
}

void cvtIget(CompilationUnit* cUnit, llvm::CallInst* callInst, OpSize size,
             bool isWide, bool isObj)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 3U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlObj = getLoc(cUnit, callInst->getArgOperand(1));
  llvm::ConstantInt* fieldIdx =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(2));
  RegLocation rlDest = getLoc(cUnit, callInst);
  genIGet(cUnit, fieldIdx->getZExtValue(), optFlags->getZExtValue(),
          size, rlDest, rlObj, isWide, isObj);
}

void cvtIput(CompilationUnit* cUnit, llvm::CallInst* callInst, OpSize size,
             bool isWide, bool isObj)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 4U);
  llvm::ConstantInt* optFlags =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlSrc = getLoc(cUnit, callInst->getArgOperand(1));
  RegLocation rlObj = getLoc(cUnit, callInst->getArgOperand(2));
  llvm::ConstantInt* fieldIdx =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(3));
  genIPut(cUnit, fieldIdx->getZExtValue(), optFlags->getZExtValue(),
          size, rlSrc, rlObj, isWide, isObj);
}

void cvtCheckCast(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 2U);
  llvm::ConstantInt* typeIdx =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  RegLocation rlSrc = getLoc(cUnit, callInst->getArgOperand(1));
  genCheckCast(cUnit, typeIdx->getZExtValue(), rlSrc);
}

void cvtFPCompare(CompilationUnit* cUnit, llvm::CallInst* callInst,
                  Instruction::Code opcode)
{
  RegLocation rlSrc1 = getLoc(cUnit, callInst->getArgOperand(0));
  RegLocation rlSrc2 = getLoc(cUnit, callInst->getArgOperand(1));
  RegLocation rlDest = getLoc(cUnit, callInst);
  genCmpFP(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
}

void cvtLongCompare(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  RegLocation rlSrc1 = getLoc(cUnit, callInst->getArgOperand(0));
  RegLocation rlSrc2 = getLoc(cUnit, callInst->getArgOperand(1));
  RegLocation rlDest = getLoc(cUnit, callInst);
  genCmpLong(cUnit, rlDest, rlSrc1, rlSrc2);
}

void cvtSwitch(CompilationUnit* cUnit, llvm::Instruction* inst)
{
  llvm::SwitchInst* swInst = llvm::dyn_cast<llvm::SwitchInst>(inst);
  DCHECK(swInst != NULL);
  llvm::Value* testVal = swInst->getCondition();
  llvm::MDNode* tableOffsetNode = swInst->getMetadata("SwitchTable");
  DCHECK(tableOffsetNode != NULL);
  llvm::ConstantInt* tableOffsetValue =
          static_cast<llvm::ConstantInt*>(tableOffsetNode->getOperand(0));
  int32_t tableOffset = tableOffsetValue->getSExtValue();
  RegLocation rlSrc = getLoc(cUnit, testVal);
  const u2* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  u2 tableMagic = *table;
  if (tableMagic == 0x100) {
    genPackedSwitch(cUnit, tableOffset, rlSrc);
  } else {
    DCHECK_EQ(tableMagic, 0x200);
    genSparseSwitch(cUnit, tableOffset, rlSrc);
  }
}

void cvtInvoke(CompilationUnit* cUnit, llvm::CallInst* callInst,
               bool isVoid, bool isFilledNewArray)
{
  CallInfo* info = (CallInfo*)oatNew(cUnit, sizeof(CallInfo), true,
                                         kAllocMisc);
  if (isVoid) {
    info->result.location = kLocInvalid;
  } else {
    info->result = getLoc(cUnit, callInst);
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
    RegLocation tLoc = getLoc(cUnit, callInst->getArgOperand(i));
    info->numArgWords += tLoc.wide ? 2 : 1;
  }
  info->args = (info->numArgWords == 0) ? NULL : (RegLocation*)
      oatNew(cUnit, sizeof(RegLocation) * info->numArgWords, false, kAllocMisc);
  // Now, fill in the location records, synthesizing high loc of wide vals
  for (int i = 3, next = 0; next < info->numArgWords;) {
    info->args[next] = getLoc(cUnit, callInst->getArgOperand(i++));
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
    genFilledNewArray(cUnit, info);
  } else {
    genInvoke(cUnit, info);
  }
}

/* Look up the RegLocation associated with a Value.  Must already be defined */
RegLocation valToLoc(CompilationUnit* cUnit, llvm::Value* val)
{
  SafeMap<llvm::Value*, RegLocation>::iterator it = cUnit->locMap.find(val);
  DCHECK(it != cUnit->locMap.end()) << "Missing definition";
  return it->second;
}

bool methodBitcodeBlockCodeGen(CompilationUnit* cUnit, llvm::BasicBlock* bb)
{
  bool isEntry = (bb == &cUnit->func->getEntryBlock());
  // Define the starting label
  LIR* blockLabel = cUnit->blockToLabelMap.Get(bb);
  // Extract the starting offset from the block's name
  if (!isEntry) {
    const char* blockName = bb->getName().str().c_str();
    int dummy;
    sscanf(blockName, kLabelFormat, &blockLabel->operands[0], &dummy);
  }
  // Set the label kind
  blockLabel->opcode = kPseudoNormalBlockLabel;
  // Insert the label
  oatAppendLIR(cUnit, blockLabel);

  // Free temp registers and reset redundant store tracking */
  oatResetRegPool(cUnit);
  oatResetDefTracking(cUnit);

  //TODO: restore oat incoming liveness optimization
  oatClobberAllRegs(cUnit);

  LIR* headLIR = NULL;

  if (isEntry) {
    cUnit->currentDalvikOffset = 0;
    RegLocation* argLocs = (RegLocation*)
        oatNew(cUnit, sizeof(RegLocation) * cUnit->numIns, true, kAllocMisc);
    llvm::Function::arg_iterator it(cUnit->func->arg_begin());
    llvm::Function::arg_iterator it_end(cUnit->func->arg_end());
    // Skip past Method*
    it++;
    for (unsigned i = 0; it != it_end; ++it) {
      llvm::Value* val = it;
      argLocs[i++] = valToLoc(cUnit, val);
      llvm::Type* ty = val->getType();
      if ((ty == cUnit->irb->getInt64Ty()) || (ty == cUnit->irb->getDoubleTy())) {
        argLocs[i] = argLocs[i-1];
        argLocs[i].lowReg = argLocs[i].highReg;
        argLocs[i].origSReg++;
        argLocs[i].sRegLow = INVALID_SREG;
        argLocs[i].highWord = true;
        i++;
      }
    }
    genEntrySequence(cUnit, argLocs, cUnit->methodLoc);
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

    oatResetRegPool(cUnit);
    if (cUnit->disableOpt & (1 << kTrackLiveTemps)) {
      oatClobberAllRegs(cUnit);
    }

    if (cUnit->disableOpt & (1 << kSuppressLoads)) {
      oatResetDefTracking(cUnit);
    }

#ifndef NDEBUG
    /* Reset temp tracking sanity check */
    cUnit->liveSReg = INVALID_SREG;
#endif

    LIR* boundaryLIR;
    const char* instStr = "boundary";
    boundaryLIR = newLIR1(cUnit, kPseudoDalvikByteCodeBoundary,
                          (intptr_t) instStr);
    cUnit->boundaryMap.Overwrite(cUnit->currentDalvikOffset, boundaryLIR);

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
            cvtICmpBr(cUnit, inst, brInst);
            ++it;
          } else {
            cvtICmp(cUnit, inst);
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
              // Ignore shadow frame stuff for quick compiler
              break;
            case greenland::IntrinsicHelper::CopyInt:
            case greenland::IntrinsicHelper::CopyObj:
            case greenland::IntrinsicHelper::CopyFloat:
            case greenland::IntrinsicHelper::CopyLong:
            case greenland::IntrinsicHelper::CopyDouble:
              cvtCopy(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::ConstInt:
            case greenland::IntrinsicHelper::ConstObj:
            case greenland::IntrinsicHelper::ConstLong:
            case greenland::IntrinsicHelper::ConstFloat:
            case greenland::IntrinsicHelper::ConstDouble:
              cvtConst(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::DivInt:
            case greenland::IntrinsicHelper::DivLong:
              cvtBinOp(cUnit, kOpDiv, inst);
              break;
            case greenland::IntrinsicHelper::RemInt:
            case greenland::IntrinsicHelper::RemLong:
              cvtBinOp(cUnit, kOpRem, inst);
              break;
            case greenland::IntrinsicHelper::MethodInfo:
              // Already dealt with - just ignore it here.
              break;
            case greenland::IntrinsicHelper::CheckSuspend:
              genSuspendTest(cUnit, 0 /* optFlags already applied */);
              break;
            case greenland::IntrinsicHelper::HLInvokeObj:
            case greenland::IntrinsicHelper::HLInvokeFloat:
            case greenland::IntrinsicHelper::HLInvokeDouble:
            case greenland::IntrinsicHelper::HLInvokeLong:
            case greenland::IntrinsicHelper::HLInvokeInt:
              cvtInvoke(cUnit, callInst, false /* isVoid */, false /* newArray */);
              break;
            case greenland::IntrinsicHelper::HLInvokeVoid:
              cvtInvoke(cUnit, callInst, true /* isVoid */, false /* newArray */);
              break;
            case greenland::IntrinsicHelper::FilledNewArray:
              cvtInvoke(cUnit, callInst, false /* isVoid */, true /* newArray */);
              break;
            case greenland::IntrinsicHelper::FillArrayData:
              cvtFillArrayData(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::ConstString:
              cvtConstObject(cUnit, callInst, true /* isString */);
              break;
            case greenland::IntrinsicHelper::ConstClass:
              cvtConstObject(cUnit, callInst, false /* isString */);
              break;
            case greenland::IntrinsicHelper::CheckCast:
              cvtCheckCast(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::NewInstance:
              cvtNewInstance(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::HLSgetObject:
              cvtSget(cUnit, callInst, false /* wide */, true /* Object */);
              break;
            case greenland::IntrinsicHelper::HLSget:
            case greenland::IntrinsicHelper::HLSgetFloat:
            case greenland::IntrinsicHelper::HLSgetBoolean:
            case greenland::IntrinsicHelper::HLSgetByte:
            case greenland::IntrinsicHelper::HLSgetChar:
            case greenland::IntrinsicHelper::HLSgetShort:
              cvtSget(cUnit, callInst, false /* wide */, false /* Object */);
              break;
            case greenland::IntrinsicHelper::HLSgetWide:
            case greenland::IntrinsicHelper::HLSgetDouble:
              cvtSget(cUnit, callInst, true /* wide */, false /* Object */);
              break;
            case greenland::IntrinsicHelper::HLSput:
            case greenland::IntrinsicHelper::HLSputFloat:
            case greenland::IntrinsicHelper::HLSputBoolean:
            case greenland::IntrinsicHelper::HLSputByte:
            case greenland::IntrinsicHelper::HLSputChar:
            case greenland::IntrinsicHelper::HLSputShort:
              cvtSput(cUnit, callInst, false /* wide */, false /* Object */);
              break;
            case greenland::IntrinsicHelper::HLSputWide:
            case greenland::IntrinsicHelper::HLSputDouble:
              cvtSput(cUnit, callInst, true /* wide */, false /* Object */);
              break;
            case greenland::IntrinsicHelper::HLSputObject:
              cvtSput(cUnit, callInst, false /* wide */, true /* Object */);
              break;
            case greenland::IntrinsicHelper::GetException:
              cvtMoveException(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::Throw:
              cvtThrow(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::ThrowVerificationError:
              cvtThrowVerificationError(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::MonitorEnter:
              cvtMonitorEnterExit(cUnit, true /* isEnter */, callInst);
              break;
            case greenland::IntrinsicHelper::MonitorExit:
              cvtMonitorEnterExit(cUnit, false /* isEnter */, callInst);
              break;
            case greenland::IntrinsicHelper::ArrayLength:
              cvtArrayLength(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::NewArray:
              cvtNewArray(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::InstanceOf:
              cvtInstanceOf(cUnit, callInst);
              break;

            case greenland::IntrinsicHelper::HLArrayGet:
            case greenland::IntrinsicHelper::HLArrayGetObject:
            case greenland::IntrinsicHelper::HLArrayGetFloat:
              cvtAget(cUnit, callInst, kWord, 2);
              break;
            case greenland::IntrinsicHelper::HLArrayGetWide:
            case greenland::IntrinsicHelper::HLArrayGetDouble:
              cvtAget(cUnit, callInst, kLong, 3);
              break;
            case greenland::IntrinsicHelper::HLArrayGetBoolean:
              cvtAget(cUnit, callInst, kUnsignedByte, 0);
              break;
            case greenland::IntrinsicHelper::HLArrayGetByte:
              cvtAget(cUnit, callInst, kSignedByte, 0);
              break;
            case greenland::IntrinsicHelper::HLArrayGetChar:
              cvtAget(cUnit, callInst, kUnsignedHalf, 1);
              break;
            case greenland::IntrinsicHelper::HLArrayGetShort:
              cvtAget(cUnit, callInst, kSignedHalf, 1);
              break;

            case greenland::IntrinsicHelper::HLArrayPut:
            case greenland::IntrinsicHelper::HLArrayPutFloat:
              cvtAputPrimitive(cUnit, callInst, kWord, 2);
              break;
            case greenland::IntrinsicHelper::HLArrayPutObject:
              cvtAputObj(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::HLArrayPutWide:
            case greenland::IntrinsicHelper::HLArrayPutDouble:
              cvtAputPrimitive(cUnit, callInst, kLong, 3);
              break;
            case greenland::IntrinsicHelper::HLArrayPutBoolean:
              cvtAputPrimitive(cUnit, callInst, kUnsignedByte, 0);
              break;
            case greenland::IntrinsicHelper::HLArrayPutByte:
              cvtAputPrimitive(cUnit, callInst, kSignedByte, 0);
              break;
            case greenland::IntrinsicHelper::HLArrayPutChar:
              cvtAputPrimitive(cUnit, callInst, kUnsignedHalf, 1);
              break;
            case greenland::IntrinsicHelper::HLArrayPutShort:
              cvtAputPrimitive(cUnit, callInst, kSignedHalf, 1);
              break;

            case greenland::IntrinsicHelper::HLIGet:
            case greenland::IntrinsicHelper::HLIGetFloat:
              cvtIget(cUnit, callInst, kWord, false /* isWide */, false /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIGetObject:
              cvtIget(cUnit, callInst, kWord, false /* isWide */, true /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIGetWide:
            case greenland::IntrinsicHelper::HLIGetDouble:
              cvtIget(cUnit, callInst, kLong, true /* isWide */, false /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIGetBoolean:
              cvtIget(cUnit, callInst, kUnsignedByte, false /* isWide */,
                      false /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIGetByte:
              cvtIget(cUnit, callInst, kSignedByte, false /* isWide */,
                      false /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIGetChar:
              cvtIget(cUnit, callInst, kUnsignedHalf, false /* isWide */,
                      false /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIGetShort:
              cvtIget(cUnit, callInst, kSignedHalf, false /* isWide */,
                      false /* obj */);
              break;

            case greenland::IntrinsicHelper::HLIPut:
            case greenland::IntrinsicHelper::HLIPutFloat:
              cvtIput(cUnit, callInst, kWord, false /* isWide */, false /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIPutObject:
              cvtIput(cUnit, callInst, kWord, false /* isWide */, true /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIPutWide:
            case greenland::IntrinsicHelper::HLIPutDouble:
              cvtIput(cUnit, callInst, kLong, true /* isWide */, false /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIPutBoolean:
              cvtIput(cUnit, callInst, kUnsignedByte, false /* isWide */,
                      false /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIPutByte:
              cvtIput(cUnit, callInst, kSignedByte, false /* isWide */,
                      false /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIPutChar:
              cvtIput(cUnit, callInst, kUnsignedHalf, false /* isWide */,
                      false /* obj */);
              break;
            case greenland::IntrinsicHelper::HLIPutShort:
              cvtIput(cUnit, callInst, kSignedHalf, false /* isWide */,
                      false /* obj */);
              break;

            case greenland::IntrinsicHelper::IntToChar:
              cvtIntNarrowing(cUnit, callInst, Instruction::INT_TO_CHAR);
              break;
            case greenland::IntrinsicHelper::IntToShort:
              cvtIntNarrowing(cUnit, callInst, Instruction::INT_TO_SHORT);
              break;
            case greenland::IntrinsicHelper::IntToByte:
              cvtIntNarrowing(cUnit, callInst, Instruction::INT_TO_BYTE);
              break;

            case greenland::IntrinsicHelper::CmplFloat:
              cvtFPCompare(cUnit, callInst, Instruction::CMPL_FLOAT);
              break;
            case greenland::IntrinsicHelper::CmpgFloat:
              cvtFPCompare(cUnit, callInst, Instruction::CMPG_FLOAT);
              break;
            case greenland::IntrinsicHelper::CmplDouble:
              cvtFPCompare(cUnit, callInst, Instruction::CMPL_DOUBLE);
              break;
            case greenland::IntrinsicHelper::CmpgDouble:
              cvtFPCompare(cUnit, callInst, Instruction::CMPG_DOUBLE);
              break;

            case greenland::IntrinsicHelper::CmpLong:
              cvtLongCompare(cUnit, callInst);
              break;

            case greenland::IntrinsicHelper::SHLLong:
              cvtShiftOp(cUnit, Instruction::SHL_LONG, callInst);
              break;
            case greenland::IntrinsicHelper::SHRLong:
              cvtShiftOp(cUnit, Instruction::SHR_LONG, callInst);
              break;
            case greenland::IntrinsicHelper::USHRLong:
              cvtShiftOp(cUnit, Instruction::USHR_LONG, callInst);
              break;
            case greenland::IntrinsicHelper::SHLInt:
              cvtShiftOp(cUnit, Instruction::SHL_INT, callInst);
              break;
            case greenland::IntrinsicHelper::SHRInt:
              cvtShiftOp(cUnit, Instruction::SHR_INT, callInst);
              break;
            case greenland::IntrinsicHelper::USHRInt:
              cvtShiftOp(cUnit, Instruction::USHR_INT, callInst);
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
                 opUnconditionalBranch(cUnit,
                                       cUnit->blockToLabelMap.Get(targetBB));
                 ++it;
              }
              break;

            default:
              LOG(FATAL) << "Unexpected intrinsic " << (int)id << ", "
                         << cUnit->intrinsic_helper->GetName(id);
          }
        }
        break;

      case llvm::Instruction::Br: cvtBr(cUnit, inst); break;
      case llvm::Instruction::Add: cvtBinOp(cUnit, kOpAdd, inst); break;
      case llvm::Instruction::Sub: cvtBinOp(cUnit, kOpSub, inst); break;
      case llvm::Instruction::Mul: cvtBinOp(cUnit, kOpMul, inst); break;
      case llvm::Instruction::SDiv: cvtBinOp(cUnit, kOpDiv, inst); break;
      case llvm::Instruction::SRem: cvtBinOp(cUnit, kOpRem, inst); break;
      case llvm::Instruction::And: cvtBinOp(cUnit, kOpAnd, inst); break;
      case llvm::Instruction::Or: cvtBinOp(cUnit, kOpOr, inst); break;
      case llvm::Instruction::Xor: cvtBinOp(cUnit, kOpXor, inst); break;
      case llvm::Instruction::PHI: cvtPhi(cUnit, inst); break;
      case llvm::Instruction::Ret: cvtRet(cUnit, inst); break;
      case llvm::Instruction::FAdd: cvtBinFPOp(cUnit, kOpAdd, inst); break;
      case llvm::Instruction::FSub: cvtBinFPOp(cUnit, kOpSub, inst); break;
      case llvm::Instruction::FMul: cvtBinFPOp(cUnit, kOpMul, inst); break;
      case llvm::Instruction::FDiv: cvtBinFPOp(cUnit, kOpDiv, inst); break;
      case llvm::Instruction::FRem: cvtBinFPOp(cUnit, kOpRem, inst); break;
      case llvm::Instruction::SIToFP: cvtIntToFP(cUnit, inst); break;
      case llvm::Instruction::FPToSI: cvtFPToInt(cUnit, inst); break;
      case llvm::Instruction::FPTrunc: cvtDoubleToFloat(cUnit, inst); break;
      case llvm::Instruction::FPExt: cvtFloatToDouble(cUnit, inst); break;
      case llvm::Instruction::Trunc: cvtTrunc(cUnit, inst); break;

      case llvm::Instruction::ZExt: cvtIntExt(cUnit, inst, false /* signed */);
        break;
      case llvm::Instruction::SExt: cvtIntExt(cUnit, inst, true /* signed */);
        break;

      case llvm::Instruction::Switch: cvtSwitch(cUnit, inst); break;

      case llvm::Instruction::Unreachable:
        break;  // FIXME: can we really ignore these?

      case llvm::Instruction::Shl:
      case llvm::Instruction::LShr:
      case llvm::Instruction::AShr:
      case llvm::Instruction::Invoke:
      case llvm::Instruction::FPToUI:
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
    oatApplyLocalOptimizations(cUnit, headLIR, cUnit->lastLIRInsn);
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
void oatMethodBitcode2LIR(CompilationUnit* cUnit)
{
  llvm::Function* func = cUnit->func;
  int numBasicBlocks = func->getBasicBlockList().size();
  // Allocate a list for LIR basic block labels
  cUnit->blockLabelList =
    (LIR*)oatNew(cUnit, sizeof(LIR) * numBasicBlocks, true, kAllocLIR);
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
            p->fpReg = (mapData >> 16) & 0xff;
            p->coreReg = (mapData >> 8) & 0xff;
            p->fpLocation = static_cast<RegLocationType>((mapData >> 4) & 0xf);
            if (p->fpLocation == kLocPhysReg) {
              oatRecordFpPromotion(cUnit, p->fpReg, i);
            }
            p->coreLocation = static_cast<RegLocationType>(mapData & 0xf);
            if (p->coreLocation == kLocPhysReg) {
              oatRecordCorePromotion(cUnit, p->coreReg, i);
            }
          }
          if (cUnit->printMe) {
            oatDumpPromotionMap(cUnit);
          }
        }
        break;
      }
    }
  }
  oatAdjustSpillMask(cUnit);
  cUnit->frameSize = oatComputeFrameSize(cUnit);

  // Create RegLocations for arguments
  llvm::Function::arg_iterator it(cUnit->func->arg_begin());
  llvm::Function::arg_iterator it_end(cUnit->func->arg_end());
  for (; it != it_end; ++it) {
    llvm::Value* val = it;
    createLocFromValue(cUnit, val);
  }
  // Create RegLocations for all non-argument defintions
  for (llvm::inst_iterator i = llvm::inst_begin(func),
       e = llvm::inst_end(func); i != e; ++i) {
    llvm::Value* val = &*i;
    if (val->hasName() && (val->getName().str().c_str()[0] == 'v')) {
      createLocFromValue(cUnit, val);
    }
  }

  // Walk the blocks, generating code.
  for (llvm::Function::iterator i = cUnit->func->begin(),
       e = cUnit->func->end(); i != e; ++i) {
    methodBitcodeBlockCodeGen(cUnit, static_cast<llvm::BasicBlock*>(i));
  }

  handleSuspendLaunchpads(cUnit);

  handleThrowLaunchpads(cUnit);

  handleIntrinsicLaunchpads(cUnit);

  freeIR(cUnit);
}


}  // namespace art

#endif  // ART_USE_QUICK_COMPILER
