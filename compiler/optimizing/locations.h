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
#include "base/bit_vector.h"
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
  static constexpr bool kDiesAtEntry = true;

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
    kQuickParameter = 6,

    // Unallocated location represents a location that is not fixed and can be
    // allocated by a register allocator.  Each unallocated location has
    // a policy that specifies what kind of location is suitable. Payload
    // contains register allocation policy.
    kUnallocated = 7,
  };

  Location() : value_(kInvalid) {
    // Verify that non-tagged location kinds do not interfere with kConstantTag.
    COMPILE_ASSERT((kInvalid & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kUnallocated & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kStackSlot & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kDoubleStackSlot & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kRegister & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kQuickParameter & kLocationTagMask) != kConstant, TagError);
    COMPILE_ASSERT((kConstant & kLocationTagMask) == kConstant, TagError);

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
    return IsConstant() ? kConstant : KindField::Decode(value_);
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

class RegisterSet : public ValueObject {
 public:
  RegisterSet() : core_registers_(0), floating_point_registers_(0) {}

  void Add(Location loc) {
    // TODO: floating point registers.
    core_registers_ |= (1 << loc.reg().RegId());
  }

  bool ContainsCoreRegister(uint32_t id) {
    return Contains(core_registers_, id);
  }

  bool ContainsFloatingPointRegister(uint32_t id) {
    return Contains(floating_point_registers_, id);
  }

  static bool Contains(uint32_t register_set, uint32_t reg) {
    return (register_set & (1 << reg)) != 0;
  }

 private:
  uint32_t core_registers_;
  uint32_t floating_point_registers_;

  DISALLOW_COPY_AND_ASSIGN(RegisterSet);
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
  enum CallKind {
    kNoCall,
    kCallOnSlowPath,
    kCall
  };

  LocationSummary(HInstruction* instruction, CallKind call_kind = kNoCall);

  void SetInAt(uint32_t at, Location location, bool dies_at_entry = false) {
    dies_at_entry_.Put(at, dies_at_entry);
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

  void SetEnvironmentAt(uint32_t at, Location location) {
    environment_.Put(at, location);
  }

  Location GetEnvironmentAt(uint32_t at) const {
    return environment_.Get(at);
  }

  Location Out() const { return output_; }

  bool CanCall() const { return call_kind_ != kNoCall; }
  bool WillCall() const { return call_kind_ == kCall; }
  bool OnlyCallsOnSlowPath() const { return call_kind_ == kCallOnSlowPath; }
  bool NeedsSafepoint() const { return CanCall(); }

  void SetStackBit(uint32_t index) {
    stack_mask_->SetBit(index);
  }

  void ClearStackBit(uint32_t index) {
    stack_mask_->ClearBit(index);
  }

  void SetRegisterBit(uint32_t reg_id) {
    register_mask_ |= (1 << reg_id);
  }

  bool RegisterContainsObject(uint32_t reg_id) {
    return RegisterSet::Contains(register_mask_, reg_id);
  }

  void AddLiveRegister(Location location) {
    live_registers_.Add(location);
  }

  BitVector* GetStackMask() const {
    return stack_mask_;
  }

  RegisterSet* GetLiveRegisters() {
    return &live_registers_;
  }

  bool InputOverlapsWithOutputOrTemp(uint32_t input, bool is_environment) const {
    if (is_environment) return true;
    Location location = Out();
    if (input == 0 && location.IsUnallocated() && location.GetPolicy() == Location::kSameAsFirstInput) {
      return false;
    }
    if (dies_at_entry_.Get(input)) {
      return false;
    }
    return true;
  }

 private:
  GrowableArray<Location> inputs_;
  GrowableArray<Location> temps_;
  GrowableArray<Location> environment_;
  GrowableArray<bool> dies_at_entry_;
  Location output_;
  const CallKind call_kind_;

  // Mask of objects that live in the stack.
  BitVector* stack_mask_;

  // Mask of objects that live in register.
  uint32_t register_mask_;

  // Registers that are in use at this position.
  RegisterSet live_registers_;

  DISALLOW_COPY_AND_ASSIGN(LocationSummary);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_LOCATIONS_H_
