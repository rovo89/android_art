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

#include "dalvik.h"
#include "compiler_internals.h"
#include "dataflow.h"
#include "leb128.h"
#include "object.h"
#include "runtime.h"

#include <llvm/Support/Threading.h>

namespace {
#if !defined(ART_USE_LLVM_COMPILER)
  pthread_once_t llvm_multi_init = PTHREAD_ONCE_INIT;
#endif
  void InitializeLLVMForQuick() {
    llvm::llvm_start_multithreaded();
  }
}

namespace art {

LLVMInfo::LLVMInfo() {
#if !defined(ART_USE_LLVM_COMPILER)
  pthread_once(&llvm_multi_init, InitializeLLVMForQuick);
#endif
  // Create context, module, intrinsic helper & ir builder
  llvm_context_.reset(new llvm::LLVMContext());
  llvm_module_ = new llvm::Module("art", *llvm_context_);
  llvm::StructType::create(*llvm_context_, "JavaObject");
  intrinsic_helper_.reset( new greenland::IntrinsicHelper(*llvm_context_, *llvm_module_));
  ir_builder_.reset(new greenland::IRBuilder(*llvm_context_, *llvm_module_, *intrinsic_helper_));
}

LLVMInfo::~LLVMInfo() {
}

extern "C" void ArtInitQuickCompilerContext(art::Compiler& compiler) {
  CHECK(compiler.GetCompilerContext() == NULL);
  LLVMInfo* llvmInfo = new LLVMInfo();
  compiler.SetCompilerContext(llvmInfo);
}

extern "C" void ArtUnInitQuickCompilerContext(art::Compiler& compiler) {
  delete reinterpret_cast<LLVMInfo*>(compiler.GetCompilerContext());
  compiler.SetCompilerContext(NULL);
}

/* Default optimizer/debug setting for the compiler. */
static uint32_t kCompilerOptimizerDisableFlags = 0 | // Disable specific optimizations
  //(1 << kLoadStoreElimination) |
  //(1 << kLoadHoisting) |
  //(1 << kSuppressLoads) |
  //(1 << kNullCheckElimination) |
  //(1 << kPromoteRegs) |
  //(1 << kTrackLiveTemps) |
  //(1 << kSkipLargeMethodOptimization) |
  //(1 << kSafeOptimizations) |
  //(1 << kBBOpt) |
  //(1 << kMatch) |
  //(1 << kPromoteCompilerTemps) |
  0;

static uint32_t kCompilerDebugFlags = 0 |     // Enable debug/testing modes
  //(1 << kDebugDisplayMissingTargets) |
  //(1 << kDebugVerbose) |
  //(1 << kDebugDumpCFG) |
  //(1 << kDebugSlowFieldPath) |
  //(1 << kDebugSlowInvokePath) |
  //(1 << kDebugSlowStringPath) |
  //(1 << kDebugSlowestFieldPath) |
  //(1 << kDebugSlowestStringPath) |
  //(1 << kDebugExerciseResolveMethod) |
  //(1 << kDebugVerifyDataflow) |
  //(1 << kDebugShowMemoryUsage) |
  //(1 << kDebugShowNops) |
  //(1 << kDebugCountOpcodes) |
  //(1 << kDebugDumpCheckStats) |
  //(1 << kDebugDumpBitcodeFile) |
  //(1 << kDebugVerifyBitcode) |
  0;

inline bool contentIsInsn(const u2* codePtr) {
  u2 instr = *codePtr;
  Instruction::Code opcode = (Instruction::Code)(instr & 0xff);

  /*
   * Since the low 8-bit in metadata may look like NOP, we need to check
   * both the low and whole sub-word to determine whether it is code or data.
   */
  return (opcode != Instruction::NOP || instr == 0);
}

/*
 * Parse an instruction, return the length of the instruction
 */
inline int parseInsn(CompilationUnit* cUnit, const u2* codePtr,
                   DecodedInstruction* decoded_instruction, bool printMe)
{
  // Don't parse instruction data
  if (!contentIsInsn(codePtr)) {
    return 0;
  }

  const Instruction* instruction = Instruction::At(codePtr);
  *decoded_instruction = DecodedInstruction(instruction);

  if (printMe) {
    char* decodedString = oatGetDalvikDisassembly(cUnit, *decoded_instruction,
                                                  NULL);
    LOG(INFO) << codePtr << ": 0x"
              << std::hex << static_cast<int>(decoded_instruction->opcode)
              << " " << decodedString;
  }
  return instruction->SizeInCodeUnits();
}

#define UNKNOWN_TARGET 0xffffffff

inline bool isGoto(MIR* insn) {
  switch (insn->dalvikInsn.opcode) {
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      return true;
    default:
      return false;
  }
}

/*
 * Identify unconditional branch instructions
 */
inline bool isUnconditionalBranch(MIR* insn) {
  switch (insn->dalvikInsn.opcode) {
    case Instruction::RETURN_VOID:
    case Instruction::RETURN:
    case Instruction::RETURN_WIDE:
    case Instruction::RETURN_OBJECT:
      return true;
  default:
    return isGoto(insn);
  }
}

/* Split an existing block from the specified code offset into two */
BasicBlock *splitBlock(CompilationUnit* cUnit, unsigned int codeOffset,
                     BasicBlock* origBlock, BasicBlock** immedPredBlockP)
{
  MIR* insn = origBlock->firstMIRInsn;
  while (insn) {
    if (insn->offset == codeOffset) break;
    insn = insn->next;
  }
  if (insn == NULL) {
    LOG(FATAL) << "Break split failed";
  }
  BasicBlock *bottomBlock = oatNewBB(cUnit, kDalvikByteCode,
                                     cUnit->numBlocks++);
  oatInsertGrowableList(cUnit, &cUnit->blockList, (intptr_t) bottomBlock);

  bottomBlock->startOffset = codeOffset;
  bottomBlock->firstMIRInsn = insn;
  bottomBlock->lastMIRInsn = origBlock->lastMIRInsn;

  /* Add it to the quick lookup cache */
  cUnit->blockMap.Put(bottomBlock->startOffset, bottomBlock);

  /* Handle the taken path */
  bottomBlock->taken = origBlock->taken;
  if (bottomBlock->taken) {
    origBlock->taken = NULL;
    oatDeleteGrowableList(bottomBlock->taken->predecessors,
                          (intptr_t)origBlock);
    oatInsertGrowableList(cUnit, bottomBlock->taken->predecessors,
                          (intptr_t)bottomBlock);
  }

  /* Handle the fallthrough path */
  bottomBlock->fallThrough = origBlock->fallThrough;
  origBlock->fallThrough = bottomBlock;
  oatInsertGrowableList(cUnit, bottomBlock->predecessors,
                        (intptr_t)origBlock);
  if (bottomBlock->fallThrough) {
    oatDeleteGrowableList(bottomBlock->fallThrough->predecessors,
                          (intptr_t)origBlock);
    oatInsertGrowableList(cUnit, bottomBlock->fallThrough->predecessors,
                          (intptr_t)bottomBlock);
  }

  /* Handle the successor list */
  if (origBlock->successorBlockList.blockListType != kNotUsed) {
    bottomBlock->successorBlockList = origBlock->successorBlockList;
    origBlock->successorBlockList.blockListType = kNotUsed;
    GrowableListIterator iterator;

    oatGrowableListIteratorInit(&bottomBlock->successorBlockList.blocks,
                                &iterator);
    while (true) {
      SuccessorBlockInfo *successorBlockInfo =
          (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);
      if (successorBlockInfo == NULL) break;
      BasicBlock *bb = successorBlockInfo->block;
      oatDeleteGrowableList(bb->predecessors, (intptr_t)origBlock);
      oatInsertGrowableList(cUnit, bb->predecessors, (intptr_t)bottomBlock);
    }
  }

  origBlock->lastMIRInsn = insn->prev;

  insn->prev->next = NULL;
  insn->prev = NULL;
  /*
   * Update the immediate predecessor block pointer so that outgoing edges
   * can be applied to the proper block.
   */
  if (immedPredBlockP) {
    DCHECK_EQ(*immedPredBlockP, origBlock);
    *immedPredBlockP = bottomBlock;
  }
  return bottomBlock;
}

/*
 * Given a code offset, find out the block that starts with it. If the offset
 * is in the middle of an existing block, split it into two.  If immedPredBlockP
 * is not non-null and is the block being split, update *immedPredBlockP to
 * point to the bottom block so that outgoing edges can be set up properly
 * (by the caller)
 * Utilizes a map for fast lookup of the typical cases.
 */
BasicBlock *findBlock(CompilationUnit* cUnit, unsigned int codeOffset,
                      bool split, bool create, BasicBlock** immedPredBlockP)
{
  GrowableList* blockList = &cUnit->blockList;
  BasicBlock* bb;
  unsigned int i;
  SafeMap<unsigned int, BasicBlock*>::iterator it;

  it = cUnit->blockMap.find(codeOffset);
  if (it != cUnit->blockMap.end()) {
    return it->second;
  } else if (!create) {
    return NULL;
  }

  if (split) {
    for (i = 0; i < blockList->numUsed; i++) {
      bb = (BasicBlock *) blockList->elemList[i];
      if (bb->blockType != kDalvikByteCode) continue;
      /* Check if a branch jumps into the middle of an existing block */
      if ((codeOffset > bb->startOffset) && (bb->lastMIRInsn != NULL) &&
          (codeOffset <= bb->lastMIRInsn->offset)) {
        BasicBlock *newBB = splitBlock(cUnit, codeOffset, bb,
                                       bb == *immedPredBlockP ?
                                       immedPredBlockP : NULL);
        return newBB;
      }
    }
  }

  /* Create a new one */
  bb = oatNewBB(cUnit, kDalvikByteCode, cUnit->numBlocks++);
  oatInsertGrowableList(cUnit, &cUnit->blockList, (intptr_t) bb);
  bb->startOffset = codeOffset;
  cUnit->blockMap.Put(bb->startOffset, bb);
  return bb;
}

/* Find existing block */
BasicBlock* oatFindBlock(CompilationUnit* cUnit, unsigned int codeOffset)
{
  return findBlock(cUnit, codeOffset, false, false, NULL);
}

/* Turn method name into a legal Linux file name */
void oatReplaceSpecialChars(std::string& str)
{
  static const struct { const char before; const char after; } match[] =
      {{'/','-'}, {';','#'}, {' ','#'}, {'$','+'},
       {'(','@'}, {')','@'}, {'<','='}, {'>','='}};
  for (unsigned int i = 0; i < sizeof(match)/sizeof(match[0]); i++) {
    std::replace(str.begin(), str.end(), match[i].before, match[i].after);
  }
}

/* Dump the CFG into a DOT graph */
void oatDumpCFG(CompilationUnit* cUnit, const char* dirPrefix)
{
  FILE* file;
  std::string fname(PrettyMethod(cUnit->method_idx, *cUnit->dex_file));
  oatReplaceSpecialChars(fname);
  fname = StringPrintf("%s%s%x.dot", dirPrefix, fname.c_str(),
                      cUnit->entryBlock->fallThrough->startOffset);
  file = fopen(fname.c_str(), "w");
  if (file == NULL) {
    return;
  }
  fprintf(file, "digraph G {\n");

  fprintf(file, "  rankdir=TB\n");

  int numReachableBlocks = cUnit->numReachableBlocks;
  int idx;
  const GrowableList *blockList = &cUnit->blockList;

  for (idx = 0; idx < numReachableBlocks; idx++) {
    int blockIdx = cUnit->dfsOrder.elemList[idx];
    BasicBlock *bb = (BasicBlock *) oatGrowableListGetElement(blockList,
                                                              blockIdx);
    if (bb == NULL) break;
    if (bb->blockType == kDead) continue;
    if (bb->blockType == kEntryBlock) {
      fprintf(file, "  entry_%d [shape=Mdiamond];\n", bb->id);
    } else if (bb->blockType == kExitBlock) {
      fprintf(file, "  exit_%d [shape=Mdiamond];\n", bb->id);
    } else if (bb->blockType == kDalvikByteCode) {
      fprintf(file, "  block%04x_%d [shape=record,label = \"{ \\\n",
              bb->startOffset, bb->id);
      const MIR *mir;
        fprintf(file, "    {block id %d\\l}%s\\\n", bb->id,
                bb->firstMIRInsn ? " | " : " ");
        for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
            fprintf(file, "    {%04x %s\\l}%s\\\n", mir->offset,
                    mir->ssaRep ? oatFullDisassembler(cUnit, mir) :
                    Instruction::Name(mir->dalvikInsn.opcode),
                    mir->next ? " | " : " ");
        }
        fprintf(file, "  }\"];\n\n");
    } else if (bb->blockType == kExceptionHandling) {
      char blockName[BLOCK_NAME_LEN];

      oatGetBlockName(bb, blockName);
      fprintf(file, "  %s [shape=invhouse];\n", blockName);
    }

    char blockName1[BLOCK_NAME_LEN], blockName2[BLOCK_NAME_LEN];

    if (bb->taken) {
      oatGetBlockName(bb, blockName1);
      oatGetBlockName(bb->taken, blockName2);
      fprintf(file, "  %s:s -> %s:n [style=dotted]\n",
              blockName1, blockName2);
    }
    if (bb->fallThrough) {
      oatGetBlockName(bb, blockName1);
      oatGetBlockName(bb->fallThrough, blockName2);
      fprintf(file, "  %s:s -> %s:n\n", blockName1, blockName2);
    }

    if (bb->successorBlockList.blockListType != kNotUsed) {
      fprintf(file, "  succ%04x_%d [shape=%s,label = \"{ \\\n",
              bb->startOffset, bb->id,
              (bb->successorBlockList.blockListType == kCatch) ?
               "Mrecord" : "record");
      GrowableListIterator iterator;
      oatGrowableListIteratorInit(&bb->successorBlockList.blocks,
                                  &iterator);
      SuccessorBlockInfo *successorBlockInfo =
          (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);

      int succId = 0;
      while (true) {
        if (successorBlockInfo == NULL) break;

        BasicBlock *destBlock = successorBlockInfo->block;
        SuccessorBlockInfo *nextSuccessorBlockInfo =
            (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);

        fprintf(file, "    {<f%d> %04x: %04x\\l}%s\\\n",
                succId++,
                successorBlockInfo->key,
                destBlock->startOffset,
                (nextSuccessorBlockInfo != NULL) ? " | " : " ");

        successorBlockInfo = nextSuccessorBlockInfo;
      }
      fprintf(file, "  }\"];\n\n");

      oatGetBlockName(bb, blockName1);
      fprintf(file, "  %s:s -> succ%04x_%d:n [style=dashed]\n",
              blockName1, bb->startOffset, bb->id);

      if (bb->successorBlockList.blockListType == kPackedSwitch ||
          bb->successorBlockList.blockListType == kSparseSwitch) {

        oatGrowableListIteratorInit(&bb->successorBlockList.blocks,
                                    &iterator);

        succId = 0;
        while (true) {
          SuccessorBlockInfo *successorBlockInfo = (SuccessorBlockInfo *)
              oatGrowableListIteratorNext(&iterator);
          if (successorBlockInfo == NULL) break;

          BasicBlock *destBlock = successorBlockInfo->block;

          oatGetBlockName(destBlock, blockName2);
          fprintf(file, "  succ%04x_%d:f%d:e -> %s:n\n", bb->startOffset,
                  bb->id, succId++, blockName2);
        }
      }
    }
    fprintf(file, "\n");

    /* Display the dominator tree */
    oatGetBlockName(bb, blockName1);
    fprintf(file, "  cfg%s [label=\"%s\", shape=none];\n",
            blockName1, blockName1);
    if (bb->iDom) {
      oatGetBlockName(bb->iDom, blockName2);
      fprintf(file, "  cfg%s:s -> cfg%s:n\n\n", blockName2, blockName1);
    }
  }
  fprintf(file, "}\n");
  fclose(file);
}

