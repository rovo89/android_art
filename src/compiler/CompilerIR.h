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

#include <vector>

#include "codegen/Optimizer.h"
#include "CompilerUtility.h"
#include "oat_compilation_unit.h"
#include "safe_map.h"
#if defined(ART_USE_QUICK_COMPILER)
#include "greenland/ir_builder.h"
#include "llvm/Module.h"
#endif

namespace art {

#define SLOW_FIELD_PATH (cUnit->enableDebug & (1 << kDebugSlowFieldPath))
#define SLOW_INVOKE_PATH (cUnit->enableDebug & (1 << kDebugSlowInvokePath))
#define SLOW_STRING_PATH (cUnit->enableDebug & (1 << kDebugSlowStringPath))
#define SLOW_TYPE_PATH (cUnit->enableDebug & (1 << kDebugSlowTypePath))
#define EXERCISE_SLOWEST_STRING_PATH (cUnit->enableDebug & \
  (1 << kDebugSlowestStringPath))

// Minimum field size to contain Dalvik vReg number
#define VREG_NUM_WIDTH 16

enum RegisterClass {
  kCoreReg,
  kFPReg,
  kAnyReg,
};

enum RegLocationType {
  kLocDalvikFrame = 0, // Normal Dalvik register
  kLocPhysReg,
  kLocCompilerTemp,
  kLocInvalid
};

struct PromotionMap {
  RegLocationType coreLocation:3;
  u1 coreReg;
  RegLocationType fpLocation:3;
  u1 fpReg;
  bool firstInPair;
};

struct RegLocation {
  RegLocationType location:3;
  unsigned wide:1;
  unsigned defined:1;   // Do we know the type?
  unsigned isConst:1;   // Constant, value in cUnit->constantValues[]
  unsigned fp:1;        // Floating point?
  unsigned core:1;      // Non-floating point?
  unsigned ref:1;       // Something GC cares about
  unsigned highWord:1;  // High word of pair?
  unsigned home:1;      // Does this represent the home location?
  u1 lowReg;            // First physical register
  u1 highReg;           // 2nd physical register (if wide)
  int32_t sRegLow;      // SSA name for low Dalvik word
  int32_t origSReg;     // TODO: remove after Bitcode gen complete
                        // and consolodate usage w/ sRegLow
};

struct CompilerTemp {
  int sReg;
  ArenaBitVector* bv;
};

struct CallInfo {
  int numArgWords;      // Note: word count, not arg count
  RegLocation* args;    // One for each word of arguments
  RegLocation result;   // Eventual target of MOVE_RESULT
  int optFlags;
  InvokeType type;
  uint32_t dexIdx;
  uint32_t index;       // Method idx for invokes, type idx for FilledNewArray
  uintptr_t directCode;
  uintptr_t directMethod;
  RegLocation target;    // Target of following move_result
  bool skipThis;
  bool isRange;
  int offset;            // Dalvik offset
};

 /*
 * Data structure tracking the mapping between a Dalvik register (pair) and a
 * native register (pair). The idea is to reuse the previously loaded value
 * if possible, otherwise to keep the value in a native register as long as
 * possible.
 */
struct RegisterInfo {
  int reg;                    // Reg number
  bool inUse;                 // Has it been allocated?
  bool isTemp;                // Can allocate as temp?
  bool pair;                  // Part of a register pair?
  int partner;                // If pair, other reg of pair
  bool live;                  // Is there an associated SSA name?
  bool dirty;                 // If live, is it dirty?
  int sReg;                   // Name of live value
  LIR *defStart;              // Starting inst in last def sequence
  LIR *defEnd;                // Ending inst in last def sequence
};

struct RegisterPool {
  int numCoreRegs;
  RegisterInfo *coreRegs;
  int nextCoreReg;
  int numFPRegs;
  RegisterInfo *FPRegs;
  int nextFPReg;
};

#define INVALID_SREG (-1)
#define INVALID_VREG (0xFFFFU)
#define INVALID_REG (0xFF)

