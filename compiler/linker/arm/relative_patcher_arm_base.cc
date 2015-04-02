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

#include "linker/arm/relative_patcher_arm_base.h"

#include "compiled_method.h"
#include "oat.h"
#include "output_stream.h"

namespace art {
namespace linker {

uint32_t ArmBaseRelativePatcher::ReserveSpace(uint32_t offset,
                                              const CompiledMethod* compiled_method) {
  return ReserveSpaceInternal(offset, compiled_method, 0u);
}

uint32_t ArmBaseRelativePatcher::WriteThunks(OutputStream* out, uint32_t offset) {
  if (current_thunk_to_write_ == thunk_locations_.size()) {
    return offset;
  }
  uint32_t aligned_offset = CompiledMethod::AlignCode(offset, instruction_set_);
  if (UNLIKELY(aligned_offset == thunk_locations_[current_thunk_to_write_])) {
    ++current_thunk_to_write_;
    uint32_t aligned_code_delta = aligned_offset - offset;
    if (aligned_code_delta != 0u && !WriteCodeAlignment(out, aligned_code_delta)) {
      return 0u;
    }
    if (UNLIKELY(!WriteRelCallThunk(out, ArrayRef<const uint8_t>(thunk_code_)))) {
      return 0u;
    }
    uint32_t thunk_end_offset = aligned_offset + thunk_code_.size();
    // Align after writing chunk, see the ReserveSpace() above.
    offset = CompiledMethod::AlignCode(thunk_end_offset, instruction_set_);
    aligned_code_delta = offset - thunk_end_offset;
    if (aligned_code_delta != 0u && !WriteCodeAlignment(out, aligned_code_delta)) {
      return 0u;
    }
  }
  return offset;
}

ArmBaseRelativePatcher::ArmBaseRelativePatcher(RelativePatcherTargetProvider* provider,
                                               InstructionSet instruction_set,
                                               std::vector<uint8_t> thunk_code,
                                               uint32_t max_positive_displacement,
                                               uint32_t max_negative_displacement)
    : provider_(provider), instruction_set_(instruction_set), thunk_code_(thunk_code),
      max_positive_displacement_(max_positive_displacement),
      max_negative_displacement_(max_negative_displacement),
      thunk_locations_(), current_thunk_to_write_(0u), unprocessed_patches_() {
}

uint32_t ArmBaseRelativePatcher::ReserveSpaceInternal(uint32_t offset,
                                                      const CompiledMethod* compiled_method,
                                                      uint32_t max_extra_space) {
  // NOTE: The final thunk can be reserved from InitCodeMethodVisitor::EndClass() while it
  // may be written early by WriteCodeMethodVisitor::VisitMethod() for a deduplicated chunk
  // of code. To avoid any alignment discrepancies for the final chunk, we always align the
  // offset after reserving of writing any chunk.
  if (UNLIKELY(compiled_method == nullptr)) {
    uint32_t aligned_offset = CompiledMethod::AlignCode(offset, instruction_set_);
    bool needs_thunk = ReserveSpaceProcessPatches(aligned_offset);
    if (needs_thunk) {
      thunk_locations_.push_back(aligned_offset);
      offset = CompiledMethod::AlignCode(aligned_offset + thunk_code_.size(), instruction_set_);
    }
    return offset;
  }
  DCHECK(compiled_method->GetQuickCode() != nullptr);
  uint32_t quick_code_size = compiled_method->GetQuickCode()->size();
  uint32_t quick_code_offset = compiled_method->AlignCode(offset) + sizeof(OatQuickMethodHeader);
  uint32_t next_aligned_offset = compiled_method->AlignCode(quick_code_offset + quick_code_size);
  // Adjust for extra space required by the subclass.
  next_aligned_offset = compiled_method->AlignCode(next_aligned_offset + max_extra_space);
  if (!unprocessed_patches_.empty() &&
      next_aligned_offset - unprocessed_patches_.front().second > max_positive_displacement_) {
    bool needs_thunk = ReserveSpaceProcessPatches(next_aligned_offset);
    if (needs_thunk) {
      // A single thunk will cover all pending patches.
      unprocessed_patches_.clear();
      uint32_t thunk_location = compiled_method->AlignCode(offset);
      thunk_locations_.push_back(thunk_location);
      offset = CompiledMethod::AlignCode(thunk_location + thunk_code_.size(), instruction_set_);
    }
  }
  for (const LinkerPatch& patch : compiled_method->GetPatches()) {
    if (patch.Type() == kLinkerPatchCallRelative) {
      unprocessed_patches_.emplace_back(patch.TargetMethod(),
                                        quick_code_offset + patch.LiteralOffset());
    }
  }
  return offset;
}

uint32_t ArmBaseRelativePatcher::CalculateDisplacement(uint32_t patch_offset,
                                                       uint32_t target_offset) {
  // Unsigned arithmetic with its well-defined overflow behavior is just fine here.
  uint32_t displacement = target_offset - patch_offset;
  // NOTE: With unsigned arithmetic we do mean to use && rather than || below.
  if (displacement > max_positive_displacement_ && displacement < -max_negative_displacement_) {
    // Unwritten thunks have higher offsets, check if it's within range.
    DCHECK(current_thunk_to_write_ == thunk_locations_.size() ||
           thunk_locations_[current_thunk_to_write_] > patch_offset);
    if (current_thunk_to_write_ != thunk_locations_.size() &&
        thunk_locations_[current_thunk_to_write_] - patch_offset < max_positive_displacement_) {
      displacement = thunk_locations_[current_thunk_to_write_] - patch_offset;
    } else {
      // We must have a previous thunk then.
      DCHECK_NE(current_thunk_to_write_, 0u);
      DCHECK_LT(thunk_locations_[current_thunk_to_write_ - 1], patch_offset);
      displacement = thunk_locations_[current_thunk_to_write_ - 1] - patch_offset;
      DCHECK(displacement >= -max_negative_displacement_);
    }
  }
  return displacement;
}

bool ArmBaseRelativePatcher::ReserveSpaceProcessPatches(uint32_t next_aligned_offset) {
  // Process as many patches as possible, stop only on unresolved targets or calls too far back.
  while (!unprocessed_patches_.empty()) {
    uint32_t patch_offset = unprocessed_patches_.front().second;
    auto result = provider_->FindMethodOffset(unprocessed_patches_.front().first);
    if (!result.first) {
      // If still unresolved, check if we have a thunk within range.
      DCHECK(thunk_locations_.empty() || thunk_locations_.back() <= patch_offset);
      if (thunk_locations_.empty() ||
          patch_offset - thunk_locations_.back() > max_negative_displacement_) {
        return next_aligned_offset - patch_offset > max_positive_displacement_;
      }
    } else if (result.second >= patch_offset) {
      DCHECK_LE(result.second - patch_offset, max_positive_displacement_);
    } else {
      // When calling back, check if we have a thunk that's closer than the actual target.
      uint32_t target_offset =
          (thunk_locations_.empty() || result.second > thunk_locations_.back())
          ? result.second
          : thunk_locations_.back();
      DCHECK_GT(patch_offset, target_offset);
      if (patch_offset - target_offset > max_negative_displacement_) {
        return true;
      }
    }
    unprocessed_patches_.pop_front();
  }
  return false;
}

}  // namespace linker
}  // namespace art
