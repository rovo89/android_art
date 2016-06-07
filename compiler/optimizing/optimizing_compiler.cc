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
#include <memory>
#include <stdint.h>

#ifdef ART_ENABLE_CODEGEN_arm
#include "dex_cache_array_fixups_arm.h"
#endif

#ifdef ART_ENABLE_CODEGEN_arm64
#include "instruction_simplifier_arm64.h"
#endif

#ifdef ART_ENABLE_CODEGEN_x86
#include "pc_relative_fixups_x86.h"
#endif

#include "art_method-inl.h"
#include "base/arena_allocator.h"
#include "base/arena_containers.h"
#include "base/dumpable.h"
#include "base/macros.h"
#include "base/timing_logger.h"
#include "bounds_check_elimination.h"
#include "builder.h"
#include "code_generator.h"
#include "compiled_method.h"
#include "compiler.h"
#include "constant_folding.h"
#include "dead_code_elimination.h"
#include "debug/elf_debug_writer.h"
#include "debug/method_debug_info.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/verification_results.h"
#include "dex/verified_method.h"
#include "driver/compiler_driver-inl.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "elf_writer_quick.h"
#include "graph_checker.h"
#include "graph_visualizer.h"
#include "gvn.h"
#include "induction_var_analysis.h"
#include "inliner.h"
#include "instruction_simplifier.h"
#include "instruction_simplifier_arm.h"
#include "intrinsics.h"
#include "jit/debugger_interface.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jni/quick/jni_compiler.h"
#include "licm.h"
#include "load_store_elimination.h"
#include "nodes.h"
#include "oat_quick_method_header.h"
#include "prepare_for_register_allocation.h"
#include "reference_type_propagation.h"
#include "register_allocator.h"
#include "select_generator.h"
#include "sharpening.h"
#include "side_effects_analysis.h"
#include "ssa_builder.h"
#include "ssa_liveness_analysis.h"
#include "ssa_phi_elimination.h"
#include "utils/assembler.h"
#include "verifier/method_verifier.h"

namespace art {

static constexpr size_t kArenaAllocatorMemoryReportThreshold = 8 * MB;

/**
 * Used by the code generator, to allocate the code in a vector.
 */
class CodeVectorAllocator FINAL : public CodeAllocator {
 public:
  explicit CodeVectorAllocator(ArenaAllocator* arena)
      : memory_(arena->Adapter(kArenaAllocCodeBuffer)),
        size_(0) {}

  virtual uint8_t* Allocate(size_t size) {
    size_ = size;
    memory_.resize(size);
    return &memory_[0];
  }

  size_t GetSize() const { return size_; }
  const ArenaVector<uint8_t>& GetMemory() const { return memory_; }

 private:
  ArenaVector<uint8_t> memory_;
  size_t size_;

  DISALLOW_COPY_AND_ASSIGN(CodeVectorAllocator);
};

/**
 * Filter to apply to the visualizer. Methods whose name contain that filter will
 * be dumped.
 */
static constexpr const char kStringFilter[] = "";

class PassScope;

class PassObserver : public ValueObject {
 public:
  PassObserver(HGraph* graph,
               CodeGenerator* codegen,
               std::ostream* visualizer_output,
               CompilerDriver* compiler_driver)
      : graph_(graph),
        cached_method_name_(),
        timing_logger_enabled_(compiler_driver->GetDumpPasses()),
        timing_logger_(timing_logger_enabled_ ? GetMethodName() : "", true, true),
        disasm_info_(graph->GetArena()),
        visualizer_enabled_(!compiler_driver->GetCompilerOptions().GetDumpCfgFileName().empty()),
        visualizer_(visualizer_output, graph, *codegen),
        graph_in_bad_state_(false) {
    if (timing_logger_enabled_ || visualizer_enabled_) {
      if (!IsVerboseMethod(compiler_driver, GetMethodName())) {
        timing_logger_enabled_ = visualizer_enabled_ = false;
      }
      if (visualizer_enabled_) {
        visualizer_.PrintHeader(GetMethodName());
        codegen->SetDisassemblyInformation(&disasm_info_);
      }
    }
  }

  ~PassObserver() {
    if (timing_logger_enabled_) {
      LOG(INFO) << "TIMINGS " << GetMethodName();
      LOG(INFO) << Dumpable<TimingLogger>(timing_logger_);
    }
  }

  void DumpDisassembly() const {
    if (visualizer_enabled_) {
      visualizer_.DumpGraphWithDisassembly();
    }
  }

  void SetGraphInBadState() { graph_in_bad_state_ = true; }

