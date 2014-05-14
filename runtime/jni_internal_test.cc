/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "jni_internal.h"

#include "common_compiler_test.h"
#include "mirror/art_method-inl.h"
#include "ScopedLocalRef.h"

namespace art {

// TODO: Convert to CommonRuntimeTest. Currently MakeExecutable is used.
class JniInternalTest : public CommonCompilerTest {
 protected:
  virtual void SetUp() {
    CommonCompilerTest::SetUp();

    vm_ = Runtime::Current()->GetJavaVM();

    // Turn on -verbose:jni for the JNI tests.
    // gLogVerbosity.jni = true;

    vm_->AttachCurrentThread(&env_, nullptr);

    ScopedLocalRef<jclass> aioobe(env_,
                                  env_->FindClass("java/lang/ArrayIndexOutOfBoundsException"));
    CHECK(aioobe.get() != nullptr);
    aioobe_ = reinterpret_cast<jclass>(env_->NewGlobalRef(aioobe.get()));

    ScopedLocalRef<jclass> ase(env_, env_->FindClass("java/lang/ArrayStoreException"));
    CHECK(ase.get() != nullptr);
    ase_ = reinterpret_cast<jclass>(env_->NewGlobalRef(ase.get()));

    ScopedLocalRef<jclass> sioobe(env_,
                                  env_->FindClass("java/lang/StringIndexOutOfBoundsException"));
    CHECK(sioobe.get() != nullptr);
    sioobe_ = reinterpret_cast<jclass>(env_->NewGlobalRef(sioobe.get()));
  }

  void ExpectException(jclass exception_class) {
    EXPECT_TRUE(env_->ExceptionCheck());
    jthrowable exception = env_->ExceptionOccurred();
    EXPECT_NE(nullptr, exception);
    env_->ExceptionClear();
    EXPECT_TRUE(env_->IsInstanceOf(exception, exception_class));
  }

  void ExpectClassFound(const char* name) {
    EXPECT_NE(env_->FindClass(name), nullptr) << name;
    EXPECT_FALSE(env_->ExceptionCheck()) << name;
  }

  void ExpectClassNotFound(const char* name) {
    EXPECT_EQ(env_->FindClass(name), nullptr) << name;
    EXPECT_TRUE(env_->ExceptionCheck()) << name;
    env_->ExceptionClear();
  }

  void CleanUpJniEnv() {
    if (aioobe_ != nullptr) {
      env_->DeleteGlobalRef(aioobe_);
      aioobe_ = nullptr;
    }
    if (ase_ != nullptr) {
      env_->DeleteGlobalRef(ase_);
      ase_ = nullptr;
    }
    if (sioobe_ != nullptr) {
      env_->DeleteGlobalRef(sioobe_);
      sioobe_ = nullptr;
    }
  }

  virtual void TearDown() OVERRIDE {
    CleanUpJniEnv();
    CommonCompilerTest::TearDown();
  }

  jclass GetPrimitiveClass(char descriptor) {
    ScopedObjectAccess soa(env_);
    mirror::Class* c = class_linker_->FindPrimitiveClass(descriptor);
    CHECK(c != nullptr);
    return soa.AddLocalReference<jclass>(c);
  }

