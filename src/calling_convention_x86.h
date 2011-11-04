// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CALLING_CONVENTION_X86_H_
#define ART_SRC_CALLING_CONVENTION_X86_H_

#include "calling_convention.h"

namespace art {
namespace x86 {

class X86ManagedRuntimeCallingConvention : public ManagedRuntimeCallingConvention {
 public:
  explicit X86ManagedRuntimeCallingConvention(const Method* method) :
                                     ManagedRuntimeCallingConvention(method) {}
  virtual ~X86ManagedRuntimeCallingConvention() {}
  // Calling convention
  virtual ManagedRegister ReturnRegister();
  virtual ManagedRegister InterproceduralScratchRegister();
  // Managed runtime calling convention
  virtual ManagedRegister MethodRegister();
  virtual bool IsCurrentParamInRegister();
  virtual bool IsCurrentParamOnStack();
  virtual ManagedRegister CurrentParamRegister();
  virtual FrameOffset CurrentParamStackOffset();

 private:
  DISALLOW_COPY_AND_ASSIGN(X86ManagedRuntimeCallingConvention);
};

class X86JniCallingConvention : public JniCallingConvention {
 public:
  explicit X86JniCallingConvention(const Method* method) :
                                   JniCallingConvention(method) {}
  virtual ~X86JniCallingConvention() {}
  // Calling convention
  virtual ManagedRegister ReturnRegister();
  virtual ManagedRegister InterproceduralScratchRegister();
  // JNI calling convention
  virtual size_t FrameSize();
  virtual size_t OutArgSize();
  virtual const std::vector<ManagedRegister>& CalleeSaveRegisters() const {
    DCHECK(callee_save_regs_.empty());
    return callee_save_regs_;
  }
  virtual ManagedRegister ReturnScratchRegister() const;
  virtual uint32_t CoreSpillMask() const {
    return 0;
  }
  virtual uint32_t FpSpillMask() const {
    return 0;
  }
  virtual bool IsMethodRegisterClobberedPreCall();
  virtual bool IsCurrentParamInRegister();
  virtual bool IsCurrentParamOnStack();
  virtual ManagedRegister CurrentParamRegister();
  virtual FrameOffset CurrentParamStackOffset();

 protected:
  virtual size_t NumberOfOutgoingStackArgs();

 private:
  static std::vector<ManagedRegister> callee_save_regs_;

  DISALLOW_COPY_AND_ASSIGN(X86JniCallingConvention);
};

}  // namespace x86
}  // namespace art

#endif  // ART_SRC_CALLING_CONVENTION_X86_H_
