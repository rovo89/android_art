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

#include "base/bit_vector-inl.h"
#include "base/logging.h"
#include "base/scoped_arena_containers.h"
#include "dataflow_iterator-inl.h"
#include "dex/verified_method.h"
#include "dex_flags.h"
#include "driver/compiler_driver.h"
#include "driver/dex_compilation_unit.h"
#include "global_value_numbering.h"
#include "gvn_dead_code_elimination.h"
#include "local_value_numbering.h"
#include "mir_field_info.h"
#include "mirror/string.h"
#include "quick/dex_file_method_inliner.h"
#include "quick/dex_file_to_method_inliner_map.h"
#include "stack.h"
#include "type_inference.h"
#include "utils.h"

namespace art {

static unsigned int Predecessors(BasicBlock* bb) {
  return bb->predecessors.size();
}

/* Setup a constant value for opcodes thare have the DF_SETS_CONST attribute */
void MIRGraph::SetConstant(int32_t ssa_reg, int32_t value) {
  is_constant_v_->SetBit(ssa_reg);
  constant_values_[ssa_reg] = value;
  reg_location_[ssa_reg].is_const = true;
}

void MIRGraph::SetConstantWide(int32_t ssa_reg, int64_t value) {
  is_constant_v_->SetBit(ssa_reg);
  is_constant_v_->SetBit(ssa_reg + 1);
  constant_values_[ssa_reg] = Low32Bits(value);
  constant_values_[ssa_reg + 1] = High32Bits(value);
  reg_location_[ssa_reg].is_const = true;
  reg_location_[ssa_reg + 1].is_const = true;
}

void MIRGraph::DoConstantPropagation(BasicBlock* bb) {
  MIR* mir;

  for (mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
    // Skip pass if BB has MIR without SSA representation.
    if (mir->ssa_rep == nullptr) {
       return;
    }

    uint64_t df_attributes = GetDataFlowAttributes(mir);

    MIR::DecodedInstruction* d_insn = &mir->dalvikInsn;

    if (!(df_attributes & DF_HAS_DEFS)) continue;

    /* Handle instructions that set up constants directly */
    if (df_attributes & DF_SETS_CONST) {
      if (df_attributes & DF_DA) {
        int32_t vB = static_cast<int32_t>(d_insn->vB);
        switch (d_insn->opcode) {
          case Instruction::CONST_4:
          case Instruction::CONST_16:
          case Instruction::CONST:
            SetConstant(mir->ssa_rep->defs[0], vB);
            break;
          case Instruction::CONST_HIGH16:
            SetConstant(mir->ssa_rep->defs[0], vB << 16);
            break;
          case Instruction::CONST_WIDE_16:
          case Instruction::CONST_WIDE_32:
            SetConstantWide(mir->ssa_rep->defs[0], static_cast<int64_t>(vB));
            break;
          case Instruction::CONST_WIDE:
            SetConstantWide(mir->ssa_rep->defs[0], d_insn->vB_wide);
            break;
          case Instruction::CONST_WIDE_HIGH16:
            SetConstantWide(mir->ssa_rep->defs[0], static_cast<int64_t>(vB) << 48);
            break;
          default:
            break;
        }
      }
      /* Handle instructions that set up constants directly */
    } else if (df_attributes & DF_IS_MOVE) {
      int i;

      for (i = 0; i < mir->ssa_rep->num_uses; i++) {
        if (!is_constant_v_->IsBitSet(mir->ssa_rep->uses[i])) break;
      }
      /* Move a register holding a constant to another register */
      if (i == mir->ssa_rep->num_uses) {
        SetConstant(mir->ssa_rep->defs[0], constant_values_[mir->ssa_rep->uses[0]]);
        if (df_attributes & DF_A_WIDE) {
          SetConstant(mir->ssa_rep->defs[1], constant_values_[mir->ssa_rep->uses[1]]);
        }
      }
    }
  }
  /* TODO: implement code to handle arithmetic operations */
}

/* Advance to next strictly dominated MIR node in an extended basic block */
MIR* MIRGraph::AdvanceMIR(BasicBlock** p_bb, MIR* mir) {
  BasicBlock* bb = *p_bb;
  if (mir != nullptr) {
    mir = mir->next;
    while (mir == nullptr) {
      bb = GetBasicBlock(bb->fall_through);
      if ((bb == nullptr) || Predecessors(bb) != 1) {
        // mir is null and we cannot proceed further.
        break;
      } else {
        *p_bb = bb;
        mir = bb->first_mir_insn;
      }
    }
  }
  return mir;
}

/*
 * To be used at an invoke mir.  If the logically next mir node represents
 * a move-result, return it.  Else, return nullptr.  If a move-result exists,
 * it is required to immediately follow the invoke with no intervening
 * opcodes or incoming arcs.  However, if the result of the invoke is not
 * used, a move-result may not be present.
 */
MIR* MIRGraph::FindMoveResult(BasicBlock* bb, MIR* mir) {
  BasicBlock* tbb = bb;
  mir = AdvanceMIR(&tbb, mir);
  while (mir != nullptr) {
    if ((mir->dalvikInsn.opcode == Instruction::MOVE_RESULT) ||
        (mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) ||
        (mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_WIDE)) {
      break;
    }
    // Keep going if pseudo op, otherwise terminate
    if (MIR::DecodedInstruction::IsPseudoMirOp(mir->dalvikInsn.opcode)) {
      mir = AdvanceMIR(&tbb, mir);
    } else {
      mir = nullptr;
    }
  }
  return mir;
}

BasicBlock* MIRGraph::NextDominatedBlock(BasicBlock* bb) {
  if (bb->block_type == kDead) {
    return nullptr;
  }
  DCHECK((bb->block_type == kEntryBlock) || (bb->block_type == kDalvikByteCode)
      || (bb->block_type == kExitBlock));
  BasicBlock* bb_taken = GetBasicBlock(bb->taken);
  BasicBlock* bb_fall_through = GetBasicBlock(bb->fall_through);
  if (((bb_fall_through == nullptr) && (bb_taken != nullptr)) &&
      ((bb_taken->block_type == kDalvikByteCode) || (bb_taken->block_type == kExitBlock))) {
    // Follow simple unconditional branches.
    bb = bb_taken;
  } else {
    // Follow simple fallthrough
    bb = (bb_taken != nullptr) ? nullptr : bb_fall_through;
  }
  if (bb == nullptr || (Predecessors(bb) != 1)) {
    return nullptr;
  }
  DCHECK((bb->block_type == kDalvikByteCode) || (bb->block_type == kExitBlock));
  return bb;
}

static MIR* FindPhi(BasicBlock* bb, int ssa_name) {
  for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
    if (static_cast<int>(mir->dalvikInsn.opcode) == kMirOpPhi) {
      for (int i = 0; i < mir->ssa_rep->num_uses; i++) {
        if (mir->ssa_rep->uses[i] == ssa_name) {
          return mir;
        }
      }
    }
  }
  return nullptr;
}

static SelectInstructionKind SelectKind(MIR* mir) {
  // Work with the case when mir is null.
  if (mir == nullptr) {
    return kSelectNone;
  }
  switch (mir->dalvikInsn.opcode) {
    case Instruction::MOVE:
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_16:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_OBJECT_FROM16:
      return kSelectMove;
    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16:
      return kSelectConst;
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      return kSelectGoto;
    default:
      return kSelectNone;
  }
}

static constexpr ConditionCode kIfCcZConditionCodes[] = {
    kCondEq, kCondNe, kCondLt, kCondGe, kCondGt, kCondLe
};

static_assert(arraysize(kIfCcZConditionCodes) == Instruction::IF_LEZ - Instruction::IF_EQZ + 1,
              "if_ccz_ccodes_size1");

static constexpr ConditionCode ConditionCodeForIfCcZ(Instruction::Code opcode) {
  return kIfCcZConditionCodes[opcode - Instruction::IF_EQZ];
}

static_assert(ConditionCodeForIfCcZ(Instruction::IF_EQZ) == kCondEq, "if_eqz ccode");
static_assert(ConditionCodeForIfCcZ(Instruction::IF_NEZ) == kCondNe, "if_nez ccode");
static_assert(ConditionCodeForIfCcZ(Instruction::IF_LTZ) == kCondLt, "if_ltz ccode");
static_assert(ConditionCodeForIfCcZ(Instruction::IF_GEZ) == kCondGe, "if_gez ccode");
static_assert(ConditionCodeForIfCcZ(Instruction::IF_GTZ) == kCondGt, "if_gtz ccode");
static_assert(ConditionCodeForIfCcZ(Instruction::IF_LEZ) == kCondLe, "if_lez ccode");

int MIRGraph::GetSSAUseCount(int s_reg) {
  DCHECK_LT(static_cast<size_t>(s_reg), ssa_subscripts_.size());
  return raw_use_counts_[s_reg];
}

size_t MIRGraph::GetNumBytesForSpecialTemps() const {
  // This logic is written with assumption that Method* is only special temp.
  DCHECK_EQ(max_available_special_compiler_temps_, 1u);
  return InstructionSetPointerSize(cu_->instruction_set);
}

size_t MIRGraph::GetNumAvailableVRTemps() {
  // First take into account all temps reserved for backend.
  if (max_available_non_special_compiler_temps_ < reserved_temps_for_backend_) {
    return 0;
  }

  // Calculate remaining ME temps available.
  size_t remaining_me_temps = max_available_non_special_compiler_temps_ -
      reserved_temps_for_backend_;

  if (num_non_special_compiler_temps_ >= remaining_me_temps) {
    return 0;
  } else {
    return remaining_me_temps - num_non_special_compiler_temps_;
  }
}

// FIXME - will probably need to revisit all uses of this, as type not defined.
static const RegLocation temp_loc = {kLocCompilerTemp,
                                     0, 1 /*defined*/, 0, 0, 0, 0, 0, 1 /*home*/,
                                     RegStorage(), INVALID_SREG, INVALID_SREG};

