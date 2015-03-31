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

#ifndef ART_RUNTIME_STACK_MAP_H_
#define ART_RUNTIME_STACK_MAP_H_

#include "base/bit_vector.h"
#include "memory_region.h"
#include "utils.h"

namespace art {

// Size of a frame slot, in bytes.  This constant is a signed value,
// to please the compiler in arithmetic operations involving int32_t
// (signed) values.
static ssize_t constexpr kFrameSlotSize = 4;

// Size of Dex virtual registers.
static size_t constexpr kVRegSize = 4;

class CodeInfo;

/**
 * Classes in the following file are wrapper on stack map information backed
 * by a MemoryRegion. As such they read and write to the region, they don't have
 * their own fields.
 */

/**
 * Inline information for a specific PC. The information is of the form:
 * [inlining_depth, [method_dex reference]+]
 */
class InlineInfo {
 public:
  explicit InlineInfo(MemoryRegion region) : region_(region) {}

  uint8_t GetDepth() const {
    return region_.LoadUnaligned<uint8_t>(kDepthOffset);
  }

  void SetDepth(uint8_t depth) {
    region_.StoreUnaligned<uint8_t>(kDepthOffset, depth);
  }

  uint32_t GetMethodReferenceIndexAtDepth(uint8_t depth) const {
    return region_.LoadUnaligned<uint32_t>(kFixedSize + depth * SingleEntrySize());
  }

  void SetMethodReferenceIndexAtDepth(uint8_t depth, uint32_t index) {
    region_.StoreUnaligned<uint32_t>(kFixedSize + depth * SingleEntrySize(), index);
  }

  static size_t SingleEntrySize() {
    return sizeof(uint32_t);
  }

 private:
  // TODO: Instead of plain types such as "uint8_t", introduce
  // typedefs (and document the memory layout of InlineInfo).
  static constexpr int kDepthOffset = 0;
  static constexpr int kFixedSize = kDepthOffset + sizeof(uint8_t);

  MemoryRegion region_;

  friend class CodeInfo;
  friend class StackMap;
  friend class StackMapStream;
};

// Dex register location container used by DexRegisterMap and StackMapStream.
class DexRegisterLocation {
 public:
  /*
   * The location kind used to populate the Dex register information in a
   * StackMapStream can either be:
   * - kNone: the register has no location yet, meaning it has not been set;
   * - kConstant: value holds the constant;
   * - kStack: value holds the stack offset;
   * - kRegister: value holds the physical register number;
   * - kFpuRegister: value holds the physical register number.
   *
   * In addition, DexRegisterMap also uses these values:
   * - kInStackLargeOffset: value holds a "large" stack offset (greater than
   *   128 bytes);
   * - kConstantLargeValue: value holds a "large" constant (lower than or
   *   equal to -16, or greater than 16).
   */
  enum class Kind : uint8_t {
    // Short location kinds, for entries fitting on one byte (3 bits
    // for the kind, 5 bits for the value) in a DexRegisterMap.
    kNone = 0,                // 0b000
    kInStack = 1,             // 0b001
    kInRegister = 2,          // 0b010
    kInFpuRegister = 3,       // 0b011
    kConstant = 4,            // 0b100

    // Large location kinds, requiring a 5-byte encoding (1 byte for the
    // kind, 4 bytes for the value).

    // Stack location at a large offset, meaning that the offset value
    // divided by the stack frame slot size (4 bytes) cannot fit on a
    // 5-bit unsigned integer (i.e., this offset value is greater than
    // or equal to 2^5 * 4 = 128 bytes).
    kInStackLargeOffset = 5,  // 0b101

    // Large constant, that cannot fit on a 5-bit signed integer (i.e.,
    // lower than -2^(5-1) = -16, or greater than or equal to
    // 2^(5-1) - 1 = 15).
    kConstantLargeValue = 6,  // 0b110

    kLastLocationKind = kConstantLargeValue
  };

  static_assert(
      sizeof(Kind) == 1u,
      "art::DexRegisterLocation::Kind has a size different from one byte.");

  static const char* PrettyDescriptor(Kind kind) {
    switch (kind) {
      case Kind::kNone:
        return "none";
      case Kind::kInStack:
        return "in stack";
      case Kind::kInRegister:
        return "in register";
      case Kind::kInFpuRegister:
        return "in fpu register";
      case Kind::kConstant:
        return "as constant";
      case Kind::kInStackLargeOffset:
        return "in stack (large offset)";
      case Kind::kConstantLargeValue:
        return "as constant (large value)";
      default:
        UNREACHABLE();
    }
  }

