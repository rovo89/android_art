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

#ifndef ART_COMPILER_DEX_REG_STORAGE_H_
#define ART_COMPILER_DEX_REG_STORAGE_H_

#include "base/logging.h"
#include "compiler_enums.h"  // For WideKind

namespace art {

/*
 * 16-bit representation of the physical register container holding a Dalvik value.
 * The encoding allows up to 64 physical elements per storage class, and supports eight
 * register container shapes.
 *
 * [V] [HHHHH] [SSS] [F] [LLLLLL]
 *
 * [LLLLLL]
 *  Physical register number for the low or solo register.
 *    0..63
 *
 * [F]
 *  Describes type of the [LLLLL] register.
 *    0: Core
 *    1: Floating point
 *
 * [SSS]
 *  Shape of the register container.
 *    000: Invalid
 *    001: 32-bit solo register
 *    010: 64-bit solo register
 *    011: 64-bit pair consisting of two 32-bit solo registers
 *    100: 128-bit solo register
 *    101: 256-bit solo register
 *    110: 512-bit solo register
 *    111: 1024-bit solo register
 *
 * [HHHHH]
 *  Physical register number of the high register (valid only for register pair).
 *    0..31
 *
 * [V]
 *    0 -> Invalid
 *    1 -> Valid
 *
 * Note that in all non-invalid cases, we can determine if the storage is floating point
 * by testing bit 7.  Note also that a register pair is effectively limited to a pair of
 * physical register numbers in the 0..31 range.
 *
 * On some target architectures, the same underlying physical register container can be given
 * different views.  For example, Arm's 32-bit single-precision floating point registers
 * s2 and s3 map to the low and high halves of double-precision d1.  Similarly, X86's xmm3
 * vector register can be viewed as 32-bit, 64-bit, 128-bit, etc.  In these cases the use of
 * one view will affect the other views.  The RegStorage class does not concern itself
 * with potential aliasing.  That will be done using the associated RegisterInfo struct.
 * Distinct RegStorage elements should be created for each view of a physical register
 * container.  The management of the aliased physical elements will be handled via RegisterInfo
 * records.
 */

class RegStorage {
 public:
  enum RegStorageKind {
    kValidMask     = 0x8000,
    kValid         = 0x8000,
    kInvalid       = 0x0000,
    kShapeMask     = 0x0380,
    k32BitSolo     = 0x0080,
    k64BitSolo     = 0x0100,
    k64BitPair     = 0x0180,
    k128BitSolo    = 0x0200,
    k256BitSolo    = 0x0280,
    k512BitSolo    = 0x0300,
    k1024BitSolo   = 0x0380,
    k64BitMask     = 0x0300,
    k64Bits        = 0x0100,
    kShapeTypeMask = 0x03c0,
    kFloatingPoint = 0x0040,
    kCoreRegister  = 0x0000,
  };

  static const uint16_t kRegValMask  = 0x03ff;     // Num, type and shape.
  static const uint16_t kRegTypeMask = 0x007f;     // Num and type.
  static const uint16_t kRegNumMask  = 0x003f;     // Num only.
  static const uint16_t kHighRegNumMask = 0x001f;  // 0..31 for high reg
  static const uint16_t kMaxRegs     = kRegValMask + 1;
  // TODO: deprecate use of kInvalidRegVal and speed up GetReg().  Rely on valid bit instead.
  static const uint16_t kInvalidRegVal = 0x03ff;
  static const uint16_t kHighRegShift = 10;
  static const uint16_t kHighRegMask = (kHighRegNumMask << kHighRegShift);

  // Reg is [F][LLLLL], will override any existing shape and use rs_kind.
  constexpr RegStorage(RegStorageKind rs_kind, int reg)
      : reg_(
          DCHECK_CONSTEXPR(rs_kind != k64BitPair, , 0u)
          DCHECK_CONSTEXPR((rs_kind & ~kShapeMask) == 0, , 0u)
          kValid | rs_kind | (reg & kRegTypeMask)) {
  }
  constexpr RegStorage(RegStorageKind rs_kind, int low_reg, int high_reg)
      : reg_(
          DCHECK_CONSTEXPR(rs_kind == k64BitPair, << rs_kind, 0u)
          DCHECK_CONSTEXPR((low_reg & kFloatingPoint) == (high_reg & kFloatingPoint),
                           << low_reg << ", " << high_reg, 0u)
          DCHECK_CONSTEXPR((high_reg & kRegNumMask) <= kHighRegNumMask,
                           << "High reg must be in 0..31: " << high_reg, false)
          kValid | rs_kind | ((high_reg & kHighRegNumMask) << kHighRegShift) |
                  (low_reg & kRegTypeMask)) {
  }
  constexpr explicit RegStorage(uint16_t val) : reg_(val) {}
  RegStorage() : reg_(kInvalid) {}

