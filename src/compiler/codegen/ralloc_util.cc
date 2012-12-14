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

/* This file contains register alloction support. */

#include "codegen_util.h"
#include "compiler/compiler_ir.h"
#include "compiler/compiler_utility.h"
#include "compiler/dataflow.h"
#include "ralloc_util.h"

namespace art {

static const RegLocation bad_loc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0,
                                    INVALID_REG, INVALID_REG, INVALID_SREG,
                                    INVALID_SREG};

/*
 * Free all allocated temps in the temp pools.  Note that this does
 * not affect the "liveness" of a temp register, which will stay
 * live until it is either explicitly killed or reallocated.
 */
void ResetRegPool(CompilationUnit* cu)
{
  int i;
  for (i=0; i < cu->reg_pool->num_core_regs; i++) {
    if (cu->reg_pool->core_regs[i].is_temp)
      cu->reg_pool->core_regs[i].in_use = false;
  }
  for (i=0; i < cu->reg_pool->num_fp_regs; i++) {
    if (cu->reg_pool->FPRegs[i].is_temp)
      cu->reg_pool->FPRegs[i].in_use = false;
  }
}

 /*
  * Set up temp & preserved register pools specialized by target.
  * Note: num_regs may be zero.
  */
void CompilerInitPool(RegisterInfo* regs, int* reg_nums, int num)
{
  int i;
  for (i=0; i < num; i++) {
    regs[i].reg = reg_nums[i];
    regs[i].in_use = false;
    regs[i].is_temp = false;
    regs[i].pair = false;
    regs[i].live = false;
    regs[i].dirty = false;
    regs[i].s_reg = INVALID_SREG;
  }
}

void DumpRegPool(RegisterInfo* p, int num_regs)
{
  LOG(INFO) << "================================================";
  for (int i = 0; i < num_regs; i++) {
    LOG(INFO) << StringPrintf(
        "R[%d]: T:%d, U:%d, P:%d, p:%d, LV:%d, D:%d, SR:%d, ST:%x, EN:%x",
        p[i].reg, p[i].is_temp, p[i].in_use, p[i].pair, p[i].partner,
        p[i].live, p[i].dirty, p[i].s_reg, reinterpret_cast<uintptr_t>(p[i].def_start),
        reinterpret_cast<uintptr_t>(p[i].def_end));
  }
  LOG(INFO) << "================================================";
}

void DumpCoreRegPool(CompilationUnit* cu)
{
  DumpRegPool(cu->reg_pool->core_regs, cu->reg_pool->num_core_regs);
}

void DumpFpRegPool(CompilationUnit* cu)
{
  DumpRegPool(cu->reg_pool->FPRegs, cu->reg_pool->num_fp_regs);
}

/* Mark a temp register as dead.  Does not affect allocation state. */
static void ClobberBody(CompilationUnit *cu, RegisterInfo* p)
{
  if (p->is_temp) {
    DCHECK(!(p->live && p->dirty))  << "Live & dirty temp in clobber";
    p->live = false;
    p->s_reg = INVALID_SREG;
    p->def_start = NULL;
    p->def_end = NULL;
    if (p->pair) {
      p->pair = false;
      Clobber(cu, p->partner);
    }
  }
}

/* Mark a temp register as dead.  Does not affect allocation state. */
void Clobber(CompilationUnit* cu, int reg)
{
  Codegen* cg = cu->cg.get();
  ClobberBody(cu, cg->GetRegInfo(cu, reg));
}

static void ClobberSRegBody(RegisterInfo* p, int num_regs, int s_reg)
{
  int i;
  for (i=0; i< num_regs; i++) {
    if (p[i].s_reg == s_reg) {
      if (p[i].is_temp) {
        p[i].live = false;
      }
      p[i].def_start = NULL;
      p[i].def_end = NULL;
    }
  }
}

/*
 * Break the association between a Dalvik vreg and a physical temp register of either register
 * class.
 * TODO: Ideally, the public version of this code should not exist.  Besides its local usage
 * in the register utilities, is is also used by code gen routines to work around a deficiency in
 * local register allocation, which fails to distinguish between the "in" and "out" identities
 * of Dalvik vregs.  This can result in useless register copies when the same Dalvik vreg
 * is used both as the source and destination register of an operation in which the type
 * changes (for example: INT_TO_FLOAT v1, v1).  Revisit when improved register allocation is
 * addressed.
 */
void ClobberSReg(CompilationUnit* cu, int s_reg)
{
#ifndef NDEBUG
  /* Reset live temp tracking sanity checker */
  if (s_reg == cu->live_sreg) {
    cu->live_sreg = INVALID_SREG;
  }
#endif
  ClobberSRegBody(cu->reg_pool->core_regs, cu->reg_pool->num_core_regs, s_reg);
  ClobberSRegBody(cu->reg_pool->FPRegs, cu->reg_pool->num_fp_regs, s_reg);
}

/*
 * SSA names associated with the initial definitions of Dalvik
 * registers are the same as the Dalvik register number (and
 * thus take the same position in the promotion_map.  However,
 * the special Method* and compiler temp resisters use negative
 * v_reg numbers to distinguish them and can have an arbitrary
 * ssa name (above the last original Dalvik register).  This function
 * maps SSA names to positions in the promotion_map array.
 */
int SRegToPMap(CompilationUnit* cu, int s_reg)
{
  DCHECK_LT(s_reg, cu->num_ssa_regs);
  DCHECK_GE(s_reg, 0);
  int v_reg = SRegToVReg(cu, s_reg);
  if (v_reg >= 0) {
    DCHECK_LT(v_reg, cu->num_dalvik_registers);
    return v_reg;
  } else {
    int pos = std::abs(v_reg) - std::abs(SSA_METHOD_BASEREG);
    DCHECK_LE(pos, cu->num_compiler_temps);
    return cu->num_dalvik_registers + pos;
  }
}

