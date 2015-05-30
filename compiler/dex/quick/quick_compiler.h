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

#ifndef ART_COMPILER_DEX_QUICK_QUICK_COMPILER_H_
#define ART_COMPILER_DEX_QUICK_QUICK_COMPILER_H_

#include "compiler.h"

namespace art {

class Compiler;
class CompilerDriver;
class Mir2Lir;
class PassManager;

class QuickCompiler : public Compiler {
 public:
  virtual ~QuickCompiler();

  void Init() OVERRIDE;

  void UnInit() const OVERRIDE;

  bool CanCompileMethod(uint32_t method_idx, const DexFile& dex_file, CompilationUnit* cu) const
      OVERRIDE;

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

  uintptr_t GetEntryPointOf(ArtMethod* method) const OVERRIDE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static Mir2Lir* GetCodeGenerator(CompilationUnit* cu, void* compilation_unit);

  void InitCompilationUnit(CompilationUnit& cu) const OVERRIDE;

  static Compiler* Create(CompilerDriver* driver);

  const PassManager* GetPreOptPassManager() const {
    return pre_opt_pass_manager_.get();
  }
  const PassManager* GetPostOptPassManager() const {
    return post_opt_pass_manager_.get();
  }

 protected:
  explicit QuickCompiler(CompilerDriver* driver);

 private:
  std::unique_ptr<PassManager> pre_opt_pass_manager_;
  std::unique_ptr<PassManager> post_opt_pass_manager_;
  DISALLOW_COPY_AND_ASSIGN(QuickCompiler);
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_QUICK_COMPILER_H_
