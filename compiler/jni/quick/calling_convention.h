/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_JNI_QUICK_CALLING_CONVENTION_H_
#define ART_COMPILER_JNI_QUICK_CALLING_CONVENTION_H_

#include <vector>
#include "handle_scope.h"
#include "thread.h"
#include "utils/managed_register.h"

namespace art {

// Top-level abstraction for different calling conventions.
class CallingConvention {
 public:
  bool IsReturnAReference() const { return shorty_[0] == 'L'; }

  Primitive::Type GetReturnType() const {
    return Primitive::GetType(shorty_[0]);
  }

  size_t SizeOfReturnValue() const {
    size_t result = Primitive::ComponentSize(Primitive::GetType(shorty_[0]));
    if (result >= 1 && result < 4) {
      result = 4;
    }
    return result;
  }

  // Register that holds result of this method invocation.
  virtual ManagedRegister ReturnRegister() = 0;
  // Register reserved for scratch usage during procedure calls.
  virtual ManagedRegister InterproceduralScratchRegister() = 0;

  // Offset of Method within the frame.
  FrameOffset MethodStackOffset() {
    return displacement_;
  }

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
    itr_float_and_doubles_ = 0;
  }

  virtual ~CallingConvention() {}

 protected:
  CallingConvention(bool is_static, bool is_synchronized, const char* shorty,
                    size_t frame_pointer_size)
      : itr_slots_(0), itr_refs_(0), itr_args_(0), itr_longs_and_doubles_(0),
        itr_float_and_doubles_(0), displacement_(0),
        frame_pointer_size_(frame_pointer_size),
        handle_scope_pointer_size_(sizeof(StackReference<mirror::Object>)),
        is_static_(is_static), is_synchronized_(is_synchronized),
        shorty_(shorty) {
    num_args_ = (is_static ? 0 : 1) + strlen(shorty) - 1;
    num_ref_args_ = is_static ? 0 : 1;  // The implicit this pointer.
    num_float_or_double_args_ = 0;
    num_long_or_double_args_ = 0;
    for (size_t i = 1; i < strlen(shorty); i++) {
      char ch = shorty_[i];
      switch (ch) {
      case 'L':
        num_ref_args_++;
        break;
      case 'J':
        num_long_or_double_args_++;
        break;
      case 'D':
        num_long_or_double_args_++;
        num_float_or_double_args_++;
        break;
      case 'F':
        num_float_or_double_args_++;
        break;
      }
    }
  }

  bool IsStatic() const {
    return is_static_;
  }
  bool IsSynchronized() const {
    return is_synchronized_;
  }
  bool IsParamALongOrDouble(unsigned int param) const {
    DCHECK_LT(param, NumArgs());
    if (IsStatic()) {
      param++;  // 0th argument must skip return value at start of the shorty
    } else if (param == 0) {
      return false;  // this argument
    }
    char ch = shorty_[param];
    return (ch == 'J' || ch == 'D');
  }
  bool IsParamAFloatOrDouble(unsigned int param) const {
    DCHECK_LT(param, NumArgs());
    if (IsStatic()) {
      param++;  // 0th argument must skip return value at start of the shorty
    } else if (param == 0) {
      return false;  // this argument
    }
    char ch = shorty_[param];
    return (ch == 'F' || ch == 'D');
  }
  bool IsParamADouble(unsigned int param) const {
    DCHECK_LT(param, NumArgs());
    if (IsStatic()) {
      param++;  // 0th argument must skip return value at start of the shorty
    } else if (param == 0) {
      return false;  // this argument
    }
    return shorty_[param] == 'D';
  }
  bool IsParamALong(unsigned int param) const {
    DCHECK_LT(param, NumArgs());
    if (IsStatic()) {
      param++;  // 0th argument must skip return value at start of the shorty
    } else if (param == 0) {
      return true;  // this argument
    }
    return shorty_[param] == 'J';
  }
  bool IsParamAReference(unsigned int param) const {
    DCHECK_LT(param, NumArgs());
    if (IsStatic()) {
      param++;  // 0th argument must skip return value at start of the shorty
    } else if (param == 0) {
      return true;  // this argument
    }
    return shorty_[param] == 'L';
  }
  size_t NumArgs() const {
    return num_args_;
  }
  size_t NumLongOrDoubleArgs() const {
    return num_long_or_double_args_;
  }
  size_t NumFloatOrDoubleArgs() const {
    return num_float_or_double_args_;
  }
  size_t NumReferenceArgs() const {
    return num_ref_args_;
  }
  size_t ParamSize(unsigned int param) const {
    DCHECK_LT(param, NumArgs());
    if (IsStatic()) {
      param++;  // 0th argument must skip return value at start of the shorty
    } else if (param == 0) {
      return frame_pointer_size_;  // this argument
    }
    size_t result = Primitive::ComponentSize(Primitive::GetType(shorty_[param]));
    if (result >= 1 && result < 4) {
      result = 4;
    }
    return result;
  }
  const char* GetShorty() const {
    return shorty_.c_str();
  }
  // The slot number for current calling_convention argument.
  // Note that each slot is 32-bit. When the current argument is bigger
  // than 32 bits, return the first slot number for this argument.
  unsigned int itr_slots_;
  // The number of references iterated past.
  unsigned int itr_refs_;
  // The argument number along argument list for current argument.
  unsigned int itr_args_;
  // Number of longs and doubles seen along argument list.
  unsigned int itr_longs_and_doubles_;
  // Number of float and doubles seen along argument list.
  unsigned int itr_float_and_doubles_;
  // Space for frames below this on the stack.
  FrameOffset displacement_;
  // The size of a reference.
  const size_t frame_pointer_size_;
  // The size of a reference entry within the handle scope.
  const size_t handle_scope_pointer_size_;

