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

namespace art {

// Minimum number of new methods that profiles must contain to enable recompilation.
static constexpr const uint32_t kMinNewMethodsForCompilation = 10;

bool ProfileAssistant::ProcessProfiles(
      const std::vector<std::string>& profile_files,
      const std::vector<std::string>& reference_profile_files,
      /*out*/ ProfileCompilationInfo** profile_compilation_info) {
  DCHECK(!profile_files.empty());
  DCHECK(reference_profile_files.empty() ||
      (profile_files.size() == reference_profile_files.size()));

  std::vector<ProfileCompilationInfo> new_info(profile_files.size());
  bool should_compile = false;
  // Read the main profile files.
  for (size_t i = 0; i < profile_files.size(); i++) {
    if (!new_info[i].Load(profile_files[i])) {
      LOG(WARNING) << "Could not load profile file: " << profile_files[i];
      return false;
    }
    // Do we have enough new profiled methods that will make the compilation worthwhile?
    should_compile |= (new_info[i].GetNumberOfMethods() > kMinNewMethodsForCompilation);
  }
  if (!should_compile) {
    *profile_compilation_info = nullptr;
    return true;
  }

  std::unique_ptr<ProfileCompilationInfo> result(new ProfileCompilationInfo());
  for (size_t i = 0; i < new_info.size(); i++) {
    // Merge all data into a single object.
    result->Load(new_info[i]);
    // If we have any reference profile information merge their information with
    // the current profiles and save them back to disk.
    if (!reference_profile_files.empty()) {
      if (!new_info[i].Load(reference_profile_files[i])) {
        LOG(WARNING) << "Could not load reference profile file: " << reference_profile_files[i];
        return false;
      }
      if (!new_info[i].Save(reference_profile_files[i])) {
        LOG(WARNING) << "Could not save reference profile file: " << reference_profile_files[i];
        return false;
      }
    }
  }
  *profile_compilation_info = result.release();
  return true;
}

}  // namespace art
