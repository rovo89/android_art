// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_compiler.h"

#include <sys/mman.h>

#include "UniquePtr.h"
#include "assembler.h"
#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "gtest/gtest.h"
#include "indirect_reference_table.h"
#include "jni_internal.h"
#include "mem_map.h"
#include "runtime.h"
#include "scoped_jni_thread_state.h"
#include "thread.h"

namespace art {

class JniCompilerTest : public CommonTest {
 protected:
  virtual void SetUp() {
    CommonTest::SetUp();
    class_loader_ = LoadDex("MyClassNatives");
  }

  void CompileForTest(bool direct, const char* method_name, const char* method_sig) {
    // Compile the native method before starting the runtime
    Class* c = class_linker_->FindClass("LMyClass;", class_loader_);
    Method* method;
    if (direct) {
      method = c->FindDirectMethod(method_name, method_sig);
    } else {
      method = c->FindVirtualMethod(method_name, method_sig);
    }
    ASSERT_TRUE(method != NULL);
    if (method->GetCode() != NULL) {
      return;
    }
    CompileMethod(method);
    ASSERT_TRUE(method->GetCode() != NULL);
  }

  void SetupForTest(bool direct, const char* method_name, const char* method_sig,
                    void* native_fnptr) {
    CompileForTest(direct, method_name, method_sig);
    if (!runtime_->IsStarted()) {
      runtime_->Start();
    }

    // JNI operations after runtime start
    env_ = Thread::Current()->GetJniEnv();
    jklass_ = env_->FindClass("MyClass");
    ASSERT_TRUE(jklass_ != NULL);

    if (direct) {
      jmethod_ = env_->GetStaticMethodID(jklass_, method_name, method_sig);
    } else {
      jmethod_ = env_->GetMethodID(jklass_, method_name, method_sig);
    }
    ASSERT_TRUE(jmethod_ != NULL);

    if (native_fnptr != NULL) {
      JNINativeMethod methods[] = {{method_name, method_sig, native_fnptr}};
      ASSERT_EQ(JNI_OK, env_->RegisterNatives(jklass_, methods, 1));
    } else {
      env_->UnregisterNatives(jklass_);
    }

    jmethodID constructor = env_->GetMethodID(jklass_, "<init>", "()V");
    jobj_ = env_->NewObject(jklass_, constructor);
    ASSERT_TRUE(jobj_ != NULL);
  }

 public:
  static jclass jklass_;
  static jobject jobj_;
 protected:
  const ClassLoader* class_loader_;
  JNIEnv* env_;
  jmethodID jmethod_;
};

jclass JniCompilerTest::jklass_;
jobject JniCompilerTest::jobj_;

int gJava_MyClass_foo_calls = 0;
void Java_MyClass_foo(JNIEnv* env, jobject thisObj) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_foo_calls++;
}

TEST_F(JniCompilerTest, CompileAndRunNoArgMethod) {
  SetupForTest(false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClass_foo));

  EXPECT_EQ(0, gJava_MyClass_foo_calls);
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClass_foo_calls);
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(2, gJava_MyClass_foo_calls);
}

TEST_F(JniCompilerTest, CompileAndRunIntMethodThroughStub) {
  SetupForTest(false,
               "bar",
               "(I)I",
               NULL /* dlsym will find &Java_MyClass_bar later */);

  std::string path("libarttest.so");
  std::string reason;
  ASSERT_TRUE(Runtime::Current()->GetJavaVM()->LoadNativeLibrary(
      path, const_cast<ClassLoader*>(class_loader_), reason))
      << path << ": " << reason;

  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 24);
  EXPECT_EQ(25, result);
}

int gJava_MyClass_fooI_calls = 0;
jint Java_MyClass_fooI(JNIEnv* env, jobject thisObj, jint x) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_fooI_calls++;
  return x;
}

TEST_F(JniCompilerTest, CompileAndRunIntMethod) {
  SetupForTest(false, "fooI", "(I)I",
               reinterpret_cast<void*>(&Java_MyClass_fooI));

  EXPECT_EQ(0, gJava_MyClass_fooI_calls);
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 42);
  EXPECT_EQ(42, result);
  EXPECT_EQ(1, gJava_MyClass_fooI_calls);
  result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 0xCAFED00D);
  EXPECT_EQ(static_cast<jint>(0xCAFED00D), result);
  EXPECT_EQ(2, gJava_MyClass_fooI_calls);
}

int gJava_MyClass_fooII_calls = 0;
jint Java_MyClass_fooII(JNIEnv* env, jobject thisObj, jint x, jint y) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_fooII_calls++;
  return x - y;  // non-commutative operator
}

