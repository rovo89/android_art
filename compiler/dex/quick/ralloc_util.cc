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

#include "dex/compiler_ir.h"
#include "dex/compiler_internals.h"
#include "mir_to_lir-inl.h"

namespace art {

/*
 * Free all allocated temps in the temp pools.  Note that this does
 * not affect the "liveness" of a temp register, which will stay
 * live until it is either explicitly killed or reallocated.
 */
void Mir2Lir::ResetRegPool() {
  GrowableArray<RegisterInfo*>::Iterator iter(&tempreg_info_);
  for (RegisterInfo* info = iter.Next(); info != NULL; info = iter.Next()) {
    info->MarkFree();
  }
  // Reset temp tracking sanity check.
  if (kIsDebugBuild) {
    live_sreg_ = INVALID_SREG;
  }
}

Mir2Lir::RegisterInfo::RegisterInfo(RegStorage r, uint64_t mask)
  : reg_(r), is_temp_(false), wide_value_(false), dirty_(false), aliased_(false), partner_(r),
    s_reg_(INVALID_SREG), def_use_mask_(mask), master_(this), def_start_(nullptr),
    def_end_(nullptr), alias_chain_(nullptr) {
  switch (r.StorageSize()) {
    case 0: storage_mask_ = 0xffffffff; break;
    case 4: storage_mask_ = 0x00000001; break;
    case 8: storage_mask_ = 0x00000003; break;
    case 16: storage_mask_ = 0x0000000f; break;
    case 32: storage_mask_ = 0x000000ff; break;
    case 64: storage_mask_ = 0x0000ffff; break;
    case 128: storage_mask_ = 0xffffffff; break;
  }
  used_storage_ = r.Valid() ? ~storage_mask_ : storage_mask_;
  liveness_ = used_storage_;
}

Mir2Lir::RegisterPool::RegisterPool(Mir2Lir* m2l, ArenaAllocator* arena,
                                    const std::vector<RegStorage>& core_regs,
                                    const std::vector<RegStorage>& sp_regs,
                                    const std::vector<RegStorage>& dp_regs,
                                    const std::vector<RegStorage>& reserved_regs,
                                    const std::vector<RegStorage>& core_temps,
                                    const std::vector<RegStorage>& sp_temps,
                                    const std::vector<RegStorage>& dp_temps) :
    core_regs_(arena, core_regs.size()), next_core_reg_(0), sp_regs_(arena, sp_regs.size()),
    next_sp_reg_(0), dp_regs_(arena, dp_regs.size()), next_dp_reg_(0), m2l_(m2l)  {
  // Initialize the fast lookup map.
  m2l_->reginfo_map_.Reset();
  if (kIsDebugBuild) {
    m2l_->reginfo_map_.Resize(RegStorage::kMaxRegs);
    for (unsigned i = 0; i < RegStorage::kMaxRegs; i++) {
      m2l_->reginfo_map_.Insert(nullptr);
    }
  } else {
    m2l_->reginfo_map_.SetSize(RegStorage::kMaxRegs);
  }

  // Construct the register pool.
  for (RegStorage reg : core_regs) {
    RegisterInfo* info = new (arena) RegisterInfo(reg, m2l_->GetRegMaskCommon(reg));
    m2l_->reginfo_map_.Put(reg.GetReg(), info);
    core_regs_.Insert(info);
  }
  for (RegStorage reg : sp_regs) {
    RegisterInfo* info = new (arena) RegisterInfo(reg, m2l_->GetRegMaskCommon(reg));
    m2l_->reginfo_map_.Put(reg.GetReg(), info);
    sp_regs_.Insert(info);
  }
  for (RegStorage reg : dp_regs) {
    RegisterInfo* info = new (arena) RegisterInfo(reg, m2l_->GetRegMaskCommon(reg));
    m2l_->reginfo_map_.Put(reg.GetReg(), info);
    dp_regs_.Insert(info);
  }

  // Keep special registers from being allocated.
  for (RegStorage reg : reserved_regs) {
    m2l_->MarkInUse(reg);
  }

  // Mark temp regs - all others not in use can be used for promotion
  for (RegStorage reg : core_temps) {
    m2l_->MarkTemp(reg);
  }
  for (RegStorage reg : sp_temps) {
    m2l_->MarkTemp(reg);
  }
  for (RegStorage reg : dp_temps) {
    m2l_->MarkTemp(reg);
  }

  // Add an entry for InvalidReg with zero'd mask.
  RegisterInfo* invalid_reg = new (arena) RegisterInfo(RegStorage::InvalidReg(), 0);
  m2l_->reginfo_map_.Put(RegStorage::InvalidReg().GetReg(), invalid_reg);
}

void Mir2Lir::DumpRegPool(GrowableArray<RegisterInfo*>* regs) {
  LOG(INFO) << "================================================";
  GrowableArray<RegisterInfo*>::Iterator it(regs);
  for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
    LOG(INFO) << StringPrintf(
        "R[%d:%d:%c]: T:%d, U:%d, W:%d, p:%d, LV:%d, D:%d, SR:%d, DEF:%d",
        info->GetReg().GetReg(), info->GetReg().GetRegNum(), info->GetReg().IsFloat() ?  'f' : 'c',
        info->IsTemp(), info->InUse(), info->IsWide(), info->Partner().GetReg(), info->IsLive(),
        info->IsDirty(), info->SReg(), info->DefStart() != nullptr);
  }
  LOG(INFO) << "================================================";
}

void Mir2Lir::DumpCoreRegPool() {
  DumpRegPool(&reg_pool_->core_regs_);
}

void Mir2Lir::DumpFpRegPool() {
  DumpRegPool(&reg_pool_->sp_regs_);
  DumpRegPool(&reg_pool_->dp_regs_);
}

void Mir2Lir::DumpRegPools() {
  LOG(INFO) << "Core registers";
  DumpCoreRegPool();
  LOG(INFO) << "FP registers";
  DumpFpRegPool();
}

