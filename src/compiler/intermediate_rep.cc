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

#include "compiler_internals.h"

namespace art {

/* Allocate a new basic block */
BasicBlock* NewMemBB(CompilationUnit* cu, BBType block_type, int block_id)
{
  BasicBlock* bb = static_cast<BasicBlock*>(NewMem(cu, sizeof(BasicBlock), true, kAllocBB));
  bb->block_type = block_type;
  bb->id = block_id;
  bb->predecessors = static_cast<GrowableList*>
      (NewMem(cu, sizeof(GrowableList), false, kAllocPredecessors));
  CompilerInitGrowableList(cu, bb->predecessors,
                      (block_type == kExitBlock) ? 2048 : 2,
                      kListPredecessors);
  cu->block_id_map.Put(block_id, block_id);
  return bb;
}

/* Insert an MIR instruction to the end of a basic block */
void AppendMIR(BasicBlock* bb, MIR* mir)
{
  if (bb->first_mir_insn == NULL) {
    DCHECK(bb->last_mir_insn == NULL);
    bb->last_mir_insn = bb->first_mir_insn = mir;
    mir->prev = mir->next = NULL;
  } else {
    bb->last_mir_insn->next = mir;
    mir->prev = bb->last_mir_insn;
    mir->next = NULL;
    bb->last_mir_insn = mir;
  }
}

/* Insert an MIR instruction to the head of a basic block */
void PrependMIR(BasicBlock* bb, MIR* mir)
{
  if (bb->first_mir_insn == NULL) {
    DCHECK(bb->last_mir_insn == NULL);
    bb->last_mir_insn = bb->first_mir_insn = mir;
    mir->prev = mir->next = NULL;
  } else {
    bb->first_mir_insn->prev = mir;
    mir->next = bb->first_mir_insn;
    mir->prev = NULL;
    bb->first_mir_insn = mir;
  }
}

/* Insert a MIR instruction after the specified MIR */
void InsertMIRAfter(BasicBlock* bb, MIR* current_mir, MIR* new_mir)
{
  new_mir->prev = current_mir;
  new_mir->next = current_mir->next;
  current_mir->next = new_mir;

  if (new_mir->next) {
    /* Is not the last MIR in the block */
    new_mir->next->prev = new_mir;
  } else {
    /* Is the last MIR in the block */
    bb->last_mir_insn = new_mir;
  }
}

/*
 * Append an LIR instruction to the LIR list maintained by a compilation
 * unit
 */
void AppendLIR(CompilationUnit *cu, LIR* lir)
{
  if (cu->first_lir_insn == NULL) {
    DCHECK(cu->last_lir_insn == NULL);
     cu->last_lir_insn = cu->first_lir_insn = lir;
    lir->prev = lir->next = NULL;
  } else {
    cu->last_lir_insn->next = lir;
    lir->prev = cu->last_lir_insn;
    lir->next = NULL;
    cu->last_lir_insn = lir;
  }
}

/*
 * Insert an LIR instruction before the current instruction, which cannot be the
 * first instruction.
 *
 * prev_lir <-> new_lir <-> current_lir
 */
void InsertLIRBefore(LIR* current_lir, LIR* new_lir)
{
  DCHECK(current_lir->prev != NULL);
  LIR *prev_lir = current_lir->prev;

  prev_lir->next = new_lir;
  new_lir->prev = prev_lir;
  new_lir->next = current_lir;
  current_lir->prev = new_lir;
}

/*
 * Insert an LIR instruction after the current instruction, which cannot be the
 * first instruction.
 *
 * current_lir -> new_lir -> old_next
 */
void InsertLIRAfter(LIR* current_lir, LIR* new_lir)
{
  new_lir->prev = current_lir;
  new_lir->next = current_lir->next;
  current_lir->next = new_lir;
  new_lir->next->prev = new_lir;
}

}  // namespace art
