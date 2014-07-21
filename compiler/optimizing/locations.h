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

#ifndef ART_COMPILER_OPTIMIZING_LOCATIONS_H_
#define ART_COMPILER_OPTIMIZING_LOCATIONS_H_

#include "base/bit_field.h"
#include "utils/allocation.h"
#include "utils/growable_array.h"
#include "utils/managed_register.h"

namespace art {

class HConstant;
class HInstruction;

/**
 * A Location is an abstraction over the potential location
 * of an instruction. It could be in register or stack.
 */
class Location : public ValueObject {
 public:
  enum Kind {
    kInvalid = 0,
    kConstant = 1,
    kStackSlot = 2,  // Word size slot.
    kDoubleStackSlot = 3,  // 64bit stack slot.
    kRegister = 4,
    // On 32bits architectures, quick can pass a long where the
    // low bits are in the last parameter register, and the high
    // bits are in a stack slot. The kQuickParameter kind is for
    // handling this special case.
    kQuickParameter = 5,

    // Unallocated location represents a location that is not fixed and can be
    // allocated by a register allocator.  Each unallocated location has
    // a policy that specifies what kind of location is suitable. Payload
    // contains register allocation policy.
    kUnallocated = 6,
  };

  Location() : value_(kInvalid) {
    // Verify that non-tagged location kinds do not interfere with kConstantTag.
    COMPILE_ASSERT((kInvalid & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kUnallocated & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kStackSlot & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kDoubleStackSlot & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kRegister & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kConstant & kLocationTagMask) == kConstant, TagError);
    COMPILE_ASSERT((kQuickParameter & kLocationTagMask) == kConstant, TagError);

    DCHECK(!IsValid());
  }

  Location(const Location& other) : ValueObject(), value_(other.value_) {}

  Location& operator=(const Location& other) {
    value_ = other.value_;
    return *this;
  }

  bool IsConstant() const {
    return (value_ & kLocationTagMask) == kConstant;
  }

  static Location ConstantLocation(HConstant* constant) {
    DCHECK(constant != nullptr);
    return Location(kConstant | reinterpret_cast<uword>(constant));
  }

  HConstant* GetConstant() const {
    DCHECK(IsConstant());
    return reinterpret_cast<HConstant*>(value_ & ~kLocationTagMask);
  }

  bool IsValid() const {
    return value_ != kInvalid;
  }

  bool IsInvalid() const {
    return !IsValid();
  }

  // Empty location. Used if there the location should be ignored.
  static Location NoLocation() {
    return Location();
  }

  // Register locations.
  static Location RegisterLocation(ManagedRegister reg) {
    return Location(kRegister, reg.RegId());
  }

  bool IsRegister() const {
    return GetKind() == kRegister;
  }

  ManagedRegister reg() const {
    DCHECK(IsRegister());
    return static_cast<ManagedRegister>(GetPayload());
  }

  static uword EncodeStackIndex(intptr_t stack_index) {
    DCHECK(-kStackIndexBias <= stack_index);
    DCHECK(stack_index < kStackIndexBias);
    return static_cast<uword>(kStackIndexBias + stack_index);
  }

  static Location StackSlot(intptr_t stack_index) {
    uword payload = EncodeStackIndex(stack_index);
    Location loc(kStackSlot, payload);
    // Ensure that sign is preserved.
    DCHECK_EQ(loc.GetStackIndex(), stack_index);
    return loc;
  }

  bool IsStackSlot() const {
    return GetKind() == kStackSlot;
  }

  static Location DoubleStackSlot(intptr_t stack_index) {
    uword payload = EncodeStackIndex(stack_index);
    Location loc(kDoubleStackSlot, payload);
    // Ensure that sign is preserved.
    DCHECK_EQ(loc.GetStackIndex(), stack_index);
    return loc;
  }

  bool IsDoubleStackSlot() const {
    return GetKind() == kDoubleStackSlot;
  }

  intptr_t GetStackIndex() const {
    DCHECK(IsStackSlot() || IsDoubleStackSlot());
    // Decode stack index manually to preserve sign.
    return GetPayload() - kStackIndexBias;
  }

  intptr_t GetHighStackIndex(uintptr_t word_size) const {
    DCHECK(IsDoubleStackSlot());
    // Decode stack index manually to preserve sign.
    return GetPayload() - kStackIndexBias + word_size;
  }

