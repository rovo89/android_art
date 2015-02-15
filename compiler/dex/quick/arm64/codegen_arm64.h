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

#ifndef ART_COMPILER_DEX_QUICK_ARM64_CODEGEN_ARM64_H_
#define ART_COMPILER_DEX_QUICK_ARM64_CODEGEN_ARM64_H_

#include "arm64_lir.h"
#include "dex/compiler_internals.h"

#include <map>

#ifdef QC_STRONG
#define QC_WEAK
#else
#define QC_WEAK __attribute__((weak))
#endif

namespace art {
class QCArm64Mir2Lir;
class Arm64Mir2Lir FINAL : public Mir2Lir {
 protected:
  // TODO: consolidate 64-bit target support.
  class InToRegStorageMapper {
   public:
    virtual RegStorage GetNextReg(bool is_double_or_float, bool is_wide, bool is_ref) = 0;
    virtual ~InToRegStorageMapper() {}
  };

  class InToRegStorageArm64Mapper : public InToRegStorageMapper {
   public:
    InToRegStorageArm64Mapper() : cur_core_reg_(0), cur_fp_reg_(0) {}
    virtual ~InToRegStorageArm64Mapper() {}
    virtual RegStorage GetNextReg(bool is_double_or_float, bool is_wide, bool is_ref);
   private:
    int cur_core_reg_;
    int cur_fp_reg_;
  };

  class InToRegStorageMapping {
   public:
    InToRegStorageMapping() : max_mapped_in_(0), is_there_stack_mapped_(false),
    initialized_(false) {}
    void Initialize(RegLocation* arg_locs, int count, InToRegStorageMapper* mapper);
    int GetMaxMappedIn() { return max_mapped_in_; }
    bool IsThereStackMapped() { return is_there_stack_mapped_; }
    RegStorage Get(int in_position);
    bool IsInitialized() { return initialized_; }
   private:
    std::map<int, RegStorage> mapping_;
    int max_mapped_in_;
    bool is_there_stack_mapped_;
    bool initialized_;
  };

 public:
  Arm64Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);

