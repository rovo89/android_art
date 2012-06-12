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
#include "UniquePtr.h"

extern "C" JNIEXPORT jint JNICALL Java_MyClassNatives_bar(JNIEnv*, jobject, jint count) {
  return count + 1;
}

extern "C" JNIEXPORT jint JNICALL Java_MyClassNatives_sbar(JNIEnv*, jclass, jint count) {
  return count + 1;
}

namespace art {

class JniCompilerTest : public CommonTest {
 protected:

  void CompileForTest(ClassLoader* class_loader, bool direct,
                      const char* method_name, const char* method_sig) {
    // Compile the native method before starting the runtime
    Class* c = class_linker_->FindClass("LMyClassNatives;", class_loader);
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

  void SetUpForTest(ClassLoader* class_loader, bool direct,
                    const char* method_name, const char* method_sig,
                    void* native_fnptr) {
    CompileForTest(class_loader, direct, method_name, method_sig);
    if (!runtime_->IsStarted()) {
      runtime_->Start();
    }

    // JNI operations after runtime start
    env_ = Thread::Current()->GetJniEnv();
    jklass_ = env_->FindClass("MyClassNatives");
    ASSERT_TRUE(jklass_ != NULL);

    if (direct) {
      jmethod_ = env_->GetStaticMethodID(jklass_, method_name, method_sig);
    } else {
      jmethod_ = env_->GetMethodID(jklass_, method_name, method_sig);
    }
    ASSERT_TRUE(jmethod_ != NULL);

    if (native_fnptr != NULL) {
      JNINativeMethod methods[] = { { method_name, method_sig, native_fnptr } };
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
  JNIEnv* env_;
  jmethodID jmethod_;
};

jclass JniCompilerTest::jklass_;
jobject JniCompilerTest::jobj_;

int gJava_MyClassNatives_foo_calls = 0;
void Java_MyClassNatives_foo(JNIEnv* env, jobject thisObj) {
  // 2 = SirtRef<ClassLoader> + thisObj
  EXPECT_EQ(2U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_foo_calls++;
}

TEST_F(JniCompilerTest, CompileAndRunNoArgMethod) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "foo", "()V",
               reinterpret_cast<void*>(&Java_MyClassNatives_foo));

  EXPECT_EQ(0, gJava_MyClassNatives_foo_calls);
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClassNatives_foo_calls);
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(2, gJava_MyClassNatives_foo_calls);
}

TEST_F(JniCompilerTest, CompileAndRunIntMethodThroughStub) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "bar", "(I)I",
               NULL /* calling through stub will link with &Java_MyClassNatives_bar */);

  std::string reason;
  ASSERT_TRUE(Runtime::Current()->GetJavaVM()->LoadNativeLibrary("", class_loader.get(), reason))
      << reason;

  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 24);
  EXPECT_EQ(25, result);
}

TEST_F(JniCompilerTest, CompileAndRunStaticIntMethodThroughStub) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), true, "sbar", "(I)I",
               NULL /* calling through stub will link with &Java_MyClassNatives_sbar */);

  std::string reason;
  ASSERT_TRUE(Runtime::Current()->GetJavaVM()->LoadNativeLibrary("", class_loader.get(), reason))
      << reason;

  jint result = env_->CallStaticIntMethod(jklass_, jmethod_, 42);
  EXPECT_EQ(43, result);
}

int gJava_MyClassNatives_fooI_calls = 0;
jint Java_MyClassNatives_fooI(JNIEnv* env, jobject thisObj, jint x) {
  // 2 = SirtRef<ClassLoader> + thisObj
  EXPECT_EQ(2U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooI_calls++;
  return x;
}

TEST_F(JniCompilerTest, CompileAndRunIntMethod) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "fooI", "(I)I",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooI));

  EXPECT_EQ(0, gJava_MyClassNatives_fooI_calls);
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 42);
  EXPECT_EQ(42, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooI_calls);
  result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 0xCAFED00D);
  EXPECT_EQ(static_cast<jint>(0xCAFED00D), result);
  EXPECT_EQ(2, gJava_MyClassNatives_fooI_calls);
}

