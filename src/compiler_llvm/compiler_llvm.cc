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

#include "compiler_llvm.h"

#include "compilation_unit.h"
#include "compiler.h"
#include "ir_builder.h"
#include "jni_compiler.h"
#include "method_compiler.h"
#include "oat_compilation_unit.h"
#include "stl_util.h"
#include "upcall_compiler.h"

#include <llvm/LinkAllPasses.h>
#include <llvm/LinkAllVMCore.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Threading.h>

namespace llvm {
  extern bool TimePassesIsEnabled;
}

extern llvm::cl::opt<bool> EnableARMLongCalls;
// NOTE: Although EnableARMLongCalls is defined in llvm/lib/Target/ARM/
// ARMISelLowering.cpp, however, it is not in the llvm namespace.


namespace {

pthread_once_t llvm_initialized = PTHREAD_ONCE_INIT;

void InitializeLLVM() {
  // NOTE: Uncomment following line to show the time consumption of LLVM passes
  //llvm::TimePassesIsEnabled = true;

  // Initialize LLVM target, MC subsystem, asm printer, and asm parser
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();
  // TODO: Maybe we don't have to initialize "all" targets.

  // Enable -arm-long-calls
  EnableARMLongCalls = true;

  // Initialize LLVM optimization passes
  llvm::PassRegistry &registry = *llvm::PassRegistry::getPassRegistry();

  llvm::initializeCore(registry);
  llvm::initializeScalarOpts(registry);
  llvm::initializeIPO(registry);
  llvm::initializeAnalysis(registry);
  llvm::initializeIPA(registry);
  llvm::initializeTransformUtils(registry);
  llvm::initializeInstCombine(registry);
  llvm::initializeInstrumentation(registry);
  llvm::initializeTarget(registry);

  // Initialize LLVM internal data structure for multithreading
  llvm::llvm_start_multithreaded();
}

} // anonymous namespace


namespace art {
namespace compiler_llvm {


llvm::Module* makeLLVMModuleContents(llvm::Module* module);


CompilerLLVM::CompilerLLVM(Compiler* compiler, InstructionSet insn_set)
    : compiler_(compiler), compiler_lock_("llvm_compiler_lock"),
      insn_set_(insn_set), curr_cunit_(NULL) {

  // Initialize LLVM libraries
  pthread_once(&llvm_initialized, InitializeLLVM);
}


CompilerLLVM::~CompilerLLVM() {
  STLDeleteElements(&cunits_);
  llvm::llvm_shutdown();
}


void CompilerLLVM::EnsureCompilationUnit() {
  DCHECK_NE(llvm_initialized, PTHREAD_ONCE_INIT);
  compiler_lock_.AssertHeld();

  if (curr_cunit_ != NULL) {
    return;
  }

  // Allocate compilation unit
  size_t cunit_idx = cunits_.size();

  curr_cunit_ = new CompilationUnit(insn_set_);

  // Setup output filename
  curr_cunit_->SetElfFileName(
    StringPrintf("%s-%zu", elf_filename_.c_str(), cunit_idx));

  if (IsBitcodeFileNameAvailable()) {
    curr_cunit_->SetBitcodeFileName(
      StringPrintf("%s-%zu", bitcode_filename_.c_str(), cunit_idx));
  }

  // Register compilation unit
  cunits_.push_back(curr_cunit_);
}


void CompilerLLVM::MaterializeRemainder() {
  MutexLock GUARD(compiler_lock_);
  if (curr_cunit_ != NULL) {
    Materialize();
  }
}


void CompilerLLVM::MaterializeIfThresholdReached() {
  MutexLock GUARD(compiler_lock_);
  if (curr_cunit_ != NULL && curr_cunit_->IsMaterializeThresholdReached()) {
    Materialize();
  }
}


void CompilerLLVM::Materialize() {
  compiler_lock_.AssertHeld();

  DCHECK(curr_cunit_ != NULL);
  DCHECK(!curr_cunit_->IsMaterialized());

  // Write bitcode to file when filename is set
  if (IsBitcodeFileNameAvailable()) {
    curr_cunit_->WriteBitcodeToFile();
  }

  // Materialize the llvm::Module into ELF object file
  curr_cunit_->Materialize();

  // Delete the compilation unit
  curr_cunit_ = NULL;
}


CompiledMethod* CompilerLLVM::
CompileDexMethod(OatCompilationUnit* oat_compilation_unit) {
  MutexLock GUARD(compiler_lock_);

  EnsureCompilationUnit();

  UniquePtr<MethodCompiler> method_compiler(
      new MethodCompiler(curr_cunit_, compiler_, oat_compilation_unit));

  return method_compiler->Compile();
}


CompiledMethod* CompilerLLVM::
CompileNativeMethod(OatCompilationUnit* oat_compilation_unit) {
  MutexLock GUARD(compiler_lock_);

  EnsureCompilationUnit();

  UniquePtr<JniCompiler> jni_compiler(
      new JniCompiler(curr_cunit_, *compiler_, oat_compilation_unit));

  return jni_compiler->Compile();
}


CompiledInvokeStub* CompilerLLVM::CreateInvokeStub(bool is_static,
                                                   char const *shorty) {
  MutexLock GUARD(compiler_lock_);

  EnsureCompilationUnit();

  UniquePtr<UpcallCompiler> upcall_compiler(
    new UpcallCompiler(curr_cunit_, *compiler_));

  return upcall_compiler->CreateStub(is_static, shorty);
}


} // namespace compiler_llvm
} // namespace art
