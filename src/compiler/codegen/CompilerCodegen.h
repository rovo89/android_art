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

#ifndef ART_SRC_COMPILER_COMPILERCODEGEN_H_
#define ART_SRC_COMPILER_COMPILERCODEGEN_H_

#include "../CompilerIR.h"

namespace art {


// Set to 1 to measure cost of suspend check
#define NO_SUSPEND 0

/* Bit flags describing the behavior of native opcodes (Arm/Mips/x86 combined) */
enum OpFeatureFlags {
  kIsBranch = 0,
  kNoOperand,
  kIsUnaryOp,
  kIsBinaryOp,
  kIsTertiaryOp,
  kIsQuadOp,
  kIsQuinOp,
  kIsSextupleOp,
  kIsIT,
  kMemLoad,
  kMemStore,
  kPCRelFixup, // x86 FIXME: add NEEDS_FIXUP to instruction attributes
  kRegDef0,
  kRegDef1,
  kRegDefA,
  kRegDefD,
  kRegDefFPCSList0,
  kRegDefFPCSList2,
  kRegDefList0,
  kRegDefList1,
  kRegDefList2,
  kRegDefLR,
  kRegDefSP,
  kRegUse0,
  kRegUse1,
  kRegUse2,
  kRegUse3,
  kRegUse4,
  kRegUseA,
  kRegUseC,
  kRegUseD,
  kRegUseFPCSList0,
  kRegUseFPCSList2,
  kRegUseList0,
  kRegUseList1,
  kRegUseLR,
  kRegUsePC,
  kRegUseSP,
  kSetsCCodes,
  kUsesCCodes
};

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

/* Common combo register usage patterns */
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

LIR* rawLIR(CompilationUnit* cUnit, int dalvikOffset, int opcode, int op0 = 0,
            int op1 = 0, int op2 = 0, int op3 = 0, int op4 = 0,
            LIR* target = NULL);

int oatGetInsnSize(LIR* lir);

void genFusedLongCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir);
void genFusedFPCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                         bool gtBias, bool isDouble);

CallInfo* oatNewCallInfo(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                         InvokeType type, bool isRange);

/* Lower middle-level IR to low-level IR for the whole method */
void oatMethodMIR2LIR(CompilationUnit* cUnit);

/* Bitcode conversions */
void oatMethodMIR2Bitcode(CompilationUnit* cUnit);
void oatMethodBitcode2LIR(CompilationUnit* cUnit);

/* Lower middle-level IR to low-level IR for the simple methods */
void oatSpecialMIR2LIR(CompilationUnit* cUnit, SpecialCaseHandler specialCase );

/* Assemble LIR into machine code */
void oatAssembleLIR(CompilationUnit* cUnit);
AssemblerStatus oatAssembleInstructions(CompilationUnit* cUnit,
                                        intptr_t startAddr);
void oatAssignOffsets(CompilationUnit* cUnit);
int oatAssignInsnOffsets(CompilationUnit* cUnit);

/* Implemented in the codegen/<target>/ArchUtility.c */
void oatCodegenDump(CompilationUnit* cUnit);
void oatDumpPromotionMap(CompilationUnit* cUnit);
std::string buildInsnString(const char* fmt, LIR* lir,
                            unsigned char* baseAddr);


/* Implemented in codegen/<target>/Ralloc.c */
void oatSimpleRegAlloc(CompilationUnit* cUnit);

/* Implemented in codegen/<target>/Thumb<version>Util.c */
void oatInitializeRegAlloc(CompilationUnit* cUnit);

/* Implemented in codegen/<target>/<target_variant>/ArchVariant.c */
InstructionSet oatInstructionSet();

/*
 * Implemented in codegen/<target>/<target_variant>/ArchVariant.c
 * Architecture-specific initializations and checks
 */
bool oatArchVariantInit(void);

/* Implemented in codegen/<target>/<target_variant>/ArchVariant.c */
int oatTargetOptHint(int key);

/* Implemented in codegen/<target>/<target_variant>/ArchVariant.c */
void oatGenMemBarrier(CompilationUnit* cUnit, int barrierKind);

LIR* genRegMemCheck(CompilationUnit* cUnit, ConditionCode cCode,
                    int reg1, int base, int offset, ThrowKind kind);
LIR* opThreadMem(CompilationUnit* cUnit, OpKind op, int threadOffset);
LIR* opMem(CompilationUnit* cUnit, OpKind op, int rBase, int disp);
LIR* storeBaseIndexedDisp(CompilationUnit *cUnit,
                          int rBase, int rIndex, int scale, int displacement,
                          int rSrc, int rSrcHi, OpSize size, int sReg);
LIR* opRegMem(CompilationUnit *cUnit, OpKind op, int rDest, int rBase, int offset);
LIR* opCmpBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
                 int src2, LIR* target);
void oatSetupRegMask(CompilationUnit* cUnit, u8* mask, int reg);
u8 oatGetRegMaskCommon(CompilationUnit* cUnit, int reg);
void setupTargetResourceMasks(CompilationUnit* cUnit, LIR* lir);
RegLocation genDivRem(CompilationUnit* cUnit, RegLocation rlDest, int regLo, int regHi, bool isDiv);
RegLocation genDivRemLit(CompilationUnit* cUnit, RegLocation rlDest, int regLo, int lit, bool isDiv);
void markGCCard(CompilationUnit* cUnit, int valReg, int tgtAddrReg);
bool genInlinedMinMaxInt(CompilationUnit *cUnit, CallInfo* info, bool isMin);
void opLea(CompilationUnit* cUnit, int rBase, int reg1, int reg2, int scale, int offset);
void opTlsCmp(CompilationUnit* cUnit, int offset, int val);
bool genInlinedSqrt(CompilationUnit* cUnit, CallInfo* info);
bool genInlinedCas32(CompilationUnit* cUnit, CallInfo* info, bool need_write_barrier);
LIR* opPcRelLoad(CompilationUnit* cUnit, int reg, LIR* target);
LIR* opVldm(CompilationUnit* cUnit, int rBase, int count);
LIR* opVstm(CompilationUnit* cUnit, int rBase, int count);
void genMultiplyByTwoBitMultiplier(CompilationUnit* cUnit, RegLocation rlSrc,
                                   RegLocation rlResult, int lit,
                                   int firstBit, int secondBit);
RegLocation inlineTarget(CompilationUnit* cUnit, CallInfo* info);
RegLocation inlineTargetWide(CompilationUnit* cUnit, CallInfo* info);
void genDivZeroCheck(CompilationUnit* cUnit, int regLo, int regHi);
LIR* genImmedCheck(CompilationUnit* cUnit, ConditionCode cCode,
                   int reg, int immVal, ThrowKind kind);
LIR* opTestSuspend(CompilationUnit* cUnit, LIR* target);
LIR* opDecAndBranch(CompilationUnit* cUnit, ConditionCode cCode, int reg, LIR* target);
LIR* opIT(CompilationUnit* cUnit, ArmConditionCode cond, const char* guide);
uint64_t getPCUseDefEncoding();
uint64_t getRegMaskCommon(CompilationUnit* cUnit, int reg);
int s2d(int lowReg, int highReg);
bool fpReg(int reg);
bool singleReg(int reg);
bool doubleReg(int reg);
uint32_t fpRegMask();
bool sameRegType(int reg1, int reg2);
int targetReg(SpecialTargetRegister reg);
RegLocation locCReturn();
RegLocation locCReturnWide();
RegLocation locCReturnFloat();
RegLocation locCReturnDouble();

}  // namespace art

#endif  // ART_SRC_COMPILER_COMPILERCODEGEN_H_
