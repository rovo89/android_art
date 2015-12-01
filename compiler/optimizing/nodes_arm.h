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

#ifndef ART_COMPILER_OPTIMIZING_NODES_ARM_H_
#define ART_COMPILER_OPTIMIZING_NODES_ARM_H_

namespace art {

class HArmDexCacheArraysBase : public HExpression<0> {
 public:
  explicit HArmDexCacheArraysBase(const DexFile& dex_file)
      : HExpression(Primitive::kPrimInt, SideEffects::None(), kNoDexPc),
        dex_file_(&dex_file),
        element_offset_(static_cast<size_t>(-1)) { }

  void UpdateElementOffset(size_t element_offset) {
    // Use the lowest offset from the requested elements so that all offsets from
    // this base are non-negative because our assemblers emit negative-offset loads
    // as a sequence of two or more instructions. (However, positive offsets beyond
    // 4KiB also require two or more instructions, so this simple heuristic could
    // be improved for cases where there is a dense cluster of elements far from
    // the lowest offset. This is expected to be rare enough though, so we choose
    // not to spend compile time on elaborate calculations.)
    element_offset_ = std::min(element_offset_, element_offset);
  }

  const DexFile& GetDexFile() const {
    return *dex_file_;
  }

  size_t GetElementOffset() const {
    return element_offset_;
  }

  DECLARE_INSTRUCTION(ArmDexCacheArraysBase);

 private:
  const DexFile* dex_file_;
  size_t element_offset_;

  DISALLOW_COPY_AND_ASSIGN(HArmDexCacheArraysBase);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_ARM_H_
