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

#include "bounds_check_elimination.h"
#include "nodes.h"
#include "utils/arena_containers.h"

namespace art {

class MonotonicValueRange;

/**
 * A value bound is represented as a pair of value and constant,
 * e.g. array.length - 1.
 */
class ValueBound : public ValueObject {
 public:
  static ValueBound Create(HInstruction* instruction, int constant) {
    if (instruction == nullptr) {
      return ValueBound(nullptr, constant);
    }
    if (instruction->IsIntConstant()) {
      return ValueBound(nullptr, instruction->AsIntConstant()->GetValue() + constant);
    }
    return ValueBound(instruction, constant);
  }

  HInstruction* GetInstruction() const { return instruction_; }
  int GetConstant() const { return constant_; }

  bool IsRelativeToArrayLength() const {
    return instruction_ != nullptr && instruction_->IsArrayLength();
  }

  bool IsConstant() const {
    return instruction_ == nullptr;
  }

  static ValueBound Min() { return ValueBound(nullptr, INT_MIN); }
  static ValueBound Max() { return ValueBound(nullptr, INT_MAX); }

  bool Equals(ValueBound bound) const {
    return instruction_ == bound.instruction_ && constant_ == bound.constant_;
  }

  // Returns if it's certain bound1 >= bound2.
  bool GreaterThanOrEqual(ValueBound bound) const {
    if (instruction_ == bound.instruction_) {
      if (instruction_ == nullptr) {
        // Pure constant.
        return constant_ >= bound.constant_;
      }
      // There might be overflow/underflow. Be conservative for now.
      return false;
    }
    // Not comparable. Just return false.
    return false;
  }

  // Returns if it's certain bound1 <= bound2.
  bool LessThanOrEqual(ValueBound bound) const {
    if (instruction_ == bound.instruction_) {
      if (instruction_ == nullptr) {
        // Pure constant.
        return constant_ <= bound.constant_;
      }
      if (IsRelativeToArrayLength()) {
        // Array length is guaranteed to be no less than 0.
        // No overflow/underflow can happen if both constants are negative.
        if (constant_ <= 0 && bound.constant_ <= 0) {
          return constant_ <= bound.constant_;
        }
        // There might be overflow/underflow. Be conservative for now.
        return false;
      }
    }

    // In case the array length is some constant, we can
    // still compare.
    if (IsConstant() && bound.IsRelativeToArrayLength()) {
      HInstruction* array = bound.GetInstruction()->AsArrayLength()->InputAt(0);
      if (array->IsNullCheck()) {
        array = array->AsNullCheck()->InputAt(0);
      }
      if (array->IsNewArray()) {
        HInstruction* len = array->InputAt(0);
        if (len->IsIntConstant()) {
          int len_const = len->AsIntConstant()->GetValue();
          return constant_ <= len_const + bound.GetConstant();
        }
      }
    }

    // Not comparable. Just return false.
    return false;
  }

  // Try to narrow lower bound. Returns the greatest of the two if possible.
  // Pick one if they are not comparable.
  static ValueBound NarrowLowerBound(ValueBound bound1, ValueBound bound2) {
    if (bound1.instruction_ == bound2.instruction_) {
      // Same instruction, compare the constant part.
      return ValueBound(bound1.instruction_,
                        std::max(bound1.constant_, bound2.constant_));
    }

    // Not comparable. Just pick one. We may lose some info, but that's ok.
    // Favor constant as lower bound.
    return bound1.IsConstant() ? bound1 : bound2;
  }

  // Try to narrow upper bound. Returns the lowest of the two if possible.
  // Pick one if they are not comparable.
  static ValueBound NarrowUpperBound(ValueBound bound1, ValueBound bound2) {
    if (bound1.instruction_ == bound2.instruction_) {
      // Same instruction, compare the constant part.
      return ValueBound(bound1.instruction_,
                        std::min(bound1.constant_, bound2.constant_));
    }

    // Not comparable. Just pick one. We may lose some info, but that's ok.
    // Favor array length as upper bound.
    return bound1.IsRelativeToArrayLength() ? bound1 : bound2;
  }

