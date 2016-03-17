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
#include <vector>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "art_method-inl.h"
#include "base/mutex.h"
#include "base/scoped_flock.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/unix_file/fd_file.h"
#include "jit/profiling_info.h"
#include "os.h"
#include "safe_map.h"

namespace art {

// Transform the actual dex location into relative paths.
// Note: this is OK because we don't store profiles of different apps into the same file.
// Apps with split apks don't cause trouble because each split has a different name and will not
// collide with other entries.
std::string ProfileCompilationInfo::GetProfileDexFileKey(const std::string& dex_location) {
  DCHECK(!dex_location.empty());
  size_t last_sep_index = dex_location.find_last_of('/');
  if (last_sep_index == std::string::npos) {
    return dex_location;
  } else {
    DCHECK(last_sep_index < dex_location.size());
    return dex_location.substr(last_sep_index + 1);
  }
}

bool ProfileCompilationInfo::SaveProfilingInfo(
    const std::string& filename,
    const std::vector<ArtMethod*>& methods,
    const std::set<DexCacheResolvedClasses>& resolved_classes,
    uint64_t* bytes_written) {
  if (methods.empty() && resolved_classes.empty()) {
    VLOG(profiler) << "No info to save to " << filename;
    if (bytes_written != nullptr) {
      *bytes_written = 0;
    }
    return true;
  }

  ScopedTrace trace(__PRETTY_FUNCTION__);
  ScopedFlock flock;
  std::string error;
  if (!flock.Init(filename.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC, /* block */ false, &error)) {
    LOG(WARNING) << "Couldn't lock the profile file " << filename << ": " << error;
    return false;
  }

  int fd = flock.GetFile()->Fd();

  ProfileCompilationInfo info;
  if (!info.Load(fd)) {
    LOG(WARNING) << "Could not load previous profile data from file " << filename;
    return false;
  }
  {
    ScopedObjectAccess soa(Thread::Current());
    for (ArtMethod* method : methods) {
      const DexFile* dex_file = method->GetDexFile();
      if (!info.AddMethodIndex(GetProfileDexFileKey(dex_file->GetLocation()),
                               dex_file->GetLocationChecksum(),
                               method->GetDexMethodIndex())) {
        return false;
      }
    }
    for (const DexCacheResolvedClasses& dex_cache : resolved_classes) {
      info.AddResolvedClasses(dex_cache);
    }
  }

  if (!flock.GetFile()->ClearContent()) {
    PLOG(WARNING) << "Could not clear profile file: " << filename;
    return false;
  }

  // This doesn't need locking because we are trying to lock the file for exclusive
  // access and fail immediately if we can't.
  bool result = info.Save(fd);
  if (result) {
    VLOG(profiler) << "Successfully saved profile info to " << filename
        << " Size: " << GetFileSizeBytes(filename);
    if (bytes_written != nullptr) {
      *bytes_written = GetFileSizeBytes(filename);
    }
  } else {
    VLOG(profiler) << "Failed to save profile info to " << filename;
  }
  return result;
}

static bool WriteToFile(int fd, const std::ostringstream& os) {
  std::string data(os.str());
  const char *p = data.c_str();
  size_t length = data.length();
  do {
    int n = TEMP_FAILURE_RETRY(write(fd, p, length));
    if (n < 0) {
      PLOG(WARNING) << "Failed to write to descriptor: " << fd;
      return false;
    }
    p += n;
    length -= n;
  } while (length > 0);
  return true;
}

static constexpr const char kFieldSeparator = ',';
static constexpr const char kLineSeparator = '\n';
static constexpr const char* kClassesMarker = "classes";

/**
 * Serialization format:
 *    dex_location1,dex_location_checksum1,method_id11,method_id12...,classes,class_id1,class_id2...
 *    dex_location2,dex_location_checksum2,method_id21,method_id22...,classes,class_id1,class_id2...
 * e.g.
 *    app.apk,131232145,11,23,454,54,classes,1,2,4,1234
 *    app.apk:classes5.dex,218490184,39,13,49,1
 **/
bool ProfileCompilationInfo::Save(int fd) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  DCHECK_GE(fd, 0);
  // TODO(calin): Profile this and see how much memory it takes. If too much,
  // write to file directly.
  std::ostringstream os;
  for (const auto& it : info_) {
    const std::string& dex_location = it.first;
    const DexFileData& dex_data = it.second;
    if (dex_data.method_set.empty() && dex_data.class_set.empty()) {
      continue;
    }

    os << dex_location << kFieldSeparator << dex_data.checksum;
    for (auto method_it : dex_data.method_set) {
      os << kFieldSeparator << method_it;
    }
    if (!dex_data.class_set.empty()) {
      os << kFieldSeparator << kClassesMarker;
      for (auto class_id : dex_data.class_set) {
        os << kFieldSeparator << class_id;
      }
    }
    os << kLineSeparator;
  }

