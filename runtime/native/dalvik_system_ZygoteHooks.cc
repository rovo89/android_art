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

#include <stdlib.h>

#include "debugger.h"
#include "instruction_set.h"
#include "jni_internal.h"
#include "JNIHelp.h"
#include "ScopedUtfChars.h"
#include "thread-inl.h"

#if defined(HAVE_PRCTL)
#include <sys/prctl.h>
#endif

#include <sys/resource.h>

namespace art {

static void EnableDebugger() {
  // To let a non-privileged gdbserver attach to this
  // process, we must set our dumpable flag.
#if defined(HAVE_PRCTL)
  if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1) {
    PLOG(ERROR) << "prctl(PR_SET_DUMPABLE) failed for pid " << getpid();
  }
#endif
  // We don't want core dumps, though, so set the core dump size to 0.
  rlimit rl;
  rl.rlim_cur = 0;
  rl.rlim_max = RLIM_INFINITY;
  if (setrlimit(RLIMIT_CORE, &rl) == -1) {
    PLOG(ERROR) << "setrlimit(RLIMIT_CORE) failed for pid " << getpid();
  }
}

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
      LOG(INFO) << "Late-enabling -Xcheck:jni";
      vm->SetCheckJniEnabled(true);
      // There's only one thread running at this point, so only one JNIEnv to fix up.
      Thread::Current()->GetJniEnv()->SetCheckJniEnabled(true);
    } else {
      LOG(INFO) << "Not late-enabling -Xcheck:jni (already on)";
    }
    debug_flags &= ~DEBUG_ENABLE_CHECKJNI;
  }

  if ((debug_flags & DEBUG_ENABLE_JNI_LOGGING) != 0) {
    gLogVerbosity.third_party_jni = true;
    debug_flags &= ~DEBUG_ENABLE_JNI_LOGGING;
  }

  Dbg::SetJdwpAllowed((debug_flags & DEBUG_ENABLE_DEBUGGER) != 0);
  if ((debug_flags & DEBUG_ENABLE_DEBUGGER) != 0) {
    EnableDebugger();
  }
  debug_flags &= ~DEBUG_ENABLE_DEBUGGER;

  // These two are for backwards compatibility with Dalvik.
  debug_flags &= ~DEBUG_ENABLE_ASSERT;
  debug_flags &= ~DEBUG_ENABLE_SAFEMODE;

  if (debug_flags != 0) {
    LOG(ERROR) << StringPrintf("Unknown bits set in debug_flags: %#x", debug_flags);
  }
}

static jlong ZygoteHooks_nativePreFork(JNIEnv* env, jclass) {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsZygote()) << "runtime instance not started with -Xzygote";

  runtime->PreZygoteFork();

  // Grab thread before fork potentially makes Thread::pthread_key_self_ unusable.
  Thread* self = Thread::Current();
  return reinterpret_cast<jlong>(self);
}

static void ZygoteHooks_nativePostForkChild(JNIEnv* env, jclass, jlong token, jint debug_flags,
                                            jstring instruction_set) {
  Thread* thread = reinterpret_cast<Thread*>(token);
  // Our system thread ID, etc, has changed so reset Thread state.
  thread->InitAfterFork();
  EnableDebugFeatures(debug_flags);

  Runtime::NativeBridgeAction action = Runtime::NativeBridgeAction::kUnload;
  if (instruction_set != nullptr) {
    ScopedUtfChars isa_string(env, instruction_set);
    InstructionSet isa = GetInstructionSetFromString(isa_string.c_str());
    if (isa != kNone && isa != kRuntimeISA) {
      action = Runtime::NativeBridgeAction::kInitialize;
    }
  }
  Runtime::Current()->DidForkFromZygote(action);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(ZygoteHooks, nativePreFork, "()J"),
  NATIVE_METHOD(ZygoteHooks, nativePostForkChild, "(JILjava/lang/String;)V"),
};

void register_dalvik_system_ZygoteHooks(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/ZygoteHooks");
}

}  // namespace art
