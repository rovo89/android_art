/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "base/logging.h"
#include "calling_convention_arm64.h"
#include "utils/arm64/managed_register_arm64.h"

namespace art {
namespace arm64 {

// Calling convention

ManagedRegister Arm64ManagedRuntimeCallingConvention::InterproceduralScratchRegister() {
  return Arm64ManagedRegister::FromCoreRegister(IP0);  // X16
}

ManagedRegister Arm64JniCallingConvention::InterproceduralScratchRegister() {
  return Arm64ManagedRegister::FromCoreRegister(IP0);  // X16
}

static ManagedRegister ReturnRegisterForShorty(const char* shorty) {
  if (shorty[0] == 'F') {
    return Arm64ManagedRegister::FromSRegister(S0);
  } else if (shorty[0] == 'D') {
    return Arm64ManagedRegister::FromDRegister(D0);
  } else if (shorty[0] == 'J') {
    return Arm64ManagedRegister::FromCoreRegister(X0);
  } else if (shorty[0] == 'V') {
    return Arm64ManagedRegister::NoRegister();
  } else {
    return Arm64ManagedRegister::FromWRegister(W0);
  }
}

ManagedRegister Arm64ManagedRuntimeCallingConvention::ReturnRegister() {
  return ReturnRegisterForShorty(GetShorty());
}

ManagedRegister Arm64JniCallingConvention::ReturnRegister() {
  return ReturnRegisterForShorty(GetShorty());
}

ManagedRegister Arm64JniCallingConvention::IntReturnRegister() {
  return Arm64ManagedRegister::FromWRegister(W0);
}

// Managed runtime calling convention

ManagedRegister Arm64ManagedRuntimeCallingConvention::MethodRegister() {
  return Arm64ManagedRegister::FromCoreRegister(X0);
}

bool Arm64ManagedRuntimeCallingConvention::IsCurrentParamInRegister() {
  return false;  // Everything moved to stack on entry.
}

bool Arm64ManagedRuntimeCallingConvention::IsCurrentParamOnStack() {
  return true;
}

ManagedRegister Arm64ManagedRuntimeCallingConvention::CurrentParamRegister() {
  LOG(FATAL) << "Should not reach here";
  return ManagedRegister::NoRegister();
}

FrameOffset Arm64ManagedRuntimeCallingConvention::CurrentParamStackOffset() {
  CHECK(IsCurrentParamOnStack());
  FrameOffset result =
      FrameOffset(displacement_.Int32Value() +         // displacement
                  kFramePointerSize +                 // Method*
                  (itr_slots_ * kFramePointerSize));  // offset into in args
  return result;
}

const ManagedRegisterEntrySpills& Arm64ManagedRuntimeCallingConvention::EntrySpills() {
  // We spill the argument registers on ARM64 to free them up for scratch use, we then assume
  // all arguments are on the stack.
  if (entry_spills_.size() == 0) {
    // TODO Need fp regs spilled too.
    //
    size_t num_spills = NumArgs();

    // TODO Floating point need spilling too.
    if (num_spills > 0) {
      entry_spills_.push_back(Arm64ManagedRegister::FromCoreRegister(X1));
      if (num_spills > 1) {
        entry_spills_.push_back(Arm64ManagedRegister::FromCoreRegister(X2));
        if (num_spills > 2) {
          entry_spills_.push_back(Arm64ManagedRegister::FromCoreRegister(X3));
          if (num_spills > 3) {
            entry_spills_.push_back(Arm64ManagedRegister::FromCoreRegister(X5));
            if (num_spills > 4) {
              entry_spills_.push_back(Arm64ManagedRegister::FromCoreRegister(X6));
              if (num_spills > 5) {
                entry_spills_.push_back(Arm64ManagedRegister::FromCoreRegister(X7));
              }
            }
          }
        }
      }
    }
  }

  return entry_spills_;
}
// JNI calling convention

Arm64JniCallingConvention::Arm64JniCallingConvention(bool is_static, bool is_synchronized,
                                                     const char* shorty)
    : JniCallingConvention(is_static, is_synchronized, shorty, kFramePointerSize) {
  // TODO This needs to be converted to 64bit.
  // Compute padding to ensure longs and doubles are not split in AAPCS. Ignore the 'this' jobject
  // or jclass for static methods and the JNIEnv. We start at the aligned register r2.
//  size_t padding = 0;
//  for (size_t cur_arg = IsStatic() ? 0 : 1, cur_reg = 2; cur_arg < NumArgs(); cur_arg++) {
//    if (IsParamALongOrDouble(cur_arg)) {
//      if ((cur_reg & 1) != 0) {
//        padding += 4;
//        cur_reg++;  // additional bump to ensure alignment
//      }
//      cur_reg++;  // additional bump to skip extra long word
//    }
//    cur_reg++;  // bump the iterator for every argument
//  }
  padding_ =0;

  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X19));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X20));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X21));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X22));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X23));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X24));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X25));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X26));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X27));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X28));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X29));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromCoreRegister(X30));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromDRegister(D8));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromDRegister(D9));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromDRegister(D10));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromDRegister(D11));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromDRegister(D12));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromDRegister(D13));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromDRegister(D14));
  callee_save_regs_.push_back(Arm64ManagedRegister::FromDRegister(D15));
}

