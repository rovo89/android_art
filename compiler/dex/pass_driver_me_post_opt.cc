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

#include "base/macros.h"
#include "post_opt_passes.h"
#include "compiler_internals.h"
#include "pass_driver_me_post_opt.h"

namespace art {

/*
 * Create the pass list. These passes are immutable and are shared across the threads.
 *
 * Advantage is that there will be no race conditions here.
 * Disadvantage is the passes can't change their internal states depending on CompilationUnit:
 *   - This is not yet an issue: no current pass would require it.
 */
// The initial list of passes to be used by the PassDriveMEPostOpt.
template<>
const Pass* const PassDriver<PassDriverMEPostOpt>::g_passes[] = {
  GetPassInstance<InitializeData>(),
  GetPassInstance<ClearPhiInstructions>(),
  GetPassInstance<CalculatePredecessors>(),
  GetPassInstance<DFSOrders>(),
  GetPassInstance<BuildDomination>(),
  GetPassInstance<TopologicalSortOrders>(),
  GetPassInstance<DefBlockMatrix>(),
  GetPassInstance<CreatePhiNodes>(),
  GetPassInstance<ClearVisitedFlag>(),
  GetPassInstance<SSAConversion>(),
  GetPassInstance<PhiNodeOperands>(),
  GetPassInstance<ConstantPropagation>(),
  GetPassInstance<PerformInitRegLocations>(),
  GetPassInstance<MethodUseCount>(),
  GetPassInstance<FreeData>(),
};

// The number of the passes in the initial list of Passes (g_passes).
template<>
uint16_t const PassDriver<PassDriverMEPostOpt>::g_passes_size =
    arraysize(PassDriver<PassDriverMEPostOpt>::g_passes);

// The default pass list is used by the PassDriverME instance of PassDriver
// to initialize pass_list_.
template<>
std::vector<const Pass*> PassDriver<PassDriverMEPostOpt>::g_default_pass_list(
    PassDriver<PassDriverMEPostOpt>::g_passes,
    PassDriver<PassDriverMEPostOpt>::g_passes +
    PassDriver<PassDriverMEPostOpt>::g_passes_size);

// By default, do not have a dump pass list.
template<>
std::string PassDriver<PassDriverMEPostOpt>::dump_pass_list_ = std::string();

// By default, do not have a print pass list.
template<>
std::string PassDriver<PassDriverMEPostOpt>::print_pass_list_ = std::string();

// By default, we do not print the pass' information.
template<>
bool PassDriver<PassDriverMEPostOpt>::default_print_passes_ = false;

}  // namespace art
