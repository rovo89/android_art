/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <vector>

#include "arch/instruction_set.h"
#include "base/bit_utils.h"
#include "base/unix_file/fd_file.h"
#include "buffered_output_stream.h"
#include "elf_utils.h"
#include "file_output_stream.h"

namespace art {

class CodeOutput {
 public:
  virtual bool Write(OutputStream* out) = 0;
  virtual ~CodeOutput() {}
};

// Writes ELF file.
// The main complication is that the sections often want to reference
// each other.  We solve this by writing the ELF file in two stages:
//  * Sections are asked about their size, and overall layout is calculated.
//  * Sections do the actual writes which may use offsets of other sections.
template <typename ElfTypes>
class ElfBuilder FINAL {
 public:
  using Elf_Addr = typename ElfTypes::Addr;
  using Elf_Off = typename ElfTypes::Off;
  using Elf_Word = typename ElfTypes::Word;
  using Elf_Sword = typename ElfTypes::Sword;
  using Elf_Ehdr = typename ElfTypes::Ehdr;
  using Elf_Shdr = typename ElfTypes::Shdr;
  using Elf_Sym = typename ElfTypes::Sym;
  using Elf_Phdr = typename ElfTypes::Phdr;
  using Elf_Dyn = typename ElfTypes::Dyn;

  // Base class of all sections.
  class Section {
   public:
    Section(const std::string& name, Elf_Word type, Elf_Word flags,
            const Section* link, Elf_Word info, Elf_Word align, Elf_Word entsize)
        : header_(), section_index_(0), name_(name), link_(link) {
      header_.sh_type = type;
      header_.sh_flags = flags;
      header_.sh_info = info;
      header_.sh_addralign = align;
      header_.sh_entsize = entsize;
    }
    virtual ~Section() {}

    // Returns the size of the content of this section.  It is used to
    // calculate file offsets of all sections before doing any writes.
    virtual Elf_Word GetSize() const = 0;

    // Write the content of this section to the given file.
    // This must write exactly the number of bytes returned by GetSize().
    // Offsets of all sections are known when this method is called.
    virtual bool Write(File* elf_file) = 0;

    Elf_Word GetLink() const {
      return (link_ != nullptr) ? link_->GetSectionIndex() : 0;
    }

    const Elf_Shdr* GetHeader() const {
      return &header_;
    }

    Elf_Shdr* GetHeader() {
      return &header_;
    }

    Elf_Word GetSectionIndex() const {
      DCHECK_NE(section_index_, 0u);
      return section_index_;
    }

    void SetSectionIndex(Elf_Word section_index) {
      section_index_ = section_index;
    }

    const std::string& GetName() const {
      return name_;
    }

   private:
    Elf_Shdr header_;
    Elf_Word section_index_;
    const std::string name_;
    const Section* const link_;

    DISALLOW_COPY_AND_ASSIGN(Section);
  };

  // Writer of .dynamic section.
  class DynamicSection FINAL : public Section {
   public:
    void AddDynamicTag(Elf_Sword tag, Elf_Word value, const Section* section) {
      DCHECK_NE(tag, static_cast<Elf_Sword>(DT_NULL));
      dynamics_.push_back({tag, value, section});
    }

    DynamicSection(const std::string& name, Section* link)
        : Section(name, SHT_DYNAMIC, SHF_ALLOC,
                  link, 0, kPageSize, sizeof(Elf_Dyn)) {}

    Elf_Word GetSize() const OVERRIDE {
      return (dynamics_.size() + 1 /* DT_NULL */) * sizeof(Elf_Dyn);
    }

    bool Write(File* elf_file) OVERRIDE {
      std::vector<Elf_Dyn> buffer;
      buffer.reserve(dynamics_.size() + 1u);
      for (const ElfDynamicState& it : dynamics_) {
        if (it.section_ != nullptr) {
          // We are adding an address relative to a section.
          buffer.push_back(
              {it.tag_, {it.value_ + it.section_->GetHeader()->sh_addr}});
        } else {
          buffer.push_back({it.tag_, {it.value_}});
        }
      }
      buffer.push_back({DT_NULL, {0}});
      return WriteArray(elf_file, buffer.data(), buffer.size());
    }

   private:
    struct ElfDynamicState {
      Elf_Sword tag_;
      Elf_Word value_;
      const Section* section_;
    };
    std::vector<ElfDynamicState> dynamics_;
  };

