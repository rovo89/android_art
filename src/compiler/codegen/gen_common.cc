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

#include "oat/runtime/oat_support_entrypoints.h"
#include "../compiler_ir.h"
#include "ralloc_util.h"
#include "codegen_util.h"

namespace art {

//TODO: remove decl.
void GenInvoke(CompilationUnit* cu, CallInfo* info);

/*
 * This source files contains "gen" codegen routines that should
 * be applicable to most targets.  Only mid-level support utilities
 * and "op" calls may be used here.
 */

void MarkSafepointPC(CompilationUnit* cu, LIR* inst)
{
  inst->def_mask = ENCODE_ALL;
  LIR* safepoint_pc = NewLIR0(cu, kPseudoSafepointPC);
  DCHECK_EQ(safepoint_pc->def_mask, ENCODE_ALL);
}

/*
 * To save scheduling time, helper calls are broken into two parts: generation of
 * the helper target address, and the actuall call to the helper.  Because x86
 * has a memory call operation, part 1 is a NOP for x86.  For other targets,
 * load arguments between the two parts.
 */
int CallHelperSetup(CompilationUnit* cu, int helper_offset)
{
  return (cu->instruction_set == kX86) ? 0 : LoadHelper(cu, helper_offset);
}

/* NOTE: if r_tgt is a temp, it will be freed following use */
LIR* CallHelper(CompilationUnit* cu, int r_tgt, int helper_offset, bool safepoint_pc)
{
  LIR* call_inst;
  if (cu->instruction_set == kX86) {
    call_inst = OpThreadMem(cu, kOpBlx, helper_offset);
  } else {
    call_inst = OpReg(cu, kOpBlx, r_tgt);
    FreeTemp(cu, r_tgt);
  }
  if (safepoint_pc) {
    MarkSafepointPC(cu, call_inst);
  }
  return call_inst;
}

void CallRuntimeHelperImm(CompilationUnit* cu, int helper_offset, int arg0, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  LoadConstant(cu, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperReg(CompilationUnit* cu, int helper_offset, int arg0, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  OpRegCopy(cu, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperRegLocation(CompilationUnit* cu, int helper_offset, RegLocation arg0,
                                  bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  if (arg0.wide == 0) {
    LoadValueDirectFixed(cu, arg0, TargetReg(kArg0));
  } else {
    LoadValueDirectWideFixed(cu, arg0, TargetReg(kArg0), TargetReg(kArg1));
  }
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperImmImm(CompilationUnit* cu, int helper_offset, int arg0, int arg1,
                             bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  LoadConstant(cu, TargetReg(kArg0), arg0);
  LoadConstant(cu, TargetReg(kArg1), arg1);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperImmRegLocation(CompilationUnit* cu, int helper_offset, int arg0,
                                     RegLocation arg1, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  if (arg1.wide == 0) {
    LoadValueDirectFixed(cu, arg1, TargetReg(kArg1));
  } else {
    LoadValueDirectWideFixed(cu, arg1, TargetReg(kArg1), TargetReg(kArg2));
  }
  LoadConstant(cu, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperRegLocationImm(CompilationUnit* cu, int helper_offset, RegLocation arg0,
                                     int arg1, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  LoadValueDirectFixed(cu, arg0, TargetReg(kArg0));
  LoadConstant(cu, TargetReg(kArg1), arg1);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperImmReg(CompilationUnit* cu, int helper_offset, int arg0, int arg1,
                             bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  OpRegCopy(cu, TargetReg(kArg1), arg1);
  LoadConstant(cu, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperRegImm(CompilationUnit* cu, int helper_offset, int arg0, int arg1,
                             bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  OpRegCopy(cu, TargetReg(kArg0), arg0);
  LoadConstant(cu, TargetReg(kArg1), arg1);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperImmMethod(CompilationUnit* cu, int helper_offset, int arg0, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  LoadCurrMethodDirect(cu, TargetReg(kArg1));
  LoadConstant(cu, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperRegLocationRegLocation(CompilationUnit* cu, int helper_offset,
                                             RegLocation arg0, RegLocation arg1, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  if (arg0.wide == 0) {
    LoadValueDirectFixed(cu, arg0, arg0.fp ? TargetReg(kFArg0) : TargetReg(kArg0));
    if (arg1.wide == 0) {
      if (cu->instruction_set == kMips) {
        LoadValueDirectFixed(cu, arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg1));
      } else {
        LoadValueDirectFixed(cu, arg1, TargetReg(kArg1));
      }
    } else {
      if (cu->instruction_set == kMips) {
        LoadValueDirectWideFixed(cu, arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg1), arg1.fp ? TargetReg(kFArg3) : TargetReg(kArg2));
      } else {
        LoadValueDirectWideFixed(cu, arg1, TargetReg(kArg1), TargetReg(kArg2));
      }
    }
  } else {
    LoadValueDirectWideFixed(cu, arg0, arg0.fp ? TargetReg(kFArg0) : TargetReg(kArg0), arg0.fp ? TargetReg(kFArg1) : TargetReg(kArg1));
    if (arg1.wide == 0) {
      LoadValueDirectFixed(cu, arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg2));
    } else {
      LoadValueDirectWideFixed(cu, arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg2), arg1.fp ? TargetReg(kFArg3) : TargetReg(kArg3));
    }
  }
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperRegReg(CompilationUnit* cu, int helper_offset, int arg0, int arg1,
                             bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  DCHECK_NE(TargetReg(kArg0), arg1);  // check copy into arg0 won't clobber arg1
  OpRegCopy(cu, TargetReg(kArg0), arg0);
  OpRegCopy(cu, TargetReg(kArg1), arg1);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperRegRegImm(CompilationUnit* cu, int helper_offset, int arg0, int arg1,
                                int arg2, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  DCHECK_NE(TargetReg(kArg0), arg1);  // check copy into arg0 won't clobber arg1
  OpRegCopy(cu, TargetReg(kArg0), arg0);
  OpRegCopy(cu, TargetReg(kArg1), arg1);
  LoadConstant(cu, TargetReg(kArg2), arg2);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperImmMethodRegLocation(CompilationUnit* cu, int helper_offset, int arg0,
                                           RegLocation arg2, bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  LoadValueDirectFixed(cu, arg2, TargetReg(kArg2));
  LoadCurrMethodDirect(cu, TargetReg(kArg1));
  LoadConstant(cu, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperImmMethodImm(CompilationUnit* cu, int helper_offset, int arg0, int arg2,
                                   bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  LoadCurrMethodDirect(cu, TargetReg(kArg1));
  LoadConstant(cu, TargetReg(kArg2), arg2);
  LoadConstant(cu, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

void CallRuntimeHelperImmRegLocationRegLocation(CompilationUnit* cu, int helper_offset,
                                                int arg0, RegLocation arg1, RegLocation arg2,
                                                bool safepoint_pc) {
  int r_tgt = CallHelperSetup(cu, helper_offset);
  LoadValueDirectFixed(cu, arg1, TargetReg(kArg1));
  if (arg2.wide == 0) {
    LoadValueDirectFixed(cu, arg2, TargetReg(kArg2));
  } else {
    LoadValueDirectWideFixed(cu, arg2, TargetReg(kArg2), TargetReg(kArg3));
  }
  LoadConstant(cu, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cu);
  CallHelper(cu, r_tgt, helper_offset, safepoint_pc);
}

/*
 * Generate an kPseudoBarrier marker to indicate the boundary of special
 * blocks.
 */
void GenBarrier(CompilationUnit* cu)
{
  LIR* barrier = NewLIR0(cu, kPseudoBarrier);
  /* Mark all resources as being clobbered */
  barrier->def_mask = -1;
}


/* Generate unconditional branch instructions */
LIR* OpUnconditionalBranch(CompilationUnit* cu, LIR* target)
{
  LIR* branch = OpBranchUnconditional(cu, kOpUncondBr);
  branch->target = target;
  return branch;
}

// FIXME: need to do some work to split out targets with
// condition codes and those without
LIR* GenCheck(CompilationUnit* cu, ConditionCode c_code,
              ThrowKind kind)
{
  DCHECK_NE(cu->instruction_set, kMips);
  LIR* tgt = RawLIR(cu, 0, kPseudoThrowTarget, kind,
                    cu->current_dalvik_offset);
  LIR* branch = OpCondBranch(cu, c_code, tgt);
  // Remember branch target - will process later
  InsertGrowableList(cu, &cu->throw_launchpads, reinterpret_cast<uintptr_t>(tgt));
  return branch;
}

LIR* GenImmedCheck(CompilationUnit* cu, ConditionCode c_code,
                   int reg, int imm_val, ThrowKind kind)
{
  LIR* tgt = RawLIR(cu, 0, kPseudoThrowTarget, kind,
                    cu->current_dalvik_offset);
  LIR* branch;
  if (c_code == kCondAl) {
    branch = OpUnconditionalBranch(cu, tgt);
  } else {
    branch = OpCmpImmBranch(cu, c_code, reg, imm_val, tgt);
  }
  // Remember branch target - will process later
  InsertGrowableList(cu, &cu->throw_launchpads, reinterpret_cast<uintptr_t>(tgt));
  return branch;
}

/* Perform null-check on a register.  */
LIR* GenNullCheck(CompilationUnit* cu, int s_reg, int m_reg, int opt_flags)
{
  if (!(cu->disable_opt & (1 << kNullCheckElimination)) &&
    opt_flags & MIR_IGNORE_NULL_CHECK) {
    return NULL;
  }
  return GenImmedCheck(cu, kCondEq, m_reg, 0, kThrowNullPointer);
}

/* Perform check on two registers */
LIR* GenRegRegCheck(CompilationUnit* cu, ConditionCode c_code,
                    int reg1, int reg2, ThrowKind kind)
{
  LIR* tgt = RawLIR(cu, 0, kPseudoThrowTarget, kind,
                    cu->current_dalvik_offset, reg1, reg2);
  LIR* branch = OpCmpBranch(cu, c_code, reg1, reg2, tgt);
  // Remember branch target - will process later
  InsertGrowableList(cu, &cu->throw_launchpads, reinterpret_cast<uintptr_t>(tgt));
  return branch;
}

void GenCompareAndBranch(CompilationUnit* cu, Instruction::Code opcode,
                         RegLocation rl_src1, RegLocation rl_src2, LIR* taken,
                         LIR* fall_through)
{
  ConditionCode cond;
  rl_src1 = LoadValue(cu, rl_src1, kCoreReg);
  rl_src2 = LoadValue(cu, rl_src2, kCoreReg);
  switch (opcode) {
    case Instruction::IF_EQ:
      cond = kCondEq;
      break;
    case Instruction::IF_NE:
      cond = kCondNe;
      break;
    case Instruction::IF_LT:
      cond = kCondLt;
      break;
    case Instruction::IF_GE:
      cond = kCondGe;
      break;
    case Instruction::IF_GT:
      cond = kCondGt;
      break;
    case Instruction::IF_LE:
      cond = kCondLe;
      break;
    default:
      cond = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  OpCmpBranch(cu, cond, rl_src1.low_reg, rl_src2.low_reg, taken);
  OpUnconditionalBranch(cu, fall_through);
}

void GenCompareZeroAndBranch(CompilationUnit* cu, Instruction::Code opcode,
                             RegLocation rl_src, LIR* taken, LIR* fall_through)
{
  ConditionCode cond;
  rl_src = LoadValue(cu, rl_src, kCoreReg);
  switch (opcode) {
    case Instruction::IF_EQZ:
      cond = kCondEq;
      break;
    case Instruction::IF_NEZ:
      cond = kCondNe;
      break;
    case Instruction::IF_LTZ:
      cond = kCondLt;
      break;
    case Instruction::IF_GEZ:
      cond = kCondGe;
      break;
    case Instruction::IF_GTZ:
      cond = kCondGt;
      break;
    case Instruction::IF_LEZ:
      cond = kCondLe;
      break;
    default:
      cond = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  if (cu->instruction_set == kThumb2) {
    OpRegImm(cu, kOpCmp, rl_src.low_reg, 0);
    OpCondBranch(cu, cond, taken);
  } else {
    OpCmpImmBranch(cu, cond, rl_src.low_reg, 0, taken);
  }
  OpUnconditionalBranch(cu, fall_through);
}

void GenIntToLong(CompilationUnit* cu, RegLocation rl_dest,
                  RegLocation rl_src)
{
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  if (rl_src.location == kLocPhysReg) {
    OpRegCopy(cu, rl_result.low_reg, rl_src.low_reg);
  } else {
    LoadValueDirect(cu, rl_src, rl_result.low_reg);
  }
  OpRegRegImm(cu, kOpAsr, rl_result.high_reg, rl_result.low_reg, 31);
  StoreValueWide(cu, rl_dest, rl_result);
}

void GenIntNarrowing(CompilationUnit* cu, Instruction::Code opcode,
                     RegLocation rl_dest, RegLocation rl_src)
{
   rl_src = LoadValue(cu, rl_src, kCoreReg);
   RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
   OpKind op = kOpInvalid;
   switch (opcode) {
     case Instruction::INT_TO_BYTE:
       op = kOp2Byte;
       break;
     case Instruction::INT_TO_SHORT:
        op = kOp2Short;
        break;
     case Instruction::INT_TO_CHAR:
        op = kOp2Char;
        break;
     default:
       LOG(ERROR) << "Bad int conversion type";
   }
   OpRegReg(cu, op, rl_result.low_reg, rl_src.low_reg);
   StoreValue(cu, rl_dest, rl_result);
}

/*
 * Let helper function take care of everything.  Will call
 * Array::AllocFromCode(type_idx, method, count);
 * Note: AllocFromCode will handle checks for errNegativeArraySize.
 */
void GenNewArray(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest,
                 RegLocation rl_src)
{
  FlushAllRegs(cu);  /* Everything to home location */
  int func_offset;
  if (cu->compiler->CanAccessTypeWithoutChecks(cu->method_idx,
                                                  *cu->dex_file,
                                                  type_idx)) {
    func_offset = ENTRYPOINT_OFFSET(pAllocArrayFromCode);
  } else {
    func_offset= ENTRYPOINT_OFFSET(pAllocArrayFromCodeWithAccessCheck);
  }
  CallRuntimeHelperImmMethodRegLocation(cu, func_offset, type_idx, rl_src, true);
  RegLocation rl_result = GetReturn(cu, false);
  StoreValue(cu, rl_dest, rl_result);
}

/*
 * Similar to GenNewArray, but with post-allocation initialization.
 * Verifier guarantees we're dealing with an array class.  Current
 * code throws runtime exception "bad Filled array req" for 'D' and 'J'.
 * Current code also throws internal unimp if not 'L', '[' or 'I'.
 */
void GenFilledNewArray(CompilationUnit* cu, CallInfo* info)
{
  int elems = info->num_arg_words;
  int type_idx = info->index;
  FlushAllRegs(cu);  /* Everything to home location */
  int func_offset;
  if (cu->compiler->CanAccessTypeWithoutChecks(cu->method_idx,
                                                  *cu->dex_file,
                                                  type_idx)) {
    func_offset = ENTRYPOINT_OFFSET(pCheckAndAllocArrayFromCode);
  } else {
    func_offset = ENTRYPOINT_OFFSET(pCheckAndAllocArrayFromCodeWithAccessCheck);
  }
  CallRuntimeHelperImmMethodImm(cu, func_offset, type_idx, elems, true);
  FreeTemp(cu, TargetReg(kArg2));
  FreeTemp(cu, TargetReg(kArg1));
  /*
   * NOTE: the implicit target for Instruction::FILLED_NEW_ARRAY is the
   * return region.  Because AllocFromCode placed the new array
   * in kRet0, we'll just lock it into place.  When debugger support is
   * added, it may be necessary to additionally copy all return
   * values to a home location in thread-local storage
   */
  LockTemp(cu, TargetReg(kRet0));

  // TODO: use the correct component size, currently all supported types
  // share array alignment with ints (see comment at head of function)
  size_t component_size = sizeof(int32_t);

  // Having a range of 0 is legal
  if (info->is_range && (elems > 0)) {
    /*
     * Bit of ugliness here.  We're going generate a mem copy loop
     * on the register range, but it is possible that some regs
     * in the range have been promoted.  This is unlikely, but
     * before generating the copy, we'll just force a flush
     * of any regs in the source range that have been promoted to
     * home location.
     */
    for (int i = 0; i < elems; i++) {
      RegLocation loc = UpdateLoc(cu, info->args[i]);
      if (loc.location == kLocPhysReg) {
        StoreBaseDisp(cu, TargetReg(kSp), SRegOffset(cu, loc.s_reg_low),
                      loc.low_reg, kWord);
      }
    }
    /*
     * TUNING note: generated code here could be much improved, but
     * this is an uncommon operation and isn't especially performance
     * critical.
     */
    int r_src = AllocTemp(cu);
    int r_dst = AllocTemp(cu);
    int r_idx = AllocTemp(cu);
    int r_val = INVALID_REG;
    switch(cu->instruction_set) {
      case kThumb2:
        r_val = TargetReg(kLr);
        break;
      case kX86:
        FreeTemp(cu, TargetReg(kRet0));
        r_val = AllocTemp(cu);
        break;
      case kMips:
        r_val = AllocTemp(cu);
        break;
      default: LOG(FATAL) << "Unexpected instruction set: " << cu->instruction_set;
    }
    // Set up source pointer
    RegLocation rl_first = info->args[0];
    OpRegRegImm(cu, kOpAdd, r_src, TargetReg(kSp),
                SRegOffset(cu, rl_first.s_reg_low));
    // Set up the target pointer
    OpRegRegImm(cu, kOpAdd, r_dst, TargetReg(kRet0),
                Array::DataOffset(component_size).Int32Value());
    // Set up the loop counter (known to be > 0)
    LoadConstant(cu, r_idx, elems - 1);
    // Generate the copy loop.  Going backwards for convenience
    LIR* target = NewLIR0(cu, kPseudoTargetLabel);
    // Copy next element
    LoadBaseIndexed(cu, r_src, r_idx, r_val, 2, kWord);
    StoreBaseIndexed(cu, r_dst, r_idx, r_val, 2, kWord);
    FreeTemp(cu, r_val);
    OpDecAndBranch(cu, kCondGe, r_idx, target);
    if (cu->instruction_set == kX86) {
      // Restore the target pointer
      OpRegRegImm(cu, kOpAdd, TargetReg(kRet0), r_dst, -Array::DataOffset(component_size).Int32Value());
    }
  } else if (!info->is_range) {
    // TUNING: interleave
    for (int i = 0; i < elems; i++) {
      RegLocation rl_arg = LoadValue(cu, info->args[i], kCoreReg);
      StoreBaseDisp(cu, TargetReg(kRet0),
                    Array::DataOffset(component_size).Int32Value() +
                    i * 4, rl_arg.low_reg, kWord);
      // If the LoadValue caused a temp to be allocated, free it
      if (IsTemp(cu, rl_arg.low_reg)) {
        FreeTemp(cu, rl_arg.low_reg);
      }
    }
  }
  if (info->result.location != kLocInvalid) {
    StoreValue(cu, info->result, GetReturn(cu, false /* not fp */));
  }
}

void GenSput(CompilationUnit* cu, uint32_t field_idx, RegLocation rl_src,
       bool is_long_or_double, bool is_object)
{
  int field_offset;
  int ssb_index;
  bool is_volatile;
  bool is_referrers_class;

  OatCompilationUnit m_unit(cu->class_loader, cu->class_linker, *cu->dex_file,
                           cu->code_item, cu->method_idx, cu->access_flags);

  bool fast_path =
      cu->compiler->ComputeStaticFieldInfo(field_idx, &m_unit,
                                              field_offset, ssb_index,
                                              is_referrers_class, is_volatile,
                                              true);
  if (fast_path && !SLOW_FIELD_PATH) {
    DCHECK_GE(field_offset, 0);
    int rBase;
    if (is_referrers_class) {
      // Fast path, static storage base is this method's class
      RegLocation rl_method  = LoadCurrMethod(cu);
      rBase = AllocTemp(cu);
      LoadWordDisp(cu, rl_method.low_reg,
                   AbstractMethod::DeclaringClassOffset().Int32Value(), rBase);
      if (IsTemp(cu, rl_method.low_reg)) {
        FreeTemp(cu, rl_method.low_reg);
      }
    } else {
      // Medium path, static storage base in a different class which
      // requires checks that the other class is initialized.
      DCHECK_GE(ssb_index, 0);
      // May do runtime call so everything to home locations.
      FlushAllRegs(cu);
      // Using fixed register to sync with possible call to runtime
      // support.
      int r_method = TargetReg(kArg1);
      LockTemp(cu, r_method);
      LoadCurrMethodDirect(cu, r_method);
      rBase = TargetReg(kArg0);
      LockTemp(cu, rBase);
      LoadWordDisp(cu, r_method,
                   AbstractMethod::DexCacheInitializedStaticStorageOffset().Int32Value(),
                   rBase);
      LoadWordDisp(cu, rBase,
                   Array::DataOffset(sizeof(Object*)).Int32Value() +
                   sizeof(int32_t*) * ssb_index, rBase);
      // rBase now points at appropriate static storage base (Class*)
      // or NULL if not initialized. Check for NULL and call helper if NULL.
      // TUNING: fast path should fall through
      LIR* branch_over = OpCmpImmBranch(cu, kCondNe, rBase, 0, NULL);
      LoadConstant(cu, TargetReg(kArg0), ssb_index);
      CallRuntimeHelperImm(cu, ENTRYPOINT_OFFSET(pInitializeStaticStorage), ssb_index, true);
      if (cu->instruction_set == kMips) {
        // For Arm, kRet0 = kArg0 = rBase, for Mips, we need to copy
        OpRegCopy(cu, rBase, TargetReg(kRet0));
      }
      LIR* skip_target = NewLIR0(cu, kPseudoTargetLabel);
      branch_over->target = skip_target;
      FreeTemp(cu, r_method);
    }
    // rBase now holds static storage base
    if (is_long_or_double) {
      rl_src = LoadValueWide(cu, rl_src, kAnyReg);
    } else {
      rl_src = LoadValue(cu, rl_src, kAnyReg);
    }
    if (is_volatile) {
      GenMemBarrier(cu, kStoreStore);
    }
    if (is_long_or_double) {
      StoreBaseDispWide(cu, rBase, field_offset, rl_src.low_reg,
                        rl_src.high_reg);
    } else {
      StoreWordDisp(cu, rBase, field_offset, rl_src.low_reg);
    }
    if (is_volatile) {
      GenMemBarrier(cu, kStoreLoad);
    }
    if (is_object) {
      MarkGCCard(cu, rl_src.low_reg, rBase);
    }
    FreeTemp(cu, rBase);
  } else {
    FlushAllRegs(cu);  // Everything to home locations
    int setter_offset = is_long_or_double ? ENTRYPOINT_OFFSET(pSet64Static) :
        (is_object ? ENTRYPOINT_OFFSET(pSetObjStatic)
        : ENTRYPOINT_OFFSET(pSet32Static));
    CallRuntimeHelperImmRegLocation(cu, setter_offset, field_idx, rl_src, true);
  }
}

void GenSget(CompilationUnit* cu, uint32_t field_idx, RegLocation rl_dest,
       bool is_long_or_double, bool is_object)
{
  int field_offset;
  int ssb_index;
  bool is_volatile;
  bool is_referrers_class;

  OatCompilationUnit m_unit(cu->class_loader, cu->class_linker,
                           *cu->dex_file,
                           cu->code_item, cu->method_idx,
                           cu->access_flags);

  bool fast_path =
    cu->compiler->ComputeStaticFieldInfo(field_idx, &m_unit,
                                            field_offset, ssb_index,
                                            is_referrers_class, is_volatile,
                                            false);
  if (fast_path && !SLOW_FIELD_PATH) {
    DCHECK_GE(field_offset, 0);
    int rBase;
    if (is_referrers_class) {
      // Fast path, static storage base is this method's class
      RegLocation rl_method  = LoadCurrMethod(cu);
      rBase = AllocTemp(cu);
      LoadWordDisp(cu, rl_method.low_reg,
                   AbstractMethod::DeclaringClassOffset().Int32Value(), rBase);
    } else {
      // Medium path, static storage base in a different class which
      // requires checks that the other class is initialized
      DCHECK_GE(ssb_index, 0);
      // May do runtime call so everything to home locations.
      FlushAllRegs(cu);
      // Using fixed register to sync with possible call to runtime
      // support
      int r_method = TargetReg(kArg1);
      LockTemp(cu, r_method);
      LoadCurrMethodDirect(cu, r_method);
      rBase = TargetReg(kArg0);
      LockTemp(cu, rBase);
      LoadWordDisp(cu, r_method,
                   AbstractMethod::DexCacheInitializedStaticStorageOffset().Int32Value(),
                   rBase);
      LoadWordDisp(cu, rBase,
                   Array::DataOffset(sizeof(Object*)).Int32Value() +
                   sizeof(int32_t*) * ssb_index, rBase);
      // rBase now points at appropriate static storage base (Class*)
      // or NULL if not initialized. Check for NULL and call helper if NULL.
      // TUNING: fast path should fall through
      LIR* branch_over = OpCmpImmBranch(cu, kCondNe, rBase, 0, NULL);
      CallRuntimeHelperImm(cu, ENTRYPOINT_OFFSET(pInitializeStaticStorage), ssb_index, true);
      if (cu->instruction_set == kMips) {
        // For Arm, kRet0 = kArg0 = rBase, for Mips, we need to copy
        OpRegCopy(cu, rBase, TargetReg(kRet0));
      }
      LIR* skip_target = NewLIR0(cu, kPseudoTargetLabel);
      branch_over->target = skip_target;
      FreeTemp(cu, r_method);
    }
    // rBase now holds static storage base
    RegLocation rl_result = EvalLoc(cu, rl_dest, kAnyReg, true);
    if (is_volatile) {
      GenMemBarrier(cu, kLoadLoad);
    }
    if (is_long_or_double) {
      LoadBaseDispWide(cu, rBase, field_offset, rl_result.low_reg,
                       rl_result.high_reg, INVALID_SREG);
    } else {
      LoadWordDisp(cu, rBase, field_offset, rl_result.low_reg);
    }
    FreeTemp(cu, rBase);
    if (is_long_or_double) {
      StoreValueWide(cu, rl_dest, rl_result);
    } else {
      StoreValue(cu, rl_dest, rl_result);
    }
  } else {
    FlushAllRegs(cu);  // Everything to home locations
    int getterOffset = is_long_or_double ? ENTRYPOINT_OFFSET(pGet64Static) :
        (is_object ? ENTRYPOINT_OFFSET(pGetObjStatic)
        : ENTRYPOINT_OFFSET(pGet32Static));
    CallRuntimeHelperImm(cu, getterOffset, field_idx, true);
    if (is_long_or_double) {
      RegLocation rl_result = GetReturnWide(cu, rl_dest.fp);
      StoreValueWide(cu, rl_dest, rl_result);
    } else {
      RegLocation rl_result = GetReturn(cu, rl_dest.fp);
      StoreValue(cu, rl_dest, rl_result);
    }
  }
}


// Debugging routine - if null target, branch to DebugMe
void GenShowTarget(CompilationUnit* cu)
{
  DCHECK_NE(cu->instruction_set, kX86) << "unimplemented GenShowTarget";
  LIR* branch_over = OpCmpImmBranch(cu, kCondNe, TargetReg(kInvokeTgt), 0, NULL);
  LoadWordDisp(cu, TargetReg(kSelf), ENTRYPOINT_OFFSET(pDebugMe), TargetReg(kInvokeTgt));
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
  branch_over->target = target;
}

void HandleSuspendLaunchPads(CompilationUnit *cu)
{
  LIR** suspend_label = reinterpret_cast<LIR**>(cu->suspend_launchpads.elem_list);
  int num_elems = cu->suspend_launchpads.num_used;
  int helper_offset = ENTRYPOINT_OFFSET(pTestSuspendFromCode);
  for (int i = 0; i < num_elems; i++) {
    ResetRegPool(cu);
    ResetDefTracking(cu);
    LIR* lab = suspend_label[i];
    LIR* resume_lab = reinterpret_cast<LIR*>(lab->operands[0]);
    cu->current_dalvik_offset = lab->operands[1];
    AppendLIR(cu, lab);
    int r_tgt = CallHelperSetup(cu, helper_offset);
    CallHelper(cu, r_tgt, helper_offset, true /* MarkSafepointPC */);
    OpUnconditionalBranch(cu, resume_lab);
  }
}

void HandleIntrinsicLaunchPads(CompilationUnit *cu)
{
  LIR** intrinsic_label = reinterpret_cast<LIR**>(cu->intrinsic_launchpads.elem_list);
  int num_elems = cu->intrinsic_launchpads.num_used;
  for (int i = 0; i < num_elems; i++) {
    ResetRegPool(cu);
    ResetDefTracking(cu);
    LIR* lab = intrinsic_label[i];
    CallInfo* info = reinterpret_cast<CallInfo*>(lab->operands[0]);
    cu->current_dalvik_offset = info->offset;
    AppendLIR(cu, lab);
    // NOTE: GenInvoke handles MarkSafepointPC
    GenInvoke(cu, info);
    LIR* resume_lab = reinterpret_cast<LIR*>(lab->operands[2]);
    if (resume_lab != NULL) {
      OpUnconditionalBranch(cu, resume_lab);
    }
  }
}

void HandleThrowLaunchPads(CompilationUnit *cu)
{
  LIR** throw_label = reinterpret_cast<LIR**>(cu->throw_launchpads.elem_list);
  int num_elems = cu->throw_launchpads.num_used;
  for (int i = 0; i < num_elems; i++) {
    ResetRegPool(cu);
    ResetDefTracking(cu);
    LIR* lab = throw_label[i];
    cu->current_dalvik_offset = lab->operands[1];
    AppendLIR(cu, lab);
    int func_offset = 0;
    int v1 = lab->operands[2];
    int v2 = lab->operands[3];
    bool target_x86 = (cu->instruction_set == kX86);
    switch (lab->operands[0]) {
      case kThrowNullPointer:
        func_offset = ENTRYPOINT_OFFSET(pThrowNullPointerFromCode);
        break;
      case kThrowArrayBounds:
        // Move v1 (array index) to kArg0 and v2 (array length) to kArg1
        if (v2 != TargetReg(kArg0)) {
          OpRegCopy(cu, TargetReg(kArg0), v1);
          if (target_x86) {
            // x86 leaves the array pointer in v2, so load the array length that the handler expects
            OpRegMem(cu, kOpMov, TargetReg(kArg1), v2, Array::LengthOffset().Int32Value());
          } else {
            OpRegCopy(cu, TargetReg(kArg1), v2);
          }
        } else {
          if (v1 == TargetReg(kArg1)) {
            // Swap v1 and v2, using kArg2 as a temp
            OpRegCopy(cu, TargetReg(kArg2), v1);
            if (target_x86) {
              // x86 leaves the array pointer in v2; load the array length that the handler expects
              OpRegMem(cu, kOpMov, TargetReg(kArg1), v2, Array::LengthOffset().Int32Value());
            } else {
              OpRegCopy(cu, TargetReg(kArg1), v2);
            }
            OpRegCopy(cu, TargetReg(kArg0), TargetReg(kArg2));
          } else {
            if (target_x86) {
              // x86 leaves the array pointer in v2; load the array length that the handler expects
              OpRegMem(cu, kOpMov, TargetReg(kArg1), v2, Array::LengthOffset().Int32Value());
            } else {
              OpRegCopy(cu, TargetReg(kArg1), v2);
            }
            OpRegCopy(cu, TargetReg(kArg0), v1);
          }
        }
        func_offset = ENTRYPOINT_OFFSET(pThrowArrayBoundsFromCode);
        break;
      case kThrowDivZero:
        func_offset = ENTRYPOINT_OFFSET(pThrowDivZeroFromCode);
        break;
      case kThrowNoSuchMethod:
        OpRegCopy(cu, TargetReg(kArg0), v1);
        func_offset =
          ENTRYPOINT_OFFSET(pThrowNoSuchMethodFromCode);
        break;
      case kThrowStackOverflow:
        func_offset = ENTRYPOINT_OFFSET(pThrowStackOverflowFromCode);
        // Restore stack alignment
        if (target_x86) {
          OpRegImm(cu, kOpAdd, TargetReg(kSp), cu->frame_size);
        } else {
          OpRegImm(cu, kOpAdd, TargetReg(kSp), (cu->num_core_spills + cu->num_fp_spills) * 4);
        }
        break;
      default:
        LOG(FATAL) << "Unexpected throw kind: " << lab->operands[0];
    }
    ClobberCalleeSave(cu);
    int r_tgt = CallHelperSetup(cu, func_offset);
    CallHelper(cu, r_tgt, func_offset, true /* MarkSafepointPC */);
  }
}

bool FastInstance(CompilationUnit* cu,  uint32_t field_idx,
                  int& field_offset, bool& is_volatile, bool is_put)
{
  OatCompilationUnit m_unit(cu->class_loader, cu->class_linker,
               *cu->dex_file,
               cu->code_item, cu->method_idx,
               cu->access_flags);
  return cu->compiler->ComputeInstanceFieldInfo(field_idx, &m_unit,
           field_offset, is_volatile, is_put);
}

void GenIGet(CompilationUnit* cu, uint32_t field_idx, int opt_flags, OpSize size,
             RegLocation rl_dest, RegLocation rl_obj,
             bool is_long_or_double, bool is_object)
{
  int field_offset;
  bool is_volatile;

  bool fast_path = FastInstance(cu, field_idx, field_offset, is_volatile, false);

  if (fast_path && !SLOW_FIELD_PATH) {
    RegLocation rl_result;
    RegisterClass reg_class = oat_reg_class_by_size(size);
    DCHECK_GE(field_offset, 0);
    rl_obj = LoadValue(cu, rl_obj, kCoreReg);
    if (is_long_or_double) {
      DCHECK(rl_dest.wide);
      GenNullCheck(cu, rl_obj.s_reg_low, rl_obj.low_reg, opt_flags);
      if (cu->instruction_set == kX86) {
        rl_result = EvalLoc(cu, rl_dest, reg_class, true);
        GenNullCheck(cu, rl_obj.s_reg_low, rl_obj.low_reg, opt_flags);
        LoadBaseDispWide(cu, rl_obj.low_reg, field_offset, rl_result.low_reg,
                         rl_result.high_reg, rl_obj.s_reg_low);
        if (is_volatile) {
          GenMemBarrier(cu, kLoadLoad);
        }
      } else {
        int reg_ptr = AllocTemp(cu);
        OpRegRegImm(cu, kOpAdd, reg_ptr, rl_obj.low_reg, field_offset);
        rl_result = EvalLoc(cu, rl_dest, reg_class, true);
        LoadPair(cu, reg_ptr, rl_result.low_reg, rl_result.high_reg);
        if (is_volatile) {
          GenMemBarrier(cu, kLoadLoad);
        }
        FreeTemp(cu, reg_ptr);
      }
      StoreValueWide(cu, rl_dest, rl_result);
    } else {
      rl_result = EvalLoc(cu, rl_dest, reg_class, true);
      GenNullCheck(cu, rl_obj.s_reg_low, rl_obj.low_reg, opt_flags);
      LoadBaseDisp(cu, rl_obj.low_reg, field_offset, rl_result.low_reg,
                   kWord, rl_obj.s_reg_low);
      if (is_volatile) {
        GenMemBarrier(cu, kLoadLoad);
      }
      StoreValue(cu, rl_dest, rl_result);
    }
  } else {
    int getterOffset = is_long_or_double ? ENTRYPOINT_OFFSET(pGet64Instance) :
        (is_object ? ENTRYPOINT_OFFSET(pGetObjInstance)
        : ENTRYPOINT_OFFSET(pGet32Instance));
    CallRuntimeHelperImmRegLocation(cu, getterOffset, field_idx, rl_obj, true);
    if (is_long_or_double) {
      RegLocation rl_result = GetReturnWide(cu, rl_dest.fp);
      StoreValueWide(cu, rl_dest, rl_result);
    } else {
      RegLocation rl_result = GetReturn(cu, rl_dest.fp);
      StoreValue(cu, rl_dest, rl_result);
    }
  }
}

void GenIPut(CompilationUnit* cu, uint32_t field_idx, int opt_flags, OpSize size,
             RegLocation rl_src, RegLocation rl_obj, bool is_long_or_double, bool is_object)
{
  int field_offset;
  bool is_volatile;

  bool fast_path = FastInstance(cu, field_idx, field_offset, is_volatile,
                 true);
  if (fast_path && !SLOW_FIELD_PATH) {
    RegisterClass reg_class = oat_reg_class_by_size(size);
    DCHECK_GE(field_offset, 0);
    rl_obj = LoadValue(cu, rl_obj, kCoreReg);
    if (is_long_or_double) {
      int reg_ptr;
      rl_src = LoadValueWide(cu, rl_src, kAnyReg);
      GenNullCheck(cu, rl_obj.s_reg_low, rl_obj.low_reg, opt_flags);
      reg_ptr = AllocTemp(cu);
      OpRegRegImm(cu, kOpAdd, reg_ptr, rl_obj.low_reg, field_offset);
      if (is_volatile) {
        GenMemBarrier(cu, kStoreStore);
      }
      StoreBaseDispWide(cu, reg_ptr, 0, rl_src.low_reg, rl_src.high_reg);
      if (is_volatile) {
        GenMemBarrier(cu, kLoadLoad);
      }
      FreeTemp(cu, reg_ptr);
    } else {
      rl_src = LoadValue(cu, rl_src, reg_class);
      GenNullCheck(cu, rl_obj.s_reg_low, rl_obj.low_reg, opt_flags);
      if (is_volatile) {
        GenMemBarrier(cu, kStoreStore);
      }
      StoreBaseDisp(cu, rl_obj.low_reg, field_offset, rl_src.low_reg, kWord);
      if (is_volatile) {
        GenMemBarrier(cu, kLoadLoad);
      }
      if (is_object) {
        MarkGCCard(cu, rl_src.low_reg, rl_obj.low_reg);
      }
    }
  } else {
    int setter_offset = is_long_or_double ? ENTRYPOINT_OFFSET(pSet64Instance) :
        (is_object ? ENTRYPOINT_OFFSET(pSetObjInstance)
        : ENTRYPOINT_OFFSET(pSet32Instance));
    CallRuntimeHelperImmRegLocationRegLocation(cu, setter_offset, field_idx, rl_obj, rl_src, true);
  }
}

void GenConstClass(CompilationUnit* cu, uint32_t type_idx,
                   RegLocation rl_dest)
{
  RegLocation rl_method = LoadCurrMethod(cu);
  int res_reg = AllocTemp(cu);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  if (!cu->compiler->CanAccessTypeWithoutChecks(cu->method_idx,
                                                   *cu->dex_file,
                                                   type_idx)) {
    // Call out to helper which resolves type and verifies access.
    // Resolved type returned in kRet0.
    CallRuntimeHelperImmReg(cu, ENTRYPOINT_OFFSET(pInitializeTypeAndVerifyAccessFromCode),
                            type_idx, rl_method.low_reg, true);
    RegLocation rl_result = GetReturn(cu, false);
    StoreValue(cu, rl_dest, rl_result);
  } else {
    // We're don't need access checks, load type from dex cache
    int32_t dex_cache_offset =
        AbstractMethod::DexCacheResolvedTypesOffset().Int32Value();
    LoadWordDisp(cu, rl_method.low_reg, dex_cache_offset, res_reg);
    int32_t offset_of_type =
        Array::DataOffset(sizeof(Class*)).Int32Value() + (sizeof(Class*)
                          * type_idx);
    LoadWordDisp(cu, res_reg, offset_of_type, rl_result.low_reg);
    if (!cu->compiler->CanAssumeTypeIsPresentInDexCache(*cu->dex_file,
        type_idx) || SLOW_TYPE_PATH) {
      // Slow path, at runtime test if type is null and if so initialize
      FlushAllRegs(cu);
      LIR* branch1 = OpCmpImmBranch(cu, kCondEq, rl_result.low_reg, 0, NULL);
      // Resolved, store and hop over following code
      StoreValue(cu, rl_dest, rl_result);
      /*
       * Because we have stores of the target value on two paths,
       * clobber temp tracking for the destination using the ssa name
       */
      ClobberSReg(cu, rl_dest.s_reg_low);
      LIR* branch2 = OpUnconditionalBranch(cu,0);
      // TUNING: move slow path to end & remove unconditional branch
      LIR* target1 = NewLIR0(cu, kPseudoTargetLabel);
      // Call out to helper, which will return resolved type in kArg0
      CallRuntimeHelperImmReg(cu, ENTRYPOINT_OFFSET(pInitializeTypeFromCode), type_idx,
                              rl_method.low_reg, true);
      RegLocation rl_result = GetReturn(cu, false);
      StoreValue(cu, rl_dest, rl_result);
      /*
       * Because we have stores of the target value on two paths,
       * clobber temp tracking for the destination using the ssa name
       */
      ClobberSReg(cu, rl_dest.s_reg_low);
      // Rejoin code paths
      LIR* target2 = NewLIR0(cu, kPseudoTargetLabel);
      branch1->target = target1;
      branch2->target = target2;
    } else {
      // Fast path, we're done - just store result
      StoreValue(cu, rl_dest, rl_result);
    }
  }
}

void GenConstString(CompilationUnit* cu, uint32_t string_idx,
                    RegLocation rl_dest)
{
  /* NOTE: Most strings should be available at compile time */
  int32_t offset_of_string = Array::DataOffset(sizeof(String*)).Int32Value() +
                 (sizeof(String*) * string_idx);
  if (!cu->compiler->CanAssumeStringIsPresentInDexCache(
      *cu->dex_file, string_idx) || SLOW_STRING_PATH) {
    // slow path, resolve string if not in dex cache
    FlushAllRegs(cu);
    LockCallTemps(cu); // Using explicit registers
    LoadCurrMethodDirect(cu, TargetReg(kArg2));
    LoadWordDisp(cu, TargetReg(kArg2),
                 AbstractMethod::DexCacheStringsOffset().Int32Value(), TargetReg(kArg0));
    // Might call out to helper, which will return resolved string in kRet0
    int r_tgt = CallHelperSetup(cu, ENTRYPOINT_OFFSET(pResolveStringFromCode));
    LoadWordDisp(cu, TargetReg(kArg0), offset_of_string, TargetReg(kRet0));
    LoadConstant(cu, TargetReg(kArg1), string_idx);
    if (cu->instruction_set == kThumb2) {
      OpRegImm(cu, kOpCmp, TargetReg(kRet0), 0);  // Is resolved?
      GenBarrier(cu);
      // For testing, always force through helper
      if (!EXERCISE_SLOWEST_STRING_PATH) {
        OpIT(cu, kArmCondEq, "T");
      }
      OpRegCopy(cu, TargetReg(kArg0), TargetReg(kArg2));   // .eq
      LIR* call_inst = OpReg(cu, kOpBlx, r_tgt);    // .eq, helper(Method*, string_idx)
      MarkSafepointPC(cu, call_inst);
      FreeTemp(cu, r_tgt);
    } else if (cu->instruction_set == kMips) {
      LIR* branch = OpCmpImmBranch(cu, kCondNe, TargetReg(kRet0), 0, NULL);
      OpRegCopy(cu, TargetReg(kArg0), TargetReg(kArg2));   // .eq
      LIR* call_inst = OpReg(cu, kOpBlx, r_tgt);
      MarkSafepointPC(cu, call_inst);
      FreeTemp(cu, r_tgt);
      LIR* target = NewLIR0(cu, kPseudoTargetLabel);
      branch->target = target;
    } else {
      DCHECK_EQ(cu->instruction_set, kX86);
      CallRuntimeHelperRegReg(cu, ENTRYPOINT_OFFSET(pResolveStringFromCode), TargetReg(kArg2), TargetReg(kArg1), true);
    }
    GenBarrier(cu);
    StoreValue(cu, rl_dest, GetReturn(cu, false));
  } else {
    RegLocation rl_method = LoadCurrMethod(cu);
    int res_reg = AllocTemp(cu);
    RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
    LoadWordDisp(cu, rl_method.low_reg,
                 AbstractMethod::DexCacheStringsOffset().Int32Value(), res_reg);
    LoadWordDisp(cu, res_reg, offset_of_string, rl_result.low_reg);
    StoreValue(cu, rl_dest, rl_result);
  }
}

/*
 * Let helper function take care of everything.  Will
 * call Class::NewInstanceFromCode(type_idx, method);
 */
void GenNewInstance(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest)
{
  FlushAllRegs(cu);  /* Everything to home location */
  // alloc will always check for resolution, do we also need to verify
  // access because the verifier was unable to?
  int func_offset;
  if (cu->compiler->CanAccessInstantiableTypeWithoutChecks(
      cu->method_idx, *cu->dex_file, type_idx)) {
    func_offset = ENTRYPOINT_OFFSET(pAllocObjectFromCode);
  } else {
    func_offset = ENTRYPOINT_OFFSET(pAllocObjectFromCodeWithAccessCheck);
  }
  CallRuntimeHelperImmMethod(cu, func_offset, type_idx, true);
  RegLocation rl_result = GetReturn(cu, false);
  StoreValue(cu, rl_dest, rl_result);
}

void GenMoveException(CompilationUnit* cu, RegLocation rl_dest)
{
  FlushAllRegs(cu);  /* Everything to home location */
  int func_offset = ENTRYPOINT_OFFSET(pGetAndClearException);
  if (cu->instruction_set == kX86) {
    // Runtime helper will load argument for x86.
    CallRuntimeHelperReg(cu, func_offset, TargetReg(kArg0), false);
  } else {
    CallRuntimeHelperReg(cu, func_offset, TargetReg(kSelf), false);
  }
  RegLocation rl_result = GetReturn(cu, false);
  StoreValue(cu, rl_dest, rl_result);
}

void GenThrow(CompilationUnit* cu, RegLocation rl_src)
{
  FlushAllRegs(cu);
  CallRuntimeHelperRegLocation(cu, ENTRYPOINT_OFFSET(pDeliverException), rl_src, true);
}

void GenInstanceof(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest,
                   RegLocation rl_src)
{
  FlushAllRegs(cu);
  // May generate a call - use explicit registers
  LockCallTemps(cu);
  LoadCurrMethodDirect(cu, TargetReg(kArg1));  // kArg1 <= current Method*
  int class_reg = TargetReg(kArg2);  // kArg2 will hold the Class*
  if (!cu->compiler->CanAccessTypeWithoutChecks(cu->method_idx,
                                                   *cu->dex_file,
                                                   type_idx)) {
    // Check we have access to type_idx and if not throw IllegalAccessError,
    // returns Class* in kArg0
    CallRuntimeHelperImm(cu, ENTRYPOINT_OFFSET(pInitializeTypeAndVerifyAccessFromCode),
                         type_idx, true);
    OpRegCopy(cu, class_reg, TargetReg(kRet0));  // Align usage with fast path
    LoadValueDirectFixed(cu, rl_src, TargetReg(kArg0));  // kArg0 <= ref
  } else {
    // Load dex cache entry into class_reg (kArg2)
    LoadValueDirectFixed(cu, rl_src, TargetReg(kArg0));  // kArg0 <= ref
    LoadWordDisp(cu, TargetReg(kArg1),
                 AbstractMethod::DexCacheResolvedTypesOffset().Int32Value(), class_reg);
    int32_t offset_of_type =
        Array::DataOffset(sizeof(Class*)).Int32Value() + (sizeof(Class*)
        * type_idx);
    LoadWordDisp(cu, class_reg, offset_of_type, class_reg);
    if (!cu->compiler->CanAssumeTypeIsPresentInDexCache(
        *cu->dex_file, type_idx)) {
      // Need to test presence of type in dex cache at runtime
      LIR* hop_branch = OpCmpImmBranch(cu, kCondNe, class_reg, 0, NULL);
      // Not resolved
      // Call out to helper, which will return resolved type in kRet0
      CallRuntimeHelperImm(cu, ENTRYPOINT_OFFSET(pInitializeTypeFromCode), type_idx, true);
      OpRegCopy(cu, TargetReg(kArg2), TargetReg(kRet0)); // Align usage with fast path
      LoadValueDirectFixed(cu, rl_src, TargetReg(kArg0));  /* reload Ref */
      // Rejoin code paths
      LIR* hop_target = NewLIR0(cu, kPseudoTargetLabel);
      hop_branch->target = hop_target;
    }
  }
  /* kArg0 is ref, kArg2 is class. If ref==null, use directly as bool result */
  RegLocation rl_result = GetReturn(cu, false);
  if (cu->instruction_set == kMips) {
    LoadConstant(cu, rl_result.low_reg, 0);  // store false result for if branch is taken
  }
  LIR* branch1 = OpCmpImmBranch(cu, kCondEq, TargetReg(kArg0), 0, NULL);
  /* load object->klass_ */
  DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);
  LoadWordDisp(cu, TargetReg(kArg0),  Object::ClassOffset().Int32Value(), TargetReg(kArg1));
  /* kArg0 is ref, kArg1 is ref->klass_, kArg2 is class */
  LIR* call_inst;
  LIR* branchover = NULL;
  if (cu->instruction_set == kThumb2) {
    /* Uses conditional nullification */
    int r_tgt = LoadHelper(cu, ENTRYPOINT_OFFSET(pInstanceofNonTrivialFromCode));
    OpRegReg(cu, kOpCmp, TargetReg(kArg1), TargetReg(kArg2));  // Same?
    OpIT(cu, kArmCondEq, "EE");   // if-convert the test
    LoadConstant(cu, TargetReg(kArg0), 1);     // .eq case - load true
    OpRegCopy(cu, TargetReg(kArg0), TargetReg(kArg2));    // .ne case - arg0 <= class
    call_inst = OpReg(cu, kOpBlx, r_tgt);    // .ne case: helper(class, ref->class)
    FreeTemp(cu, r_tgt);
  } else {
    /* Uses branchovers */
    LoadConstant(cu, rl_result.low_reg, 1);     // assume true
    branchover = OpCmpBranch(cu, kCondEq, TargetReg(kArg1), TargetReg(kArg2), NULL);
    if (cu->instruction_set != kX86) {
      int r_tgt = LoadHelper(cu, ENTRYPOINT_OFFSET(pInstanceofNonTrivialFromCode));
      OpRegCopy(cu, TargetReg(kArg0), TargetReg(kArg2));    // .ne case - arg0 <= class
      call_inst = OpReg(cu, kOpBlx, r_tgt);    // .ne case: helper(class, ref->class)
      FreeTemp(cu, r_tgt);
    } else {
      OpRegCopy(cu, TargetReg(kArg0), TargetReg(kArg2));
      call_inst = OpThreadMem(cu, kOpBlx, ENTRYPOINT_OFFSET(pInstanceofNonTrivialFromCode));
    }
  }
  MarkSafepointPC(cu, call_inst);
  ClobberCalleeSave(cu);
  /* branch targets here */
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
  StoreValue(cu, rl_dest, rl_result);
  branch1->target = target;
  if (cu->instruction_set != kThumb2) {
    branchover->target = target;
  }
}

void GenCheckCast(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_src)
{
  FlushAllRegs(cu);
  // May generate a call - use explicit registers
  LockCallTemps(cu);
  LoadCurrMethodDirect(cu, TargetReg(kArg1));  // kArg1 <= current Method*
  int class_reg = TargetReg(kArg2);  // kArg2 will hold the Class*
  if (!cu->compiler->CanAccessTypeWithoutChecks(cu->method_idx,
                                                   *cu->dex_file,
                                                   type_idx)) {
    // Check we have access to type_idx and if not throw IllegalAccessError,
    // returns Class* in kRet0
    // InitializeTypeAndVerifyAccess(idx, method)
    CallRuntimeHelperImmReg(cu, ENTRYPOINT_OFFSET(pInitializeTypeAndVerifyAccessFromCode),
                            type_idx, TargetReg(kArg1), true);
    OpRegCopy(cu, class_reg, TargetReg(kRet0));  // Align usage with fast path
  } else {
    // Load dex cache entry into class_reg (kArg2)
    LoadWordDisp(cu, TargetReg(kArg1),
                 AbstractMethod::DexCacheResolvedTypesOffset().Int32Value(), class_reg);
    int32_t offset_of_type =
        Array::DataOffset(sizeof(Class*)).Int32Value() +
        (sizeof(Class*) * type_idx);
    LoadWordDisp(cu, class_reg, offset_of_type, class_reg);
    if (!cu->compiler->CanAssumeTypeIsPresentInDexCache(
        *cu->dex_file, type_idx)) {
      // Need to test presence of type in dex cache at runtime
      LIR* hop_branch = OpCmpImmBranch(cu, kCondNe, class_reg, 0, NULL);
      // Not resolved
      // Call out to helper, which will return resolved type in kArg0
      // InitializeTypeFromCode(idx, method)
      CallRuntimeHelperImmReg(cu, ENTRYPOINT_OFFSET(pInitializeTypeFromCode), type_idx, TargetReg(kArg1),
                              true);
      OpRegCopy(cu, class_reg, TargetReg(kRet0)); // Align usage with fast path
      // Rejoin code paths
      LIR* hop_target = NewLIR0(cu, kPseudoTargetLabel);
      hop_branch->target = hop_target;
    }
  }
  // At this point, class_reg (kArg2) has class
  LoadValueDirectFixed(cu, rl_src, TargetReg(kArg0));  // kArg0 <= ref
  /* Null is OK - continue */
  LIR* branch1 = OpCmpImmBranch(cu, kCondEq, TargetReg(kArg0), 0, NULL);
  /* load object->klass_ */
  DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);
  LoadWordDisp(cu, TargetReg(kArg0),  Object::ClassOffset().Int32Value(), TargetReg(kArg1));
  /* kArg1 now contains object->klass_ */
  LIR* branch2;
  if (cu->instruction_set == kThumb2) {
    int r_tgt = LoadHelper(cu, ENTRYPOINT_OFFSET(pCheckCastFromCode));
    OpRegReg(cu, kOpCmp, TargetReg(kArg1), class_reg);
    branch2 = OpCondBranch(cu, kCondEq, NULL); /* If eq, trivial yes */
    OpRegCopy(cu, TargetReg(kArg0), TargetReg(kArg1));
    OpRegCopy(cu, TargetReg(kArg1), TargetReg(kArg2));
    ClobberCalleeSave(cu);
    LIR* call_inst = OpReg(cu, kOpBlx, r_tgt);
    MarkSafepointPC(cu, call_inst);
    FreeTemp(cu, r_tgt);
  } else {
    branch2 = OpCmpBranch(cu, kCondEq, TargetReg(kArg1), class_reg, NULL);
    CallRuntimeHelperRegReg(cu, ENTRYPOINT_OFFSET(pCheckCastFromCode), TargetReg(kArg1), TargetReg(kArg2), true);
  }
  /* branch target here */
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
  branch1->target = target;
  branch2->target = target;
}

/*
 * Generate array store
 *
 */
void GenArrayObjPut(CompilationUnit* cu, int opt_flags, RegLocation rl_array,
          RegLocation rl_index, RegLocation rl_src, int scale)
{
  int len_offset = Array::LengthOffset().Int32Value();
  int data_offset = Array::DataOffset(sizeof(Object*)).Int32Value();

  FlushAllRegs(cu);  // Use explicit registers
  LockCallTemps(cu);

  int r_value = TargetReg(kArg0);  // Register holding value
  int r_array_class = TargetReg(kArg1);  // Register holding array's Class
  int r_array = TargetReg(kArg2);  // Register holding array
  int r_index = TargetReg(kArg3);  // Register holding index into array

  LoadValueDirectFixed(cu, rl_array, r_array);  // Grab array
  LoadValueDirectFixed(cu, rl_src, r_value);  // Grab value
  LoadValueDirectFixed(cu, rl_index, r_index);  // Grab index

  GenNullCheck(cu, rl_array.s_reg_low, r_array, opt_flags);  // NPE?

  // Store of null?
  LIR* null_value_check = OpCmpImmBranch(cu, kCondEq, r_value, 0, NULL);

  // Get the array's class.
  LoadWordDisp(cu, r_array, Object::ClassOffset().Int32Value(), r_array_class);
  CallRuntimeHelperRegReg(cu, ENTRYPOINT_OFFSET(pCanPutArrayElementFromCode), r_value,
                          r_array_class, true);
  // Redo LoadValues in case they didn't survive the call.
  LoadValueDirectFixed(cu, rl_array, r_array);  // Reload array
  LoadValueDirectFixed(cu, rl_index, r_index);  // Reload index
  LoadValueDirectFixed(cu, rl_src, r_value);  // Reload value
  r_array_class = INVALID_REG;

  // Branch here if value to be stored == null
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
  null_value_check->target = target;

  if (cu->instruction_set == kX86) {
    // make an extra temp available for card mark below
    FreeTemp(cu, TargetReg(kArg1));
    if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
      /* if (rl_index >= [rl_array + len_offset]) goto kThrowArrayBounds */
      GenRegMemCheck(cu, kCondUge, r_index, r_array, len_offset, kThrowArrayBounds);
    }
    StoreBaseIndexedDisp(cu, r_array, r_index, scale,
                         data_offset, r_value, INVALID_REG, kWord, INVALID_SREG);
  } else {
    bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
    int reg_len = INVALID_REG;
    if (needs_range_check) {
      reg_len = TargetReg(kArg1);
      LoadWordDisp(cu, r_array, len_offset, reg_len);  // Get len
    }
    /* r_ptr -> array data */
    int r_ptr = AllocTemp(cu);
    OpRegRegImm(cu, kOpAdd, r_ptr, r_array, data_offset);
    if (needs_range_check) {
      GenRegRegCheck(cu, kCondCs, r_index, reg_len, kThrowArrayBounds);
    }
    StoreBaseIndexed(cu, r_ptr, r_index, r_value, scale, kWord);
    FreeTemp(cu, r_ptr);
  }
  FreeTemp(cu, r_index);
  MarkGCCard(cu, r_value, r_array);
}

/*
 * Generate array load
 */
void GenArrayGet(CompilationUnit* cu, int opt_flags, OpSize size,
                 RegLocation rl_array, RegLocation rl_index,
                 RegLocation rl_dest, int scale)
{
  RegisterClass reg_class = oat_reg_class_by_size(size);
  int len_offset = Array::LengthOffset().Int32Value();
  int data_offset;
  RegLocation rl_result;
  rl_array = LoadValue(cu, rl_array, kCoreReg);
  rl_index = LoadValue(cu, rl_index, kCoreReg);

  if (size == kLong || size == kDouble) {
    data_offset = Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  /* null object? */
  GenNullCheck(cu, rl_array.s_reg_low, rl_array.low_reg, opt_flags);

  if (cu->instruction_set == kX86) {
    if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
      /* if (rl_index >= [rl_array + len_offset]) goto kThrowArrayBounds */
      GenRegMemCheck(cu, kCondUge, rl_index.low_reg, rl_array.low_reg,
                     len_offset, kThrowArrayBounds);
    }
    if ((size == kLong) || (size == kDouble)) {
      int reg_addr = AllocTemp(cu);
      OpLea(cu, reg_addr, rl_array.low_reg, rl_index.low_reg, scale, data_offset);
      FreeTemp(cu, rl_array.low_reg);
      FreeTemp(cu, rl_index.low_reg);
      rl_result = EvalLoc(cu, rl_dest, reg_class, true);
      LoadBaseIndexedDisp(cu, reg_addr, INVALID_REG, 0, 0, rl_result.low_reg,
                          rl_result.high_reg, size, INVALID_SREG);
      StoreValueWide(cu, rl_dest, rl_result);
    } else {
      rl_result = EvalLoc(cu, rl_dest, reg_class, true);

      LoadBaseIndexedDisp(cu, rl_array.low_reg, rl_index.low_reg, scale,
                          data_offset, rl_result.low_reg, INVALID_REG, size,
                          INVALID_SREG);

      StoreValue(cu, rl_dest, rl_result);
    }
  } else {
    int reg_ptr = AllocTemp(cu);
    bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
    int reg_len = INVALID_REG;
    if (needs_range_check) {
      reg_len = AllocTemp(cu);
      /* Get len */
      LoadWordDisp(cu, rl_array.low_reg, len_offset, reg_len);
    }
    /* reg_ptr -> array data */
    OpRegRegImm(cu, kOpAdd, reg_ptr, rl_array.low_reg, data_offset);
    FreeTemp(cu, rl_array.low_reg);
    if ((size == kLong) || (size == kDouble)) {
      if (scale) {
        int r_new_index = AllocTemp(cu);
        OpRegRegImm(cu, kOpLsl, r_new_index, rl_index.low_reg, scale);
        OpRegReg(cu, kOpAdd, reg_ptr, r_new_index);
        FreeTemp(cu, r_new_index);
      } else {
        OpRegReg(cu, kOpAdd, reg_ptr, rl_index.low_reg);
      }
      FreeTemp(cu, rl_index.low_reg);
      rl_result = EvalLoc(cu, rl_dest, reg_class, true);

      if (needs_range_check) {
        // TODO: change kCondCS to a more meaningful name, is the sense of
        // carry-set/clear flipped?
        GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
        FreeTemp(cu, reg_len);
      }
      LoadPair(cu, reg_ptr, rl_result.low_reg, rl_result.high_reg);

      FreeTemp(cu, reg_ptr);
      StoreValueWide(cu, rl_dest, rl_result);
    } else {
      rl_result = EvalLoc(cu, rl_dest, reg_class, true);

      if (needs_range_check) {
        // TODO: change kCondCS to a more meaningful name, is the sense of
        // carry-set/clear flipped?
        GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
        FreeTemp(cu, reg_len);
      }
      LoadBaseIndexed(cu, reg_ptr, rl_index.low_reg, rl_result.low_reg, scale, size);

      FreeTemp(cu, reg_ptr);
      StoreValue(cu, rl_dest, rl_result);
    }
  }
}

/*
 * Generate array store
 *
 */
void GenArrayPut(CompilationUnit* cu, int opt_flags, OpSize size,
                 RegLocation rl_array, RegLocation rl_index,
                 RegLocation rl_src, int scale)
{
  RegisterClass reg_class = oat_reg_class_by_size(size);
  int len_offset = Array::LengthOffset().Int32Value();
  int data_offset;

  if (size == kLong || size == kDouble) {
    data_offset = Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  rl_array = LoadValue(cu, rl_array, kCoreReg);
  rl_index = LoadValue(cu, rl_index, kCoreReg);
  int reg_ptr = INVALID_REG;
  if (cu->instruction_set != kX86) {
    if (IsTemp(cu, rl_array.low_reg)) {
      Clobber(cu, rl_array.low_reg);
      reg_ptr = rl_array.low_reg;
    } else {
      reg_ptr = AllocTemp(cu);
      OpRegCopy(cu, reg_ptr, rl_array.low_reg);
    }
  }

  /* null object? */
  GenNullCheck(cu, rl_array.s_reg_low, rl_array.low_reg, opt_flags);

  if (cu->instruction_set == kX86) {
    if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
      /* if (rl_index >= [rl_array + len_offset]) goto kThrowArrayBounds */
      GenRegMemCheck(cu, kCondUge, rl_index.low_reg, rl_array.low_reg, len_offset, kThrowArrayBounds);
    }
    if ((size == kLong) || (size == kDouble)) {
      rl_src = LoadValueWide(cu, rl_src, reg_class);
    } else {
      rl_src = LoadValue(cu, rl_src, reg_class);
    }
    // If the src reg can't be byte accessed, move it to a temp first.
    if ((size == kSignedByte || size == kUnsignedByte) && rl_src.low_reg >= 4) {
      int temp = AllocTemp(cu);
      OpRegCopy(cu, temp, rl_src.low_reg);
      StoreBaseIndexedDisp(cu, rl_array.low_reg, rl_index.low_reg, scale, data_offset, temp,
                           INVALID_REG, size, INVALID_SREG);
    } else {
      StoreBaseIndexedDisp(cu, rl_array.low_reg, rl_index.low_reg, scale, data_offset, rl_src.low_reg,
                           rl_src.high_reg, size, INVALID_SREG);
    }
  } else {
    bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
    int reg_len = INVALID_REG;
    if (needs_range_check) {
      reg_len = AllocTemp(cu);
      //NOTE: max live temps(4) here.
      /* Get len */
      LoadWordDisp(cu, rl_array.low_reg, len_offset, reg_len);
    }
    /* reg_ptr -> array data */
    OpRegImm(cu, kOpAdd, reg_ptr, data_offset);
    /* at this point, reg_ptr points to array, 2 live temps */
    if ((size == kLong) || (size == kDouble)) {
      //TUNING: specific wide routine that can handle fp regs
      if (scale) {
        int r_new_index = AllocTemp(cu);
        OpRegRegImm(cu, kOpLsl, r_new_index, rl_index.low_reg, scale);
        OpRegReg(cu, kOpAdd, reg_ptr, r_new_index);
        FreeTemp(cu, r_new_index);
      } else {
        OpRegReg(cu, kOpAdd, reg_ptr, rl_index.low_reg);
      }
      rl_src = LoadValueWide(cu, rl_src, reg_class);

      if (needs_range_check) {
        GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
        FreeTemp(cu, reg_len);
      }

      StoreBaseDispWide(cu, reg_ptr, 0, rl_src.low_reg, rl_src.high_reg);

      FreeTemp(cu, reg_ptr);
    } else {
      rl_src = LoadValue(cu, rl_src, reg_class);
      if (needs_range_check) {
        GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
        FreeTemp(cu, reg_len);
      }
      StoreBaseIndexed(cu, reg_ptr, rl_index.low_reg, rl_src.low_reg,
                       scale, size);
    }
  }
}

void GenLong3Addr(CompilationUnit* cu, OpKind first_op,
                  OpKind second_op, RegLocation rl_dest,
                  RegLocation rl_src1, RegLocation rl_src2)
{
  RegLocation rl_result;
  if (cu->instruction_set == kThumb2) {
    /*
     * NOTE:  This is the one place in the code in which we might have
     * as many as six live temporary registers.  There are 5 in the normal
     * set for Arm.  Until we have spill capabilities, temporarily add
     * lr to the temp set.  It is safe to do this locally, but note that
     * lr is used explicitly elsewhere in the code generator and cannot
     * normally be used as a general temp register.
     */
    MarkTemp(cu, TargetReg(kLr));   // Add lr to the temp pool
    FreeTemp(cu, TargetReg(kLr));   // and make it available
  }
  rl_src1 = LoadValueWide(cu, rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(cu, rl_src2, kCoreReg);
  rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  // The longs may overlap - use intermediate temp if so
  if ((rl_result.low_reg == rl_src1.high_reg) || (rl_result.low_reg == rl_src2.high_reg)){
    int t_reg = AllocTemp(cu);
    OpRegRegReg(cu, first_op, t_reg, rl_src1.low_reg, rl_src2.low_reg);
    OpRegRegReg(cu, second_op, rl_result.high_reg, rl_src1.high_reg, rl_src2.high_reg);
    OpRegCopy(cu, rl_result.low_reg, t_reg);
    FreeTemp(cu, t_reg);
  } else {
    OpRegRegReg(cu, first_op, rl_result.low_reg, rl_src1.low_reg, rl_src2.low_reg);
    OpRegRegReg(cu, second_op, rl_result.high_reg, rl_src1.high_reg,
                rl_src2.high_reg);
  }
  /*
   * NOTE: If rl_dest refers to a frame variable in a large frame, the
   * following StoreValueWide might need to allocate a temp register.
   * To further work around the lack of a spill capability, explicitly
   * free any temps from rl_src1 & rl_src2 that aren't still live in rl_result.
   * Remove when spill is functional.
   */
  FreeRegLocTemps(cu, rl_result, rl_src1);
  FreeRegLocTemps(cu, rl_result, rl_src2);
  StoreValueWide(cu, rl_dest, rl_result);
  if (cu->instruction_set == kThumb2) {
    Clobber(cu, TargetReg(kLr));
    UnmarkTemp(cu, TargetReg(kLr));  // Remove lr from the temp pool
  }
}


bool GenShiftOpLong(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                    RegLocation rl_src1, RegLocation rl_shift)
{
  int func_offset;

  switch (opcode) {
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      func_offset = ENTRYPOINT_OFFSET(pShlLong);
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      func_offset = ENTRYPOINT_OFFSET(pShrLong);
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      func_offset = ENTRYPOINT_OFFSET(pUshrLong);
      break;
    default:
      LOG(FATAL) << "Unexpected case";
      return true;
  }
  FlushAllRegs(cu);   /* Send everything to home location */
  CallRuntimeHelperRegLocationRegLocation(cu, func_offset, rl_src1, rl_shift, false);
  RegLocation rl_result = GetReturnWide(cu, false);
  StoreValueWide(cu, rl_dest, rl_result);
  return false;
}


bool GenArithOpInt(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
           RegLocation rl_src1, RegLocation rl_src2)
{
  OpKind op = kOpBkpt;
  bool is_div_rem = false;
  bool check_zero = false;
  bool unary = false;
  RegLocation rl_result;
  bool shift_op = false;
  switch (opcode) {
    case Instruction::NEG_INT:
      op = kOpNeg;
      unary = true;
      break;
    case Instruction::NOT_INT:
      op = kOpMvn;
      unary = true;
      break;
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
      op = kOpAdd;
      break;
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      op = kOpSub;
      break;
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
      op = kOpMul;
      break;
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
      check_zero = true;
      op = kOpDiv;
      is_div_rem = true;
      break;
    /* NOTE: returns in kArg1 */
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
      check_zero = true;
      op = kOpRem;
      is_div_rem = true;
      break;
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
      op = kOpAnd;
      break;
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
      op = kOpOr;
      break;
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
      op = kOpXor;
      break;
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      shift_op = true;
      op = kOpLsl;
      break;
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      shift_op = true;
      op = kOpAsr;
      break;
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      shift_op = true;
      op = kOpLsr;
      break;
    default:
      LOG(FATAL) << "Invalid word arith op: " << opcode;
  }
  if (!is_div_rem) {
    if (unary) {
      rl_src1 = LoadValue(cu, rl_src1, kCoreReg);
      rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
      OpRegReg(cu, op, rl_result.low_reg, rl_src1.low_reg);
    } else {
      if (shift_op) {
        int t_reg = INVALID_REG;
        if (cu->instruction_set == kX86) {
          // X86 doesn't require masking and must use ECX
          t_reg = TargetReg(kCount);  // rCX
          LoadValueDirectFixed(cu, rl_src2, t_reg);
        } else {
          rl_src2 = LoadValue(cu, rl_src2, kCoreReg);
          t_reg = AllocTemp(cu);
          OpRegRegImm(cu, kOpAnd, t_reg, rl_src2.low_reg, 31);
        }
        rl_src1 = LoadValue(cu, rl_src1, kCoreReg);
        rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
        OpRegRegReg(cu, op, rl_result.low_reg, rl_src1.low_reg, t_reg);
        FreeTemp(cu, t_reg);
      } else {
        rl_src1 = LoadValue(cu, rl_src1, kCoreReg);
        rl_src2 = LoadValue(cu, rl_src2, kCoreReg);
        rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
        OpRegRegReg(cu, op, rl_result.low_reg, rl_src1.low_reg, rl_src2.low_reg);
      }
    }
    StoreValue(cu, rl_dest, rl_result);
  } else {
    if (cu->instruction_set == kMips) {
      rl_src1 = LoadValue(cu, rl_src1, kCoreReg);
      rl_src2 = LoadValue(cu, rl_src2, kCoreReg);
      if (check_zero) {
          GenImmedCheck(cu, kCondEq, rl_src2.low_reg, 0, kThrowDivZero);
      }
      rl_result = GenDivRem(cu, rl_dest, rl_src1.low_reg, rl_src2.low_reg, op == kOpDiv);
    } else {
      int func_offset = ENTRYPOINT_OFFSET(pIdivmod);
      FlushAllRegs(cu);   /* Send everything to home location */
      LoadValueDirectFixed(cu, rl_src2, TargetReg(kArg1));
      int r_tgt = CallHelperSetup(cu, func_offset);
      LoadValueDirectFixed(cu, rl_src1, TargetReg(kArg0));
      if (check_zero) {
        GenImmedCheck(cu, kCondEq, TargetReg(kArg1), 0, kThrowDivZero);
      }
      // NOTE: callout here is not a safepoint
      CallHelper(cu, r_tgt, func_offset, false /* not a safepoint */ );
      if (op == kOpDiv)
        rl_result = GetReturn(cu, false);
      else
        rl_result = GetReturnAlt(cu);
    }
    StoreValue(cu, rl_dest, rl_result);
  }
  return false;
}

/*
 * The following are the first-level codegen routines that analyze the format
 * of each bytecode then either dispatch special purpose codegen routines
 * or produce corresponding Thumb instructions directly.
 */

static bool IsPowerOfTwo(int x)
{
  return (x & (x - 1)) == 0;
}

// Returns true if no more than two bits are set in 'x'.
static bool IsPopCountLE2(unsigned int x)
{
  x &= x - 1;
  return (x & (x - 1)) == 0;
}

// Returns the index of the lowest set bit in 'x'.
static int LowestSetBit(unsigned int x) {
  int bit_posn = 0;
  while ((x & 0xf) == 0) {
    bit_posn += 4;
    x >>= 4;
  }
  while ((x & 1) == 0) {
    bit_posn++;
    x >>= 1;
  }
  return bit_posn;
}

// Returns true if it added instructions to 'cu' to divide 'rl_src' by 'lit'
// and store the result in 'rl_dest'.
static bool HandleEasyDivide(CompilationUnit* cu, Instruction::Code dalvik_opcode,
                             RegLocation rl_src, RegLocation rl_dest, int lit)
{
  if ((lit < 2) || ((cu->instruction_set != kThumb2) && !IsPowerOfTwo(lit))) {
    return false;
  }
  // No divide instruction for Arm, so check for more special cases
  if ((cu->instruction_set == kThumb2) && !IsPowerOfTwo(lit)) {
    return SmallLiteralDivide(cu, dalvik_opcode, rl_src, rl_dest, lit);
  }
  int k = LowestSetBit(lit);
  if (k >= 30) {
    // Avoid special cases.
    return false;
  }
  bool div = (dalvik_opcode == Instruction::DIV_INT_LIT8 ||
      dalvik_opcode == Instruction::DIV_INT_LIT16);
  rl_src = LoadValue(cu, rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  if (div) {
    int t_reg = AllocTemp(cu);
    if (lit == 2) {
      // Division by 2 is by far the most common division by constant.
      OpRegRegImm(cu, kOpLsr, t_reg, rl_src.low_reg, 32 - k);
      OpRegRegReg(cu, kOpAdd, t_reg, t_reg, rl_src.low_reg);
      OpRegRegImm(cu, kOpAsr, rl_result.low_reg, t_reg, k);
    } else {
      OpRegRegImm(cu, kOpAsr, t_reg, rl_src.low_reg, 31);
      OpRegRegImm(cu, kOpLsr, t_reg, t_reg, 32 - k);
      OpRegRegReg(cu, kOpAdd, t_reg, t_reg, rl_src.low_reg);
      OpRegRegImm(cu, kOpAsr, rl_result.low_reg, t_reg, k);
    }
  } else {
    int t_reg1 = AllocTemp(cu);
    int t_reg2 = AllocTemp(cu);
    if (lit == 2) {
      OpRegRegImm(cu, kOpLsr, t_reg1, rl_src.low_reg, 32 - k);
      OpRegRegReg(cu, kOpAdd, t_reg2, t_reg1, rl_src.low_reg);
      OpRegRegImm(cu, kOpAnd, t_reg2, t_reg2, lit -1);
      OpRegRegReg(cu, kOpSub, rl_result.low_reg, t_reg2, t_reg1);
    } else {
      OpRegRegImm(cu, kOpAsr, t_reg1, rl_src.low_reg, 31);
      OpRegRegImm(cu, kOpLsr, t_reg1, t_reg1, 32 - k);
      OpRegRegReg(cu, kOpAdd, t_reg2, t_reg1, rl_src.low_reg);
      OpRegRegImm(cu, kOpAnd, t_reg2, t_reg2, lit - 1);
      OpRegRegReg(cu, kOpSub, rl_result.low_reg, t_reg2, t_reg1);
    }
  }
  StoreValue(cu, rl_dest, rl_result);
  return true;
}

// Returns true if it added instructions to 'cu' to multiply 'rl_src' by 'lit'
// and store the result in 'rl_dest'.
static bool HandleEasyMultiply(CompilationUnit* cu, RegLocation rl_src,
                               RegLocation rl_dest, int lit)
{
  // Can we simplify this multiplication?
  bool power_of_two = false;
  bool pop_count_le2 = false;
  bool power_of_two_minus_one = false;
  if (lit < 2) {
    // Avoid special cases.
    return false;
  } else if (IsPowerOfTwo(lit)) {
    power_of_two = true;
  } else if (IsPopCountLE2(lit)) {
    pop_count_le2 = true;
  } else if (IsPowerOfTwo(lit + 1)) {
    power_of_two_minus_one = true;
  } else {
    return false;
  }
  rl_src = LoadValue(cu, rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  if (power_of_two) {
    // Shift.
    OpRegRegImm(cu, kOpLsl, rl_result.low_reg, rl_src.low_reg,
                LowestSetBit(lit));
  } else if (pop_count_le2) {
    // Shift and add and shift.
    int first_bit = LowestSetBit(lit);
    int second_bit = LowestSetBit(lit ^ (1 << first_bit));
    GenMultiplyByTwoBitMultiplier(cu, rl_src, rl_result, lit,
                                  first_bit, second_bit);
  } else {
    // Reverse subtract: (src << (shift + 1)) - src.
    DCHECK(power_of_two_minus_one);
    // TUNING: rsb dst, src, src lsl#LowestSetBit(lit + 1)
    int t_reg = AllocTemp(cu);
    OpRegRegImm(cu, kOpLsl, t_reg, rl_src.low_reg, LowestSetBit(lit + 1));
    OpRegRegReg(cu, kOpSub, rl_result.low_reg, t_reg, rl_src.low_reg);
  }
  StoreValue(cu, rl_dest, rl_result);
  return true;
}

bool GenArithOpIntLit(CompilationUnit* cu, Instruction::Code opcode,
                      RegLocation rl_dest, RegLocation rl_src, int lit)
{
  RegLocation rl_result;
  OpKind op = static_cast<OpKind>(0);    /* Make gcc happy */
  int shift_op = false;
  bool is_div = false;

  switch (opcode) {
    case Instruction::RSUB_INT_LIT8:
    case Instruction::RSUB_INT: {
      int t_reg;
      //TUNING: add support for use of Arm rsub op
      rl_src = LoadValue(cu, rl_src, kCoreReg);
      t_reg = AllocTemp(cu);
      LoadConstant(cu, t_reg, lit);
      rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
      OpRegRegReg(cu, kOpSub, rl_result.low_reg, t_reg, rl_src.low_reg);
      StoreValue(cu, rl_dest, rl_result);
      return false;
      break;
    }

    case Instruction::ADD_INT_LIT8:
    case Instruction::ADD_INT_LIT16:
      op = kOpAdd;
      break;
    case Instruction::MUL_INT_LIT8:
    case Instruction::MUL_INT_LIT16: {
      if (HandleEasyMultiply(cu, rl_src, rl_dest, lit)) {
        return false;
      }
      op = kOpMul;
      break;
    }
    case Instruction::AND_INT_LIT8:
    case Instruction::AND_INT_LIT16:
      op = kOpAnd;
      break;
    case Instruction::OR_INT_LIT8:
    case Instruction::OR_INT_LIT16:
      op = kOpOr;
      break;
    case Instruction::XOR_INT_LIT8:
    case Instruction::XOR_INT_LIT16:
      op = kOpXor;
      break;
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHL_INT:
      lit &= 31;
      shift_op = true;
      op = kOpLsl;
      break;
    case Instruction::SHR_INT_LIT8:
    case Instruction::SHR_INT:
      lit &= 31;
      shift_op = true;
      op = kOpAsr;
      break;
    case Instruction::USHR_INT_LIT8:
    case Instruction::USHR_INT:
      lit &= 31;
      shift_op = true;
      op = kOpLsr;
      break;

    case Instruction::DIV_INT_LIT8:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT8:
    case Instruction::REM_INT_LIT16: {
      if (lit == 0) {
        GenImmedCheck(cu, kCondAl, 0, 0, kThrowDivZero);
        return false;
      }
      if (HandleEasyDivide(cu, opcode, rl_src, rl_dest, lit)) {
        return false;
      }
      if ((opcode == Instruction::DIV_INT_LIT8) ||
          (opcode == Instruction::DIV_INT_LIT16)) {
        is_div = true;
      } else {
        is_div = false;
      }
      if (cu->instruction_set == kMips) {
        rl_src = LoadValue(cu, rl_src, kCoreReg);
        rl_result = GenDivRemLit(cu, rl_dest, rl_src.low_reg, lit, is_div);
      } else {
        FlushAllRegs(cu);   /* Everything to home location */
        LoadValueDirectFixed(cu, rl_src, TargetReg(kArg0));
        Clobber(cu, TargetReg(kArg0));
        int func_offset = ENTRYPOINT_OFFSET(pIdivmod);
        CallRuntimeHelperRegImm(cu, func_offset, TargetReg(kArg0), lit, false);
        if (is_div)
          rl_result = GetReturn(cu, false);
        else
          rl_result = GetReturnAlt(cu);
      }
      StoreValue(cu, rl_dest, rl_result);
      return false;
      break;
    }
    default:
      return true;
  }
  rl_src = LoadValue(cu, rl_src, kCoreReg);
  rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  // Avoid shifts by literal 0 - no support in Thumb.  Change to copy
  if (shift_op && (lit == 0)) {
    OpRegCopy(cu, rl_result.low_reg, rl_src.low_reg);
  } else {
    OpRegRegImm(cu, op, rl_result.low_reg, rl_src.low_reg, lit);
  }
  StoreValue(cu, rl_dest, rl_result);
  return false;
}

bool GenArithOpLong(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
          RegLocation rl_src1, RegLocation rl_src2)
{
  RegLocation rl_result;
  OpKind first_op = kOpBkpt;
  OpKind second_op = kOpBkpt;
  bool call_out = false;
  bool check_zero = false;
  int func_offset;
  int ret_reg = TargetReg(kRet0);

  switch (opcode) {
    case Instruction::NOT_LONG:
      rl_src2 = LoadValueWide(cu, rl_src2, kCoreReg);
      rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
      // Check for destructive overlap
      if (rl_result.low_reg == rl_src2.high_reg) {
        int t_reg = AllocTemp(cu);
        OpRegCopy(cu, t_reg, rl_src2.high_reg);
        OpRegReg(cu, kOpMvn, rl_result.low_reg, rl_src2.low_reg);
        OpRegReg(cu, kOpMvn, rl_result.high_reg, t_reg);
        FreeTemp(cu, t_reg);
      } else {
        OpRegReg(cu, kOpMvn, rl_result.low_reg, rl_src2.low_reg);
        OpRegReg(cu, kOpMvn, rl_result.high_reg, rl_src2.high_reg);
      }
      StoreValueWide(cu, rl_dest, rl_result);
      return false;
      break;
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      if (cu->instruction_set != kThumb2) {
        return GenAddLong(cu, rl_dest, rl_src1, rl_src2);
      }
      first_op = kOpAdd;
      second_op = kOpAdc;
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if (cu->instruction_set != kThumb2) {
        return GenSubLong(cu, rl_dest, rl_src1, rl_src2);
      }
      first_op = kOpSub;
      second_op = kOpSbc;
      break;
    case Instruction::MUL_LONG:
    case Instruction::MUL_LONG_2ADDR:
      call_out = true;
      ret_reg = TargetReg(kRet0);
      func_offset = ENTRYPOINT_OFFSET(pLmul);
      break;
    case Instruction::DIV_LONG:
    case Instruction::DIV_LONG_2ADDR:
      call_out = true;
      check_zero = true;
      ret_reg = TargetReg(kRet0);
      func_offset = ENTRYPOINT_OFFSET(pLdiv);
      break;
    case Instruction::REM_LONG:
    case Instruction::REM_LONG_2ADDR:
      call_out = true;
      check_zero = true;
      func_offset = ENTRYPOINT_OFFSET(pLdivmod);
      /* NOTE - for Arm, result is in kArg2/kArg3 instead of kRet0/kRet1 */
      ret_reg = (cu->instruction_set == kThumb2) ? TargetReg(kArg2) : TargetReg(kRet0);
      break;
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_LONG:
      if (cu->instruction_set == kX86) {
        return GenAndLong(cu, rl_dest, rl_src1, rl_src2);
      }
      first_op = kOpAnd;
      second_op = kOpAnd;
      break;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      if (cu->instruction_set == kX86) {
        return GenOrLong(cu, rl_dest, rl_src1, rl_src2);
      }
      first_op = kOpOr;
      second_op = kOpOr;
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      if (cu->instruction_set == kX86) {
        return GenXorLong(cu, rl_dest, rl_src1, rl_src2);
      }
      first_op = kOpXor;
      second_op = kOpXor;
      break;
    case Instruction::NEG_LONG: {
      return GenNegLong(cu, rl_dest, rl_src2);
    }
    default:
      LOG(FATAL) << "Invalid long arith op";
  }
  if (!call_out) {
    GenLong3Addr(cu, first_op, second_op, rl_dest, rl_src1, rl_src2);
  } else {
    FlushAllRegs(cu);   /* Send everything to home location */
    if (check_zero) {
      LoadValueDirectWideFixed(cu, rl_src2, TargetReg(kArg2), TargetReg(kArg3));
      int r_tgt = CallHelperSetup(cu, func_offset);
      GenDivZeroCheck(cu, TargetReg(kArg2), TargetReg(kArg3));
      LoadValueDirectWideFixed(cu, rl_src1, TargetReg(kArg0), TargetReg(kArg1));
      // NOTE: callout here is not a safepoint
      CallHelper(cu, r_tgt, func_offset, false /* not safepoint */);
    } else {
      CallRuntimeHelperRegLocationRegLocation(cu, func_offset,
                          rl_src1, rl_src2, false);
    }
    // Adjust return regs in to handle case of rem returning kArg2/kArg3
    if (ret_reg == TargetReg(kRet0))
      rl_result = GetReturnWide(cu, false);
    else
      rl_result = GetReturnWideAlt(cu);
    StoreValueWide(cu, rl_dest, rl_result);
  }
  return false;
}

bool GenConversionCall(CompilationUnit* cu, int func_offset,
                       RegLocation rl_dest, RegLocation rl_src)
{
  /*
   * Don't optimize the register usage since it calls out to support
   * functions
   */
  FlushAllRegs(cu);   /* Send everything to home location */
  if (rl_src.wide) {
    LoadValueDirectWideFixed(cu, rl_src, rl_src.fp ? TargetReg(kFArg0) : TargetReg(kArg0),
                             rl_src.fp ? TargetReg(kFArg1) : TargetReg(kArg1));
  } else {
    LoadValueDirectFixed(cu, rl_src, rl_src.fp ? TargetReg(kFArg0) : TargetReg(kArg0));
  }
  CallRuntimeHelperRegLocation(cu, func_offset, rl_src, false);
  if (rl_dest.wide) {
    RegLocation rl_result;
    rl_result = GetReturnWide(cu, rl_dest.fp);
    StoreValueWide(cu, rl_dest, rl_result);
  } else {
    RegLocation rl_result;
    rl_result = GetReturn(cu, rl_dest.fp);
    StoreValue(cu, rl_dest, rl_result);
  }
  return false;
}

bool GenArithOpFloatPortable(CompilationUnit* cu, Instruction::Code opcode,
                             RegLocation rl_dest, RegLocation rl_src1,
                             RegLocation rl_src2)
{
  RegLocation rl_result;
  int func_offset;

  switch (opcode) {
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::ADD_FLOAT:
      func_offset = ENTRYPOINT_OFFSET(pFadd);
      break;
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT:
      func_offset = ENTRYPOINT_OFFSET(pFsub);
      break;
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT:
      func_offset = ENTRYPOINT_OFFSET(pFdiv);
      break;
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT:
      func_offset = ENTRYPOINT_OFFSET(pFmul);
      break;
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::REM_FLOAT:
      func_offset = ENTRYPOINT_OFFSET(pFmodf);
      break;
    case Instruction::NEG_FLOAT: {
      GenNegFloat(cu, rl_dest, rl_src1);
      return false;
    }
    default:
      return true;
  }
  FlushAllRegs(cu);   /* Send everything to home location */
  CallRuntimeHelperRegLocationRegLocation(cu, func_offset, rl_src1, rl_src2, false);
  rl_result = GetReturn(cu, true);
  StoreValue(cu, rl_dest, rl_result);
  return false;
}

bool GenArithOpDoublePortable(CompilationUnit* cu, Instruction::Code opcode,
                              RegLocation rl_dest, RegLocation rl_src1,
                              RegLocation rl_src2)
{
  RegLocation rl_result;
  int func_offset;

  switch (opcode) {
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::ADD_DOUBLE:
      func_offset = ENTRYPOINT_OFFSET(pDadd);
      break;
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE:
      func_offset = ENTRYPOINT_OFFSET(pDsub);
      break;
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE:
      func_offset = ENTRYPOINT_OFFSET(pDdiv);
      break;
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE:
      func_offset = ENTRYPOINT_OFFSET(pDmul);
      break;
    case Instruction::REM_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE:
      func_offset = ENTRYPOINT_OFFSET(pFmod);
      break;
    case Instruction::NEG_DOUBLE: {
      GenNegDouble(cu, rl_dest, rl_src1);
      return false;
    }
    default:
      return true;
  }
  FlushAllRegs(cu);   /* Send everything to home location */
  CallRuntimeHelperRegLocationRegLocation(cu, func_offset, rl_src1, rl_src2, false);
  rl_result = GetReturnWide(cu, true);
  StoreValueWide(cu, rl_dest, rl_result);
  return false;
}

bool GenConversionPortable(CompilationUnit* cu, Instruction::Code opcode,
                           RegLocation rl_dest, RegLocation rl_src)
{

  switch (opcode) {
    case Instruction::INT_TO_FLOAT:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pI2f),
                   rl_dest, rl_src);
    case Instruction::FLOAT_TO_INT:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pF2iz),
                   rl_dest, rl_src);
    case Instruction::DOUBLE_TO_FLOAT:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pD2f),
                   rl_dest, rl_src);
    case Instruction::FLOAT_TO_DOUBLE:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pF2d),
                   rl_dest, rl_src);
    case Instruction::INT_TO_DOUBLE:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pI2d),
                   rl_dest, rl_src);
    case Instruction::DOUBLE_TO_INT:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pD2iz),
                   rl_dest, rl_src);
    case Instruction::FLOAT_TO_LONG:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pF2l),
                   rl_dest, rl_src);
    case Instruction::LONG_TO_FLOAT:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pL2f),
                   rl_dest, rl_src);
    case Instruction::DOUBLE_TO_LONG:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pD2l),
                   rl_dest, rl_src);
    case Instruction::LONG_TO_DOUBLE:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pL2d),
                   rl_dest, rl_src);
    default:
      return true;
  }
  return false;
}

