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

#include "linker/arm64/relative_patcher_arm64.h"

#include "arch/arm64/instruction_set_features_arm64.h"
#include "art_method.h"
#include "compiled_method.h"
#include "driver/compiler_driver.h"
#include "linker/output_stream.h"
#include "oat.h"
#include "oat_quick_method_header.h"
#include "utils/arm64/assembler_arm64.h"

namespace art {
namespace linker {

namespace {

inline bool IsAdrpPatch(const LinkerPatch& patch) {
  LinkerPatch::Type type = patch.GetType();
  return
      (type == LinkerPatch::Type::kStringRelative || type == LinkerPatch::Type::kDexCacheArray) &&
      patch.LiteralOffset() == patch.PcInsnOffset();
}

}  // anonymous namespace

Arm64RelativePatcher::Arm64RelativePatcher(RelativePatcherTargetProvider* provider,
                                           const Arm64InstructionSetFeatures* features)
    : ArmBaseRelativePatcher(provider, kArm64, CompileThunkCode(),
                             kMaxPositiveDisplacement, kMaxNegativeDisplacement),
      fix_cortex_a53_843419_(features->NeedFixCortexA53_843419()),
      reserved_adrp_thunks_(0u),
      processed_adrp_thunks_(0u) {
  if (fix_cortex_a53_843419_) {
    adrp_thunk_locations_.reserve(16u);
    current_method_thunks_.reserve(16u * kAdrpThunkSize);
  }
}

uint32_t Arm64RelativePatcher::ReserveSpace(uint32_t offset,
                                            const CompiledMethod* compiled_method,
                                            MethodReference method_ref) {
  if (!fix_cortex_a53_843419_) {
    DCHECK(adrp_thunk_locations_.empty());
    return ReserveSpaceInternal(offset, compiled_method, method_ref, 0u);
  }

  // Add thunks for previous method if any.
  if (reserved_adrp_thunks_ != adrp_thunk_locations_.size()) {
    size_t num_adrp_thunks = adrp_thunk_locations_.size() - reserved_adrp_thunks_;
    offset = CompiledMethod::AlignCode(offset, kArm64) + kAdrpThunkSize * num_adrp_thunks;
    reserved_adrp_thunks_ = adrp_thunk_locations_.size();
  }

  // Count the number of ADRP insns as the upper bound on the number of thunks needed
  // and use it to reserve space for other linker patches.
  size_t num_adrp = 0u;
  DCHECK(compiled_method != nullptr);
  for (const LinkerPatch& patch : compiled_method->GetPatches()) {
    if (IsAdrpPatch(patch)) {
      ++num_adrp;
    }
  }
  offset = ReserveSpaceInternal(offset, compiled_method, method_ref, kAdrpThunkSize * num_adrp);
  if (num_adrp == 0u) {
    return offset;
  }

  // Now that we have the actual offset where the code will be placed, locate the ADRP insns
  // that actually require the thunk.
  uint32_t quick_code_offset = compiled_method->AlignCode(offset) + sizeof(OatQuickMethodHeader);
  ArrayRef<const uint8_t> code = compiled_method->GetQuickCode();
  uint32_t thunk_offset = compiled_method->AlignCode(quick_code_offset + code.size());
  DCHECK(compiled_method != nullptr);
  for (const LinkerPatch& patch : compiled_method->GetPatches()) {
    if (IsAdrpPatch(patch)) {
      uint32_t patch_offset = quick_code_offset + patch.LiteralOffset();
      if (NeedsErratum843419Thunk(code, patch.LiteralOffset(), patch_offset)) {
        adrp_thunk_locations_.emplace_back(patch_offset, thunk_offset);
        thunk_offset += kAdrpThunkSize;
      }
    }
  }
  return offset;
}

uint32_t Arm64RelativePatcher::ReserveSpaceEnd(uint32_t offset) {
  if (!fix_cortex_a53_843419_) {
    DCHECK(adrp_thunk_locations_.empty());
  } else {
    // Add thunks for the last method if any.
    if (reserved_adrp_thunks_ != adrp_thunk_locations_.size()) {
      size_t num_adrp_thunks = adrp_thunk_locations_.size() - reserved_adrp_thunks_;
      offset = CompiledMethod::AlignCode(offset, kArm64) + kAdrpThunkSize * num_adrp_thunks;
      reserved_adrp_thunks_ = adrp_thunk_locations_.size();
    }
  }
  return ArmBaseRelativePatcher::ReserveSpaceEnd(offset);
}

uint32_t Arm64RelativePatcher::WriteThunks(OutputStream* out, uint32_t offset) {
  if (fix_cortex_a53_843419_) {
    if (!current_method_thunks_.empty()) {
      uint32_t aligned_offset = CompiledMethod::AlignCode(offset, kArm64);
      if (kIsDebugBuild) {
        CHECK_ALIGNED(current_method_thunks_.size(), kAdrpThunkSize);
        size_t num_thunks = current_method_thunks_.size() / kAdrpThunkSize;
        CHECK_LE(num_thunks, processed_adrp_thunks_);
        for (size_t i = 0u; i != num_thunks; ++i) {
          const auto& entry = adrp_thunk_locations_[processed_adrp_thunks_ - num_thunks + i];
          CHECK_EQ(entry.second, aligned_offset + i * kAdrpThunkSize);
        }
      }
      uint32_t aligned_code_delta = aligned_offset - offset;
      if (aligned_code_delta != 0u && !WriteCodeAlignment(out, aligned_code_delta)) {
        return 0u;
      }
      if (!WriteMiscThunk(out, ArrayRef<const uint8_t>(current_method_thunks_))) {
        return 0u;
      }
      offset = aligned_offset + current_method_thunks_.size();
      current_method_thunks_.clear();
    }
  }
  return ArmBaseRelativePatcher::WriteThunks(out, offset);
}

void Arm64RelativePatcher::PatchCall(std::vector<uint8_t>* code,
                                     uint32_t literal_offset,
                                     uint32_t patch_offset, uint32_t
                                     target_offset) {
  DCHECK_LE(literal_offset + 4u, code->size());
  DCHECK_EQ(literal_offset & 3u, 0u);
  DCHECK_EQ(patch_offset & 3u, 0u);
  DCHECK_EQ(target_offset & 3u, 0u);
  uint32_t displacement = CalculateDisplacement(patch_offset, target_offset & ~1u);
  DCHECK_EQ(displacement & 3u, 0u);
  DCHECK((displacement >> 27) == 0u || (displacement >> 27) == 31u);  // 28-bit signed.
  uint32_t insn = (displacement & 0x0fffffffu) >> 2;
  insn |= 0x94000000;  // BL

  // Check that we're just overwriting an existing BL.
  DCHECK_EQ(GetInsn(code, literal_offset) & 0xfc000000u, 0x94000000u);
  // Write the new BL.
  SetInsn(code, literal_offset, insn);
}

void Arm64RelativePatcher::PatchPcRelativeReference(std::vector<uint8_t>* code,
                                                    const LinkerPatch& patch,
                                                    uint32_t patch_offset,
                                                    uint32_t target_offset) {
  DCHECK_EQ(patch_offset & 3u, 0u);
  DCHECK_EQ(target_offset & 3u, 0u);
  uint32_t literal_offset = patch.LiteralOffset();
  uint32_t insn = GetInsn(code, literal_offset);
  uint32_t pc_insn_offset = patch.PcInsnOffset();
  uint32_t disp = target_offset - ((patch_offset - literal_offset + pc_insn_offset) & ~0xfffu);
  bool wide = (insn & 0x40000000) != 0;
  uint32_t shift = wide ? 3u : 2u;
  if (literal_offset == pc_insn_offset) {
    // Check it's an ADRP with imm == 0 (unset).
    DCHECK_EQ((insn & 0xffffffe0u), 0x90000000u)
        << literal_offset << ", " << pc_insn_offset << ", 0x" << std::hex << insn;
    if (fix_cortex_a53_843419_ && processed_adrp_thunks_ != adrp_thunk_locations_.size() &&
        adrp_thunk_locations_[processed_adrp_thunks_].first == patch_offset) {
      DCHECK(NeedsErratum843419Thunk(ArrayRef<const uint8_t>(*code),
                                     literal_offset, patch_offset));
      uint32_t thunk_offset = adrp_thunk_locations_[processed_adrp_thunks_].second;
      uint32_t adrp_disp = target_offset - (thunk_offset & ~0xfffu);
      uint32_t adrp = PatchAdrp(insn, adrp_disp);

      uint32_t out_disp = thunk_offset - patch_offset;
      DCHECK_EQ(out_disp & 3u, 0u);
      DCHECK((out_disp >> 27) == 0u || (out_disp >> 27) == 31u);  // 28-bit signed.
      insn = (out_disp & 0x0fffffffu) >> shift;
      insn |= 0x14000000;  // B <thunk>

      uint32_t back_disp = -out_disp;
      DCHECK_EQ(back_disp & 3u, 0u);
      DCHECK((back_disp >> 27) == 0u || (back_disp >> 27) == 31u);  // 28-bit signed.
      uint32_t b_back = (back_disp & 0x0fffffffu) >> 2;
      b_back |= 0x14000000;  // B <back>
      size_t thunks_code_offset = current_method_thunks_.size();
      current_method_thunks_.resize(thunks_code_offset + kAdrpThunkSize);
      SetInsn(&current_method_thunks_, thunks_code_offset, adrp);
      SetInsn(&current_method_thunks_, thunks_code_offset + 4u, b_back);
      static_assert(kAdrpThunkSize == 2 * 4u, "thunk has 2 instructions");

      processed_adrp_thunks_ += 1u;
    } else {
      insn = PatchAdrp(insn, disp);
    }
    // Write the new ADRP (or B to the erratum 843419 thunk).
    SetInsn(code, literal_offset, insn);
  } else {
    if ((insn & 0xfffffc00) == 0x91000000) {
      // ADD immediate, 64-bit with imm12 == 0 (unset).
      DCHECK(patch.GetType() == LinkerPatch::Type::kStringRelative) << patch.GetType();
      shift = 0u;  // No shift for ADD.
    } else {
      // LDR 32-bit or 64-bit with imm12 == 0 (unset).
      DCHECK(patch.GetType() == LinkerPatch::Type::kDexCacheArray) << patch.GetType();
      DCHECK_EQ(insn & 0xbffffc00, 0xb9400000) << std::hex << insn;
    }
    if (kIsDebugBuild) {
      uint32_t adrp = GetInsn(code, pc_insn_offset);
      if ((adrp & 0x9f000000u) != 0x90000000u) {
        CHECK(fix_cortex_a53_843419_);
        CHECK_EQ(adrp & 0xfc000000u, 0x14000000u);  // B <thunk>
        CHECK_ALIGNED(current_method_thunks_.size(), kAdrpThunkSize);
        size_t num_thunks = current_method_thunks_.size() / kAdrpThunkSize;
        CHECK_LE(num_thunks, processed_adrp_thunks_);
        uint32_t b_offset = patch_offset - literal_offset + pc_insn_offset;
        for (size_t i = processed_adrp_thunks_ - num_thunks; ; ++i) {
          CHECK_NE(i, processed_adrp_thunks_);
          if (adrp_thunk_locations_[i].first == b_offset) {
            size_t idx = num_thunks - (processed_adrp_thunks_ - i);
            adrp = GetInsn(&current_method_thunks_, idx * kAdrpThunkSize);
            break;
          }
        }
      }
      CHECK_EQ(adrp & 0x9f00001fu,                    // Check that pc_insn_offset points
               0x90000000 | ((insn >> 5) & 0x1fu));   // to ADRP with matching register.
    }
    uint32_t imm12 = (disp & 0xfffu) >> shift;
    insn = (insn & ~(0xfffu << 10)) | (imm12 << 10);
    SetInsn(code, literal_offset, insn);
  }
}

std::vector<uint8_t> Arm64RelativePatcher::CompileThunkCode() {
  // The thunk just uses the entry point in the ArtMethod. This works even for calls
  // to the generic JNI and interpreter trampolines.
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  arm64::Arm64Assembler assembler(&arena);
  Offset offset(ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kArm64PointerSize).Int32Value());
  assembler.JumpTo(ManagedRegister(arm64::X0), offset, ManagedRegister(arm64::IP0));
  // Ensure we emit the literal pool.
  assembler.FinalizeCode();
  std::vector<uint8_t> thunk_code(assembler.CodeSize());
  MemoryRegion code(thunk_code.data(), thunk_code.size());
  assembler.FinalizeInstructions(code);
  return thunk_code;
}

uint32_t Arm64RelativePatcher::PatchAdrp(uint32_t adrp, uint32_t disp) {
  return (adrp & 0x9f00001fu) |  // Clear offset bits, keep ADRP with destination reg.
      // Bottom 12 bits are ignored, the next 2 lowest bits are encoded in bits 29-30.
      ((disp & 0x00003000u) << (29 - 12)) |
      // The next 16 bits are encoded in bits 5-22.
      ((disp & 0xffffc000u) >> (12 + 2 - 5)) |
      // Since the target_offset is based on the beginning of the oat file and the
      // image space precedes the oat file, the target_offset into image space will
      // be negative yet passed as uint32_t. Therefore we limit the displacement
      // to +-2GiB (rather than the maximim +-4GiB) and determine the sign bit from
      // the highest bit of the displacement. This is encoded in bit 23.
      ((disp & 0x80000000u) >> (31 - 23));
}

bool Arm64RelativePatcher::NeedsErratum843419Thunk(ArrayRef<const uint8_t> code,
                                                   uint32_t literal_offset,
                                                   uint32_t patch_offset) {
  DCHECK_EQ(patch_offset & 0x3u, 0u);
  if ((patch_offset & 0xff8) == 0xff8) {  // ...ff8 or ...ffc
    uint32_t adrp = GetInsn(code, literal_offset);
    DCHECK_EQ(adrp & 0x9f000000, 0x90000000);
    uint32_t next_offset = patch_offset + 4u;
    uint32_t next_insn = GetInsn(code, literal_offset + 4u);

    // Below we avoid patching sequences where the adrp is followed by a load which can easily
    // be proved to be aligned.

    // First check if the next insn is the LDR using the result of the ADRP.
    // LDR <Wt>, [<Xn>, #pimm], where <Xn> == ADRP destination reg.
    if ((next_insn & 0xffc00000) == 0xb9400000 &&
        (((next_insn >> 5) ^ adrp) & 0x1f) == 0) {
      return false;
    }

    // And since LinkerPatch::Type::kStringRelative is using the result of the ADRP
    // for an ADD immediate, check for that as well. We generalize a bit to include
    // ADD/ADDS/SUB/SUBS immediate that either uses the ADRP destination or stores
    // the result to a different register.
    if ((next_insn & 0x1f000000) == 0x11000000 &&
        ((((next_insn >> 5) ^ adrp) & 0x1f) == 0 || ((next_insn ^ adrp) & 0x1f) != 0)) {
      return false;
    }

    // LDR <Wt>, <label> is always aligned and thus it doesn't cause boundary crossing.
    if ((next_insn & 0xff000000) == 0x18000000) {
      return false;
    }

    // LDR <Xt>, <label> is aligned iff the pc + displacement is a multiple of 8.
    if ((next_insn & 0xff000000) == 0x58000000) {
      bool is_aligned_load = (((next_offset >> 2) ^ (next_insn >> 5)) & 1) == 0;
      return !is_aligned_load;
    }

    // LDR <Wt>, [SP, #<pimm>] and LDR <Xt>, [SP, #<pimm>] are always aligned loads, as SP is
    // guaranteed to be 128-bits aligned and <pimm> is multiple of the load size.
    if ((next_insn & 0xbfc003e0) == 0xb94003e0) {
      return false;
    }
    return true;
  }
  return false;
}

void Arm64RelativePatcher::SetInsn(std::vector<uint8_t>* code, uint32_t offset, uint32_t value) {
  DCHECK_LE(offset + 4u, code->size());
  DCHECK_EQ(offset & 3u, 0u);
  uint8_t* addr = &(*code)[offset];
  addr[0] = (value >> 0) & 0xff;
  addr[1] = (value >> 8) & 0xff;
  addr[2] = (value >> 16) & 0xff;
  addr[3] = (value >> 24) & 0xff;
}

uint32_t Arm64RelativePatcher::GetInsn(ArrayRef<const uint8_t> code, uint32_t offset) {
  DCHECK_LE(offset + 4u, code.size());
  DCHECK_EQ(offset & 3u, 0u);
  const uint8_t* addr = &code[offset];
  return
      (static_cast<uint32_t>(addr[0]) << 0) +
      (static_cast<uint32_t>(addr[1]) << 8) +
      (static_cast<uint32_t>(addr[2]) << 16)+
      (static_cast<uint32_t>(addr[3]) << 24);
}

template <typename Alloc>
uint32_t Arm64RelativePatcher::GetInsn(std::vector<uint8_t, Alloc>* code, uint32_t offset) {
  return GetInsn(ArrayRef<const uint8_t>(*code), offset);
}

}  // namespace linker
}  // namespace art
