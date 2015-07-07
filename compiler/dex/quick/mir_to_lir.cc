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

#include "mir_to_lir-inl.h"

#include "dex/dataflow_iterator-inl.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "driver/compiler_driver.h"
#include "primitive.h"
#include "thread-inl.h"

namespace art {

class Mir2Lir::SpecialSuspendCheckSlowPath : public Mir2Lir::LIRSlowPath {
 public:
  SpecialSuspendCheckSlowPath(Mir2Lir* m2l, LIR* branch, LIR* cont)
      : LIRSlowPath(m2l, branch, cont),
        num_used_args_(0u) {
  }

  void PreserveArg(int in_position) {
    // Avoid duplicates.
    for (size_t i = 0; i != num_used_args_; ++i) {
      if (used_args_[i] == in_position) {
        return;
      }
    }
    DCHECK_LT(num_used_args_, kMaxArgsToPreserve);
    used_args_[num_used_args_] = in_position;
    ++num_used_args_;
  }

  void Compile() OVERRIDE {
    m2l_->ResetRegPool();
    m2l_->ResetDefTracking();
    GenerateTargetLabel(kPseudoSuspendTarget);

    m2l_->LockCallTemps();

    // Generate frame.
    m2l_->GenSpecialEntryForSuspend();

    // Spill all args.
    for (size_t i = 0, end = m2l_->in_to_reg_storage_mapping_.GetEndMappedIn(); i < end;
        i += m2l_->in_to_reg_storage_mapping_.GetShorty(i).IsWide() ? 2u : 1u) {
      m2l_->SpillArg(i);
    }

    m2l_->FreeCallTemps();

    // Do the actual suspend call to runtime.
    m2l_->CallRuntimeHelper(kQuickTestSuspend, true);

    m2l_->LockCallTemps();

    // Unspill used regs. (Don't unspill unused args.)
    for (size_t i = 0; i != num_used_args_; ++i) {
      m2l_->UnspillArg(used_args_[i]);
    }

    // Pop the frame.
    m2l_->GenSpecialExitForSuspend();

    // Branch to the continue label.
    DCHECK(cont_ != nullptr);
    m2l_->OpUnconditionalBranch(cont_);

    m2l_->FreeCallTemps();
  }

