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

#include "elf_writer_quick.h"

#include <unordered_map>

#include "base/logging.h"
#include "base/unix_file/fd_file.h"
#include "buffered_output_stream.h"
#include "driver/compiler_driver.h"
#include "dwarf.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "file_output_stream.h"
#include "globals.h"
#include "leb128.h"
#include "oat.h"
#include "oat_writer.h"
#include "utils.h"

namespace art {

template <typename Elf_Word, typename Elf_Shdr>
static constexpr Elf_Word NextOffset(const Elf_Shdr& cur, const Elf_Shdr& prev) {
  return RoundUp(prev.sh_size + prev.sh_offset, cur.sh_addralign);
}

static uint8_t MakeStInfo(uint8_t binding, uint8_t type) {
  return ((binding) << 4) + ((type) & 0xf);
}

static void PushByte(std::vector<uint8_t>* buf, int data) {
  buf->push_back(data & 0xff);
}

static uint32_t PushStr(std::vector<uint8_t>* buf, const char* str, const char* def = nullptr) {
  if (str == nullptr) {
    str = def;
  }

  uint32_t offset = buf->size();
  for (size_t i = 0; str[i] != '\0'; ++i) {
    buf->push_back(str[i]);
  }
  buf->push_back('\0');
  return offset;
}

static uint32_t PushStr(std::vector<uint8_t>* buf, const std::string &str) {
  uint32_t offset = buf->size();
  buf->insert(buf->end(), str.begin(), str.end());
  buf->push_back('\0');
  return offset;
}

static void UpdateWord(std::vector<uint8_t>* buf, int offset, int data) {
  (*buf)[offset+0] = data;
  (*buf)[offset+1] = data >> 8;
  (*buf)[offset+2] = data >> 16;
  (*buf)[offset+3] = data >> 24;
}

static void PushHalf(std::vector<uint8_t>* buf, int data) {
  buf->push_back(data & 0xff);
  buf->push_back((data >> 8) & 0xff);
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
bool ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfBuilder::Init() {
  // The basic layout of the elf file. Order may be different in final output.
  // +-------------------------+
  // | Elf_Ehdr                |
  // +-------------------------+
  // | Elf_Phdr PHDR           |
  // | Elf_Phdr LOAD R         | .dynsym .dynstr .hash .rodata
  // | Elf_Phdr LOAD R X       | .text
  // | Elf_Phdr LOAD RW        | .dynamic
  // | Elf_Phdr DYNAMIC        | .dynamic
  // +-------------------------+
  // | .dynsym                 |
  // | Elf_Sym  STN_UNDEF      |
  // | Elf_Sym  oatdata        |
  // | Elf_Sym  oatexec        |
  // | Elf_Sym  oatlastword    |
  // +-------------------------+
  // | .dynstr                 |
  // | \0                      |
  // | oatdata\0               |
  // | oatexec\0               |
  // | oatlastword\0           |
  // | boot.oat\0              |
  // +-------------------------+
  // | .hash                   |
  // | Elf_Word nbucket = b    |
  // | Elf_Word nchain  = c    |
  // | Elf_Word bucket[0]      |
  // |         ...             |
  // | Elf_Word bucket[b - 1]  |
  // | Elf_Word chain[0]       |
  // |         ...             |
  // | Elf_Word chain[c - 1]   |
  // +-------------------------+
  // | .rodata                 |
  // | oatdata..oatexec-4      |
  // +-------------------------+
  // | .text                   |
  // | oatexec..oatlastword    |
  // +-------------------------+
  // | .dynamic                |
  // | Elf_Dyn DT_SONAME       |
  // | Elf_Dyn DT_HASH         |
  // | Elf_Dyn DT_SYMTAB       |
  // | Elf_Dyn DT_SYMENT       |
  // | Elf_Dyn DT_STRTAB       |
  // | Elf_Dyn DT_STRSZ        |
  // | Elf_Dyn DT_NULL         |
  // +-------------------------+  (Optional)
  // | .strtab                 |  (Optional)
  // | program symbol names    |  (Optional)
  // +-------------------------+  (Optional)
  // | .symtab                 |  (Optional)
  // | program symbols         |  (Optional)
  // +-------------------------+
  // | .shstrtab               |
  // | \0                      |
  // | .dynamic\0              |
  // | .dynsym\0               |
  // | .dynstr\0               |
  // | .hash\0                 |
  // | .rodata\0               |
  // | .text\0                 |
  // | .shstrtab\0             |
  // | .symtab\0               |  (Optional)
  // | .strtab\0               |  (Optional)
  // | .debug_str\0            |  (Optional)
  // | .debug_info\0           |  (Optional)
  // | .eh_frame\0             |  (Optional)
  // | .debug_line\0           |  (Optional)
  // | .debug_abbrev\0         |  (Optional)
  // +-------------------------+  (Optional)
  // | .debug_info             |  (Optional)
  // +-------------------------+  (Optional)
  // | .debug_abbrev           |  (Optional)
  // +-------------------------+  (Optional)
  // | .eh_frame               |  (Optional)
  // +-------------------------+  (Optional)
  // | .debug_line             |  (Optional)
  // +-------------------------+  (Optional)
  // | .debug_str              |  (Optional)
  // +-------------------------+  (Optional)
  // | Elf_Shdr NULL           |
  // | Elf_Shdr .dynsym        |
  // | Elf_Shdr .dynstr        |
  // | Elf_Shdr .hash          |
  // | Elf_Shdr .text          |
  // | Elf_Shdr .rodata        |
  // | Elf_Shdr .dynamic       |
  // | Elf_Shdr .shstrtab      |
  // | Elf_Shdr .debug_info    |  (Optional)
  // | Elf_Shdr .debug_abbrev  |  (Optional)
  // | Elf_Shdr .eh_frame      |  (Optional)
  // | Elf_Shdr .debug_line    |  (Optional)
  // | Elf_Shdr .debug_str     |  (Optional)
  // +-------------------------+

  if (fatal_error_) {
    return false;
  }
  // Step 1. Figure out all the offsets.

  if (debug_logging_) {
    LOG(INFO) << "phdr_offset=" << PHDR_OFFSET << std::hex << " " << PHDR_OFFSET;
    LOG(INFO) << "phdr_size=" << PHDR_SIZE << std::hex << " " << PHDR_SIZE;
  }

  memset(&program_headers_, 0, sizeof(program_headers_));
  program_headers_[PH_PHDR].p_type    = PT_PHDR;
  program_headers_[PH_PHDR].p_offset  = PHDR_OFFSET;
  program_headers_[PH_PHDR].p_vaddr   = PHDR_OFFSET;
  program_headers_[PH_PHDR].p_paddr   = PHDR_OFFSET;
  program_headers_[PH_PHDR].p_filesz  = sizeof(program_headers_);
  program_headers_[PH_PHDR].p_memsz   = sizeof(program_headers_);
  program_headers_[PH_PHDR].p_flags   = PF_R;
  program_headers_[PH_PHDR].p_align   = sizeof(Elf_Word);

  program_headers_[PH_LOAD_R__].p_type    = PT_LOAD;
  program_headers_[PH_LOAD_R__].p_offset  = 0;
  program_headers_[PH_LOAD_R__].p_vaddr   = 0;
  program_headers_[PH_LOAD_R__].p_paddr   = 0;
  program_headers_[PH_LOAD_R__].p_flags   = PF_R;

  program_headers_[PH_LOAD_R_X].p_type    = PT_LOAD;
  program_headers_[PH_LOAD_R_X].p_flags   = PF_R | PF_X;

  program_headers_[PH_LOAD_RW_].p_type    = PT_LOAD;
  program_headers_[PH_LOAD_RW_].p_flags   = PF_R | PF_W;

  program_headers_[PH_DYNAMIC].p_type    = PT_DYNAMIC;
  program_headers_[PH_DYNAMIC].p_flags   = PF_R | PF_W;

  // Get the dynstr string.
  dynstr_ = dynsym_builder_.GenerateStrtab();

  // Add the SONAME to the dynstr.
  dynstr_soname_offset_ = dynstr_.size();
  std::string file_name(elf_file_->GetPath());
  size_t directory_separator_pos = file_name.rfind('/');
  if (directory_separator_pos != std::string::npos) {
    file_name = file_name.substr(directory_separator_pos + 1);
  }
  dynstr_ += file_name;
  dynstr_ += '\0';
  if (debug_logging_) {
    LOG(INFO) << "dynstr size (bytes)   =" << dynstr_.size()
              << std::hex << " " << dynstr_.size();
    LOG(INFO) << "dynsym size (elements)=" << dynsym_builder_.GetSize()
              << std::hex << " " << dynsym_builder_.GetSize();
  }

  // Get the section header string table.
  shstrtab_ += '\0';

  // Setup sym_undef
  memset(&null_hdr_, 0, sizeof(null_hdr_));
  null_hdr_.sh_type = SHT_NULL;
  null_hdr_.sh_link = SHN_UNDEF;
  section_ptrs_.push_back(&null_hdr_);

  section_index_ = 1;

  // setup .dynsym
  section_ptrs_.push_back(&dynsym_builder_.section_);
  AssignSectionStr(&dynsym_builder_, &shstrtab_);
  dynsym_builder_.section_index_ = section_index_++;

  // Setup .dynstr
  section_ptrs_.push_back(&dynsym_builder_.strtab_.section_);
  AssignSectionStr(&dynsym_builder_.strtab_, &shstrtab_);
  dynsym_builder_.strtab_.section_index_ = section_index_++;

  // Setup .hash
  section_ptrs_.push_back(&hash_builder_.section_);
  AssignSectionStr(&hash_builder_, &shstrtab_);
  hash_builder_.section_index_ = section_index_++;

  // Setup .rodata
  section_ptrs_.push_back(&rodata_builder_.section_);
  AssignSectionStr(&rodata_builder_, &shstrtab_);
  rodata_builder_.section_index_ = section_index_++;

  // Setup .text
  section_ptrs_.push_back(&text_builder_.section_);
  AssignSectionStr(&text_builder_, &shstrtab_);
  text_builder_.section_index_ = section_index_++;

  // Setup .dynamic
  section_ptrs_.push_back(&dynamic_builder_.section_);
  AssignSectionStr(&dynamic_builder_, &shstrtab_);
  dynamic_builder_.section_index_ = section_index_++;

  // Fill in the hash section.
  hash_ = dynsym_builder_.GenerateHashContents();

  if (debug_logging_) {
    LOG(INFO) << ".hash size (bytes)=" << hash_.size() * sizeof(Elf_Word)
              << std::hex << " " << hash_.size() * sizeof(Elf_Word);
  }

  Elf_Word base_offset = sizeof(Elf_Ehdr) + sizeof(program_headers_);

  // Get the layout in the sections.
  //
  // Get the layout of the dynsym section.
  dynsym_builder_.section_.sh_offset = RoundUp(base_offset, dynsym_builder_.section_.sh_addralign);
  dynsym_builder_.section_.sh_addr = dynsym_builder_.section_.sh_offset;
  dynsym_builder_.section_.sh_size = dynsym_builder_.GetSize() * sizeof(Elf_Sym);
  dynsym_builder_.section_.sh_link = dynsym_builder_.GetLink();

  // Get the layout of the dynstr section.
  dynsym_builder_.strtab_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                               (dynsym_builder_.strtab_.section_,
                                                dynsym_builder_.section_);
  dynsym_builder_.strtab_.section_.sh_addr = dynsym_builder_.strtab_.section_.sh_offset;
  dynsym_builder_.strtab_.section_.sh_size = dynstr_.size();
  dynsym_builder_.strtab_.section_.sh_link = dynsym_builder_.strtab_.GetLink();

  // Get the layout of the hash section
  hash_builder_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                     (hash_builder_.section_,
                                      dynsym_builder_.strtab_.section_);
  hash_builder_.section_.sh_addr = hash_builder_.section_.sh_offset;
  hash_builder_.section_.sh_size = hash_.size() * sizeof(Elf_Word);
  hash_builder_.section_.sh_link = hash_builder_.GetLink();

  // Get the layout of the rodata section.
  rodata_builder_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                       (rodata_builder_.section_,
                                        hash_builder_.section_);
  rodata_builder_.section_.sh_addr = rodata_builder_.section_.sh_offset;
  rodata_builder_.section_.sh_size = rodata_builder_.size_;
  rodata_builder_.section_.sh_link = rodata_builder_.GetLink();

  // Get the layout of the text section.
  text_builder_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                     (text_builder_.section_, rodata_builder_.section_);
  text_builder_.section_.sh_addr = text_builder_.section_.sh_offset;
  text_builder_.section_.sh_size = text_builder_.size_;
  text_builder_.section_.sh_link = text_builder_.GetLink();
  CHECK_ALIGNED(rodata_builder_.section_.sh_offset + rodata_builder_.section_.sh_size, kPageSize);

  // Get the layout of the dynamic section.
  dynamic_builder_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                        (dynamic_builder_.section_,
                                         text_builder_.section_);
  dynamic_builder_.section_.sh_addr = dynamic_builder_.section_.sh_offset;
  dynamic_builder_.section_.sh_size = dynamic_builder_.GetSize() * sizeof(Elf_Dyn);
  dynamic_builder_.section_.sh_link = dynamic_builder_.GetLink();

  if (debug_logging_) {
    LOG(INFO) << "dynsym off=" << dynsym_builder_.section_.sh_offset
              << " dynsym size=" << dynsym_builder_.section_.sh_size;
    LOG(INFO) << "dynstr off=" << dynsym_builder_.strtab_.section_.sh_offset
              << " dynstr size=" << dynsym_builder_.strtab_.section_.sh_size;
    LOG(INFO) << "hash off=" << hash_builder_.section_.sh_offset
              << " hash size=" << hash_builder_.section_.sh_size;
    LOG(INFO) << "rodata off=" << rodata_builder_.section_.sh_offset
              << " rodata size=" << rodata_builder_.section_.sh_size;
    LOG(INFO) << "text off=" << text_builder_.section_.sh_offset
              << " text size=" << text_builder_.section_.sh_size;
    LOG(INFO) << "dynamic off=" << dynamic_builder_.section_.sh_offset
              << " dynamic size=" << dynamic_builder_.section_.sh_size;
  }

  return true;
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
bool ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfBuilder::Write() {
  std::vector<ElfFilePiece> pieces;
  Elf_Shdr prev = dynamic_builder_.section_;
  std::string strtab;

  if (IncludingDebugSymbols()) {
    // Setup .symtab
    section_ptrs_.push_back(&symtab_builder_.section_);
    AssignSectionStr(&symtab_builder_, &shstrtab_);
    symtab_builder_.section_index_ = section_index_++;

    // Setup .strtab
    section_ptrs_.push_back(&symtab_builder_.strtab_.section_);
    AssignSectionStr(&symtab_builder_.strtab_, &shstrtab_);
    symtab_builder_.strtab_.section_index_ = section_index_++;

    strtab = symtab_builder_.GenerateStrtab();
    if (debug_logging_) {
      LOG(INFO) << "strtab size (bytes)    =" << strtab.size()
                << std::hex << " " << strtab.size();
      LOG(INFO) << "symtab size (elements) =" << symtab_builder_.GetSize()
                << std::hex << " " << symtab_builder_.GetSize();
    }
  }

  // Setup all the other sections.
  for (ElfRawSectionBuilder *builder = other_builders_.data(),
                            *end = builder + other_builders_.size();
       builder != end; ++builder) {
    section_ptrs_.push_back(&builder->section_);
    AssignSectionStr(builder, &shstrtab_);
    builder->section_index_ = section_index_++;
  }

  // Setup shstrtab
  section_ptrs_.push_back(&shstrtab_builder_.section_);
  AssignSectionStr(&shstrtab_builder_, &shstrtab_);
  shstrtab_builder_.section_index_ = section_index_++;

  if (debug_logging_) {
    LOG(INFO) << ".shstrtab size    (bytes)   =" << shstrtab_.size()
              << std::hex << " " << shstrtab_.size();
    LOG(INFO) << "section list size (elements)=" << section_ptrs_.size()
              << std::hex << " " << section_ptrs_.size();
  }

  if (IncludingDebugSymbols()) {
    // Get the layout of the symtab section.
    symtab_builder_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                         (symtab_builder_.section_,
                                          dynamic_builder_.section_);
    symtab_builder_.section_.sh_addr = 0;
    // Add to leave space for the null symbol.
    symtab_builder_.section_.sh_size = symtab_builder_.GetSize() * sizeof(Elf_Sym);
    symtab_builder_.section_.sh_link = symtab_builder_.GetLink();

    // Get the layout of the dynstr section.
    symtab_builder_.strtab_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                                 (symtab_builder_.strtab_.section_,
                                                  symtab_builder_.section_);
    symtab_builder_.strtab_.section_.sh_addr = 0;
    symtab_builder_.strtab_.section_.sh_size = strtab.size();
    symtab_builder_.strtab_.section_.sh_link = symtab_builder_.strtab_.GetLink();

    prev = symtab_builder_.strtab_.section_;
    if (debug_logging_) {
      LOG(INFO) << "symtab off=" << symtab_builder_.section_.sh_offset
                << " symtab size=" << symtab_builder_.section_.sh_size;
      LOG(INFO) << "strtab off=" << symtab_builder_.strtab_.section_.sh_offset
                << " strtab size=" << symtab_builder_.strtab_.section_.sh_size;
    }
  }

  // Get the layout of the extra sections. (This will deal with the debug
  // sections if they are there)
  for (auto it = other_builders_.begin(); it != other_builders_.end(); ++it) {
    it->section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>(it->section_, prev);
    it->section_.sh_addr = 0;
    it->section_.sh_size = it->GetBuffer()->size();
    it->section_.sh_link = it->GetLink();
    pieces.push_back(ElfFilePiece(it->name_, it->section_.sh_offset,
                                  it->GetBuffer()->data(), it->GetBuffer()->size()));
    prev = it->section_;
    if (debug_logging_) {
      LOG(INFO) << it->name_ << " off=" << it->section_.sh_offset
                << " " << it->name_ << " size=" << it->section_.sh_size;
    }
  }

  // Get the layout of the shstrtab section
  shstrtab_builder_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                         (shstrtab_builder_.section_, prev);
  shstrtab_builder_.section_.sh_addr = 0;
  shstrtab_builder_.section_.sh_size = shstrtab_.size();
  shstrtab_builder_.section_.sh_link = shstrtab_builder_.GetLink();
  if (debug_logging_) {
      LOG(INFO) << "shstrtab off=" << shstrtab_builder_.section_.sh_offset
                << " shstrtab size=" << shstrtab_builder_.section_.sh_size;
  }

  // The section list comes after come after.
  Elf_Word sections_offset = RoundUp(
      shstrtab_builder_.section_.sh_offset + shstrtab_builder_.section_.sh_size,
      sizeof(Elf_Word));

  // Setup the actual symbol arrays.
  std::vector<Elf_Sym> dynsym = dynsym_builder_.GenerateSymtab();
  CHECK_EQ(dynsym.size() * sizeof(Elf_Sym), dynsym_builder_.section_.sh_size);
  std::vector<Elf_Sym> symtab;
  if (IncludingDebugSymbols()) {
    symtab = symtab_builder_.GenerateSymtab();
    CHECK_EQ(symtab.size() * sizeof(Elf_Sym), symtab_builder_.section_.sh_size);
  }

  // Setup the dynamic section.
  // This will add the 2 values we cannot know until now time, namely the size
  // and the soname_offset.
  std::vector<Elf_Dyn> dynamic = dynamic_builder_.GetDynamics(dynstr_.size(),
                                                                dynstr_soname_offset_);
  CHECK_EQ(dynamic.size() * sizeof(Elf_Dyn), dynamic_builder_.section_.sh_size);

  // Finish setup of the program headers now that we know the layout of the
  // whole file.
  Elf_Word load_r_size = rodata_builder_.section_.sh_offset + rodata_builder_.section_.sh_size;
  program_headers_[PH_LOAD_R__].p_filesz = load_r_size;
  program_headers_[PH_LOAD_R__].p_memsz =  load_r_size;
  program_headers_[PH_LOAD_R__].p_align =  rodata_builder_.section_.sh_addralign;

  Elf_Word load_rx_size = text_builder_.section_.sh_size;
  program_headers_[PH_LOAD_R_X].p_offset = text_builder_.section_.sh_offset;
  program_headers_[PH_LOAD_R_X].p_vaddr  = text_builder_.section_.sh_offset;
  program_headers_[PH_LOAD_R_X].p_paddr  = text_builder_.section_.sh_offset;
  program_headers_[PH_LOAD_R_X].p_filesz = load_rx_size;
  program_headers_[PH_LOAD_R_X].p_memsz  = load_rx_size;
  program_headers_[PH_LOAD_R_X].p_align  = text_builder_.section_.sh_addralign;

  program_headers_[PH_LOAD_RW_].p_offset = dynamic_builder_.section_.sh_offset;
  program_headers_[PH_LOAD_RW_].p_vaddr  = dynamic_builder_.section_.sh_offset;
  program_headers_[PH_LOAD_RW_].p_paddr  = dynamic_builder_.section_.sh_offset;
  program_headers_[PH_LOAD_RW_].p_filesz = dynamic_builder_.section_.sh_size;
  program_headers_[PH_LOAD_RW_].p_memsz  = dynamic_builder_.section_.sh_size;
  program_headers_[PH_LOAD_RW_].p_align  = dynamic_builder_.section_.sh_addralign;

  program_headers_[PH_DYNAMIC].p_offset = dynamic_builder_.section_.sh_offset;
  program_headers_[PH_DYNAMIC].p_vaddr  = dynamic_builder_.section_.sh_offset;
  program_headers_[PH_DYNAMIC].p_paddr  = dynamic_builder_.section_.sh_offset;
  program_headers_[PH_DYNAMIC].p_filesz = dynamic_builder_.section_.sh_size;
  program_headers_[PH_DYNAMIC].p_memsz  = dynamic_builder_.section_.sh_size;
  program_headers_[PH_DYNAMIC].p_align  = dynamic_builder_.section_.sh_addralign;

  // Finish setup of the Ehdr values.
  elf_header_.e_phoff = PHDR_OFFSET;
  elf_header_.e_shoff = sections_offset;
  elf_header_.e_phnum = PH_NUM;
  elf_header_.e_shnum = section_ptrs_.size();
  elf_header_.e_shstrndx = shstrtab_builder_.section_index_;

  // Add the rest of the pieces to the list.
  pieces.push_back(ElfFilePiece("Elf Header", 0, &elf_header_, sizeof(elf_header_)));
  pieces.push_back(ElfFilePiece("Program headers", PHDR_OFFSET,
                                &program_headers_, sizeof(program_headers_)));
  pieces.push_back(ElfFilePiece(".dynamic", dynamic_builder_.section_.sh_offset,
                                dynamic.data(), dynamic_builder_.section_.sh_size));
  pieces.push_back(ElfFilePiece(".dynsym", dynsym_builder_.section_.sh_offset,
                                dynsym.data(), dynsym.size() * sizeof(Elf_Sym)));
  pieces.push_back(ElfFilePiece(".dynstr", dynsym_builder_.strtab_.section_.sh_offset,
                                dynstr_.c_str(), dynstr_.size()));
  pieces.push_back(ElfFilePiece(".hash", hash_builder_.section_.sh_offset,
                                hash_.data(), hash_.size() * sizeof(Elf_Word)));
  pieces.push_back(ElfFilePiece(".rodata", rodata_builder_.section_.sh_offset,
                                nullptr, rodata_builder_.section_.sh_size));
  pieces.push_back(ElfFilePiece(".text", text_builder_.section_.sh_offset,
                                nullptr, text_builder_.section_.sh_size));
  if (IncludingDebugSymbols()) {
    pieces.push_back(ElfFilePiece(".symtab", symtab_builder_.section_.sh_offset,
                                  symtab.data(), symtab.size() * sizeof(Elf_Sym)));
    pieces.push_back(ElfFilePiece(".strtab", symtab_builder_.strtab_.section_.sh_offset,
                                  strtab.c_str(), strtab.size()));
  }
  pieces.push_back(ElfFilePiece(".shstrtab", shstrtab_builder_.section_.sh_offset,
                                &shstrtab_[0], shstrtab_.size()));
  for (uint32_t i = 0; i < section_ptrs_.size(); ++i) {
    // Just add all the sections in induvidually since they are all over the
    // place on the heap/stack.
    Elf_Word cur_off = sections_offset + i * sizeof(Elf_Shdr);
    pieces.push_back(ElfFilePiece("section table piece", cur_off,
                                  section_ptrs_[i], sizeof(Elf_Shdr)));
  }

  if (!WriteOutFile(pieces)) {
    LOG(ERROR) << "Unable to write to file " << elf_file_->GetPath();
    return false;
  }
  // write out the actual oat file data.
  Elf_Word oat_data_offset = rodata_builder_.section_.sh_offset;
  if (static_cast<off_t>(oat_data_offset) != lseek(elf_file_->Fd(), oat_data_offset, SEEK_SET)) {
    PLOG(ERROR) << "Failed to seek to .rodata offset " << oat_data_offset
                << " for " << elf_file_->GetPath();
    return false;
  }
  std::unique_ptr<BufferedOutputStream> output_stream(
      new BufferedOutputStream(new FileOutputStream(elf_file_)));
  if (!oat_writer_->Write(output_stream.get())) {
    PLOG(ERROR) << "Failed to write .rodata and .text for " << elf_file_->GetPath();
    return false;
  }

  return true;
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
bool ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfBuilder::WriteOutFile(const std::vector<ElfFilePiece>& pieces) {
  // TODO It would be nice if this checked for overlap.
  for (auto it = pieces.begin(); it != pieces.end(); ++it) {
    if (it->data_) {
      if (static_cast<off_t>(it->offset_) != lseek(elf_file_->Fd(), it->offset_, SEEK_SET)) {
        PLOG(ERROR) << "Failed to seek to " << it->dbg_name_ << " offset location "
                    << it->offset_ << " for " << elf_file_->GetPath();
        return false;
      }
      if (!elf_file_->WriteFully(it->data_, it->size_)) {
        PLOG(ERROR) << "Failed to write " << it->dbg_name_ << " for " << elf_file_->GetPath();
        return false;
      }
    }
  }
  return true;
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfBuilder::SetupDynamic() {
  dynamic_builder_.AddDynamicTag(DT_HASH, 0, &hash_builder_);
  dynamic_builder_.AddDynamicTag(DT_STRTAB, 0, &dynsym_builder_.strtab_);
  dynamic_builder_.AddDynamicTag(DT_SYMTAB, 0, &dynsym_builder_);
  dynamic_builder_.AddDynamicTag(DT_SYMENT, sizeof(Elf_Sym));
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfBuilder::SetupRequiredSymbols() {
  dynsym_builder_.AddSymbol("oatdata", &rodata_builder_, 0, true,
                            rodata_builder_.size_, STB_GLOBAL, STT_OBJECT);
  dynsym_builder_.AddSymbol("oatexec", &text_builder_, 0, true,
                            text_builder_.size_, STB_GLOBAL, STT_OBJECT);
  dynsym_builder_.AddSymbol("oatlastword", &text_builder_, text_builder_.size_ - 4,
                            true, 4, STB_GLOBAL, STT_OBJECT);
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfDynamicBuilder::AddDynamicTag(Elf_Sword tag, Elf_Word d_un) {
  if (tag == DT_NULL) {
    return;
  }
  dynamics_.push_back({nullptr, tag, d_un});
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfDynamicBuilder::AddDynamicTag(Elf_Sword tag, Elf_Word d_un,
                                                      ElfSectionBuilder* section) {
  if (tag == DT_NULL) {
    return;
  }
  dynamics_.push_back({section, tag, d_un});
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
std::vector<Elf_Dyn> ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfDynamicBuilder::GetDynamics(Elf_Word strsz,
                                                                    Elf_Word soname) {
  std::vector<Elf_Dyn> ret;
  for (auto it = dynamics_.cbegin(); it != dynamics_.cend(); ++it) {
    if (it->section_) {
      // We are adding an address relative to a section.
      ret.push_back(
          {it->tag_, {it->off_ + it->section_->section_.sh_addr}});
    } else {
      ret.push_back({it->tag_, {it->off_}});
    }
  }
  ret.push_back({DT_STRSZ, {strsz}});
  ret.push_back({DT_SONAME, {soname}});
  ret.push_back({DT_NULL, {0}});
  return ret;
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
std::vector<Elf_Sym> ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfSymtabBuilder::GenerateSymtab() {
  std::vector<Elf_Sym> ret;
  Elf_Sym undef_sym;
  memset(&undef_sym, 0, sizeof(undef_sym));
  undef_sym.st_shndx = SHN_UNDEF;
  ret.push_back(undef_sym);

  for (auto it = symbols_.cbegin(); it != symbols_.cend(); ++it) {
    Elf_Sym sym;
    memset(&sym, 0, sizeof(sym));
    sym.st_name = it->name_idx_;
    if (it->is_relative_) {
      sym.st_value = it->addr_ + it->section_->section_.sh_offset;
    } else {
      sym.st_value = it->addr_;
    }
    sym.st_size = it->size_;
    sym.st_other = it->other_;
    sym.st_shndx = it->section_->section_index_;
    sym.st_info = it->info_;

    ret.push_back(sym);
  }
  return ret;
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
std::string ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfSymtabBuilder::GenerateStrtab() {
  std::string tab;
  tab += '\0';
  for (auto it = symbols_.begin(); it != symbols_.end(); ++it) {
    it->name_idx_ = tab.size();
    tab += it->name_;
    tab += '\0';
  }
  strtab_.section_.sh_size = tab.size();
  return tab;
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfBuilder::AssignSectionStr(
    ElfSectionBuilder* builder, std::string* strtab) {
  builder->section_.sh_name = strtab->size();
  *strtab += builder->name_;
  *strtab += '\0';
  if (debug_logging_) {
    LOG(INFO) << "adding section name \"" << builder->name_ << "\" "
              << "to shstrtab at offset " << builder->section_.sh_name;
  }
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

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
std::vector<Elf_Word> ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfSymtabBuilder::GenerateHashContents() {
  // Here is how The ELF hash table works.
  // There are 3 arrays to worry about.
  // * The symbol table where the symbol information is.
  // * The bucket array which is an array of indexes into the symtab and chain.
  // * The chain array which is also an array of indexes into the symtab and chain.
  //
  // Lets say the state is something like this.
  // +--------+       +--------+      +-----------+
  // | symtab |       | bucket |      |   chain   |
  // |  null  |       | 1      |      | STN_UNDEF |
  // | <sym1> |       | 4      |      | 2         |
  // | <sym2> |       |        |      | 5         |
  // | <sym3> |       |        |      | STN_UNDEF |
  // | <sym4> |       |        |      | 3         |
  // | <sym5> |       |        |      | STN_UNDEF |
  // +--------+       +--------+      +-----------+
  //
  // The lookup process (in python psudocode) is
  //
  // def GetSym(name):
  //     # NB STN_UNDEF == 0
  //     indx = bucket[elfhash(name) % num_buckets]
  //     while indx != STN_UNDEF:
  //         if GetSymbolName(symtab[indx]) == name:
  //             return symtab[indx]
  //         indx = chain[indx]
  //     return SYMBOL_NOT_FOUND
  //
  // Between bucket and chain arrays every symtab index must be present exactly
  // once (except for STN_UNDEF, which must be present 1 + num_bucket times).

  // Select number of buckets.
  // This is essentially arbitrary.
  Elf_Word nbuckets;
  Elf_Word chain_size = GetSize();
  if (symbols_.size() < 8) {
    nbuckets = 2;
  } else if (symbols_.size() < 32) {
    nbuckets = 4;
  } else if (symbols_.size() < 256) {
    nbuckets = 16;
  } else {
    // Have about 32 ids per bucket.
    nbuckets = RoundUp(symbols_.size()/32, 2);
  }
  std::vector<Elf_Word> hash;
  hash.push_back(nbuckets);
  hash.push_back(chain_size);
  uint32_t bucket_offset = hash.size();
  uint32_t chain_offset = bucket_offset + nbuckets;
  hash.resize(hash.size() + nbuckets + chain_size, 0);

  Elf_Word* buckets = hash.data() + bucket_offset;
  Elf_Word* chain   = hash.data() + chain_offset;

  // Set up the actual hash table.
  for (Elf_Word i = 0; i < symbols_.size(); i++) {
    // Add 1 since we need to have the null symbol that is not in the symbols
    // list.
    Elf_Word index = i + 1;
    Elf_Word hash_val = static_cast<Elf_Word>(elfhash(symbols_[i].name_.c_str())) % nbuckets;
    if (buckets[hash_val] == 0) {
      buckets[hash_val] = index;
    } else {
      hash_val = buckets[hash_val];
      CHECK_LT(hash_val, chain_size);
      while (chain[hash_val] != 0) {
        hash_val = chain[hash_val];
        CHECK_LT(hash_val, chain_size);
      }
      chain[hash_val] = index;
      // Check for loops. Works because if this is non-empty then there must be
      // another cell which already contains the same symbol index as this one,
      // which means some symbol has more then one name, which isn't allowed.
      CHECK_EQ(chain[index], static_cast<Elf_Word>(0));
    }
  }

  return hash;
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfBuilder::SetupEhdr() {
  memset(&elf_header_, 0, sizeof(elf_header_));
  elf_header_.e_ident[EI_MAG0]       = ELFMAG0;
  elf_header_.e_ident[EI_MAG1]       = ELFMAG1;
  elf_header_.e_ident[EI_MAG2]       = ELFMAG2;
  elf_header_.e_ident[EI_MAG3]       = ELFMAG3;
  elf_header_.e_ident[EI_CLASS]      = ELFCLASS32;
  elf_header_.e_ident[EI_DATA]       = ELFDATA2LSB;
  elf_header_.e_ident[EI_VERSION]    = EV_CURRENT;
  elf_header_.e_ident[EI_OSABI]      = ELFOSABI_LINUX;
  elf_header_.e_ident[EI_ABIVERSION] = 0;
  elf_header_.e_type = ET_DYN;
  elf_header_.e_version = 1;
  elf_header_.e_entry = 0;
  elf_header_.e_ehsize = sizeof(Elf_Ehdr);
  elf_header_.e_phentsize = sizeof(Elf_Phdr);
  elf_header_.e_shentsize = sizeof(Elf_Shdr);
  elf_header_.e_phoff = sizeof(Elf_Ehdr);
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfBuilder::SetISA(InstructionSet isa) {
  switch (isa) {
    case kArm:
      // Fall through.
    case kThumb2: {
      elf_header_.e_machine = EM_ARM;
      elf_header_.e_flags = EF_ARM_EABI_VER5;
      break;
    }
    case kArm64: {
      elf_header_.e_machine = EM_AARCH64;
      elf_header_.e_flags = 0;
      break;
    }
    case kX86: {
      elf_header_.e_machine = EM_386;
      elf_header_.e_flags = 0;
      break;
    }
    case kX86_64: {
      elf_header_.e_machine = EM_X86_64;
      elf_header_.e_flags = 0;
      break;
    }
    case kMips: {
      elf_header_.e_machine = EM_MIPS;
      elf_header_.e_flags = (EF_MIPS_NOREORDER |
                             EF_MIPS_PIC       |
                             EF_MIPS_CPIC      |
                             EF_MIPS_ABI_O32   |
                             EF_MIPS_ARCH_32R2);
      break;
    }
    default: {
      fatal_error_ = true;
      LOG(FATAL) << "Unknown instruction set: " << isa;
      break;
    }
  }
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ElfSymtabBuilder::AddSymbol(
    const std::string& name, const ElfSectionBuilder* section, Elf_Addr addr,
    bool is_relative, Elf_Word size, uint8_t binding, uint8_t type, uint8_t other) {
  CHECK(section);
  ElfSymtabBuilder::ElfSymbolState state {name, section, addr, size, is_relative,
                                          MakeStInfo(binding, type), other, 0};
  symbols_.push_back(state);
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
bool ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::Create(File* elf_file,
                            OatWriter* oat_writer,
                            const std::vector<const DexFile*>& dex_files,
                            const std::string& android_root,
                            bool is_host,
                            const CompilerDriver& driver) {
  ElfWriterQuick elf_writer(driver, elf_file);
  return elf_writer.Write(oat_writer, dex_files, android_root, is_host);
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
// Add patch information to this section. Each patch is a Elf_Word that
// identifies an offset from the start of the text section
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::ReservePatchSpace(std::vector<uint8_t>* buffer, bool debug) {
  size_t size =
      compiler_driver_->GetCodeToPatch().size() +
      compiler_driver_->GetMethodsToPatch().size() +
      compiler_driver_->GetClassesToPatch().size();
  if (size == 0) {
    if (debug) {
      LOG(INFO) << "No patches to record";
    }
    return;
  }
  buffer->resize(size * sizeof(uintptr_t));
  if (debug) {
    LOG(INFO) << "Patches reserved for " << size;
  }
}

std::vector<uint8_t>* ConstructCIEFrameX86(bool is_x86_64) {
  std::vector<uint8_t>* cfi_info = new std::vector<uint8_t>;

  // Length (will be filled in later in this routine).
  if (is_x86_64) {
    PushWord(cfi_info, 0xffffffff);  // Indicates 64bit
    PushWord(cfi_info, 0);
    PushWord(cfi_info, 0);
  } else {
    PushWord(cfi_info, 0);
  }

  // CIE id: always 0.
  if (is_x86_64) {
    PushWord(cfi_info, 0);
    PushWord(cfi_info, 0);
  } else {
    PushWord(cfi_info, 0);
  }

  // Version: always 1.
  cfi_info->push_back(0x01);

  // Augmentation: 'zR\0'
  cfi_info->push_back(0x7a);
  cfi_info->push_back(0x52);
  cfi_info->push_back(0x0);

  // Code alignment: 1.
  EncodeUnsignedLeb128(1, cfi_info);

  // Data alignment.
  if (is_x86_64) {
    EncodeSignedLeb128(-8, cfi_info);
  } else {
    EncodeSignedLeb128(-4, cfi_info);
  }

  // Return address register.
  if (is_x86_64) {
    // R16(RIP)
    cfi_info->push_back(0x10);
  } else {
    // R8(EIP)
    cfi_info->push_back(0x08);
  }

  // Augmentation length: 1.
  cfi_info->push_back(1);

  // Augmentation data.
  if (is_x86_64) {
    // 0x04 ((DW_EH_PE_absptr << 4) | DW_EH_PE_udata8).
    cfi_info->push_back(0x04);
  } else {
    // 0x03 ((DW_EH_PE_absptr << 4) | DW_EH_PE_udata4).
    cfi_info->push_back(0x03);
  }

  // Initial instructions.
  if (is_x86_64) {
    // DW_CFA_def_cfa R7(RSP) 8.
    cfi_info->push_back(0x0c);
    cfi_info->push_back(0x07);
    cfi_info->push_back(0x08);

    // DW_CFA_offset R16(RIP) 1 (* -8).
    cfi_info->push_back(0x90);
    cfi_info->push_back(0x01);
  } else {
    // DW_CFA_def_cfa R4(ESP) 4.
    cfi_info->push_back(0x0c);
    cfi_info->push_back(0x04);
    cfi_info->push_back(0x04);

    // DW_CFA_offset R8(EIP) 1 (* -4).
    cfi_info->push_back(0x88);
    cfi_info->push_back(0x01);
  }

  // Padding to a multiple of 4
  while ((cfi_info->size() & 3) != 0) {
    // DW_CFA_nop is encoded as 0.
    cfi_info->push_back(0);
  }

  // Set the length of the CIE inside the generated bytes.
  if (is_x86_64) {
    uint32_t length = cfi_info->size() - 12;
    UpdateWord(cfi_info, 4, length);
  } else {
    uint32_t length = cfi_info->size() - 4;
    UpdateWord(cfi_info, 0, length);
  }
  return cfi_info;
}

std::vector<uint8_t>* ConstructCIEFrame(InstructionSet isa) {
  switch (isa) {
    case kX86:
      return ConstructCIEFrameX86(false);
    case kX86_64:
      return ConstructCIEFrameX86(true);

    default:
      // Not implemented.
      return nullptr;
  }
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
bool ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::Write(OatWriter* oat_writer,
                           const std::vector<const DexFile*>& dex_files_unused,
                           const std::string& android_root_unused,
                           bool is_host_unused) {
  constexpr bool debug = false;
  const OatHeader& oat_header = oat_writer->GetOatHeader();
  Elf_Word oat_data_size = oat_header.GetExecutableOffset();
  uint32_t oat_exec_size = oat_writer->GetSize() - oat_data_size;

  ElfBuilder builder(oat_writer, elf_file_, compiler_driver_->GetInstructionSet(), 0,
                     oat_data_size, oat_data_size, oat_exec_size,
                     compiler_driver_->GetCompilerOptions().GetIncludeDebugSymbols(),
                     debug);

  if (!builder.Init()) {
    return false;
  }

  if (compiler_driver_->GetCompilerOptions().GetIncludeDebugSymbols()) {
    WriteDebugSymbols(&builder, oat_writer);
  }

  if (compiler_driver_->GetCompilerOptions().GetIncludePatchInformation()) {
    ElfRawSectionBuilder oat_patches(".oat_patches", SHT_OAT_PATCH, 0, NULL, 0,
                                     sizeof(uintptr_t), sizeof(uintptr_t));
    ReservePatchSpace(oat_patches.GetBuffer(), debug);
    builder.RegisterRawSection(oat_patches);
  }

  return builder.Write();
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::WriteDebugSymbols(ElfBuilder* builder, OatWriter* oat_writer) {
  std::unique_ptr<std::vector<uint8_t>> cfi_info(
      ConstructCIEFrame(compiler_driver_->GetInstructionSet()));

  Elf_Addr text_section_address = builder->text_builder_.section_.sh_addr;

  // Iterate over the compiled methods.
  const std::vector<OatWriter::DebugInfo>& method_info = oat_writer->GetCFIMethodInfo();
  ElfSymtabBuilder* symtab = &builder->symtab_builder_;
  for (auto it = method_info.begin(); it != method_info.end(); ++it) {
    symtab->AddSymbol(it->method_name_, &builder->text_builder_, it->low_pc_, true,
                      it->high_pc_ - it->low_pc_, STB_GLOBAL, STT_FUNC);

    // Include CFI for compiled method, if possible.
    if (cfi_info.get() != nullptr) {
      DCHECK(it->compiled_method_ != nullptr);

      // Copy in the FDE, if present
      const std::vector<uint8_t>* fde = it->compiled_method_->GetCFIInfo();
      if (fde != nullptr) {
        // Copy the information into cfi_info and then fix the address in the new copy.
        int cur_offset = cfi_info->size();
        cfi_info->insert(cfi_info->end(), fde->begin(), fde->end());

        bool is_64bit = *(reinterpret_cast<const uint32_t*>(fde->data())) == 0xffffffff;

        // Set the 'CIE_pointer' field.
        uint64_t CIE_pointer = cur_offset + (is_64bit ? 12 : 4);
        uint64_t offset_to_update = CIE_pointer;
        if (is_64bit) {
          (*cfi_info)[offset_to_update+0] = CIE_pointer;
          (*cfi_info)[offset_to_update+1] = CIE_pointer >> 8;
          (*cfi_info)[offset_to_update+2] = CIE_pointer >> 16;
          (*cfi_info)[offset_to_update+3] = CIE_pointer >> 24;
          (*cfi_info)[offset_to_update+4] = CIE_pointer >> 32;
          (*cfi_info)[offset_to_update+5] = CIE_pointer >> 40;
          (*cfi_info)[offset_to_update+6] = CIE_pointer >> 48;
          (*cfi_info)[offset_to_update+7] = CIE_pointer >> 56;
        } else {
          (*cfi_info)[offset_to_update+0] = CIE_pointer;
          (*cfi_info)[offset_to_update+1] = CIE_pointer >> 8;
          (*cfi_info)[offset_to_update+2] = CIE_pointer >> 16;
          (*cfi_info)[offset_to_update+3] = CIE_pointer >> 24;
        }

        // Set the 'initial_location' field.
        offset_to_update += is_64bit ? 8 : 4;
        if (is_64bit) {
          const uint64_t quick_code_start = it->low_pc_ + text_section_address;
          (*cfi_info)[offset_to_update+0] = quick_code_start;
          (*cfi_info)[offset_to_update+1] = quick_code_start >> 8;
          (*cfi_info)[offset_to_update+2] = quick_code_start >> 16;
          (*cfi_info)[offset_to_update+3] = quick_code_start >> 24;
          (*cfi_info)[offset_to_update+4] = quick_code_start >> 32;
          (*cfi_info)[offset_to_update+5] = quick_code_start >> 40;
          (*cfi_info)[offset_to_update+6] = quick_code_start >> 48;
          (*cfi_info)[offset_to_update+7] = quick_code_start >> 56;
        } else {
          const uint32_t quick_code_start = it->low_pc_ + text_section_address;
          (*cfi_info)[offset_to_update+0] = quick_code_start;
          (*cfi_info)[offset_to_update+1] = quick_code_start >> 8;
          (*cfi_info)[offset_to_update+2] = quick_code_start >> 16;
          (*cfi_info)[offset_to_update+3] = quick_code_start >> 24;
        }
      }
    }
  }

  bool hasCFI = (cfi_info.get() != nullptr);
  bool hasLineInfo = false;
  for (auto& dbg_info : oat_writer->GetCFIMethodInfo()) {
    if (dbg_info.dbgstream_ != nullptr &&
        !dbg_info.compiled_method_->GetSrcMappingTable().empty()) {
      hasLineInfo = true;
      break;
    }
  }

  if (hasLineInfo || hasCFI) {
    ElfRawSectionBuilder debug_info(".debug_info",     SHT_PROGBITS, 0, nullptr, 0, 1, 0);
    ElfRawSectionBuilder debug_abbrev(".debug_abbrev", SHT_PROGBITS, 0, nullptr, 0, 1, 0);
    ElfRawSectionBuilder debug_str(".debug_str",       SHT_PROGBITS, 0, nullptr, 0, 1, 0);
    ElfRawSectionBuilder debug_line(".debug_line",     SHT_PROGBITS, 0, nullptr, 0, 1, 0);

    FillInCFIInformation(oat_writer, debug_info.GetBuffer(),
                         debug_abbrev.GetBuffer(), debug_str.GetBuffer(),
                         hasLineInfo ? debug_line.GetBuffer() : nullptr,
                         text_section_address);

    builder->RegisterRawSection(debug_info);
    builder->RegisterRawSection(debug_abbrev);

    if (hasCFI) {
      ElfRawSectionBuilder eh_frame(".eh_frame",  SHT_PROGBITS, SHF_ALLOC, nullptr, 0, 4, 0);
      eh_frame.SetBuffer(std::move(*cfi_info.get()));
      builder->RegisterRawSection(eh_frame);
    }

    if (hasLineInfo) {
      builder->RegisterRawSection(debug_line);
    }

    builder->RegisterRawSection(debug_str);
  }
}

class LineTableGenerator FINAL : public Leb128Encoder {
 public:
  LineTableGenerator(int line_base, int line_range, int opcode_base,
                     std::vector<uint8_t>* data, uintptr_t current_address,
                     size_t current_line)
    : Leb128Encoder(data), line_base_(line_base), line_range_(line_range),
      opcode_base_(opcode_base), current_address_(current_address),
      current_line_(current_line) {}

  void PutDelta(unsigned delta_addr, int delta_line) {
    current_line_ += delta_line;
    current_address_ += delta_addr;

    if (delta_line >= line_base_ && delta_line < line_base_ + line_range_) {
      unsigned special_opcode = (delta_line - line_base_) +
                                (line_range_ * delta_addr) + opcode_base_;
      if (special_opcode <= 255) {
        PushByte(data_, special_opcode);
        return;
      }
    }

    // generate standart opcode for address advance
    if (delta_addr != 0) {
      PushByte(data_, DW_LNS_advance_pc);
      PushBackUnsigned(delta_addr);
    }

    // generate standart opcode for line delta
    if (delta_line != 0) {
      PushByte(data_, DW_LNS_advance_line);
      PushBackSigned(delta_line);
    }

    // generate standart opcode for new LTN entry
    PushByte(data_, DW_LNS_copy);
  }

  void SetAddr(uintptr_t addr) {
    if (current_address_ == addr) {
      return;
    }

    current_address_ = addr;

    PushByte(data_, 0);  // extended opcode:
    PushByte(data_, 1 + 4);  // length: opcode_size + address_size
    PushByte(data_, DW_LNE_set_address);
    PushWord(data_, addr);
  }

  void SetLine(unsigned line) {
    int delta_line = line - current_line_;
    if (delta_line) {
      current_line_ = line;
      PushByte(data_, DW_LNS_advance_line);
      PushBackSigned(delta_line);
    }
  }

  void SetFile(unsigned file_index) {
    PushByte(data_, DW_LNS_set_file);
    PushBackUnsigned(file_index);
  }

  void EndSequence() {
    // End of Line Table Program
    // 0(=ext), 1(len), DW_LNE_end_sequence
    PushByte(data_, 0);
    PushByte(data_, 1);
    PushByte(data_, DW_LNE_end_sequence);
  }

 private:
  const int line_base_;
  const int line_range_;
  const int opcode_base_;
  uintptr_t current_address_;
  size_t current_line_;

  DISALLOW_COPY_AND_ASSIGN(LineTableGenerator);
};

// TODO: rewriting it using DexFile::DecodeDebugInfo needs unneeded stuff.
static void GetLineInfoForJava(const uint8_t* dbgstream, const SrcMap& pc2dex,
                               SrcMap* result, uint32_t start_pc = 0) {
  if (dbgstream == nullptr) {
    return;
  }

  int adjopcode;
  uint32_t dex_offset = 0;
  uint32_t java_line = DecodeUnsignedLeb128(&dbgstream);

  // skip parameters
  for (uint32_t param_count = DecodeUnsignedLeb128(&dbgstream); param_count != 0; --param_count) {
    DecodeUnsignedLeb128(&dbgstream);
  }

  for (bool is_end = false; is_end == false; ) {
    uint8_t opcode = *dbgstream;
    dbgstream++;
    switch (opcode) {
    case DexFile::DBG_END_SEQUENCE:
      is_end = true;
      break;

    case DexFile::DBG_ADVANCE_PC:
      dex_offset += DecodeUnsignedLeb128(&dbgstream);
      break;

    case DexFile::DBG_ADVANCE_LINE:
      java_line += DecodeSignedLeb128(&dbgstream);
      break;

    case DexFile::DBG_START_LOCAL:
    case DexFile::DBG_START_LOCAL_EXTENDED:
      DecodeUnsignedLeb128(&dbgstream);
      DecodeUnsignedLeb128(&dbgstream);
      DecodeUnsignedLeb128(&dbgstream);

      if (opcode == DexFile::DBG_START_LOCAL_EXTENDED) {
        DecodeUnsignedLeb128(&dbgstream);
      }
      break;

    case DexFile::DBG_END_LOCAL:
    case DexFile::DBG_RESTART_LOCAL:
      DecodeUnsignedLeb128(&dbgstream);
      break;

    case DexFile::DBG_SET_PROLOGUE_END:
    case DexFile::DBG_SET_EPILOGUE_BEGIN:
    case DexFile::DBG_SET_FILE:
      break;

    default:
      adjopcode = opcode - DexFile::DBG_FIRST_SPECIAL;
      dex_offset += adjopcode / DexFile::DBG_LINE_RANGE;
      java_line += DexFile::DBG_LINE_BASE + (adjopcode % DexFile::DBG_LINE_RANGE);

      for (SrcMap::const_iterator found = pc2dex.FindByTo(dex_offset);
          found != pc2dex.end() && found->to_ == static_cast<int32_t>(dex_offset);
          found++) {
        result->push_back({found->from_ + start_pc, static_cast<int32_t>(java_line)});
      }
      break;
    }
  }
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
void ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::FillInCFIInformation(OatWriter* oat_writer,
                                          std::vector<uint8_t>* dbg_info,
                                          std::vector<uint8_t>* dbg_abbrev,
                                          std::vector<uint8_t>* dbg_str,
                                          std::vector<uint8_t>* dbg_line,
                                          uint32_t text_section_offset) {
  const std::vector<OatWriter::DebugInfo>& method_info = oat_writer->GetCFIMethodInfo();

  uint32_t producer_str_offset = PushStr(dbg_str, "Android dex2oat");

  // Create the debug_abbrev section with boilerplate information.
  // We only care about low_pc and high_pc right now for the compilation
  // unit and methods.

  // Tag 1: Compilation unit: DW_TAG_compile_unit.
  PushByte(dbg_abbrev, 1);
  PushByte(dbg_abbrev, DW_TAG_compile_unit);

  // There are children (the methods).
  PushByte(dbg_abbrev, DW_CHILDREN_yes);

  // DW_AT_producer DW_FORM_data1.
  // REVIEW: we can get rid of dbg_str section if
  // DW_FORM_string (immediate string) was used everywhere instead of
  // DW_FORM_strp (ref to string from .debug_str section).
  // DW_FORM_strp makes sense only if we reuse the strings.
  PushByte(dbg_abbrev, DW_AT_producer);
  PushByte(dbg_abbrev, DW_FORM_strp);

  // DW_LANG_Java DW_FORM_data1.
  PushByte(dbg_abbrev, DW_AT_language);
  PushByte(dbg_abbrev, DW_FORM_data1);

  // DW_AT_low_pc DW_FORM_addr.
  PushByte(dbg_abbrev, DW_AT_low_pc);
  PushByte(dbg_abbrev, DW_FORM_addr);

  // DW_AT_high_pc DW_FORM_addr.
  PushByte(dbg_abbrev, DW_AT_high_pc);
  PushByte(dbg_abbrev, DW_FORM_addr);

  if (dbg_line != nullptr) {
    // DW_AT_stmt_list DW_FORM_sec_offset.
    PushByte(dbg_abbrev, DW_AT_stmt_list);
    PushByte(dbg_abbrev, DW_FORM_sec_offset);
  }

  // End of DW_TAG_compile_unit.
  PushHalf(dbg_abbrev, 0);

  // Tag 2: Compilation unit: DW_TAG_subprogram.
  PushByte(dbg_abbrev, 2);
  PushByte(dbg_abbrev, DW_TAG_subprogram);

  // There are no children.
  PushByte(dbg_abbrev, DW_CHILDREN_no);

  // Name of the method.
  PushByte(dbg_abbrev, DW_AT_name);
  PushByte(dbg_abbrev, DW_FORM_strp);

  // DW_AT_low_pc DW_FORM_addr.
  PushByte(dbg_abbrev, DW_AT_low_pc);
  PushByte(dbg_abbrev, DW_FORM_addr);

  // DW_AT_high_pc DW_FORM_addr.
  PushByte(dbg_abbrev, DW_AT_high_pc);
  PushByte(dbg_abbrev, DW_FORM_addr);

  // End of DW_TAG_subprogram.
  PushHalf(dbg_abbrev, 0);

  // Start the debug_info section with the header information
  // 'unit_length' will be filled in later.
  int cunit_length = dbg_info->size();
  PushWord(dbg_info, 0);

  // 'version' - 3.
  PushHalf(dbg_info, 3);

  // Offset into .debug_abbrev section (always 0).
  PushWord(dbg_info, 0);

  // Address size: 4.
  PushByte(dbg_info, 4);

  // Start the description for the compilation unit.
  // This uses tag 1.
  PushByte(dbg_info, 1);

  // The producer is Android dex2oat.
  PushWord(dbg_info, producer_str_offset);

  // The language is Java.
  PushByte(dbg_info, DW_LANG_Java);

  // low_pc and high_pc.
  uint32_t cunit_low_pc = 0 - 1;
  uint32_t cunit_high_pc = 0;
  int cunit_low_pc_pos = dbg_info->size();
  PushWord(dbg_info, 0);
  PushWord(dbg_info, 0);

  if (dbg_line == nullptr) {
    for (size_t i = 0; i < method_info.size(); ++i) {
      const OatWriter::DebugInfo &dbg = method_info[i];

      cunit_low_pc = std::min(cunit_low_pc, dbg.low_pc_);
      cunit_high_pc = std::max(cunit_high_pc, dbg.high_pc_);

      // Start a new TAG: subroutine (2).
      PushByte(dbg_info, 2);

      // Enter name, low_pc, high_pc.
      PushWord(dbg_info, PushStr(dbg_str, dbg.method_name_));
      PushWord(dbg_info, dbg.low_pc_ + text_section_offset);
      PushWord(dbg_info, dbg.high_pc_ + text_section_offset);
    }
  } else {
    // TODO: in gdb info functions <regexp> - reports Java functions, but
    // source file is <unknown> because .debug_line is formed as one
    // compilation unit. To fix this it is possible to generate
    // a separate compilation unit for every distinct Java source.
    // Each of the these compilation units can have several non-adjacent
    // method ranges.

    // Line number table offset
    PushWord(dbg_info, dbg_line->size());

    size_t lnt_length = dbg_line->size();
    PushWord(dbg_line, 0);

    PushHalf(dbg_line, 4);  // LNT Version DWARF v4 => 4

    size_t lnt_hdr_length = dbg_line->size();
    PushWord(dbg_line, 0);  // TODO: 64-bit uses 8-byte here

    PushByte(dbg_line, 1);  // minimum_instruction_length (ubyte)
    PushByte(dbg_line, 1);  // maximum_operations_per_instruction (ubyte) = always 1
    PushByte(dbg_line, 1);  // default_is_stmt (ubyte)

    const int8_t LINE_BASE = -5;
    PushByte(dbg_line, LINE_BASE);  // line_base (sbyte)

    const uint8_t LINE_RANGE = 14;
    PushByte(dbg_line, LINE_RANGE);  // line_range (ubyte)

    const uint8_t OPCODE_BASE = 13;
    PushByte(dbg_line, OPCODE_BASE);  // opcode_base (ubyte)

    // Standard_opcode_lengths (array of ubyte).
    PushByte(dbg_line, 0); PushByte(dbg_line, 1); PushByte(dbg_line, 1);
    PushByte(dbg_line, 1); PushByte(dbg_line, 1); PushByte(dbg_line, 0);
    PushByte(dbg_line, 0); PushByte(dbg_line, 0); PushByte(dbg_line, 1);
    PushByte(dbg_line, 0); PushByte(dbg_line, 0); PushByte(dbg_line, 1);

    PushByte(dbg_line, 0);  // include_directories (sequence of path names) = EMPTY

    // File_names (sequence of file entries).
    std::unordered_map<const char*, size_t> files;
    for (size_t i = 0; i < method_info.size(); ++i) {
      const OatWriter::DebugInfo &dbg = method_info[i];
      // TODO: add package directory to the file name
      const char* file_name = dbg.src_file_name_ == nullptr ? "null" : dbg.src_file_name_;
      auto found = files.find(file_name);
      if (found == files.end()) {
        size_t file_index = 1 + files.size();
        files[file_name] = file_index;
        PushStr(dbg_line, file_name);
        PushByte(dbg_line, 0);  // include directory index = LEB128(0) - no directory
        PushByte(dbg_line, 0);  // modification time = LEB128(0) - NA
        PushByte(dbg_line, 0);  // file length = LEB128(0) - NA
      }
    }
    PushByte(dbg_line, 0);  // End of file_names.

    // Set lnt header length.
    UpdateWord(dbg_line, lnt_hdr_length, dbg_line->size() - lnt_hdr_length - 4);

    // Generate Line Number Program code, one long program for all methods.
    LineTableGenerator line_table_generator(LINE_BASE, LINE_RANGE, OPCODE_BASE,
                                            dbg_line, 0, 1);

    SrcMap pc2java_map;
    for (size_t i = 0; i < method_info.size(); ++i) {
      const OatWriter::DebugInfo &dbg = method_info[i];
      const char* file_name = (dbg.src_file_name_ == nullptr) ? "null" : dbg.src_file_name_;
      size_t file_index = files[file_name];
      DCHECK_NE(file_index, 0U) << file_name;

      cunit_low_pc = std::min(cunit_low_pc, dbg.low_pc_);
      cunit_high_pc = std::max(cunit_high_pc, dbg.high_pc_);

      // Start a new TAG: subroutine (2).
      PushByte(dbg_info, 2);

      // Enter name, low_pc, high_pc.
      PushWord(dbg_info, PushStr(dbg_str, dbg.method_name_));
      PushWord(dbg_info, dbg.low_pc_ + text_section_offset);
      PushWord(dbg_info, dbg.high_pc_ + text_section_offset);

      pc2java_map.clear();
      GetLineInfoForJava(dbg.dbgstream_, dbg.compiled_method_->GetSrcMappingTable(),
                         &pc2java_map, dbg.low_pc_);
      pc2java_map.DeltaFormat({dbg.low_pc_, 1}, dbg.high_pc_);

      line_table_generator.SetFile(file_index);
      line_table_generator.SetAddr(dbg.low_pc_ + text_section_offset);
      line_table_generator.SetLine(1);
      for (auto& src_map_elem : pc2java_map) {
        line_table_generator.PutDelta(src_map_elem.from_, src_map_elem.to_);
      }
    }

    // End Sequence should have the highest address set.
    line_table_generator.SetAddr(cunit_high_pc + text_section_offset);
    line_table_generator.EndSequence();

    // set lnt length
    UpdateWord(dbg_line, lnt_length, dbg_line->size() - lnt_length - 4);
  }

  // One byte terminator
  PushByte(dbg_info, 0);

  // Fill in cunit's low_pc and high_pc.
  UpdateWord(dbg_info, cunit_low_pc_pos, cunit_low_pc + text_section_offset);
  UpdateWord(dbg_info, cunit_low_pc_pos + 4, cunit_high_pc + text_section_offset);

  // We have now walked all the methods.  Fill in lengths.
  UpdateWord(dbg_info, cunit_length, dbg_info->size() - cunit_length - 4);
}

// Explicit instantiations
template class ElfWriterQuick<Elf32_Word, Elf32_Sword, Elf32_Addr, Elf32_Dyn,
                              Elf32_Sym, Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr>;
template class ElfWriterQuick<Elf64_Word, Elf64_Sword, Elf64_Addr, Elf64_Dyn,
                              Elf64_Sym, Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr>;

}  // namespace art
