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
#include "linker/arm64/relative_patcher_arm64.h"

namespace art {
namespace linker {

class Arm64RelativePatcherTest : public RelativePatcherTest {
 public:
  explicit Arm64RelativePatcherTest(const std::string& variant)
      : RelativePatcherTest(kArm64, variant) { }

 protected:
  static const uint8_t kCallRawCode[];
  static const ArrayRef<const uint8_t> kCallCode;
  static const uint8_t kNopRawCode[];
  static const ArrayRef<const uint8_t> kNopCode;

  // All branches can be created from kBlPlus0 or kBPlus0 by adding the low 26 bits.
  static constexpr uint32_t kBlPlus0 = 0x94000000u;
  static constexpr uint32_t kBPlus0 = 0x14000000u;

  // Special BL values.
  static constexpr uint32_t kBlPlusMax = 0x95ffffffu;
  static constexpr uint32_t kBlMinusMax = 0x96000000u;

  // LDUR x2, [sp, #4], i.e. unaligned load crossing 64-bit boundary (assuming aligned sp).
  static constexpr uint32_t kLdurInsn = 0xf840405fu;

  // LDR w12, <label> and LDR x12, <label>. Bits 5-23 contain label displacement in 4-byte units.
  static constexpr uint32_t kLdrWPcRelInsn = 0x1800000cu;
  static constexpr uint32_t kLdrXPcRelInsn = 0x5800000cu;

  // LDR w13, [SP, #<pimm>] and LDR x13, [SP, #<pimm>]. Bits 10-21 contain displacement from SP
  // in units of 4-bytes (for 32-bit load) or 8-bytes (for 64-bit load).
  static constexpr uint32_t kLdrWSpRelInsn = 0xb94003edu;
  static constexpr uint32_t kLdrXSpRelInsn = 0xf94003edu;

  uint32_t Create2MethodsWithGap(const ArrayRef<const uint8_t>& method1_code,
                                 const ArrayRef<const LinkerPatch>& method1_patches,
                                 const ArrayRef<const uint8_t>& last_method_code,
                                 const ArrayRef<const LinkerPatch>& last_method_patches,
                                 uint32_t distance_without_thunks) {
    CHECK_EQ(distance_without_thunks % kArm64Alignment, 0u);
    const uint32_t method1_offset =
        CompiledCode::AlignCode(kTrampolineSize, kArm64) + sizeof(OatQuickMethodHeader);
    AddCompiledMethod(MethodRef(1u), method1_code, method1_patches);
    const uint32_t gap_start =
        CompiledCode::AlignCode(method1_offset + method1_code.size(), kArm64);

    // We want to put the method3 at a very precise offset.
    const uint32_t last_method_offset = method1_offset + distance_without_thunks;
    const uint32_t gap_end = last_method_offset - sizeof(OatQuickMethodHeader);
    CHECK(IsAligned<kArm64Alignment>(gap_end));

    // Fill the gap with intermediate methods in chunks of 2MiB and the last in [2MiB, 4MiB).
    // (This allows deduplicating the small chunks to avoid using 256MiB of memory for +-128MiB
    // offsets by this test.)
    uint32_t method_idx = 2u;
    constexpr uint32_t kSmallChunkSize = 2 * MB;
    std::vector<uint8_t> gap_code;
    size_t gap_size = gap_end - gap_start;
    for (; gap_size >= 2u * kSmallChunkSize; gap_size -= kSmallChunkSize) {
      uint32_t chunk_code_size = kSmallChunkSize - sizeof(OatQuickMethodHeader);
      gap_code.resize(chunk_code_size, 0u);
      AddCompiledMethod(MethodRef(method_idx), ArrayRef<const uint8_t>(gap_code),
                        ArrayRef<const LinkerPatch>());
      method_idx += 1u;
    }
    uint32_t chunk_code_size = gap_size - sizeof(OatQuickMethodHeader);
    gap_code.resize(chunk_code_size, 0u);
    AddCompiledMethod(MethodRef(method_idx), ArrayRef<const uint8_t>(gap_code),
                      ArrayRef<const LinkerPatch>());
    method_idx += 1u;

    // Add the last method and link
    AddCompiledMethod(MethodRef(method_idx), last_method_code, last_method_patches);
    Link();

    // Check assumptions.
    CHECK_EQ(GetMethodOffset(1), method1_offset);
    auto last_result = method_offset_map_.FindMethodOffset(MethodRef(method_idx));
    CHECK(last_result.first);
    // There may be a thunk before method2.
    if (last_result.second != last_method_offset) {
      // Thunk present. Check that there's only one.
      uint32_t aligned_thunk_size = CompiledCode::AlignCode(ThunkSize(), kArm64);
      CHECK_EQ(last_result.second, last_method_offset + aligned_thunk_size);
    }
    return method_idx;
  }

