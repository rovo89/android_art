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

#include "jni_internal.h"
#include "JNIHelp.h"
#include "ScopedLocalRef.h"
#include "ScopedPrimitiveArray.h"
#include "ScopedUtfChars.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

#include <grp.h>
#include <paths.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "thread.h"

namespace art {

namespace {

static pid_t gSystemServerPid = 0;

void Zygote_nativeExecShell(JNIEnv* env, jclass, jstring javaCommand) {
  ScopedUtfChars command(env, javaCommand);
  if (command.c_str() == NULL) {
    return;
  }
  const char *argp[] = {_PATH_BSHELL, "-c", command.c_str(), NULL};
  LOG(INFO) << "Exec: " << argp[0] << ' ' << argp[1] << ' ' << argp[2];

  execv(_PATH_BSHELL, (char**)argp);
  exit(127);
}


// This signal handler is for zygote mode, since the zygote must reap its children
void sigchldHandler(int s) {
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

// configure sigchld handler for the zygote process This is configured
// very late, because earlier in the runtime we may fork() and exec()
// other processes, and we want to waitpid() for those rather than
// have them be harvested immediately.
//
// This ends up being called repeatedly before each fork(), but there's
// no real harm in that.
void setSigchldHandler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigchldHandler;

  int err = sigaction(SIGCHLD, &sa, NULL);
  if (err < 0) {
    PLOG(WARNING) << "Error setting SIGCHLD handler";
  }
}

// Set the SIGCHLD handler back to default behavior in zygote children
void unsetSigchldHandler() {
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
int setGids(JNIEnv* env, jintArray javaGids) {
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
int setRlimits(JNIEnv* env, jobjectArray javaRlimits) {
  if (javaRlimits == NULL) {
    return 0;
  }

  struct rlimit rlim;
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

// Set Linux capability flags.
//
// Returns 0 on success, errno on failure.
int setCapabilities(int64_t permitted, int64_t effective) {
#ifdef HAVE_ANDROID_OS
  struct __user_cap_header_struct capheader;
  struct __user_cap_data_struct capdata;

  memset(&capheader, 0, sizeof(capheader));
  memset(&capdata, 0, sizeof(capdata));

  capheader.version = _LINUX_CAPABILITY_VERSION;
  capheader.pid = 0;

  capdata.effective = effective;
  capdata.permitted = permitted;

  if (capset(&capheader, &capdata) != 0) {
    return errno;
  }
#endif /*HAVE_ANDROID_OS*/

  return 0;
}

#ifdef HAVE_ANDROID_OS
extern "C" int gMallocLeakZygoteChild;
#endif

// Utility routine to fork zygote and specialize the child process.
pid_t forkAndSpecializeCommon(JNIEnv* env, uid_t uid, gid_t gid, jintArray javaGids,
                              jint debugFlags, jobjectArray javaRlimits,
                              jlong permittedCapabilities, jlong effectiveCapabilities)
{
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsZygote()) << "runtime instance not started with -Xzygote";
  if (false) { // TODO: do we need do anything special like !dvmGcPreZygoteFork()?
    LOG(FATAL) << "pre-fork heap failed";
  }

  setSigchldHandler();

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

    int err = setGids(env, javaGids);
    if (err < 0) {
        PLOG(FATAL) << "cannot setgroups()";
    }

    err = setRlimits(env, javaRlimits);
    if (err < 0) {
      PLOG(FATAL) << "cannot setrlimit()";
    }

    err = setgid(gid);
    if (err < 0) {
      PLOG(FATAL) << "cannot setgid(" << gid << ")";
    }

    err = setuid(uid);
    if (err < 0) {
      PLOG(FATAL) << "cannot setuid(" << uid << ")";
    }

    err = setCapabilities(permittedCapabilities, effectiveCapabilities);
    if (err != 0) {
      PLOG(FATAL) << "cannot set capabilities ("
                  << permittedCapabilities << "," << effectiveCapabilities << ")";
    }

    // Our system thread ID, etc, has changed so reset Thread state.
    self->InitAfterFork();

    // configure additional debug options
    // enableDebugFeatures(debugFlags);  // TODO: debugger

    unsetSigchldHandler();
    runtime->DidForkFromZygote();
  } else if (pid > 0) {
    // the parent process
  }
  return pid;
}

jint Zygote_nativeForkAndSpecialize(JNIEnv* env, jclass, jint uid, jint gid, jintArray gids,
                                    jint debugFlags, jobjectArray rlimits) {
  return forkAndSpecializeCommon(env, uid, gid, gids, debugFlags, rlimits, 0, 0);
}

jint Zygote_nativeForkSystemServer(JNIEnv* env, jclass, uid_t uid, gid_t gid, jintArray gids,
                                   jint debugFlags, jobjectArray rlimits,
                                   jlong permittedCapabilities, jlong effectiveCapabilities) {
  pid_t pid = forkAndSpecializeCommon(env, uid, gid, gids,
                                      debugFlags, rlimits,
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

}  // namespace

void register_dalvik_system_Zygote(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/Zygote", gMethods, NELEM(gMethods));
}

}  // namespace art
