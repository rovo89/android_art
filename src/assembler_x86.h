// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_ASSEMBLER_X86_H_
#define ART_SRC_ASSEMBLER_X86_H_

#include <vector>
#include "assembler.h"
#include "constants.h"
#include "globals.h"
#include "managed_register.h"
#include "macros.h"
#include "offsets.h"
#include "utils.h"

namespace art {

class Immediate {
 public:
  explicit Immediate(int32_t value) : value_(value) {}

  int32_t value() const { return value_; }

  bool is_int8() const { return IsInt(8, value_); }
  bool is_uint8() const { return IsUint(8, value_); }
  bool is_uint16() const { return IsUint(16, value_); }

 private:
  const int32_t value_;

  DISALLOW_COPY_AND_ASSIGN(Immediate);
};


class Operand {
 public:
  uint8_t mod() const {
    return (encoding_at(0) >> 6) & 3;
  }

  Register rm() const {
    return static_cast<Register>(encoding_at(0) & 7);
  }

  ScaleFactor scale() const {
    return static_cast<ScaleFactor>((encoding_at(1) >> 6) & 3);
  }

  Register index() const {
    return static_cast<Register>((encoding_at(1) >> 3) & 7);
  }

  Register base() const {
    return static_cast<Register>(encoding_at(1) & 7);
  }

  int8_t disp8() const {
    CHECK_GE(length_, 2);
    return static_cast<int8_t>(encoding_[length_ - 1]);
  }

  int32_t disp32() const {
    CHECK_GE(length_, 5);
    int32_t value;
    memcpy(&value, &encoding_[length_ - 4], sizeof(value));
    return value;
  }

  bool IsRegister(Register reg) const {
    return ((encoding_[0] & 0xF8) == 0xC0)  // Addressing mode is register only.
        && ((encoding_[0] & 0x07) == reg);  // Register codes match.
  }

 protected:
  // Operand can be sub classed (e.g: Address).
  Operand() : length_(0) { }

  void SetModRM(int mod, Register rm) {
    CHECK_EQ(mod & ~3, 0);
    encoding_[0] = (mod << 6) | rm;
    length_ = 1;
  }

  void SetSIB(ScaleFactor scale, Register index, Register base) {
    CHECK_EQ(length_, 1);
    CHECK_EQ(scale & ~3, 0);
    encoding_[1] = (scale << 6) | (index << 3) | base;
    length_ = 2;
  }

  void SetDisp8(int8_t disp) {
    CHECK(length_ == 1 || length_ == 2);
    encoding_[length_++] = static_cast<uint8_t>(disp);
  }

  void SetDisp32(int32_t disp) {
    CHECK(length_ == 1 || length_ == 2);
    int disp_size = sizeof(disp);
    memmove(&encoding_[length_], &disp, disp_size);
    length_ += disp_size;
  }

 private:
  byte length_;
  byte encoding_[6];
  byte padding_;

  explicit Operand(Register reg) { SetModRM(3, reg); }

  // Get the operand encoding byte at the given index.
  uint8_t encoding_at(int index) const {
    CHECK_GE(index, 0);
    CHECK_LT(index, length_);
    return encoding_[index];
  }

  friend class Assembler;

  DISALLOW_COPY_AND_ASSIGN(Operand);
};


class Address : public Operand {
 public:
  Address(Register base, int32_t disp) {
    Init(base, disp);
  }

  Address(Register base, Offset disp) {
    Init(base, disp.Int32Value());
  }

  Address(Register base, FrameOffset disp) {
    CHECK_EQ(base, ESP);
    Init(ESP, disp.Int32Value());
  }

  Address(Register base, MemberOffset disp) {
    Init(base, disp.Int32Value());
  }

  void Init(Register base, int32_t disp) {
    if (disp == 0 && base != EBP) {
      SetModRM(0, base);
      if (base == ESP) SetSIB(TIMES_1, ESP, base);
    } else if (disp >= -128 && disp <= 127) {
      SetModRM(1, base);
      if (base == ESP) SetSIB(TIMES_1, ESP, base);
      SetDisp8(disp);
    } else {
      SetModRM(2, base);
      if (base == ESP) SetSIB(TIMES_1, ESP, base);
      SetDisp32(disp);
    }
  }


  Address(Register index, ScaleFactor scale, int32_t disp) {
    CHECK_NE(index, ESP);  // Illegal addressing mode.
    SetModRM(0, ESP);
    SetSIB(scale, index, EBP);
    SetDisp32(disp);
  }

