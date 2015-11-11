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
#include "base/casts.h"
#include "base/unix_file/fd_file.h"
#include "buffered_output_stream.h"
#include "elf_utils.h"
#include "file_output_stream.h"
#include "leb128.h"

namespace art {

// Writes ELF file.
//
// The basic layout of the elf file:
//   Elf_Ehdr                    - The ELF header.
//   Elf_Phdr[]                  - Program headers for the linker.
//   .rodata                     - DEX files and oat metadata.
//   .text                       - Compiled code.
//   .bss                        - Zero-initialized writeable section.
//   .dynstr                     - Names for .dynsym.
//   .dynsym                     - A few oat-specific dynamic symbols.
//   .hash                       - Hash-table for .dynsym.
//   .dynamic                    - Tags which let the linker locate .dynsym.
//   .strtab                     - Names for .symtab.
//   .symtab                     - Debug symbols.
//   .eh_frame                   - Unwind information (CFI).
//   .eh_frame_hdr               - Index of .eh_frame.
//   .debug_frame                - Unwind information (CFI).
//   .debug_frame.oat_patches    - Addresses for relocation.
//   .debug_info                 - Debug information.
//   .debug_info.oat_patches     - Addresses for relocation.
//   .debug_abbrev               - Decoding information for .debug_info.
//   .debug_str                  - Strings for .debug_info.
//   .debug_line                 - Line number tables.
//   .debug_line.oat_patches     - Addresses for relocation.
//   .text.oat_patches           - Addresses for relocation.
//   .shstrtab                   - Names of ELF sections.
//   Elf_Shdr[]                  - Section headers.
//
// Some section are optional (the debug sections in particular).
//
// We try write the section data directly into the file without much
// in-memory buffering.  This means we generally write sections based on the
// dependency order (e.g. .dynamic points to .dynsym which points to .text).
//
// In the cases where we need to buffer, we write the larger section first
// and buffer the smaller one (e.g. .strtab is bigger than .symtab).
//
// The debug sections are written last for easier stripping.
//
template <typename ElfTypes>
class ElfBuilder FINAL {
 public:
  static constexpr size_t kMaxProgramHeaders = 16;
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
  class Section : public OutputStream {
   public:
    Section(ElfBuilder<ElfTypes>* owner, const std::string& name,
            Elf_Word type, Elf_Word flags, const Section* link,
            Elf_Word info, Elf_Word align, Elf_Word entsize)
        : OutputStream(name), owner_(owner), header_(),
          section_index_(0), name_(name), link_(link),
          started_(false), finished_(false), phdr_flags_(PF_R), phdr_type_(0) {
      DCHECK_GE(align, 1u);
      header_.sh_type = type;
      header_.sh_flags = flags;
      header_.sh_info = info;
      header_.sh_addralign = align;
      header_.sh_entsize = entsize;
    }

    virtual ~Section() {
      if (started_) {
        CHECK(finished_);
      }
    }

    // Start writing of this section.
    void Start() {
      CHECK(!started_);
      CHECK(!finished_);
      started_ = true;
      auto& sections = owner_->sections_;
      // Check that the previous section is complete.
      CHECK(sections.empty() || sections.back()->finished_);
      // The first ELF section index is 1. Index 0 is reserved for NULL.
      section_index_ = sections.size() + 1;
      // Push this section on the list of written sections.
      sections.push_back(this);
      // Align file position.
      if (header_.sh_type != SHT_NOBITS) {
        header_.sh_offset = RoundUp(owner_->Seek(0, kSeekCurrent), header_.sh_addralign);
        owner_->Seek(header_.sh_offset, kSeekSet);
      }
      // Align virtual memory address.
      if ((header_.sh_flags & SHF_ALLOC) != 0) {
        header_.sh_addr = RoundUp(owner_->virtual_address_, header_.sh_addralign);
        owner_->virtual_address_ = header_.sh_addr;
      }
    }