  const char* GetMethodName() {
    // PrettyMethod() is expensive, so we delay calling it until we actually have to.
    if (cached_method_name_.empty()) {
      cached_method_name_ = PrettyMethod(graph_->GetMethodIdx(), graph_->GetDexFile());
    }
    return cached_method_name_.c_str();
  }

 private:
  void StartPass(const char* pass_name) {
    // Dump graph first, then start timer.
    if (visualizer_enabled_) {
      visualizer_.DumpGraph(pass_name, /* is_after_pass */ false, graph_in_bad_state_);
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
      visualizer_.DumpGraph(pass_name, /* is_after_pass */ true, graph_in_bad_state_);
    }

    // Validate the HGraph if running in debug mode.
    if (kIsDebugBuild) {
      if (!graph_in_bad_state_) {
        GraphChecker checker(graph_);
        checker.Run();
        if (!checker.IsValid()) {
          LOG(FATAL) << "Error after " << pass_name << ": " << Dumpable<GraphChecker>(checker);
        }
      }
    }
  }

  static bool IsVerboseMethod(CompilerDriver* compiler_driver, const char* method_name) {
    // Test an exact match to --verbose-methods. If verbose-methods is set, this overrides an
    // empty kStringFilter matching all methods.
    if (compiler_driver->GetCompilerOptions().HasVerboseMethods()) {
      return compiler_driver->GetCompilerOptions().IsVerboseMethod(method_name);
    }

    // Test the kStringFilter sub-string. constexpr helper variable to silence unreachable-code
    // warning when the string is empty.
    constexpr bool kStringFilterEmpty = arraysize(kStringFilter) <= 1;
    if (kStringFilterEmpty || strstr(method_name, kStringFilter) != nullptr) {
      return true;
    }

    return false;
  }

  HGraph* const graph_;

  std::string cached_method_name_;

  bool timing_logger_enabled_;
  TimingLogger timing_logger_;

  DisassemblyInformation disasm_info_;

  bool visualizer_enabled_;
  HGraphVisualizer visualizer_;

  // Flag to be set by the compiler if the pass failed and the graph is not
  // expected to validate.
  bool graph_in_bad_state_;

  friend PassScope;

  DISALLOW_COPY_AND_ASSIGN(PassObserver);
};

class PassScope : public ValueObject {
 public:
  PassScope(const char *pass_name, PassObserver* pass_observer)
      : pass_name_(pass_name),
        pass_observer_(pass_observer) {
    pass_observer_->StartPass(pass_name_);
  }

  ~PassScope() {
    pass_observer_->EndPass(pass_name_);
  }

 private:
  const char* const pass_name_;
  PassObserver* const pass_observer_;
};

class OptimizingCompiler FINAL : public Compiler {
 public:
  explicit OptimizingCompiler(CompilerDriver* driver);
  ~OptimizingCompiler();

  bool CanCompileMethod(uint32_t method_idx, const DexFile& dex_file) const OVERRIDE;

  CompiledMethod* Compile(const DexFile::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          jobject class_loader,
                          const DexFile& dex_file,
                          Handle<mirror::DexCache> dex_cache) const OVERRIDE;

  CompiledMethod* JniCompile(uint32_t access_flags,
                             uint32_t method_idx,
                             const DexFile& dex_file) const OVERRIDE {
    return ArtQuickJniCompileMethod(GetCompilerDriver(), access_flags, method_idx, dex_file);
  }

  uintptr_t GetEntryPointOf(ArtMethod* method) const OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_) {
    return reinterpret_cast<uintptr_t>(method->GetEntryPointFromQuickCompiledCodePtrSize(
        InstructionSetPointerSize(GetCompilerDriver()->GetInstructionSet())));
  }

  void Init() OVERRIDE;

  void UnInit() const OVERRIDE;

  void MaybeRecordStat(MethodCompilationStat compilation_stat) const {
    if (compilation_stats_.get() != nullptr) {
      compilation_stats_->RecordStat(compilation_stat);
    }
  }