/* Check if we need to check for pending suspend request */
void GenSuspendTest(CompilationUnit* cu, int opt_flags)
{
  if (NO_SUSPEND || (opt_flags & MIR_IGNORE_SUSPEND_CHECK)) {
    return;
  }
  FlushAllRegs(cu);
  LIR* branch = OpTestSuspend(cu, NULL);
  LIR* ret_lab = NewLIR0(cu, kPseudoTargetLabel);
  LIR* target = RawLIR(cu, cu->current_dalvik_offset, kPseudoSuspendTarget,
                       reinterpret_cast<uintptr_t>(ret_lab), cu->current_dalvik_offset);
  branch->target = target;
  InsertGrowableList(cu, &cu->suspend_launchpads, reinterpret_cast<uintptr_t>(target));
}

/* Check if we need to check for pending suspend request */
void GenSuspendTestAndBranch(CompilationUnit* cu, int opt_flags, LIR* target)
{
  if (NO_SUSPEND || (opt_flags & MIR_IGNORE_SUSPEND_CHECK)) {
    OpUnconditionalBranch(cu, target);
    return;
  }
  OpTestSuspend(cu, target);
  LIR* launch_pad =
      RawLIR(cu, cu->current_dalvik_offset, kPseudoSuspendTarget,
             reinterpret_cast<uintptr_t>(target), cu->current_dalvik_offset);
  FlushAllRegs(cu);
  OpUnconditionalBranch(cu, launch_pad);
  InsertGrowableList(cu, &cu->suspend_launchpads, reinterpret_cast<uintptr_t>(launch_pad));
}

}  // namespace art
