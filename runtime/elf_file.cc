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

#include "base/logging.h"
#include "base/stl_util.h"
#include "utils.h"

namespace art {

ElfFile::ElfFile(File* file, bool writable, bool program_header_only)
  : file_(file),
    writable_(writable),
    program_header_only_(program_header_only),
    header_(NULL),
    base_address_(NULL),
    program_headers_start_(NULL),
    section_headers_start_(NULL),
    dynamic_program_header_(NULL),
    dynamic_section_start_(NULL),
    symtab_section_start_(NULL),
    dynsym_section_start_(NULL),
    strtab_section_start_(NULL),
    dynstr_section_start_(NULL),
    hash_section_start_(NULL),
    symtab_symbol_table_(NULL),
    dynsym_symbol_table_(NULL) {
  CHECK(file != NULL);
}

ElfFile* ElfFile::Open(File* file, bool writable, bool program_header_only,
                       std::string* error_msg) {
  UniquePtr<ElfFile> elf_file(new ElfFile(file, writable, program_header_only));
  if (!elf_file->Setup(error_msg)) {
    return nullptr;
  }
  return elf_file.release();
}

bool ElfFile::Setup(std::string* error_msg) {
  int prot;
  int flags;
  if (writable_) {
    prot = PROT_READ | PROT_WRITE;
    flags = MAP_SHARED;
  } else {
    prot = PROT_READ;
    flags = MAP_PRIVATE;
  }
  int64_t temp_file_length = file_->GetLength();
  if (temp_file_length < 0) {
    errno = -temp_file_length;
    *error_msg = StringPrintf("Failed to get length of file: '%s' fd=%d: %s",
                              file_->GetPath().c_str(), file_->Fd(), strerror(errno));
    return false;
  }
  size_t file_length = static_cast<size_t>(temp_file_length);
  if (file_length < sizeof(Elf32_Ehdr)) {
    *error_msg = StringPrintf("File size of %zd bytes not large enough to contain ELF header of "
                              "%zd bytes: '%s'", file_length, sizeof(Elf32_Ehdr),
                              file_->GetPath().c_str());
    return false;
  }

  if (program_header_only_) {
    // first just map ELF header to get program header size information
    size_t elf_header_size = sizeof(Elf32_Ehdr);
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
                                sizeof(Elf32_Ehdr), file_->GetPath().c_str());
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

  // Either way, the program header is relative to the elf header
  program_headers_start_ = Begin() + GetHeader().e_phoff;

  if (!program_header_only_) {
    // Setup section headers.
    section_headers_start_ = Begin() + GetHeader().e_shoff;

    // Find .dynamic section info from program header
    dynamic_program_header_ = FindProgamHeaderByType(PT_DYNAMIC);
    if (dynamic_program_header_ == NULL) {
      *error_msg = StringPrintf("Failed to find PT_DYNAMIC program header in ELF file: '%s'",
                                file_->GetPath().c_str());
      return false;
    }

    dynamic_section_start_
        = reinterpret_cast<Elf32_Dyn*>(Begin() + GetDynamicProgramHeader().p_offset);

    // Find other sections from section headers
    for (Elf32_Word i = 0; i < GetSectionHeaderNum(); i++) {
      Elf32_Shdr& section_header = GetSectionHeader(i);
      byte* section_addr = Begin() + section_header.sh_offset;
      switch (section_header.sh_type) {
        case SHT_SYMTAB: {
          symtab_section_start_ = reinterpret_cast<Elf32_Sym*>(section_addr);
          break;
        }
        case SHT_DYNSYM: {
          dynsym_section_start_ = reinterpret_cast<Elf32_Sym*>(section_addr);
          break;
        }
        case SHT_STRTAB: {
          // TODO: base these off of sh_link from .symtab and .dynsym above
          if ((section_header.sh_flags & SHF_ALLOC) != 0) {
            dynstr_section_start_ = reinterpret_cast<char*>(section_addr);
          } else {
            strtab_section_start_ = reinterpret_cast<char*>(section_addr);
          }
          break;
        }
        case SHT_DYNAMIC: {
          if (reinterpret_cast<byte*>(dynamic_section_start_) != section_addr) {
            LOG(WARNING) << "Failed to find matching SHT_DYNAMIC for PT_DYNAMIC in "
                         << file_->GetPath() << ": " << std::hex
                         << reinterpret_cast<void*>(dynamic_section_start_)
                         << " != " << reinterpret_cast<void*>(section_addr);
            return false;
          }
          break;
        }
        case SHT_HASH: {
          hash_section_start_ = reinterpret_cast<Elf32_Word*>(section_addr);
          break;
        }
      }
    }
  }
  return true;
}

ElfFile::~ElfFile() {
  STLDeleteElements(&segments_);
  delete symtab_symbol_table_;
  delete dynsym_symbol_table_;
}

bool ElfFile::SetMap(MemMap* map, std::string* error_msg) {
  if (map == NULL) {
    // MemMap::Open should have already set an error.
    DCHECK(!error_msg->empty());
    return false;
  }
  map_.reset(map);
  CHECK(map_.get() != NULL) << file_->GetPath();
  CHECK(map_->Begin() != NULL) << file_->GetPath();

  header_ = reinterpret_cast<Elf32_Ehdr*>(map_->Begin());
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
  if (ELFCLASS32 != header_->e_ident[EI_CLASS]) {
    *error_msg = StringPrintf("Failed to find expected EI_CLASS value %d in %s, found %d",
                              ELFCLASS32,
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
                              header_->e_entry);
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
      *error_msg = StringPrintf("Failed to find e_phoff value %d less than %d in %s",
                                header_->e_phoff,
                                Size(),
                                file_->GetPath().c_str());
      return false;
    }
    if (header_->e_shoff >= Size()) {
      *error_msg = StringPrintf("Failed to find e_shoff value %d less than %d in %s",
                                header_->e_shoff,
                                Size(),
                                file_->GetPath().c_str());
      return false;
    }
  }
  return true;
}


