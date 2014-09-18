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

#include <memory>

#include <math.h>

#include "class_linker.h"
#include "common_compiler_test.h"
#include "dex_file.h"
#include "gtest/gtest.h"
#include "indirect_reference_table.h"
#include "jni_internal.h"
#include "mem_map.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "mirror/stack_trace_element.h"
#include "runtime.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

extern "C" JNIEXPORT jint JNICALL Java_MyClassNatives_bar(JNIEnv*, jobject, jint count) {
  return count + 1;
}

extern "C" JNIEXPORT jint JNICALL Java_MyClassNatives_sbar(JNIEnv*, jclass, jint count) {
  return count + 1;
}

namespace art {

class JniCompilerTest : public CommonCompilerTest {
 protected:
  void SetUp() OVERRIDE {
    CommonCompilerTest::SetUp();
    check_generic_jni_ = false;
  }

  void SetCheckGenericJni(bool generic) {
    check_generic_jni_ = generic;
  }

  void CompileForTest(jobject class_loader, bool direct,
                      const char* method_name, const char* method_sig) {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::ClassLoader> loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(class_loader)));
    // Compile the native method before starting the runtime
    mirror::Class* c = class_linker_->FindClass(soa.Self(), "LMyClassNatives;", loader);
    mirror::ArtMethod* method;
    if (direct) {
      method = c->FindDirectMethod(method_name, method_sig);
    } else {
      method = c->FindVirtualMethod(method_name, method_sig);
    }
    ASSERT_TRUE(method != nullptr) << method_name << " " << method_sig;
    if (check_generic_jni_) {
      method->SetEntryPointFromQuickCompiledCode(class_linker_->GetQuickGenericJniTrampoline());
    } else {
      if (method->GetEntryPointFromQuickCompiledCode() == nullptr ||
          method->GetEntryPointFromQuickCompiledCode() == class_linker_->GetQuickGenericJniTrampoline()) {
        CompileMethod(method);
        ASSERT_TRUE(method->GetEntryPointFromQuickCompiledCode() != nullptr)
            << method_name << " " << method_sig;
#if defined(ART_USE_PORTABLE_COMPILER)
        ASSERT_TRUE(method->GetEntryPointFromPortableCompiledCode() != nullptr)
            << method_name << " " << method_sig;
#endif
      }
    }
  }

  void SetUpForTest(bool direct, const char* method_name, const char* method_sig,
                    void* native_fnptr) {
    // Initialize class loader and compile method when runtime not started.
    if (!runtime_->IsStarted()) {
      {
        ScopedObjectAccess soa(Thread::Current());
        class_loader_ = LoadDex("MyClassNatives");
      }
      CompileForTest(class_loader_, direct, method_name, method_sig);
      // Start runtime.
      Thread::Current()->TransitionFromSuspendedToRunnable();
      bool started = runtime_->Start();
      CHECK(started);
    }
    // JNI operations after runtime start.
    env_ = Thread::Current()->GetJniEnv();
    jklass_ = env_->FindClass("MyClassNatives");
    ASSERT_TRUE(jklass_ != nullptr) << method_name << " " << method_sig;

    if (direct) {
      jmethod_ = env_->GetStaticMethodID(jklass_, method_name, method_sig);
    } else {
      jmethod_ = env_->GetMethodID(jklass_, method_name, method_sig);
    }
    ASSERT_TRUE(jmethod_ != nullptr) << method_name << " " << method_sig;

    if (native_fnptr != nullptr) {
      JNINativeMethod methods[] = { { method_name, method_sig, native_fnptr } };
      ASSERT_EQ(JNI_OK, env_->RegisterNatives(jklass_, methods, 1))
              << method_name << " " << method_sig;
    } else {
      env_->UnregisterNatives(jklass_);
    }

    jmethodID constructor = env_->GetMethodID(jklass_, "<init>", "()V");
    jobj_ = env_->NewObject(jklass_, constructor);
    ASSERT_TRUE(jobj_ != nullptr) << method_name << " " << method_sig;
  }

 public:
  static jclass jklass_;
  static jobject jobj_;
  static jobject class_loader_;

 protected:
  // We have to list the methods here so we can share them between default and generic JNI.
  void CompileAndRunNoArgMethodImpl();
  void CompileAndRunIntMethodThroughStubImpl();
  void CompileAndRunStaticIntMethodThroughStubImpl();
  void CompileAndRunIntMethodImpl();
  void CompileAndRunIntIntMethodImpl();
  void CompileAndRunLongLongMethodImpl();
  void CompileAndRunDoubleDoubleMethodImpl();
  void CompileAndRun_fooJJ_synchronizedImpl();
  void CompileAndRunIntObjectObjectMethodImpl();
  void CompileAndRunStaticIntIntMethodImpl();
  void CompileAndRunStaticDoubleDoubleMethodImpl();
  void RunStaticLogDoubleMethodImpl();
  void RunStaticLogFloatMethodImpl();
  void RunStaticReturnTrueImpl();
  void RunStaticReturnFalseImpl();
  void RunGenericStaticReturnIntImpl();
  void CompileAndRunStaticIntObjectObjectMethodImpl();
  void CompileAndRunStaticSynchronizedIntObjectObjectMethodImpl();
  void ExceptionHandlingImpl();
  void NativeStackTraceElementImpl();
  void ReturnGlobalRefImpl();
  void LocalReferenceTableClearingTestImpl();
  void JavaLangSystemArrayCopyImpl();
  void CompareAndSwapIntImpl();
  void GetTextImpl();
  void GetSinkPropertiesNativeImpl();
  void UpcallReturnTypeChecking_InstanceImpl();
  void UpcallReturnTypeChecking_StaticImpl();
  void UpcallArgumentTypeChecking_InstanceImpl();
  void UpcallArgumentTypeChecking_StaticImpl();
  void CompileAndRunFloatFloatMethodImpl();
  void CheckParameterAlignImpl();
  void MaxParamNumberImpl();
  void WithoutImplementationImpl();
  void StackArgsIntsFirstImpl();
  void StackArgsFloatsFirstImpl();
  void StackArgsMixedImpl();

  JNIEnv* env_;
  jmethodID jmethod_;
  bool check_generic_jni_;
};

jclass JniCompilerTest::jklass_;
jobject JniCompilerTest::jobj_;
jobject JniCompilerTest::class_loader_;

#define JNI_TEST(TestName) \
  TEST_F(JniCompilerTest, TestName ## Default) { \
    TestName ## Impl();                          \
  }                                              \
                                                 \
  TEST_F(JniCompilerTest, TestName ## Generic) { \
    TEST_DISABLED_FOR_MIPS();                    \
    SetCheckGenericJni(true);                    \
    TestName ## Impl();                          \
  }

int gJava_MyClassNatives_foo_calls = 0;
void Java_MyClassNatives_foo(JNIEnv* env, jobject thisObj) {
  // 1 = thisObj
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  Locks::mutator_lock_->AssertNotHeld(Thread::Current());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_foo_calls++;
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_EQ(1U, Thread::Current()->NumStackReferences());
}

void JniCompilerTest::CompileAndRunNoArgMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClassNatives_foo));

  EXPECT_EQ(0, gJava_MyClassNatives_foo_calls);
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClassNatives_foo_calls);
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(2, gJava_MyClassNatives_foo_calls);

  gJava_MyClassNatives_foo_calls = 0;
}

JNI_TEST(CompileAndRunNoArgMethod)

void JniCompilerTest::CompileAndRunIntMethodThroughStubImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "bar", "(I)I", nullptr);
  // calling through stub will link with &Java_MyClassNatives_bar

  ScopedObjectAccess soa(Thread::Current());
  std::string reason;
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(class_loader_)));
  ASSERT_TRUE(
      Runtime::Current()->GetJavaVM()->LoadNativeLibrary("", class_loader, &reason)) << reason;

  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 24);
  EXPECT_EQ(25, result);
}

JNI_TEST(CompileAndRunIntMethodThroughStub)

void JniCompilerTest::CompileAndRunStaticIntMethodThroughStubImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "sbar", "(I)I", nullptr);
  // calling through stub will link with &Java_MyClassNatives_sbar

  ScopedObjectAccess soa(Thread::Current());
  std::string reason;
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(class_loader_)));
  ASSERT_TRUE(
      Runtime::Current()->GetJavaVM()->LoadNativeLibrary("", class_loader, &reason)) << reason;

  jint result = env_->CallStaticIntMethod(jklass_, jmethod_, 42);
  EXPECT_EQ(43, result);
}

JNI_TEST(CompileAndRunStaticIntMethodThroughStub)

int gJava_MyClassNatives_fooI_calls = 0;
jint Java_MyClassNatives_fooI(JNIEnv* env, jobject thisObj, jint x) {
  // 1 = thisObj
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooI_calls++;
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_EQ(1U, Thread::Current()->NumStackReferences());
  return x;
}

void JniCompilerTest::CompileAndRunIntMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "fooI", "(I)I",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooI));

  EXPECT_EQ(0, gJava_MyClassNatives_fooI_calls);
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 42);
  EXPECT_EQ(42, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooI_calls);
  result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 0xCAFED00D);
  EXPECT_EQ(static_cast<jint>(0xCAFED00D), result);
  EXPECT_EQ(2, gJava_MyClassNatives_fooI_calls);

  gJava_MyClassNatives_fooI_calls = 0;
}

JNI_TEST(CompileAndRunIntMethod)

int gJava_MyClassNatives_fooII_calls = 0;
jint Java_MyClassNatives_fooII(JNIEnv* env, jobject thisObj, jint x, jint y) {
  // 1 = thisObj
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooII_calls++;
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_EQ(1U, Thread::Current()->NumStackReferences());
  return x - y;  // non-commutative operator
}

void JniCompilerTest::CompileAndRunIntIntMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "fooII", "(II)I",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooII));

  EXPECT_EQ(0, gJava_MyClassNatives_fooII_calls);
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 99, 10);
  EXPECT_EQ(99 - 10, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooII_calls);
  result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 0xCAFEBABE,
                                         0xCAFED00D);
  EXPECT_EQ(static_cast<jint>(0xCAFEBABE - 0xCAFED00D), result);
  EXPECT_EQ(2, gJava_MyClassNatives_fooII_calls);

  gJava_MyClassNatives_fooII_calls = 0;
}

JNI_TEST(CompileAndRunIntIntMethod)

int gJava_MyClassNatives_fooJJ_calls = 0;
jlong Java_MyClassNatives_fooJJ(JNIEnv* env, jobject thisObj, jlong x, jlong y) {
  // 1 = thisObj
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooJJ_calls++;
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_EQ(1U, Thread::Current()->NumStackReferences());
  return x - y;  // non-commutative operator
}

