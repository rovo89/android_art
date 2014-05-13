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

#include "dex/compiler_internals.h"
#include "dex/dataflow_iterator-inl.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "mir_to_lir-inl.h"
#include "object_utils.h"
#include "thread-inl.h"

namespace art {

void Mir2Lir::LockArg(int in_position, bool wide) {
  RegStorage reg_arg_low = GetArgMappingToPhysicalReg(in_position);
  RegStorage reg_arg_high = wide ? GetArgMappingToPhysicalReg(in_position + 1) :
      RegStorage::InvalidReg();

  if (reg_arg_low.Valid()) {
    LockTemp(reg_arg_low);
  }
  if (reg_arg_high.Valid() && reg_arg_low != reg_arg_high) {
    LockTemp(reg_arg_high);
  }
}

// TODO: needs revisit for 64-bit.
RegStorage Mir2Lir::LoadArg(int in_position, RegisterClass reg_class, bool wide) {
  RegStorage reg_arg_low = GetArgMappingToPhysicalReg(in_position);
  RegStorage reg_arg_high = wide ? GetArgMappingToPhysicalReg(in_position + 1) :
      RegStorage::InvalidReg();

  int offset = StackVisitor::GetOutVROffset(in_position, cu_->instruction_set);
  if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
    /*
     * When doing a call for x86, it moves the stack pointer in order to push return.
     * Thus, we add another 4 bytes to figure out the out of caller (in of callee).
     * TODO: This needs revisited for 64-bit.
     */
    offset += sizeof(uint32_t);
  }

  // If the VR is wide and there is no register for high part, we need to load it.
  if (wide && !reg_arg_high.Valid()) {
    // If the low part is not in a reg, we allocate a pair. Otherwise, we just load to high reg.
    if (!reg_arg_low.Valid()) {
      RegStorage new_regs = AllocTypedTempWide(false, reg_class);
      LoadBaseDisp(TargetReg(kSp), offset, new_regs, k64);
      return new_regs;  // The reg_class is OK, we can return.
    } else {
      // Assume that no ABI allows splitting a wide fp reg between a narrow fp reg and memory,
      // i.e. the low part is in a core reg. Load the second part in a core reg as well for now.
      DCHECK(!reg_arg_low.IsFloat());
      reg_arg_high = AllocTemp();
      int offset_high = offset + sizeof(uint32_t);
      Load32Disp(TargetReg(kSp), offset_high, reg_arg_high);
      // Continue below to check the reg_class.
    }
  }

  // If the low part is not in a register yet, we need to load it.
  if (!reg_arg_low.Valid()) {
    // Assume that if the low part of a wide arg is passed in memory, so is the high part,
    // thus we don't get here for wide args as it's handled above. Big-endian ABIs could
    // conceivably break this assumption but Android supports only little-endian architectures.
    DCHECK(!wide);
    reg_arg_low = AllocTypedTemp(false, reg_class);
    Load32Disp(TargetReg(kSp), offset, reg_arg_low);
    return reg_arg_low;  // The reg_class is OK, we can return.
  }

  RegStorage reg_arg = wide ? RegStorage::MakeRegPair(reg_arg_low, reg_arg_high) : reg_arg_low;
  // Check if we need to copy the arg to a different reg_class.
  if (!RegClassMatches(reg_class, reg_arg)) {
    if (wide) {
      RegStorage new_regs = AllocTypedTempWide(false, reg_class);
      OpRegCopyWide(new_regs, reg_arg);
      reg_arg = new_regs;
    } else {
      RegStorage new_reg = AllocTypedTemp(false, reg_class);
      OpRegCopy(new_reg, reg_arg);
      reg_arg = new_reg;
    }
  }
  return reg_arg;
}

void Mir2Lir::LoadArgDirect(int in_position, RegLocation rl_dest) {
  int offset = StackVisitor::GetOutVROffset(in_position, cu_->instruction_set);
  if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
    /*
     * When doing a call for x86, it moves the stack pointer in order to push return.
     * Thus, we add another 4 bytes to figure out the out of caller (in of callee).
     * TODO: This needs revisited for 64-bit.
     */
    offset += sizeof(uint32_t);
  }

  if (!rl_dest.wide) {
    RegStorage reg = GetArgMappingToPhysicalReg(in_position);
    if (reg.Valid()) {
      OpRegCopy(rl_dest.reg, reg);
    } else {
      Load32Disp(TargetReg(kSp), offset, rl_dest.reg);
    }
  } else {
    RegStorage reg_arg_low = GetArgMappingToPhysicalReg(in_position);
    RegStorage reg_arg_high = GetArgMappingToPhysicalReg(in_position + 1);

    if (reg_arg_low.Valid() && reg_arg_high.Valid()) {
      OpRegCopyWide(rl_dest.reg, RegStorage::MakeRegPair(reg_arg_low, reg_arg_high));
    } else if (reg_arg_low.Valid() && !reg_arg_high.Valid()) {
      OpRegCopy(rl_dest.reg, reg_arg_low);
      int offset_high = offset + sizeof(uint32_t);
      Load32Disp(TargetReg(kSp), offset_high, rl_dest.reg.GetHigh());
    } else if (!reg_arg_low.Valid() && reg_arg_high.Valid()) {
      OpRegCopy(rl_dest.reg.GetHigh(), reg_arg_high);
      Load32Disp(TargetReg(kSp), offset, rl_dest.reg.GetLow());
    } else {
      LoadBaseDisp(TargetReg(kSp), offset, rl_dest.reg, k64);
    }
  }
}

