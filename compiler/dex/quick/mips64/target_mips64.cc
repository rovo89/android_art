/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "codegen_mips64.h"

#include <inttypes.h>

#include <string>

#include "arch/mips64/instruction_set_features_mips64.h"
#include "backend_mips64.h"
#include "base/logging.h"
#include "dex/compiler_ir.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "driver/compiler_driver.h"
#include "mips64_lir.h"

namespace art {

static constexpr RegStorage core_regs_arr32[] =
    {rs_rZERO, rs_rAT, rs_rV0, rs_rV1, rs_rA0, rs_rA1, rs_rA2, rs_rA3, rs_rA4, rs_rA5, rs_rA6,
     rs_rA7, rs_rT0, rs_rT1, rs_rT2, rs_rT3, rs_rS0, rs_rS1, rs_rS2, rs_rS3, rs_rS4, rs_rS5,
     rs_rS6, rs_rS7, rs_rT8, rs_rT9, rs_rK0, rs_rK1, rs_rGP, rs_rSP, rs_rFP, rs_rRA};
static constexpr RegStorage core_regs_arr64[] =
    {rs_rZEROd, rs_rATd, rs_rV0d, rs_rV1d, rs_rA0d, rs_rA1d, rs_rA2d, rs_rA3d, rs_rA4d, rs_rA5d,
     rs_rA6d, rs_rA7d, rs_rT0d, rs_rT1d, rs_rT2d, rs_rT3d, rs_rS0d, rs_rS1d, rs_rS2d, rs_rS3d,
     rs_rS4d, rs_rS5d, rs_rS6d, rs_rS7d, rs_rT8d, rs_rT9d, rs_rK0d, rs_rK1d, rs_rGPd, rs_rSPd,
     rs_rFPd, rs_rRAd};
#if 0
// TODO: f24-f31 must be saved before calls and restored after.
static constexpr RegStorage sp_regs_arr[] =
    {rs_rF0, rs_rF1, rs_rF2, rs_rF3, rs_rF4, rs_rF5, rs_rF6, rs_rF7, rs_rF8, rs_rF9, rs_rF10,
     rs_rF11, rs_rF12, rs_rF13, rs_rF14, rs_rF15, rs_rF16, rs_rF17, rs_rF18, rs_rF19, rs_rF20,
     rs_rF21, rs_rF22, rs_rF23, rs_rF24, rs_rF25, rs_rF26, rs_rF27, rs_rF28, rs_rF29, rs_rF30,
     rs_rF31};
static constexpr RegStorage dp_regs_arr[] =
    {rs_rD0, rs_rD1, rs_rD2, rs_rD3, rs_rD4, rs_rD5, rs_rD6, rs_rD7, rs_rD8, rs_rD9, rs_rD10,
     rs_rD11, rs_rD12, rs_rD13, rs_rD14, rs_rD15, rs_rD16, rs_rD17, rs_rD18, rs_rD19, rs_rD20,
     rs_rD21, rs_rD22, rs_rD23, rs_rD24, rs_rD25, rs_rD26, rs_rD27, rs_rD28, rs_rD29, rs_rD30,
     rs_rD31};
#else
static constexpr RegStorage sp_regs_arr[] =
    {rs_rF0, rs_rF1, rs_rF2, rs_rF3, rs_rF4, rs_rF5, rs_rF6, rs_rF7, rs_rF8, rs_rF9, rs_rF10,
     rs_rF11, rs_rF12, rs_rF13, rs_rF14, rs_rF15, rs_rF16, rs_rF17, rs_rF18, rs_rF19, rs_rF20,
     rs_rF21, rs_rF22, rs_rF23};
static constexpr RegStorage dp_regs_arr[] =
    {rs_rD0, rs_rD1, rs_rD2, rs_rD3, rs_rD4, rs_rD5, rs_rD6, rs_rD7, rs_rD8, rs_rD9, rs_rD10,
     rs_rD11, rs_rD12, rs_rD13, rs_rD14, rs_rD15, rs_rD16, rs_rD17, rs_rD18, rs_rD19, rs_rD20,
     rs_rD21, rs_rD22, rs_rD23};
#endif
static constexpr RegStorage reserved_regs_arr32[] =
    {rs_rZERO, rs_rAT, rs_rS0, rs_rS1, rs_rT9, rs_rK0, rs_rK1, rs_rGP, rs_rSP, rs_rRA};
static constexpr RegStorage reserved_regs_arr64[] =
    {rs_rZEROd, rs_rATd, rs_rS0d, rs_rS1d, rs_rT9d, rs_rK0d, rs_rK1d, rs_rGPd, rs_rSPd, rs_rRAd};
static constexpr RegStorage core_temps_arr32[] =
    {rs_rV0, rs_rV1, rs_rA0, rs_rA1, rs_rA2, rs_rA3, rs_rA4, rs_rA5, rs_rA6, rs_rA7, rs_rT0,
     rs_rT1, rs_rT2, rs_rT3, rs_rT8};
static constexpr RegStorage core_temps_arr64[] =
    {rs_rV0d, rs_rV1d, rs_rA0d, rs_rA1d, rs_rA2d, rs_rA3d, rs_rA4d, rs_rA5d, rs_rA6d, rs_rA7d,
     rs_rT0d, rs_rT1d, rs_rT2d, rs_rT3d, rs_rT8d};
#if 0
// TODO: f24-f31 must be saved before calls and restored after.
static constexpr RegStorage sp_temps_arr[] =
    {rs_rF0, rs_rF1, rs_rF2, rs_rF3, rs_rF4, rs_rF5, rs_rF6, rs_rF7, rs_rF8, rs_rF9, rs_rF10,
     rs_rF11, rs_rF12, rs_rF13, rs_rF14, rs_rF15, rs_rF16, rs_rF17, rs_rF18, rs_rF19, rs_rF20,
     rs_rF21, rs_rF22, rs_rF23, rs_rF24, rs_rF25, rs_rF26, rs_rF27, rs_rF28, rs_rF29, rs_rF30,
     rs_rF31};
static constexpr RegStorage dp_temps_arr[] =
    {rs_rD0, rs_rD1, rs_rD2, rs_rD3, rs_rD4, rs_rD5, rs_rD6, rs_rD7, rs_rD8, rs_rD9, rs_rD10,
     rs_rD11, rs_rD12, rs_rD13, rs_rD14, rs_rD15, rs_rD16, rs_rD17, rs_rD18, rs_rD19, rs_rD20,
     rs_rD21, rs_rD22, rs_rD23, rs_rD24, rs_rD25, rs_rD26, rs_rD27, rs_rD28, rs_rD29, rs_rD30,
     rs_rD31};
#else
static constexpr RegStorage sp_temps_arr[] =
    {rs_rF0, rs_rF1, rs_rF2, rs_rF3, rs_rF4, rs_rF5, rs_rF6, rs_rF7, rs_rF8, rs_rF9, rs_rF10,
     rs_rF11, rs_rF12, rs_rF13, rs_rF14, rs_rF15, rs_rF16, rs_rF17, rs_rF18, rs_rF19, rs_rF20,
     rs_rF21, rs_rF22, rs_rF23};
static constexpr RegStorage dp_temps_arr[] =
    {rs_rD0, rs_rD1, rs_rD2, rs_rD3, rs_rD4, rs_rD5, rs_rD6, rs_rD7, rs_rD8, rs_rD9, rs_rD10,
     rs_rD11, rs_rD12, rs_rD13, rs_rD14, rs_rD15, rs_rD16, rs_rD17, rs_rD18, rs_rD19, rs_rD20,
     rs_rD21, rs_rD22, rs_rD23};
#endif

static constexpr ArrayRef<const RegStorage> empty_pool;
static constexpr ArrayRef<const RegStorage> core_regs32(core_regs_arr32);
static constexpr ArrayRef<const RegStorage> core_regs64(core_regs_arr64);
static constexpr ArrayRef<const RegStorage> sp_regs(sp_regs_arr);
static constexpr ArrayRef<const RegStorage> dp_regs(dp_regs_arr);
static constexpr ArrayRef<const RegStorage> reserved_regs32(reserved_regs_arr32);
static constexpr ArrayRef<const RegStorage> reserved_regs64(reserved_regs_arr64);
static constexpr ArrayRef<const RegStorage> core_temps32(core_temps_arr32);
static constexpr ArrayRef<const RegStorage> core_temps64(core_temps_arr64);
static constexpr ArrayRef<const RegStorage> sp_temps(sp_temps_arr);
static constexpr ArrayRef<const RegStorage> dp_temps(dp_temps_arr);

RegLocation Mips64Mir2Lir::LocCReturn() {
  return mips64_loc_c_return;
}

RegLocation Mips64Mir2Lir::LocCReturnRef() {
  return mips64_loc_c_return_ref;
}

RegLocation Mips64Mir2Lir::LocCReturnWide() {
  return mips64_loc_c_return_wide;
}

RegLocation Mips64Mir2Lir::LocCReturnFloat() {
  return mips64_loc_c_return_float;
}

RegLocation Mips64Mir2Lir::LocCReturnDouble() {
  return mips64_loc_c_return_double;
}

// Return a target-dependent special register.
RegStorage Mips64Mir2Lir::TargetReg(SpecialTargetRegister reg) {
  RegStorage res_reg;
  switch (reg) {
    case kSelf: res_reg = rs_rS1; break;
    case kSuspend: res_reg =  rs_rS0; break;
    case kLr: res_reg =  rs_rRA; break;
    case kPc: res_reg = RegStorage::InvalidReg(); break;
    case kSp: res_reg =  rs_rSP; break;
    case kArg0: res_reg = rs_rA0; break;
    case kArg1: res_reg = rs_rA1; break;
    case kArg2: res_reg = rs_rA2; break;
    case kArg3: res_reg = rs_rA3; break;
    case kArg4: res_reg = rs_rA4; break;
    case kArg5: res_reg = rs_rA5; break;
    case kArg6: res_reg = rs_rA6; break;
    case kArg7: res_reg = rs_rA7; break;
    case kFArg0: res_reg = rs_rF12; break;
    case kFArg1: res_reg = rs_rF13; break;
    case kFArg2: res_reg = rs_rF14; break;
    case kFArg3: res_reg = rs_rF15; break;
    case kFArg4: res_reg = rs_rF16; break;
    case kFArg5: res_reg = rs_rF17; break;
    case kFArg6: res_reg = rs_rF18; break;
    case kFArg7: res_reg = rs_rF19; break;
    case kRet0: res_reg = rs_rV0; break;
    case kRet1: res_reg = rs_rV1; break;
    case kInvokeTgt: res_reg = rs_rT9; break;
    case kHiddenArg: res_reg = rs_rT0; break;
    case kHiddenFpArg: res_reg = RegStorage::InvalidReg(); break;
    case kCount: res_reg = RegStorage::InvalidReg(); break;
    default: res_reg = RegStorage::InvalidReg();
  }
  return res_reg;
}

RegStorage Mips64Mir2Lir::InToRegStorageMips64Mapper::GetNextReg(ShortyArg arg) {
  const SpecialTargetRegister coreArgMappingToPhysicalReg[] =
      {kArg1, kArg2, kArg3, kArg4, kArg5, kArg6, kArg7};
  const size_t coreArgMappingToPhysicalRegSize = arraysize(coreArgMappingToPhysicalReg);
  const SpecialTargetRegister fpArgMappingToPhysicalReg[] =
      {kFArg1, kFArg2, kFArg3, kFArg4, kFArg5, kFArg6, kFArg7};
  const size_t fpArgMappingToPhysicalRegSize = arraysize(fpArgMappingToPhysicalReg);

  RegStorage result = RegStorage::InvalidReg();
  if (arg.IsFP()) {
    if (cur_arg_reg_ < fpArgMappingToPhysicalRegSize) {
      DCHECK(!arg.IsRef());
      result = m2l_->TargetReg(fpArgMappingToPhysicalReg[cur_arg_reg_++],
                               arg.IsWide() ? kWide : kNotWide);
    }
  } else {
    if (cur_arg_reg_ < coreArgMappingToPhysicalRegSize) {
      DCHECK(!(arg.IsWide() && arg.IsRef()));
      result = m2l_->TargetReg(coreArgMappingToPhysicalReg[cur_arg_reg_++],
                               arg.IsRef() ? kRef : (arg.IsWide() ? kWide : kNotWide));
    }
  }
  return result;
}

/*
 * Decode the register id.
 */
ResourceMask Mips64Mir2Lir::GetRegMaskCommon(const RegStorage& reg) const {
  return ResourceMask::Bit((reg.IsFloat() ? kMips64FPReg0 : 0) + reg.GetRegNum());
}

ResourceMask Mips64Mir2Lir::GetPCUseDefEncoding() const {
  return ResourceMask::Bit(kMips64RegPC);
}


void Mips64Mir2Lir::SetupTargetResourceMasks(LIR* lir, uint64_t flags, ResourceMask* use_mask,
                                             ResourceMask* def_mask) {
  DCHECK(!lir->flags.use_def_invalid);

  // Mips64-specific resource map setup here.
  if (flags & REG_DEF_SP) {
    def_mask->SetBit(kMips64RegSP);
  }

  if (flags & REG_USE_SP) {
    use_mask->SetBit(kMips64RegSP);
  }

  if (flags & REG_DEF_LR) {
    def_mask->SetBit(kMips64RegLR);
  }
}

/* For dumping instructions */
#define MIPS64_REG_COUNT 32
static const char *mips64_reg_name[MIPS64_REG_COUNT] = {
  "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
  "a4", "a5", "a6", "a7", "t0", "t1", "t2", "t3",
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
  "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

/*
 * Interpret a format string and build a string no longer than size
 * See format key in assemble_mips64.cc.
 */
std::string Mips64Mir2Lir::BuildInsnString(const char *fmt, LIR *lir, unsigned char* base_addr) {
  std::string buf;
  int i;
  const char *fmt_end = &fmt[strlen(fmt)];
  char tbuf[256];
  char nc;
  while (fmt < fmt_end) {
    int operand;
    if (*fmt == '!') {
      fmt++;
      DCHECK_LT(fmt, fmt_end);
      nc = *fmt++;
      if (nc == '!') {
        strcpy(tbuf, "!");
      } else {
        DCHECK_LT(fmt, fmt_end);
        DCHECK_LT(static_cast<unsigned>(nc-'0'), 4u);
        operand = lir->operands[nc-'0'];
        switch (*fmt++) {
          case 'b':
            strcpy(tbuf, "0000");
            for (i = 3; i >= 0; i--) {
              tbuf[i] += operand & 1;
              operand >>= 1;
            }
            break;
          case 's':
            snprintf(tbuf, arraysize(tbuf), "$f%d", RegStorage::RegNum(operand));
            break;
          case 'S':
            DCHECK_EQ(RegStorage::RegNum(operand) & 1, 0);
            snprintf(tbuf, arraysize(tbuf), "$f%d", RegStorage::RegNum(operand));
            break;
          case 'h':
            snprintf(tbuf, arraysize(tbuf), "%04x", operand);
            break;
          case 'M':
          case 'd':
            snprintf(tbuf, arraysize(tbuf), "%d", operand);
            break;
          case 'D':
            snprintf(tbuf, arraysize(tbuf), "%d", operand+1);
            break;
          case 'E':
            snprintf(tbuf, arraysize(tbuf), "%d", operand*4);
            break;
          case 'F':
            snprintf(tbuf, arraysize(tbuf), "%d", operand*2);
            break;
          case 't':
            snprintf(tbuf, arraysize(tbuf), "0x%08" PRIxPTR " (L%p)",
                     reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4 + (operand << 1),
                     lir->target);
            break;
          case 'T':
            snprintf(tbuf, arraysize(tbuf), "0x%08x", operand << 2);
            break;
          case 'u': {
            int offset_1 = lir->operands[0];
            int offset_2 = NEXT_LIR(lir)->operands[0];
            uintptr_t target =
                (((reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4) & ~3) +
                    (offset_1 << 21 >> 9) + (offset_2 << 1)) & 0xfffffffc;
            snprintf(tbuf, arraysize(tbuf), "%p", reinterpret_cast<void*>(target));
            break;
          }

          /* Nothing to print for BLX_2 */
          case 'v':
            strcpy(tbuf, "see above");
            break;
          case 'r':
            DCHECK(operand >= 0 && operand < MIPS64_REG_COUNT);
            strcpy(tbuf, mips64_reg_name[operand]);
            break;
          case 'N':
            // Placeholder for delay slot handling
            strcpy(tbuf, ";  nop");
            break;
          default:
            strcpy(tbuf, "DecodeError");
            break;
        }
        buf += tbuf;
      }
    } else {
      buf += *fmt++;
    }
  }
  return buf;
}

// FIXME: need to redo resource maps for MIPS64 - fix this at that time.
void Mips64Mir2Lir::DumpResourceMask(LIR *mips64_lir, const ResourceMask& mask, const char *prefix) {
  char buf[256];
  buf[0] = 0;

  if (mask.Equals(kEncodeAll)) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kMips64RegEnd; i++) {
      if (mask.HasBit(i)) {
        snprintf(num, arraysize(num), "%d ", i);
        strcat(buf, num);
      }
    }

    if (mask.HasBit(ResourceMask::kCCode)) {
      strcat(buf, "cc ");
    }
    if (mask.HasBit(ResourceMask::kFPStatus)) {
      strcat(buf, "fpcc ");
    }
    // Memory bits.
    if (mips64_lir && (mask.HasBit(ResourceMask::kDalvikReg))) {
      snprintf(buf + strlen(buf), arraysize(buf) - strlen(buf), "dr%d%s",
               DECODE_ALIAS_INFO_REG(mips64_lir->flags.alias_info),
               DECODE_ALIAS_INFO_WIDE(mips64_lir->flags.alias_info) ? "(+1)" : "");
    }
    if (mask.HasBit(ResourceMask::kLiteral)) {
      strcat(buf, "lit ");
    }

    if (mask.HasBit(ResourceMask::kHeapRef)) {
      strcat(buf, "heap ");
    }
    if (mask.HasBit(ResourceMask::kMustNotAlias)) {
      strcat(buf, "noalias ");
    }
  }
  if (buf[0]) {
    LOG(INFO) << prefix << ": " <<  buf;
  }
}

