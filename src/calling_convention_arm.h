// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CALLING_CONVENTION_ARM_H_
#define ART_SRC_CALLING_CONVENTION_ARM_H_

#include "calling_convention.h"

namespace art {
namespace arm {

class ArmManagedRuntimeCallingConvention : public ManagedRuntimeCallingConvention {
 public:
  ArmManagedRuntimeCallingConvention(bool is_static, bool is_synchronized, const char* shorty) :
      ManagedRuntimeCallingConvention(is_static, is_synchronized, shorty) {}
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
  explicit ArmJniCallingConvention(bool is_static, bool is_synchronized, const char* shorty);
  virtual ~ArmJniCallingConvention() {}
  // Calling convention
  virtual ManagedRegister ReturnRegister();
  virtual ManagedRegister InterproceduralScratchRegister();
  // JNI calling convention
  virtual void Next();  // Override default behavior for AAPCS
  virtual size_t FrameSize();
  virtual size_t OutArgSize();
  virtual const std::vector<ManagedRegister>& CalleeSaveRegisters() const {
    return callee_save_regs_;
  }
  virtual ManagedRegister ReturnScratchRegister() const;
  virtual uint32_t CoreSpillMask() const;
  virtual uint32_t FpSpillMask() const {
    return 0;  // Floats aren't spilled in JNI down call
  }
  virtual bool IsMethodRegisterClobberedPreCall();
  virtual bool IsCurrentParamInRegister();
  virtual bool IsCurrentParamOnStack();
  virtual ManagedRegister CurrentParamRegister();
  virtual FrameOffset CurrentParamStackOffset();

 protected:
  virtual size_t NumberOfOutgoingStackArgs();

 private:
  // TODO: these values aren't unique and can be shared amongst instances
  std::vector<ManagedRegister> callee_save_regs_;

  // Padding to ensure longs and doubles are not split in AAPCS
  size_t padding_;

  DISALLOW_COPY_AND_ASSIGN(ArmJniCallingConvention);
};

}  // namespace arm
}  // namespace art

#endif  // ART_SRC_CALLING_CONVENTION_ARM_H_
