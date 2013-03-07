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

#ifndef ART_SRC_ELF_WRITER_H_
#define ART_SRC_ELF_WRITER_H_

#include <stdint.h>

#include <cstddef>
#include <vector>

#include <llvm/Support/ELF.h>

#include "UniquePtr.h"
#include "dex_file.h"
#include "os.h"

namespace mcld {
class IRBuilder;
class Input;
class LDSection;
class LDSymbol;
class Linker;
class LinkerConfig;
class Module;
} // namespace mcld

namespace art {

class CompiledCode;
class CompilerDriver;
class ElfFile;

class ElfWriter {
 public:
  // Write an ELF file. Returns true on success, false on failure.
  static bool Create(File* file,
                     std::vector<uint8_t>& oat_contents,
                     const std::vector<const DexFile*>& dex_files,
                     const std::string& android_root,
                     bool is_host,
                     const CompilerDriver& driver)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Fixup an ELF file so that that oat header will be loaded at oat_begin.
  // Returns true on success, false on failure.
  static bool Fixup(File* file, uintptr_t oat_data_begin);

  // Strip an ELF file of unneeded debugging information.
  // Returns true on success, false on failure.
  static bool Strip(File* file);

  // Looks up information about location of oat file in elf file container.
  // Used for ImageWriter to perform memory layout.
  static void GetOatElfInformation(File* file,
                                   size_t& oat_loaded_size,
                                   size_t& oat_data_offset);

 private:
  ElfWriter(const CompilerDriver& driver, File* elf_file);
  ~ElfWriter();

  bool Write(std::vector<uint8_t>& oat_contents,
             const std::vector<const DexFile*>& dex_files,
             const std::string& android_root,
             bool is_host)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Init();
  void AddOatInput(std::vector<uint8_t>& oat_contents);
  void AddMethodInputs(const std::vector<const DexFile*>& dex_files);
  void AddCompiledCodeInput(const CompiledCode& compiled_code);
  void AddRuntimeInputs(const std::string& android_root, bool is_host);
  bool Link();
#if defined(ART_USE_PORTABLE_COMPILER)
  void FixupOatMethodOffsets(const std::vector<const DexFile*>& dex_files)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  uint32_t FixupCompiledCodeOffset(ElfFile& elf_file,
                                   llvm::ELF::Elf32_Addr oatdata_address,
                                   const CompiledCode& compiled_code);
#endif

  // Fixup .dynamic d_ptr values for the expected base_address.
  static bool FixupDynamic(ElfFile& elf_file, uintptr_t base_address);

  // Fixup Elf32_Shdr p_vaddr to load at the desired address.
  static bool FixupSectionHeaders(ElfFile& elf_file,uintptr_t base_address);

  // Fixup Elf32_Phdr p_vaddr to load at the desired address.
  static bool FixupProgramHeaders(ElfFile& elf_file,uintptr_t base_address);

  // Fixup symbol table
  static bool FixupSymbols(ElfFile& elf_file, uintptr_t base_address, bool dynamic);

  // Fixup dynamic relocations
  static bool FixupRelocations(ElfFile& elf_file, uintptr_t base_address);

  // Setup by constructor
  const CompilerDriver* compiler_driver_;
  File* elf_file_;

  // Setup by Init()
  UniquePtr<mcld::LinkerConfig> linker_config_;
  UniquePtr<mcld::Module> module_;
  UniquePtr<mcld::IRBuilder> ir_builder_;
  UniquePtr<mcld::Linker> linker_;

  // Setup by AddOatInput()
  // TODO: ownership of oat_input_?
  mcld::Input* oat_input_;

  // Setup by AddCompiledCodeInput
  // set of symbols for already added mcld::Inputs
  SafeMap<const std::string*, const std::string*> added_symbols_;

  // Setup by FixupCompiledCodeOffset
  // map of symbol names to oatdata offset
  SafeMap<const std::string*, uint32_t> symbol_to_compiled_code_offset_;

};

}  // namespace art

#endif  // ART_SRC_ELF_WRITER_H_
