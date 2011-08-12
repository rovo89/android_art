// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include <vector>
#include <utility>

#include "class_linker.h"
#include "jni.h"
#include "logging.h"
#include "runtime.h"
#include "scoped_ptr.h"
#include "stringpiece.h"
#include "thread.h"

#define UNIMPLEMENTED(LEVEL) LOG(LEVEL) << __FUNCTION__ << " unimplemented"

namespace art {

jint GetVersion(JNIEnv* env) {
  return JNI_VERSION_1_6;
}

jclass DefineClass(JNIEnv *env, const char *name,
    jobject loader, const jbyte *buf, jsize len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

// Section 12.3.2 of the JNI spec describes JNI class descriptors. They're
// separated with slashes but aren't wrapped with "L;" like regular descriptors
// (i.e. "a/b/C" rather than "La/b/C;"). Arrays of reference types are an
// exception; there the "L;" must be present ("[La/b/C;"). Historically we've
// supported names with dots too (such as "a.b.C").
std::string NormalizeJniClassDescriptor(const char* name) {
  std::string result;
  // Add the missing "L;" if necessary.
  if (name[0] == '[') {
    result = name;
  } else {
    result += 'L';
    result += name;
    result += ';';
  }
  // Rewrite '.' as '/' for backwards compatibility.
  for (size_t i = 0; i < result.size(); ++i) {
    if (result[i] == '.') {
      result[i] = '/';
    }
  }
  return result;
}

jclass FindClass(JNIEnv* env, const char* name) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  std::string descriptor(NormalizeJniClassDescriptor(name));
  // TODO: need to get the appropriate ClassLoader.
  Class* c = class_linker->FindClass(descriptor, NULL);
  // TODO: local reference.
  return reinterpret_cast<jclass>(c);
}

jmethodID FromReflectedMethod(JNIEnv* env, jobject method) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jfieldID FromReflectedField(JNIEnv* env, jobject field) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject ToReflectedMethod(JNIEnv* env, jclass cls,
    jmethodID methodID, jboolean isStatic) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jclass GetSuperclass(JNIEnv* env, jclass sub) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean IsAssignableFrom(JNIEnv* env, jclass sub, jclass sup) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jobject ToReflectedField(JNIEnv* env, jclass cls,
    jfieldID fieldID, jboolean isStatic) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jint Throw(JNIEnv* env, jthrowable obj) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint ThrowNew(JNIEnv* env, jclass clazz, const char* msg) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jthrowable ExceptionOccurred(JNIEnv* env) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ExceptionDescribe(JNIEnv* env) {
  UNIMPLEMENTED(FATAL);
}

void ExceptionClear(JNIEnv* env) {
  UNIMPLEMENTED(FATAL);
}

void FatalError(JNIEnv* env, const char* msg) {
  UNIMPLEMENTED(FATAL);
}

jint PushLocalFrame(JNIEnv* env, jint cap) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jobject PopLocalFrame(JNIEnv* env, jobject res) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject NewGlobalRef(JNIEnv* env, jobject lobj) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void DeleteGlobalRef(JNIEnv* env, jobject gref) {
  UNIMPLEMENTED(FATAL);
}

void DeleteLocalRef(JNIEnv* env, jobject obj) {
  UNIMPLEMENTED(FATAL);
}

jboolean IsSameObject(JNIEnv* env, jobject obj1, jobject obj2) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jobject NewLocalRef(JNIEnv* env, jobject ref) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jint EnsureLocalCapacity(JNIEnv* env, jint) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jobject AllocObject(JNIEnv* env, jclass clazz) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject NewObject(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject NewObjectV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject NewObjectA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jclass GetObjectClass(JNIEnv* env, jobject obj) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean IsInstanceOf(JNIEnv* env, jobject obj, jclass clazz) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jmethodID GetMethodID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallObjectMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallObjectMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallObjectMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue*  args) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean CallBooleanMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jboolean CallBooleanMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jboolean CallBooleanMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue*  args) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jbyte CallByteMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jbyte CallByteMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jbyte CallByteMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallCharMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallCharMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallCharMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallShortMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallShortMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallShortMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallIntMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallIntMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallIntMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallLongMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallLongMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallLongMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallFloatMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallFloatMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallFloatMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallDoubleMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallDoubleMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallDoubleMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

void CallVoidMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
}

void CallVoidMethodV(JNIEnv* env, jobject obj,
    jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
}

