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

#ifndef ART_COMPILER_DEX_QUICK_RESOURCE_MASK_H_
#define ART_COMPILER_DEX_QUICK_RESOURCE_MASK_H_

#include <stdint.h>

#include "base/logging.h"
#include "dex/reg_storage.h"

namespace art {

class ArenaAllocator;

/**
 * @brief Resource mask for LIR insn uses or defs.
 * @detail Def/Use mask used for checking dependencies between LIR insns in local
 * optimizations such as load hoisting.
 */
class ResourceMask {
 private:
  constexpr ResourceMask(uint64_t mask1, uint64_t mask2)
      : masks_{ mask1, mask2 } {  // NOLINT
  }

 public:
  /*
   * Def/Use encoding in 128-bit use_mask/def_mask.  Low positions used for target-specific
   * registers (and typically use the register number as the position).  High positions
   * reserved for common and abstract resources.
   */
  enum ResourceBit {
    kMustNotAlias = 127,
    kHeapRef = 126,         // Default memory reference type.
    kLiteral = 125,         // Literal pool memory reference.
    kDalvikReg = 124,       // Dalvik v_reg memory reference.
    kFPStatus = 123,
    kCCode = 122,
    kLowestCommonResource = kCCode,
    kHighestCommonResource = kMustNotAlias
  };

  // Default-constructible.
  constexpr ResourceMask()
    : masks_ { 0u, 0u } {
  }

  // Copy-constructible and copyable.
  ResourceMask(const ResourceMask& other) = default;
  ResourceMask& operator=(const ResourceMask& other) = default;

  // Comparable by content.
  bool operator==(const ResourceMask& other) {
    return masks_[0] == other.masks_[0] && masks_[1] == other.masks_[1];
  }

  static constexpr ResourceMask RawMask(uint64_t mask1, uint64_t mask2) {
    return ResourceMask(mask1, mask2);
  }

  static constexpr ResourceMask Bit(size_t bit) {
    return ResourceMask(bit >= 64u ? 0u : UINT64_C(1) << bit,
                        bit >= 64u ? UINT64_C(1) << (bit - 64u) : 0u);
  }

  // Two consecutive bits. The start_bit must be even.
  static constexpr ResourceMask TwoBits(size_t start_bit) {
    return
        DCHECK_CONSTEXPR((start_bit & 1u) == 0u, << start_bit << " isn't even", Bit(0))
        ResourceMask(start_bit >= 64u ? 0u : UINT64_C(3) << start_bit,
                     start_bit >= 64u ? UINT64_C(3) << (start_bit - 64u) : 0u);
  }

  static constexpr ResourceMask NoBits() {
    return ResourceMask(UINT64_C(0), UINT64_C(0));
  }

  static constexpr ResourceMask AllBits() {
    return ResourceMask(~UINT64_C(0), ~UINT64_C(0));
  }

  constexpr ResourceMask Union(const ResourceMask& other) const {
    return ResourceMask(masks_[0] | other.masks_[0], masks_[1] | other.masks_[1]);
  }

  constexpr ResourceMask Intersection(const ResourceMask& other) const {
    return ResourceMask(masks_[0] & other.masks_[0], masks_[1] & other.masks_[1]);
  }

  constexpr ResourceMask Without(const ResourceMask& other) const {
    return ResourceMask(masks_[0] & ~other.masks_[0], masks_[1] & ~other.masks_[1]);
  }

  constexpr bool Equals(const ResourceMask& other) const {
    return masks_[0] == other.masks_[0] && masks_[1] == other.masks_[1];
  }

  constexpr bool Intersects(const ResourceMask& other) const {
    return (masks_[0] & other.masks_[0]) != 0u || (masks_[1] & other.masks_[1]) != 0u;
  }

  void SetBit(size_t bit) {
    DCHECK_LE(bit, kHighestCommonResource);
    masks_[bit / 64u] |= UINT64_C(1) << (bit & 63u);
  }

  constexpr bool HasBit(size_t bit) const {
    return (masks_[bit / 64u] & (UINT64_C(1) << (bit & 63u))) != 0u;
  }

  ResourceMask& SetBits(const ResourceMask& other) {
    masks_[0] |= other.masks_[0];
    masks_[1] |= other.masks_[1];
    return *this;
  }

  ResourceMask& ClearBits(const ResourceMask& other) {
    masks_[0] &= ~other.masks_[0];
    masks_[1] &= ~other.masks_[1];
    return *this;
  }

 private:
  uint64_t masks_[2];

  friend class ResourceMaskCache;
};

constexpr ResourceMask kEncodeNone = ResourceMask::NoBits();
constexpr ResourceMask kEncodeAll = ResourceMask::AllBits();
constexpr ResourceMask kEncodeHeapRef = ResourceMask::Bit(ResourceMask::kHeapRef);
constexpr ResourceMask kEncodeLiteral = ResourceMask::Bit(ResourceMask::kLiteral);
constexpr ResourceMask kEncodeDalvikReg = ResourceMask::Bit(ResourceMask::kDalvikReg);
constexpr ResourceMask kEncodeMem = kEncodeLiteral.Union(kEncodeDalvikReg).Union(
    kEncodeHeapRef).Union(ResourceMask::Bit(ResourceMask::kMustNotAlias));

class ResourceMaskCache {
 public:
  explicit ResourceMaskCache(ArenaAllocator* allocator)
      : allocator_(allocator) {
  }

  const ResourceMask* GetMask(const ResourceMask& mask);

 private:
  ArenaAllocator* allocator_;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_RESOURCE_MASK_H_
