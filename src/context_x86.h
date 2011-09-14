// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CONTEXT_X86_H_
#define ART_SRC_CONTEXT_X86_H_

#include "context.h"

namespace art {
namespace x86 {

class X86Context : public Context {
 public:
  X86Context() : esp_(0), eip_(0) {}
  virtual ~X86Context() {}

  // No callee saves on X86
  virtual void FillCalleeSaves(const Frame& fr) {}

  virtual void SetSP(uintptr_t new_sp) {
    esp_ = new_sp;
  }

  virtual void SetPC(uintptr_t new_pc) {
    eip_ = new_pc;
  }

  virtual void DoLongJump();

 private:
  // Currently just ESP and EIP are used
  uintptr_t esp_;
  uintptr_t eip_;
};
}  // namespace x86
}  // namespace art

#endif  // ART_SRC_CONTEXT_X86_H_
