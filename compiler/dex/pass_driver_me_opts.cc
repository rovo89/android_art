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
#include "bb_optimizations.h"
#include "compiler_internals.h"
#include "dataflow_iterator.h"
#include "dataflow_iterator-inl.h"
#include "pass_driver_me_opts.h"

namespace art {

const Pass* GetMorePassInstance() {
    static const DummyPass pass;
    return &pass;
}
/*
 * Create the pass list. These passes are immutable and are shared across the threads.
 *
 * Advantage is that there will be no race conditions here.
 * Disadvantage is the passes can't change their internal states depending on CompilationUnit:
 *   - This is not yet an issue: no current pass would require it.
 */
// The initial list of passes to be used by the PassDriveMEOpts.
template<>
const Pass* const PassDriver<PassDriverMEOpts>::g_passes[] = {
  GetPassInstance<CacheFieldLoweringInfo>(),
  GetPassInstance<CacheMethodLoweringInfo>(),
  GetPassInstance<SpecialMethodInliner>(),
  GetPassInstance<CodeLayout>(),
  GetPassInstance<NullCheckEliminationAndTypeInference>(),
  GetPassInstance<ClassInitCheckElimination>(),
  GetPassInstance<GlobalValueNumberingPass>(),
  GetPassInstance<BBCombine>(),
  GetPassInstance<BBOptimizations>(),
  GetMorePassInstance(),
};

// The number of the passes in the initial list of Passes (g_passes).
template<>
uint16_t const PassDriver<PassDriverMEOpts>::g_passes_size =
    arraysize(PassDriver<PassDriverMEOpts>::g_passes);

// The default pass list is used by the PassDriverME instance of PassDriver
// to initialize pass_list_.
template<>
std::vector<const Pass*> PassDriver<PassDriverMEOpts>::g_default_pass_list(
    PassDriver<PassDriverMEOpts>::g_passes,
    PassDriver<PassDriverMEOpts>::g_passes +
    PassDriver<PassDriverMEOpts>::g_passes_size);

// By default, do not have a dump pass list.
template<>
std::string PassDriver<PassDriverMEOpts>::dump_pass_list_ = std::string();

// By default, do not have a print pass list.
template<>
std::string PassDriver<PassDriverMEOpts>::print_pass_list_ = std::string();

// By default, we do not print the pass' information.
template<>
bool PassDriver<PassDriverMEOpts>::default_print_passes_ = false;

void PassDriverMEOpts::ApplyPass(PassDataHolder* data, const Pass* pass) {
  // First call the base class' version.
  PassDriver::ApplyPass(data, pass);

  const PassME* pass_me = down_cast<const PassME*> (pass);
  DCHECK(pass_me != nullptr);

  PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);

  // Now we care about flags.
  if ((pass_me->GetFlag(kOptimizationBasicBlockChange) == true) ||
      (pass_me->GetFlag(kOptimizationDefUsesChange) == true)) {
    CompilationUnit* c_unit = pass_me_data_holder->c_unit;
    c_unit->mir_graph.get()->CalculateBasicBlockInformation();
  }
}

}  // namespace art