TEST_F(JniCompilerTest, CompileAndRunIntIntMethod) {
  SetupForTest(false, "fooII", "(II)I",
               reinterpret_cast<void*>(&Java_MyClass_fooII));

  EXPECT_EQ(0, gJava_MyClass_fooII_calls);
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 99, 10);
  EXPECT_EQ(99 - 10, result);
  EXPECT_EQ(1, gJava_MyClass_fooII_calls);
  result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 0xCAFEBABE,
                                         0xCAFED00D);
  EXPECT_EQ(static_cast<jint>(0xCAFEBABE - 0xCAFED00D), result);
  EXPECT_EQ(2, gJava_MyClass_fooII_calls);
}

int gJava_MyClass_fooJJ_calls = 0;
jlong Java_MyClass_fooJJ(JNIEnv* env, jobject thisObj, jlong x, jlong y) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_fooJJ_calls++;
  return x - y;  // non-commutative operator
}

TEST_F(JniCompilerTest, CompileAndRunLongLongMethod) {
  SetupForTest(false, "fooJJ", "(JJ)J",
               reinterpret_cast<void*>(&Java_MyClass_fooJJ));

  EXPECT_EQ(0, gJava_MyClass_fooJJ_calls);
  jlong a = 0x1234567890ABCDEFll;
  jlong b = 0xFEDCBA0987654321ll;
  jlong result = env_->CallNonvirtualLongMethod(jobj_, jklass_, jmethod_, a, b);
  EXPECT_EQ(a - b, result);
  EXPECT_EQ(1, gJava_MyClass_fooJJ_calls);
  result = env_->CallNonvirtualLongMethod(jobj_, jklass_, jmethod_, b, a);
  EXPECT_EQ(b - a, result);
  EXPECT_EQ(2, gJava_MyClass_fooJJ_calls);
}

int gJava_MyClass_fooDD_calls = 0;
jdouble Java_MyClass_fooDD(JNIEnv* env, jobject thisObj, jdouble x, jdouble y) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_fooDD_calls++;
  return x - y;  // non-commutative operator
}

TEST_F(JniCompilerTest, CompileAndRunDoubleDoubleMethod) {
  SetupForTest(false, "fooDD", "(DD)D",
               reinterpret_cast<void*>(&Java_MyClass_fooDD));

  EXPECT_EQ(0, gJava_MyClass_fooDD_calls);
  jdouble result = env_->CallNonvirtualDoubleMethod(jobj_, jklass_, jmethod_,
                                                    99.0, 10.0);
  EXPECT_EQ(99.0 - 10.0, result);
  EXPECT_EQ(1, gJava_MyClass_fooDD_calls);
  jdouble a = 3.14159265358979323846;
  jdouble b = 0.69314718055994530942;
  result = env_->CallNonvirtualDoubleMethod(jobj_, jklass_, jmethod_, a, b);
  EXPECT_EQ(a - b, result);
  EXPECT_EQ(2, gJava_MyClass_fooDD_calls);
}

int gJava_MyClass_fooIOO_calls = 0;
jobject Java_MyClass_fooIOO(JNIEnv* env, jobject thisObj, jint x, jobject y,
                            jobject z) {
  EXPECT_EQ(3u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_fooIOO_calls++;
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return thisObj;
  }
}

TEST_F(JniCompilerTest, CompileAndRunIntObjectObjectMethod) {
  SetupForTest(false, "fooIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClass_fooIOO));

  EXPECT_EQ(0, gJava_MyClass_fooIOO_calls);
  jobject result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, NULL, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(1, gJava_MyClass_fooIOO_calls);

  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, NULL, jklass_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(2, gJava_MyClass_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 1, NULL, jklass_);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(3, gJava_MyClass_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 2, NULL, jklass_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(4, gJava_MyClass_fooIOO_calls);

  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, jklass_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(5, gJava_MyClass_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 1, jklass_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(6, gJava_MyClass_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 2, jklass_, NULL);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(7, gJava_MyClass_fooIOO_calls);
}

int gJava_MyClass_fooSII_calls = 0;
jint Java_MyClass_fooSII(JNIEnv* env, jclass klass, jint x, jint y) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != NULL);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClass_fooSII_calls++;
  return x + y;
}

TEST_F(JniCompilerTest, CompileAndRunStaticIntIntMethod) {
  SetupForTest(true, "fooSII",
               "(II)I",
               reinterpret_cast<void*>(&Java_MyClass_fooSII));

  EXPECT_EQ(0, gJava_MyClass_fooSII_calls);
  jint result = env_->CallStaticIntMethod(jklass_, jmethod_, 20, 30);
  EXPECT_EQ(50, result);
  EXPECT_EQ(1, gJava_MyClass_fooSII_calls);
}

int gJava_MyClass_fooSDD_calls = 0;
jdouble Java_MyClass_fooSDD(JNIEnv* env, jclass klass, jdouble x, jdouble y) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != NULL);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClass_fooSDD_calls++;
  return x - y;  // non-commutative operator
}

