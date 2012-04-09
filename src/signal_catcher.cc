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

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "class_linker.h"
#include "file.h"
#include "heap.h"
#include "os.h"
#include "runtime.h"
#include "scoped_heap_lock.h"
#include "thread.h"
#include "thread_list.h"
#include "utils.h"

namespace art {

SignalCatcher::SignalCatcher(const std::string& stack_trace_file)
    : stack_trace_file_(stack_trace_file),
      lock_("SignalCatcher lock"),
      cond_("SignalCatcher::cond_"),
      thread_(NULL) {
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

void SignalCatcher::Output(const std::string& s) {
  if (stack_trace_file_.empty()) {
    LOG(INFO) << s;
    return;
  }

  ScopedThreadStateChange tsc(Thread::Current(), kVmWait);
  int fd = open(stack_trace_file_.c_str(), O_APPEND | O_CREAT | O_WRONLY, 0666);
  if (fd == -1) {
    PLOG(ERROR) << "Unable to open stack trace file '" << stack_trace_file_ << "'";
    return;
  }
  UniquePtr<File> file(OS::FileFromFd(stack_trace_file_.c_str(), fd));
  if (!file->WriteFully(s.data(), s.size())) {
    PLOG(ERROR) << "Failed to write stack traces to '" << stack_trace_file_ << "'";
  } else {
    LOG(INFO) << "Wrote stack traces to '" << stack_trace_file_ << "'";
  }
  close(fd);
}

void SignalCatcher::HandleSigQuit() {
  Runtime* runtime = Runtime::Current();
  ThreadList* thread_list = runtime->GetThreadList();

  // We take the heap lock before suspending all threads so we don't end up in a situation where
  // one of the suspended threads suspended via the implicit FullSuspendCheck on the slow path of
  // Heap::Lock, which is the only case where a thread can be suspended while holding the heap lock.
  // (We need the heap lock when we dump the thread list. We could probably fix this by duplicating
  // more state from java.lang.Thread in struct Thread.)
  ScopedHeapLock heap_lock;
  thread_list->SuspendAll();

  std::ostringstream os;
  os << "\n"
     << "----- pid " << getpid() << " at " << GetIsoDate() << " -----\n";

  std::string cmdline;
  if (ReadFileToString("/proc/self/cmdline", &cmdline)) {
    std::replace(cmdline.begin(), cmdline.end(), '\0', ' ');
    os << "Cmd line: " << cmdline << "\n";
  }

  runtime->Dump(os);

  if (false) {
    std::string maps;
    if (ReadFileToString("/proc/self/maps", &maps)) {
      os << "/proc/self/maps:\n" << maps;
    }
  }

  os << "----- end " << getpid() << " -----\n";

  thread_list->ResumeAll();

  Output(os.str());
}

void SignalCatcher::HandleSigUsr1() {
  LOG(INFO) << "SIGUSR1 forcing GC (no HPROF)";
  Runtime::Current()->GetHeap()->CollectGarbage(false);
}

int SignalCatcher::WaitForSignal(sigset_t& mask) {
  ScopedThreadStateChange tsc(thread_, kVmWait);

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

  if (!ShouldHalt()) {
    // Let the user know we got the signal, just in case the system's too screwed for us to
    // actually do what they want us to do...
    LOG(INFO) << *thread_ << ": reacting to signal " << signal_number;

    // If anyone's holding locks (which might prevent us from getting back into state Runnable), say so...
    Runtime::Current()->DumpLockHolders(LOG(INFO));
  }

  return signal_number;
}

void* SignalCatcher::Run(void* arg) {
  SignalCatcher* signal_catcher = reinterpret_cast<SignalCatcher*>(arg);
  CHECK(signal_catcher != NULL);

  Runtime* runtime = Runtime::Current();
  runtime->AttachCurrentThread("Signal Catcher", true, Thread::GetSystemThreadGroup());
  Thread::Current()->SetState(kRunnable);

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
    int signal_number = signal_catcher->WaitForSignal(mask);
    if (signal_catcher->ShouldHalt()) {
      runtime->DetachCurrentThread();
      return NULL;
    }

    switch (signal_number) {
    case SIGQUIT:
      signal_catcher->HandleSigQuit();
      break;
    case SIGUSR1:
      signal_catcher->HandleSigUsr1();
      break;
    default:
      LOG(ERROR) << "Unexpected signal %d" << signal_number;
      break;
    }
  }
}

}  // namespace art