/* SSA encodings for special registers */
#define SSA_METHOD_BASEREG (-2)
/* First compiler temp basereg, grows smaller */
#define SSA_CTEMP_BASEREG (SSA_METHOD_BASEREG - 1)

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

enum BBType {
  kEntryBlock,
  kDalvikByteCode,
  kExitBlock,
  kExceptionHandling,
  kDead,
};

/* Utility macros to traverse the LIR list */
#define NEXT_LIR(lir) (lir->next)
#define PREV_LIR(lir) (lir->prev)

struct LIR {
  int offset;                        // Offset of this instruction
  int dalvikOffset;                  // Offset of Dalvik opcode
  LIR* next;
  LIR* prev;
  LIR* target;
  int opcode;
  int operands[5];            // [0..4] = [dest, src1, src2, extra, extra2]
  struct {
    bool isNop:1;           // LIR is optimized away
    bool pcRelFixup:1;      // May need pc-relative fixup
    unsigned int size:5;    // in bytes
    unsigned int unused:25;
  } flags;
  int aliasInfo;              // For Dalvik register & litpool disambiguation
  u8 useMask;                 // Resource mask for use
  u8 defMask;                 // Resource mask for def
};

enum ExtendedMIROpcode {
  kMirOpFirst = kNumPackedOpcodes,
  kMirOpPhi = kMirOpFirst,
  kMirOpCopy,
  kMirOpFusedCmplFloat,
  kMirOpFusedCmpgFloat,
  kMirOpFusedCmplDouble,
  kMirOpFusedCmpgDouble,
  kMirOpFusedCmpLong,
  kMirOpNop,
  kMirOpNullCheck,
  kMirOpRangeCheck,
  kMirOpDivZeroCheck,
  kMirOpCheck,
  kMirOpLast,
};

struct SSARepresentation;

enum MIROptimizationFlagPositons {
  kMIRIgnoreNullCheck = 0,
  kMIRNullCheckOnly,
  kMIRIgnoreRangeCheck,
  kMIRRangeCheckOnly,
  kMIRInlined,                        // Invoke is inlined (ie dead)
  kMIRInlinedPred,                    // Invoke is inlined via prediction
  kMIRCallee,                         // Instruction is inlined from callee
  kMIRIgnoreSuspendCheck,
  kMIRDup,
  kMIRMark,                           // Temporary node mark
};

#define MIR_IGNORE_NULL_CHECK           (1 << kMIRIgnoreNullCheck)
#define MIR_NULL_CHECK_ONLY             (1 << kMIRNullCheckOnly)
#define MIR_IGNORE_RANGE_CHECK          (1 << kMIRIgnoreRangeCheck)
#define MIR_RANGE_CHECK_ONLY            (1 << kMIRRangeCheckOnly)
#define MIR_INLINED                     (1 << kMIRInlined)
#define MIR_INLINED_PRED                (1 << kMIRInlinedPred)
#define MIR_CALLEE                      (1 << kMIRCallee)
#define MIR_IGNORE_SUSPEND_CHECK        (1 << kMIRIgnoreSuspendCheck)
#define MIR_DUP                         (1 << kMIRDup)
#define MIR_MARK                        (1 << kMIRMark)

struct Checkstats {
  int nullChecks;
  int nullChecksEliminated;
  int rangeChecks;
  int rangeChecksEliminated;
};

struct MIR {
  DecodedInstruction dalvikInsn;
  unsigned int width;
  unsigned int offset;
  MIR* prev;
  MIR* next;
  SSARepresentation* ssaRep;
  int optimizationFlags;
  union {
    // Used to quickly locate all Phi opcodes
    MIR* phiNext;
    // Establish link between two halves of throwing instructions
    MIR* throwInsn;
  } meta;
};

struct BasicBlockDataFlow;

/* For successorBlockList */
enum BlockListType {
  kNotUsed = 0,
  kCatch,
  kPackedSwitch,
  kSparseSwitch,
};

