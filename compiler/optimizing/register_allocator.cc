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

RegisterAllocator::RegisterAllocator(ArenaAllocator* allocator,
                                     CodeGenerator* codegen,
                                     const SsaLivenessAnalysis& liveness)
      : allocator_(allocator),
        codegen_(codegen),
        liveness_(liveness),
        unhandled_(allocator, 0),
        handled_(allocator, 0),
        active_(allocator, 0),
        inactive_(allocator, 0),
        physical_register_intervals_(allocator, codegen->GetNumberOfRegisters()),
        spill_slots_(allocator, kDefaultNumberOfSpillSlots),
        processing_core_registers_(false),
        number_of_registers_(-1),
        registers_array_(nullptr),
        blocked_registers_(allocator->AllocArray<bool>(codegen->GetNumberOfRegisters())) {
  codegen->SetupBlockedRegisters(blocked_registers_);
  physical_register_intervals_.SetSize(codegen->GetNumberOfRegisters());
}

bool RegisterAllocator::CanAllocateRegistersFor(const HGraph& graph,
                                                InstructionSet instruction_set) {
  if (!Supports(instruction_set)) {
    return false;
  }
  for (size_t i = 0, e = graph.GetBlocks().Size(); i < e; ++i) {
    for (HInstructionIterator it(graph.GetBlocks().Get(i)->GetInstructions());
         !it.Done();
         it.Advance()) {
      HInstruction* current = it.Current();
      if (current->NeedsEnvironment()) return false;
      if (current->GetType() == Primitive::kPrimLong && instruction_set != kX86_64) return false;
      if (current->GetType() == Primitive::kPrimFloat) return false;
      if (current->GetType() == Primitive::kPrimDouble) return false;
    }
  }
  return true;
}

static bool ShouldProcess(bool processing_core_registers, LiveInterval* interval) {
  bool is_core_register = (interval->GetType() != Primitive::kPrimDouble)
      && (interval->GetType() != Primitive::kPrimFloat);
  return processing_core_registers == is_core_register;
}

void RegisterAllocator::AllocateRegisters() {
  processing_core_registers_ = true;
  AllocateRegistersInternal();
  processing_core_registers_ = false;
  AllocateRegistersInternal();

  Resolve();

  if (kIsDebugBuild) {
    processing_core_registers_ = true;
    ValidateInternal(true);
    processing_core_registers_ = false;
    ValidateInternal(true);
  }
}

void RegisterAllocator::BlockRegister(Location location,
                                      size_t start,
                                      size_t end,
                                      Primitive::Type type) {
  int reg = location.reg().RegId();
  LiveInterval* interval = physical_register_intervals_.Get(reg);
  if (interval == nullptr) {
    interval = LiveInterval::MakeFixedInterval(allocator_, reg, type);
    physical_register_intervals_.Put(reg, interval);
    inactive_.Add(interval);
  }
  DCHECK(interval->GetRegister() == reg);
  interval->AddRange(start, end);
}

