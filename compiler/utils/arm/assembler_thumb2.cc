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

#include <type_traits>

#include "assembler_thumb2.h"

#include "base/bit_utils.h"
#include "base/logging.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "offsets.h"
#include "thread.h"

namespace art {
namespace arm {

template <typename Function>
void Thumb2Assembler::Fixup::ForExpandableDependencies(Thumb2Assembler* assembler, Function fn) {
  static_assert(
      std::is_same<typename std::result_of<Function(FixupId, FixupId)>::type, void>::value,
      "Incorrect signature for argument `fn`: expected (FixupId, FixupId) -> void");
  Fixup* fixups = assembler->fixups_.data();
  for (FixupId fixup_id = 0u, end_id = assembler->fixups_.size(); fixup_id != end_id; ++fixup_id) {
    uint32_t target = fixups[fixup_id].target_;
    if (target > fixups[fixup_id].location_) {
      for (FixupId id = fixup_id + 1u; id != end_id && fixups[id].location_ < target; ++id) {
        if (fixups[id].CanExpand()) {
          fn(id, fixup_id);
        }
      }
    } else {
      for (FixupId id = fixup_id; id != 0u && fixups[id - 1u].location_ >= target; --id) {
        if (fixups[id - 1u].CanExpand()) {
          fn(id - 1u, fixup_id);
        }
      }
    }
  }
}

void Thumb2Assembler::Fixup::PrepareDependents(Thumb2Assembler* assembler) {
  // For each Fixup, it's easy to find the Fixups that it depends on as they are either
  // the following or the preceding Fixups until we find the target. However, for fixup
  // adjustment we need the reverse lookup, i.e. what Fixups depend on a given Fixup.
  // This function creates a compact representation of this relationship, where we have
  // all the dependents in a single array and Fixups reference their ranges by start
  // index and count. (Instead of having a per-fixup vector.)

  // Count the number of dependents of each Fixup.
  Fixup* fixups = assembler->fixups_.data();
  ForExpandableDependencies(
      assembler,
      [fixups](FixupId dependency, FixupId dependent ATTRIBUTE_UNUSED) {
        fixups[dependency].dependents_count_ += 1u;
      });
  // Assign index ranges in fixup_dependents_ to individual fixups. Record the end of the
  // range in dependents_start_, we shall later decrement it as we fill in fixup_dependents_.
  uint32_t number_of_dependents = 0u;
  for (FixupId fixup_id = 0u, end_id = assembler->fixups_.size(); fixup_id != end_id; ++fixup_id) {
    number_of_dependents += fixups[fixup_id].dependents_count_;
    fixups[fixup_id].dependents_start_ = number_of_dependents;
  }
  if (number_of_dependents == 0u) {
    return;
  }
  // Create and fill in the fixup_dependents_.
  assembler->fixup_dependents_.resize(number_of_dependents);
  FixupId* dependents = assembler->fixup_dependents_.data();
  ForExpandableDependencies(
      assembler,
      [fixups, dependents](FixupId dependency, FixupId dependent) {
        fixups[dependency].dependents_start_ -= 1u;
        dependents[fixups[dependency].dependents_start_] = dependent;
      });
}

void Thumb2Assembler::BindLabel(Label* label, uint32_t bound_pc) {
  CHECK(!label->IsBound());

  while (label->IsLinked()) {
    FixupId fixup_id = label->Position();                     // The id for linked Fixup.
    Fixup* fixup = GetFixup(fixup_id);                        // Get the Fixup at this id.
    fixup->Resolve(bound_pc);                                 // Fixup can be resolved now.
    uint32_t fixup_location = fixup->GetLocation();
    uint16_t next = buffer_.Load<uint16_t>(fixup_location);   // Get next in chain.
    buffer_.Store<int16_t>(fixup_location, 0);
    label->position_ = next;                                  // Move to next.
  }
  label->BindTo(bound_pc);
}

uint32_t Thumb2Assembler::BindLiterals() {
  // We don't add the padding here, that's done only after adjusting the Fixup sizes.
  uint32_t code_size = buffer_.Size();
  for (Literal& lit : literals_) {
    Label* label = lit.GetLabel();
    BindLabel(label, code_size);
    code_size += lit.GetSize();
  }
  return code_size;
}

void Thumb2Assembler::BindJumpTables(uint32_t code_size) {
  for (JumpTable& table : jump_tables_) {
    Label* label = table.GetLabel();
    BindLabel(label, code_size);
    code_size += table.GetSize();
  }
}

void Thumb2Assembler::AdjustFixupIfNeeded(Fixup* fixup, uint32_t* current_code_size,
                                          std::deque<FixupId>* fixups_to_recalculate) {
  uint32_t adjustment = fixup->AdjustSizeIfNeeded(*current_code_size);
  if (adjustment != 0u) {
    DCHECK(fixup->CanExpand());
    *current_code_size += adjustment;
    for (FixupId dependent_id : fixup->Dependents(*this)) {
      Fixup* dependent = GetFixup(dependent_id);
      dependent->IncreaseAdjustment(adjustment);
      if (buffer_.Load<int16_t>(dependent->GetLocation()) == 0) {
        buffer_.Store<int16_t>(dependent->GetLocation(), 1);
        fixups_to_recalculate->push_back(dependent_id);
      }
    }
  }
}

uint32_t Thumb2Assembler::AdjustFixups() {
  Fixup::PrepareDependents(this);
  uint32_t current_code_size = buffer_.Size();
  std::deque<FixupId> fixups_to_recalculate;
  if (kIsDebugBuild) {
    // We will use the placeholders in the buffer_ to mark whether the fixup has
    // been added to the fixups_to_recalculate. Make sure we start with zeros.
    for (Fixup& fixup : fixups_) {
      CHECK_EQ(buffer_.Load<int16_t>(fixup.GetLocation()), 0);
    }
  }
  for (Fixup& fixup : fixups_) {
    AdjustFixupIfNeeded(&fixup, &current_code_size, &fixups_to_recalculate);
  }
  while (!fixups_to_recalculate.empty()) {
    do {
      // Pop the fixup.
      FixupId fixup_id = fixups_to_recalculate.front();
      fixups_to_recalculate.pop_front();
      Fixup* fixup = GetFixup(fixup_id);
      DCHECK_NE(buffer_.Load<int16_t>(fixup->GetLocation()), 0);
      buffer_.Store<int16_t>(fixup->GetLocation(), 0);
      // See if it needs adjustment.
      AdjustFixupIfNeeded(fixup, &current_code_size, &fixups_to_recalculate);
    } while (!fixups_to_recalculate.empty());

    if ((current_code_size & 2) != 0 && (!literals_.empty() || !jump_tables_.empty())) {
      // If we need to add padding before literals, this may just push some out of range,
      // so recalculate all load literals. This makes up for the fact that we don't mark
      // load literal as a dependency of all previous Fixups even though it actually is.
      for (Fixup& fixup : fixups_) {
        if (fixup.IsLoadLiteral()) {
          AdjustFixupIfNeeded(&fixup, &current_code_size, &fixups_to_recalculate);
        }
      }
    }
  }
  if (kIsDebugBuild) {
    // Check that no fixup is marked as being in fixups_to_recalculate anymore.
    for (Fixup& fixup : fixups_) {
      CHECK_EQ(buffer_.Load<int16_t>(fixup.GetLocation()), 0);
    }
  }

  // Adjust literal pool labels for padding.
  DCHECK_ALIGNED(current_code_size, 2);
  uint32_t literals_adjustment = current_code_size + (current_code_size & 2) - buffer_.Size();
  if (literals_adjustment != 0u) {
    for (Literal& literal : literals_) {
      Label* label = literal.GetLabel();
      DCHECK(label->IsBound());
      int old_position = label->Position();
      label->Reinitialize();
      label->BindTo(old_position + literals_adjustment);
    }
    for (JumpTable& table : jump_tables_) {
      Label* label = table.GetLabel();
      DCHECK(label->IsBound());
      int old_position = label->Position();
      label->Reinitialize();
      label->BindTo(old_position + literals_adjustment);
    }
  }

  return current_code_size;
}

void Thumb2Assembler::EmitFixups(uint32_t adjusted_code_size) {
  // Move non-fixup code to its final place and emit fixups.
  // Process fixups in reverse order so that we don't repeatedly move the same data.
  size_t src_end = buffer_.Size();
  size_t dest_end = adjusted_code_size;
  buffer_.Resize(dest_end);
  DCHECK_GE(dest_end, src_end);
  for (auto i = fixups_.rbegin(), end = fixups_.rend(); i != end; ++i) {
    Fixup* fixup = &*i;
    if (fixup->GetOriginalSize() == fixup->GetSize()) {
      // The size of this Fixup didn't change. To avoid moving the data
      // in small chunks, emit the code to its original position.
      fixup->Emit(&buffer_, adjusted_code_size);
      fixup->Finalize(dest_end - src_end);
    } else {
      // Move the data between the end of the fixup and src_end to its final location.
      size_t old_fixup_location = fixup->GetLocation();
      size_t src_begin = old_fixup_location + fixup->GetOriginalSizeInBytes();
      size_t data_size = src_end - src_begin;
      size_t dest_begin  = dest_end - data_size;
      buffer_.Move(dest_begin, src_begin, data_size);
      src_end = old_fixup_location;
      dest_end = dest_begin - fixup->GetSizeInBytes();
      // Finalize the Fixup and emit the data to the new location.
      fixup->Finalize(dest_end - src_end);
      fixup->Emit(&buffer_, adjusted_code_size);
    }
  }
  CHECK_EQ(src_end, dest_end);
}

void Thumb2Assembler::EmitLiterals() {
  if (!literals_.empty()) {
    // Load literal instructions (LDR, LDRD, VLDR) require 4-byte alignment.
    // We don't support byte and half-word literals.
    uint32_t code_size = buffer_.Size();
    DCHECK_ALIGNED(code_size, 2);
    if ((code_size & 2u) != 0u) {
      Emit16(0);
    }
    for (Literal& literal : literals_) {
      AssemblerBuffer::EnsureCapacity ensured(&buffer_);
      DCHECK_EQ(static_cast<size_t>(literal.GetLabel()->Position()), buffer_.Size());
      DCHECK(literal.GetSize() == 4u || literal.GetSize() == 8u);
      for (size_t i = 0, size = literal.GetSize(); i != size; ++i) {
        buffer_.Emit<uint8_t>(literal.GetData()[i]);
      }
    }
  }
}

void Thumb2Assembler::EmitJumpTables() {
  if (!jump_tables_.empty()) {
    // Jump tables require 4 byte alignment. (We don't support byte and half-word jump tables.)
    uint32_t code_size = buffer_.Size();
    DCHECK_ALIGNED(code_size, 2);
    if ((code_size & 2u) != 0u) {
      Emit16(0);
    }
    for (JumpTable& table : jump_tables_) {
      // Bulk ensure capacity, as this may be large.
      size_t orig_size = buffer_.Size();
      size_t required_capacity = orig_size + table.GetSize();
      if (required_capacity > buffer_.Capacity()) {
        buffer_.ExtendCapacity(required_capacity);
      }
#ifndef NDEBUG
      buffer_.has_ensured_capacity_ = true;
#endif

      DCHECK_EQ(static_cast<size_t>(table.GetLabel()->Position()), buffer_.Size());
      int32_t anchor_position = table.GetAnchorLabel()->Position() + 4;

      for (Label* target : table.GetData()) {
        // Ensure that the label was tracked, so that it will have the right position.
        DCHECK(std::find(tracked_labels_.begin(), tracked_labels_.end(), target) !=
                   tracked_labels_.end());

        int32_t offset = target->Position() - anchor_position;
        buffer_.Emit<int32_t>(offset);
      }

#ifndef NDEBUG
      buffer_.has_ensured_capacity_ = false;
#endif
      size_t new_size = buffer_.Size();
      DCHECK_LE(new_size - orig_size, table.GetSize());
    }
  }
}

void Thumb2Assembler::PatchCFI() {
  if (cfi().NumberOfDelayedAdvancePCs() == 0u) {
    return;
  }

  typedef DebugFrameOpCodeWriterForAssembler::DelayedAdvancePC DelayedAdvancePC;
  const auto data = cfi().ReleaseStreamAndPrepareForDelayedAdvancePC();
  const std::vector<uint8_t>& old_stream = data.first;
  const std::vector<DelayedAdvancePC>& advances = data.second;

  // Refill our data buffer with patched opcodes.
  cfi().ReserveCFIStream(old_stream.size() + advances.size() + 16);
  size_t stream_pos = 0;
  for (const DelayedAdvancePC& advance : advances) {
    DCHECK_GE(advance.stream_pos, stream_pos);
    // Copy old data up to the point where advance was issued.
    cfi().AppendRawData(old_stream, stream_pos, advance.stream_pos);
    stream_pos = advance.stream_pos;
    // Insert the advance command with its final offset.
    size_t final_pc = GetAdjustedPosition(advance.pc);
    cfi().AdvancePC(final_pc);
  }
  // Copy the final segment if any.
  cfi().AppendRawData(old_stream, stream_pos, old_stream.size());
}

inline int16_t Thumb2Assembler::BEncoding16(int32_t offset, Condition cond) {
  DCHECK_ALIGNED(offset, 2);
  int16_t encoding = B15 | B14;
  if (cond != AL) {
    DCHECK(IsInt<9>(offset));
    encoding |= B12 |  (static_cast<int32_t>(cond) << 8) | ((offset >> 1) & 0xff);
  } else {
    DCHECK(IsInt<12>(offset));
    encoding |= B13 | ((offset >> 1) & 0x7ff);
  }
  return encoding;
}

inline int32_t Thumb2Assembler::BEncoding32(int32_t offset, Condition cond) {
  DCHECK_ALIGNED(offset, 2);
  int32_t s = (offset >> 31) & 1;   // Sign bit.
  int32_t encoding = B31 | B30 | B29 | B28 | B15 |
      (s << 26) |                   // Sign bit goes to bit 26.
      ((offset >> 1) & 0x7ff);      // imm11 goes to bits 0-10.
  if (cond != AL) {
    DCHECK(IsInt<21>(offset));
    // Encode cond, move imm6 from bits 12-17 to bits 16-21 and move J1 and J2.
    encoding |= (static_cast<int32_t>(cond) << 22) | ((offset & 0x3f000) << (16 - 12)) |
        ((offset & (1 << 19)) >> (19 - 13)) |   // Extract J1 from bit 19 to bit 13.
        ((offset & (1 << 18)) >> (18 - 11));    // Extract J2 from bit 18 to bit 11.
  } else {
    DCHECK(IsInt<25>(offset));
    int32_t j1 = ((offset >> 23) ^ s ^ 1) & 1;  // Calculate J1 from I1 extracted from bit 23.
    int32_t j2 = ((offset >> 22)^ s ^ 1) & 1;   // Calculate J2 from I2 extracted from bit 22.
    // Move imm10 from bits 12-21 to bits 16-25 and add J1 and J2.
    encoding |= B12 | ((offset & 0x3ff000) << (16 - 12)) |
        (j1 << 13) | (j2 << 11);
  }
  return encoding;
}

inline int16_t Thumb2Assembler::CbxzEncoding16(Register rn, int32_t offset, Condition cond) {
  DCHECK(!IsHighRegister(rn));
  DCHECK_ALIGNED(offset, 2);
  DCHECK(IsUint<7>(offset));
  DCHECK(cond == EQ || cond == NE);
  return B15 | B13 | B12 | B8 | (cond == NE ? B11 : 0) | static_cast<int32_t>(rn) |
      ((offset & 0x3e) << (3 - 1)) |    // Move imm5 from bits 1-5 to bits 3-7.
      ((offset & 0x40) << (9 - 6));     // Move i from bit 6 to bit 11
}

inline int16_t Thumb2Assembler::CmpRnImm8Encoding16(Register rn, int32_t value) {
  DCHECK(!IsHighRegister(rn));
  DCHECK(IsUint<8>(value));
  return B13 | B11 | (rn << 8) | value;
}

inline int16_t Thumb2Assembler::AddRdnRmEncoding16(Register rdn, Register rm) {
  // The high bit of rn is moved across 4-bit rm.
  return B14 | B10 | (static_cast<int32_t>(rm) << 3) |
      (static_cast<int32_t>(rdn) & 7) | ((static_cast<int32_t>(rdn) & 8) << 4);
}

inline int32_t Thumb2Assembler::MovwEncoding32(Register rd, int32_t value) {
  DCHECK(IsUint<16>(value));
  return B31 | B30 | B29 | B28 | B25 | B22 |
      (static_cast<int32_t>(rd) << 8) |
      ((value & 0xf000) << (16 - 12)) |   // Move imm4 from bits 12-15 to bits 16-19.
      ((value & 0x0800) << (26 - 11)) |   // Move i from bit 11 to bit 26.
      ((value & 0x0700) << (12 - 8)) |    // Move imm3 from bits 8-10 to bits 12-14.
      (value & 0xff);                     // Keep imm8 in bits 0-7.
}

inline int32_t Thumb2Assembler::MovtEncoding32(Register rd, int32_t value) {
  DCHECK_EQ(value & 0xffff, 0);
  int32_t movw_encoding = MovwEncoding32(rd, (value >> 16) & 0xffff);
  return movw_encoding | B25 | B23;
}

inline int32_t Thumb2Assembler::MovModImmEncoding32(Register rd, int32_t value) {
  uint32_t mod_imm = ModifiedImmediate(value);
  DCHECK_NE(mod_imm, kInvalidModifiedImmediate);
  return B31 | B30 | B29 | B28 | B22 | B19 | B18 | B17 | B16 |
      (static_cast<int32_t>(rd) << 8) | static_cast<int32_t>(mod_imm);
}

inline int16_t Thumb2Assembler::LdrLitEncoding16(Register rt, int32_t offset) {
  DCHECK(!IsHighRegister(rt));
  DCHECK_ALIGNED(offset, 4);
  DCHECK(IsUint<10>(offset));
  return B14 | B11 | (static_cast<int32_t>(rt) << 8) | (offset >> 2);
}

inline int32_t Thumb2Assembler::LdrLitEncoding32(Register rt, int32_t offset) {
  // NOTE: We don't support negative offset, i.e. U=0 (B23).
  return LdrRtRnImm12Encoding(rt, PC, offset);
}

inline int32_t Thumb2Assembler::LdrdEncoding32(Register rt, Register rt2, Register rn, int32_t offset) {
  DCHECK_ALIGNED(offset, 4);
  CHECK(IsUint<10>(offset));
  return B31 | B30 | B29 | B27 |
      B24 /* P = 1 */ | B23 /* U = 1 */ | B22 | 0 /* W = 0 */ | B20 |
      (static_cast<int32_t>(rn) << 16) | (static_cast<int32_t>(rt) << 12) |
      (static_cast<int32_t>(rt2) << 8) | (offset >> 2);
}

inline int32_t Thumb2Assembler::VldrsEncoding32(SRegister sd, Register rn, int32_t offset) {
  DCHECK_ALIGNED(offset, 4);
  CHECK(IsUint<10>(offset));
  return B31 | B30 | B29 | B27 | B26 | B24 |
      B23 /* U = 1 */ | B20 | B11 | B9 |
      (static_cast<int32_t>(rn) << 16) |
      ((static_cast<int32_t>(sd) & 0x01) << (22 - 0)) |   // Move D from bit 0 to bit 22.
      ((static_cast<int32_t>(sd) & 0x1e) << (12 - 1)) |   // Move Vd from bits 1-4 to bits 12-15.
      (offset >> 2);
}

inline int32_t Thumb2Assembler::VldrdEncoding32(DRegister dd, Register rn, int32_t offset) {
  DCHECK_ALIGNED(offset, 4);
  CHECK(IsUint<10>(offset));
  return B31 | B30 | B29 | B27 | B26 | B24 |
      B23 /* U = 1 */ | B20 | B11 | B9 | B8 |
      (rn << 16) |
      ((static_cast<int32_t>(dd) & 0x10) << (22 - 4)) |   // Move D from bit 4 to bit 22.
      ((static_cast<int32_t>(dd) & 0x0f) << (12 - 0)) |   // Move Vd from bits 0-3 to bits 12-15.
      (offset >> 2);
}

inline int16_t Thumb2Assembler::LdrRtRnImm5Encoding16(Register rt, Register rn, int32_t offset) {
  DCHECK(!IsHighRegister(rt));
  DCHECK(!IsHighRegister(rn));
  DCHECK_ALIGNED(offset, 4);
  DCHECK(IsUint<7>(offset));
  return B14 | B13 | B11 |
      (static_cast<int32_t>(rn) << 3) | static_cast<int32_t>(rt) |
      (offset << (6 - 2));                // Move imm5 from bits 2-6 to bits 6-10.
}

int32_t Thumb2Assembler::Fixup::LoadWideOrFpEncoding(Register rbase, int32_t offset) const {
  switch (type_) {
    case kLoadLiteralWide:
      return LdrdEncoding32(rn_, rt2_, rbase, offset);
    case kLoadFPLiteralSingle:
      return VldrsEncoding32(sd_, rbase, offset);
    case kLoadFPLiteralDouble:
      return VldrdEncoding32(dd_, rbase, offset);
    default:
      LOG(FATAL) << "Unexpected type: " << static_cast<int>(type_);
      UNREACHABLE();
  }
}

inline int32_t Thumb2Assembler::LdrRtRnImm12Encoding(Register rt, Register rn, int32_t offset) {
  DCHECK(IsUint<12>(offset));
  return B31 | B30 | B29 | B28 | B27 | B23 | B22 | B20 | (rn << 16) | (rt << 12) | offset;
}

inline int16_t Thumb2Assembler::AdrEncoding16(Register rd, int32_t offset) {
  DCHECK(IsUint<10>(offset));
  DCHECK(IsAligned<4>(offset));
  DCHECK(!IsHighRegister(rd));
  return B15 | B13 | (rd << 8) | (offset >> 2);
}

inline int32_t Thumb2Assembler::AdrEncoding32(Register rd, int32_t offset) {
  DCHECK(IsUint<12>(offset));
  // Bit     26: offset[11]
  // Bits 14-12: offset[10-8]
  // Bits   7-0: offset[7-0]
  int32_t immediate_mask =
      ((offset & (1 << 11)) << (26 - 11)) |
      ((offset & (7 << 8)) << (12 - 8)) |
      (offset & 0xFF);
  return B31 | B30 | B29 | B28 | B25 | B19 | B18 | B17 | B16 | (rd << 8) | immediate_mask;
}

void Thumb2Assembler::FinalizeCode() {
  ArmAssembler::FinalizeCode();
  uint32_t size_after_literals = BindLiterals();
  BindJumpTables(size_after_literals);
  uint32_t adjusted_code_size = AdjustFixups();
  EmitFixups(adjusted_code_size);
  EmitLiterals();
  FinalizeTrackedLabels();
  EmitJumpTables();
  PatchCFI();
}

bool Thumb2Assembler::ShifterOperandCanAlwaysHold(uint32_t immediate) {
  return ArmAssembler::ModifiedImmediate(immediate) != kInvalidModifiedImmediate;
}

bool Thumb2Assembler::ShifterOperandCanHold(Register rd ATTRIBUTE_UNUSED,
                                            Register rn ATTRIBUTE_UNUSED,
                                            Opcode opcode,
                                            uint32_t immediate,
                                            SetCc set_cc,
                                            ShifterOperand* shifter_op) {
  shifter_op->type_ = ShifterOperand::kImmediate;
  shifter_op->immed_ = immediate;
  shifter_op->is_shift_ = false;
  shifter_op->is_rotate_ = false;
  switch (opcode) {
    case ADD:
    case SUB:
      // Less than (or equal to) 12 bits can be done if we don't need to set condition codes.
      if (immediate < (1 << 12) && set_cc != kCcSet) {
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

void Thumb2Assembler::and_(Register rd, Register rn, const ShifterOperand& so,
                           Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, AND, set_cc, rn, rd, so);
}


void Thumb2Assembler::eor(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, EOR, set_cc, rn, rd, so);
}


void Thumb2Assembler::sub(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, SUB, set_cc, rn, rd, so);
}


void Thumb2Assembler::rsb(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, RSB, set_cc, rn, rd, so);
}


void Thumb2Assembler::add(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, ADD, set_cc, rn, rd, so);
}