CompilerTemp* MIRGraph::GetNewCompilerTemp(CompilerTempType ct_type, bool wide) {
  // Once the compiler temps have been committed, new ones cannot be requested anymore.
  DCHECK_EQ(compiler_temps_committed_, false);
  // Make sure that reserved for BE set is sane.
  DCHECK_LE(reserved_temps_for_backend_, max_available_non_special_compiler_temps_);

  bool verbose = cu_->verbose;
  const char* ct_type_str = nullptr;

  if (verbose) {
    switch (ct_type) {
      case kCompilerTempBackend:
        ct_type_str = "backend";
        break;
      case kCompilerTempSpecialMethodPtr:
        ct_type_str = "method*";
        break;
      case kCompilerTempVR:
        ct_type_str = "VR";
        break;
      default:
        ct_type_str = "unknown";
        break;
    }
    LOG(INFO) << "CompilerTemps: A compiler temp of type " << ct_type_str << " that is "
        << (wide ? "wide is being requested." : "not wide is being requested.");
  }

  CompilerTemp *compiler_temp = static_cast<CompilerTemp *>(arena_->Alloc(sizeof(CompilerTemp),
                                                            kArenaAllocRegAlloc));

  // Create the type of temp requested. Special temps need special handling because
  // they have a specific virtual register assignment.
  if (ct_type == kCompilerTempSpecialMethodPtr) {
    // This has a special location on stack which is 32-bit or 64-bit depending
    // on mode. However, we don't want to overlap with non-special section
    // and thus even for 64-bit, we allow only a non-wide temp to be requested.
    DCHECK_EQ(wide, false);

    // The vreg is always the first special temp for method ptr.
    compiler_temp->v_reg = GetFirstSpecialTempVR();

    CHECK(reg_location_ == nullptr);
  } else if (ct_type == kCompilerTempBackend) {
    requested_backend_temp_ = true;

    // Make sure that we are not exceeding temps reserved for BE.
    // Since VR temps cannot be requested once the BE temps are requested, we
    // allow reservation of VR temps as well for BE. We
    size_t available_temps = reserved_temps_for_backend_ + GetNumAvailableVRTemps();
    size_t needed_temps = wide ? 2u : 1u;
    if (available_temps < needed_temps) {
      if (verbose) {
        LOG(INFO) << "CompilerTemps: Not enough temp(s) of type " << ct_type_str
            << " are available.";
      }
      return nullptr;
    }

    // Update the remaining reserved temps since we have now used them.
    // Note that the code below is actually subtracting to remove them from reserve
    // once they have been claimed. It is careful to not go below zero.
    reserved_temps_for_backend_ =
        std::max(reserved_temps_for_backend_, needed_temps) - needed_temps;

    // The new non-special compiler temp must receive a unique v_reg.
    compiler_temp->v_reg = GetFirstNonSpecialTempVR() + num_non_special_compiler_temps_;
    num_non_special_compiler_temps_++;
  } else if (ct_type == kCompilerTempVR) {
    // Once we start giving out BE temps, we don't allow anymore ME temps to be requested.
    // This is done in order to prevent problems with ssa since these structures are allocated
    // and managed by the ME.
    DCHECK_EQ(requested_backend_temp_, false);

    // There is a limit to the number of non-special temps so check to make sure it wasn't exceeded.
    size_t available_temps = GetNumAvailableVRTemps();
    if (available_temps <= 0 || (available_temps <= 1 && wide)) {
      if (verbose) {
        LOG(INFO) << "CompilerTemps: Not enough temp(s) of type " << ct_type_str
            << " are available.";
      }
      return nullptr;
    }

    // The new non-special compiler temp must receive a unique v_reg.
    compiler_temp->v_reg = GetFirstNonSpecialTempVR() + num_non_special_compiler_temps_;
    num_non_special_compiler_temps_++;
  } else {
    UNIMPLEMENTED(FATAL) << "No handling for compiler temp type " << ct_type_str << ".";
  }

  // We allocate an sreg as well to make developer life easier.
  // However, if this is requested from an ME pass that will recalculate ssa afterwards,
  // this sreg is no longer valid. The caller should be aware of this.
  compiler_temp->s_reg_low = AddNewSReg(compiler_temp->v_reg);

  if (verbose) {
    LOG(INFO) << "CompilerTemps: New temp of type " << ct_type_str << " with v"
        << compiler_temp->v_reg << " and s" << compiler_temp->s_reg_low << " has been created.";
  }

  if (wide) {
    // Only non-special temps are handled as wide for now.
    // Note that the number of non special temps is incremented below.
    DCHECK(ct_type == kCompilerTempBackend || ct_type == kCompilerTempVR);

    // Ensure that the two registers are consecutive.
    int ssa_reg_low = compiler_temp->s_reg_low;
    int ssa_reg_high = AddNewSReg(compiler_temp->v_reg + 1);
    num_non_special_compiler_temps_++;

    if (verbose) {
      LOG(INFO) << "CompilerTemps: The wide part of temp of type " << ct_type_str << " is v"
          << compiler_temp->v_reg + 1 << " and s" << ssa_reg_high << ".";
    }

    if (reg_location_ != nullptr) {
      reg_location_[ssa_reg_high] = temp_loc;
      reg_location_[ssa_reg_high].high_word = true;
      reg_location_[ssa_reg_high].s_reg_low = ssa_reg_low;
      reg_location_[ssa_reg_high].wide = true;
    }
  }

  // If the register locations have already been allocated, add the information
  // about the temp. We will not overflow because they have been initialized
  // to support the maximum number of temps. For ME temps that have multiple
  // ssa versions, the structures below will be expanded on the post pass cleanup.
  if (reg_location_ != nullptr) {
    int ssa_reg_low = compiler_temp->s_reg_low;
    reg_location_[ssa_reg_low] = temp_loc;
    reg_location_[ssa_reg_low].s_reg_low = ssa_reg_low;
    reg_location_[ssa_reg_low].wide = wide;
  }

  return compiler_temp;
}

void MIRGraph::RemoveLastCompilerTemp(CompilerTempType ct_type, bool wide, CompilerTemp* temp) {
  // Once the compiler temps have been committed, it's too late for any modifications.
  DCHECK_EQ(compiler_temps_committed_, false);

  size_t used_temps = wide ? 2u : 1u;

  if (ct_type == kCompilerTempBackend) {
    DCHECK(requested_backend_temp_);

    // Make the temps available to backend again.
    reserved_temps_for_backend_ += used_temps;
  } else if (ct_type == kCompilerTempVR) {
    DCHECK(!requested_backend_temp_);
  } else {
    UNIMPLEMENTED(FATAL) << "No handling for compiler temp type " << static_cast<int>(ct_type);
  }

  // Reduce the number of non-special compiler temps.
  DCHECK_LE(used_temps, num_non_special_compiler_temps_);
  num_non_special_compiler_temps_ -= used_temps;

  // Check that this was really the last temp.
  DCHECK_EQ(static_cast<size_t>(temp->v_reg),
            GetFirstNonSpecialTempVR() + num_non_special_compiler_temps_);

  if (cu_->verbose) {
    LOG(INFO) << "Last temporary has been removed.";
  }
}

static bool EvaluateBranch(Instruction::Code opcode, int32_t src1, int32_t src2) {
  bool is_taken;
  switch (opcode) {
    case Instruction::IF_EQ: is_taken = (src1 == src2); break;
    case Instruction::IF_NE: is_taken = (src1 != src2); break;
    case Instruction::IF_LT: is_taken = (src1 < src2); break;
    case Instruction::IF_GE: is_taken = (src1 >= src2); break;
    case Instruction::IF_GT: is_taken = (src1 > src2); break;
    case Instruction::IF_LE: is_taken = (src1 <= src2); break;
    case Instruction::IF_EQZ: is_taken = (src1 == 0); break;
    case Instruction::IF_NEZ: is_taken = (src1 != 0); break;
    case Instruction::IF_LTZ: is_taken = (src1 < 0); break;
    case Instruction::IF_GEZ: is_taken = (src1 >= 0); break;
    case Instruction::IF_GTZ: is_taken = (src1 > 0); break;
    case Instruction::IF_LEZ: is_taken = (src1 <= 0); break;
    default:
      LOG(FATAL) << "Unexpected opcode " << opcode;
      UNREACHABLE();
  }
  return is_taken;
}

