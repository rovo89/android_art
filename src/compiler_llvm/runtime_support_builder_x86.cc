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
#include "thread.h"

#include <llvm/DerivedTypes.h>
#include <llvm/Function.h>
#include <llvm/InlineAsm.h>
#include <llvm/Module.h>
#include <llvm/Type.h>

#include <vector>

using namespace llvm;

namespace art {
namespace compiler_llvm {

using namespace runtime_support;


void RuntimeSupportBuilderX86::TargetOptimizeRuntimeSupport() {
  {
    Function* func = GetRuntimeSupportFunction(GetCurrentThread);
    MakeFunctionInline(func);
    BasicBlock* basic_block = BasicBlock::Create(context_, "entry", func);
    irb_.SetInsertPoint(basic_block);

    std::vector<Type*> func_ty_args;
    func_ty_args.push_back(irb_.getPtrEquivIntTy());
    FunctionType* func_ty = FunctionType::get(/*Result=*/irb_.getJObjectTy(),
                                              /*Params=*/func_ty_args,
                                              /*isVarArg=*/false);
    InlineAsm* get_fp = InlineAsm::get(func_ty, "movl %fs:($1), $0", "=r,r", false);
    Value* fp = irb_.CreateCall(get_fp, irb_.getPtrEquivInt(Thread::SelfOffset().Int32Value()));
    irb_.CreateRet(fp);
  }

  {
    Function* func = GetRuntimeSupportFunction(SetCurrentThread);
    MakeFunctionInline(func);
    BasicBlock* basic_block = BasicBlock::Create(context_, "entry", func);
    irb_.SetInsertPoint(basic_block);
    irb_.CreateRetVoid();
  }
}


} // namespace compiler_llvm
} // namespace art
