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

#include "compiler.h"
#include "compiler_internals.h"
#include "dataflow.h"
#include "ssa_transformation.h"
#include "leb128.h"
#include "object.h"
#include "runtime.h"
#include "codegen/codegen_util.h"
#include "codegen/mir_to_gbc.h"
#include "codegen/mir_to_lir.h"

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
  LLVMInfo* llvm_info = new LLVMInfo();
  compiler.SetCompilerContext(llvm_info);
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

static bool ContentIsInsn(const uint16_t* code_ptr) {
  uint16_t instr = *code_ptr;
  Instruction::Code opcode = static_cast<Instruction::Code>(instr & 0xff);

  /*
   * Since the low 8-bit in metadata may look like NOP, we need to check
   * both the low and whole sub-word to determine whether it is code or data.
   */
  return (opcode != Instruction::NOP || instr == 0);
}

/*
 * Parse an instruction, return the length of the instruction
 */
static int ParseInsn(CompilationUnit* cu, const uint16_t* code_ptr,
                     DecodedInstruction* decoded_instruction, bool verbose)
{
  // Don't parse instruction data
  if (!ContentIsInsn(code_ptr)) {
    return 0;
  }

  const Instruction* instruction = Instruction::At(code_ptr);
  *decoded_instruction = DecodedInstruction(instruction);

  if (verbose) {
    char* decoded_string = GetDalvikDisassembly(cu, *decoded_instruction,
                                                  NULL);
    LOG(INFO) << code_ptr << ": 0x" << std::hex << static_cast<int>(decoded_instruction->opcode)
              << " " << decoded_string;
  }
  return instruction->SizeInCodeUnits();
}

#define UNKNOWN_TARGET 0xffffffff

/* Split an existing block from the specified code offset into two */
static BasicBlock *SplitBlock(CompilationUnit* cu, unsigned int code_offset,
                              BasicBlock* orig_block, BasicBlock** immed_pred_block_p)
{
  MIR* insn = orig_block->first_mir_insn;
  while (insn) {
    if (insn->offset == code_offset) break;
    insn = insn->next;
  }
  if (insn == NULL) {
    LOG(FATAL) << "Break split failed";
  }
  BasicBlock *bottom_block = NewMemBB(cu, kDalvikByteCode,
                                     cu->num_blocks++);
  InsertGrowableList(cu, &cu->block_list, reinterpret_cast<uintptr_t>(bottom_block));

  bottom_block->start_offset = code_offset;
  bottom_block->first_mir_insn = insn;
  bottom_block->last_mir_insn = orig_block->last_mir_insn;

  /* Add it to the quick lookup cache */
  cu->block_map.Put(bottom_block->start_offset, bottom_block);

  /* Handle the taken path */
  bottom_block->taken = orig_block->taken;
  if (bottom_block->taken) {
    orig_block->taken = NULL;
    DeleteGrowableList(bottom_block->taken->predecessors, reinterpret_cast<uintptr_t>(orig_block));
    InsertGrowableList(cu, bottom_block->taken->predecessors,
                          reinterpret_cast<uintptr_t>(bottom_block));
  }

  /* Handle the fallthrough path */
  bottom_block->fall_through = orig_block->fall_through;
  orig_block->fall_through = bottom_block;
  InsertGrowableList(cu, bottom_block->predecessors,
                        reinterpret_cast<uintptr_t>(orig_block));
  if (bottom_block->fall_through) {
    DeleteGrowableList(bottom_block->fall_through->predecessors,
                          reinterpret_cast<uintptr_t>(orig_block));
    InsertGrowableList(cu, bottom_block->fall_through->predecessors,
                          reinterpret_cast<uintptr_t>(bottom_block));
  }

  /* Handle the successor list */
  if (orig_block->successor_block_list.block_list_type != kNotUsed) {
    bottom_block->successor_block_list = orig_block->successor_block_list;
    orig_block->successor_block_list.block_list_type = kNotUsed;
    GrowableListIterator iterator;

    GrowableListIteratorInit(&bottom_block->successor_block_list.blocks,
                                &iterator);
    while (true) {
      SuccessorBlockInfo *successor_block_info =
          reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));
      if (successor_block_info == NULL) break;
      BasicBlock *bb = successor_block_info->block;
      DeleteGrowableList(bb->predecessors, reinterpret_cast<uintptr_t>(orig_block));
      InsertGrowableList(cu, bb->predecessors, reinterpret_cast<uintptr_t>(bottom_block));
    }
  }

  orig_block->last_mir_insn = insn->prev;

  insn->prev->next = NULL;
  insn->prev = NULL;
  /*
   * Update the immediate predecessor block pointer so that outgoing edges
   * can be applied to the proper block.
   */
  if (immed_pred_block_p) {
    DCHECK_EQ(*immed_pred_block_p, orig_block);
    *immed_pred_block_p = bottom_block;
  }
  return bottom_block;
}

/*
 * Given a code offset, find out the block that starts with it. If the offset
 * is in the middle of an existing block, split it into two.  If immed_pred_block_p
 * is not non-null and is the block being split, update *immed_pred_block_p to
 * point to the bottom block so that outgoing edges can be set up properly
 * (by the caller)
 * Utilizes a map for fast lookup of the typical cases.
 */
