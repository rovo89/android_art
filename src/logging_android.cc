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

#include "logging.h"

#include <iostream>
#include <unistd.h>

#include "cutils/log.h"

namespace art {

static const int kLogSeverityToAndroidLogPriority[] = {
  ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
  ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL
};

LogMessage::LogMessage(const char* file, int line, LogSeverity severity, int error)
    : file_(file), line_number_(line), severity_(severity), errno_(error) {
  const char* last_slash = strrchr(file, '/');
  file_ = (last_slash == NULL) ? file : last_slash + 1;
}

void LogMessage::LogLine(const char* line) {
  int priority = kLogSeverityToAndroidLogPriority[severity_];
  LOG_PRI(priority, LOG_TAG, "%s:%d] %s", file_, line_number_, line);
}

}  // namespace art
