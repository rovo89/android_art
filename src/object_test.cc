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

TEST(Object, IsInSamePackage) {
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

// TODO: test 0 argument methods
// TODO: make this test simpler and shorter
TEST(Method, ProtoCompare) {
  scoped_ptr<DexFile> dex_file(DexFile::OpenBase64(kProtoCompareDex));
  ASSERT_TRUE(dex_file != NULL);

  scoped_ptr<ClassLinker> linker(ClassLinker::Create());
  linker->AppendToClassPath(dex_file.get());

  scoped_ptr<Class> klass(linker.get()->AllocClass(dex_file.get()));
  bool result = linker->LoadClass("LProtoCompare;", klass.get());
  ASSERT_TRUE(result);

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

TEST(Method, ProtoCompare2) {
  scoped_ptr<DexFile> dex_file1(DexFile::OpenBase64(kProtoCompareDex));
  ASSERT_TRUE(dex_file1 != NULL);
  scoped_ptr<DexFile> dex_file2(DexFile::OpenBase64(kProtoCompare2Dex));
  ASSERT_TRUE(dex_file2 != NULL);
  scoped_ptr<ClassLinker> linker1(ClassLinker::Create());
  linker1->AppendToClassPath(dex_file1.get());
  scoped_ptr<ClassLinker> linker2(ClassLinker::Create());
  linker2->AppendToClassPath(dex_file2.get());

  scoped_ptr<Class> klass1(linker1.get()->AllocClass(dex_file1.get()));
  bool result1 = linker1->LoadClass("LProtoCompare;", klass1.get());
  ASSERT_TRUE(result1);
  scoped_ptr<Class> klass2(linker2.get()->AllocClass(dex_file2.get()));
  bool result2 = linker2->LoadClass("LProtoCompare2;", klass2.get());
  ASSERT_TRUE(result2);

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
