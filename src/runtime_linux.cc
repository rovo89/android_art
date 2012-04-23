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
#include <string.h>

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

  return mangled_name;
}

struct Backtrace {
  void Dump(std::ostream& os) {
    // Get the raw stack frames.
    size_t MAX_STACK_FRAMES = 128;
    void* frames[MAX_STACK_FRAMES];
    size_t frame_count = backtrace(frames, MAX_STACK_FRAMES);
    if (frame_count == 0) {
      os << "--- backtrace(3) returned no frames";
      return;
    }

    // Turn them into something human-readable with symbols.
    char** symbols = backtrace_symbols(frames, frame_count);
    if (symbols == NULL) {
      os << "--- backtrace_symbols(3) failed";
      return;
    }


    // Parse the backtrace strings and demangle, so we can produce output like this:
    // ]    #00 art::Runtime::Abort(char const*, int)+0x15b [0xf770dd51] (libartd.so)
    for (size_t i = 0; i < frame_count; ++i) {
      std::string text(symbols[i]);
      std::string filename("???");
      std::string function_name;

#if defined(__APPLE__)
      // backtrace_symbols(3) gives us lines like this on Mac OS:
      // "0   libartd.dylib                       0x001cd29a _ZN3art9Backtrace4DumpERSo + 40>"
      // "3   ???                                 0xffffffff 0x0 + 4294967295>"
      text.erase(0, 4);
      size_t index = text.find(' ');
      filename = text.substr(0, index);
      text.erase(0, 40 - 4);
      index = text.find(' ');
      std::string address(text.substr(0, index));
      text.erase(0, index + 1);
      index = text.find(' ');
      function_name = Demangle(text.substr(0, index));
      text.erase(0, index);
      text += " [" + address + "]";
#else
      // backtrace_symbols(3) gives us lines like this on Linux:
      // "/usr/local/google/home/enh/a1/out/host/linux-x86/bin/../lib/libartd.so(_ZN3art7Runtime5AbortEPKci+0x15b) [0xf76c5af3]"
      // "[0xf7b62057]"
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
#endif

      const char* last_slash = strrchr(filename.c_str(), '/');
      const char* so_name = (last_slash == NULL) ? filename.c_str() : last_slash + 1;
      os << StringPrintf("\t#%02zd ", i) << function_name << text << " (" << so_name << ")\n";
    }

    free(symbols);
  }
};

