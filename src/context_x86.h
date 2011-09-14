// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CONTEXT_X86_H_
#define ART_SRC_CONTEXT_X86_H_

#include "context.h"

#include "constants_x86.h"

namespace art {
namespace x86 {

class X86Context : public Context {
 public:
  X86Context();
  virtual ~X86Context() {}

  // No callee saves on X86
  virtual void FillCalleeSaves(const Frame& fr);

  virtual void SetSP(uintptr_t new_sp) {
    gprs_[ESP] = new_sp;
  }

  virtual void SetPC(uintptr_t new_pc) {
    eip_ = new_pc;
  }

  virtual void DoLongJump();

 private:
  uintptr_t gprs_[8];
  uintptr_t eip_;
};
}  // namespace x86
}  // namespace art

#endif  // ART_SRC_CONTEXT_X86_H_
