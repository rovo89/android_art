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

#include "assembler_arm.h"

#include "base/logging.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "offsets.h"
#include "thread.h"
#include "utils.h"

namespace art {
namespace arm {

const char* kRegisterNames[] = {
  "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
  "fp", "ip", "sp", "lr", "pc"
};

const char* kConditionNames[] = {
  "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT",
  "LE", "AL",
};

std::ostream& operator<<(std::ostream& os, const Register& rhs) {
  if (rhs >= R0 && rhs <= PC) {
    os << kRegisterNames[rhs];
  } else {
    os << "Register[" << static_cast<int>(rhs) << "]";
  }
  return os;
}


std::ostream& operator<<(std::ostream& os, const SRegister& rhs) {
  if (rhs >= S0 && rhs < kNumberOfSRegisters) {
    os << "s" << static_cast<int>(rhs);
  } else {
    os << "SRegister[" << static_cast<int>(rhs) << "]";
  }
  return os;
}


std::ostream& operator<<(std::ostream& os, const DRegister& rhs) {
  if (rhs >= D0 && rhs < kNumberOfDRegisters) {
    os << "d" << static_cast<int>(rhs);
  } else {
    os << "DRegister[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const Condition& rhs) {
  if (rhs >= EQ && rhs <= AL) {
    os << kConditionNames[rhs];
  } else {
    os << "Condition[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

ShifterOperand::ShifterOperand(uint32_t immed)
    : type_(kImmediate), rm_(kNoRegister), rs_(kNoRegister),
      is_rotate_(false), is_shift_(false), shift_(kNoShift), rotate_(0), immed_(immed) {
  CHECK(immed < (1u << 12) || ArmAssembler::ModifiedImmediate(immed) != kInvalidModifiedImmediate);
}


uint32_t ShifterOperand::encodingArm() const {
  CHECK(is_valid());
  switch (type_) {
    case kImmediate:
      if (is_rotate_) {
        return (rotate_ << kRotateShift) | (immed_ << kImmed8Shift);
      } else {
        return immed_;
      }
      break;
    case kRegister:
      if (is_shift_) {
        // Shifted immediate or register.
        if (rs_ == kNoRegister) {
          // Immediate shift.
          return immed_ << kShiftImmShift |
                          static_cast<uint32_t>(shift_) << kShiftShift |
                          static_cast<uint32_t>(rm_);
        } else {
          // Register shift.
          return static_cast<uint32_t>(rs_) << kShiftRegisterShift |
              static_cast<uint32_t>(shift_) << kShiftShift | (1 << 4) |
              static_cast<uint32_t>(rm_);
        }
      } else {
        // Simple register
        return static_cast<uint32_t>(rm_);
      }
      break;
    default:
      // Can't get here.
      LOG(FATAL) << "Invalid shifter operand for ARM";
      return 0;
  }
}

uint32_t ShifterOperand::encodingThumb() const {
  switch (type_) {
    case kImmediate:
      return immed_;
    case kRegister:
      if (is_shift_) {
        // Shifted immediate or register.
        if (rs_ == kNoRegister) {
          // Immediate shift.
          if (shift_ == RRX) {
            // RRX is encoded as an ROR with imm 0.
            return ROR << 4 | static_cast<uint32_t>(rm_);
          } else {
            uint32_t imm3 = immed_ >> 2;
            uint32_t imm2 = immed_ & 0b11;

            return imm3 << 12 | imm2 << 6 | shift_ << 4 |
                static_cast<uint32_t>(rm_);
          }
        } else {
          LOG(FATAL) << "No register-shifted register instruction available in thumb";
          return 0;
        }
      } else {
        // Simple register
        return static_cast<uint32_t>(rm_);
      }
      break;
    default:
      // Can't get here.
      LOG(FATAL) << "Invalid shifter operand for thumb";
      return 0;
  }
  return 0;
}

bool ShifterOperand::CanHoldThumb(Register rd, Register rn, Opcode opcode,
                                  uint32_t immediate, ShifterOperand* shifter_op) {
  shifter_op->type_ = kImmediate;
  shifter_op->immed_ = immediate;
  shifter_op->is_shift_ = false;
  shifter_op->is_rotate_ = false;
  switch (opcode) {
    case ADD:
    case SUB:
      if (rn == SP) {
        if (rd == SP) {
          return immediate < (1 << 9);    // 9 bits allowed.
        } else {
          return immediate < (1 << 12);   // 12 bits.
        }
      }
      if (immediate < (1 << 12)) {    // Less than (or equal to) 12 bits can always be done.
        return true;
      }
      return ArmAssembler::ModifiedImmediate(immediate) != kInvalidModifiedImmediate;

    case MOV:
      // TODO: Support less than or equal to 12bits.
      return ArmAssembler::ModifiedImmediate(immediate) != kInvalidModifiedImmediate;
    case MVN:
    default:
      return ArmAssembler::ModifiedImmediate(immediate) != kInvalidModifiedImmediate;
  }
}

uint32_t Address::encodingArm() const {
  CHECK(IsAbsoluteUint(12, offset_));
  uint32_t encoding;
  if (is_immed_offset_) {
    if (offset_ < 0) {
      encoding = (am_ ^ (1 << kUShift)) | -offset_;  // Flip U to adjust sign.
    } else {
      encoding =  am_ | offset_;
    }
  } else {
    uint32_t imm5 = offset_;
    uint32_t shift = shift_;
    if (shift == RRX) {
      imm5 = 0;
      shift = ROR;
    }
    encoding = am_ | static_cast<uint32_t>(rm_) | shift << 5 | offset_ << 7 | B25;
  }
  encoding |= static_cast<uint32_t>(rn_) << kRnShift;
  return encoding;
}


uint32_t Address::encodingThumb(bool is_32bit) const {
  uint32_t encoding = 0;
  if (is_immed_offset_) {
    encoding = static_cast<uint32_t>(rn_) << 16;
    // Check for the T3/T4 encoding.
    // PUW must Offset for T3
    // Convert ARM PU0W to PUW
    // The Mode is in ARM encoding format which is:
    // |P|U|0|W|
    // we need this in thumb2 mode:
    // |P|U|W|

    uint32_t am = am_;
    int32_t offset = offset_;
    if (offset < 0) {
      am ^= 1 << kUShift;
      offset = -offset;
    }
    if (offset_ < 0 || (offset >= 0 && offset < 256 &&
        am_ != Mode::Offset)) {
      // T4 encoding.
      uint32_t PUW = am >> 21;   // Move down to bottom of word.
      PUW = (PUW >> 1) | (PUW & 1);   // Bits 3, 2 and 0.
      // If P is 0 then W must be 1 (Different from ARM).
      if ((PUW & 0b100) == 0) {
        PUW |= 0b1;
      }
      encoding |= B11 | PUW << 8 | offset;
    } else {
      // T3 encoding (also sets op1 to 0b01).
      encoding |= B23 | offset_;
    }
  } else {
    // Register offset, possibly shifted.
    // Need to choose between encoding T1 (16 bit) or T2.
    // Only Offset mode is supported.  Shift must be LSL and the count
    // is only 2 bits.
    CHECK_EQ(shift_, LSL);
    CHECK_LE(offset_, 4);
    CHECK_EQ(am_, Offset);
    bool is_t2 = is_32bit;
    if (ArmAssembler::IsHighRegister(rn_) || ArmAssembler::IsHighRegister(rm_)) {
      is_t2 = true;
    } else if (offset_ != 0) {
      is_t2 = true;
    }
    if (is_t2) {
      encoding = static_cast<uint32_t>(rn_) << 16 | static_cast<uint32_t>(rm_) |
          offset_ << 4;
    } else {
      encoding = static_cast<uint32_t>(rn_) << 3 | static_cast<uint32_t>(rm_) << 6;
    }
  }
  return encoding;
}

// This is very like the ARM encoding except the offset is 10 bits.
uint32_t Address::encodingThumbLdrdStrd() const {
  uint32_t encoding;
  uint32_t am = am_;
  // If P is 0 then W must be 1 (Different from ARM).
  uint32_t PU1W = am_ >> 21;   // Move down to bottom of word.
  if ((PU1W & 0b1000) == 0) {
    am |= 1 << 21;      // Set W bit.
  }
  if (offset_ < 0) {
    int32_t off = -offset_;
    CHECK_LT(off, 1024);
    CHECK_EQ((off & 0b11), 0);    // Must be multiple of 4.
    encoding = (am ^ (1 << kUShift)) | off >> 2;  // Flip U to adjust sign.
  } else {
    CHECK_LT(offset_, 1024);
    CHECK_EQ((offset_ & 0b11), 0);    // Must be multiple of 4.
    encoding =  am | offset_ >> 2;
  }
  encoding |= static_cast<uint32_t>(rn_) << 16;
  return encoding;
}

// Encoding for ARM addressing mode 3.
uint32_t Address::encoding3() const {
  const uint32_t offset_mask = (1 << 12) - 1;
  uint32_t encoding = encodingArm();
  uint32_t offset = encoding & offset_mask;
  CHECK_LT(offset, 256u);
  return (encoding & ~offset_mask) | ((offset & 0xf0) << 4) | (offset & 0xf);
}

// Encoding for vfp load/store addressing.
uint32_t Address::vencoding() const {
  const uint32_t offset_mask = (1 << 12) - 1;
  uint32_t encoding = encodingArm();
  uint32_t offset = encoding & offset_mask;
  CHECK(IsAbsoluteUint(10, offset));  // In the range -1020 to +1020.
  CHECK_ALIGNED(offset, 2);  // Multiple of 4.
  CHECK((am_ == Offset) || (am_ == NegOffset));
  uint32_t vencoding = (encoding & (0xf << kRnShift)) | (offset >> 2);
  if (am_ == Offset) {
    vencoding |= 1 << 23;
  }
  return vencoding;
}


bool Address::CanHoldLoadOffsetArm(LoadOperandType type, int offset) {
  switch (type) {
    case kLoadSignedByte:
    case kLoadSignedHalfword:
    case kLoadUnsignedHalfword:
    case kLoadWordPair:
      return IsAbsoluteUint(8, offset);  // Addressing mode 3.
    case kLoadUnsignedByte:
    case kLoadWord:
      return IsAbsoluteUint(12, offset);  // Addressing mode 2.
    case kLoadSWord:
    case kLoadDWord:
      return IsAbsoluteUint(10, offset);  // VFP addressing mode.
    default:
      LOG(FATAL) << "UNREACHABLE";
      return false;
  }
}


bool Address::CanHoldStoreOffsetArm(StoreOperandType type, int offset) {
  switch (type) {
    case kStoreHalfword:
    case kStoreWordPair:
      return IsAbsoluteUint(8, offset);  // Addressing mode 3.
    case kStoreByte:
    case kStoreWord:
      return IsAbsoluteUint(12, offset);  // Addressing mode 2.
    case kStoreSWord:
    case kStoreDWord:
      return IsAbsoluteUint(10, offset);  // VFP addressing mode.
    default:
      LOG(FATAL) << "UNREACHABLE";
      return false;
  }
}

bool Address::CanHoldLoadOffsetThumb(LoadOperandType type, int offset) {
  switch (type) {
    case kLoadSignedByte:
    case kLoadSignedHalfword:
    case kLoadUnsignedHalfword:
    case kLoadUnsignedByte:
    case kLoadWord:
      return IsAbsoluteUint(12, offset);
    case kLoadSWord:
    case kLoadDWord:
      return IsAbsoluteUint(10, offset);  // VFP addressing mode.
    case kLoadWordPair:
      return IsAbsoluteUint(10, offset);
  default:
      LOG(FATAL) << "UNREACHABLE";
      return false;
  }
}


bool Address::CanHoldStoreOffsetThumb(StoreOperandType type, int offset) {
  switch (type) {
    case kStoreHalfword:
    case kStoreByte:
    case kStoreWord:
      return IsAbsoluteUint(12, offset);
    case kStoreSWord:
    case kStoreDWord:
      return IsAbsoluteUint(10, offset);  // VFP addressing mode.
    case kStoreWordPair:
      return IsAbsoluteUint(10, offset);
  default:
      LOG(FATAL) << "UNREACHABLE";
      return false;
  }
}

void ArmAssembler::Pad(uint32_t bytes) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  for (uint32_t i = 0; i < bytes; ++i) {
    buffer_.Emit<byte>(0);
  }
}

constexpr size_t kFramePointerSize = 4;

void ArmAssembler::BuildFrame(size_t frame_size, ManagedRegister method_reg,
                              const std::vector<ManagedRegister>& callee_save_regs,
                              const ManagedRegisterEntrySpills& entry_spills) {
  CHECK_ALIGNED(frame_size, kStackAlignment);
  CHECK_EQ(R0, method_reg.AsArm().AsCoreRegister());

  // Push callee saves and link register.
  RegList push_list = 1 << LR;
  size_t pushed_values = 1;
  for (size_t i = 0; i < callee_save_regs.size(); i++) {
    Register reg = callee_save_regs.at(i).AsArm().AsCoreRegister();
    push_list |= 1 << reg;
    pushed_values++;
  }
  PushList(push_list);

  // Increase frame to required size.
  CHECK_GT(frame_size, pushed_values * kFramePointerSize);  // Must at least have space for Method*.
  size_t adjust = frame_size - (pushed_values * kFramePointerSize);
  IncreaseFrameSize(adjust);

  // Write out Method*.
  StoreToOffset(kStoreWord, R0, SP, 0);

  // Write out entry spills.
  for (size_t i = 0; i < entry_spills.size(); ++i) {
    Register reg = entry_spills.at(i).AsArm().AsCoreRegister();
    StoreToOffset(kStoreWord, reg, SP, frame_size + kFramePointerSize + (i * kFramePointerSize));
  }
}

void ArmAssembler::RemoveFrame(size_t frame_size,
                              const std::vector<ManagedRegister>& callee_save_regs) {
  CHECK_ALIGNED(frame_size, kStackAlignment);
  // Compute callee saves to pop and PC.
  RegList pop_list = 1 << PC;
  size_t pop_values = 1;
  for (size_t i = 0; i < callee_save_regs.size(); i++) {
    Register reg = callee_save_regs.at(i).AsArm().AsCoreRegister();
    pop_list |= 1 << reg;
    pop_values++;
  }

  // Decrease frame to start of callee saves.
  CHECK_GT(frame_size, pop_values * kFramePointerSize);
  size_t adjust = frame_size - (pop_values * kFramePointerSize);
  DecreaseFrameSize(adjust);

  // Pop callee saves and PC.
  PopList(pop_list);
}

void ArmAssembler::IncreaseFrameSize(size_t adjust) {
  AddConstant(SP, -adjust);
}

void ArmAssembler::DecreaseFrameSize(size_t adjust) {
  AddConstant(SP, adjust);
}

void ArmAssembler::Store(FrameOffset dest, ManagedRegister msrc, size_t size) {
  ArmManagedRegister src = msrc.AsArm();
  if (src.IsNoRegister()) {
    CHECK_EQ(0u, size);
  } else if (src.IsCoreRegister()) {
    CHECK_EQ(4u, size);
    StoreToOffset(kStoreWord, src.AsCoreRegister(), SP, dest.Int32Value());
  } else if (src.IsRegisterPair()) {
    CHECK_EQ(8u, size);
    StoreToOffset(kStoreWord, src.AsRegisterPairLow(), SP, dest.Int32Value());
    StoreToOffset(kStoreWord, src.AsRegisterPairHigh(),
                  SP, dest.Int32Value() + 4);
  } else if (src.IsSRegister()) {
    StoreSToOffset(src.AsSRegister(), SP, dest.Int32Value());
  } else {
    CHECK(src.IsDRegister()) << src;
    StoreDToOffset(src.AsDRegister(), SP, dest.Int32Value());
  }
}

void ArmAssembler::StoreRef(FrameOffset dest, ManagedRegister msrc) {
  ArmManagedRegister src = msrc.AsArm();
  CHECK(src.IsCoreRegister()) << src;
  StoreToOffset(kStoreWord, src.AsCoreRegister(), SP, dest.Int32Value());
}

void ArmAssembler::StoreRawPtr(FrameOffset dest, ManagedRegister msrc) {
  ArmManagedRegister src = msrc.AsArm();
  CHECK(src.IsCoreRegister()) << src;
  StoreToOffset(kStoreWord, src.AsCoreRegister(), SP, dest.Int32Value());
}

void ArmAssembler::StoreSpanning(FrameOffset dest, ManagedRegister msrc,
                              FrameOffset in_off, ManagedRegister mscratch) {
  ArmManagedRegister src = msrc.AsArm();
  ArmManagedRegister scratch = mscratch.AsArm();
  StoreToOffset(kStoreWord, src.AsCoreRegister(), SP, dest.Int32Value());
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, in_off.Int32Value());
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value() + 4);
}

void ArmAssembler::CopyRef(FrameOffset dest, FrameOffset src,
                        ManagedRegister mscratch) {
  ArmManagedRegister scratch = mscratch.AsArm();
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, src.Int32Value());
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value());
}