void RecordCorePromotion(CompilationUnit* cu, int reg, int s_reg)
{
  Codegen* cg = cu->cg.get();
  int p_map_idx = SRegToPMap(cu, s_reg);
  int v_reg = SRegToVReg(cu, s_reg);
  cg->GetRegInfo(cu, reg)->in_use = true;
  cu->core_spill_mask |= (1 << reg);
  // Include reg for later sort
  cu->core_vmap_table.push_back(reg << VREG_NUM_WIDTH |
                                 (v_reg & ((1 << VREG_NUM_WIDTH) - 1)));
  cu->num_core_spills++;
  cu->promotion_map[p_map_idx].core_location = kLocPhysReg;
  cu->promotion_map[p_map_idx].core_reg = reg;
}

/* Reserve a callee-save register.  Return -1 if none available */
static int AllocPreservedCoreReg(CompilationUnit* cu, int s_reg)
{
  int res = -1;
  RegisterInfo* core_regs = cu->reg_pool->core_regs;
  for (int i = 0; i < cu->reg_pool->num_core_regs; i++) {
    if (!core_regs[i].is_temp && !core_regs[i].in_use) {
      res = core_regs[i].reg;
      RecordCorePromotion(cu, res, s_reg);
      break;
    }
  }
  return res;
}

void RecordFpPromotion(CompilationUnit* cu, int reg, int s_reg)
{
  Codegen* cg = cu->cg.get();
  int p_map_idx = SRegToPMap(cu, s_reg);
  int v_reg = SRegToVReg(cu, s_reg);
  cg->GetRegInfo(cu, reg)->in_use = true;
  cg->MarkPreservedSingle(cu, v_reg, reg);
  cu->promotion_map[p_map_idx].fp_location = kLocPhysReg;
  cu->promotion_map[p_map_idx].FpReg = reg;
}

/*
 * Reserve a callee-save fp single register.  Try to fullfill request for
 * even/odd  allocation, but go ahead and allocate anything if not
 * available.  If nothing's available, return -1.
 */
static int AllocPreservedSingle(CompilationUnit* cu, int s_reg, bool even)
{
  int res = -1;
  RegisterInfo* FPRegs = cu->reg_pool->FPRegs;
  for (int i = 0; i < cu->reg_pool->num_fp_regs; i++) {
    if (!FPRegs[i].is_temp && !FPRegs[i].in_use &&
      ((FPRegs[i].reg & 0x1) == 0) == even) {
      res = FPRegs[i].reg;
      RecordFpPromotion(cu, res, s_reg);
      break;
    }
  }
  return res;
}

/*
 * Somewhat messy code here.  We want to allocate a pair of contiguous
 * physical single-precision floating point registers starting with
 * an even numbered reg.  It is possible that the paired s_reg (s_reg+1)
 * has already been allocated - try to fit if possible.  Fail to
 * allocate if we can't meet the requirements for the pair of
 * s_reg<=sX[even] & (s_reg+1)<= sX+1.
 */
static int AllocPreservedDouble(CompilationUnit* cu, int s_reg)
{
  Codegen* cg = cu->cg.get();
  int res = -1; // Assume failure
  int v_reg = SRegToVReg(cu, s_reg);
  int p_map_idx = SRegToPMap(cu, s_reg);
  if (cu->promotion_map[p_map_idx+1].fp_location == kLocPhysReg) {
    // Upper reg is already allocated.  Can we fit?
    int high_reg = cu->promotion_map[p_map_idx+1].FpReg;
    if ((high_reg & 1) == 0) {
      // High reg is even - fail.
      return res;
    }
    // Is the low reg of the pair free?
    RegisterInfo* p = cg->GetRegInfo(cu, high_reg-1);
    if (p->in_use || p->is_temp) {
      // Already allocated or not preserved - fail.
      return res;
    }
    // OK - good to go.
    res = p->reg;
    p->in_use = true;
    DCHECK_EQ((res & 1), 0);
    cg->MarkPreservedSingle(cu, v_reg, res);
  } else {
    RegisterInfo* FPRegs = cu->reg_pool->FPRegs;
    for (int i = 0; i < cu->reg_pool->num_fp_regs; i++) {
      if (!FPRegs[i].is_temp && !FPRegs[i].in_use &&
        ((FPRegs[i].reg & 0x1) == 0x0) &&
        !FPRegs[i+1].is_temp && !FPRegs[i+1].in_use &&
        ((FPRegs[i+1].reg & 0x1) == 0x1) &&
        (FPRegs[i].reg + 1) == FPRegs[i+1].reg) {
        res = FPRegs[i].reg;
        FPRegs[i].in_use = true;
        cg->MarkPreservedSingle(cu, v_reg, res);
        FPRegs[i+1].in_use = true;
        DCHECK_EQ(res + 1, FPRegs[i+1].reg);
        cg->MarkPreservedSingle(cu, v_reg+1, res+1);
        break;
      }
    }
  }
  if (res != -1) {
    cu->promotion_map[p_map_idx].fp_location = kLocPhysReg;
    cu->promotion_map[p_map_idx].FpReg = res;
    cu->promotion_map[p_map_idx+1].fp_location = kLocPhysReg;
    cu->promotion_map[p_map_idx+1].FpReg = res + 1;
  }
  return res;
}


/*
 * Reserve a callee-save fp register.   If this register can be used
 * as the first of a double, attempt to allocate an even pair of fp
 * single regs (but if can't still attempt to allocate a single, preferring
 * first to allocate an odd register.
 */
static int AllocPreservedFPReg(CompilationUnit* cu, int s_reg, bool double_start)
{
  int res = -1;
  if (double_start) {
    res = AllocPreservedDouble(cu, s_reg);
  }
  if (res == -1) {
    res = AllocPreservedSingle(cu, s_reg, false /* try odd # */);
  }
  if (res == -1)
    res = AllocPreservedSingle(cu, s_reg, true /* try even # */);
  return res;
}

