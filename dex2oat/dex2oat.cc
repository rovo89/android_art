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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <valgrind.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(__linux__) && defined(__arm__)
#include <sys/personality.h>
#include <sys/utsname.h>
#endif

#define ATRACE_TAG ATRACE_TAG_DALVIK
#include <cutils/trace.h>

#include "art_method-inl.h"
#include "arch/instruction_set_features.h"
#include "arch/mips/instruction_set_features_mips.h"
#include "base/dumpable.h"
#include "base/macros.h"
#include "base/stl_util.h"
#include "base/stringpiece.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "compiler.h"
#include "compiler_callbacks.h"
#include "dex_file-inl.h"
#include "dex/pass_manager.h"
#include "dex/verification_results.h"
#include "dex/quick_compiler_callbacks.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "elf_file.h"
#include "elf_writer.h"
#include "gc/space/image_space.h"
#include "gc/space/space-inl.h"
#include "image_writer.h"
#include "interpreter/unstarted_runtime.h"
#include "leb128.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "oat_writer.h"
#include "os.h"
#include "runtime.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "utils.h"
#include "vector_output_stream.h"
#include "well_known_classes.h"
#include "zip_archive.h"

namespace art {

static int original_argc;
static char** original_argv;

static std::string CommandLine() {
  std::vector<std::string> command;
  for (int i = 0; i < original_argc; ++i) {
    command.push_back(original_argv[i]);
  }
  return Join(command, ' ');
}

// A stripped version. Remove some less essential parameters. If we see a "--zip-fd=" parameter, be
// even more aggressive. There won't be much reasonable data here for us in that case anyways (the
// locations are all staged).
static std::string StrippedCommandLine() {
  std::vector<std::string> command;

  // Do a pre-pass to look for zip-fd.
  bool saw_zip_fd = false;
  for (int i = 0; i < original_argc; ++i) {
    if (StartsWith(original_argv[i], "--zip-fd=")) {
      saw_zip_fd = true;
      break;
    }
  }

  // Now filter out things.
  for (int i = 0; i < original_argc; ++i) {
    // All runtime-arg parameters are dropped.
    if (strcmp(original_argv[i], "--runtime-arg") == 0) {
      i++;  // Drop the next part, too.
      continue;
    }

    // Any instruction-setXXX is dropped.
    if (StartsWith(original_argv[i], "--instruction-set")) {
      continue;
    }

    // The boot image is dropped.
    if (StartsWith(original_argv[i], "--boot-image=")) {
      continue;
    }

    // This should leave any dex-file and oat-file options, describing what we compiled.

    // However, we prefer to drop this when we saw --zip-fd.
    if (saw_zip_fd) {
      // Drop anything --zip-X, --dex-X, --oat-X, --swap-X.
      if (StartsWith(original_argv[i], "--zip-") ||
          StartsWith(original_argv[i], "--dex-") ||
          StartsWith(original_argv[i], "--oat-") ||
          StartsWith(original_argv[i], "--swap-")) {
        continue;
      }
    }

    command.push_back(original_argv[i]);
  }

  // Construct the final output.
  if (command.size() <= 1U) {
    // It seems only "/system/bin/dex2oat" is left, or not even that. Use a pretty line.
    return "Starting dex2oat.";
  }
  return Join(command, ' ');
}

static void UsageErrorV(const char* fmt, va_list ap) {
  std::string error;
  StringAppendV(&error, fmt, ap);
  LOG(ERROR) << error;
}

static void UsageError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);
}

NO_RETURN static void Usage(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);

  UsageError("Command: %s", CommandLine().c_str());

  UsageError("Usage: dex2oat [options]...");
  UsageError("");
  UsageError("  -j<number>: specifies the number of threads used for compilation.");
  UsageError("       Default is the number of detected hardware threads available on the");
  UsageError("       host system.");
  UsageError("      Example: -j12");
  UsageError("");
  UsageError("  --dex-file=<dex-file>: specifies a .dex, .jar, or .apk file to compile.");
  UsageError("      Example: --dex-file=/system/framework/core.jar");
  UsageError("");
  UsageError("  --dex-location=<dex-location>: specifies an alternative dex location to");
  UsageError("      encode in the oat file for the corresponding --dex-file argument.");
  UsageError("      Example: --dex-file=/home/build/out/system/framework/core.jar");
  UsageError("               --dex-location=/system/framework/core.jar");
  UsageError("");
  UsageError("  --zip-fd=<file-descriptor>: specifies a file descriptor of a zip file");
  UsageError("      containing a classes.dex file to compile.");
  UsageError("      Example: --zip-fd=5");
  UsageError("");
  UsageError("  --zip-location=<zip-location>: specifies a symbolic name for the file");
  UsageError("      corresponding to the file descriptor specified by --zip-fd.");
  UsageError("      Example: --zip-location=/system/app/Calculator.apk");
  UsageError("");
  UsageError("  --oat-file=<file.oat>: specifies the oat output destination via a filename.");
  UsageError("      Example: --oat-file=/system/framework/boot.oat");
  UsageError("");
  UsageError("  --oat-fd=<number>: specifies the oat output destination via a file descriptor.");
  UsageError("      Example: --oat-fd=6");
  UsageError("");
  UsageError("  --oat-location=<oat-name>: specifies a symbolic name for the file corresponding");
  UsageError("      to the file descriptor specified by --oat-fd.");
  UsageError("      Example: --oat-location=/data/dalvik-cache/system@app@Calculator.apk.oat");
  UsageError("");
  UsageError("  --oat-symbols=<file.oat>: specifies the oat output destination with full symbols.");
  UsageError("      Example: --oat-symbols=/symbols/system/framework/boot.oat");
  UsageError("");
  UsageError("  --image=<file.art>: specifies the output image filename.");
  UsageError("      Example: --image=/system/framework/boot.art");
  UsageError("");
  UsageError("  --image-classes=<classname-file>: specifies classes to include in an image.");
  UsageError("      Example: --image=frameworks/base/preloaded-classes");
  UsageError("");
  UsageError("  --base=<hex-address>: specifies the base address when creating a boot image.");
  UsageError("      Example: --base=0x50000000");
  UsageError("");
  UsageError("  --boot-image=<file.art>: provide the image file for the boot class path.");
  UsageError("      Example: --boot-image=/system/framework/boot.art");
  UsageError("      Default: $ANDROID_ROOT/system/framework/boot.art");
  UsageError("");
  UsageError("  --android-root=<path>: used to locate libraries for portable linking.");
  UsageError("      Example: --android-root=out/host/linux-x86");
  UsageError("      Default: $ANDROID_ROOT");
  UsageError("");
  UsageError("  --instruction-set=(arm|arm64|mips|mips64|x86|x86_64): compile for a particular");
  UsageError("      instruction set.");
  UsageError("      Example: --instruction-set=x86");
  UsageError("      Default: arm");
  UsageError("");
  UsageError("  --instruction-set-features=...,: Specify instruction set features");
  UsageError("      Example: --instruction-set-features=div");
  UsageError("      Default: default");
  UsageError("");
  UsageError("  --compile-pic: Force indirect use of code, methods, and classes");
  UsageError("      Default: disabled");
  UsageError("");
  UsageError("  --compiler-backend=(Quick|Optimizing): select compiler backend");
  UsageError("      set.");
  UsageError("      Example: --compiler-backend=Optimizing");
  if (kUseOptimizingCompiler) {
    UsageError("      Default: Optimizing");
  } else {
    UsageError("      Default: Quick");
  }
  UsageError("");
  UsageError("  --compiler-filter="
                "(verify-none"
                "|interpret-only"
                "|space"
                "|balanced"
                "|speed"
                "|everything"
                "|time):");
  UsageError("      select compiler filter.");
  UsageError("      Example: --compiler-filter=everything");
  UsageError("      Default: speed");
  UsageError("");
  UsageError("  --huge-method-max=<method-instruction-count>: threshold size for a huge");
  UsageError("      method for compiler filter tuning.");
  UsageError("      Example: --huge-method-max=%d", CompilerOptions::kDefaultHugeMethodThreshold);
  UsageError("      Default: %d", CompilerOptions::kDefaultHugeMethodThreshold);
  UsageError("");
  UsageError("  --large-method-max=<method-instruction-count>: threshold size for a large");
  UsageError("      method for compiler filter tuning.");
  UsageError("      Example: --large-method-max=%d", CompilerOptions::kDefaultLargeMethodThreshold);
  UsageError("      Default: %d", CompilerOptions::kDefaultLargeMethodThreshold);
  UsageError("");
  UsageError("  --small-method-max=<method-instruction-count>: threshold size for a small");
  UsageError("      method for compiler filter tuning.");
  UsageError("      Example: --small-method-max=%d", CompilerOptions::kDefaultSmallMethodThreshold);
  UsageError("      Default: %d", CompilerOptions::kDefaultSmallMethodThreshold);
  UsageError("");
  UsageError("  --tiny-method-max=<method-instruction-count>: threshold size for a tiny");
  UsageError("      method for compiler filter tuning.");
  UsageError("      Example: --tiny-method-max=%d", CompilerOptions::kDefaultTinyMethodThreshold);
  UsageError("      Default: %d", CompilerOptions::kDefaultTinyMethodThreshold);
  UsageError("");
  UsageError("  --num-dex-methods=<method-count>: threshold size for a small dex file for");
  UsageError("      compiler filter tuning. If the input has fewer than this many methods");
  UsageError("      and the filter is not interpret-only or verify-none, overrides the");
  UsageError("      filter to use speed");
  UsageError("      Example: --num-dex-method=%d", CompilerOptions::kDefaultNumDexMethodsThreshold);
  UsageError("      Default: %d", CompilerOptions::kDefaultNumDexMethodsThreshold);
  UsageError("");
  UsageError("  --inline-depth-limit=<depth-limit>: the depth limit of inlining for fine tuning");
  UsageError("      the compiler. A zero value will disable inlining. Honored only by Optimizing.");
  UsageError("      Has priority over the --compiler-filter option. Intended for ");
  UsageError("      development/experimental use.");
  UsageError("      Example: --inline-depth-limit=%d", CompilerOptions::kDefaultInlineDepthLimit);
  UsageError("      Default: %d", CompilerOptions::kDefaultInlineDepthLimit);
  UsageError("");
  UsageError("  --inline-max-code-units=<code-units-count>: the maximum code units that a method");
  UsageError("      can have to be considered for inlining. A zero value will disable inlining.");
  UsageError("      Honored only by Optimizing. Has priority over the --compiler-filter option.");
  UsageError("      Intended for development/experimental use.");
  UsageError("      Example: --inline-max-code-units=%d",
             CompilerOptions::kDefaultInlineMaxCodeUnits);
  UsageError("      Default: %d", CompilerOptions::kDefaultInlineMaxCodeUnits);
  UsageError("");
  UsageError("  --dump-timing: display a breakdown of where time was spent");
  UsageError("");
  UsageError("  --include-patch-information: Include patching information so the generated code");
  UsageError("      can have its base address moved without full recompilation.");
  UsageError("");
  UsageError("  --no-include-patch-information: Do not include patching information.");
  UsageError("");
  UsageError("  -g");
  UsageError("  --generate-debug-info: Generate debug information for native debugging,");
  UsageError("      such as stack unwinding information, ELF symbols and DWARF sections.");
  UsageError("      This generates all the available information. Unneeded parts can be");
  UsageError("      stripped using standard command line tools such as strip or objcopy.");
  UsageError("      (enabled by default in debug builds, disabled by default otherwise)");
  UsageError("");
  UsageError("  --no-generate-debug-info: Do not generate debug information for native debugging.");
  UsageError("");
  UsageError("  --runtime-arg <argument>: used to specify various arguments for the runtime,");
  UsageError("      such as initial heap size, maximum heap size, and verbose output.");
  UsageError("      Use a separate --runtime-arg switch for each argument.");
  UsageError("      Example: --runtime-arg -Xms256m");
  UsageError("");
  UsageError("  --profile-file=<filename>: specify profiler output file to use for compilation.");
  UsageError("");
  UsageError("  --print-pass-names: print a list of pass names");
  UsageError("");
  UsageError("  --disable-passes=<pass-names>:  disable one or more passes separated by comma.");
  UsageError("      Example: --disable-passes=UseCount,BBOptimizations");
  UsageError("");
  UsageError("  --print-pass-options: print a list of passes that have configurable options along "
             "with the setting.");
  UsageError("      Will print default if no overridden setting exists.");
  UsageError("");
  UsageError("  --pass-options=Pass1Name:Pass1OptionName:Pass1Option#,"
             "Pass2Name:Pass2OptionName:Pass2Option#");
  UsageError("      Used to specify a pass specific option. The setting itself must be integer.");
  UsageError("      Separator used between options is a comma.");
  UsageError("");
  UsageError("  --swap-file=<file-name>:  specifies a file to use for swap.");
  UsageError("      Example: --swap-file=/data/tmp/swap.001");
  UsageError("");
  UsageError("  --swap-fd=<file-descriptor>:  specifies a file to use for swap (by descriptor).");
  UsageError("      Example: --swap-fd=10");
  UsageError("");
  std::cerr << "See log for usage error information\n";
  exit(EXIT_FAILURE);
}