void ArmAssembler::LoadRef(ManagedRegister mdest, ManagedRegister base,
                           MemberOffset offs) {
  ArmManagedRegister dst = mdest.AsArm();
  CHECK(dst.IsCoreRegister() && dst.IsCoreRegister()) << dst;
  LoadFromOffset(kLoadWord, dst.AsCoreRegister(),
                 base.AsArm().AsCoreRegister(), offs.Int32Value());
  if (kPoisonHeapReferences) {
    rsb(dst.AsCoreRegister(), dst.AsCoreRegister(), ShifterOperand(0));
  }
}

void ArmAssembler::LoadRef(ManagedRegister mdest, FrameOffset  src) {
  ArmManagedRegister dst = mdest.AsArm();
  CHECK(dst.IsCoreRegister()) << dst;
  LoadFromOffset(kLoadWord, dst.AsCoreRegister(), SP, src.Int32Value());
}

void ArmAssembler::LoadRawPtr(ManagedRegister mdest, ManagedRegister base,
                           Offset offs) {
  ArmManagedRegister dst = mdest.AsArm();
  CHECK(dst.IsCoreRegister() && dst.IsCoreRegister()) << dst;
  LoadFromOffset(kLoadWord, dst.AsCoreRegister(),
                 base.AsArm().AsCoreRegister(), offs.Int32Value());
}

