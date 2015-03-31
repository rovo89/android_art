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

#include <stdint.h>

namespace art {

constexpr size_t DexRegisterLocationCatalog::kNoLocationEntryIndex;
constexpr uint32_t StackMap::kNoDexRegisterMap;
constexpr uint32_t StackMap::kNoInlineInfo;

DexRegisterLocation::Kind DexRegisterMap::GetLocationInternalKind(uint16_t dex_register_number,
                                                                  uint16_t number_of_dex_registers,
                                                                  const CodeInfo& code_info) const {
  DexRegisterLocationCatalog dex_register_location_catalog =
      code_info.GetDexRegisterLocationCatalog();
  size_t location_catalog_entry_index = GetLocationCatalogEntryIndex(
      dex_register_number,
      number_of_dex_registers,
      code_info.GetNumberOfDexRegisterLocationCatalogEntries());
  return dex_register_location_catalog.GetLocationInternalKind(location_catalog_entry_index);
}

DexRegisterLocation DexRegisterMap::GetDexRegisterLocation(uint16_t dex_register_number,
                                                           uint16_t number_of_dex_registers,
                                                           const CodeInfo& code_info) const {
  DexRegisterLocationCatalog dex_register_location_catalog =
      code_info.GetDexRegisterLocationCatalog();
  size_t location_catalog_entry_index = GetLocationCatalogEntryIndex(
      dex_register_number,
      number_of_dex_registers,
      code_info.GetNumberOfDexRegisterLocationCatalogEntries());
  return dex_register_location_catalog.GetDexRegisterLocation(location_catalog_entry_index);
}

// Loads `number_of_bytes` at the given `offset` and assemble a uint32_t. If `check_max` is true,
// this method converts a maximum value of size `number_of_bytes` into a uint32_t 0xFFFFFFFF.
static uint32_t LoadAt(MemoryRegion region,
                       size_t number_of_bytes,
                       size_t offset,
                       bool check_max = false) {
  if (number_of_bytes == 0u) {
    DCHECK(!check_max);
    return 0;
  } else if (number_of_bytes == 1u) {
    uint8_t value = region.LoadUnaligned<uint8_t>(offset);
    if (check_max && value == 0xFF) {
      return -1;
    } else {
      return value;
    }
  } else if (number_of_bytes == 2u) {
    uint16_t value = region.LoadUnaligned<uint16_t>(offset);
    if (check_max && value == 0xFFFF) {
      return -1;
    } else {
      return value;
    }
  } else if (number_of_bytes == 3u) {
    uint16_t low = region.LoadUnaligned<uint16_t>(offset);
    uint16_t high = region.LoadUnaligned<uint8_t>(offset + sizeof(uint16_t));
    uint32_t value = (high << 16) + low;
    if (check_max && value == 0xFFFFFF) {
      return -1;
    } else {
      return value;
    }
  } else {
    DCHECK_EQ(number_of_bytes, 4u);
    return region.LoadUnaligned<uint32_t>(offset);
  }
}

static void StoreAt(MemoryRegion region, size_t number_of_bytes, size_t offset, uint32_t value) {
  if (number_of_bytes == 0u) {
    DCHECK_EQ(value, 0u);
  } else if (number_of_bytes == 1u) {
    region.StoreUnaligned<uint8_t>(offset, value);
  } else if (number_of_bytes == 2u) {
    region.StoreUnaligned<uint16_t>(offset, value);
  } else if (number_of_bytes == 3u) {
    region.StoreUnaligned<uint16_t>(offset, Low16Bits(value));
    region.StoreUnaligned<uint8_t>(offset + sizeof(uint16_t), High16Bits(value));
  } else {
    region.StoreUnaligned<uint32_t>(offset, value);
    DCHECK_EQ(number_of_bytes, 4u);
  }
}

uint32_t StackMap::GetDexPc(const CodeInfo& info) const {
  return LoadAt(region_, info.NumberOfBytesForDexPc(), info.ComputeStackMapDexPcOffset());
}

void StackMap::SetDexPc(const CodeInfo& info, uint32_t dex_pc) {
  StoreAt(region_, info.NumberOfBytesForDexPc(), info.ComputeStackMapDexPcOffset(), dex_pc);
}

uint32_t StackMap::GetNativePcOffset(const CodeInfo& info) const {
  return LoadAt(region_, info.NumberOfBytesForNativePc(), info.ComputeStackMapNativePcOffset());
}

void StackMap::SetNativePcOffset(const CodeInfo& info, uint32_t native_pc_offset) {
  StoreAt(region_, info.NumberOfBytesForNativePc(), info.ComputeStackMapNativePcOffset(), native_pc_offset);
}

uint32_t StackMap::GetDexRegisterMapOffset(const CodeInfo& info) const {
  return LoadAt(region_,
                info.NumberOfBytesForDexRegisterMap(),
                info.ComputeStackMapDexRegisterMapOffset(),
                /* check_max */ true);
}

void StackMap::SetDexRegisterMapOffset(const CodeInfo& info, uint32_t offset) {
  StoreAt(region_,
          info.NumberOfBytesForDexRegisterMap(),
          info.ComputeStackMapDexRegisterMapOffset(),
          offset);
}

uint32_t StackMap::GetInlineDescriptorOffset(const CodeInfo& info) const {
  if (!info.HasInlineInfo()) return kNoInlineInfo;
  return LoadAt(region_,
                info.NumberOfBytesForInlineInfo(),
                info.ComputeStackMapInlineInfoOffset(),
                /* check_max */ true);
}

void StackMap::SetInlineDescriptorOffset(const CodeInfo& info, uint32_t offset) {
  DCHECK(info.HasInlineInfo());
  StoreAt(region_,
          info.NumberOfBytesForInlineInfo(),
          info.ComputeStackMapInlineInfoOffset(),
          offset);
}

uint32_t StackMap::GetRegisterMask(const CodeInfo& info) const {
  return LoadAt(region_,
                info.NumberOfBytesForRegisterMask(),
                info.ComputeStackMapRegisterMaskOffset());
}

void StackMap::SetRegisterMask(const CodeInfo& info, uint32_t mask) {
  StoreAt(region_,
          info.NumberOfBytesForRegisterMask(),
          info.ComputeStackMapRegisterMaskOffset(),
          mask);
}

size_t StackMap::ComputeStackMapSizeInternal(size_t stack_mask_size,
                                             size_t number_of_bytes_for_inline_info,
                                             size_t number_of_bytes_for_dex_map,
                                             size_t number_of_bytes_for_dex_pc,
                                             size_t number_of_bytes_for_native_pc,
                                             size_t number_of_bytes_for_register_mask) {
  return stack_mask_size
      + number_of_bytes_for_inline_info
      + number_of_bytes_for_dex_map
      + number_of_bytes_for_dex_pc
      + number_of_bytes_for_native_pc
      + number_of_bytes_for_register_mask;
}

size_t StackMap::ComputeStackMapSize(size_t stack_mask_size,
                                     size_t inline_info_size,
                                     size_t dex_register_map_size,
                                     size_t dex_pc_max,
                                     size_t native_pc_max,
                                     size_t register_mask_max) {
  return ComputeStackMapSizeInternal(
      stack_mask_size,
      inline_info_size == 0
          ? 0
            // + 1 to also encode kNoInlineInfo.
          :  CodeInfo::EncodingSizeInBytes(inline_info_size + dex_register_map_size + 1),
      // + 1 to also encode kNoDexRegisterMap.
      CodeInfo::EncodingSizeInBytes(dex_register_map_size + 1),
      CodeInfo::EncodingSizeInBytes(dex_pc_max),
      CodeInfo::EncodingSizeInBytes(native_pc_max),
      CodeInfo::EncodingSizeInBytes(register_mask_max));
}

MemoryRegion StackMap::GetStackMask(const CodeInfo& info) const {
  return region_.Subregion(info.ComputeStackMapStackMaskOffset(), info.GetStackMaskSize());
}

static void DumpRegisterMapping(std::ostream& os,
                                size_t dex_register_num,
                                DexRegisterLocation location,
                                const std::string& prefix = "v",
                                const std::string& suffix = "") {
  os << "      " << prefix << dex_register_num << ": "
     << DexRegisterLocation::PrettyDescriptor(location.GetInternalKind())
     << " (" << location.GetValue() << ")" << suffix << '\n';
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
     << ", number_of_bytes_for_inline_info=" << NumberOfBytesForInlineInfo()
     << ", number_of_bytes_for_dex_register_map=" << NumberOfBytesForDexRegisterMap()
     << ", number_of_bytes_for_dex_pc=" << NumberOfBytesForDexPc()
     << ", number_of_bytes_for_native_pc=" << NumberOfBytesForNativePc()
     << ", number_of_bytes_for_register_mask=" << NumberOfBytesForRegisterMask()
     << ")\n";

  // Display the Dex register location catalog.
  size_t number_of_location_catalog_entries = GetNumberOfDexRegisterLocationCatalogEntries();
  size_t location_catalog_size_in_bytes = GetDexRegisterLocationCatalogSize();
  os << "  DexRegisterLocationCatalog (number_of_entries=" << number_of_location_catalog_entries
     << ", size_in_bytes=" << location_catalog_size_in_bytes << ")\n";
  DexRegisterLocationCatalog dex_register_location_catalog = GetDexRegisterLocationCatalog();
  for (size_t i = 0; i < number_of_location_catalog_entries; ++i) {
    DexRegisterLocation location = dex_register_location_catalog.GetDexRegisterLocation(i);
    DumpRegisterMapping(os, i, location, "entry ");
  }

  // Display stack maps along with (live) Dex register maps.
  for (size_t i = 0; i < number_of_stack_maps; ++i) {
    StackMap stack_map = GetStackMapAt(i);
    DumpStackMapHeader(os, i);
    if (stack_map.HasDexRegisterMap(*this)) {
      DexRegisterMap dex_register_map = GetDexRegisterMapOf(stack_map, number_of_dex_registers);
      // TODO: Display the bit mask of live Dex registers.
      for (size_t j = 0; j < number_of_dex_registers; ++j) {
        if (dex_register_map.IsDexRegisterLive(j)) {
          size_t location_catalog_entry_index = dex_register_map.GetLocationCatalogEntryIndex(
              j, number_of_dex_registers, number_of_location_catalog_entries);
          DexRegisterLocation location =
              dex_register_map.GetDexRegisterLocation(j, number_of_dex_registers, *this);
          DumpRegisterMapping(
              os, j, location, "v",
              "\t[entry " + std::to_string(static_cast<int>(location_catalog_entry_index)) + "]");
        }
      }
    }
  }
  // TODO: Dump the stack map's inline information.
}

}  // namespace art
