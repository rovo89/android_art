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

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "dex_instruction-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "verifier/method_verifier-inl.h"

/*
 * NOTE: This code is part of the quick compiler. It lives in the runtime
 * only to allow the debugger to check whether a method has been inlined.
 */

namespace art {

static_assert(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET), "iget type");
static_assert(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_WIDE), "iget_wide type");
static_assert(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_OBJECT),
              "iget_object type");
static_assert(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_BOOLEAN),
              "iget_boolean type");
static_assert(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_BYTE), "iget_byte type");
static_assert(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_CHAR), "iget_char type");
static_assert(InlineMethodAnalyser::IsInstructionIGet(Instruction::IGET_SHORT), "iget_short type");
static_assert(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT), "iput type");
static_assert(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_WIDE), "iput_wide type");
static_assert(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_OBJECT),
              "iput_object type");
static_assert(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_BOOLEAN),
              "iput_boolean type");
static_assert(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_BYTE), "iput_byte type");
static_assert(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_CHAR), "iput_char type");
static_assert(InlineMethodAnalyser::IsInstructionIPut(Instruction::IPUT_SHORT), "iput_short type");
static_assert(InlineMethodAnalyser::IGetVariant(Instruction::IGET) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT), "iget/iput variant");
static_assert(InlineMethodAnalyser::IGetVariant(Instruction::IGET_WIDE) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_WIDE), "iget/iput_wide variant");
static_assert(InlineMethodAnalyser::IGetVariant(Instruction::IGET_OBJECT) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_OBJECT), "iget/iput_object variant");
static_assert(InlineMethodAnalyser::IGetVariant(Instruction::IGET_BOOLEAN) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_BOOLEAN), "iget/iput_boolean variant");
static_assert(InlineMethodAnalyser::IGetVariant(Instruction::IGET_BYTE) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_BYTE), "iget/iput_byte variant");
static_assert(InlineMethodAnalyser::IGetVariant(Instruction::IGET_CHAR) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_CHAR), "iget/iput_char variant");
static_assert(InlineMethodAnalyser::IGetVariant(Instruction::IGET_SHORT) ==
    InlineMethodAnalyser::IPutVariant(Instruction::IPUT_SHORT), "iget/iput_short variant");

// This is used by compiler and debugger. We look into the dex cache for resolved methods and
// fields. However, in the context of the debugger, not all methods and fields are resolved. Since
// we need to be able to detect possibly inlined method, we pass a null inline method to indicate
// we don't want to take unresolved methods and fields into account during analysis.
bool InlineMethodAnalyser::AnalyseMethodCode(verifier::MethodVerifier* verifier,
                                             InlineMethod* method) {
  DCHECK(verifier != nullptr);
  DCHECK_EQ(Runtime::Current()->IsCompiler(), method != nullptr);
  if (!Runtime::Current()->UseJit()) {
    DCHECK_EQ(verifier->CanLoadClasses(), method != nullptr);
  }
  // We currently support only plain return or 2-instruction methods.

  const DexFile::CodeItem* code_item = verifier->CodeItem();
  DCHECK_NE(code_item->insns_size_in_code_units_, 0u);
  const Instruction* instruction = Instruction::At(code_item->insns_);
  Instruction::Code opcode = instruction->Opcode();

  switch (opcode) {
    case Instruction::RETURN_VOID:
      if (method != nullptr) {
        method->opcode = kInlineOpNop;
        method->flags = kInlineSpecial;
        method->d.data = 0u;
      }
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
    // TODO: Add handling for JIT.
    // case Instruction::IGET_QUICK:
    // case Instruction::IGET_WIDE_QUICK:
    // case Instruction::IGET_OBJECT_QUICK:
      return AnalyseIGetMethod(verifier, method);
    case Instruction::IPUT:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT:
    case Instruction::IPUT_WIDE:
      // TODO: Add handling for JIT.
    // case Instruction::IPUT_QUICK:
    // case Instruction::IPUT_WIDE_QUICK:
    // case Instruction::IPUT_OBJECT_QUICK:
      return AnalyseIPutMethod(verifier, method);
    default:
      return false;
  }
}

