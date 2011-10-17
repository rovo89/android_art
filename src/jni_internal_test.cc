// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include <sys/mman.h>

#include <cmath>

#include "common_test.h"
#include "ScopedLocalRef.h"

namespace art {

class JniInternalTest : public CommonTest {
 protected:
  virtual void SetUp() {
    CommonTest::SetUp();

    vm_ = Runtime::Current()->GetJavaVM();

    // Turn on -verbose:jni for the JNI tests.
    vm_->verbose_jni = true;

    env_ = Thread::Current()->GetJniEnv();

    ScopedLocalRef<jclass> aioobe(env_, env_->FindClass("java/lang/ArrayIndexOutOfBoundsException"));
    CHECK(aioobe.get() != NULL);
    aioobe_ = reinterpret_cast<jclass>(env_->NewGlobalRef(aioobe.get()));

    ScopedLocalRef<jclass> sioobe(env_, env_->FindClass("java/lang/StringIndexOutOfBoundsException"));
    CHECK(sioobe.get() != NULL);
    sioobe_ = reinterpret_cast<jclass>(env_->NewGlobalRef(sioobe.get()));
  }

  virtual void TearDown() {
    env_->DeleteGlobalRef(aioobe_);
    env_->DeleteGlobalRef(sioobe_);
    CommonTest::TearDown();
  }

