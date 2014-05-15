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

#include "mem_map.h"

#include "UniquePtr.h"
#include "gtest/gtest.h"

namespace art {

class MemMapTest : public testing::Test {
 public:
  static byte* BaseBegin(MemMap* mem_map) {
    return reinterpret_cast<byte*>(mem_map->base_begin_);
  }
  static size_t BaseSize(MemMap* mem_map) {
    return mem_map->base_size_;
  }

  static void RemapAtEndTest(bool low_4gb) {
    std::string error_msg;
    // Cast the page size to size_t.
    const size_t page_size = static_cast<size_t>(kPageSize);
    // Map a two-page memory region.
    MemMap* m0 = MemMap::MapAnonymous("MemMapTest_RemapAtEndTest_map0",
                                      nullptr,
                                      2 * page_size,
                                      PROT_READ | PROT_WRITE,
                                      low_4gb,
                                      &error_msg);
    // Check its state and write to it.
    byte* base0 = m0->Begin();
    ASSERT_TRUE(base0 != nullptr) << error_msg;
    size_t size0 = m0->Size();
    EXPECT_EQ(m0->Size(), 2 * page_size);
    EXPECT_EQ(BaseBegin(m0), base0);
    EXPECT_EQ(BaseSize(m0), size0);
    memset(base0, 42, 2 * page_size);
    // Remap the latter half into a second MemMap.
    MemMap* m1 = m0->RemapAtEnd(base0 + page_size,
                                "MemMapTest_RemapAtEndTest_map1",
                                PROT_READ | PROT_WRITE,
                                &error_msg);
    // Check the states of the two maps.
    EXPECT_EQ(m0->Begin(), base0) << error_msg;
    EXPECT_EQ(m0->Size(), page_size);
    EXPECT_EQ(BaseBegin(m0), base0);
    EXPECT_EQ(BaseSize(m0), page_size);
    byte* base1 = m1->Begin();
    size_t size1 = m1->Size();
    EXPECT_EQ(base1, base0 + page_size);
    EXPECT_EQ(size1, page_size);
    EXPECT_EQ(BaseBegin(m1), base1);
    EXPECT_EQ(BaseSize(m1), size1);
    // Write to the second region.
    memset(base1, 43, page_size);
    // Check the contents of the two regions.
    for (size_t i = 0; i < page_size; ++i) {
      EXPECT_EQ(base0[i], 42);
    }
    for (size_t i = 0; i < page_size; ++i) {
      EXPECT_EQ(base1[i], 43);
    }
    // Unmap the first region.
    delete m0;
    // Make sure the second region is still accessible after the first
    // region is unmapped.
    for (size_t i = 0; i < page_size; ++i) {
      EXPECT_EQ(base1[i], 43);
    }
    delete m1;
  }
};

TEST_F(MemMapTest, MapAnonymousEmpty) {
  std::string error_msg;
  UniquePtr<MemMap> map(MemMap::MapAnonymous("MapAnonymousEmpty",
                                             nullptr,
                                             0,
                                             PROT_READ,
                                             false,
                                             &error_msg));
  ASSERT_TRUE(map.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  map.reset(MemMap::MapAnonymous("MapAnonymousEmpty",
                                 nullptr,
                                 kPageSize,
                                 PROT_READ | PROT_WRITE,
                                 false,
                                 &error_msg));
  ASSERT_TRUE(map.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
}

#ifdef __LP64__
TEST_F(MemMapTest, MapAnonymousEmpty32bit) {
  std::string error_msg;
  UniquePtr<MemMap> map(MemMap::MapAnonymous("MapAnonymousEmpty",
                                             nullptr,
                                             kPageSize,
                                             PROT_READ | PROT_WRITE,
                                             true,
                                             &error_msg));
  ASSERT_TRUE(map.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  ASSERT_LT(reinterpret_cast<uintptr_t>(BaseBegin(map.get())), 1ULL << 32);
}
#endif

TEST_F(MemMapTest, MapAnonymousExactAddr) {
  std::string error_msg;
  // Map at an address that should work, which should succeed.
  UniquePtr<MemMap> map0(MemMap::MapAnonymous("MapAnonymous0",
                                              reinterpret_cast<byte*>(ART_BASE_ADDRESS),
                                              kPageSize,
                                              PROT_READ | PROT_WRITE,
                                              false,
                                              &error_msg));
  ASSERT_TRUE(map0.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  ASSERT_TRUE(map0->BaseBegin() == reinterpret_cast<void*>(ART_BASE_ADDRESS));
  // Map at an unspecified address, which should succeed.
  UniquePtr<MemMap> map1(MemMap::MapAnonymous("MapAnonymous1",
                                              nullptr,
                                              kPageSize,
                                              PROT_READ | PROT_WRITE,
                                              false,
                                              &error_msg));
  ASSERT_TRUE(map1.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  ASSERT_TRUE(map1->BaseBegin() != nullptr);
  // Attempt to map at the same address, which should fail.
  UniquePtr<MemMap> map2(MemMap::MapAnonymous("MapAnonymous2",
                                              reinterpret_cast<byte*>(map1->BaseBegin()),
                                              kPageSize,
                                              PROT_READ | PROT_WRITE,
                                              false,
                                              &error_msg));
  ASSERT_TRUE(map2.get() == nullptr) << error_msg;
  ASSERT_TRUE(!error_msg.empty());
}

TEST_F(MemMapTest, RemapAtEnd) {
  RemapAtEndTest(false);
}

#ifdef __LP64__
TEST_F(MemMapTest, RemapAtEnd32bit) {
  RemapAtEndTest(true);
}
#endif

TEST_F(MemMapTest, MapAnonymousExactAddr32bitHighAddr) {
  std::string error_msg;
  UniquePtr<MemMap> map(MemMap::MapAnonymous("MapAnonymousExactAddr32bitHighAddr",
                                             reinterpret_cast<byte*>(0x71000000),
                                             0x21000000,
                                             PROT_READ | PROT_WRITE,
                                             true,
                                             &error_msg));
  ASSERT_TRUE(map.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  ASSERT_EQ(reinterpret_cast<uintptr_t>(BaseBegin(map.get())), 0x71000000U);
}

TEST_F(MemMapTest, MapAnonymousOverflow) {
  std::string error_msg;
  uintptr_t ptr = 0;
  ptr -= kPageSize;  // Now it's close to the top.
  UniquePtr<MemMap> map(MemMap::MapAnonymous("MapAnonymousOverflow",
                                             reinterpret_cast<byte*>(ptr),
                                             2 * kPageSize,  // brings it over the top.
                                             PROT_READ | PROT_WRITE,
                                             false,
                                             &error_msg));
  ASSERT_EQ(nullptr, map.get());
  ASSERT_FALSE(error_msg.empty());
}

#ifdef __LP64__
TEST_F(MemMapTest, MapAnonymousLow4GBExpectedTooHigh) {
  std::string error_msg;
  UniquePtr<MemMap> map(MemMap::MapAnonymous("MapAnonymousLow4GBExpectedTooHigh",
                                             reinterpret_cast<byte*>(UINT64_C(0x100000000)),
                                             kPageSize,
                                             PROT_READ | PROT_WRITE,
                                             true,
                                             &error_msg));
  ASSERT_EQ(nullptr, map.get());
  ASSERT_FALSE(error_msg.empty());
}

TEST_F(MemMapTest, MapAnonymousLow4GBRangeTooHigh) {
  std::string error_msg;
  UniquePtr<MemMap> map(MemMap::MapAnonymous("MapAnonymousLow4GBRangeTooHigh",
                                             reinterpret_cast<byte*>(0xF0000000),
                                             0x20000000,
                                             PROT_READ | PROT_WRITE,
                                             true,
                                             &error_msg));
  ASSERT_EQ(nullptr, map.get());
  ASSERT_FALSE(error_msg.empty());
}
#endif

}  // namespace art
