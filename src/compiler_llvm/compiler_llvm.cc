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

#include <llvm/ADT/OwningPtr.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/DerivedTypes.h>
#include <llvm/LLVMContext.h>
#include <llvm/Module.h>
#include <llvm/Support/ToolOutputFile.h>

namespace art {
namespace compiler_llvm {


CompilerLLVM::CompilerLLVM(Compiler* compiler, InstructionSet insn_set)
: compiler_(compiler), compiler_lock_("llvm_compiler_lock"),
  insn_set_(insn_set), context_(new llvm::LLVMContext()) {

  // Create the module and include the runtime function declaration
  module_ = new llvm::Module("art", *context_);

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


CompiledMethod*
CompilerLLVM::CompileDexMethod(DexFile::CodeItem const* code_item,
                               uint32_t access_flags,
                               uint32_t method_idx,
                               ClassLoader const* class_loader,
                               DexFile const& dex_file) {

  MutexLock GUARD(compiler_lock_);

  ClassLinker *class_linker = Runtime::Current()->GetClassLinker();
  DexCache *dex_cache = class_linker->FindDexCache(dex_file);

  UniquePtr<MethodCompiler> method_compiler(
    new MethodCompiler(insn_set_, compiler_, class_linker, class_loader,
                       &dex_file, dex_cache, code_item, method_idx,
                       access_flags));

  return method_compiler->Compile();
}


} // namespace compiler_llvm
} // namespace art
