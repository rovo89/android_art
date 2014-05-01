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

#ifndef ART_COMPILER_JNI_QUICK_X86_64_CALLING_CONVENTION_X86_64_H_
#define ART_COMPILER_JNI_QUICK_X86_64_CALLING_CONVENTION_X86_64_H_

#include "jni/quick/calling_convention.h"

namespace art {
namespace x86_64 {

constexpr size_t kFramePointerSize = 8;

class X86_64ManagedRuntimeCallingConvention FINAL : public ManagedRuntimeCallingConvention {
 public:
  explicit X86_64ManagedRuntimeCallingConvention(bool is_static, bool is_synchronized,
                                              const char* shorty)
      : ManagedRuntimeCallingConvention(is_static, is_synchronized, shorty, kFramePointerSize) {}
  ~X86_64ManagedRuntimeCallingConvention() OVERRIDE {}
  // Calling convention
  ManagedRegister ReturnRegister() OVERRIDE;
  ManagedRegister InterproceduralScratchRegister() OVERRIDE;
  // Managed runtime calling convention
  ManagedRegister MethodRegister() OVERRIDE;
  bool IsCurrentParamInRegister() OVERRIDE;
  bool IsCurrentParamOnStack() OVERRIDE;
  ManagedRegister CurrentParamRegister() OVERRIDE;
  FrameOffset CurrentParamStackOffset() OVERRIDE;
  const ManagedRegisterEntrySpills& EntrySpills() OVERRIDE;
 private:
  ManagedRegisterEntrySpills entry_spills_;
  DISALLOW_COPY_AND_ASSIGN(X86_64ManagedRuntimeCallingConvention);
};

class X86_64JniCallingConvention FINAL : public JniCallingConvention {
 public:
  explicit X86_64JniCallingConvention(bool is_static, bool is_synchronized, const char* shorty);
  ~X86_64JniCallingConvention() OVERRIDE {}
  // Calling convention
  ManagedRegister ReturnRegister() OVERRIDE;
  ManagedRegister IntReturnRegister() OVERRIDE;
  ManagedRegister InterproceduralScratchRegister() OVERRIDE;
  // JNI calling convention
  size_t FrameSize() OVERRIDE;
  size_t OutArgSize() OVERRIDE;
  const std::vector<ManagedRegister>& CalleeSaveRegisters() const OVERRIDE {
    return callee_save_regs_;
  }
  ManagedRegister ReturnScratchRegister() const OVERRIDE;
  uint32_t CoreSpillMask() const OVERRIDE;
  uint32_t FpSpillMask() const OVERRIDE {
    return 0;
  }
  bool IsCurrentParamInRegister() OVERRIDE;
  bool IsCurrentParamOnStack() OVERRIDE;
  ManagedRegister CurrentParamRegister() OVERRIDE;
  FrameOffset CurrentParamStackOffset() OVERRIDE;

  // x86-64 needs to extend small return types.
  bool RequiresSmallResultTypeExtension() const OVERRIDE {
    return true;
  }

 protected:
  size_t NumberOfOutgoingStackArgs() OVERRIDE;

 private:
  // TODO: these values aren't unique and can be shared amongst instances
  std::vector<ManagedRegister> callee_save_regs_;

  DISALLOW_COPY_AND_ASSIGN(X86_64JniCallingConvention);
};

}  // namespace x86_64
}  // namespace art

#endif  // ART_COMPILER_JNI_QUICK_X86_64_CALLING_CONVENTION_X86_64_H_
