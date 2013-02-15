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

#ifndef ART_SRC_TIMING_LOGGER_H_
#define ART_SRC_TIMING_LOGGER_H_

#include "base/histogram.h"
#include "base/macros.h"
#include "base/mutex.h"

#include <string>
#include <vector>

namespace art {

class CumulativeLogger;

class TimingLogger {
 public:
  explicit TimingLogger(const std::string &name, bool precise);
  void AddSplit(const std::string &label);
  void Dump(std::ostream &os) const;
  void Reset();
  uint64_t GetTotalNs() const;

 protected:
  std::string name_;
  bool precise_;
  std::vector<uint64_t> times_;
  std::vector<std::string> labels_;

  friend class CumulativeLogger;
};

class CumulativeLogger {

 public:

  explicit CumulativeLogger(const std::string &name);
  void prepare_stats();
  ~CumulativeLogger();
  void Start();
  void End();
  void Reset();
  void Dump(std::ostream &os) LOCKS_EXCLUDED(lock_);
  uint64_t GetTotalNs() const;
  void SetName(const std::string &name);
  void AddLogger(const TimingLogger &logger) LOCKS_EXCLUDED(lock_);

 private:

  void AddPair(const std::string &label, uint64_t delta_time)
      EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void DumpHistogram(std::ostream &os) EXCLUSIVE_LOCKS_REQUIRED(lock_);
  uint64_t GetTotalTime() const;
  static const uint64_t kAdjust = 1000;
  std::vector<Histogram<uint64_t> *> histograms_ GUARDED_BY(lock_);
  std::string name_;
  mutable Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  size_t index_ GUARDED_BY(lock_);
  size_t iterations_ GUARDED_BY(lock_);

  DISALLOW_COPY_AND_ASSIGN(CumulativeLogger);
};

}  // namespace art

#endif  // ART_SRC_TIMING_LOGGER_H_