  using PatchFn = void (*)(const std::vector<uintptr_t>& patch_locations,
                           Elf_Addr buffer_address,
                           Elf_Addr base_address,
                           std::vector<uint8_t>* buffer);

  // Section with content based on simple memory buffer.
  // The buffer can be optionally patched before writing.
  class RawSection FINAL : public Section {
   public:
    RawSection(const std::string& name, Elf_Word type, Elf_Word flags,
               const Section* link, Elf_Word info, Elf_Word align, Elf_Word entsize,
               PatchFn patch = nullptr, const Section* patch_base_section = nullptr)
        : Section(name, type, flags, link, info, align, entsize),
          patched_(false), patch_(patch), patch_base_section_(patch_base_section) {
    }

    RawSection(const std::string& name, Elf_Word type)
        : RawSection(name, type, 0, nullptr, 0, 1, 0, nullptr, nullptr) {
    }

    Elf_Word GetSize() const OVERRIDE {
      return buffer_.size();
    }

    bool Write(File* elf_file) OVERRIDE {
      if (!patch_locations_.empty()) {
        DCHECK(!patched_);  // Do not patch twice.
        DCHECK(patch_ != nullptr);
        DCHECK(patch_base_section_ != nullptr);
        patch_(patch_locations_,
               this->GetHeader()->sh_addr,
               patch_base_section_->GetHeader()->sh_addr,
               &buffer_);
        patched_ = true;
      }
      return WriteArray(elf_file, buffer_.data(), buffer_.size());
    }

    bool IsEmpty() const {
      return buffer_.size() == 0;
    }

    std::vector<uint8_t>* GetBuffer() {
      return &buffer_;
    }

    void SetBuffer(const std::vector<uint8_t>& buffer) {
      buffer_ = buffer;
    }

    std::vector<uintptr_t>* GetPatchLocations() {
      return &patch_locations_;
    }

   private:
    std::vector<uint8_t> buffer_;
    std::vector<uintptr_t> patch_locations_;
    bool patched_;
    // User-provided function to do the actual patching.
    PatchFn patch_;
    // The section that we patch against (usually .text).
    const Section* patch_base_section_;
  };

  // Writer of .rodata section or .text section.
  // The write is done lazily using the provided CodeOutput.
  class OatSection FINAL : public Section {
   public:
    OatSection(const std::string& name, Elf_Word type, Elf_Word flags,
               const Section* link, Elf_Word info, Elf_Word align,
               Elf_Word entsize, Elf_Word size, CodeOutput* code_output)
        : Section(name, type, flags, link, info, align, entsize),
          size_(size), code_output_(code_output) {
    }

    Elf_Word GetSize() const OVERRIDE {
      return size_;
    }

    bool Write(File* elf_file) OVERRIDE {
      // The BufferedOutputStream class contains the buffer as field,
      // therefore it is too big to allocate on the stack.
      std::unique_ptr<BufferedOutputStream> output_stream(
          new BufferedOutputStream(new FileOutputStream(elf_file)));
      return code_output_->Write(output_stream.get());
    }

   private:
    Elf_Word size_;
    CodeOutput* code_output_;
  };

  // Writer of .bss section.
  class NoBitsSection FINAL : public Section {
   public:
    NoBitsSection(const std::string& name, Elf_Word size)
        : Section(name, SHT_NOBITS, SHF_ALLOC, nullptr, 0, kPageSize, 0),
          size_(size) {
    }

    Elf_Word GetSize() const OVERRIDE {
      return size_;
    }

    bool Write(File* elf_file ATTRIBUTE_UNUSED) OVERRIDE {
      LOG(ERROR) << "This section should not be written to the ELF file";
      return false;
    }

   private:
    Elf_Word size_;
  };

  // Writer of .dynstr .strtab and .shstrtab sections.
  class StrtabSection FINAL : public Section {
   public:
    StrtabSection(const std::string& name, Elf_Word flags)
        : Section(name, SHT_STRTAB, flags, nullptr, 0, 1, 0) {
      buffer_.reserve(4 * KB);
      // The first entry of strtab must be empty string.
      buffer_ += '\0';
    }

    Elf_Word AddName(const std::string& name) {
      Elf_Word offset = buffer_.size();
      buffer_ += name;
      buffer_ += '\0';
      return offset;
    }