  JavaVMExt* vm_;
  JNIEnv* env_;
  jclass aioobe_;
  jclass ase_;
  jclass sioobe_;
};

TEST_F(JniInternalTest, AllocObject) {
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_NE(c, nullptr);
  jobject o = env_->AllocObject(c);
  ASSERT_NE(o, nullptr);

  // We have an instance of the class we asked for...
  ASSERT_TRUE(env_->IsInstanceOf(o, c));
  // ...whose fields haven't been initialized because
  // we didn't call a constructor.
  ASSERT_EQ(0, env_->GetIntField(o, env_->GetFieldID(c, "count", "I")));
  ASSERT_EQ(0, env_->GetIntField(o, env_->GetFieldID(c, "offset", "I")));
  ASSERT_TRUE(env_->GetObjectField(o, env_->GetFieldID(c, "value", "[C")) == nullptr);
}

TEST_F(JniInternalTest, GetVersion) {
  ASSERT_EQ(JNI_VERSION_1_6, env_->GetVersion());
}

TEST_F(JniInternalTest, FindClass) {
  // Reference types...
  ExpectClassFound("java/lang/String");
  // ...for arrays too, where you must include "L;".
  ExpectClassFound("[Ljava/lang/String;");
  // Primitive arrays are okay too, if the primitive type is valid.
  ExpectClassFound("[C");

  {
    CheckJniAbortCatcher check_jni_abort_catcher;
    env_->FindClass(nullptr);
    check_jni_abort_catcher.Check("name == null");

    // We support . as well as / for compatibility, if -Xcheck:jni is off.
    ExpectClassFound("java.lang.String");
    check_jni_abort_catcher.Check("illegal class name 'java.lang.String'");
    ExpectClassNotFound("Ljava.lang.String;");
    check_jni_abort_catcher.Check("illegal class name 'Ljava.lang.String;'");
    ExpectClassFound("[Ljava.lang.String;");
    check_jni_abort_catcher.Check("illegal class name '[Ljava.lang.String;'");
    ExpectClassNotFound("[java.lang.String");
    check_jni_abort_catcher.Check("illegal class name '[java.lang.String'");

    // You can't include the "L;" in a JNI class descriptor.
    ExpectClassNotFound("Ljava/lang/String;");
    check_jni_abort_catcher.Check("illegal class name 'Ljava/lang/String;'");

    // But you must include it for an array of any reference type.
    ExpectClassNotFound("[java/lang/String");
    check_jni_abort_catcher.Check("illegal class name '[java/lang/String'");

    ExpectClassNotFound("[K");
    check_jni_abort_catcher.Check("illegal class name '[K'");

    // Void arrays aren't allowed.
    ExpectClassNotFound("[V");
    check_jni_abort_catcher.Check("illegal class name '[V'");
  }

  // But primitive types aren't allowed...
  ExpectClassNotFound("C");
  ExpectClassNotFound("V");
  ExpectClassNotFound("K");
}

TEST_F(JniInternalTest, GetFieldID) {
  jclass jlnsfe = env_->FindClass("java/lang/NoSuchFieldError");
  ASSERT_NE(jlnsfe, nullptr);
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_NE(c, nullptr);

  // Wrong type.
  jfieldID fid = env_->GetFieldID(c, "count", "J");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Wrong type where type doesn't exist.
  fid = env_->GetFieldID(c, "count", "Lrod/jane/freddy;");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Wrong name.
  fid = env_->GetFieldID(c, "Count", "I");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Good declared field lookup.
  fid = env_->GetFieldID(c, "count", "I");
  EXPECT_NE(nullptr, fid);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Good superclass field lookup.
  c = env_->FindClass("java/lang/StringBuilder");
  fid = env_->GetFieldID(c, "count", "I");
  EXPECT_NE(nullptr, fid);
  EXPECT_NE(fid, nullptr);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Not instance.
  fid = env_->GetFieldID(c, "CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Bad arguments.
  CheckJniAbortCatcher check_jni_abort_catcher;
  fid = env_->GetFieldID(nullptr, "count", "I");
  EXPECT_EQ(nullptr, fid);
  check_jni_abort_catcher.Check("java_class == null");
  fid = env_->GetFieldID(c, nullptr, "I");
  EXPECT_EQ(nullptr, fid);
  check_jni_abort_catcher.Check("name == null");
  fid = env_->GetFieldID(c, "count", nullptr);
  EXPECT_EQ(nullptr, fid);
  check_jni_abort_catcher.Check("sig == null");
}

TEST_F(JniInternalTest, GetStaticFieldID) {
  jclass jlnsfe = env_->FindClass("java/lang/NoSuchFieldError");
  ASSERT_NE(jlnsfe, nullptr);
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_NE(c, nullptr);

  // Wrong type.
  jfieldID fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", "J");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Wrong type where type doesn't exist.
  fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", "Lrod/jane/freddy;");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Wrong name.
  fid = env_->GetStaticFieldID(c, "cASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Good declared field lookup.
  fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_NE(nullptr, fid);
  EXPECT_NE(fid, nullptr);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Not static.
  fid = env_->GetStaticFieldID(c, "count", "I");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Bad arguments.
  CheckJniAbortCatcher check_jni_abort_catcher;
  fid = env_->GetStaticFieldID(nullptr, "CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_EQ(nullptr, fid);
  check_jni_abort_catcher.Check("java_class == null");
  fid = env_->GetStaticFieldID(c, nullptr, "Ljava/util/Comparator;");
  EXPECT_EQ(nullptr, fid);
  check_jni_abort_catcher.Check("name == null");
  fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", nullptr);
  EXPECT_EQ(nullptr, fid);
  check_jni_abort_catcher.Check("sig == null");
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
  EXPECT_EQ(nullptr, method);
  ExpectException(jlnsme);

  // Check that java.lang.Object.equals() does exist
  method = env_->GetMethodID(jlobject, "equals", "(Ljava/lang/Object;)Z");
  EXPECT_NE(nullptr, method);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Check that GetMethodID for java.lang.String.valueOf(int) fails as the
  // method is static
  method = env_->GetMethodID(jlstring, "valueOf", "(I)Ljava/lang/String;");
  EXPECT_EQ(nullptr, method);
  ExpectException(jlnsme);

  // Check that GetMethodID for java.lang.NoSuchMethodError.<init>(String) finds the constructor
  method = env_->GetMethodID(jlnsme, "<init>", "(Ljava/lang/String;)V");
  EXPECT_NE(nullptr, method);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Bad arguments.
  CheckJniAbortCatcher check_jni_abort_catcher;
  method = env_->GetMethodID(nullptr, "<init>", "(Ljava/lang/String;)V");
  EXPECT_EQ(nullptr, method);
  check_jni_abort_catcher.Check("java_class == null");
  method = env_->GetMethodID(jlnsme, nullptr, "(Ljava/lang/String;)V");
  EXPECT_EQ(nullptr, method);
  check_jni_abort_catcher.Check("name == null");
  method = env_->GetMethodID(jlnsme, "<init>", nullptr);
  EXPECT_EQ(nullptr, method);
  check_jni_abort_catcher.Check("sig == null");
}

TEST_F(JniInternalTest, GetStaticMethodID) {
  jclass jlobject = env_->FindClass("java/lang/Object");
  jclass jlnsme = env_->FindClass("java/lang/NoSuchMethodError");

  // Sanity check that no exceptions are pending
  ASSERT_FALSE(env_->ExceptionCheck());

  // Check that java.lang.Object.foo() doesn't exist and NoSuchMethodError is
  // a pending exception
  jmethodID method = env_->GetStaticMethodID(jlobject, "foo", "()V");
  EXPECT_EQ(nullptr, method);
  ExpectException(jlnsme);

  // Check that GetStaticMethodID for java.lang.Object.equals(Object) fails as
  // the method is not static
  method = env_->GetStaticMethodID(jlobject, "equals", "(Ljava/lang/Object;)Z");
  EXPECT_EQ(nullptr, method);
  ExpectException(jlnsme);

  // Check that java.lang.String.valueOf(int) does exist
  jclass jlstring = env_->FindClass("java/lang/String");
  method = env_->GetStaticMethodID(jlstring, "valueOf", "(I)Ljava/lang/String;");
  EXPECT_NE(nullptr, method);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Bad arguments.
  CheckJniAbortCatcher check_jni_abort_catcher;
  method = env_->GetStaticMethodID(nullptr, "valueOf", "(I)Ljava/lang/String;");
  EXPECT_EQ(nullptr, method);
  check_jni_abort_catcher.Check("java_class == null");
  method = env_->GetStaticMethodID(jlstring, nullptr, "(I)Ljava/lang/String;");
  EXPECT_EQ(nullptr, method);
  check_jni_abort_catcher.Check("name == null");
  method = env_->GetStaticMethodID(jlstring, "valueOf", nullptr);
  EXPECT_EQ(nullptr, method);
  check_jni_abort_catcher.Check("sig == null");
}

TEST_F(JniInternalTest, FromReflectedField_ToReflectedField) {
  jclass jlrField = env_->FindClass("java/lang/reflect/Field");
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_NE(c, nullptr);
  jfieldID fid = env_->GetFieldID(c, "count", "I");
  ASSERT_NE(fid, nullptr);
  // Turn the fid into a java.lang.reflect.Field...
  jobject field = env_->ToReflectedField(c, fid, JNI_FALSE);
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(env_->IsInstanceOf(field, jlrField));
  // ...and back again.
  jfieldID fid2 = env_->FromReflectedField(field);
  ASSERT_NE(fid2, nullptr);
  // Make sure we can actually use it.
  jstring s = env_->NewStringUTF("poop");
  ASSERT_EQ(4, env_->GetIntField(s, fid2));

  // Bad arguments.
  CheckJniAbortCatcher check_jni_abort_catcher;
  field = env_->ToReflectedField(c, nullptr, JNI_FALSE);
  EXPECT_EQ(field, nullptr);
  check_jni_abort_catcher.Check("fid == null");
  fid2 = env_->FromReflectedField(nullptr);
  ASSERT_EQ(fid2, nullptr);
  check_jni_abort_catcher.Check("jlr_field == null");
}

TEST_F(JniInternalTest, FromReflectedMethod_ToReflectedMethod) {
  jclass jlrMethod = env_->FindClass("java/lang/reflect/Method");
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_NE(c, nullptr);
  jmethodID mid = env_->GetMethodID(c, "length", "()I");
  ASSERT_NE(mid, nullptr);
  // Turn the mid into a java.lang.reflect.Method...
  jobject method = env_->ToReflectedMethod(c, mid, JNI_FALSE);
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(env_->IsInstanceOf(method, jlrMethod));
  // ...and back again.
  jmethodID mid2 = env_->FromReflectedMethod(method);
  ASSERT_NE(mid2, nullptr);
  // Make sure we can actually use it.
  jstring s = env_->NewStringUTF("poop");
  ASSERT_EQ(4, env_->CallIntMethod(s, mid2));

  // Bad arguments.
  CheckJniAbortCatcher check_jni_abort_catcher;
  method = env_->ToReflectedMethod(c, nullptr, JNI_FALSE);
  EXPECT_EQ(method, nullptr);
  check_jni_abort_catcher.Check("mid == null");
  mid2 = env_->FromReflectedMethod(method);
  ASSERT_EQ(mid2, nullptr);
  check_jni_abort_catcher.Check("jlr_method == null");
}

static void BogusMethod() {
  // You can't pass nullptr function pointers to RegisterNatives.
}

TEST_F(JniInternalTest, RegisterAndUnregisterNatives) {
  jclass jlobject = env_->FindClass("java/lang/Object");
  jclass jlnsme = env_->FindClass("java/lang/NoSuchMethodError");

  // Sanity check that no exceptions are pending.
  ASSERT_FALSE(env_->ExceptionCheck());

  // Check that registering to a non-existent java.lang.Object.foo() causes a NoSuchMethodError.
  {
    JNINativeMethod methods[] = { { "foo", "()V", nullptr } };
    EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 1), JNI_ERR);
  }
  ExpectException(jlnsme);

  // Check that registering non-native methods causes a NoSuchMethodError.
  {
    JNINativeMethod methods[] = { { "equals", "(Ljava/lang/Object;)Z", nullptr } };
    EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 1), JNI_ERR);
  }
  ExpectException(jlnsme);

  // Check that registering native methods is successful.
  {
    JNINativeMethod methods[] = { { "notify", "()V", reinterpret_cast<void*>(BogusMethod) } };
    EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 1), JNI_OK);
  }
  EXPECT_FALSE(env_->ExceptionCheck());
  EXPECT_EQ(env_->UnregisterNatives(jlobject), JNI_OK);

  // Check that registering no methods isn't a failure.
  {
    JNINativeMethod methods[] = { };
    EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 0), JNI_OK);
  }
  EXPECT_FALSE(env_->ExceptionCheck());
  EXPECT_EQ(env_->UnregisterNatives(jlobject), JNI_OK);

  // Check that registering a -ve number of methods is a failure.
  CheckJniAbortCatcher check_jni_abort_catcher;
  for (int i = -10; i < 0; ++i) {
    JNINativeMethod methods[] = { };
    EXPECT_EQ(env_->RegisterNatives(jlobject, methods, i), JNI_ERR);
    check_jni_abort_catcher.Check("negative method count: ");
  }
  EXPECT_FALSE(env_->ExceptionCheck());

  // Passing a class of null is a failure.
  {
    JNINativeMethod methods[] = { };
    EXPECT_EQ(env_->RegisterNatives(nullptr, methods, 0), JNI_ERR);
    check_jni_abort_catcher.Check("java_class == null");
  }

  // Passing methods as null is a failure.
  EXPECT_EQ(env_->RegisterNatives(jlobject, nullptr, 1), JNI_ERR);
  check_jni_abort_catcher.Check("methods == null");

  // Unregisters null is a failure.
  EXPECT_EQ(env_->UnregisterNatives(nullptr), JNI_ERR);
  check_jni_abort_catcher.Check("java_class == null");

  // Unregistering a class with no natives is a warning.
  EXPECT_EQ(env_->UnregisterNatives(jlnsme), JNI_OK);
}

