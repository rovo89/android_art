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

#include <string>
#include <vector>
#include <sstream>

#include "common_runtime_test.h"

#include "base/stringprintf.h"
#include "runtime/arch/instruction_set.h"
#include "runtime/gc/heap.h"
#include "runtime/gc/space/image_space.h"
#include "runtime/os.h"
#include "runtime/utils.h"
#include "utils.h"

#include <sys/types.h>
#include <unistd.h>

namespace art {

class OatDumpTest : public CommonRuntimeTest {
 protected:
  virtual void SetUp() {
    CommonRuntimeTest::SetUp();
    core_art_location_ = GetCoreArtLocation();
    core_oat_location_ = GetSystemImageFilename(GetCoreOatLocation().c_str(), kRuntimeISA);
  }

  // Returns path to the oatdump binary.
  std::string GetOatDumpFilePath() {
    std::string root = GetTestAndroidRoot();
    root += "/bin/oatdump";
    if (kIsDebugBuild) {
      root += "d";
    }
    return root;
  }

  enum Mode {
    kModeOat,
    kModeArt,
    kModeSymbolize,
  };

  // Run the test with custom arguments.
  bool Exec(Mode mode, const std::vector<std::string>& args, std::string* error_msg) {
    std::string file_path = GetOatDumpFilePath();

    EXPECT_TRUE(OS::FileExists(file_path.c_str())) << file_path << " should be a valid file path";

    std::vector<std::string> exec_argv = { file_path };
    if (mode == kModeSymbolize) {
      exec_argv.push_back("--symbolize=" + core_oat_location_);
      exec_argv.push_back("--output=" + core_oat_location_ + ".symbolize");
    } else if (mode == kModeArt) {
      exec_argv.push_back("--image=" + core_art_location_);
      exec_argv.push_back("--output=/dev/null");
    } else {
      CHECK_EQ(static_cast<size_t>(mode), static_cast<size_t>(kModeOat));
      exec_argv.push_back("--oat-file=" + core_oat_location_);
      exec_argv.push_back("--output=/dev/null");
    }
    exec_argv.insert(exec_argv.end(), args.begin(), args.end());
    return ::art::Exec(exec_argv, error_msg);
  }

 private:
  std::string core_art_location_;
  std::string core_oat_location_;
};

TEST_F(OatDumpTest, TestImage) {
  std::string error_msg;
  ASSERT_TRUE(Exec(kModeArt, {}, &error_msg)) << error_msg;
}

TEST_F(OatDumpTest, TestOatImage) {
  std::string error_msg;
  ASSERT_TRUE(Exec(kModeOat, {}, &error_msg)) << error_msg;
}

TEST_F(OatDumpTest, TestDumpRawMappingTable) {
  std::string error_msg;
  ASSERT_TRUE(Exec(kModeArt, {"--dump:raw_mapping_table"}, &error_msg)) << error_msg;
}

TEST_F(OatDumpTest, TestDumpRawGcMap) {
  std::string error_msg;
  ASSERT_TRUE(Exec(kModeArt, {"--dump:raw_gc_map"}, &error_msg)) << error_msg;
}

TEST_F(OatDumpTest, TestNoDumpVmap) {
  std::string error_msg;
  ASSERT_TRUE(Exec(kModeArt, {"--no-dump:vmap"}, &error_msg)) << error_msg;
}

TEST_F(OatDumpTest, TestNoDisassemble) {
  std::string error_msg;
  ASSERT_TRUE(Exec(kModeArt, {"--no-disassemble"}, &error_msg)) << error_msg;
}

TEST_F(OatDumpTest, TestListClasses) {
  std::string error_msg;
  ASSERT_TRUE(Exec(kModeArt, {"--list-classes"}, &error_msg)) << error_msg;
}

TEST_F(OatDumpTest, TestListMethods) {
  std::string error_msg;
  ASSERT_TRUE(Exec(kModeArt, {"--list-methods"}, &error_msg)) << error_msg;
}

TEST_F(OatDumpTest, TestSymbolize) {
  std::string error_msg;
  ASSERT_TRUE(Exec(kModeSymbolize, {}, &error_msg)) << error_msg;
}

}  // namespace art
