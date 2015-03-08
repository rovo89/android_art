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

#include "art_method-inl.h"
#include "base/arena_allocator.h"
#include "base/dumpable.h"
#include "base/timing_logger.h"
#include "boolean_simplifier.h"
#include "bounds_check_elimination.h"
#include "builder.h"
#include "code_generator.h"
#include "compiled_method.h"
#include "compiler.h"
#include "constant_folding.h"
#include "dead_code_elimination.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/verified_method.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "elf_writer_quick.h"
#include "graph_visualizer.h"
#include "gvn.h"
#include "inliner.h"
#include "instruction_simplifier.h"
#include "intrinsics.h"
#include "licm.h"
#include "jni/quick/jni_compiler.h"
#include "nodes.h"
#include "prepare_for_register_allocation.h"
#include "reference_type_propagation.h"
#include "register_allocator.h"
#include "side_effects_analysis.h"
#include "ssa_builder.h"
#include "ssa_phi_elimination.h"
#include "ssa_liveness_analysis.h"
#include "utils/assembler.h"

namespace art {

/**
 * Used by the code generator, to allocate the code in a vector.
 */
class CodeVectorAllocator FINAL : public CodeAllocator {
 public:
  CodeVectorAllocator() : size_(0) {}

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
 * Filter to apply to the visualizer. Methods whose name contain that filter will
 * be dumped.
 */
static const char* kStringFilter = "";

class PassInfo;

class PassInfoPrinter : public ValueObject {
 public:
  PassInfoPrinter(HGraph* graph,
                  const char* method_name,
                  const CodeGenerator& codegen,
                  std::ostream* visualizer_output,
                  CompilerDriver* compiler_driver)
      : method_name_(method_name),
        timing_logger_enabled_(compiler_driver->GetDumpPasses()),
        timing_logger_(method_name, true, true),
        visualizer_enabled_(!compiler_driver->GetDumpCfgFileName().empty()),
        visualizer_(visualizer_output, graph, codegen) {
    if (strstr(method_name, kStringFilter) == nullptr) {
      timing_logger_enabled_ = visualizer_enabled_ = false;
    }
    if (visualizer_enabled_) {
      visualizer_.PrintHeader(method_name_);
    }
  }

  ~PassInfoPrinter() {
    if (timing_logger_enabled_) {
      LOG(INFO) << "TIMINGS " << method_name_;
      LOG(INFO) << Dumpable<TimingLogger>(timing_logger_);
    }
  }

 private:
  void StartPass(const char* pass_name) {
    // Dump graph first, then start timer.
    if (visualizer_enabled_) {
      visualizer_.DumpGraph(pass_name, /* is_after_pass */ false);
    }
    if (timing_logger_enabled_) {
      timing_logger_.StartTiming(pass_name);
    }
  }

  void EndPass(const char* pass_name) {
    // Pause timer first, then dump graph.
    if (timing_logger_enabled_) {
      timing_logger_.EndTiming();
    }
    if (visualizer_enabled_) {
      visualizer_.DumpGraph(pass_name, /* is_after_pass */ true);
    }
  }

  const char* method_name_;

  bool timing_logger_enabled_;
  TimingLogger timing_logger_;

  bool visualizer_enabled_;
  HGraphVisualizer visualizer_;

  friend PassInfo;

  DISALLOW_COPY_AND_ASSIGN(PassInfoPrinter);
};

class PassInfo : public ValueObject {
 public:
  PassInfo(const char *pass_name, PassInfoPrinter* pass_info_printer)
      : pass_name_(pass_name),
        pass_info_printer_(pass_info_printer) {
    pass_info_printer_->StartPass(pass_name_);
  }

  ~PassInfo() {
    pass_info_printer_->EndPass(pass_name_);
  }

 private:
  const char* const pass_name_;
  PassInfoPrinter* const pass_info_printer_;
};

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