  // Required for target - codegen helpers.
  bool SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div, RegLocation rl_src,
                          RegLocation rl_dest, int lit) OVERRIDE;
  bool HandleEasyDivRem(Instruction::Code dalvik_opcode, bool is_div,
                        RegLocation rl_src, RegLocation rl_dest, int lit) OVERRIDE;
  bool HandleEasyDivRem64(Instruction::Code dalvik_opcode, bool is_div,
                          RegLocation rl_src, RegLocation rl_dest, int64_t lit);
  bool EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) OVERRIDE;
  LIR* CheckSuspendUsingLoad() OVERRIDE;
  RegStorage LoadHelper(QuickEntrypointEnum trampoline) OVERRIDE;
  LIR* LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                    OpSize size, VolatileKind is_volatile) OVERRIDE;
  LIR* LoadRefDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                   VolatileKind is_volatile) OVERRIDE;
  LIR* LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest, int scale,
                       OpSize size) OVERRIDE;
  LIR* LoadRefIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest, int scale)
      OVERRIDE;
  LIR* LoadConstantNoClobber(RegStorage r_dest, int value) OVERRIDE;
  LIR* LoadConstantWide(RegStorage r_dest, int64_t value) OVERRIDE;
  LIR* StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src, OpSize size,
                     VolatileKind is_volatile) OVERRIDE;
  LIR* StoreRefDisp(RegStorage r_base, int displacement, RegStorage r_src, VolatileKind is_volatile)
      OVERRIDE;
  LIR* StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src, int scale,
                        OpSize size) OVERRIDE;
  LIR* StoreRefIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src, int scale) OVERRIDE;
  void MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg) OVERRIDE;
  LIR* OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                         int offset, int check_value, LIR* target, LIR** compare) OVERRIDE;

  // Required for target - register utilities.
  RegStorage TargetReg(SpecialTargetRegister reg) OVERRIDE;
  RegStorage TargetReg(SpecialTargetRegister symbolic_reg, WideKind wide_kind) OVERRIDE {
    if (wide_kind == kWide || wide_kind == kRef) {
      return As64BitReg(TargetReg(symbolic_reg));
    } else {
      return Check32BitReg(TargetReg(symbolic_reg));
    }
  }
  RegStorage TargetPtrReg(SpecialTargetRegister symbolic_reg) OVERRIDE {
    return As64BitReg(TargetReg(symbolic_reg));
  }
  RegStorage GetArgMappingToPhysicalReg(int arg_num) OVERRIDE;
  RegLocation GetReturnAlt() OVERRIDE;
  RegLocation GetReturnWideAlt() OVERRIDE;
  RegLocation LocCReturn() OVERRIDE;
  RegLocation LocCReturnRef() OVERRIDE;
  RegLocation LocCReturnDouble() OVERRIDE;
  RegLocation LocCReturnFloat() OVERRIDE;
  RegLocation LocCReturnWide() OVERRIDE;
  ResourceMask GetRegMaskCommon(const RegStorage& reg) const OVERRIDE;
  void AdjustSpillMask() OVERRIDE;
  void ClobberCallerSave() OVERRIDE;
  void FreeCallTemps() OVERRIDE;
  void LockCallTemps() OVERRIDE;
  void CompilerInitializeRegAlloc() OVERRIDE;

  // Required for target - miscellaneous.
  void AssembleLIR() OVERRIDE;
  void DumpResourceMask(LIR* lir, const ResourceMask& mask, const char* prefix) OVERRIDE;
  void SetupTargetResourceMasks(LIR* lir, uint64_t flags,
                                ResourceMask* use_mask, ResourceMask* def_mask) OVERRIDE;
  const char* GetTargetInstFmt(int opcode) OVERRIDE;
  const char* GetTargetInstName(int opcode) OVERRIDE;
  std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr) OVERRIDE;
  ResourceMask GetPCUseDefEncoding() const OVERRIDE;
  uint64_t GetTargetInstFlags(int opcode) OVERRIDE;
  size_t GetInsnSize(LIR* lir) OVERRIDE;
  bool IsUnconditionalBranch(LIR* lir) OVERRIDE;

  // Get the register class for load/store of a field.
  RegisterClass RegClassForFieldLoadStore(OpSize size, bool is_volatile) OVERRIDE;

  // Required for target - Dalvik-level generators.
  void GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                      RegLocation lr_shift) OVERRIDE;
  void GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                         RegLocation rl_src2) OVERRIDE;
  void GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array, RegLocation rl_index,
                   RegLocation rl_dest, int scale) OVERRIDE;
  void GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array, RegLocation rl_index,
                   RegLocation rl_src, int scale, bool card_mark) OVERRIDE;
  void GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                         RegLocation rl_shift) OVERRIDE;
  void GenArithOpDouble(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                        RegLocation rl_src2) OVERRIDE;
  void GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                       RegLocation rl_src2) OVERRIDE;
  void GenCmpFP(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                RegLocation rl_src2) OVERRIDE;
  void GenConversion(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src) OVERRIDE;
  bool GenInlinedReverseBits(CallInfo* info, OpSize size) OVERRIDE;
  bool GenInlinedAbsFloat(CallInfo* info) OVERRIDE;
  bool GenInlinedAbsDouble(CallInfo* info) OVERRIDE;
  bool GenInlinedCas(CallInfo* info, bool is_long, bool is_object) OVERRIDE;
  bool GenInlinedMinMax(CallInfo* info, bool is_min, bool is_long) OVERRIDE;
  bool GenInlinedMinMaxFP(CallInfo* info, bool is_min, bool is_double) OVERRIDE;
  bool GenInlinedSqrt(CallInfo* info) OVERRIDE;
  bool GenInlinedCeil(CallInfo* info) OVERRIDE;
  bool GenInlinedFloor(CallInfo* info) OVERRIDE;
  bool GenInlinedRint(CallInfo* info) OVERRIDE;
  bool GenInlinedRound(CallInfo* info, bool is_double) OVERRIDE;
  bool GenInlinedPeek(CallInfo* info, OpSize size) OVERRIDE;
  bool GenInlinedPoke(CallInfo* info, OpSize size) OVERRIDE;
  bool GenInlinedAbsLong(CallInfo* info) OVERRIDE;
  bool GenInlinedArrayCopyCharArray(CallInfo* info) OVERRIDE;
  void GenIntToLong(RegLocation rl_dest, RegLocation rl_src) OVERRIDE;
  void GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                      RegLocation rl_src2) OVERRIDE;
  RegLocation GenDivRem(RegLocation rl_dest, RegStorage reg_lo, RegStorage reg_hi, bool is_div)
      OVERRIDE;
  RegLocation GenDivRemLit(RegLocation rl_dest, RegStorage reg_lo, int lit, bool is_div)
      OVERRIDE;
  void GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2)  OVERRIDE;
  void GenDivZeroCheckWide(RegStorage reg) OVERRIDE;
  void GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method) OVERRIDE;
  void GenExitSequence() OVERRIDE;
  void GenSpecialExitSequence() OVERRIDE;
  void GenFillArrayData(DexOffset table_offset, RegLocation rl_src) OVERRIDE;
  void GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias, bool is_double) OVERRIDE;
  void GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) OVERRIDE;
  void GenSelect(BasicBlock* bb, MIR* mir) OVERRIDE;
  void GenSelectConst32(RegStorage left_op, RegStorage right_op, ConditionCode code,
                        int32_t true_val, int32_t false_val, RegStorage rs_dest,
                        int dest_reg_class) OVERRIDE;

  bool GenMemBarrier(MemBarrierKind barrier_kind) OVERRIDE;
  void GenMonitorEnter(int opt_flags, RegLocation rl_src) OVERRIDE;
  void GenMonitorExit(int opt_flags, RegLocation rl_src) OVERRIDE;
  void GenMoveException(RegLocation rl_dest) OVERRIDE;
  void GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result, int lit,
                                     int first_bit, int second_bit) OVERRIDE;
  void GenNegDouble(RegLocation rl_dest, RegLocation rl_src) OVERRIDE;
  void GenNegFloat(RegLocation rl_dest, RegLocation rl_src) OVERRIDE;
  void GenLargePackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) OVERRIDE;
  void GenLargeSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) OVERRIDE;

  // Required for target - single operation generators.
  LIR* OpUnconditionalBranch(LIR* target) OVERRIDE;
  LIR* OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) OVERRIDE;
  LIR* OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value, LIR* target) OVERRIDE;
  LIR* OpCondBranch(ConditionCode cc, LIR* target) OVERRIDE;
  LIR* OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) OVERRIDE;
  LIR* OpFpRegCopy(RegStorage r_dest, RegStorage r_src) OVERRIDE;
  LIR* OpIT(ConditionCode cond, const char* guide) OVERRIDE;
  void OpEndIT(LIR* it) OVERRIDE;
  LIR* OpMem(OpKind op, RegStorage r_base, int disp) OVERRIDE;
  LIR* OpPcRelLoad(RegStorage reg, LIR* target) OVERRIDE;
  LIR* OpReg(OpKind op, RegStorage r_dest_src) OVERRIDE;
  void OpRegCopy(RegStorage r_dest, RegStorage r_src) OVERRIDE;
  LIR* OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) OVERRIDE;
  LIR* OpRegImm(OpKind op, RegStorage r_dest_src1, int value) OVERRIDE;
  LIR* OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2) OVERRIDE;
  LIR* OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset, MoveType move_type) OVERRIDE;
  LIR* OpMovMemReg(RegStorage r_base, int offset, RegStorage r_src, MoveType move_type) OVERRIDE;
  LIR* OpCondRegReg(OpKind op, ConditionCode cc, RegStorage r_dest, RegStorage r_src) OVERRIDE;
  LIR* OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src1, int value) OVERRIDE;
  LIR* OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1, RegStorage r_src2) OVERRIDE;
  LIR* OpTestSuspend(LIR* target) OVERRIDE;
  LIR* OpVldm(RegStorage r_base, int count) OVERRIDE;
  LIR* OpVstm(RegStorage r_base, int count) OVERRIDE;
  void OpRegCopyWide(RegStorage dest, RegStorage src) OVERRIDE;

  bool InexpensiveConstantInt(int32_t value) OVERRIDE;
  bool InexpensiveConstantInt(int32_t value, Instruction::Code opcode) OVERRIDE;
  bool InexpensiveConstantFloat(int32_t value) OVERRIDE;
  bool InexpensiveConstantLong(int64_t value) OVERRIDE;
  bool InexpensiveConstantDouble(int64_t value) OVERRIDE;

  void FlushIns(RegLocation* ArgLocs, RegLocation rl_method) OVERRIDE;

  int GenDalvikArgsNoRange(CallInfo* info, int call_state, LIR** pcrLabel,
                           NextCallInsn next_call_insn,
                           const MethodReference& target_method,
                           uint32_t vtable_idx,
                           uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                           bool skip_this) OVERRIDE;

  int GenDalvikArgsRange(CallInfo* info, int call_state, LIR** pcrLabel,
                         NextCallInsn next_call_insn,
                         const MethodReference& target_method,
                         uint32_t vtable_idx,
                         uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                         bool skip_this) OVERRIDE;

  bool WideGPRsAreAliases() OVERRIDE {
    return true;  // 64b architecture.
  }
  bool WideFPRsAreAliases() OVERRIDE {
    return true;  // 64b architecture.
  }

  size_t GetInstructionOffset(LIR* lir) OVERRIDE;

  LIR* InvokeTrampoline(OpKind op, RegStorage r_tgt, QuickEntrypointEnum trampoline) OVERRIDE;

  virtual void GenMachineSpecificExtendedMethodMIR(BasicBlock* bb, MIR* mir) OVERRIDE;
  void GenMoreMachineSpecificExtendedMethodMIR(BasicBlock* bb, MIR* mir) QC_WEAK;

