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

#include "file.h"

#include "UniquePtr.h"
#include "common_test.h"
#include "os.h"

namespace art {

class FileTest : public CommonTest {};

TEST_F(FileTest, Read) {
  std::string filename(GetLibCoreDexFileName());
  UniquePtr<File> file(OS::OpenFile(filename.c_str(), false));
  ASSERT_TRUE(file.get() != NULL);
  EXPECT_STREQ(filename.c_str(), file->name());
  char buffer[3];
  buffer[0] = '\0';
  EXPECT_TRUE(file->ReadFully(buffer, 2));  // ReadFully returns true.
  buffer[2] = '\0';
  EXPECT_STREQ("PK", buffer); // zip file magic
  EXPECT_FALSE(file->WriteByte(1));  // Cannot write to a read-only file.
}


TEST_F(FileTest, FileLength) {
  std::string filename(GetLibCoreDexFileName());
  UniquePtr<File> file(OS::OpenFile(filename.c_str(), false));
  ASSERT_TRUE(file.get() != NULL);
  EXPECT_NE(0, file->Length());
}


TEST_F(FileTest, FilePosition) {
  std::string filename(GetLibCoreDexFileName());
  UniquePtr<File> file(OS::OpenFile(filename.c_str(), false));
  ASSERT_TRUE(file.get() != NULL);
  char buf[4];
  EXPECT_TRUE(file->ReadFully(buf, 2));
  EXPECT_EQ(2, file->Position());
  EXPECT_TRUE(file->ReadFully(buf, 2));
  EXPECT_EQ(4, file->Position());
}


TEST_F(FileTest, FileFd) {
  std::string filename(GetLibCoreDexFileName());
  UniquePtr<File> file(OS::OpenFile(filename.c_str(), false));
  ASSERT_TRUE(file.get() != NULL);
  EXPECT_NE(-1, file->Fd());
}


}  // namespace art
