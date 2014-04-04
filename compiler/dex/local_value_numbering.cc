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

uint16_t LocalValueNumbering::GetFieldId(const DexFile* dex_file, uint16_t field_idx) {
  FieldReference key = { dex_file, field_idx };
  auto it = field_index_map_.find(key);
  if (it != field_index_map_.end()) {
    return it->second;
  }
  uint16_t id = field_index_map_.size();
  field_index_map_.Put(key, id);
  return id;
}

void LocalValueNumbering::AdvanceGlobalMemory() {
  // See AdvanceMemoryVersion() for explanation.
  global_memory_version_ = next_memory_version_;
  ++next_memory_version_;
}

uint16_t LocalValueNumbering::GetMemoryVersion(uint16_t base, uint16_t field, uint16_t type) {
  // See AdvanceMemoryVersion() for explanation.
  MemoryVersionKey key = { base, field, type };
  MemoryVersionMap::iterator it = memory_version_map_.find(key);
  uint16_t memory_version = (it != memory_version_map_.end()) ? it->second : 0u;
  if (base != NO_VALUE && non_aliasing_refs_.find(base) == non_aliasing_refs_.end()) {
    // Check modifications by potentially aliased access.
    MemoryVersionKey aliased_access_key = { NO_VALUE, field, type };
    auto aa_it = memory_version_map_.find(aliased_access_key);
    if (aa_it != memory_version_map_.end() && aa_it->second > memory_version) {
      memory_version = aa_it->second;
    }
    memory_version = std::max(memory_version, global_memory_version_);
  } else if (base != NO_VALUE) {
    // Ignore global_memory_version_ for access via unique references.
  } else {
    memory_version = std::max(memory_version, global_memory_version_);
  }
  return memory_version;
};

uint16_t LocalValueNumbering::AdvanceMemoryVersion(uint16_t base, uint16_t field, uint16_t type) {
  // When we read the same value from memory, we want to assign the same value name to it.
  // However, we need to be careful not to assign the same value name if the memory location
  // may have been written to between the reads. To avoid that we do "memory versioning".
  //
  // For each write to a memory location (instance field, static field, array element) we assign
  // a new memory version number to the location identified by the value name of the base register,
  // the field id and type, or "{ base, field, type }". For static fields the "base" is NO_VALUE
  // since they are not accessed via a reference. For arrays the "field" is NO_VALUE since they
  // don't have a field id.
  //
  // To account for the possibility of aliased access to the same memory location via different
  // "base", we also store the memory version number with the key "{ NO_VALUE, field, type }"
  // if "base" is an aliasing reference and check it in GetMemoryVersion() on reads via
  // aliasing references. A global memory version is set for method calls as a method can
  // potentially write to any memory location accessed via an aliasing reference.

  uint16_t result = next_memory_version_;
  ++next_memory_version_;
  MemoryVersionKey key = { base, field, type };
  memory_version_map_.Overwrite(key, result);
  if (base != NO_VALUE && non_aliasing_refs_.find(base) == non_aliasing_refs_.end()) {
    // Advance memory version for aliased access.
    MemoryVersionKey aliased_access_key = { NO_VALUE, field, type };
    memory_version_map_.Overwrite(aliased_access_key, result);
  }
  return result;
};

uint16_t LocalValueNumbering::MarkNonAliasingNonNull(MIR* mir) {
  uint16_t res = GetOperandValue(mir->ssa_rep->defs[0]);
  SetOperandValue(mir->ssa_rep->defs[0], res);
  DCHECK(null_checked_.find(res) == null_checked_.end());
  null_checked_.insert(res);
  non_aliasing_refs_.insert(res);
  return res;
}

void LocalValueNumbering::MakeArgsAliasing(MIR* mir) {
  for (size_t i = 0u, count = mir->ssa_rep->num_uses; i != count; ++i) {
    uint16_t reg = GetOperandValue(mir->ssa_rep->uses[i]);
    non_aliasing_refs_.erase(reg);
  }
}

void LocalValueNumbering::HandleNullCheck(MIR* mir, uint16_t reg) {
  if (null_checked_.find(reg) != null_checked_.end()) {
    if (cu_->verbose) {
      LOG(INFO) << "Removing null check for 0x" << std::hex << mir->offset;
    }
    mir->optimization_flags |= MIR_IGNORE_NULL_CHECK;
  } else {
    null_checked_.insert(reg);
  }
}

void LocalValueNumbering::HandleRangeCheck(MIR* mir, uint16_t array, uint16_t index) {
  if (ValueExists(ARRAY_REF, array, index, NO_VALUE)) {
    if (cu_->verbose) {
      LOG(INFO) << "Removing range check for 0x" << std::hex << mir->offset;
    }
    mir->optimization_flags |= MIR_IGNORE_RANGE_CHECK;
  }
  // Use side effect to note range check completed.
  (void)LookupValue(ARRAY_REF, array, index, NO_VALUE);
}

