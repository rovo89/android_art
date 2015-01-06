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

#ifdef HAVE_ANDROID_OS
#include <android/log.h>
#else
#include <stdarg.h>
#include <iostream>
#endif

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "sigchain.h"

#if defined(__APPLE__)
#define _NSIG NSIG
#endif

namespace art {

typedef int (*SigActionFnPtr)(int, const struct sigaction*, struct sigaction*);

class SignalAction {
 public:
  SignalAction() : claimed_(false) {
  }

  // Claim the signal and keep the action specified.
  void Claim(const struct sigaction& action) {
    action_ = action;
    claimed_ = true;
  }

  // Unclaim the signal and restore the old action.
  void Unclaim(int signal) {
    claimed_ = false;
    sigaction(signal, &action_, NULL);        // Restore old action.
  }

  // Get the action associated with this signal.
  const struct sigaction& GetAction() const {
    return action_;
  }

  // Is the signal claimed?
  bool IsClaimed() const {
    return claimed_;
  }

  // Change the recorded action to that specified.
  void SetAction(const struct sigaction& action) {
    action_ = action;
  }

 private:
  struct sigaction action_;     // Action to be performed.
  bool claimed_;                // Whether signal is claimed or not.
};

// User's signal handlers
static SignalAction user_sigactions[_NSIG];
static bool initialized;
static void* linked_sigaction_sym;
static void* linked_sigprocmask_sym;

static void log(const char* format, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
#ifdef HAVE_ANDROID_OS
  __android_log_write(ANDROID_LOG_ERROR, "libsigchain", buf);
#else
  std::cout << buf << "\n";
#endif
  va_end(ap);
}

static void CheckSignalValid(int signal) {
  if (signal <= 0 || signal >= _NSIG) {
    log("Invalid signal %d", signal);
    abort();
  }
}

// Claim a signal chain for a particular signal.
extern "C" void ClaimSignalChain(int signal, struct sigaction* oldaction) {
  CheckSignalValid(signal);
  user_sigactions[signal].Claim(*oldaction);
}

extern "C" void UnclaimSignalChain(int signal) {
  CheckSignalValid(signal);

  user_sigactions[signal].Unclaim(signal);
}

// Invoke the user's signal handler.
extern "C" void InvokeUserSignalHandler(int sig, siginfo_t* info, void* context) {
  // Check the arguments.
  CheckSignalValid(sig);

  // The signal must have been claimed in order to get here.  Check it.
  if (!user_sigactions[sig].IsClaimed()) {
    abort();
  }

  const struct sigaction& action = user_sigactions[sig].GetAction();
  if ((action.sa_flags & SA_SIGINFO) == 0) {
    if (action.sa_handler != NULL) {
      action.sa_handler(sig);
    } else {
       signal(sig, SIG_DFL);
       raise(sig);
    }
  } else {
    if (action.sa_sigaction != NULL) {
      action.sa_sigaction(sig, info, context);
    } else {
       signal(sig, SIG_DFL);
       raise(sig);
    }
  }
}

extern "C" void EnsureFrontOfChain(int signal, struct sigaction* expected_action) {
  CheckSignalValid(signal);
  // Read the current action without looking at the chain, it should be the expected action.
  SigActionFnPtr linked_sigaction = reinterpret_cast<SigActionFnPtr>(linked_sigaction_sym);
  struct sigaction current_action;
  linked_sigaction(signal, nullptr, &current_action);
  // If the sigactions don't match then we put the current action on the chain and make ourself as
  // the main action.
  if (current_action.sa_sigaction != expected_action->sa_sigaction) {
    log("Warning: Unexpected sigaction action found %p\n", current_action.sa_sigaction);
    user_sigactions[signal].Claim(current_action);
    linked_sigaction(signal, expected_action, nullptr);
  }
}

// These functions are C linkage since they replace the functions in libc.

extern "C" int sigaction(int signal, const struct sigaction* new_action, struct sigaction* old_action) {
  // If this signal has been claimed as a signal chain, record the user's
  // action but don't pass it on to the kernel.
  // Note that we check that the signal number is in range here.  An out of range signal
  // number should behave exactly as the libc sigaction.
  if (signal > 0 && signal < _NSIG && user_sigactions[signal].IsClaimed()) {
    struct sigaction saved_action = user_sigactions[signal].GetAction();
    if (new_action != NULL) {
      user_sigactions[signal].SetAction(*new_action);
    }
    if (old_action != NULL) {
      *old_action = saved_action;
    }
    return 0;
  }

  // Will only get here if the signal chain has not been claimed.  We want
  // to pass the sigaction on to the kernel via the real sigaction in libc.

  if (linked_sigaction_sym == nullptr) {
    // Perform lazy initialization.
    // This will only occur outside of a signal context since we have
    // not been initialized and therefore cannot be within the ART
    // runtime.
    InitializeSignalChain();
  }

  if (linked_sigaction_sym == nullptr) {
    log("Unable to find next sigaction in signal chain");
    abort();
  }
  SigActionFnPtr linked_sigaction = reinterpret_cast<SigActionFnPtr>(linked_sigaction_sym);
  return linked_sigaction(signal, new_action, old_action);
}

extern "C" int sigprocmask(int how, const sigset_t* bionic_new_set, sigset_t* bionic_old_set) {
  const sigset_t* new_set_ptr = bionic_new_set;
  sigset_t tmpset;
  if (bionic_new_set != NULL) {
    tmpset = *bionic_new_set;

    if (how == SIG_BLOCK) {
      // Don't allow claimed signals in the mask.  If a signal chain has been claimed
      // we can't allow the user to block that signal.
      for (int i = 0 ; i < _NSIG; ++i) {
        if (user_sigactions[i].IsClaimed() && sigismember(&tmpset, i)) {
            sigdelset(&tmpset, i);
        }
      }
    }
    new_set_ptr = &tmpset;
  }

  if (linked_sigprocmask_sym == nullptr) {
    // Perform lazy initialization.
    InitializeSignalChain();
  }

  if (linked_sigprocmask_sym == nullptr) {
    log("Unable to find next sigprocmask in signal chain");
    abort();
  }

  typedef int (*SigProcMask)(int how, const sigset_t*, sigset_t*);
  SigProcMask linked_sigprocmask= reinterpret_cast<SigProcMask>(linked_sigprocmask_sym);
  return linked_sigprocmask(how, new_set_ptr, bionic_old_set);
}

extern "C" void InitializeSignalChain() {
  // Warning.
  // Don't call this from within a signal context as it makes calls to
  // dlsym.  Calling into the dynamic linker will result in locks being
  // taken and if it so happens that a signal occurs while one of these
  // locks is already taken, dlsym will block trying to reenter a
  // mutex and we will never get out of it.
  if (initialized) {
    // Don't initialize twice.
    return;
  }
  linked_sigaction_sym = dlsym(RTLD_NEXT, "sigaction");
  if (linked_sigaction_sym == nullptr) {
    linked_sigaction_sym = dlsym(RTLD_DEFAULT, "sigaction");
    if (linked_sigaction_sym == nullptr ||
      linked_sigaction_sym == reinterpret_cast<void*>(sigaction)) {
        linked_sigaction_sym = nullptr;
    }
  }

  linked_sigprocmask_sym = dlsym(RTLD_NEXT, "sigprocmask");
  if (linked_sigprocmask_sym == nullptr) {
    linked_sigprocmask_sym = dlsym(RTLD_DEFAULT, "sigprocmask");
    if (linked_sigprocmask_sym == nullptr ||
        linked_sigprocmask_sym == reinterpret_cast<void*>(sigprocmask)) {
         linked_sigprocmask_sym = nullptr;
    }
  }
  initialized = true;
}
}   // namespace art

