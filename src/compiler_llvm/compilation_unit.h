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

#include "elf_image.h"
#include "globals.h"
#include "instruction_set.h"
#include "logging.h"

#include <UniquePtr.h>
#include <string>

namespace llvm {
  class LLVMContext;
  class Module;
}

namespace art {
namespace compiler_llvm {

class IRBuilder;

class CompilationUnit {
 public:
  CompilationUnit(InstructionSet insn_set, size_t elf_idx);

  ~CompilationUnit();

  size_t GetElfIndex() const {
    return elf_idx_;
  }

  InstructionSet GetInstructionSet() const {
    return insn_set_;
  }

  llvm::LLVMContext* GetLLVMContext() const {
    return context_.get();
  }

  llvm::Module* GetModule() const {
    return module_;
  }

  IRBuilder* GetIRBuilder() const {
    return irb_.get();
  }

  std::string const& GetBitcodeFileName() const {
    return bitcode_filename_;
  }

  void SetBitcodeFileName(std::string const& filename) {
    bitcode_filename_ = filename;
  }

  ElfImage GetElfImage() const {
    return ElfImage(elf_image_);
  }

  uint16_t AcquireUniqueElfFuncIndex() {
    CHECK(num_elf_funcs_ < UINT16_MAX);
    return num_elf_funcs_++;
  }

  bool WriteBitcodeToFile();

  bool Materialize();

  bool IsMaterialized() const {
    return (context_.get() == NULL);
  }

  bool IsMaterializeThresholdReached() const {
    return (mem_usage_ > 300000000u); // (threshold: 300 MB)
  }

  void AddMemUsageApproximation(size_t usage) {
    mem_usage_ += usage;
  }

 private:
  InstructionSet insn_set_;
  const size_t elf_idx_;

  UniquePtr<llvm::LLVMContext> context_;
  UniquePtr<IRBuilder> irb_;
  llvm::Module* module_;

  std::string elf_image_;
  std::string bitcode_filename_;

  size_t mem_usage_;
  uint16_t num_elf_funcs_;
};

} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_COMPILATION_UNIT_H_
