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

#include "inline_method_analyser.h"
#include "dex_instruction.h"
#include "dex_instruction-inl.h"
#include "mirror/art_field.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/dex_cache-inl.h"
#include "verifier/method_verifier.h"
#include "verifier/method_verifier-inl.h"

/*
 * NOTE: This code is part of the quick compiler. It lives in the runtime
 * only to allow the debugger to check whether a method has been inlined.
 */

namespace art {

COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET),
               check_iget_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_WIDE),
               check_iget_wide_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_OBJECT),
               check_iget_object_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_BOOLEAN),
               check_iget_boolean_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_BYTE),
               check_iget_byte_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_CHAR),
               check_iget_char_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_SHORT),
               check_iget_short_type);

COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT),
               check_iput_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_WIDE),
               check_iput_wide_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_OBJECT),
               check_iput_object_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_BOOLEAN),
               check_iput_boolean_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_BYTE),
               check_iput_byte_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_CHAR),
               check_iput_char_type);
COMPILE_ASSERT(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_SHORT),
               check_iput_short_type);

COMPILE_ASSERT(InlineMethodAnalyser::IGetVariant(Instruction::IGET) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT), check_iget_iput_variant);
COMPILE_ASSERT(InlineMethodAnalyser::IGetVariant(Instruction::IGET_WIDE) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_WIDE), check_iget_iput_wide_variant);
COMPILE_ASSERT(InlineMethodAnalyser::IGetVariant(Instruction::IGET_OBJECT) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_OBJECT), check_iget_iput_object_variant);
COMPILE_ASSERT(InlineMethodAnalyser::IGetVariant(Instruction::IGET_BOOLEAN) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_BOOLEAN), check_iget_iput_boolean_variant);
COMPILE_ASSERT(InlineMethodAnalyser::IGetVariant(Instruction::IGET_BYTE) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_BYTE), check_iget_iput_byte_variant);
COMPILE_ASSERT(InlineMethodAnalyser::IGetVariant(Instruction::IGET_CHAR) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_CHAR), check_iget_iput_char_variant);
COMPILE_ASSERT(InlineMethodAnalyser::IGetVariant(Instruction::IGET_SHORT) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_SHORT), check_iget_iput_short_variant);

bool InlineMethodAnalyser::AnalyseMethodCode(verifier::MethodVerifier* verifier,
                                             InlineMethod* method) {
  // We currently support only plain return or 2-instruction methods.

  const DexFile::CodeItem* code_item = verifier->CodeItem();
  DCHECK_NE(code_item->insns_size_in_code_units_, 0u);
  const Instruction* instruction = Instruction::At(code_item->insns_);
  Instruction::Code opcode = instruction->Opcode();

  switch (opcode) {
    case Instruction::RETURN_VOID:
      method->opcode = kInlineOpNop;
      method->flags = kInlineSpecial;
      method->d.data = 0u;
      return true;
    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT:
    case Instruction::RETURN_WIDE:
      return AnalyseReturnMethod(code_item, method);
    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16:
    case Instruction::CONST_HIGH16:
      // TODO: Support wide constants (RETURN_WIDE).
      return AnalyseConstMethod(code_item, method);
    case Instruction::IGET:
    case Instruction::IGET_OBJECT:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT:
    case Instruction::IGET_WIDE:
      return AnalyseIGetMethod(verifier, method);
    case Instruction::IPUT:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT:
    case Instruction::IPUT_WIDE:
      return AnalyseIPutMethod(verifier, method);
    default:
      return false;
  }
}

