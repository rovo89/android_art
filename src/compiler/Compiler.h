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

#ifndef ART_SRC_COMPILER_COMPILER_H_
#define ART_SRC_COMPILER_COMPILER_H_

#include "dex_file.h"

namespace art {

#define COMPILER_TRACED(X)
#define COMPILER_TRACEE(X)

/*
 * Special offsets to denote method entry/exit for debugger update.
 * NOTE: bit pattern must be loadable using 1 instruction and must
 * not be a valid Dalvik offset.
 */
#define DEBUGGER_METHOD_ENTRY -1
#define DEBUGGER_METHOD_EXIT -2

typedef enum OatInstructionSetType {
    DALVIK_OAT_NONE = 0,
    DALVIK_OAT_ARM,
    DALVIK_OAT_THUMB2,
} OatInstructionSetType;

/* Supress optimization if corresponding bit set */
enum optControlVector {
    kLoadStoreElimination = 0,
    kLoadHoisting,
    kSuppressLoads,
    kNullCheckElimination,
    kPromoteRegs,
    kTrackLiveTemps,
    kSkipLargeMethodOptimization,
    kGenCodeForDebugger,
};

/* Type of allocation for memory tuning */
enum oatAllocKind {
    kAllocMisc,
    kAllocBB,
    kAllocLIR,
    kAllocMIR,
    kAllocDFInfo,
    kAllocGrowableList,
    kAllocGrowableBitMap,
    kAllocDalvikToSSAMap,
    kAllocDebugInfo,
    kAllocSuccessor,
    kAllocRegAlloc,
    kAllocData,
    kAllocPredecessors,
    kNumAllocKinds
};

/* Type of growable list for memory tuning */
enum oatListKind {
    kListMisc = 0,
    kListBlockList,
    kListSSAtoDalvikMap,
    kListDfsOrder,
    kListDfsPostOrder,
    kListDomPostOrderTraversal,
    kListThrowLaunchPads,
    kListSuspendLaunchPads,
    kListSwitchTables,
    kListFillArrayData,
    kListSuccessorBlocks,
    kListPredecessors,
    kNumListKinds
};

/* Type of growable bitmap for memory tuning */
enum oatBitMapKind {
    kBitMapMisc = 0,
    kBitMapUse,
    kBitMapDef,
    kBitMapLiveIn,
    kBitMapBMatrix,
    kBitMapDominators,
    kBitMapIDominated,
    kBitMapDomFrontier,
    kBitMapPhi,
    kBitMapTmpBlocks,
    kBitMapInputBlocks,
    kBitMapRegisterV,
    kBitMapTempSSARegisterV,
    kBitMapNullCheck,
    kBitMapTmpBlockV,
    kBitMapPredecessors,
    kNumBitMapKinds
};

extern uint32_t compilerOptimizerDisableFlags;

/* Force code generation paths for testing */
enum debugControlVector {
    kDebugDisplayMissingTargets,
    kDebugVerbose,
    kDebugDumpCFG,
    kDebugSlowFieldPath,
    kDebugSlowInvokePath,
    kDebugSlowStringPath,
    kDebugSlowTypePath,
    kDebugSlowestFieldPath,
    kDebugSlowestStringPath,
    kDebugExerciseResolveMethod,
    kDebugVerifyDataflow,
    kDebugShowMemoryUsage,
};

extern uint32_t compilerDebugFlags;

/* If non-empty, apply optimizer/debug flags only to matching methods */
extern std::string compilerMethodMatch;

/* Flips sense of compilerMethodMatch - apply flags if doesn't match */
extern bool compilerFlipMatch;

typedef enum OatMethodAttributes {
    kIsCallee = 0,      /* Code is part of a callee (invoked by a hot trace) */
    kIsHot,             /* Code is part of a hot trace */
    kIsLeaf,            /* Method is leaf */
    kIsEmpty,           /* Method is empty */
    kIsThrowFree,       /* Method doesn't throw */
    kIsGetter,          /* Method fits the getter pattern */
    kIsSetter,          /* Method fits the setter pattern */
    kCannotCompile,     /* Method cannot be compiled */
} OatMethodAttributes;

#define METHOD_IS_CALLEE        (1 << kIsCallee)
#define METHOD_IS_HOT           (1 << kIsHot)
#define METHOD_IS_LEAF          (1 << kIsLeaf)
#define METHOD_IS_EMPTY         (1 << kIsEmpty)
#define METHOD_IS_THROW_FREE    (1 << kIsThrowFree)
#define METHOD_IS_GETTER        (1 << kIsGetter)
#define METHOD_IS_SETTER        (1 << kIsSetter)
#define METHOD_CANNOT_COMPILE   (1 << kCannotCompile)

/* Customized node traversal orders for different needs */
typedef enum DataFlowAnalysisMode {
    kAllNodes = 0,              // All nodes
    kReachableNodes,            // All reachable nodes
    kPreOrderDFSTraversal,      // Depth-First-Search / Pre-Order
    kPostOrderDFSTraversal,     // Depth-First-Search / Post-Order
    kPostOrderDOMTraversal,     // Dominator tree / Post-Order
    kReversePostOrderTraversal, // Depth-First-Search / reverse Post-Order
} DataFlowAnalysisMode;

struct CompilationUnit;
struct BasicBlock;
struct SSARepresentation;
struct GrowableList;
struct MIR;

void oatInit(CompilationUnit* cUnit, const Compiler& compiler);
bool oatArchInit(void);
void oatArchDump(void);
bool oatStartup(void);
void oatShutdown(void);
CompiledMethod* oatCompileMethod(Compiler& compiler, bool is_direct,
                                 uint32_t method_idx, const ClassLoader* class_loader,
                                 const DexFile& dex_file, OatInstructionSetType);
void oatDumpStats(void);
void oatScanAllClassPointers(void (*callback)(void* ptr));
void oatInitializeSSAConversion(struct CompilationUnit* cUnit);
int oatConvertSSARegToDalvik(const struct CompilationUnit* cUnit, int ssaReg);
bool oatFindLocalLiveIn(struct CompilationUnit* cUnit,
                                struct BasicBlock* bb);
bool oatDoSSAConversion(struct CompilationUnit* cUnit,
                                struct BasicBlock* bb);
bool oatDoConstantPropagation(struct CompilationUnit* cUnit,
                                      struct BasicBlock* bb);
bool oatFindInductionVariables(struct CompilationUnit* cUnit,
                                       struct BasicBlock* bb);
/* Clear the visited flag for each BB */
bool oatClearVisitedFlag(struct CompilationUnit* cUnit,
                                 struct BasicBlock* bb);
char* oatGetDalvikDisassembly(CompilationUnit* cUnit,
                              const DecodedInstruction* insn,
                              const char* note);
char* oatFullDisassembler(struct CompilationUnit* cUnit,
                          const struct MIR* mir);
char* oatGetSSAString(struct CompilationUnit* cUnit,
                              struct SSARepresentation* ssaRep);
void oatDataFlowAnalysisDispatcher(struct CompilationUnit* cUnit,
                bool (*func)(struct CompilationUnit* , struct BasicBlock*),
                DataFlowAnalysisMode dfaMode,
                bool isIterative);
void oatMethodSSATransformation(struct CompilationUnit* cUnit);
u8 oatGetRegResourceMask(int reg);
void oatDumpCFG(struct CompilationUnit* cUnit, const char* dirPrefix);
void oatProcessSwitchTables(CompilationUnit* cUnit);

}  // namespace art

#endif // ART_SRC_COMPILER_COMPILER_H_