  bool JitCompile(Thread* self, jit::JitCodeCache* code_cache, ArtMethod* method, bool osr)
      OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_);

 private:
  // Create a 'CompiledMethod' for an optimized graph.
  CompiledMethod* Emit(ArenaAllocator* arena,
                       CodeVectorAllocator* code_allocator,
                       CodeGenerator* codegen,
                       CompilerDriver* driver,
                       const DexFile::CodeItem* item) const;

  // Try compiling a method and return the code generator used for
  // compiling it.
  // This method:
  // 1) Builds the graph. Returns null if it failed to build it.
  // 2) Transforms the graph to SSA. Returns null if it failed.
  // 3) Runs optimizations on the graph, including register allocator.
  // 4) Generates code with the `code_allocator` provided.
  CodeGenerator* TryCompile(ArenaAllocator* arena,
                            CodeVectorAllocator* code_allocator,
                            const DexFile::CodeItem* code_item,
                            uint32_t access_flags,
                            InvokeType invoke_type,
                            uint16_t class_def_idx,
                            uint32_t method_idx,
                            jobject class_loader,
                            const DexFile& dex_file,
                            Handle<mirror::DexCache> dex_cache,
                            ArtMethod* method,
                            bool osr) const;

  std::unique_ptr<OptimizingCompilerStats> compilation_stats_;

  std::unique_ptr<std::ostream> visualizer_output_;

  DISALLOW_COPY_AND_ASSIGN(OptimizingCompiler);
};

static const int kMaximumCompilationTimeBeforeWarning = 100; /* ms */

OptimizingCompiler::OptimizingCompiler(CompilerDriver* driver)
    : Compiler(driver, kMaximumCompilationTimeBeforeWarning) {}

void OptimizingCompiler::Init() {
  // Enable C1visualizer output. Must be done in Init() because the compiler
  // driver is not fully initialized when passed to the compiler's constructor.
  CompilerDriver* driver = GetCompilerDriver();
  const std::string cfg_file_name = driver->GetCompilerOptions().GetDumpCfgFileName();
  if (!cfg_file_name.empty()) {
    CHECK_EQ(driver->GetThreadCount(), 1U)
      << "Graph visualizer requires the compiler to run single-threaded. "
      << "Invoke the compiler with '-j1'.";
    std::ios_base::openmode cfg_file_mode =
        driver->GetCompilerOptions().GetDumpCfgAppend() ? std::ofstream::app : std::ofstream::out;
    visualizer_output_.reset(new std::ofstream(cfg_file_name, cfg_file_mode));
  }
  if (driver->GetDumpStats()) {
    compilation_stats_.reset(new OptimizingCompilerStats());
  }
}

void OptimizingCompiler::UnInit() const {
}

OptimizingCompiler::~OptimizingCompiler() {
  if (compilation_stats_.get() != nullptr) {
    compilation_stats_->Log();
  }
}

bool OptimizingCompiler::CanCompileMethod(uint32_t method_idx ATTRIBUTE_UNUSED,
                                          const DexFile& dex_file ATTRIBUTE_UNUSED) const {
  return true;
}

static bool IsInstructionSetSupported(InstructionSet instruction_set) {
  return (instruction_set == kArm && !kArm32QuickCodeUseSoftFloat)
      || instruction_set == kArm64
      || (instruction_set == kThumb2 && !kArm32QuickCodeUseSoftFloat)
      || instruction_set == kMips
      || instruction_set == kMips64
      || instruction_set == kX86
      || instruction_set == kX86_64;
}

// Read barrier are supported on ARM, ARM64, x86 and x86-64 at the moment.
// TODO: Add support for other architectures and remove this function
static bool InstructionSetSupportsReadBarrier(InstructionSet instruction_set) {
  return instruction_set == kArm64
      || instruction_set == kThumb2
      || instruction_set == kX86
      || instruction_set == kX86_64;
}

static void RunOptimizations(HOptimization* optimizations[],
                             size_t length,
                             PassObserver* pass_observer) {
  for (size_t i = 0; i < length; ++i) {
    PassScope scope(optimizations[i]->GetPassName(), pass_observer);
    optimizations[i]->Run();
  }
}

static void MaybeRunInliner(HGraph* graph,
                            CodeGenerator* codegen,
                            CompilerDriver* driver,
                            OptimizingCompilerStats* stats,
                            const DexCompilationUnit& dex_compilation_unit,
                            PassObserver* pass_observer,
                            StackHandleScopeCollection* handles) {
  const CompilerOptions& compiler_options = driver->GetCompilerOptions();
  bool should_inline = (compiler_options.GetInlineDepthLimit() > 0)
      && (compiler_options.GetInlineMaxCodeUnits() > 0);
  if (!should_inline) {
    return;
  }
  size_t number_of_dex_registers = dex_compilation_unit.GetCodeItem()->registers_size_;
  HInliner* inliner = new (graph->GetArena()) HInliner(
      graph,
      graph,
      codegen,
      dex_compilation_unit,
      dex_compilation_unit,
      driver,
      handles,
      stats,
      number_of_dex_registers,
      /* depth */ 0);
  HOptimization* optimizations[] = { inliner };

  RunOptimizations(optimizations, arraysize(optimizations), pass_observer);
}