BasicBlock *FindBlock(CompilationUnit* cu, unsigned int code_offset,
                      bool split, bool create, BasicBlock** immed_pred_block_p)
{
  GrowableList* block_list = &cu->block_list;
  BasicBlock* bb;
  unsigned int i;
  SafeMap<unsigned int, BasicBlock*>::iterator it;

  it = cu->block_map.find(code_offset);
  if (it != cu->block_map.end()) {
    return it->second;
  } else if (!create) {
    return NULL;
  }

  if (split) {
    for (i = 0; i < block_list->num_used; i++) {
      bb = reinterpret_cast<BasicBlock*>(block_list->elem_list[i]);
      if (bb->block_type != kDalvikByteCode) continue;
      /* Check if a branch jumps into the middle of an existing block */
      if ((code_offset > bb->start_offset) && (bb->last_mir_insn != NULL) &&
          (code_offset <= bb->last_mir_insn->offset)) {
        BasicBlock *new_bb = SplitBlock(cu, code_offset, bb,
                                       bb == *immed_pred_block_p ?
                                       immed_pred_block_p : NULL);
        return new_bb;
      }
    }
  }

  /* Create a new one */
  bb = NewMemBB(cu, kDalvikByteCode, cu->num_blocks++);
  InsertGrowableList(cu, &cu->block_list, reinterpret_cast<uintptr_t>(bb));
  bb->start_offset = code_offset;
  cu->block_map.Put(bb->start_offset, bb);
  return bb;
}

/* Find existing block */
BasicBlock* FindBlock(CompilationUnit* cu, unsigned int code_offset)
{
  return FindBlock(cu, code_offset, false, false, NULL);
}

/* Turn method name into a legal Linux file name */
void ReplaceSpecialChars(std::string& str)
{
  static const struct { const char before; const char after; } match[] =
      {{'/','-'}, {';','#'}, {' ','#'}, {'$','+'},
       {'(','@'}, {')','@'}, {'<','='}, {'>','='}};
  for (unsigned int i = 0; i < sizeof(match)/sizeof(match[0]); i++) {
    std::replace(str.begin(), str.end(), match[i].before, match[i].after);
  }
}

/* Dump the CFG into a DOT graph */
void DumpCFG(CompilationUnit* cu, const char* dir_prefix)
{
  FILE* file;
  std::string fname(PrettyMethod(cu->method_idx, *cu->dex_file));
  ReplaceSpecialChars(fname);
  fname = StringPrintf("%s%s%x.dot", dir_prefix, fname.c_str(),
                      cu->entry_block->fall_through->start_offset);
  file = fopen(fname.c_str(), "w");
  if (file == NULL) {
    return;
  }
  fprintf(file, "digraph G {\n");

  fprintf(file, "  rankdir=TB\n");

  int num_reachable_blocks = cu->num_reachable_blocks;
  int idx;
  const GrowableList *block_list = &cu->block_list;

  for (idx = 0; idx < num_reachable_blocks; idx++) {
    int block_idx = cu->dfs_order.elem_list[idx];
    BasicBlock *bb = reinterpret_cast<BasicBlock*>(GrowableListGetElement(block_list, block_idx));
    if (bb == NULL) break;
    if (bb->block_type == kDead) continue;
    if (bb->block_type == kEntryBlock) {
      fprintf(file, "  entry_%d [shape=Mdiamond];\n", bb->id);
    } else if (bb->block_type == kExitBlock) {
      fprintf(file, "  exit_%d [shape=Mdiamond];\n", bb->id);
    } else if (bb->block_type == kDalvikByteCode) {
      fprintf(file, "  block%04x_%d [shape=record,label = \"{ \\\n",
              bb->start_offset, bb->id);
      const MIR *mir;
        fprintf(file, "    {block id %d\\l}%s\\\n", bb->id,
                bb->first_mir_insn ? " | " : " ");
        for (mir = bb->first_mir_insn; mir; mir = mir->next) {
            fprintf(file, "    {%04x %s\\l}%s\\\n", mir->offset,
                    mir->ssa_rep ? FullDisassembler(cu, mir) :
                    Instruction::Name(mir->dalvikInsn.opcode),
                    mir->next ? " | " : " ");
        }
        fprintf(file, "  }\"];\n\n");
    } else if (bb->block_type == kExceptionHandling) {
      char block_name[BLOCK_NAME_LEN];

      GetBlockName(bb, block_name);
      fprintf(file, "  %s [shape=invhouse];\n", block_name);
    }

    char block_name1[BLOCK_NAME_LEN], block_name2[BLOCK_NAME_LEN];

    if (bb->taken) {
      GetBlockName(bb, block_name1);
      GetBlockName(bb->taken, block_name2);
      fprintf(file, "  %s:s -> %s:n [style=dotted]\n",
              block_name1, block_name2);
    }
    if (bb->fall_through) {
      GetBlockName(bb, block_name1);
      GetBlockName(bb->fall_through, block_name2);
      fprintf(file, "  %s:s -> %s:n\n", block_name1, block_name2);
    }

    if (bb->successor_block_list.block_list_type != kNotUsed) {
      fprintf(file, "  succ%04x_%d [shape=%s,label = \"{ \\\n",
              bb->start_offset, bb->id,
              (bb->successor_block_list.block_list_type == kCatch) ?
               "Mrecord" : "record");
      GrowableListIterator iterator;
      GrowableListIteratorInit(&bb->successor_block_list.blocks,
                                  &iterator);
      SuccessorBlockInfo *successor_block_info =
          reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));

      int succ_id = 0;
      while (true) {
        if (successor_block_info == NULL) break;

        BasicBlock *dest_block = successor_block_info->block;
        SuccessorBlockInfo *next_successor_block_info =
            reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));

        fprintf(file, "    {<f%d> %04x: %04x\\l}%s\\\n",
                succ_id++,
                successor_block_info->key,
                dest_block->start_offset,
                (next_successor_block_info != NULL) ? " | " : " ");

        successor_block_info = next_successor_block_info;
      }
      fprintf(file, "  }\"];\n\n");

      GetBlockName(bb, block_name1);
      fprintf(file, "  %s:s -> succ%04x_%d:n [style=dashed]\n",
              block_name1, bb->start_offset, bb->id);

      if (bb->successor_block_list.block_list_type == kPackedSwitch ||
          bb->successor_block_list.block_list_type == kSparseSwitch) {

        GrowableListIteratorInit(&bb->successor_block_list.blocks,
                                    &iterator);

        succ_id = 0;
        while (true) {
          SuccessorBlockInfo *successor_block_info =
              reinterpret_cast<SuccessorBlockInfo*>( GrowableListIteratorNext(&iterator));
          if (successor_block_info == NULL) break;

          BasicBlock *dest_block = successor_block_info->block;

          GetBlockName(dest_block, block_name2);
          fprintf(file, "  succ%04x_%d:f%d:e -> %s:n\n", bb->start_offset,
                  bb->id, succ_id++, block_name2);
        }
      }
    }
    fprintf(file, "\n");

    /* Display the dominator tree */
    GetBlockName(bb, block_name1);
    fprintf(file, "  cfg%s [label=\"%s\", shape=none];\n",
            block_name1, block_name1);
    if (bb->i_dom) {
      GetBlockName(bb->i_dom, block_name2);
      fprintf(file, "  cfg%s:s -> cfg%s:n\n\n", block_name2, block_name1);
    }
  }
  fprintf(file, "}\n");
  fclose(file);
}

