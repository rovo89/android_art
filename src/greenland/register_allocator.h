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

#ifndef ART_SRC_GREENLAND_REGISTER_ALLOCATOR_H_
#define ART_SRC_GREENLAND_REGISTER_ALLOCATOR_H_

#include "backend_types.h"

#include "lir_basic_block.h"

#include <stdint.h>
#include <list>
#include <map>

namespace art {
namespace greenland {

class LIR;
class LIRFunction;
class TargetRegisterInfo;

class RegisterAllocator  {
 private:
  enum RegTypeTag {
    kPhyRegType = 0, kFrameType, kInStackType, kNoneType = -1
  };

  struct Storage {
    int index;
    RegTypeTag regTag;
  };

  // Storage Allocation
  Storage AllocateStorage(LIRFunction &lir_func, unsigned vreg_idx);
  void KeepStorage(const LIR& lir);
  void FreeStorage(unsigned vreg_idx);
  void HandleInsnCopy(LIRBasicBlock& bb,
                      LIRBasicBlock::iterator it,
                      LIRBasicBlock::iterator next_lir);

  // Pre-RA
  void BuildKillInfo(LIRFunction& lir_func);
  void PHIElimination(LIRFunction& lir_func);

  void InitializeAllocation(LIRFunction& lir_func);
  void PreRegisterAllocation(LIRFunction& lir_func);
  void FunctionRegisterAllocation(LIRFunction& lir_func);
 public:
  RegisterAllocator(const TargetRegisterInfo& info) : reg_info_(info) { }
  ~RegisterAllocator() { }

  void AllocateRegisters(LIRFunction& lir_func) {
    InitializeAllocation(lir_func);
    PreRegisterAllocation(lir_func);
    FunctionRegisterAllocation(lir_func);
  }

 private:
  std::list<unsigned> allocatable_list_;
  std::list<unsigned> stackstorage_list_;
  std::map<unsigned, Storage> allocated_map_;
  const TargetRegisterInfo& reg_info_;
  DISALLOW_COPY_AND_ASSIGN(RegisterAllocator);
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_REGISTER_ALLOCATOR_H_
