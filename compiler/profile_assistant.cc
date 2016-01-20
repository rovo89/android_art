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

#include "profile_assistant.h"

#include "base/unix_file/fd_file.h"
#include "os.h"

namespace art {

// Minimum number of new methods that profiles must contain to enable recompilation.
static constexpr const uint32_t kMinNewMethodsForCompilation = 10;

bool ProfileAssistant::ProcessProfilesInternal(
        const std::vector<ScopedFlock>& profile_files,
        const std::vector<ScopedFlock>& reference_profile_files,
        /*out*/ ProfileCompilationInfo** profile_compilation_info) {
  DCHECK(!profile_files.empty());
  DCHECK(!reference_profile_files.empty() ||
      (profile_files.size() == reference_profile_files.size()));

  std::vector<ProfileCompilationInfo> new_info(profile_files.size());
  bool should_compile = false;
  // Read the main profile files.
  for (size_t i = 0; i < new_info.size(); i++) {
    if (!new_info[i].Load(profile_files[i].GetFile()->Fd())) {
      LOG(WARNING) << "Could not load profile file at index " << i;
      return false;
    }
    // Do we have enough new profiled methods that will make the compilation worthwhile?
    should_compile |= (new_info[i].GetNumberOfMethods() > kMinNewMethodsForCompilation);
  }

  if (!should_compile) {
    return true;
  }

  std::unique_ptr<ProfileCompilationInfo> result(new ProfileCompilationInfo());
  // Merge information.
  for (size_t i = 0; i < new_info.size(); i++) {
    if (!reference_profile_files.empty()) {
      if (!new_info[i].Load(reference_profile_files[i].GetFile()->Fd())) {
        LOG(WARNING) << "Could not load reference profile file at index " << i;
        return false;
      }
    }
    // Merge all data into a single object.
    if (!result->Load(new_info[i])) {
      LOG(WARNING) << "Could not merge profile data at index " << i;
      return false;
    }
  }
  // We were successful in merging all profile information. Update the files.
  for (size_t i = 0; i < new_info.size(); i++) {
    if (!reference_profile_files.empty()) {
      if (!reference_profile_files[i].GetFile()->ClearContent()) {
        PLOG(WARNING) << "Could not clear reference profile file at index " << i;
        return false;
      }
      if (!new_info[i].Save(reference_profile_files[i].GetFile()->Fd())) {
        LOG(WARNING) << "Could not save reference profile file at index " << i;
        return false;
      }
      if (!profile_files[i].GetFile()->ClearContent()) {
        PLOG(WARNING) << "Could not clear profile file at index " << i;
        return false;
      }
    }
  }

  *profile_compilation_info = result.release();
  return true;
}

class ScopedCollectionFlock {
 public:
  explicit ScopedCollectionFlock(size_t size) : flocks_(size) {}

  // Will block until all the locks are acquired.
  bool Init(const std::vector<std::string>& filenames, /* out */ std::string* error) {
    for (size_t i = 0; i < filenames.size(); i++) {
      if (!flocks_[i].Init(filenames[i].c_str(), O_RDWR, /* block */ true, error)) {
        *error += " (index=" + std::to_string(i) + ")";
        return false;
      }
    }
    return true;
  }

  // Will block until all the locks are acquired.
  bool Init(const std::vector<uint32_t>& fds, /* out */ std::string* error) {
    for (size_t i = 0; i < fds.size(); i++) {
      // We do not own the descriptor, so disable auto-close and don't check usage.
      File file(fds[i], false);
      file.DisableAutoClose();
      if (!flocks_[i].Init(&file, error)) {
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

bool ProfileAssistant::ProcessProfiles(
        const std::vector<uint32_t>& profile_files_fd,
        const std::vector<uint32_t>& reference_profile_files_fd,
        /*out*/ ProfileCompilationInfo** profile_compilation_info) {
  *profile_compilation_info = nullptr;

  std::string error;
  ScopedCollectionFlock profile_files_flocks(profile_files_fd.size());
  if (!profile_files_flocks.Init(profile_files_fd, &error)) {
    LOG(WARNING) << "Could not lock profile files: " << error;
    return false;
  }
  ScopedCollectionFlock reference_profile_files_flocks(reference_profile_files_fd.size());
  if (!reference_profile_files_flocks.Init(reference_profile_files_fd, &error)) {
    LOG(WARNING) << "Could not lock reference profile files: " << error;
    return false;
  }

  return ProcessProfilesInternal(profile_files_flocks.Get(),
                                 reference_profile_files_flocks.Get(),
                                 profile_compilation_info);
}

bool ProfileAssistant::ProcessProfiles(
        const std::vector<std::string>& profile_files,
        const std::vector<std::string>& reference_profile_files,
        /*out*/ ProfileCompilationInfo** profile_compilation_info) {
  *profile_compilation_info = nullptr;

  std::string error;
  ScopedCollectionFlock profile_files_flocks(profile_files.size());
  if (!profile_files_flocks.Init(profile_files, &error)) {
    LOG(WARNING) << "Could not lock profile files: " << error;
    return false;
  }
  ScopedCollectionFlock reference_profile_files_flocks(reference_profile_files.size());
  if (!reference_profile_files_flocks.Init(reference_profile_files, &error)) {
    LOG(WARNING) << "Could not lock reference profile files: " << error;
    return false;
  }

  return ProcessProfilesInternal(profile_files_flocks.Get(),
                                 reference_profile_files_flocks.Get(),
                                 profile_compilation_info);
}

}  // namespace art
