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

#if __linux__
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#endif

#include "jni.h"

#include <backtrace/Backtrace.h>

#include "base/logging.h"
#include "base/macros.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "oat_file.h"
#include "utils.h"

namespace art {

// For testing debuggerd. We do not have expected-death tests, so can't test this by default.
// Code for this is copied from SignalTest.
static constexpr bool kCauseSegfault = false;
char* go_away_compiler_cfi = nullptr;

static void CauseSegfault() {
#if defined(__arm__) || defined(__i386__) || defined(__x86_64__) || defined(__aarch64__)
  // On supported architectures we cause a real SEGV.
  *go_away_compiler_cfi = 'a';
#else
  // On other architectures we simulate SEGV.
  kill(getpid(), SIGSEGV);
#endif
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_sleep(JNIEnv*, jobject, jint, jboolean, jdouble) {
  // Keep pausing.
  printf("Going to sleep\n");
  for (;;) {
    pause();
  }
}

// Helper to look for a sequence in the stack trace.
#if __linux__
static bool CheckStack(Backtrace* bt, const std::vector<std::string>& seq) {
  size_t cur_search_index = 0;  // The currently active index in seq.
  CHECK_GT(seq.size(), 0U);

  for (Backtrace::const_iterator it = bt->begin(); it != bt->end(); ++it) {
    if (BacktraceMap::IsValid(it->map)) {
      LOG(INFO) << "Got " << it->func_name << ", looking for " << seq[cur_search_index];
      if (it->func_name == seq[cur_search_index]) {
        cur_search_index++;
        if (cur_search_index == seq.size()) {
          return true;
        }
      }
    }
  }

  printf("Cannot find %s in backtrace:\n", seq[cur_search_index].c_str());
  for (Backtrace::const_iterator it = bt->begin(); it != bt->end(); ++it) {
    if (BacktraceMap::IsValid(it->map)) {
      printf("  %s\n", it->func_name.c_str());
    }
  }

  return false;
}
#endif

// Currently we have to fall back to our own loader for the boot image when it's compiled PIC
// because its base is zero. Thus in-process unwinding through it won't work. This is a helper
// detecting this.
#if __linux__
static bool IsPicImage() {
  std::vector<gc::space::ImageSpace*> image_spaces =
      Runtime::Current()->GetHeap()->GetBootImageSpaces();
  CHECK(!image_spaces.empty());  // We should be running with an image.
  const OatFile* oat_file = image_spaces[0]->GetOatFile();
  CHECK(oat_file != nullptr);     // We should have an oat file to go with the image.
  return oat_file->IsPic();
}
#endif

extern "C" JNIEXPORT jboolean JNICALL Java_Main_unwindInProcess(
    JNIEnv*,
    jobject,
    jboolean full_signatrues,
    jint,
    jboolean) {
#if __linux__
  if (IsPicImage()) {
    LOG(INFO) << "Image is pic, in-process unwinding check bypassed.";
    return JNI_TRUE;
  }

  // TODO: What to do on Valgrind?

  std::unique_ptr<Backtrace> bt(Backtrace::Create(BACKTRACE_CURRENT_PROCESS, GetTid()));
  if (!bt->Unwind(0, nullptr)) {
    printf("Cannot unwind in process.\n");
    return JNI_FALSE;
  } else if (bt->NumFrames() == 0) {
    printf("No frames for unwind in process.\n");
    return JNI_FALSE;
  }

  // We cannot really parse an exact stack, as the optimizing compiler may inline some functions.
  // This is also risky, as deduping might play a trick on us, so the test needs to make sure that
  // only unique functions are being expected.
  // "mini-debug-info" does not include parameters to save space.
  std::vector<std::string> seq = {
      "Java_Main_unwindInProcess",                   // This function.
      "Main.unwindInProcess",                        // The corresponding Java native method frame.
      "int java.util.Arrays.binarySearch(java.lang.Object[], int, int, java.lang.Object, java.util.Comparator)",  // Framework method.
      "Main.main"                                    // The Java entry method.
  };
  std::vector<std::string> full_seq = {
      "Java_Main_unwindInProcess",                   // This function.
      "boolean Main.unwindInProcess(boolean, int, boolean)",  // The corresponding Java native method frame.
      "int java.util.Arrays.binarySearch(java.lang.Object[], int, int, java.lang.Object, java.util.Comparator)",  // Framework method.
      "void Main.main(java.lang.String[])"           // The Java entry method.
  };

  bool result = CheckStack(bt.get(), full_signatrues ? full_seq : seq);
  if (!kCauseSegfault) {
    return result ? JNI_TRUE : JNI_FALSE;
  } else {
    LOG(INFO) << "Result of check-stack: " << result;
  }
#endif

  if (kCauseSegfault) {
    CauseSegfault();
  }

  return JNI_FALSE;
}

#if __linux__
static constexpr int kSleepTimeMicroseconds = 50000;            // 0.05 seconds
static constexpr int kMaxTotalSleepTimeMicroseconds = 1000000;  // 1 second

// Wait for a sigstop. This code is copied from libbacktrace.
int wait_for_sigstop(pid_t tid, int* total_sleep_time_usec, bool* detach_failed ATTRIBUTE_UNUSED) {
  for (;;) {
    int status;
    pid_t n = TEMP_FAILURE_RETRY(waitpid(tid, &status, __WALL | WNOHANG));
    if (n == -1) {
      PLOG(WARNING) << "waitpid failed: tid " << tid;
      break;
    } else if (n == tid) {
      if (WIFSTOPPED(status)) {
        return WSTOPSIG(status);
      } else {
        PLOG(ERROR) << "unexpected waitpid response: n=" << n << ", status=" << std::hex << status;
        break;
      }
    }

    if (*total_sleep_time_usec > kMaxTotalSleepTimeMicroseconds) {
      PLOG(WARNING) << "timed out waiting for stop signal: tid=" << tid;
      break;
    }

    usleep(kSleepTimeMicroseconds);
    *total_sleep_time_usec += kSleepTimeMicroseconds;
  }

  return -1;
}
#endif

extern "C" JNIEXPORT jboolean JNICALL Java_Main_unwindOtherProcess(
    JNIEnv*,
    jobject,
    jboolean full_signatrues,
    jint pid_int) {
#if __linux__
  // TODO: What to do on Valgrind?
  pid_t pid = static_cast<pid_t>(pid_int);

  // OK, this is painful. debuggerd uses ptrace to unwind other processes.

  if (ptrace(PTRACE_ATTACH, pid, 0, 0)) {
    // Were not able to attach, bad.
    printf("Failed to attach to other process.\n");
    PLOG(ERROR) << "Failed to attach.";
    kill(pid, SIGKILL);
    return JNI_FALSE;
  }

  kill(pid, SIGSTOP);

  bool detach_failed = false;
  int total_sleep_time_usec = 0;
  int signal = wait_for_sigstop(pid, &total_sleep_time_usec, &detach_failed);
  if (signal == -1) {
    LOG(WARNING) << "wait_for_sigstop failed.";
  }

  std::unique_ptr<Backtrace> bt(Backtrace::Create(pid, BACKTRACE_CURRENT_THREAD));
  bool result = true;
  if (!bt->Unwind(0, nullptr)) {
    printf("Cannot unwind other process.\n");
    result = false;
  } else if (bt->NumFrames() == 0) {
    printf("No frames for unwind of other process.\n");
    result = false;
  }

  if (result) {
    // See comment in unwindInProcess for non-exact stack matching.
    // "mini-debug-info" does not include parameters to save space.
    std::vector<std::string> seq = {
        // "Java_Main_sleep",                        // The sleep function being executed in the
                                                     // other runtime.
                                                     // Note: For some reason, the name isn't
                                                     // resolved, so don't look for it right now.
        "Main.sleep",                                // The corresponding Java native method frame.
        "int java.util.Arrays.binarySearch(java.lang.Object[], int, int, java.lang.Object, java.util.Comparator)",  // Framework method.
        "Main.main"                                  // The Java entry method.
    };
    std::vector<std::string> full_seq = {
        // "Java_Main_sleep",                        // The sleep function being executed in the
                                                     // other runtime.
                                                     // Note: For some reason, the name isn't
                                                     // resolved, so don't look for it right now.
        "boolean Main.sleep(int, boolean, double)",  // The corresponding Java native method frame.
        "int java.util.Arrays.binarySearch(java.lang.Object[], int, int, java.lang.Object, java.util.Comparator)",  // Framework method.
        "void Main.main(java.lang.String[])"         // The Java entry method.
    };

    result = CheckStack(bt.get(), full_signatrues ? full_seq : seq);
  }

  if (ptrace(PTRACE_DETACH, pid, 0, 0) != 0) {
    PLOG(ERROR) << "Detach failed";
  }

  // Kill the other process once we are done with it.
  kill(pid, SIGKILL);

  return result ? JNI_TRUE : JNI_FALSE;
#else
  UNUSED(pid_int);
  return JNI_FALSE;
#endif
}

}  // namespace art
