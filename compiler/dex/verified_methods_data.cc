/*
 * Copyright (C) 2013 The Android Open Source Project
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
#include "verified_methods_data.h"
#include "verifier/dex_gc_map.h"
#include "verifier/method_verifier.h"
#include "verifier/method_verifier-inl.h"
#include "verifier/register_line.h"
#include "verifier/register_line-inl.h"

namespace art {

VerifiedMethodsData::VerifiedMethodsData()
    : dex_gc_maps_lock_("compiler GC maps lock"),
      dex_gc_maps_(),
      safecast_map_lock_("compiler Cast Elision lock"),
      safecast_map_(),
      devirt_maps_lock_("compiler Devirtualization lock"),
      devirt_maps_(),
      rejected_classes_lock_("compiler rejected classes lock"),
      rejected_classes_() {
}

VerifiedMethodsData::~VerifiedMethodsData() {
  Thread* self = Thread::Current();
  {
    WriterMutexLock mu(self, dex_gc_maps_lock_);
    STLDeleteValues(&dex_gc_maps_);
  }
  {
    WriterMutexLock mu(self, safecast_map_lock_);
    STLDeleteValues(&safecast_map_);
  }
  {
    WriterMutexLock mu(self, devirt_maps_lock_);
    STLDeleteValues(&devirt_maps_);
  }
}

bool VerifiedMethodsData::ProcessVerifiedMethod(verifier::MethodVerifier* method_verifier) {
  MethodReference ref = method_verifier->GetMethodReference();
  bool compile = IsCandidateForCompilation(ref, method_verifier->GetAccessFlags());
  if (compile) {
    /* Generate a register map and add it to the method. */
    const std::vector<uint8_t>* dex_gc_map = GenerateGcMap(method_verifier);
    if (dex_gc_map == NULL) {
      DCHECK(method_verifier->HasFailures());
      return false;  // Not a real failure, but a failure to encode
    }
    if (kIsDebugBuild) {
      VerifyGcMap(method_verifier, *dex_gc_map);
    }
    SetDexGcMap(ref, dex_gc_map);
  }

  if (method_verifier->HasCheckCasts()) {
    MethodSafeCastSet* method_to_safe_casts = GenerateSafeCastSet(method_verifier);
    if (method_to_safe_casts != NULL) {
      SetSafeCastMap(ref, method_to_safe_casts);
    }
  }

  if (method_verifier->HasVirtualOrInterfaceInvokes()) {
    PcToConcreteMethodMap* pc_to_concrete_method = GenerateDevirtMap(method_verifier);
    if (pc_to_concrete_method != NULL) {
      SetDevirtMap(ref, pc_to_concrete_method);
    }
  }
  return true;
}

const std::vector<uint8_t>* VerifiedMethodsData::GetDexGcMap(MethodReference ref) {
  ReaderMutexLock mu(Thread::Current(), dex_gc_maps_lock_);
  DexGcMapTable::const_iterator it = dex_gc_maps_.find(ref);
  CHECK(it != dex_gc_maps_.end())
    << "Didn't find GC map for: " << PrettyMethod(ref.dex_method_index, *ref.dex_file);
  CHECK(it->second != NULL);
  return it->second;
}

const MethodReference* VerifiedMethodsData::GetDevirtMap(const MethodReference& ref,
                                                                    uint32_t dex_pc) {
  ReaderMutexLock mu(Thread::Current(), devirt_maps_lock_);
  DevirtualizationMapTable::const_iterator it = devirt_maps_.find(ref);
  if (it == devirt_maps_.end()) {
    return NULL;
  }

  // Look up the PC in the map, get the concrete method to execute and return its reference.
  PcToConcreteMethodMap::const_iterator pc_to_concrete_method = it->second->find(dex_pc);
  if (pc_to_concrete_method != it->second->end()) {
    return &(pc_to_concrete_method->second);
  } else {
    return NULL;
  }
}

bool VerifiedMethodsData::IsSafeCast(MethodReference ref, uint32_t pc) {
  ReaderMutexLock mu(Thread::Current(), safecast_map_lock_);
  SafeCastMap::const_iterator it = safecast_map_.find(ref);
  if (it == safecast_map_.end()) {
    return false;
  }

  // Look up the cast address in the set of safe casts
  MethodSafeCastSet::const_iterator cast_it = it->second->find(pc);
  return cast_it != it->second->end();
}

void VerifiedMethodsData::AddRejectedClass(ClassReference ref) {
  {
    WriterMutexLock mu(Thread::Current(), rejected_classes_lock_);
    rejected_classes_.insert(ref);
  }
  DCHECK(IsClassRejected(ref));
}

