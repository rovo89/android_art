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

#ifndef ART_SRC_COMPILER_LLVM_STUB_COMPILER_H_
#define ART_SRC_COMPILER_LLVM_STUB_COMPILER_H_

#include <stdint.h>

namespace art {
  class CompiledInvokeStub;
  class CompiledProxyStub;
  class CompilerDriver;
}

namespace llvm {
  class LLVMContext;
  class Module;
}

namespace art {
namespace llvm {

class LlvmCompilationUnit;
class CompilerLLVM;
class IRBuilder;

class StubCompiler {
 public:
  StubCompiler(LlvmCompilationUnit* cunit, const CompilerDriver& compiler);

  CompiledInvokeStub* CreateInvokeStub(bool is_static, const char* shorty);
  CompiledInvokeStub* CreateProxyStub(const char* shorty);

 private:
  LlvmCompilationUnit* cunit_;
  const CompilerDriver* const driver_;
  ::llvm::Module* module_;
  ::llvm::LLVMContext* context_;
  IRBuilder& irb_;
};


} // namespace llvm
} // namespace art


#endif // ART_SRC_COMPILER_LLVM_STUB_COMPILER_H_