#define EXPECT_PRIMITIVE_ARRAY(new_fn, \
                               get_region_fn, \
                               set_region_fn, \
                               get_elements_fn, \
                               release_elements_fn, \
                               scalar_type, \
                               expected_class_descriptor) \
  jsize size = 4; \
  \
  { \
    CheckJniAbortCatcher jni_abort_catcher; \
    /* Allocate an negative sized array and check it has the right failure type. */ \
    EXPECT_EQ(env_->new_fn(-1), nullptr); \
    jni_abort_catcher.Check("negative array length: -1"); \
    EXPECT_EQ(env_->new_fn(std::numeric_limits<jint>::min()), nullptr); \
    jni_abort_catcher.Check("negative array length: -2147483648"); \
    /* Pass the array as null. */ \
    EXPECT_EQ(0, env_->GetArrayLength(nullptr)); \
    jni_abort_catcher.Check("java_array == null"); \
    env_->get_region_fn(nullptr, 0, 0, nullptr); \
    jni_abort_catcher.Check("java_array == null"); \
    env_->set_region_fn(nullptr, 0, 0, nullptr); \
    jni_abort_catcher.Check("java_array == null"); \
    env_->get_elements_fn(nullptr, nullptr); \
    jni_abort_catcher.Check("java_array == null"); \
    env_->release_elements_fn(nullptr, nullptr, 0); \
    jni_abort_catcher.Check("java_array == null"); \
    /* Pass the elements for region as null. */ \
    scalar_type ## Array a = env_->new_fn(size); \
    env_->get_region_fn(a, 0, size, nullptr); \
    jni_abort_catcher.Check("buf == null"); \
    env_->set_region_fn(a, 0, size, nullptr); \
    jni_abort_catcher.Check("buf == null"); \
  } \
  /* Allocate an array and check it has the right type and length. */ \
  scalar_type ## Array a = env_->new_fn(size); \
  EXPECT_NE(a, nullptr); \
  EXPECT_TRUE(env_->IsInstanceOf(a, env_->FindClass(expected_class_descriptor))); \
  EXPECT_EQ(size, env_->GetArrayLength(a)); \
  \
  /* GetPrimitiveArrayRegion/SetPrimitiveArrayRegion */ \
  /* AIOOBE for negative start offset. */ \
  env_->get_region_fn(a, -1, 1, nullptr); \
  ExpectException(aioobe_); \
  env_->set_region_fn(a, -1, 1, nullptr); \
  ExpectException(aioobe_); \
  \
  /* AIOOBE for negative length. */ \
  env_->get_region_fn(a, 0, -1, nullptr); \
  ExpectException(aioobe_); \
  env_->set_region_fn(a, 0, -1, nullptr); \
  ExpectException(aioobe_); \
  \
  /* AIOOBE for buffer overrun. */ \
  env_->get_region_fn(a, size - 1, size, nullptr); \
  ExpectException(aioobe_); \
  env_->set_region_fn(a, size - 1, size, nullptr); \
  ExpectException(aioobe_); \
  \
  /* It's okay for the buffer to be nullptr as long as the length is 0. */ \
  env_->get_region_fn(a, 2, 0, nullptr); \
  /* Even if the offset is invalid... */ \
  env_->get_region_fn(a, 123, 0, nullptr); \
  ExpectException(aioobe_); \
  \
  /* It's okay for the buffer to be nullptr as long as the length is 0. */ \
  env_->set_region_fn(a, 2, 0, nullptr); \
  /* Even if the offset is invalid... */ \
  env_->set_region_fn(a, 123, 0, nullptr); \
  ExpectException(aioobe_); \
  \
  /* Prepare a couple of buffers. */ \
  UniquePtr<scalar_type[]> src_buf(new scalar_type[size]); \
  UniquePtr<scalar_type[]> dst_buf(new scalar_type[size]); \
  for (jsize i = 0; i < size; ++i) { src_buf[i] = scalar_type(i); } \
  for (jsize i = 0; i < size; ++i) { dst_buf[i] = scalar_type(-1); } \
  \
  /* Copy all of src_buf onto the heap. */ \
  env_->set_region_fn(a, 0, size, &src_buf[0]); \
  /* Copy back only part. */ \
  env_->get_region_fn(a, 1, size - 2, &dst_buf[1]); \
  EXPECT_NE(memcmp(&src_buf[0], &dst_buf[0], size * sizeof(scalar_type)), 0) \
    << "short copy equal"; \
  /* Copy the missing pieces. */ \
  env_->get_region_fn(a, 0, 1, &dst_buf[0]); \
  env_->get_region_fn(a, size - 1, 1, &dst_buf[size - 1]); \
  EXPECT_EQ(memcmp(&src_buf[0], &dst_buf[0], size * sizeof(scalar_type)), 0) \
    << "fixed copy not equal"; \
  /* Copy back the whole array. */ \
  env_->get_region_fn(a, 0, size, &dst_buf[0]); \
  EXPECT_EQ(memcmp(&src_buf[0], &dst_buf[0], size * sizeof(scalar_type)), 0) \
    << "full copy not equal"; \
  /* GetPrimitiveArrayCritical */ \
  void* v = env_->GetPrimitiveArrayCritical(a, nullptr); \
  EXPECT_EQ(memcmp(&src_buf[0], v, size * sizeof(scalar_type)), 0) \
    << "GetPrimitiveArrayCritical not equal"; \
  env_->ReleasePrimitiveArrayCritical(a, v, 0); \
  /* GetXArrayElements */ \
  scalar_type* xs = env_->get_elements_fn(a, nullptr); \
  EXPECT_EQ(memcmp(&src_buf[0], xs, size * sizeof(scalar_type)), 0) \
    << # get_elements_fn " not equal"; \
  env_->release_elements_fn(a, xs, 0); \

TEST_F(JniInternalTest, BooleanArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewBooleanArray, GetBooleanArrayRegion, SetBooleanArrayRegion,
                         GetBooleanArrayElements, ReleaseBooleanArrayElements, jboolean, "[Z");
}
TEST_F(JniInternalTest, ByteArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewByteArray, GetByteArrayRegion, SetByteArrayRegion,
                         GetByteArrayElements, ReleaseByteArrayElements, jbyte, "[B");
}
TEST_F(JniInternalTest, CharArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewCharArray, GetCharArrayRegion, SetCharArrayRegion,
                         GetCharArrayElements, ReleaseCharArrayElements, jchar, "[C");
}
TEST_F(JniInternalTest, DoubleArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewDoubleArray, GetDoubleArrayRegion, SetDoubleArrayRegion,
                         GetDoubleArrayElements, ReleaseDoubleArrayElements, jdouble, "[D");
}
TEST_F(JniInternalTest, FloatArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewFloatArray, GetFloatArrayRegion, SetFloatArrayRegion,
                         GetFloatArrayElements, ReleaseFloatArrayElements, jfloat, "[F");
}
TEST_F(JniInternalTest, IntArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewIntArray, GetIntArrayRegion, SetIntArrayRegion,
                         GetIntArrayElements, ReleaseIntArrayElements, jint, "[I");
}
TEST_F(JniInternalTest, LongArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewLongArray, GetLongArrayRegion, SetLongArrayRegion,
                         GetLongArrayElements, ReleaseLongArrayElements, jlong, "[J");
}
TEST_F(JniInternalTest, ShortArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewShortArray, GetShortArrayRegion, SetShortArrayRegion,
                         GetShortArrayElements, ReleaseShortArrayElements, jshort, "[S");
}