/* Do some MIR-level extended basic block optimizations */
bool MIRGraph::BasicBlockOpt(BasicBlock* bb) {
  if (bb->block_type == kDead) {
    return true;
  }
  // Currently multiply-accumulate backend supports are only available on arm32 and arm64.
  if (cu_->instruction_set == kArm64 || cu_->instruction_set == kThumb2) {
    MultiplyAddOpt(bb);
  }
  bool use_lvn = bb->use_lvn && (cu_->disable_opt & (1u << kLocalValueNumbering)) == 0u;
  std::unique_ptr<ScopedArenaAllocator> allocator;
  std::unique_ptr<GlobalValueNumbering> global_valnum;
  std::unique_ptr<LocalValueNumbering> local_valnum;
  if (use_lvn) {
    allocator.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
    global_valnum.reset(new (allocator.get()) GlobalValueNumbering(cu_, allocator.get(),
                                                                   GlobalValueNumbering::kModeLvn));
    local_valnum.reset(new (allocator.get()) LocalValueNumbering(global_valnum.get(), bb->id,
                                                                 allocator.get()));
  }
  while (bb != nullptr) {
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      // TUNING: use the returned value number for CSE.
      if (use_lvn) {
        local_valnum->GetValueNumber(mir);
      }
      // Look for interesting opcodes, skip otherwise
      Instruction::Code opcode = mir->dalvikInsn.opcode;
      switch (opcode) {
        case Instruction::IF_EQ:
        case Instruction::IF_NE:
        case Instruction::IF_LT:
        case Instruction::IF_GE:
        case Instruction::IF_GT:
        case Instruction::IF_LE:
          if (!IsConst(mir->ssa_rep->uses[1])) {
            break;
          }
          FALLTHROUGH_INTENDED;
        case Instruction::IF_EQZ:
        case Instruction::IF_NEZ:
        case Instruction::IF_LTZ:
        case Instruction::IF_GEZ:
        case Instruction::IF_GTZ:
        case Instruction::IF_LEZ:
          // Result known at compile time?
          if (IsConst(mir->ssa_rep->uses[0])) {
            int32_t rhs = (mir->ssa_rep->num_uses == 2) ? ConstantValue(mir->ssa_rep->uses[1]) : 0;
            bool is_taken = EvaluateBranch(opcode, ConstantValue(mir->ssa_rep->uses[0]), rhs);
            BasicBlockId edge_to_kill = is_taken ? bb->fall_through : bb->taken;
            if (is_taken) {
              // Replace with GOTO.
              bb->fall_through = NullBasicBlockId;
              mir->dalvikInsn.opcode = Instruction::GOTO;
              mir->dalvikInsn.vA =
                  IsInstructionIfCc(opcode) ? mir->dalvikInsn.vC : mir->dalvikInsn.vB;
            } else {
              // Make NOP.
              bb->taken = NullBasicBlockId;
              mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
            }
            mir->ssa_rep->num_uses = 0;
            BasicBlock* successor_to_unlink = GetBasicBlock(edge_to_kill);
            successor_to_unlink->ErasePredecessor(bb->id);
            // We have changed the graph structure.
            dfs_orders_up_to_date_ = false;
            domination_up_to_date_ = false;
            topological_order_up_to_date_ = false;
            // Keep MIR SSA rep, the worst that can happen is a Phi with just 1 input.
          }
          break;
        case Instruction::CMPL_FLOAT:
        case Instruction::CMPL_DOUBLE:
        case Instruction::CMPG_FLOAT:
        case Instruction::CMPG_DOUBLE:
        case Instruction::CMP_LONG:
          if ((cu_->disable_opt & (1 << kBranchFusing)) != 0) {
            // Bitcode doesn't allow this optimization.
            break;
          }
          if (mir->next != nullptr) {
            MIR* mir_next = mir->next;
            // Make sure result of cmp is used by next insn and nowhere else
            if (IsInstructionIfCcZ(mir_next->dalvikInsn.opcode) &&
                (mir->ssa_rep->defs[0] == mir_next->ssa_rep->uses[0]) &&
                (GetSSAUseCount(mir->ssa_rep->defs[0]) == 1)) {
              mir_next->meta.ccode = ConditionCodeForIfCcZ(mir_next->dalvikInsn.opcode);
              switch (opcode) {
                case Instruction::CMPL_FLOAT:
                  mir_next->dalvikInsn.opcode =
                      static_cast<Instruction::Code>(kMirOpFusedCmplFloat);
                  break;
                case Instruction::CMPL_DOUBLE:
                  mir_next->dalvikInsn.opcode =
                      static_cast<Instruction::Code>(kMirOpFusedCmplDouble);
                  break;
                case Instruction::CMPG_FLOAT:
                  mir_next->dalvikInsn.opcode =
                      static_cast<Instruction::Code>(kMirOpFusedCmpgFloat);
                  break;
                case Instruction::CMPG_DOUBLE:
                  mir_next->dalvikInsn.opcode =
                      static_cast<Instruction::Code>(kMirOpFusedCmpgDouble);
                  break;
                case Instruction::CMP_LONG:
                  mir_next->dalvikInsn.opcode =
                      static_cast<Instruction::Code>(kMirOpFusedCmpLong);
                  break;
                default: LOG(ERROR) << "Unexpected opcode: " << opcode;
              }
              mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
              // Clear use count of temp VR.
              use_counts_[mir->ssa_rep->defs[0]] = 0;
              raw_use_counts_[mir->ssa_rep->defs[0]] = 0;
              // Copy the SSA information that is relevant.
              mir_next->ssa_rep->num_uses = mir->ssa_rep->num_uses;
              mir_next->ssa_rep->uses = mir->ssa_rep->uses;
              mir_next->ssa_rep->num_defs = 0;
              mir->ssa_rep->num_uses = 0;
              mir->ssa_rep->num_defs = 0;
              // Copy in the decoded instruction information for potential SSA re-creation.
              mir_next->dalvikInsn.vA = mir->dalvikInsn.vB;
              mir_next->dalvikInsn.vB = mir->dalvikInsn.vC;
            }
          }
          break;
        default:
          break;
      }
      // Is this the select pattern?
      // TODO: flesh out support for Mips.  NOTE: llvm's select op doesn't quite work here.
      // TUNING: expand to support IF_xx compare & branches
      if ((cu_->instruction_set == kArm64 || cu_->instruction_set == kThumb2 ||
           cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) &&
          IsInstructionIfCcZ(mir->dalvikInsn.opcode)) {
        BasicBlock* ft = GetBasicBlock(bb->fall_through);
        DCHECK(ft != nullptr);
        BasicBlock* ft_ft = GetBasicBlock(ft->fall_through);
        BasicBlock* ft_tk = GetBasicBlock(ft->taken);

        BasicBlock* tk = GetBasicBlock(bb->taken);
        DCHECK(tk != nullptr);
        BasicBlock* tk_ft = GetBasicBlock(tk->fall_through);
        BasicBlock* tk_tk = GetBasicBlock(tk->taken);

        /*
         * In the select pattern, the taken edge goes to a block that unconditionally
         * transfers to the rejoin block and the fall_though edge goes to a block that
         * unconditionally falls through to the rejoin block.
         */
        if ((tk_ft == nullptr) && (ft_tk == nullptr) && (tk_tk == ft_ft) &&
            (Predecessors(tk) == 1) && (Predecessors(ft) == 1)) {
          /*
           * Okay - we have the basic diamond shape.
           */

          // TODO: Add logic for LONG.
          // Are the block bodies something we can handle?
          if ((ft->first_mir_insn == ft->last_mir_insn) &&
              (tk->first_mir_insn != tk->last_mir_insn) &&
              (tk->first_mir_insn->next == tk->last_mir_insn) &&
              ((SelectKind(ft->first_mir_insn) == kSelectMove) ||
              (SelectKind(ft->first_mir_insn) == kSelectConst)) &&
              (SelectKind(ft->first_mir_insn) == SelectKind(tk->first_mir_insn)) &&
              (SelectKind(tk->last_mir_insn) == kSelectGoto)) {
            // Almost there.  Are the instructions targeting the same vreg?
            MIR* if_true = tk->first_mir_insn;
            MIR* if_false = ft->first_mir_insn;
            // It's possible that the target of the select isn't used - skip those (rare) cases.
            MIR* phi = FindPhi(tk_tk, if_true->ssa_rep->defs[0]);
            if ((phi != nullptr) && (if_true->dalvikInsn.vA == if_false->dalvikInsn.vA)) {
              /*
               * We'll convert the IF_EQZ/IF_NEZ to a SELECT.  We need to find the
               * Phi node in the merge block and delete it (while using the SSA name
               * of the merge as the target of the SELECT.  Delete both taken and
               * fallthrough blocks, and set fallthrough to merge block.
               * NOTE: not updating other dataflow info (no longer used at this point).
               * If this changes, need to update i_dom, etc. here (and in CombineBlocks).
               */
              mir->meta.ccode = ConditionCodeForIfCcZ(mir->dalvikInsn.opcode);
              mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpSelect);
              bool const_form = (SelectKind(if_true) == kSelectConst);
              if ((SelectKind(if_true) == kSelectMove)) {
                if (IsConst(if_true->ssa_rep->uses[0]) &&
                    IsConst(if_false->ssa_rep->uses[0])) {
                    const_form = true;
                    if_true->dalvikInsn.vB = ConstantValue(if_true->ssa_rep->uses[0]);
                    if_false->dalvikInsn.vB = ConstantValue(if_false->ssa_rep->uses[0]);
                }
              }
              if (const_form) {
                /*
                 * TODO: If both constants are the same value, then instead of generating
                 * a select, we should simply generate a const bytecode. This should be
                 * considered after inlining which can lead to CFG of this form.
                 */
                // "true" set val in vB
                mir->dalvikInsn.vB = if_true->dalvikInsn.vB;
                // "false" set val in vC
                mir->dalvikInsn.vC = if_false->dalvikInsn.vB;
              } else {
                DCHECK_EQ(SelectKind(if_true), kSelectMove);
                DCHECK_EQ(SelectKind(if_false), kSelectMove);
                int32_t* src_ssa = arena_->AllocArray<int32_t>(3, kArenaAllocDFInfo);
                src_ssa[0] = mir->ssa_rep->uses[0];
                src_ssa[1] = if_true->ssa_rep->uses[0];
                src_ssa[2] = if_false->ssa_rep->uses[0];
                mir->ssa_rep->uses = src_ssa;
                mir->ssa_rep->num_uses = 3;
              }
              AllocateSSADefData(mir, 1);
              /*
               * There is usually a Phi node in the join block for our two cases.  If the
               * Phi node only contains our two cases as input, we will use the result
               * SSA name of the Phi node as our select result and delete the Phi.  If
               * the Phi node has more than two operands, we will arbitrarily use the SSA
               * name of the "false" path, delete the SSA name of the "true" path from the
               * Phi node (and fix up the incoming arc list).
               */
              if (phi->ssa_rep->num_uses == 2) {
                mir->ssa_rep->defs[0] = phi->ssa_rep->defs[0];
                // Rather than changing the Phi to kMirOpNop, remove it completely.
                // This avoids leaving other Phis after kMirOpNop (i.e. a non-Phi) insn.
                tk_tk->RemoveMIR(phi);
                int dead_false_def = if_false->ssa_rep->defs[0];
                raw_use_counts_[dead_false_def] = use_counts_[dead_false_def] = 0;
              } else {
                int live_def = if_false->ssa_rep->defs[0];
                mir->ssa_rep->defs[0] = live_def;
              }
              int dead_true_def = if_true->ssa_rep->defs[0];
              raw_use_counts_[dead_true_def] = use_counts_[dead_true_def] = 0;
              // Update ending vreg->sreg map for GC maps generation.
              int def_vreg = SRegToVReg(mir->ssa_rep->defs[0]);
              bb->data_flow_info->vreg_to_ssa_map_exit[def_vreg] = mir->ssa_rep->defs[0];
              // We want to remove ft and tk and link bb directly to ft_ft. First, we need
              // to update all Phi inputs correctly with UpdatePredecessor(ft->id, bb->id)
              // since the live_def above comes from ft->first_mir_insn (if_false).
              DCHECK(if_false == ft->first_mir_insn);
              ft_ft->UpdatePredecessor(ft->id, bb->id);
              // Correct the rest of the links between bb, ft and ft_ft.
              ft->ErasePredecessor(bb->id);
              ft->fall_through = NullBasicBlockId;
              bb->fall_through = ft_ft->id;
              // Now we can kill tk and ft.
              tk->Kill(this);
              ft->Kill(this);
              // NOTE: DFS order, domination info and topological order are still usable
              // despite the newly dead blocks.
            }
          }
        }
      }
    }
    bb = ((cu_->disable_opt & (1 << kSuppressExceptionEdges)) != 0) ? NextDominatedBlock(bb) :
        nullptr;
  }
  if (use_lvn && UNLIKELY(!global_valnum->Good())) {
    LOG(WARNING) << "LVN overflow in " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  }

  return true;
}

/* Collect stats on number of checks removed */
void MIRGraph::CountChecks(class BasicBlock* bb) {
  if (bb->data_flow_info != nullptr) {
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      if (mir->ssa_rep == nullptr) {
        continue;
      }
      uint64_t df_attributes = GetDataFlowAttributes(mir);
      if (df_attributes & DF_HAS_NULL_CHKS) {
        checkstats_->null_checks++;
        if (mir->optimization_flags & MIR_IGNORE_NULL_CHECK) {
          checkstats_->null_checks_eliminated++;
        }
      }
      if (df_attributes & DF_HAS_RANGE_CHKS) {
        checkstats_->range_checks++;
        if (mir->optimization_flags & MIR_IGNORE_RANGE_CHECK) {
          checkstats_->range_checks_eliminated++;
        }
      }
    }
  }
}

/* Try to make common case the fallthrough path. */
bool MIRGraph::LayoutBlocks(BasicBlock* bb) {
  // TODO: For now, just looking for direct throws.  Consider generalizing for profile feedback.
  if (!bb->explicit_throw) {
    return false;
  }

  // If we visited it, we are done.
  if (bb->visited) {
    return false;
  }
  bb->visited = true;

  BasicBlock* walker = bb;
  while (true) {
    // Check termination conditions.
    if ((walker->block_type == kEntryBlock) || (Predecessors(walker) != 1)) {
      break;
    }
    DCHECK(!walker->predecessors.empty());
    BasicBlock* prev = GetBasicBlock(walker->predecessors[0]);

    // If we visited the predecessor, we are done.
    if (prev->visited) {
      return false;
    }
    prev->visited = true;

    if (prev->conditional_branch) {
      if (GetBasicBlock(prev->fall_through) == walker) {
        // Already done - return.
        break;
      }
      DCHECK_EQ(walker, GetBasicBlock(prev->taken));
      // Got one.  Flip it and exit.
      Instruction::Code opcode = prev->last_mir_insn->dalvikInsn.opcode;
      switch (opcode) {
        case Instruction::IF_EQ: opcode = Instruction::IF_NE; break;
        case Instruction::IF_NE: opcode = Instruction::IF_EQ; break;
        case Instruction::IF_LT: opcode = Instruction::IF_GE; break;
        case Instruction::IF_GE: opcode = Instruction::IF_LT; break;
        case Instruction::IF_GT: opcode = Instruction::IF_LE; break;
        case Instruction::IF_LE: opcode = Instruction::IF_GT; break;
        case Instruction::IF_EQZ: opcode = Instruction::IF_NEZ; break;
        case Instruction::IF_NEZ: opcode = Instruction::IF_EQZ; break;
        case Instruction::IF_LTZ: opcode = Instruction::IF_GEZ; break;
        case Instruction::IF_GEZ: opcode = Instruction::IF_LTZ; break;
        case Instruction::IF_GTZ: opcode = Instruction::IF_LEZ; break;
        case Instruction::IF_LEZ: opcode = Instruction::IF_GTZ; break;
        default: LOG(FATAL) << "Unexpected opcode " << opcode;
      }
      prev->last_mir_insn->dalvikInsn.opcode = opcode;
      BasicBlockId t_bb = prev->taken;
      prev->taken = prev->fall_through;
      prev->fall_through = t_bb;
      break;
    }
    walker = prev;
  }
  return false;
}

