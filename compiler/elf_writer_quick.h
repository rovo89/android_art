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
#include "instruction_set.h"

namespace art {

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

  class ElfBuilder;
  void AddDebugSymbols(ElfBuilder& builder,
                       OatWriter* oat_writer,
                       bool debug);
  void ReservePatchSpace(std::vector<uint8_t>* buffer, bool debug);

  class ElfSectionBuilder {
   public:
    ElfSectionBuilder(const std::string& sec_name, Elf32_Word type, Elf32_Word flags,
                      const ElfSectionBuilder *link, Elf32_Word info, Elf32_Word align,
                      Elf32_Word entsize)
        : name_(sec_name), link_(link) {
      memset(&section_, 0, sizeof(section_));
      section_.sh_type = type;
      section_.sh_flags = flags;
      section_.sh_info = info;
      section_.sh_addralign = align;
      section_.sh_entsize = entsize;
    }

    virtual ~ElfSectionBuilder() {}

    Elf32_Shdr section_;
    Elf32_Word section_index_ = 0;

   protected:
    const std::string name_;
    const ElfSectionBuilder* link_;

    Elf32_Word GetLink() {
      return (link_) ? link_->section_index_ : 0;
    }

   private:
    friend class ElfBuilder;
  };

  class ElfDynamicBuilder : public ElfSectionBuilder {
   public:
    void AddDynamicTag(Elf32_Sword tag, Elf32_Word d_un);
    void AddDynamicTag(Elf32_Sword tag, Elf32_Word offset, ElfSectionBuilder* section);

    ElfDynamicBuilder(const std::string& sec_name, ElfSectionBuilder *link)
        : ElfSectionBuilder(sec_name, SHT_DYNAMIC, SHF_ALLOC | SHF_ALLOC, link,
                            0, kPageSize, sizeof(Elf32_Dyn)) {}
    ~ElfDynamicBuilder() {}

   protected:
    struct ElfDynamicState {
      ElfSectionBuilder* section_;
      Elf32_Sword tag_;
      Elf32_Word off_;
    };
    std::vector<ElfDynamicState> dynamics_;
    Elf32_Word GetSize() {
      // Add 1 for the DT_NULL, 1 for DT_STRSZ, and 1 for DT_SONAME. All of
      // these must be added when we actually put the file together because
      // their values are very dependent on state.
      return dynamics_.size() + 3;
    }

    // Create the actual dynamic vector. strsz should be the size of the .dynstr
    // table and soname_off should be the offset of the soname in .dynstr.
    // Since niether can be found prior to final layout we will wait until here
    // to add them.
    std::vector<Elf32_Dyn> GetDynamics(Elf32_Word strsz, Elf32_Word soname_off);

   private:
    friend class ElfBuilder;
  };

  class ElfRawSectionBuilder : public ElfSectionBuilder {
   public:
    ElfRawSectionBuilder(const std::string& sec_name, Elf32_Word type, Elf32_Word flags,
                         const ElfSectionBuilder* link, Elf32_Word info, Elf32_Word align,
                         Elf32_Word entsize)
        : ElfSectionBuilder(sec_name, type, flags, link, info, align, entsize) {}
    ~ElfRawSectionBuilder() {}
    std::vector<uint8_t>* GetBuffer() { return &buf_; }
    void SetBuffer(std::vector<uint8_t> buf) { buf_ = buf; }

   protected:
    std::vector<uint8_t> buf_;

   private:
    friend class ElfBuilder;
  };

  class ElfOatSectionBuilder : public ElfSectionBuilder {
   public:
    ElfOatSectionBuilder(const std::string& sec_name, Elf32_Word size, Elf32_Word offset,
                         Elf32_Word type, Elf32_Word flags)
        : ElfSectionBuilder(sec_name, type, flags, NULL, 0, kPageSize, 0),
          offset_(offset), size_(size) {}
    ~ElfOatSectionBuilder() {}

   protected:
    // Offset of the content within the file.
    Elf32_Word offset_;
    // Size of the content within the file.
    Elf32_Word size_;

   private:
    friend class ElfBuilder;
  };

  class ElfSymtabBuilder : public ElfSectionBuilder {
   public:
    // Add a symbol with given name to this symtab. The symbol refers to
    // 'relative_addr' within the given section and has the given attributes.
    void AddSymbol(const std::string& name,
                   const ElfSectionBuilder* section,
                   Elf32_Addr addr,
                   bool is_relative,
                   Elf32_Word size,
                   uint8_t binding,
                   uint8_t type,
                   uint8_t other = 0);

    ElfSymtabBuilder(const std::string& sec_name, Elf32_Word type,
                     const std::string& str_name, Elf32_Word str_type, bool alloc)
        : ElfSectionBuilder(sec_name, type, ((alloc) ? SHF_ALLOC : 0U), &strtab_, 0,
                            sizeof(Elf32_Word), sizeof(Elf32_Sym)),
          str_name_(str_name), str_type_(str_type),
          strtab_(str_name, str_type, ((alloc) ? SHF_ALLOC : 0U), NULL, 0, 1, 1) {}
    ~ElfSymtabBuilder() {}

   protected:
    std::vector<Elf32_Word> GenerateHashContents();
    std::string GenerateStrtab();
    std::vector<Elf32_Sym> GenerateSymtab();