void ArmAssembler::StoreImmediateToFrame(FrameOffset dest, uint32_t imm,
                                      ManagedRegister mscratch) {
  ArmManagedRegister scratch = mscratch.AsArm();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadImmediate(scratch.AsCoreRegister(), imm);
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value());
}

void ArmAssembler::StoreImmediateToThread32(ThreadOffset<4> dest, uint32_t imm,
                                       ManagedRegister mscratch) {
  ArmManagedRegister scratch = mscratch.AsArm();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadImmediate(scratch.AsCoreRegister(), imm);
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(), TR, dest.Int32Value());
}

static void EmitLoad(ArmAssembler* assembler, ManagedRegister m_dst,
                     Register src_register, int32_t src_offset, size_t size) {
  ArmManagedRegister dst = m_dst.AsArm();
  if (dst.IsNoRegister()) {
    CHECK_EQ(0u, size) << dst;
  } else if (dst.IsCoreRegister()) {
    CHECK_EQ(4u, size) << dst;
    assembler->LoadFromOffset(kLoadWord, dst.AsCoreRegister(), src_register, src_offset);
  } else if (dst.IsRegisterPair()) {
    CHECK_EQ(8u, size) << dst;
    assembler->LoadFromOffset(kLoadWord, dst.AsRegisterPairLow(), src_register, src_offset);
    assembler->LoadFromOffset(kLoadWord, dst.AsRegisterPairHigh(), src_register, src_offset + 4);
  } else if (dst.IsSRegister()) {
    assembler->LoadSFromOffset(dst.AsSRegister(), src_register, src_offset);
  } else {
    CHECK(dst.IsDRegister()) << dst;
    assembler->LoadDFromOffset(dst.AsDRegister(), src_register, src_offset);
  }
}