static void RunArchOptimizations(InstructionSet instruction_set,
                                 HGraph* graph,
                                 CodeGenerator* codegen,
                                 OptimizingCompilerStats* stats,
                                 PassObserver* pass_observer) {
  ArenaAllocator* arena = graph->GetArena();
  switch (instruction_set) {
#ifdef ART_ENABLE_CODEGEN_arm
    case kThumb2:
    case kArm: {
      arm::DexCacheArrayFixups* fixups = new (arena) arm::DexCacheArrayFixups(graph, stats);
      arm::InstructionSimplifierArm* simplifier =
          new (arena) arm::InstructionSimplifierArm(graph, stats);
      HOptimization* arm_optimizations[] = {
        simplifier,
        fixups
      };
      RunOptimizations(arm_optimizations, arraysize(arm_optimizations), pass_observer);
      break;
    }
#endif
#ifdef ART_ENABLE_CODEGEN_arm64
    case kArm64: {
      arm64::InstructionSimplifierArm64* simplifier =
          new (arena) arm64::InstructionSimplifierArm64(graph, stats);
      SideEffectsAnalysis* side_effects = new (arena) SideEffectsAnalysis(graph);
      GVNOptimization* gvn = new (arena) GVNOptimization(graph, *side_effects, "GVN_after_arch");
      HOptimization* arm64_optimizations[] = {
        simplifier,
        side_effects,
        gvn
      };
      RunOptimizations(arm64_optimizations, arraysize(arm64_optimizations), pass_observer);
      break;
    }
#endif
#ifdef ART_ENABLE_CODEGEN_x86
    case kX86: {
      x86::PcRelativeFixups* pc_relative_fixups =
          new (arena) x86::PcRelativeFixups(graph, codegen, stats);
      HOptimization* x86_optimizations[] = {
          pc_relative_fixups
      };
      RunOptimizations(x86_optimizations, arraysize(x86_optimizations), pass_observer);
      break;
    }
#endif
    default:
      break;
  }
}

NO_INLINE  // Avoid increasing caller's frame size by large stack-allocated objects.
static void AllocateRegisters(HGraph* graph,
                              CodeGenerator* codegen,
                              PassObserver* pass_observer) {
  {
    PassScope scope(PrepareForRegisterAllocation::kPrepareForRegisterAllocationPassName,
                    pass_observer);
    PrepareForRegisterAllocation(graph).Run();
  }
  SsaLivenessAnalysis liveness(graph, codegen);
  {
    PassScope scope(SsaLivenessAnalysis::kLivenessPassName, pass_observer);
    liveness.Analyze();
  }
  {
    PassScope scope(RegisterAllocator::kRegisterAllocatorPassName, pass_observer);
    RegisterAllocator(graph->GetArena(), codegen, liveness).AllocateRegisters();
  }
}