/* Verify if all the successor is connected with all the claimed predecessors */
bool verifyPredInfo(CompilationUnit* cUnit, BasicBlock* bb)
{
  GrowableListIterator iter;

  oatGrowableListIteratorInit(bb->predecessors, &iter);
  while (true) {
    BasicBlock *predBB = (BasicBlock*)oatGrowableListIteratorNext(&iter);
    if (!predBB) break;
    bool found = false;
    if (predBB->taken == bb) {
        found = true;
    } else if (predBB->fallThrough == bb) {
        found = true;
    } else if (predBB->successorBlockList.blockListType != kNotUsed) {
      GrowableListIterator iterator;
      oatGrowableListIteratorInit(&predBB->successorBlockList.blocks,
                                  &iterator);
      while (true) {
        SuccessorBlockInfo *successorBlockInfo = (SuccessorBlockInfo *)
            oatGrowableListIteratorNext(&iterator);
        if (successorBlockInfo == NULL) break;
        BasicBlock *succBB = successorBlockInfo->block;
        if (succBB == bb) {
            found = true;
            break;
        }
      }
    }
    if (found == false) {
      char blockName1[BLOCK_NAME_LEN], blockName2[BLOCK_NAME_LEN];
      oatGetBlockName(bb, blockName1);
      oatGetBlockName(predBB, blockName2);
      oatDumpCFG(cUnit, "/sdcard/cfg/");
      LOG(FATAL) << "Successor " << blockName1 << "not found from "
                 << blockName2;
    }
  }
  return true;
}

