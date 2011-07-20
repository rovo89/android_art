// Copyright 2011 Google Inc. All Rights Reserved.
// Author: irogers@google.com (Ian Rogers)
#ifndef ART_SRC_JNI_COMPILER_H_
#define ART_SRC_JNI_COMPILER_H_

#include "globals.h"
#include "macros.h"

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
  // A poor man's code cache
  void* AllocateCode(size_t size);

  // Base of memory region for allocated code
  byte* jni_code_;

  // Allocated code size
  size_t jni_code_size_;

  // Pointer to the free space
  byte* jni_code_top_;

  DISALLOW_COPY_AND_ASSIGN(JniCompiler);
};

}  // namespace art

#endif  // ART_SRC_JNI_COMPILER_H_