  uint32_t GetMethodOffset(uint32_t method_idx) {
    auto result = method_offset_map_.FindMethodOffset(MethodRef(method_idx));
    CHECK(result.first);
    CHECK_EQ(result.second & 3u, 0u);
    return result.second;
  }

  uint32_t ThunkSize() {
    return static_cast<Arm64RelativePatcher*>(patcher_.get())->thunk_code_.size();
  }

  bool CheckThunk(uint32_t thunk_offset) {
    Arm64RelativePatcher* patcher = static_cast<Arm64RelativePatcher*>(patcher_.get());
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
    result.reserve(num_nops * 4u + 4u);
    for (size_t i = 0; i != num_nops; ++i) {
      result.insert(result.end(), kNopCode.begin(), kNopCode.end());
    }
    result.push_back(static_cast<uint8_t>(bl));
    result.push_back(static_cast<uint8_t>(bl >> 8));
    result.push_back(static_cast<uint8_t>(bl >> 16));
    result.push_back(static_cast<uint8_t>(bl >> 24));
    return result;
  }

  std::vector<uint8_t> GenNopsAndAdrpLdr(size_t num_nops,
                                         uint32_t method_offset, uint32_t target_offset) {
    std::vector<uint8_t> result;
    result.reserve(num_nops * 4u + 8u);
    for (size_t i = 0; i != num_nops; ++i) {
      result.insert(result.end(), kNopCode.begin(), kNopCode.end());
    }
    DCHECK_EQ(method_offset & 3u, 0u);
    DCHECK_EQ(target_offset & 3u, 0u);
    uint32_t adrp_offset = method_offset + num_nops * 4u;
    uint32_t disp = target_offset - (adrp_offset & ~0xfffu);
    DCHECK_EQ(disp & 3u, 0u);
    uint32_t ldr = 0xb9400001 |               // LDR w1, [x0, #(imm12 * 2)]
        ((disp & 0xfffu) << (10 - 2));        // imm12 = ((disp & 0xfffu) >> 2) is at bit 10.
    uint32_t adrp = 0x90000000 |              // ADRP x0, +SignExtend(immhi:immlo:Zeros(12), 64)
        ((disp & 0x3000u) << (29 - 12)) |     // immlo = ((disp & 0x3000u) >> 12) is at bit 29,
        ((disp & 0xffffc000) >> (14 - 5)) |   // immhi = (disp >> 14) is at bit 5,
        // We take the sign bit from the disp, limiting disp to +- 2GiB.
        ((disp & 0x80000000) >> (31 - 23));   // sign bit in immhi is at bit 23.
    result.push_back(static_cast<uint8_t>(adrp));
    result.push_back(static_cast<uint8_t>(adrp >> 8));
    result.push_back(static_cast<uint8_t>(adrp >> 16));
    result.push_back(static_cast<uint8_t>(adrp >> 24));
    result.push_back(static_cast<uint8_t>(ldr));
    result.push_back(static_cast<uint8_t>(ldr >> 8));
    result.push_back(static_cast<uint8_t>(ldr >> 16));
    result.push_back(static_cast<uint8_t>(ldr >> 24));
    return result;
  }

  void TestNopsAdrpLdr(size_t num_nops, uint32_t dex_cache_arrays_begin, uint32_t element_offset) {
    dex_cache_arrays_begin_ = dex_cache_arrays_begin;
    auto code = GenNopsAndAdrpLdr(num_nops, 0u, 0u);  // Unpatched.
    LinkerPatch patches[] = {
        LinkerPatch::DexCacheArrayPatch(num_nops * 4u     , nullptr, num_nops * 4u, element_offset),
        LinkerPatch::DexCacheArrayPatch(num_nops * 4u + 4u, nullptr, num_nops * 4u, element_offset),
    };
    AddCompiledMethod(MethodRef(1u), ArrayRef<const uint8_t>(code),
                      ArrayRef<const LinkerPatch>(patches));
    Link();

    uint32_t method1_offset = GetMethodOffset(1u);
    uint32_t target_offset = dex_cache_arrays_begin_ + element_offset;
    auto expected_code = GenNopsAndAdrpLdr(num_nops, method1_offset, target_offset);
    EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
  }