// The primary goal of the watchdog is to prevent stuck build servers
// during development when fatal aborts lead to a cascade of failures
// that result in a deadlock.
class WatchDog {
// WatchDog defines its own CHECK_PTHREAD_CALL to avoid using LOG which uses locks
#undef CHECK_PTHREAD_CALL
#define CHECK_WATCH_DOG_PTHREAD_CALL(call, args, what) \
  do { \
    int rc = call args; \
    if (rc != 0) { \
      errno = rc; \
      std::string message(# call); \
      message += " failed for "; \
      message += reason; \
      Fatal(message); \
    } \
  } while (false)

 public:
  explicit WatchDog(bool is_watch_dog_enabled) {
    is_watch_dog_enabled_ = is_watch_dog_enabled;
    if (!is_watch_dog_enabled_) {
      return;
    }
    shutting_down_ = false;
    const char* reason = "dex2oat watch dog thread startup";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_init, (&mutex_, nullptr), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_init, (&cond_, nullptr), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_attr_init, (&attr_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_create, (&pthread_, &attr_, &CallBack, this), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_attr_destroy, (&attr_), reason);
  }
  ~WatchDog() {
    if (!is_watch_dog_enabled_) {
      return;
    }
    const char* reason = "dex2oat watch dog thread shutdown";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_lock, (&mutex_), reason);
    shutting_down_ = true;
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_signal, (&cond_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_unlock, (&mutex_), reason);

    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_join, (pthread_, nullptr), reason);

    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_destroy, (&cond_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_destroy, (&mutex_), reason);
  }

 private:
  static void* CallBack(void* arg) {
    WatchDog* self = reinterpret_cast<WatchDog*>(arg);
    ::art::SetThreadName("dex2oat watch dog");
    self->Wait();
    return nullptr;
  }

  NO_RETURN static void Fatal(const std::string& message) {
    // TODO: When we can guarantee it won't prevent shutdown in error cases, move to LOG. However,
    //       it's rather easy to hang in unwinding.
    //       LogLine also avoids ART logging lock issues, as it's really only a wrapper around
    //       logcat logging or stderr output.
    LogMessage::LogLine(__FILE__, __LINE__, LogSeverity::FATAL, message.c_str());
    exit(1);
  }

  void Wait() {
    // TODO: tune the multiplier for GC verification, the following is just to make the timeout
    //       large.
    constexpr int64_t multiplier = kVerifyObjectSupport > kVerifyObjectModeFast ? 100 : 1;
    timespec timeout_ts;
    InitTimeSpec(true, CLOCK_REALTIME, multiplier * kWatchDogTimeoutSeconds * 1000, 0, &timeout_ts);
    const char* reason = "dex2oat watch dog thread waiting";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_lock, (&mutex_), reason);
    while (!shutting_down_) {
      int rc = TEMP_FAILURE_RETRY(pthread_cond_timedwait(&cond_, &mutex_, &timeout_ts));
      if (rc == ETIMEDOUT) {
        Fatal(StringPrintf("dex2oat did not finish after %" PRId64 " seconds",
                           kWatchDogTimeoutSeconds));
      } else if (rc != 0) {
        std::string message(StringPrintf("pthread_cond_timedwait failed: %s",
                                         strerror(errno)));
        Fatal(message.c_str());
      }
    }
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_unlock, (&mutex_), reason);
  }

  // When setting timeouts, keep in mind that the build server may not be as fast as your desktop.
  // Debug builds are slower so they have larger timeouts.
  static constexpr int64_t kSlowdownFactor = kIsDebugBuild ? 5U : 1U;

  // 9.5 minutes scaled by kSlowdownFactor. This is slightly smaller than the Package Manager
  // watchdog (PackageManagerService.WATCHDOG_TIMEOUT, 10 minutes), so that dex2oat will abort
  // itself before that watchdog would take down the system server.
  static constexpr int64_t kWatchDogTimeoutSeconds = kSlowdownFactor * (9 * 60 + 30);

  bool is_watch_dog_enabled_;
  bool shutting_down_;
  // TODO: Switch to Mutex when we can guarantee it won't prevent shutdown in error cases.
  pthread_mutex_t mutex_;
  pthread_cond_t cond_;
  pthread_attr_t attr_;
  pthread_t pthread_;
};

static void ParseStringAfterChar(const std::string& s, char c, std::string* parsed_value) {
  std::string::size_type colon = s.find(c);
  if (colon == std::string::npos) {
    Usage("Missing char %c in option %s\n", c, s.c_str());
  }
  // Add one to remove the char we were trimming until.
  *parsed_value = s.substr(colon + 1);
}

static void ParseDouble(const std::string& option, char after_char, double min, double max,
                        double* parsed_value) {
  std::string substring;
  ParseStringAfterChar(option, after_char, &substring);
  bool sane_val = true;
  double value;
  if (false) {
    // TODO: this doesn't seem to work on the emulator.  b/15114595
    std::stringstream iss(substring);
    iss >> value;
    // Ensure that we have a value, there was no cruft after it and it satisfies a sensible range.
    sane_val = iss.eof() && (value >= min) && (value <= max);
  } else {
    char* end = nullptr;
    value = strtod(substring.c_str(), &end);
    sane_val = *end == '\0' && value >= min && value <= max;
  }
  if (!sane_val) {
    Usage("Invalid double value %s for option %s\n", substring.c_str(), option.c_str());
  }
  *parsed_value = value;
}

static constexpr size_t kMinDexFilesForSwap = 2;
static constexpr size_t kMinDexFileCumulativeSizeForSwap = 20 * MB;

static bool UseSwap(bool is_image, std::vector<const DexFile*>& dex_files) {
  if (is_image) {
    // Don't use swap, we know generation should succeed, and we don't want to slow it down.
    return false;
  }
  if (dex_files.size() < kMinDexFilesForSwap) {
    // If there are less dex files than the threshold, assume it's gonna be fine.
    return false;
  }
  size_t dex_files_size = 0;
  for (const auto* dex_file : dex_files) {
    dex_files_size += dex_file->GetHeader().file_size_;
  }
  return dex_files_size >= kMinDexFileCumulativeSizeForSwap;
}

class Dex2Oat FINAL {
 public:
  explicit Dex2Oat(TimingLogger* timings) :
      compiler_kind_(kUseOptimizingCompiler ? Compiler::kOptimizing : Compiler::kQuick),
      instruction_set_(kRuntimeISA),
      // Take the default set of instruction features from the build.
      verification_results_(nullptr),
      method_inliner_map_(),
      runtime_(nullptr),
      thread_count_(sysconf(_SC_NPROCESSORS_CONF)),
      start_ns_(NanoTime()),
      oat_fd_(-1),
      zip_fd_(-1),
      image_base_(0U),
      image_classes_zip_filename_(nullptr),
      image_classes_filename_(nullptr),
      compiled_classes_zip_filename_(nullptr),
      compiled_classes_filename_(nullptr),
      compiled_methods_zip_filename_(nullptr),
      compiled_methods_filename_(nullptr),
      image_(false),
      is_host_(false),
      driver_(nullptr),
      dump_stats_(false),
      dump_passes_(false),
      dump_timing_(false),
      dump_slow_timing_(kIsDebugBuild),
      swap_fd_(-1),
      timings_(timings) {}

  ~Dex2Oat() {
    // Free opened dex files before deleting the runtime_, because ~DexFile
    // uses MemMap, which is shut down by ~Runtime.
    class_path_files_.clear();
    opened_dex_files_.clear();

    // Log completion time before deleting the runtime_, because this accesses
    // the runtime.
    LogCompletionTime();

    if (kIsDebugBuild || (RUNNING_ON_VALGRIND != 0)) {
      delete runtime_;  // See field declaration for why this is manual.
      delete driver_;
      delete verification_results_;
    }
  }

