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

#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>

#include "logging.h"
#include "stringprintf.h"
#include "utils.h"

namespace art {

LogMessage::LogMessage(const char* file, int line, LogSeverity severity, int error)
    : data_(new LogMessageData(line, severity, error)) {
  const char* last_slash = strrchr(file, '/');
  data_->file = (last_slash == NULL) ? file : last_slash + 1;
}

void LogMessage::LogLine(const char* line) {
  std::cerr << "VDIWEFF"[data_->severity] << ' '
            << StringPrintf("%5d %5d", getpid(), ::art::GetTid()) << ' '
            << data_->file << ':' << data_->line_number << "] " << line << std::endl;
}

}  // namespace art
