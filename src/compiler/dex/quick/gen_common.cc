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

#include "codegen_util.h"
#include "compiler/dex/compiler_ir.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "ralloc_util.h"

namespace art {

/*
 * This source files contains "gen" codegen routines that should
 * be applicable to most targets.  Only mid-level support utilities
 * and "op" calls may be used here.
 */

/*
 * Generate an kPseudoBarrier marker to indicate the boundary of special
 * blocks.
 */
void Codegen::GenBarrier(CompilationUnit* cu)
{
  LIR* barrier = NewLIR0(cu, kPseudoBarrier);
  /* Mark all resources as being clobbered */
  barrier->def_mask = -1;
}

// FIXME: need to do some work to split out targets with
// condition codes and those without
LIR* Codegen::GenCheck(CompilationUnit* cu, ConditionCode c_code, ThrowKind kind)
{
  DCHECK_NE(cu->instruction_set, kMips);
  LIR* tgt = RawLIR(cu, 0, kPseudoThrowTarget, kind,
                    cu->current_dalvik_offset);
  LIR* branch = OpCondBranch(cu, c_code, tgt);
  // Remember branch target - will process later
  InsertGrowableList(cu, &cu->throw_launchpads, reinterpret_cast<uintptr_t>(tgt));
  return branch;
}

LIR* Codegen::GenImmedCheck(CompilationUnit* cu, ConditionCode c_code, int reg, int imm_val,
                            ThrowKind kind)
{
  LIR* tgt = RawLIR(cu, 0, kPseudoThrowTarget, kind,
                    cu->current_dalvik_offset, reg, imm_val);
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
LIR* Codegen::GenNullCheck(CompilationUnit* cu, int s_reg, int m_reg, int opt_flags)
{
  if (!(cu->disable_opt & (1 << kNullCheckElimination)) &&
    opt_flags & MIR_IGNORE_NULL_CHECK) {
    return NULL;
  }
  return GenImmedCheck(cu, kCondEq, m_reg, 0, kThrowNullPointer);
}

/* Perform check on two registers */
LIR* Codegen::GenRegRegCheck(CompilationUnit* cu, ConditionCode c_code, int reg1, int reg2,
                             ThrowKind kind)
{
  LIR* tgt = RawLIR(cu, 0, kPseudoThrowTarget, kind,
                    cu->current_dalvik_offset, reg1, reg2);
  LIR* branch = OpCmpBranch(cu, c_code, reg1, reg2, tgt);
  // Remember branch target - will process later
  InsertGrowableList(cu, &cu->throw_launchpads, reinterpret_cast<uintptr_t>(tgt));
  return branch;
}

void Codegen::GenCompareAndBranch(CompilationUnit* cu, Instruction::Code opcode,
                                  RegLocation rl_src1, RegLocation rl_src2, LIR* taken,
                                  LIR* fall_through)
{
  ConditionCode cond;
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

  // Normalize such that if either operand is constant, src2 will be constant
  if (rl_src1.is_const) {
    RegLocation rl_temp = rl_src1;
    rl_src1 = rl_src2;
    rl_src2 = rl_temp;
    cond = FlipComparisonOrder(cond);
  }

  rl_src1 = LoadValue(cu, rl_src1, kCoreReg);
  // Is this really an immediate comparison?
  if (rl_src2.is_const) {
    // If it's already live in a register or not easily materialized, just keep going
    RegLocation rl_temp = UpdateLoc(cu, rl_src2);
    if ((rl_temp.location == kLocDalvikFrame) &&
        InexpensiveConstantInt(ConstantValue(cu, rl_src2))) {
      // OK - convert this to a compare immediate and branch
      OpCmpImmBranch(cu, cond, rl_src1.low_reg, ConstantValue(cu, rl_src2), taken);
      OpUnconditionalBranch(cu, fall_through);
      return;
    }
  }
  rl_src2 = LoadValue(cu, rl_src2, kCoreReg);
  OpCmpBranch(cu, cond, rl_src1.low_reg, rl_src2.low_reg, taken);
  OpUnconditionalBranch(cu, fall_through);
}

void Codegen::GenCompareZeroAndBranch(CompilationUnit* cu, Instruction::Code opcode,
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
  OpCmpImmBranch(cu, cond, rl_src.low_reg, 0, taken);
  OpUnconditionalBranch(cu, fall_through);
}

void Codegen::GenIntToLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
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

void Codegen::GenIntNarrowing(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                              RegLocation rl_src)
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
void Codegen::GenNewArray(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest,
                          RegLocation rl_src)
{
  FlushAllRegs(cu);  /* Everything to home location */
  int func_offset;
  if (cu->compiler_driver->CanAccessTypeWithoutChecks(cu->method_idx,
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
void Codegen::GenFilledNewArray(CompilationUnit* cu, CallInfo* info)
{
  int elems = info->num_arg_words;
  int type_idx = info->index;
  FlushAllRegs(cu);  /* Everything to home location */
  int func_offset;
  if (cu->compiler_driver->CanAccessTypeWithoutChecks(cu->method_idx,
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
                mirror::Array::DataOffset(component_size).Int32Value());
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
      OpRegRegImm(cu, kOpAdd, TargetReg(kRet0), r_dst,
                  -mirror::Array::DataOffset(component_size).Int32Value());
    }
  } else if (!info->is_range) {
    // TUNING: interleave
    for (int i = 0; i < elems; i++) {
      RegLocation rl_arg = LoadValue(cu, info->args[i], kCoreReg);
      StoreBaseDisp(cu, TargetReg(kRet0),
                    mirror::Array::DataOffset(component_size).Int32Value() +
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

void Codegen::GenSput(CompilationUnit* cu, uint32_t field_idx, RegLocation rl_src,
                      bool is_long_or_double, bool is_object)
{
  int field_offset;
  int ssb_index;
  bool is_volatile;
  bool is_referrers_class;

  DexCompilationUnit m_unit(cu->class_loader, cu->class_linker, *cu->dex_file, cu->code_item,
                            cu->class_def_idx, cu->method_idx, cu->access_flags);

  bool fast_path =
      cu->compiler_driver->ComputeStaticFieldInfo(field_idx, &m_unit,
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
                   mirror::AbstractMethod::DeclaringClassOffset().Int32Value(), rBase);
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
                   mirror::AbstractMethod::DexCacheInitializedStaticStorageOffset().Int32Value(),
                   rBase);
      LoadWordDisp(cu, rBase,
                   mirror::Array::DataOffset(sizeof(mirror::Object*)).Int32Value() +
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
    if (is_object && !IsConstantNullRef(cu, rl_src)) {
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

void Codegen::GenSget(CompilationUnit* cu, uint32_t field_idx, RegLocation rl_dest,
                      bool is_long_or_double, bool is_object)
{
  int field_offset;
  int ssb_index;
  bool is_volatile;
  bool is_referrers_class;

  DexCompilationUnit m_unit(cu->class_loader, cu->class_linker,
                            *cu->dex_file, cu->code_item,
                            cu->class_def_idx, cu->method_idx,
                            cu->access_flags);

  bool fast_path =
      cu->compiler_driver->ComputeStaticFieldInfo(field_idx, &m_unit,
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
                   mirror::AbstractMethod::DeclaringClassOffset().Int32Value(), rBase);
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
                   mirror::AbstractMethod::DexCacheInitializedStaticStorageOffset().Int32Value(),
                   rBase);
      LoadWordDisp(cu, rBase,
                   mirror::Array::DataOffset(sizeof(mirror::Object*)).Int32Value() +
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
void Codegen::GenShowTarget(CompilationUnit* cu)
{
  DCHECK_NE(cu->instruction_set, kX86) << "unimplemented GenShowTarget";
  LIR* branch_over = OpCmpImmBranch(cu, kCondNe, TargetReg(kInvokeTgt), 0, NULL);
  LoadWordDisp(cu, TargetReg(kSelf), ENTRYPOINT_OFFSET(pDebugMe), TargetReg(kInvokeTgt));
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
  branch_over->target = target;
}

void Codegen::HandleSuspendLaunchPads(CompilationUnit *cu)
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

void Codegen::HandleIntrinsicLaunchPads(CompilationUnit *cu)
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

void Codegen::HandleThrowLaunchPads(CompilationUnit *cu)
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
      case kThrowConstantArrayBounds: // v1 is length reg (for Arm/Mips), v2 constant index
        // v1 holds the constant array index.  Mips/Arm uses v2 for length, x86 reloads.
        if (target_x86) {
          OpRegMem(cu, kOpMov, TargetReg(kArg1), v1, mirror::Array::LengthOffset().Int32Value());
        } else {
          OpRegCopy(cu, TargetReg(kArg1), v1);
        }
        // Make sure the following LoadConstant doesn't mess with kArg1.
        LockTemp(cu, TargetReg(kArg1));
        LoadConstant(cu, TargetReg(kArg0), v2);
        func_offset = ENTRYPOINT_OFFSET(pThrowArrayBoundsFromCode);
        break;
      case kThrowArrayBounds:
        // Move v1 (array index) to kArg0 and v2 (array length) to kArg1
        if (v2 != TargetReg(kArg0)) {
          OpRegCopy(cu, TargetReg(kArg0), v1);
          if (target_x86) {
            // x86 leaves the array pointer in v2, so load the array length that the handler expects
            OpRegMem(cu, kOpMov, TargetReg(kArg1), v2, mirror::Array::LengthOffset().Int32Value());
          } else {
            OpRegCopy(cu, TargetReg(kArg1), v2);
          }
        } else {
          if (v1 == TargetReg(kArg1)) {
            // Swap v1 and v2, using kArg2 as a temp
            OpRegCopy(cu, TargetReg(kArg2), v1);
            if (target_x86) {
              // x86 leaves the array pointer in v2; load the array length that the handler expects
              OpRegMem(cu, kOpMov, TargetReg(kArg1), v2, mirror::Array::LengthOffset().Int32Value());
            } else {
              OpRegCopy(cu, TargetReg(kArg1), v2);
            }
            OpRegCopy(cu, TargetReg(kArg0), TargetReg(kArg2));
          } else {
            if (target_x86) {
              // x86 leaves the array pointer in v2; load the array length that the handler expects
              OpRegMem(cu, kOpMov, TargetReg(kArg1), v2, mirror::Array::LengthOffset().Int32Value());
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

void Codegen::GenIGet(CompilationUnit* cu, uint32_t field_idx, int opt_flags, OpSize size,
                      RegLocation rl_dest, RegLocation rl_obj, bool is_long_or_double,
                      bool is_object)
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
        LoadBaseDispWide(cu, reg_ptr, 0, rl_result.low_reg, rl_result.high_reg, INVALID_SREG);
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

void Codegen::GenIPut(CompilationUnit* cu, uint32_t field_idx, int opt_flags, OpSize size,
                      RegLocation rl_src, RegLocation rl_obj, bool is_long_or_double,
                      bool is_object)
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
      if (is_object && !IsConstantNullRef(cu, rl_src)) {
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

void Codegen::GenConstClass(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest)
{
  RegLocation rl_method = LoadCurrMethod(cu);
  int res_reg = AllocTemp(cu);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  if (!cu->compiler_driver->CanAccessTypeWithoutChecks(cu->method_idx,
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
        mirror::AbstractMethod::DexCacheResolvedTypesOffset().Int32Value();
    LoadWordDisp(cu, rl_method.low_reg, dex_cache_offset, res_reg);
    int32_t offset_of_type =
        mirror::Array::DataOffset(sizeof(mirror::Class*)).Int32Value() + (sizeof(mirror::Class*)
                          * type_idx);
    LoadWordDisp(cu, res_reg, offset_of_type, rl_result.low_reg);
    if (!cu->compiler_driver->CanAssumeTypeIsPresentInDexCache(*cu->dex_file,
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

void Codegen::GenConstString(CompilationUnit* cu, uint32_t string_idx, RegLocation rl_dest)
{
  /* NOTE: Most strings should be available at compile time */
  int32_t offset_of_string = mirror::Array::DataOffset(sizeof(mirror::String*)).Int32Value() +
                 (sizeof(mirror::String*) * string_idx);
  if (!cu->compiler_driver->CanAssumeStringIsPresentInDexCache(
      *cu->dex_file, string_idx) || SLOW_STRING_PATH) {
    // slow path, resolve string if not in dex cache
    FlushAllRegs(cu);
    LockCallTemps(cu); // Using explicit registers
    LoadCurrMethodDirect(cu, TargetReg(kArg2));
    LoadWordDisp(cu, TargetReg(kArg2),
                 mirror::AbstractMethod::DexCacheStringsOffset().Int32Value(), TargetReg(kArg0));
    // Might call out to helper, which will return resolved string in kRet0
    int r_tgt = CallHelperSetup(cu, ENTRYPOINT_OFFSET(pResolveStringFromCode));
    LoadWordDisp(cu, TargetReg(kArg0), offset_of_string, TargetReg(kRet0));
    LoadConstant(cu, TargetReg(kArg1), string_idx);
    if (cu->instruction_set == kThumb2) {
      OpRegImm(cu, kOpCmp, TargetReg(kRet0), 0);  // Is resolved?
      GenBarrier(cu);
      // For testing, always force through helper
      if (!EXERCISE_SLOWEST_STRING_PATH) {
        OpIT(cu, kCondEq, "T");
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
                 mirror::AbstractMethod::DexCacheStringsOffset().Int32Value(), res_reg);
    LoadWordDisp(cu, res_reg, offset_of_string, rl_result.low_reg);
    StoreValue(cu, rl_dest, rl_result);
  }
}

/*
 * Let helper function take care of everything.  Will
 * call Class::NewInstanceFromCode(type_idx, method);
 */
void Codegen::GenNewInstance(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest)
{
  FlushAllRegs(cu);  /* Everything to home location */
  // alloc will always check for resolution, do we also need to verify
  // access because the verifier was unable to?
  int func_offset;
  if (cu->compiler_driver->CanAccessInstantiableTypeWithoutChecks(
      cu->method_idx, *cu->dex_file, type_idx)) {
    func_offset = ENTRYPOINT_OFFSET(pAllocObjectFromCode);
  } else {
    func_offset = ENTRYPOINT_OFFSET(pAllocObjectFromCodeWithAccessCheck);
  }
  CallRuntimeHelperImmMethod(cu, func_offset, type_idx, true);
  RegLocation rl_result = GetReturn(cu, false);
  StoreValue(cu, rl_dest, rl_result);
}

void Codegen::GenThrow(CompilationUnit* cu, RegLocation rl_src)
{
  FlushAllRegs(cu);
  CallRuntimeHelperRegLocation(cu, ENTRYPOINT_OFFSET(pDeliverException), rl_src, true);
}

void Codegen::GenInstanceof(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest,
                            RegLocation rl_src)
{
  FlushAllRegs(cu);
  // May generate a call - use explicit registers
  LockCallTemps(cu);
  LoadCurrMethodDirect(cu, TargetReg(kArg1));  // kArg1 <= current Method*
  int class_reg = TargetReg(kArg2);  // kArg2 will hold the Class*
  if (!cu->compiler_driver->CanAccessTypeWithoutChecks(cu->method_idx,
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
                 mirror::AbstractMethod::DexCacheResolvedTypesOffset().Int32Value(), class_reg);
    int32_t offset_of_type =
        mirror::Array::DataOffset(sizeof(mirror::Class*)).Int32Value() + (sizeof(mirror::Class*)
        * type_idx);
    LoadWordDisp(cu, class_reg, offset_of_type, class_reg);
    if (!cu->compiler_driver->CanAssumeTypeIsPresentInDexCache(
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
  DCHECK_EQ(mirror::Object::ClassOffset().Int32Value(), 0);
  LoadWordDisp(cu, TargetReg(kArg0),  mirror::Object::ClassOffset().Int32Value(), TargetReg(kArg1));
  /* kArg0 is ref, kArg1 is ref->klass_, kArg2 is class */
  LIR* call_inst;
  LIR* branchover = NULL;
  if (cu->instruction_set == kThumb2) {
    /* Uses conditional nullification */
    int r_tgt = LoadHelper(cu, ENTRYPOINT_OFFSET(pInstanceofNonTrivialFromCode));
    OpRegReg(cu, kOpCmp, TargetReg(kArg1), TargetReg(kArg2));  // Same?
    OpIT(cu, kCondEq, "EE");   // if-convert the test
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

void Codegen::GenCheckCast(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_src)
{
  FlushAllRegs(cu);
  // May generate a call - use explicit registers
  LockCallTemps(cu);
  LoadCurrMethodDirect(cu, TargetReg(kArg1));  // kArg1 <= current Method*
  int class_reg = TargetReg(kArg2);  // kArg2 will hold the Class*
  if (!cu->compiler_driver->CanAccessTypeWithoutChecks(cu->method_idx,
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
                 mirror::AbstractMethod::DexCacheResolvedTypesOffset().Int32Value(), class_reg);
    int32_t offset_of_type =
        mirror::Array::DataOffset(sizeof(mirror::Class*)).Int32Value() +
        (sizeof(mirror::Class*) * type_idx);
    LoadWordDisp(cu, class_reg, offset_of_type, class_reg);
    if (!cu->compiler_driver->CanAssumeTypeIsPresentInDexCache(
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
  DCHECK_EQ(mirror::Object::ClassOffset().Int32Value(), 0);
  LoadWordDisp(cu, TargetReg(kArg0), mirror::Object::ClassOffset().Int32Value(), TargetReg(kArg1));
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

void Codegen::GenLong3Addr(CompilationUnit* cu, OpKind first_op, OpKind second_op,
                           RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2)
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


void Codegen::GenShiftOpLong(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_shift)
{
  int func_offset = -1; // Make gcc happy

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
  }
  FlushAllRegs(cu);   /* Send everything to home location */
  CallRuntimeHelperRegLocationRegLocation(cu, func_offset, rl_src1, rl_shift, false);
  RegLocation rl_result = GetReturnWide(cu, false);
  StoreValueWide(cu, rl_dest, rl_result);
}


void Codegen::GenArithOpInt(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
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
  Codegen* cg = cu->cg.get();
  // No divide instruction for Arm, so check for more special cases
  if ((cu->instruction_set == kThumb2) && !IsPowerOfTwo(lit)) {
    return cg->SmallLiteralDivide(cu, dalvik_opcode, rl_src, rl_dest, lit);
  }
  int k = LowestSetBit(lit);
  if (k >= 30) {
    // Avoid special cases.
    return false;
  }
  bool div = (dalvik_opcode == Instruction::DIV_INT_LIT8 ||
      dalvik_opcode == Instruction::DIV_INT_LIT16);
  rl_src = cg->LoadValue(cu, rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  if (div) {
    int t_reg = AllocTemp(cu);
    if (lit == 2) {
      // Division by 2 is by far the most common division by constant.
      cg->OpRegRegImm(cu, kOpLsr, t_reg, rl_src.low_reg, 32 - k);
      cg->OpRegRegReg(cu, kOpAdd, t_reg, t_reg, rl_src.low_reg);
      cg->OpRegRegImm(cu, kOpAsr, rl_result.low_reg, t_reg, k);
    } else {
      cg->OpRegRegImm(cu, kOpAsr, t_reg, rl_src.low_reg, 31);
      cg->OpRegRegImm(cu, kOpLsr, t_reg, t_reg, 32 - k);
      cg->OpRegRegReg(cu, kOpAdd, t_reg, t_reg, rl_src.low_reg);
      cg->OpRegRegImm(cu, kOpAsr, rl_result.low_reg, t_reg, k);
    }
  } else {
    int t_reg1 = AllocTemp(cu);
    int t_reg2 = AllocTemp(cu);
    if (lit == 2) {
      cg->OpRegRegImm(cu, kOpLsr, t_reg1, rl_src.low_reg, 32 - k);
      cg->OpRegRegReg(cu, kOpAdd, t_reg2, t_reg1, rl_src.low_reg);
      cg->OpRegRegImm(cu, kOpAnd, t_reg2, t_reg2, lit -1);
      cg->OpRegRegReg(cu, kOpSub, rl_result.low_reg, t_reg2, t_reg1);
    } else {
      cg->OpRegRegImm(cu, kOpAsr, t_reg1, rl_src.low_reg, 31);
      cg->OpRegRegImm(cu, kOpLsr, t_reg1, t_reg1, 32 - k);
      cg->OpRegRegReg(cu, kOpAdd, t_reg2, t_reg1, rl_src.low_reg);
      cg->OpRegRegImm(cu, kOpAnd, t_reg2, t_reg2, lit - 1);
      cg->OpRegRegReg(cu, kOpSub, rl_result.low_reg, t_reg2, t_reg1);
    }
  }
  cg->StoreValue(cu, rl_dest, rl_result);
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
  Codegen* cg = cu->cg.get();
  rl_src = cg->LoadValue(cu, rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  if (power_of_two) {
    // Shift.
    cg->OpRegRegImm(cu, kOpLsl, rl_result.low_reg, rl_src.low_reg, LowestSetBit(lit));
  } else if (pop_count_le2) {
    // Shift and add and shift.
    int first_bit = LowestSetBit(lit);
    int second_bit = LowestSetBit(lit ^ (1 << first_bit));
    cg->GenMultiplyByTwoBitMultiplier(cu, rl_src, rl_result, lit, first_bit, second_bit);
  } else {
    // Reverse subtract: (src << (shift + 1)) - src.
    DCHECK(power_of_two_minus_one);
    // TUNING: rsb dst, src, src lsl#LowestSetBit(lit + 1)
    int t_reg = AllocTemp(cu);
    cg->OpRegRegImm(cu, kOpLsl, t_reg, rl_src.low_reg, LowestSetBit(lit + 1));
    cg->OpRegRegReg(cu, kOpSub, rl_result.low_reg, t_reg, rl_src.low_reg);
  }
  cg->StoreValue(cu, rl_dest, rl_result);
  return true;
}

void Codegen::GenArithOpIntLit(CompilationUnit* cu, Instruction::Code opcode,
                               RegLocation rl_dest, RegLocation rl_src, int lit)
{
  RegLocation rl_result;
  OpKind op = static_cast<OpKind>(0);    /* Make gcc happy */
  int shift_op = false;
  bool is_div = false;

  switch (opcode) {
    case Instruction::RSUB_INT_LIT8:
    case Instruction::RSUB_INT: {
      rl_src = LoadValue(cu, rl_src, kCoreReg);
      rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
      if (cu->instruction_set == kThumb2) {
        OpRegRegImm(cu, kOpRsub, rl_result.low_reg, rl_src.low_reg, lit);
      } else {
        OpRegReg(cu, kOpNeg, rl_result.low_reg, rl_src.low_reg);
        OpRegImm(cu, kOpAdd, rl_result.low_reg, lit);
      }
      StoreValue(cu, rl_dest, rl_result);
      return;
    }

    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      lit = -lit;
      // Intended fallthrough
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
    case Instruction::ADD_INT_LIT8:
    case Instruction::ADD_INT_LIT16:
      op = kOpAdd;
      break;
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::MUL_INT_LIT8:
    case Instruction::MUL_INT_LIT16: {
      if (HandleEasyMultiply(cu, rl_src, rl_dest, lit)) {
        return;
      }
      op = kOpMul;
      break;
    }
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
    case Instruction::AND_INT_LIT8:
    case Instruction::AND_INT_LIT16:
      op = kOpAnd;
      break;
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
    case Instruction::OR_INT_LIT8:
    case Instruction::OR_INT_LIT16:
      op = kOpOr;
      break;
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
    case Instruction::XOR_INT_LIT8:
    case Instruction::XOR_INT_LIT16:
      op = kOpXor;
      break;
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      lit &= 31;
      shift_op = true;
      op = kOpLsl;
      break;
    case Instruction::SHR_INT_LIT8:
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      lit &= 31;
      shift_op = true;
      op = kOpAsr;
      break;
    case Instruction::USHR_INT_LIT8:
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      lit &= 31;
      shift_op = true;
      op = kOpLsr;
      break;

    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
    case Instruction::DIV_INT_LIT8:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
    case Instruction::REM_INT_LIT8:
    case Instruction::REM_INT_LIT16: {
      if (lit == 0) {
        GenImmedCheck(cu, kCondAl, 0, 0, kThrowDivZero);
        return;
      }
      if (HandleEasyDivide(cu, opcode, rl_src, rl_dest, lit)) {
        return;
      }
      if ((opcode == Instruction::DIV_INT_LIT8) ||
          (opcode == Instruction::DIV_INT) ||
          (opcode == Instruction::DIV_INT_2ADDR) ||
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
      return;
    }
    default:
      LOG(FATAL) << "Unexpected opcode " << opcode;
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
}

void Codegen::GenArithOpLong(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
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
      return;
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      if (cu->instruction_set != kThumb2) {
        GenAddLong(cu, rl_dest, rl_src1, rl_src2);
        return;
      }
      first_op = kOpAdd;
      second_op = kOpAdc;
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if (cu->instruction_set != kThumb2) {
        GenSubLong(cu, rl_dest, rl_src1, rl_src2);
        return;
      }
      first_op = kOpSub;
      second_op = kOpSbc;
      break;
    case Instruction::MUL_LONG:
    case Instruction::MUL_LONG_2ADDR:
      if (cu->instruction_set == kThumb2) {
        GenMulLong(cu, rl_dest, rl_src1, rl_src2);
        return;
      } else {
        call_out = true;
        ret_reg = TargetReg(kRet0);
        func_offset = ENTRYPOINT_OFFSET(pLmul);
      }
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
        GenOrLong(cu, rl_dest, rl_src1, rl_src2);
        return;
      }
      first_op = kOpOr;
      second_op = kOpOr;
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      if (cu->instruction_set == kX86) {
        GenXorLong(cu, rl_dest, rl_src1, rl_src2);
        return;
      }
      first_op = kOpXor;
      second_op = kOpXor;
      break;
    case Instruction::NEG_LONG: {
      GenNegLong(cu, rl_dest, rl_src2);
      return;
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
}

void Codegen::GenConversionCall(CompilationUnit* cu, int func_offset,
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
}

/* Check if we need to check for pending suspend request */
void Codegen::GenSuspendTest(CompilationUnit* cu, int opt_flags)
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
void Codegen::GenSuspendTestAndBranch(CompilationUnit* cu, int opt_flags, LIR* target)
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
