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

#include <signal.h>
#include <fstream>

#include "base/casts.h"
#include "base/stringprintf.h"
#include "utils.h"

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
      UNREACHABLE();
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

const InstructionSetFeatures* InstructionSetFeatures::FromVariant(InstructionSet isa,
                                                                  const std::string& variant,
                                                                  std::string* error_msg) {
  const InstructionSetFeatures* result;
  switch (isa) {
    case kArm:
    case kThumb2:
      result = ArmInstructionSetFeatures::FromVariant(variant, error_msg);
      break;
    default:
      result = UnknownInstructionSetFeatures::Unknown(isa);
      break;
  }
  CHECK_EQ(result == nullptr, error_msg->size() != 0);
  return result;
}

const InstructionSetFeatures* InstructionSetFeatures::FromFeatureString(InstructionSet isa,
                                                                        const std::string& feature_list,
                                                                        std::string* error_msg) {
  const InstructionSetFeatures* result;
  switch (isa) {
    case kArm:
    case kThumb2:
      result = ArmInstructionSetFeatures::FromFeatureString(feature_list, error_msg);
      break;
    default:
      result = UnknownInstructionSetFeatures::Unknown(isa);
      break;
  }
  // TODO: warn if feature_list doesn't agree with result's GetFeatureList().
  CHECK_EQ(result == nullptr, error_msg->size() != 0);
  return result;
}

const InstructionSetFeatures* InstructionSetFeatures::FromBitmap(InstructionSet isa,
                                                                 uint32_t bitmap) {
  const InstructionSetFeatures* result;
  switch (isa) {
    case kArm:
    case kThumb2:
      result = ArmInstructionSetFeatures::FromBitmap(bitmap);
      break;
    default:
      result = UnknownInstructionSetFeatures::Unknown(isa);
      break;
  }
  CHECK_EQ(bitmap, result->AsBitmap());
  return result;
}

const InstructionSetFeatures* InstructionSetFeatures::FromCppDefines() {
  const InstructionSetFeatures* result;
  switch (kRuntimeISA) {
    case kArm:
    case kThumb2:
      result = ArmInstructionSetFeatures::FromCppDefines();
      break;
    default:
      result = UnknownInstructionSetFeatures::Unknown(kRuntimeISA);
      break;
  }
  return result;
}


const InstructionSetFeatures* InstructionSetFeatures::FromCpuInfo() {
  const InstructionSetFeatures* result;
  switch (kRuntimeISA) {
    case kArm:
    case kThumb2:
      result = ArmInstructionSetFeatures::FromCpuInfo();
      break;
    default:
      result = UnknownInstructionSetFeatures::Unknown(kRuntimeISA);
      break;
  }
  return result;
}

const InstructionSetFeatures* InstructionSetFeatures::FromHwcap() {
  const InstructionSetFeatures* result;
  switch (kRuntimeISA) {
    case kArm:
    case kThumb2:
      result = ArmInstructionSetFeatures::FromHwcap();
      break;
    default:
      result = UnknownInstructionSetFeatures::Unknown(kRuntimeISA);
      break;
  }
  return result;
}

const InstructionSetFeatures* InstructionSetFeatures::FromAssembly() {
  const InstructionSetFeatures* result;
  switch (kRuntimeISA) {
    case kArm:
    case kThumb2:
      result = ArmInstructionSetFeatures::FromAssembly();
      break;
    default:
      result = UnknownInstructionSetFeatures::Unknown(kRuntimeISA);
      break;
  }
  return result;
}

const ArmInstructionSetFeatures* InstructionSetFeatures::AsArmInstructionSetFeatures() const {
  DCHECK_EQ(kArm, GetInstructionSet());
  return down_cast<const ArmInstructionSetFeatures*>(this);
}

std::ostream& operator<<(std::ostream& os, const InstructionSetFeatures& rhs) {
  os << "ISA: " << rhs.GetInstructionSet() << " Feature string: " << rhs.GetFeatureString();
  return os;
}

const ArmInstructionSetFeatures* ArmInstructionSetFeatures::FromFeatureString(
    const std::string& feature_list, std::string* error_msg) {
  std::vector<std::string> features;
  Split(feature_list, ',', &features);
  bool has_lpae = false;
  bool has_div = false;
  for (auto i = features.begin(); i != features.end(); i++) {
    std::string feature = Trim(*i);
    if (feature == "default" || feature == "none") {
      // Nothing to do.
    } else if (feature == "div") {
      has_div = true;
    } else if (feature == "nodiv") {
      has_div = false;
    } else if (feature == "lpae") {
      has_lpae = true;
    } else if (feature == "nolpae") {
      has_lpae = false;
    } else {
      *error_msg = StringPrintf("Unknown instruction set feature: '%s'", feature.c_str());
      return nullptr;
    }
  }
  return new ArmInstructionSetFeatures(has_lpae, has_div);
}