  // Parse the arguments from the command line. In case of an unrecognized option or impossible
  // values/combinations, a usage error will be displayed and exit() is called. Thus, if the method
  // returns, arguments have been successfully parsed.
  void ParseArgs(int argc, char** argv) {
    original_argc = argc;
    original_argv = argv;

    InitLogging(argv);

    // Skip over argv[0].
    argv++;
    argc--;

    if (argc == 0) {
      Usage("No arguments specified");
    }

    std::string oat_symbols;
    std::string boot_image_filename;
    const char* compiler_filter_string = nullptr;
    bool compile_pic = false;
    int huge_method_threshold = CompilerOptions::kDefaultHugeMethodThreshold;
    int large_method_threshold = CompilerOptions::kDefaultLargeMethodThreshold;
    int small_method_threshold = CompilerOptions::kDefaultSmallMethodThreshold;
    int tiny_method_threshold = CompilerOptions::kDefaultTinyMethodThreshold;
    int num_dex_methods_threshold = CompilerOptions::kDefaultNumDexMethodsThreshold;
    static constexpr int kUnsetInlineDepthLimit = -1;
    int inline_depth_limit = kUnsetInlineDepthLimit;
    static constexpr int kUnsetInlineMaxCodeUnits = -1;
    int inline_max_code_units = kUnsetInlineMaxCodeUnits;

    // Profile file to use
    double top_k_profile_threshold = CompilerOptions::kDefaultTopKProfileThreshold;

    bool debuggable = false;
    bool include_patch_information = CompilerOptions::kDefaultIncludePatchInformation;
    bool generate_debug_info = kIsDebugBuild;
    bool watch_dog_enabled = true;
    bool abort_on_hard_verifier_error = false;
    bool requested_specific_compiler = false;

    PassManagerOptions pass_manager_options;

    std::string error_msg;

    for (int i = 0; i < argc; i++) {
      const StringPiece option(argv[i]);
      const bool log_options = false;
      if (log_options) {
        LOG(INFO) << "dex2oat: option[" << i << "]=" << argv[i];
      }
      if (option.starts_with("--dex-file=")) {
        dex_filenames_.push_back(option.substr(strlen("--dex-file=")).data());
      } else if (option.starts_with("--dex-location=")) {
        dex_locations_.push_back(option.substr(strlen("--dex-location=")).data());
      } else if (option.starts_with("--zip-fd=")) {
        const char* zip_fd_str = option.substr(strlen("--zip-fd=")).data();
        if (!ParseInt(zip_fd_str, &zip_fd_)) {
          Usage("Failed to parse --zip-fd argument '%s' as an integer", zip_fd_str);
        }
        if (zip_fd_ < 0) {
          Usage("--zip-fd passed a negative value %d", zip_fd_);
        }
      } else if (option.starts_with("--zip-location=")) {
        zip_location_ = option.substr(strlen("--zip-location=")).data();
      } else if (option.starts_with("--oat-file=")) {
        oat_filename_ = option.substr(strlen("--oat-file=")).data();
      } else if (option.starts_with("--oat-symbols=")) {
        oat_symbols = option.substr(strlen("--oat-symbols=")).data();
      } else if (option.starts_with("--oat-fd=")) {
        const char* oat_fd_str = option.substr(strlen("--oat-fd=")).data();
        if (!ParseInt(oat_fd_str, &oat_fd_)) {
          Usage("Failed to parse --oat-fd argument '%s' as an integer", oat_fd_str);
        }
        if (oat_fd_ < 0) {
          Usage("--oat-fd passed a negative value %d", oat_fd_);
        }
      } else if (option == "--watch-dog") {
        watch_dog_enabled = true;
      } else if (option == "--no-watch-dog") {
        watch_dog_enabled = false;
      } else if (option.starts_with("-j")) {
        const char* thread_count_str = option.substr(strlen("-j")).data();
        if (!ParseUint(thread_count_str, &thread_count_)) {
          Usage("Failed to parse -j argument '%s' as an integer", thread_count_str);
        }
      } else if (option.starts_with("--oat-location=")) {
        oat_location_ = option.substr(strlen("--oat-location=")).data();
      } else if (option.starts_with("--image=")) {
        image_filename_ = option.substr(strlen("--image=")).data();
      } else if (option.starts_with("--image-classes=")) {
        image_classes_filename_ = option.substr(strlen("--image-classes=")).data();
      } else if (option.starts_with("--image-classes-zip=")) {
        image_classes_zip_filename_ = option.substr(strlen("--image-classes-zip=")).data();
      } else if (option.starts_with("--compiled-classes=")) {
        compiled_classes_filename_ = option.substr(strlen("--compiled-classes=")).data();
      } else if (option.starts_with("--compiled-classes-zip=")) {
        compiled_classes_zip_filename_ = option.substr(strlen("--compiled-classes-zip=")).data();
      } else if (option.starts_with("--compiled-methods=")) {
        compiled_methods_filename_ = option.substr(strlen("--compiled-methods=")).data();
      } else if (option.starts_with("--compiled-methods-zip=")) {
        compiled_methods_zip_filename_ = option.substr(strlen("--compiled-methods-zip=")).data();
      } else if (option.starts_with("--base=")) {
        const char* image_base_str = option.substr(strlen("--base=")).data();
        char* end;
        image_base_ = strtoul(image_base_str, &end, 16);
        if (end == image_base_str || *end != '\0') {
          Usage("Failed to parse hexadecimal value for option %s", option.data());
        }
      } else if (option.starts_with("--boot-image=")) {
        boot_image_filename = option.substr(strlen("--boot-image=")).data();
      } else if (option.starts_with("--android-root=")) {
        android_root_ = option.substr(strlen("--android-root=")).data();
      } else if (option.starts_with("--instruction-set=")) {
        StringPiece instruction_set_str = option.substr(strlen("--instruction-set=")).data();
        // StringPiece is not necessarily zero-terminated, so need to make a copy and ensure it.
        std::unique_ptr<char[]> buf(new char[instruction_set_str.length() + 1]);
        strncpy(buf.get(), instruction_set_str.data(), instruction_set_str.length());
        buf.get()[instruction_set_str.length()] = 0;
        instruction_set_ = GetInstructionSetFromString(buf.get());
        // arm actually means thumb2.
        if (instruction_set_ == InstructionSet::kArm) {
          instruction_set_ = InstructionSet::kThumb2;
        }
      } else if (option.starts_with("--instruction-set-variant=")) {
        StringPiece str = option.substr(strlen("--instruction-set-variant=")).data();
        instruction_set_features_.reset(
            InstructionSetFeatures::FromVariant(instruction_set_, str.as_string(), &error_msg));
        if (instruction_set_features_.get() == nullptr) {
          Usage("%s", error_msg.c_str());
        }
      } else if (option.starts_with("--instruction-set-features=")) {
        StringPiece str = option.substr(strlen("--instruction-set-features=")).data();
        if (instruction_set_features_.get() == nullptr) {
          instruction_set_features_.reset(
              InstructionSetFeatures::FromVariant(instruction_set_, "default", &error_msg));
          if (instruction_set_features_.get() == nullptr) {
            Usage("Problem initializing default instruction set features variant: %s",
                  error_msg.c_str());
          }
        }
        instruction_set_features_.reset(
            instruction_set_features_->AddFeaturesFromString(str.as_string(), &error_msg));
        if (instruction_set_features_.get() == nullptr) {
          Usage("Error parsing '%s': %s", option.data(), error_msg.c_str());
        }
      } else if (option.starts_with("--compiler-backend=")) {
        requested_specific_compiler = true;
        StringPiece backend_str = option.substr(strlen("--compiler-backend=")).data();
        if (backend_str == "Quick") {
          compiler_kind_ = Compiler::kQuick;
        } else if (backend_str == "Optimizing") {
          compiler_kind_ = Compiler::kOptimizing;
        } else {
          Usage("Unknown compiler backend: %s", backend_str.data());
        }
      } else if (option.starts_with("--compiler-filter=")) {
        compiler_filter_string = option.substr(strlen("--compiler-filter=")).data();
      } else if (option == "--compile-pic") {
        compile_pic = true;
      } else if (option.starts_with("--huge-method-max=")) {
        const char* threshold = option.substr(strlen("--huge-method-max=")).data();
        if (!ParseInt(threshold, &huge_method_threshold)) {
          Usage("Failed to parse --huge-method-max '%s' as an integer", threshold);
        }
        if (huge_method_threshold < 0) {
          Usage("--huge-method-max passed a negative value %s", huge_method_threshold);
        }
      } else if (option.starts_with("--large-method-max=")) {
        const char* threshold = option.substr(strlen("--large-method-max=")).data();
        if (!ParseInt(threshold, &large_method_threshold)) {
          Usage("Failed to parse --large-method-max '%s' as an integer", threshold);
        }
        if (large_method_threshold < 0) {
          Usage("--large-method-max passed a negative value %s", large_method_threshold);
        }
      } else if (option.starts_with("--small-method-max=")) {
        const char* threshold = option.substr(strlen("--small-method-max=")).data();
        if (!ParseInt(threshold, &small_method_threshold)) {
          Usage("Failed to parse --small-method-max '%s' as an integer", threshold);
        }
        if (small_method_threshold < 0) {
          Usage("--small-method-max passed a negative value %s", small_method_threshold);
        }
      } else if (option.starts_with("--tiny-method-max=")) {
        const char* threshold = option.substr(strlen("--tiny-method-max=")).data();
        if (!ParseInt(threshold, &tiny_method_threshold)) {
          Usage("Failed to parse --tiny-method-max '%s' as an integer", threshold);
        }
        if (tiny_method_threshold < 0) {
          Usage("--tiny-method-max passed a negative value %s", tiny_method_threshold);
        }
      } else if (option.starts_with("--num-dex-methods=")) {
        const char* threshold = option.substr(strlen("--num-dex-methods=")).data();
        if (!ParseInt(threshold, &num_dex_methods_threshold)) {
          Usage("Failed to parse --num-dex-methods '%s' as an integer", threshold);
        }
        if (num_dex_methods_threshold < 0) {
          Usage("--num-dex-methods passed a negative value %s", num_dex_methods_threshold);
        }
      } else if (option.starts_with("--inline-depth-limit=")) {
        const char* limit = option.substr(strlen("--inline-depth-limit=")).data();
        if (!ParseInt(limit, &inline_depth_limit)) {
          Usage("Failed to parse --inline-depth-limit '%s' as an integer", limit);
        }
        if (inline_depth_limit < 0) {
          Usage("--inline-depth-limit passed a negative value %s", inline_depth_limit);
        }
      } else if (option.starts_with("--inline-max-code-units=")) {
        const char* code_units = option.substr(strlen("--inline-max-code-units=")).data();
        if (!ParseInt(code_units, &inline_max_code_units)) {
          Usage("Failed to parse --inline-max-code-units '%s' as an integer", code_units);
        }
        if (inline_max_code_units < 0) {
          Usage("--inline-max-code-units passed a negative value %s", inline_max_code_units);
        }
      } else if (option == "--host") {
        is_host_ = true;
      } else if (option == "--runtime-arg") {
        if (++i >= argc) {
          Usage("Missing required argument for --runtime-arg");
        }
        if (log_options) {
          LOG(INFO) << "dex2oat: option[" << i << "]=" << argv[i];
        }
        runtime_args_.push_back(argv[i]);
      } else if (option == "--dump-timing") {
        dump_timing_ = true;
      } else if (option == "--dump-passes") {
        dump_passes_ = true;
      } else if (option.starts_with("--dump-cfg=")) {
        dump_cfg_file_name_ = option.substr(strlen("--dump-cfg=")).data();
      } else if (option == "--dump-stats") {
        dump_stats_ = true;
      } else if (option == "--generate-debug-info" || option == "-g") {
        generate_debug_info = true;
      } else if (option == "--no-generate-debug-info") {
        generate_debug_info = false;
      } else if (option == "--debuggable") {
        debuggable = true;
        generate_debug_info = true;
      } else if (option.starts_with("--profile-file=")) {
        profile_file_ = option.substr(strlen("--profile-file=")).data();
        VLOG(compiler) << "dex2oat: profile file is " << profile_file_;
      } else if (option == "--no-profile-file") {
        // No profile
      } else if (option.starts_with("--top-k-profile-threshold=")) {
        ParseDouble(option.data(), '=', 0.0, 100.0, &top_k_profile_threshold);
      } else if (option == "--print-pass-names") {
        pass_manager_options.SetPrintPassNames(true);
      } else if (option.starts_with("--disable-passes=")) {
        const std::string disable_passes = option.substr(strlen("--disable-passes=")).data();
        pass_manager_options.SetDisablePassList(disable_passes);
      } else if (option.starts_with("--print-passes=")) {
        const std::string print_passes = option.substr(strlen("--print-passes=")).data();
        pass_manager_options.SetPrintPassList(print_passes);
      } else if (option == "--print-all-passes") {
        pass_manager_options.SetPrintAllPasses();
      } else if (option.starts_with("--dump-cfg-passes=")) {
        const std::string dump_passes_string = option.substr(strlen("--dump-cfg-passes=")).data();
        pass_manager_options.SetDumpPassList(dump_passes_string);
      } else if (option == "--print-pass-options") {
        pass_manager_options.SetPrintPassOptions(true);
      } else if (option.starts_with("--pass-options=")) {
        const std::string options = option.substr(strlen("--pass-options=")).data();
        pass_manager_options.SetOverriddenPassOptions(options);
      } else if (option == "--include-patch-information") {
        include_patch_information = true;
      } else if (option == "--no-include-patch-information") {
        include_patch_information = false;
      } else if (option.starts_with("--verbose-methods=")) {
        // TODO: rather than switch off compiler logging, make all VLOG(compiler) messages
        //       conditional on having verbost methods.
        gLogVerbosity.compiler = false;
        Split(option.substr(strlen("--verbose-methods=")).ToString(), ',', &verbose_methods_);
      } else if (option.starts_with("--dump-init-failures=")) {
        std::string file_name = option.substr(strlen("--dump-init-failures=")).data();
        init_failure_output_.reset(new std::ofstream(file_name));
        if (init_failure_output_.get() == nullptr) {
          LOG(ERROR) << "Failed to allocate ofstream";
        } else if (init_failure_output_->fail()) {
          LOG(ERROR) << "Failed to open " << file_name << " for writing the initialization "
                     << "failures.";
          init_failure_output_.reset();
        }
      } else if (option.starts_with("--swap-file=")) {
        swap_file_name_ = option.substr(strlen("--swap-file=")).data();
      } else if (option.starts_with("--swap-fd=")) {
        const char* swap_fd_str = option.substr(strlen("--swap-fd=")).data();
        if (!ParseInt(swap_fd_str, &swap_fd_)) {
          Usage("Failed to parse --swap-fd argument '%s' as an integer", swap_fd_str);
        }
        if (swap_fd_ < 0) {
          Usage("--swap-fd passed a negative value %d", swap_fd_);
        }
      } else if (option == "--abort-on-hard-verifier-error") {
        abort_on_hard_verifier_error = true;
      } else {
        Usage("Unknown argument %s", option.data());
      }
    }

    image_ = (!image_filename_.empty());
    if (!requested_specific_compiler && !kUseOptimizingCompiler) {
      // If no specific compiler is requested, the current behavior is
      // to compile the boot image with Quick, and the rest with Optimizing.
      compiler_kind_ = image_ ? Compiler::kQuick : Compiler::kOptimizing;
    }

    if (compiler_kind_ == Compiler::kOptimizing) {
      // Optimizing only supports PIC mode.
      compile_pic = true;
    }

    if (oat_filename_.empty() && oat_fd_ == -1) {
      Usage("Output must be supplied with either --oat-file or --oat-fd");
    }

    if (!oat_filename_.empty() && oat_fd_ != -1) {
      Usage("--oat-file should not be used with --oat-fd");
    }

    if (!oat_symbols.empty() && oat_fd_ != -1) {
      Usage("--oat-symbols should not be used with --oat-fd");
    }

    if (!oat_symbols.empty() && is_host_) {
      Usage("--oat-symbols should not be used with --host");
    }

    if (oat_fd_ != -1 && !image_filename_.empty()) {
      Usage("--oat-fd should not be used with --image");
    }

    if (android_root_.empty()) {
      const char* android_root_env_var = getenv("ANDROID_ROOT");
      if (android_root_env_var == nullptr) {
        Usage("--android-root unspecified and ANDROID_ROOT not set");
      }
      android_root_ += android_root_env_var;
    }

    if (!image_ && boot_image_filename.empty()) {
      boot_image_filename += android_root_;
      boot_image_filename += "/framework/boot.art";
    }
    if (!boot_image_filename.empty()) {
      boot_image_option_ += "-Ximage:";
      boot_image_option_ += boot_image_filename;
    }

    if (image_classes_filename_ != nullptr && !image_) {
      Usage("--image-classes should only be used with --image");
    }

    if (image_classes_filename_ != nullptr && !boot_image_option_.empty()) {
      Usage("--image-classes should not be used with --boot-image");
    }

    if (image_classes_zip_filename_ != nullptr && image_classes_filename_ == nullptr) {
      Usage("--image-classes-zip should be used with --image-classes");
    }

    if (compiled_classes_filename_ != nullptr && !image_) {
      Usage("--compiled-classes should only be used with --image");
    }

    if (compiled_classes_filename_ != nullptr && !boot_image_option_.empty()) {
      Usage("--compiled-classes should not be used with --boot-image");
    }

    if (compiled_classes_zip_filename_ != nullptr && compiled_classes_filename_ == nullptr) {
      Usage("--compiled-classes-zip should be used with --compiled-classes");
    }

    if (dex_filenames_.empty() && zip_fd_ == -1) {
      Usage("Input must be supplied with either --dex-file or --zip-fd");
    }

    if (!dex_filenames_.empty() && zip_fd_ != -1) {
      Usage("--dex-file should not be used with --zip-fd");
    }

    if (!dex_filenames_.empty() && !zip_location_.empty()) {
      Usage("--dex-file should not be used with --zip-location");
    }

    if (dex_locations_.empty()) {
      for (const char* dex_file_name : dex_filenames_) {
        dex_locations_.push_back(dex_file_name);
      }
    } else if (dex_locations_.size() != dex_filenames_.size()) {
      Usage("--dex-location arguments do not match --dex-file arguments");
    }

    if (zip_fd_ != -1 && zip_location_.empty()) {
      Usage("--zip-location should be supplied with --zip-fd");
    }

    if (boot_image_option_.empty()) {
      if (image_base_ == 0) {
        Usage("Non-zero --base not specified");
      }
    }

    oat_stripped_ = oat_filename_;
    if (!oat_symbols.empty()) {
      oat_unstripped_ = oat_symbols;
    } else {
      oat_unstripped_ = oat_filename_;
    }

    // If no instruction set feature was given, use the default one for the target
    // instruction set.
    if (instruction_set_features_.get() == nullptr) {
      instruction_set_features_.reset(
          InstructionSetFeatures::FromVariant(instruction_set_, "default", &error_msg));
      if (instruction_set_features_.get() == nullptr) {
        Usage("Problem initializing default instruction set features variant: %s",
              error_msg.c_str());
      }
    }

    if (instruction_set_ == kRuntimeISA) {
      std::unique_ptr<const InstructionSetFeatures> runtime_features(
          InstructionSetFeatures::FromCppDefines());
      if (!instruction_set_features_->Equals(runtime_features.get())) {
        LOG(WARNING) << "Mismatch between dex2oat instruction set features ("
            << *instruction_set_features_ << ") and those of dex2oat executable ("
            << *runtime_features <<") for the command line:\n"
            << CommandLine();
      }
    }

    if (compiler_filter_string == nullptr) {
      compiler_filter_string = "speed";
    }

    CHECK(compiler_filter_string != nullptr);
    CompilerOptions::CompilerFilter compiler_filter = CompilerOptions::kDefaultCompilerFilter;
    if (strcmp(compiler_filter_string, "verify-none") == 0) {
      compiler_filter = CompilerOptions::kVerifyNone;
    } else if (strcmp(compiler_filter_string, "interpret-only") == 0) {
      compiler_filter = CompilerOptions::kInterpretOnly;
    } else if (strcmp(compiler_filter_string, "verify-at-runtime") == 0) {
      compiler_filter = CompilerOptions::kVerifyAtRuntime;
    } else if (strcmp(compiler_filter_string, "space") == 0) {
      compiler_filter = CompilerOptions::kSpace;
    } else if (strcmp(compiler_filter_string, "balanced") == 0) {
      compiler_filter = CompilerOptions::kBalanced;
    } else if (strcmp(compiler_filter_string, "speed") == 0) {
      compiler_filter = CompilerOptions::kSpeed;
    } else if (strcmp(compiler_filter_string, "everything") == 0) {
      compiler_filter = CompilerOptions::kEverything;
    } else if (strcmp(compiler_filter_string, "time") == 0) {
      compiler_filter = CompilerOptions::kTime;
    } else {
      Usage("Unknown --compiler-filter value %s", compiler_filter_string);
    }

    // It they are not set, use default values for inlining settings.
    // TODO: We should rethink the compiler filter. We mostly save
    // time here, which is orthogonal to space.
    if (inline_depth_limit == kUnsetInlineDepthLimit) {
      inline_depth_limit = (compiler_filter == CompilerOptions::kSpace)
          // Implementation of the space filter: limit inlining depth.
          ? CompilerOptions::kSpaceFilterInlineDepthLimit
          : CompilerOptions::kDefaultInlineDepthLimit;
    }
    if (inline_max_code_units == kUnsetInlineMaxCodeUnits) {
      inline_max_code_units = (compiler_filter == CompilerOptions::kSpace)
          // Implementation of the space filter: limit inlining max code units.
          ? CompilerOptions::kSpaceFilterInlineMaxCodeUnits
          : CompilerOptions::kDefaultInlineMaxCodeUnits;
    }

    // Checks are all explicit until we know the architecture.
    bool implicit_null_checks = false;
    bool implicit_so_checks = false;
    bool implicit_suspend_checks = false;
    // Set the compilation target's implicit checks options.
    switch (instruction_set_) {
      case kArm:
      case kThumb2:
      case kArm64:
      case kX86:
      case kX86_64:
      case kMips:
      case kMips64:
        implicit_null_checks = true;
        implicit_so_checks = true;
        break;

      default:
        // Defaults are correct.
        break;
    }

    compiler_options_.reset(new CompilerOptions(compiler_filter,
                                                huge_method_threshold,
                                                large_method_threshold,
                                                small_method_threshold,
                                                tiny_method_threshold,
                                                num_dex_methods_threshold,
                                                inline_depth_limit,
                                                inline_max_code_units,
                                                include_patch_information,
                                                top_k_profile_threshold,
                                                debuggable,
                                                generate_debug_info,
                                                implicit_null_checks,
                                                implicit_so_checks,
                                                implicit_suspend_checks,
                                                compile_pic,
                                                verbose_methods_.empty() ?
                                                    nullptr :
                                                    &verbose_methods_,
                                                new PassManagerOptions(pass_manager_options),
                                                init_failure_output_.get(),
                                                abort_on_hard_verifier_error));

    // Done with usage checks, enable watchdog if requested
    if (watch_dog_enabled) {
      watchdog_.reset(new WatchDog(true));
    }

    // Fill some values into the key-value store for the oat header.
    key_value_store_.reset(new SafeMap<std::string, std::string>());

    // Insert some compiler things.
    {
      std::ostringstream oss;
      for (int i = 0; i < argc; ++i) {
        if (i > 0) {
          oss << ' ';
        }
        oss << argv[i];
      }
      key_value_store_->Put(OatHeader::kDex2OatCmdLineKey, oss.str());
      oss.str("");  // Reset.
      oss << kRuntimeISA;
      key_value_store_->Put(OatHeader::kDex2OatHostKey, oss.str());
      key_value_store_->Put(OatHeader::kPicKey,
                            compile_pic ? OatHeader::kTrueValue : OatHeader::kFalseValue);
      key_value_store_->Put(OatHeader::kDebuggableKey,
                            debuggable ? OatHeader::kTrueValue : OatHeader::kFalseValue);
    }
  }

