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

#ifndef ART_COMPILER_DWARF_DEBUG_FRAME_OPCODE_WRITER_H_
#define ART_COMPILER_DWARF_DEBUG_FRAME_OPCODE_WRITER_H_

#include "dwarf.h"
#include "register.h"
#include "writer.h"

namespace art {
namespace dwarf {

// Writer for .debug_frame opcodes (DWARF-3).
// See the DWARF specification for the precise meaning of the opcodes.
// The writer is very light-weight, however it will do the following for you:
//  * Choose the most compact encoding of a given opcode.
//  * Keep track of current state and convert absolute values to deltas.
//  * Divide by header-defined factors as appropriate.
template<typename Allocator = std::allocator<uint8_t> >
class DebugFrameOpCodeWriter : private Writer<Allocator> {
 public:
  // To save space, DWARF divides most offsets by header-defined factors.
  // They are used in integer divisions, so we make them constants.
  // We usually subtract from stack base pointer, so making the factor
  // negative makes the encoded values positive and thus easier to encode.
  static constexpr int kDataAlignmentFactor = -4;
  static constexpr int kCodeAlignmentFactor = 1;

  // Explicitely advance the program counter to given location.
  void AdvancePC(int absolute_pc) {
    DCHECK_GE(absolute_pc, current_pc_);
    int delta = FactorCodeOffset(absolute_pc - current_pc_);
    if (delta != 0) {
      if (delta <= 0x3F) {
        this->PushUint8(DW_CFA_advance_loc | delta);
      } else if (delta <= UINT8_MAX) {
        this->PushUint8(DW_CFA_advance_loc1);
        this->PushUint8(delta);
      } else if (delta <= UINT16_MAX) {
        this->PushUint8(DW_CFA_advance_loc2);
        this->PushUint16(delta);
      } else {
        this->PushUint8(DW_CFA_advance_loc4);
        this->PushUint32(delta);
      }
    }
    current_pc_ = absolute_pc;
  }

  // Override this method to automatically advance the PC before each opcode.
  virtual void ImplicitlyAdvancePC() { }

  // Common alias in assemblers - spill relative to current stack pointer.
  void RelOffset(Reg reg, int offset) {
    Offset(reg, offset - current_cfa_offset_);
  }

  // Common alias in assemblers - increase stack frame size.
  void AdjustCFAOffset(int delta) {
    DefCFAOffset(current_cfa_offset_ + delta);
  }

  // Custom alias - spill many registers based on bitmask.
  void RelOffsetForMany(Reg reg_base, int offset, uint32_t reg_mask,
                        int reg_size) {
    DCHECK(reg_size == 4 || reg_size == 8);
    for (int i = 0; reg_mask != 0u; reg_mask >>= 1, i++) {
      if ((reg_mask & 1) != 0u) {
        RelOffset(Reg(reg_base.num() + i), offset);
        offset += reg_size;
      }
    }
  }

  // Custom alias - unspill many registers based on bitmask.
  void RestoreMany(Reg reg_base, uint32_t reg_mask) {
    for (int i = 0; reg_mask != 0u; reg_mask >>= 1, i++) {
      if ((reg_mask & 1) != 0u) {
        Restore(Reg(reg_base.num() + i));
      }
    }
  }

  void Nop() {
    this->PushUint8(DW_CFA_nop);
  }

  void Offset(Reg reg, int offset) {
    ImplicitlyAdvancePC();
    int factored_offset = FactorDataOffset(offset);  // May change sign.
    if (factored_offset >= 0) {
      if (0 <= reg.num() && reg.num() <= 0x3F) {
        this->PushUint8(DW_CFA_offset | reg.num());
        this->PushUleb128(factored_offset);
      } else {
        this->PushUint8(DW_CFA_offset_extended);
        this->PushUleb128(reg.num());
        this->PushUleb128(factored_offset);
      }
    } else {
      uses_dwarf3_features_ = true;
      this->PushUint8(DW_CFA_offset_extended_sf);
      this->PushUleb128(reg.num());
      this->PushSleb128(factored_offset);
    }
  }

  void Restore(Reg reg) {
    ImplicitlyAdvancePC();
    if (0 <= reg.num() && reg.num() <= 0x3F) {
      this->PushUint8(DW_CFA_restore | reg.num());
    } else {
      this->PushUint8(DW_CFA_restore_extended);
      this->PushUleb128(reg.num());
    }
  }

  void Undefined(Reg reg) {
    ImplicitlyAdvancePC();
    this->PushUint8(DW_CFA_undefined);
    this->PushUleb128(reg.num());
  }

  void SameValue(Reg reg) {
    ImplicitlyAdvancePC();
    this->PushUint8(DW_CFA_same_value);
    this->PushUleb128(reg.num());
  }

