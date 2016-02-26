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
#include "dex_cache_resolved_classes.h"
#include "dex_file.h"
#include "method_reference.h"
#include "safe_map.h"

namespace art {

class ArtMethod;
class DexCacheProfileData;

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
  // Saves profile information about the given methods in the given file.
  // Note that the saving proceeds only if the file can be locked for exclusive access.
  // If not (the locking is not blocking), the function does not save and returns false.
  static bool SaveProfilingInfo(const std::string& filename,
                                const std::vector<ArtMethod*>& methods,
                                const std::set<DexCacheResolvedClasses>& resolved_classes);

  // Loads profile information from the given file descriptor.
  bool Load(int fd);
  // Loads the data from another ProfileCompilationInfo object.
  bool Load(const ProfileCompilationInfo& info);
  // Saves the profile data to the given file descriptor.
  bool Save(int fd);
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

  // For testing purposes.
  bool Equals(const ProfileCompilationInfo& other);
  static std::string GetProfileDexFileKey(const std::string& dex_location);

  // Returns the class descriptors for all of the classes in the profiles' class sets.
  // Note the dex location is actually the profile key, the caller needs to call back in to the
  // profile info stuff to generate a map back to the dex location.
  std::set<DexCacheResolvedClasses> GetResolvedClasses() const;

 private:
  struct DexFileData {
    explicit DexFileData(uint32_t location_checksum) : checksum(location_checksum) {}
    uint32_t checksum;
    std::set<uint16_t> method_set;
    std::set<uint16_t> class_set;

    bool operator==(const DexFileData& other) const {
      return checksum == other.checksum && method_set == other.method_set;
    }
  };

  using DexFileToProfileInfoMap = SafeMap<const std::string, DexFileData>;

  DexFileData* GetOrAddDexFileData(const std::string& dex_location, uint32_t checksum);
  bool AddMethodIndex(const std::string& dex_location, uint32_t checksum, uint16_t method_idx);
  bool AddClassIndex(const std::string& dex_location, uint32_t checksum, uint16_t class_idx);
  bool AddResolvedClasses(const DexCacheResolvedClasses& classes)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool ProcessLine(const std::string& line);

  friend class ProfileCompilationInfoTest;
  friend class CompilerDriverProfileTest;
  friend class ProfileAssistantTest;

  DexFileToProfileInfoMap info_;
};

}  // namespace art

#endif  // ART_RUNTIME_JIT_OFFLINE_PROFILING_INFO_H_
