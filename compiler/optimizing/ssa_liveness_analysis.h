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

class CodeGenerator;

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
class LiveRange : public ArenaObject {
 public:
  LiveRange(size_t start, size_t end, LiveRange* next) : start_(start), end_(end), next_(next) {
    DCHECK_LT(start, end);
    DCHECK(next_ == nullptr || next_->GetStart() > GetEnd());
  }

  size_t GetStart() const { return start_; }
  size_t GetEnd() const { return end_; }
  LiveRange* GetNext() const { return next_; }

  bool IntersectsWith(const LiveRange& other) {
    return (start_ >= other.start_ && start_ < other.end_)
        || (other.start_ >= start_ && other.start_ < end_);
  }

  bool IsBefore(const LiveRange& other) {
    return end_ <= other.start_;
  }

  void Dump(std::ostream& stream) {
    stream << "[" << start_ << ", " << end_ << ")";
  }

 private:
  size_t start_;
  size_t end_;
  LiveRange* next_;

  friend class LiveInterval;

  DISALLOW_COPY_AND_ASSIGN(LiveRange);
};

/**
 * A use position represents a live interval use at a given position.
 */
class UsePosition : public ArenaObject {
 public:
  UsePosition(HInstruction* user,
              size_t input_index,
              bool is_environment,
              size_t position,
              UsePosition* next)
      : user_(user),
        input_index_(input_index),
        is_environment_(is_environment),
        position_(position),
        next_(next) {
    DCHECK(user->AsPhi() != nullptr || GetPosition() == user->GetLifetimePosition() + 1);
    DCHECK(next_ == nullptr || next->GetPosition() >= GetPosition());
  }

  size_t GetPosition() const { return position_; }

  UsePosition* GetNext() const { return next_; }

  HInstruction* GetUser() const { return user_; }

  bool GetIsEnvironment() const { return is_environment_; }

  size_t GetInputIndex() const { return input_index_; }

  void Dump(std::ostream& stream) const {
    stream << position_;
  }

 private:
  HInstruction* const user_;
  const size_t input_index_;
  const bool is_environment_;
  const size_t position_;
  UsePosition* const next_;

  DISALLOW_COPY_AND_ASSIGN(UsePosition);
};

/**
 * An interval is a list of disjoint live ranges where an instruction is live.
 * Each instruction that has uses gets an interval.
 */
class LiveInterval : public ArenaObject {
 public:
  LiveInterval(ArenaAllocator* allocator, Primitive::Type type, HInstruction* defined_by = nullptr)
      : allocator_(allocator),
        first_range_(nullptr),
        last_range_(nullptr),
        first_use_(nullptr),
        type_(type),
        next_sibling_(nullptr),
        parent_(this),
        register_(kNoRegister),
        spill_slot_(kNoSpillSlot),
        is_fixed_(false),
        defined_by_(defined_by) {}

  static LiveInterval* MakeFixedInterval(ArenaAllocator* allocator, int reg, Primitive::Type type) {
    LiveInterval* interval = new (allocator) LiveInterval(allocator, type);
    interval->SetRegister(reg);
    interval->is_fixed_ = true;
    return interval;
  }

  bool IsFixed() const { return is_fixed_; }

  void AddUse(HInstruction* instruction, size_t input_index, bool is_environment) {
    // Set the use within the instruction.
    // TODO: Use the instruction's location to know whether the instruction can die
    // at entry, or needs to say alive within the user.
    size_t position = instruction->GetLifetimePosition() + 1;
    size_t start_block_position = instruction->GetBlock()->GetLifetimeStart();
    size_t end_block_position = instruction->GetBlock()->GetLifetimeEnd();
    if (first_range_ == nullptr) {
      // First time we see a use of that interval.
      first_range_ = last_range_ = new (allocator_) LiveRange(start_block_position, position, nullptr);
    } else if (first_range_->GetStart() == start_block_position) {
      // There is a use later in the same block.
      DCHECK_LE(position, first_range_->GetEnd());
    } else if (first_range_->GetStart() == end_block_position) {
      // Last use is in the following block.
      first_range_->start_ = start_block_position;
    } else {
      DCHECK(first_range_->GetStart() > position);
      // There is a hole in the interval. Create a new range.
      first_range_ = new (allocator_) LiveRange(start_block_position, position, first_range_);
    }
    first_use_ = new (allocator_) UsePosition(
        instruction, input_index, is_environment, position, first_use_);
  }

