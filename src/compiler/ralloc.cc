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
#include "dataflow.h"
#include "codegen/ralloc_util.h"

namespace art {

static bool SetFp(CompilationUnit* cu, int index, bool is_fp) {
  bool change = false;
  if (is_fp && !cu->reg_location[index].fp) {
    cu->reg_location[index].fp = true;
    cu->reg_location[index].defined = true;
    change = true;
  }
  return change;
}

static bool SetCore(CompilationUnit* cu, int index, bool is_core) {
  bool change = false;
  if (is_core && !cu->reg_location[index].defined) {
    cu->reg_location[index].core = true;
    cu->reg_location[index].defined = true;
    change = true;
  }
  return change;
}

static bool SetRef(CompilationUnit* cu, int index, bool is_ref) {
  bool change = false;
  if (is_ref && !cu->reg_location[index].defined) {
    cu->reg_location[index].ref = true;
    cu->reg_location[index].defined = true;
    change = true;
  }
  return change;
}

static bool SetWide(CompilationUnit* cu, int index, bool is_wide) {
  bool change = false;
  if (is_wide && !cu->reg_location[index].wide) {
    cu->reg_location[index].wide = true;
    change = true;
  }
  return change;
}

static bool SetHigh(CompilationUnit* cu, int index, bool is_high) {
  bool change = false;
  if (is_high && !cu->reg_location[index].high_word) {
    cu->reg_location[index].high_word = true;
    change = true;
  }
  return change;
}

static bool RemapNames(CompilationUnit* cu, BasicBlock* bb)
{
  if (bb->block_type != kDalvikByteCode && bb->block_type != kEntryBlock &&
      bb->block_type != kExitBlock)
    return false;

  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    SSARepresentation *ssa_rep = mir->ssa_rep;
    if (ssa_rep) {
      for (int i = 0; i < ssa_rep->num_uses; i++) {
        ssa_rep->uses[i] = cu->phi_alias_map[ssa_rep->uses[i]];
      }
      for (int i = 0; i < ssa_rep->num_defs; i++) {
        ssa_rep->defs[i] = cu->phi_alias_map[ssa_rep->defs[i]];
      }
    }
  }
  return false;
}

/*
 * Infer types and sizes.  We don't need to track change on sizes,
 * as it doesn't propagate.  We're guaranteed at least one pass through
 * the cfg.
 */
