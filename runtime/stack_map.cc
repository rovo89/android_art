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

#include "indenter.h"
#include "invoke_type.h"

namespace art {

constexpr size_t DexRegisterLocationCatalog::kNoLocationEntryIndex;
constexpr uint32_t StackMap::kNoDexRegisterMap;
constexpr uint32_t StackMap::kNoInlineInfo;

std::ostream& operator<<(std::ostream& stream, const DexRegisterLocation::Kind& kind) {
  using Kind = DexRegisterLocation::Kind;
  switch (kind) {
    case Kind::kNone:
      return stream << "none";
    case Kind::kInStack:
      return stream << "in stack";
    case Kind::kInRegister:
      return stream << "in register";
    case Kind::kInRegisterHigh:
      return stream << "in register high";
    case Kind::kInFpuRegister:
      return stream << "in fpu register";
    case Kind::kInFpuRegisterHigh:
      return stream << "in fpu register high";
    case Kind::kConstant:
      return stream << "as constant";
    case Kind::kInStackLargeOffset:
      return stream << "in stack (large offset)";
    case Kind::kConstantLargeValue:
      return stream << "as constant (large value)";
  }
  return stream << "Kind<" << static_cast<uint32_t>(kind) << ">";
}

DexRegisterLocation::Kind DexRegisterMap::GetLocationInternalKind(
    uint16_t dex_register_number,
    uint16_t number_of_dex_registers,
    const CodeInfo& code_info,
    const StackMapEncoding& enc) const {
  DexRegisterLocationCatalog dex_register_location_catalog =
      code_info.GetDexRegisterLocationCatalog(enc);
  size_t location_catalog_entry_index = GetLocationCatalogEntryIndex(
      dex_register_number,
      number_of_dex_registers,
      code_info.GetNumberOfLocationCatalogEntries());
  return dex_register_location_catalog.GetLocationInternalKind(location_catalog_entry_index);
}

DexRegisterLocation DexRegisterMap::GetDexRegisterLocation(uint16_t dex_register_number,
                                                           uint16_t number_of_dex_registers,
                                                           const CodeInfo& code_info,
                                                           const StackMapEncoding& enc) const {
  DexRegisterLocationCatalog dex_register_location_catalog =
      code_info.GetDexRegisterLocationCatalog(enc);
  size_t location_catalog_entry_index = GetLocationCatalogEntryIndex(
      dex_register_number,
      number_of_dex_registers,
      code_info.GetNumberOfLocationCatalogEntries());
  return dex_register_location_catalog.GetDexRegisterLocation(location_catalog_entry_index);
}

uint32_t StackMap::LoadAt(size_t number_of_bytes, size_t offset, bool check_max) const {
  if (number_of_bytes == 0u) {
    DCHECK(!check_max);
    return 0;
  } else if (number_of_bytes == 1u) {
    uint8_t value = region_.LoadUnaligned<uint8_t>(offset);
    return (check_max && value == 0xFF) ? -1 : value;
  } else if (number_of_bytes == 2u) {
    uint16_t value = region_.LoadUnaligned<uint16_t>(offset);
    return (check_max && value == 0xFFFF) ? -1 : value;
  } else if (number_of_bytes == 3u) {
    uint16_t low = region_.LoadUnaligned<uint16_t>(offset);
    uint16_t high = region_.LoadUnaligned<uint8_t>(offset + sizeof(uint16_t));
    uint32_t value = (high << 16) + low;
    return (check_max && value == 0xFFFFFF) ? -1 : value;
  } else {
    DCHECK_EQ(number_of_bytes, 4u);
    return region_.LoadUnaligned<uint32_t>(offset);
  }
}

void StackMap::StoreAt(size_t number_of_bytes, size_t offset, uint32_t value) const {
  if (number_of_bytes == 0u) {
    DCHECK_EQ(value, 0u);
  } else if (number_of_bytes == 1u) {
    region_.StoreUnaligned<uint8_t>(offset, value);
  } else if (number_of_bytes == 2u) {
    region_.StoreUnaligned<uint16_t>(offset, value);
  } else if (number_of_bytes == 3u) {
    region_.StoreUnaligned<uint16_t>(offset, Low16Bits(value));
    region_.StoreUnaligned<uint8_t>(offset + sizeof(uint16_t), High16Bits(value));
  } else {
    region_.StoreUnaligned<uint32_t>(offset, value);
    DCHECK_EQ(number_of_bytes, 4u);
  }
}

static void DumpRegisterMapping(std::ostream& os,
                                size_t dex_register_num,
                                DexRegisterLocation location,
                                const std::string& prefix = "v",
                                const std::string& suffix = "") {
  os << prefix << dex_register_num << ": "
     << location.GetInternalKind()
     << " (" << location.GetValue() << ")" << suffix << '\n';
}

void CodeInfo::Dump(VariableIndentationOutputStream* vios,
                    uint32_t code_offset,
                    uint16_t number_of_dex_registers,
                    bool dump_stack_maps) const {
  StackMapEncoding encoding = ExtractEncoding();
  uint32_t code_info_size = GetOverallSize();
  size_t number_of_stack_maps = GetNumberOfStackMaps();
  vios->Stream()
      << "Optimized CodeInfo (size=" << code_info_size
      << ", number_of_dex_registers=" << number_of_dex_registers
      << ", number_of_stack_maps=" << number_of_stack_maps
      << ", has_inline_info=" << encoding.HasInlineInfo()
      << ", number_of_bytes_for_inline_info=" << encoding.NumberOfBytesForInlineInfo()
      << ", number_of_bytes_for_dex_register_map=" << encoding.NumberOfBytesForDexRegisterMap()
      << ", number_of_bytes_for_dex_pc=" << encoding.NumberOfBytesForDexPc()
      << ", number_of_bytes_for_native_pc=" << encoding.NumberOfBytesForNativePc()
      << ", number_of_bytes_for_register_mask=" << encoding.NumberOfBytesForRegisterMask()
      << ")\n";
  ScopedIndentation indent1(vios);
  // Display the Dex register location catalog.
  GetDexRegisterLocationCatalog(encoding).Dump(vios, *this);
  // Display stack maps along with (live) Dex register maps.
  if (dump_stack_maps) {
    for (size_t i = 0; i < number_of_stack_maps; ++i) {
      StackMap stack_map = GetStackMapAt(i, encoding);
      stack_map.Dump(vios,
                     *this,
                     encoding,
                     code_offset,
                     number_of_dex_registers,
                     " " + std::to_string(i));
    }
  }
  // TODO: Dump the stack map's inline information? We need to know more from the caller:
  //       we need to know the number of dex registers for each inlined method.
}

void DexRegisterLocationCatalog::Dump(VariableIndentationOutputStream* vios,
                                      const CodeInfo& code_info) {
  StackMapEncoding encoding = code_info.ExtractEncoding();
  size_t number_of_location_catalog_entries = code_info.GetNumberOfLocationCatalogEntries();
  size_t location_catalog_size_in_bytes = code_info.GetDexRegisterLocationCatalogSize(encoding);
  vios->Stream()
      << "DexRegisterLocationCatalog (number_of_entries=" << number_of_location_catalog_entries
      << ", size_in_bytes=" << location_catalog_size_in_bytes << ")\n";
  for (size_t i = 0; i < number_of_location_catalog_entries; ++i) {
    DexRegisterLocation location = GetDexRegisterLocation(i);
    ScopedIndentation indent1(vios);
    DumpRegisterMapping(vios->Stream(), i, location, "entry ");
  }
}

void DexRegisterMap::Dump(VariableIndentationOutputStream* vios,
                          const CodeInfo& code_info,
                          uint16_t number_of_dex_registers) const {
  StackMapEncoding encoding = code_info.ExtractEncoding();
  size_t number_of_location_catalog_entries = code_info.GetNumberOfLocationCatalogEntries();
  // TODO: Display the bit mask of live Dex registers.
  for (size_t j = 0; j < number_of_dex_registers; ++j) {
    if (IsDexRegisterLive(j)) {
      size_t location_catalog_entry_index = GetLocationCatalogEntryIndex(
          j, number_of_dex_registers, number_of_location_catalog_entries);
      DexRegisterLocation location = GetDexRegisterLocation(j,
                                                            number_of_dex_registers,
                                                            code_info,
                                                            encoding);
      ScopedIndentation indent1(vios);
      DumpRegisterMapping(
          vios->Stream(), j, location, "v",
          "\t[entry " + std::to_string(static_cast<int>(location_catalog_entry_index)) + "]");
    }
  }
}

void StackMap::Dump(VariableIndentationOutputStream* vios,
                    const CodeInfo& code_info,
                    const StackMapEncoding& encoding,
                    uint32_t code_offset,
                    uint16_t number_of_dex_registers,
                    const std::string& header_suffix) const {
  vios->Stream()
      << "StackMap" << header_suffix
      << std::hex
      << " [native_pc=0x" << code_offset + GetNativePcOffset(encoding) << "]"
      << " (dex_pc=0x" << GetDexPc(encoding)
      << ", native_pc_offset=0x" << GetNativePcOffset(encoding)
      << ", dex_register_map_offset=0x" << GetDexRegisterMapOffset(encoding)
      << ", inline_info_offset=0x" << GetInlineDescriptorOffset(encoding)
      << ", register_mask=0x" << GetRegisterMask(encoding)
      << std::dec
      << ", stack_mask=0b";
  MemoryRegion stack_mask = GetStackMask(encoding);
  for (size_t i = 0, e = stack_mask.size_in_bits(); i < e; ++i) {
    vios->Stream() << stack_mask.LoadBit(e - i - 1);
  }
  vios->Stream() << ")\n";
  if (HasDexRegisterMap(encoding)) {
    DexRegisterMap dex_register_map = code_info.GetDexRegisterMapOf(
        *this, encoding, number_of_dex_registers);
    dex_register_map.Dump(vios, code_info, number_of_dex_registers);
  }
  if (HasInlineInfo(encoding)) {
    InlineInfo inline_info = code_info.GetInlineInfoOf(*this, encoding);
    // We do not know the length of the dex register maps of inlined frames
    // at this level, so we just pass null to `InlineInfo::Dump` to tell
    // it not to look at these maps.
    inline_info.Dump(vios, code_info, nullptr);
  }
}

void InlineInfo::Dump(VariableIndentationOutputStream* vios,
                      const CodeInfo& code_info,
                      uint16_t number_of_dex_registers[]) const {
  vios->Stream() << "InlineInfo with depth " << static_cast<uint32_t>(GetDepth()) << "\n";

  for (size_t i = 0; i < GetDepth(); ++i) {
    vios->Stream()
        << " At depth " << i
        << std::hex
        << " (dex_pc=0x" << GetDexPcAtDepth(i)
        << std::dec
        << ", method_index=" << GetMethodIndexAtDepth(i)
        << ", invoke_type=" << static_cast<InvokeType>(GetInvokeTypeAtDepth(i))
        << ")\n";
    if (HasDexRegisterMapAtDepth(i) && (number_of_dex_registers != nullptr)) {
      StackMapEncoding encoding = code_info.ExtractEncoding();
      DexRegisterMap dex_register_map =
          code_info.GetDexRegisterMapAtDepth(i, *this, encoding, number_of_dex_registers[i]);
      ScopedIndentation indent1(vios);
      dex_register_map.Dump(vios, code_info, number_of_dex_registers[i]);
    }
  }
}

}  // namespace art
