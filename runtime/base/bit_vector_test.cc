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

#include "UniquePtr.h"
#include "bit_vector.h"
#include "gtest/gtest.h"

namespace art {

TEST(BitVector, Test) {
  const size_t kBits = 32;

  BitVector bv(kBits, false, Allocator::GetMallocAllocator());
  EXPECT_EQ(1U, bv.GetStorageSize());
  EXPECT_EQ(sizeof(uint32_t), bv.GetSizeOf());
  EXPECT_FALSE(bv.IsExpandable());

  EXPECT_EQ(0U, bv.NumSetBits());
  EXPECT_EQ(0U, bv.NumSetBits(1));
  EXPECT_EQ(0U, bv.NumSetBits(kBits));
  for (size_t i = 0; i < kBits; i++) {
    EXPECT_FALSE(bv.IsBitSet(i));
  }
  EXPECT_EQ(0U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0U, *bv.GetRawStorage());

  BitVector::Iterator empty_iterator(&bv);
  EXPECT_EQ(-1, empty_iterator.Next());

  UniquePtr<BitVector::Iterator> empty_iterator_on_heap(bv.GetIterator());
  EXPECT_EQ(-1, empty_iterator_on_heap->Next());

  bv.SetBit(0);
  bv.SetBit(kBits - 1);
  EXPECT_EQ(2U, bv.NumSetBits());
  EXPECT_EQ(1U, bv.NumSetBits(1));
  EXPECT_EQ(2U, bv.NumSetBits(kBits));
  EXPECT_TRUE(bv.IsBitSet(0));
  for (size_t i = 1; i < kBits - 1; i++) {
    EXPECT_FALSE(bv.IsBitSet(i));
  }
  EXPECT_TRUE(bv.IsBitSet(kBits - 1));
  EXPECT_EQ(0x80000001U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0x80000001U, *bv.GetRawStorage());

  BitVector::Iterator iterator(&bv);
  EXPECT_EQ(0, iterator.Next());
  EXPECT_EQ(static_cast<int>(kBits - 1), iterator.Next());
  EXPECT_EQ(-1, iterator.Next());
}

TEST(BitVector, NoopAllocator) {
  const uint32_t kWords = 2;

  uint32_t bits[kWords];
  memset(bits, 0, sizeof(bits));

  BitVector bv(0U, false, Allocator::GetNoopAllocator(), kWords, bits);
  EXPECT_EQ(kWords, bv.GetStorageSize());
  EXPECT_EQ(kWords * sizeof(uint32_t), bv.GetSizeOf());
  EXPECT_EQ(bits, bv.GetRawStorage());
  EXPECT_EQ(0U, bv.NumSetBits());

  bv.SetBit(8);
  EXPECT_EQ(1U, bv.NumSetBits());
  EXPECT_EQ(0x00000100U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0x00000000U, bv.GetRawStorageWord(1));
  EXPECT_EQ(1U, bv.NumSetBits());

  bv.SetBit(16);
  EXPECT_EQ(2U, bv.NumSetBits());
  EXPECT_EQ(0x00010100U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0x00000000U, bv.GetRawStorageWord(1));
  EXPECT_EQ(2U, bv.NumSetBits());

  bv.SetBit(32);
  EXPECT_EQ(3U, bv.NumSetBits());
  EXPECT_EQ(0x00010100U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0x00000001U, bv.GetRawStorageWord(1));
  EXPECT_EQ(3U, bv.NumSetBits());

  bv.SetBit(48);
  EXPECT_EQ(4U, bv.NumSetBits());
  EXPECT_EQ(0x00010100U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0x00010001U, bv.GetRawStorageWord(1));
  EXPECT_EQ(4U, bv.NumSetBits());

  EXPECT_EQ(0U, bv.NumSetBits(1));

  EXPECT_EQ(0U, bv.NumSetBits(8));
  EXPECT_EQ(1U, bv.NumSetBits(9));
  EXPECT_EQ(1U, bv.NumSetBits(10));

  EXPECT_EQ(1U, bv.NumSetBits(16));
  EXPECT_EQ(2U, bv.NumSetBits(17));
  EXPECT_EQ(2U, bv.NumSetBits(18));

  EXPECT_EQ(2U, bv.NumSetBits(32));
  EXPECT_EQ(3U, bv.NumSetBits(33));
  EXPECT_EQ(3U, bv.NumSetBits(34));

  EXPECT_EQ(3U, bv.NumSetBits(48));
  EXPECT_EQ(4U, bv.NumSetBits(49));
  EXPECT_EQ(4U, bv.NumSetBits(50));

  EXPECT_EQ(4U, bv.NumSetBits(64));
}

TEST(BitVector, SetInitialBits) {
  const uint32_t kWords = 2;

  uint32_t bits[kWords];
  memset(bits, 0, sizeof(bits));

  BitVector bv(0U, false, Allocator::GetNoopAllocator(), kWords, bits);
  bv.SetInitialBits(0u);
  EXPECT_EQ(0u, bv.NumSetBits());
  bv.SetInitialBits(1u);
  EXPECT_EQ(1u, bv.NumSetBits());
  bv.SetInitialBits(32u);
  EXPECT_EQ(32u, bv.NumSetBits());
  bv.SetInitialBits(63u);
  EXPECT_EQ(63u, bv.NumSetBits());
  bv.SetInitialBits(64u);
  EXPECT_EQ(64u, bv.NumSetBits());
}

}  // namespace art
