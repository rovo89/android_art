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

#ifndef ART_COMPILER_COMMON_COMPILER_TEST_H_
#define ART_COMPILER_COMMON_COMPILER_TEST_H_

#include <list>
#include <vector>

#include "common_runtime_test.h"
#include "oat_file.h"

namespace art {
namespace mirror {
  class ClassLoader;
}  // namespace mirror

class CompilerDriver;
class CompilerOptions;
class CumulativeLogger;
class DexFileToMethodInlinerMap;
class VerificationResults;

template<class T> class Handle;

class CommonCompilerTest : public CommonRuntimeTest {
 public:
  CommonCompilerTest();
  ~CommonCompilerTest();

  // Create an OatMethod based on pointers (for unit tests).
  OatFile::OatMethod CreateOatMethod(const void* code, const uint8_t* gc_map);

  void MakeExecutable(mirror::ArtMethod* method) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void MakeExecutable(const void* code_start, size_t code_length);

  void MakeExecutable(mirror::ClassLoader* class_loader, const char* class_name)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 protected:
  virtual void SetUp();

  virtual void SetUpRuntimeOptions(RuntimeOptions *options);

  virtual void TearDown();

  void CompileClass(mirror::ClassLoader* class_loader, const char* class_name)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void CompileMethod(mirror::ArtMethod* method) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void CompileDirectMethod(Handle<mirror::ClassLoader> class_loader, const char* class_name,
                           const char* method_name, const char* signature)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void CompileVirtualMethod(Handle<mirror::ClassLoader> class_loader, const char* class_name,
                            const char* method_name, const char* signature)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ReserveImageSpace();

  void UnreserveImageSpace();

  std::unique_ptr<CompilerOptions> compiler_options_;
  std::unique_ptr<VerificationResults> verification_results_;
  std::unique_ptr<DexFileToMethodInlinerMap> method_inliner_map_;
  std::unique_ptr<CompilerCallbacks> callbacks_;
  std::unique_ptr<CompilerDriver> compiler_driver_;
  std::unique_ptr<CumulativeLogger> timer_;

 private:
  std::unique_ptr<MemMap> image_reservation_;

  // Chunks must not move their storage after being created - use the node-based std::list.
  std::list<std::vector<uint8_t>> header_code_and_maps_chunks_;
};

}  // namespace art

#endif  // ART_COMPILER_COMMON_COMPILER_TEST_H_