  static bool IsShortLocationKind(Kind kind) {
    switch (kind) {
      case Kind::kNone:
      case Kind::kInStack:
      case Kind::kInRegister:
      case Kind::kInFpuRegister:
      case Kind::kConstant:
        return true;

      case Kind::kInStackLargeOffset:
      case Kind::kConstantLargeValue:
        return false;

      default:
        UNREACHABLE();
    }
  }

  // Convert `kind` to a "surface" kind, i.e. one that doesn't include
  // any value with a "large" qualifier.
  // TODO: Introduce another enum type for the surface kind?
  static Kind ConvertToSurfaceKind(Kind kind) {
    switch (kind) {
      case Kind::kNone:
      case Kind::kInStack:
      case Kind::kInRegister:
      case Kind::kInFpuRegister:
      case Kind::kConstant:
        return kind;

      case Kind::kInStackLargeOffset:
        return Kind::kInStack;

      case Kind::kConstantLargeValue:
        return Kind::kConstant;

      default:
        UNREACHABLE();
    }
  }

  DexRegisterLocation(Kind kind, int32_t value)
      : kind_(kind), value_(value) {}

  static DexRegisterLocation None() {
    return DexRegisterLocation(Kind::kNone, 0);
  }

  // Get the "surface" kind of the location, i.e., the one that doesn't
  // include any value with a "large" qualifier.
  Kind GetKind() const {
    return ConvertToSurfaceKind(kind_);
  }

  // Get the value of the location.
  int32_t GetValue() const { return value_; }

  // Get the actual kind of the location.
  Kind GetInternalKind() const { return kind_; }

  bool operator==(DexRegisterLocation other) const {
    return kind_ == other.kind_ && value_ == other.value_;
  }

  bool operator!=(DexRegisterLocation other) const {
    return !(*this == other);
  }

 private:
  Kind kind_;
  int32_t value_;
};

/**
 * Information on dex register values for a specific PC. The information is
 * of the form:
 * [live_bit_mask, DexRegisterLocation+].
 * DexRegisterLocations are either 1- or 5-byte wide (see art::DexRegisterLocation::Kind).
 */
class DexRegisterMap {
 public:
  explicit DexRegisterMap(MemoryRegion region) : region_(region) {}

  // Short (compressed) location, fitting on one byte.
  typedef uint8_t ShortLocation;

  static size_t LiveBitMaskSize(uint16_t number_of_dex_registers) {
    return RoundUp(number_of_dex_registers, kBitsPerByte) / kBitsPerByte;
  }

  void SetLiveBitMask(size_t offset,
                      uint16_t number_of_dex_registers,
                      const BitVector& live_dex_registers_mask) {
    for (uint16_t i = 0; i < number_of_dex_registers; i++) {
      region_.StoreBit(offset + i, live_dex_registers_mask.IsBitSet(i));
    }
  }

  void SetRegisterInfo(size_t offset, const DexRegisterLocation& dex_register_location) {
    DexRegisterLocation::Kind kind = ComputeCompressedKind(dex_register_location);
    int32_t value = dex_register_location.GetValue();
    if (DexRegisterLocation::IsShortLocationKind(kind)) {
      // Short location.  Compress the kind and the value as a single byte.
      if (kind == DexRegisterLocation::Kind::kInStack) {
        // Instead of storing stack offsets expressed in bytes for
        // short stack locations, store slot offsets.  A stack offset
        // is a multiple of 4 (kFrameSlotSize).  This means that by
        // dividing it by 4, we can fit values from the [0, 128)
        // interval in a short stack location, and not just values
        // from the [0, 32) interval.
        DCHECK_EQ(value % kFrameSlotSize, 0);
        value /= kFrameSlotSize;
      }
      DCHECK(IsUint<kValueBits>(value)) << value;
      region_.StoreUnaligned<ShortLocation>(offset, MakeShortLocation(kind, value));
    } else {
      // Large location.  Write the location on one byte and the value
      // on 4 bytes.
      DCHECK(!IsUint<kValueBits>(value)) << value;
      if (kind == DexRegisterLocation::Kind::kInStackLargeOffset) {
        // Also divide large stack offsets by 4 for the sake of consistency.
        DCHECK_EQ(value % kFrameSlotSize, 0);
        value /= kFrameSlotSize;
      }
      // Data can be unaligned as the written Dex register locations can
      // either be 1-byte or 5-byte wide.  Use
      // art::MemoryRegion::StoreUnaligned instead of
      // art::MemoryRegion::Store to prevent unligned word accesses on ARM.
      region_.StoreUnaligned<DexRegisterLocation::Kind>(offset, kind);
      region_.StoreUnaligned<int32_t>(offset + sizeof(DexRegisterLocation::Kind), value);
    }
  }

