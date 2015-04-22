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

#include "common_runtime_test.h"

#include "art_method-inl.h"
#include "class_linker.h"
#include "jit_code_cache.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"

namespace art {
namespace jit {

class JitCodeCacheTest : public CommonRuntimeTest {
 public:
};

TEST_F(JitCodeCacheTest, TestCoverage) {
  std::string error_msg;
  constexpr size_t kSize = 1 * MB;
  std::unique_ptr<JitCodeCache> code_cache(
      JitCodeCache::Create(kSize, &error_msg));
  ASSERT_TRUE(code_cache.get() != nullptr) << error_msg;
  ASSERT_TRUE(code_cache->CodeCachePtr() != nullptr);
  ASSERT_EQ(code_cache->CodeCacheSize(), 0u);
  ASSERT_GT(code_cache->CodeCacheRemain(), 0u);
  ASSERT_TRUE(code_cache->DataCachePtr() != nullptr);
  ASSERT_EQ(code_cache->DataCacheSize(), 0u);
  ASSERT_GT(code_cache->DataCacheRemain(), 0u);
  ASSERT_EQ(code_cache->CodeCacheRemain() + code_cache->DataCacheRemain(), kSize);
  ASSERT_EQ(code_cache->NumMethods(), 0u);
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  uint8_t* const reserved_code = code_cache->ReserveCode(soa.Self(), 4 * KB);
  ASSERT_TRUE(reserved_code != nullptr);
  ASSERT_TRUE(code_cache->ContainsCodePtr(reserved_code));
  ASSERT_EQ(code_cache->NumMethods(), 1u);
  ClassLinker* const cl = Runtime::Current()->GetClassLinker();
  auto* method = cl->AllocArtMethodArray(soa.Self(), 1);
  ASSERT_FALSE(code_cache->ContainsMethod(method));
  method->SetEntryPointFromQuickCompiledCode(reserved_code);
  ASSERT_TRUE(code_cache->ContainsMethod(method));
  ASSERT_EQ(code_cache->GetCodeFor(method), reserved_code);
  // Save the code and then change it.
  code_cache->SaveCompiledCode(method, reserved_code);
  method->SetEntryPointFromQuickCompiledCode(nullptr);
  ASSERT_EQ(code_cache->GetCodeFor(method), reserved_code);
  const uint8_t data_arr[] = {1, 2, 3, 4, 5};
  uint8_t* data_ptr = code_cache->AddDataArray(soa.Self(), data_arr, data_arr + sizeof(data_arr));
  ASSERT_TRUE(data_ptr != nullptr);
  ASSERT_EQ(memcmp(data_ptr, data_arr, sizeof(data_arr)), 0);
}

TEST_F(JitCodeCacheTest, TestOverflow) {
  std::string error_msg;
  constexpr size_t kSize = 1 * MB;
  std::unique_ptr<JitCodeCache> code_cache(
      JitCodeCache::Create(kSize, &error_msg));
  ASSERT_TRUE(code_cache.get() != nullptr) << error_msg;
  ASSERT_TRUE(code_cache->CodeCachePtr() != nullptr);
  size_t code_bytes = 0;
  size_t data_bytes = 0;
  constexpr size_t kCodeArrSize = 4 * KB;
  constexpr size_t kDataArrSize = 4 * KB;
  uint8_t data_arr[kDataArrSize];
  std::fill_n(data_arr, arraysize(data_arr), 53);
  // Add code and data until we are full.
  uint8_t* code_ptr = nullptr;
  uint8_t* data_ptr = nullptr;
  do {
    code_ptr = code_cache->ReserveCode(Thread::Current(), kCodeArrSize);
    data_ptr = code_cache->AddDataArray(Thread::Current(), data_arr, data_arr + kDataArrSize);
    if (code_ptr != nullptr) {
      code_bytes += kCodeArrSize;
    }
    if (data_ptr != nullptr) {
      data_bytes += kDataArrSize;
    }
  } while (code_ptr != nullptr || data_ptr != nullptr);
  // Make sure we added a reasonable amount
  CHECK_GT(code_bytes, 0u);
  CHECK_LE(code_bytes, kSize);
  CHECK_GT(data_bytes, 0u);
  CHECK_LE(data_bytes, kSize);
  CHECK_GE(code_bytes + data_bytes, kSize * 4 / 5);
}

}  // namespace jit
}  // namespace art