    Elf32_Word GetSize() {
      // 1 is for the implicit NULL symbol.
      return symbols_.size() + 1;
    }

    struct ElfSymbolState {
      const std::string name_;
      const ElfSectionBuilder* section_;
      Elf32_Addr addr_;
      Elf32_Word size_;
      bool is_relative_;
      uint8_t info_;
      uint8_t other_;
      // Used during Write() to temporarially hold name index in the strtab.
      Elf32_Word name_idx_;
    };

    // Information for the strsym for dynstr sections.
    const std::string str_name_;
    Elf32_Word str_type_;
    // The symbols in the same order they will be in the symbol table.
    std::vector<ElfSymbolState> symbols_;
    ElfSectionBuilder strtab_;

   private:
    friend class ElfBuilder;
  };

  class ElfBuilder FINAL {
   public:
    ElfBuilder(OatWriter* oat_writer,
               File* elf_file,
               InstructionSet isa,
               Elf32_Word rodata_relative_offset,
               Elf32_Word rodata_size,
               Elf32_Word text_relative_offset,
               Elf32_Word text_size,
               const bool add_symbols,
               bool debug = false)
        : oat_writer_(oat_writer),
          elf_file_(elf_file),
          add_symbols_(add_symbols),
          debug_logging_(debug),
          text_builder_(".text", text_size, text_relative_offset, SHT_PROGBITS,
                        SHF_ALLOC | SHF_EXECINSTR),
          rodata_builder_(".rodata", rodata_size, rodata_relative_offset,
                          SHT_PROGBITS, SHF_ALLOC),
          dynsym_builder_(".dynsym", SHT_DYNSYM, ".dynstr", SHT_STRTAB, true),
          symtab_builder_(".symtab", SHT_SYMTAB, ".strtab", SHT_STRTAB, false),
          hash_builder_(".hash", SHT_HASH, SHF_ALLOC, &dynsym_builder_, 0,
                        sizeof(Elf32_Word), sizeof(Elf32_Word)),
          dynamic_builder_(".dynamic", &dynsym_builder_),
          shstrtab_builder_(".shstrtab", SHT_STRTAB, 0, NULL, 0, 1, 1) {
      SetupEhdr();
      SetupDynamic();
      SetupRequiredSymbols();
      SetISA(isa);
    }
    ~ElfBuilder() {}

    bool Write();

    // Adds the given raw section to the builder. This will copy it. The caller
    // is responsible for deallocating their copy.
    void RegisterRawSection(ElfRawSectionBuilder bld) {
      other_builders_.push_back(bld);
    }

   private:
    OatWriter* oat_writer_;
    File* elf_file_;
    const bool add_symbols_;
    const bool debug_logging_;

    bool fatal_error_ = false;

    Elf32_Ehdr elf_header_;

   public:
    ElfOatSectionBuilder text_builder_;
    ElfOatSectionBuilder rodata_builder_;
    ElfSymtabBuilder dynsym_builder_;
    ElfSymtabBuilder symtab_builder_;
    ElfSectionBuilder hash_builder_;
    ElfDynamicBuilder dynamic_builder_;
    ElfSectionBuilder shstrtab_builder_;
    std::vector<ElfRawSectionBuilder> other_builders_;

   private:
    void SetISA(InstructionSet isa);
    void SetupEhdr();

    // Sets up a bunch of the required Dynamic Section entries.
    // Namely it will initialize all the mandatory ones that it can.
    // Specifically:
    // DT_HASH
    // DT_STRTAB
    // DT_SYMTAB
    // DT_SYMENT
    //
    // Some such as DT_SONAME, DT_STRSZ and DT_NULL will be put in later.
    void SetupDynamic();

    // Sets up the basic dynamic symbols that are needed, namely all those we
    // can know already.
    //
    // Specifically adds:
    // oatdata
    // oatexec
    // oatlastword
    void SetupRequiredSymbols();
    void AssignSectionStr(ElfSectionBuilder *builder, std::string* strtab);
    struct ElfFilePiece {
      ElfFilePiece(const std::string& name, Elf32_Word offset, const void* data, Elf32_Word size)
          : dbg_name_(name), offset_(offset), data_(data), size_(size) {}
      ~ElfFilePiece() {}

      const std::string& dbg_name_;
      Elf32_Word offset_;
      const void *data_;
      Elf32_Word size_;
      static bool Compare(ElfFilePiece a, ElfFilePiece b) {
        return a.offset_ < b.offset_;
      }
    };

    // Write each of the pieces out to the file.
    bool WriteOutFile(const std::vector<ElfFilePiece>& pieces);
    bool IncludingDebugSymbols() { return add_symbols_ && symtab_builder_.GetSize() > 1; }
  };

  /*
   * @brief Generate the DWARF debug_info and debug_abbrev sections
   * @param oat_writer The Oat file Writer.
   * @param dbg_info Compilation unit information.
   * @param dbg_abbrev Abbreviations used to generate dbg_info.
   * @param dbg_str Debug strings.
   */
  void FillInCFIInformation(OatWriter* oat_writer, std::vector<uint8_t>* dbg_info,
                            std::vector<uint8_t>* dbg_abbrev, std::vector<uint8_t>* dbg_str);

  DISALLOW_IMPLICIT_CONSTRUCTORS(ElfWriterQuick);
};

}  // namespace art

#endif  // ART_COMPILER_ELF_WRITER_QUICK_H_
