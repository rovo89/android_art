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

#include "art_method.h"
#include "compiled_method.h"
#include "utils/arm/assembler_thumb2.h"

namespace art {
namespace linker {

Thumb2RelativePatcher::Thumb2RelativePatcher(RelativePatcherTargetProvider* provider)
    : ArmBaseRelativePatcher(provider, kThumb2, CompileThunkCode(),
                             kMaxPositiveDisplacement, kMaxNegativeDisplacement) {
}

void Thumb2RelativePatcher::PatchCall(std::vector<uint8_t>* code,
                                      uint32_t literal_offset,
                                      uint32_t patch_offset,
                                      uint32_t target_offset) {
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

  // Check that we're just overwriting an existing BL.
  DCHECK_EQ(GetInsn32(code, literal_offset) & 0xf800d000, 0xf000d000);
  // Write the new BL.
  SetInsn32(code, literal_offset, value);
}

void Thumb2RelativePatcher::PatchPcRelativeReference(std::vector<uint8_t>* code,
                                                     const LinkerPatch& patch,
                                                     uint32_t patch_offset,
                                                     uint32_t target_offset) {
  uint32_t literal_offset = patch.LiteralOffset();
  uint32_t pc_literal_offset = patch.PcInsnOffset();
  uint32_t pc_base = patch_offset + (pc_literal_offset - literal_offset) + 4u /* PC adjustment */;
  uint32_t diff = target_offset - pc_base;

  uint32_t insn = GetInsn32(code, literal_offset);
  DCHECK_EQ(insn & 0xff7ff0ffu, 0xf2400000u);  // MOVW/MOVT, unpatched (imm16 == 0).
  uint32_t diff16 = ((insn & 0x00800000u) != 0u) ? (diff >> 16) : (diff & 0xffffu);
  uint32_t imm4 = (diff16 >> 12) & 0xfu;
  uint32_t imm = (diff16 >> 11) & 0x1u;
  uint32_t imm3 = (diff16 >> 8) & 0x7u;
  uint32_t imm8 = diff16 & 0xffu;
  insn = (insn & 0xfbf08f00u) | (imm << 26) | (imm4 << 16) | (imm3 << 12) | imm8;
  SetInsn32(code, literal_offset, insn);
}

std::vector<uint8_t> Thumb2RelativePatcher::CompileThunkCode() {
  // The thunk just uses the entry point in the ArtMethod. This works even for calls
  // to the generic JNI and interpreter trampolines.
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  arm::Thumb2Assembler assembler(&arena);
  assembler.LoadFromOffset(
      arm::kLoadWord, arm::PC, arm::R0,
      ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArmPointerSize).Int32Value());
  assembler.bkpt(0);
  assembler.FinalizeCode();
  std::vector<uint8_t> thunk_code(assembler.CodeSize());
  MemoryRegion code(thunk_code.data(), thunk_code.size());
  assembler.FinalizeInstructions(code);
  return thunk_code;
}

void Thumb2RelativePatcher::SetInsn32(std::vector<uint8_t>* code, uint32_t offset, uint32_t value) {
  DCHECK_LE(offset + 4u, code->size());
  DCHECK_EQ(offset & 1u, 0u);
  uint8_t* addr = &(*code)[offset];
  addr[0] = (value >> 16) & 0xff;
  addr[1] = (value >> 24) & 0xff;
  addr[2] = (value >> 0) & 0xff;
  addr[3] = (value >> 8) & 0xff;
}

uint32_t Thumb2RelativePatcher::GetInsn32(ArrayRef<const uint8_t> code, uint32_t offset) {
  DCHECK_LE(offset + 4u, code.size());
  DCHECK_EQ(offset & 1u, 0u);
  const uint8_t* addr = &code[offset];
  return
      (static_cast<uint32_t>(addr[0]) << 16) +
      (static_cast<uint32_t>(addr[1]) << 24) +
      (static_cast<uint32_t>(addr[2]) << 0)+
      (static_cast<uint32_t>(addr[3]) << 8);
}

template <typename Vector>
uint32_t Thumb2RelativePatcher::GetInsn32(Vector* code, uint32_t offset) {
  static_assert(std::is_same<typename Vector::value_type, uint8_t>::value, "Invalid value type");
  return GetInsn32(ArrayRef<const uint8_t>(*code), offset);
}

}  // namespace linker
}  // namespace art