bool VerifiedMethodsData::IsClassRejected(ClassReference ref) {
  ReaderMutexLock mu(Thread::Current(), rejected_classes_lock_);
  return (rejected_classes_.find(ref) != rejected_classes_.end());
}

bool VerifiedMethodsData::IsCandidateForCompilation(MethodReference& method_ref,
                                                    const uint32_t access_flags) {
#ifdef ART_SEA_IR_MODE
    bool use_sea = Runtime::Current()->IsSeaIRMode();
    use_sea = use_sea && (std::string::npos != PrettyMethod(
                          method_ref.dex_method_index, *(method_ref.dex_file)).find("fibonacci"));
    if (use_sea) return true;
#endif
  // Don't compile class initializers, ever.
  if (((access_flags & kAccConstructor) != 0) && ((access_flags & kAccStatic) != 0)) {
    return false;
  }
  return (Runtime::Current()->GetCompilerFilter() != Runtime::kInterpretOnly);
}

const std::vector<uint8_t>* VerifiedMethodsData::GenerateGcMap(
    verifier::MethodVerifier* method_verifier) {
  size_t num_entries, ref_bitmap_bits, pc_bits;
  ComputeGcMapSizes(method_verifier, &num_entries, &ref_bitmap_bits, &pc_bits);
  // There's a single byte to encode the size of each bitmap
  if (ref_bitmap_bits >= (8 /* bits per byte */ * 8192 /* 13-bit size */ )) {
    // TODO: either a better GC map format or per method failures
    method_verifier->Fail(verifier::VERIFY_ERROR_BAD_CLASS_HARD)
        << "Cannot encode GC map for method with " << ref_bitmap_bits << " registers";
    return NULL;
  }
  size_t ref_bitmap_bytes = (ref_bitmap_bits + 7) / 8;
  // There are 2 bytes to encode the number of entries
  if (num_entries >= 65536) {
    // TODO: either a better GC map format or per method failures
    method_verifier->Fail(verifier::VERIFY_ERROR_BAD_CLASS_HARD)
        << "Cannot encode GC map for method with " << num_entries << " entries";
    return NULL;
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
    // TODO: either a better GC map format or per method failures
    method_verifier->Fail(verifier::VERIFY_ERROR_BAD_CLASS_HARD)
        << "Cannot encode GC map for method with "
        << (1 << pc_bits) << " instructions (number is rounded up to nearest power of 2)";
    return NULL;
  }
  size_t table_size = ((pc_bytes + ref_bitmap_bytes) * num_entries) + 4;
  std::vector<uint8_t>* table = new std::vector<uint8_t>;
  if (table == NULL) {
    method_verifier->Fail(verifier::VERIFY_ERROR_BAD_CLASS_HARD)
        << "Failed to encode GC map (size=" << table_size << ")";
    return NULL;
  }
  table->reserve(table_size);
  // Write table header
  table->push_back(format | ((ref_bitmap_bytes & ~0xFF) >> 5));
  table->push_back(ref_bitmap_bytes & 0xFF);
  table->push_back(num_entries & 0xFF);
  table->push_back((num_entries >> 8) & 0xFF);
  // Write table data
  const DexFile::CodeItem* code_item = method_verifier->CodeItem();
  for (size_t i = 0; i < code_item->insns_size_in_code_units_; i++) {
    if (method_verifier->GetInstructionFlags(i).IsCompileTimeInfoPoint()) {
      table->push_back(i & 0xFF);
      if (pc_bytes == 2) {
        table->push_back((i >> 8) & 0xFF);
      }
      verifier::RegisterLine* line = method_verifier->GetRegLine(i);
      line->WriteReferenceBitMap(*table, ref_bitmap_bytes);
    }
  }
  DCHECK_EQ(table->size(), table_size);
  return table;
}

