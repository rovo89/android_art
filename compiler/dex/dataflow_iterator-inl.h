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

#ifndef ART_COMPILER_DEX_DATAFLOW_ITERATOR_INL_H_
#define ART_COMPILER_DEX_DATAFLOW_ITERATOR_INL_H_

#include "dataflow_iterator.h"

namespace art {

// Single forward pass over the nodes.
inline BasicBlock* DataflowIterator::ForwardSingleNext() {
  BasicBlock* res = NULL;

  // Are we not yet at the end?
  if (idx_ < end_idx_) {
    // Get the next index.
    BasicBlockId bb_id = block_id_list_->Get(idx_);
    res = mir_graph_->GetBasicBlock(bb_id);
    idx_++;
  }

  return res;
}

// Repeat full forward passes over all nodes until no change occurs during a complete pass.
inline BasicBlock* DataflowIterator::ForwardRepeatNext() {
  BasicBlock* res = NULL;

  // Are we at the end and have we changed something?
  if ((idx_ >= end_idx_) && changed_ == true) {
    // Reset the index.
    idx_ = start_idx_;
    repeats_++;
    changed_ = false;
  }

  // Are we not yet at the end?
  if (idx_ < end_idx_) {
    // Get the BasicBlockId.
    BasicBlockId bb_id = block_id_list_->Get(idx_);
    res = mir_graph_->GetBasicBlock(bb_id);
    idx_++;
  }

  return res;
}

// Single reverse pass over the nodes.
inline BasicBlock* DataflowIterator::ReverseSingleNext() {
  BasicBlock* res = NULL;

  // Are we not yet at the end?
  if (idx_ >= 0) {
    // Get the BasicBlockId.
    BasicBlockId bb_id = block_id_list_->Get(idx_);
    res = mir_graph_->GetBasicBlock(bb_id);
    idx_--;
  }

  return res;
}

// Repeat full backwards passes over all nodes until no change occurs during a complete pass.
inline BasicBlock* DataflowIterator::ReverseRepeatNext() {
  BasicBlock* res = NULL;

  // Are we done and we changed something during the last iteration?
  if ((idx_ < 0) && changed_) {
    // Reset the index.
    idx_ = start_idx_;
    repeats_++;
    changed_ = false;
  }

  // Are we not yet done?
  if (idx_ >= 0) {
    // Get the BasicBlockId.
    BasicBlockId bb_id = block_id_list_->Get(idx_);
    res = mir_graph_->GetBasicBlock(bb_id);
    idx_--;
  }

  return res;
}

// AllNodes uses the existing GrowableArray iterator, and should be considered unordered.
inline BasicBlock* AllNodesIterator::Next(bool had_change) {
  BasicBlock* res = NULL;

  // Suppose we want to keep looking.
  bool keep_looking = true;

  // Find the next BasicBlock.
  while (keep_looking == true) {
    // Get next BasicBlock.
    res = all_nodes_iterator_.Next();

    // Are we done or is the BasicBlock not hidden?
    if ((res == NULL) || (res->hidden == false)) {
      keep_looking = false;
    }
  }

  // Update changed: if had_changed is true, we remember it for the whole iteration.
  changed_ |= had_change;

  return res;
}

}  // namespace art

#endif  // ART_COMPILER_DEX_DATAFLOW_ITERATOR_INL_H_
