/*
 * Copyright (C) 2014 The Android Open Source Project
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

/*
 * Services that OpenJDK expects the VM to provide.
 */
#include<stdio.h>
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>

#include "common_throws.h"
#include "gc/heap.h"
#include "thread.h"
#include "thread_list.h"
#include "runtime.h"
#include "handle_scope-inl.h"
#include "scoped_thread_state_change.h"
#include "ScopedUtfChars.h"
#include "mirror/class_loader.h"
#include "verify_object-inl.h"
#include "base/logging.h"
#include "base/macros.h"
#include "../../libcore/ojluni/src/main/native/jvm.h"  // TODO(narayan): fix it
#include "jni_internal.h"
#include "mirror/string-inl.h"
#include "scoped_fast_native_object_access.h"
#include "ScopedLocalRef.h"
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#ifdef __ANDROID__
// This function is provided by android linker.
extern "C" void android_update_LD_LIBRARY_PATH(const char* ld_library_path);
#endif  // __ANDROID__

#undef LOG_TAG
#define LOG_TAG "artopenjdx"

using art::DEBUG;
using art::WARNING;
using art::VERBOSE;
using art::INFO;
using art::ERROR;
using art::FATAL;

/* posix open() with extensions; used by e.g. ZipFile */
JNIEXPORT jint JVM_Open(const char* fname, jint flags, jint mode) {
    LOG(DEBUG) << "JVM_Open fname='" << fname << "', flags=" << flags << ", mode=" << mode;

    /*
     * The call is expected to handle JVM_O_DELETE, which causes the file
     * to be removed after it is opened.  Also, some code seems to
     * want the special return value JVM_EEXIST if the file open fails
     * due to O_EXCL.
     */
    int fd = TEMP_FAILURE_RETRY(open(fname, flags & ~JVM_O_DELETE, mode));
    if (fd < 0) {
        int err = errno;
        LOG(DEBUG) << "open(" << fname << ") failed: " << strerror(errno);
        if (err == EEXIST) {
            return JVM_EEXIST;
        } else {
            return -1;
        }
    }

    if (flags & JVM_O_DELETE) {
        LOG(DEBUG) << "Deleting '" << fname << "' after open\n";
        if (unlink(fname) != 0) {
            LOG(WARNING) << "Post-open deletion of '" << fname << "' failed: " << strerror(errno);
        }
        /* ignore */
    }

    LOG(VERBOSE) << "open(" << fname << ") --> " << fd;
    return fd;
}

/* posix close() */
JNIEXPORT jint JVM_Close(jint fd) {
    LOG(DEBUG) << "JVM_Close fd=" << fd;
    // don't want TEMP_FAILURE_RETRY here -- file is closed even if EINTR
    return close(fd);
}

/* posix read() */
JNIEXPORT jint JVM_Read(jint fd, char* buf, jint nbytes) {
    LOG(DEBUG) << "JVM_Read fd=" << fd << ", buf='" << buf << "', nbytes=" << nbytes;
    return TEMP_FAILURE_RETRY(read(fd, buf, nbytes));
}

/* posix write(); is used to write messages to stderr */
JNIEXPORT jint JVM_Write(jint fd, char* buf, jint nbytes) {
    LOG(DEBUG) << "JVM_Write fd=" << fd << ", buf='" << buf << "', nbytes=" << nbytes;
    return TEMP_FAILURE_RETRY(write(fd, buf, nbytes));
}

/* posix lseek() */
JNIEXPORT jlong JVM_Lseek(jint fd, jlong offset, jint whence) {
    LOG(DEBUG) << "JVM_Lseek fd=" << fd << ", offset=" << offset << ", whence=" << whence;
    return TEMP_FAILURE_RETRY(lseek(fd, offset, whence));
}

/*
 * "raw monitors" seem to be expected to behave like non-recursive pthread
 * mutexes.  They're used by ZipFile.
 */
JNIEXPORT void* JVM_RawMonitorCreate(void) {
    LOG(DEBUG) << "JVM_RawMonitorCreate";
    pthread_mutex_t* newMutex =
        reinterpret_cast<pthread_mutex_t*>(malloc(sizeof(pthread_mutex_t)));
    pthread_mutex_init(newMutex, NULL);
    return newMutex;
}