  JavaVMExt* vm_;
  JNIEnvExt* env_;
  jclass aioobe_;
  jclass sioobe_;
};

TEST_F(JniInternalTest, AllocObject) {
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_TRUE(c != NULL);
  jobject o = env_->AllocObject(c);
  ASSERT_TRUE(o != NULL);

  // We have an instance of the class we asked for...
  ASSERT_TRUE(env_->IsInstanceOf(o, c));
  // ...whose fields haven't been initialized because
  // we didn't call a constructor.
  ASSERT_EQ(0, env_->GetIntField(o, env_->GetFieldID(c, "count", "I")));
  ASSERT_EQ(0, env_->GetIntField(o, env_->GetFieldID(c, "offset", "I")));
  ASSERT_TRUE(env_->GetObjectField(o, env_->GetFieldID(c, "value", "[C")) == NULL);
}

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

std::string gCheckJniAbortMessage;
void TestCheckJniAbortHook(const std::string& reason) {
  gCheckJniAbortMessage = reason;
}

TEST_F(JniInternalTest, FindClass) {
  // Reference types...
  EXPECT_CLASS_FOUND("java/lang/String");
  // ...for arrays too, where you must include "L;".
  EXPECT_CLASS_FOUND("[Ljava/lang/String;");

  vm_->check_jni_abort_hook = TestCheckJniAbortHook;
  // We support . as well as / for compatibility, if -Xcheck:jni is off.
  EXPECT_CLASS_FOUND("java.lang.String");
  EXPECT_CLASS_NOT_FOUND("Ljava.lang.String;");
  EXPECT_CLASS_FOUND("[Ljava.lang.String;");
  EXPECT_CLASS_NOT_FOUND("[java.lang.String");

  // You can't include the "L;" in a JNI class descriptor.
  EXPECT_CLASS_NOT_FOUND("Ljava/lang/String;");
  // But you must include it for an array of any reference type.
  EXPECT_CLASS_NOT_FOUND("[java/lang/String");
  vm_->check_jni_abort_hook = NULL;

  // Primitive arrays are okay (if the primitive type is valid)...
  EXPECT_CLASS_FOUND("[C");
  vm_->check_jni_abort_hook = TestCheckJniAbortHook;
  EXPECT_CLASS_NOT_FOUND("[K");
  vm_->check_jni_abort_hook = NULL;
  // But primitive types aren't allowed...
  EXPECT_CLASS_NOT_FOUND("C");
  EXPECT_CLASS_NOT_FOUND("K");
}

#define EXPECT_EXCEPTION(exception_class) \
  do { \
    EXPECT_TRUE(env_->ExceptionCheck()); \
    jthrowable exception = env_->ExceptionOccurred(); \
    EXPECT_NE(static_cast<jthrowable>(NULL), exception); \
    env_->ExceptionClear(); \
    EXPECT_TRUE(env_->IsInstanceOf(exception, exception_class)); \
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

  // Wrong type where type doesn't exist.
  fid = env_->GetFieldID(c, "count", "Lrod/jane/freddy;");
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

  // Wrong type where type doesn't exist.
  fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", "Lrod/jane/freddy;");
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

void BogusMethod() {
  // You can't pass NULL function pointers to RegisterNatives.
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
    JNINativeMethod methods[] = {{"getClass", "()Ljava/lang/Class;", reinterpret_cast<void*>(BogusMethod)}};
    env_->RegisterNatives(jlobject, methods, 1);
  }
  EXPECT_FALSE(env_->ExceptionCheck());

  env_->UnregisterNatives(jlobject);
}

#define EXPECT_PRIMITIVE_ARRAY(new_fn, get_region_fn, set_region_fn, get_elements_fn, release_elements_fn, scalar_type, expected_class_descriptor) \
  jsize size = 4; \
  /* Allocate an array and check it has the right type and length. */ \
  scalar_type ## Array a = env_->new_fn(size); \
  EXPECT_TRUE(a != NULL); \
  EXPECT_TRUE(env_->IsInstanceOf(a, env_->FindClass(expected_class_descriptor))); \
  EXPECT_EQ(size, env_->GetArrayLength(a)); \
  /* AIOOBE for negative start offset. */ \
  env_->get_region_fn(a, -1, 1, NULL); \
  EXPECT_EXCEPTION(aioobe_); \
  env_->set_region_fn(a, -1, 1, NULL); \
  EXPECT_EXCEPTION(aioobe_); \
  /* AIOOBE for negative length. */ \
  env_->get_region_fn(a, 0, -1, NULL); \
  EXPECT_EXCEPTION(aioobe_); \
  env_->set_region_fn(a, 0, -1, NULL); \
  EXPECT_EXCEPTION(aioobe_); \
  /* AIOOBE for buffer overrun. */ \
  env_->get_region_fn(a, size - 1, size, NULL); \
  EXPECT_EXCEPTION(aioobe_); \
  env_->set_region_fn(a, size - 1, size, NULL); \
  EXPECT_EXCEPTION(aioobe_); \
  /* Prepare a couple of buffers. */ \
  scalar_type src_buf[size]; \
  scalar_type dst_buf[size]; \
  for (jsize i = 0; i < size; ++i) { src_buf[i] = scalar_type(i); } \
  for (jsize i = 0; i < size; ++i) { dst_buf[i] = scalar_type(-1); } \
  /* Copy all of src_buf onto the heap. */ \
  env_->set_region_fn(a, 0, size, src_buf); \
  /* Copy back only part. */ \
  env_->get_region_fn(a, 1, size - 2, &dst_buf[1]); \
  EXPECT_NE(memcmp(src_buf, dst_buf, sizeof(src_buf)), 0) << "short copy equal"; \
  /* Copy the missing pieces. */ \
  env_->get_region_fn(a, 0, 1, dst_buf); \
  env_->get_region_fn(a, size - 1, 1, &dst_buf[size - 1]); \
  EXPECT_EQ(memcmp(src_buf, dst_buf, sizeof(src_buf)), 0) << "fixed copy not equal"; \
  /* Copy back the whole array. */ \
  env_->get_region_fn(a, 0, size, dst_buf); \
  EXPECT_EQ(memcmp(src_buf, dst_buf, sizeof(src_buf)), 0) << "full copy not equal"; \
  /* GetPrimitiveArrayCritical */ \
  void* v = env_->GetPrimitiveArrayCritical(a, NULL); \
  EXPECT_EQ(memcmp(src_buf, v, sizeof(src_buf)), 0) << "GetPrimitiveArrayCritical not equal"; \
  env_->ReleasePrimitiveArrayCritical(a, v, 0); \
  /* GetXArrayElements */ \
  scalar_type* xs = env_->get_elements_fn(a, NULL); \
  EXPECT_EQ(memcmp(src_buf, xs, sizeof(src_buf)), 0) << # get_elements_fn " not equal"; \
  env_->release_elements_fn(a, xs, 0); \
  EXPECT_EQ(reinterpret_cast<uintptr_t>(v), reinterpret_cast<uintptr_t>(xs))

TEST_F(JniInternalTest, BooleanArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewBooleanArray, GetBooleanArrayRegion, SetBooleanArrayRegion, GetBooleanArrayElements, ReleaseBooleanArrayElements, jboolean, "[Z");
}
TEST_F(JniInternalTest, ByteArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewByteArray, GetByteArrayRegion, SetByteArrayRegion, GetByteArrayElements, ReleaseByteArrayElements, jbyte, "[B");
}
TEST_F(JniInternalTest, CharArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewCharArray, GetCharArrayRegion, SetCharArrayRegion, GetCharArrayElements, ReleaseCharArrayElements, jchar, "[C");
}
TEST_F(JniInternalTest, DoubleArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewDoubleArray, GetDoubleArrayRegion, SetDoubleArrayRegion, GetDoubleArrayElements, ReleaseDoubleArrayElements, jdouble, "[D");
}
TEST_F(JniInternalTest, FloatArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewFloatArray, GetFloatArrayRegion, SetFloatArrayRegion, GetFloatArrayElements, ReleaseFloatArrayElements, jfloat, "[F");
}
TEST_F(JniInternalTest, IntArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewIntArray, GetIntArrayRegion, SetIntArrayRegion, GetIntArrayElements, ReleaseIntArrayElements, jint, "[I");
}
TEST_F(JniInternalTest, LongArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewLongArray, GetLongArrayRegion, SetLongArrayRegion, GetLongArrayElements, ReleaseLongArrayElements, jlong, "[J");
}
TEST_F(JniInternalTest, ShortArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewShortArray, GetShortArrayRegion, SetShortArrayRegion, GetShortArrayElements, ReleaseShortArrayElements, jshort, "[S");
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
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(a, 0), NULL));

  jstring s = env_->NewStringUTF("poop");
  a = env_->NewObjectArray(2, element_class, s);
  EXPECT_TRUE(a != NULL);
  EXPECT_TRUE(env_->IsInstanceOf(a, array_class));
  EXPECT_EQ(2, env_->GetArrayLength(a));
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(a, 0), s));
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(a, 1), s));
}

