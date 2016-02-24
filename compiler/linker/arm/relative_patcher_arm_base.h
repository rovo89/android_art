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

#ifndef ART_COMPILER_LINKER_ARM_RELATIVE_PATCHER_ARM_BASE_H_
#define ART_COMPILER_LINKER_ARM_RELATIVE_PATCHER_ARM_BASE_H_

#include <deque>

#include "linker/relative_patcher.h"
#include "method_reference.h"

namespace art {
namespace linker {

class ArmBaseRelativePatcher : public RelativePatcher {
 public:
  uint32_t ReserveSpace(uint32_t offset,
                        const CompiledMethod* compiled_method,
                        MethodReference method_ref) OVERRIDE;
  uint32_t ReserveSpaceEnd(uint32_t offset) OVERRIDE;
  uint32_t WriteThunks(OutputStream* out, uint32_t offset) OVERRIDE;

 protected:
  ArmBaseRelativePatcher(RelativePatcherTargetProvider* provider,
                         InstructionSet instruction_set,
                         std::vector<uint8_t> thunk_code,
                         uint32_t max_positive_displacement,
                         uint32_t max_negative_displacement);

  uint32_t ReserveSpaceInternal(uint32_t offset,
                                const CompiledMethod* compiled_method,
                                MethodReference method_ref,
                                uint32_t max_extra_space);
  uint32_t CalculateDisplacement(uint32_t patch_offset, uint32_t target_offset);

 private:
  bool ReserveSpaceProcessPatches(uint32_t quick_code_offset, MethodReference method_ref,
                                  uint32_t next_aligned_offset);

  RelativePatcherTargetProvider* const provider_;
  const InstructionSet instruction_set_;
  const std::vector<uint8_t> thunk_code_;
  const uint32_t max_positive_displacement_;
  const uint32_t max_negative_displacement_;
  std::vector<uint32_t> thunk_locations_;
  size_t current_thunk_to_write_;

  // ReserveSpace() tracks unprocessed patches.
  typedef std::pair<MethodReference, uint32_t> UnprocessedPatch;
  std::deque<UnprocessedPatch> unprocessed_patches_;

  friend class Arm64RelativePatcherTest;
  friend class Thumb2RelativePatcherTest;

  DISALLOW_COPY_AND_ASSIGN(ArmBaseRelativePatcher);
};

}  // namespace linker
}  // namespace art

#endif  // ART_COMPILER_LINKER_ARM_RELATIVE_PATCHER_ARM_BASE_H_
