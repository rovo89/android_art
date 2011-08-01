// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

#include "assembler.h"
#include "macros.h"

namespace art {

// TODO: This is a place holder for a true JNIEnv used to provide limited
// functionality for the JNI compiler
class JniEnvironment {
 public:
  explicit JniEnvironment();

  static Offset MonitorEnterOffset() {
    return Offset(OFFSETOF_MEMBER(JniEnvironment, monitor_enter_));
  }

  static Offset MonitorExitOffset() {
    return Offset(OFFSETOF_MEMBER(JniEnvironment, monitor_exit_));
  }

 private:
  struct JNINativeInterface_* functions_;

  void (*monitor_enter_)(JniEnvironment*, jobject);
  void (*monitor_exit_)(JniEnvironment*, jobject);

  DISALLOW_COPY_AND_ASSIGN(JniEnvironment);
};

class JniNativeInterface {
 public:
  static struct JNINativeInterface* GetInterface() {
    return &native_interface_;
  }
 private:
  static jint GetVersion(JNIEnv* env);

  static jclass DefineClass(JNIEnv* env,
                            const char* name,
                            jobject loader,
                            const jbyte* buf,
                            jsize len);
  static jclass FindClass(JNIEnv* env, const char* name);

  static jmethodID FromReflectedMethod(JNIEnv* env, jobject method);
  static jfieldID FromReflectedField(JNIEnv* env, jobject field);
  static jobject ToReflectedMethod(JNIEnv* env,
                                   jclass cls,
                                   jmethodID methodID,
                                   jboolean isStatic);

  static jclass GetSuperclass(JNIEnv* env, jclass sub);
  static jboolean IsAssignableFrom(JNIEnv* env, jclass sub, jclass sup);
  static jobject ToReflectedField(JNIEnv* env,
                                  jclass cls,
                                  jfieldID fieldID,
                                  jboolean isStatic);

  static jint Throw(JNIEnv* env, jthrowable obj);
  static jint ThrowNew(JNIEnv* env, jclass clazz, const char* msg);
  static jthrowable ExceptionOccurred(JNIEnv* env);
  static void ExceptionDescribe(JNIEnv* env);
  static void ExceptionClear(JNIEnv* env);
  static void FatalError(JNIEnv* env, const char* msg);

  static jint PushLocalFrame(JNIEnv* env, jint cap);
  static jobject PopLocalFrame(JNIEnv* env, jobject res);

  static jobject NewGlobalRef(JNIEnv* env, jobject lobj);
  static void DeleteGlobalRef(JNIEnv* env, jobject gref);
  static void DeleteLocalRef(JNIEnv* env, jobject obj);
  static jboolean IsSameObject(JNIEnv* env, jobject obj1, jobject obj2);

  static jobject NewLocalRef(JNIEnv* env, jobject ref);
  static jint EnsureLocalCapacity(JNIEnv* env, jint);

  static jobject AllocObject(JNIEnv* env, jclass clazz);
  static jobject NewObject(JNIEnv* env, jclass clazz, jmethodID methodID, ...);
  static jobject NewObjectV(JNIEnv* env,
                            jclass clazz,
                            jmethodID methodID,
                            va_list args);
  static jobject NewObjectA(JNIEnv* env,
                            jclass clazz,
                            jmethodID methodID,
                            jvalue* args);

  static jclass GetObjectClass(JNIEnv* env, jobject obj);
  static jboolean IsInstanceOf(JNIEnv* env, jobject obj, jclass clazz);

  static jmethodID GetMethodID(JNIEnv* env,
                               jclass clazz,
                               const char* name,
                               const char* sig);

  static jobject CallObjectMethod(JNIEnv* env,
                                  jobject obj,
                                  jmethodID methodID,
                                  ...);
  static jobject CallObjectMethodV(JNIEnv* env,
                                   jobject obj,
                                   jmethodID methodID,
                                   va_list args);
  static jobject CallObjectMethodA(JNIEnv* env,
                                   jobject obj,
                                   jmethodID methodID,
                                   jvalue*  args);