void Mir2Lir::Clobber(RegStorage reg) {
  if (UNLIKELY(reg.IsPair())) {
    DCHECK(!GetRegInfo(reg.GetLow())->IsAliased());
    Clobber(reg.GetLow());
    DCHECK(!GetRegInfo(reg.GetHigh())->IsAliased());
    Clobber(reg.GetHigh());
  } else {
    RegisterInfo* info = GetRegInfo(reg);
    if (info->IsTemp() && !info->IsDead()) {
      ClobberBody(info);
      if (info->IsAliased()) {
        ClobberAliases(info);
      } else {
        RegisterInfo* master = info->Master();
        if (info != master) {
          ClobberBody(info->Master());
        }
      }
    }
  }
}

void Mir2Lir::ClobberAliases(RegisterInfo* info) {
  for (RegisterInfo* alias = info->GetAliasChain(); alias != nullptr;
       alias = alias->GetAliasChain()) {
    DCHECK(!alias->IsAliased());  // Only the master should be marked as alised.
    if (alias->SReg() != INVALID_SREG) {
      alias->SetSReg(INVALID_SREG);
      alias->ResetDefBody();
      if (alias->IsWide()) {
        alias->SetIsWide(false);
        if (alias->GetReg() != alias->Partner()) {
          RegisterInfo* p = GetRegInfo(alias->Partner());
          p->SetIsWide(false);
          p->MarkDead();
          p->ResetDefBody();
        }
      }
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
void Mir2Lir::ClobberSReg(int s_reg) {
  if (s_reg != INVALID_SREG) {
    if (kIsDebugBuild && s_reg == live_sreg_) {
      live_sreg_ = INVALID_SREG;
    }
    GrowableArray<RegisterInfo*>::Iterator iter(&tempreg_info_);
    for (RegisterInfo* info = iter.Next(); info != NULL; info = iter.Next()) {
      if (info->SReg() == s_reg) {
        ClobberBody(info);
        if (info->IsAliased()) {
          ClobberAliases(info);
        }
      }
    }
  }
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
int Mir2Lir::SRegToPMap(int s_reg) {
  DCHECK_LT(s_reg, mir_graph_->GetNumSSARegs());
  DCHECK_GE(s_reg, 0);
  int v_reg = mir_graph_->SRegToVReg(s_reg);
  if (v_reg >= 0) {
    DCHECK_LT(v_reg, cu_->num_dalvik_registers);
    return v_reg;
  } else {
    /*
     * It must be the case that the v_reg for temporary is less than or equal to the
     * base reg for temps. For that reason, "position" must be zero or positive.
     */
    unsigned int position = std::abs(v_reg) - std::abs(static_cast<int>(kVRegTempBaseReg));

    // The temporaries are placed after dalvik registers in the promotion map
    DCHECK_LT(position, mir_graph_->GetNumUsedCompilerTemps());
    return cu_->num_dalvik_registers + position;
  }
}

// TODO: refactor following Alloc/Record routines - much commonality.
void Mir2Lir::RecordCorePromotion(RegStorage reg, int s_reg) {
  int p_map_idx = SRegToPMap(s_reg);
  int v_reg = mir_graph_->SRegToVReg(s_reg);
  int reg_num = reg.GetRegNum();
  GetRegInfo(reg)->MarkInUse();
  core_spill_mask_ |= (1 << reg_num);
  // Include reg for later sort
  core_vmap_table_.push_back(reg_num << VREG_NUM_WIDTH | (v_reg & ((1 << VREG_NUM_WIDTH) - 1)));
  num_core_spills_++;
  promotion_map_[p_map_idx].core_location = kLocPhysReg;
  promotion_map_[p_map_idx].core_reg = reg_num;
}

/* Reserve a callee-save register.  Return InvalidReg if none available */
RegStorage Mir2Lir::AllocPreservedCoreReg(int s_reg) {
  RegStorage res;
  GrowableArray<RegisterInfo*>::Iterator it(&reg_pool_->core_regs_);
  for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
    if (!info->IsTemp() && !info->InUse()) {
      res = info->GetReg();
      RecordCorePromotion(res, s_reg);
      break;
    }
  }
  return res;
}

void Mir2Lir::RecordSinglePromotion(RegStorage reg, int s_reg) {
  int p_map_idx = SRegToPMap(s_reg);
  int v_reg = mir_graph_->SRegToVReg(s_reg);
  GetRegInfo(reg)->MarkInUse();
  MarkPreservedSingle(v_reg, reg);
  promotion_map_[p_map_idx].fp_location = kLocPhysReg;
  promotion_map_[p_map_idx].FpReg = reg.GetReg();
}

// Reserve a callee-save sp single register.
RegStorage Mir2Lir::AllocPreservedSingle(int s_reg) {
  RegStorage res;
  GrowableArray<RegisterInfo*>::Iterator it(&reg_pool_->sp_regs_);
  for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
    if (!info->IsTemp() && !info->InUse()) {
      res = info->GetReg();
      RecordSinglePromotion(res, s_reg);
      break;
    }
  }
  return res;
}

void Mir2Lir::RecordDoublePromotion(RegStorage reg, int s_reg) {
  int p_map_idx = SRegToPMap(s_reg);
  int v_reg = mir_graph_->SRegToVReg(s_reg);
  GetRegInfo(reg)->MarkInUse();
  MarkPreservedDouble(v_reg, reg);
  promotion_map_[p_map_idx].fp_location = kLocPhysReg;
  promotion_map_[p_map_idx].FpReg = reg.GetReg();
}

// Reserve a callee-save dp solo register.
RegStorage Mir2Lir::AllocPreservedDouble(int s_reg) {
  RegStorage res;
  GrowableArray<RegisterInfo*>::Iterator it(&reg_pool_->dp_regs_);
  for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
    if (!info->IsTemp() && !info->InUse()) {
      res = info->GetReg();
      RecordDoublePromotion(res, s_reg);
      break;
    }
  }
  return res;
}


RegStorage Mir2Lir::AllocTempBody(GrowableArray<RegisterInfo*> &regs, int* next_temp, bool required) {
  int num_regs = regs.Size();
  int next = *next_temp;
  for (int i = 0; i< num_regs; i++) {
    if (next >= num_regs)
      next = 0;
    RegisterInfo* info = regs.Get(next);
    // Try to allocate a register that doesn't hold a live value.
    if (info->IsTemp() && !info->InUse() && !info->IsLive()) {
      Clobber(info->GetReg());
      info->MarkInUse();
      /*
       * NOTE: "wideness" is an attribute of how the container is used, not its physical size.
       * The caller will set wideness as appropriate.
       */
      info->SetIsWide(false);
      *next_temp = next + 1;
      return info->GetReg();
    }
    next++;
  }
  next = *next_temp;
  // No free non-live regs.  Anything we can kill?
  for (int i = 0; i< num_regs; i++) {
    if (next >= num_regs)
      next = 0;
    RegisterInfo* info = regs.Get(next);
    if (info->IsTemp() && !info->InUse()) {
      // Got one.  Kill it.
      ClobberSReg(info->SReg());
      Clobber(info->GetReg());
      info->MarkInUse();
      info->SetIsWide(false);
      *next_temp = next + 1;
      return info->GetReg();
    }
    next++;
  }
  if (required) {
    CodegenDump();
    DumpRegPools();
    LOG(FATAL) << "No free temp registers";
  }
  return RegStorage::InvalidReg();  // No register available
}

/* Return a temp if one is available, -1 otherwise */
RegStorage Mir2Lir::AllocFreeTemp() {
  return AllocTempBody(reg_pool_->core_regs_, &reg_pool_->next_core_reg_, false);
}

RegStorage Mir2Lir::AllocTemp() {
  return AllocTempBody(reg_pool_->core_regs_, &reg_pool_->next_core_reg_, true);
}

RegStorage Mir2Lir::AllocTempSingle() {
  RegStorage res = AllocTempBody(reg_pool_->sp_regs_, &reg_pool_->next_sp_reg_, true);
  DCHECK(res.IsSingle()) << "Reg: 0x" << std::hex << res.GetRawBits();
  return res;
}

RegStorage Mir2Lir::AllocTempDouble() {
  RegStorage res = AllocTempBody(reg_pool_->dp_regs_, &reg_pool_->next_dp_reg_, true);
  DCHECK(res.IsDouble()) << "Reg: 0x" << std::hex << res.GetRawBits();
  return res;
}

RegStorage Mir2Lir::FindLiveReg(GrowableArray<RegisterInfo*> &regs, int s_reg) {
  RegStorage res;
  GrowableArray<RegisterInfo*>::Iterator it(&regs);
  for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
    if ((info->SReg() == s_reg) && info->IsLive()) {
      res = info->GetReg();
      break;
    }
  }
  return res;
}