TEST_F(JniInternalTest, GetPrimitiveArrayElementsOfWrongType) {
  CheckJniAbortCatcher jni_abort_catcher;
  jbooleanArray array = env_->NewBooleanArray(10);
  jboolean is_copy;
  EXPECT_EQ(env_->GetByteArrayElements(reinterpret_cast<jbyteArray>(array), &is_copy), nullptr);
  jni_abort_catcher.Check(
      "attempt to get byte primitive array elements with an object of type boolean[]");
  EXPECT_EQ(env_->GetShortArrayElements(reinterpret_cast<jshortArray>(array), &is_copy), nullptr);
  jni_abort_catcher.Check(
      "attempt to get short primitive array elements with an object of type boolean[]");
  EXPECT_EQ(env_->GetCharArrayElements(reinterpret_cast<jcharArray>(array), &is_copy), nullptr);
  jni_abort_catcher.Check(
      "attempt to get char primitive array elements with an object of type boolean[]");
  EXPECT_EQ(env_->GetIntArrayElements(reinterpret_cast<jintArray>(array), &is_copy), nullptr);
  jni_abort_catcher.Check(
      "attempt to get int primitive array elements with an object of type boolean[]");
  EXPECT_EQ(env_->GetLongArrayElements(reinterpret_cast<jlongArray>(array), &is_copy), nullptr);
  jni_abort_catcher.Check(
      "attempt to get long primitive array elements with an object of type boolean[]");
  EXPECT_EQ(env_->GetFloatArrayElements(reinterpret_cast<jfloatArray>(array), &is_copy), nullptr);
  jni_abort_catcher.Check(
      "attempt to get float primitive array elements with an object of type boolean[]");
  EXPECT_EQ(env_->GetDoubleArrayElements(reinterpret_cast<jdoubleArray>(array), &is_copy), nullptr);
  jni_abort_catcher.Check(
      "attempt to get double primitive array elements with an object of type boolean[]");
  jbyteArray array2 = env_->NewByteArray(10);
  EXPECT_EQ(env_->GetBooleanArrayElements(reinterpret_cast<jbooleanArray>(array2), &is_copy), nullptr);
  jni_abort_catcher.Check(
      "attempt to get boolean primitive array elements with an object of type byte[]");
  jobject object = env_->NewStringUTF("Test String");
  EXPECT_EQ(env_->GetBooleanArrayElements(reinterpret_cast<jbooleanArray>(object), &is_copy), nullptr);
  jni_abort_catcher.Check(
      "attempt to get boolean primitive array elements with an object of type java.lang.String");
}

TEST_F(JniInternalTest, ReleasePrimitiveArrayElementsOfWrongType) {
  CheckJniAbortCatcher jni_abort_catcher;
  jbooleanArray array = env_->NewBooleanArray(10);
  ASSERT_TRUE(array != nullptr);
  jboolean is_copy;
  jboolean* elements = env_->GetBooleanArrayElements(array, &is_copy);
  ASSERT_TRUE(elements != nullptr);
  env_->ReleaseByteArrayElements(reinterpret_cast<jbyteArray>(array),
                                 reinterpret_cast<jbyte*>(elements), 0);
  jni_abort_catcher.Check(
      "attempt to release byte primitive array elements with an object of type boolean[]");
  env_->ReleaseShortArrayElements(reinterpret_cast<jshortArray>(array),
                                  reinterpret_cast<jshort*>(elements), 0);
  jni_abort_catcher.Check(
      "attempt to release short primitive array elements with an object of type boolean[]");
  env_->ReleaseCharArrayElements(reinterpret_cast<jcharArray>(array),
                                 reinterpret_cast<jchar*>(elements), 0);
  jni_abort_catcher.Check(
      "attempt to release char primitive array elements with an object of type boolean[]");
  env_->ReleaseIntArrayElements(reinterpret_cast<jintArray>(array),
                                reinterpret_cast<jint*>(elements), 0);
  jni_abort_catcher.Check(
      "attempt to release int primitive array elements with an object of type boolean[]");
  env_->ReleaseLongArrayElements(reinterpret_cast<jlongArray>(array),
                                 reinterpret_cast<jlong*>(elements), 0);
  jni_abort_catcher.Check(
      "attempt to release long primitive array elements with an object of type boolean[]");
  env_->ReleaseFloatArrayElements(reinterpret_cast<jfloatArray>(array),
                                  reinterpret_cast<jfloat*>(elements), 0);
  jni_abort_catcher.Check(
      "attempt to release float primitive array elements with an object of type boolean[]");
  env_->ReleaseDoubleArrayElements(reinterpret_cast<jdoubleArray>(array),
                                  reinterpret_cast<jdouble*>(elements), 0);
  jni_abort_catcher.Check(
      "attempt to release double primitive array elements with an object of type boolean[]");
  jbyteArray array2 = env_->NewByteArray(10);
  env_->ReleaseBooleanArrayElements(reinterpret_cast<jbooleanArray>(array2), elements, 0);
  jni_abort_catcher.Check(
      "attempt to release boolean primitive array elements with an object of type byte[]");
  jobject object = env_->NewStringUTF("Test String");
  env_->ReleaseBooleanArrayElements(reinterpret_cast<jbooleanArray>(object), elements, 0);
  jni_abort_catcher.Check(
      "attempt to release boolean primitive array elements with an object of type java.lang.String");
}
TEST_F(JniInternalTest, GetReleasePrimitiveArrayCriticalOfWrongType) {
  CheckJniAbortCatcher jni_abort_catcher;
  jobject object = env_->NewStringUTF("Test String");
  jboolean is_copy;
  void* elements = env_->GetPrimitiveArrayCritical(reinterpret_cast<jarray>(object), &is_copy);
  jni_abort_catcher.Check("expected primitive array, given java.lang.String");
  env_->ReleasePrimitiveArrayCritical(reinterpret_cast<jarray>(object), elements, 0);
  jni_abort_catcher.Check("expected primitive array, given java.lang.String");
}

TEST_F(JniInternalTest, GetPrimitiveArrayRegionElementsOfWrongType) {
  CheckJniAbortCatcher jni_abort_catcher;
  constexpr size_t kLength = 10;
  jbooleanArray array = env_->NewBooleanArray(kLength);
  ASSERT_TRUE(array != nullptr);
  jboolean elements[kLength];
  env_->GetByteArrayRegion(reinterpret_cast<jbyteArray>(array), 0, kLength,
                           reinterpret_cast<jbyte*>(elements));
  jni_abort_catcher.Check(
      "attempt to get region of byte primitive array elements with an object of type boolean[]");
  env_->GetShortArrayRegion(reinterpret_cast<jshortArray>(array), 0, kLength,
                            reinterpret_cast<jshort*>(elements));
  jni_abort_catcher.Check(
      "attempt to get region of short primitive array elements with an object of type boolean[]");
  env_->GetCharArrayRegion(reinterpret_cast<jcharArray>(array), 0, kLength,
                           reinterpret_cast<jchar*>(elements));
  jni_abort_catcher.Check(
      "attempt to get region of char primitive array elements with an object of type boolean[]");
  env_->GetIntArrayRegion(reinterpret_cast<jintArray>(array), 0, kLength,
                          reinterpret_cast<jint*>(elements));
  jni_abort_catcher.Check(
      "attempt to get region of int primitive array elements with an object of type boolean[]");
  env_->GetLongArrayRegion(reinterpret_cast<jlongArray>(array), 0, kLength,
                           reinterpret_cast<jlong*>(elements));
  jni_abort_catcher.Check(
      "attempt to get region of long primitive array elements with an object of type boolean[]");
  env_->GetFloatArrayRegion(reinterpret_cast<jfloatArray>(array), 0, kLength,
                            reinterpret_cast<jfloat*>(elements));
  jni_abort_catcher.Check(
      "attempt to get region of float primitive array elements with an object of type boolean[]");
  env_->GetDoubleArrayRegion(reinterpret_cast<jdoubleArray>(array), 0, kLength,
                           reinterpret_cast<jdouble*>(elements));
  jni_abort_catcher.Check(
      "attempt to get region of double primitive array elements with an object of type boolean[]");
  jbyteArray array2 = env_->NewByteArray(10);
  env_->GetBooleanArrayRegion(reinterpret_cast<jbooleanArray>(array2), 0, kLength,
                              reinterpret_cast<jboolean*>(elements));
  jni_abort_catcher.Check(
      "attempt to get region of boolean primitive array elements with an object of type byte[]");
  jobject object = env_->NewStringUTF("Test String");
  env_->GetBooleanArrayRegion(reinterpret_cast<jbooleanArray>(object), 0, kLength,
                              reinterpret_cast<jboolean*>(elements));
  jni_abort_catcher.Check(
      "attempt to get region of boolean primitive array elements with an object of type java.lang.String");
}