TEST_F(JniInternalTest, GetArrayLength) {
  // Already tested in NewObjectArray/NewPrimitiveArray.
}

TEST_F(JniInternalTest, GetObjectClass) {
  jclass string_class = env_->FindClass("java/lang/String");
  ASSERT_TRUE(string_class != NULL);
  jclass class_class = env_->FindClass("java/lang/Class");
  ASSERT_TRUE(class_class != NULL);

  jstring s = env_->NewStringUTF("poop");
  jclass c = env_->GetObjectClass(s);
  ASSERT_TRUE(env_->IsSameObject(string_class, c));

  jclass c2 = env_->GetObjectClass(c);
  ASSERT_TRUE(env_->IsSameObject(class_class, env_->GetObjectClass(c2)));
}

TEST_F(JniInternalTest, GetSuperclass) {
  jclass object_class = env_->FindClass("java/lang/Object");
  ASSERT_TRUE(object_class != NULL);
  jclass string_class = env_->FindClass("java/lang/String");
  ASSERT_TRUE(string_class != NULL);
  ASSERT_TRUE(env_->IsSameObject(object_class, env_->GetSuperclass(string_class)));
  ASSERT_TRUE(env_->GetSuperclass(object_class) == NULL);
}

TEST_F(JniInternalTest, IsAssignableFrom) {
  jclass object_class = env_->FindClass("java/lang/Object");
  ASSERT_TRUE(object_class != NULL);
  jclass string_class = env_->FindClass("java/lang/String");
  ASSERT_TRUE(string_class != NULL);

  ASSERT_TRUE(env_->IsAssignableFrom(object_class, string_class));
  ASSERT_FALSE(env_->IsAssignableFrom(string_class, object_class));
}

TEST_F(JniInternalTest, GetObjectRefType) {
  jclass local = env_->FindClass("java/lang/Object");
  ASSERT_TRUE(local != NULL);
  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(local));

  jobject global = env_->NewGlobalRef(local);
  EXPECT_EQ(JNIGlobalRefType, env_->GetObjectRefType(global));

  jweak weak_global = env_->NewWeakGlobalRef(local);
  EXPECT_EQ(JNIWeakGlobalRefType, env_->GetObjectRefType(weak_global));

  jobject invalid = reinterpret_cast<jobject>(this);
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(invalid));

  // TODO: invoke a native method and test that its arguments are considered local references.
}

TEST_F(JniInternalTest, NewStringUTF) {
  EXPECT_TRUE(env_->NewStringUTF(NULL) == NULL);
  jstring s;

  s = env_->NewStringUTF("");
  EXPECT_TRUE(s != NULL);
  EXPECT_EQ(0, env_->GetStringLength(s));
  EXPECT_EQ(0, env_->GetStringUTFLength(s));
  s = env_->NewStringUTF("hello");
  EXPECT_TRUE(s != NULL);
  EXPECT_EQ(5, env_->GetStringLength(s));
  EXPECT_EQ(5, env_->GetStringUTFLength(s));

  // TODO: check some non-ASCII strings.
}