  // Add a constant to a ValueBound. If the constant part of the ValueBound
  // overflows/underflows, then we can't accurately represent it. For correctness,
  // just return Max/Min() depending on whether the returned ValueBound is used for
  // lower/upper bound.
  ValueBound Add(int c, bool for_lower_bound, bool* overflow_or_underflow) const {
    *overflow_or_underflow = false;
    if (c == 0) {
      return *this;
    }

    int new_constant;
    if (c > 0) {
      if (constant_ > INT_MAX - c) {
        // Constant part overflows.
        *overflow_or_underflow = true;
        return for_lower_bound ? Min() : Max();
      } else {
        new_constant = constant_ + c;
      }
    } else {
      if (constant_ < INT_MIN - c) {
        // Constant part underflows.
        *overflow_or_underflow = true;
        return for_lower_bound ? Min() : Max();
      } else {
        new_constant = constant_ + c;
      }
    }
    return ValueBound(instruction_, new_constant);
  }

 private:
  ValueBound(HInstruction* instruction, int constant)
      : instruction_(instruction), constant_(constant) {}

  HInstruction* instruction_;
  int constant_;
};

/**
 * Represent a range of lower bound and upper bound, both being inclusive.
 * Currently a ValueRange may be generated as a result of the following:
 * comparisons related to array bounds, array bounds check, add/sub on top
 * of an existing value range, or a loop phi corresponding to an
 * incrementing/decrementing array index (MonotonicValueRange).
 */
class ValueRange : public ArenaObject<kArenaAllocMisc> {
 public:
  ValueRange(ArenaAllocator* allocator, ValueBound lower, ValueBound upper)
      : allocator_(allocator), lower_(lower), upper_(upper) {}

  virtual ~ValueRange() {}

  virtual const MonotonicValueRange* AsMonotonicValueRange() const { return nullptr; }
  bool IsMonotonicValueRange() const {
    return AsMonotonicValueRange() != nullptr;
  }

  ArenaAllocator* GetAllocator() const { return allocator_; }
  ValueBound GetLower() const { return lower_; }
  ValueBound GetUpper() const { return upper_; }

  // If it's certain that this value range fits in other_range.
  virtual bool FitsIn(ValueRange* other_range) const {
    if (other_range == nullptr) {
      return true;
    }
    DCHECK(!other_range->IsMonotonicValueRange());
    return lower_.GreaterThanOrEqual(other_range->lower_) &&
           upper_.LessThanOrEqual(other_range->upper_);
  }

  // Returns the intersection of this and range.
  // If it's not possible to do intersection because some
  // bounds are not comparable, it's ok to pick either bound.
  virtual ValueRange* Narrow(ValueRange* range) {
    if (range == nullptr) {
      return this;
    }

    if (range->IsMonotonicValueRange()) {
      return this;
    }

    return new (allocator_) ValueRange(
        allocator_,
        ValueBound::NarrowLowerBound(lower_, range->lower_),
        ValueBound::NarrowUpperBound(upper_, range->upper_));
  }

  // Shift a range by a constant. If either bound can't be represented
  // as (instruction+c) format due to possible overflow/underflow,
  // return the full integer range.
  ValueRange* Add(int constant) const {
    bool overflow_or_underflow;
    ValueBound lower = lower_.Add(constant, true, &overflow_or_underflow);
    if (overflow_or_underflow) {
      // We can't accurately represent the bounds anymore.
      return FullIntRange();
    }
    ValueBound upper = upper_.Add(constant, false, &overflow_or_underflow);
    if (overflow_or_underflow) {
      // We can't accurately represent the bounds anymore.
      return FullIntRange();
    }
    return new (allocator_) ValueRange(allocator_, lower, upper);
  }

  // Return [INT_MIN, INT_MAX].
  ValueRange* FullIntRange() const {
    return new (allocator_) ValueRange(allocator_, ValueBound::Min(), ValueBound::Max());
  }

 private:
  ArenaAllocator* const allocator_;
  const ValueBound lower_;  // inclusive
  const ValueBound upper_;  // inclusive

  DISALLOW_COPY_AND_ASSIGN(ValueRange);
};

/**
 * A monotonically incrementing/decrementing value range, e.g.
 * the variable i in "for (int i=0; i<array.length; i++)".
 * Special care needs to be taken to account for overflow/underflow
 * of such value ranges.
 */
class MonotonicValueRange : public ValueRange {
 public:
  static MonotonicValueRange* Create(ArenaAllocator* allocator,
                                     HInstruction* initial, int increment) {
    DCHECK_NE(increment, 0);
    // To be conservative, give it full range [INT_MIN, INT_MAX] in case it's
    // used as a regular value range, due to possible overflow/underflow.
    return new (allocator) MonotonicValueRange(
        allocator, ValueBound::Min(), ValueBound::Max(), initial, increment);
  }

