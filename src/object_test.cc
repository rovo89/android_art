// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "src/class_linker.h"
#include "src/common_test.h"
#include "src/dex_file.h"
#include "src/heap.h"
#include "src/object.h"
#include "src/scoped_ptr.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

class ObjectTest : public RuntimeTest {};

TEST_F(ObjectTest, IsInSamePackage) {
  // Matches
  EXPECT_TRUE(Class::IsInSamePackage("Ljava/lang/Object;",
                                     "Ljava/lang/Class"));
  EXPECT_TRUE(Class::IsInSamePackage("LFoo;",
                                     "LBar;"));

  // Mismatches
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;",
                                      "Ljava/io/File;"));
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;",
                                      "Ljava/lang/reflect/Method;"));
}

class MethodTest : public RuntimeTest {};

// TODO: test 0 argument methods
// TODO: make this test simpler and shorter
TEST_F(MethodTest, ProtoCompare) {
  scoped_ptr<DexFile> object_dex_file(DexFile::OpenBase64(kJavaLangDex));
  ASSERT_TRUE(object_dex_file != NULL);
  scoped_ptr<DexFile> proto_dex_file(DexFile::OpenBase64(kProtoCompareDex));
  ASSERT_TRUE(proto_dex_file != NULL);

  scoped_ptr<ClassLinker> linker(ClassLinker::Create());
  linker->AppendToClassPath(object_dex_file.get());
  linker->AppendToClassPath(proto_dex_file.get());

  Class* klass = linker->FindClass("LProtoCompare;", NULL);
  ASSERT_TRUE(klass != NULL);

  ASSERT_EQ(4U, klass->NumVirtualMethods());

  Method* m1 = klass->GetVirtualMethod(0);
  ASSERT_EQ("m1", m1->GetName());

  Method* m2 = klass->GetVirtualMethod(1);
  ASSERT_EQ("m2", m2->GetName());

  Method* m3 = klass->GetVirtualMethod(2);
  ASSERT_EQ("m3", m3->GetName());

  Method* m4 = klass->GetVirtualMethod(3);
  ASSERT_EQ("m4", m4->GetName());

  EXPECT_TRUE(m1->HasSameReturnType(m2));
  EXPECT_TRUE(m2->HasSameReturnType(m1));

  EXPECT_TRUE(m1->HasSameReturnType(m2));
  EXPECT_TRUE(m2->HasSameReturnType(m1));

  EXPECT_FALSE(m1->HasSameReturnType(m4));
  EXPECT_FALSE(m4->HasSameReturnType(m1));

  EXPECT_TRUE(m1->HasSameArgumentTypes(m2));
  EXPECT_TRUE(m2->HasSameArgumentTypes(m1));

  EXPECT_FALSE(m1->HasSameArgumentTypes(m3));
  EXPECT_FALSE(m3->HasSameArgumentTypes(m1));

  EXPECT_FALSE(m1->HasSameArgumentTypes(m4));
  EXPECT_FALSE(m4->HasSameArgumentTypes(m1));

  EXPECT_TRUE(m1->HasSamePrototype(m2));
  EXPECT_TRUE(m2->HasSamePrototype(m1));

  EXPECT_FALSE(m1->HasSamePrototype(m3));
  EXPECT_FALSE(m3->HasSamePrototype(m1));

  EXPECT_FALSE(m3->HasSamePrototype(m4));
  EXPECT_FALSE(m4->HasSamePrototype(m3));

  EXPECT_FALSE(m1->HasSameName(m2));
  EXPECT_FALSE(m1->HasSameNameAndPrototype(m2));
}

TEST_F(MethodTest, ProtoCompare2) {
  scoped_ptr<DexFile> object_dex_file(DexFile::OpenBase64(kJavaLangDex));
  ASSERT_TRUE(object_dex_file != NULL);
  scoped_ptr<DexFile> proto1_dex_file(DexFile::OpenBase64(kProtoCompareDex));
  ASSERT_TRUE(proto1_dex_file != NULL);
  scoped_ptr<DexFile> proto2_dex_file(DexFile::OpenBase64(kProtoCompare2Dex));
  ASSERT_TRUE(proto2_dex_file != NULL);
  scoped_ptr<ClassLinker> linker1(ClassLinker::Create());
  linker1->AppendToClassPath(object_dex_file.get());
  linker1->AppendToClassPath(proto1_dex_file.get());
  scoped_ptr<ClassLinker> linker2(ClassLinker::Create());
  linker2->AppendToClassPath(object_dex_file.get());
  linker2->AppendToClassPath(proto2_dex_file.get());

  Class* klass1 = linker1->FindClass("LProtoCompare;", NULL);
  ASSERT_TRUE(klass1 != NULL);
  Class* klass2 = linker2->FindClass("LProtoCompare2;", NULL);
  ASSERT_TRUE(klass2 != NULL);

  Method* m1_1 = klass1->GetVirtualMethod(0);
  ASSERT_EQ("m1", m1_1->GetName());
  Method* m2_1 = klass1->GetVirtualMethod(1);
  ASSERT_EQ("m2", m2_1->GetName());
  Method* m3_1 = klass1->GetVirtualMethod(2);
  ASSERT_EQ("m3", m3_1->GetName());
  Method* m4_1 = klass1->GetVirtualMethod(3);
  ASSERT_EQ("m4", m4_1->GetName());

  Method* m1_2 = klass2->GetVirtualMethod(0);
  ASSERT_EQ("m1", m1_2->GetName());
  Method* m2_2 = klass2->GetVirtualMethod(1);
  ASSERT_EQ("m2", m2_2->GetName());
  Method* m3_2 = klass2->GetVirtualMethod(2);
  ASSERT_EQ("m3", m3_2->GetName());
  Method* m4_2 = klass2->GetVirtualMethod(3);
  ASSERT_EQ("m4", m4_2->GetName());

  EXPECT_TRUE(m1_1->HasSameNameAndPrototype(m1_2));
  EXPECT_TRUE(m1_2->HasSameNameAndPrototype(m1_1));

  EXPECT_TRUE(m2_1->HasSameNameAndPrototype(m2_2));
  EXPECT_TRUE(m2_2->HasSameNameAndPrototype(m2_1));

  EXPECT_TRUE(m3_1->HasSameNameAndPrototype(m3_2));
  EXPECT_TRUE(m3_2->HasSameNameAndPrototype(m3_1));

  EXPECT_TRUE(m4_1->HasSameNameAndPrototype(m4_2));
  EXPECT_TRUE(m4_2->HasSameNameAndPrototype(m4_1));
}

}  // namespace art