  // Check whether the oat output file is writable, and open it for later. Also open a swap file,
  // if a name is given.
  bool OpenFile() {
    bool create_file = !oat_unstripped_.empty();  // as opposed to using open file descriptor
    if (create_file) {
      // We're supposed to create this file. If the file already exists, it may be in use currently.
      // We must not change the content of that file, then. So unlink it first.
      unlink(oat_unstripped_.c_str());

      oat_file_.reset(OS::CreateEmptyFile(oat_unstripped_.c_str()));
      if (oat_location_.empty()) {
        oat_location_ = oat_filename_;
      }
    } else {
      oat_file_.reset(new File(oat_fd_, oat_location_, true));
      oat_file_->DisableAutoClose();
      if (oat_file_->SetLength(0) != 0) {
        PLOG(WARNING) << "Truncating oat file " << oat_location_ << " failed.";
      }
    }
    if (oat_file_.get() == nullptr) {
      PLOG(ERROR) << "Failed to create oat file: " << oat_location_;
      return false;
    }
    if (create_file && fchmod(oat_file_->Fd(), 0644) != 0) {
      PLOG(ERROR) << "Failed to make oat file world readable: " << oat_location_;
      oat_file_->Erase();
      return false;
    }

    // Swap file handling.
    //
    // If the swap fd is not -1, we assume this is the file descriptor of an open but unlinked file
    // that we can use for swap.
    //
    // If the swap fd is -1 and we have a swap-file string, open the given file as a swap file. We
    // will immediately unlink to satisfy the swap fd assumption.
    if (swap_fd_ == -1 && !swap_file_name_.empty()) {
      std::unique_ptr<File> swap_file(OS::CreateEmptyFile(swap_file_name_.c_str()));
      if (swap_file.get() == nullptr) {
        PLOG(ERROR) << "Failed to create swap file: " << swap_file_name_;
        return false;
      }
      swap_fd_ = swap_file->Fd();
      swap_file->MarkUnchecked();     // We don't we to track this, it will be unlinked immediately.
      swap_file->DisableAutoClose();  // We'll handle it ourselves, the File object will be
                                      // released immediately.
      unlink(swap_file_name_.c_str());
    }

    return true;
  }

