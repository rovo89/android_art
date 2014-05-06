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

#include "compiler.h"
#include "compilers.h"
#include "driver/compiler_driver.h"
#include "mirror/art_method-inl.h"

#ifdef ART_USE_PORTABLE_COMPILER
#include "dex/portable/mir_to_gbc.h"
#include "elf_writer_mclinker.h"
#endif

namespace art {

#ifdef ART_SEA_IR_MODE
extern "C" art::CompiledMethod* SeaIrCompileMethod(const art::DexFile::CodeItem* code_item,
                                                   uint32_t access_flags,
                                                   art::InvokeType invoke_type,
                                                   uint16_t class_def_idx,
                                                   uint32_t method_idx,
                                                   jobject class_loader,
                                                   const art::DexFile& dex_file);
#endif


CompiledMethod* Compiler::TryCompileWithSeaIR(const art::DexFile::CodeItem* code_item,
                                              uint32_t access_flags,
                                              art::InvokeType invoke_type,
                                              uint16_t class_def_idx,
                                              uint32_t method_idx,
                                              jobject class_loader,
                                              const art::DexFile& dex_file) {
#ifdef ART_SEA_IR_MODE
    bool use_sea = (std::string::npos != PrettyMethod(method_idx, dex_file).find("fibonacci"));
    if (use_sea) {
      LOG(INFO) << "Using SEA IR to compile..." << std::endl;
      return SeaIrCompileMethod(code_item,
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


#ifdef ART_USE_PORTABLE_COMPILER

extern "C" void ArtInitCompilerContext(art::CompilerDriver* driver);

extern "C" void ArtUnInitCompilerContext(art::CompilerDriver* driver);

extern "C" art::CompiledMethod* ArtCompileMethod(art::CompilerDriver* driver,
                                                 const art::DexFile::CodeItem* code_item,
                                                 uint32_t access_flags,
                                                 art::InvokeType invoke_type,
                                                 uint16_t class_def_idx,
                                                 uint32_t method_idx,
                                                 jobject class_loader,
                                                 const art::DexFile& dex_file);

extern "C" art::CompiledMethod* ArtLLVMJniCompileMethod(art::CompilerDriver* driver,
                                                        uint32_t access_flags, uint32_t method_idx,
                                                        const art::DexFile& dex_file);

extern "C" void compilerLLVMSetBitcodeFileName(art::CompilerDriver* driver,
                                               std::string const& filename);


class LLVMCompiler FINAL : public Compiler {
 public:
  explicit LLVMCompiler(CompilerDriver* driver) : Compiler(driver, 1000) {}

  void Init() const OVERRIDE {
    ArtInitCompilerContext(GetCompilerDriver());
  }

  void UnInit() const OVERRIDE {
    ArtUnInitCompilerContext(GetCompilerDriver());
  }

  CompiledMethod* Compile(const DexFile::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          jobject class_loader,
                          const DexFile& dex_file) const OVERRIDE {
    CompiledMethod* method = TryCompileWithSeaIR(code_item,
                                                 access_flags,
                                                 invoke_type,
                                                 class_def_idx,
                                                 method_idx,
                                                 class_loader,
                                                 dex_file);
    if (method != nullptr) {
      return method;
    }

    return ArtCompileMethod(GetCompilerDriver(),
                            code_item,
                            access_flags,
                            invoke_type,
                            class_def_idx,
                            method_idx,
                            class_loader,
                            dex_file);
  }

  CompiledMethod* JniCompile(uint32_t access_flags,
                             uint32_t method_idx,
                             const DexFile& dex_file) const OVERRIDE {
    return ArtLLVMJniCompileMethod(GetCompilerDriver(), access_flags, method_idx, dex_file);
  }

  uintptr_t GetEntryPointOf(mirror::ArtMethod* method) const {
    return reinterpret_cast<uintptr_t>(method->GetEntryPointFromPortableCompiledCode());
  }

  bool WriteElf(art::File* file,
                OatWriter* oat_writer,
                const std::vector<const art::DexFile*>& dex_files,
                const std::string& android_root,
                bool is_host, const CompilerDriver& driver) const
      OVERRIDE
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

  bool IsPortable() const OVERRIDE {
    return true;
  }

  void SetBitcodeFileName(const CompilerDriver& driver, const std::string& filename) {
    typedef void (*SetBitcodeFileNameFn)(const CompilerDriver&, const std::string&);

    SetBitcodeFileNameFn set_bitcode_file_name =
      reinterpret_cast<SetBitcodeFileNameFn>(compilerLLVMSetBitcodeFileName);

    set_bitcode_file_name(driver, filename);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(LLVMCompiler);
};
#endif

Compiler* Compiler::Create(CompilerDriver* driver, Compiler::Kind kind) {
  switch (kind) {
    case kQuick:
      return new QuickCompiler(driver);
      break;
    case kOptimizing:
      return new OptimizingCompiler(driver);
      break;
    case kPortable:
#ifdef ART_USE_PORTABLE_COMPILER
      return new LLVMCompiler(driver);
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
