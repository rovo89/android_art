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

#ifndef ART_COMPILER_DEX_QUICK_ARM_CODEGEN_ARM_H_
#define ART_COMPILER_DEX_QUICK_ARM_CODEGEN_ARM_H_

#include "arm_lir.h"
#include "base/arena_containers.h"
#include "base/logging.h"
#include "dex/quick/mir_to_lir.h"

namespace art {

struct CompilationUnit;

class ArmMir2Lir FINAL : public Mir2Lir {
 protected:
  // Inherited class for ARM backend.
  class InToRegStorageArmMapper FINAL : public InToRegStorageMapper {
   public:
    InToRegStorageArmMapper()
        : cur_core_reg_(0), cur_fp_reg_(0), cur_fp_double_reg_(0) {
    }

    RegStorage GetNextReg(ShortyArg arg) OVERRIDE;

    virtual void Reset() OVERRIDE {
      cur_core_reg_ = 0;
      cur_fp_reg_ = 0;
      cur_fp_double_reg_ = 0;
    }

   private:
    size_t cur_core_reg_;
    size_t cur_fp_reg_;
    size_t cur_fp_double_reg_;
  };

  InToRegStorageArmMapper in_to_reg_storage_arm_mapper_;
  InToRegStorageMapper* GetResetedInToRegStorageMapper() OVERRIDE {
    in_to_reg_storage_arm_mapper_.Reset();
    return &in_to_reg_storage_arm_mapper_;
  }

  public:
    ArmMir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);