static bool InferTypeAndSize(CompilationUnit* cu, BasicBlock* bb)
{
  MIR *mir;
  bool changed = false;   // Did anything change?

  if (bb->data_flow_info == NULL) return false;
  if (bb->block_type != kDalvikByteCode && bb->block_type != kEntryBlock)
    return false;

  for (mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    SSARepresentation *ssa_rep = mir->ssa_rep;
    if (ssa_rep) {
      int attrs = oat_data_flow_attributes[mir->dalvikInsn.opcode];

      // Handle defs
      if (attrs & DF_DA) {
        if (attrs & DF_CORE_A) {
          changed |= SetCore(cu, ssa_rep->defs[0], true);
        }
        if (attrs & DF_REF_A) {
          changed |= SetRef(cu, ssa_rep->defs[0], true);
        }
        if (attrs & DF_A_WIDE) {
          cu->reg_location[ssa_rep->defs[0]].wide = true;
          cu->reg_location[ssa_rep->defs[1]].wide = true;
          cu->reg_location[ssa_rep->defs[1]].high_word = true;
          DCHECK_EQ(SRegToVReg(cu, ssa_rep->defs[0])+1,
          SRegToVReg(cu, ssa_rep->defs[1]));
        }
      }

      // Handles uses
      int next = 0;
      if (attrs & DF_UA) {
        if (attrs & DF_CORE_A) {
          changed |= SetCore(cu, ssa_rep->uses[next], true);
        }
        if (attrs & DF_REF_A) {
          changed |= SetRef(cu, ssa_rep->uses[next], true);
        }
        if (attrs & DF_A_WIDE) {
          cu->reg_location[ssa_rep->uses[next]].wide = true;
          cu->reg_location[ssa_rep->uses[next + 1]].wide = true;
          cu->reg_location[ssa_rep->uses[next + 1]].high_word = true;
          DCHECK_EQ(SRegToVReg(cu, ssa_rep->uses[next])+1,
          SRegToVReg(cu, ssa_rep->uses[next + 1]));
          next += 2;
        } else {
          next++;
        }
      }
      if (attrs & DF_UB) {
        if (attrs & DF_CORE_B) {
          changed |= SetCore(cu, ssa_rep->uses[next], true);
        }
        if (attrs & DF_REF_B) {
          changed |= SetRef(cu, ssa_rep->uses[next], true);
        }
        if (attrs & DF_B_WIDE) {
          cu->reg_location[ssa_rep->uses[next]].wide = true;
          cu->reg_location[ssa_rep->uses[next + 1]].wide = true;
          cu->reg_location[ssa_rep->uses[next + 1]].high_word = true;
          DCHECK_EQ(SRegToVReg(cu, ssa_rep->uses[next])+1,
                               SRegToVReg(cu, ssa_rep->uses[next + 1]));
          next += 2;
        } else {
          next++;
        }
      }
      if (attrs & DF_UC) {
        if (attrs & DF_CORE_C) {
          changed |= SetCore(cu, ssa_rep->uses[next], true);
        }
        if (attrs & DF_REF_C) {
          changed |= SetRef(cu, ssa_rep->uses[next], true);
        }
        if (attrs & DF_C_WIDE) {
          cu->reg_location[ssa_rep->uses[next]].wide = true;
          cu->reg_location[ssa_rep->uses[next + 1]].wide = true;
          cu->reg_location[ssa_rep->uses[next + 1]].high_word = true;
          DCHECK_EQ(SRegToVReg(cu, ssa_rep->uses[next])+1,
          SRegToVReg(cu, ssa_rep->uses[next + 1]));
        }
      }

      // Special-case return handling
      if ((mir->dalvikInsn.opcode == Instruction::RETURN) ||
          (mir->dalvikInsn.opcode == Instruction::RETURN_WIDE) ||
          (mir->dalvikInsn.opcode == Instruction::RETURN_OBJECT)) {
        switch(cu->shorty[0]) {
            case 'I':
              changed |= SetCore(cu, ssa_rep->uses[0], true);
              break;
            case 'J':
              changed |= SetCore(cu, ssa_rep->uses[0], true);
              changed |= SetCore(cu, ssa_rep->uses[1], true);
              cu->reg_location[ssa_rep->uses[0]].wide = true;
              cu->reg_location[ssa_rep->uses[1]].wide = true;
              cu->reg_location[ssa_rep->uses[1]].high_word = true;
              break;
            case 'F':
              changed |= SetFp(cu, ssa_rep->uses[0], true);
              break;
            case 'D':
              changed |= SetFp(cu, ssa_rep->uses[0], true);
              changed |= SetFp(cu, ssa_rep->uses[1], true);
              cu->reg_location[ssa_rep->uses[0]].wide = true;
              cu->reg_location[ssa_rep->uses[1]].wide = true;
              cu->reg_location[ssa_rep->uses[1]].high_word = true;
              break;
            case 'L':
              changed |= SetRef(cu, ssa_rep->uses[0], true);
              break;
            default: break;
        }
      }

      // Special-case handling for format 35c/3rc invokes
      Instruction::Code opcode = mir->dalvikInsn.opcode;
      int flags = (static_cast<int>(opcode) >= kNumPackedOpcodes)
          ? 0 : Instruction::FlagsOf(mir->dalvikInsn.opcode);
      if ((flags & Instruction::kInvoke) &&
          (attrs & (DF_FORMAT_35C | DF_FORMAT_3RC))) {
        DCHECK_EQ(next, 0);
        int target_idx = mir->dalvikInsn.vB;
        const char* shorty = GetShortyFromTargetIdx(cu, target_idx);
        // Handle result type if floating point
        if ((shorty[0] == 'F') || (shorty[0] == 'D')) {
          MIR* move_result_mir = FindMoveResult(cu, bb, mir);
          // Result might not be used at all, so no move-result
          if (move_result_mir && (move_result_mir->dalvikInsn.opcode !=
              Instruction::MOVE_RESULT_OBJECT)) {
            SSARepresentation* tgt_rep = move_result_mir->ssa_rep;
            DCHECK(tgt_rep != NULL);
            tgt_rep->fp_def[0] = true;
            changed |= SetFp(cu, tgt_rep->defs[0], true);
            if (shorty[0] == 'D') {
              tgt_rep->fp_def[1] = true;
              changed |= SetFp(cu, tgt_rep->defs[1], true);
            }
          }
        }
        int num_uses = mir->dalvikInsn.vA;
        // If this is a non-static invoke, mark implicit "this"
        if (((mir->dalvikInsn.opcode != Instruction::INVOKE_STATIC) &&
            (mir->dalvikInsn.opcode != Instruction::INVOKE_STATIC_RANGE))) {
          cu->reg_location[ssa_rep->uses[next]].defined = true;
          cu->reg_location[ssa_rep->uses[next]].ref = true;
          next++;
        }
        uint32_t cpos = 1;
        if (strlen(shorty) > 1) {
          for (int i = next; i < num_uses;) {
            DCHECK_LT(cpos, strlen(shorty));
            switch (shorty[cpos++]) {
              case 'D':
                ssa_rep->fp_use[i] = true;
                ssa_rep->fp_use[i+1] = true;
                cu->reg_location[ssa_rep->uses[i]].wide = true;
                cu->reg_location[ssa_rep->uses[i+1]].wide = true;
                cu->reg_location[ssa_rep->uses[i+1]].high_word = true;
                DCHECK_EQ(SRegToVReg(cu, ssa_rep->uses[i])+1,
                                     SRegToVReg(cu, ssa_rep->uses[i+1]));
                i++;
                break;
              case 'J':
                cu->reg_location[ssa_rep->uses[i]].wide = true;
                cu->reg_location[ssa_rep->uses[i+1]].wide = true;
                cu->reg_location[ssa_rep->uses[i+1]].high_word = true;
                DCHECK_EQ(SRegToVReg(cu, ssa_rep->uses[i])+1,
                                     SRegToVReg(cu, ssa_rep->uses[i+1]));
                changed |= SetCore(cu, ssa_rep->uses[i],true);
                i++;
                break;
              case 'F':
                ssa_rep->fp_use[i] = true;
                break;
              case 'L':
                changed |= SetRef(cu,ssa_rep->uses[i], true);
                break;
              default:
                changed |= SetCore(cu,ssa_rep->uses[i], true);
                break;
            }
            i++;
          }
        }
      }

      for (int i=0; ssa_rep->fp_use && i< ssa_rep->num_uses; i++) {
        if (ssa_rep->fp_use[i])
          changed |= SetFp(cu, ssa_rep->uses[i], true);
        }
      for (int i=0; ssa_rep->fp_def && i< ssa_rep->num_defs; i++) {
        if (ssa_rep->fp_def[i])
          changed |= SetFp(cu, ssa_rep->defs[i], true);
        }
      // Special-case handling for moves & Phi
      if (attrs & (DF_IS_MOVE | DF_NULL_TRANSFER_N)) {
        /*
         * If any of our inputs or outputs is defined, set all.
         * Some ugliness related to Phi nodes and wide values.
         * The Phi set will include all low words or all high
         * words, so we have to treat them specially.
         */
        bool is_phi = (static_cast<int>(mir->dalvikInsn.opcode) ==
                      kMirOpPhi);
        RegLocation rl_temp = cu->reg_location[ssa_rep->defs[0]];
        bool defined_fp = rl_temp.defined && rl_temp.fp;
        bool defined_core = rl_temp.defined && rl_temp.core;
        bool defined_ref = rl_temp.defined && rl_temp.ref;
        bool is_wide = rl_temp.wide || ((attrs & DF_A_WIDE) != 0);
        bool is_high = is_phi && rl_temp.wide && rl_temp.high_word;
        for (int i = 0; i < ssa_rep->num_uses;i++) {
          rl_temp = cu->reg_location[ssa_rep->uses[i]];
          defined_fp |= rl_temp.defined && rl_temp.fp;
          defined_core |= rl_temp.defined && rl_temp.core;
          defined_ref |= rl_temp.defined && rl_temp.ref;
          is_wide |= rl_temp.wide;
          is_high |= is_phi && rl_temp.wide && rl_temp.high_word;
        }
        /*
         * TODO: cleaner fix
         * We don't normally expect to see a Dalvik register
         * definition used both as a floating point and core
         * value.  However, the instruction rewriting that occurs
         * during verification can eliminate some type information,
         * leaving us confused.  The real fix here is either to
         * add explicit type information to Dalvik byte codes,
         * or to recognize THROW_VERIFICATION_ERROR as
         * an unconditional branch and support dead code elimination.
         * As a workaround we can detect this situation and
         * disable register promotion (which is the only thing that
         * relies on distinctions between core and fp usages.
         */
        if ((defined_fp && (defined_core | defined_ref)) &&
            ((cu->disable_opt & (1 << kPromoteRegs)) == 0)) {
          LOG(WARNING) << PrettyMethod(cu->method_idx, *cu->dex_file)
                       << " op at block " << bb->id
                       << " has both fp and core/ref uses for same def.";
          cu->disable_opt |= (1 << kPromoteRegs);
        }
        changed |= SetFp(cu, ssa_rep->defs[0], defined_fp);
        changed |= SetCore(cu, ssa_rep->defs[0], defined_core);
        changed |= SetRef(cu, ssa_rep->defs[0], defined_ref);
        changed |= SetWide(cu, ssa_rep->defs[0], is_wide);
        changed |= SetHigh(cu, ssa_rep->defs[0], is_high);
        if (attrs & DF_A_WIDE) {
          changed |= SetWide(cu, ssa_rep->defs[1], true);
          changed |= SetHigh(cu, ssa_rep->defs[1], true);
        }
        for (int i = 0; i < ssa_rep->num_uses; i++) {
          changed |= SetFp(cu, ssa_rep->uses[i], defined_fp);
          changed |= SetCore(cu, ssa_rep->uses[i], defined_core);
          changed |= SetRef(cu, ssa_rep->uses[i], defined_ref);
          changed |= SetWide(cu, ssa_rep->uses[i], is_wide);
          changed |= SetHigh(cu, ssa_rep->uses[i], is_high);
        }
        if (attrs & DF_A_WIDE) {
          DCHECK_EQ(ssa_rep->num_uses, 2);
          changed |= SetWide(cu, ssa_rep->uses[1], true);
          changed |= SetHigh(cu, ssa_rep->uses[1], true);
        }
      }
    }
  }
  return changed;
}

