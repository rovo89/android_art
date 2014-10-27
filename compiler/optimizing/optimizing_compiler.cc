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

#include "optimizing_compiler.h"

#include <fstream>
#include <stdint.h>

#include "builder.h"
#include "code_generator.h"
#include "compiler.h"
#include "constant_folding.h"
#include "dead_code_elimination.h"
#include "driver/compiler_driver.h"
#include "driver/dex_compilation_unit.h"
#include "graph_visualizer.h"
#include "gvn.h"
#include "instruction_simplifier.h"
#include "nodes.h"
#include "prepare_for_register_allocation.h"
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
  CodeVectorAllocator() {}

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

class OptimizingCompiler FINAL : public Compiler {
 public:
  explicit OptimizingCompiler(CompilerDriver* driver);
  ~OptimizingCompiler();

  bool CanCompileMethod(uint32_t method_idx, const DexFile& dex_file, CompilationUnit* cu) const
      OVERRIDE;

  CompiledMethod* Compile(const DexFile::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          jobject class_loader,
                          const DexFile& dex_file) const OVERRIDE;

  CompiledMethod* TryCompile(const DexFile::CodeItem* code_item,
                             uint32_t access_flags,
                             InvokeType invoke_type,
                             uint16_t class_def_idx,
                             uint32_t method_idx,
                             jobject class_loader,
                             const DexFile& dex_file) const;

  // For the following methods we will use the fallback. This is a delegation pattern.
  CompiledMethod* JniCompile(uint32_t access_flags,
                             uint32_t method_idx,
                             const DexFile& dex_file) const OVERRIDE;

  uintptr_t GetEntryPointOf(mirror::ArtMethod* method) const OVERRIDE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool WriteElf(art::File* file,
                OatWriter* oat_writer,
                const std::vector<const art::DexFile*>& dex_files,
                const std::string& android_root,
                bool is_host) const OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Backend* GetCodeGenerator(CompilationUnit* cu, void* compilation_unit) const OVERRIDE;

  void InitCompilationUnit(CompilationUnit& cu) const OVERRIDE;

  void Init() const OVERRIDE;

  void UnInit() const OVERRIDE;

 private:
  // Whether we should run any optimization or register allocation. If false, will
  // just run the code generation after the graph was built.
  const bool run_optimizations_;
  mutable AtomicInteger total_compiled_methods_;
  mutable AtomicInteger unoptimized_compiled_methods_;
  mutable AtomicInteger optimized_compiled_methods_;

  std::unique_ptr<std::ostream> visualizer_output_;

  // Delegate to another compiler in case the optimizing compiler cannot compile a method.
  // Currently the fallback is the quick compiler.
  std::unique_ptr<Compiler> delegate_;

  DISALLOW_COPY_AND_ASSIGN(OptimizingCompiler);
};

static const int kMaximumCompilationTimeBeforeWarning = 100; /* ms */

OptimizingCompiler::OptimizingCompiler(CompilerDriver* driver)
    : Compiler(driver, kMaximumCompilationTimeBeforeWarning),
      run_optimizations_(
          driver->GetCompilerOptions().GetCompilerFilter() != CompilerOptions::kTime),
      total_compiled_methods_(0),
      unoptimized_compiled_methods_(0),
      optimized_compiled_methods_(0),
      delegate_(Create(driver, Compiler::Kind::kQuick)) {
  if (kIsVisualizerEnabled) {
    visualizer_output_.reset(new std::ofstream("art.cfg"));
  }
}

void OptimizingCompiler::Init() const {
  delegate_->Init();
}

void OptimizingCompiler::UnInit() const {
  delegate_->UnInit();
}

OptimizingCompiler::~OptimizingCompiler() {
  if (total_compiled_methods_ == 0) {
    LOG(INFO) << "Did not compile any method.";
  } else {
    size_t unoptimized_percent = (unoptimized_compiled_methods_ * 100 / total_compiled_methods_);
    size_t optimized_percent = (optimized_compiled_methods_ * 100 / total_compiled_methods_);
    LOG(INFO) << "Compiled " << total_compiled_methods_ << " methods: "
              << unoptimized_percent << "% (" << unoptimized_compiled_methods_ << ") unoptimized, "
              << optimized_percent << "% (" << optimized_compiled_methods_ << ") optimized.";
  }
}

bool OptimizingCompiler::CanCompileMethod(uint32_t method_idx, const DexFile& dex_file,
                                          CompilationUnit* cu) const {
  return delegate_->CanCompileMethod(method_idx, dex_file, cu);
}

CompiledMethod* OptimizingCompiler::JniCompile(uint32_t access_flags,
                                               uint32_t method_idx,
                                               const DexFile& dex_file) const {
  return delegate_->JniCompile(access_flags, method_idx, dex_file);
}

uintptr_t OptimizingCompiler::GetEntryPointOf(mirror::ArtMethod* method) const {
  return delegate_->GetEntryPointOf(method);
}

bool OptimizingCompiler::WriteElf(art::File* file, OatWriter* oat_writer,
                                  const std::vector<const art::DexFile*>& dex_files,
                                  const std::string& android_root, bool is_host) const {
  return delegate_->WriteElf(file, oat_writer, dex_files, android_root, is_host);
}

