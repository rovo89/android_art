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

#include "verified_method.h"

#include <algorithm>
#include <vector>

#include "base/logging.h"
#include "base/stl_util.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "dex_instruction-inl.h"
#include "base/mutex.h"
#include "base/mutex-inl.h"
#include "mirror/art_method.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "UniquePtr.h"
#include "verifier/dex_gc_map.h"
#include "verifier/method_verifier.h"
#include "verifier/method_verifier-inl.h"
#include "verifier/register_line.h"
#include "verifier/register_line-inl.h"

namespace art {

const VerifiedMethod* VerifiedMethod::Create(verifier::MethodVerifier* method_verifier,
                                             bool compile) {
  UniquePtr<VerifiedMethod> verified_method(new VerifiedMethod);
  if (compile) {
    /* Generate a register map. */
    if (!verified_method->GenerateGcMap(method_verifier)) {
      CHECK(method_verifier->HasFailures());
      return nullptr;  // Not a real failure, but a failure to encode.
    }
    if (kIsDebugBuild) {
      VerifyGcMap(method_verifier, verified_method->dex_gc_map_);
    }

    // TODO: move this out when DEX-to-DEX supports devirtualization.
    if (method_verifier->HasVirtualOrInterfaceInvokes()) {
      verified_method->GenerateDevirtMap(method_verifier);
    }
  }

  if (method_verifier->HasCheckCasts()) {
    verified_method->GenerateSafeCastSet(method_verifier);
  }
  return verified_method.release();
}

const MethodReference* VerifiedMethod::GetDevirtTarget(uint32_t dex_pc) const {
  auto it = devirt_map_.find(dex_pc);
  return (it != devirt_map_.end()) ? &it->second : nullptr;
}

bool VerifiedMethod::IsSafeCast(uint32_t pc) const {
  return std::binary_search(safe_cast_set_.begin(), safe_cast_set_.end(), pc);
}

bool VerifiedMethod::GenerateGcMap(verifier::MethodVerifier* method_verifier) {
  DCHECK(dex_gc_map_.empty());
  size_t num_entries, ref_bitmap_bits, pc_bits;
  ComputeGcMapSizes(method_verifier, &num_entries, &ref_bitmap_bits, &pc_bits);
  // There's a single byte to encode the size of each bitmap.
  if (ref_bitmap_bits >= (8 /* bits per byte */ * 8192 /* 13-bit size */ )) {
    // TODO: either a better GC map format or per method failures
    method_verifier->Fail(verifier::VERIFY_ERROR_BAD_CLASS_HARD)
        << "Cannot encode GC map for method with " << ref_bitmap_bits << " registers";
    return false;
  }
  size_t ref_bitmap_bytes = (ref_bitmap_bits + 7) / 8;
  // There are 2 bytes to encode the number of entries.
  if (num_entries >= 65536) {
    // TODO: Either a better GC map format or per method failures.
    method_verifier->Fail(verifier::VERIFY_ERROR_BAD_CLASS_HARD)
        << "Cannot encode GC map for method with " << num_entries << " entries";
    return false;
  }
  size_t pc_bytes;
  verifier::RegisterMapFormat format;
  if (pc_bits <= 8) {
    format = verifier::kRegMapFormatCompact8;
    pc_bytes = 1;
  } else if (pc_bits <= 16) {
    format = verifier::kRegMapFormatCompact16;
    pc_bytes = 2;
  } else {
    // TODO: Either a better GC map format or per method failures.
    method_verifier->Fail(verifier::VERIFY_ERROR_BAD_CLASS_HARD)
        << "Cannot encode GC map for method with "
        << (1 << pc_bits) << " instructions (number is rounded up to nearest power of 2)";
    return false;
  }
  size_t table_size = ((pc_bytes + ref_bitmap_bytes) * num_entries) + 4;
  dex_gc_map_.reserve(table_size);
  // Write table header.
  dex_gc_map_.push_back(format | ((ref_bitmap_bytes & ~0xFF) >> 5));
  dex_gc_map_.push_back(ref_bitmap_bytes & 0xFF);
  dex_gc_map_.push_back(num_entries & 0xFF);
  dex_gc_map_.push_back((num_entries >> 8) & 0xFF);
  // Write table data.
  const DexFile::CodeItem* code_item = method_verifier->CodeItem();
  for (size_t i = 0; i < code_item->insns_size_in_code_units_; i++) {
    if (method_verifier->GetInstructionFlags(i).IsCompileTimeInfoPoint()) {
      dex_gc_map_.push_back(i & 0xFF);
      if (pc_bytes == 2) {
        dex_gc_map_.push_back((i >> 8) & 0xFF);
      }
      verifier::RegisterLine* line = method_verifier->GetRegLine(i);
      line->WriteReferenceBitMap(dex_gc_map_, ref_bitmap_bytes);
    }
  }
  DCHECK_EQ(dex_gc_map_.size(), table_size);
  return true;
}

void VerifiedMethod::VerifyGcMap(verifier::MethodVerifier* method_verifier,
                                 const std::vector<uint8_t>& data) {
  // Check that for every GC point there is a map entry, there aren't entries for non-GC points,
  // that the table data is well formed and all references are marked (or not) in the bitmap.
  verifier::DexPcToReferenceMap map(&data[0]);
  DCHECK_EQ(data.size(), map.RawSize());
  size_t map_index = 0;
  const DexFile::CodeItem* code_item = method_verifier->CodeItem();
  for (size_t i = 0; i < code_item->insns_size_in_code_units_; i++) {
    const uint8_t* reg_bitmap = map.FindBitMap(i, false);
    if (method_verifier->GetInstructionFlags(i).IsCompileTimeInfoPoint()) {
      DCHECK_LT(map_index, map.NumEntries());
      DCHECK_EQ(map.GetDexPc(map_index), i);
      DCHECK_EQ(map.GetBitMap(map_index), reg_bitmap);
      map_index++;
      verifier::RegisterLine* line = method_verifier->GetRegLine(i);
      for (size_t j = 0; j < code_item->registers_size_; j++) {
        if (line->GetRegisterType(j).IsNonZeroReferenceTypes()) {
          DCHECK_LT(j / 8, map.RegWidth());
          DCHECK_EQ((reg_bitmap[j / 8] >> (j % 8)) & 1, 1);
        } else if ((j / 8) < map.RegWidth()) {
          DCHECK_EQ((reg_bitmap[j / 8] >> (j % 8)) & 1, 0);
        } else {
          // If a register doesn't contain a reference then the bitmap may be shorter than the line.
        }
      }
    } else {
      DCHECK(reg_bitmap == NULL);
    }
  }
}

void VerifiedMethod::ComputeGcMapSizes(verifier::MethodVerifier* method_verifier,
                                       size_t* gc_points, size_t* ref_bitmap_bits,
                                       size_t* log2_max_gc_pc) {
  size_t local_gc_points = 0;
  size_t max_insn = 0;
  size_t max_ref_reg = -1;
  const DexFile::CodeItem* code_item = method_verifier->CodeItem();
  for (size_t i = 0; i < code_item->insns_size_in_code_units_; i++) {
    if (method_verifier->GetInstructionFlags(i).IsCompileTimeInfoPoint()) {
      local_gc_points++;
      max_insn = i;
      verifier::RegisterLine* line = method_verifier->GetRegLine(i);
      max_ref_reg = line->GetMaxNonZeroReferenceReg(max_ref_reg);
    }
  }
  *gc_points = local_gc_points;
  *ref_bitmap_bits = max_ref_reg + 1;  // If max register is 0 we need 1 bit to encode (ie +1).
  size_t i = 0;
  while ((1U << i) <= max_insn) {
    i++;
  }
  *log2_max_gc_pc = i;
}

void VerifiedMethod::GenerateDevirtMap(verifier::MethodVerifier* method_verifier) {
  // It is risky to rely on reg_types for sharpening in cases of soft
  // verification, we might end up sharpening to a wrong implementation. Just abort.
  if (method_verifier->HasFailures()) {
    return;
  }

  const DexFile::CodeItem* code_item = method_verifier->CodeItem();
  const uint16_t* insns = code_item->insns_;
  const Instruction* inst = Instruction::At(insns);
  const Instruction* end = Instruction::At(insns + code_item->insns_size_in_code_units_);

  for (; inst < end; inst = inst->Next()) {
    bool is_virtual   = (inst->Opcode() == Instruction::INVOKE_VIRTUAL) ||
        (inst->Opcode() ==  Instruction::INVOKE_VIRTUAL_RANGE);
    bool is_interface = (inst->Opcode() == Instruction::INVOKE_INTERFACE) ||
        (inst->Opcode() == Instruction::INVOKE_INTERFACE_RANGE);

    if (!is_interface && !is_virtual) {
      continue;
    }
    // Get reg type for register holding the reference to the object that will be dispatched upon.
    uint32_t dex_pc = inst->GetDexPc(insns);
    verifier::RegisterLine* line = method_verifier->GetRegLine(dex_pc);
    bool is_range = (inst->Opcode() ==  Instruction::INVOKE_VIRTUAL_RANGE) ||
        (inst->Opcode() ==  Instruction::INVOKE_INTERFACE_RANGE);
    const verifier::RegType&
        reg_type(line->GetRegisterType(is_range ? inst->VRegC_3rc() : inst->VRegC_35c()));

    if (!reg_type.HasClass()) {
      // We will compute devirtualization information only when we know the Class of the reg type.
      continue;
    }
    mirror::Class* reg_class = reg_type.GetClass();
    if (reg_class->IsInterface()) {
      // We can't devirtualize when the known type of the register is an interface.
      continue;
    }
    if (reg_class->IsAbstract() && !reg_class->IsArrayClass()) {
      // We can't devirtualize abstract classes except on arrays of abstract classes.
      continue;
    }
    mirror::ArtMethod* abstract_method = method_verifier->GetDexCache()->GetResolvedMethod(
        is_range ? inst->VRegB_3rc() : inst->VRegB_35c());
    if (abstract_method == NULL) {
      // If the method is not found in the cache this means that it was never found
      // by ResolveMethodAndCheckAccess() called when verifying invoke_*.
      continue;
    }
    // Find the concrete method.
    mirror::ArtMethod* concrete_method = NULL;
    if (is_interface) {
      concrete_method = reg_type.GetClass()->FindVirtualMethodForInterface(abstract_method);
    }
    if (is_virtual) {
      concrete_method = reg_type.GetClass()->FindVirtualMethodForVirtual(abstract_method);
    }
    if (concrete_method == NULL || concrete_method->IsAbstract()) {
      // In cases where concrete_method is not found, or is abstract, continue to the next invoke.
      continue;
    }
    if (reg_type.IsPreciseReference() || concrete_method->IsFinal() ||
        concrete_method->GetDeclaringClass()->IsFinal()) {
      // If we knew exactly the class being dispatched upon, or if the target method cannot be
      // overridden record the target to be used in the compiler driver.
      MethodReference concrete_ref(
          concrete_method->GetDeclaringClass()->GetDexCache()->GetDexFile(),
          concrete_method->GetDexMethodIndex());
      devirt_map_.Put(dex_pc, concrete_ref);
    }
  }
}

void VerifiedMethod::GenerateSafeCastSet(verifier::MethodVerifier* method_verifier) {
  /*
   * Walks over the method code and adds any cast instructions in which
   * the type cast is implicit to a set, which is used in the code generation
   * to elide these casts.
   */
  if (method_verifier->HasFailures()) {
    return;
  }
  const DexFile::CodeItem* code_item = method_verifier->CodeItem();
  const Instruction* inst = Instruction::At(code_item->insns_);
  const Instruction* end = Instruction::At(code_item->insns_ +
                                           code_item->insns_size_in_code_units_);

  for (; inst < end; inst = inst->Next()) {
    Instruction::Code code = inst->Opcode();
    if ((code == Instruction::CHECK_CAST) || (code == Instruction::APUT_OBJECT)) {
      uint32_t dex_pc = inst->GetDexPc(code_item->insns_);
      const verifier::RegisterLine* line = method_verifier->GetRegLine(dex_pc);
      bool is_safe_cast = false;
      if (code == Instruction::CHECK_CAST) {
        const verifier::RegType& reg_type(line->GetRegisterType(inst->VRegA_21c()));
        const verifier::RegType& cast_type =
            method_verifier->ResolveCheckedClass(inst->VRegB_21c());
        is_safe_cast = cast_type.IsStrictlyAssignableFrom(reg_type);
      } else {
        const verifier::RegType& array_type(line->GetRegisterType(inst->VRegB_23x()));
        // We only know its safe to assign to an array if the array type is precise. For example,
        // an Object[] can have any type of object stored in it, but it may also be assigned a
        // String[] in which case the stores need to be of Strings.
        if (array_type.IsPreciseReference()) {
          const verifier::RegType& value_type(line->GetRegisterType(inst->VRegA_23x()));
          const verifier::RegType& component_type = method_verifier->GetRegTypeCache()
              ->GetComponentType(array_type, method_verifier->GetClassLoader());
          is_safe_cast = component_type.IsStrictlyAssignableFrom(value_type);
        }
      }
      if (is_safe_cast) {
        // Verify ordering for push_back() to the sorted vector.
        DCHECK(safe_cast_set_.empty() || safe_cast_set_.back() < dex_pc);
        safe_cast_set_.push_back(dex_pc);
      }
    }
  }
}

}  // namespace art
