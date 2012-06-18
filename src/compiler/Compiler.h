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
#include "dex_instruction.h"

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

/*
 * Assembly is an iterative process, and usually terminates within
 * two or three passes.  This should be high enough to handle bizarre
 * cases, but detect an infinite loop bug.
 */
#define MAX_ASSEMBLER_RETRIES 50

/* Suppress optimization if corresponding bit set */
enum optControlVector {
  kLoadStoreElimination = 0,
  kLoadHoisting,
  kSuppressLoads,
  kNullCheckElimination,
  kPromoteRegs,
  kTrackLiveTemps,
  kSkipLargeMethodOptimization,
  kSafeOptimizations,
  kBBOpt,
  kMatch,
  kPromoteCompilerTemps,
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
  kDebugShowNops,
  kDebugCountOpcodes,
#if defined(ART_USE_QUICK_COMPILER)
  kDebugDumpBitcodeFile,
#endif
};

enum OatMethodAttributes {
  kIsCallee = 0,      /* Code is part of a callee (invoked by a hot trace) */
  kIsHot,             /* Code is part of a hot trace */
  kIsLeaf,            /* Method is leaf */
  kIsEmpty,           /* Method is empty */
  kIsThrowFree,       /* Method doesn't throw */
  kIsGetter,          /* Method fits the getter pattern */
  kIsSetter,          /* Method fits the setter pattern */
  kCannotCompile,     /* Method cannot be compiled */
};

#define METHOD_IS_CALLEE        (1 << kIsCallee)
#define METHOD_IS_HOT           (1 << kIsHot)
#define METHOD_IS_LEAF          (1 << kIsLeaf)
#define METHOD_IS_EMPTY         (1 << kIsEmpty)
#define METHOD_IS_THROW_FREE    (1 << kIsThrowFree)
#define METHOD_IS_GETTER        (1 << kIsGetter)
#define METHOD_IS_SETTER        (1 << kIsSetter)
#define METHOD_CANNOT_COMPILE   (1 << kCannotCompile)

/* Customized node traversal orders for different needs */
enum DataFlowAnalysisMode {
  kAllNodes = 0,              // All nodes
  kReachableNodes,            // All reachable nodes
  kPreOrderDFSTraversal,      // Depth-First-Search / Pre-Order
  kPostOrderDFSTraversal,     // Depth-First-Search / Post-Order
  kPostOrderDOMTraversal,     // Dominator tree / Post-Order
  kReversePostOrderTraversal, // Depth-First-Search / reverse Post-Order
};

struct CompilationUnit;
struct BasicBlock;
struct SSARepresentation;
struct GrowableList;
struct MIR;

void oatInit(CompilationUnit* cUnit, const Compiler& compiler);
bool oatArchInit(void);
bool oatStartup(void);
void oatShutdown(void);
void oatScanAllClassPointers(void (*callback)(void* ptr));
void oatInitializeSSAConversion(CompilationUnit* cUnit);
int SRegToVReg(const CompilationUnit* cUnit, int ssaReg);
int SRegToSubscript(const CompilationUnit* cUnit, int ssaReg);
bool oatFindLocalLiveIn(CompilationUnit* cUnit, BasicBlock* bb);
bool oatDoSSAConversion(CompilationUnit* cUnit, BasicBlock* bb);
bool oatDoConstantPropagation(CompilationUnit* cUnit, BasicBlock* bb);
bool oatFindInductionVariables(CompilationUnit* cUnit, BasicBlock* bb);
/* Clear the visited flag for each BB */
bool oatClearVisitedFlag(CompilationUnit* cUnit, BasicBlock* bb);
char* oatGetDalvikDisassembly(CompilationUnit* cUnit, const DecodedInstruction& insn,
                              const char* note);
char* oatFullDisassembler(CompilationUnit* cUnit, const MIR* mir);
char* oatGetSSAString(CompilationUnit* cUnit, SSARepresentation* ssaRep);
void oatDataFlowAnalysisDispatcher(CompilationUnit* cUnit,
                                   bool (*func)(CompilationUnit* , BasicBlock*),
                                   DataFlowAnalysisMode dfaMode,
                                   bool isIterative);
void oatMethodSSATransformation(CompilationUnit* cUnit);
u8 oatGetRegResourceMask(int reg);
void oatDumpCFG(CompilationUnit* cUnit, const char* dirPrefix);
void oatProcessSwitchTables(CompilationUnit* cUnit);
bool oatIsFpReg(int reg);
uint32_t oatFpRegMask(void);
void oatReplaceSpecialChars(std::string& str);

}  // namespace art

extern "C" art::CompiledMethod* ArtCompileMethod(art::Compiler& compiler,
                                                 const art::DexFile::CodeItem* code_item,
                                                 uint32_t access_flags, uint32_t method_idx,
                                                 const art::ClassLoader* class_loader,
                                                 const art::DexFile& dex_file);

#endif // ART_SRC_COMPILER_COMPILER_H_
