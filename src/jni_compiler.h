// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_COMPILER_H_
#define ART_SRC_JNI_COMPILER_H_

#include "constants.h"
#include "macros.h"
#include "object.h"

namespace art {

class Assembler;
class JniCallingConvention;
class ManagedRegister;
class ManagedRuntimeCallingConvention;
class Method;

// A JNI compiler generates code that acts as the bridge between managed code
// and native code.
// TODO: move the responsibility of managing memory to somewhere else
class JniCompiler {
 public:
  explicit JniCompiler(InstructionSet insns);
  ~JniCompiler();

  void Compile(Method* method);

 private:
  // Copy a single parameter from the managed to the JNI calling convention
  void CopyParameter(Assembler* jni_asm,
                     ManagedRuntimeCallingConvention* mr_conv,
                     JniCallingConvention* jni_conv,
                     size_t frame_size, size_t out_arg_size);

  void SetNativeParameter(Assembler* jni_asm,
                          JniCallingConvention* jni_conv,
                          ManagedRegister in_reg);

  InstructionSet instruction_set_;

  ByteArray* jni_stub_;  // Stub to perform native method symbol lookup

  DISALLOW_COPY_AND_ASSIGN(JniCompiler);
};

}  // namespace art

#endif  // ART_SRC_JNI_COMPILER_H_
