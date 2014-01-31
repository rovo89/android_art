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
  /**
   * @class DataflowIterator
   * @brief The main iterator class, all other iterators derive of this one to define an iteration order.
   */
  class DataflowIterator {
    public:
      virtual ~DataflowIterator() {}

      /**
       * @brief How many times have we repeated the iterator across the BasicBlocks?
       * @return the number of iteration repetitions.
       */
      int32_t GetRepeatCount() { return repeats_; }

      /**
       * @brief Has the user of the iterator reported a change yet?
       * @details Does not mean there was or not a change, it is only whether the user passed a true to the Next function call.
       * @return whether the user of the iterator reported a change yet.
       */
      int32_t GetChanged() { return changed_; }

      /**
       * @brief Get the next BasicBlock depending on iteration order.
       * @param had_change did the user of the iteration change the previous BasicBlock.
       * @return the next BasicBlock following the iteration order, 0 if finished.
       */
      virtual BasicBlock* Next(bool had_change = false) = 0;

    protected:
      /**
       * @param mir_graph the MIRGraph we are interested in.
       * @param start_idx the first index we want to iterate across.
       * @param end_idx the last index we want to iterate (not included).
       */
      DataflowIterator(MIRGraph* mir_graph, int32_t start_idx, int32_t end_idx)
          : mir_graph_(mir_graph),
            start_idx_(start_idx),
            end_idx_(end_idx),
            block_id_list_(NULL),
            idx_(0),
            repeats_(0),
            changed_(false) {}

      /**
       * @brief Get the next BasicBlock iterating forward.
       * @return the next BasicBlock iterating forward.
       */
      virtual BasicBlock* ForwardSingleNext() ALWAYS_INLINE;

      /**
       * @brief Get the next BasicBlock iterating backward.
       * @return the next BasicBlock iterating backward.
       */
      virtual BasicBlock* ReverseSingleNext() ALWAYS_INLINE;

      /**
       * @brief Get the next BasicBlock iterating forward, restart if a BasicBlock was reported changed during the last iteration.
       * @return the next BasicBlock iterating forward, with chance of repeating the iteration.
       */
      virtual BasicBlock* ForwardRepeatNext() ALWAYS_INLINE;

      /**
       * @brief Get the next BasicBlock iterating backward, restart if a BasicBlock was reported changed during the last iteration.
       * @return the next BasicBlock iterating backward, with chance of repeating the iteration.
       */
      virtual BasicBlock* ReverseRepeatNext() ALWAYS_INLINE;

      MIRGraph* const mir_graph_;                       /**< @brief the MIRGraph */
      const int32_t start_idx_;                         /**< @brief the start index for the iteration */
      const int32_t end_idx_;                           /**< @brief the last index for the iteration */
      GrowableArray<BasicBlockId>* block_id_list_;      /**< @brief the list of BasicBlocks we want to iterate on */
      int32_t idx_;                                     /**< @brief Current index for the iterator */
      int32_t repeats_;                                 /**< @brief Number of repeats over the iteration */
      bool changed_;                                    /**< @brief Has something changed during the current iteration? */
  };  // DataflowIterator

  /**
   * @class PreOrderDfsIterator
   * @brief Used to perform a Pre-order Depth-First-Search Iteration of a MIRGraph.
   */
  class PreOrderDfsIterator : public DataflowIterator {
    public:
      /**
       * @brief The constructor, using all of the reachable blocks of the MIRGraph.
       * @param mir_graph The MIRGraph considered.
       */
      explicit PreOrderDfsIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, 0, mir_graph->GetNumReachableBlocks()) {
        // Extra setup for the PreOrderDfsIterator.
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDfsOrder();
      }

      /**
       * @brief Get the next BasicBlock depending on iteration order.
       * @param had_change did the user of the iteration change the previous BasicBlock.
       * @return the next BasicBlock following the iteration order, 0 if finished.
       */
      virtual BasicBlock* Next(bool had_change = false) {
        // Update changed: if had_changed is true, we remember it for the whole iteration.
        changed_ |= had_change;

        return ForwardSingleNext();
      }
  };

  /**
   * @class RepeatingPreOrderDfsIterator
   * @brief Used to perform a Repeating Pre-order Depth-First-Search Iteration of a MIRGraph.
   * @details If there is a change during an iteration, the iteration starts over at the end of the iteration.
   */
  class RepeatingPreOrderDfsIterator : public DataflowIterator {
    public:
      /**
       * @brief The constructor, using all of the reachable blocks of the MIRGraph.
       * @param mir_graph The MIRGraph considered.
       */
      explicit RepeatingPreOrderDfsIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, 0, mir_graph->GetNumReachableBlocks()) {
        // Extra setup for the RepeatingPreOrderDfsIterator.
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDfsOrder();
      }

      /**
       * @brief Get the next BasicBlock depending on iteration order.
       * @param had_change did the user of the iteration change the previous BasicBlock.
       * @return the next BasicBlock following the iteration order, 0 if finished.
       */
      virtual BasicBlock* Next(bool had_change = false) {
        // Update changed: if had_changed is true, we remember it for the whole iteration.
        changed_ |= had_change;

        return ForwardRepeatNext();
      }
  };

  /**
   * @class RepeatingPostOrderDfsIterator
   * @brief Used to perform a Repeating Post-order Depth-First-Search Iteration of a MIRGraph.
   * @details If there is a change during an iteration, the iteration starts over at the end of the iteration.
   */
  class RepeatingPostOrderDfsIterator : public DataflowIterator {
    public:
      /**
       * @brief The constructor, using all of the reachable blocks of the MIRGraph.
       * @param mir_graph The MIRGraph considered.
       */
      explicit RepeatingPostOrderDfsIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, 0, mir_graph->GetNumReachableBlocks()) {
        // Extra setup for the RepeatingPostOrderDfsIterator.
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDfsPostOrder();
      }

      /**
       * @brief Get the next BasicBlock depending on iteration order.
       * @param had_change did the user of the iteration change the previous BasicBlock.
       * @return the next BasicBlock following the iteration order, 0 if finished.
       */
      virtual BasicBlock* Next(bool had_change = false) {
        // Update changed: if had_changed is true, we remember it for the whole iteration.
        changed_ |= had_change;

        return ForwardRepeatNext();
      }
  };

  /**
   * @class ReversePostOrderDfsIterator
   * @brief Used to perform a Reverse Post-order Depth-First-Search Iteration of a MIRGraph.
   */
  class ReversePostOrderDfsIterator : public DataflowIterator {
    public:
      /**
       * @brief The constructor, using all of the reachable blocks of the MIRGraph.
       * @param mir_graph The MIRGraph considered.
       */
      explicit ReversePostOrderDfsIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, mir_graph->GetNumReachableBlocks() -1, 0) {
        // Extra setup for the ReversePostOrderDfsIterator.
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDfsPostOrder();
      }

      /**
       * @brief Get the next BasicBlock depending on iteration order.
       * @param had_change did the user of the iteration change the previous BasicBlock.
       * @return the next BasicBlock following the iteration order, 0 if finished.
       */
      virtual BasicBlock* Next(bool had_change = false) {
        // Update changed: if had_changed is true, we remember it for the whole iteration.
        changed_ |= had_change;

        return ReverseSingleNext();
      }
  };

  /**
   * @class ReversePostOrderDfsIterator
   * @brief Used to perform a Repeating Reverse Post-order Depth-First-Search Iteration of a MIRGraph.
   * @details If there is a change during an iteration, the iteration starts over at the end of the iteration.
   */
  class RepeatingReversePostOrderDfsIterator : public DataflowIterator {
    public:
      /**
       * @brief The constructor, using all of the reachable blocks of the MIRGraph.
       * @param mir_graph The MIRGraph considered.
       */
      explicit RepeatingReversePostOrderDfsIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, mir_graph->GetNumReachableBlocks() -1, 0) {
        // Extra setup for the RepeatingReversePostOrderDfsIterator
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDfsPostOrder();
      }

      /**
       * @brief Get the next BasicBlock depending on iteration order.
       * @param had_change did the user of the iteration change the previous BasicBlock.
       * @return the next BasicBlock following the iteration order, 0 if finished.
       */
      virtual BasicBlock* Next(bool had_change = false) {
        // Update changed: if had_changed is true, we remember it for the whole iteration.
        changed_ |= had_change;

        return ReverseRepeatNext();
      }
  };

  /**
   * @class PostOrderDOMIterator
   * @brief Used to perform a Post-order Domination Iteration of a MIRGraph.
   */
  class PostOrderDOMIterator : public DataflowIterator {
    public:
      /**
       * @brief The constructor, using all of the reachable blocks of the MIRGraph.
       * @param mir_graph The MIRGraph considered.
       */
      explicit PostOrderDOMIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, 0, mir_graph->GetNumReachableBlocks()) {
        // Extra setup for thePostOrderDOMIterator.
        idx_ = start_idx_;
        block_id_list_ = mir_graph->GetDomPostOrder();
      }

      /**
       * @brief Get the next BasicBlock depending on iteration order.
       * @param had_change did the user of the iteration change the previous BasicBlock.
       * @return the next BasicBlock following the iteration order, 0 if finished.
       */
      virtual BasicBlock* Next(bool had_change = false) {
        // Update changed: if had_changed is true, we remember it for the whole iteration.
        changed_ |= had_change;

        return ForwardSingleNext();
      }
  };

  /**
   * @class AllNodesIterator
   * @brief Used to perform an iteration on all the BasicBlocks a MIRGraph.
   */
  class AllNodesIterator : public DataflowIterator {
    public:
      /**
       * @brief The constructor, using all of the reachable blocks of the MIRGraph.
       * @param mir_graph The MIRGraph considered.
       */
      explicit AllNodesIterator(MIRGraph* mir_graph)
          : DataflowIterator(mir_graph, 0, 0),
            all_nodes_iterator_(mir_graph->GetBlockList()) {
      }

      /**
       * @brief Resetting the iterator.
       */
      void Reset() {
        all_nodes_iterator_.Reset();
      }

      /**
       * @brief Get the next BasicBlock depending on iteration order.
       * @param had_change did the user of the iteration change the previous BasicBlock.
       * @return the next BasicBlock following the iteration order, 0 if finished.
       */
      virtual BasicBlock* Next(bool had_change = false) ALWAYS_INLINE;

    private:
      GrowableArray<BasicBlock*>::Iterator all_nodes_iterator_;    /**< @brief The list of all the nodes */
  };

}  // namespace art

#endif  // ART_COMPILER_DEX_DATAFLOW_ITERATOR_H_