/* Verify if all the successor is connected with all the claimed predecessors */
static bool VerifyPredInfo(CompilationUnit* cu, BasicBlock* bb)
{
  GrowableListIterator iter;

  GrowableListIteratorInit(bb->predecessors, &iter);
  while (true) {
    BasicBlock *pred_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
    if (!pred_bb) break;
    bool found = false;
    if (pred_bb->taken == bb) {
        found = true;
    } else if (pred_bb->fall_through == bb) {
        found = true;
    } else if (pred_bb->successor_block_list.block_list_type != kNotUsed) {
      GrowableListIterator iterator;
      GrowableListIteratorInit(&pred_bb->successor_block_list.blocks,
                                  &iterator);
      while (true) {
        SuccessorBlockInfo *successor_block_info =
            reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));
        if (successor_block_info == NULL) break;
        BasicBlock *succ_bb = successor_block_info->block;
        if (succ_bb == bb) {
            found = true;
            break;
        }
      }
    }
    if (found == false) {
      char block_name1[BLOCK_NAME_LEN], block_name2[BLOCK_NAME_LEN];
      GetBlockName(bb, block_name1);
      GetBlockName(pred_bb, block_name2);
      DumpCFG(cu, "/sdcard/cfg/");
      LOG(FATAL) << "Successor " << block_name1 << "not found from "
                 << block_name2;
    }
  }
  return true;
}

/* Identify code range in try blocks and set up the empty catch blocks */
static void ProcessTryCatchBlocks(CompilationUnit* cu)
{
  const DexFile::CodeItem* code_item = cu->code_item;
  int tries_size = code_item->tries_size_;
  int offset;

  if (tries_size == 0) {
    return;
  }

  ArenaBitVector* try_block_addr = cu->try_block_addr;

  for (int i = 0; i < tries_size; i++) {
    const DexFile::TryItem* pTry =
        DexFile::GetTryItems(*code_item, i);
    int start_offset = pTry->start_addr_;
    int end_offset = start_offset + pTry->insn_count_;
    for (offset = start_offset; offset < end_offset; offset++) {
      SetBit(cu, try_block_addr, offset);
    }
  }

  // Iterate over each of the handlers to enqueue the empty Catch blocks
  const byte* handlers_ptr = DexFile::GetCatchHandlerData(*code_item, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    CatchHandlerIterator iterator(handlers_ptr);
    for (; iterator.HasNext(); iterator.Next()) {
      uint32_t address = iterator.GetHandlerAddress();
      FindBlock(cu, address, false /* split */, true /*create*/,
                /* immed_pred_block_p */ NULL);
    }
    handlers_ptr = iterator.EndDataPointer();
  }
}

