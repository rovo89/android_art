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

#ifndef ART_COMPILER_DEX_QUICK_X86_CODEGEN_X86_H_
#define ART_COMPILER_DEX_QUICK_X86_CODEGEN_X86_H_

#include "dex/compiler_internals.h"
#include "x86_lir.h"

#include <map>

namespace art {

class X86Mir2Lir : public Mir2Lir {
 protected:
  class InToRegStorageMapper {
   public:
    virtual RegStorage GetNextReg(bool is_double_or_float, bool is_wide, bool is_ref) = 0;
    virtual ~InToRegStorageMapper() {}
  };

  class InToRegStorageX86_64Mapper : public InToRegStorageMapper {
   public:
    explicit InToRegStorageX86_64Mapper(Mir2Lir* ml) : ml_(ml), cur_core_reg_(0), cur_fp_reg_(0) {}
    virtual ~InToRegStorageX86_64Mapper() {}
    virtual RegStorage GetNextReg(bool is_double_or_float, bool is_wide, bool is_ref);
   protected:
    Mir2Lir* ml_;
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
  X86Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);

  // Required for target - codegen helpers.
  bool SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div, RegLocation rl_src,
                          RegLocation rl_dest, int lit);
  bool EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) OVERRIDE;
  LIR* CheckSuspendUsingLoad() OVERRIDE;
  RegStorage LoadHelper(ThreadOffset<4> offset) OVERRIDE;
  RegStorage LoadHelper(ThreadOffset<8> offset) OVERRIDE;
  LIR* LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                    OpSize size, VolatileKind is_volatile) OVERRIDE;
  LIR* LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest, int scale,
                       OpSize size) OVERRIDE;
  LIR* LoadBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale, int displacement,
                           RegStorage r_dest, OpSize size) OVERRIDE;
  LIR* LoadConstantNoClobber(RegStorage r_dest, int value);
  LIR* LoadConstantWide(RegStorage r_dest, int64_t value);
  LIR* StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src,
                     OpSize size, VolatileKind is_volatile) OVERRIDE;
  LIR* StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src, int scale,
                        OpSize size) OVERRIDE;
  LIR* StoreBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale, int displacement,
                            RegStorage r_src, OpSize size) OVERRIDE;
  void MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg);
  void GenImplicitNullCheck(RegStorage reg, int opt_flags);

  // Required for target - register utilities.
  RegStorage TargetReg(SpecialTargetRegister reg) OVERRIDE;
  RegStorage TargetReg32(SpecialTargetRegister reg);
  RegStorage TargetReg(SpecialTargetRegister symbolic_reg, WideKind wide_kind) OVERRIDE {
    if (wide_kind == kWide) {
      if (cu_->target64) {
        return As64BitReg(TargetReg32(symbolic_reg));
      } else {
        // x86: construct a pair.
        DCHECK((kArg0 <= symbolic_reg && symbolic_reg < kArg3) ||
               (kFArg0 <= symbolic_reg && symbolic_reg < kFArg3) ||
               (kRet0 == symbolic_reg));
        return RegStorage::MakeRegPair(TargetReg32(symbolic_reg),
                                 TargetReg32(static_cast<SpecialTargetRegister>(symbolic_reg + 1)));
      }
    } else if (wide_kind == kRef && cu_->target64) {
      return As64BitReg(TargetReg32(symbolic_reg));
    } else {
      return TargetReg32(symbolic_reg);
    }
  }
  RegStorage TargetPtrReg(SpecialTargetRegister symbolic_reg) OVERRIDE {
    return TargetReg(symbolic_reg, cu_->target64 ? kWide : kNotWide);
  }
  RegStorage GetArgMappingToPhysicalReg(int arg_num);
  RegStorage GetCoreArgMappingToPhysicalReg(int core_arg_num);
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
  int VectorRegisterSize();
  int NumReservableVectorRegisters(bool fp_used);

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
  void GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                         RegLocation rl_src2);
  void GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array, RegLocation rl_index,
                   RegLocation rl_dest, int scale);
  void GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                   RegLocation rl_index, RegLocation rl_src, int scale, bool card_mark);
  void GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                         RegLocation rl_src1, RegLocation rl_shift);
  void GenMulLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
  void GenAddLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
  void GenAndLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
  void GenArithOpDouble(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                        RegLocation rl_src2);
  void GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                       RegLocation rl_src2);
  void GenRemFP(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2, bool is_double);
  void GenCmpFP(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                RegLocation rl_src2);
  void GenConversion(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src);
  bool GenInlinedCas(CallInfo* info, bool is_long, bool is_object);
  bool GenInlinedMinMax(CallInfo* info, bool is_min, bool is_long);
  bool GenInlinedMinMaxFP(CallInfo* info, bool is_min, bool is_double);
  bool GenInlinedSqrt(CallInfo* info);
  bool GenInlinedAbsFloat(CallInfo* info) OVERRIDE;
  bool GenInlinedAbsDouble(CallInfo* info) OVERRIDE;
  bool GenInlinedPeek(CallInfo* info, OpSize size);
  bool GenInlinedPoke(CallInfo* info, OpSize size);
  void GenNotLong(RegLocation rl_dest, RegLocation rl_src);
  void GenNegLong(RegLocation rl_dest, RegLocation rl_src);
  void GenOrLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                 RegLocation rl_src2);
  void GenSubLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
  void GenXorLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
  void GenDivRemLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                     RegLocation rl_src2, bool is_div);
  // TODO: collapse reg_lo, reg_hi
  RegLocation GenDivRem(RegLocation rl_dest, RegStorage reg_lo, RegStorage reg_hi, bool is_div);
  RegLocation GenDivRemLit(RegLocation rl_dest, RegStorage reg_lo, int lit, bool is_div);
  void GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
  void GenDivZeroCheckWide(RegStorage reg);
  void GenArrayBoundsCheck(RegStorage index, RegStorage array_base, int32_t len_offset);
  void GenArrayBoundsCheck(int32_t index, RegStorage array_base, int32_t len_offset);
  void GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method);
  void GenExitSequence();
  void GenSpecialExitSequence();
  void GenFillArrayData(DexOffset table_offset, RegLocation rl_src);
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
  void GenPackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);
  void GenSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);
  void GenIntToLong(RegLocation rl_dest, RegLocation rl_src);

  /*
   * @brief Generate a two address long operation with a constant value
   * @param rl_dest location of result
   * @param rl_src constant source operand
   * @param op Opcode to be generated
   * @return success or not
   */
  bool GenLongImm(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op);
  /*
   * @brief Generate a three address long operation with a constant value
   * @param rl_dest location of result
   * @param rl_src1 source operand
   * @param rl_src2 constant source operand
   * @param op Opcode to be generated
   * @return success or not
   */
  bool GenLongLongImm(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                      Instruction::Code op);

  /**
   * @brief Generate a long arithmetic operation.
   * @param rl_dest The destination.
   * @param rl_src1 First operand.
   * @param rl_src2 Second operand.
   * @param op The DEX opcode for the operation.
   * @param is_commutative The sources can be swapped if needed.
   */
  virtual void GenLongArith(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                            Instruction::Code op, bool is_commutative);

  /**
   * @brief Generate a two operand long arithmetic operation.
   * @param rl_dest The destination.
   * @param rl_src Second operand.
   * @param op The DEX opcode for the operation.
   */
  void GenLongArith(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op);

  /**
   * @brief Generate a long operation.
   * @param rl_dest The destination.  Must be in a register
   * @param rl_src The other operand.  May be in a register or in memory.
   * @param op The DEX opcode for the operation.
   */
  virtual void GenLongRegOrMemOp(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op);

  /**
   * @brief Implement instanceof a final class with x86 specific code.
   * @param use_declaring_class 'true' if we can use the class itself.
   * @param type_idx Type index to use if use_declaring_class is 'false'.
   * @param rl_dest Result to be set to 0 or 1.
   * @param rl_src Object to be tested.
   */
  void GenInstanceofFinal(bool use_declaring_class, uint32_t type_idx, RegLocation rl_dest,
                          RegLocation rl_src);

  void GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest,
                      RegLocation rl_src1, RegLocation rl_shift);

  // Single operation generators.
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
  LIR* OpRegMem(OpKind op, RegStorage r_dest, RegStorage r_base, int offset);
  LIR* OpRegMem(OpKind op, RegStorage r_dest, RegLocation value);
  LIR* OpMemReg(OpKind op, RegLocation rl_dest, int value);
  LIR* OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2);
  LIR* OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset, MoveType move_type);
  LIR* OpMovMemReg(RegStorage r_base, int offset, RegStorage r_src, MoveType move_type);
  LIR* OpCondRegReg(OpKind op, ConditionCode cc, RegStorage r_dest, RegStorage r_src);
  LIR* OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src1, int value);
  LIR* OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1, RegStorage r_src2);
  LIR* OpTestSuspend(LIR* target);
  LIR* OpThreadMem(OpKind op, ThreadOffset<4> thread_offset) OVERRIDE;
  LIR* OpThreadMem(OpKind op, ThreadOffset<8> thread_offset) OVERRIDE;
  LIR* OpVldm(RegStorage r_base, int count);
  LIR* OpVstm(RegStorage r_base, int count);
  void OpLea(RegStorage r_base, RegStorage reg1, RegStorage reg2, int scale, int offset);
  void OpRegCopyWide(RegStorage dest, RegStorage src);
  void OpTlsCmp(ThreadOffset<4> offset, int val) OVERRIDE;
  void OpTlsCmp(ThreadOffset<8> offset, int val) OVERRIDE;

  void OpRegThreadMem(OpKind op, RegStorage r_dest, ThreadOffset<4> thread_offset);
  void OpRegThreadMem(OpKind op, RegStorage r_dest, ThreadOffset<8> thread_offset);
  void SpillCoreRegs();
  void UnSpillCoreRegs();
  void UnSpillFPRegs();
  void SpillFPRegs();
  static const X86EncodingMap EncodingMap[kX86Last];
  bool InexpensiveConstantInt(int32_t value);
  bool InexpensiveConstantFloat(int32_t value);
  bool InexpensiveConstantLong(int64_t value);
  bool InexpensiveConstantDouble(int64_t value);

  /*
   * @brief Should try to optimize for two address instructions?
   * @return true if we try to avoid generating three operand instructions.
   */
  virtual bool GenerateTwoOperandInstructions() const { return true; }

  /*
   * @brief x86 specific codegen for int operations.
   * @param opcode Operation to perform.
   * @param rl_dest Destination for the result.
   * @param rl_lhs Left hand operand.
   * @param rl_rhs Right hand operand.
   */
  void GenArithOpInt(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_lhs,
                     RegLocation rl_rhs);

  /*
   * @brief Dump a RegLocation using printf
   * @param loc Register location to dump
   */
  static void DumpRegLocation(RegLocation loc);

  /*
   * @brief Load the Method* of a dex method into the register.
   * @param target_method The MethodReference of the method to be invoked.
   * @param type How the method will be invoked.
   * @param register that will contain the code address.
   * @note register will be passed to TargetReg to get physical register.
   */
  void LoadMethodAddress(const MethodReference& target_method, InvokeType type,
                         SpecialTargetRegister symbolic_reg);

  /*
   * @brief Load the Class* of a Dex Class type into the register.
   * @param type How the method will be invoked.
   * @param register that will contain the code address.
   * @note register will be passed to TargetReg to get physical register.
   */
  void LoadClassType(uint32_t type_idx, SpecialTargetRegister symbolic_reg);

  void FlushIns(RegLocation* ArgLocs, RegLocation rl_method);

  int GenDalvikArgsNoRange(CallInfo* info, int call_state, LIR** pcrLabel,
                           NextCallInsn next_call_insn,
                           const MethodReference& target_method,
                           uint32_t vtable_idx,
                           uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                           bool skip_this);

  int GenDalvikArgsRange(CallInfo* info, int call_state, LIR** pcrLabel,
                         NextCallInsn next_call_insn,
                         const MethodReference& target_method,
                         uint32_t vtable_idx,
                         uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                         bool skip_this);

  /*
   * @brief Generate a relative call to the method that will be patched at link time.
   * @param target_method The MethodReference of the method to be invoked.
   * @param type How the method will be invoked.
   * @returns Call instruction
   */
  virtual LIR * CallWithLinkerFixup(const MethodReference& target_method, InvokeType type);

  /*
   * @brief Handle x86 specific literals
   */
  void InstallLiteralPools();

  /*
   * @brief Generate the debug_frame CFI information.
   * @returns pointer to vector containing CFE information
   */
  static std::vector<uint8_t>* ReturnCommonCallFrameInformation();

  /*
   * @brief Generate the debug_frame FDE information.
   * @returns pointer to vector containing CFE information
   */
  std::vector<uint8_t>* ReturnCallFrameInformation();

 protected:
  // Casting of RegStorage
  RegStorage As32BitReg(RegStorage reg) {
    DCHECK(!reg.IsPair());
    if ((kFailOnSizeError || kReportSizeError) && !reg.Is64Bit()) {
      if (kFailOnSizeError) {
        LOG(FATAL) << "Expected 64b register " << reg.GetReg();
      } else {
        LOG(WARNING) << "Expected 64b register " << reg.GetReg();
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

  RegStorage As64BitReg(RegStorage reg) {
    DCHECK(!reg.IsPair());
    if ((kFailOnSizeError || kReportSizeError) && !reg.Is32Bit()) {
      if (kFailOnSizeError) {
        LOG(FATAL) << "Expected 32b register " << reg.GetReg();
      } else {
        LOG(WARNING) << "Expected 32b register " << reg.GetReg();
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

  size_t ComputeSize(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_index,
                     int32_t raw_base, int32_t displacement);
  void CheckValidByteRegister(const X86EncodingMap* entry, int32_t raw_reg);
  void EmitPrefix(const X86EncodingMap* entry,
                  int32_t raw_reg_r, int32_t raw_reg_x, int32_t raw_reg_b);
  void EmitOpcode(const X86EncodingMap* entry);
  void EmitPrefixAndOpcode(const X86EncodingMap* entry,
                           int32_t reg_r, int32_t reg_x, int32_t reg_b);
  void EmitDisp(uint8_t base, int32_t disp);
  void EmitModrmThread(uint8_t reg_or_opcode);
  void EmitModrmDisp(uint8_t reg_or_opcode, uint8_t base, int32_t disp);
  void EmitModrmSibDisp(uint8_t reg_or_opcode, uint8_t base, uint8_t index, int scale,
                        int32_t disp);
  void EmitImm(const X86EncodingMap* entry, int64_t imm);
  void EmitNullary(const X86EncodingMap* entry);
  void EmitOpRegOpcode(const X86EncodingMap* entry, int32_t raw_reg);
  void EmitOpReg(const X86EncodingMap* entry, int32_t raw_reg);
  void EmitOpMem(const X86EncodingMap* entry, int32_t raw_base, int32_t disp);
  void EmitOpArray(const X86EncodingMap* entry, int32_t raw_base, int32_t raw_index, int scale,
                   int32_t disp);
  void EmitMemReg(const X86EncodingMap* entry, int32_t raw_base, int32_t disp, int32_t raw_reg);
  void EmitRegMem(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_base, int32_t disp);
  void EmitRegArray(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_base,
                    int32_t raw_index, int scale, int32_t disp);
  void EmitArrayReg(const X86EncodingMap* entry, int32_t raw_base, int32_t raw_index, int scale,
                    int32_t disp, int32_t raw_reg);
  void EmitMemImm(const X86EncodingMap* entry, int32_t raw_base, int32_t disp, int32_t imm);
  void EmitArrayImm(const X86EncodingMap* entry, int32_t raw_base, int32_t raw_index, int scale,
                    int32_t raw_disp, int32_t imm);
  void EmitRegThread(const X86EncodingMap* entry, int32_t raw_reg, int32_t disp);
  void EmitRegReg(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_reg2);
  void EmitRegRegImm(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_reg2, int32_t imm);
  void EmitRegMemImm(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_base, int32_t disp,
                     int32_t imm);
  void EmitMemRegImm(const X86EncodingMap* entry, int32_t base, int32_t disp, int32_t raw_reg1,
                     int32_t imm);
  void EmitRegImm(const X86EncodingMap* entry, int32_t raw_reg, int32_t imm);
  void EmitThreadImm(const X86EncodingMap* entry, int32_t disp, int32_t imm);
  void EmitMovRegImm(const X86EncodingMap* entry, int32_t raw_reg, int64_t imm);
  void EmitShiftRegImm(const X86EncodingMap* entry, int32_t raw_reg, int32_t imm);
  void EmitShiftRegCl(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_cl);
  void EmitShiftMemCl(const X86EncodingMap* entry, int32_t raw_base, int32_t disp, int32_t raw_cl);
  void EmitShiftMemImm(const X86EncodingMap* entry, int32_t raw_base, int32_t disp, int32_t imm);
  void EmitRegCond(const X86EncodingMap* entry, int32_t raw_reg, int32_t cc);
  void EmitMemCond(const X86EncodingMap* entry, int32_t raw_base, int32_t disp, int32_t cc);
  void EmitRegRegCond(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_reg2, int32_t cc);
  void EmitRegMemCond(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_base, int32_t disp,
                      int32_t cc);

  void EmitJmp(const X86EncodingMap* entry, int32_t rel);
  void EmitJcc(const X86EncodingMap* entry, int32_t rel, int32_t cc);
  void EmitCallMem(const X86EncodingMap* entry, int32_t raw_base, int32_t disp);
  void EmitCallImmediate(const X86EncodingMap* entry, int32_t disp);
  void EmitCallThread(const X86EncodingMap* entry, int32_t disp);
  void EmitPcRel(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_base_or_table,
                 int32_t raw_index, int scale, int32_t table_or_disp);
  void EmitMacro(const X86EncodingMap* entry, int32_t raw_reg, int32_t offset);
  void EmitUnimplemented(const X86EncodingMap* entry, LIR* lir);
  void GenFusedLongCmpImmBranch(BasicBlock* bb, RegLocation rl_src1,
                                int64_t val, ConditionCode ccode);
  void GenConstWide(RegLocation rl_dest, int64_t value);
  void GenMultiplyVectorSignedByte(BasicBlock *bb, MIR *mir);
  void GenShiftByteVector(BasicBlock *bb, MIR *mir);
  void AndMaskVectorRegister(RegStorage rs_src1, uint32_t m1, uint32_t m2, uint32_t m3, uint32_t m4);
  void MaskVectorRegister(X86OpCode opcode, RegStorage rs_src1, uint32_t m1, uint32_t m2, uint32_t m3, uint32_t m4);
  void AppendOpcodeWithConst(X86OpCode opcode, int reg, MIR* mir);

  static bool ProvidesFullMemoryBarrier(X86OpCode opcode);

  /*
   * @brief Ensure that a temporary register is byte addressable.
   * @returns a temporary guarenteed to be byte addressable.
   */
  virtual RegStorage AllocateByteRegister();

  /*
   * @brief Use a wide temporary as a 128-bit register
   * @returns a 128-bit temporary register.
   */
  virtual RegStorage Get128BitRegister(RegStorage reg);

  /*
   * @brief Check if a register is byte addressable.
   * @returns true if a register is byte addressable.
   */
  bool IsByteRegister(RegStorage reg);
  bool GenInlinedArrayCopyCharArray(CallInfo* info) OVERRIDE;

  /*
   * @brief generate inline code for fast case of Strng.indexOf.
   * @param info Call parameters
   * @param zero_based 'true' if the index into the string is 0.
   * @returns 'true' if the call was inlined, 'false' if a regular call needs to be
   * generated.
   */
  bool GenInlinedIndexOf(CallInfo* info, bool zero_based);

  /**
   * @brief Reserve a fixed number of vector  registers from the register pool
   * @details The mir->dalvikInsn.vA specifies an N such that vector registers
   * [0..N-1] are removed from the temporary pool. The caller must call
   * ReturnVectorRegisters before calling ReserveVectorRegisters again.
   * Also sets the num_reserved_vector_regs_ to the specified value
   * @param mir whose vA specifies the number of registers to reserve
   */
  void ReserveVectorRegisters(MIR* mir);

  /**
   * @brief Return all the reserved vector registers to the temp pool
   * @details Returns [0..num_reserved_vector_regs_]
   */
  void ReturnVectorRegisters();

  /*
   * @brief Load 128 bit constant into vector register.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector
   * @note vA is the TypeSize for the register.
   * @note vB is the destination XMM register. arg[0..3] are 32 bit constant values.
   */
  void GenConst128(BasicBlock* bb, MIR* mir);

  /*
   * @brief MIR to move a vectorized register to another.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize
   * @note vB: destination
   * @note vC: source
   */
  void GenMoveVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Packed multiply of units in two vector registers: vB = vB .* @note vC using vA to know the type of the vector.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize
   * @note vB: destination and source
   * @note vC: source
   */
  void GenMultiplyVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Packed addition of units in two vector registers: vB = vB .+ vC using vA to know the type of the vector.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize
   * @note vB: destination and source
   * @note vC: source
   */
  void GenAddVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Packed subtraction of units in two vector registers: vB = vB .- vC using vA to know the type of the vector.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize
   * @note vB: destination and source
   * @note vC: source
   */
  void GenSubtractVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Packed shift left of units in two vector registers: vB = vB .<< vC using vA to know the type of the vector.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize
   * @note vB: destination and source
   * @note vC: immediate
   */
  void GenShiftLeftVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Packed signed shift right of units in two vector registers: vB = vB .>> vC using vA to know the type of the vector.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize
   * @note vB: destination and source
   * @note vC: immediate
   */
  void GenSignedShiftRightVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Packed unsigned shift right of units in two vector registers: vB = vB .>>> vC using vA to know the type of the vector.
   * @param bb The basic block in which the MIR is from..
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize
   * @note vB: destination and source
   * @note vC: immediate
   */
  void GenUnsignedShiftRightVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Packed bitwise and of units in two vector registers: vB = vB .& vC using vA to know the type of the vector.
   * @note vA: TypeSize
   * @note vB: destination and source
   * @note vC: source
   */
  void GenAndVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Packed bitwise or of units in two vector registers: vB = vB .| vC using vA to know the type of the vector.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize
   * @note vB: destination and source
   * @note vC: source
   */
  void GenOrVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Packed bitwise xor of units in two vector registers: vB = vB .^ vC using vA to know the type of the vector.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize
   * @note vB: destination and source
   * @note vC: source
   */
  void GenXorVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Reduce a 128-bit packed element into a single VR by taking lower bits
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @details Instruction does a horizontal addition of the packed elements and then adds it to VR.
   * @note vA: TypeSize
   * @note vB: destination and source VR (not vector register)
   * @note vC: source (vector register)
   */
  void GenAddReduceVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Extract a packed element into a single VR.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize
   * @note vB: destination VR (not vector register)
   * @note vC: source (vector register)
   * @note arg[0]: The index to use for extraction from vector register (which packed element).
   */
  void GenReduceVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Create a vector value, with all TypeSize values equal to vC
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is kMirConstVector.
   * @note vA: TypeSize.
   * @note vB: destination vector register.
   * @note vC: source VR (not vector register).
   */
  void GenSetVector(BasicBlock *bb, MIR *mir);

  /*
   * @brief Generate code for a vector opcode.
   * @param bb The basic block in which the MIR is from.
   * @param mir The MIR whose opcode is a non-standard opcode.
   */
  void GenMachineSpecificExtendedMethodMIR(BasicBlock* bb, MIR* mir);

  /*
   * @brief Return the correct x86 opcode for the Dex operation
   * @param op Dex opcode for the operation
   * @param loc Register location of the operand
   * @param is_high_op 'true' if this is an operation on the high word
   * @param value Immediate value for the operation.  Used for byte variants
   * @returns the correct x86 opcode to perform the operation
   */
  X86OpCode GetOpcode(Instruction::Code op, RegLocation loc, bool is_high_op, int32_t value);

  /*
   * @brief Return the correct x86 opcode for the Dex operation
   * @param op Dex opcode for the operation
   * @param dest location of the destination.  May be register or memory.
   * @param rhs Location for the rhs of the operation.  May be in register or memory.
   * @param is_high_op 'true' if this is an operation on the high word
   * @returns the correct x86 opcode to perform the operation
   * @note at most one location may refer to memory
   */
  X86OpCode GetOpcode(Instruction::Code op, RegLocation dest, RegLocation rhs,
                      bool is_high_op);

  /*
   * @brief Is this operation a no-op for this opcode and value
   * @param op Dex opcode for the operation
   * @param value Immediate value for the operation.
   * @returns 'true' if the operation will have no effect
   */
  bool IsNoOp(Instruction::Code op, int32_t value);

  /**
   * @brief Calculate magic number and shift for a given divisor
   * @param divisor divisor number for calculation
   * @param magic hold calculated magic number
   * @param shift hold calculated shift
   */
  void CalculateMagicAndShift(int divisor, int& magic, int& shift);

  /*
   * @brief Generate an integer div or rem operation.
   * @param rl_dest Destination Location.
   * @param rl_src1 Numerator Location.
   * @param rl_src2 Divisor Location.
   * @param is_div 'true' if this is a division, 'false' for a remainder.
   * @param check_zero 'true' if an exception should be generated if the divisor is 0.
   */
  RegLocation GenDivRem(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                        bool is_div, bool check_zero);

  /*
   * @brief Generate an integer div or rem operation by a literal.
   * @param rl_dest Destination Location.
   * @param rl_src Numerator Location.
   * @param lit Divisor.
   * @param is_div 'true' if this is a division, 'false' for a remainder.
   */
  RegLocation GenDivRemLit(RegLocation rl_dest, RegLocation rl_src, int lit, bool is_div);

  /*
   * Generate code to implement long shift operations.
   * @param opcode The DEX opcode to specify the shift type.
   * @param rl_dest The destination.
   * @param rl_src The value to be shifted.
   * @param shift_amount How much to shift.
   * @returns the RegLocation of the result.
   */
  RegLocation GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                RegLocation rl_src, int shift_amount);
  /*
   * Generate an imul of a register by a constant or a better sequence.
   * @param dest Destination Register.
   * @param src Source Register.
   * @param val Constant multiplier.
   */
  void GenImulRegImm(RegStorage dest, RegStorage src, int val);

  /*
   * Generate an imul of a memory location by a constant or a better sequence.
   * @param dest Destination Register.
   * @param sreg Symbolic register.
   * @param displacement Displacement on stack of Symbolic Register.
   * @param val Constant multiplier.
   */
  void GenImulMemImm(RegStorage dest, int sreg, int displacement, int val);

  /*
   * @brief Compare memory to immediate, and branch if condition true.
   * @param cond The condition code that when true will branch to the target.
   * @param temp_reg A temporary register that can be used if compare memory is not
   * supported by the architecture.
   * @param base_reg The register holding the base address.
   * @param offset The offset from the base.
   * @param check_value The immediate to compare to.
   * @param target branch target (or nullptr)
   * @param compare output for getting LIR for comparison (or nullptr)
   */
  LIR* OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                         int offset, int check_value, LIR* target, LIR** compare);

  /*
   * Can this operation be using core registers without temporaries?
   * @param rl_lhs Left hand operand.
   * @param rl_rhs Right hand operand.
   * @returns 'true' if the operation can proceed without needing temporary regs.
   */
  bool IsOperationSafeWithoutTemps(RegLocation rl_lhs, RegLocation rl_rhs);

  /**
   * @brief Generates inline code for conversion of long to FP by using x87/
   * @param rl_dest The destination of the FP.
   * @param rl_src The source of the long.
   * @param is_double 'true' if dealing with double, 'false' for float.
   */
  virtual void GenLongToFP(RegLocation rl_dest, RegLocation rl_src, bool is_double);

  /*
   * @brief Perform MIR analysis before compiling method.
   * @note Invokes Mir2LiR::Materialize after analysis.
   */
  void Materialize();

  /*
   * Mir2Lir's UpdateLoc() looks to see if the Dalvik value is currently live in any temp register
   * without regard to data type.  In practice, this can result in UpdateLoc returning a
   * location record for a Dalvik float value in a core register, and vis-versa.  For targets
   * which can inexpensively move data between core and float registers, this can often be a win.
   * However, for x86 this is generally not a win.  These variants of UpdateLoc()
   * take a register class argument - and will return an in-register location record only if
   * the value is live in a temp register of the correct class.  Additionally, if the value is in
   * a temp register of the wrong register class, it will be clobbered.
   */
  RegLocation UpdateLocTyped(RegLocation loc, int reg_class);
  RegLocation UpdateLocWideTyped(RegLocation loc, int reg_class);

  /*
   * @brief Analyze MIR before generating code, to prepare for the code generation.
   */
  void AnalyzeMIR();

  /*
   * @brief Analyze one basic block.
   * @param bb Basic block to analyze.
   */
  void AnalyzeBB(BasicBlock * bb);

  /*
   * @brief Analyze one extended MIR instruction
   * @param opcode MIR instruction opcode.
   * @param bb Basic block containing instruction.
   * @param mir Extended instruction to analyze.
   */
  void AnalyzeExtendedMIR(int opcode, BasicBlock * bb, MIR *mir);

  /*
   * @brief Analyze one MIR instruction
   * @param opcode MIR instruction opcode.
   * @param bb Basic block containing instruction.
   * @param mir Instruction to analyze.
   */
  virtual void AnalyzeMIR(int opcode, BasicBlock * bb, MIR *mir);

  /*
   * @brief Analyze one MIR float/double instruction
   * @param opcode MIR instruction opcode.
   * @param bb Basic block containing instruction.
   * @param mir Instruction to analyze.
   */
  void AnalyzeFPInstruction(int opcode, BasicBlock * bb, MIR *mir);

  /*
   * @brief Analyze one use of a double operand.
   * @param rl_use Double RegLocation for the operand.
   */
  void AnalyzeDoubleUse(RegLocation rl_use);

  /*
   * @brief Analyze one invoke-static MIR instruction
   * @param opcode MIR instruction opcode.
   * @param bb Basic block containing instruction.
   * @param mir Instruction to analyze.
   */
  void AnalyzeInvokeStatic(int opcode, BasicBlock * bb, MIR *mir);

  // Information derived from analysis of MIR

  // The compiler temporary for the code address of the method.
  CompilerTemp *base_of_code_;

  // Have we decided to compute a ptr to code and store in temporary VR?
  bool store_method_addr_;

  // Have we used the stored method address?
  bool store_method_addr_used_;

  // Instructions to remove if we didn't use the stored method address.
  LIR* setup_method_address_[2];

  // Instructions needing patching with Method* values.
  GrowableArray<LIR*> method_address_insns_;

  // Instructions needing patching with Class Type* values.
  GrowableArray<LIR*> class_type_address_insns_;

  // Instructions needing patching with PC relative code addresses.
  GrowableArray<LIR*> call_method_insns_;

  // Prologue decrement of stack pointer.
  LIR* stack_decrement_;

  // Epilogue increment of stack pointer.
  LIR* stack_increment_;

  // The list of const vector literals.
  LIR *const_vectors_;

  /*
   * @brief Search for a matching vector literal
   * @param mir A kMirOpConst128b MIR instruction to match.
   * @returns pointer to matching LIR constant, or nullptr if not found.
   */
  LIR *ScanVectorLiteral(MIR *mir);

  /*
   * @brief Add a constant vector literal
   * @param mir A kMirOpConst128b MIR instruction to match.
   */
  LIR *AddVectorLiteral(MIR *mir);

  InToRegStorageMapping in_to_reg_storage_mapping_;

  bool WideGPRsAreAliases() OVERRIDE {
    return cu_->target64;  // On 64b, we have 64b GPRs.
  }
  bool WideFPRsAreAliases() OVERRIDE {
    return true;  // xmm registers have 64b views even on x86.
  }

 private:
  // The number of vector registers [0..N] reserved by a call to ReserveVectorRegisters
  int num_reserved_vector_regs_;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_X86_CODEGEN_X86_H_