  return WriteToFile(fd, os);
}

// TODO(calin): This a duplicate of Utils::Split fixing the case where the first character
// is the separator. Merge the fix into Utils::Split once verified that it doesn't break its users.
static void SplitString(const std::string& s, char separator, std::vector<std::string>* result) {
  const char* p = s.data();
  const char* end = p + s.size();
  // Check if the first character is the separator.
  if (p != end && *p ==separator) {
    result->push_back("");
    ++p;
  }
  // Process the rest of the characters.
  while (p != end) {
    if (*p == separator) {
      ++p;
    } else {
      const char* start = p;
      while (++p != end && *p != separator) {
        // Skip to the next occurrence of the separator.
      }
      result->push_back(std::string(start, p - start));
    }
  }
}

ProfileCompilationInfo::DexFileData* ProfileCompilationInfo::GetOrAddDexFileData(
    const std::string& dex_location,
    uint32_t checksum) {
  auto info_it = info_.find(dex_location);
  if (info_it == info_.end()) {
    info_it = info_.Put(dex_location, DexFileData(checksum));
  }
  if (info_it->second.checksum != checksum) {
    LOG(WARNING) << "Checksum mismatch for dex " << dex_location;
    return nullptr;
  }
  return &info_it->second;
}

bool ProfileCompilationInfo::AddResolvedClasses(const DexCacheResolvedClasses& classes) {
  const std::string dex_location = GetProfileDexFileKey(classes.GetDexLocation());
  const uint32_t checksum = classes.GetLocationChecksum();
  DexFileData* const data = GetOrAddDexFileData(dex_location, checksum);
  if (data == nullptr) {
    return false;
  }
  data->class_set.insert(classes.GetClasses().begin(), classes.GetClasses().end());
  return true;
}

bool ProfileCompilationInfo::AddMethodIndex(const std::string& dex_location,
                                            uint32_t checksum,
                                            uint16_t method_idx) {
  DexFileData* const data = GetOrAddDexFileData(dex_location, checksum);
  if (data == nullptr) {
    return false;
  }
  data->method_set.insert(method_idx);
  return true;
}

bool ProfileCompilationInfo::AddClassIndex(const std::string& dex_location,
                                           uint32_t checksum,
                                           uint16_t class_idx) {
  DexFileData* const data = GetOrAddDexFileData(dex_location, checksum);
  if (data == nullptr) {
    return false;
  }
  data->class_set.insert(class_idx);
  return true;
}

bool ProfileCompilationInfo::ProcessLine(const std::string& line) {
  std::vector<std::string> parts;
  SplitString(line, kFieldSeparator, &parts);
  if (parts.size() < 3) {
    LOG(WARNING) << "Invalid line: " << line;
    return false;
  }

  const std::string& dex_location = parts[0];
  uint32_t checksum;
  if (!ParseInt(parts[1].c_str(), &checksum)) {
    return false;
  }

  for (size_t i = 2; i < parts.size(); i++) {
    if (parts[i] == kClassesMarker) {
      ++i;
      // All of the remaining idx are class def indexes.
      for (++i; i < parts.size(); ++i) {
        uint32_t class_def_idx;
        if (!ParseInt(parts[i].c_str(), &class_def_idx)) {
          LOG(WARNING) << "Cannot parse class_def_idx " << parts[i];
          return false;
        } else if (class_def_idx >= std::numeric_limits<uint16_t>::max()) {
          LOG(WARNING) << "Class def idx " << class_def_idx << " is larger than uint16_t max";
          return false;
        }
        if (!AddClassIndex(dex_location, checksum, class_def_idx)) {
          return false;
        }
      }
      break;
    }
    uint32_t method_idx;
    if (!ParseInt(parts[i].c_str(), &method_idx)) {
      LOG(WARNING) << "Cannot parse method_idx " << parts[i];
      return false;
    }
    if (!AddMethodIndex(dex_location, checksum, method_idx)) {
      return false;
    }
  }
  return true;
}

// Parses the buffer (of length n) starting from start_from and identify new lines
// based on kLineSeparator marker.
// Returns the first position after kLineSeparator in the buffer (starting from start_from),
// or -1 if the marker doesn't appear.
// The processed characters are appended to the given line.
static int GetLineFromBuffer(char* buffer, int n, int start_from, std::string& line) {
  if (start_from >= n) {
    return -1;
  }
  int new_line_pos = -1;
  for (int i = start_from; i < n; i++) {
    if (buffer[i] == kLineSeparator) {
      new_line_pos = i;
      break;
    }
  }
  int append_limit = new_line_pos == -1 ? n : new_line_pos;
  line.append(buffer + start_from, append_limit - start_from);
  // Jump over kLineSeparator and return the position of the next character.
  return new_line_pos == -1 ? new_line_pos : new_line_pos + 1;
}