    // Finish writing of this section.
    void End() {
      CHECK(started_);
      CHECK(!finished_);
      finished_ = true;
      if (header_.sh_type == SHT_NOBITS) {
        CHECK_GT(header_.sh_size, 0u);
      } else {
        // Use the current file position to determine section size.
        off_t file_offset = owner_->Seek(0, kSeekCurrent);
        CHECK_GE(file_offset, (off_t)header_.sh_offset);
        header_.sh_size = file_offset - header_.sh_offset;
      }
      if ((header_.sh_flags & SHF_ALLOC) != 0) {
        owner_->virtual_address_ += header_.sh_size;
      }
    }

    // Get the location of this section in virtual memory.
    Elf_Addr GetAddress() const {
      CHECK(started_);
      return header_.sh_addr;
    }

    // Returns the size of the content of this section.
    Elf_Word GetSize() const {
      if (finished_) {
        return header_.sh_size;
      } else {
        CHECK(started_);
        CHECK_NE(header_.sh_type, (Elf_Word)SHT_NOBITS);
        return owner_->Seek(0, kSeekCurrent) - header_.sh_offset;
      }
    }

    // Set desired allocation size for .bss section.
    void SetSize(Elf_Word size) {
      CHECK_EQ(header_.sh_type, (Elf_Word)SHT_NOBITS);
      header_.sh_size = size;
    }

    // This function always succeeds to simplify code.
    // Use builder's Good() to check the actual status.
    bool WriteFully(const void* buffer, size_t byte_count) OVERRIDE {
      CHECK(started_);
      CHECK(!finished_);
      owner_->WriteFully(buffer, byte_count);
      return true;
    }

    // This function always succeeds to simplify code.
    // Use builder's Good() to check the actual status.
    off_t Seek(off_t offset, Whence whence) OVERRIDE {
      // Forward the seek as-is and trust the caller to use it reasonably.
      return owner_->Seek(offset, whence);
    }

    Elf_Word GetSectionIndex() const {
      DCHECK(started_);
      DCHECK_NE(section_index_, 0u);
      return section_index_;
    }

   private:
    ElfBuilder<ElfTypes>* owner_;
    Elf_Shdr header_;
    Elf_Word section_index_;
    const std::string name_;
    const Section* const link_;
    bool started_;
    bool finished_;
    Elf_Word phdr_flags_;
    Elf_Word phdr_type_;

    friend class ElfBuilder;

    DISALLOW_COPY_AND_ASSIGN(Section);
  };

  // Writer of .dynstr .strtab and .shstrtab sections.
  class StringSection FINAL : public Section {
   public:
    StringSection(ElfBuilder<ElfTypes>* owner, const std::string& name,
                  Elf_Word flags, Elf_Word align)
        : Section(owner, name, SHT_STRTAB, flags, nullptr, 0, align, 0),
          current_offset_(0) {
    }

    Elf_Word Write(const std::string& name) {
      if (current_offset_ == 0) {
        DCHECK(name.empty());
      }
      Elf_Word offset = current_offset_;
      this->WriteFully(name.c_str(), name.length() + 1);
      current_offset_ += name.length() + 1;
      return offset;
    }

   private:
    Elf_Word current_offset_;
  };

  // Writer of .dynsym and .symtab sections.
  class SymbolSection FINAL : public Section {
   public:
    SymbolSection(ElfBuilder<ElfTypes>* owner, const std::string& name,
                  Elf_Word type, Elf_Word flags, StringSection* strtab)
        : Section(owner, name, type, flags, strtab, 0,
                  sizeof(Elf_Off), sizeof(Elf_Sym)) {
    }

    // Buffer symbol for this section.  It will be written later.
    void Add(Elf_Word name, const Section* section,
             Elf_Addr addr, bool is_relative, Elf_Word size,
             uint8_t binding, uint8_t type, uint8_t other = 0) {
      CHECK(section != nullptr);
      Elf_Sym sym = Elf_Sym();
      sym.st_name = name;
      sym.st_value = addr + (is_relative ? section->GetAddress() : 0);
      sym.st_size = size;
      sym.st_other = other;
      sym.st_shndx = section->GetSectionIndex();
      sym.st_info = (binding << 4) + (type & 0xf);
      symbols_.push_back(sym);
    }

    void Write() {
      // The symbol table always has to start with NULL symbol.
      Elf_Sym null_symbol = Elf_Sym();
      this->WriteFully(&null_symbol, sizeof(null_symbol));
      this->WriteFully(symbols_.data(), symbols_.size() * sizeof(symbols_[0]));
      symbols_.clear();
      symbols_.shrink_to_fit();
    }

