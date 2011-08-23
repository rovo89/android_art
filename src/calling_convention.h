// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CALLING_CONVENTION_H_
#define ART_SRC_CALLING_CONVENTION_H_

#include <vector>
#include "managed_register.h"
#include "object.h"
#include "thread.h"

namespace art {

// Top-level abstraction for different calling conventions
class CallingConvention {
 public:
  CallingConvention* GetCallingConvention(Method* method);

  bool IsReturnAReference() const { return method_->IsReturnAReference(); }

  size_t SizeOfReturnValue() const { return method_->ReturnSize(); }

  // Register that holds the incoming method argument
  ManagedRegister MethodRegister();
  // Register that holds result of this method
  ManagedRegister ReturnRegister();
  // Register reserved for scratch usage during procedure calls
  ManagedRegister InterproceduralScratchRegister();

  // Offset of Method within the frame
  FrameOffset MethodStackOffset();

  // Iterator interface

  // Place iterator at start of arguments. The displacement is applied to
  // frame offset methods to account for frames which may be on the stack
  // below the one being iterated over.
  void ResetIterator(FrameOffset displacement) {
    displacement_ = displacement;
    itr_slots_ = 0;
    itr_args_ = 0;
    itr_longs_and_doubles_ = 0;
  }

 protected:
  explicit CallingConvention(Method* method) : displacement_(0),
                                               method_(method) {}
  const Method* GetMethod() const { return method_; }

  // The slot number for current calling_convention argument.
  // Note that each slot is 32-bit. When the current argument is bigger
  // than 32 bits, return the first slot number for this argument.
  unsigned int itr_slots_;
  // The argument number along argument list for current argument
  unsigned int itr_args_;
  // Number of longs and doubles seen along argument list
  unsigned int itr_longs_and_doubles_;
  // Space for frames below this on the stack
  FrameOffset displacement_;

 private:
  const Method* method_;
};

// Abstraction for managed code's calling conventions
class ManagedRuntimeCallingConvention : public CallingConvention {
 public:
  explicit ManagedRuntimeCallingConvention(Method* method) :
                                          CallingConvention(method) {}

  size_t FrameSize();

  // Iterator interface
  bool HasNext();
  void Next();
  bool IsCurrentParamAReference();
  bool IsCurrentParamInRegister();
  bool IsCurrentParamOnStack();
  bool IsCurrentUserArg();
  size_t CurrentParamSize();
  ManagedRegister CurrentParamRegister();
  FrameOffset CurrentParamStackOffset();

  DISALLOW_COPY_AND_ASSIGN(ManagedRuntimeCallingConvention);
};

// Abstraction for JNI calling conventions
// | incoming stack args    | <-- Prior SP
// | { Return address }     |     (x86)
// | { Return value spill } |     (live on return slow paths)
// | { Stack Handle Block   |
// |   ...                  |
// |   num. refs./link }    |     (here to prior SP is frame size)
// | { Spill area }         |     (ARM)
// | Method*                | <-- Anchor SP written to thread
// | { Outgoing stack args  |
// |   ... }                | <-- SP at point of call
// | Native frame           |
class JniCallingConvention : public CallingConvention {
 public:
  explicit JniCallingConvention(Method* native_method) :
                      CallingConvention(native_method),
                      spill_regs_(ComputeRegsToSpillPreCall()) {}

  // Size of frame excluding space for outgoing args (its assumed Method* is
  // always at the bottom of a frame, but this doesn't work for outgoing
  // native args). Includes alignment.
  size_t FrameSize();
  // Offset within the frame of the return pc
  size_t ReturnPcOffset();
  // Size of outgoing arguments, including alignment
  size_t OutArgSize();
  // Number of handles in stack handle block
  size_t HandleCount();
  // Size of area used to hold spilled registers
  size_t SpillAreaSize();
  // Location where the return value of a call can be squirreled if another
  // call is made following the native call
  FrameOffset ReturnValueSaveLocation();

  // Registers that must be spilled (due to clobbering) before the call into
  // the native routine
  const std::vector<ManagedRegister>& RegsToSpillPreCall() {
    return *spill_regs_.get();
  }

  // Returns true if the register will be clobbered by an outgoing
  // argument value.
  bool IsOutArgRegister(ManagedRegister reg);

  // Iterator interface
  bool HasNext();
  void Next();
  bool IsCurrentParamAReference();
  bool IsCurrentParamInRegister();
  bool IsCurrentParamOnStack();
  size_t CurrentParamSize();
  ManagedRegister CurrentParamRegister();
  FrameOffset CurrentParamStackOffset();

  // Iterator interface extension for JNI
  FrameOffset CurrentParamHandleOffset();

  // Position of stack handle block and interior fields
  FrameOffset ShbOffset() {
    return FrameOffset(displacement_.Int32Value() +
                       SpillAreaSize() +
                       kPointerSize);  // above Method*
  }
  FrameOffset ShbNumRefsOffset() {
    return FrameOffset(ShbOffset().Int32Value() +
                       StackHandleBlock::NumberOfReferencesOffset());
  }
  FrameOffset ShbLinkOffset() {
    return FrameOffset(ShbOffset().Int32Value() +
                       StackHandleBlock::LinkOffset());
  }

 private:
  // Named iterator positions
  enum IteratorPos {
    kJniEnv = 0,
    kObjectOrClass = 1
  };

  // Number of stack slots for outgoing arguments, above which handles are
  // located
  size_t NumberOfOutgoingStackArgs();

  // Compute registers for RegsToSpillPreCall
  std::vector<ManagedRegister>* ComputeRegsToSpillPreCall();

  // Extra registers to spill before the call into native
  const scoped_ptr<std::vector<ManagedRegister> > spill_regs_;

  static size_t NumberOfExtraArgumentsForJni(const Method* method);
  DISALLOW_COPY_AND_ASSIGN(JniCallingConvention);
};

}  // namespace art

#endif  // ART_SRC_CALLING_CONVENTION_H_
