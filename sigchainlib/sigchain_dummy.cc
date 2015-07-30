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

#include <stdio.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <android/log.h>
#else
#include <stdarg.h>
#include <iostream>
#endif

#include "sigchain.h"

#define ATTRIBUTE_UNUSED __attribute__((__unused__))

// We cannot annotate the declarations, as they are not no-return in the non-dummy version.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"

static void log(const char* format, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
#ifdef __ANDROID__
  __android_log_write(ANDROID_LOG_ERROR, "libsigchain", buf);
#else
  std::cout << buf << "\n";
#endif
  va_end(ap);
}

namespace art {


extern "C" void ClaimSignalChain(int signal ATTRIBUTE_UNUSED,
                                 struct sigaction* oldaction ATTRIBUTE_UNUSED) {
  log("ClaimSignalChain is not exported by the main executable.");
  abort();
}

extern "C" void UnclaimSignalChain(int signal ATTRIBUTE_UNUSED) {
  log("UnclaimSignalChain is not exported by the main executable.");
  abort();
}

extern "C" void InvokeUserSignalHandler(int sig ATTRIBUTE_UNUSED,
                                        siginfo_t* info ATTRIBUTE_UNUSED,
                                        void* context ATTRIBUTE_UNUSED) {
  log("InvokeUserSignalHandler is not exported by the main executable.");
  abort();
}

extern "C" void InitializeSignalChain() {
  log("InitializeSignalChain is not exported by the main executable.");
  abort();
}

extern "C" void EnsureFrontOfChain(int signal ATTRIBUTE_UNUSED,
                                   struct sigaction* expected_action ATTRIBUTE_UNUSED) {
  log("EnsureFrontOfChain is not exported by the main executable.");
  abort();
}

extern "C" void SetSpecialSignalHandlerFn(int signal ATTRIBUTE_UNUSED,
                                          SpecialSignalHandlerFn fn ATTRIBUTE_UNUSED) {
  log("SetSpecialSignalHandlerFn is not exported by the main executable.");
  abort();
}

#pragma GCC diagnostic pop

}  // namespace art