void JniCompilerTest::CompileAndRunLongLongMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "fooJJ", "(JJ)J",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooJJ));

  EXPECT_EQ(0, gJava_MyClassNatives_fooJJ_calls);
  jlong a = INT64_C(0x1234567890ABCDEF);
  jlong b = INT64_C(0xFEDCBA0987654321);
  jlong result = env_->CallNonvirtualLongMethod(jobj_, jklass_, jmethod_, a, b);
  EXPECT_EQ(a - b, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooJJ_calls);
  result = env_->CallNonvirtualLongMethod(jobj_, jklass_, jmethod_, b, a);
  EXPECT_EQ(b - a, result);
  EXPECT_EQ(2, gJava_MyClassNatives_fooJJ_calls);

  gJava_MyClassNatives_fooJJ_calls = 0;
}

JNI_TEST(CompileAndRunLongLongMethod)

int gJava_MyClassNatives_fooDD_calls = 0;
jdouble Java_MyClassNatives_fooDD(JNIEnv* env, jobject thisObj, jdouble x, jdouble y) {
  // 1 = thisObj
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooDD_calls++;
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_EQ(1U, Thread::Current()->NumStackReferences());
  return x - y;  // non-commutative operator
}

void JniCompilerTest::CompileAndRunDoubleDoubleMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "fooDD", "(DD)D",
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

  gJava_MyClassNatives_fooDD_calls = 0;
}

int gJava_MyClassNatives_fooJJ_synchronized_calls = 0;
jlong Java_MyClassNatives_fooJJ_synchronized(JNIEnv* env, jobject thisObj, jlong x, jlong y) {
  // 1 = thisObj
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooJJ_synchronized_calls++;
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_EQ(1U, Thread::Current()->NumStackReferences());
  return x | y;
}

void JniCompilerTest::CompileAndRun_fooJJ_synchronizedImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "fooJJ_synchronized", "(JJ)J",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooJJ_synchronized));

  EXPECT_EQ(0, gJava_MyClassNatives_fooJJ_synchronized_calls);
  jlong a = 0x1000000020000000ULL;
  jlong b = 0x00ff000000aa0000ULL;
  jlong result = env_->CallNonvirtualLongMethod(jobj_, jklass_, jmethod_, a, b);
  EXPECT_EQ(a | b, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooJJ_synchronized_calls);

  gJava_MyClassNatives_fooJJ_synchronized_calls = 0;
}

JNI_TEST(CompileAndRun_fooJJ_synchronized)

int gJava_MyClassNatives_fooIOO_calls = 0;
jobject Java_MyClassNatives_fooIOO(JNIEnv* env, jobject thisObj, jint x, jobject y,
                            jobject z) {
  // 3 = this + y + z
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClassNatives_fooIOO_calls++;
  ScopedObjectAccess soa(Thread::Current());
  size_t null_args = (y == nullptr ? 1 : 0) + (z == nullptr ? 1 : 0);
  EXPECT_TRUE(3U == Thread::Current()->NumStackReferences() ||
              (3U - null_args) == Thread::Current()->NumStackReferences());
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return thisObj;
  }
}

void JniCompilerTest::CompileAndRunIntObjectObjectMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "fooIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooIOO));

  EXPECT_EQ(0, gJava_MyClassNatives_fooIOO_calls);
  jobject result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, nullptr, nullptr);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(1, gJava_MyClassNatives_fooIOO_calls);

  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, nullptr, jklass_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(2, gJava_MyClassNatives_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 1, nullptr, jklass_);
  EXPECT_TRUE(env_->IsSameObject(nullptr, result));
  EXPECT_EQ(3, gJava_MyClassNatives_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 2, nullptr, jklass_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(4, gJava_MyClassNatives_fooIOO_calls);

  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, jklass_, nullptr);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(5, gJava_MyClassNatives_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 1, jklass_, nullptr);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(6, gJava_MyClassNatives_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 2, jklass_, nullptr);
  EXPECT_TRUE(env_->IsSameObject(nullptr, result));
  EXPECT_EQ(7, gJava_MyClassNatives_fooIOO_calls);

  gJava_MyClassNatives_fooIOO_calls = 0;
}

JNI_TEST(CompileAndRunIntObjectObjectMethod)

int gJava_MyClassNatives_fooSII_calls = 0;
jint Java_MyClassNatives_fooSII(JNIEnv* env, jclass klass, jint x, jint y) {
  // 1 = klass
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClassNatives_fooSII_calls++;
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_EQ(1U, Thread::Current()->NumStackReferences());
  return x + y;
}

void JniCompilerTest::CompileAndRunStaticIntIntMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "fooSII", "(II)I",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooSII));

  EXPECT_EQ(0, gJava_MyClassNatives_fooSII_calls);
  jint result = env_->CallStaticIntMethod(jklass_, jmethod_, 20, 30);
  EXPECT_EQ(50, result);
  EXPECT_EQ(1, gJava_MyClassNatives_fooSII_calls);

  gJava_MyClassNatives_fooSII_calls = 0;
}

JNI_TEST(CompileAndRunStaticIntIntMethod)

int gJava_MyClassNatives_fooSDD_calls = 0;
jdouble Java_MyClassNatives_fooSDD(JNIEnv* env, jclass klass, jdouble x, jdouble y) {
  // 1 = klass
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClassNatives_fooSDD_calls++;
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_EQ(1U, Thread::Current()->NumStackReferences());
  return x - y;  // non-commutative operator
}

void JniCompilerTest::CompileAndRunStaticDoubleDoubleMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "fooSDD", "(DD)D",
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

  gJava_MyClassNatives_fooSDD_calls = 0;
}

JNI_TEST(CompileAndRunStaticDoubleDoubleMethod)

// The x86 generic JNI code had a bug where it assumed a floating
// point return value would be in xmm0. We use log, to somehow ensure
// the compiler will use the floating point stack.

jdouble Java_MyClassNatives_logD(JNIEnv* env, jclass klass, jdouble x) {
  return log(x);
}

void JniCompilerTest::RunStaticLogDoubleMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "logD", "(D)D", reinterpret_cast<void*>(&Java_MyClassNatives_logD));

  jdouble result = env_->CallStaticDoubleMethod(jklass_, jmethod_, 2.0);
  EXPECT_EQ(log(2.0), result);
}

JNI_TEST(RunStaticLogDoubleMethod)

jfloat Java_MyClassNatives_logF(JNIEnv* env, jclass klass, jfloat x) {
  return logf(x);
}

void JniCompilerTest::RunStaticLogFloatMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "logF", "(F)F", reinterpret_cast<void*>(&Java_MyClassNatives_logF));

  jfloat result = env_->CallStaticFloatMethod(jklass_, jmethod_, 2.0);
  EXPECT_EQ(logf(2.0), result);
}

JNI_TEST(RunStaticLogFloatMethod)

jboolean Java_MyClassNatives_returnTrue(JNIEnv* env, jclass klass) {
  return JNI_TRUE;
}

jboolean Java_MyClassNatives_returnFalse(JNIEnv* env, jclass klass) {
  return JNI_FALSE;
}

jint Java_MyClassNatives_returnInt(JNIEnv* env, jclass klass) {
  return 42;
}

void JniCompilerTest::RunStaticReturnTrueImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "returnTrue", "()Z", reinterpret_cast<void*>(&Java_MyClassNatives_returnTrue));

  jboolean result = env_->CallStaticBooleanMethod(jklass_, jmethod_);
  EXPECT_TRUE(result);
}

JNI_TEST(RunStaticReturnTrue)

void JniCompilerTest::RunStaticReturnFalseImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "returnFalse", "()Z",
               reinterpret_cast<void*>(&Java_MyClassNatives_returnFalse));

  jboolean result = env_->CallStaticBooleanMethod(jklass_, jmethod_);
  EXPECT_FALSE(result);
}

JNI_TEST(RunStaticReturnFalse)

void JniCompilerTest::RunGenericStaticReturnIntImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "returnInt", "()I", reinterpret_cast<void*>(&Java_MyClassNatives_returnInt));

  jint result = env_->CallStaticIntMethod(jklass_, jmethod_);
  EXPECT_EQ(42, result);
}

JNI_TEST(RunGenericStaticReturnInt)

int gJava_MyClassNatives_fooSIOO_calls = 0;
jobject Java_MyClassNatives_fooSIOO(JNIEnv* env, jclass klass, jint x, jobject y,
                             jobject z) {
  // 3 = klass + y + z
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClassNatives_fooSIOO_calls++;
  ScopedObjectAccess soa(Thread::Current());
  size_t null_args = (y == nullptr ? 1 : 0) + (z == nullptr ? 1 : 0);
  EXPECT_TRUE(3U == Thread::Current()->NumStackReferences() ||
              (3U - null_args) == Thread::Current()->NumStackReferences());
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return klass;
  }
}


void JniCompilerTest::CompileAndRunStaticIntObjectObjectMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "fooSIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooSIOO));

  EXPECT_EQ(0, gJava_MyClassNatives_fooSIOO_calls);
  jobject result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, nullptr, nullptr);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(1, gJava_MyClassNatives_fooSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, nullptr, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(2, gJava_MyClassNatives_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, nullptr, jobj_);
  EXPECT_TRUE(env_->IsSameObject(nullptr, result));
  EXPECT_EQ(3, gJava_MyClassNatives_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, nullptr, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(4, gJava_MyClassNatives_fooSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, jobj_, nullptr);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(5, gJava_MyClassNatives_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, jobj_, nullptr);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(6, gJava_MyClassNatives_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, jobj_, nullptr);
  EXPECT_TRUE(env_->IsSameObject(nullptr, result));
  EXPECT_EQ(7, gJava_MyClassNatives_fooSIOO_calls);

  gJava_MyClassNatives_fooSIOO_calls = 0;
}

JNI_TEST(CompileAndRunStaticIntObjectObjectMethod)

int gJava_MyClassNatives_fooSSIOO_calls = 0;
jobject Java_MyClassNatives_fooSSIOO(JNIEnv* env, jclass klass, jint x, jobject y, jobject z) {
  // 3 = klass + y + z
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClassNatives_fooSSIOO_calls++;
  ScopedObjectAccess soa(Thread::Current());
  size_t null_args = (y == nullptr ? 1 : 0) + (z == nullptr ? 1 : 0);
  EXPECT_TRUE(3U == Thread::Current()->NumStackReferences() ||
              (3U - null_args) == Thread::Current()->NumStackReferences());
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return klass;
  }
}

