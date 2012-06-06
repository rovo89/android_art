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
#include "compilation_unit.h"
#include "compiled_method.h"
#include "compiler.h"
#include "compiler_llvm.h"
#include "ir_builder.h"
#include "logging.h"
#include "oat_compilation_unit.h"
#include "object.h"
#include "runtime.h"
#include "runtime_support_func.h"
#include "shadow_frame.h"
#include "utils_llvm.h"

#include <llvm/BasicBlock.h>
#include <llvm/DerivedTypes.h>
#include <llvm/Function.h>
#include <llvm/Type.h>

namespace art {
namespace compiler_llvm {

using namespace runtime_support;

JniCompiler::JniCompiler(CompilationUnit* cunit,
                         Compiler const& compiler,
                         OatCompilationUnit* oat_compilation_unit)
: cunit_(cunit), compiler_(&compiler), module_(cunit_->GetModule()),
  context_(cunit_->GetLLVMContext()), irb_(*cunit_->GetIRBuilder()),
  oat_compilation_unit_(oat_compilation_unit),
  access_flags_(oat_compilation_unit->access_flags_),
  method_idx_(oat_compilation_unit->method_idx_),
  class_linker_(oat_compilation_unit->class_linker_),
  class_loader_(oat_compilation_unit->class_loader_),
  dex_cache_(oat_compilation_unit->dex_cache_),
  dex_file_(oat_compilation_unit->dex_file_),
  method_(dex_cache_->GetResolvedMethod(method_idx_)),
  elf_func_idx_(cunit_->AcquireUniqueElfFuncIndex()) {

  // Check: Ensure that the method is resolved
  CHECK_NE(method_, static_cast<art::Method*>(NULL));

  // Check: Ensure that JNI compiler will only get "native" method
  CHECK((access_flags_ & kAccNative) != 0);
}


CompiledMethod* JniCompiler::Compile() {
  const bool is_static = (access_flags_ & kAccStatic) != 0;
  const bool is_synchronized = (access_flags_ & kAccSynchronized) != 0;
  DexFile::MethodId const& method_id = dex_file_->GetMethodId(method_idx_);
  char const return_shorty = dex_file_->GetMethodShorty(method_id)[0];
  llvm::Value* this_object_or_class_object;

  CreateFunction();

  // Set argument name
  llvm::Function::arg_iterator arg_begin(func_->arg_begin());
  llvm::Function::arg_iterator arg_end(func_->arg_end());
  llvm::Function::arg_iterator arg_iter(arg_begin);

  DCHECK_NE(arg_iter, arg_end);
  arg_iter->setName("method");
  llvm::Value* method_object_addr = arg_iter++;

  if (!is_static) {
    // Non-static, the second argument is "this object"
    this_object_or_class_object = arg_iter++;
  } else {
    // Load class object
    this_object_or_class_object =
        irb_.LoadFromObjectOffset(method_object_addr,
                                  Method::DeclaringClassOffset().Int32Value(),
                                  irb_.getJObjectTy(),
                                  kTBAAConstJObject);
  }
  // Actual argument (ignore method and this object)
  arg_begin = arg_iter;

  // Count the number of Object* arguments
  uint32_t sirt_size = 1;
  // "this" object pointer for non-static
  // "class" object pointer for static
  for (unsigned i = 0; arg_iter != arg_end; ++i, ++arg_iter) {
#if !defined(NDEBUG)
    arg_iter->setName(StringPrintf("a%u", i));
#endif
    if (arg_iter->getType() == irb_.getJObjectTy()) {
      ++sirt_size;
    }
  }

  // Shadow stack
  llvm::StructType* shadow_frame_type = irb_.getShadowFrameTy(sirt_size);
  llvm::AllocaInst* shadow_frame_ = irb_.CreateAlloca(shadow_frame_type);

  // Store the dex pc
  irb_.StoreToObjectOffset(shadow_frame_,
                           ShadowFrame::DexPCOffset(),
                           irb_.getInt32(0),
                           kTBAAShadowFrame);

  // Push the shadow frame
  llvm::Value* shadow_frame_upcast = irb_.CreateConstGEP2_32(shadow_frame_, 0, 0);
  llvm::Value* old_shadow_frame =
      irb_.Runtime().EmitPushShadowFrame(shadow_frame_upcast, method_object_addr, sirt_size);

  // Get JNIEnv
  llvm::Value* jni_env_object_addr =
      irb_.Runtime().EmitLoadFromThreadOffset(Thread::JniEnvOffset().Int32Value(),
                                          irb_.getJObjectTy(),
                                          kTBAAJRuntime);

  // Set thread state to kNative
  irb_.Runtime().EmitStoreToThreadOffset(Thread::StateOffset().Int32Value(),
                                     irb_.getInt32(kNative),
                                     kTBAARuntimeInfo);

  // Get callee code_addr
  llvm::Value* code_addr =
      irb_.LoadFromObjectOffset(method_object_addr,
                                Method::NativeMethodOffset().Int32Value(),
                                GetFunctionType(method_idx_, is_static, true)->getPointerTo(),
                                kTBAAJRuntime);

  // Load actual parameters
  std::vector<llvm::Value*> args;

  // The 1st parameter: JNIEnv*
  args.push_back(jni_env_object_addr);

  // Variables for GetElementPtr
  llvm::Value* gep_index[] = {
    irb_.getInt32(0), // No displacement for shadow frame pointer
    irb_.getInt32(1), // SIRT
    NULL,
  };

  size_t sirt_member_index = 0;

  // Store the "this object or class object" to SIRT
  gep_index[2] = irb_.getInt32(sirt_member_index++);
  llvm::Value* sirt_field_addr = irb_.CreateGEP(shadow_frame_, gep_index);
  irb_.CreateStore(this_object_or_class_object, sirt_field_addr, kTBAAShadowFrame);
  // Push the "this object or class object" to out args
  args.push_back(irb_.CreateBitCast(sirt_field_addr, irb_.getJObjectTy()));
  // Store arguments to SIRT, and push back to args
  for (arg_iter = arg_begin; arg_iter != arg_end; ++arg_iter) {
    if (arg_iter->getType() == irb_.getJObjectTy()) {
      // Store the reference type arguments to SIRT
      gep_index[2] = irb_.getInt32(sirt_member_index++);
      llvm::Value* sirt_field_addr = irb_.CreateGEP(shadow_frame_, gep_index);
      irb_.CreateStore(arg_iter, sirt_field_addr, kTBAAShadowFrame);
      // Note null is placed in the SIRT but the jobject passed to the native code must be null
      // (not a pointer into the SIRT as with regular references).
      llvm::Value* equal_null = irb_.CreateICmpEQ(arg_iter, irb_.getJNull());
      llvm::Value* arg =
          irb_.CreateSelect(equal_null,
                            irb_.getJNull(),
                            irb_.CreateBitCast(sirt_field_addr, irb_.getJObjectTy()));
      args.push_back(arg);
    } else {
      args.push_back(arg_iter);
    }
  }

  // Acquire lock for synchronized methods.
  if (is_synchronized) {
    irb_.Runtime().EmitLockObject(this_object_or_class_object);
  }

  // saved_local_ref_cookie = env->local_ref_cookie
  llvm::Value* saved_local_ref_cookie =
      irb_.LoadFromObjectOffset(jni_env_object_addr,
                                JNIEnvExt::LocalRefCookieOffset().Int32Value(),
                                irb_.getInt32Ty(),
                                kTBAARuntimeInfo);

  // env->local_ref_cookie = env->locals.segment_state
  llvm::Value* segment_state =
      irb_.LoadFromObjectOffset(jni_env_object_addr,
                                JNIEnvExt::SegmentStateOffset().Int32Value(),
                                irb_.getInt32Ty(),
                                kTBAARuntimeInfo);
  irb_.StoreToObjectOffset(jni_env_object_addr,
                           JNIEnvExt::LocalRefCookieOffset().Int32Value(),
                           segment_state,
                           kTBAARuntimeInfo);


  // Call!!!
  llvm::Value* retval = irb_.CreateCall(code_addr, args);


  // Release lock for synchronized methods.
  if (is_synchronized) {
    irb_.Runtime().EmitUnlockObject(this_object_or_class_object);
  }

  // Set thread state to kRunnable
  irb_.Runtime().EmitStoreToThreadOffset(Thread::StateOffset().Int32Value(),
                                     irb_.getInt32(kRunnable),
                                     kTBAARuntimeInfo);

  // Do a suspend check
  irb_.Runtime().EmitTestSuspend();

  if (return_shorty == 'L') {
    // Get thread object
    llvm::Value* thread_object_addr = irb_.Runtime().EmitGetCurrentThread();

    // If the return value is reference, it may point to SIRT, we should decode it.
    retval = irb_.CreateCall2(irb_.GetRuntime(DecodeJObjectInThread),
                              thread_object_addr,
                              retval);
  }

  // env->locals.segment_state = env->local_ref_cookie
  llvm::Value* local_ref_cookie =
      irb_.LoadFromObjectOffset(jni_env_object_addr,
                                JNIEnvExt::LocalRefCookieOffset().Int32Value(),
                                irb_.getInt32Ty(),
                                kTBAARuntimeInfo);
  irb_.StoreToObjectOffset(jni_env_object_addr,
                           JNIEnvExt::SegmentStateOffset().Int32Value(),
                           local_ref_cookie,
                           kTBAARuntimeInfo);

  // env->local_ref_cookie = saved_local_ref_cookie
  irb_.StoreToObjectOffset(jni_env_object_addr,
                           JNIEnvExt::LocalRefCookieOffset().Int32Value(),
                           saved_local_ref_cookie,
                           kTBAARuntimeInfo);

  // Pop the shadow frame
  irb_.Runtime().EmitPopShadowFrame(old_shadow_frame);

  // Return!
  if (return_shorty != 'V') {
    irb_.CreateRet(retval);
  } else {
    irb_.CreateRetVoid();
  }

  // Verify the generated bitcode
  VERIFY_LLVM_FUNCTION(*func_);

  // Add the memory usage approximation of the compilation unit
  cunit_->AddMemUsageApproximation((sirt_size * 4 + 50) * 50);
  // NOTE: We will emit 4 LLVM instructions per object argument,
  // And about 50 instructions for other operations. (Some runtime support will be inlined.)
  // Beside, we guess that we have to use 50 bytes to represent one LLVM instruction.

  CompiledMethod* compiled_method =
      new CompiledMethod(cunit_->GetInstructionSet(),
                         cunit_->GetElfIndex(),
                         elf_func_idx_);

  cunit_->RegisterCompiledMethod(func_, compiled_method);

  return compiled_method;
}


void JniCompiler::CreateFunction() {
  // LLVM function name
  std::string func_name(ElfFuncName(elf_func_idx_));

  // Get function type
  llvm::FunctionType* func_type =
    GetFunctionType(method_idx_, method_->IsStatic(), false);

  // Create function
  func_ = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage,
                                 func_name, module_);

  // Create basic block
  llvm::BasicBlock* basic_block = llvm::BasicBlock::Create(*context_, "B0", func_);

  // Set insert point
  irb_.SetInsertPoint(basic_block);
}


llvm::FunctionType* JniCompiler::GetFunctionType(uint32_t method_idx,
                                                 bool is_static, bool is_native_function) {
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

  if (!is_static || is_native_function) {
    // "this" object pointer for non-static
    // "class" object pointer for static naitve
    args_type.push_back(irb_.getJType('L', kAccurate));
  }

  for (uint32_t i = 1; i < shorty_size; ++i) {
    args_type.push_back(irb_.getJType(shorty[i], kAccurate));
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}

} // namespace compiler_llvm
} // namespace art
