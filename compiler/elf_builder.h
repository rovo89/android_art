/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_ELF_BUILDER_H_
#define ART_COMPILER_ELF_BUILDER_H_

#include "base/stl_util.h"
#include "buffered_output_stream.h"
#include "elf_utils.h"
#include "file_output_stream.h"
#include "instruction_set.h"

namespace art {

template <typename Elf_Word, typename Elf_Sword, typename Elf_Shdr>
class ElfSectionBuilder {
 public:
  ElfSectionBuilder(const std::string& sec_name, Elf_Word type, Elf_Word flags,
                    const ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> *link, Elf_Word info,
                    Elf_Word align, Elf_Word entsize) : name_(sec_name), link_(link) {
    memset(&section_, 0, sizeof(section_));
    section_.sh_type = type;
    section_.sh_flags = flags;
    section_.sh_info = info;
    section_.sh_addralign = align;
    section_.sh_entsize = entsize;
  }

  virtual ~ElfSectionBuilder() {}

  Elf_Shdr section_;
  Elf_Word section_index_ = 0;

  Elf_Word GetLink() {
    return (link_) ? link_->section_index_ : 0;
  }

  const std::string name_;

 protected:
  const ElfSectionBuilder* link_;
};

template <typename Elf_Word, typename Elf_Sword, typename Elf_Dyn, typename Elf_Shdr>
class ElfDynamicBuilder : public ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> {
 public:
  void AddDynamicTag(Elf_Sword tag, Elf_Word d_un) {
    if (tag == DT_NULL) {
      return;
    }
    dynamics_.push_back({nullptr, tag, d_un});
  }

  void AddDynamicTag(Elf_Sword tag, Elf_Word d_un,
                     ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>* section) {
    if (tag == DT_NULL) {
      return;
    }
    dynamics_.push_back({section, tag, d_un});
  }

  ElfDynamicBuilder(const std::string& sec_name,
                    ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> *link)
  : ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>(sec_name, SHT_DYNAMIC, SHF_ALLOC | SHF_ALLOC,
                                                     link, 0, kPageSize, sizeof(Elf_Dyn)) {}
  ~ElfDynamicBuilder() {}

  Elf_Word GetSize() {
    // Add 1 for the DT_NULL, 1 for DT_STRSZ, and 1 for DT_SONAME. All of
    // these must be added when we actually put the file together because
    // their values are very dependent on state.
    return dynamics_.size() + 3;
  }

  // Create the actual dynamic vector. strsz should be the size of the .dynstr
  // table and soname_off should be the offset of the soname in .dynstr.
  // Since niether can be found prior to final layout we will wait until here
  // to add them.
  std::vector<Elf_Dyn> GetDynamics(Elf_Word strsz, Elf_Word soname) {
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

 protected:
  struct ElfDynamicState {
    ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>* section_;
    Elf_Sword tag_;
    Elf_Word off_;
  };
  std::vector<ElfDynamicState> dynamics_;
};

template <typename Elf_Word, typename Elf_Sword, typename Elf_Shdr>
class ElfRawSectionBuilder : public ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> {
 public:
  ElfRawSectionBuilder(const std::string& sec_name, Elf_Word type, Elf_Word flags,
                       const ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>* link, Elf_Word info,
                       Elf_Word align, Elf_Word entsize)
    : ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>(sec_name, type, flags, link, info, align,
                                                       entsize) {}
  ~ElfRawSectionBuilder() {}
  std::vector<uint8_t>* GetBuffer() { return &buf_; }
  void SetBuffer(std::vector<uint8_t>&& buf) { buf_ = buf; }

 protected:
  std::vector<uint8_t> buf_;
};

template <typename Elf_Word, typename Elf_Sword, typename Elf_Shdr>
class ElfOatSectionBuilder : public ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> {
 public:
  ElfOatSectionBuilder(const std::string& sec_name, Elf_Word size, Elf_Word offset,
                       Elf_Word type, Elf_Word flags)
    : ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>(sec_name, type, flags, nullptr, 0, kPageSize,
                                                       0), offset_(offset), size_(size) {}
  ~ElfOatSectionBuilder() {}

  Elf_Word GetOffset() {
    return offset_;
  }

