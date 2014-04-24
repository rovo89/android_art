/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_RUNTIME_INSTRUCTION_SET_H_
#define ART_RUNTIME_INSTRUCTION_SET_H_

#include <iosfwd>
#include <string>

#include "base/macros.h"

namespace art {

enum InstructionSet {
  kNone,
  kArm,
  kArm64,
  kThumb2,
  kX86,
  kX86_64,
  kMips
};
std::ostream& operator<<(std::ostream& os, const InstructionSet& rhs);

const char* GetInstructionSetString(const InstructionSet isa);
InstructionSet GetInstructionSetFromString(const char* instruction_set);

size_t GetInstructionSetPointerSize(InstructionSet isa);
size_t GetInstructionSetAlignment(InstructionSet isa);
bool Is64BitInstructionSet(InstructionSet isa);
size_t GetBytesPerGprSpillLocation(InstructionSet isa);
size_t GetBytesPerFprSpillLocation(InstructionSet isa);

#if defined(__arm__)
static constexpr InstructionSet kRuntimeISA = kArm;
#elif defined(__aarch64__)
static constexpr InstructionSet kRuntimeISA = kArm64;
#elif defined(__mips__)
static constexpr InstructionSet kRuntimeISA = kMips;
#elif defined(__i386__)
static constexpr InstructionSet kRuntimeISA = kX86;
#elif defined(__x86_64__)
static constexpr InstructionSet kRuntimeISA = kX86_64;
#else
static constexpr InstructionSet kRuntimeISA = kNone;
#endif

enum InstructionFeatures {
  kHwDiv  = 0x1,              // Supports hardware divide.
  kHwLpae = 0x2,              // Supports Large Physical Address Extension.
};

// This is a bitmask of supported features per architecture.
class PACKED(4) InstructionSetFeatures {
 public:
  InstructionSetFeatures() : mask_(0) {}
  explicit InstructionSetFeatures(uint32_t mask) : mask_(mask) {}

  static InstructionSetFeatures GuessInstructionSetFeatures();

  bool HasDivideInstruction() const {
      return (mask_ & kHwDiv) != 0;
  }

  void SetHasDivideInstruction(bool v) {
    mask_ = (mask_ & ~kHwDiv) | (v ? kHwDiv : 0);
  }

  bool HasLpae() const {
    return (mask_ & kHwLpae) != 0;
  }

  void SetHasLpae(bool v) {
    mask_ = (mask_ & ~kHwLpae) | (v ? kHwLpae : 0);
  }

  std::string GetFeatureString() const;

  // Other features in here.

  bool operator==(const InstructionSetFeatures &peer) const {
    return mask_ == peer.mask_;
  }

  bool operator!=(const InstructionSetFeatures &peer) const {
    return mask_ != peer.mask_;
  }

  bool operator<=(const InstructionSetFeatures &peer) const {
    return (mask_ & peer.mask_) == mask_;
  }

 private:
  uint32_t mask_;
};

}  // namespace art

#endif  // ART_RUNTIME_INSTRUCTION_SET_H_
