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

#include "compiler.h"
#include "ir_builder.h"
#include "logging.h"
#include "object.h"
#include "object_utils.h"
#include "stl_util.h"

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
  // UNIMPLEMENTED(WARNING);
  return new CompiledMethod(insn_set_, NULL);
}


} // namespace compiler_llvm
} // namespace art