/* Identify code range in try blocks and set up the empty catch blocks */
void processTryCatchBlocks(CompilationUnit* cUnit)
{
  const DexFile::CodeItem* code_item = cUnit->code_item;
  int triesSize = code_item->tries_size_;
  int offset;

  if (triesSize == 0) {
    return;
  }

  ArenaBitVector* tryBlockAddr = cUnit->tryBlockAddr;

  for (int i = 0; i < triesSize; i++) {
    const DexFile::TryItem* pTry =
        DexFile::GetTryItems(*code_item, i);
    int startOffset = pTry->start_addr_;
    int endOffset = startOffset + pTry->insn_count_;
    for (offset = startOffset; offset < endOffset; offset++) {
      oatSetBit(cUnit, tryBlockAddr, offset);
    }
  }

  // Iterate over each of the handlers to enqueue the empty Catch blocks
  const byte* handlers_ptr = DexFile::GetCatchHandlerData(*code_item, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    CatchHandlerIterator iterator(handlers_ptr);
    for (; iterator.HasNext(); iterator.Next()) {
      uint32_t address = iterator.GetHandlerAddress();
      findBlock(cUnit, address, false /* split */, true /*create*/,
                /* immedPredBlockP */ NULL);
    }
    handlers_ptr = iterator.EndDataPointer();
  }
}