int gJava_MyClassNatives_fooII_calls = 0;
jint Java_MyClassNatives_fooII(JNIEnv* env, jobject thisObj, jint x, jint y) {
  // 2 = SirtRef<ClassLoader> + thisObj
  EXPECT_EQ(2U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooII_calls++;
  return x - y;  // non-commutative operator
}

TEST_F(JniCompilerTest, CompileAndRunIntIntMethod) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "fooII", "(II)I",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooII));

  EXPECT_EQ(0, gJava_MyClassNatives_fooII_calls);
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 99, 10);
  EXPECT_EQ(99 - 10, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooII_calls);
  result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 0xCAFEBABE,
                                         0xCAFED00D);
  EXPECT_EQ(static_cast<jint>(0xCAFEBABE - 0xCAFED00D), result);
  EXPECT_EQ(2, gJava_MyClassNatives_fooII_calls);
}

int gJava_MyClassNatives_fooJJ_calls = 0;
jlong Java_MyClassNatives_fooJJ(JNIEnv* env, jobject thisObj, jlong x, jlong y) {
  // 2 = SirtRef<ClassLoader> + thisObj
  EXPECT_EQ(2U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooJJ_calls++;
  return x - y;  // non-commutative operator
}

TEST_F(JniCompilerTest, CompileAndRunLongLongMethod) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "fooJJ", "(JJ)J",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooJJ));

  EXPECT_EQ(0, gJava_MyClassNatives_fooJJ_calls);
  jlong a = 0x1234567890ABCDEFll;
  jlong b = 0xFEDCBA0987654321ll;
  jlong result = env_->CallNonvirtualLongMethod(jobj_, jklass_, jmethod_, a, b);
  EXPECT_EQ(a - b, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooJJ_calls);
  result = env_->CallNonvirtualLongMethod(jobj_, jklass_, jmethod_, b, a);
  EXPECT_EQ(b - a, result);
  EXPECT_EQ(2, gJava_MyClassNatives_fooJJ_calls);
}

int gJava_MyClassNatives_fooDD_calls = 0;
jdouble Java_MyClassNatives_fooDD(JNIEnv* env, jobject thisObj, jdouble x, jdouble y) {
  // 2 = SirtRef<ClassLoader> + thisObj
  EXPECT_EQ(2U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooDD_calls++;
  return x - y;  // non-commutative operator
}

TEST_F(JniCompilerTest, CompileAndRunDoubleDoubleMethod) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "fooDD", "(DD)D",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooDD));

  EXPECT_EQ(0, gJava_MyClassNatives_fooDD_calls);
  jdouble result = env_->CallNonvirtualDoubleMethod(jobj_, jklass_, jmethod_,
                                                    99.0, 10.0);
  EXPECT_EQ(99.0 - 10.0, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooDD_calls);
  jdouble a = 3.14159265358979323846;
  jdouble b = 0.69314718055994530942;
  result = env_->CallNonvirtualDoubleMethod(jobj_, jklass_, jmethod_, a, b);
  EXPECT_EQ(a - b, result);
  EXPECT_EQ(2, gJava_MyClassNatives_fooDD_calls);
}

int gJava_MyClassNatives_fooJJ_synchronized_calls = 0;
jlong Java_MyClassNatives_fooJJ_synchronized(JNIEnv* env, jobject thisObj, jlong x, jlong y) {
  // 2 = SirtRef<ClassLoader> + thisObj
  EXPECT_EQ(2U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooJJ_synchronized_calls++;
  return x | y;
}

TEST_F(JniCompilerTest, CompileAndRun_fooJJ_synchronized) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "fooJJ_synchronized", "(JJ)J",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooJJ_synchronized));

  EXPECT_EQ(0, gJava_MyClassNatives_fooJJ_synchronized_calls);
  jlong a = 0x1000000020000000ULL;
  jlong b = 0x00ff000000aa0000ULL;
  jlong result = env_->CallNonvirtualLongMethod(jobj_, jklass_, jmethod_, a, b);
  EXPECT_EQ(a | b, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooJJ_synchronized_calls);
}