bool ProfileCompilationInfo::Load(int fd) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  DCHECK_GE(fd, 0);

  std::string current_line;
  const int kBufferSize = 1024;
  char buffer[kBufferSize];

  while (true) {
    int n = TEMP_FAILURE_RETRY(read(fd, buffer, kBufferSize));
    if (n < 0) {
      PLOG(WARNING) << "Error when reading profile file";
      return false;
    } else if (n == 0) {
      break;
    }
    // Detect the new lines from the buffer. If we manage to complete a line,
    // process it. Otherwise append to the current line.
    int current_start_pos = 0;
    while (current_start_pos < n) {
      current_start_pos = GetLineFromBuffer(buffer, n, current_start_pos, current_line);
      if (current_start_pos == -1) {
        break;
      }
      if (!ProcessLine(current_line)) {
        return false;
      }
      // Reset the current line (we just processed it).
      current_line.clear();
    }
  }
  return true;
}

bool ProfileCompilationInfo::Load(const ProfileCompilationInfo& other) {
  for (const auto& other_it : other.info_) {
    const std::string& other_dex_location = other_it.first;
    const DexFileData& other_dex_data = other_it.second;

    auto info_it = info_.find(other_dex_location);
    if (info_it == info_.end()) {
      info_it = info_.Put(other_dex_location, DexFileData(other_dex_data.checksum));
    }
    if (info_it->second.checksum != other_dex_data.checksum) {
      LOG(WARNING) << "Checksum mismatch for dex " << other_dex_location;
      return false;
    }
    info_it->second.method_set.insert(other_dex_data.method_set.begin(),
                                      other_dex_data.method_set.end());
    info_it->second.class_set.insert(other_dex_data.class_set.begin(),
                                     other_dex_data.class_set.end());
  }
  return true;
}

bool ProfileCompilationInfo::ContainsMethod(const MethodReference& method_ref) const {
  auto info_it = info_.find(GetProfileDexFileKey(method_ref.dex_file->GetLocation()));
  if (info_it != info_.end()) {
    if (method_ref.dex_file->GetLocationChecksum() != info_it->second.checksum) {
      return false;
    }
    const std::set<uint16_t>& methods = info_it->second.method_set;
    return methods.find(method_ref.dex_method_index) != methods.end();
  }
  return false;
}

uint32_t ProfileCompilationInfo::GetNumberOfMethods() const {
  uint32_t total = 0;
  for (const auto& it : info_) {
    total += it.second.method_set.size();
  }
  return total;
}

std::string ProfileCompilationInfo::DumpInfo(const std::vector<const DexFile*>* dex_files,
                                             bool print_full_dex_location) const {
  std::ostringstream os;
  if (info_.empty()) {
    return "ProfileInfo: empty";
  }

  os << "ProfileInfo:";

  const std::string kFirstDexFileKeySubstitute = ":classes.dex";
  for (const auto& it : info_) {
    os << "\n";
    const std::string& location = it.first;
    const DexFileData& dex_data = it.second;
    if (print_full_dex_location) {
      os << location;
    } else {
      // Replace the (empty) multidex suffix of the first key with a substitute for easier reading.
      std::string multidex_suffix = DexFile::GetMultiDexSuffix(location);
      os << (multidex_suffix.empty() ? kFirstDexFileKeySubstitute : multidex_suffix);
    }
    for (const auto method_it : dex_data.method_set) {
      if (dex_files != nullptr) {
        const DexFile* dex_file = nullptr;
        for (size_t i = 0; i < dex_files->size(); i++) {
          if (location == (*dex_files)[i]->GetLocation()) {
            dex_file = (*dex_files)[i];
          }
        }
        if (dex_file != nullptr) {
          os << "\n  " << PrettyMethod(method_it, *dex_file, true);
        }
      }
      os << "\n  " << method_it;
    }
  }
  return os.str();
}

bool ProfileCompilationInfo::Equals(const ProfileCompilationInfo& other) {
  return info_.Equals(other.info_);
}

std::set<DexCacheResolvedClasses> ProfileCompilationInfo::GetResolvedClasses() const {
  std::set<DexCacheResolvedClasses> ret;
  for (auto&& pair : info_) {
    const std::string& profile_key = pair.first;
    const DexFileData& data = pair.second;
    DexCacheResolvedClasses classes(profile_key, data.checksum);
    classes.AddClasses(data.class_set.begin(), data.class_set.end());
    ret.insert(classes);
  }
  return ret;
}

}  // namespace art