void ArmAssembler::Load(ManagedRegister m_dst, FrameOffset src, size_t size) {
  return EmitLoad(this, m_dst, SP, src.Int32Value(), size);
}

void ArmAssembler::LoadFromThread32(ManagedRegister m_dst, ThreadOffset<4> src, size_t size) {
  return EmitLoad(this, m_dst, TR, src.Int32Value(), size);
}

void ArmAssembler::LoadRawPtrFromThread32(ManagedRegister m_dst, ThreadOffset<4> offs) {
  ArmManagedRegister dst = m_dst.AsArm();
  CHECK(dst.IsCoreRegister()) << dst;
  LoadFromOffset(kLoadWord, dst.AsCoreRegister(), TR, offs.Int32Value());
}

void ArmAssembler::CopyRawPtrFromThread32(FrameOffset fr_offs,
                                        ThreadOffset<4> thr_offs,
                                        ManagedRegister mscratch) {
  ArmManagedRegister scratch = mscratch.AsArm();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 TR, thr_offs.Int32Value());
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(),
                SP, fr_offs.Int32Value());
}

void ArmAssembler::CopyRawPtrToThread32(ThreadOffset<4> thr_offs,
                                      FrameOffset fr_offs,
                                      ManagedRegister mscratch) {
  ArmManagedRegister scratch = mscratch.AsArm();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 SP, fr_offs.Int32Value());
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(),
                TR, thr_offs.Int32Value());
}

