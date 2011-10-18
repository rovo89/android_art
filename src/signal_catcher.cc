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

#include "signal_catcher.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include "class_linker.h"
#include "heap.h"
#include "runtime.h"
#include "thread.h"
#include "thread_list.h"
#include "utils.h"

namespace art {

SignalCatcher::SignalCatcher()
    : lock_("SignalCatcher lock"), cond_("SignalCatcher::cond_"), thread_(NULL) {
  SetHaltFlag(false);

  // Create a raw pthread; its start routine will attach to the runtime.
  CHECK_PTHREAD_CALL(pthread_create, (&pthread_, NULL, &Run, this), "signal catcher thread");

  MutexLock mu(lock_);
  while (thread_ == NULL) {
    cond_.Wait(lock_);
  }
}

SignalCatcher::~SignalCatcher() {
  // Since we know the thread is just sitting around waiting for signals
  // to arrive, send it one.
  SetHaltFlag(true);
  CHECK_PTHREAD_CALL(pthread_kill, (pthread_, SIGQUIT), "signal catcher shutdown");
  CHECK_PTHREAD_CALL(pthread_join, (pthread_, NULL), "signal catcher shutdown");
}

void SignalCatcher::SetHaltFlag(bool new_value) {
  MutexLock mu(lock_);
  halt_ = new_value;
}

bool SignalCatcher::ShouldHalt() {
  MutexLock mu(lock_);
  return halt_;
}

void SignalCatcher::HandleSigQuit() {
  Runtime* runtime = Runtime::Current();
  ThreadList* thread_list = runtime->GetThreadList();
  ClassLinker* class_linker = runtime->GetClassLinker();

  LOG(INFO) << "Heap lock owner tid: " << Heap::GetLockOwner() << "\n"
            << "ThreadList lock owner tid: " << thread_list->GetLockOwner() << "\n"
            << "ClassLinker lock owner tid: " << class_linker->GetLockOwner() << "\n";

  thread_list->SuspendAll();

  std::ostringstream os;
  os << "\n"
     << "\n"
     << "----- pid " << getpid() << " at " << GetIsoDate() << " -----\n";

  std::string cmdline;
  if (ReadFileToString("/proc/self/cmdline", &cmdline)) {
    std::replace(cmdline.begin(), cmdline.end(), '\0', ' ');
    os << "Cmd line: " << cmdline << "\n";
  }

  runtime->Dump(os);

  std::string maps;
  if (ReadFileToString("/proc/self/maps", &maps)) {
    os << "/proc/self/maps:\n" << maps;
  }

  os << "----- end " << getpid() << " -----";

  thread_list->ResumeAll();

  LOG(INFO) << os.str();
}

void SignalCatcher::HandleSigUsr1() {
  LOG(INFO) << "SIGUSR1 forcing GC (no HPROF)";
  Heap::CollectGarbage();
}

int WaitForSignal(Thread* thread, sigset_t& mask) {
  ScopedThreadStateChange tsc(thread, Thread::kVmWait);

  // Signals for sigwait() must be blocked but not ignored.  We
  // block signals like SIGQUIT for all threads, so the condition
  // is met.  When the signal hits, we wake up, without any signal
  // handlers being invoked.

  // Sleep in sigwait() until a signal arrives. gdb causes EINTR failures.
  int signal_number;
  int rc = TEMP_FAILURE_RETRY(sigwait(&mask, &signal_number));
  if (rc != 0) {
    PLOG(FATAL) << "sigwait failed";
  }

  return signal_number;
}

void* SignalCatcher::Run(void* arg) {
  SignalCatcher* signal_catcher = reinterpret_cast<SignalCatcher*>(arg);
  CHECK(signal_catcher != NULL);

  Runtime* runtime = Runtime::Current();
  runtime->AttachCurrentThread("Signal Catcher", true);

  {
    MutexLock mu(signal_catcher->lock_);
    signal_catcher->thread_ = Thread::Current();
    signal_catcher->cond_.Broadcast();
  }

  // Set up mask with signals we want to handle.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGQUIT);
  sigaddset(&mask, SIGUSR1);

  while (true) {
    int signal_number = WaitForSignal(signal_catcher->thread_, mask);
    if (signal_catcher->ShouldHalt()) {
      runtime->DetachCurrentThread();
      return NULL;
    }

    LOG(INFO) << *signal_catcher->thread_ << ": reacting to signal " << signal_number;
    switch (signal_number) {
    case SIGQUIT:
      HandleSigQuit();
      break;
    case SIGUSR1:
      HandleSigUsr1();
      break;
    default:
      LOG(ERROR) << "Unexpected signal %d" << signal_number;
      break;
    }
  }
}

}  // namespace art
