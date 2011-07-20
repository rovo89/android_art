// Copyright 2011 Google Inc. All Rights Reserved.
// Author: irogers@google.com (Ian Rogers)

#ifndef ART_SRC_CALLING_CONVENTION_H_
#define ART_SRC_CALLING_CONVENTION_H_

#include "src/managed_register.h"
#include "src/object.h"
#include "src/thread.h"

namespace art {

// Top-level abstraction for different calling conventions
class CallingConvention {
 public:
  CallingConvention* GetCallingConvention(Method* method);

  bool IsReturnAReference() const { return method_->IsReturnAReference(); }

  // Register that holds the incoming method argument
  ManagedRegister MethodRegister();
  // Register that holds result of this method
  ManagedRegister ReturnRegister();
  // Register reserved for scratch usage during procedure calls
  ManagedRegister InterproceduralScratchRegister();

  // Iterator interface

  // Place iterator at start of arguments. The displacement is applied to
  // frame offset methods to account for frames which may be on the stack
  // below the one being iterated over.
  void ResetIterator(FrameOffset displacement) {
    displacement_ = displacement;
    itr_position_ = 0;
    itr_longs_and_doubles_ = 0;
  }

 protected:
  explicit CallingConvention(Method* method) : displacement_(0),
                                               method_(method) {}
  const Method* GetMethod() const { return method_; }

  // position along argument list
  unsigned int itr_position_;
  // number of longs and doubles seen along argument list
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
  bool IsCurrentParamPossiblyNull();
  size_t CurrentParamSizeInBytes();
  ManagedRegister CurrentParamRegister();
  FrameOffset CurrentParamStackOffset();

  DISALLOW_COPY_AND_ASSIGN(ManagedRuntimeCallingConvention);
};

// Abstraction for JNI calling conventions
// | incoming stack args    | <-- Prior SP
// | { Spilled registers    |
// |   & return address }   |
// | { Saved JNI Env Data } |
// | { Stack Handle Block   |
// |   ...                  |
// |   length/link }        |     (here to prior SP is frame size)
// | Method*                | <-- Anchor SP written to thread
// | { Outgoing stack args  |
// |   ... }                | <-- SP at point of call
// | Native frame           |
class JniCallingConvention : public CallingConvention {
 public:
  explicit JniCallingConvention(Method* native_method) :
                      CallingConvention(native_method) {}

  // Size of frame excluding space for outgoing args (its assumed Method* is
  // always at the bottom of a frame, but this doesn't work for outgoing
  // native args). Includes alignment.
  size_t FrameSize();
  // Size of outgoing arguments, including alignment
  size_t OutArgSize();
  // Number of handles in stack handle block
  size_t HandleCount();

  // Iterator interface
  bool HasNext();
  void Next();
  bool IsCurrentParamAReference();
  bool IsCurrentParamInRegister();
  bool IsCurrentParamOnStack();
  size_t CurrentParamSizeInBytes();
  ManagedRegister CurrentParamRegister();
  FrameOffset CurrentParamStackOffset();

  // Iterator interface extension for JNI
  FrameOffset CurrentParamHandleOffset();

  // Position of stack handle block and interior fields
  FrameOffset ShbOffset() {
    return FrameOffset(displacement_.Int32Value() +
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

  DISALLOW_COPY_AND_ASSIGN(JniCallingConvention);
};

}  // namespace art

#endif  // ART_SRC_CALLING_CONVENTION_H_