  static jboolean CallBooleanMethod(JNIEnv* env,
                                    jobject obj,
                                    jmethodID methodID,
                                    ...);
  static jboolean CallBooleanMethodV(JNIEnv* env,
                                     jobject obj,
                                     jmethodID methodID,
                                     va_list args);
  static jboolean CallBooleanMethodA(JNIEnv* env,
                                     jobject obj,
                                     jmethodID methodID,
                                     jvalue*  args);

  static jbyte CallByteMethod(JNIEnv* env,
                              jobject obj,
                              jmethodID methodID,
                              ...);
  static jbyte CallByteMethodV(JNIEnv* env,
                               jobject obj,
                               jmethodID methodID,
                               va_list args);
  static jbyte CallByteMethodA(JNIEnv* env,
                               jobject obj,
                               jmethodID methodID,
                               jvalue* args);

  static jchar CallCharMethod(JNIEnv* env,
                              jobject obj,
                              jmethodID methodID,
                              ...);
  static jchar CallCharMethodV(JNIEnv* env,
                               jobject obj,
                               jmethodID methodID,
                               va_list args);
  static jchar CallCharMethodA(JNIEnv* env,
                               jobject obj,
                               jmethodID methodID,
                               jvalue* args);

  static jshort CallShortMethod(JNIEnv* env,
                                jobject obj,
                                jmethodID methodID,
                                ...);
  static jshort CallShortMethodV(JNIEnv* env,
                                 jobject obj,
                                 jmethodID methodID,
                                 va_list args);
  static jshort CallShortMethodA(JNIEnv* env,
                                 jobject obj,
                                 jmethodID methodID,
                                 jvalue* args);

  static jint CallIntMethod(JNIEnv* env,
                            jobject obj,
                            jmethodID methodID,
                            ...);
  static jint CallIntMethodV(JNIEnv* env,
                             jobject obj,
                             jmethodID methodID,
                             va_list args);
  static jint CallIntMethodA(JNIEnv* env,
                             jobject obj,
                             jmethodID methodID,
                             jvalue* args);

  static jlong CallLongMethod(JNIEnv* env,
                              jobject obj,
                              jmethodID methodID,
                              ...);
  static jlong CallLongMethodV(JNIEnv* env,
                               jobject obj,
                               jmethodID methodID,
                               va_list args);
  static jlong CallLongMethodA(JNIEnv* env,
                               jobject obj,
                               jmethodID methodID,
                               jvalue* args);

  static jfloat CallFloatMethod(JNIEnv* env,
                                jobject obj,
                                jmethodID methodID,
                                ...);
  static jfloat CallFloatMethodV(JNIEnv* env,
                                 jobject obj,
                                 jmethodID methodID,
                                 va_list args);
  static jfloat CallFloatMethodA(JNIEnv* env,
                                 jobject obj,
                                 jmethodID methodID,
                                 jvalue* args);

  static jdouble CallDoubleMethod(JNIEnv* env,
                                  jobject obj,
                                  jmethodID methodID,
                                  ...);
  static jdouble CallDoubleMethodV(JNIEnv* env,
                                   jobject obj,
                                   jmethodID methodID,
                                   va_list args);
  static jdouble CallDoubleMethodA(JNIEnv* env,
                                   jobject obj,
                                   jmethodID methodID,
                                   jvalue* args);

  static void CallVoidMethod(JNIEnv* env,
                             jobject obj,
                             jmethodID methodID,
                             ...);
  static void CallVoidMethodV(JNIEnv* env,
                              jobject obj,
                              jmethodID methodID,
                              va_list args);
  static void CallVoidMethodA(JNIEnv* env,
                              jobject obj,
                              jmethodID methodID,
                              jvalue*  args);

