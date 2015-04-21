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

#include "elf_file.h"

#include <inttypes.h>
#include <sys/types.h>
#include <unistd.h>

#include "arch/instruction_set.h"
#include "base/logging.h"
#include "base/stringprintf.h"
#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "elf_file_impl.h"
#include "elf_utils.h"
#include "leb128.h"
#include "utils.h"

namespace art {

// -------------------------------------------------------------------
// Binary GDB JIT Interface as described in
//   http://sourceware.org/gdb/onlinedocs/gdb/Declarations.html
extern "C" {
  typedef enum {
    JIT_NOACTION = 0,
    JIT_REGISTER_FN,
    JIT_UNREGISTER_FN
  } JITAction;

  struct JITCodeEntry {
    JITCodeEntry* next_;
    JITCodeEntry* prev_;
    const uint8_t *symfile_addr_;
    uint64_t symfile_size_;
  };

  struct JITDescriptor {
    uint32_t version_;
    uint32_t action_flag_;
    JITCodeEntry* relevant_entry_;
    JITCodeEntry* first_entry_;
  };

  // GDB will place breakpoint into this function.
  // To prevent GCC from inlining or removing it we place noinline attribute
  // and inline assembler statement inside.
  void __attribute__((noinline)) __jit_debug_register_code();
  void __attribute__((noinline)) __jit_debug_register_code() {
    __asm__("");
  }

  // GDB will inspect contents of this descriptor.
  // Static initialization is necessary to prevent GDB from seeing
  // uninitialized descriptor.
  JITDescriptor __jit_debug_descriptor = { 1, JIT_NOACTION, nullptr, nullptr };
}


static JITCodeEntry* CreateCodeEntry(const uint8_t *symfile_addr,
                                     uintptr_t symfile_size) {
  JITCodeEntry* entry = new JITCodeEntry;
  entry->symfile_addr_ = symfile_addr;
  entry->symfile_size_ = symfile_size;
  entry->prev_ = nullptr;

  // TODO: Do we need a lock here?
  entry->next_ = __jit_debug_descriptor.first_entry_;
  if (entry->next_ != nullptr) {
    entry->next_->prev_ = entry;
  }
  __jit_debug_descriptor.first_entry_ = entry;
  __jit_debug_descriptor.relevant_entry_ = entry;

  __jit_debug_descriptor.action_flag_ = JIT_REGISTER_FN;
  __jit_debug_register_code();
  return entry;
}


static void UnregisterCodeEntry(JITCodeEntry* entry) {
  // TODO: Do we need a lock here?
  if (entry->prev_ != nullptr) {
    entry->prev_->next_ = entry->next_;
  } else {
    __jit_debug_descriptor.first_entry_ = entry->next_;
  }

  if (entry->next_ != nullptr) {
    entry->next_->prev_ = entry->prev_;
  }

  __jit_debug_descriptor.relevant_entry_ = entry;
  __jit_debug_descriptor.action_flag_ = JIT_UNREGISTER_FN;
  __jit_debug_register_code();
  delete entry;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::ElfFileImpl(File* file, bool writable, bool program_header_only, uint8_t* requested_base)
  : file_(file),
    writable_(writable),
    program_header_only_(program_header_only),
    header_(nullptr),
    base_address_(nullptr),
    program_headers_start_(nullptr),
    section_headers_start_(nullptr),
    dynamic_program_header_(nullptr),
    dynamic_section_start_(nullptr),
    symtab_section_start_(nullptr),
    dynsym_section_start_(nullptr),
    strtab_section_start_(nullptr),
    dynstr_section_start_(nullptr),
    hash_section_start_(nullptr),
    symtab_symbol_table_(nullptr),
    dynsym_symbol_table_(nullptr),
    jit_elf_image_(nullptr),
    jit_gdb_entry_(nullptr),
    requested_base_(requested_base) {
  CHECK(file != nullptr);
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>*
    ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::Open(File* file, bool writable, bool program_header_only,
           std::string* error_msg, uint8_t* requested_base) {
  std::unique_ptr<ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>>
    elf_file(new ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
                 Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
                 (file, writable, program_header_only, requested_base));
  int prot;
  int flags;
  if (writable) {
    prot = PROT_READ | PROT_WRITE;
    flags = MAP_SHARED;
  } else {
    prot = PROT_READ;
    flags = MAP_PRIVATE;
  }
  if (!elf_file->Setup(prot, flags, error_msg)) {
    return nullptr;
  }
  return elf_file.release();
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>*
    ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::Open(File* file, int prot, int flags, std::string* error_msg) {
  std::unique_ptr<ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>>
    elf_file(new ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
                 Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
                 (file, (prot & PROT_WRITE) == PROT_WRITE, /*program_header_only*/false,
                 /*requested_base*/nullptr));
  if (!elf_file->Setup(prot, flags, error_msg)) {
    return nullptr;
  }
  return elf_file.release();
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::Setup(int prot, int flags, std::string* error_msg) {
  int64_t temp_file_length = file_->GetLength();
  if (temp_file_length < 0) {
    errno = -temp_file_length;
    *error_msg = StringPrintf("Failed to get length of file: '%s' fd=%d: %s",
                              file_->GetPath().c_str(), file_->Fd(), strerror(errno));
    return false;
  }
  size_t file_length = static_cast<size_t>(temp_file_length);
  if (file_length < sizeof(Elf_Ehdr)) {
    *error_msg = StringPrintf("File size of %zd bytes not large enough to contain ELF header of "
                              "%zd bytes: '%s'", file_length, sizeof(Elf_Ehdr),
                              file_->GetPath().c_str());
    return false;
  }

  if (program_header_only_) {
    // first just map ELF header to get program header size information
    size_t elf_header_size = sizeof(Elf_Ehdr);
    if (!SetMap(MemMap::MapFile(elf_header_size, prot, flags, file_->Fd(), 0,
                                file_->GetPath().c_str(), error_msg),
                error_msg)) {
      return false;
    }
    // then remap to cover program header
    size_t program_header_size = header_->e_phoff + (header_->e_phentsize * header_->e_phnum);
    if (file_length < program_header_size) {
      *error_msg = StringPrintf("File size of %zd bytes not large enough to contain ELF program "
                                "header of %zd bytes: '%s'", file_length,
                                sizeof(Elf_Ehdr), file_->GetPath().c_str());
      return false;
    }
    if (!SetMap(MemMap::MapFile(program_header_size, prot, flags, file_->Fd(), 0,
                                file_->GetPath().c_str(), error_msg),
                error_msg)) {
      *error_msg = StringPrintf("Failed to map ELF program headers: %s", error_msg->c_str());
      return false;
    }
  } else {
    // otherwise map entire file
    if (!SetMap(MemMap::MapFile(file_->GetLength(), prot, flags, file_->Fd(), 0,
                                file_->GetPath().c_str(), error_msg),
                error_msg)) {
      *error_msg = StringPrintf("Failed to map ELF file: %s", error_msg->c_str());
      return false;
    }
  }

  if (program_header_only_) {
    program_headers_start_ = Begin() + GetHeader().e_phoff;
  } else {
    if (!CheckAndSet(GetHeader().e_phoff, "program headers", &program_headers_start_, error_msg)) {
      return false;
    }

    // Setup section headers.
    if (!CheckAndSet(GetHeader().e_shoff, "section headers", &section_headers_start_, error_msg)) {
      return false;
    }

    // Find shstrtab.
    Elf_Shdr* shstrtab_section_header = GetSectionNameStringSection();
    if (shstrtab_section_header == nullptr) {
      *error_msg = StringPrintf("Failed to find shstrtab section header in ELF file: '%s'",
                                file_->GetPath().c_str());
      return false;
    }

    // Find .dynamic section info from program header
    dynamic_program_header_ = FindProgamHeaderByType(PT_DYNAMIC);
    if (dynamic_program_header_ == nullptr) {
      *error_msg = StringPrintf("Failed to find PT_DYNAMIC program header in ELF file: '%s'",
                                file_->GetPath().c_str());
      return false;
    }

    if (!CheckAndSet(GetDynamicProgramHeader().p_offset, "dynamic section",
                     reinterpret_cast<uint8_t**>(&dynamic_section_start_), error_msg)) {
      return false;
    }

    // Find other sections from section headers
    for (Elf_Word i = 0; i < GetSectionHeaderNum(); i++) {
      Elf_Shdr* section_header = GetSectionHeader(i);
      if (section_header == nullptr) {
        *error_msg = StringPrintf("Failed to find section header for section %d in ELF file: '%s'",
                                  i, file_->GetPath().c_str());
        return false;
      }
      switch (section_header->sh_type) {
        case SHT_SYMTAB: {
          if (!CheckAndSet(section_header->sh_offset, "symtab",
                           reinterpret_cast<uint8_t**>(&symtab_section_start_), error_msg)) {
            return false;
          }
          break;
        }
        case SHT_DYNSYM: {
          if (!CheckAndSet(section_header->sh_offset, "dynsym",
                           reinterpret_cast<uint8_t**>(&dynsym_section_start_), error_msg)) {
            return false;
          }
          break;
        }
        case SHT_STRTAB: {
          // TODO: base these off of sh_link from .symtab and .dynsym above
          if ((section_header->sh_flags & SHF_ALLOC) != 0) {
            // Check that this is named ".dynstr" and ignore otherwise.
            const char* header_name = GetString(*shstrtab_section_header, section_header->sh_name);
            if (strncmp(".dynstr", header_name, 8) == 0) {
              if (!CheckAndSet(section_header->sh_offset, "dynstr",
                               reinterpret_cast<uint8_t**>(&dynstr_section_start_), error_msg)) {
                return false;
              }
            }
          } else {
            // Check that this is named ".strtab" and ignore otherwise.
            const char* header_name = GetString(*shstrtab_section_header, section_header->sh_name);
            if (strncmp(".strtab", header_name, 8) == 0) {
              if (!CheckAndSet(section_header->sh_offset, "strtab",
                               reinterpret_cast<uint8_t**>(&strtab_section_start_), error_msg)) {
                return false;
              }
            }
          }
          break;
        }
        case SHT_DYNAMIC: {
          if (reinterpret_cast<uint8_t*>(dynamic_section_start_) !=
              Begin() + section_header->sh_offset) {
            LOG(WARNING) << "Failed to find matching SHT_DYNAMIC for PT_DYNAMIC in "
                         << file_->GetPath() << ": " << std::hex
                         << reinterpret_cast<void*>(dynamic_section_start_)
                         << " != " << reinterpret_cast<void*>(Begin() + section_header->sh_offset);
            return false;
          }
          break;
        }
        case SHT_HASH: {
          if (!CheckAndSet(section_header->sh_offset, "hash section",
                           reinterpret_cast<uint8_t**>(&hash_section_start_), error_msg)) {
            return false;
          }
          break;
        }
      }
    }

    // Check for the existence of some sections.
    if (!CheckSectionsExist(error_msg)) {
      return false;
    }
  }

  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::~ElfFileImpl() {
  STLDeleteElements(&segments_);
  delete symtab_symbol_table_;
  delete dynsym_symbol_table_;
  delete jit_elf_image_;
  if (jit_gdb_entry_) {
    UnregisterCodeEntry(jit_gdb_entry_);
  }
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::CheckAndSet(Elf32_Off offset, const char* label,
                  uint8_t** target, std::string* error_msg) {
  if (Begin() + offset >= End()) {
    *error_msg = StringPrintf("Offset %d is out of range for %s in ELF file: '%s'", offset, label,
                              file_->GetPath().c_str());
    return false;
  }
  *target = Begin() + offset;
  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::CheckSectionsLinked(const uint8_t* source, const uint8_t* target) const {
  // Only works in whole-program mode, as we need to iterate over the sections.
  // Note that we normally can't search by type, as duplicates are allowed for most section types.
  if (program_header_only_) {
    return true;
  }

  Elf_Shdr* source_section = nullptr;
  Elf_Word target_index = 0;
  bool target_found = false;
  for (Elf_Word i = 0; i < GetSectionHeaderNum(); i++) {
    Elf_Shdr* section_header = GetSectionHeader(i);

    if (Begin() + section_header->sh_offset == source) {
      // Found the source.
      source_section = section_header;
      if (target_index) {
        break;
      }
    } else if (Begin() + section_header->sh_offset == target) {
      target_index = i;
      target_found = true;
      if (source_section != nullptr) {
        break;
      }
    }
  }

  return target_found && source_section != nullptr && source_section->sh_link == target_index;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::CheckSectionsExist(std::string* error_msg) const {
  if (!program_header_only_) {
    // If in full mode, need section headers.
    if (section_headers_start_ == nullptr) {
      *error_msg = StringPrintf("No section headers in ELF file: '%s'", file_->GetPath().c_str());
      return false;
    }
  }

  // This is redundant, but defensive.
  if (dynamic_program_header_ == nullptr) {
    *error_msg = StringPrintf("Failed to find PT_DYNAMIC program header in ELF file: '%s'",
                              file_->GetPath().c_str());
    return false;
  }

  // Need a dynamic section. This is redundant, but defensive.
  if (dynamic_section_start_ == nullptr) {
    *error_msg = StringPrintf("Failed to find dynamic section in ELF file: '%s'",
                              file_->GetPath().c_str());
    return false;
  }

  // Symtab validation. These is not really a hard failure, as we are currently not using the
  // symtab internally, but it's nice to be defensive.
  if (symtab_section_start_ != nullptr) {
    // When there's a symtab, there should be a strtab.
    if (strtab_section_start_ == nullptr) {
      *error_msg = StringPrintf("No strtab for symtab in ELF file: '%s'", file_->GetPath().c_str());
      return false;
    }

    // The symtab should link to the strtab.
    if (!CheckSectionsLinked(reinterpret_cast<const uint8_t*>(symtab_section_start_),
                             reinterpret_cast<const uint8_t*>(strtab_section_start_))) {
      *error_msg = StringPrintf("Symtab is not linked to the strtab in ELF file: '%s'",
                                file_->GetPath().c_str());
      return false;
    }
  }

  // We always need a dynstr & dynsym.
  if (dynstr_section_start_ == nullptr) {
    *error_msg = StringPrintf("No dynstr in ELF file: '%s'", file_->GetPath().c_str());
    return false;
  }
  if (dynsym_section_start_ == nullptr) {
    *error_msg = StringPrintf("No dynsym in ELF file: '%s'", file_->GetPath().c_str());
    return false;
  }

  // Need a hash section for dynamic symbol lookup.
  if (hash_section_start_ == nullptr) {
    *error_msg = StringPrintf("Failed to find hash section in ELF file: '%s'",
                              file_->GetPath().c_str());
    return false;
  }

  // And the hash section should be linking to the dynsym.
  if (!CheckSectionsLinked(reinterpret_cast<const uint8_t*>(hash_section_start_),
                           reinterpret_cast<const uint8_t*>(dynsym_section_start_))) {
    *error_msg = StringPrintf("Hash section is not linked to the dynstr in ELF file: '%s'",
                              file_->GetPath().c_str());
    return false;
  }

  // We'd also like to confirm a shstrtab in program_header_only_ mode (else Open() does this for
  // us). This is usually the last in an oat file, and a good indicator of whether writing was
  // successful (or the process crashed and left garbage).
  if (program_header_only_) {
    // It might not be mapped, but we can compare against the file size.
    int64_t offset = static_cast<int64_t>(GetHeader().e_shoff +
                                          (GetHeader().e_shstrndx * GetHeader().e_shentsize));
    if (offset >= file_->GetLength()) {
      *error_msg = StringPrintf("Shstrtab is not in the mapped ELF file: '%s'",
                                file_->GetPath().c_str());
      return false;
    }
  }

  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::SetMap(MemMap* map, std::string* error_msg) {
  if (map == nullptr) {
    // MemMap::Open should have already set an error.
    DCHECK(!error_msg->empty());
    return false;
  }
  map_.reset(map);
  CHECK(map_.get() != nullptr) << file_->GetPath();
  CHECK(map_->Begin() != nullptr) << file_->GetPath();

  header_ = reinterpret_cast<Elf_Ehdr*>(map_->Begin());
  if ((ELFMAG0 != header_->e_ident[EI_MAG0])
      || (ELFMAG1 != header_->e_ident[EI_MAG1])
      || (ELFMAG2 != header_->e_ident[EI_MAG2])
      || (ELFMAG3 != header_->e_ident[EI_MAG3])) {
    *error_msg = StringPrintf("Failed to find ELF magic value %d %d %d %d in %s, found %d %d %d %d",
                              ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3,
                              file_->GetPath().c_str(),
                              header_->e_ident[EI_MAG0],
                              header_->e_ident[EI_MAG1],
                              header_->e_ident[EI_MAG2],
                              header_->e_ident[EI_MAG3]);
    return false;
  }
  uint8_t elf_class = (sizeof(Elf_Addr) == sizeof(Elf64_Addr)) ? ELFCLASS64 : ELFCLASS32;
  if (elf_class != header_->e_ident[EI_CLASS]) {
    *error_msg = StringPrintf("Failed to find expected EI_CLASS value %d in %s, found %d",
                              elf_class,
                              file_->GetPath().c_str(),
                              header_->e_ident[EI_CLASS]);
    return false;
  }
  if (ELFDATA2LSB != header_->e_ident[EI_DATA]) {
    *error_msg = StringPrintf("Failed to find expected EI_DATA value %d in %s, found %d",
                              ELFDATA2LSB,
                              file_->GetPath().c_str(),
                              header_->e_ident[EI_CLASS]);
    return false;
  }
  if (EV_CURRENT != header_->e_ident[EI_VERSION]) {
    *error_msg = StringPrintf("Failed to find expected EI_VERSION value %d in %s, found %d",
                              EV_CURRENT,
                              file_->GetPath().c_str(),
                              header_->e_ident[EI_CLASS]);
    return false;
  }
  if (ET_DYN != header_->e_type) {
    *error_msg = StringPrintf("Failed to find expected e_type value %d in %s, found %d",
                              ET_DYN,
                              file_->GetPath().c_str(),
                              header_->e_type);
    return false;
  }
  if (EV_CURRENT != header_->e_version) {
    *error_msg = StringPrintf("Failed to find expected e_version value %d in %s, found %d",
                              EV_CURRENT,
                              file_->GetPath().c_str(),
                              header_->e_version);
    return false;
  }
  if (0 != header_->e_entry) {
    *error_msg = StringPrintf("Failed to find expected e_entry value %d in %s, found %d",
                              0,
                              file_->GetPath().c_str(),
                              static_cast<int32_t>(header_->e_entry));
    return false;
  }
  if (0 == header_->e_phoff) {
    *error_msg = StringPrintf("Failed to find non-zero e_phoff value in %s",
                              file_->GetPath().c_str());
    return false;
  }
  if (0 == header_->e_shoff) {
    *error_msg = StringPrintf("Failed to find non-zero e_shoff value in %s",
                              file_->GetPath().c_str());
    return false;
  }
  if (0 == header_->e_ehsize) {
    *error_msg = StringPrintf("Failed to find non-zero e_ehsize value in %s",
                              file_->GetPath().c_str());
    return false;
  }
  if (0 == header_->e_phentsize) {
    *error_msg = StringPrintf("Failed to find non-zero e_phentsize value in %s",
                              file_->GetPath().c_str());
    return false;
  }
  if (0 == header_->e_phnum) {
    *error_msg = StringPrintf("Failed to find non-zero e_phnum value in %s",
                              file_->GetPath().c_str());
    return false;
  }
  if (0 == header_->e_shentsize) {
    *error_msg = StringPrintf("Failed to find non-zero e_shentsize value in %s",
                              file_->GetPath().c_str());
    return false;
  }
  if (0 == header_->e_shnum) {
    *error_msg = StringPrintf("Failed to find non-zero e_shnum value in %s",
                              file_->GetPath().c_str());
    return false;
  }
  if (0 == header_->e_shstrndx) {
    *error_msg = StringPrintf("Failed to find non-zero e_shstrndx value in %s",
                              file_->GetPath().c_str());
    return false;
  }
  if (header_->e_shstrndx >= header_->e_shnum) {
    *error_msg = StringPrintf("Failed to find e_shnum value %d less than %d in %s",
                              header_->e_shstrndx,
                              header_->e_shnum,
                              file_->GetPath().c_str());
    return false;
  }

  if (!program_header_only_) {
    if (header_->e_phoff >= Size()) {
      *error_msg = StringPrintf("Failed to find e_phoff value %" PRIu64 " less than %zd in %s",
                                static_cast<uint64_t>(header_->e_phoff),
                                Size(),
                                file_->GetPath().c_str());
      return false;
    }
    if (header_->e_shoff >= Size()) {
      *error_msg = StringPrintf("Failed to find e_shoff value %" PRIu64 " less than %zd in %s",
                                static_cast<uint64_t>(header_->e_shoff),
                                Size(),
                                file_->GetPath().c_str());
      return false;
    }
  }
  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Ehdr& ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetHeader() const {
  CHECK(header_ != nullptr);  // Header has been checked in SetMap. This is a sanity check.
  return *header_;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
uint8_t* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetProgramHeadersStart() const {
  CHECK(program_headers_start_ != nullptr);  // Header has been set in Setup. This is a sanity
                                             // check.
  return program_headers_start_;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
uint8_t* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetSectionHeadersStart() const {
  CHECK(!program_header_only_);              // Only used in "full" mode.
  CHECK(section_headers_start_ != nullptr);  // Is checked in CheckSectionsExist. Sanity check.
  return section_headers_start_;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Phdr& ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetDynamicProgramHeader() const {
  CHECK(dynamic_program_header_ != nullptr);  // Is checked in CheckSectionsExist. Sanity check.
  return *dynamic_program_header_;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Dyn* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetDynamicSectionStart() const {
  CHECK(dynamic_section_start_ != nullptr);  // Is checked in CheckSectionsExist. Sanity check.
  return dynamic_section_start_;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Sym* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetSymbolSectionStart(Elf_Word section_type) const {
  CHECK(IsSymbolSectionType(section_type)) << file_->GetPath() << " " << section_type;
  switch (section_type) {
    case SHT_SYMTAB: {
      return symtab_section_start_;
      break;
    }
    case SHT_DYNSYM: {
      return dynsym_section_start_;
      break;
    }
    default: {
      LOG(FATAL) << section_type;
      return nullptr;
    }
  }
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
const char* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetStringSectionStart(Elf_Word section_type) const {
  CHECK(IsSymbolSectionType(section_type)) << file_->GetPath() << " " << section_type;
  switch (section_type) {
    case SHT_SYMTAB: {
      return strtab_section_start_;
    }
    case SHT_DYNSYM: {
      return dynstr_section_start_;
    }
    default: {
      LOG(FATAL) << section_type;
      return nullptr;
    }
  }
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
const char* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetString(Elf_Word section_type, Elf_Word i) const {
  CHECK(IsSymbolSectionType(section_type)) << file_->GetPath() << " " << section_type;
  if (i == 0) {
    return nullptr;
  }
  const char* string_section_start = GetStringSectionStart(section_type);
  if (string_section_start == nullptr) {
    return nullptr;
  }
  return string_section_start + i;
}

// WARNING: The following methods do not check for an error condition (non-existent hash section).
//          It is the caller's job to do this.

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetHashSectionStart() const {
  return hash_section_start_;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetHashBucketNum() const {
  return GetHashSectionStart()[0];
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetHashChainNum() const {
  return GetHashSectionStart()[1];
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetHashBucket(size_t i, bool* ok) const {
  if (i >= GetHashBucketNum()) {
    *ok = false;
    return 0;
  }
  *ok = true;
  // 0 is nbucket, 1 is nchain
  return GetHashSectionStart()[2 + i];
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetHashChain(size_t i, bool* ok) const {
  if (i >= GetHashChainNum()) {
    *ok = false;
    return 0;
  }
  *ok = true;
  // 0 is nbucket, 1 is nchain, & chains are after buckets
  return GetHashSectionStart()[2 + GetHashBucketNum() + i];
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetProgramHeaderNum() const {
  return GetHeader().e_phnum;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Phdr* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetProgramHeader(Elf_Word i) const {
  CHECK_LT(i, GetProgramHeaderNum()) << file_->GetPath();  // Sanity check for caller.
  uint8_t* program_header = GetProgramHeadersStart() + (i * GetHeader().e_phentsize);
  if (program_header >= End()) {
    return nullptr;  // Failure condition.
  }
  return reinterpret_cast<Elf_Phdr*>(program_header);
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Phdr* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FindProgamHeaderByType(Elf_Word type) const {
  for (Elf_Word i = 0; i < GetProgramHeaderNum(); i++) {
    Elf_Phdr* program_header = GetProgramHeader(i);
    if (program_header->p_type == type) {
      return program_header;
    }
  }
  return nullptr;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetSectionHeaderNum() const {
  return GetHeader().e_shnum;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Shdr* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetSectionHeader(Elf_Word i) const {
  // Can only access arbitrary sections when we have the whole file, not just program header.
  // Even if we Load(), it doesn't bring in all the sections.
  CHECK(!program_header_only_) << file_->GetPath();
  if (i >= GetSectionHeaderNum()) {
    return nullptr;  // Failure condition.
  }
  uint8_t* section_header = GetSectionHeadersStart() + (i * GetHeader().e_shentsize);
  if (section_header >= End()) {
    return nullptr;  // Failure condition.
  }
  return reinterpret_cast<Elf_Shdr*>(section_header);
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Shdr* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FindSectionByType(Elf_Word type) const {
  // Can only access arbitrary sections when we have the whole file, not just program header.
  // We could change this to switch on known types if they were detected during loading.
  CHECK(!program_header_only_) << file_->GetPath();
  for (Elf_Word i = 0; i < GetSectionHeaderNum(); i++) {
    Elf_Shdr* section_header = GetSectionHeader(i);
    if (section_header->sh_type == type) {
      return section_header;
    }
  }
  return nullptr;
}

// from bionic
static unsigned elfhash(const char *_name) {
  const unsigned char *name = (const unsigned char *) _name;
  unsigned h = 0, g;

  while (*name) {
    h = (h << 4) + *name++;
    g = h & 0xf0000000;
    h ^= g;
    h ^= g >> 24;
  }
  return h;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Shdr* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetSectionNameStringSection() const {
  return GetSectionHeader(GetHeader().e_shstrndx);
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
const uint8_t* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FindDynamicSymbolAddress(const std::string& symbol_name) const {
  // Check that we have a hash section.
  if (GetHashSectionStart() == nullptr) {
    return nullptr;  // Failure condition.
  }
  const Elf_Sym* sym = FindDynamicSymbol(symbol_name);
  if (sym != nullptr) {
    // TODO: we need to change this to calculate base_address_ in ::Open,
    // otherwise it will be wrongly 0 if ::Load has not yet been called.
    return base_address_ + sym->st_value;
  } else {
    return nullptr;
  }
}

// WARNING: Only called from FindDynamicSymbolAddress. Elides check for hash section.
template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
const Elf_Sym* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FindDynamicSymbol(const std::string& symbol_name) const {
  if (GetHashBucketNum() == 0) {
    // No dynamic symbols at all.
    return nullptr;
  }
  Elf_Word hash = elfhash(symbol_name.c_str());
  Elf_Word bucket_index = hash % GetHashBucketNum();
  bool ok;
  Elf_Word symbol_and_chain_index = GetHashBucket(bucket_index, &ok);
  if (!ok) {
    return nullptr;
  }
  while (symbol_and_chain_index != 0 /* STN_UNDEF */) {
    Elf_Sym* symbol = GetSymbol(SHT_DYNSYM, symbol_and_chain_index);
    if (symbol == nullptr) {
      return nullptr;  // Failure condition.
    }
    const char* name = GetString(SHT_DYNSYM, symbol->st_name);
    if (symbol_name == name) {
      return symbol;
    }
    symbol_and_chain_index = GetHashChain(symbol_and_chain_index, &ok);
    if (!ok) {
      return nullptr;
    }
  }
  return nullptr;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::IsSymbolSectionType(Elf_Word section_type) {
  return ((section_type == SHT_SYMTAB) || (section_type == SHT_DYNSYM));
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetSymbolNum(Elf_Shdr& section_header) const {
  CHECK(IsSymbolSectionType(section_header.sh_type))
      << file_->GetPath() << " " << section_header.sh_type;
  CHECK_NE(0U, section_header.sh_entsize) << file_->GetPath();
  return section_header.sh_size / section_header.sh_entsize;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Sym* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetSymbol(Elf_Word section_type,
                Elf_Word i) const {
  Elf_Sym* sym_start = GetSymbolSectionStart(section_type);
  if (sym_start == nullptr) {
    return nullptr;
  }
  return sym_start + i;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
typename ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::SymbolTable** ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetSymbolTable(Elf_Word section_type) {
  CHECK(IsSymbolSectionType(section_type)) << file_->GetPath() << " " << section_type;
  switch (section_type) {
    case SHT_SYMTAB: {
      return &symtab_symbol_table_;
    }
    case SHT_DYNSYM: {
      return &dynsym_symbol_table_;
    }
    default: {
      LOG(FATAL) << section_type;
      return nullptr;
    }
  }
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Sym* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FindSymbolByName(Elf_Word section_type,
                       const std::string& symbol_name,
                       bool build_map) {
  CHECK(!program_header_only_) << file_->GetPath();
  CHECK(IsSymbolSectionType(section_type)) << file_->GetPath() << " " << section_type;

  SymbolTable** symbol_table = GetSymbolTable(section_type);
  if (*symbol_table != nullptr || build_map) {
    if (*symbol_table == nullptr) {
      DCHECK(build_map);
      *symbol_table = new SymbolTable;
      Elf_Shdr* symbol_section = FindSectionByType(section_type);
      if (symbol_section == nullptr) {
        return nullptr;  // Failure condition.
      }
      Elf_Shdr* string_section = GetSectionHeader(symbol_section->sh_link);
      if (string_section == nullptr) {
        return nullptr;  // Failure condition.
      }
      for (uint32_t i = 0; i < GetSymbolNum(*symbol_section); i++) {
        Elf_Sym* symbol = GetSymbol(section_type, i);
        if (symbol == nullptr) {
          return nullptr;  // Failure condition.
        }
        unsigned char type = (sizeof(Elf_Addr) == sizeof(Elf64_Addr))
                             ? ELF64_ST_TYPE(symbol->st_info)
                             : ELF32_ST_TYPE(symbol->st_info);
        if (type == STT_NOTYPE) {
          continue;
        }
        const char* name = GetString(*string_section, symbol->st_name);
        if (name == nullptr) {
          continue;
        }
        std::pair<typename SymbolTable::iterator, bool> result =
            (*symbol_table)->insert(std::make_pair(name, symbol));
        if (!result.second) {
          // If a duplicate, make sure it has the same logical value. Seen on x86.
          if ((symbol->st_value != result.first->second->st_value) ||
              (symbol->st_size != result.first->second->st_size) ||
              (symbol->st_info != result.first->second->st_info) ||
              (symbol->st_other != result.first->second->st_other) ||
              (symbol->st_shndx != result.first->second->st_shndx)) {
            return nullptr;  // Failure condition.
          }
        }
      }
    }
    CHECK(*symbol_table != nullptr);
    typename SymbolTable::const_iterator it = (*symbol_table)->find(symbol_name);
    if (it == (*symbol_table)->end()) {
      return nullptr;
    }
    return it->second;
  }

  // Fall back to linear search
  Elf_Shdr* symbol_section = FindSectionByType(section_type);
  if (symbol_section == nullptr) {
    return nullptr;
  }
  Elf_Shdr* string_section = GetSectionHeader(symbol_section->sh_link);
  if (string_section == nullptr) {
    return nullptr;
  }
  for (uint32_t i = 0; i < GetSymbolNum(*symbol_section); i++) {
    Elf_Sym* symbol = GetSymbol(section_type, i);
    if (symbol == nullptr) {
      return nullptr;  // Failure condition.
    }
    const char* name = GetString(*string_section, symbol->st_name);
    if (name == nullptr) {
      continue;
    }
    if (symbol_name == name) {
      return symbol;
    }
  }
  return nullptr;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Addr ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FindSymbolAddress(Elf_Word section_type,
                        const std::string& symbol_name,
                        bool build_map) {
  Elf_Sym* symbol = FindSymbolByName(section_type, symbol_name, build_map);
  if (symbol == nullptr) {
    return 0;
  }
  return symbol->st_value;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
const char* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetString(Elf_Shdr& string_section, Elf_Word i) const {
  CHECK(!program_header_only_) << file_->GetPath();
  // TODO: remove this static_cast from enum when using -std=gnu++0x
  if (static_cast<Elf_Word>(SHT_STRTAB) != string_section.sh_type) {
    return nullptr;  // Failure condition.
  }
  if (i >= string_section.sh_size) {
    return nullptr;
  }
  if (i == 0) {
    return nullptr;
  }
  uint8_t* strings = Begin() + string_section.sh_offset;
  uint8_t* string = strings + i;
  if (string >= End()) {
    return nullptr;
  }
  return reinterpret_cast<const char*>(string);
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetDynamicNum() const {
  return GetDynamicProgramHeader().p_filesz / sizeof(Elf_Dyn);
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Dyn& ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetDynamic(Elf_Word i) const {
  CHECK_LT(i, GetDynamicNum()) << file_->GetPath();
  return *(GetDynamicSectionStart() + i);
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Dyn* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FindDynamicByType(Elf_Sword type) const {
  for (Elf_Word i = 0; i < GetDynamicNum(); i++) {
    Elf_Dyn* dyn = &GetDynamic(i);
    if (dyn->d_tag == type) {
      return dyn;
    }
  }
  return NULL;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FindDynamicValueByType(Elf_Sword type) const {
  Elf_Dyn* dyn = FindDynamicByType(type);
  if (dyn == NULL) {
    return 0;
  } else {
    return dyn->d_un.d_val;
  }
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Rel* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetRelSectionStart(Elf_Shdr& section_header) const {
  CHECK(SHT_REL == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  return reinterpret_cast<Elf_Rel*>(Begin() + section_header.sh_offset);
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetRelNum(Elf_Shdr& section_header) const {
  CHECK(SHT_REL == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  CHECK_NE(0U, section_header.sh_entsize) << file_->GetPath();
  return section_header.sh_size / section_header.sh_entsize;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Rel& ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetRel(Elf_Shdr& section_header, Elf_Word i) const {
  CHECK(SHT_REL == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  CHECK_LT(i, GetRelNum(section_header)) << file_->GetPath();
  return *(GetRelSectionStart(section_header) + i);
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Rela* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
  Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
  ::GetRelaSectionStart(Elf_Shdr& section_header) const {
  CHECK(SHT_RELA == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  return reinterpret_cast<Elf_Rela*>(Begin() + section_header.sh_offset);
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Word ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetRelaNum(Elf_Shdr& section_header) const {
  CHECK(SHT_RELA == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  return section_header.sh_size / section_header.sh_entsize;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Rela& ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetRela(Elf_Shdr& section_header, Elf_Word i) const {
  CHECK(SHT_RELA == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  CHECK_LT(i, GetRelaNum(section_header)) << file_->GetPath();
  return *(GetRelaSectionStart(section_header) + i);
}

// Base on bionic phdr_table_get_load_size
template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
size_t ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GetLoadedSize() const {
  Elf_Addr min_vaddr = 0xFFFFFFFFu;
  Elf_Addr max_vaddr = 0x00000000u;
  for (Elf_Word i = 0; i < GetProgramHeaderNum(); i++) {
    Elf_Phdr* program_header = GetProgramHeader(i);
    if (program_header->p_type != PT_LOAD) {
      continue;
    }
    Elf_Addr begin_vaddr = program_header->p_vaddr;
    if (begin_vaddr < min_vaddr) {
       min_vaddr = begin_vaddr;
    }
    Elf_Addr end_vaddr = program_header->p_vaddr + program_header->p_memsz;
    if (end_vaddr > max_vaddr) {
      max_vaddr = end_vaddr;
    }
  }
  min_vaddr = RoundDown(min_vaddr, kPageSize);
  max_vaddr = RoundUp(max_vaddr, kPageSize);
  CHECK_LT(min_vaddr, max_vaddr) << file_->GetPath();
  size_t loaded_size = max_vaddr - min_vaddr;
  return loaded_size;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::Load(bool executable, std::string* error_msg) {
  CHECK(program_header_only_) << file_->GetPath();

  if (executable) {
    InstructionSet elf_ISA = GetInstructionSetFromELF(GetHeader().e_machine, GetHeader().e_flags);
    if (elf_ISA != kRuntimeISA) {
      std::ostringstream oss;
      oss << "Expected ISA " << kRuntimeISA << " but found " << elf_ISA;
      *error_msg = oss.str();
      return false;
    }
  }

  bool reserved = false;
  for (Elf_Word i = 0; i < GetProgramHeaderNum(); i++) {
    Elf_Phdr* program_header = GetProgramHeader(i);
    if (program_header == nullptr) {
      *error_msg = StringPrintf("No program header for entry %d in ELF file %s.",
                                i, file_->GetPath().c_str());
      return false;
    }

    // Record .dynamic header information for later use
    if (program_header->p_type == PT_DYNAMIC) {
      dynamic_program_header_ = program_header;
      continue;
    }

    // Not something to load, move on.
    if (program_header->p_type != PT_LOAD) {
      continue;
    }

    // Found something to load.

    // Before load the actual segments, reserve a contiguous chunk
    // of required size and address for all segments, but with no
    // permissions. We'll then carve that up with the proper
    // permissions as we load the actual segments. If p_vaddr is
    // non-zero, the segments require the specific address specified,
    // which either was specified in the file because we already set
    // base_address_ after the first zero segment).
    int64_t temp_file_length = file_->GetLength();
    if (temp_file_length < 0) {
      errno = -temp_file_length;
      *error_msg = StringPrintf("Failed to get length of file: '%s' fd=%d: %s",
                                file_->GetPath().c_str(), file_->Fd(), strerror(errno));
      return false;
    }
    size_t file_length = static_cast<size_t>(temp_file_length);
    if (!reserved) {
      uint8_t* reserve_base = reinterpret_cast<uint8_t*>(program_header->p_vaddr);
      uint8_t* reserve_base_override = reserve_base;
      // Override the base (e.g. when compiling with --compile-pic)
      if (requested_base_ != nullptr) {
        reserve_base_override = requested_base_;
      }
      std::string reservation_name("ElfFile reservation for ");
      reservation_name += file_->GetPath();
      std::unique_ptr<MemMap> reserve(MemMap::MapAnonymous(reservation_name.c_str(),
                                                           reserve_base_override,
                                                           GetLoadedSize(), PROT_NONE, false, false,
                                                           error_msg));
      if (reserve.get() == nullptr) {
        *error_msg = StringPrintf("Failed to allocate %s: %s",
                                  reservation_name.c_str(), error_msg->c_str());
        return false;
      }
      reserved = true;

      // Base address is the difference of actual mapped location and the p_vaddr
      base_address_ = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(reserve->Begin())
                       - reinterpret_cast<uintptr_t>(reserve_base));
      // By adding the p_vaddr of a section/symbol to base_address_ we will always get the
      // dynamic memory address of where that object is actually mapped
      //
      // TODO: base_address_ needs to be calculated in ::Open, otherwise
      // FindDynamicSymbolAddress returns the wrong values until Load is called.
      segments_.push_back(reserve.release());
    }
    // empty segment, nothing to map
    if (program_header->p_memsz == 0) {
      continue;
    }
    uint8_t* p_vaddr = base_address_ + program_header->p_vaddr;
    int prot = 0;
    if (executable && ((program_header->p_flags & PF_X) != 0)) {
      prot |= PROT_EXEC;
    }
    if ((program_header->p_flags & PF_W) != 0) {
      prot |= PROT_WRITE;
    }
    if ((program_header->p_flags & PF_R) != 0) {
      prot |= PROT_READ;
    }
    int flags = 0;
    if (writable_) {
      prot |= PROT_WRITE;
      flags |= MAP_SHARED;
    } else {
      flags |= MAP_PRIVATE;
    }
    if (program_header->p_filesz > program_header->p_memsz) {
      *error_msg = StringPrintf("Invalid p_filesz > p_memsz (%" PRIu64 " > %" PRIu64 "): %s",
                                static_cast<uint64_t>(program_header->p_filesz),
                                static_cast<uint64_t>(program_header->p_memsz),
                                file_->GetPath().c_str());
      return false;
    }
    if (program_header->p_filesz < program_header->p_memsz &&
        !IsAligned<kPageSize>(program_header->p_filesz)) {
      *error_msg = StringPrintf("Unsupported unaligned p_filesz < p_memsz (%" PRIu64
                                " < %" PRIu64 "): %s",
                                static_cast<uint64_t>(program_header->p_filesz),
                                static_cast<uint64_t>(program_header->p_memsz),
                                file_->GetPath().c_str());
      return false;
    }
    if (file_length < (program_header->p_offset + program_header->p_filesz)) {
      *error_msg = StringPrintf("File size of %zd bytes not large enough to contain ELF segment "
                                "%d of %" PRIu64 " bytes: '%s'", file_length, i,
                                static_cast<uint64_t>(program_header->p_offset + program_header->p_filesz),
                                file_->GetPath().c_str());
      return false;
    }
    if (program_header->p_filesz != 0u) {
      std::unique_ptr<MemMap> segment(
          MemMap::MapFileAtAddress(p_vaddr,
                                   program_header->p_filesz,
                                   prot, flags, file_->Fd(),
                                   program_header->p_offset,
                                   true,  // implies MAP_FIXED
                                   file_->GetPath().c_str(),
                                   error_msg));
      if (segment.get() == nullptr) {
        *error_msg = StringPrintf("Failed to map ELF file segment %d from %s: %s",
                                  i, file_->GetPath().c_str(), error_msg->c_str());
        return false;
      }
      if (segment->Begin() != p_vaddr) {
        *error_msg = StringPrintf("Failed to map ELF file segment %d from %s at expected address %p, "
                                  "instead mapped to %p",
                                  i, file_->GetPath().c_str(), p_vaddr, segment->Begin());
        return false;
      }
      segments_.push_back(segment.release());
    }
    if (program_header->p_filesz < program_header->p_memsz) {
      std::string name = StringPrintf("Zero-initialized segment %" PRIu64 " of ELF file %s",
                                      static_cast<uint64_t>(i), file_->GetPath().c_str());
      std::unique_ptr<MemMap> segment(
          MemMap::MapAnonymous(name.c_str(),
                               p_vaddr + program_header->p_filesz,
                               program_header->p_memsz - program_header->p_filesz,
                               prot, false, true /* reuse */, error_msg));
      if (segment == nullptr) {
        *error_msg = StringPrintf("Failed to map zero-initialized ELF file segment %d from %s: %s",
                                  i, file_->GetPath().c_str(), error_msg->c_str());
        return false;
      }
      if (segment->Begin() != p_vaddr) {
        *error_msg = StringPrintf("Failed to map zero-initialized ELF file segment %d from %s "
                                  "at expected address %p, instead mapped to %p",
                                  i, file_->GetPath().c_str(), p_vaddr, segment->Begin());
        return false;
      }
      segments_.push_back(segment.release());
    }
  }

  // Now that we are done loading, .dynamic should be in memory to find .dynstr, .dynsym, .hash
  uint8_t* dsptr = base_address_ + GetDynamicProgramHeader().p_vaddr;
  if ((dsptr < Begin() || dsptr >= End()) && !ValidPointer(dsptr)) {
    *error_msg = StringPrintf("dynamic section address invalid in ELF file %s",
                              file_->GetPath().c_str());
    return false;
  }
  dynamic_section_start_ = reinterpret_cast<Elf_Dyn*>(dsptr);

  for (Elf_Word i = 0; i < GetDynamicNum(); i++) {
    Elf_Dyn& elf_dyn = GetDynamic(i);
    uint8_t* d_ptr = base_address_ + elf_dyn.d_un.d_ptr;
    switch (elf_dyn.d_tag) {
      case DT_HASH: {
        if (!ValidPointer(d_ptr)) {
          *error_msg = StringPrintf("DT_HASH value %p does not refer to a loaded ELF segment of %s",
                                    d_ptr, file_->GetPath().c_str());
          return false;
        }
        hash_section_start_ = reinterpret_cast<Elf_Word*>(d_ptr);
        break;
      }
      case DT_STRTAB: {
        if (!ValidPointer(d_ptr)) {
          *error_msg = StringPrintf("DT_HASH value %p does not refer to a loaded ELF segment of %s",
                                    d_ptr, file_->GetPath().c_str());
          return false;
        }
        dynstr_section_start_ = reinterpret_cast<char*>(d_ptr);
        break;
      }
      case DT_SYMTAB: {
        if (!ValidPointer(d_ptr)) {
          *error_msg = StringPrintf("DT_HASH value %p does not refer to a loaded ELF segment of %s",
                                    d_ptr, file_->GetPath().c_str());
          return false;
        }
        dynsym_section_start_ = reinterpret_cast<Elf_Sym*>(d_ptr);
        break;
      }
      case DT_NULL: {
        if (GetDynamicNum() != i+1) {
          *error_msg = StringPrintf("DT_NULL found after %d .dynamic entries, "
                                    "expected %d as implied by size of PT_DYNAMIC segment in %s",
                                    i + 1, GetDynamicNum(), file_->GetPath().c_str());
          return false;
        }
        break;
      }
    }
  }

  // Check for the existence of some sections.
  if (!CheckSectionsExist(error_msg)) {
    return false;
  }

  // Use GDB JIT support to do stack backtrace, etc.
  if (executable) {
    GdbJITSupport();
  }

  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::ValidPointer(const uint8_t* start) const {
  for (size_t i = 0; i < segments_.size(); ++i) {
    const MemMap* segment = segments_[i];
    if (segment->Begin() <= start && start < segment->End()) {
      return true;
    }
  }
  return false;
}


template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
Elf_Shdr* ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FindSectionByName(const std::string& name) const {
  CHECK(!program_header_only_);
  Elf_Shdr* shstrtab_sec = GetSectionNameStringSection();
  if (shstrtab_sec == nullptr) {
    return nullptr;
  }
  for (uint32_t i = 0; i < GetSectionHeaderNum(); i++) {
    Elf_Shdr* shdr = GetSectionHeader(i);
    if (shdr == nullptr) {
      return nullptr;
    }
    const char* sec_name = GetString(*shstrtab_sec, shdr->sh_name);
    if (sec_name == nullptr) {
      continue;
    }
    if (name == sec_name) {
      return shdr;
    }
  }
  return nullptr;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FixupDebugSections(typename std::make_signed<Elf_Off>::type base_address_delta) {
  const Elf_Shdr* debug_info = FindSectionByName(".debug_info");
  const Elf_Shdr* debug_abbrev = FindSectionByName(".debug_abbrev");
  const Elf_Shdr* debug_str = FindSectionByName(".debug_str");
  const Elf_Shdr* strtab_sec = FindSectionByName(".strtab");
  const Elf_Shdr* symtab_sec = FindSectionByName(".symtab");

  if (debug_info == nullptr || debug_abbrev == nullptr ||
      debug_str == nullptr || strtab_sec == nullptr || symtab_sec == nullptr) {
    // Release version of ART does not generate debug info.
    return true;
  }
  if (base_address_delta == 0) {
    return true;
  }
  if (!ApplyOatPatchesTo(".debug_info", base_address_delta)) {
    return false;
  }
  if (!ApplyOatPatchesTo(".debug_line", base_address_delta)) {
    return false;
  }
  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::ApplyOatPatchesTo(const char* target_section_name,
                        typename std::make_signed<Elf_Off>::type delta) {
  auto patches_section = FindSectionByName(".oat_patches");
  if (patches_section == nullptr) {
    LOG(ERROR) << ".oat_patches section not found.";
    return false;
  }
  if (patches_section->sh_type != SHT_OAT_PATCH) {
    LOG(ERROR) << "Unexpected type of .oat_patches.";
    return false;
  }
  auto target_section = FindSectionByName(target_section_name);
  if (target_section == nullptr) {
    LOG(ERROR) << target_section_name << " section not found.";
    return false;
  }
  if (!ApplyOatPatches(
      Begin() + patches_section->sh_offset,
      Begin() + patches_section->sh_offset + patches_section->sh_size,
      target_section_name, delta,
      Begin() + target_section->sh_offset,
      Begin() + target_section->sh_offset + target_section->sh_size)) {
    LOG(ERROR) << target_section_name << " section not found in .oat_patches.";
  }
  return true;
}

// Apply .oat_patches to given section.
template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::ApplyOatPatches(const uint8_t* patches, const uint8_t* patches_end,
                      const char* target_section_name,
                      typename std::make_signed<Elf_Off>::type delta,
                      uint8_t* to_patch, const uint8_t* to_patch_end) {
  // Read null-terminated section name.
  const char* section_name;
  while ((section_name = reinterpret_cast<const char*>(patches))[0] != '\0') {
    patches += strlen(section_name) + 1;
    uint32_t length = DecodeUnsignedLeb128(&patches);
    const uint8_t* next_section = patches + length;
    // Is it the section we want to patch?
    if (strcmp(section_name, target_section_name) == 0) {
      // Read LEB128 encoded list of advances.
      while (patches < next_section) {
        DCHECK_LT(patches, patches_end) << "Unexpected end of .oat_patches.";
        to_patch += DecodeUnsignedLeb128(&patches);
        DCHECK_LT(to_patch, to_patch_end) << "Patch past the end of " << section_name;
        // TODO: 32-bit vs 64-bit.  What is the right type to use here?
        auto* patch_loc = reinterpret_cast<typename std::make_signed<Elf_Off>::type*>(to_patch);
        *patch_loc += delta;
      }
      return true;
    }
    patches = next_section;
  }
  return false;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
void ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::GdbJITSupport() {
  // We only get here if we only are mapping the program header.
  DCHECK(program_header_only_);

  // Well, we need the whole file to do this.
  std::string error_msg;
  // Make it MAP_PRIVATE so we can just give it to gdb if all the necessary
  // sections are there.
  std::unique_ptr<ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
      Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>>
      all_ptr(Open(const_cast<File*>(file_), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE, &error_msg));
  if (all_ptr.get() == nullptr) {
    return;
  }
  ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
      Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>& all = *all_ptr;

  // We need the eh_frame for gdb but debug info might be present without it.
  const Elf_Shdr* eh_frame = all.FindSectionByName(".eh_frame");
  if (eh_frame == nullptr) {
    return;
  }

  // Do we have interesting sections?
  // We need to add in a strtab and symtab to the image.
  // all is MAP_PRIVATE so it can be written to freely.
  // We also already have strtab and symtab so we are fine there.
  Elf_Ehdr& elf_hdr = all.GetHeader();
  elf_hdr.e_entry = 0;
  elf_hdr.e_phoff = 0;
  elf_hdr.e_phnum = 0;
  elf_hdr.e_phentsize = 0;
  elf_hdr.e_type = ET_EXEC;

  // Since base_address_ is 0 if we are actually loaded at a known address (i.e. this is boot.oat)
  // and the actual address stuff starts at in regular files this is good.
  if (!all.FixupDebugSections(reinterpret_cast<intptr_t>(base_address_))) {
    LOG(ERROR) << "Failed to load GDB data";
    return;
  }

  jit_gdb_entry_ = CreateCodeEntry(all.Begin(), all.Size());
  gdb_file_mapping_.reset(all_ptr.release());
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::Strip(std::string* error_msg) {
  // ELF files produced by MCLinker look roughly like this
  //
  // +------------+
  // | Elf_Ehdr   | contains number of Elf_Shdr and offset to first
  // +------------+
  // | Elf_Phdr   | program headers
  // | Elf_Phdr   |
  // | ...        |
  // | Elf_Phdr   |
  // +------------+
  // | section    | mixture of needed and unneeded sections
  // +------------+
  // | section    |
  // +------------+
  // | ...        |
  // +------------+
  // | section    |
  // +------------+
  // | Elf_Shdr   | section headers
  // | Elf_Shdr   |
  // | ...        | contains offset to section start
  // | Elf_Shdr   |
  // +------------+
  //
  // To strip:
  // - leave the Elf_Ehdr and Elf_Phdr values in place.
  // - walk the sections making a new set of Elf_Shdr section headers for what we want to keep
  // - move the sections are keeping up to fill in gaps of sections we want to strip
  // - write new Elf_Shdr section headers to end of file, updating Elf_Ehdr
  // - truncate rest of file
  //

  std::vector<Elf_Shdr> section_headers;
  std::vector<Elf_Word> section_headers_original_indexes;
  section_headers.reserve(GetSectionHeaderNum());


  Elf_Shdr* string_section = GetSectionNameStringSection();
  CHECK(string_section != nullptr);
  for (Elf_Word i = 0; i < GetSectionHeaderNum(); i++) {
    Elf_Shdr* sh = GetSectionHeader(i);
    CHECK(sh != nullptr);
    const char* name = GetString(*string_section, sh->sh_name);
    if (name == nullptr) {
      CHECK_EQ(0U, i);
      section_headers.push_back(*sh);
      section_headers_original_indexes.push_back(0);
      continue;
    }
    if (StartsWith(name, ".debug")
        || (strcmp(name, ".strtab") == 0)
        || (strcmp(name, ".symtab") == 0)) {
      continue;
    }
    section_headers.push_back(*sh);
    section_headers_original_indexes.push_back(i);
  }
  CHECK_NE(0U, section_headers.size());
  CHECK_EQ(section_headers.size(), section_headers_original_indexes.size());

  // section 0 is the NULL section, sections start at offset of first section
  CHECK(GetSectionHeader(1) != nullptr);
  Elf_Off offset = GetSectionHeader(1)->sh_offset;
  for (size_t i = 1; i < section_headers.size(); i++) {
    Elf_Shdr& new_sh = section_headers[i];
    Elf_Shdr* old_sh = GetSectionHeader(section_headers_original_indexes[i]);
    CHECK(old_sh != nullptr);
    CHECK_EQ(new_sh.sh_name, old_sh->sh_name);
    if (old_sh->sh_addralign > 1) {
      offset = RoundUp(offset, old_sh->sh_addralign);
    }
    if (old_sh->sh_offset == offset) {
      // already in place
      offset += old_sh->sh_size;
      continue;
    }
    // shift section earlier
    memmove(Begin() + offset,
            Begin() + old_sh->sh_offset,
            old_sh->sh_size);
    new_sh.sh_offset = offset;
    offset += old_sh->sh_size;
  }

  Elf_Off shoff = offset;
  size_t section_headers_size_in_bytes = section_headers.size() * sizeof(Elf_Shdr);
  memcpy(Begin() + offset, &section_headers[0], section_headers_size_in_bytes);
  offset += section_headers_size_in_bytes;

  GetHeader().e_shnum = section_headers.size();
  GetHeader().e_shoff = shoff;
  int result = ftruncate(file_->Fd(), offset);
  if (result != 0) {
    *error_msg = StringPrintf("Failed to truncate while stripping ELF file: '%s': %s",
                              file_->GetPath().c_str(), strerror(errno));
    return false;
  }
  return true;
}

static const bool DEBUG_FIXUP = false;

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::Fixup(Elf_Addr base_address) {
  if (!FixupDynamic(base_address)) {
    LOG(WARNING) << "Failed to fixup .dynamic in " << file_->GetPath();
    return false;
  }
  if (!FixupSectionHeaders(base_address)) {
    LOG(WARNING) << "Failed to fixup section headers in " << file_->GetPath();
    return false;
  }
  if (!FixupProgramHeaders(base_address)) {
    LOG(WARNING) << "Failed to fixup program headers in " << file_->GetPath();
    return false;
  }
  if (!FixupSymbols(base_address, true)) {
    LOG(WARNING) << "Failed to fixup .dynsym in " << file_->GetPath();
    return false;
  }
  if (!FixupSymbols(base_address, false)) {
    LOG(WARNING) << "Failed to fixup .symtab in " << file_->GetPath();
    return false;
  }
  if (!FixupRelocations(base_address)) {
    LOG(WARNING) << "Failed to fixup .rel.dyn in " << file_->GetPath();
    return false;
  }
  static_assert(sizeof(Elf_Off) >= sizeof(base_address), "Potentially losing precision.");
  if (!FixupDebugSections(static_cast<Elf_Off>(base_address))) {
    LOG(WARNING) << "Failed to fixup debug sections in " << file_->GetPath();
    return false;
  }
  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FixupDynamic(Elf_Addr base_address) {
  for (Elf_Word i = 0; i < GetDynamicNum(); i++) {
    Elf_Dyn& elf_dyn = GetDynamic(i);
    Elf_Word d_tag = elf_dyn.d_tag;
    if (IsDynamicSectionPointer(d_tag, GetHeader().e_machine)) {
      Elf_Addr d_ptr = elf_dyn.d_un.d_ptr;
      if (DEBUG_FIXUP) {
        LOG(INFO) << StringPrintf("In %s moving Elf_Dyn[%d] from 0x%" PRIx64 " to 0x%" PRIx64,
                                  GetFile().GetPath().c_str(), i,
                                  static_cast<uint64_t>(d_ptr),
                                  static_cast<uint64_t>(d_ptr + base_address));
      }
      d_ptr += base_address;
      elf_dyn.d_un.d_ptr = d_ptr;
    }
  }
  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FixupSectionHeaders(Elf_Addr base_address) {
  for (Elf_Word i = 0; i < GetSectionHeaderNum(); i++) {
    Elf_Shdr* sh = GetSectionHeader(i);
    CHECK(sh != nullptr);
    // 0 implies that the section will not exist in the memory of the process
    if (sh->sh_addr == 0) {
      continue;
    }
    if (DEBUG_FIXUP) {
      LOG(INFO) << StringPrintf("In %s moving Elf_Shdr[%d] from 0x%" PRIx64 " to 0x%" PRIx64,
                                GetFile().GetPath().c_str(), i,
                                static_cast<uint64_t>(sh->sh_addr),
                                static_cast<uint64_t>(sh->sh_addr + base_address));
    }
    sh->sh_addr += base_address;
  }
  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FixupProgramHeaders(Elf_Addr base_address) {
  // TODO: ELFObjectFile doesn't have give to Elf_Phdr, so we do that ourselves for now.
  for (Elf_Word i = 0; i < GetProgramHeaderNum(); i++) {
    Elf_Phdr* ph = GetProgramHeader(i);
    CHECK(ph != nullptr);
    CHECK_EQ(ph->p_vaddr, ph->p_paddr) << GetFile().GetPath() << " i=" << i;
    CHECK((ph->p_align == 0) || (0 == ((ph->p_vaddr - ph->p_offset) & (ph->p_align - 1))))
            << GetFile().GetPath() << " i=" << i;
    if (DEBUG_FIXUP) {
      LOG(INFO) << StringPrintf("In %s moving Elf_Phdr[%d] from 0x%" PRIx64 " to 0x%" PRIx64,
                                GetFile().GetPath().c_str(), i,
                                static_cast<uint64_t>(ph->p_vaddr),
                                static_cast<uint64_t>(ph->p_vaddr + base_address));
    }
    ph->p_vaddr += base_address;
    ph->p_paddr += base_address;
    CHECK((ph->p_align == 0) || (0 == ((ph->p_vaddr - ph->p_offset) & (ph->p_align - 1))))
            << GetFile().GetPath() << " i=" << i;
  }
  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FixupSymbols(Elf_Addr base_address, bool dynamic) {
  Elf_Word section_type = dynamic ? SHT_DYNSYM : SHT_SYMTAB;
  // TODO: Unfortunate ELFObjectFile has protected symbol access, so use ElfFile
  Elf_Shdr* symbol_section = FindSectionByType(section_type);
  if (symbol_section == nullptr) {
    // file is missing optional .symtab
    CHECK(!dynamic) << GetFile().GetPath();
    return true;
  }
  for (uint32_t i = 0; i < GetSymbolNum(*symbol_section); i++) {
    Elf_Sym* symbol = GetSymbol(section_type, i);
    CHECK(symbol != nullptr);
    if (symbol->st_value != 0) {
      if (DEBUG_FIXUP) {
        LOG(INFO) << StringPrintf("In %s moving Elf_Sym[%d] from 0x%" PRIx64 " to 0x%" PRIx64,
                                  GetFile().GetPath().c_str(), i,
                                  static_cast<uint64_t>(symbol->st_value),
                                  static_cast<uint64_t>(symbol->st_value + base_address));
      }
      symbol->st_value += base_address;
    }
  }
  return true;
}

template <typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr, typename Elf_Word,
          typename Elf_Sword, typename Elf_Addr, typename Elf_Sym, typename Elf_Rel,
          typename Elf_Rela, typename Elf_Dyn, typename Elf_Off>
bool ElfFileImpl<Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Word,
    Elf_Sword, Elf_Addr, Elf_Sym, Elf_Rel, Elf_Rela, Elf_Dyn, Elf_Off>
    ::FixupRelocations(Elf_Addr base_address) {
  for (Elf_Word i = 0; i < GetSectionHeaderNum(); i++) {
    Elf_Shdr* sh = GetSectionHeader(i);
    CHECK(sh != nullptr);
    if (sh->sh_type == SHT_REL) {
      for (uint32_t j = 0; j < GetRelNum(*sh); j++) {
        Elf_Rel& rel = GetRel(*sh, j);
        if (DEBUG_FIXUP) {
          LOG(INFO) << StringPrintf("In %s moving Elf_Rel[%d] from 0x%" PRIx64 " to 0x%" PRIx64,
                                    GetFile().GetPath().c_str(), j,
                                    static_cast<uint64_t>(rel.r_offset),
                                    static_cast<uint64_t>(rel.r_offset + base_address));
        }
        rel.r_offset += base_address;
      }
    } else if (sh->sh_type == SHT_RELA) {
      for (uint32_t j = 0; j < GetRelaNum(*sh); j++) {
        Elf_Rela& rela = GetRela(*sh, j);
        if (DEBUG_FIXUP) {
          LOG(INFO) << StringPrintf("In %s moving Elf_Rela[%d] from 0x%" PRIx64 " to 0x%" PRIx64,
                                    GetFile().GetPath().c_str(), j,
                                    static_cast<uint64_t>(rela.r_offset),
                                    static_cast<uint64_t>(rela.r_offset + base_address));
        }
        rela.r_offset += base_address;
      }
    }
  }
  return true;
}

// Explicit instantiations
template class ElfFileImpl<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Word,
    Elf32_Sword, Elf32_Addr, Elf32_Sym, Elf32_Rel, Elf32_Rela, Elf32_Dyn, Elf32_Off>;
template class ElfFileImpl<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Word,
    Elf64_Sword, Elf64_Addr, Elf64_Sym, Elf64_Rel, Elf64_Rela, Elf64_Dyn, Elf64_Off>;

ElfFile::ElfFile(ElfFileImpl32* elf32) : elf32_(elf32), elf64_(nullptr) {
}

ElfFile::ElfFile(ElfFileImpl64* elf64) : elf32_(nullptr), elf64_(elf64) {
}

ElfFile::~ElfFile() {
  // Should never have 32 and 64-bit impls.
  CHECK_NE(elf32_.get() == nullptr, elf64_.get() == nullptr);
}

ElfFile* ElfFile::Open(File* file, bool writable, bool program_header_only, std::string* error_msg,
                       uint8_t* requested_base) {
  if (file->GetLength() < EI_NIDENT) {
    *error_msg = StringPrintf("File %s is too short to be a valid ELF file",
                              file->GetPath().c_str());
    return nullptr;
  }
  std::unique_ptr<MemMap> map(MemMap::MapFile(EI_NIDENT, PROT_READ, MAP_PRIVATE, file->Fd(), 0,
                                              file->GetPath().c_str(), error_msg));
  if (map == nullptr && map->Size() != EI_NIDENT) {
    return nullptr;
  }
  uint8_t* header = map->Begin();
  if (header[EI_CLASS] == ELFCLASS64) {
    ElfFileImpl64* elf_file_impl = ElfFileImpl64::Open(file, writable, program_header_only,
                                                       error_msg, requested_base);
    if (elf_file_impl == nullptr)
      return nullptr;
    return new ElfFile(elf_file_impl);
  } else if (header[EI_CLASS] == ELFCLASS32) {
    ElfFileImpl32* elf_file_impl = ElfFileImpl32::Open(file, writable, program_header_only,
                                                       error_msg, requested_base);
    if (elf_file_impl == nullptr) {
      return nullptr;
    }
    return new ElfFile(elf_file_impl);
  } else {
    *error_msg = StringPrintf("Failed to find expected EI_CLASS value %d or %d in %s, found %d",
                              ELFCLASS32, ELFCLASS64,
                              file->GetPath().c_str(),
                              header[EI_CLASS]);
    return nullptr;
  }
}

ElfFile* ElfFile::Open(File* file, int mmap_prot, int mmap_flags, std::string* error_msg) {
  if (file->GetLength() < EI_NIDENT) {
    *error_msg = StringPrintf("File %s is too short to be a valid ELF file",
                              file->GetPath().c_str());
    return nullptr;
  }
  std::unique_ptr<MemMap> map(MemMap::MapFile(EI_NIDENT, PROT_READ, MAP_PRIVATE, file->Fd(), 0,
                                              file->GetPath().c_str(), error_msg));
  if (map == nullptr && map->Size() != EI_NIDENT) {
    return nullptr;
  }
  uint8_t* header = map->Begin();
  if (header[EI_CLASS] == ELFCLASS64) {
    ElfFileImpl64* elf_file_impl = ElfFileImpl64::Open(file, mmap_prot, mmap_flags, error_msg);
    if (elf_file_impl == nullptr) {
      return nullptr;
    }
    return new ElfFile(elf_file_impl);
  } else if (header[EI_CLASS] == ELFCLASS32) {
    ElfFileImpl32* elf_file_impl = ElfFileImpl32::Open(file, mmap_prot, mmap_flags, error_msg);
    if (elf_file_impl == nullptr) {
      return nullptr;
    }
    return new ElfFile(elf_file_impl);
  } else {
    *error_msg = StringPrintf("Failed to find expected EI_CLASS value %d or %d in %s, found %d",
                              ELFCLASS32, ELFCLASS64,
                              file->GetPath().c_str(),
                              header[EI_CLASS]);
    return nullptr;
  }
}

#define DELEGATE_TO_IMPL(func, ...) \
  if (elf64_.get() != nullptr) { \
    return elf64_->func(__VA_ARGS__); \
  } else { \
    DCHECK(elf32_.get() != nullptr); \
    return elf32_->func(__VA_ARGS__); \
  }

bool ElfFile::Load(bool executable, std::string* error_msg) {
  DELEGATE_TO_IMPL(Load, executable, error_msg);
}

const uint8_t* ElfFile::FindDynamicSymbolAddress(const std::string& symbol_name) const {
  DELEGATE_TO_IMPL(FindDynamicSymbolAddress, symbol_name);
}

size_t ElfFile::Size() const {
  DELEGATE_TO_IMPL(Size);
}

uint8_t* ElfFile::Begin() const {
  DELEGATE_TO_IMPL(Begin);
}

uint8_t* ElfFile::End() const {
  DELEGATE_TO_IMPL(End);
}

const File& ElfFile::GetFile() const {
  DELEGATE_TO_IMPL(GetFile);
}

bool ElfFile::GetSectionOffsetAndSize(const char* section_name, uint64_t* offset, uint64_t* size) {
  if (elf32_.get() == nullptr) {
    CHECK(elf64_.get() != nullptr);

    Elf64_Shdr *shdr = elf64_->FindSectionByName(section_name);
    if (shdr == nullptr) {
      return false;
    }
    if (offset != nullptr) {
      *offset = shdr->sh_offset;
    }
    if (size != nullptr) {
      *size = shdr->sh_size;
    }
    return true;
  } else {
    Elf32_Shdr *shdr = elf32_->FindSectionByName(section_name);
    if (shdr == nullptr) {
      return false;
    }
    if (offset != nullptr) {
      *offset = shdr->sh_offset;
    }
    if (size != nullptr) {
      *size = shdr->sh_size;
    }
    return true;
  }
}

uint64_t ElfFile::FindSymbolAddress(unsigned section_type,
                                    const std::string& symbol_name,
                                    bool build_map) {
  DELEGATE_TO_IMPL(FindSymbolAddress, section_type, symbol_name, build_map);
}

size_t ElfFile::GetLoadedSize() const {
  DELEGATE_TO_IMPL(GetLoadedSize);
}

bool ElfFile::Strip(File* file, std::string* error_msg) {
  std::unique_ptr<ElfFile> elf_file(ElfFile::Open(file, true, false, error_msg));
  if (elf_file.get() == nullptr) {
    return false;
  }

  if (elf_file->elf64_.get() != nullptr)
    return elf_file->elf64_->Strip(error_msg);
  else
    return elf_file->elf32_->Strip(error_msg);
}

bool ElfFile::Fixup(uint64_t base_address) {
  if (elf64_.get() != nullptr) {
    return elf64_->Fixup(static_cast<Elf64_Addr>(base_address));
  } else {
    DCHECK(elf32_.get() != nullptr);
    CHECK(IsUint<32>(base_address)) << std::hex << base_address;
    return elf32_->Fixup(static_cast<Elf32_Addr>(base_address));
  }
  DELEGATE_TO_IMPL(Fixup, base_address);
}

}  // namespace art
