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

#include "pass_driver_me_post_opt.h"

#include "base/macros.h"
#include "post_opt_passes.h"
#include "pass_manager.h"

namespace art {

void PassDriverMEPostOpt::SetupPasses(PassManager* pass_manager) {
  /*
   * Create the pass list. These passes are immutable and are shared across the threads.
   *
   * Advantage is that there will be no race conditions here.
   * Disadvantage is the passes can't change their internal states depending on CompilationUnit:
   *   - This is not yet an issue: no current pass would require it.
   */
  // The initial list of passes to be used by the PassDriveMEPostOpt.
  pass_manager->AddPass(new DFSOrders);
  pass_manager->AddPass(new BuildDomination);
  pass_manager->AddPass(new TopologicalSortOrders);
  pass_manager->AddPass(new InitializeSSATransformation);
  pass_manager->AddPass(new ClearPhiInstructions);
  pass_manager->AddPass(new DefBlockMatrix);
  pass_manager->AddPass(new FindPhiNodeBlocksPass);
  pass_manager->AddPass(new SSAConversion);
  pass_manager->AddPass(new PhiNodeOperands);
  pass_manager->AddPass(new PerformInitRegLocations);
  pass_manager->AddPass(new TypeInferencePass);
  pass_manager->AddPass(new FinishSSATransformation);
}

}  // namespace art