  // The previous value of "reg" is stored in register "new_reg".
  void Register(Reg reg, Reg new_reg) {
    ImplicitlyAdvancePC();
    this->PushUint8(DW_CFA_register);
    this->PushUleb128(reg.num());
    this->PushUleb128(new_reg.num());
  }

  void RememberState() {
    ImplicitlyAdvancePC();
    this->PushUint8(DW_CFA_remember_state);
  }

  void RestoreState() {
    ImplicitlyAdvancePC();
    this->PushUint8(DW_CFA_restore_state);
  }

  void DefCFA(Reg reg, int offset) {
    ImplicitlyAdvancePC();
    if (offset >= 0) {
      this->PushUint8(DW_CFA_def_cfa);
      this->PushUleb128(reg.num());
      this->PushUleb128(offset);  // Non-factored.
    } else {
      uses_dwarf3_features_ = true;
      this->PushUint8(DW_CFA_def_cfa_sf);
      this->PushUleb128(reg.num());
      this->PushSleb128(FactorDataOffset(offset));
    }
    current_cfa_offset_ = offset;
  }

  void DefCFARegister(Reg reg) {
    ImplicitlyAdvancePC();
    this->PushUint8(DW_CFA_def_cfa_register);
    this->PushUleb128(reg.num());
  }

  void DefCFAOffset(int offset) {
    if (current_cfa_offset_ != offset) {
      ImplicitlyAdvancePC();
      if (offset >= 0) {
        this->PushUint8(DW_CFA_def_cfa_offset);
        this->PushUleb128(offset);  // Non-factored.
      } else {
        uses_dwarf3_features_ = true;
        this->PushUint8(DW_CFA_def_cfa_offset_sf);
        this->PushSleb128(FactorDataOffset(offset));
      }
      current_cfa_offset_ = offset;
    }
  }

  void ValOffset(Reg reg, int offset) {
    ImplicitlyAdvancePC();
    uses_dwarf3_features_ = true;
    int factored_offset = FactorDataOffset(offset);  // May change sign.
    if (factored_offset >= 0) {
      this->PushUint8(DW_CFA_val_offset);
      this->PushUleb128(reg.num());
      this->PushUleb128(factored_offset);
    } else {
      this->PushUint8(DW_CFA_val_offset_sf);
      this->PushUleb128(reg.num());
      this->PushSleb128(factored_offset);
    }
  }

  void DefCFAExpression(void* expr, int expr_size) {
    ImplicitlyAdvancePC();
    uses_dwarf3_features_ = true;
    this->PushUint8(DW_CFA_def_cfa_expression);
    this->PushUleb128(expr_size);
    this->PushData(expr, expr_size);
  }

  void Expression(Reg reg, void* expr, int expr_size) {
    ImplicitlyAdvancePC();
    uses_dwarf3_features_ = true;
    this->PushUint8(DW_CFA_expression);
    this->PushUleb128(reg.num());
    this->PushUleb128(expr_size);
    this->PushData(expr, expr_size);
  }

  void ValExpression(Reg reg, void* expr, int expr_size) {
    ImplicitlyAdvancePC();
    uses_dwarf3_features_ = true;
    this->PushUint8(DW_CFA_val_expression);
    this->PushUleb128(reg.num());
    this->PushUleb128(expr_size);
    this->PushData(expr, expr_size);
  }

  int GetCurrentPC() const {
    return current_pc_;
  }

  int GetCurrentCFAOffset() const {
    return current_cfa_offset_;
  }

  void SetCurrentCFAOffset(int offset) {
    current_cfa_offset_ = offset;
  }

  using Writer<Allocator>::data;

  DebugFrameOpCodeWriter(const Allocator& alloc = Allocator())
      : Writer<Allocator>(&opcodes_),
        opcodes_(alloc),
        current_cfa_offset_(0),
        current_pc_(0),
        uses_dwarf3_features_(false) {
  }

  virtual ~DebugFrameOpCodeWriter() { }

 protected:
  int FactorDataOffset(int offset) const {
    DCHECK_EQ(offset % kDataAlignmentFactor, 0);
    return offset / kDataAlignmentFactor;
  }

  int FactorCodeOffset(int offset) const {
    DCHECK_EQ(offset % kCodeAlignmentFactor, 0);
    return offset / kCodeAlignmentFactor;
  }

  std::vector<uint8_t, Allocator> opcodes_;
  int current_cfa_offset_;
  int current_pc_;
  bool uses_dwarf3_features_;

 private:
  DISALLOW_COPY_AND_ASSIGN(DebugFrameOpCodeWriter);
};

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_DWARF_DEBUG_FRAME_OPCODE_WRITER_H_
