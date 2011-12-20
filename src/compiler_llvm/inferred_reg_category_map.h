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

#ifndef ART_SRC_COMPILER_LLVM_INFERRED_REG_CATEGORY_MAP_H_
#define ART_SRC_COMPILER_LLVM_INFERRED_REG_CATEGORY_MAP_H_

#include "backend_types.h"

#include <stdint.h>
#include <vector>

namespace art {
namespace compiler_llvm {


class InferredRegCategoryMap {
 private:
  typedef std::vector<uint8_t> RegCategoryLine;

 public:
  InferredRegCategoryMap(uint32_t insns_size_in_code_units, uint8_t regs_size);

  ~InferredRegCategoryMap();

  RegCategory GetRegCategory(uint32_t dex_pc, uint16_t reg_idx) const;
  void SetRegCategory(uint32_t dex_pc, uint16_t reg_idx, RegCategory cat);

  bool operator==(InferredRegCategoryMap const& rhs) const;
  bool operator!=(InferredRegCategoryMap const& rhs) const;

 private:
  uint16_t registers_size_;

  std::vector<RegCategoryLine*> lines_;

  DISALLOW_COPY_AND_ASSIGN(InferredRegCategoryMap);
};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_INFERRED_REG_CATEGORY_MAP_H_