/* Combine any basic blocks terminated by instructions that we now know can't throw */
void MIRGraph::CombineBlocks(class BasicBlock* bb) {
  // Loop here to allow combining a sequence of blocks
  while ((bb->block_type == kDalvikByteCode) &&
      (bb->last_mir_insn != nullptr) &&
      (static_cast<int>(bb->last_mir_insn->dalvikInsn.opcode) == kMirOpCheck)) {
    MIR* mir = bb->last_mir_insn;
    DCHECK(bb->first_mir_insn !=  nullptr);

    // Get the paired insn and check if it can still throw.
    MIR* throw_insn = mir->meta.throw_insn;
    if (CanThrow(throw_insn)) {
      break;
    }

    // OK - got one.  Combine
    BasicBlock* bb_next = GetBasicBlock(bb->fall_through);
    DCHECK(!bb_next->catch_entry);
    DCHECK_EQ(bb_next->predecessors.size(), 1u);

    // Now move instructions from bb_next to bb. Start off with doing a sanity check
    // that kMirOpCheck's throw instruction is first one in the bb_next.
    DCHECK_EQ(bb_next->first_mir_insn, throw_insn);
    // Now move all instructions (throw instruction to last one) from bb_next to bb.
    MIR* last_to_move = bb_next->last_mir_insn;
    bb_next->RemoveMIRList(throw_insn, last_to_move);
    bb->InsertMIRListAfter(bb->last_mir_insn, throw_insn, last_to_move);
    // The kMirOpCheck instruction is not needed anymore.
    mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
    bb->RemoveMIR(mir);

    // Before we overwrite successors, remove their predecessor links to bb.
    bb_next->ErasePredecessor(bb->id);
    if (bb->taken != NullBasicBlockId) {
      DCHECK_EQ(bb->successor_block_list_type, kNotUsed);
      BasicBlock* bb_taken = GetBasicBlock(bb->taken);
      // bb->taken will be overwritten below.
      DCHECK_EQ(bb_taken->block_type, kExceptionHandling);
      DCHECK_EQ(bb_taken->predecessors.size(), 1u);
      DCHECK_EQ(bb_taken->predecessors[0], bb->id);
      bb_taken->predecessors.clear();
      bb_taken->block_type = kDead;
      DCHECK(bb_taken->data_flow_info == nullptr);
    } else {
      DCHECK_EQ(bb->successor_block_list_type, kCatch);
      for (SuccessorBlockInfo* succ_info : bb->successor_blocks) {
        if (succ_info->block != NullBasicBlockId) {
          BasicBlock* succ_bb = GetBasicBlock(succ_info->block);
          DCHECK(succ_bb->catch_entry);
          succ_bb->ErasePredecessor(bb->id);
        }
      }
    }
    // Use the successor info from the next block
    bb->successor_block_list_type = bb_next->successor_block_list_type;
    bb->successor_blocks.swap(bb_next->successor_blocks);  // Swap instead of copying.
    bb_next->successor_block_list_type = kNotUsed;
    // Use the ending block linkage from the next block
    bb->fall_through = bb_next->fall_through;
    bb_next->fall_through = NullBasicBlockId;
    bb->taken = bb_next->taken;
    bb_next->taken = NullBasicBlockId;
    /*
     * If lower-half of pair of blocks to combine contained
     * a return or a conditional branch or an explicit throw,
     * move the flag to the newly combined block.
     */
    bb->terminated_by_return = bb_next->terminated_by_return;
    bb->conditional_branch = bb_next->conditional_branch;
    bb->explicit_throw = bb_next->explicit_throw;
    // Merge the use_lvn flag.
    bb->use_lvn |= bb_next->use_lvn;

    // Kill the unused block.
    bb_next->data_flow_info = nullptr;

    /*
     * NOTE: we aren't updating all dataflow info here.  Should either make sure this pass
     * happens after uses of i_dominated, dom_frontier or update the dataflow info here.
     * NOTE: GVN uses bb->data_flow_info->live_in_v which is unaffected by the block merge.
     */

    // Kill bb_next and remap now-dead id to parent.
    bb_next->block_type = kDead;
    bb_next->data_flow_info = nullptr;  // Must be null for dead blocks. (Relied on by the GVN.)
    block_id_map_.Overwrite(bb_next->id, bb->id);
    // Update predecessors in children.
    ChildBlockIterator iter(bb, this);
    for (BasicBlock* child = iter.Next(); child != nullptr; child = iter.Next()) {
      child->UpdatePredecessor(bb_next->id, bb->id);
    }

    // DFS orders, domination and topological order are not up to date anymore.
    dfs_orders_up_to_date_ = false;
    domination_up_to_date_ = false;
    topological_order_up_to_date_ = false;

    // Now, loop back and see if we can keep going
  }
}

bool MIRGraph::EliminateNullChecksGate() {
  if ((cu_->disable_opt & (1 << kNullCheckElimination)) != 0 ||
      (merged_df_flags_ & DF_HAS_NULL_CHKS) == 0) {
    return false;
  }

  DCHECK(temp_scoped_alloc_.get() == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
  temp_.nce.num_vregs = GetNumOfCodeAndTempVRs();
  temp_.nce.work_vregs_to_check = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), temp_.nce.num_vregs, false, kBitMapNullCheck);
  temp_.nce.ending_vregs_to_check_matrix =
      temp_scoped_alloc_->AllocArray<ArenaBitVector*>(GetNumBlocks(), kArenaAllocMisc);
  std::fill_n(temp_.nce.ending_vregs_to_check_matrix, GetNumBlocks(), nullptr);

  // reset MIR_MARK
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      mir->optimization_flags &= ~MIR_MARK;
    }
  }

  return true;
}

/*
 * Eliminate unnecessary null checks for a basic block.
 */
bool MIRGraph::EliminateNullChecks(BasicBlock* bb) {
  if (bb->block_type != kDalvikByteCode && bb->block_type != kEntryBlock) {
    // Ignore the kExitBlock as well.
    DCHECK(bb->first_mir_insn == nullptr);
    return false;
  }

  ArenaBitVector* vregs_to_check = temp_.nce.work_vregs_to_check;
  /*
   * Set initial state. Catch blocks don't need any special treatment.
   */
  if (bb->block_type == kEntryBlock) {
    vregs_to_check->ClearAllBits();
    // Assume all ins are objects.
    for (uint16_t in_reg = GetFirstInVR();
         in_reg < GetNumOfCodeVRs(); in_reg++) {
      vregs_to_check->SetBit(in_reg);
    }
    if ((cu_->access_flags & kAccStatic) == 0) {
      // If non-static method, mark "this" as non-null.
      int this_reg = GetFirstInVR();
      vregs_to_check->ClearBit(this_reg);
    }
  } else {
    DCHECK_EQ(bb->block_type, kDalvikByteCode);
    // Starting state is union of all incoming arcs.
    bool copied_first = false;
    for (BasicBlockId pred_id : bb->predecessors) {
      if (temp_.nce.ending_vregs_to_check_matrix[pred_id] == nullptr) {
        continue;
      }
      BasicBlock* pred_bb = GetBasicBlock(pred_id);
      DCHECK(pred_bb != nullptr);
      MIR* null_check_insn = nullptr;
      // Check to see if predecessor had an explicit null-check.
      if (pred_bb->BranchesToSuccessorOnlyIfNotZero(bb->id)) {
        // Remember the null check insn if there's no other predecessor requiring null check.
        if (!copied_first || !vregs_to_check->IsBitSet(pred_bb->last_mir_insn->dalvikInsn.vA)) {
          null_check_insn = pred_bb->last_mir_insn;
          DCHECK(null_check_insn != nullptr);
        }
      }
      if (!copied_first) {
        copied_first = true;
        vregs_to_check->Copy(temp_.nce.ending_vregs_to_check_matrix[pred_id]);
      } else {
        vregs_to_check->Union(temp_.nce.ending_vregs_to_check_matrix[pred_id]);
      }
      if (null_check_insn != nullptr) {
        vregs_to_check->ClearBit(null_check_insn->dalvikInsn.vA);
      }
    }
    DCHECK(copied_first);  // At least one predecessor must have been processed before this bb.
  }
  // At this point, vregs_to_check shows which sregs have an object definition with
  // no intervening uses.

  // Walk through the instruction in the block, updating as necessary
  for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
    uint64_t df_attributes = GetDataFlowAttributes(mir);

    if ((df_attributes & DF_NULL_TRANSFER_N) != 0u) {
      // The algorithm was written in a phi agnostic way.
      continue;
    }

    // Might need a null check?
    if (df_attributes & DF_HAS_NULL_CHKS) {
      int src_vreg;
      if (df_attributes & DF_NULL_CHK_OUT0) {
        DCHECK_NE(df_attributes & DF_IS_INVOKE, 0u);
        src_vreg = mir->dalvikInsn.vC;
      } else if (df_attributes & DF_NULL_CHK_B) {
        DCHECK_NE(df_attributes & DF_REF_B, 0u);
        src_vreg = mir->dalvikInsn.vB;
      } else {
        DCHECK_NE(df_attributes & DF_NULL_CHK_A, 0u);
        DCHECK_NE(df_attributes & DF_REF_A, 0u);
        src_vreg = mir->dalvikInsn.vA;
      }
      if (!vregs_to_check->IsBitSet(src_vreg)) {
        // Eliminate the null check.
        mir->optimization_flags |= MIR_MARK;
      } else {
        // Do the null check.
        mir->optimization_flags &= ~MIR_MARK;
        // Mark src_vreg as null-checked.
        vregs_to_check->ClearBit(src_vreg);
      }
    }

    if ((df_attributes & DF_A_WIDE) ||
        (df_attributes & (DF_REF_A | DF_SETS_CONST | DF_NULL_TRANSFER)) == 0) {
      continue;
    }

    /*
     * First, mark all object definitions as requiring null check.
     * Note: we can't tell if a CONST definition might be used as an object, so treat
     * them all as object definitions.
     */
    if ((df_attributes & (DF_DA | DF_REF_A)) == (DF_DA | DF_REF_A) ||
        (df_attributes & DF_SETS_CONST))  {
      vregs_to_check->SetBit(mir->dalvikInsn.vA);
    }

    // Then, remove mark from all object definitions we know are non-null.
    if (df_attributes & DF_NON_NULL_DST) {
      // Mark target of NEW* as non-null
      DCHECK_NE(df_attributes & DF_REF_A, 0u);
      vregs_to_check->ClearBit(mir->dalvikInsn.vA);
    }

    // Mark non-null returns from invoke-style NEW*
    if (df_attributes & DF_NON_NULL_RET) {
      MIR* next_mir = mir->next;
      // Next should be an MOVE_RESULT_OBJECT
      if (UNLIKELY(next_mir == nullptr)) {
        // The MethodVerifier makes sure there's no MOVE_RESULT at the catch entry or branch
        // target, so the MOVE_RESULT cannot be broken away into another block.
        LOG(WARNING) << "Unexpected end of block following new";
      } else if (UNLIKELY(next_mir->dalvikInsn.opcode != Instruction::MOVE_RESULT_OBJECT)) {
        LOG(WARNING) << "Unexpected opcode following new: " << next_mir->dalvikInsn.opcode;
      } else {
        // Mark as null checked.
        vregs_to_check->ClearBit(next_mir->dalvikInsn.vA);
      }
    }

    // Propagate null check state on register copies.
    if (df_attributes & DF_NULL_TRANSFER_0) {
      DCHECK_EQ(df_attributes | ~(DF_DA | DF_REF_A | DF_UB | DF_REF_B), static_cast<uint64_t>(-1));
      if (vregs_to_check->IsBitSet(mir->dalvikInsn.vB)) {
        vregs_to_check->SetBit(mir->dalvikInsn.vA);
      } else {
        vregs_to_check->ClearBit(mir->dalvikInsn.vA);
      }
    }
  }

  // Did anything change?
  bool nce_changed = false;
  ArenaBitVector* old_ending_ssa_regs_to_check = temp_.nce.ending_vregs_to_check_matrix[bb->id];
  if (old_ending_ssa_regs_to_check == nullptr) {
    DCHECK(temp_scoped_alloc_.get() != nullptr);
    nce_changed = vregs_to_check->GetHighestBitSet() != -1;
    temp_.nce.ending_vregs_to_check_matrix[bb->id] = vregs_to_check;
    // Create a new vregs_to_check for next BB.
    temp_.nce.work_vregs_to_check = new (temp_scoped_alloc_.get()) ArenaBitVector(
        temp_scoped_alloc_.get(), temp_.nce.num_vregs, false, kBitMapNullCheck);
  } else if (!vregs_to_check->SameBitsSet(old_ending_ssa_regs_to_check)) {
    nce_changed = true;
    temp_.nce.ending_vregs_to_check_matrix[bb->id] = vregs_to_check;
    temp_.nce.work_vregs_to_check = old_ending_ssa_regs_to_check;  // Reuse for next BB.
  }
  return nce_changed;
}

