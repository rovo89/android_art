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

#include "errno.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>

#include "base/dumpable.h"
#include "base/scoped_flock.h"
#include "base/stringpiece.h"
#include "base/stringprintf.h"
#include "base/time_utils.h"
#include "base/unix_file/fd_file.h"
#include "dex_file.h"
#include "jit/offline_profiling_info.h"
#include "utils.h"
#include "zip_archive.h"
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

static constexpr int kInvalidFd = -1;

static bool FdIsValid(int fd) {
  return fd != kInvalidFd;
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
  UsageError("  --dump-only: dumps the content of the specified profile files");
  UsageError("      to standard output (default) in a human readable form.");
  UsageError("");
  UsageError("  --dump-output-to-fd=<number>: redirects --dump-info-for output to a file");
  UsageError("      descriptor.");
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
  UsageError("  --dex-location=<string>: location string to use with corresponding");
  UsageError("      apk-fd to find dex files");
  UsageError("");
  UsageError("  --apk-fd=<number>: file descriptor containing an open APK to");
  UsageError("      search for dex files");
  UsageError("");

  exit(EXIT_FAILURE);
}

class ProfMan FINAL {
 public:
  ProfMan() :
      reference_profile_file_fd_(kInvalidFd),
      dump_only_(false),
      dump_output_to_fd_(kInvalidFd),
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
        LOG(INFO) << "profman: option[" << i << "]=" << argv[i];
      }
      if (option == "--dump-only") {
        dump_only_ = true;
      } else if (option.starts_with("--dump-output-to-fd=")) {
        ParseUintOption(option, "--dump-output-to-fd", &dump_output_to_fd_, Usage);
      } else if (option.starts_with("--profile-file=")) {
        profile_files_.push_back(option.substr(strlen("--profile-file=")).ToString());
      } else if (option.starts_with("--profile-file-fd=")) {
        ParseFdForCollection(option, "--profile-file-fd", &profile_files_fd_);
      } else if (option.starts_with("--reference-profile-file=")) {
        reference_profile_file_ = option.substr(strlen("--reference-profile-file=")).ToString();
      } else if (option.starts_with("--reference-profile-file-fd=")) {
        ParseUintOption(option, "--reference-profile-file-fd", &reference_profile_file_fd_, Usage);
      } else if (option.starts_with("--dex-location=")) {
        dex_locations_.push_back(option.substr(strlen("--dex-location=")).ToString());
      } else if (option.starts_with("--apk-fd=")) {
        ParseFdForCollection(option, "--apk-fd", &apks_fd_);
      } else {
        Usage("Unknown argument '%s'", option.data());
      }
    }

    bool has_profiles = !profile_files_.empty() || !profile_files_fd_.empty();
    bool has_reference_profile = !reference_profile_file_.empty() ||
        FdIsValid(reference_profile_file_fd_);

    // --dump-only may be specified with only --reference-profiles present.
    if (!dump_only_ && !has_profiles) {
      Usage("No profile files specified.");
    }
    if (!profile_files_.empty() && !profile_files_fd_.empty()) {
      Usage("Profile files should not be specified with both --profile-file-fd and --profile-file");
    }
    if (!dump_only_ && !has_reference_profile) {
      Usage("No reference profile file specified.");
    }
    if (!reference_profile_file_.empty() && FdIsValid(reference_profile_file_fd_)) {
      Usage("Reference profile should not be specified with both "
            "--reference-profile-file-fd and --reference-profile-file");
    }
    if ((!profile_files_.empty() && FdIsValid(reference_profile_file_fd_)) ||
        (!dump_only_ && !profile_files_fd_.empty() && !FdIsValid(reference_profile_file_fd_))) {
      Usage("Options --profile-file-fd and --reference-profile-file-fd "
            "should only be used together");
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

  int DumpOneProfile(const std::string& banner, const std::string& filename, int fd,
                     const std::vector<const DexFile*>* dex_files, std::string* dump) {
    if (!filename.empty()) {
      fd = open(filename.c_str(), O_RDWR);
      if (fd < 0) {
        std::cerr << "Cannot open " << filename << strerror(errno);
        return -1;
      }
    }
    ProfileCompilationInfo info;
    if (!info.Load(fd)) {
      std::cerr << "Cannot load profile info from fd=" << fd << "\n";
      return -1;
    }
    std::string this_dump = banner + "\n" + info.DumpInfo(dex_files) + "\n";
    *dump += this_dump;
    if (close(fd) < 0) {
      PLOG(WARNING) << "Failed to close descriptor";
    }
    return 0;
  }

  int DumpProfileInfo() {
    static const char* kEmptyString = "";
    static const char* kOrdinaryProfile = "=== profile ===";
    static const char* kReferenceProfile = "=== reference profile ===";

    // Open apk/zip files and and read dex files.
    MemMap::Init();  // for ZipArchive::OpenFromFd
    std::vector<const DexFile*> dex_files;
    assert(dex_locations_.size() == apks_fd_.size());
    for (size_t i = 0; i < dex_locations_.size(); ++i) {
      std::string error_msg;
      std::vector<std::unique_ptr<const DexFile>> dex_files_for_location;
      std::unique_ptr<ZipArchive> zip_archive(ZipArchive::OpenFromFd(apks_fd_[i],
                                                                     dex_locations_[i].c_str(),
                                                                     &error_msg));
      if (zip_archive == nullptr) {
        LOG(WARNING) << "OpenFromFd failed for '" << dex_locations_[i] << "' " << error_msg;
        continue;
      }
      if (DexFile::OpenFromZip(*zip_archive,
                               dex_locations_[i],
                               &error_msg,
                               &dex_files_for_location)) {
      } else {
        LOG(WARNING) << "OpenFromZip failed for '" << dex_locations_[i] << "' " << error_msg;
        continue;
      }
      for (std::unique_ptr<const DexFile>& dex_file : dex_files_for_location) {
        dex_files.push_back(dex_file.release());
      }
    }

    std::string dump;
    // Dump individual profile files.
    if (!profile_files_fd_.empty()) {
      for (int profile_file_fd : profile_files_fd_) {
        int ret = DumpOneProfile(kOrdinaryProfile,
                                 kEmptyString,
                                 profile_file_fd,
                                 &dex_files,
                                 &dump);
        if (ret != 0) {
          return ret;
        }
      }
    }
    if (!profile_files_.empty()) {
      for (const std::string& profile_file : profile_files_) {
        int ret = DumpOneProfile(kOrdinaryProfile, profile_file, kInvalidFd, &dex_files, &dump);
        if (ret != 0) {
          return ret;
        }
      }
    }
    // Dump reference profile file.
    if (FdIsValid(reference_profile_file_fd_)) {
      int ret = DumpOneProfile(kReferenceProfile,
                               kEmptyString,
                               reference_profile_file_fd_,
                               &dex_files,
                               &dump);
      if (ret != 0) {
        return ret;
      }
    }
    if (!reference_profile_file_.empty()) {
      int ret = DumpOneProfile(kReferenceProfile,
                               reference_profile_file_,
                               kInvalidFd,
                               &dex_files,
                               &dump);
      if (ret != 0) {
        return ret;
      }
    }
    if (!FdIsValid(dump_output_to_fd_)) {
      std::cout << dump;
    } else {
      unix_file::FdFile out_fd(dump_output_to_fd_, false /*check_usage*/);
      if (!out_fd.WriteFully(dump.c_str(), dump.length())) {
        return -1;
      }
    }
    return 0;
  }

  bool ShouldOnlyDumpProfile() {
    return dump_only_;
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
    static constexpr uint64_t kLogThresholdTime = MsToNs(100);  // 100ms
    uint64_t time_taken = NanoTime() - start_ns_;
    if (time_taken > kLogThresholdTime) {
      LOG(WARNING) << "profman took " << PrettyDuration(time_taken);
    }
  }

  std::vector<std::string> profile_files_;
  std::vector<int> profile_files_fd_;
  std::vector<std::string> dex_locations_;
  std::vector<int> apks_fd_;
  std::string reference_profile_file_;
  int reference_profile_file_fd_;
  bool dump_only_;
  int dump_output_to_fd_;
  uint64_t start_ns_;
};

// See ProfileAssistant::ProcessingResult for return codes.
static int profman(int argc, char** argv) {
  ProfMan profman;

  // Parse arguments. Argument mistakes will lead to exit(EXIT_FAILURE) in UsageError.
  profman.ParseArgs(argc, argv);

  if (profman.ShouldOnlyDumpProfile()) {
    return profman.DumpProfileInfo();
  }
  // Process profile information and assess if we need to do a profile guided compilation.
  // This operation involves I/O.
  return profman.ProcessProfiles();
}

}  // namespace art

int main(int argc, char **argv) {
  return art::profman(argc, argv);
}