void ArmAssembler::StoreStackOffsetToThread32(ThreadOffset<4> thr_offs,
                                            FrameOffset fr_offs,
                                            ManagedRegister mscratch) {
  ArmManagedRegister scratch = mscratch.AsArm();
  CHECK(scratch.IsCoreRegister()) << scratch;
  AddConstant(scratch.AsCoreRegister(), SP, fr_offs.Int32Value(), AL);
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(),
                TR, thr_offs.Int32Value());
}

void ArmAssembler::StoreStackPointerToThread32(ThreadOffset<4> thr_offs) {
  StoreToOffset(kStoreWord, SP, TR, thr_offs.Int32Value());
}

void ArmAssembler::SignExtend(ManagedRegister /*mreg*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "no sign extension necessary for arm";
}

void ArmAssembler::ZeroExtend(ManagedRegister /*mreg*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "no zero extension necessary for arm";
}

void ArmAssembler::Move(ManagedRegister m_dst, ManagedRegister m_src, size_t /*size*/) {
  ArmManagedRegister dst = m_dst.AsArm();
  ArmManagedRegister src = m_src.AsArm();
  if (!dst.Equals(src)) {
    if (dst.IsCoreRegister()) {
      CHECK(src.IsCoreRegister()) << src;
      mov(dst.AsCoreRegister(), ShifterOperand(src.AsCoreRegister()));
    } else if (dst.IsDRegister()) {
      CHECK(src.IsDRegister()) << src;
      vmovd(dst.AsDRegister(), src.AsDRegister());
    } else if (dst.IsSRegister()) {
      CHECK(src.IsSRegister()) << src;
      vmovs(dst.AsSRegister(), src.AsSRegister());
    } else {
      CHECK(dst.IsRegisterPair()) << dst;
      CHECK(src.IsRegisterPair()) << src;
      // Ensure that the first move doesn't clobber the input of the second.
      if (src.AsRegisterPairHigh() != dst.AsRegisterPairLow()) {
        mov(dst.AsRegisterPairLow(), ShifterOperand(src.AsRegisterPairLow()));
        mov(dst.AsRegisterPairHigh(), ShifterOperand(src.AsRegisterPairHigh()));
      } else {
        mov(dst.AsRegisterPairHigh(), ShifterOperand(src.AsRegisterPairHigh()));
        mov(dst.AsRegisterPairLow(), ShifterOperand(src.AsRegisterPairLow()));
      }
    }
  }
}

