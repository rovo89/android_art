// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "heap.h"
#include "object.h"
#include "scoped_ptr.h"

#include <stdint.h>
#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

class CompilerTest : public CommonTest {
};

#if defined(__arm__)
TEST_F(CompilerTest, BasicCodegen) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kFibonacciDex,
                               "kFibonacciDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("Fibonacci");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "fibonacci", "(I)I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m, 10);
  LOG(INFO) << "Fibonacci[10] is " << result;

  ASSERT_EQ(55, result);
}
#endif

}  // namespace art