static void RunOptimizations(HGraph* graph,
                             CodeGenerator* codegen,
                             CompilerDriver* driver,
                             OptimizingCompilerStats* stats,
                             const DexCompilationUnit& dex_compilation_unit,
                             PassObserver* pass_observer,
                             StackHandleScopeCollection* handles) {
  ArenaAllocator* arena = graph->GetArena();
  HDeadCodeElimination* dce1 = new (arena) HDeadCodeElimination(
      graph, stats, HDeadCodeElimination::kInitialDeadCodeEliminationPassName);
  HDeadCodeElimination* dce2 = new (arena) HDeadCodeElimination(
      graph, stats, HDeadCodeElimination::kFinalDeadCodeEliminationPassName);
  HConstantFolding* fold1 = new (arena) HConstantFolding(graph);
  InstructionSimplifier* simplify1 = new (arena) InstructionSimplifier(graph, stats);
  HSelectGenerator* select_generator = new (arena) HSelectGenerator(graph, stats);
  HConstantFolding* fold2 = new (arena) HConstantFolding(graph, "constant_folding_after_inlining");
  HConstantFolding* fold3 = new (arena) HConstantFolding(graph, "constant_folding_after_bce");
  SideEffectsAnalysis* side_effects = new (arena) SideEffectsAnalysis(graph);
  GVNOptimization* gvn = new (arena) GVNOptimization(graph, *side_effects);
  LICM* licm = new (arena) LICM(graph, *side_effects, stats);
  LoadStoreElimination* lse = new (arena) LoadStoreElimination(graph, *side_effects);
  HInductionVarAnalysis* induction = new (arena) HInductionVarAnalysis(graph);
  BoundsCheckElimination* bce = new (arena) BoundsCheckElimination(graph, *side_effects, induction);
  HSharpening* sharpening = new (arena) HSharpening(graph, codegen, dex_compilation_unit, driver);
  InstructionSimplifier* simplify2 = new (arena) InstructionSimplifier(
      graph, stats, "instruction_simplifier_after_bce");
  InstructionSimplifier* simplify3 = new (arena) InstructionSimplifier(
      graph, stats, "instruction_simplifier_before_codegen");
  IntrinsicsRecognizer* intrinsics = new (arena) IntrinsicsRecognizer(graph, driver, stats);

  HOptimization* optimizations1[] = {
    intrinsics,
    sharpening,
    fold1,
    simplify1,
    dce1,
  };
  RunOptimizations(optimizations1, arraysize(optimizations1), pass_observer);

  MaybeRunInliner(graph, codegen, driver, stats, dex_compilation_unit, pass_observer, handles);

  HOptimization* optimizations2[] = {
    // SelectGenerator depends on the InstructionSimplifier removing
    // redundant suspend checks to recognize empty blocks.
    select_generator,
    fold2,  // TODO: if we don't inline we can also skip fold2.
    side_effects,
    gvn,
    licm,
    induction,
    bce,
    fold3,  // evaluates code generated by dynamic bce
    simplify2,
    lse,
    dce2,
    // The codegen has a few assumptions that only the instruction simplifier
    // can satisfy. For example, the code generator does not expect to see a
    // HTypeConversion from a type to the same type.
    simplify3,
  };
  RunOptimizations(optimizations2, arraysize(optimizations2), pass_observer);

  RunArchOptimizations(driver->GetInstructionSet(), graph, codegen, stats, pass_observer);
  AllocateRegisters(graph, codegen, pass_observer);
}

static ArenaVector<LinkerPatch> EmitAndSortLinkerPatches(CodeGenerator* codegen) {
  ArenaVector<LinkerPatch> linker_patches(codegen->GetGraph()->GetArena()->Adapter());
  codegen->EmitLinkerPatches(&linker_patches);

  // Sort patches by literal offset. Required for .oat_patches encoding.
  std::sort(linker_patches.begin(), linker_patches.end(),
            [](const LinkerPatch& lhs, const LinkerPatch& rhs) {
    return lhs.LiteralOffset() < rhs.LiteralOffset();
  });

  return linker_patches;
}

CompiledMethod* OptimizingCompiler::Emit(ArenaAllocator* arena,
                                         CodeVectorAllocator* code_allocator,
                                         CodeGenerator* codegen,
                                         CompilerDriver* compiler_driver,
                                         const DexFile::CodeItem* code_item) const {
  ArenaVector<LinkerPatch> linker_patches = EmitAndSortLinkerPatches(codegen);
  ArenaVector<uint8_t> stack_map(arena->Adapter(kArenaAllocStackMaps));
  stack_map.resize(codegen->ComputeStackMapsSize());
  codegen->BuildStackMaps(MemoryRegion(stack_map.data(), stack_map.size()), *code_item);

  CompiledMethod* compiled_method = CompiledMethod::SwapAllocCompiledMethod(
      compiler_driver,
      codegen->GetInstructionSet(),
      ArrayRef<const uint8_t>(code_allocator->GetMemory()),
      // Follow Quick's behavior and set the frame size to zero if it is
      // considered "empty" (see the definition of
      // art::CodeGenerator::HasEmptyFrame).
      codegen->HasEmptyFrame() ? 0 : codegen->GetFrameSize(),
      codegen->GetCoreSpillMask(),
      codegen->GetFpuSpillMask(),
      ArrayRef<const SrcMapElem>(),
      ArrayRef<const uint8_t>(stack_map),
      ArrayRef<const uint8_t>(*codegen->GetAssembler()->cfi().data()),
      ArrayRef<const LinkerPatch>(linker_patches));

  return compiled_method;
}

