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

#include "assembler_arm64.h"
#include "base/logging.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "offsets.h"
#include "thread.h"
#include "utils.h"

namespace art {
namespace arm64 {

#ifdef ___
#error "ARM64 Assembler macro already defined."
#else
#define ___   vixl_masm_->
#endif

void Arm64Assembler::EmitSlowPaths() {
  if (!exception_blocks_.empty()) {
    for (size_t i = 0; i < exception_blocks_.size(); i++) {
      EmitExceptionPoll(exception_blocks_.at(i));
    }
  }
  ___ FinalizeCode();
}

size_t Arm64Assembler::CodeSize() const {
  return ___ SizeOfCodeGenerated();
}

void Arm64Assembler::FinalizeInstructions(const MemoryRegion& region) {
  // Copy the instructions from the buffer.
  MemoryRegion from(reinterpret_cast<void*>(vixl_buf_), CodeSize());
  region.CopyFrom(0, from);
}

void Arm64Assembler::GetCurrentThread(ManagedRegister tr) {
  ___ Mov(reg_x(tr.AsArm64().AsCoreRegister()), reg_x(ETR));
}

void Arm64Assembler::GetCurrentThread(FrameOffset offset, ManagedRegister /* scratch */) {
  StoreToOffset(ETR, SP, offset.Int32Value());
}

// See Arm64 PCS Section 5.2.2.1.
void Arm64Assembler::IncreaseFrameSize(size_t adjust) {
  CHECK_ALIGNED(adjust, kStackAlignment);
  AddConstant(SP, -adjust);
}

// See Arm64 PCS Section 5.2.2.1.
void Arm64Assembler::DecreaseFrameSize(size_t adjust) {
  CHECK_ALIGNED(adjust, kStackAlignment);
  AddConstant(SP, adjust);
}

void Arm64Assembler::AddConstant(Register rd, int32_t value, Condition cond) {
  AddConstant(rd, rd, value, cond);
}

void Arm64Assembler::AddConstant(Register rd, Register rn, int32_t value,
                                 Condition cond) {
  if ((cond == AL) || (cond == NV)) {
    // VIXL macro-assembler handles all variants.
    ___ Add(reg_x(rd), reg_x(rn), value);
  } else {
    // temp = rd + value
    // rd = cond ? temp : rn
    vixl::UseScratchRegisterScope temps(vixl_masm_);
    temps.Exclude(reg_x(rd), reg_x(rn));
    vixl::Register temp = temps.AcquireX();
    ___ Add(temp, reg_x(rn), value);
    ___ Csel(reg_x(rd), temp, reg_x(rd), COND_OP(cond));
  }
}

void Arm64Assembler::StoreWToOffset(StoreOperandType type, WRegister source,
                                    Register base, int32_t offset) {
  switch (type) {
    case kStoreByte:
      ___ Strb(reg_w(source), MEM_OP(reg_x(base), offset));
      break;
    case kStoreHalfword:
      ___ Strh(reg_w(source), MEM_OP(reg_x(base), offset));
      break;
    case kStoreWord:
      ___ Str(reg_w(source), MEM_OP(reg_x(base), offset));
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}

void Arm64Assembler::StoreToOffset(Register source, Register base, int32_t offset) {
  CHECK_NE(source, SP);
  ___ Str(reg_x(source), MEM_OP(reg_x(base), offset));
}

void Arm64Assembler::StoreSToOffset(SRegister source, Register base, int32_t offset) {
  ___ Str(reg_s(source), MEM_OP(reg_x(base), offset));
}

void Arm64Assembler::StoreDToOffset(DRegister source, Register base, int32_t offset) {
  ___ Str(reg_d(source), MEM_OP(reg_x(base), offset));
}

void Arm64Assembler::Store(FrameOffset offs, ManagedRegister m_src, size_t size) {
  Arm64ManagedRegister src = m_src.AsArm64();
  if (src.IsNoRegister()) {
    CHECK_EQ(0u, size);
  } else if (src.IsWRegister()) {
    CHECK_EQ(4u, size);
    StoreWToOffset(kStoreWord, src.AsWRegister(), SP, offs.Int32Value());
  } else if (src.IsCoreRegister()) {
    CHECK_EQ(8u, size);
    StoreToOffset(src.AsCoreRegister(), SP, offs.Int32Value());
  } else if (src.IsSRegister()) {
    StoreSToOffset(src.AsSRegister(), SP, offs.Int32Value());
  } else {
    CHECK(src.IsDRegister()) << src;
    StoreDToOffset(src.AsDRegister(), SP, offs.Int32Value());
  }
}

void Arm64Assembler::StoreRef(FrameOffset offs, ManagedRegister m_src) {
  Arm64ManagedRegister src = m_src.AsArm64();
  CHECK(src.IsCoreRegister()) << src;
  StoreWToOffset(kStoreWord, src.AsOverlappingCoreRegisterLow(), SP,
                 offs.Int32Value());
}

void Arm64Assembler::StoreRawPtr(FrameOffset offs, ManagedRegister m_src) {
  Arm64ManagedRegister src = m_src.AsArm64();
  CHECK(src.IsCoreRegister()) << src;
  StoreToOffset(src.AsCoreRegister(), SP, offs.Int32Value());
}

void Arm64Assembler::StoreImmediateToFrame(FrameOffset offs, uint32_t imm,
                                           ManagedRegister m_scratch) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadImmediate(scratch.AsCoreRegister(), imm);
  StoreWToOffset(kStoreWord, scratch.AsOverlappingCoreRegisterLow(), SP,
                 offs.Int32Value());
}

void Arm64Assembler::StoreImmediateToThread64(ThreadOffset<8> offs, uint32_t imm,
                                            ManagedRegister m_scratch) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadImmediate(scratch.AsCoreRegister(), imm);
  StoreToOffset(scratch.AsCoreRegister(), ETR, offs.Int32Value());
}

void Arm64Assembler::StoreStackOffsetToThread64(ThreadOffset<8> tr_offs,
                                              FrameOffset fr_offs,
                                              ManagedRegister m_scratch) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(scratch.IsCoreRegister()) << scratch;
  AddConstant(scratch.AsCoreRegister(), SP, fr_offs.Int32Value());
  StoreToOffset(scratch.AsCoreRegister(), ETR, tr_offs.Int32Value());
}

