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

#include <gtest/gtest.h>

#include "compiled_method.h"

namespace art {

TEST(CompiledMethod, SrcMapElemOperators) {
  SrcMapElem elems[] = {
      { 1u, -1 },
      { 1u, 0 },
      { 1u, 1 },
      { 2u, -1 },
      { 2u, 0 },    // Index 4.
      { 2u, 1 },
      { 2u, 0u },   // Index 6: Arbitrarily add identical SrcMapElem with index 4.
  };

  for (size_t i = 0; i != arraysize(elems); ++i) {
    for (size_t j = 0; j != arraysize(elems); ++j) {
      bool expected = (i != 6u ? i : 4u) == (j != 6u ? j : 4u);
      EXPECT_EQ(expected, elems[i] == elems[j]) << i << " " << j;
    }
  }

  for (size_t i = 0; i != arraysize(elems); ++i) {
    for (size_t j = 0; j != arraysize(elems); ++j) {
      bool expected = (i != 6u ? i : 4u) < (j != 6u ? j : 4u);
      EXPECT_EQ(expected, elems[i] < elems[j]) << i << " " << j;
    }
  }
}

TEST(CompiledMethod, LinkerPatchOperators) {
  const DexFile* dex_file1 = reinterpret_cast<const DexFile*>(1);
  const DexFile* dex_file2 = reinterpret_cast<const DexFile*>(2);
  LinkerPatch patches[] = {
      LinkerPatch::MethodPatch(16u, dex_file1, 1000u),
      LinkerPatch::MethodPatch(16u, dex_file1, 1001u),
      LinkerPatch::MethodPatch(16u, dex_file2, 1000u),
      LinkerPatch::MethodPatch(16u, dex_file2, 1001u),  // Index 3.
      LinkerPatch::CodePatch(16u, dex_file1, 1000u),
      LinkerPatch::CodePatch(16u, dex_file1, 1001u),
      LinkerPatch::CodePatch(16u, dex_file2, 1000u),
      LinkerPatch::CodePatch(16u, dex_file2, 1001u),
      LinkerPatch::RelativeCodePatch(16u, dex_file1, 1000u),
      LinkerPatch::RelativeCodePatch(16u, dex_file1, 1001u),
      LinkerPatch::RelativeCodePatch(16u, dex_file2, 1000u),
      LinkerPatch::RelativeCodePatch(16u, dex_file2, 1001u),
      LinkerPatch::TypePatch(16u, dex_file1, 1000u),
      LinkerPatch::TypePatch(16u, dex_file1, 1001u),
      LinkerPatch::TypePatch(16u, dex_file2, 1000u),
      LinkerPatch::TypePatch(16u, dex_file2, 1001u),
      LinkerPatch::DexCacheArrayPatch(16u, dex_file1, 3000u, 2000u),
      LinkerPatch::DexCacheArrayPatch(16u, dex_file1, 3001u, 2000u),
      LinkerPatch::DexCacheArrayPatch(16u, dex_file1, 3000u, 2001u),
      LinkerPatch::DexCacheArrayPatch(16u, dex_file1, 3001u, 2001u),
      LinkerPatch::DexCacheArrayPatch(16u, dex_file2, 3000u, 2000u),
      LinkerPatch::DexCacheArrayPatch(16u, dex_file2, 3001u, 2000u),
      LinkerPatch::DexCacheArrayPatch(16u, dex_file2, 3000u, 2001u),
      LinkerPatch::DexCacheArrayPatch(16u, dex_file2, 3001u, 2001u),
      LinkerPatch::MethodPatch(32u, dex_file1, 1000u),
      LinkerPatch::MethodPatch(32u, dex_file1, 1001u),
      LinkerPatch::MethodPatch(32u, dex_file2, 1000u),
      LinkerPatch::MethodPatch(32u, dex_file2, 1001u),
      LinkerPatch::CodePatch(32u, dex_file1, 1000u),
      LinkerPatch::CodePatch(32u, dex_file1, 1001u),
      LinkerPatch::CodePatch(32u, dex_file2, 1000u),
      LinkerPatch::CodePatch(32u, dex_file2, 1001u),
      LinkerPatch::RelativeCodePatch(32u, dex_file1, 1000u),
      LinkerPatch::RelativeCodePatch(32u, dex_file1, 1001u),
      LinkerPatch::RelativeCodePatch(32u, dex_file2, 1000u),
      LinkerPatch::RelativeCodePatch(32u, dex_file2, 1001u),
      LinkerPatch::TypePatch(32u, dex_file1, 1000u),
      LinkerPatch::TypePatch(32u, dex_file1, 1001u),
      LinkerPatch::TypePatch(32u, dex_file2, 1000u),
      LinkerPatch::TypePatch(32u, dex_file2, 1001u),
      LinkerPatch::DexCacheArrayPatch(32u, dex_file1, 3000u, 2000u),
      LinkerPatch::DexCacheArrayPatch(32u, dex_file1, 3001u, 2000u),
      LinkerPatch::DexCacheArrayPatch(32u, dex_file1, 3000u, 2001u),
      LinkerPatch::DexCacheArrayPatch(32u, dex_file1, 3001u, 2001u),
      LinkerPatch::DexCacheArrayPatch(32u, dex_file2, 3000u, 2000u),
      LinkerPatch::DexCacheArrayPatch(32u, dex_file2, 3001u, 2000u),
      LinkerPatch::DexCacheArrayPatch(32u, dex_file2, 3000u, 2001u),
      LinkerPatch::DexCacheArrayPatch(32u, dex_file2, 3001u, 2001u),
      LinkerPatch::MethodPatch(16u, dex_file2, 1001u),  // identical with patch as index 3.
  };
  constexpr size_t last_index = arraysize(patches) - 1u;

  for (size_t i = 0; i != arraysize(patches); ++i) {
    for (size_t j = 0; j != arraysize(patches); ++j) {
      bool expected = (i != last_index ? i : 3u) == (j != last_index ? j : 3u);
      EXPECT_EQ(expected, patches[i] == patches[j]) << i << " " << j;
    }
  }

  for (size_t i = 0; i != arraysize(patches); ++i) {
    for (size_t j = 0; j != arraysize(patches); ++j) {
      bool expected = (i != last_index ? i : 3u) < (j != last_index ? j : 3u);
      EXPECT_EQ(expected, patches[i] < patches[j]) << i << " " << j;
    }
  }
}

}  // namespace art