  void EraseOatFile() {
    DCHECK(oat_file_.get() != nullptr);
    oat_file_->Erase();
    oat_file_.reset();
  }

  // Set up the environment for compilation. Includes starting the runtime and loading/opening the
  // boot class path.
  bool Setup() {
    TimingLogger::ScopedTiming t("dex2oat Setup", timings_);
    RuntimeOptions runtime_options;
    art::MemMap::Init();  // For ZipEntry::ExtractToMemMap.
    if (boot_image_option_.empty()) {
      std::string boot_class_path = "-Xbootclasspath:";
      boot_class_path += Join(dex_filenames_, ':');
      runtime_options.push_back(std::make_pair(boot_class_path, nullptr));
      std::string boot_class_path_locations = "-Xbootclasspath-locations:";
      boot_class_path_locations += Join(dex_locations_, ':');
      runtime_options.push_back(std::make_pair(boot_class_path_locations, nullptr));
    } else {
      runtime_options.push_back(std::make_pair(boot_image_option_, nullptr));
    }
    for (size_t i = 0; i < runtime_args_.size(); i++) {
      runtime_options.push_back(std::make_pair(runtime_args_[i], nullptr));
    }

    verification_results_ = new VerificationResults(compiler_options_.get());
    callbacks_.reset(new QuickCompilerCallbacks(
        verification_results_,
        &method_inliner_map_,
        image_ ?
            CompilerCallbacks::CallbackMode::kCompileBootImage :
            CompilerCallbacks::CallbackMode::kCompileApp));
    runtime_options.push_back(std::make_pair("compilercallbacks", callbacks_.get()));
    runtime_options.push_back(
        std::make_pair("imageinstructionset", GetInstructionSetString(instruction_set_)));

    // Only allow no boot image for the runtime if we're compiling one. When we compile an app,
    // we don't want fallback mode, it will abort as we do not push a boot classpath (it might
    // have been stripped in preopting, anyways).
    if (!image_) {
      runtime_options.push_back(std::make_pair("-Xno-dex-file-fallback", nullptr));
    }

    if (!CreateRuntime(runtime_options)) {
      return false;
    }

    // Runtime::Create acquired the mutator_lock_ that is normally given away when we
    // Runtime::Start, give it away now so that we don't starve GC.
    Thread* self = Thread::Current();
    self->TransitionFromRunnableToSuspended(kNative);
    // If we're doing the image, override the compiler filter to force full compilation. Must be
    // done ahead of WellKnownClasses::Init that causes verification.  Note: doesn't force
    // compilation of class initializers.
    // Whilst we're in native take the opportunity to initialize well known classes.
    WellKnownClasses::Init(self->GetJniEnv());

    // If --image-classes was specified, calculate the full list of classes to include in the image
    if (image_classes_filename_ != nullptr) {
      std::string error_msg;
      if (image_classes_zip_filename_ != nullptr) {
        image_classes_.reset(ReadImageClassesFromZip(image_classes_zip_filename_,
                                                     image_classes_filename_,
                                                     &error_msg));
      } else {
        image_classes_.reset(ReadImageClassesFromFile(image_classes_filename_));
      }
      if (image_classes_.get() == nullptr) {
        LOG(ERROR) << "Failed to create list of image classes from '" << image_classes_filename_ <<
            "': " << error_msg;
        return false;
      }
    } else if (image_) {
      image_classes_.reset(new std::unordered_set<std::string>);
    }
    // If --compiled-classes was specified, calculate the full list of classes to compile in the
    // image.
    if (compiled_classes_filename_ != nullptr) {
      std::string error_msg;
      if (compiled_classes_zip_filename_ != nullptr) {
        compiled_classes_.reset(ReadImageClassesFromZip(compiled_classes_zip_filename_,
                                                        compiled_classes_filename_,
                                                        &error_msg));
      } else {
        compiled_classes_.reset(ReadImageClassesFromFile(compiled_classes_filename_));
      }
      if (compiled_classes_.get() == nullptr) {
        LOG(ERROR) << "Failed to create list of compiled classes from '"
                   << compiled_classes_filename_ << "': " << error_msg;
        return false;
      }
    } else {
      compiled_classes_.reset(nullptr);  // By default compile everything.
    }
    // If --compiled-methods was specified, read the methods to compile from the given file(s).
    if (compiled_methods_filename_ != nullptr) {
      std::string error_msg;
      if (compiled_methods_zip_filename_ != nullptr) {
        compiled_methods_.reset(ReadCommentedInputFromZip(compiled_methods_zip_filename_,
                                                          compiled_methods_filename_,
                                                          nullptr,            // No post-processing.
                                                          &error_msg));
      } else {
        compiled_methods_.reset(ReadCommentedInputFromFile(compiled_methods_filename_,
                                                           nullptr));         // No post-processing.
      }
      if (compiled_methods_.get() == nullptr) {
        LOG(ERROR) << "Failed to create list of compiled methods from '"
            << compiled_methods_filename_ << "': " << error_msg;
        return false;
      }
    } else {
      compiled_methods_.reset(nullptr);  // By default compile everything.
    }

    if (boot_image_option_.empty()) {
      dex_files_ = Runtime::Current()->GetClassLinker()->GetBootClassPath();
    } else {
      if (dex_filenames_.empty()) {
        ATRACE_BEGIN("Opening zip archive from file descriptor");
        std::string error_msg;
        std::unique_ptr<ZipArchive> zip_archive(ZipArchive::OpenFromFd(zip_fd_,
                                                                       zip_location_.c_str(),
                                                                       &error_msg));
        if (zip_archive.get() == nullptr) {
          LOG(ERROR) << "Failed to open zip from file descriptor for '" << zip_location_ << "': "
              << error_msg;
          return false;
        }
        if (!DexFile::OpenFromZip(*zip_archive.get(), zip_location_, &error_msg, &opened_dex_files_)) {
          LOG(ERROR) << "Failed to open dex from file descriptor for zip file '" << zip_location_
              << "': " << error_msg;
          return false;
        }
        for (auto& dex_file : opened_dex_files_) {
          dex_files_.push_back(dex_file.get());
        }
        ATRACE_END();
      } else {
        size_t failure_count = OpenDexFiles(dex_filenames_, dex_locations_, &opened_dex_files_);
        if (failure_count > 0) {
          LOG(ERROR) << "Failed to open some dex files: " << failure_count;
          return false;
        }
        for (auto& dex_file : opened_dex_files_) {
          dex_files_.push_back(dex_file.get());
        }
      }

      constexpr bool kSaveDexInput = false;
      if (kSaveDexInput) {
        for (size_t i = 0; i < dex_files_.size(); ++i) {
          const DexFile* dex_file = dex_files_[i];
          std::string tmp_file_name(StringPrintf("/data/local/tmp/dex2oat.%d.%zd.dex",
                                                 getpid(), i));
          std::unique_ptr<File> tmp_file(OS::CreateEmptyFile(tmp_file_name.c_str()));
          if (tmp_file.get() == nullptr) {
            PLOG(ERROR) << "Failed to open file " << tmp_file_name
                << ". Try: adb shell chmod 777 /data/local/tmp";
            continue;
          }
          // This is just dumping files for debugging. Ignore errors, and leave remnants.
          UNUSED(tmp_file->WriteFully(dex_file->Begin(), dex_file->Size()));
          UNUSED(tmp_file->Flush());
          UNUSED(tmp_file->Close());
          LOG(INFO) << "Wrote input to " << tmp_file_name;
        }
      }
    }
    // Ensure opened dex files are writable for dex-to-dex transformations.
    for (const auto& dex_file : dex_files_) {
      if (!dex_file->EnableWrite()) {
        PLOG(ERROR) << "Failed to make .dex file writeable '" << dex_file->GetLocation() << "'\n";
      }
    }

    // If we use a swap file, ensure we are above the threshold to make it necessary.
    if (swap_fd_ != -1) {
      if (!UseSwap(image_, dex_files_)) {
        close(swap_fd_);
        swap_fd_ = -1;
        VLOG(compiler) << "Decided to run without swap.";
      } else {
        LOG(INFO) << "Large app, accepted running with swap.";
      }
    }
    // Note that dex2oat won't close the swap_fd_. The compiler driver's swap space will do that.

    /*
     * If we're not in interpret-only or verify-none mode, go ahead and compile small applications.
     * Don't bother to check if we're doing the image.
     */
    if (!image_ &&
        compiler_options_->IsCompilationEnabled() &&
        compiler_kind_ == Compiler::kQuick) {
      size_t num_methods = 0;
      for (size_t i = 0; i != dex_files_.size(); ++i) {
        const DexFile* dex_file = dex_files_[i];
        CHECK(dex_file != nullptr);
        num_methods += dex_file->NumMethodIds();
      }
      if (num_methods <= compiler_options_->GetNumDexMethodsThreshold()) {
        compiler_options_->SetCompilerFilter(CompilerOptions::kSpeed);
        VLOG(compiler) << "Below method threshold, compiling anyways";
      }
    }

    return true;
  }