void MIRGraph::EliminateNullChecksEnd() {
  // Clean up temporaries.
  temp_.nce.num_vregs = 0u;
  temp_.nce.work_vregs_to_check = nullptr;
  temp_.nce.ending_vregs_to_check_matrix = nullptr;
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  temp_scoped_alloc_.reset();

  // converge MIR_MARK with MIR_IGNORE_NULL_CHECK
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      constexpr int kMarkToIgnoreNullCheckShift = kMIRMark - kMIRIgnoreNullCheck;
      static_assert(kMarkToIgnoreNullCheckShift > 0, "Not a valid right-shift");
      uint16_t mirMarkAdjustedToIgnoreNullCheck =
          (mir->optimization_flags & MIR_MARK) >> kMarkToIgnoreNullCheckShift;
      mir->optimization_flags |= mirMarkAdjustedToIgnoreNullCheck;
    }
  }
}

void MIRGraph::InferTypesStart() {
  DCHECK(temp_scoped_alloc_ != nullptr);
  temp_.ssa.ti = new (temp_scoped_alloc_.get()) TypeInference(this, temp_scoped_alloc_.get());
}

/*
 * Perform type and size inference for a basic block.
 */
bool MIRGraph::InferTypes(BasicBlock* bb) {
  if (bb->data_flow_info == nullptr) return false;

  DCHECK(temp_.ssa.ti != nullptr);
  return temp_.ssa.ti->Apply(bb);
}

void MIRGraph::InferTypesEnd() {
  DCHECK(temp_.ssa.ti != nullptr);
  temp_.ssa.ti->Finish();
  delete temp_.ssa.ti;
  temp_.ssa.ti = nullptr;
}

bool MIRGraph::EliminateClassInitChecksGate() {
  if ((cu_->disable_opt & (1 << kClassInitCheckElimination)) != 0 ||
      (merged_df_flags_ & DF_CLINIT) == 0) {
    return false;
  }

  DCHECK(temp_scoped_alloc_.get() == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));

  // Each insn we use here has at least 2 code units, offset/2 will be a unique index.
  const size_t end = (GetNumDalvikInsns() + 1u) / 2u;
  temp_.cice.indexes = temp_scoped_alloc_->AllocArray<uint16_t>(end, kArenaAllocGrowableArray);
  std::fill_n(temp_.cice.indexes, end, 0xffffu);

  uint32_t unique_class_count = 0u;
  {
    // Get unique_class_count and store indexes in temp_insn_data_ using a map on a nested
    // ScopedArenaAllocator.

    // Embed the map value in the entry to save space.
    struct MapEntry {
      // Map key: the class identified by the declaring dex file and type index.
      const DexFile* declaring_dex_file;
      uint16_t declaring_class_idx;
      // Map value: index into bit vectors of classes requiring initialization checks.
      uint16_t index;
    };
    struct MapEntryComparator {
      bool operator()(const MapEntry& lhs, const MapEntry& rhs) const {
        if (lhs.declaring_class_idx != rhs.declaring_class_idx) {
          return lhs.declaring_class_idx < rhs.declaring_class_idx;
        }
        return lhs.declaring_dex_file < rhs.declaring_dex_file;
      }
    };

    ScopedArenaAllocator allocator(&cu_->arena_stack);
    ScopedArenaSet<MapEntry, MapEntryComparator> class_to_index_map(MapEntryComparator(),
                                                                    allocator.Adapter());

    // First, find all SGET/SPUTs that may need class initialization checks, record INVOKE_STATICs.
    AllNodesIterator iter(this);
    for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
      if (bb->block_type == kDalvikByteCode) {
        for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
          if (IsInstructionSGetOrSPut(mir->dalvikInsn.opcode)) {
            const MirSFieldLoweringInfo& field_info = GetSFieldLoweringInfo(mir);
            if (!field_info.IsReferrersClass()) {
              DCHECK_LT(class_to_index_map.size(), 0xffffu);
              MapEntry entry = {
                  // Treat unresolved fields as if each had its own class.
                  field_info.IsResolved() ? field_info.DeclaringDexFile()
                                          : nullptr,
                  field_info.IsResolved() ? field_info.DeclaringClassIndex()
                                          : field_info.FieldIndex(),
                  static_cast<uint16_t>(class_to_index_map.size())
              };
              uint16_t index = class_to_index_map.insert(entry).first->index;
              // Using offset/2 for index into temp_.cice.indexes.
              temp_.cice.indexes[mir->offset / 2u] = index;
            }
          } else if (IsInstructionInvokeStatic(mir->dalvikInsn.opcode)) {
            const MirMethodLoweringInfo& method_info = GetMethodLoweringInfo(mir);
            DCHECK(method_info.IsStatic());
            if (method_info.FastPath() && !method_info.IsReferrersClass()) {
              MapEntry entry = {
                  method_info.DeclaringDexFile(),
                  method_info.DeclaringClassIndex(),
                  static_cast<uint16_t>(class_to_index_map.size())
              };
              uint16_t index = class_to_index_map.insert(entry).first->index;
              // Using offset/2 for index into temp_.cice.indexes.
              temp_.cice.indexes[mir->offset / 2u] = index;
            }
          }
        }
      }
    }
    unique_class_count = static_cast<uint32_t>(class_to_index_map.size());
  }

  if (unique_class_count == 0u) {
    // All SGET/SPUTs refer to initialized classes. Nothing to do.
    temp_.cice.indexes = nullptr;
    temp_scoped_alloc_.reset();
    return false;
  }

  // 2 bits for each class: is class initialized, is class in dex cache.
  temp_.cice.num_class_bits = 2u * unique_class_count;
  temp_.cice.work_classes_to_check = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), temp_.cice.num_class_bits, false, kBitMapClInitCheck);
  temp_.cice.ending_classes_to_check_matrix =
      temp_scoped_alloc_->AllocArray<ArenaBitVector*>(GetNumBlocks(), kArenaAllocMisc);
  std::fill_n(temp_.cice.ending_classes_to_check_matrix, GetNumBlocks(), nullptr);
  DCHECK_GT(temp_.cice.num_class_bits, 0u);
  return true;
}

/*
 * Eliminate unnecessary class initialization checks for a basic block.
 */
bool MIRGraph::EliminateClassInitChecks(BasicBlock* bb) {
  DCHECK_EQ((cu_->disable_opt & (1 << kClassInitCheckElimination)), 0u);
  if (bb->block_type != kDalvikByteCode && bb->block_type != kEntryBlock) {
    // Ignore the kExitBlock as well.
    DCHECK(bb->first_mir_insn == nullptr);
    return false;
  }

  /*
   * Set initial state.  Catch blocks don't need any special treatment.
   */
  ArenaBitVector* classes_to_check = temp_.cice.work_classes_to_check;
  DCHECK(classes_to_check != nullptr);
  if (bb->block_type == kEntryBlock) {
    classes_to_check->SetInitialBits(temp_.cice.num_class_bits);
  } else {
    // Starting state is union of all incoming arcs.
    bool copied_first = false;
    for (BasicBlockId pred_id : bb->predecessors) {
      if (temp_.cice.ending_classes_to_check_matrix[pred_id] == nullptr) {
        continue;
      }
      if (!copied_first) {
        copied_first = true;
        classes_to_check->Copy(temp_.cice.ending_classes_to_check_matrix[pred_id]);
      } else {
        classes_to_check->Union(temp_.cice.ending_classes_to_check_matrix[pred_id]);
      }
    }
    DCHECK(copied_first);  // At least one predecessor must have been processed before this bb.
  }
  // At this point, classes_to_check shows which classes need clinit checks.

  // Walk through the instruction in the block, updating as necessary
  for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
    uint16_t index = temp_.cice.indexes[mir->offset / 2u];
    if (index != 0xffffu) {
      bool check_initialization = false;
      bool check_dex_cache = false;

      // NOTE: index != 0xffff does not guarantee that this is an SGET/SPUT/INVOKE_STATIC.
      // Dex instructions with width 1 can have the same offset/2.

      if (IsInstructionSGetOrSPut(mir->dalvikInsn.opcode)) {
        check_initialization = true;
        check_dex_cache = true;
      } else if (IsInstructionInvokeStatic(mir->dalvikInsn.opcode)) {
        check_initialization = true;
        // NOTE: INVOKE_STATIC doesn't guarantee that the type will be in the dex cache.
      }

      if (check_dex_cache) {
        uint32_t check_dex_cache_index = 2u * index + 1u;
        if (!classes_to_check->IsBitSet(check_dex_cache_index)) {
          // Eliminate the class init check.
          mir->optimization_flags |= MIR_CLASS_IS_IN_DEX_CACHE;
        } else {
          // Do the class init check.
          mir->optimization_flags &= ~MIR_CLASS_IS_IN_DEX_CACHE;
        }
        classes_to_check->ClearBit(check_dex_cache_index);
      }
      if (check_initialization) {
        uint32_t check_clinit_index = 2u * index;
        if (!classes_to_check->IsBitSet(check_clinit_index)) {
          // Eliminate the class init check.
          mir->optimization_flags |= MIR_CLASS_IS_INITIALIZED;
        } else {
          // Do the class init check.
          mir->optimization_flags &= ~MIR_CLASS_IS_INITIALIZED;
        }
        // Mark the class as initialized.
        classes_to_check->ClearBit(check_clinit_index);
      }
    }
  }

  // Did anything change?
  bool changed = false;
  ArenaBitVector* old_ending_classes_to_check = temp_.cice.ending_classes_to_check_matrix[bb->id];
  if (old_ending_classes_to_check == nullptr) {
    DCHECK(temp_scoped_alloc_.get() != nullptr);
    changed = classes_to_check->GetHighestBitSet() != -1;
    temp_.cice.ending_classes_to_check_matrix[bb->id] = classes_to_check;
    // Create a new classes_to_check for next BB.
    temp_.cice.work_classes_to_check = new (temp_scoped_alloc_.get()) ArenaBitVector(
        temp_scoped_alloc_.get(), temp_.cice.num_class_bits, false, kBitMapClInitCheck);
  } else if (!classes_to_check->Equal(old_ending_classes_to_check)) {
    changed = true;
    temp_.cice.ending_classes_to_check_matrix[bb->id] = classes_to_check;
    temp_.cice.work_classes_to_check = old_ending_classes_to_check;  // Reuse for next BB.
  }
  return changed;
}

