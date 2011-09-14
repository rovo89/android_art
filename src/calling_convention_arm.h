// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CALLING_CONVENTION_ARM_H_
#define ART_SRC_CALLING_CONVENTION_ARM_H_

#include "calling_convention.h"

namespace art {
namespace arm {

class ArmManagedRuntimeCallingConvention : public ManagedRuntimeCallingConvention {
 public:
  explicit ArmManagedRuntimeCallingConvention(Method* method) :
                                     ManagedRuntimeCallingConvention(method) {}
  virtual ~ArmManagedRuntimeCallingConvention() {}
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
  DISALLOW_COPY_AND_ASSIGN(ArmManagedRuntimeCallingConvention);
};

class ArmJniCallingConvention : public JniCallingConvention {
 public:
  explicit ArmJniCallingConvention(Method* method);
  virtual ~ArmJniCallingConvention() {}
  // Calling convention
  virtual ManagedRegister ReturnRegister();
  virtual ManagedRegister InterproceduralScratchRegister();
  // JNI calling convention
  virtual size_t FrameSize();
  virtual size_t ReturnPcOffset();
  virtual size_t OutArgSize();
  virtual const std::vector<ManagedRegister>& CalleeSaveRegisters() const {
    return callee_save_regs_;
  }
  virtual uint32_t CoreSpillMask() const {
    return 0x0FF0;  // R4 to R12
  }
  virtual uint32_t FpSpillMask() const {
    return 0;
  }
  virtual bool IsOutArgRegister(ManagedRegister reg);
  virtual bool IsCurrentParamInRegister();
  virtual bool IsCurrentParamOnStack();
  virtual ManagedRegister CurrentParamRegister();
  virtual FrameOffset CurrentParamStackOffset();

 protected:
  virtual size_t NumberOfOutgoingStackArgs();

 private:
  // TODO: these values aren't unique and can be shared amongst instances
  std::vector<ManagedRegister> callee_save_regs_;

  DISALLOW_COPY_AND_ASSIGN(ArmJniCallingConvention);
};

}  // namespace arm
}  // namespace art

#endif  // ART_SRC_CALLING_CONVENTION_ARM_H_