void Thumb2Assembler::adc(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, ADC, set_cc, rn, rd, so);
}


void Thumb2Assembler::sbc(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, SBC, set_cc, rn, rd, so);
}


void Thumb2Assembler::rsc(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, RSC, set_cc, rn, rd, so);
}


void Thumb2Assembler::tst(Register rn, const ShifterOperand& so, Condition cond) {
  CHECK_NE(rn, PC);  // Reserve tst pc instruction for exception handler marker.
  EmitDataProcessing(cond, TST, kCcSet, rn, R0, so);
}


void Thumb2Assembler::teq(Register rn, const ShifterOperand& so, Condition cond) {
  CHECK_NE(rn, PC);  // Reserve teq pc instruction for exception handler marker.
  EmitDataProcessing(cond, TEQ, kCcSet, rn, R0, so);
}


void Thumb2Assembler::cmp(Register rn, const ShifterOperand& so, Condition cond) {
  EmitDataProcessing(cond, CMP, kCcSet, rn, R0, so);
}


void Thumb2Assembler::cmn(Register rn, const ShifterOperand& so, Condition cond) {
  EmitDataProcessing(cond, CMN, kCcSet, rn, R0, so);
}


void Thumb2Assembler::orr(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, ORR, set_cc, rn, rd, so);
}


void Thumb2Assembler::orn(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, ORN, set_cc, rn, rd, so);
}


void Thumb2Assembler::mov(Register rd, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, MOV, set_cc, R0, rd, so);
}


void Thumb2Assembler::bic(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, BIC, set_cc, rn, rd, so);
}


void Thumb2Assembler::mvn(Register rd, const ShifterOperand& so,
                          Condition cond, SetCc set_cc) {
  EmitDataProcessing(cond, MVN, set_cc, R0, rd, so);
}


void Thumb2Assembler::mul(Register rd, Register rn, Register rm, Condition cond) {
  CheckCondition(cond);

  if (rd == rm && !IsHighRegister(rd) && !IsHighRegister(rn) && !force_32bit_) {
    // 16 bit.
    int16_t encoding = B14 | B9 | B8 | B6 |
        rn << 3 | rd;
    Emit16(encoding);
  } else {
    // 32 bit.
    uint32_t op1 = 0U /* 0b000 */;
    uint32_t op2 = 0U /* 0b00 */;
    int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 |
        op1 << 20 |
        B15 | B14 | B13 | B12 |
        op2 << 4 |
        static_cast<uint32_t>(rd) << 8 |
        static_cast<uint32_t>(rn) << 16 |
        static_cast<uint32_t>(rm);

    Emit32(encoding);
  }
}


void Thumb2Assembler::mla(Register rd, Register rn, Register rm, Register ra,
                          Condition cond) {
  CheckCondition(cond);

  uint32_t op1 = 0U /* 0b000 */;
  uint32_t op2 = 0U /* 0b00 */;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 |
      op1 << 20 |
      op2 << 4 |
      static_cast<uint32_t>(rd) << 8 |
      static_cast<uint32_t>(ra) << 12 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::mls(Register rd, Register rn, Register rm, Register ra,
                          Condition cond) {
  CheckCondition(cond);

  uint32_t op1 = 0U /* 0b000 */;
  uint32_t op2 = 01 /* 0b01 */;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 |
      op1 << 20 |
      op2 << 4 |
      static_cast<uint32_t>(rd) << 8 |
      static_cast<uint32_t>(ra) << 12 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::smull(Register rd_lo, Register rd_hi, Register rn,
                            Register rm, Condition cond) {
  CheckCondition(cond);

  uint32_t op1 = 0U /* 0b000; */;
  uint32_t op2 = 0U /* 0b0000 */;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 | B23 |
      op1 << 20 |
      op2 << 4 |
      static_cast<uint32_t>(rd_lo) << 12 |
      static_cast<uint32_t>(rd_hi) << 8 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::umull(Register rd_lo, Register rd_hi, Register rn,
                            Register rm, Condition cond) {
  CheckCondition(cond);

  uint32_t op1 = 2U /* 0b010; */;
  uint32_t op2 = 0U /* 0b0000 */;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 | B23 |
      op1 << 20 |
      op2 << 4 |
      static_cast<uint32_t>(rd_lo) << 12 |
      static_cast<uint32_t>(rd_hi) << 8 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::sdiv(Register rd, Register rn, Register rm, Condition cond) {
  CheckCondition(cond);

  uint32_t op1 = 1U  /* 0b001 */;
  uint32_t op2 = 15U /* 0b1111 */;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 | B23 | B20 |
      op1 << 20 |
      op2 << 4 |
      0xf << 12 |
      static_cast<uint32_t>(rd) << 8 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::udiv(Register rd, Register rn, Register rm, Condition cond) {
  CheckCondition(cond);

  uint32_t op1 = 1U  /* 0b001 */;
  uint32_t op2 = 15U /* 0b1111 */;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 | B23 | B21 | B20 |
      op1 << 20 |
      op2 << 4 |
      0xf << 12 |
      static_cast<uint32_t>(rd) << 8 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::sbfx(Register rd, Register rn, uint32_t lsb, uint32_t width, Condition cond) {
  CheckCondition(cond);
  CHECK_LE(lsb, 31U);
  CHECK(1U <= width && width <= 32U) << width;
  uint32_t widthminus1 = width - 1;
  uint32_t imm2 = lsb & (B1 | B0);  // Bits 0-1 of `lsb`.
  uint32_t imm3 = (lsb & (B4 | B3 | B2)) >> 2;  // Bits 2-4 of `lsb`.

  uint32_t op = 20U /* 0b10100 */;
  int32_t encoding = B31 | B30 | B29 | B28 | B25 |
      op << 20 |
      static_cast<uint32_t>(rn) << 16 |
      imm3 << 12 |
      static_cast<uint32_t>(rd) << 8 |
      imm2 << 6 |
      widthminus1;

  Emit32(encoding);
}


void Thumb2Assembler::ubfx(Register rd, Register rn, uint32_t lsb, uint32_t width, Condition cond) {
  CheckCondition(cond);
  CHECK_LE(lsb, 31U);
  CHECK(1U <= width && width <= 32U) << width;
  uint32_t widthminus1 = width - 1;
  uint32_t imm2 = lsb & (B1 | B0);  // Bits 0-1 of `lsb`.
  uint32_t imm3 = (lsb & (B4 | B3 | B2)) >> 2;  // Bits 2-4 of `lsb`.

  uint32_t op = 28U /* 0b11100 */;
  int32_t encoding = B31 | B30 | B29 | B28 | B25 |
      op << 20 |
      static_cast<uint32_t>(rn) << 16 |
      imm3 << 12 |
      static_cast<uint32_t>(rd) << 8 |
      imm2 << 6 |
      widthminus1;

  Emit32(encoding);
}


void Thumb2Assembler::ldr(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, true, false, false, false, rd, ad);
}


void Thumb2Assembler::str(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, false, false, false, false, rd, ad);
}


void Thumb2Assembler::ldrb(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, true, true, false, false, rd, ad);
}


void Thumb2Assembler::strb(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, false, true, false, false, rd, ad);
}


void Thumb2Assembler::ldrh(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, true, false, true, false, rd, ad);
}


void Thumb2Assembler::strh(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, false, false, true, false, rd, ad);
}


void Thumb2Assembler::ldrsb(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, true, true, false, true, rd, ad);
}


void Thumb2Assembler::ldrsh(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, true, false, true, true, rd, ad);
}


void Thumb2Assembler::ldrd(Register rd, const Address& ad, Condition cond) {
  ldrd(rd, Register(rd + 1), ad, cond);
}


void Thumb2Assembler::ldrd(Register rd, Register rd2, const Address& ad, Condition cond) {
  CheckCondition(cond);
  // Encoding T1.
  // This is different from other loads.  The encoding is like ARM.
  int32_t encoding = B31 | B30 | B29 | B27 | B22 | B20 |
      static_cast<int32_t>(rd) << 12 |
      static_cast<int32_t>(rd2) << 8 |
      ad.encodingThumbLdrdStrd();
  Emit32(encoding);
}


void Thumb2Assembler::strd(Register rd, const Address& ad, Condition cond) {
  strd(rd, Register(rd + 1), ad, cond);
}


void Thumb2Assembler::strd(Register rd, Register rd2, const Address& ad, Condition cond) {
  CheckCondition(cond);
  // Encoding T1.
  // This is different from other loads.  The encoding is like ARM.
  int32_t encoding = B31 | B30 | B29 | B27 | B22 |
      static_cast<int32_t>(rd) << 12 |
      static_cast<int32_t>(rd2) << 8 |
      ad.encodingThumbLdrdStrd();
  Emit32(encoding);
}


void Thumb2Assembler::ldm(BlockAddressMode am,
                          Register base,
                          RegList regs,
                          Condition cond) {
  CHECK_NE(regs, 0u);  // Do not use ldm if there's nothing to load.
  if (IsPowerOfTwo(regs)) {
    // Thumb doesn't support one reg in the list.
    // Find the register number.
    int reg = CTZ(static_cast<uint32_t>(regs));
    CHECK_LT(reg, 16);
    CHECK(am == DB_W);      // Only writeback is supported.
    ldr(static_cast<Register>(reg), Address(base, kRegisterSize, Address::PostIndex), cond);
  } else {
    EmitMultiMemOp(cond, am, true, base, regs);
  }
}


void Thumb2Assembler::stm(BlockAddressMode am,
                          Register base,
                          RegList regs,
                          Condition cond) {
  CHECK_NE(regs, 0u);  // Do not use stm if there's nothing to store.
  if (IsPowerOfTwo(regs)) {
    // Thumb doesn't support one reg in the list.
    // Find the register number.
    int reg = CTZ(static_cast<uint32_t>(regs));
    CHECK_LT(reg, 16);
    CHECK(am == IA || am == IA_W);
    Address::Mode strmode = am == IA ? Address::PreIndex : Address::Offset;
    str(static_cast<Register>(reg), Address(base, -kRegisterSize, strmode), cond);
  } else {
    EmitMultiMemOp(cond, am, false, base, regs);
  }
}