  void InsertInsn(std::vector<uint8_t>* code, size_t pos, uint32_t insn) {
    CHECK_LE(pos, code->size());
    const uint8_t insn_code[] = {
        static_cast<uint8_t>(insn), static_cast<uint8_t>(insn >> 8),
        static_cast<uint8_t>(insn >> 16), static_cast<uint8_t>(insn >> 24),
    };
    static_assert(sizeof(insn_code) == 4u, "Invalid sizeof(insn_code).");
    code->insert(code->begin() + pos, insn_code, insn_code + sizeof(insn_code));
  }

  void PrepareNopsAdrpInsn2Ldr(size_t num_nops, uint32_t insn2,
                               uint32_t dex_cache_arrays_begin, uint32_t element_offset) {
    dex_cache_arrays_begin_ = dex_cache_arrays_begin;
    auto code = GenNopsAndAdrpLdr(num_nops, 0u, 0u);  // Unpatched.
    InsertInsn(&code, num_nops * 4u + 4u, insn2);
    LinkerPatch patches[] = {
        LinkerPatch::DexCacheArrayPatch(num_nops * 4u     , nullptr, num_nops * 4u, element_offset),
        LinkerPatch::DexCacheArrayPatch(num_nops * 4u + 8u, nullptr, num_nops * 4u, element_offset),
    };
    AddCompiledMethod(MethodRef(1u), ArrayRef<const uint8_t>(code),
                      ArrayRef<const LinkerPatch>(patches));
    Link();
  }

  void TestNopsAdrpInsn2Ldr(size_t num_nops, uint32_t insn2,
                            uint32_t dex_cache_arrays_begin, uint32_t element_offset) {
    PrepareNopsAdrpInsn2Ldr(num_nops, insn2, dex_cache_arrays_begin, element_offset);

    uint32_t method1_offset = GetMethodOffset(1u);
    uint32_t target_offset = dex_cache_arrays_begin_ + element_offset;
    auto expected_code = GenNopsAndAdrpLdr(num_nops, method1_offset, target_offset);
    InsertInsn(&expected_code, num_nops * 4u + 4u, insn2);
    EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
  }

  void TestNopsAdrpInsn2LdrHasThunk(size_t num_nops, uint32_t insn2,
                                    uint32_t dex_cache_arrays_begin, uint32_t element_offset) {
    PrepareNopsAdrpInsn2Ldr(num_nops, insn2, dex_cache_arrays_begin, element_offset);

    uint32_t method1_offset = GetMethodOffset(1u);
    CHECK(!compiled_method_refs_.empty());
    CHECK_EQ(compiled_method_refs_[0].dex_method_index, 1u);
    CHECK_EQ(compiled_method_refs_.size(), compiled_methods_.size());
    uint32_t method1_size = compiled_methods_[0]->GetQuickCode()->size();
    uint32_t thunk_offset = CompiledCode::AlignCode(method1_offset + method1_size, kArm64);
    uint32_t b_diff = thunk_offset - (method1_offset + num_nops * 4u);
    ASSERT_EQ(b_diff & 3u, 0u);
    ASSERT_LT(b_diff, 128 * MB);
    uint32_t b_out = kBPlus0 + ((b_diff >> 2) & 0x03ffffffu);
    uint32_t b_in = kBPlus0 + ((-b_diff >> 2) & 0x03ffffffu);

    uint32_t target_offset = dex_cache_arrays_begin_ + element_offset;
    auto expected_code = GenNopsAndAdrpLdr(num_nops, method1_offset, target_offset);
    InsertInsn(&expected_code, num_nops * 4u + 4u, insn2);
    // Replace adrp with bl.
    expected_code.erase(expected_code.begin() + num_nops * 4u,
                        expected_code.begin() + num_nops * 4u + 4u);
    InsertInsn(&expected_code, num_nops * 4u, b_out);
    EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));