Backend* OptimizingCompiler::GetCodeGenerator(CompilationUnit* cu, void* compilation_unit) const {
  return delegate_->GetCodeGenerator(cu, compilation_unit);
}

void OptimizingCompiler::InitCompilationUnit(CompilationUnit& cu) const {
  delegate_->InitCompilationUnit(cu);
}

CompiledMethod* OptimizingCompiler::TryCompile(const DexFile::CodeItem* code_item,
                                               uint32_t access_flags,
                                               InvokeType invoke_type,
                                               uint16_t class_def_idx,
                                               uint32_t method_idx,
                                               jobject class_loader,
                                               const DexFile& dex_file) const {
  total_compiled_methods_++;
  InstructionSet instruction_set = GetCompilerDriver()->GetInstructionSet();
  // Always use the thumb2 assembler: some runtime functionality (like implicit stack
  // overflow checks) assume thumb2.
  if (instruction_set == kArm) {
    instruction_set = kThumb2;
  }

  // Do not attempt to compile on architectures we do not support.
  if (instruction_set != kArm64 &&
      instruction_set != kThumb2 &&
      instruction_set != kX86 &&
      instruction_set != kX86_64) {
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

  if (run_optimizations_ && RegisterAllocator::CanAllocateRegistersFor(*graph, instruction_set)) {
    optimized_compiled_methods_++;
    graph->BuildDominatorTree();
    graph->TransformToSSA();
    visualizer.DumpGraph("ssa");
    graph->FindNaturalLoops();

    HDeadCodeElimination(graph, visualizer).Execute();
    HConstantFolding(graph, visualizer).Execute();

    SsaRedundantPhiElimination(graph).Run();
    SsaDeadPhiElimination(graph).Run();
    InstructionSimplifier(graph).Run();
    GlobalValueNumberer(graph->GetArena(), graph).Run();
    visualizer.DumpGraph(kGVNPassName);
    PrepareForRegisterAllocation(graph).Run();

    SsaLivenessAnalysis liveness(*graph, codegen);
    liveness.Analyze();
    visualizer.DumpGraph(kLivenessPassName);

    RegisterAllocator register_allocator(graph->GetArena(), codegen, liveness);
    register_allocator.AllocateRegisters();

    visualizer.DumpGraph(kRegisterAllocatorPassName);
    codegen->CompileOptimized(&allocator);

    std::vector<uint8_t> mapping_table;
    SrcMap src_mapping_table;
    codegen->BuildMappingTable(&mapping_table,
            GetCompilerDriver()->GetCompilerOptions().GetIncludeDebugSymbols() ?
                 &src_mapping_table : nullptr);

    std::vector<uint8_t> stack_map;
    codegen->BuildStackMaps(&stack_map);

    return new CompiledMethod(GetCompilerDriver(),
                              instruction_set,
                              allocator.GetMemory(),
                              codegen->GetFrameSize(),
                              codegen->GetCoreSpillMask(),
                              0, /* FPR spill mask, unused */
                              mapping_table,
                              stack_map);
  } else if (shouldOptimize && RegisterAllocator::Supports(instruction_set)) {
    LOG(FATAL) << "Could not allocate registers in optimizing compiler";
    return nullptr;
  } else {
    unoptimized_compiled_methods_++;
    codegen->CompileBaseline(&allocator);

    // Run these phases to get some test coverage.
    graph->BuildDominatorTree();
    graph->TransformToSSA();
    visualizer.DumpGraph("ssa");
    graph->FindNaturalLoops();
    SsaRedundantPhiElimination(graph).Run();
    SsaDeadPhiElimination(graph).Run();
    GlobalValueNumberer(graph->GetArena(), graph).Run();
    SsaLivenessAnalysis liveness(*graph, codegen);
    liveness.Analyze();
    visualizer.DumpGraph(kLivenessPassName);

    std::vector<uint8_t> mapping_table;
    SrcMap src_mapping_table;
    codegen->BuildMappingTable(&mapping_table,
            GetCompilerDriver()->GetCompilerOptions().GetIncludeDebugSymbols() ?
                 &src_mapping_table : nullptr);
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
                              &src_mapping_table,
                              mapping_table,
                              vmap_table,
                              gc_map,
                              nullptr);
  }
}

CompiledMethod* OptimizingCompiler::Compile(const DexFile::CodeItem* code_item,
                                            uint32_t access_flags,
                                            InvokeType invoke_type,
                                            uint16_t class_def_idx,
                                            uint32_t method_idx,
                                            jobject class_loader,
                                            const DexFile& dex_file) const {
  CompiledMethod* method = TryCompile(code_item, access_flags, invoke_type, class_def_idx,
                                      method_idx, class_loader, dex_file);
  if (method != nullptr) {
    return method;
  }

  return delegate_->Compile(code_item, access_flags, invoke_type, class_def_idx, method_idx,
                            class_loader, dex_file);
}

Compiler* CreateOptimizingCompiler(CompilerDriver* driver) {
  return new OptimizingCompiler(driver);
}

}  // namespace art
