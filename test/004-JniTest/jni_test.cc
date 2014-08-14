/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <vector>

#include "jni.h"

#if defined(NDEBUG)
#error test code compiled without NDEBUG
#endif

static JavaVM* jvm = NULL;

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
  assert(vm != nullptr);
  assert(jvm == nullptr);
  jvm = vm;
  return JNI_VERSION_1_6;
}

static void* AttachHelper(void* arg) {
  assert(jvm != nullptr);

  JNIEnv* env = nullptr;
  JavaVMAttachArgs args = { JNI_VERSION_1_6, __FUNCTION__, NULL };
  int attach_result = jvm->AttachCurrentThread(&env, &args);
  assert(attach_result == 0);

  typedef void (*Fn)(JNIEnv*);
  Fn fn = reinterpret_cast<Fn>(arg);
  fn(env);

  int detach_result = jvm->DetachCurrentThread();
  assert(detach_result == 0);
  return nullptr;
}

static void PthreadHelper(void (*fn)(JNIEnv*)) {
  pthread_t pthread;
  int pthread_create_result = pthread_create(&pthread, nullptr, AttachHelper,
                                             reinterpret_cast<void*>(fn));
  assert(pthread_create_result == 0);
  int pthread_join_result = pthread_join(pthread, nullptr);
  assert(pthread_join_result == 0);
}

static void testFindClassOnAttachedNativeThread(JNIEnv* env) {
  jclass clazz = env->FindClass("Main");
  assert(clazz != nullptr);
  assert(!env->ExceptionCheck());

  jobjectArray array = env->NewObjectArray(0, clazz, nullptr);
  assert(array != nullptr);
  assert(!env->ExceptionCheck());
}

// http://b/10994325
extern "C" JNIEXPORT void JNICALL Java_Main_testFindClassOnAttachedNativeThread(JNIEnv*, jclass) {
  PthreadHelper(&testFindClassOnAttachedNativeThread);
}

