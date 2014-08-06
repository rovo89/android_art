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
#include "dex/quick/mir_to_lir-inl.h"

namespace art {

#define DEBUG_OPT(X)

#define LOAD_STORE_CHECK_REG_DEP(mask, check) (mask.Intersects(*check->u.m.def_mask))

/* Check RAW, WAR, and RAW dependency on the register operands */
#define CHECK_REG_DEP(use, def, check) (def.Intersects(*check->u.m.use_mask)) || \
                                       (use.Union(def).Intersects(*check->u.m.def_mask))

/* Load Store Elimination filter:
 *  - Wide Load/Store
 *  - Exclusive Load/Store
 *  - Quad operand Load/Store
 *  - List Load/Store
 *  - IT blocks
 *  - Branch
 *  - Dmb
 */
#define LOAD_STORE_FILTER(flags) ((flags & (IS_QUAD_OP|IS_STORE)) == (IS_QUAD_OP|IS_STORE) || \
                                 (flags & (IS_QUAD_OP|IS_LOAD)) == (IS_QUAD_OP|IS_LOAD) || \
                                 (flags & REG_USE012) == REG_USE012 || \
                                 (flags & REG_DEF01) == REG_DEF01 || \
                                 (flags & REG_DEF_LIST0) || \
                                 (flags & REG_DEF_LIST1) || \
                                 (flags & REG_USE_LIST0) || \
                                 (flags & REG_USE_LIST1) || \
                                 (flags & REG_DEF_FPCS_LIST0) || \
                                 (flags & REG_DEF_FPCS_LIST2) || \
                                 (flags & REG_USE_FPCS_LIST0) || \
                                 (flags & REG_USE_FPCS_LIST2) || \
                                 (flags & IS_VOLATILE) || \
                                 (flags & IS_BRANCH) || \
                                 (flags & IS_IT))

/* Scheduler heuristics */
#define MAX_HOIST_DISTANCE 20
#define LDLD_DISTANCE 4
#define LD_LATENCY 2

static bool IsDalvikRegisterClobbered(LIR* lir1, LIR* lir2) {
  int reg1Lo = DECODE_ALIAS_INFO_REG(lir1->flags.alias_info);
  int reg1Hi = reg1Lo + DECODE_ALIAS_INFO_WIDE(lir1->flags.alias_info);
  int reg2Lo = DECODE_ALIAS_INFO_REG(lir2->flags.alias_info);
  int reg2Hi = reg2Lo + DECODE_ALIAS_INFO_WIDE(lir2->flags.alias_info);

  return (reg1Lo == reg2Lo) || (reg1Lo == reg2Hi) || (reg1Hi == reg2Lo);
}

/* Convert a more expensive instruction (ie load) into a move */
void Mir2Lir::ConvertMemOpIntoMove(LIR* orig_lir, RegStorage dest, RegStorage src) {
  /* Insert a move to replace the load */
  LIR* move_lir;
  move_lir = OpRegCopyNoInsert(dest, src);
  move_lir->dalvik_offset = orig_lir->dalvik_offset;
  /*
   * Insert the converted instruction after the original since the
   * optimization is scannng in the top-down order and the new instruction
   * will need to be re-checked (eg the new dest clobbers the src used in
   * this_lir).
   */
  InsertLIRAfter(orig_lir, move_lir);
}

void Mir2Lir::DumpDependentInsnPair(LIR* check_lir, LIR* this_lir, const char* type) {
  LOG(INFO) << type;
  LOG(INFO) << "Check LIR:";
  DumpLIRInsn(check_lir, 0);
  LOG(INFO) << "This LIR:";
  DumpLIRInsn(this_lir, 0);
}