 private:
  static constexpr size_t kMaxArgsToPreserve = 2u;
  size_t num_used_args_;
  int used_args_[kMaxArgsToPreserve];
};

RegisterClass Mir2Lir::ShortyToRegClass(char shorty_type) {
  RegisterClass res;
  switch (shorty_type) {
    case 'L':
      res = kRefReg;
      break;
    case 'F':
      // Expected fallthrough.
    case 'D':
      res = kFPReg;
      break;
    default:
      res = kCoreReg;
  }
  return res;
}

void Mir2Lir::LockArg(size_t in_position) {
  RegStorage reg_arg = in_to_reg_storage_mapping_.GetReg(in_position);

  if (reg_arg.Valid()) {
    LockTemp(reg_arg);
  }
}

RegStorage Mir2Lir::LoadArg(size_t in_position, RegisterClass reg_class, bool wide) {
  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
  int offset = StackVisitor::GetOutVROffset(in_position, cu_->instruction_set);

  if (cu_->instruction_set == kX86) {
    /*
     * When doing a call for x86, it moves the stack pointer in order to push return.
     * Thus, we add another 4 bytes to figure out the out of caller (in of callee).
     */
    offset += sizeof(uint32_t);
  }

  if (cu_->instruction_set == kX86_64) {
    /*
     * When doing a call for x86, it moves the stack pointer in order to push return.
     * Thus, we add another 8 bytes to figure out the out of caller (in of callee).
     */
    offset += sizeof(uint64_t);
  }

  RegStorage reg_arg = in_to_reg_storage_mapping_.GetReg(in_position);

  // TODO: REVISIT: This adds a spill of low part while we could just copy it.
  if (reg_arg.Valid() && wide && (reg_arg.GetWideKind() == kNotWide)) {
    // For wide register we've got only half of it.
    // Flush it to memory then.
    StoreBaseDisp(TargetPtrReg(kSp), offset, reg_arg, k32, kNotVolatile);
    reg_arg = RegStorage::InvalidReg();
  }

  if (!reg_arg.Valid()) {
    reg_arg = wide ?  AllocTypedTempWide(false, reg_class) : AllocTypedTemp(false, reg_class);
    LoadBaseDisp(TargetPtrReg(kSp), offset, reg_arg, wide ? k64 : k32, kNotVolatile);
  } else {
    // Check if we need to copy the arg to a different reg_class.
    if (!RegClassMatches(reg_class, reg_arg)) {
      if (wide) {
        RegStorage new_reg = AllocTypedTempWide(false, reg_class);
        OpRegCopyWide(new_reg, reg_arg);
        reg_arg = new_reg;
      } else {
        RegStorage new_reg = AllocTypedTemp(false, reg_class);
        OpRegCopy(new_reg, reg_arg);
        reg_arg = new_reg;
      }
    }
  }
  return reg_arg;
}

void Mir2Lir::LoadArgDirect(size_t in_position, RegLocation rl_dest) {
  DCHECK_EQ(rl_dest.location, kLocPhysReg);
  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
  int offset = StackVisitor::GetOutVROffset(in_position, cu_->instruction_set);
  if (cu_->instruction_set == kX86) {
    /*
     * When doing a call for x86, it moves the stack pointer in order to push return.
     * Thus, we add another 4 bytes to figure out the out of caller (in of callee).
     */
    offset += sizeof(uint32_t);
  }

  if (cu_->instruction_set == kX86_64) {
    /*
     * When doing a call for x86, it moves the stack pointer in order to push return.
     * Thus, we add another 8 bytes to figure out the out of caller (in of callee).
     */
    offset += sizeof(uint64_t);
  }

  RegStorage reg_arg = in_to_reg_storage_mapping_.GetReg(in_position);

  // TODO: REVISIT: This adds a spill of low part while we could just copy it.
  if (reg_arg.Valid() && rl_dest.wide && (reg_arg.GetWideKind() == kNotWide)) {
    // For wide register we've got only half of it.
    // Flush it to memory then.
    StoreBaseDisp(TargetPtrReg(kSp), offset, reg_arg, k32, kNotVolatile);
    reg_arg = RegStorage::InvalidReg();
  }

  if (!reg_arg.Valid()) {
    OpSize op_size = rl_dest.wide ? k64 : (rl_dest.ref ? kReference : k32);
    LoadBaseDisp(TargetPtrReg(kSp), offset, rl_dest.reg, op_size, kNotVolatile);
  } else {
    if (rl_dest.wide) {
      OpRegCopyWide(rl_dest.reg, reg_arg);
    } else {
      OpRegCopy(rl_dest.reg, reg_arg);
    }
  }
}

void Mir2Lir::SpillArg(size_t in_position) {
  RegStorage reg_arg = in_to_reg_storage_mapping_.GetReg(in_position);

  if (reg_arg.Valid()) {
    int offset = frame_size_ + StackVisitor::GetOutVROffset(in_position, cu_->instruction_set);
    ShortyArg arg = in_to_reg_storage_mapping_.GetShorty(in_position);
    OpSize size = arg.IsRef() ? kReference :
        (arg.IsWide() && reg_arg.GetWideKind() == kWide) ? k64 : k32;
    StoreBaseDisp(TargetPtrReg(kSp), offset, reg_arg, size, kNotVolatile);
  }
}

void Mir2Lir::UnspillArg(size_t in_position) {
  RegStorage reg_arg = in_to_reg_storage_mapping_.GetReg(in_position);

  if (reg_arg.Valid()) {
    int offset = frame_size_ + StackVisitor::GetOutVROffset(in_position, cu_->instruction_set);
    ShortyArg arg = in_to_reg_storage_mapping_.GetShorty(in_position);
    OpSize size = arg.IsRef() ? kReference :
        (arg.IsWide() && reg_arg.GetWideKind() == kWide) ? k64 : k32;
    LoadBaseDisp(TargetPtrReg(kSp), offset, reg_arg, size, kNotVolatile);
  }
}

Mir2Lir::SpecialSuspendCheckSlowPath* Mir2Lir::GenSpecialSuspendTest() {
  LockCallTemps();
  LIR* branch = OpTestSuspend(nullptr);
  FreeCallTemps();
  LIR* cont = NewLIR0(kPseudoTargetLabel);
  SpecialSuspendCheckSlowPath* slow_path =
      new (arena_) SpecialSuspendCheckSlowPath(this, branch, cont);
  AddSlowPath(slow_path);
  return slow_path;
}

bool Mir2Lir::GenSpecialIGet(MIR* mir, const InlineMethod& special) {
  // FastInstance() already checked by DexFileMethodInliner.
  const InlineIGetIPutData& data = special.d.ifield_data;
  if (data.method_is_static != 0u || data.object_arg != 0u) {
    // The object is not "this" and has to be null-checked.
    return false;
  }

  OpSize size;
  switch (data.op_variant) {
    case InlineMethodAnalyser::IGetVariant(Instruction::IGET):
      size = in_to_reg_storage_mapping_.GetShorty(data.src_arg).IsFP() ? kSingle : k32;
      break;
    case InlineMethodAnalyser::IGetVariant(Instruction::IGET_WIDE):
      size = in_to_reg_storage_mapping_.GetShorty(data.src_arg).IsFP() ? kDouble : k64;
      break;
    case InlineMethodAnalyser::IGetVariant(Instruction::IGET_OBJECT):
      size = kReference;
      break;
    case InlineMethodAnalyser::IGetVariant(Instruction::IGET_SHORT):
      size = kSignedHalf;
      break;
    case InlineMethodAnalyser::IGetVariant(Instruction::IGET_CHAR):
      size = kUnsignedHalf;
      break;
    case InlineMethodAnalyser::IGetVariant(Instruction::IGET_BYTE):
      size = kSignedByte;
      break;
    case InlineMethodAnalyser::IGetVariant(Instruction::IGET_BOOLEAN):
      size = kUnsignedByte;
      break;
    default:
      LOG(FATAL) << "Unknown variant: " << data.op_variant;
      UNREACHABLE();
  }

  // Point of no return - no aborts after this
  if (!kLeafOptimization) {
    auto* slow_path = GenSpecialSuspendTest();
    slow_path->PreserveArg(data.object_arg);
  }
  LockArg(data.object_arg);
  GenPrintLabel(mir);
  RegStorage reg_obj = LoadArg(data.object_arg, kRefReg);
  RegisterClass reg_class = RegClassForFieldLoadStore(size, data.is_volatile);
  RegisterClass ret_reg_class = ShortyToRegClass(cu_->shorty[0]);
  RegLocation rl_dest = IsWide(size) ? GetReturnWide(ret_reg_class) : GetReturn(ret_reg_class);
  RegStorage r_result = rl_dest.reg;
  if (!RegClassMatches(reg_class, r_result)) {
    r_result = IsWide(size) ? AllocTypedTempWide(rl_dest.fp, reg_class)
                            : AllocTypedTemp(rl_dest.fp, reg_class);
  }
  if (IsRef(size)) {
    LoadRefDisp(reg_obj, data.field_offset, r_result, data.is_volatile ? kVolatile : kNotVolatile);
  } else {
    LoadBaseDisp(reg_obj, data.field_offset, r_result, size, data.is_volatile ? kVolatile :
        kNotVolatile);
  }
  if (r_result.NotExactlyEquals(rl_dest.reg)) {
    if (IsWide(size)) {
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

  OpSize size;
  switch (data.op_variant) {
    case InlineMethodAnalyser::IPutVariant(Instruction::IPUT):
      size = in_to_reg_storage_mapping_.GetShorty(data.src_arg).IsFP() ? kSingle : k32;
      break;
    case InlineMethodAnalyser::IPutVariant(Instruction::IPUT_WIDE):
      size = in_to_reg_storage_mapping_.GetShorty(data.src_arg).IsFP() ? kDouble : k64;
      break;
    case InlineMethodAnalyser::IPutVariant(Instruction::IPUT_OBJECT):
      size = kReference;
      break;
    case InlineMethodAnalyser::IPutVariant(Instruction::IPUT_SHORT):
      size = kSignedHalf;
      break;
    case InlineMethodAnalyser::IPutVariant(Instruction::IPUT_CHAR):
      size = kUnsignedHalf;
      break;
    case InlineMethodAnalyser::IPutVariant(Instruction::IPUT_BYTE):
      size = kSignedByte;
      break;
    case InlineMethodAnalyser::IPutVariant(Instruction::IPUT_BOOLEAN):
      size = kUnsignedByte;
      break;
    default:
      LOG(FATAL) << "Unknown variant: " << data.op_variant;
      UNREACHABLE();
  }

  // Point of no return - no aborts after this
  if (!kLeafOptimization) {
    auto* slow_path = GenSpecialSuspendTest();
    slow_path->PreserveArg(data.object_arg);
    slow_path->PreserveArg(data.src_arg);
  }
  LockArg(data.object_arg);
  LockArg(data.src_arg);
  GenPrintLabel(mir);
  RegStorage reg_obj = LoadArg(data.object_arg, kRefReg);
  RegisterClass reg_class = RegClassForFieldLoadStore(size, data.is_volatile);
  RegStorage reg_src = LoadArg(data.src_arg, reg_class, IsWide(size));
  if (IsRef(size)) {
    StoreRefDisp(reg_obj, data.field_offset, reg_src, data.is_volatile ? kVolatile : kNotVolatile);
  } else {
    StoreBaseDisp(reg_obj, data.field_offset, reg_src, size, data.is_volatile ? kVolatile :
        kNotVolatile);
  }
  if (IsRef(size)) {
    MarkGCCard(0, reg_src, reg_obj);
  }
  return true;
}

bool Mir2Lir::GenSpecialIdentity(MIR* mir, const InlineMethod& special) {
  const InlineReturnArgData& data = special.d.return_data;
  bool wide = (data.is_wide != 0u);

  // Point of no return - no aborts after this
  if (!kLeafOptimization) {
    auto* slow_path = GenSpecialSuspendTest();
    slow_path->PreserveArg(data.arg);
  }
  LockArg(data.arg);
  GenPrintLabel(mir);
  RegisterClass reg_class = ShortyToRegClass(cu_->shorty[0]);
  RegLocation rl_dest = wide ? GetReturnWide(reg_class) : GetReturn(reg_class);
  LoadArgDirect(data.arg, rl_dest);
  return true;
}

/*
 * Special-case code generation for simple non-throwing leaf methods.
 */
bool Mir2Lir::GenSpecialCase(BasicBlock* bb, MIR* mir, const InlineMethod& special) {
  DCHECK(special.flags & kInlineSpecial);
  current_dalvik_offset_ = mir->offset;
  DCHECK(current_mir_ == nullptr);  // Safepoints attributed to prologue.
  MIR* return_mir = nullptr;
  bool successful = false;
  EnsureInitializedArgMappingToPhysicalReg();

  switch (special.opcode) {
    case kInlineOpNop:
      successful = true;
      DCHECK_EQ(mir->dalvikInsn.opcode, Instruction::RETURN_VOID);
      if (!kLeafOptimization) {
        GenSpecialSuspendTest();
      }
      return_mir = mir;
      break;
    case kInlineOpNonWideConst: {
      successful = true;
      if (!kLeafOptimization) {
        GenSpecialSuspendTest();
      }
      RegLocation rl_dest = GetReturn(ShortyToRegClass(cu_->shorty[0]));
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

    if (!kLeafOptimization) {
      HandleSlowPaths();
    } else {
      core_spill_mask_ = 0;
      num_core_spills_ = 0;
      fp_spill_mask_ = 0;
      num_fp_spills_ = 0;
      frame_size_ = 0;
      core_vmap_table_.clear();
      fp_vmap_table_.clear();
    }
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
  const Instruction::Code opcode = mir->dalvikInsn.opcode;
  const int opt_flags = mir->optimization_flags;
  const uint32_t vB = mir->dalvikInsn.vB;
  const uint32_t vC = mir->dalvikInsn.vC;
  DCHECK(CheckCorePoolSanity()) << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " @ 0x:"
                                << std::hex << current_dalvik_offset_;

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

    case Instruction::RETURN_VOID_NO_BARRIER:
    case Instruction::RETURN_VOID:
      if (((cu_->access_flags & kAccConstructor) != 0) &&
          cu_->compiler_driver->RequiresConstructorBarrier(Thread::Current(), cu_->dex_file,
                                                          cu_->class_def_idx)) {
        GenMemBarrier(kStoreStore);
      }
      if (!kLeafOptimization || !mir_graph_->MethodIsLeaf()) {
        GenSuspendTest(opt_flags);
      }
      break;

    case Instruction::RETURN_OBJECT:
      DCHECK(rl_src[0].ref);
      FALLTHROUGH_INTENDED;
    case Instruction::RETURN:
      if (!kLeafOptimization || !mir_graph_->MethodIsLeaf()) {
        GenSuspendTest(opt_flags);
      }
      StoreValue(GetReturn(ShortyToRegClass(cu_->shorty[0])), rl_src[0]);
      break;

    case Instruction::RETURN_WIDE:
      if (!kLeafOptimization || !mir_graph_->MethodIsLeaf()) {
        GenSuspendTest(opt_flags);
      }
      StoreValueWide(GetReturnWide(ShortyToRegClass(cu_->shorty[0])), rl_src[0]);
      break;

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_WIDE:
    case Instruction::MOVE_RESULT_OBJECT:
      // Already processed with invoke or filled-new-array.
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
      GenConst(rl_dest, vB);
      break;

    case Instruction::CONST_HIGH16:
      GenConst(rl_dest, vB << 16);
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
      GenCheckCast(opt_flags, mir->offset, vB, rl_src[0]);
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

    case Instruction::ARRAY_LENGTH: {
      int len_offset;
      len_offset = mirror::Array::LengthOffset().Int32Value();
      rl_src[0] = LoadValue(rl_src[0], kRefReg);
      GenNullCheck(rl_src[0].reg, opt_flags);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      Load32Disp(rl_src[0].reg, len_offset, rl_result.reg);
      MarkPossibleNullPointerException(opt_flags);
      StoreValue(rl_dest, rl_result);
      break;
    }
    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      GenConstString(vB, rl_dest);
      break;

    case Instruction::CONST_CLASS:
      GenConstClass(vB, rl_dest);
      break;

    case Instruction::FILL_ARRAY_DATA:
      GenFillArrayData(mir, vB, rl_src[0]);
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
      if (mir_graph_->IsBackEdge(bb, bb->taken)) {
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
      if (mir_graph_->IsBackEdge(bb, bb->taken) || mir_graph_->IsBackEdge(bb, bb->fall_through)) {
        GenSuspendTest(opt_flags);
      }
      LIR* taken = &label_list[bb->taken];
      GenCompareAndBranch(opcode, rl_src[0], rl_src[1], taken);
      break;
    }
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ: {
      if (mir_graph_->IsBackEdge(bb, bb->taken) || mir_graph_->IsBackEdge(bb, bb->fall_through)) {
        GenSuspendTest(opt_flags);
      }
      LIR* taken = &label_list[bb->taken];
      GenCompareZeroAndBranch(opcode, rl_src[0], taken);
      break;
    }

    case Instruction::AGET_WIDE:
      GenArrayGet(opt_flags, rl_dest.fp ? kDouble : k64, rl_src[0], rl_src[1], rl_dest, 3);
      break;
    case Instruction::AGET_OBJECT:
      GenArrayGet(opt_flags, kReference, rl_src[0], rl_src[1], rl_dest, 2);
      break;
    case Instruction::AGET:
      GenArrayGet(opt_flags, rl_dest.fp ? kSingle : k32, rl_src[0], rl_src[1], rl_dest, 2);
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
      GenArrayPut(opt_flags, rl_src[0].fp ? kDouble : k64, rl_src[1], rl_src[2], rl_src[0], 3, false);
      break;
    case Instruction::APUT:
      GenArrayPut(opt_flags, rl_src[0].fp ? kSingle : k32, rl_src[1], rl_src[2], rl_src[0], 2, false);
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

    case Instruction::IGET_OBJECT_QUICK:
    case Instruction::IGET_OBJECT:
      GenIGet(mir, opt_flags, kReference, Primitive::kPrimNot, rl_dest, rl_src[0]);
      break;

    case Instruction::IGET_WIDE_QUICK:
    case Instruction::IGET_WIDE:
      // kPrimLong and kPrimDouble share the same entrypoints.
      if (rl_dest.fp) {
        GenIGet(mir, opt_flags, kDouble, Primitive::kPrimDouble, rl_dest, rl_src[0]);
      } else {
        GenIGet(mir, opt_flags, k64, Primitive::kPrimLong, rl_dest, rl_src[0]);
      }
      break;

    case Instruction::IGET_QUICK:
    case Instruction::IGET:
      if (rl_dest.fp) {
        GenIGet(mir, opt_flags, kSingle, Primitive::kPrimFloat, rl_dest, rl_src[0]);
      } else {
        GenIGet(mir, opt_flags, k32, Primitive::kPrimInt, rl_dest, rl_src[0]);
      }
      break;

    case Instruction::IGET_CHAR_QUICK:
    case Instruction::IGET_CHAR:
      GenIGet(mir, opt_flags, kUnsignedHalf, Primitive::kPrimChar, rl_dest, rl_src[0]);
      break;

    case Instruction::IGET_SHORT_QUICK:
    case Instruction::IGET_SHORT:
      GenIGet(mir, opt_flags, kSignedHalf, Primitive::kPrimShort, rl_dest, rl_src[0]);
      break;

    case Instruction::IGET_BOOLEAN_QUICK:
    case Instruction::IGET_BOOLEAN:
      GenIGet(mir, opt_flags, kUnsignedByte, Primitive::kPrimBoolean, rl_dest, rl_src[0]);
      break;

    case Instruction::IGET_BYTE_QUICK:
    case Instruction::IGET_BYTE:
      GenIGet(mir, opt_flags, kSignedByte, Primitive::kPrimByte, rl_dest, rl_src[0]);
      break;

    case Instruction::IPUT_WIDE_QUICK:
    case Instruction::IPUT_WIDE:
      GenIPut(mir, opt_flags, rl_src[0].fp ? kDouble : k64, rl_src[0], rl_src[1]);
      break;

    case Instruction::IPUT_OBJECT_QUICK:
    case Instruction::IPUT_OBJECT:
      GenIPut(mir, opt_flags, kReference, rl_src[0], rl_src[1]);
      break;

    case Instruction::IPUT_QUICK:
    case Instruction::IPUT:
      GenIPut(mir, opt_flags, rl_src[0].fp ? kSingle : k32, rl_src[0], rl_src[1]);
      break;

    case Instruction::IPUT_BYTE_QUICK:
    case Instruction::IPUT_BOOLEAN_QUICK:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_BOOLEAN:
      GenIPut(mir, opt_flags, kUnsignedByte, rl_src[0], rl_src[1]);
      break;

    case Instruction::IPUT_CHAR_QUICK:
    case Instruction::IPUT_CHAR:
      GenIPut(mir, opt_flags, kUnsignedHalf, rl_src[0], rl_src[1]);
      break;

    case Instruction::IPUT_SHORT_QUICK:
    case Instruction::IPUT_SHORT:
      GenIPut(mir, opt_flags, kSignedHalf, rl_src[0], rl_src[1]);
      break;

    case Instruction::SGET_OBJECT:
      GenSget(mir, rl_dest, kReference, Primitive::kPrimNot);
      break;

    case Instruction::SGET:
      GenSget(mir, rl_dest, rl_dest.fp ? kSingle : k32, Primitive::kPrimInt);
      break;

    case Instruction::SGET_CHAR:
      GenSget(mir, rl_dest, kUnsignedHalf, Primitive::kPrimChar);
      break;

    case Instruction::SGET_SHORT:
      GenSget(mir, rl_dest, kSignedHalf, Primitive::kPrimShort);
      break;

    case Instruction::SGET_BOOLEAN:
      GenSget(mir, rl_dest, kUnsignedByte, Primitive::kPrimBoolean);
      break;

    case Instruction::SGET_BYTE:
      GenSget(mir, rl_dest, kSignedByte, Primitive::kPrimByte);
      break;

    case Instruction::SGET_WIDE:
      // kPrimLong and kPrimDouble share the same entrypoints.
      GenSget(mir, rl_dest, rl_dest.fp ? kDouble : k64, Primitive::kPrimDouble);
      break;

    case Instruction::SPUT_OBJECT:
      GenSput(mir, rl_src[0], kReference);
      break;

    case Instruction::SPUT:
      GenSput(mir, rl_src[0], rl_src[0].fp ? kSingle : k32);
      break;

    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_BOOLEAN:
      GenSput(mir, rl_src[0], kUnsignedByte);
      break;

    case Instruction::SPUT_CHAR:
      GenSput(mir, rl_src[0], kUnsignedHalf);
      break;

    case Instruction::SPUT_SHORT:
      GenSput(mir, rl_src[0], kSignedHalf);
      break;


    case Instruction::SPUT_WIDE:
      GenSput(mir, rl_src[0], rl_src[0].fp ? kDouble : k64);
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

    case Instruction::INVOKE_VIRTUAL_QUICK:
    case Instruction::INVOKE_VIRTUAL:
      GenInvoke(mir_graph_->NewMemCallInfo(bb, mir, kVirtual, false));
      break;

    case Instruction::INVOKE_VIRTUAL_RANGE_QUICK:
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
      GenArithOpInt(opcode, rl_dest, rl_src[0], rl_src[0], opt_flags);
      break;

    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      GenArithOpLong(opcode, rl_dest, rl_src[0], rl_src[0], opt_flags);
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
      GenLongToInt(rl_dest, rl_src[0]);
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
          InexpensiveConstantInt(mir_graph_->ConstantValue(rl_src[0]), opcode)) {
        GenArithOpIntLit(opcode, rl_dest, rl_src[1],
                             mir_graph_->ConstantValue(rl_src[0].orig_sreg));
      } else if (rl_src[1].is_const &&
                 InexpensiveConstantInt(mir_graph_->ConstantValue(rl_src[1]), opcode)) {
        GenArithOpIntLit(opcode, rl_dest, rl_src[0],
                             mir_graph_->ConstantValue(rl_src[1].orig_sreg));
      } else {
        GenArithOpInt(opcode, rl_dest, rl_src[0], rl_src[1], opt_flags);
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
          InexpensiveConstantInt(mir_graph_->ConstantValue(rl_src[1]), opcode)) {
        GenArithOpIntLit(opcode, rl_dest, rl_src[0], mir_graph_->ConstantValue(rl_src[1]));
      } else {
        GenArithOpInt(opcode, rl_dest, rl_src[0], rl_src[1], opt_flags);
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
        GenArithImmOpLong(opcode, rl_dest, rl_src[0], rl_src[1], opt_flags);
        break;
      }
      FALLTHROUGH_INTENDED;
    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
      GenArithOpLong(opcode, rl_dest, rl_src[0], rl_src[1], opt_flags);
      break;

    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
      if (rl_src[1].is_const) {
        GenShiftImmOpLong(opcode, rl_dest, rl_src[0], rl_src[1], opt_flags);
      } else {
        GenShiftOpLong(opcode, rl_dest, rl_src[0], rl_src[1]);
      }
      break;

    case Instruction::DIV_FLOAT:
    case Instruction::DIV_FLOAT_2ADDR:
      if (HandleEasyFloatingPointDiv(rl_dest, rl_src[0], rl_src[1])) {
        break;
      }
      FALLTHROUGH_INTENDED;
    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::REM_FLOAT:
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
      GenArithOpFloat(opcode, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::DIV_DOUBLE:
    case Instruction::DIV_DOUBLE_2ADDR:
      if (HandleEasyFloatingPointDiv(rl_dest, rl_src[0], rl_src[1])) {
        break;
      }
      FALLTHROUGH_INTENDED;
    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::REM_DOUBLE:
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
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
  DCHECK(CheckCorePoolSanity());
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
      if (mir_graph_->IsBackEdge(bb, bb->taken) || mir_graph_->IsBackEdge(bb, bb->fall_through)) {
        GenSuspendTest(mir->optimization_flags);
      }
      GenFusedFPCmpBranch(bb, mir, false /*gt bias*/, false /*double*/);
      break;
    case kMirOpFusedCmpgFloat:
      if (mir_graph_->IsBackEdge(bb, bb->taken) || mir_graph_->IsBackEdge(bb, bb->fall_through)) {
        GenSuspendTest(mir->optimization_flags);
      }
      GenFusedFPCmpBranch(bb, mir, true /*gt bias*/, false /*double*/);
      break;
    case kMirOpFusedCmplDouble:
      if (mir_graph_->IsBackEdge(bb, bb->taken) || mir_graph_->IsBackEdge(bb, bb->fall_through)) {
        GenSuspendTest(mir->optimization_flags);
      }
      GenFusedFPCmpBranch(bb, mir, false /*gt bias*/, true /*double*/);
      break;
    case kMirOpFusedCmpgDouble:
      if (mir_graph_->IsBackEdge(bb, bb->taken) || mir_graph_->IsBackEdge(bb, bb->fall_through)) {
        GenSuspendTest(mir->optimization_flags);
      }
      GenFusedFPCmpBranch(bb, mir, true /*gt bias*/, true /*double*/);
      break;
    case kMirOpFusedCmpLong:
      if (mir_graph_->IsBackEdge(bb, bb->taken) || mir_graph_->IsBackEdge(bb, bb->fall_through)) {
        GenSuspendTest(mir->optimization_flags);
      }
      GenFusedLongCmpBranch(bb, mir);
      break;
    case kMirOpSelect:
      GenSelect(bb, mir);
      break;
    case kMirOpNullCheck: {
      RegLocation rl_obj = mir_graph_->GetSrc(mir, 0);
      rl_obj = LoadValue(rl_obj, kRefReg);
      // An explicit check is done because it is not expected that when this is used,
      // that it will actually trip up the implicit checks (since an invalid access
      // is needed on the null object).
      GenExplicitNullCheck(rl_obj.reg, mir->optimization_flags);
      break;
    }
    case kMirOpPhi:
    case kMirOpNop:
    case kMirOpRangeCheck:
    case kMirOpDivZeroCheck:
    case kMirOpCheck:
      // Ignore these known opcodes
      break;
    default:
      // Give the backends a chance to handle unknown extended MIR opcodes.
      GenMachineSpecificExtendedMethodMIR(bb, mir);
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

  LIR* head_lir = nullptr;

  // If this is a catch block, export the start address.
  if (bb->catch_entry) {
    head_lir = NewLIR0(kPseudoExportedPC);
  }

  // Free temp registers and reset redundant store tracking.
  ClobberAllTemps();

  if (bb->block_type == kEntryBlock) {
    ResetRegPool();
    int start_vreg = mir_graph_->GetFirstInVR();
    AppendLIR(NewLIR0(kPseudoPrologueBegin));
    DCHECK_EQ(cu_->target64, Is64BitInstructionSet(cu_->instruction_set));
    if (cu_->target64) {
      DCHECK(mir_graph_->GetMethodLoc().wide);
    }
    GenEntrySequence(&mir_graph_->reg_location_[start_vreg], mir_graph_->GetMethodLoc());
    AppendLIR(NewLIR0(kPseudoPrologueEnd));
    DCHECK_EQ(cfi_.GetCurrentCFAOffset(), frame_size_);
  } else if (bb->block_type == kExitBlock) {
    ResetRegPool();
    DCHECK_EQ(cfi_.GetCurrentCFAOffset(), frame_size_);
    AppendLIR(NewLIR0(kPseudoEpilogueBegin));
    GenExitSequence();
    AppendLIR(NewLIR0(kPseudoEpilogueEnd));
    DCHECK_EQ(cfi_.GetCurrentCFAOffset(), frame_size_);
  }

  for (mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
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
    current_mir_ = mir;
    int opcode = mir->dalvikInsn.opcode;

    GenPrintLabel(mir);

    // Remember the first LIR for this block.
    if (head_lir == nullptr) {
      head_lir = &block_label_list_[bb->id];
      // Set the first label as a scheduling barrier.
      DCHECK(!head_lir->flags.use_def_invalid);
      head_lir->u.m.def_mask = &kEncodeAll;
    }

    if (MIR::DecodedInstruction::IsPseudoMirOp(opcode)) {
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
  DCHECK_EQ(mir_graph_->GetNumReachableBlocks(), mir_graph_->GetDfsOrder().size());
  BasicBlock*bb = nullptr;
  for (BasicBlockId dfs_id : mir_graph_->GetDfsOrder()) {
    BasicBlock* candidate = mir_graph_->GetBasicBlock(dfs_id);
    if (candidate->block_type == kDalvikByteCode) {
      bb = candidate;
      break;
    }
  }
  if (bb == nullptr) {
    return false;
  }
  DCHECK_EQ(bb->start_offset, 0);
  DCHECK(bb->first_mir_insn != nullptr);

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
  block_label_list_ = arena_->AllocArray<LIR>(mir_graph_->GetNumBlocks(), kArenaAllocLIR);

  PreOrderDfsIterator iter(mir_graph_);
  BasicBlock* curr_bb = iter.Next();
  BasicBlock* next_bb = iter.Next();
  while (curr_bb != nullptr) {
    MethodBlockCodeGen(curr_bb);
    // If the fall_through block is no longer laid out consecutively, drop in a branch.
    BasicBlock* curr_bb_fall_through = mir_graph_->GetBasicBlock(curr_bb->fall_through);
    if ((curr_bb_fall_through != nullptr) && (curr_bb_fall_through != next_bb)) {
      OpUnconditionalBranch(&block_label_list_[curr_bb->fall_through]);
    }
    curr_bb = next_bb;
    do {
      next_bb = iter.Next();
    } while ((next_bb != nullptr) && (next_bb->block_type == kDead));
  }
  HandleSlowPaths();
}

//
// LIR Slow Path
//

LIR* Mir2Lir::LIRSlowPath::GenerateTargetLabel(int opcode) {
  m2l_->SetCurrentDexPc(current_dex_pc_);
  m2l_->current_mir_ = current_mir_;
  LIR* target = m2l_->NewLIR0(opcode);
  fromfast_->target = target;
  return target;
}


void Mir2Lir::CheckRegStorageImpl(RegStorage rs, WidenessCheck wide, RefCheck ref, FPCheck fp,
                                  bool fail, bool report)
    const  {
  if (rs.Valid()) {
    if (ref == RefCheck::kCheckRef) {
      if (cu_->target64 && !rs.Is64Bit()) {
        if (fail) {
          CHECK(false) << "Reg storage not 64b for ref.";
        } else if (report) {
          LOG(WARNING) << "Reg storage not 64b for ref.";
        }
      }
    }
    if (wide == WidenessCheck::kCheckWide) {
      if (!rs.Is64Bit()) {
        if (fail) {
          CHECK(false) << "Reg storage not 64b for wide.";
        } else if (report) {
          LOG(WARNING) << "Reg storage not 64b for wide.";
        }
      }
    }
    // A tighter check would be nice, but for now soft-float will not check float at all.
    if (fp == FPCheck::kCheckFP && cu_->instruction_set != kArm) {
      if (!rs.IsFloat()) {
        if (fail) {
          CHECK(false) << "Reg storage not float for fp.";
        } else if (report) {
          LOG(WARNING) << "Reg storage not float for fp.";
        }
      }
    } else if (fp == FPCheck::kCheckNotFP) {
      if (rs.IsFloat()) {
        if (fail) {
          CHECK(false) << "Reg storage float for not-fp.";
        } else if (report) {
          LOG(WARNING) << "Reg storage float for not-fp.";
        }
      }
    }
  }
}

void Mir2Lir::CheckRegLocationImpl(RegLocation rl, bool fail, bool report) const {
  // Regrettably can't use the fp part of rl, as that is not really indicative of where a value
  // will be stored.
  CheckRegStorageImpl(rl.reg, rl.wide ? WidenessCheck::kCheckWide : WidenessCheck::kCheckNotWide,
      rl.ref ? RefCheck::kCheckRef : RefCheck::kCheckNotRef, FPCheck::kIgnoreFP, fail, report);
}

size_t Mir2Lir::GetInstructionOffset(LIR* lir) {
  UNUSED(lir);
  UNIMPLEMENTED(FATAL) << "Unsupported GetInstructionOffset()";
  UNREACHABLE();
}

void Mir2Lir::InToRegStorageMapping::Initialize(ShortyIterator* shorty,
                                                InToRegStorageMapper* mapper) {
  DCHECK(mapper != nullptr);
  DCHECK(shorty != nullptr);
  DCHECK(!IsInitialized());
  DCHECK_EQ(end_mapped_in_, 0u);
  DCHECK(!has_arguments_on_stack_);
  while (shorty->Next()) {
     ShortyArg arg = shorty->GetArg();
     RegStorage reg = mapper->GetNextReg(arg);
     mapping_.emplace_back(arg, reg);
     if (arg.IsWide()) {
       mapping_.emplace_back(ShortyArg(kInvalidShorty), RegStorage::InvalidReg());
     }
     if (reg.Valid()) {
       end_mapped_in_ = mapping_.size();
       // If the VR is wide but wasn't mapped as wide then account for it.
       if (arg.IsWide() && !reg.Is64Bit()) {
         --end_mapped_in_;
       }
     } else {
       has_arguments_on_stack_ = true;
     }
  }
  initialized_ = true;
}

RegStorage Mir2Lir::InToRegStorageMapping::GetReg(size_t in_position) {
  DCHECK(IsInitialized());
  DCHECK_LT(in_position, mapping_.size());
  DCHECK_NE(mapping_[in_position].first.GetType(), kInvalidShorty);
  return mapping_[in_position].second;
}

Mir2Lir::ShortyArg Mir2Lir::InToRegStorageMapping::GetShorty(size_t in_position) {
  DCHECK(IsInitialized());
  DCHECK_LT(static_cast<size_t>(in_position), mapping_.size());
  DCHECK_NE(mapping_[in_position].first.GetType(), kInvalidShorty);
  return mapping_[in_position].first;
}

}  // namespace art
