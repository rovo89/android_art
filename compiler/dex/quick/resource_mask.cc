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

#include <iomanip>

#include "resource_mask.h"

#include "utils/arena_allocator.h"

namespace art {

namespace {  // anonymous namespace

constexpr ResourceMask kNoRegMasks[] = {
    kEncodeNone,
    kEncodeHeapRef,
    kEncodeLiteral,
    kEncodeDalvikReg,
    ResourceMask::Bit(ResourceMask::kFPStatus),
    ResourceMask::Bit(ResourceMask::kCCode),
};
// The 127-bit is the same as CLZ(masks_[1]) for a ResourceMask with only that bit set.
COMPILE_ASSERT(kNoRegMasks[127-ResourceMask::kHeapRef].Equals(
    kEncodeHeapRef), check_kNoRegMasks_heap_ref_index);
COMPILE_ASSERT(kNoRegMasks[127-ResourceMask::kLiteral].Equals(
    kEncodeLiteral), check_kNoRegMasks_literal_index);
COMPILE_ASSERT(kNoRegMasks[127-ResourceMask::kDalvikReg].Equals(
    kEncodeDalvikReg), check_kNoRegMasks_dalvik_reg_index);
COMPILE_ASSERT(kNoRegMasks[127-ResourceMask::kFPStatus].Equals(
    ResourceMask::Bit(ResourceMask::kFPStatus)), check_kNoRegMasks_fp_status_index);
COMPILE_ASSERT(kNoRegMasks[127-ResourceMask::kCCode].Equals(
    ResourceMask::Bit(ResourceMask::kCCode)), check_kNoRegMasks_ccode_index);

template <size_t special_bit>
constexpr ResourceMask OneRegOneSpecial(size_t reg) {
  return ResourceMask::Bit(reg).Union(ResourceMask::Bit(special_bit));
}

// NOTE: Working around gcc bug https://gcc.gnu.org/bugzilla/show_bug.cgi?id=61484 .
// This should be a two-dimensions array, kSingleRegMasks[][32] and each line should be
// enclosed in an extra { }. However, gcc issues a bogus "error: array must be initialized
// with a brace-enclosed initializer" for that, so we flatten this to a one-dimensional array.
constexpr ResourceMask kSingleRegMasks[] = {
#define DEFINE_LIST_32(fn) \
    fn(0), fn(1), fn(2), fn(3), fn(4), fn(5), fn(6), fn(7),           \
    fn(8), fn(9), fn(10), fn(11), fn(12), fn(13), fn(14), fn(15),     \
    fn(16), fn(17), fn(18), fn(19), fn(20), fn(21), fn(22), fn(23),   \
    fn(24), fn(25), fn(26), fn(27), fn(28), fn(29), fn(30), fn(31)
    // NOTE: Each line is 512B of constant data, 3KiB in total.
    DEFINE_LIST_32(ResourceMask::Bit),
    DEFINE_LIST_32(OneRegOneSpecial<ResourceMask::kHeapRef>),
    DEFINE_LIST_32(OneRegOneSpecial<ResourceMask::kLiteral>),
    DEFINE_LIST_32(OneRegOneSpecial<ResourceMask::kDalvikReg>),
    DEFINE_LIST_32(OneRegOneSpecial<ResourceMask::kFPStatus>),
    DEFINE_LIST_32(OneRegOneSpecial<ResourceMask::kCCode>),
#undef DEFINE_LIST_32
};

constexpr size_t SingleRegMaskIndex(size_t main_index, size_t sub_index) {
  return main_index * 32u + sub_index;
}

// The 127-bit is the same as CLZ(masks_[1]) for a ResourceMask with only that bit set.
COMPILE_ASSERT(kSingleRegMasks[SingleRegMaskIndex(127-ResourceMask::kHeapRef, 0)].Equals(
    OneRegOneSpecial<ResourceMask::kHeapRef>(0)), check_kSingleRegMasks_heap_ref_index);
COMPILE_ASSERT(kSingleRegMasks[SingleRegMaskIndex(127-ResourceMask::kLiteral, 0)].Equals(
    OneRegOneSpecial<ResourceMask::kLiteral>(0)), check_kSingleRegMasks_literal_index);
COMPILE_ASSERT(kSingleRegMasks[SingleRegMaskIndex(127-ResourceMask::kDalvikReg, 0)].Equals(
    OneRegOneSpecial<ResourceMask::kDalvikReg>(0)), check_kSingleRegMasks_dalvik_reg_index);
COMPILE_ASSERT(kSingleRegMasks[SingleRegMaskIndex(127-ResourceMask::kFPStatus, 0)].Equals(
    OneRegOneSpecial<ResourceMask::kFPStatus>(0)), check_kSingleRegMasks_fp_status_index);
COMPILE_ASSERT(kSingleRegMasks[SingleRegMaskIndex(127-ResourceMask::kCCode, 0)].Equals(
    OneRegOneSpecial<ResourceMask::kCCode>(0)), check_kSingleRegMasks_ccode_index);

// NOTE: arraysize(kNoRegMasks) multiplied by 32 due to the gcc bug workaround, see above.
COMPILE_ASSERT(arraysize(kSingleRegMasks) == arraysize(kNoRegMasks) * 32, check_arraysizes);

constexpr ResourceMask kTwoRegsMasks[] = {
#define TWO(a, b) ResourceMask::Bit(a).Union(ResourceMask::Bit(b))
    // NOTE: 16 * 15 / 2 = 120 entries, 16 bytes each, 1920B in total.
    TWO(0, 1),
    TWO(0, 2), TWO(1, 2),
    TWO(0, 3), TWO(1, 3), TWO(2, 3),
    TWO(0, 4), TWO(1, 4), TWO(2, 4), TWO(3, 4),
    TWO(0, 5), TWO(1, 5), TWO(2, 5), TWO(3, 5), TWO(4, 5),
    TWO(0, 6), TWO(1, 6), TWO(2, 6), TWO(3, 6), TWO(4, 6), TWO(5, 6),
    TWO(0, 7), TWO(1, 7), TWO(2, 7), TWO(3, 7), TWO(4, 7), TWO(5, 7), TWO(6, 7),
    TWO(0, 8), TWO(1, 8), TWO(2, 8), TWO(3, 8), TWO(4, 8), TWO(5, 8), TWO(6, 8), TWO(7, 8),
    TWO(0, 9), TWO(1, 9), TWO(2, 9), TWO(3, 9), TWO(4, 9), TWO(5, 9), TWO(6, 9), TWO(7, 9),
        TWO(8, 9),
    TWO(0, 10), TWO(1, 10), TWO(2, 10), TWO(3, 10), TWO(4, 10), TWO(5, 10), TWO(6, 10), TWO(7, 10),
        TWO(8, 10), TWO(9, 10),
    TWO(0, 11), TWO(1, 11), TWO(2, 11), TWO(3, 11), TWO(4, 11), TWO(5, 11), TWO(6, 11), TWO(7, 11),
        TWO(8, 11), TWO(9, 11), TWO(10, 11),
    TWO(0, 12), TWO(1, 12), TWO(2, 12), TWO(3, 12), TWO(4, 12), TWO(5, 12), TWO(6, 12), TWO(7, 12),
        TWO(8, 12), TWO(9, 12), TWO(10, 12), TWO(11, 12),
    TWO(0, 13), TWO(1, 13), TWO(2, 13), TWO(3, 13), TWO(4, 13), TWO(5, 13), TWO(6, 13), TWO(7, 13),
        TWO(8, 13), TWO(9, 13), TWO(10, 13), TWO(11, 13), TWO(12, 13),
    TWO(0, 14), TWO(1, 14), TWO(2, 14), TWO(3, 14), TWO(4, 14), TWO(5, 14), TWO(6, 14), TWO(7, 14),
        TWO(8, 14), TWO(9, 14), TWO(10, 14), TWO(11, 14), TWO(12, 14), TWO(13, 14),
    TWO(0, 15), TWO(1, 15), TWO(2, 15), TWO(3, 15), TWO(4, 15), TWO(5, 15), TWO(6, 15), TWO(7, 15),
        TWO(8, 15), TWO(9, 15), TWO(10, 15), TWO(11, 15), TWO(12, 15), TWO(13, 15), TWO(14, 15),
#undef TWO
};
COMPILE_ASSERT(arraysize(kTwoRegsMasks) ==  16 * 15 / 2, check_arraysize_kTwoRegsMasks);

constexpr size_t TwoRegsIndex(size_t higher, size_t lower) {
  return (higher * (higher - 1)) / 2u + lower;
}

constexpr bool CheckTwoRegsMask(size_t higher, size_t lower) {
  return ResourceMask::Bit(lower).Union(ResourceMask::Bit(higher)).Equals(
      kTwoRegsMasks[TwoRegsIndex(higher, lower)]);
}

constexpr bool CheckTwoRegsMaskLine(size_t line, size_t lower = 0u) {
  return (lower == line) ||
      (CheckTwoRegsMask(line, lower) && CheckTwoRegsMaskLine(line, lower + 1u));
}

constexpr bool CheckTwoRegsMaskTable(size_t lines) {
  return lines == 0 ||
      (CheckTwoRegsMaskLine(lines - 1) && CheckTwoRegsMaskTable(lines - 1u));
}

COMPILE_ASSERT(CheckTwoRegsMaskTable(16), check_two_regs_masks_table);

}  // anonymous namespace

const ResourceMask* ResourceMaskCache::GetMask(const ResourceMask& mask) {
  // Instead of having a deduplication map, we shall just use pre-defined constexpr
  // masks for the common cases. At most one of the these special bits is allowed:
  constexpr ResourceMask kAllowedSpecialBits = ResourceMask::Bit(ResourceMask::kFPStatus)
      .Union(ResourceMask::Bit(ResourceMask::kCCode))
      .Union(kEncodeHeapRef).Union(kEncodeLiteral).Union(kEncodeDalvikReg);
  const ResourceMask* res = nullptr;
  // Limit to low 32 regs and the kAllowedSpecialBits.
  if ((mask.masks_[0] >> 32) == 0u && (mask.masks_[1] & ~kAllowedSpecialBits.masks_[1]) == 0u) {
    // Check if it's only up to two registers.
    uint32_t low_regs = static_cast<uint32_t>(mask.masks_[0]);
    uint32_t low_regs_without_lowest = low_regs & (low_regs - 1u);
    if (low_regs_without_lowest == 0u && IsPowerOfTwo(mask.masks_[1])) {
      // 0 or 1 register, 0 or 1 bit from kAllowedBits. Use a pre-defined mask.
      size_t index = (mask.masks_[1] != 0u) ? CLZ(mask.masks_[1]) : 0u;
      DCHECK_LT(index, arraysize(kNoRegMasks));
      res = (low_regs != 0) ? &kSingleRegMasks[SingleRegMaskIndex(index, CTZ(low_regs))]
                            : &kNoRegMasks[index];
    } else if (IsPowerOfTwo(low_regs_without_lowest) && mask.masks_[1] == 0u) {
      // 2 registers and no other flags. Use predefined mask if higher reg is < 16.
      if (low_regs_without_lowest < (1u << 16)) {
        res = &kTwoRegsMasks[TwoRegsIndex(CTZ(low_regs_without_lowest), CTZ(low_regs))];
      }
    }
  } else if (mask.Equals(kEncodeAll)) {
    res = &kEncodeAll;
  }
  if (res != nullptr) {
    DCHECK(res->Equals(mask))
        << "(" << std::hex << std::setw(16) << mask.masks_[0]
        << ", "<< std::hex << std::setw(16) << mask.masks_[1]
        << ") != (" << std::hex << std::setw(16) << res->masks_[0]
        << ", "<< std::hex << std::setw(16) << res->masks_[1] << ")";
    return res;
  }

  // TODO: Deduplicate. (At least the most common masks.)
  void* mem = allocator_->Alloc(sizeof(ResourceMask), kArenaAllocLIRResourceMask);
  return new (mem) ResourceMask(mask);
}

}  // namespace art
