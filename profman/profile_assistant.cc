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

#include "profile_assistant.h"

#include "base/unix_file/fd_file.h"
#include "os.h"

namespace art {

// Minimum number of new methods/classes that profiles
// must contain to enable recompilation.
static constexpr const uint32_t kMinNewMethodsForCompilation = 10;
static constexpr const uint32_t kMinNewClassesForCompilation = 10;

ProfileAssistant::ProcessingResult ProfileAssistant::ProcessProfilesInternal(
        const std::vector<ScopedFlock>& profile_files,
        const ScopedFlock& reference_profile_file) {
  DCHECK(!profile_files.empty());

  ProfileCompilationInfo info;
  // Load the reference profile.
  if (!info.Load(reference_profile_file.GetFile()->Fd())) {
    LOG(WARNING) << "Could not load reference profile file";
    return kErrorBadProfiles;
  }

  // Store the current state of the reference profile before merging with the current profiles.
  uint32_t number_of_methods = info.GetNumberOfMethods();
  uint32_t number_of_classes = info.GetNumberOfResolvedClasses();

  // Merge all current profiles.
  for (size_t i = 0; i < profile_files.size(); i++) {
    if (!info.Load(profile_files[i].GetFile()->Fd())) {
      LOG(WARNING) << "Could not load profile file at index " << i;
      return kErrorBadProfiles;
    }
  }

  // Check if there is enough new information added by the current profiles.
  if (((info.GetNumberOfMethods() - number_of_methods) < kMinNewMethodsForCompilation) &&
      ((info.GetNumberOfResolvedClasses() - number_of_classes) < kMinNewClassesForCompilation)) {
    return kSkipCompilation;
  }

  // We were successful in merging all profile information. Update the reference profile.
  if (!reference_profile_file.GetFile()->ClearContent()) {
    PLOG(WARNING) << "Could not clear reference profile file";
    return kErrorIO;
  }
  if (!info.Save(reference_profile_file.GetFile()->Fd())) {
    LOG(WARNING) << "Could not save reference profile file";
    return kErrorIO;
  }

  return kCompile;
}

static bool InitFlock(const std::string& filename, ScopedFlock& flock, std::string* error) {
  return flock.Init(filename.c_str(), O_RDWR, /* block */ true, error);
}

static bool InitFlock(int fd, ScopedFlock& flock, std::string* error) {
  DCHECK_GE(fd, 0);
  // We do not own the descriptor, so disable auto-close and don't check usage.
  File file(fd, false);
  file.DisableAutoClose();
  return flock.Init(&file, error);
}

class ScopedCollectionFlock {
 public:
  explicit ScopedCollectionFlock(size_t size) : flocks_(size) {}

  // Will block until all the locks are acquired.
  bool Init(const std::vector<std::string>& filenames, /* out */ std::string* error) {
    for (size_t i = 0; i < filenames.size(); i++) {
      if (!InitFlock(filenames[i], flocks_[i], error)) {
        *error += " (index=" + std::to_string(i) + ")";
        return false;
      }
    }
    return true;
  }

  // Will block until all the locks are acquired.
  bool Init(const std::vector<int>& fds, /* out */ std::string* error) {
    for (size_t i = 0; i < fds.size(); i++) {
      DCHECK_GE(fds[i], 0);
      if (!InitFlock(fds[i], flocks_[i], error)) {
        *error += " (index=" + std::to_string(i) + ")";
        return false;
      }
    }
    return true;
  }

  const std::vector<ScopedFlock>& Get() const { return flocks_; }

 private:
  std::vector<ScopedFlock> flocks_;
};

ProfileAssistant::ProcessingResult ProfileAssistant::ProcessProfiles(
        const std::vector<int>& profile_files_fd,
        int reference_profile_file_fd) {
  DCHECK_GE(reference_profile_file_fd, 0);
  std::string error;
  ScopedCollectionFlock profile_files_flocks(profile_files_fd.size());
  if (!profile_files_flocks.Init(profile_files_fd, &error)) {
    LOG(WARNING) << "Could not lock profile files: " << error;
    return kErrorCannotLock;
  }
  ScopedFlock reference_profile_file_flock;
  if (!InitFlock(reference_profile_file_fd, reference_profile_file_flock, &error)) {
    LOG(WARNING) << "Could not lock reference profiled files: " << error;
    return kErrorCannotLock;
  }

  return ProcessProfilesInternal(profile_files_flocks.Get(),
                                 reference_profile_file_flock);
}

ProfileAssistant::ProcessingResult ProfileAssistant::ProcessProfiles(
        const std::vector<std::string>& profile_files,
        const std::string& reference_profile_file) {
  std::string error;
  ScopedCollectionFlock profile_files_flocks(profile_files.size());
  if (!profile_files_flocks.Init(profile_files, &error)) {
    LOG(WARNING) << "Could not lock profile files: " << error;
    return kErrorCannotLock;
  }
  ScopedFlock reference_profile_file_flock;
  if (!InitFlock(reference_profile_file, reference_profile_file_flock, &error)) {
    LOG(WARNING) << "Could not lock reference profile files: " << error;
    return kErrorCannotLock;
  }

  return ProcessProfilesInternal(profile_files_flocks.Get(),
                                 reference_profile_file_flock);
}

}  // namespace art
