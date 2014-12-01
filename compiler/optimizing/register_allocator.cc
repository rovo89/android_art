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

#include <sstream>

#include "base/bit_vector-inl.h"
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
        unhandled_core_intervals_(allocator, 0),
        unhandled_fp_intervals_(allocator, 0),
        unhandled_(nullptr),
        handled_(allocator, 0),
        active_(allocator, 0),
        inactive_(allocator, 0),
        physical_core_register_intervals_(allocator, codegen->GetNumberOfCoreRegisters()),
        physical_fp_register_intervals_(allocator, codegen->GetNumberOfFloatingPointRegisters()),
        temp_intervals_(allocator, 4),
        spill_slots_(allocator, kDefaultNumberOfSpillSlots),
        safepoints_(allocator, 0),
        processing_core_registers_(false),
        number_of_registers_(-1),
        registers_array_(nullptr),
        blocked_core_registers_(codegen->GetBlockedCoreRegisters()),
        blocked_fp_registers_(codegen->GetBlockedFloatingPointRegisters()),
        reserved_out_slots_(0),
        maximum_number_of_live_registers_(0) {
  codegen->SetupBlockedRegisters();
  physical_core_register_intervals_.SetSize(codegen->GetNumberOfCoreRegisters());
  physical_fp_register_intervals_.SetSize(codegen->GetNumberOfFloatingPointRegisters());
  // Always reserve for the current method and the graph's max out registers.
  // TODO: compute it instead.
  reserved_out_slots_ = 1 + codegen->GetGraph()->GetMaximumNumberOfOutVRegs();
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
      if (current->GetType() == Primitive::kPrimLong && instruction_set != kX86_64) return false;
      if ((current->GetType() == Primitive::kPrimFloat
           || current->GetType() == Primitive::kPrimDouble)
          && instruction_set != kX86_64) {
        return false;
      }
    }
  }
  return true;
}

static bool ShouldProcess(bool processing_core_registers, LiveInterval* interval) {
  if (interval == nullptr) return false;
  bool is_core_register = (interval->GetType() != Primitive::kPrimDouble)
      && (interval->GetType() != Primitive::kPrimFloat);
  return processing_core_registers == is_core_register;
}

void RegisterAllocator::AllocateRegisters() {
  AllocateRegistersInternal();
  Resolve();

  if (kIsDebugBuild) {
    processing_core_registers_ = true;
    ValidateInternal(true);
    processing_core_registers_ = false;
    ValidateInternal(true);
    // Check that the linear order is still correct with regards to lifetime positions.
    // Since only parallel moves have been inserted during the register allocation,
    // these checks are mostly for making sure these moves have been added correctly.
    size_t current_liveness = 0;
    for (HLinearOrderIterator it(liveness_); !it.Done(); it.Advance()) {
      HBasicBlock* block = it.Current();
      for (HInstructionIterator inst_it(block->GetPhis()); !inst_it.Done(); inst_it.Advance()) {
        HInstruction* instruction = inst_it.Current();
        DCHECK_LE(current_liveness, instruction->GetLifetimePosition());
        current_liveness = instruction->GetLifetimePosition();
      }
      for (HInstructionIterator inst_it(block->GetInstructions());
           !inst_it.Done();
           inst_it.Advance()) {
        HInstruction* instruction = inst_it.Current();
        DCHECK_LE(current_liveness, instruction->GetLifetimePosition()) << instruction->DebugName();
        current_liveness = instruction->GetLifetimePosition();
      }
    }
  }
}

void RegisterAllocator::BlockRegister(Location location,
                                      size_t start,
                                      size_t end) {
  int reg = location.reg();
  DCHECK(location.IsRegister() || location.IsFpuRegister());
  LiveInterval* interval = location.IsRegister()
      ? physical_core_register_intervals_.Get(reg)
      : physical_fp_register_intervals_.Get(reg);
  Primitive::Type type = location.IsRegister()
      ? Primitive::kPrimInt
      : Primitive::kPrimDouble;
  if (interval == nullptr) {
    interval = LiveInterval::MakeFixedInterval(allocator_, reg, type);
    if (location.IsRegister()) {
      physical_core_register_intervals_.Put(reg, interval);
    } else {
      physical_fp_register_intervals_.Put(reg, interval);
    }
  }
  DCHECK(interval->GetRegister() == reg);
  interval->AddRange(start, end);
}