void Arm64Assembler::StoreStackPointerToThread64(ThreadOffset<8> tr_offs) {
  vixl::UseScratchRegisterScope temps(vixl_masm_);
  vixl::Register temp = temps.AcquireX();
  ___ Mov(temp, reg_x(SP));
  ___ Str(temp, MEM_OP(reg_x(ETR), tr_offs.Int32Value()));
}

void Arm64Assembler::StoreSpanning(FrameOffset dest_off, ManagedRegister m_source,
                                   FrameOffset in_off, ManagedRegister m_scratch) {
  Arm64ManagedRegister source = m_source.AsArm64();
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  StoreToOffset(source.AsCoreRegister(), SP, dest_off.Int32Value());
  LoadFromOffset(scratch.AsCoreRegister(), SP, in_off.Int32Value());
  StoreToOffset(scratch.AsCoreRegister(), SP, dest_off.Int32Value() + 8);
}

// Load routines.
void Arm64Assembler::LoadImmediate(Register dest, int32_t value,
                                   Condition cond) {
  if ((cond == AL) || (cond == NV)) {
    ___ Mov(reg_x(dest), value);
  } else {
    // temp = value
    // rd = cond ? temp : rd
    if (value != 0) {
      vixl::UseScratchRegisterScope temps(vixl_masm_);
      temps.Exclude(reg_x(dest));
      vixl::Register temp = temps.AcquireX();
      ___ Mov(temp, value);
      ___ Csel(reg_x(dest), temp, reg_x(dest), COND_OP(cond));
    } else {
      ___ Csel(reg_x(dest), reg_x(XZR), reg_x(dest), COND_OP(cond));
    }
  }
}

void Arm64Assembler::LoadWFromOffset(LoadOperandType type, WRegister dest,
                                     Register base, int32_t offset) {
  switch (type) {
    case kLoadSignedByte:
      ___ Ldrsb(reg_w(dest), MEM_OP(reg_x(base), offset));
      break;
    case kLoadSignedHalfword:
      ___ Ldrsh(reg_w(dest), MEM_OP(reg_x(base), offset));
      break;
    case kLoadUnsignedByte:
      ___ Ldrb(reg_w(dest), MEM_OP(reg_x(base), offset));
      break;
    case kLoadUnsignedHalfword:
      ___ Ldrh(reg_w(dest), MEM_OP(reg_x(base), offset));
      break;
    case kLoadWord:
      ___ Ldr(reg_w(dest), MEM_OP(reg_x(base), offset));
      break;
    default:
        LOG(FATAL) << "UNREACHABLE";
  }
}

