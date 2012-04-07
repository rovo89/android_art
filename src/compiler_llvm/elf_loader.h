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

#ifndef ART_SRC_COMPILER_LLVM_ELF_LOADER_H_
#define ART_SRC_COMPILER_LLVM_ELF_LOADER_H_

#include "elf_image.h"
#include "globals.h"
#include "oat_file.h"
#include "object.h"

#include <android/librsloader.h>
#include <vector>

namespace art {
namespace compiler_llvm {

class ElfLoader {
 public:
  ~ElfLoader();

  bool LoadElfAt(size_t elf_idx, const ElfImage& elf_image,
                 OatFile::RelocationBehavior reloc);

  void RelocateExecutable();

  const void* GetMethodCodeAddr(uint16_t elf_idx,
                                uint16_t elf_func_idx) const;

  const Method::InvokeStub* GetMethodInvokeStubAddr(uint16_t elf_idx,
                                                    uint16_t elf_func_idx) const;

  size_t GetCodeSize(uint16_t elf_idx, uint16_t elf_func_idx) const;

 private:
  const void* GetAddr(size_t elf_idx, const char* sym_name) const;

  std::vector<RSExecRef> executables_;
};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_ELF_LOADER_H_
