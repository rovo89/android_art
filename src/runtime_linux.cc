// Copyright 2011 Google Inc. All Rights Reserved.
// Author: enh@google.com (Elliott Hughes)

#include "runtime.h"

#include <execinfo.h>

#include "logging.h"
#include "scoped_ptr.h"
#include "stringprintf.h"

namespace art {

void Runtime::PlatformAbort(const char* file, int line) {
  // On the host, we don't have debuggerd to dump a stack for us.

  // Get the raw stack frames.
  size_t MAX_STACK_FRAMES = 64;
  void* frames[MAX_STACK_FRAMES];
  size_t frame_count = backtrace(frames, MAX_STACK_FRAMES);

  // Turn them into something human-readable with symbols.
  // TODO: in practice, we may find that we should use backtrace_symbols_fd
  // to avoid allocation, rather than use our own custom formatting.
  scoped_ptr_malloc<char*> symbols(backtrace_symbols(frames, frame_count));
  if (symbols == NULL) {
    PLOG(ERROR) << "backtrace_symbols failed";
    return;
  }

  for (size_t i = 0; i < frame_count; ++i) {
    LogMessage(file, line, ERROR, -1).stream()
        << StringPrintf("\t#%02d %s", i, symbols.get()[i]);
  }
}

}  // namespace art
