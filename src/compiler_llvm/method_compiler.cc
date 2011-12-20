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

#include "method_compiler.h"

#include "backend_types.h"
#include "compiler.h"
#include "ir_builder.h"
#include "logging.h"
#include "object.h"
#include "object_utils.h"
#include "stl_util.h"
#include "stringprintf.h"
#include "utils_llvm.h"

#include <iomanip>

#include <llvm/Analysis/Verifier.h>
#include <llvm/Function.h>

namespace art {
namespace compiler_llvm {


MethodCompiler::MethodCompiler(InstructionSet insn_set,
                               Compiler* compiler,
                               ClassLinker* class_linker,
                               ClassLoader const* class_loader,
                               DexFile const* dex_file,
                               DexCache* dex_cache,
                               DexFile::CodeItem const* code_item,
                               uint32_t method_idx,
                               uint32_t access_flags)
: insn_set_(insn_set),
  compiler_(compiler), compiler_llvm_(compiler->GetCompilerLLVM()),
  class_linker_(class_linker), class_loader_(class_loader),
  dex_file_(dex_file), dex_cache_(dex_cache), code_item_(code_item),
  method_(dex_cache->GetResolvedMethod(method_idx)),
  method_helper_(method_), method_idx_(method_idx),
  access_flags_(access_flags), module_(compiler_llvm_->GetModule()),
  context_(compiler_llvm_->GetLLVMContext()),
  irb_(*compiler_llvm_->GetIRBuilder()), func_(NULL) {
}


MethodCompiler::~MethodCompiler() {
}


void MethodCompiler::CreateFunction() {
  // LLVM function name
  std::string func_name(LLVMLongName(method_));

  // Get function type
  llvm::FunctionType* func_type =
    GetFunctionType(method_idx_, method_->IsStatic());

  // Create function
  func_ = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage,
                                 func_name, module_);

  // Set argument name
  llvm::Function::arg_iterator arg_iter(func_->arg_begin());
  llvm::Function::arg_iterator arg_end(func_->arg_end());

  DCHECK_NE(arg_iter, arg_end);
  arg_iter->setName("method");
  ++arg_iter;

  if (!method_->IsStatic()) {
    DCHECK_NE(arg_iter, arg_end);
    arg_iter->setName("this");
    ++arg_iter;
  }

  for (unsigned i = 0; arg_iter != arg_end; ++i, ++arg_iter) {
    arg_iter->setName(StringPrintf("a%u", i));
  }
}


llvm::FunctionType* MethodCompiler::GetFunctionType(uint32_t method_idx,
                                                    bool is_static) {
  // Get method signature
  DexFile::MethodId const& method_id = dex_file_->GetMethodId(method_idx);

  int32_t shorty_size;
  char const* shorty = dex_file_->GetMethodShorty(method_id, &shorty_size);
  CHECK_GE(shorty_size, 1);

  // Get return type
  llvm::Type* ret_type = irb_.getJType(shorty[0], kAccurate);

  // Get argument type
  std::vector<llvm::Type*> args_type;

  args_type.push_back(irb_.getJObjectTy()); // method object pointer

  if (!is_static) {
    args_type.push_back(irb_.getJType('L', kAccurate)); // "this" object pointer
  }

  for (int32_t i = 1; i < shorty_size; ++i) {
    args_type.push_back(irb_.getJType(shorty[i], kAccurate));
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}


void MethodCompiler::EmitPrologue() {
  // UNIMPLEMENTED(WARNING);
}


void MethodCompiler::EmitInstructions() {
  // UNIMPLEMENTED(WARNING);
}


void MethodCompiler::EmitInstruction(uint32_t dex_pc,
                                     Instruction const* insn) {
  // UNIMPLEMENTED(WARNING);
}


CompiledMethod *MethodCompiler::Compile() {
  // Code generation
  CreateFunction();

  EmitPrologue();
  EmitInstructions();

  // Delete the inferred register category map (won't be used anymore)
  method_->ResetInferredRegCategoryMap();

  return new CompiledMethod(insn_set_, func_);
}


llvm::Value* MethodCompiler::EmitLoadMethodObjectAddr() {
  return func_->arg_begin();
}


} // namespace compiler_llvm
} // namespace art