  bool IsDexRegisterLive(uint16_t dex_register_index) const {
    size_t offset = kFixedSize;
    return region_.LoadBit(offset + dex_register_index);
  }

  static constexpr size_t kNoDexRegisterLocationOffset = -1;

  static size_t GetDexRegisterMapLocationsOffset(uint16_t number_of_dex_registers) {
    return kLiveBitMaskOffset + LiveBitMaskSize(number_of_dex_registers);
  }

  // Find the offset of the Dex register location number `dex_register_index`.
  size_t FindLocationOffset(uint16_t dex_register_index, uint16_t number_of_dex_registers) const {
    if (!IsDexRegisterLive(dex_register_index)) return kNoDexRegisterLocationOffset;
    size_t offset = GetDexRegisterMapLocationsOffset(number_of_dex_registers);
    // Skip the first `dex_register_index - 1` entries.
    for (uint16_t i = 0; i < dex_register_index; ++i) {
      if (IsDexRegisterLive(i)) {
        // Read the first next byte and inspect its first 3 bits to decide
        // whether it is a short or a large location.
        DexRegisterLocation::Kind kind = ExtractKindAtOffset(offset);
        if (DexRegisterLocation::IsShortLocationKind(kind)) {
          // Short location.  Skip the current byte.
          offset += SingleShortEntrySize();
        } else {
          // Large location.  Skip the 5 next bytes.
          offset += SingleLargeEntrySize();
        }
      }
    }
    return offset;
  }

  // Get the surface kind.
  DexRegisterLocation::Kind GetLocationKind(uint16_t dex_register_index,
                                            uint16_t number_of_dex_registers) const {
    return IsDexRegisterLive(dex_register_index)
        ? DexRegisterLocation::ConvertToSurfaceKind(
              GetLocationInternalKind(dex_register_index, number_of_dex_registers))
        : DexRegisterLocation::Kind::kNone;
  }

  // Get the internal kind.
  DexRegisterLocation::Kind GetLocationInternalKind(uint16_t dex_register_index,
                                                    uint16_t number_of_dex_registers) const {
    return IsDexRegisterLive(dex_register_index)
        ? ExtractKindAtOffset(FindLocationOffset(dex_register_index, number_of_dex_registers))
        : DexRegisterLocation::Kind::kNone;
  }

  // TODO: Rename as GetDexRegisterLocation?
  DexRegisterLocation GetLocationKindAndValue(uint16_t dex_register_index,
                                              uint16_t number_of_dex_registers) const {
    if (!IsDexRegisterLive(dex_register_index)) {
      return DexRegisterLocation::None();
    }
    size_t offset = FindLocationOffset(dex_register_index, number_of_dex_registers);
    // Read the first byte and inspect its first 3 bits to get the location.
    ShortLocation first_byte = region_.LoadUnaligned<ShortLocation>(offset);
    DexRegisterLocation::Kind kind = ExtractKindFromShortLocation(first_byte);
    if (DexRegisterLocation::IsShortLocationKind(kind)) {
      // Short location.  Extract the value from the remaining 5 bits.
      int32_t value = ExtractValueFromShortLocation(first_byte);
      if (kind == DexRegisterLocation::Kind::kInStack) {
        // Convert the stack slot (short) offset to a byte offset value.
        value *= kFrameSlotSize;
      }
      return DexRegisterLocation(kind, value);
    } else {
      // Large location.  Read the four next bytes to get the value.
      int32_t value = region_.LoadUnaligned<int32_t>(offset + sizeof(DexRegisterLocation::Kind));
      if (kind == DexRegisterLocation::Kind::kInStackLargeOffset) {
        // Convert the stack slot (large) offset to a byte offset value.
        value *= kFrameSlotSize;
      }
      return DexRegisterLocation(kind, value);
    }
  }

