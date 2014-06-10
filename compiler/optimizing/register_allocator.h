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

#ifndef ART_COMPILER_OPTIMIZING_REGISTER_ALLOCATOR_H_
#define ART_COMPILER_OPTIMIZING_REGISTER_ALLOCATOR_H_

#include "base/macros.h"
#include "utils/growable_array.h"

namespace art {

class CodeGenerator;
class LiveInterval;
class SsaLivenessAnalysis;

/**
 * An implementation of a linear scan register allocator on an `HGraph` with SSA form.
 */
class RegisterAllocator {
 public:
  RegisterAllocator(ArenaAllocator* allocator, const CodeGenerator& codegen);

  // Main entry point for the register allocator. Given the liveness analysis,
  // allocates registers to live intervals.
  void AllocateRegisters(const SsaLivenessAnalysis& liveness) {
    processing_core_registers_ = true;
    AllocateRegistersInternal(liveness);
    processing_core_registers_ = false;
    AllocateRegistersInternal(liveness);
  }

  // Validate that the register allocator did not allocate the same register to
  // intervals that intersect each other. Returns false if it did not.
  bool Validate(const SsaLivenessAnalysis& liveness, bool log_fatal_on_failure) {
    processing_core_registers_ = true;
    if (!ValidateInternal(liveness, log_fatal_on_failure)) {
      return false;
    }
    processing_core_registers_ = false;
    return ValidateInternal(liveness, log_fatal_on_failure);
  }

  // Helper method for validation. Used by unit testing.
  static bool ValidateIntervals(const GrowableArray<LiveInterval*>& intervals,
                                size_t number_of_spill_slots,
                                const CodeGenerator& codegen,
                                ArenaAllocator* allocator,
                                bool processing_core_registers,
                                bool log_fatal_on_failure);

 private:
  // Main methods of the allocator.
  void LinearScan();
  bool TryAllocateFreeReg(LiveInterval* interval);
  bool AllocateBlockedReg(LiveInterval* interval);

  // Add `interval` in the sorted list of unhandled intervals.
  void AddToUnhandled(LiveInterval* interval);

  // Split `interval` at the position `at`. The new interval starts at `at`.
  LiveInterval* Split(LiveInterval* interval, size_t at);

  // Returns whether `reg` is blocked by the code generator.
  bool IsBlocked(int reg) const;

  // Allocate a spill slot for the given interval.
  void AllocateSpillSlotFor(LiveInterval* interval);

  // Helper methods.
  void AllocateRegistersInternal(const SsaLivenessAnalysis& liveness);
  bool ValidateInternal(const SsaLivenessAnalysis& liveness, bool log_fatal_on_failure) const;
  void DumpInterval(std::ostream& stream, LiveInterval* interval);

  ArenaAllocator* const allocator_;
  const CodeGenerator& codegen_;

  // List of intervals that must be processed, ordered by start position. Last entry
  // is the interval that has the lowest start position.
  GrowableArray<LiveInterval*> unhandled_;

  // List of intervals that have been processed.
  GrowableArray<LiveInterval*> handled_;

  // List of intervals that are currently active when processing a new live interval.
  // That is, they have a live range that spans the start of the new interval.
  GrowableArray<LiveInterval*> active_;

  // List of intervals that are currently inactive when processing a new live interval.
  // That is, they have a lifetime hole that spans the start of the new interval.
  GrowableArray<LiveInterval*> inactive_;

  // The spill slots allocated for live intervals.
  GrowableArray<size_t> spill_slots_;

  // True if processing core registers. False if processing floating
  // point registers.
  bool processing_core_registers_;

  // Number of registers for the current register kind (core or floating point).
  size_t number_of_registers_;

  // Temporary array, allocated ahead of time for simplicity.
  size_t* registers_array_;

  // Blocked registers, as decided by the code generator.
  bool* const blocked_registers_;

  DISALLOW_COPY_AND_ASSIGN(RegisterAllocator);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_REGISTER_ALLOCATOR_H_
