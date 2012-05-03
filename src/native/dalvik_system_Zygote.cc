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

#include <grp.h>
#include <paths.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cutils/sched_policy.h"
#include "debugger.h"
#include "jni_internal.h"
#include "JniConstants.h"
#include "JNIHelp.h"
#include "ScopedLocalRef.h"
#include "ScopedPrimitiveArray.h"
#include "ScopedUtfChars.h"
#include "thread.h"

#if defined(HAVE_PRCTL)
#include <sys/prctl.h>
#endif

#if defined(__linux__)
#include <sys/personality.h>
#endif

namespace art {

static pid_t gSystemServerPid = 0;

static void Zygote_nativeExecShell(JNIEnv* env, jclass, jstring javaCommand) {
  ScopedUtfChars command(env, javaCommand);
  if (command.c_str() == NULL) {
    return;
  }
  const char* argp[] = {_PATH_BSHELL, "-c", command.c_str(), NULL};
  LOG(INFO) << "Exec: " << argp[0] << ' ' << argp[1] << ' ' << argp[2];

  execv(_PATH_BSHELL, const_cast<char**>(argp));
  exit(127);
}

// This signal handler is for zygote mode, since the zygote must reap its children
static void SigChldHandler(int /*signal_number*/) {
  pid_t pid;
  int status;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
     // Log process-death status that we care about.  In general it is
     // not safe to call LOG(...) from a signal handler because of
     // possible reentrancy.  However, we know a priori that the
     // current implementation of LOG() is safe to call from a SIGCHLD
     // handler in the zygote process.  If the LOG() implementation
     // changes its locking strategy or its use of syscalls within the
     // lazy-init critical section, its use here may become unsafe.
    if (WIFEXITED(status)) {
      if (WEXITSTATUS(status)) {
        LOG(INFO) << "Process " << pid << " exited cleanly (" << WEXITSTATUS(status) << ")";
      } else if (false) {
        LOG(INFO) << "Process " << pid << " exited cleanly (" << WEXITSTATUS(status) << ")";
      }
    } else if (WIFSIGNALED(status)) {
      if (WTERMSIG(status) != SIGKILL) {
        LOG(INFO) << "Process " << pid << " terminated by signal (" << WTERMSIG(status) << ")";
      } else if (false) {
        LOG(INFO) << "Process " << pid << " terminated by signal (" << WTERMSIG(status) << ")";
      }
#ifdef WCOREDUMP
      if (WCOREDUMP(status)) {
        LOG(INFO) << "Process " << pid << " dumped core";
      }
#endif /* ifdef WCOREDUMP */
    }

    // If the just-crashed process is the system_server, bring down zygote
    // so that it is restarted by init and system server will be restarted
    // from there.
    if (pid == gSystemServerPid) {
      LOG(ERROR) << "Exit zygote because system server (" << pid << ") has terminated";
      kill(getpid(), SIGKILL);
    }
  }

  if (pid < 0) {
    PLOG(WARNING) << "Zygote SIGCHLD error in waitpid";
  }
}

// Configures the SIGCHLD handler for the zygote process. This is configured
// very late, because earlier in the runtime we may fork() and exec()
// other processes, and we want to waitpid() for those rather than
// have them be harvested immediately.
//
// This ends up being called repeatedly before each fork(), but there's
// no real harm in that.
static void SetSigChldHandler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SigChldHandler;

  int err = sigaction(SIGCHLD, &sa, NULL);
  if (err < 0) {
    PLOG(WARNING) << "Error setting SIGCHLD handler";
  }
}

// Sets the SIGCHLD handler back to default behavior in zygote children.
static void UnsetSigChldHandler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;

  int err = sigaction(SIGCHLD, &sa, NULL);
  if (err < 0) {
    PLOG(WARNING) << "Error unsetting SIGCHLD handler";
  }
}

// Calls POSIX setgroups() using the int[] object as an argument.
// A NULL argument is tolerated.
static int SetGids(JNIEnv* env, jintArray javaGids) {
  if (javaGids == NULL) {
    return 0;
  }

  COMPILE_ASSERT(sizeof(gid_t) == sizeof(jint), sizeof_gid_and_jint_are_differerent);
  ScopedIntArrayRO gids(env, javaGids);
  if (gids.get() == NULL) {
    return -1;
  }
  return setgroups(gids.size(), (const gid_t *) &gids[0]);
}

