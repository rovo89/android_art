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

#include "offline_profiling_info.h"

#include <fstream>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "art_method-inl.h"
#include "base/mutex.h"
#include "jit/profiling_info.h"
#include "safe_map.h"
#include "utils.h"

namespace art {

// An arbitrary value to throttle save requests. Set to 500ms for now.
static constexpr const uint64_t kMilisecondsToNano = 1000000;
static constexpr const uint64_t kMinimumTimeBetweenSavesNs = 500 * kMilisecondsToNano;

bool OfflineProfilingInfo::NeedsSaving(uint64_t last_update_time_ns) const {
  return last_update_time_ns - last_update_time_ns_.LoadRelaxed() > kMinimumTimeBetweenSavesNs;
}

void OfflineProfilingInfo::SaveProfilingInfo(const std::string& filename,
                                             uint64_t last_update_time_ns,
                                             const std::set<ArtMethod*>& methods) {
  if (!NeedsSaving(last_update_time_ns)) {
    VLOG(profiler) << "No need to saved profile info to " << filename;
    return;
  }

  if (methods.empty()) {
    VLOG(profiler) << "No info to save to " << filename;
    return;
  }

  DexFileToMethodsMap info;
  {
    ScopedObjectAccess soa(Thread::Current());
    for (auto it = methods.begin(); it != methods.end(); it++) {
      AddMethodInfo(*it, &info);
    }
  }

  // This doesn't need locking because we are trying to lock the file for exclusive
  // access and fail immediately if we can't.
  if (Serialize(filename, info)) {
    last_update_time_ns_.StoreRelaxed(last_update_time_ns);
    VLOG(profiler) << "Successfully saved profile info to "
                   << filename << " with time stamp: " << last_update_time_ns;
  }
}


void OfflineProfilingInfo::AddMethodInfo(ArtMethod* method, DexFileToMethodsMap* info) {
  DCHECK(method != nullptr);
  const DexFile* dex_file = method->GetDexFile();

  auto info_it = info->find(dex_file);
  if (info_it == info->end()) {
    info_it = info->Put(dex_file, std::set<uint32_t>());
  }
  info_it->second.insert(method->GetDexMethodIndex());
}

static int OpenOrCreateFile(const std::string& filename) {
  // TODO(calin) allow the shared uid of the app to access the file.
  int fd = open(filename.c_str(),
                O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                S_IRUSR | S_IWUSR);
  if (fd < 0) {
    PLOG(WARNING) << "Failed to open profile file " << filename;
    return -1;
  }

  // Lock the file for exclusive access but don't wait if we can't lock it.
  int err = flock(fd, LOCK_EX | LOCK_NB);
  if (err < 0) {
    PLOG(WARNING) << "Failed to lock profile file " << filename;
    return -1;
  }

  return fd;
}

static bool CloseDescriptorForFile(int fd, const std::string& filename) {
  // Now unlock the file, allowing another process in.
  int err = flock(fd, LOCK_UN);
  if (err < 0) {
    PLOG(WARNING) << "Failed to unlock profile file " << filename;
    return false;
  }

  // Done, close the file.
  err = ::close(fd);
  if (err < 0) {
    PLOG(WARNING) << "Failed to close descriptor for profile file" << filename;
    return false;
  }

  return true;
}

static void WriteToFile(int fd, const std::ostringstream& os) {
  std::string data(os.str());
  const char *p = data.c_str();
  size_t length = data.length();
  do {
    int n = ::write(fd, p, length);
    p += n;
    length -= n;
  } while (length > 0);
}

static constexpr char kFieldSeparator = ',';
static constexpr char kLineSeparator = '\n';

/**
 * Serialization format:
 *    multidex_suffix1,dex_location_checksum1,method_id11,method_id12...
 *    multidex_suffix2,dex_location_checksum2,method_id21,method_id22...
 * e.g.
 *    ,131232145,11,23,454,54               -> this is the first dex file, it has no multidex suffix
 *    :classes5.dex,218490184,39,13,49,1    -> this is the fifth dex file.
 **/
bool OfflineProfilingInfo::Serialize(const std::string& filename,
                                     const DexFileToMethodsMap& info) const {
  int fd = OpenOrCreateFile(filename);
  if (fd == -1) {
    return false;
  }

  // TODO(calin): Merge with a previous existing profile.
  // TODO(calin): Profile this and see how much memory it takes. If too much,
  // write to file directly.
  std::ostringstream os;
  for (auto it : info) {
    const DexFile* dex_file = it.first;
    const std::set<uint32_t>& method_dex_ids = it.second;

    os << DexFile::GetMultiDexSuffix(dex_file->GetLocation())
        << kFieldSeparator
        << dex_file->GetLocationChecksum();
    for (auto method_it : method_dex_ids) {
      os << kFieldSeparator << method_it;
    }
    os << kLineSeparator;
  }

  WriteToFile(fd, os);

  return CloseDescriptorForFile(fd, filename);
}
}  // namespace art