void ArmAssembler::Copy(FrameOffset dest, FrameOffset src, ManagedRegister mscratch, size_t size) {
  ArmManagedRegister scratch = mscratch.AsArm();
  CHECK(scratch.IsCoreRegister()) << scratch;
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, src.Int32Value());
    StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value());
  } else if (size == 8) {
    LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, src.Int32Value());
    StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value());
    LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, src.Int32Value() + 4);
    StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value() + 4);
  }
}

void ArmAssembler::Copy(FrameOffset dest, ManagedRegister src_base, Offset src_offset,
                        ManagedRegister mscratch, size_t size) {
  Register scratch = mscratch.AsArm().AsCoreRegister();
  CHECK_EQ(size, 4u);
  LoadFromOffset(kLoadWord, scratch, src_base.AsArm().AsCoreRegister(), src_offset.Int32Value());
  StoreToOffset(kStoreWord, scratch, SP, dest.Int32Value());
}

void ArmAssembler::Copy(ManagedRegister dest_base, Offset dest_offset, FrameOffset src,
                        ManagedRegister mscratch, size_t size) {
  Register scratch = mscratch.AsArm().AsCoreRegister();
  CHECK_EQ(size, 4u);
  LoadFromOffset(kLoadWord, scratch, SP, src.Int32Value());
  StoreToOffset(kStoreWord, scratch, dest_base.AsArm().AsCoreRegister(), dest_offset.Int32Value());
}

