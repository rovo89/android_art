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

#include "compiler_backend.h"
#include "elf_writer_quick.h"
#include "dex/quick/mir_to_lir.h"
#include "dex/mir_graph.h"
#include "driver/compiler_driver.h"
#include "mirror/art_method-inl.h"

#ifdef ART_USE_PORTABLE_COMPILER
#include "dex/portable/mir_to_gbc.h"
#include "elf_writer_mclinker.h"
#endif

namespace art {

#ifdef ART_SEA_IR_MODE
extern "C" art::CompiledMethod* SeaIrCompileMethod(art::CompilerDriver& compiler,
                                                   const art::DexFile::CodeItem* code_item,
                                                   uint32_t access_flags,
                                                   art::InvokeType invoke_type,
                                                   uint16_t class_def_idx,
                                                   uint32_t method_idx,
                                                   jobject class_loader,
                                                   const art::DexFile& dex_file);
#endif

extern "C" void ArtInitQuickCompilerContext(art::CompilerDriver& driver);
extern "C" void ArtUnInitQuickCompilerContext(art::CompilerDriver& driver);
extern "C" art::CompiledMethod* ArtQuickCompileMethod(art::CompilerDriver& compiler,
                                                      const art::DexFile::CodeItem* code_item,
                                                      uint32_t access_flags,
                                                      art::InvokeType invoke_type,
                                                      uint16_t class_def_idx,
                                                      uint32_t method_idx,
                                                      jobject class_loader,
                                                      const art::DexFile& dex_file);

extern "C" art::CompiledMethod* ArtQuickJniCompileMethod(art::CompilerDriver& compiler,
                                                         uint32_t access_flags, uint32_t method_idx,
                                                         const art::DexFile& dex_file);


static CompiledMethod* TryCompileWithSeaIR(art::CompilerDriver& compiler,
                                           const art::DexFile::CodeItem* code_item,
                                           uint32_t access_flags,
                                           art::InvokeType invoke_type,
                                           uint16_t class_def_idx,
                                           uint32_t method_idx,
                                           jobject class_loader,
                                           const art::DexFile& dex_file) {
#ifdef ART_SEA_IR_MODE
    bool use_sea = Runtime::Current()->IsSeaIRMode();
    use_sea = use_sea &&
        (std::string::npos != PrettyMethod(method_idx, dex_file).find("fibonacci"));
    if (use_sea) {
      LOG(INFO) << "Using SEA IR to compile..." << std::endl;
      return SeaIrCompileMethod(compiler,
                                code_item,
                                access_flags,
                                invoke_type,
                                class_def_idx,
                                method_idx,
                                class_loader,
                                dex_file);
  }
#endif
  return nullptr;
}


class QuickBackend : public CompilerBackend {
 public:
  QuickBackend() : CompilerBackend(100) {}

  void Init(CompilerDriver& driver) const {
    ArtInitQuickCompilerContext(driver);
  }

  void UnInit(CompilerDriver& driver) const {
    ArtUnInitQuickCompilerContext(driver);
  }

  CompiledMethod* Compile(CompilerDriver& compiler,
                          const DexFile::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          jobject class_loader,
                          const DexFile& dex_file) const {
    CompiledMethod* method = TryCompileWithSeaIR(compiler,
                                                 code_item,
                                                 access_flags,
                                                 invoke_type,
                                                 class_def_idx,
                                                 method_idx,
                                                 class_loader,
                                                 dex_file);
    if (method != nullptr) return method;

    return ArtQuickCompileMethod(compiler,
                                 code_item,
                                 access_flags,
                                 invoke_type,
                                 class_def_idx,
                                 method_idx,
                                 class_loader,
                                 dex_file);
  }

  CompiledMethod* JniCompile(CompilerDriver& driver,
                             uint32_t access_flags,
                             uint32_t method_idx,
                             const DexFile& dex_file) const {
    return ArtQuickJniCompileMethod(driver, access_flags, method_idx, dex_file);
  }

  uintptr_t GetEntryPointOf(mirror::ArtMethod* method) const {
    return reinterpret_cast<uintptr_t>(method->GetEntryPointFromQuickCompiledCode());
  }

  bool WriteElf(art::File* file,
                OatWriter& oat_writer,
                const std::vector<const art::DexFile*>& dex_files,
                const std::string& android_root,
                bool is_host, const CompilerDriver& driver) const
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return art::ElfWriterQuick::Create(file, oat_writer, dex_files, android_root, is_host, driver);
  }