/* Process instructions with the kBranch flag */
BasicBlock* processCanBranch(CompilationUnit* cUnit, BasicBlock* curBlock,
                           MIR* insn, int curOffset, int width, int flags,
                           const u2* codePtr, const u2* codeEnd)
{
  int target = curOffset;
  switch (insn->dalvikInsn.opcode) {
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      target += (int) insn->dalvikInsn.vA;
      break;
    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
      curBlock->conditionalBranch = true;
      target += (int) insn->dalvikInsn.vC;
      break;
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ:
      curBlock->conditionalBranch = true;
      target += (int) insn->dalvikInsn.vB;
      break;
    default:
      LOG(FATAL) << "Unexpected opcode(" << (int)insn->dalvikInsn.opcode
                 << ") with kBranch set";
  }
  BasicBlock *takenBlock = findBlock(cUnit, target,
                                     /* split */
                                     true,
                                     /* create */
                                     true,
                                     /* immedPredBlockP */
                                     &curBlock);
  curBlock->taken = takenBlock;
  oatInsertGrowableList(cUnit, takenBlock->predecessors, (intptr_t)curBlock);

  /* Always terminate the current block for conditional branches */
  if (flags & Instruction::kContinue) {
    BasicBlock *fallthroughBlock = findBlock(cUnit,
                                             curOffset +  width,
                                             /*
                                              * If the method is processed
                                              * in sequential order from the
                                              * beginning, we don't need to
                                              * specify split for continue
                                              * blocks. However, this
                                              * routine can be called by
                                              * compileLoop, which starts
                                              * parsing the method from an
                                              * arbitrary address in the
                                              * method body.
                                              */
                                             true,
                                             /* create */
                                             true,
                                             /* immedPredBlockP */
                                             &curBlock);
    curBlock->fallThrough = fallthroughBlock;
    oatInsertGrowableList(cUnit, fallthroughBlock->predecessors,
                          (intptr_t)curBlock);
  } else if (codePtr < codeEnd) {
    /* Create a fallthrough block for real instructions (incl. NOP) */
    if (contentIsInsn(codePtr)) {
      findBlock(cUnit, curOffset + width,
                /* split */
                false,
                /* create */
                true,
                /* immedPredBlockP */
                NULL);
    }
  }
  return curBlock;
}

/* Process instructions with the kSwitch flag */
void processCanSwitch(CompilationUnit* cUnit, BasicBlock* curBlock,
                      MIR* insn, int curOffset, int width, int flags)
{
  u2* switchData= (u2 *) (cUnit->insns + curOffset + insn->dalvikInsn.vB);
  int size;
  int* keyTable;
  int* targetTable;
  int i;
  int firstKey;

  /*
   * Packed switch data format:
   *  ushort ident = 0x0100   magic value
   *  ushort size             number of entries in the table
   *  int first_key           first (and lowest) switch case value
   *  int targets[size]       branch targets, relative to switch opcode
   *
   * Total size is (4+size*2) 16-bit code units.
   */
  if (insn->dalvikInsn.opcode == Instruction::PACKED_SWITCH) {
    DCHECK_EQ(static_cast<int>(switchData[0]),
              static_cast<int>(Instruction::kPackedSwitchSignature));
    size = switchData[1];
    firstKey = switchData[2] | (switchData[3] << 16);
    targetTable = (int *) &switchData[4];
    keyTable = NULL;        // Make the compiler happy
  /*
   * Sparse switch data format:
   *  ushort ident = 0x0200   magic value
   *  ushort size             number of entries in the table; > 0
   *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
   *  int targets[size]       branch targets, relative to switch opcode
   *
   * Total size is (2+size*4) 16-bit code units.
   */
  } else {
    DCHECK_EQ(static_cast<int>(switchData[0]),
              static_cast<int>(Instruction::kSparseSwitchSignature));
    size = switchData[1];
    keyTable = (int *) &switchData[2];
    targetTable = (int *) &switchData[2 + size*2];
    firstKey = 0;   // To make the compiler happy
  }

  if (curBlock->successorBlockList.blockListType != kNotUsed) {
    LOG(FATAL) << "Successor block list already in use: "
               << (int)curBlock->successorBlockList.blockListType;
  }
  curBlock->successorBlockList.blockListType =
      (insn->dalvikInsn.opcode == Instruction::PACKED_SWITCH) ?
      kPackedSwitch : kSparseSwitch;
  oatInitGrowableList(cUnit, &curBlock->successorBlockList.blocks, size,
                      kListSuccessorBlocks);

  for (i = 0; i < size; i++) {
    BasicBlock *caseBlock = findBlock(cUnit, curOffset + targetTable[i],
                                      /* split */
                                      true,
                                      /* create */
                                      true,
                                      /* immedPredBlockP */
                                      &curBlock);
    SuccessorBlockInfo *successorBlockInfo =
        (SuccessorBlockInfo *) oatNew(cUnit, sizeof(SuccessorBlockInfo),
                                      false, kAllocSuccessor);
    successorBlockInfo->block = caseBlock;
    successorBlockInfo->key =
        (insn->dalvikInsn.opcode == Instruction::PACKED_SWITCH) ?
        firstKey + i : keyTable[i];
    oatInsertGrowableList(cUnit, &curBlock->successorBlockList.blocks,
                          (intptr_t) successorBlockInfo);
    oatInsertGrowableList(cUnit, caseBlock->predecessors,
                          (intptr_t)curBlock);
  }

  /* Fall-through case */
  BasicBlock* fallthroughBlock = findBlock(cUnit,
                                           curOffset +  width,
                                           /* split */
                                           false,
                                           /* create */
                                           true,
                                           /* immedPredBlockP */
                                           NULL);
  curBlock->fallThrough = fallthroughBlock;
  oatInsertGrowableList(cUnit, fallthroughBlock->predecessors,
                        (intptr_t)curBlock);
}