bool Mir2Lir::GenSpecialIGet(MIR* mir, const InlineMethod& special) {
  // FastInstance() already checked by DexFileMethodInliner.
  const InlineIGetIPutData& data = special.d.ifield_data;
  if (data.method_is_static != 0u || data.object_arg != 0u) {
    // The object is not "this" and has to be null-checked.
    return false;
  }

  bool wide = (data.op_variant == InlineMethodAnalyser::IGetVariant(Instruction::IGET_WIDE));
  bool ref = (data.op_variant == InlineMethodAnalyser::IGetVariant(Instruction::IGET_OBJECT));
  OpSize size = LoadStoreOpSize(wide, ref);
  if (data.is_volatile && !SupportsVolatileLoadStore(size)) {
    return false;
  }

  // The inliner doesn't distinguish kDouble or kFloat, use shorty.
  bool double_or_float = cu_->shorty[0] == 'F' || cu_->shorty[0] == 'D';

  // Point of no return - no aborts after this
  GenPrintLabel(mir);
  LockArg(data.object_arg);
  RegStorage reg_obj = LoadArg(data.object_arg, kCoreReg);
  RegLocation rl_dest = wide ? GetReturnWide(double_or_float) : GetReturn(double_or_float);
  RegisterClass reg_class = RegClassForFieldLoadStore(size, data.is_volatile);
  RegStorage r_result = rl_dest.reg;
  if (!RegClassMatches(reg_class, r_result)) {
    r_result = wide ? AllocTypedTempWide(rl_dest.fp, reg_class)
                    : AllocTypedTemp(rl_dest.fp, reg_class);
  }
  if (data.is_volatile) {
    LoadBaseDispVolatile(reg_obj, data.field_offset, r_result, size);
    // Without context sensitive analysis, we must issue the most conservative barriers.
    // In this case, either a load or store may follow so we issue both barriers.
    GenMemBarrier(kLoadLoad);
    GenMemBarrier(kLoadStore);
  } else {
    LoadBaseDisp(reg_obj, data.field_offset, r_result, size);
  }
  if (r_result != rl_dest.reg) {
    if (wide) {
      OpRegCopyWide(rl_dest.reg, r_result);
    } else {
      OpRegCopy(rl_dest.reg, r_result);
    }
  }
  return true;
}

bool Mir2Lir::GenSpecialIPut(MIR* mir, const InlineMethod& special) {
  // FastInstance() already checked by DexFileMethodInliner.
  const InlineIGetIPutData& data = special.d.ifield_data;
  if (data.method_is_static != 0u || data.object_arg != 0u) {
    // The object is not "this" and has to be null-checked.
    return false;
  }
  if (data.return_arg_plus1 != 0u) {
    // The setter returns a method argument which we don't support here.
    return false;
  }

  bool wide = (data.op_variant == InlineMethodAnalyser::IPutVariant(Instruction::IPUT_WIDE));
  bool ref = (data.op_variant == InlineMethodAnalyser::IGetVariant(Instruction::IGET_OBJECT));
  OpSize size = LoadStoreOpSize(wide, ref);
  if (data.is_volatile && !SupportsVolatileLoadStore(size)) {
    return false;
  }

  // Point of no return - no aborts after this
  GenPrintLabel(mir);
  LockArg(data.object_arg);
  LockArg(data.src_arg, wide);
  RegStorage reg_obj = LoadArg(data.object_arg, kCoreReg);
  RegisterClass reg_class = RegClassForFieldLoadStore(size, data.is_volatile);
  RegStorage reg_src = LoadArg(data.src_arg, reg_class, wide);
  if (data.is_volatile) {
    // There might have been a store before this volatile one so insert StoreStore barrier.
    GenMemBarrier(kStoreStore);
    StoreBaseDispVolatile(reg_obj, data.field_offset, reg_src, size);
    // A load might follow the volatile store so insert a StoreLoad barrier.
    GenMemBarrier(kStoreLoad);
  } else {
    StoreBaseDisp(reg_obj, data.field_offset, reg_src, size);
  }
  if (ref) {
    MarkGCCard(reg_src, reg_obj);
  }
  return true;
}