   private:
    std::vector<Elf_Sym> symbols_;
  };

  ElfBuilder(InstructionSet isa, OutputStream* output)
    : isa_(isa),
      output_(output),
      output_good_(true),
      output_offset_(0),
      rodata_(this, ".rodata", SHT_PROGBITS, SHF_ALLOC, nullptr, 0, kPageSize, 0),
      text_(this, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, nullptr, 0, kPageSize, 0),
      bss_(this, ".bss", SHT_NOBITS, SHF_ALLOC, nullptr, 0, kPageSize, 0),
      dynstr_(this, ".dynstr", SHF_ALLOC, kPageSize),
      dynsym_(this, ".dynsym", SHT_DYNSYM, SHF_ALLOC, &dynstr_),
      hash_(this, ".hash", SHT_HASH, SHF_ALLOC, &dynsym_, 0, sizeof(Elf_Word), sizeof(Elf_Word)),
      dynamic_(this, ".dynamic", SHT_DYNAMIC, SHF_ALLOC, &dynstr_, 0, kPageSize, sizeof(Elf_Dyn)),
      eh_frame_(this, ".eh_frame", SHT_PROGBITS, SHF_ALLOC, nullptr, 0, kPageSize, 0),
      eh_frame_hdr_(this, ".eh_frame_hdr", SHT_PROGBITS, SHF_ALLOC, nullptr, 0, 4, 0),
      strtab_(this, ".strtab", 0, kPageSize),
      symtab_(this, ".symtab", SHT_SYMTAB, 0, &strtab_),
      debug_frame_(this, ".debug_frame", SHT_PROGBITS, 0, nullptr, 0, sizeof(Elf_Addr), 0),
      debug_info_(this, ".debug_info", SHT_PROGBITS, 0, nullptr, 0, 1, 0),
      debug_line_(this, ".debug_line", SHT_PROGBITS, 0, nullptr, 0, 1, 0),
      shstrtab_(this, ".shstrtab", 0, 1),
      virtual_address_(0) {
    text_.phdr_flags_ = PF_R | PF_X;
    bss_.phdr_flags_ = PF_R | PF_W;
    dynamic_.phdr_flags_ = PF_R | PF_W;
    dynamic_.phdr_type_ = PT_DYNAMIC;
    eh_frame_hdr_.phdr_type_ = PT_GNU_EH_FRAME;
  }
  ~ElfBuilder() {}

  InstructionSet GetIsa() { return isa_; }
  Section* GetRoData() { return &rodata_; }
  Section* GetText() { return &text_; }
  Section* GetBss() { return &bss_; }
  StringSection* GetStrTab() { return &strtab_; }
  SymbolSection* GetSymTab() { return &symtab_; }
  Section* GetEhFrame() { return &eh_frame_; }
  Section* GetEhFrameHdr() { return &eh_frame_hdr_; }
  Section* GetDebugFrame() { return &debug_frame_; }
  Section* GetDebugInfo() { return &debug_info_; }
  Section* GetDebugLine() { return &debug_line_; }

  // Encode patch locations as LEB128 list of deltas between consecutive addresses.
  // (exposed publicly for tests)
  static void EncodeOatPatches(const std::vector<uintptr_t>& locations,
                               std::vector<uint8_t>* buffer) {
    buffer->reserve(buffer->size() + locations.size() * 2);  // guess 2 bytes per ULEB128.
    uintptr_t address = 0;  // relative to start of section.
    for (uintptr_t location : locations) {
      DCHECK_GE(location, address) << "Patch locations are not in sorted order";
      EncodeUnsignedLeb128(buffer, dchecked_integral_cast<uint32_t>(location - address));
      address = location;
    }
  }

  void WritePatches(const char* name, const std::vector<uintptr_t>* patch_locations) {
    std::vector<uint8_t> buffer;
    EncodeOatPatches(*patch_locations, &buffer);
    std::unique_ptr<Section> s(new Section(this, name, SHT_OAT_PATCH, 0, nullptr, 0, 1, 0));
    s->Start();
    s->WriteFully(buffer.data(), buffer.size());
    s->End();
    other_sections_.push_back(std::move(s));
  }

