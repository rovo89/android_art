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

#include "register_allocator.h"

#include "code_generator.h"
#include "ssa_liveness_analysis.h"

namespace art {

static constexpr size_t kMaxLifetimePosition = -1;
static constexpr size_t kDefaultNumberOfSpillSlots = 4;

RegisterAllocator::RegisterAllocator(ArenaAllocator* allocator, const CodeGenerator& codegen)
      : allocator_(allocator),
        codegen_(codegen),
        unhandled_(allocator, 0),
        handled_(allocator, 0),
        active_(allocator, 0),
        inactive_(allocator, 0),
        spill_slots_(allocator, kDefaultNumberOfSpillSlots),
        processing_core_registers_(false),
        number_of_registers_(-1),
        registers_array_(nullptr),
        blocked_registers_(allocator->AllocArray<bool>(codegen.GetNumberOfRegisters())) {
  codegen.SetupBlockedRegisters(blocked_registers_);
}

static bool ShouldProcess(bool processing_core_registers, HInstruction* instruction) {
  bool is_core_register = (instruction->GetType() != Primitive::kPrimDouble)
      && (instruction->GetType() != Primitive::kPrimFloat);
  return processing_core_registers == is_core_register;
}

void RegisterAllocator::AllocateRegistersInternal(const SsaLivenessAnalysis& liveness) {
  number_of_registers_ = processing_core_registers_
      ? codegen_.GetNumberOfCoreRegisters()
      : codegen_.GetNumberOfFloatingPointRegisters();

  registers_array_ = allocator_->AllocArray<size_t>(number_of_registers_);

  // Iterate post-order, to ensure the list is sorted, and the last added interval
  // is the one with the lowest start position.
  for (size_t i = liveness.GetNumberOfSsaValues(); i > 0; --i) {
    HInstruction* instruction = liveness.GetInstructionFromSsaIndex(i - 1);
    if (ShouldProcess(processing_core_registers_, instruction)) {
      LiveInterval* current = instruction->GetLiveInterval();
      DCHECK(unhandled_.IsEmpty() || current->StartsBefore(unhandled_.Peek()));
      unhandled_.Add(current);
    }
  }

  LinearScan();
  if (kIsDebugBuild) {
    ValidateInternal(liveness, true);
  }
}

bool RegisterAllocator::ValidateInternal(const SsaLivenessAnalysis& liveness,
                                         bool log_fatal_on_failure) const {
  // To simplify unit testing, we eagerly create the array of intervals, and
  // call the helper method.
  GrowableArray<LiveInterval*> intervals(allocator_, 0);
  for (size_t i = 0; i < liveness.GetNumberOfSsaValues(); ++i) {
    HInstruction* instruction = liveness.GetInstructionFromSsaIndex(i);
    if (ShouldProcess(processing_core_registers_, instruction)) {
      intervals.Add(instruction->GetLiveInterval());
    }
  }
  return ValidateIntervals(intervals, spill_slots_.Size(), codegen_, allocator_,
                           processing_core_registers_, log_fatal_on_failure);
}

class AllRangesIterator : public ValueObject {
 public:
  explicit AllRangesIterator(LiveInterval* interval)
      : current_interval_(interval),
        current_range_(interval->GetFirstRange()) {}

  bool Done() const { return current_interval_ == nullptr; }
  LiveRange* CurrentRange() const { return current_range_; }
  LiveInterval* CurrentInterval() const { return current_interval_; }

  void Advance() {
    current_range_ = current_range_->GetNext();
    if (current_range_ == nullptr) {
      current_interval_ = current_interval_->GetNextSibling();
      if (current_interval_ != nullptr) {
        current_range_ = current_interval_->GetFirstRange();
      }
    }
  }

 private:
  LiveInterval* current_interval_;
  LiveRange* current_range_;

