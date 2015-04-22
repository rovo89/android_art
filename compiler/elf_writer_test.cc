/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "elf_file.h"

#include "base/stringprintf.h"
#include "base/unix_file/fd_file.h"
#include "common_compiler_test.h"
#include "elf_file.h"
#include "elf_file_impl.h"
#include "elf_writer_quick.h"
#include "oat.h"
#include "utils.h"

namespace art {

class ElfWriterTest : public CommonCompilerTest {
 protected:
  virtual void SetUp() {
    ReserveImageSpace();
    CommonCompilerTest::SetUp();
  }
};

#define EXPECT_ELF_FILE_ADDRESS(ef, expected_value, symbol_name, build_map) \
  do { \
    void* addr = reinterpret_cast<void*>(ef->FindSymbolAddress(SHT_DYNSYM, \
                                                               symbol_name, \
                                                               build_map)); \
    EXPECT_NE(nullptr, addr); \
    EXPECT_LT(static_cast<uintptr_t>(ART_BASE_ADDRESS), reinterpret_cast<uintptr_t>(addr)); \
    if (expected_value == nullptr) { \
      expected_value = addr; \
    }                        \
    EXPECT_EQ(expected_value, addr); \
    EXPECT_EQ(expected_value, ef->FindDynamicSymbolAddress(symbol_name)); \
  } while (false)

TEST_F(ElfWriterTest, dlsym) {
  std::string elf_location = GetCoreOatLocation();
  std::string elf_filename = GetSystemImageFilename(elf_location.c_str(), kRuntimeISA);
  LOG(INFO) << "elf_filename=" << elf_filename;

  UnreserveImageSpace();
  void* dl_oatdata = nullptr;
  void* dl_oatexec = nullptr;
  void* dl_oatlastword = nullptr;

  std::unique_ptr<File> file(OS::OpenFileForReading(elf_filename.c_str()));
  ASSERT_TRUE(file.get() != nullptr);
  {
    std::string error_msg;
    std::unique_ptr<ElfFile> ef(ElfFile::Open(file.get(), false, false, &error_msg));
    CHECK(ef.get() != nullptr) << error_msg;
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatdata, "oatdata", false);
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatexec, "oatexec", false);
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatlastword, "oatlastword", false);
  }
  {
    std::string error_msg;
    std::unique_ptr<ElfFile> ef(ElfFile::Open(file.get(), false, false, &error_msg));
    CHECK(ef.get() != nullptr) << error_msg;
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatdata, "oatdata", true);
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatexec, "oatexec", true);
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatlastword, "oatlastword", true);
  }
  {
    std::string error_msg;
    std::unique_ptr<ElfFile> ef(ElfFile::Open(file.get(), false, true, &error_msg));
    CHECK(ef.get() != nullptr) << error_msg;
    CHECK(ef->Load(false, &error_msg)) << error_msg;
    EXPECT_EQ(dl_oatdata, ef->FindDynamicSymbolAddress("oatdata"));
    EXPECT_EQ(dl_oatexec, ef->FindDynamicSymbolAddress("oatexec"));
    EXPECT_EQ(dl_oatlastword, ef->FindDynamicSymbolAddress("oatlastword"));
  }
}

// Run only on host since we do unaligned memory accesses.
#ifndef HAVE_ANDROID_OS

static void PatchSection(const std::vector<uintptr_t>& patch_locations,
                         std::vector<uint8_t>* section, int32_t delta) {
  for (uintptr_t location : patch_locations) {
    *reinterpret_cast<int32_t*>(section->data() + location) += delta;
  }
}

TEST_F(ElfWriterTest, EncodeDecodeOatPatches) {
  std::vector<uint8_t> oat_patches;  // Encoded patches.

  // Encode patch locations for a few sections.
  OatWriter::PatchLocationsMap sections;
  std::vector<uintptr_t> patches0 { 0, 4, 8, 15, 128, 200 };  // NOLINT
  sections.emplace(".section0", std::unique_ptr<std::vector<uintptr_t>>(
      new std::vector<uintptr_t> { patches0 }));
  std::vector<uintptr_t> patches1 { 8, 127 };  // NOLINT
  sections.emplace(".section1", std::unique_ptr<std::vector<uintptr_t>>(
      new std::vector<uintptr_t> { patches1 }));
  std::vector<uintptr_t> patches2 { };  // NOLINT
  sections.emplace(".section2", std::unique_ptr<std::vector<uintptr_t>>(
      new std::vector<uintptr_t> { patches2 }));
  ElfWriterQuick32::EncodeOatPatches(sections, &oat_patches);

  // Create buffers to be patched.
  std::vector<uint8_t> initial_data(256);
  for (size_t i = 0; i < initial_data.size(); i++) {
    initial_data[i] = i;
  }
  std::vector<uint8_t> section0_expected = initial_data;
  std::vector<uint8_t> section1_expected = initial_data;
  std::vector<uint8_t> section2_expected = initial_data;
  std::vector<uint8_t> section0_actual = initial_data;
  std::vector<uint8_t> section1_actual = initial_data;
  std::vector<uint8_t> section2_actual = initial_data;

  // Patch manually.
  constexpr int32_t delta = 0x11235813;
  PatchSection(patches0, &section0_expected, delta);
  PatchSection(patches1, &section1_expected, delta);
  PatchSection(patches2, &section2_expected, delta);

  // Decode and apply patch locations.
  bool section0_successful = ElfFileImpl32::ApplyOatPatches(
      oat_patches.data(), oat_patches.data() + oat_patches.size(),
      ".section0", delta,
      section0_actual.data(), section0_actual.data() + section0_actual.size());
  EXPECT_TRUE(section0_successful);
  EXPECT_EQ(section0_expected, section0_actual);

  bool section1_successful = ElfFileImpl32::ApplyOatPatches(
      oat_patches.data(), oat_patches.data() + oat_patches.size(),
      ".section1", delta,
      section1_actual.data(), section1_actual.data() + section1_actual.size());
  EXPECT_TRUE(section1_successful);
  EXPECT_EQ(section1_expected, section1_actual);

  bool section2_successful = ElfFileImpl32::ApplyOatPatches(
      oat_patches.data(), oat_patches.data() + oat_patches.size(),
      ".section2", delta,
      section2_actual.data(), section2_actual.data() + section2_actual.size());
  EXPECT_TRUE(section2_successful);
  EXPECT_EQ(section2_expected, section2_actual);
}

#endif

}  // namespace art