bool Thumb2Assembler::vmovs(SRegister sd, float s_imm, Condition cond) {
  uint32_t imm32 = bit_cast<uint32_t, float>(s_imm);
  if (((imm32 & ((1 << 19) - 1)) == 0) &&
      ((((imm32 >> 25) & ((1 << 6) - 1)) == (1 << 5)) ||
       (((imm32 >> 25) & ((1 << 6) - 1)) == ((1 << 5) -1)))) {
    uint8_t imm8 = ((imm32 >> 31) << 7) | (((imm32 >> 29) & 1) << 6) |
        ((imm32 >> 19) & ((1 << 6) -1));
    EmitVFPsss(cond, B23 | B21 | B20 | ((imm8 >> 4)*B16) | (imm8 & 0xf),
               sd, S0, S0);
    return true;
  }
  return false;
}


bool Thumb2Assembler::vmovd(DRegister dd, double d_imm, Condition cond) {
  uint64_t imm64 = bit_cast<uint64_t, double>(d_imm);
  if (((imm64 & ((1LL << 48) - 1)) == 0) &&
      ((((imm64 >> 54) & ((1 << 9) - 1)) == (1 << 8)) ||
       (((imm64 >> 54) & ((1 << 9) - 1)) == ((1 << 8) -1)))) {
    uint8_t imm8 = ((imm64 >> 63) << 7) | (((imm64 >> 61) & 1) << 6) |
        ((imm64 >> 48) & ((1 << 6) -1));
    EmitVFPddd(cond, B23 | B21 | B20 | ((imm8 >> 4)*B16) | B8 | (imm8 & 0xf),
               dd, D0, D0);
    return true;
  }
  return false;
}


void Thumb2Assembler::vmovs(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B6, sd, S0, sm);
}


void Thumb2Assembler::vmovd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B6, dd, D0, dm);
}


void Thumb2Assembler::vadds(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, B21 | B20, sd, sn, sm);
}


void Thumb2Assembler::vaddd(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, B21 | B20, dd, dn, dm);
}


void Thumb2Assembler::vsubs(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, B21 | B20 | B6, sd, sn, sm);
}


void Thumb2Assembler::vsubd(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, B21 | B20 | B6, dd, dn, dm);
}


void Thumb2Assembler::vmuls(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, B21, sd, sn, sm);
}


void Thumb2Assembler::vmuld(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, B21, dd, dn, dm);
}


void Thumb2Assembler::vmlas(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, 0, sd, sn, sm);
}


void Thumb2Assembler::vmlad(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, 0, dd, dn, dm);
}


void Thumb2Assembler::vmlss(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, B6, sd, sn, sm);
}


void Thumb2Assembler::vmlsd(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, B6, dd, dn, dm);
}


void Thumb2Assembler::vdivs(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, B23, sd, sn, sm);
}


void Thumb2Assembler::vdivd(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, B23, dd, dn, dm);
}


void Thumb2Assembler::vabss(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B7 | B6, sd, S0, sm);
}


void Thumb2Assembler::vabsd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B7 | B6, dd, D0, dm);
}


void Thumb2Assembler::vnegs(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B16 | B6, sd, S0, sm);
}


void Thumb2Assembler::vnegd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B16 | B6, dd, D0, dm);
}


void Thumb2Assembler::vsqrts(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B16 | B7 | B6, sd, S0, sm);
}

void Thumb2Assembler::vsqrtd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B16 | B7 | B6, dd, D0, dm);
}


void Thumb2Assembler::vcvtsd(SRegister sd, DRegister dm, Condition cond) {
  EmitVFPsd(cond, B23 | B21 | B20 | B18 | B17 | B16 | B8 | B7 | B6, sd, dm);
}


void Thumb2Assembler::vcvtds(DRegister dd, SRegister sm, Condition cond) {
  EmitVFPds(cond, B23 | B21 | B20 | B18 | B17 | B16 | B7 | B6, dd, sm);
}


void Thumb2Assembler::vcvtis(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B18 | B16 | B7 | B6, sd, S0, sm);
}


void Thumb2Assembler::vcvtid(SRegister sd, DRegister dm, Condition cond) {
  EmitVFPsd(cond, B23 | B21 | B20 | B19 | B18 | B16 | B8 | B7 | B6, sd, dm);
}


void Thumb2Assembler::vcvtsi(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B7 | B6, sd, S0, sm);
}


void Thumb2Assembler::vcvtdi(DRegister dd, SRegister sm, Condition cond) {
  EmitVFPds(cond, B23 | B21 | B20 | B19 | B8 | B7 | B6, dd, sm);
}


void Thumb2Assembler::vcvtus(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B18 | B7 | B6, sd, S0, sm);
}


void Thumb2Assembler::vcvtud(SRegister sd, DRegister dm, Condition cond) {
  EmitVFPsd(cond, B23 | B21 | B20 | B19 | B18 | B8 | B7 | B6, sd, dm);
}


void Thumb2Assembler::vcvtsu(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B6, sd, S0, sm);
}


void Thumb2Assembler::vcvtdu(DRegister dd, SRegister sm, Condition cond) {
  EmitVFPds(cond, B23 | B21 | B20 | B19 | B8 | B6, dd, sm);
}


void Thumb2Assembler::vcmps(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B18 | B6, sd, S0, sm);
}


void Thumb2Assembler::vcmpd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B18 | B6, dd, D0, dm);
}


void Thumb2Assembler::vcmpsz(SRegister sd, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B18 | B16 | B6, sd, S0, S0);
}


void Thumb2Assembler::vcmpdz(DRegister dd, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B18 | B16 | B6, dd, D0, D0);
}

void Thumb2Assembler::b(Label* label, Condition cond) {
  DCHECK_EQ(next_condition_, AL);
  EmitBranch(cond, label, false, false);
}


void Thumb2Assembler::bl(Label* label, Condition cond) {
  CheckCondition(cond);
  EmitBranch(cond, label, true, false);
}


void Thumb2Assembler::blx(Label* label) {
  EmitBranch(AL, label, true, true);
}


void Thumb2Assembler::MarkExceptionHandler(Label* label) {
  EmitDataProcessing(AL, TST, kCcSet, PC, R0, ShifterOperand(0));
  Label l;
  b(&l);
  EmitBranch(AL, label, false, false);
  Bind(&l);
}


void Thumb2Assembler::Emit32(int32_t value) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  buffer_.Emit<int16_t>(value >> 16);
  buffer_.Emit<int16_t>(value & 0xffff);
}


void Thumb2Assembler::Emit16(int16_t value) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  buffer_.Emit<int16_t>(value);
}


bool Thumb2Assembler::Is32BitDataProcessing(Condition cond,
                                            Opcode opcode,
                                            SetCc set_cc,
                                            Register rn,
                                            Register rd,
                                            const ShifterOperand& so) {
  if (force_32bit_) {
    return true;
  }

  // Check special case for SP relative ADD and SUB immediate.
  if ((opcode == ADD || opcode == SUB) && rn == SP && so.IsImmediate() && set_cc != kCcSet) {
    // If the immediate is in range, use 16 bit.
    if (rd == SP) {
      if (so.GetImmediate() < (1 << 9)) {    // 9 bit immediate.
        return false;
      }
    } else if (!IsHighRegister(rd) && opcode == ADD) {
      if (so.GetImmediate() < (1 << 10)) {    // 10 bit immediate.
        return false;
      }
    }
  }

  bool can_contain_high_register =
      (opcode == CMP) ||
      (opcode == MOV && set_cc != kCcSet) ||
      ((opcode == ADD) && (rn == rd) && set_cc != kCcSet);

  if (IsHighRegister(rd) || IsHighRegister(rn)) {
    if (!can_contain_high_register) {
      return true;
    }

    // There are high register instructions available for this opcode.
    // However, there is no actual shift available, neither for ADD nor for MOV (ASR/LSR/LSL/ROR).
    if (so.IsShift() && (so.GetShift() == RRX || so.GetImmediate() != 0u)) {
      return true;
    }

    // The ADD and MOV instructions that work with high registers don't have 16-bit
    // immediate variants.
    if (so.IsImmediate()) {
      return true;
    }
  }

  if (so.IsRegister() && IsHighRegister(so.GetRegister()) && !can_contain_high_register) {
    return true;
  }

  bool rn_is_valid = true;

  // Check for single operand instructions and ADD/SUB.
  switch (opcode) {
    case CMP:
    case MOV:
    case TST:
    case MVN:
      rn_is_valid = false;      // There is no Rn for these instructions.
      break;
    case TEQ:
    case ORN:
      return true;
    case ADD:
    case SUB:
      break;
    default:
      if (so.IsRegister() && rd != rn) {
        return true;
      }
  }

  if (so.IsImmediate()) {
    if (opcode == RSB) {
      DCHECK(rn_is_valid);
      if (so.GetImmediate() != 0u) {
        return true;
      }
    } else if (rn_is_valid && rn != rd) {
      // The only thumb1 instructions with a register and an immediate are ADD and SUB
      // with a 3-bit immediate, and RSB with zero immediate.
      if (opcode == ADD || opcode == SUB) {
        if ((cond == AL) ? set_cc == kCcKeep : set_cc == kCcSet) {
          return true;  // Cannot match "setflags".
        }
        if (!IsUint<3>(so.GetImmediate()) && !IsUint<3>(-so.GetImmediate())) {
          return true;
        }
      } else {
        return true;
      }
    } else {
      // ADD, SUB, CMP and MOV may be thumb1 only if the immediate is 8 bits.
      if (!(opcode == ADD || opcode == SUB || opcode == MOV || opcode == CMP)) {
        return true;
      } else if (opcode != CMP && ((cond == AL) ? set_cc == kCcKeep : set_cc == kCcSet)) {
        return true;  // Cannot match "setflags" for ADD, SUB or MOV.
      } else {
        // For ADD and SUB allow also negative 8-bit immediate as we will emit the oposite opcode.
        if (!IsUint<8>(so.GetImmediate()) &&
            (opcode == MOV || opcode == CMP || !IsUint<8>(-so.GetImmediate()))) {
          return true;
        }
      }
    }
  } else {
    DCHECK(so.IsRegister());
    if (so.IsShift()) {
      // Shift operand - check if it is a MOV convertible to a 16-bit shift instruction.
      if (opcode != MOV) {
        return true;
      }
      // Check for MOV with an ROR/RRX. There is no 16-bit ROR immediate and no 16-bit RRX.
      if (so.GetShift() == ROR || so.GetShift() == RRX) {
        return true;
      }
      // 16-bit shifts set condition codes if and only if outside IT block,
      // i.e. if and only if cond == AL.
      if ((cond == AL) ? set_cc == kCcKeep : set_cc == kCcSet) {
        return true;
      }
    } else {
      // Register operand without shift.
      switch (opcode) {
        case ADD:
          // The 16-bit ADD that cannot contain high registers can set condition codes
          // if and only if outside IT block, i.e. if and only if cond == AL.
          if (!can_contain_high_register &&
              ((cond == AL) ? set_cc == kCcKeep : set_cc == kCcSet)) {
            return true;
          }
          break;
        case AND:
        case BIC:
        case EOR:
        case ORR:
        case MVN:
        case ADC:
        case SUB:
        case SBC:
          // These 16-bit opcodes set condition codes if and only if outside IT block,
          // i.e. if and only if cond == AL.
          if ((cond == AL) ? set_cc == kCcKeep : set_cc == kCcSet) {
            return true;
          }
          break;
        case RSB:
        case RSC:
          // No 16-bit RSB/RSC Rd, Rm, Rn. It would be equivalent to SUB/SBC Rd, Rn, Rm.
          return true;
        case CMP:
        default:
          break;
      }
    }
  }

  // The instruction can be encoded in 16 bits.
  return false;
}


void Thumb2Assembler::Emit32BitDataProcessing(Condition cond ATTRIBUTE_UNUSED,
                                              Opcode opcode,
                                              SetCc set_cc,
                                              Register rn,
                                              Register rd,
                                              const ShifterOperand& so) {
  uint8_t thumb_opcode = 255U /* 0b11111111 */;
  switch (opcode) {
    case AND: thumb_opcode =  0U /* 0b0000 */; break;
    case EOR: thumb_opcode =  4U /* 0b0100 */; break;
    case SUB: thumb_opcode = 13U /* 0b1101 */; break;
    case RSB: thumb_opcode = 14U /* 0b1110 */; break;
    case ADD: thumb_opcode =  8U /* 0b1000 */; break;
    case ADC: thumb_opcode = 10U /* 0b1010 */; break;
    case SBC: thumb_opcode = 11U /* 0b1011 */; break;
    case RSC: break;
    case TST: thumb_opcode =  0U /* 0b0000 */; DCHECK(set_cc == kCcSet); rd = PC; break;
    case TEQ: thumb_opcode =  4U /* 0b0100 */; DCHECK(set_cc == kCcSet); rd = PC; break;
    case CMP: thumb_opcode = 13U /* 0b1101 */; DCHECK(set_cc == kCcSet); rd = PC; break;
    case CMN: thumb_opcode =  8U /* 0b1000 */; DCHECK(set_cc == kCcSet); rd = PC; break;
    case ORR: thumb_opcode =  2U /* 0b0010 */; break;
    case MOV: thumb_opcode =  2U /* 0b0010 */; rn = PC; break;
    case BIC: thumb_opcode =  1U /* 0b0001 */; break;
    case MVN: thumb_opcode =  3U /* 0b0011 */; rn = PC; break;
    case ORN: thumb_opcode =  3U /* 0b0011 */; break;
    default:
      break;
  }

  if (thumb_opcode == 255U /* 0b11111111 */) {
    LOG(FATAL) << "Invalid thumb2 opcode " << opcode;
    UNREACHABLE();
  }

  int32_t encoding = 0;
  if (so.IsImmediate()) {
    // Check special cases.
    if ((opcode == SUB || opcode == ADD) && (so.GetImmediate() < (1u << 12)) &&
        /* Prefer T3 encoding to T4. */ !ShifterOperandCanAlwaysHold(so.GetImmediate())) {
      if (set_cc != kCcSet) {
        if (opcode == SUB) {
          thumb_opcode = 5U;
        } else if (opcode == ADD) {
          thumb_opcode = 0U;
        }
      }
      uint32_t imm = so.GetImmediate();

      uint32_t i = (imm >> 11) & 1;
      uint32_t imm3 = (imm >> 8) & 7U /* 0b111 */;
      uint32_t imm8 = imm & 0xff;

      encoding = B31 | B30 | B29 | B28 |
          (set_cc == kCcSet ? B20 : B25) |
          thumb_opcode << 21 |
          rn << 16 |
          rd << 8 |
          i << 26 |
          imm3 << 12 |
          imm8;
    } else {
      // Modified immediate.
      uint32_t imm = ModifiedImmediate(so.encodingThumb());
      if (imm == kInvalidModifiedImmediate) {
        LOG(FATAL) << "Immediate value cannot fit in thumb2 modified immediate";
        UNREACHABLE();
      }
      encoding = B31 | B30 | B29 | B28 |
          thumb_opcode << 21 |
          (set_cc == kCcSet ? B20 : 0) |
          rn << 16 |
          rd << 8 |
          imm;
    }
  } else if (so.IsRegister()) {
    // Register (possibly shifted)
    encoding = B31 | B30 | B29 | B27 | B25 |
        thumb_opcode << 21 |
        (set_cc == kCcSet ? B20 : 0) |
        rn << 16 |
        rd << 8 |
        so.encodingThumb();
  }
  Emit32(encoding);
}


