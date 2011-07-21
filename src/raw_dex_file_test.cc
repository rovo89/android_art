// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"
#include "dex_file.h"
#include "raw_dex_file.h"
#include "scoped_ptr.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

TEST(RawDexFileTest, Open) {
  scoped_ptr<RawDexFile> raw(OpenRawDexFileBase64(kNestedDex));
  ASSERT_TRUE(raw != NULL);
}

TEST(RawDexFileTest, Header) {
  scoped_ptr<RawDexFile> raw(OpenRawDexFileBase64(kNestedDex));
  ASSERT_TRUE(raw != NULL);

  const RawDexFile::Header& header = raw->GetHeader();
  // TODO: header.magic_
  EXPECT_EQ(0x00d87910U, header.checksum_);
  // TODO: header.signature_
  EXPECT_EQ(904U, header.file_size_);
  EXPECT_EQ(112U, header.header_size_);
  EXPECT_EQ(0U, header.link_size_);
  EXPECT_EQ(0U, header.link_off_);
  EXPECT_EQ(15U, header.string_ids_size_);
  EXPECT_EQ(112U, header.string_ids_off_);
  EXPECT_EQ(7U, header.type_ids_size_);
  EXPECT_EQ(172U, header.type_ids_off_);
  EXPECT_EQ(2U, header.proto_ids_size_);
  EXPECT_EQ(200U, header.proto_ids_off_);
  EXPECT_EQ(1U, header.field_ids_size_);
  EXPECT_EQ(224U, header.field_ids_off_);
  EXPECT_EQ(3U, header.method_ids_size_);
  EXPECT_EQ(232U, header.method_ids_off_);
  EXPECT_EQ(2U, header.class_defs_size_);
  EXPECT_EQ(256U, header.class_defs_off_);
  EXPECT_EQ(584U, header.data_size_);
  EXPECT_EQ(320U, header.data_off_);
}

TEST(RawDexFileTest, ClassDefs) {
  scoped_ptr<RawDexFile> raw(OpenRawDexFileBase64(kNestedDex));
  ASSERT_TRUE(raw != NULL);
  EXPECT_EQ(2U, raw->NumClassDefs());

  const RawDexFile::ClassDef& c0 = raw->GetClassDef(0);
  EXPECT_STREQ("LNested$Inner;", raw->GetClassDescriptor(c0));

  const RawDexFile::ClassDef& c1 = raw->GetClassDef(1);
  EXPECT_STREQ("LNested;", raw->GetClassDescriptor(c1));
}

}  // namespace art
