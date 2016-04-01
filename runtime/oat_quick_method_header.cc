/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "oat_quick_method_header.h"

#include "art_method.h"
#include "mapping_table.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {

OatQuickMethodHeader::OatQuickMethodHeader(
    uint32_t mapping_table_offset,
    uint32_t vmap_table_offset,
    uint32_t gc_map_offset,
    uint32_t frame_size_in_bytes,
    uint32_t core_spill_mask,
    uint32_t fp_spill_mask,
    uint32_t code_size)
    : mapping_table_offset_(mapping_table_offset),
      vmap_table_offset_(vmap_table_offset),
      gc_map_offset_(gc_map_offset),
      frame_info_(frame_size_in_bytes, core_spill_mask, fp_spill_mask),
      code_size_(code_size) {}

OatQuickMethodHeader::~OatQuickMethodHeader() {}

uint32_t OatQuickMethodHeader::ToDexPc(ArtMethod* method,
                                       const uintptr_t pc,
                                       bool abort_on_failure) const {
  const void* entry_point = GetEntryPoint();
  uint32_t sought_offset = pc - reinterpret_cast<uintptr_t>(entry_point);
  if (IsOptimized()) {
    CodeInfo code_info = GetOptimizedCodeInfo();
    CodeInfoEncoding encoding = code_info.ExtractEncoding();
    StackMap stack_map = code_info.GetStackMapForNativePcOffset(sought_offset, encoding);
    if (stack_map.IsValid()) {
      return stack_map.GetDexPc(encoding.stack_map_encoding);
    }
  } else {
    MappingTable table(GetMappingTable());
    // NOTE: Special methods (see Mir2Lir::GenSpecialCase()) have an empty mapping
    // but they have no suspend checks and, consequently, we never call ToDexPc() for them.
    if (table.TotalSize() == 0) {
      DCHECK(method->IsNative());
      return DexFile::kDexNoIndex;
    }

    // Assume the caller wants a pc-to-dex mapping so check here first.
    typedef MappingTable::PcToDexIterator It;
    for (It cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
      if (cur.NativePcOffset() == sought_offset) {
        return cur.DexPc();
      }
    }
    // Now check dex-to-pc mappings.
    typedef MappingTable::DexToPcIterator It2;
    for (It2 cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
      if (cur.NativePcOffset() == sought_offset) {
        return cur.DexPc();
      }
    }
  }
  if (abort_on_failure) {
    ScopedObjectAccess soa(Thread::Current());
    LOG(FATAL) << "Failed to find Dex offset for PC offset "
           << reinterpret_cast<void*>(sought_offset)
           << "(PC " << reinterpret_cast<void*>(pc) << ", entry_point=" << entry_point
           << " current entry_point=" << method->GetEntryPointFromQuickCompiledCode()
           << ") in " << PrettyMethod(method);
  }
  return DexFile::kDexNoIndex;
}

uintptr_t OatQuickMethodHeader::ToNativeQuickPc(ArtMethod* method,
                                                const uint32_t dex_pc,
                                                bool is_for_catch_handler,
                                                bool abort_on_failure) const {
  const void* entry_point = GetEntryPoint();
  if (IsOptimized()) {
    // Optimized code does not have a mapping table. Search for the dex-to-pc
    // mapping in stack maps.
    CodeInfo code_info = GetOptimizedCodeInfo();
    CodeInfoEncoding encoding = code_info.ExtractEncoding();

    // All stack maps are stored in the same CodeItem section, safepoint stack
    // maps first, then catch stack maps. We use `is_for_catch_handler` to select
    // the order of iteration.
    StackMap stack_map =
        LIKELY(is_for_catch_handler) ? code_info.GetCatchStackMapForDexPc(dex_pc, encoding)
                                     : code_info.GetStackMapForDexPc(dex_pc, encoding);
    if (stack_map.IsValid()) {
      return reinterpret_cast<uintptr_t>(entry_point) +
             stack_map.GetNativePcOffset(encoding.stack_map_encoding);
    }
  } else {
    MappingTable table(GetMappingTable());
    if (table.TotalSize() == 0) {
      DCHECK_EQ(dex_pc, 0U);
      return 0;   // Special no mapping/pc == 0 case
    }
    // Assume the caller wants a dex-to-pc mapping so check here first.
    typedef MappingTable::DexToPcIterator It;
    for (It cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
      if (cur.DexPc() == dex_pc) {
        return reinterpret_cast<uintptr_t>(entry_point) + cur.NativePcOffset();
      }
    }
    // Now check pc-to-dex mappings.
    typedef MappingTable::PcToDexIterator It2;
    for (It2 cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
      if (cur.DexPc() == dex_pc) {
        return reinterpret_cast<uintptr_t>(entry_point) + cur.NativePcOffset();
      }
    }
  }

  if (abort_on_failure) {
    ScopedObjectAccess soa(Thread::Current());
    LOG(FATAL) << "Failed to find native offset for dex pc 0x" << std::hex << dex_pc
               << " in " << PrettyMethod(method);
  }
  return UINTPTR_MAX;
}

}  // namespace art