TEST_F(JniInternalTest, NewString) {
  EXPECT_TRUE(env_->NewString(NULL, 0) == NULL);

  jchar chars[] = { 'h', 'i' };
  jstring s;
  s = env_->NewString(chars, 0);
  EXPECT_TRUE(s != NULL);
  EXPECT_EQ(0, env_->GetStringLength(s));
  EXPECT_EQ(0, env_->GetStringUTFLength(s));
  s = env_->NewString(chars, 2);
  EXPECT_TRUE(s != NULL);
  EXPECT_EQ(2, env_->GetStringLength(s));
  EXPECT_EQ(2, env_->GetStringUTFLength(s));

  // TODO: check some non-ASCII strings.
}

TEST_F(JniInternalTest, GetStringLength_GetStringUTFLength) {
  // Already tested in the NewString/NewStringUTF tests.
}

TEST_F(JniInternalTest, GetStringRegion_GetStringUTFRegion) {
  jstring s = env_->NewStringUTF("hello");
  ASSERT_TRUE(s != NULL);

  env_->GetStringRegion(s, -1, 0, NULL);
  EXPECT_EXCEPTION(sioobe_);
  env_->GetStringRegion(s, 0, -1, NULL);
  EXPECT_EXCEPTION(sioobe_);
  env_->GetStringRegion(s, 0, 10, NULL);
  EXPECT_EXCEPTION(sioobe_);
  env_->GetStringRegion(s, 10, 1, NULL);
  EXPECT_EXCEPTION(sioobe_);

  jchar chars[4] = { 'x', 'x', 'x', 'x' };
  env_->GetStringRegion(s, 1, 2, &chars[1]);
  EXPECT_EQ('x', chars[0]);
  EXPECT_EQ('e', chars[1]);
  EXPECT_EQ('l', chars[2]);
  EXPECT_EQ('x', chars[3]);

  env_->GetStringUTFRegion(s, -1, 0, NULL);
  EXPECT_EXCEPTION(sioobe_);
  env_->GetStringUTFRegion(s, 0, -1, NULL);
  EXPECT_EXCEPTION(sioobe_);
  env_->GetStringUTFRegion(s, 0, 10, NULL);
  EXPECT_EXCEPTION(sioobe_);
  env_->GetStringUTFRegion(s, 10, 1, NULL);
  EXPECT_EXCEPTION(sioobe_);

  char bytes[4] = { 'x', 'x', 'x', 'x' };
  env_->GetStringUTFRegion(s, 1, 2, &bytes[1]);
  EXPECT_EQ('x', bytes[0]);
  EXPECT_EQ('e', bytes[1]);
  EXPECT_EQ('l', bytes[2]);
  EXPECT_EQ('x', bytes[3]);
}

TEST_F(JniInternalTest, GetStringUTFChars_ReleaseStringUTFChars) {
  vm_->check_jni_abort_hook = TestCheckJniAbortHook;
  // Passing in a NULL jstring is ignored normally, but caught by -Xcheck:jni.
  EXPECT_TRUE(env_->GetStringUTFChars(NULL, NULL) == NULL);
  vm_->check_jni_abort_hook = NULL;

  jstring s = env_->NewStringUTF("hello");
  ASSERT_TRUE(s != NULL);

  const char* utf = env_->GetStringUTFChars(s, NULL);
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
  ASSERT_TRUE(s != NULL);

  jchar expected[] = { 'h', 'e', 'l', 'l', 'o' };
  const jchar* chars = env_->GetStringChars(s, NULL);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringChars(s, chars);

  jboolean is_copy = JNI_FALSE;
  chars = env_->GetStringChars(s, &is_copy);
  EXPECT_EQ(JNI_FALSE, is_copy);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringChars(s, chars);
}

TEST_F(JniInternalTest, GetStringCritical_ReleaseStringCritical) {
  jstring s = env_->NewStringUTF("hello");
  ASSERT_TRUE(s != NULL);

  jchar expected[] = { 'h', 'e', 'l', 'l', 'o' };
  const jchar* chars = env_->GetStringCritical(s, NULL);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringCritical(s, chars);

  jboolean is_copy = JNI_FALSE;
  chars = env_->GetStringCritical(s, &is_copy);
  EXPECT_EQ(JNI_FALSE, is_copy);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringCritical(s, chars);
}

