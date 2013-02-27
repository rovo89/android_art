/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_DEX_QUICK_CODEGEN_H_
#define ART_SRC_COMPILER_DEX_QUICK_CODEGEN_H_

#include "compiler/dex/compiler_ir.h"

namespace art {

// Set to 1 to measure cost of suspend check.
#define NO_SUSPEND 0

#define IS_BINARY_OP         (1ULL << kIsBinaryOp)
#define IS_BRANCH            (1ULL << kIsBranch)
#define IS_IT                (1ULL << kIsIT)
#define IS_LOAD              (1ULL << kMemLoad)
#define IS_QUAD_OP           (1ULL << kIsQuadOp)
#define IS_QUIN_OP           (1ULL << kIsQuinOp)
#define IS_SEXTUPLE_OP       (1ULL << kIsSextupleOp)
#define IS_STORE             (1ULL << kMemStore)
#define IS_TERTIARY_OP       (1ULL << kIsTertiaryOp)
#define IS_UNARY_OP          (1ULL << kIsUnaryOp)
#define NEEDS_FIXUP          (1ULL << kPCRelFixup)
#define NO_OPERAND           (1ULL << kNoOperand)
#define REG_DEF0             (1ULL << kRegDef0)
#define REG_DEF1             (1ULL << kRegDef1)
#define REG_DEFA             (1ULL << kRegDefA)
#define REG_DEFD             (1ULL << kRegDefD)
#define REG_DEF_FPCS_LIST0   (1ULL << kRegDefFPCSList0)
#define REG_DEF_FPCS_LIST2   (1ULL << kRegDefFPCSList2)
#define REG_DEF_LIST0        (1ULL << kRegDefList0)
#define REG_DEF_LIST1        (1ULL << kRegDefList1)
#define REG_DEF_LR           (1ULL << kRegDefLR)
#define REG_DEF_SP           (1ULL << kRegDefSP)
#define REG_USE0             (1ULL << kRegUse0)
#define REG_USE1             (1ULL << kRegUse1)
#define REG_USE2             (1ULL << kRegUse2)
#define REG_USE3             (1ULL << kRegUse3)
#define REG_USE4             (1ULL << kRegUse4)
#define REG_USEA             (1ULL << kRegUseA)
#define REG_USEC             (1ULL << kRegUseC)
#define REG_USED             (1ULL << kRegUseD)
#define REG_USE_FPCS_LIST0   (1ULL << kRegUseFPCSList0)
#define REG_USE_FPCS_LIST2   (1ULL << kRegUseFPCSList2)
#define REG_USE_LIST0        (1ULL << kRegUseList0)
#define REG_USE_LIST1        (1ULL << kRegUseList1)
#define REG_USE_LR           (1ULL << kRegUseLR)
#define REG_USE_PC           (1ULL << kRegUsePC)
#define REG_USE_SP           (1ULL << kRegUseSP)
#define SETS_CCODES          (1ULL << kSetsCCodes)
#define USES_CCODES          (1ULL << kUsesCCodes)

// Common combo register usage patterns.
#define REG_DEF01            (REG_DEF0 | REG_DEF1)
#define REG_DEF01_USE2       (REG_DEF0 | REG_DEF1 | REG_USE2)
#define REG_DEF0_USE01       (REG_DEF0 | REG_USE01)
#define REG_DEF0_USE0        (REG_DEF0 | REG_USE0)
#define REG_DEF0_USE12       (REG_DEF0 | REG_USE12)
#define REG_DEF0_USE1        (REG_DEF0 | REG_USE1)
#define REG_DEF0_USE2        (REG_DEF0 | REG_USE2)
#define REG_DEFAD_USEAD      (REG_DEFAD_USEA | REG_USED)
#define REG_DEFAD_USEA       (REG_DEFA_USEA | REG_DEFD)
#define REG_DEFA_USEA        (REG_DEFA | REG_USEA)
#define REG_USE012           (REG_USE01 | REG_USE2)
#define REG_USE014           (REG_USE01 | REG_USE4)
#define REG_USE01            (REG_USE0 | REG_USE1)
#define REG_USE02            (REG_USE0 | REG_USE2)
#define REG_USE12            (REG_USE1 | REG_USE2)
#define REG_USE23            (REG_USE2 | REG_USE3)

typedef int (*NextCallInsn)(CompilationUnit*, CallInfo*, int, uint32_t dex_idx,
                            uint32_t method_idx, uintptr_t direct_code,
                            uintptr_t direct_method, InvokeType type);

// Target-specific initialization.
bool InitArmCodegen(CompilationUnit* cu);
bool InitMipsCodegen(CompilationUnit* cu);
bool InitX86Codegen(CompilationUnit* cu);

class Codegen {

