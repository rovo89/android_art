// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"
#include "dex_file.h"
#include "scoped_ptr.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

TEST(DexFileTest, Open) {
  scoped_ptr<const DexFile> dex(OpenDexFileBase64(kNestedDex, "kNestedDex"));
  ASSERT_TRUE(dex != NULL);
}

TEST(DexFileTest, Header) {
  scoped_ptr<const DexFile> raw(OpenDexFileBase64(kNestedDex, "kNestedDex"));
  ASSERT_TRUE(raw != NULL);

  const DexFile::Header& header = raw->GetHeader();
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

TEST(DexFileTest, ClassDefs) {
  scoped_ptr<const DexFile> raw(OpenDexFileBase64(kNestedDex, "kNestedDex"));
  ASSERT_TRUE(raw != NULL);
  EXPECT_EQ(2U, raw->NumClassDefs());

  const DexFile::ClassDef& c0 = raw->GetClassDef(0);
  EXPECT_STREQ("LNested$Inner;", raw->GetClassDescriptor(c0));

  const DexFile::ClassDef& c1 = raw->GetClassDef(1);
  EXPECT_STREQ("LNested;", raw->GetClassDescriptor(c1));
}

TEST(DexFileTest, CreateMethodDescriptor) {
  scoped_ptr<const DexFile> raw(OpenDexFileBase64(kCreateMethodDescriptorDex, "kCreateMethodDescriptorDex"));
  ASSERT_TRUE(raw != NULL);
  EXPECT_EQ(1U, raw->NumClassDefs());

  const DexFile::ClassDef& class_def = raw->GetClassDef(0);
  ASSERT_STREQ("LCreateMethodDescriptor;", raw->GetClassDescriptor(class_def));

  const byte* class_data = raw->GetClassData(class_def);
  ASSERT_TRUE(class_data != NULL);
  DexFile::ClassDataHeader header = raw->ReadClassDataHeader(&class_data);

  EXPECT_EQ(1u, header.direct_methods_size_);

  // Check the descriptor for the static initializer.
  {
    uint32_t last_idx = 0;
    ASSERT_EQ(1U, header.direct_methods_size_);
    DexFile::Method method;
    raw->dexReadClassDataMethod(&class_data, &method, &last_idx);
    const DexFile::MethodId& method_id = raw->GetMethodId(method.method_idx_);
    uint32_t proto_idx = method_id.proto_idx_;
    const char* name = raw->dexStringById(method_id.name_idx_);
    ASSERT_STREQ("<init>", name);
    int32_t length;
    std::string descriptor(raw->CreateMethodDescriptor(proto_idx, &length));
    ASSERT_EQ("()V", descriptor);
  }

  // Check both virtual methods.
  ASSERT_EQ(2U, header.virtual_methods_size_);
  uint32_t last_idx = 0;

  {
    DexFile::Method method;
    raw->dexReadClassDataMethod(&class_data, &method, &last_idx);
    const DexFile::MethodId& method_id = raw->GetMethodId(method.method_idx_);

    const char* name = raw->dexStringById(method_id.name_idx_);
    ASSERT_STREQ("m1", name);

    uint32_t proto_idx = method_id.proto_idx_;
    int32_t length;
    std::string descriptor(raw->CreateMethodDescriptor(proto_idx, &length));
    ASSERT_EQ("(IDJLjava/lang/Object;)Ljava/lang/Float;", descriptor);
  }

  {
    DexFile::Method method;
    raw->dexReadClassDataMethod(&class_data, &method, &last_idx);
    const DexFile::MethodId& method_id = raw->GetMethodId(method.method_idx_);

    const char* name = raw->dexStringById(method_id.name_idx_);
    ASSERT_STREQ("m2", name);

    uint32_t proto_idx = method_id.proto_idx_;
    int32_t length;
    std::string descriptor(raw->CreateMethodDescriptor(proto_idx, &length));
    ASSERT_EQ("(ZSC)LCreateMethodDescriptor;", descriptor);
  }

}

}  // namespace art