void JniCompilerTest::CompileAndRunStaticSynchronizedIntObjectObjectMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "fooSSIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooSSIOO));

  EXPECT_EQ(0, gJava_MyClassNatives_fooSSIOO_calls);
  jobject result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, nullptr, nullptr);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(1, gJava_MyClassNatives_fooSSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, nullptr, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(2, gJava_MyClassNatives_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, nullptr, jobj_);
  EXPECT_TRUE(env_->IsSameObject(nullptr, result));
  EXPECT_EQ(3, gJava_MyClassNatives_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, nullptr, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(4, gJava_MyClassNatives_fooSSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, jobj_, nullptr);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(5, gJava_MyClassNatives_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, jobj_, nullptr);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(6, gJava_MyClassNatives_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, jobj_, nullptr);
  EXPECT_TRUE(env_->IsSameObject(nullptr, result));
  EXPECT_EQ(7, gJava_MyClassNatives_fooSSIOO_calls);

  gJava_MyClassNatives_fooSSIOO_calls = 0;
}

JNI_TEST(CompileAndRunStaticSynchronizedIntObjectObjectMethod)

void Java_MyClassNatives_throwException(JNIEnv* env, jobject) {
  jclass c = env->FindClass("java/lang/RuntimeException");
  env->ThrowNew(c, "hello");
}

void JniCompilerTest::ExceptionHandlingImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  {
    ASSERT_FALSE(runtime_->IsStarted());
    ScopedObjectAccess soa(Thread::Current());
    class_loader_ = LoadDex("MyClassNatives");

    // all compilation needs to happen before Runtime::Start
    CompileForTest(class_loader_, false, "foo", "()V");
    CompileForTest(class_loader_, false, "throwException", "()V");
    CompileForTest(class_loader_, false, "foo", "()V");
  }
  // Start runtime to avoid re-initialization in SetupForTest.
  Thread::Current()->TransitionFromSuspendedToRunnable();
  bool started = runtime_->Start();
  CHECK(started);

  gJava_MyClassNatives_foo_calls = 0;

  // Check a single call of a JNI method is ok
  SetUpForTest(false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClassNatives_foo));
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClassNatives_foo_calls);
  EXPECT_FALSE(Thread::Current()->IsExceptionPending());

  // Get class for exception we expect to be thrown
  ScopedLocalRef<jclass> jlre(env_, env_->FindClass("java/lang/RuntimeException"));
  SetUpForTest(false, "throwException", "()V",
               reinterpret_cast<void*>(&Java_MyClassNatives_throwException));
  // Call Java_MyClassNatives_throwException (JNI method that throws exception)
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClassNatives_foo_calls);
  EXPECT_TRUE(env_->ExceptionCheck() == JNI_TRUE);
  ScopedLocalRef<jthrowable> exception(env_, env_->ExceptionOccurred());
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(exception.get(), jlre.get()));

  // Check a single call of a JNI method is ok
  SetUpForTest(false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClassNatives_foo));
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(2, gJava_MyClassNatives_foo_calls);

  gJava_MyClassNatives_foo_calls = 0;
}

JNI_TEST(ExceptionHandling)

jint Java_MyClassNatives_nativeUpCall(JNIEnv* env, jobject thisObj, jint i) {
  if (i <= 0) {
    // We want to check raw Object* / Array* below
    ScopedObjectAccess soa(env);

    // Build stack trace
    jobject internal = Thread::Current()->CreateInternalStackTrace<false>(soa);
    jobjectArray ste_array = Thread::InternalStackTraceToStackTraceElementArray(soa, internal);
    mirror::ObjectArray<mirror::StackTraceElement>* trace_array =
        soa.Decode<mirror::ObjectArray<mirror::StackTraceElement>*>(ste_array);
    EXPECT_TRUE(trace_array != nullptr);
    EXPECT_EQ(11, trace_array->GetLength());

    // Check stack trace entries have expected values
    for (int32_t i = 0; i < trace_array->GetLength(); ++i) {
      EXPECT_EQ(-2, trace_array->Get(i)->GetLineNumber());
      mirror::StackTraceElement* ste = trace_array->Get(i);
      EXPECT_STREQ("MyClassNatives.java", ste->GetFileName()->ToModifiedUtf8().c_str());
      EXPECT_STREQ("MyClassNatives", ste->GetDeclaringClass()->ToModifiedUtf8().c_str());
      EXPECT_STREQ("fooI", ste->GetMethodName()->ToModifiedUtf8().c_str());
    }

    // end recursion
    return 0;
  } else {
    jclass jklass = env->FindClass("MyClassNatives");
    EXPECT_TRUE(jklass != nullptr);
    jmethodID jmethod = env->GetMethodID(jklass, "fooI", "(I)I");
    EXPECT_TRUE(jmethod != nullptr);

    // Recurse with i - 1
    jint result = env->CallNonvirtualIntMethod(thisObj, jklass, jmethod, i - 1);

    // Return sum of all depths
    return i + result;
  }
}

void JniCompilerTest::NativeStackTraceElementImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "fooI", "(I)I",
               reinterpret_cast<void*>(&Java_MyClassNatives_nativeUpCall));
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 10);
  EXPECT_EQ(10+9+8+7+6+5+4+3+2+1, result);
}

JNI_TEST(NativeStackTraceElement)

jobject Java_MyClassNatives_fooO(JNIEnv* env, jobject, jobject x) {
  return env->NewGlobalRef(x);
}

void JniCompilerTest::ReturnGlobalRefImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "fooO", "(Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClassNatives_fooO));
  jobject result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, jobj_);
  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(result));
  EXPECT_TRUE(env_->IsSameObject(result, jobj_));
}

JNI_TEST(ReturnGlobalRef)

jint local_ref_test(JNIEnv* env, jobject thisObj, jint x) {
  // Add 10 local references
  ScopedObjectAccess soa(env);
  for (int i = 0; i < 10; i++) {
    soa.AddLocalReference<jobject>(soa.Decode<mirror::Object*>(thisObj));
  }
  return x+1;
}

void JniCompilerTest::LocalReferenceTableClearingTestImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "fooI", "(I)I", reinterpret_cast<void*>(&local_ref_test));
  // 1000 invocations of a method that adds 10 local references
  for (int i = 0; i < 1000; i++) {
    jint result = env_->CallIntMethod(jobj_, jmethod_, i);
    EXPECT_TRUE(result == i + 1);
  }
}

JNI_TEST(LocalReferenceTableClearingTest)

void my_arraycopy(JNIEnv* env, jclass klass, jobject src, jint src_pos, jobject dst, jint dst_pos, jint length) {
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jklass_, klass));
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jklass_, dst));
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jobj_, src));
  EXPECT_EQ(1234, src_pos);
  EXPECT_EQ(5678, dst_pos);
  EXPECT_EQ(9876, length);
}

void JniCompilerTest::JavaLangSystemArrayCopyImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V",
               reinterpret_cast<void*>(&my_arraycopy));
  env_->CallStaticVoidMethod(jklass_, jmethod_, jobj_, 1234, jklass_, 5678, 9876);
}

JNI_TEST(JavaLangSystemArrayCopy)

jboolean my_casi(JNIEnv* env, jobject unsafe, jobject obj, jlong offset, jint expected, jint newval) {
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jobj_, unsafe));
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jobj_, obj));
  EXPECT_EQ(INT64_C(0x12345678ABCDEF88), offset);
  EXPECT_EQ(static_cast<jint>(0xCAFEF00D), expected);
  EXPECT_EQ(static_cast<jint>(0xEBADF00D), newval);
  return JNI_TRUE;
}

void JniCompilerTest::CompareAndSwapIntImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "compareAndSwapInt", "(Ljava/lang/Object;JII)Z",
               reinterpret_cast<void*>(&my_casi));
  jboolean result = env_->CallBooleanMethod(jobj_, jmethod_, jobj_, INT64_C(0x12345678ABCDEF88),
                                            0xCAFEF00D, 0xEBADF00D);
  EXPECT_EQ(result, JNI_TRUE);
}

JNI_TEST(CompareAndSwapInt)

jint my_gettext(JNIEnv* env, jclass klass, jlong val1, jobject obj1, jlong val2, jobject obj2) {
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jobj_, obj1));
  EXPECT_TRUE(env->IsSameObject(JniCompilerTest::jobj_, obj2));
  EXPECT_EQ(0x12345678ABCDEF88ll, val1);
  EXPECT_EQ(0x7FEDCBA987654321ll, val2);
  return 42;
}

void JniCompilerTest::GetTextImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "getText", "(JLjava/lang/Object;JLjava/lang/Object;)I",
               reinterpret_cast<void*>(&my_gettext));
  jint result = env_->CallStaticIntMethod(jklass_, jmethod_, 0x12345678ABCDEF88ll, jobj_,
                                          INT64_C(0x7FEDCBA987654321), jobj_);
  EXPECT_EQ(result, 42);
}

JNI_TEST(GetText)

int gJava_MyClassNatives_GetSinkProperties_calls = 0;
jarray Java_MyClassNatives_GetSinkProperties(JNIEnv* env, jobject thisObj, jstring s) {
  // 1 = thisObj
  Thread* self = Thread::Current();
  EXPECT_EQ(kNative, self->GetState());
  Locks::mutator_lock_->AssertNotHeld(self);
  EXPECT_EQ(self->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  EXPECT_EQ(s, nullptr);
  gJava_MyClassNatives_GetSinkProperties_calls++;
  ScopedObjectAccess soa(self);
  EXPECT_EQ(2U, self->NumStackReferences());
  EXPECT_TRUE(self->HoldsLock(soa.Decode<mirror::Object*>(thisObj)));
  return nullptr;
}

void JniCompilerTest::GetSinkPropertiesNativeImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "getSinkPropertiesNative", "(Ljava/lang/String;)[Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClassNatives_GetSinkProperties));

  EXPECT_EQ(0, gJava_MyClassNatives_GetSinkProperties_calls);
  jarray result = down_cast<jarray>(
      env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, nullptr));
  EXPECT_EQ(nullptr, result);
  EXPECT_EQ(1, gJava_MyClassNatives_GetSinkProperties_calls);

  gJava_MyClassNatives_GetSinkProperties_calls = 0;
}

JNI_TEST(GetSinkPropertiesNative)

// This should return jclass, but we're imitating a bug pattern.
jobject Java_MyClassNatives_instanceMethodThatShouldReturnClass(JNIEnv* env, jobject) {
  return env->NewStringUTF("not a class!");
}

// This should return jclass, but we're imitating a bug pattern.
jobject Java_MyClassNatives_staticMethodThatShouldReturnClass(JNIEnv* env, jclass) {
  return env->NewStringUTF("not a class!");
}