  public:

    virtual ~Codegen(){};

    // Shared by all targets - implemented in gen_common.cc.
    void HandleSuspendLaunchPads(CompilationUnit *cu);
    void HandleIntrinsicLaunchPads(CompilationUnit *cu);
    void HandleThrowLaunchPads(CompilationUnit *cu);
    void GenBarrier(CompilationUnit* cu);
    LIR* GenCheck(CompilationUnit* cu, ConditionCode c_code, ThrowKind kind);
    LIR* GenImmedCheck(CompilationUnit* cu, ConditionCode c_code, int reg, int imm_val,
                       ThrowKind kind);
    LIR* GenNullCheck(CompilationUnit* cu, int s_reg, int m_reg, int opt_flags);
    LIR* GenRegRegCheck(CompilationUnit* cu, ConditionCode c_code, int reg1, int reg2,
                        ThrowKind kind);
    void GenCompareAndBranch(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_src1,
                             RegLocation rl_src2, LIR* taken, LIR* fall_through);
    void GenCompareZeroAndBranch(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_src,
                                 LIR* taken, LIR* fall_through);
    void GenIntToLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src);
    void GenIntNarrowing(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                         RegLocation rl_src);
    void GenNewArray(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest,
                     RegLocation rl_src);
    void GenFilledNewArray(CompilationUnit* cu, CallInfo* info);
    void GenSput(CompilationUnit* cu, uint32_t field_idx, RegLocation rl_src,
                 bool is_long_or_double, bool is_object);
    void GenSget(CompilationUnit* cu, uint32_t field_idx, RegLocation rl_dest,
                 bool is_long_or_double, bool is_object);
    void GenShowTarget(CompilationUnit* cu);
    void GenIGet(CompilationUnit* cu, uint32_t field_idx, int opt_flags, OpSize size,
                 RegLocation rl_dest, RegLocation rl_obj, bool is_long_or_double, bool is_object);
    void GenIPut(CompilationUnit* cu, uint32_t field_idx, int opt_flags, OpSize size,
                 RegLocation rl_src, RegLocation rl_obj, bool is_long_or_double, bool is_object);
    void GenConstClass(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest);
    void GenConstString(CompilationUnit* cu, uint32_t string_idx, RegLocation rl_dest);
    void GenNewInstance(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest);
    void GenThrow(CompilationUnit* cu, RegLocation rl_src);
    void GenInstanceof(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest,
                       RegLocation rl_src);
    void GenCheckCast(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_src);
    void GenLong3Addr(CompilationUnit* cu, OpKind first_op, OpKind second_op, RegLocation rl_dest,
                      RegLocation rl_src1, RegLocation rl_src2);
    void GenShiftOpLong(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                        RegLocation rl_src1, RegLocation rl_shift);
    void GenArithOpInt(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                       RegLocation rl_src1, RegLocation rl_src2);
    void GenArithOpIntLit(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                          RegLocation rl_src, int lit);
    void GenArithOpLong(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                        RegLocation rl_src1, RegLocation rl_src2);
    void GenConversionCall(CompilationUnit* cu, int func_offset, RegLocation rl_dest,
                           RegLocation rl_src);
    void GenSuspendTest(CompilationUnit* cu, int opt_flags);
    void GenSuspendTestAndBranch(CompilationUnit* cu, int opt_flags, LIR* target);