JNIEXPORT void JVM_RawMonitorDestroy(void* mon) {
    LOG(DEBUG) << "JVM_RawMonitorDestroy mon=" << mon;
    pthread_mutex_destroy(reinterpret_cast<pthread_mutex_t*>(mon));
}

JNIEXPORT jint JVM_RawMonitorEnter(void* mon) {
    LOG(DEBUG) << "JVM_RawMonitorEnter mon=" << mon;
    return pthread_mutex_lock(reinterpret_cast<pthread_mutex_t*>(mon));
}

JNIEXPORT void JVM_RawMonitorExit(void* mon) {
    LOG(DEBUG) << "JVM_RawMonitorExit mon=" << mon;
    pthread_mutex_unlock(reinterpret_cast<pthread_mutex_t*>(mon));
}

JNIEXPORT char* JVM_NativePath(char* path) {
    LOG(DEBUG) << "JVM_NativePath path='" << path << "'";
    return path;
}

JNIEXPORT jint JVM_GetLastErrorString(char* buf, int len) {
#if defined(__GLIBC__) || defined(__BIONIC__)
  int err = errno;    // grab before JVM_TRACE can trash it
  LOG(DEBUG) << "JVM_GetLastErrorString buf=" << buf << ", len=" << len;

  if (len == 0) {
    return 0;
  }

  char* result = strerror_r(err, buf, len);
  if (result != buf) {
    strncpy(buf, result, len);
    buf[len - 1] = '\0';
  }

  return strlen(buf);
#else
  UNUSED(buf);
  UNUSED(len);
  return -1;
#endif
}

JNIEXPORT int jio_fprintf(FILE* fp, const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    int len = jio_vfprintf(fp, fmt, args);
    va_end(args);

    return len;
}

JNIEXPORT int jio_vfprintf(FILE* fp, const char* fmt, va_list args) {
    assert(fp != NULL);
    return vfprintf(fp, fmt, args);
}

/* posix fsync() */
JNIEXPORT jint JVM_Sync(jint fd) {
    LOG(DEBUG) << "JVM_Sync fd=" << fd;
    return TEMP_FAILURE_RETRY(fsync(fd));
}

JNIEXPORT void* JVM_FindLibraryEntry(void* handle, const char* name) {
    LOG(DEBUG) << "JVM_FindLibraryEntry handle=" << handle << " name=" << name;
    return dlsym(handle, name);
}

JNIEXPORT jlong JVM_CurrentTimeMillis(JNIEnv* env, jclass clazz ATTRIBUTE_UNUSED) {
    LOG(DEBUG) << "JVM_CurrentTimeMillis env=" << env;
    struct timeval tv;

    gettimeofday(&tv, (struct timezone *) NULL);
    jlong when = tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    return when;
}

JNIEXPORT jint JVM_Socket(jint domain, jint type, jint protocol) {
    LOG(DEBUG) << "JVM_Socket domain=" << domain << ", type=" << type << ", protocol=" << protocol;

    return TEMP_FAILURE_RETRY(socket(domain, type, protocol));
}

JNIEXPORT jint JVM_InitializeSocketLibrary() {
  return 0;
}

int jio_vsnprintf(char *str, size_t count, const char *fmt, va_list args) {
  if ((intptr_t)count <= 0) return -1;
  return vsnprintf(str, count, fmt, args);
}

int jio_snprintf(char *str, size_t count, const char *fmt, ...) {
  va_list args;
  int len;
  va_start(args, fmt);
  len = jio_vsnprintf(str, count, fmt, args);
  va_end(args);
  return len;
}

JNIEXPORT jint JVM_SetSockOpt(jint fd, int level, int optname,
    const char* optval, int optlen) {
  LOG(DEBUG) << "JVM_SetSockOpt fd=" << fd << ", level=" << level << ", optname=" << optname
             << ", optval=" << optval << ", optlen=" << optlen;
  return TEMP_FAILURE_RETRY(setsockopt(fd, level, optname, optval, optlen));
}

