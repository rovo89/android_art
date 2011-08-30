// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_COMPILER_H_
#define ART_SRC_JNI_COMPILER_H_

#include "UniquePtr.h"
#include "calling_convention.h"
#include "globals.h"
#include "macros.h"
#include "mem_map.h"

namespace art {

class Assembler;
class Method;

// A JNI compiler generates code that acts as the bridge between managed code
// and native code.
// TODO: move the responsibility of managing memory to somewhere else
class JniCompiler {
 public:
  JniCompiler();
  ~JniCompiler();
  void Compile(Assembler* jni_asm, Method* method);

 private:
  // Copy a single parameter from the managed to the JNI calling convention
  void CopyParameter(Assembler* jni_asm,
                     ManagedRuntimeCallingConvention* mr_conv,
                     JniCallingConvention* jni_conv,
                     size_t frame_size, size_t out_arg_size);

  // A poor man's code cache
  void* AllocateCode(size_t size);

  // Allocated code
  UniquePtr<MemMap> jni_code_;

  // Pointer to the free space
  byte* jni_code_top_;

  DISALLOW_COPY_AND_ASSIGN(JniCompiler);
};

}  // namespace art

#endif  // ART_SRC_JNI_COMPILER_H_
