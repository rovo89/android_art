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

#include "pass_driver_me_opts.h"

#include "base/logging.h"
#include "base/macros.h"
#include "bb_optimizations.h"
#include "dataflow_iterator.h"
#include "dataflow_iterator-inl.h"
#include "pass_driver_me_opts.h"
#include "pass_manager.h"
#include "post_opt_passes.h"

namespace art {

void PassDriverMEOpts::SetupPasses(PassManager* pass_manager) {
  /*
   * Create the pass list. These passes are immutable and are shared across the threads.
   *
   * Advantage is that there will be no race conditions here.
   * Disadvantage is the passes can't change their internal states depending on CompilationUnit:
   *   - This is not yet an issue: no current pass would require it.
   */
  pass_manager->AddPass(new StringChange);
  pass_manager->AddPass(new CacheFieldLoweringInfo);
  pass_manager->AddPass(new CacheMethodLoweringInfo);
  pass_manager->AddPass(new CalculatePredecessors);
  pass_manager->AddPass(new DFSOrders);
  pass_manager->AddPass(new ClassInitCheckElimination);
  pass_manager->AddPass(new SpecialMethodInliner);
  pass_manager->AddPass(new NullCheckElimination);
  pass_manager->AddPass(new BBCombine);
  pass_manager->AddPass(new CodeLayout);
  pass_manager->AddPass(new GlobalValueNumberingPass);
  pass_manager->AddPass(new DeadCodeEliminationPass);
  pass_manager->AddPass(new GlobalValueNumberingCleanupPass);
  pass_manager->AddPass(new ConstantPropagation);
  pass_manager->AddPass(new MethodUseCount);
  pass_manager->AddPass(new BBOptimizations);
  pass_manager->AddPass(new SuspendCheckElimination);
}

void PassDriverMEOpts::ApplyPass(PassDataHolder* data, const Pass* pass) {
  const PassME* const pass_me = down_cast<const PassME*>(pass);
  DCHECK(pass_me != nullptr);
  PassMEDataHolder* const pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
  // Set to dirty.
  pass_me_data_holder->dirty = true;
  // First call the base class' version.
  PassDriver::ApplyPass(data, pass);
  // Now we care about flags.
  if ((pass_me->GetFlag(kOptimizationBasicBlockChange) == true) ||
      (pass_me->GetFlag(kOptimizationDefUsesChange) == true)) {
    // Is it dirty at least?
    if (pass_me_data_holder->dirty == true) {
      CompilationUnit* c_unit = pass_me_data_holder->c_unit;
      c_unit->mir_graph.get()->CalculateBasicBlockInformation(post_opt_pass_manager_);
    }
  }
}

}  // namespace art
