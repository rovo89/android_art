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

/**
 * A live range contains the start and end of a range where an instruction
 * is live.
 */
class LiveRange : public ValueObject {
 public:
  LiveRange(size_t start, size_t end) : start_(start), end_(end) {
    DCHECK_LT(start, end);
  }

  size_t GetStart() const { return start_; }
  size_t GetEnd() const { return end_; }

 private:
  size_t start_;
  size_t end_;
};

static constexpr int kDefaultNumberOfRanges = 3;

/**
 * An interval is a list of disjoint live ranges where an instruction is live.
 * Each instruction that has uses gets an interval.
 */
class LiveInterval : public ArenaObject {
 public:
  explicit LiveInterval(ArenaAllocator* allocator) : ranges_(allocator, kDefaultNumberOfRanges) {}

  void AddUse(HInstruction* instruction) {
    size_t position = instruction->GetLifetimePosition();
    size_t start_block_position = instruction->GetBlock()->GetLifetimeStart();
    size_t end_block_position = instruction->GetBlock()->GetLifetimeEnd();
    if (ranges_.IsEmpty()) {
      // First time we see a use of that interval.
      ranges_.Add(LiveRange(start_block_position, position));
    } else if (ranges_.Peek().GetStart() == start_block_position) {
      // There is a use later in the same block.
      DCHECK_LE(position, ranges_.Peek().GetEnd());
    } else if (ranges_.Peek().GetStart() == end_block_position + 1) {
      // Last use is in a following block.
      LiveRange existing = ranges_.Pop();
      ranges_.Add(LiveRange(start_block_position, existing.GetEnd()));
    } else {
      // There is a hole in the interval. Create a new range.
      ranges_.Add(LiveRange(start_block_position, position));
    }
  }

  void AddRange(size_t start, size_t end) {
    if (ranges_.IsEmpty()) {
      ranges_.Add(LiveRange(start, end));
    } else if (ranges_.Peek().GetStart() == end + 1) {
      // There is a use in the following block.
      LiveRange existing = ranges_.Pop();
      ranges_.Add(LiveRange(start, existing.GetEnd()));
    } else {
      // There is a hole in the interval. Create a new range.
      ranges_.Add(LiveRange(start, end));
    }
  }

  void AddLoopRange(size_t start, size_t end) {
    DCHECK(!ranges_.IsEmpty());
    while (!ranges_.IsEmpty() && ranges_.Peek().GetEnd() < end) {
      DCHECK_LE(start, ranges_.Peek().GetStart());
      ranges_.Pop();
    }
    if (ranges_.IsEmpty()) {
      // Uses are only in the loop.
      ranges_.Add(LiveRange(start, end));
    } else {
      // There are uses after the loop.
      LiveRange range = ranges_.Pop();
      ranges_.Add(LiveRange(start, range.GetEnd()));
    }
  }

  void SetFrom(size_t from) {
    DCHECK(!ranges_.IsEmpty());
    LiveRange existing = ranges_.Pop();
    ranges_.Add(LiveRange(from, existing.GetEnd()));
  }

  const GrowableArray<LiveRange>& GetRanges() const { return ranges_; }

 private:
  GrowableArray<LiveRange> ranges_;

  DISALLOW_COPY_AND_ASSIGN(LiveInterval);
};

class SsaLivenessAnalysis : public ValueObject {
 public:
  explicit SsaLivenessAnalysis(const HGraph& graph)
      : graph_(graph),
        linear_post_order_(graph.GetArena(), graph.GetBlocks().Size()),
        block_infos_(graph.GetArena(), graph.GetBlocks().Size()),
        instructions_from_ssa_index_(graph.GetArena(), 0),
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

  HInstruction* GetInstructionFromSsaIndex(size_t index) {
    return instructions_from_ssa_index_.Get(index);
  }

 private:
  // Linearize the graph so that:
  // (1): a block is always after its dominator,
  // (2): blocks of loops are contiguous.
  // This creates a natural and efficient ordering when visualizing live ranges.
  void LinearizeGraph();

  // Give an SSA number to each instruction that defines a value used by another instruction,
  // and setup the lifetime information of each instruction and block.
  void NumberInstructions();

  // Compute live ranges of instructions, as well as live_in, live_out and kill sets.
  void ComputeLiveness();

  // Compute the live ranges of instructions, as well as the initial live_in, live_out and
  // kill sets, that do not take into account backward branches.
  void ComputeLiveRanges();

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
  GrowableArray<HInstruction*> instructions_from_ssa_index_;
  size_t number_of_ssa_values_;

  DISALLOW_COPY_AND_ASSIGN(SsaLivenessAnalysis);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_SSA_LIVENESS_ANALYSIS_H_