  void AddPhiUse(HInstruction* instruction, size_t input_index, HBasicBlock* block) {
    DCHECK(instruction->AsPhi() != nullptr);
    first_use_ = new (allocator_) UsePosition(
        instruction, input_index, false, block->GetLifetimeEnd(), first_use_);
  }

  void AddRange(size_t start, size_t end) {
    if (first_range_ == nullptr) {
      first_range_ = last_range_ = new (allocator_) LiveRange(start, end, first_range_);
    } else if (first_range_->GetStart() == end) {
      // There is a use in the following block.
      first_range_->start_ = start;
    } else {
      DCHECK(first_range_->GetStart() > end);
      // There is a hole in the interval. Create a new range.
      first_range_ = new (allocator_) LiveRange(start, end, first_range_);
    }
  }

  void AddLoopRange(size_t start, size_t end) {
    DCHECK(first_range_ != nullptr);
    while (first_range_ != nullptr && first_range_->GetEnd() < end) {
      DCHECK_LE(start, first_range_->GetStart());
      first_range_ = first_range_->GetNext();
    }
    if (first_range_ == nullptr) {
      // Uses are only in the loop.
      first_range_ = last_range_ = new (allocator_) LiveRange(start, end, nullptr);
    } else {
      // There are uses after the loop.
      first_range_->start_ = start;
    }
  }

  bool HasSpillSlot() const { return spill_slot_ != kNoSpillSlot; }
  void SetSpillSlot(int slot) { spill_slot_ = slot; }
  int GetSpillSlot() const { return spill_slot_; }

  void SetFrom(size_t from) {
    if (first_range_ != nullptr) {
      first_range_->start_ = from;
    } else {
      // Instruction without uses.
      DCHECK(!defined_by_->HasUses());
      DCHECK(from == defined_by_->GetLifetimePosition());
      first_range_ = last_range_ = new (allocator_) LiveRange(from, from + 2, nullptr);
    }
  }

  LiveInterval* GetParent() const { return parent_; }

  LiveRange* GetFirstRange() const { return first_range_; }

  int GetRegister() const { return register_; }
  void SetRegister(int reg) { register_ = reg; }
  void ClearRegister() { register_ = kNoRegister; }
  bool HasRegister() const { return register_ != kNoRegister; }

  bool IsDeadAt(size_t position) const {
    return last_range_->GetEnd() <= position;
  }

  bool Covers(size_t position) const {
    LiveRange* current = first_range_;
    while (current != nullptr) {
      if (position >= current->GetStart() && position < current->GetEnd()) {
        return true;
      }
      current = current->GetNext();
    }
    return false;
  }

  /**
   * Returns the first intersection of this interval with `other`.
   */
  size_t FirstIntersectionWith(LiveInterval* other) const {
    // Advance both intervals and find the first matching range start in
    // this interval.
    LiveRange* my_range = first_range_;
    LiveRange* other_range = other->first_range_;
    do {
      if (my_range->IntersectsWith(*other_range)) {
        return std::max(my_range->GetStart(), other_range->GetStart());
      } else if (my_range->IsBefore(*other_range)) {
        my_range = my_range->GetNext();
        if (my_range == nullptr) {
          return kNoLifetime;
        }
      } else {
        DCHECK(other_range->IsBefore(*my_range));
        other_range = other_range->GetNext();
        if (other_range == nullptr) {
          return kNoLifetime;
        }
      }
    } while (true);
  }

  size_t GetStart() const {
    return first_range_->GetStart();
  }

  size_t GetEnd() const {
    return last_range_->GetEnd();
  }

