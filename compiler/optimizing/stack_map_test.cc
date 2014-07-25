/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "stack_map.h"
#include "stack_map_stream.h"
#include "utils/arena_bit_vector.h"

#include "gtest/gtest.h"

namespace art {

bool SameBits(MemoryRegion region, const BitVector& bit_vector) {
  for (size_t i = 0; i < region.size_in_bits(); ++i) {
    if (region.LoadBit(i) != bit_vector.IsBitSet(i)) {
      return false;
    }
  }
  return true;
}

TEST(StackMapTest, Test1) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  StackMapStream<size_t> stream(&arena);

  ArenaBitVector sp_mask(&arena, 0, false);
  stream.AddStackMapEntry(0, 64, 0x3, &sp_mask, 2, 0);
  stream.AddDexRegisterEntry(DexRegisterMap::kInStack, 0);
  stream.AddDexRegisterEntry(DexRegisterMap::kConstant, -2);

  size_t size = stream.ComputeNeededSize();
  void* memory = arena.Alloc(size, kArenaAllocMisc);
  MemoryRegion region(memory, size);
  stream.FillIn(region);

  CodeInfo<size_t> code_info(region);
  ASSERT_EQ(0u, code_info.GetStackMaskSize());
  ASSERT_EQ(1u, code_info.GetNumberOfStackMaps());

  StackMap<size_t> stack_map = code_info.GetStackMapAt(0);
  ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForDexPc(0)));
  ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForNativePc(64)));
  ASSERT_EQ(0u, stack_map.GetDexPc());
  ASSERT_EQ(64u, stack_map.GetNativePc());
  ASSERT_EQ(0x3u, stack_map.GetRegisterMask());
  ASSERT_FALSE(stack_map.HasInlineInfo());

  MemoryRegion stack_mask = stack_map.GetStackMask();
  ASSERT_TRUE(SameBits(stack_mask, sp_mask));

  DexRegisterMap dex_registers = code_info.GetDexRegisterMapOf(stack_map, 2);
  ASSERT_EQ(DexRegisterMap::kInStack, dex_registers.GetLocationKind(0));
  ASSERT_EQ(DexRegisterMap::kConstant, dex_registers.GetLocationKind(1));
  ASSERT_EQ(0, dex_registers.GetValue(0));
  ASSERT_EQ(-2, dex_registers.GetValue(1));
}

TEST(StackMapTest, Test2) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  StackMapStream<size_t> stream(&arena);

  ArenaBitVector sp_mask1(&arena, 0, true);
  sp_mask1.SetBit(2);
  sp_mask1.SetBit(4);
  stream.AddStackMapEntry(0, 64, 0x3, &sp_mask1, 2, 2);
  stream.AddDexRegisterEntry(DexRegisterMap::kInStack, 0);
  stream.AddDexRegisterEntry(DexRegisterMap::kConstant, -2);
  stream.AddInlineInfoEntry(42);
  stream.AddInlineInfoEntry(82);

  ArenaBitVector sp_mask2(&arena, 0, true);
  sp_mask2.SetBit(3);
  sp_mask1.SetBit(8);
  stream.AddStackMapEntry(1, 128, 0xFF, &sp_mask2, 1, 0);
  stream.AddDexRegisterEntry(DexRegisterMap::kInRegister, 0);

  size_t size = stream.ComputeNeededSize();
  void* memory = arena.Alloc(size, kArenaAllocMisc);
  MemoryRegion region(memory, size);
  stream.FillIn(region);

  CodeInfo<size_t> code_info(region);
  ASSERT_EQ(1u, code_info.GetStackMaskSize());
  ASSERT_EQ(2u, code_info.GetNumberOfStackMaps());

  StackMap<size_t> stack_map = code_info.GetStackMapAt(0);
  ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForDexPc(0)));
  ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForNativePc(64)));
  ASSERT_EQ(0u, stack_map.GetDexPc());
  ASSERT_EQ(64u, stack_map.GetNativePc());
  ASSERT_EQ(0x3u, stack_map.GetRegisterMask());

  MemoryRegion stack_mask = stack_map.GetStackMask();
  ASSERT_TRUE(SameBits(stack_mask, sp_mask1));

  DexRegisterMap dex_registers = code_info.GetDexRegisterMapOf(stack_map, 2);
  ASSERT_EQ(DexRegisterMap::kInStack, dex_registers.GetLocationKind(0));
  ASSERT_EQ(DexRegisterMap::kConstant, dex_registers.GetLocationKind(1));
  ASSERT_EQ(0, dex_registers.GetValue(0));
  ASSERT_EQ(-2, dex_registers.GetValue(1));

  InlineInfo inline_info = code_info.GetInlineInfoOf(stack_map);
  ASSERT_EQ(2u, inline_info.GetDepth());
  ASSERT_EQ(42u, inline_info.GetMethodReferenceIndexAtDepth(0));
  ASSERT_EQ(82u, inline_info.GetMethodReferenceIndexAtDepth(1));

  stack_map = code_info.GetStackMapAt(1);
  ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForDexPc(1u)));
  ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForNativePc(128u)));
  ASSERT_EQ(1u, stack_map.GetDexPc());
  ASSERT_EQ(128u, stack_map.GetNativePc());
  ASSERT_EQ(0xFFu, stack_map.GetRegisterMask());

  stack_mask = stack_map.GetStackMask();
  ASSERT_TRUE(SameBits(stack_mask, sp_mask2));

  ASSERT_FALSE(stack_map.HasInlineInfo());
}

}  // namespace art
