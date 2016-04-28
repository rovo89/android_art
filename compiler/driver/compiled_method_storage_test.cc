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

#include "compiled_method_storage.h"
#include "compiled_method.h"
#include "compiler_driver.h"
#include "compiler_options.h"
#include "dex/verification_results.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"

namespace art {

TEST(CompiledMethodStorage, Deduplicate) {
  CompilerOptions compiler_options;
  VerificationResults verification_results(&compiler_options);
  DexFileToMethodInlinerMap method_inliner_map;
  CompilerDriver driver(&compiler_options,
                        &verification_results,
                        &method_inliner_map,
                        Compiler::kOptimizing,
                        /* instruction_set_ */ kNone,
                        /* instruction_set_features */ nullptr,
                        /* boot_image */ false,
                        /* app_image */ false,
                        /* image_classes */ nullptr,
                        /* compiled_classes */ nullptr,
                        /* compiled_methods */ nullptr,
                        /* thread_count */ 1u,
                        /* dump_stats */ false,
                        /* dump_passes */ false,
                        /* timer */ nullptr,
                        /* swap_fd */ -1,
                        /* profile_compilation_info */ nullptr);
  CompiledMethodStorage* storage = driver.GetCompiledMethodStorage();

  ASSERT_TRUE(storage->DedupeEnabled());  // The default.

  const uint8_t raw_code1[] = { 1u, 2u, 3u };
  const uint8_t raw_code2[] = { 4u, 3u, 2u, 1u };
  ArrayRef<const uint8_t> code[] = {
      ArrayRef<const uint8_t>(raw_code1),
      ArrayRef<const uint8_t>(raw_code2),
  };
  const SrcMapElem raw_src_map1[] = { { 1u, 2u }, { 3u, 4u }, { 5u, 6u } };
  const SrcMapElem raw_src_map2[] = { { 8u, 7u }, { 6u, 5u }, { 4u, 3u }, { 2u, 1u } };
  ArrayRef<const SrcMapElem> src_map[] = {
      ArrayRef<const SrcMapElem>(raw_src_map1),
      ArrayRef<const SrcMapElem>(raw_src_map2),
  };
  const uint8_t raw_vmap_table1[] = { 2, 4, 6 };
  const uint8_t raw_vmap_table2[] = { 7, 5, 3, 1 };
  ArrayRef<const uint8_t> vmap_table[] = {
      ArrayRef<const uint8_t>(raw_vmap_table1),
      ArrayRef<const uint8_t>(raw_vmap_table2),
  };
  const uint8_t raw_cfi_info1[] = { 1, 3, 5 };
  const uint8_t raw_cfi_info2[] = { 8, 6, 4, 2 };
  ArrayRef<const uint8_t> cfi_info[] = {
      ArrayRef<const uint8_t>(raw_cfi_info1),
      ArrayRef<const uint8_t>(raw_cfi_info2),
  };
  const LinkerPatch raw_patches1[] = {
      LinkerPatch::CodePatch(0u, nullptr, 1u),
      LinkerPatch::MethodPatch(4u, nullptr, 1u),
  };
  const LinkerPatch raw_patches2[] = {
      LinkerPatch::CodePatch(0u, nullptr, 1u),
      LinkerPatch::MethodPatch(4u, nullptr, 2u),
  };
  ArrayRef<const LinkerPatch> patches[] = {
      ArrayRef<const LinkerPatch>(raw_patches1),
      ArrayRef<const LinkerPatch>(raw_patches2),
  };

  std::vector<CompiledMethod*> compiled_methods;
  compiled_methods.reserve(1u << 7);
  for (auto&& c : code) {
    for (auto&& s : src_map) {
      for (auto&& v : vmap_table) {
        for (auto&& f : cfi_info) {
          for (auto&& p : patches) {
            compiled_methods.push_back(CompiledMethod::SwapAllocCompiledMethod(
                &driver, kNone, c, 0u, 0u, 0u, s, v, f, p));
          }
        }
      }
    }
  }
  constexpr size_t code_bit = 1u << 4;
  constexpr size_t src_map_bit = 1u << 3;
  constexpr size_t vmap_table_bit = 1u << 2;
  constexpr size_t cfi_info_bit = 1u << 1;
  constexpr size_t patches_bit = 1u << 0;
  CHECK_EQ(compiled_methods.size(), 1u << 5);
  for (size_t i = 0; i != compiled_methods.size(); ++i) {
    for (size_t j = 0; j != compiled_methods.size(); ++j) {
      CompiledMethod* lhs = compiled_methods[i];
      CompiledMethod* rhs = compiled_methods[j];
      bool same_code = ((i ^ j) & code_bit) == 0u;
      bool same_src_map = ((i ^ j) & src_map_bit) == 0u;
      bool same_vmap_table = ((i ^ j) & vmap_table_bit) == 0u;
      bool same_cfi_info = ((i ^ j) & cfi_info_bit) == 0u;
      bool same_patches = ((i ^ j) & patches_bit) == 0u;
      ASSERT_EQ(same_code, lhs->GetQuickCode().data() == rhs->GetQuickCode().data())
          << i << " " << j;
      ASSERT_EQ(same_src_map, lhs->GetSrcMappingTable().data() == rhs->GetSrcMappingTable().data())
          << i << " " << j;
      ASSERT_EQ(same_vmap_table, lhs->GetVmapTable().data() == rhs->GetVmapTable().data())
          << i << " " << j;
      ASSERT_EQ(same_cfi_info, lhs->GetCFIInfo().data() == rhs->GetCFIInfo().data())
          << i << " " << j;
      ASSERT_EQ(same_patches, lhs->GetPatches().data() == rhs->GetPatches().data())
          << i << " " << j;
    }
  }
  for (CompiledMethod* method : compiled_methods) {
    CompiledMethod::ReleaseSwapAllocatedCompiledMethod(&driver, method);
  }
}

}  // namespace art
