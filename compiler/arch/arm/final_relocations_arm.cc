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
#include "arch/arm/final_relocations_arm.h"
#include "compiled_method.h"
#include "driver/compiler_driver.h"
#include "oat_writer.h"
#include "dex/compiler_ir.h"

namespace art {

void FinalEntrypointRelocationSetArm::Apply(uint8_t* code, const OatWriter* writer,
                                            uint32_t address) const {
  uint32_t island_offset = writer->GetCurrentTrampolineIslandOffset();
  const bool kDebugPrint = false;

  for (auto& reloc : relocations_) {
    switch (reloc.type_) {
      case kRelocationCall: {
        // Fetch the instruction.  This is two 16 bit words.  We can't do a 32 bit load
        // because it's not guaranteed to be 4-byte aligned.
        uint32_t inst = static_cast<uint32_t>(
            *reinterpret_cast<uint16_t*>(code + reloc.code_offset_) << 16
            | *reinterpret_cast<uint16_t*>(code + reloc.code_offset_ + 2));

        // Remove the current offset from the instruction.  This is there
        // to prevent deduplication from wrongly handling this instruction.
        inst &= ~0x7ff;     // Bottom 11 bits.

        // Check that we are trying to relocate a Thumb2 BL instruction.
        CHECK_EQ(inst, 0xf000d000);

        uint32_t pc = address + reloc.code_offset_ + 4;   // Thumb PC is instruction + 4

        // The trampoline target is to a table starting at the island.  Each trampoline
        // entry is 4 bytes long.
        uint32_t target = island_offset + static_cast<uint32_t>(reloc.value_) * 4;
        int32_t delta = target - pc;


        if (kDebugPrint) {
          LOG(INFO) << "applying final relocation for island " << island_offset;
          LOG(INFO) << "pc: " << std::hex << pc << ", target: " << target <<
              ", reloc.value: " << reloc.value_ << ", delta: " << delta;
        }

        // Max range for a Thumb2 BL is 16MB. All calls will be to a lower address.
        const int32_t kMaxRange = -16 * static_cast<int32_t>(MB);
        CHECK_LT(delta, 0);
        CHECK_GT(delta, kMaxRange);

        // Modify the instruction using the T1 BL instruction format.
        // This is equivalent of a R_ARM_THM_CALL ELF relocation.
        delta >>= 1;      // Low bit is implicit.
        uint32_t signbit = (delta >> 31) & 0x1;
        uint32_t i1 = (delta >> 22) & 0x1;
        uint32_t i2 = (delta >> 21) & 0x1;
        uint32_t imm10 = (delta >> 11) & 0x03ff;
        uint32_t imm11 = delta & 0x07ff;
        uint32_t j1 = (i1 ^ signbit) ? 0 : 1;
        uint32_t j2 = (i2 ^ signbit) ? 0 : 1;
        uint32_t value = (signbit << 26) | (j1 << 13) | (j2 << 11) | (imm10 << 16) |
            imm11;

        // Blit the value into the instruction.
        inst |= value;

        // Write the instruction back.  High 16 bits first, little endian format.
        uint32_t offset = reloc.code_offset_;
        code[offset+0] = (inst >> 16) & 0xff;
        code[offset+1] = (inst >> 24) & 0xff;
        code[offset+2] = (inst >> 0) & 0xff;
        code[offset+3] = (inst >> 8) & 0xff;
        break;
      }

      default:
        LOG(FATAL) << "Unknown entrypoint relocation type " << reloc.type_;
    }
  }
}
}   // namespace art
