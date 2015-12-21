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

#ifndef ART_RUNTIME_JIT_OFFLINE_PROFILING_INFO_H_
#define ART_RUNTIME_JIT_OFFLINE_PROFILING_INFO_H_

#include <set>
#include <vector>

#include "atomic.h"
#include "dex_file.h"
#include "method_reference.h"
#include "safe_map.h"

namespace art {

class ArtMethod;

// TODO: rename file.
/**
 * Profile information in a format suitable to be queried by the compiler and
 * performing profile guided compilation.
 * It is a serialize-friendly format based on information collected by the
 * interpreter (ProfileInfo).
 * Currently it stores only the hot compiled methods.
 */
class ProfileCompilationInfo {
 public:
  static bool SaveProfilingInfo(const std::string& filename,
                                const std::vector<ArtMethod*>& methods);

  // Loads profile information from the given file.
  bool Load(const std::string& profile_filename);
  // Loads the data from another ProfileCompilationInfo object.
  bool Load(const ProfileCompilationInfo& info);
  // Saves the profile data to the given file.
  bool Save(const std::string& profile_filename);
  // Returns the number of methods that were profiled.
  uint32_t GetNumberOfMethods() const;

  // Returns true if the method reference is present in the profiling info.
  bool ContainsMethod(const MethodReference& method_ref) const;

  // Dumps all the loaded profile info into a string and returns it.
  // If dex_files is not null then the method indices will be resolved to their
  // names.
  // This is intended for testing and debugging.
  std::string DumpInfo(const std::vector<const DexFile*>* dex_files,
                       bool print_full_dex_location = true) const;

 private:
  bool AddData(const std::string& dex_location, uint32_t checksum, uint16_t method_idx);
  bool ProcessLine(const std::string& line);

  struct DexFileData {
    explicit DexFileData(uint32_t location_checksum) : checksum(location_checksum) {}
    uint32_t checksum;
    std::set<uint16_t> method_set;
  };

  using DexFileToProfileInfoMap = SafeMap<const std::string, DexFileData>;

  DexFileToProfileInfoMap info_;
};

}  // namespace art

#endif  // ART_RUNTIME_JIT_OFFLINE_PROFILING_INFO_H_