RegStorage Mir2Lir::AllocLiveReg(int s_reg, int reg_class, bool wide) {
  RegStorage reg;
  // TODO: might be worth a sanity check here to verify at most 1 live reg per s_reg.
  if ((reg_class == kAnyReg) || (reg_class == kFPReg)) {
    reg = FindLiveReg(wide ? reg_pool_->dp_regs_ : reg_pool_->sp_regs_, s_reg);
  }
  if (!reg.Valid() && (reg_class != kFPReg)) {
    // TODO: add 64-bit core pool similar to above.
    reg = FindLiveReg(reg_pool_->core_regs_, s_reg);
  }
  if (reg.Valid()) {
    if (wide && !reg.IsFloat() && !Is64BitInstructionSet(cu_->instruction_set)) {
      // Only allow reg pairs for core regs on 32-bit targets.
      RegStorage high_reg = FindLiveReg(reg_pool_->core_regs_, s_reg + 1);
      if (high_reg.Valid()) {
        reg = RegStorage::MakeRegPair(reg, high_reg);
        MarkWide(reg);
      } else {
        // Only half available.
        reg = RegStorage::InvalidReg();
      }
    }
    if (reg.Valid() && (wide != GetRegInfo(reg)->IsWide())) {
      // Width mismatch - don't try to reuse.
      reg = RegStorage::InvalidReg();
    }
  }
  if (reg.Valid()) {
    if (reg.IsPair()) {
      RegisterInfo* info_low = GetRegInfo(reg.GetLow());
      RegisterInfo* info_high = GetRegInfo(reg.GetHigh());
      if (info_low->IsTemp()) {
        info_low->MarkInUse();
      }
      if (info_high->IsTemp()) {
        info_high->MarkInUse();
      }
    } else {
      RegisterInfo* info = GetRegInfo(reg);
      if (info->IsTemp()) {
        info->MarkInUse();
      }
    }
  } else {
    // Either not found, or something didn't match up. Clobber to prevent any stale instances.
    ClobberSReg(s_reg);
    if (wide) {
      ClobberSReg(s_reg + 1);
    }
  }
  return reg;
}

void Mir2Lir::FreeTemp(RegStorage reg) {
  if (reg.IsPair()) {
    FreeTemp(reg.GetLow());
    FreeTemp(reg.GetHigh());
  } else {
    RegisterInfo* p = GetRegInfo(reg);
    if (p->IsTemp()) {
      p->MarkFree();
      p->SetIsWide(false);
      p->SetPartner(reg);
    }
  }
}

bool Mir2Lir::IsLive(RegStorage reg) {
  bool res;
  if (reg.IsPair()) {
    RegisterInfo* p_lo = GetRegInfo(reg.GetLow());
    RegisterInfo* p_hi = GetRegInfo(reg.GetHigh());
    DCHECK_EQ(p_lo->IsLive(), p_hi->IsLive());
    res = p_lo->IsLive() || p_hi->IsLive();
  } else {
    RegisterInfo* p = GetRegInfo(reg);
    res = p->IsLive();
  }
  return res;
}

bool Mir2Lir::IsTemp(RegStorage reg) {
  bool res;
  if (reg.IsPair()) {
    RegisterInfo* p_lo = GetRegInfo(reg.GetLow());
    RegisterInfo* p_hi = GetRegInfo(reg.GetHigh());
    res = p_lo->IsTemp() || p_hi->IsTemp();
  } else {
    RegisterInfo* p = GetRegInfo(reg);
    res = p->IsTemp();
  }
  return res;
}

