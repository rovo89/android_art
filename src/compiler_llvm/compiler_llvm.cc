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
#include "upcall_compiler.h"

#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Threading.h>


namespace {

pthread_once_t llvm_initialized = PTHREAD_ONCE_INIT;

void InitializeLLVM() {
  // Initialize LLVM target, MC subsystem, asm printer, and asm parser
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();
  // TODO: Maybe we don't have to initialize "all" targets.

  // Initialize LLVM internal data structure for multithreading
  llvm::llvm_start_multithreaded();
}

} // anonymous namespace


namespace art {
namespace compiler_llvm {


llvm::Module* makeLLVMModuleContents(llvm::Module* module);


CompilerLLVM::CompilerLLVM(Compiler* compiler, InstructionSet insn_set)
    : compiler_(compiler), compiler_lock_("llvm_compiler_lock"),
      insn_set_(insn_set), cunit_counter_(0) {

  // Initialize LLVM libraries
  pthread_once(&llvm_initialized, InitializeLLVM);
}


CompilerLLVM::~CompilerLLVM() {
  DCHECK(cunit_.get() == NULL);
}


void CompilerLLVM::EnsureCompilationUnit() {
  MutexLock GUARD(compiler_lock_);
  DCHECK_NE(llvm_initialized, PTHREAD_ONCE_INIT);
  if (cunit_.get() == NULL) {
    cunit_.reset(new CompilationUnit(insn_set_));
  }
}


void CompilerLLVM::MaterializeEveryCompilationUnit() {
  if (cunit_.get() != NULL) {
    MaterializeCompilationUnit();
  }
}


void CompilerLLVM::MaterializeCompilationUnitSafePoint() {
  if (cunit_->IsMaterializeThresholdReached()) {
    MaterializeCompilationUnit();
  }
}


void CompilerLLVM::MaterializeCompilationUnit() {
  MutexLock GUARD(compiler_lock_);

  cunit_->SetElfFileName(StringPrintf("%s-%u", elf_filename_.c_str(),
                                      cunit_counter_));

  // Write the translated bitcode for debugging
  if (!bitcode_filename_.empty()) {
    cunit_->SetBitcodeFileName(StringPrintf("%s-%u", bitcode_filename_.c_str(),
                                            cunit_counter_));
    cunit_->WriteBitcodeToFile();
  }

  // Materialize the llvm::Module into ELF object file
  cunit_->Materialize();

  // Increase compilation unit counter
  ++cunit_counter_;

  // Delete the compilation unit
  cunit_.reset(NULL);
}


CompiledMethod* CompilerLLVM::CompileDexMethod(OatCompilationUnit* oat_compilation_unit) {
  MutexLock GUARD(compiler_lock_);

  EnsureCompilationUnit();

  UniquePtr<MethodCompiler> method_compiler(
      new MethodCompiler(cunit_.get(), compiler_, oat_compilation_unit));

  CompiledMethod* result = method_compiler->Compile();

  MaterializeCompilationUnitSafePoint();

  return result;
}


CompiledMethod* CompilerLLVM::CompileNativeMethod(OatCompilationUnit* oat_compilation_unit) {
  MutexLock GUARD(compiler_lock_);

  EnsureCompilationUnit();

  UniquePtr<JniCompiler> jni_compiler(
      new JniCompiler(cunit_.get(), *compiler_, oat_compilation_unit));

  CompiledMethod* result = jni_compiler->Compile();

  MaterializeCompilationUnitSafePoint();

  return result;
}


CompiledInvokeStub* CompilerLLVM::CreateInvokeStub(bool is_static,
                                                   char const *shorty) {

  MutexLock GUARD(compiler_lock_);

  EnsureCompilationUnit();

  UniquePtr<UpcallCompiler> upcall_compiler(
    new UpcallCompiler(cunit_.get(), *compiler_));

  CompiledInvokeStub* result = upcall_compiler->CreateStub(is_static, shorty);

  MaterializeCompilationUnitSafePoint();

  return result;
}


} // namespace compiler_llvm
} // namespace art