const ArmInstructionSetFeatures* ArmInstructionSetFeatures::FromVariant(
    const std::string& variant, std::string* error_msg) {
  // Look for variants that have divide support.
  bool has_div = false;
  {
    static const char* arm_variants_with_div[] = {
        "cortex-a7", "cortex-a12", "cortex-a15", "cortex-a17", "cortex-a53", "cortex-a57",
        "cortex-m3", "cortex-m4", "cortex-r4", "cortex-r5",
        "cyclone", "denver", "krait", "swift"
    };
    for (const char* div_variant : arm_variants_with_div) {
      if (variant == div_variant) {
        has_div = true;
        break;
      }
    }
  }
  // Look for variants that have LPAE support.
  bool has_lpae = false;
  {
    static const char* arm_variants_with_lpae[] = {
        "cortex-a7", "cortex-a15", "krait", "denver"
    };
    for (const char* lpae_variant : arm_variants_with_lpae) {
      if (variant == lpae_variant) {
        has_lpae = true;
        break;
      }
    }
  }
  if (has_div == false && has_lpae == false) {
    // Avoid unsupported variants.
    static const char* unsupported_arm_variants[] = {
        // ARM processors that aren't ARMv7 compatible aren't supported.
        "arm2", "arm250", "arm3", "arm6", "arm60", "arm600", "arm610", "arm620",
        "cortex-m0", "cortex-m0plus", "cortex-m1",
        "fa526", "fa626", "fa606te", "fa626te", "fmp626", "fa726te",
        "iwmmxt", "iwmmxt2",
        "strongarm", "strongarm110", "strongarm1100", "strongarm1110",
        "xscale"
    };
    for (const char* us_variant : unsupported_arm_variants) {
      if (variant == us_variant) {
        *error_msg = StringPrintf("Attempt to use unsupported ARM variant: %s", us_variant);
        return nullptr;
      }
    }
    // Warn if the variant is unknown.
    // TODO: some of the variants below may have feature support, but that support is currently
    //       unknown so we'll choose conservative (sub-optimal) defaults without warning.
    // TODO: some of the architectures may not support all features required by ART and should be
    //       moved to unsupported_arm_variants[] above.
    static const char* arm_variants_without_known_features[] = {
        "arm7", "arm7m", "arm7d", "arm7dm", "arm7di", "arm7dmi", "arm70", "arm700", "arm700i",
        "arm710", "arm710c", "arm7100", "arm720", "arm7500", "arm7500fe", "arm7tdmi", "arm7tdmi-s",
        "arm710t", "arm720t", "arm740t",
        "arm8", "arm810",
        "arm9", "arm9e", "arm920", "arm920t", "arm922t", "arm946e-s", "arm966e-s", "arm968e-s",
        "arm926ej-s", "arm940t", "arm9tdmi",
        "arm10tdmi", "arm1020t", "arm1026ej-s", "arm10e", "arm1020e", "arm1022e",
        "arm1136j-s", "arm1136jf-s",
        "arm1156t2-s", "arm1156t2f-s", "arm1176jz-s", "arm1176jzf-s",
        "cortex-a5", "cortex-a8", "cortex-a9", "cortex-a9-mp", "cortex-r4f",
        "marvell-pj4", "mpcore", "mpcorenovfp"
    };
    bool found = false;
    for (const char* ff_variant : arm_variants_without_known_features) {
      if (variant == ff_variant) {
        found = true;
        break;
      }
    }
    if (!found) {
      LOG(WARNING) << "Unknown instruction set features for ARM CPU variant (" << variant
          << ") using conservative defaults";
    }
  }
  return new ArmInstructionSetFeatures(has_lpae, has_div);
}

const ArmInstructionSetFeatures* ArmInstructionSetFeatures::FromBitmap(uint32_t bitmap) {
  bool has_lpae = (bitmap & kLpaeBitfield) != 0;
  bool has_div = (bitmap & kDivBitfield) != 0;
  return new ArmInstructionSetFeatures(has_lpae, has_div);
}

const ArmInstructionSetFeatures* ArmInstructionSetFeatures::FromCppDefines() {
#if defined(__ARM_ARCH_EXT_IDIV__)
  bool has_div = true;
#else
  bool has_div = false;
#endif
#if defined(__ARM_FEATURE_LPAE)
  bool has_lpae = true;
#else
  bool has_lpae = false;
#endif
  return new ArmInstructionSetFeatures(has_lpae, has_div);
}