  virtual ~MonotonicValueRange() {}

  const MonotonicValueRange* AsMonotonicValueRange() const OVERRIDE { return this; }

  // If it's certain that this value range fits in other_range.
  bool FitsIn(ValueRange* other_range) const OVERRIDE {
    if (other_range == nullptr) {
      return true;
    }
    DCHECK(!other_range->IsMonotonicValueRange());
    return false;
  }

  // Try to narrow this MonotonicValueRange given another range.
  // Ideally it will return a normal ValueRange. But due to
  // possible overflow/underflow, that may not be possible.
  ValueRange* Narrow(ValueRange* range) OVERRIDE {
    if (range == nullptr) {
      return this;
    }
    DCHECK(!range->IsMonotonicValueRange());

    if (increment_ > 0) {
      // Monotonically increasing.
      ValueBound lower = ValueBound::NarrowLowerBound(
          ValueBound::Create(initial_, 0), range->GetLower());

      // We currently conservatively assume max array length is INT_MAX. If we can
      // make assumptions about the max array length, e.g. due to the max heap size,
      // divided by the element size (such as 4 bytes for each integer array), we can
      // lower this number and rule out some possible overflows.
      int max_array_len = INT_MAX;

      int upper = INT_MAX;
      if (range->GetUpper().IsConstant()) {
        upper = range->GetUpper().GetConstant();
      } else if (range->GetUpper().IsRelativeToArrayLength()) {
        int constant = range->GetUpper().GetConstant();
        if (constant <= 0) {
          // Normal case. e.g. <= array.length - 1, <= array.length - 2, etc.
          upper = max_array_len + constant;
        } else {
          // There might be overflow. Give up narrowing.
          return this;
        }
      } else {
        // There might be overflow. Give up narrowing.
        return this;
      }

      // If we can prove for the last number in sequence of initial_,
      // initial_ + increment_, initial_ + 2 x increment_, ...
      // that's <= upper, (last_num_in_sequence + increment_) doesn't trigger overflow,
      // then this MonoticValueRange is narrowed to a normal value range.

      // Be conservative first, assume last number in the sequence hits upper.
      int last_num_in_sequence = upper;
      if (initial_->IsIntConstant()) {
        int initial_constant = initial_->AsIntConstant()->GetValue();
        if (upper <= initial_constant) {
          last_num_in_sequence = upper;
        } else {
          // Cast to int64_t for the substraction part to avoid int overflow.
          last_num_in_sequence = initial_constant +
              ((int64_t)upper - (int64_t)initial_constant) / increment_ * increment_;
        }
      }
      if (last_num_in_sequence <= INT_MAX - increment_) {
        // No overflow. The sequence will be stopped by the upper bound test as expected.
        return new (GetAllocator()) ValueRange(GetAllocator(), lower, range->GetUpper());
      }

      // There might be overflow. Give up narrowing.
      return this;
    } else {
      DCHECK_NE(increment_, 0);
      // Monotonically decreasing.
      ValueBound upper = ValueBound::NarrowUpperBound(
          ValueBound::Create(initial_, 0), range->GetUpper());

      // Need to take care of underflow. Try to prove underflow won't happen
      // for common cases. Basically need to be able to prove for any value
      // that's >= range->GetLower(), it won't be positive with value+increment.
      if (range->GetLower().IsConstant()) {
        int constant = range->GetLower().GetConstant();
        if (constant >= INT_MIN - increment_) {
          return new (GetAllocator()) ValueRange(GetAllocator(), range->GetLower(), upper);
        }
      }

      // There might be underflow. Give up narrowing.
      return this;
    }
  }

 private:
  MonotonicValueRange(ArenaAllocator* allocator, ValueBound lower,
                      ValueBound upper, HInstruction* initial, int increment)
      : ValueRange(allocator, lower, upper),
        initial_(initial),
        increment_(increment) {}

  HInstruction* const initial_;
  const int increment_;

