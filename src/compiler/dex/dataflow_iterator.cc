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

#include "dataflow_iterator.h"

namespace art {

  DataflowIterator::DataflowIterator(MIRGraph* mir_graph, DataFlowAnalysisMode dfa_mode, bool is_iterative)
      : mir_graph_(mir_graph),
        mode_(dfa_mode),
        is_iterative_(is_iterative),
        changed_(false) {
    switch(mode_) {
      case kAllNodes:
        GrowableListIteratorInit(mir_graph_->GetBlockList(), &all_nodes_iterator_);
        break;

      case kReachableNodes:
      case kPreOrderDFSTraversal:
        start_idx_ = 0;
        end_idx_ = mir_graph_->GetNumReachableBlocks();
        idx_ = start_idx_;
        block_id_list_ = mir_graph_->GetDfsOrder();
        reverse_ = false;
        break;

      case kPostOrderDFSTraversal:
        start_idx_ = mir_graph_->GetNumReachableBlocks() - 1;
        end_idx_ = 0;
        idx_ = start_idx_;
        block_id_list_ = mir_graph_->GetDfsOrder();
        reverse_ = true;
        break;

      case kPostOrderDOMTraversal:
        start_idx_ = 0;
        end_idx_ = mir_graph_->GetNumReachableBlocks();
        idx_ = start_idx_;
        block_id_list_ = mir_graph_->GetDomPostOrder();
        reverse_ = false;
        break;

      case kReversePostOrderTraversal:
        start_idx_ = mir_graph_->GetNumReachableBlocks() - 1;
        end_idx_ = 0;
        idx_ = start_idx_;
        block_id_list_ = mir_graph_->GetDfsPostOrder();
        reverse_ = true;
        break;
      default:
        LOG(FATAL) << "Unknown traversal mode: " << dfa_mode;
    }
  }

  BasicBlock* DataflowIterator::NextBody(bool had_change)
  {
    changed_ |= had_change;
    BasicBlock* res = NULL;
    if (mode_ == kAllNodes) {
      bool keep_looking = true;
      while (keep_looking) {
        res = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&all_nodes_iterator_));
        if (is_iterative_ && changed_ && (res == NULL)) {
          GrowableListIteratorInit(mir_graph_->GetBlockList(), &all_nodes_iterator_);
          changed_ = false;
        } else if ((res == NULL) || (!res->hidden)) {
          keep_looking = false;
        }
      }
    } else if (reverse_) {
      if (is_iterative_ && changed_ && (idx_ < 0)) {
        idx_ = start_idx_;
        changed_ = false;
      }
      if (idx_ >= 0) {
        int bb_id = block_id_list_->elem_list[idx_--];
        res = mir_graph_->GetBasicBlock(bb_id);
      }
    } else {
      if (is_iterative_ && changed_ && (idx_ >= end_idx_)) {
        idx_ = start_idx_;
        changed_ = false;
      }
      if (idx_ < end_idx_) {
        int bb_id = block_id_list_->elem_list[idx_++];
        res = mir_graph_->GetBasicBlock(bb_id);
      }
    }
    return res;
  }

  BasicBlock* DataflowIterator::Next(bool had_change)
  {
    DCHECK(is_iterative_);
    return NextBody(had_change);
  }

  BasicBlock* DataflowIterator::Next()
  {
    DCHECK(!is_iterative_);
    return NextBody(false);
  }

}  // namespace art