  int32_t GetStackOffsetInBytes(uint16_t dex_register_index,
                                uint16_t number_of_dex_registers) const {
    DexRegisterLocation location =
        GetLocationKindAndValue(dex_register_index, number_of_dex_registers);
    DCHECK(location.GetKind() == DexRegisterLocation::Kind::kInStack);
    // GetLocationKindAndValue returns the offset in bytes.
    return location.GetValue();
  }

  int32_t GetConstant(uint16_t dex_register_index, uint16_t number_of_dex_registers) const {
    DexRegisterLocation location =
        GetLocationKindAndValue(dex_register_index, number_of_dex_registers);
    DCHECK(location.GetKind() == DexRegisterLocation::Kind::kConstant);
    return location.GetValue();
  }

  int32_t GetMachineRegister(uint16_t dex_register_index, uint16_t number_of_dex_registers) const {
    DexRegisterLocation location =
        GetLocationKindAndValue(dex_register_index, number_of_dex_registers);
    DCHECK(location.GetInternalKind() == DexRegisterLocation::Kind::kInRegister
           || location.GetInternalKind() == DexRegisterLocation::Kind::kInFpuRegister)
        << DexRegisterLocation::PrettyDescriptor(location.GetInternalKind());
    return location.GetValue();
  }

  // Compute the compressed kind of `location`.
  static DexRegisterLocation::Kind ComputeCompressedKind(const DexRegisterLocation& location) {
    switch (location.GetInternalKind()) {
      case DexRegisterLocation::Kind::kNone:
        DCHECK_EQ(location.GetValue(), 0);
        return DexRegisterLocation::Kind::kNone;

      case DexRegisterLocation::Kind::kInRegister:
        DCHECK_GE(location.GetValue(), 0);
        DCHECK_LT(location.GetValue(), 1 << DexRegisterMap::kValueBits);
        return DexRegisterLocation::Kind::kInRegister;

      case DexRegisterLocation::Kind::kInFpuRegister:
        DCHECK_GE(location.GetValue(), 0);
        DCHECK_LT(location.GetValue(), 1 << DexRegisterMap::kValueBits);
        return DexRegisterLocation::Kind::kInFpuRegister;

      case DexRegisterLocation::Kind::kInStack:
        DCHECK_EQ(location.GetValue() % kFrameSlotSize, 0);
        return IsUint<DexRegisterMap::kValueBits>(location.GetValue() / kFrameSlotSize)
            ? DexRegisterLocation::Kind::kInStack
            : DexRegisterLocation::Kind::kInStackLargeOffset;

      case DexRegisterLocation::Kind::kConstant:
        return IsUint<DexRegisterMap::kValueBits>(location.GetValue())
            ? DexRegisterLocation::Kind::kConstant
            : DexRegisterLocation::Kind::kConstantLargeValue;

      default:
        LOG(FATAL) << "Unexpected location kind"
                   << DexRegisterLocation::PrettyDescriptor(location.GetInternalKind());
        UNREACHABLE();
    }
  }

  // Can `location` be turned into a short location?
  static bool CanBeEncodedAsShortLocation(const DexRegisterLocation& location) {
    switch (location.GetInternalKind()) {
      case DexRegisterLocation::Kind::kNone:
      case DexRegisterLocation::Kind::kInRegister:
      case DexRegisterLocation::Kind::kInFpuRegister:
        return true;

      case DexRegisterLocation::Kind::kInStack:
        DCHECK_EQ(location.GetValue() % kFrameSlotSize, 0);
        return IsUint<kValueBits>(location.GetValue() / kFrameSlotSize);

      case DexRegisterLocation::Kind::kConstant:
        return IsUint<kValueBits>(location.GetValue());

      default:
        UNREACHABLE();
    }
  }

  static size_t EntrySize(const DexRegisterLocation& location) {
    return CanBeEncodedAsShortLocation(location)
        ? DexRegisterMap::SingleShortEntrySize()
        : DexRegisterMap::SingleLargeEntrySize();
  }

  static size_t SingleShortEntrySize() {
    return sizeof(ShortLocation);
  }

  static size_t SingleLargeEntrySize() {
    return sizeof(DexRegisterLocation::Kind) + sizeof(int32_t);
  }

  size_t Size() const {
    return region_.size();
  }

  static constexpr int kLiveBitMaskOffset = 0;
  static constexpr int kFixedSize = kLiveBitMaskOffset;

