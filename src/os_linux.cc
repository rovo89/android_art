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

#include "os.h"

#include <cstddef>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "file_linux.h"

namespace art {

File* OS::OpenFile(const char* name, bool writable, bool create) {
  int flags = 0;
  if (writable) {
    flags |= O_RDWR;
    if (create) {
      flags |= (O_CREAT | O_TRUNC);
    }
  } else {
    flags |= O_RDONLY;
  }
  int fd = open(name, flags, 0666);
  if (fd < 0) {
    return NULL;
  }
  return new LinuxFile(name, fd, true);
}

File* OS::FileFromFd(const char* name, int fd) {
  return new LinuxFile(name, fd, false);
}

bool OS::FileExists(const char* name) {
  struct stat st;
  if (stat(name, &st) == 0) {
    return S_ISREG(st.st_mode);  // TODO: Deal with symlinks?
  } else {
    return false;
  }
}

bool OS::DirectoryExists(const char* name) {
  struct stat st;
  if (stat(name, &st) == 0) {
    return S_ISDIR(st.st_mode);  // TODO: Deal with symlinks?
  } else {
    return false;
  }
}

}  // namespace art
