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

#include "globals.h"
#include "object.h"

#include <android/librsloader.h>
#include <vector>

namespace art {
  class Method;
}

namespace art {
namespace compiler_llvm {

class ElfLoader {
 public:
  ~ElfLoader();

  bool LoadElfAt(size_t elf_idx, const byte* addr, size_t size);

  const void* GetMethodCodeAddr(size_t elf_idx, const Method* method) const;

  const Method::InvokeStub* GetMethodInvokeStubAddr(size_t elf_idx,
                                                    const Method* method) const;

 private:
  const void* GetAddr(size_t elf_idx, const char* sym_name) const;

  std::vector<RSExecRef> executables_;
};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_ELF_LOADER_H_
