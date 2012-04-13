/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "runtime_support_builder.h"

#include "ir_builder.h"
#include "shadow_frame.h"
#include "thread.h"

#include <llvm/DerivedTypes.h>
#include <llvm/Function.h>
#include <llvm/Module.h>
#include <llvm/Type.h>

using namespace llvm;

namespace art {
namespace compiler_llvm {

using namespace runtime_support;


RuntimeSupportBuilder::RuntimeSupportBuilder(llvm::LLVMContext& context,
                                             llvm::Module& module,
                                             IRBuilder& irb)
    : context_(context), module_(module), irb_(irb)
{
#define GET_RUNTIME_SUPPORT_FUNC_DECL(ID, NAME) \
  do { \
    llvm::Function* fn = module_.getFunction(#NAME); \
    DCHECK_NE(fn, (void*)NULL) << "Function not found: " << #NAME; \
    runtime_support_func_decls_[ID] = fn; \
  } while (0);

#include "runtime_support_func_list.h"
  RUNTIME_SUPPORT_FUNC_LIST(GET_RUNTIME_SUPPORT_FUNC_DECL)
#undef RUNTIME_SUPPORT_FUNC_LIST
#undef GET_RUNTIME_SUPPORT_FUNC_DECL
}

void RuntimeSupportBuilder::MakeFunctionInline(llvm::Function* func) {
  func->setLinkage(GlobalValue::LinkOnceODRLinkage);

  SmallVector<AttributeWithIndex, 4> Attrs;
  AttributeWithIndex PAWI;
  PAWI.Index = ~0U;
  PAWI.Attrs = Attribute::None | Attribute::NoUnwind | Attribute::AlwaysInline;
  Attrs.push_back(PAWI);
  AttrListPtr func_PAL = AttrListPtr::get(Attrs.begin(), Attrs.end());

  func->setAttributes(func_PAL);
}

void RuntimeSupportBuilder::OverrideRuntimeSupportFunction(RuntimeId id, llvm::Function* function) {
  // TODO: Check function prototype.
  if (id >= 0 && id < MAX_ID) {
    runtime_support_func_decls_[id] = function;
    target_runtime_support_func_[id] = true;
  } else {
    LOG(ERROR) << "Unknown runtime function id: " << id;
  }
}

void RuntimeSupportBuilder::OptimizeRuntimeSupport() {
  TargetOptimizeRuntimeSupport();

  if (!target_runtime_support_func_[PushShadowFrame]) {
    Function* func = GetRuntimeSupportFunction(PushShadowFrame);
    MakeFunctionInline(func);
    BasicBlock* basic_block = BasicBlock::Create(context_, "entry", func);
    irb_.SetInsertPoint(basic_block);

    Function* get_thread = GetRuntimeSupportFunction(GetCurrentThread);
    Value* thread = irb_.CreateCall(get_thread);
    Value* new_shadow_frame = func->arg_begin();
    Value* old_shadow_frame = irb_.LoadFromObjectOffset(thread,
                                                        Thread::TopShadowFrameOffset().Int32Value(),
                                                        irb_.getJObjectTy());
    irb_.StoreToObjectOffset(new_shadow_frame,
                             ShadowFrame::LinkOffset(),
                             old_shadow_frame);
    irb_.StoreToObjectOffset(thread,
                             Thread::TopShadowFrameOffset().Int32Value(),
                             new_shadow_frame);
    irb_.CreateRetVoid();
  }

  if (!target_runtime_support_func_[PopShadowFrame]) {
    Function* func = GetRuntimeSupportFunction(PopShadowFrame);
    MakeFunctionInline(func);
    BasicBlock* basic_block = BasicBlock::Create(context_, "entry", func);
    irb_.SetInsertPoint(basic_block);

    Function* get_thread = GetRuntimeSupportFunction(GetCurrentThread);
    Value* thread = irb_.CreateCall(get_thread);
    Value* new_shadow_frame = irb_.LoadFromObjectOffset(thread,
                                                        Thread::TopShadowFrameOffset().Int32Value(),
                                                        irb_.getJObjectTy());
    Value* old_shadow_frame = irb_.LoadFromObjectOffset(new_shadow_frame,
                                                        ShadowFrame::LinkOffset(),
                                                        irb_.getJObjectTy());
    irb_.StoreToObjectOffset(thread,
                             Thread::TopShadowFrameOffset().Int32Value(),
                             old_shadow_frame);
    irb_.CreateRetVoid();
  }

  if (!target_runtime_support_func_[IsExceptionPending]) {
    Function* func = GetRuntimeSupportFunction(IsExceptionPending);
    MakeFunctionInline(func);
    BasicBlock* basic_block = BasicBlock::Create(context_, "entry", func);
    irb_.SetInsertPoint(basic_block);

    Function* get_thread = GetRuntimeSupportFunction(GetCurrentThread);
    Value* thread = irb_.CreateCall(get_thread);
    Value* exception = irb_.LoadFromObjectOffset(thread,
                                                 Thread::ExceptionOffset().Int32Value(),
                                                 irb_.getJObjectTy());
    Value* is_exception_not_null = irb_.CreateICmpNE(exception, irb_.getJNull());
    irb_.CreateRet(is_exception_not_null);
  }

  if (!target_runtime_support_func_[TestSuspend]) {
    Function* slow_func = GetRuntimeSupportFunction(TestSuspend);

    Function* func = Function::Create(slow_func->getFunctionType(),
                                      GlobalValue::LinkOnceODRLinkage,
                                      "test_suspend_fast",
                                      &module_);
    MakeFunctionInline(func);
    BasicBlock* basic_block = BasicBlock::Create(context_, "entry", func);
    irb_.SetInsertPoint(basic_block);

    Function* get_thread = GetRuntimeSupportFunction(GetCurrentThread);
    Value* thread = irb_.CreateCall(get_thread);
    Value* suspend_count = irb_.LoadFromObjectOffset(thread,
                                                     Thread::SuspendCountOffset().Int32Value(),
                                                     irb_.getJIntTy());
    Value* is_suspend = irb_.CreateICmpNE(suspend_count, irb_.getJInt(0));

    BasicBlock* basic_block_suspend = BasicBlock::Create(context_, "suspend", func);
    BasicBlock* basic_block_else = BasicBlock::Create(context_, "else", func);
    irb_.CreateCondBr(is_suspend, basic_block_suspend, basic_block_else);
    irb_.SetInsertPoint(basic_block_suspend);
    irb_.CreateCall(slow_func);
    irb_.CreateBr(basic_block_else);
    irb_.SetInsertPoint(basic_block_else);
    irb_.CreateRetVoid();

    OverrideRuntimeSupportFunction(TestSuspend, func);
  }
}

} // namespace compiler_llvm
} // namespace art