TEST_F(JniInternalTest, SetPrimitiveArrayRegionElementsOfWrongType) {
  CheckJniAbortCatcher jni_abort_catcher;
  constexpr size_t kLength = 10;
  jbooleanArray array = env_->NewBooleanArray(kLength);
  ASSERT_TRUE(array != nullptr);
  jboolean elements[kLength];
  env_->SetByteArrayRegion(reinterpret_cast<jbyteArray>(array), 0, kLength,
                           reinterpret_cast<jbyte*>(elements));
  jni_abort_catcher.Check(
      "attempt to set region of byte primitive array elements with an object of type boolean[]");
  env_->SetShortArrayRegion(reinterpret_cast<jshortArray>(array), 0, kLength,
                            reinterpret_cast<jshort*>(elements));
  jni_abort_catcher.Check(
      "attempt to set region of short primitive array elements with an object of type boolean[]");
  env_->SetCharArrayRegion(reinterpret_cast<jcharArray>(array), 0, kLength,
                           reinterpret_cast<jchar*>(elements));
  jni_abort_catcher.Check(
      "attempt to set region of char primitive array elements with an object of type boolean[]");
  env_->SetIntArrayRegion(reinterpret_cast<jintArray>(array), 0, kLength,
                          reinterpret_cast<jint*>(elements));
  jni_abort_catcher.Check(
      "attempt to set region of int primitive array elements with an object of type boolean[]");
  env_->SetLongArrayRegion(reinterpret_cast<jlongArray>(array), 0, kLength,
                           reinterpret_cast<jlong*>(elements));
  jni_abort_catcher.Check(
      "attempt to set region of long primitive array elements with an object of type boolean[]");
  env_->SetFloatArrayRegion(reinterpret_cast<jfloatArray>(array), 0, kLength,
                            reinterpret_cast<jfloat*>(elements));
  jni_abort_catcher.Check(
      "attempt to set region of float primitive array elements with an object of type boolean[]");
  env_->SetDoubleArrayRegion(reinterpret_cast<jdoubleArray>(array), 0, kLength,
                           reinterpret_cast<jdouble*>(elements));
  jni_abort_catcher.Check(
      "attempt to set region of double primitive array elements with an object of type boolean[]");
  jbyteArray array2 = env_->NewByteArray(10);
  env_->SetBooleanArrayRegion(reinterpret_cast<jbooleanArray>(array2), 0, kLength,
                              reinterpret_cast<jboolean*>(elements));
  jni_abort_catcher.Check(
      "attempt to set region of boolean primitive array elements with an object of type byte[]");
  jobject object = env_->NewStringUTF("Test String");
  env_->SetBooleanArrayRegion(reinterpret_cast<jbooleanArray>(object), 0, kLength,
                              reinterpret_cast<jboolean*>(elements));
  jni_abort_catcher.Check(
      "attempt to set region of boolean primitive array elements with an object of type java.lang.String");
}

TEST_F(JniInternalTest, NewObjectArray) {
  jclass element_class = env_->FindClass("java/lang/String");
  ASSERT_NE(element_class, nullptr);
  jclass array_class = env_->FindClass("[Ljava/lang/String;");
  ASSERT_NE(array_class, nullptr);

  jobjectArray a = env_->NewObjectArray(0, element_class, nullptr);
  EXPECT_NE(a, nullptr);
  EXPECT_TRUE(env_->IsInstanceOf(a, array_class));
  EXPECT_EQ(0, env_->GetArrayLength(a));

  a = env_->NewObjectArray(1, element_class, nullptr);
  EXPECT_NE(a, nullptr);
  EXPECT_TRUE(env_->IsInstanceOf(a, array_class));
  EXPECT_EQ(1, env_->GetArrayLength(a));
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(a, 0), nullptr));

  // Negative array length checks.
  CheckJniAbortCatcher jni_abort_catcher;
  env_->NewObjectArray(-1, element_class, nullptr);
  jni_abort_catcher.Check("negative array length: -1");

  env_->NewObjectArray(std::numeric_limits<jint>::min(), element_class, nullptr);
  jni_abort_catcher.Check("negative array length: -2147483648");
}

TEST_F(JniInternalTest, NewObjectArrayWithPrimitiveClasses) {
  const char* primitive_descriptors = "VZBSCIJFD";
  const char* primitive_names[] = {
      "void", "boolean", "byte", "short", "char", "int", "long", "float", "double"
  };
  ASSERT_EQ(strlen(primitive_descriptors), arraysize(primitive_names));

  CheckJniAbortCatcher jni_abort_catcher;
  for (size_t i = 0; i < strlen(primitive_descriptors); ++i) {
    env_->NewObjectArray(0, nullptr, nullptr);
    jni_abort_catcher.Check("element_jclass == null");
    jclass primitive_class = GetPrimitiveClass(primitive_descriptors[i]);
    env_->NewObjectArray(1, primitive_class, nullptr);
    std::string error_msg(StringPrintf("not an object type: %s", primitive_names[i]));
    jni_abort_catcher.Check(error_msg.c_str());
  }
}

TEST_F(JniInternalTest, NewObjectArrayWithInitialValue) {
  jclass element_class = env_->FindClass("java/lang/String");
  ASSERT_NE(element_class, nullptr);
  jclass array_class = env_->FindClass("[Ljava/lang/String;");
  ASSERT_NE(array_class, nullptr);

  jstring s = env_->NewStringUTF("poop");
  jobjectArray a = env_->NewObjectArray(2, element_class, s);
  EXPECT_NE(a, nullptr);
  EXPECT_TRUE(env_->IsInstanceOf(a, array_class));
  EXPECT_EQ(2, env_->GetArrayLength(a));
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(a, 0), s));
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(a, 1), s));

  // Attempt to incorrect create an array of strings with initial value of string arrays.
  CheckJniAbortCatcher jni_abort_catcher;
  env_->NewObjectArray(2, element_class, a);
  jni_abort_catcher.Check("cannot assign object of type 'java.lang.String[]' to array with element "
                          "type of 'java.lang.String'");
}

TEST_F(JniInternalTest, GetArrayLength) {
  // Already tested in NewObjectArray/NewPrimitiveArray.
}

TEST_F(JniInternalTest, GetObjectClass) {
  jclass string_class = env_->FindClass("java/lang/String");
  ASSERT_NE(string_class, nullptr);
  jclass class_class = env_->FindClass("java/lang/Class");
  ASSERT_NE(class_class, nullptr);

  jstring s = env_->NewStringUTF("poop");
  jclass c = env_->GetObjectClass(s);
  ASSERT_TRUE(env_->IsSameObject(string_class, c));

  jclass c2 = env_->GetObjectClass(c);
  ASSERT_TRUE(env_->IsSameObject(class_class, env_->GetObjectClass(c2)));

  // Null as object should fail.
  CheckJniAbortCatcher jni_abort_catcher;
  EXPECT_EQ(env_->GetObjectClass(nullptr), nullptr);
  jni_abort_catcher.Check("java_object == null");
}

TEST_F(JniInternalTest, GetSuperclass) {
  jclass object_class = env_->FindClass("java/lang/Object");
  ASSERT_NE(object_class, nullptr);
  jclass string_class = env_->FindClass("java/lang/String");
  ASSERT_NE(string_class, nullptr);
  jclass runnable_interface = env_->FindClass("java/lang/Runnable");
  ASSERT_NE(runnable_interface, nullptr);
  ASSERT_TRUE(env_->IsSameObject(object_class, env_->GetSuperclass(string_class)));
  ASSERT_EQ(env_->GetSuperclass(object_class), nullptr);
  ASSERT_TRUE(env_->IsSameObject(object_class, env_->GetSuperclass(runnable_interface)));

  // Null as class should fail.
  CheckJniAbortCatcher jni_abort_catcher;
  EXPECT_EQ(env_->GetSuperclass(nullptr), nullptr);
  jni_abort_catcher.Check("java_class == null");
}

TEST_F(JniInternalTest, IsAssignableFrom) {
  jclass object_class = env_->FindClass("java/lang/Object");
  ASSERT_NE(object_class, nullptr);
  jclass string_class = env_->FindClass("java/lang/String");
  ASSERT_NE(string_class, nullptr);

  ASSERT_TRUE(env_->IsAssignableFrom(object_class, string_class));
  ASSERT_FALSE(env_->IsAssignableFrom(string_class, object_class));

  // Null as either class should fail.
  CheckJniAbortCatcher jni_abort_catcher;
  EXPECT_EQ(env_->IsAssignableFrom(nullptr, string_class), JNI_FALSE);
  jni_abort_catcher.Check("java_class1 == null");
  EXPECT_EQ(env_->IsAssignableFrom(object_class, nullptr), JNI_FALSE);
  jni_abort_catcher.Check("java_class2 == null");
}

TEST_F(JniInternalTest, GetObjectRefType) {
  jclass local = env_->FindClass("java/lang/Object");
  ASSERT_TRUE(local != nullptr);
  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(local));

  jobject global = env_->NewGlobalRef(local);
  EXPECT_EQ(JNIGlobalRefType, env_->GetObjectRefType(global));

  jweak weak_global = env_->NewWeakGlobalRef(local);
  EXPECT_EQ(JNIWeakGlobalRefType, env_->GetObjectRefType(weak_global));

  jobject invalid = reinterpret_cast<jobject>(this);
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(invalid));

  // TODO: invoke a native method and test that its arguments are considered local references.

  // Null as object should fail.
  CheckJniAbortCatcher jni_abort_catcher;
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(nullptr));
  jni_abort_catcher.Check("java_object == null");
}

