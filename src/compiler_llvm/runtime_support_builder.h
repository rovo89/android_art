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

#ifndef ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_BUILDER_H_
#define ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_BUILDER_H_

#include "logging.h"
#include "runtime_support_func.h"

namespace llvm {
  class LLVMContext;
  class Module;
  class Function;
}

namespace art {
namespace compiler_llvm {

class IRBuilder;


class RuntimeSupportBuilder {
 public:
  RuntimeSupportBuilder(llvm::LLVMContext& context, llvm::Module& module, IRBuilder& irb);

  llvm::Function* GetRuntimeSupportFunction(runtime_support::RuntimeId id) {
    if (id >= 0 && id < runtime_support::MAX_ID) {
      return runtime_support_func_decls_[id];
    } else {
      LOG(ERROR) << "Unknown runtime function id: " << id;
      return NULL;
    }
  }

  void OptimizeRuntimeSupport();

  virtual ~RuntimeSupportBuilder() {}

 protected:
  // Mark a function as inline function.
  // You should implement the function, if mark as inline.
  void MakeFunctionInline(llvm::Function* function);

  void OverrideRuntimeSupportFunction(runtime_support::RuntimeId id, llvm::Function* function);

 private:
  // Target can override this function to make some runtime support more efficient.
  virtual void TargetOptimizeRuntimeSupport() {}


 protected:
  llvm::LLVMContext& context_;
  llvm::Module& module_;
  IRBuilder& irb_;

 private:
  llvm::Function* runtime_support_func_decls_[runtime_support::MAX_ID];
  bool target_runtime_support_func_[runtime_support::MAX_ID];
};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_BUILDER_H_
