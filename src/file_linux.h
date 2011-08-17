// Copyright 2010 Google Inc. All Rights Reserved.

#ifndef ART_SRC_FILE_LINUX_H_
#define ART_SRC_FILE_LINUX_H_

#include "file.h"

namespace art {

class LinuxFile : public File {
 public:
  LinuxFile(const char* name, int fd, bool auto_close) :
        File(name), fd_(fd), auto_close_(auto_close) {}
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
