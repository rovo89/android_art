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

#include "compiler_internals.h"
#include "local_value_numbering.h"
#include "dataflow_iterator-inl.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "utils/scoped_arena_containers.h"

namespace art {

static unsigned int Predecessors(BasicBlock* bb) {
  return bb->predecessors->Size();
}

/* Setup a constant value for opcodes thare have the DF_SETS_CONST attribute */
void MIRGraph::SetConstant(int32_t ssa_reg, int value) {
  is_constant_v_->SetBit(ssa_reg);
  constant_values_[ssa_reg] = value;
}

void MIRGraph::SetConstantWide(int ssa_reg, int64_t value) {
  is_constant_v_->SetBit(ssa_reg);
  constant_values_[ssa_reg] = Low32Bits(value);
  constant_values_[ssa_reg + 1] = High32Bits(value);
}

void MIRGraph::DoConstantPropagation(BasicBlock* bb) {
  MIR* mir;

  for (mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
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
  if (mir != NULL) {
    mir = mir->next;
    if (mir == NULL) {
      bb = GetBasicBlock(bb->fall_through);
      if ((bb == NULL) || Predecessors(bb) != 1) {
        mir = NULL;
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
 * a move-result, return it.  Else, return NULL.  If a move-result exists,
 * it is required to immediately follow the invoke with no intervening
 * opcodes or incoming arcs.  However, if the result of the invoke is not
 * used, a move-result may not be present.
 */
MIR* MIRGraph::FindMoveResult(BasicBlock* bb, MIR* mir) {
  BasicBlock* tbb = bb;
  mir = AdvanceMIR(&tbb, mir);
  while (mir != NULL) {
    int opcode = mir->dalvikInsn.opcode;
    if ((mir->dalvikInsn.opcode == Instruction::MOVE_RESULT) ||
        (mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) ||
        (mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_WIDE)) {
      break;
    }
    // Keep going if pseudo op, otherwise terminate
    if (opcode < kNumPackedOpcodes) {
      mir = NULL;
    } else {
      mir = AdvanceMIR(&tbb, mir);
    }
  }
  return mir;
}

BasicBlock* MIRGraph::NextDominatedBlock(BasicBlock* bb) {
  if (bb->block_type == kDead) {
    return NULL;
  }
  DCHECK((bb->block_type == kEntryBlock) || (bb->block_type == kDalvikByteCode)
      || (bb->block_type == kExitBlock));
  BasicBlock* bb_taken = GetBasicBlock(bb->taken);
  BasicBlock* bb_fall_through = GetBasicBlock(bb->fall_through);
  if (((bb_fall_through == NULL) && (bb_taken != NULL)) &&
      ((bb_taken->block_type == kDalvikByteCode) || (bb_taken->block_type == kExitBlock))) {
    // Follow simple unconditional branches.
    bb = bb_taken;
  } else {
    // Follow simple fallthrough
    bb = (bb_taken != NULL) ? NULL : bb_fall_through;
  }
  if (bb == NULL || (Predecessors(bb) != 1)) {
    return NULL;
  }
  DCHECK((bb->block_type == kDalvikByteCode) || (bb->block_type == kExitBlock));
  return bb;
}

static MIR* FindPhi(BasicBlock* bb, int ssa_name) {
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    if (static_cast<int>(mir->dalvikInsn.opcode) == kMirOpPhi) {
      for (int i = 0; i < mir->ssa_rep->num_uses; i++) {
        if (mir->ssa_rep->uses[i] == ssa_name) {
          return mir;
        }
      }
    }
  }
  return NULL;
}

static SelectInstructionKind SelectKind(MIR* mir) {
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

COMPILE_ASSERT(arraysize(kIfCcZConditionCodes) == Instruction::IF_LEZ - Instruction::IF_EQZ + 1,
               if_ccz_ccodes_size1);

static constexpr bool IsInstructionIfCcZ(Instruction::Code opcode) {
  return Instruction::IF_EQZ <= opcode && opcode <= Instruction::IF_LEZ;
}

static constexpr ConditionCode ConditionCodeForIfCcZ(Instruction::Code opcode) {
  return kIfCcZConditionCodes[opcode - Instruction::IF_EQZ];
}

COMPILE_ASSERT(ConditionCodeForIfCcZ(Instruction::IF_EQZ) == kCondEq, check_if_eqz_ccode);
COMPILE_ASSERT(ConditionCodeForIfCcZ(Instruction::IF_NEZ) == kCondNe, check_if_nez_ccode);
COMPILE_ASSERT(ConditionCodeForIfCcZ(Instruction::IF_LTZ) == kCondLt, check_if_ltz_ccode);
COMPILE_ASSERT(ConditionCodeForIfCcZ(Instruction::IF_GEZ) == kCondGe, check_if_gez_ccode);
COMPILE_ASSERT(ConditionCodeForIfCcZ(Instruction::IF_GTZ) == kCondGt, check_if_gtz_ccode);
COMPILE_ASSERT(ConditionCodeForIfCcZ(Instruction::IF_LEZ) == kCondLe, check_if_lez_ccode);

int MIRGraph::GetSSAUseCount(int s_reg) {
  return raw_use_counts_.Get(s_reg);
}

size_t MIRGraph::GetNumAvailableNonSpecialCompilerTemps() {
  if (num_non_special_compiler_temps_ >= max_available_non_special_compiler_temps_) {
    return 0;
  } else {
    return max_available_non_special_compiler_temps_ - num_non_special_compiler_temps_;
  }
}


// FIXME - will probably need to revisit all uses of this, as type not defined.
static const RegLocation temp_loc = {kLocCompilerTemp,
                                     0, 1 /*defined*/, 0, 0, 0, 0, 0, 1 /*home*/,
                                     RegStorage(), INVALID_SREG, INVALID_SREG};

CompilerTemp* MIRGraph::GetNewCompilerTemp(CompilerTempType ct_type, bool wide) {
  // There is a limit to the number of non-special temps so check to make sure it wasn't exceeded.
  if (ct_type == kCompilerTempVR) {
    size_t available_temps = GetNumAvailableNonSpecialCompilerTemps();
    if (available_temps <= 0 || (available_temps <= 1 && wide)) {
      return 0;
    }
  }

  CompilerTemp *compiler_temp = static_cast<CompilerTemp *>(arena_->Alloc(sizeof(CompilerTemp),
                                                            kArenaAllocRegAlloc));

  // Create the type of temp requested. Special temps need special handling because
  // they have a specific virtual register assignment.
  if (ct_type == kCompilerTempSpecialMethodPtr) {
    DCHECK_EQ(wide, false);
    compiler_temp->v_reg = static_cast<int>(kVRegMethodPtrBaseReg);
    compiler_temp->s_reg_low = AddNewSReg(compiler_temp->v_reg);

    // The MIR graph keeps track of the sreg for method pointer specially, so record that now.
    method_sreg_ = compiler_temp->s_reg_low;
  } else {
    DCHECK_EQ(ct_type, kCompilerTempVR);

    // The new non-special compiler temp must receive a unique v_reg with a negative value.
    compiler_temp->v_reg = static_cast<int>(kVRegNonSpecialTempBaseReg) - num_non_special_compiler_temps_;
    compiler_temp->s_reg_low = AddNewSReg(compiler_temp->v_reg);
    num_non_special_compiler_temps_++;

    if (wide) {
      // Ensure that the two registers are consecutive. Since the virtual registers used for temps grow in a
      // negative fashion, we need the smaller to refer to the low part. Thus, we redefine the v_reg and s_reg_low.
      compiler_temp->v_reg--;
      int ssa_reg_high = compiler_temp->s_reg_low;
      compiler_temp->s_reg_low = AddNewSReg(compiler_temp->v_reg);
      int ssa_reg_low = compiler_temp->s_reg_low;

      // If needed initialize the register location for the high part.
      // The low part is handled later in this method on a common path.
      if (reg_location_ != nullptr) {
        reg_location_[ssa_reg_high] = temp_loc;
        reg_location_[ssa_reg_high].high_word = 1;
        reg_location_[ssa_reg_high].s_reg_low = ssa_reg_low;
        reg_location_[ssa_reg_high].wide = true;
      }

      num_non_special_compiler_temps_++;
    }
  }

  // Have we already allocated the register locations?
  if (reg_location_ != nullptr) {
    int ssa_reg_low = compiler_temp->s_reg_low;
    reg_location_[ssa_reg_low] = temp_loc;
    reg_location_[ssa_reg_low].s_reg_low = ssa_reg_low;
    reg_location_[ssa_reg_low].wide = wide;
  }

  compiler_temps_.Insert(compiler_temp);
  return compiler_temp;
}

/* Do some MIR-level extended basic block optimizations */
bool MIRGraph::BasicBlockOpt(BasicBlock* bb) {
  if (bb->block_type == kDead) {
    return true;
  }
  bool use_lvn = bb->use_lvn;
  UniquePtr<LocalValueNumbering> local_valnum;
  if (use_lvn) {
    local_valnum.reset(LocalValueNumbering::Create(cu_));
  }
  while (bb != NULL) {
    for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
      // TUNING: use the returned value number for CSE.
      if (use_lvn) {
        local_valnum->GetValueNumber(mir);
      }
      // Look for interesting opcodes, skip otherwise
      Instruction::Code opcode = mir->dalvikInsn.opcode;
      switch (opcode) {
        case Instruction::CMPL_FLOAT:
        case Instruction::CMPL_DOUBLE:
        case Instruction::CMPG_FLOAT:
        case Instruction::CMPG_DOUBLE:
        case Instruction::CMP_LONG:
          if ((cu_->disable_opt & (1 << kBranchFusing)) != 0) {
            // Bitcode doesn't allow this optimization.
            break;
          }
          if (mir->next != NULL) {
            MIR* mir_next = mir->next;
            // Make sure result of cmp is used by next insn and nowhere else
            if (IsInstructionIfCcZ(mir->next->dalvikInsn.opcode) &&
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
              mir_next->ssa_rep->num_uses = mir->ssa_rep->num_uses;
              mir_next->ssa_rep->uses = mir->ssa_rep->uses;
              mir_next->ssa_rep->fp_use = mir->ssa_rep->fp_use;
              mir_next->ssa_rep->num_defs = 0;
              mir->ssa_rep->num_uses = 0;
              mir->ssa_rep->num_defs = 0;
            }
          }
          break;
        case Instruction::GOTO:
        case Instruction::GOTO_16:
        case Instruction::GOTO_32:
        case Instruction::IF_EQ:
        case Instruction::IF_NE:
        case Instruction::IF_LT:
        case Instruction::IF_GE:
        case Instruction::IF_GT:
        case Instruction::IF_LE:
        case Instruction::IF_EQZ:
        case Instruction::IF_NEZ:
        case Instruction::IF_LTZ:
        case Instruction::IF_GEZ:
        case Instruction::IF_GTZ:
        case Instruction::IF_LEZ:
          // If we've got a backwards branch to return, no need to suspend check.
          if ((IsBackedge(bb, bb->taken) && GetBasicBlock(bb->taken)->dominates_return) ||
              (IsBackedge(bb, bb->fall_through) &&
                          GetBasicBlock(bb->fall_through)->dominates_return)) {
            mir->optimization_flags |= MIR_IGNORE_SUSPEND_CHECK;
            if (cu_->verbose) {
              LOG(INFO) << "Suppressed suspend check on branch to return at 0x" << std::hex
                        << mir->offset;
            }
          }
          break;
        default:
          break;
      }
      // Is this the select pattern?
      // TODO: flesh out support for Mips.  NOTE: llvm's select op doesn't quite work here.
      // TUNING: expand to support IF_xx compare & branches
      if (!cu_->compiler->IsPortable() &&
          (cu_->instruction_set == kThumb2 || cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) &&
          IsInstructionIfCcZ(mir->dalvikInsn.opcode)) {
        BasicBlock* ft = GetBasicBlock(bb->fall_through);
        DCHECK(ft != NULL);
        BasicBlock* ft_ft = GetBasicBlock(ft->fall_through);
        BasicBlock* ft_tk = GetBasicBlock(ft->taken);

        BasicBlock* tk = GetBasicBlock(bb->taken);
        DCHECK(tk != NULL);
        BasicBlock* tk_ft = GetBasicBlock(tk->fall_through);
        BasicBlock* tk_tk = GetBasicBlock(tk->taken);

        /*
         * In the select pattern, the taken edge goes to a block that unconditionally
         * transfers to the rejoin block and the fall_though edge goes to a block that
         * unconditionally falls through to the rejoin block.
         */
        if ((tk_ft == NULL) && (ft_tk == NULL) && (tk_tk == ft_ft) &&
            (Predecessors(tk) == 1) && (Predecessors(ft) == 1)) {
          /*
           * Okay - we have the basic diamond shape.  At the very least, we can eliminate the
           * suspend check on the taken-taken branch back to the join point.
           */
          if (SelectKind(tk->last_mir_insn) == kSelectGoto) {
              tk->last_mir_insn->optimization_flags |= (MIR_IGNORE_SUSPEND_CHECK);
          }
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
            if ((phi != NULL) && (if_true->dalvikInsn.vA == if_false->dalvikInsn.vA)) {
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
                int* src_ssa =
                    static_cast<int*>(arena_->Alloc(sizeof(int) * 3, kArenaAllocDFInfo));
                src_ssa[0] = mir->ssa_rep->uses[0];
                src_ssa[1] = if_true->ssa_rep->uses[0];
                src_ssa[2] = if_false->ssa_rep->uses[0];
                mir->ssa_rep->uses = src_ssa;
                mir->ssa_rep->num_uses = 3;
              }
              mir->ssa_rep->num_defs = 1;
              mir->ssa_rep->defs =
                  static_cast<int*>(arena_->Alloc(sizeof(int) * 1, kArenaAllocDFInfo));
              mir->ssa_rep->fp_def =
                  static_cast<bool*>(arena_->Alloc(sizeof(bool) * 1, kArenaAllocDFInfo));
              mir->ssa_rep->fp_def[0] = if_true->ssa_rep->fp_def[0];
              // Match type of uses to def.
              mir->ssa_rep->fp_use =
                  static_cast<bool*>(arena_->Alloc(sizeof(bool) * mir->ssa_rep->num_uses,
                                                   kArenaAllocDFInfo));
              for (int i = 0; i < mir->ssa_rep->num_uses; i++) {
                mir->ssa_rep->fp_use[i] = mir->ssa_rep->fp_def[0];
              }
              /*
               * There is usually a Phi node in the join block for our two cases.  If the
               * Phi node only contains our two cases as input, we will use the result
               * SSA name of the Phi node as our select result and delete the Phi.  If
               * the Phi node has more than two operands, we will arbitrarily use the SSA
               * name of the "true" path, delete the SSA name of the "false" path from the
               * Phi node (and fix up the incoming arc list).
               */
              if (phi->ssa_rep->num_uses == 2) {
                mir->ssa_rep->defs[0] = phi->ssa_rep->defs[0];
                phi->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
              } else {
                int dead_def = if_false->ssa_rep->defs[0];
                int live_def = if_true->ssa_rep->defs[0];
                mir->ssa_rep->defs[0] = live_def;
                BasicBlockId* incoming = phi->meta.phi_incoming;
                for (int i = 0; i < phi->ssa_rep->num_uses; i++) {
                  if (phi->ssa_rep->uses[i] == live_def) {
                    incoming[i] = bb->id;
                  }
                }
                for (int i = 0; i < phi->ssa_rep->num_uses; i++) {
                  if (phi->ssa_rep->uses[i] == dead_def) {
                    int last_slot = phi->ssa_rep->num_uses - 1;
                    phi->ssa_rep->uses[i] = phi->ssa_rep->uses[last_slot];
                    incoming[i] = incoming[last_slot];
                  }
                }
              }
              phi->ssa_rep->num_uses--;
              bb->taken = NullBasicBlockId;
              tk->block_type = kDead;
              for (MIR* tmir = ft->first_mir_insn; tmir != NULL; tmir = tmir->next) {
                tmir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
              }
            }
          }
        }
      }
    }
    bb = ((cu_->disable_opt & (1 << kSuppressExceptionEdges)) != 0) ? NextDominatedBlock(bb) : NULL;
  }

  return true;
}

/* Collect stats on number of checks removed */
void MIRGraph::CountChecks(struct BasicBlock* bb) {
  if (bb->data_flow_info != NULL) {
    for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
      if (mir->ssa_rep == NULL) {
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

/* Try to make common case the fallthrough path */
bool MIRGraph::LayoutBlocks(BasicBlock* bb) {
  // TODO: For now, just looking for direct throws.  Consider generalizing for profile feedback
  if (!bb->explicit_throw) {
    return false;
  }
  BasicBlock* walker = bb;
  while (true) {
    // Check termination conditions
    if ((walker->block_type == kEntryBlock) || (Predecessors(walker) != 1)) {
      break;
    }
    BasicBlock* prev = GetBasicBlock(walker->predecessors->Get(0));
    if (prev->conditional_branch) {
      if (GetBasicBlock(prev->fall_through) == walker) {
        // Already done - return
        break;
      }
      DCHECK_EQ(walker, GetBasicBlock(prev->taken));
      // Got one.  Flip it and exit
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
void MIRGraph::CombineBlocks(struct BasicBlock* bb) {
  // Loop here to allow combining a sequence of blocks
  while (true) {
    // Check termination conditions
    if ((bb->first_mir_insn == NULL)
        || (bb->data_flow_info == NULL)
        || (bb->block_type == kExceptionHandling)
        || (bb->block_type == kExitBlock)
        || (bb->block_type == kDead)
        || (bb->taken == NullBasicBlockId)
        || (GetBasicBlock(bb->taken)->block_type != kExceptionHandling)
        || (bb->successor_block_list_type != kNotUsed)
        || (static_cast<int>(bb->last_mir_insn->dalvikInsn.opcode) != kMirOpCheck)) {
      break;
    }

    // Test the kMirOpCheck instruction
    MIR* mir = bb->last_mir_insn;
    // Grab the attributes from the paired opcode
    MIR* throw_insn = mir->meta.throw_insn;
    uint64_t df_attributes = GetDataFlowAttributes(throw_insn);
    bool can_combine = true;
    if (df_attributes & DF_HAS_NULL_CHKS) {
      can_combine &= ((throw_insn->optimization_flags & MIR_IGNORE_NULL_CHECK) != 0);
    }
    if (df_attributes & DF_HAS_RANGE_CHKS) {
      can_combine &= ((throw_insn->optimization_flags & MIR_IGNORE_RANGE_CHECK) != 0);
    }
    if (!can_combine) {
      break;
    }
    // OK - got one.  Combine
    BasicBlock* bb_next = GetBasicBlock(bb->fall_through);
    DCHECK(!bb_next->catch_entry);
    DCHECK_EQ(Predecessors(bb_next), 1U);
    // Overwrite the kOpCheck insn with the paired opcode
    DCHECK_EQ(bb_next->first_mir_insn, throw_insn);
    *bb->last_mir_insn = *throw_insn;
    // Use the successor info from the next block
    bb->successor_block_list_type = bb_next->successor_block_list_type;
    bb->successor_blocks = bb_next->successor_blocks;
    // Use the ending block linkage from the next block
    bb->fall_through = bb_next->fall_through;
    GetBasicBlock(bb->taken)->block_type = kDead;  // Kill the unused exception block
    bb->taken = bb_next->taken;
    // Include the rest of the instructions
    bb->last_mir_insn = bb_next->last_mir_insn;
    /*
     * If lower-half of pair of blocks to combine contained a return, move the flag
     * to the newly combined block.
     */
    bb->terminated_by_return = bb_next->terminated_by_return;

    /*
     * NOTE: we aren't updating all dataflow info here.  Should either make sure this pass
     * happens after uses of i_dominated, dom_frontier or update the dataflow info here.
     */

    // Kill bb_next and remap now-dead id to parent
    bb_next->block_type = kDead;
    block_id_map_.Overwrite(bb_next->id, bb->id);

    // Now, loop back and see if we can keep going
  }
}

void MIRGraph::EliminateNullChecksAndInferTypesStart() {
  if ((cu_->disable_opt & (1 << kNullCheckElimination)) == 0) {
    if (kIsDebugBuild) {
      AllNodesIterator iter(this);
      for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
        CHECK(bb->data_flow_info == nullptr || bb->data_flow_info->ending_check_v == nullptr);
      }
    }

    DCHECK(temp_scoped_alloc_.get() == nullptr);
    temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
    temp_bit_vector_size_ = GetNumSSARegs();
    temp_bit_vector_ = new (temp_scoped_alloc_.get()) ArenaBitVector(
        temp_scoped_alloc_.get(), temp_bit_vector_size_, false, kBitMapTempSSARegisterV);
  }
}

/*
 * Eliminate unnecessary null checks for a basic block.   Also, while we're doing
 * an iterative walk go ahead and perform type and size inference.
 */
bool MIRGraph::EliminateNullChecksAndInferTypes(BasicBlock* bb) {
  if (bb->data_flow_info == NULL) return false;
  bool infer_changed = false;
  bool do_nce = ((cu_->disable_opt & (1 << kNullCheckElimination)) == 0);

  ArenaBitVector* ssa_regs_to_check = temp_bit_vector_;
  if (do_nce) {
    /*
     * Set initial state.  Be conservative with catch
     * blocks and start with no assumptions about null check
     * status (except for "this").
     */
    if ((bb->block_type == kEntryBlock) | bb->catch_entry) {
      ssa_regs_to_check->ClearAllBits();
      // Assume all ins are objects.
      for (uint16_t in_reg = cu_->num_dalvik_registers - cu_->num_ins;
           in_reg < cu_->num_dalvik_registers; in_reg++) {
        ssa_regs_to_check->SetBit(in_reg);
      }
      if ((cu_->access_flags & kAccStatic) == 0) {
        // If non-static method, mark "this" as non-null
        int this_reg = cu_->num_dalvik_registers - cu_->num_ins;
        ssa_regs_to_check->ClearBit(this_reg);
      }
    } else if (bb->predecessors->Size() == 1) {
      BasicBlock* pred_bb = GetBasicBlock(bb->predecessors->Get(0));
      // pred_bb must have already been processed at least once.
      DCHECK(pred_bb->data_flow_info->ending_check_v != nullptr);
      ssa_regs_to_check->Copy(pred_bb->data_flow_info->ending_check_v);
      if (pred_bb->block_type == kDalvikByteCode) {
        // Check to see if predecessor had an explicit null-check.
        MIR* last_insn = pred_bb->last_mir_insn;
        if (last_insn != nullptr) {
          Instruction::Code last_opcode = last_insn->dalvikInsn.opcode;
          if (last_opcode == Instruction::IF_EQZ) {
            if (pred_bb->fall_through == bb->id) {
              // The fall-through of a block following a IF_EQZ, set the vA of the IF_EQZ to show that
              // it can't be null.
              ssa_regs_to_check->ClearBit(last_insn->ssa_rep->uses[0]);
            }
          } else if (last_opcode == Instruction::IF_NEZ) {
            if (pred_bb->taken == bb->id) {
              // The taken block following a IF_NEZ, set the vA of the IF_NEZ to show that it can't be
              // null.
              ssa_regs_to_check->ClearBit(last_insn->ssa_rep->uses[0]);
            }
          }
        }
      }
    } else {
      // Starting state is union of all incoming arcs
      GrowableArray<BasicBlockId>::Iterator iter(bb->predecessors);
      BasicBlock* pred_bb = GetBasicBlock(iter.Next());
      CHECK(pred_bb != NULL);
      while (pred_bb->data_flow_info->ending_check_v == nullptr) {
        pred_bb = GetBasicBlock(iter.Next());
        // At least one predecessor must have been processed before this bb.
        DCHECK(pred_bb != nullptr);
        DCHECK(pred_bb->data_flow_info != nullptr);
      }
      ssa_regs_to_check->Copy(pred_bb->data_flow_info->ending_check_v);
      while (true) {
        pred_bb = GetBasicBlock(iter.Next());
        if (!pred_bb) break;
        DCHECK(pred_bb->data_flow_info != nullptr);
        if (pred_bb->data_flow_info->ending_check_v == nullptr) {
          continue;
        }
        ssa_regs_to_check->Union(pred_bb->data_flow_info->ending_check_v);
      }
    }
    // At this point, ssa_regs_to_check shows which sregs have an object definition with
    // no intervening uses.
  }

  // Walk through the instruction in the block, updating as necessary
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    if (mir->ssa_rep == NULL) {
        continue;
    }

    // Propagate type info.
    infer_changed = InferTypeAndSize(bb, mir, infer_changed);
    if (!do_nce) {
      continue;
    }

    uint64_t df_attributes = GetDataFlowAttributes(mir);

    // Might need a null check?
    if (df_attributes & DF_HAS_NULL_CHKS) {
      int src_idx;
      if (df_attributes & DF_NULL_CHK_1) {
        src_idx = 1;
      } else if (df_attributes & DF_NULL_CHK_2) {
        src_idx = 2;
      } else {
        src_idx = 0;
      }
      int src_sreg = mir->ssa_rep->uses[src_idx];
      if (!ssa_regs_to_check->IsBitSet(src_sreg)) {
        // Eliminate the null check.
        mir->optimization_flags |= MIR_IGNORE_NULL_CHECK;
      } else {
        // Do the null check.
        mir->optimization_flags &= ~MIR_IGNORE_NULL_CHECK;
        // Mark s_reg as null-checked
        ssa_regs_to_check->ClearBit(src_sreg);
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
    if (((df_attributes & (DF_DA | DF_REF_A)) == (DF_DA | DF_REF_A)) ||
        (df_attributes & DF_SETS_CONST))  {
      ssa_regs_to_check->SetBit(mir->ssa_rep->defs[0]);
    }

    // Now, remove mark from all object definitions we know are non-null.
    if (df_attributes & DF_NON_NULL_DST) {
      // Mark target of NEW* as non-null
      ssa_regs_to_check->ClearBit(mir->ssa_rep->defs[0]);
    }

    // Mark non-null returns from invoke-style NEW*
    if (df_attributes & DF_NON_NULL_RET) {
      MIR* next_mir = mir->next;
      // Next should be an MOVE_RESULT_OBJECT
      if (next_mir &&
          next_mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) {
        // Mark as null checked
        ssa_regs_to_check->ClearBit(next_mir->ssa_rep->defs[0]);
      } else {
        if (next_mir) {
          LOG(WARNING) << "Unexpected opcode following new: " << next_mir->dalvikInsn.opcode;
        } else if (bb->fall_through != NullBasicBlockId) {
          // Look in next basic block
          struct BasicBlock* next_bb = GetBasicBlock(bb->fall_through);
          for (MIR* tmir = next_bb->first_mir_insn; tmir != NULL;
            tmir =tmir->next) {
            if (static_cast<int>(tmir->dalvikInsn.opcode) >= static_cast<int>(kMirOpFirst)) {
              continue;
            }
            // First non-pseudo should be MOVE_RESULT_OBJECT
            if (tmir->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) {
              // Mark as null checked
              ssa_regs_to_check->ClearBit(tmir->ssa_rep->defs[0]);
            } else {
              LOG(WARNING) << "Unexpected op after new: " << tmir->dalvikInsn.opcode;
            }
            break;
          }
        }
      }
    }

    /*
     * Propagate nullcheck state on register copies (including
     * Phi pseudo copies.  For the latter, nullcheck state is
     * the "or" of all the Phi's operands.
     */
    if (df_attributes & (DF_NULL_TRANSFER_0 | DF_NULL_TRANSFER_N)) {
      int tgt_sreg = mir->ssa_rep->defs[0];
      int operands = (df_attributes & DF_NULL_TRANSFER_0) ? 1 :
          mir->ssa_rep->num_uses;
      bool needs_null_check = false;
      for (int i = 0; i < operands; i++) {
        needs_null_check |= ssa_regs_to_check->IsBitSet(mir->ssa_rep->uses[i]);
      }
      if (needs_null_check) {
        ssa_regs_to_check->SetBit(tgt_sreg);
      } else {
        ssa_regs_to_check->ClearBit(tgt_sreg);
      }
    }
  }

  // Did anything change?
  bool nce_changed = false;
  if (do_nce) {
    if (bb->data_flow_info->ending_check_v == nullptr) {
      DCHECK(temp_scoped_alloc_.get() != nullptr);
      bb->data_flow_info->ending_check_v = new (temp_scoped_alloc_.get()) ArenaBitVector(
          temp_scoped_alloc_.get(), temp_bit_vector_size_, false, kBitMapNullCheck);
      nce_changed = ssa_regs_to_check->GetHighestBitSet() != -1;
      bb->data_flow_info->ending_check_v->Copy(ssa_regs_to_check);
    } else if (!ssa_regs_to_check->SameBitsSet(bb->data_flow_info->ending_check_v)) {
      nce_changed = true;
      bb->data_flow_info->ending_check_v->Copy(ssa_regs_to_check);
    }
  }
  return infer_changed | nce_changed;
}

void MIRGraph::EliminateNullChecksAndInferTypesEnd() {
  if ((cu_->disable_opt & (1 << kNullCheckElimination)) == 0) {
    // Clean up temporaries.
    temp_bit_vector_size_ = 0u;
    temp_bit_vector_ = nullptr;
    AllNodesIterator iter(this);
    for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
      if (bb->data_flow_info != nullptr) {
        bb->data_flow_info->ending_check_v = nullptr;
      }
    }
    DCHECK(temp_scoped_alloc_.get() != nullptr);
    temp_scoped_alloc_.reset();
  }
}

bool MIRGraph::EliminateClassInitChecksGate() {
  if ((cu_->disable_opt & (1 << kClassInitCheckElimination)) != 0 ||
      !cu_->mir_graph->HasStaticFieldAccess()) {
    return false;
  }

  if (kIsDebugBuild) {
    AllNodesIterator iter(this);
    for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
      CHECK(bb->data_flow_info == nullptr || bb->data_flow_info->ending_check_v == nullptr);
    }
  }

  DCHECK(temp_scoped_alloc_.get() == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));

  // Each insn we use here has at least 2 code units, offset/2 will be a unique index.
  const size_t end = (cu_->code_item->insns_size_in_code_units_ + 1u) / 2u;
  temp_insn_data_ = static_cast<uint16_t*>(
      temp_scoped_alloc_->Alloc(end * sizeof(*temp_insn_data_), kArenaAllocGrowableArray));

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
      for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
        DCHECK(bb->data_flow_info != nullptr);
        if (mir->dalvikInsn.opcode >= Instruction::SGET &&
            mir->dalvikInsn.opcode <= Instruction::SPUT_SHORT) {
          const MirSFieldLoweringInfo& field_info = GetSFieldLoweringInfo(mir);
          uint16_t index = 0xffffu;
          if (field_info.IsResolved() && !field_info.IsInitialized()) {
            DCHECK_LT(class_to_index_map.size(), 0xffffu);
            MapEntry entry = {
                field_info.DeclaringDexFile(),
                field_info.DeclaringClassIndex(),
                static_cast<uint16_t>(class_to_index_map.size())
            };
            index = class_to_index_map.insert(entry).first->index;
          }
          // Using offset/2 for index into temp_insn_data_.
          temp_insn_data_[mir->offset / 2u] = index;
        }
      }
    }
    unique_class_count = static_cast<uint32_t>(class_to_index_map.size());
  }

  if (unique_class_count == 0u) {
    // All SGET/SPUTs refer to initialized classes. Nothing to do.
    temp_insn_data_ = nullptr;
    temp_scoped_alloc_.reset();
    return false;
  }

  temp_bit_vector_size_ = unique_class_count;
  temp_bit_vector_ = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), temp_bit_vector_size_, false, kBitMapClInitCheck);
  DCHECK_GT(temp_bit_vector_size_, 0u);
  return true;
}

/*
 * Eliminate unnecessary class initialization checks for a basic block.
 */
bool MIRGraph::EliminateClassInitChecks(BasicBlock* bb) {
  DCHECK_EQ((cu_->disable_opt & (1 << kClassInitCheckElimination)), 0u);
  if (bb->data_flow_info == NULL) {
    return false;
  }

  /*
   * Set initial state.  Be conservative with catch
   * blocks and start with no assumptions about class init check status.
   */
  ArenaBitVector* classes_to_check = temp_bit_vector_;
  DCHECK(classes_to_check != nullptr);
  if ((bb->block_type == kEntryBlock) | bb->catch_entry) {
    classes_to_check->SetInitialBits(temp_bit_vector_size_);
  } else if (bb->predecessors->Size() == 1) {
    BasicBlock* pred_bb = GetBasicBlock(bb->predecessors->Get(0));
    // pred_bb must have already been processed at least once.
    DCHECK(pred_bb != nullptr);
    DCHECK(pred_bb->data_flow_info != nullptr);
    DCHECK(pred_bb->data_flow_info->ending_check_v != nullptr);
    classes_to_check->Copy(pred_bb->data_flow_info->ending_check_v);
  } else {
    // Starting state is union of all incoming arcs
    GrowableArray<BasicBlockId>::Iterator iter(bb->predecessors);
    BasicBlock* pred_bb = GetBasicBlock(iter.Next());
    DCHECK(pred_bb != NULL);
    DCHECK(pred_bb->data_flow_info != NULL);
    while (pred_bb->data_flow_info->ending_check_v == nullptr) {
      pred_bb = GetBasicBlock(iter.Next());
      // At least one predecessor must have been processed before this bb.
      DCHECK(pred_bb != nullptr);
      DCHECK(pred_bb->data_flow_info != nullptr);
    }
    classes_to_check->Copy(pred_bb->data_flow_info->ending_check_v);
    while (true) {
      pred_bb = GetBasicBlock(iter.Next());
      if (!pred_bb) break;
      DCHECK(pred_bb->data_flow_info != nullptr);
      if (pred_bb->data_flow_info->ending_check_v == nullptr) {
        continue;
      }
      classes_to_check->Union(pred_bb->data_flow_info->ending_check_v);
    }
  }
  // At this point, classes_to_check shows which classes need clinit checks.

  // Walk through the instruction in the block, updating as necessary
  for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
    if (mir->dalvikInsn.opcode >= Instruction::SGET &&
        mir->dalvikInsn.opcode <= Instruction::SPUT_SHORT) {
      uint16_t index = temp_insn_data_[mir->offset / 2u];
      if (index != 0xffffu) {
        if (mir->dalvikInsn.opcode >= Instruction::SGET &&
            mir->dalvikInsn.opcode <= Instruction::SPUT_SHORT) {
          if (!classes_to_check->IsBitSet(index)) {
            // Eliminate the class init check.
            mir->optimization_flags |= MIR_IGNORE_CLINIT_CHECK;
          } else {
            // Do the class init check.
            mir->optimization_flags &= ~MIR_IGNORE_CLINIT_CHECK;
          }
        }
        // Mark the class as initialized.
        classes_to_check->ClearBit(index);
      }
    }
  }