  static Location QuickParameter(uint32_t parameter_index) {
    return Location(kQuickParameter, parameter_index);
  }

  uint32_t GetQuickParameterIndex() const {
    DCHECK(IsQuickParameter());
    return GetPayload();
  }

  bool IsQuickParameter() const {
    return GetKind() == kQuickParameter;
  }

  arm::ArmManagedRegister AsArm() const;
  x86::X86ManagedRegister AsX86() const;
  x86_64::X86_64ManagedRegister AsX86_64() const;

  Kind GetKind() const {
    return KindField::Decode(value_);
  }

  bool Equals(Location other) const {
    return value_ == other.value_;
  }

  const char* DebugString() const {
    switch (GetKind()) {
      case kInvalid: return "I";
      case kRegister: return "R";
      case kStackSlot: return "S";
      case kDoubleStackSlot: return "DS";
      case kQuickParameter: return "Q";
      case kUnallocated: return "U";
      case kConstant: return "C";
    }
    return "?";
  }

  // Unallocated locations.
  enum Policy {
    kAny,
    kRequiresRegister,
    kSameAsFirstInput,
  };

  bool IsUnallocated() const {
    return GetKind() == kUnallocated;
  }

  static Location UnallocatedLocation(Policy policy) {
    return Location(kUnallocated, PolicyField::Encode(policy));
  }

  // Any free register is suitable to replace this unallocated location.
  static Location Any() {
    return UnallocatedLocation(kAny);
  }

  static Location RequiresRegister() {
    return UnallocatedLocation(kRequiresRegister);
  }

  static Location RegisterOrConstant(HInstruction* instruction);

  // The location of the first input to the instruction will be
  // used to replace this unallocated location.
  static Location SameAsFirstInput() {
    return UnallocatedLocation(kSameAsFirstInput);
  }

  Policy GetPolicy() const {
    DCHECK(IsUnallocated());
    return PolicyField::Decode(GetPayload());
  }

  uword GetEncoding() const {
    return GetPayload();
  }

 private:
  // Number of bits required to encode Kind value.
  static constexpr uint32_t kBitsForKind = 4;
  static constexpr uint32_t kBitsForPayload = kWordSize * kBitsPerByte - kBitsForKind;
  static constexpr uword kLocationTagMask = 0x3;

  explicit Location(uword value) : value_(value) {}

  Location(Kind kind, uword payload)
      : value_(KindField::Encode(kind) | PayloadField::Encode(payload)) {}

  uword GetPayload() const {
    return PayloadField::Decode(value_);
  }

  typedef BitField<Kind, 0, kBitsForKind> KindField;
  typedef BitField<uword, kBitsForKind, kBitsForPayload> PayloadField;

  // Layout for kUnallocated locations payload.
  typedef BitField<Policy, 0, 3> PolicyField;

  // Layout for stack slots.
  static const intptr_t kStackIndexBias =
      static_cast<intptr_t>(1) << (kBitsForPayload - 1);

  // Location either contains kind and payload fields or a tagged handle for
  // a constant locations. Values of enumeration Kind are selected in such a
  // way that none of them can be interpreted as a kConstant tag.
  uword value_;
};

/**
 * The code generator computes LocationSummary for each instruction so that
 * the instruction itself knows what code to generate: where to find the inputs
 * and where to place the result.
 *
 * The intent is to have the code for generating the instruction independent of
 * register allocation. A register allocator just has to provide a LocationSummary.
 */
class LocationSummary : public ArenaObject {
 public:
  explicit LocationSummary(HInstruction* instruction);

  void SetInAt(uint32_t at, Location location) {
    inputs_.Put(at, location);
  }

  Location InAt(uint32_t at) const {
    return inputs_.Get(at);
  }

  size_t GetInputCount() const {
    return inputs_.Size();
  }

  void SetOut(Location location) {
    output_ = Location(location);
  }

  void AddTemp(Location location) {
    temps_.Add(location);
  }

  Location GetTemp(uint32_t at) const {
    return temps_.Get(at);
  }

  void SetTempAt(uint32_t at, Location location) {
    temps_.Put(at, location);
  }

  size_t GetTempCount() const {
    return temps_.Size();
  }

  Location Out() const { return output_; }

 private:
  GrowableArray<Location> inputs_;
  GrowableArray<Location> temps_;
  Location output_;

  DISALLOW_COPY_AND_ASSIGN(LocationSummary);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_LOCATIONS_H_