/* Process instructions with the kThrow flag */
BasicBlock* processCanThrow(CompilationUnit* cUnit, BasicBlock* curBlock,
                            MIR* insn, int curOffset, int width, int flags,
                            ArenaBitVector* tryBlockAddr, const u2* codePtr,
                            const u2* codeEnd)
{
  const DexFile::CodeItem* code_item = cUnit->code_item;
  bool inTryBlock = oatIsBitSet(tryBlockAddr, curOffset);

  /* In try block */
  if (inTryBlock) {
    CatchHandlerIterator iterator(*code_item, curOffset);

    if (curBlock->successorBlockList.blockListType != kNotUsed) {
      LOG(INFO) << PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
      LOG(FATAL) << "Successor block list already in use: "
                 << (int)curBlock->successorBlockList.blockListType;
    }

    curBlock->successorBlockList.blockListType = kCatch;
    oatInitGrowableList(cUnit, &curBlock->successorBlockList.blocks, 2,
                        kListSuccessorBlocks);

    for (;iterator.HasNext(); iterator.Next()) {
      BasicBlock *catchBlock = findBlock(cUnit, iterator.GetHandlerAddress(),
                                         false /* split*/,
                                         false /* creat */,
                                         NULL  /* immedPredBlockP */);
      catchBlock->catchEntry = true;
      cUnit->catches.insert(catchBlock->startOffset);
      SuccessorBlockInfo *successorBlockInfo = (SuccessorBlockInfo *)
          oatNew(cUnit, sizeof(SuccessorBlockInfo), false, kAllocSuccessor);
      successorBlockInfo->block = catchBlock;
      successorBlockInfo->key = iterator.GetHandlerTypeIndex();
      oatInsertGrowableList(cUnit, &curBlock->successorBlockList.blocks,
                            (intptr_t) successorBlockInfo);
      oatInsertGrowableList(cUnit, catchBlock->predecessors,
                            (intptr_t)curBlock);
    }
  } else {
    BasicBlock *ehBlock = oatNewBB(cUnit, kExceptionHandling,
                                   cUnit->numBlocks++);
    curBlock->taken = ehBlock;
    oatInsertGrowableList(cUnit, &cUnit->blockList, (intptr_t) ehBlock);
    ehBlock->startOffset = curOffset;
    oatInsertGrowableList(cUnit, ehBlock->predecessors, (intptr_t)curBlock);
  }

  if (insn->dalvikInsn.opcode == Instruction::THROW){
    curBlock->explicitThrow = true;
    if ((codePtr < codeEnd) && contentIsInsn(codePtr)) {
      // Force creation of new block following THROW via side-effect
      findBlock(cUnit, curOffset + width, /* split */ false,
                /* create */ true, /* immedPredBlockP */ NULL);
    }
    if (!inTryBlock) {
       // Don't split a THROW that can't rethrow - we're done.
      return curBlock;
    }
  }

  /*
   * Split the potentially-throwing instruction into two parts.
   * The first half will be a pseudo-op that captures the exception
   * edges and terminates the basic block.  It always falls through.
   * Then, create a new basic block that begins with the throwing instruction
   * (minus exceptions).  Note: this new basic block must NOT be entered into
   * the blockMap.  If the potentially-throwing instruction is the target of a
   * future branch, we need to find the check psuedo half.  The new
   * basic block containing the work portion of the instruction should
   * only be entered via fallthrough from the block containing the
   * pseudo exception edge MIR.  Note also that this new block is
   * not automatically terminated after the work portion, and may
   * contain following instructions.
   */
  BasicBlock *newBlock = oatNewBB(cUnit, kDalvikByteCode, cUnit->numBlocks++);
  oatInsertGrowableList(cUnit, &cUnit->blockList, (intptr_t)newBlock);
  newBlock->startOffset = insn->offset;
  curBlock->fallThrough = newBlock;
  oatInsertGrowableList(cUnit, newBlock->predecessors, (intptr_t)curBlock);
  MIR* newInsn = (MIR*)oatNew(cUnit, sizeof(MIR), true, kAllocMIR);
  *newInsn = *insn;
  insn->dalvikInsn.opcode =
      static_cast<Instruction::Code>(kMirOpCheck);
  // Associate the two halves
  insn->meta.throwInsn = newInsn;
  newInsn->meta.throwInsn = insn;
  oatAppendMIR(newBlock, newInsn);
  return newBlock;
}

void oatInit(CompilationUnit* cUnit, const Compiler& compiler) {
  if (!oatArchInit()) {
    LOG(FATAL) << "Failed to initialize oat";
  }
  if (!oatHeapInit(cUnit)) {
    LOG(FATAL) << "Failed to initialize oat heap";
  }
}

