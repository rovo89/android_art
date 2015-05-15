/*
 * Copyright (C) 2009 The Android Open Source Project
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

#include "base/unix_file/fd_file.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "base/logging.h"

namespace unix_file {

FdFile::FdFile() : guard_state_(GuardState::kClosed), fd_(-1), auto_close_(true) {
}

FdFile::FdFile(int fd, bool check_usage)
    : guard_state_(check_usage ? GuardState::kBase : GuardState::kNoCheck),
      fd_(fd), auto_close_(true) {
}

FdFile::FdFile(int fd, const std::string& path, bool check_usage)
    : guard_state_(check_usage ? GuardState::kBase : GuardState::kNoCheck),
      fd_(fd), file_path_(path), auto_close_(true) {
  CHECK_NE(0U, path.size());
}

FdFile::~FdFile() {
  if (kCheckSafeUsage && (guard_state_ < GuardState::kNoCheck)) {
    if (guard_state_ < GuardState::kFlushed) {
      LOG(::art::ERROR) << "File " << file_path_ << " wasn't explicitly flushed before destruction.";
    }
    if (guard_state_ < GuardState::kClosed) {
      LOG(::art::ERROR) << "File " << file_path_ << " wasn't explicitly closed before destruction.";
    }
    CHECK_GE(guard_state_, GuardState::kClosed);
  }
  if (auto_close_ && fd_ != -1) {
    if (Close() != 0) {
      PLOG(::art::WARNING) << "Failed to close file " << file_path_;
    }
  }
}

void FdFile::moveTo(GuardState target, GuardState warn_threshold, const char* warning) {
  if (kCheckSafeUsage) {
    if (guard_state_ < GuardState::kNoCheck) {
      if (warn_threshold < GuardState::kNoCheck && guard_state_ >= warn_threshold) {
        LOG(::art::ERROR) << warning;
      }
      guard_state_ = target;
    }
  }
}

void FdFile::moveUp(GuardState target, const char* warning) {
  if (kCheckSafeUsage) {
    if (guard_state_ < GuardState::kNoCheck) {
      if (guard_state_ < target) {
        guard_state_ = target;
      } else if (target < guard_state_) {
        LOG(::art::ERROR) << warning;
      }
    }
  }
}

void FdFile::DisableAutoClose() {
  auto_close_ = false;
}

bool FdFile::Open(const std::string& path, int flags) {
  return Open(path, flags, 0640);
}

bool FdFile::Open(const std::string& path, int flags, mode_t mode) {
  CHECK_EQ(fd_, -1) << path;
  fd_ = TEMP_FAILURE_RETRY(open(path.c_str(), flags, mode));
  if (fd_ == -1) {
    return false;
  }
  file_path_ = path;
  static_assert(O_RDONLY == 0, "Readonly flag has unexpected value.");
  if (kCheckSafeUsage && (flags & (O_RDWR | O_CREAT | O_WRONLY)) != 0) {
    // Start in the base state (not flushed, not closed).
    guard_state_ = GuardState::kBase;
  } else {
    // We are not concerned with read-only files. In that case, proper flushing and closing is
    // not important.
    guard_state_ = GuardState::kNoCheck;
  }
  return true;
}

int FdFile::Close() {
  int result = close(fd_);

  // Test here, so the file is closed and not leaked.
  if (kCheckSafeUsage) {
    CHECK_GE(guard_state_, GuardState::kFlushed) << "File " << file_path_
        << " has not been flushed before closing.";
    moveUp(GuardState::kClosed, nullptr);
  }

  if (result == -1) {
    return -errno;
  } else {
    fd_ = -1;
    file_path_ = "";
    return 0;
  }
}

int FdFile::Flush() {
#ifdef __linux__
  int rc = TEMP_FAILURE_RETRY(fdatasync(fd_));
#else
  int rc = TEMP_FAILURE_RETRY(fsync(fd_));
#endif
  moveUp(GuardState::kFlushed, "Flushing closed file.");
  return (rc == -1) ? -errno : rc;
}

int64_t FdFile::Read(char* buf, int64_t byte_count, int64_t offset) const {
#ifdef __linux__
  int rc = TEMP_FAILURE_RETRY(pread64(fd_, buf, byte_count, offset));
#else
  int rc = TEMP_FAILURE_RETRY(pread(fd_, buf, byte_count, offset));
#endif
  return (rc == -1) ? -errno : rc;
}

int FdFile::SetLength(int64_t new_length) {
#ifdef __linux__
  int rc = TEMP_FAILURE_RETRY(ftruncate64(fd_, new_length));
#else
  int rc = TEMP_FAILURE_RETRY(ftruncate(fd_, new_length));
#endif
  moveTo(GuardState::kBase, GuardState::kClosed, "Truncating closed file.");
  return (rc == -1) ? -errno : rc;
}

int64_t FdFile::GetLength() const {
  struct stat s;
  int rc = TEMP_FAILURE_RETRY(fstat(fd_, &s));
  return (rc == -1) ? -errno : s.st_size;
}

int64_t FdFile::Write(const char* buf, int64_t byte_count, int64_t offset) {
#ifdef __linux__
  int rc = TEMP_FAILURE_RETRY(pwrite64(fd_, buf, byte_count, offset));
#else
  int rc = TEMP_FAILURE_RETRY(pwrite(fd_, buf, byte_count, offset));
#endif
  moveTo(GuardState::kBase, GuardState::kClosed, "Writing into closed file.");
  return (rc == -1) ? -errno : rc;
}

int FdFile::Fd() const {
  return fd_;
}

bool FdFile::IsOpened() const {
  return fd_ >= 0;
}

static ssize_t ReadIgnoreOffset(int fd, void *buf, size_t count, off_t offset) {
  DCHECK_EQ(offset, 0);
  return read(fd, buf, count);
}

template <ssize_t (*read_func)(int, void*, size_t, off_t)>
static bool ReadFullyGeneric(int fd, void* buffer, size_t byte_count, size_t offset) {
  char* ptr = static_cast<char*>(buffer);
  while (byte_count > 0) {
    ssize_t bytes_read = TEMP_FAILURE_RETRY(read_func(fd, ptr, byte_count, offset));
    if (bytes_read <= 0) {
      // 0: end of file
      // -1: error
      return false;
    }
    byte_count -= bytes_read;  // Reduce the number of remaining bytes.
    ptr += bytes_read;  // Move the buffer forward.
    offset += static_cast<size_t>(bytes_read);  // Move the offset forward.
  }
  return true;
}

bool FdFile::ReadFully(void* buffer, size_t byte_count) {
  return ReadFullyGeneric<ReadIgnoreOffset>(fd_, buffer, byte_count, 0);
}

bool FdFile::PreadFully(void* buffer, size_t byte_count, size_t offset) {
  return ReadFullyGeneric<pread>(fd_, buffer, byte_count, offset);
}

bool FdFile::WriteFully(const void* buffer, size_t byte_count) {
  const char* ptr = static_cast<const char*>(buffer);
  moveTo(GuardState::kBase, GuardState::kClosed, "Writing into closed file.");
  while (byte_count > 0) {
    ssize_t bytes_written = TEMP_FAILURE_RETRY(write(fd_, ptr, byte_count));
    if (bytes_written == -1) {
      return false;
    }
    byte_count -= bytes_written;  // Reduce the number of remaining bytes.
    ptr += bytes_written;  // Move the buffer forward.
  }
  return true;
}

void FdFile::Erase() {
  TEMP_FAILURE_RETRY(SetLength(0));
  TEMP_FAILURE_RETRY(Flush());
  TEMP_FAILURE_RETRY(Close());
}

int FdFile::FlushCloseOrErase() {
  int flush_result = TEMP_FAILURE_RETRY(Flush());
  if (flush_result != 0) {
    LOG(::art::ERROR) << "CloseOrErase failed while flushing a file.";
    Erase();
    return flush_result;
  }
  int close_result = TEMP_FAILURE_RETRY(Close());
  if (close_result != 0) {
    LOG(::art::ERROR) << "CloseOrErase failed while closing a file.";
    Erase();
    return close_result;
  }
  return 0;
}

int FdFile::FlushClose() {
  int flush_result = TEMP_FAILURE_RETRY(Flush());
  if (flush_result != 0) {
    LOG(::art::ERROR) << "FlushClose failed while flushing a file.";
  }
  int close_result = TEMP_FAILURE_RETRY(Close());
  if (close_result != 0) {
    LOG(::art::ERROR) << "FlushClose failed while closing a file.";
  }
  return (flush_result != 0) ? flush_result : close_result;
}

void FdFile::MarkUnchecked() {
  guard_state_ = GuardState::kNoCheck;
}

}  // namespace unix_file