  Address(Register base, Register index, ScaleFactor scale, int32_t disp) {
    CHECK_NE(index, ESP);  // Illegal addressing mode.
    if (disp == 0 && base != EBP) {
      SetModRM(0, ESP);
      SetSIB(scale, index, base);
    } else if (disp >= -128 && disp <= 127) {
      SetModRM(1, ESP);
      SetSIB(scale, index, base);
      SetDisp8(disp);
    } else {
      SetModRM(2, ESP);
      SetSIB(scale, index, base);
      SetDisp32(disp);
    }
  }

  static Address Absolute(uword addr) {
    Address result;
    result.SetModRM(0, EBP);
    result.SetDisp32(addr);
    return result;
  }

  static Address Absolute(ThreadOffset addr) {
    return Absolute(addr.Int32Value());
  }

 private:
  Address() {}

  DISALLOW_COPY_AND_ASSIGN(Address);
};


class Assembler {
 public:
  Assembler() : buffer_() {}
  ~Assembler() {}

  InstructionSet GetInstructionSet() const {
    return kX86;
  }

  /*
   * Emit Machine Instructions.
   */
  void call(Register reg);
  void call(const Address& address);
  void call(Label* label);

  void pushl(Register reg);
  void pushl(const Address& address);
  void pushl(const Immediate& imm);

  void popl(Register reg);
  void popl(const Address& address);

  void movl(Register dst, const Immediate& src);
  void movl(Register dst, Register src);

  void movl(Register dst, const Address& src);
  void movl(const Address& dst, Register src);
  void movl(const Address& dst, const Immediate& imm);

  void movzxb(Register dst, ByteRegister src);
  void movzxb(Register dst, const Address& src);
  void movsxb(Register dst, ByteRegister src);
  void movsxb(Register dst, const Address& src);
  void movb(Register dst, const Address& src);
  void movb(const Address& dst, ByteRegister src);
  void movb(const Address& dst, const Immediate& imm);

  void movzxw(Register dst, Register src);
  void movzxw(Register dst, const Address& src);
  void movsxw(Register dst, Register src);
  void movsxw(Register dst, const Address& src);
  void movw(Register dst, const Address& src);
  void movw(const Address& dst, Register src);

  void leal(Register dst, const Address& src);

  void cmovl(Condition condition, Register dst, Register src);

  void setb(Condition condition, Register dst);

  void movss(XmmRegister dst, const Address& src);
  void movss(const Address& dst, XmmRegister src);
  void movss(XmmRegister dst, XmmRegister src);

  void movd(XmmRegister dst, Register src);
  void movd(Register dst, XmmRegister src);

  void addss(XmmRegister dst, XmmRegister src);
  void addss(XmmRegister dst, const Address& src);
  void subss(XmmRegister dst, XmmRegister src);
  void subss(XmmRegister dst, const Address& src);
  void mulss(XmmRegister dst, XmmRegister src);
  void mulss(XmmRegister dst, const Address& src);
  void divss(XmmRegister dst, XmmRegister src);
  void divss(XmmRegister dst, const Address& src);

  void movsd(XmmRegister dst, const Address& src);
  void movsd(const Address& dst, XmmRegister src);
  void movsd(XmmRegister dst, XmmRegister src);

  void addsd(XmmRegister dst, XmmRegister src);
  void addsd(XmmRegister dst, const Address& src);
  void subsd(XmmRegister dst, XmmRegister src);
  void subsd(XmmRegister dst, const Address& src);
  void mulsd(XmmRegister dst, XmmRegister src);
  void mulsd(XmmRegister dst, const Address& src);
  void divsd(XmmRegister dst, XmmRegister src);
  void divsd(XmmRegister dst, const Address& src);

  void cvtsi2ss(XmmRegister dst, Register src);
  void cvtsi2sd(XmmRegister dst, Register src);

  void cvtss2si(Register dst, XmmRegister src);
  void cvtss2sd(XmmRegister dst, XmmRegister src);

  void cvtsd2si(Register dst, XmmRegister src);
  void cvtsd2ss(XmmRegister dst, XmmRegister src);

  void cvttss2si(Register dst, XmmRegister src);
  void cvttsd2si(Register dst, XmmRegister src);

  void cvtdq2pd(XmmRegister dst, XmmRegister src);

  void comiss(XmmRegister a, XmmRegister b);
  void comisd(XmmRegister a, XmmRegister b);

  void sqrtsd(XmmRegister dst, XmmRegister src);
  void sqrtss(XmmRegister dst, XmmRegister src);

  void xorpd(XmmRegister dst, const Address& src);
  void xorpd(XmmRegister dst, XmmRegister src);
  void xorps(XmmRegister dst, const Address& src);
  void xorps(XmmRegister dst, XmmRegister src);