TEST_F(JniInternalTest, StaleWeakGlobal) {
  jclass java_lang_Class = env_->FindClass("java/lang/Class");
  ASSERT_NE(java_lang_Class, nullptr);
  jobjectArray local_ref = env_->NewObjectArray(1, java_lang_Class, nullptr);
  ASSERT_NE(local_ref, nullptr);
  jweak weak_global = env_->NewWeakGlobalRef(local_ref);
  ASSERT_NE(weak_global, nullptr);
  env_->DeleteLocalRef(local_ref);
  Runtime::Current()->GetHeap()->CollectGarbage(false);  // GC should clear the weak global.
  jobject new_global_ref = env_->NewGlobalRef(weak_global);
  EXPECT_EQ(new_global_ref, nullptr);
  jobject new_local_ref = env_->NewLocalRef(weak_global);
  EXPECT_EQ(new_local_ref, nullptr);
}

TEST_F(JniInternalTest, NewStringUTF) {
  EXPECT_EQ(env_->NewStringUTF(nullptr), nullptr);
  jstring s;

  s = env_->NewStringUTF("");
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(0, env_->GetStringLength(s));
  EXPECT_EQ(0, env_->GetStringUTFLength(s));
  s = env_->NewStringUTF("hello");
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(5, env_->GetStringLength(s));
  EXPECT_EQ(5, env_->GetStringUTFLength(s));

  // TODO: check some non-ASCII strings.
}

TEST_F(JniInternalTest, NewString) {
  jchar chars[] = { 'h', 'i' };
  jstring s;
  s = env_->NewString(chars, 0);
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(0, env_->GetStringLength(s));
  EXPECT_EQ(0, env_->GetStringUTFLength(s));
  s = env_->NewString(chars, 2);
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(2, env_->GetStringLength(s));
  EXPECT_EQ(2, env_->GetStringUTFLength(s));

  // TODO: check some non-ASCII strings.
}

TEST_F(JniInternalTest, NewStringNullCharsZeroLength) {
  jstring s = env_->NewString(nullptr, 0);
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(0, env_->GetStringLength(s));
}

TEST_F(JniInternalTest, NewStringNullCharsNonzeroLength) {
  CheckJniAbortCatcher jni_abort_catcher;
  env_->NewString(nullptr, 1);
  jni_abort_catcher.Check("chars == null && char_count > 0");
}

TEST_F(JniInternalTest, NewStringNegativeLength) {
  CheckJniAbortCatcher jni_abort_catcher;
  env_->NewString(nullptr, -1);
  jni_abort_catcher.Check("char_count < 0: -1");
  env_->NewString(nullptr, std::numeric_limits<jint>::min());
  jni_abort_catcher.Check("char_count < 0: -2147483648");
}

TEST_F(JniInternalTest, GetStringLength_GetStringUTFLength) {
  // Already tested in the NewString/NewStringUTF tests.
}

TEST_F(JniInternalTest, GetStringRegion_GetStringUTFRegion) {
  jstring s = env_->NewStringUTF("hello");
  ASSERT_TRUE(s != nullptr);

  env_->GetStringRegion(s, -1, 0, nullptr);
  ExpectException(sioobe_);
  env_->GetStringRegion(s, 0, -1, nullptr);
  ExpectException(sioobe_);
  env_->GetStringRegion(s, 0, 10, nullptr);
  ExpectException(sioobe_);
  env_->GetStringRegion(s, 10, 1, nullptr);
  ExpectException(sioobe_);

  jchar chars[4] = { 'x', 'x', 'x', 'x' };
  env_->GetStringRegion(s, 1, 2, &chars[1]);
  EXPECT_EQ('x', chars[0]);
  EXPECT_EQ('e', chars[1]);
  EXPECT_EQ('l', chars[2]);
  EXPECT_EQ('x', chars[3]);

  // It's okay for the buffer to be nullptr as long as the length is 0.
  env_->GetStringRegion(s, 2, 0, nullptr);
  // Even if the offset is invalid...
  env_->GetStringRegion(s, 123, 0, nullptr);
  ExpectException(sioobe_);

  env_->GetStringUTFRegion(s, -1, 0, nullptr);
  ExpectException(sioobe_);
  env_->GetStringUTFRegion(s, 0, -1, nullptr);
  ExpectException(sioobe_);
  env_->GetStringUTFRegion(s, 0, 10, nullptr);
  ExpectException(sioobe_);
  env_->GetStringUTFRegion(s, 10, 1, nullptr);
  ExpectException(sioobe_);

  char bytes[4] = { 'x', 'x', 'x', 'x' };
  env_->GetStringUTFRegion(s, 1, 2, &bytes[1]);
  EXPECT_EQ('x', bytes[0]);
  EXPECT_EQ('e', bytes[1]);
  EXPECT_EQ('l', bytes[2]);
  EXPECT_EQ('x', bytes[3]);

  // It's okay for the buffer to be nullptr as long as the length is 0.
  env_->GetStringUTFRegion(s, 2, 0, nullptr);
  // Even if the offset is invalid...
  env_->GetStringUTFRegion(s, 123, 0, nullptr);
  ExpectException(sioobe_);
}

TEST_F(JniInternalTest, GetStringUTFChars_ReleaseStringUTFChars) {
  // Passing in a nullptr jstring is ignored normally, but caught by -Xcheck:jni.
  {
    CheckJniAbortCatcher check_jni_abort_catcher;
    EXPECT_EQ(env_->GetStringUTFChars(nullptr, nullptr), nullptr);
    check_jni_abort_catcher.Check("GetStringUTFChars received null jstring");
  }

  jstring s = env_->NewStringUTF("hello");
  ASSERT_TRUE(s != nullptr);

  const char* utf = env_->GetStringUTFChars(s, nullptr);
  EXPECT_STREQ("hello", utf);
  env_->ReleaseStringUTFChars(s, utf);

  jboolean is_copy = JNI_FALSE;
  utf = env_->GetStringUTFChars(s, &is_copy);
  EXPECT_EQ(JNI_TRUE, is_copy);
  EXPECT_STREQ("hello", utf);
  env_->ReleaseStringUTFChars(s, utf);
}

TEST_F(JniInternalTest, GetStringChars_ReleaseStringChars) {
  jstring s = env_->NewStringUTF("hello");
  ASSERT_TRUE(s != nullptr);

  jchar expected[] = { 'h', 'e', 'l', 'l', 'o' };
  const jchar* chars = env_->GetStringChars(s, nullptr);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringChars(s, chars);

  jboolean is_copy = JNI_FALSE;
  chars = env_->GetStringChars(s, &is_copy);
  EXPECT_EQ(JNI_TRUE, is_copy);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringChars(s, chars);
}

TEST_F(JniInternalTest, GetStringCritical_ReleaseStringCritical) {
  jstring s = env_->NewStringUTF("hello");
  ASSERT_TRUE(s != nullptr);

  jchar expected[] = { 'h', 'e', 'l', 'l', 'o' };
  const jchar* chars = env_->GetStringCritical(s, nullptr);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringCritical(s, chars);

  jboolean is_copy = JNI_FALSE;
  chars = env_->GetStringCritical(s, &is_copy);
  // TODO: Fix GetStringCritical to use the same mechanism as GetPrimitiveArrayElementsCritical.
  EXPECT_EQ(JNI_TRUE, is_copy);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringCritical(s, chars);
}

TEST_F(JniInternalTest, GetObjectArrayElement_SetObjectArrayElement) {
  jclass java_lang_Class = env_->FindClass("java/lang/Class");
  ASSERT_TRUE(java_lang_Class != nullptr);

  jobjectArray array = env_->NewObjectArray(1, java_lang_Class, nullptr);
  EXPECT_NE(array, nullptr);
  EXPECT_EQ(env_->GetObjectArrayElement(array, 0), nullptr);
  env_->SetObjectArrayElement(array, 0, java_lang_Class);
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(array, 0), java_lang_Class));

  // ArrayIndexOutOfBounds for negative index.
  env_->SetObjectArrayElement(array, -1, java_lang_Class);
  ExpectException(aioobe_);

  // ArrayIndexOutOfBounds for too-large index.
  env_->SetObjectArrayElement(array, 1, java_lang_Class);
  ExpectException(aioobe_);

  // ArrayStoreException thrown for bad types.
  env_->SetObjectArrayElement(array, 0, env_->NewStringUTF("not a jclass!"));
  ExpectException(ase_);

  // Null as array should fail.
  CheckJniAbortCatcher jni_abort_catcher;
  EXPECT_EQ(nullptr, env_->GetObjectArrayElement(nullptr, 0));
  jni_abort_catcher.Check("java_array == null");
  env_->SetObjectArrayElement(nullptr, 0, nullptr);
  jni_abort_catcher.Check("java_array == null");
}