void Thumb2Assembler::Emit16BitDataProcessing(Condition cond,
                                              Opcode opcode,
                                              SetCc set_cc,
                                              Register rn,
                                              Register rd,
                                              const ShifterOperand& so) {
  if (opcode == ADD || opcode == SUB) {
    Emit16BitAddSub(cond, opcode, set_cc, rn, rd, so);
    return;
  }
  uint8_t thumb_opcode = 255U /* 0b11111111 */;
  // Thumb1.
  uint8_t dp_opcode = 1U /* 0b01 */;
  uint8_t opcode_shift = 6;
  uint8_t rd_shift = 0;
  uint8_t rn_shift = 3;
  uint8_t immediate_shift = 0;
  bool use_immediate = false;
  uint8_t immediate = 0;

  if (opcode == MOV && so.IsRegister() && so.IsShift()) {
    // Convert shifted mov operand2 into 16 bit opcodes.
    dp_opcode = 0;
    opcode_shift = 11;

    use_immediate = true;
    immediate = so.GetImmediate();
    immediate_shift = 6;

    rn = so.GetRegister();

    switch (so.GetShift()) {
    case LSL:
      DCHECK_LE(immediate, 31u);
      thumb_opcode = 0U /* 0b00 */;
      break;
    case LSR:
      DCHECK(1 <= immediate && immediate <= 32);
      immediate &= 31;  // 32 is encoded as 0.
      thumb_opcode = 1U /* 0b01 */;
      break;
    case ASR:
      DCHECK(1 <= immediate && immediate <= 32);
      immediate &= 31;  // 32 is encoded as 0.
      thumb_opcode = 2U /* 0b10 */;
      break;
    case ROR:  // No 16-bit ROR immediate.
    case RRX:  // No 16-bit RRX.
    default:
      LOG(FATAL) << "Unexpected shift: " << so.GetShift();
      UNREACHABLE();
    }
  } else {
    if (so.IsImmediate()) {
      use_immediate = true;
      immediate = so.GetImmediate();
    } else {
      CHECK(!(so.IsRegister() && so.IsShift() && so.GetSecondRegister() != kNoRegister))
          << "No register-shifted register instruction available in thumb";
      // Adjust rn and rd: only two registers will be emitted.
      switch (opcode) {
        case AND:
        case ORR:
        case EOR:
        case RSB:
        case ADC:
        case SBC:
        case BIC: {
          // Sets condition codes if and only if outside IT block,
          // check that it complies with set_cc.
          DCHECK((cond == AL) ? set_cc != kCcKeep : set_cc != kCcSet);
          if (rn == rd) {
            rn = so.GetRegister();
          } else {
            CHECK_EQ(rd, so.GetRegister());
          }
          break;
        }
        case CMP:
        case CMN: {
          CHECK_EQ(rd, 0);
          rd = rn;
          rn = so.GetRegister();
          break;
        }
        case MVN: {
          // Sets condition codes if and only if outside IT block,
          // check that it complies with set_cc.
          DCHECK((cond == AL) ? set_cc != kCcKeep : set_cc != kCcSet);
          CHECK_EQ(rn, 0);
          rn = so.GetRegister();
          break;
        }
        case TST:
        case TEQ: {
          DCHECK(set_cc == kCcSet);
          CHECK_EQ(rn, 0);
          rn = so.GetRegister();
          break;
        }
        default:
          break;
      }
    }

    switch (opcode) {
      case AND: thumb_opcode = 0U /* 0b0000 */; break;
      case ORR: thumb_opcode = 12U /* 0b1100 */; break;
      case EOR: thumb_opcode = 1U /* 0b0001 */; break;
      case RSB: thumb_opcode = 9U /* 0b1001 */; break;
      case ADC: thumb_opcode = 5U /* 0b0101 */; break;
      case SBC: thumb_opcode = 6U /* 0b0110 */; break;
      case BIC: thumb_opcode = 14U /* 0b1110 */; break;
      case TST: thumb_opcode = 8U /* 0b1000 */; CHECK(!use_immediate); break;
      case MVN: thumb_opcode = 15U /* 0b1111 */; CHECK(!use_immediate); break;
      case CMP: {
        DCHECK(set_cc == kCcSet);
        if (use_immediate) {
          // T2 encoding.
          dp_opcode = 0;
          opcode_shift = 11;
          thumb_opcode = 5U /* 0b101 */;
          rd_shift = 8;
          rn_shift = 8;
        } else if (IsHighRegister(rd) || IsHighRegister(rn)) {
          // Special cmp for high registers.
          dp_opcode = 1U /* 0b01 */;
          opcode_shift = 7;
          // Put the top bit of rd into the bottom bit of the opcode.
          thumb_opcode = 10U /* 0b0001010 */ | static_cast<uint32_t>(rd) >> 3;
          rd = static_cast<Register>(static_cast<uint32_t>(rd) & 7U /* 0b111 */);
        } else {
          thumb_opcode = 10U /* 0b1010 */;
        }

        break;
      }
      case CMN: {
        CHECK(!use_immediate);
        thumb_opcode = 11U /* 0b1011 */;
        break;
      }
      case MOV:
        dp_opcode = 0;
        if (use_immediate) {
          // T2 encoding.
          opcode_shift = 11;
          thumb_opcode = 4U /* 0b100 */;
          rd_shift = 8;
          rn_shift = 8;
        } else {
          rn = so.GetRegister();
          if (set_cc != kCcSet) {
            // Special mov for high registers.
            dp_opcode = 1U /* 0b01 */;
            opcode_shift = 7;
            // Put the top bit of rd into the bottom bit of the opcode.
            thumb_opcode = 12U /* 0b0001100 */ | static_cast<uint32_t>(rd) >> 3;
            rd = static_cast<Register>(static_cast<uint32_t>(rd) & 7U /* 0b111 */);
          } else {
            DCHECK(!IsHighRegister(rn));
            DCHECK(!IsHighRegister(rd));
            thumb_opcode = 0;
          }
        }
        break;

      case TEQ:
      case RSC:
      default:
        LOG(FATAL) << "Invalid thumb1 opcode " << opcode;
        break;
    }
  }

  if (thumb_opcode == 255U /* 0b11111111 */) {
    LOG(FATAL) << "Invalid thumb1 opcode " << opcode;
    UNREACHABLE();
  }

  int16_t encoding = dp_opcode << 14 |
      (thumb_opcode << opcode_shift) |
      rd << rd_shift |
      rn << rn_shift |
      (use_immediate ? (immediate << immediate_shift) : 0);

  Emit16(encoding);
}


// ADD and SUB are complex enough to warrant their own emitter.
void Thumb2Assembler::Emit16BitAddSub(Condition cond,
                                      Opcode opcode,
                                      SetCc set_cc,
                                      Register rn,
                                      Register rd,
                                      const ShifterOperand& so) {
  uint8_t dp_opcode = 0;
  uint8_t opcode_shift = 6;
  uint8_t rd_shift = 0;
  uint8_t rn_shift = 3;
  uint8_t immediate_shift = 0;
  bool use_immediate = false;
  uint32_t immediate = 0;  // Should be at most 10 bits but keep the full immediate for CHECKs.
  uint8_t thumb_opcode;

  if (so.IsImmediate()) {
    use_immediate = true;
    immediate = so.GetImmediate();
    if (!IsUint<10>(immediate)) {
      // Flip ADD/SUB.
      opcode = (opcode == ADD) ? SUB : ADD;
      immediate = -immediate;
      DCHECK(IsUint<10>(immediate));  // More stringent checks below.
    }
  }

  switch (opcode) {
    case ADD:
      if (so.IsRegister()) {
        Register rm = so.GetRegister();
        if (rn == rd && set_cc != kCcSet) {
          // Can use T2 encoding (allows 4 bit registers)
          dp_opcode = 1U /* 0b01 */;
          opcode_shift = 10;
          thumb_opcode = 1U /* 0b0001 */;
          // Make Rn also contain the top bit of rd.
          rn = static_cast<Register>(static_cast<uint32_t>(rm) |
                                     (static_cast<uint32_t>(rd) & 8U /* 0b1000 */) << 1);
          rd = static_cast<Register>(static_cast<uint32_t>(rd) & 7U /* 0b111 */);
        } else {
          // T1.
          DCHECK(!IsHighRegister(rd));
          DCHECK(!IsHighRegister(rn));
          DCHECK(!IsHighRegister(rm));
          // Sets condition codes if and only if outside IT block,
          // check that it complies with set_cc.
          DCHECK((cond == AL) ? set_cc != kCcKeep : set_cc != kCcSet);
          opcode_shift = 9;
          thumb_opcode = 12U /* 0b01100 */;
          immediate = static_cast<uint32_t>(so.GetRegister());
          use_immediate = true;
          immediate_shift = 6;
        }
      } else {
        // Immediate.
        if (rd == SP && rn == SP) {
          // ADD sp, sp, #imm
          dp_opcode = 2U /* 0b10 */;
          thumb_opcode = 3U /* 0b11 */;
          opcode_shift = 12;
          CHECK(IsUint<9>(immediate));
          CHECK_ALIGNED(immediate, 4);

          // Remove rd and rn from instruction by orring it with immed and clearing bits.
          rn = R0;
          rd = R0;
          rd_shift = 0;
          rn_shift = 0;
          immediate >>= 2;
        } else if (rd != SP && rn == SP) {
          // ADD rd, SP, #imm
          dp_opcode = 2U /* 0b10 */;
          thumb_opcode = 5U /* 0b101 */;
          opcode_shift = 11;
          CHECK(IsUint<10>(immediate));
          CHECK_ALIGNED(immediate, 4);

          // Remove rn from instruction.
          rn = R0;
          rn_shift = 0;
          rd_shift = 8;
          immediate >>= 2;
        } else if (rn != rd) {
          // Must use T1.
          CHECK(IsUint<3>(immediate));
          opcode_shift = 9;
          thumb_opcode = 14U /* 0b01110 */;
          immediate_shift = 6;
        } else {
          // T2 encoding.
          CHECK(IsUint<8>(immediate));
          opcode_shift = 11;
          thumb_opcode = 6U /* 0b110 */;
          rd_shift = 8;
          rn_shift = 8;
        }
      }
      break;

    case SUB:
      if (so.IsRegister()) {
        // T1.
        Register rm = so.GetRegister();
        DCHECK(!IsHighRegister(rd));
        DCHECK(!IsHighRegister(rn));
        DCHECK(!IsHighRegister(rm));
        // Sets condition codes if and only if outside IT block,
        // check that it complies with set_cc.
        DCHECK((cond == AL) ? set_cc != kCcKeep : set_cc != kCcSet);
        opcode_shift = 9;
        thumb_opcode = 13U /* 0b01101 */;
        immediate = static_cast<uint32_t>(rm);
        use_immediate = true;
        immediate_shift = 6;
      } else {
        if (rd == SP && rn == SP) {
          // SUB sp, sp, #imm
          dp_opcode = 2U /* 0b10 */;
          thumb_opcode = 0x61 /* 0b1100001 */;
          opcode_shift = 7;
          CHECK(IsUint<9>(immediate));
          CHECK_ALIGNED(immediate, 4);

          // Remove rd and rn from instruction by orring it with immed and clearing bits.
          rn = R0;
          rd = R0;
          rd_shift = 0;
          rn_shift = 0;
          immediate >>= 2;
        } else if (rn != rd) {
          // Must use T1.
          CHECK(IsUint<3>(immediate));
          opcode_shift = 9;
          thumb_opcode = 15U /* 0b01111 */;
          immediate_shift = 6;
        } else {
          // T2 encoding.
          CHECK(IsUint<8>(immediate));
          opcode_shift = 11;
          thumb_opcode = 7U /* 0b111 */;
          rd_shift = 8;
          rn_shift = 8;
        }
      }
      break;
    default:
      LOG(FATAL) << "This opcode is not an ADD or SUB: " << opcode;
      UNREACHABLE();
  }

  int16_t encoding = dp_opcode << 14 |
      (thumb_opcode << opcode_shift) |
      rd << rd_shift |
      rn << rn_shift |
      (use_immediate ? (immediate << immediate_shift) : 0);

  Emit16(encoding);
}


void Thumb2Assembler::EmitDataProcessing(Condition cond,
                                         Opcode opcode,
                                         SetCc set_cc,
                                         Register rn,
                                         Register rd,
                                         const ShifterOperand& so) {
  CHECK_NE(rd, kNoRegister);
  CheckCondition(cond);

  if (Is32BitDataProcessing(cond, opcode, set_cc, rn, rd, so)) {
    Emit32BitDataProcessing(cond, opcode, set_cc, rn, rd, so);
  } else {
    Emit16BitDataProcessing(cond, opcode, set_cc, rn, rd, so);
  }
}

void Thumb2Assembler::EmitShift(Register rd,
                                Register rm,
                                Shift shift,
                                uint8_t amount,
                                Condition cond,
                                SetCc set_cc) {
  CHECK_LT(amount, (1 << 5));
  if ((IsHighRegister(rd) || IsHighRegister(rm) || shift == ROR || shift == RRX) ||
      ((cond == AL) ? set_cc == kCcKeep : set_cc == kCcSet)) {
    uint16_t opcode = 0;
    switch (shift) {
      case LSL: opcode = 0U /* 0b00 */; break;
      case LSR: opcode = 1U /* 0b01 */; break;
      case ASR: opcode = 2U /* 0b10 */; break;
      case ROR: opcode = 3U /* 0b11 */; break;
      case RRX: opcode = 3U /* 0b11 */; amount = 0; break;
      default:
        LOG(FATAL) << "Unsupported thumb2 shift opcode";
        UNREACHABLE();
    }
    // 32 bit.
    int32_t encoding = B31 | B30 | B29 | B27 | B25 | B22 |
        0xf << 16 | (set_cc == kCcSet ? B20 : 0);
    uint32_t imm3 = amount >> 2;
    uint32_t imm2 = amount & 3U /* 0b11 */;
    encoding |= imm3 << 12 | imm2 << 6 | static_cast<int16_t>(rm) |
        static_cast<int16_t>(rd) << 8 | opcode << 4;
    Emit32(encoding);
  } else {
    // 16 bit shift
    uint16_t opcode = 0;
    switch (shift) {
      case LSL: opcode = 0U /* 0b00 */; break;
      case LSR: opcode = 1U /* 0b01 */; break;
      case ASR: opcode = 2U /* 0b10 */; break;
      default:
        LOG(FATAL) << "Unsupported thumb2 shift opcode";
        UNREACHABLE();
    }
    int16_t encoding = opcode << 11 | amount << 6 | static_cast<int16_t>(rm) << 3 |
        static_cast<int16_t>(rd);
    Emit16(encoding);
  }
}

void Thumb2Assembler::EmitShift(Register rd,
                                Register rn,
                                Shift shift,
                                Register rm,
                                Condition cond,
                                SetCc set_cc) {
  CHECK_NE(shift, RRX);
  bool must_be_32bit = false;
  if (IsHighRegister(rd) || IsHighRegister(rm) || IsHighRegister(rn) || rd != rn ||
      ((cond == AL) ? set_cc == kCcKeep : set_cc == kCcSet)) {
    must_be_32bit = true;
  }

  if (must_be_32bit) {
    uint16_t opcode = 0;
     switch (shift) {
       case LSL: opcode = 0U /* 0b00 */; break;
       case LSR: opcode = 1U /* 0b01 */; break;
       case ASR: opcode = 2U /* 0b10 */; break;
       case ROR: opcode = 3U /* 0b11 */; break;
       default:
         LOG(FATAL) << "Unsupported thumb2 shift opcode";
         UNREACHABLE();
     }
     // 32 bit.
     int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 |
         0xf << 12 | (set_cc == kCcSet ? B20 : 0);
     encoding |= static_cast<int16_t>(rn) << 16 | static_cast<int16_t>(rm) |
         static_cast<int16_t>(rd) << 8 | opcode << 21;
     Emit32(encoding);
  } else {
    uint16_t opcode = 0;
    switch (shift) {
      case LSL: opcode = 2U /* 0b0010 */; break;
      case LSR: opcode = 3U /* 0b0011 */; break;
      case ASR: opcode = 4U /* 0b0100 */; break;
      case ROR: opcode = 7U /* 0b0111 */; break;
      default:
        LOG(FATAL) << "Unsupported thumb2 shift opcode";
        UNREACHABLE();
    }
    int16_t encoding = B14 | opcode << 6 | static_cast<int16_t>(rm) << 3 |
        static_cast<int16_t>(rd);
    Emit16(encoding);
  }
}

inline size_t Thumb2Assembler::Fixup::SizeInBytes(Size size) {
  switch (size) {
    case kBranch16Bit:
      return 2u;
    case kBranch32Bit:
      return 4u;

    case kCbxz16Bit:
      return 2u;
    case kCbxz32Bit:
      return 4u;
    case kCbxz48Bit:
      return 6u;

    case kLiteral1KiB:
      return 2u;
    case kLiteral4KiB:
      return 4u;
    case kLiteral64KiB:
      return 8u;
    case kLiteral1MiB:
      return 10u;
    case kLiteralFar:
      return 14u;

    case kLiteralAddr1KiB:
      return 2u;
    case kLiteralAddr4KiB:
      return 4u;
    case kLiteralAddr64KiB:
      return 6u;
    case kLiteralAddrFar:
      return 10u;

    case kLongOrFPLiteral1KiB:
      return 4u;
    case kLongOrFPLiteral256KiB:
      return 10u;
    case kLongOrFPLiteralFar:
      return 14u;
  }
  LOG(FATAL) << "Unexpected size: " << static_cast<int>(size);
  UNREACHABLE();
}

inline uint32_t Thumb2Assembler::Fixup::GetOriginalSizeInBytes() const {
  return SizeInBytes(original_size_);
}

inline uint32_t Thumb2Assembler::Fixup::GetSizeInBytes() const {
  return SizeInBytes(size_);
}

inline size_t Thumb2Assembler::Fixup::LiteralPoolPaddingSize(uint32_t current_code_size) {
  // The code size must be a multiple of 2.
  DCHECK_ALIGNED(current_code_size, 2);
  // If it isn't a multiple of 4, we need to add a 2-byte padding before the literal pool.
  return current_code_size & 2;
}