static int AllocTempBody(CompilationUnit* cu, RegisterInfo* p, int num_regs, int* next_temp,
                          bool required)
{
  int i;
  int next = *next_temp;
  for (i=0; i< num_regs; i++) {
    if (next >= num_regs)
      next = 0;
    if (p[next].is_temp && !p[next].in_use && !p[next].live) {
      Clobber(cu, p[next].reg);
      p[next].in_use = true;
      p[next].pair = false;
      *next_temp = next + 1;
      return p[next].reg;
    }
    next++;
  }
  next = *next_temp;
  for (i=0; i< num_regs; i++) {
    if (next >= num_regs)
      next = 0;
    if (p[next].is_temp && !p[next].in_use) {
      Clobber(cu, p[next].reg);
      p[next].in_use = true;
      p[next].pair = false;
      *next_temp = next + 1;
      return p[next].reg;
    }
    next++;
  }
  if (required) {
    CodegenDump(cu);
    DumpRegPool(cu->reg_pool->core_regs,
          cu->reg_pool->num_core_regs);
    LOG(FATAL) << "No free temp registers";
  }
  return -1;  // No register available
}

//REDO: too many assumptions.
int AllocTempDouble(CompilationUnit* cu)
{
  RegisterInfo* p = cu->reg_pool->FPRegs;
  int num_regs = cu->reg_pool->num_fp_regs;
  /* Start looking at an even reg */
  int next = cu->reg_pool->next_fp_reg & ~0x1;

  // First try to avoid allocating live registers
  for (int i=0; i < num_regs; i+=2) {
    if (next >= num_regs)
      next = 0;
    if ((p[next].is_temp && !p[next].in_use && !p[next].live) &&
      (p[next+1].is_temp && !p[next+1].in_use && !p[next+1].live)) {
      Clobber(cu, p[next].reg);
      Clobber(cu, p[next+1].reg);
      p[next].in_use = true;
      p[next+1].in_use = true;
      DCHECK_EQ((p[next].reg+1), p[next+1].reg);
      DCHECK_EQ((p[next].reg & 0x1), 0);
      cu->reg_pool->next_fp_reg = next + 2;
      if (cu->reg_pool->next_fp_reg >= num_regs) {
        cu->reg_pool->next_fp_reg = 0;
      }
      return p[next].reg;
    }
    next += 2;
  }
  next = cu->reg_pool->next_fp_reg & ~0x1;

  // No choice - find a pair and kill it.
  for (int i=0; i < num_regs; i+=2) {
    if (next >= num_regs)
      next = 0;
    if (p[next].is_temp && !p[next].in_use && p[next+1].is_temp &&
      !p[next+1].in_use) {
      Clobber(cu, p[next].reg);
      Clobber(cu, p[next+1].reg);
      p[next].in_use = true;
      p[next+1].in_use = true;
      DCHECK_EQ((p[next].reg+1), p[next+1].reg);
      DCHECK_EQ((p[next].reg & 0x1), 0);
      cu->reg_pool->next_fp_reg = next + 2;
      if (cu->reg_pool->next_fp_reg >= num_regs) {
        cu->reg_pool->next_fp_reg = 0;
      }
      return p[next].reg;
    }
    next += 2;
  }
  LOG(FATAL) << "No free temp registers (pair)";
  return -1;
}

/* Return a temp if one is available, -1 otherwise */
int AllocFreeTemp(CompilationUnit* cu)
{
  return AllocTempBody(cu, cu->reg_pool->core_regs,
             cu->reg_pool->num_core_regs,
             &cu->reg_pool->next_core_reg, true);
}

int AllocTemp(CompilationUnit* cu)
{
  return AllocTempBody(cu, cu->reg_pool->core_regs,
             cu->reg_pool->num_core_regs,
             &cu->reg_pool->next_core_reg, true);
}

int AllocTempFloat(CompilationUnit* cu)
{
  return AllocTempBody(cu, cu->reg_pool->FPRegs,
             cu->reg_pool->num_fp_regs,
             &cu->reg_pool->next_fp_reg, true);
}

static RegisterInfo* AllocLiveBody(RegisterInfo* p, int num_regs, int s_reg)
{
  int i;
  if (s_reg == -1)
    return NULL;
  for (i=0; i < num_regs; i++) {
    if (p[i].live && (p[i].s_reg == s_reg)) {
      if (p[i].is_temp)
        p[i].in_use = true;
      return &p[i];
    }
  }
  return NULL;
}

RegisterInfo* AllocLive(CompilationUnit* cu, int s_reg, int reg_class)
{
  RegisterInfo* res = NULL;
  switch (reg_class) {
    case kAnyReg:
      res = AllocLiveBody(cu->reg_pool->FPRegs,
                cu->reg_pool->num_fp_regs, s_reg);
      if (res)
        break;
      /* Intentional fallthrough */
    case kCoreReg:
      res = AllocLiveBody(cu->reg_pool->core_regs,
                cu->reg_pool->num_core_regs, s_reg);
      break;
    case kFPReg:
      res = AllocLiveBody(cu->reg_pool->FPRegs,
                cu->reg_pool->num_fp_regs, s_reg);
      break;
    default:
      LOG(FATAL) << "Invalid register type";
  }
  return res;
}

void FreeTemp(CompilationUnit* cu, int reg)
{
  RegisterInfo* p = cu->reg_pool->core_regs;
  int num_regs = cu->reg_pool->num_core_regs;
  int i;
  for (i=0; i< num_regs; i++) {
    if (p[i].reg == reg) {
      if (p[i].is_temp) {
        p[i].in_use = false;
      }
      p[i].pair = false;
      return;
    }
  }
  p = cu->reg_pool->FPRegs;
  num_regs = cu->reg_pool->num_fp_regs;
  for (i=0; i< num_regs; i++) {
    if (p[i].reg == reg) {
      if (p[i].is_temp) {
        p[i].in_use = false;
      }
      p[i].pair = false;
      return;
    }
  }
  LOG(FATAL) << "Tried to free a non-existant temp: r" << reg;
}

RegisterInfo* IsLive(CompilationUnit* cu, int reg)
{
  RegisterInfo* p = cu->reg_pool->core_regs;
  int num_regs = cu->reg_pool->num_core_regs;
  int i;
  for (i=0; i< num_regs; i++) {
    if (p[i].reg == reg) {
      return p[i].live ? &p[i] : NULL;
    }
  }
  p = cu->reg_pool->FPRegs;
  num_regs = cu->reg_pool->num_fp_regs;
  for (i=0; i< num_regs; i++) {
    if (p[i].reg == reg) {
      return p[i].live ? &p[i] : NULL;
    }
  }
  return NULL;
}

