/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "calling_convention_mips64.h"

#include "base/logging.h"
#include "handle_scope-inl.h"
#include "utils/mips64/managed_register_mips64.h"

namespace art {
namespace mips64 {

static const GpuRegister kGpuArgumentRegisters[] = {
  A0, A1, A2, A3, A4, A5, A6, A7
};

static const FpuRegister kFpuArgumentRegisters[] = {
  F12, F13, F14, F15, F16, F17, F18, F19
};

// Calling convention
ManagedRegister Mips64ManagedRuntimeCallingConvention::InterproceduralScratchRegister() {
  return Mips64ManagedRegister::FromGpuRegister(T9);
}

ManagedRegister Mips64JniCallingConvention::InterproceduralScratchRegister() {
  return Mips64ManagedRegister::FromGpuRegister(T9);
}

static ManagedRegister ReturnRegisterForShorty(const char* shorty) {
  if (shorty[0] == 'F' || shorty[0] == 'D') {
    return Mips64ManagedRegister::FromFpuRegister(F0);
  } else if (shorty[0] == 'V') {
    return Mips64ManagedRegister::NoRegister();
  } else {
    return Mips64ManagedRegister::FromGpuRegister(V0);
  }
}

ManagedRegister Mips64ManagedRuntimeCallingConvention::ReturnRegister() {
  return ReturnRegisterForShorty(GetShorty());
}

ManagedRegister Mips64JniCallingConvention::ReturnRegister() {
  return ReturnRegisterForShorty(GetShorty());
}

ManagedRegister Mips64JniCallingConvention::IntReturnRegister() {
  return Mips64ManagedRegister::FromGpuRegister(V0);
}

// Managed runtime calling convention

ManagedRegister Mips64ManagedRuntimeCallingConvention::MethodRegister() {
  return Mips64ManagedRegister::FromGpuRegister(A0);
}

bool Mips64ManagedRuntimeCallingConvention::IsCurrentParamInRegister() {
  return false;  // Everything moved to stack on entry.
}

bool Mips64ManagedRuntimeCallingConvention::IsCurrentParamOnStack() {
  return true;
}

ManagedRegister Mips64ManagedRuntimeCallingConvention::CurrentParamRegister() {
  LOG(FATAL) << "Should not reach here";
  return ManagedRegister::NoRegister();
}

FrameOffset Mips64ManagedRuntimeCallingConvention::CurrentParamStackOffset() {
  CHECK(IsCurrentParamOnStack());
  FrameOffset result =
      FrameOffset(displacement_.Int32Value() +  // displacement
                  kFramePointerSize +  // Method ref
                  (itr_slots_ * sizeof(uint32_t)));  // offset into in args
  return result;
}

const ManagedRegisterEntrySpills& Mips64ManagedRuntimeCallingConvention::EntrySpills() {
  // We spill the argument registers on MIPS64 to free them up for scratch use,
  // we then assume all arguments are on the stack.
  if ((entry_spills_.size() == 0) && (NumArgs() > 0)) {
    int reg_index = 1;   // we start from A1, A0 holds ArtMethod*.

    // We need to choose the correct register size since the managed
    // stack uses 32bit stack slots.
    ResetIterator(FrameOffset(0));
    while (HasNext()) {
      if (reg_index < 8) {
        if (IsCurrentParamAFloatOrDouble()) {  // FP regs.
          FpuRegister arg = kFpuArgumentRegisters[reg_index];
          Mips64ManagedRegister reg = Mips64ManagedRegister::FromFpuRegister(arg);
          entry_spills_.push_back(reg, IsCurrentParamADouble() ? 8 : 4);
        } else {  // GP regs.
          GpuRegister arg = kGpuArgumentRegisters[reg_index];
          Mips64ManagedRegister reg = Mips64ManagedRegister::FromGpuRegister(arg);
          entry_spills_.push_back(reg,
                                  (IsCurrentParamALong() && (!IsCurrentParamAReference())) ? 8 : 4);
        }
        // e.g. A1, A2, F3, A4, F5, F6, A7
        reg_index++;
      }

      Next();
    }
  }
  return entry_spills_;
}

// JNI calling convention

Mips64JniCallingConvention::Mips64JniCallingConvention(bool is_static, bool is_synchronized,
                                                       const char* shorty)
    : JniCallingConvention(is_static, is_synchronized, shorty, kFramePointerSize) {
  callee_save_regs_.push_back(Mips64ManagedRegister::FromGpuRegister(S2));
  callee_save_regs_.push_back(Mips64ManagedRegister::FromGpuRegister(S3));
  callee_save_regs_.push_back(Mips64ManagedRegister::FromGpuRegister(S4));
  callee_save_regs_.push_back(Mips64ManagedRegister::FromGpuRegister(S5));
  callee_save_regs_.push_back(Mips64ManagedRegister::FromGpuRegister(S6));
  callee_save_regs_.push_back(Mips64ManagedRegister::FromGpuRegister(S7));
  callee_save_regs_.push_back(Mips64ManagedRegister::FromGpuRegister(GP));
  callee_save_regs_.push_back(Mips64ManagedRegister::FromGpuRegister(S8));
}

uint32_t Mips64JniCallingConvention::CoreSpillMask() const {
  // Compute spill mask to agree with callee saves initialized in the constructor
  uint32_t result = 0;
  result = 1 << S2 | 1 << S3 | 1 << S4 | 1 << S5 | 1 << S6 | 1 << S7 | 1 << GP | 1 << S8 | 1 << RA;
  DCHECK_EQ(static_cast<size_t>(POPCOUNT(result)), callee_save_regs_.size() + 1);
  return result;
}

ManagedRegister Mips64JniCallingConvention::ReturnScratchRegister() const {
  return Mips64ManagedRegister::FromGpuRegister(AT);
}

size_t Mips64JniCallingConvention::FrameSize() {
  // ArtMethod*, RA and callee save area size, local reference segment state
  size_t frame_data_size = kFramePointerSize +
      (CalleeSaveRegisters().size() + 1) * kFramePointerSize + sizeof(uint32_t);
  // References plus 2 words for HandleScope header
  size_t handle_scope_size = HandleScope::SizeOf(kFramePointerSize, ReferenceCount());
  // Plus return value spill area size
  return RoundUp(frame_data_size + handle_scope_size + SizeOfReturnValue(), kStackAlignment);
}

size_t Mips64JniCallingConvention::OutArgSize() {
  return RoundUp(NumberOfOutgoingStackArgs() * kFramePointerSize, kStackAlignment);
}

bool Mips64JniCallingConvention::IsCurrentParamInRegister() {
  return itr_args_ < 8;
}

bool Mips64JniCallingConvention::IsCurrentParamOnStack() {
  return !IsCurrentParamInRegister();
}

ManagedRegister Mips64JniCallingConvention::CurrentParamRegister() {
  CHECK(IsCurrentParamInRegister());
  if (IsCurrentParamAFloatOrDouble()) {
    return Mips64ManagedRegister::FromFpuRegister(kFpuArgumentRegisters[itr_args_]);
  } else {
    return Mips64ManagedRegister::FromGpuRegister(kGpuArgumentRegisters[itr_args_]);
  }
}

FrameOffset Mips64JniCallingConvention::CurrentParamStackOffset() {
  CHECK(IsCurrentParamOnStack());
  size_t offset = displacement_.Int32Value() - OutArgSize() + ((itr_args_ - 8) * kFramePointerSize);
  CHECK_LT(offset, OutArgSize());
  return FrameOffset(offset);
}

size_t Mips64JniCallingConvention::NumberOfOutgoingStackArgs() {
  // all arguments including JNI args
  size_t all_args = NumArgs() + NumberOfExtraArgumentsForJni();

  // Nothing on the stack unless there are more than 8 arguments
  return (all_args > 8) ? all_args - 8 : 0;
}
}  // namespace mips64
}  // namespace art