 private:
  // Width of the kind "field" in a short location, in bits.
  static constexpr size_t kKindBits = 3;
  // Width of the value "field" in a short location, in bits.
  static constexpr size_t kValueBits = 5;

  static constexpr uint8_t kKindMask = (1 << kKindBits) - 1;
  static constexpr int32_t kValueMask = (1 << kValueBits) - 1;
  static constexpr size_t kKindOffset = 0;
  static constexpr size_t kValueOffset = kKindBits;

  static ShortLocation MakeShortLocation(DexRegisterLocation::Kind kind, int32_t value) {
    DCHECK(IsUint<kKindBits>(static_cast<uint8_t>(kind))) << static_cast<uint8_t>(kind);
    DCHECK(IsUint<kValueBits>(value)) << value;
    return (static_cast<uint8_t>(kind) & kKindMask) << kKindOffset
        | (value & kValueMask) << kValueOffset;
  }

  static DexRegisterLocation::Kind ExtractKindFromShortLocation(ShortLocation location) {
    uint8_t kind = (location >> kKindOffset) & kKindMask;
    DCHECK_LE(kind, static_cast<uint8_t>(DexRegisterLocation::Kind::kLastLocationKind));
    // We do not encode kNone locations in the stack map.
    DCHECK_NE(kind, static_cast<uint8_t>(DexRegisterLocation::Kind::kNone));
    return static_cast<DexRegisterLocation::Kind>(kind);
  }

  static int32_t ExtractValueFromShortLocation(ShortLocation location) {
    return (location >> kValueOffset) & kValueMask;
  }

  // Extract a location kind from the byte at position `offset`.
  DexRegisterLocation::Kind ExtractKindAtOffset(size_t offset) const {
    ShortLocation first_byte = region_.LoadUnaligned<ShortLocation>(offset);
    return ExtractKindFromShortLocation(first_byte);
  }

  MemoryRegion region_;

  friend class CodeInfo;
  friend class StackMapStream;
};

/**
 * A Stack Map holds compilation information for a specific PC necessary for:
 * - Mapping it to a dex PC,
 * - Knowing which stack entries are objects,
 * - Knowing which registers hold objects,
 * - Knowing the inlining information,
 * - Knowing the values of dex registers.
 *
 * The information is of the form:
 * [dex_pc, native_pc_offset, dex_register_map_offset, inlining_info_offset, register_mask, stack_mask].
 *
 * Note that register_mask is fixed size, but stack_mask is variable size, depending on the
 * stack size of a method.
 */
class StackMap {
 public:
  explicit StackMap(MemoryRegion region) : region_(region) {}

  uint32_t GetDexPc(const CodeInfo& info) const;

  void SetDexPc(const CodeInfo& info, uint32_t dex_pc);

  uint32_t GetNativePcOffset(const CodeInfo& info) const;

  void SetNativePcOffset(const CodeInfo& info, uint32_t native_pc_offset);

  uint32_t GetDexRegisterMapOffset(const CodeInfo& info) const;

  void SetDexRegisterMapOffset(const CodeInfo& info, uint32_t offset);

  uint32_t GetInlineDescriptorOffset(const CodeInfo& info) const;

  void SetInlineDescriptorOffset(const CodeInfo& info, uint32_t offset);

  uint32_t GetRegisterMask(const CodeInfo& info) const;

  void SetRegisterMask(const CodeInfo& info, uint32_t mask);

  MemoryRegion GetStackMask(const CodeInfo& info) const;

  void SetStackMask(const CodeInfo& info, const BitVector& sp_map) {
    MemoryRegion region = GetStackMask(info);
    for (size_t i = 0; i < region.size_in_bits(); i++) {
      region.StoreBit(i, sp_map.IsBitSet(i));
    }
  }

  bool HasDexRegisterMap(const CodeInfo& info) const {
    return GetDexRegisterMapOffset(info) != kNoDexRegisterMap;
  }

  bool HasInlineInfo(const CodeInfo& info) const {
    return GetInlineDescriptorOffset(info) != kNoInlineInfo;
  }

  bool Equals(const StackMap& other) const {
    return region_.pointer() == other.region_.pointer()
       && region_.size() == other.region_.size();
  }

