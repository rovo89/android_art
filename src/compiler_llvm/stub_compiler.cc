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

#include "stub_compiler.h"

#include "base/logging.h"
#include "compiled_method.h"
#include "compiler.h"
#include "compiler_llvm.h"
#include "ir_builder.h"
#include "llvm_compilation_unit.h"
#include "mirror/abstract_method.h"
#include "runtime_support_func.h"
#include "utils_llvm.h"

#include <llvm/BasicBlock.h>
#include <llvm/Function.h>
#include <llvm/GlobalVariable.h>
#include <llvm/Intrinsics.h>

#include <string>
#include <string.h>

namespace art {
namespace compiler_llvm {

using namespace runtime_support;


StubCompiler::StubCompiler(LlvmCompilationUnit* cunit, Compiler& compiler)
: cunit_(cunit), compiler_(&compiler), module_(cunit_->GetModule()),
  context_(cunit_->GetLLVMContext()), irb_(*cunit_->GetIRBuilder()) {
}


CompiledInvokeStub* StubCompiler::CreateInvokeStub(bool is_static,
                                                   const char* shorty) {
  CHECK(shorty != NULL);
  size_t shorty_size = strlen(shorty);

  // Function name
  std::string func_name(ElfFuncName(cunit_->GetIndex()));

  // Get argument types
  llvm::Type* arg_types[] = {
    irb_.getJObjectTy(), // Method object pointer
    irb_.getJObjectTy(), // "this" object pointer (NULL for static)
    irb_.getJObjectTy(), // Thread object pointer
    irb_.getJValueTy()->getPointerTo(),
    irb_.getJValueTy()->getPointerTo(),
  };

  // Function type
  llvm::FunctionType* func_type =
    llvm::FunctionType::get(irb_.getVoidTy(), arg_types, false);

  // Create function
  llvm::Function* func =
    llvm::Function::Create(func_type, llvm::Function::ExternalLinkage,
                           func_name, module_);


  // Create basic block for the body of this function
  llvm::BasicBlock* block_body =
    llvm::BasicBlock::Create(*context_, "upcall", func);

  irb_.SetInsertPoint(block_body);

  // Actual arguments
  llvm::Function::arg_iterator arg_iter = func->arg_begin();

  llvm::Value* method_object_addr = arg_iter++;
  llvm::Value* callee_this_addr = arg_iter++;
  llvm::Value* thread_object_addr = arg_iter++;
  llvm::Value* actual_args_array_addr = arg_iter++;
  llvm::Value* retval_addr = arg_iter++;

  // Setup thread pointer
  llvm::Value* old_thread_register = irb_.Runtime().EmitSetCurrentThread(thread_object_addr);

  // Accurate function type
  llvm::Type* accurate_ret_type = irb_.getJType(shorty[0], kAccurate);

  std::vector<llvm::Type*> accurate_arg_types;

  accurate_arg_types.push_back(irb_.getJObjectTy()); // method object pointer

  if (!is_static) {
    accurate_arg_types.push_back(irb_.getJObjectTy());
  }

  for (size_t i = 1; i < shorty_size; ++i) {
    accurate_arg_types.push_back(irb_.getJType(shorty[i], kAccurate));
  }

  llvm::FunctionType* accurate_func_type =
    llvm::FunctionType::get(accurate_ret_type, accurate_arg_types, false);

  // Load actual arguments
  std::vector<llvm::Value*> args;

  args.push_back(method_object_addr);

  if (!is_static) {
    args.push_back(callee_this_addr);
  }

  for (size_t i = 1; i < shorty_size; ++i) {
    char arg_shorty = shorty[i];

    if (arg_shorty == 'Z' || arg_shorty == 'B' || arg_shorty == 'C' ||
        arg_shorty == 'S' || arg_shorty == 'I' || arg_shorty == 'J' ||
        arg_shorty == 'F' || arg_shorty == 'D' || arg_shorty == 'L') {

      llvm::Type* arg_type =
        irb_.getJType(shorty[i], kAccurate)->getPointerTo();

      llvm::Value* arg_jvalue_addr =
        irb_.CreateConstGEP1_32(actual_args_array_addr, i - 1);

      llvm::Value* arg_addr = irb_.CreateBitCast(arg_jvalue_addr, arg_type);

      args.push_back(irb_.CreateLoad(arg_addr, kTBAAStackTemp));

    } else {
      LOG(FATAL) << "Unexpected arg shorty for invoke stub: " << shorty[i];
    }
  }

  // Invoke managed method now!
  llvm::Value* code_field_offset_value =
    irb_.getPtrEquivInt(mirror::AbstractMethod::GetCodeOffset().Int32Value());

  llvm::Value* code_field_addr =
    irb_.CreatePtrDisp(method_object_addr, code_field_offset_value,
                       accurate_func_type->getPointerTo()->getPointerTo());

  llvm::Value* code_addr = irb_.CreateLoad(code_field_addr, kTBAARuntimeInfo);

  llvm::CallInst* retval = irb_.CreateCall(code_addr, args);
  for (size_t i = 1; i < shorty_size; ++i) {
    switch(shorty[i]) {
      case 'Z':
      case 'C':
        retval->addAttribute(i + (is_static ? 1 : 2), llvm::Attribute::ZExt);
        break;

      case 'B':
      case 'S':
        retval->addAttribute(i + (is_static ? 1 : 2), llvm::Attribute::SExt);
        break;

      default: break;
    }
  }

  // Store the returned value
  if (shorty[0] != 'V') {
    llvm::Value* ret_addr =
      irb_.CreateBitCast(retval_addr, accurate_ret_type->getPointerTo());

    irb_.CreateStore(retval, ret_addr, kTBAAStackTemp);
  }

  // Restore thread register
  irb_.Runtime().EmitSetCurrentThread(old_thread_register);
  irb_.CreateRetVoid();

  // Verify the generated function
  VERIFY_LLVM_FUNCTION(*func);

  cunit_->Materialize();

  return new CompiledInvokeStub(cunit_->GetInstructionSet(),
                                cunit_->GetCompiledCode());
}


CompiledInvokeStub* StubCompiler::CreateProxyStub(const char* shorty) {
  CHECK(shorty != NULL);
  size_t shorty_size = strlen(shorty);

  // Function name
  std::string func_name(ElfFuncName(cunit_->GetIndex()));

  // Accurate function type
  llvm::Type* accurate_ret_type = irb_.getJType(shorty[0], kAccurate);

  std::vector<llvm::Type*> accurate_arg_types;
  accurate_arg_types.push_back(irb_.getJObjectTy()); // method
  accurate_arg_types.push_back(irb_.getJObjectTy()); // this

  for (size_t i = 1; i < shorty_size; ++i) {
    accurate_arg_types.push_back(irb_.getJType(shorty[i], kAccurate));
  }

  llvm::FunctionType* accurate_func_type =
    llvm::FunctionType::get(accurate_ret_type, accurate_arg_types, false);

  // Create function
  llvm::Function* func =
    llvm::Function::Create(accurate_func_type, llvm::Function::ExternalLinkage,
                           func_name, module_);
  switch(shorty[0]) {
    case 'Z':
    case 'C':
      func->addAttribute(0, llvm::Attribute::ZExt);
      break;

    case 'B':
    case 'S':
      func->addAttribute(0, llvm::Attribute::SExt);
      break;

    default: break;
  }

  // Create basic block for the body of this function
  llvm::BasicBlock* block_body =
    llvm::BasicBlock::Create(*context_, "proxy", func);
  irb_.SetInsertPoint(block_body);

  // JValue for proxy return
  llvm::AllocaInst* jvalue_temp = irb_.CreateAlloca(irb_.getJValueTy());

  // Load actual arguments
  llvm::Function::arg_iterator arg_iter = func->arg_begin();

  std::vector<llvm::Value*> args;
  args.push_back(arg_iter++); // method
  args.push_back(arg_iter++); // this
  args.push_back(irb_.Runtime().EmitGetCurrentThread()); // thread

  for (size_t i = 1; i < shorty_size; ++i) {
    args.push_back(arg_iter++);
  }

  if (shorty[0] != 'V') {
    args.push_back(jvalue_temp);
  }

  // Call ProxyInvokeHandler
  // TODO: Partial inline ProxyInvokeHandler, don't use VarArg.
  irb_.CreateCall(irb_.GetRuntime(ProxyInvokeHandler), args);

  if (shorty[0] != 'V') {
    llvm::Value* result_addr =
        irb_.CreateBitCast(jvalue_temp, accurate_ret_type->getPointerTo());
    llvm::Value* retval = irb_.CreateLoad(result_addr, kTBAAStackTemp);
    irb_.CreateRet(retval);
  } else {
    irb_.CreateRetVoid();
  }

  // Verify the generated function
  VERIFY_LLVM_FUNCTION(*func);

  cunit_->Materialize();

  return new CompiledInvokeStub(cunit_->GetInstructionSet(),
                                cunit_->GetCompiledCode());
}


} // namespace compiler_llvm
} // namespace art