bool InlineMethodAnalyser::IsSyntheticAccessor(MethodReference ref) {
  const DexFile::MethodId& method_id = ref.dex_file->GetMethodId(ref.dex_method_index);
  const char* method_name = ref.dex_file->GetMethodName(method_id);
  // javac names synthetic accessors "access$nnn",
  // jack names them "-getN", "-putN", "-wrapN".
  return strncmp(method_name, "access$", strlen("access$")) == 0 ||
      strncmp(method_name, "-", strlen("-")) == 0;
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

  if (result != nullptr) {
    result->opcode = kInlineOpReturnArg;
    result->flags = kInlineSpecial;
    InlineReturnArgData* data = &result->d.return_data;
    data->arg = reg - arg_start;
    data->is_wide = (return_opcode == Instruction::RETURN_WIDE) ? 1u : 0u;
    data->is_object = (return_opcode == Instruction::RETURN_OBJECT) ? 1u : 0u;
    data->reserved = 0u;
    data->reserved2 = 0u;
  }
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

  int32_t return_reg = return_instruction->VRegA_11x();
  DCHECK_LT(return_reg, code_item->registers_size_);

  int32_t const_value = instruction->VRegB();
  if (instruction->Opcode() == Instruction::CONST_HIGH16) {
    const_value <<= 16;
  }
  DCHECK_LT(instruction->VRegA(), code_item->registers_size_);
  if (instruction->VRegA() != return_reg) {
    return false;  // Not returning the value set by const?
  }
  if (return_opcode == Instruction::RETURN_OBJECT && const_value != 0) {
    return false;  // Returning non-null reference constant?
  }
  if (result != nullptr) {
    result->opcode = kInlineOpNonWideConst;
    result->flags = kInlineSpecial;
    result->d.data = static_cast<uint64_t>(const_value);
  }
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
  uint32_t object_arg = object_reg - arg_start;

  DCHECK_LT(opcode == Instruction::IGET_WIDE ? dst_reg + 1 : dst_reg, code_item->registers_size_);
  if (dst_reg != return_reg) {
    return false;  // Not returning the value retrieved by IGET?
  }

  if ((verifier->GetAccessFlags() & kAccStatic) != 0u || object_arg != 0u) {
    // TODO: Implement inlining of IGET on non-"this" registers (needs correct stack trace for NPE).
    // Allow synthetic accessors. We don't care about losing their stack frame in NPE.
    if (!IsSyntheticAccessor(verifier->GetMethodReference())) {
      return false;
    }
  }

  // InlineIGetIPutData::object_arg is only 4 bits wide.
  static constexpr uint16_t kMaxObjectArg = 15u;
  if (object_arg > kMaxObjectArg) {
    return false;
  }

  if (result != nullptr) {
    InlineIGetIPutData* data = &result->d.ifield_data;
    if (!ComputeSpecialAccessorInfo(field_idx, false, verifier, data)) {
      return false;
    }
    result->opcode = kInlineOpIGet;
    result->flags = kInlineSpecial;
    data->op_variant = IGetVariant(opcode);
    data->method_is_static = (verifier->GetAccessFlags() & kAccStatic) != 0u ? 1u : 0u;
    data->object_arg = object_arg;  // Allow IGET on any register, not just "this".
    data->src_arg = 0u;
    data->return_arg_plus1 = 0u;
  }
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
  uint32_t arg_start = code_item->registers_size_ - code_item->ins_size_;
  uint16_t return_arg_plus1 = 0u;
  if (return_opcode != Instruction::RETURN_VOID) {
    if (return_opcode != Instruction::RETURN &&
        return_opcode != Instruction::RETURN_OBJECT &&
        return_opcode != Instruction::RETURN_WIDE) {
      return false;
    }
    // Returning an argument.
    uint32_t return_reg = return_instruction->VRegA_11x();
    DCHECK_GE(return_reg, arg_start);
    DCHECK_LT(return_opcode == Instruction::RETURN_WIDE ? return_reg + 1u : return_reg,
              code_item->registers_size_);
    return_arg_plus1 = return_reg - arg_start + 1u;
  }

  uint32_t src_reg = instruction->VRegA_22c();
  uint32_t object_reg = instruction->VRegB_22c();
  uint32_t field_idx = instruction->VRegC_22c();
  DCHECK_GE(object_reg, arg_start);
  DCHECK_LT(object_reg, code_item->registers_size_);
  DCHECK_GE(src_reg, arg_start);
  DCHECK_LT(opcode == Instruction::IPUT_WIDE ? src_reg + 1 : src_reg, code_item->registers_size_);
  uint32_t object_arg = object_reg - arg_start;
  uint32_t src_arg = src_reg - arg_start;

  if ((verifier->GetAccessFlags() & kAccStatic) != 0u || object_arg != 0u) {
    // TODO: Implement inlining of IPUT on non-"this" registers (needs correct stack trace for NPE).
    // Allow synthetic accessors. We don't care about losing their stack frame in NPE.
    if (!IsSyntheticAccessor(verifier->GetMethodReference())) {
      return false;
    }
  }

  // InlineIGetIPutData::object_arg/src_arg/return_arg_plus1 are each only 4 bits wide.
  static constexpr uint16_t kMaxObjectArg = 15u;
  static constexpr uint16_t kMaxSrcArg = 15u;
  static constexpr uint16_t kMaxReturnArgPlus1 = 15u;
  if (object_arg > kMaxObjectArg || src_arg > kMaxSrcArg || return_arg_plus1 > kMaxReturnArgPlus1) {
    return false;
  }

  if (result != nullptr) {
    InlineIGetIPutData* data = &result->d.ifield_data;
    if (!ComputeSpecialAccessorInfo(field_idx, true, verifier, data)) {
      return false;
    }
    result->opcode = kInlineOpIPut;
    result->flags = kInlineSpecial;
    data->op_variant = IPutVariant(opcode);
    data->method_is_static = (verifier->GetAccessFlags() & kAccStatic) != 0u ? 1u : 0u;
    data->object_arg = object_arg;  // Allow IPUT on any register, not just "this".
    data->src_arg = src_arg;
    data->return_arg_plus1 = return_arg_plus1;
  }
  return true;
}

bool InlineMethodAnalyser::ComputeSpecialAccessorInfo(uint32_t field_idx, bool is_put,
                                                      verifier::MethodVerifier* verifier,
                                                      InlineIGetIPutData* result) {
  mirror::DexCache* dex_cache = verifier->GetDexCache();
  uint32_t method_idx = verifier->GetMethodReference().dex_method_index;
  auto* cl = Runtime::Current()->GetClassLinker();
  ArtMethod* method = dex_cache->GetResolvedMethod(method_idx, cl->GetImagePointerSize());
  ArtField* field = cl->GetResolvedField(field_idx, dex_cache);
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
