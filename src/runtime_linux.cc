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
#include <signal.h>

#include "logging.h"
#include "stringprintf.h"

namespace art {

static std::string Demangle(const std::string& mangled_name) {
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

static void Backtrace() {
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

static const char* GetSignalCodeName(int signal_number, int signal_code) {
  // Try the signal-specific codes...
  switch (signal_number) {
    case SIGILL:
      switch (signal_code) {
        case ILL_ILLOPC: return "ILL_ILLOPC";
        case ILL_ILLOPN: return "ILL_ILLOPN";
        case ILL_ILLADR: return "ILL_ILLADR";
        case ILL_ILLTRP: return "ILL_ILLTRP";
        case ILL_PRVOPC: return "ILL_PRVOPC";
        case ILL_PRVREG: return "ILL_PRVREG";
        case ILL_COPROC: return "ILL_COPROC";
        case ILL_BADSTK: return "ILL_BADSTK";
      }
      break;
    case SIGBUS:
      switch (signal_code) {
        case BUS_ADRALN: return "BUS_ADRALN";
        case BUS_ADRERR: return "BUS_ADRERR";
        case BUS_OBJERR: return "BUS_OBJERR";
      }
      break;
    case SIGFPE:
      switch (signal_code) {
        case FPE_INTDIV: return "FPE_INTDIV";
        case FPE_INTOVF: return "FPE_INTOVF";
        case FPE_FLTDIV: return "FPE_FLTDIV";
        case FPE_FLTOVF: return "FPE_FLTOVF";
        case FPE_FLTUND: return "FPE_FLTUND";
        case FPE_FLTRES: return "FPE_FLTRES";
        case FPE_FLTINV: return "FPE_FLTINV";
        case FPE_FLTSUB: return "FPE_FLTSUB";
      }
      break;
    case SIGSEGV:
      switch (signal_code) {
        case SEGV_MAPERR: return "SEGV_MAPERR";
        case SEGV_ACCERR: return "SEGV_ACCERR";
      }
      break;
    case SIGTRAP:
      switch (signal_code) {
        case TRAP_BRKPT: return "TRAP_BRKPT";
        case TRAP_TRACE: return "TRAP_TRACE";
      }
      break;
  }
  // Then the other codes...
  switch (signal_code) {
    case SI_USER:     return "SI_USER";
#if defined(SI_KERNEL)
    case SI_KERNEL:   return "SI_KERNEL";
#endif
    case SI_QUEUE:    return "SI_QUEUE";
    case SI_TIMER:    return "SI_TIMER";
    case SI_MESGQ:    return "SI_MESGQ";
    case SI_ASYNCIO:  return "SI_ASYNCIO";
#if defined(SI_SIGIO)
    case SI_SIGIO:    return "SI_SIGIO";
#endif
#if defined(SI_TKILL)
    case SI_TKILL:    return "SI_TKILL";
#endif
  }
  // Then give up...
  return "?";
}

static void HandleUnexpectedSignal(int signal_number, siginfo_t* info, void*) {
  const char* signal_name = "?";
  bool has_address = false;
  if (signal_number == SIGILL) {
    signal_name = "SIGILL";
    has_address = true;
  } else if (signal_number == SIGTRAP) {
    signal_name = "SIGTRAP";
  } else if (signal_number == SIGABRT) {
    signal_name = "SIGABRT";
  } else if (signal_number == SIGBUS) {
    signal_name = "SIGBUS";
    has_address = true;
  } else if (signal_number == SIGFPE) {
    signal_name = "SIGFPE";
    has_address = true;
  } else if (signal_number == SIGSEGV) {
    signal_name = "SIGSEGV";
    has_address = true;
#if defined(SIGSTKFLT)
  } else if (signal_number == SIGSTKFLT) {
    signal_name = "SIGSTKFLT";
#endif
  } else if (signal_number == SIGPIPE) {
    signal_name = "SIGPIPE";
  }

  LOG(INTERNAL_FATAL) << "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n"
                      << StringPrintf("Fatal signal %d (%s), code %d (%s)",
                                      signal_number, signal_name,
                                      info->si_code,
                                      GetSignalCodeName(signal_number, info->si_code))
                      << (has_address ? StringPrintf(" fault addr %p", info->si_addr) : "");
  Backtrace();

  // TODO: instead, get debuggerd running on the host, try to connect, and hang around on success.
  if (getenv("debug_db_uid") != NULL) {
    LOG(INTERNAL_FATAL) << "********************************************************\n"
                        << "* Process " << getpid() << " has been suspended while crashing. Attach gdb:\n"
                        << "*     gdb -p " << getpid() << "\n"
                        << "********************************************************\n";
    // Wait for debugger to attach.
    while (true) {
    }
  }
}

void Runtime::PlatformAbort(const char* /*file*/, int /*line_number*/) {
  // On the host, we don't have debuggerd to dump a stack for us when we LOG(FATAL).
  Backtrace();
}

void Runtime::InitPlatformSignalHandlers() {
  // On the host, we don't have debuggerd to dump a stack for us when something unexpected happens.
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  action.sa_sigaction = HandleUnexpectedSignal;
  action.sa_flags = SA_RESTART | SA_SIGINFO | SA_ONSTACK;

  int rc = 0;
  rc += sigaction(SIGILL, &action, NULL);
  rc += sigaction(SIGTRAP, &action, NULL);
  rc += sigaction(SIGABRT, &action, NULL);
  rc += sigaction(SIGBUS, &action, NULL);
  rc += sigaction(SIGFPE, &action, NULL);
  rc += sigaction(SIGSEGV, &action, NULL);
#if defined(SIGSTKFLT)
  rc += sigaction(SIGSTKFLT, &action, NULL);
#endif
  rc += sigaction(SIGPIPE, &action, NULL);
  CHECK_EQ(rc, 0);
}

}  // namespace art
