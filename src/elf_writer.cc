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

#include "elf_writer.h"

#include "base/unix_file/fd_file.h"
#include "compiler/driver/compiler_driver.h"
#include "elf_file.h"
#include "oat.h"
#include "oat_file.h"

#include <llvm/Support/TargetSelect.h>

#include <mcld/Environment.h>
#include <mcld/IRBuilder.h>
#include <mcld/Linker.h>
#include <mcld/LinkerConfig.h>
#include <mcld/MC/ZOption.h>
#include <mcld/Module.h>
#include <mcld/Support/Path.h>
#include <mcld/Support/TargetSelect.h>

namespace art {

bool ElfWriter::Create(File* file, std::vector<uint8_t>& oat_contents, const CompilerDriver& compiler) {
  ElfWriter elf_writer(&compiler);
  return elf_writer.Write(oat_contents, file);
}

ElfWriter::ElfWriter(const CompilerDriver* driver) : compiler_driver_(driver) {}

ElfWriter::~ElfWriter() {}

static void InitializeLLVM() {
  // TODO: this is lifted from art's compiler_llvm.cc, should be factored out
#if defined(ART_TARGET)
  llvm::InitializeNativeTarget();
  // TODO: odd that there is no InitializeNativeTargetMC?
#else
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
#endif
}

bool ElfWriter::Write(std::vector<uint8_t>& oat_contents, File* elf_file) {

  std::string target_triple;
  std::string target_cpu;
  std::string target_attr;
  CompilerDriver::InstructionSetToLLVMTarget(compiler_driver_->GetInstructionSet(),
                                       target_triple,
                                       target_cpu,
                                       target_attr);

  {
    // Based on mclinker's llvm-mcld.cpp main() and LinkerTest
    //
    // TODO: LinkerTest uses mcld::Initialize(), but it does an
    // llvm::InitializeAllTargets, which we don't want. Basically we
    // want mcld::InitializeNative, but it doesn't exist yet, so we
    // inline the minimal we need here.
    InitializeLLVM();
    mcld::InitializeAllTargets();
    mcld::InitializeAllLinkers();
    mcld::InitializeAllEmulations();
    mcld::InitializeAllDiagnostics();

    UniquePtr<mcld::LinkerConfig> linker_config(new mcld::LinkerConfig(target_triple));
    CHECK(linker_config.get() != NULL);
    linker_config->setCodeGenType(mcld::LinkerConfig::DynObj);
    if (compiler_driver_->GetInstructionSet() == kMips) {
      // MCLinker defaults MIPS section alignment to 0x10000, not 0x1000
      mcld::ZOption z_option;
      z_option.setKind(mcld::ZOption::MaxPageSize);
      z_option.setPageSize(kPageSize);
      linker_config->options().addZOption(z_option);
    }
    linker_config->options().setSOName(elf_file->GetPath());
    // TODO: Wire up mcld DiagnosticEngine to LOG?
    if (false) {
      // enables some tracing of input file processing
      linker_config->options().setTrace(true);
    }

    // Based on alone::Linker::config
    UniquePtr<mcld::Module> module(new mcld::Module(linker_config->options().soname()));
    CHECK(module.get() != NULL);
    UniquePtr<mcld::IRBuilder> ir_builder(new mcld::IRBuilder(*module.get(), *linker_config.get()));
    CHECK(ir_builder.get() != NULL);
    UniquePtr<mcld::Linker> linker(new mcld::Linker());
    CHECK(linker.get() != NULL);
    linker->config(*linker_config.get());


    // Add an artificial memory input. Based on LinkerTest.
    UniquePtr<OatFile> oat_file(OatFile::Open(oat_contents, elf_file->GetPath()));
    CHECK(oat_file.get() != NULL) << elf_file->GetPath();

    const char* oat_data_start = reinterpret_cast<const char*>(&oat_file->GetOatHeader());
    const size_t oat_data_length = oat_file->GetOatHeader().GetExecutableOffset();
    const char* oat_code_start = oat_data_start + oat_data_length;
    const size_t oat_code_length = oat_file->Size() - oat_data_length;

    // TODO: ownership of input?
    mcld::Input* input = ir_builder->CreateInput("oat contents",
                                                 mcld::sys::fs::Path("oat contents path"),
                                                 mcld::Input::Object);
    CHECK(input != NULL);

    // TODO: ownership of null_section?
    mcld::LDSection* null_section = ir_builder->CreateELFHeader(*input,
                                                                "",
                                                                mcld::LDFileFormat::Null,
                                                                llvm::ELF::SHT_NULL,
                                                                0);
    CHECK(null_section != NULL);

    // TODO: we should split readonly data from readonly executable
    // code like .oat does.  We need to control section layout with
    // linker script like functionality to guarantee references
    // between sections maintain relative position which isn't
    // possible right now with the mclinker APIs.
    CHECK(oat_code_start);

    // we need to ensure that oatdata is page aligned so when we
    // fixup the segment load addresses, they remain page aligned.
    uint32_t alignment = kPageSize;

    // TODO: ownership of text_section?
    mcld::LDSection* text_section = ir_builder->CreateELFHeader(*input,
                                                                ".text",
                                                                llvm::ELF::SHT_PROGBITS,
                                                                llvm::ELF::SHF_EXECINSTR
                                                                | llvm::ELF::SHF_ALLOC,
                                                                alignment);
    CHECK(text_section != NULL);

    mcld::SectionData* text_section_data = ir_builder->CreateSectionData(*text_section);
    CHECK(text_section_data != NULL);

    // TODO: why does IRBuilder::CreateRegion take a non-const pointer?
    mcld::Fragment* text_fragment = ir_builder->CreateRegion(const_cast<char*>(oat_data_start),
                                                             oat_file->Size());
    CHECK(text_fragment != NULL);
    ir_builder->AppendFragment(*text_fragment, *text_section_data);

    ir_builder->AddSymbol(*input,
                          "oatdata",
                          mcld::ResolveInfo::Object,
                          mcld::ResolveInfo::Define,
                          mcld::ResolveInfo::Global,
                          oat_data_length,  // size
                          0,                // offset
                          text_section);

    ir_builder->AddSymbol(*input,
                          "oatexec",
                          mcld::ResolveInfo::Function,
                          mcld::ResolveInfo::Define,
                          mcld::ResolveInfo::Global,
                          oat_code_length,  // size
                          oat_data_length,  // offset
                          text_section);

    ir_builder->AddSymbol(*input,
                          "oatlastword",
                          mcld::ResolveInfo::Object,
                          mcld::ResolveInfo::Define,
                          mcld::ResolveInfo::Global,
                          0,                // size
                          // subtract a word so symbol is within section
                          (oat_data_length + oat_code_length) - sizeof(uint32_t),  // offset
                          text_section);

    // link inputs
    if (!linker->link(*module.get(), *ir_builder.get())) {
      LOG(ERROR) << "problem linking " << elf_file->GetPath();
      return false;
    }

    // emited linked output
    if (!linker->emit(elf_file->Fd())) {
      LOG(ERROR) << "problem emitting " << elf_file->GetPath();
      return false;
    }
    // TODO: mcld::Linker::emit closed the file descriptor. It probably shouldn't.
    // For now, close our File to match.
    elf_file->Close();
    mcld::Finalize();
  }
  LOG(INFO) << "ELF file written successfully: " << elf_file->GetPath();
  return true;
}

bool ElfWriter::Fixup(File* file, uintptr_t oat_data_begin) {
  UniquePtr<ElfFile> elf_file(ElfFile::Open(file, true, false));
  CHECK(elf_file.get() != NULL);

  // Lookup "oatdata" symbol address.
  llvm::ELF::Elf32_Addr oatdata_address = elf_file->FindSymbolAddress(llvm::ELF::SHT_DYNSYM,
                                                                      "oatdata");
  CHECK_NE(0U, oatdata_address);
  llvm::ELF::Elf32_Off base_address = oat_data_begin - oatdata_address;

  if (!FixupDynamic(*elf_file.get(), base_address)) {
      LOG(WARNING) << "Failed fo fixup .dynamic in " << file->GetPath();
      return false;
  }
  if (!FixupSectionHeaders(*elf_file.get(), base_address)) {
      LOG(WARNING) << "Failed fo fixup section headers in " << file->GetPath();
      return false;
  }
  if (!FixupProgramHeaders(*elf_file.get(), base_address)) {
      LOG(WARNING) << "Failed fo fixup program headers in " << file->GetPath();
      return false;
  }
  if (!FixupSymbols(*elf_file.get(), base_address, true)) {
      LOG(WARNING) << "Failed fo fixup .dynsym in " << file->GetPath();
      return false;
  }
  if (!FixupSymbols(*elf_file.get(), base_address, false)) {
      LOG(WARNING) << "Failed fo fixup .symtab in " << file->GetPath();
      return false;
  }
  return true;
}

bool ElfWriter::FixupDynamic(ElfFile& elf_file, uintptr_t base_address) {
  // TODO: C++0x auto.
  for (llvm::ELF::Elf32_Word i = 0; i < elf_file.GetDynamicNum(); i++) {
    llvm::ELF::Elf32_Dyn& elf_dyn = elf_file.GetDynamic(i);
    bool elf_dyn_needs_fixup = false;
    // case 1: if Elf32_Dyn.d_tag implies Elf32_Dyn.d_un contains an address in d_ptr
    switch (elf_dyn.d_tag) {
      case llvm::ELF::DT_PLTGOT:
      case llvm::ELF::DT_HASH:
      case llvm::ELF::DT_STRTAB:
      case llvm::ELF::DT_SYMTAB:
      case llvm::ELF::DT_RELA:
      case llvm::ELF::DT_INIT:
      case llvm::ELF::DT_FINI:
      case llvm::ELF::DT_REL:
      case llvm::ELF::DT_DEBUG:
      case llvm::ELF::DT_JMPREL: {
          elf_dyn_needs_fixup = true;
        break;
      }
      default: {
        // case 2: if d_tag is even and greater than  > DT_ENCODING
        if ((elf_dyn.d_tag > llvm::ELF::DT_ENCODING) && ((elf_dyn.d_tag % 2) == 0)) {
          elf_dyn_needs_fixup = true;
        }
        break;
      }
    }
    if (elf_dyn_needs_fixup) {
        uint32_t d_ptr = elf_dyn.d_un.d_ptr;
        d_ptr += base_address;
        elf_dyn.d_un.d_ptr = d_ptr;
    }
  }
  return true;
}

bool ElfWriter::FixupSectionHeaders(ElfFile& elf_file, uintptr_t base_address) {
  for (llvm::ELF::Elf32_Word i = 0; i < elf_file.GetSectionHeaderNum(); i++) {
    llvm::ELF::Elf32_Shdr& sh = elf_file.GetSectionHeader(i);
    // 0 implies that the section will not exist in the memory of the process
    if (sh.sh_addr == 0) {
      continue;
    }
    sh.sh_addr += base_address;
  }
  return true;
}

bool ElfWriter::FixupProgramHeaders(ElfFile& elf_file, uintptr_t base_address) {
  // TODO: ELFObjectFile doesn't have give to Elf32_Phdr, so we do that ourselves for now.
  for (llvm::ELF::Elf32_Word i = 0; i < elf_file.GetProgramHeaderNum(); i++) {
    llvm::ELF::Elf32_Phdr& ph = elf_file.GetProgramHeader(i);
    CHECK_EQ(ph.p_vaddr, ph.p_paddr) << elf_file.GetFile().GetPath() << " i=" << i;
    CHECK((ph.p_align == 0) || (0 == ((ph.p_vaddr - ph.p_offset) & (ph.p_align - 1))));
    ph.p_vaddr += base_address;
    ph.p_paddr += base_address;
    CHECK((ph.p_align == 0) || (0 == ((ph.p_vaddr - ph.p_offset) & (ph.p_align - 1))));
  }
  return true;
}

bool ElfWriter::FixupSymbols(ElfFile& elf_file, uintptr_t base_address, bool dynamic) {
  llvm::ELF::Elf32_Word section_type = dynamic ? llvm::ELF::SHT_DYNSYM : llvm::ELF::SHT_SYMTAB;
  // TODO: Unfortunate ELFObjectFile has protected symbol access, so use ElfFile
  llvm::ELF::Elf32_Shdr* symbol_section = elf_file.FindSectionByType(section_type);
  CHECK(symbol_section != NULL) << elf_file.GetFile().GetPath();
  for (uint32_t i = 0; i < elf_file.GetSymbolNum(*symbol_section); i++) {
    llvm::ELF::Elf32_Sym& symbol = elf_file.GetSymbol(section_type, i);
    if (symbol.st_value != 0) {
      symbol.st_value += base_address;
    }
  }
  return true;
}

void ElfWriter::GetOatElfInformation(File* file,
                                     size_t& oat_loaded_size,
                                     size_t& oat_data_offset) {
  UniquePtr<ElfFile> elf_file(ElfFile::Open(file, false, false));
  CHECK(elf_file.get() != NULL);

  oat_loaded_size = elf_file->GetLoadedSize();
  CHECK_NE(0U, oat_loaded_size);
  oat_data_offset = elf_file->FindSymbolAddress(llvm::ELF::SHT_DYNSYM, "oatdata");
  CHECK_NE(0U, oat_data_offset);
}

}  // namespace art
