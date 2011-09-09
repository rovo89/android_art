// Copyright 2011 Google Inc. All Rights Reserved.

#include "compiler.h"

#include <stdint.h>
#include <stdio.h>

#include "UniquePtr.h"
#include "class_linker.h"
#include "common_test.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "heap.h"
#include "object.h"

namespace art {

class CompilerTest : public CommonTest {
 protected:

  void AssertStaticIntMethod(const ClassLoader* class_loader,
                             const char* class_name, const char* method, const char* signature,
                             jint expected, ...) {
    EnsureCompiled(class_loader, class_name, method, signature);
#if defined(__arm__)
    va_list args;
    va_start(args, expected);
    jint result = env_->CallStaticIntMethodV(class_, mid_, args);
    va_end(args);
    LOG(INFO) << class_name << "." << method << "(...) result is " << result;
    EXPECT_EQ(expected, result);
#endif // __arm__
  }

  void AssertStaticLongMethod(const ClassLoader* class_loader,
                              const char* class_name, const char* method, const char* signature,
                              jlong expected, ...) {
    EnsureCompiled(class_loader, class_name, method, signature);
#if defined(__arm__)
    va_list args;
    va_start(args, expected);
    jlong result = env_->CallStaticLongMethodV(class_, mid_, args);
    va_end(args);
    LOG(INFO) << class_name << "." << method << "(...) result is " << result;
    EXPECT_EQ(expected, result);
#endif // __arm__
  }

  void CompileAll(const ClassLoader* class_loader) {
    compiler_->CompileAll(class_loader);
    MakeAllExecutable(class_loader);
  }

  void EnsureCompiled(const ClassLoader* class_loader,
      const char* class_name, const char* method, const char* signature) {
    CompileAll(class_loader);
    env_ = Thread::Current()->GetJniEnv();
    class_ = env_->FindClass(class_name);
    CHECK(class_ != NULL) << "Class not found: " << class_name;
    mid_ = env_->GetStaticMethodID(class_, method, signature);
    CHECK(mid_ != NULL) << "Method not found: " << class_name << "." << method << signature;
  }

  void MakeAllExecutable(const ClassLoader* class_loader) {
    const std::vector<const DexFile*>& class_path = ClassLoader::GetClassPath(class_loader);
    for (size_t i = 0; i != class_path.size(); ++i) {
      const DexFile* dex_file = class_path[i];
      CHECK(dex_file != NULL);
      MakeDexFileExecutable(class_loader, *dex_file);
    }
  }

  void MakeDexFileExecutable(const ClassLoader* class_loader, const DexFile& dex_file) {
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    for (size_t i = 0; i < dex_file.NumClassDefs(); i++) {
      const DexFile::ClassDef& class_def = dex_file.GetClassDef(i);
      const char* descriptor = dex_file.GetClassDescriptor(class_def);
      Class* c = class_linker->FindClass(descriptor, class_loader);
      CHECK(c != NULL);
      for (size_t i = 0; i < c->NumDirectMethods(); i++) {
        MakeMethodExecutable(c->GetDirectMethod(i));
      }
      for (size_t i = 0; i < c->NumVirtualMethods(); i++) {
        MakeMethodExecutable(c->GetVirtualMethod(i));
      }
    }
  }

  void MakeMethodExecutable(Method* m) {
    if (m->GetCodeArray() != NULL) {
      MakeExecutable(m->GetCodeArray());
    } else {
      LOG(WARNING) << "no code for " << PrettyMethod(m);
    }
    if (m->GetInvokeStubArray() != NULL) {
      MakeExecutable(m->GetInvokeStubArray());
    } else {
      LOG(WARNING) << "no invoke stub for " << PrettyMethod(m);
    }
  }

  JNIEnv* env_;
  jclass class_;
  jmethodID mid_;
};

// Disabled due to 10 second runtime on host
TEST_F(CompilerTest, DISABLED_LARGE_CompileDexLibCore) {
  CompileAll(NULL);

  // All libcore references should resolve
  const DexFile* dex = java_lang_dex_file_.get();
  DexCache* dex_cache = class_linker_->FindDexCache(*dex);
  EXPECT_EQ(dex->NumStringIds(), dex_cache->NumStrings());
  for (size_t i = 0; i < dex_cache->NumStrings(); i++) {
    const String* string = dex_cache->GetResolvedString(i);
    EXPECT_TRUE(string != NULL) << "string_idx=" << i;
  }
  EXPECT_EQ(dex->NumTypeIds(), dex_cache->NumResolvedTypes());
  for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
    Class* type = dex_cache->GetResolvedType(i);
    EXPECT_TRUE(type != NULL) << "type_idx=" << i
                              << " " << dex->GetTypeDescriptor(dex->GetTypeId(i));
  }
  EXPECT_EQ(dex->NumMethodIds(), dex_cache->NumResolvedMethods());
  for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
    Method* method = dex_cache->GetResolvedMethod(i);
    EXPECT_TRUE(method != NULL) << "method_idx=" << i
                                << " " << dex->GetMethodClassDescriptor(dex->GetMethodId(i))
                                << " " << dex->GetMethodName(dex->GetMethodId(i));
  }
  EXPECT_EQ(dex->NumFieldIds(), dex_cache->NumResolvedFields());
  for (size_t i = 0; i < dex_cache->NumResolvedFields(); i++) {
    Field* field = dex_cache->GetResolvedField(i);
    EXPECT_TRUE(field != NULL) << "field_idx=" << i
                               << " " << dex->GetFieldClassDescriptor(dex->GetFieldId(i))
                               << " " << dex->GetFieldName(dex->GetFieldId(i));
  }

  // TODO check Class::IsVerified for all classes

  // TODO: check that all Method::GetCode() values are non-null

  EXPECT_EQ(dex->NumMethodIds(), dex_cache->NumCodeAndDirectMethods());
  CodeAndDirectMethods* code_and_direct_methods = dex_cache->GetCodeAndDirectMethods();
  for (size_t i = 0; i < dex_cache->NumCodeAndDirectMethods(); i++) {
    Method* method = dex_cache->GetResolvedMethod(i);
    if (method->IsDirect()) {
      EXPECT_EQ(method->GetCode(), code_and_direct_methods->GetResolvedCode(i));
      EXPECT_EQ(method,            code_and_direct_methods->GetResolvedMethod(i));
    } else {
      EXPECT_EQ(0U, code_and_direct_methods->GetResolvedCode(i));
      EXPECT_TRUE(code_and_direct_methods->GetResolvedMethod(i) == NULL);
    }
  }
}