// TODO: make the register allocator understand instructions like HCondition
// that may not need to be materialized.  It doesn't need to allocate any
// registers for it.
void RegisterAllocator::AllocateRegistersInternal() {
  number_of_registers_ = processing_core_registers_
      ? codegen_->GetNumberOfCoreRegisters()
      : codegen_->GetNumberOfFloatingPointRegisters();

  registers_array_ = allocator_->AllocArray<size_t>(number_of_registers_);

  // Iterate post-order, to ensure the list is sorted, and the last added interval
  // is the one with the lowest start position.
  for (size_t i = liveness_.GetNumberOfSsaValues(); i > 0; --i) {
    HInstruction* instruction = liveness_.GetInstructionFromSsaIndex(i - 1);
    LiveInterval* current = instruction->GetLiveInterval();
    if (ShouldProcess(processing_core_registers_, current)) {
      DCHECK(unhandled_.IsEmpty() || current->StartsBefore(unhandled_.Peek()));

      LocationSummary* locations = instruction->GetLocations();
      if (locations->GetTempCount() != 0) {
        // Note that we already filtered out instructions requiring temporaries in
        // RegisterAllocator::CanAllocateRegistersFor.
        LOG(FATAL) << "Unimplemented";
      }

      // Some instructions define their output in fixed register/stack slot. We need
      // to ensure we know these locations before doing register allocation. For a
      // given register, we create an interval that covers these locations. The register
      // will be unavailable at these locations when trying to allocate one for an
      // interval.
      //
      // The backwards walking ensures the ranges are ordered on increasing start positions.
      Location output = locations->Out();
      size_t position = instruction->GetLifetimePosition();
      if (output.IsRegister()) {
        // Shift the interval's start by one to account for the blocked register.
        current->SetFrom(position + 1);
        current->SetRegister(output.reg().RegId());
        BlockRegister(output, position, position + 1, instruction->GetType());
      } else if (output.IsStackSlot() || output.IsDoubleStackSlot()) {
        current->SetSpillSlot(output.GetStackIndex());
      }
      for (size_t i = 0; i < instruction->InputCount(); ++i) {
        Location input = locations->InAt(i);
        if (input.IsRegister()) {
          BlockRegister(input, position, position + 1, instruction->InputAt(i)->GetType());
        }
      }

      // Add the interval to the correct list.
      if (current->HasRegister()) {
        DCHECK(instruction->IsParameterValue());
        inactive_.Add(current);
      } else if (current->HasSpillSlot() || instruction->IsConstant()) {
        // Split before first register use.
        size_t first_register_use = current->FirstRegisterUse();
        if (first_register_use != kNoLifetime) {
          LiveInterval* split = Split(current, first_register_use - 1);
          // Don't add direclty to `unhandled_`, it needs to be sorted and the start
          // of this new interval might be after intervals already in the list.
          AddToUnhandled(split);
        } else {
          // Nothing to do, we won't allocate a register for this value.
        }
      } else {
        DCHECK(unhandled_.IsEmpty() || current->StartsBefore(unhandled_.Peek()));
        unhandled_.Add(current);
      }
    }
  }

  LinearScan();
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

bool RegisterAllocator::ValidateInternal(bool log_fatal_on_failure) const {
  // To simplify unit testing, we eagerly create the array of intervals, and
  // call the helper method.
  GrowableArray<LiveInterval*> intervals(allocator_, 0);
  for (size_t i = 0; i < liveness_.GetNumberOfSsaValues(); ++i) {
    HInstruction* instruction = liveness_.GetInstructionFromSsaIndex(i);
    if (ShouldProcess(processing_core_registers_, instruction->GetLiveInterval())) {
      intervals.Add(instruction->GetLiveInterval());
    }
  }

  for (size_t i = 0, e = physical_register_intervals_.Size(); i < e; ++i) {
    LiveInterval* fixed = physical_register_intervals_.Get(i);
    if (fixed != nullptr && ShouldProcess(processing_core_registers_, fixed)) {
      intervals.Add(fixed);
    }
  }

  return ValidateIntervals(intervals, spill_slots_.Size(), *codegen_, allocator_,
                           processing_core_registers_, log_fatal_on_failure);
}

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
      HInstruction* defined_by = current->GetParent()->GetDefinedBy();
      if (current->GetParent()->HasSpillSlot()
           // Parameters have their own stack slot.
           && !(defined_by != nullptr && defined_by->IsParameterValue())) {
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

void RegisterAllocator::DumpInterval(std::ostream& stream, LiveInterval* interval) const {
  interval->Dump(stream);
  stream << ": ";
  if (interval->HasRegister()) {
    if (processing_core_registers_) {
      codegen_->DumpCoreRegister(stream, interval->GetRegister());
    } else {
      codegen_->DumpFloatingPointRegister(stream, interval->GetRegister());
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
    DCHECK(!current->IsFixed() && !current->HasRegister() && !current->HasSpillSlot());
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

  // For each active interval, set its register to not free.
  for (size_t i = 0, e = active_.Size(); i < e; ++i) {
    LiveInterval* interval = active_.Get(i);
    DCHECK(interval->HasRegister());
    free_until[interval->GetRegister()] = 0;
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
  if (first_register_use == kNoLifetime) {
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
    if (active->IsFixed()) {
      next_use[active->GetRegister()] = current->GetStart();
    } else {
      size_t use = active->FirstRegisterUseAfter(current->GetStart());
      if (use != kNoLifetime) {
        next_use[active->GetRegister()] = use;
      }
    }
  }

  // For each inactive interval, find the next use of its register after the
  // start of current.
  // Thanks to SSA, this should only be needed for intervals
  // that are the result of a split.
  for (size_t i = 0, e = inactive_.Size(); i < e; ++i) {
    LiveInterval* inactive = inactive_.Get(i);
    DCHECK(inactive->HasRegister());
    size_t next_intersection = inactive->FirstIntersectionWith(current);
    if (next_intersection != kNoLifetime) {
      if (inactive->IsFixed()) {
        next_use[inactive->GetRegister()] =
            std::min(next_intersection, next_use[inactive->GetRegister()]);
      } else {
        size_t use = inactive->FirstRegisterUseAfter(current->GetStart());
        if (use != kNoLifetime) {
          next_use[inactive->GetRegister()] = std::min(use, next_use[inactive->GetRegister()]);
        }
      }
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
        DCHECK(!active->IsFixed());
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
        size_t next_intersection = inactive->FirstIntersectionWith(current);
        if (next_intersection != kNoLifetime) {
          if (inactive->IsFixed()) {
            LiveInterval* split = Split(current, next_intersection);
            AddToUnhandled(split);
          } else {
            LiveInterval* split = Split(inactive, current->GetStart());
            inactive_.DeleteAt(i);
            handled_.Add(inactive);
            AddToUnhandled(split);
            --i;
          }
        }
      }
    }

    return true;
  }
}

void RegisterAllocator::AddToUnhandled(LiveInterval* interval) {
  size_t insert_at = 0;
  for (size_t i = unhandled_.Size(); i > 0; --i) {
    LiveInterval* current = unhandled_.Get(i - 1);
    if (current->StartsAfter(interval)) {
      insert_at = i;
      break;
    }
  }
  unhandled_.InsertAt(insert_at, interval);
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

static bool NeedTwoSpillSlot(Primitive::Type type) {
  return type == Primitive::kPrimLong || type == Primitive::kPrimDouble;
}

void RegisterAllocator::AllocateSpillSlotFor(LiveInterval* interval) {
  LiveInterval* parent = interval->GetParent();

  // An instruction gets a spill slot for its entire lifetime. If the parent
  // of this interval already has a spill slot, there is nothing to do.
  if (parent->HasSpillSlot()) {
    return;
  }

  HInstruction* defined_by = parent->GetDefinedBy();
  if (defined_by->IsParameterValue()) {
    // Parameters have their own stack slot.
    parent->SetSpillSlot(codegen_->GetStackSlotOfParameter(defined_by->AsParameterValue()));
    return;
  }

  if (defined_by->IsConstant()) {
    // Constants don't need a spill slot.
    return;
  }

  LiveInterval* last_sibling = interval;
  while (last_sibling->GetNextSibling() != nullptr) {
    last_sibling = last_sibling->GetNextSibling();
  }
  size_t end = last_sibling->GetEnd();

  if (NeedTwoSpillSlot(parent->GetType())) {
    AllocateTwoSpillSlots(parent, end);
  } else {
    AllocateOneSpillSlot(parent, end);
  }
}

void RegisterAllocator::AllocateTwoSpillSlots(LiveInterval* parent, size_t end) {
  // Find an available spill slot.
  size_t slot = 0;
  for (size_t e = spill_slots_.Size(); slot < e; ++slot) {
    // We check if it is less rather than less or equal because the parallel move
    // resolver does not work when a single spill slot needs to be exchanged with
    // a double spill slot. The strict comparison avoids needing to exchange these
    // locations at the same lifetime position.
    if (spill_slots_.Get(slot) < parent->GetStart()
        && (slot == (e - 1) || spill_slots_.Get(slot + 1) < parent->GetStart())) {
      break;
    }
  }

  if (slot == spill_slots_.Size()) {
    // We need a new spill slot.
    spill_slots_.Add(end);
    spill_slots_.Add(end);
  } else if (slot == spill_slots_.Size() - 1) {
    spill_slots_.Put(slot, end);
    spill_slots_.Add(end);
  } else {
    spill_slots_.Put(slot, end);
    spill_slots_.Put(slot + 1, end);
  }

  parent->SetSpillSlot(slot * kVRegSize);
}

void RegisterAllocator::AllocateOneSpillSlot(LiveInterval* parent, size_t end) {
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

  parent->SetSpillSlot(slot * kVRegSize);
}

static Location ConvertToLocation(LiveInterval* interval) {
  if (interval->HasRegister()) {
    return Location::RegisterLocation(ManagedRegister(interval->GetRegister()));
  } else {
    HInstruction* defined_by = interval->GetParent()->GetDefinedBy();
    if (defined_by->IsConstant()) {
      return defined_by->GetLocations()->Out();
    } else {
      DCHECK(interval->GetParent()->HasSpillSlot());
      if (NeedTwoSpillSlot(interval->GetType())) {
        return Location::DoubleStackSlot(interval->GetParent()->GetSpillSlot());
      } else {
        return Location::StackSlot(interval->GetParent()->GetSpillSlot());
      }
    }
  }
}

// We create a special marker for inputs moves to differentiate them from
// moves created during resolution. They must be different instructions
// because the input moves work on the assumption that the interval moves
// have been executed.
static constexpr size_t kInputMoveLifetimePosition = 0;
static bool IsInputMove(HInstruction* instruction) {
  return instruction->GetLifetimePosition() == kInputMoveLifetimePosition;
}

void RegisterAllocator::AddInputMoveFor(HInstruction* instruction,
                                        Location source,
                                        Location destination) const {
  if (source.Equals(destination)) return;

  DCHECK(instruction->AsPhi() == nullptr);

  HInstruction* previous = instruction->GetPrevious();
  HParallelMove* move = nullptr;
  if (previous == nullptr
      || previous->AsParallelMove() == nullptr
      || !IsInputMove(previous)) {
    move = new (allocator_) HParallelMove(allocator_);
    move->SetLifetimePosition(kInputMoveLifetimePosition);
    instruction->GetBlock()->InsertInstructionBefore(move, instruction);
  } else {
    move = previous->AsParallelMove();
  }
  DCHECK(IsInputMove(move));
  move->AddMove(new (allocator_) MoveOperands(source, destination));
}

void RegisterAllocator::InsertParallelMoveAt(size_t position,
                                             Location source,
                                             Location destination) const {
  if (source.Equals(destination)) return;

  HInstruction* at = liveness_.GetInstructionFromPosition(position / 2);
  if (at == nullptr) {
    // Block boundary, don't no anything the connection of split siblings will handle it.
    return;
  }
  HParallelMove* move;
  if ((position & 1) == 1) {
    // Move must happen after the instruction.
    DCHECK(!at->IsControlFlow());
    move = at->GetNext()->AsParallelMove();
    // This is a parallel move for connecting siblings in a same block. We need to
    // differentiate it with moves for connecting blocks, and input moves.
    if (move == nullptr || move->GetLifetimePosition() != position) {
      move = new (allocator_) HParallelMove(allocator_);
      move->SetLifetimePosition(position);
      at->GetBlock()->InsertInstructionBefore(move, at->GetNext());
    }
  } else {
    // Move must happen before the instruction.
    HInstruction* previous = at->GetPrevious();
    if (previous != nullptr && previous->AsParallelMove() != nullptr) {
      // This is a parallel move for connecting siblings in a same block. We need to
      // differentiate it with moves for connecting blocks, and input moves.
      if (previous->GetLifetimePosition() != position) {
        previous = previous->GetPrevious();
      }
    }
    if (previous == nullptr || previous->AsParallelMove() == nullptr) {
      move = new (allocator_) HParallelMove(allocator_);
      move->SetLifetimePosition(position);
      at->GetBlock()->InsertInstructionBefore(move, at);
    } else {
      move = previous->AsParallelMove();
    }
  }
  move->AddMove(new (allocator_) MoveOperands(source, destination));
}

void RegisterAllocator::InsertParallelMoveAtExitOf(HBasicBlock* block,
                                                   Location source,
                                                   Location destination) const {
  if (source.Equals(destination)) return;

  DCHECK_EQ(block->GetSuccessors().Size(), 1u);
  HInstruction* last = block->GetLastInstruction();
  HInstruction* previous = last->GetPrevious();
  HParallelMove* move;
  // This is a parallel move for connecting blocks. We need to differentiate
  // it with moves for connecting siblings in a same block, and output moves.
  if (previous == nullptr || previous->AsParallelMove() == nullptr
      || previous->AsParallelMove()->GetLifetimePosition() != block->GetLifetimeEnd()) {
    move = new (allocator_) HParallelMove(allocator_);
    move->SetLifetimePosition(block->GetLifetimeEnd());
    block->InsertInstructionBefore(move, last);
  } else {
    move = previous->AsParallelMove();
  }
  move->AddMove(new (allocator_) MoveOperands(source, destination));
}

void RegisterAllocator::InsertParallelMoveAtEntryOf(HBasicBlock* block,
                                                    Location source,
                                                    Location destination) const {
  if (source.Equals(destination)) return;

  HInstruction* first = block->GetFirstInstruction();
  HParallelMove* move = first->AsParallelMove();
  // This is a parallel move for connecting blocks. We need to differentiate
  // it with moves for connecting siblings in a same block, and input moves.
  if (move == nullptr || move->GetLifetimePosition() != block->GetLifetimeStart()) {
    move = new (allocator_) HParallelMove(allocator_);
    move->SetLifetimePosition(block->GetLifetimeStart());
    block->InsertInstructionBefore(move, first);
  }
  move->AddMove(new (allocator_) MoveOperands(source, destination));
}

void RegisterAllocator::InsertMoveAfter(HInstruction* instruction,
                                        Location source,
                                        Location destination) const {
  if (source.Equals(destination)) return;

  if (instruction->AsPhi() != nullptr) {
    InsertParallelMoveAtEntryOf(instruction->GetBlock(), source, destination);
    return;
  }

  size_t position = instruction->GetLifetimePosition() + 1;
  HParallelMove* move = instruction->GetNext()->AsParallelMove();
  // This is a parallel move for moving the output of an instruction. We need
  // to differentiate with input moves, moves for connecting siblings in a
  // and moves for connecting blocks.
  if (move == nullptr || move->GetLifetimePosition() != position) {
    move = new (allocator_) HParallelMove(allocator_);
    move->SetLifetimePosition(position);
    instruction->GetBlock()->InsertInstructionBefore(move, instruction->GetNext());
  }
  move->AddMove(new (allocator_) MoveOperands(source, destination));
}

void RegisterAllocator::ConnectSiblings(LiveInterval* interval) {
  LiveInterval* current = interval;
  if (current->HasSpillSlot() && current->HasRegister()) {
    // We spill eagerly, so move must be at definition.
    InsertMoveAfter(interval->GetDefinedBy(),
                    Location::RegisterLocation(ManagedRegister(interval->GetRegister())),
                    NeedTwoSpillSlot(interval->GetType())
                        ? Location::DoubleStackSlot(interval->GetParent()->GetSpillSlot())
                        : Location::StackSlot(interval->GetParent()->GetSpillSlot()));
  }
  UsePosition* use = current->GetFirstUse();

  // Walk over all siblings, updating locations of use positions, and
  // connecting them when they are adjacent.
  do {
    Location source = ConvertToLocation(current);

    // Walk over all uses covered by this interval, and update the location
    // information.
    while (use != nullptr && use->GetPosition() <= current->GetEnd()) {
      if (!use->GetIsEnvironment()) {
        LocationSummary* locations = use->GetUser()->GetLocations();
        Location expected_location = locations->InAt(use->GetInputIndex());
        if (expected_location.IsUnallocated()) {
          locations->SetInAt(use->GetInputIndex(), source);
        } else {
          AddInputMoveFor(use->GetUser(), source, expected_location);
        }
      }
      use = use->GetNext();
    }

    // If the next interval starts just after this one, and has a register,
    // insert a move.
    LiveInterval* next_sibling = current->GetNextSibling();
    if (next_sibling != nullptr
        && next_sibling->HasRegister()
        && current->GetEnd() == next_sibling->GetStart()) {
      Location destination = ConvertToLocation(next_sibling);
      InsertParallelMoveAt(current->GetEnd(), source, destination);
    }
    current = next_sibling;
  } while (current != nullptr);
  DCHECK(use == nullptr);
}

void RegisterAllocator::ConnectSplitSiblings(LiveInterval* interval,
                                             HBasicBlock* from,
                                             HBasicBlock* to) const {
  if (interval->GetNextSibling() == nullptr) {
    // Nothing to connect. The whole range was allocated to the same location.
    return;
  }

  size_t from_position = from->GetLifetimeEnd() - 1;
  size_t to_position = to->GetLifetimeStart();

  LiveInterval* destination = nullptr;
  LiveInterval* source = nullptr;

  LiveInterval* current = interval;

  // Check the intervals that cover `from` and `to`.
  while ((current != nullptr) && (source == nullptr || destination == nullptr)) {
    if (current->Covers(from_position)) {
      DCHECK(source == nullptr);
      source = current;
    }
    if (current->Covers(to_position)) {
      DCHECK(destination == nullptr);
      destination = current;
    }

    current = current->GetNextSibling();
  }

  if (destination == source) {
    // Interval was not split.
    return;
  }

  if (!destination->HasRegister()) {
    // Values are eagerly spilled. Spill slot already contains appropriate value.
    return;
  }

  // If `from` has only one successor, we can put the moves at the exit of it. Otherwise
  // we need to put the moves at the entry of `to`.
  if (from->GetSuccessors().Size() == 1) {
    InsertParallelMoveAtExitOf(from, ConvertToLocation(source), ConvertToLocation(destination));
  } else {
    DCHECK_EQ(to->GetPredecessors().Size(), 1u);
    InsertParallelMoveAtEntryOf(to, ConvertToLocation(source), ConvertToLocation(destination));
  }
}

// Returns the location of `interval`, or siblings of `interval`, at `position`.
static Location FindLocationAt(LiveInterval* interval, size_t position) {
  LiveInterval* current = interval;
  while (!current->Covers(position)) {
    current = current->GetNextSibling();
    DCHECK(current != nullptr);
  }
  return ConvertToLocation(current);
}

void RegisterAllocator::Resolve() {
  codegen_->ComputeFrameSize(spill_slots_.Size());

  // Adjust the Out Location of instructions.
  // TODO: Use pointers of Location inside LiveInterval to avoid doing another iteration.
  for (size_t i = 0, e = liveness_.GetNumberOfSsaValues(); i < e; ++i) {
    HInstruction* instruction = liveness_.GetInstructionFromSsaIndex(i);
    LiveInterval* current = instruction->GetLiveInterval();
    LocationSummary* locations = instruction->GetLocations();
    Location location = locations->Out();
    if (instruction->AsParameterValue() != nullptr) {
      // Now that we know the frame size, adjust the parameter's location.
      if (location.IsStackSlot()) {
        location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
        current->SetSpillSlot(location.GetStackIndex());
        locations->SetOut(location);
      } else if (location.IsDoubleStackSlot()) {
        location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
        current->SetSpillSlot(location.GetStackIndex());
        locations->SetOut(location);
      } else if (current->HasSpillSlot()) {
        current->SetSpillSlot(current->GetSpillSlot() + codegen_->GetFrameSize());
      }
    }

    Location source = ConvertToLocation(current);

    if (location.IsUnallocated()) {
      if (location.GetPolicy() == Location::kSameAsFirstInput) {
        locations->SetInAt(0, source);
      }
      locations->SetOut(source);
    } else {
      DCHECK(source.Equals(location));
    }
  }

  // Connect siblings.
  for (size_t i = 0, e = liveness_.GetNumberOfSsaValues(); i < e; ++i) {
    HInstruction* instruction = liveness_.GetInstructionFromSsaIndex(i);
    ConnectSiblings(instruction->GetLiveInterval());
  }

  // Resolve non-linear control flow across branches. Order does not matter.
  for (HLinearOrderIterator it(liveness_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    BitVector* live = liveness_.GetLiveInSet(*block);
    for (uint32_t idx : live->Indexes()) {
      HInstruction* current = liveness_.GetInstructionFromSsaIndex(idx);
      LiveInterval* interval = current->GetLiveInterval();
      for (size_t i = 0, e = block->GetPredecessors().Size(); i < e; ++i) {
        ConnectSplitSiblings(interval, block->GetPredecessors().Get(i), block);
      }
    }
  }

  // Resolve phi inputs. Order does not matter.
  for (HLinearOrderIterator it(liveness_); !it.Done(); it.Advance()) {
    HBasicBlock* current = it.Current();
    for (HInstructionIterator it(current->GetPhis()); !it.Done(); it.Advance()) {
      HInstruction* phi = it.Current();
      for (size_t i = 0, e = current->GetPredecessors().Size(); i < e; ++i) {
        HBasicBlock* predecessor = current->GetPredecessors().Get(i);
        DCHECK_EQ(predecessor->GetSuccessors().Size(), 1u);
        HInstruction* input = phi->InputAt(i);
        Location source = FindLocationAt(input->GetLiveInterval(),
                                         predecessor->GetLastInstruction()->GetLifetimePosition());
        Location destination = ConvertToLocation(phi->GetLiveInterval());
        InsertParallelMoveAtExitOf(predecessor, source, destination);
      }
    }
  }
}

}  // namespace art