// Sets the resource limits via setrlimit(2) for the values in the
// two-dimensional array of integers that's passed in. The second dimension
// contains a tuple of length 3: (resource, rlim_cur, rlim_max). NULL is
// treated as an empty array.
//
// -1 is returned on error.
static int SetRLimits(JNIEnv* env, jobjectArray javaRlimits) {
  if (javaRlimits == NULL) {
    return 0;
  }

  rlimit rlim;
  memset(&rlim, 0, sizeof(rlim));

  for (int i = 0; i < env->GetArrayLength(javaRlimits); i++) {
    ScopedLocalRef<jobject> javaRlimitObject(env, env->GetObjectArrayElement(javaRlimits, i));
    ScopedIntArrayRO javaRlimit(env, reinterpret_cast<jintArray>(javaRlimitObject.get()));
    if (javaRlimit.size() != 3) {
      LOG(ERROR) << "rlimits array must have a second dimension of size 3";
      return -1;
    }

    rlim.rlim_cur = javaRlimit[1];
    rlim.rlim_max = javaRlimit[2];

    int err = setrlimit(javaRlimit[0], &rlim);
    if (err < 0) {
      return -1;
    }
  }
  return 0;
}

#if defined(HAVE_ANDROID_OS)
static void SetCapabilities(int64_t permitted, int64_t effective) {
  __user_cap_header_struct capheader;
  __user_cap_data_struct capdata;

  memset(&capheader, 0, sizeof(capheader));
  memset(&capdata, 0, sizeof(capdata));

  capheader.version = _LINUX_CAPABILITY_VERSION;
  capheader.pid = 0;

  capdata.effective = effective;
  capdata.permitted = permitted;

  if (capset(&capheader, &capdata) != 0) {
    PLOG(FATAL) << "capset(" << permitted << ", " << effective << ") failed";
  }
}
#else
static void SetCapabilities(int64_t, int64_t) {}
#endif

static void EnableDebugFeatures(uint32_t debug_flags) {
  // Must match values in dalvik.system.Zygote.
  enum {
    DEBUG_ENABLE_DEBUGGER           = 1,
    DEBUG_ENABLE_CHECKJNI           = 1 << 1,
    DEBUG_ENABLE_ASSERT             = 1 << 2,
    DEBUG_ENABLE_SAFEMODE           = 1 << 3,
    DEBUG_ENABLE_JNI_LOGGING        = 1 << 4,
  };

  if ((debug_flags & DEBUG_ENABLE_CHECKJNI) != 0) {
    Runtime* runtime = Runtime::Current();
    JavaVMExt* vm = runtime->GetJavaVM();
    if (!vm->check_jni) {
      LOG(DEBUG) << "Late-enabling -Xcheck:jni";
      vm->SetCheckJniEnabled(true);
      // There's only one thread running at this point, so only one JNIEnv to fix up.
      Thread::Current()->GetJniEnv()->SetCheckJniEnabled(true);
    } else {
      LOG(DEBUG) << "Not late-enabling -Xcheck:jni (already on)";
    }
    debug_flags &= ~DEBUG_ENABLE_CHECKJNI;
  }

  if ((debug_flags & DEBUG_ENABLE_JNI_LOGGING) != 0) {
    gLogVerbosity.third_party_jni = true;
    debug_flags &= ~DEBUG_ENABLE_JNI_LOGGING;
  }

  Dbg::SetJdwpAllowed((debug_flags & DEBUG_ENABLE_DEBUGGER) != 0);
#ifdef HAVE_ANDROID_OS
  if ((debug_flags & DEBUG_ENABLE_DEBUGGER) != 0) {
    /* To let a non-privileged gdbserver attach to this
     * process, we must set its dumpable bit flag. However
     * we are not interested in generating a coredump in
     * case of a crash, so also set the coredump size to 0
     * to disable that
     */
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) < 0) {
      PLOG(ERROR) << "could not set dumpable bit flag for pid " << getpid();
    } else {
      rlimit rl;
      rl.rlim_cur = 0;
      rl.rlim_max = RLIM_INFINITY;
      if (setrlimit(RLIMIT_CORE, &rl) < 0) {
        PLOG(ERROR) << "could not disable core file generation for pid " << getpid();
      }
    }
  }
#endif
  debug_flags &= ~DEBUG_ENABLE_DEBUGGER;

  // These two are for backwards compatibility with Dalvik.
  debug_flags &= ~DEBUG_ENABLE_ASSERT;
  debug_flags &= ~DEBUG_ENABLE_SAFEMODE;

  if (debug_flags != 0) {
    LOG(ERROR) << StringPrintf("Unknown bits set in debug_flags: %#x", debug_flags);
  }
}

#ifdef HAVE_ANDROID_OS
extern "C" int gMallocLeakZygoteChild;
#endif

