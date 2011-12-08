// Copyright 2011 Google Inc. All Rights Reserved.

#include "zip_archive.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "UniquePtr.h"
#include "common_test.h"
#include "os.h"

namespace art {

class ZipArchiveTest : public CommonTest {};

TEST_F(ZipArchiveTest, FindAndExtract) {
  UniquePtr<ZipArchive> zip_archive(ZipArchive::Open(GetLibCoreDexFileName()));
  ASSERT_TRUE(zip_archive.get() != false);
  UniquePtr<ZipEntry> zip_entry(zip_archive->Find("classes.dex"));
  ASSERT_TRUE(zip_entry.get() != false);

  ScratchFile tmp;
  ASSERT_NE(-1, tmp.GetFd());
  UniquePtr<File> file(OS::FileFromFd(tmp.GetFilename(), tmp.GetFd()));
  ASSERT_TRUE(file.get() != NULL);
  bool success = zip_entry->ExtractToFile(*file);
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
