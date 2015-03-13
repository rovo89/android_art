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

static bool SameBits(MemoryRegion region, const BitVector& bit_vector) {
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
  StackMapStream stream(&arena);

  ArenaBitVector sp_mask(&arena, 0, false);
  ArenaBitVector live_registers_mask(&arena, 0, true);
  live_registers_mask.SetBit(0);
  live_registers_mask.SetBit(1);
  size_t number_of_dex_registers = 2;
  stream.AddStackMapEntry(0, 64, 0x3, &sp_mask, number_of_dex_registers, 0, &live_registers_mask);
  stream.AddDexRegisterEntry(DexRegisterLocation::Kind::kInStack, 0);
  stream.AddDexRegisterEntry(DexRegisterLocation::Kind::kConstant, -2);

  size_t size = stream.ComputeNeededSize();
  void* memory = arena.Alloc(size, kArenaAllocMisc);
  MemoryRegion region(memory, size);
  stream.FillIn(region);

  CodeInfo code_info(region);
  ASSERT_EQ(0u, code_info.GetStackMaskSize());
  ASSERT_EQ(1u, code_info.GetNumberOfStackMaps());

  StackMap stack_map = code_info.GetStackMapAt(0);
  ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForDexPc(0)));
  ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForNativePcOffset(64)));
  ASSERT_EQ(0u, stack_map.GetDexPc());
  ASSERT_EQ(64u, stack_map.GetNativePcOffset());
  ASSERT_EQ(0x3u, stack_map.GetRegisterMask());

  MemoryRegion stack_mask = stack_map.GetStackMask();
  ASSERT_TRUE(SameBits(stack_mask, sp_mask));

  ASSERT_TRUE(stack_map.HasDexRegisterMap());
  DexRegisterMap dex_registers = code_info.GetDexRegisterMapOf(stack_map, number_of_dex_registers);
  ASSERT_EQ(7u, dex_registers.Size());
  DexRegisterLocation location0 = dex_registers.GetLocationKindAndValue(0, number_of_dex_registers);
  DexRegisterLocation location1 = dex_registers.GetLocationKindAndValue(1, number_of_dex_registers);
  ASSERT_EQ(DexRegisterLocation::Kind::kInStack, location0.GetKind());
  ASSERT_EQ(DexRegisterLocation::Kind::kConstant, location1.GetKind());
  ASSERT_EQ(DexRegisterLocation::Kind::kInStack, location0.GetInternalKind());
  ASSERT_EQ(DexRegisterLocation::Kind::kConstantLargeValue, location1.GetInternalKind());
  ASSERT_EQ(0, location0.GetValue());
  ASSERT_EQ(-2, location1.GetValue());

  ASSERT_FALSE(stack_map.HasInlineInfo());
}

