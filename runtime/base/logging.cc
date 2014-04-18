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

#include "base/mutex.h"
#include "runtime.h"
#include "thread-inl.h"
#include "UniquePtr.h"
#include "utils.h"

namespace art {

LogVerbosity gLogVerbosity;

std::vector<std::string> gVerboseMethods;

unsigned int gAborting = 0;

static LogSeverity gMinimumLogSeverity = INFO;
static UniquePtr<std::string> gCmdLine;
static UniquePtr<std::string> gProgramInvocationName;
static UniquePtr<std::string> gProgramInvocationShortName;

const char* GetCmdLine() {
  return (gCmdLine.get() != nullptr) ? gCmdLine->c_str() : nullptr;
}

const char* ProgramInvocationName() {
  return (gProgramInvocationName.get() != nullptr) ? gProgramInvocationName->c_str() : "art";
}

const char* ProgramInvocationShortName() {
  return (gProgramInvocationShortName.get() != nullptr) ? gProgramInvocationShortName->c_str()
                                                        : "art";
}

// Configure logging based on ANDROID_LOG_TAGS environment variable.
// We need to parse a string that looks like
//
//      *:v jdwp:d dalvikvm:d dalvikvm-gc:i dalvikvmi:i
//
// The tag (or '*' for the global level) comes first, followed by a colon
// and a letter indicating the minimum priority level we're expected to log.
// This can be used to reveal or conceal logs with specific tags.
void InitLogging(char* argv[]) {
  if (gCmdLine.get() != nullptr) {
    return;
  }
  // TODO: Move this to a more obvious InitART...
  Locks::Init();

  // Stash the command line for later use. We can use /proc/self/cmdline on Linux to recover this,
  // but we don't have that luxury on the Mac, and there are a couple of argv[0] variants that are
  // commonly used.
  if (argv != NULL) {
    gCmdLine.reset(new std::string(argv[0]));
    for (size_t i = 1; argv[i] != NULL; ++i) {
      gCmdLine->append(" ");
      gCmdLine->append(argv[i]);
    }
    gProgramInvocationName.reset(new std::string(argv[0]));
    const char* last_slash = strrchr(argv[0], '/');
    gProgramInvocationShortName.reset(new std::string((last_slash != NULL) ? last_slash + 1
                                                                           : argv[0]));
  } else {
    // TODO: fall back to /proc/self/cmdline when argv is NULL on Linux
    gCmdLine.reset(new std::string("<unset>"));
  }
  const char* tags = getenv("ANDROID_LOG_TAGS");
  if (tags == NULL) {
    return;
  }

  std::vector<std::string> specs;
  Split(tags, ' ', specs);
  for (size_t i = 0; i < specs.size(); ++i) {
    // "tag-pattern:[vdiwefs]"
    std::string spec(specs[i]);
    if (spec.size() == 3 && StartsWith(spec, "*:")) {
      switch (spec[2]) {
        case 'v':
          gMinimumLogSeverity = VERBOSE;
          continue;
        case 'd':
          gMinimumLogSeverity = DEBUG;
          continue;
        case 'i':
          gMinimumLogSeverity = INFO;
          continue;
        case 'w':
          gMinimumLogSeverity = WARNING;
          continue;
        case 'e':
          gMinimumLogSeverity = ERROR;
          continue;
        case 'f':
          gMinimumLogSeverity = FATAL;
          continue;
        // liblog will even suppress FATAL if you say 's' for silent, but that's crazy!
        case 's':
          gMinimumLogSeverity = FATAL;
          continue;
      }
    }
    LOG(FATAL) << "unsupported '" << spec << "' in ANDROID_LOG_TAGS (" << tags << ")";
  }
}

LogMessageData::LogMessageData(const char* file, int line, LogSeverity severity, int error)
    : file(file),
      line_number(line),
      severity(severity),
      error(error) {
  const char* last_slash = strrchr(file, '/');
  file = (last_slash == NULL) ? file : last_slash + 1;
}

LogMessage::~LogMessage() {
  if (data_->severity < gMinimumLogSeverity) {
    return;  // No need to format something we're not going to output.
  }

  // Finish constructing the message.
  if (data_->error != -1) {
    data_->buffer << ": " << strerror(data_->error);
  }
  std::string msg(data_->buffer.str());

  // Do the actual logging with the lock held.
  {
    MutexLock mu(Thread::Current(), *Locks::logging_lock_);
    if (msg.find('\n') == std::string::npos) {
      LogLine(*data_, msg.c_str());
    } else {
      msg += '\n';
      size_t i = 0;
      while (i < msg.size()) {
        size_t nl = msg.find('\n', i);
        msg[nl] = '\0';
        LogLine(*data_, &msg[i]);
        i = nl + 1;
      }
    }
  }

  // Abort if necessary.
  if (data_->severity == FATAL) {
    Runtime::Abort();
  }
}

}  // namespace art
