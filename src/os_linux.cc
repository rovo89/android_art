// Copyright 2010 Google Inc. All Rights Reserved.

#include "os.h"

#include <cstddef>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "file_linux.h"

namespace art {

File* OS::OpenFile(const char* name, bool writable) {
  int flags = O_RDONLY;
  if (writable) {
    flags = (O_RDWR | O_CREAT | O_TRUNC);
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