    auto expected_thunk_code = GenNopsAndAdrpLdr(0u, thunk_offset, target_offset);
    ASSERT_EQ(expected_thunk_code.size(), 8u);
    expected_thunk_code.erase(expected_thunk_code.begin() + 4u, expected_thunk_code.begin() + 8u);
    InsertInsn(&expected_thunk_code, 4u, b_in);
    ASSERT_EQ(expected_thunk_code.size(), 8u);

    uint32_t thunk_size = ThunkSize();
    ASSERT_EQ(thunk_offset + thunk_size, output_.size());
    ASSERT_EQ(thunk_size, expected_thunk_code.size());
    ArrayRef<const uint8_t> thunk_code(&output_[thunk_offset], thunk_size);
    if (ArrayRef<const uint8_t>(expected_thunk_code) != thunk_code) {
      DumpDiff(ArrayRef<const uint8_t>(expected_thunk_code), thunk_code);
      FAIL();
    }
  }

  void TestAdrpInsn2Ldr(uint32_t insn2, uint32_t adrp_offset, bool has_thunk,
                        uint32_t dex_cache_arrays_begin, uint32_t element_offset) {
    uint32_t method1_offset =
        CompiledCode::AlignCode(kTrampolineSize, kArm64) + sizeof(OatQuickMethodHeader);
    ASSERT_LT(method1_offset, adrp_offset);
    ASSERT_EQ(adrp_offset & 3u, 0u);
    uint32_t num_nops = (adrp_offset - method1_offset) / 4u;
    if (has_thunk) {
      TestNopsAdrpInsn2LdrHasThunk(num_nops, insn2, dex_cache_arrays_begin, element_offset);
    } else {
      TestNopsAdrpInsn2Ldr(num_nops, insn2, dex_cache_arrays_begin, element_offset);
    }
    ASSERT_EQ(method1_offset, GetMethodOffset(1u));  // If this fails, num_nops is wrong.
  }

  void TestAdrpLdurLdr(uint32_t adrp_offset, bool has_thunk,
                       uint32_t dex_cache_arrays_begin, uint32_t element_offset) {
    TestAdrpInsn2Ldr(kLdurInsn, adrp_offset, has_thunk, dex_cache_arrays_begin, element_offset);
  }

  void TestAdrpLdrPcRelLdr(uint32_t pcrel_ldr_insn, int32_t pcrel_disp,
                           uint32_t adrp_offset, bool has_thunk,
                           uint32_t dex_cache_arrays_begin, uint32_t element_offset) {
    ASSERT_LT(pcrel_disp, 0x100000);
    ASSERT_GE(pcrel_disp, -0x100000);
    ASSERT_EQ(pcrel_disp & 0x3, 0);
    uint32_t insn2 = pcrel_ldr_insn | (((static_cast<uint32_t>(pcrel_disp) >> 2) & 0x7ffffu) << 5);
    TestAdrpInsn2Ldr(insn2, adrp_offset, has_thunk, dex_cache_arrays_begin, element_offset);
  }

  void TestAdrpLdrSpRelLdr(uint32_t sprel_ldr_insn, uint32_t sprel_disp_in_load_units,
                           uint32_t adrp_offset, bool has_thunk,
                           uint32_t dex_cache_arrays_begin, uint32_t element_offset) {
    ASSERT_LT(sprel_disp_in_load_units, 0x1000u);
    uint32_t insn2 = sprel_ldr_insn | ((sprel_disp_in_load_units & 0xfffu) << 10);
    TestAdrpInsn2Ldr(insn2, adrp_offset, has_thunk, dex_cache_arrays_begin, element_offset);
  }
};

const uint8_t Arm64RelativePatcherTest::kCallRawCode[] = {
    0x00, 0x00, 0x00, 0x94
};

const ArrayRef<const uint8_t> Arm64RelativePatcherTest::kCallCode(kCallRawCode);

const uint8_t Arm64RelativePatcherTest::kNopRawCode[] = {
    0x1f, 0x20, 0x03, 0xd5
};

const ArrayRef<const uint8_t> Arm64RelativePatcherTest::kNopCode(kNopRawCode);

class Arm64RelativePatcherTestDefault : public Arm64RelativePatcherTest {
 public:
  Arm64RelativePatcherTestDefault() : Arm64RelativePatcherTest("default") { }
};

class Arm64RelativePatcherTestDenver64 : public Arm64RelativePatcherTest {
 public:
  Arm64RelativePatcherTestDenver64() : Arm64RelativePatcherTest("denver64") { }
};