void LocalValueNumbering::HandlePutObject(MIR* mir) {
  // If we're storing a non-aliasing reference, stop tracking it as non-aliasing now.
  uint16_t base = GetOperandValue(mir->ssa_rep->uses[0]);
  non_aliasing_refs_.erase(base);
}

uint16_t LocalValueNumbering::GetValueNumber(MIR* mir) {
  uint16_t res = NO_VALUE;
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
        // The MOVE_RESULT_OBJECT will be processed next and we'll return the value name then.
      }
      MakeArgsAliasing(mir);
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
        AdvanceGlobalMemory();
        MakeArgsAliasing(mir);
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
    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
    case Instruction::CONST_CLASS:
    case Instruction::NEW_ARRAY:
      // 1 result, treat as unique each time, use result s_reg - will be unique.
      res = MarkNonAliasingNonNull(mir);
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
                        High16Bits(mir->dalvikInsn.vB >> 16), 0);
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
        res = LookupValue(opcode, operand1, NO_VALUE, NO_VALUE);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::LONG_TO_FLOAT:
    case Instruction::LONG_TO_INT:
    case Instruction::DOUBLE_TO_FLOAT:
    case Instruction::DOUBLE_TO_INT: {
        // res = op + 1 wide operand
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        res = LookupValue(opcode, operand1, NO_VALUE, NO_VALUE);
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
        res = LookupValue(opcode, operand1, NO_VALUE, NO_VALUE);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::FLOAT_TO_DOUBLE:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::INT_TO_DOUBLE:
    case Instruction::INT_TO_LONG: {
        // wide res = op + 1 operand
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        res = LookupValue(opcode, operand1, NO_VALUE, NO_VALUE);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
    case Instruction::CMP_LONG: {
        // res = op + 2 wide operands
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValueWide(mir->ssa_rep->uses[2]);
        res = LookupValue(opcode, operand1, operand2, NO_VALUE);
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
        res = LookupValue(opcode, operand1, operand2, NO_VALUE);
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
        res = LookupValue(opcode, operand1, operand2, NO_VALUE);
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
        uint16_t operand2 = GetOperandValueWide(mir->ssa_rep->uses[2]);
        res = LookupValue(opcode, operand1, operand2, NO_VALUE);
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
        res = LookupValue(opcode, operand1, operand2, NO_VALUE);
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
        res = LookupValue(opcode, operand1, operand2, NO_VALUE);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::AGET_OBJECT:
    case Instruction::AGET:
    case Instruction::AGET_WIDE:
    case Instruction::AGET_BOOLEAN:
    case Instruction::AGET_BYTE:
    case Instruction::AGET_CHAR:
    case Instruction::AGET_SHORT: {
        uint16_t type = opcode - Instruction::AGET;
        uint16_t array = GetOperandValue(mir->ssa_rep->uses[0]);
        HandleNullCheck(mir, array);
        uint16_t index = GetOperandValue(mir->ssa_rep->uses[1]);
        HandleRangeCheck(mir, array, index);
        // Establish value number for loaded register. Note use of memory version.
        uint16_t memory_version = GetMemoryVersion(array, NO_VALUE, type);
        uint16_t res = LookupValue(ARRAY_REF, array, index, memory_version);
        if (opcode == Instruction::AGET_WIDE) {
          SetOperandValueWide(mir->ssa_rep->defs[0], res);
        } else {
          SetOperandValue(mir->ssa_rep->defs[0], res);
        }
      }
      break;

    case Instruction::APUT_OBJECT:
      HandlePutObject(mir);
      // Intentional fall-through.
    case Instruction::APUT:
    case Instruction::APUT_WIDE:
    case Instruction::APUT_BYTE:
    case Instruction::APUT_BOOLEAN:
    case Instruction::APUT_SHORT:
    case Instruction::APUT_CHAR: {
        uint16_t type = opcode - Instruction::APUT;
        int array_idx = (opcode == Instruction::APUT_WIDE) ? 2 : 1;
        int index_idx = array_idx + 1;
        uint16_t array = GetOperandValue(mir->ssa_rep->uses[array_idx]);
        HandleNullCheck(mir, array);
        uint16_t index = GetOperandValue(mir->ssa_rep->uses[index_idx]);
        HandleRangeCheck(mir, array, index);
        // Rev the memory version
        AdvanceMemoryVersion(array, NO_VALUE, type);
      }
      break;

    case Instruction::IGET_OBJECT:
    case Instruction::IGET:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT: {
        uint16_t type = opcode - Instruction::IGET;
        uint16_t base = GetOperandValue(mir->ssa_rep->uses[0]);
        HandleNullCheck(mir, base);
        const MirFieldInfo& field_info = cu_->mir_graph->GetIFieldLoweringInfo(mir);
        uint16_t memory_version;
        uint16_t field_id;
        if (!field_info.IsResolved() || field_info.IsVolatile()) {
          // Volatile fields always get a new memory version; field id is irrelevant.
          // Unresolved fields may be volatile, so handle them as such to be safe.
          field_id = 0u;
          memory_version = next_memory_version_;
          ++next_memory_version_;
        } else {
          DCHECK(field_info.IsResolved());
          field_id = GetFieldId(field_info.DeclaringDexFile(), field_info.DeclaringFieldIndex());
          memory_version = std::max(unresolved_ifield_version_[type],
                                    GetMemoryVersion(base, field_id, type));
        }
        if (opcode == Instruction::IGET_WIDE) {
          res = LookupValue(Instruction::IGET_WIDE, base, field_id, memory_version);
          SetOperandValueWide(mir->ssa_rep->defs[0], res);
        } else {
          res = LookupValue(Instruction::IGET, base, field_id, memory_version);
          SetOperandValue(mir->ssa_rep->defs[0], res);
        }
      }
      break;

    case Instruction::IPUT_OBJECT:
      HandlePutObject(mir);
      // Intentional fall-through.
    case Instruction::IPUT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT: {
        uint16_t type = opcode - Instruction::IPUT;
        int base_reg = (opcode == Instruction::IPUT_WIDE) ? 2 : 1;
        uint16_t base = GetOperandValue(mir->ssa_rep->uses[base_reg]);
        HandleNullCheck(mir, base);
        const MirFieldInfo& field_info = cu_->mir_graph->GetIFieldLoweringInfo(mir);
        if (!field_info.IsResolved()) {
          // Unresolved fields always alias with everything of the same type.
          unresolved_ifield_version_[type] = next_memory_version_;
          ++next_memory_version_;
        } else if (field_info.IsVolatile()) {
          // Nothing to do, resolved volatile fields always get a new memory version anyway and
          // can't alias with resolved non-volatile fields.
        } else {
          AdvanceMemoryVersion(base, GetFieldId(field_info.DeclaringDexFile(),
                                                field_info.DeclaringFieldIndex()), type);
        }
      }
      break;

    case Instruction::SGET_OBJECT:
    case Instruction::SGET:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT: {
        uint16_t type = opcode - Instruction::SGET;
        const MirFieldInfo& field_info = cu_->mir_graph->GetSFieldLoweringInfo(mir);
        uint16_t memory_version;
        uint16_t field_id;
        if (!field_info.IsResolved() || field_info.IsVolatile()) {
          // Volatile fields always get a new memory version; field id is irrelevant.
          // Unresolved fields may be volatile, so handle them as such to be safe.
          field_id = 0u;
          memory_version = next_memory_version_;
          ++next_memory_version_;
        } else {
          DCHECK(field_info.IsResolved());
          field_id = GetFieldId(field_info.DeclaringDexFile(), field_info.DeclaringFieldIndex());
          memory_version = std::max(unresolved_sfield_version_[type],
                                    GetMemoryVersion(NO_VALUE, field_id, type));
        }
        if (opcode == Instruction::SGET_WIDE) {
          res = LookupValue(Instruction::SGET_WIDE, NO_VALUE, field_id, memory_version);
          SetOperandValueWide(mir->ssa_rep->defs[0], res);
        } else {
          res = LookupValue(Instruction::SGET, NO_VALUE, field_id, memory_version);
          SetOperandValue(mir->ssa_rep->defs[0], res);
        }
      }
      break;

    case Instruction::SPUT_OBJECT:
      HandlePutObject(mir);
      // Intentional fall-through.
    case Instruction::SPUT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT: {
        uint16_t type = opcode - Instruction::SPUT;
        const MirFieldInfo& field_info = cu_->mir_graph->GetSFieldLoweringInfo(mir);
        if (!field_info.IsResolved()) {
          // Unresolved fields always alias with everything of the same type.
          unresolved_sfield_version_[type] = next_memory_version_;
          ++next_memory_version_;
        } else if (field_info.IsVolatile()) {
          // Nothing to do, resolved volatile fields always get a new memory version anyway and
          // can't alias with resolved non-volatile fields.
        } else {
          AdvanceMemoryVersion(NO_VALUE, GetFieldId(field_info.DeclaringDexFile(),
                                                    field_info.DeclaringFieldIndex()), type);
        }
      }
      break;
  }
  return res;
}

}    // namespace art