Elf32_Ehdr& ElfFile::GetHeader() const {
  CHECK(header_ != NULL);
  return *header_;
}

byte* ElfFile::GetProgramHeadersStart() const {
  CHECK(program_headers_start_ != NULL);
  return program_headers_start_;
}

byte* ElfFile::GetSectionHeadersStart() const {
  CHECK(section_headers_start_ != NULL);
  return section_headers_start_;
}

Elf32_Phdr& ElfFile::GetDynamicProgramHeader() const {
  CHECK(dynamic_program_header_ != NULL);
  return *dynamic_program_header_;
}

Elf32_Dyn* ElfFile::GetDynamicSectionStart() const {
  CHECK(dynamic_section_start_ != NULL);
  return dynamic_section_start_;
}

Elf32_Sym* ElfFile::GetSymbolSectionStart(Elf32_Word section_type) const {
  CHECK(IsSymbolSectionType(section_type)) << file_->GetPath() << " " << section_type;
  Elf32_Sym* symbol_section_start;
  switch (section_type) {
    case SHT_SYMTAB: {
      symbol_section_start = symtab_section_start_;
      break;
    }
    case SHT_DYNSYM: {
      symbol_section_start = dynsym_section_start_;
      break;
    }
    default: {
      LOG(FATAL) << section_type;
      symbol_section_start = NULL;
    }
  }
  CHECK(symbol_section_start != NULL);
  return symbol_section_start;
}

const char* ElfFile::GetStringSectionStart(Elf32_Word section_type) const {
  CHECK(IsSymbolSectionType(section_type)) << file_->GetPath() << " " << section_type;
  const char* string_section_start;
  switch (section_type) {
    case SHT_SYMTAB: {
      string_section_start = strtab_section_start_;
      break;
    }
    case SHT_DYNSYM: {
      string_section_start = dynstr_section_start_;
      break;
    }
    default: {
      LOG(FATAL) << section_type;
      string_section_start = NULL;
    }
  }
  CHECK(string_section_start != NULL);
  return string_section_start;
}