bool Mir2Lir::GenSpecialIdentity(MIR* mir, const InlineMethod& special) {
  const InlineReturnArgData& data = special.d.return_data;
  bool wide = (data.is_wide != 0u);
  // The inliner doesn't distinguish kDouble or kFloat, use shorty.
  bool double_or_float = cu_->shorty[0] == 'F' || cu_->shorty[0] == 'D';

  // Point of no return - no aborts after this
  GenPrintLabel(mir);
  LockArg(data.arg, wide);
  RegLocation rl_dest = wide ? GetReturnWide(double_or_float) : GetReturn(double_or_float);
  LoadArgDirect(data.arg, rl_dest);
  return true;
}

/*
 * Special-case code generation for simple non-throwing leaf methods.
 */
bool Mir2Lir::GenSpecialCase(BasicBlock* bb, MIR* mir, const InlineMethod& special) {
  DCHECK(special.flags & kInlineSpecial);
  current_dalvik_offset_ = mir->offset;
  MIR* return_mir = nullptr;
  bool successful = false;

  switch (special.opcode) {
    case kInlineOpNop:
      successful = true;
      DCHECK_EQ(mir->dalvikInsn.opcode, Instruction::RETURN_VOID);
      return_mir = mir;
      break;
    case kInlineOpNonWideConst: {
      successful = true;
      RegLocation rl_dest = GetReturn(cu_->shorty[0] == 'F');
      GenPrintLabel(mir);
      LoadConstant(rl_dest.reg, static_cast<int>(special.d.data));
      return_mir = bb->GetNextUnconditionalMir(mir_graph_, mir);
      break;
    }
    case kInlineOpReturnArg:
      successful = GenSpecialIdentity(mir, special);
      return_mir = mir;
      break;
    case kInlineOpIGet:
      successful = GenSpecialIGet(mir, special);
      return_mir = bb->GetNextUnconditionalMir(mir_graph_, mir);
      break;
    case kInlineOpIPut:
      successful = GenSpecialIPut(mir, special);
      return_mir = bb->GetNextUnconditionalMir(mir_graph_, mir);
      break;
    default:
      break;
  }

  if (successful) {
    if (kIsDebugBuild) {
      // Clear unreachable catch entries.
      mir_graph_->catches_.clear();
    }

    // Handle verbosity for return MIR.
    if (return_mir != nullptr) {
      current_dalvik_offset_ = return_mir->offset;
      // Not handling special identity case because it already generated code as part
      // of the return. The label should have been added before any code was generated.
      if (special.opcode != kInlineOpReturnArg) {
        GenPrintLabel(return_mir);
      }
    }
    GenSpecialExitSequence();

    core_spill_mask_ = 0;
    num_core_spills_ = 0;
    fp_spill_mask_ = 0;
    num_fp_spills_ = 0;
    frame_size_ = 0;
    core_vmap_table_.clear();
    fp_vmap_table_.clear();
  }

  return successful;
}

/*
 * Target-independent code generation.  Use only high-level
 * load/store utilities here, or target-dependent genXX() handlers
 * when necessary.
 */