/* Process instructions with the kBranch flag */
static BasicBlock* ProcessCanBranch(CompilationUnit* cu, BasicBlock* cur_block,
                                    MIR* insn, int cur_offset, int width, int flags,
                                    const uint16_t* code_ptr, const uint16_t* code_end)
{
  int target = cur_offset;
  switch (insn->dalvikInsn.opcode) {
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      target += insn->dalvikInsn.vA;
      break;
    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
      cur_block->conditional_branch = true;
      target += insn->dalvikInsn.vC;
      break;
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ:
      cur_block->conditional_branch = true;
      target += insn->dalvikInsn.vB;
      break;
    default:
      LOG(FATAL) << "Unexpected opcode(" << insn->dalvikInsn.opcode << ") with kBranch set";
  }
  BasicBlock *taken_block = FindBlock(cu, target,
                                     /* split */
                                     true,
                                     /* create */
                                     true,
                                     /* immed_pred_block_p */
                                     &cur_block);
  cur_block->taken = taken_block;
  InsertGrowableList(cu, taken_block->predecessors, reinterpret_cast<uintptr_t>(cur_block));

  /* Always terminate the current block for conditional branches */
  if (flags & Instruction::kContinue) {
    BasicBlock *fallthrough_block = FindBlock(cu,
                                             cur_offset +  width,
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
                                             /* immed_pred_block_p */
                                             &cur_block);
    cur_block->fall_through = fallthrough_block;
    InsertGrowableList(cu, fallthrough_block->predecessors,
                          reinterpret_cast<uintptr_t>(cur_block));
  } else if (code_ptr < code_end) {
    /* Create a fallthrough block for real instructions (incl. NOP) */
    if (ContentIsInsn(code_ptr)) {
      FindBlock(cu, cur_offset + width,
                /* split */
                false,
                /* create */
                true,
                /* immed_pred_block_p */
                NULL);
    }
  }
  return cur_block;
}

/* Process instructions with the kSwitch flag */
static void ProcessCanSwitch(CompilationUnit* cu, BasicBlock* cur_block,
                             MIR* insn, int cur_offset, int width, int flags)
{
  const uint16_t* switch_data =
      reinterpret_cast<const uint16_t*>(cu->insns + cur_offset + insn->dalvikInsn.vB);
  int size;
  const int* keyTable;
  const int* target_table;
  int i;
  int first_key;

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
    DCHECK_EQ(static_cast<int>(switch_data[0]),
              static_cast<int>(Instruction::kPackedSwitchSignature));
    size = switch_data[1];
    first_key = switch_data[2] | (switch_data[3] << 16);
    target_table = reinterpret_cast<const int*>(&switch_data[4]);
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
    DCHECK_EQ(static_cast<int>(switch_data[0]),
              static_cast<int>(Instruction::kSparseSwitchSignature));
    size = switch_data[1];
    keyTable = reinterpret_cast<const int*>(&switch_data[2]);
    target_table = reinterpret_cast<const int*>(&switch_data[2 + size*2]);
    first_key = 0;   // To make the compiler happy
  }

  if (cur_block->successor_block_list.block_list_type != kNotUsed) {
    LOG(FATAL) << "Successor block list already in use: "
               << static_cast<int>(cur_block->successor_block_list.block_list_type);
  }
  cur_block->successor_block_list.block_list_type =
      (insn->dalvikInsn.opcode == Instruction::PACKED_SWITCH) ?
      kPackedSwitch : kSparseSwitch;
  CompilerInitGrowableList(cu, &cur_block->successor_block_list.blocks, size,
                      kListSuccessorBlocks);

  for (i = 0; i < size; i++) {
    BasicBlock *case_block = FindBlock(cu, cur_offset + target_table[i],
                                      /* split */
                                      true,
                                      /* create */
                                      true,
                                      /* immed_pred_block_p */
                                      &cur_block);
    SuccessorBlockInfo *successor_block_info =
        static_cast<SuccessorBlockInfo*>(NewMem(cu, sizeof(SuccessorBlockInfo),
                                         false, kAllocSuccessor));
    successor_block_info->block = case_block;
    successor_block_info->key =
        (insn->dalvikInsn.opcode == Instruction::PACKED_SWITCH) ?
        first_key + i : keyTable[i];
    InsertGrowableList(cu, &cur_block->successor_block_list.blocks,
                          reinterpret_cast<uintptr_t>(successor_block_info));
    InsertGrowableList(cu, case_block->predecessors,
                          reinterpret_cast<uintptr_t>(cur_block));
  }

  /* Fall-through case */
  BasicBlock* fallthrough_block = FindBlock(cu,
                                           cur_offset +  width,
                                           /* split */
                                           false,
                                           /* create */
                                           true,
                                           /* immed_pred_block_p */
                                           NULL);
  cur_block->fall_through = fallthrough_block;
  InsertGrowableList(cu, fallthrough_block->predecessors,
                        reinterpret_cast<uintptr_t>(cur_block));
}