int gJava_MyClassNatives_fooIOO_calls = 0;
jobject Java_MyClassNatives_fooIOO(JNIEnv* env, jobject thisObj, jint x, jobject y,
                            jobject z) {
  // 4 = SirtRef<ClassLoader> + this + y + z
  EXPECT_EQ(4U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooIOO_calls++;
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
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "fooIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooIOO));

  EXPECT_EQ(0, gJava_MyClassNatives_fooIOO_calls);
  jobject result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, NULL, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(1, gJava_MyClassNatives_fooIOO_calls);

  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, NULL, jklass_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(2, gJava_MyClassNatives_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 1, NULL, jklass_);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(3, gJava_MyClassNatives_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 2, NULL, jklass_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(4, gJava_MyClassNatives_fooIOO_calls);

  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, jklass_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(5, gJava_MyClassNatives_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 1, jklass_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(6, gJava_MyClassNatives_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 2, jklass_, NULL);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(7, gJava_MyClassNatives_fooIOO_calls);
}

int gJava_MyClassNatives_fooSII_calls = 0;
jint Java_MyClassNatives_fooSII(JNIEnv* env, jclass klass, jint x, jint y) {
  // 2 = SirtRef<ClassLoader> + klass
  EXPECT_EQ(2U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != NULL);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClassNatives_fooSII_calls++;
  return x + y;
}

TEST_F(JniCompilerTest, CompileAndRunStaticIntIntMethod) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), true, "fooSII", "(II)I",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooSII));

  EXPECT_EQ(0, gJava_MyClassNatives_fooSII_calls);
  jint result = env_->CallStaticIntMethod(jklass_, jmethod_, 20, 30);
  EXPECT_EQ(50, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooSII_calls);
}

int gJava_MyClassNatives_fooSDD_calls = 0;
jdouble Java_MyClassNatives_fooSDD(JNIEnv* env, jclass klass, jdouble x, jdouble y) {
  // 2 = SirtRef<ClassLoader> + klass
  EXPECT_EQ(2U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != NULL);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClassNatives_fooSDD_calls++;
  return x - y;  // non-commutative operator
}

TEST_F(JniCompilerTest, CompileAndRunStaticDoubleDoubleMethod) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), true, "fooSDD", "(DD)D",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooSDD));

  EXPECT_EQ(0, gJava_MyClassNatives_fooSDD_calls);
  jdouble result = env_->CallStaticDoubleMethod(jklass_, jmethod_, 99.0, 10.0);
  EXPECT_EQ(99.0 - 10.0, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooSDD_calls);
  jdouble a = 3.14159265358979323846;
  jdouble b = 0.69314718055994530942;
  result = env_->CallStaticDoubleMethod(jklass_, jmethod_, a, b);
  EXPECT_EQ(a - b, result);
  EXPECT_EQ(2, gJava_MyClassNatives_fooSDD_calls);
}