const char* ElfFile::GetString(Elf32_Word section_type, Elf32_Word i) const {
  CHECK(IsSymbolSectionType(section_type)) << file_->GetPath() << " " << section_type;
  if (i == 0) {
    return NULL;
  }
  const char* string_section_start = GetStringSectionStart(section_type);
  const char* string = string_section_start + i;
  return string;
}

Elf32_Word* ElfFile::GetHashSectionStart() const {
  CHECK(hash_section_start_ != NULL);
  return hash_section_start_;
}

Elf32_Word ElfFile::GetHashBucketNum() const {
  return GetHashSectionStart()[0];
}

Elf32_Word ElfFile::GetHashChainNum() const {
  return GetHashSectionStart()[1];
}

Elf32_Word ElfFile::GetHashBucket(size_t i) const {
  CHECK_LT(i, GetHashBucketNum());
  // 0 is nbucket, 1 is nchain
  return GetHashSectionStart()[2 + i];
}

Elf32_Word ElfFile::GetHashChain(size_t i) const {
  CHECK_LT(i, GetHashChainNum());
  // 0 is nbucket, 1 is nchain, & chains are after buckets
  return GetHashSectionStart()[2 + GetHashBucketNum() + i];
}

Elf32_Word ElfFile::GetProgramHeaderNum() const {
  return GetHeader().e_phnum;
}

Elf32_Phdr& ElfFile::GetProgramHeader(Elf32_Word i) const {
  CHECK_LT(i, GetProgramHeaderNum()) << file_->GetPath();
  byte* program_header = GetProgramHeadersStart() + (i * GetHeader().e_phentsize);
  CHECK_LT(program_header, End()) << file_->GetPath();
  return *reinterpret_cast<Elf32_Phdr*>(program_header);
}

Elf32_Phdr* ElfFile::FindProgamHeaderByType(Elf32_Word type) const {
  for (Elf32_Word i = 0; i < GetProgramHeaderNum(); i++) {
    Elf32_Phdr& program_header = GetProgramHeader(i);
    if (program_header.p_type == type) {
      return &program_header;
    }
  }
  return NULL;
}

Elf32_Word ElfFile::GetSectionHeaderNum() const {
  return GetHeader().e_shnum;
}

Elf32_Shdr& ElfFile::GetSectionHeader(Elf32_Word i) const {
  // Can only access arbitrary sections when we have the whole file, not just program header.
  // Even if we Load(), it doesn't bring in all the sections.
  CHECK(!program_header_only_) << file_->GetPath();
  CHECK_LT(i, GetSectionHeaderNum()) << file_->GetPath();
  byte* section_header = GetSectionHeadersStart() + (i * GetHeader().e_shentsize);
  CHECK_LT(section_header, End()) << file_->GetPath();
  return *reinterpret_cast<Elf32_Shdr*>(section_header);
}

