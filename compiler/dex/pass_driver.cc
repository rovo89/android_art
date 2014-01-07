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

#include <dlfcn.h>

#include "bb_optimizations.h"
#include "compiler_internals.h"
#include "dataflow_iterator.h"
#include "dataflow_iterator-inl.h"
#include "pass.h"
#include "pass_driver.h"

namespace art {

PassDriver::PassDriver(CompilationUnit* const cu, bool create_default_passes) : cu_(cu) {
  dump_cfg_folder_ = "/sdcard/";

  // If need be, create the default passes.
  if (create_default_passes == true) {
    CreatePasses();
  }
}

PassDriver::~PassDriver() {
  // Clear the map: done to remove any chance of having a pointer after freeing below
  pass_map_.clear();
}

void PassDriver::InsertPass(Pass* new_pass, bool warn_override) {
  assert(new_pass != 0);

  // Get name here to not do it all over the method.
  const std::string& name = new_pass->GetName();

  // Do we want to warn the user about squashing a pass?
  if (warn_override == false) {
    SafeMap<std::string, Pass* >::iterator it = pass_map_.find(name);

    if (it != pass_map_.end()) {
      LOG(INFO) << "Pass name " << name << " already used, overwriting pass";
    }
  }

  // Now add to map and list.
  pass_map_.Put(name, new_pass);
  pass_list_.push_back(new_pass);
}

void PassDriver::CreatePasses() {
  /*
   * Create the pass list:
   *   - These passes are immutable and are shared across the threads:
   *    - This is achieved via:
   *     - The UniquePtr used here.
   *     - DISALLOW_COPY_AND_ASSIGN in the base Pass class.
   *
   * Advantage is that there will be no race conditions here.
   * Disadvantage is the passes can't change their internal states depending on CompilationUnit:
   *   - This is not yet an issue: no current pass would require it.
   */
  static UniquePtr<Pass> *passes[] = {
      new UniquePtr<Pass>(new CodeLayout()),
      new UniquePtr<Pass>(new SSATransformation()),
      new UniquePtr<Pass>(new ConstantPropagation()),
      new UniquePtr<Pass>(new InitRegLocations()),
      new UniquePtr<Pass>(new MethodUseCount()),
      new UniquePtr<Pass>(new NullCheckEliminationAndTypeInferenceInit()),
      new UniquePtr<Pass>(new NullCheckEliminationAndTypeInference()),
      new UniquePtr<Pass>(new BBCombine()),
      new UniquePtr<Pass>(new BBOptimizations()),
  };

  // Get number of elements in the array.
  unsigned int nbr = (sizeof(passes) / sizeof(passes[0]));

  // Insert each pass into the map and into the list via the InsertPass method:
  //   - Map is used for the lookup
  //   - List is used for the pass walk
  for (unsigned int i = 0; i < nbr; i++) {
    InsertPass(passes[i]->get());
  }
}

void PassDriver::HandlePassFlag(CompilationUnit* c_unit, Pass* pass) {
  // Unused parameters for the moment.
  UNUSED(c_unit);
  UNUSED(pass);
}

void PassDriver::DispatchPass(CompilationUnit* c_unit, Pass* curPass) {
  DataflowIterator* iterator = 0;

  LOG(DEBUG) << "Dispatching " << curPass->GetName();

  MIRGraph* mir_graph = c_unit->mir_graph.get();
  ArenaAllocator *arena = &(c_unit->arena);

  // Let us start by getting the right iterator.
  DataFlowAnalysisMode mode = curPass->GetTraversal();

  switch (mode) {
    case kPreOrderDFSTraversal:
      iterator = new (arena) PreOrderDfsIterator(mir_graph);
      break;
    case kRepeatingPreOrderDFSTraversal:
      iterator = new (arena) RepeatingPreOrderDfsIterator(mir_graph);
      break;
    case kRepeatingPostOrderDFSTraversal:
      iterator = new (arena) RepeatingPostOrderDfsIterator(mir_graph);
      break;
    case kReversePostOrderDFSTraversal:
      iterator = new (arena) ReversePostOrderDfsIterator(mir_graph);
      break;
    case kRepeatingReversePostOrderDFSTraversal:
      iterator = new (arena) RepeatingReversePostOrderDfsIterator(mir_graph);
      break;
    case kPostOrderDOMTraversal:
      iterator = new (arena) PostOrderDOMIterator(mir_graph);
      break;
    case kAllNodes:
      iterator = new (arena) AllNodesIterator(mir_graph);
      break;
    default:
      LOG(DEBUG) << "Iterator mode not handled in dispatcher: " << mode;
      return;
  }

  // Paranoid: Check the iterator before walking the BasicBlocks.
  assert(iterator != 0);

  bool change = false;
  for (BasicBlock *bb = iterator->Next(change); bb != 0; bb = iterator->Next(change)) {
    change = curPass->WalkBasicBlocks(c_unit, bb);
  }
}

void PassDriver::ApplyPass(CompilationUnit* c_unit, Pass* curPass) {
  curPass->Start(c_unit);
  DispatchPass(c_unit, curPass);
  curPass->End(c_unit);
}

bool PassDriver::RunPass(CompilationUnit* c_unit, Pass* curPass, bool time_split) {
  // Paranoid: c_unit or curPass cannot be 0, and the pass should have a name.
  if (c_unit == 0 || curPass == 0 || (strcmp(curPass->GetName(), "") == 0)) {
    return false;
  }

  // Do we perform a time split
  if (time_split == true) {
    std::string name = "MIROpt:";
    name += curPass->GetName();
    c_unit->NewTimingSplit(name.c_str());
  }

  // Check the pass gate first.
  bool shouldApplyPass = curPass->Gate(c_unit);

  if (shouldApplyPass == true) {
    // Applying the pass: first start, doWork, and end calls.
    ApplyPass(c_unit, curPass);

    // Clean up if need be.
    HandlePassFlag(c_unit, curPass);

    // Do we want to log it?
    if ((c_unit->enable_debug&  (1 << kDebugDumpCFG)) != 0) {
      // Do we have a pass folder?
      const std::string& passFolder = curPass->GetDumpCFGFolder();

      if (passFolder != "") {
        // Create directory prefix.
        std::string prefix = GetDumpCFGFolder();
        prefix += passFolder;
        prefix += "/";

        c_unit->mir_graph->DumpCFG(prefix.c_str(), false);
      }
    }
  }

  // If the pass gate passed, we can declare success.
  return shouldApplyPass;
}

bool PassDriver::RunPass(CompilationUnit* c_unit, const std::string& pass_name) {
  // Paranoid: c_unit cannot be 0 and we need a pass name.
  if (c_unit == 0 || pass_name == "") {
    return false;
  }

  Pass* curPass = GetPass(pass_name);

  if (curPass != 0) {
    return RunPass(c_unit, curPass);
  }

  // Return false, we did not find the pass.
  return false;
}

void PassDriver::Launch() {
  for (std::list<Pass* >::iterator it = pass_list_.begin(); it != pass_list_.end(); it++) {
    Pass* curPass = *it;
    RunPass(cu_, curPass, true);
  }
}

void PassDriver::PrintPassNames() const {
  LOG(INFO) << "Loop Passes are:";

  for (std::list<Pass* >::const_iterator it = pass_list_.begin(); it != pass_list_.end(); it++) {
    const Pass* curPass = *it;
    LOG(INFO) << "\t-" << curPass->GetName();
  }
}

Pass* PassDriver::GetPass(const std::string& name) const {
  SafeMap<std::string, Pass*>::const_iterator it = pass_map_.find(name);

  if (it != pass_map_.end()) {
    return it->second;
  }

  return 0;
}

}  // namespace art