  CompiledMethod* JniCompile(uint32_t access_flags,
                             uint32_t method_idx,
                             const DexFile& dex_file) const OVERRIDE {
    return ArtQuickJniCompileMethod(GetCompilerDriver(), access_flags, method_idx, dex_file);
  }

  uintptr_t GetEntryPointOf(ArtMethod* method) const OVERRIDE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return reinterpret_cast<uintptr_t>(method->GetEntryPointFromQuickCompiledCodePtrSize(
        InstructionSetPointerSize(GetCompilerDriver()->GetInstructionSet())));
  }

  void InitCompilationUnit(CompilationUnit& cu) const OVERRIDE;

  void Init() OVERRIDE;

  void UnInit() const OVERRIDE;

  void MaybeRecordStat(MethodCompilationStat compilation_stat) const {
    if (compilation_stats_.get() != nullptr) {
      compilation_stats_->RecordStat(compilation_stat);
    }
  }

 private:
  // Whether we should run any optimization or register allocation. If false, will
  // just run the code generation after the graph was built.
  const bool run_optimizations_;

  // Optimize and compile `graph`.
  CompiledMethod* CompileOptimized(HGraph* graph,
                                   CodeGenerator* codegen,
                                   CompilerDriver* driver,
                                   const DexFile& dex_file,
                                   const DexCompilationUnit& dex_compilation_unit,
                                   PassInfoPrinter* pass_info) const;

  // Just compile without doing optimizations.
  CompiledMethod* CompileBaseline(CodeGenerator* codegen,
                                  CompilerDriver* driver,
                                  const DexCompilationUnit& dex_compilation_unit) const;

  std::unique_ptr<OptimizingCompilerStats> compilation_stats_;

  std::unique_ptr<std::ostream> visualizer_output_;

  // Delegate to Quick in case the optimizing compiler cannot compile a method.
  std::unique_ptr<Compiler> delegate_;

  DISALLOW_COPY_AND_ASSIGN(OptimizingCompiler);
};

static const int kMaximumCompilationTimeBeforeWarning = 100; /* ms */

OptimizingCompiler::OptimizingCompiler(CompilerDriver* driver)
    : Compiler(driver, kMaximumCompilationTimeBeforeWarning),
      run_optimizations_(
          (driver->GetCompilerOptions().GetCompilerFilter() != CompilerOptions::kTime)
          && !driver->GetCompilerOptions().GetDebuggable()),
      delegate_(Create(driver, Compiler::Kind::kQuick)) {}

void OptimizingCompiler::Init() {
  delegate_->Init();
  // Enable C1visualizer output. Must be done in Init() because the compiler
  // driver is not fully initialized when passed to the compiler's constructor.
  CompilerDriver* driver = GetCompilerDriver();
  const std::string cfg_file_name = driver->GetDumpCfgFileName();
  if (!cfg_file_name.empty()) {
    CHECK_EQ(driver->GetThreadCount(), 1U)
      << "Graph visualizer requires the compiler to run single-threaded. "
      << "Invoke the compiler with '-j1'.";
    visualizer_output_.reset(new std::ofstream(cfg_file_name));
  }
  if (driver->GetDumpStats()) {
    compilation_stats_.reset(new OptimizingCompilerStats());
  }
}

void OptimizingCompiler::UnInit() const {
  delegate_->UnInit();
}

OptimizingCompiler::~OptimizingCompiler() {
  if (compilation_stats_.get() != nullptr) {
    compilation_stats_->Log();
  }
}

void OptimizingCompiler::InitCompilationUnit(CompilationUnit& cu) const {
  delegate_->InitCompilationUnit(cu);
}

bool OptimizingCompiler::CanCompileMethod(uint32_t method_idx ATTRIBUTE_UNUSED,
                                          const DexFile& dex_file ATTRIBUTE_UNUSED,
                                          CompilationUnit* cu ATTRIBUTE_UNUSED) const {
  return true;
}

