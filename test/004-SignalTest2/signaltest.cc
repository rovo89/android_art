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
#include <stdlib.h>
#include <unistd.h>

#include "jni.h"

#include <sys/ucontext.h>

static int signal_count;
static const int kMaxSignal = 2;

static void signalhandler(int sig) {
  printf("signal caught\n");
  ++signal_count;
  if (signal_count > kMaxSignal) {
     printf("too many signals\n");
     abort();
  }
  printf("Signal test OK\n");
  exit(0);
}

sighandler_t oldsignal;

extern "C" JNIEXPORT void JNICALL Java_Main_initSignalTest2(JNIEnv*, jclass) {
  oldsignal = signal(SIGSEGV, reinterpret_cast<sighandler_t>(signalhandler));
}

// Prevent the compiler being a smart-alec and optimizing out the assignment
// to nullptr.
char *go_away_compiler2 = nullptr;

extern "C" JNIEXPORT void JNICALL Java_Main_testSignal2(JNIEnv*, jclass) {
  *go_away_compiler2 = 'a';
}