  static jobject CallNonvirtualObjectMethod(JNIEnv* env,
                                            jobject obj,
                                            jclass clazz,
                                            jmethodID methodID,
                                            ...);
  static jobject CallNonvirtualObjectMethodV(JNIEnv* env,
                                             jobject obj,
                                             jclass clazz,
                                             jmethodID methodID,
                                             va_list args);
  static jobject CallNonvirtualObjectMethodA(JNIEnv* env,
                                             jobject obj,
                                             jclass clazz,
                                             jmethodID methodID,
                                             jvalue*  args);

  static jboolean CallNonvirtualBooleanMethod(JNIEnv* env,
                                              jobject obj,
                                              jclass clazz,
                                              jmethodID methodID,
                                              ...);
  static jboolean CallNonvirtualBooleanMethodV(JNIEnv* env,
                                               jobject obj,
                                               jclass clazz,
                                               jmethodID methodID,
                                               va_list args);
  static jboolean CallNonvirtualBooleanMethodA(JNIEnv* env,
                                               jobject obj,
                                               jclass clazz,
                                               jmethodID methodID,
                                               jvalue*  args);

  static jbyte CallNonvirtualByteMethod(JNIEnv* env,
                                        jobject obj,
                                        jclass clazz,
                                        jmethodID methodID,
                                        ...);
  static jbyte CallNonvirtualByteMethodV(JNIEnv* env,
                                         jobject obj,
                                         jclass clazz,
                                         jmethodID methodID,
                                         va_list args);
  static jbyte CallNonvirtualByteMethodA(JNIEnv* env,
                                         jobject obj,
                                         jclass clazz,
                                         jmethodID methodID,
                                         jvalue* args);

  static jchar CallNonvirtualCharMethod(JNIEnv* env,
                                        jobject obj,
                                        jclass clazz,
                                        jmethodID methodID,
                                        ...);
  static jchar CallNonvirtualCharMethodV(JNIEnv* env,
                                         jobject obj,
                                         jclass clazz,
                                         jmethodID methodID,
                                         va_list args);
  static jchar CallNonvirtualCharMethodA(JNIEnv* env,
                                         jobject obj,
                                         jclass clazz,
                                         jmethodID methodID,
                                         jvalue* args);

  static jshort CallNonvirtualShortMethod(JNIEnv* env,
                                          jobject obj,
                                          jclass clazz,
                                          jmethodID methodID,
                                          ...);
  static jshort CallNonvirtualShortMethodV(JNIEnv* env,
                                           jobject obj,
                                           jclass clazz,
                                           jmethodID methodID,
                                           va_list args);
  static jshort CallNonvirtualShortMethodA(JNIEnv* env,
                                           jobject obj,
                                           jclass clazz,
                                           jmethodID methodID,
                                           jvalue* args);

  static jint CallNonvirtualIntMethod(JNIEnv* env,
                                      jobject obj,
                                      jclass clazz,
                                      jmethodID methodID,
                                      ...);
  static jint CallNonvirtualIntMethodV(JNIEnv* env,
                                       jobject obj,
                                       jclass clazz,
                                       jmethodID methodID,
                                       va_list args);
  static jint CallNonvirtualIntMethodA(JNIEnv* env,
                                       jobject obj,
                                       jclass clazz,
                                       jmethodID methodID,
                                       jvalue* args);

  static jlong CallNonvirtualLongMethod(JNIEnv* env,
                                        jobject obj,
                                        jclass clazz,
                                        jmethodID methodID,
                                        ...);
  static jlong CallNonvirtualLongMethodV(JNIEnv* env,
                                         jobject obj,
                                         jclass clazz,
                                         jmethodID methodID,
                                         va_list args);
  static jlong CallNonvirtualLongMethodA(JNIEnv* env,
                                         jobject obj,
                                         jclass clazz,
                                         jmethodID methodID,
                                         jvalue* args);

  static jfloat CallNonvirtualFloatMethod(JNIEnv* env,
                                          jobject obj,
                                          jclass clazz,
                                          jmethodID methodID,
                                          ...);
  static jfloat CallNonvirtualFloatMethodV(JNIEnv* env,
                                           jobject obj,
                                           jclass clazz,
                                           jmethodID methodID,
                                           va_list args);
  static jfloat CallNonvirtualFloatMethodA(JNIEnv* env,
                                           jobject obj,
                                           jclass clazz,
                                           jmethodID methodID,
                                           jvalue* args);