const ArmInstructionSetFeatures* ArmInstructionSetFeatures::FromCpuInfo() {
  // Look in /proc/cpuinfo for features we need.  Only use this when we can guarantee that
  // the kernel puts the appropriate feature flags in here.  Sometimes it doesn't.
  bool has_lpae = false;
  bool has_div = false;

  std::ifstream in("/proc/cpuinfo");
  if (!in.fail()) {
    while (!in.eof()) {
      std::string line;
      std::getline(in, line);
      if (!in.eof()) {
        LOG(INFO) << "cpuinfo line: " << line;
        if (line.find("Features") != std::string::npos) {
          LOG(INFO) << "found features";
          if (line.find("idivt") != std::string::npos) {
            // We always expect both ARM and Thumb divide instructions to be available or not
            // available.
            CHECK_NE(line.find("idiva"), std::string::npos);
            has_div = true;
          }
          if (line.find("lpae") != std::string::npos) {
            has_lpae = true;
          }
        }
      }
    }
    in.close();
  } else {
    LOG(INFO) << "Failed to open /proc/cpuinfo";
  }
  return new ArmInstructionSetFeatures(has_lpae, has_div);
}

#if defined(HAVE_ANDROID_OS) && defined(__arm__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif

const ArmInstructionSetFeatures* ArmInstructionSetFeatures::FromHwcap() {
  bool has_lpae = false;
  bool has_div = false;

#if defined(HAVE_ANDROID_OS) && defined(__arm__)
  uint64_t hwcaps = getauxval(AT_HWCAP);
  LOG(INFO) << "hwcaps=" << hwcaps;
  if ((hwcaps & HWCAP_IDIVT) != 0) {
    // We always expect both ARM and Thumb divide instructions to be available or not
    // available.
    CHECK_NE(hwcaps & HWCAP_IDIVA, 0U);
    has_div = true;
  }
  if ((hwcaps & HWCAP_LPAE) != 0) {
    has_lpae = true;
  }
#endif

  return new ArmInstructionSetFeatures(has_lpae, has_div);
}

// A signal handler called by a fault for an illegal instruction.  We record the fact in r0
// and then increment the PC in the signal context to return to the next instruction.  We know the
// instruction is an sdiv (4 bytes long).
static void bad_divide_inst_handle(int signo, siginfo_t* si, void* data) {
  UNUSED(signo);
  UNUSED(si);
#if defined(__arm__)
  struct ucontext *uc = (struct ucontext *)data;
  struct sigcontext *sc = &uc->uc_mcontext;
  sc->arm_r0 = 0;     // Set R0 to #0 to signal error.
  sc->arm_pc += 4;    // Skip offending instruction.
#else
  UNUSED(data);
#endif
}

#if defined(__arm__)
extern "C" bool artCheckForARMSDIVInstruction();
#endif

const ArmInstructionSetFeatures* ArmInstructionSetFeatures::FromAssembly() {
  // See if have a sdiv instruction.  Register a signal handler and try to execute an sdiv
  // instruction.  If we get a SIGILL then it's not supported.
  struct sigaction sa, osa;
  sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO;
  sa.sa_sigaction = bad_divide_inst_handle;
  sigaction(SIGILL, &sa, &osa);

  bool has_div = false;
#if defined(__arm__)
  if (artCheckForARMSDIVInstruction()) {
    has_div = true;
  }
#endif

  // Restore the signal handler.
  sigaction(SIGILL, &osa, nullptr);

  // Use compile time features to "detect" LPAE support.
  // TODO: write an assembly LPAE support test.
#if defined(__ARM_FEATURE_LPAE)
  bool has_lpae = true;
#else
  bool has_lpae = false;
#endif
  return new ArmInstructionSetFeatures(has_lpae, has_div);
}


bool ArmInstructionSetFeatures::Equals(const InstructionSetFeatures* other) const {
  if (kArm != other->GetInstructionSet()) {
    return false;
  }
  const ArmInstructionSetFeatures* other_as_arm = other->AsArmInstructionSetFeatures();
  return has_lpae_ == other_as_arm->has_lpae_ && has_div_ == other_as_arm->has_div_;
}

uint32_t ArmInstructionSetFeatures::AsBitmap() const {
  return (has_lpae_ ? kLpaeBitfield : 0) | (has_div_ ? kDivBitfield : 0);
}

std::string ArmInstructionSetFeatures::GetFeatureString() const {
  std::string result;
  if (has_div_) {
    result += ",div";
  }
  if (has_lpae_) {
    result += ",lpae";
  }
  if (result.size() == 0) {
    return "none";
  } else {
    // Strip leading comma.
    return result.substr(1, result.size());
  }
}

}  // namespace art
