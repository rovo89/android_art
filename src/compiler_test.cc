// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_linker.h"
#include "common_test.h"
#include "compiler.h"
#include "compiler_test.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "heap.h"
#include "object.h"
#include "scoped_ptr.h"

#include <stdint.h>
#include <stdio.h>

namespace art {

class CompilerTest : public CommonTest {
 protected:
  void CompileDex(const char* base64_dex, const char* base64_name) {
    dex_file_.reset(OpenDexFileBase64(base64_dex, base64_name));
    class_linker_->RegisterDexFile(*dex_file_.get());
    std::vector<const DexFile*> class_path;
    class_path.push_back(dex_file_.get());
    Compiler compiler;
    const ClassLoader* class_loader = compiler.Compile(class_path);
    Thread::Current()->SetClassLoaderOverride(class_loader);
  }

  void AssertStaticIntMethod(const char* klass, const char* method, const char* signature,
                             jint expected, ...) {
    JNIEnv* env = Thread::Current()->GetJniEnv();
    jclass c = env->FindClass(klass);
    CHECK(c != NULL);
    jmethodID m = env->GetStaticMethodID(c, method, signature);
    CHECK(m != NULL);
#if defined(__arm__)
    va_list args;
    va_start(args, expected);
    jint result = env->CallStaticIntMethodV(c, m, args);
    va_end(args);
    LOG(INFO) << klass << "." << method << "(...) result is " << result;
    EXPECT_EQ(expected, result);
#endif // __arm__
  }
 private:
  scoped_ptr<DexFile> dex_file_;
};

TEST_F(CompilerTest, CompileDexLibCore) {
  // TODO renenable when compiler can handle libcore
  if (true) {
    return;
  }
  Compiler compiler;
  compiler.Compile(boot_class_path_);

  // All libcore references should resolve
  const DexFile* dex = java_lang_dex_file_.get();
  DexCache* dex_cache = class_linker_->FindDexCache(*dex);
  EXPECT_EQ(dex->NumStringIds(), dex_cache->NumStrings());
  for (size_t i = 0; i < dex_cache->NumStrings(); i++) {
    String* string = dex_cache->GetResolvedString(i);
    EXPECT_TRUE(string != NULL);
  }
  EXPECT_EQ(dex->NumTypeIds(), dex_cache->NumTypes());
  for (size_t i = 0; i < dex_cache->NumTypes(); i++) {
    Class* type = dex_cache->GetResolvedType(i);
    EXPECT_TRUE(type != NULL);
  }
  EXPECT_EQ(dex->NumMethodIds(), dex_cache->NumMethods());
  for (size_t i = 0; i < dex_cache->NumMethods(); i++) {
    // TODO: ClassLinker::ResolveMethod
    // Method* method = dex_cache->GetResolvedMethod(i);
    // EXPECT_TRUE(method != NULL);
  }
  EXPECT_EQ(dex->NumFieldIds(), dex_cache->NumFields());
  for (size_t i = 0; i < dex_cache->NumFields(); i++) {
    // TODO: ClassLinker::ResolveField
    // Field* field = dex_cache->GetResolvedField(i);
    // EXPECT_TRUE(field != NULL);
  }

}

TEST_F(CompilerTest, BasicCodegen) {
  CompileDex(kFibonacciDex, "kFibonacciDex");
  AssertStaticIntMethod("Fibonacci", "fibonacci", "(I)I", 55,
                        10);
}

TEST_F(CompilerTest, UnopTest) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "unopTest", "(I)I", 37,
                        38);
}

#if 0 // Does filled array - needs load-time class resolution
TEST_F(CompilerTest, ShiftTest1) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "shiftTest1", "()I", 0);
}
#endif

TEST_F(CompilerTest, ShiftTest2) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "shiftTest2", "()I", 0);
}

TEST_F(CompilerTest, UnsignedShiftTest) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "unsignedShiftTest", "()I", 0);
}

#if 0 // Fail subtest #3, long to int conversion w/ truncation.
TEST_F(CompilerTest, ConvTest) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "convTest", "()I", 0);
}
#endif

TEST_F(CompilerTest, CharSubTest) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "charSubTest", "()I", 0);
}

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, IntOperTest) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "intOperTest", "(II)I", 0,
                        70000, -3);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, Lit16Test) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "lit16Test", "(I)I", 0,
                        77777);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, Lit8Test) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "lit8Test", "(I)I", 0,
                        -55555);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, IntShiftTest) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "intShiftTest", "(II)I", 0,
                        0xff00aa01, 8);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, LongOperTest) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "longOperTest", "(JJ)I", 0,
                        70000000000LL, 3);
}
#endif

#if 0 // Needs array allocation & r9 to be set up with Thread*
TEST_F(CompilerTest, LongShiftTest) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "longShiftTest", "(JJ)I", 0,
                        0xd5aa96deff00aa01LL, 8);
}
#endif

TEST_F(CompilerTest, SwitchTest1) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "switchTest", "(I)I", 1234,
                        1);
}

TEST_F(CompilerTest, IntCompare) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "testIntCompare", "(IIII)I", 1111,
                        -5, 4, 4, 0);
}

TEST_F(CompilerTest, LongCompare) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "testLongCompare", "(JJJJ)I", 2222,
                        -5LL, -4294967287LL, 4LL, 8LL);
}

#if 0
TEST_F(CompilerTest, FloatCompare) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "testFloatCompare", "(FFFF)I", 3333
                        -5.0f, 4.0f, 4.0f,
                        (1.0f/0.0f) / (1.0f/0.0f));
}
#endif

TEST_F(CompilerTest, DoubleCompare) {
  CompileDex(kIntMathDex, "kIntMathDex");
  AssertStaticIntMethod("IntMath", "testDoubleCompare", "(DDDD)I", 4444,
                                    -5.0, 4.0, 4.0,
                                    (1.0/0.0) / (1.0/0.0));
}

}  // namespace art
