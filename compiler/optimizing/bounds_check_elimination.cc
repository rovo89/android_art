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

#include "base/arena_containers.h"
#include "bounds_check_elimination.h"
#include "nodes.h"

namespace art {

class MonotonicValueRange;

/**
 * A value bound is represented as a pair of value and constant,
 * e.g. array.length - 1.
 */
class ValueBound : public ValueObject {
 public:
  ValueBound(HInstruction* instruction, int32_t constant) {
    if (instruction != nullptr && instruction->IsIntConstant()) {
      // Normalize ValueBound with constant instruction.
      int32_t instr_const = instruction->AsIntConstant()->GetValue();
      if (!WouldAddOverflowOrUnderflow(instr_const, constant)) {
        instruction_ = nullptr;
        constant_ = instr_const + constant;
        return;
      }
    }
    instruction_ = instruction;
    constant_ = constant;
  }

  // Return whether (left + right) overflows or underflows.
  static bool WouldAddOverflowOrUnderflow(int32_t left, int32_t right) {
    if (right == 0) {
      return false;
    }
    if ((right > 0) && (left <= INT_MAX - right)) {
      // No overflow.
      return false;
    }
    if ((right < 0) && (left >= INT_MIN - right)) {
      // No underflow.
      return false;
    }
    return true;
  }

  static bool IsAddOrSubAConstant(HInstruction* instruction,
                                  HInstruction** left_instruction,
                                  int* right_constant) {
    if (instruction->IsAdd() || instruction->IsSub()) {
      HBinaryOperation* bin_op = instruction->AsBinaryOperation();
      HInstruction* left = bin_op->GetLeft();
      HInstruction* right = bin_op->GetRight();
      if (right->IsIntConstant()) {
        *left_instruction = left;
        int32_t c = right->AsIntConstant()->GetValue();
        *right_constant = instruction->IsAdd() ? c : -c;
        return true;
      }
    }
    *left_instruction = nullptr;
    *right_constant = 0;
    return false;
  }

  // Try to detect useful value bound format from an instruction, e.g.
  // a constant or array length related value.
  static ValueBound DetectValueBoundFromValue(HInstruction* instruction, bool* found) {
    DCHECK(instruction != nullptr);
    if (instruction->IsIntConstant()) {
      *found = true;
      return ValueBound(nullptr, instruction->AsIntConstant()->GetValue());
    }

    if (instruction->IsArrayLength()) {
      *found = true;
      return ValueBound(instruction, 0);
    }
    // Try to detect (array.length + c) format.
    HInstruction *left;
    int32_t right;
    if (IsAddOrSubAConstant(instruction, &left, &right)) {
      if (left->IsArrayLength()) {
        *found = true;
        return ValueBound(left, right);
      }
    }

    // No useful bound detected.
    *found = false;
    return ValueBound::Max();
  }

  HInstruction* GetInstruction() const { return instruction_; }
  int32_t GetConstant() const { return constant_; }

  bool IsRelatedToArrayLength() const {
    // Some bounds are created with HNewArray* as the instruction instead
    // of HArrayLength*. They are treated the same.
    return (instruction_ != nullptr) &&
           (instruction_->IsArrayLength() || instruction_->IsNewArray());
  }

  bool IsConstant() const {
    return instruction_ == nullptr;
  }

  static ValueBound Min() { return ValueBound(nullptr, INT_MIN); }
  static ValueBound Max() { return ValueBound(nullptr, INT_MAX); }

  bool Equals(ValueBound bound) const {
    return instruction_ == bound.instruction_ && constant_ == bound.constant_;
  }

  static HInstruction* FromArrayLengthToArray(HInstruction* instruction) {
    DCHECK(instruction->IsArrayLength() || instruction->IsNewArray());
    if (instruction->IsArrayLength()) {
      HInstruction* input = instruction->InputAt(0);
      if (input->IsNullCheck()) {
        input = input->AsNullCheck()->InputAt(0);
      }
      return input;
    }
    return instruction;
  }

  static bool Equal(HInstruction* instruction1, HInstruction* instruction2) {
    if (instruction1 == instruction2) {
      return true;
    }

    if (instruction1 == nullptr || instruction2 == nullptr) {
      return false;
    }

    // Some bounds are created with HNewArray* as the instruction instead
    // of HArrayLength*. They are treated the same.
    // HArrayLength with the same array input are considered equal also.
    instruction1 = FromArrayLengthToArray(instruction1);
    instruction2 = FromArrayLengthToArray(instruction2);
    return instruction1 == instruction2;
  }

  // Returns if it's certain this->bound >= `bound`.
  bool GreaterThanOrEqualTo(ValueBound bound) const {
    if (Equal(instruction_, bound.instruction_)) {
      return constant_ >= bound.constant_;
    }
    // Not comparable. Just return false.
    return false;
  }

  // Returns if it's certain this->bound <= `bound`.
  bool LessThanOrEqualTo(ValueBound bound) const {
    if (Equal(instruction_, bound.instruction_)) {
      return constant_ <= bound.constant_;
    }
    // Not comparable. Just return false.
    return false;
  }

  // Try to narrow lower bound. Returns the greatest of the two if possible.
  // Pick one if they are not comparable.
  static ValueBound NarrowLowerBound(ValueBound bound1, ValueBound bound2) {
    if (bound1.GreaterThanOrEqualTo(bound2)) {
      return bound1;
    }
    if (bound2.GreaterThanOrEqualTo(bound1)) {
      return bound2;
    }

    // Not comparable. Just pick one. We may lose some info, but that's ok.
    // Favor constant as lower bound.
    return bound1.IsConstant() ? bound1 : bound2;
  }

  // Try to narrow upper bound. Returns the lowest of the two if possible.
  // Pick one if they are not comparable.
  static ValueBound NarrowUpperBound(ValueBound bound1, ValueBound bound2) {
    if (bound1.LessThanOrEqualTo(bound2)) {
      return bound1;
    }
    if (bound2.LessThanOrEqualTo(bound1)) {
      return bound2;
    }

    // Not comparable. Just pick one. We may lose some info, but that's ok.
    // Favor array length as upper bound.
    return bound1.IsRelatedToArrayLength() ? bound1 : bound2;
  }

  // Add a constant to a ValueBound.
  // `overflow` or `underflow` will return whether the resulting bound may
  // overflow or underflow an int.
  ValueBound Add(int32_t c, bool* overflow, bool* underflow) const {
    *overflow = *underflow = false;
    if (c == 0) {
      return *this;
    }

    int32_t new_constant;
    if (c > 0) {
      if (constant_ > INT_MAX - c) {
        *overflow = true;
        return Max();
      }

      new_constant = constant_ + c;
      // (array.length + non-positive-constant) won't overflow an int.
      if (IsConstant() || (IsRelatedToArrayLength() && new_constant <= 0)) {
        return ValueBound(instruction_, new_constant);
      }
      // Be conservative.
      *overflow = true;
      return Max();
    } else {
      if (constant_ < INT_MIN - c) {
        *underflow = true;
        return Min();
      }

      new_constant = constant_ + c;
      // Regardless of the value new_constant, (array.length+new_constant) will
      // never underflow since array.length is no less than 0.
      if (IsConstant() || IsRelatedToArrayLength()) {
        return ValueBound(instruction_, new_constant);
      }
      // Be conservative.
      *underflow = true;
      return Min();
    }
  }

 private:
  HInstruction* instruction_;
  int32_t constant_;
};

// Collect array access data for a loop.
// TODO: make it work for multiple arrays inside the loop.
class ArrayAccessInsideLoopFinder : public ValueObject {
 public:
  explicit ArrayAccessInsideLoopFinder(HInstruction* induction_variable)
      : induction_variable_(induction_variable),
        found_array_length_(nullptr),
        offset_low_(INT_MAX),
        offset_high_(INT_MIN) {
    Run();
  }

  HArrayLength* GetFoundArrayLength() const { return found_array_length_; }
  bool HasFoundArrayLength() const { return found_array_length_ != nullptr; }
  int32_t GetOffsetLow() const { return offset_low_; }
  int32_t GetOffsetHigh() const { return offset_high_; }