RegisterInfo* IsTemp(CompilationUnit* cu, int reg)
{
  Codegen* cg = cu->cg.get();
  RegisterInfo* p = cg->GetRegInfo(cu, reg);
  return (p->is_temp) ? p : NULL;
}

RegisterInfo* IsPromoted(CompilationUnit* cu, int reg)
{
  Codegen* cg = cu->cg.get();
  RegisterInfo* p = cg->GetRegInfo(cu, reg);
  return (p->is_temp) ? NULL : p;
}

bool IsDirty(CompilationUnit* cu, int reg)
{
  Codegen* cg = cu->cg.get();
  RegisterInfo* p = cg->GetRegInfo(cu, reg);
  return p->dirty;
}

/*
 * Similar to AllocTemp(), but forces the allocation of a specific
 * register.  No check is made to see if the register was previously
 * allocated.  Use with caution.
 */
void LockTemp(CompilationUnit* cu, int reg)
{
  RegisterInfo* p = cu->reg_pool->core_regs;
  int num_regs = cu->reg_pool->num_core_regs;
  int i;
  for (i=0; i< num_regs; i++) {
    if (p[i].reg == reg) {
      DCHECK(p[i].is_temp);
      p[i].in_use = true;
      p[i].live = false;
      return;
    }
  }
  p = cu->reg_pool->FPRegs;
  num_regs = cu->reg_pool->num_fp_regs;
  for (i=0; i< num_regs; i++) {
    if (p[i].reg == reg) {
      DCHECK(p[i].is_temp);
      p[i].in_use = true;
      p[i].live = false;
      return;
    }
  }
  LOG(FATAL) << "Tried to lock a non-existant temp: r" << reg;
}

static void ResetDefBody(RegisterInfo* p)
{
  p->def_start = NULL;
  p->def_end = NULL;
}

void ResetDef(CompilationUnit* cu, int reg)
{
  Codegen* cg = cu->cg.get();
  ResetDefBody(cg->GetRegInfo(cu, reg));
}

static void NullifyRange(CompilationUnit* cu, LIR *start, LIR *finish, int s_reg1, int s_reg2)
{
  if (start && finish) {
    LIR *p;
    DCHECK_EQ(s_reg1, s_reg2);
    for (p = start; ;p = p->next) {
      NopLIR(p);
      if (p == finish)
        break;
    }
  }
}

/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
void MarkDef(CompilationUnit* cu, RegLocation rl,
             LIR *start, LIR *finish)
{
  DCHECK(!rl.wide);
  DCHECK(start && start->next);
  DCHECK(finish);
  Codegen* cg = cu->cg.get();
  RegisterInfo* p = cg->GetRegInfo(cu, rl.low_reg);
  p->def_start = start->next;
  p->def_end = finish;
}

/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
void MarkDefWide(CompilationUnit* cu, RegLocation rl,
               LIR *start, LIR *finish)
{
  DCHECK(rl.wide);
  DCHECK(start && start->next);
  DCHECK(finish);
  Codegen* cg = cu->cg.get();
  RegisterInfo* p = cg->GetRegInfo(cu, rl.low_reg);
  ResetDef(cu, rl.high_reg);  // Only track low of pair
  p->def_start = start->next;
  p->def_end = finish;
}

RegLocation WideToNarrow(CompilationUnit* cu, RegLocation rl)
{
  DCHECK(rl.wide);
  Codegen* cg = cu->cg.get();
  if (rl.location == kLocPhysReg) {
    RegisterInfo* info_lo = cg->GetRegInfo(cu, rl.low_reg);
    RegisterInfo* info_hi = cg->GetRegInfo(cu, rl.high_reg);
    if (info_lo->is_temp) {
      info_lo->pair = false;
      info_lo->def_start = NULL;
      info_lo->def_end = NULL;
    }
    if (info_hi->is_temp) {
      info_hi->pair = false;
      info_hi->def_start = NULL;
      info_hi->def_end = NULL;
    }
  }
  rl.wide = false;
  return rl;
}

void ResetDefLoc(CompilationUnit* cu, RegLocation rl)
{
  DCHECK(!rl.wide);
  RegisterInfo* p = IsTemp(cu, rl.low_reg);
  if (p && !(cu->disable_opt & (1 << kSuppressLoads))) {
    DCHECK(!p->pair);
    NullifyRange(cu, p->def_start, p->def_end, p->s_reg, rl.s_reg_low);
  }
  ResetDef(cu, rl.low_reg);
}

void ResetDefLocWide(CompilationUnit* cu, RegLocation rl)
{
  DCHECK(rl.wide);
  RegisterInfo* p_low = IsTemp(cu, rl.low_reg);
  RegisterInfo* p_high = IsTemp(cu, rl.high_reg);
  if (p_low && !(cu->disable_opt & (1 << kSuppressLoads))) {
    DCHECK(p_low->pair);
    NullifyRange(cu, p_low->def_start, p_low->def_end, p_low->s_reg, rl.s_reg_low);
  }
  if (p_high && !(cu->disable_opt & (1 << kSuppressLoads))) {
    DCHECK(p_high->pair);
  }
  ResetDef(cu, rl.low_reg);
  ResetDef(cu, rl.high_reg);
}

void ResetDefTracking(CompilationUnit* cu)
{
  int i;
  for (i=0; i< cu->reg_pool->num_core_regs; i++) {
    ResetDefBody(&cu->reg_pool->core_regs[i]);
  }
  for (i=0; i< cu->reg_pool->num_fp_regs; i++) {
    ResetDefBody(&cu->reg_pool->FPRegs[i]);
  }
}

void ClobberAllRegs(CompilationUnit* cu)
{
  int i;
  for (i=0; i< cu->reg_pool->num_core_regs; i++) {
    ClobberBody(cu, &cu->reg_pool->core_regs[i]);
  }
  for (i=0; i< cu->reg_pool->num_fp_regs; i++) {
    ClobberBody(cu, &cu->reg_pool->FPRegs[i]);
  }
}