Elf32_Shdr* ElfFile::FindSectionByType(Elf32_Word type) const {
  // Can only access arbitrary sections when we have the whole file, not just program header.
  // We could change this to switch on known types if they were detected during loading.
  CHECK(!program_header_only_) << file_->GetPath();
  for (Elf32_Word i = 0; i < GetSectionHeaderNum(); i++) {
    Elf32_Shdr& section_header = GetSectionHeader(i);
    if (section_header.sh_type == type) {
      return &section_header;
    }
  }
  return NULL;
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

Elf32_Shdr& ElfFile::GetSectionNameStringSection() const {
  return GetSectionHeader(GetHeader().e_shstrndx);
}

const byte* ElfFile::FindDynamicSymbolAddress(const std::string& symbol_name) const {
  Elf32_Word hash = elfhash(symbol_name.c_str());
  Elf32_Word bucket_index = hash % GetHashBucketNum();
  Elf32_Word symbol_and_chain_index = GetHashBucket(bucket_index);
  while (symbol_and_chain_index != 0 /* STN_UNDEF */) {
    Elf32_Sym& symbol = GetSymbol(SHT_DYNSYM, symbol_and_chain_index);
    const char* name = GetString(SHT_DYNSYM, symbol.st_name);
    if (symbol_name == name) {
      return base_address_ + symbol.st_value;
    }
    symbol_and_chain_index = GetHashChain(symbol_and_chain_index);
  }
  return NULL;
}

bool ElfFile::IsSymbolSectionType(Elf32_Word section_type) {
  return ((section_type == SHT_SYMTAB) || (section_type == SHT_DYNSYM));
}

Elf32_Word ElfFile::GetSymbolNum(Elf32_Shdr& section_header) const {
  CHECK(IsSymbolSectionType(section_header.sh_type))
      << file_->GetPath() << " " << section_header.sh_type;
  CHECK_NE(0U, section_header.sh_entsize) << file_->GetPath();
  return section_header.sh_size / section_header.sh_entsize;
}

Elf32_Sym& ElfFile::GetSymbol(Elf32_Word section_type,
                              Elf32_Word i) const {
  return *(GetSymbolSectionStart(section_type) + i);
}

ElfFile::SymbolTable** ElfFile::GetSymbolTable(Elf32_Word section_type) {
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
      return NULL;
    }
  }
}

Elf32_Sym* ElfFile::FindSymbolByName(Elf32_Word section_type,
                                     const std::string& symbol_name,
                                     bool build_map) {
  CHECK(!program_header_only_) << file_->GetPath();
  CHECK(IsSymbolSectionType(section_type)) << file_->GetPath() << " " << section_type;

  SymbolTable** symbol_table = GetSymbolTable(section_type);
  if (*symbol_table != NULL || build_map) {
    if (*symbol_table == NULL) {
      DCHECK(build_map);
      *symbol_table = new SymbolTable;
      Elf32_Shdr* symbol_section = FindSectionByType(section_type);
      CHECK(symbol_section != NULL) << file_->GetPath();
      Elf32_Shdr& string_section = GetSectionHeader(symbol_section->sh_link);
      for (uint32_t i = 0; i < GetSymbolNum(*symbol_section); i++) {
        Elf32_Sym& symbol = GetSymbol(section_type, i);
        unsigned char type = ELF32_ST_TYPE(symbol.st_info);
        if (type == STT_NOTYPE) {
          continue;
        }
        const char* name = GetString(string_section, symbol.st_name);
        if (name == NULL) {
          continue;
        }
        std::pair<SymbolTable::iterator, bool> result =
            (*symbol_table)->insert(std::make_pair(name, &symbol));
        if (!result.second) {
          // If a duplicate, make sure it has the same logical value. Seen on x86.
          CHECK_EQ(symbol.st_value, result.first->second->st_value);
          CHECK_EQ(symbol.st_size, result.first->second->st_size);
          CHECK_EQ(symbol.st_info, result.first->second->st_info);
          CHECK_EQ(symbol.st_other, result.first->second->st_other);
          CHECK_EQ(symbol.st_shndx, result.first->second->st_shndx);
        }
      }
    }
    CHECK(*symbol_table != NULL);
    SymbolTable::const_iterator it = (*symbol_table)->find(symbol_name);
    if (it == (*symbol_table)->end()) {
      return NULL;
    }
    return it->second;
  }

  // Fall back to linear search
  Elf32_Shdr* symbol_section = FindSectionByType(section_type);
  CHECK(symbol_section != NULL) << file_->GetPath();
  Elf32_Shdr& string_section = GetSectionHeader(symbol_section->sh_link);
  for (uint32_t i = 0; i < GetSymbolNum(*symbol_section); i++) {
    Elf32_Sym& symbol = GetSymbol(section_type, i);
    const char* name = GetString(string_section, symbol.st_name);
    if (name == NULL) {
      continue;
    }
    if (symbol_name == name) {
      return &symbol;
    }
  }
  return NULL;
}

