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

#include "linker/relative_patcher_test.h"
#include "linker/arm/relative_patcher_thumb2.h"

namespace art {
namespace linker {

class Thumb2RelativePatcherTest : public RelativePatcherTest {
 public:
  Thumb2RelativePatcherTest() : RelativePatcherTest(kThumb2, "default") { }

 protected:
  static const uint8_t kCallRawCode[];
  static const ArrayRef<const uint8_t> kCallCode;
  static const uint8_t kNopRawCode[];
  static const ArrayRef<const uint8_t> kNopCode;

  // Branches within range [-256, 256) can be created from these by adding the low 8 bits.
  static constexpr uint32_t kBlPlus0 = 0xf000f800;
  static constexpr uint32_t kBlMinus256 = 0xf7ffff00;

  // Special BL values.
  static constexpr uint32_t kBlPlusMax = 0xf3ffd7ff;
  static constexpr uint32_t kBlMinusMax = 0xf400d000;

  bool Create2MethodsWithGap(const ArrayRef<const uint8_t>& method1_code,
                             const ArrayRef<const LinkerPatch>& method1_patches,
                             const ArrayRef<const uint8_t>& method3_code,
                             const ArrayRef<const LinkerPatch>& method3_patches,
                             uint32_t distance_without_thunks) {
    CHECK_EQ(distance_without_thunks % kArmAlignment, 0u);
    const uint32_t method1_offset =
        CompiledCode::AlignCode(kTrampolineSize, kThumb2) + sizeof(OatQuickMethodHeader);
    AddCompiledMethod(MethodRef(1u), method1_code, method1_patches);

    // We want to put the method3 at a very precise offset.
    const uint32_t method3_offset = method1_offset + distance_without_thunks;
    CHECK(IsAligned<kArmAlignment>(method3_offset - sizeof(OatQuickMethodHeader)));

    // Calculate size of method2 so that we put method3 at the correct place.
    const uint32_t method2_offset =
        CompiledCode::AlignCode(method1_offset + method1_code.size(), kThumb2) +
        sizeof(OatQuickMethodHeader);
    const uint32_t method2_size = (method3_offset - sizeof(OatQuickMethodHeader) - method2_offset);
    std::vector<uint8_t> method2_raw_code(method2_size);
    ArrayRef<const uint8_t> method2_code(method2_raw_code);
    AddCompiledMethod(MethodRef(2u), method2_code, ArrayRef<const LinkerPatch>());

    AddCompiledMethod(MethodRef(3u), method3_code, method3_patches);

    Link();

    // Check assumptions.
    CHECK_EQ(GetMethodOffset(1), method1_offset);
    CHECK_EQ(GetMethodOffset(2), method2_offset);
    auto result3 = method_offset_map_.FindMethodOffset(MethodRef(3));
    CHECK(result3.first);
    // There may be a thunk before method2.
    if (result3.second == method3_offset + 1 /* thumb mode */) {
      return false;  // No thunk.
    } else {
      uint32_t aligned_thunk_size = CompiledCode::AlignCode(ThunkSize(), kThumb2);
      CHECK_EQ(result3.second, method3_offset + aligned_thunk_size + 1 /* thumb mode */);
      return true;   // Thunk present.
    }
  }

  uint32_t GetMethodOffset(uint32_t method_idx) {
    auto result = method_offset_map_.FindMethodOffset(MethodRef(method_idx));
    CHECK(result.first);
    CHECK_NE(result.second & 1u, 0u);
    return result.second - 1 /* thumb mode */;
  }

  uint32_t ThunkSize() {
    return static_cast<Thumb2RelativePatcher*>(patcher_.get())->thunk_code_.size();
  }

  bool CheckThunk(uint32_t thunk_offset) {
    Thumb2RelativePatcher* patcher = static_cast<Thumb2RelativePatcher*>(patcher_.get());
    ArrayRef<const uint8_t> expected_code(patcher->thunk_code_);
    if (output_.size() < thunk_offset + expected_code.size()) {
      LOG(ERROR) << "output_.size() == " << output_.size() << " < "
          << "thunk_offset + expected_code.size() == " << (thunk_offset + expected_code.size());
      return false;
    }
    ArrayRef<const uint8_t> linked_code(&output_[thunk_offset], expected_code.size());
    if (linked_code == expected_code) {
      return true;
    }
    // Log failure info.
    DumpDiff(expected_code, linked_code);
    return false;
  }

