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
#include "CompilerUtility.h"
#include <vector>
#include "oat_compilation_unit.h"

namespace art {

#define SLOW_FIELD_PATH (cUnit->enableDebug & (1 << kDebugSlowFieldPath))
#define SLOW_INVOKE_PATH (cUnit->enableDebug & (1 << kDebugSlowInvokePath))
#define SLOW_STRING_PATH (cUnit->enableDebug & (1 << kDebugSlowStringPath))
#define SLOW_TYPE_PATH (cUnit->enableDebug & (1 << kDebugSlowTypePath))
#define EXERCISE_SLOWEST_FIELD_PATH (cUnit->enableDebug & \
    (1 << kDebugSlowestFieldPath))
#define EXERCISE_SLOWEST_STRING_PATH (cUnit->enableDebug & \
    (1 << kDebugSlowestStringPath))
#define EXERCISE_RESOLVE_METHOD (cUnit->enableDebug & \
    (1 << kDebugExerciseResolveMethod))

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

typedef struct PromotionMap {
   RegLocationType coreLocation:3;
   u1 coreReg;
   RegLocationType fpLocation:3;
   u1 fpReg;
   bool firstInPair;
} PromotionMap;

typedef struct RegLocation {
    RegLocationType location:3;
    unsigned wide:1;
    unsigned defined:1;   // Do we know the type?
    unsigned fp:1;        // Floating point?
    unsigned core:1;      // Non-floating point?
    unsigned highWord:1;  // High word of pair?
    unsigned home:1;      // Does this represent the home location?
    u1 lowReg;            // First physical register
    u1 highReg;           // 2nd physical register (if wide)
    s2 sRegLow;           // SSA name for low Dalvik word
} RegLocation;

 /*
 * Data structure tracking the mapping between a Dalvik register (pair) and a
 * native register (pair). The idea is to reuse the previously loaded value
 * if possible, otherwise to keep the value in a native register as long as
 * possible.
 */
typedef struct RegisterInfo {
    int reg;                    // Reg number
    bool inUse;                 // Has it been allocated?
    bool isTemp;                // Can allocate as temp?
    bool pair;                  // Part of a register pair?
    int partner;                // If pair, other reg of pair
    bool live;                  // Is there an associated SSA name?
    bool dirty;                 // If live, is it dirty?
    int sReg;                   // Name of live value
    struct LIR *defStart;       // Starting inst in last def sequence
    struct LIR *defEnd;         // Ending inst in last def sequence
} RegisterInfo;

typedef struct RegisterPool {
    int numCoreRegs;
    RegisterInfo *coreRegs;
    int nextCoreReg;
    int numFPRegs;
    RegisterInfo *FPRegs;
    int nextFPReg;
} RegisterPool;

#define INVALID_SREG (-1)
#define INVALID_VREG (0xFFFFU)
#define INVALID_REG (0xFF)
#define INVALID_OFFSET (-1)

/*
 * Some code patterns cause the generation of excessively large
 * methods - in particular initialization sequences.  There isn't much
 * benefit in optimizing these methods, and the cost can be very high.
 * We attempt to identify these cases, and avoid performing most dataflow
 * analysis.  Two thresholds are used - one for known initializers and one
 * for everything else.
 */
#define MANY_BLOCKS_INITIALIZER 1000 /* Threshold for switching dataflow off */
#define MANY_BLOCKS 4000 /* Non-initializer threshold */

typedef enum BBType {
    kEntryBlock,
    kDalvikByteCode,
    kExitBlock,
    kExceptionHandling,
    kCatchEntry,
} BBType;

/* Utility macros to traverse the LIR list */
#define NEXT_LIR(lir) (lir->next)
#define PREV_LIR(lir) (lir->prev)

#define NEXT_LIR_LVALUE(lir) (lir)->next
#define PREV_LIR_LVALUE(lir) (lir)->prev