  // Create and invoke the compiler driver. This will compile all the dex files.
  void Compile() {
    TimingLogger::ScopedTiming t("dex2oat Compile", timings_);
    compiler_phases_timings_.reset(new CumulativeLogger("compilation times"));

    // Handle and ClassLoader creation needs to come after Runtime::Create
    jobject class_loader = nullptr;
    Thread* self = Thread::Current();
    if (!boot_image_option_.empty()) {
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      OpenClassPathFiles(runtime_->GetClassPathString(), dex_files_, &class_path_files_);
      ScopedObjectAccess soa(self);

      // Classpath: first the class-path given.
      std::vector<const DexFile*> class_path_files;
      for (auto& class_path_file : class_path_files_) {
        class_path_files.push_back(class_path_file.get());
      }

      // Store the classpath we have right now.
      key_value_store_->Put(OatHeader::kClassPathKey,
                            OatFile::EncodeDexFileDependencies(class_path_files));

      // Then the dex files we'll compile. Thus we'll resolve the class-path first.
      class_path_files.insert(class_path_files.end(), dex_files_.begin(), dex_files_.end());

      class_loader = class_linker->CreatePathClassLoader(self, class_path_files);
    }

    driver_ = new CompilerDriver(compiler_options_.get(),
                                 verification_results_,
                                 &method_inliner_map_,
                                 compiler_kind_,
                                 instruction_set_,
                                 instruction_set_features_.get(),
                                 image_,
                                 image_classes_.release(),
                                 compiled_classes_.release(),
                                 nullptr,
                                 thread_count_,
                                 dump_stats_,
                                 dump_passes_,
                                 dump_cfg_file_name_,
                                 compiler_phases_timings_.get(),
                                 swap_fd_,
                                 profile_file_);

    driver_->CompileAll(class_loader, dex_files_, timings_);
  }

  // Notes on the interleaving of creating the image and oat file to
  // ensure the references between the two are correct.
  //
  // Currently we have a memory layout that looks something like this:
  //
  // +--------------+
  // | image        |
  // +--------------+
  // | boot oat     |
  // +--------------+
  // | alloc spaces |
  // +--------------+
  //
  // There are several constraints on the loading of the image and boot.oat.
  //
  // 1. The image is expected to be loaded at an absolute address and
  // contains Objects with absolute pointers within the image.
  //
  // 2. There are absolute pointers from Methods in the image to their
  // code in the oat.
  //
  // 3. There are absolute pointers from the code in the oat to Methods
  // in the image.
  //
  // 4. There are absolute pointers from code in the oat to other code
  // in the oat.
  //
  // To get this all correct, we go through several steps.
  //
  // 1. We prepare offsets for all data in the oat file and calculate
  // the oat data size and code size. During this stage, we also set
  // oat code offsets in methods for use by the image writer.
  //
  // 2. We prepare offsets for the objects in the image and calculate
  // the image size.
  //
  // 3. We create the oat file. Originally this was just our own proprietary
  // file but now it is contained within an ELF dynamic object (aka an .so
  // file). Since we know the image size and oat data size and code size we
  // can prepare the ELF headers and we then know the ELF memory segment
  // layout and we can now resolve all references. The compiler provides
  // LinkerPatch information in each CompiledMethod and we resolve these,
  // using the layout information and image object locations provided by
  // image writer, as we're writing the method code.
  //
  // 4. We create the image file. It needs to know where the oat file
  // will be loaded after itself. Originally when oat file was simply
  // memory mapped so we could predict where its contents were based
  // on the file size. Now that it is an ELF file, we need to inspect
  // the ELF file to understand the in memory segment layout including
  // where the oat header is located within.
  // TODO: We could just remember this information from step 3.
  //
  // 5. We fixup the ELF program headers so that dlopen will try to
  // load the .so at the desired location at runtime by offsetting the
  // Elf32_Phdr.p_vaddr values by the desired base address.
  // TODO: Do this in step 3. We already know the layout there.
  //
  // Steps 1.-3. are done by the CreateOatFile() above, steps 4.-5.
  // are done by the CreateImageFile() below.


  // Write out the generated code part. Calls the OatWriter and ElfBuilder. Also prepares the
  // ImageWriter, if necessary.
  // Note: Flushing (and closing) the file is the caller's responsibility, except for the failure
  //       case (when the file will be explicitly erased).
  bool CreateOatFile() {
    CHECK(key_value_store_.get() != nullptr);

    TimingLogger::ScopedTiming t("dex2oat Oat", timings_);

    std::unique_ptr<OatWriter> oat_writer;
    {
      TimingLogger::ScopedTiming t2("dex2oat OatWriter", timings_);
      std::string image_file_location;
      uint32_t image_file_location_oat_checksum = 0;
      uintptr_t image_file_location_oat_data_begin = 0;
      int32_t image_patch_delta = 0;
      if (image_) {
        PrepareImageWriter(image_base_);
      } else {
        TimingLogger::ScopedTiming t3("Loading image checksum", timings_);
        gc::space::ImageSpace* image_space = Runtime::Current()->GetHeap()->GetImageSpace();
        image_file_location_oat_checksum = image_space->GetImageHeader().GetOatChecksum();
        image_file_location_oat_data_begin =
            reinterpret_cast<uintptr_t>(image_space->GetImageHeader().GetOatDataBegin());
        image_file_location = image_space->GetImageFilename();
        image_patch_delta = image_space->GetImageHeader().GetPatchDelta();
      }

      if (!image_file_location.empty()) {
        key_value_store_->Put(OatHeader::kImageLocationKey, image_file_location);
      }

      oat_writer.reset(new OatWriter(dex_files_, image_file_location_oat_checksum,
                                     image_file_location_oat_data_begin,
                                     image_patch_delta,
                                     driver_,
                                     image_writer_.get(),
                                     timings_,
                                     key_value_store_.get()));
    }

    if (image_) {
      // The OatWriter constructor has already updated offsets in methods and we need to
      // prepare method offsets in the image address space for direct method patching.
      TimingLogger::ScopedTiming t2("dex2oat Prepare image address space", timings_);
      if (!image_writer_->PrepareImageAddressSpace()) {
        LOG(ERROR) << "Failed to prepare image address space.";
        return false;
      }
    }

    {
      TimingLogger::ScopedTiming t2("dex2oat Write ELF", timings_);
      if (!driver_->WriteElf(android_root_, is_host_, dex_files_, oat_writer.get(),
                             oat_file_.get())) {
        LOG(ERROR) << "Failed to write ELF file " << oat_file_->GetPath();
        return false;
      }
    }

    VLOG(compiler) << "Oat file written successfully (unstripped): " << oat_location_;
    return true;
  }