int gJava_MyClassNatives_fooSIOO_calls = 0;
jobject Java_MyClassNatives_fooSIOO(JNIEnv* env, jclass klass, jint x, jobject y,
                             jobject z) {
  // 4 = SirtRef<ClassLoader> + klass + y + z
  EXPECT_EQ(4U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != NULL);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClassNatives_fooSIOO_calls++;
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
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), true, "fooSIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooSIOO));

  EXPECT_EQ(0, gJava_MyClassNatives_fooSIOO_calls);
  jobject result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(1, gJava_MyClassNatives_fooSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(2, gJava_MyClassNatives_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(3, gJava_MyClassNatives_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(4, gJava_MyClassNatives_fooSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(5, gJava_MyClassNatives_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(6, gJava_MyClassNatives_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(7, gJava_MyClassNatives_fooSIOO_calls);
}

int gJava_MyClassNatives_fooSSIOO_calls = 0;
jobject Java_MyClassNatives_fooSSIOO(JNIEnv* env, jclass klass, jint x, jobject y,
                             jobject z) {
  // 4 = SirtRef<ClassLoader> + klass + y + z
  EXPECT_EQ(4U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != NULL);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClassNatives_fooSSIOO_calls++;
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
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), true, "fooSSIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooSSIOO));

  EXPECT_EQ(0, gJava_MyClassNatives_fooSSIOO_calls);
  jobject result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(1, gJava_MyClassNatives_fooSSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(2, gJava_MyClassNatives_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(3, gJava_MyClassNatives_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(4, gJava_MyClassNatives_fooSSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(5, gJava_MyClassNatives_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(6, gJava_MyClassNatives_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(7, gJava_MyClassNatives_fooSSIOO_calls);
}

void Java_MyClassNatives_throwException(JNIEnv* env, jobject) {
  jclass c = env->FindClass("java/lang/RuntimeException");
  env->ThrowNew(c, "hello");
}

TEST_F(JniCompilerTest, ExceptionHandling) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));

  // all compilation needs to happen before SetUpForTest calls Runtime::Start
  CompileForTest(class_loader.get(), false, "foo", "()V");
  CompileForTest(class_loader.get(), false, "throwException", "()V");
  CompileForTest(class_loader.get(), false, "foo", "()V");

  gJava_MyClassNatives_foo_calls = 0;

  // Check a single call of a JNI method is ok
  SetUpForTest(class_loader.get(), false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClassNatives_foo));
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClassNatives_foo_calls);
  EXPECT_FALSE(Thread::Current()->IsExceptionPending());

  // Get class for exception we expect to be thrown
  Class* jlre = class_linker_->FindClass("Ljava/lang/RuntimeException;", class_loader.get());
  SetUpForTest(class_loader.get(), false, "throwException", "()V",
               reinterpret_cast<void*>(&Java_MyClassNatives_throwException));
  // Call Java_MyClassNatives_throwException (JNI method that throws exception)
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClassNatives_foo_calls);
  EXPECT_TRUE(Thread::Current()->IsExceptionPending());
  EXPECT_TRUE(Thread::Current()->GetException()->InstanceOf(jlre));
  Thread::Current()->ClearException();

  // Check a single call of a JNI method is ok
  SetUpForTest(class_loader.get(), false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClassNatives_foo));
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(2, gJava_MyClassNatives_foo_calls);
}

jint Java_MyClassNatives_nativeUpCall(JNIEnv* env, jobject thisObj, jint i) {
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
      EXPECT_STREQ("MyClassNatives", ste->GetDeclaringClass()->ToModifiedUtf8().c_str());
      EXPECT_STREQ("fooI", ste->GetMethodName()->ToModifiedUtf8().c_str());
    }

    // end recursion
    return 0;
  } else {
    jclass jklass = env->FindClass("MyClassNatives");
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
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "fooI", "(I)I",
               reinterpret_cast<void*>(&Java_MyClassNatives_nativeUpCall));
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 10);
  EXPECT_EQ(10+9+8+7+6+5+4+3+2+1, result);
}

jobject Java_MyClassNatives_fooO(JNIEnv* env, jobject, jobject x) {
  return env->NewGlobalRef(x);
}

TEST_F(JniCompilerTest, ReturnGlobalRef) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "fooO", "(Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooO));
  jobject result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, jobj_);
  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(result));
  EXPECT_TRUE(env_->IsSameObject(result, jobj_));
}

jint local_ref_test(JNIEnv* env, jobject thisObj, jint x) {
  // Add 10 local references
  for (int i = 0; i < 10; i++) {
    AddLocalReference<jobject>(env, Decode<Object*>(env, thisObj));
  }
  return x+1;
}

TEST_F(JniCompilerTest, LocalReferenceTableClearingTest) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "fooI", "(I)I", reinterpret_cast<void*>(&local_ref_test));
  // 1000 invocations of a method that adds 10 local references
  for (int i = 0; i < 1000; i++) {
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
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), true, "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V",
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
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "compareAndSwapInt", "(Ljava/lang/Object;JII)Z",
               reinterpret_cast<void*>(&my_casi));
  jboolean result = env_->CallBooleanMethod(jobj_, jmethod_, jobj_, 0x12345678ABCDEF88ll, 0xCAFEF00D, 0xEBADF00D);
  EXPECT_EQ(result, JNI_TRUE);
}

jint my_gettext(JNIEnv* env, jclass klass, jlong val1, jobject obj1, jlong val2, jobject obj2) {
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jobj_, obj1));
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jobj_, obj2));
  EXPECT_EQ(0x12345678ABCDEF88ll, val1);
  EXPECT_EQ(0x7FEDCBA987654321ll, val2);
  return 42;
}

