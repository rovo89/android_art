// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CONTEXT_ARM_H_
#define ART_SRC_CONTEXT_ARM_H_

#include "constants_arm.h"
#include "context.h"

namespace art {
namespace arm {

class ArmContext : public Context {
 public:
  ArmContext();
  virtual ~ArmContext() {}

  virtual void FillCalleeSaves(const Frame& fr);

  virtual void SetSP(uintptr_t new_sp) {
    gprs_[SP] = new_sp;
  }

  virtual void SetPC(uintptr_t new_pc) {
    gprs_[PC] = new_pc;
  }

  virtual void DoLongJump();

 private:
  uintptr_t gprs_[16];
  float fprs_[32];
};

}  // namespace arm
}  // namespace art

#endif  // ART_SRC_CONTEXT_ARM_H_