inline int32_t Thumb2Assembler::Fixup::GetOffset(uint32_t current_code_size) const {
  static constexpr int32_t int32_min = std::numeric_limits<int32_t>::min();
  static constexpr int32_t int32_max = std::numeric_limits<int32_t>::max();
  DCHECK_LE(target_, static_cast<uint32_t>(int32_max));
  DCHECK_LE(location_, static_cast<uint32_t>(int32_max));
  DCHECK_LE(adjustment_, static_cast<uint32_t>(int32_max));
  int32_t diff = static_cast<int32_t>(target_) - static_cast<int32_t>(location_);
  if (target_ > location_) {
    DCHECK_LE(adjustment_, static_cast<uint32_t>(int32_max - diff));
    diff += static_cast<int32_t>(adjustment_);
  } else {
    DCHECK_LE(int32_min + static_cast<int32_t>(adjustment_), diff);
    diff -= static_cast<int32_t>(adjustment_);
  }
  // The default PC adjustment for Thumb2 is 4 bytes.
  DCHECK_GE(diff, int32_min + 4);
  diff -= 4;
  // Add additional adjustment for instructions preceding the PC usage, padding
  // before the literal pool and rounding down the PC for literal loads.
  switch (GetSize()) {
    case kBranch16Bit:
    case kBranch32Bit:
      break;

    case kCbxz16Bit:
      break;
    case kCbxz32Bit:
    case kCbxz48Bit:
      DCHECK_GE(diff, int32_min + 2);
      diff -= 2;        // Extra CMP Rn, #0, 16-bit.
      break;

    case kLiteral1KiB:
    case kLiteral4KiB:
    case kLongOrFPLiteral1KiB:
    case kLiteralAddr1KiB:
    case kLiteralAddr4KiB:
      DCHECK(diff >= 0 || (GetSize() == kLiteral1KiB && diff == -2));
      diff += LiteralPoolPaddingSize(current_code_size);
      // Load literal instructions round down the PC+4 to a multiple of 4, so if the PC
      // isn't a multiple of 2, we need to adjust. Since we already adjusted for the target
      // being aligned, current PC alignment can be inferred from diff.
      DCHECK_ALIGNED(diff, 2);
      diff = diff + (diff & 2);
      DCHECK_GE(diff, 0);
      break;
    case kLiteral1MiB:
    case kLiteral64KiB:
    case kLongOrFPLiteral256KiB:
    case kLiteralAddr64KiB:
      DCHECK_GE(diff, 4);  // The target must be at least 4 bytes after the ADD rX, PC.
      diff -= 4;        // One extra 32-bit MOV.
      diff += LiteralPoolPaddingSize(current_code_size);
      break;
    case kLiteralFar:
    case kLongOrFPLiteralFar:
    case kLiteralAddrFar:
      DCHECK_GE(diff, 8);  // The target must be at least 4 bytes after the ADD rX, PC.
      diff -= 8;        // Extra MOVW+MOVT; both 32-bit.
      diff += LiteralPoolPaddingSize(current_code_size);
      break;
  }
  return diff;
}

inline size_t Thumb2Assembler::Fixup::IncreaseSize(Size new_size) {
  DCHECK_NE(target_, kUnresolved);
  Size old_size = size_;
  size_ = new_size;
  DCHECK_GT(SizeInBytes(new_size), SizeInBytes(old_size));
  size_t adjustment = SizeInBytes(new_size) - SizeInBytes(old_size);
  if (target_ > location_) {
    adjustment_ += adjustment;
  }
  return adjustment;
}

uint32_t Thumb2Assembler::Fixup::AdjustSizeIfNeeded(uint32_t current_code_size) {
  uint32_t old_code_size = current_code_size;
  switch (GetSize()) {
    case kBranch16Bit:
      if (IsInt(cond_ != AL ? 9 : 12, GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kBranch32Bit);
      FALLTHROUGH_INTENDED;
    case kBranch32Bit:
      // We don't support conditional branches beyond +-1MiB
      // or unconditional branches beyond +-16MiB.
      break;

    case kCbxz16Bit:
      if (IsUint<7>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kCbxz32Bit);
      FALLTHROUGH_INTENDED;
    case kCbxz32Bit:
      if (IsInt<9>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kCbxz48Bit);
      FALLTHROUGH_INTENDED;
    case kCbxz48Bit:
      // We don't support conditional branches beyond +-1MiB.
      break;

    case kLiteral1KiB:
      DCHECK(!IsHighRegister(rn_));
      if (IsUint<10>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kLiteral4KiB);
      FALLTHROUGH_INTENDED;
    case kLiteral4KiB:
      if (IsUint<12>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kLiteral64KiB);
      FALLTHROUGH_INTENDED;
    case kLiteral64KiB:
      // Can't handle high register which we can encounter by fall-through from kLiteral4KiB.
      if (!IsHighRegister(rn_) && IsUint<16>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kLiteral1MiB);
      FALLTHROUGH_INTENDED;
    case kLiteral1MiB:
      if (IsUint<20>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kLiteralFar);
      FALLTHROUGH_INTENDED;
    case kLiteralFar:
      // This encoding can reach any target.
      break;

    case kLiteralAddr1KiB:
      DCHECK(!IsHighRegister(rn_));
      if (IsUint<10>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kLiteralAddr4KiB);
      FALLTHROUGH_INTENDED;
    case kLiteralAddr4KiB:
      if (IsUint<12>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kLiteralAddr64KiB);
      FALLTHROUGH_INTENDED;
    case kLiteralAddr64KiB:
      if (IsUint<16>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kLiteralAddrFar);
      FALLTHROUGH_INTENDED;
    case kLiteralAddrFar:
      // This encoding can reach any target.
      break;

    case kLongOrFPLiteral1KiB:
      if (IsUint<10>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kLongOrFPLiteral256KiB);
      FALLTHROUGH_INTENDED;
    case kLongOrFPLiteral256KiB:
      if (IsUint<18>(GetOffset(current_code_size))) {
        break;
      }
      current_code_size += IncreaseSize(kLongOrFPLiteralFar);
      FALLTHROUGH_INTENDED;
    case kLongOrFPLiteralFar:
      // This encoding can reach any target.
      break;
  }
  return current_code_size - old_code_size;
}

void Thumb2Assembler::Fixup::Emit(AssemblerBuffer* buffer, uint32_t code_size) const {
  switch (GetSize()) {
    case kBranch16Bit: {
      DCHECK(type_ == kUnconditional || type_ == kConditional);
      DCHECK_EQ(type_ == kConditional, cond_ != AL);
      int16_t encoding = BEncoding16(GetOffset(code_size), cond_);
      buffer->Store<int16_t>(location_, encoding);
      break;
    }
    case kBranch32Bit: {
      DCHECK(type_ == kConditional || type_ == kUnconditional ||
             type_ == kUnconditionalLink || type_ == kUnconditionalLinkX);
      DCHECK_EQ(type_ == kConditional, cond_ != AL);
      int32_t encoding = BEncoding32(GetOffset(code_size), cond_);
      if (type_ == kUnconditionalLink) {
        DCHECK_NE(encoding & B12, 0);
        encoding |= B14;
      } else if (type_ == kUnconditionalLinkX) {
        DCHECK_NE(encoding & B12, 0);
        encoding ^= B14 | B12;
      }
      buffer->Store<int16_t>(location_, encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(encoding & 0xffff));
      break;
    }

    case kCbxz16Bit: {
      DCHECK(type_ == kCompareAndBranchXZero);
      int16_t encoding = CbxzEncoding16(rn_, GetOffset(code_size), cond_);
      buffer->Store<int16_t>(location_, encoding);
      break;
    }
    case kCbxz32Bit: {
      DCHECK(type_ == kCompareAndBranchXZero);
      DCHECK(cond_ == EQ || cond_ == NE);
      int16_t cmp_encoding = CmpRnImm8Encoding16(rn_, 0);
      int16_t b_encoding = BEncoding16(GetOffset(code_size), cond_);
      buffer->Store<int16_t>(location_, cmp_encoding);
      buffer->Store<int16_t>(location_ + 2, b_encoding);
      break;
    }
    case kCbxz48Bit: {
      DCHECK(type_ == kCompareAndBranchXZero);
      DCHECK(cond_ == EQ || cond_ == NE);
      int16_t cmp_encoding = CmpRnImm8Encoding16(rn_, 0);
      int32_t b_encoding = BEncoding32(GetOffset(code_size), cond_);
      buffer->Store<int16_t>(location_, cmp_encoding);
      buffer->Store<int16_t>(location_ + 2u, b_encoding >> 16);
      buffer->Store<int16_t>(location_ + 4u, static_cast<int16_t>(b_encoding & 0xffff));
      break;
    }

    case kLiteral1KiB: {
      DCHECK(type_ == kLoadLiteralNarrow);
      int16_t encoding = LdrLitEncoding16(rn_, GetOffset(code_size));
      buffer->Store<int16_t>(location_, encoding);
      break;
    }
    case kLiteral4KiB: {
      DCHECK(type_ == kLoadLiteralNarrow);
      // GetOffset() uses PC+4 but load literal uses AlignDown(PC+4, 4). Adjust offset accordingly.
      int32_t encoding = LdrLitEncoding32(rn_, GetOffset(code_size));
      buffer->Store<int16_t>(location_, encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(encoding & 0xffff));
      break;
    }
    case kLiteral64KiB: {
      DCHECK(type_ == kLoadLiteralNarrow);
      int32_t mov_encoding = MovwEncoding32(rn_, GetOffset(code_size));
      int16_t add_pc_encoding = AddRdnRmEncoding16(rn_, PC);
      int16_t ldr_encoding = LdrRtRnImm5Encoding16(rn_, rn_, 0);
      buffer->Store<int16_t>(location_, mov_encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(mov_encoding & 0xffff));
      buffer->Store<int16_t>(location_ + 4u, add_pc_encoding);
      buffer->Store<int16_t>(location_ + 6u, ldr_encoding);
      break;
    }
    case kLiteral1MiB: {
      DCHECK(type_ == kLoadLiteralNarrow);
      int32_t offset = GetOffset(code_size);
      int32_t mov_encoding = MovModImmEncoding32(rn_, offset & ~0xfff);
      int16_t add_pc_encoding = AddRdnRmEncoding16(rn_, PC);
      int32_t ldr_encoding = LdrRtRnImm12Encoding(rn_, rn_, offset & 0xfff);
      buffer->Store<int16_t>(location_, mov_encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(mov_encoding & 0xffff));
      buffer->Store<int16_t>(location_ + 4u, add_pc_encoding);
      buffer->Store<int16_t>(location_ + 6u, ldr_encoding >> 16);
      buffer->Store<int16_t>(location_ + 8u, static_cast<int16_t>(ldr_encoding & 0xffff));
      break;
    }
    case kLiteralFar: {
      DCHECK(type_ == kLoadLiteralNarrow);
      int32_t offset = GetOffset(code_size);
      int32_t movw_encoding = MovwEncoding32(rn_, offset & 0xffff);
      int32_t movt_encoding = MovtEncoding32(rn_, offset & ~0xffff);
      int16_t add_pc_encoding = AddRdnRmEncoding16(rn_, PC);
      int32_t ldr_encoding = LdrRtRnImm12Encoding(rn_, rn_, 0);
      buffer->Store<int16_t>(location_, movw_encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(movw_encoding & 0xffff));
      buffer->Store<int16_t>(location_ + 4u, movt_encoding >> 16);
      buffer->Store<int16_t>(location_ + 6u, static_cast<int16_t>(movt_encoding & 0xffff));
      buffer->Store<int16_t>(location_ + 8u, add_pc_encoding);
      buffer->Store<int16_t>(location_ + 10u, ldr_encoding >> 16);
      buffer->Store<int16_t>(location_ + 12u, static_cast<int16_t>(ldr_encoding & 0xffff));
      break;
    }

    case kLiteralAddr1KiB: {
      DCHECK(type_ == kLoadLiteralAddr);
      int16_t encoding = AdrEncoding16(rn_, GetOffset(code_size));
      buffer->Store<int16_t>(location_, encoding);
      break;
    }
    case kLiteralAddr4KiB: {
      DCHECK(type_ == kLoadLiteralAddr);
      int32_t encoding = AdrEncoding32(rn_, GetOffset(code_size));
      buffer->Store<int16_t>(location_, encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(encoding & 0xffff));
      break;
    }
    case kLiteralAddr64KiB: {
      DCHECK(type_ == kLoadLiteralAddr);
      int32_t mov_encoding = MovwEncoding32(rn_, GetOffset(code_size));
      int16_t add_pc_encoding = AddRdnRmEncoding16(rn_, PC);
      buffer->Store<int16_t>(location_, mov_encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(mov_encoding & 0xffff));
      buffer->Store<int16_t>(location_ + 4u, add_pc_encoding);
      break;
    }
    case kLiteralAddrFar: {
      DCHECK(type_ == kLoadLiteralAddr);
      int32_t offset = GetOffset(code_size);
      int32_t movw_encoding = MovwEncoding32(rn_, offset & 0xffff);
      int32_t movt_encoding = MovtEncoding32(rn_, offset & ~0xffff);
      int16_t add_pc_encoding = AddRdnRmEncoding16(rn_, PC);
      buffer->Store<int16_t>(location_, movw_encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(movw_encoding & 0xffff));
      buffer->Store<int16_t>(location_ + 4u, movt_encoding >> 16);
      buffer->Store<int16_t>(location_ + 6u, static_cast<int16_t>(movt_encoding & 0xffff));
      buffer->Store<int16_t>(location_ + 8u, add_pc_encoding);
      break;
    }

    case kLongOrFPLiteral1KiB: {
      int32_t encoding = LoadWideOrFpEncoding(PC, GetOffset(code_size));  // DCHECKs type_.
      buffer->Store<int16_t>(location_, encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(encoding & 0xffff));
      break;
    }
    case kLongOrFPLiteral256KiB: {
      int32_t offset = GetOffset(code_size);
      int32_t mov_encoding = MovModImmEncoding32(IP, offset & ~0x3ff);
      int16_t add_pc_encoding = AddRdnRmEncoding16(IP, PC);
      int32_t ldr_encoding = LoadWideOrFpEncoding(IP, offset & 0x3ff);    // DCHECKs type_.
      buffer->Store<int16_t>(location_, mov_encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(mov_encoding & 0xffff));
      buffer->Store<int16_t>(location_ + 4u, add_pc_encoding);
      buffer->Store<int16_t>(location_ + 6u, ldr_encoding >> 16);
      buffer->Store<int16_t>(location_ + 8u, static_cast<int16_t>(ldr_encoding & 0xffff));
      break;
    }
    case kLongOrFPLiteralFar: {
      int32_t offset = GetOffset(code_size);
      int32_t movw_encoding = MovwEncoding32(IP, offset & 0xffff);
      int32_t movt_encoding = MovtEncoding32(IP, offset & ~0xffff);
      int16_t add_pc_encoding = AddRdnRmEncoding16(IP, PC);
      int32_t ldr_encoding = LoadWideOrFpEncoding(IP, 0);                 // DCHECKs type_.
      buffer->Store<int16_t>(location_, movw_encoding >> 16);
      buffer->Store<int16_t>(location_ + 2u, static_cast<int16_t>(movw_encoding & 0xffff));
      buffer->Store<int16_t>(location_ + 4u, movt_encoding >> 16);
      buffer->Store<int16_t>(location_ + 6u, static_cast<int16_t>(movt_encoding & 0xffff));
      buffer->Store<int16_t>(location_ + 8u, add_pc_encoding);
      buffer->Store<int16_t>(location_ + 10u, ldr_encoding >> 16);
      buffer->Store<int16_t>(location_ + 12u, static_cast<int16_t>(ldr_encoding & 0xffff));
      break;
    }
  }
}

uint16_t Thumb2Assembler::EmitCompareAndBranch(Register rn, uint16_t prev, bool n) {
  CHECK(IsLowRegister(rn));
  uint32_t location = buffer_.Size();

  // This is always unresolved as it must be a forward branch.
  Emit16(prev);      // Previous link.
  return AddFixup(Fixup::CompareAndBranch(location, rn, n ? NE : EQ));
}


// NOTE: this only support immediate offsets, not [rx,ry].
// TODO: support [rx,ry] instructions.
void Thumb2Assembler::EmitLoadStore(Condition cond,
                                    bool load,
                                    bool byte,
                                    bool half,
                                    bool is_signed,
                                    Register rd,
                                    const Address& ad) {
  CHECK_NE(rd, kNoRegister);
  CheckCondition(cond);
  bool must_be_32bit = force_32bit_;
  if (IsHighRegister(rd)) {
    must_be_32bit = true;
  }

  Register rn = ad.GetRegister();
  if (IsHighRegister(rn) && rn != SP && rn != PC) {
    must_be_32bit = true;
  }

  if (is_signed || ad.GetOffset() < 0 || ad.GetMode() != Address::Offset) {
    must_be_32bit = true;
  }

  if (ad.IsImmediate()) {
    // Immediate offset
    int32_t offset = ad.GetOffset();

    // The 16 bit SP relative instruction can only have a 10 bit offset.
    if (rn == SP && offset >= (1 << 10)) {
      must_be_32bit = true;
    }

    if (byte) {
      // 5 bit offset, no shift.
      if (offset >= (1 << 5)) {
        must_be_32bit = true;
      }
    } else if (half) {
      // 6 bit offset, shifted by 1.
      if (offset >= (1 << 6)) {
        must_be_32bit = true;
      }
    } else {
      // 7 bit offset, shifted by 2.
      if (offset >= (1 << 7)) {
        must_be_32bit = true;
      }
    }

    if (must_be_32bit) {
      int32_t encoding = B31 | B30 | B29 | B28 | B27 |
          (load ? B20 : 0) |
          (is_signed ? B24 : 0) |
          static_cast<uint32_t>(rd) << 12 |
          ad.encodingThumb(true) |
          (byte ? 0 : half ? B21 : B22);
      Emit32(encoding);
    } else {
      // 16 bit thumb1.
      uint8_t opA = 0;
      bool sp_relative = false;

      if (byte) {
        opA = 7U /* 0b0111 */;
      } else if (half) {
        opA = 8U /* 0b1000 */;
      } else {
        if (rn == SP) {
          opA = 9U /* 0b1001 */;
          sp_relative = true;
        } else {
          opA = 6U /* 0b0110 */;
        }
      }
      int16_t encoding = opA << 12 |
          (load ? B11 : 0);

      CHECK_GE(offset, 0);
      if (sp_relative) {
        // SP relative, 10 bit offset.
        CHECK_LT(offset, (1 << 10));
        CHECK_ALIGNED(offset, 4);
        encoding |= rd << 8 | offset >> 2;
      } else {
        // No SP relative.  The offset is shifted right depending on
        // the size of the load/store.
        encoding |= static_cast<uint32_t>(rd);

        if (byte) {
          // 5 bit offset, no shift.
          CHECK_LT(offset, (1 << 5));
        } else if (half) {
          // 6 bit offset, shifted by 1.
          CHECK_LT(offset, (1 << 6));
          CHECK_ALIGNED(offset, 2);
          offset >>= 1;
        } else {
          // 7 bit offset, shifted by 2.
          CHECK_LT(offset, (1 << 7));
          CHECK_ALIGNED(offset, 4);
          offset >>= 2;
        }
        encoding |= rn << 3 | offset  << 6;
      }

      Emit16(encoding);
    }
  } else {
    // Register shift.
    if (ad.GetRegister() == PC) {
       // PC relative literal encoding.
      int32_t offset = ad.GetOffset();
      if (must_be_32bit || offset < 0 || offset >= (1 << 10) || !load) {
        int32_t up = B23;
        if (offset < 0) {
          offset = -offset;
          up = 0;
        }
        CHECK_LT(offset, (1 << 12));
        int32_t encoding = 0x1f << 27 | 0xf << 16 | B22 | (load ? B20 : 0) |
            offset | up |
            static_cast<uint32_t>(rd) << 12;
        Emit32(encoding);
      } else {
        // 16 bit literal load.
        CHECK_GE(offset, 0);
        CHECK_LT(offset, (1 << 10));
        int32_t encoding = B14 | (load ? B11 : 0) | static_cast<uint32_t>(rd) << 8 | offset >> 2;
        Emit16(encoding);
      }
    } else {
      if (ad.GetShiftCount() != 0) {
        // If there is a shift count this must be 32 bit.
        must_be_32bit = true;
      } else if (IsHighRegister(ad.GetRegisterOffset())) {
        must_be_32bit = true;
      }

      if (must_be_32bit) {
        int32_t encoding = 0x1f << 27 | (load ? B20 : 0) | static_cast<uint32_t>(rd) << 12 |
            ad.encodingThumb(true);
        if (half) {
          encoding |= B21;
        } else if (!byte) {
          encoding |= B22;
        }
        Emit32(encoding);
      } else {
        // 16 bit register offset.
        int32_t encoding = B14 | B12 | (load ? B11 : 0) | static_cast<uint32_t>(rd) |
            ad.encodingThumb(false);
        if (byte) {
          encoding |= B10;
        } else if (half) {
          encoding |= B9;
        }
        Emit16(encoding);
      }
    }
  }
}


void Thumb2Assembler::EmitMultiMemOp(Condition cond,
                                     BlockAddressMode bam,
                                     bool load,
                                     Register base,
                                     RegList regs) {
  CHECK_NE(base, kNoRegister);
  CheckCondition(cond);
  bool must_be_32bit = force_32bit_;

  if (!must_be_32bit && base == SP && bam == (load ? IA_W : DB_W) &&
      (regs & 0xff00 & ~(1 << (load ? PC : LR))) == 0) {
    // Use 16-bit PUSH/POP.
    int16_t encoding = B15 | B13 | B12 | (load ? B11 : 0) | B10 |
        ((regs & (1 << (load ? PC : LR))) != 0 ? B8 : 0) | (regs & 0x00ff);
    Emit16(encoding);
    return;
  }

  if ((regs & 0xff00) != 0) {
    must_be_32bit = true;
  }

  bool w_bit = bam == IA_W || bam == DB_W || bam == DA_W || bam == IB_W;
  // 16 bit always uses writeback.
  if (!w_bit) {
    must_be_32bit = true;
  }

  if (must_be_32bit) {
    uint32_t op = 0;
    switch (bam) {
      case IA:
      case IA_W:
        op = 1U /* 0b01 */;
        break;
      case DB:
      case DB_W:
        op = 2U /* 0b10 */;
        break;
      case DA:
      case IB:
      case DA_W:
      case IB_W:
        LOG(FATAL) << "LDM/STM mode not supported on thumb: " << bam;
        UNREACHABLE();
    }
    if (load) {
      // Cannot have SP in the list.
      CHECK_EQ((regs & (1 << SP)), 0);
    } else {
      // Cannot have PC or SP in the list.
      CHECK_EQ((regs & (1 << PC | 1 << SP)), 0);
    }
    int32_t encoding = B31 | B30 | B29 | B27 |
                    (op << 23) |
                    (load ? B20 : 0) |
                    base << 16 |
                    regs |
                    (w_bit << 21);
    Emit32(encoding);
  } else {
    int16_t encoding = B15 | B14 |
                    (load ? B11 : 0) |
                    base << 8 |
                    regs;
    Emit16(encoding);
  }
}

void Thumb2Assembler::EmitBranch(Condition cond, Label* label, bool link, bool x) {
  bool use32bit = IsForced32Bit() || !CanRelocateBranches();
  uint32_t pc = buffer_.Size();
  Fixup::Type branch_type;
  if (cond == AL) {
    if (link) {
      use32bit = true;
      if (x) {
        branch_type = Fixup::kUnconditionalLinkX;      // BLX.
      } else {
        branch_type = Fixup::kUnconditionalLink;       // BX.
      }
    } else {
      branch_type = Fixup::kUnconditional;             // B.
      // The T2 encoding offset is `SignExtend(imm11:'0', 32)` and there is a PC adjustment of 4.
      static constexpr size_t kMaxT2BackwardDistance = (1u << 11) - 4u;
      if (!use32bit && label->IsBound() && pc - label->Position() > kMaxT2BackwardDistance) {
        use32bit = true;
      }
    }
  } else {
    branch_type = Fixup::kConditional;                 // B<cond>.
    // The T1 encoding offset is `SignExtend(imm8:'0', 32)` and there is a PC adjustment of 4.
    static constexpr size_t kMaxT1BackwardDistance = (1u << 8) - 4u;
    if (!use32bit && label->IsBound() && pc - label->Position() > kMaxT1BackwardDistance) {
      use32bit = true;
    }
  }

  Fixup::Size size = use32bit ? Fixup::kBranch32Bit : Fixup::kBranch16Bit;
  FixupId branch_id = AddFixup(Fixup::Branch(pc, branch_type, size, cond));

  if (label->IsBound()) {
    // The branch is to a bound label which means that it's a backwards branch.
    GetFixup(branch_id)->Resolve(label->Position());
    Emit16(0);
  } else {
    // Branch target is an unbound label. Add it to a singly-linked list maintained within
    // the code with the label serving as the head.
    Emit16(static_cast<uint16_t>(label->position_));
    label->LinkTo(branch_id);
  }

  if (use32bit) {
    Emit16(0);
  }
  DCHECK_EQ(buffer_.Size() - pc, GetFixup(branch_id)->GetSizeInBytes());
}


void Thumb2Assembler::Emit32Miscellaneous(uint8_t op1,
                                          uint8_t op2,
                                          uint32_t rest_encoding) {
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B23 |
      op1 << 20 |
      0xf << 12 |
      B7 |
      op2 << 4 |
      rest_encoding;
  Emit32(encoding);
}


void Thumb2Assembler::Emit16Miscellaneous(uint32_t rest_encoding) {
  int16_t encoding = B15 | B13 | B12 |
      rest_encoding;
  Emit16(encoding);
}

void Thumb2Assembler::clz(Register rd, Register rm, Condition cond) {
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(rm, kNoRegister);
  CheckCondition(cond);
  CHECK_NE(rd, PC);
  CHECK_NE(rm, PC);
  int32_t encoding =
      static_cast<uint32_t>(rm) << 16 |
      static_cast<uint32_t>(rd) << 8 |
      static_cast<uint32_t>(rm);
  Emit32Miscellaneous(0b11, 0b00, encoding);
}


void Thumb2Assembler::movw(Register rd, uint16_t imm16, Condition cond) {
  CheckCondition(cond);
  // Always 32 bits, encoding T3. (Other encondings are called MOV, not MOVW.)
  uint32_t imm4 = (imm16 >> 12) & 15U /* 0b1111 */;
  uint32_t i = (imm16 >> 11) & 1U /* 0b1 */;
  uint32_t imm3 = (imm16 >> 8) & 7U /* 0b111 */;
  uint32_t imm8 = imm16 & 0xff;
  int32_t encoding = B31 | B30 | B29 | B28 |
                  B25 | B22 |
                  static_cast<uint32_t>(rd) << 8 |
                  i << 26 |
                  imm4 << 16 |
                  imm3 << 12 |
                  imm8;
  Emit32(encoding);
}


void Thumb2Assembler::movt(Register rd, uint16_t imm16, Condition cond) {
  CheckCondition(cond);
  // Always 32 bits.
  uint32_t imm4 = (imm16 >> 12) & 15U /* 0b1111 */;
  uint32_t i = (imm16 >> 11) & 1U /* 0b1 */;
  uint32_t imm3 = (imm16 >> 8) & 7U /* 0b111 */;
  uint32_t imm8 = imm16 & 0xff;
  int32_t encoding = B31 | B30 | B29 | B28 |
                  B25 | B23 | B22 |
                  static_cast<uint32_t>(rd) << 8 |
                  i << 26 |
                  imm4 << 16 |
                  imm3 << 12 |
                  imm8;
  Emit32(encoding);
}


void Thumb2Assembler::rbit(Register rd, Register rm, Condition cond) {
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(rm, kNoRegister);
  CheckCondition(cond);
  CHECK_NE(rd, PC);
  CHECK_NE(rm, PC);
  CHECK_NE(rd, SP);
  CHECK_NE(rm, SP);
  int32_t encoding =
      static_cast<uint32_t>(rm) << 16 |
      static_cast<uint32_t>(rd) << 8 |
      static_cast<uint32_t>(rm);

  Emit32Miscellaneous(0b01, 0b10, encoding);
}


void Thumb2Assembler::EmitReverseBytes(Register rd, Register rm,
                                       uint32_t op) {
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(rm, kNoRegister);
  CHECK_NE(rd, PC);
  CHECK_NE(rm, PC);
  CHECK_NE(rd, SP);
  CHECK_NE(rm, SP);

  if (!IsHighRegister(rd) && !IsHighRegister(rm) && !force_32bit_) {
    uint16_t t1_op = B11 | B9 | (op << 6);
    int16_t encoding = t1_op |
        static_cast<uint16_t>(rm) << 3 |
        static_cast<uint16_t>(rd);
    Emit16Miscellaneous(encoding);
  } else {
    int32_t encoding =
        static_cast<uint32_t>(rm) << 16 |
        static_cast<uint32_t>(rd) << 8 |
        static_cast<uint32_t>(rm);
    Emit32Miscellaneous(0b01, op, encoding);
  }
}


void Thumb2Assembler::rev(Register rd, Register rm, Condition cond) {
  CheckCondition(cond);
  EmitReverseBytes(rd, rm, 0b00);
}


void Thumb2Assembler::rev16(Register rd, Register rm, Condition cond) {
  CheckCondition(cond);
  EmitReverseBytes(rd, rm, 0b01);
}


void Thumb2Assembler::revsh(Register rd, Register rm, Condition cond) {
  CheckCondition(cond);
  EmitReverseBytes(rd, rm, 0b11);
}


void Thumb2Assembler::ldrex(Register rt, Register rn, uint16_t imm, Condition cond) {
  CHECK_NE(rn, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  CheckCondition(cond);
  CHECK_LT(imm, (1u << 10));

  int32_t encoding = B31 | B30 | B29 | B27 | B22 | B20 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rt) << 12 |
      0xf << 8 |
      imm >> 2;
  Emit32(encoding);
}


void Thumb2Assembler::ldrex(Register rt, Register rn, Condition cond) {
  ldrex(rt, rn, 0, cond);
}


void Thumb2Assembler::strex(Register rd,
                            Register rt,
                            Register rn,
                            uint16_t imm,
                            Condition cond) {
  CHECK_NE(rn, kNoRegister);
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  CheckCondition(cond);
  CHECK_LT(imm, (1u << 10));

  int32_t encoding = B31 | B30 | B29 | B27 | B22 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rt) << 12 |
      static_cast<uint32_t>(rd) << 8 |
      imm >> 2;
  Emit32(encoding);
}