  Elf_Word GetSize() {
    return size_;
  }

 protected:
  // Offset of the content within the file.
  Elf_Word offset_;
  // Size of the content within the file.
  Elf_Word size_;
};

static inline constexpr uint8_t MakeStInfo(uint8_t binding, uint8_t type) {
  return ((binding) << 4) + ((type) & 0xf);
}

// from bionic
static inline unsigned elfhash(const char *_name) {
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

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr, typename Elf_Sym,
          typename Elf_Shdr>
class ElfSymtabBuilder : public ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> {
 public:
  // Add a symbol with given name to this symtab. The symbol refers to
  // 'relative_addr' within the given section and has the given attributes.
  void AddSymbol(const std::string& name,
                 const ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>* section,
                 Elf_Addr addr,
                 bool is_relative,
                 Elf_Word size,
                 uint8_t binding,
                 uint8_t type,
                 uint8_t other = 0) {
    CHECK(section);
    ElfSymtabBuilder::ElfSymbolState state {name, section, addr, size, is_relative,
                                            MakeStInfo(binding, type), other, 0};
    symbols_.push_back(state);
  }

  ElfSymtabBuilder(const std::string& sec_name, Elf_Word type,
                   const std::string& str_name, Elf_Word str_type, bool alloc)
  : ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>(sec_name, type, ((alloc) ? SHF_ALLOC : 0U),
                                                     &strtab_, 0, sizeof(Elf_Word),
                                                     sizeof(Elf_Sym)), str_name_(str_name),
                                                     str_type_(str_type),
                                                     strtab_(str_name,
                                                             str_type,
                                                             ((alloc) ? SHF_ALLOC : 0U),
                                                             nullptr, 0, 1, 1) {}
  ~ElfSymtabBuilder() {}

  std::vector<Elf_Word> GenerateHashContents() {
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

  std::string GenerateStrtab() {
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

  std::vector<Elf_Sym> GenerateSymtab() {
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

  Elf_Word GetSize() {
    // 1 is for the implicit NULL symbol.
    return symbols_.size() + 1;
  }

  ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>* GetStrTab() {
    return &strtab_;
  }

 protected:
  struct ElfSymbolState {
    const std::string name_;
    const ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>* section_;
    Elf_Addr addr_;
    Elf_Word size_;
    bool is_relative_;
    uint8_t info_;
    uint8_t other_;
    // Used during Write() to temporarially hold name index in the strtab.
    Elf_Word name_idx_;
  };

  // Information for the strsym for dynstr sections.
  const std::string str_name_;
  Elf_Word str_type_;
  // The symbols in the same order they will be in the symbol table.
  std::vector<ElfSymbolState> symbols_;
  ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> strtab_;
};

template <typename Elf_Word>
class ElfFilePiece {
 public:
  virtual ~ElfFilePiece() {}

  virtual bool Write(File* elf_file) {
    if (static_cast<off_t>(offset_) != lseek(elf_file->Fd(), offset_, SEEK_SET)) {
      PLOG(ERROR) << "Failed to seek to " << GetDescription() << " offset " << offset_ << " for "
          << elf_file->GetPath();
      return false;
    }

    return DoActualWrite(elf_file);
  }

  static bool Compare(ElfFilePiece* a, ElfFilePiece* b) {
    return a->offset_ < b->offset_;
  }

 protected:
  explicit ElfFilePiece(Elf_Word offset) : offset_(offset) {}

  virtual std::string GetDescription() = 0;
  virtual bool DoActualWrite(File* elf_file) = 0;

  Elf_Word offset_;
};

template <typename Elf_Word>
class ElfFileMemoryPiece : public ElfFilePiece<Elf_Word> {
 public:
  ElfFileMemoryPiece(const std::string& name, Elf_Word offset, const void* data, Elf_Word size)
      : ElfFilePiece<Elf_Word>(offset), dbg_name_(name), data_(data), size_(size) {}

  bool DoActualWrite(File* elf_file) OVERRIDE {
    DCHECK(data_ != nullptr || size_ == 0U) << dbg_name_ << " " << size_;

    if (!elf_file->WriteFully(data_, size_)) {
      PLOG(ERROR) << "Failed to write " << dbg_name_ << " for " << elf_file->GetPath();
      return false;
    }

    return true;
  }

  std::string GetDescription() OVERRIDE {
    return dbg_name_;
  }

 private:
  const std::string& dbg_name_;
  const void *data_;
  Elf_Word size_;
};

class CodeOutput {
 public:
  virtual void SetCodeOffset(size_t offset) = 0;
  virtual bool Write(OutputStream* out) = 0;
  virtual ~CodeOutput() {}
};

template <typename Elf_Word>
class ElfFileRodataPiece : public ElfFilePiece<Elf_Word> {
 public:
  ElfFileRodataPiece(Elf_Word offset, CodeOutput* output) : ElfFilePiece<Elf_Word>(offset),
      output_(output) {}

  bool DoActualWrite(File* elf_file) OVERRIDE {
    output_->SetCodeOffset(this->offset_);
    std::unique_ptr<BufferedOutputStream> output_stream(
        new BufferedOutputStream(new FileOutputStream(elf_file)));
    if (!output_->Write(output_stream.get())) {
      PLOG(ERROR) << "Failed to write .rodata and .text for " << elf_file->GetPath();
      return false;
    }

    return true;
  }

  std::string GetDescription() OVERRIDE {
    return ".rodata";
  }

 private:
  CodeOutput* output_;
};

template <typename Elf_Word>
class ElfFileOatTextPiece : public ElfFilePiece<Elf_Word> {
 public:
  ElfFileOatTextPiece(Elf_Word offset, CodeOutput* output) : ElfFilePiece<Elf_Word>(offset),
      output_(output) {}

  bool DoActualWrite(File* elf_file) OVERRIDE {
    // All data is written by the ElfFileRodataPiece right now, as the oat writer writes in one
    // piece. This is for future flexibility.
    UNUSED(output_);
    return true;
  }

  std::string GetDescription() OVERRIDE {
    return ".text";
  }

 private:
  CodeOutput* output_;
};

template <typename Elf_Word>
static bool WriteOutFile(const std::vector<ElfFilePiece<Elf_Word>*>& pieces, File* elf_file) {
  // TODO It would be nice if this checked for overlap.
  for (auto it = pieces.begin(); it != pieces.end(); ++it) {
    if (!(*it)->Write(elf_file)) {
      return false;
    }
  }
  return true;
}

template <typename Elf_Word, typename Elf_Shdr>
static inline constexpr Elf_Word NextOffset(const Elf_Shdr& cur, const Elf_Shdr& prev) {
  return RoundUp(prev.sh_size + prev.sh_offset, cur.sh_addralign);
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr, typename Elf_Dyn,
          typename Elf_Sym, typename Elf_Ehdr, typename Elf_Phdr, typename Elf_Shdr>
class ElfBuilder FINAL {
 public:
  ElfBuilder(CodeOutput* oat_writer,
             File* elf_file,
             InstructionSet isa,
             Elf_Word rodata_relative_offset,
             Elf_Word rodata_size,
             Elf_Word text_relative_offset,
             Elf_Word text_size,
             const bool add_symbols,
             bool debug = false)
    : oat_writer_(oat_writer),
      elf_file_(elf_file),
      add_symbols_(add_symbols),
      debug_logging_(debug),
      text_builder_(".text", text_size, text_relative_offset, SHT_PROGBITS,
                    SHF_ALLOC | SHF_EXECINSTR),
      rodata_builder_(".rodata", rodata_size, rodata_relative_offset, SHT_PROGBITS, SHF_ALLOC),
      dynsym_builder_(".dynsym", SHT_DYNSYM, ".dynstr", SHT_STRTAB, true),
      symtab_builder_(".symtab", SHT_SYMTAB, ".strtab", SHT_STRTAB, false),
      hash_builder_(".hash", SHT_HASH, SHF_ALLOC, &dynsym_builder_, 0, sizeof(Elf_Word),
                    sizeof(Elf_Word)),
      dynamic_builder_(".dynamic", &dynsym_builder_),
      shstrtab_builder_(".shstrtab", SHT_STRTAB, 0, NULL, 0, 1, 1) {
    SetupEhdr();
    SetupDynamic();
    SetupRequiredSymbols();
    SetISA(isa);
  }
  ~ElfBuilder() {}

  bool Init() {
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
    section_ptrs_.push_back(&dynsym_builder_.GetStrTab()->section_);
    AssignSectionStr(dynsym_builder_.GetStrTab(), &shstrtab_);
    dynsym_builder_.GetStrTab()->section_index_ = section_index_++;

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
    dynsym_builder_.GetStrTab()->section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                                 (dynsym_builder_.GetStrTab()->section_,
                                                  dynsym_builder_.section_);
    dynsym_builder_.GetStrTab()->section_.sh_addr = dynsym_builder_.GetStrTab()->section_.sh_offset;
    dynsym_builder_.GetStrTab()->section_.sh_size = dynstr_.size();
    dynsym_builder_.GetStrTab()->section_.sh_link = dynsym_builder_.GetStrTab()->GetLink();

    // Get the layout of the hash section
    hash_builder_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                       (hash_builder_.section_,
                                        dynsym_builder_.GetStrTab()->section_);
    hash_builder_.section_.sh_addr = hash_builder_.section_.sh_offset;
    hash_builder_.section_.sh_size = hash_.size() * sizeof(Elf_Word);
    hash_builder_.section_.sh_link = hash_builder_.GetLink();

    // Get the layout of the rodata section.
    rodata_builder_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                         (rodata_builder_.section_,
                                          hash_builder_.section_);
    rodata_builder_.section_.sh_addr = rodata_builder_.section_.sh_offset;
    rodata_builder_.section_.sh_size = rodata_builder_.GetSize();
    rodata_builder_.section_.sh_link = rodata_builder_.GetLink();

    // Get the layout of the text section.
    text_builder_.section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                       (text_builder_.section_, rodata_builder_.section_);
    text_builder_.section_.sh_addr = text_builder_.section_.sh_offset;
    text_builder_.section_.sh_size = text_builder_.GetSize();
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
      LOG(INFO) << "dynstr off=" << dynsym_builder_.GetStrTab()->section_.sh_offset
                << " dynstr size=" << dynsym_builder_.GetStrTab()->section_.sh_size;
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

  bool Write() {
    std::vector<ElfFilePiece<Elf_Word>*> pieces;
    Elf_Shdr prev = dynamic_builder_.section_;
    std::string strtab;

    if (IncludingDebugSymbols()) {
      // Setup .symtab
      section_ptrs_.push_back(&symtab_builder_.section_);
      AssignSectionStr(&symtab_builder_, &shstrtab_);
      symtab_builder_.section_index_ = section_index_++;

      // Setup .strtab
      section_ptrs_.push_back(&symtab_builder_.GetStrTab()->section_);
      AssignSectionStr(symtab_builder_.GetStrTab(), &shstrtab_);
      symtab_builder_.GetStrTab()->section_index_ = section_index_++;

      strtab = symtab_builder_.GenerateStrtab();
      if (debug_logging_) {
        LOG(INFO) << "strtab size (bytes)    =" << strtab.size()
                  << std::hex << " " << strtab.size();
        LOG(INFO) << "symtab size (elements) =" << symtab_builder_.GetSize()
                  << std::hex << " " << symtab_builder_.GetSize();
      }
    }

    // Setup all the other sections.
    for (ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> *builder = other_builders_.data(),
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
      symtab_builder_.GetStrTab()->section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>
                                                   (symtab_builder_.GetStrTab()->section_,
                                                    symtab_builder_.section_);
      symtab_builder_.GetStrTab()->section_.sh_addr = 0;
      symtab_builder_.GetStrTab()->section_.sh_size = strtab.size();
      symtab_builder_.GetStrTab()->section_.sh_link = symtab_builder_.GetStrTab()->GetLink();

      prev = symtab_builder_.GetStrTab()->section_;
      if (debug_logging_) {
        LOG(INFO) << "symtab off=" << symtab_builder_.section_.sh_offset
                  << " symtab size=" << symtab_builder_.section_.sh_size;
        LOG(INFO) << "strtab off=" << symtab_builder_.GetStrTab()->section_.sh_offset
                  << " strtab size=" << symtab_builder_.GetStrTab()->section_.sh_size;
      }
    }

    // Get the layout of the extra sections. (This will deal with the debug
    // sections if they are there)
    for (auto it = other_builders_.begin(); it != other_builders_.end(); ++it) {
      it->section_.sh_offset = NextOffset<Elf_Word, Elf_Shdr>(it->section_, prev);
      it->section_.sh_addr = 0;
      it->section_.sh_size = it->GetBuffer()->size();
      it->section_.sh_link = it->GetLink();

      // We postpone adding an ElfFilePiece to keep the order in "pieces."

      prev = it->section_;
      if (debug_logging_) {
        LOG(INFO) << it->name_ << " off=" << it->section_.sh_offset
                  << " size=" << it->section_.sh_size;
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
    pieces.push_back(new ElfFileMemoryPiece<Elf_Word>("Elf Header", 0, &elf_header_,
                                                      sizeof(elf_header_)));
    pieces.push_back(new ElfFileMemoryPiece<Elf_Word>("Program headers", PHDR_OFFSET,
                                                      &program_headers_, sizeof(program_headers_)));
    pieces.push_back(new ElfFileMemoryPiece<Elf_Word>(".dynamic",
                                                      dynamic_builder_.section_.sh_offset,
                                                      dynamic.data(),
                                                      dynamic_builder_.section_.sh_size));
    pieces.push_back(new ElfFileMemoryPiece<Elf_Word>(".dynsym", dynsym_builder_.section_.sh_offset,
                                                      dynsym.data(),
                                                      dynsym.size() * sizeof(Elf_Sym)));
    pieces.push_back(new ElfFileMemoryPiece<Elf_Word>(".dynstr",
                                                    dynsym_builder_.GetStrTab()->section_.sh_offset,
                                                    dynstr_.c_str(), dynstr_.size()));
    pieces.push_back(new ElfFileMemoryPiece<Elf_Word>(".hash", hash_builder_.section_.sh_offset,
                                                      hash_.data(),
                                                      hash_.size() * sizeof(Elf_Word)));
    pieces.push_back(new ElfFileRodataPiece<Elf_Word>(rodata_builder_.section_.sh_offset,
                                                      oat_writer_));
    pieces.push_back(new ElfFileOatTextPiece<Elf_Word>(text_builder_.section_.sh_offset,
                                                       oat_writer_));
    if (IncludingDebugSymbols()) {
      pieces.push_back(new ElfFileMemoryPiece<Elf_Word>(".symtab",
                                                        symtab_builder_.section_.sh_offset,
                                                        symtab.data(),
                                                        symtab.size() * sizeof(Elf_Sym)));
      pieces.push_back(new ElfFileMemoryPiece<Elf_Word>(".strtab",
                                                    symtab_builder_.GetStrTab()->section_.sh_offset,
                                                    strtab.c_str(), strtab.size()));
    }
    pieces.push_back(new ElfFileMemoryPiece<Elf_Word>(".shstrtab",
                                                      shstrtab_builder_.section_.sh_offset,
                                                      &shstrtab_[0], shstrtab_.size()));
    for (uint32_t i = 0; i < section_ptrs_.size(); ++i) {
      // Just add all the sections in induvidually since they are all over the
      // place on the heap/stack.
      Elf_Word cur_off = sections_offset + i * sizeof(Elf_Shdr);
      pieces.push_back(new ElfFileMemoryPiece<Elf_Word>("section table piece", cur_off,
                                                        section_ptrs_[i], sizeof(Elf_Shdr)));
    }

    // Postponed debug info.
    for (auto it = other_builders_.begin(); it != other_builders_.end(); ++it) {
      pieces.push_back(new ElfFileMemoryPiece<Elf_Word>(it->name_, it->section_.sh_offset,
                                                        it->GetBuffer()->data(),
                                                        it->GetBuffer()->size()));
    }

    if (!WriteOutFile(pieces)) {
      LOG(ERROR) << "Unable to write to file " << elf_file_->GetPath();

      STLDeleteElements(&pieces);  // Have to manually clean pieces.
      return false;
    }

    STLDeleteElements(&pieces);  // Have to manually clean pieces.
    return true;
  }

  // Adds the given raw section to the builder. This will copy it. The caller
  // is responsible for deallocating their copy.
  void RegisterRawSection(ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> bld) {
    other_builders_.push_back(bld);
  }

 private:
  CodeOutput* oat_writer_;
  File* elf_file_;
  const bool add_symbols_;
  const bool debug_logging_;

  bool fatal_error_ = false;

  // What phdr is.
  static const uint32_t PHDR_OFFSET = sizeof(Elf_Ehdr);
  enum : uint8_t {
    PH_PHDR     = 0,
        PH_LOAD_R__ = 1,
        PH_LOAD_R_X = 2,
        PH_LOAD_RW_ = 3,
        PH_DYNAMIC  = 4,
        PH_NUM      = 5,
  };
  static const uint32_t PHDR_SIZE = sizeof(Elf_Phdr) * PH_NUM;
  Elf_Phdr program_headers_[PH_NUM];

  Elf_Ehdr elf_header_;

  Elf_Shdr null_hdr_;
  std::string shstrtab_;
  uint32_t section_index_;
  std::string dynstr_;
  uint32_t dynstr_soname_offset_;
  std::vector<Elf_Shdr*> section_ptrs_;
  std::vector<Elf_Word> hash_;

 public:
  ElfOatSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> text_builder_;
  ElfOatSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> rodata_builder_;
  ElfSymtabBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Sym, Elf_Shdr> dynsym_builder_;
  ElfSymtabBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Sym, Elf_Shdr> symtab_builder_;
  ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> hash_builder_;
  ElfDynamicBuilder<Elf_Word, Elf_Sword, Elf_Dyn, Elf_Shdr> dynamic_builder_;
  ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> shstrtab_builder_;
  std::vector<ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr>> other_builders_;

 private:
  void SetISA(InstructionSet isa) {
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

  void SetupEhdr() {
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

  // Sets up a bunch of the required Dynamic Section entries.
  // Namely it will initialize all the mandatory ones that it can.
  // Specifically:
  // DT_HASH
  // DT_STRTAB
  // DT_SYMTAB
  // DT_SYMENT
  //
  // Some such as DT_SONAME, DT_STRSZ and DT_NULL will be put in later.
  void SetupDynamic() {
    dynamic_builder_.AddDynamicTag(DT_HASH, 0, &hash_builder_);
    dynamic_builder_.AddDynamicTag(DT_STRTAB, 0, dynsym_builder_.GetStrTab());
    dynamic_builder_.AddDynamicTag(DT_SYMTAB, 0, &dynsym_builder_);
    dynamic_builder_.AddDynamicTag(DT_SYMENT, sizeof(Elf_Sym));
  }

  // Sets up the basic dynamic symbols that are needed, namely all those we
  // can know already.
  //
  // Specifically adds:
  // oatdata
  // oatexec
  // oatlastword
  void SetupRequiredSymbols() {
    dynsym_builder_.AddSymbol("oatdata", &rodata_builder_, 0, true,
                              rodata_builder_.GetSize(), STB_GLOBAL, STT_OBJECT);
    dynsym_builder_.AddSymbol("oatexec", &text_builder_, 0, true,
                              text_builder_.GetSize(), STB_GLOBAL, STT_OBJECT);
    dynsym_builder_.AddSymbol("oatlastword", &text_builder_, text_builder_.GetSize() - 4,
                              true, 4, STB_GLOBAL, STT_OBJECT);
  }

  void AssignSectionStr(ElfSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> *builder,
                        std::string* strtab) {
    builder->section_.sh_name = strtab->size();
    *strtab += builder->name_;
    *strtab += '\0';
    if (debug_logging_) {
      LOG(INFO) << "adding section name \"" << builder->name_ << "\" "
                << "to shstrtab at offset " << builder->section_.sh_name;
    }
  }


  // Write each of the pieces out to the file.
  bool WriteOutFile(const std::vector<ElfFilePiece<Elf_Word>*>& pieces) {
    for (auto it = pieces.begin(); it != pieces.end(); ++it) {
      if (!(*it)->Write(elf_file_)) {
        return false;
      }
    }
    return true;
  }

  bool IncludingDebugSymbols() { return add_symbols_ && symtab_builder_.GetSize() > 1; }
};

}  // namespace art

#endif  // ART_COMPILER_ELF_BUILDER_H_
