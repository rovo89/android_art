/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_RUNTIME_CHECK_REFERENCE_MAP_VISITOR_H_
#define ART_RUNTIME_CHECK_REFERENCE_MAP_VISITOR_H_

#include "gc_map.h"
#include "mirror/art_method-inl.h"
#include "scoped_thread_state_change.h"
#include "stack_map.h"

namespace art {

// Helper class for tests checking that the compiler keeps track of dex registers
// holding references.
class CheckReferenceMapVisitor : public StackVisitor {
 public:
  explicit CheckReferenceMapVisitor(Thread* thread) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr) {}

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* m = GetMethod();
    if (m->IsCalleeSaveMethod() || m->IsNative()) {
      CHECK_EQ(GetDexPc(), DexFile::kDexNoIndex);
    }

    if (!m || m->IsNative() || m->IsRuntimeMethod() || IsShadowFrame()) {
      return true;
    }

    LOG(INFO) << "At " << PrettyMethod(m, false);

    if (m->IsCalleeSaveMethod()) {
      LOG(WARNING) << "no PC for " << PrettyMethod(m);
      return true;
    }

    return false;
  }

  void CheckReferences(int* registers, int number_of_references, uint32_t native_pc_offset)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (GetMethod()->IsOptimized()) {
      CheckOptimizedMethod(registers, number_of_references, native_pc_offset);
    } else {
      CheckQuickMethod(registers, number_of_references, native_pc_offset);
    }
  }

 private:
  void CheckOptimizedMethod(int* registers, int number_of_references, uint32_t native_pc_offset)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* m = GetMethod();
    CodeInfo code_info = m->GetOptimizedCodeInfo();
    StackMap stack_map = code_info.GetStackMapForNativePcOffset(native_pc_offset);
    DexRegisterMap dex_register_map = code_info.GetDexRegisterMapOf(stack_map, m->GetCodeItem()->registers_size_);
    MemoryRegion stack_mask = stack_map.GetStackMask();
    uint32_t register_mask = stack_map.GetRegisterMask();
    for (int i = 0; i < number_of_references; ++i) {
      int reg = registers[i];
      CHECK(reg < m->GetCodeItem()->registers_size_);
      DexRegisterMap::LocationKind location = dex_register_map.GetLocationKind(reg);
      switch (location) {
        case DexRegisterMap::kNone:
          // Not set, should not be a reference.
          CHECK(false);
          break;
        case DexRegisterMap::kInStack:
          CHECK(stack_mask.LoadBit(dex_register_map.GetValue(reg) >> 2));
          break;
        case DexRegisterMap::kInRegister:
          CHECK_NE(register_mask & dex_register_map.GetValue(reg), 0u);
          break;
        case DexRegisterMap::kInFpuRegister:
          // In Fpu register, should not be a reference.
          CHECK(false);
          break;
        case DexRegisterMap::kConstant:
          CHECK_EQ(dex_register_map.GetValue(reg), 0);
          break;
      }
    }
  }

  void CheckQuickMethod(int* registers, int number_of_references, uint32_t native_pc_offset)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* m = GetMethod();
    NativePcOffsetToReferenceMap map(m->GetNativeGcMap());
    const uint8_t* ref_bitmap = map.FindBitMap(native_pc_offset);
    CHECK(ref_bitmap);
    for (int i = 0; i < number_of_references; ++i) {
      int reg = registers[i];
      CHECK(reg < m->GetCodeItem()->registers_size_);
      CHECK((*((ref_bitmap) + reg / 8) >> (reg % 8) ) & 0x01)
          << "Error: Reg @" << i << " is not in GC map";
    }
  }
};

}  // namespace art

#endif  // ART_RUNTIME_CHECK_REFERENCE_MAP_VISITOR_H_