void RegisterAllocator::AllocateRegistersInternal() {
  // Iterate post-order, to ensure the list is sorted, and the last added interval
  // is the one with the lowest start position.
  for (HLinearPostOrderIterator it(liveness_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    for (HBackwardInstructionIterator back_it(block->GetInstructions()); !back_it.Done();
         back_it.Advance()) {
      ProcessInstruction(back_it.Current());
    }
    for (HInstructionIterator inst_it(block->GetPhis()); !inst_it.Done(); inst_it.Advance()) {
      ProcessInstruction(inst_it.Current());
    }
  }

  number_of_registers_ = codegen_->GetNumberOfCoreRegisters();
  registers_array_ = allocator_->AllocArray<size_t>(number_of_registers_);
  processing_core_registers_ = true;
  unhandled_ = &unhandled_core_intervals_;
  for (size_t i = 0, e = physical_core_register_intervals_.Size(); i < e; ++i) {
    LiveInterval* fixed = physical_core_register_intervals_.Get(i);
    if (fixed != nullptr) {
      // Fixed interval is added to inactive_ instead of unhandled_.
      // It's also the only type of inactive interval whose start position
      // can be after the current interval during linear scan.
      // Fixed interval is never split and never moves to unhandled_.
      inactive_.Add(fixed);
    }
  }
  LinearScan();

  size_t saved_maximum_number_of_live_registers = maximum_number_of_live_registers_;
  maximum_number_of_live_registers_ = 0;

  inactive_.Reset();
  active_.Reset();
  handled_.Reset();

  number_of_registers_ = codegen_->GetNumberOfFloatingPointRegisters();
  registers_array_ = allocator_->AllocArray<size_t>(number_of_registers_);
  processing_core_registers_ = false;
  unhandled_ = &unhandled_fp_intervals_;
  for (size_t i = 0, e = physical_fp_register_intervals_.Size(); i < e; ++i) {
    LiveInterval* fixed = physical_fp_register_intervals_.Get(i);
    if (fixed != nullptr) {
      // Fixed interval is added to inactive_ instead of unhandled_.
      // It's also the only type of inactive interval whose start position
      // can be after the current interval during linear scan.
      // Fixed interval is never split and never moves to unhandled_.
      inactive_.Add(fixed);
    }
  }
  LinearScan();
  maximum_number_of_live_registers_ += saved_maximum_number_of_live_registers;
}

void RegisterAllocator::ProcessInstruction(HInstruction* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  size_t position = instruction->GetLifetimePosition();

  if (locations == nullptr) return;

  // Create synthesized intervals for temporaries.
  for (size_t i = 0; i < locations->GetTempCount(); ++i) {
    Location temp = locations->GetTemp(i);
    if (temp.IsRegister() || temp.IsFpuRegister()) {
      BlockRegister(temp, position, position + 1);
    } else {
      DCHECK(temp.IsUnallocated());
      switch (temp.GetPolicy()) {
        case Location::kRequiresRegister: {
          LiveInterval* interval =
              LiveInterval::MakeTempInterval(allocator_, Primitive::kPrimInt);
          temp_intervals_.Add(interval);
          interval->AddRange(position, position + 1);
          unhandled_core_intervals_.Add(interval);
          break;
        }

        case Location::kRequiresFpuRegister: {
          LiveInterval* interval =
              LiveInterval::MakeTempInterval(allocator_, Primitive::kPrimDouble);
          temp_intervals_.Add(interval);
          interval->AddRange(position, position + 1);
          unhandled_fp_intervals_.Add(interval);
          break;
        }

        default:
          LOG(FATAL) << "Unexpected policy for temporary location "
                     << temp.GetPolicy();
      }
    }
  }

  bool core_register = (instruction->GetType() != Primitive::kPrimDouble)
      && (instruction->GetType() != Primitive::kPrimFloat);

  if (locations->CanCall()) {
    if (!instruction->IsSuspendCheck()) {
      codegen_->MarkNotLeaf();
    }
    safepoints_.Add(instruction);
    if (locations->OnlyCallsOnSlowPath()) {
      // We add a synthesized range at this position to record the live registers
      // at this position. Ideally, we could just update the safepoints when locations
      // are updated, but we currently need to know the full stack size before updating
      // locations (because of parameters and the fact that we don't have a frame pointer).
      // And knowing the full stack size requires to know the maximum number of live
      // registers at calls in slow paths.
      // By adding the following interval in the algorithm, we can compute this
      // maximum before updating locations.
      LiveInterval* interval = LiveInterval::MakeSlowPathInterval(allocator_, instruction);
      interval->AddRange(position, position + 1);
      AddSorted(&unhandled_core_intervals_, interval);
      AddSorted(&unhandled_fp_intervals_, interval);
    }
  }

  if (locations->WillCall()) {
    // Block all registers.
    for (size_t i = 0; i < codegen_->GetNumberOfCoreRegisters(); ++i) {
      BlockRegister(Location::RegisterLocation(i),
                    position,
                    position + 1);
    }
    for (size_t i = 0; i < codegen_->GetNumberOfFloatingPointRegisters(); ++i) {
      BlockRegister(Location::FpuRegisterLocation(i),
                    position,
                    position + 1);
    }
  }

  for (size_t i = 0; i < instruction->InputCount(); ++i) {
    Location input = locations->InAt(i);
    if (input.IsRegister() || input.IsFpuRegister()) {
      BlockRegister(input, position, position + 1);
    }
  }

  LiveInterval* current = instruction->GetLiveInterval();
  if (current == nullptr) return;

  GrowableArray<LiveInterval*>& unhandled = core_register
      ? unhandled_core_intervals_
      : unhandled_fp_intervals_;

  DCHECK(unhandled.IsEmpty() || current->StartsBeforeOrAt(unhandled.Peek()));

  // Some instructions define their output in fixed register/stack slot. We need
  // to ensure we know these locations before doing register allocation. For a
  // given register, we create an interval that covers these locations. The register
  // will be unavailable at these locations when trying to allocate one for an
  // interval.
  //
  // The backwards walking ensures the ranges are ordered on increasing start positions.
  Location output = locations->Out();
  if (output.IsUnallocated() && output.GetPolicy() == Location::kSameAsFirstInput) {
    Location first = locations->InAt(0);
    if (first.IsRegister() || first.IsFpuRegister()) {
      current->SetFrom(position + 1);
      current->SetRegister(first.reg());
    }
  } else if (output.IsRegister() || output.IsFpuRegister()) {
    // Shift the interval's start by one to account for the blocked register.
    current->SetFrom(position + 1);
    current->SetRegister(output.reg());
    BlockRegister(output, position, position + 1);
  } else if (output.IsStackSlot() || output.IsDoubleStackSlot()) {
    current->SetSpillSlot(output.GetStackIndex());
  }

  // If needed, add interval to the list of unhandled intervals.
  if (current->HasSpillSlot() || instruction->IsConstant()) {
    // Split just before first register use.
    size_t first_register_use = current->FirstRegisterUse();
    if (first_register_use != kNoLifetime) {
      LiveInterval* split = Split(current, first_register_use - 1);
      // Don't add directly to `unhandled`, it needs to be sorted and the start
      // of this new interval might be after intervals already in the list.
      AddSorted(&unhandled, split);
    } else {
      // Nothing to do, we won't allocate a register for this value.
    }
  } else {
    // Don't add directly to `unhandled`, temp or safepoint intervals
    // for this instruction may have been added, and those can be
    // processed first.
    AddSorted(&unhandled, current);
  }
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

  if (processing_core_registers_) {
    for (size_t i = 0, e = physical_core_register_intervals_.Size(); i < e; ++i) {
      LiveInterval* fixed = physical_core_register_intervals_.Get(i);
      if (fixed != nullptr) {
        intervals.Add(fixed);
      }
    }
  } else {
    for (size_t i = 0, e = physical_fp_register_intervals_.Size(); i < e; ++i) {
      LiveInterval* fixed = physical_fp_register_intervals_.Get(i);
      if (fixed != nullptr) {
        intervals.Add(fixed);
      }
    }
  }

  for (size_t i = 0, e = temp_intervals_.Size(); i < e; ++i) {
    LiveInterval* temp = temp_intervals_.Get(i);
    if (ShouldProcess(processing_core_registers_, temp)) {
      intervals.Add(temp);
    }
  }

  return ValidateIntervals(intervals, spill_slots_.Size(), reserved_out_slots_, *codegen_,
                           allocator_, processing_core_registers_, log_fatal_on_failure);
}

bool RegisterAllocator::ValidateIntervals(const GrowableArray<LiveInterval*>& intervals,
                                          size_t number_of_spill_slots,
                                          size_t number_of_out_slots,
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
        BitVector* liveness_of_spill_slot = liveness_of_values.Get(number_of_registers
            + current->GetParent()->GetSpillSlot() / kVRegSize
            - number_of_out_slots);
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
              message << "Register conflict at " << j << " ";
              if (defined_by != nullptr) {
                message << "(" << defined_by->DebugName() << ")";
              }
              message << "for ";
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
    if (interval->IsFloatingPoint()) {
      codegen_->DumpFloatingPointRegister(stream, interval->GetRegister());
    } else {
      codegen_->DumpCoreRegister(stream, interval->GetRegister());
    }
  } else {
    stream << "spilled";
  }
  stream << std::endl;
}

