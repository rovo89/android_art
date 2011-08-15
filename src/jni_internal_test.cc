// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include <cmath>
#include <sys/mman.h>

#include "common_test.h"
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

TEST_F(JniInternalTest, NewPrimitiveArray) {
  // TODO: death tests for negative array sizes.

  // TODO: check returned array size.

  // TODO: check returned array class.

  CHECK(env_->NewBooleanArray(0) != NULL);
  CHECK(env_->NewByteArray(0) != NULL);
  CHECK(env_->NewCharArray(0) != NULL);
  CHECK(env_->NewDoubleArray(0) != NULL);
  CHECK(env_->NewFloatArray(0) != NULL);
  CHECK(env_->NewIntArray(0) != NULL);
  CHECK(env_->NewLongArray(0) != NULL);
  CHECK(env_->NewShortArray(0) != NULL);

  CHECK(env_->NewBooleanArray(1) != NULL);
  CHECK(env_->NewByteArray(1) != NULL);
  CHECK(env_->NewCharArray(1) != NULL);
  CHECK(env_->NewDoubleArray(1) != NULL);
  CHECK(env_->NewFloatArray(1) != NULL);
  CHECK(env_->NewIntArray(1) != NULL);
  CHECK(env_->NewLongArray(1) != NULL);
  CHECK(env_->NewShortArray(1) != NULL);
}

TEST_F(JniInternalTest, NewObjectArray) {
  // TODO: death tests for negative array sizes.

  // TODO: check returned array size.

  // TODO: check returned array class.

  // TODO: check non-NULL initial elements.

  jclass c = env_->FindClass("[Ljava.lang.String;");

  CHECK(env_->NewObjectArray(0, c, NULL) != NULL);

  CHECK(env_->NewObjectArray(1, c, NULL) != NULL);
}

bool EnsureInvokeStub(Method* method);

byte* AllocateCode(void* code, size_t length) {
  int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
  void* addr = mmap(NULL, length, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(addr != MAP_FAILED);
  memcpy(addr, code, length);
  __builtin___clear_cache(addr, (byte*)addr + length);
  // Set the low-order bit so a BLX will switch to Thumb mode.
  return reinterpret_cast<byte*>(reinterpret_cast<uintptr_t>(addr) | 1);
}

Method::InvokeStub* AllocateStub(Method* method,
                                 byte* code,
                                 size_t length) {
  CHECK(method->GetInvokeStub() == NULL);
  EnsureInvokeStub(method);
  Method::InvokeStub* stub = method->GetInvokeStub();
  CHECK(stub != NULL);
  method->SetCode(AllocateCode(code, length));
  CHECK(method->GetCode() != NULL);
  return stub;
}

void FreeStub(Method* method, size_t length) {
  void* addr = const_cast<void*>(method->GetCode());
  munmap(addr, length);
  method->SetCode(NULL);
}

#if defined(__arm__)
TEST_F(JniInternalTest, StaticMainMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMainDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LMain;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("main", "([Ljava/lang/String;)V");
  ASSERT_TRUE(method != NULL);

  byte main_LV_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x83, 0xb0, 0xcd, 0xf8, 0x00, 0x00,
    0xcd, 0xf8, 0x14, 0x10, 0x03, 0xb0, 0xbd, 0xe8, 0x00, 0x80,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          main_LV_code,
                                          sizeof(main_LV_code));

  Object* arg = NULL;

  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), NULL);

  FreeStub(method, sizeof(main_LV_code));
}

TEST_F(JniInternalTest, StaticNopMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("nop", "()V");
  ASSERT_TRUE(method != NULL);

  byte nop_V_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x83, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0x03, 0xb0, 0xbd, 0xe8, 0x00, 0x80,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          nop_V_code,
                                          sizeof(nop_V_code));
  ASSERT_TRUE(stub);

  (*stub)(method, NULL, NULL, NULL, NULL);

  FreeStub(method, sizeof(nop_V_code));
}

TEST_F(JniInternalTest, StaticIdentityByteMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("identity", "(B)B");
  ASSERT_TRUE(method != NULL);

  byte identity_BB_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x83, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x14, 0x10, 0x05, 0x98,
    0x03, 0xb0, 0xbd, 0xe8, 0x00, 0x80, 0x00, 0x00,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          identity_BB_code,
                                          sizeof(identity_BB_code));

  int arg;
  JValue result;

  arg = 0;
  result.b = -1;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(0, result.b);

  arg = -1;
  result.b = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(-1, result.b);

  arg = SCHAR_MAX;
  result.b = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(SCHAR_MAX, result.b);

  arg = SCHAR_MIN;
  result.b = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(SCHAR_MIN, result.b);

  FreeStub(method, sizeof(identity_BB_code));
}

