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
  CHECK(placeholder != NULL) << "Null placeholder - shouldn't happen";
  placeholder->replaceAllUsesWith(val);
  val->takeName(placeholder);
  cUnit->llvmValues.elemList[sReg] = (intptr_t)val;
}

llvm::Type* llvmTypeFromLocRec(CompilationUnit* cUnit, RegLocation loc)
{
  llvm::Type* res = NULL;
  if (loc.wide) {
    if (loc.fp)
        res = cUnit->irb->GetJDoubleTy();
    else
        res = cUnit->irb->GetJLongTy();
  } else {
    if (loc.fp) {
      res = cUnit->irb->GetJFloatTy();
    } else {
      if (loc.ref)
        res = cUnit->irb->GetJObjectTy();
      else
        res = cUnit->irb->GetJIntTy();
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
  if (cUnit->printMe) {
    LOG(INFO) << "Processing llvm Value " << valName;
  }
  SafeMap<llvm::Value*, RegLocation>::iterator it = cUnit->locMap.find(val);
  DCHECK(it == cUnit->locMap.end()) << " - already defined: " << valName;
  int baseSReg = INVALID_SREG;
  int subscript = -1;
  sscanf(valName, "v%d_%d", &baseSReg, &subscript);
  if ((baseSReg == INVALID_SREG) && (!strcmp(valName, "method"))) {
    baseSReg = SSA_METHOD_BASEREG;
    subscript = 0;
  }
  if (cUnit->printMe) {
    LOG(INFO) << "Base: " << baseSReg << ", Sub: " << subscript;
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
  if ((ty == cUnit->irb->getFloatTy()) ||
      (ty == cUnit->irb->getDoubleTy())) {
    loc.fp = true;
  } else if (ty == cUnit->irb->GetJObjectTy()) {
    loc.ref = true;
  } else {
    loc.core = true;
  }
  loc.home = false;  // Will change during promotion
  loc.sRegLow = baseSReg;
  loc.origSReg = cUnit->locMap.size();
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
    } if (loc.ref) {
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
    } if (loc.ref) {
      id = greenland::IntrinsicHelper::CopyObj;
    } else {
      id = greenland::IntrinsicHelper::CopyInt;
    }
  }
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  return cUnit->irb->CreateCall(intr, src);
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
  condValue->setName(StringPrintf("t%d", cUnit->tempName++));
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
    case kOpMul: res = cUnit->irb->CreateMul(src1, src2); break;
    case kOpOr: res = cUnit->irb->CreateOr(src1, src2); break;
    case kOpAnd: res = cUnit->irb->CreateAnd(src1, src2); break;
    case kOpXor: res = cUnit->irb->CreateXor(src1, src2); break;
    case kOpDiv: res = genDivModOp(cUnit, true, isLong, src1, src2); break;
    case kOpRem: res = genDivModOp(cUnit, false, isLong, src1, src2); break;
    case kOpLsl: UNIMPLEMENTED(FATAL) << "Need Lsl"; break;
    case kOpLsr: UNIMPLEMENTED(FATAL) << "Need Lsr"; break;
    case kOpAsr: UNIMPLEMENTED(FATAL) << "Need Asr"; break;
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

void convertArithOp(CompilationUnit* cUnit, OpKind op, RegLocation rlDest,
                    RegLocation rlSrc1, RegLocation rlSrc2)
{
  llvm::Value* src1 = getLLVMValue(cUnit, rlSrc1.origSReg);
  llvm::Value* src2 = getLLVMValue(cUnit, rlSrc2.origSReg);
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

void convertInvoke(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                   InvokeType invokeType, bool isRange)
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
  if (cUnit->printMe) {
    LOG(INFO) << "Building Invoke info";
  }
  for (int i = 0; i < info->numArgWords;) {
    if (cUnit->printMe) {
      oatDumpRegLoc(info->args[i]);
    }
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
  if (info->result.location == kLocInvalid) {
    id = greenland::IntrinsicHelper::HLInvokeVoid;
  } else {
    if (info->result.wide) {
      if (info->result.fp) {
        id = greenland::IntrinsicHelper::HLInvokeDouble;
      } else {
        id = greenland::IntrinsicHelper::HLInvokeFloat;
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

void convertConstString(CompilationUnit* cUnit, BasicBlock* bb,
                        uint32_t string_idx, RegLocation rlDest)
{
  greenland::IntrinsicHelper::IntrinsicId id;
  id = greenland::IntrinsicHelper::ConstString;
  llvm::Function* intr = cUnit->intrinsic_helper->GetIntrinsicFunction(id);
  llvm::Value* index = cUnit->irb->getInt32(string_idx);
  llvm::Value* res = cUnit->irb->CreateCall(intr, index);
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
  RegLocation rlResult = badLoc;
  Instruction::Code opcode = mir->dalvikInsn.opcode;
  uint32_t vB = mir->dalvikInsn.vB;
  uint32_t vC = mir->dalvikInsn.vC;

  bool objectDefinition = false;

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
        llvm::Constant* immValue = cUnit->irb->GetJLong(vB);
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
    case Instruction::CONST_WIDE_HIGH16: {
        int64_t imm = static_cast<int64_t>(vB) << 48;
        llvm::Constant* immValue = cUnit->irb->GetJLong(imm);
        llvm::Value* res = emitConst(cUnit, immValue, rlDest);
        defineValue(cUnit, res, rlDest.origSReg);
    }

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
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      convertArithOp(cUnit, kOpLsl, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      convertArithOp(cUnit, kOpAsr, rlDest, rlSrc[0], rlSrc[1]);
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      convertArithOp(cUnit, kOpLsr, rlDest, rlSrc[0], rlSrc[1]);
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
      convertArithOpLit(cUnit, kOpLsl, rlDest, rlSrc[0], vC);
      break;
    case Instruction::SHR_INT_LIT8:
      convertArithOpLit(cUnit, kOpLsr, rlDest, rlSrc[0], vC);
      break;
    case Instruction::USHR_INT_LIT8:
      convertArithOpLit(cUnit, kOpAsr, rlDest, rlSrc[0], vC);
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
      convertInvoke(cUnit, bb, mir, kStatic, false /*range*/);
      break;
    case Instruction::INVOKE_STATIC_RANGE:
      convertInvoke(cUnit, bb, mir, kStatic, true /*range*/);
      break;

    case Instruction::INVOKE_DIRECT:
      convertInvoke(cUnit, bb,  mir, kDirect, false /*range*/);
      break;
    case Instruction::INVOKE_DIRECT_RANGE:
      convertInvoke(cUnit, bb, mir, kDirect, true /*range*/);
      break;

    case Instruction::INVOKE_VIRTUAL:
      convertInvoke(cUnit, bb, mir, kVirtual, false /*range*/);
      break;
    case Instruction::INVOKE_VIRTUAL_RANGE:
      convertInvoke(cUnit, bb, mir, kVirtual, true /*range*/);
      break;

    case Instruction::INVOKE_SUPER:
      convertInvoke(cUnit, bb, mir, kSuper, false /*range*/);
      break;
    case Instruction::INVOKE_SUPER_RANGE:
      convertInvoke(cUnit, bb, mir, kSuper, true /*range*/);
      break;

    case Instruction::INVOKE_INTERFACE:
      convertInvoke(cUnit, bb, mir, kInterface, false /*range*/);
      break;
    case Instruction::INVOKE_INTERFACE_RANGE:
      convertInvoke(cUnit, bb, mir, kInterface, true /*range*/);
      break;

    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      convertConstString(cUnit, bb, vB, rlDest);
      break;


#if 0

    case Instruction::MOVE_EXCEPTION: {
      int exOffset = Thread::ExceptionOffset().Int32Value();
      rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
#if defined(TARGET_X86)
      newLIR2(cUnit, kX86Mov32RT, rlResult.lowReg, exOffset);
      newLIR2(cUnit, kX86Mov32TI, exOffset, 0);
#else
      int resetReg = oatAllocTemp(cUnit);
      loadWordDisp(cUnit, rSELF, exOffset, rlResult.lowReg);
      loadConstant(cUnit, resetReg, 0);
      storeWordDisp(cUnit, rSELF, exOffset, resetReg);
      storeValue(cUnit, rlDest, rlResult);
      oatFreeTemp(cUnit, resetReg);
#endif
      break;
    }

    case Instruction::MOVE_RESULT_WIDE:
      if (mir->optimizationFlags & MIR_INLINED)
        break;  // Nop - combined w/ previous invoke
      storeValueWide(cUnit, rlDest, oatGetReturnWide(cUnit, rlDest.fp));
      break;

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_OBJECT:
      if (mir->optimizationFlags & MIR_INLINED)
        break;  // Nop - combined w/ previous invoke
      storeValue(cUnit, rlDest, oatGetReturn(cUnit, rlDest.fp));
      break;

    case Instruction::MONITOR_ENTER:
      genMonitorEnter(cUnit, mir, rlSrc[0]);
      break;

    case Instruction::MONITOR_EXIT:
      genMonitorExit(cUnit, mir, rlSrc[0]);
      break;

    case Instruction::CHECK_CAST:
      genCheckCast(cUnit, mir, rlSrc[0]);
      break;

    case Instruction::INSTANCE_OF:
      genInstanceof(cUnit, mir, rlDest, rlSrc[0]);
      break;

    case Instruction::NEW_INSTANCE:
      genNewInstance(cUnit, mir, rlDest);
      break;

    case Instruction::THROW:
      genThrow(cUnit, mir, rlSrc[0]);
      break;

    case Instruction::THROW_VERIFICATION_ERROR:
      genThrowVerificationError(cUnit, mir);
      break;

    case Instruction::ARRAY_LENGTH:
      int lenOffset;
      lenOffset = Array::LengthOffset().Int32Value();
      rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
      genNullCheck(cUnit, rlSrc[0].sRegLow, rlSrc[0].lowReg, mir);
      rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
      loadWordDisp(cUnit, rlSrc[0].lowReg, lenOffset, rlResult.lowReg);
      storeValue(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_CLASS:
      genConstClass(cUnit, mir, rlDest, rlSrc[0]);
      break;

    case Instruction::FILL_ARRAY_DATA:
      genFillArrayData(cUnit, mir, rlSrc[0]);
      break;

    case Instruction::FILLED_NEW_ARRAY:
      genFilledNewArray(cUnit, mir, false /* not range */);
      break;

    case Instruction::FILLED_NEW_ARRAY_RANGE:
      genFilledNewArray(cUnit, mir, true /* range */);
      break;

    case Instruction::NEW_ARRAY:
      genNewArray(cUnit, mir, rlDest, rlSrc[0]);
      break;

    case Instruction::PACKED_SWITCH:
      genPackedSwitch(cUnit, mir, rlSrc[0]);
      break;

    case Instruction::SPARSE_SWITCH:
      genSparseSwitch(cUnit, mir, rlSrc[0], labelList);
      break;

    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
      res = genCmpFP(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::CMP_LONG:
      genCmpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::AGET_WIDE:
      genArrayGet(cUnit, mir, kLong, rlSrc[0], rlSrc[1], rlDest, 3);
      break;
    case Instruction::AGET:
    case Instruction::AGET_OBJECT:
      genArrayGet(cUnit, mir, kWord, rlSrc[0], rlSrc[1], rlDest, 2);
      break;
    case Instruction::AGET_BOOLEAN:
      genArrayGet(cUnit, mir, kUnsignedByte, rlSrc[0], rlSrc[1], rlDest, 0);
      break;
    case Instruction::AGET_BYTE:
      genArrayGet(cUnit, mir, kSignedByte, rlSrc[0], rlSrc[1], rlDest, 0);
      break;
    case Instruction::AGET_CHAR:
      genArrayGet(cUnit, mir, kUnsignedHalf, rlSrc[0], rlSrc[1], rlDest, 1);
      break;
    case Instruction::AGET_SHORT:
      genArrayGet(cUnit, mir, kSignedHalf, rlSrc[0], rlSrc[1], rlDest, 1);
      break;
    case Instruction::APUT_WIDE:
      genArrayPut(cUnit, mir, kLong, rlSrc[1], rlSrc[2], rlSrc[0], 3);
      break;
    case Instruction::APUT:
      genArrayPut(cUnit, mir, kWord, rlSrc[1], rlSrc[2], rlSrc[0], 2);
      break;
    case Instruction::APUT_OBJECT:
      genArrayObjPut(cUnit, mir, rlSrc[1], rlSrc[2], rlSrc[0], 2);
      break;
    case Instruction::APUT_SHORT:
    case Instruction::APUT_CHAR:
      genArrayPut(cUnit, mir, kUnsignedHalf, rlSrc[1], rlSrc[2], rlSrc[0], 1);
      break;
    case Instruction::APUT_BYTE:
    case Instruction::APUT_BOOLEAN:
      genArrayPut(cUnit, mir, kUnsignedByte, rlSrc[1], rlSrc[2],
            rlSrc[0], 0);
      break;

    case Instruction::IGET_OBJECT:
    //case Instruction::IGET_OBJECT_VOLATILE:
      genIGet(cUnit, mir, kWord, rlDest, rlSrc[0], false, true);
      break;

    case Instruction::IGET_WIDE:
    //case Instruction::IGET_WIDE_VOLATILE:
      genIGet(cUnit, mir, kLong, rlDest, rlSrc[0], true, false);
      break;

    case Instruction::IGET:
    //case Instruction::IGET_VOLATILE:
      genIGet(cUnit, mir, kWord, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IGET_CHAR:
      genIGet(cUnit, mir, kUnsignedHalf, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IGET_SHORT:
      genIGet(cUnit, mir, kSignedHalf, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
      genIGet(cUnit, mir, kUnsignedByte, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IPUT_WIDE:
    //case Instruction::IPUT_WIDE_VOLATILE:
      genIPut(cUnit, mir, kLong, rlSrc[0], rlSrc[1], true, false);
      break;

    case Instruction::IPUT_OBJECT:
    //case Instruction::IPUT_OBJECT_VOLATILE:
      genIPut(cUnit, mir, kWord, rlSrc[0], rlSrc[1], false, true);
      break;

    case Instruction::IPUT:
    //case Instruction::IPUT_VOLATILE:
      genIPut(cUnit, mir, kWord, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
      genIPut(cUnit, mir, kUnsignedByte, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::IPUT_CHAR:
      genIPut(cUnit, mir, kUnsignedHalf, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::IPUT_SHORT:
      genIPut(cUnit, mir, kSignedHalf, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::SGET_OBJECT:
      genSget(cUnit, mir, rlDest, false, true);
      break;
    case Instruction::SGET:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
      genSget(cUnit, mir, rlDest, false, false);
      break;

    case Instruction::SGET_WIDE:
      genSget(cUnit, mir, rlDest, true, false);
      break;

    case Instruction::SPUT_OBJECT:
      genSput(cUnit, mir, rlSrc[0], false, true);
      break;

    case Instruction::SPUT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
      genSput(cUnit, mir, rlSrc[0], false, false);
      break;

    case Instruction::SPUT_WIDE:
      genSput(cUnit, mir, rlSrc[0], true, false);
      break;

    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
      res = genArithOpInt(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      res = genArithOpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::NEG_FLOAT:
      res = genArithOpFloat(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::NEG_DOUBLE:
      res = genArithOpDouble(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::INT_TO_LONG:
      genIntToLong(cUnit, mir, rlDest, rlSrc[0]);
      break;

    case Instruction::LONG_TO_INT:
      rlSrc[0] = oatUpdateLocWide(cUnit, rlSrc[0]);
      rlSrc[0] = oatWideToNarrow(cUnit, rlSrc[0]);
      storeValue(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_BYTE:
    case Instruction::INT_TO_SHORT:
    case Instruction::INT_TO_CHAR:
      genIntNarrowing(cUnit, mir, rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_FLOAT:
    case Instruction::INT_TO_DOUBLE:
    case Instruction::LONG_TO_FLOAT:
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::FLOAT_TO_INT:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::FLOAT_TO_DOUBLE:
    case Instruction::DOUBLE_TO_INT:
    case Instruction::DOUBLE_TO_LONG:
    case Instruction::DOUBLE_TO_FLOAT:
      genConversion(cUnit, mir);
      break;

#endif

    default:
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
      int* incoming = (int*)mir->dalvikInsn.vB;
      RegLocation rlDest = cUnit->regLocation[mir->ssaRep->defs[0]];
      llvm::Type* phiType =
          llvmTypeFromLocRec(cUnit, rlDest);
      llvm::PHINode* phi = cUnit->irb->CreatePHI(phiType, mir->ssaRep->numUses);
      for (int i = 0; i < mir->ssaRep->numUses; i++) {
        RegLocation loc;
        if (rlDest.wide) {
           loc = oatGetSrcWide(cUnit, mir, i);
           i++;
        } else {
           loc = oatGetSrc(cUnit, mir, i);
        }
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
  llvm::SmallVector<llvm::Value*, 1>arrayRef;
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
     * TODO: add new block type for exception blocks that we generate
     * greenland code for.
     */
    llvmBB->eraseFromParent();
    return false;
  }

  for (MIR* mir = bb->firstMIRInsn; mir; mir = mir->next) {

    setDexOffset(cUnit, mir->offset);

    Instruction::Code dalvikOpcode = mir->dalvikInsn.opcode;
    Instruction::Format dalvikFormat = Instruction::FormatOf(dalvikOpcode);

    /* If we're compiling for the debugger, generate an update callout */
    if (cUnit->genDebugger) {
      UNIMPLEMENTED(FATAL) << "Need debug codegen";
      //genDebuggerUpdate(cUnit, mir->offset);
    }

    if ((int)mir->dalvikInsn.opcode >= (int)kMirOpFirst) {
      convertExtendedMIR(cUnit, bb, mir, llvmBB);
      continue;
    }

    bool notHandled = convertMIRNode(cUnit, mir, bb, llvmBB,
                                     NULL /* labelList */);
    if (notHandled) {
      LOG(WARNING) << StringPrintf("%#06x: Op %#x (%s) / Fmt %d not handled",
                                   mir->offset, dalvikOpcode,
                                   Instruction::Name(dalvikOpcode),
                                   dalvikFormat);
    }
  }

  if ((bb->fallThrough != NULL) && !bb->hasReturn) {
    cUnit->irb->CreateBr(getLLVMBlock(cUnit, bb->fallThrough->id));
  }

  return false;
}

llvm::FunctionType* getFunctionType(CompilationUnit* cUnit) {

  // Get return type
  llvm::Type* ret_type = cUnit->irb->GetJType(cUnit->shorty[0],
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
    args_type.push_back(cUnit->irb->GetJType(cUnit->shorty[i],
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
    llvm::Type* ty = llvmTypeFromLocRec(cUnit, cUnit->regLocation[i]);
    if (i < cUnit->numRegs) {
      // Skip non-argument _0 names - should never be a use
      oatInsertGrowableList(cUnit, &cUnit->llvmValues, (intptr_t)0);
    } else if (i >= (cUnit->numRegs + cUnit->numIns)) {
      // Handle SSA defs, skipping Method* and compiler temps
      if (SRegToVReg(cUnit, i) < 0) {
        val = NULL;
      } else {
        val = cUnit->irb->CreateLoad(cUnit->irb->CreateAlloca(ty, 0));
        val->setName(llvmSSAName(cUnit, i));
      }
      oatInsertGrowableList(cUnit, &cUnit->llvmValues, (intptr_t)val);
      if (cUnit->regLocation[i].wide) {
        // Skip high half of wide values
        oatInsertGrowableList(cUnit, &cUnit->llvmValues, 0);
        i++;
      }
    } else {
      // Recover previously-created argument values
      llvm::Value* argVal = arg_iter++;
      oatInsertGrowableList(cUnit, &cUnit->llvmValues, (intptr_t)argVal);
    }
  }
  cUnit->irb->CreateBr(cUnit->placeholderBB);

  oatDataFlowAnalysisDispatcher(cUnit, methodBlockBitcodeConversion,
                                kPreOrderDFSTraversal, false /* Iterative */);

  cUnit->placeholderBB->eraseFromParent();

  llvm::verifyFunction(*cUnit->func, llvm::PrintMessageAction);

  if (cUnit->enableDebug & (1 << kDebugDumpBitcodeFile)) {
    // Write bitcode to file
    std::string errmsg;
    std::string fname(PrettyMethod(cUnit->method_idx, *cUnit->dex_file));
    oatReplaceSpecialChars(fname);
    // TODO: make configurable
    fname = StringPrintf("/tmp/%s.bc", fname.c_str());

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
    std::string valName(val->getName().str());
    DCHECK(!valName.empty());
    if (valName[0] == 'v') {
      int baseSReg = INVALID_SREG;
      sscanf(valName.c_str(), "v%d_", &baseSReg);
      res = cUnit->regLocation[baseSReg];
      cUnit->locMap.Put(val, res);
    } else {
      UNIMPLEMENTED(WARNING) << "Need to handle llvm temps";
      DCHECK_EQ(valName[0], 't');
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

void cvtBinOp(CompilationUnit* cUnit, OpKind op, llvm::Instruction* inst)
{
  RegLocation rlDest = getLoc(cUnit, inst);
  llvm::Value* lhs = inst->getOperand(0);
  DCHECK(llvm::dyn_cast<llvm::ConstantInt>(lhs) == NULL);
  RegLocation rlSrc1 = getLoc(cUnit, inst->getOperand(0));
  llvm::Value* rhs = inst->getOperand(1);
  if (llvm::ConstantInt* src2 = llvm::dyn_cast<llvm::ConstantInt>(rhs)) {
    Instruction::Code dalvikOp = getDalvikOpcode(op, true, false);
    genArithOpIntLit(cUnit, dalvikOp, rlDest, rlSrc1, src2->getSExtValue());
  } else {
    Instruction::Code dalvikOp = getDalvikOpcode(op, false, rlDest.wide);
    RegLocation rlSrc2 = getLoc(cUnit, rhs);
    if (rlDest.wide) {
      genArithOpLong(cUnit, dalvikOp, rlDest, rlSrc1, rlSrc2);
    } else {
      genArithOpInt(cUnit, dalvikOp, rlDest, rlSrc1, rlSrc2);
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
    case llvm::ICmpInst::ICMP_NE: res = kCondNe; break;
    case llvm::ICmpInst::ICMP_EQ: res = kCondEq; break;
    case llvm::ICmpInst::ICMP_SGT: res = kCondGt; break;
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
  DCHECK_EQ(callInst->getNumArgOperands(), 1);
  RegLocation rlSrc = getLoc(cUnit, callInst->getArgOperand(0));
  RegLocation rlDest = getLoc(cUnit, callInst);
  if (rlSrc.wide) {
    storeValueWide(cUnit, rlDest, rlSrc);
  } else {
    storeValue(cUnit, rlDest, rlSrc);
  }
}

// Note: Immediate arg is a ConstantInt regardless of result type
void cvtConst(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1);
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

void cvtConstString(CompilationUnit* cUnit, llvm::CallInst* callInst)
{
  DCHECK_EQ(callInst->getNumArgOperands(), 1);
  llvm::ConstantInt* stringIdxVal =
      llvm::dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(0));
  uint32_t stringIdx = stringIdxVal->getZExtValue();
  RegLocation rlDest = getLoc(cUnit, callInst);
  genConstString(cUnit, stringIdx, rlDest);
}

void cvtInvoke(CompilationUnit* cUnit, llvm::CallInst* callInst,
               greenland::JType jtype)
{
  CallInfo* info = (CallInfo*)oatNew(cUnit, sizeof(CallInfo), true,
                                         kAllocMisc);
  if (jtype == greenland::kVoid) {
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

  // FIXME - rework such that we no longer need isRange
  info->isRange = false;

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
    info->args[next] = getLoc(cUnit, callInst->getArgOperand(i));
    if (cUnit->printMe) {
      oatDumpRegLoc(info->args[next]);
    }
    if (info->args[next].wide) {
      next++;
      // TODO: Might make sense to mark this as an invalid loc
      info->args[next].origSReg = info->args[next-1].origSReg+1;
      info->args[next].sRegLow = info->args[next-1].sRegLow+1;
    }
    next++;
  }
  genInvoke(cUnit, info);
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
    for (unsigned i = 0; it != it_end; ++it) {
      llvm::Value* val = it;
      argLocs[i++] = valToLoc(cUnit, val);
      llvm::Type* ty = val->getType();
      if ((ty == cUnit->irb->getInt64Ty()) || (ty == cUnit->irb->getDoubleTy())) {
        argLocs[i++].sRegLow = INVALID_SREG;
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
            case greenland::IntrinsicHelper::MethodInfo:
              // Already dealt with - just ignore it here.
              break;
            case greenland::IntrinsicHelper::CheckSuspend:
              genSuspendTest(cUnit, 0 /* optFlags already applied */);
              break;
            case greenland::IntrinsicHelper::HLInvokeInt:
              cvtInvoke(cUnit, callInst, greenland::kInt);
              break;
            case greenland::IntrinsicHelper::HLInvokeVoid:
              cvtInvoke(cUnit, callInst, greenland::kVoid);
              break;
            case greenland::IntrinsicHelper::ConstString:
              cvtConstString(cUnit, callInst);
              break;
            case greenland::IntrinsicHelper::UnknownId:
              cvtCall(cUnit, callInst, callee);
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
      case llvm::Instruction::Shl: cvtBinOp(cUnit, kOpLsl, inst); break;
      case llvm::Instruction::LShr: cvtBinOp(cUnit, kOpLsr, inst); break;
      case llvm::Instruction::AShr: cvtBinOp(cUnit, kOpAsr, inst); break;
      case llvm::Instruction::PHI: cvtPhi(cUnit, inst); break;
      case llvm::Instruction::Ret: cvtRet(cUnit, inst); break;

      case llvm::Instruction::Invoke:
      case llvm::Instruction::FAdd:
      case llvm::Instruction::FSub:
      case llvm::Instruction::FMul:
      case llvm::Instruction::FDiv:
      case llvm::Instruction::FRem:
      case llvm::Instruction::Trunc:
      case llvm::Instruction::ZExt:
      case llvm::Instruction::SExt:
      case llvm::Instruction::FPToUI:
      case llvm::Instruction::FPToSI:
      case llvm::Instruction::UIToFP:
      case llvm::Instruction::SIToFP:
      case llvm::Instruction::FPTrunc:
      case llvm::Instruction::FPExt:
      case llvm::Instruction::PtrToInt:
      case llvm::Instruction::IntToPtr:
      case llvm::Instruction::Switch:
      case llvm::Instruction::FCmp:
        UNIMPLEMENTED(FATAL) << "Unimplemented llvm opcode: " << opcode; break;

      case llvm::Instruction::URem:
      case llvm::Instruction::UDiv:
      case llvm::Instruction::Resume:
      case llvm::Instruction::Unreachable:
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
        LOG(FATAL) << "Unknown llvm opcode: " << opcode; break;
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
    (void*)oatNew(cUnit, sizeof(LIR) * numBasicBlocks, true, kAllocLIR);
  LIR* labelList = (LIR*)cUnit->blockLabelList;
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
  oatAdjustSpillMask(cUnit);
  cUnit->frameSize = oatComputeFrameSize(cUnit);

  /*
   * At this point, we've lost all knowledge of register promotion.
   * Rebuild that info from the MethodInfo intrinsic (if it
   * exists - not required for correctness).
   */
  // TODO: find and recover MethodInfo.

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