// Note: We can extend this member by adding load type info - see
// sign extended A64 load variants.
void Arm64Assembler::LoadFromOffset(Register dest, Register base,
                                    int32_t offset) {
  CHECK_NE(dest, SP);
  ___ Ldr(reg_x(dest), MEM_OP(reg_x(base), offset));
}

void Arm64Assembler::LoadSFromOffset(SRegister dest, Register base,
                                     int32_t offset) {
  ___ Ldr(reg_s(dest), MEM_OP(reg_x(base), offset));
}

void Arm64Assembler::LoadDFromOffset(DRegister dest, Register base,
                                     int32_t offset) {
  ___ Ldr(reg_d(dest), MEM_OP(reg_x(base), offset));
}

void Arm64Assembler::Load(Arm64ManagedRegister dest, Register base,
                          int32_t offset, size_t size) {
  if (dest.IsNoRegister()) {
    CHECK_EQ(0u, size) << dest;
  } else if (dest.IsWRegister()) {
    CHECK_EQ(4u, size) << dest;
    ___ Ldr(reg_w(dest.AsWRegister()), MEM_OP(reg_x(base), offset));
  } else if (dest.IsCoreRegister()) {
    CHECK_NE(dest.AsCoreRegister(), SP) << dest;
    if (size == 4u) {
      ___ Ldr(reg_w(dest.AsOverlappingCoreRegisterLow()), MEM_OP(reg_x(base), offset));
    } else {
      CHECK_EQ(8u, size) << dest;
      ___ Ldr(reg_x(dest.AsCoreRegister()), MEM_OP(reg_x(base), offset));
    }
  } else if (dest.IsSRegister()) {
    ___ Ldr(reg_s(dest.AsSRegister()), MEM_OP(reg_x(base), offset));
  } else {
    CHECK(dest.IsDRegister()) << dest;
    ___ Ldr(reg_d(dest.AsDRegister()), MEM_OP(reg_x(base), offset));
  }
}

void Arm64Assembler::Load(ManagedRegister m_dst, FrameOffset src, size_t size) {
  return Load(m_dst.AsArm64(), SP, src.Int32Value(), size);
}

void Arm64Assembler::LoadFromThread64(ManagedRegister m_dst, ThreadOffset<8> src, size_t size) {
  return Load(m_dst.AsArm64(), ETR, src.Int32Value(), size);
}

void Arm64Assembler::LoadRef(ManagedRegister m_dst, FrameOffset offs) {
  Arm64ManagedRegister dst = m_dst.AsArm64();
  CHECK(dst.IsCoreRegister()) << dst;
  LoadWFromOffset(kLoadWord, dst.AsOverlappingCoreRegisterLow(), SP, offs.Int32Value());
}

void Arm64Assembler::LoadRef(ManagedRegister m_dst, ManagedRegister m_base,
                             MemberOffset offs) {
  Arm64ManagedRegister dst = m_dst.AsArm64();
  Arm64ManagedRegister base = m_base.AsArm64();
  CHECK(dst.IsCoreRegister() && base.IsCoreRegister());
  LoadWFromOffset(kLoadWord, dst.AsOverlappingCoreRegisterLow(), base.AsCoreRegister(),
                  offs.Int32Value());
}

void Arm64Assembler::LoadRawPtr(ManagedRegister m_dst, ManagedRegister m_base, Offset offs) {
  Arm64ManagedRegister dst = m_dst.AsArm64();
  Arm64ManagedRegister base = m_base.AsArm64();
  CHECK(dst.IsCoreRegister() && base.IsCoreRegister());
  // Remove dst and base form the temp list - higher level API uses IP1, IP0.
  vixl::UseScratchRegisterScope temps(vixl_masm_);
  temps.Exclude(reg_x(dst.AsCoreRegister()), reg_x(base.AsCoreRegister()));
  ___ Ldr(reg_x(dst.AsCoreRegister()), MEM_OP(reg_x(base.AsCoreRegister()), offs.Int32Value()));
}

