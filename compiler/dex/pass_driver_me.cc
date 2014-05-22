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
#include "pass_driver_me.h"

namespace art {

namespace {  // anonymous namespace

void DoWalkBasicBlocks(PassMEDataHolder* data, const PassME* pass, DataflowIterator* iterator) {
  // Paranoid: Check the iterator before walking the BasicBlocks.
  DCHECK(iterator != nullptr);
  bool change = false;
  for (BasicBlock *bb = iterator->Next(change); bb != 0; bb = iterator->Next(change)) {
    data->bb = bb;
    change = pass->Worker(data);
  }
}

template <typename Iterator>
inline void DoWalkBasicBlocks(PassMEDataHolder* data, const PassME* pass) {
  DCHECK(data != nullptr);
  CompilationUnit* c_unit = data->c_unit;
  DCHECK(c_unit != nullptr);
  Iterator iterator(c_unit->mir_graph.get());
  DoWalkBasicBlocks(data, pass, &iterator);
}
}  // anonymous namespace

/*
 * Create the pass list. These passes are immutable and are shared across the threads.
 *
 * Advantage is that there will be no race conditions here.
 * Disadvantage is the passes can't change their internal states depending on CompilationUnit:
 *   - This is not yet an issue: no current pass would require it.
 */
// The initial list of passes to be used by the PassDriveME.
template<>
const Pass* const PassDriver<PassDriverME>::g_passes[] = {
  GetPassInstance<CacheFieldLoweringInfo>(),
  GetPassInstance<CacheMethodLoweringInfo>(),
  GetPassInstance<CallInlining>(),
  GetPassInstance<CodeLayout>(),
  GetPassInstance<SSATransformation>(),
  GetPassInstance<ConstantPropagation>(),
  GetPassInstance<InitRegLocations>(),
  GetPassInstance<MethodUseCount>(),
  GetPassInstance<NullCheckEliminationAndTypeInference>(),
  GetPassInstance<ClassInitCheckElimination>(),
  GetPassInstance<BBCombine>(),
  GetPassInstance<BBOptimizations>(),
};

// The number of the passes in the initial list of Passes (g_passes).
template<>
uint16_t const PassDriver<PassDriverME>::g_passes_size = arraysize(PassDriver<PassDriverME>::g_passes);

// The default pass list is used by the PassDriverME instance of PassDriver to initialize pass_list_.
template<>
std::vector<const Pass*> PassDriver<PassDriverME>::g_default_pass_list(PassDriver<PassDriverME>::g_passes, PassDriver<PassDriverME>::g_passes + PassDriver<PassDriverME>::g_passes_size);

PassDriverME::PassDriverME(CompilationUnit* cu)
    : PassDriver(), pass_me_data_holder_(), dump_cfg_folder_("/sdcard/") {
  pass_me_data_holder_.bb = nullptr;
  pass_me_data_holder_.c_unit = cu;
}

PassDriverME::~PassDriverME() {
}

void PassDriverME::DispatchPass(const Pass* pass) {
  VLOG(compiler) << "Dispatching " << pass->GetName();
  const PassME* me_pass = down_cast<const PassME*>(pass);

  DataFlowAnalysisMode mode = me_pass->GetTraversal();

  switch (mode) {
    case kPreOrderDFSTraversal:
      DoWalkBasicBlocks<PreOrderDfsIterator>(&pass_me_data_holder_, me_pass);
      break;
    case kRepeatingPreOrderDFSTraversal:
      DoWalkBasicBlocks<RepeatingPreOrderDfsIterator>(&pass_me_data_holder_, me_pass);
      break;
    case kRepeatingPostOrderDFSTraversal:
      DoWalkBasicBlocks<RepeatingPostOrderDfsIterator>(&pass_me_data_holder_, me_pass);
      break;
    case kReversePostOrderDFSTraversal:
      DoWalkBasicBlocks<ReversePostOrderDfsIterator>(&pass_me_data_holder_, me_pass);
      break;
    case kRepeatingReversePostOrderDFSTraversal:
      DoWalkBasicBlocks<RepeatingReversePostOrderDfsIterator>(&pass_me_data_holder_, me_pass);
      break;
    case kPostOrderDOMTraversal:
      DoWalkBasicBlocks<PostOrderDOMIterator>(&pass_me_data_holder_, me_pass);
      break;
    case kAllNodes:
      DoWalkBasicBlocks<AllNodesIterator>(&pass_me_data_holder_, me_pass);
      break;
    case kNoNodes:
      break;
    default:
      LOG(FATAL) << "Iterator mode not handled in dispatcher: " << mode;
      break;
  }
}

bool PassDriverME::RunPass(const Pass* pass, bool time_split) {
  // Paranoid: c_unit and pass cannot be nullptr, and the pass should have a name
  DCHECK(pass != nullptr);
  DCHECK(pass->GetName() != nullptr && pass->GetName()[0] != 0);
  CompilationUnit* c_unit = pass_me_data_holder_.c_unit;
  DCHECK(c_unit != nullptr);

  // Do we perform a time split
  if (time_split) {
    c_unit->NewTimingSplit(pass->GetName());
  }

  // Check the pass gate first.
  bool should_apply_pass = pass->Gate(&pass_me_data_holder_);
  if (should_apply_pass) {
    // Applying the pass: first start, doWork, and end calls.
    ApplyPass(&pass_me_data_holder_, pass);

    // Do we want to log it?
    if ((c_unit->enable_debug&  (1 << kDebugDumpCFG)) != 0) {
      // Do we have a pass folder?
      const PassME* me_pass = (down_cast<const PassME*>(pass));
      const char* passFolder = me_pass->GetDumpCFGFolder();
      DCHECK(passFolder != nullptr);

      if (passFolder[0] != 0) {
        // Create directory prefix.
        std::string prefix = GetDumpCFGFolder();
        prefix += passFolder;
        prefix += "/";

        c_unit->mir_graph->DumpCFG(prefix.c_str(), false);
      }
    }
  }

  // If the pass gate passed, we can declare success.
  return should_apply_pass;
}

const char* PassDriverME::GetDumpCFGFolder() const {
  return dump_cfg_folder_;
}


}  // namespace art
