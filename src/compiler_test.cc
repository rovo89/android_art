// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_linker.h"
#include "common_test.h"
#include "compiler.h"
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

  const ClassLoader* LoadDex(const char* dex_name) {
    dex_file_.reset(OpenTestDexFile(dex_name));
    class_linker_->RegisterDexFile(*dex_file_.get());
    std::vector<const DexFile*> class_path;
    class_path.push_back(dex_file_.get());
    const ClassLoader* class_loader = PathClassLoader::Alloc(class_path);
    Thread::Current()->SetClassLoaderOverride(class_loader);
    return class_loader;
  }


  void CompileDex(const char* dex_name) {
    Compile(LoadDex(dex_name));
  }

  void CompileSystem() {
    Compile(NULL);
  }

  void Compile(const ClassLoader* class_loader) {
    Compiler compiler;
    compiler.CompileAll(NULL);
  }

  std::string ConvertClassNameToClassDescriptor(const char* class_name) {
    std::string desc;
    desc += "L";
    desc += class_name;
    desc += ";";
    std::replace(desc.begin(), desc.end(), '.', '/');
    return desc;
  }


  void CompileDirectMethod(const ClassLoader* class_loader,
                           const char* class_name,
                           const char* method_name,
                           const char* signature) {
    std::string class_descriptor = ConvertClassNameToClassDescriptor(class_name);
    Class* klass = class_linker_->FindClass(class_descriptor, class_loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    Method* method = klass->FindDirectMethod(method_name, signature);
    CHECK(method != NULL) << "Method not found " << method_name;
    Compiler compiler;
    compiler.CompileOne(method);
  }

  void CompileVirtualMethod(const ClassLoader* class_loader,
                            const char* class_name,
                            const char* method_name,
                            const char* signature) {
    std::string class_descriptor = ConvertClassNameToClassDescriptor(class_name);
    Class* klass = class_linker_->FindClass(class_descriptor, class_loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    Method* method = klass->FindVirtualMethod(method_name, signature);
    CHECK(method != NULL) << "Method not found " << method_name;
    Compiler compiler;
    compiler.CompileOne(method);
  }

  void AssertStaticIntMethod(const ClassLoader* class_loader,
                             const char* klass, const char* method, const char* signature,
                             jint expected, ...) {
    CompileDirectMethod(class_loader, klass, method, signature);
    JNIEnv* env = Thread::Current()->GetJniEnv();
    jclass c = env->FindClass(klass);
    CHECK(c != NULL) << "Class not found " << klass;
    jmethodID m = env->GetStaticMethodID(c, method, signature);
    CHECK(m != NULL) << "Method not found " << method;
#if defined(__arm__)
    va_list args;
    va_start(args, expected);
    jint result = env->CallStaticIntMethodV(c, m, args);
    va_end(args);
    LOG(INFO) << klass << "." << method << "(...) result is " << result;
    EXPECT_EQ(expected, result);
#endif // __arm__
  }
  void AssertStaticLongMethod(const ClassLoader* class_loader,
                              const char* klass, const char* method, const char* signature,
                              jlong expected, ...) {
    CompileDirectMethod(class_loader, klass, method, signature);
    JNIEnv* env = Thread::Current()->GetJniEnv();
    jclass c = env->FindClass(klass);
    CHECK(c != NULL) << "Class not found " << klass;
    jmethodID m = env->GetStaticMethodID(c, method, signature);
    CHECK(m != NULL) << "Method not found " << method;
#if defined(__arm__)
    va_list args;
    va_start(args, expected);
    jlong result = env->CallStaticLongMethodV(c, m, args);
    va_end(args);
    LOG(INFO) << klass << "." << method << "(...) result is " << result;
    EXPECT_EQ(expected, result);
#endif // __arm__
  }
 private:
  scoped_ptr<const DexFile> dex_file_;
};

// TODO renenable when compiler can handle libcore
TEST_F(CompilerTest, DISABLED_CompileDexLibCore) {
  Compiler compiler;
  compiler.CompileAll(NULL);

  // All libcore references should resolve
  const DexFile* dex = java_lang_dex_file_.get();
  DexCache* dex_cache = class_linker_->FindDexCache(*dex);
  EXPECT_EQ(dex->NumStringIds(), dex_cache->NumStrings());
  for (size_t i = 0; i < dex_cache->NumStrings(); i++) {
    String* string = dex_cache->GetResolvedString(i);
    EXPECT_TRUE(string != NULL);
  }
  EXPECT_EQ(dex->NumTypeIds(), dex_cache->NumResolvedTypes());
  for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
    Class* type = dex_cache->GetResolvedType(i);
    EXPECT_TRUE(type != NULL);
  }
  EXPECT_EQ(dex->NumMethodIds(), dex_cache->NumResolvedMethods());
  for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
    Method* method = dex_cache->GetResolvedMethod(i);
    EXPECT_TRUE(method != NULL);
  }
  EXPECT_EQ(dex->NumFieldIds(), dex_cache->NumResolvedFields());
  for (size_t i = 0; i < dex_cache->NumResolvedFields(); i++) {
    Field* field = dex_cache->GetResolvedField(i);
    EXPECT_TRUE(field != NULL);
  }

  // TODO check Class::IsVerified for all classes

  // TODO: check that all Method::GetCode() values are non-null

  EXPECT_EQ(dex->NumMethodIds(), dex_cache->NumCodeAndDirectMethods());
  CodeAndDirectMethods* code_and_direct_methods = dex_cache->GetCodeAndDirectMethods();
  for (size_t i = 0; i < dex_cache->NumCodeAndDirectMethods(); i++) {
    Method* method = dex_cache->GetResolvedMethod(i);
    EXPECT_EQ(method->GetCode(), code_and_direct_methods->GetResolvedCode(i));
    EXPECT_EQ(method,            code_and_direct_methods->GetResolvedMethod(i));
  }
}