static void testFindFieldOnAttachedNativeThread(JNIEnv* env) {
  jclass clazz = env->FindClass("Main");
  assert(clazz != nullptr);
  assert(!env->ExceptionCheck());

  jfieldID field = env->GetStaticFieldID(clazz, "testFindFieldOnAttachedNativeThreadField", "Z");
  assert(field != nullptr);
  assert(!env->ExceptionCheck());

  env->SetStaticBooleanField(clazz, field, JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL Java_Main_testFindFieldOnAttachedNativeThreadNative(JNIEnv*,
                                                                                      jclass) {
  PthreadHelper(&testFindFieldOnAttachedNativeThread);
}

static void testReflectFieldGetFromAttachedNativeThread(JNIEnv* env) {
  jclass clazz = env->FindClass("Main");
  assert(clazz != nullptr);
  assert(!env->ExceptionCheck());

  jclass class_clazz = env->FindClass("java/lang/Class");
  assert(class_clazz != nullptr);
  assert(!env->ExceptionCheck());

  jmethodID getFieldMetodId = env->GetMethodID(class_clazz, "getField",
                                               "(Ljava/lang/String;)Ljava/lang/reflect/Field;");
  assert(getFieldMetodId != nullptr);
  assert(!env->ExceptionCheck());

  jstring field_name = env->NewStringUTF("testReflectFieldGetFromAttachedNativeThreadField");
  assert(field_name != nullptr);
  assert(!env->ExceptionCheck());

  jobject field = env->CallObjectMethod(clazz, getFieldMetodId, field_name);
  assert(field != nullptr);
  assert(!env->ExceptionCheck());

  jclass field_clazz = env->FindClass("java/lang/reflect/Field");
  assert(field_clazz != nullptr);
  assert(!env->ExceptionCheck());

  jmethodID getBooleanMetodId = env->GetMethodID(field_clazz, "getBoolean",
                                                 "(Ljava/lang/Object;)Z");
  assert(getBooleanMetodId != nullptr);
  assert(!env->ExceptionCheck());

  jboolean value = env->CallBooleanMethod(field, getBooleanMetodId, /* ignored */ clazz);
  assert(value == false);
  assert(!env->ExceptionCheck());
}

// http://b/15539150
extern "C" JNIEXPORT void JNICALL Java_Main_testReflectFieldGetFromAttachedNativeThreadNative(
    JNIEnv*, jclass) {
  PthreadHelper(&testReflectFieldGetFromAttachedNativeThread);
}


// http://b/11243757
extern "C" JNIEXPORT void JNICALL Java_Main_testCallStaticVoidMethodOnSubClassNative(JNIEnv* env,
                                                                                     jclass) {
  jclass super_class = env->FindClass("Main$testCallStaticVoidMethodOnSubClass_SuperClass");
  assert(super_class != nullptr);

  jmethodID execute = env->GetStaticMethodID(super_class, "execute", "()V");
  assert(execute != nullptr);

  jclass sub_class = env->FindClass("Main$testCallStaticVoidMethodOnSubClass_SubClass");
  assert(sub_class != nullptr);

  env->CallStaticVoidMethod(sub_class, execute);
}

extern "C" JNIEXPORT jobject JNICALL Java_Main_testGetMirandaMethodNative(JNIEnv* env, jclass) {
  jclass abstract_class = env->FindClass("Main$testGetMirandaMethod_MirandaAbstract");
  assert(abstract_class != nullptr);
  jmethodID miranda_method = env->GetMethodID(abstract_class, "inInterface", "()Z");
  assert(miranda_method != nullptr);
  return env->ToReflectedMethod(abstract_class, miranda_method, JNI_FALSE);
}

// https://code.google.com/p/android/issues/detail?id=63055
extern "C" void JNICALL Java_Main_testZeroLengthByteBuffers(JNIEnv* env, jclass) {
  std::vector<uint8_t> buffer(1);
  jobject byte_buffer = env->NewDirectByteBuffer(&buffer[0], 0);
  assert(byte_buffer != nullptr);
  assert(!env->ExceptionCheck());

  assert(env->GetDirectBufferAddress(byte_buffer) == &buffer[0]);
  assert(env->GetDirectBufferCapacity(byte_buffer) == 0);
}

constexpr size_t kByteReturnSize = 7;
jbyte byte_returns[kByteReturnSize] = { 0, 1, 2, 127, -1, -2, -128 };

extern "C" jbyte JNICALL Java_Main_byteMethod(JNIEnv* env, jclass klass, jbyte b1, jbyte b2,
                                              jbyte b3, jbyte b4, jbyte b5, jbyte b6,
                                              jbyte b7, jbyte b8, jbyte b9, jbyte b10) {
  // We use b1 to drive the output.
  assert(b2 == 2);
  assert(b3 == -3);
  assert(b4 == 4);
  assert(b5 == -5);
  assert(b6 == 6);
  assert(b7 == -7);
  assert(b8 == 8);
  assert(b9 == -9);
  assert(b10 == 10);

  assert(0 <= b1);
  assert(b1 < static_cast<jbyte>(kByteReturnSize));

  return byte_returns[b1];
}

constexpr size_t kShortReturnSize = 9;
jshort short_returns[kShortReturnSize] = { 0, 1, 2, 127, 32767, -1, -2, -128,
    static_cast<jshort>(0x8000) };
// The weird static_cast is because short int is only guaranteed down to -32767, not Java's -32768.

extern "C" jshort JNICALL Java_Main_shortMethod(JNIEnv* env, jclass klass, jshort s1, jshort s2,
                                                jshort s3, jshort s4, jshort s5, jshort s6,
                                                jshort s7, jshort s8, jshort s9, jshort s10) {
  // We use s1 to drive the output.
  assert(s2 == 2);
  assert(s3 == -3);
  assert(s4 == 4);
  assert(s5 == -5);
  assert(s6 == 6);
  assert(s7 == -7);
  assert(s8 == 8);
  assert(s9 == -9);
  assert(s10 == 10);

  assert(0 <= s1);
  assert(s1 < static_cast<jshort>(kShortReturnSize));

  return short_returns[s1];
}

extern "C" jboolean JNICALL Java_Main_booleanMethod(JNIEnv* env, jclass klass, jboolean b1,
                                                    jboolean b2, jboolean b3, jboolean b4,
                                                    jboolean b5, jboolean b6, jboolean b7,
                                                    jboolean b8, jboolean b9, jboolean b10) {
  // We use b1 to drive the output.
  assert(b2 == JNI_TRUE);
  assert(b3 == JNI_FALSE);
  assert(b4 == JNI_TRUE);
  assert(b5 == JNI_FALSE);
  assert(b6 == JNI_TRUE);
  assert(b7 == JNI_FALSE);
  assert(b8 == JNI_TRUE);
  assert(b9 == JNI_FALSE);
  assert(b10 == JNI_TRUE);

  assert(b1 == JNI_TRUE || b1 == JNI_FALSE);
  return b1;
}

constexpr size_t kCharReturnSize = 8;
jchar char_returns[kCharReturnSize] = { 0, 1, 2, 127, 255, 256, 15000, 34000 };

extern "C" jchar JNICALL Java_Main_charMethod(JNIEnv* env, jclass klacc, jchar c1, jchar c2,
                                              jchar c3, jchar c4, jchar c5, jchar c6, jchar c7,
                                              jchar c8, jchar c9, jchar c10) {
  // We use c1 to drive the output.
  assert(c2 == 'a');
  assert(c3 == 'b');
  assert(c4 == 'c');
  assert(c5 == '0');
  assert(c6 == '1');
  assert(c7 == '2');
  assert(c8 == 1234);
  assert(c9 == 2345);
  assert(c10 == 3456);

  assert(c1 < static_cast<jchar>(kCharReturnSize));

  return char_returns[c1];
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_nativeIsAssignableFrom(JNIEnv* env, jclass,
                                                                       jclass from, jclass to) {
  return env->IsAssignableFrom(from, to);
}

static void testShallowGetCallingClassLoader(JNIEnv* env) {
  // Test direct call.
  {
    jclass vmstack_clazz = env->FindClass("dalvik/system/VMStack");
    assert(vmstack_clazz != nullptr);
    assert(!env->ExceptionCheck());

    jmethodID getCallingClassLoaderMethodId = env->GetStaticMethodID(vmstack_clazz,
                                                                     "getCallingClassLoader",
                                                                     "()Ljava/lang/ClassLoader;");
    assert(getCallingClassLoaderMethodId != nullptr);
    assert(!env->ExceptionCheck());

    jobject class_loader = env->CallStaticObjectMethod(vmstack_clazz,
                                                       getCallingClassLoaderMethodId);
    assert(class_loader == nullptr);
    assert(!env->ExceptionCheck());
  }

  // Test one-level call. Use System.loadLibrary().
  {
    jclass system_clazz = env->FindClass("java/lang/System");
    assert(system_clazz != nullptr);
    assert(!env->ExceptionCheck());

    jmethodID loadLibraryMethodId = env->GetStaticMethodID(system_clazz, "loadLibrary",
                                                           "(Ljava/lang/String;)V");
    assert(loadLibraryMethodId != nullptr);
    assert(!env->ExceptionCheck());

    // Create a string object.
    jobject library_string = env->NewStringUTF("arttest");
    assert(library_string != nullptr);
    assert(!env->ExceptionCheck());

    env->CallStaticVoidMethod(system_clazz, loadLibraryMethodId, library_string);
    if (env->ExceptionCheck()) {
      // At most we expect UnsatisfiedLinkError.
      jthrowable thrown = env->ExceptionOccurred();
      env->ExceptionClear();

      jclass unsatisfied_link_error_clazz = env->FindClass("java/lang/UnsatisfiedLinkError");
      jclass thrown_class = env->GetObjectClass(thrown);
      assert(env->IsSameObject(unsatisfied_link_error_clazz, thrown_class));
    }
  }
}

// http://b/16867274
extern "C" JNIEXPORT void JNICALL Java_Main_nativeTestShallowGetCallingClassLoader(JNIEnv* env,
                                                                                   jclass) {
  PthreadHelper(&testShallowGetCallingClassLoader);
}

static void testShallowGetStackClass2(JNIEnv* env) {
  jclass vmstack_clazz = env->FindClass("dalvik/system/VMStack");
  assert(vmstack_clazz != nullptr);
  assert(!env->ExceptionCheck());

  // Test direct call.
  {
    jmethodID getStackClass2MethodId = env->GetStaticMethodID(vmstack_clazz, "getStackClass2",
                                                              "()Ljava/lang/Class;");
    assert(getStackClass2MethodId != nullptr);
    assert(!env->ExceptionCheck());

    jobject caller_class = env->CallStaticObjectMethod(vmstack_clazz, getStackClass2MethodId);
    assert(caller_class == nullptr);
    assert(!env->ExceptionCheck());
  }

  // Test one-level call. Use VMStack.getStackClass1().
  {
    jmethodID getStackClass1MethodId = env->GetStaticMethodID(vmstack_clazz, "getStackClass1",
                                                              "()Ljava/lang/Class;");
    assert(getStackClass1MethodId != nullptr);
    assert(!env->ExceptionCheck());

    jobject caller_class = env->CallStaticObjectMethod(vmstack_clazz, getStackClass1MethodId);
    assert(caller_class == nullptr);
    assert(!env->ExceptionCheck());
  }

  // For better testing we would need to compile against libcore and have a two-deep stack
  // ourselves.
}

extern "C" JNIEXPORT void JNICALL Java_Main_nativeTestShallowGetStackClass2(JNIEnv* env, jclass) {
  PthreadHelper(&testShallowGetStackClass2);
}
