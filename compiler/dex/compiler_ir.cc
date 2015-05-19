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

#include "compiler_ir.h"

#include "arch/instruction_set_features.h"
#include "base/dumpable.h"
#include "dex_flags.h"
#include "dex/quick/mir_to_lir.h"
#include "driver/compiler_driver.h"
#include "mir_graph.h"
#include "utils.h"

namespace art {

CompilationUnit::CompilationUnit(ArenaPool* pool, InstructionSet isa, CompilerDriver* driver,
                                 ClassLinker* linker)
  : compiler_driver(driver),
    class_linker(linker),
    dex_file(nullptr),
    class_loader(nullptr),
    class_def_idx(0),
    method_idx(0),
    access_flags(0),
    invoke_type(kDirect),
    shorty(nullptr),
    disable_opt(0),
    enable_debug(0),
    verbose(false),
    instruction_set(isa),
    target64(Is64BitInstructionSet(isa)),
    arena(pool),
    arena_stack(pool),
    mir_graph(nullptr),
    cg(nullptr),
    timings("QuickCompiler", true, false),
    print_pass(false) {
}

CompilationUnit::~CompilationUnit() {
  overridden_pass_options.clear();
}

void CompilationUnit::StartTimingSplit(const char* label) {
  if (compiler_driver->GetDumpPasses()) {
    timings.StartTiming(label);
  }
}

void CompilationUnit::NewTimingSplit(const char* label) {
  if (compiler_driver->GetDumpPasses()) {
    timings.EndTiming();
    timings.StartTiming(label);
  }
}

void CompilationUnit::EndTiming() {
  if (compiler_driver->GetDumpPasses()) {
    timings.EndTiming();
    if (enable_debug & (1 << kDebugTimings)) {
      LOG(INFO) << "TIMINGS " << PrettyMethod(method_idx, *dex_file);
      LOG(INFO) << Dumpable<TimingLogger>(timings);
    }
  }
}

}  // namespace art
