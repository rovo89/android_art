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

#ifndef ART_SRC_FILE_LINUX_H_
#define ART_SRC_FILE_LINUX_H_

#include "file.h"

namespace art {

class LinuxFile : public File {
 public:
  LinuxFile(const char* name, int fd, bool auto_close)
      : File(name), fd_(fd), auto_close_(auto_close) {}
  virtual ~LinuxFile();

  virtual void Close();
  virtual bool IsClosed();

  virtual int64_t Read(void* buffer, int64_t num_bytes);
  virtual int64_t Write(const void* buffer, int64_t num_bytes);

  virtual off_t Length();
  virtual off_t Position();

  virtual int Fd() {
    return fd_;
  }

 private:
  static const int kClosedFd = -1;

  int fd_;
  bool auto_close_;
};

}  // namespace art

#endif  // ART_SRC_FILE_LINUX_H_
