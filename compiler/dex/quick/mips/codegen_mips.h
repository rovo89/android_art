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

#include "dex/compiler_internals.h"
#include "mips_lir.h"

namespace art {

class MipsMir2Lir FINAL : public Mir2Lir {
  public:
    MipsMir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);

    // Required for target - codegen utilities.
    bool SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div, RegLocation rl_src,
                            RegLocation rl_dest, int lit);
    bool EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) OVERRIDE;
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
    LIR* GenAtomic64Load(RegStorage r_base, int displacement, RegStorage r_dest);
    LIR* GenAtomic64Store(RegStorage r_base, int displacement, RegStorage r_src);
    void MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg);

    // Required for target - register utilities.
    RegStorage Solo64ToPair64(RegStorage reg);
    RegStorage TargetReg(SpecialTargetRegister reg);
    RegStorage GetArgMappingToPhysicalReg(int arg_num);
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
    void GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                           RegLocation rl_src1, RegLocation rl_src2);
    void GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                     RegLocation rl_index, RegLocation rl_dest, int scale);
    void GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                     RegLocation rl_index, RegLocation rl_src, int scale, bool card_mark);
    void GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_shift);
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
    void GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                        RegLocation rl_src2) OVERRIDE;
    RegLocation GenDivRem(RegLocation rl_dest, RegStorage reg_lo, RegStorage reg_hi, bool is_div);
    RegLocation GenDivRemLit(RegLocation rl_dest, RegStorage reg_lo, int lit, bool is_div);
    void GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
    void GenDivZeroCheckWide(RegStorage reg);
    void GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method);
    void GenExitSequence();
    void GenSpecialExitSequence();
    void GenFillArrayData(uint32_t table_offset, RegLocation rl_src);
    void GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias, bool is_double);
    void GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir);
    void GenSelect(BasicBlock* bb, MIR* mir);
    void GenSelectConst32(RegStorage left_op, RegStorage right_op, ConditionCode code,
                          int32_t true_val, int32_t false_val, RegStorage rs_dest,
                          int dest_reg_class) OVERRIDE;
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
    LIR* OpPcRelLoad(RegStorage reg, LIR* target);
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
    LIR* LoadBaseDispBody(RegStorage r_base, int displacement, RegStorage r_dest,
                          OpSize size);
    // TODO: collapse r_src.
    LIR* StoreBaseDispBody(RegStorage r_base, int displacement, RegStorage r_src,
                           OpSize size);
    void SpillCoreRegs();
    void UnSpillCoreRegs();
    static const MipsEncodingMap EncodingMap[kMipsLast];
    bool InexpensiveConstantInt(int32_t value);
    bool InexpensiveConstantFloat(int32_t value);
    bool InexpensiveConstantLong(int64_t value);
    bool InexpensiveConstantDouble(int64_t value);

    bool WideGPRsAreAliases() OVERRIDE {
      return false;  // Wide GPRs are formed by pairing.
    }
    bool WideFPRsAreAliases() OVERRIDE {
      return false;  // Wide FPRs are formed by pairing.
    }

    LIR* InvokeTrampoline(OpKind op, RegStorage r_tgt, QuickEntrypointEnum trampoline) OVERRIDE;

  private:
    void GenNegLong(RegLocation rl_dest, RegLocation rl_src);
    void GenAddLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                    RegLocation rl_src2);
    void GenSubLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                    RegLocation rl_src2);

    void ConvertShortToLongBranch(LIR* lir);
    RegLocation GenDivRem(RegLocation rl_dest, RegLocation rl_src1,
                          RegLocation rl_src2, bool is_div, bool check_zero);
    RegLocation GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit, bool is_div);
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_MIPS_CODEGEN_MIPS_H_