TEST_F(CompilerTest, BasicCodegen) {
  AssertStaticIntMethod(LoadDex("Fibonacci"), "Fibonacci", "fibonacci", "(I)I", 55, 10);
}

// TODO: need stub for InstanceofNonTrivialFromCode
TEST_F(CompilerTest, InstanceTest) {
  CompileDirectMethod(NULL, "java.lang.Object", "<init>", "()V");
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "instanceTest", "(I)I", 1352, 10);
}

// TODO: need check-cast test (when stub complete & we can throw/catch

// TODO: Need invoke-interface test

TEST_F(CompilerTest, SuperTest) {
  CompileDirectMethod(NULL, "java.lang.Object", "<init>", "()V");
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "superTest", "(I)I", 4175, 4141);
}

TEST_F(CompilerTest, ConstStringTest) {
  CompileDirectMethod(NULL, "java.lang.String", "<clinit>", "()V");
  CompileDirectMethod(NULL, "java.lang.String", "<init>", "(II[C)V");
  CompileDirectMethod(NULL, "java.lang.String", "<init>", "([CII)V");
  CompileVirtualMethod(NULL, "java.lang.String", "_getChars", "(II[CI)V");
  CompileVirtualMethod(NULL, "java.lang.String", "charAt", "(I)C");
  CompileVirtualMethod(NULL, "java.lang.String", "length", "()I");
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "constStringTest", "(I)I", 1246, 1234);
}

TEST_F(CompilerTest, ConstClassTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "constClassTest", "(I)I", 2222, 1111);
}

// TODO: Need native nativeFillInStackTrace()
TEST_F(CompilerTest, DISABLED_CatchTest) {
  CompileDirectMethod(NULL, "java.lang.Object", "<init>", "()V");
  CompileDirectMethod(NULL, "java.lang.NullPointerException", "<init>", "()V");
  CompileDirectMethod(NULL, "java.lang.RuntimeException", "<init>", "()V");
  CompileDirectMethod(NULL, "java.lang.Exception", "<init>", "()V");
  CompileDirectMethod(NULL, "java.lang.Throwable","<init>", "()V");
  CompileDirectMethod(NULL, "java.util.ArrayList","<init>","()V");
  CompileDirectMethod(NULL, "java.util.AbstractList","<init>","()V");
  CompileDirectMethod(NULL, "java.util.AbstractCollection","<init>","()V");
  CompileVirtualMethod(NULL, "java.lang.Throwable","fillInStackTrace","()Ljava/lang/Throwable;");
  CompileDirectMethod(NULL, "java.lang.Throwable","nativeFillInStackTrace","()Ljava/lang/Object;");
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "catchBlock", "(I)I", 1579, 1000);
}

TEST_F(CompilerTest, CatchTestNoThrow) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "catchBlockNoThrow", "(I)I", 1123, 1000);
}

TEST_F(CompilerTest, StaticFieldTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "staticFieldTest", "(I)I", 1404, 404);
}

TEST_F(CompilerTest, UnopTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "unopTest", "(I)I", 37, 38);
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
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "intOperTest", "(II)I", 0, 70000, -3);
}

TEST_F(CompilerTest, Lit16Test) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "lit16Test", "(I)I", 0, 77777);
}

TEST_F(CompilerTest, Lit8Test) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "lit8Test", "(I)I", 0, -55555);
}

TEST_F(CompilerTest, IntShiftTest) {
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "intShiftTest", "(II)I", 0, 0xff00aa01, 8);
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
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "switchTest", "(I)I", 1234, 1);
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
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "fibonacci", "(I)I", 55, 10);
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
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "staticCall", "(I)I", 6, 3);
}

TEST_F(CompilerTest, TestIGetPut) {
  CompileDirectMethod(NULL, "java.lang.Object", "<init>", "()V");
  AssertStaticIntMethod(LoadDex("IntMath"), "IntMath", "testIGetPut", "(I)I", 333, 111);
}

TEST_F(CompilerTest, InvokeTest) {
  CompileDirectMethod(NULL, "java.lang.Object", "<init>", "()V");
  AssertStaticIntMethod(LoadDex("Invoke"), "Invoke", "test0", "(I)I", 20664, 912);
}

TEST_F(CompilerTest, DISABLED_LARGE_SystemMethodsTest) {
  CompileAll(NULL); // This test calls a bunch of stuff from libcore.
  AssertStaticIntMethod(LoadDex("SystemMethods"), "SystemMethods", "test5", "()I", 123);
}

}  // namespace art
