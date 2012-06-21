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

#ifndef ART_SRC_OS_H_
#define ART_SRC_OS_H_

namespace art {

// Interface to the underlying OS platform.

class File;

class OS {
 public:
  // Open a file. The returned file must be deleted by the caller.
  static File* OpenFile(const char* name, bool writable, bool create = true);

  // Create a file from an already open file descriptor
  static File* FileFromFd(const char* name, int fd);

  // Check if a file exists.
  static bool FileExists(const char* name);

  // Check if a directory exists.
  static bool DirectoryExists(const char* name);
};

}  // namespace art

#endif  // ART_SRC_OS_H_