/*
 * TUNING: is true leaf?  Can't just use METHOD_IS_LEAF to determine as some
 * instructions might call out to C/assembly helper functions.  Until
 * machinery is in place, always spill lr.
 */

void Mips64Mir2Lir::AdjustSpillMask() {
  core_spill_mask_ |= (1 << rs_rRA.GetRegNum());
  num_core_spills_++;
}

/* Clobber all regs that might be used by an external C call */
void Mips64Mir2Lir::ClobberCallerSave() {
  Clobber(rs_rZEROd);
  Clobber(rs_rATd);
  Clobber(rs_rV0d);
  Clobber(rs_rV1d);
  Clobber(rs_rA0d);
  Clobber(rs_rA1d);
  Clobber(rs_rA2d);
  Clobber(rs_rA3d);
  Clobber(rs_rA4d);
  Clobber(rs_rA5d);
  Clobber(rs_rA6d);
  Clobber(rs_rA7d);
  Clobber(rs_rT0d);
  Clobber(rs_rT1d);
  Clobber(rs_rT2d);
  Clobber(rs_rT3d);
  Clobber(rs_rT8d);
  Clobber(rs_rT9d);
  Clobber(rs_rK0d);
  Clobber(rs_rK1d);
  Clobber(rs_rGPd);
  Clobber(rs_rFPd);
  Clobber(rs_rRAd);

  Clobber(rs_rF0);
  Clobber(rs_rF1);
  Clobber(rs_rF2);
  Clobber(rs_rF3);
  Clobber(rs_rF4);
  Clobber(rs_rF5);
  Clobber(rs_rF6);
  Clobber(rs_rF7);
  Clobber(rs_rF8);
  Clobber(rs_rF9);
  Clobber(rs_rF10);
  Clobber(rs_rF11);
  Clobber(rs_rF12);
  Clobber(rs_rF13);
  Clobber(rs_rF14);
  Clobber(rs_rF15);
  Clobber(rs_rD0);
  Clobber(rs_rD1);
  Clobber(rs_rD2);
  Clobber(rs_rD3);
  Clobber(rs_rD4);
  Clobber(rs_rD5);
  Clobber(rs_rD6);
  Clobber(rs_rD7);
}

