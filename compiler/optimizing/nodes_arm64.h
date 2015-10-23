/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_NODES_ARM64_H_
#define ART_COMPILER_OPTIMIZING_NODES_ARM64_H_

namespace art {

// This instruction computes an intermediate address pointing in the 'middle' of an object. The
// result pointer cannot be handled by GC, so extra care is taken to make sure that this value is
// never used across anything that can trigger GC.
class HArm64IntermediateAddress : public HExpression<2> {
 public:
  HArm64IntermediateAddress(HInstruction* base_address, HInstruction* offset, uint32_t dex_pc)
      : HExpression(Primitive::kPrimNot, SideEffects::DependsOnGC(), dex_pc) {
    SetRawInputAt(0, base_address);
    SetRawInputAt(1, offset);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE { return true; }

  HInstruction* GetBaseAddress() const { return InputAt(0); }
  HInstruction* GetOffset() const { return InputAt(1); }

  DECLARE_INSTRUCTION(Arm64IntermediateAddress);

 private:
  DISALLOW_COPY_AND_ASSIGN(HArm64IntermediateAddress);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_ARM64_H_
