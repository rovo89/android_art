// Copyright 2011 Google Inc. All Rights Reserved.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "common_test.h"
#include "zip_archive.h"
#include "gtest/gtest.h"

namespace art {

class ZipArchiveTest : public RuntimeTest {};

class TmpFile {
 public:
  TmpFile() {
    std::string filename_template;
    filename_template = getenv("ANDROID_DATA");
    filename_template += "/TmpFile-XXXXXX";
    filename_.reset(strdup(filename_template.c_str()));
    CHECK(filename_ != NULL);
    fd_ = mkstemp(filename_.get());
    CHECK_NE(-1, fd_);
  }

  ~TmpFile() {
    int unlink_result = unlink(filename_.get());
    CHECK_EQ(0, unlink_result);
    int close_result = close(fd_);
    CHECK_EQ(0, close_result);
  }

  const char* GetFilename() const {
    return filename_.get();
  }

  int GetFd() const {
    return fd_;
  }

 private:
  scoped_ptr_malloc<char> filename_;
  int fd_;
};

TEST_F(ZipArchiveTest, FindAndExtract) {
  scoped_ptr<ZipArchive> zip_archive(ZipArchive::Open(GetLibCoreDexFileName()));
  ASSERT_TRUE(zip_archive != false);
  scoped_ptr<ZipEntry> zip_entry(zip_archive->Find("classes.dex"));
  ASSERT_TRUE(zip_entry != false);

  TmpFile tmp;
  ASSERT_NE(-1, tmp.GetFd());
  bool success = zip_entry->Extract(tmp.GetFd());
  ASSERT_TRUE(success);
  close(tmp.GetFd());

  uint32_t computed_crc = crc32(0L, Z_NULL, 0);
  int fd = open(tmp.GetFilename(), O_RDONLY);
  ASSERT_NE(-1, fd);
  const size_t kBufSize = 32768;
  uint8_t buf[kBufSize];
  while (true) {
    ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fd, buf, kBufSize));
    if (bytes_read == 0) {
      break;
    }
    computed_crc = crc32(computed_crc, buf, bytes_read);
  }
  EXPECT_EQ(zip_entry->GetCrc32(), computed_crc);
}

}  // namespace art
