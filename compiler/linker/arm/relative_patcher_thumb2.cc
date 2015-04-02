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

#include "linker/arm/relative_patcher_thumb2.h"

#include "compiled_method.h"
#include "mirror/art_method.h"
#include "utils/arm/assembler_thumb2.h"

namespace art {
namespace linker {

Thumb2RelativePatcher::Thumb2RelativePatcher(RelativePatcherTargetProvider* provider)
    : ArmBaseRelativePatcher(provider, kThumb2, CompileThunkCode(),
                             kMaxPositiveDisplacement, kMaxNegativeDisplacement) {
}

void Thumb2RelativePatcher::PatchCall(std::vector<uint8_t>* code, uint32_t literal_offset,
                                      uint32_t patch_offset, uint32_t target_offset) {
  DCHECK_LE(literal_offset + 4u, code->size());
  DCHECK_EQ(literal_offset & 1u, 0u);
  DCHECK_EQ(patch_offset & 1u, 0u);
  DCHECK_EQ(target_offset & 1u, 1u);  // Thumb2 mode bit.
  uint32_t displacement = CalculateDisplacement(patch_offset, target_offset & ~1u);
  displacement -= kPcDisplacement;  // The base PC is at the end of the 4-byte patch.
  DCHECK_EQ(displacement & 1u, 0u);
  DCHECK((displacement >> 24) == 0u || (displacement >> 24) == 255u);  // 25-bit signed.
  uint32_t signbit = (displacement >> 31) & 0x1;
  uint32_t i1 = (displacement >> 23) & 0x1;
  uint32_t i2 = (displacement >> 22) & 0x1;
  uint32_t imm10 = (displacement >> 12) & 0x03ff;
  uint32_t imm11 = (displacement >> 1) & 0x07ff;
  uint32_t j1 = i1 ^ (signbit ^ 1);
  uint32_t j2 = i2 ^ (signbit ^ 1);
  uint32_t value = (signbit << 26) | (j1 << 13) | (j2 << 11) | (imm10 << 16) | imm11;
  value |= 0xf000d000;  // BL

  uint8_t* addr = &(*code)[literal_offset];
  // Check that we're just overwriting an existing BL.
  DCHECK_EQ(addr[1] & 0xf8, 0xf0);
  DCHECK_EQ(addr[3] & 0xd0, 0xd0);
  // Write the new BL.
  addr[0] = (value >> 16) & 0xff;
  addr[1] = (value >> 24) & 0xff;
  addr[2] = (value >> 0) & 0xff;
  addr[3] = (value >> 8) & 0xff;
}

void Thumb2RelativePatcher::PatchDexCacheReference(std::vector<uint8_t>* code ATTRIBUTE_UNUSED,
                                                   const LinkerPatch& patch ATTRIBUTE_UNUSED,
                                                   uint32_t patch_offset ATTRIBUTE_UNUSED,
                                                   uint32_t target_offset ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "Unexpected relative dex cache array patch.";
}

std::vector<uint8_t> Thumb2RelativePatcher::CompileThunkCode() {
  // The thunk just uses the entry point in the ArtMethod. This works even for calls
  // to the generic JNI and interpreter trampolines.
  arm::Thumb2Assembler assembler;
  assembler.LoadFromOffset(
      arm::kLoadWord, arm::PC, arm::R0,
      mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArmPointerSize).Int32Value());
  assembler.bkpt(0);
  std::vector<uint8_t> thunk_code(assembler.CodeSize());
  MemoryRegion code(thunk_code.data(), thunk_code.size());
  assembler.FinalizeInstructions(code);
  return thunk_code;
}

}  // namespace linker
}  // namespace art
