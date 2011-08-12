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

namespace art {

jint GetVersion(JNIEnv* env) {
  return JNI_VERSION_1_6;
}

jclass DefineClass(JNIEnv *env, const char *name,
    jobject loader, const jbyte *buf, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jclass FindClass(JNIEnv* env, const char* name) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  // TODO: need to get the appropriate ClassLoader.
  Class* c = class_linker->FindClass(name, NULL);
  // TODO: local reference.
  return reinterpret_cast<jclass>(c);
}

jmethodID FromReflectedMethod(JNIEnv* env, jobject method) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jfieldID FromReflectedField(JNIEnv* env, jobject field) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject ToReflectedMethod(JNIEnv* env, jclass cls,
    jmethodID methodID, jboolean isStatic) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jclass GetSuperclass(JNIEnv* env, jclass sub) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean IsAssignableFrom(JNIEnv* env, jclass sub, jclass sup) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jobject ToReflectedField(JNIEnv* env, jclass cls,
    jfieldID fieldID, jboolean isStatic) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jint Throw(JNIEnv* env, jthrowable obj) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint ThrowNew(JNIEnv* env, jclass clazz, const char* msg) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jthrowable ExceptionOccurred(JNIEnv* env) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void ExceptionDescribe(JNIEnv* env) {
  LOG(FATAL) << "Unimplemented";
}

void ExceptionClear(JNIEnv* env) {
  LOG(FATAL) << "Unimplemented";
}

void FatalError(JNIEnv* env, const char* msg) {
  LOG(FATAL) << "Unimplemented";
}

jint PushLocalFrame(JNIEnv* env, jint cap) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jobject PopLocalFrame(JNIEnv* env, jobject res) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject NewGlobalRef(JNIEnv* env, jobject lobj) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void DeleteGlobalRef(JNIEnv* env, jobject gref) {
  LOG(FATAL) << "Unimplemented";
}

void DeleteLocalRef(JNIEnv* env, jobject obj) {
  LOG(FATAL) << "Unimplemented";
}

jboolean IsSameObject(JNIEnv* env, jobject obj1, jobject obj2) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jobject NewLocalRef(JNIEnv* env, jobject ref) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jint EnsureLocalCapacity(JNIEnv* env, jint) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jobject AllocObject(JNIEnv* env, jclass clazz) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject NewObject(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject NewObjectV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject NewObjectA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jclass GetObjectClass(JNIEnv* env, jobject obj) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean IsInstanceOf(JNIEnv* env, jobject obj, jclass clazz) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jmethodID GetMethodID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject CallObjectMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject CallObjectMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject CallObjectMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean CallBooleanMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean CallBooleanMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean CallBooleanMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jbyte CallByteMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte CallByteMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte CallByteMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar CallCharMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar CallCharMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar CallCharMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort CallShortMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort CallShortMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort CallShortMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint CallIntMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint CallIntMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint CallIntMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong CallLongMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong CallLongMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong CallLongMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat CallFloatMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat CallFloatMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat CallFloatMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble CallDoubleMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble CallDoubleMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble CallDoubleMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void CallVoidMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
}

void CallVoidMethodV(JNIEnv* env, jobject obj,
    jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
}

void CallVoidMethodA(JNIEnv* env, jobject obj,
    jmethodID methodID, jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
}

jobject CallNonvirtualObjectMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject CallNonvirtualObjectMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject CallNonvirtualObjectMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean CallNonvirtualBooleanMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean CallNonvirtualBooleanMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean CallNonvirtualBooleanMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jbyte CallNonvirtualByteMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte CallNonvirtualByteMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte CallNonvirtualByteMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar CallNonvirtualCharMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar CallNonvirtualCharMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar CallNonvirtualCharMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort CallNonvirtualShortMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort CallNonvirtualShortMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort CallNonvirtualShortMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint CallNonvirtualIntMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint CallNonvirtualIntMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint CallNonvirtualIntMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong CallNonvirtualLongMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong CallNonvirtualLongMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong CallNonvirtualLongMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat CallNonvirtualFloatMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat CallNonvirtualFloatMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat CallNonvirtualFloatMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble CallNonvirtualDoubleMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble CallNonvirtualDoubleMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble CallNonvirtualDoubleMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void CallNonvirtualVoidMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
}

void CallNonvirtualVoidMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
}

void CallNonvirtualVoidMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
}

jfieldID GetFieldID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject GetObjectField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean GetBooleanField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jbyte GetByteField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar GetCharField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort GetShortField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint GetIntField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong GetLongField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat GetFloatField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble GetDoubleField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void SetObjectField(JNIEnv* env, jobject obj, jfieldID fieldID, jobject val) {
  LOG(FATAL) << "Unimplemented";
}

void SetBooleanField(JNIEnv* env, jobject obj, jfieldID fieldID, jboolean val) {
  LOG(FATAL) << "Unimplemented";
}

void SetByteField(JNIEnv* env, jobject obj, jfieldID fieldID, jbyte val) {
  LOG(FATAL) << "Unimplemented";
}

void SetCharField(JNIEnv* env, jobject obj, jfieldID fieldID, jchar val) {
  LOG(FATAL) << "Unimplemented";
}

void SetShortField(JNIEnv* env, jobject obj, jfieldID fieldID, jshort val) {
  LOG(FATAL) << "Unimplemented";
}

void SetIntField(JNIEnv* env, jobject obj, jfieldID fieldID, jint val) {
  LOG(FATAL) << "Unimplemented";
}

void SetLongField(JNIEnv* env, jobject obj, jfieldID fieldID, jlong val) {
  LOG(FATAL) << "Unimplemented";
}

void SetFloatField(JNIEnv* env, jobject obj, jfieldID fieldID, jfloat val) {
  LOG(FATAL) << "Unimplemented";
}

void SetDoubleField(JNIEnv* env, jobject obj, jfieldID fieldID, jdouble val) {
  LOG(FATAL) << "Unimplemented";
}

jmethodID GetStaticMethodID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject CallStaticObjectMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject CallStaticObjectMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject CallStaticObjectMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean CallStaticBooleanMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean CallStaticBooleanMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean CallStaticBooleanMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jbyte CallStaticByteMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte CallStaticByteMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte CallStaticByteMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar CallStaticCharMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar CallStaticCharMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar CallStaticCharMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort CallStaticShortMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort CallStaticShortMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort CallStaticShortMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint CallStaticIntMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint CallStaticIntMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint CallStaticIntMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong CallStaticLongMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong CallStaticLongMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong CallStaticLongMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat CallStaticFloatMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat CallStaticFloatMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat CallStaticFloatMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble CallStaticDoubleMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble CallStaticDoubleMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble CallStaticDoubleMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void CallStaticVoidMethod(JNIEnv* env, jclass cls, jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
}

void CallStaticVoidMethodV(JNIEnv* env,
    jclass cls, jmethodID methodID, va_list args) {
  LOG(FATAL) << "Unimplemented";
}

void CallStaticVoidMethodA(JNIEnv* env,
    jclass cls, jmethodID methodID, jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
}

jfieldID GetStaticFieldID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jobject GetStaticObjectField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean GetStaticBooleanField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jbyte GetStaticByteField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar GetStaticCharField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort GetStaticShortField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint GetStaticIntField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong GetStaticLongField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat GetStaticFloatField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble GetStaticDoubleField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void SetStaticObjectField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jobject value) {
  LOG(FATAL) << "Unimplemented";
}

void SetStaticBooleanField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jboolean value) {
  LOG(FATAL) << "Unimplemented";
}

void SetStaticByteField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jbyte value) {
  LOG(FATAL) << "Unimplemented";
}

void SetStaticCharField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jchar value) {
  LOG(FATAL) << "Unimplemented";
}

void SetStaticShortField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jshort value) {
  LOG(FATAL) << "Unimplemented";
}

void SetStaticIntField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jint value) {
  LOG(FATAL) << "Unimplemented";
}