// Make sure nothing is live and dirty
static void FlushAllRegsBody(CompilationUnit* cu, RegisterInfo* info, int num_regs)
{
  Codegen* cg = cu->cg.get();
  int i;
  for (i=0; i < num_regs; i++) {
    if (info[i].live && info[i].dirty) {
      if (info[i].pair) {
        cg->FlushRegWide(cu, info[i].reg, info[i].partner);
      } else {
        cg->FlushReg(cu, info[i].reg);
      }
    }
  }
}

void FlushAllRegs(CompilationUnit* cu)
{
  FlushAllRegsBody(cu, cu->reg_pool->core_regs,
           cu->reg_pool->num_core_regs);
  FlushAllRegsBody(cu, cu->reg_pool->FPRegs,
           cu->reg_pool->num_fp_regs);
  ClobberAllRegs(cu);
}


//TUNING: rewrite all of this reg stuff.  Probably use an attribute table
static bool RegClassMatches(CompilationUnit* cu, int reg_class, int reg)
{
  Codegen* cg = cu->cg.get();
  if (reg_class == kAnyReg) {
    return true;
  } else if (reg_class == kCoreReg) {
    return !cg->IsFpReg(reg);
  } else {
    return cg->IsFpReg(reg);
  }
}

void MarkLive(CompilationUnit* cu, int reg, int s_reg)
{
  Codegen* cg = cu->cg.get();
  RegisterInfo* info = cg->GetRegInfo(cu, reg);
  if ((info->reg == reg) && (info->s_reg == s_reg) && info->live) {
    return;  /* already live */
  } else if (s_reg != INVALID_SREG) {
    ClobberSReg(cu, s_reg);
    if (info->is_temp) {
      info->live = true;
    }
  } else {
    /* Can't be live if no associated s_reg */
    DCHECK(info->is_temp);
    info->live = false;
  }
  info->s_reg = s_reg;
}

void MarkTemp(CompilationUnit* cu, int reg)
{
  Codegen* cg = cu->cg.get();
  RegisterInfo* info = cg->GetRegInfo(cu, reg);
  info->is_temp = true;
}

void UnmarkTemp(CompilationUnit* cu, int reg)
{
  Codegen* cg = cu->cg.get();
  RegisterInfo* info = cg->GetRegInfo(cu, reg);
  info->is_temp = false;
}

void MarkPair(CompilationUnit* cu, int low_reg, int high_reg)
{
  Codegen* cg = cu->cg.get();
  RegisterInfo* info_lo = cg->GetRegInfo(cu, low_reg);
  RegisterInfo* info_hi = cg->GetRegInfo(cu, high_reg);
  info_lo->pair = info_hi->pair = true;
  info_lo->partner = high_reg;
  info_hi->partner = low_reg;
}

void MarkClean(CompilationUnit* cu, RegLocation loc)
{
  Codegen* cg = cu->cg.get();
  RegisterInfo* info = cg->GetRegInfo(cu, loc.low_reg);
  info->dirty = false;
  if (loc.wide) {
    info = cg->GetRegInfo(cu, loc.high_reg);
    info->dirty = false;
  }
}

void MarkDirty(CompilationUnit* cu, RegLocation loc)
{
  if (loc.home) {
    // If already home, can't be dirty
    return;
  }
  Codegen* cg = cu->cg.get();
  RegisterInfo* info = cg->GetRegInfo(cu, loc.low_reg);
  info->dirty = true;
  if (loc.wide) {
    info = cg->GetRegInfo(cu, loc.high_reg);
    info->dirty = true;
  }
}

void MarkInUse(CompilationUnit* cu, int reg)
{
  Codegen* cg = cu->cg.get();
    RegisterInfo* info = cg->GetRegInfo(cu, reg);
    info->in_use = true;
}

static void CopyRegInfo(CompilationUnit* cu, int new_reg, int old_reg)
{
  Codegen* cg = cu->cg.get();
  RegisterInfo* new_info = cg->GetRegInfo(cu, new_reg);
  RegisterInfo* old_info = cg->GetRegInfo(cu, old_reg);
  // Target temp status must not change
  bool is_temp = new_info->is_temp;
  *new_info = *old_info;
  // Restore target's temp status
  new_info->is_temp = is_temp;
  new_info->reg = new_reg;
}

static bool CheckCorePoolSanity(CompilationUnit* cu)
{
  Codegen* cg = cu->cg.get();
   for (static int i = 0; i < cu->reg_pool->num_core_regs; i++) {
     if (cu->reg_pool->core_regs[i].pair) {
       static int my_reg = cu->reg_pool->core_regs[i].reg;
       static int my_sreg = cu->reg_pool->core_regs[i].s_reg;
       static int partner_reg = cu->reg_pool->core_regs[i].partner;
       static RegisterInfo* partner = cg->GetRegInfo(cu, partner_reg);
       DCHECK(partner != NULL);
       DCHECK(partner->pair);
       DCHECK_EQ(my_reg, partner->partner);
       static int partner_sreg = partner->s_reg;
       if (my_sreg == INVALID_SREG) {
         DCHECK_EQ(partner_sreg, INVALID_SREG);
       } else {
         int diff = my_sreg - partner_sreg;
         DCHECK((diff == -1) || (diff == 1));
       }
     }
     if (!cu->reg_pool->core_regs[i].live) {
       DCHECK(cu->reg_pool->core_regs[i].def_start == NULL);
       DCHECK(cu->reg_pool->core_regs[i].def_end == NULL);
     }
   }
   return true;
}

/*
 * Return an updated location record with current in-register status.
 * If the value lives in live temps, reflect that fact.  No code
 * is generated.  If the live value is part of an older pair,
 * clobber both low and high.
 * TUNING: clobbering both is a bit heavy-handed, but the alternative
 * is a bit complex when dealing with FP regs.  Examine code to see
 * if it's worthwhile trying to be more clever here.
 */

