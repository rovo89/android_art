/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "frontend.h"

#include <cstdint>

#include "backend.h"
#include "compiler.h"
#include "compiler_internals.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "mirror/object.h"
#include "pass_driver_me_opts.h"
#include "runtime.h"
#include "base/logging.h"
#include "base/timing_logger.h"
#include "driver/compiler_options.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"

namespace art {

/* Default optimizer/debug setting for the compiler. */
static uint32_t kCompilerOptimizerDisableFlags = 0 |  // Disable specific optimizations
  // (1 << kLoadStoreElimination) |
  // (1 << kLoadHoisting) |
  // (1 << kSuppressLoads) |
  // (1 << kNullCheckElimination) |
  // (1 << kClassInitCheckElimination) |
  // (1 << kGlobalValueNumbering) |
  // (1 << kLocalValueNumbering) |
  // (1 << kPromoteRegs) |
  // (1 << kTrackLiveTemps) |
  // (1 << kSafeOptimizations) |
  // (1 << kBBOpt) |
  // (1 << kMatch) |
  // (1 << kPromoteCompilerTemps) |
  // (1 << kSuppressExceptionEdges) |
  // (1 << kSuppressMethodInlining) |
  0;

static uint32_t kCompilerDebugFlags = 0 |     // Enable debug/testing modes
  // (1 << kDebugDisplayMissingTargets) |
  // (1 << kDebugVerbose) |
  // (1 << kDebugDumpCFG) |
  // (1 << kDebugSlowFieldPath) |
  // (1 << kDebugSlowInvokePath) |
  // (1 << kDebugSlowStringPath) |
  // (1 << kDebugSlowestFieldPath) |
  // (1 << kDebugSlowestStringPath) |
  // (1 << kDebugExerciseResolveMethod) |
  // (1 << kDebugVerifyDataflow) |
  // (1 << kDebugShowMemoryUsage) |
  // (1 << kDebugShowNops) |
  // (1 << kDebugCountOpcodes) |
  // (1 << kDebugDumpCheckStats) |
  // (1 << kDebugDumpBitcodeFile) |
  // (1 << kDebugVerifyBitcode) |
  // (1 << kDebugShowSummaryMemoryUsage) |
  // (1 << kDebugShowFilterStats) |
  // (1 << kDebugTimings) |
  // (1 << kDebugCodegenDump) |
  0;

static CompiledMethod* CompileMethod(CompilerDriver& driver,
                                     const Compiler* compiler,
                                     const DexFile::CodeItem* code_item,
                                     uint32_t access_flags, InvokeType invoke_type,
                                     uint16_t class_def_idx, uint32_t method_idx,
                                     jobject class_loader, const DexFile& dex_file,
                                     void* llvm_compilation_unit) {
  VLOG(compiler) << "Compiling " << PrettyMethod(method_idx, dex_file) << "...";
  /*
   * Skip compilation for pathologically large methods - either by instruction count or num vregs.
   * Dalvik uses 16-bit uints for instruction and register counts.  We'll limit to a quarter
   * of that, which also guarantees we cannot overflow our 16-bit internal SSA name space.
   */
  if (code_item->insns_size_in_code_units_ >= UINT16_MAX / 4) {
    LOG(INFO) << "Method exceeds compiler instruction limit: "
              << code_item->insns_size_in_code_units_
              << " in " << PrettyMethod(method_idx, dex_file);
    return NULL;
  }
  if (code_item->registers_size_ >= UINT16_MAX / 4) {
    LOG(INFO) << "Method exceeds compiler virtual register limit: "
              << code_item->registers_size_ << " in " << PrettyMethod(method_idx, dex_file);
    return NULL;
  }

  if (!driver.GetCompilerOptions().IsCompilationEnabled()) {
    return nullptr;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  CompilationUnit cu(driver.GetArenaPool());

  cu.compiler_driver = &driver;
  cu.class_linker = class_linker;
  cu.instruction_set = driver.GetInstructionSet();
  if (cu.instruction_set == kArm) {
    cu.instruction_set = kThumb2;
  }
  cu.target64 = Is64BitInstructionSet(cu.instruction_set);
  cu.compiler = compiler;
  // TODO: Mips64 is not yet implemented.
  CHECK((cu.instruction_set == kThumb2) ||
        (cu.instruction_set == kArm64) ||
        (cu.instruction_set == kX86) ||
        (cu.instruction_set == kX86_64) ||
        (cu.instruction_set == kMips));

  // TODO: set this from command line
  cu.compiler_flip_match = false;
  bool use_match = !cu.compiler_method_match.empty();
  bool match = use_match && (cu.compiler_flip_match ^
      (PrettyMethod(method_idx, dex_file).find(cu.compiler_method_match) != std::string::npos));
  if (!use_match || match) {
    cu.disable_opt = kCompilerOptimizerDisableFlags;
    cu.enable_debug = kCompilerDebugFlags;
    cu.verbose = VLOG_IS_ON(compiler) ||
        (cu.enable_debug & (1 << kDebugVerbose));
  }

  if (gVerboseMethods.size() != 0) {
    cu.verbose = false;
    for (size_t i = 0; i < gVerboseMethods.size(); ++i) {
      if (PrettyMethod(method_idx, dex_file).find(gVerboseMethods[i])
          != std::string::npos) {
        cu.verbose = true;
        break;
      }
    }
  }

  if (cu.verbose) {
    cu.enable_debug |= (1 << kDebugCodegenDump);
  }

  /*
   * TODO: rework handling of optimization and debug flags.  Should we split out
   * MIR and backend flags?  Need command-line setting as well.
   */

  compiler->InitCompilationUnit(cu);

  cu.StartTimingSplit("BuildMIRGraph");
  cu.mir_graph.reset(new MIRGraph(&cu, &cu.arena));

  /*
   * After creation of the MIR graph, also create the code generator.
   * The reason we do this is that optimizations on the MIR graph may need to get information
   * that is only available if a CG exists.
   */
  cu.cg.reset(compiler->GetCodeGenerator(&cu, llvm_compilation_unit));

  /* Gathering opcode stats? */
  if (kCompilerDebugFlags & (1 << kDebugCountOpcodes)) {
    cu.mir_graph->EnableOpcodeCounting();
  }

  /* Build the raw MIR graph */
  cu.mir_graph->InlineMethod(code_item, access_flags, invoke_type, class_def_idx, method_idx,
                              class_loader, dex_file);

  if (!compiler->CanCompileMethod(method_idx, dex_file, &cu)) {
    VLOG(compiler)  << cu.instruction_set << ": Cannot compile method : "
        << PrettyMethod(method_idx, dex_file);
    return nullptr;
  }

  cu.NewTimingSplit("MIROpt:CheckFilters");
  std::string skip_message;
  if (cu.mir_graph->SkipCompilation(&skip_message)) {
    VLOG(compiler) << cu.instruction_set << ": Skipping method : "
                   << PrettyMethod(method_idx, dex_file) << "  Reason = " << skip_message;
    return nullptr;
  }

  /* Create the pass driver and launch it */
  PassDriverMEOpts pass_driver(&cu);
  pass_driver.Launch();

  /* For non-leaf methods check if we should skip compilation when the profiler is enabled. */
  if (cu.compiler_driver->ProfilePresent()
      && !cu.mir_graph->MethodIsLeaf()
      && cu.mir_graph->SkipCompilationByName(PrettyMethod(method_idx, dex_file))) {
    return nullptr;
  }

  if (cu.enable_debug & (1 << kDebugDumpCheckStats)) {
    cu.mir_graph->DumpCheckStats();
  }

  if (kCompilerDebugFlags & (1 << kDebugCountOpcodes)) {
    cu.mir_graph->ShowOpcodeStats();
  }

  /* Reassociate sreg names with original Dalvik vreg names. */
  cu.mir_graph->RemapRegLocations();

  /* Free Arenas from the cu.arena_stack for reuse by the cu.arena in the codegen. */
  if (cu.enable_debug & (1 << kDebugShowMemoryUsage)) {
    if (cu.arena_stack.PeakBytesAllocated() > 1 * 1024 * 1024) {
      MemStats stack_stats(cu.arena_stack.GetPeakStats());
      LOG(INFO) << PrettyMethod(method_idx, dex_file) << " " << Dumpable<MemStats>(stack_stats);
    }
  }
  cu.arena_stack.Reset();

  CompiledMethod* result = NULL;

  if (cu.mir_graph->PuntToInterpreter()) {
    VLOG(compiler) << cu.instruction_set << ": Punted method to interpreter: "
        << PrettyMethod(method_idx, dex_file);
    return nullptr;
  }

  cu.cg->Materialize();

  cu.NewTimingSplit("Dedupe");  /* deduping takes up the vast majority of time in GetCompiledMethod(). */
  result = cu.cg->GetCompiledMethod();
  cu.NewTimingSplit("Cleanup");

  if (result) {
    VLOG(compiler) << cu.instruction_set << ": Compiled " << PrettyMethod(method_idx, dex_file);
  } else {
    VLOG(compiler) << cu.instruction_set << ": Deferred " << PrettyMethod(method_idx, dex_file);
  }

  if (cu.enable_debug & (1 << kDebugShowMemoryUsage)) {
    if (cu.arena.BytesAllocated() > (1 * 1024 *1024)) {
      MemStats mem_stats(cu.arena.GetMemStats());
      LOG(INFO) << PrettyMethod(method_idx, dex_file) << " " << Dumpable<MemStats>(mem_stats);
    }
  }

  if (cu.enable_debug & (1 << kDebugShowSummaryMemoryUsage)) {
    LOG(INFO) << "MEMINFO " << cu.arena.BytesAllocated() << " " << cu.mir_graph->GetNumBlocks()
              << " " << PrettyMethod(method_idx, dex_file);
  }

  cu.EndTiming();
  driver.GetTimingsLogger()->AddLogger(cu.timings);
  return result;
}

CompiledMethod* CompileOneMethod(CompilerDriver* driver,
                                 const Compiler* compiler,
                                 const DexFile::CodeItem* code_item,
                                 uint32_t access_flags,
                                 InvokeType invoke_type,
                                 uint16_t class_def_idx,
                                 uint32_t method_idx,
                                 jobject class_loader,
                                 const DexFile& dex_file,
                                 void* compilation_unit) {
  return CompileMethod(*driver, compiler, code_item, access_flags, invoke_type, class_def_idx,
                       method_idx, class_loader, dex_file, compilation_unit);
}

}  // namespace art