TEST_F(CompilerTest, BasicCodegen) {
  AssertStaticIntMethod(LoadDex("Fibonacci"), "Fibonacci", "fibonacci", "(I)I", 55,
                        10);
}

TEST_F(CompilerTest, StaticFieldTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "staticFieldTest", "(I)I", 1404,
                        404);
}

TEST_F(CompilerTest, UnopTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "unopTest", "(I)I", 37,
                        38);
}

TEST_F(CompilerTest, ShiftTest1) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "shiftTest1", "()I", 0);
}

TEST_F(CompilerTest, ShiftTest2) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "shiftTest2", "()I", 0);
}

TEST_F(CompilerTest, UnsignedShiftTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "unsignedShiftTest", "()I", 0);
}

TEST_F(CompilerTest, ConvTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "convTest", "()I", 0);
}

TEST_F(CompilerTest, CharSubTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "charSubTest", "()I", 0);
}

TEST_F(CompilerTest, IntOperTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "intOperTest", "(II)I", 0,
                        70000, -3);
}

TEST_F(CompilerTest, Lit16Test) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "lit16Test", "(I)I", 0,
                        77777);
}

TEST_F(CompilerTest, Lit8Test) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "lit8Test", "(I)I", 0,
                        -55555);
}

TEST_F(CompilerTest, IntShiftTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "intShiftTest", "(II)I", 0,
                        0xff00aa01, 8);
}

TEST_F(CompilerTest, LongOperTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "longOperTest", "(JJ)I", 0,
                        70000000000LL, -3LL);
}

TEST_F(CompilerTest, LongShiftTest) {
  AssertStaticLongMethod(LoadDex("IntMath"), "IntMath", "longShiftTest", "(JI)J",
                         0x96deff00aa010000LL, 0xd5aa96deff00aa01LL, 16);
}

TEST_F(CompilerTest, SwitchTest1) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "switchTest", "(I)I", 1234,
                        1);
}

TEST_F(CompilerTest, IntCompare) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "testIntCompare", "(IIII)I", 1111,
                        -5, 4, 4, 0);
}

TEST_F(CompilerTest, LongCompare) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "testLongCompare", "(JJJJ)I", 2222,
                        -5LL, -4294967287LL, 4LL, 8LL);
}

TEST_F(CompilerTest, FloatCompare) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "testFloatCompare", "(FFFF)I", 3333,
                        -5.0f, 4.0f, 4.0f,
                        (1.0f/0.0f) / (1.0f/0.0f));
}

TEST_F(CompilerTest, DoubleCompare) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "testDoubleCompare", "(DDDD)I", 4444,
                                    -5.0, 4.0, 4.0,
                                    (1.0/0.0) / (1.0/0.0));
}

TEST_F(CompilerTest, RecursiveFibonacci) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "fibonacci", "(I)I", 55,
                        10);
}

#if 0 // Need to complete try/catch block handling
TEST_F(CompilerTest, ThrowAndCatch) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "throwAndCatch", "()I", 4);
}
#endif

TEST_F(CompilerTest, ManyArgs) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "manyArgs",
                        "(IJIJIJIIDFDSICIIBZIIJJIIIII)I", -1,
                        0, 1LL, 2, 3LL, 4, 5LL, 6, 7, 8.0, 9.0f, 10.0,
                        (short)11, 12, (char)13, 14, 15, (int8_t)-16, true, 18,
                        19, 20LL, 21LL, 22, 23, 24, 25, 26);
}

TEST_F(CompilerTest, VirtualCall) {
  CompileDirectMethod(NULL, "java.lang.Object", "<init>", "()V");
  const ClassLoader* class_loader = LoadDex("IntMath");
  CompileDirectMethod(class_loader, "IntMath", "<init>", "()V");
  CompileVirtualMethod(class_loader, "IntMath", "virtualCall", "(I)I");
  AssertStaticIntMethod(class_loader, "IntMath", "staticCall", "(I)I", 6,
                        3);
}

TEST_F(CompilerTest, TestIGetPut) {
  CompileDirectMethod(NULL, "java.lang.Object", "<init>", "()V");
  const ClassLoader* class_loader = LoadDex("IntMath");
  CompileDirectMethod(class_loader, "IntMath", "<init>", "(I)V");
  CompileDirectMethod(class_loader, "IntMath", "<init>", "()V");
  CompileVirtualMethod(class_loader, "IntMath", "getFoo", "()I");
  CompileVirtualMethod(class_loader, "IntMath", "setFoo", "(I)V");
  AssertStaticIntMethod(class_loader, "IntMath", "testIGetPut", "(I)I", 333,
                        111);
}

}  // namespace art