  // Did anything change?
  bool changed = false;
  if (bb->data_flow_info->ending_check_v == nullptr) {
    DCHECK(temp_scoped_alloc_.get() != nullptr);
    DCHECK(bb->data_flow_info != nullptr);
    bb->data_flow_info->ending_check_v = new (temp_scoped_alloc_.get()) ArenaBitVector(
        temp_scoped_alloc_.get(), temp_bit_vector_size_, false, kBitMapClInitCheck);
    changed = classes_to_check->GetHighestBitSet() != -1;
    bb->data_flow_info->ending_check_v->Copy(classes_to_check);
  } else if (!classes_to_check->Equal(bb->data_flow_info->ending_check_v)) {
    changed = true;
    bb->data_flow_info->ending_check_v->Copy(classes_to_check);
  }
  return changed;
}

void MIRGraph::EliminateClassInitChecksEnd() {
  // Clean up temporaries.
  temp_bit_vector_size_ = 0u;
  temp_bit_vector_ = nullptr;
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    if (bb->data_flow_info != nullptr) {
      bb->data_flow_info->ending_check_v = nullptr;
    }
  }

  DCHECK(temp_insn_data_ != nullptr);
  temp_insn_data_ = nullptr;
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  temp_scoped_alloc_.reset();
}

void MIRGraph::ComputeInlineIFieldLoweringInfo(uint16_t field_idx, MIR* invoke, MIR* iget_or_iput) {
  uint32_t method_index = invoke->meta.method_lowering_info;
  if (temp_bit_vector_->IsBitSet(method_index)) {
    iget_or_iput->meta.ifield_lowering_info = temp_insn_data_[method_index];
    DCHECK_EQ(field_idx, GetIFieldLoweringInfo(iget_or_iput).FieldIndex());
    return;
  }

  const MirMethodLoweringInfo& method_info = GetMethodLoweringInfo(invoke);
  MethodReference target = method_info.GetTargetMethod();
  DexCompilationUnit inlined_unit(
      cu_, cu_->class_loader, cu_->class_linker, *target.dex_file,
      nullptr /* code_item not used */, 0u /* class_def_idx not used */, target.dex_method_index,
      0u /* access_flags not used */, nullptr /* verified_method not used */);
  MirIFieldLoweringInfo inlined_field_info(field_idx);
  MirIFieldLoweringInfo::Resolve(cu_->compiler_driver, &inlined_unit, &inlined_field_info, 1u);
  DCHECK(inlined_field_info.IsResolved());

  uint32_t field_info_index = ifield_lowering_infos_.Size();
  ifield_lowering_infos_.Insert(inlined_field_info);
  temp_bit_vector_->SetBit(method_index);
  temp_insn_data_[method_index] = field_info_index;
  iget_or_iput->meta.ifield_lowering_info = field_info_index;
}

