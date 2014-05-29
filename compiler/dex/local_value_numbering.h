/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_LOCAL_VALUE_NUMBERING_H_
#define ART_COMPILER_DEX_LOCAL_VALUE_NUMBERING_H_

#include <memory>

#include "compiler_internals.h"
#include "utils/scoped_arena_allocator.h"
#include "utils/scoped_arena_containers.h"

namespace art {

class DexFile;
class MirFieldInfo;

class LocalValueNumbering {
 public:
  LocalValueNumbering(CompilationUnit* cu, ScopedArenaAllocator* allocator);

  uint16_t GetValueNumber(MIR* mir);

  // LocalValueNumbering should be allocated on the ArenaStack (or the native stack).
  static void* operator new(size_t size, ScopedArenaAllocator* allocator) {
    return allocator->Alloc(sizeof(LocalValueNumbering), kArenaAllocMIR);
  }

  // Allow delete-expression to destroy a LocalValueNumbering object without deallocation.
  static void operator delete(void* ptr) { UNUSED(ptr); }

  // Checks that the value names didn't overflow.
  bool Good() const {
    return last_value_ < kNoValue;
  }

 private:
  static constexpr uint16_t kNoValue = 0xffffu;

  // Field types correspond to the ordering of GET/PUT instructions; this order is the same
  // for IGET, IPUT, SGET, SPUT, AGET and APUT:
  // op         0
  // op_WIDE    1
  // op_OBJECT  2
  // op_BOOLEAN 3
  // op_BYTE    4
  // op_CHAR    5
  // op_SHORT   6
  static constexpr size_t kFieldTypeCount = 7;

  // FieldReference represents a unique resolved field.
  struct FieldReference {
    const DexFile* dex_file;
    uint16_t field_idx;
  };

  struct FieldReferenceComparator {
    bool operator()(const FieldReference& lhs, const FieldReference& rhs) const {
      if (lhs.field_idx != rhs.field_idx) {
        return lhs.field_idx < rhs.field_idx;
      }
      return lhs.dex_file < rhs.dex_file;
    }
  };

  // Maps field key to field id for resolved fields.
  typedef ScopedArenaSafeMap<FieldReference, uint32_t, FieldReferenceComparator> FieldIndexMap;

  struct RangeCheckKey {
    uint16_t array;
    uint16_t index;
  };

  struct RangeCheckKeyComparator {
    bool operator()(const RangeCheckKey& lhs, const RangeCheckKey& rhs) const {
      if (lhs.array != rhs.array) {
        return lhs.array < rhs.array;
      }
      return lhs.index < rhs.index;
    }
  };

  typedef ScopedArenaSet<RangeCheckKey, RangeCheckKeyComparator> RangeCheckSet;

  typedef ScopedArenaSafeMap<uint16_t, uint16_t> AliasingIFieldVersionMap;
  typedef ScopedArenaSafeMap<uint16_t, uint16_t> NonAliasingArrayVersionMap;

  struct NonAliasingIFieldKey {
    uint16_t base;
    uint16_t field_id;
    uint16_t type;
  };

  struct NonAliasingIFieldKeyComparator {
    bool operator()(const NonAliasingIFieldKey& lhs, const NonAliasingIFieldKey& rhs) const {
      // Compare the type first. This allows iterating across all the entries for a certain type
      // as needed when we need to purge them for an unresolved field IPUT.
      if (lhs.type != rhs.type) {
        return lhs.type < rhs.type;
      }
      // Compare the field second. This allows iterating across all the entries for a certain
      // field as needed when we need to purge them for an aliasing field IPUT.
      if (lhs.field_id != rhs.field_id) {
        return lhs.field_id < rhs.field_id;
      }
      // Compare the base last.
      return lhs.base < rhs.base;
    }
  };

  // Set of instance fields still holding non-aliased values after the base has been stored.
  typedef ScopedArenaSet<NonAliasingIFieldKey, NonAliasingIFieldKeyComparator> NonAliasingFieldSet;

  struct EscapedArrayKey {
    uint16_t base;
    uint16_t type;
  };

  struct EscapedArrayKeyComparator {
    bool operator()(const EscapedArrayKey& lhs, const EscapedArrayKey& rhs) const {
      // Compare the type first. This allows iterating across all the entries for a certain type
      // as needed when we need to purge them for an unresolved field APUT.
      if (lhs.type != rhs.type) {
        return lhs.type < rhs.type;
      }
      // Compare the base last.
      return lhs.base < rhs.base;
    }
  };

  // Set of previously non-aliasing array refs that escaped.
  typedef ScopedArenaSet<EscapedArrayKey, EscapedArrayKeyComparator> EscapedArraySet;

  // Key is s_reg, value is value name.
  typedef ScopedArenaSafeMap<uint16_t, uint16_t> SregValueMap;
  // Key is concatenation of opcode, operand1, operand2 and modifier, value is value name.
  typedef ScopedArenaSafeMap<uint64_t, uint16_t> ValueMap;
  // Key represents a memory address, value is generation.
  // A set of value names.
  typedef ScopedArenaSet<uint16_t> ValueNameSet;

  static uint64_t BuildKey(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier) {
    return (static_cast<uint64_t>(op) << 48 | static_cast<uint64_t>(operand1) << 32 |
            static_cast<uint64_t>(operand2) << 16 | static_cast<uint64_t>(modifier));
  };

  static uint16_t ExtractOp(uint64_t key) {
    return static_cast<uint16_t>(key >> 48);
  }

  static uint16_t ExtractOperand1(uint64_t key) {
    return static_cast<uint16_t>(key >> 32);
  }

