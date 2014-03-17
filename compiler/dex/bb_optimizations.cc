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

#include "bb_optimizations.h"
#include "dataflow_iterator.h"
#include "dataflow_iterator-inl.h"

namespace art {

/*
 * Code Layout pass implementation start.
 */
bool CodeLayout::WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const {
  cUnit->mir_graph->LayoutBlocks(bb);
  // No need of repeating, so just return false.
  return false;
}

/*
 * SSATransformation pass implementation start.
 */
bool SSATransformation::WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const {
  cUnit->mir_graph->InsertPhiNodeOperands(bb);
  // No need of repeating, so just return false.
  return false;
}

void SSATransformation::End(CompilationUnit* cUnit) const {
  // Verify the dataflow information after the pass.
  if (cUnit->enable_debug & (1 << kDebugVerifyDataflow)) {
    cUnit->mir_graph->VerifyDataflow();
  }
}

/*
 * ConstantPropagation pass implementation start
 */
bool ConstantPropagation::WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const {
  cUnit->mir_graph->DoConstantPropagation(bb);
  // No need of repeating, so just return false.
  return false;
}

/*
 * MethodUseCount pass implementation start.
 */
bool MethodUseCount::Gate(const CompilationUnit* cUnit) const {
  // First initialize the data.
  cUnit->mir_graph->InitializeMethodUses();

  // Now check if the pass is to be ignored.
  bool res = ((cUnit->disable_opt & (1 << kPromoteRegs)) == 0);

  return res;
}

bool MethodUseCount::WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const {
  cUnit->mir_graph->CountUses(bb);
  // No need of repeating, so just return false.
  return false;
}

/*
 * BasicBlock Combine pass implementation start.
 */
bool BBCombine::WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const {
  cUnit->mir_graph->CombineBlocks(bb);

  // No need of repeating, so just return false.
  return false;
}

/*
 * BasicBlock Optimization pass implementation start.
 */
void BBOptimizations::Start(CompilationUnit* cUnit) const {
  /*
   * This pass has a different ordering depEnding on the suppress exception,
   * so do the pass here for now:
   *   - Later, the Start should just change the ordering and we can move the extended
   *     creation into the pass driver's main job with a new iterator
   */
  cUnit->mir_graph->BasicBlockOptimization();
}

}  // namespace art