  size_t FirstRegisterUseAfter(size_t position) const {
    if (position == GetStart() && defined_by_ != nullptr) {
      LocationSummary* locations = defined_by_->GetLocations();
      Location location = locations->Out();
      // This interval is the first interval of the instruction. If the output
      // of the instruction requires a register, we return the position of that instruction
      // as the first register use.
      if (location.IsUnallocated()) {
        if ((location.GetPolicy() == Location::kRequiresRegister)
             || (location.GetPolicy() == Location::kSameAsFirstInput
                 && locations->InAt(0).GetPolicy() == Location::kRequiresRegister)) {
          return position;
        }
      }
    }

    UsePosition* use = first_use_;
    size_t end = GetEnd();
    while (use != nullptr && use->GetPosition() <= end) {
      size_t use_position = use->GetPosition();
      if (use_position >= position && !use->GetIsEnvironment()) {
        Location location = use->GetUser()->GetLocations()->InAt(use->GetInputIndex());
        if (location.IsUnallocated() && location.GetPolicy() == Location::kRequiresRegister) {
          return use_position;
        }
      }
      use = use->GetNext();
    }
    return kNoLifetime;
  }

  size_t FirstRegisterUse() const {
    return FirstRegisterUseAfter(GetStart());
  }

  UsePosition* GetFirstUse() const {
    return first_use_;
  }

  Primitive::Type GetType() const {
    return type_;
  }

  HInstruction* GetDefinedBy() const {
    return defined_by_;
  }

  /**
   * Split this interval at `position`. This interval is changed to:
   * [start ... position).
   *
   * The new interval covers:
   * [position ... end)
   */
  LiveInterval* SplitAt(size_t position) {
    DCHECK(!is_fixed_);
    DCHECK_GT(position, GetStart());

    if (last_range_->GetEnd() <= position) {
      // This range dies before `position`, no need to split.
      return nullptr;
    }

    LiveInterval* new_interval = new (allocator_) LiveInterval(allocator_, type_);
    new_interval->next_sibling_ = next_sibling_;
    next_sibling_ = new_interval;
    new_interval->parent_ = parent_;

    new_interval->first_use_ = first_use_;
    LiveRange* current = first_range_;
    LiveRange* previous = nullptr;
    // Iterate over the ranges, and either find a range that covers this position, or
    // a two ranges in between this position (that is, the position is in a lifetime hole).
    do {
      if (position >= current->GetEnd()) {
        // Move to next range.
        previous = current;
        current = current->next_;
      } else if (position <= current->GetStart()) {
        // If the previous range did not cover this position, we know position is in
        // a lifetime hole. We can just break the first_range_ and last_range_ links
        // and return the new interval.
        DCHECK(previous != nullptr);
        DCHECK(current != first_range_);
        new_interval->last_range_ = last_range_;
        last_range_ = previous;
        previous->next_ = nullptr;
        new_interval->first_range_ = current;
        return new_interval;
      } else {
        // This range covers position. We create a new last_range_ for this interval
        // that covers last_range_->Start() and position. We also shorten the current
        // range and make it the first range of the new interval.
        DCHECK(position < current->GetEnd() && position > current->GetStart());
        new_interval->last_range_ = last_range_;
        last_range_ = new (allocator_) LiveRange(current->start_, position, nullptr);
        if (previous != nullptr) {
          previous->next_ = last_range_;
        } else {
          first_range_ = last_range_;
        }
        new_interval->first_range_ = current;
        current->start_ = position;
        return new_interval;
      }
    } while (current != nullptr);

    LOG(FATAL) << "Unreachable";
    return nullptr;
  }

  bool StartsBefore(LiveInterval* other) const {
    return GetStart() <= other->GetStart();
  }

  bool StartsAfter(LiveInterval* other) const {
    return GetStart() >= other->GetStart();
  }