struct BasicBlock {
  int id;
  int dfsId;
  bool visited;
  bool hidden;
  bool catchEntry;
  bool explicitThrow;
  bool conditionalBranch;
#if defined(ART_USE_QUICK_COMPILER)
  bool hasReturn;
#endif
  uint16_t startOffset;
  uint16_t nestingDepth;
  BBType blockType;
  MIR* firstMIRInsn;
  MIR* lastMIRInsn;
  BasicBlock* fallThrough;
  BasicBlock* taken;
  BasicBlock* iDom;            // Immediate dominator
  BasicBlockDataFlow* dataFlowInfo;
  GrowableList* predecessors;
  ArenaBitVector* dominators;
  ArenaBitVector* iDominated;         // Set nodes being immediately dominated
  ArenaBitVector* domFrontier;        // Dominance frontier
  struct {                            // For one-to-many successors like
    BlockListType blockListType;    // switch and exception handling
    GrowableList blocks;
  } successorBlockList;
};

/*
 * The "blocks" field in "successorBlockList" points to an array of
 * elements with the type "SuccessorBlockInfo".
 * For catch blocks, key is type index for the exception.
 * For swtich blocks, key is the case value.
 */
struct SuccessorBlockInfo {
  BasicBlock* block;
  int key;
};

struct LoopAnalysis;
struct RegisterPool;
struct ArenaMemBlock;
struct Memstats;

enum AssemblerStatus {
  kSuccess,
  kRetryAll,
};

#define NOTVISITED (-1)

struct CompilationUnit {
  CompilationUnit()
    : numBlocks(0),
      compiler(NULL),
      class_linker(NULL),
      dex_file(NULL),
      class_loader(NULL),
      method_idx(0),
      code_item(NULL),
      access_flags(0),
      invoke_type(kDirect),
      shorty(NULL),
      firstLIRInsn(NULL),
      lastLIRInsn(NULL),
      literalList(NULL),
      methodLiteralList(NULL),
      codeLiteralList(NULL),
      disableOpt(0),
      enableDebug(0),
      dataOffset(0),
      totalSize(0),
      assemblerStatus(kSuccess),
      assemblerRetries(0),
      genDebugger(false),
      printMe(false),
      hasLoop(false),
      hasInvoke(false),
      qdMode(false),
      regPool(NULL),
      instructionSet(kNone),
      numSSARegs(0),
      ssaBaseVRegs(NULL),
      ssaSubscripts(NULL),
      ssaStrings(NULL),
      vRegToSSAMap(NULL),
      SSALastDefs(NULL),
      isConstantV(NULL),
      constantValues(NULL),
      phiAliasMap(NULL),
      phiList(NULL),
      regLocation(NULL),
      promotionMap(NULL),
      methodSReg(0),
      numReachableBlocks(0),
      numDalvikRegisters(0),
      entryBlock(NULL),
      exitBlock(NULL),
      curBlock(NULL),
      iDomList(NULL),
      tryBlockAddr(NULL),
      defBlockMatrix(NULL),
      tempBlockV(NULL),
      tempDalvikRegisterV(NULL),
      tempSSARegisterV(NULL),
      tempSSABlockIdV(NULL),
      blockLabelList(NULL),
      numIns(0),
      numOuts(0),
      numRegs(0),
      numCoreSpills(0),
      numFPSpills(0),
      numCompilerTemps(0),
      frameSize(0),
      coreSpillMask(0U),
      fpSpillMask(0U),
      attrs(0U),
      currentDalvikOffset(0),
      insns(NULL),
      insnsSize(0U),
      disableDataflow(false),
      defCount(0),
      compilerFlipMatch(false),
      arenaHead(NULL),
      currentArena(NULL),
      numArenaBlocks(0),
      mstats(NULL),
      checkstats(NULL),
#if defined(ART_USE_QUICK_COMPILER)
      genBitcode(false),
      context(NULL),
      module(NULL),
      func(NULL),
      intrinsic_helper(NULL),
      irb(NULL),
      placeholderBB(NULL),
      entryBB(NULL),
      entryTargetBB(NULL),
      tempName(0),
      numShadowFrameEntries(0),
      shadowMap(NULL),
#endif
#ifndef NDEBUG
      liveSReg(0),
#endif
      opcodeCount(NULL) {}

