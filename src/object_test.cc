// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "src/dex_file.h"
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

// class ProtoCompare {
//     int m1(short x, int y, long z) { return x + y + (int)z; }
//     int m2(short x, int y, long z) { return x + y + (int)z; }
//     int m3(long x, int y, short z) { return (int)x + y + z; }
//     long m4(long x, int y, short z) { return x + y + z; }
// }
static const char kProtoCompareDex[] =
  "ZGV4CjAzNQBLUetu+TVZ8gsYsCOFoij7ecsHaGSEGA8gAwAAcAAAAHhWNBIAAAAAAAAAAIwCAAAP"
  "AAAAcAAAAAYAAACsAAAABAAAAMQAAAAAAAAAAAAAAAYAAAD0AAAAAQAAACQBAADcAQAARAEAAN4B"
  "AADmAQAA6QEAAO8BAAD1AQAA+AEAAP4BAAAOAgAAIgIAADUCAAA4AgAAOwIAAD8CAABDAgAARwIA"
  "AAEAAAAEAAAABgAAAAcAAAAJAAAACgAAAAIAAAAAAAAAyAEAAAMAAAAAAAAA1AEAAAUAAAABAAAA"
  "yAEAAAoAAAAFAAAAAAAAAAIAAwAAAAAAAgABAAsAAAACAAEADAAAAAIAAAANAAAAAgACAA4AAAAD"
  "AAMAAAAAAAIAAAAAAAAAAwAAAAAAAAAIAAAAAAAAAHACAAAAAAAAAQABAAEAAABLAgAABAAAAHAQ"
  "BQAAAA4ABwAFAAAAAABQAgAABQAAAJAAAwSEUbAQDwAAAAcABQAAAAAAWAIAAAUAAACQAAMEhFGw"
  "EA8AAAAGAAUAAAAAAGACAAAEAAAAhCCwQLBQDwAJAAUAAAAAAGgCAAAFAAAAgXC7UIGCuyAQAAAA"
  "AwAAAAEAAAAEAAAAAwAAAAQAAAABAAY8aW5pdD4AAUkABElKSVMABElTSUoAAUoABEpKSVMADkxQ"
  "cm90b0NvbXBhcmU7ABJMamF2YS9sYW5nL09iamVjdDsAEVByb3RvQ29tcGFyZS5qYXZhAAFTAAFW"
  "AAJtMQACbTIAAm0zAAJtNAABAAcOAAIDAAAABw4AAwMAAAAHDgAEAwAAAAcOAAUDAAAABw4AAAAB"
  "BACAgATEAgEA3AIBAPgCAQCUAwEArAMAAAwAAAAAAAAAAQAAAAAAAAABAAAADwAAAHAAAAACAAAA"
  "BgAAAKwAAAADAAAABAAAAMQAAAAFAAAABgAAAPQAAAAGAAAAAQAAACQBAAABIAAABQAAAEQBAAAB"
  "EAAAAgAAAMgBAAACIAAADwAAAN4BAAADIAAABQAAAEsCAAAAIAAAAQAAAHACAAAAEAAAAQAAAIwC"
  "AAA=";

