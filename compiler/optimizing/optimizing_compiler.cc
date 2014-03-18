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

#include <stdint.h>

#include "builder.h"
#include "code_generator.h"
#include "compilers.h"
#include "driver/compiler_driver.h"
#include "nodes.h"
#include "utils/arena_allocator.h"

namespace art {

/**
 * Used by the code generator, to allocate the code in a vector.
 */
class CodeVectorAllocator FINAL : public CodeAllocator {
 public:
  CodeVectorAllocator() { }

  virtual uint8_t* Allocate(size_t size) {
    size_ = size;
    memory_.reserve(size);
    return &memory_[0];
  }

  size_t GetSize() const { return size_; }
  std::vector<uint8_t>* GetMemory() { return &memory_; }

 private:
  std::vector<uint8_t> memory_;
  size_t size_;

  DISALLOW_COPY_AND_ASSIGN(CodeVectorAllocator);
};


CompiledMethod* OptimizingCompiler::TryCompile(CompilerDriver& driver,
                                               const DexFile::CodeItem* code_item,
                                               uint32_t access_flags,
                                               InvokeType invoke_type,
                                               uint16_t class_def_idx,
                                               uint32_t method_idx,
                                               jobject class_loader,
                                               const DexFile& dex_file) const {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  HGraphBuilder builder(&arena);
  HGraph* graph = builder.BuildGraph(*code_item);
  if (graph == nullptr) {
    return nullptr;
  }

  InstructionSet instruction_set = driver.GetInstructionSet();
  CodeGenerator* codegen = CodeGenerator::Create(&arena, graph, instruction_set);
  if (codegen == nullptr) {
    return nullptr;
  }

  CodeVectorAllocator allocator;
  codegen->Compile(&allocator);

  std::vector<uint8_t> mapping_table;
  codegen->BuildMappingTable(&mapping_table);
  std::vector<uint8_t> vmap_table;
  codegen->BuildVMapTable(&vmap_table);
  std::vector<uint8_t> gc_map;
  codegen->BuildNativeGCMap(&gc_map);

  return new CompiledMethod(driver,
                            instruction_set,
                            *allocator.GetMemory(),
                            codegen->GetFrameSize(),
                            0, /* GPR spill mask, unused */
                            0, /* FPR spill mask, unused */
                            mapping_table,
                            vmap_table,
                            gc_map,
                            nullptr);
}

}  // namespace art
