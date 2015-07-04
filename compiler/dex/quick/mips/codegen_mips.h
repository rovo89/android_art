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

#ifndef ART_COMPILER_DEX_QUICK_MIPS_CODEGEN_MIPS_H_
#define ART_COMPILER_DEX_QUICK_MIPS_CODEGEN_MIPS_H_

#include "dex/compiler_ir.h"
#include "dex/quick/mir_to_lir.h"
#include "mips_lir.h"

namespace art {

struct CompilationUnit;

class MipsMir2Lir FINAL : public Mir2Lir {
 protected:
  class InToRegStorageMipsMapper : public InToRegStorageMapper {
   public:
    explicit InToRegStorageMipsMapper(Mir2Lir* m2l) : m2l_(m2l), cur_core_reg_(0) {}
    virtual RegStorage GetNextReg(ShortyArg arg);
    virtual void Reset() OVERRIDE {
      cur_core_reg_ = 0;
    }
   protected:
    Mir2Lir* m2l_;
   private:
    size_t cur_core_reg_;
  };

  class InToRegStorageMips64Mapper : public InToRegStorageMapper {
   public:
    explicit InToRegStorageMips64Mapper(Mir2Lir* m2l) : m2l_(m2l), cur_arg_reg_(0) {}
    virtual RegStorage GetNextReg(ShortyArg arg);
    virtual void Reset() OVERRIDE {
      cur_arg_reg_ = 0;
    }
   protected:
    Mir2Lir* m2l_;
   private:
    size_t cur_arg_reg_;
  };

  InToRegStorageMips64Mapper in_to_reg_storage_mips64_mapper_;
  InToRegStorageMipsMapper in_to_reg_storage_mips_mapper_;
  InToRegStorageMapper* GetResetedInToRegStorageMapper() OVERRIDE {
    InToRegStorageMapper* res;
    if (cu_->target64) {
      res = &in_to_reg_storage_mips64_mapper_;
    } else {
      res = &in_to_reg_storage_mips_mapper_;
    }
    res->Reset();
    return res;
  }

 public:
  MipsMir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);

