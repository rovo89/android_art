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

#include "procedure_linkage_table.h"

#include "base/logging.h"
#include "compiler_runtime_func_list.h"
#include "globals.h"
#include "instruction_set.h"
#include "runtime_support_func_list.h"
#include "runtime_support_llvm.h"
#include "utils_llvm.h"

#include <algorithm>

#include <UniquePtr.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>


namespace {
  const char* const art_runtime_func_name_list[] = {
#define DEFINE_ENTRY(ID, NAME) #NAME,
    RUNTIME_SUPPORT_FUNC_LIST(DEFINE_ENTRY)
#undef DEFINE_ENTRY
  };

  const char* const compiler_runtime_func_name_list_arm[] = {
#define DEFINE_ENTRY(NAME, RETURN_TYPE, ...) #NAME,
    COMPILER_RUNTIME_FUNC_LIST_ARM(DEFINE_ENTRY)
#undef DEFINE_ENTRY
  };

  const char* const compiler_runtime_func_name_list_mips[] = {
#define DEFINE_ENTRY(NAME, RETURN_TYPE, ...) #NAME,
    COMPILER_RUNTIME_FUNC_LIST_MIPS(DEFINE_ENTRY)
#undef DEFINE_ENTRY
  };

  const char* const compiler_runtime_func_name_list_x86[] = {
#define DEFINE_ENTRY(NAME, RETURN_TYPE, ...) #NAME,
    COMPILER_RUNTIME_FUNC_LIST_X86(DEFINE_ENTRY)
#undef DEFINE_ENTRY
  };

  const size_t art_runtime_func_count =
    sizeof(art_runtime_func_name_list) / sizeof(const char*);

  const size_t compiler_runtime_func_count_arm =
    sizeof(compiler_runtime_func_name_list_arm) / sizeof(const char*);

  const size_t compiler_runtime_func_count_mips =
    sizeof(compiler_runtime_func_name_list_mips) / sizeof(const char*);

  const size_t compiler_runtime_func_count_x86 =
    sizeof(compiler_runtime_func_name_list_x86) / sizeof(const char*);
}