inline void Mir2Lir::EliminateLoad(LIR* lir, int reg_id) {
  DCHECK(RegStorage::SameRegType(lir->operands[0], reg_id));
  RegStorage dest_reg, src_reg;

  /* Same Register - Nop */
  if (lir->operands[0] == reg_id) {
    NopLIR(lir);
    return;
  }

  /* different Regsister - Move + Nop */
  switch (reg_id & RegStorage::kShapeTypeMask) {
    case RegStorage::k32BitSolo | RegStorage::kCoreRegister:
      dest_reg = RegStorage::Solo32(lir->operands[0]);
      src_reg = RegStorage::Solo32(reg_id);
      break;
    case RegStorage::k64BitSolo | RegStorage::kCoreRegister:
      dest_reg = RegStorage::Solo64(lir->operands[0]);
      src_reg = RegStorage::Solo64(reg_id);
      break;
    case RegStorage::k32BitSolo | RegStorage::kFloatingPoint:
      dest_reg = RegStorage::FloatSolo32(lir->operands[0]);
      src_reg = RegStorage::FloatSolo32(reg_id);
      break;
    case RegStorage::k64BitSolo | RegStorage::kFloatingPoint:
      dest_reg = RegStorage::FloatSolo64(lir->operands[0]);
      src_reg = RegStorage::FloatSolo64(reg_id);
      break;
    default:
      LOG(INFO) << "Load Store: Unsuported register type!";
      return;
  }
  ConvertMemOpIntoMove(lir, dest_reg, src_reg);
  NopLIR(lir);
  return;
}

/*
 * Perform a pass of top-down walk, from the first to the last instruction in the
 * superblock, to eliminate redundant loads and stores.
 *
 * An earlier load can eliminate a later load iff
 *   1) They are must-aliases
 *   2) The native register is not clobbered in between
 *   3) The memory location is not written to in between
 *
 * An earlier store can eliminate a later load iff
 *   1) They are must-aliases
 *   2) The native register is not clobbered in between
 *   3) The memory location is not written to in between
 *
 * An earlier store can eliminate a later store iff
 *   1) They are must-aliases
 *   2) The memory location is not written to in between
 */
