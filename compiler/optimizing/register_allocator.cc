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

#include <iostream>
#include <sstream>

#include "base/bit_vector-inl.h"
#include "code_generator.h"
#include "ssa_liveness_analysis.h"

namespace art {

static constexpr size_t kMaxLifetimePosition = -1;
static constexpr size_t kDefaultNumberOfSpillSlots = 4;

// For simplicity, we implement register pairs as (reg, reg + 1).
// Note that this is a requirement for double registers on ARM, since we
// allocate SRegister.
static int GetHighForLowRegister(int reg) { return reg + 1; }
static bool IsLowRegister(int reg) { return (reg & 1) == 0; }
static bool IsLowOfUnalignedPairInterval(LiveInterval* low) {
  return GetHighForLowRegister(low->GetRegister()) != low->GetHighInterval()->GetRegister();
}

RegisterAllocator::RegisterAllocator(ArenaAllocator* allocator,
                                     CodeGenerator* codegen,
                                     const SsaLivenessAnalysis& liveness)
      : allocator_(allocator),
        codegen_(codegen),
        liveness_(liveness),
        unhandled_core_intervals_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        unhandled_fp_intervals_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        unhandled_(nullptr),
        handled_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        active_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        inactive_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        physical_core_register_intervals_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        physical_fp_register_intervals_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        temp_intervals_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        int_spill_slots_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        long_spill_slots_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        float_spill_slots_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        double_spill_slots_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        catch_phi_spill_slots_(0),
        safepoints_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        processing_core_registers_(false),
        number_of_registers_(-1),
        registers_array_(nullptr),
        blocked_core_registers_(codegen->GetBlockedCoreRegisters()),
        blocked_fp_registers_(codegen->GetBlockedFloatingPointRegisters()),
        reserved_out_slots_(0),
        maximum_number_of_live_core_registers_(0),
        maximum_number_of_live_fp_registers_(0) {
  temp_intervals_.reserve(4);
  int_spill_slots_.reserve(kDefaultNumberOfSpillSlots);
  long_spill_slots_.reserve(kDefaultNumberOfSpillSlots);
  float_spill_slots_.reserve(kDefaultNumberOfSpillSlots);
  double_spill_slots_.reserve(kDefaultNumberOfSpillSlots);

  codegen->SetupBlockedRegisters();
  physical_core_register_intervals_.resize(codegen->GetNumberOfCoreRegisters(), nullptr);
  physical_fp_register_intervals_.resize(codegen->GetNumberOfFloatingPointRegisters(), nullptr);
  // Always reserve for the current method and the graph's max out registers.
  // TODO: compute it instead.
  // ArtMethod* takes 2 vregs for 64 bits.
  reserved_out_slots_ = InstructionSetPointerSize(codegen->GetInstructionSet()) / kVRegSize +
      codegen->GetGraph()->GetMaximumNumberOfOutVRegs();
}

bool RegisterAllocator::CanAllocateRegistersFor(const HGraph& graph ATTRIBUTE_UNUSED,
                                                InstructionSet instruction_set) {
  return instruction_set == kArm
      || instruction_set == kArm64
      || instruction_set == kMips
      || instruction_set == kMips64
      || instruction_set == kThumb2
      || instruction_set == kX86
      || instruction_set == kX86_64;
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
    for (HLinearOrderIterator it(*codegen_->GetGraph()); !it.Done(); it.Advance()) {
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

void RegisterAllocator::BlockRegister(Location location, size_t start, size_t end) {
  int reg = location.reg();
  DCHECK(location.IsRegister() || location.IsFpuRegister());
  LiveInterval* interval = location.IsRegister()
      ? physical_core_register_intervals_[reg]
      : physical_fp_register_intervals_[reg];
  Primitive::Type type = location.IsRegister()
      ? Primitive::kPrimInt
      : Primitive::kPrimFloat;
  if (interval == nullptr) {
    interval = LiveInterval::MakeFixedInterval(allocator_, reg, type);
    if (location.IsRegister()) {
      physical_core_register_intervals_[reg] = interval;
    } else {
      physical_fp_register_intervals_[reg] = interval;
    }
  }
  DCHECK(interval->GetRegister() == reg);
  interval->AddRange(start, end);
}

void RegisterAllocator::BlockRegisters(size_t start, size_t end, bool caller_save_only) {
  for (size_t i = 0; i < codegen_->GetNumberOfCoreRegisters(); ++i) {
    if (!caller_save_only || !codegen_->IsCoreCalleeSaveRegister(i)) {
      BlockRegister(Location::RegisterLocation(i), start, end);
    }
  }
  for (size_t i = 0; i < codegen_->GetNumberOfFloatingPointRegisters(); ++i) {
    if (!caller_save_only || !codegen_->IsFloatingPointCalleeSaveRegister(i)) {
      BlockRegister(Location::FpuRegisterLocation(i), start, end);
    }
  }
}

void RegisterAllocator::AllocateRegistersInternal() {
  // Iterate post-order, to ensure the list is sorted, and the last added interval
  // is the one with the lowest start position.
  for (HLinearPostOrderIterator it(*codegen_->GetGraph()); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    for (HBackwardInstructionIterator back_it(block->GetInstructions()); !back_it.Done();
         back_it.Advance()) {
      ProcessInstruction(back_it.Current());
    }
    for (HInstructionIterator inst_it(block->GetPhis()); !inst_it.Done(); inst_it.Advance()) {
      ProcessInstruction(inst_it.Current());
    }

    if (block->IsCatchBlock() ||
        (block->IsLoopHeader() && block->GetLoopInformation()->IsIrreducible())) {
      // By blocking all registers at the top of each catch block or irreducible loop, we force
      // intervals belonging to the live-in set of the catch/header block to be spilled.
      // TODO(ngeoffray): Phis in this block could be allocated in register.
      size_t position = block->GetLifetimeStart();
      BlockRegisters(position, position + 1);
    }
  }

  number_of_registers_ = codegen_->GetNumberOfCoreRegisters();
  registers_array_ = allocator_->AllocArray<size_t>(number_of_registers_,
                                                    kArenaAllocRegisterAllocator);
  processing_core_registers_ = true;
  unhandled_ = &unhandled_core_intervals_;
  for (LiveInterval* fixed : physical_core_register_intervals_) {
    if (fixed != nullptr) {
      // Fixed interval is added to inactive_ instead of unhandled_.
      // It's also the only type of inactive interval whose start position
      // can be after the current interval during linear scan.
      // Fixed interval is never split and never moves to unhandled_.
      inactive_.push_back(fixed);
    }
  }
  LinearScan();

  inactive_.clear();
  active_.clear();
  handled_.clear();

  number_of_registers_ = codegen_->GetNumberOfFloatingPointRegisters();
  registers_array_ = allocator_->AllocArray<size_t>(number_of_registers_,
                                                    kArenaAllocRegisterAllocator);
  processing_core_registers_ = false;
  unhandled_ = &unhandled_fp_intervals_;
  for (LiveInterval* fixed : physical_fp_register_intervals_) {
    if (fixed != nullptr) {
      // Fixed interval is added to inactive_ instead of unhandled_.
      // It's also the only type of inactive interval whose start position
      // can be after the current interval during linear scan.
      // Fixed interval is never split and never moves to unhandled_.
      inactive_.push_back(fixed);
    }
  }
  LinearScan();
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
      // Ensure that an explicit temporary register is marked as being allocated.
      codegen_->AddAllocatedRegister(temp);
    } else {
      DCHECK(temp.IsUnallocated());
      switch (temp.GetPolicy()) {
        case Location::kRequiresRegister: {
          LiveInterval* interval =
              LiveInterval::MakeTempInterval(allocator_, Primitive::kPrimInt);
          temp_intervals_.push_back(interval);
          interval->AddTempUse(instruction, i);
          unhandled_core_intervals_.push_back(interval);
          break;
        }

        case Location::kRequiresFpuRegister: {
          LiveInterval* interval =
              LiveInterval::MakeTempInterval(allocator_, Primitive::kPrimDouble);
          temp_intervals_.push_back(interval);
          interval->AddTempUse(instruction, i);
          if (codegen_->NeedsTwoRegisters(Primitive::kPrimDouble)) {
            interval->AddHighInterval(/* is_temp */ true);
            LiveInterval* high = interval->GetHighInterval();
            temp_intervals_.push_back(high);
            unhandled_fp_intervals_.push_back(high);
          }
          unhandled_fp_intervals_.push_back(interval);
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

  if (locations->NeedsSafepoint()) {
    if (codegen_->IsLeafMethod()) {
      // TODO: We do this here because we do not want the suspend check to artificially
      // create live registers. We should find another place, but this is currently the
      // simplest.
      DCHECK(instruction->IsSuspendCheckEntry());
      instruction->GetBlock()->RemoveInstruction(instruction);
      return;
    }
    safepoints_.push_back(instruction);
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
    BlockRegisters(position, position + 1, /* caller_save_only */ true);
  }

  for (size_t i = 0; i < instruction->InputCount(); ++i) {
    Location input = locations->InAt(i);
    if (input.IsRegister() || input.IsFpuRegister()) {
      BlockRegister(input, position, position + 1);
    } else if (input.IsPair()) {
      BlockRegister(input.ToLow(), position, position + 1);
      BlockRegister(input.ToHigh(), position, position + 1);
    }
  }

  LiveInterval* current = instruction->GetLiveInterval();
  if (current == nullptr) return;

  ArenaVector<LiveInterval*>& unhandled = core_register
      ? unhandled_core_intervals_
      : unhandled_fp_intervals_;

  DCHECK(unhandled.empty() || current->StartsBeforeOrAt(unhandled.back()));

  if (codegen_->NeedsTwoRegisters(current->GetType())) {
    current->AddHighInterval();
  }

  for (size_t safepoint_index = safepoints_.size(); safepoint_index > 0; --safepoint_index) {
    HInstruction* safepoint = safepoints_[safepoint_index - 1u];
    size_t safepoint_position = safepoint->GetLifetimePosition();

    // Test that safepoints are ordered in the optimal way.
    DCHECK(safepoint_index == safepoints_.size() ||
           safepoints_[safepoint_index]->GetLifetimePosition() < safepoint_position);

    if (safepoint_position == current->GetStart()) {
      // The safepoint is for this instruction, so the location of the instruction
      // does not need to be saved.
      DCHECK_EQ(safepoint_index, safepoints_.size());
      DCHECK_EQ(safepoint, instruction);
      continue;
    } else if (current->IsDeadAt(safepoint_position)) {
      break;
    } else if (!current->Covers(safepoint_position)) {
      // Hole in the interval.
      continue;
    }
    current->AddSafepoint(safepoint);
  }
  current->ResetSearchCache();

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
    } else if (first.IsPair()) {
      current->SetFrom(position + 1);
      current->SetRegister(first.low());
      LiveInterval* high = current->GetHighInterval();
      high->SetRegister(first.high());
      high->SetFrom(position + 1);
    }
  } else if (output.IsRegister() || output.IsFpuRegister()) {
    // Shift the interval's start by one to account for the blocked register.
    current->SetFrom(position + 1);
    current->SetRegister(output.reg());
    BlockRegister(output, position, position + 1);
  } else if (output.IsPair()) {
    current->SetFrom(position + 1);
    current->SetRegister(output.low());
    LiveInterval* high = current->GetHighInterval();
    high->SetRegister(output.high());
    high->SetFrom(position + 1);
    BlockRegister(output.ToLow(), position, position + 1);
    BlockRegister(output.ToHigh(), position, position + 1);
  } else if (output.IsStackSlot() || output.IsDoubleStackSlot()) {
    current->SetSpillSlot(output.GetStackIndex());
  } else {
    DCHECK(output.IsUnallocated() || output.IsConstant());
  }

  if (instruction->IsPhi() && instruction->AsPhi()->IsCatchPhi()) {
    AllocateSpillSlotForCatchPhi(instruction->AsPhi());
  }

  // If needed, add interval to the list of unhandled intervals.
  if (current->HasSpillSlot() || instruction->IsConstant()) {
    // Split just before first register use.
    size_t first_register_use = current->FirstRegisterUse();
    if (first_register_use != kNoLifetime) {
      LiveInterval* split = SplitBetween(current, current->GetStart(), first_register_use - 1);
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
  ArenaVector<LiveInterval*> intervals(allocator_->Adapter(kArenaAllocRegisterAllocatorValidate));
  for (size_t i = 0; i < liveness_.GetNumberOfSsaValues(); ++i) {
    HInstruction* instruction = liveness_.GetInstructionFromSsaIndex(i);
    if (ShouldProcess(processing_core_registers_, instruction->GetLiveInterval())) {
      intervals.push_back(instruction->GetLiveInterval());
    }
  }

  const ArenaVector<LiveInterval*>* physical_register_intervals = processing_core_registers_
      ? &physical_core_register_intervals_
      : &physical_fp_register_intervals_;
  for (LiveInterval* fixed : *physical_register_intervals) {
    if (fixed != nullptr) {
      intervals.push_back(fixed);
    }
  }

  for (LiveInterval* temp : temp_intervals_) {
    if (ShouldProcess(processing_core_registers_, temp)) {
      intervals.push_back(temp);
    }
  }

  return ValidateIntervals(intervals, GetNumberOfSpillSlots(), reserved_out_slots_, *codegen_,
                           allocator_, processing_core_registers_, log_fatal_on_failure);
}

bool RegisterAllocator::ValidateIntervals(const ArenaVector<LiveInterval*>& intervals,
                                          size_t number_of_spill_slots,
                                          size_t number_of_out_slots,
                                          const CodeGenerator& codegen,
                                          ArenaAllocator* allocator,
                                          bool processing_core_registers,
                                          bool log_fatal_on_failure) {
  size_t number_of_registers = processing_core_registers
      ? codegen.GetNumberOfCoreRegisters()
      : codegen.GetNumberOfFloatingPointRegisters();
  ArenaVector<ArenaBitVector*> liveness_of_values(
      allocator->Adapter(kArenaAllocRegisterAllocatorValidate));
  liveness_of_values.reserve(number_of_registers + number_of_spill_slots);

  size_t max_end = 0u;
  for (LiveInterval* start_interval : intervals) {
    for (AllRangesIterator it(start_interval); !it.Done(); it.Advance()) {
      max_end = std::max(max_end, it.CurrentRange()->GetEnd());
    }
  }

  // Allocate a bit vector per register. A live interval that has a register
  // allocated will populate the associated bit vector based on its live ranges.
  for (size_t i = 0; i < number_of_registers + number_of_spill_slots; ++i) {
    liveness_of_values.push_back(
        ArenaBitVector::Create(allocator, max_end, false, kArenaAllocRegisterAllocatorValidate));
  }

  for (LiveInterval* start_interval : intervals) {
    for (AllRangesIterator it(start_interval); !it.Done(); it.Advance()) {
      LiveInterval* current = it.CurrentInterval();
      HInstruction* defined_by = current->GetParent()->GetDefinedBy();
      if (current->GetParent()->HasSpillSlot()
           // Parameters and current method have their own stack slot.
           && !(defined_by != nullptr && (defined_by->IsParameterValue()
                                          || defined_by->IsCurrentMethod()))) {
        BitVector* liveness_of_spill_slot = liveness_of_values[number_of_registers
            + current->GetParent()->GetSpillSlot() / kVRegSize
            - number_of_out_slots];
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
        if (kIsDebugBuild && log_fatal_on_failure && !current->IsFixed()) {
          // Only check when an error is fatal. Only tests code ask for non-fatal failures
          // and test code may not properly fill the right information to the code generator.
          CHECK(codegen.HasAllocatedRegister(processing_core_registers, current->GetRegister()));
        }
        BitVector* liveness_of_register = liveness_of_values[current->GetRegister()];
        for (size_t j = it.CurrentRange()->GetStart(); j < it.CurrentRange()->GetEnd(); ++j) {
          if (liveness_of_register->IsBitSet(j)) {
            if (current->IsUsingInputRegister() && current->CanUseInputRegister()) {
              continue;
            }
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
  for (LiveInterval* inactive_interval : inactive_) {
    DumpInterval(stream, inactive_interval);
  }
  stream << "active: " << std::endl;
  for (LiveInterval* active_interval : active_) {
    DumpInterval(stream, active_interval);
  }
  stream << "unhandled: " << std::endl;
  auto unhandled = (unhandled_ != nullptr) ?
      unhandled_ : &unhandled_core_intervals_;
  for (LiveInterval* unhandled_interval : *unhandled) {
    DumpInterval(stream, unhandled_interval);
  }
  stream << "handled: " << std::endl;
  for (LiveInterval* handled_interval : handled_) {
    DumpInterval(stream, handled_interval);
  }
}

// By the book implementation of a linear scan register allocator.
void RegisterAllocator::LinearScan() {
  while (!unhandled_->empty()) {
    // (1) Remove interval with the lowest start position from unhandled.
    LiveInterval* current = unhandled_->back();
    unhandled_->pop_back();

    // Make sure the interval is an expected state.
    DCHECK(!current->IsFixed() && !current->HasSpillSlot());
    // Make sure we are going in the right order.
    DCHECK(unhandled_->empty() || unhandled_->back()->GetStart() >= current->GetStart());
    // Make sure a low interval is always with a high.
    DCHECK(!current->IsLowInterval() || unhandled_->back()->IsHighInterval());
    // Make sure a high interval is always with a low.
    DCHECK(current->IsLowInterval() ||
           unhandled_->empty() ||
           !unhandled_->back()->IsHighInterval());

    size_t position = current->GetStart();

    // Remember the inactive_ size here since the ones moved to inactive_ from
    // active_ below shouldn't need to be re-checked.
    size_t inactive_intervals_to_handle = inactive_.size();

    // (2) Remove currently active intervals that are dead at this position.
    //     Move active intervals that have a lifetime hole at this position
    //     to inactive.
    auto active_kept_end = std::remove_if(
        active_.begin(),
        active_.end(),
        [this, position](LiveInterval* interval) {
          if (interval->IsDeadAt(position)) {
            handled_.push_back(interval);
            return true;
          } else if (!interval->Covers(position)) {
            inactive_.push_back(interval);
            return true;
          } else {
            return false;  // Keep this interval.
          }
        });
    active_.erase(active_kept_end, active_.end());

    // (3) Remove currently inactive intervals that are dead at this position.
    //     Move inactive intervals that cover this position to active.
    auto inactive_to_handle_end = inactive_.begin() + inactive_intervals_to_handle;
    auto inactive_kept_end = std::remove_if(
        inactive_.begin(),
        inactive_to_handle_end,
        [this, position](LiveInterval* interval) {
          DCHECK(interval->GetStart() < position || interval->IsFixed());
          if (interval->IsDeadAt(position)) {
            handled_.push_back(interval);
            return true;
          } else if (interval->Covers(position)) {
            active_.push_back(interval);
            return true;
          } else {
            return false;  // Keep this interval.
          }
        });
    inactive_.erase(inactive_kept_end, inactive_to_handle_end);

    if (current->IsSlowPathSafepoint()) {
      // Synthesized interval to record the maximum number of live registers
      // at safepoints. No need to allocate a register for it.
      if (processing_core_registers_) {
        maximum_number_of_live_core_registers_ =
          std::max(maximum_number_of_live_core_registers_, active_.size());
      } else {
        maximum_number_of_live_fp_registers_ =
          std::max(maximum_number_of_live_fp_registers_, active_.size());
      }
      DCHECK(unhandled_->empty() || unhandled_->back()->GetStart() > current->GetStart());
      continue;
    }

    if (current->IsHighInterval() && !current->GetLowInterval()->HasRegister()) {
      DCHECK(!current->HasRegister());
      // Allocating the low part was unsucessful. The splitted interval for the high part
      // will be handled next (it is in the `unhandled_` list).
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
      codegen_->AddAllocatedRegister(processing_core_registers_
          ? Location::RegisterLocation(current->GetRegister())
          : Location::FpuRegisterLocation(current->GetRegister()));
      active_.push_back(current);
      if (current->HasHighInterval() && !current->GetHighInterval()->HasRegister()) {
        current->GetHighInterval()->SetRegister(GetHighForLowRegister(current->GetRegister()));
      }
    }
  }
}

static void FreeIfNotCoverAt(LiveInterval* interval, size_t position, size_t* free_until) {
  DCHECK(!interval->IsHighInterval());
  // Note that the same instruction may occur multiple times in the input list,
  // so `free_until` may have changed already.
  // Since `position` is not the current scan position, we need to use CoversSlow.
  if (interval->IsDeadAt(position)) {
    // Set the register to be free. Note that inactive intervals might later
    // update this.
    free_until[interval->GetRegister()] = kMaxLifetimePosition;
    if (interval->HasHighInterval()) {
      DCHECK(interval->GetHighInterval()->IsDeadAt(position));
      free_until[interval->GetHighInterval()->GetRegister()] = kMaxLifetimePosition;
    }
  } else if (!interval->CoversSlow(position)) {
    // The interval becomes inactive at `defined_by`. We make its register
    // available only until the next use strictly after `defined_by`.
    free_until[interval->GetRegister()] = interval->FirstUseAfter(position);
    if (interval->HasHighInterval()) {
      DCHECK(!interval->GetHighInterval()->CoversSlow(position));
      free_until[interval->GetHighInterval()->GetRegister()] = free_until[interval->GetRegister()];
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
  for (LiveInterval* interval : active_) {
    DCHECK(interval->HasRegister());
    free_until[interval->GetRegister()] = 0;
  }

  // An interval that starts an instruction (that is, it is not split), may
  // re-use the registers used by the inputs of that instruciton, based on the
  // location summary.
  HInstruction* defined_by = current->GetDefinedBy();
  if (defined_by != nullptr && !current->IsSplit()) {
    LocationSummary* locations = defined_by->GetLocations();
    if (!locations->OutputCanOverlapWithInputs() && locations->Out().IsUnallocated()) {
      for (size_t i = 0, e = defined_by->InputCount(); i < e; ++i) {
        // Take the last interval of the input. It is the location of that interval
        // that will be used at `defined_by`.
        LiveInterval* interval = defined_by->InputAt(i)->GetLiveInterval()->GetLastSibling();
        // Note that interval may have not been processed yet.
        // TODO: Handle non-split intervals last in the work list.
        if (locations->InAt(i).IsValid()
            && interval->HasRegister()
            && interval->SameRegisterKind(*current)) {
          // The input must be live until the end of `defined_by`, to comply to
          // the linear scan algorithm. So we use `defined_by`'s end lifetime
          // position to check whether the input is dead or is inactive after
          // `defined_by`.
          DCHECK(interval->CoversSlow(defined_by->GetLifetimePosition()));
          size_t position = defined_by->GetLifetimePosition() + 1;
          FreeIfNotCoverAt(interval, position, free_until);
        }
      }
    }
  }

  // For each inactive interval, set its register to be free until
  // the next intersection with `current`.
  for (LiveInterval* inactive : inactive_) {
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

  int reg = kNoRegister;
  if (current->HasRegister()) {
    // Some instructions have a fixed register output.
    reg = current->GetRegister();
    if (free_until[reg] == 0) {
      DCHECK(current->IsHighInterval());
      // AllocateBlockedReg will spill the holder of the register.
      return false;
    }
  } else {
    DCHECK(!current->IsHighInterval());
    int hint = current->FindFirstRegisterHint(free_until, liveness_);
    if ((hint != kNoRegister)
        // For simplicity, if the hint we are getting for a pair cannot be used,
        // we are just going to allocate a new pair.
        && !(current->IsLowInterval() && IsBlocked(GetHighForLowRegister(hint)))) {
      DCHECK(!IsBlocked(hint));
      reg = hint;
    } else if (current->IsLowInterval()) {
      reg = FindAvailableRegisterPair(free_until, current->GetStart());
    } else {
      reg = FindAvailableRegister(free_until, current);
    }
  }

  DCHECK_NE(reg, kNoRegister);
  // If we could not find a register, we need to spill.
  if (free_until[reg] == 0) {
    return false;
  }

  if (current->IsLowInterval()) {
    // If the high register of this interval is not available, we need to spill.
    int high_reg = current->GetHighInterval()->GetRegister();
    if (high_reg == kNoRegister) {
      high_reg = GetHighForLowRegister(reg);
    }
    if (free_until[high_reg] == 0) {
      return false;
    }
  }

  current->SetRegister(reg);
  if (!current->IsDeadAt(free_until[reg])) {
    // If the register is only available for a subset of live ranges
    // covered by `current`, split `current` before the position where
    // the register is not available anymore.
    LiveInterval* split = SplitBetween(current, current->GetStart(), free_until[reg]);
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

int RegisterAllocator::FindAvailableRegisterPair(size_t* next_use, size_t starting_at) const {
  int reg = kNoRegister;
  // Pick the register pair that is used the last.
  for (size_t i = 0; i < number_of_registers_; ++i) {
    if (IsBlocked(i)) continue;
    if (!IsLowRegister(i)) continue;
    int high_register = GetHighForLowRegister(i);
    if (IsBlocked(high_register)) continue;
    int existing_high_register = GetHighForLowRegister(reg);
    if ((reg == kNoRegister) || (next_use[i] >= next_use[reg]
                        && next_use[high_register] >= next_use[existing_high_register])) {
      reg = i;
      if (next_use[i] == kMaxLifetimePosition
          && next_use[high_register] == kMaxLifetimePosition) {
        break;
      }
    } else if (next_use[reg] <= starting_at || next_use[existing_high_register] <= starting_at) {
      // If one of the current register is known to be unavailable, just unconditionally
      // try a new one.
      reg = i;
    }
  }
  return reg;
}

bool RegisterAllocator::IsCallerSaveRegister(int reg) const {
  return processing_core_registers_
      ? !codegen_->IsCoreCalleeSaveRegister(reg)
      : !codegen_->IsFloatingPointCalleeSaveRegister(reg);
}

int RegisterAllocator::FindAvailableRegister(size_t* next_use, LiveInterval* current) const {
  // We special case intervals that do not span a safepoint to try to find a caller-save
  // register if one is available. We iterate from 0 to the number of registers,
  // so if there are caller-save registers available at the end, we continue the iteration.
  bool prefers_caller_save = !current->HasWillCallSafepoint();
  int reg = kNoRegister;
  for (size_t i = 0; i < number_of_registers_; ++i) {
    if (IsBlocked(i)) {
      // Register cannot be used. Continue.
      continue;
    }

    // Best case: we found a register fully available.
    if (next_use[i] == kMaxLifetimePosition) {
      if (prefers_caller_save && !IsCallerSaveRegister(i)) {
        // We can get shorter encodings on some platforms by using
        // small register numbers. So only update the candidate if the previous
        // one was not available for the whole method.
        if (reg == kNoRegister || next_use[reg] != kMaxLifetimePosition) {
          reg = i;
        }
        // Continue the iteration in the hope of finding a caller save register.
        continue;
      } else {
        reg = i;
        // We know the register is good enough. Return it.
        break;
      }
    }

    // If we had no register before, take this one as a reference.
    if (reg == kNoRegister) {
      reg = i;
      continue;
    }

    // Pick the register that is used the last.
    if (next_use[i] > next_use[reg]) {
      reg = i;
      continue;
    }
  }
  return reg;
}

// Remove interval and its other half if any. Return iterator to the following element.
static ArenaVector<LiveInterval*>::iterator RemoveIntervalAndPotentialOtherHalf(
    ArenaVector<LiveInterval*>* intervals, ArenaVector<LiveInterval*>::iterator pos) {
  DCHECK(intervals->begin() <= pos && pos < intervals->end());
  LiveInterval* interval = *pos;
  if (interval->IsLowInterval()) {
    DCHECK(pos + 1 < intervals->end());
    DCHECK_EQ(*(pos + 1), interval->GetHighInterval());
    return intervals->erase(pos, pos + 2);
  } else if (interval->IsHighInterval()) {
    DCHECK(intervals->begin() < pos);
    DCHECK_EQ(*(pos - 1), interval->GetLowInterval());
    return intervals->erase(pos - 1, pos + 1);
  } else {
    return intervals->erase(pos);
  }
}

bool RegisterAllocator::TrySplitNonPairOrUnalignedPairIntervalAt(size_t position,
                                                                 size_t first_register_use,
                                                                 size_t* next_use) {
  for (auto it = active_.begin(), end = active_.end(); it != end; ++it) {
    LiveInterval* active = *it;
    DCHECK(active->HasRegister());
    if (active->IsFixed()) continue;
    if (active->IsHighInterval()) continue;
    if (first_register_use > next_use[active->GetRegister()]) continue;

    // Split the first interval found that is either:
    // 1) A non-pair interval.
    // 2) A pair interval whose high is not low + 1.
    // 3) A pair interval whose low is not even.
    if (!active->IsLowInterval() ||
        IsLowOfUnalignedPairInterval(active) ||
        !IsLowRegister(active->GetRegister())) {
      LiveInterval* split = Split(active, position);
      if (split != active) {
        handled_.push_back(active);
      }
      RemoveIntervalAndPotentialOtherHalf(&active_, it);
      AddSorted(unhandled_, split);
      return true;
    }
  }
  return false;
}

// Find the register that is used the last, and spill the interval
// that holds it. If the first use of `current` is after that register
// we spill `current` instead.
bool RegisterAllocator::AllocateBlockedReg(LiveInterval* current) {
  size_t first_register_use = current->FirstRegisterUse();
  if (current->HasRegister()) {
    DCHECK(current->IsHighInterval());
    // The low interval has allocated the register for the high interval. In
    // case the low interval had to split both intervals, we may end up in a
    // situation where the high interval does not have a register use anymore.
    // We must still proceed in order to split currently active and inactive
    // uses of the high interval's register, and put the high interval in the
    // active set.
    DCHECK(first_register_use != kNoLifetime || (current->GetNextSibling() != nullptr));
  } else if (first_register_use == kNoLifetime) {
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
  for (LiveInterval* active : active_) {
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
  for (LiveInterval* inactive : inactive_) {
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
        size_t use = inactive->FirstUseAfter(current->GetStart());
        if (use != kNoLifetime) {
          next_use[inactive->GetRegister()] = std::min(use, next_use[inactive->GetRegister()]);
        }
      }
    }
  }

  int reg = kNoRegister;
  bool should_spill = false;
  if (current->HasRegister()) {
    DCHECK(current->IsHighInterval());
    reg = current->GetRegister();
    // When allocating the low part, we made sure the high register was available.
    DCHECK_LT(first_register_use, next_use[reg]);
  } else if (current->IsLowInterval()) {
    reg = FindAvailableRegisterPair(next_use, first_register_use);
    // We should spill if both registers are not available.
    should_spill = (first_register_use >= next_use[reg])
      || (first_register_use >= next_use[GetHighForLowRegister(reg)]);
  } else {
    DCHECK(!current->IsHighInterval());
    reg = FindAvailableRegister(next_use, current);
    should_spill = (first_register_use >= next_use[reg]);
  }

  DCHECK_NE(reg, kNoRegister);
  if (should_spill) {
    DCHECK(!current->IsHighInterval());
    bool is_allocation_at_use_site = (current->GetStart() >= (first_register_use - 1));
    if (is_allocation_at_use_site) {
      if (!current->IsLowInterval()) {
        DumpInterval(std::cerr, current);
        DumpAllIntervals(std::cerr);
        // This situation has the potential to infinite loop, so we make it a non-debug CHECK.
        HInstruction* at = liveness_.GetInstructionFromPosition(first_register_use / 2);
        CHECK(false) << "There is not enough registers available for "
          << current->GetParent()->GetDefinedBy()->DebugName() << " "
          << current->GetParent()->GetDefinedBy()->GetId()
          << " at " << first_register_use - 1 << " "
          << (at == nullptr ? "" : at->DebugName());
      }

      // If we're allocating a register for `current` because the instruction at
      // that position requires it, but we think we should spill, then there are
      // non-pair intervals or unaligned pair intervals blocking the allocation.
      // We split the first interval found, and put ourselves first in the
      // `unhandled_` list.
      bool success = TrySplitNonPairOrUnalignedPairIntervalAt(current->GetStart(),
                                                              first_register_use,
                                                              next_use);
      DCHECK(success);
      LiveInterval* existing = unhandled_->back();
      DCHECK(existing->IsHighInterval());
      DCHECK_EQ(existing->GetLowInterval(), current);
      unhandled_->push_back(current);
    } else {
      // If the first use of that instruction is after the last use of the found
      // register, we split this interval just before its first register use.
      AllocateSpillSlotFor(current);
      LiveInterval* split = SplitBetween(current, current->GetStart(), first_register_use - 1);
      DCHECK(current != split);
      AddSorted(unhandled_, split);
    }
    return false;
  } else {
    // Use this register and spill the active and inactives interval that
    // have that register.
    current->SetRegister(reg);

    for (auto it = active_.begin(), end = active_.end(); it != end; ++it) {
      LiveInterval* active = *it;
      if (active->GetRegister() == reg) {
        DCHECK(!active->IsFixed());
        LiveInterval* split = Split(active, current->GetStart());
        if (split != active) {
          handled_.push_back(active);
        }
        RemoveIntervalAndPotentialOtherHalf(&active_, it);
        AddSorted(unhandled_, split);
        break;
      }
    }

    // NOTE: Retrieve end() on each iteration because we're removing elements in the loop body.
    for (auto it = inactive_.begin(); it != inactive_.end(); ) {
      LiveInterval* inactive = *it;
      bool erased = false;
      if (inactive->GetRegister() == reg) {
        if (!current->IsSplit() && !inactive->IsFixed()) {
          // Neither current nor inactive are fixed.
          // Thanks to SSA, a non-split interval starting in a hole of an
          // inactive interval should never intersect with that inactive interval.
          // Only if it's not fixed though, because fixed intervals don't come from SSA.
          DCHECK_EQ(inactive->FirstIntersectionWith(current), kNoLifetime);
        } else {
          size_t next_intersection = inactive->FirstIntersectionWith(current);
          if (next_intersection != kNoLifetime) {
            if (inactive->IsFixed()) {
              LiveInterval* split = Split(current, next_intersection);
              DCHECK_NE(split, current);
              AddSorted(unhandled_, split);
            } else {
              // Split at the start of `current`, which will lead to splitting
              // at the end of the lifetime hole of `inactive`.
              LiveInterval* split = Split(inactive, current->GetStart());
              // If it's inactive, it must start before the current interval.
              DCHECK_NE(split, inactive);
              it = RemoveIntervalAndPotentialOtherHalf(&inactive_, it);
              erased = true;
              handled_.push_back(inactive);
              AddSorted(unhandled_, split);
            }
          }
        }
      }
      // If we have erased the element, `it` already points to the next element.
      // Otherwise we need to move to the next element.
      if (!erased) {
        ++it;
      }
    }

    return true;
  }
}

void RegisterAllocator::AddSorted(ArenaVector<LiveInterval*>* array, LiveInterval* interval) {
  DCHECK(!interval->IsFixed() && !interval->HasSpillSlot());
  size_t insert_at = 0;
  for (size_t i = array->size(); i > 0; --i) {
    LiveInterval* current = (*array)[i - 1u];
    // High intervals must be processed right after their low equivalent.
    if (current->StartsAfter(interval) && !current->IsHighInterval()) {
      insert_at = i;
      break;
    } else if ((current->GetStart() == interval->GetStart()) && current->IsSlowPathSafepoint()) {
      // Ensure the slow path interval is the last to be processed at its location: we want the
      // interval to know all live registers at this location.
      DCHECK(i == 1 || (*array)[i - 2u]->StartsAfter(current));
      insert_at = i;
      break;
    }
  }

  // Insert the high interval before the low, to ensure the low is processed before.
  auto insert_pos = array->begin() + insert_at;
  if (interval->HasHighInterval()) {
    array->insert(insert_pos, { interval->GetHighInterval(), interval });
  } else if (interval->HasLowInterval()) {
    array->insert(insert_pos, { interval, interval->GetLowInterval() });
  } else {
    array->insert(insert_pos, interval);
  }
}

LiveInterval* RegisterAllocator::SplitBetween(LiveInterval* interval, size_t from, size_t to) {
  HBasicBlock* block_from = liveness_.GetBlockFromPosition(from / 2);
  HBasicBlock* block_to = liveness_.GetBlockFromPosition(to / 2);
  DCHECK(block_from != nullptr);
  DCHECK(block_to != nullptr);

  // Both locations are in the same block. We split at the given location.
  if (block_from == block_to) {
    return Split(interval, to);
  }

  /*
   * Non-linear control flow will force moves at every branch instruction to the new location.
   * To avoid having all branches doing the moves, we find the next non-linear position and
   * split the interval at this position. Take the following example (block number is the linear
   * order position):
   *
   *     B1
   *    /  \
   *   B2  B3
   *    \  /
   *     B4
   *
   * B2 needs to split an interval, whose next use is in B4. If we were to split at the
   * beginning of B4, B3 would need to do a move between B3 and B4 to ensure the interval
   * is now in the correct location. It makes performance worst if the interval is spilled
   * and both B2 and B3 need to reload it before entering B4.
   *
   * By splitting at B3, we give a chance to the register allocator to allocate the
   * interval to the same register as in B1, and therefore avoid doing any
   * moves in B3.
   */
  if (block_from->GetDominator() != nullptr) {
    for (HBasicBlock* dominated : block_from->GetDominator()->GetDominatedBlocks()) {
      size_t position = dominated->GetLifetimeStart();
      if ((position > from) && (block_to->GetLifetimeStart() > position)) {
        // Even if we found a better block, we continue iterating in case
        // a dominated block is closer.
        // Note that dominated blocks are not sorted in liveness order.
        block_to = dominated;
        DCHECK_NE(block_to, block_from);
      }
    }
  }

  // If `to` is in a loop, find the outermost loop header which does not contain `from`.
  for (HLoopInformationOutwardIterator it(*block_to); !it.Done(); it.Advance()) {
    HBasicBlock* header = it.Current()->GetHeader();
    if (block_from->GetLifetimeStart() >= header->GetLifetimeStart()) {
      break;
    }
    block_to = header;
  }

  // Split at the start of the found block, to piggy back on existing moves
  // due to resolution if non-linear control flow (see `ConnectSplitSiblings`).
  return Split(interval, block_to->GetLifetimeStart());
}

LiveInterval* RegisterAllocator::Split(LiveInterval* interval, size_t position) {
  DCHECK_GE(position, interval->GetStart());
  DCHECK(!interval->IsDeadAt(position));
  if (position == interval->GetStart()) {
    // Spill slot will be allocated when handling `interval` again.
    interval->ClearRegister();
    if (interval->HasHighInterval()) {
      interval->GetHighInterval()->ClearRegister();
    } else if (interval->HasLowInterval()) {
      interval->GetLowInterval()->ClearRegister();
    }
    return interval;
  } else {
    LiveInterval* new_interval = interval->SplitAt(position);
    if (interval->HasHighInterval()) {
      LiveInterval* high = interval->GetHighInterval()->SplitAt(position);
      new_interval->SetHighInterval(high);
      high->SetLowInterval(new_interval);
    } else if (interval->HasLowInterval()) {
      LiveInterval* low = interval->GetLowInterval()->SplitAt(position);
      new_interval->SetLowInterval(low);
      low->SetHighInterval(new_interval);
    }
    return new_interval;
  }
}

void RegisterAllocator::AllocateSpillSlotFor(LiveInterval* interval) {
  if (interval->IsHighInterval()) {
    // The low interval already took care of allocating the spill slot.
    DCHECK(!interval->GetLowInterval()->HasRegister());
    DCHECK(interval->GetLowInterval()->GetParent()->HasSpillSlot());
    return;
  }

  LiveInterval* parent = interval->GetParent();

  // An instruction gets a spill slot for its entire lifetime. If the parent
  // of this interval already has a spill slot, there is nothing to do.
  if (parent->HasSpillSlot()) {
    return;
  }

  HInstruction* defined_by = parent->GetDefinedBy();
  DCHECK(!defined_by->IsPhi() || !defined_by->AsPhi()->IsCatchPhi());

  if (defined_by->IsParameterValue()) {
    // Parameters have their own stack slot.
    parent->SetSpillSlot(codegen_->GetStackSlotOfParameter(defined_by->AsParameterValue()));
    return;
  }

  if (defined_by->IsCurrentMethod()) {
    parent->SetSpillSlot(0);
    return;
  }

  if (defined_by->IsConstant()) {
    // Constants don't need a spill slot.
    return;
  }

  ArenaVector<size_t>* spill_slots = nullptr;
  switch (interval->GetType()) {
    case Primitive::kPrimDouble:
      spill_slots = &double_spill_slots_;
      break;
    case Primitive::kPrimLong:
      spill_slots = &long_spill_slots_;
      break;
    case Primitive::kPrimFloat:
      spill_slots = &float_spill_slots_;
      break;
    case Primitive::kPrimNot:
    case Primitive::kPrimInt:
    case Primitive::kPrimChar:
    case Primitive::kPrimByte:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimShort:
      spill_slots = &int_spill_slots_;
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected type for interval " << interval->GetType();
  }

  // Find an available spill slot.
  size_t slot = 0;
  for (size_t e = spill_slots->size(); slot < e; ++slot) {
    if ((*spill_slots)[slot] <= parent->GetStart()
        && (slot == (e - 1) || (*spill_slots)[slot + 1] <= parent->GetStart())) {
      break;
    }
  }

  size_t end = interval->GetLastSibling()->GetEnd();
  if (parent->NeedsTwoSpillSlots()) {
    if (slot + 2u > spill_slots->size()) {
      // We need a new spill slot.
      spill_slots->resize(slot + 2u, end);
    }
    (*spill_slots)[slot] = end;
    (*spill_slots)[slot + 1] = end;
  } else {
    if (slot == spill_slots->size()) {
      // We need a new spill slot.
      spill_slots->push_back(end);
    } else {
      (*spill_slots)[slot] = end;
    }
  }

  // Note that the exact spill slot location will be computed when we resolve,
  // that is when we know the number of spill slots for each type.
  parent->SetSpillSlot(slot);
}

static bool IsValidDestination(Location destination) {
  return destination.IsRegister()
      || destination.IsRegisterPair()
      || destination.IsFpuRegister()
      || destination.IsFpuRegisterPair()
      || destination.IsStackSlot()
      || destination.IsDoubleStackSlot();
}

void RegisterAllocator::AllocateSpillSlotForCatchPhi(HPhi* phi) {
  LiveInterval* interval = phi->GetLiveInterval();

  HInstruction* previous_phi = phi->GetPrevious();
  DCHECK(previous_phi == nullptr ||
         previous_phi->AsPhi()->GetRegNumber() <= phi->GetRegNumber())
      << "Phis expected to be sorted by vreg number, so that equivalent phis are adjacent.";

  if (phi->IsVRegEquivalentOf(previous_phi)) {
    // This is an equivalent of the previous phi. We need to assign the same
    // catch phi slot.
    DCHECK(previous_phi->GetLiveInterval()->HasSpillSlot());
    interval->SetSpillSlot(previous_phi->GetLiveInterval()->GetSpillSlot());
  } else {
    // Allocate a new spill slot for this catch phi.
    // TODO: Reuse spill slots when intervals of phis from different catch
    //       blocks do not overlap.
    interval->SetSpillSlot(catch_phi_spill_slots_);
    catch_phi_spill_slots_ += interval->NeedsTwoSpillSlots() ? 2 : 1;
  }
}

void RegisterAllocator::AddMove(HParallelMove* move,
                                Location source,
                                Location destination,
                                HInstruction* instruction,
                                Primitive::Type type) const {
  if (type == Primitive::kPrimLong
      && codegen_->ShouldSplitLongMoves()
      // The parallel move resolver knows how to deal with long constants.
      && !source.IsConstant()) {
    move->AddMove(source.ToLow(), destination.ToLow(), Primitive::kPrimInt, instruction);
    move->AddMove(source.ToHigh(), destination.ToHigh(), Primitive::kPrimInt, nullptr);
  } else {
    move->AddMove(source, destination, type, instruction);
  }
}

void RegisterAllocator::AddInputMoveFor(HInstruction* input,
                                        HInstruction* user,
                                        Location source,
                                        Location destination) const {
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
  AddMove(move, source, destination, nullptr, input->GetType());
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
  DCHECK(IsValidDestination(destination)) << destination;
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
      // not contain the `HParallelMove` instructions.
      at = at->GetBlock()->GetFirstInstruction();

      if (at->GetLifetimePosition() < position) {
        // We may insert moves for split siblings and phi spills at the beginning of the block.
        // Since this is a different lifetime position, we need to go to the next instruction.
        DCHECK(at->IsParallelMove());
        at = at->GetNext();
      }

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
  AddMove(move, source, destination, instruction, instruction->GetType());
}

void RegisterAllocator::InsertParallelMoveAtExitOf(HBasicBlock* block,
                                                   HInstruction* instruction,
                                                   Location source,
                                                   Location destination) const {
  DCHECK(IsValidDestination(destination)) << destination;
  if (source.Equals(destination)) return;

  DCHECK_EQ(block->GetNormalSuccessors().size(), 1u);
  HInstruction* last = block->GetLastInstruction();
  // We insert moves at exit for phi predecessors and connecting blocks.
  // A block ending with an if or a packed switch cannot branch to a block
  // with phis because we do not allow critical edges. It can also not connect
  // a split interval between two blocks: the move has to happen in the successor.
  DCHECK(!last->IsIf() && !last->IsPackedSwitch());
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
  AddMove(move, source, destination, instruction, instruction->GetType());
}

void RegisterAllocator::InsertParallelMoveAtEntryOf(HBasicBlock* block,
                                                    HInstruction* instruction,
                                                    Location source,
                                                    Location destination) const {
  DCHECK(IsValidDestination(destination)) << destination;
  if (source.Equals(destination)) return;

  HInstruction* first = block->GetFirstInstruction();
  HParallelMove* move = first->AsParallelMove();
  size_t position = block->GetLifetimeStart();
  // This is a parallel move for connecting blocks. We need to differentiate
  // it with moves for connecting siblings in a same block, and input moves.
  if (move == nullptr || move->GetLifetimePosition() != position) {
    move = new (allocator_) HParallelMove(allocator_);
    move->SetLifetimePosition(position);
    block->InsertInstructionBefore(move, first);
  }
  AddMove(move, source, destination, instruction, instruction->GetType());
}

void RegisterAllocator::InsertMoveAfter(HInstruction* instruction,
                                        Location source,
                                        Location destination) const {
  DCHECK(IsValidDestination(destination)) << destination;
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
  AddMove(move, source, destination, instruction, instruction->GetType());
}

void RegisterAllocator::ConnectSiblings(LiveInterval* interval) {
  LiveInterval* current = interval;
  if (current->HasSpillSlot()
      && current->HasRegister()
      // Currently, we spill unconditionnally the current method in the code generators.
      && !interval->GetDefinedBy()->IsCurrentMethod()) {
    // We spill eagerly, so move must be at definition.
    InsertMoveAfter(interval->GetDefinedBy(),
                    interval->ToLocation(),
                    interval->NeedsTwoSpillSlots()
                        ? Location::DoubleStackSlot(interval->GetParent()->GetSpillSlot())
                        : Location::StackSlot(interval->GetParent()->GetSpillSlot()));
  }
  UsePosition* use = current->GetFirstUse();
  UsePosition* env_use = current->GetFirstEnvironmentUse();

  // Walk over all siblings, updating locations of use positions, and
  // connecting them when they are adjacent.
  do {
    Location source = current->ToLocation();

    // Walk over all uses covered by this interval, and update the location
    // information.

    LiveRange* range = current->GetFirstRange();
    while (range != nullptr) {
      while (use != nullptr && use->GetPosition() < range->GetStart()) {
        DCHECK(use->IsSynthesized());
        use = use->GetNext();
      }
      while (use != nullptr && use->GetPosition() <= range->GetEnd()) {
        DCHECK(!use->GetIsEnvironment());
        DCHECK(current->CoversSlow(use->GetPosition()) || (use->GetPosition() == range->GetEnd()));
        if (!use->IsSynthesized()) {
          LocationSummary* locations = use->GetUser()->GetLocations();
          Location expected_location = locations->InAt(use->GetInputIndex());
          // The expected (actual) location may be invalid in case the input is unused. Currently
          // this only happens for intrinsics.
          if (expected_location.IsValid()) {
            if (expected_location.IsUnallocated()) {
              locations->SetInAt(use->GetInputIndex(), source);
            } else if (!expected_location.IsConstant()) {
              AddInputMoveFor(interval->GetDefinedBy(), use->GetUser(), source, expected_location);
            }
          } else {
            DCHECK(use->GetUser()->IsInvoke());
            DCHECK(use->GetUser()->AsInvoke()->GetIntrinsic() != Intrinsics::kNone);
          }
        }
        use = use->GetNext();
      }

      // Walk over the environment uses, and update their locations.
      while (env_use != nullptr && env_use->GetPosition() < range->GetStart()) {
        env_use = env_use->GetNext();
      }

      while (env_use != nullptr && env_use->GetPosition() <= range->GetEnd()) {
        DCHECK(current->CoversSlow(env_use->GetPosition())
               || (env_use->GetPosition() == range->GetEnd()));
        HEnvironment* environment = env_use->GetEnvironment();
        environment->SetLocationAt(env_use->GetInputIndex(), source);
        env_use = env_use->GetNext();
      }

      range = range->GetNext();
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

    for (SafepointPosition* safepoint_position = current->GetFirstSafepoint();
         safepoint_position != nullptr;
         safepoint_position = safepoint_position->GetNext()) {
      DCHECK(current->CoversSlow(safepoint_position->GetPosition()));

      LocationSummary* locations = safepoint_position->GetLocations();
      if ((current->GetType() == Primitive::kPrimNot) && current->GetParent()->HasSpillSlot()) {
        DCHECK(interval->GetDefinedBy()->IsActualObject())
            << interval->GetDefinedBy()->DebugName()
            << "@" << safepoint_position->GetInstruction()->DebugName();
        locations->SetStackBit(current->GetParent()->GetSpillSlot() / kVRegSize);
      }

      switch (source.GetKind()) {
        case Location::kRegister: {
          locations->AddLiveRegister(source);
          if (kIsDebugBuild && locations->OnlyCallsOnSlowPath()) {
            DCHECK_LE(locations->GetNumberOfLiveRegisters(),
                      maximum_number_of_live_core_registers_ +
                      maximum_number_of_live_fp_registers_);
          }
          if (current->GetType() == Primitive::kPrimNot) {
            DCHECK(interval->GetDefinedBy()->IsActualObject())
                << interval->GetDefinedBy()->DebugName()
                << "@" << safepoint_position->GetInstruction()->DebugName();
            locations->SetRegisterBit(source.reg());
          }
          break;
        }
        case Location::kFpuRegister: {
          locations->AddLiveRegister(source);
          break;
        }

        case Location::kRegisterPair:
        case Location::kFpuRegisterPair: {
          locations->AddLiveRegister(source.ToLow());
          locations->AddLiveRegister(source.ToHigh());
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

  if (kIsDebugBuild) {
    // Following uses can only be synthesized uses.
    while (use != nullptr) {
      DCHECK(use->IsSynthesized());
      use = use->GetNext();
    }
  }
}

static bool IsMaterializableEntryBlockInstructionOfGraphWithIrreducibleLoop(
    HInstruction* instruction) {
  return instruction->GetBlock()->GetGraph()->HasIrreducibleLoops() &&
         (instruction->IsConstant() || instruction->IsCurrentMethod());
}

void RegisterAllocator::ConnectSplitSiblings(LiveInterval* interval,
                                             HBasicBlock* from,
                                             HBasicBlock* to) const {
  if (interval->GetNextSibling() == nullptr) {
    // Nothing to connect. The whole range was allocated to the same location.
    return;
  }

  // Find the intervals that cover `from` and `to`.
  size_t destination_position = to->GetLifetimeStart();
  size_t source_position = from->GetLifetimeEnd() - 1;
  LiveInterval* destination = interval->GetSiblingAt(destination_position);
  LiveInterval* source = interval->GetSiblingAt(source_position);

  if (destination == source) {
    // Interval was not split.
    return;
  }

  LiveInterval* parent = interval->GetParent();
  HInstruction* defined_by = parent->GetDefinedBy();
  if (codegen_->GetGraph()->HasIrreducibleLoops() &&
      (destination == nullptr || !destination->CoversSlow(destination_position))) {
    // Our live_in fixed point calculation has found that the instruction is live
    // in the `to` block because it will eventually enter an irreducible loop. Our
    // live interval computation however does not compute a fixed point, and
    // therefore will not have a location for that instruction for `to`.
    // Because the instruction is a constant or the ArtMethod, we don't need to
    // do anything: it will be materialized in the irreducible loop.
    DCHECK(IsMaterializableEntryBlockInstructionOfGraphWithIrreducibleLoop(defined_by))
        << defined_by->DebugName() << ":" << defined_by->GetId()
        << " " << from->GetBlockId() << " -> " << to->GetBlockId();
    return;
  }

  if (!destination->HasRegister()) {
    // Values are eagerly spilled. Spill slot already contains appropriate value.
    return;
  }

  Location location_source;
  // `GetSiblingAt` returns the interval whose start and end cover `position`,
  // but does not check whether the interval is inactive at that position.
  // The only situation where the interval is inactive at that position is in the
  // presence of irreducible loops for constants and ArtMethod.
  if (codegen_->GetGraph()->HasIrreducibleLoops() &&
      (source == nullptr || !source->CoversSlow(source_position))) {
    DCHECK(IsMaterializableEntryBlockInstructionOfGraphWithIrreducibleLoop(defined_by));
    if (defined_by->IsConstant()) {
      location_source = defined_by->GetLocations()->Out();
    } else {
      DCHECK(defined_by->IsCurrentMethod());
      location_source = parent->NeedsTwoSpillSlots()
          ? Location::DoubleStackSlot(parent->GetSpillSlot())
          : Location::StackSlot(parent->GetSpillSlot());
    }
  } else {
    DCHECK(source != nullptr);
    DCHECK(source->CoversSlow(source_position));
    DCHECK(destination->CoversSlow(destination_position));
    location_source = source->ToLocation();
  }

  // If `from` has only one successor, we can put the moves at the exit of it. Otherwise
  // we need to put the moves at the entry of `to`.
  if (from->GetNormalSuccessors().size() == 1) {
    InsertParallelMoveAtExitOf(from,
                               defined_by,
                               location_source,
                               destination->ToLocation());
  } else {
    DCHECK_EQ(to->GetPredecessors().size(), 1u);
    InsertParallelMoveAtEntryOf(to,
                                defined_by,
                                location_source,
                                destination->ToLocation());
  }
}

void RegisterAllocator::Resolve() {
  codegen_->InitializeCodeGeneration(GetNumberOfSpillSlots(),
                                     maximum_number_of_live_core_registers_,
                                     maximum_number_of_live_fp_registers_,
                                     reserved_out_slots_,
                                     codegen_->GetGraph()->GetLinearOrder());

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
    } else if (instruction->IsCurrentMethod()) {
      // The current method is always at offset 0.
      DCHECK(!current->HasSpillSlot() || (current->GetSpillSlot() == 0));
    } else if (instruction->IsPhi() && instruction->AsPhi()->IsCatchPhi()) {
      DCHECK(current->HasSpillSlot());
      size_t slot = current->GetSpillSlot()
                    + GetNumberOfSpillSlots()
                    + reserved_out_slots_
                    - catch_phi_spill_slots_;
      current->SetSpillSlot(slot * kVRegSize);
    } else if (current->HasSpillSlot()) {
      // Adjust the stack slot, now that we know the number of them for each type.
      // The way this implementation lays out the stack is the following:
      // [parameter slots       ]
      // [catch phi spill slots ]
      // [double spill slots    ]
      // [long spill slots      ]
      // [float spill slots     ]
      // [int/ref values        ]
      // [maximum out values    ] (number of arguments for calls)
      // [art method            ].
      size_t slot = current->GetSpillSlot();
      switch (current->GetType()) {
        case Primitive::kPrimDouble:
          slot += long_spill_slots_.size();
          FALLTHROUGH_INTENDED;
        case Primitive::kPrimLong:
          slot += float_spill_slots_.size();
          FALLTHROUGH_INTENDED;
        case Primitive::kPrimFloat:
          slot += int_spill_slots_.size();
          FALLTHROUGH_INTENDED;
        case Primitive::kPrimNot:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
        case Primitive::kPrimByte:
        case Primitive::kPrimBoolean:
        case Primitive::kPrimShort:
          slot += reserved_out_slots_;
          break;
        case Primitive::kPrimVoid:
          LOG(FATAL) << "Unexpected type for interval " << current->GetType();
      }
      current->SetSpillSlot(slot * kVRegSize);
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
      locations->UpdateOut(source);
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
  for (HLinearOrderIterator it(*codegen_->GetGraph()); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (block->IsCatchBlock() ||
        (block->IsLoopHeader() && block->GetLoopInformation()->IsIrreducible())) {
      // Instructions live at the top of catch blocks or irreducible loop header
      // were forced to spill.
      if (kIsDebugBuild) {
        BitVector* live = liveness_.GetLiveInSet(*block);
        for (uint32_t idx : live->Indexes()) {
          LiveInterval* interval = liveness_.GetInstructionFromSsaIndex(idx)->GetLiveInterval();
          LiveInterval* sibling = interval->GetSiblingAt(block->GetLifetimeStart());
          // `GetSiblingAt` returns the sibling that contains a position, but there could be
          // a lifetime hole in it. `CoversSlow` returns whether the interval is live at that
          // position.
          if ((sibling != nullptr) && sibling->CoversSlow(block->GetLifetimeStart())) {
            DCHECK(!sibling->HasRegister());
          }
        }
      }
    } else {
      BitVector* live = liveness_.GetLiveInSet(*block);
      for (uint32_t idx : live->Indexes()) {
        LiveInterval* interval = liveness_.GetInstructionFromSsaIndex(idx)->GetLiveInterval();
        for (HBasicBlock* predecessor : block->GetPredecessors()) {
          ConnectSplitSiblings(interval, predecessor, block);
        }
      }
    }
  }

  // Resolve phi inputs. Order does not matter.
  for (HLinearOrderIterator it(*codegen_->GetGraph()); !it.Done(); it.Advance()) {
    HBasicBlock* current = it.Current();
    if (current->IsCatchBlock()) {
      // Catch phi values are set at runtime by the exception delivery mechanism.
    } else {
      for (HInstructionIterator inst_it(current->GetPhis()); !inst_it.Done(); inst_it.Advance()) {
        HInstruction* phi = inst_it.Current();
        for (size_t i = 0, e = current->GetPredecessors().size(); i < e; ++i) {
          HBasicBlock* predecessor = current->GetPredecessors()[i];
          DCHECK_EQ(predecessor->GetNormalSuccessors().size(), 1u);
          HInstruction* input = phi->InputAt(i);
          Location source = input->GetLiveInterval()->GetLocationAt(
              predecessor->GetLifetimeEnd() - 1);
          Location destination = phi->GetLiveInterval()->ToLocation();
          InsertParallelMoveAtExitOf(predecessor, phi, source, destination);
        }
      }
    }
  }

  // Assign temp locations.
  for (LiveInterval* temp : temp_intervals_) {
    if (temp->IsHighInterval()) {
      // High intervals can be skipped, they are already handled by the low interval.
      continue;
    }
    HInstruction* at = liveness_.GetTempUser(temp);
    size_t temp_index = liveness_.GetTempIndex(temp);
    LocationSummary* locations = at->GetLocations();
    switch (temp->GetType()) {
      case Primitive::kPrimInt:
        locations->SetTempAt(temp_index, Location::RegisterLocation(temp->GetRegister()));
        break;

      case Primitive::kPrimDouble:
        if (codegen_->NeedsTwoRegisters(Primitive::kPrimDouble)) {
          Location location = Location::FpuRegisterPairLocation(
              temp->GetRegister(), temp->GetHighInterval()->GetRegister());
          locations->SetTempAt(temp_index, location);
        } else {
          locations->SetTempAt(temp_index, Location::FpuRegisterLocation(temp->GetRegister()));
        }
        break;

      default:
        LOG(FATAL) << "Unexpected type for temporary location "
                   << temp->GetType();
    }
  }
}

}  // namespace art