JNIEXPORT jint JVM_SocketShutdown(jint fd, jint howto) {
  LOG(DEBUG) << "JVM_SocketShutdown fd=" << fd << ", howto=" << howto;
  return TEMP_FAILURE_RETRY(shutdown(fd, howto));
}

JNIEXPORT jint JVM_GetSockOpt(jint fd, int level, int optname, char* optval,
  int* optlen) {
  LOG(DEBUG) << "JVM_GetSockOpt fd=" << fd << ", level=" << level << ", optname=" << optname
             << ", optval=" << optval << ", optlen=" << optlen;

  socklen_t len = *optlen;
  int cc = TEMP_FAILURE_RETRY(getsockopt(fd, level, optname, optval, &len));
  *optlen = len;
  return cc;
}

JNIEXPORT jint JVM_GetSockName(jint fd, struct sockaddr* addr, int* addrlen) {
  LOG(DEBUG) << "JVM_GetSockName fd=" << fd << ", addr=" << addr << ", addrlen=" << addrlen;

  socklen_t len = *addrlen;
  int cc = TEMP_FAILURE_RETRY(getsockname(fd, addr, &len));
  *addrlen = len;
  return cc;
}

JNIEXPORT jint JVM_SocketAvailable(jint fd, jint* result) {
  LOG(DEBUG) << "JVM_SocketAvailable fd=" << fd << ", result=" << result;

  if (TEMP_FAILURE_RETRY(ioctl(fd, FIONREAD, result)) < 0) {
      LOG(DEBUG) << "ioctl(" << fd << ", FIONREAD) failed: " << strerror(errno);
      return JNI_FALSE;
  }

  return JNI_TRUE;
}

JNIEXPORT jint JVM_Send(jint fd, char* buf, jint nBytes, jint flags) {
  LOG(DEBUG) << "JVM_Send fd=" << fd << ", buf=" << buf << ", nBytes="
             << nBytes << ", flags=" << flags;

  return TEMP_FAILURE_RETRY(send(fd, buf, nBytes, flags));
}

JNIEXPORT jint JVM_SocketClose(jint fd) {
  LOG(DEBUG) << "JVM_SocketClose fd=" << fd;

    // don't want TEMP_FAILURE_RETRY here -- file is closed even if EINTR
  return close(fd);
}

JNIEXPORT jint JVM_Listen(jint fd, jint count) {
  LOG(DEBUG) << "JVM_Listen fd=" << fd << ", count=" << count;

  return TEMP_FAILURE_RETRY(listen(fd, count));
}

JNIEXPORT jint JVM_Connect(jint fd, struct sockaddr* addr, jint addrlen) {
  LOG(DEBUG) << "JVM_Connect fd=" << fd << ", addr=" << addr << ", addrlen=" << addrlen;

  return TEMP_FAILURE_RETRY(connect(fd, addr, addrlen));
}

JNIEXPORT int JVM_GetHostName(char* name, int namelen) {
  LOG(DEBUG) << "JVM_GetHostName name=" << name << ", namelen=" << namelen;

  return TEMP_FAILURE_RETRY(gethostname(name, namelen));
}

JNIEXPORT jstring JVM_InternString(JNIEnv* env, jstring jstr) {
  LOG(DEBUG) << "JVM_InternString env=" << env << ", jstr=" << jstr;
  art::ScopedFastNativeObjectAccess soa(env);
  art::mirror::String* s = soa.Decode<art::mirror::String*>(jstr);
  art::mirror::String* result = s->Intern();
  return soa.AddLocalReference<jstring>(result);
}

JNIEXPORT jlong JVM_FreeMemory(void) {
  return art::Runtime::Current()->GetHeap()->GetFreeMemory();
}

JNIEXPORT jlong JVM_TotalMemory(void) {
  return art::Runtime::Current()->GetHeap()->GetTotalMemory();
}

JNIEXPORT jlong JVM_MaxMemory(void) {
  return art::Runtime::Current()->GetHeap()->GetMaxMemory();
}