void Mir2Lir::ApplyLoadStoreElimination(LIR* head_lir, LIR* tail_lir) {
  LIR* this_lir, *check_lir;
  std::vector<int> alias_list;

  if (head_lir == tail_lir) {
    return;
  }

  for (this_lir = head_lir; this_lir != tail_lir; this_lir = NEXT_LIR(this_lir)) {
    if (this_lir->flags.is_nop || IsPseudoLirOp(this_lir->opcode)) {
      continue;
    }

    uint64_t target_flags = GetTargetInstFlags(this_lir->opcode);
    /* Target LIR - skip if instr is:
     *  - NOP
     *  - Branch
     *  - Load and store
     *  - Wide load
     *  - Wide store
     *  - Exclusive load/store
     */
    if (LOAD_STORE_FILTER(target_flags) ||
        ((target_flags & (IS_LOAD | IS_STORE)) == (IS_LOAD | IS_STORE)) ||
        !(target_flags & (IS_LOAD | IS_STORE))) {
      continue;
    }
    int native_reg_id = this_lir->operands[0];
    int dest_reg_id = this_lir->operands[1];
    bool is_this_lir_load = target_flags & IS_LOAD;
    ResourceMask this_mem_mask = kEncodeMem.Intersection(this_lir->u.m.use_mask->Union(
                                                        *this_lir->u.m.def_mask));

    /* Memory region */
    if (!this_mem_mask.Intersects(kEncodeLiteral.Union(kEncodeDalvikReg)) &&
      (!this_mem_mask.Intersects(kEncodeLiteral.Union(kEncodeHeapRef)))) {
      continue;
    }

    /* Does not redefine the address */
    if (this_lir->u.m.def_mask->Intersects(*this_lir->u.m.use_mask)) {
      continue;
    }

    ResourceMask stop_def_reg_mask = this_lir->u.m.def_mask->Without(kEncodeMem);
    ResourceMask stop_use_reg_mask = this_lir->u.m.use_mask->Without(kEncodeMem);

    /* The ARM backend can load/store PC */
    ResourceMask uses_pc = GetPCUseDefEncoding();
    if (uses_pc.Intersects(this_lir->u.m.use_mask->Union(*this_lir->u.m.def_mask))) {
      continue;
    }

    /* Initialize alias list */
    alias_list.clear();
    ResourceMask alias_reg_list_mask = kEncodeNone;
    if (!this_mem_mask.Intersects(kEncodeMem) && !this_mem_mask.Intersects(kEncodeLiteral)) {
      alias_list.push_back(dest_reg_id);
      SetupRegMask(&alias_reg_list_mask, dest_reg_id);
    }

    /* Scan through the BB for posible elimination candidates */
    for (check_lir = NEXT_LIR(this_lir); check_lir != tail_lir; check_lir = NEXT_LIR(check_lir)) {
      if (check_lir->flags.is_nop || IsPseudoLirOp(check_lir->opcode)) {
        continue;
      }

      if (uses_pc.Intersects(check_lir->u.m.use_mask->Union(*check_lir->u.m.def_mask))) {
        break;
      }

      ResourceMask check_mem_mask = kEncodeMem.Intersection(check_lir->u.m.use_mask->Union(
                                                          *check_lir->u.m.def_mask));
      ResourceMask alias_mem_mask = this_mem_mask.Intersection(check_mem_mask);
      uint64_t check_flags = GetTargetInstFlags(check_lir->opcode);
      bool stop_here = false;
      bool pass_over = false;

      /* Check LIR - skip if instr is:
       *  - Wide Load
       *  - Wide Store
       *  - Branch
       *  - Dmb
       *  - Exclusive load/store
       *  - IT blocks
       *  - Quad loads
       */
      if (LOAD_STORE_FILTER(check_flags)) {
        stop_here = true;
        /* Possible alias or result of earlier pass */
      } else if (check_flags & IS_MOVE) {
        for (auto &reg : alias_list) {
          if (RegStorage::RegNum(check_lir->operands[1]) == RegStorage::RegNum(reg)) {
            pass_over = true;
            alias_list.push_back(check_lir->operands[0]);
            SetupRegMask(&alias_reg_list_mask, check_lir->operands[0]);
          }
        }
      /* Memory regions */
      } else if (!alias_mem_mask.Equals(kEncodeNone)) {
        DCHECK((check_flags & IS_LOAD) || (check_flags & IS_STORE));
        bool is_check_lir_load = check_flags & IS_LOAD;
        bool reg_compatible = RegStorage::SameRegType(check_lir->operands[0], native_reg_id);

        if (!alias_mem_mask.Intersects(kEncodeMem) && alias_mem_mask.Equals(kEncodeLiteral)) {
          DCHECK(check_flags & IS_LOAD);
          /* Same value && same register type */
          if (reg_compatible && (this_lir->target == check_lir->target)) {
            DEBUG_OPT(DumpDependentInsnPair(check_lir, this_lir, "LITERAL"));
            EliminateLoad(check_lir, native_reg_id);
          }
        } else if (((alias_mem_mask.Equals(kEncodeDalvikReg)) || (alias_mem_mask.Equals(kEncodeHeapRef))) &&
                   alias_reg_list_mask.Intersects((check_lir->u.m.use_mask)->Without(kEncodeMem))) {
          bool same_offset = (GetInstructionOffset(this_lir) == GetInstructionOffset(check_lir));
          if (same_offset && !is_check_lir_load) {
            if (check_lir->operands[0] != native_reg_id) {
              DEBUG_OPT(DumpDependentInsnPair(check_lir, this_lir, "STORE STOP"));
              stop_here = true;
              break;
            }
          }

          if (reg_compatible && same_offset &&
              ((is_this_lir_load && is_check_lir_load)  /* LDR - LDR */ ||
              (!is_this_lir_load && is_check_lir_load)  /* STR - LDR */ ||
              (!is_this_lir_load && !is_check_lir_load) /* STR - STR */)) {
            DEBUG_OPT(DumpDependentInsnPair(check_lir, this_lir, "LOAD STORE"));
            EliminateLoad(check_lir, native_reg_id);
          }
        } else {
          /* Unsupported memory region */
        }
      }

      if (pass_over) {
        continue;
      }

      if (stop_here == false) {
        bool stop_alias = LOAD_STORE_CHECK_REG_DEP(alias_reg_list_mask, check_lir);
        if (stop_alias) {
          /* Scan through alias list and if alias remove from alias list. */
          for (auto &reg : alias_list) {
            stop_alias = false;
            ResourceMask alias_reg_mask = kEncodeNone;
            SetupRegMask(&alias_reg_mask, reg);
            stop_alias = LOAD_STORE_CHECK_REG_DEP(alias_reg_mask, check_lir);
            if (stop_alias) {
              ClearRegMask(&alias_reg_list_mask, reg);
              alias_list.erase(std::remove(alias_list.begin(), alias_list.end(),
                                           reg), alias_list.end());
            }
          }
        }
        ResourceMask stop_search_mask = stop_def_reg_mask.Union(stop_use_reg_mask);
        stop_search_mask = stop_search_mask.Union(alias_reg_list_mask);
        stop_here = LOAD_STORE_CHECK_REG_DEP(stop_search_mask, check_lir);
        if (stop_here) {
          break;
        }
      } else {
        break;
      }
    }
  }
}