CodeGenerator* OptimizingCompiler::TryCompile(ArenaAllocator* arena,
                                              CodeVectorAllocator* code_allocator,
                                              const DexFile::CodeItem* code_item,
                                              uint32_t access_flags,
                                              InvokeType invoke_type,
                                              uint16_t class_def_idx,
                                              uint32_t method_idx,
                                              jobject class_loader,
                                              const DexFile& dex_file,
                                              Handle<mirror::DexCache> dex_cache,
                                              ArtMethod* method,
                                              bool osr) const {
  MaybeRecordStat(MethodCompilationStat::kAttemptCompilation);
  CompilerDriver* compiler_driver = GetCompilerDriver();
  InstructionSet instruction_set = compiler_driver->GetInstructionSet();

  // Always use the Thumb-2 assembler: some runtime functionality
  // (like implicit stack overflow checks) assume Thumb-2.
  if (instruction_set == kArm) {
    instruction_set = kThumb2;
  }

  // Do not attempt to compile on architectures we do not support.
  if (!IsInstructionSetSupported(instruction_set)) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledUnsupportedIsa);
    return nullptr;
  }

  // When read barriers are enabled, do not attempt to compile for
  // instruction sets that have no read barrier support.
  if (kEmitCompilerReadBarrier && !InstructionSetSupportsReadBarrier(instruction_set)) {
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
  if ((compiler_options.GetCompilerFilter() == CompilerFilter::kSpace)
      && (code_item->insns_size_in_code_units_ > kSpaceFilterOptimizingThreshold)) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledSpaceFilter);
    return nullptr;
  }

  DexCompilationUnit dex_compilation_unit(
      class_loader,
      Runtime::Current()->GetClassLinker(),
      dex_file,
      code_item,
      class_def_idx,
      method_idx,
      access_flags,
      /* verified_method */ nullptr,
      dex_cache);

  bool requires_barrier = dex_compilation_unit.IsConstructor()
      && compiler_driver->RequiresConstructorBarrier(Thread::Current(),
                                                     dex_compilation_unit.GetDexFile(),
                                                     dex_compilation_unit.GetClassDefIndex());

  HGraph* graph = new (arena) HGraph(
      arena,
      dex_file,
      method_idx,
      requires_barrier,
      compiler_driver->GetInstructionSet(),
      kInvalidInvokeType,
      compiler_driver->GetCompilerOptions().GetDebuggable(),
      osr);

  const uint8_t* interpreter_metadata = nullptr;
  if (method == nullptr) {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::ClassLoader> loader(hs.NewHandle(
        soa.Decode<mirror::ClassLoader*>(class_loader)));
    method = compiler_driver->ResolveMethod(
        soa, dex_cache, loader, &dex_compilation_unit, method_idx, invoke_type);
  }
  // For AOT compilation, we may not get a method, for example if its class is erroneous.
  // JIT should always have a method.
  DCHECK(Runtime::Current()->IsAotCompiler() || method != nullptr);
  if (method != nullptr) {
    graph->SetArtMethod(method);
    ScopedObjectAccess soa(Thread::Current());
    interpreter_metadata = method->GetQuickenedInfo();
    uint16_t type_index = method->GetDeclaringClass()->GetDexTypeIndex();

    // Update the dex cache if the type is not in it yet. Note that under AOT,
    // the verifier must have set it, but under JIT, there's no guarantee, as we
    // don't necessarily run the verifier.
    // The compiler and the compiler driver assume the compiling class is
    // in the dex cache.
    if (dex_cache->GetResolvedType(type_index) == nullptr) {
      dex_cache->SetResolvedType(type_index, method->GetDeclaringClass());
    }
  }

  std::unique_ptr<CodeGenerator> codegen(
      CodeGenerator::Create(graph,
                            instruction_set,
                            *compiler_driver->GetInstructionSetFeatures(),
                            compiler_driver->GetCompilerOptions(),
                            compilation_stats_.get()));
  if (codegen.get() == nullptr) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledNoCodegen);
    return nullptr;
  }
  codegen->GetAssembler()->cfi().SetEnabled(
      compiler_driver->GetCompilerOptions().GenerateAnyDebugInfo());

  PassObserver pass_observer(graph,
                             codegen.get(),
                             visualizer_output_.get(),
                             compiler_driver);

  VLOG(compiler) << "Building " << pass_observer.GetMethodName();

  {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScopeCollection handles(soa.Self());
    // Do not hold `mutator_lock_` between optimizations.
    ScopedThreadSuspension sts(soa.Self(), kNative);

    {
      PassScope scope(HGraphBuilder::kBuilderPassName, &pass_observer);
      HGraphBuilder builder(graph,
                            &dex_compilation_unit,
                            &dex_compilation_unit,
                            &dex_file,
                            *code_item,
                            compiler_driver,
                            compilation_stats_.get(),
                            interpreter_metadata,
                            dex_cache,
                            &handles);
      GraphAnalysisResult result = builder.BuildGraph();
      if (result != kAnalysisSuccess) {
        switch (result) {
          case kAnalysisSkipped:
            MaybeRecordStat(MethodCompilationStat::kNotCompiledSkipped);
            break;
          case kAnalysisInvalidBytecode:
            MaybeRecordStat(MethodCompilationStat::kNotCompiledInvalidBytecode);
            break;
          case kAnalysisFailThrowCatchLoop:
            MaybeRecordStat(MethodCompilationStat::kNotCompiledThrowCatchLoop);
            break;
          case kAnalysisFailAmbiguousArrayOp:
            MaybeRecordStat(MethodCompilationStat::kNotCompiledAmbiguousArrayOp);
            break;
          case kAnalysisSuccess:
            UNREACHABLE();
        }
        pass_observer.SetGraphInBadState();
        return nullptr;
      }
    }

    RunOptimizations(graph,
                     codegen.get(),
                     compiler_driver,
                     compilation_stats_.get(),
                     dex_compilation_unit,
                     &pass_observer,
                     &handles);

    codegen->Compile(code_allocator);
    pass_observer.DumpDisassembly();
  }

  return codegen.release();
}