typedef struct LIR {
    int offset;                        // Offset of this instruction
    int dalvikOffset;                  // Offset of Dalvik opcode
    struct LIR* next;
    struct LIR* prev;
    struct LIR* target;
    int opcode;
    int operands[4];            // [0..3] = [dest, src1, src2, extra]
    struct {
        bool isNop:1;           // LIR is optimized away
        bool pcRelFixup:1;      // May need pc-relative fixup
        unsigned int age:4;     // default is 0, set lazily by the optimizer
        unsigned int size:5;    // in bytes
        unsigned int unused:21;
    } flags;
    int aliasInfo;              // For Dalvik register & litpool disambiguation
    u8 useMask;                 // Resource mask for use
    u8 defMask;                 // Resource mask for def
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
    kMIRIgnoreSuspendCheck,
} MIROptimizationFlagPositons;

#define MIR_IGNORE_NULL_CHECK           (1 << kMIRIgnoreNullCheck)
#define MIR_NULL_CHECK_ONLY             (1 << kMIRNullCheckOnly)
#define MIR_IGNORE_RANGE_CHECK          (1 << kMIRIgnoreRangeCheck)
#define MIR_RANGE_CHECK_ONLY            (1 << kMIRRangeCheckOnly)
#define MIR_INLINED                     (1 << kMIRInlined)
#define MIR_INLINED_PRED                (1 << kMIRInlinedPred)
#define MIR_CALLEE                      (1 << kMIRCallee)
#define MIR_IGNORE_SUSPEND_CHECK        (1 << kMIRIgnoreSuspendCheck)

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
    int optimizationFlags;
    int seqNum;
    union {
        // Used by the inlined insn from the callee to find the mother method
        const Method* calleeMethod;
        // Used by the inlined invoke to find the class and method pointers
        CallsiteInfo* callsiteInfo;
        // Used to quickly locate all Phi opcodes
        struct MIR* phiNext;
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
    int dfsId;
    bool visited;
    bool hidden;
    bool catchEntry;
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
    GrowableList* predecessors;
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
struct ArenaMemBlock;
struct Memstats;

typedef enum AssemblerStatus {
    kSuccess,
    kRetryAll,
    kRetryHalve
} AssemblerStatus;

#define NOTVISITED (-1)

typedef struct CompilationUnit {
    int numInsts;
    int numBlocks;
    GrowableList blockList;
    Compiler* compiler;            // Compiler driving this compiler
    ClassLinker* class_linker;     // Linker to resolve fields and methods
    const DexFile* dex_file;       // DexFile containing the method being compiled
    DexCache* dex_cache;           // DexFile's corresponding cache
    const ClassLoader* class_loader;  // compiling method's class loader
    uint32_t method_idx;                // compiling method's index into method_ids of DexFile
    const DexFile::CodeItem* code_item;  // compiling method's DexFile code_item
    uint32_t access_flags;              // compiling method's access flags
    const char* shorty;                 // compiling method's shorty
    LIR* firstLIRInsn;
    LIR* lastLIRInsn;
    LIR* literalList;                   // Constants
    LIR* classPointerList;              // Relocatable
    int numClassPointers;
    LIR* chainCellOffsetLIR;
    uint32_t disableOpt;                // optControlVector flags
    uint32_t enableDebug;               // debugControlVector flags
    int headerSize;                     // bytes before the first code ptr
    int dataOffset;                     // starting offset of literal pool
    int totalSize;                      // header + code size
    AssemblerStatus assemblerStatus;    // Success or fix and retry
    int assemblerRetries;
    std::vector<uint16_t> codeBuffer;
    std::vector<uint32_t> mappingTable;
    std::vector<uint16_t> coreVmapTable;
    std::vector<uint16_t> fpVmapTable;
    bool genDebugger;                   // Generate code for debugger
    bool printMe;
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
    int* phiAliasMap;                   // length == numSSAReg
    MIR* phiList;

    /* Map SSA names to location */
    RegLocation* regLocation;
    int sequenceNumber;

    /* Keep track of Dalvik vReg to physical register mappings */
    PromotionMap* promotionMap;

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
    GrowableList dfsPostOrder;
    GrowableList domPostOrderTraversal;
    GrowableList throwLaunchpads;
    GrowableList suspendLaunchpads;
    int* iDomList;
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
    int numRegs;            // Unlike numDalvikRegisters, does not include ins
    int numCoreSpills;
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
     * mechanism to propagate the original Dalvik opcode address to the
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
     bool disableDataflow; // Skip dataflow analysis if possible
     std::map<unsigned int, BasicBlock*> blockMap; // findBlock lookup cache
     std::map<unsigned int, LIR*> boundaryMap; // boundary lookup cache
     int defCount;         // Used to estimate number of SSA names
     std::string* compilerMethodMatch;
     bool compilerFlipMatch;
     struct ArenaMemBlock* arenaHead;
     struct ArenaMemBlock* currentArena;
     int numArenaBlocks;
     struct Memstats* mstats;
} CompilationUnit;

typedef enum OpSize {
    kWord,
    kLong,
    kSingle,
    kDouble,
    kUnsignedHalf,
    kSignedHalf,
    kUnsignedByte,
    kSignedByte,
} OpSize;

typedef enum OpKind {
    kOpMov,
    kOpMvn,
    kOpCmp,
    kOpLsl,
    kOpLsr,
    kOpAsr,
    kOpRor,
    kOpNot,
    kOpAnd,
    kOpOr,
    kOpXor,
    kOpNeg,
    kOpAdd,
    kOpAdc,
    kOpSub,
    kOpSbc,
    kOpRsub,
    kOpMul,
    kOpDiv,
    kOpRem,
    kOpBic,
    kOpCmn,
    kOpTst,
    kOpBkpt,
    kOpBlx,
    kOpPush,
    kOpPop,
    kOp2Char,
    kOp2Short,
    kOp2Byte,
    kOpCondBr,
    kOpUncondBr,
    kOpBx,
    kOpInvalid,
} OpKind;

std::ostream& operator<<(std::ostream& os, const OpKind& kind);

typedef enum ConditionCode {
    kCondEq,
    kCondNe,
    kCondCs,
    kCondCc,
    kCondMi,
    kCondPl,
    kCondVs,
    kCondVc,
    kCondHi,
    kCondLs,
    kCondGe,
    kCondLt,
    kCondGt,
    kCondLe,
    kCondAl,
    kCondNv,
} ConditionCode;

typedef enum ThrowKind {
    kThrowNullPointer,
    kThrowDivZero,
    kThrowArrayBounds,
    kThrowVerificationError,
    kThrowNegArraySize,
    kThrowNoSuchMethod,
    kThrowStackOverflow,
} ThrowKind;

typedef struct SwitchTable {
    int offset;
    const u2* table;            // Original dex table
    int vaddr;                  // Dalvik offset of switch opcode
    LIR* anchor;                // Reference instruction for relative offsets
    LIR** targets;              // Array of case targets
} SwitchTable;

typedef struct FillArrayData {
    int offset;
    const u2* table;           // Original dex table
    int size;
    int vaddr;                 // Dalvik offset of FILL_ARRAY_DATA opcode
} FillArrayData;


BasicBlock* oatNewBB(CompilationUnit* cUnit, BBType blockType, int blockId);

void oatAppendMIR(BasicBlock* bb, MIR* mir);

void oatPrependMIR(BasicBlock* bb, MIR* mir);

void oatInsertMIRAfter(BasicBlock* bb, MIR* currentMIR, MIR* newMIR);

void oatAppendLIR(CompilationUnit* cUnit, LIR* lir);

void oatInsertLIRBefore(LIR* currentLIR, LIR* newLIR);

void oatInsertLIRAfter(LIR* currentLIR, LIR* newLIR);

/* Debug Utilities */
void oatDumpCompilationUnit(CompilationUnit* cUnit);

}  // namespace art

#endif // ART_SRC_COMPILER_COMPILER_IR_H_