TEST_F(JniInternalTest, StaticIdentityIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("identity", "(I)I");
  ASSERT_TRUE(method != NULL);

  byte identity_II_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x83, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x14, 0x10, 0x05, 0x98,
    0x03, 0xb0, 0xbd, 0xe8, 0x00, 0x80, 0x00, 0x00,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          identity_II_code,
                                          sizeof(identity_II_code));

  int arg;
  JValue result;

  arg = 0;
  result.i = -1;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(0, result.i);

  arg = -1;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(-1, result.i);

  arg = INT_MAX;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(INT_MAX, result.i);

  arg = INT_MIN;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(INT_MIN, result.i);

  FreeStub(method, sizeof(identity_II_code));
}

TEST_F(JniInternalTest, StaticIdentityDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("identity", "(D)D");
  ASSERT_TRUE(method != NULL);

  byte identity_DD_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x83, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x14, 0x10, 0xcd, 0xf8,
    0x18, 0x20, 0x05, 0x98, 0x06, 0x99, 0x03, 0xb0,
    0xbd, 0xe8, 0x00, 0x80,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          identity_DD_code,
                                          sizeof(identity_DD_code));

  double arg;
  JValue result;

  arg = 0.0;
  result.d = -1.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(0.0, result.d);

  arg = -1.0;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(-1.0, result.d);

  arg = DBL_MAX;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(DBL_MAX, result.d);

  arg = DBL_MIN;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(DBL_MIN, result.d);

  FreeStub(method, sizeof(identity_DD_code));
}

TEST_F(JniInternalTest, StaticSumIntIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(II)I");
  ASSERT_TRUE(method != NULL);

  byte sum_III_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x83, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x14, 0x10, 0xcd, 0xf8,
    0x18, 0x20, 0x05, 0x98, 0x06, 0x99, 0x42, 0x18,
    0xcd, 0xf8, 0x04, 0x20, 0x01, 0x98, 0x03, 0xb0,
    0xbd, 0xe8, 0x00, 0x80,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          sum_III_code,
                                          sizeof(sum_III_code));

  int args[2];
  JValue result;

  args[0] = 0;
  args[1] = 0;
  result.i = -1;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0, result.i);

  args[0] = 1;
  args[1] = 2;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(3, result.i);

  args[0] = -2;
  args[1] = 5;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(3, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MIN;
  result.i = 1234;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-1, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MAX;
  result.i = INT_MIN;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-2, result.i);

  FreeStub(method, sizeof(sum_III_code));
}

TEST_F(JniInternalTest, StaticSumIntIntIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(III)I");
  ASSERT_TRUE(method != NULL);

  byte sum_IIII_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x83, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x14, 0x10, 0xcd, 0xf8,
    0x18, 0x20, 0xcd, 0xf8, 0x1c, 0x30, 0x05, 0x98,
    0x06, 0x99, 0x42, 0x18, 0xcd, 0xf8, 0x04, 0x20,
    0x01, 0x9b, 0xdd, 0xf8, 0x1c, 0xc0, 0x13, 0xeb,
    0x0c, 0x03, 0xcd, 0xf8, 0x04, 0x30, 0x01, 0x98,
    0x03, 0xb0, 0xbd, 0xe8, 0x00, 0x80, 0x00, 0x00,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          sum_IIII_code,
                                          sizeof(sum_IIII_code));

  int args[3];
  JValue result;

  args[0] = 0;
  args[1] = 0;
  args[2] = 0;
  result.i = -1;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0, result.i);

  args[0] = 1;
  args[1] = 2;
  args[2] = 3;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(6, result.i);

  args[0] = -1;
  args[1] = 2;
  args[2] = -3;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-2, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MIN;
  args[2] = INT_MAX;
  result.i = 1234;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2147483646, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MAX;
  args[2] = INT_MAX;
  result.i = INT_MIN;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2147483645, result.i);

  FreeStub(method, sizeof(sum_IIII_code));
}

TEST_F(JniInternalTest, StaticSumIntIntIntIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(IIII)I");
  ASSERT_TRUE(method != NULL);

  byte sum_IIIII_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x83, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x14, 0x10, 0xcd, 0xf8,
    0x18, 0x20, 0xcd, 0xf8, 0x1c, 0x30, 0x05, 0x98,
    0x06, 0x99, 0x42, 0x18, 0xcd, 0xf8, 0x04, 0x20,
    0x01, 0x9b, 0xdd, 0xf8, 0x1c, 0xc0, 0x13, 0xeb,
    0x0c, 0x03, 0xcd, 0xf8, 0x04, 0x30, 0x01, 0x98,
    0x08, 0x99, 0x40, 0x18, 0xcd, 0xf8, 0x04, 0x00,
    0x01, 0x98, 0x03, 0xb0, 0xbd, 0xe8, 0x00, 0x80,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          sum_IIIII_code,
                                          sizeof(sum_IIIII_code));

  int args[4];
  JValue result;

  args[0] = 0;
  args[1] = 0;
  args[2] = 0;
  args[3] = 0;
  result.i = -1;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0, result.i);

  args[0] = 1;
  args[1] = 2;
  args[2] = 3;
  args[3] = 4;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(10, result.i);

  args[0] = -1;
  args[1] = 2;
  args[2] = -3;
  args[3] = 4;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MIN;
  args[2] = INT_MAX;
  args[3] = INT_MIN;
  result.i = 1234;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-2, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MAX;
  args[2] = INT_MAX;
  args[3] = INT_MAX;
  result.i = INT_MIN;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-4, result.i);

  FreeStub(method, sizeof(sum_IIIII_code));
}

