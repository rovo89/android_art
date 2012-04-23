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

#include "../mutex.h"
#include "elf_image.h"
#include "globals.h"
#include "instruction_set.h"
#include "logging.h"
#include "runtime_support_builder.h"
#include "runtime_support_func.h"
#include "safe_map.h"

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
    cunit_lock_.AssertHeld();
    return insn_set_;
  }

  llvm::LLVMContext* GetLLVMContext() const {
    cunit_lock_.AssertHeld();
    return context_.get();
  }

  llvm::Module* GetModule() const {
    cunit_lock_.AssertHeld();
    return module_;
  }

  IRBuilder* GetIRBuilder() const {
    cunit_lock_.AssertHeld();
    return irb_.get();
  }

  ElfImage GetElfImage() const {
    MutexLock GUARD(cunit_lock_);
    CHECK_GT(elf_image_.size(), 0u);
    return ElfImage(elf_image_);
  }

  uint16_t AcquireUniqueElfFuncIndex() {
    cunit_lock_.AssertHeld();
    CHECK(num_elf_funcs_ < UINT16_MAX);
    return num_elf_funcs_++;
  }

  bool WriteBitcodeToFile(const std::string& bitcode_filename);

  bool Materialize();

  bool IsMaterialized() const {
    MutexLock GUARD(cunit_lock_);
    return (context_.get() == NULL);
  }

  bool IsMaterializeThresholdReached() const {
    MutexLock GUARD(cunit_lock_);
    return (mem_usage_ > 100000000u); // (threshold: 100 MB)
  }

  void AddMemUsageApproximation(size_t usage) {
    MutexLock GUARD(cunit_lock_);
    mem_usage_ += usage;
  }

  void RegisterCompiledMethod(const llvm::Function* func, CompiledMethod* cm);

  void UpdateFrameSizeInBytes(const llvm::Function* func, size_t frame_size_in_bytes);

  mutable Mutex cunit_lock_;

 private:
  InstructionSet insn_set_;
  const size_t elf_idx_;

  UniquePtr<llvm::LLVMContext> context_;
  UniquePtr<IRBuilder> irb_;
  UniquePtr<RuntimeSupportBuilder> runtime_support_;
  llvm::Module* module_;

  std::vector<uint8_t> elf_image_;

  SafeMap<const llvm::Function*, CompiledMethod*> compiled_methods_map_;

  size_t mem_usage_;
  uint16_t num_elf_funcs_;

  bool MaterializeToFile(int output_fd, InstructionSet insn_set);
};

} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_COMPILATION_UNIT_H_