  static jdouble CallNonvirtualDoubleMethod(JNIEnv* env,
                                            jobject obj,
                                            jclass clazz,
                                            jmethodID methodID,
                                            ...);
  static jdouble CallNonvirtualDoubleMethodV(JNIEnv* env,
                                             jobject obj,
                                             jclass clazz,
                                             jmethodID methodID,
                                             va_list args);
  static jdouble CallNonvirtualDoubleMethodA(JNIEnv* env,
                                             jobject obj,
                                             jclass clazz,
                                             jmethodID methodID,
                                             jvalue* args);

  static void CallNonvirtualVoidMethod(JNIEnv* env,
                                       jobject obj,
                                       jclass clazz,
                                       jmethodID methodID,
                                       ...);
  static void CallNonvirtualVoidMethodV(JNIEnv* env,
                                        jobject obj,
                                        jclass clazz,
                                        jmethodID methodID,
                                        va_list args);
  static void CallNonvirtualVoidMethodA(JNIEnv* env,
                                        jobject obj,
                                        jclass clazz,
                                        jmethodID methodID,
                                        jvalue*  args);

  static jfieldID GetFieldID(JNIEnv* env,
                             jclass clazz,
                             const char* name,
                             const char* sig);

  static jobject GetObjectField(JNIEnv* env, jobject obj, jfieldID fieldID);
  static jboolean GetBooleanField(JNIEnv* env, jobject obj, jfieldID fieldID);
  static jbyte GetByteField(JNIEnv* env, jobject obj, jfieldID fieldID);
  static jchar GetCharField(JNIEnv* env, jobject obj, jfieldID fieldID);
  static jshort GetShortField(JNIEnv* env, jobject obj, jfieldID fieldID);
  static jint GetIntField(JNIEnv* env, jobject obj, jfieldID fieldID);
  static jlong GetLongField(JNIEnv* env, jobject obj, jfieldID fieldID);
  static jfloat GetFloatField(JNIEnv* env, jobject obj, jfieldID fieldID);
  static jdouble GetDoubleField(JNIEnv* env, jobject obj, jfieldID fieldID);

  static void SetObjectField(JNIEnv* env,
                             jobject obj,
                             jfieldID fieldID,
                             jobject val);
  static void SetBooleanField(JNIEnv* env,
                              jobject obj,
                              jfieldID fieldID,
                              jboolean val);
  static void SetByteField(JNIEnv* env,
                           jobject obj,
                           jfieldID fieldID,
                           jbyte val);
  static void SetCharField(JNIEnv* env,
                           jobject obj,
                           jfieldID fieldID,
                           jchar val);
  static void SetShortField(JNIEnv* env,
                            jobject obj,
                            jfieldID fieldID,
                            jshort val);
  static void SetIntField(JNIEnv* env, jobject obj, jfieldID fieldID, jint val);
  static void SetLongField(JNIEnv* env,
                           jobject obj,
                           jfieldID fieldID,
                           jlong val);
  static void SetFloatField(JNIEnv* env,
                            jobject obj,
                            jfieldID fieldID,
                            jfloat val);
  static void SetDoubleField(JNIEnv* env,
                             jobject obj,
                             jfieldID fieldID,
                             jdouble val);

  static jmethodID GetStaticMethodID(JNIEnv* env,
                                     jclass clazz,
                                     const char* name,
                                     const char* sig);

  static jobject CallStaticObjectMethod(JNIEnv* env,
                                        jclass clazz,
                                        jmethodID methodID,
                                        ...);
  static jobject CallStaticObjectMethodV(JNIEnv* env,
                                         jclass clazz,
                                         jmethodID methodID,
                                         va_list args);
  static jobject CallStaticObjectMethodA(JNIEnv* env,
                                         jclass clazz,
                                         jmethodID methodID,
                                         jvalue* args);

