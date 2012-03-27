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

LogVerbosity gLogVerbosity;

static Mutex& GetLoggingLock() {
  static Mutex logging_lock("LogMessage lock");
  return logging_lock;
}

LogMessage::~LogMessage() {
  // Finish constructing the message.
  if (data_->error != -1) {
    data_->buffer << ": " << strerror(data_->error);
  }
  std::string msg(data_->buffer.str());

  // Do the actual logging with the lock held.
  {
    MutexLock mu(GetLoggingLock());
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
    Runtime::Abort(data_->file, data_->line_number);
  }

  delete data_;
}

std::ostream& LogMessage::stream() {
  return data_->buffer;
}

/*
 * Print a hex dump in this format:
 *
 * 01234567: 00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff  0123456789abcdef
 *
 * Does not use printf() or other string-formatting calls.
 */
void HexDump(const void* address, size_t byte_count, bool show_actual_address) {
  static const char gHexDigit[] = "0123456789abcdef";
  const unsigned char* addr = reinterpret_cast<const unsigned char*>(address);
  char out[76];           /* exact fit */
  unsigned int offset;    /* offset to show while printing */

  if (show_actual_address) {
    offset = reinterpret_cast<int>(addr);
  } else {
    offset = 0;
  }
  memset(out, ' ', sizeof(out)-1);
  out[8] = ':';
  out[sizeof(out)-1] = '\0';

  int gap = static_cast<int>(offset & 0x0f);
  while (byte_count) {
    unsigned int lineOffset = offset & ~0x0f;

    char* hex = out;
    char* asc = out + 59;

    for (int i = 0; i < 8; i++) {
      *hex++ = gHexDigit[lineOffset >> 28];
      lineOffset <<= 4;
    }
    hex++;
    hex++;

    int count = std::min(static_cast<int>(byte_count), 16 - gap);
    CHECK_NE(count, 0);
    CHECK_LE(count + gap, 16);

    if (gap) {
      /* only on first line */
      hex += gap * 3;
      asc += gap;
    }

    int i;
    for (i = gap ; i < count+gap; i++) {
      *hex++ = gHexDigit[*addr >> 4];
      *hex++ = gHexDigit[*addr & 0x0f];
      hex++;
      if (*addr >= 0x20 && *addr < 0x7f /*isprint(*addr)*/) {
        *asc++ = *addr;
      } else {
        *asc++ = '.';
      }
      addr++;
    }
    for (; i < 16; i++) {
      /* erase extra stuff; only happens on last line */
      *hex++ = ' ';
      *hex++ = ' ';
      hex++;
      *asc++ = ' ';
    }

    LOG(INFO) << out;

    gap = 0;
    byte_count -= count;
    offset += count;
  }
}

}  // namespace art