TEST(StackMapTest, Test2) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  StackMapStream stream(&arena);

  ArenaBitVector sp_mask1(&arena, 0, true);
  sp_mask1.SetBit(2);
  sp_mask1.SetBit(4);
  size_t number_of_dex_registers = 2;
  ArenaBitVector live_registers_mask1(&arena, 0, true);
  live_registers_mask1.SetBit(0);
  live_registers_mask1.SetBit(1);
  stream.AddStackMapEntry(0, 64, 0x3, &sp_mask1, number_of_dex_registers, 2, &live_registers_mask1);
  stream.AddDexRegisterEntry(DexRegisterLocation::Kind::kInStack, 0);
  stream.AddDexRegisterEntry(DexRegisterLocation::Kind::kConstant, -2);
  stream.AddInlineInfoEntry(42);
  stream.AddInlineInfoEntry(82);

  ArenaBitVector sp_mask2(&arena, 0, true);
  sp_mask2.SetBit(3);
  sp_mask1.SetBit(8);
  ArenaBitVector live_registers_mask2(&arena, 0, true);
  live_registers_mask2.SetBit(0);
  live_registers_mask2.SetBit(1);
  stream.AddStackMapEntry(1, 128, 0xFF, &sp_mask2, number_of_dex_registers, 0, &live_registers_mask2);
  stream.AddDexRegisterEntry(DexRegisterLocation::Kind::kInRegister, 18);
  stream.AddDexRegisterEntry(DexRegisterLocation::Kind::kInFpuRegister, 3);

  size_t size = stream.ComputeNeededSize();
  void* memory = arena.Alloc(size, kArenaAllocMisc);
  MemoryRegion region(memory, size);
  stream.FillIn(region);

  CodeInfo code_info(region);
  ASSERT_EQ(1u, code_info.GetStackMaskSize());
  ASSERT_EQ(2u, code_info.GetNumberOfStackMaps());

  // First stack map.
  {
    StackMap stack_map = code_info.GetStackMapAt(0);
    ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForDexPc(0)));
    ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForNativePcOffset(64)));
    ASSERT_EQ(0u, stack_map.GetDexPc());
    ASSERT_EQ(64u, stack_map.GetNativePcOffset());
    ASSERT_EQ(0x3u, stack_map.GetRegisterMask());

    MemoryRegion stack_mask = stack_map.GetStackMask();
    ASSERT_TRUE(SameBits(stack_mask, sp_mask1));

    ASSERT_TRUE(stack_map.HasDexRegisterMap());
    DexRegisterMap dex_registers =
        code_info.GetDexRegisterMapOf(stack_map, number_of_dex_registers);
    ASSERT_EQ(7u, dex_registers.Size());
    DexRegisterLocation location0 =
        dex_registers.GetLocationKindAndValue(0, number_of_dex_registers);
    DexRegisterLocation location1 =
        dex_registers.GetLocationKindAndValue(1, number_of_dex_registers);
    ASSERT_EQ(DexRegisterLocation::Kind::kInStack, location0.GetKind());
    ASSERT_EQ(DexRegisterLocation::Kind::kConstant, location1.GetKind());
    ASSERT_EQ(DexRegisterLocation::Kind::kInStack, location0.GetInternalKind());
    ASSERT_EQ(DexRegisterLocation::Kind::kConstantLargeValue, location1.GetInternalKind());
    ASSERT_EQ(0, location0.GetValue());
    ASSERT_EQ(-2, location1.GetValue());

    ASSERT_TRUE(stack_map.HasInlineInfo());
    InlineInfo inline_info = code_info.GetInlineInfoOf(stack_map);
    ASSERT_EQ(2u, inline_info.GetDepth());
    ASSERT_EQ(42u, inline_info.GetMethodReferenceIndexAtDepth(0));
    ASSERT_EQ(82u, inline_info.GetMethodReferenceIndexAtDepth(1));
  }

  // Second stack map.
  {
    StackMap stack_map = code_info.GetStackMapAt(1);
    ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForDexPc(1u)));
    ASSERT_TRUE(stack_map.Equals(code_info.GetStackMapForNativePcOffset(128u)));
    ASSERT_EQ(1u, stack_map.GetDexPc());
    ASSERT_EQ(128u, stack_map.GetNativePcOffset());
    ASSERT_EQ(0xFFu, stack_map.GetRegisterMask());

    MemoryRegion stack_mask = stack_map.GetStackMask();
    ASSERT_TRUE(SameBits(stack_mask, sp_mask2));

    ASSERT_TRUE(stack_map.HasDexRegisterMap());
    DexRegisterMap dex_registers =
        code_info.GetDexRegisterMapOf(stack_map, number_of_dex_registers);
    ASSERT_EQ(3u, dex_registers.Size());
    DexRegisterLocation location0 =
        dex_registers.GetLocationKindAndValue(0, number_of_dex_registers);
    DexRegisterLocation location1 =
        dex_registers.GetLocationKindAndValue(1, number_of_dex_registers);
    ASSERT_EQ(DexRegisterLocation::Kind::kInRegister, location0.GetKind());
    ASSERT_EQ(DexRegisterLocation::Kind::kInFpuRegister, location1.GetKind());
    ASSERT_EQ(DexRegisterLocation::Kind::kInRegister, location0.GetInternalKind());
    ASSERT_EQ(DexRegisterLocation::Kind::kInFpuRegister, location1.GetInternalKind());
    ASSERT_EQ(18, location0.GetValue());
    ASSERT_EQ(3, location1.GetValue());

    ASSERT_FALSE(stack_map.HasInlineInfo());
  }
}

TEST(StackMapTest, TestNonLiveDexRegisters) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  StackMapStream stream(&arena);

  ArenaBitVector sp_mask(&arena, 0, false);
  ArenaBitVector live_registers_mask(&arena, 0, true);
  live_registers_mask.SetBit(1);
  uint32_t number_of_dex_registers = 2;
  stream.AddStackMapEntry(0, 64, 0x3, &sp_mask, number_of_dex_registers, 0, &live_registers_mask);
  stream.AddDexRegisterEntry(DexRegisterLocation::Kind::kConstant, -2);

  size_t size = stream.ComputeNeededSize();
  void* memory = arena.Alloc(size, kArenaAllocMisc);
  MemoryRegion region(memory, size);
  stream.FillIn(region);

  CodeInfo code_info(region);
  StackMap stack_map = code_info.GetStackMapAt(0);
  ASSERT_TRUE(stack_map.HasDexRegisterMap());
  DexRegisterMap dex_registers = code_info.GetDexRegisterMapOf(stack_map, 2);
  ASSERT_EQ(DexRegisterLocation::Kind::kNone,
            dex_registers.GetLocationKind(0, number_of_dex_registers));
  ASSERT_EQ(DexRegisterLocation::Kind::kConstant,
            dex_registers.GetLocationKind(1, number_of_dex_registers));
  ASSERT_EQ(-2, dex_registers.GetConstant(1, number_of_dex_registers));
  ASSERT_FALSE(stack_map.HasInlineInfo());
}

}  // namespace art