void Thumb2Assembler::ldrexd(Register rt, Register rt2, Register rn, Condition cond) {
  CHECK_NE(rn, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt, rt2);
  CheckCondition(cond);

  int32_t encoding = B31 | B30 | B29 | B27 | B23 | B22 | B20 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rt) << 12 |
      static_cast<uint32_t>(rt2) << 8 |
      B6 | B5 | B4 | B3 | B2 | B1 | B0;
  Emit32(encoding);
}


void Thumb2Assembler::strex(Register rd,
                            Register rt,
                            Register rn,
                            Condition cond) {
  strex(rd, rt, rn, 0, cond);
}


void Thumb2Assembler::strexd(Register rd, Register rt, Register rt2, Register rn, Condition cond) {
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(rn, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt, rt2);
  CHECK_NE(rd, rt);
  CHECK_NE(rd, rt2);
  CheckCondition(cond);

  int32_t encoding = B31 | B30 | B29 | B27 | B23 | B22 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rt) << 12 |
      static_cast<uint32_t>(rt2) << 8 |
      B6 | B5 | B4 |
      static_cast<uint32_t>(rd);
  Emit32(encoding);
}


void Thumb2Assembler::clrex(Condition cond) {
  CheckCondition(cond);
  int32_t encoding = B31 | B30 | B29 | B27 | B28 | B25 | B24 | B23 |
      B21 | B20 |
      0xf << 16 |
      B15 |
      0xf << 8 |
      B5 |
      0xf;
  Emit32(encoding);
}


void Thumb2Assembler::nop(Condition cond) {
  CheckCondition(cond);
  uint16_t encoding = B15 | B13 | B12 |
      B11 | B10 | B9 | B8;
  Emit16(static_cast<int16_t>(encoding));
}


void Thumb2Assembler::vmovsr(SRegister sn, Register rt, Condition cond) {
  CHECK_NE(sn, kNoSRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 |
                     ((static_cast<int32_t>(sn) >> 1)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sn) & 1)*B7) | B4;
  Emit32(encoding);
}


void Thumb2Assembler::vmovrs(Register rt, SRegister sn, Condition cond) {
  CHECK_NE(sn, kNoSRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B20 |
                     ((static_cast<int32_t>(sn) >> 1)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sn) & 1)*B7) | B4;
  Emit32(encoding);
}


void Thumb2Assembler::vmovsrr(SRegister sm, Register rt, Register rt2,
                              Condition cond) {
  CHECK_NE(sm, kNoSRegister);
  CHECK_NE(sm, S31);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sm) & 1)*B5) | B4 |
                     (static_cast<int32_t>(sm) >> 1);
  Emit32(encoding);
}


void Thumb2Assembler::vmovrrs(Register rt, Register rt2, SRegister sm,
                              Condition cond) {
  CHECK_NE(sm, kNoSRegister);
  CHECK_NE(sm, S31);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CHECK_NE(rt, rt2);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 | B20 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sm) & 1)*B5) | B4 |
                     (static_cast<int32_t>(sm) >> 1);
  Emit32(encoding);
}


