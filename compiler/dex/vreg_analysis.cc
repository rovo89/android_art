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
#include "dex/dataflow_iterator-inl.h"

namespace art {

bool MIRGraph::SetFp(int index, bool is_fp) {
  bool change = false;
  if (is_fp && !reg_location_[index].fp) {
    reg_location_[index].fp = true;
    reg_location_[index].defined = true;
    change = true;
  }
  return change;
}

bool MIRGraph::SetFp(int index) {
  bool change = false;
  if (!reg_location_[index].fp) {
    reg_location_[index].fp = true;
    reg_location_[index].defined = true;
    change = true;
  }
  return change;
}

bool MIRGraph::SetCore(int index, bool is_core) {
  bool change = false;
  if (is_core && !reg_location_[index].defined) {
    reg_location_[index].core = true;
    reg_location_[index].defined = true;
    change = true;
  }
  return change;
}

bool MIRGraph::SetCore(int index) {
  bool change = false;
  if (!reg_location_[index].defined) {
    reg_location_[index].core = true;
    reg_location_[index].defined = true;
    change = true;
  }
  return change;
}

bool MIRGraph::SetRef(int index, bool is_ref) {
  bool change = false;
  if (is_ref && !reg_location_[index].defined) {
    reg_location_[index].ref = true;
    reg_location_[index].defined = true;
    change = true;
  }
  return change;
}

bool MIRGraph::SetRef(int index) {
  bool change = false;
  if (!reg_location_[index].defined) {
    reg_location_[index].ref = true;
    reg_location_[index].defined = true;
    change = true;
  }
  return change;
}

bool MIRGraph::SetWide(int index, bool is_wide) {
  bool change = false;
  if (is_wide && !reg_location_[index].wide) {
    reg_location_[index].wide = true;
    change = true;
  }
  return change;
}

bool MIRGraph::SetWide(int index) {
  bool change = false;
  if (!reg_location_[index].wide) {
    reg_location_[index].wide = true;
    change = true;
  }
  return change;
}

bool MIRGraph::SetHigh(int index, bool is_high) {
  bool change = false;
  if (is_high && !reg_location_[index].high_word) {
    reg_location_[index].high_word = true;
    change = true;
  }
  return change;
}

bool MIRGraph::SetHigh(int index) {
  bool change = false;
  if (!reg_location_[index].high_word) {
    reg_location_[index].high_word = true;
    change = true;
  }
  return change;
}


/*
 * Infer types and sizes.  We don't need to track change on sizes,
 * as it doesn't propagate.  We're guaranteed at least one pass through
 * the cfg.
 */
bool MIRGraph::InferTypeAndSize(BasicBlock* bb, MIR* mir, bool changed) {
  SSARepresentation *ssa_rep = mir->ssa_rep;

  /*
   * The dex bytecode definition does not explicitly outlaw the definition of the same
   * virtual register to be used in both a 32-bit and 64-bit pair context.  However, dx
   * does not generate this pattern (at least recently).  Further, in the next revision of
   * dex, we will forbid this.  To support the few cases in the wild, detect this pattern
   * and punt to the interpreter.
   */
  bool type_mismatch = false;

  if (ssa_rep) {
    uint64_t attrs = GetDataFlowAttributes(mir);
    const int* uses = ssa_rep->uses;
    const int* defs = ssa_rep->defs;

    // Handle defs
    if (attrs & DF_DA) {
      if (attrs & DF_CORE_A) {
        changed |= SetCore(defs[0]);
      }
      if (attrs & DF_REF_A) {
        changed |= SetRef(defs[0]);
      }
      if (attrs & DF_A_WIDE) {
        reg_location_[defs[0]].wide = true;
        reg_location_[defs[1]].wide = true;
        reg_location_[defs[1]].high_word = true;
        DCHECK_EQ(SRegToVReg(defs[0])+1,
        SRegToVReg(defs[1]));
      }
    }


    // Handles uses
    int next = 0;
    if (attrs & DF_UA) {
      if (attrs & DF_CORE_A) {
        changed |= SetCore(uses[next]);
      }
      if (attrs & DF_REF_A) {
        changed |= SetRef(uses[next]);
      }
      if (attrs & DF_A_WIDE) {
        reg_location_[uses[next]].wide = true;
        reg_location_[uses[next + 1]].wide = true;
        reg_location_[uses[next + 1]].high_word = true;
        DCHECK_EQ(SRegToVReg(uses[next])+1,
        SRegToVReg(uses[next + 1]));
        next += 2;
      } else {
        type_mismatch |= reg_location_[uses[next]].wide;
        next++;
      }
    }
    if (attrs & DF_UB) {
      if (attrs & DF_CORE_B) {
        changed |= SetCore(uses[next]);
      }
      if (attrs & DF_REF_B) {
        changed |= SetRef(uses[next]);
      }
      if (attrs & DF_B_WIDE) {
        reg_location_[uses[next]].wide = true;
        reg_location_[uses[next + 1]].wide = true;
        reg_location_[uses[next + 1]].high_word = true;
        DCHECK_EQ(SRegToVReg(uses[next])+1,
                             SRegToVReg(uses[next + 1]));
        next += 2;
      } else {
        type_mismatch |= reg_location_[uses[next]].wide;
        next++;
      }
    }
    if (attrs & DF_UC) {
      if (attrs & DF_CORE_C) {
        changed |= SetCore(uses[next]);
      }
      if (attrs & DF_REF_C) {
        changed |= SetRef(uses[next]);
      }
      if (attrs & DF_C_WIDE) {
        reg_location_[uses[next]].wide = true;
        reg_location_[uses[next + 1]].wide = true;
        reg_location_[uses[next + 1]].high_word = true;
        DCHECK_EQ(SRegToVReg(uses[next])+1,
        SRegToVReg(uses[next + 1]));
      } else {
        type_mismatch |= reg_location_[uses[next]].wide;
      }
    }

    // Special-case return handling
    if ((mir->dalvikInsn.opcode == Instruction::RETURN) ||
        (mir->dalvikInsn.opcode == Instruction::RETURN_WIDE) ||
        (mir->dalvikInsn.opcode == Instruction::RETURN_OBJECT)) {
      switch (cu_->shorty[0]) {
          case 'I':
            type_mismatch |= reg_location_[uses[0]].wide;
            changed |= SetCore(uses[0]);
            break;
          case 'J':
            changed |= SetCore(uses[0]);
            changed |= SetCore(uses[1]);
            reg_location_[uses[0]].wide = true;
            reg_location_[uses[1]].wide = true;
            reg_location_[uses[1]].high_word = true;
            break;
          case 'F':
            type_mismatch |= reg_location_[uses[0]].wide;
            changed |= SetFp(uses[0]);
            break;
          case 'D':
            changed |= SetFp(uses[0]);
            changed |= SetFp(uses[1]);
            reg_location_[uses[0]].wide = true;
            reg_location_[uses[1]].wide = true;
            reg_location_[uses[1]].high_word = true;
            break;
          case 'L':
            type_mismatch |= reg_location_[uses[0]].wide;
            changed |= SetRef(uses[0]);
            break;
          default: break;
      }
    }

    // Special-case handling for format 35c/3rc invokes
    Instruction::Code opcode = mir->dalvikInsn.opcode;
    int flags = MIR::DecodedInstruction::IsPseudoMirOp(opcode) ?
                  0 : Instruction::FlagsOf(mir->dalvikInsn.opcode);
    if ((flags & Instruction::kInvoke) &&
        (attrs & (DF_FORMAT_35C | DF_FORMAT_3RC))) {
      DCHECK_EQ(next, 0);
      int target_idx = mir->dalvikInsn.vB;
      const char* shorty = GetShortyFromTargetIdx(target_idx);
      // Handle result type if floating point
      if ((shorty[0] == 'F') || (shorty[0] == 'D')) {
        MIR* move_result_mir = FindMoveResult(bb, mir);
        // Result might not be used at all, so no move-result
        if (move_result_mir && (move_result_mir->dalvikInsn.opcode !=
            Instruction::MOVE_RESULT_OBJECT)) {
          SSARepresentation* tgt_rep = move_result_mir->ssa_rep;
          DCHECK(tgt_rep != NULL);
          tgt_rep->fp_def[0] = true;
          changed |= SetFp(tgt_rep->defs[0]);
          if (shorty[0] == 'D') {
            tgt_rep->fp_def[1] = true;
            changed |= SetFp(tgt_rep->defs[1]);
          }
        }
      }
      int num_uses = mir->dalvikInsn.vA;
      // If this is a non-static invoke, mark implicit "this"
      if (((mir->dalvikInsn.opcode != Instruction::INVOKE_STATIC) &&
          (mir->dalvikInsn.opcode != Instruction::INVOKE_STATIC_RANGE))) {
        reg_location_[uses[next]].defined = true;
        reg_location_[uses[next]].ref = true;
        type_mismatch |= reg_location_[uses[next]].wide;
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
              reg_location_[uses[i]].wide = true;
              reg_location_[uses[i+1]].wide = true;
              reg_location_[uses[i+1]].high_word = true;
              DCHECK_EQ(SRegToVReg(uses[i])+1, SRegToVReg(uses[i+1]));
              i++;
              break;
            case 'J':
              reg_location_[uses[i]].wide = true;
              reg_location_[uses[i+1]].wide = true;
              reg_location_[uses[i+1]].high_word = true;
              DCHECK_EQ(SRegToVReg(uses[i])+1, SRegToVReg(uses[i+1]));
              changed |= SetCore(uses[i]);
              i++;
              break;
            case 'F':
              type_mismatch |= reg_location_[uses[i]].wide;
              ssa_rep->fp_use[i] = true;
              break;
            case 'L':
              type_mismatch |= reg_location_[uses[i]].wide;
              changed |= SetRef(uses[i]);
              break;
            default:
              type_mismatch |= reg_location_[uses[i]].wide;
              changed |= SetCore(uses[i]);
              break;
          }
          i++;
        }
      }
    }

