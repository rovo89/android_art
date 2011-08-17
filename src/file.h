// Copyright 2009 Google Inc. All Rights Reserved.

#ifndef ART_SRC_FILE_H_
#define ART_SRC_FILE_H_

#include <stdint.h>
#include <sys/types.h>

namespace art {

class File {
 public:
  virtual ~File() { }

  virtual int64_t Read(void* buffer, int64_t num_bytes) = 0;
  virtual int64_t Write(const void* buffer, int64_t num_bytes) = 0;

  // ReadFully and WriteFully do attempt to transfer all of the bytes to/from
  // the buffer. In the event of short accesses they will loop internally until
  // the whole buffer has been transferred or an error occurs. If an error
  // occurred the result will be set to false.
  virtual bool ReadFully(void* buffer, int64_t num_bytes);
  virtual bool WriteFully(const void* buffer, int64_t num_bytes);
  bool WriteByte(uint8_t byte) {
    return WriteFully(&byte, 1);
  }

  // Get the length of the file. Returns a negative value if the length cannot
  // be determined (e.g. not seekable device).
  virtual off_t Length() = 0;

  // Get the current position in the file.
  // Returns a negative value if position cannot be determined.
  virtual off_t Position() = 0;

  virtual int Fd() = 0;

  const char* name() const { return name_; }

 protected:
  explicit File(const char* name) : name_(name) { }
  virtual void Close() = 0;
  virtual bool IsClosed() = 0;

 private:
  const char* name_;
};

}  // namespace art

#endif  // ART_SRC_FILE_H_