RegLocation Mips64Mir2Lir::GetReturnWideAlt() {
  UNIMPLEMENTED(FATAL) << "No GetReturnWideAlt for MIPS64";
  RegLocation res = LocCReturnWide();
  return res;
}

RegLocation Mips64Mir2Lir::GetReturnAlt() {
  UNIMPLEMENTED(FATAL) << "No GetReturnAlt for MIPS64";
  RegLocation res = LocCReturn();
  return res;
}

/* To be used when explicitly managing register use */
void Mips64Mir2Lir::LockCallTemps() {
  LockTemp(rs_rMIPS64_ARG0);
  LockTemp(rs_rMIPS64_ARG1);
  LockTemp(rs_rMIPS64_ARG2);
  LockTemp(rs_rMIPS64_ARG3);
  LockTemp(rs_rMIPS64_ARG4);
  LockTemp(rs_rMIPS64_ARG5);
  LockTemp(rs_rMIPS64_ARG6);
  LockTemp(rs_rMIPS64_ARG7);
}

/* To be used when explicitly managing register use */
void Mips64Mir2Lir::FreeCallTemps() {
  FreeTemp(rs_rMIPS64_ARG0);
  FreeTemp(rs_rMIPS64_ARG1);
  FreeTemp(rs_rMIPS64_ARG2);
  FreeTemp(rs_rMIPS64_ARG3);
  FreeTemp(rs_rMIPS64_ARG4);
  FreeTemp(rs_rMIPS64_ARG5);
  FreeTemp(rs_rMIPS64_ARG6);
  FreeTemp(rs_rMIPS64_ARG7);
  FreeTemp(TargetReg(kHiddenArg));
}