  // Returns if `block` that is in loop_info may exit the loop, unless it's
  // the loop header for loop_info.
  static bool EarlyExit(HBasicBlock* block, HLoopInformation* loop_info) {
    DCHECK(loop_info->Contains(*block));
    if (block == loop_info->GetHeader()) {
      // Loop header of loop_info. Exiting loop is normal.
      return false;
    }
    const GrowableArray<HBasicBlock*>& successors = block->GetSuccessors();
    for (size_t i = 0; i < successors.Size(); i++) {
      if (!loop_info->Contains(*successors.Get(i))) {
        // One of the successors exits the loop.
        return true;
      }
    }
    return false;
  }

  static bool DominatesAllBackEdges(HBasicBlock* block, HLoopInformation* loop_info) {
    for (size_t i = 0, e = loop_info->GetBackEdges().Size(); i < e; ++i) {
      HBasicBlock* back_edge = loop_info->GetBackEdges().Get(i);
      if (!block->Dominates(back_edge)) {
        return false;
      }
    }
    return true;
  }

  void Run() {
    HLoopInformation* loop_info = induction_variable_->GetBlock()->GetLoopInformation();
    HBlocksInLoopReversePostOrderIterator it_loop(*loop_info);
    HBasicBlock* block = it_loop.Current();
    DCHECK(block == induction_variable_->GetBlock());
    // Skip loop header. Since narrowed value range of a MonotonicValueRange only
    // applies to the loop body (after the test at the end of the loop header).
    it_loop.Advance();
    for (; !it_loop.Done(); it_loop.Advance()) {
      block = it_loop.Current();
      DCHECK(block->IsInLoop());
      if (!DominatesAllBackEdges(block, loop_info)) {
        // In order not to trigger deoptimization unnecessarily, make sure
        // that all array accesses collected are really executed in the loop.
        // For array accesses in a branch inside the loop, don't collect the
        // access. The bounds check in that branch might not be eliminated.
        continue;
      }
      if (EarlyExit(block, loop_info)) {
        // If the loop body can exit loop (like break, return, etc.), it's not guaranteed
        // that the loop will loop through the full monotonic value range from
        // initial_ to end_. So adding deoptimization might be too aggressive and can
        // trigger deoptimization unnecessarily even if the loop won't actually throw
        // AIOOBE.
        found_array_length_ = nullptr;
        return;
      }
      for (HInstruction* instruction = block->GetFirstInstruction();
           instruction != nullptr;
           instruction = instruction->GetNext()) {
        if (!instruction->IsBoundsCheck()) {
          continue;
        }

        HInstruction* length_value = instruction->InputAt(1);
        if (length_value->IsIntConstant()) {
          // TODO: may optimize for constant case.
          continue;
        }

        if (length_value->IsPhi()) {
          // When adding deoptimizations in outer loops, we might create
          // a phi for the array length, and update all uses of the
          // length in the loop to that phi. Therefore, inner loops having
          // bounds checks on the same array will use that phi.
          // TODO: handle these cases.
          continue;
        }

        DCHECK(length_value->IsArrayLength());
        HArrayLength* array_length = length_value->AsArrayLength();

        HInstruction* array = array_length->InputAt(0);
        if (array->IsNullCheck()) {
          array = array->AsNullCheck()->InputAt(0);
        }
        if (loop_info->Contains(*array->GetBlock())) {
          // Array is defined inside the loop. Skip.
          continue;
        }

        if (found_array_length_ != nullptr && found_array_length_ != array_length) {
          // There is already access for another array recorded for the loop.
          // TODO: handle multiple arrays.
          continue;
        }

        HInstruction* index = instruction->AsBoundsCheck()->InputAt(0);
        HInstruction* left = index;
        int32_t right = 0;
        if (left == induction_variable_ ||
            (ValueBound::IsAddOrSubAConstant(index, &left, &right) &&
             left == induction_variable_)) {
          // For patterns like array[i] or array[i + 2].
          if (right < offset_low_) {
            offset_low_ = right;
          }
          if (right > offset_high_) {
            offset_high_ = right;
          }
        } else {
          // Access not in induction_variable/(induction_variable_ + constant)
          // format. Skip.
          continue;
        }
        // Record this array.
        found_array_length_ = array_length;
      }
    }
  }

 private:
  // The instruction that corresponds to a MonotonicValueRange.
  HInstruction* induction_variable_;

  // The array length of the array that's accessed inside the loop body.
  HArrayLength* found_array_length_;

  // The lowest and highest constant offsets relative to induction variable
  // instruction_ in all array accesses.
  // If array access are: array[i-1], array[i], array[i+1],
  // offset_low_ is -1 and offset_high is 1.
  int32_t offset_low_;
  int32_t offset_high_;

  DISALLOW_COPY_AND_ASSIGN(ArrayAccessInsideLoopFinder);
};

/**
 * Represent a range of lower bound and upper bound, both being inclusive.
 * Currently a ValueRange may be generated as a result of the following:
 * comparisons related to array bounds, array bounds check, add/sub on top
 * of an existing value range, NewArray or a loop phi corresponding to an
 * incrementing/decrementing array index (MonotonicValueRange).
 */
class ValueRange : public ArenaObject<kArenaAllocMisc> {
 public:
  ValueRange(ArenaAllocator* allocator, ValueBound lower, ValueBound upper)
      : allocator_(allocator), lower_(lower), upper_(upper) {}

  virtual ~ValueRange() {}

  virtual MonotonicValueRange* AsMonotonicValueRange() { return nullptr; }
  bool IsMonotonicValueRange() {
    return AsMonotonicValueRange() != nullptr;
  }

  ArenaAllocator* GetAllocator() const { return allocator_; }
  ValueBound GetLower() const { return lower_; }
  ValueBound GetUpper() const { return upper_; }

  bool IsConstantValueRange() { return lower_.IsConstant() && upper_.IsConstant(); }