void JniCompilerTest::UpcallReturnTypeChecking_InstanceImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "instanceMethodThatShouldReturnClass", "()Ljava/lang/Class;",
               reinterpret_cast<void*>(&Java_MyClassNatives_instanceMethodThatShouldReturnClass));

  CheckJniAbortCatcher check_jni_abort_catcher;
  // TODO: check type of returns with portable JNI compiler.
  // This native method is bad, and tries to return a jstring as a jclass.
  env_->CallObjectMethod(jobj_, jmethod_);
  check_jni_abort_catcher.Check("attempt to return an instance of java.lang.String from java.lang.Class MyClassNatives.instanceMethodThatShouldReturnClass()");

  // Here, we just call the method incorrectly; we should catch that too.
  env_->CallVoidMethod(jobj_, jmethod_);
  check_jni_abort_catcher.Check("attempt to return an instance of java.lang.String from java.lang.Class MyClassNatives.instanceMethodThatShouldReturnClass()");
  env_->CallStaticVoidMethod(jklass_, jmethod_);
  check_jni_abort_catcher.Check("calling non-static method java.lang.Class MyClassNatives.instanceMethodThatShouldReturnClass() with CallStaticVoidMethodV");
}

JNI_TEST(UpcallReturnTypeChecking_Instance)

void JniCompilerTest::UpcallReturnTypeChecking_StaticImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "staticMethodThatShouldReturnClass", "()Ljava/lang/Class;",
               reinterpret_cast<void*>(&Java_MyClassNatives_staticMethodThatShouldReturnClass));

  CheckJniAbortCatcher check_jni_abort_catcher;
  // TODO: check type of returns with portable JNI compiler.
  // This native method is bad, and tries to return a jstring as a jclass.
  env_->CallStaticObjectMethod(jklass_, jmethod_);
  check_jni_abort_catcher.Check("attempt to return an instance of java.lang.String from java.lang.Class MyClassNatives.staticMethodThatShouldReturnClass()");

  // Here, we just call the method incorrectly; we should catch that too.
  env_->CallStaticVoidMethod(jklass_, jmethod_);
  check_jni_abort_catcher.Check("attempt to return an instance of java.lang.String from java.lang.Class MyClassNatives.staticMethodThatShouldReturnClass()");
  env_->CallVoidMethod(jobj_, jmethod_);
  check_jni_abort_catcher.Check("calling static method java.lang.Class MyClassNatives.staticMethodThatShouldReturnClass() with CallVoidMethodV");
}

JNI_TEST(UpcallReturnTypeChecking_Static)

// This should take jclass, but we're imitating a bug pattern.
void Java_MyClassNatives_instanceMethodThatShouldTakeClass(JNIEnv*, jobject, jclass) {
}

// This should take jclass, but we're imitating a bug pattern.
void Java_MyClassNatives_staticMethodThatShouldTakeClass(JNIEnv*, jclass, jclass) {
}

void JniCompilerTest::UpcallArgumentTypeChecking_InstanceImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "instanceMethodThatShouldTakeClass", "(ILjava/lang/Class;)V",
               reinterpret_cast<void*>(&Java_MyClassNatives_instanceMethodThatShouldTakeClass));

  CheckJniAbortCatcher check_jni_abort_catcher;
  // We deliberately pass a bad second argument here.
  env_->CallVoidMethod(jobj_, jmethod_, 123, env_->NewStringUTF("not a class!"));
  check_jni_abort_catcher.Check("bad arguments passed to void MyClassNatives.instanceMethodThatShouldTakeClass(int, java.lang.Class)");
}

JNI_TEST(UpcallArgumentTypeChecking_Instance)

void JniCompilerTest::UpcallArgumentTypeChecking_StaticImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "staticMethodThatShouldTakeClass", "(ILjava/lang/Class;)V",
               reinterpret_cast<void*>(&Java_MyClassNatives_staticMethodThatShouldTakeClass));

  CheckJniAbortCatcher check_jni_abort_catcher;
  // We deliberately pass a bad second argument here.
  env_->CallStaticVoidMethod(jklass_, jmethod_, 123, env_->NewStringUTF("not a class!"));
  check_jni_abort_catcher.Check("bad arguments passed to void MyClassNatives.staticMethodThatShouldTakeClass(int, java.lang.Class)");
}

JNI_TEST(UpcallArgumentTypeChecking_Static)

jfloat Java_MyClassNatives_checkFloats(JNIEnv* env, jobject thisObj, jfloat f1, jfloat f2) {
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_EQ(1U, Thread::Current()->NumStackReferences());
  return f1 - f2;  // non-commutative operator
}

void JniCompilerTest::CompileAndRunFloatFloatMethodImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "checkFloats", "(FF)F",
               reinterpret_cast<void*>(&Java_MyClassNatives_checkFloats));

  jfloat result = env_->CallNonvirtualFloatMethod(jobj_, jklass_, jmethod_,
                                                    99.0F, 10.0F);
  EXPECT_EQ(99.0F - 10.0F, result);
  jfloat a = 3.14159F;
  jfloat b = 0.69314F;
  result = env_->CallNonvirtualFloatMethod(jobj_, jklass_, jmethod_, a, b);
  EXPECT_EQ(a - b, result);
}

JNI_TEST(CompileAndRunFloatFloatMethod)

void Java_MyClassNatives_checkParameterAlign(JNIEnv* env, jobject thisObj, jint i1, jlong l1) {
//  EXPECT_EQ(kNative, Thread::Current()->GetState());
//  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
//  EXPECT_TRUE(thisObj != nullptr);
//  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
//  ScopedObjectAccess soa(Thread::Current());
//  EXPECT_EQ(1U, Thread::Current()->NumStackReferences());
  EXPECT_EQ(i1, 1234);
  EXPECT_EQ(l1, INT64_C(0x12345678ABCDEF0));
}

void JniCompilerTest::CheckParameterAlignImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "checkParameterAlign", "(IJ)V",
               reinterpret_cast<void*>(&Java_MyClassNatives_checkParameterAlign));

  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_, 1234, INT64_C(0x12345678ABCDEF0));
}

JNI_TEST(CheckParameterAlign)