/* Process instructions with the kThrow flag */
static BasicBlock* ProcessCanThrow(CompilationUnit* cu, BasicBlock* cur_block,
                                   MIR* insn, int cur_offset, int width, int flags,
                                   ArenaBitVector* try_block_addr, const uint16_t* code_ptr,
                                   const uint16_t* code_end)
{
  const DexFile::CodeItem* code_item = cu->code_item;
  bool in_try_block = IsBitSet(try_block_addr, cur_offset);

  /* In try block */
  if (in_try_block) {
    CatchHandlerIterator iterator(*code_item, cur_offset);

    if (cur_block->successor_block_list.block_list_type != kNotUsed) {
      LOG(INFO) << PrettyMethod(cu->method_idx, *cu->dex_file);
      LOG(FATAL) << "Successor block list already in use: "
                 << static_cast<int>(cur_block->successor_block_list.block_list_type);
    }

    cur_block->successor_block_list.block_list_type = kCatch;
    CompilerInitGrowableList(cu, &cur_block->successor_block_list.blocks, 2,
                        kListSuccessorBlocks);

    for (;iterator.HasNext(); iterator.Next()) {
      BasicBlock *catch_block = FindBlock(cu, iterator.GetHandlerAddress(),
                                         false /* split*/,
                                         false /* creat */,
                                         NULL  /* immed_pred_block_p */);
      catch_block->catch_entry = true;
      cu->catches.insert(catch_block->start_offset);
      SuccessorBlockInfo *successor_block_info = reinterpret_cast<SuccessorBlockInfo*>
          (NewMem(cu, sizeof(SuccessorBlockInfo), false, kAllocSuccessor));
      successor_block_info->block = catch_block;
      successor_block_info->key = iterator.GetHandlerTypeIndex();
      InsertGrowableList(cu, &cur_block->successor_block_list.blocks,
                            reinterpret_cast<uintptr_t>(successor_block_info));
      InsertGrowableList(cu, catch_block->predecessors,
                            reinterpret_cast<uintptr_t>(cur_block));
    }
  } else {
    BasicBlock *eh_block = NewMemBB(cu, kExceptionHandling,
                                   cu->num_blocks++);
    cur_block->taken = eh_block;
    InsertGrowableList(cu, &cu->block_list, reinterpret_cast<uintptr_t>(eh_block));
    eh_block->start_offset = cur_offset;
    InsertGrowableList(cu, eh_block->predecessors, reinterpret_cast<uintptr_t>(cur_block));
  }

  if (insn->dalvikInsn.opcode == Instruction::THROW){
    cur_block->explicit_throw = true;
    if ((code_ptr < code_end) && ContentIsInsn(code_ptr)) {
      // Force creation of new block following THROW via side-effect
      FindBlock(cu, cur_offset + width, /* split */ false,
                /* create */ true, /* immed_pred_block_p */ NULL);
    }
    if (!in_try_block) {
       // Don't split a THROW that can't rethrow - we're done.
      return cur_block;
    }
  }

  /*
   * Split the potentially-throwing instruction into two parts.
   * The first half will be a pseudo-op that captures the exception
   * edges and terminates the basic block.  It always falls through.
   * Then, create a new basic block that begins with the throwing instruction
   * (minus exceptions).  Note: this new basic block must NOT be entered into
   * the block_map.  If the potentially-throwing instruction is the target of a
   * future branch, we need to find the check psuedo half.  The new
   * basic block containing the work portion of the instruction should
   * only be entered via fallthrough from the block containing the
   * pseudo exception edge MIR.  Note also that this new block is
   * not automatically terminated after the work portion, and may
   * contain following instructions.
   */
  BasicBlock *new_block = NewMemBB(cu, kDalvikByteCode, cu->num_blocks++);
  InsertGrowableList(cu, &cu->block_list, reinterpret_cast<uintptr_t>(new_block));
  new_block->start_offset = insn->offset;
  cur_block->fall_through = new_block;
  InsertGrowableList(cu, new_block->predecessors, reinterpret_cast<uintptr_t>(cur_block));
  MIR* new_insn = static_cast<MIR*>(NewMem(cu, sizeof(MIR), true, kAllocMIR));
  *new_insn = *insn;
  insn->dalvikInsn.opcode =
      static_cast<Instruction::Code>(kMirOpCheck);
  // Associate the two halves
  insn->meta.throw_insn = new_insn;
  new_insn->meta.throw_insn = insn;
  AppendMIR(new_block, new_insn);
  return new_block;
}

void CompilerInit(CompilationUnit* cu, const Compiler& compiler) {
  bool success = false;
  switch (compiler.GetInstructionSet()) {
    case kThumb2:
      success = InitArmCodegen(cu);
      break;
    case kMips:
      success = InitMipsCodegen(cu);
      break;
    case kX86:
      success = InitX86Codegen(cu);
      break;
    default:;
  }
  if (!success) {
    LOG(FATAL) << "Failed to initialize codegen for " << compiler.GetInstructionSet();
  }
  if (!HeapInit(cu)) {
    LOG(FATAL) << "Failed to initialize oat heap";
  }
}