  int numBlocks;
  GrowableList blockList;
  Compiler* compiler;            // Compiler driving this compiler
  ClassLinker* class_linker;     // Linker to resolve fields and methods
  const DexFile* dex_file;       // DexFile containing the method being compiled
  jobject class_loader;          // compiling method's class loader
  uint32_t method_idx;                // compiling method's index into method_ids of DexFile
  const DexFile::CodeItem* code_item;  // compiling method's DexFile code_item
  uint32_t access_flags;              // compiling method's access flags
  InvokeType invoke_type;             // compiling method's invocation type
  const char* shorty;                 // compiling method's shorty
  LIR* firstLIRInsn;
  LIR* lastLIRInsn;
  LIR* literalList;                   // Constants
  LIR* methodLiteralList;             // Method literals requiring patching
  LIR* codeLiteralList;               // Code literals requiring patching
  uint32_t disableOpt;                // optControlVector flags
  uint32_t enableDebug;               // debugControlVector flags
  int dataOffset;                     // starting offset of literal pool
  int totalSize;                      // header + code size
  AssemblerStatus assemblerStatus;    // Success or fix and retry
  int assemblerRetries;
  std::vector<uint8_t> codeBuffer;
  /*
   * Holds mapping from native PC to dex PC for safepoints where we may deoptimize.
   * Native PC is on the return address of the safepointed operation.  Dex PC is for
   * the instruction being executed at the safepoint.
   */
  std::vector<uint32_t> pc2dexMappingTable;
  /*
   * Holds mapping from Dex PC to native PC for catch entry points.  Native PC and Dex PC
   * immediately preceed the instruction.
   */
  std::vector<uint32_t> dex2pcMappingTable;
  std::vector<uint32_t> combinedMappingTable;
  std::vector<uint32_t> coreVmapTable;
  std::vector<uint32_t> fpVmapTable;
  std::vector<uint8_t> nativeGcMap;
  bool genDebugger;                   // Generate code for debugger
  bool printMe;
  bool hasLoop;                       // Contains a loop
  bool hasInvoke;                     // Contains an invoke instruction
  bool qdMode;                        // Compile for code size/compile time
  RegisterPool* regPool;
  InstructionSet instructionSet;
  /* Number of total regs used in the whole cUnit after SSA transformation */
  int numSSARegs;
  /* Map SSA reg i to the base virtual register/subscript */
  GrowableList* ssaBaseVRegs;
  GrowableList* ssaSubscripts;
  GrowableList* ssaStrings;

  /* The following are new data structures to support SSA representations */
  /* Map original Dalvik virtual reg i to the current SSA name */
  int* vRegToSSAMap;                  // length == method->registersSize
  int* SSALastDefs;                   // length == method->registersSize
  ArenaBitVector* isConstantV;        // length == numSSAReg
  int* constantValues;                // length == numSSAReg
  int* phiAliasMap;                   // length == numSSAReg
  MIR* phiList;

  /* Use counts of ssa names */
  GrowableList useCounts;             // Weighted by nesting depth
  GrowableList rawUseCounts;          // Not weighted

  /* Optimization support */
  GrowableList loopHeaders;

  /* Map SSA names to location */
  RegLocation* regLocation;

  /* Keep track of Dalvik vReg to physical register mappings */
  PromotionMap* promotionMap;

  /* SSA name for Method* */
  int methodSReg;
  RegLocation methodLoc;            // Describes location of method*

