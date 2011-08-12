// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

class JniInternalTest : public RuntimeTest {
 protected:
  virtual void SetUp() {
    RuntimeTest::SetUp();
    env_ = Thread::Current()->GetJniEnv();
  }
  JNIEnv* env_;
};

TEST_F(JniInternalTest, GetVersion) {
  ASSERT_EQ(JNI_VERSION_1_6, env_->GetVersion());
}

#define EXPECT_CLASS_FOUND(NAME) \
  EXPECT_TRUE(env_->FindClass(NAME) != NULL)

#define EXPECT_CLASS_NOT_FOUND(NAME) \
  EXPECT_TRUE(env_->FindClass(NAME) == NULL)

TEST_F(JniInternalTest, FindClass) {
  // TODO: when these tests start failing because you're calling FindClass
  // with a pending exception, fix EXPECT_CLASS_NOT_FOUND to assert that an
  // exception was thrown and clear the exception.

  // TODO: . is only allowed as an alternative to / if CheckJNI is off.

  // Reference types...
  // You can't include the "L;" in a JNI class descriptor.
  EXPECT_CLASS_FOUND("java/lang/String");
  EXPECT_CLASS_NOT_FOUND("Ljava/lang/String;");
  // We support . as well as / for compatibility.
  EXPECT_CLASS_FOUND("java.lang.String");
  EXPECT_CLASS_NOT_FOUND("Ljava.lang.String;");
  // ...for arrays too, where you must include "L;".
  EXPECT_CLASS_FOUND("[Ljava/lang/String;");
  EXPECT_CLASS_NOT_FOUND("[java/lang/String");
  EXPECT_CLASS_FOUND("[Ljava.lang.String;");
  EXPECT_CLASS_NOT_FOUND("[java.lang.String");

  // Primitive arrays are okay (if the primitive type is valid)...
  EXPECT_CLASS_FOUND("[C");
  EXPECT_CLASS_NOT_FOUND("[K");
  // But primitive types aren't allowed...
  EXPECT_CLASS_NOT_FOUND("C");
  EXPECT_CLASS_NOT_FOUND("K");
}

}  // namespace art