void Mir2Lir::CompileDalvikInstruction(MIR* mir, BasicBlock* bb, LIR* label_list) {
  RegLocation rl_src[3];
  RegLocation rl_dest = mir_graph_->GetBadLoc();
  RegLocation rl_result = mir_graph_->GetBadLoc();
  Instruction::Code opcode = mir->dalvikInsn.opcode;
  int opt_flags = mir->optimization_flags;
  uint32_t vB = mir->dalvikInsn.vB;
  uint32_t vC = mir->dalvikInsn.vC;

  // Prep Src and Dest locations.
  int next_sreg = 0;
  int next_loc = 0;
  uint64_t attrs = MIRGraph::GetDataFlowAttributes(opcode);
  rl_src[0] = rl_src[1] = rl_src[2] = mir_graph_->GetBadLoc();
  if (attrs & DF_UA) {
    if (attrs & DF_A_WIDE) {
      rl_src[next_loc++] = mir_graph_->GetSrcWide(mir, next_sreg);
      next_sreg+= 2;
    } else {
      rl_src[next_loc++] = mir_graph_->GetSrc(mir, next_sreg);
      next_sreg++;
    }
  }
  if (attrs & DF_UB) {
    if (attrs & DF_B_WIDE) {
      rl_src[next_loc++] = mir_graph_->GetSrcWide(mir, next_sreg);
      next_sreg+= 2;
    } else {
      rl_src[next_loc++] = mir_graph_->GetSrc(mir, next_sreg);
      next_sreg++;
    }
  }
  if (attrs & DF_UC) {
    if (attrs & DF_C_WIDE) {
      rl_src[next_loc++] = mir_graph_->GetSrcWide(mir, next_sreg);
    } else {
      rl_src[next_loc++] = mir_graph_->GetSrc(mir, next_sreg);
    }
  }
  if (attrs & DF_DA) {
    if (attrs & DF_A_WIDE) {
      rl_dest = mir_graph_->GetDestWide(mir);
    } else {
      rl_dest = mir_graph_->GetDest(mir);
    }
  }
  switch (opcode) {
    case Instruction::NOP:
      break;

    case Instruction::MOVE_EXCEPTION:
      GenMoveException(rl_dest);
      break;

    case Instruction::RETURN_VOID:
      if (((cu_->access_flags & kAccConstructor) != 0) &&
          cu_->compiler_driver->RequiresConstructorBarrier(Thread::Current(), cu_->dex_file,
                                                          cu_->class_def_idx)) {
        GenMemBarrier(kStoreStore);
      }
      if (!mir_graph_->MethodIsLeaf()) {
        GenSuspendTest(opt_flags);
      }
      break;

    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT:
      if (!mir_graph_->MethodIsLeaf()) {
        GenSuspendTest(opt_flags);
      }
      StoreValue(GetReturn(cu_->shorty[0] == 'F'), rl_src[0]);
      break;

    case Instruction::RETURN_WIDE:
      if (!mir_graph_->MethodIsLeaf()) {
        GenSuspendTest(opt_flags);
      }
      StoreValueWide(GetReturnWide(cu_->shorty[0] == 'D'), rl_src[0]);
      break;

    case Instruction::MOVE_RESULT_WIDE:
      if ((opt_flags & MIR_INLINED) != 0) {
        break;  // Nop - combined w/ previous invoke.
      }
      StoreValueWide(rl_dest, GetReturnWide(rl_dest.fp));
      break;

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_OBJECT:
      if ((opt_flags & MIR_INLINED) != 0) {
        break;  // Nop - combined w/ previous invoke.
      }
      StoreValue(rl_dest, GetReturn(rl_dest.fp));
      break;

    case Instruction::MOVE:
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_16:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_OBJECT_FROM16:
      StoreValue(rl_dest, rl_src[0]);
      break;

    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_16:
    case Instruction::MOVE_WIDE_FROM16:
      StoreValueWide(rl_dest, rl_src[0]);
      break;

    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16:
      rl_result = EvalLoc(rl_dest, kAnyReg, true);
      LoadConstantNoClobber(rl_result.reg, vB);
      StoreValue(rl_dest, rl_result);
      if (vB == 0) {
        Workaround7250540(rl_dest, rl_result.reg);
      }
      break;

    case Instruction::CONST_HIGH16:
      rl_result = EvalLoc(rl_dest, kAnyReg, true);
      LoadConstantNoClobber(rl_result.reg, vB << 16);
      StoreValue(rl_dest, rl_result);
      if (vB == 0) {
        Workaround7250540(rl_dest, rl_result.reg);
      }
      break;

    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32:
      GenConstWide(rl_dest, static_cast<int64_t>(static_cast<int32_t>(vB)));
      break;

    case Instruction::CONST_WIDE:
      GenConstWide(rl_dest, mir->dalvikInsn.vB_wide);
      break;

    case Instruction::CONST_WIDE_HIGH16:
      rl_result = EvalLoc(rl_dest, kAnyReg, true);
      LoadConstantWide(rl_result.reg, static_cast<int64_t>(vB) << 48);
      StoreValueWide(rl_dest, rl_result);
      break;

    case Instruction::MONITOR_ENTER:
      GenMonitorEnter(opt_flags, rl_src[0]);
      break;

    case Instruction::MONITOR_EXIT:
      GenMonitorExit(opt_flags, rl_src[0]);
      break;

    case Instruction::CHECK_CAST: {
      GenCheckCast(mir->offset, vB, rl_src[0]);
      break;
    }
    case Instruction::INSTANCE_OF:
      GenInstanceof(vC, rl_dest, rl_src[0]);
      break;

    case Instruction::NEW_INSTANCE:
      GenNewInstance(vB, rl_dest);
      break;

    case Instruction::THROW:
      GenThrow(rl_src[0]);
      break;

    case Instruction::ARRAY_LENGTH:
      int len_offset;
      len_offset = mirror::Array::LengthOffset().Int32Value();
      rl_src[0] = LoadValue(rl_src[0], kCoreReg);
      GenNullCheck(rl_src[0].reg, opt_flags);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      Load32Disp(rl_src[0].reg, len_offset, rl_result.reg);
      MarkPossibleNullPointerException(opt_flags);
      StoreValue(rl_dest, rl_result);
      break;

    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      GenConstString(vB, rl_dest);
      break;

    case Instruction::CONST_CLASS:
      GenConstClass(vB, rl_dest);
      break;

    case Instruction::FILL_ARRAY_DATA:
      GenFillArrayData(vB, rl_src[0]);
      break;

    case Instruction::FILLED_NEW_ARRAY:
      GenFilledNewArray(mir_graph_->NewMemCallInfo(bb, mir, kStatic,
                        false /* not range */));
      break;

    case Instruction::FILLED_NEW_ARRAY_RANGE:
      GenFilledNewArray(mir_graph_->NewMemCallInfo(bb, mir, kStatic,
                        true /* range */));
      break;

    case Instruction::NEW_ARRAY:
      GenNewArray(vC, rl_dest, rl_src[0]);
      break;

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      if (mir_graph_->IsBackedge(bb, bb->taken)) {
        GenSuspendTestAndBranch(opt_flags, &label_list[bb->taken]);
      } else {
        OpUnconditionalBranch(&label_list[bb->taken]);
      }
      break;

    case Instruction::PACKED_SWITCH:
      GenPackedSwitch(mir, vB, rl_src[0]);
      break;

    case Instruction::SPARSE_SWITCH:
      GenSparseSwitch(mir, vB, rl_src[0]);
      break;

    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
      GenCmpFP(opcode, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::CMP_LONG:
      GenCmpLong(rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE: {
      LIR* taken = &label_list[bb->taken];
      LIR* fall_through = &label_list[bb->fall_through];
      // Result known at compile time?
      if (rl_src[0].is_const && rl_src[1].is_const) {
        bool is_taken = EvaluateBranch(opcode, mir_graph_->ConstantValue(rl_src[0].orig_sreg),
                                       mir_graph_->ConstantValue(rl_src[1].orig_sreg));
        BasicBlockId target_id = is_taken ? bb->taken : bb->fall_through;
        if (mir_graph_->IsBackedge(bb, target_id)) {
          GenSuspendTest(opt_flags);
        }
        OpUnconditionalBranch(&label_list[target_id]);
      } else {
        if (mir_graph_->IsBackwardsBranch(bb)) {
          GenSuspendTest(opt_flags);
        }
        GenCompareAndBranch(opcode, rl_src[0], rl_src[1], taken, fall_through);
      }
      break;
      }

    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ: {
      LIR* taken = &label_list[bb->taken];
      LIR* fall_through = &label_list[bb->fall_through];
      // Result known at compile time?
      if (rl_src[0].is_const) {
        bool is_taken = EvaluateBranch(opcode, mir_graph_->ConstantValue(rl_src[0].orig_sreg), 0);
        BasicBlockId target_id = is_taken ? bb->taken : bb->fall_through;
        if (mir_graph_->IsBackedge(bb, target_id)) {
          GenSuspendTest(opt_flags);
        }
        OpUnconditionalBranch(&label_list[target_id]);
      } else {
        if (mir_graph_->IsBackwardsBranch(bb)) {
          GenSuspendTest(opt_flags);
        }
        GenCompareZeroAndBranch(opcode, rl_src[0], taken, fall_through);
      }
      break;
      }

    case Instruction::AGET_WIDE:
      GenArrayGet(opt_flags, k64, rl_src[0], rl_src[1], rl_dest, 3);
      break;
    case Instruction::AGET_OBJECT:
      GenArrayGet(opt_flags, kReference, rl_src[0], rl_src[1], rl_dest, 2);
      break;
    case Instruction::AGET:
      GenArrayGet(opt_flags, k32, rl_src[0], rl_src[1], rl_dest, 2);
      break;
    case Instruction::AGET_BOOLEAN:
      GenArrayGet(opt_flags, kUnsignedByte, rl_src[0], rl_src[1], rl_dest, 0);
      break;
    case Instruction::AGET_BYTE:
      GenArrayGet(opt_flags, kSignedByte, rl_src[0], rl_src[1], rl_dest, 0);
      break;
    case Instruction::AGET_CHAR:
      GenArrayGet(opt_flags, kUnsignedHalf, rl_src[0], rl_src[1], rl_dest, 1);
      break;
    case Instruction::AGET_SHORT:
      GenArrayGet(opt_flags, kSignedHalf, rl_src[0], rl_src[1], rl_dest, 1);
      break;
    case Instruction::APUT_WIDE:
      GenArrayPut(opt_flags, k64, rl_src[1], rl_src[2], rl_src[0], 3, false);
      break;
    case Instruction::APUT:
      GenArrayPut(opt_flags, k32, rl_src[1], rl_src[2], rl_src[0], 2, false);
      break;
    case Instruction::APUT_OBJECT: {
      bool is_null = mir_graph_->IsConstantNullRef(rl_src[0]);
      bool is_safe = is_null;  // Always safe to store null.
      if (!is_safe) {
        // Check safety from verifier type information.
        const DexCompilationUnit* unit = mir_graph_->GetCurrentDexCompilationUnit();
        is_safe = cu_->compiler_driver->IsSafeCast(unit, mir->offset);
      }
      if (is_null || is_safe) {
        // Store of constant null doesn't require an assignability test and can be generated inline
        // without fixed register usage or a card mark.
        GenArrayPut(opt_flags, kReference, rl_src[1], rl_src[2], rl_src[0], 2, !is_null);
      } else {
        GenArrayObjPut(opt_flags, rl_src[1], rl_src[2], rl_src[0]);
      }
      break;
    }
    case Instruction::APUT_SHORT:
    case Instruction::APUT_CHAR:
      GenArrayPut(opt_flags, kUnsignedHalf, rl_src[1], rl_src[2], rl_src[0], 1, false);
      break;
    case Instruction::APUT_BYTE:
    case Instruction::APUT_BOOLEAN:
      GenArrayPut(opt_flags, kUnsignedByte, rl_src[1], rl_src[2], rl_src[0], 0, false);
      break;

    case Instruction::IGET_OBJECT:
      GenIGet(mir, opt_flags, kReference, rl_dest, rl_src[0], false, true);
      break;

    case Instruction::IGET_WIDE:
      GenIGet(mir, opt_flags, k64, rl_dest, rl_src[0], true, false);
      break;

    case Instruction::IGET:
      GenIGet(mir, opt_flags, k32, rl_dest, rl_src[0], false, false);
      break;

    case Instruction::IGET_CHAR:
      GenIGet(mir, opt_flags, kUnsignedHalf, rl_dest, rl_src[0], false, false);
      break;

    case Instruction::IGET_SHORT:
      GenIGet(mir, opt_flags, kSignedHalf, rl_dest, rl_src[0], false, false);
      break;

    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
      GenIGet(mir, opt_flags, kUnsignedByte, rl_dest, rl_src[0], false, false);
      break;

    case Instruction::IPUT_WIDE:
      GenIPut(mir, opt_flags, k64, rl_src[0], rl_src[1], true, false);
      break;

    case Instruction::IPUT_OBJECT:
      GenIPut(mir, opt_flags, kReference, rl_src[0], rl_src[1], false, true);
      break;

    case Instruction::IPUT:
      GenIPut(mir, opt_flags, k32, rl_src[0], rl_src[1], false, false);
      break;

    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
      GenIPut(mir, opt_flags, kUnsignedByte, rl_src[0], rl_src[1], false, false);
      break;

    case Instruction::IPUT_CHAR:
      GenIPut(mir, opt_flags, kUnsignedHalf, rl_src[0], rl_src[1], false, false);
      break;

    case Instruction::IPUT_SHORT:
      GenIPut(mir, opt_flags, kSignedHalf, rl_src[0], rl_src[1], false, false);
      break;

    case Instruction::SGET_OBJECT:
      GenSget(mir, rl_dest, false, true);
      break;
    case Instruction::SGET:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
      GenSget(mir, rl_dest, false, false);
      break;

    case Instruction::SGET_WIDE:
      GenSget(mir, rl_dest, true, false);
      break;

    case Instruction::SPUT_OBJECT:
      GenSput(mir, rl_src[0], false, true);
      break;

    case Instruction::SPUT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
      GenSput(mir, rl_src[0], false, false);
      break;

    case Instruction::SPUT_WIDE:
      GenSput(mir, rl_src[0], true, false);
      break;

    case Instruction::INVOKE_STATIC_RANGE:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kStatic, true));
      break;
    case Instruction::INVOKE_STATIC:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kStatic, false));
      break;

    case Instruction::INVOKE_DIRECT:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kDirect, false));
      break;
    case Instruction::INVOKE_DIRECT_RANGE:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kDirect, true));
      break;

    case Instruction::INVOKE_VIRTUAL:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kVirtual, false));
      break;
    case Instruction::INVOKE_VIRTUAL_RANGE:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kVirtual, true));
      break;

    case Instruction::INVOKE_SUPER:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kSuper, false));
      break;
    case Instruction::INVOKE_SUPER_RANGE:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kSuper, true));
      break;

    case Instruction::INVOKE_INTERFACE:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kInterface, false));
      break;
    case Instruction::INVOKE_INTERFACE_RANGE:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kInterface, true));
      break;

    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
      GenArithOpInt(opcode, rl_dest, rl_src[0], rl_src[0]);
      break;

    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      GenArithOpLong(opcode, rl_dest, rl_src[0], rl_src[0]);
      break;

    case Instruction::NEG_FLOAT:
      GenArithOpFloat(opcode, rl_dest, rl_src[0], rl_src[0]);
      break;

    case Instruction::NEG_DOUBLE:
      GenArithOpDouble(opcode, rl_dest, rl_src[0], rl_src[0]);
      break;

    case Instruction::INT_TO_LONG:
      GenIntToLong(rl_dest, rl_src[0]);
      break;

    case Instruction::LONG_TO_INT:
      rl_src[0] = UpdateLocWide(rl_src[0]);
      rl_src[0] = WideToNarrow(rl_src[0]);
      StoreValue(rl_dest, rl_src[0]);
      break;

    case Instruction::INT_TO_BYTE:
    case Instruction::INT_TO_SHORT:
    case Instruction::INT_TO_CHAR:
      GenIntNarrowing(opcode, rl_dest, rl_src[0]);
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
      GenConversion(opcode, rl_dest, rl_src[0]);
      break;


    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
      if (rl_src[0].is_const &&
          InexpensiveConstantInt(mir_graph_->ConstantValue(rl_src[0]))) {
        GenArithOpIntLit(opcode, rl_dest, rl_src[1],
                             mir_graph_->ConstantValue(rl_src[0].orig_sreg));
      } else if (rl_src[1].is_const &&
          InexpensiveConstantInt(mir_graph_->ConstantValue(rl_src[1]))) {
        GenArithOpIntLit(opcode, rl_dest, rl_src[0],
                             mir_graph_->ConstantValue(rl_src[1].orig_sreg));
      } else {
        GenArithOpInt(opcode, rl_dest, rl_src[0], rl_src[1]);
      }
      break;

    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      if (rl_src[1].is_const &&
          InexpensiveConstantInt(mir_graph_->ConstantValue(rl_src[1]))) {
        GenArithOpIntLit(opcode, rl_dest, rl_src[0], mir_graph_->ConstantValue(rl_src[1]));
      } else {
        GenArithOpInt(opcode, rl_dest, rl_src[0], rl_src[1]);
      }
      break;

    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
      if (rl_src[0].is_const || rl_src[1].is_const) {
        GenArithImmOpLong(opcode, rl_dest, rl_src[0], rl_src[1]);
        break;
      }
      // Note: intentional fallthrough.

    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
      GenArithOpLong(opcode, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
      if (rl_src[1].is_const) {
        GenShiftImmOpLong(opcode, rl_dest, rl_src[0], rl_src[1]);
      } else {
        GenShiftOpLong(opcode, rl_dest, rl_src[0], rl_src[1]);
      }
      break;

    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
      GenArithOpFloat(opcode, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
      GenArithOpDouble(opcode, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::RSUB_INT:
    case Instruction::ADD_INT_LIT16:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8:
    case Instruction::USHR_INT_LIT8:
      GenArithOpIntLit(opcode, rl_dest, rl_src[0], vC);
      break;

    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
  }
}  // NOLINT(readability/fn_size)

// Process extended MIR instructions
void Mir2Lir::HandleExtendedMethodMIR(BasicBlock* bb, MIR* mir) {
  switch (static_cast<ExtendedMIROpcode>(mir->dalvikInsn.opcode)) {
    case kMirOpCopy: {
      RegLocation rl_src = mir_graph_->GetSrc(mir, 0);
      RegLocation rl_dest = mir_graph_->GetDest(mir);
      StoreValue(rl_dest, rl_src);
      break;
    }
    case kMirOpFusedCmplFloat:
      GenFusedFPCmpBranch(bb, mir, false /*gt bias*/, false /*double*/);
      break;
    case kMirOpFusedCmpgFloat:
      GenFusedFPCmpBranch(bb, mir, true /*gt bias*/, false /*double*/);
      break;
    case kMirOpFusedCmplDouble:
      GenFusedFPCmpBranch(bb, mir, false /*gt bias*/, true /*double*/);
      break;
    case kMirOpFusedCmpgDouble:
      GenFusedFPCmpBranch(bb, mir, true /*gt bias*/, true /*double*/);
      break;
    case kMirOpFusedCmpLong:
      GenFusedLongCmpBranch(bb, mir);
      break;
    case kMirOpSelect:
      GenSelect(bb, mir);
      break;
    default:
      break;
  }
}

void Mir2Lir::GenPrintLabel(MIR* mir) {
  // Mark the beginning of a Dalvik instruction for line tracking.
  if (cu_->verbose) {
     char* inst_str = mir_graph_->GetDalvikDisassembly(mir);
     MarkBoundary(mir->offset, inst_str);
  }
}

// Handle the content in each basic block.
bool Mir2Lir::MethodBlockCodeGen(BasicBlock* bb) {
  if (bb->block_type == kDead) return false;
  current_dalvik_offset_ = bb->start_offset;
  MIR* mir;
  int block_id = bb->id;

  block_label_list_[block_id].operands[0] = bb->start_offset;

  // Insert the block label.
  block_label_list_[block_id].opcode = kPseudoNormalBlockLabel;
  block_label_list_[block_id].flags.fixup = kFixupLabel;
  AppendLIR(&block_label_list_[block_id]);

  LIR* head_lir = NULL;

  // If this is a catch block, export the start address.
  if (bb->catch_entry) {
    head_lir = NewLIR0(kPseudoExportedPC);
  }

  // Free temp registers and reset redundant store tracking.
  ClobberAllTemps();

  if (bb->block_type == kEntryBlock) {
    ResetRegPool();
    int start_vreg = cu_->num_dalvik_registers - cu_->num_ins;
    GenEntrySequence(&mir_graph_->reg_location_[start_vreg],
                         mir_graph_->reg_location_[mir_graph_->GetMethodSReg()]);
  } else if (bb->block_type == kExitBlock) {
    ResetRegPool();
    GenExitSequence();
  }

  for (mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    ResetRegPool();
    if (cu_->disable_opt & (1 << kTrackLiveTemps)) {
      ClobberAllTemps();
      // Reset temp allocation to minimize differences when A/B testing.
      reg_pool_->ResetNextTemp();
    }

    if (cu_->disable_opt & (1 << kSuppressLoads)) {
      ResetDefTracking();
    }

    // Reset temp tracking sanity check.
    if (kIsDebugBuild) {
      live_sreg_ = INVALID_SREG;
    }

    current_dalvik_offset_ = mir->offset;
    int opcode = mir->dalvikInsn.opcode;

    GenPrintLabel(mir);

    // Remember the first LIR for this block.
    if (head_lir == NULL) {
      head_lir = &block_label_list_[bb->id];
      // Set the first label as a scheduling barrier.
      DCHECK(!head_lir->flags.use_def_invalid);
      head_lir->u.m.def_mask = ENCODE_ALL;
    }

    if (opcode == kMirOpCheck) {
      // Combine check and work halves of throwing instruction.
      MIR* work_half = mir->meta.throw_insn;
      mir->dalvikInsn.opcode = work_half->dalvikInsn.opcode;
      mir->meta = work_half->meta;  // Whatever the work_half had, we need to copy it.
      opcode = work_half->dalvikInsn.opcode;
      SSARepresentation* ssa_rep = work_half->ssa_rep;
      work_half->ssa_rep = mir->ssa_rep;
      mir->ssa_rep = ssa_rep;
      work_half->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpCheckPart2);
      work_half->meta.throw_insn = mir;
    }

    if (opcode >= kMirOpFirst) {
      HandleExtendedMethodMIR(bb, mir);
      continue;
    }

    CompileDalvikInstruction(mir, bb, block_label_list_);
  }

  if (head_lir) {
    // Eliminate redundant loads/stores and delay stores into later slots.
    ApplyLocalOptimizations(head_lir, last_lir_insn_);
  }
  return false;
}