  static jboolean CallStaticBooleanMethod(JNIEnv* env,
                                          jclass clazz,
                                          jmethodID methodID,
                                          ...);
  static jboolean CallStaticBooleanMethodV(JNIEnv* env,
                                           jclass clazz,
                                           jmethodID methodID,
                                           va_list args);
  static jboolean CallStaticBooleanMethodA(JNIEnv* env,
                                           jclass clazz,
                                           jmethodID methodID,
                                           jvalue* args);

  static jbyte CallStaticByteMethod(JNIEnv* env,
                                    jclass clazz,
                                    jmethodID methodID,
                                    ...);
  static jbyte CallStaticByteMethodV(JNIEnv* env,
                                     jclass clazz,
                                     jmethodID methodID,
                                     va_list args);
  static jbyte CallStaticByteMethodA(JNIEnv* env,
                                     jclass clazz,
                                     jmethodID methodID,
                                     jvalue* args);

  static jchar CallStaticCharMethod(JNIEnv* env,
                                    jclass clazz,
                                    jmethodID methodID,
                                    ...);
  static jchar CallStaticCharMethodV(JNIEnv* env,
                                     jclass clazz,
                                     jmethodID methodID,
                                     va_list args);
  static jchar CallStaticCharMethodA(JNIEnv* env,
                                     jclass clazz,
                                     jmethodID methodID,
                                     jvalue* args);

  static jshort CallStaticShortMethod(JNIEnv* env,
                                      jclass clazz,
                                      jmethodID methodID,
                                      ...);
  static jshort CallStaticShortMethodV(JNIEnv* env,
                                       jclass clazz,
                                       jmethodID methodID,
                                       va_list args);
  static jshort CallStaticShortMethodA(JNIEnv* env,
                                       jclass clazz,
                                       jmethodID methodID,
                                       jvalue* args);

  static jint CallStaticIntMethod(JNIEnv* env,
                                  jclass clazz,
                                  jmethodID methodID,
                                  ...);
  static jint CallStaticIntMethodV(JNIEnv* env,
                                   jclass clazz,
                                   jmethodID methodID,
                                   va_list args);
  static jint CallStaticIntMethodA(JNIEnv* env,
                                   jclass clazz,
                                   jmethodID methodID,
                                   jvalue* args);

  static jlong CallStaticLongMethod(JNIEnv* env,
                                    jclass clazz,
                                    jmethodID methodID,
                                    ...);
  static jlong CallStaticLongMethodV(JNIEnv* env,
                                     jclass clazz,
                                     jmethodID methodID,
                                     va_list args);
  static jlong CallStaticLongMethodA(JNIEnv* env,
                                     jclass clazz,
                                     jmethodID methodID,
                                     jvalue* args);

  static jfloat CallStaticFloatMethod(JNIEnv* env,
                                      jclass clazz,
                                      jmethodID methodID,
                                      ...);
  static jfloat CallStaticFloatMethodV(JNIEnv* env,
                                       jclass clazz,
                                       jmethodID methodID,
                                       va_list args);
  static jfloat CallStaticFloatMethodA(JNIEnv* env,
                                       jclass clazz,
                                       jmethodID methodID,
                                       jvalue* args);

  static jdouble CallStaticDoubleMethod(JNIEnv* env,
                                        jclass clazz,
                                        jmethodID methodID,
                                        ...);
  static jdouble CallStaticDoubleMethodV(JNIEnv* env,
                                         jclass clazz,
                                         jmethodID methodID,
                                         va_list args);
  static jdouble CallStaticDoubleMethodA(JNIEnv* env,
                                         jclass clazz,
                                         jmethodID methodID,
                                         jvalue* args);

  static void CallStaticVoidMethod(JNIEnv* env,
                                   jclass cls,
                                   jmethodID methodID,
                                   ...);
  static void CallStaticVoidMethodV(JNIEnv* env,
                                    jclass cls,
                                    jmethodID methodID,
                                    va_list args);
  static void CallStaticVoidMethodA(JNIEnv* env,
                                    jclass cls,
                                    jmethodID methodID,
                                    jvalue*  args);

