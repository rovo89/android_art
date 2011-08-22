// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include <cmath>
#include <sys/mman.h>

#include "common_test.h"
#include "gtest/gtest.h"

namespace art {

class JniInternalTest : public CommonTest {
 protected:
  virtual void SetUp() {
    CommonTest::SetUp();
    env_ = Thread::Current()->GetJniEnv();
  }
  JNIEnv* env_;
};

TEST_F(JniInternalTest, GetVersion) {
  ASSERT_EQ(JNI_VERSION_1_6, env_->GetVersion());
}

#define EXPECT_CLASS_FOUND(NAME) \
  EXPECT_TRUE(env_->FindClass(NAME) != NULL); \
  EXPECT_FALSE(env_->ExceptionCheck())

#define EXPECT_CLASS_NOT_FOUND(NAME) \
  EXPECT_TRUE(env_->FindClass(NAME) == NULL); \
  EXPECT_TRUE(env_->ExceptionCheck()); \
  env_->ExceptionClear()

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

#define EXPECT_EXCEPTION(exception_class) \
  do { \
    EXPECT_TRUE(env_->ExceptionCheck()); \
    jthrowable exception = env_->ExceptionOccurred(); \
    EXPECT_NE(static_cast<jthrowable>(NULL), exception); \
    EXPECT_TRUE(env_->IsInstanceOf(exception, exception_class)); \
    env_->ExceptionClear(); \
  } while (false)

TEST_F(JniInternalTest, GetFieldID) {
  jclass jlnsfe = env_->FindClass("java/lang/NoSuchFieldError");
  ASSERT_TRUE(jlnsfe != NULL);
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_TRUE(c != NULL);

  // Wrong type.
  jfieldID fid = env_->GetFieldID(c, "count", "J");
  EXPECT_EQ(static_cast<jfieldID>(NULL), fid);
  EXPECT_EXCEPTION(jlnsfe);

  // Wrong name.
  fid = env_->GetFieldID(c, "Count", "I");
  EXPECT_EQ(static_cast<jfieldID>(NULL), fid);
  EXPECT_EXCEPTION(jlnsfe);

  // Good declared field lookup.
  fid = env_->GetFieldID(c, "count", "I");
  EXPECT_NE(static_cast<jfieldID>(NULL), fid);
  EXPECT_TRUE(fid != NULL);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Good superclass field lookup.
  c = env_->FindClass("java/lang/StringBuilder");
  fid = env_->GetFieldID(c, "count", "I");
  EXPECT_NE(static_cast<jfieldID>(NULL), fid);
  EXPECT_TRUE(fid != NULL);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Not instance.
  fid = env_->GetFieldID(c, "CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_EQ(static_cast<jfieldID>(NULL), fid);
  EXPECT_EXCEPTION(jlnsfe);
}

TEST_F(JniInternalTest, GetStaticFieldID) {
  jclass jlnsfe = env_->FindClass("java/lang/NoSuchFieldError");
  ASSERT_TRUE(jlnsfe != NULL);
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_TRUE(c != NULL);

  // Wrong type.
  jfieldID fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", "J");
  EXPECT_EQ(static_cast<jfieldID>(NULL), fid);
  EXPECT_EXCEPTION(jlnsfe);

  // Wrong name.
  fid = env_->GetStaticFieldID(c, "cASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_EQ(static_cast<jfieldID>(NULL), fid);
  EXPECT_EXCEPTION(jlnsfe);

  // Good declared field lookup.
  fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_NE(static_cast<jfieldID>(NULL), fid);
  EXPECT_TRUE(fid != NULL);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Not static.
  fid = env_->GetStaticFieldID(c, "count", "I");
  EXPECT_EQ(static_cast<jfieldID>(NULL), fid);
  EXPECT_EXCEPTION(jlnsfe);
}

TEST_F(JniInternalTest, GetMethodID) {
  jclass jlobject = env_->FindClass("java/lang/Object");
  jclass jlstring = env_->FindClass("java/lang/String");
  jclass jlnsme = env_->FindClass("java/lang/NoSuchMethodError");

  // Sanity check that no exceptions are pending
  ASSERT_FALSE(env_->ExceptionCheck());

  // Check that java.lang.Object.foo() doesn't exist and NoSuchMethodError is
  // a pending exception
  jmethodID method = env_->GetMethodID(jlobject, "foo", "()V");
  EXPECT_EQ(static_cast<jmethodID>(NULL), method);
  EXPECT_EXCEPTION(jlnsme);

  // Check that java.lang.Object.equals() does exist
  method = env_->GetMethodID(jlobject, "equals", "(Ljava/lang/Object;)Z");
  EXPECT_NE(static_cast<jmethodID>(NULL), method);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Check that GetMethodID for java.lang.String.valueOf(int) fails as the
  // method is static
  method = env_->GetMethodID(jlstring, "valueOf", "(I)Ljava/lang/String;");
  EXPECT_EQ(static_cast<jmethodID>(NULL), method);
  EXPECT_EXCEPTION(jlnsme);
}

TEST_F(JniInternalTest, GetStaticMethodID) {
  jclass jlobject = env_->FindClass("java/lang/Object");
  jclass jlnsme = env_->FindClass("java/lang/NoSuchMethodError");

  // Sanity check that no exceptions are pending
  ASSERT_FALSE(env_->ExceptionCheck());

  // Check that java.lang.Object.foo() doesn't exist and NoSuchMethodError is
  // a pending exception
  jmethodID method = env_->GetStaticMethodID(jlobject, "foo", "()V");
  EXPECT_EQ(static_cast<jmethodID>(NULL), method);
  EXPECT_EXCEPTION(jlnsme);

  // Check that GetStaticMethodID for java.lang.Object.equals(Object) fails as
  // the method is not static
  method = env_->GetStaticMethodID(jlobject, "equals", "(Ljava/lang/Object;)Z");
  EXPECT_EQ(static_cast<jmethodID>(NULL), method);
  EXPECT_EXCEPTION(jlnsme);

  // Check that java.lang.String.valueOf(int) does exist
  jclass jlstring = env_->FindClass("java/lang/String");
  method = env_->GetStaticMethodID(jlstring, "valueOf",
                                   "(I)Ljava/lang/String;");
  EXPECT_NE(static_cast<jmethodID>(NULL), method);
  EXPECT_FALSE(env_->ExceptionCheck());
}

TEST_F(JniInternalTest, FromReflectedField_ToReflectedField) {
  jclass jlrField = env_->FindClass("java/lang/reflect/Field");
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_TRUE(c != NULL);
  jfieldID fid = env_->GetFieldID(c, "count", "I");
  ASSERT_TRUE(fid != NULL);
  // Turn the fid into a java.lang.reflect.Field...
  jobject field = env_->ToReflectedField(c, fid, JNI_FALSE);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(env_->IsInstanceOf(field, jlrField));
  // ...and back again.
  jfieldID fid2 = env_->FromReflectedField(field);
  ASSERT_TRUE(fid2 != NULL);
}

TEST_F(JniInternalTest, FromReflectedMethod_ToReflectedMethod) {
  jclass jlrMethod = env_->FindClass("java/lang/reflect/Method");
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_TRUE(c != NULL);
  jmethodID mid = env_->GetMethodID(c, "length", "()I");
  ASSERT_TRUE(mid != NULL);
  // Turn the mid into a java.lang.reflect.Method...
  jobject method = env_->ToReflectedMethod(c, mid, JNI_FALSE);
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(env_->IsInstanceOf(method, jlrMethod));
  // ...and back again.
  jmethodID mid2 = env_->FromReflectedMethod(method);
  ASSERT_TRUE(mid2 != NULL);
}

TEST_F(JniInternalTest, RegisterNatives) {
  jclass jlobject = env_->FindClass("java/lang/Object");
  jclass jlnsme = env_->FindClass("java/lang/NoSuchMethodError");

  // Sanity check that no exceptions are pending
  ASSERT_FALSE(env_->ExceptionCheck());

  // Check that registering to a non-existent java.lang.Object.foo() causes a
  // NoSuchMethodError
  {
    JNINativeMethod methods[] = {{"foo", "()V", NULL}};
    env_->RegisterNatives(jlobject, methods, 1);
  }
  EXPECT_EXCEPTION(jlnsme);

  // Check that registering non-native methods causes a NoSuchMethodError
  {
    JNINativeMethod methods[] = {{"equals", "(Ljava/lang/Object;)Z", NULL}};
    env_->RegisterNatives(jlobject, methods, 1);
  }
  EXPECT_EXCEPTION(jlnsme);

  // Check that registering native methods is successful
  {
    JNINativeMethod methods[] = {{"hashCode", "()I", NULL}};
    env_->RegisterNatives(jlobject, methods, 1);
  }
  EXPECT_FALSE(env_->ExceptionCheck());
}

#define EXPECT_PRIMITIVE_ARRAY(fn, size, expected_class_name) \
  do { \
    jarray a = env_->fn(size); \
    EXPECT_TRUE(a != NULL); \
    EXPECT_TRUE(env_->IsInstanceOf(a, \
        env_->FindClass(expected_class_name))); \
    EXPECT_EQ(size, env_->GetArrayLength(a)); \
  } while (false)

TEST_F(JniInternalTest, NewPrimitiveArray) {
  // TODO: death tests for negative array sizes.

  EXPECT_PRIMITIVE_ARRAY(NewBooleanArray, 0, "[Z");
  EXPECT_PRIMITIVE_ARRAY(NewByteArray, 0, "[B");
  EXPECT_PRIMITIVE_ARRAY(NewCharArray, 0, "[C");
  EXPECT_PRIMITIVE_ARRAY(NewDoubleArray, 0, "[D");
  EXPECT_PRIMITIVE_ARRAY(NewFloatArray, 0, "[F");
  EXPECT_PRIMITIVE_ARRAY(NewIntArray, 0, "[I");
  EXPECT_PRIMITIVE_ARRAY(NewLongArray, 0, "[J");
  EXPECT_PRIMITIVE_ARRAY(NewShortArray, 0, "[S");

  EXPECT_PRIMITIVE_ARRAY(NewBooleanArray, 1, "[Z");
  EXPECT_PRIMITIVE_ARRAY(NewByteArray, 1, "[B");
  EXPECT_PRIMITIVE_ARRAY(NewCharArray, 1, "[C");
  EXPECT_PRIMITIVE_ARRAY(NewDoubleArray, 1, "[D");
  EXPECT_PRIMITIVE_ARRAY(NewFloatArray, 1, "[F");
  EXPECT_PRIMITIVE_ARRAY(NewIntArray, 1, "[I");
  EXPECT_PRIMITIVE_ARRAY(NewLongArray, 1, "[J");
  EXPECT_PRIMITIVE_ARRAY(NewShortArray, 1, "[S");
}

TEST_F(JniInternalTest, NewObjectArray) {
  // TODO: death tests for negative array sizes.

  // TODO: check non-NULL initial elements.

  jclass element_class = env_->FindClass("java/lang/String");
  ASSERT_TRUE(element_class != NULL);
  jclass array_class = env_->FindClass("[Ljava/lang/String;");
  ASSERT_TRUE(array_class != NULL);

  jobjectArray a;

  a = env_->NewObjectArray(0, element_class, NULL);
  EXPECT_TRUE(a != NULL);
  EXPECT_TRUE(env_->IsInstanceOf(a, array_class));
  EXPECT_EQ(0, env_->GetArrayLength(a));

  a = env_->NewObjectArray(1, element_class, NULL);
  EXPECT_TRUE(a != NULL);
  EXPECT_TRUE(env_->IsInstanceOf(a, array_class));
  EXPECT_EQ(1, env_->GetArrayLength(a));
}

TEST_F(JniInternalTest, GetArrayLength) {
  // Already tested in NewObjectArray/NewPrimitiveArray.
}

TEST_F(JniInternalTest, NewStringUTF) {
  EXPECT_TRUE(env_->NewStringUTF(NULL) == NULL);
  EXPECT_TRUE(env_->NewStringUTF("") != NULL);
  EXPECT_TRUE(env_->NewStringUTF("hello") != NULL);
  // TODO: check some non-ASCII strings.
}

TEST_F(JniInternalTest, SetObjectArrayElement) {
  jclass aioobe = env_->FindClass("java/lang/ArrayIndexOutOfBoundsException");
  jclass c = env_->FindClass("[Ljava/lang/Object;");
  ASSERT_TRUE(c != NULL);

  jobjectArray array = env_->NewObjectArray(1, c, NULL);
  EXPECT_TRUE(array != NULL);
  env_->SetObjectArrayElement(array, 0, c);
  // TODO: check reading value back

  // ArrayIndexOutOfBounds for negative index.
  env_->SetObjectArrayElement(array, -1, c);
  EXPECT_EXCEPTION(aioobe);

  // ArrayIndexOutOfBounds for too-large index.
  env_->SetObjectArrayElement(array, 1, c);
  EXPECT_EXCEPTION(aioobe);

  // TODO: check ArrayStoreException thrown for bad types.
}

TEST_F(JniInternalTest, NewLocalRef_NULL) {
  EXPECT_TRUE(env_->NewLocalRef(NULL) == NULL);
}

TEST_F(JniInternalTest, NewLocalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_TRUE(s != NULL);
  jobject o = env_->NewLocalRef(s);
  EXPECT_TRUE(o != NULL);
  EXPECT_TRUE(o != s);

  // TODO: check that o is a local reference.
}

TEST_F(JniInternalTest, DeleteLocalRef_NULL) {
  env_->DeleteLocalRef(NULL);
}

TEST_F(JniInternalTest, DeleteLocalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_TRUE(s != NULL);
  env_->DeleteLocalRef(s);

  // Currently, deleting an already-deleted reference is just a warning.
  env_->DeleteLocalRef(s);

  s = env_->NewStringUTF("");
  ASSERT_TRUE(s != NULL);
  jobject o = env_->NewLocalRef(s);
  ASSERT_TRUE(o != NULL);

  env_->DeleteLocalRef(s);
  env_->DeleteLocalRef(o);
}

TEST_F(JniInternalTest, NewGlobalRef_NULL) {
  EXPECT_TRUE(env_->NewGlobalRef(NULL) == NULL);
}

TEST_F(JniInternalTest, NewGlobalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_TRUE(s != NULL);
  jobject o = env_->NewGlobalRef(s);
  EXPECT_TRUE(o != NULL);
  EXPECT_TRUE(o != s);

  // TODO: check that o is a global reference.
}

TEST_F(JniInternalTest, DeleteGlobalRef_NULL) {
  env_->DeleteGlobalRef(NULL);
}

TEST_F(JniInternalTest, DeleteGlobalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_TRUE(s != NULL);

  jobject o = env_->NewGlobalRef(s);
  ASSERT_TRUE(o != NULL);
  env_->DeleteGlobalRef(o);

  // Currently, deleting an already-deleted reference is just a warning.
  env_->DeleteGlobalRef(o);

  jobject o1 = env_->NewGlobalRef(s);
  ASSERT_TRUE(o1 != NULL);
  jobject o2 = env_->NewGlobalRef(s);
  ASSERT_TRUE(o2 != NULL);

  env_->DeleteGlobalRef(o1);
  env_->DeleteGlobalRef(o2);
}

TEST_F(JniInternalTest, NewWeakGlobalRef_NULL) {
  EXPECT_TRUE(env_->NewWeakGlobalRef(NULL) == NULL);
}

TEST_F(JniInternalTest, NewWeakGlobalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_TRUE(s != NULL);
  jobject o = env_->NewWeakGlobalRef(s);
  EXPECT_TRUE(o != NULL);
  EXPECT_TRUE(o != s);

  // TODO: check that o is a weak global reference.
}

TEST_F(JniInternalTest, DeleteWeakGlobalRef_NULL) {
  env_->DeleteWeakGlobalRef(NULL);
}

TEST_F(JniInternalTest, DeleteWeakGlobalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_TRUE(s != NULL);

  jobject o = env_->NewWeakGlobalRef(s);
  ASSERT_TRUE(o != NULL);
  env_->DeleteWeakGlobalRef(o);

  // Currently, deleting an already-deleted reference is just a warning.
  env_->DeleteWeakGlobalRef(o);

  jobject o1 = env_->NewWeakGlobalRef(s);
  ASSERT_TRUE(o1 != NULL);
  jobject o2 = env_->NewWeakGlobalRef(s);
  ASSERT_TRUE(o2 != NULL);

  env_->DeleteWeakGlobalRef(o1);
  env_->DeleteWeakGlobalRef(o2);
}

bool EnsureInvokeStub(Method* method);

Method::InvokeStub* AllocateStub(Method* method,
                                 byte* code,
                                 size_t length) {
  CHECK(method->GetInvokeStub() == NULL);
  EnsureInvokeStub(method);
  Method::InvokeStub* stub = method->GetInvokeStub();
  CHECK(stub != NULL);
  method->SetCode(code, length, kThumb2);
  return stub;
}

#if defined(__arm__)
TEST_F(JniInternalTest, StaticMainMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMainDex, "kMainDex"));

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
}

TEST_F(JniInternalTest, StaticNopMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticIdentityByteMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticIdentityIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticIdentityDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticSumIntIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticSumIntIntIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticSumIntIntIntIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticSumIntIntIntIntIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleDoubleDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleDoubleDoubleDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStaticLeafMethodsDex, "kStaticLeafMethodsDex"));

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
}
#endif  // __arm__

}