  // We do not provide a general operator overload for equality of reg storage, as this is
  // dangerous in the case of architectures with multiple views, and the naming ExactEquals
  // expresses the exact match expressed here. It is more likely that a comparison between the views
  // is intended in most cases. Such code can be found in, for example, Mir2Lir::IsSameReg.
  //
  // If you know what you are doing, include reg_storage_eq.h, which defines == and != for brevity.

  bool ExactlyEquals(const RegStorage& rhs) const {
    return (reg_ == rhs.GetRawBits());
  }

  bool NotExactlyEquals(const RegStorage& rhs) const {
    return (reg_ != rhs.GetRawBits());
  }

  constexpr bool Valid() const {
    return ((reg_ & kValidMask) == kValid);
  }

  constexpr bool Is32Bit() const {
    return ((reg_ & kShapeMask) == k32BitSolo);
  }

  constexpr bool Is64Bit() const {
    return ((reg_ & k64BitMask) == k64Bits);
  }

  constexpr WideKind GetWideKind() const {
    return Is64Bit() ? kWide : kNotWide;
  }

  constexpr bool Is64BitSolo() const {
    return ((reg_ & kShapeMask) == k64BitSolo);
  }

  constexpr bool Is128BitSolo() const {
    return ((reg_ & kShapeMask) == k128BitSolo);
  }

  constexpr bool IsPair() const {
    return ((reg_ & kShapeMask) == k64BitPair);
  }

  constexpr bool IsFloat() const {
    return
        DCHECK_CONSTEXPR(Valid(), , false)
        ((reg_ & kFloatingPoint) == kFloatingPoint);
  }

  constexpr bool IsDouble() const {
    return
        DCHECK_CONSTEXPR(Valid(), , false)
        (reg_ & (kFloatingPoint | k64BitMask)) == (kFloatingPoint | k64Bits);
  }

  constexpr bool IsQuad() const {
    return
        DCHECK_CONSTEXPR(Valid(), , false)
        (reg_ & (kFloatingPoint | k128BitSolo)) == (kFloatingPoint | k128BitSolo);
  }

  constexpr bool IsSingle() const {
    return
        DCHECK_CONSTEXPR(Valid(), , false)
        (reg_ & (kFloatingPoint | k64BitMask)) == kFloatingPoint;
  }

  static constexpr bool IsFloat(uint16_t reg) {
    return ((reg & kFloatingPoint) == kFloatingPoint);
  }

  static constexpr bool IsDouble(uint16_t reg) {
    return (reg & (kFloatingPoint | k64BitMask)) == (kFloatingPoint | k64Bits);
  }

  static constexpr bool IsQuad(uint16_t reg) {
    return (reg & (kFloatingPoint | k128BitSolo)) == (kFloatingPoint | k128BitSolo);
  }

  static constexpr bool IsSingle(uint16_t reg) {
    return (reg & (kFloatingPoint | k64BitMask)) == kFloatingPoint;
  }

  static constexpr bool Is32Bit(uint16_t reg) {
    return ((reg & kShapeMask) == k32BitSolo);
  }

  static constexpr bool Is64Bit(uint16_t reg) {
    return ((reg & k64BitMask) == k64Bits);
  }

  static constexpr bool Is64BitSolo(uint16_t reg) {
    return ((reg & kShapeMask) == k64BitSolo);
  }

  // Used to retrieve either the low register of a pair, or the only register.
  int GetReg() const {
    DCHECK(!IsPair()) << "reg_ = 0x" << std::hex << reg_;
    return Valid() ? (reg_ & kRegValMask) : kInvalidRegVal;
  }

  // Sets shape, type and num of solo.
  void SetReg(int reg) {
    DCHECK(Valid());
    DCHECK(!IsPair());
    reg_ = (reg_ & ~kRegValMask) | reg;
  }

  // Set the reg number and type only, target remain 64-bit pair.
  void SetLowReg(int reg) {
    DCHECK(IsPair());
    reg_ = (reg_ & ~kRegTypeMask) | (reg & kRegTypeMask);
  }

  // Retrieve the least significant register of a pair and return as 32-bit solo.
  int GetLowReg() const {
    DCHECK(IsPair());
    return ((reg_ & kRegTypeMask) | k32BitSolo);
  }

  // Retrieve the low register num of a pair
  int GetLowRegNum() const {
    DCHECK(IsPair());
    return (reg_ & kRegNumMask);
  }

  // Create a stand-alone RegStorage from the low reg of a pair.
  RegStorage GetLow() const {
    DCHECK(IsPair());
    return RegStorage(k32BitSolo, reg_ & kRegTypeMask);
  }

  // Create a stand-alone RegStorage from the low 32bit of 64bit float solo.
  RegStorage GetLowFromFloatSolo64() const {
    DCHECK(IsFloat() && Is64BitSolo());
    return RegStorage(k32BitSolo, ((reg_ & kRegNumMask)<<1) | kFloatingPoint);
  }