/*
 * Perform a pass of bottom-up walk, from the second instruction in the
 * superblock, to try to hoist loads to earlier slots.
 */
void Mir2Lir::ApplyLoadHoisting(LIR* head_lir, LIR* tail_lir) {
  LIR* this_lir, *check_lir;
  /*
   * Store the list of independent instructions that can be hoisted past.
   * Will decide the best place to insert later.
   */
  LIR* prev_inst_list[MAX_HOIST_DISTANCE];

  /* Empty block */
  if (head_lir == tail_lir) {
    return;
  }

  /* Start from the second instruction */
  for (this_lir = NEXT_LIR(head_lir); this_lir != tail_lir; this_lir = NEXT_LIR(this_lir)) {
    if (IsPseudoLirOp(this_lir->opcode)) {
      continue;
    }

    uint64_t target_flags = GetTargetInstFlags(this_lir->opcode);
    /* Skip non-interesting instructions */
    if (!(target_flags & IS_LOAD) ||
        (this_lir->flags.is_nop == true) ||
        ((target_flags & (REG_DEF0 | REG_DEF1)) == (REG_DEF0 | REG_DEF1)) ||
        ((target_flags & (IS_STORE | IS_LOAD)) == (IS_STORE | IS_LOAD))) {
      continue;
    }

    ResourceMask stop_use_all_mask = *this_lir->u.m.use_mask;

    /*
     * Branches for null/range checks are marked with the true resource
     * bits, and loads to Dalvik registers, constant pools, and non-alias
     * locations are safe to be hoisted. So only mark the heap references
     * conservatively here.
     *
     * Note: on x86(-64) and Arm64 this will add kEncodeNone.
     * TODO: Sanity check. LoadStoreElimination uses kBranchBit to fake a PC.
     */
    if (stop_use_all_mask.HasBit(ResourceMask::kHeapRef)) {
      stop_use_all_mask.SetBits(GetPCUseDefEncoding());
    }

    /* Similar as above, but just check for pure register dependency */
    ResourceMask stop_use_reg_mask = stop_use_all_mask.Without(kEncodeMem);
    ResourceMask stop_def_reg_mask = this_lir->u.m.def_mask->Without(kEncodeMem);

    int next_slot = 0;
    bool stop_here = false;

    /* Try to hoist the load to a good spot */
    for (check_lir = PREV_LIR(this_lir); check_lir != head_lir; check_lir = PREV_LIR(check_lir)) {
      /*
       * Skip already dead instructions (whose dataflow information is
       * outdated and misleading).
       */
      if (check_lir->flags.is_nop) {
        continue;
      }

      ResourceMask check_mem_mask = check_lir->u.m.def_mask->Intersection(kEncodeMem);
      ResourceMask alias_condition = stop_use_all_mask.Intersection(check_mem_mask);
      stop_here = false;

      /* Potential WAR alias seen - check the exact relation */
      if (!check_mem_mask.Equals(kEncodeMem) && !alias_condition.Equals(kEncodeNone)) {
        /* We can fully disambiguate Dalvik references */
        if (alias_condition.Equals(kEncodeDalvikReg)) {
          /* Must alias or partially overlap */
          if ((check_lir->flags.alias_info == this_lir->flags.alias_info) ||
            IsDalvikRegisterClobbered(this_lir, check_lir)) {
            stop_here = true;
          }
        /* Conservatively treat all heap refs as may-alias */
        } else {
          DCHECK(alias_condition.Equals(kEncodeHeapRef));
          stop_here = true;
        }
        /* Memory content may be updated. Stop looking now. */
        if (stop_here) {
          prev_inst_list[next_slot++] = check_lir;
          break;
        }
      }

      if (stop_here == false) {
        stop_here = CHECK_REG_DEP(stop_use_reg_mask, stop_def_reg_mask,
                     check_lir);
      }

      /*
       * Store the dependent or non-pseudo/indepedent instruction to the
       * list.
       */
      if (stop_here || !IsPseudoLirOp(check_lir->opcode)) {
        prev_inst_list[next_slot++] = check_lir;
        if (next_slot == MAX_HOIST_DISTANCE) {
          break;
        }
      }

      /* Found a new place to put the load - move it here */
      if (stop_here == true) {
        DEBUG_OPT(DumpDependentInsnPair(check_lir, this_lir, "HOIST STOP"));
        break;
      }
    }

    /*
     * Reached the top - use head_lir as the dependent marker as all labels
     * are barriers.
     */
    if (stop_here == false && next_slot < MAX_HOIST_DISTANCE) {
      prev_inst_list[next_slot++] = head_lir;
    }

    /*
     * At least one independent instruction is found. Scan in the reversed
     * direction to find a beneficial slot.
     */
    if (next_slot >= 2) {
      int first_slot = next_slot - 2;
      int slot;
      LIR* dep_lir = prev_inst_list[next_slot-1];
      /* If there is ld-ld dependency, wait LDLD_DISTANCE cycles */
      if (!IsPseudoLirOp(dep_lir->opcode) &&
        (GetTargetInstFlags(dep_lir->opcode) & IS_LOAD)) {
        first_slot -= LDLD_DISTANCE;
      }
      /*
       * Make sure we check slot >= 0 since first_slot may be negative
       * when the loop is first entered.
       */
      for (slot = first_slot; slot >= 0; slot--) {
        LIR* cur_lir = prev_inst_list[slot];
        LIR* prev_lir = prev_inst_list[slot+1];

        /* Check the highest instruction */
        if (prev_lir->u.m.def_mask->Equals(kEncodeAll)) {
          /*
           * If the first instruction is a load, don't hoist anything
           * above it since it is unlikely to be beneficial.
           */
          if (GetTargetInstFlags(cur_lir->opcode) & IS_LOAD) {
            continue;
          }
          /*
           * If the remaining number of slots is less than LD_LATENCY,
           * insert the hoisted load here.
           */
          if (slot < LD_LATENCY) {
            break;
          }
        }

        // Don't look across a barrier label
        if ((prev_lir->opcode == kPseudoTargetLabel) ||
            (prev_lir->opcode == kPseudoSafepointPC) ||
            (prev_lir->opcode == kPseudoBarrier)) {
          break;
        }

        /*
         * Try to find two instructions with load/use dependency until
         * the remaining instructions are less than LD_LATENCY.
         */
        bool prev_is_load = IsPseudoLirOp(prev_lir->opcode) ? false :
            (GetTargetInstFlags(prev_lir->opcode) & IS_LOAD);
        if ((prev_is_load && (cur_lir->u.m.use_mask->Intersects(*prev_lir->u.m.def_mask))) ||
            (slot < LD_LATENCY)) {
          break;
        }
      }

      /* Found a slot to hoist to */
      if (slot >= 0) {
        LIR* cur_lir = prev_inst_list[slot];
        LIR* new_load_lir =
          static_cast<LIR*>(arena_->Alloc(sizeof(LIR), kArenaAllocLIR));
        *new_load_lir = *this_lir;
        /*
         * Insertion is guaranteed to succeed since check_lir
         * is never the first LIR on the list
         */
        InsertLIRBefore(cur_lir, new_load_lir);
        NopLIR(this_lir);
      }
    }
  }
}

void Mir2Lir::ApplyLocalOptimizations(LIR* head_lir, LIR* tail_lir) {
  if (!(cu_->disable_opt & (1 << kLoadStoreElimination))) {
    ApplyLoadStoreElimination(head_lir, tail_lir);
  }
  if (!(cu_->disable_opt & (1 << kLoadHoisting))) {
    ApplyLoadHoisting(head_lir, tail_lir);
  }
}

}  // namespace art