  static uint16_t ExtractOperand2(uint64_t key) {
    return static_cast<uint16_t>(key >> 16);
  }

  static uint16_t ExtractModifier(uint64_t key) {
    return static_cast<uint16_t>(key);
  }

  static bool EqualOpAndOperand1(uint64_t key1, uint64_t key2) {
    return static_cast<uint32_t>(key1 >> 32) == static_cast<uint32_t>(key2 >> 32);
  }

  uint16_t LookupValue(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier) {
    uint16_t res;
    uint64_t key = BuildKey(op, operand1, operand2, modifier);
    ValueMap::iterator it = value_map_.find(key);
    if (it != value_map_.end()) {
      res = it->second;
    } else {
      ++last_value_;
      res = last_value_;
      value_map_.Put(key, res);
    }
    return res;
  };

  void StoreValue(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier,
                  uint16_t value) {
    uint64_t key = BuildKey(op, operand1, operand2, modifier);
    value_map_.Overwrite(key, value);
  }

  bool HasValue(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier,
                uint16_t value) const {
    uint64_t key = BuildKey(op, operand1, operand2, modifier);
    ValueMap::const_iterator it = value_map_.find(key);
    return (it != value_map_.end() && it->second == value);
  };

  bool ValueExists(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier) const {
    uint64_t key = BuildKey(op, operand1, operand2, modifier);
    ValueMap::const_iterator it = value_map_.find(key);
    return (it != value_map_.end());
  };

  void SetOperandValue(uint16_t s_reg, uint16_t value) {
    SregValueMap::iterator it = sreg_value_map_.find(s_reg);
    if (it != sreg_value_map_.end()) {
      DCHECK_EQ(it->second, value);
    } else {
      sreg_value_map_.Put(s_reg, value);
    }
  };

  uint16_t GetOperandValue(int s_reg) {
    uint16_t res = kNoValue;
    SregValueMap::iterator it = sreg_value_map_.find(s_reg);
    if (it != sreg_value_map_.end()) {
      res = it->second;
    } else {
      // First use
      res = LookupValue(kNoValue, s_reg, kNoValue, kNoValue);
      sreg_value_map_.Put(s_reg, res);
    }
    return res;
  };

  void SetOperandValueWide(uint16_t s_reg, uint16_t value) {
    SregValueMap::iterator it = sreg_wide_value_map_.find(s_reg);
    if (it != sreg_wide_value_map_.end()) {
      DCHECK_EQ(it->second, value);
    } else {
      sreg_wide_value_map_.Put(s_reg, value);
    }
  };

  uint16_t GetOperandValueWide(int s_reg) {
    uint16_t res = kNoValue;
    SregValueMap::iterator it = sreg_wide_value_map_.find(s_reg);
    if (it != sreg_wide_value_map_.end()) {
      res = it->second;
    } else {
      // First use
      res = LookupValue(kNoValue, s_reg, kNoValue, kNoValue);
      sreg_wide_value_map_.Put(s_reg, res);
    }
    return res;
  };

  uint16_t GetFieldId(const MirFieldInfo& field_info);
  uint16_t MarkNonAliasingNonNull(MIR* mir);
  bool IsNonAliasing(uint16_t reg);
  bool IsNonAliasingIField(uint16_t reg, uint16_t field_id, uint16_t type);
  bool IsNonAliasingArray(uint16_t reg, uint16_t type);
  void HandleNullCheck(MIR* mir, uint16_t reg);
  void HandleRangeCheck(MIR* mir, uint16_t array, uint16_t index);
  void HandlePutObject(MIR* mir);
  void HandleEscapingRef(uint16_t base);
  uint16_t HandleAGet(MIR* mir, uint16_t opcode);
  void HandleAPut(MIR* mir, uint16_t opcode);
  uint16_t HandleIGet(MIR* mir, uint16_t opcode);
  void HandleIPut(MIR* mir, uint16_t opcode);
  uint16_t HandleSGet(MIR* mir, uint16_t opcode);
  void HandleSPut(MIR* mir, uint16_t opcode);

  CompilationUnit* const cu_;

  // We have 32-bit last_value_ so that we can detect when we run out of value names, see Good().
  // We usually don't check Good() until the end of LVN unless we're about to modify code.
  uint32_t last_value_;

  SregValueMap sreg_value_map_;
  SregValueMap sreg_wide_value_map_;
  ValueMap value_map_;

  // Data for dealing with memory clobbering and store/load aliasing.
  uint16_t global_memory_version_;
  uint16_t unresolved_sfield_version_[kFieldTypeCount];
  uint16_t unresolved_ifield_version_[kFieldTypeCount];
  uint16_t aliasing_array_version_[kFieldTypeCount];
  AliasingIFieldVersionMap aliasing_ifield_version_map_;
  NonAliasingArrayVersionMap non_aliasing_array_version_map_;
  FieldIndexMap field_index_map_;
  // Value names of references to objects that cannot be reached through a different value name.
  ValueNameSet non_aliasing_refs_;
  // Instance fields still holding non-aliased values after the base has escaped.
  NonAliasingFieldSet non_aliasing_ifields_;
  // Previously non-aliasing array refs that escaped but can still be used for non-aliasing AGET.
  EscapedArraySet escaped_array_refs_;

  // Range check and null check elimination.
  RangeCheckSet range_checked_;
  ValueNameSet null_checked_;

  DISALLOW_COPY_AND_ASSIGN(LocalValueNumbering);
};

}  // namespace art

#endif  // ART_COMPILER_DEX_LOCAL_VALUE_NUMBERING_H_