static const char* storage_name[] = {" Frame ", "PhysReg", " Spill "};

static void DumpRegLocTable(CompilationUnit* cu, RegLocation* table, int count)
{
  Codegen* cg = cu->cg.get();
  for (int i = 0; i < count; i++) {
    LOG(INFO) << StringPrintf("Loc[%02d] : %s, %c %c %c %c %c %c%d %c%d S%d",
        table[i].orig_sreg, storage_name[table[i].location],
        table[i].wide ? 'W' : 'N', table[i].defined ? 'D' : 'U',
        table[i].fp ? 'F' : table[i].ref ? 'R' :'C',
        table[i].high_word ? 'H' : 'L', table[i].home ? 'h' : 't',
        cg->IsFpReg(table[i].low_reg) ? 's' : 'r',
        table[i].low_reg & cg->FpRegMask(),
        cg->IsFpReg(table[i].high_reg) ? 's' : 'r',
        table[i].high_reg & cg->FpRegMask(), table[i].s_reg_low);
  }
}

static const RegLocation fresh_loc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0,
                                     INVALID_REG, INVALID_REG, INVALID_SREG,
                                     INVALID_SREG};

int ComputeFrameSize(CompilationUnit* cu) {
  /* Figure out the frame size */
  static const uint32_t kAlignMask = kStackAlignment - 1;
  uint32_t size = (cu->num_core_spills + cu->num_fp_spills +
                   1 /* filler word */ + cu->num_regs + cu->num_outs +
                   cu->num_compiler_temps + 1 /* cur_method* */)
                   * sizeof(uint32_t);
  /* Align and set */
  return (size + kAlignMask) & ~(kAlignMask);
}

