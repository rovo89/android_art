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

#include "upcall_compiler.h"

#include "compilation_unit.h"
#include "compiled_method.h"
#include "compiler.h"
#include "compiler_llvm.h"
#include "ir_builder.h"
#include "logging.h"
#include "object.h"
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


UpcallCompiler::UpcallCompiler(CompilationUnit* cunit, Compiler& compiler)
: cunit_(cunit), compiler_(&compiler), module_(cunit_->GetModule()),
  context_(cunit_->GetLLVMContext()), irb_(*cunit_->GetIRBuilder()),
  elf_func_idx_(cunit_->AcquireUniqueElfFuncIndex()) {
}


CompiledInvokeStub* UpcallCompiler::CreateStub(bool is_static,
                                               char const* shorty) {

  CHECK_NE(shorty, static_cast<char const*>(NULL));
  size_t shorty_size = strlen(shorty);

  // Function name
  std::string func_name(ElfFuncName(elf_func_idx_));

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
  irb_.Runtime().EmitSetCurrentThread(thread_object_addr);

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
  // TODO: If we solve the trampoline related problems, we can just get the code address and call.
#if 0
  llvm::Value* code_field_offset_value =
    irb_.getPtrEquivInt(Method::GetCodeOffset().Int32Value());

  llvm::Value* code_field_addr =
    irb_.CreatePtrDisp(method_object_addr, code_field_offset_value,
                       accurate_func_type->getPointerTo()->getPointerTo());

  llvm::Value* code_addr = irb_.CreateLoad(code_field_addr, kTBAAJRuntime);
#else
  llvm::Value* result = irb_.CreateCall(irb_.GetRuntime(FixStub), method_object_addr);
  llvm::Value* code_addr = irb_.CreatePointerCast(result, accurate_func_type->getPointerTo());

  // Exception unwind.
  llvm::Value* exception_pending = irb_.Runtime().EmitIsExceptionPending();
  llvm::BasicBlock* block_unwind = llvm::BasicBlock::Create(*context_, "exception_unwind", func);
  llvm::BasicBlock* block_cont = llvm::BasicBlock::Create(*context_, "cont", func);
  irb_.CreateCondBr(exception_pending, block_unwind, block_cont);
  irb_.SetInsertPoint(block_unwind);
  irb_.CreateRetVoid();
  irb_.SetInsertPoint(block_cont);
#endif

  llvm::Value* retval = irb_.CreateCall(code_addr, args);

  // Store the returned value
  if (shorty[0] != 'V') {
    llvm::Value* ret_addr =
      irb_.CreateBitCast(retval_addr, accurate_ret_type->getPointerTo());

    irb_.CreateStore(retval, ret_addr, kTBAAStackTemp);
  }

  irb_.CreateRetVoid();

  // Verify the generated function
  VERIFY_LLVM_FUNCTION(*func);

  // Add the memory usage approximation of the compilation unit
  cunit_->AddMemUsageApproximation((shorty_size * 3 + 8) * 50);
  // NOTE: We will emit 3 LLVM instructions per shorty for the argument,
  // plus 3 for pointer arithmetic, and 5 for code_addr, retval, ret_addr,
  // store ret_addr, and ret_void.  Beside, we guess that we have to use
  // 50 bytes to represent one LLVM instruction.

  return new CompiledInvokeStub(cunit_->GetElfIndex(), elf_func_idx_);
}


} // namespace compiler_llvm
} // namespace art
