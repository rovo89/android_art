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

#include <string>

#include "base/unix_file/fd_file.h"
#include "elf_file_impl.h"

namespace art {

// Used for compile time and runtime for ElfFile access. Because of
// the need for use at runtime, cannot directly use LLVM classes such as
// ELFObjectFile.
class ElfFile {
 public:
  static ElfFile* Open(File* file, bool writable, bool program_header_only, std::string* error_msg);
  // Open with specific mmap flags, Always maps in the whole file, not just the
  // program header sections.
  static ElfFile* Open(File* file, int mmap_prot, int mmap_flags, std::string* error_msg);
  ~ElfFile();

  const bool is_elf64_;

  // Load segments into memory based on PT_LOAD program headers
  bool Load(bool executable, std::string* error_msg);

  const uint8_t* FindDynamicSymbolAddress(const std::string& symbol_name) const;

  size_t Size() const;

  uint8_t* Begin() const;

  uint8_t* End() const;

  const File& GetFile() const;

  bool GetSectionOffsetAndSize(const char* section_name, uint64_t* offset, uint64_t* size);

  uint64_t FindSymbolAddress(unsigned section_type,
                             const std::string& symbol_name,
                             bool build_map);

  size_t GetLoadedSize() const;

  // Strip an ELF file of unneeded debugging information.
  // Returns true on success, false on failure.
  static bool Strip(File* file, std::string* error_msg);

  // Fixup an ELF file so that that oat header will be loaded at oat_begin.
  // Returns true on success, false on failure.
  static bool Fixup(File* file, uintptr_t oat_data_begin);

  bool Fixup(uintptr_t base_address);

  ElfFileImpl32* GetImpl32() const;
  ElfFileImpl64* GetImpl64() const;

 private:
  explicit ElfFile(ElfFileImpl32* elf32);
  explicit ElfFile(ElfFileImpl64* elf64);

  union ElfFileContainer {
    ElfFileImpl32* elf32_;
    ElfFileImpl64* elf64_;
  } elf_;
};

}  // namespace art

#endif  // ART_RUNTIME_ELF_FILE_H_