Elf32_Addr ElfFile::FindSymbolAddress(Elf32_Word section_type,
                                      const std::string& symbol_name,
                                      bool build_map) {
  Elf32_Sym* symbol = FindSymbolByName(section_type, symbol_name, build_map);
  if (symbol == NULL) {
    return 0;
  }
  return symbol->st_value;
}

const char* ElfFile::GetString(Elf32_Shdr& string_section, Elf32_Word i) const {
  CHECK(!program_header_only_) << file_->GetPath();
  // TODO: remove this static_cast from enum when using -std=gnu++0x
  CHECK_EQ(static_cast<Elf32_Word>(SHT_STRTAB), string_section.sh_type) << file_->GetPath();
  CHECK_LT(i, string_section.sh_size) << file_->GetPath();
  if (i == 0) {
    return NULL;
  }
  byte* strings = Begin() + string_section.sh_offset;
  byte* string = strings + i;
  CHECK_LT(string, End()) << file_->GetPath();
  return reinterpret_cast<const char*>(string);
}

Elf32_Word ElfFile::GetDynamicNum() const {
  return GetDynamicProgramHeader().p_filesz / sizeof(Elf32_Dyn);
}

Elf32_Dyn& ElfFile::GetDynamic(Elf32_Word i) const {
  CHECK_LT(i, GetDynamicNum()) << file_->GetPath();
  return *(GetDynamicSectionStart() + i);
}

Elf32_Word ElfFile::FindDynamicValueByType(Elf32_Sword type) const {
  for (Elf32_Word i = 0; i < GetDynamicNum(); i++) {
    Elf32_Dyn& elf_dyn = GetDynamic(i);
    if (elf_dyn.d_tag == type) {
      return elf_dyn.d_un.d_val;
    }
  }
  return 0;
}