TEST_F(JniCompilerTest, GetText) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), true, "getText", "(JLjava/lang/Object;JLjava/lang/Object;)I",
               reinterpret_cast<void*>(&my_gettext));
  jint result = env_->CallStaticIntMethod(jklass_, jmethod_, 0x12345678ABCDEF88ll, jobj_,
                                          0x7FEDCBA987654321ll, jobj_);
  EXPECT_EQ(result, 42);
}

// This should return jclass, but we're imitating a bug pattern.
jobject Java_MyClassNatives_instanceMethodThatShouldReturnClass(JNIEnv* env, jobject) {
  return env->NewStringUTF("not a class!");
}

// This should return jclass, but we're imitating a bug pattern.
jobject Java_MyClassNatives_staticMethodThatShouldReturnClass(JNIEnv* env, jclass) {
  return env->NewStringUTF("not a class!");
}

TEST_F(JniCompilerTest, UpcallReturnTypeChecking_Instance) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "instanceMethodThatShouldReturnClass", "()Ljava/lang/Class;",
               reinterpret_cast<void*>(&Java_MyClassNatives_instanceMethodThatShouldReturnClass));

  CheckJniAbortCatcher check_jni_abort_catcher;

  // This native method is bad, and tries to return a jstring as a jclass.
  env_->CallObjectMethod(jobj_, jmethod_);
  check_jni_abort_catcher.Check("java.lang.Class MyClassNatives.instanceMethodThatShouldReturnClass");

  // Here, we just call the method wrong; we should catch that too.
  env_->CallVoidMethod(jobj_, jmethod_);
  check_jni_abort_catcher.Check("java.lang.Class MyClassNatives.instanceMethodThatShouldReturnClass");
}

TEST_F(JniCompilerTest, UpcallReturnTypeChecking_Static) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), true, "staticMethodThatShouldReturnClass", "()Ljava/lang/Class;",
               reinterpret_cast<void*>(&Java_MyClassNatives_staticMethodThatShouldReturnClass));

  CheckJniAbortCatcher check_jni_abort_catcher;

  // This native method is bad, and tries to return a jstring as a jclass.
  env_->CallStaticObjectMethod(jklass_, jmethod_);
  check_jni_abort_catcher.Check("java.lang.Class MyClassNatives.staticMethodThatShouldReturnClass");

  // Here, we just call the method wrong; we should catch that too.
  env_->CallVoidMethod(jobj_, jmethod_);
  check_jni_abort_catcher.Check("java.lang.Class MyClassNatives.staticMethodThatShouldReturnClass");
}

// This should take jclass, but we're imitating a bug pattern.
void Java_MyClassNatives_instanceMethodThatShouldTakeClass(JNIEnv*, jobject, jclass) {
}

// This should take jclass, but we're imitating a bug pattern.
void Java_MyClassNatives_staticMethodThatShouldTakeClass(JNIEnv*, jclass, jclass) {
}

TEST_F(JniCompilerTest, UpcallArgumentTypeChecking_Instance) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), false, "instanceMethodThatShouldTakeClass", "(ILjava/lang/Class;)V",
               reinterpret_cast<void*>(&Java_MyClassNatives_instanceMethodThatShouldTakeClass));

  CheckJniAbortCatcher check_jni_abort_catcher;
  // We deliberately pass a bad second argument here.
  env_->CallVoidMethod(jobj_, jmethod_, 123, env_->NewStringUTF("not a class!"));
  check_jni_abort_catcher.Check("Aborting because JNI app bug detected");
}

TEST_F(JniCompilerTest, UpcallArgumentTypeChecking_Static) {
  SirtRef<ClassLoader> class_loader(LoadDex("MyClassNatives"));
  SetUpForTest(class_loader.get(), true, "staticMethodThatShouldTakeClass", "(ILjava/lang/Class;)V",
               reinterpret_cast<void*>(&Java_MyClassNatives_staticMethodThatShouldTakeClass));

  CheckJniAbortCatcher check_jni_abort_catcher;
  // We deliberately pass a bad second argument here.
  env_->CallStaticVoidMethod(jklass_, jmethod_, 123, env_->NewStringUTF("not a class!"));
  check_jni_abort_catcher.Check("Aborting because JNI app bug detected");
}

}  // namespace art
