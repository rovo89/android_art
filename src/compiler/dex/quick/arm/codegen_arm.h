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

#ifndef ART_SRC_COMPILER_DEX_QUICK_ARM_CODEGENARM_H_
#define ART_SRC_COMPILER_DEX_QUICK_ARM_CODEGENARM_H_

#include "compiler/dex/compiler_internals.h"

namespace art {

class ArmMir2Lir : public Mir2Lir {
  public:

    ArmMir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);

    // Required for target - codegen helpers.
    virtual bool SmallLiteralDivide(Instruction::Code dalvik_opcode, RegLocation rl_src,
                                    RegLocation rl_dest, int lit);
    virtual int LoadHelper(int offset);
    virtual LIR* LoadBaseDisp(int rBase, int displacement, int r_dest, OpSize size, int s_reg);
    virtual LIR* LoadBaseDispWide(int rBase, int displacement, int r_dest_lo, int r_dest_hi,
                                  int s_reg);
    virtual LIR* LoadBaseIndexed(int rBase, int r_index, int r_dest, int scale, OpSize size);
    virtual LIR* LoadBaseIndexedDisp(int rBase, int r_index, int scale, int displacement,
                                     int r_dest, int r_dest_hi, OpSize size, int s_reg);
    virtual LIR* LoadConstantNoClobber(int r_dest, int value);
    virtual LIR* LoadConstantWide(int r_dest_lo, int r_dest_hi, int64_t value);
    virtual LIR* StoreBaseDisp(int rBase, int displacement, int r_src, OpSize size);
    virtual LIR* StoreBaseDispWide(int rBase, int displacement, int r_src_lo, int r_src_hi);
    virtual LIR* StoreBaseIndexed(int rBase, int r_index, int r_src, int scale, OpSize size);
    virtual LIR* StoreBaseIndexedDisp(int rBase, int r_index, int scale, int displacement,
                                      int r_src, int r_src_hi, OpSize size, int s_reg);
    virtual void MarkGCCard(int val_reg, int tgt_addr_reg);

    // Required for target - register utilities.
    virtual bool IsFpReg(int reg);
    virtual bool SameRegType(int reg1, int reg2);
    virtual int AllocTypedTemp(bool fp_hint, int reg_class);
    virtual int AllocTypedTempPair(bool fp_hint, int reg_class);
    virtual int S2d(int low_reg, int high_reg);
    virtual int TargetReg(SpecialTargetRegister reg);
    virtual RegisterInfo* GetRegInfo(int reg);
    virtual RegLocation GetReturnAlt();
    virtual RegLocation GetReturnWideAlt();
    virtual RegLocation LocCReturn();
    virtual RegLocation LocCReturnDouble();
    virtual RegLocation LocCReturnFloat();
    virtual RegLocation LocCReturnWide();
    virtual uint32_t FpRegMask();
    virtual uint64_t GetRegMaskCommon(int reg);
    virtual void AdjustSpillMask();
    virtual void ClobberCalleeSave();
    virtual void FlushReg(int reg);
    virtual void FlushRegWide(int reg1, int reg2);
    virtual void FreeCallTemps();
    virtual void FreeRegLocTemps(RegLocation rl_keep, RegLocation rl_free);
    virtual void LockCallTemps();
    virtual void MarkPreservedSingle(int v_reg, int reg);
    virtual void CompilerInitializeRegAlloc();

