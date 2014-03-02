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

#ifndef ART_RUNTIME_ELF_FILE_H_
#define ART_RUNTIME_ELF_FILE_H_

#include <map>
#include <vector>

#include "base/unix_file/fd_file.h"
#include "globals.h"
#include "elf_utils.h"
#include "mem_map.h"
#include "os.h"
#include "UniquePtr.h"

namespace art {

// Interface to GDB JIT for backtrace information.
extern "C" {
  struct JITCodeEntry;
}


// Used for compile time and runtime for ElfFile access. Because of
// the need for use at runtime, cannot directly use LLVM classes such as
// ELFObjectFile.
class ElfFile {
 public:
  static ElfFile* Open(File* file, bool writable, bool program_header_only, std::string* error_msg);
  ~ElfFile();

  // Load segments into memory based on PT_LOAD program headers

  const File& GetFile() const {
    return *file_;
  }

  byte* Begin() const {
    return map_->Begin();
  }

  byte* End() const {
    return map_->End();
  }

  size_t Size() const {
    return map_->Size();
  }

  Elf32_Ehdr& GetHeader() const;

  Elf32_Word GetProgramHeaderNum() const;
  Elf32_Phdr& GetProgramHeader(Elf32_Word) const;
  Elf32_Phdr* FindProgamHeaderByType(Elf32_Word type) const;

  Elf32_Word GetSectionHeaderNum() const;
  Elf32_Shdr& GetSectionHeader(Elf32_Word) const;
  Elf32_Shdr* FindSectionByType(Elf32_Word type) const;

  Elf32_Shdr& GetSectionNameStringSection() const;

  // Find .dynsym using .hash for more efficient lookup than FindSymbolAddress.
  const byte* FindDynamicSymbolAddress(const std::string& symbol_name) const;

  static bool IsSymbolSectionType(Elf32_Word section_type);
  Elf32_Word GetSymbolNum(Elf32_Shdr&) const;
  Elf32_Sym& GetSymbol(Elf32_Word section_type, Elf32_Word i) const;

  // Find symbol in specified table, returning NULL if it is not found.
  //
  // If build_map is true, builds a map to speed repeated access. The
  // map does not included untyped symbol values (aka STT_NOTYPE)
  // since they can contain duplicates. If build_map is false, the map
  // will be used if it was already created. Typically build_map
  // should be set unless only a small number of symbols will be
  // looked up.
  Elf32_Sym* FindSymbolByName(Elf32_Word section_type,
                              const std::string& symbol_name,
                              bool build_map);

  // Find address of symbol in specified table, returning 0 if it is
  // not found. See FindSymbolByName for an explanation of build_map.
  Elf32_Addr FindSymbolAddress(Elf32_Word section_type,
                               const std::string& symbol_name,
                               bool build_map);

  // Lookup a string given string section and offset. Returns NULL for
  // special 0 offset.
  const char* GetString(Elf32_Shdr&, Elf32_Word) const;

  // Lookup a string by section type. Returns NULL for special 0 offset.
  const char* GetString(Elf32_Word section_type, Elf32_Word) const;

  Elf32_Word GetDynamicNum() const;
  Elf32_Dyn& GetDynamic(Elf32_Word) const;
  Elf32_Word FindDynamicValueByType(Elf32_Sword type) const;

  Elf32_Word GetRelNum(Elf32_Shdr&) const;
  Elf32_Rel& GetRel(Elf32_Shdr&, Elf32_Word) const;

  Elf32_Word GetRelaNum(Elf32_Shdr&) const;
  Elf32_Rela& GetRela(Elf32_Shdr&, Elf32_Word) const;

  // Returns the expected size when the file is loaded at runtime
  size_t GetLoadedSize() const;

  // Load segments into memory based on PT_LOAD program headers.
  // executable is true at run time, false at compile time.
  bool Load(bool executable, std::string* error_msg);

 private:
  ElfFile(File* file, bool writable, bool program_header_only);

  bool Setup(std::string* error_msg);

  bool SetMap(MemMap* map, std::string* error_msg);

  byte* GetProgramHeadersStart() const;
  byte* GetSectionHeadersStart() const;
  Elf32_Phdr& GetDynamicProgramHeader() const;
  Elf32_Dyn* GetDynamicSectionStart() const;
  Elf32_Sym* GetSymbolSectionStart(Elf32_Word section_type) const;
  const char* GetStringSectionStart(Elf32_Word section_type) const;
  Elf32_Rel* GetRelSectionStart(Elf32_Shdr&) const;
  Elf32_Rela* GetRelaSectionStart(Elf32_Shdr&) const;
  Elf32_Word* GetHashSectionStart() const;
  Elf32_Word GetHashBucketNum() const;
  Elf32_Word GetHashChainNum() const;
  Elf32_Word GetHashBucket(size_t i) const;
  Elf32_Word GetHashChain(size_t i) const;

  typedef std::map<std::string, Elf32_Sym*> SymbolTable;
  SymbolTable** GetSymbolTable(Elf32_Word section_type);

  bool ValidPointer(const byte* start) const;

  const File* const file_;
  const bool writable_;
  const bool program_header_only_;

  // ELF header mapping. If program_header_only_ is false, will
  // actually point to the entire elf file.
  UniquePtr<MemMap> map_;
  Elf32_Ehdr* header_;
  std::vector<MemMap*> segments_;

  // Pointer to start of first PT_LOAD program segment after Load()
  // when program_header_only_ is true.
  byte* base_address_;

  // The program header should always available but use GetProgramHeadersStart() to be sure.
  byte* program_headers_start_;

  // Conditionally available values. Use accessors to ensure they exist if they are required.
  byte* section_headers_start_;
  Elf32_Phdr* dynamic_program_header_;
  Elf32_Dyn* dynamic_section_start_;
  Elf32_Sym* symtab_section_start_;
  Elf32_Sym* dynsym_section_start_;
  char* strtab_section_start_;
  char* dynstr_section_start_;
  Elf32_Word* hash_section_start_;

  SymbolTable* symtab_symbol_table_;
  SymbolTable* dynsym_symbol_table_;

  // Support for GDB JIT
  byte* jit_elf_image_;
  JITCodeEntry* jit_gdb_entry_;
  void GdbJITSupport();
  // Is this an OAT file with debug information in it?
  static constexpr uint32_t kExpectedSectionsInOATFile = 12;
};

}  // namespace art

#endif  // ART_RUNTIME_ELF_FILE_H_