  // If it's certain that this value range fits in other_range.
  virtual bool FitsIn(ValueRange* other_range) const {
    if (other_range == nullptr) {
      return true;
    }
    DCHECK(!other_range->IsMonotonicValueRange());
    return lower_.GreaterThanOrEqualTo(other_range->lower_) &&
           upper_.LessThanOrEqualTo(other_range->upper_);
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

  // Shift a range by a constant.
  ValueRange* Add(int32_t constant) const {
    bool overflow, underflow;
    ValueBound lower = lower_.Add(constant, &overflow, &underflow);
    if (underflow) {
      // Lower bound underflow will wrap around to positive values
      // and invalidate the upper bound.
      return nullptr;
    }
    ValueBound upper = upper_.Add(constant, &overflow, &underflow);
    if (overflow) {
      // Upper bound overflow will wrap around to negative values
      // and invalidate the lower bound.
      return nullptr;
    }
    return new (allocator_) ValueRange(allocator_, lower, upper);
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
  MonotonicValueRange(ArenaAllocator* allocator,
                      HPhi* induction_variable,
                      HInstruction* initial,
                      int32_t increment,
                      ValueBound bound)
      // To be conservative, give it full range [INT_MIN, INT_MAX] in case it's
      // used as a regular value range, due to possible overflow/underflow.
      : ValueRange(allocator, ValueBound::Min(), ValueBound::Max()),
        induction_variable_(induction_variable),
        initial_(initial),
        end_(nullptr),
        inclusive_(false),
        increment_(increment),
        bound_(bound) {}

  virtual ~MonotonicValueRange() {}

  HInstruction* GetInductionVariable() const { return induction_variable_; }
  int32_t GetIncrement() const { return increment_; }
  ValueBound GetBound() const { return bound_; }
  void SetEnd(HInstruction* end) { end_ = end; }
  void SetInclusive(bool inclusive) { inclusive_ = inclusive; }
  HBasicBlock* GetLoopHeader() const {
    DCHECK(induction_variable_->GetBlock()->IsLoopHeader());
    return induction_variable_->GetBlock();
  }

  MonotonicValueRange* AsMonotonicValueRange() OVERRIDE { return this; }

  HBasicBlock* GetLoopHeaderSuccesorInLoop() {
    HBasicBlock* header = GetLoopHeader();
    HInstruction* instruction = header->GetLastInstruction();
    DCHECK(instruction->IsIf());
    HIf* h_if = instruction->AsIf();
    HLoopInformation* loop_info = header->GetLoopInformation();
    bool true_successor_in_loop = loop_info->Contains(*h_if->IfTrueSuccessor());
    bool false_successor_in_loop = loop_info->Contains(*h_if->IfFalseSuccessor());

    // Just in case it's some strange loop structure.
    if (true_successor_in_loop && false_successor_in_loop) {
      return nullptr;
    }
    DCHECK(true_successor_in_loop || false_successor_in_loop);
    return false_successor_in_loop ? h_if->IfFalseSuccessor() : h_if->IfTrueSuccessor();
  }

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
      ValueBound lower = ValueBound::NarrowLowerBound(bound_, range->GetLower());
      if (!lower.IsConstant() || lower.GetConstant() == INT_MIN) {
        // Lower bound isn't useful. Leave it to deoptimization.
        return this;
      }

      // We currently conservatively assume max array length is INT_MAX. If we can
      // make assumptions about the max array length, e.g. due to the max heap size,
      // divided by the element size (such as 4 bytes for each integer array), we can
      // lower this number and rule out some possible overflows.
      int32_t max_array_len = INT_MAX;

      // max possible integer value of range's upper value.
      int32_t upper = INT_MAX;
      // Try to lower upper.
      ValueBound upper_bound = range->GetUpper();
      if (upper_bound.IsConstant()) {
        upper = upper_bound.GetConstant();
      } else if (upper_bound.IsRelatedToArrayLength() && upper_bound.GetConstant() <= 0) {
        // Normal case. e.g. <= array.length - 1.
        upper = max_array_len + upper_bound.GetConstant();
      }

      // If we can prove for the last number in sequence of initial_,
      // initial_ + increment_, initial_ + 2 x increment_, ...
      // that's <= upper, (last_num_in_sequence + increment_) doesn't trigger overflow,
      // then this MonoticValueRange is narrowed to a normal value range.

      // Be conservative first, assume last number in the sequence hits upper.
      int32_t last_num_in_sequence = upper;
      if (initial_->IsIntConstant()) {
        int32_t initial_constant = initial_->AsIntConstant()->GetValue();
        if (upper <= initial_constant) {
          last_num_in_sequence = upper;
        } else {
          // Cast to int64_t for the substraction part to avoid int32_t overflow.
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
      ValueBound upper = ValueBound::NarrowUpperBound(bound_, range->GetUpper());
      if ((!upper.IsConstant() || upper.GetConstant() == INT_MAX) &&
          !upper.IsRelatedToArrayLength()) {
        // Upper bound isn't useful. Leave it to deoptimization.
        return this;
      }

      // Need to take care of underflow. Try to prove underflow won't happen
      // for common cases.
      if (range->GetLower().IsConstant()) {
        int32_t constant = range->GetLower().GetConstant();
        if (constant >= INT_MIN - increment_) {
          return new (GetAllocator()) ValueRange(GetAllocator(), range->GetLower(), upper);
        }
      }

      // For non-constant lower bound, just assume might be underflow. Give up narrowing.
      return this;
    }
  }

  // Try to add HDeoptimize's in the loop pre-header first to narrow this range.
  // For example, this loop:
  //
  //   for (int i = start; i < end; i++) {
  //     array[i - 1] = array[i] + array[i + 1];
  //   }
  //
  // will be transformed to:
  //
  //   int array_length_in_loop_body_if_needed;
  //   if (start >= end) {
  //     array_length_in_loop_body_if_needed = 0;
  //   } else {
  //     if (start < 1) deoptimize();
  //     if (array == null) deoptimize();
  //     array_length = array.length;
  //     if (end > array_length - 1) deoptimize;
  //     array_length_in_loop_body_if_needed = array_length;
  //   }
  //   for (int i = start; i < end; i++) {
  //     // No more null check and bounds check.
  //     // array.length value is replaced with array_length_in_loop_body_if_needed
  //     // in the loop body.
  //     array[i - 1] = array[i] + array[i + 1];
  //   }
  //
  // We basically first go through the loop body and find those array accesses whose
  // index is at a constant offset from the induction variable ('i' in the above example),
  // and update offset_low and offset_high along the way. We then add the following
  // deoptimizations in the loop pre-header (suppose end is not inclusive).
  //   if (start < -offset_low) deoptimize();
  //   if (end >= array.length - offset_high) deoptimize();
  // It might be necessary to first hoist array.length (and the null check on it) out of
  // the loop with another deoptimization.
  //
  // In order not to trigger deoptimization unnecessarily, we want to make a strong
  // guarantee that no deoptimization is triggered if the loop body itself doesn't
  // throw AIOOBE. (It's the same as saying if deoptimization is triggered, the loop
  // body must throw AIOOBE).
  // This is achieved by the following:
  // 1) We only process loops that iterate through the full monotonic range from
  //    initial_ to end_. We do the following checks to make sure that's the case:
  //    a) The loop doesn't have early exit (via break, return, etc.)
  //    b) The increment_ is 1/-1. An increment of 2, for example, may skip end_.
  // 2) We only collect array accesses of blocks in the loop body that dominate
  //    all loop back edges, these array accesses are guaranteed to happen
  //    at each loop iteration.
  // With 1) and 2), if the loop body doesn't throw AIOOBE, collected array accesses
  // when the induction variable is at initial_ and end_ must be in a legal range.
  // Since the added deoptimizations are basically checking the induction variable
  // at initial_ and end_ values, no deoptimization will be triggered either.
  //
  // A special case is the loop body isn't entered at all. In that case, we may still
  // add deoptimization due to the analysis described above. In order not to trigger
  // deoptimization, we do a test between initial_ and end_ first and skip over
  // the added deoptimization.
  ValueRange* NarrowWithDeoptimization() {
    if (increment_ != 1 && increment_ != -1) {
      // In order not to trigger deoptimization unnecessarily, we want to
      // make sure the loop iterates through the full range from initial_ to
      // end_ so that boundaries are covered by the loop. An increment of 2,
      // for example, may skip end_.
      return this;
    }

    if (end_ == nullptr) {
      // No full info to add deoptimization.
      return this;
    }

    HBasicBlock* header = induction_variable_->GetBlock();
    DCHECK(header->IsLoopHeader());
    HBasicBlock* pre_header = header->GetLoopInformation()->GetPreHeader();
    if (!initial_->GetBlock()->Dominates(pre_header) ||
        !end_->GetBlock()->Dominates(pre_header)) {
      // Can't add a check in loop pre-header if the value isn't available there.
      return this;
    }

    ArrayAccessInsideLoopFinder finder(induction_variable_);

    if (!finder.HasFoundArrayLength()) {
      // No array access was found inside the loop that can benefit
      // from deoptimization.
      return this;
    }

    if (!AddDeoptimization(finder)) {
      return this;
    }

    // After added deoptimizations, induction variable fits in
    // [-offset_low, array.length-1-offset_high], adjusted with collected offsets.
    ValueBound lower = ValueBound(0, -finder.GetOffsetLow());
    ValueBound upper = ValueBound(finder.GetFoundArrayLength(), -1 - finder.GetOffsetHigh());
    // We've narrowed the range after added deoptimizations.
    return new (GetAllocator()) ValueRange(GetAllocator(), lower, upper);
  }

  // Returns true if adding a (constant >= value) check for deoptimization
  // is allowed and will benefit compiled code.
  bool CanAddDeoptimizationConstant(HInstruction* value, int32_t constant, bool* is_proven) {
    *is_proven = false;
    HBasicBlock* header = induction_variable_->GetBlock();
    DCHECK(header->IsLoopHeader());
    HBasicBlock* pre_header = header->GetLoopInformation()->GetPreHeader();
    DCHECK(value->GetBlock()->Dominates(pre_header));

    // See if we can prove the relationship first.
    if (value->IsIntConstant()) {
      if (value->AsIntConstant()->GetValue() >= constant) {
        // Already true.
        *is_proven = true;
        return true;
      } else {
        // May throw exception. Don't add deoptimization.
        // Keep bounds checks in the loops.
        return false;
      }
    }
    // Can benefit from deoptimization.
    return true;
  }

  // Try to filter out cases that the loop entry test will never be true.
  bool LoopEntryTestUseful() {
    if (initial_->IsIntConstant() && end_->IsIntConstant()) {
      int32_t initial_val = initial_->AsIntConstant()->GetValue();
      int32_t end_val = end_->AsIntConstant()->GetValue();
      if (increment_ == 1) {
        if (inclusive_) {
          return initial_val > end_val;
        } else {
          return initial_val >= end_val;
        }
      } else {
        DCHECK_EQ(increment_, -1);
        if (inclusive_) {
          return initial_val < end_val;
        } else {
          return initial_val <= end_val;
        }
      }
    }
    return true;
  }

  // Returns the block for adding deoptimization.
  HBasicBlock* TransformLoopForDeoptimizationIfNeeded() {
    HBasicBlock* header = induction_variable_->GetBlock();
    DCHECK(header->IsLoopHeader());
    HBasicBlock* pre_header = header->GetLoopInformation()->GetPreHeader();
    // Deoptimization is only added when both initial_ and end_ are defined
    // before the loop.
    DCHECK(initial_->GetBlock()->Dominates(pre_header));
    DCHECK(end_->GetBlock()->Dominates(pre_header));

    // If it can be proven the loop body is definitely entered (unless exception
    // is thrown in the loop header for which triggering deoptimization is fine),
    // there is no need for tranforming the loop. In that case, deoptimization
    // will just be added in the loop pre-header.
    if (!LoopEntryTestUseful()) {
      return pre_header;
    }

    HGraph* graph = header->GetGraph();
    graph->TransformLoopHeaderForBCE(header);
    HBasicBlock* new_pre_header = header->GetDominator();
    DCHECK(new_pre_header == header->GetLoopInformation()->GetPreHeader());
    HBasicBlock* if_block = new_pre_header->GetDominator();
    HBasicBlock* dummy_block = if_block->GetSuccessors().Get(0);  // True successor.
    HBasicBlock* deopt_block = if_block->GetSuccessors().Get(1);  // False successor.

    dummy_block->AddInstruction(new (graph->GetArena()) HGoto());
    deopt_block->AddInstruction(new (graph->GetArena()) HGoto());
    new_pre_header->AddInstruction(new (graph->GetArena()) HGoto());
    return deopt_block;
  }

  // Adds a test between initial_ and end_ to see if the loop body is entered.
  // If the loop body isn't entered at all, it jumps to the loop pre-header (after
  // transformation) to avoid any deoptimization.
  void AddLoopBodyEntryTest() {
    HBasicBlock* header = induction_variable_->GetBlock();
    DCHECK(header->IsLoopHeader());
    HBasicBlock* pre_header = header->GetLoopInformation()->GetPreHeader();
    HBasicBlock* if_block = pre_header->GetDominator();
    HGraph* graph = header->GetGraph();

    HCondition* cond;
    if (increment_ == 1) {
      if (inclusive_) {
        cond = new (graph->GetArena()) HGreaterThan(initial_, end_);
      } else {
        cond = new (graph->GetArena()) HGreaterThanOrEqual(initial_, end_);
      }
    } else {
      DCHECK_EQ(increment_, -1);
      if (inclusive_) {
        cond = new (graph->GetArena()) HLessThan(initial_, end_);
      } else {
        cond = new (graph->GetArena()) HLessThanOrEqual(initial_, end_);
      }
    }
    HIf* h_if = new (graph->GetArena()) HIf(cond);
    if_block->AddInstruction(cond);
    if_block->AddInstruction(h_if);
  }

  // Adds a check that (value >= constant), and HDeoptimize otherwise.
  void AddDeoptimizationConstant(HInstruction* value,
                                 int32_t constant,
                                 HBasicBlock* deopt_block,
                                 bool loop_entry_test_block_added) {
    HBasicBlock* header = induction_variable_->GetBlock();
    DCHECK(header->IsLoopHeader());
    HBasicBlock* pre_header = header->GetDominator();
    if (loop_entry_test_block_added) {
      DCHECK(deopt_block->GetSuccessors().Get(0) == pre_header);
    } else {
      DCHECK(deopt_block == pre_header);
    }
    HGraph* graph = header->GetGraph();
    HSuspendCheck* suspend_check = header->GetLoopInformation()->GetSuspendCheck();
    if (loop_entry_test_block_added) {
      DCHECK_EQ(deopt_block, header->GetDominator()->GetDominator()->GetSuccessors().Get(1));
    }

    HIntConstant* const_instr = graph->GetIntConstant(constant);
    HCondition* cond = new (graph->GetArena()) HLessThan(value, const_instr);
    HDeoptimize* deoptimize = new (graph->GetArena())
        HDeoptimize(cond, suspend_check->GetDexPc());
    deopt_block->InsertInstructionBefore(cond, deopt_block->GetLastInstruction());
    deopt_block->InsertInstructionBefore(deoptimize, deopt_block->GetLastInstruction());
    deoptimize->CopyEnvironmentFromWithLoopPhiAdjustment(
        suspend_check->GetEnvironment(), header);
  }

  // Returns true if adding a (value <= array_length + offset) check for deoptimization
  // is allowed and will benefit compiled code.
  bool CanAddDeoptimizationArrayLength(HInstruction* value,
                                       HArrayLength* array_length,
                                       int32_t offset,
                                       bool* is_proven) {
    *is_proven = false;
    HBasicBlock* header = induction_variable_->GetBlock();
    DCHECK(header->IsLoopHeader());
    HBasicBlock* pre_header = header->GetLoopInformation()->GetPreHeader();
    DCHECK(value->GetBlock()->Dominates(pre_header));

    if (array_length->GetBlock() == header) {
      // array_length_in_loop_body_if_needed only has correct value when the loop
      // body is entered. We bail out in this case. Usually array_length defined
      // in the loop header is already hoisted by licm.
      return false;
    } else {
      // array_length is defined either before the loop header already, or in
      // the loop body since it's used in the loop body. If it's defined in the loop body,
      // a phi array_length_in_loop_body_if_needed is used to replace it. In that case,
      // all the uses of array_length must be dominated by its definition in the loop
      // body. array_length_in_loop_body_if_needed is guaranteed to be the same as
      // array_length once the loop body is entered so all the uses of the phi will
      // use the correct value.
    }

    if (offset > 0) {
      // There might be overflow issue.
      // TODO: handle this, possibly with some distance relationship between
      // offset_low and offset_high, or using another deoptimization to make
      // sure (array_length + offset) doesn't overflow.
      return false;
    }

    // See if we can prove the relationship first.
    if (value == array_length) {
      if (offset >= 0) {
        // Already true.
        *is_proven = true;
        return true;
      } else {
        // May throw exception. Don't add deoptimization.
        // Keep bounds checks in the loops.
        return false;
      }
    }
    // Can benefit from deoptimization.
    return true;
  }

  // Adds a check that (value <= array_length + offset), and HDeoptimize otherwise.
  void AddDeoptimizationArrayLength(HInstruction* value,
                                    HArrayLength* array_length,
                                    int32_t offset,
                                    HBasicBlock* deopt_block,
                                    bool loop_entry_test_block_added) {
    HBasicBlock* header = induction_variable_->GetBlock();
    DCHECK(header->IsLoopHeader());
    HBasicBlock* pre_header = header->GetDominator();
    if (loop_entry_test_block_added) {
      DCHECK(deopt_block->GetSuccessors().Get(0) == pre_header);
    } else {
      DCHECK(deopt_block == pre_header);
    }
    HGraph* graph = header->GetGraph();
    HSuspendCheck* suspend_check = header->GetLoopInformation()->GetSuspendCheck();

    // We may need to hoist null-check and array_length out of loop first.
    if (!array_length->GetBlock()->Dominates(deopt_block)) {
      // array_length must be defined in the loop body.
      DCHECK(header->GetLoopInformation()->Contains(*array_length->GetBlock()));
      DCHECK(array_length->GetBlock() != header);

      HInstruction* array = array_length->InputAt(0);
      HNullCheck* null_check = array->AsNullCheck();
      if (null_check != nullptr) {
        array = null_check->InputAt(0);
      }
      // We've already made sure the array is defined before the loop when collecting
      // array accesses for the loop.
      DCHECK(array->GetBlock()->Dominates(deopt_block));
      if (null_check != nullptr && !null_check->GetBlock()->Dominates(deopt_block)) {
        // Hoist null check out of loop with a deoptimization.
        HNullConstant* null_constant = graph->GetNullConstant();
        HCondition* null_check_cond = new (graph->GetArena()) HEqual(array, null_constant);
        // TODO: for one dex_pc, share the same deoptimization slow path.
        HDeoptimize* null_check_deoptimize = new (graph->GetArena())
            HDeoptimize(null_check_cond, suspend_check->GetDexPc());
        deopt_block->InsertInstructionBefore(
            null_check_cond, deopt_block->GetLastInstruction());
        deopt_block->InsertInstructionBefore(
            null_check_deoptimize, deopt_block->GetLastInstruction());
        // Eliminate null check in the loop.
        null_check->ReplaceWith(array);
        null_check->GetBlock()->RemoveInstruction(null_check);
        null_check_deoptimize->CopyEnvironmentFromWithLoopPhiAdjustment(
            suspend_check->GetEnvironment(), header);
      }

      HArrayLength* new_array_length = new (graph->GetArena()) HArrayLength(array);
      deopt_block->InsertInstructionBefore(new_array_length, deopt_block->GetLastInstruction());

      if (loop_entry_test_block_added) {
        // Replace array_length defined inside the loop body with a phi
        // array_length_in_loop_body_if_needed. This is a synthetic phi so there is
        // no vreg number for it.
        HPhi* phi = new (graph->GetArena()) HPhi(
            graph->GetArena(), kNoRegNumber, 2, Primitive::kPrimInt);
        // Set to 0 if the loop body isn't entered.
        phi->SetRawInputAt(0, graph->GetIntConstant(0));
        // Set to array.length if the loop body is entered.
        phi->SetRawInputAt(1, new_array_length);
        pre_header->AddPhi(phi);
        array_length->ReplaceWith(phi);
        // Make sure phi is only used after the loop body is entered.
        if (kIsDebugBuild) {
          for (HUseIterator<HInstruction*> it(phi->GetUses());
               !it.Done();
               it.Advance()) {
            HInstruction* user = it.Current()->GetUser();
            DCHECK(GetLoopHeaderSuccesorInLoop()->Dominates(user->GetBlock()));
          }
        }
      } else {
        array_length->ReplaceWith(new_array_length);
      }

      array_length->GetBlock()->RemoveInstruction(array_length);
      // Use new_array_length for deopt.
      array_length = new_array_length;
    }

    HInstruction* added = array_length;
    if (offset != 0) {
      HIntConstant* offset_instr = graph->GetIntConstant(offset);
      added = new (graph->GetArena()) HAdd(Primitive::kPrimInt, array_length, offset_instr);
      deopt_block->InsertInstructionBefore(added, deopt_block->GetLastInstruction());
    }
    HCondition* cond = new (graph->GetArena()) HGreaterThan(value, added);
    HDeoptimize* deopt = new (graph->GetArena()) HDeoptimize(cond, suspend_check->GetDexPc());
    deopt_block->InsertInstructionBefore(cond, deopt_block->GetLastInstruction());
    deopt_block->InsertInstructionBefore(deopt, deopt_block->GetLastInstruction());
    deopt->CopyEnvironmentFromWithLoopPhiAdjustment(suspend_check->GetEnvironment(), header);
  }

  // Adds deoptimizations in loop pre-header with the collected array access
  // data so that value ranges can be established in loop body.
  // Returns true if deoptimizations are successfully added, or if it's proven
  // it's not necessary.
  bool AddDeoptimization(const ArrayAccessInsideLoopFinder& finder) {
    int32_t offset_low = finder.GetOffsetLow();
    int32_t offset_high = finder.GetOffsetHigh();
    HArrayLength* array_length = finder.GetFoundArrayLength();

    HBasicBlock* pre_header =
        induction_variable_->GetBlock()->GetLoopInformation()->GetPreHeader();
    if (!initial_->GetBlock()->Dominates(pre_header) ||
        !end_->GetBlock()->Dominates(pre_header)) {
      // Can't move initial_ or end_ into pre_header for comparisons.
      return false;
    }

    HBasicBlock* deopt_block;
    bool loop_entry_test_block_added = false;
    bool is_constant_proven, is_length_proven;

    HInstruction* const_comparing_instruction;
    int32_t const_compared_to;
    HInstruction* array_length_comparing_instruction;
    int32_t array_length_offset;
    if (increment_ == 1) {
      // Increasing from initial_ to end_.
      const_comparing_instruction = initial_;
      const_compared_to = -offset_low;
      array_length_comparing_instruction = end_;
      array_length_offset = inclusive_ ? -offset_high - 1 : -offset_high;
    } else {
      const_comparing_instruction = end_;
      const_compared_to = inclusive_ ? -offset_low : -offset_low - 1;
      array_length_comparing_instruction = initial_;
      array_length_offset = -offset_high - 1;
    }

    if (CanAddDeoptimizationConstant(const_comparing_instruction,
                                     const_compared_to,
                                     &is_constant_proven) &&
        CanAddDeoptimizationArrayLength(array_length_comparing_instruction,
                                        array_length,
                                        array_length_offset,
                                        &is_length_proven)) {
      if (!is_constant_proven || !is_length_proven) {
        deopt_block = TransformLoopForDeoptimizationIfNeeded();
        loop_entry_test_block_added = (deopt_block != pre_header);
        if (loop_entry_test_block_added) {
          // Loop body may be entered.
          AddLoopBodyEntryTest();
        }
      }
      if (!is_constant_proven) {
        AddDeoptimizationConstant(const_comparing_instruction,
                                  const_compared_to,
                                  deopt_block,
                                  loop_entry_test_block_added);
      }
      if (!is_length_proven) {
        AddDeoptimizationArrayLength(array_length_comparing_instruction,
                                     array_length,
                                     array_length_offset,
                                     deopt_block,
                                     loop_entry_test_block_added);
      }
      return true;
    }
    return false;
  }

 private:
  HPhi* const induction_variable_;  // Induction variable for this monotonic value range.
  HInstruction* const initial_;     // Initial value.
  HInstruction* end_;               // End value.
  bool inclusive_;                  // Whether end value is inclusive.
  const int32_t increment_;         // Increment for each loop iteration.
  const ValueBound bound_;          // Additional value bound info for initial_.

  DISALLOW_COPY_AND_ASSIGN(MonotonicValueRange);
};

class BCEVisitor : public HGraphVisitor {
 public:
  // The least number of bounds checks that should be eliminated by triggering
  // the deoptimization technique.
  static constexpr size_t kThresholdForAddingDeoptimize = 2;

  // Very large constant index is considered as an anomaly. This is a threshold
  // beyond which we don't bother to apply the deoptimization technique since
  // it's likely some AIOOBE will be thrown.
  static constexpr int32_t kMaxConstantForAddingDeoptimize = INT_MAX - 1024 * 1024;

  // Added blocks for loop body entry test.
  bool IsAddedBlock(HBasicBlock* block) const {
    return block->GetBlockId() >= initial_block_size_;
  }

  explicit BCEVisitor(HGraph* graph)
      : HGraphVisitor(graph), maps_(graph->GetBlocks().Size()),
        need_to_revisit_block_(false), initial_block_size_(graph->GetBlocks().Size()) {}

  void VisitBasicBlock(HBasicBlock* block) OVERRIDE {
    DCHECK(!IsAddedBlock(block));
    first_constant_index_bounds_check_map_.clear();
    HGraphVisitor::VisitBasicBlock(block);
    if (need_to_revisit_block_) {
      AddComparesWithDeoptimization(block);
      need_to_revisit_block_ = false;
      first_constant_index_bounds_check_map_.clear();
      GetValueRangeMap(block)->clear();
      HGraphVisitor::VisitBasicBlock(block);
    }
  }

 private:
  // Return the map of proven value ranges at the beginning of a basic block.
  ArenaSafeMap<int, ValueRange*>* GetValueRangeMap(HBasicBlock* basic_block) {
    if (IsAddedBlock(basic_block)) {
      // Added blocks don't keep value ranges.
      return nullptr;
    }
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
      if (map != nullptr) {
        if (map->find(instruction->GetId()) != map->end()) {
          return map->Get(instruction->GetId());
        }
      } else {
        DCHECK(IsAddedBlock(basic_block));
      }
      basic_block = basic_block->GetDominator();
    }
    // Didn't find any.
    return nullptr;
  }

  // Narrow the value range of `instruction` at the end of `basic_block` with `range`,
  // and push the narrowed value range to `successor`.
  void ApplyRangeFromComparison(HInstruction* instruction, HBasicBlock* basic_block,
                                HBasicBlock* successor, ValueRange* range) {
    ValueRange* existing_range = LookupValueRange(instruction, basic_block);
    if (existing_range == nullptr) {
      if (range != nullptr) {
        GetValueRangeMap(successor)->Overwrite(instruction->GetId(), range);
      }
      return;
    }
    if (existing_range->IsMonotonicValueRange()) {
      DCHECK(instruction->IsLoopHeaderPhi());
      // Make sure the comparison is in the loop header so each increment is
      // checked with a comparison.
      if (instruction->GetBlock() != basic_block) {
        return;
      }
    }
    ValueRange* narrowed_range = existing_range->Narrow(range);
    GetValueRangeMap(successor)->Overwrite(instruction->GetId(), narrowed_range);
  }

  // Special case that we may simultaneously narrow two MonotonicValueRange's to
  // regular value ranges.
  void HandleIfBetweenTwoMonotonicValueRanges(HIf* instruction,
                                              HInstruction* left,
                                              HInstruction* right,
                                              IfCondition cond,
                                              MonotonicValueRange* left_range,
                                              MonotonicValueRange* right_range) {
    DCHECK(left->IsLoopHeaderPhi());
    DCHECK(right->IsLoopHeaderPhi());
    if (instruction->GetBlock() != left->GetBlock()) {
      // Comparison needs to be in loop header to make sure it's done after each
      // increment/decrement.
      return;
    }

    // Handle common cases which also don't have overflow/underflow concerns.
    if (left_range->GetIncrement() == 1 &&
        left_range->GetBound().IsConstant() &&
        right_range->GetIncrement() == -1 &&
        right_range->GetBound().IsRelatedToArrayLength() &&
        right_range->GetBound().GetConstant() < 0) {
      HBasicBlock* successor = nullptr;
      int32_t left_compensation = 0;
      int32_t right_compensation = 0;
      if (cond == kCondLT) {
        left_compensation = -1;
        right_compensation = 1;
        successor = instruction->IfTrueSuccessor();
      } else if (cond == kCondLE) {
        successor = instruction->IfTrueSuccessor();
      } else if (cond == kCondGT) {
        successor = instruction->IfFalseSuccessor();
      } else if (cond == kCondGE) {
        left_compensation = -1;
        right_compensation = 1;
        successor = instruction->IfFalseSuccessor();
      } else {
        // We don't handle '=='/'!=' test in case left and right can cross and
        // miss each other.
        return;
      }

      if (successor != nullptr) {
        bool overflow;
        bool underflow;
        ValueRange* new_left_range = new (GetGraph()->GetArena()) ValueRange(
            GetGraph()->GetArena(),
            left_range->GetBound(),
            right_range->GetBound().Add(left_compensation, &overflow, &underflow));
        if (!overflow && !underflow) {
          ApplyRangeFromComparison(left, instruction->GetBlock(), successor,
                                   new_left_range);
        }

        ValueRange* new_right_range = new (GetGraph()->GetArena()) ValueRange(
            GetGraph()->GetArena(),
            left_range->GetBound().Add(right_compensation, &overflow, &underflow),
            right_range->GetBound());
        if (!overflow && !underflow) {
          ApplyRangeFromComparison(right, instruction->GetBlock(), successor,
                                   new_right_range);
        }
      }
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

    ValueRange* left_range = LookupValueRange(left, block);
    MonotonicValueRange* left_monotonic_range = nullptr;
    if (left_range != nullptr) {
      left_monotonic_range = left_range->AsMonotonicValueRange();
      if (left_monotonic_range != nullptr) {
        HBasicBlock* loop_head = left_monotonic_range->GetLoopHeader();
        if (instruction->GetBlock() != loop_head) {
          // For monotonic value range, don't handle `instruction`
          // if it's not defined in the loop header.
          return;
        }
      }
    }

    bool found;
    ValueBound bound = ValueBound::DetectValueBoundFromValue(right, &found);
    // Each comparison can establish a lower bound and an upper bound
    // for the left hand side.
    ValueBound lower = bound;
    ValueBound upper = bound;
    if (!found) {
      // No constant or array.length+c format bound found.
      // For i<j, we can still use j's upper bound as i's upper bound. Same for lower.
      ValueRange* right_range = LookupValueRange(right, block);
      if (right_range != nullptr) {
        if (right_range->IsMonotonicValueRange()) {
          if (left_range != nullptr && left_range->IsMonotonicValueRange()) {
            HandleIfBetweenTwoMonotonicValueRanges(instruction, left, right, cond,
                                                   left_range->AsMonotonicValueRange(),
                                                   right_range->AsMonotonicValueRange());
            return;
          }
        }
        lower = right_range->GetLower();
        upper = right_range->GetUpper();
      } else {
        lower = ValueBound::Min();
        upper = ValueBound::Max();
      }
    }

    bool overflow, underflow;
    if (cond == kCondLT || cond == kCondLE) {
      if (left_monotonic_range != nullptr) {
        // Update the info for monotonic value range.
        if (left_monotonic_range->GetInductionVariable() == left &&
            left_monotonic_range->GetIncrement() < 0 &&
            block == left_monotonic_range->GetLoopHeader() &&
            instruction->IfFalseSuccessor()->GetLoopInformation() == block->GetLoopInformation()) {
          left_monotonic_range->SetEnd(right);
          left_monotonic_range->SetInclusive(cond == kCondLT);
        }
      }

      if (!upper.Equals(ValueBound::Max())) {
        int32_t compensation = (cond == kCondLT) ? -1 : 0;  // upper bound is inclusive
        ValueBound new_upper = upper.Add(compensation, &overflow, &underflow);
        if (overflow || underflow) {
          return;
        }
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), ValueBound::Min(), new_upper);
        ApplyRangeFromComparison(left, block, true_successor, new_range);
      }

      // array.length as a lower bound isn't considered useful.
      if (!lower.Equals(ValueBound::Min()) && !lower.IsRelatedToArrayLength()) {
        int32_t compensation = (cond == kCondLE) ? 1 : 0;  // lower bound is inclusive
        ValueBound new_lower = lower.Add(compensation, &overflow, &underflow);
        if (overflow || underflow) {
          return;
        }
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), new_lower, ValueBound::Max());
        ApplyRangeFromComparison(left, block, false_successor, new_range);
      }
    } else if (cond == kCondGT || cond == kCondGE) {
      if (left_monotonic_range != nullptr) {
        // Update the info for monotonic value range.
        if (left_monotonic_range->GetInductionVariable() == left &&
            left_monotonic_range->GetIncrement() > 0 &&
            block == left_monotonic_range->GetLoopHeader() &&
            instruction->IfFalseSuccessor()->GetLoopInformation() == block->GetLoopInformation()) {
          left_monotonic_range->SetEnd(right);
          left_monotonic_range->SetInclusive(cond == kCondGT);
        }
      }

      // array.length as a lower bound isn't considered useful.
      if (!lower.Equals(ValueBound::Min()) && !lower.IsRelatedToArrayLength()) {
        int32_t compensation = (cond == kCondGT) ? 1 : 0;  // lower bound is inclusive
        ValueBound new_lower = lower.Add(compensation, &overflow, &underflow);
        if (overflow || underflow) {
          return;
        }
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), new_lower, ValueBound::Max());
        ApplyRangeFromComparison(left, block, true_successor, new_range);
      }

      if (!upper.Equals(ValueBound::Max())) {
        int32_t compensation = (cond == kCondGE) ? -1 : 0;  // upper bound is inclusive
        ValueBound new_upper = upper.Add(compensation, &overflow, &underflow);
        if (overflow || underflow) {
          return;
        }
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
    DCHECK(array_length->IsIntConstant() ||
           array_length->IsArrayLength() ||
           array_length->IsPhi());

    if (array_length->IsPhi()) {
      // Input 1 of the phi contains the real array.length once the loop body is
      // entered. That value will be used for bound analysis. The graph is still
      // strictly in SSA form.
      array_length = array_length->AsPhi()->InputAt(1)->AsArrayLength();
    }

    if (!index->IsIntConstant()) {
      ValueRange* index_range = LookupValueRange(index, block);
      if (index_range != nullptr) {
        ValueBound lower = ValueBound(nullptr, 0);        // constant 0
        ValueBound upper = ValueBound(array_length, -1);  // array_length - 1
        ValueRange* array_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), lower, upper);
        if (index_range->FitsIn(array_range)) {
          ReplaceBoundsCheck(bounds_check, index);
          return;
        }
      }
    } else {
      int32_t constant = index->AsIntConstant()->GetValue();
      if (constant < 0) {
        // Will always throw exception.
        return;
      }
      if (array_length->IsIntConstant()) {
        if (constant < array_length->AsIntConstant()->GetValue()) {
          ReplaceBoundsCheck(bounds_check, index);
        }
        return;
      }

      DCHECK(array_length->IsArrayLength());
      ValueRange* existing_range = LookupValueRange(array_length, block);
      if (existing_range != nullptr) {
        ValueBound lower = existing_range->GetLower();
        DCHECK(lower.IsConstant());
        if (constant < lower.GetConstant()) {
          ReplaceBoundsCheck(bounds_check, index);
          return;
        } else {
          // Existing range isn't strong enough to eliminate the bounds check.
          // Fall through to update the array_length range with info from this
          // bounds check.
        }
      }

      if (first_constant_index_bounds_check_map_.find(array_length->GetId()) ==
          first_constant_index_bounds_check_map_.end()) {
        // Remember the first bounds check against array_length of a constant index.
        // That bounds check instruction has an associated HEnvironment where we
        // may add an HDeoptimize to eliminate bounds checks of constant indices
        // against array_length.
        first_constant_index_bounds_check_map_.Put(array_length->GetId(), bounds_check);
      } else {
        // We've seen it at least twice. It's beneficial to introduce a compare with
        // deoptimization fallback to eliminate the bounds checks.
        need_to_revisit_block_ = true;
      }

      // Once we have an array access like 'array[5] = 1', we record array.length >= 6.
      // We currently don't do it for non-constant index since a valid array[i] can't prove
      // a valid array[i-1] yet due to the lower bound side.
      if (constant == INT_MAX) {
        // INT_MAX as an index will definitely throw AIOOBE.
        return;
      }
      ValueBound lower = ValueBound(nullptr, constant + 1);
      ValueBound upper = ValueBound::Max();
      ValueRange* range = new (GetGraph()->GetArena())
          ValueRange(GetGraph()->GetArena(), lower, upper);
      GetValueRangeMap(block)->Overwrite(array_length->GetId(), range);
    }
  }

  void ReplaceBoundsCheck(HInstruction* bounds_check, HInstruction* index) {
    bounds_check->ReplaceWith(index);
    bounds_check->GetBlock()->RemoveInstruction(bounds_check);
  }

  static bool HasSameInputAtBackEdges(HPhi* phi) {
    DCHECK(phi->IsLoopHeaderPhi());
    // Start with input 1. Input 0 is from the incoming block.
    HInstruction* input1 = phi->InputAt(1);
    DCHECK(phi->GetBlock()->GetLoopInformation()->IsBackEdge(
        *phi->GetBlock()->GetPredecessors().Get(1)));
    for (size_t i = 2, e = phi->InputCount(); i < e; ++i) {
      DCHECK(phi->GetBlock()->GetLoopInformation()->IsBackEdge(
          *phi->GetBlock()->GetPredecessors().Get(i)));
      if (input1 != phi->InputAt(i)) {
        return false;
      }
    }
    return true;
  }

  void VisitPhi(HPhi* phi) {
    if (phi->IsLoopHeaderPhi()
        && (phi->GetType() == Primitive::kPrimInt)
        && HasSameInputAtBackEdges(phi)) {
      HInstruction* instruction = phi->InputAt(1);
      HInstruction *left;
      int32_t increment;
      if (ValueBound::IsAddOrSubAConstant(instruction, &left, &increment)) {
        if (left == phi) {
          HInstruction* initial_value = phi->InputAt(0);
          ValueRange* range = nullptr;
          if (increment == 0) {
            // Add constant 0. It's really a fixed value.
            range = new (GetGraph()->GetArena()) ValueRange(
                GetGraph()->GetArena(),
                ValueBound(initial_value, 0),
                ValueBound(initial_value, 0));
          } else {
            // Monotonically increasing/decreasing.
            bool found;
            ValueBound bound = ValueBound::DetectValueBoundFromValue(
                initial_value, &found);
            if (!found) {
              // No constant or array.length+c bound found.
              // For i=j, we can still use j's upper bound as i's upper bound.
              // Same for lower.
              ValueRange* initial_range = LookupValueRange(initial_value, phi->GetBlock());
              if (initial_range != nullptr) {
                bound = increment > 0 ? initial_range->GetLower() :
                                        initial_range->GetUpper();
              } else {
                bound = increment > 0 ? ValueBound::Min() : ValueBound::Max();
              }
            }
            range = new (GetGraph()->GetArena()) MonotonicValueRange(
                GetGraph()->GetArena(),
                phi,
                initial_value,
                increment,
                bound);
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

        HBasicBlock* block = instruction->GetBlock();
        ValueRange* left_range = LookupValueRange(left, block);
        if (left_range == nullptr) {
          return;
        }

        if (left_range->IsMonotonicValueRange() &&
            block == left_range->AsMonotonicValueRange()->GetLoopHeader()) {
          // The comparison is for an induction variable in the loop header.
          DCHECK(left == left_range->AsMonotonicValueRange()->GetInductionVariable());
          HBasicBlock* loop_body_successor =
            left_range->AsMonotonicValueRange()->GetLoopHeaderSuccesorInLoop();
          if (loop_body_successor == nullptr) {
            // In case it's some strange loop structure.
            return;
          }
          ValueRange* new_left_range = LookupValueRange(left, loop_body_successor);
          if ((new_left_range == left_range) ||
              // Range narrowed with deoptimization is usually more useful than
              // a constant range.
              new_left_range->IsConstantValueRange()) {
            // We are not successful in narrowing the monotonic value range to
            // a regular value range. Try using deoptimization.
            new_left_range = left_range->AsMonotonicValueRange()->
                NarrowWithDeoptimization();
            if (new_left_range != left_range) {
              GetValueRangeMap(loop_body_successor)->Overwrite(left->GetId(), new_left_range);
            }
          }
        }
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

    // Try to handle (array.length - i) or (array.length + c - i) format.
    HInstruction* left_of_left;  // left input of left.
    int32_t right_const = 0;
    if (ValueBound::IsAddOrSubAConstant(left, &left_of_left, &right_const)) {
      left = left_of_left;
    }
    // The value of left input of the sub equals (left + right_const).

    if (left->IsArrayLength()) {
      HInstruction* array_length = left->AsArrayLength();
      ValueRange* right_range = LookupValueRange(right, sub->GetBlock());
      if (right_range != nullptr) {
        ValueBound lower = right_range->GetLower();
        ValueBound upper = right_range->GetUpper();
        if (lower.IsConstant() && upper.IsRelatedToArrayLength()) {
          HInstruction* upper_inst = upper.GetInstruction();
          // Make sure it's the same array.
          if (ValueBound::Equal(array_length, upper_inst)) {
            int32_t c0 = right_const;
            int32_t c1 = lower.GetConstant();
            int32_t c2 = upper.GetConstant();
            // (array.length + c0 - v) where v is in [c1, array.length + c2]
            // gets [c0 - c2, array.length + c0 - c1] as its value range.
            if (!ValueBound::WouldAddOverflowOrUnderflow(c0, -c2) &&
                !ValueBound::WouldAddOverflowOrUnderflow(c0, -c1)) {
              if ((c0 - c1) <= 0) {
                // array.length + (c0 - c1) won't overflow/underflow.
                ValueRange* range = new (GetGraph()->GetArena()) ValueRange(
                    GetGraph()->GetArena(),
                    ValueBound(nullptr, right_const - upper.GetConstant()),
                    ValueBound(array_length, right_const - lower.GetConstant()));
                GetValueRangeMap(sub->GetBlock())->Overwrite(sub->GetId(), range);
              }
            }
          }
        }
      }
    }
  }

  void FindAndHandlePartialArrayLength(HBinaryOperation* instruction) {
    DCHECK(instruction->IsDiv() || instruction->IsShr() || instruction->IsUShr());
    HInstruction* right = instruction->GetRight();
    int32_t right_const;
    if (right->IsIntConstant()) {
      right_const = right->AsIntConstant()->GetValue();
      // Detect division by two or more.
      if ((instruction->IsDiv() && right_const <= 1) ||
          (instruction->IsShr() && right_const < 1) ||
          (instruction->IsUShr() && right_const < 1)) {
        return;
      }
    } else {
      return;
    }

    // Try to handle array.length/2 or (array.length-1)/2 format.
    HInstruction* left = instruction->GetLeft();
    HInstruction* left_of_left;  // left input of left.
    int32_t c = 0;
    if (ValueBound::IsAddOrSubAConstant(left, &left_of_left, &c)) {
      left = left_of_left;
    }
    // The value of left input of instruction equals (left + c).

    // (array_length + 1) or smaller divided by two or more
    // always generate a value in [INT_MIN, array_length].
    // This is true even if array_length is INT_MAX.
    if (left->IsArrayLength() && c <= 1) {
      if (instruction->IsUShr() && c < 0) {
        // Make sure for unsigned shift, left side is not negative.
        // e.g. if array_length is 2, ((array_length - 3) >>> 2) is way bigger
        // than array_length.
        return;
      }
      ValueRange* range = new (GetGraph()->GetArena()) ValueRange(
          GetGraph()->GetArena(),
          ValueBound(nullptr, INT_MIN),
          ValueBound(left, 0));
      GetValueRangeMap(instruction->GetBlock())->Overwrite(instruction->GetId(), range);
    }
  }

  void VisitDiv(HDiv* div) {
    FindAndHandlePartialArrayLength(div);
  }

  void VisitShr(HShr* shr) {
    FindAndHandlePartialArrayLength(shr);
  }

  void VisitUShr(HUShr* ushr) {
    FindAndHandlePartialArrayLength(ushr);
  }

  void VisitAnd(HAnd* instruction) {
    if (instruction->GetRight()->IsIntConstant()) {
      int32_t constant = instruction->GetRight()->AsIntConstant()->GetValue();
      if (constant > 0) {
        // constant serves as a mask so any number masked with it
        // gets a [0, constant] value range.
        ValueRange* range = new (GetGraph()->GetArena()) ValueRange(
            GetGraph()->GetArena(),
            ValueBound(nullptr, 0),
            ValueBound(nullptr, constant));
        GetValueRangeMap(instruction->GetBlock())->Overwrite(instruction->GetId(), range);
      }
    }
  }

  void VisitNewArray(HNewArray* new_array) {
    HInstruction* len = new_array->InputAt(0);
    if (!len->IsIntConstant()) {
      HInstruction *left;
      int32_t right_const;
      if (ValueBound::IsAddOrSubAConstant(len, &left, &right_const)) {
        // (left + right_const) is used as size to new the array.
        // We record "-right_const <= left <= new_array - right_const";
        ValueBound lower = ValueBound(nullptr, -right_const);
        // We use new_array for the bound instead of new_array.length,
        // which isn't available as an instruction yet. new_array will
        // be treated the same as new_array.length when it's used in a ValueBound.
        ValueBound upper = ValueBound(new_array, -right_const);
        ValueRange* range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), lower, upper);
        ValueRange* existing_range = LookupValueRange(left, new_array->GetBlock());
        if (existing_range != nullptr) {
          range = existing_range->Narrow(range);
        }
        GetValueRangeMap(new_array->GetBlock())->Overwrite(left->GetId(), range);
      }
    }
  }

  void VisitDeoptimize(HDeoptimize* deoptimize) {
    // Right now it's only HLessThanOrEqual.
    DCHECK(deoptimize->InputAt(0)->IsLessThanOrEqual());
    HLessThanOrEqual* less_than_or_equal = deoptimize->InputAt(0)->AsLessThanOrEqual();
    HInstruction* instruction = less_than_or_equal->InputAt(0);
    if (instruction->IsArrayLength()) {
      HInstruction* constant = less_than_or_equal->InputAt(1);
      DCHECK(constant->IsIntConstant());
      DCHECK(constant->AsIntConstant()->GetValue() <= kMaxConstantForAddingDeoptimize);
      ValueBound lower = ValueBound(nullptr, constant->AsIntConstant()->GetValue() + 1);
      ValueRange* range = new (GetGraph()->GetArena())
          ValueRange(GetGraph()->GetArena(), lower, ValueBound::Max());
      GetValueRangeMap(deoptimize->GetBlock())->Overwrite(instruction->GetId(), range);
    }
  }

  void AddCompareWithDeoptimization(HInstruction* array_length,
                                    HIntConstant* const_instr,
                                    HBasicBlock* block) {
    DCHECK(array_length->IsArrayLength());
    ValueRange* range = LookupValueRange(array_length, block);
    ValueBound lower_bound = range->GetLower();
    DCHECK(lower_bound.IsConstant());
    DCHECK(const_instr->GetValue() <= kMaxConstantForAddingDeoptimize);
    // Note that the lower bound of the array length may have been refined
    // through other instructions (such as `HNewArray(length - 4)`).
    DCHECK_LE(const_instr->GetValue() + 1, lower_bound.GetConstant());

    // If array_length is less than lower_const, deoptimize.
    HBoundsCheck* bounds_check = first_constant_index_bounds_check_map_.Get(
        array_length->GetId())->AsBoundsCheck();
    HCondition* cond = new (GetGraph()->GetArena()) HLessThanOrEqual(array_length, const_instr);
    HDeoptimize* deoptimize = new (GetGraph()->GetArena())
        HDeoptimize(cond, bounds_check->GetDexPc());
    block->InsertInstructionBefore(cond, bounds_check);
    block->InsertInstructionBefore(deoptimize, bounds_check);
    deoptimize->CopyEnvironmentFrom(bounds_check->GetEnvironment());
  }

  void AddComparesWithDeoptimization(HBasicBlock* block) {
    for (ArenaSafeMap<int, HBoundsCheck*>::iterator it =
             first_constant_index_bounds_check_map_.begin();
         it != first_constant_index_bounds_check_map_.end();
         ++it) {
      HBoundsCheck* bounds_check = it->second;
      HInstruction* array_length = bounds_check->InputAt(1);
      if (!array_length->IsArrayLength()) {
        // Prior deoptimizations may have changed the array length to a phi.
        // TODO(mingyao): propagate the range to the phi?
        DCHECK(array_length->IsPhi()) << array_length->DebugName();
        continue;
      }
      HIntConstant* lower_bound_const_instr = nullptr;
      int32_t lower_bound_const = INT_MIN;
      size_t counter = 0;
      // Count the constant indexing for which bounds checks haven't
      // been removed yet.
      for (HUseIterator<HInstruction*> it2(array_length->GetUses());
           !it2.Done();
           it2.Advance()) {
        HInstruction* user = it2.Current()->GetUser();
        if (user->GetBlock() == block &&
            user->IsBoundsCheck() &&
            user->AsBoundsCheck()->InputAt(0)->IsIntConstant()) {
          DCHECK_EQ(array_length, user->AsBoundsCheck()->InputAt(1));
          HIntConstant* const_instr = user->AsBoundsCheck()->InputAt(0)->AsIntConstant();
          if (const_instr->GetValue() > lower_bound_const) {
            lower_bound_const = const_instr->GetValue();
            lower_bound_const_instr = const_instr;
          }
          counter++;
        }
      }
      if (counter >= kThresholdForAddingDeoptimize &&
          lower_bound_const_instr->GetValue() <= kMaxConstantForAddingDeoptimize) {
        AddCompareWithDeoptimization(array_length, lower_bound_const_instr, block);
      }
    }
  }

  std::vector<std::unique_ptr<ArenaSafeMap<int, ValueRange*>>> maps_;

  // Map an HArrayLength instruction's id to the first HBoundsCheck instruction in
  // a block that checks a constant index against that HArrayLength.
  SafeMap<int, HBoundsCheck*> first_constant_index_bounds_check_map_;

  // For the block, there is at least one HArrayLength instruction for which there
  // is more than one bounds check instruction with constant indexing. And it's
  // beneficial to add a compare instruction that has deoptimization fallback and
  // eliminate those bounds checks.
  bool need_to_revisit_block_;

  // Initial number of blocks.
  int32_t initial_block_size_;

  DISALLOW_COPY_AND_ASSIGN(BCEVisitor);
};

void BoundsCheckElimination::Run() {
  if (!graph_->HasBoundsChecks()) {
    return;
  }

  BCEVisitor visitor(graph_);
  // Reverse post order guarantees a node's dominators are visited first.
  // We want to visit in the dominator-based order since if a value is known to
  // be bounded by a range at one instruction, it must be true that all uses of
  // that value dominated by that instruction fits in that range. Range of that
  // value can be narrowed further down in the dominator tree.
  //
  // TODO: only visit blocks that dominate some array accesses.
  HBasicBlock* last_visited_block = nullptr;
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* current = it.Current();
    if (current == last_visited_block) {
      // We may insert blocks into the reverse post order list when processing
      // a loop header. Don't process it again.
      DCHECK(current->IsLoopHeader());
      continue;
    }
    if (visitor.IsAddedBlock(current)) {
      // Skip added blocks. Their effects are already taken care of.
      continue;
    }
    visitor.VisitBasicBlock(current);
    last_visited_block = current;
  }
}

}  // namespace art