  static jfieldID GetStaticFieldID(JNIEnv* env,
                                   jclass clazz,
                                   const char* name,
                                   const char* sig);
  static jobject GetStaticObjectField(JNIEnv* env,
                                      jclass clazz,
                                      jfieldID fieldID);
  static jboolean GetStaticBooleanField(JNIEnv* env,
                                        jclass clazz,
                                        jfieldID fieldID);
  static jbyte GetStaticByteField(JNIEnv* env, jclass clazz, jfieldID fieldID);
  static jchar GetStaticCharField(JNIEnv* env, jclass clazz, jfieldID fieldID);
  static jshort GetStaticShortField(JNIEnv* env,
                                    jclass clazz,
                                    jfieldID fieldID);
  static jint GetStaticIntField(JNIEnv* env, jclass clazz, jfieldID fieldID);
  static jlong GetStaticLongField(JNIEnv* env, jclass clazz, jfieldID fieldID);
  static jfloat GetStaticFloatField(JNIEnv* env,
                                    jclass clazz,
                                    jfieldID fieldID);
  static jdouble GetStaticDoubleField(JNIEnv* env,
                                      jclass clazz,
                                      jfieldID fieldID);

  static void SetStaticObjectField(JNIEnv* env,
                                   jclass clazz,
                                   jfieldID fieldID,
                                   jobject value);
  static void SetStaticBooleanField(JNIEnv* env,
                                    jclass clazz,
                                    jfieldID fieldID,
                                    jboolean value);
  static void SetStaticByteField(JNIEnv* env,
                                 jclass clazz,
                                 jfieldID fieldID,
                                 jbyte value);
  static void SetStaticCharField(JNIEnv* env,
                                 jclass clazz,
                                 jfieldID fieldID,
                                 jchar value);
  static void SetStaticShortField(JNIEnv* env,
                                  jclass clazz,
                                  jfieldID fieldID,
                                  jshort value);
  static void SetStaticIntField(JNIEnv* env,
                                jclass clazz,
                                jfieldID fieldID,
                                jint value);
  static void SetStaticLongField(JNIEnv* env,
                                 jclass clazz,
                                 jfieldID fieldID,
                                 jlong value);
  static void SetStaticFloatField(JNIEnv* env,
                                  jclass clazz,
                                  jfieldID fieldID,
                                  jfloat value);
  static void SetStaticDoubleField(JNIEnv* env,
                                   jclass clazz,
                                   jfieldID fieldID,
                                   jdouble value);

  static jstring NewString(JNIEnv* env, const jchar* unicode, jsize len);
  static jsize GetStringLength(JNIEnv* env, jstring str);
  static const jchar* GetStringChars(JNIEnv* env,
                                     jstring str,
                                     jboolean* isCopy);
  static void ReleaseStringChars(JNIEnv* env, jstring str, const jchar* chars);
  static jstring NewStringUTF(JNIEnv* env, const char* utf);
  static jsize GetStringUTFLength(JNIEnv* env, jstring str);
  static const char* GetStringUTFChars(JNIEnv* env,
                                       jstring str,
                                       jboolean* isCopy);
  static void ReleaseStringUTFChars(JNIEnv* env,
                                    jstring str,
                                    const char* chars);

  static jsize GetArrayLength(JNIEnv* env, jarray array);

  static jobjectArray NewObjectArray(JNIEnv* env,
                                     jsize len,
                                     jclass clazz,
                                     jobject init);
  static jobject GetObjectArrayElement(JNIEnv* env,
                                       jobjectArray array,
                                       jsize index);
  static void SetObjectArrayElement(JNIEnv* env,
                                    jobjectArray array,
                                    jsize index,
                                    jobject val);

