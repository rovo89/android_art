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

#include "runtime_support_builder_x86.h"

#include "ir_builder.h"
#include "stringprintf.h"
#include "thread.h"
#include "utils_llvm.h"

#include <llvm/DerivedTypes.h>
#include <llvm/Function.h>
#include <llvm/InlineAsm.h>
#include <llvm/Module.h>
#include <llvm/Type.h>

#include <vector>

using namespace llvm;

namespace art {
namespace compiler_llvm {


llvm::Value* RuntimeSupportBuilderX86::EmitGetCurrentThread() {
  Function* ori_func = GetRuntimeSupportFunction(runtime_support::GetCurrentThread);
  std::string inline_asm(StringPrintf("movl %%fs:%d, $0", Thread::SelfOffset().Int32Value()));
  InlineAsm* func = InlineAsm::get(ori_func->getFunctionType(), inline_asm, "=r", false);
  CallInst* thread = irb_.CreateCall(func);
  thread->setDoesNotAccessMemory();
  irb_.SetTBAA(thread, kTBAAConstJObject);
  return thread;
}

llvm::Value* RuntimeSupportBuilderX86::EmitLoadFromThreadOffset(int64_t offset, llvm::Type* type,
                                                                TBAASpecialType s_ty) {
  FunctionType* func_ty = FunctionType::get(/*Result=*/type,
                                            /*isVarArg=*/false);
  std::string inline_asm(StringPrintf("movl %%fs:%d, $0", static_cast<int>(offset)));
  InlineAsm* func = InlineAsm::get(func_ty, inline_asm, "=r", true);
  CallInst* result = irb_.CreateCall(func);
  result->setOnlyReadsMemory();
  irb_.SetTBAA(result, s_ty);
  return result;
}

void RuntimeSupportBuilderX86::EmitStoreToThreadOffset(int64_t offset, llvm::Value* value,
                                                       TBAASpecialType s_ty) {
  FunctionType* func_ty = FunctionType::get(/*Result=*/Type::getVoidTy(context_),
                                            /*Params=*/value->getType(),
                                            /*isVarArg=*/false);
  std::string inline_asm(StringPrintf("movl $0, %%fs:%d", static_cast<int>(offset)));
  InlineAsm* func = InlineAsm::get(func_ty, inline_asm, "r", true);
  CallInst* call_inst = irb_.CreateCall(func, value);
  irb_.SetTBAA(call_inst, s_ty);
}

llvm::Value* RuntimeSupportBuilderX86::EmitSetCurrentThread(llvm::Value*) {
  /* Nothing to be done. */
  return llvm::UndefValue::get(irb_.getJObjectTy());
}


} // namespace compiler_llvm
} // namespace art
