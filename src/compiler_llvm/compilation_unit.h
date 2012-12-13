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

#ifndef ART_SRC_COMPILER_LLVM_COMPILATION_UNIT_H_
#define ART_SRC_COMPILER_LLVM_COMPILATION_UNIT_H_

#include "base/mutex.h"
#include "globals.h"
#if defined(ART_USE_DEXLANG_FRONTEND)
# include "greenland/dex_lang.h"
#endif
#include "instruction_set.h"
#include "logging.h"
#include "runtime_support_builder.h"
#include "runtime_support_func.h"
#include "safe_map.h"

#if defined(ART_USE_PORTABLE_COMPILER)
# include "compiler/Dalvik.h"
# include "compiler.h"
# include "oat_compilation_unit.h"
#endif

#include <UniquePtr.h>
#include <string>
#include <vector>

namespace art {
  class CompiledMethod;
}

namespace llvm {
  class Function;
  class LLVMContext;
  class Module;
  class raw_ostream;
}

namespace art {
namespace compiler_llvm {

class CompilerLLVM;
class IRBuilder;

class CompilationUnit {
 public:
  CompilationUnit(const CompilerLLVM* compiler_llvm,
                  size_t cunit_idx);

  ~CompilationUnit();

  size_t GetIndex() const {
    return cunit_idx_;
  }

  InstructionSet GetInstructionSet() const;

  llvm::LLVMContext* GetLLVMContext() const {
    return context_.get();
  }

  llvm::Module* GetModule() const {
    return module_;
  }

  IRBuilder* GetIRBuilder() const {
    return irb_.get();
  }

#if defined(ART_USE_DEXLANG_FRONTEND)
  greenland::DexLang::Context* GetDexLangContext() const {
    return dex_lang_ctx_;
  }
#endif

  void SetBitcodeFileName(const std::string& bitcode_filename) {
    bitcode_filename_ = bitcode_filename;
  }

#if defined(ART_USE_PORTABLE_COMPILER)
  LLVMInfo* GetQuickContext() const {
    return llvm_info_.get();
  }
  void SetCompiler(Compiler* compiler) {
    compiler_ = compiler;
  }
  void SetOatCompilationUnit(OatCompilationUnit* oat_compilation_unit) {
    oat_compilation_unit_ = oat_compilation_unit;
  }
#endif

  bool Materialize();

  bool IsMaterialized() const {
    return !compiled_code_.empty();
  }

  const std::vector<uint8_t>& GetCompiledCode() const {
    DCHECK(IsMaterialized());
    return compiled_code_;
  }

 private:
  const CompilerLLVM* compiler_llvm_;
  const size_t cunit_idx_;

  UniquePtr<llvm::LLVMContext> context_;
  UniquePtr<IRBuilder> irb_;
  UniquePtr<RuntimeSupportBuilder> runtime_support_;
  llvm::Module* module_; // Managed by context_
#if defined(ART_USE_DEXLANG_FRONTEND)
  greenland::DexLang::Context* dex_lang_ctx_;
#endif
#if defined(ART_USE_PORTABLE_COMPILER)
  UniquePtr<LLVMInfo> llvm_info_;
  Compiler* compiler_;
  OatCompilationUnit* oat_compilation_unit_;
#endif

  std::string bitcode_filename_;

  std::vector<uint8_t> compiled_code_;

  SafeMap<const llvm::Function*, CompiledMethod*> compiled_methods_map_;

  void CheckCodeAlign(uint32_t offset) const;

  bool MaterializeToString(std::string& str_buffer);
  bool MaterializeToRawOStream(llvm::raw_ostream& out_stream);

  bool ExtractCodeAndPrelink(const std::string& elf_image);
};

} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_COMPILATION_UNIT_H_