 private:
  const bool is_static_;
  const bool is_synchronized_;
  std::string shorty_;
  size_t num_args_;
  size_t num_ref_args_;
  size_t num_float_or_double_args_;
  size_t num_long_or_double_args_;
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
  static ManagedRuntimeCallingConvention* Create(bool is_static, bool is_synchronized,
                                                 const char* shorty,
                                                 InstructionSet instruction_set);

  // Register that holds the incoming method argument
  virtual ManagedRegister MethodRegister() = 0;

  // Iterator interface
  bool HasNext();
  void Next();
  bool IsCurrentParamAReference();
  bool IsCurrentParamAFloatOrDouble();
  bool IsCurrentParamADouble();
  bool IsCurrentParamALong();
  bool IsCurrentArgExplicit();  // ie a non-implict argument such as this
  bool IsCurrentArgPossiblyNull();
  size_t CurrentParamSize();
  virtual bool IsCurrentParamInRegister() = 0;
  virtual bool IsCurrentParamOnStack() = 0;
  virtual ManagedRegister CurrentParamRegister() = 0;
  virtual FrameOffset CurrentParamStackOffset() = 0;

  virtual ~ManagedRuntimeCallingConvention() {}

  // Registers to spill to caller's out registers on entry.
  virtual const ManagedRegisterEntrySpills& EntrySpills() = 0;

 protected:
  ManagedRuntimeCallingConvention(bool is_static, bool is_synchronized, const char* shorty,
                                  size_t frame_pointer_size)
      : CallingConvention(is_static, is_synchronized, shorty, frame_pointer_size) {}
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
  static JniCallingConvention* Create(bool is_static, bool is_synchronized, const char* shorty,
                                      InstructionSet instruction_set);

  // Size of frame excluding space for outgoing args (its assumed Method* is
  // always at the bottom of a frame, but this doesn't work for outgoing
  // native args). Includes alignment.
  virtual size_t FrameSize() = 0;
  // Size of outgoing arguments, including alignment
  virtual size_t OutArgSize() = 0;
  // Number of references in stack indirect reference table
  size_t ReferenceCount() const;
  // Location where the segment state of the local indirect reference table is saved
  FrameOffset SavedLocalReferenceCookieOffset() const;
  // Location where the return value of a call can be squirreled if another
  // call is made following the native call
  FrameOffset ReturnValueSaveLocation() const;
  // Register that holds result if it is integer.
  virtual ManagedRegister IntReturnRegister() = 0;
  // Whether the compiler needs to ensure zero-/sign-extension of a small result type
  virtual bool RequiresSmallResultTypeExtension() const = 0;

  // Callee save registers to spill prior to native code (which may clobber)
  virtual const std::vector<ManagedRegister>& CalleeSaveRegisters() const = 0;

  // Spill mask values
  virtual uint32_t CoreSpillMask() const = 0;
  virtual uint32_t FpSpillMask() const = 0;

  // An extra scratch register live after the call
  virtual ManagedRegister ReturnScratchRegister() const = 0;

  // Iterator interface
  bool HasNext();
  virtual void Next();
  bool IsCurrentParamAReference();
  bool IsCurrentParamAFloatOrDouble();
  bool IsCurrentParamADouble();
  bool IsCurrentParamALong();
  bool IsCurrentParamJniEnv();
  size_t CurrentParamSize();
  virtual bool IsCurrentParamInRegister() = 0;
  virtual bool IsCurrentParamOnStack() = 0;
  virtual ManagedRegister CurrentParamRegister() = 0;
  virtual FrameOffset CurrentParamStackOffset() = 0;

  // Iterator interface extension for JNI
  FrameOffset CurrentParamHandleScopeEntryOffset();

  // Position of handle scope and interior fields
  FrameOffset HandleScopeOffset() const {
    return FrameOffset(this->displacement_.Int32Value() + frame_pointer_size_);  // above Method*
  }

  FrameOffset HandleScopeLinkOffset() const {
    return FrameOffset(HandleScopeOffset().Int32Value() + HandleScope::LinkOffset(frame_pointer_size_));
  }

  FrameOffset HandleScopeNumRefsOffset() const {
    return FrameOffset(HandleScopeOffset().Int32Value() +
                       HandleScope::NumberOfReferencesOffset(frame_pointer_size_));
  }

  FrameOffset HandleerencesOffset() const {
    return FrameOffset(HandleScopeOffset().Int32Value() +
                       HandleScope::ReferencesOffset(frame_pointer_size_));
  }

  virtual ~JniCallingConvention() {}

 protected:
  // Named iterator positions
  enum IteratorPos {
    kJniEnv = 0,
    kObjectOrClass = 1
  };

  explicit JniCallingConvention(bool is_static, bool is_synchronized, const char* shorty,
                                size_t frame_pointer_size)
      : CallingConvention(is_static, is_synchronized, shorty, frame_pointer_size) {}

  // Number of stack slots for outgoing arguments, above which the handle scope is
  // located
  virtual size_t NumberOfOutgoingStackArgs() = 0;

 protected:
  size_t NumberOfExtraArgumentsForJni();
};

}  // namespace art

#endif  // ART_COMPILER_JNI_QUICK_CALLING_CONVENTION_H_
