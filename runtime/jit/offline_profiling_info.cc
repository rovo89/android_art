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
#include "base/stl_util.h"
#include "jit/profiling_info.h"
#include "safe_map.h"

namespace art {

bool ProfileCompilationInfo::SaveProfilingInfo(const std::string& filename,
                                               const std::vector<ArtMethod*>& methods) {
  if (methods.empty()) {
    VLOG(profiler) << "No info to save to " << filename;
    return true;
  }

  ProfileCompilationInfo info;
  if (!info.Load(filename)) {
    LOG(WARNING) << "Could not load previous profile data from file " << filename;
    return false;
  }
  {
    ScopedObjectAccess soa(Thread::Current());
    for (auto it = methods.begin(); it != methods.end(); it++) {
      const DexFile* dex_file = (*it)->GetDexFile();
      if (!info.AddData(dex_file->GetLocation(),
                        dex_file->GetLocationChecksum(),
                        (*it)->GetDexMethodIndex())) {
        return false;
      }
    }
  }

  // This doesn't need locking because we are trying to lock the file for exclusive
  // access and fail immediately if we can't.
  bool result = info.Save(filename);
  if (result) {
    VLOG(profiler) << "Successfully saved profile info to " << filename
        << " Size: " << GetFileSizeBytes(filename);
  } else {
    VLOG(profiler) << "Failed to save profile info to " << filename;
  }
  return result;
}

enum OpenMode {
  READ,
  READ_WRITE
};

static int OpenFile(const std::string& filename, OpenMode open_mode) {
  int fd = -1;
  switch (open_mode) {
    case READ:
      fd = open(filename.c_str(), O_RDONLY);
      break;
    case READ_WRITE:
      // TODO(calin) allow the shared uid of the app to access the file.
      fd = open(filename.c_str(), O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC);
      break;
  }

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

static constexpr const char kFieldSeparator = ',';
static constexpr const char kLineSeparator = '\n';

/**
 * Serialization format:
 *    dex_location1,dex_location_checksum1,method_id11,method_id12...
 *    dex_location2,dex_location_checksum2,method_id21,method_id22...
 * e.g.
 *    /system/priv-app/app/app.apk,131232145,11,23,454,54
 *    /system/priv-app/app/app.apk:classes5.dex,218490184,39,13,49,1
 **/
bool ProfileCompilationInfo::Save(const std::string& filename) {
  int fd = OpenFile(filename, READ_WRITE);
  if (fd == -1) {
    return false;
  }

  // TODO(calin): Merge with a previous existing profile.
  // TODO(calin): Profile this and see how much memory it takes. If too much,
  // write to file directly.
  std::ostringstream os;
  for (const auto& it : info_) {
    const std::string& dex_location = it.first;
    const DexFileData& dex_data = it.second;

    os << dex_location << kFieldSeparator << dex_data.checksum;
    for (auto method_it : dex_data.method_set) {
      os << kFieldSeparator << method_it;
    }
    os << kLineSeparator;
  }

  WriteToFile(fd, os);

  return CloseDescriptorForFile(fd, filename);
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

bool ProfileCompilationInfo::AddData(const std::string& dex_location,
                                     uint32_t checksum,
                                     uint16_t method_idx) {
  auto info_it = info_.find(dex_location);
  if (info_it == info_.end()) {
    info_it = info_.Put(dex_location, DexFileData(checksum));
  }
  if (info_it->second.checksum != checksum) {
    LOG(WARNING) << "Checksum mismatch for dex " << dex_location;
    return false;
  }
  info_it->second.method_set.insert(method_idx);
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
    uint32_t method_idx;
    if (!ParseInt(parts[i].c_str(), &method_idx)) {
      LOG(WARNING) << "Cannot parse method_idx " << parts[i];
      return false;
    }
    AddData(dex_location, checksum, method_idx);
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

bool ProfileCompilationInfo::Load(const std::string& filename) {
  int fd = OpenFile(filename, READ);
  if (fd == -1) {
    return false;
  }

  std::string current_line;
  const int kBufferSize = 1024;
  char buffer[kBufferSize];
  bool success = true;

  while (success) {
    int n = read(fd, buffer, kBufferSize);
    if (n < 0) {
      PLOG(WARNING) << "Error when reading profile file " << filename;
      success = false;
      break;
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
        success = false;
        break;
      }
      // Reset the current line (we just processed it).
      current_line.clear();
    }
  }
  if (!success) {
    info_.clear();
  }
  return CloseDescriptorForFile(fd, filename) && success;
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
  }
  return true;
}

bool ProfileCompilationInfo::ContainsMethod(const MethodReference& method_ref) const {
  auto info_it = info_.find(method_ref.dex_file->GetLocation());
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

}  // namespace art