  void Dump(std::ostream& stream) const {
    stream << "ranges: { ";
    LiveRange* current = first_range_;
    do {
      current->Dump(stream);
      stream << " ";
    } while ((current = current->GetNext()) != nullptr);
    stream << "}, uses: { ";
    UsePosition* use = first_use_;
    if (use != nullptr) {
      do {
        use->Dump(stream);
        stream << " ";
      } while ((use = use->GetNext()) != nullptr);
    }
    stream << "}";
  }

  LiveInterval* GetNextSibling() const { return next_sibling_; }

 private:
  ArenaAllocator* const allocator_;

  // Ranges of this interval. We need a quick access to the last range to test
  // for liveness (see `IsDeadAt`).
  LiveRange* first_range_;
  LiveRange* last_range_;

  // Uses of this interval. Note that this linked list is shared amongst siblings.
  UsePosition* first_use_;

  // The instruction type this interval corresponds to.
  const Primitive::Type type_;

  // Live interval that is the result of a split.
  LiveInterval* next_sibling_;

  // The first interval from which split intervals come from.
  LiveInterval* parent_;

  // The register allocated to this interval.
  int register_;

  // The spill slot allocated to this interval.
  int spill_slot_;

  // Whether the interval is for a fixed register.
  bool is_fixed_;

  // The instruction represented by this interval.
  HInstruction* const defined_by_;

  static constexpr int kNoRegister = -1;
  static constexpr int kNoSpillSlot = -1;

  DISALLOW_COPY_AND_ASSIGN(LiveInterval);
};

class SsaLivenessAnalysis : public ValueObject {
 public:
  SsaLivenessAnalysis(const HGraph& graph, CodeGenerator* codegen)
      : graph_(graph),
        codegen_(codegen),
        linear_post_order_(graph.GetArena(), graph.GetBlocks().Size()),
        block_infos_(graph.GetArena(), graph.GetBlocks().Size()),
        instructions_from_ssa_index_(graph.GetArena(), 0),
        instructions_from_lifetime_position_(graph.GetArena(), 0),
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

  HInstruction* GetInstructionFromSsaIndex(size_t index) const {
    return instructions_from_ssa_index_.Get(index);
  }

  HInstruction* GetInstructionFromPosition(size_t index) const {
    return instructions_from_lifetime_position_.Get(index);
  }

  size_t GetMaxLifetimePosition() const {
    return instructions_from_lifetime_position_.Size() * 2 - 1;
  }

  size_t GetNumberOfSsaValues() const {
    return number_of_ssa_values_;
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
  CodeGenerator* const codegen_;
  GrowableArray<HBasicBlock*> linear_post_order_;
  GrowableArray<BlockInfo*> block_infos_;

  // Temporary array used when computing live_in, live_out, and kill sets.
  GrowableArray<HInstruction*> instructions_from_ssa_index_;

  // Temporary array used when inserting moves in the graph.
  GrowableArray<HInstruction*> instructions_from_lifetime_position_;
  size_t number_of_ssa_values_;

  DISALLOW_COPY_AND_ASSIGN(SsaLivenessAnalysis);
};

class HLinearOrderIterator : public ValueObject {
 public:
  explicit HLinearOrderIterator(const SsaLivenessAnalysis& liveness)
      : post_order_(liveness.GetLinearPostOrder()), index_(liveness.GetLinearPostOrder().Size()) {}

  bool Done() const { return index_ == 0; }
  HBasicBlock* Current() const { return post_order_.Get(index_ -1); }
  void Advance() { --index_; DCHECK_GE(index_, 0U); }

 private:
  const GrowableArray<HBasicBlock*>& post_order_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HLinearOrderIterator);
};

class HLinearPostOrderIterator : public ValueObject {
 public:
  explicit HLinearPostOrderIterator(const SsaLivenessAnalysis& liveness)
      : post_order_(liveness.GetLinearPostOrder()), index_(0) {}

  bool Done() const { return index_ == post_order_.Size(); }
  HBasicBlock* Current() const { return post_order_.Get(index_); }
  void Advance() { ++index_; }

 private:
  const GrowableArray<HBasicBlock*>& post_order_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HLinearPostOrderIterator);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_SSA_LIVENESS_ANALYSIS_H_
