/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include "file_linux.h"

#include <errno.h>
#include <unistd.h>

#include "logging.h"

namespace art {

LinuxFile::~LinuxFile() {
  // Close the file if necessary (unless it's a standard stream).
  if (auto_close_ && fd_ > STDERR_FILENO) {
    Close();
  }
}

void LinuxFile::Close() {
  DCHECK_GT(fd_, 0);
  int err = close(fd_);
  if (err != 0) {
    PLOG(WARNING) << "Problem closing " << name();
  }
  fd_ = kClosedFd;
}


bool LinuxFile::IsClosed() {
  return fd_ == kClosedFd;
}


int64_t LinuxFile::Read(void* buffer, int64_t num_bytes) {
  DCHECK_GE(fd_, 0);
  return read(fd_, buffer, num_bytes);
}


int64_t LinuxFile::Write(const void* buffer, int64_t num_bytes) {
  DCHECK_GE(fd_, 0);
  return write(fd_, buffer, num_bytes);
}


off_t LinuxFile::Position() {
  DCHECK_GE(fd_, 0);
  return lseek(fd_, 0, SEEK_CUR);
}


off_t LinuxFile::Length() {
  DCHECK_GE(fd_, 0);
  off_t position = lseek(fd_, 0, SEEK_CUR);
  if (position < 0) {
    // The file is not capable of seeking. Return an error.
    return -1;
  }
  off_t result = lseek(fd_, 0, SEEK_END);
  lseek(fd_, position, SEEK_SET);
  return result;
}

}  // namespace art