bool Mips64Mir2Lir::GenMemBarrier(MemBarrierKind barrier_kind ATTRIBUTE_UNUSED) {
  if (cu_->compiler_driver->GetInstructionSetFeatures()->IsSmp()) {
    NewLIR1(kMips64Sync, 0 /* Only stype currently supported */);
    return true;
  } else {
    return false;
  }
}

void Mips64Mir2Lir::CompilerInitializeRegAlloc() {
  reg_pool_.reset(new (arena_) RegisterPool(this, arena_, core_regs32, core_regs64 , sp_regs,
                                        dp_regs, reserved_regs32, reserved_regs64,
                                        core_temps32, core_temps64, sp_temps,
                                        dp_temps));

  // Target-specific adjustments.

  // Alias single precision floats to appropriate half of overlapping double.
  for (RegisterInfo* info : reg_pool_->sp_regs_) {
    int sp_reg_num = info->GetReg().GetRegNum();
    int dp_reg_num = sp_reg_num;
    RegStorage dp_reg = RegStorage::Solo64(RegStorage::kFloatingPoint | dp_reg_num);
    RegisterInfo* dp_reg_info = GetRegInfo(dp_reg);
    // Double precision register's master storage should refer to itself.
    DCHECK_EQ(dp_reg_info, dp_reg_info->Master());
    // Redirect single precision's master storage to master.
    info->SetMaster(dp_reg_info);
    // Singles should show a single 32-bit mask bit, at first referring to the low half.
    DCHECK_EQ(info->StorageMask(), 0x1U);
  }

  // Alias 32bit W registers to corresponding 64bit X registers.
  for (RegisterInfo* info : reg_pool_->core_regs_) {
    int d_reg_num = info->GetReg().GetRegNum();
    RegStorage d_reg = RegStorage::Solo64(d_reg_num);
    RegisterInfo* d_reg_info = GetRegInfo(d_reg);
    // 64bit D register's master storage should refer to itself.
    DCHECK_EQ(d_reg_info, d_reg_info->Master());
    // Redirect 32bit master storage to 64bit D.
    info->SetMaster(d_reg_info);
    // 32bit should show a single 32-bit mask bit, at first referring to the low half.
    DCHECK_EQ(info->StorageMask(), 0x1U);
  }

  // Don't start allocating temps at r0/s0/d0 or you may clobber return regs in early-exit methods.
  // TODO: adjust when we roll to hard float calling convention.
  reg_pool_->next_core_reg_ = 2;
  reg_pool_->next_sp_reg_ = 2;
  reg_pool_->next_dp_reg_ = 1;
}

