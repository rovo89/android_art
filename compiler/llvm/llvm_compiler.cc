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

#include "llvm_compiler.h"

#ifdef ART_USE_PORTABLE_COMPILER
#include "compiler.h"
#include "compiler_llvm.h"
#include "dex/portable/mir_to_gbc.h"
#include "dex_file.h"
#include "elf_writer_mclinker.h"
#include "mirror/art_method-inl.h"
#endif

namespace art {

#ifdef ART_USE_PORTABLE_COMPILER

namespace llvm {

// Thread-local storage compiler worker threads
class LLVMCompilerTls : public CompilerTls {
  public:
    LLVMCompilerTls() : llvm_info_(nullptr) {}
    ~LLVMCompilerTls() {}

    void* GetLLVMInfo() { return llvm_info_; }

    void SetLLVMInfo(void* llvm_info) { llvm_info_ = llvm_info; }

  private:
    void* llvm_info_;
};



class LLVMCompiler FINAL : public Compiler {
 public:
  explicit LLVMCompiler(CompilerDriver* driver) : Compiler(driver, 1000) {}

  CompilerTls* CreateNewCompilerTls() {
    return new LLVMCompilerTls();
  }

  void Init() const OVERRIDE {
    ArtInitCompilerContext(GetCompilerDriver());
  }

  void UnInit() const OVERRIDE {
    ArtUnInitCompilerContext(GetCompilerDriver());
  }

  bool CanCompileMethod(uint32_t method_idx, const DexFile& dex_file, CompilationUnit* cu) const
      OVERRIDE {
    return true;
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

}  // namespace llvm
#endif

Compiler* CreateLLVMCompiler(CompilerDriver* driver) {
#ifdef ART_USE_PORTABLE_COMPILER
      return new llvm::LLVMCompiler(driver);
#else
      return nullptr;
#endif
}

}  // namespace art
