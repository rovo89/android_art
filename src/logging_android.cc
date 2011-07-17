// Copyright 2011 Google Inc. All Rights Reserved.
// Author: enh@google.com (Elliott Hughes)

#include "logging.h"

#include <iostream>
#include <unistd.h>

#include "cutils/log.h"
#include "runtime.h"

static const int kLogSeverityToAndroidLogPriority[] = {
  ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL
};

LogMessage::LogMessage(const char* file, int line, LogSeverity severity, int error)
: file_(file), line_(line), severity_(severity), errno_(error)
{
}

LogMessage::~LogMessage() {
  if (errno_ != -1) {
    stream() << ": " << strerror(errno_);
  }
  int priority = kLogSeverityToAndroidLogPriority[severity_];
  LOG_PRI(priority, LOG_TAG, "%s", buffer_.str().c_str());
  if (severity_ == FATAL) {
    art::Runtime::Abort(file_, line_);
  }
}

std::ostream& LogMessage::stream() {
  return buffer_;
}
