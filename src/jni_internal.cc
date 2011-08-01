// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include <vector>
#include <utility>

#include "jni.h"
#include "logging.h"
#include "runtime.h"
#include "scoped_ptr.h"
#include "stringpiece.h"
#include "thread.h"

namespace art {

static void JniMonitorEnter(JniEnvironment*, jobject) {
  LOG(WARNING) << "Unimplemented: JNI Monitor Enter";
}

static void JniMonitorExit(JniEnvironment*, jobject) {
  LOG(WARNING) << "Unimplemented: JNI Monitor Exit";
}

JniEnvironment::JniEnvironment() {
  monitor_enter_ = &JniMonitorEnter;
  monitor_exit_ = &JniMonitorExit;
}

// JNI Native interface.

jint JniNativeInterface::GetVersion(JNIEnv* env) {
  return JNI_VERSION_1_6;
}

jclass JniNativeInterface::DefineClass(JNIEnv *env,
                                       const char *name,
                                       jobject loader,
                                       const jbyte *buf,
                                       jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jclass JniNativeInterface::FindClass(JNIEnv *env, const char *name) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jmethodID JniNativeInterface::FromReflectedMethod(JNIEnv* env, jobject method) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jfieldID JniNativeInterface::FromReflectedField(JNIEnv* env, jobject field) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::ToReflectedMethod(JNIEnv* env,
                                              jclass cls,
                                              jmethodID methodID,
                                              jboolean isStatic) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jclass JniNativeInterface::GetSuperclass(JNIEnv* env, jclass sub) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean JniNativeInterface::IsAssignableFrom(JNIEnv* env,
                                              jclass sub,
                                              jclass sup) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jobject JniNativeInterface::ToReflectedField(JNIEnv* env,
                                             jclass cls,
                                             jfieldID fieldID,
                                             jboolean isStatic) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jint JniNativeInterface::Throw(JNIEnv* env, jthrowable obj) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::ThrowNew(JNIEnv* env, jclass clazz, const char* msg) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jthrowable JniNativeInterface::ExceptionOccurred(JNIEnv* env) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void JniNativeInterface::ExceptionDescribe(JNIEnv* env) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::ExceptionClear(JNIEnv* env) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::FatalError(JNIEnv* env, const char* msg) {
  LOG(FATAL) << "Unimplemented";
}

jint JniNativeInterface::PushLocalFrame(JNIEnv* env, jint cap) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jobject JniNativeInterface::PopLocalFrame(JNIEnv* env, jobject res) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::NewGlobalRef(JNIEnv* env, jobject lobj) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void JniNativeInterface::DeleteGlobalRef(JNIEnv* env, jobject gref) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::DeleteLocalRef(JNIEnv* env, jobject obj) {
  LOG(FATAL) << "Unimplemented";
}

jboolean JniNativeInterface::IsSameObject(JNIEnv* env,
                                          jobject obj1,
                                          jobject obj2) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jobject JniNativeInterface::NewLocalRef(JNIEnv* env, jobject ref) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jint JniNativeInterface::EnsureLocalCapacity(JNIEnv* env, jint) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jobject JniNativeInterface::AllocObject(JNIEnv* env, jclass clazz) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::NewObject(JNIEnv* env,
                                      jclass clazz,
                                      jmethodID methodID,
                                      ...) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::NewObjectV(JNIEnv* env,
                                       jclass clazz,
                                       jmethodID methodID,
                                       va_list args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::NewObjectA(JNIEnv* env,
                                       jclass clazz,
                                       jmethodID methodID,
                                       jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jclass JniNativeInterface::GetObjectClass(JNIEnv* env, jobject obj) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean JniNativeInterface::IsInstanceOf(JNIEnv* env,
                                          jobject obj,
                                          jclass clazz) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jmethodID JniNativeInterface::GetMethodID(JNIEnv* env,
                                          jclass clazz,
                                          const char* name,
                                          const char* sig) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::CallObjectMethod(JNIEnv* env,
                                             jobject obj,
                                             jmethodID methodID,
                                             ...) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::CallObjectMethodV(JNIEnv* env,
                                              jobject obj,
                                              jmethodID methodID,
                                              va_list args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::CallObjectMethodA(JNIEnv* env,
                                              jobject obj,
                                              jmethodID methodID,
                                              jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean JniNativeInterface::CallBooleanMethod(JNIEnv* env,
                                               jobject obj,
                                               jmethodID methodID,
                                               ...) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean JniNativeInterface::CallBooleanMethodV(JNIEnv* env,
                                                jobject obj,
                                                jmethodID methodID,
                                                va_list args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean JniNativeInterface::CallBooleanMethodA(JNIEnv* env,
                                                jobject obj,
                                                jmethodID methodID,
                                                jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jbyte JniNativeInterface::CallByteMethod(JNIEnv* env,
                                         jobject obj,
                                         jmethodID methodID,
                                         ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte JniNativeInterface::CallByteMethodV(JNIEnv* env,
                                          jobject obj,
                                          jmethodID methodID,
                                          va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte JniNativeInterface::CallByteMethodA(JNIEnv* env,
                                          jobject obj,
                                          jmethodID methodID,
                                          jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::CallCharMethod(JNIEnv* env,
                                         jobject obj,
                                         jmethodID methodID, ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::CallCharMethodV(JNIEnv* env,
                                          jobject obj,
                                          jmethodID methodID,
                                          va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::CallCharMethodA(JNIEnv* env,
                                          jobject obj,
                                          jmethodID methodID,
                                          jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::CallShortMethod(JNIEnv* env,
                                           jobject obj,
                                           jmethodID methodID,
                                           ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::CallShortMethodV(JNIEnv* env,
                                            jobject obj,
                                            jmethodID methodID,
                                            va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::CallShortMethodA(JNIEnv* env,
                                            jobject obj,
                                            jmethodID methodID,
                                            jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::CallIntMethod(JNIEnv* env,
                                       jobject obj,
                                       jmethodID methodID,
                                       ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::CallIntMethodV(JNIEnv* env,
                                        jobject obj,
                                        jmethodID methodID,
                                        va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::CallIntMethodA(JNIEnv* env,
                                        jobject obj,
                                        jmethodID methodID,
                                        jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::CallLongMethod(JNIEnv* env,
                                         jobject obj,
                                         jmethodID methodID,
                                         ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::CallLongMethodV(JNIEnv* env,
                                          jobject obj,
                                          jmethodID methodID,
                                          va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::CallLongMethodA(JNIEnv* env,
                                          jobject obj,
                                          jmethodID methodID,
                                          jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::CallFloatMethod(JNIEnv* env,
                                           jobject obj,
                                           jmethodID methodID,
                                           ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::CallFloatMethodV(JNIEnv* env,
                                            jobject obj,
                                            jmethodID methodID,
                                            va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::CallFloatMethodA(JNIEnv* env,
                                            jobject obj,
                                            jmethodID methodID,
                                            jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::CallDoubleMethod(JNIEnv* env,
                                             jobject obj,
                                             jmethodID methodID,
                                             ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::CallDoubleMethodV(JNIEnv* env,
                                              jobject obj,
                                              jmethodID methodID,
                                              va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::CallDoubleMethodA(JNIEnv* env,
                                              jobject obj,
                                              jmethodID methodID,
                                              jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void JniNativeInterface::CallVoidMethod(JNIEnv* env,
                                        jobject obj,
                                        jmethodID methodID,
                                        ...) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::CallVoidMethodV(JNIEnv* env,
                                         jobject obj,
                                         jmethodID methodID,
                                         va_list args) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::CallVoidMethodA(JNIEnv* env,
                                         jobject obj,
                                         jmethodID methodID,
                                         jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
}

jobject JniNativeInterface::CallNonvirtualObjectMethod(JNIEnv* env,
                                                       jobject obj,
                                                       jclass clazz,
                                                       jmethodID methodID,
                                                       ...) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::CallNonvirtualObjectMethodV(JNIEnv* env,
                                                        jobject obj,
                                                        jclass clazz,
                                                        jmethodID methodID,
                                                        va_list args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::CallNonvirtualObjectMethodA(JNIEnv* env,
                                                        jobject obj,
                                                        jclass clazz,
                                                        jmethodID methodID,
                                                        jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean JniNativeInterface::CallNonvirtualBooleanMethod(JNIEnv* env,
                                                         jobject obj,
                                                         jclass clazz,
                                                         jmethodID methodID,
                                                         ...) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean JniNativeInterface::CallNonvirtualBooleanMethodV(JNIEnv* env,
                                                          jobject obj,
                                                          jclass clazz,
                                                          jmethodID methodID,
                                                          va_list args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean JniNativeInterface::CallNonvirtualBooleanMethodA(JNIEnv* env,
                                                          jobject obj,
                                                          jclass clazz,
                                                          jmethodID methodID,
                                                          jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jbyte JniNativeInterface::CallNonvirtualByteMethod(JNIEnv* env,
                                                   jobject obj,
                                                   jclass clazz,
                                                   jmethodID methodID,
                                                   ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte JniNativeInterface::CallNonvirtualByteMethodV(JNIEnv* env,
                                                    jobject obj,
                                                    jclass clazz,
                                                    jmethodID methodID,
                                                    va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte JniNativeInterface::CallNonvirtualByteMethodA(JNIEnv* env,
                                                    jobject obj,
                                                    jclass clazz,
                                                    jmethodID methodID,
                                                    jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::CallNonvirtualCharMethod(JNIEnv* env,
                                                   jobject obj,
                                                   jclass clazz,
                                                   jmethodID methodID,
                                                   ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::CallNonvirtualCharMethodV(JNIEnv* env,
                                                    jobject obj,
                                                    jclass clazz,
                                                    jmethodID methodID,
                                                    va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::CallNonvirtualCharMethodA(JNIEnv* env,
                                                    jobject obj,
                                                    jclass clazz,
                                                    jmethodID methodID,
                                                    jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::CallNonvirtualShortMethod(JNIEnv* env,
                                                     jobject obj,
                                                     jclass clazz,
                                                     jmethodID methodID,
                                                     ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::CallNonvirtualShortMethodV(JNIEnv* env,
                                                      jobject obj,
                                                      jclass clazz,
                                                      jmethodID methodID,
                                                      va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::CallNonvirtualShortMethodA(JNIEnv* env,
                                                      jobject obj,
                                                      jclass clazz,
                                                      jmethodID methodID,
                                                      jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::CallNonvirtualIntMethod(JNIEnv* env,
                                                 jobject obj,
                                                 jclass clazz,
                                                 jmethodID methodID,
                                                 ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::CallNonvirtualIntMethodV(JNIEnv* env,
                                                  jobject obj,
                                                  jclass clazz,
                                                  jmethodID methodID,
                                                  va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::CallNonvirtualIntMethodA(JNIEnv* env,
                                                  jobject obj,
                                                  jclass clazz,
                                                  jmethodID methodID,
                                                  jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::CallNonvirtualLongMethod(JNIEnv* env,
                                                   jobject obj,
                                                   jclass clazz,
                                                   jmethodID methodID,
                                                   ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::CallNonvirtualLongMethodV(JNIEnv* env,
                                                    jobject obj,
                                                    jclass clazz,
                                                    jmethodID methodID,
                                                    va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::CallNonvirtualLongMethodA(JNIEnv* env,
                                                    jobject obj,
                                                    jclass clazz,
                                                    jmethodID methodID,
                                                    jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::CallNonvirtualFloatMethod(JNIEnv* env,
                                                     jobject obj,
                                                     jclass clazz,
                                                     jmethodID methodID,
                                                     ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::CallNonvirtualFloatMethodV(JNIEnv* env,
                                                      jobject obj,
                                                      jclass clazz,
                                                      jmethodID methodID,
                                                      va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::CallNonvirtualFloatMethodA(JNIEnv* env,
                                                      jobject obj,
                                                      jclass clazz,
                                                      jmethodID methodID,
                                                      jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::CallNonvirtualDoubleMethod(JNIEnv* env,
                                                       jobject obj,
                                                       jclass clazz,
                                                       jmethodID methodID,
                                                       ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::CallNonvirtualDoubleMethodV(JNIEnv* env,
                                                        jobject obj,
                                                        jclass clazz,
                                                        jmethodID methodID,
                                                        va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::CallNonvirtualDoubleMethodA(JNIEnv* env,
                                                        jobject obj,
                                                        jclass clazz,
                                                        jmethodID methodID,
                                                        jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void JniNativeInterface::CallNonvirtualVoidMethod(JNIEnv* env,
                                                  jobject obj,
                                                  jclass clazz,
                                                  jmethodID methodID,
                                                  ...) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::CallNonvirtualVoidMethodV(JNIEnv* env,
                                                   jobject obj,
                                                   jclass clazz,
                                                   jmethodID methodID,
                                                   va_list args) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::CallNonvirtualVoidMethodA(JNIEnv* env,
                                                   jobject obj,
                                                   jclass clazz,
                                                   jmethodID methodID,
                                                   jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
}

jfieldID JniNativeInterface::GetFieldID(JNIEnv* env,
                                        jclass clazz,
                                        const char* name,
                                        const char* sig) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::GetObjectField(JNIEnv* env,
                                           jobject obj,
                                           jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean JniNativeInterface::GetBooleanField(JNIEnv* env,
                                             jobject obj,
                                             jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jbyte JniNativeInterface::GetByteField(JNIEnv* env,
                                       jobject obj,
                                       jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::GetCharField(JNIEnv* env,
                                       jobject obj,
                                       jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::GetShortField(JNIEnv* env,
                                         jobject obj,
                                         jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::GetIntField(JNIEnv* env,
                                     jobject obj,
                                     jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::GetLongField(JNIEnv* env,
                                       jobject obj,
                                       jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::GetFloatField(JNIEnv* env,
                                         jobject obj,
                                         jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::GetDoubleField(JNIEnv* env,
                                           jobject obj,
                                           jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void JniNativeInterface::SetObjectField(JNIEnv* env,
                                        jobject obj,
                                        jfieldID fieldID,
                                        jobject val) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetBooleanField(JNIEnv* env,
                                         jobject obj,
                                         jfieldID fieldID,
                                         jboolean val) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetByteField(JNIEnv* env,
                                      jobject obj,
                                      jfieldID fieldID,
                                      jbyte val) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetCharField(JNIEnv* env,
                                      jobject obj,
                                      jfieldID fieldID,
                                      jchar val) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetShortField(JNIEnv* env,
                                       jobject obj,
                                       jfieldID fieldID,
                                       jshort val) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetIntField(JNIEnv* env,
                                     jobject obj,
                                     jfieldID fieldID,
                                     jint val) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetLongField(JNIEnv* env,
                                      jobject obj,
                                      jfieldID fieldID,
                                      jlong val) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetFloatField(JNIEnv* env,
                                       jobject obj,
                                       jfieldID fieldID,
                                       jfloat val) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetDoubleField(JNIEnv* env,
                                        jobject obj,
                                        jfieldID fieldID,
                                        jdouble val) {
  LOG(FATAL) << "Unimplemented";
}

jmethodID JniNativeInterface::GetStaticMethodID(JNIEnv* env,
                                                jclass clazz,
                                                const char* name,
                                                const char* sig) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::CallStaticObjectMethod(JNIEnv* env,
                                                   jclass clazz,
                                                   jmethodID methodID,
                                                   ...) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::CallStaticObjectMethodV(JNIEnv* env,
                                                    jclass clazz,
                                                    jmethodID methodID,
                                                    va_list args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::CallStaticObjectMethodA(JNIEnv* env,
                                                    jclass clazz,
                                                    jmethodID methodID,
                                                    jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean JniNativeInterface::CallStaticBooleanMethod(JNIEnv* env,
                                                     jclass clazz,
                                                     jmethodID methodID,
                                                     ...) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean JniNativeInterface::CallStaticBooleanMethodV(JNIEnv* env,
                                                      jclass clazz,
                                                      jmethodID methodID,
                                                      va_list args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jboolean JniNativeInterface::CallStaticBooleanMethodA(JNIEnv* env,
                                                      jclass clazz,
                                                      jmethodID methodID,
                                                      jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jbyte JniNativeInterface::CallStaticByteMethod(JNIEnv* env,
                                               jclass clazz,
                                               jmethodID methodID,
                                               ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte JniNativeInterface::CallStaticByteMethodV(JNIEnv* env,
                                                jclass clazz,
                                                jmethodID methodID,
                                                va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jbyte JniNativeInterface::CallStaticByteMethodA(JNIEnv* env,
                                                jclass clazz,
                                                jmethodID methodID,
                                                jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::CallStaticCharMethod(JNIEnv* env,
                                               jclass clazz,
                                               jmethodID methodID,
                                               ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::CallStaticCharMethodV(JNIEnv* env,
                                                jclass clazz,
                                                jmethodID methodID,
                                                va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::CallStaticCharMethodA(JNIEnv* env,
                                                jclass clazz,
                                                jmethodID methodID,
                                                jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::CallStaticShortMethod(JNIEnv* env,
                                                 jclass clazz,
                                                 jmethodID methodID,
                                                 ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::CallStaticShortMethodV(JNIEnv* env,
                                                  jclass clazz,
                                                  jmethodID methodID,
                                                  va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::CallStaticShortMethodA(JNIEnv* env,
                                                  jclass clazz,
                                                  jmethodID methodID,
                                                  jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::CallStaticIntMethod(JNIEnv* env,
                                             jclass clazz,
                                             jmethodID methodID,
                                             ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::CallStaticIntMethodV(JNIEnv* env,
                                              jclass clazz,
                                              jmethodID methodID,
                                              va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::CallStaticIntMethodA(JNIEnv* env,
                                              jclass clazz,
                                              jmethodID methodID,
                                              jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::CallStaticLongMethod(JNIEnv* env,
                                               jclass clazz,
                                               jmethodID methodID,
                                               ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::CallStaticLongMethodV(JNIEnv* env,
                                                jclass clazz,
                                                jmethodID methodID,
                                                va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::CallStaticLongMethodA(JNIEnv* env,
                                                jclass clazz,
                                                jmethodID methodID,
                                                jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::CallStaticFloatMethod(JNIEnv* env,
                                                 jclass clazz,
                                                 jmethodID methodID,
                                                 ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::CallStaticFloatMethodV(JNIEnv* env,
                                                  jclass clazz,
                                                  jmethodID methodID,
                                                  va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::CallStaticFloatMethodA(JNIEnv* env,
                                                  jclass clazz,
                                                  jmethodID methodID,
                                                  jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::CallStaticDoubleMethod(JNIEnv* env,
                                                   jclass clazz,
                                                   jmethodID methodID,
                                                   ...) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::CallStaticDoubleMethodV(JNIEnv* env,
                                                    jclass clazz,
                                                    jmethodID methodID,
                                                    va_list args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::CallStaticDoubleMethodA(JNIEnv* env,
                                                    jclass clazz,
                                                    jmethodID methodID,
                                                    jvalue* args) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void JniNativeInterface::CallStaticVoidMethod(JNIEnv* env,
                                              jclass cls,
                                              jmethodID methodID,
                                              ...) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::CallStaticVoidMethodV(JNIEnv* env,
                                               jclass cls,
                                               jmethodID methodID,
                                               va_list args) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::CallStaticVoidMethodA(JNIEnv* env,
                                               jclass cls,
                                               jmethodID methodID,
                                               jvalue*  args) {
  LOG(FATAL) << "Unimplemented";
}

jfieldID JniNativeInterface::GetStaticFieldID(JNIEnv* env,
                                              jclass clazz,
                                              const char* name,
                                              const char* sig) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jobject JniNativeInterface::GetStaticObjectField(JNIEnv* env,
                                                 jclass clazz,
                                                 jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean JniNativeInterface::GetStaticBooleanField(JNIEnv* env,
                                                   jclass clazz,
                                                   jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jbyte JniNativeInterface::GetStaticByteField(JNIEnv* env,
                                             jclass clazz,
                                             jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jchar JniNativeInterface::GetStaticCharField(JNIEnv* env,
                                             jclass clazz,
                                             jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jshort JniNativeInterface::GetStaticShortField(JNIEnv* env,
                                               jclass clazz,
                                               jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::GetStaticIntField(JNIEnv* env,
                                           jclass clazz,
                                           jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jlong JniNativeInterface::GetStaticLongField(JNIEnv* env,
                                             jclass clazz,
                                             jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jfloat JniNativeInterface::GetStaticFloatField(JNIEnv* env,
                                               jclass clazz,
                                               jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jdouble JniNativeInterface::GetStaticDoubleField(JNIEnv* env,
                                                 jclass clazz,
                                                 jfieldID fieldID) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void JniNativeInterface::SetStaticObjectField(JNIEnv* env,
                                              jclass clazz,
                                              jfieldID fieldID,
                                              jobject value) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetStaticBooleanField(JNIEnv* env,
                                               jclass clazz,
                                               jfieldID fieldID,
                                               jboolean value) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetStaticByteField(JNIEnv* env,
                                            jclass clazz,
                                            jfieldID fieldID,
                                            jbyte value) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetStaticCharField(JNIEnv* env,
                                            jclass clazz,
                                            jfieldID fieldID,
                                            jchar value) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetStaticShortField(JNIEnv* env,
                                             jclass clazz,
                                             jfieldID fieldID,
                                             jshort value) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetStaticIntField(JNIEnv* env,
                                           jclass clazz,
                                           jfieldID fieldID,
                                           jint value) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetStaticLongField(JNIEnv* env,
                                            jclass clazz,
                                            jfieldID fieldID,
                                            jlong value) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetStaticFloatField(JNIEnv* env,
                                             jclass clazz,
                                             jfieldID fieldID,
                                             jfloat value) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetStaticDoubleField(JNIEnv* env,
                                              jclass clazz,
                                              jfieldID fieldID,
                                              jdouble value) {
  LOG(FATAL) << "Unimplemented";
}

jstring JniNativeInterface::NewString(JNIEnv* env,
                                      const jchar* unicode,
                                      jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jsize JniNativeInterface::GetStringLength(JNIEnv* env, jstring str) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

const jchar* JniNativeInterface::GetStringChars(JNIEnv* env,
                                                jstring str,
                                                jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void JniNativeInterface::ReleaseStringChars(JNIEnv* env,
                                            jstring str,
                                            const jchar* chars) {
  LOG(FATAL) << "Unimplemented";
}

jstring JniNativeInterface::NewStringUTF(JNIEnv* env, const char* utf) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jsize JniNativeInterface::GetStringUTFLength(JNIEnv* env, jstring str) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

const char* JniNativeInterface::GetStringUTFChars(JNIEnv* env,
                                                  jstring str,
                                                  jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void JniNativeInterface::ReleaseStringUTFChars(JNIEnv* env,
                                               jstring str,
                                               const char* chars) {
  LOG(FATAL) << "Unimplemented";
}

jsize JniNativeInterface::GetArrayLength(JNIEnv* env, jarray array) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jobjectArray JniNativeInterface::NewObjectArray(JNIEnv* env,
                                                jsize len,
                                                jclass clazz,
                                                jobject init) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jobject JniNativeInterface::GetObjectArrayElement(JNIEnv* env,
                                                  jobjectArray array,
                                                  jsize index) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void JniNativeInterface::SetObjectArrayElement(JNIEnv* env,
                                               jobjectArray array,
                                               jsize index,
                                               jobject val) {
  LOG(FATAL) << "Unimplemented";
}

jbooleanArray JniNativeInterface::NewBooleanArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jbyteArray JniNativeInterface::NewByteArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jcharArray JniNativeInterface::NewCharArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jshortArray JniNativeInterface::NewShortArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jintArray JniNativeInterface::NewIntArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jlongArray JniNativeInterface::NewLongArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jfloatArray JniNativeInterface::NewFloatArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jdoubleArray JniNativeInterface::NewDoubleArray(JNIEnv* env, jsize len) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jboolean* JniNativeInterface::GetBooleanArrayElements(JNIEnv* env,
                                                      jbooleanArray array,
                                                      jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jbyte* JniNativeInterface::GetByteArrayElements(JNIEnv* env,
                                                jbyteArray array,
                                                jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jchar* JniNativeInterface::GetCharArrayElements(JNIEnv* env,
                                               jcharArray array,
                                               jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jshort* JniNativeInterface::GetShortArrayElements(JNIEnv* env,
                                                  jshortArray array,
                                                  jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jint* JniNativeInterface::GetIntArrayElements(JNIEnv* env,
                                              jintArray array,
                                              jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jlong* JniNativeInterface::GetLongArrayElements(JNIEnv* env,
                                                jlongArray array,
                                                jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jfloat* JniNativeInterface::GetFloatArrayElements(JNIEnv* env,
                                                  jfloatArray array,
                                                  jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jdouble* JniNativeInterface::GetDoubleArrayElements(JNIEnv* env,
                                                    jdoubleArray array,
                                                    jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void JniNativeInterface::ReleaseBooleanArrayElements(JNIEnv* env,
                                                     jbooleanArray array,
                                                     jboolean* elems,
                                                     jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::ReleaseByteArrayElements(JNIEnv* env,
                                                  jbyteArray array,
                                                  jbyte* elems,
                                                  jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::ReleaseCharArrayElements(JNIEnv* env,
                                                  jcharArray array,
                                                  jchar* elems,
                                                  jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::ReleaseShortArrayElements(JNIEnv* env,
                                                   jshortArray array,
                                                   jshort* elems,
                                                   jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::ReleaseIntArrayElements(JNIEnv* env,
                                                 jintArray array,
                                                 jint* elems,
                                                 jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::ReleaseLongArrayElements(JNIEnv* env,
                                                  jlongArray array,
                                                  jlong* elems,
                                                  jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::ReleaseFloatArrayElements(JNIEnv* env,
                                                   jfloatArray array,
                                                   jfloat* elems,
                                                   jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::ReleaseDoubleArrayElements(JNIEnv* env,
                                                    jdoubleArray array,
                                                    jdouble* elems,
                                                    jint mode) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::GetBooleanArrayRegion(JNIEnv* env,
                                               jbooleanArray array,
                                               jsize start,
                                               jsize l,
                                               jboolean* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::GetByteArrayRegion(JNIEnv* env,
                                            jbyteArray array,
                                            jsize start,
                                            jsize len,
                                            jbyte* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::GetCharArrayRegion(JNIEnv* env,
                                            jcharArray array,
                                            jsize start,
                                            jsize len,
                                            jchar* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::GetShortArrayRegion(JNIEnv* env,
                                             jshortArray array,
                                             jsize start,
                                             jsize len,
                                             jshort* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::GetIntArrayRegion(JNIEnv* env,
                                           jintArray array,
                                           jsize start,
                                           jsize len,
                                           jint* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::GetLongArrayRegion(JNIEnv* env,
                                            jlongArray array,
                                            jsize start,
                                            jsize len,
                                            jlong* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::GetFloatArrayRegion(JNIEnv* env,
                                             jfloatArray array,
                                             jsize start,
                                             jsize len,
                                             jfloat* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::GetDoubleArrayRegion(JNIEnv* env,
                                              jdoubleArray array,
                                              jsize start,
                                              jsize len,
                                              jdouble* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetBooleanArrayRegion(JNIEnv* env,
                                               jbooleanArray array,
                                               jsize start,
                                               jsize l,
                                               const jboolean* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetByteArrayRegion(JNIEnv* env,
                                            jbyteArray array,
                                            jsize start,
                                            jsize len,
                                            const jbyte* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetCharArrayRegion(JNIEnv* env,
                                            jcharArray array,
                                            jsize start,
                                            jsize len,
                                            const jchar* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetShortArrayRegion(JNIEnv* env,
                                             jshortArray array,
                                             jsize start,
                                             jsize len,
                                             const jshort* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetIntArrayRegion(JNIEnv* env,
                                           jintArray array,
                                           jsize start,
                                           jsize len,
                                           const jint* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetLongArrayRegion(JNIEnv* env,
                                            jlongArray array,
                                            jsize start,
                                            jsize len,
                                            const jlong* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetFloatArrayRegion(JNIEnv* env,
                                             jfloatArray array,
                                             jsize start,
                                             jsize len,
                                             const jfloat* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::SetDoubleArrayRegion(JNIEnv* env,
                                              jdoubleArray array,
                                              jsize start,
                                              jsize len,
                                              const jdouble* buf) {
  LOG(FATAL) << "Unimplemented";
}

jint JniNativeInterface::RegisterNatives(JNIEnv* env,
                                         jclass clazz,
                                         const JNINativeMethod* methods,
                                         jint nMethods) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::UnregisterNatives(JNIEnv* env, jclass clazz) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::MonitorEnter(JNIEnv* env, jobject obj) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::MonitorExit(JNIEnv* env, jobject obj) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jint JniNativeInterface::GetJavaVM(JNIEnv* env, JavaVM* *vm) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

void JniNativeInterface::GetStringRegion(JNIEnv* env,
                                         jstring str,
                                         jsize start,
                                         jsize len,
                                         jchar* buf) {
  LOG(FATAL) << "Unimplemented";
}

void JniNativeInterface::GetStringUTFRegion(JNIEnv* env,
                                            jstring str,
                                            jsize start,
                                            jsize len,
                                            char* buf) {
  LOG(FATAL) << "Unimplemented";
}

void* JniNativeInterface::GetPrimitiveArrayCritical(JNIEnv* env,
                                                    jarray array,
                                                    jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void JniNativeInterface::ReleasePrimitiveArrayCritical(JNIEnv* env,
                                                       jarray array,
                                                       void* carray,
                                                       jint mode) {
  LOG(FATAL) << "Unimplemented";
}

const jchar* JniNativeInterface::GetStringCritical(JNIEnv* env,
                                                   jstring s,
                                                   jboolean* isCopy) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void JniNativeInterface::ReleaseStringCritical(JNIEnv* env,
                                               jstring s,
                                               const jchar* cstr) {
  LOG(FATAL) << "Unimplemented";
}

jweak JniNativeInterface::NewWeakGlobalRef(JNIEnv* env, jobject obj) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

void JniNativeInterface::DeleteWeakGlobalRef(JNIEnv* env, jweak obj) {
  LOG(FATAL) << "Unimplemented";
}

jboolean JniNativeInterface::ExceptionCheck(JNIEnv* env) {
  LOG(FATAL) << "Unimplemented";
  return JNI_FALSE;
}

jobject JniNativeInterface::NewDirectByteBuffer(JNIEnv* env,
                                                void* address,
                                                jlong capacity) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}


void* JniNativeInterface::GetDirectBufferAddress(JNIEnv* env, jobject buf) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

jlong JniNativeInterface::GetDirectBufferCapacity(JNIEnv* env, jobject buf) {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

jobjectRefType JniNativeInterface::GetObjectRefType(JNIEnv* env, jobject jobj) {
  LOG(FATAL) << "Unimplemented";
  return JNIInvalidRefType;
}

struct JNINativeInterface JniNativeInterface::native_interface_ = {
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
  JniEnvironment** jni_env = reinterpret_cast<JniEnvironment**>(p_env);
  const char* name = NULL;
  if (thr_args != NULL) {
    // TODO: check version
    name = static_cast<JavaVMAttachArgs*>(thr_args)->name;
    // TODO: thread group
  }
  bool success = runtime->AttachCurrentThread(name, jni_env);
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
  JniEnvironment** jni_env = reinterpret_cast<JniEnvironment**>(p_env);
  const char* name = NULL;
  if (thr_args != NULL) {
    // TODO: check version
    name = static_cast<JavaVMAttachArgs*>(thr_args)->name;
    // TODO: thread group
  }
  bool success = runtime->AttachCurrentThreadAsDaemon(name, jni_env);
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