    for (int i = 0; ssa_rep->fp_use && i< ssa_rep->num_uses; i++) {
      if (ssa_rep->fp_use[i]) {
        changed |= SetFp(uses[i]);
      }
    }
    for (int i = 0; ssa_rep->fp_def && i< ssa_rep->num_defs; i++) {
      if (ssa_rep->fp_def[i]) {
        changed |= SetFp(defs[i]);
      }
    }
    // Special-case handling for moves & Phi
    if (attrs & (DF_IS_MOVE | DF_NULL_TRANSFER_N)) {
      /*
       * If any of our inputs or outputs is defined, set all.
       * Some ugliness related to Phi nodes and wide values.
       * The Phi set will include all low words or all high
       * words, so we have to treat them specially.
       */
      bool is_phi = (static_cast<int>(mir->dalvikInsn.opcode) == kMirOpPhi);
      RegLocation rl_temp = reg_location_[defs[0]];
      bool defined_fp = rl_temp.defined && rl_temp.fp;
      bool defined_core = rl_temp.defined && rl_temp.core;
      bool defined_ref = rl_temp.defined && rl_temp.ref;
      bool is_wide = rl_temp.wide || ((attrs & DF_A_WIDE) != 0);
      bool is_high = is_phi && rl_temp.wide && rl_temp.high_word;
      for (int i = 0; i < ssa_rep->num_uses; i++) {
        rl_temp = reg_location_[uses[i]];
        defined_fp |= rl_temp.defined && rl_temp.fp;
        defined_core |= rl_temp.defined && rl_temp.core;
        defined_ref |= rl_temp.defined && rl_temp.ref;
        is_wide |= rl_temp.wide;
        is_high |= is_phi && rl_temp.wide && rl_temp.high_word;
      }
      /*
       * We don't normally expect to see a Dalvik register definition used both as a
       * floating point and core value, though technically it could happen with constants.
       * Until we have proper typing, detect this situation and disable register promotion
       * (which relies on the distinction between core a fp usages).
       */
      if ((defined_fp && (defined_core | defined_ref)) &&
          ((cu_->disable_opt & (1 << kPromoteRegs)) == 0)) {
        LOG(WARNING) << PrettyMethod(cu_->method_idx, *cu_->dex_file)
                     << " op at block " << bb->id
                     << " has both fp and core/ref uses for same def.";
        cu_->disable_opt |= (1 << kPromoteRegs);
      }
      changed |= SetFp(defs[0], defined_fp);
      changed |= SetCore(defs[0], defined_core);
      changed |= SetRef(defs[0], defined_ref);
      changed |= SetWide(defs[0], is_wide);
      changed |= SetHigh(defs[0], is_high);
      if (attrs & DF_A_WIDE) {
        changed |= SetWide(defs[1]);
        changed |= SetHigh(defs[1]);
      }
      for (int i = 0; i < ssa_rep->num_uses; i++) {
        changed |= SetFp(uses[i], defined_fp);
        changed |= SetCore(uses[i], defined_core);
        changed |= SetRef(uses[i], defined_ref);
        changed |= SetWide(uses[i], is_wide);
        changed |= SetHigh(uses[i], is_high);
      }
      if (attrs & DF_A_WIDE) {
        DCHECK_EQ(ssa_rep->num_uses, 2);
        changed |= SetWide(uses[1]);
        changed |= SetHigh(uses[1]);
      }
    }
  }
  if (type_mismatch) {
    LOG(WARNING) << "Deprecated dex type mismatch, interpreting "
                 << PrettyMethod(cu_->method_idx, *cu_->dex_file);
    LOG(INFO) << "@ 0x" << std::hex << mir->offset;
    SetPuntToInterpreter(true);
  }
  return changed;
}

