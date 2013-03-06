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

#include <llvm/Support/TargetSelect.h>

#include <mcld/Environment.h>
#include <mcld/IRBuilder.h>
#include <mcld/Linker.h>
#include <mcld/LinkerConfig.h>
#include <mcld/MC/ZOption.h>
#include <mcld/Module.h>
#include <mcld/Support/Path.h>
#include <mcld/Support/TargetSelect.h>

#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "compiler/driver/compiler_driver.h"
#include "compiler/llvm/utils_llvm.h"
#include "dex_method_iterator.h"
#include "elf_file.h"
#include "invoke_type.h"
#include "mirror/abstract_method-inl.h"
#include "oat.h"
#include "oat_file.h"
#include "scoped_thread_state_change.h"

namespace art {

bool ElfWriter::Create(File* elf_file,
                       std::vector<uint8_t>& oat_contents,
                       const std::vector<const DexFile*>& dex_files,
                       const std::string* host_prefix,
                       bool is_host,
                       const CompilerDriver& driver) {
  ElfWriter elf_writer(driver, elf_file);
  return elf_writer.Write(oat_contents, dex_files, host_prefix, is_host);
}

ElfWriter::ElfWriter(const CompilerDriver& driver, File* elf_file)
  : compiler_driver_(&driver), elf_file_(elf_file), oat_input_(NULL) {}

ElfWriter::~ElfWriter() {}

bool ElfWriter::Write(std::vector<uint8_t>& oat_contents,
                      const std::vector<const DexFile*>& dex_files,
                      const std::string* host_prefix,
                      bool is_host) {
  Init();
  AddOatInput(oat_contents);
#if defined(ART_USE_PORTABLE_COMPILER)
  AddMethodInputs(dex_files);
  AddRuntimeInputs(host_prefix, is_host);
#endif
  if (!Link()) {
    return false;
  }
#if defined(ART_USE_PORTABLE_COMPILER)
  FixupOatMethodOffsets(dex_files);
#endif
  return true;
}

static void InitializeLLVM() {
  // TODO: this is lifted from art's compiler_llvm.cc, should be factored out
  if (kIsTargetBuild) {
    llvm::InitializeNativeTarget();
    // TODO: odd that there is no InitializeNativeTargetMC?
  } else {
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
  }
}

void ElfWriter::Init() {
  std::string target_triple;
  std::string target_cpu;
  std::string target_attr;
  CompilerDriver::InstructionSetToLLVMTarget(compiler_driver_->GetInstructionSet(),
                                       target_triple,
                                       target_cpu,
                                       target_attr);

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

  linker_config_.reset(new mcld::LinkerConfig(target_triple));
  CHECK(linker_config_.get() != NULL);
  linker_config_->setCodeGenType(mcld::LinkerConfig::DynObj);
  linker_config_->options().setSOName(elf_file_->GetPath());

  // error on undefined symbols.
  // TODO: should this just be set if kIsDebugBuild?
  linker_config_->options().setNoUndefined(true);

  // TODO: Wire up mcld DiagnosticEngine to LOG?
  linker_config_->options().setColor(false);
  if (false) {
    // enables some tracing of input file processing
    linker_config_->options().setTrace(true);
  }

  // Based on alone::Linker::config
  module_.reset(new mcld::Module(linker_config_->options().soname()));
  CHECK(module_.get() != NULL);
  ir_builder_.reset(new mcld::IRBuilder(*module_.get(), *linker_config_.get()));
  CHECK(ir_builder_.get() != NULL);
  linker_.reset(new mcld::Linker());
  CHECK(linker_.get() != NULL);
  linker_->config(*linker_config_.get());
}

void ElfWriter::AddOatInput(std::vector<uint8_t>& oat_contents) {
  // Add an artificial memory input. Based on LinkerTest.
  UniquePtr<OatFile> oat_file(OatFile::OpenMemory(oat_contents, elf_file_->GetPath()));
  CHECK(oat_file.get() != NULL) << elf_file_->GetPath();

  const char* oat_data_start = reinterpret_cast<const char*>(&oat_file->GetOatHeader());
  const size_t oat_data_length = oat_file->GetOatHeader().GetExecutableOffset();
  const char* oat_code_start = oat_data_start + oat_data_length;
  const size_t oat_code_length = oat_file->Size() - oat_data_length;

  // TODO: ownership of oat_input?
  oat_input_ = ir_builder_->CreateInput("oat contents",
                                        mcld::sys::fs::Path("oat contents path"),
                                        mcld::Input::Object);
  CHECK(oat_input_ != NULL);

  // TODO: ownership of null_section?
  mcld::LDSection* null_section = ir_builder_->CreateELFHeader(*oat_input_,
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
  CHECK(oat_code_start != NULL);

  // we need to ensure that oatdata is page aligned so when we
  // fixup the segment load addresses, they remain page aligned.
  uint32_t alignment = kPageSize;

  // TODO: ownership of text_section?
  mcld::LDSection* text_section = ir_builder_->CreateELFHeader(*oat_input_,
                                                               ".text",
                                                               llvm::ELF::SHT_PROGBITS,
                                                               llvm::ELF::SHF_EXECINSTR
                                                               | llvm::ELF::SHF_ALLOC,
                                                               alignment);
  CHECK(text_section != NULL);

  mcld::SectionData* text_sectiondata = ir_builder_->CreateSectionData(*text_section);
  CHECK(text_sectiondata != NULL);

  // TODO: why does IRBuilder::CreateRegion take a non-const pointer?
  mcld::Fragment* text_fragment = ir_builder_->CreateRegion(const_cast<char*>(oat_data_start),
                                                            oat_file->Size());
  CHECK(text_fragment != NULL);
  ir_builder_->AppendFragment(*text_fragment, *text_sectiondata);

  ir_builder_->AddSymbol(*oat_input_,
                         "oatdata",
                         mcld::ResolveInfo::Object,
                         mcld::ResolveInfo::Define,
                         mcld::ResolveInfo::Global,
                         oat_data_length,  // size
                         0,                // offset
                         text_section);

  ir_builder_->AddSymbol(*oat_input_,
                         "oatexec",
                         mcld::ResolveInfo::Function,
                         mcld::ResolveInfo::Define,
                         mcld::ResolveInfo::Global,
                         oat_code_length,  // size
                         oat_data_length,  // offset
                         text_section);

  ir_builder_->AddSymbol(*oat_input_,
                         "oatlastword",
                         mcld::ResolveInfo::Object,
                         mcld::ResolveInfo::Define,
                         mcld::ResolveInfo::Global,
                         0,                // size
                         // subtract a word so symbol is within section
                         (oat_data_length + oat_code_length) - sizeof(uint32_t),  // offset
                         text_section);
}

#if defined(ART_USE_PORTABLE_COMPILER)
void ElfWriter::AddMethodInputs(const std::vector<const DexFile*>& dex_files) {
  DCHECK(oat_input_ != NULL);

  DexMethodIterator it(dex_files);
  while (it.HasNext()) {
    const DexFile& dex_file = it.GetDexFile();
    uint32_t method_idx = it.GetMemberIndex();
    InvokeType invoke_type = it.GetInvokeType();
    const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
    const CompiledMethod* compiled_method =
      compiler_driver_->GetCompiledMethod(CompilerDriver::MethodReference(&dex_file, method_idx));
    if (compiled_method != NULL) {
      AddCompiledCodeInput(*compiled_method);
    }
    const CompiledInvokeStub* compiled_invoke_stub = compiler_driver_->FindInvokeStub(invoke_type == kStatic,
                                                                                      shorty);
    if (compiled_invoke_stub != NULL) {
      AddCompiledCodeInput(*compiled_invoke_stub);
    }

    if (invoke_type != kStatic) {
      const CompiledInvokeStub* compiled_proxy_stub = compiler_driver_->FindProxyStub(shorty);
      if (compiled_proxy_stub != NULL) {
        AddCompiledCodeInput(*compiled_proxy_stub);
      }
    }
    it.Next();
  }
  added_symbols_.clear();
}

void ElfWriter::AddCompiledCodeInput(const CompiledCode& compiled_code) {
  // Check if we've seen this compiled code before. If so skip
  // it. This can happen for reused code such as invoke stubs.
  const std::string& symbol = compiled_code.GetSymbol();
  SafeMap<const std::string*, const std::string*>::iterator it = added_symbols_.find(&symbol);
  if (it != added_symbols_.end()) {
    return;
  }
  added_symbols_.Put(&symbol, &symbol);

  // Add input to supply code for symbol
  const std::vector<uint8_t>& code = compiled_code.GetCode();
  // TODO: ownership of code_input?
  // TODO: why does IRBuilder::ReadInput take a non-const pointer?
  mcld::Input* code_input = ir_builder_->ReadInput(symbol,
                                                   const_cast<uint8_t*>(&code[0]),
                                                   code.size());
  CHECK(code_input != NULL);
}

void ElfWriter::AddRuntimeInputs(const std::string* host_prefix, bool is_host) {
  std::string android_root;
  if (is_host) {
    const char* android_host_out = getenv("ANDROID_HOST_OUT");
    CHECK(android_host_out != NULL) << "ANDROID_HOST_OUT environment variable not set";
    android_root += android_host_out;
  } else {
    if (host_prefix != NULL) {
      android_root += *host_prefix;
      android_root += "/system";
    } else {
      android_root += GetAndroidRoot();
    }
  }

  std::string libart_so(android_root);
  libart_so += kIsDebugBuild ? "/lib/libartd.so" : "/lib/libart.so";
  // TODO: ownership of libart_so_input?
  mcld::Input* libart_so_input = ir_builder_->ReadInput(libart_so, libart_so);
  CHECK(libart_so_input != NULL);

  std::string host_prebuilt_dir("prebuilts/gcc/linux-x86/host/i686-linux-glibc2.7-4.6");

  std::string compiler_runtime_lib;
  if (is_host) {
    compiler_runtime_lib += host_prebuilt_dir;
    compiler_runtime_lib += "/lib/gcc/i686-linux/4.6.x-google/libgcc.a";
  } else {
    compiler_runtime_lib += android_root;
    compiler_runtime_lib += "/lib/libcompiler-rt.a";
  }
  // TODO: ownership of compiler_runtime_lib_input?
  mcld::Input* compiler_runtime_lib_input = ir_builder_->ReadInput(compiler_runtime_lib,
                                                                   compiler_runtime_lib);
  CHECK(compiler_runtime_lib_input != NULL);

  std::string libc_lib;
  if (is_host) {
    libc_lib += host_prebuilt_dir;
    libc_lib += "/sysroot/usr/lib/libc.so.6";
  } else {
    libc_lib += android_root;
    libc_lib += "/lib/libc.so";
  }
  // TODO: ownership of libc_lib_input?
  mcld::Input* libc_lib_input_input = ir_builder_->ReadInput(libc_lib, libc_lib);
  CHECK(libc_lib_input_input != NULL);

  std::string libm_lib;
  if (is_host) {
    libm_lib += host_prebuilt_dir;
    libm_lib += "/sysroot/usr/lib/libm.so";
  } else {
    libm_lib += android_root;
    libm_lib += "/lib/libm.so";
  }
  // TODO: ownership of libm_lib_input?
  mcld::Input* libm_lib_input_input = ir_builder_->ReadInput(libm_lib, libm_lib);
  CHECK(libm_lib_input_input != NULL);

}
#endif

bool ElfWriter::Link() {
  // link inputs
  if (!linker_->link(*module_.get(), *ir_builder_.get())) {
    LOG(ERROR) << "Failed to link " << elf_file_->GetPath();
    return false;
  }

  // emit linked output
  // TODO: avoid dup of fd by fixing Linker::emit to not close the argument fd.
  int fd = dup(elf_file_->Fd());
  if (fd == -1) {
    PLOG(ERROR) << "Failed to dup file descriptor for " << elf_file_->GetPath();
    return false;
  }
  if (!linker_->emit(fd)) {
    LOG(ERROR) << "Failed to emit " << elf_file_->GetPath();
    return false;
  }
  mcld::Finalize();
  LOG(INFO) << "ELF file written successfully: " << elf_file_->GetPath();
  return true;
}

static llvm::ELF::Elf32_Addr GetOatDataAddress(ElfFile* elf_file) {
  llvm::ELF::Elf32_Addr oatdata_address = elf_file->FindSymbolAddress(llvm::ELF::SHT_DYNSYM,
                                                                      "oatdata",
                                                                      false);
  CHECK_NE(0U, oatdata_address);
  return oatdata_address;
}

#if defined(ART_USE_PORTABLE_COMPILER)
void ElfWriter::FixupOatMethodOffsets(const std::vector<const DexFile*>& dex_files) {
  UniquePtr<ElfFile> elf_file(ElfFile::Open(elf_file_, true, false));
  CHECK(elf_file.get() != NULL) << elf_file_->GetPath();

  llvm::ELF::Elf32_Addr oatdata_address = GetOatDataAddress(elf_file.get());
  DexMethodIterator it(dex_files);
  while (it.HasNext()) {
    const DexFile& dex_file = it.GetDexFile();
    uint32_t method_idx = it.GetMemberIndex();
    InvokeType invoke_type = it.GetInvokeType();
    const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
    mirror::AbstractMethod* method = NULL;
    if (compiler_driver_->IsImage()) {
      ClassLinker* linker = Runtime::Current()->GetClassLinker();
      mirror::DexCache* dex_cache = linker->FindDexCache(dex_file);
      // Unchecked as we hold mutator_lock_ on entry.
      ScopedObjectAccessUnchecked soa(Thread::Current());
      method = linker->ResolveMethod(dex_file, method_idx, dex_cache, NULL, NULL, invoke_type);
      CHECK(method != NULL);
    }
    const CompiledMethod* compiled_method =
      compiler_driver_->GetCompiledMethod(CompilerDriver::MethodReference(&dex_file, method_idx));
    if (compiled_method != NULL) {
      uint32_t offset = FixupCompiledCodeOffset(*elf_file.get(), oatdata_address, *compiled_method);
      // Don't overwrite static method trampoline
      if (method != NULL &&
          (!method->IsStatic() ||
           method->IsConstructor() ||
           method->GetDeclaringClass()->IsInitialized())) {
        method->SetOatCodeOffset(offset);
      }
    }
    const CompiledInvokeStub* compiled_invoke_stub = compiler_driver_->FindInvokeStub(invoke_type == kStatic,
                                                                                      shorty);
    if (compiled_invoke_stub != NULL) {
      uint32_t offset = FixupCompiledCodeOffset(*elf_file.get(), oatdata_address, *compiled_invoke_stub);
      if (method != NULL) {
        method->SetOatInvokeStubOffset(offset);
      }
    }

    if (invoke_type != kStatic) {
      const CompiledInvokeStub* compiled_proxy_stub = compiler_driver_->FindProxyStub(shorty);
      if (compiled_proxy_stub != NULL) {
        FixupCompiledCodeOffset(*elf_file.get(), oatdata_address, *compiled_proxy_stub);
      }
    }
    it.Next();
  }
  symbol_to_compiled_code_offset_.clear();
}

uint32_t ElfWriter::FixupCompiledCodeOffset(ElfFile& elf_file,
                                            llvm::ELF::Elf32_Addr oatdata_address,
                                            const CompiledCode& compiled_code) {
  const std::string& symbol = compiled_code.GetSymbol();
  SafeMap<const std::string*, uint32_t>::iterator it = symbol_to_compiled_code_offset_.find(&symbol);
  if (it != symbol_to_compiled_code_offset_.end()) {
    return it->second;
  }

  llvm::ELF::Elf32_Addr compiled_code_address = elf_file.FindSymbolAddress(llvm::ELF::SHT_SYMTAB,
                                                                           symbol,
                                                                           true);
  CHECK_NE(0U, compiled_code_address) << symbol;
  CHECK_LT(oatdata_address, compiled_code_address) << symbol;
  uint32_t compiled_code_offset = compiled_code_address - oatdata_address;
  symbol_to_compiled_code_offset_.Put(&symbol, compiled_code_offset);

  const std::vector<uint32_t>& offsets = compiled_code.GetOatdataOffsetsToCompliledCodeOffset();
  for (uint32_t i = 0; i < offsets.size(); i++) {
    uint32_t oatdata_offset = oatdata_address + offsets[i];
    uint32_t* addr = reinterpret_cast<uint32_t*>(elf_file.Begin() + oatdata_offset);
    *addr = compiled_code_offset;
  }
  return compiled_code_offset;
}
#endif

bool ElfWriter::Fixup(File* file, uintptr_t oat_data_begin) {
  UniquePtr<ElfFile> elf_file(ElfFile::Open(file, true, false));
  CHECK(elf_file.get() != NULL);

  // Lookup "oatdata" symbol address.
  ::llvm::ELF::Elf32_Addr oatdata_address = GetOatDataAddress(elf_file.get());
  ::llvm::ELF::Elf32_Off base_address = oat_data_begin - oatdata_address;

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
  if (!FixupRelocations(*elf_file.get(), base_address)) {
      LOG(WARNING) << "Failed fo fixup .rel.dyn in " << file->GetPath();
      return false;
  }
  return true;
}

bool ElfWriter::FixupDynamic(ElfFile& elf_file, uintptr_t base_address) {
  // TODO: C++0x auto.
  for (::llvm::ELF::Elf32_Word i = 0; i < elf_file.GetDynamicNum(); i++) {
    ::llvm::ELF::Elf32_Dyn& elf_dyn = elf_file.GetDynamic(i);
    bool elf_dyn_needs_fixup = false;
    // case 1: if Elf32_Dyn.d_tag implies Elf32_Dyn.d_un contains an address in d_ptr
    switch (elf_dyn.d_tag) {
      case ::llvm::ELF::DT_PLTGOT:
      case ::llvm::ELF::DT_HASH:
      case ::llvm::ELF::DT_STRTAB:
      case ::llvm::ELF::DT_SYMTAB:
      case ::llvm::ELF::DT_RELA:
      case ::llvm::ELF::DT_INIT:
      case ::llvm::ELF::DT_FINI:
      case ::llvm::ELF::DT_REL:
      case ::llvm::ELF::DT_DEBUG:
      case ::llvm::ELF::DT_JMPREL: {
          elf_dyn_needs_fixup = true;
        break;
      }
      default: {
        // case 2: if d_tag is even and greater than  > DT_ENCODING
        if ((elf_dyn.d_tag > ::llvm::ELF::DT_ENCODING) && ((elf_dyn.d_tag % 2) == 0)) {
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
  for (::llvm::ELF::Elf32_Word i = 0; i < elf_file.GetSectionHeaderNum(); i++) {
    ::llvm::ELF::Elf32_Shdr& sh = elf_file.GetSectionHeader(i);
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
  for (::llvm::ELF::Elf32_Word i = 0; i < elf_file.GetProgramHeaderNum(); i++) {
    ::llvm::ELF::Elf32_Phdr& ph = elf_file.GetProgramHeader(i);
    CHECK_EQ(ph.p_vaddr, ph.p_paddr) << elf_file.GetFile().GetPath() << " i=" << i;
    CHECK((ph.p_align == 0) || (0 == ((ph.p_vaddr - ph.p_offset) & (ph.p_align - 1))));
    ph.p_vaddr += base_address;
    ph.p_paddr += base_address;
    CHECK((ph.p_align == 0) || (0 == ((ph.p_vaddr - ph.p_offset) & (ph.p_align - 1))));
  }
  return true;
}

bool ElfWriter::FixupSymbols(ElfFile& elf_file, uintptr_t base_address, bool dynamic) {
  ::llvm::ELF::Elf32_Word section_type = dynamic ? ::llvm::ELF::SHT_DYNSYM : ::llvm::ELF::SHT_SYMTAB;
  // TODO: Unfortunate ELFObjectFile has protected symbol access, so use ElfFile
  ::llvm::ELF::Elf32_Shdr* symbol_section = elf_file.FindSectionByType(section_type);
  CHECK(symbol_section != NULL) << elf_file.GetFile().GetPath();
  for (uint32_t i = 0; i < elf_file.GetSymbolNum(*symbol_section); i++) {
    ::llvm::ELF::Elf32_Sym& symbol = elf_file.GetSymbol(section_type, i);
    if (symbol.st_value != 0) {
      symbol.st_value += base_address;
    }
  }
  return true;
}

bool ElfWriter::FixupRelocations(ElfFile& elf_file, uintptr_t base_address) {
  for (llvm::ELF::Elf32_Word i = 0; i < elf_file.GetSectionHeaderNum(); i++) {
    llvm::ELF::Elf32_Shdr& sh = elf_file.GetSectionHeader(i);
    if (sh.sh_type == llvm::ELF::SHT_REL) {
      for (uint32_t i = 0; i < elf_file.GetRelNum(sh); i++) {
        llvm::ELF::Elf32_Rel& rel = elf_file.GetRel(sh, i);
        rel.r_offset += base_address;
      }
    } else if (sh.sh_type == llvm::ELF::SHT_RELA) {
      for (uint32_t i = 0; i < elf_file.GetRelaNum(sh); i++) {
        llvm::ELF::Elf32_Rela& rela = elf_file.GetRela(sh, i);
        rela.r_offset += base_address;
      }
    }
  }
  return true;
}

bool ElfWriter::Strip(File* file) {
  UniquePtr<ElfFile> elf_file(ElfFile::Open(file, true, false));
  CHECK(elf_file.get() != NULL);

  // ELF files produced by MCLinker look roughly like this
  //
  // +------------+
  // | Elf32_Ehdr | contains number of Elf32_Shdr and offset to first
  // +------------+
  // | Elf32_Phdr | program headers
  // | Elf32_Phdr |
  // | ...        |
  // | Elf32_Phdr |
  // +------------+
  // | section    | mixture of needed and unneeded sections
  // +------------+
  // | section    |
  // +------------+
  // | ...        |
  // +------------+
  // | section    |
  // +------------+
  // | Elf32_Shdr | section headers
  // | Elf32_Shdr |
  // | ...        | contains offset to section start
  // | Elf32_Shdr |
  // +------------+
  //
  // To strip:
  // - leave the Elf32_Ehdr and Elf32_Phdr values in place.
  // - walk the sections making a new set of Elf32_Shdr section headers for what we want to keep
  // - move the sections are keeping up to fill in gaps of sections we want to strip
  // - write new Elf32_Shdr section headers to end of file, updating Elf32_Ehdr
  // - truncate rest of file
  //

  std::vector<llvm::ELF::Elf32_Shdr> section_headers;
  std::vector<llvm::ELF::Elf32_Word> section_headers_original_indexes;
  section_headers.reserve(elf_file->GetSectionHeaderNum());


  llvm::ELF::Elf32_Shdr& string_section = elf_file->GetSectionNameStringSection();
  for (llvm::ELF::Elf32_Word i = 0; i < elf_file->GetSectionHeaderNum(); i++) {
    llvm::ELF::Elf32_Shdr& sh = elf_file->GetSectionHeader(i);
    const char* name = elf_file->GetString(string_section, sh.sh_name);
    if (name == NULL) {
      CHECK_EQ(0U, i);
      section_headers.push_back(sh);
      section_headers_original_indexes.push_back(0);
      continue;
    }
    if (StartsWith(name, ".debug")
        || (strcmp(name, ".strtab") == 0)
        || (strcmp(name, ".symtab") == 0)) {
      continue;
    }
    section_headers.push_back(sh);
    section_headers_original_indexes.push_back(i);
  }
  CHECK_NE(0U, section_headers.size());
  CHECK_EQ(section_headers.size(), section_headers_original_indexes.size());

  // section 0 is the NULL section, sections start at offset of first section
  llvm::ELF::Elf32_Off offset = elf_file->GetSectionHeader(1).sh_offset;
  for (size_t i = 1; i < section_headers.size(); i++) {
    llvm::ELF::Elf32_Shdr& new_sh = section_headers[i];
    llvm::ELF::Elf32_Shdr& old_sh = elf_file->GetSectionHeader(section_headers_original_indexes[i]);
    CHECK_EQ(new_sh.sh_name, old_sh.sh_name);
    if (old_sh.sh_addralign > 1) {
      offset = RoundUp(offset, old_sh.sh_addralign);
    }
    if (old_sh.sh_offset == offset) {
      // already in place
      offset += old_sh.sh_size;
      continue;
    }
    // shift section earlier
    memmove(elf_file->Begin() + offset,
            elf_file->Begin() + old_sh.sh_offset,
            old_sh.sh_size);
    new_sh.sh_offset = offset;
    offset += old_sh.sh_size;
  }

  llvm::ELF::Elf32_Off shoff = offset;
  size_t section_headers_size_in_bytes = section_headers.size() * sizeof(llvm::ELF::Elf32_Shdr);
  memcpy(elf_file->Begin() + offset, &section_headers[0], section_headers_size_in_bytes);
  offset += section_headers_size_in_bytes;

  elf_file->GetHeader().e_shnum = section_headers.size();
  elf_file->GetHeader().e_shoff = shoff;
  int result = ftruncate(file->Fd(), offset);
  if (result != 0) {
    PLOG(ERROR) << "Failed to truncate while stripping ELF file: " << file->GetPath();
    return false;
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
  oat_data_offset = GetOatDataAddress(elf_file.get());
  CHECK_NE(0U, oat_data_offset);
}

}  // namespace art