uint32_t Arm64JniCallingConvention::CoreSpillMask() const {
  // Compute spill mask to agree with callee saves initialized in the constructor
  uint32_t result = 0;
  result =  1 << X19 | 1 << X20 | 1 << X21 | 1 << X22 | 1 << X23 | 1 << X24 | 1 << X25
      | 1 << X26 | 1 << X27 | 1 << X28 | 1<< X29 | 1 << LR;
  return result;
}

ManagedRegister Arm64JniCallingConvention::ReturnScratchRegister() const {
  return Arm64ManagedRegister::FromCoreRegister(X9);
}

size_t Arm64JniCallingConvention::FrameSize() {
  // Method*, LR and callee save area size, local reference segment state
  size_t frame_data_size = (3 + CalleeSaveRegisters().size()) * kFramePointerSize;
  // References plus 2 words for SIRT header
  size_t sirt_size = StackIndirectReferenceTable::GetAlignedSirtSizeTarget(kFramePointerSize, ReferenceCount());
  // Plus return value spill area size
  return RoundUp(frame_data_size + sirt_size + SizeOfReturnValue(), kStackAlignment);
}

size_t Arm64JniCallingConvention::OutArgSize() {
  return RoundUp(NumberOfOutgoingStackArgs() * kFramePointerSize + padding_,
                 kStackAlignment);
}

// JniCallingConvention ABI follows AAPCS where longs and doubles must occur
// in even register numbers and stack slots
void Arm64JniCallingConvention::Next() {
  JniCallingConvention::Next();
  size_t arg_pos = itr_args_ - NumberOfExtraArgumentsForJni();
  if ((itr_args_ >= 2) &&
      (arg_pos < NumArgs()) &&
      IsParamALongOrDouble(arg_pos)) {
    // itr_slots_ needs to be an even number, according to AAPCS.
    if ((itr_slots_ & 0x1u) != 0) {
      itr_slots_++;
    }
  }
}

bool Arm64JniCallingConvention::IsCurrentParamInRegister() {
  return itr_slots_ < 4;
}

bool Arm64JniCallingConvention::IsCurrentParamOnStack() {
  return !IsCurrentParamInRegister();
}

// TODO and floating point?

static const Register kJniArgumentRegisters[] = {
  X0, X1, X2, X3, X4, X5, X6, X7
};
ManagedRegister Arm64JniCallingConvention::CurrentParamRegister() {
  CHECK_LT(itr_slots_, 4u);
  int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni();
  // TODO Floating point & 64bit registers.
  if ((itr_args_ >= 2) && IsParamALongOrDouble(arg_pos)) {
    CHECK_EQ(itr_slots_, 2u);
    return Arm64ManagedRegister::FromCoreRegister(X1);
  } else {
    return
      Arm64ManagedRegister::FromCoreRegister(kJniArgumentRegisters[itr_slots_]);
  }
}

FrameOffset Arm64JniCallingConvention::CurrentParamStackOffset() {
  CHECK_GE(itr_slots_, 4u);
  size_t offset = displacement_.Int32Value() - OutArgSize() + ((itr_slots_ - 4) * kFramePointerSize);
  CHECK_LT(offset, OutArgSize());
  return FrameOffset(offset);
}

size_t Arm64JniCallingConvention::NumberOfOutgoingStackArgs() {
  size_t static_args = IsStatic() ? 1 : 0;  // count jclass
  // regular argument parameters and this
  size_t param_args = NumArgs() + NumLongOrDoubleArgs();
  // count JNIEnv* less arguments in registers
  return static_args + param_args + 1 - 4;
}

}  // namespace arm64
}  // namespace art