RegLocation UpdateLoc(CompilationUnit* cu, RegLocation loc)
{
  DCHECK(!loc.wide);
  DCHECK(CheckCorePoolSanity(cu));
  if (loc.location != kLocPhysReg) {
    DCHECK((loc.location == kLocDalvikFrame) ||
         (loc.location == kLocCompilerTemp));
    RegisterInfo* info_lo = AllocLive(cu, loc.s_reg_low, kAnyReg);
    if (info_lo) {
      if (info_lo->pair) {
        Clobber(cu, info_lo->reg);
        Clobber(cu, info_lo->partner);
        FreeTemp(cu, info_lo->reg);
      } else {
        loc.low_reg = info_lo->reg;
        loc.location = kLocPhysReg;
      }
    }
  }

  return loc;
}

/* see comments for update_loc */
RegLocation UpdateLocWide(CompilationUnit* cu, RegLocation loc)
{
  DCHECK(loc.wide);
  DCHECK(CheckCorePoolSanity(cu));
  Codegen* cg = cu->cg.get();
  if (loc.location != kLocPhysReg) {
    DCHECK((loc.location == kLocDalvikFrame) ||
         (loc.location == kLocCompilerTemp));
    // Are the dalvik regs already live in physical registers?
    RegisterInfo* info_lo = AllocLive(cu, loc.s_reg_low, kAnyReg);
    RegisterInfo* info_hi = AllocLive(cu,
        GetSRegHi(loc.s_reg_low), kAnyReg);
    bool match = true;
    match = match && (info_lo != NULL);
    match = match && (info_hi != NULL);
    // Are they both core or both FP?
    match = match && (cg->IsFpReg(info_lo->reg) == cg->IsFpReg(info_hi->reg));
    // If a pair of floating point singles, are they properly aligned?
    if (match && cg->IsFpReg(info_lo->reg)) {
      match &= ((info_lo->reg & 0x1) == 0);
      match &= ((info_hi->reg - info_lo->reg) == 1);
    }
    // If previously used as a pair, it is the same pair?
    if (match && (info_lo->pair || info_hi->pair)) {
      match = (info_lo->pair == info_hi->pair);
      match &= ((info_lo->reg == info_hi->partner) &&
            (info_hi->reg == info_lo->partner));
    }
    if (match) {
      // Can reuse - update the register usage info
      loc.low_reg = info_lo->reg;
      loc.high_reg = info_hi->reg;
      loc.location = kLocPhysReg;
      MarkPair(cu, loc.low_reg, loc.high_reg);
      DCHECK(!cg->IsFpReg(loc.low_reg) || ((loc.low_reg & 0x1) == 0));
      return loc;
    }
    // Can't easily reuse - clobber and free any overlaps
    if (info_lo) {
      Clobber(cu, info_lo->reg);
      FreeTemp(cu, info_lo->reg);
      if (info_lo->pair)
        Clobber(cu, info_lo->partner);
    }
    if (info_hi) {
      Clobber(cu, info_hi->reg);
      FreeTemp(cu, info_hi->reg);
      if (info_hi->pair)
        Clobber(cu, info_hi->partner);
    }
  }
  return loc;
}


/* For use in cases we don't know (or care) width */
RegLocation UpdateRawLoc(CompilationUnit* cu, RegLocation loc)
{
  if (loc.wide)
    return UpdateLocWide(cu, loc);
  else
    return UpdateLoc(cu, loc);
}

RegLocation EvalLocWide(CompilationUnit* cu, RegLocation loc, int reg_class, bool update)
{
  DCHECK(loc.wide);
  int new_regs;
  int low_reg;
  int high_reg;
  Codegen* cg = cu->cg.get();

  loc = UpdateLocWide(cu, loc);

  /* If already in registers, we can assume proper form.  Right reg class? */
  if (loc.location == kLocPhysReg) {
    DCHECK_EQ(cg->IsFpReg(loc.low_reg), cg->IsFpReg(loc.high_reg));
    DCHECK(!cg->IsFpReg(loc.low_reg) || ((loc.low_reg & 0x1) == 0));
    if (!RegClassMatches(cu, reg_class, loc.low_reg)) {
      /* Wrong register class.  Reallocate and copy */
      new_regs = cg->AllocTypedTempPair(cu, loc.fp, reg_class);
      low_reg = new_regs & 0xff;
      high_reg = (new_regs >> 8) & 0xff;
      cg->OpRegCopyWide(cu, low_reg, high_reg, loc.low_reg, loc.high_reg);
      CopyRegInfo(cu, low_reg, loc.low_reg);
      CopyRegInfo(cu, high_reg, loc.high_reg);
      Clobber(cu, loc.low_reg);
      Clobber(cu, loc.high_reg);
      loc.low_reg = low_reg;
      loc.high_reg = high_reg;
      MarkPair(cu, loc.low_reg, loc.high_reg);
      DCHECK(!cg->IsFpReg(loc.low_reg) || ((loc.low_reg & 0x1) == 0));
    }
    return loc;
  }

  DCHECK_NE(loc.s_reg_low, INVALID_SREG);
  DCHECK_NE(GetSRegHi(loc.s_reg_low), INVALID_SREG);

  new_regs = cg->AllocTypedTempPair(cu, loc.fp, reg_class);
  loc.low_reg = new_regs & 0xff;
  loc.high_reg = (new_regs >> 8) & 0xff;

  MarkPair(cu, loc.low_reg, loc.high_reg);
  if (update) {
    loc.location = kLocPhysReg;
    MarkLive(cu, loc.low_reg, loc.s_reg_low);
    MarkLive(cu, loc.high_reg, GetSRegHi(loc.s_reg_low));
  }
  DCHECK(!cg->IsFpReg(loc.low_reg) || ((loc.low_reg & 0x1) == 0));
  return loc;
}

