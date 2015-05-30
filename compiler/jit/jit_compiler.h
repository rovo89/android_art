/*
 * Copyright 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_JIT_JIT_COMPILER_H_
#define ART_COMPILER_JIT_JIT_COMPILER_H_

#include "base/mutex.h"
#include "compiler_callbacks.h"
#include "compiled_method.h"
#include "dex/verification_results.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "oat_file.h"

namespace art {

class ArtMethod;
class InstructionSetFeatures;

namespace jit {

class JitCompiler {
 public:
  static JitCompiler* Create();
  virtual ~JitCompiler();
  bool CompileMethod(Thread* self, ArtMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  // This is in the compiler since the runtime doesn't have access to the compiled method
  // structures.
  bool AddToCodeCache(ArtMethod* method, const CompiledMethod* compiled_method,
                      OatFile::OatMethod* out_method) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  CompilerCallbacks* GetCompilerCallbacks() const;
  size_t GetTotalCompileTime() const {
    return total_time_;
  }

 private:
  uint64_t total_time_;
  std::unique_ptr<CompilerOptions> compiler_options_;
  std::unique_ptr<CumulativeLogger> cumulative_logger_;
  std::unique_ptr<VerificationResults> verification_results_;
  std::unique_ptr<DexFileToMethodInlinerMap> method_inliner_map_;
  std::unique_ptr<CompilerCallbacks> callbacks_;
  std::unique_ptr<CompilerDriver> compiler_driver_;
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features_;

  explicit JitCompiler();
  uint8_t* WriteMethodHeaderAndCode(
      const CompiledMethod* compiled_method, uint8_t* reserve_begin, uint8_t* reserve_end,
      const uint8_t* mapping_table, const uint8_t* vmap_table, const uint8_t* gc_map);
  bool MakeExecutable(CompiledMethod* compiled_method, ArtMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  DISALLOW_COPY_AND_ASSIGN(JitCompiler);
};

}  // namespace jit
}  // namespace art

#endif  // ART_COMPILER_JIT_JIT_COMPILER_H_