void VerifiedMethodsData::VerifyGcMap(verifier::MethodVerifier* method_verifier,
                                      const std::vector<uint8_t>& data) {
  // Check that for every GC point there is a map entry, there aren't entries for non-GC points,
  // that the table data is well formed and all references are marked (or not) in the bitmap
  verifier::DexPcToReferenceMap map(&data[0]);
  DCHECK_EQ(data.size(), map.RawSize());
  size_t map_index = 0;
  const DexFile::CodeItem* code_item = method_verifier->CodeItem();
  for (size_t i = 0; i < code_item->insns_size_in_code_units_; i++) {
    const uint8_t* reg_bitmap = map.FindBitMap(i, false);
    if (method_verifier->GetInstructionFlags(i).IsCompileTimeInfoPoint()) {
      CHECK_LT(map_index, map.NumEntries());
      CHECK_EQ(map.GetDexPc(map_index), i);
      CHECK_EQ(map.GetBitMap(map_index), reg_bitmap);
      map_index++;
      verifier::RegisterLine* line = method_verifier->GetRegLine(i);
      for (size_t j = 0; j < code_item->registers_size_; j++) {
        if (line->GetRegisterType(j).IsNonZeroReferenceTypes()) {
          CHECK_LT(j / 8, map.RegWidth());
          CHECK_EQ((reg_bitmap[j / 8] >> (j % 8)) & 1, 1);
        } else if ((j / 8) < map.RegWidth()) {
          CHECK_EQ((reg_bitmap[j / 8] >> (j % 8)) & 1, 0);
        } else {
          // If a register doesn't contain a reference then the bitmap may be shorter than the line
        }
      }
    } else {
      CHECK(reg_bitmap == NULL);
    }
  }
}

void VerifiedMethodsData::ComputeGcMapSizes(verifier::MethodVerifier* method_verifier,
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
  *ref_bitmap_bits = max_ref_reg + 1;  // if max register is 0 we need 1 bit to encode (ie +1)
  size_t i = 0;
  while ((1U << i) <= max_insn) {
    i++;
  }
  *log2_max_gc_pc = i;
}

void VerifiedMethodsData::SetDexGcMap(MethodReference ref, const std::vector<uint8_t>* gc_map) {
  DCHECK(Runtime::Current()->IsCompiler());
  {
    WriterMutexLock mu(Thread::Current(), dex_gc_maps_lock_);
    DexGcMapTable::iterator it = dex_gc_maps_.find(ref);
    if (it != dex_gc_maps_.end()) {
      delete it->second;
      dex_gc_maps_.erase(it);
    }
    dex_gc_maps_.Put(ref, gc_map);
  }
  DCHECK(GetDexGcMap(ref) != NULL);
}

VerifiedMethodsData::MethodSafeCastSet* VerifiedMethodsData::GenerateSafeCastSet(
    verifier::MethodVerifier* method_verifier) {
  /*
   * Walks over the method code and adds any cast instructions in which
   * the type cast is implicit to a set, which is used in the code generation
   * to elide these casts.
   */
  if (method_verifier->HasFailures()) {
    return NULL;
  }
  UniquePtr<MethodSafeCastSet> mscs;
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
        if (mscs.get() == nullptr) {
          mscs.reset(new MethodSafeCastSet());
        }
        mscs->insert(dex_pc);
      }
    }
  }
  return mscs.release();
}

void  VerifiedMethodsData::SetSafeCastMap(MethodReference ref, const MethodSafeCastSet* cast_set) {
  WriterMutexLock mu(Thread::Current(), safecast_map_lock_);
  SafeCastMap::iterator it = safecast_map_.find(ref);
  if (it != safecast_map_.end()) {
    delete it->second;
    safecast_map_.erase(it);
  }
  safecast_map_.Put(ref, cast_set);
  DCHECK(safecast_map_.find(ref) != safecast_map_.end());
}

VerifiedMethodsData::PcToConcreteMethodMap* VerifiedMethodsData::GenerateDevirtMap(
    verifier::MethodVerifier* method_verifier) {
  // It is risky to rely on reg_types for sharpening in cases of soft
  // verification, we might end up sharpening to a wrong implementation. Just abort.
  if (method_verifier->HasFailures()) {
    return NULL;
  }

  UniquePtr<PcToConcreteMethodMap> pc_to_concrete_method_map;
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
      if (pc_to_concrete_method_map.get() == NULL) {
        pc_to_concrete_method_map.reset(new PcToConcreteMethodMap());
      }
      MethodReference concrete_ref(
          concrete_method->GetDeclaringClass()->GetDexCache()->GetDexFile(),
          concrete_method->GetDexMethodIndex());
      pc_to_concrete_method_map->Put(dex_pc, concrete_ref);
    }
  }
  return pc_to_concrete_method_map.release();
}

void  VerifiedMethodsData::SetDevirtMap(MethodReference ref,
                                   const PcToConcreteMethodMap* devirt_map) {
  WriterMutexLock mu(Thread::Current(), devirt_maps_lock_);
  DevirtualizationMapTable::iterator it = devirt_maps_.find(ref);
  if (it != devirt_maps_.end()) {
    delete it->second;
    devirt_maps_.erase(it);
  }

  devirt_maps_.Put(ref, devirt_map);
  DCHECK(devirt_maps_.find(ref) != devirt_maps_.end());
}

}  // namespace art
