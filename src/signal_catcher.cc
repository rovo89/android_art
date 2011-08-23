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

bool SignalCatcher::halt_ = false;

SignalCatcher::SignalCatcher() {
  // Create a raw pthread; its start routine will attach to the runtime.
  errno = pthread_create(&thread_, NULL, &Run, NULL);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_create failed for signal catcher thread";
  }
}

SignalCatcher::~SignalCatcher() {
  // Since we know the thread is just sitting around waiting for signals
  // to arrive, send it one.
  halt_ = true;
  pthread_kill(thread_, SIGQUIT);
  pthread_join(thread_, NULL);
}

void SignalCatcher::HandleSigQuit() {
  // TODO: suspend all threads

  std::stringstream buffer;
  buffer << "\n"
         << "\n"
         << "----- pid " << getpid() << " at " << GetIsoDate() << " -----\n"
         << "Cmd line: " << ReadFileToString("/proc/self/cmdline") << "\n";

  Runtime::Current()->DumpStatistics(buffer);

  // TODO: dump all threads.
  // dvmDumpAllThreadsEx(&target, true);

  buffer << "/proc/self/maps:\n" << ReadFileToString("/proc/self/maps");
  buffer << "----- end " << getpid() << " -----";

  // TODO: resume all threads

  LOG(INFO) << buffer.str();
}

void SignalCatcher::HandleSigUsr1() {
  LOG(INFO) << "SIGUSR1 forcing GC (no HPROF)";
  Heap::CollectGarbage();
}

void* SignalCatcher::Run(void*) {
  CHECK(Runtime::Current()->AttachCurrentThread("Signal Catcher", NULL, true));
  Thread* self = Thread::Current();
  CHECK(self != NULL);

  LOG(INFO) << "Signal catcher thread started " << *self;

  // Set up mask with signals we want to handle.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGQUIT);
  sigaddset(&mask, SIGUSR1);

  while (true) {
    self->SetState(Thread::kWaiting); // TODO: VMWAIT

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

    if (!halt_) {
      LOG(INFO) << *self << ": reacting to signal " << signal_number;
    }

    // Set our status to runnable, self-suspending if GC in progress.
    self->SetState(Thread::kRunnable);

    if (halt_) {
      return NULL;
    }

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