void Thumb2Assembler::vmovdrr(DRegister dm, Register rt, Register rt2,
                              Condition cond) {
  CHECK_NE(dm, kNoDRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 | B8 |
                     ((static_cast<int32_t>(dm) >> 4)*B5) | B4 |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit32(encoding);
}


void Thumb2Assembler::vmovrrd(Register rt, Register rt2, DRegister dm,
                              Condition cond) {
  CHECK_NE(dm, kNoDRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CHECK_NE(rt, rt2);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 | B20 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 | B8 |
                     ((static_cast<int32_t>(dm) >> 4)*B5) | B4 |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit32(encoding);
}


void Thumb2Assembler::vldrs(SRegister sd, const Address& ad, Condition cond) {
  const Address& addr = static_cast<const Address&>(ad);
  CHECK_NE(sd, kNoSRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 | B20 |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     B11 | B9 | addr.vencoding();
  Emit32(encoding);
}


void Thumb2Assembler::vstrs(SRegister sd, const Address& ad, Condition cond) {
  const Address& addr = static_cast<const Address&>(ad);
  CHECK_NE(static_cast<Register>(addr.encodingArm() & (0xf << kRnShift)), PC);
  CHECK_NE(sd, kNoSRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     B11 | B9 | addr.vencoding();
  Emit32(encoding);
}


void Thumb2Assembler::vldrd(DRegister dd, const Address& ad, Condition cond) {
  const Address& addr = static_cast<const Address&>(ad);
  CHECK_NE(dd, kNoDRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 | B20 |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     B11 | B9 | B8 | addr.vencoding();
  Emit32(encoding);
}


void Thumb2Assembler::vstrd(DRegister dd, const Address& ad, Condition cond) {
  const Address& addr = static_cast<const Address&>(ad);
  CHECK_NE(static_cast<Register>(addr.encodingArm() & (0xf << kRnShift)), PC);
  CHECK_NE(dd, kNoDRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     B11 | B9 | B8 | addr.vencoding();
  Emit32(encoding);
}


void Thumb2Assembler::vpushs(SRegister reg, int nregs, Condition cond) {
  EmitVPushPop(static_cast<uint32_t>(reg), nregs, true, false, cond);
}


void Thumb2Assembler::vpushd(DRegister reg, int nregs, Condition cond) {
  EmitVPushPop(static_cast<uint32_t>(reg), nregs, true, true, cond);
}


void Thumb2Assembler::vpops(SRegister reg, int nregs, Condition cond) {
  EmitVPushPop(static_cast<uint32_t>(reg), nregs, false, false, cond);
}


void Thumb2Assembler::vpopd(DRegister reg, int nregs, Condition cond) {
  EmitVPushPop(static_cast<uint32_t>(reg), nregs, false, true, cond);
}


void Thumb2Assembler::EmitVPushPop(uint32_t reg, int nregs, bool push, bool dbl, Condition cond) {
  CheckCondition(cond);

  uint32_t D;
  uint32_t Vd;
  if (dbl) {
    // Encoded as D:Vd.
    D = (reg >> 4) & 1;
    Vd = reg & 15U /* 0b1111 */;
  } else {
    // Encoded as Vd:D.
    D = reg & 1;
    Vd = (reg >> 1) & 15U /* 0b1111 */;
  }
  int32_t encoding = B27 | B26 | B21 | B19 | B18 | B16 |
                    B11 | B9 |
        (dbl ? B8 : 0) |
        (push ? B24 : (B23 | B20)) |
        14U /* 0b1110 */ << 28 |
        nregs << (dbl ? 1 : 0) |
        D << 22 |
        Vd << 12;
  Emit32(encoding);
}


void Thumb2Assembler::EmitVFPsss(Condition cond, int32_t opcode,
                                 SRegister sd, SRegister sn, SRegister sm) {
  CHECK_NE(sd, kNoSRegister);
  CHECK_NE(sn, kNoSRegister);
  CHECK_NE(sm, kNoSRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | opcode |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sn) >> 1)*B16) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     ((static_cast<int32_t>(sn) & 1)*B7) |
                     ((static_cast<int32_t>(sm) & 1)*B5) |
                     (static_cast<int32_t>(sm) >> 1);
  Emit32(encoding);
}


void Thumb2Assembler::EmitVFPddd(Condition cond, int32_t opcode,
                                 DRegister dd, DRegister dn, DRegister dm) {
  CHECK_NE(dd, kNoDRegister);
  CHECK_NE(dn, kNoDRegister);
  CHECK_NE(dm, kNoDRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | B8 | opcode |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dn) & 0xf)*B16) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     ((static_cast<int32_t>(dn) >> 4)*B7) |
                     ((static_cast<int32_t>(dm) >> 4)*B5) |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit32(encoding);
}


void Thumb2Assembler::EmitVFPsd(Condition cond, int32_t opcode,
                                SRegister sd, DRegister dm) {
  CHECK_NE(sd, kNoSRegister);
  CHECK_NE(dm, kNoDRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | opcode |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     ((static_cast<int32_t>(dm) >> 4)*B5) |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit32(encoding);
}


void Thumb2Assembler::EmitVFPds(Condition cond, int32_t opcode,
                                DRegister dd, SRegister sm) {
  CHECK_NE(dd, kNoDRegister);
  CHECK_NE(sm, kNoSRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | opcode |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     ((static_cast<int32_t>(sm) & 1)*B5) |
                     (static_cast<int32_t>(sm) >> 1);
  Emit32(encoding);
}


void Thumb2Assembler::vmstat(Condition cond) {  // VMRS APSR_nzcv, FPSCR.
  CHECK_NE(cond, kNoCondition);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
      B27 | B26 | B25 | B23 | B22 | B21 | B20 | B16 |
      (static_cast<int32_t>(PC)*B12) |
      B11 | B9 | B4;
  Emit32(encoding);
}


void Thumb2Assembler::svc(uint32_t imm8) {
  CHECK(IsUint<8>(imm8)) << imm8;
  int16_t encoding = B15 | B14 | B12 |
       B11 | B10 | B9 | B8 |
       imm8;
  Emit16(encoding);
}


void Thumb2Assembler::bkpt(uint16_t imm8) {
  CHECK(IsUint<8>(imm8)) << imm8;
  int16_t encoding = B15 | B13 | B12 |
      B11 | B10 | B9 |
      imm8;
  Emit16(encoding);
}

// Convert the given IT state to a mask bit given bit 0 of the first
// condition and a shift position.
static uint8_t ToItMask(ItState s, uint8_t firstcond0, uint8_t shift) {
  switch (s) {
  case kItOmitted: return 1 << shift;
  case kItThen: return firstcond0 << shift;
  case kItElse: return !firstcond0 << shift;
  }
  return 0;
}


// Set the IT condition in the given position for the given state.  This is used
// to check that conditional instructions match the preceding IT statement.
void Thumb2Assembler::SetItCondition(ItState s, Condition cond, uint8_t index) {
  switch (s) {
  case kItOmitted: it_conditions_[index] = AL; break;
  case kItThen: it_conditions_[index] = cond; break;
  case kItElse:
    it_conditions_[index] = static_cast<Condition>(static_cast<uint8_t>(cond) ^ 1);
    break;
  }
}


void Thumb2Assembler::it(Condition firstcond, ItState i1, ItState i2, ItState i3) {
  CheckCondition(AL);       // Not allowed in IT block.
  uint8_t firstcond0 = static_cast<uint8_t>(firstcond) & 1;

  // All conditions to AL.
  for (uint8_t i = 0; i < 4; ++i) {
    it_conditions_[i] = AL;
  }

  SetItCondition(kItThen, firstcond, 0);
  uint8_t mask = ToItMask(i1, firstcond0, 3);
  SetItCondition(i1, firstcond, 1);

  if (i1 != kItOmitted) {
    mask |= ToItMask(i2, firstcond0, 2);
    SetItCondition(i2, firstcond, 2);
    if (i2 != kItOmitted) {
      mask |= ToItMask(i3, firstcond0, 1);
      SetItCondition(i3, firstcond, 3);
      if (i3 != kItOmitted) {
        mask |= 1U /* 0b0001 */;
      }
    }
  }

  // Start at first condition.
  it_cond_index_ = 0;
  next_condition_ = it_conditions_[0];
  uint16_t encoding = B15 | B13 | B12 |
        B11 | B10 | B9 | B8 |
        firstcond << 4 |
        mask;
  Emit16(encoding);
}


void Thumb2Assembler::cbz(Register rn, Label* label) {
  CheckCondition(AL);
  if (label->IsBound()) {
    LOG(FATAL) << "cbz can only be used to branch forwards";
    UNREACHABLE();
  } else if (IsHighRegister(rn)) {
    LOG(FATAL) << "cbz can only be used with low registers";
    UNREACHABLE();
  } else {
    uint16_t branchid = EmitCompareAndBranch(rn, static_cast<uint16_t>(label->position_), false);
    label->LinkTo(branchid);
  }
}


void Thumb2Assembler::cbnz(Register rn, Label* label) {
  CheckCondition(AL);
  if (label->IsBound()) {
    LOG(FATAL) << "cbnz can only be used to branch forwards";
    UNREACHABLE();
  } else if (IsHighRegister(rn)) {
    LOG(FATAL) << "cbnz can only be used with low registers";
    UNREACHABLE();
  } else {
    uint16_t branchid = EmitCompareAndBranch(rn, static_cast<uint16_t>(label->position_), true);
    label->LinkTo(branchid);
  }
}


void Thumb2Assembler::blx(Register rm, Condition cond) {
  CHECK_NE(rm, kNoRegister);
  CheckCondition(cond);
  int16_t encoding = B14 | B10 | B9 | B8 | B7 | static_cast<int16_t>(rm) << 3;
  Emit16(encoding);
}


void Thumb2Assembler::bx(Register rm, Condition cond) {
  CHECK_NE(rm, kNoRegister);
  CheckCondition(cond);
  int16_t encoding = B14 | B10 | B9 | B8 | static_cast<int16_t>(rm) << 3;
  Emit16(encoding);
}


void Thumb2Assembler::Push(Register rd, Condition cond) {
  str(rd, Address(SP, -kRegisterSize, Address::PreIndex), cond);
}


void Thumb2Assembler::Pop(Register rd, Condition cond) {
  ldr(rd, Address(SP, kRegisterSize, Address::PostIndex), cond);
}


void Thumb2Assembler::PushList(RegList regs, Condition cond) {
  stm(DB_W, SP, regs, cond);
}


void Thumb2Assembler::PopList(RegList regs, Condition cond) {
  ldm(IA_W, SP, regs, cond);
}


void Thumb2Assembler::Mov(Register rd, Register rm, Condition cond) {
  if (cond != AL || rd != rm) {
    mov(rd, ShifterOperand(rm), cond);
  }
}


void Thumb2Assembler::Bind(Label* label) {
  BindLabel(label, buffer_.Size());
}


void Thumb2Assembler::Lsl(Register rd, Register rm, uint32_t shift_imm,
                          Condition cond, SetCc set_cc) {
  CHECK_LE(shift_imm, 31u);
  CheckCondition(cond);
  EmitShift(rd, rm, LSL, shift_imm, cond, set_cc);
}


void Thumb2Assembler::Lsr(Register rd, Register rm, uint32_t shift_imm,
                          Condition cond, SetCc set_cc) {
  CHECK(1u <= shift_imm && shift_imm <= 32u);
  if (shift_imm == 32) shift_imm = 0;  // Comply to UAL syntax.
  CheckCondition(cond);
  EmitShift(rd, rm, LSR, shift_imm, cond, set_cc);
}


void Thumb2Assembler::Asr(Register rd, Register rm, uint32_t shift_imm,
                          Condition cond, SetCc set_cc) {
  CHECK(1u <= shift_imm && shift_imm <= 32u);
  if (shift_imm == 32) shift_imm = 0;  // Comply to UAL syntax.
  CheckCondition(cond);
  EmitShift(rd, rm, ASR, shift_imm, cond, set_cc);
}


void Thumb2Assembler::Ror(Register rd, Register rm, uint32_t shift_imm,
                          Condition cond, SetCc set_cc) {
  CHECK(1u <= shift_imm && shift_imm <= 31u);
  CheckCondition(cond);
  EmitShift(rd, rm, ROR, shift_imm, cond, set_cc);
}


void Thumb2Assembler::Rrx(Register rd, Register rm, Condition cond, SetCc set_cc) {
  CheckCondition(cond);
  EmitShift(rd, rm, RRX, 0, cond, set_cc);
}


void Thumb2Assembler::Lsl(Register rd, Register rm, Register rn,
                          Condition cond, SetCc set_cc) {
  CheckCondition(cond);
  EmitShift(rd, rm, LSL, rn, cond, set_cc);
}


void Thumb2Assembler::Lsr(Register rd, Register rm, Register rn,
                          Condition cond, SetCc set_cc) {
  CheckCondition(cond);
  EmitShift(rd, rm, LSR, rn, cond, set_cc);
}


void Thumb2Assembler::Asr(Register rd, Register rm, Register rn,
                          Condition cond, SetCc set_cc) {
  CheckCondition(cond);
  EmitShift(rd, rm, ASR, rn, cond, set_cc);
}


void Thumb2Assembler::Ror(Register rd, Register rm, Register rn,
                          Condition cond, SetCc set_cc) {
  CheckCondition(cond);
  EmitShift(rd, rm, ROR, rn, cond, set_cc);
}


int32_t Thumb2Assembler::EncodeBranchOffset(int32_t offset, int32_t inst) {
  // The offset is off by 4 due to the way the ARM CPUs read PC.
  offset -= 4;
  offset >>= 1;

  uint32_t value = 0;
  // There are two different encodings depending on the value of bit 12.  In one case
  // intermediate values are calculated using the sign bit.
  if ((inst & B12) == B12) {
    // 25 bits of offset.
    uint32_t signbit = (offset >> 31) & 0x1;
    uint32_t i1 = (offset >> 22) & 0x1;
    uint32_t i2 = (offset >> 21) & 0x1;
    uint32_t imm10 = (offset >> 11) & 0x03ff;
    uint32_t imm11 = offset & 0x07ff;
    uint32_t j1 = (i1 ^ signbit) ? 0 : 1;
    uint32_t j2 = (i2 ^ signbit) ? 0 : 1;
    value = (signbit << 26) | (j1 << 13) | (j2 << 11) | (imm10 << 16) |
                      imm11;
    // Remove the offset from the current encoding.
    inst &= ~(0x3ff << 16 | 0x7ff);
  } else {
    uint32_t signbit = (offset >> 31) & 0x1;
    uint32_t imm6 = (offset >> 11) & 0x03f;
    uint32_t imm11 = offset & 0x07ff;
    uint32_t j1 = (offset >> 19) & 1;
    uint32_t j2 = (offset >> 17) & 1;
    value = (signbit << 26) | (j1 << 13) | (j2 << 11) | (imm6 << 16) |
        imm11;
    // Remove the offset from the current encoding.
    inst &= ~(0x3f << 16 | 0x7ff);
  }
  // Mask out offset bits in current instruction.
  inst &= ~(B26 | B13 | B11);
  inst |= value;
  return inst;
}


int Thumb2Assembler::DecodeBranchOffset(int32_t instr) {
  int32_t imm32;
  if ((instr & B12) == B12) {
    uint32_t S = (instr >> 26) & 1;
    uint32_t J2 = (instr >> 11) & 1;
    uint32_t J1 = (instr >> 13) & 1;
    uint32_t imm10 = (instr >> 16) & 0x3FF;
    uint32_t imm11 = instr & 0x7FF;

    uint32_t I1 = ~(J1 ^ S) & 1;
    uint32_t I2 = ~(J2 ^ S) & 1;
    imm32 = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1);
    imm32 = (imm32 << 8) >> 8;  // sign extend 24 bit immediate.
  } else {
    uint32_t S = (instr >> 26) & 1;
    uint32_t J2 = (instr >> 11) & 1;
    uint32_t J1 = (instr >> 13) & 1;
    uint32_t imm6 = (instr >> 16) & 0x3F;
    uint32_t imm11 = instr & 0x7FF;

    imm32 = (S << 20) | (J2 << 19) | (J1 << 18) | (imm6 << 12) | (imm11 << 1);
    imm32 = (imm32 << 11) >> 11;  // sign extend 21 bit immediate.
  }
  imm32 += 4;
  return imm32;
}

uint32_t Thumb2Assembler::GetAdjustedPosition(uint32_t old_position) {
  // We can reconstruct the adjustment by going through all the fixups from the beginning
  // up to the old_position. Since we expect AdjustedPosition() to be called in a loop
  // with increasing old_position, we can use the data from last AdjustedPosition() to
  // continue where we left off and the whole loop should be O(m+n) where m is the number
  // of positions to adjust and n is the number of fixups.
  if (old_position < last_old_position_) {
    last_position_adjustment_ = 0u;
    last_old_position_ = 0u;
    last_fixup_id_ = 0u;
  }
  while (last_fixup_id_ != fixups_.size()) {
    Fixup* fixup = GetFixup(last_fixup_id_);
    if (fixup->GetLocation() >= old_position + last_position_adjustment_) {
      break;
    }
    if (fixup->GetSize() != fixup->GetOriginalSize()) {
      last_position_adjustment_ += fixup->GetSizeInBytes() - fixup->GetOriginalSizeInBytes();
    }
     ++last_fixup_id_;
  }
  last_old_position_ = old_position;
  return old_position + last_position_adjustment_;
}

Literal* Thumb2Assembler::NewLiteral(size_t size, const uint8_t* data)  {
  DCHECK(size == 4u || size == 8u) << size;
  literals_.emplace_back(size, data);
  return &literals_.back();
}

void Thumb2Assembler::LoadLiteral(Register rt, Literal* literal)  {
  DCHECK_EQ(literal->GetSize(), 4u);
  DCHECK(!literal->GetLabel()->IsBound());
  bool use32bit = IsForced32Bit() || IsHighRegister(rt);
  uint32_t location = buffer_.Size();
  Fixup::Size size = use32bit ? Fixup::kLiteral4KiB : Fixup::kLiteral1KiB;
  FixupId fixup_id = AddFixup(Fixup::LoadNarrowLiteral(location, rt, size));
  Emit16(static_cast<uint16_t>(literal->GetLabel()->position_));
  literal->GetLabel()->LinkTo(fixup_id);
  if (use32bit) {
    Emit16(0);
  }
  DCHECK_EQ(location + GetFixup(fixup_id)->GetSizeInBytes(), buffer_.Size());
}

void Thumb2Assembler::LoadLiteral(Register rt, Register rt2, Literal* literal)  {
  DCHECK_EQ(literal->GetSize(), 8u);
  DCHECK(!literal->GetLabel()->IsBound());
  uint32_t location = buffer_.Size();
  FixupId fixup_id =
      AddFixup(Fixup::LoadWideLiteral(location, rt, rt2, Fixup::kLongOrFPLiteral1KiB));
  Emit16(static_cast<uint16_t>(literal->GetLabel()->position_));
  literal->GetLabel()->LinkTo(fixup_id);
  Emit16(0);
  DCHECK_EQ(location + GetFixup(fixup_id)->GetSizeInBytes(), buffer_.Size());
}

void Thumb2Assembler::LoadLiteral(SRegister sd, Literal* literal)  {
  DCHECK_EQ(literal->GetSize(), 4u);
  DCHECK(!literal->GetLabel()->IsBound());
  uint32_t location = buffer_.Size();
  FixupId fixup_id = AddFixup(Fixup::LoadSingleLiteral(location, sd, Fixup::kLongOrFPLiteral1KiB));
  Emit16(static_cast<uint16_t>(literal->GetLabel()->position_));
  literal->GetLabel()->LinkTo(fixup_id);
  Emit16(0);
  DCHECK_EQ(location + GetFixup(fixup_id)->GetSizeInBytes(), buffer_.Size());
}

void Thumb2Assembler::LoadLiteral(DRegister dd, Literal* literal) {
  DCHECK_EQ(literal->GetSize(), 8u);
  DCHECK(!literal->GetLabel()->IsBound());
  uint32_t location = buffer_.Size();
  FixupId fixup_id = AddFixup(Fixup::LoadDoubleLiteral(location, dd, Fixup::kLongOrFPLiteral1KiB));
  Emit16(static_cast<uint16_t>(literal->GetLabel()->position_));
  literal->GetLabel()->LinkTo(fixup_id);
  Emit16(0);
  DCHECK_EQ(location + GetFixup(fixup_id)->GetSizeInBytes(), buffer_.Size());
}


void Thumb2Assembler::AddConstant(Register rd, Register rn, int32_t value,
                                  Condition cond, SetCc set_cc) {
  if (value == 0 && set_cc != kCcSet) {
    if (rd != rn) {
      mov(rd, ShifterOperand(rn), cond);
    }
    return;
  }
  // We prefer to select the shorter code sequence rather than selecting add for
  // positive values and sub for negatives ones, which would slightly improve
  // the readability of generated code for some constants.
  ShifterOperand shifter_op;
  if (ShifterOperandCanHold(rd, rn, ADD, value, set_cc, &shifter_op)) {
    add(rd, rn, shifter_op, cond, set_cc);
  } else if (ShifterOperandCanHold(rd, rn, SUB, -value, set_cc, &shifter_op)) {
    sub(rd, rn, shifter_op, cond, set_cc);
  } else {
    CHECK(rn != IP);
    // If rd != rn, use rd as temp. This alows 16-bit ADD/SUB in more situations than using IP.
    Register temp = (rd != rn) ? rd : IP;
    if (ShifterOperandCanHold(temp, kNoRegister, MVN, ~value, kCcKeep, &shifter_op)) {
      mvn(temp, shifter_op, cond, kCcKeep);
      add(rd, rn, ShifterOperand(temp), cond, set_cc);
    } else if (ShifterOperandCanHold(temp, kNoRegister, MVN, ~(-value), kCcKeep, &shifter_op)) {
      mvn(temp, shifter_op, cond, kCcKeep);
      sub(rd, rn, ShifterOperand(temp), cond, set_cc);
    } else if (High16Bits(-value) == 0) {
      movw(temp, Low16Bits(-value), cond);
      sub(rd, rn, ShifterOperand(temp), cond, set_cc);
    } else {
      movw(temp, Low16Bits(value), cond);
      uint16_t value_high = High16Bits(value);
      if (value_high != 0) {
        movt(temp, value_high, cond);
      }
      add(rd, rn, ShifterOperand(temp), cond, set_cc);
    }
  }
}

void Thumb2Assembler::CmpConstant(Register rn, int32_t value, Condition cond) {
  // We prefer to select the shorter code sequence rather than using plain cmp and cmn
  // which would slightly improve the readability of generated code for some constants.
  ShifterOperand shifter_op;
  if (ShifterOperandCanHold(kNoRegister, rn, CMP, value, kCcSet, &shifter_op)) {
    cmp(rn, shifter_op, cond);
  } else if (ShifterOperandCanHold(kNoRegister, rn, CMN, -value, kCcSet, &shifter_op)) {
    cmn(rn, shifter_op, cond);
  } else {
    CHECK(rn != IP);
    if (ShifterOperandCanHold(IP, kNoRegister, MVN, ~value, kCcKeep, &shifter_op)) {
      mvn(IP, shifter_op, cond, kCcKeep);
      cmp(rn, ShifterOperand(IP), cond);
    } else if (ShifterOperandCanHold(IP, kNoRegister, MVN, ~(-value), kCcKeep, &shifter_op)) {
      mvn(IP, shifter_op, cond, kCcKeep);
      cmn(rn, ShifterOperand(IP), cond);
    } else if (High16Bits(-value) == 0) {
      movw(IP, Low16Bits(-value), cond);
      cmn(rn, ShifterOperand(IP), cond);
    } else {
      movw(IP, Low16Bits(value), cond);
      uint16_t value_high = High16Bits(value);
      if (value_high != 0) {
        movt(IP, value_high, cond);
      }
      cmp(rn, ShifterOperand(IP), cond);
    }
  }
}

void Thumb2Assembler::LoadImmediate(Register rd, int32_t value, Condition cond) {
  ShifterOperand shifter_op;
  if (ShifterOperandCanHold(rd, R0, MOV, value, &shifter_op)) {
    mov(rd, shifter_op, cond);
  } else if (ShifterOperandCanHold(rd, R0, MVN, ~value, &shifter_op)) {
    mvn(rd, shifter_op, cond);
  } else {
    movw(rd, Low16Bits(value), cond);
    uint16_t value_high = High16Bits(value);
    if (value_high != 0) {
      movt(rd, value_high, cond);
    }
  }
}

int32_t Thumb2Assembler::GetAllowedLoadOffsetBits(LoadOperandType type) {
  switch (type) {
    case kLoadSignedByte:
    case kLoadSignedHalfword:
    case kLoadUnsignedHalfword:
    case kLoadUnsignedByte:
    case kLoadWord:
      // We can encode imm12 offset.
      return 0xfffu;
    case kLoadSWord:
    case kLoadDWord:
    case kLoadWordPair:
      // We can encode imm8:'00' offset.
      return 0xff << 2;
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
}

int32_t Thumb2Assembler::GetAllowedStoreOffsetBits(StoreOperandType type) {
  switch (type) {
    case kStoreHalfword:
    case kStoreByte:
    case kStoreWord:
      // We can encode imm12 offset.
      return 0xfff;
    case kStoreSWord:
    case kStoreDWord:
    case kStoreWordPair:
      // We can encode imm8:'00' offset.
      return 0xff << 2;
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
}

bool Thumb2Assembler::CanSplitLoadStoreOffset(int32_t allowed_offset_bits,
                                              int32_t offset,
                                              /*out*/ int32_t* add_to_base,
                                              /*out*/ int32_t* offset_for_load_store) {
  int32_t other_bits = offset & ~allowed_offset_bits;
  if (ShifterOperandCanAlwaysHold(other_bits) || ShifterOperandCanAlwaysHold(-other_bits)) {
    *add_to_base = offset & ~allowed_offset_bits;
    *offset_for_load_store = offset & allowed_offset_bits;
    return true;
  }
  return false;
}

int32_t Thumb2Assembler::AdjustLoadStoreOffset(int32_t allowed_offset_bits,
                                               Register temp,
                                               Register base,
                                               int32_t offset,
                                               Condition cond) {
  DCHECK_NE(offset & ~allowed_offset_bits, 0);
  int32_t add_to_base, offset_for_load;
  if (CanSplitLoadStoreOffset(allowed_offset_bits, offset, &add_to_base, &offset_for_load)) {
    AddConstant(temp, base, add_to_base, cond, kCcKeep);
    return offset_for_load;
  } else {
    LoadImmediate(temp, offset, cond);
    add(temp, temp, ShifterOperand(base), cond, kCcKeep);
    return 0;
  }
}

// Implementation note: this method must emit at most one instruction when
// Address::CanHoldLoadOffsetThumb.
void Thumb2Assembler::LoadFromOffset(LoadOperandType type,
                                     Register reg,
                                     Register base,
                                     int32_t offset,
                                     Condition cond) {
  if (!Address::CanHoldLoadOffsetThumb(type, offset)) {
    CHECK_NE(base, IP);
    // Inlined AdjustLoadStoreOffset() allows us to pull a few more tricks.
    int32_t allowed_offset_bits = GetAllowedLoadOffsetBits(type);
    DCHECK_NE(offset & ~allowed_offset_bits, 0);
    int32_t add_to_base, offset_for_load;
    if (CanSplitLoadStoreOffset(allowed_offset_bits, offset, &add_to_base, &offset_for_load)) {
      // Use reg for the adjusted base. If it's low reg, we may end up using 16-bit load.
      AddConstant(reg, base, add_to_base, cond, kCcKeep);
      base = reg;
      offset = offset_for_load;
    } else {
      Register temp = (reg == base) ? IP : reg;
      LoadImmediate(temp, offset, cond);
      // TODO: Implement indexed load (not available for LDRD) and use it here to avoid the ADD.
      // Use reg for the adjusted base. If it's low reg, we may end up using 16-bit load.
      add(reg, reg, ShifterOperand((reg == base) ? IP : base), cond, kCcKeep);
      base = reg;
      offset = 0;
    }
  }
  DCHECK(Address::CanHoldLoadOffsetThumb(type, offset));
  switch (type) {
    case kLoadSignedByte:
      ldrsb(reg, Address(base, offset), cond);
      break;
    case kLoadUnsignedByte:
      ldrb(reg, Address(base, offset), cond);
      break;
    case kLoadSignedHalfword:
      ldrsh(reg, Address(base, offset), cond);
      break;
    case kLoadUnsignedHalfword:
      ldrh(reg, Address(base, offset), cond);
      break;
    case kLoadWord:
      ldr(reg, Address(base, offset), cond);
      break;
    case kLoadWordPair:
      ldrd(reg, Address(base, offset), cond);
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
}

// Implementation note: this method must emit at most one instruction when
// Address::CanHoldLoadOffsetThumb, as expected by JIT::GuardedLoadFromOffset.
void Thumb2Assembler::LoadSFromOffset(SRegister reg,
                                      Register base,
                                      int32_t offset,
                                      Condition cond) {
  if (!Address::CanHoldLoadOffsetThumb(kLoadSWord, offset)) {
    CHECK_NE(base, IP);
    offset = AdjustLoadStoreOffset(GetAllowedLoadOffsetBits(kLoadSWord), IP, base, offset, cond);
    base = IP;
  }
  DCHECK(Address::CanHoldLoadOffsetThumb(kLoadSWord, offset));
  vldrs(reg, Address(base, offset), cond);
}


// Implementation note: this method must emit at most one instruction when
// Address::CanHoldLoadOffsetThumb, as expected by JIT::GuardedLoadFromOffset.
void Thumb2Assembler::LoadDFromOffset(DRegister reg,
                                      Register base,
                                      int32_t offset,
                                      Condition cond) {
  if (!Address::CanHoldLoadOffsetThumb(kLoadDWord, offset)) {
    CHECK_NE(base, IP);
    offset = AdjustLoadStoreOffset(GetAllowedLoadOffsetBits(kLoadDWord), IP, base, offset, cond);
    base = IP;
  }
  DCHECK(Address::CanHoldLoadOffsetThumb(kLoadDWord, offset));
  vldrd(reg, Address(base, offset), cond);
}


// Implementation note: this method must emit at most one instruction when
// Address::CanHoldStoreOffsetThumb.
void Thumb2Assembler::StoreToOffset(StoreOperandType type,
                                    Register reg,
                                    Register base,
                                    int32_t offset,
                                    Condition cond) {
  Register tmp_reg = kNoRegister;
  if (!Address::CanHoldStoreOffsetThumb(type, offset)) {
    CHECK_NE(base, IP);
    if ((reg != IP) &&
        ((type != kStoreWordPair) || (reg + 1 != IP))) {
      tmp_reg = IP;
    } else {
      // Be careful not to use IP twice (for `reg` (or `reg` + 1 in
      // the case of a word-pair store) and `base`) to build the
      // Address object used by the store instruction(s) below.
      // Instead, save R5 on the stack (or R6 if R5 is already used by
      // `base`), use it as secondary temporary register, and restore
      // it after the store instruction has been emitted.
      tmp_reg = (base != R5) ? R5 : R6;
      Push(tmp_reg);
      if (base == SP) {
        offset += kRegisterSize;
      }
    }
    // TODO: Implement indexed store (not available for STRD), inline AdjustLoadStoreOffset()
    // and in the "unsplittable" path get rid of the "add" by using the store indexed instead.
    offset = AdjustLoadStoreOffset(GetAllowedStoreOffsetBits(type), tmp_reg, base, offset, cond);
    base = tmp_reg;
  }
  DCHECK(Address::CanHoldStoreOffsetThumb(type, offset));
  switch (type) {
    case kStoreByte:
      strb(reg, Address(base, offset), cond);
      break;
    case kStoreHalfword:
      strh(reg, Address(base, offset), cond);
      break;
    case kStoreWord:
      str(reg, Address(base, offset), cond);
      break;
    case kStoreWordPair:
      strd(reg, Address(base, offset), cond);
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
  if ((tmp_reg != kNoRegister) && (tmp_reg != IP)) {
    CHECK((tmp_reg == R5) || (tmp_reg == R6));
    Pop(tmp_reg);
  }
}


// Implementation note: this method must emit at most one instruction when
// Address::CanHoldStoreOffsetThumb, as expected by JIT::GuardedStoreToOffset.
void Thumb2Assembler::StoreSToOffset(SRegister reg,
                                     Register base,
                                     int32_t offset,
                                     Condition cond) {
  if (!Address::CanHoldStoreOffsetThumb(kStoreSWord, offset)) {
    CHECK_NE(base, IP);
    offset = AdjustLoadStoreOffset(GetAllowedStoreOffsetBits(kStoreSWord), IP, base, offset, cond);
    base = IP;
  }
  DCHECK(Address::CanHoldStoreOffsetThumb(kStoreSWord, offset));
  vstrs(reg, Address(base, offset), cond);
}


// Implementation note: this method must emit at most one instruction when
// Address::CanHoldStoreOffsetThumb, as expected by JIT::GuardedStoreSToOffset.
void Thumb2Assembler::StoreDToOffset(DRegister reg,
                                     Register base,
                                     int32_t offset,
                                     Condition cond) {
  if (!Address::CanHoldStoreOffsetThumb(kStoreDWord, offset)) {
    CHECK_NE(base, IP);
    offset = AdjustLoadStoreOffset(GetAllowedStoreOffsetBits(kStoreDWord), IP, base, offset, cond);
    base = IP;
  }
  DCHECK(Address::CanHoldStoreOffsetThumb(kStoreDWord, offset));
  vstrd(reg, Address(base, offset), cond);
}


void Thumb2Assembler::MemoryBarrier(ManagedRegister mscratch) {
  CHECK_EQ(mscratch.AsArm().AsCoreRegister(), R12);
  dmb(SY);
}


void Thumb2Assembler::dmb(DmbOptions flavor) {
  int32_t encoding = 0xf3bf8f50;  // dmb in T1 encoding.
  Emit32(encoding | flavor);
}


void Thumb2Assembler::CompareAndBranchIfZero(Register r, Label* label) {
  if (CanRelocateBranches() && IsLowRegister(r) && !label->IsBound()) {
    cbz(r, label);
  } else {
    cmp(r, ShifterOperand(0));
    b(label, EQ);
  }
}


void Thumb2Assembler::CompareAndBranchIfNonZero(Register r, Label* label) {
  if (CanRelocateBranches() && IsLowRegister(r) && !label->IsBound()) {
    cbnz(r, label);
  } else {
    cmp(r, ShifterOperand(0));
    b(label, NE);
  }
}

JumpTable* Thumb2Assembler::CreateJumpTable(std::vector<Label*>&& labels, Register base_reg) {
  jump_tables_.emplace_back(std::move(labels));
  JumpTable* table = &jump_tables_.back();
  DCHECK(!table->GetLabel()->IsBound());

  bool use32bit = IsForced32Bit() || IsHighRegister(base_reg);
  uint32_t location = buffer_.Size();
  Fixup::Size size = use32bit ? Fixup::kLiteralAddr4KiB : Fixup::kLiteralAddr1KiB;
  FixupId fixup_id = AddFixup(Fixup::LoadLiteralAddress(location, base_reg, size));
  Emit16(static_cast<uint16_t>(table->GetLabel()->position_));
  table->GetLabel()->LinkTo(fixup_id);
  if (use32bit) {
    Emit16(0);
  }
  DCHECK_EQ(location + GetFixup(fixup_id)->GetSizeInBytes(), buffer_.Size());

  return table;
}

void Thumb2Assembler::EmitJumpTableDispatch(JumpTable* jump_table, Register displacement_reg) {
  CHECK(!IsForced32Bit()) << "Forced 32-bit dispatch not implemented yet";
  // 32-bit ADD doesn't support PC as an input, so we need a two-instruction sequence:
  //   SUB ip, ip, #0
  //   ADD pc, ip, reg
  // TODO: Implement.

  // The anchor's position needs to be fixed up before we can compute offsets - so make it a tracked
  // label.
  BindTrackedLabel(jump_table->GetAnchorLabel());

  add(PC, PC, ShifterOperand(displacement_reg));
}

}  // namespace arm
}  // namespace art