  static size_t ComputeStackMapSize(size_t stack_mask_size,
                                    bool has_inline_info,
                                    bool is_small_inline_info,
                                    bool is_small_dex_map,
                                    bool is_small_dex_pc,
                                    bool is_small_native_pc);

  static size_t ComputeStackMapSize(size_t stack_mask_size,
                                    size_t inline_info_size,
                                    size_t dex_register_map_size,
                                    size_t dex_pc_max,
                                    size_t native_pc_max);

  // TODO: Revisit this abstraction if we allow 3 bytes encoding.
  typedef uint8_t kSmallEncoding;
  typedef uint32_t kLargeEncoding;
  static constexpr size_t kBytesForSmallEncoding = sizeof(kSmallEncoding);
  static constexpr size_t kBitsForSmallEncoding = kBitsPerByte * kBytesForSmallEncoding;
  static constexpr size_t kBytesForLargeEncoding = sizeof(kLargeEncoding);
  static constexpr size_t kBitsForLargeEncoding = kBitsPerByte * kBytesForLargeEncoding;

  // Special (invalid) offset for the DexRegisterMapOffset field meaning
  // that there is no Dex register map for this stack map.
  static constexpr uint32_t kNoDexRegisterMap = -1;
  static constexpr uint32_t kNoDexRegisterMapSmallEncoding =
      std::numeric_limits<kSmallEncoding>::max();

  // Special (invalid) offset for the InlineDescriptorOffset field meaning
  // that there is no inline info for this stack map.
  static constexpr uint32_t kNoInlineInfo = -1;
  static constexpr uint32_t kNoInlineInfoSmallEncoding =
    std::numeric_limits<kSmallEncoding>::max();

  // Returns the number of bytes needed for an entry in the StackMap.
  static size_t NumberOfBytesForEntry(bool small_encoding) {
    return small_encoding ? kBytesForSmallEncoding : kBytesForLargeEncoding;
  }

 private:
  // TODO: Instead of plain types such as "uint32_t", introduce
  // typedefs (and document the memory layout of StackMap).
  static constexpr int kRegisterMaskOffset = 0;
  static constexpr int kFixedSize = kRegisterMaskOffset + sizeof(uint32_t);
  static constexpr int kStackMaskOffset = kFixedSize;

  MemoryRegion region_;

  friend class CodeInfo;
  friend class StackMapStream;
};


/**
 * Wrapper around all compiler information collected for a method.
 * The information is of the form:
 * [overall_size, number_of_stack_maps, stack_mask_size, StackMap+, DexRegisterInfo+, InlineInfo*].
 */
class CodeInfo {
 public:
  explicit CodeInfo(MemoryRegion region) : region_(region) {}

  explicit CodeInfo(const void* data) {
    uint32_t size = reinterpret_cast<const uint32_t*>(data)[0];
    region_ = MemoryRegion(const_cast<void*>(data), size);
  }

  void SetEncoding(size_t inline_info_size,
                   size_t dex_register_map_size,
                   size_t dex_pc_max,
                   size_t native_pc_max) {
    if (inline_info_size != 0) {
      region_.StoreBit(kHasInlineInfoBitOffset, 1);
      region_.StoreBit(kHasSmallInlineInfoBitOffset, IsUint<StackMap::kBitsForSmallEncoding>(
          // + 1 to also encode kNoInlineInfo: if an inline info offset
          // is at 0xFF, we want to overflow to a larger encoding, because it will
          // conflict with kNoInlineInfo.
          // The offset is relative to the dex register map. TODO: Change this.
          inline_info_size + dex_register_map_size + 1));
    } else {
      region_.StoreBit(kHasInlineInfoBitOffset, 0);
      region_.StoreBit(kHasSmallInlineInfoBitOffset, 0);
    }
    region_.StoreBit(kHasSmallDexRegisterMapBitOffset,
                     // + 1 to also encode kNoDexRegisterMap: if a dex register map offset
                     // is at 0xFF, we want to overflow to a larger encoding, because it will
                     // conflict with kNoDexRegisterMap.
                     IsUint<StackMap::kBitsForSmallEncoding>(dex_register_map_size + 1));
    region_.StoreBit(kHasSmallDexPcBitOffset, IsUint<StackMap::kBitsForSmallEncoding>(dex_pc_max));
    region_.StoreBit(kHasSmallNativePcBitOffset,
                     IsUint<StackMap::kBitsForSmallEncoding>(native_pc_max));
  }