void Java_MyClassNatives_maxParamNumber(JNIEnv* env, jobject thisObj,
    jobject o0, jobject o1, jobject o2, jobject o3, jobject o4, jobject o5, jobject o6, jobject o7,
    jobject o8, jobject o9, jobject o10, jobject o11, jobject o12, jobject o13, jobject o14, jobject o15,
    jobject o16, jobject o17, jobject o18, jobject o19, jobject o20, jobject o21, jobject o22, jobject o23,
    jobject o24, jobject o25, jobject o26, jobject o27, jobject o28, jobject o29, jobject o30, jobject o31,
    jobject o32, jobject o33, jobject o34, jobject o35, jobject o36, jobject o37, jobject o38, jobject o39,
    jobject o40, jobject o41, jobject o42, jobject o43, jobject o44, jobject o45, jobject o46, jobject o47,
    jobject o48, jobject o49, jobject o50, jobject o51, jobject o52, jobject o53, jobject o54, jobject o55,
    jobject o56, jobject o57, jobject o58, jobject o59, jobject o60, jobject o61, jobject o62, jobject o63,
    jobject o64, jobject o65, jobject o66, jobject o67, jobject o68, jobject o69, jobject o70, jobject o71,
    jobject o72, jobject o73, jobject o74, jobject o75, jobject o76, jobject o77, jobject o78, jobject o79,
    jobject o80, jobject o81, jobject o82, jobject o83, jobject o84, jobject o85, jobject o86, jobject o87,
    jobject o88, jobject o89, jobject o90, jobject o91, jobject o92, jobject o93, jobject o94, jobject o95,
    jobject o96, jobject o97, jobject o98, jobject o99, jobject o100, jobject o101, jobject o102, jobject o103,
    jobject o104, jobject o105, jobject o106, jobject o107, jobject o108, jobject o109, jobject o110, jobject o111,
    jobject o112, jobject o113, jobject o114, jobject o115, jobject o116, jobject o117, jobject o118, jobject o119,
    jobject o120, jobject o121, jobject o122, jobject o123, jobject o124, jobject o125, jobject o126, jobject o127,
    jobject o128, jobject o129, jobject o130, jobject o131, jobject o132, jobject o133, jobject o134, jobject o135,
    jobject o136, jobject o137, jobject o138, jobject o139, jobject o140, jobject o141, jobject o142, jobject o143,
    jobject o144, jobject o145, jobject o146, jobject o147, jobject o148, jobject o149, jobject o150, jobject o151,
    jobject o152, jobject o153, jobject o154, jobject o155, jobject o156, jobject o157, jobject o158, jobject o159,
    jobject o160, jobject o161, jobject o162, jobject o163, jobject o164, jobject o165, jobject o166, jobject o167,
    jobject o168, jobject o169, jobject o170, jobject o171, jobject o172, jobject o173, jobject o174, jobject o175,
    jobject o176, jobject o177, jobject o178, jobject o179, jobject o180, jobject o181, jobject o182, jobject o183,
    jobject o184, jobject o185, jobject o186, jobject o187, jobject o188, jobject o189, jobject o190, jobject o191,
    jobject o192, jobject o193, jobject o194, jobject o195, jobject o196, jobject o197, jobject o198, jobject o199,
    jobject o200, jobject o201, jobject o202, jobject o203, jobject o204, jobject o205, jobject o206, jobject o207,
    jobject o208, jobject o209, jobject o210, jobject o211, jobject o212, jobject o213, jobject o214, jobject o215,
    jobject o216, jobject o217, jobject o218, jobject o219, jobject o220, jobject o221, jobject o222, jobject o223,
    jobject o224, jobject o225, jobject o226, jobject o227, jobject o228, jobject o229, jobject o230, jobject o231,
    jobject o232, jobject o233, jobject o234, jobject o235, jobject o236, jobject o237, jobject o238, jobject o239,
    jobject o240, jobject o241, jobject o242, jobject o243, jobject o244, jobject o245, jobject o246, jobject o247,
    jobject o248, jobject o249, jobject o250, jobject o251, jobject o252, jobject o253) {
  EXPECT_EQ(kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != nullptr);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_GE(255U, Thread::Current()->NumStackReferences());

  // two tests possible
  if (o0 == nullptr) {
    // 1) everything is null
    EXPECT_TRUE(o0 == nullptr && o1 == nullptr && o2 == nullptr && o3 == nullptr && o4 == nullptr
        && o5 == nullptr && o6 == nullptr && o7 == nullptr && o8 == nullptr && o9 == nullptr
        && o10 == nullptr && o11 == nullptr && o12 == nullptr && o13 == nullptr && o14 == nullptr
        && o15 == nullptr && o16 == nullptr && o17 == nullptr && o18 == nullptr && o19 == nullptr
        && o20 == nullptr && o21 == nullptr && o22 == nullptr && o23 == nullptr && o24 == nullptr
        && o25 == nullptr && o26 == nullptr && o27 == nullptr && o28 == nullptr && o29 == nullptr
        && o30 == nullptr && o31 == nullptr && o32 == nullptr && o33 == nullptr && o34 == nullptr
        && o35 == nullptr && o36 == nullptr && o37 == nullptr && o38 == nullptr && o39 == nullptr
        && o40 == nullptr && o41 == nullptr && o42 == nullptr && o43 == nullptr && o44 == nullptr
        && o45 == nullptr && o46 == nullptr && o47 == nullptr && o48 == nullptr && o49 == nullptr
        && o50 == nullptr && o51 == nullptr && o52 == nullptr && o53 == nullptr && o54 == nullptr
        && o55 == nullptr && o56 == nullptr && o57 == nullptr && o58 == nullptr && o59 == nullptr
        && o60 == nullptr && o61 == nullptr && o62 == nullptr && o63 == nullptr && o64 == nullptr
        && o65 == nullptr && o66 == nullptr && o67 == nullptr && o68 == nullptr && o69 == nullptr
        && o70 == nullptr && o71 == nullptr && o72 == nullptr && o73 == nullptr && o74 == nullptr
        && o75 == nullptr && o76 == nullptr && o77 == nullptr && o78 == nullptr && o79 == nullptr
        && o80 == nullptr && o81 == nullptr && o82 == nullptr && o83 == nullptr && o84 == nullptr
        && o85 == nullptr && o86 == nullptr && o87 == nullptr && o88 == nullptr && o89 == nullptr
        && o90 == nullptr && o91 == nullptr && o92 == nullptr && o93 == nullptr && o94 == nullptr
        && o95 == nullptr && o96 == nullptr && o97 == nullptr && o98 == nullptr && o99 == nullptr
        && o100 == nullptr && o101 == nullptr && o102 == nullptr && o103 == nullptr && o104 == nullptr
        && o105 == nullptr && o106 == nullptr && o107 == nullptr && o108 == nullptr && o109 == nullptr
        && o110 == nullptr && o111 == nullptr && o112 == nullptr && o113 == nullptr && o114 == nullptr
        && o115 == nullptr && o116 == nullptr && o117 == nullptr && o118 == nullptr && o119 == nullptr
        && o120 == nullptr && o121 == nullptr && o122 == nullptr && o123 == nullptr && o124 == nullptr
        && o125 == nullptr && o126 == nullptr && o127 == nullptr && o128 == nullptr && o129 == nullptr
        && o130 == nullptr && o131 == nullptr && o132 == nullptr && o133 == nullptr && o134 == nullptr
        && o135 == nullptr && o136 == nullptr && o137 == nullptr && o138 == nullptr && o139 == nullptr
        && o140 == nullptr && o141 == nullptr && o142 == nullptr && o143 == nullptr && o144 == nullptr
        && o145 == nullptr && o146 == nullptr && o147 == nullptr && o148 == nullptr && o149 == nullptr
        && o150 == nullptr && o151 == nullptr && o152 == nullptr && o153 == nullptr && o154 == nullptr
        && o155 == nullptr && o156 == nullptr && o157 == nullptr && o158 == nullptr && o159 == nullptr
        && o160 == nullptr && o161 == nullptr && o162 == nullptr && o163 == nullptr && o164 == nullptr
        && o165 == nullptr && o166 == nullptr && o167 == nullptr && o168 == nullptr && o169 == nullptr
        && o170 == nullptr && o171 == nullptr && o172 == nullptr && o173 == nullptr && o174 == nullptr
        && o175 == nullptr && o176 == nullptr && o177 == nullptr && o178 == nullptr && o179 == nullptr
        && o180 == nullptr && o181 == nullptr && o182 == nullptr && o183 == nullptr && o184 == nullptr
        && o185 == nullptr && o186 == nullptr && o187 == nullptr && o188 == nullptr && o189 == nullptr
        && o190 == nullptr && o191 == nullptr && o192 == nullptr && o193 == nullptr && o194 == nullptr
        && o195 == nullptr && o196 == nullptr && o197 == nullptr && o198 == nullptr && o199 == nullptr
        && o200 == nullptr && o201 == nullptr && o202 == nullptr && o203 == nullptr && o204 == nullptr
        && o205 == nullptr && o206 == nullptr && o207 == nullptr && o208 == nullptr && o209 == nullptr
        && o210 == nullptr && o211 == nullptr && o212 == nullptr && o213 == nullptr && o214 == nullptr
        && o215 == nullptr && o216 == nullptr && o217 == nullptr && o218 == nullptr && o219 == nullptr
        && o220 == nullptr && o221 == nullptr && o222 == nullptr && o223 == nullptr && o224 == nullptr
        && o225 == nullptr && o226 == nullptr && o227 == nullptr && o228 == nullptr && o229 == nullptr
        && o230 == nullptr && o231 == nullptr && o232 == nullptr && o233 == nullptr && o234 == nullptr
        && o235 == nullptr && o236 == nullptr && o237 == nullptr && o238 == nullptr && o239 == nullptr
        && o240 == nullptr && o241 == nullptr && o242 == nullptr && o243 == nullptr && o244 == nullptr
        && o245 == nullptr && o246 == nullptr && o247 == nullptr && o248 == nullptr && o249 == nullptr
        && o250 == nullptr && o251 == nullptr && o252 == nullptr && o253 == nullptr);
  } else {
    EXPECT_EQ(0, env->GetArrayLength(reinterpret_cast<jarray>(o0)));
    EXPECT_EQ(1, env->GetArrayLength(reinterpret_cast<jarray>(o1)));
    EXPECT_EQ(2, env->GetArrayLength(reinterpret_cast<jarray>(o2)));
    EXPECT_EQ(3, env->GetArrayLength(reinterpret_cast<jarray>(o3)));
    EXPECT_EQ(4, env->GetArrayLength(reinterpret_cast<jarray>(o4)));
    EXPECT_EQ(5, env->GetArrayLength(reinterpret_cast<jarray>(o5)));
    EXPECT_EQ(6, env->GetArrayLength(reinterpret_cast<jarray>(o6)));
    EXPECT_EQ(7, env->GetArrayLength(reinterpret_cast<jarray>(o7)));
    EXPECT_EQ(8, env->GetArrayLength(reinterpret_cast<jarray>(o8)));
    EXPECT_EQ(9, env->GetArrayLength(reinterpret_cast<jarray>(o9)));
    EXPECT_EQ(10, env->GetArrayLength(reinterpret_cast<jarray>(o10)));
    EXPECT_EQ(11, env->GetArrayLength(reinterpret_cast<jarray>(o11)));
    EXPECT_EQ(12, env->GetArrayLength(reinterpret_cast<jarray>(o12)));
    EXPECT_EQ(13, env->GetArrayLength(reinterpret_cast<jarray>(o13)));
    EXPECT_EQ(14, env->GetArrayLength(reinterpret_cast<jarray>(o14)));
    EXPECT_EQ(15, env->GetArrayLength(reinterpret_cast<jarray>(o15)));
    EXPECT_EQ(16, env->GetArrayLength(reinterpret_cast<jarray>(o16)));
    EXPECT_EQ(17, env->GetArrayLength(reinterpret_cast<jarray>(o17)));
    EXPECT_EQ(18, env->GetArrayLength(reinterpret_cast<jarray>(o18)));
    EXPECT_EQ(19, env->GetArrayLength(reinterpret_cast<jarray>(o19)));
    EXPECT_EQ(20, env->GetArrayLength(reinterpret_cast<jarray>(o20)));
    EXPECT_EQ(21, env->GetArrayLength(reinterpret_cast<jarray>(o21)));
    EXPECT_EQ(22, env->GetArrayLength(reinterpret_cast<jarray>(o22)));
    EXPECT_EQ(23, env->GetArrayLength(reinterpret_cast<jarray>(o23)));
    EXPECT_EQ(24, env->GetArrayLength(reinterpret_cast<jarray>(o24)));
    EXPECT_EQ(25, env->GetArrayLength(reinterpret_cast<jarray>(o25)));
    EXPECT_EQ(26, env->GetArrayLength(reinterpret_cast<jarray>(o26)));
    EXPECT_EQ(27, env->GetArrayLength(reinterpret_cast<jarray>(o27)));
    EXPECT_EQ(28, env->GetArrayLength(reinterpret_cast<jarray>(o28)));
    EXPECT_EQ(29, env->GetArrayLength(reinterpret_cast<jarray>(o29)));
    EXPECT_EQ(30, env->GetArrayLength(reinterpret_cast<jarray>(o30)));
    EXPECT_EQ(31, env->GetArrayLength(reinterpret_cast<jarray>(o31)));
    EXPECT_EQ(32, env->GetArrayLength(reinterpret_cast<jarray>(o32)));
    EXPECT_EQ(33, env->GetArrayLength(reinterpret_cast<jarray>(o33)));
    EXPECT_EQ(34, env->GetArrayLength(reinterpret_cast<jarray>(o34)));
    EXPECT_EQ(35, env->GetArrayLength(reinterpret_cast<jarray>(o35)));
    EXPECT_EQ(36, env->GetArrayLength(reinterpret_cast<jarray>(o36)));
    EXPECT_EQ(37, env->GetArrayLength(reinterpret_cast<jarray>(o37)));
    EXPECT_EQ(38, env->GetArrayLength(reinterpret_cast<jarray>(o38)));
    EXPECT_EQ(39, env->GetArrayLength(reinterpret_cast<jarray>(o39)));
    EXPECT_EQ(40, env->GetArrayLength(reinterpret_cast<jarray>(o40)));
    EXPECT_EQ(41, env->GetArrayLength(reinterpret_cast<jarray>(o41)));
    EXPECT_EQ(42, env->GetArrayLength(reinterpret_cast<jarray>(o42)));
    EXPECT_EQ(43, env->GetArrayLength(reinterpret_cast<jarray>(o43)));
    EXPECT_EQ(44, env->GetArrayLength(reinterpret_cast<jarray>(o44)));
    EXPECT_EQ(45, env->GetArrayLength(reinterpret_cast<jarray>(o45)));
    EXPECT_EQ(46, env->GetArrayLength(reinterpret_cast<jarray>(o46)));
    EXPECT_EQ(47, env->GetArrayLength(reinterpret_cast<jarray>(o47)));
    EXPECT_EQ(48, env->GetArrayLength(reinterpret_cast<jarray>(o48)));
    EXPECT_EQ(49, env->GetArrayLength(reinterpret_cast<jarray>(o49)));
    EXPECT_EQ(50, env->GetArrayLength(reinterpret_cast<jarray>(o50)));
    EXPECT_EQ(51, env->GetArrayLength(reinterpret_cast<jarray>(o51)));
    EXPECT_EQ(52, env->GetArrayLength(reinterpret_cast<jarray>(o52)));
    EXPECT_EQ(53, env->GetArrayLength(reinterpret_cast<jarray>(o53)));
    EXPECT_EQ(54, env->GetArrayLength(reinterpret_cast<jarray>(o54)));
    EXPECT_EQ(55, env->GetArrayLength(reinterpret_cast<jarray>(o55)));
    EXPECT_EQ(56, env->GetArrayLength(reinterpret_cast<jarray>(o56)));
    EXPECT_EQ(57, env->GetArrayLength(reinterpret_cast<jarray>(o57)));
    EXPECT_EQ(58, env->GetArrayLength(reinterpret_cast<jarray>(o58)));
    EXPECT_EQ(59, env->GetArrayLength(reinterpret_cast<jarray>(o59)));
    EXPECT_EQ(60, env->GetArrayLength(reinterpret_cast<jarray>(o60)));
    EXPECT_EQ(61, env->GetArrayLength(reinterpret_cast<jarray>(o61)));
    EXPECT_EQ(62, env->GetArrayLength(reinterpret_cast<jarray>(o62)));
    EXPECT_EQ(63, env->GetArrayLength(reinterpret_cast<jarray>(o63)));
    EXPECT_EQ(64, env->GetArrayLength(reinterpret_cast<jarray>(o64)));
    EXPECT_EQ(65, env->GetArrayLength(reinterpret_cast<jarray>(o65)));
    EXPECT_EQ(66, env->GetArrayLength(reinterpret_cast<jarray>(o66)));
    EXPECT_EQ(67, env->GetArrayLength(reinterpret_cast<jarray>(o67)));
    EXPECT_EQ(68, env->GetArrayLength(reinterpret_cast<jarray>(o68)));
    EXPECT_EQ(69, env->GetArrayLength(reinterpret_cast<jarray>(o69)));
    EXPECT_EQ(70, env->GetArrayLength(reinterpret_cast<jarray>(o70)));
    EXPECT_EQ(71, env->GetArrayLength(reinterpret_cast<jarray>(o71)));
    EXPECT_EQ(72, env->GetArrayLength(reinterpret_cast<jarray>(o72)));
    EXPECT_EQ(73, env->GetArrayLength(reinterpret_cast<jarray>(o73)));
    EXPECT_EQ(74, env->GetArrayLength(reinterpret_cast<jarray>(o74)));
    EXPECT_EQ(75, env->GetArrayLength(reinterpret_cast<jarray>(o75)));
    EXPECT_EQ(76, env->GetArrayLength(reinterpret_cast<jarray>(o76)));
    EXPECT_EQ(77, env->GetArrayLength(reinterpret_cast<jarray>(o77)));
    EXPECT_EQ(78, env->GetArrayLength(reinterpret_cast<jarray>(o78)));
    EXPECT_EQ(79, env->GetArrayLength(reinterpret_cast<jarray>(o79)));
    EXPECT_EQ(80, env->GetArrayLength(reinterpret_cast<jarray>(o80)));
    EXPECT_EQ(81, env->GetArrayLength(reinterpret_cast<jarray>(o81)));
    EXPECT_EQ(82, env->GetArrayLength(reinterpret_cast<jarray>(o82)));
    EXPECT_EQ(83, env->GetArrayLength(reinterpret_cast<jarray>(o83)));
    EXPECT_EQ(84, env->GetArrayLength(reinterpret_cast<jarray>(o84)));
    EXPECT_EQ(85, env->GetArrayLength(reinterpret_cast<jarray>(o85)));
    EXPECT_EQ(86, env->GetArrayLength(reinterpret_cast<jarray>(o86)));
    EXPECT_EQ(87, env->GetArrayLength(reinterpret_cast<jarray>(o87)));
    EXPECT_EQ(88, env->GetArrayLength(reinterpret_cast<jarray>(o88)));
    EXPECT_EQ(89, env->GetArrayLength(reinterpret_cast<jarray>(o89)));
    EXPECT_EQ(90, env->GetArrayLength(reinterpret_cast<jarray>(o90)));
    EXPECT_EQ(91, env->GetArrayLength(reinterpret_cast<jarray>(o91)));
    EXPECT_EQ(92, env->GetArrayLength(reinterpret_cast<jarray>(o92)));
    EXPECT_EQ(93, env->GetArrayLength(reinterpret_cast<jarray>(o93)));
    EXPECT_EQ(94, env->GetArrayLength(reinterpret_cast<jarray>(o94)));
    EXPECT_EQ(95, env->GetArrayLength(reinterpret_cast<jarray>(o95)));
    EXPECT_EQ(96, env->GetArrayLength(reinterpret_cast<jarray>(o96)));
    EXPECT_EQ(97, env->GetArrayLength(reinterpret_cast<jarray>(o97)));
    EXPECT_EQ(98, env->GetArrayLength(reinterpret_cast<jarray>(o98)));
    EXPECT_EQ(99, env->GetArrayLength(reinterpret_cast<jarray>(o99)));
    EXPECT_EQ(100, env->GetArrayLength(reinterpret_cast<jarray>(o100)));
    EXPECT_EQ(101, env->GetArrayLength(reinterpret_cast<jarray>(o101)));
    EXPECT_EQ(102, env->GetArrayLength(reinterpret_cast<jarray>(o102)));
    EXPECT_EQ(103, env->GetArrayLength(reinterpret_cast<jarray>(o103)));
    EXPECT_EQ(104, env->GetArrayLength(reinterpret_cast<jarray>(o104)));
    EXPECT_EQ(105, env->GetArrayLength(reinterpret_cast<jarray>(o105)));
    EXPECT_EQ(106, env->GetArrayLength(reinterpret_cast<jarray>(o106)));
    EXPECT_EQ(107, env->GetArrayLength(reinterpret_cast<jarray>(o107)));
    EXPECT_EQ(108, env->GetArrayLength(reinterpret_cast<jarray>(o108)));
    EXPECT_EQ(109, env->GetArrayLength(reinterpret_cast<jarray>(o109)));
    EXPECT_EQ(110, env->GetArrayLength(reinterpret_cast<jarray>(o110)));
    EXPECT_EQ(111, env->GetArrayLength(reinterpret_cast<jarray>(o111)));
    EXPECT_EQ(112, env->GetArrayLength(reinterpret_cast<jarray>(o112)));
    EXPECT_EQ(113, env->GetArrayLength(reinterpret_cast<jarray>(o113)));
    EXPECT_EQ(114, env->GetArrayLength(reinterpret_cast<jarray>(o114)));
    EXPECT_EQ(115, env->GetArrayLength(reinterpret_cast<jarray>(o115)));
    EXPECT_EQ(116, env->GetArrayLength(reinterpret_cast<jarray>(o116)));
    EXPECT_EQ(117, env->GetArrayLength(reinterpret_cast<jarray>(o117)));
    EXPECT_EQ(118, env->GetArrayLength(reinterpret_cast<jarray>(o118)));
    EXPECT_EQ(119, env->GetArrayLength(reinterpret_cast<jarray>(o119)));
    EXPECT_EQ(120, env->GetArrayLength(reinterpret_cast<jarray>(o120)));
    EXPECT_EQ(121, env->GetArrayLength(reinterpret_cast<jarray>(o121)));
    EXPECT_EQ(122, env->GetArrayLength(reinterpret_cast<jarray>(o122)));
    EXPECT_EQ(123, env->GetArrayLength(reinterpret_cast<jarray>(o123)));
    EXPECT_EQ(124, env->GetArrayLength(reinterpret_cast<jarray>(o124)));
    EXPECT_EQ(125, env->GetArrayLength(reinterpret_cast<jarray>(o125)));
    EXPECT_EQ(126, env->GetArrayLength(reinterpret_cast<jarray>(o126)));
    EXPECT_EQ(127, env->GetArrayLength(reinterpret_cast<jarray>(o127)));
    EXPECT_EQ(128, env->GetArrayLength(reinterpret_cast<jarray>(o128)));
    EXPECT_EQ(129, env->GetArrayLength(reinterpret_cast<jarray>(o129)));
    EXPECT_EQ(130, env->GetArrayLength(reinterpret_cast<jarray>(o130)));
    EXPECT_EQ(131, env->GetArrayLength(reinterpret_cast<jarray>(o131)));
    EXPECT_EQ(132, env->GetArrayLength(reinterpret_cast<jarray>(o132)));
    EXPECT_EQ(133, env->GetArrayLength(reinterpret_cast<jarray>(o133)));
    EXPECT_EQ(134, env->GetArrayLength(reinterpret_cast<jarray>(o134)));
    EXPECT_EQ(135, env->GetArrayLength(reinterpret_cast<jarray>(o135)));
    EXPECT_EQ(136, env->GetArrayLength(reinterpret_cast<jarray>(o136)));
    EXPECT_EQ(137, env->GetArrayLength(reinterpret_cast<jarray>(o137)));
    EXPECT_EQ(138, env->GetArrayLength(reinterpret_cast<jarray>(o138)));
    EXPECT_EQ(139, env->GetArrayLength(reinterpret_cast<jarray>(o139)));
    EXPECT_EQ(140, env->GetArrayLength(reinterpret_cast<jarray>(o140)));
    EXPECT_EQ(141, env->GetArrayLength(reinterpret_cast<jarray>(o141)));
    EXPECT_EQ(142, env->GetArrayLength(reinterpret_cast<jarray>(o142)));
    EXPECT_EQ(143, env->GetArrayLength(reinterpret_cast<jarray>(o143)));
    EXPECT_EQ(144, env->GetArrayLength(reinterpret_cast<jarray>(o144)));
    EXPECT_EQ(145, env->GetArrayLength(reinterpret_cast<jarray>(o145)));
    EXPECT_EQ(146, env->GetArrayLength(reinterpret_cast<jarray>(o146)));
    EXPECT_EQ(147, env->GetArrayLength(reinterpret_cast<jarray>(o147)));
    EXPECT_EQ(148, env->GetArrayLength(reinterpret_cast<jarray>(o148)));
    EXPECT_EQ(149, env->GetArrayLength(reinterpret_cast<jarray>(o149)));
    EXPECT_EQ(150, env->GetArrayLength(reinterpret_cast<jarray>(o150)));
    EXPECT_EQ(151, env->GetArrayLength(reinterpret_cast<jarray>(o151)));
    EXPECT_EQ(152, env->GetArrayLength(reinterpret_cast<jarray>(o152)));
    EXPECT_EQ(153, env->GetArrayLength(reinterpret_cast<jarray>(o153)));
    EXPECT_EQ(154, env->GetArrayLength(reinterpret_cast<jarray>(o154)));
    EXPECT_EQ(155, env->GetArrayLength(reinterpret_cast<jarray>(o155)));
    EXPECT_EQ(156, env->GetArrayLength(reinterpret_cast<jarray>(o156)));
    EXPECT_EQ(157, env->GetArrayLength(reinterpret_cast<jarray>(o157)));
    EXPECT_EQ(158, env->GetArrayLength(reinterpret_cast<jarray>(o158)));
    EXPECT_EQ(159, env->GetArrayLength(reinterpret_cast<jarray>(o159)));
    EXPECT_EQ(160, env->GetArrayLength(reinterpret_cast<jarray>(o160)));
    EXPECT_EQ(161, env->GetArrayLength(reinterpret_cast<jarray>(o161)));
    EXPECT_EQ(162, env->GetArrayLength(reinterpret_cast<jarray>(o162)));
    EXPECT_EQ(163, env->GetArrayLength(reinterpret_cast<jarray>(o163)));
    EXPECT_EQ(164, env->GetArrayLength(reinterpret_cast<jarray>(o164)));
    EXPECT_EQ(165, env->GetArrayLength(reinterpret_cast<jarray>(o165)));
    EXPECT_EQ(166, env->GetArrayLength(reinterpret_cast<jarray>(o166)));
    EXPECT_EQ(167, env->GetArrayLength(reinterpret_cast<jarray>(o167)));
    EXPECT_EQ(168, env->GetArrayLength(reinterpret_cast<jarray>(o168)));
    EXPECT_EQ(169, env->GetArrayLength(reinterpret_cast<jarray>(o169)));
    EXPECT_EQ(170, env->GetArrayLength(reinterpret_cast<jarray>(o170)));
    EXPECT_EQ(171, env->GetArrayLength(reinterpret_cast<jarray>(o171)));
    EXPECT_EQ(172, env->GetArrayLength(reinterpret_cast<jarray>(o172)));
    EXPECT_EQ(173, env->GetArrayLength(reinterpret_cast<jarray>(o173)));
    EXPECT_EQ(174, env->GetArrayLength(reinterpret_cast<jarray>(o174)));
    EXPECT_EQ(175, env->GetArrayLength(reinterpret_cast<jarray>(o175)));
    EXPECT_EQ(176, env->GetArrayLength(reinterpret_cast<jarray>(o176)));
    EXPECT_EQ(177, env->GetArrayLength(reinterpret_cast<jarray>(o177)));
    EXPECT_EQ(178, env->GetArrayLength(reinterpret_cast<jarray>(o178)));
    EXPECT_EQ(179, env->GetArrayLength(reinterpret_cast<jarray>(o179)));
    EXPECT_EQ(180, env->GetArrayLength(reinterpret_cast<jarray>(o180)));
    EXPECT_EQ(181, env->GetArrayLength(reinterpret_cast<jarray>(o181)));
    EXPECT_EQ(182, env->GetArrayLength(reinterpret_cast<jarray>(o182)));
    EXPECT_EQ(183, env->GetArrayLength(reinterpret_cast<jarray>(o183)));
    EXPECT_EQ(184, env->GetArrayLength(reinterpret_cast<jarray>(o184)));
    EXPECT_EQ(185, env->GetArrayLength(reinterpret_cast<jarray>(o185)));
    EXPECT_EQ(186, env->GetArrayLength(reinterpret_cast<jarray>(o186)));
    EXPECT_EQ(187, env->GetArrayLength(reinterpret_cast<jarray>(o187)));
    EXPECT_EQ(188, env->GetArrayLength(reinterpret_cast<jarray>(o188)));
    EXPECT_EQ(189, env->GetArrayLength(reinterpret_cast<jarray>(o189)));
    EXPECT_EQ(190, env->GetArrayLength(reinterpret_cast<jarray>(o190)));
    EXPECT_EQ(191, env->GetArrayLength(reinterpret_cast<jarray>(o191)));
    EXPECT_EQ(192, env->GetArrayLength(reinterpret_cast<jarray>(o192)));
    EXPECT_EQ(193, env->GetArrayLength(reinterpret_cast<jarray>(o193)));
    EXPECT_EQ(194, env->GetArrayLength(reinterpret_cast<jarray>(o194)));
    EXPECT_EQ(195, env->GetArrayLength(reinterpret_cast<jarray>(o195)));
    EXPECT_EQ(196, env->GetArrayLength(reinterpret_cast<jarray>(o196)));
    EXPECT_EQ(197, env->GetArrayLength(reinterpret_cast<jarray>(o197)));
    EXPECT_EQ(198, env->GetArrayLength(reinterpret_cast<jarray>(o198)));
    EXPECT_EQ(199, env->GetArrayLength(reinterpret_cast<jarray>(o199)));
    EXPECT_EQ(200, env->GetArrayLength(reinterpret_cast<jarray>(o200)));
    EXPECT_EQ(201, env->GetArrayLength(reinterpret_cast<jarray>(o201)));
    EXPECT_EQ(202, env->GetArrayLength(reinterpret_cast<jarray>(o202)));
    EXPECT_EQ(203, env->GetArrayLength(reinterpret_cast<jarray>(o203)));
    EXPECT_EQ(204, env->GetArrayLength(reinterpret_cast<jarray>(o204)));
    EXPECT_EQ(205, env->GetArrayLength(reinterpret_cast<jarray>(o205)));
    EXPECT_EQ(206, env->GetArrayLength(reinterpret_cast<jarray>(o206)));
    EXPECT_EQ(207, env->GetArrayLength(reinterpret_cast<jarray>(o207)));
    EXPECT_EQ(208, env->GetArrayLength(reinterpret_cast<jarray>(o208)));
    EXPECT_EQ(209, env->GetArrayLength(reinterpret_cast<jarray>(o209)));
    EXPECT_EQ(210, env->GetArrayLength(reinterpret_cast<jarray>(o210)));
    EXPECT_EQ(211, env->GetArrayLength(reinterpret_cast<jarray>(o211)));
    EXPECT_EQ(212, env->GetArrayLength(reinterpret_cast<jarray>(o212)));
    EXPECT_EQ(213, env->GetArrayLength(reinterpret_cast<jarray>(o213)));
    EXPECT_EQ(214, env->GetArrayLength(reinterpret_cast<jarray>(o214)));
    EXPECT_EQ(215, env->GetArrayLength(reinterpret_cast<jarray>(o215)));
    EXPECT_EQ(216, env->GetArrayLength(reinterpret_cast<jarray>(o216)));
    EXPECT_EQ(217, env->GetArrayLength(reinterpret_cast<jarray>(o217)));
    EXPECT_EQ(218, env->GetArrayLength(reinterpret_cast<jarray>(o218)));
    EXPECT_EQ(219, env->GetArrayLength(reinterpret_cast<jarray>(o219)));
    EXPECT_EQ(220, env->GetArrayLength(reinterpret_cast<jarray>(o220)));
    EXPECT_EQ(221, env->GetArrayLength(reinterpret_cast<jarray>(o221)));
    EXPECT_EQ(222, env->GetArrayLength(reinterpret_cast<jarray>(o222)));
    EXPECT_EQ(223, env->GetArrayLength(reinterpret_cast<jarray>(o223)));
    EXPECT_EQ(224, env->GetArrayLength(reinterpret_cast<jarray>(o224)));
    EXPECT_EQ(225, env->GetArrayLength(reinterpret_cast<jarray>(o225)));
    EXPECT_EQ(226, env->GetArrayLength(reinterpret_cast<jarray>(o226)));
    EXPECT_EQ(227, env->GetArrayLength(reinterpret_cast<jarray>(o227)));
    EXPECT_EQ(228, env->GetArrayLength(reinterpret_cast<jarray>(o228)));
    EXPECT_EQ(229, env->GetArrayLength(reinterpret_cast<jarray>(o229)));
    EXPECT_EQ(230, env->GetArrayLength(reinterpret_cast<jarray>(o230)));
    EXPECT_EQ(231, env->GetArrayLength(reinterpret_cast<jarray>(o231)));
    EXPECT_EQ(232, env->GetArrayLength(reinterpret_cast<jarray>(o232)));
    EXPECT_EQ(233, env->GetArrayLength(reinterpret_cast<jarray>(o233)));
    EXPECT_EQ(234, env->GetArrayLength(reinterpret_cast<jarray>(o234)));
    EXPECT_EQ(235, env->GetArrayLength(reinterpret_cast<jarray>(o235)));
    EXPECT_EQ(236, env->GetArrayLength(reinterpret_cast<jarray>(o236)));
    EXPECT_EQ(237, env->GetArrayLength(reinterpret_cast<jarray>(o237)));
    EXPECT_EQ(238, env->GetArrayLength(reinterpret_cast<jarray>(o238)));
    EXPECT_EQ(239, env->GetArrayLength(reinterpret_cast<jarray>(o239)));
    EXPECT_EQ(240, env->GetArrayLength(reinterpret_cast<jarray>(o240)));
    EXPECT_EQ(241, env->GetArrayLength(reinterpret_cast<jarray>(o241)));
    EXPECT_EQ(242, env->GetArrayLength(reinterpret_cast<jarray>(o242)));
    EXPECT_EQ(243, env->GetArrayLength(reinterpret_cast<jarray>(o243)));
    EXPECT_EQ(244, env->GetArrayLength(reinterpret_cast<jarray>(o244)));
    EXPECT_EQ(245, env->GetArrayLength(reinterpret_cast<jarray>(o245)));
    EXPECT_EQ(246, env->GetArrayLength(reinterpret_cast<jarray>(o246)));
    EXPECT_EQ(247, env->GetArrayLength(reinterpret_cast<jarray>(o247)));
    EXPECT_EQ(248, env->GetArrayLength(reinterpret_cast<jarray>(o248)));
    EXPECT_EQ(249, env->GetArrayLength(reinterpret_cast<jarray>(o249)));
    EXPECT_EQ(250, env->GetArrayLength(reinterpret_cast<jarray>(o250)));
    EXPECT_EQ(251, env->GetArrayLength(reinterpret_cast<jarray>(o251)));
    EXPECT_EQ(252, env->GetArrayLength(reinterpret_cast<jarray>(o252)));
    EXPECT_EQ(253, env->GetArrayLength(reinterpret_cast<jarray>(o253)));
  }
}

