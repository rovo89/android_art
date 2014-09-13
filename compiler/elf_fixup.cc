/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "elf_fixup.h"

#include <inttypes.h>
#include <memory>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "elf_file.h"
#include "elf_writer.h"

namespace art {

static const bool DEBUG_FIXUP = false;

bool ElfFixup::Fixup(File* file, uintptr_t oat_data_begin) {
  std::string error_msg;
  std::unique_ptr<ElfFile> elf_file(ElfFile::Open(file, true, false, &error_msg));
  CHECK(elf_file.get() != nullptr) << error_msg;

  // Lookup "oatdata" symbol address.
  Elf32_Addr oatdata_address = ElfWriter::GetOatDataAddress(elf_file.get());
  Elf32_Off base_address = oat_data_begin - oatdata_address;

  if (!FixupDynamic(*elf_file.get(), base_address)) {
      LOG(WARNING) << "Failed fo fixup .dynamic in " << file->GetPath();
      return false;
  }
  if (!FixupSectionHeaders(*elf_file.get(), base_address)) {
      LOG(WARNING) << "Failed fo fixup section headers in " << file->GetPath();
      return false;
  }
  if (!FixupProgramHeaders(*elf_file.get(), base_address)) {
      LOG(WARNING) << "Failed fo fixup program headers in " << file->GetPath();
      return false;
  }
  if (!FixupSymbols(*elf_file.get(), base_address, true)) {
      LOG(WARNING) << "Failed fo fixup .dynsym in " << file->GetPath();
      return false;
  }
  if (!FixupSymbols(*elf_file.get(), base_address, false)) {
      LOG(WARNING) << "Failed fo fixup .symtab in " << file->GetPath();
      return false;
  }
  if (!FixupRelocations(*elf_file.get(), base_address)) {
      LOG(WARNING) << "Failed fo fixup .rel.dyn in " << file->GetPath();
      return false;
  }
  return true;
}


bool ElfFixup::FixupDynamic(ElfFile& elf_file, uintptr_t base_address) {
  for (Elf32_Word i = 0; i < elf_file.GetDynamicNum(); i++) {
    Elf32_Dyn& elf_dyn = elf_file.GetDynamic(i);
    Elf32_Word d_tag = elf_dyn.d_tag;
    if (IsDynamicSectionPointer(d_tag, elf_file.GetHeader().e_machine)) {
      uint32_t d_ptr = elf_dyn.d_un.d_ptr;
      if (DEBUG_FIXUP) {
        LOG(INFO) << StringPrintf("In %s moving Elf32_Dyn[%d] from 0x%08x to 0x%08" PRIxPTR,
                                  elf_file.GetFile().GetPath().c_str(), i,
                                  d_ptr, d_ptr + base_address);
      }
      d_ptr += base_address;
      elf_dyn.d_un.d_ptr = d_ptr;
    }
  }
  return true;
}

bool ElfFixup::FixupSectionHeaders(ElfFile& elf_file, uintptr_t base_address) {
  for (Elf32_Word i = 0; i < elf_file.GetSectionHeaderNum(); i++) {
    Elf32_Shdr* sh = elf_file.GetSectionHeader(i);
    CHECK(sh != nullptr);
    // 0 implies that the section will not exist in the memory of the process
    if (sh->sh_addr == 0) {
      continue;
    }
    if (DEBUG_FIXUP) {
      LOG(INFO) << StringPrintf("In %s moving Elf32_Shdr[%d] from 0x%08x to 0x%08" PRIxPTR,
                                elf_file.GetFile().GetPath().c_str(), i,
                                sh->sh_addr, sh->sh_addr + base_address);
    }
    sh->sh_addr += base_address;
  }
  return true;
}

bool ElfFixup::FixupProgramHeaders(ElfFile& elf_file, uintptr_t base_address) {
  // TODO: ELFObjectFile doesn't have give to Elf32_Phdr, so we do that ourselves for now.
  for (Elf32_Word i = 0; i < elf_file.GetProgramHeaderNum(); i++) {
    Elf32_Phdr* ph = elf_file.GetProgramHeader(i);
    CHECK(ph != nullptr);
    CHECK_EQ(ph->p_vaddr, ph->p_paddr) << elf_file.GetFile().GetPath() << " i=" << i;
    CHECK((ph->p_align == 0) || (0 == ((ph->p_vaddr - ph->p_offset) & (ph->p_align - 1))))
            << elf_file.GetFile().GetPath() << " i=" << i;
    if (DEBUG_FIXUP) {
      LOG(INFO) << StringPrintf("In %s moving Elf32_Phdr[%d] from 0x%08x to 0x%08" PRIxPTR,
                                elf_file.GetFile().GetPath().c_str(), i,
                                ph->p_vaddr, ph->p_vaddr + base_address);
    }
    ph->p_vaddr += base_address;
    ph->p_paddr += base_address;
    CHECK((ph->p_align == 0) || (0 == ((ph->p_vaddr - ph->p_offset) & (ph->p_align - 1))))
            << elf_file.GetFile().GetPath() << " i=" << i;
  }
  return true;
}

bool ElfFixup::FixupSymbols(ElfFile& elf_file, uintptr_t base_address, bool dynamic) {
  Elf32_Word section_type = dynamic ? SHT_DYNSYM : SHT_SYMTAB;
  // TODO: Unfortunate ELFObjectFile has protected symbol access, so use ElfFile
  Elf32_Shdr* symbol_section = elf_file.FindSectionByType(section_type);
  if (symbol_section == nullptr) {
    // file is missing optional .symtab
    CHECK(!dynamic) << elf_file.GetFile().GetPath();
    return true;
  }
  for (uint32_t i = 0; i < elf_file.GetSymbolNum(*symbol_section); i++) {
    Elf32_Sym* symbol = elf_file.GetSymbol(section_type, i);
    CHECK(symbol != nullptr);
    if (symbol->st_value != 0) {
      if (DEBUG_FIXUP) {
        LOG(INFO) << StringPrintf("In %s moving Elf32_Sym[%d] from 0x%08x to 0x%08" PRIxPTR,
                                  elf_file.GetFile().GetPath().c_str(), i,
                                  symbol->st_value, symbol->st_value + base_address);
      }
      symbol->st_value += base_address;
    }
  }
  return true;
}

bool ElfFixup::FixupRelocations(ElfFile& elf_file, uintptr_t base_address) {
  for (Elf32_Word i = 0; i < elf_file.GetSectionHeaderNum(); i++) {
    Elf32_Shdr* sh = elf_file.GetSectionHeader(i);
    CHECK(sh != nullptr);
    if (sh->sh_type == SHT_REL) {
      for (uint32_t i = 0; i < elf_file.GetRelNum(*sh); i++) {
        Elf32_Rel& rel = elf_file.GetRel(*sh, i);
        if (DEBUG_FIXUP) {
          LOG(INFO) << StringPrintf("In %s moving Elf32_Rel[%d] from 0x%08x to 0x%08" PRIxPTR,
                                    elf_file.GetFile().GetPath().c_str(), i,
                                    rel.r_offset, rel.r_offset + base_address);
        }
        rel.r_offset += base_address;
      }
    } else if (sh->sh_type == SHT_RELA) {
      for (uint32_t i = 0; i < elf_file.GetRelaNum(*sh); i++) {
        Elf32_Rela& rela = elf_file.GetRela(*sh, i);
        if (DEBUG_FIXUP) {
          LOG(INFO) << StringPrintf("In %s moving Elf32_Rela[%d] from 0x%08x to 0x%08" PRIxPTR,
                                    elf_file.GetFile().GetPath().c_str(), i,
                                    rela.r_offset, rela.r_offset + base_address);
        }
        rela.r_offset += base_address;
      }
    }
  }
  return true;
}

}  // namespace art
