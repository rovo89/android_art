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

#ifndef ART_COMPILER_DEX_DATAFLOW_ITERATOR_H_
#define ART_COMPILER_DEX_DATAFLOW_ITERATOR_H_

#include "compiler_ir.h"
#include "mir_graph.h"

namespace art {

  /*
   * This class supports iterating over lists of basic blocks in various
   * interesting orders.  Note that for efficiency, the visit orders have been pre-computed.
   * The order itself will not change during the iteration.  However, for some uses,
   * auxiliary data associated with the basic blocks may be changed during the iteration,
   * necessitating another pass over the list.  If this behavior is required, use the
   * "Repeating" variant.  For the repeating variant, the caller must tell the iterator
   * whether a change has been made that necessitates another pass.  Note that calling Next(true)
   * does not affect the iteration order or short-circuit the current pass - it simply tells
   * the iterator that once it has finished walking through the block list it should reset and
   * do another full pass through the list.
   */
  class DataflowIterator {
    public:
      virtual ~DataflowIterator() {}

    protected:
      DataflowIterator(MIRGraph* mir_graph, int start_idx, int end_idx)
          : mir_graph_(mir_graph),
            start_idx_(start_idx),
            end_idx_(end_idx),
            block_id_list_(NULL),
            idx_(0),
            changed_(false) {}

      virtual BasicBlock* ForwardSingleNext() ALWAYS_INLINE;
      virtual BasicBlock* ReverseSingleNext() ALWAYS_INLINE;
      virtual BasicBlock* ForwardRepeatNext(bool had_change) ALWAYS_INLINE;
      virtual BasicBlock* ReverseRepeatNext(bool had_change) ALWAYS_INLINE;

      MIRGraph* const mir_graph_;
      const int start_idx_;
      const int end_idx_;
      GrowableArray<int>* block_id_list_;
      int idx_;
      bool changed_;
  };  // DataflowIterator

  class PreOrderDfsIterator : public DataflowIterator {
    public:
      explicit PreOrderDfsIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, 0, mir_graph->GetNumReachableBlocks()) {
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDfsOrder();
      }

      BasicBlock* Next() {
        return ForwardSingleNext();
      }
  };

  class RepeatingPreOrderDfsIterator : public DataflowIterator {
    public:
      explicit RepeatingPreOrderDfsIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, 0, mir_graph->GetNumReachableBlocks()) {
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDfsOrder();
      }

      BasicBlock* Next(bool had_change) {
        return ForwardRepeatNext(had_change);
      }
  };

  class RepeatingPostOrderDfsIterator : public DataflowIterator {
    public:
      explicit RepeatingPostOrderDfsIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, 0, mir_graph->GetNumReachableBlocks()) {
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDfsPostOrder();
      }

      BasicBlock* Next(bool had_change) {
        return ForwardRepeatNext(had_change);
      }
  };

  class ReversePostOrderDfsIterator : public DataflowIterator {
    public:
      explicit ReversePostOrderDfsIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, mir_graph->GetNumReachableBlocks() -1, 0) {
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDfsPostOrder();
      }

      BasicBlock* Next() {
        return ReverseSingleNext();
      }
  };

  class RepeatingReversePostOrderDfsIterator : public DataflowIterator {
    public:
      explicit RepeatingReversePostOrderDfsIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, mir_graph->GetNumReachableBlocks() -1, 0) {
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDfsPostOrder();
      }

      BasicBlock* Next(bool had_change) {
        return ReverseRepeatNext(had_change);
      }
  };

  class PostOrderDOMIterator : public DataflowIterator {
    public:
      explicit PostOrderDOMIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, 0, mir_graph->GetNumReachableBlocks()) {
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDomPostOrder();
      }

      BasicBlock* Next() {
        return ForwardSingleNext();
      }
  };

  class AllNodesIterator : public DataflowIterator {
    public:
      explicit AllNodesIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, 0, 0) {
        all_nodes_iterator_ = new
            (mir_graph->GetArena()) GrowableArray<BasicBlock*>::Iterator(mir_graph->GetBlockList());
      }

      void Reset() {
        all_nodes_iterator_->Reset();
      }

      BasicBlock* Next() ALWAYS_INLINE;

    private:
      GrowableArray<BasicBlock*>::Iterator* all_nodes_iterator_;
  };

}  // namespace art

#endif  // ART_COMPILER_DEX_DATAFLOW_ITERATOR_H_