#define EXPECT_STATIC_PRIMITIVE_FIELD(type, field_name, sig, value1, value2) \
  do { \
    jfieldID fid = env_->GetStaticFieldID(c, field_name, sig); \
    EXPECT_NE(fid, nullptr); \
    env_->SetStatic ## type ## Field(c, fid, value1); \
    EXPECT_EQ(value1, env_->GetStatic ## type ## Field(c, fid)); \
    env_->SetStatic ## type ## Field(c, fid, value2); \
    EXPECT_EQ(value2, env_->GetStatic ## type ## Field(c, fid)); \
    \
    CheckJniAbortCatcher jni_abort_catcher; \
    env_->GetStatic ## type ## Field(nullptr, fid); \
    jni_abort_catcher.Check("received null jclass"); \
    env_->SetStatic ## type ## Field(nullptr, fid, value1); \
    jni_abort_catcher.Check("received null jclass"); \
    env_->GetStatic ## type ## Field(c, nullptr); \
    jni_abort_catcher.Check("fid == null"); \
    env_->SetStatic ## type ## Field(c, nullptr, value1); \
    jni_abort_catcher.Check("fid == null"); \
  } while (false)

#define EXPECT_PRIMITIVE_FIELD(instance, type, field_name, sig, value1, value2) \
  do { \
    jfieldID fid = env_->GetFieldID(c, field_name, sig); \
    EXPECT_NE(fid, nullptr); \
    env_->Set ## type ## Field(instance, fid, value1); \
    EXPECT_EQ(value1, env_->Get ## type ## Field(instance, fid)); \
    env_->Set ## type ## Field(instance, fid, value2); \
    EXPECT_EQ(value2, env_->Get ## type ## Field(instance, fid)); \
    \
    CheckJniAbortCatcher jni_abort_catcher; \
    env_->Get ## type ## Field(nullptr, fid); \
    jni_abort_catcher.Check("obj == null"); \
    env_->Set ## type ## Field(nullptr, fid, value1); \
    jni_abort_catcher.Check("obj == null"); \
    env_->Get ## type ## Field(instance, nullptr); \
    jni_abort_catcher.Check("fid == null"); \
    env_->Set ## type ## Field(instance, nullptr, value1); \
    jni_abort_catcher.Check("fid == null"); \
  } while (false)


TEST_F(JniInternalTest, GetPrimitiveField_SetPrimitiveField) {
  TEST_DISABLED_FOR_PORTABLE();
  Thread::Current()->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  bool started = runtime_->Start();
  ASSERT_TRUE(started);

  jclass c = env_->FindClass("AllFields");
  ASSERT_NE(c, nullptr);
  jobject o = env_->AllocObject(c);
  ASSERT_NE(o, nullptr);

  EXPECT_STATIC_PRIMITIVE_FIELD(Boolean, "sZ", "Z", JNI_TRUE, JNI_FALSE);
  EXPECT_STATIC_PRIMITIVE_FIELD(Byte, "sB", "B", 1, 2);
  EXPECT_STATIC_PRIMITIVE_FIELD(Char, "sC", "C", 'a', 'b');
  EXPECT_STATIC_PRIMITIVE_FIELD(Double, "sD", "D", 1.0, 2.0);
  EXPECT_STATIC_PRIMITIVE_FIELD(Float, "sF", "F", 1.0, 2.0);
  EXPECT_STATIC_PRIMITIVE_FIELD(Int, "sI", "I", 1, 2);
  EXPECT_STATIC_PRIMITIVE_FIELD(Long, "sJ", "J", 1, 2);
  EXPECT_STATIC_PRIMITIVE_FIELD(Short, "sS", "S", 1, 2);

  EXPECT_PRIMITIVE_FIELD(o, Boolean, "iZ", "Z", JNI_TRUE, JNI_FALSE);
  EXPECT_PRIMITIVE_FIELD(o, Byte, "iB", "B", 1, 2);
  EXPECT_PRIMITIVE_FIELD(o, Char, "iC", "C", 'a', 'b');
  EXPECT_PRIMITIVE_FIELD(o, Double, "iD", "D", 1.0, 2.0);
  EXPECT_PRIMITIVE_FIELD(o, Float, "iF", "F", 1.0, 2.0);
  EXPECT_PRIMITIVE_FIELD(o, Int, "iI", "I", 1, 2);
  EXPECT_PRIMITIVE_FIELD(o, Long, "iJ", "J", 1, 2);
  EXPECT_PRIMITIVE_FIELD(o, Short, "iS", "S", 1, 2);
}

TEST_F(JniInternalTest, GetObjectField_SetObjectField) {
  TEST_DISABLED_FOR_PORTABLE();
  Thread::Current()->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  runtime_->Start();

  jclass c = env_->FindClass("AllFields");
  ASSERT_NE(c, nullptr);
  jobject o = env_->AllocObject(c);
  ASSERT_NE(o, nullptr);

  jstring s1 = env_->NewStringUTF("hello");
  ASSERT_NE(s1, nullptr);
  jstring s2 = env_->NewStringUTF("world");
  ASSERT_NE(s2, nullptr);

  jfieldID s_fid = env_->GetStaticFieldID(c, "sObject", "Ljava/lang/Object;");
  ASSERT_NE(s_fid, nullptr);
  jfieldID i_fid = env_->GetFieldID(c, "iObject", "Ljava/lang/Object;");
  ASSERT_NE(i_fid, nullptr);

  env_->SetStaticObjectField(c, s_fid, s1);
  ASSERT_TRUE(env_->IsSameObject(s1, env_->GetStaticObjectField(c, s_fid)));
  env_->SetStaticObjectField(c, s_fid, s2);
  ASSERT_TRUE(env_->IsSameObject(s2, env_->GetStaticObjectField(c, s_fid)));

  env_->SetObjectField(o, i_fid, s1);
  ASSERT_TRUE(env_->IsSameObject(s1, env_->GetObjectField(o, i_fid)));
  env_->SetObjectField(o, i_fid, s2);
  ASSERT_TRUE(env_->IsSameObject(s2, env_->GetObjectField(o, i_fid)));
}

TEST_F(JniInternalTest, NewLocalRef_nullptr) {
  EXPECT_EQ(env_->NewLocalRef(nullptr), nullptr);
}

TEST_F(JniInternalTest, NewLocalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);
  jobject o = env_->NewLocalRef(s);
  EXPECT_NE(o, nullptr);
  EXPECT_NE(o, s);

  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(o));
}

TEST_F(JniInternalTest, DeleteLocalRef_nullptr) {
  env_->DeleteLocalRef(nullptr);
}

TEST_F(JniInternalTest, DeleteLocalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);
  env_->DeleteLocalRef(s);

  // Currently, deleting an already-deleted reference is just a CheckJNI warning.
  {
    CheckJniAbortCatcher check_jni_abort_catcher;
    env_->DeleteLocalRef(s);

    std::string expected(StringPrintf("native code passing in reference to "
                                      "invalid local reference: %p", s));
    check_jni_abort_catcher.Check(expected.c_str());
  }

  s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);
  jobject o = env_->NewLocalRef(s);
  ASSERT_NE(o, nullptr);

  env_->DeleteLocalRef(s);
  env_->DeleteLocalRef(o);
}

TEST_F(JniInternalTest, PushLocalFrame_10395422) {
  // The JNI specification is ambiguous about whether the given capacity is to be interpreted as a
  // maximum or as a minimum, but it seems like it's supposed to be a minimum, and that's how
  // Android historically treated it, and it's how the RI treats it. It's also the more useful
  // interpretation!
  ASSERT_EQ(JNI_OK, env_->PushLocalFrame(0));
  env_->PopLocalFrame(nullptr);

  // Negative capacities are not allowed.
  ASSERT_EQ(JNI_ERR, env_->PushLocalFrame(-1));

  // And it's okay to have an upper limit. Ours is currently 512.
  ASSERT_EQ(JNI_ERR, env_->PushLocalFrame(8192));
}

TEST_F(JniInternalTest, PushLocalFrame_PopLocalFrame) {
  jobject original = env_->NewStringUTF("");
  ASSERT_NE(original, nullptr);

  jobject outer;
  jobject inner1, inner2;
  ScopedObjectAccess soa(env_);
  mirror::Object* inner2_direct_pointer;
  {
    ASSERT_EQ(JNI_OK, env_->PushLocalFrame(4));
    outer = env_->NewLocalRef(original);

    {
      ASSERT_EQ(JNI_OK, env_->PushLocalFrame(4));
      inner1 = env_->NewLocalRef(outer);
      inner2 = env_->NewStringUTF("survivor");
      inner2_direct_pointer = soa.Decode<mirror::Object*>(inner2);
      env_->PopLocalFrame(inner2);
    }

    EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(original));
    EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(outer));
    EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner1));

    // Our local reference for the survivor is invalid because the survivor
    // gets a new local reference...
    EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner2));

    env_->PopLocalFrame(nullptr);
  }
  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(original));
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(outer));
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner1));
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner2));
}