  int numReachableBlocks;
  int numDalvikRegisters;             // method->registersSize
  BasicBlock* entryBlock;
  BasicBlock* exitBlock;
  BasicBlock* curBlock;
  GrowableList dfsOrder;
  GrowableList dfsPostOrder;
  GrowableList domPostOrderTraversal;
  GrowableList throwLaunchpads;
  GrowableList suspendLaunchpads;
  GrowableList intrinsicLaunchpads;
  GrowableList compilerTemps;
  int* iDomList;
  ArenaBitVector* tryBlockAddr;
  ArenaBitVector** defBlockMatrix;    // numDalvikRegister x numBlocks
  ArenaBitVector* tempBlockV;
  ArenaBitVector* tempDalvikRegisterV;
  ArenaBitVector* tempSSARegisterV;   // numSSARegs
  int* tempSSABlockIdV;               // working storage for Phi labels
  LIR* blockLabelList;
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
  int numCompilerTemps;
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
  SafeMap<unsigned int, BasicBlock*> blockMap; // findBlock lookup cache
  SafeMap<unsigned int, unsigned int> blockIdMap; // Block collapse lookup cache
  SafeMap<unsigned int, LIR*> boundaryMap; // boundary lookup cache
  int defCount;         // Used to estimate number of SSA names

  // If non-empty, apply optimizer/debug flags only to matching methods.
  std::string compilerMethodMatch;
  // Flips sense of compilerMethodMatch - apply flags if doesn't match.
  bool compilerFlipMatch;
  ArenaMemBlock* arenaHead;
  ArenaMemBlock* currentArena;
  int numArenaBlocks;
  Memstats* mstats;
  Checkstats* checkstats;
#if defined(ART_USE_QUICK_COMPILER)
  bool genBitcode;
  LLVMInfo* llvm_info;
  llvm::LLVMContext* context;
  llvm::Module* module;
  llvm::Function* func;
  greenland::IntrinsicHelper* intrinsic_helper;
  greenland::IRBuilder* irb;
  llvm::BasicBlock* placeholderBB;
  llvm::BasicBlock* entryBB;
  llvm::BasicBlock* entryTargetBB;
  std::string bitcode_filename;
  GrowableList llvmValues;
  int32_t tempName;
  SafeMap<llvm::BasicBlock*, LIR*> blockToLabelMap; // llvm bb -> LIR label
  SafeMap<int32_t, llvm::BasicBlock*> idToBlockMap; // block id -> llvm bb
  SafeMap<llvm::Value*, RegLocation> locMap; // llvm Value to loc rec
  int numShadowFrameEntries;
  int* shadowMap;
  std::set<llvm::BasicBlock*> llvmBlocks;
#endif
#ifndef NDEBUG
  /*
   * Sanity checking for the register temp tracking.  The same ssa
   * name should never be associated with one temp register per
   * instruction compilation.
   */
  int liveSReg;
#endif
  std::set<uint32_t> catches;
  int* opcodeCount;    // Count Dalvik opcodes for tuning
};

enum OpSize {
  kWord,
  kLong,
  kSingle,
  kDouble,
  kUnsignedHalf,
  kSignedHalf,
  kUnsignedByte,
  kSignedByte,
};

enum OpKind {
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
};

std::ostream& operator<<(std::ostream& os, const OpKind& kind);

enum ConditionCode {
  kCondEq,  // equal
  kCondNe,  // not equal
  kCondCs,  // carry set (unsigned less than)
  kCondUlt = kCondCs,
  kCondCc,  // carry clear (unsigned greater than or same)
  kCondUge = kCondCc,
  kCondMi,  // minus
  kCondPl,  // plus, positive or zero
  kCondVs,  // overflow
  kCondVc,  // no overflow
  kCondHi,  // unsigned greater than
  kCondLs,  // unsigned lower or same
  kCondGe,  // signed greater than or equal
  kCondLt,  // signed less than
  kCondGt,  // signed greater than
  kCondLe,  // signed less than or equal
  kCondAl,  // always
  kCondNv,  // never
};

enum ThrowKind {
  kThrowNullPointer,
  kThrowDivZero,
  kThrowArrayBounds,
  kThrowNoSuchMethod,
  kThrowStackOverflow,
};