TEST_F(Arm64RelativePatcherTestDefault, CallSelf) {
  LinkerPatch patches[] = {
      LinkerPatch::RelativeCodePatch(0u, nullptr, 1u),
  };
  AddCompiledMethod(MethodRef(1u), kCallCode, ArrayRef<const LinkerPatch>(patches));
  Link();

  static const uint8_t expected_code[] = {
      0x00, 0x00, 0x00, 0x94
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(Arm64RelativePatcherTestDefault, CallOther) {
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
  uint32_t diff_after = method2_offset - method1_offset;
  ASSERT_EQ(diff_after & 3u, 0u);
  ASSERT_LT(diff_after >> 2, 1u << 8);  // Simple encoding, (diff_after >> 2) fits into 8 bits.
  static const uint8_t method1_expected_code[] = {
      static_cast<uint8_t>(diff_after >> 2), 0x00, 0x00, 0x94
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(method1_expected_code)));
  uint32_t diff_before = method1_offset - method2_offset;
  ASSERT_EQ(diff_before & 3u, 0u);
  ASSERT_GE(diff_before, -1u << 27);
  auto method2_expected_code = GenNopsAndBl(0u, kBlPlus0 | ((diff_before >> 2) & 0x03ffffffu));
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(2u), ArrayRef<const uint8_t>(method2_expected_code)));
}

