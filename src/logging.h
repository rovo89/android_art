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

#ifndef ART_SRC_LOGGING_H_
#define ART_SRC_LOGGING_H_

#include <cerrno>
#include <cstring>
#include <iostream>  // NOLINT
#include <sstream>
#include "log_severity.h"
#include "macros.h"

#define CHECK(x) \
  if (!(x)) \
    LogMessage(__FILE__, __LINE__, FATAL, -1).stream() \
        << "Check failed: " #x << " "

#define CHECK_OP(LHS, RHS, OP) \
  do { \
    typeof (LHS) _lhs = (LHS); \
    typeof (RHS) _rhs = (RHS); \
    if (!(_lhs OP _rhs)) { \
      LogMessage(__FILE__, __LINE__, FATAL, -1).stream() \
          << "Check failed: " << #LHS << " " << #OP << " " << #RHS \
          << " (" #LHS "=" << _lhs << ", " #RHS "=" << _rhs << ")";  \
    } \
  } while (false)

#define CHECK_EQ(x, y) CHECK_OP(x, y, ==)
#define CHECK_NE(x, y) CHECK_OP(x, y, !=)
#define CHECK_LE(x, y) CHECK_OP(x, y, <=)
#define CHECK_LT(x, y) CHECK_OP(x, y, <)
#define CHECK_GE(x, y) CHECK_OP(x, y, >=)
#define CHECK_GT(x, y) CHECK_OP(x, y, >)

#define CHECK_STROP(s1, s2, sense) \
  do { \
    if ((strcmp(s1, s2) == 0) != sense) { \
      LOG(FATAL) << "Check failed: " \
                 << "\"" << s1 << "\"" \
                 << (sense ? " == " : " != ") \
                 << "\"" << s2 << "\""; \
    } \
  } while (false)

#define CHECK_STREQ(s1, s2) CHECK_STROP(s1, s2, true)
#define CHECK_STRNE(s1, s2) CHECK_STROP(s1, s2, false)

#ifndef NDEBUG

#define DCHECK(x) CHECK(x)
#define DCHECK_EQ(x, y) CHECK_EQ(x, y)
#define DCHECK_NE(x, y) CHECK_NE(x, y)
#define DCHECK_LE(x, y) CHECK_LE(x, y)
#define DCHECK_LT(x, y) CHECK_LT(x, y)
#define DCHECK_GE(x, y) CHECK_GE(x, y)
#define DCHECK_GT(x, y) CHECK_GT(x, y)
#define DCHECK_STREQ(s1, s2) CHECK_STREQ(s1, s2)
#define DCHECK_STRNE(s1, s2) CHECK_STRNE(s1, s2)

  // These require "utils.h" and only work with bionic (not glibc).
#ifdef __BIONIC__
#define DCHECK_LOCK_HELD(l) CHECK_EQ(art::GetOwner(l->GetImpl()), art::GetTid())
#define DCHECK_LOCK_NOT_HELD(l) CHECK_NE(art::GetOwner(l->GetImpl()), art::GetTid())
#else
#define DCHECK_LOCK_HELD(l)
#define DCHECK_LOCK_NOT_HELD(l)
#endif

#else  // NDEBUG

#define DCHECK_LOCK_HELD(l)
#define DCHECK_LOCK_NOT_HELD(l)

#define DCHECK(condition) \
  while (false) \
    CHECK(condition)

#define DCHECK_EQ(val1, val2) \
  while (false) \
    CHECK_EQ(val1, val2)

#define DCHECK_NE(val1, val2) \
  while (false) \
    CHECK_NE(val1, val2)

#define DCHECK_LE(val1, val2) \
  while (false) \
    CHECK_LE(val1, val2)

#define DCHECK_LT(val1, val2) \
  while (false) \
    CHECK_LT(val1, val2)

#define DCHECK_GE(val1, val2) \
  while (false) \
    CHECK_GE(val1, val2)

#define DCHECK_GT(val1, val2) \
  while (false) \
    CHECK_GT(val1, val2)

#define DCHECK_STREQ(str1, str2) \
  while (false) \
    CHECK_STREQ(str1, str2)

#define DCHECK_STRNE(str1, str2) \
  while (false) \
    CHECK_STRNE(str1, str2)

#endif

#define LOG(severity) LogMessage(__FILE__, __LINE__, severity, -1).stream()
#define PLOG(severity) LogMessage(__FILE__, __LINE__, severity, errno).stream()

#define LG LOG(INFO)

#define UNIMPLEMENTED(level) LOG(level) << __FUNCTION__ << " unimplemented "

class LogMessage {
 public:
  LogMessage(const char* file, int line, LogSeverity severity, int error);
  ~LogMessage();
  std::ostream& stream();

 private:
  void LogLine(const char*);

  std::stringstream buffer_;
  const char* file_;
  int line_number_;
  LogSeverity severity_;
  int errno_;

  DISALLOW_COPY_AND_ASSIGN(LogMessage);
};

#endif  // ART_SRC_LOGGING_H_