static bool IsInstructionSetSupported(InstructionSet instruction_set) {
  return instruction_set == kArm64
      || (instruction_set == kThumb2 && !kArm32QuickCodeUseSoftFloat)
      || instruction_set == kMips64
      || instruction_set == kX86
      || instruction_set == kX86_64;
}

static bool CanOptimize(const DexFile::CodeItem& code_item) {
  // TODO: We currently cannot optimize methods with try/catch.
  return code_item.tries_size_ == 0;
}

static void RunOptimizations(HOptimization* optimizations[],
                             size_t length,
                             PassInfoPrinter* pass_info_printer) {
  for (size_t i = 0; i < length; ++i) {
    HOptimization* optimization = optimizations[i];
    {
      PassInfo pass_info(optimization->GetPassName(), pass_info_printer);
      optimization->Run();
    }
    optimization->Check();
  }
}

static void RunOptimizations(HGraph* graph,
                             CompilerDriver* driver,
                             OptimizingCompilerStats* stats,
                             const DexFile& dex_file,
                             const DexCompilationUnit& dex_compilation_unit,
                             PassInfoPrinter* pass_info_printer,
                             StackHandleScopeCollection* handles) {
  HDeadCodeElimination dce1(graph, stats,
                            HDeadCodeElimination::kInitialDeadCodeEliminationPassName);
  HDeadCodeElimination dce2(graph, stats,
                            HDeadCodeElimination::kFinalDeadCodeEliminationPassName);
  HConstantFolding fold1(graph);
  InstructionSimplifier simplify1(graph, stats);
  HBooleanSimplifier boolean_simplify(graph);

  HConstantFolding fold2(graph, "constant_folding_after_inlining");
  SideEffectsAnalysis side_effects(graph);
  GVNOptimization gvn(graph, side_effects);
  LICM licm(graph, side_effects);
  BoundsCheckElimination bce(graph);
  ReferenceTypePropagation type_propagation(graph, dex_file, dex_compilation_unit, handles);
  InstructionSimplifier simplify2(graph, stats, "instruction_simplifier_after_types");
  InstructionSimplifier simplify3(graph, stats, "instruction_simplifier_before_codegen");

  IntrinsicsRecognizer intrinsics(graph, dex_compilation_unit.GetDexFile(), driver);

  HOptimization* optimizations[] = {
    &intrinsics,
    &fold1,
    &simplify1,
    &dce1,
    // BooleanSimplifier depends on the InstructionSimplifier removing redundant
    // suspend checks to recognize empty blocks.
    &boolean_simplify,
    &fold2,
    &side_effects,
    &gvn,
    &licm,
    &bce,
    &type_propagation,
    &simplify2,
    &dce2,
    // The codegen has a few assumptions that only the instruction simplifier can
    // satisfy. For example, the code generator does not expect to see a
    // HTypeConversion from a type to the same type.
    &simplify3,
  };

  RunOptimizations(optimizations, arraysize(optimizations), pass_info_printer);
}

// The stack map we generate must be 4-byte aligned on ARM. Since existing
// maps are generated alongside these stack maps, we must also align them.
static ArrayRef<const uint8_t> AlignVectorSize(std::vector<uint8_t>& vector) {
  size_t size = vector.size();
  size_t aligned_size = RoundUp(size, 4);
  for (; size < aligned_size; ++size) {
    vector.push_back(0);
  }
  return ArrayRef<const uint8_t>(vector);
}

static void AllocateRegisters(HGraph* graph,
                              CodeGenerator* codegen,
                              PassInfoPrinter* pass_info_printer) {
  PrepareForRegisterAllocation(graph).Run();
  SsaLivenessAnalysis liveness(graph, codegen);
  {
    PassInfo pass_info(SsaLivenessAnalysis::kLivenessPassName, pass_info_printer);
    liveness.Analyze();
  }
  {
    PassInfo pass_info(RegisterAllocator::kRegisterAllocatorPassName, pass_info_printer);
    RegisterAllocator(graph->GetArena(), codegen, liveness).AllocateRegisters();
  }
}

