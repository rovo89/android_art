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
  size_t number_of_dex_registers = 2;
  stream.AddStackMapEntry(0, 64, 0x3, &sp_mask, number_of_dex_registers, 0);
  stream.AddDexRegisterEntry(0, DexRegisterLocation::Kind::kInStack, 0);
  stream.AddDexRegisterEntry(1, DexRegisterLocation::Kind::kConstant, -2);

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
  ASSERT_EQ(0u, stack_map.GetDexPc(code_info));
  ASSERT_EQ(64u, stack_map.GetNativePcOffset(code_info));
  ASSERT_EQ(0x3u, stack_map.GetRegisterMask(code_info));

  MemoryRegion stack_mask = stack_map.GetStackMask(code_info);
  ASSERT_TRUE(SameBits(stack_mask, sp_mask));

  ASSERT_TRUE(stack_map.HasDexRegisterMap(code_info));
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

  ASSERT_FALSE(stack_map.HasInlineInfo(code_info));
}

TEST(StackMapTest, Test2) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  StackMapStream stream(&arena);

  ArenaBitVector sp_mask1(&arena, 0, true);
  sp_mask1.SetBit(2);
  sp_mask1.SetBit(4);
  size_t number_of_dex_registers = 2;
  stream.AddStackMapEntry(0, 64, 0x3, &sp_mask1, number_of_dex_registers, 2);
  stream.AddDexRegisterEntry(0, DexRegisterLocation::Kind::kInStack, 0);
  stream.AddDexRegisterEntry(1, DexRegisterLocation::Kind::kConstant, -2);
  stream.AddInlineInfoEntry(42);
  stream.AddInlineInfoEntry(82);

  ArenaBitVector sp_mask2(&arena, 0, true);
  sp_mask2.SetBit(3);
  sp_mask1.SetBit(8);
  stream.AddStackMapEntry(1, 128, 0xFF, &sp_mask2, number_of_dex_registers, 0);
  stream.AddDexRegisterEntry(0, DexRegisterLocation::Kind::kInRegister, 18);
  stream.AddDexRegisterEntry(1, DexRegisterLocation::Kind::kInFpuRegister, 3);

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
    ASSERT_EQ(0u, stack_map.GetDexPc(code_info));
    ASSERT_EQ(64u, stack_map.GetNativePcOffset(code_info));
    ASSERT_EQ(0x3u, stack_map.GetRegisterMask(code_info));

    MemoryRegion stack_mask = stack_map.GetStackMask(code_info);
    ASSERT_TRUE(SameBits(stack_mask, sp_mask1));

    ASSERT_TRUE(stack_map.HasDexRegisterMap(code_info));
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

    ASSERT_TRUE(stack_map.HasInlineInfo(code_info));
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
    ASSERT_EQ(1u, stack_map.GetDexPc(code_info));
    ASSERT_EQ(128u, stack_map.GetNativePcOffset(code_info));
    ASSERT_EQ(0xFFu, stack_map.GetRegisterMask(code_info));

    MemoryRegion stack_mask = stack_map.GetStackMask(code_info);
    ASSERT_TRUE(SameBits(stack_mask, sp_mask2));

    ASSERT_TRUE(stack_map.HasDexRegisterMap(code_info));
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

    ASSERT_FALSE(stack_map.HasInlineInfo(code_info));
  }
}

TEST(StackMapTest, TestNonLiveDexRegisters) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  StackMapStream stream(&arena);

  ArenaBitVector sp_mask(&arena, 0, false);
  uint32_t number_of_dex_registers = 2;
  stream.AddStackMapEntry(0, 64, 0x3, &sp_mask, number_of_dex_registers, 0);
  stream.AddDexRegisterEntry(0, DexRegisterLocation::Kind::kNone, 0);
  stream.AddDexRegisterEntry(1, DexRegisterLocation::Kind::kConstant, -2);

  size_t size = stream.ComputeNeededSize();
  void* memory = arena.Alloc(size, kArenaAllocMisc);
  MemoryRegion region(memory, size);
  stream.FillIn(region);

  CodeInfo code_info(region);
  StackMap stack_map = code_info.GetStackMapAt(0);
  ASSERT_TRUE(stack_map.HasDexRegisterMap(code_info));
  DexRegisterMap dex_registers = code_info.GetDexRegisterMapOf(stack_map, 2);
  ASSERT_EQ(DexRegisterLocation::Kind::kNone,
            dex_registers.GetLocationKind(0, number_of_dex_registers));
  ASSERT_EQ(DexRegisterLocation::Kind::kConstant,
            dex_registers.GetLocationKind(1, number_of_dex_registers));
  ASSERT_EQ(-2, dex_registers.GetConstant(1, number_of_dex_registers));
  ASSERT_FALSE(stack_map.HasInlineInfo(code_info));
}

// Generate a stack map whose dex register offset is
// StackMap::kNoDexRegisterMapSmallEncoding, and ensure we do
// not treat it as kNoDexRegisterMap.
TEST(StackMapTest, DexRegisterMapOffsetOverflow) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  StackMapStream stream(&arena);

  ArenaBitVector sp_mask(&arena, 0, false);
  uint32_t number_of_dex_registers = 0xEA;
  stream.AddStackMapEntry(0, 64, 0x3, &sp_mask, number_of_dex_registers, 0);
  for (uint32_t i = 0; i < number_of_dex_registers - 9; ++i) {
    stream.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kConstant, 0);
  }
  stream.AddStackMapEntry(0, 64, 0x3, &sp_mask, number_of_dex_registers, 0);
  for (uint32_t i = 0; i < number_of_dex_registers; ++i) {
    stream.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kConstant, 0);
  }

  size_t size = stream.ComputeNeededSize();
  void* memory = arena.Alloc(size, kArenaAllocMisc);
  MemoryRegion region(memory, size);
  stream.FillIn(region);

  CodeInfo code_info(region);
  StackMap stack_map = code_info.GetStackMapAt(1);
  ASSERT_TRUE(stack_map.HasDexRegisterMap(code_info));
  ASSERT_NE(stack_map.GetDexRegisterMapOffset(code_info), StackMap::kNoDexRegisterMap);
  ASSERT_EQ(stack_map.GetDexRegisterMapOffset(code_info), StackMap::kNoDexRegisterMapSmallEncoding);
}

}  // namespace art
