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

#ifndef ART_SRC_COMPILER_COMPILER_IR_H_
#define ART_SRC_COMPILER_COMPILER_IR_H_

#include "codegen/Optimizer.h"
#include <vector>

typedef enum RegisterClass {
    kCoreReg,
    kFPReg,
    kAnyReg,
} RegisterClass;

typedef enum RegLocationType {
    kLocDalvikFrame = 0, // Normal Dalvik register
    kLocPhysReg,
    kLocSpill,
} RegLocationType;

typedef struct RegLocation {
    RegLocationType location:2;
    unsigned wide:1;
    unsigned fp:1;      // Hint for float/double
    u1 lowReg:6;        // First physical register
    u1 highReg:6;       // 2nd physical register (if wide)
    s2 sRegLow;         // SSA name for low Dalvik word
    unsigned home:1;    // Does this represent the home location?
    RegLocationType fpLocation:2; // Used only for non-SSA loc records
    u1 fpLowReg:6;                // Used only for non-SSA loc records
    u1 fpHighReg:6;               // Used only for non-SSA loc records
    int spOffset:17;
} RegLocation;

#define INVALID_SREG (-1)
#define INVALID_REG (0x3F)
#define INVALID_OFFSET (-1)

typedef enum BBType {
    kEntryBlock,
    kDalvikByteCode,
    kExitBlock,
    kExceptionHandling,
    kCatchEntry,
} BBType;

typedef struct LIR {
    int offset;                        // Offset of this instruction
    int dalvikOffset;                  // Offset of Dalvik opcode
    struct LIR* next;
    struct LIR* prev;
    struct LIR* target;
} LIR;

enum ExtendedMIROpcode {
    kMirOpFirst = kNumPackedOpcodes,
    kMirOpPhi = kMirOpFirst,
    kMirOpNullNRangeUpCheck,
    kMirOpNullNRangeDownCheck,
    kMirOpLowerBound,
    kMirOpPunt,
    kMirOpCheckInlinePrediction,        // Gen checks for predicted inlining
    kMirOpLast,
};

struct SSARepresentation;

typedef enum {
    kMIRIgnoreNullCheck = 0,
    kMIRNullCheckOnly,
    kMIRIgnoreRangeCheck,
    kMIRRangeCheckOnly,
    kMIRInlined,                        // Invoke is inlined (ie dead)
    kMIRInlinedPred,                    // Invoke is inlined via prediction
    kMIRCallee,                         // Instruction is inlined from callee
} MIROptimizationFlagPositons;

#define MIR_IGNORE_NULL_CHECK           (1 << kMIRIgnoreNullCheck)
#define MIR_NULL_CHECK_ONLY             (1 << kMIRNullCheckOnly)
#define MIR_IGNORE_RANGE_CHECK          (1 << kMIRIgnoreRangeCheck)
#define MIR_RANGE_CHECK_ONLY            (1 << kMIRRangeCheckOnly)
#define MIR_INLINED                     (1 << kMIRInlined)
#define MIR_INLINED_PRED                (1 << kMIRInlinedPred)
#define MIR_CALLEE                      (1 << kMIRCallee)

typedef struct CallsiteInfo {
    const char* classDescriptor;
    Object* classLoader;
    const Method* method;
    LIR* misPredBranchOver;
} CallsiteInfo;

typedef struct MIR {
    DecodedInstruction dalvikInsn;
    unsigned int width;
    unsigned int offset;
    struct MIR* prev;
    struct MIR* next;
    struct SSARepresentation* ssaRep;
    int OptimizationFlags;
    int seqNum;
    union {
        // Used by the inlined insn from the callee to find the mother method
        const Method* calleeMethod;
        // Used by the inlined invoke to find the class and method pointers
        CallsiteInfo* callsiteInfo;
    } meta;
} MIR;

struct BasicBlockDataFlow;

/* For successorBlockList */
typedef enum BlockListType {
    kNotUsed = 0,
    kCatch,
    kPackedSwitch,
    kSparseSwitch,
} BlockListType;

typedef struct BasicBlock {
    int id;
    bool visited;
    bool hidden;
    unsigned int startOffset;
    const Method* containingMethod;     // For blocks from the callee
    BBType blockType;
    bool needFallThroughBranch;         // For blocks ended due to length limit
    bool isFallThroughFromInvoke;       // True means the block needs alignment
    MIR* firstMIRInsn;
    MIR* lastMIRInsn;
    struct BasicBlock* fallThrough;
    struct BasicBlock* taken;
    struct BasicBlock* iDom;            // Immediate dominator
    struct BasicBlockDataFlow* dataFlowInfo;
    ArenaBitVector* predecessors;
    ArenaBitVector* dominators;
    ArenaBitVector* iDominated;         // Set nodes being immediately dominated
    ArenaBitVector* domFrontier;        // Dominance frontier
    struct {                            // For one-to-many successors like
        BlockListType blockListType;    // switch and exception handling
        GrowableList blocks;
    } successorBlockList;
} BasicBlock;

/*
 * The "blocks" field in "successorBlockList" points to an array of
 * elements with the type "SuccessorBlockInfo".
 * For catch blocks, key is type index for the exception.
 * For swtich blocks, key is the case value.
 */
typedef struct SuccessorBlockInfo {
    BasicBlock* block;
    int key;
} SuccessorBlockInfo;