    // Required for target - codegen helpers.
    bool SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div, RegLocation rl_src,
                            RegLocation rl_dest, int lit);
    bool EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) OVERRIDE;
    void GenMultiplyByConstantFloat(RegLocation rl_dest, RegLocation rl_src1,
                                    int32_t constant) OVERRIDE;
    void GenMultiplyByConstantDouble(RegLocation rl_dest, RegLocation rl_src1,
                                     int64_t constant) OVERRIDE;
    LIR* CheckSuspendUsingLoad() OVERRIDE;
    RegStorage LoadHelper(QuickEntrypointEnum trampoline) OVERRIDE;
    LIR* LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                      OpSize size, VolatileKind is_volatile) OVERRIDE;
    LIR* LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest, int scale,
                         OpSize size) OVERRIDE;
    LIR* LoadConstantNoClobber(RegStorage r_dest, int value);
    LIR* LoadConstantWide(RegStorage r_dest, int64_t value);
    LIR* StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src,
                       OpSize size, VolatileKind is_volatile) OVERRIDE;
    LIR* StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src, int scale,
                          OpSize size) OVERRIDE;

    /// @copydoc Mir2Lir::UnconditionallyMarkGCCard(RegStorage)
    void UnconditionallyMarkGCCard(RegStorage tgt_addr_reg) OVERRIDE;

    bool CanUseOpPcRelDexCacheArrayLoad() const OVERRIDE;
    void OpPcRelDexCacheArrayLoad(const DexFile* dex_file, int offset, RegStorage r_dest,
                                  bool wide) OVERRIDE;

    // Required for target - register utilities.
    RegStorage TargetReg(SpecialTargetRegister reg) OVERRIDE;
    RegStorage TargetReg(SpecialTargetRegister reg, WideKind wide_kind) OVERRIDE {
      if (wide_kind == kWide) {
        DCHECK((kArg0 <= reg && reg < kArg3) || (kFArg0 <= reg && reg < kFArg15) || (kRet0 == reg));
        RegStorage ret_reg = RegStorage::MakeRegPair(TargetReg(reg),
            TargetReg(static_cast<SpecialTargetRegister>(reg + 1)));
        if (ret_reg.IsFloat()) {
          // Regard double as double, be consistent with register allocation.
          ret_reg = As64BitFloatReg(ret_reg);
        }
        return ret_reg;
      } else {
        return TargetReg(reg);
      }
    }

    RegLocation GetReturnAlt() OVERRIDE;
    RegLocation GetReturnWideAlt() OVERRIDE;
    RegLocation LocCReturn() OVERRIDE;
    RegLocation LocCReturnRef() OVERRIDE;
    RegLocation LocCReturnDouble() OVERRIDE;
    RegLocation LocCReturnFloat() OVERRIDE;
    RegLocation LocCReturnWide() OVERRIDE;
    ResourceMask GetRegMaskCommon(const RegStorage& reg) const OVERRIDE;
    void AdjustSpillMask();
    void ClobberCallerSave();
    void FreeCallTemps();
    void LockCallTemps();
    void MarkPreservedSingle(int v_reg, RegStorage reg);
    void MarkPreservedDouble(int v_reg, RegStorage reg);
    void CompilerInitializeRegAlloc();

    // Required for target - miscellaneous.
    void AssembleLIR();
    uint32_t LinkFixupInsns(LIR* head_lir, LIR* tail_lir, CodeOffset offset);
    int AssignInsnOffsets();
    void AssignOffsets();
    static uint8_t* EncodeLIRs(uint8_t* write_pos, LIR* lir);
    void DumpResourceMask(LIR* lir, const ResourceMask& mask, const char* prefix) OVERRIDE;
    void SetupTargetResourceMasks(LIR* lir, uint64_t flags,
                                  ResourceMask* use_mask, ResourceMask* def_mask) OVERRIDE;
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
    void GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                        RegLocation rl_src2, int flags) OVERRIDE;
    void GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                           RegLocation rl_src1, RegLocation rl_src2, int flags);
    void GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                     RegLocation rl_index, RegLocation rl_dest, int scale);
    void GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array, RegLocation rl_index,
                     RegLocation rl_src, int scale, bool card_mark);
    void GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                           RegLocation rl_src1, RegLocation rl_shift, int flags);
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
    bool GenInlinedArrayCopyCharArray(CallInfo* info) OVERRIDE;
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
    void GenMonitorEnter(int opt_flags, RegLocation rl_src);
    void GenMonitorExit(int opt_flags, RegLocation rl_src);
    void GenMoveException(RegLocation rl_dest);
    void GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result, int lit,
                                       int first_bit, int second_bit);
    void GenNegDouble(RegLocation rl_dest, RegLocation rl_src);
    void GenNegFloat(RegLocation rl_dest, RegLocation rl_src);
    void GenLargePackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);
    void GenLargeSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);
    void GenMaddMsubInt(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                        RegLocation rl_src3, bool is_sub);

    // Required for target - single operation generators.
    LIR* OpUnconditionalBranch(LIR* target);
    LIR* OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target);
    LIR* OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value, LIR* target);
    LIR* OpCondBranch(ConditionCode cc, LIR* target);
    LIR* OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target);
    LIR* OpFpRegCopy(RegStorage r_dest, RegStorage r_src);
    LIR* OpIT(ConditionCode cond, const char* guide);
    void UpdateIT(LIR* it, const char* new_guide);
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

    LIR* LoadBaseDispBody(RegStorage r_base, int displacement, RegStorage r_dest, OpSize size);
    LIR* StoreBaseDispBody(RegStorage r_base, int displacement, RegStorage r_src, OpSize size);
    LIR* OpRegRegRegShift(OpKind op, RegStorage r_dest, RegStorage r_src1, RegStorage r_src2,
                          int shift);
    LIR* OpRegRegShift(OpKind op, RegStorage r_dest_src1, RegStorage r_src2, int shift);
    static const ArmEncodingMap EncodingMap[kArmLast];
    int EncodeShift(int code, int amount);
    int ModifiedImmediate(uint32_t value);
    ArmConditionCode ArmConditionEncoding(ConditionCode code);
    bool InexpensiveConstantInt(int32_t value) OVERRIDE;
    bool InexpensiveConstantInt(int32_t value, Instruction::Code opcode) OVERRIDE;
    bool InexpensiveConstantFloat(int32_t value) OVERRIDE;
    bool InexpensiveConstantLong(int64_t value) OVERRIDE;
    bool InexpensiveConstantDouble(int64_t value) OVERRIDE;
    RegStorage AllocPreservedDouble(int s_reg);
    RegStorage AllocPreservedSingle(int s_reg);

    bool WideGPRsAreAliases() const OVERRIDE {
      return false;  // Wide GPRs are formed by pairing.
    }
    bool WideFPRsAreAliases() const OVERRIDE {
      return false;  // Wide FPRs are formed by pairing.
    }

    NextCallInsn GetNextSDCallInsn() OVERRIDE;

    /*
     * @brief Generate a relative call to the method that will be patched at link time.
     * @param target_method The MethodReference of the method to be invoked.
     * @param type How the method will be invoked.
     * @returns Call instruction
     */
    LIR* CallWithLinkerFixup(const MethodReference& target_method, InvokeType type);

    /*
     * @brief Generate the actual call insn based on the method info.
     * @param method_info the lowering info for the method call.
     * @returns Call instruction
     */
    LIR* GenCallInsn(const MirMethodLoweringInfo& method_info) OVERRIDE;

    void CountRefs(RefCounts* core_counts, RefCounts* fp_counts, size_t num_regs) OVERRIDE;
    void DoPromotion() OVERRIDE;

    /*
     * @brief Handle ARM specific literals.
     */
    void InstallLiteralPools() OVERRIDE;

    LIR* InvokeTrampoline(OpKind op, RegStorage r_tgt, QuickEntrypointEnum trampoline) OVERRIDE;
    size_t GetInstructionOffset(LIR* lir);

    void GenMachineSpecificExtendedMethodMIR(BasicBlock* bb, MIR* mir) OVERRIDE;

    bool HandleEasyDivRem(Instruction::Code dalvik_opcode, bool is_div,
                          RegLocation rl_src, RegLocation rl_dest, int lit) OVERRIDE;

  private:
    void GenNegLong(RegLocation rl_dest, RegLocation rl_src);
    void GenMulLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                    RegLocation rl_src2);
    void GenFusedLongCmpImmBranch(BasicBlock* bb, RegLocation rl_src1, int64_t val,
                                  ConditionCode ccode);
    LIR* LoadFPConstantValue(int r_dest, int value);
    LIR* LoadStoreUsingInsnWithOffsetImm8Shl2(ArmOpcode opcode, RegStorage r_base,
                                              int displacement, RegStorage r_src_dest,
                                              RegStorage r_work = RegStorage::InvalidReg());
    void ReplaceFixup(LIR* prev_lir, LIR* orig_lir, LIR* new_lir);
    void InsertFixupBefore(LIR* prev_lir, LIR* orig_lir, LIR* new_lir);
    void AssignDataOffsets();
    RegLocation GenDivRem(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                          bool is_div, int flags) OVERRIDE;
    RegLocation GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit, bool is_div) OVERRIDE;
    struct EasyMultiplyOp {
      OpKind op;
      uint32_t shift;
    };
    bool GetEasyMultiplyOp(int lit, EasyMultiplyOp* op);
    bool GetEasyMultiplyTwoOps(int lit, EasyMultiplyOp* ops);
    void GenEasyMultiplyTwoOps(RegStorage r_dest, RegStorage r_src, EasyMultiplyOp* ops);

    static constexpr ResourceMask GetRegMaskArm(RegStorage reg);
    static constexpr ResourceMask EncodeArmRegList(int reg_list);
    static constexpr ResourceMask EncodeArmRegFpcsList(int reg_list);

    ArenaVector<LIR*> call_method_insns_;

    // Instructions needing patching with PC relative code addresses.
    ArenaVector<LIR*> dex_cache_access_insns_;

    // Register with a reference to the dex cache arrays at dex_cache_arrays_min_offset_,
    // if promoted.
    RegStorage dex_cache_arrays_base_reg_;

    /**
     * @brief Given float register pair, returns Solo64 float register.
     * @param reg #RegStorage containing a float register pair (e.g. @c s2 and @c s3).
     * @return A Solo64 float mapping to the register pair (e.g. @c d1).
     */
    static RegStorage As64BitFloatReg(RegStorage reg) {
      DCHECK(reg.IsFloat());

      RegStorage low = reg.GetLow();
      RegStorage high = reg.GetHigh();
      DCHECK((low.GetRegNum() % 2 == 0) && (low.GetRegNum() + 1 == high.GetRegNum()));

      return RegStorage::FloatSolo64(low.GetRegNum() / 2);
    }

    /**
     * @brief Given Solo64 float register, returns float register pair.
     * @param reg #RegStorage containing a Solo64 float register (e.g. @c d1).
     * @return A float register pair mapping to the Solo64 float pair (e.g. @c s2 and s3).
     */
    static RegStorage As64BitFloatRegPair(RegStorage reg) {
      DCHECK(reg.IsDouble() && reg.Is64BitSolo());

      int reg_num = reg.GetRegNum();
      return RegStorage::MakeRegPair(RegStorage::FloatSolo32(reg_num * 2),
                                     RegStorage::FloatSolo32(reg_num * 2 + 1));
    }

    int GenDalvikArgsBulkCopy(CallInfo* info, int first, int count) OVERRIDE;

    static int ArmNextSDCallInsn(CompilationUnit* cu, CallInfo* info ATTRIBUTE_UNUSED,
                                 int state, const MethodReference& target_method,
                                 uint32_t unused_idx ATTRIBUTE_UNUSED,
                                 uintptr_t direct_code, uintptr_t direct_method,
                                 InvokeType type);

    void OpPcRelDexCacheArrayAddr(const DexFile* dex_file, int offset, RegStorage r_dest);
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_ARM_CODEGEN_ARM_H_