bool MIRGraph::InlineCallsGate() {
  if ((cu_->disable_opt & (1 << kSuppressMethodInlining)) != 0 ||
      method_lowering_infos_.Size() == 0u) {
    return false;
  }
  if (cu_->compiler_driver->GetMethodInlinerMap() == nullptr) {
    // This isn't the Quick compiler.
    return false;
  }
  return true;
}

void MIRGraph::InlineCallsStart() {
  // Prepare for inlining getters/setters. Since we're inlining at most 1 IGET/IPUT from
  // each INVOKE, we can index the data by the MIR::meta::method_lowering_info index.

  DCHECK(temp_scoped_alloc_.get() == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
  temp_bit_vector_size_ = method_lowering_infos_.Size();
  temp_bit_vector_ = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), temp_bit_vector_size_, false, kBitMapMisc);
  temp_bit_vector_->ClearAllBits();
  temp_insn_data_ = static_cast<uint16_t*>(temp_scoped_alloc_->Alloc(
      temp_bit_vector_size_ * sizeof(*temp_insn_data_), kArenaAllocGrowableArray));
}

void MIRGraph::InlineCalls(BasicBlock* bb) {
  if (bb->block_type != kDalvikByteCode) {
    return;
  }
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    if (!(Instruction::FlagsOf(mir->dalvikInsn.opcode) & Instruction::kInvoke)) {
      continue;
    }
    const MirMethodLoweringInfo& method_info = GetMethodLoweringInfo(mir);
    if (!method_info.FastPath()) {
      continue;
    }
    InvokeType sharp_type = method_info.GetSharpType();
    if ((sharp_type != kDirect) &&
        (sharp_type != kStatic || method_info.NeedsClassInitialization())) {
      continue;
    }
    DCHECK(cu_->compiler_driver->GetMethodInlinerMap() != nullptr);
    MethodReference target = method_info.GetTargetMethod();
    if (cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(target.dex_file)
            ->GenInline(this, bb, mir, target.dex_method_index)) {
      if (cu_->verbose) {
        LOG(INFO) << "In \"" << PrettyMethod(cu_->method_idx, *cu_->dex_file)
            << "\" @0x" << std::hex << mir->offset
            << " inlined " << method_info.GetInvokeType() << " (" << sharp_type << ") call to \""
            << PrettyMethod(target.dex_method_index, *target.dex_file) << "\"";
      }
    }
  }
}