  void WriteSection(const char* name, const std::vector<uint8_t>* buffer) {
    std::unique_ptr<Section> s(new Section(this, name, SHT_PROGBITS, 0, nullptr, 0, 1, 0));
    s->Start();
    s->WriteFully(buffer->data(), buffer->size());
    s->End();
    other_sections_.push_back(std::move(s));
  }

  void Start() {
    // Reserve space for ELF header and program headers.
    // We do not know the number of headers until later, so
    // it is easiest to just reserve a fixed amount of space.
    int size = sizeof(Elf_Ehdr) + sizeof(Elf_Phdr) * kMaxProgramHeaders;
    Seek(size, kSeekSet);
    virtual_address_ += size;
  }

  void End() {
    // Write section names and finish the section headers.
    shstrtab_.Start();
    shstrtab_.Write("");
    for (auto* section : sections_) {
      section->header_.sh_name = shstrtab_.Write(section->name_);
      if (section->link_ != nullptr) {
        section->header_.sh_link = section->link_->GetSectionIndex();
      }
    }
    shstrtab_.End();

    // Write section headers at the end of the ELF file.
    std::vector<Elf_Shdr> shdrs;
    shdrs.reserve(1u + sections_.size());
    shdrs.push_back(Elf_Shdr());  // NULL at index 0.
    for (auto* section : sections_) {
      shdrs.push_back(section->header_);
    }
    Elf_Off section_headers_offset;
    section_headers_offset = RoundUp(Seek(0, kSeekCurrent), sizeof(Elf_Off));
    Seek(section_headers_offset, kSeekSet);
    WriteFully(shdrs.data(), shdrs.size() * sizeof(shdrs[0]));

    // Write the initial file headers.
    std::vector<Elf_Phdr> phdrs = MakeProgramHeaders();
    Elf_Ehdr elf_header = MakeElfHeader(isa_);
    elf_header.e_phoff = sizeof(Elf_Ehdr);
    elf_header.e_shoff = section_headers_offset;
    elf_header.e_phnum = phdrs.size();
    elf_header.e_shnum = shdrs.size();
    elf_header.e_shstrndx = shstrtab_.GetSectionIndex();
    Seek(0, kSeekSet);
    WriteFully(&elf_header, sizeof(elf_header));
    WriteFully(phdrs.data(), phdrs.size() * sizeof(phdrs[0]));
  }