static const char* storage_name[] = {" Frame ", "PhysReg", " Spill "};

void MIRGraph::DumpRegLocTable(RegLocation* table, int count) {
  // FIXME: Quick-specific.  Move to Quick (and make a generic version for MIRGraph?
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu_->cg.get());
  if (cg != NULL) {
    for (int i = 0; i < count; i++) {
      LOG(INFO) << StringPrintf("Loc[%02d] : %s, %c %c %c %c %c %c 0x%04x S%d",
          table[i].orig_sreg, storage_name[table[i].location],
          table[i].wide ? 'W' : 'N', table[i].defined ? 'D' : 'U',
          table[i].fp ? 'F' : table[i].ref ? 'R' :'C',
          table[i].is_const ? 'c' : 'n',
          table[i].high_word ? 'H' : 'L', table[i].home ? 'h' : 't',
          table[i].reg.GetRawBits(),
          table[i].s_reg_low);
    }
  } else {
    // Either pre-regalloc or Portable.
    for (int i = 0; i < count; i++) {
      LOG(INFO) << StringPrintf("Loc[%02d] : %s, %c %c %c %c %c %c S%d",
          table[i].orig_sreg, storage_name[table[i].location],
          table[i].wide ? 'W' : 'N', table[i].defined ? 'D' : 'U',
          table[i].fp ? 'F' : table[i].ref ? 'R' :'C',
          table[i].is_const ? 'c' : 'n',
          table[i].high_word ? 'H' : 'L', table[i].home ? 'h' : 't',
          table[i].s_reg_low);
    }
  }
}