RegLocation EvalLoc(CompilationUnit* cu, RegLocation loc,
                int reg_class, bool update)
{
  int new_reg;

  if (loc.wide)
    return EvalLocWide(cu, loc, reg_class, update);

  Codegen* cg = cu->cg.get();
  loc = UpdateLoc(cu, loc);

  if (loc.location == kLocPhysReg) {
    if (!RegClassMatches(cu, reg_class, loc.low_reg)) {
      /* Wrong register class.  Realloc, copy and transfer ownership */
      new_reg = cg->AllocTypedTemp(cu, loc.fp, reg_class);
      cg->OpRegCopy(cu, new_reg, loc.low_reg);
      CopyRegInfo(cu, new_reg, loc.low_reg);
      Clobber(cu, loc.low_reg);
      loc.low_reg = new_reg;
    }
    return loc;
  }

  DCHECK_NE(loc.s_reg_low, INVALID_SREG);

  new_reg = cg->AllocTypedTemp(cu, loc.fp, reg_class);
  loc.low_reg = new_reg;

  if (update) {
    loc.location = kLocPhysReg;
    MarkLive(cu, loc.low_reg, loc.s_reg_low);
  }
  return loc;
}

RegLocation GetRawSrc(CompilationUnit* cu, MIR* mir, int num)
{
  DCHECK(num < mir->ssa_rep->num_uses);
  RegLocation res = cu->reg_location[mir->ssa_rep->uses[num]];
  return res;
}

RegLocation GetRawDest(CompilationUnit* cu, MIR* mir)
{
  DCHECK_GT(mir->ssa_rep->num_defs, 0);
  RegLocation res = cu->reg_location[mir->ssa_rep->defs[0]];
  return res;
}

RegLocation GetDest(CompilationUnit* cu, MIR* mir)
{
  RegLocation res = GetRawDest(cu, mir);
  DCHECK(!res.wide);
  return res;
}

RegLocation GetSrc(CompilationUnit* cu, MIR* mir, int num)
{
  RegLocation res = GetRawSrc(cu, mir, num);
  DCHECK(!res.wide);
  return res;
}

RegLocation GetDestWide(CompilationUnit* cu, MIR* mir)
{
  RegLocation res = GetRawDest(cu, mir);
  DCHECK(res.wide);
  return res;
}

RegLocation GetSrcWide(CompilationUnit* cu, MIR* mir,
                 int low)
{
  RegLocation res = GetRawSrc(cu, mir, low);
  DCHECK(res.wide);
  return res;
}

/* USE SSA names to count references of base Dalvik v_regs. */
static void CountRefs(CompilationUnit *cu, BasicBlock* bb, RefCounts* core_counts,
                      RefCounts* fp_counts)
{
  if ((cu->disable_opt & (1 << kPromoteRegs)) ||
    !((bb->block_type == kEntryBlock) || (bb->block_type == kExitBlock) ||
      (bb->block_type == kDalvikByteCode))) {
    return;
  }
  for (int i = 0; i < cu->num_ssa_regs;) {
    RegLocation loc = cu->reg_location[i];
    RefCounts* counts = loc.fp ? fp_counts : core_counts;
    int p_map_idx = SRegToPMap(cu, loc.s_reg_low);
    //Don't count easily regenerated immediates
    if (loc.fp || loc.wide || !IsInexpensiveConstant(cu, loc)) {
      counts[p_map_idx].count += cu->use_counts.elem_list[i];
    }
    if (loc.wide) {
      if (loc.fp) {
        counts[p_map_idx].double_start = true;
        counts[p_map_idx+1].count += cu->use_counts.elem_list[i+1];
      }
      i += 2;
    } else {
      i++;
    }
  }
}

/* qsort callback function, sort descending */
static int SortCounts(const void *val1, const void *val2)
{
  const RefCounts* op1 = reinterpret_cast<const RefCounts*>(val1);
  const RefCounts* op2 = reinterpret_cast<const RefCounts*>(val2);
  return (op1->count == op2->count) ? 0 : (op1->count < op2->count ? 1 : -1);
}

static void DumpCounts(const RefCounts* arr, int size, const char* msg)
{
  LOG(INFO) << msg;
  for (int i = 0; i < size; i++) {
    LOG(INFO) << "s_reg[" << arr[i].s_reg << "]: " << arr[i].count;
  }
}

/*
 * Note: some portions of this code required even if the kPromoteRegs
 * optimization is disabled.
 */