void ArmAssembler::Copy(FrameOffset /*dst*/, FrameOffset /*src_base*/, Offset /*src_offset*/,
                        ManagedRegister /*mscratch*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL);
}

void ArmAssembler::Copy(ManagedRegister dest, Offset dest_offset,
                        ManagedRegister src, Offset src_offset,
                        ManagedRegister mscratch, size_t size) {
  CHECK_EQ(size, 4u);
  Register scratch = mscratch.AsArm().AsCoreRegister();
  LoadFromOffset(kLoadWord, scratch, src.AsArm().AsCoreRegister(), src_offset.Int32Value());
  StoreToOffset(kStoreWord, scratch, dest.AsArm().AsCoreRegister(), dest_offset.Int32Value());
}

void ArmAssembler::Copy(FrameOffset /*dst*/, Offset /*dest_offset*/, FrameOffset /*src*/, Offset /*src_offset*/,
                        ManagedRegister /*scratch*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL);
}

void ArmAssembler::CreateHandleScopeEntry(ManagedRegister mout_reg,
                                   FrameOffset handle_scope_offset,
                                   ManagedRegister min_reg, bool null_allowed) {
  ArmManagedRegister out_reg = mout_reg.AsArm();
  ArmManagedRegister in_reg = min_reg.AsArm();
  CHECK(in_reg.IsNoRegister() || in_reg.IsCoreRegister()) << in_reg;
  CHECK(out_reg.IsCoreRegister()) << out_reg;
  if (null_allowed) {
    // Null values get a handle scope entry value of 0.  Otherwise, the handle scope entry is
    // the address in the handle scope holding the reference.
    // e.g. out_reg = (handle == 0) ? 0 : (SP+handle_offset)
    if (in_reg.IsNoRegister()) {
      LoadFromOffset(kLoadWord, out_reg.AsCoreRegister(),
                     SP, handle_scope_offset.Int32Value());
      in_reg = out_reg;
    }
    cmp(in_reg.AsCoreRegister(), ShifterOperand(0));
    if (!out_reg.Equals(in_reg)) {
      it(EQ, kItElse);
      LoadImmediate(out_reg.AsCoreRegister(), 0, EQ);
    } else {
      it(NE);
    }
    AddConstant(out_reg.AsCoreRegister(), SP, handle_scope_offset.Int32Value(), NE);
  } else {
    AddConstant(out_reg.AsCoreRegister(), SP, handle_scope_offset.Int32Value(), AL);
  }
}

void ArmAssembler::CreateHandleScopeEntry(FrameOffset out_off,
                                   FrameOffset handle_scope_offset,
                                   ManagedRegister mscratch,
                                   bool null_allowed) {
  ArmManagedRegister scratch = mscratch.AsArm();
  CHECK(scratch.IsCoreRegister()) << scratch;
  if (null_allowed) {
    LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP,
                   handle_scope_offset.Int32Value());
    // Null values get a handle scope entry value of 0.  Otherwise, the handle scope entry is
    // the address in the handle scope holding the reference.
    // e.g. scratch = (scratch == 0) ? 0 : (SP+handle_scope_offset)
    cmp(scratch.AsCoreRegister(), ShifterOperand(0));
    it(NE);
    AddConstant(scratch.AsCoreRegister(), SP, handle_scope_offset.Int32Value(), NE);
  } else {
    AddConstant(scratch.AsCoreRegister(), SP, handle_scope_offset.Int32Value(), AL);
  }
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, out_off.Int32Value());
}

void ArmAssembler::LoadReferenceFromHandleScope(ManagedRegister mout_reg,
                                         ManagedRegister min_reg) {
  ArmManagedRegister out_reg = mout_reg.AsArm();
  ArmManagedRegister in_reg = min_reg.AsArm();
  CHECK(out_reg.IsCoreRegister()) << out_reg;
  CHECK(in_reg.IsCoreRegister()) << in_reg;
  Label null_arg;
  if (!out_reg.Equals(in_reg)) {
    LoadImmediate(out_reg.AsCoreRegister(), 0, EQ);     // TODO: why EQ?
  }
  cmp(in_reg.AsCoreRegister(), ShifterOperand(0));
  it(NE);
  LoadFromOffset(kLoadWord, out_reg.AsCoreRegister(),
                 in_reg.AsCoreRegister(), 0, NE);
}

void ArmAssembler::VerifyObject(ManagedRegister /*src*/, bool /*could_be_null*/) {
  // TODO: not validating references.
}

void ArmAssembler::VerifyObject(FrameOffset /*src*/, bool /*could_be_null*/) {
  // TODO: not validating references.
}

void ArmAssembler::Call(ManagedRegister mbase, Offset offset,
                        ManagedRegister mscratch) {
  ArmManagedRegister base = mbase.AsArm();
  ArmManagedRegister scratch = mscratch.AsArm();
  CHECK(base.IsCoreRegister()) << base;
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 base.AsCoreRegister(), offset.Int32Value());
  blx(scratch.AsCoreRegister());
  // TODO: place reference map on call.
}