const char* longSig =
    "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
    "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V";

void JniCompilerTest::MaxParamNumberImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "maxParamNumber", longSig,
               reinterpret_cast<void*>(&Java_MyClassNatives_maxParamNumber));

  jvalue args[254];

  // First test: test with all arguments null.
  for (int i = 0; i < 254; ++i) {
    args[i].l = nullptr;
  }

  env_->CallNonvirtualVoidMethodA(jobj_, jklass_, jmethod_, args);

  // Second test: test with int[] objects with increasing lengths
  for (int i = 0; i < 254; ++i) {
    jintArray tmp = env_->NewIntArray(i);
    args[i].l = tmp;
    EXPECT_NE(args[i].l, nullptr);
  }

  env_->CallNonvirtualVoidMethodA(jobj_, jklass_, jmethod_, args);
}

JNI_TEST(MaxParamNumber)

void JniCompilerTest::WithoutImplementationImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(false, "withoutImplementation", "()V", nullptr);

  env_->CallVoidMethod(jobj_, jmethod_);

  EXPECT_TRUE(Thread::Current()->IsExceptionPending());
  EXPECT_TRUE(env_->ExceptionCheck() == JNI_TRUE);
}

JNI_TEST(WithoutImplementation)

void Java_MyClassNatives_stackArgsIntsFirst(JNIEnv* env, jclass klass, jint i1, jint i2, jint i3,
                                            jint i4, jint i5, jint i6, jint i7, jint i8, jint i9,
                                            jint i10, jfloat f1, jfloat f2, jfloat f3, jfloat f4,
                                            jfloat f5, jfloat f6, jfloat f7, jfloat f8, jfloat f9,
                                            jfloat f10) {
  EXPECT_EQ(i1, 1);
  EXPECT_EQ(i2, 2);
  EXPECT_EQ(i3, 3);
  EXPECT_EQ(i4, 4);
  EXPECT_EQ(i5, 5);
  EXPECT_EQ(i6, 6);
  EXPECT_EQ(i7, 7);
  EXPECT_EQ(i8, 8);
  EXPECT_EQ(i9, 9);
  EXPECT_EQ(i10, 10);

  jint i11 = bit_cast<jfloat, jint>(f1);
  EXPECT_EQ(i11, 11);
  jint i12 = bit_cast<jfloat, jint>(f2);
  EXPECT_EQ(i12, 12);
  jint i13 = bit_cast<jfloat, jint>(f3);
  EXPECT_EQ(i13, 13);
  jint i14 = bit_cast<jfloat, jint>(f4);
  EXPECT_EQ(i14, 14);
  jint i15 = bit_cast<jfloat, jint>(f5);
  EXPECT_EQ(i15, 15);
  jint i16 = bit_cast<jfloat, jint>(f6);
  EXPECT_EQ(i16, 16);
  jint i17 = bit_cast<jfloat, jint>(f7);
  EXPECT_EQ(i17, 17);
  jint i18 = bit_cast<jfloat, jint>(f8);
  EXPECT_EQ(i18, 18);
  jint i19 = bit_cast<jfloat, jint>(f9);
  EXPECT_EQ(i19, 19);
  jint i20 = bit_cast<jfloat, jint>(f10);
  EXPECT_EQ(i20, 20);
}