bool InlineMethodAnalyser::AnalyseReturnMethod(const DexFile::CodeItem* code_item,
                                               InlineMethod* result) {
  const Instruction* return_instruction = Instruction::At(code_item->insns_);
  Instruction::Code return_opcode = return_instruction->Opcode();
  uint32_t reg = return_instruction->VRegA_11x();
  uint32_t arg_start = code_item->registers_size_ - code_item->ins_size_;
  DCHECK_GE(reg, arg_start);
  DCHECK_LT((return_opcode == Instruction::RETURN_WIDE) ? reg + 1 : reg,
      code_item->registers_size_);

  result->opcode = kInlineOpReturnArg;
  result->flags = kInlineSpecial;
  InlineReturnArgData* data = &result->d.return_data;
  data->arg = reg - arg_start;
  data->is_wide = (return_opcode == Instruction::RETURN_WIDE) ? 1u : 0u;
  data->is_object = (return_opcode == Instruction::RETURN_OBJECT) ? 1u : 0u;
  data->reserved = 0u;
  data->reserved2 = 0u;
  return true;
}

bool InlineMethodAnalyser::AnalyseConstMethod(const DexFile::CodeItem* code_item,
                                              InlineMethod* result) {
  const Instruction* instruction = Instruction::At(code_item->insns_);
  const Instruction* return_instruction = instruction->Next();
  Instruction::Code return_opcode = return_instruction->Opcode();
  if (return_opcode != Instruction::RETURN &&
      return_opcode != Instruction::RETURN_OBJECT) {
    return false;
  }

  uint32_t return_reg = return_instruction->VRegA_11x();
  DCHECK_LT(return_reg, code_item->registers_size_);

  uint32_t vA, vB, dummy;
  uint64_t dummy_wide;
  instruction->Decode(vA, vB, dummy_wide, dummy, nullptr);
  if (instruction->Opcode() == Instruction::CONST_HIGH16) {
    vB <<= 16;
  }
  DCHECK_LT(vA, code_item->registers_size_);
  if (vA != return_reg) {
    return false;  // Not returning the value set by const?
  }
  if (return_opcode == Instruction::RETURN_OBJECT && vB != 0) {
    return false;  // Returning non-null reference constant?
  }
  result->opcode = kInlineOpNonWideConst;
  result->flags = kInlineSpecial;
  result->d.data = static_cast<uint64_t>(vB);
  return true;
}

bool InlineMethodAnalyser::AnalyseIGetMethod(verifier::MethodVerifier* verifier,
                                             InlineMethod* result) {
  const DexFile::CodeItem* code_item = verifier->CodeItem();
  const Instruction* instruction = Instruction::At(code_item->insns_);
  Instruction::Code opcode = instruction->Opcode();
  DCHECK(IsInstructionIGet(opcode));

  const Instruction* return_instruction = instruction->Next();
  Instruction::Code return_opcode = return_instruction->Opcode();
  if (!(return_opcode == Instruction::RETURN_WIDE && opcode == Instruction::IGET_WIDE) &&
      !(return_opcode == Instruction::RETURN_OBJECT && opcode == Instruction::IGET_OBJECT) &&
      !(return_opcode == Instruction::RETURN && opcode != Instruction::IGET_WIDE &&
          opcode != Instruction::IGET_OBJECT)) {
    return false;
  }

  uint32_t return_reg = return_instruction->VRegA_11x();
  DCHECK_LT(return_opcode == Instruction::RETURN_WIDE ? return_reg + 1 : return_reg,
            code_item->registers_size_);

  uint32_t dst_reg = instruction->VRegA_22c();
  uint32_t object_reg = instruction->VRegB_22c();
  uint32_t field_idx = instruction->VRegC_22c();
  uint32_t arg_start = code_item->registers_size_ - code_item->ins_size_;
  DCHECK_GE(object_reg, arg_start);
  DCHECK_LT(object_reg, code_item->registers_size_);
  DCHECK_LT(opcode == Instruction::IGET_WIDE ? dst_reg + 1 : dst_reg, code_item->registers_size_);
  if (dst_reg != return_reg) {
    return false;  // Not returning the value retrieved by IGET?
  }

  if ((verifier->GetAccessFlags() & kAccStatic) != 0 || object_reg != arg_start) {
    // TODO: Support inlining IGET on other register than "this".
    return false;
  }

  if (!ComputeSpecialAccessorInfo(field_idx, false, verifier, &result->d.ifield_data)) {
    return false;
  }

  result->opcode = kInlineOpIGet;
  result->flags = kInlineSpecial;
  InlineIGetIPutData* data = &result->d.ifield_data;
  data->op_variant = IGetVariant(opcode);
  data->object_arg = object_reg - arg_start;  // Allow IGET on any register, not just "this".
  data->src_arg = 0;
  data->method_is_static = (verifier->GetAccessFlags() & kAccStatic) != 0;
  data->reserved = 0;
  return true;
}