  // Create a stand-alone RegStorage from the low 64bit of 128bit float solo.
  RegStorage GetLowFromFloatSolo128() const {
    DCHECK(IsFloat() && Is128BitSolo());
    return RegStorage(k64BitSolo, ((reg_ & kRegNumMask)<<1) | kFloatingPoint);
  }

  // Retrieve the most significant register of a pair.
  int GetHighReg() const {
    DCHECK(IsPair());
    return k32BitSolo | ((reg_ & kHighRegMask) >> kHighRegShift) | (reg_ & kFloatingPoint);
  }

  // Retrieve the high register num of a pair.
  int GetHighRegNum() const {
    DCHECK(IsPair());
    return ((reg_ & kHighRegMask) >> kHighRegShift);
  }

  // Create a stand-alone RegStorage from the high reg of a pair.
  RegStorage GetHigh() const {
    DCHECK(IsPair());
    return RegStorage(kValid | GetHighReg());
  }

  // Create a stand-alone RegStorage from the high 32bit of 64bit float solo.
  RegStorage GetHighFromFloatSolo64() const {
    DCHECK(IsFloat() && Is64BitSolo());
    return RegStorage(k32BitSolo, (((reg_ & kRegNumMask)<<1) +1) | kFloatingPoint);
  }

  // Create a stand-alone RegStorage from the high 64bit of 128bit float solo.
  RegStorage GetHighFromFloatSolo128() const {
    DCHECK(IsFloat() && Is128BitSolo());
    return RegStorage(k64BitSolo, (((reg_ & kRegNumMask)<<1) +1) | kFloatingPoint);
  }

  void SetHighReg(int reg) {
    DCHECK(IsPair());
    reg_ = (reg_ & ~kHighRegMask) | ((reg & kHighRegNumMask) << kHighRegShift);
  }

  // Return the register number of low or solo.
  constexpr int GetRegNum() const {
    return reg_ & kRegNumMask;
  }

  // Is register number in 0..7?
  constexpr bool Low8() const {
    return GetRegNum() < 8;
  }

  // Is register number in 0..3?
  constexpr bool Low4() const {
    return GetRegNum() < 4;
  }

  // Combine 2 32-bit solo regs into a pair.
  static RegStorage MakeRegPair(RegStorage low, RegStorage high) {
    DCHECK(!low.IsPair());
    DCHECK(low.Is32Bit());
    DCHECK(!high.IsPair());
    DCHECK(high.Is32Bit());
    return RegStorage(k64BitPair, low.GetReg(), high.GetReg());
  }

  static constexpr bool SameRegType(RegStorage reg1, RegStorage reg2) {
    return ((reg1.reg_ & kShapeTypeMask) == (reg2.reg_ & kShapeTypeMask));
  }

  static constexpr bool SameRegType(int reg1, int reg2) {
    return ((reg1 & kShapeTypeMask) == (reg2 & kShapeTypeMask));
  }

  // Create a 32-bit solo.
  static RegStorage Solo32(int reg_num) {
    return RegStorage(k32BitSolo, reg_num & kRegTypeMask);
  }

  // Create a floating point 32-bit solo.
  static constexpr RegStorage FloatSolo32(int reg_num) {
    return RegStorage(k32BitSolo, (reg_num & kRegNumMask) | kFloatingPoint);
  }

  // Create a 128-bit solo.
  static constexpr RegStorage Solo128(int reg_num) {
    return RegStorage(k128BitSolo, reg_num & kRegTypeMask);
  }

  // Create a 64-bit solo.
  static constexpr RegStorage Solo64(int reg_num) {
    return RegStorage(k64BitSolo, reg_num & kRegTypeMask);
  }

  // Create a floating point 64-bit solo.
  static RegStorage FloatSolo64(int reg_num) {
    return RegStorage(k64BitSolo, (reg_num & kRegNumMask) | kFloatingPoint);
  }

  static constexpr RegStorage InvalidReg() {
    return RegStorage(kInvalid);
  }

  static constexpr uint16_t RegNum(int raw_reg_bits) {
    return raw_reg_bits & kRegNumMask;
  }

  constexpr int GetRawBits() const {
    return reg_;
  }

  size_t StorageSize() const {
    switch (reg_ & kShapeMask) {
      case kInvalid: return 0;
      case k32BitSolo: return 4;
      case k64BitSolo: return 8;
      case k64BitPair: return 8;  // Is this useful?  Might want to disallow taking size of pair.
      case k128BitSolo: return 16;
      case k256BitSolo: return 32;
      case k512BitSolo: return 64;
      case k1024BitSolo: return 128;
      default: LOG(FATAL) << "Unexpected shape";
    }
    return 0;
  }

 private:
  uint16_t reg_;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_REG_STORAGE_H_