void Arm64Assembler::LoadRawPtrFromThread64(ManagedRegister m_dst, ThreadOffset<8> offs) {
  Arm64ManagedRegister dst = m_dst.AsArm64();
  CHECK(dst.IsCoreRegister()) << dst;
  LoadFromOffset(dst.AsCoreRegister(), ETR, offs.Int32Value());
}

// Copying routines.
void Arm64Assembler::Move(ManagedRegister m_dst, ManagedRegister m_src, size_t size) {
  Arm64ManagedRegister dst = m_dst.AsArm64();
  Arm64ManagedRegister src = m_src.AsArm64();
  if (!dst.Equals(src)) {
    if (dst.IsCoreRegister()) {
      if (size == 4) {
        CHECK(src.IsWRegister());
        ___ Mov(reg_x(dst.AsCoreRegister()), reg_w(src.AsWRegister()));
      } else {
        if (src.IsCoreRegister()) {
          ___ Mov(reg_x(dst.AsCoreRegister()), reg_x(src.AsCoreRegister()));
        } else {
          ___ Mov(reg_x(dst.AsCoreRegister()), reg_w(src.AsWRegister()));
        }
      }
    } else if (dst.IsWRegister()) {
      CHECK(src.IsWRegister()) << src;
      ___ Mov(reg_w(dst.AsWRegister()), reg_w(src.AsWRegister()));
    } else if (dst.IsSRegister()) {
      CHECK(src.IsSRegister()) << src;
      ___ Fmov(reg_s(dst.AsSRegister()), reg_s(src.AsSRegister()));
    } else {
      CHECK(dst.IsDRegister()) << dst;
      CHECK(src.IsDRegister()) << src;
      ___ Fmov(reg_d(dst.AsDRegister()), reg_d(src.AsDRegister()));
    }
  }
}

void Arm64Assembler::CopyRawPtrFromThread64(FrameOffset fr_offs,
                                          ThreadOffset<8> tr_offs,
                                          ManagedRegister m_scratch) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadFromOffset(scratch.AsCoreRegister(), ETR, tr_offs.Int32Value());
  StoreToOffset(scratch.AsCoreRegister(), SP, fr_offs.Int32Value());
}

void Arm64Assembler::CopyRawPtrToThread64(ThreadOffset<8> tr_offs,
                                        FrameOffset fr_offs,
                                        ManagedRegister m_scratch) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadFromOffset(scratch.AsCoreRegister(), SP, fr_offs.Int32Value());
  StoreToOffset(scratch.AsCoreRegister(), ETR, tr_offs.Int32Value());
}

void Arm64Assembler::CopyRef(FrameOffset dest, FrameOffset src,
                             ManagedRegister m_scratch) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadWFromOffset(kLoadWord, scratch.AsOverlappingCoreRegisterLow(),
                  SP, src.Int32Value());
  StoreWToOffset(kStoreWord, scratch.AsOverlappingCoreRegisterLow(),
                 SP, dest.Int32Value());
}

