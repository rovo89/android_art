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

#include "stack_map.h"

namespace art {

constexpr uint32_t StackMap::kNoDexRegisterMapSmallEncoding;
constexpr uint32_t StackMap::kNoInlineInfoSmallEncoding;
constexpr uint32_t StackMap::kNoDexRegisterMap;
constexpr uint32_t StackMap::kNoInlineInfo;

uint32_t StackMap::GetDexPc(const CodeInfo& info) const {
  return info.HasSmallDexPc()
      ? region_.LoadUnaligned<kSmallEncoding>(info.ComputeStackMapDexPcOffset())
      : region_.LoadUnaligned<kLargeEncoding>(info.ComputeStackMapDexPcOffset());
}

void StackMap::SetDexPc(const CodeInfo& info, uint32_t dex_pc) {
  DCHECK(!info.HasSmallDexPc() || IsUint<kBitsForSmallEncoding>(dex_pc)) << dex_pc;
  info.HasSmallDexPc()
      ? region_.StoreUnaligned<kSmallEncoding>(info.ComputeStackMapDexPcOffset(), dex_pc)
      : region_.StoreUnaligned<kLargeEncoding>(info.ComputeStackMapDexPcOffset(), dex_pc);
}

uint32_t StackMap::GetNativePcOffset(const CodeInfo& info) const {
  return info.HasSmallNativePc()
      ? region_.LoadUnaligned<kSmallEncoding>(info.ComputeStackMapNativePcOffset())
      : region_.LoadUnaligned<kLargeEncoding>(info.ComputeStackMapNativePcOffset());
}

void StackMap::SetNativePcOffset(const CodeInfo& info, uint32_t native_pc_offset) {
  DCHECK(!info.HasSmallNativePc()
         || IsUint<kBitsForSmallEncoding>(native_pc_offset)) << native_pc_offset;
  uint32_t entry = info.ComputeStackMapNativePcOffset();
  info.HasSmallNativePc()
      ? region_.StoreUnaligned<kSmallEncoding>(entry, native_pc_offset)
      : region_.StoreUnaligned<kLargeEncoding>(entry, native_pc_offset);
}

uint32_t StackMap::GetDexRegisterMapOffset(const CodeInfo& info) const {
  if (info.HasSmallDexRegisterMap()) {
    uint8_t value = region_.LoadUnaligned<kSmallEncoding>(
        info.ComputeStackMapDexRegisterMapOffset());
    if (value == kNoDexRegisterMapSmallEncoding) {
      return kNoDexRegisterMap;
    } else {
      return value;
    }
  } else {
    return region_.LoadUnaligned<kLargeEncoding>(info.ComputeStackMapDexRegisterMapOffset());
  }
}

void StackMap::SetDexRegisterMapOffset(const CodeInfo& info, uint32_t offset) {
  DCHECK(!info.HasSmallDexRegisterMap()
         || (IsUint<kBitsForSmallEncoding>(offset)
             || (offset == kNoDexRegisterMap))) << offset;
  size_t dex_register_map_entry = info.ComputeStackMapDexRegisterMapOffset();
  info.HasSmallDexRegisterMap()
      ? region_.StoreUnaligned<kSmallEncoding>(dex_register_map_entry, offset)
      : region_.StoreUnaligned<kLargeEncoding>(dex_register_map_entry, offset);
}

uint32_t StackMap::GetInlineDescriptorOffset(const CodeInfo& info) const {
  if (!info.HasInlineInfo()) return kNoInlineInfo;
  if (info.HasSmallInlineInfo()) {
    uint8_t value = region_.LoadUnaligned<kSmallEncoding>(
        info.ComputeStackMapInlineInfoOffset());
    if (value == kNoInlineInfoSmallEncoding) {
      return kNoInlineInfo;
    } else {
      return value;
    }
  } else {
    return region_.LoadUnaligned<kLargeEncoding>(info.ComputeStackMapInlineInfoOffset());
  }
}

void StackMap::SetInlineDescriptorOffset(const CodeInfo& info, uint32_t offset) {
  DCHECK(info.HasInlineInfo());
  DCHECK(!info.HasSmallInlineInfo()
         || (IsUint<kBitsForSmallEncoding>(offset)
             || (offset == kNoInlineInfo))) << offset;
  size_t inline_entry = info.ComputeStackMapInlineInfoOffset();
  info.HasSmallInlineInfo()
      ? region_.StoreUnaligned<kSmallEncoding>(inline_entry, offset)
      : region_.StoreUnaligned<kLargeEncoding>(inline_entry, offset);
}

uint32_t StackMap::GetRegisterMask(const CodeInfo& info) const {
  return region_.LoadUnaligned<kLargeEncoding>(info.ComputeStackMapRegisterMaskOffset());
}

void StackMap::SetRegisterMask(const CodeInfo& info, uint32_t mask) {
  region_.StoreUnaligned<kLargeEncoding>(info.ComputeStackMapRegisterMaskOffset(), mask);
}

size_t StackMap::ComputeStackMapSize(size_t stack_mask_size,
                                     bool has_inline_info,
                                     bool is_small_inline_info,
                                     bool is_small_dex_map,
                                     bool is_small_dex_pc,
                                     bool is_small_native_pc) {
  return StackMap::kFixedSize
      + stack_mask_size
      + (has_inline_info ? NumberOfBytesForEntry(is_small_inline_info) : 0)
      + NumberOfBytesForEntry(is_small_dex_map)
      + NumberOfBytesForEntry(is_small_dex_pc)
      + NumberOfBytesForEntry(is_small_native_pc);
}

size_t StackMap::ComputeStackMapSize(size_t stack_mask_size,
                                     size_t inline_info_size,
                                     size_t dex_register_map_size,
                                     size_t dex_pc_max,
                                     size_t native_pc_max) {
  return ComputeStackMapSize(
      stack_mask_size,
      inline_info_size != 0,
      // + 1 to also encode kNoInlineInfo.
      IsUint<kBitsForSmallEncoding>(inline_info_size + dex_register_map_size + 1),
      // + 1 to also encode kNoDexRegisterMap.
      IsUint<kBitsForSmallEncoding>(dex_register_map_size + 1),
      IsUint<kBitsForSmallEncoding>(dex_pc_max),
      IsUint<kBitsForSmallEncoding>(native_pc_max));
}

MemoryRegion StackMap::GetStackMask(const CodeInfo& info) const {
  return region_.Subregion(info.ComputeStackMapStackMaskOffset(), info.GetStackMaskSize());
}

void CodeInfo::DumpStackMapHeader(std::ostream& os, size_t stack_map_num) const {
  StackMap stack_map = GetStackMapAt(stack_map_num);
  os << "    StackMap " << stack_map_num
     << std::hex
     << " (dex_pc=0x" << stack_map.GetDexPc(*this)
     << ", native_pc_offset=0x" << stack_map.GetNativePcOffset(*this)
     << ", dex_register_map_offset=0x" << stack_map.GetDexRegisterMapOffset(*this)
     << ", inline_info_offset=0x" << stack_map.GetInlineDescriptorOffset(*this)
     << ", register_mask=0x" << stack_map.GetRegisterMask(*this)
     << std::dec
     << ", stack_mask=0b";
  MemoryRegion stack_mask = stack_map.GetStackMask(*this);
  for (size_t i = 0, e = stack_mask.size_in_bits(); i < e; ++i) {
    os << stack_mask.LoadBit(e - i - 1);
  }
  os << ")\n";
};

void CodeInfo::Dump(std::ostream& os, uint16_t number_of_dex_registers) const {
  uint32_t code_info_size = GetOverallSize();
  size_t number_of_stack_maps = GetNumberOfStackMaps();
  os << "  Optimized CodeInfo (size=" << code_info_size
     << ", number_of_dex_registers=" << number_of_dex_registers
     << ", number_of_stack_maps=" << number_of_stack_maps
     << ", has_inline_info=" << HasInlineInfo()
     << ", has_small_inline_info=" << HasSmallInlineInfo()
     << ", has_small_dex_register_map=" << HasSmallDexRegisterMap()
     << ", has_small_dex_pc=" << HasSmallDexPc()
     << ", has_small_native_pc=" << HasSmallNativePc()
     << ")\n";

  // Display stack maps along with Dex register maps.
  for (size_t i = 0; i < number_of_stack_maps; ++i) {
    StackMap stack_map = GetStackMapAt(i);
    DumpStackMapHeader(os, i);
    if (stack_map.HasDexRegisterMap(*this)) {
      DexRegisterMap dex_register_map = GetDexRegisterMapOf(stack_map, number_of_dex_registers);
      // TODO: Display the bit mask of live Dex registers.
      for (size_t j = 0; j < number_of_dex_registers; ++j) {
        if (dex_register_map.IsDexRegisterLive(j)) {
          DexRegisterLocation location =
              dex_register_map.GetLocationKindAndValue(j, number_of_dex_registers);
           os << "      " << "v" << j << ": "
              << DexRegisterLocation::PrettyDescriptor(location.GetInternalKind())
              << " (" << location.GetValue() << ")" << '\n';
        }
      }
    }
  }
  // TODO: Dump the stack map's inline information.
}

}  // namespace art