  DISALLOW_COPY_AND_ASSIGN(MonotonicValueRange);
};

class BCEVisitor : public HGraphVisitor {
 public:
  explicit BCEVisitor(HGraph* graph)
      : HGraphVisitor(graph),
        maps_(graph->GetBlocks().Size()) {}

 private:
  // Return the map of proven value ranges at the beginning of a basic block.
  ArenaSafeMap<int, ValueRange*>* GetValueRangeMap(HBasicBlock* basic_block) {
    int block_id = basic_block->GetBlockId();
    if (maps_.at(block_id) == nullptr) {
      std::unique_ptr<ArenaSafeMap<int, ValueRange*>> map(
          new ArenaSafeMap<int, ValueRange*>(
              std::less<int>(), GetGraph()->GetArena()->Adapter()));
      maps_.at(block_id) = std::move(map);
    }
    return maps_.at(block_id).get();
  }

  // Traverse up the dominator tree to look for value range info.
  ValueRange* LookupValueRange(HInstruction* instruction, HBasicBlock* basic_block) {
    while (basic_block != nullptr) {
      ArenaSafeMap<int, ValueRange*>* map = GetValueRangeMap(basic_block);
      if (map->find(instruction->GetId()) != map->end()) {
        return map->Get(instruction->GetId());
      }
      basic_block = basic_block->GetDominator();
    }
    // Didn't find any.
    return nullptr;
  }

  // Try to detect useful value bound format from an instruction, e.g.
  // a constant or array length related value.
  ValueBound DetectValueBoundFromValue(HInstruction* instruction) {
    if (instruction->IsIntConstant()) {
      return ValueBound::Create(nullptr, instruction->AsIntConstant()->GetValue());
    }

    if (instruction->IsArrayLength()) {
      return ValueBound::Create(instruction, 0);
    }
    // Try to detect (array.length + c) format.
    if (instruction->IsAdd()) {
      HAdd* add = instruction->AsAdd();
      HInstruction* left = add->GetLeft();
      HInstruction* right = add->GetRight();
      if (left->IsArrayLength() && right->IsIntConstant()) {
        return ValueBound::Create(left, right->AsIntConstant()->GetValue());
      }
    }

    // No useful bound detected.
    return ValueBound::Max();
  }

  // Narrow the value range of 'instruction' at the end of 'basic_block' with 'range',
  // and push the narrowed value range to 'successor'.
  void ApplyRangeFromComparison(HInstruction* instruction, HBasicBlock* basic_block,
                  HBasicBlock* successor, ValueRange* range) {
    ValueRange* existing_range = LookupValueRange(instruction, basic_block);
    ValueRange* narrowed_range = (existing_range == nullptr) ?
        range : existing_range->Narrow(range);
    if (narrowed_range != nullptr) {
      GetValueRangeMap(successor)->Overwrite(instruction->GetId(), narrowed_range);
    }
  }

  // Handle "if (left cmp_cond right)".
  void HandleIf(HIf* instruction, HInstruction* left, HInstruction* right, IfCondition cond) {
    HBasicBlock* block = instruction->GetBlock();

    HBasicBlock* true_successor = instruction->IfTrueSuccessor();
    // There should be no critical edge at this point.
    DCHECK_EQ(true_successor->GetPredecessors().Size(), 1u);

    HBasicBlock* false_successor = instruction->IfFalseSuccessor();
    // There should be no critical edge at this point.
    DCHECK_EQ(false_successor->GetPredecessors().Size(), 1u);

    ValueBound bound = DetectValueBoundFromValue(right);
    bool found = !bound.Equals(ValueBound::Max());

    ValueBound lower = bound;
    ValueBound upper = bound;
    if (!found) {
      // No constant or array.length+c bound found.
      // For i<j, we can still use j's upper bound as i's upper bound. Same for lower.
      ValueRange* range = LookupValueRange(right, block);
      if (range != nullptr) {
        lower = range->GetLower();
        upper = range->GetUpper();
      } else {
        lower = ValueBound::Min();
        upper = ValueBound::Max();
      }
    }

    bool overflow_or_underflow;
    if (cond == kCondLT || cond == kCondLE) {
      if (!upper.Equals(ValueBound::Max())) {
        int compensation = (cond == kCondLT) ? -1 : 0;  // upper bound is inclusive
        ValueBound new_upper = upper.Add(compensation, false, &overflow_or_underflow);
        // overflow_or_underflow is ignored here since we already use ValueBound::Min()
        // for lower bound.
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), ValueBound::Min(), new_upper);
        ApplyRangeFromComparison(left, block, true_successor, new_range);
      }

