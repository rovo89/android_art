/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "post_opt_passes.h"
#include "dataflow_iterator.h"
#include "dataflow_iterator-inl.h"

namespace art {

/*
 * MethodUseCount pass implementation start.
 */
bool MethodUseCount::Gate(const PassDataHolder* data) const {
  DCHECK(data != nullptr);
  CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
  DCHECK(c_unit != nullptr);
  // First initialize the data.
  c_unit->mir_graph->InitializeMethodUses();

  // Now check if the pass is to be ignored.
  bool res = ((c_unit->disable_opt & (1 << kPromoteRegs)) == 0);

  return res;
}

bool MethodUseCount::Worker(const PassDataHolder* data) const {
  DCHECK(data != nullptr);
  const PassMEDataHolder* pass_me_data_holder = down_cast<const PassMEDataHolder*>(data);
  CompilationUnit* c_unit = pass_me_data_holder->c_unit;
  DCHECK(c_unit != nullptr);
  BasicBlock* bb = pass_me_data_holder->bb;
  DCHECK(bb != nullptr);
  c_unit->mir_graph->CountUses(bb);
  // No need of repeating, so just return false.
  return false;
}


bool ClearPhiInstructions::Worker(const PassDataHolder* data) const {
  DCHECK(data != nullptr);
  const PassMEDataHolder* pass_me_data_holder = down_cast<const PassMEDataHolder*>(data);
  CompilationUnit* c_unit = pass_me_data_holder->c_unit;
  DCHECK(c_unit != nullptr);
  BasicBlock* bb = pass_me_data_holder->bb;
  DCHECK(bb != nullptr);
  MIR* mir = bb->first_mir_insn;

  while (mir != nullptr) {
    MIR* next = mir->next;

    Instruction::Code opcode = mir->dalvikInsn.opcode;

    if (opcode == static_cast<Instruction::Code> (kMirOpPhi)) {
      bb->RemoveMIR(mir);
    }

    mir = next;
  }

  // We do not care in reporting a change or not in the MIR.
  return false;
}

void CalculatePredecessors::Start(PassDataHolder* data) const {
  DCHECK(data != nullptr);
  CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
  DCHECK(c_unit != nullptr);
  // First get the MIRGraph here to factorize a bit the code.
  MIRGraph *mir_graph = c_unit->mir_graph.get();

  // First clear all predecessors.
  AllNodesIterator first(mir_graph);
  for (BasicBlock* bb = first.Next(); bb != nullptr; bb = first.Next()) {
    bb->predecessors->Reset();
  }

  // Now calculate all predecessors.
  AllNodesIterator second(mir_graph);
  for (BasicBlock* bb = second.Next(); bb != nullptr; bb = second.Next()) {
    // We only care about non hidden blocks.
    if (bb->hidden == true) {
      continue;
    }

    // Create iterator for visiting children.
    ChildBlockIterator child_iter(bb, mir_graph);

    // Now iterate through the children to set the predecessor bits.
    for (BasicBlock* child = child_iter.Next(); child != nullptr; child = child_iter.Next()) {
      child->predecessors->Insert(bb->id);
    }
  }
}

}  // namespace art