void MIRGraph::EliminateClassInitChecksEnd() {
  // Clean up temporaries.
  temp_.cice.num_class_bits = 0u;
  temp_.cice.work_classes_to_check = nullptr;
  temp_.cice.ending_classes_to_check_matrix = nullptr;
  DCHECK(temp_.cice.indexes != nullptr);
  temp_.cice.indexes = nullptr;
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  temp_scoped_alloc_.reset();
}

static void DisableGVNDependentOptimizations(CompilationUnit* cu) {
  cu->disable_opt |= (1u << kGvnDeadCodeElimination);
}

bool MIRGraph::ApplyGlobalValueNumberingGate() {
  if (GlobalValueNumbering::Skip(cu_)) {
    DisableGVNDependentOptimizations(cu_);
    return false;
  }

  DCHECK(temp_scoped_alloc_ == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
  temp_.gvn.ifield_ids =
      GlobalValueNumbering::PrepareGvnFieldIds(temp_scoped_alloc_.get(), ifield_lowering_infos_);
  temp_.gvn.sfield_ids =
      GlobalValueNumbering::PrepareGvnFieldIds(temp_scoped_alloc_.get(), sfield_lowering_infos_);
  DCHECK(temp_.gvn.gvn == nullptr);
  temp_.gvn.gvn = new (temp_scoped_alloc_.get()) GlobalValueNumbering(
      cu_, temp_scoped_alloc_.get(), GlobalValueNumbering::kModeGvn);
  return true;
}

bool MIRGraph::ApplyGlobalValueNumbering(BasicBlock* bb) {
  DCHECK(temp_.gvn.gvn != nullptr);
  LocalValueNumbering* lvn = temp_.gvn.gvn->PrepareBasicBlock(bb);
  if (lvn != nullptr) {
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      lvn->GetValueNumber(mir);
    }
  }
  bool change = (lvn != nullptr) && temp_.gvn.gvn->FinishBasicBlock(bb);
  return change;
}

void MIRGraph::ApplyGlobalValueNumberingEnd() {
  // Perform modifications.
  DCHECK(temp_.gvn.gvn != nullptr);
  if (temp_.gvn.gvn->Good()) {
    temp_.gvn.gvn->StartPostProcessing();
    if (max_nested_loops_ != 0u) {
      TopologicalSortIterator iter(this);
      for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
        ScopedArenaAllocator allocator(&cu_->arena_stack);  // Reclaim memory after each LVN.
        LocalValueNumbering* lvn = temp_.gvn.gvn->PrepareBasicBlock(bb, &allocator);
        if (lvn != nullptr) {
          for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
            lvn->GetValueNumber(mir);
          }
          bool change = temp_.gvn.gvn->FinishBasicBlock(bb);
          DCHECK(!change) << PrettyMethod(cu_->method_idx, *cu_->dex_file);
        }
      }
    }
    // GVN was successful, running the LVN would be useless.
    cu_->disable_opt |= (1u << kLocalValueNumbering);
  } else {
    LOG(WARNING) << "GVN failed for " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
    DisableGVNDependentOptimizations(cu_);
  }
}

bool MIRGraph::EliminateDeadCodeGate() {
  if ((cu_->disable_opt & (1 << kGvnDeadCodeElimination)) != 0 || temp_.gvn.gvn == nullptr) {
    return false;
  }
  DCHECK(temp_scoped_alloc_ != nullptr);
  temp_.gvn.dce = new (temp_scoped_alloc_.get()) GvnDeadCodeElimination(temp_.gvn.gvn,
                                                                        temp_scoped_alloc_.get());
  return true;
}

bool MIRGraph::EliminateDeadCode(BasicBlock* bb) {
  DCHECK(temp_scoped_alloc_ != nullptr);
  DCHECK(temp_.gvn.gvn != nullptr);
  if (bb->block_type != kDalvikByteCode) {
    return false;
  }
  DCHECK(temp_.gvn.dce != nullptr);
  temp_.gvn.dce->Apply(bb);
  return false;  // No need to repeat.
}

void MIRGraph::EliminateDeadCodeEnd() {
  if (kIsDebugBuild) {
    // DCE can make some previously dead vregs alive again. Make sure the obsolete
    // live-in information is not used anymore.
    AllNodesIterator iter(this);
    for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
      if (bb->data_flow_info != nullptr) {
        bb->data_flow_info->live_in_v = nullptr;
      }
    }
  }
}

void MIRGraph::GlobalValueNumberingCleanup() {
  // If the GVN didn't run, these pointers should be null and everything is effectively no-op.
  delete temp_.gvn.dce;
  temp_.gvn.dce = nullptr;
  delete temp_.gvn.gvn;
  temp_.gvn.gvn = nullptr;
  temp_.gvn.ifield_ids = nullptr;
  temp_.gvn.sfield_ids = nullptr;
  temp_scoped_alloc_.reset();
}

void MIRGraph::ComputeInlineIFieldLoweringInfo(uint16_t field_idx, MIR* invoke, MIR* iget_or_iput) {
  uint32_t method_index = invoke->meta.method_lowering_info;
  if (temp_.smi.processed_indexes->IsBitSet(method_index)) {
    iget_or_iput->meta.ifield_lowering_info = temp_.smi.lowering_infos[method_index];
    DCHECK_EQ(field_idx, GetIFieldLoweringInfo(iget_or_iput).FieldIndex());
    return;
  }

  const MirMethodLoweringInfo& method_info = GetMethodLoweringInfo(invoke);
  MethodReference target = method_info.GetTargetMethod();
  DexCompilationUnit inlined_unit(
      cu_, cu_->class_loader, cu_->class_linker, *target.dex_file,
      nullptr /* code_item not used */, 0u /* class_def_idx not used */, target.dex_method_index,
      0u /* access_flags not used */, nullptr /* verified_method not used */);
  DexMemAccessType type = IGetOrIPutMemAccessType(iget_or_iput->dalvikInsn.opcode);
  MirIFieldLoweringInfo inlined_field_info(field_idx, type, false);
  MirIFieldLoweringInfo::Resolve(cu_->compiler_driver, &inlined_unit, &inlined_field_info, 1u);
  DCHECK(inlined_field_info.IsResolved());

  uint32_t field_info_index = ifield_lowering_infos_.size();
  ifield_lowering_infos_.push_back(inlined_field_info);
  temp_.smi.processed_indexes->SetBit(method_index);
  temp_.smi.lowering_infos[method_index] = field_info_index;
  iget_or_iput->meta.ifield_lowering_info = field_info_index;
}

bool MIRGraph::InlineSpecialMethodsGate() {
  if ((cu_->disable_opt & (1 << kSuppressMethodInlining)) != 0 ||
      method_lowering_infos_.size() == 0u) {
    return false;
  }
  if (cu_->compiler_driver->GetMethodInlinerMap() == nullptr) {
    // This isn't the Quick compiler.
    return false;
  }
  return true;
}

void MIRGraph::InlineSpecialMethodsStart() {
  // Prepare for inlining getters/setters. Since we're inlining at most 1 IGET/IPUT from
  // each INVOKE, we can index the data by the MIR::meta::method_lowering_info index.

  DCHECK(temp_scoped_alloc_.get() == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
  temp_.smi.num_indexes = method_lowering_infos_.size();
  temp_.smi.processed_indexes = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), temp_.smi.num_indexes, false, kBitMapMisc);
  temp_.smi.processed_indexes->ClearAllBits();
  temp_.smi.lowering_infos =
      temp_scoped_alloc_->AllocArray<uint16_t>(temp_.smi.num_indexes, kArenaAllocGrowableArray);
}

void MIRGraph::InlineSpecialMethods(BasicBlock* bb) {
  if (bb->block_type != kDalvikByteCode) {
    return;
  }
  for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
    if (MIR::DecodedInstruction::IsPseudoMirOp(mir->dalvikInsn.opcode)) {
      continue;
    }
    if (!(mir->dalvikInsn.FlagsOf() & Instruction::kInvoke)) {
      continue;
    }
    const MirMethodLoweringInfo& method_info = GetMethodLoweringInfo(mir);
    if (!method_info.FastPath() || !method_info.IsSpecial()) {
      continue;
    }

    InvokeType sharp_type = method_info.GetSharpType();
    if ((sharp_type != kDirect) && (sharp_type != kStatic)) {
      continue;
    }

    if (sharp_type == kStatic) {
      bool needs_clinit = !method_info.IsClassInitialized() &&
          ((mir->optimization_flags & MIR_CLASS_IS_INITIALIZED) == 0);
      if (needs_clinit) {
        continue;
      }
    }

    DCHECK(cu_->compiler_driver->GetMethodInlinerMap() != nullptr);
    MethodReference target = method_info.GetTargetMethod();
    if (cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(target.dex_file)
            ->GenInline(this, bb, mir, target.dex_method_index)) {
      if (cu_->verbose || cu_->print_pass) {
        LOG(INFO) << "SpecialMethodInliner: Inlined " << method_info.GetInvokeType() << " ("
            << sharp_type << ") call to \"" << PrettyMethod(target.dex_method_index,
                                                            *target.dex_file)
            << "\" from \"" << PrettyMethod(cu_->method_idx, *cu_->dex_file)
            << "\" @0x" << std::hex << mir->offset;
      }
    }
  }
}

void MIRGraph::InlineSpecialMethodsEnd() {
  // Clean up temporaries.
  DCHECK(temp_.smi.lowering_infos != nullptr);
  temp_.smi.lowering_infos = nullptr;
  temp_.smi.num_indexes = 0u;
  DCHECK(temp_.smi.processed_indexes != nullptr);
  temp_.smi.processed_indexes = nullptr;
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  temp_scoped_alloc_.reset();
}

