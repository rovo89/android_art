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

#include "instruction_set.h"

namespace art {

const char* GetInstructionSetString(const InstructionSet isa) {
  switch (isa) {
    case kArm:
    case kThumb2:
      return "arm";
    case kArm64:
      return "arm64";
    case kX86:
      return "x86";
    case kX86_64:
      return "x86_64";
    case kMips:
      return "mips";
    case kNone:
      return "none";
    default:
      LOG(FATAL) << "Unknown ISA " << isa;
      return nullptr;
  }
}

InstructionSet GetInstructionSetFromString(const char* isa_str) {
  CHECK(isa_str != nullptr);

  if (strcmp("arm", isa_str) == 0) {
    return kArm;
  } else if (strcmp("arm64", isa_str) == 0) {
    return kArm64;
  } else if (strcmp("x86", isa_str) == 0) {
    return kX86;
  } else if (strcmp("x86_64", isa_str) == 0) {
    return kX86_64;
  } else if (strcmp("mips", isa_str) == 0) {
    return kMips;
  }

  return kNone;
}

size_t GetInstructionSetAlignment(InstructionSet isa) {
  switch (isa) {
    case kArm:
      // Fall-through.
    case kThumb2:
      return kArmAlignment;
    case kArm64:
      return kArm64Alignment;
    case kX86:
      // Fall-through.
    case kX86_64:
      return kX86Alignment;
    case kMips:
      return kMipsAlignment;
    case kNone:
      LOG(FATAL) << "ISA kNone does not have alignment.";
      return 0;
    default:
      LOG(FATAL) << "Unknown ISA " << isa;
      return 0;
  }
}


static constexpr size_t kDefaultStackOverflowReservedBytes = 16 * KB;
static constexpr size_t kMipsStackOverflowReservedBytes = kDefaultStackOverflowReservedBytes;

static constexpr size_t kArmStackOverflowReservedBytes =    8 * KB;
static constexpr size_t kArm64StackOverflowReservedBytes =  8 * KB;
static constexpr size_t kX86StackOverflowReservedBytes =    8 * KB;
static constexpr size_t kX86_64StackOverflowReservedBytes = 8 * KB;

size_t GetStackOverflowReservedBytes(InstructionSet isa) {
  switch (isa) {
    case kArm:      // Intentional fall-through.
    case kThumb2:
      return kArmStackOverflowReservedBytes;

    case kArm64:
      return kArm64StackOverflowReservedBytes;

    case kMips:
      return kMipsStackOverflowReservedBytes;

    case kX86:
      return kX86StackOverflowReservedBytes;

    case kX86_64:
      return kX86_64StackOverflowReservedBytes;

    case kNone:
      LOG(FATAL) << "kNone has no stack overflow size";
      return 0;

    default:
      LOG(FATAL) << "Unknown instruction set" << isa;
      return 0;
  }
}

std::string InstructionSetFeatures::GetFeatureString() const {
  std::string result;
  if ((mask_ & kHwDiv) != 0) {
    result += "div";
  }
  if (result.size() == 0) {
    result = "none";
  }
  return result;
}

}  // namespace art