// TODO: test 0 argument methods
// TODO: make this test simpler and shorter
TEST(Method, ProtoCompare) {
  scoped_ptr<DexFile> dex_file(DexFile::OpenBase64(kProtoCompareDex));
  ASSERT_TRUE(dex_file != NULL);

  Class* klass = dex_file->LoadClass("LProtoCompare;");
  ASSERT_TRUE(klass != NULL);

  ASSERT_EQ(4U, klass->NumVirtualMethods());

  Method* m1 = klass->GetVirtualMethod(0);
  ASSERT_STREQ("m1", m1->GetName().data());

  Method* m2 = klass->GetVirtualMethod(1);
  ASSERT_STREQ("m2", m2->GetName().data());

  Method* m3 = klass->GetVirtualMethod(2);
  ASSERT_STREQ("m3", m3->GetName().data());

  Method* m4 = klass->GetVirtualMethod(3);
  ASSERT_STREQ("m4", m4->GetName().data());

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

// class ProtoCompare2 {
//     int m1(short x, int y, long z) { return x + y + (int)z; }
//     int m2(short x, int y, long z) { return x + y + (int)z; }
//     int m3(long x, int y, short z) { return (int)x + y + z; }
//     long m4(long x, int y, short z) { return x + y + z; }
// }
static const char kProtoCompare2Dex[] =
  "ZGV4CjAzNQDVUXj687EpyTTDJZEZPA8dEYnDlm0Ir6YgAwAAcAAAAHhWNBIAAAAAAAAAAIwCAAAP"
  "AAAAcAAAAAYAAACsAAAABAAAAMQAAAAAAAAAAAAAAAYAAAD0AAAAAQAAACQBAADcAQAARAEAAN4B"
  "AADmAQAA6QEAAO8BAAD1AQAA+AEAAP4BAAAPAgAAIwIAADcCAAA6AgAAPQIAAEECAABFAgAASQIA"
  "AAEAAAAEAAAABgAAAAcAAAAJAAAACgAAAAIAAAAAAAAAyAEAAAMAAAAAAAAA1AEAAAUAAAABAAAA"
  "yAEAAAoAAAAFAAAAAAAAAAIAAwAAAAAAAgABAAsAAAACAAEADAAAAAIAAAANAAAAAgACAA4AAAAD"
  "AAMAAAAAAAIAAAAAAAAAAwAAAAAAAAAIAAAAAAAAAHICAAAAAAAAAQABAAEAAABNAgAABAAAAHAQ"
  "BQAAAA4ABwAFAAAAAABSAgAABQAAAJAAAwSEUbAQDwAAAAcABQAAAAAAWgIAAAUAAACQAAMEhFGw"
  "EA8AAAAGAAUAAAAAAGICAAAEAAAAhCCwQLBQDwAJAAUAAAAAAGoCAAAFAAAAgXC7UIGCuyAQAAAA"
  "AwAAAAEAAAAEAAAAAwAAAAQAAAABAAY8aW5pdD4AAUkABElKSVMABElTSUoAAUoABEpKSVMAD0xQ"
  "cm90b0NvbXBhcmUyOwASTGphdmEvbGFuZy9PYmplY3Q7ABJQcm90b0NvbXBhcmUyLmphdmEAAVMA"
  "AVYAAm0xAAJtMgACbTMAAm00AAEABw4AAgMAAAAHDgADAwAAAAcOAAQDAAAABw4ABQMAAAAHDgAA"
  "AAEEAICABMQCAQDcAgEA+AIBAJQDAQCsAwwAAAAAAAAAAQAAAAAAAAABAAAADwAAAHAAAAACAAAA"
  "BgAAAKwAAAADAAAABAAAAMQAAAAFAAAABgAAAPQAAAAGAAAAAQAAACQBAAABIAAABQAAAEQBAAAB"
  "EAAAAgAAAMgBAAACIAAADwAAAN4BAAADIAAABQAAAE0CAAAAIAAAAQAAAHICAAAAEAAAAQAAAIwC"
  "AAA=";

TEST(Method, ProtoCompare2) {
  scoped_ptr<DexFile> dex_file1(DexFile::OpenBase64(kProtoCompareDex));
  ASSERT_TRUE(dex_file1 != NULL);
  scoped_ptr<DexFile> dex_file2(DexFile::OpenBase64(kProtoCompare2Dex));
  ASSERT_TRUE(dex_file2 != NULL);

  Class* klass1 = dex_file1->LoadClass("LProtoCompare;");
  ASSERT_TRUE(klass1 != NULL);
  Class* klass2 = dex_file2->LoadClass("LProtoCompare2;");
  ASSERT_TRUE(klass2 != NULL);

  Method* m1_1 = klass1->GetVirtualMethod(0);
  ASSERT_STREQ("m1", m1_1->GetName().data());
  Method* m2_1 = klass1->GetVirtualMethod(1);
  ASSERT_STREQ("m2", m2_1->GetName().data());
  Method* m3_1 = klass1->GetVirtualMethod(2);
  ASSERT_STREQ("m3", m3_1->GetName().data());
  Method* m4_1 = klass1->GetVirtualMethod(3);
  ASSERT_STREQ("m4", m4_1->GetName().data());

  Method* m1_2 = klass2->GetVirtualMethod(0);
  ASSERT_STREQ("m1", m1_2->GetName().data());
  Method* m2_2 = klass2->GetVirtualMethod(1);
  ASSERT_STREQ("m2", m2_2->GetName().data());
  Method* m3_2 = klass2->GetVirtualMethod(2);
  ASSERT_STREQ("m3", m3_2->GetName().data());
  Method* m4_2 = klass2->GetVirtualMethod(3);
  ASSERT_STREQ("m4", m4_2->GetName().data());

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