void MIRGraph::DumpCheckStats() {
  Checkstats* stats =
      static_cast<Checkstats*>(arena_->Alloc(sizeof(Checkstats), kArenaAllocDFInfo));
  checkstats_ = stats;
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    CountChecks(bb);
  }
  if (stats->null_checks > 0) {
    float eliminated = static_cast<float>(stats->null_checks_eliminated);
    float checks = static_cast<float>(stats->null_checks);
    LOG(INFO) << "Null Checks: " << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
              << stats->null_checks_eliminated << " of " << stats->null_checks << " -> "
              << (eliminated/checks) * 100.0 << "%";
    }
  if (stats->range_checks > 0) {
    float eliminated = static_cast<float>(stats->range_checks_eliminated);
    float checks = static_cast<float>(stats->range_checks);
    LOG(INFO) << "Range Checks: " << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
              << stats->range_checks_eliminated << " of " << stats->range_checks << " -> "
              << (eliminated/checks) * 100.0 << "%";
  }
}

bool MIRGraph::BuildExtendedBBList(class BasicBlock* bb) {
  if (bb->visited) return false;
  if (!((bb->block_type == kEntryBlock) || (bb->block_type == kDalvikByteCode)
      || (bb->block_type == kExitBlock))) {
    // Ignore special blocks
    bb->visited = true;
    return false;
  }
  // Must be head of extended basic block.
  BasicBlock* start_bb = bb;
  extended_basic_blocks_.push_back(bb->id);
  bool terminated_by_return = false;
  bool do_local_value_numbering = false;
  // Visit blocks strictly dominated by this head.
  while (bb != nullptr) {
    bb->visited = true;
    terminated_by_return |= bb->terminated_by_return;
    do_local_value_numbering |= bb->use_lvn;
    bb = NextDominatedBlock(bb);
  }
  if (terminated_by_return || do_local_value_numbering) {
    // Do lvn for all blocks in this extended set.
    bb = start_bb;
    while (bb != nullptr) {
      bb->use_lvn = do_local_value_numbering;
      bb->dominates_return = terminated_by_return;
      bb = NextDominatedBlock(bb);
    }
  }
  return false;  // Not iterative - return value will be ignored
}

void MIRGraph::BasicBlockOptimizationStart() {
  if ((cu_->disable_opt & (1 << kLocalValueNumbering)) == 0) {
    temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
    temp_.gvn.ifield_ids =
        GlobalValueNumbering::PrepareGvnFieldIds(temp_scoped_alloc_.get(), ifield_lowering_infos_);
    temp_.gvn.sfield_ids =
        GlobalValueNumbering::PrepareGvnFieldIds(temp_scoped_alloc_.get(), sfield_lowering_infos_);
  }
}

void MIRGraph::BasicBlockOptimization() {
  if ((cu_->disable_opt & (1 << kSuppressExceptionEdges)) != 0) {
    ClearAllVisitedFlags();
    PreOrderDfsIterator iter2(this);
    for (BasicBlock* bb = iter2.Next(); bb != nullptr; bb = iter2.Next()) {
      BuildExtendedBBList(bb);
    }
    // Perform extended basic block optimizations.
    for (unsigned int i = 0; i < extended_basic_blocks_.size(); i++) {
      BasicBlockOpt(GetBasicBlock(extended_basic_blocks_[i]));
    }
  } else {
    PreOrderDfsIterator iter(this);
    for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
      BasicBlockOpt(bb);
    }
  }
}

void MIRGraph::BasicBlockOptimizationEnd() {
  // Clean up after LVN.
  temp_.gvn.ifield_ids = nullptr;
  temp_.gvn.sfield_ids = nullptr;
  temp_scoped_alloc_.reset();
}

void MIRGraph::StringChange() {
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      // Look for new instance opcodes, skip otherwise
      Instruction::Code opcode = mir->dalvikInsn.opcode;
      if (opcode == Instruction::NEW_INSTANCE) {
        uint32_t type_idx = mir->dalvikInsn.vB;
        if (cu_->compiler_driver->IsStringTypeIndex(type_idx, cu_->dex_file)) {
          // Change NEW_INSTANCE into CONST_4 of 0
          mir->dalvikInsn.opcode = Instruction::CONST_4;
          mir->dalvikInsn.vB = 0;
        }
      } else if ((opcode == Instruction::INVOKE_DIRECT) ||
                 (opcode == Instruction::INVOKE_DIRECT_RANGE)) {
        uint32_t method_idx = mir->dalvikInsn.vB;
        DexFileMethodInliner* inliner =
            cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(cu_->dex_file);
        if (inliner->IsStringInitMethodIndex(method_idx)) {
          bool is_range = (opcode == Instruction::INVOKE_DIRECT_RANGE);
          uint32_t orig_this_reg = is_range ? mir->dalvikInsn.vC : mir->dalvikInsn.arg[0];
          // Remove this pointer from string init and change to static call.
          mir->dalvikInsn.vA--;
          if (!is_range) {
            mir->dalvikInsn.opcode = Instruction::INVOKE_STATIC;
            for (uint32_t i = 0; i < mir->dalvikInsn.vA; i++) {
              mir->dalvikInsn.arg[i] = mir->dalvikInsn.arg[i + 1];
            }
          } else {
            mir->dalvikInsn.opcode = Instruction::INVOKE_STATIC_RANGE;
            mir->dalvikInsn.vC++;
          }
          // Insert a move-result instruction to the original this pointer reg.
          MIR* move_result_mir = static_cast<MIR *>(arena_->Alloc(sizeof(MIR), kArenaAllocMIR));
          move_result_mir->dalvikInsn.opcode = Instruction::MOVE_RESULT_OBJECT;
          move_result_mir->dalvikInsn.vA = orig_this_reg;
          move_result_mir->offset = mir->offset;
          move_result_mir->m_unit_index = mir->m_unit_index;
          bb->InsertMIRAfter(mir, move_result_mir);
          // Add additional moves if this pointer was copied to other registers.
          const VerifiedMethod* verified_method =
              cu_->compiler_driver->GetVerifiedMethod(cu_->dex_file, cu_->method_idx);
          DCHECK(verified_method != nullptr);
          const SafeMap<uint32_t, std::set<uint32_t>>& string_init_map =
              verified_method->GetStringInitPcRegMap();
          auto map_it = string_init_map.find(mir->offset);
          if (map_it != string_init_map.end()) {
            const std::set<uint32_t>& reg_set = map_it->second;
            for (auto set_it = reg_set.begin(); set_it != reg_set.end(); ++set_it) {
              MIR* move_mir = static_cast<MIR *>(arena_->Alloc(sizeof(MIR), kArenaAllocMIR));
              move_mir->dalvikInsn.opcode = Instruction::MOVE_OBJECT;
              move_mir->dalvikInsn.vA = *set_it;
              move_mir->dalvikInsn.vB = orig_this_reg;
              move_mir->offset = mir->offset;
              move_mir->m_unit_index = mir->m_unit_index;
              bb->InsertMIRAfter(move_result_mir, move_mir);
            }
          }
        }
      }
    }
  }
}


bool MIRGraph::EliminateSuspendChecksGate() {
  if (kLeafOptimization ||           // Incompatible (could create loops without suspend checks).
      (cu_->disable_opt & (1 << kSuspendCheckElimination)) != 0 ||  // Disabled.
      GetMaxNestedLoops() == 0u ||   // Nothing to do.
      GetMaxNestedLoops() >= 32u ||  // Only 32 bits in suspend_checks_in_loops_[.].
                                     // Exclude 32 as well to keep bit shifts well-defined.
      !HasInvokes()) {               // No invokes to actually eliminate any suspend checks.
    return false;
  }
  suspend_checks_in_loops_ = arena_->AllocArray<uint32_t>(GetNumBlocks(), kArenaAllocMisc);
  return true;
}

bool MIRGraph::EliminateSuspendChecks(BasicBlock* bb) {
  if (bb->block_type != kDalvikByteCode) {
    return false;
  }
  DCHECK_EQ(GetTopologicalSortOrderLoopHeadStack()->size(), bb->nesting_depth);
  if (bb->nesting_depth == 0u) {
    // Out of loops.
    DCHECK_EQ(suspend_checks_in_loops_[bb->id], 0u);  // The array was zero-initialized.
    return false;
  }
  uint32_t suspend_checks_in_loops = (1u << bb->nesting_depth) - 1u;  // Start with all loop heads.
  bool found_invoke = false;
  for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
    if ((IsInstructionInvoke(mir->dalvikInsn.opcode) ||
        IsInstructionQuickInvoke(mir->dalvikInsn.opcode)) &&
        !GetMethodLoweringInfo(mir).IsIntrinsic()) {
      // Non-intrinsic invoke, rely on a suspend point in the invoked method.
      found_invoke = true;
      break;
    }
  }
  if (!found_invoke) {
    // Intersect suspend checks from predecessors.
    uint16_t bb_topo_idx = topological_order_indexes_[bb->id];
    uint32_t pred_mask_union = 0u;
    for (BasicBlockId pred_id : bb->predecessors) {
      uint16_t pred_topo_idx = topological_order_indexes_[pred_id];
      if (pred_topo_idx < bb_topo_idx) {
        // Determine the loop depth of the predecessors relative to this block.
        size_t pred_loop_depth = topological_order_loop_head_stack_.size();
        while (pred_loop_depth != 0u &&
            pred_topo_idx < topological_order_loop_head_stack_[pred_loop_depth - 1].first) {
          --pred_loop_depth;
        }
        DCHECK_LE(pred_loop_depth, GetBasicBlock(pred_id)->nesting_depth);
        uint32_t pred_mask = (1u << pred_loop_depth) - 1u;
        // Intersect pred_mask bits in suspend_checks_in_loops with
        // suspend_checks_in_loops_[pred_id].
        uint32_t pred_loops_without_checks = pred_mask & ~suspend_checks_in_loops_[pred_id];
        suspend_checks_in_loops = suspend_checks_in_loops & ~pred_loops_without_checks;
        pred_mask_union |= pred_mask;
      }
    }
    // DCHECK_EQ() may not hold for unnatural loop heads, so use DCHECK_GE().
    DCHECK_GE(((1u << (IsLoopHead(bb->id) ? bb->nesting_depth - 1u: bb->nesting_depth)) - 1u),
              pred_mask_union);
    suspend_checks_in_loops &= pred_mask_union;
  }
  suspend_checks_in_loops_[bb->id] = suspend_checks_in_loops;
  if (suspend_checks_in_loops == 0u) {
    return false;
  }
  // Apply MIR_IGNORE_SUSPEND_CHECK if appropriate.
  if (bb->taken != NullBasicBlockId) {
    DCHECK(bb->last_mir_insn != nullptr);
    DCHECK(IsInstructionIfCc(bb->last_mir_insn->dalvikInsn.opcode) ||
           IsInstructionIfCcZ(bb->last_mir_insn->dalvikInsn.opcode) ||
           IsInstructionGoto(bb->last_mir_insn->dalvikInsn.opcode) ||
           (static_cast<int>(bb->last_mir_insn->dalvikInsn.opcode) >= kMirOpFusedCmplFloat &&
            static_cast<int>(bb->last_mir_insn->dalvikInsn.opcode) <= kMirOpFusedCmpLong));
    if (!IsSuspendCheckEdge(bb, bb->taken) &&
        (bb->fall_through == NullBasicBlockId || !IsSuspendCheckEdge(bb, bb->fall_through))) {
      bb->last_mir_insn->optimization_flags |= MIR_IGNORE_SUSPEND_CHECK;
    }
  } else if (bb->fall_through != NullBasicBlockId && IsSuspendCheckEdge(bb, bb->fall_through)) {
    // We've got a fall-through suspend edge. Add an artificial GOTO to force suspend check.
    MIR* mir = NewMIR();
    mir->dalvikInsn.opcode = Instruction::GOTO;
    mir->dalvikInsn.vA = 0;  // Branch offset.
    mir->offset = GetBasicBlock(bb->fall_through)->start_offset;
    mir->m_unit_index = current_method_;
    mir->ssa_rep = reinterpret_cast<SSARepresentation*>(
        arena_->Alloc(sizeof(SSARepresentation), kArenaAllocDFInfo));  // Zero-initialized.
    bb->AppendMIR(mir);
    std::swap(bb->fall_through, bb->taken);  // The fall-through has become taken.
  }
  return true;
}

