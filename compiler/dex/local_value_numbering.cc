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

#include "local_value_numbering.h"

#include "mir_field_info.h"
#include "mir_graph.h"

namespace art {

namespace {  // anonymous namespace

// Operations used for value map keys instead of actual opcode.
static constexpr uint16_t kInvokeMemoryVersionBumpOp = Instruction::INVOKE_DIRECT;
static constexpr uint16_t kUnresolvedSFieldOp = Instruction::SPUT;
static constexpr uint16_t kResolvedSFieldOp = Instruction::SGET;
static constexpr uint16_t kUnresolvedIFieldOp = Instruction::IPUT;
static constexpr uint16_t kNonAliasingIFieldOp = Instruction::IGET;
static constexpr uint16_t kAliasingIFieldOp = Instruction::IGET_WIDE;
static constexpr uint16_t kAliasingIFieldStartVersionOp = Instruction::IGET_WIDE;
static constexpr uint16_t kAliasingIFieldBumpVersionOp = Instruction::IGET_OBJECT;
static constexpr uint16_t kArrayAccessLocOp = Instruction::APUT;
static constexpr uint16_t kNonAliasingArrayOp = Instruction::AGET;
static constexpr uint16_t kNonAliasingArrayStartVersionOp = Instruction::AGET_WIDE;
static constexpr uint16_t kAliasingArrayOp = Instruction::AGET_OBJECT;
static constexpr uint16_t kAliasingArrayMemoryVersionOp = Instruction::AGET_BOOLEAN;
static constexpr uint16_t kAliasingArrayBumpVersionOp = Instruction::AGET_BYTE;

}  // anonymous namespace

LocalValueNumbering::LocalValueNumbering(CompilationUnit* cu, ScopedArenaAllocator* allocator)
    : cu_(cu),
      last_value_(0u),
      sreg_value_map_(std::less<uint16_t>(), allocator->Adapter()),
      sreg_wide_value_map_(std::less<uint16_t>(), allocator->Adapter()),
      value_map_(std::less<uint64_t>(), allocator->Adapter()),
      global_memory_version_(0u),
      aliasing_ifield_version_map_(std::less<uint16_t>(), allocator->Adapter()),
      non_aliasing_array_version_map_(std::less<uint16_t>(), allocator->Adapter()),
      field_index_map_(FieldReferenceComparator(), allocator->Adapter()),
      non_aliasing_refs_(std::less<uint16_t>(), allocator->Adapter()),
      non_aliasing_ifields_(NonAliasingIFieldKeyComparator(), allocator->Adapter()),
      escaped_array_refs_(EscapedArrayKeyComparator(), allocator->Adapter()),
      range_checked_(RangeCheckKeyComparator() , allocator->Adapter()),
      null_checked_(std::less<uint16_t>(), allocator->Adapter()) {
  std::fill_n(unresolved_sfield_version_, kFieldTypeCount, 0u);
  std::fill_n(unresolved_ifield_version_, kFieldTypeCount, 0u);
  std::fill_n(aliasing_array_version_, kFieldTypeCount, 0u);
}

uint16_t LocalValueNumbering::GetFieldId(const MirFieldInfo& field_info) {
  FieldReference key = { field_info.DeclaringDexFile(), field_info.DeclaringFieldIndex() };
  auto it = field_index_map_.find(key);
  if (it != field_index_map_.end()) {
    return it->second;
  }
  uint16_t id = field_index_map_.size();
  field_index_map_.Put(key, id);
  return id;
}

uint16_t LocalValueNumbering::MarkNonAliasingNonNull(MIR* mir) {
  uint16_t res = GetOperandValue(mir->ssa_rep->defs[0]);
  SetOperandValue(mir->ssa_rep->defs[0], res);
  DCHECK(null_checked_.find(res) == null_checked_.end());
  null_checked_.insert(res);
  non_aliasing_refs_.insert(res);
  return res;
}

bool LocalValueNumbering::IsNonAliasing(uint16_t reg) {
  return non_aliasing_refs_.find(reg) != non_aliasing_refs_.end();
}

bool LocalValueNumbering::IsNonAliasingIField(uint16_t reg, uint16_t field_id, uint16_t type) {
  if (IsNonAliasing(reg)) {
    return true;
  }
  NonAliasingIFieldKey key = { reg, field_id, type };
  return non_aliasing_ifields_.count(key) != 0u;
}

bool LocalValueNumbering::IsNonAliasingArray(uint16_t reg, uint16_t type) {
  if (IsNonAliasing(reg)) {
    return true;
  }
  EscapedArrayKey key = { reg, type };
  return escaped_array_refs_.count(key) != 0u;
}


void LocalValueNumbering::HandleNullCheck(MIR* mir, uint16_t reg) {
  auto lb = null_checked_.lower_bound(reg);
  if (lb != null_checked_.end() && *lb == reg) {
    if (LIKELY(Good())) {
      if (cu_->verbose) {
        LOG(INFO) << "Removing null check for 0x" << std::hex << mir->offset;
      }
      mir->optimization_flags |= MIR_IGNORE_NULL_CHECK;
    }
  } else {
    null_checked_.insert(lb, reg);
  }
}

void LocalValueNumbering::HandleRangeCheck(MIR* mir, uint16_t array, uint16_t index) {
  RangeCheckKey key = { array, index };
  auto lb = range_checked_.lower_bound(key);
  if (lb != range_checked_.end() && !RangeCheckKeyComparator()(key, *lb)) {
    if (LIKELY(Good())) {
      if (cu_->verbose) {
        LOG(INFO) << "Removing range check for 0x" << std::hex << mir->offset;
      }
      mir->optimization_flags |= MIR_IGNORE_RANGE_CHECK;
    }
  } else {
    // Mark range check completed.
    range_checked_.insert(lb, key);
  }
}

void LocalValueNumbering::HandlePutObject(MIR* mir) {
  // If we're storing a non-aliasing reference, stop tracking it as non-aliasing now.
  uint16_t base = GetOperandValue(mir->ssa_rep->uses[0]);
  HandleEscapingRef(base);
}

void LocalValueNumbering::HandleEscapingRef(uint16_t base) {
  auto it = non_aliasing_refs_.find(base);
  if (it != non_aliasing_refs_.end()) {
    uint64_t iget_key = BuildKey(Instruction::IGET, base, 0u, 0u);
    for (auto iget_it = value_map_.lower_bound(iget_key), iget_end = value_map_.end();
        iget_it != iget_end && EqualOpAndOperand1(iget_it->first, iget_key); ++iget_it) {
      uint16_t field_id = ExtractOperand2(iget_it->first);
      uint16_t type = ExtractModifier(iget_it->first);
      NonAliasingIFieldKey key = { base, field_id, type };
      non_aliasing_ifields_.insert(key);
    }
    uint64_t aget_key = BuildKey(kNonAliasingArrayStartVersionOp, base, 0u, 0u);
    auto aget_it = value_map_.lower_bound(aget_key);
    if (aget_it != value_map_.end() && EqualOpAndOperand1(aget_key, aget_it->first)) {
      DCHECK_EQ(ExtractOperand2(aget_it->first), kNoValue);
      uint16_t type = ExtractModifier(aget_it->first);
      EscapedArrayKey key = { base, type };
      escaped_array_refs_.insert(key);
    }
    non_aliasing_refs_.erase(it);
  }
}

uint16_t LocalValueNumbering::HandleAGet(MIR* mir, uint16_t opcode) {
  // uint16_t type = opcode - Instruction::AGET;
  uint16_t array = GetOperandValue(mir->ssa_rep->uses[0]);
  HandleNullCheck(mir, array);
  uint16_t index = GetOperandValue(mir->ssa_rep->uses[1]);
  HandleRangeCheck(mir, array, index);
  uint16_t type = opcode - Instruction::AGET;
  // Establish value number for loaded register.
  uint16_t res;
  if (IsNonAliasingArray(array, type)) {
    // Get the start version that accounts for aliasing within the array (different index names).
    uint16_t start_version = LookupValue(kNonAliasingArrayStartVersionOp, array, kNoValue, type);
    // Find the current version from the non_aliasing_array_version_map_.
    uint16_t memory_version = start_version;
    auto it = non_aliasing_array_version_map_.find(start_version);
    if (it != non_aliasing_array_version_map_.end()) {
      memory_version = it->second;
    } else {
      // Just use the start_version.
    }
    res = LookupValue(kNonAliasingArrayOp, array, index, memory_version);
  } else {
    // Get the memory version of aliased array accesses of this type.
    uint16_t memory_version = LookupValue(kAliasingArrayMemoryVersionOp, global_memory_version_,
                                          aliasing_array_version_[type], kNoValue);
    res = LookupValue(kAliasingArrayOp, array, index, memory_version);
  }
  if (opcode == Instruction::AGET_WIDE) {
    SetOperandValueWide(mir->ssa_rep->defs[0], res);
  } else {
    SetOperandValue(mir->ssa_rep->defs[0], res);
  }
  return res;
}

void LocalValueNumbering::HandleAPut(MIR* mir, uint16_t opcode) {
  int array_idx = (opcode == Instruction::APUT_WIDE) ? 2 : 1;
  int index_idx = array_idx + 1;
  uint16_t array = GetOperandValue(mir->ssa_rep->uses[array_idx]);
  HandleNullCheck(mir, array);
  uint16_t index = GetOperandValue(mir->ssa_rep->uses[index_idx]);
  HandleRangeCheck(mir, array, index);

  uint16_t type = opcode - Instruction::APUT;
  uint16_t value = (opcode == Instruction::APUT_WIDE)
                   ? GetOperandValueWide(mir->ssa_rep->uses[0])
                   : GetOperandValue(mir->ssa_rep->uses[0]);
  if (IsNonAliasing(array)) {
    // Get the start version that accounts for aliasing within the array (different index values).
    uint16_t start_version = LookupValue(kNonAliasingArrayStartVersionOp, array, kNoValue, type);
    auto it = non_aliasing_array_version_map_.find(start_version);
    uint16_t memory_version = start_version;
    if (it != non_aliasing_array_version_map_.end()) {
      memory_version = it->second;
    }
    // We need to take 4 values (array, index, memory_version, value) into account for bumping
    // the memory version but the key can take only 3. Merge array and index into a location.
    uint16_t array_access_location = LookupValue(kArrayAccessLocOp, array, index, kNoValue);
    // Bump the version, adding to the chain.
    memory_version = LookupValue(kAliasingArrayBumpVersionOp, memory_version,
                                 array_access_location, value);
    non_aliasing_array_version_map_.Overwrite(start_version, memory_version);
    StoreValue(kNonAliasingArrayOp, array, index, memory_version, value);
  } else {
    // Get the memory version based on global_memory_version_ and aliasing_array_version_[type].
    uint16_t memory_version = LookupValue(kAliasingArrayMemoryVersionOp, global_memory_version_,
                                          aliasing_array_version_[type], kNoValue);
    if (HasValue(kAliasingArrayOp, array, index, memory_version, value)) {
      // This APUT can be eliminated, it stores the same value that's already in the field.
      // TODO: Eliminate the APUT.
      return;
    }
    // We need to take 4 values (array, index, memory_version, value) into account for bumping
    // the memory version but the key can take only 3. Merge array and index into a location.
    uint16_t array_access_location = LookupValue(kArrayAccessLocOp, array, index, kNoValue);
    // Bump the version, adding to the chain.
    uint16_t bumped_version = LookupValue(kAliasingArrayBumpVersionOp, memory_version,
                                          array_access_location, value);
    aliasing_array_version_[type] = bumped_version;
    memory_version = LookupValue(kAliasingArrayMemoryVersionOp, global_memory_version_,
                                 bumped_version, kNoValue);
    StoreValue(kAliasingArrayOp, array, index, memory_version, value);

    // Clear escaped array refs for this type.
    EscapedArrayKey array_key = { type, 0u };
    auto it = escaped_array_refs_.lower_bound(array_key), end = escaped_array_refs_.end();
    while (it != end && it->type == type) {
      it = escaped_array_refs_.erase(it);
    }
  }
}

uint16_t LocalValueNumbering::HandleIGet(MIR* mir, uint16_t opcode) {
  uint16_t base = GetOperandValue(mir->ssa_rep->uses[0]);
  HandleNullCheck(mir, base);
  const MirFieldInfo& field_info = cu_->mir_graph->GetIFieldLoweringInfo(mir);
  uint16_t res;
  if (!field_info.IsResolved() || field_info.IsVolatile()) {
    // Volatile fields always get a new memory version; field id is irrelevant.
    // Unresolved fields may be volatile, so handle them as such to be safe.
    // Use result s_reg - will be unique.
    res = LookupValue(kNoValue, mir->ssa_rep->defs[0], kNoValue, kNoValue);
  } else {
    uint16_t type = opcode - Instruction::IGET;
    uint16_t field_id = GetFieldId(field_info);
    if (IsNonAliasingIField(base, field_id, type)) {
      res = LookupValue(kNonAliasingIFieldOp, base, field_id, type);
    } else {
      // Get the start version that accounts for aliasing with unresolved fields of the same type
      // and make it unique for the field by including the field_id.
      uint16_t start_version = LookupValue(kAliasingIFieldStartVersionOp, global_memory_version_,
                                           unresolved_ifield_version_[type], field_id);
      // Find the current version from the aliasing_ifield_version_map_.
      uint16_t memory_version = start_version;
      auto version_it = aliasing_ifield_version_map_.find(start_version);
      if (version_it != aliasing_ifield_version_map_.end()) {
        memory_version = version_it->second;
      } else {
        // Just use the start_version.
      }
      res = LookupValue(kAliasingIFieldOp, base, field_id, memory_version);
    }
  }
  if (opcode == Instruction::IGET_WIDE) {
    SetOperandValueWide(mir->ssa_rep->defs[0], res);
  } else {
    SetOperandValue(mir->ssa_rep->defs[0], res);
  }
  return res;
}

void LocalValueNumbering::HandleIPut(MIR* mir, uint16_t opcode) {
  uint16_t type = opcode - Instruction::IPUT;
  int base_reg = (opcode == Instruction::IPUT_WIDE) ? 2 : 1;
  uint16_t base = GetOperandValue(mir->ssa_rep->uses[base_reg]);
  HandleNullCheck(mir, base);
  const MirFieldInfo& field_info = cu_->mir_graph->GetIFieldLoweringInfo(mir);
  if (!field_info.IsResolved()) {
    // Unresolved fields always alias with everything of the same type.
    // Use mir->offset as modifier; without elaborate inlining, it will be unique.
    unresolved_ifield_version_[type] =
        LookupValue(kUnresolvedIFieldOp, kNoValue, kNoValue, mir->offset);

    // Treat fields of escaped references of the same type as potentially modified.
    NonAliasingIFieldKey key = { type, 0u, 0u };  // lowest possible key of this type.
    auto it = non_aliasing_ifields_.lower_bound(key), end = non_aliasing_ifields_.end();
    while (it != end && it->type == type) {
      it = non_aliasing_ifields_.erase(it);
    }
  } else if (field_info.IsVolatile()) {
    // Nothing to do, resolved volatile fields always get a new memory version anyway and
    // can't alias with resolved non-volatile fields.
  } else {
    uint16_t field_id = GetFieldId(field_info);
    uint16_t value = (opcode == Instruction::IPUT_WIDE)
                     ? GetOperandValueWide(mir->ssa_rep->uses[0])
                     : GetOperandValue(mir->ssa_rep->uses[0]);
    if (IsNonAliasing(base)) {
      StoreValue(kNonAliasingIFieldOp, base, field_id, type, value);
    } else {
      // Get the start version that accounts for aliasing with unresolved fields of the same type
      // and make it unique for the field by including the field_id.
      uint16_t start_version = LookupValue(kAliasingIFieldStartVersionOp, global_memory_version_,
                                           unresolved_ifield_version_[type], field_id);
      // Find the old version from the aliasing_ifield_version_map_.
      uint16_t old_version = start_version;
      auto version_it = aliasing_ifield_version_map_.find(start_version);
      if (version_it != aliasing_ifield_version_map_.end()) {
        old_version = version_it->second;
      }
      // Check if the field currently contains the value, making this a NOP.
      if (HasValue(kAliasingIFieldOp, base, field_id, old_version, value)) {
        // This IPUT can be eliminated, it stores the same value that's already in the field.
        // TODO: Eliminate the IPUT.
        return;
      }
      // Bump the version, adding to the chain started by start_version.
      uint16_t memory_version = LookupValue(kAliasingIFieldBumpVersionOp, old_version, base, value);
      // Update the aliasing_ifield_version_map_ so that HandleIGet() can get the memory_version
      // without knowing the values used to build the chain.
      aliasing_ifield_version_map_.Overwrite(start_version, memory_version);
      StoreValue(kAliasingIFieldOp, base, field_id, memory_version, value);

      // Clear non-aliasing fields for this field_id.
      NonAliasingIFieldKey field_key = { type, field_id, 0u };
      auto it = non_aliasing_ifields_.lower_bound(field_key), end = non_aliasing_ifields_.end();
      while (it != end && it->field_id == field_id) {
        DCHECK_EQ(type, it->type);
        it = non_aliasing_ifields_.erase(it);
      }
    }
  }
}

uint16_t LocalValueNumbering::HandleSGet(MIR* mir, uint16_t opcode) {
  const MirFieldInfo& field_info = cu_->mir_graph->GetSFieldLoweringInfo(mir);
  uint16_t res;
  if (!field_info.IsResolved() || field_info.IsVolatile()) {
    // Volatile fields always get a new memory version; field id is irrelevant.
    // Unresolved fields may be volatile, so handle them as such to be safe.
    // Use result s_reg - will be unique.
    res = LookupValue(kNoValue, mir->ssa_rep->defs[0], kNoValue, kNoValue);
  } else {
    uint16_t field_id = GetFieldId(field_info);
    // Resolved non-volatile static fields can alias with non-resolved fields of the same type,
    // so we need to use unresolved_sfield_version_[type] in addition to global_memory_version_
    // to determine the version of the field.
    uint16_t type = opcode - Instruction::SGET;
    res = LookupValue(kResolvedSFieldOp, field_id,
                      unresolved_sfield_version_[type], global_memory_version_);
  }
  if (opcode == Instruction::SGET_WIDE) {
    SetOperandValueWide(mir->ssa_rep->defs[0], res);
  } else {
    SetOperandValue(mir->ssa_rep->defs[0], res);
  }
  return res;
}

void LocalValueNumbering::HandleSPut(MIR* mir, uint16_t opcode) {
  uint16_t type = opcode - Instruction::SPUT;
  const MirFieldInfo& field_info = cu_->mir_graph->GetSFieldLoweringInfo(mir);
  if (!field_info.IsResolved()) {
    // Unresolved fields always alias with everything of the same type.
    // Use mir->offset as modifier; without elaborate inlining, it will be unique.
    unresolved_sfield_version_[type] =
        LookupValue(kUnresolvedSFieldOp, kNoValue, kNoValue, mir->offset);
  } else if (field_info.IsVolatile()) {
    // Nothing to do, resolved volatile fields always get a new memory version anyway and
    // can't alias with resolved non-volatile fields.
  } else {
    uint16_t field_id = GetFieldId(field_info);
    uint16_t value = (opcode == Instruction::SPUT_WIDE)
                     ? GetOperandValueWide(mir->ssa_rep->uses[0])
                     : GetOperandValue(mir->ssa_rep->uses[0]);
    // Resolved non-volatile static fields can alias with non-resolved fields of the same type,
    // so we need to use unresolved_sfield_version_[type] in addition to global_memory_version_
    // to determine the version of the field.
    uint16_t type = opcode - Instruction::SGET;
    StoreValue(kResolvedSFieldOp, field_id,
               unresolved_sfield_version_[type], global_memory_version_, value);
  }
}

uint16_t LocalValueNumbering::GetValueNumber(MIR* mir) {
  uint16_t res = kNoValue;
  uint16_t opcode = mir->dalvikInsn.opcode;
  switch (opcode) {
    case Instruction::NOP:
    case Instruction::RETURN_VOID:
    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT:
    case Instruction::RETURN_WIDE:
    case Instruction::MONITOR_ENTER:
    case Instruction::MONITOR_EXIT:
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
    case Instruction::CHECK_CAST:
    case Instruction::THROW:
    case Instruction::FILL_ARRAY_DATA:
    case Instruction::PACKED_SWITCH:
    case Instruction::SPARSE_SWITCH:
    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ:
    case kMirOpFusedCmplFloat:
    case kMirOpFusedCmpgFloat:
    case kMirOpFusedCmplDouble:
    case kMirOpFusedCmpgDouble:
    case kMirOpFusedCmpLong:
      // Nothing defined - take no action.
      break;

    case Instruction::FILLED_NEW_ARRAY:
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      // Nothing defined but the result will be unique and non-null.
      if (mir->next != nullptr && mir->next->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) {
        MarkNonAliasingNonNull(mir->next);
        // TUNING: We could track value names stored in the array.
        // The MOVE_RESULT_OBJECT will be processed next and we'll return the value name then.
      }
      // All args escaped (if references).
      for (size_t i = 0u, count = mir->ssa_rep->num_uses; i != count; ++i) {
        uint16_t reg = GetOperandValue(mir->ssa_rep->uses[i]);
        HandleEscapingRef(reg);
      }
      break;

    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE: {
        // Nothing defined but handle the null check.
        uint16_t reg = GetOperandValue(mir->ssa_rep->uses[0]);
        HandleNullCheck(mir, reg);
      }
      // Intentional fall-through.
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
      if ((mir->optimization_flags & MIR_INLINED) == 0) {
        // Use mir->offset as modifier; without elaborate inlining, it will be unique.
        global_memory_version_ = LookupValue(kInvokeMemoryVersionBumpOp, 0u, 0u, mir->offset);
        // Make ref args aliasing.
        for (size_t i = 0u, count = mir->ssa_rep->num_uses; i != count; ++i) {
          uint16_t reg = GetOperandValue(mir->ssa_rep->uses[i]);
          non_aliasing_refs_.erase(reg);
        }
        // All fields of escaped references need to be treated as potentially modified.
        non_aliasing_ifields_.clear();
        // Array elements may also have been modified via escaped array refs.
        escaped_array_refs_.clear();
      }
      break;

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_OBJECT:
    case Instruction::INSTANCE_OF:
      // 1 result, treat as unique each time, use result s_reg - will be unique.
      res = GetOperandValue(mir->ssa_rep->defs[0]);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      break;
    case Instruction::MOVE_EXCEPTION:
    case Instruction::NEW_INSTANCE:
    case Instruction::CONST_CLASS:
    case Instruction::NEW_ARRAY:
      // 1 result, treat as unique each time, use result s_reg - will be unique.
      res = MarkNonAliasingNonNull(mir);
      break;
    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      // These strings are internalized, so assign value based on the string pool index.
      res = LookupValue(Instruction::CONST_STRING, Low16Bits(mir->dalvikInsn.vB),
                        High16Bits(mir->dalvikInsn.vB), 0);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      null_checked_.insert(res);  // May already be there.
      // NOTE: Hacking the contents of an internalized string via reflection is possible
      // but the behavior is undefined. Therefore, we consider the string constant and
      // the reference non-aliasing.
      // TUNING: We could keep this property even if the reference "escapes".
      non_aliasing_refs_.insert(res);  // May already be there.
      break;
    case Instruction::MOVE_RESULT_WIDE:
      // 1 wide result, treat as unique each time, use result s_reg - will be unique.
      res = GetOperandValueWide(mir->ssa_rep->defs[0]);
      SetOperandValueWide(mir->ssa_rep->defs[0], res);
      break;

    case kMirOpPhi:
      /*
       * Because we'll only see phi nodes at the beginning of an extended basic block,
       * we can ignore them.  Revisit if we shift to global value numbering.
       */
      break;

    case Instruction::MOVE:
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_16:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_OBJECT_FROM16:
    case kMirOpCopy:
      // Just copy value number of source to value number of result.
      res = GetOperandValue(mir->ssa_rep->uses[0]);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      break;

    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_16:
    case Instruction::MOVE_WIDE_FROM16:
      // Just copy value number of source to value number of result.
      res = GetOperandValueWide(mir->ssa_rep->uses[0]);
      SetOperandValueWide(mir->ssa_rep->defs[0], res);
      break;

    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16:
      res = LookupValue(Instruction::CONST, Low16Bits(mir->dalvikInsn.vB),
                        High16Bits(mir->dalvikInsn.vB), 0);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      break;

    case Instruction::CONST_HIGH16:
      res = LookupValue(Instruction::CONST, 0, mir->dalvikInsn.vB, 0);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      break;

    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32: {
        uint16_t low_res = LookupValue(Instruction::CONST, Low16Bits(mir->dalvikInsn.vB),
                                       High16Bits(mir->dalvikInsn.vB >> 16), 1);
        uint16_t high_res;
        if (mir->dalvikInsn.vB & 0x80000000) {
          high_res = LookupValue(Instruction::CONST, 0xffff, 0xffff, 2);
        } else {
          high_res = LookupValue(Instruction::CONST, 0, 0, 2);
        }
        res = LookupValue(Instruction::CONST, low_res, high_res, 3);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::CONST_WIDE: {
        uint32_t low_word = Low32Bits(mir->dalvikInsn.vB_wide);
        uint32_t high_word = High32Bits(mir->dalvikInsn.vB_wide);
        uint16_t low_res = LookupValue(Instruction::CONST, Low16Bits(low_word),
                                       High16Bits(low_word), 1);
        uint16_t high_res = LookupValue(Instruction::CONST, Low16Bits(high_word),
                                       High16Bits(high_word), 2);
        res = LookupValue(Instruction::CONST, low_res, high_res, 3);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::CONST_WIDE_HIGH16: {
        uint16_t low_res = LookupValue(Instruction::CONST, 0, 0, 1);
        uint16_t high_res = LookupValue(Instruction::CONST, 0, Low16Bits(mir->dalvikInsn.vB), 2);
        res = LookupValue(Instruction::CONST, low_res, high_res, 3);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::ARRAY_LENGTH:
    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
    case Instruction::NEG_FLOAT:
    case Instruction::INT_TO_BYTE:
    case Instruction::INT_TO_SHORT:
    case Instruction::INT_TO_CHAR:
    case Instruction::INT_TO_FLOAT:
    case Instruction::FLOAT_TO_INT: {
        // res = op + 1 operand
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        res = LookupValue(opcode, operand1, kNoValue, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::LONG_TO_FLOAT:
    case Instruction::LONG_TO_INT:
    case Instruction::DOUBLE_TO_FLOAT:
    case Instruction::DOUBLE_TO_INT: {
        // res = op + 1 wide operand
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        res = LookupValue(opcode, operand1, kNoValue, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;


    case Instruction::DOUBLE_TO_LONG:
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
    case Instruction::NEG_DOUBLE: {
        // wide res = op + 1 wide operand
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        res = LookupValue(opcode, operand1, kNoValue, kNoValue);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::FLOAT_TO_DOUBLE:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::INT_TO_DOUBLE:
    case Instruction::INT_TO_LONG: {
        // wide res = op + 1 operand
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        res = LookupValue(opcode, operand1, kNoValue, kNoValue);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
    case Instruction::CMP_LONG: {
        // res = op + 2 wide operands
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValueWide(mir->ssa_rep->uses[2]);
        res = LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::CMPG_FLOAT:
    case Instruction::CMPL_FLOAT:
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR: {
        // res = op + 2 operands
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValue(mir->ssa_rep->uses[1]);
        res = LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR: {
        // wide res = op + 2 wide operands
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValueWide(mir->ssa_rep->uses[2]);
        res = LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR: {
        // wide res = op + 1 wide operand + 1 operand
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValue(mir->ssa_rep->uses[2]);
        res = LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR: {
        // res = op + 2 operands
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValue(mir->ssa_rep->uses[1]);
        res = LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::RSUB_INT:
    case Instruction::ADD_INT_LIT16:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8:
    case Instruction::USHR_INT_LIT8: {
        // Same as res = op + 2 operands, except use vC as operand 2
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        uint16_t operand2 = LookupValue(Instruction::CONST, mir->dalvikInsn.vC, 0, 0);
        res = LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::AGET_OBJECT:
    case Instruction::AGET:
    case Instruction::AGET_WIDE:
    case Instruction::AGET_BOOLEAN:
    case Instruction::AGET_BYTE:
    case Instruction::AGET_CHAR:
    case Instruction::AGET_SHORT:
      res = HandleAGet(mir, opcode);
      break;

    case Instruction::APUT_OBJECT:
      HandlePutObject(mir);
      // Intentional fall-through.
    case Instruction::APUT:
    case Instruction::APUT_WIDE:
    case Instruction::APUT_BYTE:
    case Instruction::APUT_BOOLEAN:
    case Instruction::APUT_SHORT:
    case Instruction::APUT_CHAR:
      HandleAPut(mir, opcode);
      break;

    case Instruction::IGET_OBJECT:
    case Instruction::IGET:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT:
      res = HandleIGet(mir, opcode);
      break;

    case Instruction::IPUT_OBJECT:
      HandlePutObject(mir);
      // Intentional fall-through.
    case Instruction::IPUT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT:
      HandleIPut(mir, opcode);
      break;

    case Instruction::SGET_OBJECT:
    case Instruction::SGET:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
      res = HandleSGet(mir, opcode);
      break;

    case Instruction::SPUT_OBJECT:
      HandlePutObject(mir);
      // Intentional fall-through.
    case Instruction::SPUT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
      HandleSPut(mir, opcode);
      break;
  }
  return res;
}

}    // namespace art
