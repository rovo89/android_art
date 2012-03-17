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

#include "elf_loader.h"

#include "compiled_method.h"
#include "logging.h"
#include "object.h"
#include "runtime_support_llvm.h"
#include "utils_llvm.h"

#include <android/librsloader.h>

namespace art {
namespace compiler_llvm {


ElfLoader::~ElfLoader() {
  // Release every ELF object
  for (size_t i = 0; i < executables_.size(); ++i) {
    rsloaderDisposeExec(executables_[i]);
  }
}


bool ElfLoader::LoadElfAt(size_t elf_idx, const byte* image, size_t size) {
  if (elf_idx < executables_.size() && executables_[elf_idx] != NULL) {
    return false;
  }

  if (elf_idx >= executables_.size()) {
    executables_.resize(elf_idx + 1);
  }

  RSExecRef executable =
    rsloaderCreateExec(reinterpret_cast<const unsigned char*>(image),
                       size, art_find_runtime_support_func, NULL);

  if (executable == NULL) {
    LOG(WARNING) << "Failed to load ELF image: " << image << " size: " << size;
    return false;
  }

  executables_[elf_idx] = executable;
  return true;
}


const void* ElfLoader::GetMethodCodeAddr(size_t elf_idx,
                                         const Method* method) const {
  CHECK_LT(elf_idx, executables_.size());
  CHECK(method != NULL);
  return GetAddr(elf_idx, LLVMLongName(method).c_str());
}


const Method::InvokeStub* ElfLoader::
GetMethodInvokeStubAddr(size_t elf_idx, const Method* method) const {
  CHECK_LT(elf_idx, executables_.size());
  CHECK(method != NULL);
  return reinterpret_cast<const Method::InvokeStub*>(
      GetAddr(elf_idx, LLVMStubName(method).c_str()));
}


const void* ElfLoader::GetAddr(size_t elf_idx, const char* sym_name) const {
  CHECK_LT(elf_idx, executables_.size());
  CHECK(executables_[elf_idx] != NULL);
  return rsloaderGetSymbolAddress(executables_[elf_idx], sym_name);
}


} // namespace compiler_llvm
} // namespace art