  // The running program does not have access to section headers
  // and the loader is not supposed to use them either.
  // The dynamic sections therefore replicates some of the layout
  // information like the address and size of .rodata and .text.
  // It also contains other metadata like the SONAME.
  // The .dynamic section is found using the PT_DYNAMIC program header.
  void WriteDynamicSection(const std::string& elf_file_path) {
    std::string soname(elf_file_path);
    size_t directory_separator_pos = soname.rfind('/');
    if (directory_separator_pos != std::string::npos) {
      soname = soname.substr(directory_separator_pos + 1);
    }

    dynstr_.Start();
    dynstr_.Write("");  // dynstr should start with empty string.
    dynsym_.Add(dynstr_.Write("oatdata"), &rodata_, 0, true,
                rodata_.GetSize(), STB_GLOBAL, STT_OBJECT);
    if (text_.GetSize() != 0u) {
      dynsym_.Add(dynstr_.Write("oatexec"), &text_, 0, true,
                  text_.GetSize(), STB_GLOBAL, STT_OBJECT);
      dynsym_.Add(dynstr_.Write("oatlastword"), &text_, text_.GetSize() - 4,
                  true, 4, STB_GLOBAL, STT_OBJECT);
    } else if (rodata_.GetSize() != 0) {
      // rodata_ can be size 0 for dwarf_test.
      dynsym_.Add(dynstr_.Write("oatlastword"), &rodata_, rodata_.GetSize() - 4,
                  true, 4, STB_GLOBAL, STT_OBJECT);
    }
    if (bss_.finished_) {
      dynsym_.Add(dynstr_.Write("oatbss"), &bss_,
                  0, true, bss_.GetSize(), STB_GLOBAL, STT_OBJECT);
      dynsym_.Add(dynstr_.Write("oatbsslastword"), &bss_,
                  bss_.GetSize() - 4, true, 4, STB_GLOBAL, STT_OBJECT);
    }
    Elf_Word soname_offset = dynstr_.Write(soname);
    dynstr_.End();

    dynsym_.Start();
    dynsym_.Write();
    dynsym_.End();

    // We do not really need a hash-table since there is so few entries.
    // However, the hash-table is the only way the linker can actually
    // determine the number of symbols in .dynsym so it is required.
    hash_.Start();
    int count = dynsym_.GetSize() / sizeof(Elf_Sym);  // Includes NULL.
    std::vector<Elf_Word> hash;
    hash.push_back(1);  // Number of buckets.
    hash.push_back(count);  // Number of chains.
    // Buckets.  Having just one makes it linear search.
    hash.push_back(1);  // Point to first non-NULL symbol.
    // Chains.  This creates linked list of symbols.
    hash.push_back(0);  // Dummy entry for the NULL symbol.
    for (int i = 1; i < count - 1; i++) {
      hash.push_back(i + 1);  // Each symbol points to the next one.
    }
    hash.push_back(0);  // Last symbol terminates the chain.
    hash_.WriteFully(hash.data(), hash.size() * sizeof(hash[0]));
    hash_.End();

    dynamic_.Start();
    Elf_Dyn dyns[] = {
      { DT_HASH, { hash_.GetAddress() } },
      { DT_STRTAB, { dynstr_.GetAddress() } },
      { DT_SYMTAB, { dynsym_.GetAddress() } },
      { DT_SYMENT, { sizeof(Elf_Sym) } },
      { DT_STRSZ, { dynstr_.GetSize() } },
      { DT_SONAME, { soname_offset } },
      { DT_NULL, { 0 } },
    };
    dynamic_.WriteFully(&dyns, sizeof(dyns));
    dynamic_.End();
  }

  // Returns true if all writes and seeks on the output stream succeeded.
  bool Good() {
    return output_good_;
  }

 private:
  // This function always succeeds to simplify code.
  // Use Good() to check the actual status of the output stream.
  void WriteFully(const void* buffer, size_t byte_count) {
    if (output_good_) {
      if (!output_->WriteFully(buffer, byte_count)) {
        PLOG(ERROR) << "Failed to write " << byte_count
                    << " bytes to ELF file at offset " << output_offset_;
        output_good_ = false;
      }
    }
    output_offset_ += byte_count;
  }

