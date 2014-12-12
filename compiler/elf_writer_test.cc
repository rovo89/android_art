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

#if defined(ART_USE_OPTIMIZING_COMPILER)
TEST_F(ElfWriterTest, DISABLED_dlsym) {
#else
TEST_F(ElfWriterTest, dlsym) {
#endif
  std::string elf_location;
  if (IsHost()) {
    const char* host_dir = getenv("ANDROID_HOST_OUT");
    CHECK(host_dir != NULL);
    elf_location = StringPrintf("%s/framework/core.oat", host_dir);
  } else {
    elf_location = "/data/art-test/core.oat";
  }
  std::string elf_filename = GetSystemImageFilename(elf_location.c_str(), kRuntimeISA);
  LOG(INFO) << "elf_filename=" << elf_filename;

  UnreserveImageSpace();
  void* dl_oatdata = NULL;
  void* dl_oatexec = NULL;
  void* dl_oatlastword = NULL;

  std::unique_ptr<File> file(OS::OpenFileForReading(elf_filename.c_str()));
  ASSERT_TRUE(file.get() != NULL);
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

}  // namespace art
