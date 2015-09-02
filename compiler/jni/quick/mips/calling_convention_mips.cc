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

#include "calling_convention_mips.h"

#include "base/logging.h"
#include "handle_scope-inl.h"
#include "utils/mips/managed_register_mips.h"

namespace art {
namespace mips {

static const Register kCoreArgumentRegisters[] = { A0, A1, A2, A3 };
static const FRegister kFArgumentRegisters[] = { F12, F14 };
static const DRegister kDArgumentRegisters[] = { D6, D7 };

// Calling convention
ManagedRegister MipsManagedRuntimeCallingConvention::InterproceduralScratchRegister() {
  return MipsManagedRegister::FromCoreRegister(T9);
}

ManagedRegister MipsJniCallingConvention::InterproceduralScratchRegister() {
  return MipsManagedRegister::FromCoreRegister(T9);
}

static ManagedRegister ReturnRegisterForShorty(const char* shorty) {
  if (shorty[0] == 'F') {
    return MipsManagedRegister::FromFRegister(F0);
  } else if (shorty[0] == 'D') {
    return MipsManagedRegister::FromDRegister(D0);
  } else if (shorty[0] == 'J') {
    return MipsManagedRegister::FromRegisterPair(V0_V1);
  } else if (shorty[0] == 'V') {
    return MipsManagedRegister::NoRegister();
  } else {
    return MipsManagedRegister::FromCoreRegister(V0);
  }
}

ManagedRegister MipsManagedRuntimeCallingConvention::ReturnRegister() {
  return ReturnRegisterForShorty(GetShorty());
}

ManagedRegister MipsJniCallingConvention::ReturnRegister() {
  return ReturnRegisterForShorty(GetShorty());
}

ManagedRegister MipsJniCallingConvention::IntReturnRegister() {
  return MipsManagedRegister::FromCoreRegister(V0);
}

// Managed runtime calling convention

ManagedRegister MipsManagedRuntimeCallingConvention::MethodRegister() {
  return MipsManagedRegister::FromCoreRegister(A0);
}

bool MipsManagedRuntimeCallingConvention::IsCurrentParamInRegister() {
  return false;  // Everything moved to stack on entry.
}

bool MipsManagedRuntimeCallingConvention::IsCurrentParamOnStack() {
  return true;
}

ManagedRegister MipsManagedRuntimeCallingConvention::CurrentParamRegister() {
  LOG(FATAL) << "Should not reach here";
  return ManagedRegister::NoRegister();
}

FrameOffset MipsManagedRuntimeCallingConvention::CurrentParamStackOffset() {
  CHECK(IsCurrentParamOnStack());
  FrameOffset result =
      FrameOffset(displacement_.Int32Value() +        // displacement
                  kFramePointerSize +                 // Method*
                  (itr_slots_ * kFramePointerSize));  // offset into in args
  return result;
}

const ManagedRegisterEntrySpills& MipsManagedRuntimeCallingConvention::EntrySpills() {
  // We spill the argument registers on MIPS to free them up for scratch use, we then assume
  // all arguments are on the stack.
  if ((entry_spills_.size() == 0) && (NumArgs() > 0)) {
    uint32_t gpr_index = 1;  // Skip A0, it is used for ArtMethod*.
    uint32_t fpr_index = 0;

    for (ResetIterator(FrameOffset(0)); HasNext(); Next()) {
      if (IsCurrentParamAFloatOrDouble()) {
        if (IsCurrentParamADouble()) {
          if (fpr_index < arraysize(kDArgumentRegisters)) {
            entry_spills_.push_back(
                MipsManagedRegister::FromDRegister(kDArgumentRegisters[fpr_index++]));
          } else {
            entry_spills_.push_back(ManagedRegister::NoRegister(), 8);
          }
        } else {
          if (fpr_index < arraysize(kFArgumentRegisters)) {
            entry_spills_.push_back(
                MipsManagedRegister::FromFRegister(kFArgumentRegisters[fpr_index++]));
          } else {
            entry_spills_.push_back(ManagedRegister::NoRegister(), 4);
          }
        }
      } else {
        if (IsCurrentParamALong() && !IsCurrentParamAReference()) {
          if (gpr_index == 1) {
            // Don't use a1-a2 as a register pair, move to a2-a3 instead.
            gpr_index++;
          }
          if (gpr_index < arraysize(kCoreArgumentRegisters) - 1) {
            entry_spills_.push_back(
                MipsManagedRegister::FromCoreRegister(kCoreArgumentRegisters[gpr_index++]));
          } else if (gpr_index == arraysize(kCoreArgumentRegisters) - 1) {
            gpr_index++;
            entry_spills_.push_back(ManagedRegister::NoRegister(), 4);
          } else {
            entry_spills_.push_back(ManagedRegister::NoRegister(), 4);
          }
        }

        if (gpr_index < arraysize(kCoreArgumentRegisters)) {
          entry_spills_.push_back(
            MipsManagedRegister::FromCoreRegister(kCoreArgumentRegisters[gpr_index++]));
        } else {
          entry_spills_.push_back(ManagedRegister::NoRegister(), 4);
        }
      }
    }
  }
  return entry_spills_;
}
// JNI calling convention

MipsJniCallingConvention::MipsJniCallingConvention(bool is_static, bool is_synchronized,
                                                   const char* shorty)
    : JniCallingConvention(is_static, is_synchronized, shorty, kFramePointerSize) {
  // Compute padding to ensure longs and doubles are not split in AAPCS. Ignore the 'this' jobject
  // or jclass for static methods and the JNIEnv. We start at the aligned register A2.
  size_t padding = 0;
  for (size_t cur_arg = IsStatic() ? 0 : 1, cur_reg = 2; cur_arg < NumArgs(); cur_arg++) {
    if (IsParamALongOrDouble(cur_arg)) {
      if ((cur_reg & 1) != 0) {
        padding += 4;
        cur_reg++;  // additional bump to ensure alignment
      }
      cur_reg++;  // additional bump to skip extra long word
    }
    cur_reg++;  // bump the iterator for every argument
  }
  padding_ = padding;

  callee_save_regs_.push_back(MipsManagedRegister::FromCoreRegister(S2));
  callee_save_regs_.push_back(MipsManagedRegister::FromCoreRegister(S3));
  callee_save_regs_.push_back(MipsManagedRegister::FromCoreRegister(S4));
  callee_save_regs_.push_back(MipsManagedRegister::FromCoreRegister(S5));
  callee_save_regs_.push_back(MipsManagedRegister::FromCoreRegister(S6));
  callee_save_regs_.push_back(MipsManagedRegister::FromCoreRegister(S7));
  callee_save_regs_.push_back(MipsManagedRegister::FromCoreRegister(FP));
}

uint32_t MipsJniCallingConvention::CoreSpillMask() const {
  // Compute spill mask to agree with callee saves initialized in the constructor
  uint32_t result = 0;
  result = 1 << S2 | 1 << S3 | 1 << S4 | 1 << S5 | 1 << S6 | 1 << S7 | 1 << FP | 1 << RA;
  return result;
}

ManagedRegister MipsJniCallingConvention::ReturnScratchRegister() const {
  return MipsManagedRegister::FromCoreRegister(AT);
}

size_t MipsJniCallingConvention::FrameSize() {
  // ArtMethod*, RA and callee save area size, local reference segment state
  size_t frame_data_size = kMipsPointerSize +
      (2 + CalleeSaveRegisters().size()) * kFramePointerSize;
  // References plus 2 words for HandleScope header
  size_t handle_scope_size = HandleScope::SizeOf(kFramePointerSize, ReferenceCount());
  // Plus return value spill area size
  return RoundUp(frame_data_size + handle_scope_size + SizeOfReturnValue(), kStackAlignment);
}

size_t MipsJniCallingConvention::OutArgSize() {
  return RoundUp(NumberOfOutgoingStackArgs() * kFramePointerSize + padding_, kStackAlignment);
}

// JniCallingConvention ABI follows AAPCS where longs and doubles must occur
// in even register numbers and stack slots
void MipsJniCallingConvention::Next() {
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

bool MipsJniCallingConvention::IsCurrentParamInRegister() {
  return itr_slots_ < 4;
}

bool MipsJniCallingConvention::IsCurrentParamOnStack() {
  return !IsCurrentParamInRegister();
}

static const Register kJniArgumentRegisters[] = {
  A0, A1, A2, A3
};
ManagedRegister MipsJniCallingConvention::CurrentParamRegister() {
  CHECK_LT(itr_slots_, 4u);
  int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni();
  if ((itr_args_ >= 2) && IsParamALongOrDouble(arg_pos)) {
    CHECK_EQ(itr_slots_, 2u);
    return MipsManagedRegister::FromRegisterPair(A2_A3);
  } else {
    return
      MipsManagedRegister::FromCoreRegister(kJniArgumentRegisters[itr_slots_]);
  }
}

FrameOffset MipsJniCallingConvention::CurrentParamStackOffset() {
  CHECK_GE(itr_slots_, 4u);
  size_t offset = displacement_.Int32Value() - OutArgSize() + (itr_slots_ * kFramePointerSize);
  CHECK_LT(offset, OutArgSize());
  return FrameOffset(offset);
}

size_t MipsJniCallingConvention::NumberOfOutgoingStackArgs() {
  size_t static_args = IsStatic() ? 1 : 0;  // count jclass
  // regular argument parameters and this
  size_t param_args = NumArgs() + NumLongOrDoubleArgs();
  // count JNIEnv*
  return static_args + param_args + 1;
}
}  // namespace mips
}  // namespace art
