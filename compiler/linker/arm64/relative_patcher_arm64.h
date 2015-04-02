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

#ifndef ART_COMPILER_LINKER_ARM64_RELATIVE_PATCHER_ARM64_H_
#define ART_COMPILER_LINKER_ARM64_RELATIVE_PATCHER_ARM64_H_

#include "linker/arm/relative_patcher_arm_base.h"
#include "utils/array_ref.h"

namespace art {
namespace linker {

class Arm64RelativePatcher FINAL : public ArmBaseRelativePatcher {
 public:
  Arm64RelativePatcher(RelativePatcherTargetProvider* provider,
                       const Arm64InstructionSetFeatures* features);

  uint32_t ReserveSpace(uint32_t offset, const CompiledMethod* compiled_method,
                        MethodReference method_ref) OVERRIDE;
  uint32_t ReserveSpaceEnd(uint32_t offset) OVERRIDE;
  uint32_t WriteThunks(OutputStream* out, uint32_t offset) OVERRIDE;
  void PatchCall(std::vector<uint8_t>* code, uint32_t literal_offset,
                 uint32_t patch_offset, uint32_t target_offset) OVERRIDE;
  void PatchDexCacheReference(std::vector<uint8_t>* code, const LinkerPatch& patch,
                              uint32_t patch_offset, uint32_t target_offset) OVERRIDE;

 private:
  static std::vector<uint8_t> CompileThunkCode();
  static uint32_t PatchAdrp(uint32_t adrp, uint32_t disp);

  static bool NeedsErratum843419Thunk(ArrayRef<const uint8_t> code, uint32_t literal_offset,
                                      uint32_t patch_offset);
  void SetInsn(std::vector<uint8_t>* code, uint32_t offset, uint32_t value);
  static uint32_t GetInsn(ArrayRef<const uint8_t> code, uint32_t offset);

  template <typename Alloc>
  static uint32_t GetInsn(std::vector<uint8_t, Alloc>* code, uint32_t offset);

  // Maximum positive and negative displacement measured from the patch location.
  // (Signed 28 bit displacement with the last bit 0 has range [-2^27, 2^27-4] measured from
  // the ARM64 PC pointing to the BL.)
  static constexpr uint32_t kMaxPositiveDisplacement = (1u << 27) - 4u;
  static constexpr uint32_t kMaxNegativeDisplacement = (1u << 27);

  // The ADRP thunk for erratum 843419 is 2 instructions, i.e. 8 bytes.
  static constexpr uint32_t kAdrpThunkSize = 8u;

  const bool fix_cortex_a53_843419_;
  // Map original patch_offset to thunk offset.
  std::vector<std::pair<uint32_t, uint32_t>> adrp_thunk_locations_;
  size_t reserved_adrp_thunks_;
  size_t processed_adrp_thunks_;
  std::vector<uint8_t> current_method_thunks_;

  DISALLOW_COPY_AND_ASSIGN(Arm64RelativePatcher);
};

}  // namespace linker
}  // namespace art

#endif  // ART_COMPILER_LINKER_ARM64_RELATIVE_PATCHER_ARM64_H_