  static jbooleanArray NewBooleanArray(JNIEnv* env, jsize len);
  static jbyteArray NewByteArray(JNIEnv* env, jsize len);
  static jcharArray NewCharArray(JNIEnv* env, jsize len);
  static jshortArray NewShortArray(JNIEnv* env, jsize len);
  static jintArray NewIntArray(JNIEnv* env, jsize len);
  static jlongArray NewLongArray(JNIEnv* env, jsize len);
  static jfloatArray NewFloatArray(JNIEnv* env, jsize len);
  static jdoubleArray NewDoubleArray(JNIEnv* env, jsize len);

  static jboolean*  GetBooleanArrayElements(JNIEnv* env,
                                            jbooleanArray array,
                                            jboolean* isCopy);
  static jbyte*  GetByteArrayElements(JNIEnv* env,
                                      jbyteArray array,
                                      jboolean* isCopy);
  static jchar*  GetCharArrayElements(JNIEnv* env,
                                      jcharArray array,
                                      jboolean* isCopy);
  static jshort*  GetShortArrayElements(JNIEnv* env,
                                        jshortArray array,
                                        jboolean* isCopy);
  static jint*  GetIntArrayElements(JNIEnv* env,
                                    jintArray array,
                                    jboolean* isCopy);
  static jlong*  GetLongArrayElements(JNIEnv* env,
                                      jlongArray array,
                                      jboolean* isCopy);
  static jfloat*  GetFloatArrayElements(JNIEnv* env,
                                        jfloatArray array,
                                        jboolean* isCopy);
  static jdouble*  GetDoubleArrayElements(JNIEnv* env,
                                          jdoubleArray array,
                                          jboolean* isCopy);

  static void ReleaseBooleanArrayElements(JNIEnv* env,
                                          jbooleanArray array,
                                          jboolean* elems,
                                          jint mode);
  static void ReleaseByteArrayElements(JNIEnv* env,
                                       jbyteArray array,
                                       jbyte* elems,
                                       jint mode);
  static void ReleaseCharArrayElements(JNIEnv* env,
                                       jcharArray array,
                                       jchar* elems,
                                       jint mode);
  static void ReleaseShortArrayElements(JNIEnv* env,
                                        jshortArray array,
                                        jshort* elems,
                                        jint mode);
  static void ReleaseIntArrayElements(JNIEnv* env,
                                      jintArray array,
                                      jint* elems,
                                      jint mode);
  static void ReleaseLongArrayElements(JNIEnv* env,
                                       jlongArray array,
                                       jlong* elems,
                                       jint mode);
  static void ReleaseFloatArrayElements(JNIEnv* env,
                                        jfloatArray array,
                                        jfloat* elems,
                                        jint mode);
  static void ReleaseDoubleArrayElements(JNIEnv* env,
                                         jdoubleArray array,
                                         jdouble* elems,
                                         jint mode);

  static void GetBooleanArrayRegion(JNIEnv* env,
                                    jbooleanArray array,
                                    jsize start,
                                    jsize l,
                                    jboolean* buf);
  static void GetByteArrayRegion(JNIEnv* env,
                                 jbyteArray array,
                                 jsize start,
                                 jsize len,
                                 jbyte* buf);
  static void GetCharArrayRegion(JNIEnv* env,
                                 jcharArray array,
                                 jsize start,
                                 jsize len,
                                 jchar* buf);
  static void GetShortArrayRegion(JNIEnv* env,
                                  jshortArray array,
                                  jsize start,
                                  jsize len,
                                  jshort* buf);
  static void GetIntArrayRegion(JNIEnv* env,
                                jintArray array,
                                jsize start,
                                jsize len,
                                jint* buf);
  static void GetLongArrayRegion(JNIEnv* env,
                                 jlongArray array,
                                 jsize start,
                                 jsize len,
                                 jlong* buf);
  static void GetFloatArrayRegion(JNIEnv* env,
                                  jfloatArray array,
                                  jsize start,
                                  jsize len,
                                  jfloat* buf);
  static void GetDoubleArrayRegion(JNIEnv* env,
                                   jdoubleArray array,
                                   jsize start,
                                   jsize len,
                                   jdouble* buf);