static CompiledMethod* CompileMethod(Compiler& compiler,
                                     const CompilerBackend compiler_backend,
                                     const DexFile::CodeItem* code_item,
                                     uint32_t access_flags, InvokeType invoke_type,
                                     uint32_t method_idx, jobject class_loader,
                                     const DexFile& dex_file,
                                     LLVMInfo* llvm_info)
{
  VLOG(compiler) << "Compiling " << PrettyMethod(method_idx, dex_file) << "...";

  const uint16_t* code_ptr = code_item->insns_;
  const uint16_t* code_end = code_item->insns_ + code_item->insns_size_in_code_units_;
  int num_blocks = 0;
  unsigned int cur_offset = 0;

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  UniquePtr<CompilationUnit> cu(new CompilationUnit);

  CompilerInit(cu.get(), compiler);

  cu->compiler = &compiler;
  cu->class_linker = class_linker;
  cu->dex_file = &dex_file;
  cu->method_idx = method_idx;
  cu->code_item = code_item;
  cu->access_flags = access_flags;
  cu->invoke_type = invoke_type;
  cu->shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
  cu->instruction_set = compiler.GetInstructionSet();
  cu->insns = code_item->insns_;
  cu->insns_size = code_item->insns_size_in_code_units_;
  cu->num_ins = code_item->ins_size_;
  cu->num_regs = code_item->registers_size_ - cu->num_ins;
  cu->num_outs = code_item->outs_size_;
  DCHECK((cu->instruction_set == kThumb2) ||
         (cu->instruction_set == kX86) ||
         (cu->instruction_set == kMips));
  if ((compiler_backend == kQuickGBC) || (compiler_backend == kPortable)) {
    cu->gen_bitcode = true;
  }
  DCHECK_NE(compiler_backend, kIceland);  // TODO: remove when Portable/Iceland merge complete
  // TODO: remove this once x86 is tested
  if (cu->gen_bitcode && (cu->instruction_set != kThumb2)) {
    UNIMPLEMENTED(WARNING) << "GBC generation untested for non-Thumb targets";
  }
  cu->llvm_info = llvm_info;
  /* Adjust this value accordingly once inlining is performed */
  cu->num_dalvik_registers = code_item->registers_size_;
  // TODO: set this from command line
  cu->compiler_flip_match = false;
  bool use_match = !cu->compiler_method_match.empty();
  bool match = use_match && (cu->compiler_flip_match ^
      (PrettyMethod(method_idx, dex_file).find(cu->compiler_method_match) !=
       std::string::npos));
  if (!use_match || match) {
    cu->disable_opt = kCompilerOptimizerDisableFlags;
    cu->enable_debug = kCompilerDebugFlags;
    cu->verbose = VLOG_IS_ON(compiler) ||
        (cu->enable_debug & (1 << kDebugVerbose));
  }
#ifndef NDEBUG
  if (cu->gen_bitcode) {
    cu->enable_debug |= (1 << kDebugVerifyBitcode);
  }
#endif

  if (cu->instruction_set == kMips) {
    // Disable some optimizations for mips for now
    cu->disable_opt |= (
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
    cu->opcode_count =
        static_cast<int*>(NewMem(cu.get(), kNumPackedOpcodes * sizeof(int), true, kAllocMisc));
  }

  /* Assume non-throwing leaf */
  cu->attrs = (METHOD_IS_LEAF | METHOD_IS_THROW_FREE);

  /* Initialize the block list, estimate size based on insns_size */
  CompilerInitGrowableList(cu.get(), &cu->block_list, cu->insns_size,
                      kListBlockList);

  /* Initialize the switch_tables list */
  CompilerInitGrowableList(cu.get(), &cu->switch_tables, 4,
                      kListSwitchTables);

  /* Intialize the fill_array_data list */
  CompilerInitGrowableList(cu.get(), &cu->fill_array_data, 4,
                      kListFillArrayData);

  /* Intialize the throw_launchpads list, estimate size based on insns_size */
  CompilerInitGrowableList(cu.get(), &cu->throw_launchpads, cu->insns_size,
                      kListThrowLaunchPads);

  /* Intialize the instrinsic_launchpads list */
  CompilerInitGrowableList(cu.get(), &cu->intrinsic_launchpads, 4,
                      kListMisc);


  /* Intialize the suspend_launchpads list */
  CompilerInitGrowableList(cu.get(), &cu->suspend_launchpads, 2048,
                      kListSuspendLaunchPads);

  /* Allocate the bit-vector to track the beginning of basic blocks */
  ArenaBitVector *try_block_addr = AllocBitVector(cu.get(),
                                                   cu->insns_size,
                                                   true /* expandable */);
  cu->try_block_addr = try_block_addr;

  /* Create the default entry and exit blocks and enter them to the list */
  BasicBlock *entry_block = NewMemBB(cu.get(), kEntryBlock, num_blocks++);
  BasicBlock *exit_block = NewMemBB(cu.get(), kExitBlock, num_blocks++);

  cu->entry_block = entry_block;
  cu->exit_block = exit_block;

  InsertGrowableList(cu.get(), &cu->block_list, reinterpret_cast<uintptr_t>(entry_block));
  InsertGrowableList(cu.get(), &cu->block_list, reinterpret_cast<uintptr_t>(exit_block));

  /* Current block to record parsed instructions */
  BasicBlock *cur_block = NewMemBB(cu.get(), kDalvikByteCode, num_blocks++);
  cur_block->start_offset = 0;
  InsertGrowableList(cu.get(), &cu->block_list, reinterpret_cast<uintptr_t>(cur_block));
  /* Add first block to the fast lookup cache */
  cu->block_map.Put(cur_block->start_offset, cur_block);
  entry_block->fall_through = cur_block;
  InsertGrowableList(cu.get(), cur_block->predecessors,
                        reinterpret_cast<uintptr_t>(entry_block));

  /*
   * Store back the number of blocks since new blocks may be created of
   * accessing cu.
   */
  cu->num_blocks = num_blocks;

  /* Identify code range in try blocks and set up the empty catch blocks */
  ProcessTryCatchBlocks(cu.get());

  /* Set up for simple method detection */
  int num_patterns = sizeof(special_patterns)/sizeof(special_patterns[0]);
  bool live_pattern = (num_patterns > 0) && !(cu->disable_opt & (1 << kMatch));
  bool* dead_pattern =
      static_cast<bool*>(NewMem(cu.get(), sizeof(bool) * num_patterns, true, kAllocMisc));
  SpecialCaseHandler special_case = kNoHandler;
  int pattern_pos = 0;

  /* Parse all instructions and put them into containing basic blocks */
  while (code_ptr < code_end) {
    MIR *insn = static_cast<MIR *>(NewMem(cu.get(), sizeof(MIR), true, kAllocMIR));
    insn->offset = cur_offset;
    int width = ParseInsn(cu.get(), code_ptr, &insn->dalvikInsn, false);
    insn->width = width;
    Instruction::Code opcode = insn->dalvikInsn.opcode;
    if (cu->opcode_count != NULL) {
      cu->opcode_count[static_cast<int>(opcode)]++;
    }

    /* Terminate when the data section is seen */
    if (width == 0)
      break;

    /* Possible simple method? */
    if (live_pattern) {
      live_pattern = false;
      special_case = kNoHandler;
      for (int i = 0; i < num_patterns; i++) {
        if (!dead_pattern[i]) {
          if (special_patterns[i].opcodes[pattern_pos] == opcode) {
            live_pattern = true;
            special_case = special_patterns[i].handler_code;
          } else {
             dead_pattern[i] = true;
          }
        }
      }
    pattern_pos++;
    }

    AppendMIR(cur_block, insn);

    code_ptr += width;
    int flags = Instruction::FlagsOf(insn->dalvikInsn.opcode);

    int df_flags = oat_data_flow_attributes[insn->dalvikInsn.opcode];

    if (df_flags & DF_HAS_DEFS) {
      cu->def_count += (df_flags & DF_A_WIDE) ? 2 : 1;
    }

    if (flags & Instruction::kBranch) {
      cur_block = ProcessCanBranch(cu.get(), cur_block, insn, cur_offset,
                                  width, flags, code_ptr, code_end);
    } else if (flags & Instruction::kReturn) {
      cur_block->fall_through = exit_block;
      InsertGrowableList(cu.get(), exit_block->predecessors,
                            reinterpret_cast<uintptr_t>(cur_block));
      /*
       * Terminate the current block if there are instructions
       * afterwards.
       */
      if (code_ptr < code_end) {
        /*
         * Create a fallthrough block for real instructions
         * (incl. NOP).
         */
        if (ContentIsInsn(code_ptr)) {
            FindBlock(cu.get(), cur_offset + width,
                      /* split */
                      false,
                      /* create */
                      true,
                      /* immed_pred_block_p */
                      NULL);
        }
      }
    } else if (flags & Instruction::kThrow) {
      cur_block = ProcessCanThrow(cu.get(), cur_block, insn, cur_offset,
                                 width, flags, try_block_addr, code_ptr, code_end);
    } else if (flags & Instruction::kSwitch) {
      ProcessCanSwitch(cu.get(), cur_block, insn, cur_offset, width, flags);
    }
    cur_offset += width;
    BasicBlock *next_block = FindBlock(cu.get(), cur_offset,
                                      /* split */
                                      false,
                                      /* create */
                                      false,
                                      /* immed_pred_block_p */
                                      NULL);
    if (next_block) {
      /*
       * The next instruction could be the target of a previously parsed
       * forward branch so a block is already created. If the current
       * instruction is not an unconditional branch, connect them through
       * the fall-through link.
       */
      DCHECK(cur_block->fall_through == NULL ||
             cur_block->fall_through == next_block ||
             cur_block->fall_through == exit_block);

      if ((cur_block->fall_through == NULL) && (flags & Instruction::kContinue)) {
        cur_block->fall_through = next_block;
        InsertGrowableList(cu.get(), next_block->predecessors,
                              reinterpret_cast<uintptr_t>(cur_block));
      }
      cur_block = next_block;
    }
  }

  if (!(cu->disable_opt & (1 << kSkipLargeMethodOptimization))) {
    if ((cu->num_blocks > MANY_BLOCKS) ||
        ((cu->num_blocks > MANY_BLOCKS_INITIALIZER) &&
      PrettyMethod(method_idx, dex_file, false).find("init>") !=
          std::string::npos)) {
        cu->qd_mode = true;
    }
  }

  if (cu->qd_mode) {
    // Bitcode generation requires full dataflow analysis
    cu->disable_dataflow = !cu->gen_bitcode;
    // Disable optimization which require dataflow/ssa
    cu->disable_opt |= (1 << kBBOpt) | (1 << kPromoteRegs) | (1 << kNullCheckElimination);
    if (cu->verbose) {
        LOG(INFO) << "QD mode enabled: "
                  << PrettyMethod(method_idx, dex_file)
                  << " num blocks: " << cu->num_blocks;
    }
  }

  if (cu->verbose) {
    DumpCompilationUnit(cu.get());
  }

  /* Do a code layout pass */
  CodeLayout(cu.get());

  if (cu->enable_debug & (1 << kDebugVerifyDataflow)) {
    /* Verify if all blocks are connected as claimed */
    DataFlowAnalysisDispatcher(cu.get(), VerifyPredInfo, kAllNodes,
                                  false /* is_iterative */);
  }

  /* Perform SSA transformation for the whole method */
  SSATransformation(cu.get());

  /* Do constant propagation */
  // TODO: Probably need to make these expandable to support new ssa names
  // introducted during MIR optimization passes
  cu->is_constant_v = AllocBitVector(cu.get(), cu->num_ssa_regs,
                                         false  /* not expandable */);
  cu->constant_values =
      static_cast<int*>(NewMem(cu.get(), sizeof(int) * cu->num_ssa_regs, true, kAllocDFInfo));
  DataFlowAnalysisDispatcher(cu.get(), DoConstantPropogation,
                                kAllNodes,
                                false /* is_iterative */);

  /* Detect loops */
  LoopDetection(cu.get());

  /* Count uses */
  MethodUseCount(cu.get());

  /* Perform null check elimination */
  NullCheckElimination(cu.get());

  /* Combine basic blocks where possible */
  BasicBlockCombine(cu.get());

  /* Do some basic block optimizations */
  BasicBlockOptimization(cu.get());

  if (cu->enable_debug & (1 << kDebugDumpCheckStats)) {
    DumpCheckStats(cu.get());
  }

  cu.get()->cg->CompilerInitializeRegAlloc(cu.get());  // Needs to happen after SSA naming

  /* Allocate Registers using simple local allocation scheme */
  SimpleRegAlloc(cu.get());

  /* Go the LLVM path? */
  if (cu->gen_bitcode) {
    // MIR->Bitcode
    MethodMIR2Bitcode(cu.get());
    if (compiler_backend == kPortable) {
      // all done
      ArenaReset(cu.get());
      return NULL;
    }
    // Bitcode->LIR
    MethodBitcode2LIR(cu.get());
  } else {
    if (special_case != kNoHandler) {
      /*
       * Custom codegen for special cases.  If for any reason the
       * special codegen doesn't succeed, cu->first_lir_insn will
       * set to NULL;
       */
      SpecialMIR2LIR(cu.get(), special_case);
    }

    /* Convert MIR to LIR, etc. */
    if (cu->first_lir_insn == NULL) {
      MethodMIR2LIR(cu.get());
    }
  }

  // Debugging only
  if (cu->enable_debug & (1 << kDebugDumpCFG)) {
    DumpCFG(cu.get(), "/sdcard/cfg/");
  }

  /* Method is not empty */
  if (cu->first_lir_insn) {

    // mark the targets of switch statement case labels
    ProcessSwitchTables(cu.get());

    /* Convert LIR into machine code. */
    AssembleLIR(cu.get());

    if (cu->verbose) {
      CodegenDump(cu.get());
    }

    if (cu->opcode_count != NULL) {
      LOG(INFO) << "Opcode Count";
      for (int i = 0; i < kNumPackedOpcodes; i++) {
        if (cu->opcode_count[i] != 0) {
          LOG(INFO) << "-C- "
                    << Instruction::Name(static_cast<Instruction::Code>(i))
                    << " " << cu->opcode_count[i];
        }
      }
    }
  }

  // Combine vmap tables - core regs, then fp regs - into vmap_table
  std::vector<uint16_t> vmap_table;
  // Core regs may have been inserted out of order - sort first
  std::sort(cu->core_vmap_table.begin(), cu->core_vmap_table.end());
  for (size_t i = 0 ; i < cu->core_vmap_table.size(); i++) {
    // Copy, stripping out the phys register sort key
    vmap_table.push_back(~(-1 << VREG_NUM_WIDTH) & cu->core_vmap_table[i]);
  }
  // If we have a frame, push a marker to take place of lr
  if (cu->frame_size > 0) {
    vmap_table.push_back(INVALID_VREG);
  } else {
    DCHECK_EQ(__builtin_popcount(cu->core_spill_mask), 0);
    DCHECK_EQ(__builtin_popcount(cu->fp_spill_mask), 0);
  }
  // Combine vmap tables - core regs, then fp regs. fp regs already sorted
  for (uint32_t i = 0; i < cu->fp_vmap_table.size(); i++) {
    vmap_table.push_back(cu->fp_vmap_table[i]);
  }
  CompiledMethod* result =
      new CompiledMethod(cu->instruction_set, cu->code_buffer,
                         cu->frame_size, cu->core_spill_mask, cu->fp_spill_mask,
                         cu->combined_mapping_table, vmap_table, cu->native_gc_map);

  VLOG(compiler) << "Compiled " << PrettyMethod(method_idx, dex_file)
     << " (" << (cu->code_buffer.size() * sizeof(cu->code_buffer[0]))
     << " bytes)";

#ifdef WITH_MEMSTATS
  if (cu->enable_debug & (1 << kDebugShowMemoryUsage)) {
    DumpMemStats(cu.get());
  }
#endif

  ArenaReset(cu.get());

  return result;
}

CompiledMethod* CompileOneMethod(Compiler& compiler,
                                 const CompilerBackend backend,
                                 const DexFile::CodeItem* code_item,
                                 uint32_t access_flags, InvokeType invoke_type,
                                 uint32_t method_idx, jobject class_loader,
                                 const DexFile& dex_file,
                                 LLVMInfo* llvm_info)
{
  return CompileMethod(compiler, backend, code_item, access_flags, invoke_type, method_idx, class_loader,
                       dex_file, llvm_info);
}

}  // namespace art

extern "C" art::CompiledMethod*
    ArtQuickCompileMethod(art::Compiler& compiler,
                          const art::DexFile::CodeItem* code_item,
                          uint32_t access_flags, art::InvokeType invoke_type,
                          uint32_t method_idx, jobject class_loader,
                          const art::DexFile& dex_file)
{
  // TODO: check method fingerprint here to determine appropriate backend type.  Until then, use build default
  art::CompilerBackend backend = compiler.GetCompilerBackend();
  return art::CompileOneMethod(compiler, backend, code_item, access_flags, invoke_type,
                               method_idx, class_loader, dex_file, NULL /* use thread llvm_info */);
}
