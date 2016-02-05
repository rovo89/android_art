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
#include "base/bit_utils.h"
#include "memory_region.h"

namespace art {

#define ELEMENT_BYTE_OFFSET_AFTER(PreviousElement) \
  k ## PreviousElement ## Offset + sizeof(PreviousElement ## Type)

#define ELEMENT_BIT_OFFSET_AFTER(PreviousElement) \
  k ## PreviousElement ## BitOffset + PreviousElement ## BitSize

class VariableIndentationOutputStream;

// Size of a frame slot, in bytes.  This constant is a signed value,
// to please the compiler in arithmetic operations involving int32_t
// (signed) values.
static constexpr ssize_t kFrameSlotSize = 4;

// Size of Dex virtual registers.
static constexpr size_t kVRegSize = 4;

// We encode the number of bytes needed for writing a value on 3 bits
// (i.e. up to 8 values), for values that we know are maximum 32-bit
// long.
static constexpr size_t kNumberOfBitForNumberOfBytesForEncoding = 3;

class CodeInfo;
class StackMapEncoding;

/**
 * Classes in the following file are wrapper on stack map information backed
 * by a MemoryRegion. As such they read and write to the region, they don't have
 * their own fields.
 */

// Dex register location container used by DexRegisterMap and StackMapStream.
class DexRegisterLocation {
 public:
  /*
   * The location kind used to populate the Dex register information in a
   * StackMapStream can either be:
   * - kStack: vreg stored on the stack, value holds the stack offset;
   * - kInRegister: vreg stored in low 32 bits of a core physical register,
   *                value holds the register number;
   * - kInRegisterHigh: vreg stored in high 32 bits of a core physical register,
   *                    value holds the register number;
   * - kInFpuRegister: vreg stored in low 32 bits of an FPU register,
   *                   value holds the register number;
   * - kInFpuRegisterHigh: vreg stored in high 32 bits of an FPU register,
   *                       value holds the register number;
   * - kConstant: value holds the constant;
   *
   * In addition, DexRegisterMap also uses these values:
   * - kInStackLargeOffset: value holds a "large" stack offset (greater than
   *   or equal to 128 bytes);
   * - kConstantLargeValue: value holds a "large" constant (lower than 0, or
   *   or greater than or equal to 32);
   * - kNone: the register has no location, meaning it has not been set.
   */
  enum class Kind : uint8_t {
    // Short location kinds, for entries fitting on one byte (3 bits
    // for the kind, 5 bits for the value) in a DexRegisterMap.
    kInStack = 0,             // 0b000
    kInRegister = 1,          // 0b001
    kInRegisterHigh = 2,      // 0b010
    kInFpuRegister = 3,       // 0b011
    kInFpuRegisterHigh = 4,   // 0b100
    kConstant = 5,            // 0b101

    // Large location kinds, requiring a 5-byte encoding (1 byte for the
    // kind, 4 bytes for the value).

    // Stack location at a large offset, meaning that the offset value
    // divided by the stack frame slot size (4 bytes) cannot fit on a
    // 5-bit unsigned integer (i.e., this offset value is greater than
    // or equal to 2^5 * 4 = 128 bytes).
    kInStackLargeOffset = 6,  // 0b110

    // Large constant, that cannot fit on a 5-bit signed integer (i.e.,
    // lower than 0, or greater than or equal to 2^5 = 32).
    kConstantLargeValue = 7,  // 0b111

    // Entries with no location are not stored and do not need own marker.
    kNone = static_cast<uint8_t>(-1),

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
      case Kind::kInRegisterHigh:
        return "in register high";
      case Kind::kInFpuRegister:
        return "in fpu register";
      case Kind::kInFpuRegisterHigh:
        return "in fpu register high";
      case Kind::kConstant:
        return "as constant";
      case Kind::kInStackLargeOffset:
        return "in stack (large offset)";
      case Kind::kConstantLargeValue:
        return "as constant (large value)";
    }
    UNREACHABLE();
  }

  static bool IsShortLocationKind(Kind kind) {
    switch (kind) {
      case Kind::kInStack:
      case Kind::kInRegister:
      case Kind::kInRegisterHigh:
      case Kind::kInFpuRegister:
      case Kind::kInFpuRegisterHigh:
      case Kind::kConstant:
        return true;

      case Kind::kInStackLargeOffset:
      case Kind::kConstantLargeValue:
        return false;

      case Kind::kNone:
        LOG(FATAL) << "Unexpected location kind " << PrettyDescriptor(kind);
    }
    UNREACHABLE();
  }

  // Convert `kind` to a "surface" kind, i.e. one that doesn't include
  // any value with a "large" qualifier.
  // TODO: Introduce another enum type for the surface kind?
  static Kind ConvertToSurfaceKind(Kind kind) {
    switch (kind) {
      case Kind::kInStack:
      case Kind::kInRegister:
      case Kind::kInRegisterHigh:
      case Kind::kInFpuRegister:
      case Kind::kInFpuRegisterHigh:
      case Kind::kConstant:
        return kind;

      case Kind::kInStackLargeOffset:
        return Kind::kInStack;

      case Kind::kConstantLargeValue:
        return Kind::kConstant;

      case Kind::kNone:
        return kind;
    }
    UNREACHABLE();
  }

  // Required by art::StackMapStream::LocationCatalogEntriesIndices.
  DexRegisterLocation() : kind_(Kind::kNone), value_(0) {}

  DexRegisterLocation(Kind kind, int32_t value) : kind_(kind), value_(value) {}

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

  friend class DexRegisterLocationHashFn;
};

/**
 * Store information on unique Dex register locations used in a method.
 * The information is of the form:
 *
 *   [DexRegisterLocation+].
 *
 * DexRegisterLocations are either 1- or 5-byte wide (see art::DexRegisterLocation::Kind).
 */
class DexRegisterLocationCatalog {
 public:
  explicit DexRegisterLocationCatalog(MemoryRegion region) : region_(region) {}

