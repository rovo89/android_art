// Copyright 2011 Google Inc. All Rights Reserved.
// Author: enh@google.com (Elliott Hughes)

#include "logging.h"

#include "runtime.h"
#include "scoped_ptr.h"
#include "stringprintf.h"

#include <cstdio>
#include <cstring>
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

LogMessage::LogMessage(const char* file, int line, LogSeverity severity, int error)
: line_(line), severity_(severity), errno_(error)
{
  const char* last_slash = strrchr(file, '/');
  file_ = (last_slash == NULL) ? file : last_slash + 1;
  stream() << StringPrintf("%c %5d %5d %s:%d] ",
      "IWEF"[severity], getpid(), gettid(), file_, line);
}

LogMessage::~LogMessage() {
  if (errno_ != -1) {
    stream() << ": " << strerror(errno_);
  }
  stream() << std::endl;
  if (severity_ == FATAL) {
    art::Runtime::Abort(file_, line_);
  }
}

std::ostream& LogMessage::stream() {
  return std::cerr;
}