    Elf_Word GetSize() const OVERRIDE {
      return buffer_.size();
    }

    bool Write(File* elf_file) OVERRIDE {
      return WriteArray(elf_file, buffer_.data(), buffer_.size());
    }

   private:
    std::string buffer_;
  };

  class HashSection;

  // Writer of .dynsym and .symtab sections.
  class SymtabSection FINAL : public Section {
   public:
    // Add a symbol with given name to this symtab. The symbol refers to
    // 'relative_addr' within the given section and has the given attributes.
    void AddSymbol(const std::string& name, const Section* section,
                   Elf_Addr addr, bool is_relative, Elf_Word size,
                   uint8_t binding, uint8_t type, uint8_t other = 0) {
      CHECK(section != nullptr);
      Elf_Word name_idx = strtab_->AddName(name);
      symbols_.push_back({ name, section, addr, size, is_relative,
                           MakeStInfo(binding, type), other, name_idx });
    }

    SymtabSection(const std::string& name, Elf_Word type, Elf_Word flags,
                  StrtabSection* strtab)
        : Section(name, type, flags, strtab, 0, sizeof(Elf_Off), sizeof(Elf_Sym)),
          strtab_(strtab) {
    }

    bool IsEmpty() const {
      return symbols_.empty();
    }

    Elf_Word GetSize() const OVERRIDE {
      return (1 /* NULL */ + symbols_.size()) * sizeof(Elf_Sym);
    }

    bool Write(File* elf_file) OVERRIDE {
      std::vector<Elf_Sym> buffer;
      buffer.reserve(1u + symbols_.size());
      buffer.push_back(Elf_Sym());  // NULL.
      for (const ElfSymbolState& it : symbols_) {
        Elf_Sym sym = Elf_Sym();
        sym.st_name = it.name_idx_;
        if (it.is_relative_) {
          sym.st_value = it.addr_ + it.section_->GetHeader()->sh_addr;
        } else {
          sym.st_value = it.addr_;
        }
        sym.st_size = it.size_;
        sym.st_other = it.other_;
        sym.st_shndx = it.section_->GetSectionIndex();
        sym.st_info = it.info_;
        buffer.push_back(sym);
      }
      return WriteArray(elf_file, buffer.data(), buffer.size());
    }

   private:
    struct ElfSymbolState {
      const std::string name_;
      const Section* section_;
      Elf_Addr addr_;
      Elf_Word size_;
      bool is_relative_;
      uint8_t info_;
      uint8_t other_;
      Elf_Word name_idx_;  // index in the strtab.
    };

    static inline constexpr uint8_t MakeStInfo(uint8_t binding, uint8_t type) {
      return ((binding) << 4) + ((type) & 0xf);
    }

    // The symbols in the same order they will be in the symbol table.
    std::vector<ElfSymbolState> symbols_;
    StrtabSection* strtab_;

    friend class HashSection;
  };

  // TODO: Consider removing.
  // We use it only for the dynsym section which has only 5 symbols.
  // We do not use it for symtab, and we probably do not have to
  // since we use those symbols only to print backtraces.
  class HashSection FINAL : public Section {
   public:
    HashSection(const std::string& name, Elf_Word flags, SymtabSection* symtab)
        : Section(name, SHT_HASH, flags, symtab,
                  0, sizeof(Elf_Word), sizeof(Elf_Word)),
          symtab_(symtab) {
    }

    Elf_Word GetSize() const OVERRIDE {
      Elf_Word nbuckets = GetNumBuckets();
      Elf_Word chain_size = symtab_->symbols_.size() + 1 /* NULL */;
      return (2 /* header */ + nbuckets + chain_size) * sizeof(Elf_Word);
    }