TEST_F(JniInternalTest, GetObjectArrayElement_SetObjectArrayElement) {
  jclass c = env_->FindClass("java/lang/Object");
  ASSERT_TRUE(c != NULL);

  jobjectArray array = env_->NewObjectArray(1, c, NULL);
  EXPECT_TRUE(array != NULL);
  EXPECT_TRUE(env_->GetObjectArrayElement(array, 0) == NULL);
  env_->SetObjectArrayElement(array, 0, c);
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(array, 0), c));

  // ArrayIndexOutOfBounds for negative index.
  env_->SetObjectArrayElement(array, -1, c);
  EXPECT_EXCEPTION(aioobe_);

  // ArrayIndexOutOfBounds for too-large index.
  env_->SetObjectArrayElement(array, 1, c);
  EXPECT_EXCEPTION(aioobe_);

  // TODO: check ArrayStoreException thrown for bad types.
}

#define EXPECT_STATIC_PRIMITIVE_FIELD(type, field_name, sig, value1, value2) \
  do { \
    jfieldID fid = env_->GetStaticFieldID(c, field_name, sig); \
    EXPECT_TRUE(fid != NULL); \
    env_->SetStatic ## type ## Field(c, fid, value1); \
    EXPECT_EQ(value1, env_->GetStatic ## type ## Field(c, fid)); \
    env_->SetStatic ## type ## Field(c, fid, value2); \
    EXPECT_EQ(value2, env_->GetStatic ## type ## Field(c, fid)); \
  } while (false)

#define EXPECT_PRIMITIVE_FIELD(instance, type, field_name, sig, value1, value2) \
  do { \
    jfieldID fid = env_->GetFieldID(c, field_name, sig); \
    EXPECT_TRUE(fid != NULL); \
    env_->Set ## type ## Field(instance, fid, value1); \
    EXPECT_EQ(value1, env_->Get ## type ## Field(instance, fid)); \
    env_->Set ## type ## Field(instance, fid, value2); \
    EXPECT_EQ(value2, env_->Get ## type ## Field(instance, fid)); \
  } while (false)


TEST_F(JniInternalTest, GetPrimitiveField_SetPrimitiveField) {
  LoadDex("AllFields");
  runtime_->Start();

  jclass c = env_->FindClass("AllFields");
  ASSERT_TRUE(c != NULL);
  jobject o = env_->AllocObject(c);
  ASSERT_TRUE(o != NULL);

  EXPECT_STATIC_PRIMITIVE_FIELD(Boolean, "sZ", "Z", true, false);
  EXPECT_STATIC_PRIMITIVE_FIELD(Byte, "sB", "B", 1, 2);
  EXPECT_STATIC_PRIMITIVE_FIELD(Char, "sC", "C", 'a', 'b');
  EXPECT_STATIC_PRIMITIVE_FIELD(Double, "sD", "D", 1.0, 2.0);
  EXPECT_STATIC_PRIMITIVE_FIELD(Float, "sF", "F", 1.0, 2.0);
  EXPECT_STATIC_PRIMITIVE_FIELD(Int, "sI", "I", 1, 2);
  EXPECT_STATIC_PRIMITIVE_FIELD(Long, "sJ", "J", 1, 2);
  EXPECT_STATIC_PRIMITIVE_FIELD(Short, "sS", "S", 1, 2);

  EXPECT_PRIMITIVE_FIELD(o, Boolean, "iZ", "Z", true, false);
  EXPECT_PRIMITIVE_FIELD(o, Byte, "iB", "B", 1, 2);
  EXPECT_PRIMITIVE_FIELD(o, Char, "iC", "C", 'a', 'b');
  EXPECT_PRIMITIVE_FIELD(o, Double, "iD", "D", 1.0, 2.0);
  EXPECT_PRIMITIVE_FIELD(o, Float, "iF", "F", 1.0, 2.0);
  EXPECT_PRIMITIVE_FIELD(o, Int, "iI", "I", 1, 2);
  EXPECT_PRIMITIVE_FIELD(o, Long, "iJ", "J", 1, 2);
  EXPECT_PRIMITIVE_FIELD(o, Short, "iS", "S", 1, 2);
}

TEST_F(JniInternalTest, GetObjectField_SetObjectField) {
  LoadDex("AllFields");
  runtime_->Start();

  jclass c = env_->FindClass("AllFields");
  ASSERT_TRUE(c != NULL);
  jobject o = env_->AllocObject(c);
  ASSERT_TRUE(o != NULL);

  jstring s1 = env_->NewStringUTF("hello");
  ASSERT_TRUE(s1 != NULL);
  jstring s2 = env_->NewStringUTF("world");
  ASSERT_TRUE(s2 != NULL);

  jfieldID s_fid = env_->GetStaticFieldID(c, "sObject", "Ljava/lang/Object;");
  ASSERT_TRUE(s_fid != NULL);
  jfieldID i_fid = env_->GetFieldID(c, "iObject", "Ljava/lang/Object;");
  ASSERT_TRUE(i_fid != NULL);

  env_->SetStaticObjectField(c, s_fid, s1);
  ASSERT_TRUE(env_->IsSameObject(s1, env_->GetStaticObjectField(c, s_fid)));
  env_->SetStaticObjectField(c, s_fid, s2);
  ASSERT_TRUE(env_->IsSameObject(s2, env_->GetStaticObjectField(c, s_fid)));

  env_->SetObjectField(o, i_fid, s1);
  ASSERT_TRUE(env_->IsSameObject(s1, env_->GetObjectField(o, i_fid)));
  env_->SetObjectField(o, i_fid, s2);
  ASSERT_TRUE(env_->IsSameObject(s2, env_->GetObjectField(o, i_fid)));
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

  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(o));
}

TEST_F(JniInternalTest, DeleteLocalRef_NULL) {
  env_->DeleteLocalRef(NULL);
}

TEST_F(JniInternalTest, DeleteLocalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_TRUE(s != NULL);
  env_->DeleteLocalRef(s);

  // Currently, deleting an already-deleted reference is just a warning.
  vm_->check_jni_abort_hook = TestCheckJniAbortHook;
  env_->DeleteLocalRef(s);
  vm_->check_jni_abort_hook = NULL;

  s = env_->NewStringUTF("");
  ASSERT_TRUE(s != NULL);
  jobject o = env_->NewLocalRef(s);
  ASSERT_TRUE(o != NULL);

  env_->DeleteLocalRef(s);
  env_->DeleteLocalRef(o);
}

TEST_F(JniInternalTest, PushLocalFrame_PopLocalFrame) {
  jobject original = env_->NewStringUTF("");
  ASSERT_TRUE(original != NULL);

  jobject outer;
  jobject inner1, inner2;
  Object* inner2_direct_pointer;
  {
    env_->PushLocalFrame(4);
    outer = env_->NewLocalRef(original);

    {
      env_->PushLocalFrame(4);
      inner1 = env_->NewLocalRef(outer);
      inner2 = env_->NewStringUTF("survivor");
      inner2_direct_pointer = Decode<Object*>(env_, inner2);
      env_->PopLocalFrame(inner2);
    }

    EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(original));
    EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(outer));
    EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner1));

    // Our local reference for the survivor is invalid because the survivor
    // gets a new local reference...
    EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner2));
    // ...but the survivor should be in the local reference table.
    EXPECT_TRUE(env_->locals.ContainsDirectPointer(inner2_direct_pointer));

    env_->PopLocalFrame(NULL);
  }
  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(original));
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(outer));
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner1));
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner2));
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

  vm_->check_jni_abort_hook = TestCheckJniAbortHook;
  // Currently, deleting an already-deleted reference is just a warning.
  env_->DeleteGlobalRef(o);
  vm_->check_jni_abort_hook = NULL;

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

  vm_->check_jni_abort_hook = TestCheckJniAbortHook;
  // Currently, deleting an already-deleted reference is just a warning.
  env_->DeleteWeakGlobalRef(o);
  vm_->check_jni_abort_hook = NULL;

  jobject o1 = env_->NewWeakGlobalRef(s);
  ASSERT_TRUE(o1 != NULL);
  jobject o2 = env_->NewWeakGlobalRef(s);
  ASSERT_TRUE(o2 != NULL);

  env_->DeleteWeakGlobalRef(o1);
  env_->DeleteWeakGlobalRef(o2);
}