/*
 * In the Arm code a it is typical to use the link register
 * to hold the target address.  However, for Mips64 we must
 * ensure that all branch instructions can be restarted if
 * there is a trap in the shadow.  Allocate a temp register.
 */
RegStorage Mips64Mir2Lir::LoadHelper(QuickEntrypointEnum trampoline) {
  // NOTE: native pointer.
  LoadWordDisp(rs_rMIPS64_SELF, GetThreadOffset<8>(trampoline).Int32Value(), rs_rT9d);
  return rs_rT9d;
}

LIR* Mips64Mir2Lir::CheckSuspendUsingLoad() {
  RegStorage tmp = AllocTemp();
  // NOTE: native pointer.
  LoadWordDisp(rs_rMIPS64_SELF, Thread::ThreadSuspendTriggerOffset<8>().Int32Value(), tmp);
  LIR *inst = LoadWordDisp(tmp, 0, tmp);
  FreeTemp(tmp);
  return inst;
}

LIR* Mips64Mir2Lir::GenAtomic64Load(RegStorage r_base, int displacement, RegStorage r_dest) {
  DCHECK(!r_dest.IsFloat());  // See RegClassForFieldLoadStore().
  ClobberCallerSave();
  LockCallTemps();  // Using fixed registers.
  RegStorage reg_ptr = TargetReg(kArg0);
  OpRegRegImm(kOpAdd, reg_ptr, r_base, displacement);
  RegStorage r_tgt = LoadHelper(kQuickA64Load);
  LIR *ret = OpReg(kOpBlx, r_tgt);
  OpRegCopy(r_dest, TargetReg(kRet0));
  return ret;
}

