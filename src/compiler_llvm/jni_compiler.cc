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

#include "jni_compiler.h"

#include "class_linker.h"
#include "compiled_method.h"
#include "compiler.h"
#include "compiler_llvm.h"
#include "ir_builder.h"
#include "logging.h"
#include "oat_compilation_unit.h"
#include "object.h"
#include "runtime.h"
#include "utils_llvm.h"

#include <llvm/DerivedTypes.h>
#include <llvm/Function.h>
#include <llvm/Type.h>

namespace art {
namespace compiler_llvm {


JniCompiler::JniCompiler(InstructionSet insn_set,
                         Compiler const& compiler,
                         OatCompilationUnit* oat_compilation_unit)
: insn_set_(insn_set), compiler_(&compiler),
  compiler_llvm_(compiler_->GetCompilerLLVM()),
  module_(compiler_llvm_->GetModule()),
  context_(compiler_llvm_->GetLLVMContext()),
  irb_(*compiler_llvm_->GetIRBuilder()),
  oat_compilation_unit_(oat_compilation_unit),
  access_flags_(oat_compilation_unit->access_flags_),
  method_idx_(oat_compilation_unit->method_idx_),
  class_linker_(oat_compilation_unit->class_linker_),
  class_loader_(oat_compilation_unit->class_loader_),
  dex_cache_(oat_compilation_unit->dex_cache_),
  dex_file_(oat_compilation_unit->dex_file_),
  method_(dex_cache_->GetResolvedMethod(method_idx_)) {

  // Check: Ensure that the method is resolved
  CHECK_NE(method_, static_cast<art::Method*>(NULL));

  // Check: Ensure that JNI compiler will only get "native" method
  CHECK((access_flags_ & kAccNative) != 0);
}


CompiledMethod* JniCompiler::Compile() {
  CreateFunction();

  return new CompiledMethod(insn_set_, func_);
}


void JniCompiler::CreateFunction() {
  // LLVM function name
  std::string func_name(LLVMLongName(method_));

  // Get function type
  llvm::FunctionType* func_type =
    GetFunctionType(method_idx_, method_->IsStatic());

  // Create function
  func_ = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage,
                                 func_name, module_);
}


llvm::FunctionType* JniCompiler::GetFunctionType(uint32_t method_idx,
                                                 bool is_static) {
  // Get method signature
  DexFile::MethodId const& method_id = dex_file_->GetMethodId(method_idx);

  uint32_t shorty_size;
  char const* shorty = dex_file_->GetMethodShorty(method_id, &shorty_size);
  CHECK_GE(shorty_size, 1u);

  // Get return type
  llvm::Type* ret_type = irb_.getJType(shorty[0], kAccurate);

  // Get argument type
  std::vector<llvm::Type*> args_type;

  args_type.push_back(irb_.getJObjectTy()); // method object pointer

  if (!is_static) {
    args_type.push_back(irb_.getJType('L', kAccurate)); // "this" object pointer
  }

  for (uint32_t i = 1; i < shorty_size; ++i) {
    args_type.push_back(irb_.getJType(shorty[i], kAccurate));
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}


} // namespace compiler_llvm
} // namespace art
