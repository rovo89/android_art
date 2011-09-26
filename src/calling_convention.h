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
  bool IsReturnAReference() const { return method_->IsReturnAReference(); }

  size_t SizeOfReturnValue() const { return method_->ReturnSize(); }

  // Register that holds result of this method
  virtual ManagedRegister ReturnRegister() = 0;
  // Register reserved for scratch usage during procedure calls
  virtual ManagedRegister InterproceduralScratchRegister() = 0;

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
    itr_refs_ = 0;
    itr_longs_and_doubles_ = 0;
  }

  virtual ~CallingConvention() {}

 protected:
  explicit CallingConvention(Method* method) : displacement_(0),
                                               method_(method) {}

  Method* GetMethod() const { return method_; }

  // The slot number for current calling_convention argument.
  // Note that each slot is 32-bit. When the current argument is bigger
  // than 32 bits, return the first slot number for this argument.
  unsigned int itr_slots_;
  // The number of references iterated past
  unsigned int itr_refs_;
  // The argument number along argument list for current argument
  unsigned int itr_args_;
  // Number of longs and doubles seen along argument list
  unsigned int itr_longs_and_doubles_;
  // Space for frames below this on the stack
  FrameOffset displacement_;

 private:
  Method* method_;
};

// Abstraction for managed code's calling conventions
// | { Incoming stack args } |
// | { Prior Method* }       | <-- Prior SP
// | { Return address }      |
// | { Callee saves }        |
// | { Spills ... }          |
// | { Outgoing stack args } |
// | { Method* }             | <-- SP
class ManagedRuntimeCallingConvention : public CallingConvention {
 public:
  static ManagedRuntimeCallingConvention* Create(Method* native_method,
                                                InstructionSet instruction_set);

  size_t FrameSize();

  // Register that holds the incoming method argument
  virtual ManagedRegister MethodRegister() = 0;

  // Iterator interface
  bool HasNext();
  void Next();
  bool IsCurrentParamAReference();
  bool IsCurrentArgExplicit();  // ie a non-implict argument such as this
  bool IsCurrentArgPossiblyNull();
  size_t CurrentParamSize();
  virtual bool IsCurrentParamInRegister() = 0;
  virtual bool IsCurrentParamOnStack() = 0;
  virtual ManagedRegister CurrentParamRegister() = 0;
  virtual FrameOffset CurrentParamStackOffset() = 0;

  virtual ~ManagedRuntimeCallingConvention() {}

 protected:
  explicit ManagedRuntimeCallingConvention(Method* method) :
                                           CallingConvention(method) {}
};

// Abstraction for JNI calling conventions
// | { Incoming stack args }         | <-- Prior SP
// | { Return address }              |
// | { Callee saves }                |     ([1])
// | { Return value spill }          |     (live on return slow paths)
// | { Local Ref. Table State }      |
// | { Stack Indirect Ref. Table     |
// |   num. refs./link }             |     (here to prior SP is frame size)
// | { Method* }                     | <-- Anchor SP written to thread
// | { Outgoing stack args }         | <-- SP at point of call
// | Native frame                    |
//
// [1] We must save all callee saves here to enable any exception throws to restore
// callee saves for frames above this one.
class JniCallingConvention : public CallingConvention {
 public:
  static JniCallingConvention* Create(Method* native_method,
                                      InstructionSet instruction_set);

  // Size of frame excluding space for outgoing args (its assumed Method* is
  // always at the bottom of a frame, but this doesn't work for outgoing
  // native args). Includes alignment.
  virtual size_t FrameSize() = 0;
  // Offset within the frame of the return pc
  virtual size_t ReturnPcOffset() = 0;
  // Size of outgoing arguments, including alignment
  virtual size_t OutArgSize() = 0;
  // Number of references in stack indirect reference table
  size_t ReferenceCount() const;
  // Location where the segment state of the local indirect reference table is saved
  FrameOffset SavedLocalReferenceCookieOffset() const;
  // Location where the return value of a call can be squirreled if another
  // call is made following the native call
  FrameOffset ReturnValueSaveLocation() const;

  // Callee save registers to spill prior to native code (which may clobber)
  virtual const std::vector<ManagedRegister>& CalleeSaveRegisters() const = 0;

  // Spill mask values
  virtual uint32_t CoreSpillMask() const = 0;
  virtual uint32_t FpSpillMask() const = 0;

  // Returns true if the method register will have been clobbered during argument
  // set up
  virtual bool IsMethodRegisterClobberedPreCall() = 0;

  // An extra scratch register live after the call
  virtual ManagedRegister ReturnScratchRegister() const = 0;

  // Iterator interface
  bool HasNext();
  virtual void Next();
  bool IsCurrentParamAReference();
  size_t CurrentParamSize();
  virtual bool IsCurrentParamInRegister() = 0;
  virtual bool IsCurrentParamOnStack() = 0;
  virtual ManagedRegister CurrentParamRegister() = 0;
  virtual FrameOffset CurrentParamStackOffset() = 0;

  // Iterator interface extension for JNI
  FrameOffset CurrentParamSirtEntryOffset();

  // Position of SIRT and interior fields
  FrameOffset SirtOffset() const {
    return FrameOffset(displacement_.Int32Value() +
                       kPointerSize);  // above Method*
  }
  FrameOffset SirtNumRefsOffset() const {
    return FrameOffset(SirtOffset().Int32Value() +
                       StackIndirectReferenceTable::NumberOfReferencesOffset());
  }
  FrameOffset SirtLinkOffset() const {
    return FrameOffset(SirtOffset().Int32Value() +
                       StackIndirectReferenceTable::LinkOffset());
  }

  virtual ~JniCallingConvention() {}

 protected:
  // Named iterator positions
  enum IteratorPos {
    kJniEnv = 0,
    kObjectOrClass = 1
  };

  explicit JniCallingConvention(Method* native_method) :
      CallingConvention(native_method) {}

  // Number of stack slots for outgoing arguments, above which the SIRT is
  // located
  virtual size_t NumberOfOutgoingStackArgs() = 0;

 protected:
  static size_t NumberOfExtraArgumentsForJni(Method* method);
};

}  // namespace art

#endif  // ART_SRC_CALLING_CONVENTION_H_