  // Required for target - codegen utilities.
  bool SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div, RegLocation rl_src,
                          RegLocation rl_dest, int lit);
  bool EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) OVERRIDE;
  void GenMultiplyByConstantFloat(RegLocation rl_dest, RegLocation rl_src1, int32_t constant)
  OVERRIDE;
  void GenMultiplyByConstantDouble(RegLocation rl_dest, RegLocation rl_src1, int64_t constant)
  OVERRIDE;
  LIR* CheckSuspendUsingLoad() OVERRIDE;
  RegStorage LoadHelper(QuickEntrypointEnum trampoline) OVERRIDE;
  void ForceImplicitNullCheck(RegStorage reg, int opt_flags, bool is_wide);
  LIR* LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest, OpSize size,
                    VolatileKind is_volatile) OVERRIDE;
  LIR* LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest, int scale,
                       OpSize size) OVERRIDE;
  LIR* LoadConstantNoClobber(RegStorage r_dest, int value);
  LIR* LoadConstantWideNoClobber(RegStorage r_dest, int64_t value);
  LIR* LoadConstantWide(RegStorage r_dest, int64_t value);
  LIR* StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src, OpSize size,
                     VolatileKind is_volatile) OVERRIDE;
  LIR* StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src, int scale,
                        OpSize size) OVERRIDE;
  LIR* GenAtomic64Load(RegStorage r_base, int displacement, RegStorage r_dest);
  LIR* GenAtomic64Store(RegStorage r_base, int displacement, RegStorage r_src);

  /// @copydoc Mir2Lir::UnconditionallyMarkGCCard(RegStorage)
  void UnconditionallyMarkGCCard(RegStorage tgt_addr_reg) OVERRIDE;

  // Required for target - register utilities.
  RegStorage Solo64ToPair64(RegStorage reg);
  RegStorage Fp64ToSolo32(RegStorage reg);
  RegStorage TargetReg(SpecialTargetRegister reg);
  RegStorage TargetReg(SpecialTargetRegister reg, WideKind wide_kind) OVERRIDE;
  RegStorage TargetPtrReg(SpecialTargetRegister reg) OVERRIDE {
    return TargetReg(reg, cu_->target64 ? kWide : kNotWide);
  }
  RegLocation GetReturnAlt();
  RegLocation GetReturnWideAlt();
  RegLocation LocCReturn();
  RegLocation LocCReturnRef();
  RegLocation LocCReturnDouble();
  RegLocation LocCReturnFloat();
  RegLocation LocCReturnWide();
  ResourceMask GetRegMaskCommon(const RegStorage& reg) const OVERRIDE;
  void AdjustSpillMask();
  void ClobberCallerSave();
  void FreeCallTemps();
  void LockCallTemps();
  void CompilerInitializeRegAlloc();

  // Required for target - miscellaneous.
  void AssembleLIR();
  int AssignInsnOffsets();
  void AssignOffsets();
  AssemblerStatus AssembleInstructions(CodeOffset start_addr);
  void DumpResourceMask(LIR* lir, const ResourceMask& mask, const char* prefix) OVERRIDE;
  void SetupTargetResourceMasks(LIR* lir, uint64_t flags, ResourceMask* use_mask,
                                ResourceMask* def_mask) OVERRIDE;
  const char* GetTargetInstFmt(int opcode);
  const char* GetTargetInstName(int opcode);
  std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr);
  ResourceMask GetPCUseDefEncoding() const OVERRIDE;
  uint64_t GetTargetInstFlags(int opcode);
  size_t GetInsnSize(LIR* lir) OVERRIDE;
  bool IsUnconditionalBranch(LIR* lir);

  // Get the register class for load/store of a field.
  RegisterClass RegClassForFieldLoadStore(OpSize size, bool is_volatile) OVERRIDE;

  // Required for target - Dalvik-level generators.
  void GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                      RegLocation lr_shift);
  void GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                         RegLocation rl_src2, int flags);
  void GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array, RegLocation rl_index,
                   RegLocation rl_dest, int scale);
  void GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array, RegLocation rl_index,
                   RegLocation rl_src, int scale, bool card_mark);
  void GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                         RegLocation rl_shift, int flags);
  void GenArithOpDouble(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                        RegLocation rl_src2);
  void GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                       RegLocation rl_src2);
  void GenCmpFP(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                RegLocation rl_src2);
  void GenConversion(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src);
  bool GenInlinedAbsFloat(CallInfo* info) OVERRIDE;
  bool GenInlinedAbsDouble(CallInfo* info) OVERRIDE;
  bool GenInlinedCas(CallInfo* info, bool is_long, bool is_object);
  bool GenInlinedMinMax(CallInfo* info, bool is_min, bool is_long);
  bool GenInlinedSqrt(CallInfo* info);
  bool GenInlinedPeek(CallInfo* info, OpSize size);
  bool GenInlinedPoke(CallInfo* info, OpSize size);
  void GenIntToLong(RegLocation rl_dest, RegLocation rl_src) OVERRIDE;
  void GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                      RegLocation rl_src2, int flags) OVERRIDE;
  RegLocation GenDivRem(RegLocation rl_dest, RegStorage reg_lo, RegStorage reg_hi, bool is_div);
  RegLocation GenDivRemLit(RegLocation rl_dest, RegStorage reg_lo, int lit, bool is_div);
  void GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
  void GenDivZeroCheckWide(RegStorage reg);
  void GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method);
  void GenExitSequence();
  void GenSpecialExitSequence() OVERRIDE;
  void GenSpecialEntryForSuspend() OVERRIDE;
  void GenSpecialExitForSuspend() OVERRIDE;
  void GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias, bool is_double);
  void GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir);
  void GenSelect(BasicBlock* bb, MIR* mir);
  void GenSelectConst32(RegStorage left_op, RegStorage right_op, ConditionCode code,
                        int32_t true_val, int32_t false_val, RegStorage rs_dest,
                        RegisterClass dest_reg_class) OVERRIDE;
  bool GenMemBarrier(MemBarrierKind barrier_kind);
  void GenMoveException(RegLocation rl_dest);
  void GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result, int lit,
                                     int first_bit, int second_bit);
  void GenNegDouble(RegLocation rl_dest, RegLocation rl_src);
  void GenNegFloat(RegLocation rl_dest, RegLocation rl_src);
  void GenLargePackedSwitch(MIR* mir, uint32_t table_offset, RegLocation rl_src);
  void GenLargeSparseSwitch(MIR* mir, uint32_t table_offset, RegLocation rl_src);
  bool GenSpecialCase(BasicBlock* bb, MIR* mir, const InlineMethod& special);

  // Required for target - single operation generators.
  LIR* OpUnconditionalBranch(LIR* target);
  LIR* OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target);
  LIR* OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value, LIR* target);
  LIR* OpCondBranch(ConditionCode cc, LIR* target);
  LIR* OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target);
  LIR* OpFpRegCopy(RegStorage r_dest, RegStorage r_src);
  LIR* OpIT(ConditionCode cond, const char* guide);
  void OpEndIT(LIR* it);
  LIR* OpMem(OpKind op, RegStorage r_base, int disp);
  void OpPcRelLoad(RegStorage reg, LIR* target);
  LIR* OpReg(OpKind op, RegStorage r_dest_src);
  void OpRegCopy(RegStorage r_dest, RegStorage r_src);
  LIR* OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src);
  LIR* OpRegImm(OpKind op, RegStorage r_dest_src1, int value);
  LIR* OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2);
  LIR* OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset, MoveType move_type);
  LIR* OpMovMemReg(RegStorage r_base, int offset, RegStorage r_src, MoveType move_type);
  LIR* OpCondRegReg(OpKind op, ConditionCode cc, RegStorage r_dest, RegStorage r_src);
  LIR* OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src1, int value);
  LIR* OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1, RegStorage r_src2);
  LIR* OpTestSuspend(LIR* target);
  LIR* OpVldm(RegStorage r_base, int count);
  LIR* OpVstm(RegStorage r_base, int count);
  void OpRegCopyWide(RegStorage dest, RegStorage src);

  // TODO: collapse r_dest.
  LIR* LoadBaseDispBody(RegStorage r_base, int displacement, RegStorage r_dest, OpSize size);
  // TODO: collapse r_src.
  LIR* StoreBaseDispBody(RegStorage r_base, int displacement, RegStorage r_src, OpSize size);
  void SpillCoreRegs();
  void UnSpillCoreRegs();
  static const MipsEncodingMap EncodingMap[kMipsLast];
  bool InexpensiveConstantInt(int32_t value);
  bool InexpensiveConstantFloat(int32_t value);
  bool InexpensiveConstantLong(int64_t value);
  bool InexpensiveConstantDouble(int64_t value);

  bool WideGPRsAreAliases() const OVERRIDE {
    return cu_->target64;  // Wide GPRs are formed by pairing on mips32.
  }
  bool WideFPRsAreAliases() const OVERRIDE {
    return cu_->target64;  // Wide FPRs are formed by pairing on mips32.
  }

  LIR* InvokeTrampoline(OpKind op, RegStorage r_tgt, QuickEntrypointEnum trampoline) OVERRIDE;

  RegLocation GenDivRem(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2, bool is_div,
                        int flags) OVERRIDE;
  RegLocation GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit, bool is_div) OVERRIDE;
  NextCallInsn GetNextSDCallInsn() OVERRIDE;
  LIR* GenCallInsn(const MirMethodLoweringInfo& method_info) OVERRIDE;

  // Unimplemented intrinsics.
  bool GenInlinedCharAt(CallInfo* info ATTRIBUTE_UNUSED) OVERRIDE {
    return false;
  }
  bool GenInlinedAbsInt(CallInfo* info ATTRIBUTE_UNUSED) OVERRIDE {
    return false;
  }
  bool GenInlinedAbsLong(CallInfo* info ATTRIBUTE_UNUSED) OVERRIDE {
    return false;
  }
  bool GenInlinedIndexOf(CallInfo* info ATTRIBUTE_UNUSED, bool zero_based ATTRIBUTE_UNUSED)
  OVERRIDE {
    return false;
  }

  // True if isa is rev R6.
  const bool isaIsR6_;

  // True if floating point unit is 32bits.
  const bool fpuIs32Bit_;

 private:
  void GenNegLong(RegLocation rl_dest, RegLocation rl_src);
  void GenAddLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
  void GenSubLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);

  void ConvertShortToLongBranch(LIR* lir);

  // Mips64 specific long gen methods:
  void GenLongOp(OpKind op, RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
  void GenNotLong(RegLocation rl_dest, RegLocation rl_src);
  void GenMulLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
  void GenDivRemLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                     RegLocation rl_src2, bool is_div, int flags);
  void GenConversionCall(QuickEntrypointEnum trampoline, RegLocation rl_dest, RegLocation rl_src,
                         RegisterClass reg_class);
  RegStorage AllocPtrSizeTemp(bool required = true);

  /**
   * @param reg #RegStorage containing a Solo64 input register (e.g. @c a1 or @c d0).
   * @return A Solo32 with the same register number as the @p reg (e.g. @c a1 or @c f0).
   * @see As64BitReg
   */
  RegStorage As32BitReg(RegStorage reg) {
    DCHECK(!reg.IsPair());
    if ((kFailOnSizeError || kReportSizeError) && !reg.Is64Bit()) {
      if (kFailOnSizeError) {
        LOG(FATAL) << "Expected 64b register";
      } else {
        LOG(WARNING) << "Expected 64b register";
        return reg;
      }
    }
    RegStorage ret_val = RegStorage(RegStorage::k32BitSolo,
                                    reg.GetRawBits() & RegStorage::kRegTypeMask);
    DCHECK_EQ(GetRegInfo(reg)->FindMatchingView(RegisterInfo::k32SoloStorageMask)
              ->GetReg().GetReg(),
              ret_val.GetReg());
    return ret_val;
  }

  /**
   * @param reg #RegStorage containing a Solo32 input register (e.g. @c a1 or @c f0).
   * @return A Solo64 with the same register number as the @p reg (e.g. @c a1 or @c d0).
   */
  RegStorage As64BitReg(RegStorage reg) {
    DCHECK(!reg.IsPair());
    if ((kFailOnSizeError || kReportSizeError) && !reg.Is32Bit()) {
      if (kFailOnSizeError) {
        LOG(FATAL) << "Expected 32b register";
      } else {
        LOG(WARNING) << "Expected 32b register";
        return reg;
      }
    }
    RegStorage ret_val = RegStorage(RegStorage::k64BitSolo,
                                    reg.GetRawBits() & RegStorage::kRegTypeMask);
    DCHECK_EQ(GetRegInfo(reg)->FindMatchingView(RegisterInfo::k64SoloStorageMask)
              ->GetReg().GetReg(),
              ret_val.GetReg());
    return ret_val;
  }

  RegStorage Check64BitReg(RegStorage reg) {
    if ((kFailOnSizeError || kReportSizeError) && !reg.Is64Bit()) {
      if (kFailOnSizeError) {
        LOG(FATAL) << "Checked for 64b register";
      } else {
        LOG(WARNING) << "Checked for 64b register";
        return As64BitReg(reg);
      }
    }
    return reg;
  }
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_MIPS_CODEGEN_MIPS_H_
