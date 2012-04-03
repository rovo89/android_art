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
#include "elf_image.h"
#include "logging.h"
#include "oat_file.h"
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


bool ElfLoader::LoadElfAt(size_t elf_idx,
                          const ElfImage& elf_image,
                          OatFile::RelocationBehavior reloc) {
  if (elf_idx < executables_.size() && executables_[elf_idx] != NULL) {
    return false;
  }

  if (elf_idx >= executables_.size()) {
    executables_.resize(elf_idx + 1);
  }

  RSExecRef executable = rsloaderLoadExecutable(elf_image.begin(),
                                                elf_image.size());

  if (executable == NULL) {
    LOG(WARNING) << "Failed to load ELF"
                 << " image: " << static_cast<const void*>(elf_image.begin())
                 << " size: " << elf_image.size();
    return false;
  }

  if (reloc == OatFile::kRelocAll) {
    if (!rsloaderRelocateExecutable(executable,
                                    art_find_runtime_support_func, NULL)) {
      LOG(ERROR) << "Failed to relocate the ELF image";
      rsloaderDisposeExec(executable);
      return false;
    }
  }

  executables_[elf_idx] = executable;
  return true;
}


void ElfLoader::RelocateExecutable() {
  for (size_t i = 0; i < executables_.size(); ++i) {
    if (executables_[i] != NULL &&
        !rsloaderRelocateExecutable(executables_[i],
                                    art_find_runtime_support_func, NULL)) {
      LOG(FATAL) << "Failed to relocate ELF image " << i;
    }
  }
}


const void* ElfLoader::GetMethodCodeAddr(uint16_t elf_idx,
                                         uint16_t elf_func_idx) const {
  CHECK_LT(elf_idx, executables_.size());
  return GetAddr(elf_idx, ElfFuncName(elf_func_idx).c_str());
}


const Method::InvokeStub* ElfLoader::
GetMethodInvokeStubAddr(uint16_t elf_idx, uint16_t elf_func_idx) const {
  CHECK_LT(elf_idx, executables_.size());
  return reinterpret_cast<const Method::InvokeStub*>(
      GetAddr(elf_idx, ElfFuncName(elf_func_idx).c_str()));
}


size_t ElfLoader::GetCodeSize(uint16_t elf_idx, uint16_t elf_func_idx) const {
  CHECK_LT(elf_idx, executables_.size());
  CHECK(executables_[elf_idx] != NULL);
  return rsloaderGetSymbolSize(executables_[elf_idx],
                               ElfFuncName(elf_func_idx).c_str());
}


const void* ElfLoader::GetAddr(size_t elf_idx, const char* sym_name) const {
  CHECK_LT(elf_idx, executables_.size());
  CHECK(executables_[elf_idx] != NULL);
  return rsloaderGetSymbolAddress(executables_[elf_idx], sym_name);
}


} // namespace compiler_llvm
} // namespace art