JNIEXPORT void JVM_GC(void) {
  if (art::Runtime::Current()->IsExplicitGcDisabled()) {
      LOG(INFO) << "Explicit GC skipped.";
      return;
  }
  art::Runtime::Current()->GetHeap()->CollectGarbage(false);
}

JNIEXPORT __attribute__((noreturn)) void JVM_Exit(jint status) {
  LOG(INFO) << "System.exit called, status: " << status;
  art::Runtime::Current()->CallExitHook(status);
  exit(status);
}

static void SetLdLibraryPath(JNIEnv* env, jstring javaLdLibraryPath) {
#ifdef __ANDROID__
  if (javaLdLibraryPath != nullptr) {
    ScopedUtfChars ldLibraryPath(env, javaLdLibraryPath);
    if (ldLibraryPath.c_str() != nullptr) {
      android_update_LD_LIBRARY_PATH(ldLibraryPath.c_str());
    }
  }

#else
  LOG(WARNING) << "android_update_LD_LIBRARY_PATH not found; .so dependencies will not work!";
  UNUSED(javaLdLibraryPath, env);
#endif
}


JNIEXPORT jstring JVM_NativeLoad(JNIEnv* env, jstring javaFilename, jobject javaLoader,
                                 jstring javaLdLibraryPath, jstring javaLibraryPermittedPath) {
  ScopedUtfChars filename(env, javaFilename);
  if (filename.c_str() == NULL) {
    return NULL;
  }

  int32_t target_sdk_version = art::Runtime::Current()->GetTargetSdkVersion();

  // Starting with N nativeLoad uses classloader local
  // linker namespace instead of global LD_LIBRARY_PATH
  // (23 is Marshmallow)
  if (target_sdk_version <= 23) {
    SetLdLibraryPath(env, javaLdLibraryPath);
  }

  std::string error_msg;
  {
    art::ScopedObjectAccess soa(env);
    art::StackHandleScope<1> hs(soa.Self());
    art::JavaVMExt* vm = art::Runtime::Current()->GetJavaVM();
    bool success = vm->LoadNativeLibrary(env, filename.c_str(), javaLoader,
                                         javaLdLibraryPath, javaLibraryPermittedPath, &error_msg);
    if (success) {
      return nullptr;
    }
  }

  // Don't let a pending exception from JNI_OnLoad cause a CheckJNI issue with NewStringUTF.
  env->ExceptionClear();
  return env->NewStringUTF(error_msg.c_str());
}

JNIEXPORT void JVM_StartThread(JNIEnv* env, jobject jthread, jlong stack_size, jboolean daemon) {
  art::Thread::CreateNativeThread(env, jthread, stack_size, daemon == JNI_TRUE);
}

JNIEXPORT void JVM_SetThreadPriority(JNIEnv* env, jobject jthread, jint prio) {
  art::ScopedObjectAccess soa(env);
  art::MutexLock mu(soa.Self(), *art::Locks::thread_list_lock_);
  art::Thread* thread = art::Thread::FromManagedThread(soa, jthread);
  if (thread != NULL) {
    thread->SetNativePriority(prio);
  }
}

JNIEXPORT void JVM_Yield(JNIEnv* env ATTRIBUTE_UNUSED, jclass threadClass ATTRIBUTE_UNUSED) {
  sched_yield();
}

JNIEXPORT void JVM_Sleep(JNIEnv* env, jclass threadClass ATTRIBUTE_UNUSED,
                         jobject java_lock, jlong millis) {
  art::ScopedFastNativeObjectAccess soa(env);
  art::mirror::Object* lock = soa.Decode<art::mirror::Object*>(java_lock);
  art::Monitor::Wait(art::Thread::Current(), lock, millis, 0, true, art::kSleeping);
}

JNIEXPORT jobject JVM_CurrentThread(JNIEnv* env, jclass unused ATTRIBUTE_UNUSED) {
  art::ScopedFastNativeObjectAccess soa(env);
  return soa.AddLocalReference<jobject>(soa.Self()->GetPeer());
}