struct LoopAnalysis;
struct RegisterPool;

typedef enum AssemblerStatus {
    kSuccess,
    kRetryAll,
    kRetryHalve
} AssemblerStatus;

typedef struct CompilationUnit {
    int numInsts;
    int numBlocks;
    GrowableList blockList;
    const Method *method;
    LIR* firstLIRInsn;
    LIR* lastLIRInsn;
    LIR* literalList;                   // Constants
    LIR* classPointerList;              // Relocatable
    int numClassPointers;
    LIR* chainCellOffsetLIR;
    int disableOpt;
    int headerSize;                     // bytes before the first code ptr
    int dataOffset;                     // starting offset of literal pool
    int totalSize;                      // header + code size
    AssemblerStatus assemblerStatus;    // Success or fix and retry
    int assemblerRetries;
    std::vector<short> codeBuffer;
    std::vector<uint32_t> mappingTable;
    bool printMe;
    bool printMeVerbose;
    bool dumpCFG;
    bool hasClassLiterals;              // Contains class ptrs used as literals
    bool hasLoop;                       // Contains a loop
    bool hasInvoke;                     // Contains an invoke instruction
    bool heapMemOp;                     // Mark mem ops for self verification
    bool usesLinkRegister;              // For self-verification only
    bool methodTraceSupport;            // For TraceView profiling
    struct RegisterPool* regPool;
    int optRound;                       // round number to tell an LIR's age
    OatInstructionSetType instructionSet;
    /* Number of total regs used in the whole cUnit after SSA transformation */
    int numSSARegs;
    /* Map SSA reg i to the Dalvik[15..0]/Sub[31..16] pair. */
    GrowableList* ssaToDalvikMap;

    /* The following are new data structures to support SSA representations */
    /* Map original Dalvik reg i to the SSA[15..0]/Sub[31..16] pair */
    int* dalvikToSSAMap;                // length == method->registersSize
    int* SSALastDefs;                   // length == method->registersSize
    ArenaBitVector* isConstantV;        // length == numSSAReg
    int* constantValues;                // length == numSSAReg

    /* Map SSA names to location */
    RegLocation* regLocation;
    int sequenceNumber;

    /*
     * Set to the Dalvik PC of the switch instruction if it has more than
     * MAX_CHAINED_SWITCH_CASES cases.
     */
    const u2* switchOverflowPad;

    int numReachableBlocks;
    int numDalvikRegisters;             // method->registersSize + inlined
    BasicBlock* entryBlock;
    BasicBlock* exitBlock;
    BasicBlock* curBlock;
    BasicBlock* nextCodegenBlock;       // for extended trace codegen
    GrowableList dfsOrder;
    GrowableList domPostOrderTraversal;
    GrowableList throwLaunchpads;
    ArenaBitVector* tryBlockAddr;
    ArenaBitVector** defBlockMatrix;    // numDalvikRegister x numBlocks
    ArenaBitVector* tempBlockV;
    ArenaBitVector* tempDalvikRegisterV;
    ArenaBitVector* tempSSARegisterV;   // numSSARegs
    bool printSSANames;
    void* blockLabelList;
    bool quitLoopMode;                  // cold path/complex bytecode
    int preservedRegsUsed;              // How many callee save regs used
    /*
     * Frame layout details.
     * NOTE: for debug support it will be necessary to add a structure
     * to map the Dalvik virtual registers to the promoted registers.
     * NOTE: "num" fields are in 4-byte words, "Size" and "Offset" in bytes.
     */
    int numIns;
    int numOuts;
    int numRegs;            // Unlike struct Method, does not include ins
    int numSpills;          // NOTE: includes numFPSpills
    int numFPSpills;
    int numPadding;         // # of 4-byte padding cells
    int regsOffset;         // sp-relative offset to beginning of Dalvik regs
    int insOffset;          // sp-relative offset to beginning of Dalvik ins
    int frameSize;
    unsigned int coreSpillMask;
    unsigned int fpSpillMask;
    unsigned int attrs;
    /*
     * CLEANUP/RESTRUCTURE: The code generation utilities don't have a built-in
     * mechanism to propogate the original Dalvik opcode address to the
     * associated generated instructions.  For the trace compiler, this wasn't
     * necessary because the interpreter handled all throws and debugging
     * requests.  For now we'll handle this by placing the Dalvik offset
     * in the CompilationUnit struct before codegen for each instruction.
     * The low-level LIR creation utilites will pull it from here.  Should
     * be rewritten.
     */
     int currentDalvikOffset;
     GrowableList switchTables;
     GrowableList fillArrayData;
     const u2* insns;
     u4 insnsSize;
} CompilationUnit;

BasicBlock* oatNewBB(BBType blockType, int blockId);

void oatAppendMIR(BasicBlock* bb, MIR* mir);

void oatPrependMIR(BasicBlock* bb, MIR* mir);

void oatInsertMIRAfter(BasicBlock* bb, MIR* currentMIR, MIR* newMIR);

void oatAppendLIR(CompilationUnit* cUnit, LIR* lir);

void oatInsertLIRBefore(LIR* currentLIR, LIR* newLIR);

void oatInsertLIRAfter(LIR* currentLIR, LIR* newLIR);

/* Debug Utilities */
void oatDumpCompilationUnit(CompilationUnit* cUnit);

#endif // ART_SRC_COMPILER_COMPILER_IR_H_