    bool Write(File* const elf_file) OVERRIDE {
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
      const auto& symbols = symtab_->symbols_;
      // Select number of buckets.
      // This is essentially arbitrary.
      Elf_Word nbuckets = GetNumBuckets();
      // 1 is for the implicit NULL symbol.
      Elf_Word chain_size = (symbols.size() + 1);
      std::vector<Elf_Word> hash;
      hash.push_back(nbuckets);
      hash.push_back(chain_size);
      uint32_t bucket_offset = hash.size();
      uint32_t chain_offset = bucket_offset + nbuckets;
      hash.resize(hash.size() + nbuckets + chain_size, 0);

      Elf_Word* buckets = hash.data() + bucket_offset;
      Elf_Word* chain   = hash.data() + chain_offset;

      // Set up the actual hash table.
      for (Elf_Word i = 0; i < symbols.size(); i++) {
        // Add 1 since we need to have the null symbol that is not in the symbols
        // list.
        Elf_Word index = i + 1;
        Elf_Word hash_val = static_cast<Elf_Word>(elfhash(symbols[i].name_.c_str())) % nbuckets;
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
      return WriteArray(elf_file, hash.data(), hash.size());
    }

   private:
    Elf_Word GetNumBuckets() const {
      const auto& symbols = symtab_->symbols_;
      if (symbols.size() < 8) {
        return 2;
      } else if (symbols.size() < 32) {
        return 4;
      } else if (symbols.size() < 256) {
        return 16;
      } else {
        // Have about 32 ids per bucket.
        return RoundUp(symbols.size()/32, 2);
      }
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

    SymtabSection* symtab_;

    DISALLOW_COPY_AND_ASSIGN(HashSection);
  };

  ElfBuilder(InstructionSet isa,
             Elf_Word rodata_size, CodeOutput* rodata_writer,
             Elf_Word text_size, CodeOutput* text_writer,
             Elf_Word bss_size)
    : isa_(isa),
      dynstr_(".dynstr", SHF_ALLOC),
      dynsym_(".dynsym", SHT_DYNSYM, SHF_ALLOC, &dynstr_),
      hash_(".hash", SHF_ALLOC, &dynsym_),
      rodata_(".rodata", SHT_PROGBITS, SHF_ALLOC,
              nullptr, 0, kPageSize, 0, rodata_size, rodata_writer),
      text_(".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
            nullptr, 0, kPageSize, 0, text_size, text_writer),
      bss_(".bss", bss_size),
      dynamic_(".dynamic", &dynstr_),
      strtab_(".strtab", 0),
      symtab_(".symtab", SHT_SYMTAB, 0, &strtab_),
      shstrtab_(".shstrtab", 0) {
  }
  ~ElfBuilder() {}

  OatSection* GetText() { return &text_; }
  SymtabSection* GetSymtab() { return &symtab_; }

  bool Write(File* elf_file) {
    // Since the .text section of an oat file contains relative references to .rodata
    // and (optionally) .bss, we keep these 2 or 3 sections together. This creates
    // a non-traditional layout where the .bss section is mapped independently of the
    // .dynamic section and needs its own program header with LOAD RW.
    //
    // The basic layout of the elf file. Order may be different in final output.
    // +-------------------------+
    // | Elf_Ehdr                |
    // +-------------------------+
    // | Elf_Phdr PHDR           |
    // | Elf_Phdr LOAD R         | .dynsym .dynstr .hash .rodata
    // | Elf_Phdr LOAD R X       | .text
    // | Elf_Phdr LOAD RW        | .bss (Optional)
    // | Elf_Phdr LOAD RW        | .dynamic
    // | Elf_Phdr DYNAMIC        | .dynamic
    // | Elf_Phdr LOAD R         | .eh_frame .eh_frame_hdr
    // | Elf_Phdr EH_FRAME R     | .eh_frame_hdr
    // +-------------------------+
    // | .dynsym                 |
    // | Elf_Sym  STN_UNDEF      |
    // | Elf_Sym  oatdata        |
    // | Elf_Sym  oatexec        |
    // | Elf_Sym  oatlastword    |
    // | Elf_Sym  oatbss         | (Optional)
    // | Elf_Sym  oatbsslastword | (Optional)
    // +-------------------------+
    // | .dynstr                 |
    // | names for .dynsym       |
    // +-------------------------+
    // | .hash                   |
    // | hashtable for dynsym    |
    // +-------------------------+
    // | .rodata                 |
    // | oatdata..oatexec-4      |
    // +-------------------------+
    // | .text                   |
    // | oatexec..oatlastword    |
    // +-------------------------+
    // | .dynamic                |
    // | Elf_Dyn DT_HASH         |
    // | Elf_Dyn DT_STRTAB       |
    // | Elf_Dyn DT_SYMTAB       |
    // | Elf_Dyn DT_SYMENT       |
    // | Elf_Dyn DT_STRSZ        |
    // | Elf_Dyn DT_SONAME       |
    // | Elf_Dyn DT_NULL         |
    // +-------------------------+  (Optional)
    // | .symtab                 |  (Optional)
    // | program symbols         |  (Optional)
    // +-------------------------+  (Optional)
    // | .strtab                 |  (Optional)
    // | names for .symtab       |  (Optional)
    // +-------------------------+  (Optional)
    // | .eh_frame               |  (Optional)
    // +-------------------------+  (Optional)
    // | .eh_frame_hdr           |  (Optional)
    // +-------------------------+  (Optional)
    // | .debug_info             |  (Optional)
    // +-------------------------+  (Optional)
    // | .debug_abbrev           |  (Optional)
    // +-------------------------+  (Optional)
    // | .debug_str              |  (Optional)
    // +-------------------------+  (Optional)
    // | .debug_line             |  (Optional)
    // +-------------------------+
    // | .shstrtab               |
    // | names of sections       |
    // +-------------------------+
    // | Elf_Shdr null           |
    // | Elf_Shdr .dynsym        |
    // | Elf_Shdr .dynstr        |
    // | Elf_Shdr .hash          |
    // | Elf_Shdr .rodata        |
    // | Elf_Shdr .text          |
    // | Elf_Shdr .bss           |  (Optional)
    // | Elf_Shdr .dynamic       |
    // | Elf_Shdr .symtab        |  (Optional)
    // | Elf_Shdr .strtab        |  (Optional)
    // | Elf_Shdr .eh_frame      |  (Optional)
    // | Elf_Shdr .eh_frame_hdr  |  (Optional)
    // | Elf_Shdr .debug_info    |  (Optional)
    // | Elf_Shdr .debug_abbrev  |  (Optional)
    // | Elf_Shdr .debug_str     |  (Optional)
    // | Elf_Shdr .debug_line    |  (Optional)
    // | Elf_Shdr .oat_patches   |  (Optional)
    // | Elf_Shdr .shstrtab      |
    // +-------------------------+
    constexpr bool debug_logging_ = false;

    // Create a list of all section which we want to write.
    // This is the order in which they will be written.
    std::vector<Section*> sections;
    sections.push_back(&dynsym_);
    sections.push_back(&dynstr_);
    sections.push_back(&hash_);
    sections.push_back(&rodata_);
    sections.push_back(&text_);
    if (bss_.GetSize() != 0u) {
      sections.push_back(&bss_);
    }
    sections.push_back(&dynamic_);
    if (!symtab_.IsEmpty()) {
      sections.push_back(&symtab_);
      sections.push_back(&strtab_);
    }
    for (Section* section : other_sections_) {
      sections.push_back(section);
    }
    sections.push_back(&shstrtab_);
    for (size_t i = 0; i < sections.size(); i++) {
      // The first section index is 1.  Index 0 is reserved for NULL.
      // Section index is used for relative symbols and for section links.
      sections[i]->SetSectionIndex(i + 1);
      // Add section name to .shstrtab.
      Elf_Word name_offset = shstrtab_.AddName(sections[i]->GetName());
      sections[i]->GetHeader()->sh_name = name_offset;
    }

    // The running program does not have access to section headers
    // and the loader is not supposed to use them either.
    // The dynamic sections therefore replicates some of the layout
    // information like the address and size of .rodata and .text.
    // It also contains other metadata like the SONAME.
    // The .dynamic section is found using the PT_DYNAMIC program header.
    BuildDynsymSection();
    BuildDynamicSection(elf_file->GetPath());

    // We do not know the number of headers until the final stages of write.
    // It is easiest to just reserve a fixed amount of space for them.
    constexpr size_t kMaxProgramHeaders = 8;
    constexpr size_t kProgramHeadersOffset = sizeof(Elf_Ehdr);

    // Layout of all sections - determine the final file offsets and addresses.
    // This must be done after we have built all sections and know their size.
    Elf_Off file_offset = kProgramHeadersOffset + sizeof(Elf_Phdr) * kMaxProgramHeaders;
    Elf_Addr load_address = file_offset;
    std::vector<Elf_Shdr> section_headers;
    section_headers.reserve(1u + sections.size());
    section_headers.push_back(Elf_Shdr());  // NULL at index 0.
    for (auto* section : sections) {
      Elf_Shdr* header = section->GetHeader();
      Elf_Off alignment = header->sh_addralign > 0 ? header->sh_addralign : 1;
      header->sh_size = section->GetSize();
      header->sh_link = section->GetLink();
      // Allocate memory for the section in the file.
      if (header->sh_type != SHT_NOBITS) {
        header->sh_offset = RoundUp(file_offset, alignment);
        file_offset = header->sh_offset + header->sh_size;
      }
      // Allocate memory for the section during program execution.
      if ((header->sh_flags & SHF_ALLOC) != 0) {
        header->sh_addr = RoundUp(load_address, alignment);
        load_address = header->sh_addr + header->sh_size;
      }
      if (debug_logging_) {
        LOG(INFO) << "Section " << section->GetName() << ":" << std::hex
                  << " offset=0x" << header->sh_offset
                  << " addr=0x" << header->sh_addr
                  << " size=0x" << header->sh_size;
      }
      // Collect section headers into continuous array for convenience.
      section_headers.push_back(*header);
    }
    Elf_Off section_headers_offset = RoundUp(file_offset, sizeof(Elf_Off));

    // Create program headers now that we know the layout of the whole file.
    // Each segment contains one or more sections which are mapped together.
    // Not all sections are mapped during the execution of the program.
    // PT_LOAD does the mapping.  Other PT_* types allow the program to locate
    // interesting parts of memory and their addresses overlap with PT_LOAD.
    std::vector<Elf_Phdr> program_headers;
    program_headers.push_back(Elf_Phdr());  // Placeholder for PT_PHDR.
    // Create the main LOAD R segment which spans all sections up to .rodata.
    const Elf_Shdr* rodata = rodata_.GetHeader();
    program_headers.push_back(MakeProgramHeader(PT_LOAD, PF_R,
      0, rodata->sh_offset + rodata->sh_size, rodata->sh_addralign));
    program_headers.push_back(MakeProgramHeader(PT_LOAD, PF_R | PF_X, text_));
    if (bss_.GetHeader()->sh_size != 0u) {
      program_headers.push_back(MakeProgramHeader(PT_LOAD, PF_R | PF_W, bss_));
    }
    program_headers.push_back(MakeProgramHeader(PT_LOAD, PF_R | PF_W, dynamic_));
    program_headers.push_back(MakeProgramHeader(PT_DYNAMIC, PF_R | PF_W, dynamic_));
    const Section* eh_frame = FindSection(".eh_frame");
    if (eh_frame != nullptr) {
      program_headers.push_back(MakeProgramHeader(PT_LOAD, PF_R, *eh_frame));
      const Section* eh_frame_hdr = FindSection(".eh_frame_hdr");
      if (eh_frame_hdr != nullptr) {
        // Check layout: eh_frame is before eh_frame_hdr and there is no gap.
        CHECK_LE(eh_frame->GetHeader()->sh_offset, eh_frame_hdr->GetHeader()->sh_offset);
        CHECK_EQ(eh_frame->GetHeader()->sh_offset + eh_frame->GetHeader()->sh_size,
                 eh_frame_hdr->GetHeader()->sh_offset);
        // Extend the PT_LOAD of .eh_frame to include the .eh_frame_hdr as well.
        program_headers.back().p_filesz += eh_frame_hdr->GetHeader()->sh_size;
        program_headers.back().p_memsz  += eh_frame_hdr->GetHeader()->sh_size;
        program_headers.push_back(MakeProgramHeader(PT_GNU_EH_FRAME, PF_R, *eh_frame_hdr));
      }
    }
    DCHECK_EQ(program_headers[0].p_type, 0u);  // Check placeholder.
    program_headers[0] = MakeProgramHeader(PT_PHDR, PF_R,
      kProgramHeadersOffset, program_headers.size() * sizeof(Elf_Phdr), sizeof(Elf_Off));
    CHECK_LE(program_headers.size(), kMaxProgramHeaders);

    // Create the main ELF header.
    Elf_Ehdr elf_header = MakeElfHeader(isa_);
    elf_header.e_phoff = kProgramHeadersOffset;
    elf_header.e_shoff = section_headers_offset;
    elf_header.e_phnum = program_headers.size();
    elf_header.e_shnum = section_headers.size();
    elf_header.e_shstrndx = shstrtab_.GetSectionIndex();

    // Write all headers and section content to the file.
    // Depending on the implementations of Section::Write, this
    // might be just memory copies or some more elaborate operations.
    if (!WriteArray(elf_file, &elf_header, 1)) {
      LOG(INFO) << "Failed to write the ELF header";
      return false;
    }
    if (!WriteArray(elf_file, program_headers.data(), program_headers.size())) {
      LOG(INFO) << "Failed to write the program headers";
      return false;
    }
    for (Section* section : sections) {
      const Elf_Shdr* header = section->GetHeader();
      if (header->sh_type != SHT_NOBITS) {
        if (!SeekTo(elf_file, header->sh_offset) || !section->Write(elf_file)) {
          LOG(INFO) << "Failed to write section " << section->GetName();
          return false;
        }
        Elf_Word current_offset = lseek(elf_file->Fd(), 0, SEEK_CUR);
        CHECK_EQ(current_offset, header->sh_offset + header->sh_size)
          << "The number of bytes written does not match GetSize()";
      }
    }
    if (!SeekTo(elf_file, section_headers_offset) ||
        !WriteArray(elf_file, section_headers.data(), section_headers.size())) {
      LOG(INFO) << "Failed to write the section headers";
      return false;
    }
    return true;
  }

  // Adds the given section to the builder.  It does not take ownership.
  void RegisterSection(Section* section) {
    other_sections_.push_back(section);
  }

  const Section* FindSection(const char* name) {
    for (const auto* section : other_sections_) {
      if (section->GetName() == name) {
        return section;
      }
    }
    return nullptr;
  }

 private:
  static bool SeekTo(File* elf_file, Elf_Word offset) {
    DCHECK_LE(lseek(elf_file->Fd(), 0, SEEK_CUR), static_cast<off_t>(offset))
      << "Seeking backwards";
    if (static_cast<off_t>(offset) != lseek(elf_file->Fd(), offset, SEEK_SET)) {
      PLOG(ERROR) << "Failed to seek in file " << elf_file->GetPath();
      return false;
    }
    return true;
  }

  template<typename T>
  static bool WriteArray(File* elf_file, const T* data, size_t count) {
    if (count != 0) {
      DCHECK(data != nullptr);
      if (!elf_file->WriteFully(data, count * sizeof(T))) {
        PLOG(ERROR) << "Failed to write to file " << elf_file->GetPath();
        return false;
      }
    }
    return true;
  }

  // Helper - create segment header based on memory range.
  static Elf_Phdr MakeProgramHeader(Elf_Word type, Elf_Word flags,
                                    Elf_Off offset, Elf_Word size, Elf_Word align) {
    Elf_Phdr phdr = Elf_Phdr();
    phdr.p_type    = type;
    phdr.p_flags   = flags;
    phdr.p_offset  = offset;
    phdr.p_vaddr   = offset;
    phdr.p_paddr   = offset;
    phdr.p_filesz  = size;
    phdr.p_memsz   = size;
    phdr.p_align   = align;
    return phdr;
  }

  // Helper - create segment header based on section header.
  static Elf_Phdr MakeProgramHeader(Elf_Word type, Elf_Word flags,
                                    const Section& section) {
    const Elf_Shdr* shdr = section.GetHeader();
    // Only run-time allocated sections should be in segment headers.
    CHECK_NE(shdr->sh_flags & SHF_ALLOC, 0u);
    Elf_Phdr phdr = Elf_Phdr();
    phdr.p_type   = type;
    phdr.p_flags  = flags;
    phdr.p_offset = shdr->sh_offset;
    phdr.p_vaddr  = shdr->sh_addr;
    phdr.p_paddr  = shdr->sh_addr;
    phdr.p_filesz = shdr->sh_type != SHT_NOBITS ? shdr->sh_size : 0u;
    phdr.p_memsz  = shdr->sh_size;
    phdr.p_align  = shdr->sh_addralign;
    return phdr;
  }

  static Elf_Ehdr MakeElfHeader(InstructionSet isa) {
    Elf_Ehdr elf_header = Elf_Ehdr();
    switch (isa) {
      case kArm:
        // Fall through.
      case kThumb2: {
        elf_header.e_machine = EM_ARM;
        elf_header.e_flags = EF_ARM_EABI_VER5;
        break;
      }
      case kArm64: {
        elf_header.e_machine = EM_AARCH64;
        elf_header.e_flags = 0;
        break;
      }
      case kX86: {
        elf_header.e_machine = EM_386;
        elf_header.e_flags = 0;
        break;
      }
      case kX86_64: {
        elf_header.e_machine = EM_X86_64;
        elf_header.e_flags = 0;
        break;
      }
      case kMips: {
        elf_header.e_machine = EM_MIPS;
        elf_header.e_flags = (EF_MIPS_NOREORDER |
                               EF_MIPS_PIC       |
                               EF_MIPS_CPIC      |
                               EF_MIPS_ABI_O32   |
                               EF_MIPS_ARCH_32R2);
        break;
      }
      case kMips64: {
        elf_header.e_machine = EM_MIPS;
        elf_header.e_flags = (EF_MIPS_NOREORDER |
                               EF_MIPS_PIC       |
                               EF_MIPS_CPIC      |
                               EF_MIPS_ARCH_64R6);
        break;
      }
      case kNone: {
        LOG(FATAL) << "No instruction set";
      }
    }

    elf_header.e_ident[EI_MAG0]       = ELFMAG0;
    elf_header.e_ident[EI_MAG1]       = ELFMAG1;
    elf_header.e_ident[EI_MAG2]       = ELFMAG2;
    elf_header.e_ident[EI_MAG3]       = ELFMAG3;
    elf_header.e_ident[EI_CLASS]      = (sizeof(Elf_Addr) == sizeof(Elf32_Addr))
                                         ? ELFCLASS32 : ELFCLASS64;;
    elf_header.e_ident[EI_DATA]       = ELFDATA2LSB;
    elf_header.e_ident[EI_VERSION]    = EV_CURRENT;
    elf_header.e_ident[EI_OSABI]      = ELFOSABI_LINUX;
    elf_header.e_ident[EI_ABIVERSION] = 0;
    elf_header.e_type = ET_DYN;
    elf_header.e_version = 1;
    elf_header.e_entry = 0;
    elf_header.e_ehsize = sizeof(Elf_Ehdr);
    elf_header.e_phentsize = sizeof(Elf_Phdr);
    elf_header.e_shentsize = sizeof(Elf_Shdr);
    elf_header.e_phoff = sizeof(Elf_Ehdr);
    return elf_header;
  }

  void BuildDynamicSection(const std::string& elf_file_path) {
    std::string soname(elf_file_path);
    size_t directory_separator_pos = soname.rfind('/');
    if (directory_separator_pos != std::string::npos) {
      soname = soname.substr(directory_separator_pos + 1);
    }
    // NB: We must add the name before adding DT_STRSZ.
    Elf_Word soname_offset = dynstr_.AddName(soname);

    dynamic_.AddDynamicTag(DT_HASH, 0, &hash_);
    dynamic_.AddDynamicTag(DT_STRTAB, 0, &dynstr_);
    dynamic_.AddDynamicTag(DT_SYMTAB, 0, &dynsym_);
    dynamic_.AddDynamicTag(DT_SYMENT, sizeof(Elf_Sym), nullptr);
    dynamic_.AddDynamicTag(DT_STRSZ, dynstr_.GetSize(), nullptr);
    dynamic_.AddDynamicTag(DT_SONAME, soname_offset, nullptr);
  }

  void BuildDynsymSection() {
    dynsym_.AddSymbol("oatdata", &rodata_, 0, true,
                      rodata_.GetSize(), STB_GLOBAL, STT_OBJECT);
    dynsym_.AddSymbol("oatexec", &text_, 0, true,
                      text_.GetSize(), STB_GLOBAL, STT_OBJECT);
    dynsym_.AddSymbol("oatlastword", &text_, text_.GetSize() - 4,
                      true, 4, STB_GLOBAL, STT_OBJECT);
    if (bss_.GetSize() != 0u) {
      dynsym_.AddSymbol("oatbss", &bss_, 0, true,
                        bss_.GetSize(), STB_GLOBAL, STT_OBJECT);
      dynsym_.AddSymbol("oatbsslastword", &bss_, bss_.GetSize() - 4,
                        true, 4, STB_GLOBAL, STT_OBJECT);
    }
  }

  InstructionSet isa_;
  StrtabSection dynstr_;
  SymtabSection dynsym_;
  HashSection hash_;
  OatSection rodata_;
  OatSection text_;
  NoBitsSection bss_;
  DynamicSection dynamic_;
  StrtabSection strtab_;
  SymtabSection symtab_;
  std::vector<Section*> other_sections_;
  StrtabSection shstrtab_;

  DISALLOW_COPY_AND_ASSIGN(ElfBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_ELF_BUILDER_H_