  bool HasInlineInfo() const {
    return region_.LoadBit(kHasInlineInfoBitOffset);
  }

  bool HasSmallInlineInfo() const {
    return region_.LoadBit(kHasSmallInlineInfoBitOffset);
  }

  bool HasSmallDexRegisterMap() const {
    return region_.LoadBit(kHasSmallDexRegisterMapBitOffset);
  }

  bool HasSmallNativePc() const {
    return region_.LoadBit(kHasSmallNativePcBitOffset);
  }

  bool HasSmallDexPc() const {
    return region_.LoadBit(kHasSmallDexPcBitOffset);
  }

  size_t ComputeStackMapRegisterMaskOffset() const {
    return StackMap::kRegisterMaskOffset;
  }

  size_t ComputeStackMapStackMaskOffset() const {
    return StackMap::kStackMaskOffset;
  }

  size_t ComputeStackMapDexPcOffset() const {
    return ComputeStackMapStackMaskOffset() + GetStackMaskSize();
  }

  size_t ComputeStackMapNativePcOffset() const {
    return ComputeStackMapDexPcOffset()
        + (HasSmallDexPc() ? sizeof(uint8_t) : sizeof(uint32_t));
  }

  size_t ComputeStackMapDexRegisterMapOffset() const {
    return ComputeStackMapNativePcOffset()
        + (HasSmallNativePc() ? sizeof(uint8_t) : sizeof(uint32_t));
  }

  size_t ComputeStackMapInlineInfoOffset() const {
    CHECK(HasInlineInfo());
    return ComputeStackMapDexRegisterMapOffset()
        + (HasSmallDexRegisterMap() ? sizeof(uint8_t) : sizeof(uint32_t));
  }

  StackMap GetStackMapAt(size_t i) const {
    size_t size = StackMapSize();
    return StackMap(GetStackMaps().Subregion(i * size, size));
  }

  uint32_t GetOverallSize() const {
    return region_.LoadUnaligned<uint32_t>(kOverallSizeOffset);
  }

  void SetOverallSize(uint32_t size) {
    region_.StoreUnaligned<uint32_t>(kOverallSizeOffset, size);
  }

  uint32_t GetStackMaskSize() const {
    return region_.LoadUnaligned<uint32_t>(kStackMaskSizeOffset);
  }

  void SetStackMaskSize(uint32_t size) {
    region_.StoreUnaligned<uint32_t>(kStackMaskSizeOffset, size);
  }

  size_t GetNumberOfStackMaps() const {
    return region_.LoadUnaligned<uint32_t>(kNumberOfStackMapsOffset);
  }

  void SetNumberOfStackMaps(uint32_t number_of_stack_maps) {
    region_.StoreUnaligned<uint32_t>(kNumberOfStackMapsOffset, number_of_stack_maps);
  }

  // Get the size of one stack map of this CodeInfo object, in bytes.
  // All stack maps of a CodeInfo have the same size.
  size_t StackMapSize() const {
    return StackMap::ComputeStackMapSize(GetStackMaskSize(),
                                         HasInlineInfo(),
                                         HasSmallInlineInfo(),
                                         HasSmallDexRegisterMap(),
                                         HasSmallDexPc(),
                                         HasSmallNativePc());
  }

  // Get the size all the stack maps of this CodeInfo object, in bytes.
  size_t StackMapsSize() const {
    return StackMapSize() * GetNumberOfStackMaps();
  }

  size_t GetDexRegisterMapsOffset() const {
    return CodeInfo::kFixedSize + StackMapsSize();
  }

  uint32_t GetStackMapsOffset() const {
    return kFixedSize;
  }

  DexRegisterMap GetDexRegisterMapOf(StackMap stack_map, uint32_t number_of_dex_registers) const {
    DCHECK(stack_map.HasDexRegisterMap(*this));
    uint32_t offset = stack_map.GetDexRegisterMapOffset(*this) + GetDexRegisterMapsOffset();
    size_t size = ComputeDexRegisterMapSize(offset, number_of_dex_registers);
    return DexRegisterMap(region_.Subregion(offset, size));
  }

  InlineInfo GetInlineInfoOf(StackMap stack_map) const {
    DCHECK(stack_map.HasInlineInfo(*this));
    uint32_t offset = stack_map.GetInlineDescriptorOffset(*this) + GetDexRegisterMapsOffset();
    uint8_t depth = region_.LoadUnaligned<uint8_t>(offset);
    return InlineInfo(region_.Subregion(offset,
        InlineInfo::kFixedSize + depth * InlineInfo::SingleEntrySize()));
  }

