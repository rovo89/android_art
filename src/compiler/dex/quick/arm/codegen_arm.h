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

class ArmCodegen : public Codegen {
  public:
    // Required for target - codegen helpers.
    virtual bool SmallLiteralDivide(CompilationUnit* cu, Instruction::Code dalvik_opcode,
                                    RegLocation rl_src, RegLocation rl_dest, int lit);
    virtual int LoadHelper(CompilationUnit* cu, int offset);
    virtual LIR* LoadBaseDisp(CompilationUnit* cu, int rBase, int displacement, int r_dest,
                              OpSize size, int s_reg);
    virtual LIR* LoadBaseDispWide(CompilationUnit* cu, int rBase, int displacement, int r_dest_lo,
                                  int r_dest_hi, int s_reg);
    virtual LIR* LoadBaseIndexed(CompilationUnit* cu, int rBase, int r_index, int r_dest, int scale,
                                 OpSize size);
    virtual LIR* LoadBaseIndexedDisp(CompilationUnit *cu, int rBase, int r_index, int scale,
                                     int displacement, int r_dest, int r_dest_hi, OpSize size,
                                     int s_reg);
    virtual LIR* LoadConstantNoClobber(CompilationUnit* cu, int r_dest, int value);
    virtual LIR* LoadConstantWide(CompilationUnit* cu, int r_dest_lo, int r_dest_hi, int64_t value);
    virtual LIR* StoreBaseDisp(CompilationUnit* cu, int rBase, int displacement, int r_src,
                               OpSize size);
    virtual LIR* StoreBaseDispWide(CompilationUnit* cu, int rBase, int displacement, int r_src_lo,
                                   int r_src_hi);
    virtual LIR* StoreBaseIndexed(CompilationUnit* cu, int rBase, int r_index, int r_src, int scale,
                                 OpSize size);
    virtual LIR* StoreBaseIndexedDisp(CompilationUnit *cu, int rBase, int r_index, int scale,
                                      int displacement, int r_src, int r_src_hi, OpSize size,
                                      int s_reg);
    virtual void MarkGCCard(CompilationUnit* cu, int val_reg, int tgt_addr_reg);

    // Required for target - register utilities.
    virtual bool IsFpReg(int reg);
    virtual bool SameRegType(int reg1, int reg2);
    virtual int AllocTypedTemp(CompilationUnit* cu, bool fp_hint, int reg_class);
    virtual int AllocTypedTempPair(CompilationUnit* cu, bool fp_hint, int reg_class);
    virtual int S2d(int low_reg, int high_reg);
    virtual int TargetReg(SpecialTargetRegister reg);
    virtual RegisterInfo* GetRegInfo(CompilationUnit* cu, int reg);
    virtual RegLocation GetReturnAlt(CompilationUnit* cu);
    virtual RegLocation GetReturnWideAlt(CompilationUnit* cu);
    virtual RegLocation LocCReturn();
    virtual RegLocation LocCReturnDouble();
    virtual RegLocation LocCReturnFloat();
    virtual RegLocation LocCReturnWide();
    virtual uint32_t FpRegMask();
    virtual uint64_t GetRegMaskCommon(CompilationUnit* cu, int reg);
    virtual void AdjustSpillMask(CompilationUnit* cu);
    virtual void ClobberCalleeSave(CompilationUnit *cu);
    virtual void FlushReg(CompilationUnit* cu, int reg);
    virtual void FlushRegWide(CompilationUnit* cu, int reg1, int reg2);
    virtual void FreeCallTemps(CompilationUnit* cu);
    virtual void FreeRegLocTemps(CompilationUnit* cu, RegLocation rl_keep, RegLocation rl_free);
    virtual void LockCallTemps(CompilationUnit* cu);
    virtual void MarkPreservedSingle(CompilationUnit* cu, int v_reg, int reg);
    virtual void CompilerInitializeRegAlloc(CompilationUnit* cu);