bool InlineMethodAnalyser::AnalyseIPutMethod(verifier::MethodVerifier* verifier,
                                             InlineMethod* result) {
  const DexFile::CodeItem* code_item = verifier->CodeItem();
  const Instruction* instruction = Instruction::At(code_item->insns_);
  Instruction::Code opcode = instruction->Opcode();
  DCHECK(IsInstructionIPut(opcode));

  const Instruction* return_instruction = instruction->Next();
  Instruction::Code return_opcode = return_instruction->Opcode();
  if (return_opcode != Instruction::RETURN_VOID) {
    // TODO: Support returning an argument.
    // This is needed by builder classes and generated accessor setters.
    //    builder.setX(value): iput value, this, fieldX; return-object this;
    //    object.access$nnn(value): iput value, this, fieldX; return value;
    // Use InlineIGetIPutData::reserved to hold the information.
    return false;
  }

  uint32_t src_reg = instruction->VRegA_22c();
  uint32_t object_reg = instruction->VRegB_22c();
  uint32_t field_idx = instruction->VRegC_22c();
  uint32_t arg_start = code_item->registers_size_ - code_item->ins_size_;
  DCHECK_GE(object_reg, arg_start);
  DCHECK_LT(object_reg, code_item->registers_size_);
  DCHECK_GE(src_reg, arg_start);
  DCHECK_LT(opcode == Instruction::IPUT_WIDE ? src_reg + 1 : src_reg, code_item->registers_size_);

  if ((verifier->GetAccessFlags() & kAccStatic) != 0 || object_reg != arg_start) {
    // TODO: Support inlining IPUT on other register than "this".
    return false;
  }

  if (!ComputeSpecialAccessorInfo(field_idx, true, verifier, &result->d.ifield_data)) {
    return false;
  }

  result->opcode = kInlineOpIPut;
  result->flags = kInlineSpecial;
  InlineIGetIPutData* data = &result->d.ifield_data;
  data->op_variant = IPutVariant(opcode);
  data->object_arg = object_reg - arg_start;  // Allow IPUT on any register, not just "this".
  data->src_arg = src_reg - arg_start;
  data->method_is_static = (verifier->GetAccessFlags() & kAccStatic) != 0;
  data->reserved = 0;
  return true;
}

bool InlineMethodAnalyser::ComputeSpecialAccessorInfo(uint32_t field_idx, bool is_put,
                                                      verifier::MethodVerifier* verifier,
                                                      InlineIGetIPutData* result) {
  mirror::DexCache* dex_cache = verifier->GetDexCache();
  uint32_t method_idx = verifier->GetMethodReference().dex_method_index;
  mirror::ArtMethod* method = dex_cache->GetResolvedMethod(method_idx);
  mirror::ArtField* field = dex_cache->GetResolvedField(field_idx);
  if (method == nullptr || field == nullptr || field->IsStatic()) {
    return false;
  }
  mirror::Class* method_class = method->GetDeclaringClass();
  mirror::Class* field_class = field->GetDeclaringClass();
  if (!method_class->CanAccessResolvedField(field_class, field, dex_cache, field_idx) ||
      (is_put && field->IsFinal() && method_class != field_class)) {
    return false;
  }
  DCHECK_GE(field->GetOffset().Int32Value(), 0);
  result->field_idx = field_idx;
  result->field_offset = field->GetOffset().Int32Value();
  result->is_volatile = field->IsVolatile();
  return true;
}

}  // namespace art
