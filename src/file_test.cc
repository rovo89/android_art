// Copyright 2009 Google Inc. All Rights Reserved.

#include "common_test.h"
#include "file.h"
#include "os.h"
#include "scoped_ptr.h"

#include "gtest/gtest.h"

namespace art {

class FileTest : public CommonTest {};

TEST_F(FileTest, Read) {
  std::string filename = GetLibCoreDexFileName();
  scoped_ptr<File> file(OS::OpenFile(filename.c_str(), false));
  ASSERT_TRUE(file != NULL);
  EXPECT_STREQ(filename.c_str(), file->name());
  char buffer[3];
  buffer[0] = '\0';
  EXPECT_TRUE(file->ReadFully(buffer, 2));  // ReadFully returns true.
  buffer[2] = '\0';
  EXPECT_STREQ("PK", buffer); // zip file magic
  EXPECT_FALSE(file->WriteByte(1));  // Cannot write to a read-only file.
}


TEST_F(FileTest, FileLength) {
  std::string filename = GetLibCoreDexFileName();
  scoped_ptr<File> file(OS::OpenFile(filename.c_str(), false));
  ASSERT_TRUE(file != NULL);
  EXPECT_NE(0, file->Length());
}


TEST_F(FileTest, FilePosition) {
  std::string filename = GetLibCoreDexFileName();
  scoped_ptr<File> file(OS::OpenFile(filename.c_str(), false));
  ASSERT_TRUE(file != NULL);
  char buf[4];
  EXPECT_TRUE(file->ReadFully(buf, 2));
  EXPECT_EQ(2, file->Position());
  EXPECT_TRUE(file->ReadFully(buf, 2));
  EXPECT_EQ(4, file->Position());
}

}  // namespace art