static const char* GetSignalName(int signal_number) {
  switch (signal_number) {
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGPIPE: return "SIGPIPE";
    case SIGSEGV: return "SIGSEGV";
#if defined(STIGSTLFKT)
    case SIGSTKFLT: return "SIGSTKFLT";
#endif
    case SIGTRAP: return "SIGTRAP";
  }
  return "??";
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

struct UContext {
  UContext(void* raw_context) : context(reinterpret_cast<ucontext_t*>(raw_context)->uc_mcontext) {}

  void Dump(std::ostream& os) {
    // TODO: support non-x86 hosts (not urgent because this code doesn't run on targets).
#if defined(__APPLE__)
    DumpRegister32(os, "eax", context->__ss.__eax);
    DumpRegister32(os, "ebx", context->__ss.__ebx);
    DumpRegister32(os, "ecx", context->__ss.__ecx);
    DumpRegister32(os, "edx", context->__ss.__edx);
    os << '\n';

    DumpRegister32(os, "edi", context->__ss.__edi);
    DumpRegister32(os, "esi", context->__ss.__esi);
    DumpRegister32(os, "ebp", context->__ss.__ebp);
    DumpRegister32(os, "esp", context->__ss.__esp);
    os << '\n';

    DumpRegister32(os, "eip", context->__ss.__eip);
    DumpRegister32(os, "eflags", context->__ss.__eflags);
    os << '\n';

    DumpRegister32(os, "cs",  context->__ss.__cs);
    DumpRegister32(os, "ds",  context->__ss.__ds);
    DumpRegister32(os, "es",  context->__ss.__es);
    DumpRegister32(os, "fs",  context->__ss.__fs);
    os << '\n';
    DumpRegister32(os, "gs",  context->__ss.__gs);
    DumpRegister32(os, "ss",  context->__ss.__ss);
#else
    DumpRegister32(os, "eax", context.gregs[REG_EAX]);
    DumpRegister32(os, "ebx", context.gregs[REG_EBX]);
    DumpRegister32(os, "ecx", context.gregs[REG_ECX]);
    DumpRegister32(os, "edx", context.gregs[REG_EDX]);
    os << '\n';

    DumpRegister32(os, "edi", context.gregs[REG_EDI]);
    DumpRegister32(os, "esi", context.gregs[REG_ESI]);
    DumpRegister32(os, "ebp", context.gregs[REG_EBP]);
    DumpRegister32(os, "esp", context.gregs[REG_ESP]);
    os << '\n';

    DumpRegister32(os, "eip", context.gregs[REG_EIP]);
    DumpRegister32(os, "eflags", context.gregs[REG_EFL]);
    os << '\n';

    DumpRegister32(os, "cs",  context.gregs[REG_CS]);
    DumpRegister32(os, "ds",  context.gregs[REG_DS]);
    DumpRegister32(os, "es",  context.gregs[REG_ES]);
    DumpRegister32(os, "fs",  context.gregs[REG_FS]);
    os << '\n';
    DumpRegister32(os, "gs",  context.gregs[REG_GS]);
    DumpRegister32(os, "ss",  context.gregs[REG_SS]);
#endif
  }

  void DumpRegister32(std::ostream& os, const char* name, uint32_t value) {
    os << StringPrintf(" %6s: 0x%08x", name, value);
  }

  mcontext_t& context;
};

static void HandleUnexpectedSignal(int signal_number, siginfo_t* info, void* raw_context) {
  bool has_address = (signal_number == SIGILL || signal_number == SIGBUS ||
                      signal_number == SIGFPE || signal_number == SIGSEGV);

  UContext thread_context(raw_context);
  Backtrace thread_backtrace;

  LOG(INTERNAL_FATAL) << "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n"
                      << StringPrintf("Fatal signal %d (%s), code %d (%s)",
                                      signal_number, GetSignalName(signal_number),
                                      info->si_code,
                                      GetSignalCodeName(signal_number, info->si_code))
                      << (has_address ? StringPrintf(" fault addr %p", info->si_addr) : "") << "\n"
                      << "Registers:\n" << Dumpable<UContext>(thread_context) << "\n"
                      << "Backtrace:\n" << Dumpable<Backtrace>(thread_backtrace);

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

void Runtime::InitPlatformSignalHandlers() {
  // On the host, we don't have debuggerd to dump a stack for us when something unexpected happens.
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  action.sa_sigaction = HandleUnexpectedSignal;
  action.sa_flags = SA_RESTART;
  // Use the three-argument sa_sigaction handler.
  action.sa_flags |= SA_SIGINFO;
  // Remove ourselves as signal handler for this signal, in case of recursion.
  action.sa_flags |= SA_RESETHAND;

  int rc = 0;
  rc += sigaction(SIGILL, &action, NULL);
  rc += sigaction(SIGTRAP, &action, NULL);
  rc += sigaction(SIGABRT, &action, NULL);
  rc += sigaction(SIGBUS, &action, NULL);
  rc += sigaction(SIGFPE, &action, NULL);
#if defined(SIGSTKFLT)
  rc += sigaction(SIGSTKFLT, &action, NULL);
#endif
  rc += sigaction(SIGPIPE, &action, NULL);

  // Use the alternate signal stack so we can catch stack overflows.
  // On Mac OS 10.7, backtrace(3) is broken and will return no frames when called from the alternate stack,
  // so we only use the alternate stack for SIGSEGV so that we at least get backtraces for other signals.
  // (glibc does the right thing, so we could use the alternate stack for all signals there.)
  action.sa_flags |= SA_ONSTACK;
  rc += sigaction(SIGSEGV, &action, NULL);

  CHECK_EQ(rc, 0);
}

}  // namespace art
