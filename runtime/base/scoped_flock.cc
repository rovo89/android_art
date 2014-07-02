/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "scoped_flock.h"

#include <sys/file.h>
#include <sys/stat.h>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "base/unix_file/fd_file.h"

namespace art {

bool ScopedFlock::Init(const char* filename, std::string* error_msg) {
  while (true) {
    file_.reset(OS::OpenFileWithFlags(filename, O_CREAT | O_RDWR));
    if (file_.get() == NULL) {
      *error_msg = StringPrintf("Failed to open file '%s': %s", filename, strerror(errno));
      return false;
    }
    int flock_result = TEMP_FAILURE_RETRY(flock(file_->Fd(), LOCK_EX));
    if (flock_result != 0) {
      *error_msg = StringPrintf("Failed to lock file '%s': %s", filename, strerror(errno));
      return false;
    }
    struct stat fstat_stat;
    int fstat_result = TEMP_FAILURE_RETRY(fstat(file_->Fd(), &fstat_stat));
    if (fstat_result != 0) {
      *error_msg = StringPrintf("Failed to fstat file '%s': %s", filename, strerror(errno));
      return false;
    }
    struct stat stat_stat;
    int stat_result = TEMP_FAILURE_RETRY(stat(filename, &stat_stat));
    if (stat_result != 0) {
      PLOG(WARNING) << "Failed to stat, will retry: " << filename;
      // ENOENT can happen if someone racing with us unlinks the file we created so just retry.
      continue;
    }
    if (fstat_stat.st_dev != stat_stat.st_dev || fstat_stat.st_ino != stat_stat.st_ino) {
      LOG(WARNING) << "File changed while locking, will retry: " << filename;
      continue;
    }
    return true;
  }
}

bool ScopedFlock::Init(File* file, std::string* error_msg) {
  file_.reset(new File(dup(file->Fd())));
  if (file_->Fd() == -1) {
    file_.reset();
    *error_msg = StringPrintf("Failed to duplicate open file '%s': %s",
                              file->GetPath().c_str(), strerror(errno));
    return false;
  }
  if (0 != TEMP_FAILURE_RETRY(flock(file_->Fd(), LOCK_EX))) {
    file_.reset();
    *error_msg = StringPrintf("Failed to lock file '%s': %s", file->GetPath().c_str(), strerror(errno));
    return false;
  }
  return true;
}

File* ScopedFlock::GetFile() {
  CHECK(file_.get() != NULL);
  return file_.get();
}

bool ScopedFlock::HasFile() {
  return file_.get() != nullptr;
}

ScopedFlock::ScopedFlock() { }

ScopedFlock::~ScopedFlock() {
  if (file_.get() != NULL) {
    int flock_result = TEMP_FAILURE_RETRY(flock(file_->Fd(), LOCK_UN));
    CHECK_EQ(0, flock_result);
  }
}

}  // namespace art