void RegisterAllocator::DumpAllIntervals(std::ostream& stream) const {
  stream << "inactive: " << std::endl;
  for (size_t i = 0; i < inactive_.Size(); i ++) {
    DumpInterval(stream, inactive_.Get(i));
  }
  stream << "active: " << std::endl;
  for (size_t i = 0; i < active_.Size(); i ++) {
    DumpInterval(stream, active_.Get(i));
  }
  stream << "unhandled: " << std::endl;
  auto unhandled = (unhandled_ != nullptr) ?
      unhandled_ : &unhandled_core_intervals_;
  for (size_t i = 0; i < unhandled->Size(); i ++) {
    DumpInterval(stream, unhandled->Get(i));
  }
  stream << "handled: " << std::endl;
  for (size_t i = 0; i < handled_.Size(); i ++) {
    DumpInterval(stream, handled_.Get(i));
  }
}

// By the book implementation of a linear scan register allocator.
void RegisterAllocator::LinearScan() {
  while (!unhandled_->IsEmpty()) {
    // (1) Remove interval with the lowest start position from unhandled.
    LiveInterval* current = unhandled_->Pop();
    DCHECK(!current->IsFixed() && !current->HasSpillSlot());
    DCHECK(unhandled_->IsEmpty() || unhandled_->Peek()->GetStart() >= current->GetStart());

    size_t position = current->GetStart();

    // Remember the inactive_ size here since the ones moved to inactive_ from
    // active_ below shouldn't need to be re-checked.
    size_t inactive_intervals_to_handle = inactive_.Size();

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
    for (size_t i = 0; i < inactive_intervals_to_handle; ++i) {
      LiveInterval* interval = inactive_.Get(i);
      DCHECK(interval->GetStart() < position || interval->IsFixed());
      if (interval->IsDeadAt(position)) {
        inactive_.Delete(interval);
        --i;
        --inactive_intervals_to_handle;
        handled_.Add(interval);
      } else if (interval->Covers(position)) {
        inactive_.Delete(interval);
        --i;
        --inactive_intervals_to_handle;
        active_.Add(interval);
      }
    }

    if (current->IsSlowPathSafepoint()) {
      // Synthesized interval to record the maximum number of live registers
      // at safepoints. No need to allocate a register for it.
      maximum_number_of_live_registers_ =
          std::max(maximum_number_of_live_registers_, active_.Size());
      DCHECK(unhandled_->IsEmpty() || unhandled_->Peek()->GetStart() > current->GetStart());
      continue;
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
  for (size_t i = 0, e = inactive_.Size(); i < e; ++i) {
    LiveInterval* inactive = inactive_.Get(i);
    // Temp/Slow-path-safepoint interval has no holes.
    DCHECK(!inactive->IsTemp() && !inactive->IsSlowPathSafepoint());
    if (!current->IsSplit() && !inactive->IsFixed()) {
      // Neither current nor inactive are fixed.
      // Thanks to SSA, a non-split interval starting in a hole of an
      // inactive interval should never intersect with that inactive interval.
      // Only if it's not fixed though, because fixed intervals don't come from SSA.
      DCHECK_EQ(inactive->FirstIntersectionWith(current), kNoLifetime);
      continue;
    }

    DCHECK(inactive->HasRegister());
    if (free_until[inactive->GetRegister()] == 0) {
      // Already used by some active interval. No need to intersect.
      continue;
    }
    size_t next_intersection = inactive->FirstIntersectionWith(current);
    if (next_intersection != kNoLifetime) {
      free_until[inactive->GetRegister()] =
          std::min(free_until[inactive->GetRegister()], next_intersection);
    }
  }

  int reg = -1;
  if (current->HasRegister()) {
    // Some instructions have a fixed register output.
    reg = current->GetRegister();
    DCHECK_NE(free_until[reg], 0u);
  } else {
    int hint = current->FindFirstRegisterHint(free_until);
    if (hint != kNoRegister) {
      DCHECK(!IsBlocked(hint));
      reg = hint;
    } else {
      // Pick the register that is free the longest.
      for (size_t i = 0; i < number_of_registers_; ++i) {
        if (IsBlocked(i)) continue;
        if (reg == -1 || free_until[i] > free_until[reg]) {
          reg = i;
          if (free_until[i] == kMaxLifetimePosition) break;
        }
      }
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
    AddSorted(unhandled_, split);
  }
  return true;
}

bool RegisterAllocator::IsBlocked(int reg) const {
  return processing_core_registers_
      ? blocked_core_registers_[reg]
      : blocked_fp_registers_[reg];
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
  for (size_t i = 0, e = inactive_.Size(); i < e; ++i) {
    LiveInterval* inactive = inactive_.Get(i);
    // Temp/Slow-path-safepoint interval has no holes.
    DCHECK(!inactive->IsTemp() && !inactive->IsSlowPathSafepoint());
    if (!current->IsSplit() && !inactive->IsFixed()) {
      // Neither current nor inactive are fixed.
      // Thanks to SSA, a non-split interval starting in a hole of an
      // inactive interval should never intersect with that inactive interval.
      // Only if it's not fixed though, because fixed intervals don't come from SSA.
      DCHECK_EQ(inactive->FirstIntersectionWith(current), kNoLifetime);
      continue;
    }
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
    DCHECK_NE(current, split) << "There is not enough registers available for "
      << split->GetParent()->GetDefinedBy()->DebugName();
    AddSorted(unhandled_, split);
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
        AddSorted(unhandled_, split);
        break;
      }
    }

    for (size_t i = 0, e = inactive_.Size(); i < e; ++i) {
      LiveInterval* inactive = inactive_.Get(i);
      if (inactive->GetRegister() == reg) {
        if (!current->IsSplit() && !inactive->IsFixed()) {
          // Neither current nor inactive are fixed.
          // Thanks to SSA, a non-split interval starting in a hole of an
          // inactive interval should never intersect with that inactive interval.
          // Only if it's not fixed though, because fixed intervals don't come from SSA.
          DCHECK_EQ(inactive->FirstIntersectionWith(current), kNoLifetime);
          continue;
        }
        size_t next_intersection = inactive->FirstIntersectionWith(current);
        if (next_intersection != kNoLifetime) {
          if (inactive->IsFixed()) {
            LiveInterval* split = Split(current, next_intersection);
            AddSorted(unhandled_, split);
          } else {
            LiveInterval* split = Split(inactive, next_intersection);
            inactive_.DeleteAt(i);
            --i;
            --e;
            handled_.Add(inactive);
            AddSorted(unhandled_, split);
          }
        }
      }
    }

    return true;
  }
}

void RegisterAllocator::AddSorted(GrowableArray<LiveInterval*>* array, LiveInterval* interval) {
  DCHECK(!interval->IsFixed() && !interval->HasSpillSlot());
  size_t insert_at = 0;
  for (size_t i = array->Size(); i > 0; --i) {
    LiveInterval* current = array->Get(i - 1);
    if (current->StartsAfter(interval)) {
      insert_at = i;
      break;
    } else if ((current->GetStart() == interval->GetStart()) && current->IsSlowPathSafepoint()) {
      // Ensure the slow path interval is the last to be processed at its location: we want the
      // interval to know all live registers at this location.
      DCHECK(i == 1 || array->Get(i - 2)->StartsAfter(current));
      insert_at = i;
      break;
    }
  }
  array->InsertAt(insert_at, interval);
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

  if (parent->NeedsTwoSpillSlots()) {
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
  } else {
    if (slot == spill_slots_.Size()) {
      // We need a new spill slot.
      spill_slots_.Add(end);
    } else {
      spill_slots_.Put(slot, end);
    }
  }

  parent->SetSpillSlot((slot + reserved_out_slots_) * kVRegSize);
}

static bool IsValidDestination(Location destination) {
  return destination.IsRegister()
      || destination.IsFpuRegister()
      || destination.IsStackSlot()
      || destination.IsDoubleStackSlot();
}

void RegisterAllocator::AddInputMoveFor(HInstruction* user,
                                        Location source,
                                        Location destination) const {
  DCHECK(IsValidDestination(destination));
  if (source.Equals(destination)) return;

  DCHECK(!user->IsPhi());

  HInstruction* previous = user->GetPrevious();
  HParallelMove* move = nullptr;
  if (previous == nullptr
      || !previous->IsParallelMove()
      || previous->GetLifetimePosition() < user->GetLifetimePosition()) {
    move = new (allocator_) HParallelMove(allocator_);
    move->SetLifetimePosition(user->GetLifetimePosition());
    user->GetBlock()->InsertInstructionBefore(move, user);
  } else {
    move = previous->AsParallelMove();
  }
  DCHECK_EQ(move->GetLifetimePosition(), user->GetLifetimePosition());
  move->AddMove(new (allocator_) MoveOperands(source, destination, nullptr));
}

static bool IsInstructionStart(size_t position) {
  return (position & 1) == 0;
}

static bool IsInstructionEnd(size_t position) {
  return (position & 1) == 1;
}

void RegisterAllocator::InsertParallelMoveAt(size_t position,
                                             HInstruction* instruction,
                                             Location source,
                                             Location destination) const {
  DCHECK(IsValidDestination(destination));
  if (source.Equals(destination)) return;

  HInstruction* at = liveness_.GetInstructionFromPosition(position / 2);
  HParallelMove* move;
  if (at == nullptr) {
    if (IsInstructionStart(position)) {
      // Block boundary, don't do anything the connection of split siblings will handle it.
      return;
    } else {
      // Move must happen before the first instruction of the block.
      at = liveness_.GetInstructionFromPosition((position + 1) / 2);
      // Note that parallel moves may have already been inserted, so we explicitly
      // ask for the first instruction of the block: `GetInstructionFromPosition` does
      // not contain the moves.
      at = at->GetBlock()->GetFirstInstruction();
      if (at->GetLifetimePosition() != position) {
        DCHECK_GT(at->GetLifetimePosition(), position);
        move = new (allocator_) HParallelMove(allocator_);
        move->SetLifetimePosition(position);
        at->GetBlock()->InsertInstructionBefore(move, at);
      } else {
        DCHECK(at->IsParallelMove());
        move = at->AsParallelMove();
      }
    }
  } else if (IsInstructionEnd(position)) {
    // Move must happen after the instruction.
    DCHECK(!at->IsControlFlow());
    move = at->GetNext()->AsParallelMove();
    // This is a parallel move for connecting siblings in a same block. We need to
    // differentiate it with moves for connecting blocks, and input moves.
    if (move == nullptr || move->GetLifetimePosition() > position) {
      move = new (allocator_) HParallelMove(allocator_);
      move->SetLifetimePosition(position);
      at->GetBlock()->InsertInstructionBefore(move, at->GetNext());
    }
  } else {
    // Move must happen before the instruction.
    HInstruction* previous = at->GetPrevious();
    if (previous == nullptr
        || !previous->IsParallelMove()
        || previous->GetLifetimePosition() != position) {
      // If the previous is a parallel move, then its position must be lower
      // than the given `position`: it was added just after the non-parallel
      // move instruction that precedes `instruction`.
      DCHECK(previous == nullptr
             || !previous->IsParallelMove()
             || previous->GetLifetimePosition() < position);
      move = new (allocator_) HParallelMove(allocator_);
      move->SetLifetimePosition(position);
      at->GetBlock()->InsertInstructionBefore(move, at);
    } else {
      move = previous->AsParallelMove();
    }
  }
  DCHECK_EQ(move->GetLifetimePosition(), position);
  move->AddMove(new (allocator_) MoveOperands(source, destination, instruction));
}

void RegisterAllocator::InsertParallelMoveAtExitOf(HBasicBlock* block,
                                                   HInstruction* instruction,
                                                   Location source,
                                                   Location destination) const {
  DCHECK(IsValidDestination(destination));
  if (source.Equals(destination)) return;

  DCHECK_EQ(block->GetSuccessors().Size(), 1u);
  HInstruction* last = block->GetLastInstruction();
  // We insert moves at exit for phi predecessors and connecting blocks.
  // A block ending with an if cannot branch to a block with phis because
  // we do not allow critical edges. It can also not connect
  // a split interval between two blocks: the move has to happen in the successor.
  DCHECK(!last->IsIf());
  HInstruction* previous = last->GetPrevious();
  HParallelMove* move;
  // This is a parallel move for connecting blocks. We need to differentiate
  // it with moves for connecting siblings in a same block, and output moves.
  size_t position = last->GetLifetimePosition();
  if (previous == nullptr || !previous->IsParallelMove()
      || previous->AsParallelMove()->GetLifetimePosition() != position) {
    move = new (allocator_) HParallelMove(allocator_);
    move->SetLifetimePosition(position);
    block->InsertInstructionBefore(move, last);
  } else {
    move = previous->AsParallelMove();
  }
  move->AddMove(new (allocator_) MoveOperands(source, destination, instruction));
}

void RegisterAllocator::InsertParallelMoveAtEntryOf(HBasicBlock* block,
                                                    HInstruction* instruction,
                                                    Location source,
                                                    Location destination) const {
  DCHECK(IsValidDestination(destination));
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
  move->AddMove(new (allocator_) MoveOperands(source, destination, instruction));
}

void RegisterAllocator::InsertMoveAfter(HInstruction* instruction,
                                        Location source,
                                        Location destination) const {
  DCHECK(IsValidDestination(destination));
  if (source.Equals(destination)) return;

  if (instruction->IsPhi()) {
    InsertParallelMoveAtEntryOf(instruction->GetBlock(), instruction, source, destination);
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
  move->AddMove(new (allocator_) MoveOperands(source, destination, instruction));
}

void RegisterAllocator::ConnectSiblings(LiveInterval* interval) {
  LiveInterval* current = interval;
  if (current->HasSpillSlot() && current->HasRegister()) {
    // We spill eagerly, so move must be at definition.
    InsertMoveAfter(interval->GetDefinedBy(),
                    interval->IsFloatingPoint()
                        ? Location::FpuRegisterLocation(interval->GetRegister())
                        : Location::RegisterLocation(interval->GetRegister()),
                    interval->NeedsTwoSpillSlots()
                        ? Location::DoubleStackSlot(interval->GetParent()->GetSpillSlot())
                        : Location::StackSlot(interval->GetParent()->GetSpillSlot()));
  }
  UsePosition* use = current->GetFirstUse();

  // Walk over all siblings, updating locations of use positions, and
  // connecting them when they are adjacent.
  do {
    Location source = current->ToLocation();

    // Walk over all uses covered by this interval, and update the location
    // information.
    while (use != nullptr && use->GetPosition() <= current->GetEnd()) {
      LocationSummary* locations = use->GetUser()->GetLocations();
      if (use->GetIsEnvironment()) {
        locations->SetEnvironmentAt(use->GetInputIndex(), source);
      } else {
        Location expected_location = locations->InAt(use->GetInputIndex());
        if (expected_location.IsUnallocated()) {
          locations->SetInAt(use->GetInputIndex(), source);
        } else if (!expected_location.IsConstant()) {
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
      Location destination = next_sibling->ToLocation();
      InsertParallelMoveAt(current->GetEnd(), interval->GetDefinedBy(), source, destination);
    }

    // At each safepoint, we record stack and register information.
    for (size_t i = 0, e = safepoints_.Size(); i < e; ++i) {
      HInstruction* safepoint = safepoints_.Get(i);
      size_t position = safepoint->GetLifetimePosition();
      LocationSummary* locations = safepoint->GetLocations();
      if (!current->Covers(position)) {
        continue;
      }
      if (interval->GetStart() == position) {
        // The safepoint is for this instruction, so the location of the instruction
        // does not need to be saved.
        continue;
      }

      if ((current->GetType() == Primitive::kPrimNot) && current->GetParent()->HasSpillSlot()) {
        locations->SetStackBit(current->GetParent()->GetSpillSlot() / kVRegSize);
      }

      switch (source.GetKind()) {
        case Location::kRegister: {
          locations->AddLiveRegister(source);
          DCHECK_LE(locations->GetNumberOfLiveRegisters(), maximum_number_of_live_registers_);

          if (current->GetType() == Primitive::kPrimNot) {
            locations->SetRegisterBit(source.reg());
          }
          break;
        }
        case Location::kFpuRegister: {
          locations->AddLiveRegister(source);
          break;
        }
        case Location::kStackSlot:  // Fall-through
        case Location::kDoubleStackSlot:  // Fall-through
        case Location::kConstant: {
          // Nothing to do.
          break;
        }
        default: {
          LOG(FATAL) << "Unexpected location for object";
        }
      }
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

  // Intervals end at the lifetime end of a block. The decrement by one
  // ensures the `Cover` call will return true.
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

  DCHECK(destination != nullptr && source != nullptr);

  if (!destination->HasRegister()) {
    // Values are eagerly spilled. Spill slot already contains appropriate value.
    return;
  }

  // If `from` has only one successor, we can put the moves at the exit of it. Otherwise
  // we need to put the moves at the entry of `to`.
  if (from->GetSuccessors().Size() == 1) {
    InsertParallelMoveAtExitOf(from,
                               interval->GetParent()->GetDefinedBy(),
                               source->ToLocation(),
                               destination->ToLocation());
  } else {
    DCHECK_EQ(to->GetPredecessors().Size(), 1u);
    InsertParallelMoveAtEntryOf(to,
                                interval->GetParent()->GetDefinedBy(),
                                source->ToLocation(),
                                destination->ToLocation());
  }
}

void RegisterAllocator::Resolve() {
  codegen_->ComputeFrameSize(
      spill_slots_.Size(), maximum_number_of_live_registers_, reserved_out_slots_);

  // Adjust the Out Location of instructions.
  // TODO: Use pointers of Location inside LiveInterval to avoid doing another iteration.
  for (size_t i = 0, e = liveness_.GetNumberOfSsaValues(); i < e; ++i) {
    HInstruction* instruction = liveness_.GetInstructionFromSsaIndex(i);
    LiveInterval* current = instruction->GetLiveInterval();
    LocationSummary* locations = instruction->GetLocations();
    Location location = locations->Out();
    if (instruction->IsParameterValue()) {
      // Now that we know the frame size, adjust the parameter's location.
      if (location.IsStackSlot()) {
        location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
        current->SetSpillSlot(location.GetStackIndex());
        locations->UpdateOut(location);
      } else if (location.IsDoubleStackSlot()) {
        location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
        current->SetSpillSlot(location.GetStackIndex());
        locations->UpdateOut(location);
      } else if (current->HasSpillSlot()) {
        current->SetSpillSlot(current->GetSpillSlot() + codegen_->GetFrameSize());
      }
    }

    Location source = current->ToLocation();

    if (location.IsUnallocated()) {
      if (location.GetPolicy() == Location::kSameAsFirstInput) {
        if (locations->InAt(0).IsUnallocated()) {
          locations->SetInAt(0, source);
        } else {
          DCHECK(locations->InAt(0).Equals(source));
        }
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
    for (HInstructionIterator inst_it(current->GetPhis()); !inst_it.Done(); inst_it.Advance()) {
      HInstruction* phi = inst_it.Current();
      for (size_t i = 0, e = current->GetPredecessors().Size(); i < e; ++i) {
        HBasicBlock* predecessor = current->GetPredecessors().Get(i);
        DCHECK_EQ(predecessor->GetSuccessors().Size(), 1u);
        HInstruction* input = phi->InputAt(i);
        Location source = input->GetLiveInterval()->GetLocationAt(
            predecessor->GetLifetimeEnd() - 1);
        Location destination = phi->GetLiveInterval()->ToLocation();
        InsertParallelMoveAtExitOf(predecessor, nullptr, source, destination);
      }
    }
  }

  // Assign temp locations.
  HInstruction* current = nullptr;
  size_t temp_index = 0;
  for (size_t i = 0; i < temp_intervals_.Size(); ++i) {
    LiveInterval* temp = temp_intervals_.Get(i);
    HInstruction* at = liveness_.GetTempUser(temp);
    if (at != current) {
      temp_index = 0;
      current = at;
    }
    LocationSummary* locations = at->GetLocations();
    switch (temp->GetType()) {
      case Primitive::kPrimInt:
        locations->SetTempAt(
            temp_index++, Location::RegisterLocation(temp->GetRegister()));
        break;

      case Primitive::kPrimDouble:
        // TODO: Support the case of ARM, where a double value
        // requires an FPU register pair (note that the ARM back end
        // does not yet use this register allocator when a method uses
        // floats or doubles).
        DCHECK(codegen_->GetInstructionSet() != kArm
               && codegen_->GetInstructionSet() != kThumb2);
        locations->SetTempAt(
            temp_index++, Location::FpuRegisterLocation(temp->GetRegister()));
        break;

      default:
        LOG(FATAL) << "Unexpected type for temporary location "
                   << temp->GetType();
    }
  }
}

}  // namespace art
