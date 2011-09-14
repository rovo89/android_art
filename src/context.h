// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CONTEXT_H_
#define ART_SRC_CONTEXT_H_

#include <stdint.h>

namespace art {

class Frame;

// Representation of a thread's context on the executing machine
class Context {
 public:
  // Creates a context for the running architecture
  static Context* Create();

  virtual ~Context() {}

  // Read values from callee saves in the given frame. The frame also holds
  // the method that holds the layout.
  virtual void FillCalleeSaves(const Frame& fr) = 0;

  // Set the stack pointer value
  virtual void SetSP(uintptr_t new_sp) = 0;

  // Set the program counter value
  virtual void SetPC(uintptr_t new_pc) = 0;

  // Switch execution of the executing context to this context
  virtual void DoLongJump() = 0;
};

}  // namespace art

#endif  // ART_SRC_CONTEXT_H_