TEST_F(Arm64RelativePatcherTestDefault, CallTrampoline) {
  LinkerPatch patches[] = {
      LinkerPatch::RelativeCodePatch(0u, nullptr, 2u),
  };
  AddCompiledMethod(MethodRef(1u), kCallCode, ArrayRef<const LinkerPatch>(patches));
  Link();

  uint32_t method1_offset = GetMethodOffset(1u);
  uint32_t diff = kTrampolineOffset - method1_offset;
  ASSERT_EQ(diff & 1u, 0u);
  ASSERT_GE(diff, -1u << 9);  // Simple encoding, -256 <= (diff >> 1) < 0 (checked as unsigned).
  auto expected_code = GenNopsAndBl(0u, kBlPlus0 | ((diff >> 2) & 0x03ffffffu));
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(Arm64RelativePatcherTestDefault, CallOtherAlmostTooFarAfter) {
  auto method1_raw_code = GenNopsAndBl(1u, kBlPlus0);
  constexpr uint32_t bl_offset_in_method1 = 1u * 4u;  // After NOPs.
  ArrayRef<const uint8_t> method1_code(method1_raw_code);
  ASSERT_EQ(bl_offset_in_method1 + 4u, method1_code.size());
  uint32_t expected_last_method_idx = 65;  // Based on 2MiB chunks in Create2MethodsWithGap().
  LinkerPatch method1_patches[] = {
      LinkerPatch::RelativeCodePatch(bl_offset_in_method1, nullptr, expected_last_method_idx),
  };

  constexpr uint32_t max_positive_disp = 128 * MB - 4u;
  uint32_t last_method_idx = Create2MethodsWithGap(method1_code, method1_patches,
                                                   kNopCode, ArrayRef<const LinkerPatch>(),
                                                   bl_offset_in_method1 + max_positive_disp);
  ASSERT_EQ(expected_last_method_idx, last_method_idx);

  uint32_t method1_offset = GetMethodOffset(1u);
  uint32_t last_method_offset = GetMethodOffset(last_method_idx);
  ASSERT_EQ(method1_offset + bl_offset_in_method1 + max_positive_disp, last_method_offset);

  // Check linked code.
  auto expected_code = GenNopsAndBl(1u, kBlPlusMax);
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(Arm64RelativePatcherTestDefault, CallOtherAlmostTooFarBefore) {
  auto last_method_raw_code = GenNopsAndBl(0u, kBlPlus0);
  constexpr uint32_t bl_offset_in_last_method = 0u * 4u;  // After NOPs.
  ArrayRef<const uint8_t> last_method_code(last_method_raw_code);
  ASSERT_EQ(bl_offset_in_last_method + 4u, last_method_code.size());
  LinkerPatch last_method_patches[] = {
      LinkerPatch::RelativeCodePatch(bl_offset_in_last_method, nullptr, 1u),
  };

  constexpr uint32_t max_negative_disp = 128 * MB;
  uint32_t last_method_idx = Create2MethodsWithGap(kNopCode, ArrayRef<const LinkerPatch>(),
                                                   last_method_code, last_method_patches,
                                                   max_negative_disp - bl_offset_in_last_method);
  uint32_t method1_offset = GetMethodOffset(1u);
  uint32_t last_method_offset = GetMethodOffset(last_method_idx);
  ASSERT_EQ(method1_offset, last_method_offset + bl_offset_in_last_method - max_negative_disp);

  // Check linked code.
  auto expected_code = GenNopsAndBl(0u, kBlMinusMax);
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(last_method_idx),
                                ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(Arm64RelativePatcherTestDefault, CallOtherJustTooFarAfter) {
  auto method1_raw_code = GenNopsAndBl(0u, kBlPlus0);
  constexpr uint32_t bl_offset_in_method1 = 0u * 4u;  // After NOPs.
  ArrayRef<const uint8_t> method1_code(method1_raw_code);
  ASSERT_EQ(bl_offset_in_method1 + 4u, method1_code.size());
  uint32_t expected_last_method_idx = 65;  // Based on 2MiB chunks in Create2MethodsWithGap().
  LinkerPatch method1_patches[] = {
      LinkerPatch::RelativeCodePatch(bl_offset_in_method1, nullptr, expected_last_method_idx),
  };

  constexpr uint32_t just_over_max_positive_disp = 128 * MB;
  uint32_t last_method_idx = Create2MethodsWithGap(
      method1_code, method1_patches, kNopCode, ArrayRef<const LinkerPatch>(),
      bl_offset_in_method1 + just_over_max_positive_disp);
  ASSERT_EQ(expected_last_method_idx, last_method_idx);

  uint32_t method1_offset = GetMethodOffset(1u);
  uint32_t last_method_offset = GetMethodOffset(last_method_idx);
  uint32_t last_method_header_offset = last_method_offset - sizeof(OatQuickMethodHeader);
  ASSERT_TRUE(IsAligned<kArm64Alignment>(last_method_header_offset));
  uint32_t thunk_offset = last_method_header_offset - CompiledCode::AlignCode(ThunkSize(), kArm64);
  ASSERT_TRUE(IsAligned<kArm64Alignment>(thunk_offset));
  uint32_t diff = thunk_offset - (method1_offset + bl_offset_in_method1);
  ASSERT_EQ(diff & 3u, 0u);
  ASSERT_LT(diff, 128 * MB);
  auto expected_code = GenNopsAndBl(0u, kBlPlus0 | (diff >> 2));
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
  CheckThunk(thunk_offset);
}

TEST_F(Arm64RelativePatcherTestDefault, CallOtherJustTooFarBefore) {
  auto last_method_raw_code = GenNopsAndBl(1u, kBlPlus0);
  constexpr uint32_t bl_offset_in_last_method = 1u * 4u;  // After NOPs.
  ArrayRef<const uint8_t> last_method_code(last_method_raw_code);
  ASSERT_EQ(bl_offset_in_last_method + 4u, last_method_code.size());
  LinkerPatch last_method_patches[] = {
      LinkerPatch::RelativeCodePatch(bl_offset_in_last_method, nullptr, 1u),
  };

  constexpr uint32_t just_over_max_negative_disp = 128 * MB + 4;
  uint32_t last_method_idx = Create2MethodsWithGap(
      kNopCode, ArrayRef<const LinkerPatch>(), last_method_code, last_method_patches,
      just_over_max_negative_disp - bl_offset_in_last_method);
  uint32_t method1_offset = GetMethodOffset(1u);
  uint32_t last_method_offset = GetMethodOffset(last_method_idx);
  ASSERT_EQ(method1_offset,
            last_method_offset + bl_offset_in_last_method - just_over_max_negative_disp);

  // Check linked code.
  uint32_t thunk_offset =
      CompiledCode::AlignCode(last_method_offset + last_method_code.size(), kArm64);
  uint32_t diff = thunk_offset - (last_method_offset + bl_offset_in_last_method);
  ASSERT_EQ(diff & 3u, 0u);
  ASSERT_LT(diff, 128 * MB);
  auto expected_code = GenNopsAndBl(1u, kBlPlus0 | (diff >> 2));
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(last_method_idx),
                                ArrayRef<const uint8_t>(expected_code)));
  EXPECT_TRUE(CheckThunk(thunk_offset));
}

TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference1) {
  TestNopsAdrpLdr(0u, 0x12345678u, 0x1234u);
}

TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference2) {
  TestNopsAdrpLdr(0u, -0x12345678u, 0x4444u);
}

TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference3) {
  TestNopsAdrpLdr(0u, 0x12345000u, 0x3ffcu);
}

TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference4) {
  TestNopsAdrpLdr(0u, 0x12345000u, 0x4000u);
}

TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference0xff4) {
  TestAdrpLdurLdr(0xff4u, false, 0x12345678u, 0x1234u);
}

TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference0xff8) {
  TestAdrpLdurLdr(0xff8u, true, 0x12345678u, 0x1234u);
}

TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference0xffc) {
  TestAdrpLdurLdr(0xffcu, true, 0x12345678u, 0x1234u);
}

TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference0x1000) {
  TestAdrpLdurLdr(0x1000u, false, 0x12345678u, 0x1234u);
}

TEST_F(Arm64RelativePatcherTestDenver64, DexCacheReference0xff4) {
  TestAdrpLdurLdr(0xff4u, false, 0x12345678u, 0x1234u);
}

TEST_F(Arm64RelativePatcherTestDenver64, DexCacheReference0xff8) {
  TestAdrpLdurLdr(0xff8u, false, 0x12345678u, 0x1234u);
}

TEST_F(Arm64RelativePatcherTestDenver64, DexCacheReference0xffc) {
  TestAdrpLdurLdr(0xffcu, false, 0x12345678u, 0x1234u);
}

TEST_F(Arm64RelativePatcherTestDenver64, DexCacheReference0x1000) {
  TestAdrpLdurLdr(0x1000u, false, 0x12345678u, 0x1234u);
}

#define TEST_FOR_OFFSETS(test, disp1, disp2) \
  test(0xff4u, disp1) test(0xff8u, disp1) test(0xffcu, disp1) test(0x1000u, disp1) \
  test(0xff4u, disp2) test(0xff8u, disp2) test(0xffcu, disp2) test(0x1000u, disp2)

// LDR <Wt>, <label> is always aligned. We should never have to use a fixup.
#define LDRW_PCREL_TEST(adrp_offset, disp) \
  TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference ## adrp_offset ## WPcRel ## disp) { \
    TestAdrpLdrPcRelLdr(kLdrWPcRelInsn, disp, adrp_offset, false, 0x12345678u, 0x1234u); \
  }

TEST_FOR_OFFSETS(LDRW_PCREL_TEST, 0x1234, 0x1238)

// LDR <Xt>, <label> is aligned when offset + displacement is a multiple of 8.
#define LDRX_PCREL_TEST(adrp_offset, disp) \
  TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference ## adrp_offset ## XPcRel ## disp) { \
    bool unaligned = ((adrp_offset + 4u + static_cast<uint32_t>(disp)) & 7u) != 0; \
    bool has_thunk = (adrp_offset == 0xff8u || adrp_offset == 0xffcu) && unaligned; \
    TestAdrpLdrPcRelLdr(kLdrXPcRelInsn, disp, adrp_offset, has_thunk, 0x12345678u, 0x1234u); \
  }

TEST_FOR_OFFSETS(LDRX_PCREL_TEST, 0x1234, 0x1238)

// LDR <Wt>, [SP, #<pimm>] and LDR <Xt>, [SP, #<pimm>] are always aligned. No fixup needed.
#define LDRW_SPREL_TEST(adrp_offset, disp) \
  TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference ## adrp_offset ## WSpRel ## disp) { \
    TestAdrpLdrSpRelLdr(kLdrWSpRelInsn, disp >> 2, adrp_offset, false, 0x12345678u, 0x1234u); \
  }

TEST_FOR_OFFSETS(LDRW_SPREL_TEST, 0, 4)

#define LDRX_SPREL_TEST(adrp_offset, disp) \
  TEST_F(Arm64RelativePatcherTestDefault, DexCacheReference ## adrp_offset ## XSpRel ## disp) { \
    TestAdrpLdrSpRelLdr(kLdrXSpRelInsn, disp >> 3, adrp_offset, false, 0x12345678u, 0x1234u); \
  }

TEST_FOR_OFFSETS(LDRX_SPREL_TEST, 0, 8)

}  // namespace linker
}  // namespace art
