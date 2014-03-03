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


namespace art {

/*
 * Representation of the physical register, register pair or vector holding a Dalvik value.
 * The basic configuration of the storage (i.e. solo reg, pair, vector) is common across all
 * targets, but the encoding of the actual storage element is target independent.
 *
 * The two most-significant bits describe the basic shape of the storage, while meaning of the
 * lower 14 bits depends on the shape:
 *
 *  [PW]
 *       P: 0 -> pair, 1 -> solo (or vector)
 *       W: 1 -> 64 bits, 0 -> 32 bits
 *
 *  [00] [xxxxxxxxxxxxxx]     Invalid (typically all zeros)
 *  [01] [HHHHHHH] [LLLLLLL]  64-bit storage, composed of 2 32-bit registers
 *  [10] [0] [xxxxxx] [RRRRRRR]  32-bit solo register
 *  [11] [0] [xxxxxx] [RRRRRRR]  64-bit solo register
 *  [10] [1] [xxxxxx] [VVVVVVV]  32-bit vector storage
 *  [11] [1] [xxxxxx] [VVVVVVV]  64-bit vector storage
 *
 * x - don't care
 * L - low register number of a pair
 * H - high register number of a pair
 * R - register number of a solo reg
 * V - vector description
 *
 * Note that in all non-invalid cases, the low 7 bits must be sufficient to describe
 * whether the storage element is floating point (see IsFloatReg()).
 *
 */

class RegStorage {
 public:
  enum RegStorageKind {
    kInvalid     = 0x0000,
    k64BitPair   = 0x4000,
    k32BitSolo   = 0x8000,
    k64BitSolo   = 0xc000,
    k32BitVector = 0xa000,
    k64BitVector = 0xe000,
    kPairMask    = 0x8000,
    kPair        = 0x0000,
    kSizeMask    = 0x4000,
    k64Bit       = 0x4000,
    k32Bit       = 0x0000,
    kVectorMask  = 0xa000,
    kVector      = 0xa000,
    kSolo        = 0x8000,
    kShapeMask   = 0xc000,
    kKindMask    = 0xe000
  };

  static const uint16_t kRegValMask = 0x007f;
  static const uint16_t kHighRegShift = 7;
  static const uint16_t kHighRegMask = kRegValMask << kHighRegShift;

  RegStorage(RegStorageKind rs_kind, int reg) {
    DCHECK_NE(rs_kind & kShapeMask, kInvalid);
    DCHECK_NE(rs_kind & kShapeMask, k64BitPair);
    DCHECK_EQ(rs_kind & ~kKindMask, 0);
    DCHECK_EQ(reg & ~kRegValMask, 0);
    reg_ = rs_kind | reg;
  }
  RegStorage(RegStorageKind rs_kind, int low_reg, int high_reg) {
    DCHECK_EQ(rs_kind, k64BitPair);
    DCHECK_EQ(low_reg & ~kRegValMask, 0);
    DCHECK_EQ(high_reg & ~kRegValMask, 0);
    reg_ = rs_kind | (high_reg << kHighRegShift) | low_reg;
  }
  explicit RegStorage(uint16_t val) : reg_(val) {}
  RegStorage() : reg_(kInvalid) {}
  ~RegStorage() {}

  bool IsInvalid() const {
    return ((reg_ & kShapeMask) == kInvalid);
  }

  bool Is32Bit() const {
    DCHECK(!IsInvalid());
    return ((reg_ & kSizeMask) == k32Bit);
  }

  bool Is64Bit() const {
    DCHECK(!IsInvalid());
    return ((reg_ & kSizeMask) == k64Bit);
  }

  bool IsPair() const {
    DCHECK(!IsInvalid());
    return ((reg_ & kPairMask) == kPair);
  }

  bool IsSolo() const {
    DCHECK(!IsInvalid());
    return ((reg_ & kVectorMask) == kSolo);
  }

  bool IsVector() const {
    DCHECK(!IsInvalid());
    return ((reg_ & kVectorMask) == kVector);
  }

  // Used to retrieve either the low register of a pair, or the only register.
  int GetReg() const {
    DCHECK(!IsInvalid());
    return (reg_ & kRegValMask);
  }

  void SetReg(int reg) {
    DCHECK(!IsInvalid());
    reg_ = (reg_ & ~kRegValMask) | reg;
    DCHECK_EQ(GetReg(), reg);
  }

  // Retrieve the most significant register of a pair.
  int GetHighReg() const {
    DCHECK(IsPair());
    return (reg_ & kHighRegMask) >> kHighRegShift;
  }

  void SetHighReg(int reg) {
    DCHECK(IsPair());
    reg_ = (reg_ & ~kHighRegMask) | (reg << kHighRegShift);
    DCHECK_EQ(GetHighReg(), reg);
  }

  int GetRawBits() const {
    return reg_;
  }

 private:
  uint16_t reg_;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_REG_STORAGE_H_
