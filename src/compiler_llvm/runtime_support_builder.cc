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

#include "card_table.h"
#include "ir_builder.h"
#include "monitor.h"
#include "object.h"
#include "shadow_frame.h"
#include "thread.h"
#include "utils_llvm.h"

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
  memset(target_runtime_support_func_, 0, sizeof(target_runtime_support_func_));
#define GET_RUNTIME_SUPPORT_FUNC_DECL(ID, NAME) \
  do { \
    llvm::Function* fn = module_.getFunction(#NAME); \
    DCHECK_NE(fn, (void*)NULL) << "Function not found: " << #NAME; \
    runtime_support_func_decls_[runtime_support::ID] = fn; \
  } while (0);

#include "runtime_support_func_list.h"
  RUNTIME_SUPPORT_FUNC_LIST(GET_RUNTIME_SUPPORT_FUNC_DECL)
#undef RUNTIME_SUPPORT_FUNC_LIST
#undef GET_RUNTIME_SUPPORT_FUNC_DECL
}

void RuntimeSupportBuilder::MakeFunctionInline(llvm::Function* func) {
  func->setLinkage(GlobalValue::LinkOnceODRLinkage);
  func->addFnAttr(Attribute::AlwaysInline);
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


/* Thread */

llvm::Value* RuntimeSupportBuilder::EmitGetCurrentThread() {
  Function* func = GetRuntimeSupportFunction(runtime_support::GetCurrentThread);
  llvm::CallInst* call_inst = irb_.CreateCall(func);
  call_inst->setOnlyReadsMemory();
  irb_.SetTBAA(call_inst, kTBAAConstJObject);
  return call_inst;
}

llvm::Value* RuntimeSupportBuilder::EmitLoadFromThreadOffset(int64_t offset, llvm::Type* type,
                                                             TBAASpecialType s_ty) {
  llvm::Value* thread = EmitGetCurrentThread();
  return irb_.LoadFromObjectOffset(thread, offset, type, s_ty);
}

void RuntimeSupportBuilder::EmitStoreToThreadOffset(int64_t offset, llvm::Value* value,
                                                    TBAASpecialType s_ty) {
  llvm::Value* thread = EmitGetCurrentThread();
  irb_.StoreToObjectOffset(thread, offset, value, s_ty);
}

void RuntimeSupportBuilder::EmitSetCurrentThread(llvm::Value* thread) {
  Function* func = GetRuntimeSupportFunction(runtime_support::SetCurrentThread);
  irb_.CreateCall(func, thread);
}


/* ShadowFrame */

llvm::Value* RuntimeSupportBuilder::EmitPushShadowFrame(llvm::Value* new_shadow_frame,
                                                        llvm::Value* method, uint32_t size) {
  Value* old_shadow_frame = EmitLoadFromThreadOffset(Thread::TopShadowFrameOffset().Int32Value(),
                                                     irb_.getArtFrameTy()->getPointerTo(),
                                                     kTBAARuntimeInfo);
  EmitStoreToThreadOffset(Thread::TopShadowFrameOffset().Int32Value(),
                          new_shadow_frame,
                          kTBAARuntimeInfo);

  // Store the method pointer
  irb_.StoreToObjectOffset(new_shadow_frame,
                           ShadowFrame::MethodOffset(),
                           method,
                           kTBAAShadowFrame);

  // Store the number of the pointer slots
  irb_.StoreToObjectOffset(new_shadow_frame,
                           ShadowFrame::NumberOfReferencesOffset(),
                           irb_.getInt32(size),
                           kTBAAShadowFrame);

  // Store the link to previous shadow frame
  irb_.StoreToObjectOffset(new_shadow_frame,
                           ShadowFrame::LinkOffset(),
                           old_shadow_frame,
                           kTBAAShadowFrame);

  return old_shadow_frame;
}

llvm::Value*
RuntimeSupportBuilder::EmitPushShadowFrameNoInline(llvm::Value* new_shadow_frame,
                                                   llvm::Value* method, uint32_t size) {
  Function* func = GetRuntimeSupportFunction(runtime_support::PushShadowFrame);
  llvm::CallInst* call_inst =
      irb_.CreateCall4(func, EmitGetCurrentThread(), new_shadow_frame, method, irb_.getInt32(size));
  irb_.SetTBAA(call_inst, kTBAARuntimeInfo);
  return call_inst;
}

void RuntimeSupportBuilder::EmitPopShadowFrame(llvm::Value* old_shadow_frame) {
  // Store old shadow frame to TopShadowFrame
  EmitStoreToThreadOffset(Thread::TopShadowFrameOffset().Int32Value(),
                          old_shadow_frame,
                          kTBAARuntimeInfo);
}


/* Check */

llvm::Value* RuntimeSupportBuilder::EmitIsExceptionPending() {
  Value* exception = EmitLoadFromThreadOffset(Thread::ExceptionOffset().Int32Value(),
                                              irb_.getJObjectTy(),
                                              kTBAAJRuntime);
  // If exception not null
  return irb_.CreateICmpNE(exception, irb_.getJNull());
}

void RuntimeSupportBuilder::EmitTestSuspend() {
  Function* slow_func = GetRuntimeSupportFunction(runtime_support::TestSuspend);
  Value* suspend_count = EmitLoadFromThreadOffset(Thread::SuspendCountOffset().Int32Value(),
                                                  irb_.getJIntTy(),
                                                  kTBAARuntimeInfo);
  Value* is_suspend = irb_.CreateICmpNE(suspend_count, irb_.getJInt(0));

  llvm::Function* parent_func = irb_.GetInsertBlock()->getParent();
  BasicBlock* basic_block_suspend = BasicBlock::Create(context_, "suspend", parent_func);
  BasicBlock* basic_block_cont = BasicBlock::Create(context_, "suspend_cont", parent_func);
  irb_.CreateCondBr(is_suspend, basic_block_suspend, basic_block_cont, kUnlikely);

  irb_.SetInsertPoint(basic_block_suspend);
  CallInst* call_inst = irb_.CreateCall(slow_func, EmitGetCurrentThread());
  irb_.SetTBAA(call_inst, kTBAARuntimeInfo);
  irb_.CreateBr(basic_block_cont);

  irb_.SetInsertPoint(basic_block_cont);
}


/* Monitor */

void RuntimeSupportBuilder::EmitLockObject(llvm::Value* object) {
  // TODO: Implement a fast path.
  Function* slow_func = GetRuntimeSupportFunction(runtime_support::LockObject);
  irb_.CreateCall2(slow_func, object, EmitGetCurrentThread());
}

void RuntimeSupportBuilder::EmitUnlockObject(llvm::Value* object) {
  llvm::Value* lock_id =
      EmitLoadFromThreadOffset(Thread::ThinLockIdOffset().Int32Value(),
                               irb_.getJIntTy(),
                               kTBAARuntimeInfo);
  llvm::Value* monitor =
      irb_.LoadFromObjectOffset(object,
                                Object::MonitorOffset().Int32Value(),
                                irb_.getJIntTy(),
                                kTBAARuntimeInfo);

  llvm::Value* my_monitor = irb_.CreateShl(lock_id, LW_LOCK_OWNER_SHIFT);
  llvm::Value* hash_state = irb_.CreateAnd(monitor, (LW_HASH_STATE_MASK << LW_HASH_STATE_SHIFT));
  llvm::Value* real_monitor = irb_.CreateAnd(monitor, ~(LW_HASH_STATE_MASK << LW_HASH_STATE_SHIFT));

  // Is thin lock, held by us and not recursively acquired
  llvm::Value* is_fast_path = irb_.CreateICmpEQ(real_monitor, my_monitor);

  llvm::Function* parent_func = irb_.GetInsertBlock()->getParent();
  BasicBlock* bb_fast = BasicBlock::Create(context_, "unlock_fast", parent_func);
  BasicBlock* bb_slow = BasicBlock::Create(context_, "unlock_slow", parent_func);
  BasicBlock* bb_cont = BasicBlock::Create(context_, "unlock_cont", parent_func);
  irb_.CreateCondBr(is_fast_path, bb_fast, bb_slow, kLikely);

  irb_.SetInsertPoint(bb_fast);
  // Set all bits to zero (except hash state)
  irb_.StoreToObjectOffset(object,
                           Object::MonitorOffset().Int32Value(),
                           hash_state,
                           kTBAARuntimeInfo);
  irb_.CreateBr(bb_cont);

  irb_.SetInsertPoint(bb_slow);
  Function* slow_func = GetRuntimeSupportFunction(runtime_support::UnlockObject);
  irb_.CreateCall2(slow_func, object, EmitGetCurrentThread());
  irb_.CreateBr(bb_cont);

  irb_.SetInsertPoint(bb_cont);
}


void RuntimeSupportBuilder::OptimizeRuntimeSupport() {
  // TODO: Remove this after we remove suspend loop pass.
  if (!target_runtime_support_func_[runtime_support::TestSuspend]) {
    Function* slow_func = GetRuntimeSupportFunction(runtime_support::TestSuspend);
    Function* func = Function::Create(slow_func->getFunctionType(),
                                      GlobalValue::LinkOnceODRLinkage,
                                      "test_suspend_fast",
                                      &module_);
    MakeFunctionInline(func);
    BasicBlock* basic_block = BasicBlock::Create(context_, "entry", func);
    irb_.SetInsertPoint(basic_block);

    EmitTestSuspend();

    irb_.CreateRetVoid();

    OverrideRuntimeSupportFunction(runtime_support::TestSuspend, func);

    VERIFY_LLVM_FUNCTION(*func);
  }

  if (!target_runtime_support_func_[MarkGCCard]) {
    Function* func = GetRuntimeSupportFunction(MarkGCCard);
    MakeFunctionInline(func);
    BasicBlock* basic_block = BasicBlock::Create(context_, "entry", func);
    irb_.SetInsertPoint(basic_block);
    Function::arg_iterator arg_iter = func->arg_begin();
    Value* value = arg_iter++;
    Value* target_addr = arg_iter++;

    llvm::Value* is_value_null = irb_.CreateICmpEQ(value, irb_.getJNull());

    llvm::BasicBlock* block_value_is_null = BasicBlock::Create(context_, "value_is_null", func);
    llvm::BasicBlock* block_mark_gc_card = BasicBlock::Create(context_, "mark_gc_card", func);

    irb_.CreateCondBr(is_value_null, block_value_is_null, block_mark_gc_card);

    irb_.SetInsertPoint(block_value_is_null);
    irb_.CreateRetVoid();

    irb_.SetInsertPoint(block_mark_gc_card);
    Value* card_table = EmitLoadFromThreadOffset(Thread::CardTableOffset().Int32Value(),
                                                 irb_.getInt8Ty()->getPointerTo(),
                                                 kTBAAConstJObject);
    Value* target_addr_int = irb_.CreatePtrToInt(target_addr, irb_.getPtrEquivIntTy());
    Value* card_no = irb_.CreateLShr(target_addr_int, irb_.getPtrEquivInt(GC_CARD_SHIFT));
    Value* card_table_entry = irb_.CreateGEP(card_table, card_no);
    irb_.CreateStore(irb_.getInt8(GC_CARD_DIRTY), card_table_entry, kTBAARuntimeInfo);
    irb_.CreateRetVoid();

    VERIFY_LLVM_FUNCTION(*func);
  }
}

} // namespace compiler_llvm
} // namespace art