  // This function always succeeds to simplify code.
  // Use Good() to check the actual status of the output stream.
  off_t Seek(off_t offset, Whence whence) {
    // We keep shadow copy of the offset so that we return
    // the expected value even if the output stream failed.
    off_t new_offset;
    switch (whence) {
      case kSeekSet:
        new_offset = offset;
        break;
      case kSeekCurrent:
        new_offset = output_offset_ + offset;
        break;
      default:
        LOG(FATAL) << "Unsupported seek type: " << whence;
        UNREACHABLE();
    }
    if (output_good_) {
      off_t actual_offset = output_->Seek(offset, whence);
      if (actual_offset == (off_t)-1) {
        PLOG(ERROR) << "Failed to seek in ELF file. Offset=" << offset
                    << " whence=" << whence << " new_offset=" << new_offset;
        output_good_ = false;
      }
      DCHECK_EQ(actual_offset, new_offset);
    }
    output_offset_ = new_offset;
    return new_offset;
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
        break;
      }
      default: {
        LOG(FATAL) << "Unknown instruction set " << isa;
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

  // Create program headers based on written sections.
  std::vector<Elf_Phdr> MakeProgramHeaders() {
    CHECK(!sections_.empty());
    std::vector<Elf_Phdr> phdrs;
    {
      // The program headers must start with PT_PHDR which is used in
      // loaded process to determine the number of program headers.
      Elf_Phdr phdr = Elf_Phdr();
      phdr.p_type    = PT_PHDR;
      phdr.p_flags   = PF_R;
      phdr.p_offset  = phdr.p_vaddr = phdr.p_paddr = sizeof(Elf_Ehdr);
      phdr.p_filesz  = phdr.p_memsz = 0;  // We need to fill this later.
      phdr.p_align   = sizeof(Elf_Off);
      phdrs.push_back(phdr);
      // Tell the linker to mmap the start of file to memory.
      Elf_Phdr load = Elf_Phdr();
      load.p_type    = PT_LOAD;
      load.p_flags   = PF_R;
      load.p_offset  = load.p_vaddr = load.p_paddr = 0;
      load.p_filesz  = load.p_memsz = sections_[0]->header_.sh_offset;
      load.p_align   = kPageSize;
      phdrs.push_back(load);
    }
    // Create program headers for sections.
    for (auto* section : sections_) {
      const Elf_Shdr& shdr = section->header_;
      if ((shdr.sh_flags & SHF_ALLOC) != 0 && shdr.sh_size != 0) {
        // PT_LOAD tells the linker to mmap part of the file.
        // The linker can only mmap page-aligned sections.
        // Single PT_LOAD may contain several ELF sections.
        Elf_Phdr& prev = phdrs.back();
        Elf_Phdr load = Elf_Phdr();
        load.p_type   = PT_LOAD;
        load.p_flags  = section->phdr_flags_;
        load.p_offset = shdr.sh_offset;
        load.p_vaddr  = load.p_paddr = shdr.sh_addr;
        load.p_filesz = (shdr.sh_type != SHT_NOBITS ? shdr.sh_size : 0u);
        load.p_memsz  = shdr.sh_size;
        load.p_align  = shdr.sh_addralign;
        if (prev.p_type == load.p_type &&
            prev.p_flags == load.p_flags &&
            prev.p_filesz == prev.p_memsz &&  // Do not merge .bss
            load.p_filesz == load.p_memsz) {  // Do not merge .bss
          // Merge this PT_LOAD with the previous one.
          Elf_Word size = shdr.sh_offset + shdr.sh_size - prev.p_offset;
          prev.p_filesz = size;
          prev.p_memsz  = size;
        } else {
          // If we are adding new load, it must be aligned.
          CHECK_EQ(shdr.sh_addralign, (Elf_Word)kPageSize);
          phdrs.push_back(load);
        }
      }
    }
    for (auto* section : sections_) {
      const Elf_Shdr& shdr = section->header_;
      if ((shdr.sh_flags & SHF_ALLOC) != 0 && shdr.sh_size != 0) {
        // Other PT_* types allow the program to locate interesting
        // parts of memory at runtime. They must overlap with PT_LOAD.
        if (section->phdr_type_ != 0) {
          Elf_Phdr phdr = Elf_Phdr();
          phdr.p_type   = section->phdr_type_;
          phdr.p_flags  = section->phdr_flags_;
          phdr.p_offset = shdr.sh_offset;
          phdr.p_vaddr  = phdr.p_paddr = shdr.sh_addr;
          phdr.p_filesz = phdr.p_memsz = shdr.sh_size;
          phdr.p_align  = shdr.sh_addralign;
          phdrs.push_back(phdr);
        }
      }
    }
    // Set the size of the initial PT_PHDR.
    CHECK_EQ(phdrs[0].p_type, (Elf_Word)PT_PHDR);
    phdrs[0].p_filesz = phdrs[0].p_memsz = phdrs.size() * sizeof(Elf_Phdr);

    return phdrs;
  }

  InstructionSet isa_;

  OutputStream* output_;
  bool output_good_;  // True if all writes to output succeeded.
  off_t output_offset_;  // Keep track of the current position in the stream.

  Section rodata_;
  Section text_;
  Section bss_;
  StringSection dynstr_;
  SymbolSection dynsym_;
  Section hash_;
  Section dynamic_;
  Section eh_frame_;
  Section eh_frame_hdr_;
  StringSection strtab_;
  SymbolSection symtab_;
  Section debug_frame_;
  Section debug_info_;
  Section debug_line_;
  StringSection shstrtab_;
  std::vector<std::unique_ptr<Section>> other_sections_;

  // List of used section in the order in which they were written.
  std::vector<Section*> sections_;

  // Used for allocation of virtual address space.
  Elf_Addr virtual_address_;

  DISALLOW_COPY_AND_ASSIGN(ElfBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_ELF_BUILDER_H_