  void andpd(XmmRegister dst, const Address& src);

  void flds(const Address& src);
  void fstps(const Address& dst);

  void fldl(const Address& src);
  void fstpl(const Address& dst);

  void fnstcw(const Address& dst);
  void fldcw(const Address& src);

  void fistpl(const Address& dst);
  void fistps(const Address& dst);
  void fildl(const Address& src);

  void fincstp();
  void ffree(const Immediate& index);

  void fsin();
  void fcos();
  void fptan();

  void xchgl(Register dst, Register src);

  void cmpl(Register reg, const Immediate& imm);
  void cmpl(Register reg0, Register reg1);
  void cmpl(Register reg, const Address& address);

  void cmpl(const Address& address, Register reg);
  void cmpl(const Address& address, const Immediate& imm);

  void testl(Register reg1, Register reg2);
  void testl(Register reg, const Immediate& imm);

  void andl(Register dst, const Immediate& imm);
  void andl(Register dst, Register src);

  void orl(Register dst, const Immediate& imm);
  void orl(Register dst, Register src);

  void xorl(Register dst, Register src);

  void addl(Register dst, Register src);
  void addl(Register reg, const Immediate& imm);
  void addl(Register reg, const Address& address);

  void addl(const Address& address, Register reg);
  void addl(const Address& address, const Immediate& imm);

  void adcl(Register dst, Register src);
  void adcl(Register reg, const Immediate& imm);
  void adcl(Register dst, const Address& address);

  void subl(Register dst, Register src);
  void subl(Register reg, const Immediate& imm);
  void subl(Register reg, const Address& address);

  void cdq();

  void idivl(Register reg);

  void imull(Register dst, Register src);
  void imull(Register reg, const Immediate& imm);
  void imull(Register reg, const Address& address);

  void imull(Register reg);
  void imull(const Address& address);

  void mull(Register reg);
  void mull(const Address& address);

  void sbbl(Register dst, Register src);
  void sbbl(Register reg, const Immediate& imm);
  void sbbl(Register reg, const Address& address);

  void incl(Register reg);
  void incl(const Address& address);

  void decl(Register reg);
  void decl(const Address& address);

  void shll(Register reg, const Immediate& imm);
  void shll(Register operand, Register shifter);
  void shrl(Register reg, const Immediate& imm);
  void shrl(Register operand, Register shifter);
  void sarl(Register reg, const Immediate& imm);
  void sarl(Register operand, Register shifter);
  void shld(Register dst, Register src);

  void negl(Register reg);
  void notl(Register reg);

  void enter(const Immediate& imm);
  void leave();

  void ret();
  void ret(const Immediate& imm);

  void nop();
  void int3();
  void hlt();

  void j(Condition condition, Label* label);

  void jmp(Register reg);
  void jmp(Label* label);

  Assembler* lock();
  void cmpxchgl(const Address& address, Register reg);

  Assembler* fs();

  //
  // Macros for High-level operations.
  //

  // Emit code that will create an activation on the stack
  void BuildFrame(size_t frame_size, ManagedRegister method_reg,
                  const std::vector<ManagedRegister>& spill_regs);

  // Emit code that will remove an activation from the stack
  void RemoveFrame(size_t frame_size,
                   const std::vector<ManagedRegister>& spill_regs);

  // Fill registers from spill area - no-op on x86
  void FillFromSpillArea(const std::vector<ManagedRegister>& spill_regs,
                         size_t displacement);

  void IncreaseFrameSize(size_t adjust);
  void DecreaseFrameSize(size_t adjust);

  // Store bytes from the given register onto the stack
  void Store(FrameOffset offs, ManagedRegister src, size_t size);
  void StoreRef(FrameOffset dest, ManagedRegister src);
  void StoreRawPtr(FrameOffset dest, ManagedRegister src);

  void CopyRef(FrameOffset dest, FrameOffset src, ManagedRegister scratch);

  void StoreImmediateToFrame(FrameOffset dest, uint32_t imm,
                             ManagedRegister scratch);

  void StoreImmediateToThread(ThreadOffset dest, uint32_t imm,
                              ManagedRegister scratch);

  void Load(ManagedRegister dest, FrameOffset src, size_t size);

  void LoadRef(ManagedRegister dest, FrameOffset  src);

  void LoadRef(ManagedRegister dest, ManagedRegister base, MemberOffset offs);

  void LoadRawPtr(ManagedRegister dest, ManagedRegister base, Offset offs);

  void LoadRawPtrFromThread(ManagedRegister dest, ThreadOffset offs);