  // Short (compressed) location, fitting on one byte.
  typedef uint8_t ShortLocation;

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
      DCHECK(IsShortValue(value)) << value;
      region_.StoreUnaligned<ShortLocation>(offset, MakeShortLocation(kind, value));
    } else {
      // Large location.  Write the location on one byte and the value
      // on 4 bytes.
      DCHECK(!IsShortValue(value)) << value;
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

  // Find the offset of the location catalog entry number `location_catalog_entry_index`.
  size_t FindLocationOffset(size_t location_catalog_entry_index) const {
    size_t offset = kFixedSize;
    // Skip the first `location_catalog_entry_index - 1` entries.
    for (uint16_t i = 0; i < location_catalog_entry_index; ++i) {
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
    return offset;
  }

  // Get the internal kind of entry at `location_catalog_entry_index`.
  DexRegisterLocation::Kind GetLocationInternalKind(size_t location_catalog_entry_index) const {
    if (location_catalog_entry_index == kNoLocationEntryIndex) {
      return DexRegisterLocation::Kind::kNone;
    }
    return ExtractKindAtOffset(FindLocationOffset(location_catalog_entry_index));
  }

  // Get the (surface) kind and value of entry at `location_catalog_entry_index`.
  DexRegisterLocation GetDexRegisterLocation(size_t location_catalog_entry_index) const {
    if (location_catalog_entry_index == kNoLocationEntryIndex) {
      return DexRegisterLocation::None();
    }
    size_t offset = FindLocationOffset(location_catalog_entry_index);
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

  // Compute the compressed kind of `location`.
  static DexRegisterLocation::Kind ComputeCompressedKind(const DexRegisterLocation& location) {
    DexRegisterLocation::Kind kind = location.GetInternalKind();
    switch (kind) {
      case DexRegisterLocation::Kind::kInStack:
        return IsShortStackOffsetValue(location.GetValue())
            ? DexRegisterLocation::Kind::kInStack
            : DexRegisterLocation::Kind::kInStackLargeOffset;

      case DexRegisterLocation::Kind::kInRegister:
      case DexRegisterLocation::Kind::kInRegisterHigh:
        DCHECK_GE(location.GetValue(), 0);
        DCHECK_LT(location.GetValue(), 1 << kValueBits);
        return kind;

      case DexRegisterLocation::Kind::kInFpuRegister:
      case DexRegisterLocation::Kind::kInFpuRegisterHigh:
        DCHECK_GE(location.GetValue(), 0);
        DCHECK_LT(location.GetValue(), 1 << kValueBits);
        return kind;

      case DexRegisterLocation::Kind::kConstant:
        return IsShortConstantValue(location.GetValue())
            ? DexRegisterLocation::Kind::kConstant
            : DexRegisterLocation::Kind::kConstantLargeValue;

      case DexRegisterLocation::Kind::kConstantLargeValue:
      case DexRegisterLocation::Kind::kInStackLargeOffset:
      case DexRegisterLocation::Kind::kNone:
        LOG(FATAL) << "Unexpected location kind " << DexRegisterLocation::PrettyDescriptor(kind);
    }
    UNREACHABLE();
  }

  // Can `location` be turned into a short location?
  static bool CanBeEncodedAsShortLocation(const DexRegisterLocation& location) {
    DexRegisterLocation::Kind kind = location.GetInternalKind();
    switch (kind) {
      case DexRegisterLocation::Kind::kInStack:
        return IsShortStackOffsetValue(location.GetValue());

      case DexRegisterLocation::Kind::kInRegister:
      case DexRegisterLocation::Kind::kInRegisterHigh:
      case DexRegisterLocation::Kind::kInFpuRegister:
      case DexRegisterLocation::Kind::kInFpuRegisterHigh:
        return true;

      case DexRegisterLocation::Kind::kConstant:
        return IsShortConstantValue(location.GetValue());

      case DexRegisterLocation::Kind::kConstantLargeValue:
      case DexRegisterLocation::Kind::kInStackLargeOffset:
      case DexRegisterLocation::Kind::kNone:
        LOG(FATAL) << "Unexpected location kind " << DexRegisterLocation::PrettyDescriptor(kind);
    }
    UNREACHABLE();
  }

  static size_t EntrySize(const DexRegisterLocation& location) {
    return CanBeEncodedAsShortLocation(location) ? SingleShortEntrySize() : SingleLargeEntrySize();
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

  void Dump(VariableIndentationOutputStream* vios, const CodeInfo& code_info);

  // Special (invalid) Dex register location catalog entry index meaning
  // that there is no location for a given Dex register (i.e., it is
  // mapped to a DexRegisterLocation::Kind::kNone location).
  static constexpr size_t kNoLocationEntryIndex = -1;

 private:
  static constexpr int kFixedSize = 0;

  // Width of the kind "field" in a short location, in bits.
  static constexpr size_t kKindBits = 3;
  // Width of the value "field" in a short location, in bits.
  static constexpr size_t kValueBits = 5;

  static constexpr uint8_t kKindMask = (1 << kKindBits) - 1;
  static constexpr int32_t kValueMask = (1 << kValueBits) - 1;
  static constexpr size_t kKindOffset = 0;
  static constexpr size_t kValueOffset = kKindBits;

  static bool IsShortStackOffsetValue(int32_t value) {
    DCHECK_EQ(value % kFrameSlotSize, 0);
    return IsShortValue(value / kFrameSlotSize);
  }

  static bool IsShortConstantValue(int32_t value) {
    return IsShortValue(value);
  }

  static bool IsShortValue(int32_t value) {
    return IsUint<kValueBits>(value);
  }

  static ShortLocation MakeShortLocation(DexRegisterLocation::Kind kind, int32_t value) {
    uint8_t kind_integer_value = static_cast<uint8_t>(kind);
    DCHECK(IsUint<kKindBits>(kind_integer_value)) << kind_integer_value;
    DCHECK(IsShortValue(value)) << value;
    return (kind_integer_value & kKindMask) << kKindOffset
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

/* Information on Dex register locations for a specific PC, mapping a
 * stack map's Dex register to a location entry in a DexRegisterLocationCatalog.
 * The information is of the form:
 *
 *   [live_bit_mask, entries*]
 *
 * where entries are concatenated unsigned integer values encoded on a number
 * of bits (fixed per DexRegisterMap instances of a CodeInfo object) depending
 * on the number of entries in the Dex register location catalog
 * (see DexRegisterMap::SingleEntrySizeInBits).  The map is 1-byte aligned.
 */
class DexRegisterMap {
 public:
  explicit DexRegisterMap(MemoryRegion region) : region_(region) {}
  DexRegisterMap() {}

  bool IsValid() const { return region_.pointer() != nullptr; }

  // Get the surface kind of Dex register `dex_register_number`.
  DexRegisterLocation::Kind GetLocationKind(uint16_t dex_register_number,
                                            uint16_t number_of_dex_registers,
                                            const CodeInfo& code_info,
                                            const StackMapEncoding& enc) const {
    return DexRegisterLocation::ConvertToSurfaceKind(
        GetLocationInternalKind(dex_register_number, number_of_dex_registers, code_info, enc));
  }

  // Get the internal kind of Dex register `dex_register_number`.
  DexRegisterLocation::Kind GetLocationInternalKind(uint16_t dex_register_number,
                                                    uint16_t number_of_dex_registers,
                                                    const CodeInfo& code_info,
                                                    const StackMapEncoding& enc) const;

  // Get the Dex register location `dex_register_number`.
  DexRegisterLocation GetDexRegisterLocation(uint16_t dex_register_number,
                                             uint16_t number_of_dex_registers,
                                             const CodeInfo& code_info,
                                             const StackMapEncoding& enc) const;

  int32_t GetStackOffsetInBytes(uint16_t dex_register_number,
                                uint16_t number_of_dex_registers,
                                const CodeInfo& code_info,
                                const StackMapEncoding& enc) const {
    DexRegisterLocation location =
        GetDexRegisterLocation(dex_register_number, number_of_dex_registers, code_info, enc);
    DCHECK(location.GetKind() == DexRegisterLocation::Kind::kInStack);
    // GetDexRegisterLocation returns the offset in bytes.
    return location.GetValue();
  }

  int32_t GetConstant(uint16_t dex_register_number,
                      uint16_t number_of_dex_registers,
                      const CodeInfo& code_info,
                      const StackMapEncoding& enc) const {
    DexRegisterLocation location =
        GetDexRegisterLocation(dex_register_number, number_of_dex_registers, code_info, enc);
    DCHECK(location.GetKind() == DexRegisterLocation::Kind::kConstant)
        << DexRegisterLocation::PrettyDescriptor(location.GetKind());
    return location.GetValue();
  }

  int32_t GetMachineRegister(uint16_t dex_register_number,
                             uint16_t number_of_dex_registers,
                             const CodeInfo& code_info,
                             const StackMapEncoding& enc) const {
    DexRegisterLocation location =
        GetDexRegisterLocation(dex_register_number, number_of_dex_registers, code_info, enc);
    DCHECK(location.GetInternalKind() == DexRegisterLocation::Kind::kInRegister ||
           location.GetInternalKind() == DexRegisterLocation::Kind::kInRegisterHigh ||
           location.GetInternalKind() == DexRegisterLocation::Kind::kInFpuRegister ||
           location.GetInternalKind() == DexRegisterLocation::Kind::kInFpuRegisterHigh)
        << DexRegisterLocation::PrettyDescriptor(location.GetInternalKind());
    return location.GetValue();
  }

  // Get the index of the entry in the Dex register location catalog
  // corresponding to `dex_register_number`.
  size_t GetLocationCatalogEntryIndex(uint16_t dex_register_number,
                                      uint16_t number_of_dex_registers,
                                      size_t number_of_location_catalog_entries) const {
    if (!IsDexRegisterLive(dex_register_number)) {
      return DexRegisterLocationCatalog::kNoLocationEntryIndex;
    }

    if (number_of_location_catalog_entries == 1) {
      // We do not allocate space for location maps in the case of a
      // single-entry location catalog, as it is useless.  The only valid
      // entry index is 0;
      return 0;
    }

    // The bit offset of the beginning of the map locations.
    size_t map_locations_offset_in_bits =
        GetLocationMappingDataOffset(number_of_dex_registers) * kBitsPerByte;
    size_t index_in_dex_register_map = GetIndexInDexRegisterMap(dex_register_number);
    DCHECK_LT(index_in_dex_register_map, GetNumberOfLiveDexRegisters(number_of_dex_registers));
    // The bit size of an entry.
    size_t map_entry_size_in_bits = SingleEntrySizeInBits(number_of_location_catalog_entries);
    // The bit offset where `index_in_dex_register_map` is located.
    size_t entry_offset_in_bits =
        map_locations_offset_in_bits + index_in_dex_register_map * map_entry_size_in_bits;
    size_t location_catalog_entry_index =
        region_.LoadBits(entry_offset_in_bits, map_entry_size_in_bits);
    DCHECK_LT(location_catalog_entry_index, number_of_location_catalog_entries);
    return location_catalog_entry_index;
  }

  // Map entry at `index_in_dex_register_map` to `location_catalog_entry_index`.
  void SetLocationCatalogEntryIndex(size_t index_in_dex_register_map,
                                    size_t location_catalog_entry_index,
                                    uint16_t number_of_dex_registers,
                                    size_t number_of_location_catalog_entries) {
    DCHECK_LT(index_in_dex_register_map, GetNumberOfLiveDexRegisters(number_of_dex_registers));
    DCHECK_LT(location_catalog_entry_index, number_of_location_catalog_entries);

    if (number_of_location_catalog_entries == 1) {
      // We do not allocate space for location maps in the case of a
      // single-entry location catalog, as it is useless.
      return;
    }

    // The bit offset of the beginning of the map locations.
    size_t map_locations_offset_in_bits =
        GetLocationMappingDataOffset(number_of_dex_registers) * kBitsPerByte;
    // The bit size of an entry.
    size_t map_entry_size_in_bits = SingleEntrySizeInBits(number_of_location_catalog_entries);
    // The bit offset where `index_in_dex_register_map` is located.
    size_t entry_offset_in_bits =
        map_locations_offset_in_bits + index_in_dex_register_map * map_entry_size_in_bits;
    region_.StoreBits(entry_offset_in_bits, location_catalog_entry_index, map_entry_size_in_bits);
  }

  void SetLiveBitMask(uint16_t number_of_dex_registers,
                      const BitVector& live_dex_registers_mask) {
    size_t live_bit_mask_offset_in_bits = GetLiveBitMaskOffset() * kBitsPerByte;
    for (uint16_t i = 0; i < number_of_dex_registers; ++i) {
      region_.StoreBit(live_bit_mask_offset_in_bits + i, live_dex_registers_mask.IsBitSet(i));
    }
  }

  bool IsDexRegisterLive(uint16_t dex_register_number) const {
    size_t live_bit_mask_offset_in_bits = GetLiveBitMaskOffset() * kBitsPerByte;
    return region_.LoadBit(live_bit_mask_offset_in_bits + dex_register_number);
  }

  size_t GetNumberOfLiveDexRegisters(uint16_t number_of_dex_registers) const {
    size_t number_of_live_dex_registers = 0;
    for (size_t i = 0; i < number_of_dex_registers; ++i) {
      if (IsDexRegisterLive(i)) {
        ++number_of_live_dex_registers;
      }
    }
    return number_of_live_dex_registers;
  }

  static size_t GetLiveBitMaskOffset() {
    return kFixedSize;
  }

  // Compute the size of the live register bit mask (in bytes), for a
  // method having `number_of_dex_registers` Dex registers.
  static size_t GetLiveBitMaskSize(uint16_t number_of_dex_registers) {
    return RoundUp(number_of_dex_registers, kBitsPerByte) / kBitsPerByte;
  }

  static size_t GetLocationMappingDataOffset(uint16_t number_of_dex_registers) {
    return GetLiveBitMaskOffset() + GetLiveBitMaskSize(number_of_dex_registers);
  }

  size_t GetLocationMappingDataSize(uint16_t number_of_dex_registers,
                                    size_t number_of_location_catalog_entries) const {
    size_t location_mapping_data_size_in_bits =
        GetNumberOfLiveDexRegisters(number_of_dex_registers)
        * SingleEntrySizeInBits(number_of_location_catalog_entries);
    return RoundUp(location_mapping_data_size_in_bits, kBitsPerByte) / kBitsPerByte;
  }

  // Return the size of a map entry in bits.  Note that if
  // `number_of_location_catalog_entries` equals 1, this function returns 0,
  // which is fine, as there is no need to allocate a map for a
  // single-entry location catalog; the only valid location catalog entry index
  // for a live register in this case is 0 and there is no need to
  // store it.
  static size_t SingleEntrySizeInBits(size_t number_of_location_catalog_entries) {
    // Handle the case of 0, as we cannot pass 0 to art::WhichPowerOf2.
    return number_of_location_catalog_entries == 0
        ? 0u
        : WhichPowerOf2(RoundUpToPowerOfTwo(number_of_location_catalog_entries));
  }

  // Return the size of the DexRegisterMap object, in bytes.
  size_t Size() const {
    return region_.size();
  }

  void Dump(VariableIndentationOutputStream* vios,
            const CodeInfo& code_info, uint16_t number_of_dex_registers) const;

 private:
  // Return the index in the Dex register map corresponding to the Dex
  // register number `dex_register_number`.
  size_t GetIndexInDexRegisterMap(uint16_t dex_register_number) const {
    if (!IsDexRegisterLive(dex_register_number)) {
      return kInvalidIndexInDexRegisterMap;
    }
    return GetNumberOfLiveDexRegisters(dex_register_number);
  }

  // Special (invalid) Dex register map entry index meaning that there
  // is no index in the map for a given Dex register (i.e., it must
  // have been mapped to a DexRegisterLocation::Kind::kNone location).
  static constexpr size_t kInvalidIndexInDexRegisterMap = -1;

  static constexpr int kFixedSize = 0;

  MemoryRegion region_;

  friend class CodeInfo;
  friend class StackMapStream;
};

class StackMapEncoding {
 public:
  StackMapEncoding() {}

  StackMapEncoding(size_t stack_mask_size,
                   size_t bytes_for_inline_info,
                   size_t bytes_for_dex_register_map,
                   size_t bytes_for_dex_pc,
                   size_t bytes_for_native_pc,
                   size_t bytes_for_register_mask)
      : bytes_for_stack_mask_(stack_mask_size),
        bytes_for_inline_info_(bytes_for_inline_info),
        bytes_for_dex_register_map_(bytes_for_dex_register_map),
        bytes_for_dex_pc_(bytes_for_dex_pc),
        bytes_for_native_pc_(bytes_for_native_pc),
        bytes_for_register_mask_(bytes_for_register_mask) {}

  static StackMapEncoding CreateFromSizes(size_t stack_mask_size,
                                          size_t inline_info_size,
                                          size_t dex_register_map_size,
                                          size_t dex_pc_max,
                                          size_t native_pc_max,
                                          size_t register_mask_max) {
    return StackMapEncoding(
        stack_mask_size,
        // + 1 to also encode kNoInlineInfo: if an inline info offset
        // is at 0xFF, we want to overflow to a larger encoding, because it will
        // conflict with kNoInlineInfo.
        // The offset is relative to the dex register map. TODO: Change this.
        inline_info_size == 0
          ? 0
          : EncodingSizeInBytes(dex_register_map_size + inline_info_size + 1),
        // + 1 to also encode kNoDexRegisterMap: if a dex register map offset
        // is at 0xFF, we want to overflow to a larger encoding, because it will
        // conflict with kNoDexRegisterMap.
        EncodingSizeInBytes(dex_register_map_size + 1),
        EncodingSizeInBytes(dex_pc_max),
        EncodingSizeInBytes(native_pc_max),
        EncodingSizeInBytes(register_mask_max));
  }

  // Get the size of one stack map of this CodeInfo object, in bytes.
  // All stack maps of a CodeInfo have the same size.
  size_t ComputeStackMapSize() const {
    return bytes_for_register_mask_
         + bytes_for_stack_mask_
         + bytes_for_inline_info_
         + bytes_for_dex_register_map_
         + bytes_for_dex_pc_
         + bytes_for_native_pc_;
  }

  bool HasInlineInfo() const { return bytes_for_inline_info_ > 0; }

  size_t NumberOfBytesForStackMask() const { return bytes_for_stack_mask_; }
  size_t NumberOfBytesForInlineInfo() const { return bytes_for_inline_info_; }
  size_t NumberOfBytesForDexRegisterMap() const { return bytes_for_dex_register_map_; }
  size_t NumberOfBytesForDexPc() const { return bytes_for_dex_pc_; }
  size_t NumberOfBytesForNativePc() const { return bytes_for_native_pc_; }
  size_t NumberOfBytesForRegisterMask() const { return bytes_for_register_mask_; }

  size_t ComputeStackMapRegisterMaskOffset() const {
    return kRegisterMaskOffset;
  }

  size_t ComputeStackMapStackMaskOffset() const {
    return ComputeStackMapRegisterMaskOffset() + bytes_for_register_mask_;
  }

  size_t ComputeStackMapDexPcOffset() const {
    return ComputeStackMapStackMaskOffset() + bytes_for_stack_mask_;
  }

  size_t ComputeStackMapNativePcOffset() const {
    return ComputeStackMapDexPcOffset() + bytes_for_dex_pc_;
  }

  size_t ComputeStackMapDexRegisterMapOffset() const {
    return ComputeStackMapNativePcOffset() + bytes_for_native_pc_;
  }

  size_t ComputeStackMapInlineInfoOffset() const {
    return ComputeStackMapDexRegisterMapOffset() + bytes_for_dex_register_map_;
  }

 private:
  static size_t EncodingSizeInBytes(size_t max_element) {
    DCHECK(IsUint<32>(max_element));
    return (max_element == 0) ? 0
         : IsUint<8>(max_element) ? 1
         : IsUint<16>(max_element) ? 2
         : IsUint<24>(max_element) ? 3
         : 4;
  }

  static constexpr int kRegisterMaskOffset = 0;

  size_t bytes_for_stack_mask_;
  size_t bytes_for_inline_info_;
  size_t bytes_for_dex_register_map_;
  size_t bytes_for_dex_pc_;
  size_t bytes_for_native_pc_;
  size_t bytes_for_register_mask_;
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
 *
 *   [dex_pc, native_pc_offset, dex_register_map_offset, inlining_info_offset, register_mask,
 *   stack_mask].
 */
class StackMap {
 public:
  StackMap() {}
  explicit StackMap(MemoryRegion region) : region_(region) {}

  bool IsValid() const { return region_.pointer() != nullptr; }

  uint32_t GetDexPc(const StackMapEncoding& encoding) const {
    return LoadAt(encoding.NumberOfBytesForDexPc(), encoding.ComputeStackMapDexPcOffset());
  }

  void SetDexPc(const StackMapEncoding& encoding, uint32_t dex_pc) {
    StoreAt(encoding.NumberOfBytesForDexPc(), encoding.ComputeStackMapDexPcOffset(), dex_pc);
  }

  uint32_t GetNativePcOffset(const StackMapEncoding& encoding) const {
    return LoadAt(encoding.NumberOfBytesForNativePc(), encoding.ComputeStackMapNativePcOffset());
  }

  void SetNativePcOffset(const StackMapEncoding& encoding, uint32_t native_pc_offset) {
    StoreAt(encoding.NumberOfBytesForNativePc(),
            encoding.ComputeStackMapNativePcOffset(),
            native_pc_offset);
  }

  uint32_t GetDexRegisterMapOffset(const StackMapEncoding& encoding) const {
    return LoadAt(encoding.NumberOfBytesForDexRegisterMap(),
                  encoding.ComputeStackMapDexRegisterMapOffset(),
                  /* check_max */ true);
  }

  void SetDexRegisterMapOffset(const StackMapEncoding& encoding, uint32_t offset) {
    StoreAt(encoding.NumberOfBytesForDexRegisterMap(),
            encoding.ComputeStackMapDexRegisterMapOffset(),
            offset);
  }

  uint32_t GetInlineDescriptorOffset(const StackMapEncoding& encoding) const {
    if (!encoding.HasInlineInfo()) return kNoInlineInfo;
    return LoadAt(encoding.NumberOfBytesForInlineInfo(),
                  encoding.ComputeStackMapInlineInfoOffset(),
                  /* check_max */ true);
  }

  void SetInlineDescriptorOffset(const StackMapEncoding& encoding, uint32_t offset) {
    DCHECK(encoding.HasInlineInfo());
    StoreAt(encoding.NumberOfBytesForInlineInfo(),
            encoding.ComputeStackMapInlineInfoOffset(),
            offset);
  }

  uint32_t GetRegisterMask(const StackMapEncoding& encoding) const {
    return LoadAt(encoding.NumberOfBytesForRegisterMask(),
                  encoding.ComputeStackMapRegisterMaskOffset());
  }

  void SetRegisterMask(const StackMapEncoding& encoding, uint32_t mask) {
    StoreAt(encoding.NumberOfBytesForRegisterMask(),
            encoding.ComputeStackMapRegisterMaskOffset(),
            mask);
  }

  MemoryRegion GetStackMask(const StackMapEncoding& encoding) const {
    return region_.Subregion(encoding.ComputeStackMapStackMaskOffset(),
                             encoding.NumberOfBytesForStackMask());
  }

  void SetStackMask(const StackMapEncoding& encoding, const BitVector& sp_map) {
    MemoryRegion region = GetStackMask(encoding);
    sp_map.CopyTo(region.start(), region.size());
  }

  bool HasDexRegisterMap(const StackMapEncoding& encoding) const {
    return GetDexRegisterMapOffset(encoding) != kNoDexRegisterMap;
  }

  bool HasInlineInfo(const StackMapEncoding& encoding) const {
    return GetInlineDescriptorOffset(encoding) != kNoInlineInfo;
  }

  bool Equals(const StackMap& other) const {
    return region_.pointer() == other.region_.pointer()
       && region_.size() == other.region_.size();
  }

  void Dump(VariableIndentationOutputStream* vios,
            const CodeInfo& code_info,
            const StackMapEncoding& encoding,
            uint32_t code_offset,
            uint16_t number_of_dex_registers,
            const std::string& header_suffix = "") const;

  // Special (invalid) offset for the DexRegisterMapOffset field meaning
  // that there is no Dex register map for this stack map.
  static constexpr uint32_t kNoDexRegisterMap = -1;

  // Special (invalid) offset for the InlineDescriptorOffset field meaning
  // that there is no inline info for this stack map.
  static constexpr uint32_t kNoInlineInfo = -1;

 private:
  static constexpr int kFixedSize = 0;

  // Loads `number_of_bytes` at the given `offset` and assemble a uint32_t. If `check_max` is true,
  // this method converts a maximum value of size `number_of_bytes` into a uint32_t 0xFFFFFFFF.
  uint32_t LoadAt(size_t number_of_bytes, size_t offset, bool check_max = false) const;
  void StoreAt(size_t number_of_bytes, size_t offset, uint32_t value) const;

  MemoryRegion region_;

  friend class StackMapStream;
};

/**
 * Inline information for a specific PC. The information is of the form:
 *
 *   [inlining_depth, entry+]
 *
 * where `entry` is of the form:
 *
 *   [dex_pc, method_index, dex_register_map_offset].
 */
class InlineInfo {
 public:
  // Memory layout: fixed contents.
  typedef uint8_t DepthType;
  // Memory layout: single entry contents.
  typedef uint32_t MethodIndexType;
  typedef uint32_t DexPcType;
  typedef uint8_t InvokeTypeType;
  typedef uint32_t DexRegisterMapType;

  explicit InlineInfo(MemoryRegion region) : region_(region) {}

  DepthType GetDepth() const {
    return region_.LoadUnaligned<DepthType>(kDepthOffset);
  }

  void SetDepth(DepthType depth) {
    region_.StoreUnaligned<DepthType>(kDepthOffset, depth);
  }

  MethodIndexType GetMethodIndexAtDepth(DepthType depth) const {
    return region_.LoadUnaligned<MethodIndexType>(
        kFixedSize + depth * SingleEntrySize() + kMethodIndexOffset);
  }

  void SetMethodIndexAtDepth(DepthType depth, MethodIndexType index) {
    region_.StoreUnaligned<MethodIndexType>(
        kFixedSize + depth * SingleEntrySize() + kMethodIndexOffset, index);
  }

  DexPcType GetDexPcAtDepth(DepthType depth) const {
    return region_.LoadUnaligned<DexPcType>(
        kFixedSize + depth * SingleEntrySize() + kDexPcOffset);
  }

  void SetDexPcAtDepth(DepthType depth, DexPcType dex_pc) {
    region_.StoreUnaligned<DexPcType>(
        kFixedSize + depth * SingleEntrySize() + kDexPcOffset, dex_pc);
  }

  InvokeTypeType GetInvokeTypeAtDepth(DepthType depth) const {
    return region_.LoadUnaligned<InvokeTypeType>(
        kFixedSize + depth * SingleEntrySize() + kInvokeTypeOffset);
  }

  void SetInvokeTypeAtDepth(DepthType depth, InvokeTypeType invoke_type) {
    region_.StoreUnaligned<InvokeTypeType>(
        kFixedSize + depth * SingleEntrySize() + kInvokeTypeOffset, invoke_type);
  }

  DexRegisterMapType GetDexRegisterMapOffsetAtDepth(DepthType depth) const {
    return region_.LoadUnaligned<DexRegisterMapType>(
        kFixedSize + depth * SingleEntrySize() + kDexRegisterMapOffset);
  }

  void SetDexRegisterMapOffsetAtDepth(DepthType depth, DexRegisterMapType offset) {
    region_.StoreUnaligned<DexRegisterMapType>(
        kFixedSize + depth * SingleEntrySize() + kDexRegisterMapOffset, offset);
  }

  bool HasDexRegisterMapAtDepth(DepthType depth) const {
    return GetDexRegisterMapOffsetAtDepth(depth) != StackMap::kNoDexRegisterMap;
  }

  static size_t SingleEntrySize() {
    return kFixedEntrySize;
  }

  void Dump(VariableIndentationOutputStream* vios,
            const CodeInfo& info, uint16_t* number_of_dex_registers) const;


 private:
  static constexpr int kDepthOffset = 0;
  static constexpr int kFixedSize = ELEMENT_BYTE_OFFSET_AFTER(Depth);

  static constexpr int kMethodIndexOffset = 0;
  static constexpr int kDexPcOffset = ELEMENT_BYTE_OFFSET_AFTER(MethodIndex);
  static constexpr int kInvokeTypeOffset = ELEMENT_BYTE_OFFSET_AFTER(DexPc);
  static constexpr int kDexRegisterMapOffset = ELEMENT_BYTE_OFFSET_AFTER(InvokeType);
  static constexpr int kFixedEntrySize = ELEMENT_BYTE_OFFSET_AFTER(DexRegisterMap);

  MemoryRegion region_;

  friend class CodeInfo;
  friend class StackMap;
  friend class StackMapStream;
};

/**
 * Wrapper around all compiler information collected for a method.
 * The information is of the form:
 *
 *   [overall_size, encoding_info, number_of_location_catalog_entries, number_of_stack_maps,
 *   stack_mask_size, DexRegisterLocationCatalog+, StackMap+, DexRegisterMap+, InlineInfo*]
 *
 * where `encoding_info` is of the form:
 *
 *  [has_inline_info, inline_info_size_in_bytes, dex_register_map_size_in_bytes,
 *  dex_pc_size_in_bytes, native_pc_size_in_bytes, register_mask_size_in_bytes].
 */
class CodeInfo {
 public:
  // Memory layout: fixed contents.
  typedef uint32_t OverallSizeType;
  typedef uint16_t EncodingInfoType;
  typedef uint32_t NumberOfLocationCatalogEntriesType;
  typedef uint32_t NumberOfStackMapsType;
  typedef uint32_t StackMaskSizeType;

  // Memory (bit) layout: encoding info.
  static constexpr int HasInlineInfoBitSize = 1;
  static constexpr int InlineInfoBitSize = kNumberOfBitForNumberOfBytesForEncoding;
  static constexpr int DexRegisterMapBitSize = kNumberOfBitForNumberOfBytesForEncoding;
  static constexpr int DexPcBitSize = kNumberOfBitForNumberOfBytesForEncoding;
  static constexpr int NativePcBitSize = kNumberOfBitForNumberOfBytesForEncoding;
  static constexpr int RegisterMaskBitSize = kNumberOfBitForNumberOfBytesForEncoding;

  explicit CodeInfo(MemoryRegion region) : region_(region) {}

  explicit CodeInfo(const void* data) {
    uint32_t size = reinterpret_cast<const uint32_t*>(data)[0];
    region_ = MemoryRegion(const_cast<void*>(data), size);
  }

  StackMapEncoding ExtractEncoding() const {
    return StackMapEncoding(region_.LoadUnaligned<uint32_t>(kStackMaskSizeOffset),
                            GetNumberOfBytesForEncoding(kInlineInfoBitOffset),
                            GetNumberOfBytesForEncoding(kDexRegisterMapBitOffset),
                            GetNumberOfBytesForEncoding(kDexPcBitOffset),
                            GetNumberOfBytesForEncoding(kNativePcBitOffset),
                            GetNumberOfBytesForEncoding(kRegisterMaskBitOffset));
  }

  void SetEncoding(const StackMapEncoding& encoding) {
    region_.StoreUnaligned<uint32_t>(kStackMaskSizeOffset, encoding.NumberOfBytesForStackMask());
    region_.StoreBit(kHasInlineInfoBitOffset, encoding.NumberOfBytesForInlineInfo() != 0);
    SetEncodingAt(kInlineInfoBitOffset, encoding.NumberOfBytesForInlineInfo());
    SetEncodingAt(kDexRegisterMapBitOffset, encoding.NumberOfBytesForDexRegisterMap());
    SetEncodingAt(kDexPcBitOffset, encoding.NumberOfBytesForDexPc());
    SetEncodingAt(kNativePcBitOffset, encoding.NumberOfBytesForNativePc());
    SetEncodingAt(kRegisterMaskBitOffset, encoding.NumberOfBytesForRegisterMask());
  }

  void SetEncodingAt(size_t bit_offset, size_t number_of_bytes) {
    region_.StoreBits(bit_offset, number_of_bytes, kNumberOfBitForNumberOfBytesForEncoding);
  }

  size_t GetNumberOfBytesForEncoding(size_t bit_offset) const {
    return region_.LoadBits(bit_offset, kNumberOfBitForNumberOfBytesForEncoding);
  }

  bool HasInlineInfo() const {
    return region_.LoadBit(kHasInlineInfoBitOffset);
  }

  DexRegisterLocationCatalog GetDexRegisterLocationCatalog(const StackMapEncoding& encoding) const {
    return DexRegisterLocationCatalog(region_.Subregion(
        GetDexRegisterLocationCatalogOffset(encoding),
        GetDexRegisterLocationCatalogSize(encoding)));
  }

  StackMap GetStackMapAt(size_t i, const StackMapEncoding& encoding) const {
    size_t stack_map_size = encoding.ComputeStackMapSize();
    return StackMap(GetStackMaps(encoding).Subregion(i * stack_map_size, stack_map_size));
  }

  OverallSizeType GetOverallSize() const {
    return region_.LoadUnaligned<OverallSizeType>(kOverallSizeOffset);
  }

  void SetOverallSize(OverallSizeType size) {
    region_.StoreUnaligned<OverallSizeType>(kOverallSizeOffset, size);
  }

  NumberOfLocationCatalogEntriesType GetNumberOfLocationCatalogEntries() const {
    return region_.LoadUnaligned<NumberOfLocationCatalogEntriesType>(
        kNumberOfLocationCatalogEntriesOffset);
  }

  void SetNumberOfLocationCatalogEntries(NumberOfLocationCatalogEntriesType num_entries) {
    region_.StoreUnaligned<NumberOfLocationCatalogEntriesType>(
        kNumberOfLocationCatalogEntriesOffset, num_entries);
  }

  uint32_t GetDexRegisterLocationCatalogSize(const StackMapEncoding& encoding) const {
    return ComputeDexRegisterLocationCatalogSize(GetDexRegisterLocationCatalogOffset(encoding),
                                                 GetNumberOfLocationCatalogEntries());
  }

  NumberOfStackMapsType GetNumberOfStackMaps() const {
    return region_.LoadUnaligned<NumberOfStackMapsType>(kNumberOfStackMapsOffset);
  }

  void SetNumberOfStackMaps(NumberOfStackMapsType number_of_stack_maps) {
    region_.StoreUnaligned<NumberOfStackMapsType>(kNumberOfStackMapsOffset, number_of_stack_maps);
  }

  // Get the size of all the stack maps of this CodeInfo object, in bytes.
  size_t GetStackMapsSize(const StackMapEncoding& encoding) const {
    return encoding.ComputeStackMapSize() * GetNumberOfStackMaps();
  }

  uint32_t GetDexRegisterLocationCatalogOffset(const StackMapEncoding& encoding) const {
    return GetStackMapsOffset() + GetStackMapsSize(encoding);
  }

  size_t GetDexRegisterMapsOffset(const StackMapEncoding& encoding) const {
    return GetDexRegisterLocationCatalogOffset(encoding)
         + GetDexRegisterLocationCatalogSize(encoding);
  }

  uint32_t GetStackMapsOffset() const {
    return kFixedSize;
  }

  DexRegisterMap GetDexRegisterMapOf(StackMap stack_map,
                                     const StackMapEncoding& encoding,
                                     uint32_t number_of_dex_registers) const {
    if (!stack_map.HasDexRegisterMap(encoding)) {
      return DexRegisterMap();
    } else {
      uint32_t offset = GetDexRegisterMapsOffset(encoding)
                        + stack_map.GetDexRegisterMapOffset(encoding);
      size_t size = ComputeDexRegisterMapSizeOf(offset, number_of_dex_registers);
      return DexRegisterMap(region_.Subregion(offset, size));
    }
  }

  // Return the `DexRegisterMap` pointed by `inline_info` at depth `depth`.
  DexRegisterMap GetDexRegisterMapAtDepth(uint8_t depth,
                                          InlineInfo inline_info,
                                          const StackMapEncoding& encoding,
                                          uint32_t number_of_dex_registers) const {
    if (!inline_info.HasDexRegisterMapAtDepth(depth)) {
      return DexRegisterMap();
    } else {
      uint32_t offset = GetDexRegisterMapsOffset(encoding)
                        + inline_info.GetDexRegisterMapOffsetAtDepth(depth);
      size_t size = ComputeDexRegisterMapSizeOf(offset, number_of_dex_registers);
      return DexRegisterMap(region_.Subregion(offset, size));
    }
  }

  InlineInfo GetInlineInfoOf(StackMap stack_map, const StackMapEncoding& encoding) const {
    DCHECK(stack_map.HasInlineInfo(encoding));
    uint32_t offset = stack_map.GetInlineDescriptorOffset(encoding)
                      + GetDexRegisterMapsOffset(encoding);
    uint8_t depth = region_.LoadUnaligned<uint8_t>(offset);
    return InlineInfo(region_.Subregion(offset,
        InlineInfo::kFixedSize + depth * InlineInfo::SingleEntrySize()));
  }

  StackMap GetStackMapForDexPc(uint32_t dex_pc, const StackMapEncoding& encoding) const {
    for (size_t i = 0, e = GetNumberOfStackMaps(); i < e; ++i) {
      StackMap stack_map = GetStackMapAt(i, encoding);
      if (stack_map.GetDexPc(encoding) == dex_pc) {
        return stack_map;
      }
    }
    return StackMap();
  }

  // Searches the stack map list backwards because catch stack maps are stored
  // at the end.
  StackMap GetCatchStackMapForDexPc(uint32_t dex_pc, const StackMapEncoding& encoding) const {
    for (size_t i = GetNumberOfStackMaps(); i > 0; --i) {
      StackMap stack_map = GetStackMapAt(i - 1, encoding);
      if (stack_map.GetDexPc(encoding) == dex_pc) {
        return stack_map;
      }
    }
    return StackMap();
  }

  StackMap GetOsrStackMapForDexPc(uint32_t dex_pc, const StackMapEncoding& encoding) const {
    size_t e = GetNumberOfStackMaps();
    if (e == 0) {
      // There cannot be OSR stack map if there is no stack map.
      return StackMap();
    }
    // Walk over all stack maps. If two consecutive stack maps are identical, then we
    // have found a stack map suitable for OSR.
    for (size_t i = 0; i < e - 1; ++i) {
      StackMap stack_map = GetStackMapAt(i, encoding);
      if (stack_map.GetDexPc(encoding) == dex_pc) {
        StackMap other = GetStackMapAt(i + 1, encoding);
        if (other.GetDexPc(encoding) == dex_pc &&
            other.GetNativePcOffset(encoding) == stack_map.GetNativePcOffset(encoding)) {
          DCHECK_EQ(other.GetDexRegisterMapOffset(encoding),
                    stack_map.GetDexRegisterMapOffset(encoding));
          DCHECK(!stack_map.HasInlineInfo(encoding));
          if (i < e - 2) {
            // Make sure there are not three identical stack maps following each other.
            DCHECK_NE(stack_map.GetNativePcOffset(encoding),
                      GetStackMapAt(i + 2, encoding).GetNativePcOffset(encoding));
          }
          return stack_map;
        }
      }
    }
    return StackMap();
  }

  StackMap GetStackMapForNativePcOffset(uint32_t native_pc_offset,
                                        const StackMapEncoding& encoding) const {
    // TODO: Safepoint stack maps are sorted by native_pc_offset but catch stack
    //       maps are not. If we knew that the method does not have try/catch,
    //       we could do binary search.
    for (size_t i = 0, e = GetNumberOfStackMaps(); i < e; ++i) {
      StackMap stack_map = GetStackMapAt(i, encoding);
      if (stack_map.GetNativePcOffset(encoding) == native_pc_offset) {
        return stack_map;
      }
    }
    return StackMap();
  }

  // Dump this CodeInfo object on `os`.  `code_offset` is the (absolute)
  // native PC of the compiled method and `number_of_dex_registers` the
  // number of Dex virtual registers used in this method.  If
  // `dump_stack_maps` is true, also dump the stack maps and the
  // associated Dex register maps.
  void Dump(VariableIndentationOutputStream* vios,
            uint32_t code_offset,
            uint16_t number_of_dex_registers,
            bool dump_stack_maps) const;

 private:
  static constexpr int kOverallSizeOffset = 0;
  static constexpr int kEncodingInfoOffset = ELEMENT_BYTE_OFFSET_AFTER(OverallSize);
  static constexpr int kNumberOfLocationCatalogEntriesOffset =
      ELEMENT_BYTE_OFFSET_AFTER(EncodingInfo);
  static constexpr int kNumberOfStackMapsOffset =
      ELEMENT_BYTE_OFFSET_AFTER(NumberOfLocationCatalogEntries);
  static constexpr int kStackMaskSizeOffset = ELEMENT_BYTE_OFFSET_AFTER(NumberOfStackMaps);
  static constexpr int kFixedSize = ELEMENT_BYTE_OFFSET_AFTER(StackMaskSize);

  static constexpr int kHasInlineInfoBitOffset = kEncodingInfoOffset * kBitsPerByte;
  static constexpr int kInlineInfoBitOffset = ELEMENT_BIT_OFFSET_AFTER(HasInlineInfo);
  static constexpr int kDexRegisterMapBitOffset = ELEMENT_BIT_OFFSET_AFTER(InlineInfo);
  static constexpr int kDexPcBitOffset = ELEMENT_BIT_OFFSET_AFTER(DexRegisterMap);
  static constexpr int kNativePcBitOffset = ELEMENT_BIT_OFFSET_AFTER(DexPc);
  static constexpr int kRegisterMaskBitOffset = ELEMENT_BIT_OFFSET_AFTER(NativePc);

  static constexpr int kEncodingInfoPastTheEndBitOffset = ELEMENT_BIT_OFFSET_AFTER(RegisterMask);
  static constexpr int kEncodingInfoOverallBitSize =
      kEncodingInfoPastTheEndBitOffset - kHasInlineInfoBitOffset;

  static_assert(kEncodingInfoOverallBitSize <= (sizeof(EncodingInfoType) * kBitsPerByte),
                "art::CodeInfo::EncodingInfoType is too short to hold all encoding info elements.");

  MemoryRegion GetStackMaps(const StackMapEncoding& encoding) const {
    return region_.size() == 0
        ? MemoryRegion()
        : region_.Subregion(GetStackMapsOffset(), GetStackMapsSize(encoding));
  }

  // Compute the size of the Dex register map associated to the stack map at
  // `dex_register_map_offset_in_code_info`.
  size_t ComputeDexRegisterMapSizeOf(uint32_t dex_register_map_offset_in_code_info,
                                     uint16_t number_of_dex_registers) const {
    // Offset where the actual mapping data starts within art::DexRegisterMap.
    size_t location_mapping_data_offset_in_dex_register_map =
        DexRegisterMap::GetLocationMappingDataOffset(number_of_dex_registers);
    // Create a temporary art::DexRegisterMap to be able to call
    // art::DexRegisterMap::GetNumberOfLiveDexRegisters and
    DexRegisterMap dex_register_map_without_locations(
        MemoryRegion(region_.Subregion(dex_register_map_offset_in_code_info,
                                       location_mapping_data_offset_in_dex_register_map)));
    size_t number_of_live_dex_registers =
        dex_register_map_without_locations.GetNumberOfLiveDexRegisters(number_of_dex_registers);
    size_t location_mapping_data_size_in_bits =
        DexRegisterMap::SingleEntrySizeInBits(GetNumberOfLocationCatalogEntries())
        * number_of_live_dex_registers;
    size_t location_mapping_data_size_in_bytes =
        RoundUp(location_mapping_data_size_in_bits, kBitsPerByte) / kBitsPerByte;
    size_t dex_register_map_size =
        location_mapping_data_offset_in_dex_register_map + location_mapping_data_size_in_bytes;
    return dex_register_map_size;
  }

  // Compute the size of a Dex register location catalog starting at offset `origin`
  // in `region_` and containing `number_of_dex_locations` entries.
  size_t ComputeDexRegisterLocationCatalogSize(uint32_t origin,
                                               uint32_t number_of_dex_locations) const {
    // TODO: Ideally, we would like to use art::DexRegisterLocationCatalog::Size or
    // art::DexRegisterLocationCatalog::FindLocationOffset, but the
    // DexRegisterLocationCatalog is not yet built.  Try to factor common code.
    size_t offset = origin + DexRegisterLocationCatalog::kFixedSize;

    // Skip the first `number_of_dex_locations - 1` entries.
    for (uint16_t i = 0; i < number_of_dex_locations; ++i) {
      // Read the first next byte and inspect its first 3 bits to decide
      // whether it is a short or a large location.
      DexRegisterLocationCatalog::ShortLocation first_byte =
          region_.LoadUnaligned<DexRegisterLocationCatalog::ShortLocation>(offset);
      DexRegisterLocation::Kind kind =
          DexRegisterLocationCatalog::ExtractKindFromShortLocation(first_byte);
      if (DexRegisterLocation::IsShortLocationKind(kind)) {
        // Short location.  Skip the current byte.
        offset += DexRegisterLocationCatalog::SingleShortEntrySize();
      } else {
        // Large location.  Skip the 5 next bytes.
        offset += DexRegisterLocationCatalog::SingleLargeEntrySize();
      }
    }
    size_t size = offset - origin;
    return size;
  }

  MemoryRegion region_;
  friend class StackMapStream;
};

#undef ELEMENT_BYTE_OFFSET_AFTER
#undef ELEMENT_BIT_OFFSET_AFTER

}  // namespace art

#endif  // ART_RUNTIME_STACK_MAP_H_