bool Mir2Lir::IsPromoted(RegStorage reg) {
  bool res;
  if (reg.IsPair()) {
    RegisterInfo* p_lo = GetRegInfo(reg.GetLow());
    RegisterInfo* p_hi = GetRegInfo(reg.GetHigh());
    res = !p_lo->IsTemp() || !p_hi->IsTemp();
  } else {
    RegisterInfo* p = GetRegInfo(reg);
    res = !p->IsTemp();
  }
  return res;
}

bool Mir2Lir::IsDirty(RegStorage reg) {
  bool res;
  if (reg.IsPair()) {
    RegisterInfo* p_lo = GetRegInfo(reg.GetLow());
    RegisterInfo* p_hi = GetRegInfo(reg.GetHigh());
    res = p_lo->IsDirty() || p_hi->IsDirty();
  } else {
    RegisterInfo* p = GetRegInfo(reg);
    res = p->IsDirty();
  }
  return res;
}

/*
 * Similar to AllocTemp(), but forces the allocation of a specific
 * register.  No check is made to see if the register was previously
 * allocated.  Use with caution.
 */
void Mir2Lir::LockTemp(RegStorage reg) {
  DCHECK(IsTemp(reg));
  if (reg.IsPair()) {
    RegisterInfo* p_lo = GetRegInfo(reg.GetLow());
    RegisterInfo* p_hi = GetRegInfo(reg.GetHigh());
    p_lo->MarkInUse();
    p_lo->MarkDead();
    p_hi->MarkInUse();
    p_hi->MarkDead();
  } else {
    RegisterInfo* p = GetRegInfo(reg);
    p->MarkInUse();
    p->MarkDead();
  }
}

void Mir2Lir::ResetDef(RegStorage reg) {
  if (reg.IsPair()) {
    GetRegInfo(reg.GetLow())->ResetDefBody();
    GetRegInfo(reg.GetHigh())->ResetDefBody();
  } else {
    GetRegInfo(reg)->ResetDefBody();
  }
}

void Mir2Lir::NullifyRange(RegStorage reg, int s_reg) {
  RegisterInfo* info = nullptr;
  RegStorage rs = reg.IsPair() ? reg.GetLow() : reg;
  if (IsTemp(rs)) {
    info = GetRegInfo(reg);
  }
  if ((info != nullptr) && (info->DefStart() != nullptr) && (info->DefEnd() != nullptr)) {
    DCHECK_EQ(info->SReg(), s_reg);  // Make sure we're on the same page.
    for (LIR* p = info->DefStart();; p = p->next) {
      NopLIR(p);
      if (p == info->DefEnd()) {
        break;
      }
    }
  }
}

/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
void Mir2Lir::MarkDef(RegLocation rl, LIR *start, LIR *finish) {
  DCHECK(!rl.wide);
  DCHECK(start && start->next);
  DCHECK(finish);
  RegisterInfo* p = GetRegInfo(rl.reg);
  p->SetDefStart(start->next);
  p->SetDefEnd(finish);
}

/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
void Mir2Lir::MarkDefWide(RegLocation rl, LIR *start, LIR *finish) {
  DCHECK(rl.wide);
  DCHECK(start && start->next);
  DCHECK(finish);
  RegisterInfo* p;
  if (rl.reg.IsPair()) {
    p = GetRegInfo(rl.reg.GetLow());
    ResetDef(rl.reg.GetHigh());  // Only track low of pair
  } else {
    p = GetRegInfo(rl.reg);
  }
  p->SetDefStart(start->next);
  p->SetDefEnd(finish);
}

RegLocation Mir2Lir::WideToNarrow(RegLocation rl) {
  DCHECK(rl.wide);
  if (rl.location == kLocPhysReg) {
    if (rl.reg.IsPair()) {
      RegisterInfo* info_lo = GetRegInfo(rl.reg.GetLow());
      RegisterInfo* info_hi = GetRegInfo(rl.reg.GetHigh());
      if (info_lo->IsTemp()) {
        info_lo->SetIsWide(false);
        info_lo->ResetDefBody();
      }
      if (info_hi->IsTemp()) {
        info_hi->SetIsWide(false);
        info_hi->ResetDefBody();
      }
      rl.reg = rl.reg.GetLow();
    } else {
      /*
       * TODO: If not a pair, we can't just drop the high register.  On some targets, we may be
       * able to re-cast the 64-bit register as 32 bits, so it might be worthwhile to revisit
       * this code.  Will probably want to make this a virtual function.
       */
      // Can't narrow 64-bit register.  Clobber.
      if (GetRegInfo(rl.reg)->IsTemp()) {
        Clobber(rl.reg);
        FreeTemp(rl.reg);
      }
      rl.location = kLocDalvikFrame;
    }
  }
  rl.wide = false;
  return rl;
}

void Mir2Lir::ResetDefLoc(RegLocation rl) {
  DCHECK(!rl.wide);
  if (IsTemp(rl.reg) && !(cu_->disable_opt & (1 << kSuppressLoads))) {
    NullifyRange(rl.reg, rl.s_reg_low);
  }
  ResetDef(rl.reg);
}

void Mir2Lir::ResetDefLocWide(RegLocation rl) {
  DCHECK(rl.wide);
  // If pair, only track low reg of pair.
  RegStorage rs = rl.reg.IsPair() ? rl.reg.GetLow() : rl.reg;
  if (IsTemp(rs) && !(cu_->disable_opt & (1 << kSuppressLoads))) {
    NullifyRange(rs, rl.s_reg_low);
  }
  ResetDef(rs);
}

void Mir2Lir::ResetDefTracking() {
  GrowableArray<RegisterInfo*>::Iterator core_it(&reg_pool_->core_regs_);
  for (RegisterInfo* info = core_it.Next(); info != nullptr; info = core_it.Next()) {
    info->ResetDefBody();
  }
  GrowableArray<RegisterInfo*>::Iterator sp_it(&reg_pool_->core_regs_);
  for (RegisterInfo* info = sp_it.Next(); info != nullptr; info = sp_it.Next()) {
    info->ResetDefBody();
  }
  GrowableArray<RegisterInfo*>::Iterator dp_it(&reg_pool_->core_regs_);
  for (RegisterInfo* info = dp_it.Next(); info != nullptr; info = dp_it.Next()) {
    info->ResetDefBody();
  }
}