JNIEXPORT void JVM_Interrupt(JNIEnv* env, jobject jthread) {
  art::ScopedFastNativeObjectAccess soa(env);
  art::MutexLock mu(soa.Self(), *art::Locks::thread_list_lock_);
  art::Thread* thread = art::Thread::FromManagedThread(soa, jthread);
  if (thread != nullptr) {
    thread->Interrupt(soa.Self());
  }
}

JNIEXPORT jboolean JVM_IsInterrupted(JNIEnv* env, jobject jthread, jboolean clearInterrupted) {
  if (clearInterrupted) {
    return static_cast<art::JNIEnvExt*>(env)->self->Interrupted() ? JNI_TRUE : JNI_FALSE;
  } else {
    art::ScopedFastNativeObjectAccess soa(env);
    art::MutexLock mu(soa.Self(), *art::Locks::thread_list_lock_);
    art::Thread* thread = art::Thread::FromManagedThread(soa, jthread);
    return (thread != nullptr) ? thread->IsInterrupted() : JNI_FALSE;
  }
}

JNIEXPORT jboolean JVM_HoldsLock(JNIEnv* env, jclass unused ATTRIBUTE_UNUSED, jobject jobj) {
  art::ScopedObjectAccess soa(env);
  art::mirror::Object* object = soa.Decode<art::mirror::Object*>(jobj);
  if (object == NULL) {
    art::ThrowNullPointerException("object == null");
    return JNI_FALSE;
  }
  return soa.Self()->HoldsLock(object);
}

JNIEXPORT void JVM_SetNativeThreadName(JNIEnv* env, jobject jthread, jstring java_name) {
  ScopedUtfChars name(env, java_name);
  {
    art::ScopedObjectAccess soa(env);
    if (soa.Decode<art::mirror::Object*>(jthread) == soa.Self()->GetPeer()) {
      soa.Self()->SetThreadName(name.c_str());
      return;
    }
  }
  // Suspend thread to avoid it from killing itself while we set its name. We don't just hold the
  // thread list lock to avoid this, as setting the thread name causes mutator to lock/unlock
  // in the DDMS send code.
  art::ThreadList* thread_list = art::Runtime::Current()->GetThreadList();
  bool timed_out;
  // Take suspend thread lock to avoid races with threads trying to suspend this one.
  art::Thread* thread;
  {
    thread = thread_list->SuspendThreadByPeer(jthread, true, false, &timed_out);
  }
  if (thread != NULL) {
    {
      art::ScopedObjectAccess soa(env);
      thread->SetThreadName(name.c_str());
    }
    thread_list->Resume(thread, false);
  } else if (timed_out) {
    LOG(ERROR) << "Trying to set thread name to '" << name.c_str() << "' failed as the thread "
        "failed to suspend within a generous timeout.";
  }
}

JNIEXPORT jint JVM_IHashCode(JNIEnv* env ATTRIBUTE_UNUSED,
                             jobject javaObject ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL) << "JVM_IHashCode is not implemented";
  return 0;
}

JNIEXPORT jlong JVM_NanoTime(JNIEnv* env ATTRIBUTE_UNUSED, jclass unused ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL) << "JVM_NanoTime is not implemented";
  return 0L;
}

JNIEXPORT void JVM_ArrayCopy(JNIEnv* /* env */, jclass /* unused */, jobject /* javaSrc */,
                             jint /* srcPos */, jobject /* javaDst */, jint /* dstPos */,
                             jint /* length */) {
  UNIMPLEMENTED(FATAL) << "JVM_ArrayCopy is not implemented";
}

JNIEXPORT jint JVM_FindSignal(const char* name ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "JVM_FindSignal is not implemented";
  return 0;
}

JNIEXPORT void* JVM_RegisterSignal(jint signum ATTRIBUTE_UNUSED, void* handler ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "JVM_RegisterSignal is not implemented";
  return nullptr;
}

JNIEXPORT jboolean JVM_RaiseSignal(jint signum ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "JVM_RaiseSignal is not implemented";
  return JNI_FALSE;
}

JNIEXPORT __attribute__((noreturn))  void JVM_Halt(jint code) {
  exit(code);
}

JNIEXPORT jboolean JVM_IsNaN(jdouble d) {
  return isnan(d);
}