  void CopyRawPtrFromThread(FrameOffset fr_offs, ThreadOffset thr_offs,
                            ManagedRegister scratch);

  void CopyRawPtrToThread(ThreadOffset thr_offs, FrameOffset fr_offs,
                          ManagedRegister scratch);

  void StoreStackOffsetToThread(ThreadOffset thr_offs, FrameOffset fr_offs,
                                ManagedRegister scratch);
  void StoreStackPointerToThread(ThreadOffset thr_offs);

  void Move(ManagedRegister dest, ManagedRegister src);

  void Copy(FrameOffset dest, FrameOffset src, ManagedRegister scratch,
            unsigned int size);

  void CreateStackHandle(ManagedRegister out_reg, FrameOffset handle_offset,
                         ManagedRegister in_reg, bool null_allowed);

  void CreateStackHandle(FrameOffset out_off, FrameOffset handle_offset,
                         ManagedRegister scratch, bool null_allowed);

  void LoadReferenceFromStackHandle(ManagedRegister dst, ManagedRegister src);

  void ValidateRef(ManagedRegister src, bool could_be_null);
  void ValidateRef(FrameOffset src, bool could_be_null);

  void Call(ManagedRegister base, Offset offset, ManagedRegister scratch);
  void Call(FrameOffset base, Offset offset, ManagedRegister scratch);

  // Generate code to check if Thread::Current()->suspend_count_ is non-zero
  // and branch to a SuspendSlowPath if it is. The SuspendSlowPath will continue
  // at the next instruction.
  void SuspendPoll(ManagedRegister scratch, ManagedRegister return_reg,
                   FrameOffset return_save_location, size_t return_size);

  // Generate code to check if Thread::Current()->exception_ is non-null
  // and branch to a ExceptionSlowPath if it is.
  void ExceptionPoll(ManagedRegister scratch);

  void AddImmediate(Register reg, const Immediate& imm);

  void LoadDoubleConstant(XmmRegister dst, double value);

  void DoubleNegate(XmmRegister d);
  void FloatNegate(XmmRegister f);

  void DoubleAbs(XmmRegister reg);

  void LockCmpxchgl(const Address& address, Register reg) {
    lock()->cmpxchgl(address, reg);
  }

  //
  // Misc. functionality
  //
  int PreferredLoopAlignment() { return 16; }
  void Align(int alignment, int offset);
  void Bind(Label* label);

  void EmitSlowPaths() { buffer_.EmitSlowPaths(this); }

  size_t CodeSize() const { return buffer_.Size(); }

  void FinalizeInstructions(const MemoryRegion& region) {
    buffer_.FinalizeInstructions(region);
  }

  // Debugging and bringup support.
  void Stop(const char* message);

  static void InitializeMemoryWithBreakpoints(byte* data, size_t length);

 private:
  AssemblerBuffer buffer_;

  inline void EmitUint8(uint8_t value);
  inline void EmitInt32(int32_t value);
  inline void EmitRegisterOperand(int rm, int reg);
  inline void EmitXmmRegisterOperand(int rm, XmmRegister reg);
  inline void EmitFixup(AssemblerFixup* fixup);
  inline void EmitOperandSizeOverride();

  void EmitOperand(int rm, const Operand& operand);
  void EmitImmediate(const Immediate& imm);
  void EmitComplex(int rm, const Operand& operand, const Immediate& immediate);
  void EmitLabel(Label* label, int instruction_size);
  void EmitLabelLink(Label* label);
  void EmitNearLabelLink(Label* label);

  void EmitGenericShift(int rm, Register reg, const Immediate& imm);
  void EmitGenericShift(int rm, Register operand, Register shifter);

  DISALLOW_COPY_AND_ASSIGN(Assembler);
};


inline void Assembler::EmitUint8(uint8_t value) {
  buffer_.Emit<uint8_t>(value);
}


inline void Assembler::EmitInt32(int32_t value) {
  buffer_.Emit<int32_t>(value);
}


inline void Assembler::EmitRegisterOperand(int rm, int reg) {
  CHECK_GE(rm, 0);
  CHECK_LT(rm, 8);
  buffer_.Emit<uint8_t>(0xC0 + (rm << 3) + reg);
}


inline void Assembler::EmitXmmRegisterOperand(int rm, XmmRegister reg) {
  EmitRegisterOperand(rm, static_cast<Register>(reg));
}


inline void Assembler::EmitFixup(AssemblerFixup* fixup) {
  buffer_.EmitFixup(fixup);
}


inline void Assembler::EmitOperandSizeOverride() {
  EmitUint8(0x66);
}

}  // namespace art

#endif  // ART_SRC_ASSEMBLER_X86_H_
