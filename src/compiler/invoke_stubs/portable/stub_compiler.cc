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
#include "compiler/driver/compiler_driver.h"
#include "compiler/llvm/compiler_llvm.h"
#include "compiler/llvm/ir_builder.h"
#include "compiler/llvm/llvm_compilation_unit.h"
#include "compiler/llvm/runtime_support_func.h"
#include "compiler/llvm/utils_llvm.h"
#include "mirror/abstract_method.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Intrinsics.h>

#include <string>
#include <string.h>

namespace art {
namespace llvm {

using namespace runtime_support;


StubCompiler::StubCompiler(LlvmCompilationUnit* cunit, const CompilerDriver& driver)
: cunit_(cunit), driver_(&driver), module_(cunit_->GetModule()),
  context_(cunit_->GetLLVMContext()), irb_(*cunit_->GetIRBuilder()) {
}


CompiledInvokeStub* StubCompiler::CreateProxyStub(const char* shorty) {
  CHECK(shorty != NULL);
  size_t shorty_size = strlen(shorty);

  // Function name
  std::string func_name(StringPrintf("proxy_stub_%s", shorty));

  // Accurate function type
  ::llvm::Type* accurate_ret_type = irb_.getJType(shorty[0]);

  std::vector< ::llvm::Type*> accurate_arg_types;
  accurate_arg_types.push_back(irb_.getJObjectTy()); // method
  accurate_arg_types.push_back(irb_.getJObjectTy()); // this

  for (size_t i = 1; i < shorty_size; ++i) {
    accurate_arg_types.push_back(irb_.getJType(shorty[i]));
  }

  ::llvm::FunctionType* accurate_func_type =
    ::llvm::FunctionType::get(accurate_ret_type, accurate_arg_types, false);

  // Create function
  ::llvm::Function* func =
    ::llvm::Function::Create(accurate_func_type, ::llvm::Function::InternalLinkage,
                             func_name, module_);
  switch(shorty[0]) {
    case 'Z':
    case 'C':
      func->addAttribute(0, ::llvm::Attribute::ZExt);
      break;

    case 'B':
    case 'S':
      func->addAttribute(0, ::llvm::Attribute::SExt);
      break;

    default: break;
  }

  // Create basic block for the body of this function
  ::llvm::BasicBlock* block_body =
    ::llvm::BasicBlock::Create(*context_, "proxy", func);
  irb_.SetInsertPoint(block_body);

  // JValue for proxy return
  ::llvm::AllocaInst* jvalue_temp = irb_.CreateAlloca(irb_.getJValueTy());

  // Load actual arguments
  ::llvm::Function::arg_iterator arg_iter = func->arg_begin();

  std::vector< ::llvm::Value*> args;
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
    ::llvm::Value* result_addr =
        irb_.CreateBitCast(jvalue_temp, accurate_ret_type->getPointerTo());
    ::llvm::Value* retval = irb_.CreateLoad(result_addr, kTBAAStackTemp);
    irb_.CreateRet(retval);
  } else {
    irb_.CreateRetVoid();
  }

  // Verify the generated function
  VERIFY_LLVM_FUNCTION(*func);

  cunit_->Materialize();

  return new CompiledInvokeStub(cunit_->GetInstructionSet(),
                                cunit_->GetElfObject(),
                                func_name);
}


} // namespace llvm
} // namespace art
