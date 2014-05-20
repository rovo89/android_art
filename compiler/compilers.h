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

#ifndef ART_COMPILER_COMPILERS_H_
#define ART_COMPILER_COMPILERS_H_

#include "compiler.h"

namespace art {

class QuickCompiler : public Compiler {
 public:
  explicit QuickCompiler(CompilerDriver* driver) : Compiler(driver, 100) {}

  void Init() const OVERRIDE;

  void UnInit() const OVERRIDE;

  CompiledMethod* Compile(const DexFile::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          jobject class_loader,
                          const DexFile& dex_file) const OVERRIDE;

  CompiledMethod* JniCompile(uint32_t access_flags,
                             uint32_t method_idx,
                             const DexFile& dex_file) const OVERRIDE;

  uintptr_t GetEntryPointOf(mirror::ArtMethod* method) const OVERRIDE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool WriteElf(art::File* file,
                OatWriter* oat_writer,
                const std::vector<const art::DexFile*>& dex_files,
                const std::string& android_root,
                bool is_host) const
    OVERRIDE
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Backend* GetCodeGenerator(CompilationUnit* cu, void* compilation_unit) const OVERRIDE;

  void InitCompilationUnit(CompilationUnit& cu) const OVERRIDE {}

  /*
   * @brief Generate and return Dwarf CFI initialization, if supported by the
   * backend.
   * @param driver CompilerDriver for this compile.
   * @returns nullptr if not supported by backend or a vector of bytes for CFI DWARF
   * information.
   * @note This is used for backtrace information in generated code.
   */
  std::vector<uint8_t>* GetCallFrameInformationInitialization(const CompilerDriver& driver) const
      OVERRIDE;

 private:
  DISALLOW_COPY_AND_ASSIGN(QuickCompiler);
};

class OptimizingCompiler FINAL : public QuickCompiler {
 public:
  explicit OptimizingCompiler(CompilerDriver* driver);

  CompiledMethod* Compile(const DexFile::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          jobject class_loader,
                          const DexFile& dex_file) const OVERRIDE;

  CompiledMethod* TryCompile(const DexFile::CodeItem* code_item,
                             uint32_t access_flags,
                             InvokeType invoke_type,
                             uint16_t class_def_idx,
                             uint32_t method_idx,
                             jobject class_loader,
                             const DexFile& dex_file) const;

 private:
  std::unique_ptr<std::ostream> visualizer_output_;

  DISALLOW_COPY_AND_ASSIGN(OptimizingCompiler);
};

}  // namespace art

#endif  // ART_COMPILER_COMPILERS_H_
