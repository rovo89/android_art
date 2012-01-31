/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "object.h"
#include "common_test.h"
#include "utils_llvm.h"

namespace art {

class UtilsLLVMTest : public CommonTest {
};

TEST_F(UtilsLLVMTest, MangleForLLVM) {
  // Unit test inheritted from MangleForJni
  EXPECT_EQ("hello_00024world", MangleForLLVM("hello$world"));
  EXPECT_EQ("hello_000a9world", MangleForLLVM("hello\xc2\xa9world"));
  EXPECT_EQ("hello_1world", MangleForLLVM("hello_world"));
  EXPECT_EQ("Ljava_lang_String_2", MangleForLLVM("Ljava/lang/String;"));
  EXPECT_EQ("_3C", MangleForLLVM("[C"));

  // MangleForLLVM()-specific unit test
  EXPECT_EQ("_0003cinit_0003e", MangleForLLVM("<init>"));
  EXPECT_EQ("_0003cclinit_0003e", MangleForLLVM("<clinit>"));
}

TEST_F(UtilsLLVMTest, LLVMShortName_LLVMLongName) {
  Class* c = class_linker_->FindSystemClass("Ljava/lang/String;");
  ASSERT_TRUE(c != NULL);
  Method* m;

  // Unit test inheritted from MangleForJni
  m = c->FindVirtualMethod("charAt", "(I)C");
  ASSERT_TRUE(m != NULL);
  EXPECT_EQ("Art_java_lang_String_charAt", LLVMShortName(m));
  EXPECT_EQ("Art_java_lang_String_charAt__I", LLVMLongName(m));

  m = c->FindVirtualMethod("indexOf", "(Ljava/lang/String;I)I");
  ASSERT_TRUE(m != NULL);
  EXPECT_EQ("Art_java_lang_String_indexOf", LLVMShortName(m));
  EXPECT_EQ("Art_java_lang_String_indexOf__Ljava_lang_String_2I", LLVMLongName(m));

  m = c->FindDirectMethod("copyValueOf", "([CII)Ljava/lang/String;");
  ASSERT_TRUE(m != NULL);
  EXPECT_EQ("Art_java_lang_String_copyValueOf", LLVMShortName(m));
  EXPECT_EQ("Art_java_lang_String_copyValueOf___3CII", LLVMLongName(m));

  // MangleForLLVM()-specific unit test
  m = c->FindDirectMethod("<init>", "()V");
  ASSERT_TRUE(m != NULL);
  EXPECT_EQ("Art_java_lang_String__0003cinit_0003e", LLVMShortName(m));
  EXPECT_EQ("Art_java_lang_String__0003cinit_0003e__", LLVMLongName(m));

  m = c->FindDirectMethod("<init>", "([C)V");
  ASSERT_TRUE(m != NULL);
  EXPECT_EQ("Art_java_lang_String__0003cinit_0003e", LLVMShortName(m));
  EXPECT_EQ("Art_java_lang_String__0003cinit_0003e___3C", LLVMLongName(m));

  m = c->FindDirectMethod("<init>", "([CII)V");
  ASSERT_TRUE(m != NULL);
  EXPECT_EQ("Art_java_lang_String__0003cinit_0003e", LLVMShortName(m));
  EXPECT_EQ("Art_java_lang_String__0003cinit_0003e___3CII", LLVMLongName(m));

  m = c->FindDirectMethod("<init>", "(Ljava/lang/String;)V");
  ASSERT_TRUE(m != NULL);
  EXPECT_EQ("Art_java_lang_String__0003cinit_0003e", LLVMShortName(m));
  EXPECT_EQ("Art_java_lang_String__0003cinit_0003e__Ljava_lang_String_2", LLVMLongName(m));
}

}  // namespace art
