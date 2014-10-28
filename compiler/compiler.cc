/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "compiler.h"

#include "base/logging.h"
#include "dex/quick/quick_compiler.h"
#include "driver/compiler_driver.h"
#include "llvm/llvm_compiler.h"
#include "optimizing/optimizing_compiler.h"

namespace art {

#ifdef ART_SEA_IR_MODE
extern "C" art::CompiledMethod* SeaIrCompileMethod(const art::DexFile::CodeItem* code_item,
                                                   uint32_t access_flags,
                                                   art::InvokeType invoke_type,
                                                   uint16_t class_def_idx,
                                                   uint32_t method_idx,
                                                   jobject class_loader,
                                                   const art::DexFile& dex_file);
#endif


CompiledMethod* Compiler::TryCompileWithSeaIR(const art::DexFile::CodeItem* code_item,
                                              uint32_t access_flags,
                                              art::InvokeType invoke_type,
                                              uint16_t class_def_idx,
                                              uint32_t method_idx,
                                              jobject class_loader,
                                              const art::DexFile& dex_file) {
#ifdef ART_SEA_IR_MODE
    bool use_sea = (std::string::npos != PrettyMethod(method_idx, dex_file).find("fibonacci"));
    if (use_sea) {
      LOG(INFO) << "Using SEA IR to compile..." << std::endl;
      return SeaIrCompileMethod(code_item,
                                access_flags,
                                invoke_type,
                                class_def_idx,
                                method_idx,
                                class_loader,
                                dex_file);
  }
#endif
  return nullptr;
}

Compiler* Compiler::Create(CompilerDriver* driver, Compiler::Kind kind) {
  switch (kind) {
    case kQuick:
      return CreateQuickCompiler(driver);

    case kOptimizing:
      return CreateOptimizingCompiler(driver);

    case kPortable:
      {
        Compiler* compiler = CreateLLVMCompiler(driver);
        CHECK(compiler != nullptr) << "Portable compiler not compiled";
        return compiler;
      }

    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
}

}  // namespace art
