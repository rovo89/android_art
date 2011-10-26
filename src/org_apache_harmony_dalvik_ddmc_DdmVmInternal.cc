/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "debugger.h"
#include "logging.h"

#include "JniConstants.h"  // Last to avoid problems with LOG redefinition.
#include "ScopedPrimitiveArray.h"  // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

static void DdmVmInternal_enableRecentAllocations(JNIEnv* env, jclass, jboolean enable) {
  UNIMPLEMENTED(WARNING);
  if (enable) {
    //dvmEnableAllocTracker();
  } else {
    //dvmDisableAllocTracker();
  }
}

static jbyteArray DdmVmInternal_getRecentAllocations(JNIEnv* env, jclass) {
  UNIMPLEMENTED(WARNING);
  return NULL;
  //ArrayObject* data = dvmDdmGetRecentAllocations();
  //dvmReleaseTrackedAlloc(data, NULL);
  //return reinterpret_cast<jbyteArray>(addLocalReference(env, data));
}

static jboolean DdmVmInternal_getRecentAllocationStatus(JNIEnv* env, jclass) {
  UNIMPLEMENTED(WARNING);
  return JNI_FALSE;
  //return (gDvm.allocRecords != NULL);
}

/*
 * Get a stack trace as an array of StackTraceElement objects.  Returns
 * NULL on failure, e.g. if the threadId couldn't be found.
 */
static jobjectArray DdmVmInternal_getStackTraceById(JNIEnv* env, jclass, jint threadId) {
  UNIMPLEMENTED(WARNING);
  return NULL;
  //ArrayObject* trace = dvmDdmGetStackTraceById(threadId);
  //return reinterpret_cast<jobjectArray>(addLocalReference(env, trace));
}

static jbyteArray DdmVmInternal_getThreadStats(JNIEnv* env, jclass) {
  UNIMPLEMENTED(WARNING);
  return NULL;
  //ArrayObject* result = dvmDdmGenerateThreadStats();
  //dvmReleaseTrackedAlloc(result, NULL);
  //return reinterpret_cast<jbyteArray>(addLocalReference(env, result));
}

static jint DdmVmInternal_heapInfoNotify(JNIEnv* env, jclass, jint when) {
  UNIMPLEMENTED(WARNING);
  return 0;
  //return dvmDdmHandleHpifChunk(when);
}

/*
 * Enable DDM heap notifications.
 * @param when: 0=never (off), 1=during GC
 * @param what: 0=merged objects, 1=distinct objects
 * @param native: false=virtual heap, true=native heap
 */
static jboolean DdmVmInternal_heapSegmentNotify(JNIEnv* env, jclass, jint when, jint what, jboolean native) {
  UNIMPLEMENTED(WARNING);
  return JNI_FALSE;
  //return dvmDdmHandleHpsgNhsgChunk(when, what, native);
}

static void DdmVmInternal_threadNotify(JNIEnv* env, jclass, jboolean enable) {
  Dbg::DdmSetThreadNotification(enable);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(DdmVmInternal, enableRecentAllocations, "(Z)V"),
  NATIVE_METHOD(DdmVmInternal, getRecentAllocations, "()[B"),
  NATIVE_METHOD(DdmVmInternal, getRecentAllocationStatus, "()Z"),
  NATIVE_METHOD(DdmVmInternal, getStackTraceById, "(I)[Ljava/lang/StackTraceElement;"),
  NATIVE_METHOD(DdmVmInternal, getThreadStats, "()[B"),
  NATIVE_METHOD(DdmVmInternal, heapInfoNotify, "(I)Z"),
  NATIVE_METHOD(DdmVmInternal, heapSegmentNotify, "(IIZ)Z"),
  NATIVE_METHOD(DdmVmInternal, threadNotify, "(Z)V"),
};

}  // namespace

void register_org_apache_harmony_dalvik_ddmc_DdmVmInternal(JNIEnv* env) {
  jniRegisterNativeMethods(env, "org/apache/harmony/dalvik/ddmc/DdmVmInternal", gMethods, NELEM(gMethods));
}

}  // namespace art