LIR* Mips64Mir2Lir::GenAtomic64Store(RegStorage r_base, int displacement, RegStorage r_src) {
  DCHECK(!r_src.IsFloat());  // See RegClassForFieldLoadStore().
  DCHECK(!r_src.IsPair());
  ClobberCallerSave();
  LockCallTemps();  // Using fixed registers.
  RegStorage temp_ptr = AllocTemp();
  OpRegRegImm(kOpAdd, temp_ptr, r_base, displacement);
  RegStorage temp_value = AllocTemp();
  OpRegCopy(temp_value, r_src);
  OpRegCopy(TargetReg(kArg0), temp_ptr);
  OpRegCopy(TargetReg(kArg1), temp_value);
  FreeTemp(temp_ptr);
  FreeTemp(temp_value);
  RegStorage r_tgt = LoadHelper(kQuickA64Store);
  return OpReg(kOpBlx, r_tgt);
}

void Mips64Mir2Lir::SpillCoreRegs() {
  if (num_core_spills_ == 0) {
    return;
  }
  uint32_t mask = core_spill_mask_;
  // Start saving from offset 0 so that ra ends up on the top of the frame.
  int offset = 0;
  OpRegImm(kOpSub, rs_rSPd, num_core_spills_ * 8);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      StoreWordDisp(rs_rMIPS64_SP, offset, RegStorage::Solo64(reg));
      offset += 8;
    }
  }
}

