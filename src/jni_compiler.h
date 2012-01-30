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

#ifndef ART_SRC_JNI_COMPILER_H_
#define ART_SRC_JNI_COMPILER_H_

#include "compiled_method.h"
#include "constants.h"
#include "macros.h"
#include "thread.h"

namespace art {

class Assembler;
class ClassLoader;
class Compiler;
class DexFile;
class JniCallingConvention;
class ManagedRegister;
class ManagedRuntimeCallingConvention;
class Method;

// A JNI compiler generates code that acts as the bridge between managed code
// and native code.
// TODO: move the responsibility of managing memory to somewhere else
class JniCompiler {
 public:
  explicit JniCompiler(InstructionSet instruction_set);
  ~JniCompiler();

  CompiledMethod* Compile(uint32_t access_flags, uint32_t method_idx,
                          const ClassLoader* class_loader, const DexFile& dex_file);

 private:
  // Copy a single parameter from the managed to the JNI calling convention
  void CopyParameter(Assembler* jni_asm,
                     ManagedRuntimeCallingConvention* mr_conv,
                     JniCallingConvention* jni_conv,
                     size_t frame_size, size_t out_arg_size);

  void SetNativeParameter(Assembler* jni_asm,
                          JniCallingConvention* jni_conv,
                          ManagedRegister in_reg);

  void ChangeThreadState(Assembler* jni_asm, Thread::State new_state,
                         ManagedRegister scratch, ManagedRegister return_reg,
                         FrameOffset return_save_location,
                         size_t return_size);

  // Architecture to generate code for
  InstructionSet instruction_set_;

  DISALLOW_COPY_AND_ASSIGN(JniCompiler);
};

}  // namespace art

#endif  // ART_SRC_JNI_COMPILER_H_