void JniCompilerTest::StackArgsIntsFirstImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "stackArgsIntsFirst", "(IIIIIIIIIIFFFFFFFFFF)V",
               reinterpret_cast<void*>(&Java_MyClassNatives_stackArgsIntsFirst));

  jint i1 = 1;
  jint i2 = 2;
  jint i3 = 3;
  jint i4 = 4;
  jint i5 = 5;
  jint i6 = 6;
  jint i7 = 7;
  jint i8 = 8;
  jint i9 = 9;
  jint i10 = 10;

  jfloat f1 = bit_cast<jint, jfloat>(11);
  jfloat f2 = bit_cast<jint, jfloat>(12);
  jfloat f3 = bit_cast<jint, jfloat>(13);
  jfloat f4 = bit_cast<jint, jfloat>(14);
  jfloat f5 = bit_cast<jint, jfloat>(15);
  jfloat f6 = bit_cast<jint, jfloat>(16);
  jfloat f7 = bit_cast<jint, jfloat>(17);
  jfloat f8 = bit_cast<jint, jfloat>(18);
  jfloat f9 = bit_cast<jint, jfloat>(19);
  jfloat f10 = bit_cast<jint, jfloat>(20);

  env_->CallStaticVoidMethod(jklass_, jmethod_, i1, i2, i3, i4, i5, i6, i7, i8, i9, i10, f1, f2,
                             f3, f4, f5, f6, f7, f8, f9, f10);
}