    // Required for target - miscellaneous.
    virtual AssemblerStatus AssembleInstructions(CompilationUnit* cu, uintptr_t start_addr);
    virtual void DumpResourceMask(LIR* lir, uint64_t mask, const char* prefix);
    virtual void SetupTargetResourceMasks(CompilationUnit* cu, LIR* lir);
    virtual const char* GetTargetInstFmt(int opcode);
    virtual const char* GetTargetInstName(int opcode);
    virtual std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr);
    virtual uint64_t GetPCUseDefEncoding();
    virtual uint64_t GetTargetInstFlags(int opcode);
    virtual int GetInsnSize(LIR* lir);
    virtual bool IsUnconditionalBranch(LIR* lir);

    // Required for target - Dalvik-level generators.
    virtual void GenArithImmOpLong(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenArrayObjPut(CompilationUnit* cu, int opt_flags, RegLocation rl_array,
                                RegLocation rl_index, RegLocation rl_src, int scale);
    virtual void GenArrayGet(CompilationUnit* cu, int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_dest, int scale);
    virtual void GenArrayPut(CompilationUnit* cu, int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_src, int scale);
    virtual void GenShiftImmOpLong(CompilationUnit* cu, Instruction::Code opcode,
                                   RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_shift);
    virtual void GenMulLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2);
    virtual void GenAddLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2);
    virtual void GenAndLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2);
    virtual void GenArithOpDouble(CompilationUnit* cu, Instruction::Code opcode,
                                  RegLocation rl_dest, RegLocation rl_src1,
                                  RegLocation rl_src2);
    virtual void GenArithOpFloat(CompilationUnit *cu, Instruction::Code opcode, RegLocation rl_dest,
                                 RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenCmpFP(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                          RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenConversion(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                               RegLocation rl_src);
    virtual bool GenInlinedCas32(CompilationUnit* cu, CallInfo* info, bool need_write_barrier);
    virtual bool GenInlinedMinMaxInt(CompilationUnit *cu, CallInfo* info, bool is_min);
    virtual bool GenInlinedSqrt(CompilationUnit* cu, CallInfo* info);
    virtual void GenNegLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src);
    virtual void GenOrLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_src2);
    virtual void GenSubLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2);
    virtual void GenXorLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2);
    virtual LIR* GenRegMemCheck(CompilationUnit* cu, ConditionCode c_code, int reg1, int base,
                                int offset, ThrowKind kind);
    virtual RegLocation GenDivRem(CompilationUnit* cu, RegLocation rl_dest, int reg_lo, int reg_hi,
                                  bool is_div);
    virtual RegLocation GenDivRemLit(CompilationUnit* cu, RegLocation rl_dest, int reg_lo, int lit,
                                     bool is_div);
    virtual void GenCmpLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2);
    virtual void GenDivZeroCheck(CompilationUnit* cu, int reg_lo, int reg_hi);
    virtual void GenEntrySequence(CompilationUnit* cu, RegLocation* ArgLocs,
                                  RegLocation rl_method);
    virtual void GenExitSequence(CompilationUnit* cu);
    virtual void GenFillArrayData(CompilationUnit* cu, uint32_t table_offset,
                                  RegLocation rl_src);
    virtual void GenFusedFPCmpBranch(CompilationUnit* cu, BasicBlock* bb, MIR* mir, bool gt_bias,
                                     bool is_double);
    virtual void GenFusedLongCmpBranch(CompilationUnit* cu, BasicBlock* bb, MIR* mir);
    virtual void GenSelect(CompilationUnit* cu, BasicBlock* bb, MIR* mir);
    virtual void GenMemBarrier(CompilationUnit* cu, MemBarrierKind barrier_kind);
    virtual void GenMonitorEnter(CompilationUnit* cu, int opt_flags, RegLocation rl_src);
    virtual void GenMonitorExit(CompilationUnit* cu, int opt_flags, RegLocation rl_src);
    virtual void GenMoveException(CompilationUnit* cu, RegLocation rl_dest);
    virtual void GenMultiplyByTwoBitMultiplier(CompilationUnit* cu, RegLocation rl_src,
                                               RegLocation rl_result, int lit, int first_bit,
                                               int second_bit);
    virtual void GenNegDouble(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src);
    virtual void GenNegFloat(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src);
    virtual void GenPackedSwitch(CompilationUnit* cu, uint32_t table_offset,
                                 RegLocation rl_src);
    virtual void GenSparseSwitch(CompilationUnit* cu, uint32_t table_offset,
                                 RegLocation rl_src);
    virtual void GenSpecialCase(CompilationUnit* cu, BasicBlock* bb, MIR* mir,
                                SpecialCaseHandler special_case);

    // Required for target - single operation generators.
    virtual LIR* OpUnconditionalBranch(CompilationUnit* cu, LIR* target);
    virtual LIR* OpCmpBranch(CompilationUnit* cu, ConditionCode cond, int src1, int src2,
                             LIR* target);
    virtual LIR* OpCmpImmBranch(CompilationUnit* cu, ConditionCode cond, int reg, int check_value,
                                LIR* target);
    virtual LIR* OpCondBranch(CompilationUnit* cu, ConditionCode cc, LIR* target);
    virtual LIR* OpDecAndBranch(CompilationUnit* cu, ConditionCode c_code, int reg,
                                LIR* target);
    virtual LIR* OpFpRegCopy(CompilationUnit* cu, int r_dest, int r_src);
    virtual LIR* OpIT(CompilationUnit* cu, ConditionCode cond, const char* guide);
    virtual LIR* OpMem(CompilationUnit* cu, OpKind op, int rBase, int disp);
    virtual LIR* OpPcRelLoad(CompilationUnit* cu, int reg, LIR* target);
    virtual LIR* OpReg(CompilationUnit* cu, OpKind op, int r_dest_src);
    virtual LIR* OpRegCopy(CompilationUnit* cu, int r_dest, int r_src);
    virtual LIR* OpRegCopyNoInsert(CompilationUnit* cu, int r_dest, int r_src);
    virtual LIR* OpRegImm(CompilationUnit* cu, OpKind op, int r_dest_src1, int value);
    virtual LIR* OpRegMem(CompilationUnit* cu, OpKind op, int r_dest, int rBase, int offset);
    virtual LIR* OpRegReg(CompilationUnit* cu, OpKind op, int r_dest_src1, int r_src2);
    virtual LIR* OpRegRegImm(CompilationUnit* cu, OpKind op, int r_dest, int r_src1, int value);
    virtual LIR* OpRegRegReg(CompilationUnit* cu, OpKind op, int r_dest, int r_src1,
                             int r_src2);
    virtual LIR* OpTestSuspend(CompilationUnit* cu, LIR* target);
    virtual LIR* OpThreadMem(CompilationUnit* cu, OpKind op, int thread_offset);
    virtual LIR* OpVldm(CompilationUnit* cu, int rBase, int count);
    virtual LIR* OpVstm(CompilationUnit* cu, int rBase, int count);
    virtual void OpLea(CompilationUnit* cu, int rBase, int reg1, int reg2, int scale,
                       int offset);
    virtual void OpRegCopyWide(CompilationUnit* cu, int dest_lo, int dest_hi, int src_lo,
                               int src_hi);
    virtual void OpTlsCmp(CompilationUnit* cu, int offset, int val);

    static RegLocation ArgLoc(CompilationUnit* cu, RegLocation loc);
    LIR* LoadBaseDispBody(CompilationUnit* cu, int rBase, int displacement, int r_dest,
                          int r_dest_hi, OpSize size, int s_reg);
    LIR* StoreBaseDispBody(CompilationUnit* cu, int rBase, int displacement, int r_src,
                           int r_src_hi, OpSize size);
    static void GenPrintLabel(CompilationUnit *cu, MIR* mir);
    static LIR* OpRegRegRegShift(CompilationUnit* cu, OpKind op, int r_dest, int r_src1,
                                 int r_src2, int shift);
    static LIR* OpRegRegShift(CompilationUnit* cu, OpKind op, int r_dest_src1, int r_src2,
                              int shift);
    static const ArmEncodingMap EncodingMap[kArmLast];
    static int EncodeShift(int code, int amount);
    static int ModifiedImmediate(uint32_t value);
    static ArmConditionCode ArmConditionEncoding(ConditionCode code);
    bool InexpensiveConstantInt(int32_t value);
    bool InexpensiveConstantFloat(int32_t value);
    bool InexpensiveConstantLong(int64_t value);
    bool InexpensiveConstantDouble(int64_t value);

  private:
    void GenFusedLongCmpImmBranch(CompilationUnit* cu, BasicBlock* bb, RegLocation rl_src1,
                                  int64_t val, ConditionCode ccode);
};

}  // namespace art

#endif  // ART_SRC_COMPILER_DEX_QUICK_ARM_CODEGENARM_H_