  // If we are compiling an image, invoke the image creation routine. Else just skip.
  bool HandleImage() {
    if (image_) {
      TimingLogger::ScopedTiming t("dex2oat ImageWriter", timings_);
      if (!CreateImageFile()) {
        return false;
      }
      VLOG(compiler) << "Image written successfully: " << image_filename_;
    }
    return true;
  }

  // Create a copy from unstripped to stripped.
  bool CopyUnstrippedToStripped() {
    // If we don't want to strip in place, copy from unstripped location to stripped location.
    // We need to strip after image creation because FixupElf needs to use .strtab.
    if (oat_unstripped_ != oat_stripped_) {
      // If the oat file is still open, flush it.
      if (oat_file_.get() != nullptr && oat_file_->IsOpened()) {
        if (!FlushCloseOatFile()) {
          return false;
        }
      }

      TimingLogger::ScopedTiming t("dex2oat OatFile copy", timings_);
      std::unique_ptr<File> in(OS::OpenFileForReading(oat_unstripped_.c_str()));
      std::unique_ptr<File> out(OS::CreateEmptyFile(oat_stripped_.c_str()));
      size_t buffer_size = 8192;
      std::unique_ptr<uint8_t[]> buffer(new uint8_t[buffer_size]);
      while (true) {
        int bytes_read = TEMP_FAILURE_RETRY(read(in->Fd(), buffer.get(), buffer_size));
        if (bytes_read <= 0) {
          break;
        }
        bool write_ok = out->WriteFully(buffer.get(), bytes_read);
        CHECK(write_ok);
      }
      if (out->FlushCloseOrErase() != 0) {
        PLOG(ERROR) << "Failed to flush and close copied oat file: " << oat_stripped_;
        return false;
      }
      VLOG(compiler) << "Oat file copied successfully (stripped): " << oat_stripped_;
    }
    return true;
  }

  bool FlushOatFile() {
    if (oat_file_.get() != nullptr) {
      TimingLogger::ScopedTiming t2("dex2oat Flush ELF", timings_);
      if (oat_file_->Flush() != 0) {
        PLOG(ERROR) << "Failed to flush oat file: " << oat_location_ << " / "
            << oat_filename_;
        oat_file_->Erase();
        return false;
      }
    }
    return true;
  }

  bool FlushCloseOatFile() {
    if (oat_file_.get() != nullptr) {
      std::unique_ptr<File> tmp(oat_file_.release());
      if (tmp->FlushCloseOrErase() != 0) {
        PLOG(ERROR) << "Failed to flush and close oat file: " << oat_location_ << " / "
            << oat_filename_;
        return false;
      }
    }
    return true;
  }

  void DumpTiming() {
    if (dump_timing_ || (dump_slow_timing_ && timings_->GetTotalNs() > MsToNs(1000))) {
      LOG(INFO) << Dumpable<TimingLogger>(*timings_);
    }
    if (dump_passes_) {
      LOG(INFO) << Dumpable<CumulativeLogger>(*driver_->GetTimingsLogger());
    }
  }

  CompilerOptions* GetCompilerOptions() const {
    return compiler_options_.get();
  }

  bool IsImage() const {
    return image_;
  }

  bool IsHost() const {
    return is_host_;
  }

 private:
  static size_t OpenDexFiles(const std::vector<const char*>& dex_filenames,
                             const std::vector<const char*>& dex_locations,
                             std::vector<std::unique_ptr<const DexFile>>* dex_files) {
    DCHECK(dex_files != nullptr) << "OpenDexFiles out-param is nullptr";
    size_t failure_count = 0;
    for (size_t i = 0; i < dex_filenames.size(); i++) {
      const char* dex_filename = dex_filenames[i];
      const char* dex_location = dex_locations[i];
      ATRACE_BEGIN(StringPrintf("Opening dex file '%s'", dex_filenames[i]).c_str());
      std::string error_msg;
      if (!OS::FileExists(dex_filename)) {
        LOG(WARNING) << "Skipping non-existent dex file '" << dex_filename << "'";
        continue;
      }
      if (!DexFile::Open(dex_filename, dex_location, &error_msg, dex_files)) {
        LOG(WARNING) << "Failed to open .dex from file '" << dex_filename << "': " << error_msg;
        ++failure_count;
      }
      ATRACE_END();
    }
    return failure_count;
  }

  // Returns true if dex_files has a dex with the named location. We compare canonical locations,
  // so that relative and absolute paths will match. Not caching for the dex_files isn't very
  // efficient, but under normal circumstances the list is neither large nor is this part too
  // sensitive.
  static bool DexFilesContains(const std::vector<const DexFile*>& dex_files,
                               const std::string& location) {
    std::string canonical_location(DexFile::GetDexCanonicalLocation(location.c_str()));
    for (size_t i = 0; i < dex_files.size(); ++i) {
      if (DexFile::GetDexCanonicalLocation(dex_files[i]->GetLocation().c_str()) ==
          canonical_location) {
        return true;
      }
    }
    return false;
  }

  // Appends to opened_dex_files any elements of class_path that dex_files
  // doesn't already contain. This will open those dex files as necessary.
  static void OpenClassPathFiles(const std::string& class_path,
                                 std::vector<const DexFile*> dex_files,
                                 std::vector<std::unique_ptr<const DexFile>>* opened_dex_files) {
    DCHECK(opened_dex_files != nullptr) << "OpenClassPathFiles out-param is nullptr";
    std::vector<std::string> parsed;
    Split(class_path, ':', &parsed);
    // Take Locks::mutator_lock_ so that lock ordering on the ClassLinker::dex_lock_ is maintained.
    ScopedObjectAccess soa(Thread::Current());
    for (size_t i = 0; i < parsed.size(); ++i) {
      if (DexFilesContains(dex_files, parsed[i])) {
        continue;
      }
      std::string error_msg;
      if (!DexFile::Open(parsed[i].c_str(), parsed[i].c_str(), &error_msg, opened_dex_files)) {
        LOG(WARNING) << "Failed to open dex file '" << parsed[i] << "': " << error_msg;
      }
    }
  }

  // Create a runtime necessary for compilation.
  bool CreateRuntime(const RuntimeOptions& runtime_options)
      SHARED_TRYLOCK_FUNCTION(true, Locks::mutator_lock_) {
    if (!Runtime::Create(runtime_options, false)) {
      LOG(ERROR) << "Failed to create runtime";
      return false;
    }
    Runtime* runtime = Runtime::Current();
    runtime->SetInstructionSet(instruction_set_);
    for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
      Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
      if (!runtime->HasCalleeSaveMethod(type)) {
        runtime->SetCalleeSaveMethod(runtime->CreateCalleeSaveMethod(), type);
      }
    }
    runtime->GetClassLinker()->FixupDexCaches(runtime->GetResolutionMethod());

    // Initialize maps for unstarted runtime. This needs to be here, as running clinits needs this
    // set up.
    interpreter::UnstartedRuntime::Initialize();

    runtime->GetClassLinker()->RunRootClinits();
    runtime_ = runtime;

