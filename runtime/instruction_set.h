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
  kThumb2,
  kX86,
  kMips
};

enum InstructionFeatures {
  kHwDiv = 1                  // Supports hardware divide.
};

// This is a bitmask of supported features per architecture.
class PACKED(4) InstructionSetFeatures {
 public:
  InstructionSetFeatures() : mask_(0) {}
  explicit InstructionSetFeatures(uint32_t mask) : mask_(mask) {}

  bool HasDivideInstruction() const {
      return (mask_ & kHwDiv) != 0;
  }

  void SetHasDivideInstruction(bool v) {
    mask_ = (mask_ & ~kHwDiv) | (v ? kHwDiv : 0);
  }

  std::string GetFeatureString() const {
    std::string result;
    if ((mask_ & kHwDiv) != 0) {
      result += "div";
    }
    if (result.size() == 0) {
      result = "none";
    }
    return result;
  }

  uint32_t get_mask() const {
    return mask_;
  }

  // Other features in here.

  bool operator==(const InstructionSetFeatures &peer) const {
    return mask_ == peer.mask_;
  }

  bool operator!=(const InstructionSetFeatures &peer) const {
    return mask_ != peer.mask_;
  }

 private:
  uint32_t mask_;
};

std::ostream& operator<<(std::ostream& os, const InstructionSet& rhs);

}  // namespace art

#endif  // ART_RUNTIME_INSTRUCTION_SET_H_