void CallVoidMethodA(JNIEnv* env, jobject obj,
    jmethodID methodID, jvalue*  args) {
  UNIMPLEMENTED(FATAL);
}

jobject CallNonvirtualObjectMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallNonvirtualObjectMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallNonvirtualObjectMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean CallNonvirtualBooleanMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jboolean CallNonvirtualBooleanMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jboolean CallNonvirtualBooleanMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jbyte CallNonvirtualByteMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jbyte CallNonvirtualByteMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jbyte CallNonvirtualByteMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallNonvirtualCharMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallNonvirtualCharMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallNonvirtualCharMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallNonvirtualShortMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallNonvirtualShortMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallNonvirtualShortMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallNonvirtualIntMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallNonvirtualIntMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallNonvirtualIntMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallNonvirtualLongMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallNonvirtualLongMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallNonvirtualLongMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallNonvirtualFloatMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallNonvirtualFloatMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallNonvirtualFloatMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallNonvirtualDoubleMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallNonvirtualDoubleMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallNonvirtualDoubleMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

void CallNonvirtualVoidMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
}

void CallNonvirtualVoidMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
}

void CallNonvirtualVoidMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
  UNIMPLEMENTED(FATAL);
}

jfieldID GetFieldID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject GetObjectField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean GetBooleanField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jbyte GetByteField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar GetCharField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort GetShortField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint GetIntField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong GetLongField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat GetFloatField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble GetDoubleField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

void SetObjectField(JNIEnv* env, jobject obj, jfieldID fieldID, jobject val) {
  UNIMPLEMENTED(FATAL);
}

void SetBooleanField(JNIEnv* env, jobject obj, jfieldID fieldID, jboolean val) {
  UNIMPLEMENTED(FATAL);
}

void SetByteField(JNIEnv* env, jobject obj, jfieldID fieldID, jbyte val) {
  UNIMPLEMENTED(FATAL);
}

void SetCharField(JNIEnv* env, jobject obj, jfieldID fieldID, jchar val) {
  UNIMPLEMENTED(FATAL);
}

void SetShortField(JNIEnv* env, jobject obj, jfieldID fieldID, jshort val) {
  UNIMPLEMENTED(FATAL);
}

void SetIntField(JNIEnv* env, jobject obj, jfieldID fieldID, jint val) {
  UNIMPLEMENTED(FATAL);
}

void SetLongField(JNIEnv* env, jobject obj, jfieldID fieldID, jlong val) {
  UNIMPLEMENTED(FATAL);
}

void SetFloatField(JNIEnv* env, jobject obj, jfieldID fieldID, jfloat val) {
  UNIMPLEMENTED(FATAL);
}

void SetDoubleField(JNIEnv* env, jobject obj, jfieldID fieldID, jdouble val) {
  UNIMPLEMENTED(FATAL);
}

jmethodID GetStaticMethodID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallStaticObjectMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallStaticObjectMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallStaticObjectMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean CallStaticBooleanMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jboolean CallStaticBooleanMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jboolean CallStaticBooleanMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jbyte CallStaticByteMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jbyte CallStaticByteMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jbyte CallStaticByteMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallStaticCharMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallStaticCharMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallStaticCharMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallStaticShortMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallStaticShortMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallStaticShortMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallStaticIntMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallStaticIntMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallStaticIntMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallStaticLongMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallStaticLongMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallStaticLongMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallStaticFloatMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallStaticFloatMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallStaticFloatMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallStaticDoubleMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallStaticDoubleMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallStaticDoubleMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

void CallStaticVoidMethod(JNIEnv* env, jclass cls, jmethodID methodID, ...) {
  UNIMPLEMENTED(FATAL);
}

void CallStaticVoidMethodV(JNIEnv* env,
    jclass cls, jmethodID methodID, va_list args) {
  UNIMPLEMENTED(FATAL);
}

void CallStaticVoidMethodA(JNIEnv* env,
    jclass cls, jmethodID methodID, jvalue*  args) {
  UNIMPLEMENTED(FATAL);
}

jfieldID GetStaticFieldID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jobject GetStaticObjectField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean GetStaticBooleanField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jbyte GetStaticByteField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar GetStaticCharField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort GetStaticShortField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint GetStaticIntField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong GetStaticLongField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat GetStaticFloatField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble GetStaticDoubleField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