    // Shared by all targets - implemented in gen_invoke.cc.
    int CallHelperSetup(CompilationUnit* cu, int helper_offset);
    LIR* CallHelper(CompilationUnit* cu, int r_tgt, int helper_offset, bool safepoint_pc);
    void CallRuntimeHelperImm(CompilationUnit* cu, int helper_offset, int arg0, bool safepoint_pc);
    void CallRuntimeHelperReg(CompilationUnit* cu, int helper_offset, int arg0, bool safepoint_pc);
    void CallRuntimeHelperRegLocation(CompilationUnit* cu, int helper_offset, RegLocation arg0,
                                       bool safepoint_pc);
    void CallRuntimeHelperImmImm(CompilationUnit* cu, int helper_offset, int arg0, int arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperImmRegLocation(CompilationUnit* cu, int helper_offset, int arg0,
                                         RegLocation arg1, bool safepoint_pc);
    void CallRuntimeHelperRegLocationImm(CompilationUnit* cu, int helper_offset, RegLocation arg0,
                                         int arg1, bool safepoint_pc);
    void CallRuntimeHelperImmReg(CompilationUnit* cu, int helper_offset, int arg0, int arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperRegImm(CompilationUnit* cu, int helper_offset, int arg0, int arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperImmMethod(CompilationUnit* cu, int helper_offset, int arg0,
                                    bool safepoint_pc);
    void CallRuntimeHelperRegLocationRegLocation(CompilationUnit* cu, int helper_offset,
                                                 RegLocation arg0, RegLocation arg1,
                                                 bool safepoint_pc);
    void CallRuntimeHelperRegReg(CompilationUnit* cu, int helper_offset, int arg0, int arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperRegRegImm(CompilationUnit* cu, int helper_offset, int arg0, int arg1,
                                    int arg2, bool safepoint_pc);
    void CallRuntimeHelperImmMethodRegLocation(CompilationUnit* cu, int helper_offset, int arg0,
                                               RegLocation arg2, bool safepoint_pc);
    void CallRuntimeHelperImmMethodImm(CompilationUnit* cu, int helper_offset, int arg0, int arg2,
                                       bool safepoint_pc);
    void CallRuntimeHelperImmRegLocationRegLocation(CompilationUnit* cu, int helper_offset,
                                                    int arg0, RegLocation arg1, RegLocation arg2,
                                                    bool safepoint_pc);
    void GenInvoke(CompilationUnit* cu, CallInfo* info);
    void FlushIns(CompilationUnit* cu, RegLocation* ArgLocs, RegLocation rl_method);
    int GenDalvikArgsNoRange(CompilationUnit* cu, CallInfo* info, int call_state, LIR** pcrLabel,
                             NextCallInsn next_call_insn, uint32_t dex_idx, uint32_t method_idx,
                             uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                             bool skip_this);
    int GenDalvikArgsRange(CompilationUnit* cu, CallInfo* info, int call_state, LIR** pcrLabel,
                           NextCallInsn next_call_insn, uint32_t dex_idx, uint32_t method_idx,
                           uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                           bool skip_this);
    RegLocation InlineTarget(CompilationUnit* cu, CallInfo* info);
    RegLocation InlineTargetWide(CompilationUnit* cu, CallInfo* info);
    CallInfo* NewMemCallInfo(CompilationUnit* cu, BasicBlock* bb, MIR* mir, InvokeType type,
                             bool is_range);
    bool GenInlinedCharAt(CompilationUnit* cu, CallInfo* info);
    bool GenInlinedStringIsEmptyOrLength(CompilationUnit* cu, CallInfo* info, bool is_empty);
    bool GenInlinedAbsInt(CompilationUnit *cu, CallInfo* info);
    bool GenInlinedAbsLong(CompilationUnit *cu, CallInfo* info);
    bool GenInlinedFloatCvt(CompilationUnit *cu, CallInfo* info);
    bool GenInlinedDoubleCvt(CompilationUnit *cu, CallInfo* info);
    bool GenInlinedIndexOf(CompilationUnit* cu, CallInfo* info, bool zero_based);
    bool GenInlinedStringCompareTo(CompilationUnit* cu, CallInfo* info);
    bool GenInlinedCurrentThread(CompilationUnit* cu, CallInfo* info);
    bool GenInlinedUnsafeGet(CompilationUnit* cu, CallInfo* info, bool is_long, bool is_volatile);
    bool GenInlinedUnsafePut(CompilationUnit* cu, CallInfo* info, bool is_long, bool is_object,
                             bool is_volatile, bool is_ordered);
    bool GenIntrinsic(CompilationUnit* cu, CallInfo* info);

    // Shared by all targets - implemented in gen_loadstore.cc.
    RegLocation LoadCurrMethod(CompilationUnit *cu);
    void LoadCurrMethodDirect(CompilationUnit *cu, int r_tgt);
    LIR* LoadConstant(CompilationUnit* cu, int r_dest, int value);
    LIR* LoadWordDisp(CompilationUnit* cu, int rBase, int displacement, int r_dest);
    RegLocation LoadValue(CompilationUnit* cu, RegLocation rl_src, RegisterClass op_kind);
    RegLocation LoadValueWide(CompilationUnit* cu, RegLocation rl_src, RegisterClass op_kind);
    void LoadValueDirect(CompilationUnit* cu, RegLocation rl_src, int r_dest);
    void LoadValueDirectFixed(CompilationUnit* cu, RegLocation rl_src, int r_dest);
    void LoadValueDirectWide(CompilationUnit* cu, RegLocation rl_src, int reg_lo, int reg_hi);
    void LoadValueDirectWideFixed(CompilationUnit* cu, RegLocation rl_src, int reg_lo, int reg_hi);
    LIR* StoreWordDisp(CompilationUnit* cu, int rBase, int displacement, int r_src);
    void StoreValue(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src);
    void StoreValueWide(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src);

    // Required for target - codegen helpers.
    virtual bool SmallLiteralDivide(CompilationUnit* cu, Instruction::Code dalvik_opcode,
                                    RegLocation rl_src, RegLocation rl_dest, int lit) = 0;
    virtual int LoadHelper(CompilationUnit* cu, int offset) = 0;
    virtual LIR* LoadBaseDisp(CompilationUnit* cu, int rBase, int displacement, int r_dest,
                              OpSize size, int s_reg) = 0;
    virtual LIR* LoadBaseDispWide(CompilationUnit* cu, int rBase, int displacement, int r_dest_lo,
                                  int r_dest_hi, int s_reg) = 0;
    virtual LIR* LoadBaseIndexed(CompilationUnit* cu, int rBase, int r_index, int r_dest, int scale,
                                 OpSize size) = 0;
    virtual LIR* LoadBaseIndexedDisp(CompilationUnit *cu, int rBase, int r_index, int scale,
                                     int displacement, int r_dest, int r_dest_hi, OpSize size,
                                     int s_reg) = 0;
    virtual LIR* LoadConstantNoClobber(CompilationUnit* cu, int r_dest, int value) = 0;
    virtual LIR* LoadConstantWide(CompilationUnit* cu, int r_dest_lo, int r_dest_hi,
                                  int64_t value) = 0;
    virtual LIR* StoreBaseDisp(CompilationUnit* cu, int rBase, int displacement, int r_src,
                               OpSize size) = 0;
    virtual LIR* StoreBaseDispWide(CompilationUnit* cu, int rBase, int displacement, int r_src_lo,
                                   int r_src_hi) = 0;
    virtual LIR* StoreBaseIndexed(CompilationUnit* cu, int rBase, int r_index, int r_src, int scale,
                                 OpSize size) = 0;
    virtual LIR* StoreBaseIndexedDisp(CompilationUnit *cu, int rBase, int r_index, int scale,
                                      int displacement, int r_src, int r_src_hi, OpSize size,
                                      int s_reg) = 0;
    virtual void MarkGCCard(CompilationUnit* cu, int val_reg, int tgt_addr_reg) = 0;

    // Required for target - register utilities.
    virtual bool IsFpReg(int reg) = 0;
    virtual bool SameRegType(int reg1, int reg2) = 0;
    virtual int AllocTypedTemp(CompilationUnit* cu, bool fp_hint, int reg_class) = 0;
    virtual int AllocTypedTempPair(CompilationUnit* cu, bool fp_hint, int reg_class) = 0;
    virtual int S2d(int low_reg, int high_reg) = 0;
    virtual int TargetReg(SpecialTargetRegister reg) = 0;
    virtual RegisterInfo* GetRegInfo(CompilationUnit* cu, int reg) = 0;
    virtual RegLocation GetReturnAlt(CompilationUnit* cu) = 0;
    virtual RegLocation GetReturnWideAlt(CompilationUnit* cu) = 0;
    virtual RegLocation LocCReturn() = 0;
    virtual RegLocation LocCReturnDouble() = 0;
    virtual RegLocation LocCReturnFloat() = 0;
    virtual RegLocation LocCReturnWide() = 0;
    virtual uint32_t FpRegMask() = 0;
    virtual uint64_t GetRegMaskCommon(CompilationUnit* cu, int reg) = 0;
    virtual void AdjustSpillMask(CompilationUnit* cu) = 0;
    virtual void ClobberCalleeSave(CompilationUnit *cu) = 0;
    virtual void FlushReg(CompilationUnit* cu, int reg) = 0;
    virtual void FlushRegWide(CompilationUnit* cu, int reg1, int reg2) = 0;
    virtual void FreeCallTemps(CompilationUnit* cu) = 0;
    virtual void FreeRegLocTemps(CompilationUnit* cu, RegLocation rl_keep, RegLocation rl_free) = 0;
    virtual void LockCallTemps(CompilationUnit* cu) = 0;
    virtual void MarkPreservedSingle(CompilationUnit* cu, int v_reg, int reg) = 0;
    virtual void CompilerInitializeRegAlloc(CompilationUnit* cu) = 0;

    // Required for target - miscellaneous.
    virtual AssemblerStatus AssembleInstructions(CompilationUnit* cu, uintptr_t start_addr) = 0;
    virtual void DumpResourceMask(LIR* lir, uint64_t mask, const char* prefix) = 0;
    virtual void SetupTargetResourceMasks(CompilationUnit* cu, LIR* lir) = 0;
    virtual const char* GetTargetInstFmt(int opcode) = 0;
    virtual const char* GetTargetInstName(int opcode) = 0;
    virtual std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr) = 0;
    virtual uint64_t GetPCUseDefEncoding() = 0;
    virtual uint64_t GetTargetInstFlags(int opcode) = 0;
    virtual int GetInsnSize(LIR* lir) = 0;
    virtual bool IsUnconditionalBranch(LIR* lir) = 0;

    // Required for target - Dalvik-level generators.
    virtual void GenArithImmOpLong(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenMulLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenAddLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenAndLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenArithOpDouble(CompilationUnit* cu, Instruction::Code opcode,
                                  RegLocation rl_dest, RegLocation rl_src1,
                                  RegLocation rl_src2) = 0;
    virtual void GenArithOpFloat(CompilationUnit *cu, Instruction::Code opcode, RegLocation rl_dest,
                                 RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenCmpFP(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                          RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenConversion(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                               RegLocation rl_src) = 0;
    virtual bool GenInlinedCas32(CompilationUnit* cu, CallInfo* info, bool need_write_barrier) = 0;
    virtual bool GenInlinedMinMaxInt(CompilationUnit *cu, CallInfo* info, bool is_min) = 0;
    virtual bool GenInlinedSqrt(CompilationUnit* cu, CallInfo* info) = 0;
    virtual void GenNegLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenOrLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_src2) = 0;
    virtual void GenSubLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenXorLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual LIR* GenRegMemCheck(CompilationUnit* cu, ConditionCode c_code, int reg1, int base,
                                int offset, ThrowKind kind) = 0;
    virtual RegLocation GenDivRem(CompilationUnit* cu, RegLocation rl_dest, int reg_lo, int reg_hi,
                                  bool is_div) = 0;
    virtual RegLocation GenDivRemLit(CompilationUnit* cu, RegLocation rl_dest, int reg_lo, int lit,
                                     bool is_div) = 0;
    virtual void GenCmpLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenDivZeroCheck(CompilationUnit* cu, int reg_lo, int reg_hi) = 0;
    virtual void GenEntrySequence(CompilationUnit* cu, RegLocation* ArgLocs,
                                  RegLocation rl_method) = 0;
    virtual void GenExitSequence(CompilationUnit* cu) = 0;
    virtual void GenFillArrayData(CompilationUnit* cu, uint32_t table_offset,
                                  RegLocation rl_src) = 0;
    virtual void GenFusedFPCmpBranch(CompilationUnit* cu, BasicBlock* bb, MIR* mir, bool gt_bias,
                                     bool is_double) = 0;
    virtual void GenFusedLongCmpBranch(CompilationUnit* cu, BasicBlock* bb, MIR* mir) = 0;
    virtual void GenSelect(CompilationUnit* cu, BasicBlock* bb, MIR* mir) = 0;
    virtual void GenMemBarrier(CompilationUnit* cu, MemBarrierKind barrier_kind) = 0;
    virtual void GenMonitorEnter(CompilationUnit* cu, int opt_flags, RegLocation rl_src) = 0;
    virtual void GenMonitorExit(CompilationUnit* cu, int opt_flags, RegLocation rl_src) = 0;
    virtual void GenMoveException(CompilationUnit* cu, RegLocation rl_dest) = 0;
    virtual void GenMultiplyByTwoBitMultiplier(CompilationUnit* cu, RegLocation rl_src,
                                               RegLocation rl_result, int lit, int first_bit,
                                               int second_bit) = 0;
    virtual void GenNegDouble(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenNegFloat(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenPackedSwitch(CompilationUnit* cu, uint32_t table_offset,
                                 RegLocation rl_src) = 0;
    virtual void GenSparseSwitch(CompilationUnit* cu, uint32_t table_offset,
                                 RegLocation rl_src) = 0;
    virtual void GenSpecialCase(CompilationUnit* cu, BasicBlock* bb, MIR* mir,
                                SpecialCaseHandler special_case) = 0;
    virtual void GenArrayObjPut(CompilationUnit* cu, int opt_flags, RegLocation rl_array,
                                RegLocation rl_index, RegLocation rl_src, int scale) = 0;
    virtual void GenArrayGet(CompilationUnit* cu, int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_dest, int scale) = 0;
    virtual void GenArrayPut(CompilationUnit* cu, int opt_flags, OpSize size, RegLocation rl_array,
                     RegLocation rl_index, RegLocation rl_src, int scale) = 0;
    virtual void GenShiftImmOpLong(CompilationUnit* cu, Instruction::Code opcode,
                                   RegLocation rl_dest, RegLocation rl_src1,
                                   RegLocation rl_shift) = 0;

    // Required for target - single operation generators.
    virtual LIR* OpUnconditionalBranch(CompilationUnit* cu, LIR* target) = 0;
    virtual LIR* OpCmpBranch(CompilationUnit* cu, ConditionCode cond, int src1, int src2,
                             LIR* target) = 0;
    virtual LIR* OpCmpImmBranch(CompilationUnit* cu, ConditionCode cond, int reg, int check_value,
                                LIR* target) = 0;
    virtual LIR* OpCondBranch(CompilationUnit* cu, ConditionCode cc, LIR* target) = 0;
    virtual LIR* OpDecAndBranch(CompilationUnit* cu, ConditionCode c_code, int reg,
                                LIR* target) = 0;
    virtual LIR* OpFpRegCopy(CompilationUnit* cu, int r_dest, int r_src) = 0;
    virtual LIR* OpIT(CompilationUnit* cu, ConditionCode cond, const char* guide) = 0;
    virtual LIR* OpMem(CompilationUnit* cu, OpKind op, int rBase, int disp) = 0;
    virtual LIR* OpPcRelLoad(CompilationUnit* cu, int reg, LIR* target) = 0;
    virtual LIR* OpReg(CompilationUnit* cu, OpKind op, int r_dest_src) = 0;
    virtual LIR* OpRegCopy(CompilationUnit* cu, int r_dest, int r_src) = 0;
    virtual LIR* OpRegCopyNoInsert(CompilationUnit* cu, int r_dest, int r_src) = 0;
    virtual LIR* OpRegImm(CompilationUnit* cu, OpKind op, int r_dest_src1, int value) = 0;
    virtual LIR* OpRegMem(CompilationUnit* cu, OpKind op, int r_dest, int rBase, int offset) = 0;
    virtual LIR* OpRegReg(CompilationUnit* cu, OpKind op, int r_dest_src1, int r_src2) = 0;
    virtual LIR* OpRegRegImm(CompilationUnit* cu, OpKind op, int r_dest, int r_src1, int value) = 0;
    virtual LIR* OpRegRegReg(CompilationUnit* cu, OpKind op, int r_dest, int r_src1,
                             int r_src2) = 0;
    virtual LIR* OpTestSuspend(CompilationUnit* cu, LIR* target) = 0;
    virtual LIR* OpThreadMem(CompilationUnit* cu, OpKind op, int thread_offset) = 0;
    virtual LIR* OpVldm(CompilationUnit* cu, int rBase, int count) = 0;
    virtual LIR* OpVstm(CompilationUnit* cu, int rBase, int count) = 0;
    virtual void OpLea(CompilationUnit* cu, int rBase, int reg1, int reg2, int scale,
                       int offset) = 0;
    virtual void OpRegCopyWide(CompilationUnit* cu, int dest_lo, int dest_hi, int src_lo,
                               int src_hi) = 0;
    virtual void OpTlsCmp(CompilationUnit* cu, int offset, int val) = 0;
    virtual bool InexpensiveConstantInt(int32_t value) = 0;
    virtual bool InexpensiveConstantFloat(int32_t value) = 0;
    virtual bool InexpensiveConstantLong(int64_t value) = 0;
    virtual bool InexpensiveConstantDouble(int64_t value) = 0;

    // Temp workaround
    void Workaround7250540(CompilationUnit* cu, RegLocation rl_dest, int value);
    };  // Class Codegen

}  // namespace art

#endif // ART_SRC_COMPILER_DEX_QUICK_CODEGEN_H_