void SetStaticLongField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jlong value) {
  LOG(FATAL) << "Unimplemented";
}

void SetStaticFloatField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jfloat value) {
  LOG(FATAL) << "Unimplemented";
}

void SetStaticDoubleField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jdouble value) {
  LOG(FATAL) << "Unimplemented";
}

jstring NewString(JNIEnv* env, const jchar* unicode, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jsize GetStringLength(JNIEnv* env, jstring str) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

const jchar* GetStringChars(JNIEnv* env, jstring str, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void ReleaseStringChars(JNIEnv* env, jstring str, const jchar* chars) {
  LOG(FATAL) << "Unimplemented";
}

jstring NewStringUTF(JNIEnv* env, const char* utf) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jsize GetStringUTFLength(JNIEnv* env, jstring str) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

const char* GetStringUTFChars(JNIEnv* env, jstring str, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void ReleaseStringUTFChars(JNIEnv* env, jstring str, const char* chars) {
  LOG(FATAL) << "Unimplemented";
}

jsize GetArrayLength(JNIEnv* env, jarray array) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jobjectArray NewObjectArray(JNIEnv* env,
    jsize len, jclass clazz, jobject init) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject GetObjectArrayElement(JNIEnv* env, jobjectArray array, jsize index) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void SetObjectArrayElement(JNIEnv* env,
    jobjectArray array, jsize index, jobject val) {
  LOG(FATAL) << "Unimplemented";
}

jbooleanArray NewBooleanArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jbyteArray NewByteArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jcharArray NewCharArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jshortArray NewShortArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jintArray NewIntArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jlongArray NewLongArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jfloatArray NewFloatArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jdoubleArray NewDoubleArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean* GetBooleanArrayElements(JNIEnv* env,
    jbooleanArray array, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jbyte* GetByteArrayElements(JNIEnv* env, jbyteArray array, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jchar* GetCharArrayElements(JNIEnv* env, jcharArray array, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jshort* GetShortArrayElements(JNIEnv* env,
    jshortArray array, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jint* GetIntArrayElements(JNIEnv* env, jintArray array, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jlong* GetLongArrayElements(JNIEnv* env, jlongArray array, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jfloat* GetFloatArrayElements(JNIEnv* env,
    jfloatArray array, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jdouble* GetDoubleArrayElements(JNIEnv* env,
    jdoubleArray array, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void ReleaseBooleanArrayElements(JNIEnv* env,
    jbooleanArray array, jboolean* elems, jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void ReleaseByteArrayElements(JNIEnv* env,
    jbyteArray array, jbyte* elems, jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void ReleaseCharArrayElements(JNIEnv* env,
    jcharArray array, jchar* elems, jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void ReleaseShortArrayElements(JNIEnv* env,
    jshortArray array, jshort* elems, jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void ReleaseIntArrayElements(JNIEnv* env,
    jintArray array, jint* elems, jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void ReleaseLongArrayElements(JNIEnv* env,
    jlongArray array, jlong* elems, jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void ReleaseFloatArrayElements(JNIEnv* env,
    jfloatArray array, jfloat* elems, jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void ReleaseDoubleArrayElements(JNIEnv* env,
    jdoubleArray array, jdouble* elems, jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void GetBooleanArrayRegion(JNIEnv* env,
    jbooleanArray array, jsize start, jsize l, jboolean* buf) {
  LOG(FATAL) << "Unimplemented";
}

void GetByteArrayRegion(JNIEnv* env,
    jbyteArray array, jsize start, jsize len, jbyte* buf) {
  LOG(FATAL) << "Unimplemented";
}

void GetCharArrayRegion(JNIEnv* env,
    jcharArray array, jsize start, jsize len, jchar* buf) {
  LOG(FATAL) << "Unimplemented";
}

void GetShortArrayRegion(JNIEnv* env,
    jshortArray array, jsize start, jsize len, jshort* buf) {
  LOG(FATAL) << "Unimplemented";
}

void GetIntArrayRegion(JNIEnv* env,
    jintArray array, jsize start, jsize len, jint* buf) {
  LOG(FATAL) << "Unimplemented";
}

void GetLongArrayRegion(JNIEnv* env,
    jlongArray array, jsize start, jsize len, jlong* buf) {
  LOG(FATAL) << "Unimplemented";
}

void GetFloatArrayRegion(JNIEnv* env,
    jfloatArray array, jsize start, jsize len, jfloat* buf) {
  LOG(FATAL) << "Unimplemented";
}

void GetDoubleArrayRegion(JNIEnv* env,
    jdoubleArray array, jsize start, jsize len, jdouble* buf) {
  LOG(FATAL) << "Unimplemented";
}

void SetBooleanArrayRegion(JNIEnv* env,
    jbooleanArray array, jsize start, jsize l, const jboolean* buf) {
  LOG(FATAL) << "Unimplemented";
}

void SetByteArrayRegion(JNIEnv* env,
    jbyteArray array, jsize start, jsize len, const jbyte* buf) {
  LOG(FATAL) << "Unimplemented";
}

void SetCharArrayRegion(JNIEnv* env,
    jcharArray array, jsize start, jsize len, const jchar* buf) {
  LOG(FATAL) << "Unimplemented";
}

void SetShortArrayRegion(JNIEnv* env,
    jshortArray array, jsize start, jsize len, const jshort* buf) {
  LOG(FATAL) << "Unimplemented";
}

void SetIntArrayRegion(JNIEnv* env,
    jintArray array, jsize start, jsize len, const jint* buf) {
  LOG(FATAL) << "Unimplemented";
}

void SetLongArrayRegion(JNIEnv* env,
    jlongArray array, jsize start, jsize len, const jlong* buf) {
  LOG(FATAL) << "Unimplemented";
}

void SetFloatArrayRegion(JNIEnv* env,
    jfloatArray array, jsize start, jsize len, const jfloat* buf) {
  LOG(FATAL) << "Unimplemented";
}

void SetDoubleArrayRegion(JNIEnv* env,
    jdoubleArray array, jsize start, jsize len, const jdouble* buf) {
  LOG(FATAL) << "Unimplemented";
}

jint RegisterNatives(JNIEnv* env,
    jclass clazz, const JNINativeMethod* methods, jint nMethods) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint UnregisterNatives(JNIEnv* env, jclass clazz) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint MonitorEnter(JNIEnv* env, jobject obj) {
  LOG(WARNING) << "MonitorEnter unimplemented";
  return 0;
}

jint MonitorExit(JNIEnv* env, jobject obj) {
  LOG(WARNING) << "MonitorExit unimplemented";
  return 0;
}

jint GetJavaVM(JNIEnv* env, JavaVM* *vm) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void GetStringRegion(JNIEnv* env,
    jstring str, jsize start, jsize len, jchar* buf) {
  LOG(FATAL) << "Unimplemented";
}

void GetStringUTFRegion(JNIEnv* env,
    jstring str, jsize start, jsize len, char* buf) {
  LOG(FATAL) << "Unimplemented";
}

void* GetPrimitiveArrayCritical(JNIEnv* env,
    jarray array, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void ReleasePrimitiveArrayCritical(JNIEnv* env,
    jarray array, void* carray, jint mode) {
  LOG(FATAL) << "Unimplemented";
}

const jchar* GetStringCritical(JNIEnv* env, jstring s, jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void ReleaseStringCritical(JNIEnv* env, jstring s, const jchar* cstr) {
  LOG(FATAL) << "Unimplemented";
}

jweak NewWeakGlobalRef(JNIEnv* env, jobject obj) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void DeleteWeakGlobalRef(JNIEnv* env, jweak obj) {
  LOG(FATAL) << "Unimplemented";
}

jboolean ExceptionCheck(JNIEnv* env) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jobject NewDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}


void* GetDirectBufferAddress(JNIEnv* env, jobject buf) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jlong GetDirectBufferCapacity(JNIEnv* env, jobject buf) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jobjectRefType GetObjectRefType(JNIEnv* env, jobject jobj) {
  LOG(FATAL) << "Unimplemented";
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