#if defined(__arm__)
TEST_F(JniInternalTest, StaticMainMethod) {
  const ClassLoader* class_loader = LoadDex("Main");
  CompileDirectMethod(class_loader, "Main", "main", "([Ljava/lang/String;)V");

  Class* klass = class_linker_->FindClass("LMain;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("main", "([Ljava/lang/String;)V");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  Object* arg = NULL;

  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), NULL);
}

TEST_F(JniInternalTest, StaticNopMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "nop", "()V");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("nop", "()V");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  (*stub)(method, NULL, Thread::Current(), NULL, NULL);
}

TEST_F(JniInternalTest, StaticIdentityByteMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "identity", "(B)B");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("identity", "(B)B");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  int arg;
  JValue result;

  arg = 0;
  result.b = -1;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(0, result.b);

  arg = -1;
  result.b = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(-1, result.b);

  arg = SCHAR_MAX;
  result.b = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(SCHAR_MAX, result.b);

  arg = SCHAR_MIN;
  result.b = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(SCHAR_MIN, result.b);
}

TEST_F(JniInternalTest, StaticIdentityIntMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "identity", "(I)I");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("identity", "(I)I");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  int arg;
  JValue result;

  arg = 0;
  result.i = -1;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(0, result.i);

  arg = -1;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(-1, result.i);

  arg = INT_MAX;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(INT_MAX, result.i);

  arg = INT_MIN;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(INT_MIN, result.i);
}