  Backend* GetCodeGenerator(CompilationUnit* cu, void* compilation_unit) const {
    Mir2Lir* mir_to_lir = nullptr;
    switch (cu->instruction_set) {
      case kThumb2:
        mir_to_lir = ArmCodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
        break;
      case kMips:
        mir_to_lir = MipsCodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
        break;
      case kX86:
        mir_to_lir = X86CodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
        break;
      default:
        LOG(FATAL) << "Unexpected instruction set: " << cu->instruction_set;
    }

    /* The number of compiler temporaries depends on backend so set it up now if possible */
    if (mir_to_lir) {
      size_t max_temps = mir_to_lir->GetMaxPossibleCompilerTemps();
      bool set_max = cu->mir_graph->SetMaxAvailableNonSpecialCompilerTemps(max_temps);
      CHECK(set_max);
    }
    return mir_to_lir;;
  }

  void InitCompilationUnit(CompilationUnit& cu) const {}

 private:
  DISALLOW_COPY_AND_ASSIGN(QuickBackend);
};

#ifdef ART_USE_PORTABLE_COMPILER

extern "C" void ArtInitCompilerContext(art::CompilerDriver& driver);
extern "C" void ArtUnInitCompilerContext(art::CompilerDriver& driver);
extern "C" art::CompiledMethod* ArtCompileMethod(art::CompilerDriver& driver,
                                                 const art::DexFile::CodeItem* code_item,
                                                 uint32_t access_flags,
                                                 art::InvokeType invoke_type,
                                                 uint16_t class_def_idx,
                                                 uint32_t method_idx,
                                                 jobject class_loader,
                                                 const art::DexFile& dex_file);
extern "C" art::CompiledMethod* ArtLLVMJniCompileMethod(art::CompilerDriver& driver,
                                                        uint32_t access_flags, uint32_t method_idx,
                                                        const art::DexFile& dex_file);


class LLVMBackend : public CompilerBackend {
 public:
  LLVMBackend() : CompilerBackend(1000) {}

  void Init(CompilerDriver& driver) const {
    ArtInitCompilerContext(driver);
  }

  void UnInit(CompilerDriver& driver) const {
    ArtUnInitCompilerContext(driver);
  }

  CompiledMethod* Compile(CompilerDriver& compiler,
                          const DexFile::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          jobject class_loader,
                          const DexFile& dex_file) const {
    CompiledMethod* method = TryCompileWithSeaIR(compiler,
                                                 code_item,
                                                 access_flags,
                                                 invoke_type,
                                                 class_def_idx,
                                                 method_idx,
                                                 class_loader,
                                                 dex_file);
    if (method != nullptr) return method;

    return ArtCompileMethod(compiler,
                            code_item,
                            access_flags,
                            invoke_type,
                            class_def_idx,
                            method_idx,
                            class_loader,
                            dex_file);
  }

  CompiledMethod* JniCompile(CompilerDriver& driver,
                             uint32_t access_flags,
                             uint32_t method_idx,
                             const DexFile& dex_file) const {
    return ArtLLVMJniCompileMethod(driver, access_flags, method_idx, dex_file);
  }

  uintptr_t GetEntryPointOf(mirror::ArtMethod* method) const {
    return reinterpret_cast<uintptr_t>(method->GetEntryPointFromPortableCompiledCode());
  }

  bool WriteElf(art::File* file,
                OatWriter& oat_writer,
                const std::vector<const art::DexFile*>& dex_files,
                const std::string& android_root,
                bool is_host, const CompilerDriver& driver) const
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return art::ElfWriterMclinker::Create(
        file, oat_writer, dex_files, android_root, is_host, driver);
  }

  Backend* GetCodeGenerator(CompilationUnit* cu, void* compilation_unit) const {
    return PortableCodeGenerator(
        cu, cu->mir_graph.get(), &cu->arena,
        reinterpret_cast<art::llvm::LlvmCompilationUnit*>(compilation_unit));
  }

  void InitCompilationUnit(CompilationUnit& cu) const {
      // Fused long branches not currently useful in bitcode.
    cu.disable_opt |=
        (1 << kBranchFusing) |
        (1 << kSuppressExceptionEdges);
  }

  bool isPortable() const { return true; }

 private:
  DISALLOW_COPY_AND_ASSIGN(LLVMBackend);
};
#endif

CompilerBackend* CompilerBackend::Create(CompilerBackend::Kind kind) {
  switch (kind) {
    case kQuick:
      return new QuickBackend();
      break;
    case kPortable:
#ifdef ART_USE_PORTABLE_COMPILER
      return new LLVMBackend();
#else
      LOG(FATAL) << "Portable compiler not compiled";
#endif
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
  return nullptr;
}

}  // namespace art