void Mir2Lir::ClobberAllTemps() {
  GrowableArray<RegisterInfo*>::Iterator iter(&tempreg_info_);
  for (RegisterInfo* info = iter.Next(); info != NULL; info = iter.Next()) {
    ClobberBody(info);
  }
}

void Mir2Lir::FlushRegWide(RegStorage reg) {
  if (reg.IsPair()) {
    RegisterInfo* info1 = GetRegInfo(reg.GetLow());
    RegisterInfo* info2 = GetRegInfo(reg.GetHigh());
    DCHECK(info1 && info2 && info1->IsWide() && info2->IsWide() &&
         (info1->Partner() == info2->GetReg()) && (info2->Partner() == info1->GetReg()));
    if ((info1->IsLive() && info1->IsDirty()) || (info2->IsLive() && info2->IsDirty())) {
      if (!(info1->IsTemp() && info2->IsTemp())) {
        /* Should not happen.  If it does, there's a problem in eval_loc */
        LOG(FATAL) << "Long half-temp, half-promoted";
      }

      info1->SetIsDirty(false);
      info2->SetIsDirty(false);
      if (mir_graph_->SRegToVReg(info2->SReg()) < mir_graph_->SRegToVReg(info1->SReg())) {
        info1 = info2;
      }
      int v_reg = mir_graph_->SRegToVReg(info1->SReg());
      StoreBaseDisp(TargetReg(kSp), VRegOffset(v_reg), reg, k64);
    }
  } else {
    RegisterInfo* info = GetRegInfo(reg);
    if (info->IsLive() && info->IsDirty()) {
      info->SetIsDirty(false);
      int v_reg = mir_graph_->SRegToVReg(info->SReg());
      StoreBaseDisp(TargetReg(kSp), VRegOffset(v_reg), reg, k64);
    }
  }
}

void Mir2Lir::FlushReg(RegStorage reg) {
  DCHECK(!reg.IsPair());
  RegisterInfo* info = GetRegInfo(reg);
  if (info->IsLive() && info->IsDirty()) {
    info->SetIsDirty(false);
    int v_reg = mir_graph_->SRegToVReg(info->SReg());
    StoreBaseDisp(TargetReg(kSp), VRegOffset(v_reg), reg, kWord);
  }
}

void Mir2Lir::FlushSpecificReg(RegisterInfo* info) {
  if (info->IsWide()) {
    FlushRegWide(info->GetReg());
  } else {
    FlushReg(info->GetReg());
  }
}

void Mir2Lir::FlushAllRegs() {
  GrowableArray<RegisterInfo*>::Iterator it(&tempreg_info_);
  for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
    if (info->IsDirty() && info->IsLive()) {
      FlushSpecificReg(info);
    }
    info->MarkDead();
    info->SetSReg(INVALID_SREG);
    info->ResetDefBody();
    info->SetIsWide(false);
  }
}


bool Mir2Lir::RegClassMatches(int reg_class, RegStorage reg) {
  if (reg_class == kAnyReg) {
    return true;
  } else if (reg_class == kCoreReg) {
    return !reg.IsFloat();
  } else {
    return reg.IsFloat();
  }
}

void Mir2Lir::MarkLiveReg(RegStorage reg, int s_reg) {
  RegisterInfo* info = GetRegInfo(reg);
  if ((info->SReg() == s_reg) && info->IsLive()) {
    return;  // Already live.
  }
  if (s_reg != INVALID_SREG) {
    ClobberSReg(s_reg);
    if (info->IsTemp()) {
      info->MarkLive();
    }
  } else {
    // Can't be live if no associated s_reg.
    DCHECK(info->IsTemp());
    info->MarkDead();
  }
  info->SetSReg(s_reg);
}

void Mir2Lir::MarkLive(RegLocation loc) {
  RegStorage reg = loc.reg;
  int s_reg = loc.s_reg_low;
  if (reg.IsPair()) {
    MarkLiveReg(reg.GetLow(), s_reg);
    MarkLiveReg(reg.GetHigh(), s_reg+1);
  } else {
    if (loc.wide) {
      ClobberSReg(s_reg + 1);
    }
    MarkLiveReg(reg, s_reg);
  }
}

void Mir2Lir::MarkTemp(RegStorage reg) {
  DCHECK(!reg.IsPair());
  RegisterInfo* info = GetRegInfo(reg);
  tempreg_info_.Insert(info);
  info->SetIsTemp(true);
}

void Mir2Lir::UnmarkTemp(RegStorage reg) {
  DCHECK(!reg.IsPair());
  RegisterInfo* info = GetRegInfo(reg);
  tempreg_info_.Delete(info);
  info->SetIsTemp(false);
}

void Mir2Lir::MarkWide(RegStorage reg) {
  if (reg.IsPair()) {
    RegisterInfo* info_lo = GetRegInfo(reg.GetLow());
    RegisterInfo* info_hi = GetRegInfo(reg.GetHigh());
    info_lo->SetIsWide(true);
    info_hi->SetIsWide(true);
    info_lo->SetPartner(reg.GetHigh());
    info_hi->SetPartner(reg.GetLow());
  } else {
    RegisterInfo* info = GetRegInfo(reg);
    info->SetIsWide(true);
    info->SetPartner(reg);
  }
}

void Mir2Lir::MarkClean(RegLocation loc) {
  if (loc.reg.IsPair()) {
    RegisterInfo* info = GetRegInfo(loc.reg.GetLow());
    info->SetIsDirty(false);
    info = GetRegInfo(loc.reg.GetHigh());
    info->SetIsDirty(false);
  } else {
    RegisterInfo* info = GetRegInfo(loc.reg);
    info->SetIsDirty(false);
  }
}

