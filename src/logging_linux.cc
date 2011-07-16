// Copyright 2011 Google Inc. All Rights Reserved.
// Author: enh@google.com (Elliott Hughes)

#include "logging.h"

#include "scoped_ptr.h"
#include "stringprintf.h"

#include <cstdio>
#include <cstring>
#include <execinfo.h>
#include <iostream>
#include <sys/types.h>
#include <unistd.h>

// glibc doesn't expose gettid(2).
#define __KERNEL__
# include <linux/unistd.h>
#ifdef _syscall0
_syscall0(pid_t,gettid)
#else
pid_t gettid() { return syscall(__NR_gettid);}
#endif
#undef __KERNEL__

static void dumpStackTrace(std::ostream& os) {
  // Get the raw stack frames.
  size_t MAX_STACK_FRAMES = 64;
  void* stack_frames[MAX_STACK_FRAMES];
  size_t frame_count = backtrace(stack_frames, MAX_STACK_FRAMES);

  // Turn them into something human-readable with symbols.
  // TODO: in practice, we may find that we should use backtrace_symbols_fd
  // to avoid allocation, rather than use our own custom formatting.
  art::scoped_ptr_malloc<char*> strings(backtrace_symbols(stack_frames, frame_count));
  if (strings.get() == NULL) {
    os << "backtrace_symbols failed: " << strerror(errno) << std::endl;
    return;
  }

  for (size_t i = 0; i < frame_count; ++i) {
    os << StringPrintf("\t#%02d %s", i, strings.get()[i]) << std::endl;
  }
}

LogMessage::LogMessage(const char* file, int line, LogSeverity severity, int error)
: severity_(severity), errno_(error)
{
  const char* last_slash = strrchr(file, '/');
  const char* leaf = (last_slash == NULL) ? file : last_slash + 1;
  stream() << StringPrintf("%c %5d %5d %s:%d] ",
    "IWEF"[severity], getpid(), gettid(), leaf, line);
}

LogMessage::~LogMessage() {
  if (errno_ != -1) {
    stream() << ": " << strerror(errno);
  }
  stream() << std::endl;
  if (severity_ == FATAL) {
    stream() << "Aborting:" << std::endl;
    dumpStackTrace(stream());
    abort();
  }
}

std::ostream& LogMessage::stream() {
  return std::cerr;
}