TEST_F(JniInternalTest, NewGlobalRef_nullptr) {
  EXPECT_EQ(env_->NewGlobalRef(nullptr), nullptr);
}

TEST_F(JniInternalTest, NewGlobalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);
  jobject o = env_->NewGlobalRef(s);
  EXPECT_NE(o, nullptr);
  EXPECT_NE(o, s);

  EXPECT_EQ(env_->GetObjectRefType(o), JNIGlobalRefType);
}

TEST_F(JniInternalTest, DeleteGlobalRef_nullptr) {
  env_->DeleteGlobalRef(nullptr);
}

TEST_F(JniInternalTest, DeleteGlobalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);

  jobject o = env_->NewGlobalRef(s);
  ASSERT_NE(o, nullptr);
  env_->DeleteGlobalRef(o);

  // Currently, deleting an already-deleted reference is just a CheckJNI warning.
  {
    CheckJniAbortCatcher check_jni_abort_catcher;
    env_->DeleteGlobalRef(o);

    std::string expected(StringPrintf("native code passing in reference to "
                                      "invalid global reference: %p", o));
    check_jni_abort_catcher.Check(expected.c_str());
  }

  jobject o1 = env_->NewGlobalRef(s);
  ASSERT_NE(o1, nullptr);
  jobject o2 = env_->NewGlobalRef(s);
  ASSERT_NE(o2, nullptr);

  env_->DeleteGlobalRef(o1);
  env_->DeleteGlobalRef(o2);
}

TEST_F(JniInternalTest, NewWeakGlobalRef_nullptr) {
  EXPECT_EQ(env_->NewWeakGlobalRef(nullptr),   nullptr);
}

TEST_F(JniInternalTest, NewWeakGlobalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);
  jobject o = env_->NewWeakGlobalRef(s);
  EXPECT_NE(o, nullptr);
  EXPECT_NE(o, s);

  EXPECT_EQ(env_->GetObjectRefType(o), JNIWeakGlobalRefType);
}

TEST_F(JniInternalTest, DeleteWeakGlobalRef_nullptr) {
  env_->DeleteWeakGlobalRef(nullptr);
}

TEST_F(JniInternalTest, DeleteWeakGlobalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);

  jobject o = env_->NewWeakGlobalRef(s);
  ASSERT_NE(o, nullptr);
  env_->DeleteWeakGlobalRef(o);

  // Currently, deleting an already-deleted reference is just a CheckJNI warning.
  {
    CheckJniAbortCatcher check_jni_abort_catcher;
    env_->DeleteWeakGlobalRef(o);

    std::string expected(StringPrintf("native code passing in reference to "
                                      "invalid weak global reference: %p", o));
    check_jni_abort_catcher.Check(expected.c_str());
  }

  jobject o1 = env_->NewWeakGlobalRef(s);
  ASSERT_NE(o1, nullptr);
  jobject o2 = env_->NewWeakGlobalRef(s);
  ASSERT_NE(o2, nullptr);

  env_->DeleteWeakGlobalRef(o1);
  env_->DeleteWeakGlobalRef(o2);
}

TEST_F(JniInternalTest, Throw) {
  EXPECT_EQ(JNI_ERR, env_->Throw(nullptr));

  jclass exception_class = env_->FindClass("java/lang/RuntimeException");
  ASSERT_TRUE(exception_class != nullptr);
  jthrowable exception = reinterpret_cast<jthrowable>(env_->AllocObject(exception_class));
  ASSERT_TRUE(exception != nullptr);

  EXPECT_EQ(JNI_OK, env_->Throw(exception));
  EXPECT_TRUE(env_->ExceptionCheck());
  jthrowable thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsSameObject(exception, thrown_exception));
}

TEST_F(JniInternalTest, ThrowNew) {
  EXPECT_EQ(JNI_ERR, env_->Throw(nullptr));

  jclass exception_class = env_->FindClass("java/lang/RuntimeException");
  ASSERT_TRUE(exception_class != nullptr);

  jthrowable thrown_exception;

  EXPECT_EQ(JNI_OK, env_->ThrowNew(exception_class, "hello world"));
  EXPECT_TRUE(env_->ExceptionCheck());
  thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(thrown_exception, exception_class));

  EXPECT_EQ(JNI_OK, env_->ThrowNew(exception_class, nullptr));
  EXPECT_TRUE(env_->ExceptionCheck());
  thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(thrown_exception, exception_class));
}

TEST_F(JniInternalTest, NewDirectBuffer_GetDirectBufferAddress_GetDirectBufferCapacity) {
  // Start runtime.
  Thread* self = Thread::Current();
  self->TransitionFromSuspendedToRunnable();
  MakeExecutable(nullptr, "java.lang.Class");
  MakeExecutable(nullptr, "java.lang.Object");
  MakeExecutable(nullptr, "java.nio.DirectByteBuffer");
  MakeExecutable(nullptr, "java.nio.MemoryBlock");
  MakeExecutable(nullptr, "java.nio.MemoryBlock$UnmanagedBlock");
  MakeExecutable(nullptr, "java.nio.MappedByteBuffer");
  MakeExecutable(nullptr, "java.nio.ByteBuffer");
  MakeExecutable(nullptr, "java.nio.Buffer");
  // TODO: we only load a dex file here as starting the runtime relies upon it.
  const char* class_name = "StaticLeafMethods";
  LoadDex(class_name);
  bool started = runtime_->Start();
  ASSERT_TRUE(started);

  jclass buffer_class = env_->FindClass("java/nio/Buffer");
  ASSERT_NE(buffer_class, nullptr);

  char bytes[1024];
  jobject buffer = env_->NewDirectByteBuffer(bytes, sizeof(bytes));
  ASSERT_NE(buffer, nullptr);
  ASSERT_TRUE(env_->IsInstanceOf(buffer, buffer_class));
  ASSERT_EQ(env_->GetDirectBufferAddress(buffer), bytes);
  ASSERT_EQ(env_->GetDirectBufferCapacity(buffer), static_cast<jlong>(sizeof(bytes)));
}

TEST_F(JniInternalTest, MonitorEnterExit) {
  // Create an object to torture.
  jclass object_class = env_->FindClass("java/lang/Object");
  ASSERT_NE(object_class, nullptr);
  jobject object = env_->AllocObject(object_class);
  ASSERT_NE(object, nullptr);

  // Expected class of exceptions
  jclass imse_class = env_->FindClass("java/lang/IllegalMonitorStateException");
  ASSERT_NE(imse_class, nullptr);

  jthrowable thrown_exception;

  // Unlock of unowned monitor
  env_->MonitorExit(object);
  EXPECT_TRUE(env_->ExceptionCheck());
  thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(thrown_exception, imse_class));

  // Lock of unowned monitor
  env_->MonitorEnter(object);
  EXPECT_FALSE(env_->ExceptionCheck());
  // Regular unlock
  env_->MonitorExit(object);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Recursively lock a lot
  size_t max_recursive_lock = 1024;
  for (size_t i = 0; i < max_recursive_lock; i++) {
    env_->MonitorEnter(object);
    EXPECT_FALSE(env_->ExceptionCheck());
  }
  // Recursively unlock a lot
  for (size_t i = 0; i < max_recursive_lock; i++) {
    env_->MonitorExit(object);
    EXPECT_FALSE(env_->ExceptionCheck());
  }

  // Unlock of unowned monitor
  env_->MonitorExit(object);
  EXPECT_TRUE(env_->ExceptionCheck());
  thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(thrown_exception, imse_class));

  // It's an error to call MonitorEnter or MonitorExit on nullptr.
  {
    CheckJniAbortCatcher check_jni_abort_catcher;
    env_->MonitorEnter(nullptr);
    check_jni_abort_catcher.Check("in call to MonitorEnter");

    env_->MonitorExit(nullptr);
    check_jni_abort_catcher.Check("in call to MonitorExit");
  }
}

TEST_F(JniInternalTest, DetachCurrentThread) {
  CleanUpJniEnv();  // cleanup now so TearDown won't have junk from wrong JNIEnv
  jint ok = vm_->DetachCurrentThread();
  EXPECT_EQ(JNI_OK, ok);

  jint err = vm_->DetachCurrentThread();
  EXPECT_EQ(JNI_ERR, err);
  vm_->AttachCurrentThread(&env_, nullptr);  // need attached thread for CommonRuntimeTest::TearDown
}

}  // namespace art
