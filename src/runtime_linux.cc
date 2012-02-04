/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "runtime.h"

#include <cxxabi.h>
#include <execinfo.h>

#include "logging.h"
#include "stringprintf.h"

namespace art {

std::string Demangle(const std::string& mangled_name) {
  if (mangled_name.empty()) {
    return "??";
  }

  // http://gcc.gnu.org/onlinedocs/libstdc++/manual/ext_demangling.html
  int status;
  char* name(abi::__cxa_demangle(mangled_name.c_str(), NULL, NULL, &status));
  if (name != NULL) {
    std::string result(name);
    free(name);
    return result;
  }

  return mangled_name + "()";
}

void Runtime::PlatformAbort(const char* file, int line) {
  // On the host, we don't have debuggerd to dump a stack for us.

  // Get the raw stack frames.
  size_t MAX_STACK_FRAMES = 64;
  void* frames[MAX_STACK_FRAMES];
  size_t frame_count = backtrace(frames, MAX_STACK_FRAMES);

  // Turn them into something human-readable with symbols.
  char** symbols = backtrace_symbols(frames, frame_count);
  if (symbols == NULL) {
    PLOG(ERROR) << "backtrace_symbols failed";
    return;
  }

  // backtrace_symbols(3) gives us lines like this:
  // "/usr/local/google/home/enh/a1/out/host/linux-x86/bin/../lib/libartd.so(_ZN3art7Runtime13PlatformAbortEPKci+0x15b) [0xf76c5af3]"
  // "[0xf7b62057]"

  // We extract the pieces and demangle, so we can produce output like this:
  // libartd.so:-1]    #00 art::Runtime::PlatformAbort(char const*, int) +0x15b [0xf770dd51]

  for (size_t i = 0; i < frame_count; ++i) {
    std::string text(symbols[i]);
    std::string filename("??");
    std::string function_name;

    size_t index = text.find('(');
    if (index != std::string::npos) {
      filename = text.substr(0, index);
      text.erase(0, index + 1);

      index = text.find_first_of("+)");
      function_name = Demangle(text.substr(0, index));
      text.erase(0, index);
      index = text.find(')');
      text.erase(index, 1);
    }
    std::string log_line(StringPrintf("\t#%02zd ", i) + function_name + text);
    LogMessage(filename.c_str(), -1, INTERNAL_FATAL, -1).stream() << log_line;
  }

  free(symbols);
}

}  // namespace art
