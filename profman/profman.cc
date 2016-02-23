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

#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "base/dumpable.h"
#include "base/scoped_flock.h"
#include "base/stringpiece.h"
#include "base/stringprintf.h"
#include "base/time_utils.h"
#include "base/unix_file/fd_file.h"
#include "jit/offline_profiling_info.h"
#include "utils.h"
#include "profile_assistant.h"

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

NO_RETURN static void Usage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);

  UsageError("Command: %s", CommandLine().c_str());
  UsageError("Usage: profman [options]...");
  UsageError("");
  UsageError("  --profile-file=<filename>: specify profiler output file to use for compilation.");
  UsageError("      Can be specified multiple time, in which case the data from the different");
  UsageError("      profiles will be aggregated.");
  UsageError("");
  UsageError("  --profile-file-fd=<number>: same as --profile-file but accepts a file descriptor.");
  UsageError("      Cannot be used together with --profile-file.");
  UsageError("");
  UsageError("  --reference-profile-file=<filename>: specify a reference profile.");
  UsageError("      The data in this file will be compared with the data obtained by merging");
  UsageError("      all the files specified with --profile-file or --profile-file-fd.");
  UsageError("      If the exit code is EXIT_COMPILE then all --profile-file will be merged into");
  UsageError("      --reference-profile-file. ");
  UsageError("");
  UsageError("  --reference-profile-file-fd=<number>: same as --reference-profile-file but");
  UsageError("      accepts a file descriptor. Cannot be used together with");
  UsageError("      --reference-profile-file.");
  UsageError("");

  exit(EXIT_FAILURE);
}

class ProfMan FINAL {
 public:
  ProfMan() :
      reference_profile_file_fd_(-1),
      start_ns_(NanoTime()) {}

  ~ProfMan() {
    LogCompletionTime();
  }

  void ParseArgs(int argc, char **argv) {
    original_argc = argc;
    original_argv = argv;

    InitLogging(argv);

    // Skip over the command name.
    argv++;
    argc--;

    if (argc == 0) {
      Usage("No arguments specified");
    }

    for (int i = 0; i < argc; ++i) {
      const StringPiece option(argv[i]);
      const bool log_options = false;
      if (log_options) {
        LOG(INFO) << "patchoat: option[" << i << "]=" << argv[i];
      }
      if (option.starts_with("--profile-file=")) {
        profile_files_.push_back(option.substr(strlen("--profile-file=")).ToString());
      } else if (option.starts_with("--profile-file-fd=")) {
        ParseFdForCollection(option, "--profile-file-fd", &profile_files_fd_);
      } else if (option.starts_with("--reference-profile-file=")) {
        reference_profile_file_ = option.substr(strlen("--reference-profile-file=")).ToString();
      } else if (option.starts_with("--reference-profile-file-fd=")) {
        ParseUintOption(option, "--reference-profile-file-fd", &reference_profile_file_fd_, Usage);
      } else {
        Usage("Unknown argument %s", option.data());
      }
    }

    if (profile_files_.empty() && profile_files_fd_.empty()) {
      Usage("No profile files specified.");
    }
    if (!profile_files_.empty() && !profile_files_fd_.empty()) {
      Usage("Profile files should not be specified with both --profile-file-fd and --profile-file");
    }
    if (!reference_profile_file_.empty() && (reference_profile_file_fd_ != -1)) {
      Usage("--reference-profile-file-fd should only be supplied with --profile-file-fd");
    }
    if (reference_profile_file_.empty() && (reference_profile_file_fd_ == -1)) {
      Usage("Reference profile file not specified");
    }
  }

  ProfileAssistant::ProcessingResult ProcessProfiles() {
    ProfileAssistant::ProcessingResult result;
    if (profile_files_.empty()) {
      // The file doesn't need to be flushed here (ProcessProfiles will do it)
      // so don't check the usage.
      File file(reference_profile_file_fd_, false);
      result = ProfileAssistant::ProcessProfiles(profile_files_fd_, reference_profile_file_fd_);
      CloseAllFds(profile_files_fd_, "profile_files_fd_");
    } else {
      result = ProfileAssistant::ProcessProfiles(profile_files_, reference_profile_file_);
    }
    return result;
  }

 private:
  static void ParseFdForCollection(const StringPiece& option,
                                   const char* arg_name,
                                   std::vector<int>* fds) {
    int fd;
    ParseUintOption(option, arg_name, &fd, Usage);
    fds->push_back(fd);
  }

  static void CloseAllFds(const std::vector<int>& fds, const char* descriptor) {
    for (size_t i = 0; i < fds.size(); i++) {
      if (close(fds[i]) < 0) {
        PLOG(WARNING) << "Failed to close descriptor for " << descriptor << " at index " << i;
      }
    }
  }

  void LogCompletionTime() {
    LOG(INFO) << "profman took " << PrettyDuration(NanoTime() - start_ns_);
  }

  std::vector<std::string> profile_files_;
  std::vector<int> profile_files_fd_;
  std::string reference_profile_file_;
  int reference_profile_file_fd_;
  uint64_t start_ns_;
};

// See ProfileAssistant::ProcessingResult for return codes.
static int profman(int argc, char** argv) {
  ProfMan profman;

  // Parse arguments. Argument mistakes will lead to exit(EXIT_FAILURE) in UsageError.
  profman.ParseArgs(argc, argv);

  // Process profile information and assess if we need to do a profile guided compilation.
  // This operation involves I/O.
  return profman.ProcessProfiles();
}

}  // namespace art

int main(int argc, char **argv) {
  return art::profman(argc, argv);
}