void Mips64Mir2Lir::UnSpillCoreRegs() {
  if (num_core_spills_ == 0) {
    return;
  }
  uint32_t mask = core_spill_mask_;
  int offset = frame_size_ - num_core_spills_ * 8;
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      LoadWordDisp(rs_rMIPS64_SP, offset, RegStorage::Solo64(reg));
      offset += 8;
    }
  }
  OpRegImm(kOpAdd, rs_rSPd, frame_size_);
}

bool Mips64Mir2Lir::IsUnconditionalBranch(LIR* lir) {
  return (lir->opcode == kMips64B);
}

RegisterClass Mips64Mir2Lir::RegClassForFieldLoadStore(OpSize size, bool is_volatile) {
  if (UNLIKELY(is_volatile)) {
    // On Mips64, atomic 64-bit load/store requires a core register.
    // Smaller aligned load/store is atomic for both core and fp registers.
    if (size == k64 || size == kDouble) {
      return kCoreReg;
    }
  }
  // TODO: Verify that both core and fp registers are suitable for smaller sizes.
  return RegClassBySize(size);
}

Mips64Mir2Lir::Mips64Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena)
    : Mir2Lir(cu, mir_graph, arena), in_to_reg_storage_mips64_mapper_(this) {
  for (int i = 0; i < kMips64Last; i++) {
    DCHECK_EQ(Mips64Mir2Lir::EncodingMap[i].opcode, i)
        << "Encoding order for " << Mips64Mir2Lir::EncodingMap[i].name
        << " is wrong: expecting " << i << ", seeing "
        << static_cast<int>(Mips64Mir2Lir::EncodingMap[i].opcode);
  }
}

Mir2Lir* Mips64CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                             ArenaAllocator* const arena) {
  return new Mips64Mir2Lir(cu, mir_graph, arena);
}

uint64_t Mips64Mir2Lir::GetTargetInstFlags(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return Mips64Mir2Lir::EncodingMap[opcode].flags;
}

const char* Mips64Mir2Lir::GetTargetInstName(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return Mips64Mir2Lir::EncodingMap[opcode].name;
}

const char* Mips64Mir2Lir::GetTargetInstFmt(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return Mips64Mir2Lir::EncodingMap[opcode].fmt;
}

void Mips64Mir2Lir::GenBreakpoint(int code) {
    NewLIR1(kMips64Break, code);
}

}  // namespace art