void SetStaticObjectField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jobject value) {
  UNIMPLEMENTED(FATAL);
}

void SetStaticBooleanField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jboolean value) {
  UNIMPLEMENTED(FATAL);
}

void SetStaticByteField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jbyte value) {
  UNIMPLEMENTED(FATAL);
}

void SetStaticCharField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jchar value) {
  UNIMPLEMENTED(FATAL);
}

void SetStaticShortField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jshort value) {
  UNIMPLEMENTED(FATAL);
}

void SetStaticIntField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jint value) {
  UNIMPLEMENTED(FATAL);
}

void SetStaticLongField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jlong value) {
  UNIMPLEMENTED(FATAL);
}

void SetStaticFloatField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jfloat value) {
  UNIMPLEMENTED(FATAL);
}

void SetStaticDoubleField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jdouble value) {
  UNIMPLEMENTED(FATAL);
}

jstring NewString(JNIEnv* env, const jchar* unicode, jsize len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jsize GetStringLength(JNIEnv* env, jstring str) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

const jchar* GetStringChars(JNIEnv* env, jstring str, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ReleaseStringChars(JNIEnv* env, jstring str, const jchar* chars) {
  UNIMPLEMENTED(FATAL);
}

jstring NewStringUTF(JNIEnv* env, const char* utf) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jsize GetStringUTFLength(JNIEnv* env, jstring str) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

const char* GetStringUTFChars(JNIEnv* env, jstring str, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ReleaseStringUTFChars(JNIEnv* env, jstring str, const char* chars) {
  UNIMPLEMENTED(FATAL);
}

jsize GetArrayLength(JNIEnv* env, jarray array) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jobjectArray NewObjectArray(JNIEnv* env,
    jsize len, jclass clazz, jobject init) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject GetObjectArrayElement(JNIEnv* env, jobjectArray array, jsize index) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void SetObjectArrayElement(JNIEnv* env,
    jobjectArray array, jsize index, jobject val) {
  UNIMPLEMENTED(FATAL);
}

jbooleanArray NewBooleanArray(JNIEnv* env, jsize len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jbyteArray NewByteArray(JNIEnv* env, jsize len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jcharArray NewCharArray(JNIEnv* env, jsize len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jshortArray NewShortArray(JNIEnv* env, jsize len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jintArray NewIntArray(JNIEnv* env, jsize len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jlongArray NewLongArray(JNIEnv* env, jsize len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jfloatArray NewFloatArray(JNIEnv* env, jsize len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jdoubleArray NewDoubleArray(JNIEnv* env, jsize len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean* GetBooleanArrayElements(JNIEnv* env,
    jbooleanArray array, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jbyte* GetByteArrayElements(JNIEnv* env, jbyteArray array, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jchar* GetCharArrayElements(JNIEnv* env, jcharArray array, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jshort* GetShortArrayElements(JNIEnv* env,
    jshortArray array, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jint* GetIntArrayElements(JNIEnv* env, jintArray array, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jlong* GetLongArrayElements(JNIEnv* env, jlongArray array, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jfloat* GetFloatArrayElements(JNIEnv* env,
    jfloatArray array, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jdouble* GetDoubleArrayElements(JNIEnv* env,
    jdoubleArray array, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ReleaseBooleanArrayElements(JNIEnv* env,
    jbooleanArray array, jboolean* elems, jint mode) {
  UNIMPLEMENTED(FATAL);
}

void ReleaseByteArrayElements(JNIEnv* env,
    jbyteArray array, jbyte* elems, jint mode) {
  UNIMPLEMENTED(FATAL);
}

void ReleaseCharArrayElements(JNIEnv* env,
    jcharArray array, jchar* elems, jint mode) {
  UNIMPLEMENTED(FATAL);
}

void ReleaseShortArrayElements(JNIEnv* env,
    jshortArray array, jshort* elems, jint mode) {
  UNIMPLEMENTED(FATAL);
}

void ReleaseIntArrayElements(JNIEnv* env,
    jintArray array, jint* elems, jint mode) {
  UNIMPLEMENTED(FATAL);
}

void ReleaseLongArrayElements(JNIEnv* env,
    jlongArray array, jlong* elems, jint mode) {
  UNIMPLEMENTED(FATAL);
}

void ReleaseFloatArrayElements(JNIEnv* env,
    jfloatArray array, jfloat* elems, jint mode) {
  UNIMPLEMENTED(FATAL);
}

void ReleaseDoubleArrayElements(JNIEnv* env,
    jdoubleArray array, jdouble* elems, jint mode) {
  UNIMPLEMENTED(FATAL);
}

void GetBooleanArrayRegion(JNIEnv* env,
    jbooleanArray array, jsize start, jsize l, jboolean* buf) {
  UNIMPLEMENTED(FATAL);
}

void GetByteArrayRegion(JNIEnv* env,
    jbyteArray array, jsize start, jsize len, jbyte* buf) {
  UNIMPLEMENTED(FATAL);
}

void GetCharArrayRegion(JNIEnv* env,
    jcharArray array, jsize start, jsize len, jchar* buf) {
  UNIMPLEMENTED(FATAL);
}

void GetShortArrayRegion(JNIEnv* env,
    jshortArray array, jsize start, jsize len, jshort* buf) {
  UNIMPLEMENTED(FATAL);
}

void GetIntArrayRegion(JNIEnv* env,
    jintArray array, jsize start, jsize len, jint* buf) {
  UNIMPLEMENTED(FATAL);
}

void GetLongArrayRegion(JNIEnv* env,
    jlongArray array, jsize start, jsize len, jlong* buf) {
  UNIMPLEMENTED(FATAL);
}

void GetFloatArrayRegion(JNIEnv* env,
    jfloatArray array, jsize start, jsize len, jfloat* buf) {
  UNIMPLEMENTED(FATAL);
}

void GetDoubleArrayRegion(JNIEnv* env,
    jdoubleArray array, jsize start, jsize len, jdouble* buf) {
  UNIMPLEMENTED(FATAL);
}

void SetBooleanArrayRegion(JNIEnv* env,
    jbooleanArray array, jsize start, jsize l, const jboolean* buf) {
  UNIMPLEMENTED(FATAL);
}

void SetByteArrayRegion(JNIEnv* env,
    jbyteArray array, jsize start, jsize len, const jbyte* buf) {
  UNIMPLEMENTED(FATAL);
}

void SetCharArrayRegion(JNIEnv* env,
    jcharArray array, jsize start, jsize len, const jchar* buf) {
  UNIMPLEMENTED(FATAL);
}

void SetShortArrayRegion(JNIEnv* env,
    jshortArray array, jsize start, jsize len, const jshort* buf) {
  UNIMPLEMENTED(FATAL);
}

void SetIntArrayRegion(JNIEnv* env,
    jintArray array, jsize start, jsize len, const jint* buf) {
  UNIMPLEMENTED(FATAL);
}

void SetLongArrayRegion(JNIEnv* env,
    jlongArray array, jsize start, jsize len, const jlong* buf) {
  UNIMPLEMENTED(FATAL);
}

void SetFloatArrayRegion(JNIEnv* env,
    jfloatArray array, jsize start, jsize len, const jfloat* buf) {
  UNIMPLEMENTED(FATAL);
}

void SetDoubleArrayRegion(JNIEnv* env,
    jdoubleArray array, jsize start, jsize len, const jdouble* buf) {
  UNIMPLEMENTED(FATAL);
}

jint RegisterNatives(JNIEnv* env,
    jclass clazz, const JNINativeMethod* methods, jint nMethods) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint UnregisterNatives(JNIEnv* env, jclass clazz) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint MonitorEnter(JNIEnv* env, jobject obj) {
  UNIMPLEMENTED(WARNING);
  return 0;
}

jint MonitorExit(JNIEnv* env, jobject obj) {
  UNIMPLEMENTED(WARNING);
  return 0;
}

jint GetJavaVM(JNIEnv* env, JavaVM* *vm) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

void GetStringRegion(JNIEnv* env,
    jstring str, jsize start, jsize len, jchar* buf) {
  UNIMPLEMENTED(FATAL);
}

void GetStringUTFRegion(JNIEnv* env,
    jstring str, jsize start, jsize len, char* buf) {
  UNIMPLEMENTED(FATAL);
}

void* GetPrimitiveArrayCritical(JNIEnv* env,
    jarray array, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ReleasePrimitiveArrayCritical(JNIEnv* env,
    jarray array, void* carray, jint mode) {
  UNIMPLEMENTED(FATAL);
}

const jchar* GetStringCritical(JNIEnv* env, jstring s, jboolean* isCopy) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ReleaseStringCritical(JNIEnv* env, jstring s, const jchar* cstr) {
  UNIMPLEMENTED(FATAL);
}

jweak NewWeakGlobalRef(JNIEnv* env, jobject obj) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void DeleteWeakGlobalRef(JNIEnv* env, jweak obj) {
  UNIMPLEMENTED(FATAL);
}

jboolean ExceptionCheck(JNIEnv* env) {
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jobject NewDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}


void* GetDirectBufferAddress(JNIEnv* env, jobject buf) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jlong GetDirectBufferCapacity(JNIEnv* env, jobject buf) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jobjectRefType GetObjectRefType(JNIEnv* env, jobject jobj) {
  UNIMPLEMENTED(FATAL);
  return JNIInvalidRefType;
}

static const struct JNINativeInterface gNativeInterface = {
  NULL,  // reserved0.
  NULL,  // reserved1.
  NULL,  // reserved2.
  NULL,  // reserved3.
  GetVersion,
  DefineClass,
  FindClass,
  FromReflectedMethod,
  FromReflectedField,
  ToReflectedMethod,
  GetSuperclass,
  IsAssignableFrom,
  ToReflectedField,
  Throw,
  ThrowNew,
  ExceptionOccurred,
  ExceptionDescribe,
  ExceptionClear,
  FatalError,
  PushLocalFrame,
  PopLocalFrame,
  NewGlobalRef,
  DeleteGlobalRef,
  DeleteLocalRef,
  IsSameObject,
  NewLocalRef,
  EnsureLocalCapacity,
  AllocObject,
  NewObject,
  NewObjectV,
  NewObjectA,
  GetObjectClass,
  IsInstanceOf,
  GetMethodID,
  CallObjectMethod,
  CallObjectMethodV,
  CallObjectMethodA,
  CallBooleanMethod,
  CallBooleanMethodV,
  CallBooleanMethodA,
  CallByteMethod,
  CallByteMethodV,
  CallByteMethodA,
  CallCharMethod,
  CallCharMethodV,
  CallCharMethodA,
  CallShortMethod,
  CallShortMethodV,
  CallShortMethodA,
  CallIntMethod,
  CallIntMethodV,
  CallIntMethodA,
  CallLongMethod,
  CallLongMethodV,
  CallLongMethodA,
  CallFloatMethod,
  CallFloatMethodV,
  CallFloatMethodA,
  CallDoubleMethod,
  CallDoubleMethodV,
  CallDoubleMethodA,
  CallVoidMethod,
  CallVoidMethodV,
  CallVoidMethodA,
  CallNonvirtualObjectMethod,
  CallNonvirtualObjectMethodV,
  CallNonvirtualObjectMethodA,
  CallNonvirtualBooleanMethod,
  CallNonvirtualBooleanMethodV,
  CallNonvirtualBooleanMethodA,
  CallNonvirtualByteMethod,
  CallNonvirtualByteMethodV,
  CallNonvirtualByteMethodA,
  CallNonvirtualCharMethod,
  CallNonvirtualCharMethodV,
  CallNonvirtualCharMethodA,
  CallNonvirtualShortMethod,
  CallNonvirtualShortMethodV,
  CallNonvirtualShortMethodA,
  CallNonvirtualIntMethod,
  CallNonvirtualIntMethodV,
  CallNonvirtualIntMethodA,
  CallNonvirtualLongMethod,
  CallNonvirtualLongMethodV,
  CallNonvirtualLongMethodA,
  CallNonvirtualFloatMethod,
  CallNonvirtualFloatMethodV,
  CallNonvirtualFloatMethodA,
  CallNonvirtualDoubleMethod,
  CallNonvirtualDoubleMethodV,
  CallNonvirtualDoubleMethodA,
  CallNonvirtualVoidMethod,
  CallNonvirtualVoidMethodV,
  CallNonvirtualVoidMethodA,
  GetFieldID,
  GetObjectField,
  GetBooleanField,
  GetByteField,
  GetCharField,
  GetShortField,
  GetIntField,
  GetLongField,
  GetFloatField,
  GetDoubleField,
  SetObjectField,
  SetBooleanField,
  SetByteField,
  SetCharField,
  SetShortField,
  SetIntField,
  SetLongField,
  SetFloatField,
  SetDoubleField,
  GetStaticMethodID,
  CallStaticObjectMethod,
  CallStaticObjectMethodV,
  CallStaticObjectMethodA,
  CallStaticBooleanMethod,
  CallStaticBooleanMethodV,
  CallStaticBooleanMethodA,
  CallStaticByteMethod,
  CallStaticByteMethodV,
  CallStaticByteMethodA,
  CallStaticCharMethod,
  CallStaticCharMethodV,
  CallStaticCharMethodA,
  CallStaticShortMethod,
  CallStaticShortMethodV,
  CallStaticShortMethodA,
  CallStaticIntMethod,
  CallStaticIntMethodV,
  CallStaticIntMethodA,
  CallStaticLongMethod,
  CallStaticLongMethodV,
  CallStaticLongMethodA,
  CallStaticFloatMethod,
  CallStaticFloatMethodV,
  CallStaticFloatMethodA,
  CallStaticDoubleMethod,
  CallStaticDoubleMethodV,
  CallStaticDoubleMethodA,
  CallStaticVoidMethod,
  CallStaticVoidMethodV,
  CallStaticVoidMethodA,
  GetStaticFieldID,
  GetStaticObjectField,
  GetStaticBooleanField,
  GetStaticByteField,
  GetStaticCharField,
  GetStaticShortField,
  GetStaticIntField,
  GetStaticLongField,
  GetStaticFloatField,
  GetStaticDoubleField,
  SetStaticObjectField,
  SetStaticBooleanField,
  SetStaticByteField,
  SetStaticCharField,
  SetStaticShortField,
  SetStaticIntField,
  SetStaticLongField,
  SetStaticFloatField,
  SetStaticDoubleField,
  NewString,
  GetStringLength,
  GetStringChars,
  ReleaseStringChars,
  NewStringUTF,
  GetStringUTFLength,
  GetStringUTFChars,
  ReleaseStringUTFChars,
  GetArrayLength,
  NewObjectArray,
  GetObjectArrayElement,
  SetObjectArrayElement,
  NewBooleanArray,
  NewByteArray,
  NewCharArray,
  NewShortArray,
  NewIntArray,
  NewLongArray,
  NewFloatArray,
  NewDoubleArray,
  GetBooleanArrayElements,
  GetByteArrayElements,
  GetCharArrayElements,
  GetShortArrayElements,
  GetIntArrayElements,
  GetLongArrayElements,
  GetFloatArrayElements,
  GetDoubleArrayElements,
  ReleaseBooleanArrayElements,
  ReleaseByteArrayElements,
  ReleaseCharArrayElements,
  ReleaseShortArrayElements,
  ReleaseIntArrayElements,
  ReleaseLongArrayElements,
  ReleaseFloatArrayElements,
  ReleaseDoubleArrayElements,
  GetBooleanArrayRegion,
  GetByteArrayRegion,
  GetCharArrayRegion,
  GetShortArrayRegion,
  GetIntArrayRegion,
  GetLongArrayRegion,
  GetFloatArrayRegion,
  GetDoubleArrayRegion,
  SetBooleanArrayRegion,
  SetByteArrayRegion,
  SetCharArrayRegion,
  SetShortArrayRegion,
  SetIntArrayRegion,
  SetLongArrayRegion,
  SetFloatArrayRegion,
  SetDoubleArrayRegion,
  RegisterNatives,
  UnregisterNatives,
  MonitorEnter,
  MonitorExit,
  GetJavaVM,
  GetStringRegion,
  GetStringUTFRegion,
  GetPrimitiveArrayCritical,
  ReleasePrimitiveArrayCritical,
  GetStringCritical,
  ReleaseStringCritical,
  NewWeakGlobalRef,
  DeleteWeakGlobalRef,
  ExceptionCheck,
  NewDirectByteBuffer,
  GetDirectBufferAddress,
  GetDirectBufferCapacity,
  GetObjectRefType,
};

void MonitorEnterHelper(JNIEnv* env, jobject obj) {
  MonitorEnter(env, obj);  // Ignore the result.
}

void MonitorExitHelper(JNIEnv* env, jobject obj) {
  MonitorExit(env, obj);  // Ignore the result.
}

JNIEnv* CreateJNIEnv() {
  JNIEnvExt* result = (JNIEnvExt*) calloc(1, sizeof(JNIEnvExt));
  result->fns = &gNativeInterface;
  result->self = Thread::Current();
  result->MonitorEnterHelper = &MonitorEnterHelper;
  result->MonitorExitHelper = &MonitorExitHelper;
  return reinterpret_cast<JNIEnv*>(result);
}

// JNI Invocation interface.

extern "C" jint JNI_CreateJavaVM(JavaVM** p_vm, void** p_env, void* vm_args) {
  const JavaVMInitArgs* args = static_cast<JavaVMInitArgs*>(vm_args);
  if (args->version < JNI_VERSION_1_2) {
    return JNI_EVERSION;
  }
  Runtime::Options options;
  for (int i = 0; i < args->nOptions; ++i) {
    JavaVMOption* option = &args->options[i];
    options.push_back(std::make_pair(StringPiece(option->optionString),
                                     option->extraInfo));
  }
  bool ignore_unrecognized = args->ignoreUnrecognized;
  scoped_ptr<Runtime> runtime(Runtime::Create(options, ignore_unrecognized));
  if (runtime == NULL) {
    return JNI_ERR;
  } else {
    *p_env = reinterpret_cast<JNIEnv*>(Thread::Current()->GetJniEnv());
    *p_vm = reinterpret_cast<JavaVM*>(runtime.release());
    return JNI_OK;
  }
}

extern "C" jint JNI_GetCreatedJavaVMs(JavaVM** vmBuf, jsize bufLen,
                                      jsize* nVMs) {
  Runtime* runtime = Runtime::Current();
  if (runtime == NULL) {
    *nVMs = 0;
  } else {
    *nVMs = 1;
    vmBuf[0] = reinterpret_cast<JavaVM*>(runtime);
  }
  return JNI_OK;
}

// Historically unsupported.
extern "C" jint JNI_GetDefaultJavaVMInitArgs(void* vm_args) {
  return JNI_ERR;
}

jint JniInvokeInterface::DestroyJavaVM(JavaVM* vm) {
  if (vm == NULL) {
    return JNI_ERR;
  } else {
    Runtime* runtime = reinterpret_cast<Runtime*>(vm);
    delete runtime;
    return JNI_OK;
  }
}

jint JniInvokeInterface::AttachCurrentThread(JavaVM* vm,
                                             JNIEnv** p_env,
                                             void* thr_args) {
  if (vm == NULL || p_env == NULL) {
    return JNI_ERR;
  }
  Runtime* runtime = reinterpret_cast<Runtime*>(vm);
  const char* name = NULL;
  if (thr_args != NULL) {
    // TODO: check version
    name = static_cast<JavaVMAttachArgs*>(thr_args)->name;
    // TODO: thread group
  }
  bool success = runtime->AttachCurrentThread(name, p_env);
  if (!success) {
    return JNI_ERR;
  } else {
    return JNI_OK;
  }
}

jint JniInvokeInterface::DetachCurrentThread(JavaVM* vm) {
  if (vm == NULL) {
    return JNI_ERR;
  } else {
    Runtime* runtime = reinterpret_cast<Runtime*>(vm);
    runtime->DetachCurrentThread();
    return JNI_OK;
  }
}

jint JniInvokeInterface::GetEnv(JavaVM *vm, void **env, jint version) {
  if (version < JNI_VERSION_1_1 || version > JNI_VERSION_1_6) {
    return JNI_EVERSION;
  }
  if (vm == NULL || env == NULL) {
    return JNI_ERR;
  }
  Thread* thread = Thread::Current();
  if (thread == NULL) {
    *env = NULL;
    return JNI_EDETACHED;
  }
  *env = thread->GetJniEnv();
  return JNI_OK;
}

jint JniInvokeInterface::AttachCurrentThreadAsDaemon(JavaVM* vm,
                                                     JNIEnv** p_env,
                                                     void* thr_args) {
  if (vm == NULL || p_env == NULL) {
    return JNI_ERR;
  }
  Runtime* runtime = reinterpret_cast<Runtime*>(vm);
  const char* name = NULL;
  if (thr_args != NULL) {
    // TODO: check version
    name = static_cast<JavaVMAttachArgs*>(thr_args)->name;
    // TODO: thread group
  }
  bool success = runtime->AttachCurrentThreadAsDaemon(name, p_env);
  if (!success) {
    return JNI_ERR;
  } else {
    return JNI_OK;
  }
}

struct JNIInvokeInterface JniInvokeInterface::invoke_interface_ = {
  NULL,  // reserved0
  NULL,  // reserved1
  NULL,  // reserved2
  DestroyJavaVM,
  AttachCurrentThread,
  DetachCurrentThread,
  GetEnv,
  AttachCurrentThreadAsDaemon
};

}  // namespace art