// FIXME - will likely need to revisit all uses of this.
static const RegLocation fresh_loc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0,
                                      RegStorage(), INVALID_SREG, INVALID_SREG};

void MIRGraph::InitRegLocations() {
  /* Allocate the location map */
  int max_regs = GetNumSSARegs() + GetMaxPossibleCompilerTemps();
  RegLocation* loc = static_cast<RegLocation*>(arena_->Alloc(max_regs * sizeof(*loc),
                                                             kArenaAllocRegAlloc));
  for (int i = 0; i < GetNumSSARegs(); i++) {
    loc[i] = fresh_loc;
    loc[i].s_reg_low = i;
    loc[i].is_const = is_constant_v_->IsBitSet(i);
    loc[i].wide = false;
  }

  /* Patch up the locations for the compiler temps */
  GrowableArray<CompilerTemp*>::Iterator iter(&compiler_temps_);
  for (CompilerTemp* ct = iter.Next(); ct != NULL; ct = iter.Next()) {
    loc[ct->s_reg_low].location = kLocCompilerTemp;
    loc[ct->s_reg_low].defined = true;
  }

  /* Treat Method* as a normal reference */
  loc[GetMethodSReg()].ref = true;

  reg_location_ = loc;

  int num_regs = cu_->num_dalvik_registers;

  /* Add types of incoming arguments based on signature */
  int num_ins = cu_->num_ins;
  if (num_ins > 0) {
    int s_reg = num_regs - num_ins;
    if ((cu_->access_flags & kAccStatic) == 0) {
      // For non-static, skip past "this"
      reg_location_[s_reg].defined = true;
      reg_location_[s_reg].ref = true;
      s_reg++;
    }
    const char* shorty = cu_->shorty;
    int shorty_len = strlen(shorty);
    for (int i = 1; i < shorty_len; i++) {
      switch (shorty[i]) {
        case 'D':
          reg_location_[s_reg].wide = true;
          reg_location_[s_reg+1].high_word = true;
          reg_location_[s_reg+1].fp = true;
          DCHECK_EQ(SRegToVReg(s_reg)+1, SRegToVReg(s_reg+1));
          reg_location_[s_reg].fp = true;
          reg_location_[s_reg].defined = true;
          s_reg++;
          break;
        case 'J':
          reg_location_[s_reg].wide = true;
          reg_location_[s_reg+1].high_word = true;
          DCHECK_EQ(SRegToVReg(s_reg)+1, SRegToVReg(s_reg+1));
          reg_location_[s_reg].core = true;
          reg_location_[s_reg].defined = true;
          s_reg++;
          break;
        case 'F':
          reg_location_[s_reg].fp = true;
          reg_location_[s_reg].defined = true;
          break;
        case 'L':
          reg_location_[s_reg].ref = true;
          reg_location_[s_reg].defined = true;
          break;
        default:
          reg_location_[s_reg].core = true;
          reg_location_[s_reg].defined = true;
          break;
        }
        s_reg++;
      }
  }
}

/*
 * Set the s_reg_low field to refer to the pre-SSA name of the
 * base Dalvik virtual register.  Once we add a better register
 * allocator, remove this remapping.
 */
void MIRGraph::RemapRegLocations() {
  for (int i = 0; i < GetNumSSARegs(); i++) {
    if (reg_location_[i].location != kLocCompilerTemp) {
      int orig_sreg = reg_location_[i].s_reg_low;
      reg_location_[i].orig_sreg = orig_sreg;
      reg_location_[i].s_reg_low = SRegToVReg(orig_sreg);
    }
  }
}

}  // namespace art