  static void SetBooleanArrayRegion(JNIEnv* env,
                                    jbooleanArray array,
                                    jsize start,
                                    jsize l,
                                    const jboolean* buf);
  static void SetByteArrayRegion(JNIEnv* env,
                                 jbyteArray array,
                                 jsize start,
                                 jsize len,
                                 const jbyte* buf);
  static void SetCharArrayRegion(JNIEnv* env,
                                 jcharArray array,
                                 jsize start,
                                 jsize len,
                                 const jchar* buf);
  static void SetShortArrayRegion(JNIEnv* env,
                                  jshortArray array,
                                  jsize start,
                                  jsize len,
                                  const jshort* buf);
  static void SetIntArrayRegion(JNIEnv* env,
                                jintArray array,
                                jsize start,
                                jsize len,
                                const jint* buf);
  static void SetLongArrayRegion(JNIEnv* env,
                                 jlongArray array,
                                 jsize start,
                                 jsize len,
                                 const jlong* buf);
  static void SetFloatArrayRegion(JNIEnv* env,
                                  jfloatArray array,
                                  jsize start,
                                  jsize len,
                                  const jfloat* buf);
  static void SetDoubleArrayRegion(JNIEnv* env,
                                   jdoubleArray array,
                                   jsize start,
                                   jsize len,
                                   const jdouble* buf);

  static jint RegisterNatives(JNIEnv* env,
                              jclass clazz,
                              const JNINativeMethod* methods,
                              jint nMethods);
  static jint UnregisterNatives(JNIEnv* env, jclass clazz);

  static jint MonitorEnter(JNIEnv* env, jobject obj);
  static jint MonitorExit(JNIEnv* env, jobject obj);

  static jint GetJavaVM(JNIEnv* env, JavaVM* *vm);

  static void GetStringRegion(JNIEnv* env,
                              jstring str,
                              jsize start,
                              jsize len,
                              jchar* buf);
  static void GetStringUTFRegion(JNIEnv* env,
                                 jstring str,
                                 jsize start,
                                 jsize len,
                                 char* buf);

  static void* GetPrimitiveArrayCritical(JNIEnv* env,
                                         jarray array,
                                         jboolean* isCopy);
  static void ReleasePrimitiveArrayCritical(JNIEnv* env,
                                            jarray array,
                                            void* carray,
                                            jint mode);

  static const jchar* GetStringCritical(JNIEnv* env,
                                        jstring s,
                                        jboolean* isCopy);
  static void ReleaseStringCritical(JNIEnv* env, jstring s, const jchar* cstr);

  static jweak NewWeakGlobalRef(JNIEnv* env, jobject obj);
  static void DeleteWeakGlobalRef(JNIEnv* env, jweak obj);

  static jboolean ExceptionCheck(JNIEnv* env);

  static jobject NewDirectByteBuffer(JNIEnv* env,
                                     void* address,
                                     jlong capacity);
  static void* GetDirectBufferAddress(JNIEnv* env, jobject buf);
  static jlong GetDirectBufferCapacity(JNIEnv* env, jobject buf);

  static jobjectRefType GetObjectRefType(JNIEnv* env, jobject obj);

  static struct JNINativeInterface native_interface_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(JniNativeInterface);
};

class JniInvokeInterface {
 public:
  static struct JNIInvokeInterface* GetInterface() {
    return &invoke_interface_;
  }
 private:
  static jint DestroyJavaVM(JavaVM* vm);
  static jint AttachCurrentThread(JavaVM* vm, JNIEnv** penv, void* thr_args);
  static jint DetachCurrentThread(JavaVM* vm);
  static jint GetEnv(JavaVM* vm, void** penv, int version);
  static jint AttachCurrentThreadAsDaemon(JavaVM* vm,
                                          JNIEnv** penv,
                                          void* thr_args);
  static struct JNIInvokeInterface invoke_interface_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(JniInvokeInterface);
};

}  // namespace art

#endif  // ART_SRC_JNI_INTERNAL_H_