// FIXME: need to verify rules/assumptions about how wide values are treated in 64BitSolos.
void Mir2Lir::MarkDirty(RegLocation loc) {
  if (loc.home) {
    // If already home, can't be dirty
    return;
  }
  if (loc.reg.IsPair()) {
    RegisterInfo* info = GetRegInfo(loc.reg.GetLow());
    info->SetIsDirty(true);
    info = GetRegInfo(loc.reg.GetHigh());
    info->SetIsDirty(true);
  } else {
    RegisterInfo* info = GetRegInfo(loc.reg);
    info->SetIsDirty(true);
  }
}

void Mir2Lir::MarkInUse(RegStorage reg) {
  if (reg.IsPair()) {
    GetRegInfo(reg.GetLow())->MarkInUse();
    GetRegInfo(reg.GetHigh())->MarkInUse();
  } else {
    GetRegInfo(reg)->MarkInUse();
  }
}

bool Mir2Lir::CheckCorePoolSanity() {
  GrowableArray<RegisterInfo*>::Iterator it(&reg_pool_->core_regs_);
  for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
    RegStorage my_reg = info->GetReg();
    if (info->IsWide() && my_reg.IsPair()) {
      int my_sreg = info->SReg();
      RegStorage partner_reg = info->Partner();
      RegisterInfo* partner = GetRegInfo(partner_reg);
      DCHECK(partner != NULL);
      DCHECK(partner->IsWide());
      DCHECK_EQ(my_reg.GetReg(), partner->Partner().GetReg());
      int partner_sreg = partner->SReg();
      if (my_sreg == INVALID_SREG) {
        DCHECK_EQ(partner_sreg, INVALID_SREG);
      } else {
        int diff = my_sreg - partner_sreg;
        DCHECK((diff == 0) || (diff == -1) || (diff == 1));
      }
    } else {
      // TODO: add whatever sanity checks might be useful for 64BitSolo regs here.
      // TODO: sanity checks for floating point pools?
    }
    if (!info->IsLive()) {
      DCHECK(info->DefStart() == NULL);
      DCHECK(info->DefEnd() == NULL);
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
RegLocation Mir2Lir::UpdateLoc(RegLocation loc) {
  DCHECK(!loc.wide);
  DCHECK(CheckCorePoolSanity());
  if (loc.location != kLocPhysReg) {
    DCHECK((loc.location == kLocDalvikFrame) ||
         (loc.location == kLocCompilerTemp));
    RegStorage reg = AllocLiveReg(loc.s_reg_low, kAnyReg, false);
    if (reg.Valid()) {
      bool match = true;
      RegisterInfo* info = GetRegInfo(reg);
      match &= !reg.IsPair();
      match &= !info->IsWide();
      if (match) {
        loc.location = kLocPhysReg;
        loc.reg = reg;
      } else {
        Clobber(reg);
        FreeTemp(reg);
      }
    }
  }
  return loc;
}

RegLocation Mir2Lir::UpdateLocWide(RegLocation loc) {
  DCHECK(loc.wide);
  DCHECK(CheckCorePoolSanity());
  if (loc.location != kLocPhysReg) {
    DCHECK((loc.location == kLocDalvikFrame) ||
         (loc.location == kLocCompilerTemp));
    RegStorage reg = AllocLiveReg(loc.s_reg_low, kAnyReg, true);
    if (reg.Valid()) {
      bool match = true;
      if (reg.IsPair()) {
        // If we've got a register pair, make sure that it was last used as the same pair.
        RegisterInfo* info_lo = GetRegInfo(reg.GetLow());
        RegisterInfo* info_hi = GetRegInfo(reg.GetHigh());
        match &= info_lo->IsWide();
        match &= info_hi->IsWide();
        match &= (info_lo->Partner() == info_hi->GetReg());
        match &= (info_hi->Partner() == info_lo->GetReg());
      } else {
        RegisterInfo* info = GetRegInfo(reg);
        match &= info->IsWide();
        match &= (info->GetReg() == info->Partner());
      }
      if (match) {
        loc.location = kLocPhysReg;
        loc.reg = reg;
      } else {
        Clobber(reg);
        FreeTemp(reg);
      }
    }
  }
  return loc;
}

/* For use in cases we don't know (or care) width */
RegLocation Mir2Lir::UpdateRawLoc(RegLocation loc) {
  if (loc.wide)
    return UpdateLocWide(loc);
  else
    return UpdateLoc(loc);
}

RegLocation Mir2Lir::EvalLocWide(RegLocation loc, int reg_class, bool update) {
  DCHECK(loc.wide);

  loc = UpdateLocWide(loc);

  /* If already in registers, we can assume proper form.  Right reg class? */
  if (loc.location == kLocPhysReg) {
    if (!RegClassMatches(reg_class, loc.reg)) {
      // Wrong register class.  Reallocate and transfer ownership.
      RegStorage new_regs = AllocTypedTempWide(loc.fp, reg_class);
      // Associate the old sreg with the new register and clobber the old register.
      GetRegInfo(new_regs)->SetSReg(GetRegInfo(loc.reg)->SReg());
      Clobber(loc.reg);
      loc.reg = new_regs;
      MarkWide(loc.reg);
    }
    return loc;
  }

  DCHECK_NE(loc.s_reg_low, INVALID_SREG);
  DCHECK_NE(GetSRegHi(loc.s_reg_low), INVALID_SREG);

  loc.reg = AllocTypedTempWide(loc.fp, reg_class);
  MarkWide(loc.reg);

  if (update) {
    loc.location = kLocPhysReg;
    MarkLive(loc);
  }
  return loc;
}

RegLocation Mir2Lir::EvalLoc(RegLocation loc, int reg_class, bool update) {
  if (loc.wide) {
    return EvalLocWide(loc, reg_class, update);
  }

  loc = UpdateLoc(loc);

  if (loc.location == kLocPhysReg) {
    if (!RegClassMatches(reg_class, loc.reg)) {
      // Wrong register class.  Reallocate and transfer ownership.
      RegStorage new_reg = AllocTypedTemp(loc.fp, reg_class);
      // Associate the old sreg with the new register and clobber the old register.
      GetRegInfo(new_reg)->SetSReg(GetRegInfo(loc.reg)->SReg());
      Clobber(loc.reg);
      loc.reg = new_reg;
    }
    return loc;
  }

  DCHECK_NE(loc.s_reg_low, INVALID_SREG);

  loc.reg = AllocTypedTemp(loc.fp, reg_class);

  if (update) {
    loc.location = kLocPhysReg;
    MarkLive(loc);
  }
  return loc;
}

/* USE SSA names to count references of base Dalvik v_regs. */
void Mir2Lir::CountRefs(RefCounts* core_counts, RefCounts* fp_counts, size_t num_regs) {
  for (int i = 0; i < mir_graph_->GetNumSSARegs(); i++) {
    RegLocation loc = mir_graph_->reg_location_[i];
    RefCounts* counts = loc.fp ? fp_counts : core_counts;
    int p_map_idx = SRegToPMap(loc.s_reg_low);
    if (loc.fp) {
      if (loc.wide) {
        // Treat doubles as a unit, using upper half of fp_counts array.
        counts[p_map_idx + num_regs].count += mir_graph_->GetUseCount(i);
        i++;
      } else {
        counts[p_map_idx].count += mir_graph_->GetUseCount(i);
      }
    } else if (!IsInexpensiveConstant(loc)) {
      counts[p_map_idx].count += mir_graph_->GetUseCount(i);
    }
  }
}

/* qsort callback function, sort descending */
static int SortCounts(const void *val1, const void *val2) {
  const Mir2Lir::RefCounts* op1 = reinterpret_cast<const Mir2Lir::RefCounts*>(val1);
  const Mir2Lir::RefCounts* op2 = reinterpret_cast<const Mir2Lir::RefCounts*>(val2);
  // Note that we fall back to sorting on reg so we get stable output
  // on differing qsort implementations (such as on host and target or
  // between local host and build servers).
  return (op1->count == op2->count)
          ? (op1->s_reg - op2->s_reg)
          : (op1->count < op2->count ? 1 : -1);
}

void Mir2Lir::DumpCounts(const RefCounts* arr, int size, const char* msg) {
  LOG(INFO) << msg;
  for (int i = 0; i < size; i++) {
    if ((arr[i].s_reg & STARTING_DOUBLE_SREG) != 0) {
      LOG(INFO) << "s_reg[D" << (arr[i].s_reg & ~STARTING_DOUBLE_SREG) << "]: " << arr[i].count;
    } else {
      LOG(INFO) << "s_reg[" << arr[i].s_reg << "]: " << arr[i].count;
    }
  }
}

/*
 * Note: some portions of this code required even if the kPromoteRegs
 * optimization is disabled.
 */
void Mir2Lir::DoPromotion() {
  int dalvik_regs = cu_->num_dalvik_registers;
  int num_regs = dalvik_regs + mir_graph_->GetNumUsedCompilerTemps();
  const int promotion_threshold = 1;
  // Allocate the promotion map - one entry for each Dalvik vReg or compiler temp
  promotion_map_ = static_cast<PromotionMap*>
      (arena_->Alloc(num_regs * sizeof(promotion_map_[0]), kArenaAllocRegAlloc));

  // Allow target code to add any special registers
  AdjustSpillMask();

  /*
   * Simple register promotion. Just do a static count of the uses
   * of Dalvik registers.  Note that we examine the SSA names, but
   * count based on original Dalvik register name.  Count refs
   * separately based on type in order to give allocation
   * preference to fp doubles - which must be allocated sequential
   * physical single fp registers starting with an even-numbered
   * reg.
   * TUNING: replace with linear scan once we have the ability
   * to describe register live ranges for GC.
   */
  RefCounts *core_regs =
      static_cast<RefCounts*>(arena_->Alloc(sizeof(RefCounts) * num_regs,
                                            kArenaAllocRegAlloc));
  RefCounts *FpRegs =
      static_cast<RefCounts *>(arena_->Alloc(sizeof(RefCounts) * num_regs * 2,
                                             kArenaAllocRegAlloc));
  // Set ssa names for original Dalvik registers
  for (int i = 0; i < dalvik_regs; i++) {
    core_regs[i].s_reg = FpRegs[i].s_reg = i;
  }

  // Set ssa names for compiler temporaries
  for (unsigned int ct_idx = 0; ct_idx < mir_graph_->GetNumUsedCompilerTemps(); ct_idx++) {
    CompilerTemp* ct = mir_graph_->GetCompilerTemp(ct_idx);
    core_regs[dalvik_regs + ct_idx].s_reg = ct->s_reg_low;
    FpRegs[dalvik_regs + ct_idx].s_reg = ct->s_reg_low;
    FpRegs[num_regs + dalvik_regs + ct_idx].s_reg = ct->s_reg_low;
  }

  // Duplicate in upper half to represent possible fp double starting sregs.
  for (int i = 0; i < num_regs; i++) {
    FpRegs[num_regs + i].s_reg = FpRegs[i].s_reg | STARTING_DOUBLE_SREG;
  }

  // Sum use counts of SSA regs by original Dalvik vreg.
  CountRefs(core_regs, FpRegs, num_regs);


  // Sort the count arrays
  qsort(core_regs, num_regs, sizeof(RefCounts), SortCounts);
  qsort(FpRegs, num_regs * 2, sizeof(RefCounts), SortCounts);

  if (cu_->verbose) {
    DumpCounts(core_regs, num_regs, "Core regs after sort");
    DumpCounts(FpRegs, num_regs * 2, "Fp regs after sort");
  }

  if (!(cu_->disable_opt & (1 << kPromoteRegs))) {
    // Promote FpRegs
    for (int i = 0; (i < (num_regs * 2)) && (FpRegs[i].count >= promotion_threshold); i++) {
      int p_map_idx = SRegToPMap(FpRegs[i].s_reg & ~STARTING_DOUBLE_SREG);
      if ((FpRegs[i].s_reg & STARTING_DOUBLE_SREG) != 0) {
        if ((promotion_map_[p_map_idx].fp_location != kLocPhysReg) &&
            (promotion_map_[p_map_idx + 1].fp_location != kLocPhysReg)) {
          int low_sreg = FpRegs[i].s_reg & ~STARTING_DOUBLE_SREG;
          // Ignore result - if can't alloc double may still be able to alloc singles.
          AllocPreservedDouble(low_sreg);
        }
      } else if (promotion_map_[p_map_idx].fp_location != kLocPhysReg) {
        RegStorage reg = AllocPreservedSingle(FpRegs[i].s_reg);
        if (!reg.Valid()) {
          break;  // No more left.
        }
      }
    }

    // Promote core regs
    for (int i = 0; (i < num_regs) &&
            (core_regs[i].count >= promotion_threshold); i++) {
      int p_map_idx = SRegToPMap(core_regs[i].s_reg);
      if (promotion_map_[p_map_idx].core_location !=
          kLocPhysReg) {
        RegStorage reg = AllocPreservedCoreReg(core_regs[i].s_reg);
        if (!reg.Valid()) {
           break;  // No more left
        }
      }
    }
  }

  // Now, update SSA names to new home locations
  for (int i = 0; i < mir_graph_->GetNumSSARegs(); i++) {
    RegLocation *curr = &mir_graph_->reg_location_[i];
    int p_map_idx = SRegToPMap(curr->s_reg_low);
    if (!curr->wide) {
      if (curr->fp) {
        if (promotion_map_[p_map_idx].fp_location == kLocPhysReg) {
          curr->location = kLocPhysReg;
          curr->reg = RegStorage::Solo32(promotion_map_[p_map_idx].FpReg);
          curr->home = true;
        }
      } else {
        if (promotion_map_[p_map_idx].core_location == kLocPhysReg) {
          curr->location = kLocPhysReg;
          curr->reg = RegStorage::Solo32(promotion_map_[p_map_idx].core_reg);
          curr->home = true;
        }
      }
    } else {
      if (curr->high_word) {
        continue;
      }
      if (curr->fp) {
        if ((promotion_map_[p_map_idx].fp_location == kLocPhysReg) &&
            (promotion_map_[p_map_idx+1].fp_location == kLocPhysReg)) {
          int low_reg = promotion_map_[p_map_idx].FpReg;
          int high_reg = promotion_map_[p_map_idx+1].FpReg;
          // Doubles require pair of singles starting at even reg
          // TODO: move target-specific restrictions out of here.
          if (((low_reg & 0x1) == 0) && ((low_reg + 1) == high_reg)) {
            curr->location = kLocPhysReg;
            if (cu_->instruction_set == kThumb2) {
              curr->reg = RegStorage::FloatSolo64(RegStorage::RegNum(low_reg) >> 1);
            } else {
              curr->reg = RegStorage(RegStorage::k64BitPair, low_reg, high_reg);
            }
            curr->home = true;
          }
        }
      } else {
        if ((promotion_map_[p_map_idx].core_location == kLocPhysReg)
           && (promotion_map_[p_map_idx+1].core_location ==
           kLocPhysReg)) {
          curr->location = kLocPhysReg;
          curr->reg = RegStorage(RegStorage::k64BitPair, promotion_map_[p_map_idx].core_reg,
                                 promotion_map_[p_map_idx+1].core_reg);
          curr->home = true;
        }
      }
    }
  }
  if (cu_->verbose) {
    DumpPromotionMap();
  }
}

/* Returns sp-relative offset in bytes for a VReg */
int Mir2Lir::VRegOffset(int v_reg) {
  return StackVisitor::GetVRegOffset(cu_->code_item, core_spill_mask_,
                                     fp_spill_mask_, frame_size_, v_reg,
                                     cu_->instruction_set);
}

/* Returns sp-relative offset in bytes for a SReg */
int Mir2Lir::SRegOffset(int s_reg) {
  return VRegOffset(mir_graph_->SRegToVReg(s_reg));
}

/* Mark register usage state and return long retloc */
RegLocation Mir2Lir::GetReturnWide(bool is_double) {
  RegLocation gpr_res = LocCReturnWide();
  RegLocation fpr_res = LocCReturnDouble();
  RegLocation res = is_double ? fpr_res : gpr_res;
  if (res.reg.IsPair()) {
    Clobber(res.reg);
    LockTemp(res.reg);
    // Does this wide value live in two registers or one vector register?
    if (res.reg.GetLowReg() != res.reg.GetHighReg()) {
      // FIXME: I think we want to mark these as wide as well.
      MarkWide(res.reg);
    }
  } else {
    Clobber(res.reg);
    LockTemp(res.reg);
    MarkWide(res.reg);
  }
  return res;
}

RegLocation Mir2Lir::GetReturn(bool is_float) {
  RegLocation gpr_res = LocCReturn();
  RegLocation fpr_res = LocCReturnFloat();
  RegLocation res = is_float ? fpr_res : gpr_res;
  Clobber(res.reg);
  if (cu_->instruction_set == kMips) {
    MarkInUse(res.reg);
  } else {
    LockTemp(res.reg);
  }
  return res;
}

void Mir2Lir::SimpleRegAlloc() {
  DoPromotion();

  if (cu_->verbose && !(cu_->disable_opt & (1 << kPromoteRegs))) {
    LOG(INFO) << "After Promotion";
    mir_graph_->DumpRegLocTable(mir_graph_->reg_location_, mir_graph_->GetNumSSARegs());
  }

  /* Set the frame size */
  frame_size_ = ComputeFrameSize();
}

/*
 * Get the "real" sreg number associated with an s_reg slot.  In general,
 * s_reg values passed through codegen are the SSA names created by
 * dataflow analysis and refer to slot numbers in the mir_graph_->reg_location
 * array.  However, renaming is accomplished by simply replacing RegLocation
 * entries in the reglocation[] array.  Therefore, when location
 * records for operands are first created, we need to ask the locRecord
 * identified by the dataflow pass what it's new name is.
 */
int Mir2Lir::GetSRegHi(int lowSreg) {
  return (lowSreg == INVALID_SREG) ? INVALID_SREG : lowSreg + 1;
}

bool Mir2Lir::LiveOut(int s_reg) {
  // For now.
  return true;
}

}  // namespace art