CompiledMethod* OptimizingCompiler::CompileOptimized(HGraph* graph,
                                                     CodeGenerator* codegen,
                                                     CompilerDriver* compiler_driver,
                                                     const DexFile& dex_file,
                                                     const DexCompilationUnit& dex_compilation_unit,
                                                     PassInfoPrinter* pass_info_printer) const {
  StackHandleScopeCollection handles(Thread::Current());
  RunOptimizations(graph, compiler_driver, compilation_stats_.get(),
                   dex_file, dex_compilation_unit, pass_info_printer, &handles);

  AllocateRegisters(graph, codegen, pass_info_printer);

  CodeVectorAllocator allocator;
  codegen->CompileOptimized(&allocator);

  DefaultSrcMap src_mapping_table;
  if (compiler_driver->GetCompilerOptions().GetGenerateDebugInfo()) {
    codegen->BuildSourceMap(&src_mapping_table);
  }

  std::vector<uint8_t> stack_map;
  codegen->BuildStackMaps(&stack_map);

  MaybeRecordStat(MethodCompilationStat::kCompiledOptimized);

  return CompiledMethod::SwapAllocCompiledMethod(
      compiler_driver,
      codegen->GetInstructionSet(),
      ArrayRef<const uint8_t>(allocator.GetMemory()),
      // Follow Quick's behavior and set the frame size to zero if it is
      // considered "empty" (see the definition of
      // art::CodeGenerator::HasEmptyFrame).
      codegen->HasEmptyFrame() ? 0 : codegen->GetFrameSize(),
      codegen->GetCoreSpillMask(),
      codegen->GetFpuSpillMask(),
      &src_mapping_table,
      ArrayRef<const uint8_t>(),  // mapping_table.
      ArrayRef<const uint8_t>(stack_map),
      ArrayRef<const uint8_t>(),  // native_gc_map.
      ArrayRef<const uint8_t>(*codegen->GetAssembler()->cfi().data()),
      ArrayRef<const LinkerPatch>());
}

CompiledMethod* OptimizingCompiler::CompileBaseline(
    CodeGenerator* codegen,
    CompilerDriver* compiler_driver,
    const DexCompilationUnit& dex_compilation_unit) const {
  CodeVectorAllocator allocator;
  codegen->CompileBaseline(&allocator);

  std::vector<uint8_t> mapping_table;
  codegen->BuildMappingTable(&mapping_table);
  DefaultSrcMap src_mapping_table;
  if (compiler_driver->GetCompilerOptions().GetGenerateDebugInfo()) {
    codegen->BuildSourceMap(&src_mapping_table);
  }
  std::vector<uint8_t> vmap_table;
  codegen->BuildVMapTable(&vmap_table);
  std::vector<uint8_t> gc_map;
  codegen->BuildNativeGCMap(&gc_map, dex_compilation_unit);

  MaybeRecordStat(MethodCompilationStat::kCompiledBaseline);
  return CompiledMethod::SwapAllocCompiledMethod(
      compiler_driver,
      codegen->GetInstructionSet(),
      ArrayRef<const uint8_t>(allocator.GetMemory()),
      // Follow Quick's behavior and set the frame size to zero if it is
      // considered "empty" (see the definition of
      // art::CodeGenerator::HasEmptyFrame).
      codegen->HasEmptyFrame() ? 0 : codegen->GetFrameSize(),
      codegen->GetCoreSpillMask(),
      codegen->GetFpuSpillMask(),
      &src_mapping_table,
      AlignVectorSize(mapping_table),
      AlignVectorSize(vmap_table),
      AlignVectorSize(gc_map),
      ArrayRef<const uint8_t>(*codegen->GetAssembler()->cfi().data()),
      ArrayRef<const LinkerPatch>());
}