TEST_F(JniInternalTest, StaticIdentityDoubleMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "identity", "(D)D");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("identity", "(D)D");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  double arg;
  JValue result;

  arg = 0.0;
  result.d = -1.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(0.0, result.d);

  arg = -1.0;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(-1.0, result.d);

  arg = DBL_MAX;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(DBL_MAX, result.d);

  arg = DBL_MIN;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(&arg), &result);
  EXPECT_EQ(DBL_MIN, result.d);
}

TEST_F(JniInternalTest, StaticSumIntIntMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "sum", "(II)I");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(II)I");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  int args[2];
  JValue result;

  args[0] = 0;
  args[1] = 0;
  result.i = -1;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0, result.i);

  args[0] = 1;
  args[1] = 2;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(3, result.i);

  args[0] = -2;
  args[1] = 5;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(3, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MIN;
  result.i = 1234;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-1, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MAX;
  result.i = INT_MIN;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-2, result.i);
}

TEST_F(JniInternalTest, StaticSumIntIntIntMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "sum", "(III)I");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(III)I");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  int args[3];
  JValue result;

  args[0] = 0;
  args[1] = 0;
  args[2] = 0;
  result.i = -1;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0, result.i);

  args[0] = 1;
  args[1] = 2;
  args[2] = 3;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(6, result.i);

  args[0] = -1;
  args[1] = 2;
  args[2] = -3;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-2, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MIN;
  args[2] = INT_MAX;
  result.i = 1234;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2147483646, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MAX;
  args[2] = INT_MAX;
  result.i = INT_MIN;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2147483645, result.i);
}

TEST_F(JniInternalTest, StaticSumIntIntIntIntMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "sum", "(IIII)I");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(IIII)I");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  int args[4];
  JValue result;

  args[0] = 0;
  args[1] = 0;
  args[2] = 0;
  args[3] = 0;
  result.i = -1;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0, result.i);

  args[0] = 1;
  args[1] = 2;
  args[2] = 3;
  args[3] = 4;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(10, result.i);

  args[0] = -1;
  args[1] = 2;
  args[2] = -3;
  args[3] = 4;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MIN;
  args[2] = INT_MAX;
  args[3] = INT_MIN;
  result.i = 1234;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-2, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MAX;
  args[2] = INT_MAX;
  args[3] = INT_MAX;
  result.i = INT_MIN;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-4, result.i);
}

TEST_F(JniInternalTest, StaticSumIntIntIntIntIntMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "sum", "(IIIII)I");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(IIIII)I");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  int args[5];
  JValue result;

  args[0] = 0;
  args[1] = 0;
  args[2] = 0;
  args[3] = 0;
  args[4] = 0;
  result.i = -1.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0, result.i);

  args[0] = 1;
  args[1] = 2;
  args[2] = 3;
  args[3] = 4;
  args[4] = 5;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(15, result.i);

  args[0] = -1;
  args[1] = 2;
  args[2] = -3;
  args[3] = 4;
  args[4] = -5;
  result.i = 0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-3, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MIN;
  args[2] = INT_MAX;
  args[3] = INT_MIN;
  args[4] = INT_MAX;
  result.i = 1234;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2147483645, result.i);

  args[0] = INT_MAX;
  args[1] = INT_MAX;
  args[2] = INT_MAX;
  args[3] = INT_MAX;
  args[4] = INT_MAX;
  result.i = INT_MIN;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2147483643, result.i);
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "sum", "(DD)D");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(DD)D");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  double args[2];
  JValue result;

  args[0] = 0.0;
  args[1] = 0.0;
  result.d = -1.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0.0, result.d);

  args[0] = 1.0;
  args[1] = 2.0;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(3.0, result.d);

  args[0] = 1.0;
  args[1] = -2.0;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-1.0, result.d);

  args[0] = DBL_MAX;
  args[1] = DBL_MIN;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(1.7976931348623157e308, result.d);

  args[0] = DBL_MAX;
  args[1] = DBL_MAX;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(INFINITY, result.d);
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleDoubleMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "sum", "(DDD)D");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(DDD)D");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  double args[3];
  JValue result;

  args[0] = 0.0;
  args[1] = 0.0;
  args[2] = 0.0;
  result.d = -1.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0.0, result.d);

  args[0] = 1.0;
  args[1] = 2.0;
  args[2] = 3.0;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(6.0, result.d);

  args[0] = 1.0;
  args[1] = -2.0;
  args[2] = 3.0;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(2.0, result.d);
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleDoubleDoubleMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "sum", "(DDDD)D");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(DDDD)D");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  double args[4];
  JValue result;

  args[0] = 0.0;
  args[1] = 0.0;
  args[2] = 0.0;
  args[3] = 0.0;
  result.d = -1.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0.0, result.d);

  args[0] = 1.0;
  args[1] = 2.0;
  args[2] = 3.0;
  args[3] = 4.0;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(10.0, result.d);

  args[0] = 1.0;
  args[1] = -2.0;
  args[2] = 3.0;
  args[3] = -4.0;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(-2.0, result.d);
}