JNI_TEST(StackArgsIntsFirst)

void Java_MyClassNatives_stackArgsFloatsFirst(JNIEnv* env, jclass klass, jfloat f1, jfloat f2,
                                              jfloat f3, jfloat f4, jfloat f5, jfloat f6, jfloat f7,
                                              jfloat f8, jfloat f9, jfloat f10, jint i1, jint i2,
                                              jint i3, jint i4, jint i5, jint i6, jint i7, jint i8,
                                              jint i9, jint i10) {
  EXPECT_EQ(i1, 1);
  EXPECT_EQ(i2, 2);
  EXPECT_EQ(i3, 3);
  EXPECT_EQ(i4, 4);
  EXPECT_EQ(i5, 5);
  EXPECT_EQ(i6, 6);
  EXPECT_EQ(i7, 7);
  EXPECT_EQ(i8, 8);
  EXPECT_EQ(i9, 9);
  EXPECT_EQ(i10, 10);

  jint i11 = bit_cast<jfloat, jint>(f1);
  EXPECT_EQ(i11, 11);
  jint i12 = bit_cast<jfloat, jint>(f2);
  EXPECT_EQ(i12, 12);
  jint i13 = bit_cast<jfloat, jint>(f3);
  EXPECT_EQ(i13, 13);
  jint i14 = bit_cast<jfloat, jint>(f4);
  EXPECT_EQ(i14, 14);
  jint i15 = bit_cast<jfloat, jint>(f5);
  EXPECT_EQ(i15, 15);
  jint i16 = bit_cast<jfloat, jint>(f6);
  EXPECT_EQ(i16, 16);
  jint i17 = bit_cast<jfloat, jint>(f7);
  EXPECT_EQ(i17, 17);
  jint i18 = bit_cast<jfloat, jint>(f8);
  EXPECT_EQ(i18, 18);
  jint i19 = bit_cast<jfloat, jint>(f9);
  EXPECT_EQ(i19, 19);
  jint i20 = bit_cast<jfloat, jint>(f10);
  EXPECT_EQ(i20, 20);
}

void JniCompilerTest::StackArgsFloatsFirstImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "stackArgsFloatsFirst", "(FFFFFFFFFFIIIIIIIIII)V",
               reinterpret_cast<void*>(&Java_MyClassNatives_stackArgsFloatsFirst));

  jint i1 = 1;
  jint i2 = 2;
  jint i3 = 3;
  jint i4 = 4;
  jint i5 = 5;
  jint i6 = 6;
  jint i7 = 7;
  jint i8 = 8;
  jint i9 = 9;
  jint i10 = 10;

  jfloat f1 = bit_cast<jint, jfloat>(11);
  jfloat f2 = bit_cast<jint, jfloat>(12);
  jfloat f3 = bit_cast<jint, jfloat>(13);
  jfloat f4 = bit_cast<jint, jfloat>(14);
  jfloat f5 = bit_cast<jint, jfloat>(15);
  jfloat f6 = bit_cast<jint, jfloat>(16);
  jfloat f7 = bit_cast<jint, jfloat>(17);
  jfloat f8 = bit_cast<jint, jfloat>(18);
  jfloat f9 = bit_cast<jint, jfloat>(19);
  jfloat f10 = bit_cast<jint, jfloat>(20);

  env_->CallStaticVoidMethod(jklass_, jmethod_, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, i1, i2, i3,
                             i4, i5, i6, i7, i8, i9, i10);
}

JNI_TEST(StackArgsFloatsFirst)

void Java_MyClassNatives_stackArgsMixed(JNIEnv* env, jclass klass, jint i1, jfloat f1, jint i2,
                                        jfloat f2, jint i3, jfloat f3, jint i4, jfloat f4, jint i5,
                                        jfloat f5, jint i6, jfloat f6, jint i7, jfloat f7, jint i8,
                                        jfloat f8, jint i9, jfloat f9, jint i10, jfloat f10) {
  EXPECT_EQ(i1, 1);
  EXPECT_EQ(i2, 2);
  EXPECT_EQ(i3, 3);
  EXPECT_EQ(i4, 4);
  EXPECT_EQ(i5, 5);
  EXPECT_EQ(i6, 6);
  EXPECT_EQ(i7, 7);
  EXPECT_EQ(i8, 8);
  EXPECT_EQ(i9, 9);
  EXPECT_EQ(i10, 10);

  jint i11 = bit_cast<jfloat, jint>(f1);
  EXPECT_EQ(i11, 11);
  jint i12 = bit_cast<jfloat, jint>(f2);
  EXPECT_EQ(i12, 12);
  jint i13 = bit_cast<jfloat, jint>(f3);
  EXPECT_EQ(i13, 13);
  jint i14 = bit_cast<jfloat, jint>(f4);
  EXPECT_EQ(i14, 14);
  jint i15 = bit_cast<jfloat, jint>(f5);
  EXPECT_EQ(i15, 15);
  jint i16 = bit_cast<jfloat, jint>(f6);
  EXPECT_EQ(i16, 16);
  jint i17 = bit_cast<jfloat, jint>(f7);
  EXPECT_EQ(i17, 17);
  jint i18 = bit_cast<jfloat, jint>(f8);
  EXPECT_EQ(i18, 18);
  jint i19 = bit_cast<jfloat, jint>(f9);
  EXPECT_EQ(i19, 19);
  jint i20 = bit_cast<jfloat, jint>(f10);
  EXPECT_EQ(i20, 20);
}

void JniCompilerTest::StackArgsMixedImpl() {
  TEST_DISABLED_FOR_PORTABLE();
  SetUpForTest(true, "stackArgsMixed", "(IFIFIFIFIFIFIFIFIFIF)V",
               reinterpret_cast<void*>(&Java_MyClassNatives_stackArgsMixed));

  jint i1 = 1;
  jint i2 = 2;
  jint i3 = 3;
  jint i4 = 4;
  jint i5 = 5;
  jint i6 = 6;
  jint i7 = 7;
  jint i8 = 8;
  jint i9 = 9;
  jint i10 = 10;

  jfloat f1 = bit_cast<jint, jfloat>(11);
  jfloat f2 = bit_cast<jint, jfloat>(12);
  jfloat f3 = bit_cast<jint, jfloat>(13);
  jfloat f4 = bit_cast<jint, jfloat>(14);
  jfloat f5 = bit_cast<jint, jfloat>(15);
  jfloat f6 = bit_cast<jint, jfloat>(16);
  jfloat f7 = bit_cast<jint, jfloat>(17);
  jfloat f8 = bit_cast<jint, jfloat>(18);
  jfloat f9 = bit_cast<jint, jfloat>(19);
  jfloat f10 = bit_cast<jint, jfloat>(20);

  env_->CallStaticVoidMethod(jklass_, jmethod_, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7,
                             f7, i8, f8, i9, f9, i10, f10);
}

JNI_TEST(StackArgsMixed)

}  // namespace art