    // Required for target - miscellaneous.
    virtual AssemblerStatus AssembleInstructions(uintptr_t start_addr);
    virtual void DumpResourceMask(LIR* lir, uint64_t mask, const char* prefix);
    virtual void SetupTargetResourceMasks(LIR* lir);
    virtual const char* GetTargetInstFmt(int opcode);
    virtual const char* GetTargetInstName(int opcode);
    virtual std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr);
    virtual uint64_t GetPCUseDefEncoding();
    virtual uint64_t GetTargetInstFlags(int opcode);
    virtual int GetInsnSize(LIR* lir);
    virtual bool IsUnconditionalBranch(LIR* lir);

    // Required for target - Dalvik-level generators.
    virtual void GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenArrayObjPut(int opt_flags, RegLocation rl_array, RegLocation rl_index,
                                RegLocation rl_src, int scale);
    virtual void GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_dest, int scale);
    virtual void GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_src, int scale);
    virtual void GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_shift);
    virtual void GenMulLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenAddLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenAndLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenArithOpDouble(Instruction::Code opcode, RegLocation rl_dest,
                                  RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest,
                                 RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenCmpFP(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                          RegLocation rl_src2);
    virtual void GenConversion(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src);
    virtual bool GenInlinedCas32(CallInfo* info, bool need_write_barrier);
    virtual bool GenInlinedMinMaxInt(CallInfo* info, bool is_min);
    virtual bool GenInlinedSqrt(CallInfo* info);
    virtual void GenNegLong(RegLocation rl_dest, RegLocation rl_src);
    virtual void GenOrLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenSubLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenXorLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
    virtual LIR* GenRegMemCheck(ConditionCode c_code, int reg1, int base, int offset,
                                ThrowKind kind);
    virtual RegLocation GenDivRem(RegLocation rl_dest, int reg_lo, int reg_hi, bool is_div);
    virtual RegLocation GenDivRemLit(RegLocation rl_dest, int reg_lo, int lit, bool is_div);
    virtual void GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenDivZeroCheck(int reg_lo, int reg_hi);
    virtual void GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method);
    virtual void GenExitSequence();
    virtual void GenFillArrayData(uint32_t table_offset, RegLocation rl_src);
    virtual void GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias, bool is_double);
    virtual void GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir);
    virtual void GenSelect(BasicBlock* bb, MIR* mir);
    virtual void GenMemBarrier(MemBarrierKind barrier_kind);
    virtual void GenMonitorEnter(int opt_flags, RegLocation rl_src);
    virtual void GenMonitorExit(int opt_flags, RegLocation rl_src);
    virtual void GenMoveException(RegLocation rl_dest);
    virtual void GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result, int lit,
                                               int first_bit, int second_bit);
    virtual void GenNegDouble(RegLocation rl_dest, RegLocation rl_src);
    virtual void GenNegFloat(RegLocation rl_dest, RegLocation rl_src);
    virtual void GenPackedSwitch(MIR* mir, uint32_t table_offset, RegLocation rl_src);
    virtual void GenSparseSwitch(MIR* mir, uint32_t table_offset, RegLocation rl_src);
    virtual void GenSpecialCase(BasicBlock* bb, MIR* mir, SpecialCaseHandler special_case);

    // Required for target - single operation generators.
    virtual LIR* OpUnconditionalBranch(LIR* target);
    virtual LIR* OpCmpBranch(ConditionCode cond, int src1, int src2, LIR* target);
    virtual LIR* OpCmpImmBranch(ConditionCode cond, int reg, int check_value, LIR* target);
    virtual LIR* OpCondBranch(ConditionCode cc, LIR* target);
    virtual LIR* OpDecAndBranch(ConditionCode c_code, int reg, LIR* target);
    virtual LIR* OpFpRegCopy(int r_dest, int r_src);
    virtual LIR* OpIT(ConditionCode cond, const char* guide);
    virtual LIR* OpMem(OpKind op, int rBase, int disp);
    virtual LIR* OpPcRelLoad(int reg, LIR* target);
    virtual LIR* OpReg(OpKind op, int r_dest_src);
    virtual LIR* OpRegCopy(int r_dest, int r_src);
    virtual LIR* OpRegCopyNoInsert(int r_dest, int r_src);
    virtual LIR* OpRegImm(OpKind op, int r_dest_src1, int value);
    virtual LIR* OpRegMem(OpKind op, int r_dest, int rBase, int offset);
    virtual LIR* OpRegReg(OpKind op, int r_dest_src1, int r_src2);
    virtual LIR* OpRegRegImm(OpKind op, int r_dest, int r_src1, int value);
    virtual LIR* OpRegRegReg(OpKind op, int r_dest, int r_src1, int r_src2);
    virtual LIR* OpTestSuspend(LIR* target);
    virtual LIR* OpThreadMem(OpKind op, int thread_offset);
    virtual LIR* OpVldm(int rBase, int count);
    virtual LIR* OpVstm(int rBase, int count);
    virtual void OpLea(int rBase, int reg1, int reg2, int scale, int offset);
    virtual void OpRegCopyWide(int dest_lo, int dest_hi, int src_lo, int src_hi);
    virtual void OpTlsCmp(int offset, int val);

    RegLocation ArgLoc(RegLocation loc);
    LIR* LoadBaseDispBody(int rBase, int displacement, int r_dest, int r_dest_hi, OpSize size,
                          int s_reg);
    LIR* StoreBaseDispBody(int rBase, int displacement, int r_src, int r_src_hi, OpSize size);
    void GenPrintLabel(MIR* mir);
    LIR* OpRegRegRegShift(OpKind op, int r_dest, int r_src1, int r_src2, int shift);
    LIR* OpRegRegShift(OpKind op, int r_dest_src1, int r_src2, int shift);
    static const ArmEncodingMap EncodingMap[kArmLast];
    int EncodeShift(int code, int amount);
    int ModifiedImmediate(uint32_t value);
    ArmConditionCode ArmConditionEncoding(ConditionCode code);
    bool InexpensiveConstantInt(int32_t value);
    bool InexpensiveConstantFloat(int32_t value);
    bool InexpensiveConstantLong(int64_t value);
    bool InexpensiveConstantDouble(int64_t value);

  private:
    void GenFusedLongCmpImmBranch(BasicBlock* bb, RegLocation rl_src1, int64_t val,
                                  ConditionCode ccode);
    int InPosition(int s_reg);
    RegLocation LoadArg(RegLocation loc);
    void LockLiveArgs(MIR* mir);
    MIR* GetNextMir(BasicBlock** p_bb, MIR* mir);
    MIR* SpecialIGet(BasicBlock** bb, MIR* mir, OpSize size, bool long_or_double, bool is_object);
    MIR* SpecialIPut(BasicBlock** bb, MIR* mir, OpSize size, bool long_or_double, bool is_object);
    MIR* SpecialIdentity(MIR* mir);
    LIR* LoadFPConstantValue(int r_dest, int value);
    bool BadOverlap(RegLocation rl_src, RegLocation rl_dest);
};

}  // namespace art

#endif  // ART_SRC_COMPILER_DEX_QUICK_ARM_CODEGENARM_H_