  std::vector<uint8_t> GenNopsAndBl(size_t num_nops, uint32_t bl) {
    std::vector<uint8_t> result;
    result.reserve(num_nops * 2u + 4u);
    for (size_t i = 0; i != num_nops; ++i) {
      result.push_back(0x00);
      result.push_back(0xbf);
    }
    result.push_back(static_cast<uint8_t>(bl >> 16));
    result.push_back(static_cast<uint8_t>(bl >> 24));
    result.push_back(static_cast<uint8_t>(bl));
    result.push_back(static_cast<uint8_t>(bl >> 8));
    return result;
  }

  void TestDexCachereference(uint32_t dex_cache_arrays_begin, uint32_t element_offset) {
    dex_cache_arrays_begin_ = dex_cache_arrays_begin;
    static const uint8_t raw_code[] = {
        0x40, 0xf2, 0x00, 0x00,   // MOVW r0, #0 (placeholder)
        0xc0, 0xf2, 0x00, 0x00,   // MOVT r0, #0 (placeholder)
        0x78, 0x44,               // ADD r0, pc
    };
    constexpr uint32_t pc_insn_offset = 8u;
    const ArrayRef<const uint8_t> code(raw_code);
    LinkerPatch patches[] = {
        LinkerPatch::DexCacheArrayPatch(0u, nullptr, pc_insn_offset, element_offset),
        LinkerPatch::DexCacheArrayPatch(4u, nullptr, pc_insn_offset, element_offset),
    };
    AddCompiledMethod(MethodRef(1u), code, ArrayRef<const LinkerPatch>(patches));
    Link();

    uint32_t method1_offset = GetMethodOffset(1u);
    uint32_t pc_base_offset = method1_offset + pc_insn_offset + 4u /* PC adjustment */;
    uint32_t diff = dex_cache_arrays_begin_ + element_offset - pc_base_offset;
    // Distribute the bits of the diff between the MOVW and MOVT:
    uint32_t diffw = diff & 0xffffu;
    uint32_t difft = diff >> 16;
    uint32_t movw = 0xf2400000u |           // MOVW r0, #0 (placeholder),
        ((diffw & 0xf000u) << (16 - 12)) |  // move imm4 from bits 12-15 to bits 16-19,
        ((diffw & 0x0800u) << (26 - 11)) |  // move imm from bit 11 to bit 26,
        ((diffw & 0x0700u) << (12 - 8)) |   // move imm3 from bits 8-10 to bits 12-14,
        ((diffw & 0x00ffu));                // keep imm8 at bits 0-7.
    uint32_t movt = 0xf2c00000u |           // MOVT r0, #0 (placeholder),
        ((difft & 0xf000u) << (16 - 12)) |  // move imm4 from bits 12-15 to bits 16-19,
        ((difft & 0x0800u) << (26 - 11)) |  // move imm from bit 11 to bit 26,
        ((difft & 0x0700u) << (12 - 8)) |   // move imm3 from bits 8-10 to bits 12-14,
        ((difft & 0x00ffu));                // keep imm8 at bits 0-7.
    const uint8_t expected_code[] = {
        static_cast<uint8_t>(movw >> 16), static_cast<uint8_t>(movw >> 24),
        static_cast<uint8_t>(movw >> 0), static_cast<uint8_t>(movw >> 8),
        static_cast<uint8_t>(movt >> 16), static_cast<uint8_t>(movt >> 24),
        static_cast<uint8_t>(movt >> 0), static_cast<uint8_t>(movt >> 8),
        0x78, 0x44,
    };
    EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
  }
};

const uint8_t Thumb2RelativePatcherTest::kCallRawCode[] = {
    0x00, 0xf0, 0x00, 0xf8
};

const ArrayRef<const uint8_t> Thumb2RelativePatcherTest::kCallCode(kCallRawCode);

const uint8_t Thumb2RelativePatcherTest::kNopRawCode[] = {
    0x00, 0xbf
};

const ArrayRef<const uint8_t> Thumb2RelativePatcherTest::kNopCode(kNopRawCode);

TEST_F(Thumb2RelativePatcherTest, CallSelf) {
  LinkerPatch patches[] = {
      LinkerPatch::RelativeCodePatch(0u, nullptr, 1u),
  };
  AddCompiledMethod(MethodRef(1u), kCallCode, ArrayRef<const LinkerPatch>(patches));
  Link();

  static const uint8_t expected_code[] = {
      0xff, 0xf7, 0xfe, 0xff
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(Thumb2RelativePatcherTest, CallOther) {
  LinkerPatch method1_patches[] = {
      LinkerPatch::RelativeCodePatch(0u, nullptr, 2u),
  };
  AddCompiledMethod(MethodRef(1u), kCallCode, ArrayRef<const LinkerPatch>(method1_patches));
  LinkerPatch method2_patches[] = {
      LinkerPatch::RelativeCodePatch(0u, nullptr, 1u),
  };
  AddCompiledMethod(MethodRef(2u), kCallCode, ArrayRef<const LinkerPatch>(method2_patches));
  Link();

  uint32_t method1_offset = GetMethodOffset(1u);
  uint32_t method2_offset = GetMethodOffset(2u);
  uint32_t diff_after = method2_offset - (method1_offset + 4u /* PC adjustment */);
  ASSERT_EQ(diff_after & 1u, 0u);
  ASSERT_LT(diff_after >> 1, 1u << 8);  // Simple encoding, (diff_after >> 1) fits into 8 bits.
  static const uint8_t method1_expected_code[] = {
      0x00, 0xf0, static_cast<uint8_t>(diff_after >> 1), 0xf8
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(method1_expected_code)));
  uint32_t diff_before = method1_offset - (method2_offset + 4u /* PC adjustment */);
  ASSERT_EQ(diff_before & 1u, 0u);
  ASSERT_GE(diff_before, -1u << 9);  // Simple encoding, -256 <= (diff >> 1) < 0.
  auto method2_expected_code = GenNopsAndBl(0u, kBlMinus256 | ((diff_before >> 1) & 0xffu));
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(2u), ArrayRef<const uint8_t>(method2_expected_code)));
}

TEST_F(Thumb2RelativePatcherTest, CallTrampoline) {
  LinkerPatch patches[] = {
      LinkerPatch::RelativeCodePatch(0u, nullptr, 2u),
  };
  AddCompiledMethod(MethodRef(1u), kCallCode, ArrayRef<const LinkerPatch>(patches));
  Link();

  uint32_t method1_offset = GetMethodOffset(1u);
  uint32_t diff = kTrampolineOffset - (method1_offset + 4u);
  ASSERT_EQ(diff & 1u, 0u);
  ASSERT_GE(diff, -1u << 9);  // Simple encoding, -256 <= (diff >> 1) < 0 (checked as unsigned).
  auto expected_code = GenNopsAndBl(0u, kBlMinus256 | ((diff >> 1) & 0xffu));
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(Thumb2RelativePatcherTest, CallOtherAlmostTooFarAfter) {
  auto method1_raw_code = GenNopsAndBl(3u, kBlPlus0);
  constexpr uint32_t bl_offset_in_method1 = 3u * 2u;  // After NOPs.
  ArrayRef<const uint8_t> method1_code(method1_raw_code);
  ASSERT_EQ(bl_offset_in_method1 + 4u, method1_code.size());
  LinkerPatch method1_patches[] = {
      LinkerPatch::RelativeCodePatch(bl_offset_in_method1, nullptr, 3u),
  };

  constexpr uint32_t max_positive_disp = 16 * MB - 2u + 4u /* PC adjustment */;
  bool thunk_in_gap = Create2MethodsWithGap(method1_code, method1_patches,
                                            kNopCode, ArrayRef<const LinkerPatch>(),
                                            bl_offset_in_method1 + max_positive_disp);
  ASSERT_FALSE(thunk_in_gap);  // There should be no thunk.

  // Check linked code.
  auto expected_code = GenNopsAndBl(3u, kBlPlusMax);
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(Thumb2RelativePatcherTest, CallOtherAlmostTooFarBefore) {
  auto method3_raw_code = GenNopsAndBl(2u, kBlPlus0);
  constexpr uint32_t bl_offset_in_method3 = 2u * 2u;  // After NOPs.
  ArrayRef<const uint8_t> method3_code(method3_raw_code);
  ASSERT_EQ(bl_offset_in_method3 + 4u, method3_code.size());
  LinkerPatch method3_patches[] = {
      LinkerPatch::RelativeCodePatch(bl_offset_in_method3, nullptr, 1u),
  };

  constexpr uint32_t just_over_max_negative_disp = 16 * MB - 4u /* PC adjustment */;
  bool thunk_in_gap = Create2MethodsWithGap(kNopCode, ArrayRef<const LinkerPatch>(),
                                            method3_code, method3_patches,
                                            just_over_max_negative_disp - bl_offset_in_method3);
  ASSERT_FALSE(thunk_in_gap);  // There should be no thunk.

  // Check linked code.
  auto expected_code = GenNopsAndBl(2u, kBlMinusMax);
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(3u), ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(Thumb2RelativePatcherTest, CallOtherJustTooFarAfter) {
  auto method1_raw_code = GenNopsAndBl(2u, kBlPlus0);
  constexpr uint32_t bl_offset_in_method1 = 2u * 2u;  // After NOPs.
  ArrayRef<const uint8_t> method1_code(method1_raw_code);
  ASSERT_EQ(bl_offset_in_method1 + 4u, method1_code.size());
  LinkerPatch method1_patches[] = {
      LinkerPatch::RelativeCodePatch(bl_offset_in_method1, nullptr, 3u),
  };

  constexpr uint32_t just_over_max_positive_disp = 16 * MB + 4u /* PC adjustment */;
  bool thunk_in_gap = Create2MethodsWithGap(method1_code, method1_patches,
                                            kNopCode, ArrayRef<const LinkerPatch>(),
                                            bl_offset_in_method1 + just_over_max_positive_disp);
  ASSERT_TRUE(thunk_in_gap);

  uint32_t method1_offset = GetMethodOffset(1u);
  uint32_t method3_offset = GetMethodOffset(3u);
  uint32_t method3_header_offset = method3_offset - sizeof(OatQuickMethodHeader);
  ASSERT_TRUE(IsAligned<kArmAlignment>(method3_header_offset));
  uint32_t thunk_offset = method3_header_offset - CompiledCode::AlignCode(ThunkSize(), kThumb2);
  ASSERT_TRUE(IsAligned<kArmAlignment>(thunk_offset));
  uint32_t diff = thunk_offset - (method1_offset + bl_offset_in_method1 + 4u /* PC adjustment */);
  ASSERT_EQ(diff & 1u, 0u);
  ASSERT_GE(diff, 16 * MB - (1u << 9));  // Simple encoding, unknown bits fit into the low 8 bits.
  auto expected_code = GenNopsAndBl(2u, 0xf3ffd700 | ((diff >> 1) & 0xffu));
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
  CheckThunk(thunk_offset);
}

TEST_F(Thumb2RelativePatcherTest, CallOtherJustTooFarBefore) {
  auto method3_raw_code = GenNopsAndBl(3u, kBlPlus0);
  constexpr uint32_t bl_offset_in_method3 = 3u * 2u;  // After NOPs.
  ArrayRef<const uint8_t> method3_code(method3_raw_code);
  ASSERT_EQ(bl_offset_in_method3 + 4u, method3_code.size());
  LinkerPatch method3_patches[] = {
      LinkerPatch::RelativeCodePatch(bl_offset_in_method3, nullptr, 1u),
  };

  constexpr uint32_t just_over_max_negative_disp = 16 * MB + 2 - 4u /* PC adjustment */;
  bool thunk_in_gap = Create2MethodsWithGap(kNopCode, ArrayRef<const LinkerPatch>(),
                                            method3_code, method3_patches,
                                            just_over_max_negative_disp - bl_offset_in_method3);
  ASSERT_FALSE(thunk_in_gap);  // There should be a thunk but it should be after the method2.

  // Check linked code.
  uint32_t method3_offset = GetMethodOffset(3u);
  uint32_t thunk_offset = CompiledCode::AlignCode(method3_offset + method3_code.size(), kThumb2);
  uint32_t diff = thunk_offset - (method3_offset + bl_offset_in_method3 + 4u /* PC adjustment */);
  ASSERT_EQ(diff & 1u, 0u);
  ASSERT_LT(diff >> 1, 1u << 8);  // Simple encoding, (diff >> 1) fits into 8 bits.
  auto expected_code = GenNopsAndBl(3u, kBlPlus0 | ((diff >> 1) & 0xffu));
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(3u), ArrayRef<const uint8_t>(expected_code)));
  EXPECT_TRUE(CheckThunk(thunk_offset));
}

TEST_F(Thumb2RelativePatcherTest, DexCacheReferenceImm8) {
  TestDexCachereference(0x00ff0000u, 0x00fcu);
  ASSERT_LT(GetMethodOffset(1u), 0xfcu);
}

TEST_F(Thumb2RelativePatcherTest, DexCacheReferenceImm3) {
  TestDexCachereference(0x02ff0000u, 0x05fcu);
  ASSERT_LT(GetMethodOffset(1u), 0xfcu);
}

TEST_F(Thumb2RelativePatcherTest, DexCacheReferenceImm) {
  TestDexCachereference(0x08ff0000u, 0x08fcu);
  ASSERT_LT(GetMethodOffset(1u), 0xfcu);
}

TEST_F(Thumb2RelativePatcherTest, DexCacheReferenceimm4) {
  TestDexCachereference(0xd0ff0000u, 0x60fcu);
  ASSERT_LT(GetMethodOffset(1u), 0xfcu);
}

}  // namespace linker
}  // namespace art