void ArmAssembler::Call(FrameOffset base, Offset offset,
                        ManagedRegister mscratch) {
  ArmManagedRegister scratch = mscratch.AsArm();
  CHECK(scratch.IsCoreRegister()) << scratch;
  // Call *(*(SP + base) + offset)
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 SP, base.Int32Value());
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 scratch.AsCoreRegister(), offset.Int32Value());
  blx(scratch.AsCoreRegister());
  // TODO: place reference map on call
}

void ArmAssembler::CallFromThread32(ThreadOffset<4> /*offset*/, ManagedRegister /*scratch*/) {
  UNIMPLEMENTED(FATAL);
}

void ArmAssembler::GetCurrentThread(ManagedRegister tr) {
  mov(tr.AsArm().AsCoreRegister(), ShifterOperand(TR));
}

void ArmAssembler::GetCurrentThread(FrameOffset offset,
                                    ManagedRegister /*scratch*/) {
  StoreToOffset(kStoreWord, TR, SP, offset.Int32Value(), AL);
}

void ArmAssembler::ExceptionPoll(ManagedRegister mscratch, size_t stack_adjust) {
  ArmManagedRegister scratch = mscratch.AsArm();
  ArmExceptionSlowPath* slow = new ArmExceptionSlowPath(scratch, stack_adjust);
  buffer_.EnqueueSlowPath(slow);
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 TR, Thread::ExceptionOffset<4>().Int32Value());
  cmp(scratch.AsCoreRegister(), ShifterOperand(0));
  b(slow->Entry(), NE);
}

void ArmExceptionSlowPath::Emit(Assembler* sasm) {
  ArmAssembler* sp_asm = down_cast<ArmAssembler*>(sasm);
#define __ sp_asm->
  __ Bind(&entry_);
  if (stack_adjust_ != 0) {  // Fix up the frame.
    __ DecreaseFrameSize(stack_adjust_);
  }
  // Pass exception object as argument.
  // Don't care about preserving R0 as this call won't return.
  __ mov(R0, ShifterOperand(scratch_.AsCoreRegister()));
  // Set up call to Thread::Current()->pDeliverException.
  __ LoadFromOffset(kLoadWord, R12, TR, QUICK_ENTRYPOINT_OFFSET(4, pDeliverException).Int32Value());
  __ blx(R12);
  // Call never returns.
  __ bkpt(0);
#undef __
}


static int LeadingZeros(uint32_t val) {
  uint32_t alt;
  int32_t n;
  int32_t count;

  count = 16;
  n = 32;
  do {
    alt = val >> count;
    if (alt != 0) {
      n = n - count;
      val = alt;
    }
    count >>= 1;
  } while (count);
  return n - val;
}


uint32_t ArmAssembler::ModifiedImmediate(uint32_t value) {
  int32_t z_leading;
  int32_t z_trailing;
  uint32_t b0 = value & 0xff;

  /* Note: case of value==0 must use 0:000:0:0000000 encoding */
  if (value <= 0xFF)
    return b0;  // 0:000:a:bcdefgh.
  if (value == ((b0 << 16) | b0))
    return (0x1 << 12) | b0; /* 0:001:a:bcdefgh */
  if (value == ((b0 << 24) | (b0 << 16) | (b0 << 8) | b0))
    return (0x3 << 12) | b0; /* 0:011:a:bcdefgh */
  b0 = (value >> 8) & 0xff;
  if (value == ((b0 << 24) | (b0 << 8)))
    return (0x2 << 12) | b0; /* 0:010:a:bcdefgh */
  /* Can we do it with rotation? */
  z_leading = LeadingZeros(value);
  z_trailing = 32 - LeadingZeros(~value & (value - 1));
  /* A run of eight or fewer active bits? */
  if ((z_leading + z_trailing) < 24)
    return kInvalidModifiedImmediate;  /* No - bail */
  /* left-justify the constant, discarding msb (known to be 1) */
  value <<= z_leading + 1;
  /* Create bcdefgh */
  value >>= 25;

  /* Put it all together */
  uint32_t v = 8 + z_leading;

  uint32_t i = (v & 0b10000) >> 4;
  uint32_t imm3 = (v >> 1) & 0b111;
  uint32_t a = v & 1;
  return value | i << 26 | imm3 << 12 | a << 7;
}

}  // namespace arm
}  // namespace art