Elf32_Rel* ElfFile::GetRelSectionStart(Elf32_Shdr& section_header) const {
  CHECK(SHT_REL == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  return reinterpret_cast<Elf32_Rel*>(Begin() + section_header.sh_offset);
}

Elf32_Word ElfFile::GetRelNum(Elf32_Shdr& section_header) const {
  CHECK(SHT_REL == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  CHECK_NE(0U, section_header.sh_entsize) << file_->GetPath();
  return section_header.sh_size / section_header.sh_entsize;
}

Elf32_Rel& ElfFile::GetRel(Elf32_Shdr& section_header, Elf32_Word i) const {
  CHECK(SHT_REL == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  CHECK_LT(i, GetRelNum(section_header)) << file_->GetPath();
  return *(GetRelSectionStart(section_header) + i);
}

Elf32_Rela* ElfFile::GetRelaSectionStart(Elf32_Shdr& section_header) const {
  CHECK(SHT_RELA == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  return reinterpret_cast<Elf32_Rela*>(Begin() + section_header.sh_offset);
}

Elf32_Word ElfFile::GetRelaNum(Elf32_Shdr& section_header) const {
  CHECK(SHT_RELA == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  return section_header.sh_size / section_header.sh_entsize;
}

Elf32_Rela& ElfFile::GetRela(Elf32_Shdr& section_header, Elf32_Word i) const {
  CHECK(SHT_RELA == section_header.sh_type) << file_->GetPath() << " " << section_header.sh_type;
  CHECK_LT(i, GetRelaNum(section_header)) << file_->GetPath();
  return *(GetRelaSectionStart(section_header) + i);
}

// Base on bionic phdr_table_get_load_size
size_t ElfFile::GetLoadedSize() const {
  Elf32_Addr min_vaddr = 0xFFFFFFFFu;
  Elf32_Addr max_vaddr = 0x00000000u;
  for (Elf32_Word i = 0; i < GetProgramHeaderNum(); i++) {
    Elf32_Phdr& program_header = GetProgramHeader(i);
    if (program_header.p_type != PT_LOAD) {
      continue;
    }
    Elf32_Addr begin_vaddr = program_header.p_vaddr;
    if (begin_vaddr < min_vaddr) {
       min_vaddr = begin_vaddr;
    }
    Elf32_Addr end_vaddr = program_header.p_vaddr + program_header.p_memsz;
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

bool ElfFile::Load(bool executable, std::string* error_msg) {
  CHECK(program_header_only_) << file_->GetPath();
  for (Elf32_Word i = 0; i < GetProgramHeaderNum(); i++) {
    Elf32_Phdr& program_header = GetProgramHeader(i);

    // Record .dynamic header information for later use
    if (program_header.p_type == PT_DYNAMIC) {
      dynamic_program_header_ = &program_header;
      continue;
    }

    // Not something to load, move on.
    if (program_header.p_type != PT_LOAD) {
      continue;
    }

    // Found something to load.

    // If p_vaddr is zero, it must be the first loadable segment,
    // since they must be in order.  Since it is zero, there isn't a
    // specific address requested, so first request a contiguous chunk
    // of required size for all segments, but with no
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
    if (program_header.p_vaddr == 0) {
      std::string reservation_name("ElfFile reservation for ");
      reservation_name += file_->GetPath();
      UniquePtr<MemMap> reserve(MemMap::MapAnonymous(reservation_name.c_str(),
                                                     NULL, GetLoadedSize(), PROT_NONE, false,
                                                     error_msg));
      if (reserve.get() == nullptr) {
        *error_msg = StringPrintf("Failed to allocate %s: %s",
                                  reservation_name.c_str(), error_msg->c_str());
        return false;
      }
      base_address_ = reserve->Begin();
      segments_.push_back(reserve.release());
    }
    // empty segment, nothing to map
    if (program_header.p_memsz == 0) {
      continue;
    }
    byte* p_vaddr = base_address_ + program_header.p_vaddr;
    int prot = 0;
    if (executable && ((program_header.p_flags & PF_X) != 0)) {
      prot |= PROT_EXEC;
    }
    if ((program_header.p_flags & PF_W) != 0) {
      prot |= PROT_WRITE;
    }
    if ((program_header.p_flags & PF_R) != 0) {
      prot |= PROT_READ;
    }
    int flags = MAP_FIXED;
    if (writable_) {
      prot |= PROT_WRITE;
      flags |= MAP_SHARED;
    } else {
      flags |= MAP_PRIVATE;
    }
    if (file_length < (program_header.p_offset + program_header.p_memsz)) {
      *error_msg = StringPrintf("File size of %zd bytes not large enough to contain ELF segment "
                                "%d of %d bytes: '%s'", file_length, i,
                                program_header.p_offset + program_header.p_memsz,
                                file_->GetPath().c_str());
      return false;
    }
    UniquePtr<MemMap> segment(MemMap::MapFileAtAddress(p_vaddr,
                                                       program_header.p_memsz,
                                                       prot, flags, file_->Fd(),
                                                       program_header.p_offset,
                                                       true,
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

  // Now that we are done loading, .dynamic should be in memory to find .dynstr, .dynsym, .hash
  dynamic_section_start_
      = reinterpret_cast<Elf32_Dyn*>(base_address_ + GetDynamicProgramHeader().p_vaddr);
  for (Elf32_Word i = 0; i < GetDynamicNum(); i++) {
    Elf32_Dyn& elf_dyn = GetDynamic(i);
    byte* d_ptr = base_address_ + elf_dyn.d_un.d_ptr;
    switch (elf_dyn.d_tag) {
      case DT_HASH: {
        if (!ValidPointer(d_ptr)) {
          *error_msg = StringPrintf("DT_HASH value %p does not refer to a loaded ELF segment of %s",
                                    d_ptr, file_->GetPath().c_str());
          return false;
        }
        hash_section_start_ = reinterpret_cast<Elf32_Word*>(d_ptr);
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
        dynsym_section_start_ = reinterpret_cast<Elf32_Sym*>(d_ptr);
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

  return true;
}

bool ElfFile::ValidPointer(const byte* start) const {
  for (size_t i = 0; i < segments_.size(); ++i) {
    const MemMap* segment = segments_[i];
    if (segment->Begin() <= start && start < segment->End()) {
      return true;
    }
  }
  return false;
}

}  // namespace art
