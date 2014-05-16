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

#ifndef ART_COMPILER_OPTIMIZING_SSA_LIVENESS_ANALYSIS_H_
#define ART_COMPILER_OPTIMIZING_SSA_LIVENESS_ANALYSIS_H_

#include "nodes.h"

namespace art {

class BlockInfo : public ArenaObject {
 public:
  BlockInfo(ArenaAllocator* allocator, const HBasicBlock& block, size_t number_of_ssa_values)
      : block_(block),
        live_in_(allocator, number_of_ssa_values, false),
        live_out_(allocator, number_of_ssa_values, false),
        kill_(allocator, number_of_ssa_values, false) {
    live_in_.ClearAllBits();
    live_out_.ClearAllBits();
    kill_.ClearAllBits();
  }

 private:
  const HBasicBlock& block_;
  ArenaBitVector live_in_;
  ArenaBitVector live_out_;
  ArenaBitVector kill_;

  friend class SsaLivenessAnalysis;

  DISALLOW_COPY_AND_ASSIGN(BlockInfo);
};

class SsaLivenessAnalysis : public ValueObject {
 public:
  explicit SsaLivenessAnalysis(const HGraph& graph)
      : graph_(graph),
        linear_post_order_(graph.GetArena(), graph.GetBlocks().Size()),
        block_infos_(graph.GetArena(), graph.GetBlocks().Size()),
        number_of_ssa_values_(0) {
    block_infos_.SetSize(graph.GetBlocks().Size());
  }

  void Analyze();

  BitVector* GetLiveInSet(const HBasicBlock& block) const {
    return &block_infos_.Get(block.GetBlockId())->live_in_;
  }

  BitVector* GetLiveOutSet(const HBasicBlock& block) const {
    return &block_infos_.Get(block.GetBlockId())->live_out_;
  }

  BitVector* GetKillSet(const HBasicBlock& block) const {
    return &block_infos_.Get(block.GetBlockId())->kill_;
  }

  const GrowableArray<HBasicBlock*>& GetLinearPostOrder() const {
    return linear_post_order_;
  }

 private:
  // Linearize the graph so that:
  // (1): a block is always after its dominator,
  // (2): blocks of loops are contiguous.
  // This creates a natural and efficient ordering when visualizing live ranges.
  void LinearizeGraph();

  // Give an SSA number to each instruction that defines a value used by another instruction.
  void NumberInstructions();

  // Compute live_in, live_out and kill sets.
  void ComputeSets();

  // Compute the initial live_in, live_out and kill sets, without analyzing
  // backward branches.
  void ComputeInitialSets();

  // After computing the initial sets, this method does a fixed point
  // calculation over the live_in and live_out set to take into account
  // backwards branches.
  void ComputeLiveInAndLiveOutSets();

  // Update the live_in set of the block and returns whether it has changed.
  bool UpdateLiveIn(const HBasicBlock& block);

  // Update the live_out set of the block and returns whether it has changed.
  bool UpdateLiveOut(const HBasicBlock& block);

  const HGraph& graph_;
  GrowableArray<HBasicBlock*> linear_post_order_;
  GrowableArray<BlockInfo*> block_infos_;
  size_t number_of_ssa_values_;

  DISALLOW_COPY_AND_ASSIGN(SsaLivenessAnalysis);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_SSA_LIVENESS_ANALYSIS_H_
