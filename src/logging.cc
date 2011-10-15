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
#include "thread.h"
#include "utils.h"

namespace art {

art::Mutex& GetLoggingLock() {
  static art::Mutex lock("LogMessage lock");
  return lock;
}

LogMessage::~LogMessage() {
  // Finish constructing the message.
  if (data_->error != -1) {
    data_->buffer << ": " << strerror(data_->error);
  }
  std::string msg(data_->buffer.str());

  // Do the actual logging with the lock held.
  {
    art::MutexLock mu(GetLoggingLock());
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
  }

  // Abort if necessary.
  if (data_->severity == FATAL) {
    art::Runtime::Abort(data_->file, data_->line_number);
  }

  delete data_;
}

std::ostream& LogMessage::stream() {
  return data_->buffer;
}

}  // namespace art