void Arm64Assembler::Copy(FrameOffset dest, FrameOffset src,
                          ManagedRegister m_scratch, size_t size) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(scratch.IsCoreRegister()) << scratch;
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    LoadWFromOffset(kLoadWord, scratch.AsOverlappingCoreRegisterLow(), SP, src.Int32Value());
    StoreWToOffset(kStoreWord, scratch.AsOverlappingCoreRegisterLow(), SP, dest.Int32Value());
  } else if (size == 8) {
    LoadFromOffset(scratch.AsCoreRegister(), SP, src.Int32Value());
    StoreToOffset(scratch.AsCoreRegister(), SP, dest.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Arm64Assembler::Copy(FrameOffset dest, ManagedRegister src_base, Offset src_offset,
                          ManagedRegister m_scratch, size_t size) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  Arm64ManagedRegister base = src_base.AsArm64();
  CHECK(base.IsCoreRegister()) << base;
  CHECK(scratch.IsCoreRegister() || scratch.IsWRegister()) << scratch;
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    LoadWFromOffset(kLoadWord, scratch.AsWRegister(), base.AsCoreRegister(),
                   src_offset.Int32Value());
    StoreWToOffset(kStoreWord, scratch.AsWRegister(), SP, dest.Int32Value());
  } else if (size == 8) {
    LoadFromOffset(scratch.AsCoreRegister(), base.AsCoreRegister(), src_offset.Int32Value());
    StoreToOffset(scratch.AsCoreRegister(), SP, dest.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Arm64Assembler::Copy(ManagedRegister m_dest_base, Offset dest_offs, FrameOffset src,
                          ManagedRegister m_scratch, size_t size) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  Arm64ManagedRegister base = m_dest_base.AsArm64();
  CHECK(base.IsCoreRegister()) << base;
  CHECK(scratch.IsCoreRegister() || scratch.IsWRegister()) << scratch;
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    LoadWFromOffset(kLoadWord, scratch.AsWRegister(), SP, src.Int32Value());
    StoreWToOffset(kStoreWord, scratch.AsWRegister(), base.AsCoreRegister(),
                   dest_offs.Int32Value());
  } else if (size == 8) {
    LoadFromOffset(scratch.AsCoreRegister(), SP, src.Int32Value());
    StoreToOffset(scratch.AsCoreRegister(), base.AsCoreRegister(), dest_offs.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Arm64Assembler::Copy(FrameOffset /*dst*/, FrameOffset /*src_base*/, Offset /*src_offset*/,
                          ManagedRegister /*mscratch*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "Unimplemented Copy() variant";
}

void Arm64Assembler::Copy(ManagedRegister m_dest, Offset dest_offset,
                          ManagedRegister m_src, Offset src_offset,
                          ManagedRegister m_scratch, size_t size) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  Arm64ManagedRegister src = m_src.AsArm64();
  Arm64ManagedRegister dest = m_dest.AsArm64();
  CHECK(dest.IsCoreRegister()) << dest;
  CHECK(src.IsCoreRegister()) << src;
  CHECK(scratch.IsCoreRegister() || scratch.IsWRegister()) << scratch;
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    if (scratch.IsWRegister()) {
      LoadWFromOffset(kLoadWord, scratch.AsWRegister(), src.AsCoreRegister(),
                    src_offset.Int32Value());
      StoreWToOffset(kStoreWord, scratch.AsWRegister(), dest.AsCoreRegister(),
                   dest_offset.Int32Value());
    } else {
      LoadWFromOffset(kLoadWord, scratch.AsOverlappingCoreRegisterLow(), src.AsCoreRegister(),
                    src_offset.Int32Value());
      StoreWToOffset(kStoreWord, scratch.AsOverlappingCoreRegisterLow(), dest.AsCoreRegister(),
                   dest_offset.Int32Value());
    }
  } else if (size == 8) {
    LoadFromOffset(scratch.AsCoreRegister(), src.AsCoreRegister(), src_offset.Int32Value());
    StoreToOffset(scratch.AsCoreRegister(), dest.AsCoreRegister(), dest_offset.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Arm64Assembler::Copy(FrameOffset /*dst*/, Offset /*dest_offset*/,
                          FrameOffset /*src*/, Offset /*src_offset*/,
                          ManagedRegister /*scratch*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "Unimplemented Copy() variant";
}

void Arm64Assembler::MemoryBarrier(ManagedRegister m_scratch) {
  // TODO: Should we check that m_scratch is IP? - see arm.
#if ANDROID_SMP != 0
  ___ Dmb(vixl::InnerShareable, vixl::BarrierAll);
#endif
}

void Arm64Assembler::SignExtend(ManagedRegister mreg, size_t size) {
  Arm64ManagedRegister reg = mreg.AsArm64();
  CHECK(size == 1 || size == 2) << size;
  CHECK(reg.IsWRegister()) << reg;
  if (size == 1) {
    ___ sxtb(reg_w(reg.AsWRegister()), reg_w(reg.AsWRegister()));
  } else {
    ___ sxth(reg_w(reg.AsWRegister()), reg_w(reg.AsWRegister()));
  }
}

void Arm64Assembler::ZeroExtend(ManagedRegister mreg, size_t size) {
  Arm64ManagedRegister reg = mreg.AsArm64();
  CHECK(size == 1 || size == 2) << size;
  CHECK(reg.IsWRegister()) << reg;
  if (size == 1) {
    ___ uxtb(reg_w(reg.AsWRegister()), reg_w(reg.AsWRegister()));
  } else {
    ___ uxth(reg_w(reg.AsWRegister()), reg_w(reg.AsWRegister()));
  }
}

void Arm64Assembler::VerifyObject(ManagedRegister /*src*/, bool /*could_be_null*/) {
  // TODO: not validating references.
}

void Arm64Assembler::VerifyObject(FrameOffset /*src*/, bool /*could_be_null*/) {
  // TODO: not validating references.
}

void Arm64Assembler::Call(ManagedRegister m_base, Offset offs, ManagedRegister m_scratch) {
  Arm64ManagedRegister base = m_base.AsArm64();
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(base.IsCoreRegister()) << base;
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadFromOffset(scratch.AsCoreRegister(), base.AsCoreRegister(), offs.Int32Value());
  ___ Blr(reg_x(scratch.AsCoreRegister()));
}

void Arm64Assembler::JumpTo(ManagedRegister m_base, Offset offs, ManagedRegister m_scratch) {
  Arm64ManagedRegister base = m_base.AsArm64();
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(base.IsCoreRegister()) << base;
  CHECK(scratch.IsCoreRegister()) << scratch;
  // Remove base and scratch form the temp list - higher level API uses IP1, IP0.
  vixl::UseScratchRegisterScope temps(vixl_masm_);
  temps.Exclude(reg_x(base.AsCoreRegister()), reg_x(scratch.AsCoreRegister()));
  ___ Ldr(reg_x(scratch.AsCoreRegister()), MEM_OP(reg_x(base.AsCoreRegister()), offs.Int32Value()));
  ___ Br(reg_x(scratch.AsCoreRegister()));
}

void Arm64Assembler::Call(FrameOffset base, Offset offs, ManagedRegister m_scratch) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(scratch.IsCoreRegister()) << scratch;
  // Call *(*(SP + base) + offset)
  LoadWFromOffset(kLoadWord, scratch.AsOverlappingCoreRegisterLow(), SP, base.Int32Value());
  LoadFromOffset(scratch.AsCoreRegister(), scratch.AsCoreRegister(), offs.Int32Value());
  ___ Blr(reg_x(scratch.AsCoreRegister()));
}

void Arm64Assembler::CallFromThread64(ThreadOffset<8> /*offset*/, ManagedRegister /*scratch*/) {
  UNIMPLEMENTED(FATAL) << "Unimplemented Call() variant";
}

void Arm64Assembler::CreateHandleScopeEntry(ManagedRegister m_out_reg, FrameOffset handle_scope_offs,
                                     ManagedRegister m_in_reg, bool null_allowed) {
  Arm64ManagedRegister out_reg = m_out_reg.AsArm64();
  Arm64ManagedRegister in_reg = m_in_reg.AsArm64();
  // For now we only hold stale handle scope entries in x registers.
  CHECK(in_reg.IsNoRegister() || in_reg.IsCoreRegister()) << in_reg;
  CHECK(out_reg.IsCoreRegister()) << out_reg;
  if (null_allowed) {
    // Null values get a handle scope entry value of 0.  Otherwise, the handle scope entry is
    // the address in the handle scope holding the reference.
    // e.g. out_reg = (handle == 0) ? 0 : (SP+handle_offset)
    if (in_reg.IsNoRegister()) {
      LoadWFromOffset(kLoadWord, out_reg.AsOverlappingCoreRegisterLow(), SP,
                      handle_scope_offs.Int32Value());
      in_reg = out_reg;
    }
    ___ Cmp(reg_w(in_reg.AsOverlappingCoreRegisterLow()), 0);
    if (!out_reg.Equals(in_reg)) {
      LoadImmediate(out_reg.AsCoreRegister(), 0, EQ);
    }
    AddConstant(out_reg.AsCoreRegister(), SP, handle_scope_offs.Int32Value(), NE);
  } else {
    AddConstant(out_reg.AsCoreRegister(), SP, handle_scope_offs.Int32Value(), AL);
  }
}

void Arm64Assembler::CreateHandleScopeEntry(FrameOffset out_off, FrameOffset handle_scope_offset,
                                     ManagedRegister m_scratch, bool null_allowed) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(scratch.IsCoreRegister()) << scratch;
  if (null_allowed) {
    LoadWFromOffset(kLoadWord, scratch.AsOverlappingCoreRegisterLow(), SP,
                    handle_scope_offset.Int32Value());
    // Null values get a handle scope entry value of 0.  Otherwise, the handle scope entry is
    // the address in the handle scope holding the reference.
    // e.g. scratch = (scratch == 0) ? 0 : (SP+handle_scope_offset)
    ___ Cmp(reg_w(scratch.AsOverlappingCoreRegisterLow()), 0);
    // Move this logic in add constants with flags.
    AddConstant(scratch.AsCoreRegister(), SP, handle_scope_offset.Int32Value(), NE);
  } else {
    AddConstant(scratch.AsCoreRegister(), SP, handle_scope_offset.Int32Value(), AL);
  }
  StoreToOffset(scratch.AsCoreRegister(), SP, out_off.Int32Value());
}

void Arm64Assembler::LoadReferenceFromHandleScope(ManagedRegister m_out_reg,
                                           ManagedRegister m_in_reg) {
  Arm64ManagedRegister out_reg = m_out_reg.AsArm64();
  Arm64ManagedRegister in_reg = m_in_reg.AsArm64();
  CHECK(out_reg.IsCoreRegister()) << out_reg;
  CHECK(in_reg.IsCoreRegister()) << in_reg;
  vixl::Label exit;
  if (!out_reg.Equals(in_reg)) {
    // FIXME: Who sets the flags here?
    LoadImmediate(out_reg.AsCoreRegister(), 0, EQ);
  }
  ___ Cbz(reg_x(in_reg.AsCoreRegister()), &exit);
  LoadFromOffset(out_reg.AsCoreRegister(), in_reg.AsCoreRegister(), 0);
  ___ Bind(&exit);
}

void Arm64Assembler::ExceptionPoll(ManagedRegister m_scratch, size_t stack_adjust) {
  CHECK_ALIGNED(stack_adjust, kStackAlignment);
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  Arm64Exception *current_exception = new Arm64Exception(scratch, stack_adjust);
  exception_blocks_.push_back(current_exception);
  LoadFromOffset(scratch.AsCoreRegister(), ETR, Thread::ExceptionOffset<8>().Int32Value());
  ___ Cbnz(reg_x(scratch.AsCoreRegister()), current_exception->Entry());
}

void Arm64Assembler::EmitExceptionPoll(Arm64Exception *exception) {
  vixl::UseScratchRegisterScope temps(vixl_masm_);
  temps.Exclude(reg_x(exception->scratch_.AsCoreRegister()));
  vixl::Register temp = temps.AcquireX();

  // Bind exception poll entry.
  ___ Bind(exception->Entry());
  if (exception->stack_adjust_ != 0) {  // Fix up the frame.
    DecreaseFrameSize(exception->stack_adjust_);
  }
  // Pass exception object as argument.
  // Don't care about preserving X0 as this won't return.
  ___ Mov(reg_x(X0), reg_x(exception->scratch_.AsCoreRegister()));
  ___ Ldr(temp, MEM_OP(reg_x(ETR), QUICK_ENTRYPOINT_OFFSET(8, pDeliverException).Int32Value()));

  // Move ETR(Callee saved) back to TR(Caller saved) reg. We use ETR on calls
  // to external functions that might trash TR. We do not need the original
  // ETR(X21) saved in BuildFrame().
  ___ Mov(reg_x(TR), reg_x(ETR));

  ___ Blr(temp);
  // Call should never return.
  ___ Brk();
}

constexpr size_t kFramePointerSize = 8;

void Arm64Assembler::BuildFrame(size_t frame_size, ManagedRegister method_reg,
                        const std::vector<ManagedRegister>& callee_save_regs,
                        const ManagedRegisterEntrySpills& entry_spills) {
  CHECK_ALIGNED(frame_size, kStackAlignment);
  CHECK(X0 == method_reg.AsArm64().AsCoreRegister());

  // TODO: *create APCS FP - end of FP chain;
  //       *add support for saving a different set of callee regs.
  // For now we check that the size of callee regs vector is 11.
  CHECK_EQ(callee_save_regs.size(), kJniRefSpillRegsSize);
  // Increase frame to required size - must be at least space to push StackReference<Method>.
  CHECK_GT(frame_size, kJniRefSpillRegsSize * kFramePointerSize);
  IncreaseFrameSize(frame_size);

  // TODO: Ugly hard code...
  // Should generate these according to the spill mask automatically.
  // TUNING: Use stp.
  // Note: Must match Arm64JniCallingConvention::CoreSpillMask().
  size_t reg_offset = frame_size;
  reg_offset -= 8;
  StoreToOffset(LR, SP, reg_offset);
  reg_offset -= 8;
  StoreToOffset(X29, SP, reg_offset);
  reg_offset -= 8;
  StoreToOffset(X28, SP, reg_offset);
  reg_offset -= 8;
  StoreToOffset(X27, SP, reg_offset);
  reg_offset -= 8;
  StoreToOffset(X26, SP, reg_offset);
  reg_offset -= 8;
  StoreToOffset(X25, SP, reg_offset);
  reg_offset -= 8;
  StoreToOffset(X24, SP, reg_offset);
  reg_offset -= 8;
  StoreToOffset(X23, SP, reg_offset);
  reg_offset -= 8;
  StoreToOffset(X22, SP, reg_offset);
  reg_offset -= 8;
  StoreToOffset(X21, SP, reg_offset);
  reg_offset -= 8;
  StoreToOffset(X20, SP, reg_offset);

  // Move TR(Caller saved) to ETR(Callee saved). The original (ETR)X21 has been saved on stack.
  // This way we make sure that TR is not trashed by native code.
  ___ Mov(reg_x(ETR), reg_x(TR));

  // Write StackReference<Method>.
  DCHECK_EQ(4U, sizeof(StackReference<mirror::ArtMethod>));
  StoreWToOffset(StoreOperandType::kStoreWord, W0, SP, 0);

  // Write out entry spills
  int32_t offset = frame_size + sizeof(StackReference<mirror::ArtMethod>);
  for (size_t i = 0; i < entry_spills.size(); ++i) {
    Arm64ManagedRegister reg = entry_spills.at(i).AsArm64();
    if (reg.IsNoRegister()) {
      // only increment stack offset.
      ManagedRegisterSpill spill = entry_spills.at(i);
      offset += spill.getSize();
    } else if (reg.IsCoreRegister()) {
      StoreToOffset(reg.AsCoreRegister(), SP, offset);
      offset += 8;
    } else if (reg.IsWRegister()) {
      StoreWToOffset(kStoreWord, reg.AsWRegister(), SP, offset);
      offset += 4;
    } else if (reg.IsDRegister()) {
      StoreDToOffset(reg.AsDRegister(), SP, offset);
      offset += 8;
    } else if (reg.IsSRegister()) {
      StoreSToOffset(reg.AsSRegister(), SP, offset);
      offset += 4;
    }
  }
}

void Arm64Assembler::RemoveFrame(size_t frame_size, const std::vector<ManagedRegister>& callee_save_regs) {
  CHECK_ALIGNED(frame_size, kStackAlignment);

  // For now we only check that the size of the frame is greater than the spill size.
  CHECK_EQ(callee_save_regs.size(), kJniRefSpillRegsSize);
  CHECK_GT(frame_size, kJniRefSpillRegsSize * kFramePointerSize);

  // We move ETR(aapcs64 callee saved) back to TR(aapcs64 caller saved) which might have
  // been trashed in the native call. The original ETR(X21) is restored from stack.
  ___ Mov(reg_x(TR), reg_x(ETR));

  // TODO: Ugly hard code...
  // Should generate these according to the spill mask automatically.
  // TUNING: Use ldp.
  // Note: Must match Arm64JniCallingConvention::CoreSpillMask().
  size_t reg_offset = frame_size;
  reg_offset -= 8;
  LoadFromOffset(LR, SP, reg_offset);
  reg_offset -= 8;
  LoadFromOffset(X29, SP, reg_offset);
  reg_offset -= 8;
  LoadFromOffset(X28, SP, reg_offset);
  reg_offset -= 8;
  LoadFromOffset(X27, SP, reg_offset);
  reg_offset -= 8;
  LoadFromOffset(X26, SP, reg_offset);
  reg_offset -= 8;
  LoadFromOffset(X25, SP, reg_offset);
  reg_offset -= 8;
  LoadFromOffset(X24, SP, reg_offset);
  reg_offset -= 8;
  LoadFromOffset(X23, SP, reg_offset);
  reg_offset -= 8;
  LoadFromOffset(X22, SP, reg_offset);
  reg_offset -= 8;
  LoadFromOffset(X21, SP, reg_offset);
  reg_offset -= 8;
  LoadFromOffset(X20, SP, reg_offset);

  // Decrease frame size to start of callee saved regs.
  DecreaseFrameSize(frame_size);

  // Pop callee saved and return to LR.
  ___ Ret();
}

}  // namespace arm64
}  // namespace art