// Utility routine to fork zygote and specialize the child process.
static pid_t ForkAndSpecializeCommon(JNIEnv* env, uid_t uid, gid_t gid, jintArray javaGids,
                                     jint debug_flags, jobjectArray javaRlimits,
                                     jlong permittedCapabilities, jlong effectiveCapabilities) {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsZygote()) << "runtime instance not started with -Xzygote";
  if (false) { // TODO: do we need do anything special like !dvmGcPreZygoteFork()?
    LOG(FATAL) << "pre-fork heap failed";
  }

  SetSigChldHandler();

  // Grab thread before fork potentially makes Thread::pthread_key_self_ unusable.
  Thread* self = Thread::Current();

  // dvmDumpLoaderStats("zygote");  // TODO: ?
  pid_t pid = fork();

  if (pid == 0) {
    // The child process

#ifdef HAVE_ANDROID_OS
    gMallocLeakZygoteChild = 1;

    // keep caps across UID change, unless we're staying root */
    if (uid != 0) {
      int err = prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
      if (err < 0) {
        PLOG(FATAL) << "cannot PR_SET_KEEPCAPS";
      }
    }
#endif // HAVE_ANDROID_OS

    int err = SetGids(env, javaGids);
    if (err < 0) {
        PLOG(FATAL) << "setgroups failed";
    }

    err = SetRLimits(env, javaRlimits);
    if (err < 0) {
      PLOG(FATAL) << "setrlimit failed";
    }

    err = setgid(gid);
    if (err < 0) {
      PLOG(FATAL) << "setgid(" << gid << ") failed";
    }

    err = setuid(uid);
    if (err < 0) {
      PLOG(FATAL) << "setuid(" << uid << ") failed";
    }

#if defined(__linux__)
    // Work around ARM kernel ASLR lossage (http://b/5817320).
    int old_personality = personality(0xffffffff);
    int new_personality = personality(old_personality | ADDR_NO_RANDOMIZE);
    if (new_personality == -1) {
      PLOG(WARNING) << "personality(" << new_personality << ") failed";
    }
#endif

    SetCapabilities(permittedCapabilities, effectiveCapabilities);

#if 1
    UNIMPLEMENTED(WARNING) << "enable this code when cutils/sched_policy.h has SP_DEFAULT";
#else
    err = set_sched_policy(0, SP_DEFAULT);
    if (err < 0) {
      errno = -err;
      PLOG(FATAL) << "set_sched_policy(0, SP_DEFAULT) failed";
    }
#endif

    // Our system thread ID, etc, has changed so reset Thread state.
    self->InitAfterFork();

    EnableDebugFeatures(debug_flags);

    UnsetSigChldHandler();
    runtime->DidForkFromZygote();
  } else if (pid > 0) {
    // the parent process
  }
  return pid;
}

static jint Zygote_nativeForkAndSpecialize(JNIEnv* env, jclass, jint uid, jint gid, jintArray gids,
                                           jint debug_flags, jobjectArray rlimits) {
  return ForkAndSpecializeCommon(env, uid, gid, gids, debug_flags, rlimits, 0, 0);
}

static jint Zygote_nativeForkSystemServer(JNIEnv* env, jclass, uid_t uid, gid_t gid, jintArray gids,
                                          jint debug_flags, jobjectArray rlimits,
                                          jlong permittedCapabilities, jlong effectiveCapabilities) {
  pid_t pid = ForkAndSpecializeCommon(env, uid, gid, gids,
                                      debug_flags, rlimits,
                                      permittedCapabilities, effectiveCapabilities);
  if (pid > 0) {
      // The zygote process checks whether the child process has died or not.
      LOG(INFO) << "System server process " << pid << " has been created";
      gSystemServerPid = pid;
      // There is a slight window that the system server process has crashed
      // but it went unnoticed because we haven't published its pid yet. So
      // we recheck here just to make sure that all is well.
      int status;
      if (waitpid(pid, &status, WNOHANG) == pid) {
          LOG(FATAL) << "System server process " << pid << " has died. Restarting Zygote!";
      }
  }
  return pid;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Zygote, nativeExecShell, "(Ljava/lang/String;)V"),
  //NATIVE_METHOD(Zygote, nativeFork, "()I"),
  NATIVE_METHOD(Zygote, nativeForkAndSpecialize, "(II[II[[I)I"),
  NATIVE_METHOD(Zygote, nativeForkSystemServer, "(II[II[[IJJ)I"),
};

void register_dalvik_system_Zygote(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/Zygote", gMethods, NELEM(gMethods));
}

}  // namespace art