void DoPromotion(CompilationUnit* cu)
{
  Codegen* cg = cu->cg.get();
  int reg_bias = cu->num_compiler_temps + 1;
  int dalvik_regs = cu->num_dalvik_registers;
  int num_regs = dalvik_regs + reg_bias;
  const int promotion_threshold = 2;

  // Allow target code to add any special registers
  cg->AdjustSpillMask(cu);

  /*
   * Simple register promotion. Just do a static count of the uses
   * of Dalvik registers.  Note that we examine the SSA names, but
   * count based on original Dalvik register name.  Count refs
   * separately based on type in order to give allocation
   * preference to fp doubles - which must be allocated sequential
   * physical single fp registers started with an even-numbered
   * reg.
   * TUNING: replace with linear scan once we have the ability
   * to describe register live ranges for GC.
   */
  RefCounts *core_regs = static_cast<RefCounts*>(NewMem(cu, sizeof(RefCounts) * num_regs,
                                                       true, kAllocRegAlloc));
  RefCounts *FpRegs = static_cast<RefCounts *>(NewMem(cu, sizeof(RefCounts) * num_regs,
                                                      true, kAllocRegAlloc));
  // Set ssa names for original Dalvik registers
  for (int i = 0; i < dalvik_regs; i++) {
    core_regs[i].s_reg = FpRegs[i].s_reg = i;
  }
  // Set ssa name for Method*
  core_regs[dalvik_regs].s_reg = cu->method_sreg;
  FpRegs[dalvik_regs].s_reg = cu->method_sreg;  // For consistecy
  // Set ssa names for compiler_temps
  for (int i = 1; i <= cu->num_compiler_temps; i++) {
    CompilerTemp* ct = reinterpret_cast<CompilerTemp*>(cu->compiler_temps.elem_list[i]);
    core_regs[dalvik_regs + i].s_reg = ct->s_reg;
    FpRegs[dalvik_regs + i].s_reg = ct->s_reg;
  }

  GrowableListIterator iterator;
  GrowableListIteratorInit(&cu->block_list, &iterator);
  while (true) {
    BasicBlock* bb;
    bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iterator));
    if (bb == NULL) break;
    CountRefs(cu, bb, core_regs, FpRegs);
  }

  /*
   * Ideally, we'd allocate doubles starting with an even-numbered
   * register.  Bias the counts to try to allocate any vreg that's
   * used as the start of a pair first.
   */
  for (int i = 0; i < num_regs; i++) {
    if (FpRegs[i].double_start) {
      FpRegs[i].count *= 2;
    }
  }

  // Sort the count arrays
  qsort(core_regs, num_regs, sizeof(RefCounts), SortCounts);
  qsort(FpRegs, num_regs, sizeof(RefCounts), SortCounts);

  if (cu->verbose) {
    DumpCounts(core_regs, num_regs, "Core regs after sort");
    DumpCounts(FpRegs, num_regs, "Fp regs after sort");
  }

  if (!(cu->disable_opt & (1 << kPromoteRegs))) {
    // Promote FpRegs
    for (int i = 0; (i < num_regs) &&
            (FpRegs[i].count >= promotion_threshold ); i++) {
      int p_map_idx = SRegToPMap(cu, FpRegs[i].s_reg);
      if (cu->promotion_map[p_map_idx].fp_location != kLocPhysReg) {
        int reg = AllocPreservedFPReg(cu, FpRegs[i].s_reg,
          FpRegs[i].double_start);
        if (reg < 0) {
          break;  // No more left
        }
      }
    }

    // Promote core regs
    for (int i = 0; (i < num_regs) &&
            (core_regs[i].count > promotion_threshold); i++) {
      int p_map_idx = SRegToPMap(cu, core_regs[i].s_reg);
      if (cu->promotion_map[p_map_idx].core_location !=
          kLocPhysReg) {
        int reg = AllocPreservedCoreReg(cu, core_regs[i].s_reg);
        if (reg < 0) {
           break;  // No more left
        }
      }
    }
  } else if (cu->qd_mode) {
    AllocPreservedCoreReg(cu, cu->method_sreg);
    for (int i = 0; i < num_regs; i++) {
      int reg = AllocPreservedCoreReg(cu, i);
      if (reg < 0) {
         break;  // No more left
      }
    }
  }


  // Now, update SSA names to new home locations
  for (int i = 0; i < cu->num_ssa_regs; i++) {
    RegLocation *curr = &cu->reg_location[i];
    int p_map_idx = SRegToPMap(cu, curr->s_reg_low);
    if (!curr->wide) {
      if (curr->fp) {
        if (cu->promotion_map[p_map_idx].fp_location == kLocPhysReg) {
          curr->location = kLocPhysReg;
          curr->low_reg = cu->promotion_map[p_map_idx].FpReg;
          curr->home = true;
        }
      } else {
        if (cu->promotion_map[p_map_idx].core_location == kLocPhysReg) {
          curr->location = kLocPhysReg;
          curr->low_reg = cu->promotion_map[p_map_idx].core_reg;
          curr->home = true;
        }
      }
      curr->high_reg = INVALID_REG;
    } else {
      if (curr->high_word) {
        continue;
      }
      if (curr->fp) {
        if ((cu->promotion_map[p_map_idx].fp_location == kLocPhysReg) &&
          (cu->promotion_map[p_map_idx+1].fp_location ==
          kLocPhysReg)) {
          int low_reg = cu->promotion_map[p_map_idx].FpReg;
          int high_reg = cu->promotion_map[p_map_idx+1].FpReg;
          // Doubles require pair of singles starting at even reg
          if (((low_reg & 0x1) == 0) && ((low_reg + 1) == high_reg)) {
            curr->location = kLocPhysReg;
            curr->low_reg = low_reg;
            curr->high_reg = high_reg;
            curr->home = true;
          }
        }
      } else {
        if ((cu->promotion_map[p_map_idx].core_location == kLocPhysReg)
           && (cu->promotion_map[p_map_idx+1].core_location ==
           kLocPhysReg)) {
          curr->location = kLocPhysReg;
          curr->low_reg = cu->promotion_map[p_map_idx].core_reg;
          curr->high_reg = cu->promotion_map[p_map_idx+1].core_reg;
          curr->home = true;
        }
      }
    }
  }
  if (cu->verbose) {
    DumpPromotionMap(cu);
  }
}

/* Returns sp-relative offset in bytes for a VReg */
int VRegOffset(CompilationUnit* cu, int v_reg)
{
  return StackVisitor::GetVRegOffset(cu->code_item, cu->core_spill_mask,
                                     cu->fp_spill_mask, cu->frame_size, v_reg);
}

/* Returns sp-relative offset in bytes for a SReg */
int SRegOffset(CompilationUnit* cu, int s_reg)
{
  return VRegOffset(cu, SRegToVReg(cu, s_reg));
}

RegLocation GetBadLoc()
{
  RegLocation res = bad_loc;
  return res;
}

/* Mark register usage state and return long retloc */
RegLocation GetReturnWide(CompilationUnit* cu, bool is_double)
{
  Codegen* cg = cu->cg.get();
  RegLocation gpr_res = cg->LocCReturnWide();
  RegLocation fpr_res = cg->LocCReturnDouble();
  RegLocation res = is_double ? fpr_res : gpr_res;
  Clobber(cu, res.low_reg);
  Clobber(cu, res.high_reg);
  LockTemp(cu, res.low_reg);
  LockTemp(cu, res.high_reg);
  MarkPair(cu, res.low_reg, res.high_reg);
  return res;
}

RegLocation GetReturn(CompilationUnit* cu, bool is_float)
{
  Codegen* cg = cu->cg.get();
  RegLocation gpr_res = cg->LocCReturn();
  RegLocation fpr_res = cg->LocCReturnFloat();
  RegLocation res = is_float ? fpr_res : gpr_res;
  Clobber(cu, res.low_reg);
  if (cu->instruction_set == kMips) {
    MarkInUse(cu, res.low_reg);
  } else {
    LockTemp(cu, res.low_reg);
  }
  return res;
}

}  // namespace art
