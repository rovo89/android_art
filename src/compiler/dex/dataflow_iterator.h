/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_DEX_DATAFLOW_ITERATOR_H_
#define ART_SRC_COMPILER_DEX_DATAFLOW_ITERATOR_H_

#include "compiler_ir.h"
#include "mir_graph.h"

namespace art {

  class DataflowIterator {
    public:
      DataflowIterator(MIRGraph* mir_graph, DataFlowAnalysisMode dfa_mode, bool is_iterative);
      ~DataflowIterator(){}

      BasicBlock* Next(bool had_change);
      BasicBlock* Next();

    private:
      // TODO: rework this class.
      MIRGraph* mir_graph_;
      DataFlowAnalysisMode mode_;
      bool is_iterative_;
      bool changed_;
      int start_idx_;
      int end_idx_;
      int idx_;
      bool reverse_;
      GrowableList* block_id_list_;
      GrowableListIterator all_nodes_iterator_;

      BasicBlock* NextBody(bool had_change);

  }; // DataflowIterator
}  // namespace art

#endif // ART_SRC_COMPILER_DEX_DATAFLOW_ITERATOR_H_
