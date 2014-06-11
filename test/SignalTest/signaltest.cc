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

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "jni.h"

#ifdef __arm__
#include <sys/ucontext.h>
#endif

static void signalhandler(int sig, siginfo_t* info, void* context) {
  printf("signal caught\n");
#ifdef __arm__
  // On ARM we do a more exhaustive test to make sure the signal
  // context is OK.
  // We can do this because we know that the instruction causing
  // the signal is 2 bytes long (thumb mov instruction).  On
  // other architectures this is more difficult.
  // TODO: we could do this on other architectures too if necessary, it's just harder.
  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  sc->arm_pc += 2;          // Skip instruction causing segv.
#endif
}

static struct sigaction oldaction;

extern "C" JNIEXPORT void JNICALL Java_SignalTest_initSignalTest(JNIEnv*, jclass) {
  struct sigaction action;
  action.sa_sigaction = signalhandler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
#if !defined(__APPLE__) && !defined(__mips__)
  action.sa_restorer = nullptr;
#endif

  sigaction(SIGSEGV, &action, &oldaction);
}

extern "C" JNIEXPORT void JNICALL Java_SignalTest_terminateSignalTest(JNIEnv*, jclass) {
  sigaction(SIGSEGV, &oldaction, nullptr);
}

// Prevent the compiler being a smart-alec and optimizing out the assignment
// to nullptr.
char *p = nullptr;

extern "C" JNIEXPORT jint JNICALL Java_SignalTest_testSignal(JNIEnv*, jclass) {
#ifdef __arm__
  // On ARM we cause a real SEGV.
  *p = 'a';
#else
  // On other architectures we simulate SEGV.
  kill(getpid(), SIGSEGV);
#endif
  return 1234;
}

