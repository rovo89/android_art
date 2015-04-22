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

#include "mir_to_lir-inl.h"

#include "dex/compiler_ir.h"
#include "dex/dataflow_iterator-inl.h"
#include "dex/mir_graph.h"
#include "driver/compiler_driver.h"
#include "driver/dex_compilation_unit.h"
#include "utils/dex_cache_arrays_layout-inl.h"

namespace art {

/*
 * Free all allocated temps in the temp pools.  Note that this does
 * not affect the "liveness" of a temp register, which will stay
 * live until it is either explicitly killed or reallocated.
 */
void Mir2Lir::ResetRegPool() {
  for (RegisterInfo* info : tempreg_info_) {
    info->MarkFree();
  }
  // Reset temp tracking sanity check.
  if (kIsDebugBuild) {
    live_sreg_ = INVALID_SREG;
  }
}

Mir2Lir::RegisterInfo::RegisterInfo(RegStorage r, const ResourceMask& mask)
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
                                    const ArrayRef<const RegStorage>& core_regs,
                                    const ArrayRef<const RegStorage>& core64_regs,
                                    const ArrayRef<const RegStorage>& sp_regs,
                                    const ArrayRef<const RegStorage>& dp_regs,
                                    const ArrayRef<const RegStorage>& reserved_regs,
                                    const ArrayRef<const RegStorage>& reserved64_regs,
                                    const ArrayRef<const RegStorage>& core_temps,
                                    const ArrayRef<const RegStorage>& core64_temps,
                                    const ArrayRef<const RegStorage>& sp_temps,
                                    const ArrayRef<const RegStorage>& dp_temps) :
    core_regs_(arena->Adapter()), next_core_reg_(0),
    core64_regs_(arena->Adapter()), next_core64_reg_(0),
    sp_regs_(arena->Adapter()), next_sp_reg_(0),
    dp_regs_(arena->Adapter()), next_dp_reg_(0), m2l_(m2l)  {
  // Initialize the fast lookup map.
  m2l_->reginfo_map_.clear();
  m2l_->reginfo_map_.resize(RegStorage::kMaxRegs, nullptr);

  // Construct the register pool.
  core_regs_.reserve(core_regs.size());
  for (const RegStorage& reg : core_regs) {
    RegisterInfo* info = new (arena) RegisterInfo(reg, m2l_->GetRegMaskCommon(reg));
    m2l_->reginfo_map_[reg.GetReg()] = info;
    core_regs_.push_back(info);
  }
  core64_regs_.reserve(core64_regs.size());
  for (const RegStorage& reg : core64_regs) {
    RegisterInfo* info = new (arena) RegisterInfo(reg, m2l_->GetRegMaskCommon(reg));
    m2l_->reginfo_map_[reg.GetReg()] = info;
    core64_regs_.push_back(info);
  }
  sp_regs_.reserve(sp_regs.size());
  for (const RegStorage& reg : sp_regs) {
    RegisterInfo* info = new (arena) RegisterInfo(reg, m2l_->GetRegMaskCommon(reg));
    m2l_->reginfo_map_[reg.GetReg()] = info;
    sp_regs_.push_back(info);
  }
  dp_regs_.reserve(dp_regs.size());
  for (const RegStorage& reg : dp_regs) {
    RegisterInfo* info = new (arena) RegisterInfo(reg, m2l_->GetRegMaskCommon(reg));
    m2l_->reginfo_map_[reg.GetReg()] = info;
    dp_regs_.push_back(info);
  }

  // Keep special registers from being allocated.
  for (RegStorage reg : reserved_regs) {
    m2l_->MarkInUse(reg);
  }
  for (RegStorage reg : reserved64_regs) {
    m2l_->MarkInUse(reg);
  }

  // Mark temp regs - all others not in use can be used for promotion
  for (RegStorage reg : core_temps) {
    m2l_->MarkTemp(reg);
  }
  for (RegStorage reg : core64_temps) {
    m2l_->MarkTemp(reg);
  }
  for (RegStorage reg : sp_temps) {
    m2l_->MarkTemp(reg);
  }
  for (RegStorage reg : dp_temps) {
    m2l_->MarkTemp(reg);
  }

  // Add an entry for InvalidReg with zero'd mask.
  RegisterInfo* invalid_reg = new (arena) RegisterInfo(RegStorage::InvalidReg(), kEncodeNone);
  m2l_->reginfo_map_[RegStorage::InvalidReg().GetReg()] = invalid_reg;

  // Existence of core64 registers implies wide references.
  if (core64_regs_.size() != 0) {
    ref_regs_ = &core64_regs_;
    next_ref_reg_ = &next_core64_reg_;
  } else {
    ref_regs_ = &core_regs_;
    next_ref_reg_ = &next_core_reg_;
  }
}

void Mir2Lir::DumpRegPool(ArenaVector<RegisterInfo*>* regs) {
  LOG(INFO) << "================================================";
  for (RegisterInfo* info : *regs) {
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
  DumpRegPool(&reg_pool_->core64_regs_);
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
      if (info->GetReg().NotExactlyEquals(info->Partner())) {
        ClobberBody(GetRegInfo(info->Partner()));
      }
      ClobberBody(info);
      if (info->IsAliased()) {
        ClobberAliases(info, info->StorageMask());
      } else {
        RegisterInfo* master = info->Master();
        if (info != master) {
          ClobberBody(info->Master());
          ClobberAliases(info->Master(), info->StorageMask());
        }
      }
    }
  }
}