  DISALLOW_COPY_AND_ASSIGN(AllRangesIterator);
};

bool RegisterAllocator::ValidateIntervals(const GrowableArray<LiveInterval*>& intervals,
                                          size_t number_of_spill_slots,
                                          const CodeGenerator& codegen,
                                          ArenaAllocator* allocator,
                                          bool processing_core_registers,
                                          bool log_fatal_on_failure) {
  size_t number_of_registers = processing_core_registers
      ? codegen.GetNumberOfCoreRegisters()
      : codegen.GetNumberOfFloatingPointRegisters();
  GrowableArray<ArenaBitVector*> liveness_of_values(
      allocator, number_of_registers + number_of_spill_slots);

  // Allocate a bit vector per register. A live interval that has a register
  // allocated will populate the associated bit vector based on its live ranges.
  for (size_t i = 0; i < number_of_registers + number_of_spill_slots; ++i) {
    liveness_of_values.Add(new (allocator) ArenaBitVector(allocator, 0, true));
  }

  for (size_t i = 0, e = intervals.Size(); i < e; ++i) {
    for (AllRangesIterator it(intervals.Get(i)); !it.Done(); it.Advance()) {
      LiveInterval* current = it.CurrentInterval();
      if (current->GetParent()->HasSpillSlot()) {
        BitVector* liveness_of_spill_slot = liveness_of_values.Get(
            number_of_registers + current->GetParent()->GetSpillSlot() / kVRegSize);
        for (size_t j = it.CurrentRange()->GetStart(); j < it.CurrentRange()->GetEnd(); ++j) {
          if (liveness_of_spill_slot->IsBitSet(j)) {
            if (log_fatal_on_failure) {
              std::ostringstream message;
              message << "Spill slot conflict at " << j;
              LOG(FATAL) << message.str();
            } else {
              return false;
            }
          } else {
            liveness_of_spill_slot->SetBit(j);
          }
        }
      }

      if (current->HasRegister()) {
        BitVector* liveness_of_register = liveness_of_values.Get(current->GetRegister());
        for (size_t j = it.CurrentRange()->GetStart(); j < it.CurrentRange()->GetEnd(); ++j) {
          if (liveness_of_register->IsBitSet(j)) {
            if (log_fatal_on_failure) {
              std::ostringstream message;
              message << "Register conflict at " << j << " for ";
              if (processing_core_registers) {
                codegen.DumpCoreRegister(message, current->GetRegister());
              } else {
                codegen.DumpFloatingPointRegister(message, current->GetRegister());
              }
              LOG(FATAL) << message.str();
            } else {
              return false;
            }
          } else {
            liveness_of_register->SetBit(j);
          }
        }
      }
    }
  }
  return true;
}

void RegisterAllocator::DumpInterval(std::ostream& stream, LiveInterval* interval) {
  interval->Dump(stream);
  stream << ": ";
  if (interval->HasRegister()) {
    if (processing_core_registers_) {
      codegen_.DumpCoreRegister(stream, interval->GetRegister());
    } else {
      codegen_.DumpFloatingPointRegister(stream, interval->GetRegister());
    }
  } else {
    stream << "spilled";
  }
  stream << std::endl;
}

// By the book implementation of a linear scan register allocator.
void RegisterAllocator::LinearScan() {
  while (!unhandled_.IsEmpty()) {
    // (1) Remove interval with the lowest start position from unhandled.
    LiveInterval* current = unhandled_.Pop();
    size_t position = current->GetStart();

    // (2) Remove currently active intervals that are dead at this position.
    //     Move active intervals that have a lifetime hole at this position
    //     to inactive.
    for (size_t i = 0; i < active_.Size(); ++i) {
      LiveInterval* interval = active_.Get(i);
      if (interval->IsDeadAt(position)) {
        active_.Delete(interval);
        --i;
        handled_.Add(interval);
      } else if (!interval->Covers(position)) {
        active_.Delete(interval);
        --i;
        inactive_.Add(interval);
      }
    }

    // (3) Remove currently inactive intervals that are dead at this position.
    //     Move inactive intervals that cover this position to active.
    for (size_t i = 0; i < inactive_.Size(); ++i) {
      LiveInterval* interval = inactive_.Get(i);
      if (interval->IsDeadAt(position)) {
        inactive_.Delete(interval);
        --i;
        handled_.Add(interval);
      } else if (interval->Covers(position)) {
        inactive_.Delete(interval);
        --i;
        active_.Add(interval);
      }
    }

    // (4) Try to find an available register.
    bool success = TryAllocateFreeReg(current);

    // (5) If no register could be found, we need to spill.
    if (!success) {
      success = AllocateBlockedReg(current);
    }

    // (6) If the interval had a register allocated, add it to the list of active
    //     intervals.
    if (success) {
      active_.Add(current);
    }
  }
}

// Find a free register. If multiple are found, pick the register that
// is free the longest.
bool RegisterAllocator::TryAllocateFreeReg(LiveInterval* current) {
  size_t* free_until = registers_array_;

  // First set all registers to be free.
  for (size_t i = 0; i < number_of_registers_; ++i) {
    free_until[i] = kMaxLifetimePosition;
  }

  // For each active interval, set its register to not free.
  for (size_t i = 0, e = active_.Size(); i < e; ++i) {
    LiveInterval* interval = active_.Get(i);
    DCHECK(interval->HasRegister());
    free_until[interval->GetRegister()] = 0;
  }

  // For each inactive interval, set its register to be free until
  // the next intersection with `current`.
  // Thanks to SSA, this should only be needed for intervals
  // that are the result of a split.
  for (size_t i = 0, e = inactive_.Size(); i < e; ++i) {
    LiveInterval* inactive = inactive_.Get(i);
    DCHECK(inactive->HasRegister());
    size_t next_intersection = inactive->FirstIntersectionWith(current);
    if (next_intersection != kNoLifetime) {
      free_until[inactive->GetRegister()] = next_intersection;
    }
  }

  // Pick the register that is free the longest.
  int reg = -1;
  for (size_t i = 0; i < number_of_registers_; ++i) {
    if (IsBlocked(i)) continue;
    if (reg == -1 || free_until[i] > free_until[reg]) {
      reg = i;
      if (free_until[i] == kMaxLifetimePosition) break;
    }
  }

  // If we could not find a register, we need to spill.
  if (reg == -1 || free_until[reg] == 0) {
    return false;
  }

  current->SetRegister(reg);
  if (!current->IsDeadAt(free_until[reg])) {
    // If the register is only available for a subset of live ranges
    // covered by `current`, split `current` at the position where
    // the register is not available anymore.
    LiveInterval* split = Split(current, free_until[reg]);
    DCHECK(split != nullptr);
    AddToUnhandled(split);
  }
  return true;
}

bool RegisterAllocator::IsBlocked(int reg) const {
  // TODO: This only works for core registers and needs to be adjusted for
  // floating point registers.
  DCHECK(processing_core_registers_);
  return blocked_registers_[reg];
}

// Find the register that is used the last, and spill the interval
// that holds it. If the first use of `current` is after that register
// we spill `current` instead.
bool RegisterAllocator::AllocateBlockedReg(LiveInterval* current) {
  size_t first_register_use = current->FirstRegisterUse();
  if (current->FirstRegisterUse() == kNoLifetime) {
    AllocateSpillSlotFor(current);
    return false;
  }

  // First set all registers as not being used.
  size_t* next_use = registers_array_;
  for (size_t i = 0; i < number_of_registers_; ++i) {
    next_use[i] = kMaxLifetimePosition;
  }

  // For each active interval, find the next use of its register after the
  // start of current.
  for (size_t i = 0, e = active_.Size(); i < e; ++i) {
    LiveInterval* active = active_.Get(i);
    DCHECK(active->HasRegister());
    size_t use = active->FirstRegisterUseAfter(current->GetStart());
    if (use != kNoLifetime) {
      next_use[active->GetRegister()] = use;
    }
  }

  // For each inactive interval, find the next use of its register after the
  // start of current.
  // Thanks to SSA, this should only be needed for intervals
  // that are the result of a split.
  for (size_t i = 0, e = inactive_.Size(); i < e; ++i) {
    LiveInterval* inactive = inactive_.Get(i);
    DCHECK(inactive->HasRegister());
    size_t use = inactive->FirstRegisterUseAfter(current->GetStart());
    if (use != kNoLifetime) {
      next_use[inactive->GetRegister()] = use;
    }
  }

  // Pick the register that is used the last.
  int reg = -1;
  for (size_t i = 0; i < number_of_registers_; ++i) {
    if (IsBlocked(i)) continue;
    if (reg == -1 || next_use[i] > next_use[reg]) {
      reg = i;
      if (next_use[i] == kMaxLifetimePosition) break;
    }
  }

  if (first_register_use >= next_use[reg]) {
    // If the first use of that instruction is after the last use of the found
    // register, we split this interval just before its first register use.
    AllocateSpillSlotFor(current);
    LiveInterval* split = Split(current, first_register_use - 1);
    AddToUnhandled(split);
    return false;
  } else {
    // Use this register and spill the active and inactives interval that
    // have that register.
    current->SetRegister(reg);

    for (size_t i = 0, e = active_.Size(); i < e; ++i) {
      LiveInterval* active = active_.Get(i);
      if (active->GetRegister() == reg) {
        LiveInterval* split = Split(active, current->GetStart());
        active_.DeleteAt(i);
        handled_.Add(active);
        AddToUnhandled(split);
        break;
      }
    }

    for (size_t i = 0; i < inactive_.Size(); ++i) {
      LiveInterval* inactive = inactive_.Get(i);
      if (inactive->GetRegister() == reg) {
        LiveInterval* split = Split(inactive, current->GetStart());
        inactive_.DeleteAt(i);
        handled_.Add(inactive);
        AddToUnhandled(split);
        --i;
      }
    }

    return true;
  }
}

void RegisterAllocator::AddToUnhandled(LiveInterval* interval) {
  for (size_t i = unhandled_.Size(); i > 0; --i) {
    LiveInterval* current = unhandled_.Get(i - 1);
    if (current->StartsAfter(interval)) {
      unhandled_.InsertAt(i, interval);
      break;
    }
  }
}

LiveInterval* RegisterAllocator::Split(LiveInterval* interval, size_t position) {
  DCHECK(position >= interval->GetStart());
  DCHECK(!interval->IsDeadAt(position));
  if (position == interval->GetStart()) {
    // Spill slot will be allocated when handling `interval` again.
    interval->ClearRegister();
    return interval;
  } else {
    LiveInterval* new_interval = interval->SplitAt(position);
    return new_interval;
  }
}

void RegisterAllocator::AllocateSpillSlotFor(LiveInterval* interval) {
  LiveInterval* parent = interval->GetParent();

  // An instruction gets a spill slot for its entire lifetime. If the parent
  // of this interval already has a spill slot, there is nothing to do.
  if (parent->HasSpillSlot()) {
    return;
  }

  // Find when this instruction dies.
  LiveInterval* last_sibling = interval;
  while (last_sibling->GetNextSibling() != nullptr) {
    last_sibling = last_sibling->GetNextSibling();
  }
  size_t end = last_sibling->GetEnd();

  // Find an available spill slot.
  size_t slot = 0;
  for (size_t e = spill_slots_.Size(); slot < e; ++slot) {
    if (spill_slots_.Get(slot) <= parent->GetStart()) {
      break;
    }
  }

  if (slot == spill_slots_.Size()) {
    // We need a new spill slot.
    spill_slots_.Add(end);
  } else {
    spill_slots_.Put(slot, end);
  }

  interval->GetParent()->SetSpillSlot(slot * kVRegSize);
}

}  // namespace art
