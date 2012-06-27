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

#ifndef ART_SRC_COMPILER_LLVM_PROCEDURE_LINKAGE_TABLE_H_
#define ART_SRC_COMPILER_LLVM_PROCEDURE_LINKAGE_TABLE_H_

#include "globals.h"
#include "instruction_set.h"
#include "mem_map.h"

#include <UniquePtr.h>

#include <stddef.h>
#include <stdint.h>

namespace art {
namespace compiler_llvm {


class ProcedureLinkageTable {
 public:
  ProcedureLinkageTable(InstructionSet insn_set);

  ~ProcedureLinkageTable();

  bool AllocateTable();

  uintptr_t GetEntryAddress(const char* func_name) const;

 private:
  static size_t GetStubCount(InstructionSet insn_set);
  static size_t GetStubSizeInBytes(InstructionSet insn_set);
  static void CreateStub(InstructionSet insn_set,
                         byte* stub, void* branch_dest);

  int IndexOfRuntimeFunc(const char* name) const;
  static int IndexOfArtRuntimeFunc(const char* name);
  static int IndexOfCompilerRuntimeFunc(InstructionSet insn_set,
                                        const char* name);

  size_t GetStubCount() const {
    return GetStubCount(insn_set_);
  }

  size_t GetStubSizeInBytes() const {
    return GetStubSizeInBytes(insn_set_);
  }

  size_t GetTableSizeInBytes() const {
    return GetStubSizeInBytes() * GetStubCount();
  }

  void CreateStub(byte* stub, void* branch_dest) {
    return CreateStub(insn_set_, stub, branch_dest);
  }

  int IndexOfCompilerRuntimeFunc(const char* name) const {
    return IndexOfCompilerRuntimeFunc(insn_set_, name);
  }

  InstructionSet insn_set_;
  UniquePtr<MemMap> table_mmap_;

  static const size_t kTableSizeInBytes = 1024u;
  static const uintptr_t kTableAddress = 0x5fffc000u;
};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_PROCEDURE_LINKAGE_TABLE_H_
