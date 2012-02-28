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

#include "compiler.h"
#include "ir_builder.h"
#include "method_compiler.h"
#include "oat_compilation_unit.h"
#include "upcall_compiler.h"

#include <llvm/ADT/OwningPtr.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/DerivedTypes.h>
#include <llvm/LLVMContext.h>
#include <llvm/Module.h>
#include <llvm/Support/ToolOutputFile.h>

namespace art {
namespace compiler_llvm {


llvm::Module* makeLLVMModuleContents(llvm::Module* module);


CompilerLLVM::CompilerLLVM(Compiler* compiler, InstructionSet insn_set)
: compiler_(compiler), compiler_lock_("llvm_compiler_lock"),
  insn_set_(insn_set), context_(new llvm::LLVMContext()) {

  // Create the module and include the runtime function declaration
  module_ = new llvm::Module("art", *context_);
  makeLLVMModuleContents(module_);

  // Create IRBuilder
  irb_.reset(new IRBuilder(*context_, *module_));
}


CompilerLLVM::~CompilerLLVM() {
}


void CompilerLLVM::MaterializeLLVMModule() {
#if !defined(NDEBUG)
  // TODO: Add options to JNI_CreateJavaVM() and dex2oat, so that we don't
  // have to hard-code the path.
  WriteBitcodeToFile("/tmp/art_llvm_module.bc");
#endif
}


void CompilerLLVM::WriteBitcodeToFile(std::string const &filepath) {
  std::string error_msg;

  // Write the translated bitcode
  llvm::OwningPtr<llvm::tool_output_file>
    out(new llvm::tool_output_file(filepath.c_str(), error_msg,
                                   llvm::raw_fd_ostream::F_Binary));

  if (!error_msg.empty()) {
    LOG(FATAL) << "Unable to open file: " << error_msg;
    return;
  }

  llvm::WriteBitcodeToFile(module_, out->os());
  out->keep();

  LOG(DEBUG) << "Bitcode Written At: " << filepath;
}


CompiledMethod* CompilerLLVM::CompileDexMethod(OatCompilationUnit* oat_compilation_unit) {
  MutexLock GUARD(compiler_lock_);

  UniquePtr<MethodCompiler> method_compiler(
    new MethodCompiler(insn_set_, compiler_, oat_compilation_unit));

  return method_compiler->Compile();
}


CompiledInvokeStub* CompilerLLVM::CreateInvokeStub(bool is_static,
                                                   char const *shorty) {

  MutexLock GUARD(compiler_lock_);

  UniquePtr<UpcallCompiler> upcall_compiler(
    new UpcallCompiler(insn_set_, *compiler_));

  return upcall_compiler->CreateStub(is_static, shorty);
}


} // namespace compiler_llvm
} // namespace art
