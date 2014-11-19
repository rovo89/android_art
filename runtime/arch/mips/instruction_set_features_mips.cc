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

#include "instruction_set_features_mips.h"

#include <fstream>
#include <sstream>

#include "base/stringprintf.h"
#include "utils.h"  // For Trim.

namespace art {

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromVariant(
    const std::string& variant ATTRIBUTE_UNUSED, std::string* error_msg ATTRIBUTE_UNUSED) {
  if (variant != "default") {
    std::ostringstream os;
    LOG(WARNING) << "Unexpected CPU variant for Mips using defaults: " << variant;
  }
  bool smp = true;  // Conservative default.
  bool fpu_32bit = true;
  bool mips_isa_gte2 = true;
  return new MipsInstructionSetFeatures(smp, fpu_32bit, mips_isa_gte2);
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromBitmap(uint32_t bitmap) {
  bool smp = (bitmap & kSmpBitfield) != 0;
  bool fpu_32bit = (bitmap & kFpu32Bitfield) != 0;
  bool mips_isa_gte2 = (bitmap & kIsaRevGte2Bitfield) != 0;
  return new MipsInstructionSetFeatures(smp, fpu_32bit, mips_isa_gte2);
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromCppDefines() {
  const bool smp = true;

  // TODO: here we assume the FPU is always 32-bit.
  const bool fpu_32bit = true;

#if __mips_isa_rev >= 2
  const bool mips_isa_gte2 = true;
#else
  const bool mips_isa_gte2 = false;
#endif

  return new MipsInstructionSetFeatures(smp, fpu_32bit, mips_isa_gte2);
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromCpuInfo() {
  // Look in /proc/cpuinfo for features we need.  Only use this when we can guarantee that
  // the kernel puts the appropriate feature flags in here.  Sometimes it doesn't.
  bool smp = false;

  // TODO: here we assume the FPU is always 32-bit.
  const bool fpu_32bit = true;

  // TODO: here we assume all MIPS processors are >= v2.
#if __mips_isa_rev >= 2
  const bool mips_isa_gte2 = true;
#else
  const bool mips_isa_gte2 = false;
#endif

  std::ifstream in("/proc/cpuinfo");
  if (!in.fail()) {
    while (!in.eof()) {
      std::string line;
      std::getline(in, line);
      if (!in.eof()) {
        LOG(INFO) << "cpuinfo line: " << line;
        if (line.find("processor") != std::string::npos && line.find(": 1") != std::string::npos) {
          smp = true;
        }
      }
    }
    in.close();
  } else {
    LOG(ERROR) << "Failed to open /proc/cpuinfo";
  }
  return new MipsInstructionSetFeatures(smp, fpu_32bit, mips_isa_gte2);
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromHwcap() {
  UNIMPLEMENTED(WARNING);
  return FromCppDefines();
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromAssembly() {
  UNIMPLEMENTED(WARNING);
  return FromCppDefines();
}

bool MipsInstructionSetFeatures::Equals(const InstructionSetFeatures* other) const {
  if (kMips != other->GetInstructionSet()) {
    return false;
  }
  const MipsInstructionSetFeatures* other_as_mips = other->AsMipsInstructionSetFeatures();
  return (IsSmp() == other->IsSmp()) &&
      (fpu_32bit_ == other_as_mips->fpu_32bit_) &&
      (mips_isa_gte2_ == other_as_mips->mips_isa_gte2_);
}

uint32_t MipsInstructionSetFeatures::AsBitmap() const {
  return (IsSmp() ? kSmpBitfield : 0) |
      (fpu_32bit_ ? kFpu32Bitfield : 0) |
      (mips_isa_gte2_ ? kIsaRevGte2Bitfield : 0);
}

std::string MipsInstructionSetFeatures::GetFeatureString() const {
  std::string result;
  if (IsSmp()) {
    result += "smp";
  } else {
    result += "-smp";
  }
  if (fpu_32bit_) {
    result += ",fpu32";
  } else {
    result += ",-fpu32";
  }
  if (mips_isa_gte2_) {
    result += ",mips2";
  } else {
    result += ",-mips2";
  }
  return result;
}

const InstructionSetFeatures* MipsInstructionSetFeatures::AddFeaturesFromSplitString(
    const bool smp, const std::vector<std::string>& features, std::string* error_msg) const {
  bool fpu_32bit = fpu_32bit_;
  bool mips_isa_gte2 = mips_isa_gte2_;
  for (auto i = features.begin(); i != features.end(); i++) {
    std::string feature = Trim(*i);
    if (feature == "fpu32") {
      fpu_32bit = true;
    } else if (feature == "-fpu32") {
      fpu_32bit = false;
    } else if (feature == "mips2") {
      mips_isa_gte2 = true;
    } else if (feature == "-mips2") {
      mips_isa_gte2 = false;
    } else {
      *error_msg = StringPrintf("Unknown instruction set feature: '%s'", feature.c_str());
      return nullptr;
    }
  }
  return new MipsInstructionSetFeatures(smp, fpu_32bit, mips_isa_gte2);
}

}  // namespace art
