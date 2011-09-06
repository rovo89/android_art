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

#include "runtime.h"
#include "utils.h"

LogMessage::~LogMessage() {
  if (errno_ != -1) {
    buffer_ << ": " << strerror(errno_);
  }
  std::string msg(buffer_.str());
  if (msg.find('\n') == std::string::npos) {
    LogLine(msg.c_str());
  } else {
    msg += '\n';
    size_t i = 0;
    while (i < msg.size()) {
      size_t nl = msg.find('\n', i);
      msg[nl] = '\0';
      LogLine(&msg[i]);
      i = nl + 1;
    }
  }

  if (severity_ == FATAL) {
    art::Runtime::Abort(file_, line_number_);
  }
}

std::ostream& LogMessage::stream() {
  return buffer_;
}
