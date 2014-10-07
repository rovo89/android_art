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

#include <stdlib.h>
#include <stdio.h>

#include "sigchain.h"

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

extern "C" void ClaimSignalChain(int signal, struct sigaction* oldaction) {
  log("ClaimSignalChain is not exported by the main executable.");
  abort();
}

extern "C" void EnsureFrontOfChain(int signal, struct sigaction* expected_action) {
  log("EnsureFrontOfChain is not exported by the main executable.");
  abort();
}

extern "C" void UnclaimSignalChain(int signal) {
  log("UnclaimSignalChain is not exported by the main executable.");
  abort();
}

extern "C" void InvokeUserSignalHandler(int sig, siginfo_t* info, void* context) {
  log("InvokeUserSignalHandler is not exported by the main executable.");
  abort();
}

extern "C" void InitializeSignalChain() {
  log("InitializeSignalChain is not exported by the main executable.");
  abort();
}
