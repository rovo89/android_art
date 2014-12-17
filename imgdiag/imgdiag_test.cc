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

#include <string>
#include <vector>
#include <sstream>

#include "common_runtime_test.h"

#include "runtime/os.h"
#include "runtime/arch/instruction_set.h"
#include "runtime/utils.h"
#include "runtime/gc/space/image_space.h"
#include "runtime/gc/heap.h"
#include "base/stringprintf.h"

#include <sys/types.h>
#include <unistd.h>

namespace art {

static const char* kImgDiagDiffPid = "--image-diff-pid";
static const char* kImgDiagBootImage = "--boot-image";
static const char* kImgDiagBinaryName = "imgdiag";

class ImgDiagTest : public CommonRuntimeTest {
 protected:
  virtual void SetUp() {
    CommonRuntimeTest::SetUp();

    // We loaded the runtime with an explicit image. Therefore the image space must exist.
    gc::space::ImageSpace* image_space = Runtime::Current()->GetHeap()->GetImageSpace();
    ASSERT_TRUE(image_space != nullptr);
    boot_image_location_ = image_space->GetImageLocation();
  }

  virtual void SetUpRuntimeOptions(RuntimeOptions* options) OVERRIDE {
    // Needs to live until CommonRuntimeTest::SetUp finishes, since we pass it a cstring.
    runtime_args_image_ = StringPrintf("-Ximage:%s", GetCoreArtLocation().c_str());
    options->push_back(std::make_pair(runtime_args_image_, nullptr));
  }

  // Path to the imgdiag(d?)[32|64] binary.
  std::string GetImgDiagFilePath() {
    std::string root = GetTestAndroidRoot();

    root += "/bin/";
    root += kImgDiagBinaryName;

    if (kIsDebugBuild) {
      root += "d";
    }

    std::string root32 = root + "32";
    // If we have both a 32-bit and a 64-bit build, the 32-bit file will have a 32 suffix.
    if (OS::FileExists(root32.c_str()) && !Is64BitInstructionSet(kRuntimeISA)) {
      return root32;
    // Only a single build exists, so the filename never has an extra suffix.
    } else {
      return root;
    }
  }

  // Run imgdiag with a custom boot image location.
  bool Exec(pid_t image_diff_pid, const std::string& boot_image, std::string* error_msg) {
    // Invoke 'img_diag' against the current process.
    // This should succeed because we have a runtime and so it should
    // be able to map in the boot.art and do a diff for it.
    std::string file_path = GetImgDiagFilePath();
    EXPECT_TRUE(OS::FileExists(file_path.c_str())) << file_path << " should be a valid file path";

    // Run imgdiag --image-diff-pid=$image_diff_pid and wait until it's done with a 0 exit code.
    std::string diff_pid_args;
    {
      std::stringstream diff_pid_args_ss;
      diff_pid_args_ss << kImgDiagDiffPid << "=" << image_diff_pid;
      diff_pid_args = diff_pid_args_ss.str();
    }
    std::string boot_image_args;
    {
      boot_image_args = boot_image_args + kImgDiagBootImage + "=" + boot_image;
    }

    std::vector<std::string> exec_argv = { file_path, diff_pid_args, boot_image_args };

    return ::art::Exec(exec_argv, error_msg);
  }

  // Run imgdiag with the default boot image location.
  bool ExecDefaultBootImage(pid_t image_diff_pid, std::string* error_msg) {
    return Exec(image_diff_pid, boot_image_location_, error_msg);
  }

 private:
  std::string runtime_args_image_;
  std::string boot_image_location_;
};

#if defined (ART_TARGET)
TEST_F(ImgDiagTest, ImageDiffPidSelf) {
#else
// Can't run this test on the host, it will fail when trying to open /proc/kpagestats
// because it's root read-only.
TEST_F(ImgDiagTest, DISABLED_ImageDiffPidSelf) {
#endif
  // Invoke 'img_diag' against the current process.
  // This should succeed because we have a runtime and so it should
  // be able to map in the boot.art and do a diff for it.

  // Run imgdiag --image-diff-pid=$(self pid) and wait until it's done with a 0 exit code.
  std::string error_msg;
  ASSERT_TRUE(ExecDefaultBootImage(getpid(), &error_msg)) << "Failed to execute -- because: "
                                                          << error_msg;
}

TEST_F(ImgDiagTest, ImageDiffBadPid) {
  // Invoke 'img_diag' against a non-existing process. This should fail.

  // Run imgdiag --image-diff-pid=some_bad_pid and wait until it's done with a 0 exit code.
  std::string error_msg;
  ASSERT_FALSE(ExecDefaultBootImage(-12345, &error_msg)) << "Incorrectly executed";
  UNUSED(error_msg);
}

}  // namespace art