CompiledMethod* OptimizingCompiler::Compile(const DexFile::CodeItem* code_item,
                                            uint32_t access_flags,
                                            InvokeType invoke_type,
                                            uint16_t class_def_idx,
                                            uint32_t method_idx,
                                            jobject jclass_loader,
                                            const DexFile& dex_file,
                                            Handle<mirror::DexCache> dex_cache) const {
  CompilerDriver* compiler_driver = GetCompilerDriver();
  CompiledMethod* method = nullptr;
  DCHECK(Runtime::Current()->IsAotCompiler());
  const VerifiedMethod* verified_method = compiler_driver->GetVerifiedMethod(&dex_file, method_idx);
  DCHECK(!verified_method->HasRuntimeThrow());
  if (compiler_driver->IsMethodVerifiedWithoutFailures(method_idx, class_def_idx, dex_file)
      || verifier::MethodVerifier::CanCompilerHandleVerificationFailure(
            verified_method->GetEncounteredVerificationFailures())) {
    ArenaAllocator arena(Runtime::Current()->GetArenaPool());
    CodeVectorAllocator code_allocator(&arena);
    std::unique_ptr<CodeGenerator> codegen(
        TryCompile(&arena,
                   &code_allocator,
                   code_item,
                   access_flags,
                   invoke_type,
                   class_def_idx,
                   method_idx,
                   jclass_loader,
                   dex_file,
                   dex_cache,
                   nullptr,
                   /* osr */ false));
    if (codegen.get() != nullptr) {
      MaybeRecordStat(MethodCompilationStat::kCompiled);
      method = Emit(&arena, &code_allocator, codegen.get(), compiler_driver, code_item);

      if (kArenaAllocatorCountAllocations) {
        if (arena.BytesAllocated() > kArenaAllocatorMemoryReportThreshold) {
          MemStats mem_stats(arena.GetMemStats());
          LOG(INFO) << PrettyMethod(method_idx, dex_file) << " " << Dumpable<MemStats>(mem_stats);
        }
      }
    }
  } else {
    if (compiler_driver->GetCompilerOptions().VerifyAtRuntime()) {
      MaybeRecordStat(MethodCompilationStat::kNotCompiledVerifyAtRuntime);
    } else {
      MaybeRecordStat(MethodCompilationStat::kNotCompiledVerificationError);
    }
  }

  if (kIsDebugBuild &&
      IsCompilingWithCoreImage() &&
      IsInstructionSetSupported(compiler_driver->GetInstructionSet()) &&
      (!kEmitCompilerReadBarrier ||
       InstructionSetSupportsReadBarrier(compiler_driver->GetInstructionSet()))) {
    // For testing purposes, we put a special marker on method names
    // that should be compiled with this compiler (when the the
    // instruction set is supported -- and has support for read
    // barriers, if they are enabled). This makes sure we're not
    // regressing.
    std::string method_name = PrettyMethod(method_idx, dex_file);
    bool shouldCompile = method_name.find("$opt$") != std::string::npos;
    DCHECK((method != nullptr) || !shouldCompile) << "Didn't compile " << method_name;
  }

  return method;
}

Compiler* CreateOptimizingCompiler(CompilerDriver* driver) {
  return new OptimizingCompiler(driver);
}

bool IsCompilingWithCoreImage() {
  const std::string& image = Runtime::Current()->GetImageLocation();
  // TODO: This is under-approximating...
  if (EndsWith(image, "core.art") || EndsWith(image, "core-optimizing.art")) {
    return true;
  }
  return false;
}