CompiledMethod* OptimizingCompiler::TryCompile(const DexFile::CodeItem* code_item,
                                               uint32_t access_flags,
                                               InvokeType invoke_type,
                                               uint16_t class_def_idx,
                                               uint32_t method_idx,
                                               jobject class_loader,
                                               const DexFile& dex_file) const {
  UNUSED(invoke_type);
  std::string method_name = PrettyMethod(method_idx, dex_file);
  MaybeRecordStat(MethodCompilationStat::kAttemptCompilation);
  CompilerDriver* compiler_driver = GetCompilerDriver();
  InstructionSet instruction_set = compiler_driver->GetInstructionSet();
  // Always use the thumb2 assembler: some runtime functionality (like implicit stack
  // overflow checks) assume thumb2.
  if (instruction_set == kArm) {
    instruction_set = kThumb2;
  }

  // `run_optimizations_` is set explicitly (either through a compiler filter
  // or the debuggable flag). If it is set, we can run baseline. Otherwise, we
  // fall back to Quick.
  bool should_use_baseline = !run_optimizations_;
  bool can_optimize = CanOptimize(*code_item);
  if (!can_optimize && !should_use_baseline) {
    // We know we will not compile this method. Bail out before doing any work.
    return nullptr;
  }

  // Do not attempt to compile on architectures we do not support.
  if (!IsInstructionSetSupported(instruction_set)) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledUnsupportedIsa);
    return nullptr;
  }

  if (Compiler::IsPathologicalCase(*code_item, method_idx, dex_file)) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledPathological);
    return nullptr;
  }

  // Implementation of the space filter: do not compile a code item whose size in
  // code units is bigger than 128.
  static constexpr size_t kSpaceFilterOptimizingThreshold = 128;
  const CompilerOptions& compiler_options = compiler_driver->GetCompilerOptions();
  if ((compiler_options.GetCompilerFilter() == CompilerOptions::kSpace)
      && (code_item->insns_size_in_code_units_ > kSpaceFilterOptimizingThreshold)) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledSpaceFilter);
    return nullptr;
  }

  DexCompilationUnit dex_compilation_unit(
    nullptr, class_loader, art::Runtime::Current()->GetClassLinker(), dex_file, code_item,
    class_def_idx, method_idx, access_flags,
    compiler_driver->GetVerifiedMethod(&dex_file, method_idx));

  ArenaAllocator arena(Runtime::Current()->GetArenaPool());
  HGraph* graph = new (&arena) HGraph(
      &arena, dex_file, method_idx, compiler_driver->GetInstructionSet(),
      compiler_driver->GetCompilerOptions().GetDebuggable());

  // For testing purposes, we put a special marker on method names that should be compiled
  // with this compiler. This makes sure we're not regressing.
  bool shouldCompile = method_name.find("$opt$") != std::string::npos;
  bool shouldOptimize = method_name.find("$opt$reg$") != std::string::npos && run_optimizations_;

  std::unique_ptr<CodeGenerator> codegen(
      CodeGenerator::Create(graph,
                            instruction_set,
                            *compiler_driver->GetInstructionSetFeatures(),
                            compiler_driver->GetCompilerOptions()));
  if (codegen.get() == nullptr) {
    CHECK(!shouldCompile) << "Could not find code generator for optimizing compiler";
    MaybeRecordStat(MethodCompilationStat::kNotCompiledNoCodegen);
    return nullptr;
  }
  codegen->GetAssembler()->cfi().SetEnabled(
      compiler_driver->GetCompilerOptions().GetGenerateDebugInfo());

  PassInfoPrinter pass_info_printer(graph,
                                    method_name.c_str(),
                                    *codegen.get(),
                                    visualizer_output_.get(),
                                    compiler_driver);

  HGraphBuilder builder(graph,
                        &dex_compilation_unit,
                        &dex_compilation_unit,
                        &dex_file,
                        compiler_driver,
                        compilation_stats_.get());

  VLOG(compiler) << "Building " << method_name;

  {
    PassInfo pass_info(HGraphBuilder::kBuilderPassName, &pass_info_printer);
    if (!builder.BuildGraph(*code_item)) {
      DCHECK(!(IsCompilingWithCoreImage() && shouldCompile))
          << "Could not build graph in optimizing compiler";
      return nullptr;
    }
  }

  bool can_allocate_registers = RegisterAllocator::CanAllocateRegistersFor(*graph, instruction_set);

  if (run_optimizations_ && can_optimize && can_allocate_registers) {
    VLOG(compiler) << "Optimizing " << method_name;

    {
      PassInfo pass_info(SsaBuilder::kSsaBuilderPassName, &pass_info_printer);
      if (!graph->TryBuildingSsa()) {
        // We could not transform the graph to SSA, bailout.
        LOG(INFO) << "Skipping compilation of " << method_name << ": it contains a non natural loop";
        MaybeRecordStat(MethodCompilationStat::kNotCompiledCannotBuildSSA);
        return nullptr;
      }
    }

    return CompileOptimized(graph,
                            codegen.get(),
                            compiler_driver,
                            dex_file,
                            dex_compilation_unit,
                            &pass_info_printer);
  } else if (shouldOptimize && can_allocate_registers) {
    LOG(FATAL) << "Could not allocate registers in optimizing compiler";
    UNREACHABLE();
  } else if (should_use_baseline) {
    VLOG(compiler) << "Compile baseline " << method_name;

    if (!run_optimizations_) {
      MaybeRecordStat(MethodCompilationStat::kNotOptimizedDisabled);
    } else if (!can_optimize) {
      MaybeRecordStat(MethodCompilationStat::kNotOptimizedTryCatch);
    } else if (!can_allocate_registers) {
      MaybeRecordStat(MethodCompilationStat::kNotOptimizedRegisterAllocator);
    }

    return CompileBaseline(codegen.get(), compiler_driver, dex_compilation_unit);
  } else {
    return nullptr;
  }
}

