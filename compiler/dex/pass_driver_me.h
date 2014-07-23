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

#ifndef ART_COMPILER_DEX_PASS_DRIVER_ME_H_
#define ART_COMPILER_DEX_PASS_DRIVER_ME_H_

#include "bb_optimizations.h"
#include "dataflow_iterator.h"
#include "dataflow_iterator-inl.h"
#include "pass_driver.h"
#include "pass_me.h"

namespace art {

template <typename PassDriverType>
class PassDriverME: public PassDriver<PassDriverType> {
 public:
  explicit PassDriverME(CompilationUnit* cu)
      : pass_me_data_holder_(), dump_cfg_folder_("/sdcard/") {
        pass_me_data_holder_.bb = nullptr;
        pass_me_data_holder_.c_unit = cu;
  }

  ~PassDriverME() {
  }

  void DispatchPass(const Pass* pass) {
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
      case kTopologicalSortTraversal:
        DoWalkBasicBlocks<TopologicalSortIterator>(&pass_me_data_holder_, me_pass);
        break;
      case kRepeatingTopologicalSortTraversal:
        DoWalkBasicBlocks<RepeatingTopologicalSortIterator>(&pass_me_data_holder_, me_pass);
        break;
      case kLoopRepeatingTopologicalSortTraversal:
        DoWalkBasicBlocks<LoopRepeatingTopologicalSortIterator>(&pass_me_data_holder_, me_pass);
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

  bool RunPass(const Pass* pass, bool time_split) {
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
      bool old_print_pass = c_unit->print_pass;

      c_unit->print_pass = PassDriver<PassDriverType>::default_print_passes_;

      const char* print_pass_list = PassDriver<PassDriverType>::print_pass_list_.c_str();

      if (print_pass_list != nullptr && strstr(print_pass_list, pass->GetName()) != nullptr) {
        c_unit->print_pass = true;
      }

      // Applying the pass: first start, doWork, and end calls.
      this->ApplyPass(&pass_me_data_holder_, pass);

      bool should_dump = ((c_unit->enable_debug & (1 << kDebugDumpCFG)) != 0);

      const char* dump_pass_list = PassDriver<PassDriverType>::dump_pass_list_.c_str();

      if (dump_pass_list != nullptr) {
        bool found = strstr(dump_pass_list, pass->GetName());
        should_dump = (should_dump || found);
      }

      if (should_dump) {
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

      c_unit->print_pass = old_print_pass;
    }

    // If the pass gate passed, we can declare success.
    return should_apply_pass;
  }

  const char* GetDumpCFGFolder() const {
    return dump_cfg_folder_;
  }

 protected:
  /** @brief The data holder that contains data needed for the PassDriverME. */
  PassMEDataHolder pass_me_data_holder_;

  /** @brief Dump CFG base folder: where is the base folder for dumping CFGs. */
  const char* dump_cfg_folder_;

  static void DoWalkBasicBlocks(PassMEDataHolder* data, const PassME* pass,
                                DataflowIterator* iterator) {
    // Paranoid: Check the iterator before walking the BasicBlocks.
    DCHECK(iterator != nullptr);
    bool change = false;
    for (BasicBlock* bb = iterator->Next(change); bb != nullptr; bb = iterator->Next(change)) {
      data->bb = bb;
      change = pass->Worker(data);
    }
  }

  template <typename Iterator>
  inline static void DoWalkBasicBlocks(PassMEDataHolder* data, const PassME* pass) {
      DCHECK(data != nullptr);
      CompilationUnit* c_unit = data->c_unit;
      DCHECK(c_unit != nullptr);
      Iterator iterator(c_unit->mir_graph.get());
      DoWalkBasicBlocks(data, pass, &iterator);
    }
};
}  // namespace art
#endif  // ART_COMPILER_DEX_PASS_DRIVER_ME_H_

