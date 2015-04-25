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
bool CodeLayout::Worker(PassDataHolder* data) const {
  DCHECK(data != nullptr);
  PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
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
bool BBCombine::Worker(PassDataHolder* data) const {
  DCHECK(data != nullptr);
  PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
  CompilationUnit* c_unit = pass_me_data_holder->c_unit;
  DCHECK(c_unit != nullptr);
  BasicBlock* bb = pass_me_data_holder->bb;
  DCHECK(bb != nullptr);
  c_unit->mir_graph->CombineBlocks(bb);

  // No need of repeating, so just return false.
  return false;
}

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

bool MethodUseCount::Worker(PassDataHolder* data) const {
  DCHECK(data != nullptr);
  PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
  CompilationUnit* c_unit = pass_me_data_holder->c_unit;
  DCHECK(c_unit != nullptr);
  BasicBlock* bb = pass_me_data_holder->bb;
  DCHECK(bb != nullptr);
  c_unit->mir_graph->CountUses(bb);
  // No need of repeating, so just return false.
  return false;
}

}  // namespace art