TEST_F(JniInternalTest, StaticSumDoubleDoubleDoubleDoubleDoubleMethod) {
  const ClassLoader* class_loader = LoadDex("StaticLeafMethods");
  CompileDirectMethod(class_loader, "StaticLeafMethods", "sum", "(DDDDD)D");

  Class* klass = class_linker_->FindClass("LStaticLeafMethods;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod("sum", "(DDDDD)D");
  ASSERT_TRUE(method != NULL);

  Method::InvokeStub* stub = method->GetInvokeStub();

  double args[5];
  JValue result;

  args[0] = 0.0;
  args[1] = 0.0;
  args[2] = 0.0;
  args[3] = 0.0;
  args[4] = 0.0;
  result.d = -1.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(0.0, result.d);

  args[0] = 1.0;
  args[1] = 2.0;
  args[2] = 3.0;
  args[3] = 4.0;
  args[4] = 5.0;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(15.0, result.d);

  args[0] = 1.0;
  args[1] = -2.0;
  args[2] = 3.0;
  args[3] = -4.0;
  args[4] = 5.0;
  result.d = 0.0;
  (*stub)(method, NULL, Thread::Current(), reinterpret_cast<byte*>(args), &result);
  EXPECT_EQ(3.0, result.d);
}
#endif  // __arm__

TEST_F(JniInternalTest, Throw) {
  EXPECT_EQ(JNI_ERR, env_->Throw(NULL));

  jclass exception_class = env_->FindClass("java/lang/RuntimeException");
  ASSERT_TRUE(exception_class != NULL);
  jthrowable exception = reinterpret_cast<jthrowable>(env_->AllocObject(exception_class));
  ASSERT_TRUE(exception != NULL);

  EXPECT_EQ(JNI_OK, env_->Throw(exception));
  EXPECT_TRUE(env_->ExceptionCheck());
  jthrowable thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsSameObject(exception, thrown_exception));
}

TEST_F(JniInternalTest, ThrowNew) {
  EXPECT_EQ(JNI_ERR, env_->Throw(NULL));

  jclass exception_class = env_->FindClass("java/lang/RuntimeException");
  ASSERT_TRUE(exception_class != NULL);

  jthrowable thrown_exception;

  EXPECT_EQ(JNI_OK, env_->ThrowNew(exception_class, "hello world"));
  EXPECT_TRUE(env_->ExceptionCheck());
  thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(thrown_exception, exception_class));

  EXPECT_EQ(JNI_OK, env_->ThrowNew(exception_class, NULL));
  EXPECT_TRUE(env_->ExceptionCheck());
  thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(thrown_exception, exception_class));
}

// TODO: this test is DISABLED until we can actually run java.nio.Buffer's <init>.
TEST_F(JniInternalTest, DISABLED_NewDirectBuffer_GetDirectBufferAddress_GetDirectBufferCapacity) {
  jclass buffer_class = env_->FindClass("java/nio/Buffer");
  ASSERT_TRUE(buffer_class != NULL);

  char bytes[1024];
  jobject buffer = env_->NewDirectByteBuffer(bytes, sizeof(bytes));
  ASSERT_TRUE(buffer != NULL);
  ASSERT_TRUE(env_->IsInstanceOf(buffer, buffer_class));
  ASSERT_TRUE(env_->GetDirectBufferAddress(buffer) == bytes);
  ASSERT_TRUE(env_->GetDirectBufferCapacity(buffer) == sizeof(bytes));
}

}  // namespace art