CompiledMethod* compileMethod(Compiler& compiler,
                              const CompilerBackend compilerBackend,
                              const DexFile::CodeItem* code_item,
                              uint32_t access_flags, InvokeType invoke_type,
                              uint32_t method_idx, jobject class_loader,
                              const DexFile& dex_file,
                              LLVMInfo* llvm_info
                             )
{
  VLOG(compiler) << "Compiling " << PrettyMethod(method_idx, dex_file) << "...";

  const u2* codePtr = code_item->insns_;
  const u2* codeEnd = code_item->insns_ + code_item->insns_size_in_code_units_;
  int numBlocks = 0;
  unsigned int curOffset = 0;

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  UniquePtr<CompilationUnit> cUnit(new CompilationUnit);

  oatInit(cUnit.get(), compiler);

  cUnit->compiler = &compiler;
  cUnit->class_linker = class_linker;
  cUnit->dex_file = &dex_file;
  cUnit->method_idx = method_idx;
  cUnit->code_item = code_item;
  cUnit->access_flags = access_flags;
  cUnit->invoke_type = invoke_type;
  cUnit->shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
  cUnit->instructionSet = compiler.GetInstructionSet();
  cUnit->insns = code_item->insns_;
  cUnit->insnsSize = code_item->insns_size_in_code_units_;
  cUnit->numIns = code_item->ins_size_;
  cUnit->numRegs = code_item->registers_size_ - cUnit->numIns;
  cUnit->numOuts = code_item->outs_size_;
  DCHECK((cUnit->instructionSet == kThumb2) ||
         (cUnit->instructionSet == kX86) ||
         (cUnit->instructionSet == kMips));
  if ((compilerBackend == kQuickGBC) || (compilerBackend == kPortable)) {
    cUnit->genBitcode = true;
  }
  DCHECK_NE(compilerBackend, kIceland);  // TODO: remove when Portable/Iceland merge complete
  // TODO: remove this once x86 is tested
  if (cUnit->genBitcode && (cUnit->instructionSet != kThumb2)) {
    UNIMPLEMENTED(WARNING) << "GBC generation untested for non-Thumb targets";
  }
  cUnit->llvm_info = llvm_info;
  /* Adjust this value accordingly once inlining is performed */
  cUnit->numDalvikRegisters = code_item->registers_size_;
  // TODO: set this from command line
  cUnit->compilerFlipMatch = false;
  bool useMatch = !cUnit->compilerMethodMatch.empty();
  bool match = useMatch && (cUnit->compilerFlipMatch ^
      (PrettyMethod(method_idx, dex_file).find(cUnit->compilerMethodMatch) !=
       std::string::npos));
  if (!useMatch || match) {
    cUnit->disableOpt = kCompilerOptimizerDisableFlags;
    cUnit->enableDebug = kCompilerDebugFlags;
    cUnit->printMe = VLOG_IS_ON(compiler) ||
        (cUnit->enableDebug & (1 << kDebugVerbose));
  }
#ifndef NDEBUG
  if (cUnit->genBitcode) {
    cUnit->enableDebug |= (1 << kDebugVerifyBitcode);
  }
#endif

#if 1
// *** Temporary ****
// For use in debugging issue 7250540.  Disable optimization in problem method
// to see if monkey results change.  Should be removed after monkey runs
// complete.
if (PrettyMethod(method_idx, dex_file).find("void com.android.inputmethod.keyboard.Key.<init>(android.content.res.Resources, com.android.inputmethod.keyboard.Keyboard$Params, com.android.inputmethod.keyboard.Keyboard$Builder$Row, org.xmlpull.v1.XmlPullParser)") != std::string::npos) {
    cUnit->disableOpt |= (
        (1 << kLoadStoreElimination) |
        (1 << kLoadHoisting) |
        (1 << kSuppressLoads) |
        //(1 << kNullCheckElimination) |
        //(1 << kPromoteRegs) |
        (1 << kTrackLiveTemps) |
        //(1 << kSkipLargeMethodOptimization) |
        //(1 << kSafeOptimizations) |
        (1 << kBBOpt) |
        (1 << kMatch) |
        //(1 << kPromoteCompilerTemps) |
        0);
}
#endif

  if (cUnit->instructionSet == kMips) {
    // Disable some optimizations for mips for now
    cUnit->disableOpt |= (
        (1 << kLoadStoreElimination) |
        (1 << kLoadHoisting) |
        (1 << kSuppressLoads) |
        (1 << kNullCheckElimination) |
        (1 << kPromoteRegs) |
        (1 << kTrackLiveTemps) |
        (1 << kSkipLargeMethodOptimization) |
        (1 << kSafeOptimizations) |
        (1 << kBBOpt) |
        (1 << kMatch) |
        (1 << kPromoteCompilerTemps));
  }

  /* Gathering opcode stats? */
  if (kCompilerDebugFlags & (1 << kDebugCountOpcodes)) {
    cUnit->opcodeCount = (int*)oatNew(cUnit.get(),
       kNumPackedOpcodes * sizeof(int), true, kAllocMisc);
  }

  /* Assume non-throwing leaf */
  cUnit->attrs = (METHOD_IS_LEAF | METHOD_IS_THROW_FREE);

  /* Initialize the block list, estimate size based on insnsSize */
  oatInitGrowableList(cUnit.get(), &cUnit->blockList, cUnit->insnsSize,
                      kListBlockList);

  /* Initialize the switchTables list */
  oatInitGrowableList(cUnit.get(), &cUnit->switchTables, 4,
                      kListSwitchTables);

  /* Intialize the fillArrayData list */
  oatInitGrowableList(cUnit.get(), &cUnit->fillArrayData, 4,
                      kListFillArrayData);

  /* Intialize the throwLaunchpads list, estimate size based on insnsSize */
  oatInitGrowableList(cUnit.get(), &cUnit->throwLaunchpads, cUnit->insnsSize,
                      kListThrowLaunchPads);

  /* Intialize the instrinsicLaunchpads list */
  oatInitGrowableList(cUnit.get(), &cUnit->intrinsicLaunchpads, 4,
                      kListMisc);


  /* Intialize the suspendLaunchpads list */
  oatInitGrowableList(cUnit.get(), &cUnit->suspendLaunchpads, 2048,
                      kListSuspendLaunchPads);

  /* Allocate the bit-vector to track the beginning of basic blocks */
  ArenaBitVector *tryBlockAddr = oatAllocBitVector(cUnit.get(),
                                                   cUnit->insnsSize,
                                                   true /* expandable */);
  cUnit->tryBlockAddr = tryBlockAddr;

  /* Create the default entry and exit blocks and enter them to the list */
  BasicBlock *entryBlock = oatNewBB(cUnit.get(), kEntryBlock, numBlocks++);
  BasicBlock *exitBlock = oatNewBB(cUnit.get(), kExitBlock, numBlocks++);

  cUnit->entryBlock = entryBlock;
  cUnit->exitBlock = exitBlock;

  oatInsertGrowableList(cUnit.get(), &cUnit->blockList, (intptr_t) entryBlock);
  oatInsertGrowableList(cUnit.get(), &cUnit->blockList, (intptr_t) exitBlock);

  /* Current block to record parsed instructions */
  BasicBlock *curBlock = oatNewBB(cUnit.get(), kDalvikByteCode, numBlocks++);
  curBlock->startOffset = 0;
  oatInsertGrowableList(cUnit.get(), &cUnit->blockList, (intptr_t) curBlock);
  /* Add first block to the fast lookup cache */
  cUnit->blockMap.Put(curBlock->startOffset, curBlock);
  entryBlock->fallThrough = curBlock;
  oatInsertGrowableList(cUnit.get(), curBlock->predecessors,
                        (intptr_t)entryBlock);

  /*
   * Store back the number of blocks since new blocks may be created of
   * accessing cUnit.
   */
  cUnit->numBlocks = numBlocks;

  /* Identify code range in try blocks and set up the empty catch blocks */
  processTryCatchBlocks(cUnit.get());

  /* Set up for simple method detection */
  int numPatterns = sizeof(specialPatterns)/sizeof(specialPatterns[0]);
  bool livePattern = (numPatterns > 0) && !(cUnit->disableOpt & (1 << kMatch));
  bool* deadPattern = (bool*)oatNew(cUnit.get(), sizeof(bool) * numPatterns, true,
                                     kAllocMisc);
  SpecialCaseHandler specialCase = kNoHandler;
  int patternPos = 0;

  /* Parse all instructions and put them into containing basic blocks */
  while (codePtr < codeEnd) {
    MIR *insn = (MIR *) oatNew(cUnit.get(), sizeof(MIR), true, kAllocMIR);
    insn->offset = curOffset;
    int width = parseInsn(cUnit.get(), codePtr, &insn->dalvikInsn, false);
    insn->width = width;
    Instruction::Code opcode = insn->dalvikInsn.opcode;
    if (cUnit->opcodeCount != NULL) {
      cUnit->opcodeCount[static_cast<int>(opcode)]++;
    }

    /* Terminate when the data section is seen */
    if (width == 0)
      break;

    /* Possible simple method? */
    if (livePattern) {
      livePattern = false;
      specialCase = kNoHandler;
      for (int i = 0; i < numPatterns; i++) {
        if (!deadPattern[i]) {
          if (specialPatterns[i].opcodes[patternPos] == opcode) {
            livePattern = true;
            specialCase = specialPatterns[i].handlerCode;
          } else {
             deadPattern[i] = true;
          }
        }
      }
    patternPos++;
    }

    oatAppendMIR(curBlock, insn);

    codePtr += width;
    int flags = Instruction::FlagsOf(insn->dalvikInsn.opcode);

    int dfFlags = oatDataFlowAttributes[insn->dalvikInsn.opcode];

    if (dfFlags & DF_HAS_DEFS) {
      cUnit->defCount += (dfFlags & DF_A_WIDE) ? 2 : 1;
    }

    if (flags & Instruction::kBranch) {
      curBlock = processCanBranch(cUnit.get(), curBlock, insn, curOffset,
                                  width, flags, codePtr, codeEnd);
    } else if (flags & Instruction::kReturn) {
      curBlock->fallThrough = exitBlock;
      oatInsertGrowableList(cUnit.get(), exitBlock->predecessors,
                            (intptr_t)curBlock);
      /*
       * Terminate the current block if there are instructions
       * afterwards.
       */
      if (codePtr < codeEnd) {
        /*
         * Create a fallthrough block for real instructions
         * (incl. NOP).
         */
        if (contentIsInsn(codePtr)) {
            findBlock(cUnit.get(), curOffset + width,
                      /* split */
                      false,
                      /* create */
                      true,
                      /* immedPredBlockP */
                      NULL);
        }
      }
    } else if (flags & Instruction::kThrow) {
      curBlock = processCanThrow(cUnit.get(), curBlock, insn, curOffset,
                                 width, flags, tryBlockAddr, codePtr, codeEnd);
    } else if (flags & Instruction::kSwitch) {
      processCanSwitch(cUnit.get(), curBlock, insn, curOffset, width, flags);
    }
    curOffset += width;
    BasicBlock *nextBlock = findBlock(cUnit.get(), curOffset,
                                      /* split */
                                      false,
                                      /* create */
                                      false,
                                      /* immedPredBlockP */
                                      NULL);
    if (nextBlock) {
      /*
       * The next instruction could be the target of a previously parsed
       * forward branch so a block is already created. If the current
       * instruction is not an unconditional branch, connect them through
       * the fall-through link.
       */
      DCHECK(curBlock->fallThrough == NULL ||
             curBlock->fallThrough == nextBlock ||
             curBlock->fallThrough == exitBlock);

      if ((curBlock->fallThrough == NULL) && (flags & Instruction::kContinue)) {
        curBlock->fallThrough = nextBlock;
        oatInsertGrowableList(cUnit.get(), nextBlock->predecessors,
                              (intptr_t)curBlock);
      }
      curBlock = nextBlock;
    }
  }

  if (!(cUnit->disableOpt & (1 << kSkipLargeMethodOptimization))) {
    if ((cUnit->numBlocks > MANY_BLOCKS) ||
        ((cUnit->numBlocks > MANY_BLOCKS_INITIALIZER) &&
      PrettyMethod(method_idx, dex_file, false).find("init>") !=
          std::string::npos)) {
        cUnit->qdMode = true;
    }
  }

  if (cUnit->qdMode) {
    // Bitcode generation requires full dataflow analysis
    cUnit->disableDataflow = !cUnit->genBitcode;
    // Disable optimization which require dataflow/ssa
    cUnit->disableOpt |= (1 << kBBOpt) | (1 << kPromoteRegs) | (1 << kNullCheckElimination);
    if (cUnit->printMe) {
        LOG(INFO) << "QD mode enabled: "
                  << PrettyMethod(method_idx, dex_file)
                  << " num blocks: " << cUnit->numBlocks;
    }
  }

  if (cUnit->printMe) {
    oatDumpCompilationUnit(cUnit.get());
  }

  /* Do a code layout pass */
  oatMethodCodeLayout(cUnit.get());

  if (cUnit->enableDebug & (1 << kDebugVerifyDataflow)) {
    /* Verify if all blocks are connected as claimed */
    oatDataFlowAnalysisDispatcher(cUnit.get(), verifyPredInfo, kAllNodes,
                                  false /* isIterative */);
  }

  /* Perform SSA transformation for the whole method */
  oatMethodSSATransformation(cUnit.get());

  /* Do constant propagation */
  // TODO: Probably need to make these expandable to support new ssa names
  // introducted during MIR optimization passes
  cUnit->isConstantV = oatAllocBitVector(cUnit.get(), cUnit->numSSARegs,
                                         false  /* not expandable */);
  cUnit->constantValues =
      (int*)oatNew(cUnit.get(), sizeof(int) * cUnit->numSSARegs, true,
                   kAllocDFInfo);
  oatDataFlowAnalysisDispatcher(cUnit.get(), oatDoConstantPropagation,
                                kAllNodes,
                                false /* isIterative */);

  /* Detect loops */
  oatMethodLoopDetection(cUnit.get());

  /* Count uses */
  oatMethodUseCount(cUnit.get());

  /* Perform null check elimination */
  oatMethodNullCheckElimination(cUnit.get());

  /* Combine basic blocks where possible */
  oatMethodBasicBlockCombine(cUnit.get());

  /* Do some basic block optimizations */
  oatMethodBasicBlockOptimization(cUnit.get());

  if (cUnit->enableDebug & (1 << kDebugDumpCheckStats)) {
    oatDumpCheckStats(cUnit.get());
  }

  oatInitializeRegAlloc(cUnit.get());  // Needs to happen after SSA naming

  /* Allocate Registers using simple local allocation scheme */
  oatSimpleRegAlloc(cUnit.get());

  /* Go the LLVM path? */
  if (cUnit->genBitcode) {
    // MIR->Bitcode
    oatMethodMIR2Bitcode(cUnit.get());
    if (compilerBackend == kPortable) {
      // all done
      oatArenaReset(cUnit.get());
      return NULL;
    }
    // Bitcode->LIR
    oatMethodBitcode2LIR(cUnit.get());
  } else {
    if (specialCase != kNoHandler) {
      /*
       * Custom codegen for special cases.  If for any reason the
       * special codegen doesn't succeed, cUnit->firstLIRInsn will
       * set to NULL;
       */
      oatSpecialMIR2LIR(cUnit.get(), specialCase);
    }

    /* Convert MIR to LIR, etc. */
    if (cUnit->firstLIRInsn == NULL) {
      oatMethodMIR2LIR(cUnit.get());
    }
  }

  // Debugging only
  if (cUnit->enableDebug & (1 << kDebugDumpCFG)) {
    oatDumpCFG(cUnit.get(), "/sdcard/cfg/");
  }

  /* Method is not empty */
  if (cUnit->firstLIRInsn) {

    // mark the targets of switch statement case labels
    oatProcessSwitchTables(cUnit.get());

    /* Convert LIR into machine code. */
    oatAssembleLIR(cUnit.get());

    if (cUnit->printMe) {
      oatCodegenDump(cUnit.get());
    }

    if (cUnit->opcodeCount != NULL) {
      LOG(INFO) << "Opcode Count";
      for (int i = 0; i < kNumPackedOpcodes; i++) {
        if (cUnit->opcodeCount[i] != 0) {
          LOG(INFO) << "-C- "
                    << Instruction::Name(static_cast<Instruction::Code>(i))
                    << " " << cUnit->opcodeCount[i];
        }
      }
    }
  }

  // Combine vmap tables - core regs, then fp regs - into vmapTable
  std::vector<uint16_t> vmapTable;
  // Core regs may have been inserted out of order - sort first
  std::sort(cUnit->coreVmapTable.begin(), cUnit->coreVmapTable.end());
  for (size_t i = 0 ; i < cUnit->coreVmapTable.size(); i++) {
    // Copy, stripping out the phys register sort key
    vmapTable.push_back(~(-1 << VREG_NUM_WIDTH) & cUnit->coreVmapTable[i]);
  }
  // If we have a frame, push a marker to take place of lr
  if (cUnit->frameSize > 0) {
    vmapTable.push_back(INVALID_VREG);
  } else {
    DCHECK_EQ(__builtin_popcount(cUnit->coreSpillMask), 0);
    DCHECK_EQ(__builtin_popcount(cUnit->fpSpillMask), 0);
  }
  // Combine vmap tables - core regs, then fp regs. fp regs already sorted
  for (uint32_t i = 0; i < cUnit->fpVmapTable.size(); i++) {
    vmapTable.push_back(cUnit->fpVmapTable[i]);
  }
  CompiledMethod* result =
      new CompiledMethod(cUnit->instructionSet, cUnit->codeBuffer,
                         cUnit->frameSize, cUnit->coreSpillMask, cUnit->fpSpillMask,
                         cUnit->combinedMappingTable, vmapTable, cUnit->nativeGcMap);

  VLOG(compiler) << "Compiled " << PrettyMethod(method_idx, dex_file)
     << " (" << (cUnit->codeBuffer.size() * sizeof(cUnit->codeBuffer[0]))
     << " bytes)";

#ifdef WITH_MEMSTATS
  if (cUnit->enableDebug & (1 << kDebugShowMemoryUsage)) {
    oatDumpMemStats(cUnit.get());
  }
#endif

  oatArenaReset(cUnit.get());

  return result;
}

CompiledMethod* oatCompileMethod(Compiler& compiler,
                                 const CompilerBackend backend,
                                 const DexFile::CodeItem* code_item,
                                 uint32_t access_flags, InvokeType invoke_type,
                                 uint32_t method_idx, jobject class_loader,
                                 const DexFile& dex_file,
                                 LLVMInfo* llvmInfo)
{
  return compileMethod(compiler, backend, code_item, access_flags, invoke_type, method_idx, class_loader,
                       dex_file, llvmInfo);
}

}  // namespace art

extern "C" art::CompiledMethod*
    ArtQuickCompileMethod(art::Compiler& compiler,
                          const art::DexFile::CodeItem* code_item,
                          uint32_t access_flags, art::InvokeType invoke_type,
                          uint32_t method_idx, jobject class_loader,
                          const art::DexFile& dex_file)
{
  CHECK_EQ(compiler.GetInstructionSet(), art::oatInstructionSet());
  // TODO: check method fingerprint here to determine appropriate backend type.  Until then, use build default
  art::CompilerBackend backend = compiler.GetCompilerBackend();
  return art::oatCompileMethod(compiler, backend, code_item, access_flags, invoke_type,
                               method_idx, class_loader, dex_file, NULL /* use thread llvmInfo */);
}