void Mir2Lir::ClobberAliases(RegisterInfo* info, uint32_t clobber_mask) {
  for (RegisterInfo* alias = info->GetAliasChain(); alias != nullptr;
       alias = alias->GetAliasChain()) {
    DCHECK(!alias->IsAliased());  // Only the master should be marked as alised.
    // Only clobber if we have overlap.
    if ((alias->StorageMask() & clobber_mask) != 0) {
      ClobberBody(alias);
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
    for (RegisterInfo* info : tempreg_info_) {
      if (info->SReg() == s_reg) {
        if (info->GetReg().NotExactlyEquals(info->Partner())) {
          // Dealing with a pair - clobber the other half.
          DCHECK(!info->IsAliased());
          ClobberBody(GetRegInfo(info->Partner()));
        }
        ClobberBody(info);
        if (info->IsAliased()) {
          ClobberAliases(info, info->StorageMask());
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
  return v_reg;
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
  /*
   * Note: it really doesn't matter much whether we allocate from the core or core64
   * pool for 64-bit targets - but for some targets it does matter whether allocations
   * happens from the single or double pool.  This entire section of code could stand
   * a good refactoring.
   */
  for (RegisterInfo* info : reg_pool_->core_regs_) {
    if (!info->IsTemp() && !info->InUse()) {
      res = info->GetReg();
      RecordCorePromotion(res, s_reg);
      break;
    }
  }
  return res;
}

void Mir2Lir::RecordFpPromotion(RegStorage reg, int s_reg) {
  DCHECK_NE(cu_->instruction_set, kThumb2);
  int p_map_idx = SRegToPMap(s_reg);
  int v_reg = mir_graph_->SRegToVReg(s_reg);
  int reg_num = reg.GetRegNum();
  GetRegInfo(reg)->MarkInUse();
  fp_spill_mask_ |= (1 << reg_num);
  // Include reg for later sort
  fp_vmap_table_.push_back(reg_num << VREG_NUM_WIDTH | (v_reg & ((1 << VREG_NUM_WIDTH) - 1)));
  num_fp_spills_++;
  promotion_map_[p_map_idx].fp_location = kLocPhysReg;
  promotion_map_[p_map_idx].fp_reg = reg.GetReg();
}

// Reserve a callee-save floating point.
RegStorage Mir2Lir::AllocPreservedFpReg(int s_reg) {
  /*
   * For targets other than Thumb2, it doesn't matter whether we allocate from
   * the sp_regs_ or dp_regs_ pool.  Some refactoring is in order here.
   */
  DCHECK_NE(cu_->instruction_set, kThumb2);
  RegStorage res;
  for (RegisterInfo* info : reg_pool_->sp_regs_) {
    if (!info->IsTemp() && !info->InUse()) {
      res = info->GetReg();
      RecordFpPromotion(res, s_reg);
      break;
    }
  }
  return res;
}

// TODO: this is Thumb2 only.  Remove when DoPromotion refactored.
RegStorage Mir2Lir::AllocPreservedDouble(int s_reg) {
  UNUSED(s_reg);
  UNIMPLEMENTED(FATAL) << "Unexpected use of AllocPreservedDouble";
  UNREACHABLE();
}

// TODO: this is Thumb2 only.  Remove when DoPromotion refactored.
RegStorage Mir2Lir::AllocPreservedSingle(int s_reg) {
  UNUSED(s_reg);
  UNIMPLEMENTED(FATAL) << "Unexpected use of AllocPreservedSingle";
  UNREACHABLE();
}


RegStorage Mir2Lir::AllocTempBody(ArenaVector<RegisterInfo*>& regs, int* next_temp, bool required) {
  int num_regs = regs.size();
  int next = *next_temp;
  for (int i = 0; i< num_regs; i++) {
    if (next >= num_regs) {
      next = 0;
    }
    RegisterInfo* info = regs[next];
    // Try to allocate a register that doesn't hold a live value.
    if (info->IsTemp() && !info->InUse() && info->IsDead()) {
      // If it's wide, split it up.
      if (info->IsWide()) {
        // If the pair was associated with a wide value, unmark the partner as well.
        if (info->SReg() != INVALID_SREG) {
          RegisterInfo* partner = GetRegInfo(info->Partner());
          DCHECK_EQ(info->GetReg().GetRegNum(), partner->Partner().GetRegNum());
          DCHECK(partner->IsWide());
          partner->SetIsWide(false);
        }
        info->SetIsWide(false);
      }
      Clobber(info->GetReg());
      info->MarkInUse();
      *next_temp = next + 1;
      return info->GetReg();
    }
    next++;
  }
  next = *next_temp;
  // No free non-live regs.  Anything we can kill?
  for (int i = 0; i< num_regs; i++) {
    if (next >= num_regs) {
      next = 0;
    }
    RegisterInfo* info = regs[next];
    if (info->IsTemp() && !info->InUse()) {
      // Got one.  Kill it.
      ClobberSReg(info->SReg());
      Clobber(info->GetReg());
      info->MarkInUse();
      if (info->IsWide()) {
        RegisterInfo* partner = GetRegInfo(info->Partner());
        DCHECK_EQ(info->GetReg().GetRegNum(), partner->Partner().GetRegNum());
        DCHECK(partner->IsWide());
        info->SetIsWide(false);
        partner->SetIsWide(false);
      }
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

RegStorage Mir2Lir::AllocTemp(bool required) {
  return AllocTempBody(reg_pool_->core_regs_, &reg_pool_->next_core_reg_, required);
}

RegStorage Mir2Lir::AllocTempWide(bool required) {
  RegStorage res;
  if (reg_pool_->core64_regs_.size() != 0) {
    res = AllocTempBody(reg_pool_->core64_regs_, &reg_pool_->next_core64_reg_, required);
  } else {
    RegStorage low_reg = AllocTemp();
    RegStorage high_reg = AllocTemp();
    res = RegStorage::MakeRegPair(low_reg, high_reg);
  }
  if (required) {
    CheckRegStorage(res, WidenessCheck::kCheckWide, RefCheck::kIgnoreRef, FPCheck::kCheckNotFP);
  }
  return res;
}

RegStorage Mir2Lir::AllocTempRef(bool required) {
  RegStorage res = AllocTempBody(*reg_pool_->ref_regs_, reg_pool_->next_ref_reg_, required);
  if (required) {
    DCHECK(!res.IsPair());
    CheckRegStorage(res, WidenessCheck::kCheckNotWide, RefCheck::kCheckRef, FPCheck::kCheckNotFP);
  }
  return res;
}

RegStorage Mir2Lir::AllocTempSingle(bool required) {
  RegStorage res = AllocTempBody(reg_pool_->sp_regs_, &reg_pool_->next_sp_reg_, required);
  if (required) {
    DCHECK(res.IsSingle()) << "Reg: 0x" << std::hex << res.GetRawBits();
    CheckRegStorage(res, WidenessCheck::kCheckNotWide, RefCheck::kCheckNotRef, FPCheck::kIgnoreFP);
  }
  return res;
}

RegStorage Mir2Lir::AllocTempDouble(bool required) {
  RegStorage res = AllocTempBody(reg_pool_->dp_regs_, &reg_pool_->next_dp_reg_, required);
  if (required) {
    DCHECK(res.IsDouble()) << "Reg: 0x" << std::hex << res.GetRawBits();
    CheckRegStorage(res, WidenessCheck::kCheckWide, RefCheck::kCheckNotRef, FPCheck::kIgnoreFP);
  }
  return res;
}

RegStorage Mir2Lir::AllocTypedTempWide(bool fp_hint, int reg_class, bool required) {
  DCHECK_NE(reg_class, kRefReg);  // NOTE: the Dalvik width of a reference is always 32 bits.
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg)) {
    return AllocTempDouble(required);
  }
  return AllocTempWide(required);
}

RegStorage Mir2Lir::AllocTypedTemp(bool fp_hint, int reg_class, bool required) {
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg)) {
    return AllocTempSingle(required);
  } else if (reg_class == kRefReg) {
    return AllocTempRef(required);
  }
  return AllocTemp(required);
}

RegStorage Mir2Lir::FindLiveReg(ArenaVector<RegisterInfo*>& regs, int s_reg) {
  RegStorage res;
  for (RegisterInfo* info : regs) {
    if ((info->SReg() == s_reg) && info->IsLive()) {
      res = info->GetReg();
      break;
    }
  }
  return res;
}

RegStorage Mir2Lir::AllocLiveReg(int s_reg, int reg_class, bool wide) {
  RegStorage reg;
  if (reg_class == kRefReg) {
    reg = FindLiveReg(*reg_pool_->ref_regs_, s_reg);
    CheckRegStorage(reg, WidenessCheck::kCheckNotWide, RefCheck::kCheckRef, FPCheck::kCheckNotFP);
  }
  if (!reg.Valid() && ((reg_class == kAnyReg) || (reg_class == kFPReg))) {
    reg = FindLiveReg(wide ? reg_pool_->dp_regs_ : reg_pool_->sp_regs_, s_reg);
  }
  if (!reg.Valid() && (reg_class != kFPReg)) {
    if (cu_->target64) {
      reg = FindLiveReg(wide || reg_class == kRefReg ? reg_pool_->core64_regs_ :
                                                       reg_pool_->core_regs_, s_reg);
    } else {
      reg = FindLiveReg(reg_pool_->core_regs_, s_reg);
    }
  }
  if (reg.Valid()) {
    if (wide && !reg.IsFloat() && !cu_->target64) {
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
  CheckRegStorage(reg, WidenessCheck::kIgnoreWide,
                  reg_class == kRefReg ? RefCheck::kCheckRef : RefCheck::kIgnoreRef,
                  FPCheck::kIgnoreFP);
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

void Mir2Lir::FreeRegLocTemps(RegLocation rl_keep, RegLocation rl_free) {
  DCHECK(rl_keep.wide);
  DCHECK(rl_free.wide);
  int free_low = rl_free.reg.GetLowReg();
  int free_high = rl_free.reg.GetHighReg();
  int keep_low = rl_keep.reg.GetLowReg();
  int keep_high = rl_keep.reg.GetHighReg();
  if ((free_low != keep_low) && (free_low != keep_high) &&
      (free_high != keep_low) && (free_high != keep_high)) {
    // No overlap, free both
    FreeTemp(rl_free.reg);
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
  for (RegisterInfo* info : tempreg_info_) {
    info->ResetDefBody();
  }
}

void Mir2Lir::ClobberAllTemps() {
  for (RegisterInfo* info : tempreg_info_) {
    ClobberBody(info);
  }
}

void Mir2Lir::FlushRegWide(RegStorage reg) {
  if (reg.IsPair()) {
    RegisterInfo* info1 = GetRegInfo(reg.GetLow());
    RegisterInfo* info2 = GetRegInfo(reg.GetHigh());
    DCHECK(info1 && info2 && info1->IsWide() && info2->IsWide() &&
           (info1->Partner().ExactlyEquals(info2->GetReg())) &&
           (info2->Partner().ExactlyEquals(info1->GetReg())));
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
      ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
      StoreBaseDisp(TargetPtrReg(kSp), VRegOffset(v_reg), reg, k64, kNotVolatile);
    }
  } else {
    RegisterInfo* info = GetRegInfo(reg);
    if (info->IsLive() && info->IsDirty()) {
      info->SetIsDirty(false);
      int v_reg = mir_graph_->SRegToVReg(info->SReg());
      ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
      StoreBaseDisp(TargetPtrReg(kSp), VRegOffset(v_reg), reg, k64, kNotVolatile);
    }
  }
}

void Mir2Lir::FlushReg(RegStorage reg) {
  DCHECK(!reg.IsPair());
  RegisterInfo* info = GetRegInfo(reg);
  if (info->IsLive() && info->IsDirty()) {
    info->SetIsDirty(false);
    int v_reg = mir_graph_->SRegToVReg(info->SReg());
    ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
    StoreBaseDisp(TargetPtrReg(kSp), VRegOffset(v_reg), reg, kWord, kNotVolatile);
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
  for (RegisterInfo* info : tempreg_info_) {
    if (info->IsDirty() && info->IsLive()) {
      FlushSpecificReg(info);
    }
    info->MarkDead();
    info->SetIsWide(false);
  }
}


bool Mir2Lir::RegClassMatches(int reg_class, RegStorage reg) {
  if (reg_class == kAnyReg) {
    return true;
  } else if ((reg_class == kCoreReg) || (reg_class == kRefReg)) {
    /*
     * For this purpose, consider Core and Ref to be the same class. We aren't dealing
     * with width here - that should be checked at a higher level (if needed).
     */
    return !reg.IsFloat();
  } else {
    return reg.IsFloat();
  }
}

void Mir2Lir::MarkLive(RegLocation loc) {
  RegStorage reg = loc.reg;
  if (!IsTemp(reg)) {
    return;
  }
  int s_reg = loc.s_reg_low;
  if (s_reg == INVALID_SREG) {
    // Can't be live if no associated sreg.
    if (reg.IsPair()) {
      GetRegInfo(reg.GetLow())->MarkDead();
      GetRegInfo(reg.GetHigh())->MarkDead();
    } else {
      GetRegInfo(reg)->MarkDead();
    }
  } else {
    if (reg.IsPair()) {
      RegisterInfo* info_lo = GetRegInfo(reg.GetLow());
      RegisterInfo* info_hi = GetRegInfo(reg.GetHigh());
      if (info_lo->IsLive() && (info_lo->SReg() == s_reg) && info_hi->IsLive() &&
          (info_hi->SReg() == s_reg)) {
        return;  // Already live.
      }
      ClobberSReg(s_reg);
      ClobberSReg(s_reg + 1);
      info_lo->MarkLive(s_reg);
      info_hi->MarkLive(s_reg + 1);
    } else {
      RegisterInfo* info = GetRegInfo(reg);
      if (info->IsLive() && (info->SReg() == s_reg)) {
        return;  // Already live.
      }
      ClobberSReg(s_reg);
      if (loc.wide) {
        ClobberSReg(s_reg + 1);
      }
      info->MarkLive(s_reg);
    }
    if (loc.wide) {
      MarkWide(reg);
    } else {
      MarkNarrow(reg);
    }
  }
}

void Mir2Lir::MarkTemp(RegStorage reg) {
  DCHECK(!reg.IsPair());
  RegisterInfo* info = GetRegInfo(reg);
  tempreg_info_.push_back(info);
  info->SetIsTemp(true);
}

void Mir2Lir::UnmarkTemp(RegStorage reg) {
  DCHECK(!reg.IsPair());
  RegisterInfo* info = GetRegInfo(reg);
  auto pos = std::find(tempreg_info_.begin(), tempreg_info_.end(), info);
  DCHECK(pos != tempreg_info_.end());
  tempreg_info_.erase(pos);
  info->SetIsTemp(false);
}

void Mir2Lir::MarkWide(RegStorage reg) {
  if (reg.IsPair()) {
    RegisterInfo* info_lo = GetRegInfo(reg.GetLow());
    RegisterInfo* info_hi = GetRegInfo(reg.GetHigh());
    // Unpair any old partners.
    if (info_lo->IsWide() && info_lo->Partner().NotExactlyEquals(info_hi->GetReg())) {
      GetRegInfo(info_lo->Partner())->SetIsWide(false);
    }
    if (info_hi->IsWide() && info_hi->Partner().NotExactlyEquals(info_lo->GetReg())) {
      GetRegInfo(info_hi->Partner())->SetIsWide(false);
    }
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

void Mir2Lir::MarkNarrow(RegStorage reg) {
  DCHECK(!reg.IsPair());
  RegisterInfo* info = GetRegInfo(reg);
  info->SetIsWide(false);
  info->SetPartner(reg);
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
  for (RegisterInfo* info : tempreg_info_) {
    int my_sreg = info->SReg();
    if (info->IsTemp() && info->IsLive() && info->IsWide() && my_sreg != INVALID_SREG) {
      RegStorage my_reg = info->GetReg();
      RegStorage partner_reg = info->Partner();
      RegisterInfo* partner = GetRegInfo(partner_reg);
      DCHECK(partner != nullptr);
      DCHECK(partner->IsWide());
      DCHECK_EQ(my_reg.GetReg(), partner->Partner().GetReg());
      DCHECK(partner->IsLive());
      int partner_sreg = partner->SReg();
      int diff = my_sreg - partner_sreg;
      DCHECK((diff == 0) || (diff == -1) || (diff == 1));
    }
    if (info->Master() != info) {
      // Aliased.
      if (info->IsLive() && (info->SReg() != INVALID_SREG)) {
        // If I'm live, master should not be live, but should show liveness in alias set.
        DCHECK_EQ(info->Master()->SReg(), INVALID_SREG);
        DCHECK(!info->Master()->IsDead());
      }
// TODO: Add checks in !info->IsDead() case to ensure every live bit is owned by exactly 1 reg.
    }
    if (info->IsAliased()) {
      // Has child aliases.
      DCHECK_EQ(info->Master(), info);
      if (info->IsLive() && (info->SReg() != INVALID_SREG)) {
        // Master live, no child should be dead - all should show liveness in set.
        for (RegisterInfo* p = info->GetAliasChain(); p != nullptr; p = p->GetAliasChain()) {
          DCHECK(!p->IsDead());
          DCHECK_EQ(p->SReg(), INVALID_SREG);
        }
      } else if (!info->IsDead()) {
        // Master not live, one or more aliases must be.
        bool live_alias = false;
        for (RegisterInfo* p = info->GetAliasChain(); p != nullptr; p = p->GetAliasChain()) {
          live_alias |= p->IsLive();
        }
        DCHECK(live_alias);
      }
    }
    if (info->IsLive() && (info->SReg() == INVALID_SREG)) {
      // If not fully live, should have INVALID_SREG and def's should be null.
      DCHECK(info->DefStart() == nullptr);
      DCHECK(info->DefEnd() == nullptr);
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
    RegStorage reg = AllocLiveReg(loc.s_reg_low, loc.ref ? kRefReg : kAnyReg, false);
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
    CheckRegLocation(loc);
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
        match &= (info_lo->Partner().ExactlyEquals(info_hi->GetReg()));
        match &= (info_hi->Partner().ExactlyEquals(info_lo->GetReg()));
      } else {
        RegisterInfo* info = GetRegInfo(reg);
        match &= info->IsWide();
        match &= (info->GetReg().ExactlyEquals(info->Partner()));
      }
      if (match) {
        loc.location = kLocPhysReg;
        loc.reg = reg;
      } else {
        Clobber(reg);
        FreeTemp(reg);
      }
    }
    CheckRegLocation(loc);
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
      // Clobber the old regs.
      Clobber(loc.reg);
      // ...and mark the new ones live.
      loc.reg = new_regs;
      MarkWide(loc.reg);
      MarkLive(loc);
    }
    CheckRegLocation(loc);
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
  CheckRegLocation(loc);
  return loc;
}

RegLocation Mir2Lir::EvalLoc(RegLocation loc, int reg_class, bool update) {
  // Narrow reg_class if the loc is a ref.
  if (loc.ref && reg_class == kAnyReg) {
    reg_class = kRefReg;
  }

  if (loc.wide) {
    return EvalLocWide(loc, reg_class, update);
  }

  loc = UpdateLoc(loc);

  if (loc.location == kLocPhysReg) {
    if (!RegClassMatches(reg_class, loc.reg)) {
      // Wrong register class.  Reallocate and transfer ownership.
      RegStorage new_reg = AllocTypedTemp(loc.fp, reg_class);
      // Clobber the old reg.
      Clobber(loc.reg);
      // ...and mark the new one live.
      loc.reg = new_reg;
      MarkLive(loc);
    }
    CheckRegLocation(loc);
    return loc;
  }

  DCHECK_NE(loc.s_reg_low, INVALID_SREG);

  loc.reg = AllocTypedTemp(loc.fp, reg_class);
  CheckRegLocation(loc);

  if (update) {
    loc.location = kLocPhysReg;
    MarkLive(loc);
  }
  CheckRegLocation(loc);
  return loc;
}

void Mir2Lir::AnalyzeMIR(RefCounts* core_counts, MIR* mir, uint32_t weight) {
  // NOTE: This should be in sync with functions that actually generate code for
  // the opcodes below. However, if we get this wrong, the generated code will
  // still be correct even if it may be sub-optimal.
  int opcode = mir->dalvikInsn.opcode;
  bool uses_method = false;
  bool uses_pc_rel_load = false;
  uint32_t dex_cache_array_offset = std::numeric_limits<uint32_t>::max();
  switch (opcode) {
    case Instruction::CHECK_CAST:
    case Instruction::INSTANCE_OF: {
      if ((opcode == Instruction::CHECK_CAST) &&
          (mir->optimization_flags & MIR_IGNORE_CHECK_CAST) != 0) {
        break;  // No code generated.
      }
      uint32_t type_idx =
          (opcode == Instruction::CHECK_CAST) ? mir->dalvikInsn.vB : mir->dalvikInsn.vC;
      bool type_known_final, type_known_abstract, use_declaring_class;
      bool needs_access_check = !cu_->compiler_driver->CanAccessTypeWithoutChecks(
          cu_->method_idx, *cu_->dex_file, type_idx,
          &type_known_final, &type_known_abstract, &use_declaring_class);
      if (opcode == Instruction::CHECK_CAST && !needs_access_check &&
          cu_->compiler_driver->IsSafeCast(
              mir_graph_->GetCurrentDexCompilationUnit(), mir->offset)) {
        break;  // No code generated.
      }
      if (!needs_access_check && !use_declaring_class && CanUseOpPcRelDexCacheArrayLoad()) {
        uses_pc_rel_load = true;  // And ignore method use in slow path.
        dex_cache_array_offset = dex_cache_arrays_layout_.TypeOffset(type_idx);
      } else {
        uses_method = true;
      }
      break;
    }

    case Instruction::CONST_CLASS:
      if (CanUseOpPcRelDexCacheArrayLoad() &&
          cu_->compiler_driver->CanAccessTypeWithoutChecks(cu_->method_idx, *cu_->dex_file,
                                                           mir->dalvikInsn.vB)) {
        uses_pc_rel_load = true;  // And ignore method use in slow path.
        dex_cache_array_offset = dex_cache_arrays_layout_.TypeOffset(mir->dalvikInsn.vB);
      } else {
        uses_method = true;
      }
      break;

    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      if (CanUseOpPcRelDexCacheArrayLoad()) {
        uses_pc_rel_load = true;  // And ignore method use in slow path.
        dex_cache_array_offset = dex_cache_arrays_layout_.StringOffset(mir->dalvikInsn.vB);
      } else {
        uses_method = true;
      }
      break;

    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::INVOKE_INTERFACE_RANGE:
    case Instruction::INVOKE_VIRTUAL_QUICK:
    case Instruction::INVOKE_VIRTUAL_RANGE_QUICK: {
      const MirMethodLoweringInfo& info = mir_graph_->GetMethodLoweringInfo(mir);
      InvokeType sharp_type = info.GetSharpType();
      if (info.IsIntrinsic()) {
        // Nothing to do, if an intrinsic uses ArtMethod* it's in the slow-path - don't count it.
      } else if (!info.FastPath() || (sharp_type != kStatic && sharp_type != kDirect)) {
        // Nothing to do, the generated code or entrypoint uses method from the stack.
      } else if (info.DirectCode() != 0 && info.DirectMethod() != 0) {
        // Nothing to do, the generated code uses method from the stack.
      } else if (CanUseOpPcRelDexCacheArrayLoad()) {
        uses_pc_rel_load = true;
        dex_cache_array_offset = dex_cache_arrays_layout_.MethodOffset(mir->dalvikInsn.vB);
      } else {
        uses_method = true;
      }
      break;
    }

    case Instruction::NEW_INSTANCE:
    case Instruction::NEW_ARRAY:
    case Instruction::FILLED_NEW_ARRAY:
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      uses_method = true;
      break;
    case Instruction::FILL_ARRAY_DATA:
      // Nothing to do, the entrypoint uses method from the stack.
      break;
    case Instruction::THROW:
      // Nothing to do, the entrypoint uses method from the stack.
      break;

    case Instruction::SGET:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_OBJECT:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
    case Instruction::SPUT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_OBJECT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT: {
      const MirSFieldLoweringInfo& field_info = mir_graph_->GetSFieldLoweringInfo(mir);
      bool fast = IsInstructionSGet(static_cast<Instruction::Code>(opcode))
          ? field_info.FastGet()
          : field_info.FastPut();
      if (fast && (cu_->enable_debug & (1 << kDebugSlowFieldPath)) == 0) {
        if (!field_info.IsReferrersClass() && CanUseOpPcRelDexCacheArrayLoad()) {
          uses_pc_rel_load = true;  // And ignore method use in slow path.
          dex_cache_array_offset = dex_cache_arrays_layout_.TypeOffset(field_info.StorageIndex());
        } else {
          uses_method = true;
        }
      } else {
        // Nothing to do, the entrypoint uses method from the stack.
      }
      break;
    }

    default:
      break;
  }
  if (uses_method) {
    core_counts[SRegToPMap(mir_graph_->GetMethodLoc().s_reg_low)].count += weight;
  }
  if (uses_pc_rel_load) {
    if (pc_rel_temp_ != nullptr) {
      core_counts[SRegToPMap(pc_rel_temp_->s_reg_low)].count += weight;
      DCHECK_NE(dex_cache_array_offset, std::numeric_limits<uint32_t>::max());
      dex_cache_arrays_min_offset_ = std::min(dex_cache_arrays_min_offset_, dex_cache_array_offset);
    } else {
      // Nothing to do, using PC-relative addressing without promoting base PC to register.
    }
  }
}

/* USE SSA names to count references of base Dalvik v_regs. */
void Mir2Lir::CountRefs(RefCounts* core_counts, RefCounts* fp_counts, size_t num_regs) {
  for (int i = 0; i < mir_graph_->GetNumSSARegs(); i++) {
    RegLocation loc = mir_graph_->reg_location_[i];
    RefCounts* counts = loc.fp ? fp_counts : core_counts;
    int p_map_idx = SRegToPMap(loc.s_reg_low);
    int use_count = mir_graph_->GetUseCount(i);
    if (loc.fp) {
      if (loc.wide) {
        if (WideFPRsAreAliases()) {
          // Floats and doubles can be counted together.
          counts[p_map_idx].count += use_count;
        } else {
          // Treat doubles as a unit, using upper half of fp_counts array.
          counts[p_map_idx + num_regs].count += use_count;
        }
        i++;
      } else {
        counts[p_map_idx].count += use_count;
      }
    } else {
      if (loc.wide && WideGPRsAreAliases()) {
        i++;
      }
      if (!IsInexpensiveConstant(loc)) {
        counts[p_map_idx].count += use_count;
      }
    }
  }

  // Now analyze the ArtMethod* and pc_rel_temp_ uses.
  DCHECK_EQ(core_counts[SRegToPMap(mir_graph_->GetMethodLoc().s_reg_low)].count, 0);
  if (pc_rel_temp_ != nullptr) {
    DCHECK_EQ(core_counts[SRegToPMap(pc_rel_temp_->s_reg_low)].count, 0);
  }
  PreOrderDfsIterator iter(mir_graph_);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    if (bb->block_type == kDead) {
      continue;
    }
    uint32_t weight = mir_graph_->GetUseCountWeight(bb);
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      AnalyzeMIR(core_counts, mir, weight);
    }
  }
}

/* qsort callback function, sort descending */
static int SortCounts(const void *val1, const void *val2) {
  const Mir2Lir::RefCounts* op1 = reinterpret_cast<const Mir2Lir::RefCounts*>(val1);
  const Mir2Lir::RefCounts* op2 = reinterpret_cast<const Mir2Lir::RefCounts*>(val2);
  // Note that we fall back to sorting on reg so we get stable output on differing qsort
  // implementations (such as on host and target or between local host and build servers).
  // Note also that if a wide val1 and a non-wide val2 have the same count, then val1 always
  // ``loses'' (as STARTING_WIDE_SREG is or-ed in val1->s_reg).
  return (op1->count == op2->count)
          ? (op1->s_reg - op2->s_reg)
          : (op1->count < op2->count ? 1 : -1);
}

void Mir2Lir::DumpCounts(const RefCounts* arr, int size, const char* msg) {
  LOG(INFO) << msg;
  for (int i = 0; i < size; i++) {
    if ((arr[i].s_reg & STARTING_WIDE_SREG) != 0) {
      LOG(INFO) << "s_reg[64_" << (arr[i].s_reg & ~STARTING_WIDE_SREG) << "]: " << arr[i].count;
    } else {
      LOG(INFO) << "s_reg[32_" << arr[i].s_reg << "]: " << arr[i].count;
    }
  }
}

/*
 * Note: some portions of this code required even if the kPromoteRegs
 * optimization is disabled.
 */
void Mir2Lir::DoPromotion() {
  int num_regs = mir_graph_->GetNumOfCodeAndTempVRs();
  const int promotion_threshold = 1;
  // Allocate the promotion map - one entry for each Dalvik vReg or compiler temp
  promotion_map_ = arena_->AllocArray<PromotionMap>(num_regs, kArenaAllocRegAlloc);

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
  size_t core_reg_count_size = WideGPRsAreAliases() ? num_regs : num_regs * 2;
  size_t fp_reg_count_size = WideFPRsAreAliases() ? num_regs : num_regs * 2;
  RefCounts *core_regs = arena_->AllocArray<RefCounts>(core_reg_count_size, kArenaAllocRegAlloc);
  RefCounts *fp_regs = arena_->AllocArray<RefCounts>(fp_reg_count_size, kArenaAllocRegAlloc);
  // Set ssa names for original Dalvik registers
  for (int i = 0; i < num_regs; i++) {
    core_regs[i].s_reg = fp_regs[i].s_reg = i;
  }

  // Duplicate in upper half to represent possible wide starting sregs.
  for (size_t i = num_regs; i < fp_reg_count_size; i++) {
    fp_regs[i].s_reg = fp_regs[i - num_regs].s_reg | STARTING_WIDE_SREG;
  }
  for (size_t i = num_regs; i < core_reg_count_size; i++) {
    core_regs[i].s_reg = core_regs[i - num_regs].s_reg | STARTING_WIDE_SREG;
  }

  // Sum use counts of SSA regs by original Dalvik vreg.
  CountRefs(core_regs, fp_regs, num_regs);

  // Sort the count arrays
  qsort(core_regs, core_reg_count_size, sizeof(RefCounts), SortCounts);
  qsort(fp_regs, fp_reg_count_size, sizeof(RefCounts), SortCounts);

  if (cu_->verbose) {
    DumpCounts(core_regs, core_reg_count_size, "Core regs after sort");
    DumpCounts(fp_regs, fp_reg_count_size, "Fp regs after sort");
  }

  if (!(cu_->disable_opt & (1 << kPromoteRegs))) {
    // Promote fp regs
    for (size_t i = 0; (i < fp_reg_count_size) && (fp_regs[i].count >= promotion_threshold); i++) {
      int low_sreg = fp_regs[i].s_reg & ~STARTING_WIDE_SREG;
      size_t p_map_idx = SRegToPMap(low_sreg);
      RegStorage reg = RegStorage::InvalidReg();
      if (promotion_map_[p_map_idx].fp_location != kLocPhysReg) {
        // TODO: break out the Thumb2-specific code.
        if (cu_->instruction_set == kThumb2) {
          bool wide = fp_regs[i].s_reg & STARTING_WIDE_SREG;
          if (wide) {
            if (promotion_map_[p_map_idx + 1].fp_location != kLocPhysReg) {
              // Ignore result - if can't alloc double may still be able to alloc singles.
              AllocPreservedDouble(low_sreg);
            }
            // Continue regardless of success - might still be able to grab a single.
            continue;
          } else {
            reg = AllocPreservedSingle(low_sreg);
          }
        } else {
          reg = AllocPreservedFpReg(low_sreg);
        }
        if (!reg.Valid()) {
           break;  // No more left
        }
      }
    }

    // Promote core regs
    for (size_t i = 0; (i < core_reg_count_size) &&
         (core_regs[i].count >= promotion_threshold); i++) {
      int low_sreg = core_regs[i].s_reg & ~STARTING_WIDE_SREG;
      size_t p_map_idx = SRegToPMap(low_sreg);
      if (promotion_map_[p_map_idx].core_location != kLocPhysReg) {
        RegStorage reg = AllocPreservedCoreReg(low_sreg);
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
    int reg_num = curr->fp ? promotion_map_[p_map_idx].fp_reg : promotion_map_[p_map_idx].core_reg;
    bool wide = curr->wide || (cu_->target64 && curr->ref);
    RegStorage reg = RegStorage::InvalidReg();
    if (curr->fp && promotion_map_[p_map_idx].fp_location == kLocPhysReg) {
      if (wide && cu_->instruction_set == kThumb2) {
        if (promotion_map_[p_map_idx + 1].fp_location == kLocPhysReg) {
          int high_reg = promotion_map_[p_map_idx+1].fp_reg;
          // TODO: move target-specific restrictions out of here.
          if (((reg_num & 0x1) == 0) && ((reg_num + 1) == high_reg)) {
            reg = RegStorage::FloatSolo64(RegStorage::RegNum(reg_num) >> 1);
          }
        }
      } else {
        reg = wide ? RegStorage::FloatSolo64(reg_num) : RegStorage::FloatSolo32(reg_num);
      }
    } else if (!curr->fp && promotion_map_[p_map_idx].core_location == kLocPhysReg) {
      if (wide && !cu_->target64) {
        if (promotion_map_[p_map_idx + 1].core_location == kLocPhysReg) {
          int high_reg = promotion_map_[p_map_idx+1].core_reg;
          reg = RegStorage(RegStorage::k64BitPair, reg_num, high_reg);
        }
      } else {
        reg = wide ? RegStorage::Solo64(reg_num) : RegStorage::Solo32(reg_num);
      }
    }
    if (reg.Valid()) {
      curr->reg = reg;
      curr->location = kLocPhysReg;
      curr->home = true;
    }
  }
  if (cu_->verbose) {
    DumpPromotionMap();
  }
}

/* Returns sp-relative offset in bytes for a VReg */
int Mir2Lir::VRegOffset(int v_reg) {
  const DexFile::CodeItem* code_item = mir_graph_->GetCurrentDexCompilationUnit()->GetCodeItem();
  return StackVisitor::GetVRegOffsetFromQuickCode(code_item, core_spill_mask_,
                                                  fp_spill_mask_, frame_size_, v_reg,
                                                  cu_->instruction_set);
}

/* Returns sp-relative offset in bytes for a SReg */
int Mir2Lir::SRegOffset(int s_reg) {
  return VRegOffset(mir_graph_->SRegToVReg(s_reg));
}

/* Mark register usage state and return long retloc */
RegLocation Mir2Lir::GetReturnWide(RegisterClass reg_class) {
  RegLocation res;
  switch (reg_class) {
    case kRefReg: LOG(FATAL); break;
    case kFPReg: res = LocCReturnDouble(); break;
    default: res = LocCReturnWide(); break;
  }
  Clobber(res.reg);
  LockTemp(res.reg);
  MarkWide(res.reg);
  CheckRegLocation(res);
  return res;
}

RegLocation Mir2Lir::GetReturn(RegisterClass reg_class) {
  RegLocation res;
  switch (reg_class) {
    case kRefReg: res = LocCReturnRef(); break;
    case kFPReg: res = LocCReturnFloat(); break;
    default: res = LocCReturn(); break;
  }
  Clobber(res.reg);
  if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64) {
    MarkInUse(res.reg);
  } else {
    LockTemp(res.reg);
  }
  CheckRegLocation(res);
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
  UNUSED(s_reg);
  // For now.
  return true;
}

}  // namespace art
