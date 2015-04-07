/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_LINKER_ARM_RELATIVE_PATCHER_THUMB2_H_
#define ART_COMPILER_LINKER_ARM_RELATIVE_PATCHER_THUMB2_H_

#include "linker/arm/relative_patcher_arm_base.h"

namespace art {
namespace linker {

class Thumb2RelativePatcher FINAL : public ArmBaseRelativePatcher {
 public:
  explicit Thumb2RelativePatcher(RelativePatcherTargetProvider* provider);

  void PatchCall(std::vector<uint8_t>* code, uint32_t literal_offset,
                 uint32_t patch_offset, uint32_t target_offset) OVERRIDE;
  void PatchDexCacheReference(std::vector<uint8_t>* code, const LinkerPatch& patch,
                              uint32_t patch_offset, uint32_t target_offset) OVERRIDE;

 private:
  static std::vector<uint8_t> CompileThunkCode();

  void SetInsn32(std::vector<uint8_t>* code, uint32_t offset, uint32_t value);
  static uint32_t GetInsn32(ArrayRef<const uint8_t> code, uint32_t offset);

  template <typename Alloc>
  static uint32_t GetInsn32(std::vector<uint8_t, Alloc>* code, uint32_t offset);

  // PC displacement from patch location; Thumb2 PC is always at instruction address + 4.
  static constexpr int32_t kPcDisplacement = 4;

  // Maximum positive and negative displacement measured from the patch location.
  // (Signed 25 bit displacement with the last bit 0 has range [-2^24, 2^24-2] measured from
  // the Thumb2 PC pointing right after the BL, i.e. 4 bytes later than the patch location.)
  static constexpr uint32_t kMaxPositiveDisplacement = (1u << 24) - 2 + kPcDisplacement;
  static constexpr uint32_t kMaxNegativeDisplacement = (1u << 24) - kPcDisplacement;

  DISALLOW_COPY_AND_ASSIGN(Thumb2RelativePatcher);
};

}  // namespace linker
}  // namespace art

#endif  // ART_COMPILER_LINKER_ARM_RELATIVE_PATCHER_THUMB2_H_
