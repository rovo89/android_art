// Copyright 2009 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OS_H_
#define ART_SRC_OS_H_

namespace art {

// Interface to the underlying OS platform.

class File;

class OS {
 public:

  // Open a file. The returned file must be deleted by the caller.
  static File* OpenBinaryFile(const char* name, bool writable);
  static File* OpenTextFile(const char* name, bool writable);

  // Create a file from an already open file descriptor
  static File* FileFromFd(const char* name, int fd);

  // Check if a file exists.
  static bool FileExists(const char* name);
};

}  // namespace art

#endif  // ART_SRC_OS_H_
