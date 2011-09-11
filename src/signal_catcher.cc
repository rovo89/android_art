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

#include "heap.h"
#include "runtime.h"
#include "thread.h"
#include "utils.h"

namespace art {

SignalCatcher::SignalCatcher() : lock_("SignalCatcher lock") {
  SetHaltFlag(false);

  // Create a raw pthread; its start routine will attach to the runtime.
  errno = pthread_create(&thread_, NULL, &Run, this);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_create failed for signal catcher thread";
  }
}

SignalCatcher::~SignalCatcher() {
  // Since we know the thread is just sitting around waiting for signals
  // to arrive, send it one.
  SetHaltFlag(true);
  pthread_kill(thread_, SIGQUIT);
  pthread_join(thread_, NULL);
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
  // TODO: suspend all threads

  std::stringstream os;
  os << "\n"
     << "\n"
     << "----- pid " << getpid() << " at " << GetIsoDate() << " -----\n";

  std::string cmdline;
  if (ReadFileToString("/proc/self/cmdline", &cmdline)) {
    std::replace(cmdline.begin(), cmdline.end(), '\0', ' ');
    os << "Cmd line: " << cmdline << "\n";
  }

  Runtime::Current()->Dump(os);

  std::string maps;
  if (ReadFileToString("/proc/self/maps", &maps)) {
    os << "/proc/self/maps:\n" << maps;
  }

  os << "----- end " << getpid() << " -----";

  // TODO: resume all threads

  LOG(INFO) << os.str();
}

void SignalCatcher::HandleSigUsr1() {
  LOG(INFO) << "SIGUSR1 forcing GC (no HPROF)";
  Heap::CollectGarbage();
}

int WaitForSignal(Thread* thread, sigset_t& mask) {
  ScopedThreadStateChange tsc(thread, Thread::kWaiting); // TODO: VMWAIT

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

  Runtime::Current()->AttachCurrentThread("Signal Catcher", true);
  Thread* self = Thread::Current();
  CHECK(self != NULL);

  // Set up mask with signals we want to handle.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGQUIT);
  sigaddset(&mask, SIGUSR1);

  while (true) {
    int signal_number = WaitForSignal(self, mask);
    if (signal_catcher->ShouldHalt()) {
      Runtime::Current()->DetachCurrentThread();
      return NULL;
    }

    LOG(INFO) << *self << ": reacting to signal " << signal_number;
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
