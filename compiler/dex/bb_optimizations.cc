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
bool CodeLayout::Worker(const PassDataHolder* data) const {
  DCHECK(data != nullptr);
  const PassMEDataHolder* pass_me_data_holder = down_cast<const PassMEDataHolder*>(data);
  CompilationUnit* c_unit = pass_me_data_holder->c_unit;
  DCHECK(c_unit != nullptr);
  BasicBlock* bb = pass_me_data_holder->bb;
  DCHECK(bb != nullptr);
  c_unit->mir_graph->LayoutBlocks(bb);
  // No need of repeating, so just return false.
  return false;
}

/*
 * BasicBlock Combine pass implementation start.
 */
bool BBCombine::Worker(const PassDataHolder* data) const {
  DCHECK(data != nullptr);
  const PassMEDataHolder* pass_me_data_holder = down_cast<const PassMEDataHolder*>(data);
  CompilationUnit* c_unit = pass_me_data_holder->c_unit;
  DCHECK(c_unit != nullptr);
  BasicBlock* bb = pass_me_data_holder->bb;
  DCHECK(bb != nullptr);
  c_unit->mir_graph->CombineBlocks(bb);

  // No need of repeating, so just return false.
  return false;
}

/*
 * BasicBlock Optimization pass implementation start.
 */
void BBOptimizations::Start(PassDataHolder* data) const {
  DCHECK(data != nullptr);
  CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
  DCHECK(c_unit != nullptr);
  /*
   * This pass has a different ordering depEnding on the suppress exception,
   * so do the pass here for now:
   *   - Later, the Start should just change the ordering and we can move the extended
   *     creation into the pass driver's main job with a new iterator
   */
  c_unit->mir_graph->BasicBlockOptimization();
}

}  // namespace art