      // array.length as a lower bound isn't considered useful.
      if (!lower.Equals(ValueBound::Min()) && !lower.IsRelativeToArrayLength()) {
        int compensation = (cond == kCondLE) ? 1 : 0;  // lower bound is inclusive
        ValueBound new_lower = lower.Add(compensation, true, &overflow_or_underflow);
        // overflow_or_underflow is ignored here since we already use ValueBound::Max()
        // for upper bound.
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), new_lower, ValueBound::Max());
        ApplyRangeFromComparison(left, block, false_successor, new_range);
      }
    } else if (cond == kCondGT || cond == kCondGE) {
      // array.length as a lower bound isn't considered useful.
      if (!lower.Equals(ValueBound::Min()) && !lower.IsRelativeToArrayLength()) {
        int compensation = (cond == kCondGT) ? 1 : 0;  // lower bound is inclusive
        ValueBound new_lower = lower.Add(compensation, true, &overflow_or_underflow);
        // overflow_or_underflow is ignored here since we already use ValueBound::Max()
        // for upper bound.
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), new_lower, ValueBound::Max());
        ApplyRangeFromComparison(left, block, true_successor, new_range);
      }

      if (!upper.Equals(ValueBound::Max())) {
        int compensation = (cond == kCondGE) ? -1 : 0;  // upper bound is inclusive
        ValueBound new_upper = upper.Add(compensation, false, &overflow_or_underflow);
        // overflow_or_underflow is ignored here since we already use ValueBound::Min()
        // for lower bound.
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), ValueBound::Min(), new_upper);
        ApplyRangeFromComparison(left, block, false_successor, new_range);
      }
    }
  }

  void VisitBoundsCheck(HBoundsCheck* bounds_check) {
    HBasicBlock* block = bounds_check->GetBlock();
    HInstruction* index = bounds_check->InputAt(0);
    HInstruction* array_length = bounds_check->InputAt(1);
    ValueRange* index_range = LookupValueRange(index, block);

    if (index_range != nullptr) {
      ValueBound lower = ValueBound::Create(nullptr, 0);        // constant 0
      ValueBound upper = ValueBound::Create(array_length, -1);  // array_length - 1
      ValueRange* array_range = new (GetGraph()->GetArena())
          ValueRange(GetGraph()->GetArena(), lower, upper);
      if (index_range->FitsIn(array_range)) {
        ReplaceBoundsCheck(bounds_check, index);
        return;
      }
    }

    if (index->IsIntConstant()) {
      ValueRange* array_length_range = LookupValueRange(array_length, block);
      int constant = index->AsIntConstant()->GetValue();
      if (array_length_range != nullptr &&
          array_length_range->GetLower().IsConstant()) {
        if (constant < array_length_range->GetLower().GetConstant()) {
          ReplaceBoundsCheck(bounds_check, index);
          return;
        }
      }

      // Once we have an array access like 'array[5] = 1', we record array.length >= 6.
      ValueBound lower = ValueBound::Create(nullptr, constant + 1);
      ValueBound upper = ValueBound::Max();
      ValueRange* range = new (GetGraph()->GetArena())
          ValueRange(GetGraph()->GetArena(), lower, upper);
      ValueRange* existing_range = LookupValueRange(array_length, block);
      ValueRange* new_range = range;
      if (existing_range != nullptr) {
        new_range = range->Narrow(existing_range);
      }
      GetValueRangeMap(block)->Overwrite(array_length->GetId(), new_range);
    }
  }

  void ReplaceBoundsCheck(HInstruction* bounds_check, HInstruction* index) {
    bounds_check->ReplaceWith(index);
    bounds_check->GetBlock()->RemoveInstruction(bounds_check);
  }

  void VisitPhi(HPhi* phi) {
    if (phi->IsLoopHeaderPhi() && phi->GetType() == Primitive::kPrimInt) {
      DCHECK_EQ(phi->InputCount(), 2U);
      HInstruction* instruction = phi->InputAt(1);
      if (instruction->IsAdd()) {
        HAdd* add = instruction->AsAdd();
        HInstruction* left = add->GetLeft();
        HInstruction* right = add->GetRight();
        if (left == phi && right->IsIntConstant()) {
          HInstruction* initial_value = phi->InputAt(0);
          ValueRange* range = nullptr;
          if (right->AsIntConstant()->GetValue() == 0) {
            // Add constant 0. It's really a fixed value.
            range = new (GetGraph()->GetArena()) ValueRange(
                GetGraph()->GetArena(),
                ValueBound::Create(initial_value, 0),
                ValueBound::Create(initial_value, 0));
          } else {
            // Monotonically increasing/decreasing.
            range = MonotonicValueRange::Create(
                GetGraph()->GetArena(),
                initial_value,
                right->AsIntConstant()->GetValue());
          }
          GetValueRangeMap(phi->GetBlock())->Overwrite(phi->GetId(), range);
        }
      }
    }
  }

  void VisitIf(HIf* instruction) {
    if (instruction->InputAt(0)->IsCondition()) {
      HCondition* cond = instruction->InputAt(0)->AsCondition();
      IfCondition cmp = cond->GetCondition();
      if (cmp == kCondGT || cmp == kCondGE ||
          cmp == kCondLT || cmp == kCondLE) {
        HInstruction* left = cond->GetLeft();
        HInstruction* right = cond->GetRight();
        HandleIf(instruction, left, right, cmp);
      }
    }
  }

  void VisitAdd(HAdd* add) {
    HInstruction* right = add->GetRight();
    if (right->IsIntConstant()) {
      ValueRange* left_range = LookupValueRange(add->GetLeft(), add->GetBlock());
      if (left_range == nullptr) {
        return;
      }
      ValueRange* range = left_range->Add(right->AsIntConstant()->GetValue());
      if (range != nullptr) {
        GetValueRangeMap(add->GetBlock())->Overwrite(add->GetId(), range);
      }
    }
  }

  void VisitSub(HSub* sub) {
    HInstruction* left = sub->GetLeft();
    HInstruction* right = sub->GetRight();
    if (right->IsIntConstant()) {
      ValueRange* left_range = LookupValueRange(left, sub->GetBlock());
      if (left_range == nullptr) {
        return;
      }
      ValueRange* range = left_range->Add(-right->AsIntConstant()->GetValue());
      if (range != nullptr) {
        GetValueRangeMap(sub->GetBlock())->Overwrite(sub->GetId(), range);
        return;
      }
    }

    // Here we are interested in the typical triangular case of nested loops,
    // such as the inner loop 'for (int j=0; j<array.length-i; j++)' where i
    // is the index for outer loop. In this case, we know j is bounded by array.length-1.
    if (left->IsArrayLength()) {
      HInstruction* array_length = left->AsArrayLength();
      ValueRange* right_range = LookupValueRange(right, sub->GetBlock());
      if (right_range != nullptr) {
        ValueBound lower = right_range->GetLower();
        ValueBound upper = right_range->GetUpper();
        if (lower.IsConstant() && upper.IsRelativeToArrayLength()) {
          HInstruction* upper_inst = upper.GetInstruction();
          if (upper_inst->IsArrayLength() &&
              upper_inst->AsArrayLength() == array_length) {
            // (array.length - v) where v is in [c1, array.length + c2]
            // gets [-c2, array.length - c1] as its value range.
            ValueRange* range = new (GetGraph()->GetArena()) ValueRange(
                GetGraph()->GetArena(),
                ValueBound::Create(nullptr, - upper.GetConstant()),
                ValueBound::Create(array_length, - lower.GetConstant()));
            GetValueRangeMap(sub->GetBlock())->Overwrite(sub->GetId(), range);
          }
        }
      }
    }
  }

  std::vector<std::unique_ptr<ArenaSafeMap<int, ValueRange*>>> maps_;

  DISALLOW_COPY_AND_ASSIGN(BCEVisitor);
};

void BoundsCheckElimination::Run() {
  BCEVisitor visitor(graph_);
  // Reverse post order guarantees a node's dominators are visited first.
  // We want to visit in the dominator-based order since if a value is known to
  // be bounded by a range at one instruction, it must be true that all uses of
  // that value dominated by that instruction fits in that range. Range of that
  // value can be narrowed further down in the dominator tree.
  //
  // TODO: only visit blocks that dominate some array accesses.
  visitor.VisitReversePostOrder();
}

}  // namespace art
