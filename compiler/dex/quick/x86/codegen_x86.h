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

namespace art {

class X86Mir2Lir FINAL : public Mir2Lir {
  public:
    X86Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);

    // Required for target - codegen helpers.
    bool SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div, RegLocation rl_src,
                            RegLocation rl_dest, int lit);
    bool EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) OVERRIDE;
    LIR* CheckSuspendUsingLoad() OVERRIDE;
    RegStorage LoadHelper(ThreadOffset<4> offset) OVERRIDE;
    RegStorage LoadHelper(ThreadOffset<8> offset) OVERRIDE;
    LIR* LoadBaseDispVolatile(RegStorage r_base, int displacement, RegStorage r_dest,
                              OpSize size) OVERRIDE;
    LIR* LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                      OpSize size) OVERRIDE;
    LIR* LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest, int scale,
                         OpSize size) OVERRIDE;
    LIR* LoadBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale, int displacement,
                             RegStorage r_dest, OpSize size) OVERRIDE;
    LIR* LoadConstantNoClobber(RegStorage r_dest, int value);
    LIR* LoadConstantWide(RegStorage r_dest, int64_t value);
    LIR* StoreBaseDispVolatile(RegStorage r_base, int displacement, RegStorage r_src,
                               OpSize size) OVERRIDE;
    LIR* StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src,
                       OpSize size) OVERRIDE;
    LIR* StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src, int scale,
                          OpSize size) OVERRIDE;
    LIR* StoreBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale, int displacement,
                              RegStorage r_src, OpSize size) OVERRIDE;
    void MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg);

    // Required for target - register utilities.
    RegStorage AllocTypedTemp(bool fp_hint, int reg_class);
    RegStorage AllocTypedTempWide(bool fp_hint, int reg_class);
    RegStorage TargetReg(SpecialTargetRegister reg);
    RegStorage GetArgMappingToPhysicalReg(int arg_num);
    RegLocation GetReturnAlt();
    RegLocation GetReturnWideAlt();
    RegLocation LocCReturn();
    RegLocation LocCReturnDouble();
    RegLocation LocCReturnFloat();
    RegLocation LocCReturnWide();
    uint64_t GetRegMaskCommon(RegStorage reg);
    void AdjustSpillMask();
    void ClobberCallerSave();
    void FreeCallTemps();
    void FreeRegLocTemps(RegLocation rl_keep, RegLocation rl_free);
    void LockCallTemps();
    void MarkPreservedSingle(int v_reg, RegStorage reg);
    void MarkPreservedDouble(int v_reg, RegStorage reg);
    void CompilerInitializeRegAlloc();

    // Required for target - miscellaneous.
    void AssembleLIR();
    int AssignInsnOffsets();
    void AssignOffsets();
    AssemblerStatus AssembleInstructions(CodeOffset start_addr);
    void DumpResourceMask(LIR* lir, uint64_t mask, const char* prefix);
    void SetupTargetResourceMasks(LIR* lir, uint64_t flags);
    const char* GetTargetInstFmt(int opcode);
    const char* GetTargetInstName(int opcode);
    std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr);
    uint64_t GetPCUseDefEncoding();
    uint64_t GetTargetInstFlags(int opcode);
    int GetInsnSize(LIR* lir);
    bool IsUnconditionalBranch(LIR* lir);

    // Check support for volatile load/store of a given size.
    bool SupportsVolatileLoadStore(OpSize size) OVERRIDE;
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
    void GenCmpFP(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
    void GenConversion(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src);
    bool GenInlinedCas(CallInfo* info, bool is_long, bool is_object);
    bool GenInlinedMinMaxInt(CallInfo* info, bool is_min);
    bool GenInlinedSqrt(CallInfo* info);
    bool GenInlinedPeek(CallInfo* info, OpSize size);
    bool GenInlinedPoke(CallInfo* info, OpSize size);
    void GenNegLong(RegLocation rl_dest, RegLocation rl_src);
    void GenOrLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                   RegLocation rl_src2);
    void GenSubLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                    RegLocation rl_src2);
    void GenXorLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                    RegLocation rl_src2);
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
    void GenMemBarrier(MemBarrierKind barrier_kind);
    void GenMoveException(RegLocation rl_dest);
    void GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result, int lit,
                                       int first_bit, int second_bit);
    void GenNegDouble(RegLocation rl_dest, RegLocation rl_src);
    void GenNegFloat(RegLocation rl_dest, RegLocation rl_src);
    void GenPackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);
    void GenSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);

    /*
     * @brief Generate a two address long operation with a constant value
     * @param rl_dest location of result
     * @param rl_src constant source operand
     * @param op Opcode to be generated
     */
    void GenLongImm(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op);
    /*
     * @brief Generate a three address long operation with a constant value
     * @param rl_dest location of result
     * @param rl_src1 source operand
     * @param rl_src2 constant source operand
     * @param op Opcode to be generated
     */
    void GenLongLongImm(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                        Instruction::Code op);

    /**
      * @brief Generate a long arithmetic operation.
      * @param rl_dest The destination.
      * @param rl_src1 First operand.
      * @param rl_src2 Second operand.
      * @param op The DEX opcode for the operation.
      * @param is_commutative The sources can be swapped if needed.
      */
    void GenLongArith(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
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
    void GenLongRegOrMemOp(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op);

    /**
     * @brief Implement instanceof a final class with x86 specific code.
     * @param use_declaring_class 'true' if we can use the class itself.
     * @param type_idx Type index to use if use_declaring_class is 'false'.
     * @param rl_dest Result to be set to 0 or 1.
     * @param rl_src Object to be tested.
     */
    void GenInstanceofFinal(bool use_declaring_class, uint32_t type_idx, RegLocation rl_dest,
                            RegLocation rl_src);
    /*
     *
     * @brief Implement Set up instanceof a class with x86 specific code.
     * @param needs_access_check 'true' if we must check the access.
     * @param type_known_final 'true' if the type is known to be a final class.
     * @param type_known_abstract 'true' if the type is known to be an abstract class.
     * @param use_declaring_class 'true' if the type can be loaded off the current Method*.
     * @param can_assume_type_is_in_dex_cache 'true' if the type is known to be in the cache.
     * @param type_idx Type index to use if use_declaring_class is 'false'.
     * @param rl_dest Result to be set to 0 or 1.
     * @param rl_src Object to be tested.
     */
    void GenInstanceofCallingHelper(bool needs_access_check, bool type_known_final,
                                    bool type_known_abstract, bool use_declaring_class,
                                    bool can_assume_type_is_in_dex_cache,
                                    uint32_t type_idx, RegLocation rl_dest, RegLocation rl_src);

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
    static const X86EncodingMap EncodingMap[kX86Last];
    bool InexpensiveConstantInt(int32_t value);
    bool InexpensiveConstantFloat(int32_t value);
    bool InexpensiveConstantLong(int64_t value);
    bool InexpensiveConstantDouble(int64_t value);

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

    /*
     * @brief Generate a relative call to the method that will be patched at link time.
     * @param target_method The MethodReference of the method to be invoked.
     * @param type How the method will be invoked.
     * @returns Call instruction
     */
    LIR * CallWithLinkerFixup(const MethodReference& target_method, InvokeType type);

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

  private:
    void EmitPrefix(const X86EncodingMap* entry);
    void EmitOpcode(const X86EncodingMap* entry);
    void EmitPrefixAndOpcode(const X86EncodingMap* entry);
    void EmitDisp(uint8_t base, int disp);
    void EmitModrmDisp(uint8_t reg_or_opcode, uint8_t base, int disp);
    void EmitModrmSibDisp(uint8_t reg_or_opcode, uint8_t base, uint8_t index, int scale, int disp);
    void EmitImm(const X86EncodingMap* entry, int imm);
    void EmitOpRegOpcode(const X86EncodingMap* entry, uint8_t reg);
    void EmitOpReg(const X86EncodingMap* entry, uint8_t reg);
    void EmitOpMem(const X86EncodingMap* entry, uint8_t base, int disp);
    void EmitOpArray(const X86EncodingMap* entry, uint8_t base, uint8_t index, int scale, int disp);
    void EmitMemReg(const X86EncodingMap* entry, uint8_t base, int disp, uint8_t reg);
    void EmitMemImm(const X86EncodingMap* entry, uint8_t base, int disp, int32_t imm);
    void EmitRegMem(const X86EncodingMap* entry, uint8_t reg, uint8_t base, int disp);
    void EmitRegArray(const X86EncodingMap* entry, uint8_t reg, uint8_t base, uint8_t index,
                      int scale, int disp);
    void EmitArrayReg(const X86EncodingMap* entry, uint8_t base, uint8_t index, int scale, int disp,
                      uint8_t reg);
    void EmitArrayImm(const X86EncodingMap* entry, uint8_t base, uint8_t index, int scale, int disp,
                      int32_t imm);
    void EmitRegThread(const X86EncodingMap* entry, uint8_t reg, int disp);
    void EmitRegReg(const X86EncodingMap* entry, uint8_t reg1, uint8_t reg2);
    void EmitRegRegImm(const X86EncodingMap* entry, uint8_t reg1, uint8_t reg2, int32_t imm);
    void EmitRegRegImmRev(const X86EncodingMap* entry, uint8_t reg1, uint8_t reg2, int32_t imm);
    void EmitRegMemImm(const X86EncodingMap* entry, uint8_t reg1, uint8_t base, int disp,
                       int32_t imm);
    void EmitMemRegImm(const X86EncodingMap* entry, uint8_t base, int disp, uint8_t reg1, int32_t imm);
    void EmitRegImm(const X86EncodingMap* entry, uint8_t reg, int imm);
    void EmitThreadImm(const X86EncodingMap* entry, int disp, int imm);
    void EmitMovRegImm(const X86EncodingMap* entry, uint8_t reg, int imm);
    void EmitShiftRegImm(const X86EncodingMap* entry, uint8_t reg, int imm);
    void EmitShiftMemImm(const X86EncodingMap* entry, uint8_t base, int disp, int imm);
    void EmitShiftMemCl(const X86EncodingMap* entry, uint8_t base, int displacement, uint8_t cl);
    void EmitShiftRegCl(const X86EncodingMap* entry, uint8_t reg, uint8_t cl);
    void EmitRegCond(const X86EncodingMap* entry, uint8_t reg, uint8_t condition);
    void EmitMemCond(const X86EncodingMap* entry, uint8_t base, int displacement, uint8_t condition);

    /**
     * @brief Used for encoding conditional register to register operation.
     * @param entry The entry in the encoding map for the opcode.
     * @param reg1 The first physical register.
     * @param reg2 The second physical register.
     * @param condition The condition code for operation.
     */
    void EmitRegRegCond(const X86EncodingMap* entry, uint8_t reg1, uint8_t reg2, uint8_t condition);

    /**
     * @brief Used for encoding conditional register to memory operation.
     * @param entry The entry in the encoding map for the opcode.
     * @param reg1 The first physical register.
     * @param base The memory base register.
     * @param displacement The memory displacement.
     * @param condition The condition code for operation.
     */
    void EmitRegMemCond(const X86EncodingMap* entry, uint8_t reg1, uint8_t base, int displacement, uint8_t condition);

    void EmitJmp(const X86EncodingMap* entry, int rel);
    void EmitJcc(const X86EncodingMap* entry, int rel, uint8_t cc);
    void EmitCallMem(const X86EncodingMap* entry, uint8_t base, int disp);
    void EmitCallImmediate(const X86EncodingMap* entry, int disp);
    void EmitCallThread(const X86EncodingMap* entry, int disp);
    void EmitPcRel(const X86EncodingMap* entry, uint8_t reg, int base_or_table, uint8_t index,
                   int scale, int table_or_disp);
    void EmitMacro(const X86EncodingMap* entry, uint8_t reg, int offset);
    void EmitUnimplemented(const X86EncodingMap* entry, LIR* lir);
    void GenFusedLongCmpImmBranch(BasicBlock* bb, RegLocation rl_src1,
                                  int64_t val, ConditionCode ccode);
    void GenConstWide(RegLocation rl_dest, int64_t value);

    static bool ProvidesFullMemoryBarrier(X86OpCode opcode);

    /*
     * @brief generate inline code for fast case of Strng.indexOf.
     * @param info Call parameters
     * @param zero_based 'true' if the index into the string is 0.
     * @returns 'true' if the call was inlined, 'false' if a regular call needs to be
     * generated.
     */
    bool GenInlinedIndexOf(CallInfo* info, bool zero_based);

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
     */
    LIR* OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                           int offset, int check_value, LIR* target);

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
    void GenLongToFP(RegLocation rl_dest, RegLocation rl_src, bool is_double);

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
    void AnalyzeMIR(int opcode, BasicBlock * bb, MIR *mir);

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
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_X86_CODEGEN_X86_H_