TEST_F(JniInternalTest, StaticSumIntIntIntIntIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(IIIII)I");
  ASSERT_TRUE(method != NULL);

  byte sum_IIIIII_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x83, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x14, 0x10, 0xcd, 0xf8,
    0x18, 0x20, 0xcd, 0xf8, 0x1c, 0x30, 0x05, 0x98,
    0x06, 0x99, 0x42, 0x18, 0xcd, 0xf8, 0x04, 0x20,
    0x01, 0x9b, 0xdd, 0xf8, 0x1c, 0xc0, 0x13, 0xeb,
    0x0c, 0x03, 0xcd, 0xf8, 0x04, 0x30, 0x01, 0x98,
    0x08, 0x99, 0x40, 0x18, 0xcd, 0xf8, 0x04, 0x00,
    0x01, 0x9a, 0x09, 0x9b, 0xd2, 0x18, 0xcd, 0xf8,
    0x04, 0x20, 0x01, 0x98, 0x03, 0xb0, 0xbd, 0xe8,
    0x00, 0x80, 0x00, 0x00,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          sum_IIIIII_code,
                                          sizeof(sum_IIIIII_code));

  int args[5];
  JValue result;

  args[0] = 0;
  args[1] = 0;
  args[2] = 0;
  args[3] = 0;
  args[4] = 0;
  result.i = -1.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0, result.i);

  args[0] = 1;
  args[1] = 2;
  args[2] = 3;
  args[3] = 4;
  args[4] = 5;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(15, result.i);

  args[0] = -1;
  args[1] = 2;
  args[2] = -3;
  args[3] = 4;
  args[4] = -5;
  result.i = 0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-3, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MIN;
  args[2] = INT_MAX;
  args[3] = INT_MIN;
  args[4] = INT_MAX;
  result.i = 1234;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2147483645, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MAX;
  args[2] = INT_MAX;
  args[3] = INT_MAX;
  args[4] = INT_MAX;
  result.i = INT_MIN;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2147483643, result.i);

  FreeStub(method, sizeof(sum_IIIIII_code));
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(DD)D");
  ASSERT_TRUE(method != NULL);

  byte sum_DDD_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x87, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x24, 0x10, 0xcd, 0xf8,
    0x28, 0x20, 0xcd, 0xf8, 0x2c, 0x30, 0x9d, 0xed,
    0x09, 0x0b, 0x9d, 0xed, 0x0b, 0x1b, 0x30, 0xee,
    0x01, 0x2b, 0x8d, 0xed, 0x04, 0x2b, 0x04, 0x98,
    0x05, 0x99, 0x07, 0xb0, 0xbd, 0xe8, 0x00, 0x80,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          sum_DDD_code,
                                          sizeof(sum_DDD_code));

  double args[2];
  JValue result;

  args[0] = 0.0;
  args[1] = 0.0;
  result.d = -1.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0.0, result.d);

  args[0] = 1.0;
  args[1] = 2.0;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(3.0, result.d);

  args[0] = 1.0;
  args[1] = -2.0;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-1.0, result.d);

  args[0] = DBL_MAX;
  args[1] = DBL_MIN;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(1.7976931348623157e308, result.d);

  args[0] = DBL_MAX;
  args[1] = DBL_MAX;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(INFINITY, result.d);

  FreeStub(method, sizeof(sum_DDD_code));
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(DDD)D");
  ASSERT_TRUE(method != NULL);

  byte sum_DDDD_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x87, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x24, 0x10, 0xcd, 0xf8,
    0x28, 0x20, 0xcd, 0xf8, 0x2c, 0x30, 0x9d, 0xed,
    0x09, 0x0b, 0x9d, 0xed, 0x0b, 0x1b, 0x30, 0xee,
    0x01, 0x2b, 0x8d, 0xed, 0x04, 0x2b, 0x9d, 0xed,
    0x04, 0x3b, 0x9d, 0xed, 0x0d, 0x4b, 0x33, 0xee,
    0x04, 0x3b, 0x8d, 0xed, 0x04, 0x3b, 0x04, 0x98,
    0x05, 0x99, 0x07, 0xb0, 0xbd, 0xe8, 0x00, 0x80,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          sum_DDDD_code,
                                          sizeof(sum_DDDD_code));

  double args[3];
  JValue result;

  args[0] = 0.0;
  args[1] = 0.0;
  args[2] = 0.0;
  result.d = -1.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0.0, result.d);

  args[0] = 1.0;
  args[1] = 2.0;
  args[2] = 3.0;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(6.0, result.d);

  args[0] = 1.0;
  args[1] = -2.0;
  args[2] = 3.0;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2.0, result.d);

  FreeStub(method, sizeof(sum_DDDD_code));
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleDoubleDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(DDDD)D");
  ASSERT_TRUE(method != NULL);

  byte sum_DDDDD_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x87, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x24, 0x10, 0xcd, 0xf8,
    0x28, 0x20, 0xcd, 0xf8, 0x2c, 0x30, 0x9d, 0xed,
    0x09, 0x0b, 0x9d, 0xed, 0x0b, 0x1b, 0x30, 0xee,
    0x01, 0x2b, 0x8d, 0xed, 0x04, 0x2b, 0x9d, 0xed,
    0x04, 0x3b, 0x9d, 0xed, 0x0d, 0x4b, 0x33, 0xee,
    0x04, 0x3b, 0x8d, 0xed, 0x04, 0x3b, 0x9d, 0xed,
    0x04, 0x5b, 0x9d, 0xed, 0x0f, 0x6b, 0x35, 0xee,
    0x06, 0x5b, 0x8d, 0xed, 0x04, 0x5b, 0x04, 0x98,
    0x05, 0x99, 0x07, 0xb0, 0xbd, 0xe8, 0x00, 0x80,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          sum_DDDDD_code,
                                          sizeof(sum_DDDDD_code));

  double args[4];
  JValue result;

  args[0] = 0.0;
  args[1] = 0.0;
  args[2] = 0.0;
  args[3] = 0.0;
  result.d = -1.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0.0, result.d);

  args[0] = 1.0;
  args[1] = 2.0;
  args[2] = 3.0;
  args[3] = 4.0;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(10.0, result.d);

  args[0] = 1.0;
  args[1] = -2.0;
  args[2] = 3.0;
  args[3] = -4.0;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-2.0, result.d);

  FreeStub(method, sizeof(sum_DDDDD_code));
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleDoubleDoubleDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex));

  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  ASSERT_TRUE(class_loader != NULL);

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(DDDDD)D");
  ASSERT_TRUE(method != NULL);

  byte sum_DDDDDD_code[] = {
    0x2d, 0xe9, 0x00, 0x40, 0x87, 0xb0, 0xcd, 0xf8,
    0x00, 0x00, 0xcd, 0xf8, 0x24, 0x10, 0xcd, 0xf8,
    0x28, 0x20, 0xcd, 0xf8, 0x2c, 0x30, 0x9d, 0xed,
    0x09, 0x0b, 0x9d, 0xed, 0x0b, 0x1b, 0x30, 0xee,
    0x01, 0x2b, 0x8d, 0xed, 0x04, 0x2b, 0x9d, 0xed,
    0x04, 0x3b, 0x9d, 0xed, 0x0d, 0x4b, 0x33, 0xee,
    0x04, 0x3b, 0x8d, 0xed, 0x04, 0x3b, 0x9d, 0xed,
    0x04, 0x5b, 0x9d, 0xed, 0x0f, 0x6b, 0x35, 0xee,
    0x06, 0x5b, 0x8d, 0xed, 0x04, 0x5b, 0x9d, 0xed,
    0x04, 0x7b, 0x9d, 0xed, 0x11, 0x0b, 0x37, 0xee,
    0x00, 0x7b, 0x8d, 0xed, 0x04, 0x7b, 0x04, 0x98,
    0x05, 0x99, 0x07, 0xb0, 0xbd, 0xe8, 0x00, 0x80,
  };

  Method::InvokeStub* stub = AllocateStub(method,
                                          sum_DDDDDD_code,
                                          sizeof(sum_DDDDDD_code));

  double args[5];
  JValue result;

  args[0] = 0.0;
  args[1] = 0.0;
  args[2] = 0.0;
  args[3] = 0.0;
  args[4] = 0.0;
  result.d = -1.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0.0, result.d);

  args[0] = 1.0;
  args[1] = 2.0;
  args[2] = 3.0;
  args[3] = 4.0;
  args[4] = 5.0;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(15.0, result.d);

  args[0] = 1.0;
  args[1] = -2.0;
  args[2] = 3.0;
  args[3] = -4.0;
  args[4] = 5.0;
  result.d = 0.0;
  (*stub)(method, NULL, NULL, reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(3.0, result.d);

  FreeStub(method, sizeof(sum_DDDDDD_code));
}
#endif  // __arm__

}
