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

#ifndef ART_COMPILER_ELF_WRITER_QUICK_H_
#define ART_COMPILER_ELF_WRITER_QUICK_H_

#include "elf_utils.h"
#include "elf_writer.h"

namespace art {

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
class ElfWriterQuick FINAL : public ElfWriter {
 public:
  // Write an ELF file. Returns true on success, false on failure.
  static bool Create(File* file,
                     OatWriter* oat_writer,
                     const std::vector<const DexFile*>& dex_files,
                     const std::string& android_root,
                     bool is_host,
                     const CompilerDriver& driver)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 protected:
  bool Write(OatWriter* oat_writer,
             const std::vector<const DexFile*>& dex_files,
             const std::string& android_root,
             bool is_host)
      OVERRIDE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  ElfWriterQuick(const CompilerDriver& driver, File* elf_file)
    : ElfWriter(driver, elf_file) {}
  ~ElfWriterQuick() {}

  DISALLOW_IMPLICIT_CONSTRUCTORS(ElfWriterQuick);
};

// Explicitly instantiated in elf_writer_quick.cc
typedef ElfWriterQuick<Elf32_Word, Elf32_Sword, Elf32_Addr, Elf32_Dyn,
                       Elf32_Sym, Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr> ElfWriterQuick32;
typedef ElfWriterQuick<Elf64_Word, Elf64_Sword, Elf64_Addr, Elf64_Dyn,
                       Elf64_Sym, Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr> ElfWriterQuick64;

}  // namespace art

#endif  // ART_COMPILER_ELF_WRITER_QUICK_H_