CompiledMethod* OptimizingCompiler::Compile(const DexFile::CodeItem* code_item,
                                            uint32_t access_flags,
                                            InvokeType invoke_type,
                                            uint16_t class_def_idx,
                                            uint32_t method_idx,
                                            jobject jclass_loader,
                                            const DexFile& dex_file) const {
  CompilerDriver* compiler_driver = GetCompilerDriver();
  CompiledMethod* method = nullptr;
  if (compiler_driver->IsMethodVerifiedWithoutFailures(method_idx, class_def_idx, dex_file) &&
      !compiler_driver->GetVerifiedMethod(&dex_file, method_idx)->HasRuntimeThrow()) {
     method = TryCompile(code_item, access_flags, invoke_type, class_def_idx,
                         method_idx, jclass_loader, dex_file);
  } else {
    if (compiler_driver->GetCompilerOptions().VerifyAtRuntime()) {
      MaybeRecordStat(MethodCompilationStat::kNotCompiledVerifyAtRuntime);
    } else {
      MaybeRecordStat(MethodCompilationStat::kNotCompiledClassNotVerified);
    }
  }

  if (method != nullptr) {
    return method;
  }
  method = delegate_->Compile(code_item, access_flags, invoke_type, class_def_idx, method_idx,
                              jclass_loader, dex_file);

  if (method != nullptr) {
    MaybeRecordStat(MethodCompilationStat::kCompiledQuick);
  }
  return method;
}

Compiler* CreateOptimizingCompiler(CompilerDriver* driver) {
  return new OptimizingCompiler(driver);
}

bool IsCompilingWithCoreImage() {
  const std::string& image = Runtime::Current()->GetImageLocation();
  return EndsWith(image, "core.art") || EndsWith(image, "core-optimizing.art");
}

}  // namespace art