bool MIRGraph::CanThrow(MIR* mir) const {
  if ((mir->dalvikInsn.FlagsOf() & Instruction::kThrow) == 0) {
    return false;
  }
  const int opt_flags = mir->optimization_flags;
  uint64_t df_attributes = GetDataFlowAttributes(mir);

  // First, check if the insn can still throw NPE.
  if (((df_attributes & DF_HAS_NULL_CHKS) != 0) && ((opt_flags & MIR_IGNORE_NULL_CHECK) == 0)) {
    return true;
  }

  // Now process specific instructions.
  if ((df_attributes & DF_IFIELD) != 0) {
    // The IGET/IPUT family. We have processed the IGET/IPUT null check above.
    DCHECK_NE(opt_flags & MIR_IGNORE_NULL_CHECK, 0);
    // If not fast, weird things can happen and the insn can throw.
    const MirIFieldLoweringInfo& field_info = GetIFieldLoweringInfo(mir);
    bool fast = (df_attributes & DF_DA) != 0 ? field_info.FastGet() : field_info.FastPut();
    return !fast;
  } else if ((df_attributes & DF_SFIELD) != 0) {
    // The SGET/SPUT family. Check for potentially throwing class initialization.
    // Also, if not fast, weird things can happen and the insn can throw.
    const MirSFieldLoweringInfo& field_info = GetSFieldLoweringInfo(mir);
    bool fast = (df_attributes & DF_DA) != 0 ? field_info.FastGet() : field_info.FastPut();
    bool is_class_initialized = field_info.IsClassInitialized() ||
        ((mir->optimization_flags & MIR_CLASS_IS_INITIALIZED) != 0);
    return !(fast && is_class_initialized);
  } else if ((df_attributes & DF_HAS_RANGE_CHKS) != 0) {
    // Only AGET/APUT have range checks. We have processed the AGET/APUT null check above.
    DCHECK_NE(opt_flags & MIR_IGNORE_NULL_CHECK, 0);
    // Non-throwing only if range check has been eliminated.
    return ((opt_flags & MIR_IGNORE_RANGE_CHECK) == 0);
  } else if (mir->dalvikInsn.opcode == Instruction::CHECK_CAST &&
      (opt_flags & MIR_IGNORE_CHECK_CAST) != 0) {
    return false;
  } else if (mir->dalvikInsn.opcode == Instruction::ARRAY_LENGTH ||
      static_cast<int>(mir->dalvikInsn.opcode) == kMirOpNullCheck) {
    // No more checks for these (null check was processed above).
    return false;
  }
  return true;
}

bool MIRGraph::HasAntiDependency(MIR* first, MIR* second) {
  DCHECK(first->ssa_rep != nullptr);
  DCHECK(second->ssa_rep != nullptr);
  if ((second->ssa_rep->num_defs > 0) && (first->ssa_rep->num_uses > 0)) {
    int vreg0 = SRegToVReg(second->ssa_rep->defs[0]);
    int vreg1 = (second->ssa_rep->num_defs == 2) ?
        SRegToVReg(second->ssa_rep->defs[1]) : INVALID_VREG;
    for (int i = 0; i < first->ssa_rep->num_uses; i++) {
      int32_t use = SRegToVReg(first->ssa_rep->uses[i]);
      if (use == vreg0 || use == vreg1) {
        return true;
      }
    }
  }
  return false;
}

void MIRGraph::CombineMultiplyAdd(MIR* mul_mir, MIR* add_mir, bool mul_is_first_addend,
                                  bool is_wide, bool is_sub) {
  if (is_wide) {
    if (is_sub) {
      add_mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpMsubLong);
    } else {
      add_mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpMaddLong);
    }
  } else {
    if (is_sub) {
      add_mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpMsubInt);
    } else {
      add_mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpMaddInt);
    }
  }
  add_mir->ssa_rep->num_uses = is_wide ? 6 : 3;
  int32_t addend0 = INVALID_SREG;
  int32_t addend1 = INVALID_SREG;
  if (is_wide) {
    addend0 = mul_is_first_addend ? add_mir->ssa_rep->uses[2] : add_mir->ssa_rep->uses[0];
    addend1 = mul_is_first_addend ? add_mir->ssa_rep->uses[3] : add_mir->ssa_rep->uses[1];
  } else {
    addend0 = mul_is_first_addend ? add_mir->ssa_rep->uses[1] : add_mir->ssa_rep->uses[0];
  }

  AllocateSSAUseData(add_mir, add_mir->ssa_rep->num_uses);
  add_mir->ssa_rep->uses[0] = mul_mir->ssa_rep->uses[0];
  add_mir->ssa_rep->uses[1] = mul_mir->ssa_rep->uses[1];
  // Clear the original multiply product ssa use count, as it is not used anymore.
  raw_use_counts_[mul_mir->ssa_rep->defs[0]] = 0;
  use_counts_[mul_mir->ssa_rep->defs[0]] = 0;
  if (is_wide) {
    DCHECK_EQ(add_mir->ssa_rep->num_uses, 6);
    add_mir->ssa_rep->uses[2] = mul_mir->ssa_rep->uses[2];
    add_mir->ssa_rep->uses[3] = mul_mir->ssa_rep->uses[3];
    add_mir->ssa_rep->uses[4] = addend0;
    add_mir->ssa_rep->uses[5] = addend1;
    raw_use_counts_[mul_mir->ssa_rep->defs[1]] = 0;
    use_counts_[mul_mir->ssa_rep->defs[1]] = 0;
  } else {
    DCHECK_EQ(add_mir->ssa_rep->num_uses, 3);
    add_mir->ssa_rep->uses[2] = addend0;
  }
  // Copy in the decoded instruction information.
  add_mir->dalvikInsn.vB = SRegToVReg(add_mir->ssa_rep->uses[0]);
  if (is_wide) {
    add_mir->dalvikInsn.vC = SRegToVReg(add_mir->ssa_rep->uses[2]);
    add_mir->dalvikInsn.arg[0] = SRegToVReg(add_mir->ssa_rep->uses[4]);
  } else {
    add_mir->dalvikInsn.vC = SRegToVReg(add_mir->ssa_rep->uses[1]);
    add_mir->dalvikInsn.arg[0] = SRegToVReg(add_mir->ssa_rep->uses[2]);
  }
  // Original multiply MIR is set to Nop.
  mul_mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
}

void MIRGraph::MultiplyAddOpt(BasicBlock* bb) {
  if (bb->block_type == kDead) {
    return;
  }
  ScopedArenaAllocator allocator(&cu_->arena_stack);
  ScopedArenaSafeMap<uint32_t, MIR*> ssa_mul_map(std::less<uint32_t>(), allocator.Adapter());
  ScopedArenaSafeMap<uint32_t, MIR*>::iterator map_it;
  for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
    Instruction::Code opcode = mir->dalvikInsn.opcode;
    bool is_sub = true;
    bool is_candidate_multiply = false;
    switch (opcode) {
      case Instruction::MUL_INT:
      case Instruction::MUL_INT_2ADDR:
        is_candidate_multiply = true;
        break;
      case Instruction::MUL_LONG:
      case Instruction::MUL_LONG_2ADDR:
        if (cu_->target64) {
          is_candidate_multiply = true;
        }
        break;
      case Instruction::ADD_INT:
      case Instruction::ADD_INT_2ADDR:
        is_sub = false;
        FALLTHROUGH_INTENDED;
      case Instruction::SUB_INT:
      case Instruction::SUB_INT_2ADDR:
        if (((map_it = ssa_mul_map.find(mir->ssa_rep->uses[0])) != ssa_mul_map.end()) && !is_sub) {
          // a*b+c
          CombineMultiplyAdd(map_it->second, mir, true /* product is the first addend */,
                             false /* is_wide */, false /* is_sub */);
          ssa_mul_map.erase(mir->ssa_rep->uses[0]);
        } else if ((map_it = ssa_mul_map.find(mir->ssa_rep->uses[1])) != ssa_mul_map.end()) {
          // c+a*b or c-a*b
          CombineMultiplyAdd(map_it->second, mir, false /* product is the second addend */,
                             false /* is_wide */, is_sub);
          ssa_mul_map.erase(map_it);
        }
        break;
      case Instruction::ADD_LONG:
      case Instruction::ADD_LONG_2ADDR:
        is_sub = false;
        FALLTHROUGH_INTENDED;
      case Instruction::SUB_LONG:
      case Instruction::SUB_LONG_2ADDR:
        if (!cu_->target64) {
          break;
        }
        if ((map_it = ssa_mul_map.find(mir->ssa_rep->uses[0])) != ssa_mul_map.end() && !is_sub) {
          // a*b+c
          CombineMultiplyAdd(map_it->second, mir, true /* product is the first addend */,
                             true /* is_wide */, false /* is_sub */);
          ssa_mul_map.erase(map_it);
        } else if ((map_it = ssa_mul_map.find(mir->ssa_rep->uses[2])) != ssa_mul_map.end()) {
          // c+a*b or c-a*b
          CombineMultiplyAdd(map_it->second, mir, false /* product is the second addend */,
                             true /* is_wide */, is_sub);
          ssa_mul_map.erase(map_it);
        }
        break;
      default:
        if (!ssa_mul_map.empty() && CanThrow(mir)) {
          // Should not combine multiply and add MIRs across potential exception.
          ssa_mul_map.clear();
        }
        break;
    }

    // Exclude the case when an MIR writes a vreg which is previous candidate multiply MIR's uses.
    // It is because that current RA may allocate the same physical register to them. For this
    // kind of cases, the multiplier has been updated, we should not use updated value to the
    // multiply-add insn.
    if (ssa_mul_map.size() > 0) {
      for (auto it = ssa_mul_map.begin(); it != ssa_mul_map.end();) {
        MIR* mul = it->second;
        if (HasAntiDependency(mul, mir)) {
          it = ssa_mul_map.erase(it);
        } else {
          ++it;
        }
      }
    }

    if (is_candidate_multiply &&
        (GetRawUseCount(mir->ssa_rep->defs[0]) == 1) && (mir->next != nullptr)) {
      ssa_mul_map.Put(mir->ssa_rep->defs[0], mir);
    }
  }
}

}  // namespace art
