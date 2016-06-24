/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "base/logging.h"
#include "base/macros.h"
#include "base/stringprintf.h"
#include "dex2oat_environment_test.h"
#include "utils.h"

#include <sys/wait.h>
#include <unistd.h>

namespace art {

class Dex2oatTest : public Dex2oatEnvironmentTest {
 public:
  virtual void TearDown() OVERRIDE {
    Dex2oatEnvironmentTest::TearDown();

    output_ = "";
    error_msg_ = "";
    success_ = false;
  }

 protected:
  void GenerateOdexForTest(const std::string& dex_location,
                           const std::string& odex_location,
                           CompilerFilter::Filter filter,
                           const std::vector<std::string>& extra_args = {},
                           bool expect_success = true) {
    std::vector<std::string> args;
    args.push_back("--dex-file=" + dex_location);
    args.push_back("--oat-file=" + odex_location);
    args.push_back("--compiler-filter=" + CompilerFilter::NameOfFilter(filter));
    args.push_back("--runtime-arg");
    args.push_back("-Xnorelocate");

    args.insert(args.end(), extra_args.begin(), extra_args.end());

    std::string error_msg;
    bool success = Dex2Oat(args, &error_msg);

    if (expect_success) {
      ASSERT_TRUE(success) << error_msg;

      // Verify the odex file was generated as expected.
      std::unique_ptr<OatFile> odex_file(OatFile::Open(odex_location.c_str(),
                                                       odex_location.c_str(),
                                                       nullptr,
                                                       nullptr,
                                                       false,
                                                       /*low_4gb*/false,
                                                       dex_location.c_str(),
                                                       &error_msg));
      ASSERT_TRUE(odex_file.get() != nullptr) << error_msg;

      CheckFilter(filter, odex_file->GetCompilerFilter());
    } else {
      ASSERT_FALSE(success) << output_;

      error_msg_ = error_msg;

      // Verify there's no loadable odex file.
      std::unique_ptr<OatFile> odex_file(OatFile::Open(odex_location.c_str(),
                                                       odex_location.c_str(),
                                                       nullptr,
                                                       nullptr,
                                                       false,
                                                       /*low_4gb*/false,
                                                       dex_location.c_str(),
                                                       &error_msg));
      ASSERT_TRUE(odex_file.get() == nullptr);
    }
  }

  // Check the input compiler filter against the generated oat file's filter. Mayb be overridden
  // in subclasses when equality is not expected.
  virtual void CheckFilter(CompilerFilter::Filter expected, CompilerFilter::Filter actual) {
    EXPECT_EQ(expected, actual);
  }

  bool Dex2Oat(const std::vector<std::string>& dex2oat_args, std::string* error_msg) {
    Runtime* runtime = Runtime::Current();

    const std::vector<gc::space::ImageSpace*>& image_spaces =
        runtime->GetHeap()->GetBootImageSpaces();
    if (image_spaces.empty()) {
      *error_msg = "No image location found for Dex2Oat.";
      return false;
    }
    std::string image_location = image_spaces[0]->GetImageLocation();

    std::vector<std::string> argv;
    argv.push_back(runtime->GetCompilerExecutable());
    argv.push_back("--runtime-arg");
    argv.push_back("-classpath");
    argv.push_back("--runtime-arg");
    std::string class_path = runtime->GetClassPathString();
    if (class_path == "") {
      class_path = OatFile::kSpecialSharedLibrary;
    }
    argv.push_back(class_path);
    if (runtime->IsDebuggable()) {
      argv.push_back("--debuggable");
    }
    runtime->AddCurrentRuntimeFeaturesAsDex2OatArguments(&argv);

    if (!runtime->IsVerificationEnabled()) {
      argv.push_back("--compiler-filter=verify-none");
    }

    if (runtime->MustRelocateIfPossible()) {
      argv.push_back("--runtime-arg");
      argv.push_back("-Xrelocate");
    } else {
      argv.push_back("--runtime-arg");
      argv.push_back("-Xnorelocate");
    }

    if (!kIsTargetBuild) {
      argv.push_back("--host");
    }

    argv.push_back("--boot-image=" + image_location);

    std::vector<std::string> compiler_options = runtime->GetCompilerOptions();
    argv.insert(argv.end(), compiler_options.begin(), compiler_options.end());

    argv.insert(argv.end(), dex2oat_args.begin(), dex2oat_args.end());

    // We must set --android-root.
    const char* android_root = getenv("ANDROID_ROOT");
    CHECK(android_root != nullptr);
    argv.push_back("--android-root=" + std::string(android_root));

    std::string command_line(Join(argv, ' '));

    // We need to fix up the '&' being used for "do not check classpath."
    size_t ampersand = command_line.find(" &");
    CHECK_NE(ampersand, std::string::npos);
    command_line = command_line.replace(ampersand, 2, " \\&");

    command_line += " 2>&1";

    // We need dex2oat to actually log things.
    setenv("ANDROID_LOG_TAGS", "*:d", 1);

    FILE* pipe = popen(command_line.c_str(), "r");

    setenv("ANDROID_LOG_TAGS", "*:e", 1);

    if (pipe == nullptr) {
      success_ = false;
    } else {
      char buffer[128];

      while (fgets(buffer, 128, pipe) != nullptr) {
        output_ += buffer;
      }

      int result = pclose(pipe);
      success_ = result == 0;
    }
    return success_;
  }

