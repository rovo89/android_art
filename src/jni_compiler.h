// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_COMPILER_H_
#define ART_SRC_JNI_COMPILER_H_

#include "compiled_method.h"
#include "constants.h"
#include "macros.h"
#include "object.h"

namespace art {

class Assembler;
class Compiler;
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

  CompiledMethod* Compile(bool is_direct, uint32_t method_idx, const ClassLoader* class_loader,
                          const DexFile& dex_file);

  // Stub to perform native method symbol lookup via dlsym
  // TODO: remove from JniCompiler
  static ByteArray* CreateJniStub(InstructionSet instruction_set);

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
