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

#include <fstream>
#include <stdint.h>

#include "builder.h"
#include "code_generator.h"
#include "compilers.h"
#include "driver/compiler_driver.h"
#include "driver/dex_compilation_unit.h"
#include "graph_visualizer.h"
#include "nodes.h"
#include "register_allocator.h"
#include "ssa_phi_elimination.h"
#include "ssa_liveness_analysis.h"
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
    memory_.resize(size);
    return &memory_[0];
  }

  size_t GetSize() const { return size_; }
  const std::vector<uint8_t>& GetMemory() const { return memory_; }

 private:
  std::vector<uint8_t> memory_;
  size_t size_;

  DISALLOW_COPY_AND_ASSIGN(CodeVectorAllocator);
};

/**
 * If set to true, generates a file suitable for the c1visualizer tool and IRHydra.
 */
static bool kIsVisualizerEnabled = false;

/**
 * Filter to apply to the visualizer. Methods whose name contain that filter will
 * be in the file.
 */
static const char* kStringFilter = "";

OptimizingCompiler::OptimizingCompiler(CompilerDriver* driver) : QuickCompiler(driver) {
  if (kIsVisualizerEnabled) {
    visualizer_output_.reset(new std::ofstream("art.cfg"));
  }
}

CompiledMethod* OptimizingCompiler::TryCompile(const DexFile::CodeItem* code_item,
                                               uint32_t access_flags,
                                               InvokeType invoke_type,
                                               uint16_t class_def_idx,
                                               uint32_t method_idx,
                                               jobject class_loader,
                                               const DexFile& dex_file) const {
  InstructionSet instruction_set = GetCompilerDriver()->GetInstructionSet();
  // Always use the thumb2 assembler: some runtime functionality (like implicit stack
  // overflow checks) assume thumb2.
  if (instruction_set == kArm) {
    instruction_set = kThumb2;
  }

  // Do not attempt to compile on architectures we do not support.
  if (instruction_set != kX86 && instruction_set != kX86_64 && instruction_set != kThumb2) {
    return nullptr;
  }

  DexCompilationUnit dex_compilation_unit(
    nullptr, class_loader, art::Runtime::Current()->GetClassLinker(), dex_file, code_item,
    class_def_idx, method_idx, access_flags,
    GetCompilerDriver()->GetVerifiedMethod(&dex_file, method_idx));

  // For testing purposes, we put a special marker on method names that should be compiled
  // with this compiler. This makes sure we're not regressing.
  bool shouldCompile = dex_compilation_unit.GetSymbol().find("00024opt_00024") != std::string::npos;
  bool shouldOptimize =
      dex_compilation_unit.GetSymbol().find("00024reg_00024") != std::string::npos;

  ArenaPool pool;
  ArenaAllocator arena(&pool);
  HGraphBuilder builder(&arena, &dex_compilation_unit, &dex_file, GetCompilerDriver());

  HGraph* graph = builder.BuildGraph(*code_item);
  if (graph == nullptr) {
    if (shouldCompile) {
      LOG(FATAL) << "Could not build graph in optimizing compiler";
    }
    return nullptr;
  }

  CodeGenerator* codegen = CodeGenerator::Create(&arena, graph, instruction_set);
  if (codegen == nullptr) {
    if (shouldCompile) {
      LOG(FATAL) << "Could not find code generator for optimizing compiler";
    }
    return nullptr;
  }

  HGraphVisualizer visualizer(
      visualizer_output_.get(), graph, kStringFilter, *codegen, dex_compilation_unit);
  visualizer.DumpGraph("builder");

  CodeVectorAllocator allocator;

  if (RegisterAllocator::CanAllocateRegistersFor(*graph, instruction_set)) {
    graph->BuildDominatorTree();
    graph->TransformToSSA();
    visualizer.DumpGraph("ssa");
    graph->FindNaturalLoops();

    SsaRedundantPhiElimination(graph).Run();
    SsaDeadPhiElimination(graph).Run();

    SsaLivenessAnalysis liveness(*graph, codegen);
    liveness.Analyze();
    visualizer.DumpGraph(kLivenessPassName);

    RegisterAllocator register_allocator(graph->GetArena(), codegen, liveness);
    register_allocator.AllocateRegisters();

    visualizer.DumpGraph(kRegisterAllocatorPassName);
    codegen->CompileOptimized(&allocator);
  } else if (shouldOptimize && RegisterAllocator::Supports(instruction_set)) {
    LOG(FATAL) << "Could not allocate registers in optimizing compiler";
  } else {
    codegen->CompileBaseline(&allocator);

    // Run these phases to get some test coverage.
    graph->BuildDominatorTree();
    graph->TransformToSSA();
    visualizer.DumpGraph("ssa");
    graph->FindNaturalLoops();
    SsaLivenessAnalysis liveness(*graph, codegen);
    liveness.Analyze();
    visualizer.DumpGraph(kLivenessPassName);
  }

  std::vector<uint8_t> mapping_table;
  codegen->BuildMappingTable(&mapping_table);
  std::vector<uint8_t> vmap_table;
  codegen->BuildVMapTable(&vmap_table);
  std::vector<uint8_t> gc_map;
  codegen->BuildNativeGCMap(&gc_map, dex_compilation_unit);

  return new CompiledMethod(GetCompilerDriver(),
                            instruction_set,
                            allocator.GetMemory(),
                            codegen->GetFrameSize(),
                            codegen->GetCoreSpillMask(),
                            0, /* FPR spill mask, unused */
                            mapping_table,
                            vmap_table,
                            gc_map,
                            nullptr);
}

}  // namespace art