    return true;
  }

  void PrepareImageWriter(uintptr_t image_base) {
    image_writer_.reset(new ImageWriter(*driver_, image_base, compiler_options_->GetCompilePic()));
  }

  // Let the ImageWriter write the image file. If we do not compile PIC, also fix up the oat file.
  bool CreateImageFile()
      LOCKS_EXCLUDED(Locks::mutator_lock_) {
    CHECK(image_writer_ != nullptr);
    if (!image_writer_->Write(image_filename_, oat_unstripped_, oat_location_)) {
      LOG(ERROR) << "Failed to create image file " << image_filename_;
      return false;
    }
    uintptr_t oat_data_begin = image_writer_->GetOatDataBegin();

    // Destroy ImageWriter before doing FixupElf.
    image_writer_.reset();

    // Do not fix up the ELF file if we are --compile-pic
    if (!compiler_options_->GetCompilePic()) {
      std::unique_ptr<File> oat_file(OS::OpenFileReadWrite(oat_unstripped_.c_str()));
      if (oat_file.get() == nullptr) {
        PLOG(ERROR) << "Failed to open ELF file: " << oat_unstripped_;
        return false;
      }

      if (!ElfWriter::Fixup(oat_file.get(), oat_data_begin)) {
        oat_file->Erase();
        LOG(ERROR) << "Failed to fixup ELF file " << oat_file->GetPath();
        return false;
      }

      if (oat_file->FlushCloseOrErase()) {
        PLOG(ERROR) << "Failed to flush and close fixed ELF file " << oat_file->GetPath();
        return false;
      }
    }

    return true;
  }

  // Reads the class names (java.lang.Object) and returns a set of descriptors (Ljava/lang/Object;)
  static std::unordered_set<std::string>* ReadImageClassesFromFile(
      const char* image_classes_filename) {
    std::function<std::string(const char*)> process = DotToDescriptor;
    return ReadCommentedInputFromFile(image_classes_filename, &process);
  }

  // Reads the class names (java.lang.Object) and returns a set of descriptors (Ljava/lang/Object;)
  static std::unordered_set<std::string>* ReadImageClassesFromZip(
        const char* zip_filename,
        const char* image_classes_filename,
        std::string* error_msg) {
    std::function<std::string(const char*)> process = DotToDescriptor;
    return ReadCommentedInputFromZip(zip_filename, image_classes_filename, &process, error_msg);
  }

  // Read lines from the given file, dropping comments and empty lines. Post-process each line with
  // the given function.
  static std::unordered_set<std::string>* ReadCommentedInputFromFile(
      const char* input_filename, std::function<std::string(const char*)>* process) {
    std::unique_ptr<std::ifstream> input_file(new std::ifstream(input_filename, std::ifstream::in));
    if (input_file.get() == nullptr) {
      LOG(ERROR) << "Failed to open input file " << input_filename;
      return nullptr;
    }
    std::unique_ptr<std::unordered_set<std::string>> result(
        ReadCommentedInputStream(*input_file, process));
    input_file->close();
    return result.release();
  }

  // Read lines from the given file from the given zip file, dropping comments and empty lines.
  // Post-process each line with the given function.
  static std::unordered_set<std::string>* ReadCommentedInputFromZip(
      const char* zip_filename,
      const char* input_filename,
      std::function<std::string(const char*)>* process,
      std::string* error_msg) {
    std::unique_ptr<ZipArchive> zip_archive(ZipArchive::Open(zip_filename, error_msg));
    if (zip_archive.get() == nullptr) {
      return nullptr;
    }
    std::unique_ptr<ZipEntry> zip_entry(zip_archive->Find(input_filename, error_msg));
    if (zip_entry.get() == nullptr) {
      *error_msg = StringPrintf("Failed to find '%s' within '%s': %s", input_filename,
                                zip_filename, error_msg->c_str());
      return nullptr;
    }
    std::unique_ptr<MemMap> input_file(zip_entry->ExtractToMemMap(zip_filename,
                                                                  input_filename,
                                                                  error_msg));
    if (input_file.get() == nullptr) {
      *error_msg = StringPrintf("Failed to extract '%s' from '%s': %s", input_filename,
                                zip_filename, error_msg->c_str());
      return nullptr;
    }
    const std::string input_string(reinterpret_cast<char*>(input_file->Begin()),
                                   input_file->Size());
    std::istringstream input_stream(input_string);
    return ReadCommentedInputStream(input_stream, process);
  }

  // Read lines from the given stream, dropping comments and empty lines. Post-process each line
  // with the given function.
  static std::unordered_set<std::string>* ReadCommentedInputStream(
      std::istream& in_stream,
      std::function<std::string(const char*)>* process) {
    std::unique_ptr<std::unordered_set<std::string>> image_classes(
        new std::unordered_set<std::string>);
    while (in_stream.good()) {
      std::string dot;
      std::getline(in_stream, dot);
      if (StartsWith(dot, "#") || dot.empty()) {
        continue;
      }
      if (process != nullptr) {
        std::string descriptor((*process)(dot.c_str()));
        image_classes->insert(descriptor);
      } else {
        image_classes->insert(dot);
      }
    }
    return image_classes.release();
  }

  void LogCompletionTime() {
    // Note: when creation of a runtime fails, e.g., when trying to compile an app but when there
    //       is no image, there won't be a Runtime::Current().
    // Note: driver creation can fail when loading an invalid dex file.
    LOG(INFO) << "dex2oat took " << PrettyDuration(NanoTime() - start_ns_)
              << " (threads: " << thread_count_ << ") "
              << ((Runtime::Current() != nullptr && driver_ != nullptr) ?
                  driver_->GetMemoryUsageString(kIsDebugBuild || VLOG_IS_ON(compiler)) :
                  "");
  }

  std::unique_ptr<CompilerOptions> compiler_options_;
  Compiler::Kind compiler_kind_;

  InstructionSet instruction_set_;
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features_;

  std::unique_ptr<SafeMap<std::string, std::string> > key_value_store_;

  // Not a unique_ptr as we want to just exit on non-debug builds, not bringing the compiler down
  // in an orderly fashion. The destructor takes care of deleting this.
  VerificationResults* verification_results_;

  DexFileToMethodInlinerMap method_inliner_map_;
  std::unique_ptr<QuickCompilerCallbacks> callbacks_;

  // Ownership for the class path files.
  std::vector<std::unique_ptr<const DexFile>> class_path_files_;

  // Not a unique_ptr as we want to just exit on non-debug builds, not bringing the runtime down
  // in an orderly fashion. The destructor takes care of deleting this.
  Runtime* runtime_;

  size_t thread_count_;
  uint64_t start_ns_;
  std::unique_ptr<WatchDog> watchdog_;
  std::unique_ptr<File> oat_file_;
  std::string oat_stripped_;
  std::string oat_unstripped_;
  std::string oat_location_;
  std::string oat_filename_;
  int oat_fd_;
  std::vector<const char*> dex_filenames_;
  std::vector<const char*> dex_locations_;
  int zip_fd_;
  std::string zip_location_;
  std::string boot_image_option_;
  std::vector<const char*> runtime_args_;
  std::string image_filename_;
  uintptr_t image_base_;
  const char* image_classes_zip_filename_;
  const char* image_classes_filename_;
  const char* compiled_classes_zip_filename_;
  const char* compiled_classes_filename_;
  const char* compiled_methods_zip_filename_;
  const char* compiled_methods_filename_;
  std::unique_ptr<std::unordered_set<std::string>> image_classes_;
  std::unique_ptr<std::unordered_set<std::string>> compiled_classes_;
  std::unique_ptr<std::unordered_set<std::string>> compiled_methods_;
  bool image_;
  std::unique_ptr<ImageWriter> image_writer_;
  bool is_host_;
  std::string android_root_;
  std::vector<const DexFile*> dex_files_;
  std::vector<std::unique_ptr<const DexFile>> opened_dex_files_;

  // Not a unique_ptr as we want to just exit on non-debug builds, not bringing the driver down
  // in an orderly fashion. The destructor takes care of deleting this.
  CompilerDriver* driver_;

  std::vector<std::string> verbose_methods_;
  bool dump_stats_;
  bool dump_passes_;
  bool dump_timing_;
  bool dump_slow_timing_;
  std::string dump_cfg_file_name_;
  std::string swap_file_name_;
  int swap_fd_;
  std::string profile_file_;  // Profile file to use
  TimingLogger* timings_;
  std::unique_ptr<CumulativeLogger> compiler_phases_timings_;
  std::unique_ptr<std::ostream> init_failure_output_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Dex2Oat);
};

static void b13564922() {
#if defined(__linux__) && defined(__arm__)
  int major, minor;
  struct utsname uts;
  if (uname(&uts) != -1 &&
      sscanf(uts.release, "%d.%d", &major, &minor) == 2 &&
      ((major < 3) || ((major == 3) && (minor < 4)))) {
    // Kernels before 3.4 don't handle the ASLR well and we can run out of address
    // space (http://b/13564922). Work around the issue by inhibiting further mmap() randomization.
    int old_personality = personality(0xffffffff);
    if ((old_personality & ADDR_NO_RANDOMIZE) == 0) {
      int new_personality = personality(old_personality | ADDR_NO_RANDOMIZE);
      if (new_personality == -1) {
        LOG(WARNING) << "personality(. | ADDR_NO_RANDOMIZE) failed.";
      }
    }
  }
#endif
}

static int CompileImage(Dex2Oat& dex2oat) {
  dex2oat.Compile();

  // Create the boot.oat.
  if (!dex2oat.CreateOatFile()) {
    dex2oat.EraseOatFile();
    return EXIT_FAILURE;
  }

  // Flush and close the boot.oat. We always expect the output file by name, and it will be
  // re-opened from the unstripped name.
  if (!dex2oat.FlushCloseOatFile()) {
    return EXIT_FAILURE;
  }

  // Creates the boot.art and patches the boot.oat.
  if (!dex2oat.HandleImage()) {
    return EXIT_FAILURE;
  }

  // When given --host, finish early without stripping.
  if (dex2oat.IsHost()) {
    dex2oat.DumpTiming();
    return EXIT_SUCCESS;
  }

  // Copy unstripped to stripped location, if necessary.
  if (!dex2oat.CopyUnstrippedToStripped()) {
    return EXIT_FAILURE;
  }

  // FlushClose again, as stripping might have re-opened the oat file.
  if (!dex2oat.FlushCloseOatFile()) {
    return EXIT_FAILURE;
  }

  dex2oat.DumpTiming();
  return EXIT_SUCCESS;
}

static int CompileApp(Dex2Oat& dex2oat) {
  dex2oat.Compile();

  // Create the app oat.
  if (!dex2oat.CreateOatFile()) {
    dex2oat.EraseOatFile();
    return EXIT_FAILURE;
  }

  // Do not close the oat file here. We might haven gotten the output file by file descriptor,
  // which we would lose.
  if (!dex2oat.FlushOatFile()) {
    return EXIT_FAILURE;
  }

  // When given --host, finish early without stripping.
  if (dex2oat.IsHost()) {
    if (!dex2oat.FlushCloseOatFile()) {
      return EXIT_FAILURE;
    }

    dex2oat.DumpTiming();
    return EXIT_SUCCESS;
  }

  // Copy unstripped to stripped location, if necessary. This will implicitly flush & close the
  // unstripped version. If this is given, we expect to be able to open writable files by name.
  if (!dex2oat.CopyUnstrippedToStripped()) {
    return EXIT_FAILURE;
  }

  // Flush and close the file.
  if (!dex2oat.FlushCloseOatFile()) {
    return EXIT_FAILURE;
  }

  dex2oat.DumpTiming();
  return EXIT_SUCCESS;
}

static int dex2oat(int argc, char** argv) {
  b13564922();

  TimingLogger timings("compiler", false, false);

  Dex2Oat dex2oat(&timings);

  // Parse arguments. Argument mistakes will lead to exit(EXIT_FAILURE) in UsageError.
  dex2oat.ParseArgs(argc, argv);

  // Check early that the result of compilation can be written
  if (!dex2oat.OpenFile()) {
    return EXIT_FAILURE;
  }

  // Print the complete line when any of the following is true:
  //   1) Debug build
  //   2) Compiling an image
  //   3) Compiling with --host
  //   4) Compiling on the host (not a target build)
  // Otherwise, print a stripped command line.
  if (kIsDebugBuild || dex2oat.IsImage() || dex2oat.IsHost() || !kIsTargetBuild) {
    LOG(INFO) << CommandLine();
  } else {
    LOG(INFO) << StrippedCommandLine();
  }

  if (!dex2oat.Setup()) {
    dex2oat.EraseOatFile();
    return EXIT_FAILURE;
  }

  if (dex2oat.IsImage()) {
    return CompileImage(dex2oat);
  } else {
    return CompileApp(dex2oat);
  }
}
}  // namespace art

int main(int argc, char** argv) {
  int result = art::dex2oat(argc, argv);
  // Everything was done, do an explicit exit here to avoid running Runtime destructors that take
  // time (bug 10645725) unless we're a debug build or running on valgrind. Note: The Dex2Oat class
  // should not destruct the runtime in this case.
  if (!art::kIsDebugBuild && (RUNNING_ON_VALGRIND == 0)) {
    exit(result);
  }
  return result;
}