namespace art {
namespace compiler_llvm {


ProcedureLinkageTable::ProcedureLinkageTable(InstructionSet insn_set)
    : insn_set_(insn_set) {
}


ProcedureLinkageTable::~ProcedureLinkageTable() {
}


bool ProcedureLinkageTable::AllocateTable() {
  if (table_mmap_.get()) {
    return true;
  }

  // Allocate the PLT
  byte* suggested_table_addr = reinterpret_cast<byte*>(kTableAddress);

  UniquePtr<MemMap> table_mmap(
      MemMap::MapAnonymous(".plt", suggested_table_addr,
                           GetTableSizeInBytes(), PROT_READ | PROT_WRITE));

  if (!table_mmap.get()) {
    return false;
  }

  if (table_mmap->Begin() != suggested_table_addr) {
    // Our PLT should be allocated at the FIXED address
    return false;
  }

  // Create the stubs in the PLT
  byte* stub_ptr = table_mmap->Begin();
  size_t stub_size = GetStubSizeInBytes();

  for (size_t i = 0; i < art_runtime_func_count; ++i, stub_ptr += stub_size) {
    const char* name = art_runtime_func_name_list[i];
    void* func = art_portable_find_runtime_support_func(NULL, name);
    DCHECK(func != NULL);
    CreateStub(stub_ptr, func);
  }

  const char* const* crt_name_list = NULL;
  size_t crt_count = 0u;

  switch (insn_set_) {
  case kArm:
  case kThumb2:
    crt_name_list = compiler_runtime_func_name_list_arm;
    crt_count = compiler_runtime_func_count_arm;
    break;

  case kMips:
    crt_name_list = compiler_runtime_func_name_list_mips;
    crt_count = compiler_runtime_func_count_mips;
    break;

  case kX86:
    crt_name_list = compiler_runtime_func_name_list_x86;
    crt_count = compiler_runtime_func_count_x86;
    break;

  default:
    LOG(FATAL) << "Unknown instruction set: " << insn_set_;
    return false;
  }

  for (size_t i = 0; i < crt_count; ++i, stub_ptr += stub_size) {
    void* func = art_portable_find_runtime_support_func(NULL, crt_name_list[i]);
    DCHECK(func != NULL);
    CreateStub(stub_ptr, func);
  }

  // Protect the procedure linkage table
  table_mmap->Protect(PROT_READ | PROT_EXEC);

  // Flush the instruction cache on specific architecture
#if defined(__arm__) || defined(__mips__)
  cacheflush(reinterpret_cast<long int>(table_mmap->Begin()),
             reinterpret_cast<long int>(table_mmap->End()), 0);
#endif

  // Transfer the ownership
  table_mmap_.reset(table_mmap.release());

  return true;
}


uintptr_t ProcedureLinkageTable::GetEntryAddress(const char* name) const {
  int func_idx = IndexOfRuntimeFunc(name);
  if (func_idx == -1) {
    return 0u;
  }

  return (kTableAddress + func_idx * GetStubSizeInBytes());
}



int ProcedureLinkageTable::IndexOfRuntimeFunc(const char* name) const {
  int result = IndexOfCompilerRuntimeFunc(name);
  if (result != -1) {
    return art_runtime_func_count + result;
  }

  return IndexOfArtRuntimeFunc(name);
}


int ProcedureLinkageTable::IndexOfArtRuntimeFunc(const char* name) {
  for (size_t i = 0; i < art_runtime_func_count; ++i) {
    if (strcmp(name, art_runtime_func_name_list[i]) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int ProcedureLinkageTable::IndexOfCompilerRuntimeFunc(InstructionSet insn_set,
                                                      const char* name) {
  const char* const* rt_begin = NULL;
  const char* const* rt_end = NULL;

  switch (insn_set) {
  case kArm:
  case kThumb2:
    rt_begin = compiler_runtime_func_name_list_arm;
    rt_end = compiler_runtime_func_name_list_arm +
             compiler_runtime_func_count_arm;
    break;

  case kMips:
    rt_begin = compiler_runtime_func_name_list_mips;
    rt_end = compiler_runtime_func_name_list_mips +
             compiler_runtime_func_count_mips;
    break;

  case kX86:
    rt_begin = compiler_runtime_func_name_list_x86;
    rt_end = compiler_runtime_func_name_list_x86 +
             compiler_runtime_func_count_x86;
    break;

  default:
    LOG(FATAL) << "Unknown instruction set: " << insn_set;
    return -1;
  }

  const char* const* name_lbound_ptr =
      std::lower_bound(rt_begin, rt_end, name, CStringLessThanComparator());

  if (name_lbound_ptr < rt_end && strcmp(*name_lbound_ptr, name) == 0) {
    return (name_lbound_ptr - rt_begin);
  } else {
    return -1;
  }
}


size_t ProcedureLinkageTable::GetStubCount(InstructionSet insn_set) {
  switch (insn_set) {
  case kArm:
  case kThumb2:
    return art_runtime_func_count + compiler_runtime_func_count_arm;

  case kMips:
    return art_runtime_func_count + compiler_runtime_func_count_mips;

  case kX86:
    return art_runtime_func_count + compiler_runtime_func_count_x86;

  default:
    LOG(FATAL) << "Unknown instruction set: " << insn_set;
    return 0u;
  }
}


size_t ProcedureLinkageTable::GetStubSizeInBytes(InstructionSet insn_set) {
  switch (insn_set) {
  case kArm:
  case kThumb2:
    return 8u;

  case kMips:
    return 16u;

  case kX86:
    return 8u;

  default:
    LOG(FATAL) << "Unknown instruction set: " << insn_set;
    return 0u;
  }
}


void ProcedureLinkageTable::CreateStub(InstructionSet insn_set,
                                       byte* stub, void* dest_) {
  switch (insn_set) {
  case kArm:
  case kThumb2:
    {
      uint32_t dest = static_cast<uint32_t>(
                      reinterpret_cast<uintptr_t>(dest_) & 0xfffffffful);
      uint32_t* stub_w = reinterpret_cast<uint32_t*>(stub);

      stub_w[0] = 0xe51ff004ul; // ldr pc, [pc #-4]
      stub_w[1] = dest;
    }
    break;

  case kMips:
    {
      uint32_t dest = static_cast<uint32_t>(
                      reinterpret_cast<uintptr_t>(dest_) & 0xfffffffful);
      uint32_t* stub_w = reinterpret_cast<uint32_t*>(stub);

      stub_w[0] = 0x3c190000ul | ((dest >> 16) & 0xfffful); // lui
      stub_w[1] = 0x37390000ul | (dest & 0xfffful);; // ori
      stub_w[2] = 0x03200008ul; // jr (jump register)
      stub_w[3] = 0x00000000ul; // nop
    }
    break;

  case kX86:
    {
      uint32_t off = static_cast<uint32_t>(
                     reinterpret_cast<uintptr_t>(dest_) -
                     reinterpret_cast<uintptr_t>(stub + 1) - 4);
      // jmp (32-bit offset)
      stub[0] = 0xe9u;
      stub[1] = off & 0xffu;
      stub[2] = (off >> 8) & 0xffu;
      stub[2] = (off >> 16) & 0xffu;
      stub[2] = (off >> 24) & 0xffu;
    }
    break;

  default:
    LOG(FATAL) << "Unknown instruction set: " << insn_set;
  }
}


} // namespace compiler_llvm
} // namespace art