  std::string output_ = "";
  std::string error_msg_ = "";
  bool success_ = false;
};

class Dex2oatSwapTest : public Dex2oatTest {
 protected:
  void RunTest(bool use_fd, bool expect_use, const std::vector<std::string>& extra_args = {}) {
    std::string dex_location = GetScratchDir() + "/Dex2OatSwapTest.jar";
    std::string odex_location = GetOdexDir() + "/Dex2OatSwapTest.odex";

    Copy(GetDexSrc1(), dex_location);

    std::vector<std::string> copy(extra_args);

    std::unique_ptr<ScratchFile> sf;
    if (use_fd) {
      sf.reset(new ScratchFile());
      copy.push_back(StringPrintf("--swap-fd=%d", sf->GetFd()));
    } else {
      std::string swap_location = GetOdexDir() + "/Dex2OatSwapTest.odex.swap";
      copy.push_back("--swap-file=" + swap_location);
    }
    GenerateOdexForTest(dex_location, odex_location, CompilerFilter::kSpeed, copy);

    CheckValidity();
    ASSERT_TRUE(success_);
    CheckResult(expect_use);
  }

  void CheckResult(bool expect_use) {
    if (kIsTargetBuild) {
      CheckTargetResult(expect_use);
    } else {
      CheckHostResult(expect_use);
    }
  }

  void CheckTargetResult(bool expect_use ATTRIBUTE_UNUSED) {
    // TODO: Ignore for now, as we won't capture any output (it goes to the logcat). We may do
    //       something for variants with file descriptor where we can control the lifetime of
    //       the swap file and thus take a look at it.
  }

  void CheckHostResult(bool expect_use) {
    if (!kIsTargetBuild) {
      if (expect_use) {
        EXPECT_NE(output_.find("Large app, accepted running with swap."), std::string::npos)
            << output_;
      } else {
        EXPECT_EQ(output_.find("Large app, accepted running with swap."), std::string::npos)
            << output_;
      }
    }
  }

  // Check whether the dex2oat run was really successful.
  void CheckValidity() {
    if (kIsTargetBuild) {
      CheckTargetValidity();
    } else {
      CheckHostValidity();
    }
  }

  void CheckTargetValidity() {
    // TODO: Ignore for now, as we won't capture any output (it goes to the logcat). We may do
    //       something for variants with file descriptor where we can control the lifetime of
    //       the swap file and thus take a look at it.
  }

  // On the host, we can get the dex2oat output. Here, look for "dex2oat took."
  void CheckHostValidity() {
    EXPECT_NE(output_.find("dex2oat took"), std::string::npos) << output_;
  }
};

TEST_F(Dex2oatSwapTest, DoNotUseSwapDefaultSingleSmall) {
  RunTest(false /* use_fd */, false /* expect_use */);
  RunTest(true /* use_fd */, false /* expect_use */);
}

TEST_F(Dex2oatSwapTest, DoNotUseSwapSingle) {
  RunTest(false /* use_fd */, false /* expect_use */, { "--swap-dex-size-threshold=0" });
  RunTest(true /* use_fd */, false /* expect_use */, { "--swap-dex-size-threshold=0" });
}

TEST_F(Dex2oatSwapTest, DoNotUseSwapSmall) {
  RunTest(false /* use_fd */, false /* expect_use */, { "--swap-dex-count-threshold=0" });
  RunTest(true /* use_fd */, false /* expect_use */, { "--swap-dex-count-threshold=0" });
}

TEST_F(Dex2oatSwapTest, DoUseSwapSingleSmall) {
  RunTest(false /* use_fd */,
          true /* expect_use */,
          { "--swap-dex-size-threshold=0", "--swap-dex-count-threshold=0" });
  RunTest(true /* use_fd */,
          true /* expect_use */,
          { "--swap-dex-size-threshold=0", "--swap-dex-count-threshold=0" });
}

}  // namespace art