/*
 * Simple register allocation.  Some Dalvik virtual registers may
 * be promoted to physical registers.  Most of the work for temp
 * allocation is done on the fly.  We also do some initialization and
 * type inference here.
 */
void SimpleRegAlloc(CompilationUnit* cu)
{
  int i;
  RegLocation* loc;

  /* Allocate the location map */
  loc = static_cast<RegLocation*>(NewMem(cu, cu->num_ssa_regs * sizeof(*loc),
                                  true, kAllocRegAlloc));
  for (i=0; i< cu->num_ssa_regs; i++) {
    loc[i] = fresh_loc;
    loc[i].s_reg_low = i;
    loc[i].is_const = IsBitSet(cu->is_constant_v, i);
  }

  /* Patch up the locations for Method* and the compiler temps */
  loc[cu->method_sreg].location = kLocCompilerTemp;
  loc[cu->method_sreg].defined = true;
  for (i = 0; i < cu->num_compiler_temps; i++) {
    CompilerTemp* ct = reinterpret_cast<CompilerTemp*>(cu->compiler_temps.elem_list[i]);
    loc[ct->s_reg].location = kLocCompilerTemp;
    loc[ct->s_reg].defined = true;
  }

  cu->reg_location = loc;

  /* Allocation the promotion map */
  int num_regs = cu->num_dalvik_registers;
  cu->promotion_map = static_cast<PromotionMap*>
      (NewMem(cu, (num_regs + cu->num_compiler_temps + 1) * sizeof(cu->promotion_map[0]),
              true, kAllocRegAlloc));

  /* Add types of incoming arguments based on signature */
  int num_ins = cu->num_ins;
  if (num_ins > 0) {
    int s_reg = num_regs - num_ins;
    if ((cu->access_flags & kAccStatic) == 0) {
      // For non-static, skip past "this"
      cu->reg_location[s_reg].defined = true;
      cu->reg_location[s_reg].ref = true;
      s_reg++;
    }
    const char* shorty = cu->shorty;
    int shorty_len = strlen(shorty);
    for (int i = 1; i < shorty_len; i++) {
      switch (shorty[i]) {
        case 'D':
          cu->reg_location[s_reg].wide = true;
          cu->reg_location[s_reg+1].high_word = true;
          cu->reg_location[s_reg+1].fp = true;
          DCHECK_EQ(SRegToVReg(cu, s_reg)+1, SRegToVReg(cu, s_reg+1));
          cu->reg_location[s_reg].fp = true;
          cu->reg_location[s_reg].defined = true;
          s_reg++;
          break;
        case 'J':
          cu->reg_location[s_reg].wide = true;
          cu->reg_location[s_reg+1].high_word = true;
          DCHECK_EQ(SRegToVReg(cu, s_reg)+1, SRegToVReg(cu, s_reg+1));
          cu->reg_location[s_reg].core = true;
          cu->reg_location[s_reg].defined = true;
          s_reg++;
          break;
        case 'F':
          cu->reg_location[s_reg].fp = true;
          cu->reg_location[s_reg].defined = true;
          break;
        case 'L':
          cu->reg_location[s_reg].ref = true;
          cu->reg_location[s_reg].defined = true;
          break;
        default:
          cu->reg_location[s_reg].core = true;
          cu->reg_location[s_reg].defined = true;
          break;
        }
        s_reg++;
      }
  }

  if (!cu->gen_bitcode) {
    /* Remap names */
    DataFlowAnalysisDispatcher(cu, RemapNames,
                                  kPreOrderDFSTraversal,
                                  false /* is_iterative */);
  }

  /* Do type & size inference pass */
  DataFlowAnalysisDispatcher(cu, InferTypeAndSize,
                                kPreOrderDFSTraversal,
                                true /* is_iterative */);

  /*
   * Set the s_reg_low field to refer to the pre-SSA name of the
   * base Dalvik virtual register.  Once we add a better register
   * allocator, remove this remapping.
   */
  for (i=0; i < cu->num_ssa_regs; i++) {
    if (cu->reg_location[i].location != kLocCompilerTemp) {
      int orig_sreg = cu->reg_location[i].s_reg_low;
      cu->reg_location[i].orig_sreg = orig_sreg;
      cu->reg_location[i].s_reg_low = SRegToVReg(cu, orig_sreg);
    }
  }

  cu->core_spill_mask = 0;
  cu->fp_spill_mask = 0;
  cu->num_core_spills = 0;

  DoPromotion(cu);

  /* Get easily-accessable post-promotion copy of RegLocation for Method* */
  cu->method_loc = cu->reg_location[cu->method_sreg];

  if (cu->verbose && !(cu->disable_opt & (1 << kPromoteRegs))) {
    LOG(INFO) << "After Promotion";
    DumpRegLocTable(cu, cu->reg_location, cu->num_ssa_regs);
  }

  /* Set the frame size */
  cu->frame_size = ComputeFrameSize(cu);
}

}  // namespace art
