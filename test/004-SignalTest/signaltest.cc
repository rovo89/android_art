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

#include <jni.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "base/macros.h"

static int signal_count;
static const int kMaxSignal = 2;

#if defined(__i386__) || defined(__x86_64__)
#if defined(__APPLE__)
#define ucontext __darwin_ucontext

#if defined(__x86_64__)
// 64 bit mac build.
#define CTX_EIP uc_mcontext->__ss.__rip
#else
// 32 bit mac build.
#define CTX_EIP uc_mcontext->__ss.__eip
#endif

#elif defined(__x86_64__)
// 64 bit linux build.
#define CTX_EIP uc_mcontext.gregs[REG_RIP]
#else
// 32 bit linux build.
#define CTX_EIP uc_mcontext.gregs[REG_EIP]
#endif
#endif

static void signalhandler(int sig ATTRIBUTE_UNUSED, siginfo_t* info ATTRIBUTE_UNUSED,
                          void* context) {
  printf("signal caught\n");
  ++signal_count;
  if (signal_count > kMaxSignal) {
     abort();
  }
#if defined(__arm__)
  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  sc->arm_pc += 2;          // Skip instruction causing segv.
#elif defined(__aarch64__)
  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  sc->pc += 4;          // Skip instruction causing segv.
#elif defined(__i386__)
  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  uc->CTX_EIP += 3;
#elif defined(__x86_64__)
  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  uc->CTX_EIP += 2;
#else
  UNUSED(context);
#endif
}

static struct sigaction oldaction;

extern "C" JNIEXPORT void JNICALL Java_Main_initSignalTest(JNIEnv*, jclass) {
  struct sigaction action;
  action.sa_sigaction = signalhandler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
#if !defined(__APPLE__) && !defined(__mips__)
  action.sa_restorer = nullptr;
#endif

  sigaction(SIGSEGV, &action, &oldaction);
}

extern "C" JNIEXPORT void JNICALL Java_Main_terminateSignalTest(JNIEnv*, jclass) {
  sigaction(SIGSEGV, &oldaction, nullptr);
}

// Prevent the compiler being a smart-alec and optimizing out the assignment
// to null.
char *go_away_compiler = nullptr;

extern "C" JNIEXPORT jint JNICALL Java_Main_testSignal(JNIEnv*, jclass) {
#if defined(__arm__) || defined(__i386__) || defined(__aarch64__)
  // On supported architectures we cause a real SEGV.
  *go_away_compiler = 'a';
#elif defined(__x86_64__)
  // Cause a SEGV using an instruction known to be 2 bytes long to account for hardcoded jump
  // in the signal handler
  asm volatile("movl $0, %%eax;" "movb %%ah, (%%rax);" : : : "%eax");
#else
  // On other architectures we simulate SEGV.
  kill(getpid(), SIGSEGV);
#endif
  return 1234;
}