private:
  /**
   * @brief Given register xNN (dNN), returns register wNN (sNN).
   * @param reg #RegStorage containing a Solo64 input register (e.g. @c x1 or @c d2).
   * @return A Solo32 with the same register number as the @p reg (e.g. @c w1 or @c s2).
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

  RegStorage Check32BitReg(RegStorage reg) {
    if ((kFailOnSizeError || kReportSizeError) && !reg.Is32Bit()) {
      if (kFailOnSizeError) {
        LOG(FATAL) << "Checked for 32b register";
      } else {
        LOG(WARNING) << "Checked for 32b register";
        return As32BitReg(reg);
      }
    }
    return reg;
  }

  /**
   * @brief Given register wNN (sNN), returns register xNN (dNN).
   * @param reg #RegStorage containing a Solo32 input register (e.g. @c w1 or @c s2).
   * @return A Solo64 with the same register number as the @p reg (e.g. @c x1 or @c d2).
   * @see As32BitReg
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

  int32_t EncodeImmSingle(uint32_t bits);
  int32_t EncodeImmDouble(uint64_t bits);
  LIR* LoadFPConstantValue(RegStorage r_dest, int32_t value);
  LIR* LoadFPConstantValueWide(RegStorage r_dest, int64_t value);
  void ReplaceFixup(LIR* prev_lir, LIR* orig_lir, LIR* new_lir);
  void InsertFixupBefore(LIR* prev_lir, LIR* orig_lir, LIR* new_lir);
  void AssignDataOffsets();
  RegLocation GenDivRem(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                        bool is_div, bool check_zero);
  RegLocation GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit, bool is_div);
  size_t GetLoadStoreSize(LIR* lir);

  bool SmallLiteralDivRem64(Instruction::Code dalvik_opcode, bool is_div, RegLocation rl_src,
                            RegLocation rl_dest, int64_t lit);

  uint32_t LinkFixupInsns(LIR* head_lir, LIR* tail_lir, CodeOffset offset);
  int AssignInsnOffsets();
  void AssignOffsets();
  uint8_t* EncodeLIRs(uint8_t* write_pos, LIR* lir);

  // Spill core and FP registers. Returns the SP difference: either spill size, or whole
  // frame size.
  int SpillRegs(RegStorage base, uint32_t core_reg_mask, uint32_t fp_reg_mask, int frame_size);

  // Unspill core and FP registers.
  void UnspillRegs(RegStorage base, uint32_t core_reg_mask, uint32_t fp_reg_mask, int frame_size);

  void GenLongOp(OpKind op, RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);

  LIR* OpRegImm64(OpKind op, RegStorage r_dest_src1, int64_t value);
  LIR* OpRegRegImm64(OpKind op, RegStorage r_dest, RegStorage r_src1, int64_t value);

  LIR* OpRegRegShift(OpKind op, RegStorage r_dest_src1, RegStorage r_src2, int shift);
  LIR* OpRegRegRegShift(OpKind op, RegStorage r_dest, RegStorage r_src1, RegStorage r_src2,
                        int shift);
  int EncodeShift(int code, int amount);

  LIR* OpRegRegExtend(OpKind op, RegStorage r_dest_src1, RegStorage r_src2,
                      A64RegExtEncodings ext, uint8_t amount);
  LIR* OpRegRegRegExtend(OpKind op, RegStorage r_dest, RegStorage r_src1, RegStorage r_src2,
                         A64RegExtEncodings ext, uint8_t amount);
  int EncodeExtend(int extend_type, int amount);
  bool IsExtendEncoding(int encoded_value);

  LIR* LoadBaseDispBody(RegStorage r_base, int displacement, RegStorage r_dest, OpSize size);
  LIR* StoreBaseDispBody(RegStorage r_base, int displacement, RegStorage r_src, OpSize size);

  int EncodeLogicalImmediate(bool is_wide, uint64_t value);
  uint64_t DecodeLogicalImmediate(bool is_wide, int value);
  ArmConditionCode ArmConditionEncoding(ConditionCode code);

  // Helper used in the two GenSelect variants.
  void GenSelect(int32_t left, int32_t right, ConditionCode code, RegStorage rs_dest,
                 int result_reg_class);

  void GenNotLong(RegLocation rl_dest, RegLocation rl_src);
  void GenNegLong(RegLocation rl_dest, RegLocation rl_src);
  void GenDivRemLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                     RegLocation rl_src2, bool is_div);

  InToRegStorageMapping in_to_reg_storage_mapping_;
  static const ArmEncodingMap EncodingMap[kA64Last];

private:
  static uint32_t ProcessMoreEncodings(const ArmEncodingMap* encoder, int i, uint32_t operand) QC_WEAK;
  static const ArmEncodingMap* GetEncoder(int opcode) QC_WEAK;

  virtual void ApplyArchOptimizations(LIR* head_lir, LIR* tail_lir, BasicBlock* bb) QC_WEAK;

  void CompilerPostInitializeRegAlloc() QC_WEAK;
  void Arm64Mir2LirPostInit(Arm64Mir2Lir* mir_to_lir) QC_WEAK;

  friend class QCArm64Mir2Lir;
  QCArm64Mir2Lir* qcm2l;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_ARM64_CODEGEN_ARM64_H_