TEST_F(JniCompilerTest, CompileAndRunStaticDoubleDoubleMethod) {
  SetupForTest(true, "fooSDD", "(DD)D",
               reinterpret_cast<void*>(&Java_MyClass_fooSDD));

  EXPECT_EQ(0, gJava_MyClass_fooSDD_calls);
  jdouble result = env_->CallStaticDoubleMethod(jklass_, jmethod_, 99.0, 10.0);
  EXPECT_EQ(99.0 - 10.0, result);
  EXPECT_EQ(1, gJava_MyClass_fooSDD_calls);
  jdouble a = 3.14159265358979323846;
  jdouble b = 0.69314718055994530942;
  result = env_->CallStaticDoubleMethod(jklass_, jmethod_, a, b);
  EXPECT_EQ(a - b, result);
  EXPECT_EQ(2, gJava_MyClass_fooSDD_calls);
}

int gJava_MyClass_fooSIOO_calls = 0;
jobject Java_MyClass_fooSIOO(JNIEnv* env, jclass klass, jint x, jobject y,
                             jobject z) {
  EXPECT_EQ(3u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != NULL);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClass_fooSIOO_calls++;
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return klass;
  }
}


TEST_F(JniCompilerTest, CompileAndRunStaticIntObjectObjectMethod) {
  SetupForTest(true, "fooSIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClass_fooSIOO));

  EXPECT_EQ(0, gJava_MyClass_fooSIOO_calls);
  jobject result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(1, gJava_MyClass_fooSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(2, gJava_MyClass_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(3, gJava_MyClass_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(4, gJava_MyClass_fooSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(5, gJava_MyClass_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(6, gJava_MyClass_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(7, gJava_MyClass_fooSIOO_calls);
}

int gJava_MyClass_fooSSIOO_calls = 0;
jobject Java_MyClass_fooSSIOO(JNIEnv* env, jclass klass, jint x, jobject y,
                             jobject z) {
  EXPECT_EQ(3u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != NULL);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClass_fooSSIOO_calls++;
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return klass;
  }
}

TEST_F(JniCompilerTest, CompileAndRunStaticSynchronizedIntObjectObjectMethod) {
  SetupForTest(true, "fooSSIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClass_fooSSIOO));

  EXPECT_EQ(0, gJava_MyClass_fooSSIOO_calls);
  jobject result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(1, gJava_MyClass_fooSSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(2, gJava_MyClass_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(3, gJava_MyClass_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(4, gJava_MyClass_fooSSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(5, gJava_MyClass_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(6, gJava_MyClass_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(7, gJava_MyClass_fooSSIOO_calls);
}

void Java_MyClass_throwException(JNIEnv* env, jobject) {
  jclass c = env->FindClass("java/lang/RuntimeException");
  env->ThrowNew(c, "hello");
}

TEST_F(JniCompilerTest, ExceptionHandling) {
  // all compilation needs to happen before SetupForTest calls Runtime::Start
  CompileForTest(false, "foo", "()V");
  CompileForTest(false, "throwException", "()V");
  CompileForTest(false, "foo", "()V");

  gJava_MyClass_foo_calls = 0;

  // Check a single call of a JNI method is ok
  SetupForTest(false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClass_foo));
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClass_foo_calls);
  EXPECT_FALSE(Thread::Current()->IsExceptionPending());

  // Get class for exception we expect to be thrown
  Class* jlre = class_linker_->FindClass("Ljava/lang/RuntimeException;", class_loader_);
  SetupForTest(false, "throwException", "()V", reinterpret_cast<void*>(&Java_MyClass_throwException));
  // Call Java_MyClass_throwException (JNI method that throws exception)
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClass_foo_calls);
  EXPECT_TRUE(Thread::Current()->IsExceptionPending());
  EXPECT_TRUE(Thread::Current()->GetException()->InstanceOf(jlre));
  Thread::Current()->ClearException();

  // Check a single call of a JNI method is ok
  SetupForTest(false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClass_foo));
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(2, gJava_MyClass_foo_calls);
}

jint Java_MyClass_nativeUpCall(JNIEnv* env, jobject thisObj, jint i) {
  if (i <= 0) {
    // We want to check raw Object*/Array* below
    ScopedJniThreadState ts(env);

    // Build stack trace
    jobject internal = Thread::Current()->CreateInternalStackTrace(env);
    jobjectArray ste_array = Thread::InternalStackTraceToStackTraceElementArray(env, internal);
    ObjectArray<StackTraceElement>* trace_array =
        Decode<ObjectArray<StackTraceElement>*>(env, ste_array);
    EXPECT_TRUE(trace_array != NULL);
    EXPECT_EQ(11, trace_array->GetLength());

    // Check stack trace entries have expected values
    for (int32_t i = 0; i < trace_array->GetLength(); ++i) {
      EXPECT_EQ(-2, trace_array->Get(i)->GetLineNumber());
      StackTraceElement* ste = trace_array->Get(i);
      EXPECT_STREQ("MyClassNatives.java", ste->GetFileName()->ToModifiedUtf8().c_str());
      EXPECT_STREQ("MyClass", ste->GetDeclaringClass()->ToModifiedUtf8().c_str());
      EXPECT_STREQ("fooI", ste->GetMethodName()->ToModifiedUtf8().c_str());
    }

    // end recursion
    return 0;
  } else {
    jclass jklass = env->FindClass("MyClass");
    EXPECT_TRUE(jklass != NULL);
    jmethodID jmethod = env->GetMethodID(jklass, "fooI", "(I)I");
    EXPECT_TRUE(jmethod != NULL);

    // Recurse with i - 1
    jint result = env->CallNonvirtualIntMethod(thisObj, jklass, jmethod, i - 1);

    // Return sum of all depths
    return i + result;
  }
}

TEST_F(JniCompilerTest, NativeStackTraceElement) {
  SetupForTest(false, "fooI", "(I)I", reinterpret_cast<void*>(&Java_MyClass_nativeUpCall));
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 10);
  EXPECT_EQ(10+9+8+7+6+5+4+3+2+1, result);
}

jobject Java_MyClass_fooO(JNIEnv* env, jobject thisObj, jobject x) {
  return env->NewGlobalRef(x);
}

TEST_F(JniCompilerTest, ReturnGlobalRef) {
  SetupForTest(false, "fooO", "(Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClass_fooO));
  jobject result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, jobj_);
  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(result));
  EXPECT_TRUE(env_->IsSameObject(result, jobj_));
}

jint local_ref_test(JNIEnv* env, jobject thisObj, jint x) {
  // Add 10 local references
  for(int i = 0; i < 10; i++) {
    AddLocalReference<jobject>(env, Decode<Object*>(env, thisObj));
  }
  return x+1;
}

TEST_F(JniCompilerTest, LocalReferenceTableClearingTest) {
  SetupForTest(false, "fooI", "(I)I", reinterpret_cast<void*>(&local_ref_test));
  // 1000 invocations of a method that adds 10 local references
  for(int i=0; i < 1000; i++) {
    jint result = env_->CallIntMethod(jobj_, jmethod_, i);
    EXPECT_TRUE(result == i + 1);
  }
}

void my_arraycopy(JNIEnv* env, jclass klass, jobject src, jint src_pos, jobject dst, jint dst_pos, jint length) {
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jklass_, klass));
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jklass_, dst));
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jobj_, src));
  EXPECT_EQ(1234, src_pos);
  EXPECT_EQ(5678, dst_pos);
  EXPECT_EQ(9876, length);
}

TEST_F(JniCompilerTest, JavaLangSystemArrayCopy) {
  SetupForTest(true, "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V",
               reinterpret_cast<void*>(&my_arraycopy));
  env_->CallStaticVoidMethod(jklass_, jmethod_, jobj_, 1234, jklass_, 5678, 9876);
}

jboolean my_casi(JNIEnv* env, jobject unsafe, jobject obj, jlong offset, jint expected, jint newval) {
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jobj_, unsafe));
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jobj_, obj));
  EXPECT_EQ(0x12345678ABCDEF88ll, offset);
  EXPECT_EQ(static_cast<jint>(0xCAFEF00D), expected);
  EXPECT_EQ(static_cast<jint>(0xEBADF00D), newval);
  return JNI_TRUE;
}

TEST_F(JniCompilerTest, CompareAndSwapInt) {
  SetupForTest(false, "compareAndSwapInt", "(Ljava/lang/Object;JII)Z",
               reinterpret_cast<void*>(&my_casi));
  jboolean result = env_->CallBooleanMethod(jobj_, jmethod_, jobj_, 0x12345678ABCDEF88ll, 0xCAFEF00D, 0xEBADF00D);
  EXPECT_EQ(result, JNI_TRUE);
}

}  // namespace art
