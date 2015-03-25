/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "oat_file.h"

#include <string>

#include <gtest/gtest.h>

namespace art {

TEST(OatFileTest, ResolveRelativeEncodedDexLocation) {
  EXPECT_EQ(std::string("/data/app/foo/base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        nullptr, "/data/app/foo/base.apk"));

  EXPECT_EQ(std::string("/system/framework/base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "/system/framework/base.apk"));

  EXPECT_EQ(std::string("/data/app/foo/base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "base.apk"));

  EXPECT_EQ(std::string("/data/app/foo/base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "foo/base.apk"));

  EXPECT_EQ(std::string("/data/app/foo/base.apk:classes2.dex"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "base.apk:classes2.dex"));

  EXPECT_EQ(std::string("/data/app/foo/base.apk:classes11.dex"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "base.apk:classes11.dex"));

  EXPECT_EQ(std::string("base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/sludge.apk", "base.apk"));

  EXPECT_EQ(std::string("o/base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "o/base.apk"));
}

}  // namespace art