void MIRGraph::InlineCallsEnd() {
  DCHECK(temp_insn_data_ != nullptr);
  temp_insn_data_ = nullptr;
  DCHECK(temp_bit_vector_ != nullptr);
  temp_bit_vector_ = nullptr;
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  temp_scoped_alloc_.reset();
}

void MIRGraph::DumpCheckStats() {
  Checkstats* stats =
      static_cast<Checkstats*>(arena_->Alloc(sizeof(Checkstats), kArenaAllocDFInfo));
  checkstats_ = stats;
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
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

bool MIRGraph::BuildExtendedBBList(struct BasicBlock* bb) {
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
  while (bb != NULL) {
    bb->visited = true;
    terminated_by_return |= bb->terminated_by_return;
    do_local_value_numbering |= bb->use_lvn;
    bb = NextDominatedBlock(bb);
  }
  if (terminated_by_return || do_local_value_numbering) {
    // Do lvn for all blocks in this extended set.
    bb = start_bb;
    while (bb != NULL) {
      bb->use_lvn = do_local_value_numbering;
      bb->dominates_return = terminated_by_return;
      bb = NextDominatedBlock(bb);
    }
  }
  return false;  // Not iterative - return value will be ignored
}

void MIRGraph::BasicBlockOptimization() {
  if ((cu_->disable_opt & (1 << kSuppressExceptionEdges)) != 0) {
    ClearAllVisitedFlags();
    PreOrderDfsIterator iter2(this);
    for (BasicBlock* bb = iter2.Next(); bb != NULL; bb = iter2.Next()) {
      BuildExtendedBBList(bb);
    }
    // Perform extended basic block optimizations.
    for (unsigned int i = 0; i < extended_basic_blocks_.size(); i++) {
      BasicBlockOpt(GetBasicBlock(extended_basic_blocks_[i]));
    }
  } else {
    PreOrderDfsIterator iter(this);
    for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
      BasicBlockOpt(bb);
    }
  }
}

}  // namespace art