  StackMap GetStackMapForDexPc(uint32_t dex_pc) const {
    for (size_t i = 0, e = GetNumberOfStackMaps(); i < e; ++i) {
      StackMap stack_map = GetStackMapAt(i);
      if (stack_map.GetDexPc(*this) == dex_pc) {
        return stack_map;
      }
    }
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }

  StackMap GetStackMapForNativePcOffset(uint32_t native_pc_offset) const {
    // TODO: stack maps are sorted by native pc, we can do a binary search.
    for (size_t i = 0, e = GetNumberOfStackMaps(); i < e; ++i) {
      StackMap stack_map = GetStackMapAt(i);
      if (stack_map.GetNativePcOffset(*this) == native_pc_offset) {
        return stack_map;
      }
    }
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }

  void Dump(std::ostream& os, uint16_t number_of_dex_registers) const;
  void DumpStackMapHeader(std::ostream& os, size_t stack_map_num) const;

 private:
  // TODO: Instead of plain types such as "uint32_t", introduce
  // typedefs (and document the memory layout of CodeInfo).
  static constexpr int kOverallSizeOffset = 0;
  static constexpr int kEncodingInfoOffset = kOverallSizeOffset + sizeof(uint32_t);
  static constexpr int kNumberOfStackMapsOffset = kEncodingInfoOffset + sizeof(uint8_t);
  static constexpr int kStackMaskSizeOffset = kNumberOfStackMapsOffset + sizeof(uint32_t);
  static constexpr int kFixedSize = kStackMaskSizeOffset + sizeof(uint32_t);

  static constexpr int kHasInlineInfoBitOffset = (kEncodingInfoOffset * kBitsPerByte);
  static constexpr int kHasSmallInlineInfoBitOffset = kHasInlineInfoBitOffset + 1;
  static constexpr int kHasSmallDexRegisterMapBitOffset = kHasSmallInlineInfoBitOffset + 1;
  static constexpr int kHasSmallDexPcBitOffset = kHasSmallDexRegisterMapBitOffset + 1;
  static constexpr int kHasSmallNativePcBitOffset = kHasSmallDexPcBitOffset + 1;

  MemoryRegion GetStackMaps() const {
    return region_.size() == 0
        ? MemoryRegion()
        : region_.Subregion(kFixedSize, StackMapsSize());
  }

  // Compute the size of a Dex register map starting at offset `origin` in
  // `region_` and containing `number_of_dex_registers` locations.
  size_t ComputeDexRegisterMapSize(uint32_t origin, uint32_t number_of_dex_registers) const {
    // TODO: Ideally, we would like to use art::DexRegisterMap::Size or
    // art::DexRegisterMap::FindLocationOffset, but the DexRegisterMap is not
    // yet built.  Try to factor common code.
    size_t offset =
        origin + DexRegisterMap::GetDexRegisterMapLocationsOffset(number_of_dex_registers);

    // Create a temporary DexRegisterMap to be able to call DexRegisterMap.IsDexRegisterLive.
    DexRegisterMap only_live_mask(MemoryRegion(region_.Subregion(origin, offset - origin)));

    // Skip the first `number_of_dex_registers - 1` entries.
    for (uint16_t i = 0; i < number_of_dex_registers; ++i) {
      if (only_live_mask.IsDexRegisterLive(i)) {
        // Read the first next byte and inspect its first 3 bits to decide
        // whether it is a short or a large location.
        DexRegisterMap::ShortLocation first_byte =
            region_.LoadUnaligned<DexRegisterMap::ShortLocation>(offset);
        DexRegisterLocation::Kind kind =
            DexRegisterMap::ExtractKindFromShortLocation(first_byte);
        if (DexRegisterLocation::IsShortLocationKind(kind)) {
          // Short location.  Skip the current byte.
          offset += DexRegisterMap::SingleShortEntrySize();
        } else {
          // Large location.  Skip the 5 next bytes.
          offset += DexRegisterMap::SingleLargeEntrySize();
        }
      }
    }
    size_t size = offset - origin;
    return size;
  }

  MemoryRegion region_;
  friend class StackMapStream;
};

}  // namespace art

#endif  // ART_RUNTIME_STACK_MAP_H_