bool Mir2Lir::SpecialMIR2LIR(const InlineMethod& special) {
  cu_->NewTimingSplit("SpecialMIR2LIR");
  // Find the first DalvikByteCode block.
  int num_reachable_blocks = mir_graph_->GetNumReachableBlocks();
  BasicBlock*bb = NULL;
  for (int idx = 0; idx < num_reachable_blocks; idx++) {
    // TODO: no direct access of growable lists.
    int dfs_index = mir_graph_->GetDfsOrder()->Get(idx);
    bb = mir_graph_->GetBasicBlock(dfs_index);
    if (bb->block_type == kDalvikByteCode) {
      break;
    }
  }
  if (bb == NULL) {
    return false;
  }
  DCHECK_EQ(bb->start_offset, 0);
  DCHECK(bb->first_mir_insn != NULL);

  // Get the first instruction.
  MIR* mir = bb->first_mir_insn;

  // Free temp registers and reset redundant store tracking.
  ResetRegPool();
  ResetDefTracking();
  ClobberAllTemps();

  return GenSpecialCase(bb, mir, special);
}

void Mir2Lir::MethodMIR2LIR() {
  cu_->NewTimingSplit("MIR2LIR");

  // Hold the labels of each block.
  block_label_list_ =
      static_cast<LIR*>(arena_->Alloc(sizeof(LIR) * mir_graph_->GetNumBlocks(),
                                      kArenaAllocLIR));

  PreOrderDfsIterator iter(mir_graph_);
  BasicBlock* curr_bb = iter.Next();
  BasicBlock* next_bb = iter.Next();
  while (curr_bb != NULL) {
    MethodBlockCodeGen(curr_bb);
    // If the fall_through block is no longer laid out consecutively, drop in a branch.
    BasicBlock* curr_bb_fall_through = mir_graph_->GetBasicBlock(curr_bb->fall_through);
    if ((curr_bb_fall_through != NULL) && (curr_bb_fall_through != next_bb)) {
      OpUnconditionalBranch(&block_label_list_[curr_bb->fall_through]);
    }
    curr_bb = next_bb;
    do {
      next_bb = iter.Next();
    } while ((next_bb != NULL) && (next_bb->block_type == kDead));
  }
  HandleSlowPaths();
}

//
// LIR Slow Path
//

LIR* Mir2Lir::LIRSlowPath::GenerateTargetLabel(int opcode) {
  m2l_->SetCurrentDexPc(current_dex_pc_);
  LIR* target = m2l_->NewLIR0(opcode);
  fromfast_->target = target;
  return target;
}

}  // namespace art