struct SwitchTable {
  int offset;
  const u2* table;            // Original dex table
  int vaddr;                  // Dalvik offset of switch opcode
  LIR* anchor;                // Reference instruction for relative offsets
  LIR** targets;              // Array of case targets
};

struct FillArrayData {
  int offset;
  const u2* table;           // Original dex table
  int size;
  int vaddr;                 // Dalvik offset of FILL_ARRAY_DATA opcode
};

#define MAX_PATTERN_LEN 5

enum SpecialCaseHandler {
  kNoHandler,
  kNullMethod,
  kConstFunction,
  kIGet,
  kIGetBoolean,
  kIGetObject,
  kIGetByte,
  kIGetChar,
  kIGetShort,
  kIGetWide,
  kIPut,
  kIPutBoolean,
  kIPutObject,
  kIPutByte,
  kIPutChar,
  kIPutShort,
  kIPutWide,
  kIdentity,
};

struct CodePattern {
  const Instruction::Code opcodes[MAX_PATTERN_LEN];
  const SpecialCaseHandler handlerCode;
};

static const CodePattern specialPatterns[] = {
  {{Instruction::RETURN_VOID}, kNullMethod},
  {{Instruction::CONST, Instruction::RETURN}, kConstFunction},
  {{Instruction::CONST_4, Instruction::RETURN}, kConstFunction},
  {{Instruction::CONST_4, Instruction::RETURN_OBJECT}, kConstFunction},
  {{Instruction::CONST_16, Instruction::RETURN}, kConstFunction},
  {{Instruction::IGET, Instruction:: RETURN}, kIGet},
  {{Instruction::IGET_BOOLEAN, Instruction::RETURN}, kIGetBoolean},
  {{Instruction::IGET_OBJECT, Instruction::RETURN_OBJECT}, kIGetObject},
  {{Instruction::IGET_BYTE, Instruction::RETURN}, kIGetByte},
  {{Instruction::IGET_CHAR, Instruction::RETURN}, kIGetChar},
  {{Instruction::IGET_SHORT, Instruction::RETURN}, kIGetShort},
  {{Instruction::IGET_WIDE, Instruction::RETURN_WIDE}, kIGetWide},
  {{Instruction::IPUT, Instruction::RETURN_VOID}, kIPut},
  {{Instruction::IPUT_BOOLEAN, Instruction::RETURN_VOID}, kIPutBoolean},
  {{Instruction::IPUT_OBJECT, Instruction::RETURN_VOID}, kIPutObject},
  {{Instruction::IPUT_BYTE, Instruction::RETURN_VOID}, kIPutByte},
  {{Instruction::IPUT_CHAR, Instruction::RETURN_VOID}, kIPutChar},
  {{Instruction::IPUT_SHORT, Instruction::RETURN_VOID}, kIPutShort},
  {{Instruction::IPUT_WIDE, Instruction::RETURN_VOID}, kIPutWide},
  {{Instruction::RETURN}, kIdentity},
  {{Instruction::RETURN_OBJECT}, kIdentity},
  {{Instruction::RETURN_WIDE}, kIdentity},
};

BasicBlock* oatNewBB(CompilationUnit* cUnit, BBType blockType, int blockId);

void oatAppendMIR(BasicBlock* bb, MIR* mir);

void oatPrependMIR(BasicBlock* bb, MIR* mir);

void oatInsertMIRAfter(BasicBlock* bb, MIR* currentMIR, MIR* newMIR);

void oatAppendLIR(CompilationUnit* cUnit, LIR* lir);

void oatInsertLIRBefore(LIR* currentLIR, LIR* newLIR);

void oatInsertLIRAfter(LIR* currentLIR, LIR* newLIR);

MIR* oatFindMoveResult(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir);
/* Debug Utilities */
void oatDumpCompilationUnit(CompilationUnit* cUnit);

}  // namespace art

#endif // ART_SRC_COMPILER_COMPILER_IR_H_
