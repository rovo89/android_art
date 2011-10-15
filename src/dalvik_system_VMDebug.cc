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

#include "class_linker.h"
#include "jni_internal.h"
#include "ScopedUtfChars.h"
#include "toStringArray.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

#include <string.h>
#include <unistd.h>

namespace art {

namespace {

/*
 * Return a set of strings describing available VM features (this is chiefly
 * of interest to DDMS).
 */
jobjectArray VMDebug_getVmFeatureList(JNIEnv* env, jclass) {
  std::vector<std::string> features;
  //features.push_back("method-trace-profiling");
  //features.push_back("method-trace-profiling-streaming");
  //features.push_back("hprof-heap-dump");
  //features.push_back("hprof-heap-dump-streaming");
  return toStringArray(env, features);
}

void VMDebug_startAllocCounting(JNIEnv*, jclass) {
  Runtime::Current()->SetStatsEnabled(true);
}

void VMDebug_stopAllocCounting(JNIEnv*, jclass) {
  Runtime::Current()->SetStatsEnabled(false);
}

jint VMDebug_getAllocCount(JNIEnv* env, jclass, jint kind) {
  return Runtime::Current()->GetStat(kind);
}

void VMDebug_resetAllocCount(JNIEnv*, jclass, jint kinds) {
  Runtime::Current()->ResetStats(kinds);
}

void VMDebug_startMethodTracingDdmsImpl(JNIEnv* env, jclass, jint bufferSize, jint flags) {
  UNIMPLEMENTED(WARNING);
  //dvmMethodTraceStart("[DDMS]", -1, bufferSize, flags, true);
}

void VMDebug_startMethodTracingFd(JNIEnv* env, jclass, jstring javaTraceFilename, jobject javaFd, jint bufferSize, jint flags) {
  int originalFd = jniGetFDFromFileDescriptor(env, javaFd);
  if (originalFd < 0) {
    return;
  }

  int fd = dup(originalFd);
  if (fd < 0) {
    jniThrowExceptionFmt(env, "java/lang/RuntimeException", "dup(%d) failed: %s", originalFd, strerror(errno));
    return;
  }

  ScopedUtfChars traceFilename(env, javaTraceFilename);
  if (traceFilename.c_str() == NULL) {
    return;
  }
  UNIMPLEMENTED(WARNING);
  //dvmMethodTraceStart(traceFilename.c_str(), fd, bufferSize, flags, false);
}

void VMDebug_startMethodTracingFilename(JNIEnv* env, jclass, jstring javaTraceFilename, jint bufferSize, jint flags) {
  ScopedUtfChars traceFilename(env, javaTraceFilename);
  if (traceFilename.c_str() == NULL) {
    return;
  }
  UNIMPLEMENTED(WARNING);
  //dvmMethodTraceStart(traceFilename.c_str(), -1, bufferSize, flags, false);
}

jboolean VMDebug_isMethodTracingActive(JNIEnv*, jclass) {
  UNIMPLEMENTED(WARNING);
  return JNI_FALSE; //dvmIsMethodTraceActive();
}

void VMDebug_stopMethodTracing(JNIEnv*, jclass) {
  UNIMPLEMENTED(WARNING);
  //dvmMethodTraceStop();
}

void VMDebug_startEmulatorTracing(JNIEnv*, jclass) {
  UNIMPLEMENTED(WARNING);
  //dvmEmulatorTraceStart();
}

void VMDebug_stopEmulatorTracing(JNIEnv*, jclass) {
  UNIMPLEMENTED(WARNING);
  //dvmEmulatorTraceStop();
}

jboolean VMDebug_isDebuggerConnected(JNIEnv*, jclass) {
  UNIMPLEMENTED(WARNING);
  return JNI_FALSE; //dvmDbgIsDebuggerConnected();
}

jboolean VMDebug_isDebuggingEnabled(JNIEnv*, jclass) {
  UNIMPLEMENTED(WARNING);
  return JNI_FALSE; //return gDvm.jdwpConfigured;
}

jlong VMDebug_lastDebuggerActivity(JNIEnv*, jclass) {
  UNIMPLEMENTED(WARNING);
  return 0; //dvmDbgLastDebuggerActivity();
}

void VMDebug_startInstructionCounting(JNIEnv* env, jclass) {
  jniThrowException(env, "java/lang/UnsupportedOperationException", NULL);
}

void VMDebug_stopInstructionCounting(JNIEnv* env, jclass) {
  jniThrowException(env, "java/lang/UnsupportedOperationException", NULL);
}

void VMDebug_getInstructionCount(JNIEnv* env, jclass, jintArray javaCounts) {
  jniThrowException(env, "java/lang/UnsupportedOperationException", NULL);
}

void VMDebug_resetInstructionCount(JNIEnv* env, jclass) {
  jniThrowException(env, "java/lang/UnsupportedOperationException", NULL);
}

void VMDebug_printLoadedClasses(JNIEnv*, jclass, jint flags) {
  return Runtime::Current()->GetClassLinker()->DumpAllClasses(flags);
}

jint VMDebug_getLoadedClassCount(JNIEnv*, jclass) {
  return Runtime::Current()->GetClassLinker()->NumLoadedClasses();
}

/*
 * Returns the thread-specific CPU-time clock value for the current thread,
 * or -1 if the feature isn't supported.
 */
jlong VMDebug_threadCpuTimeNanos(JNIEnv*, jclass) {
#ifdef HAVE_POSIX_CLOCKS
  struct timespec now;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<jlong>(now.tv_sec*1000000000LL + now.tv_nsec);
#else
  return -1LL;
#endif
}

/*
 * static void dumpHprofData(String fileName, FileDescriptor fd)
 *
 * Cause "hprof" data to be dumped.  We can throw an IOException if an
 * error occurs during file handling.
 */
void VMDebug_dumpHprofData(JNIEnv* env, jclass, jstring javaFilename, jobject javaFd) {
  // Only one of these may be NULL.
  if (javaFilename == NULL && javaFd == NULL) {
    jniThrowNullPointerException(env, "fileName == null && fd == null");
    return;
  }

  std::string filename;
  if (javaFilename != NULL) {
    ScopedUtfChars chars(env, javaFilename);
    if (env->ExceptionCheck()) {
      return;
    }
    filename = chars.c_str();
  } else {
    filename = "[fd]";
  }

  int fd = -1;
  if (javaFd != NULL) {
    fd = jniGetFDFromFileDescriptor(env, javaFd);
    if (fd < 0) {
      jniThrowException(env, "Ljava/lang/RuntimeException;", "Invalid file descriptor");
      return;
    }
  }

  UNIMPLEMENTED(WARNING);
  int result = 0; //hprofDumpHeap(filename.c_str(), fd, false);
  if (result != 0) {
    // TODO: ideally we'd throw something more specific based on actual failure
    jniThrowException(env, "Ljava/lang/RuntimeException;", "Failure during heap dump; check log output for details");
    return;
  }
}

void VMDebug_dumpHprofDataDdms(JNIEnv* env, jclass) {
  UNIMPLEMENTED(WARNING);
  int result = 0; //hprofDumpHeap("[DDMS]", -1, true);
  if (result != 0) {
    // TODO: ideally we'd throw something more specific based on actual failure
    jniThrowException(env, "Ljava/lang/RuntimeException;", "Failure during heap dump; check log output for details");
    return;
  }
}

void VMDebug_dumpReferenceTables(JNIEnv* env, jclass) {
  LOG(INFO) << "--- reference table dump ---";

  JNIEnvExt* e = reinterpret_cast<JNIEnvExt*>(env);
  e->DumpReferenceTables();
  e->vm->DumpReferenceTables();

  LOG(INFO) << "---";
}

/*
 * Dump the current thread's interpreted stack and abort the VM.  Useful
 * for seeing both interpreted and native stack traces.
 */
void VMDebug_crash(JNIEnv*, jclass) {
  std::ostringstream os;
  os << "Crashing VM on request:\n";
  Thread::Current()->Dump(os);
  LOG(FATAL) << os.str();
}

/*
 * Provide a hook for gdb to hang to so that the VM can be stopped when
 * user-tagged source locations are being executed.
 */
void VMDebug_infopoint(JNIEnv*, jclass, jint id) {
  LOG(INFO) << "VMDebug infopoint " << id << " hit";
}

jlong VMDebug_countInstancesOfClass(JNIEnv* env, jclass, jclass javaClass, jboolean countAssignable) {
  Class* c = Decode<Class*>(env, javaClass);
  if (c == NULL) {
    return 0;
  }
  return Heap::CountInstances(c, countAssignable);
}

JNINativeMethod gMethods[] = {
  NATIVE_METHOD(VMDebug, countInstancesOfClass, "(Ljava/lang/Class;Z)J"),
  NATIVE_METHOD(VMDebug, crash, "()V"),
  NATIVE_METHOD(VMDebug, dumpHprofData, "(Ljava/lang/String;Ljava/io/FileDescriptor;)V"),
  NATIVE_METHOD(VMDebug, dumpHprofDataDdms, "()V"),
  NATIVE_METHOD(VMDebug, dumpReferenceTables, "()V"),
  NATIVE_METHOD(VMDebug, getAllocCount, "(I)I"),
  NATIVE_METHOD(VMDebug, getInstructionCount, "([I)V"),
  NATIVE_METHOD(VMDebug, getLoadedClassCount, "()I"),
  NATIVE_METHOD(VMDebug, getVmFeatureList, "()[Ljava/lang/String;"),
  NATIVE_METHOD(VMDebug, infopoint, "(I)V"),
  NATIVE_METHOD(VMDebug, isDebuggerConnected, "()Z"),
  NATIVE_METHOD(VMDebug, isDebuggingEnabled, "()Z"),
  NATIVE_METHOD(VMDebug, isMethodTracingActive, "()Z"),
  NATIVE_METHOD(VMDebug, lastDebuggerActivity, "()J"),
  NATIVE_METHOD(VMDebug, printLoadedClasses, "(I)V"),
  NATIVE_METHOD(VMDebug, resetAllocCount, "(I)V"),
  NATIVE_METHOD(VMDebug, resetInstructionCount, "()V"),
  NATIVE_METHOD(VMDebug, startAllocCounting, "()V"),
  NATIVE_METHOD(VMDebug, startEmulatorTracing, "()V"),
  NATIVE_METHOD(VMDebug, startInstructionCounting, "()V"),
  NATIVE_METHOD(VMDebug, startMethodTracingDdmsImpl, "(II)V"),
  NATIVE_METHOD(VMDebug, startMethodTracingFd, "(Ljava/lang/String;Ljava/io/FileDescriptor;II)V"),
  NATIVE_METHOD(VMDebug, startMethodTracingFilename, "(Ljava/lang/String;II)V"),
  NATIVE_METHOD(VMDebug, stopAllocCounting, "()V"),
  NATIVE_METHOD(VMDebug, stopEmulatorTracing, "()V"),
  NATIVE_METHOD(VMDebug, stopInstructionCounting, "()V"),
  NATIVE_METHOD(VMDebug, stopMethodTracing, "()V"),
  NATIVE_METHOD(VMDebug, threadCpuTimeNanos, "()J"),
};

}  // namespace

void register_dalvik_system_VMDebug(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/VMDebug", gMethods, NELEM(gMethods));
}

}  // namespace art
