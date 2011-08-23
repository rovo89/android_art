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

TEST_F(CompilerTest, UnopTest) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "unopTest", "(I)I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m, 38);
  LOG(INFO) << "unopTest(38) == " << result;

  ASSERT_EQ(37, result);
}

#if 0 // Does filled array - needs load-time class resolution
TEST_F(CompilerTest, ShiftTest1) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "shiftTest1", "()I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m);

  ASSERT_EQ(0, result);
}
#endif

#if 0 // Fails, needs 64-bit shift helper functions
TEST_F(CompilerTest, ShiftTest2) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "shiftTest2", "()I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m);

  ASSERT_EQ(0, result);
}
#endif

TEST_F(CompilerTest, UnsignedShiftTest) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "unsignedShiftTest", "()I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m);

  ASSERT_EQ(0, result);
}

#if 0 // Fail subtest #3, long to int conversion w/ truncation.
TEST_F(CompilerTest, ConvTest) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "convTest", "()I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m);

  ASSERT_EQ(0, result);
}
#endif

TEST_F(CompilerTest, CharSubTest) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "charSubTest", "()I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m);

  ASSERT_EQ(0, result);
}

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, IntOperTest) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "intOperTest", "(II)I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m, 70000, -3);

  ASSERT_EQ(0, result);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, Lit16Test) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "lit16Test", "(I)I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m, 77777);

  ASSERT_EQ(0, result);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, Lit8Test) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "lit8Test", "(I)I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m, -55555);

  ASSERT_EQ(0, result);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, Lit8Test) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "lit8Test", "(I)I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m, -55555);

  ASSERT_EQ(0, result);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, IntShiftTest) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "intShiftTest", "(II)I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m, 0xff00aa01, 8);

  ASSERT_EQ(0, result);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, LongOperTest) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "longOperTest", "(LL)I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m, 70000000000L, 3);

  ASSERT_EQ(0, result);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, LongShiftTest) {
  scoped_ptr<DexFile> dex_file(OpenDexFileBase64(kIntMathDex,
                               "kIntMathDex"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex_file.get());

  Thread::Current()->SetClassLoaderOverride(class_loader);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  jclass c = env->FindClass("IntMath");
  ASSERT_TRUE(c != NULL);

  jmethodID m = env->GetStaticMethodID(c, "longShiftTest", "(LL)I");
  ASSERT_TRUE(m != NULL);

  jint result = env->CallStaticIntMethod(c, m, 0xd5aa96deff00aa01, 8);

  ASSERT_EQ(0, result);
}
#endif

#endif // Arm
}  // namespace art