bool OptimizingCompiler::JitCompile(Thread* self,
                                    jit::JitCodeCache* code_cache,
                                    ArtMethod* method,
                                    bool osr) {
  StackHandleScope<2> hs(self);
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      method->GetDeclaringClass()->GetClassLoader()));
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(method->GetDexCache()));
  DCHECK(method->IsCompilable());

  jobject jclass_loader = class_loader.ToJObject();
  const DexFile* dex_file = method->GetDexFile();
  const uint16_t class_def_idx = method->GetClassDefIndex();
  const DexFile::CodeItem* code_item = dex_file->GetCodeItem(method->GetCodeItemOffset());
  const uint32_t method_idx = method->GetDexMethodIndex();
  const uint32_t access_flags = method->GetAccessFlags();
  const InvokeType invoke_type = method->GetInvokeType();

  ArenaAllocator arena(Runtime::Current()->GetJitArenaPool());
  CodeVectorAllocator code_allocator(&arena);
  std::unique_ptr<CodeGenerator> codegen;
  {
    // Go to native so that we don't block GC during compilation.
    ScopedThreadSuspension sts(self, kNative);
    codegen.reset(
        TryCompile(&arena,
                   &code_allocator,
                   code_item,
                   access_flags,
                   invoke_type,
                   class_def_idx,
                   method_idx,
                   jclass_loader,
                   *dex_file,
                   dex_cache,
                   method,
                   osr));
    if (codegen.get() == nullptr) {
      return false;
    }

    if (kArenaAllocatorCountAllocations) {
      if (arena.BytesAllocated() > kArenaAllocatorMemoryReportThreshold) {
        MemStats mem_stats(arena.GetMemStats());
        LOG(INFO) << PrettyMethod(method_idx, *dex_file) << " " << Dumpable<MemStats>(mem_stats);
      }
    }
  }

  size_t stack_map_size = codegen->ComputeStackMapsSize();
  uint8_t* stack_map_data = code_cache->ReserveData(self, stack_map_size, method);
  if (stack_map_data == nullptr) {
    return false;
  }
  MaybeRecordStat(MethodCompilationStat::kCompiled);
  codegen->BuildStackMaps(MemoryRegion(stack_map_data, stack_map_size), *code_item);
  const void* code = code_cache->CommitCode(
      self,
      method,
      stack_map_data,
      codegen->HasEmptyFrame() ? 0 : codegen->GetFrameSize(),
      codegen->GetCoreSpillMask(),
      codegen->GetFpuSpillMask(),
      code_allocator.GetMemory().data(),
      code_allocator.GetSize(),
      osr);

  if (code == nullptr) {
    code_cache->ClearData(self, stack_map_data);
    return false;
  }

  const CompilerOptions& compiler_options = GetCompilerDriver()->GetCompilerOptions();
  if (compiler_options.GetGenerateDebugInfo()) {
    const auto* method_header = reinterpret_cast<const OatQuickMethodHeader*>(code);
    const uintptr_t code_address = reinterpret_cast<uintptr_t>(method_header->GetCode());
    debug::MethodDebugInfo info = debug::MethodDebugInfo();
    info.trampoline_name = nullptr;
    info.dex_file = dex_file;
    info.class_def_index = class_def_idx;
    info.dex_method_index = method_idx;
    info.access_flags = access_flags;
    info.code_item = code_item;
    info.isa = codegen->GetInstructionSet();
    info.deduped = false;
    info.is_native_debuggable = compiler_options.GetNativeDebuggable();
    info.is_optimized = true;
    info.is_code_address_text_relative = false;
    info.code_address = code_address;
    info.code_size = code_allocator.GetSize();
    info.frame_size_in_bytes = method_header->GetFrameSizeInBytes();
    info.code_info = stack_map_size == 0 ? nullptr : stack_map_data;
    info.cfi = ArrayRef<const uint8_t>(*codegen->GetAssembler()->cfi().data());
    std::vector<uint8_t> elf_file = debug::WriteDebugElfFileForMethods(
        GetCompilerDriver()->GetInstructionSet(),
        GetCompilerDriver()->GetInstructionSetFeatures(),
        ArrayRef<const debug::MethodDebugInfo>(&info, 1));
    CreateJITCodeEntryForAddress(code_address, std::move(elf_file));
  }

  Runtime::Current()->GetJit()->AddMemoryUsage(method, arena.BytesUsed());

  return true;
}

}  // namespace art
