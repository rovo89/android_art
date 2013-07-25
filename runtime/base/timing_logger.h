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

#ifndef ART_RUNTIME_BASE_TIMING_LOGGER_H_
#define ART_RUNTIME_BASE_TIMING_LOGGER_H_

#include "base/histogram.h"
#include "base/macros.h"
#include "base/mutex.h"

#include <string>
#include <vector>

namespace art {

namespace base {
  class TimingLogger;
}  // namespace base

class CumulativeLogger {
 public:
  explicit CumulativeLogger(const std::string& name);
  void prepare_stats();
  ~CumulativeLogger();
  void Start();
  void End();
  void Reset();
  void Dump(std::ostream& os) LOCKS_EXCLUDED(lock_);
  uint64_t GetTotalNs() const;
  // Allow the name to be modified, particularly when the cumulative logger is a field within a
  // parent class that is unable to determine the "name" of a sub-class.
  void SetName(const std::string& name);
  void AddLogger(const base::TimingLogger& logger) LOCKS_EXCLUDED(lock_);

 private:
  void AddPair(const std::string &label, uint64_t delta_time)
      EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void DumpHistogram(std::ostream &os) EXCLUSIVE_LOCKS_REQUIRED(lock_);
  uint64_t GetTotalTime() const;
  static const uint64_t kAdjust = 1000;
  std::vector<Histogram<uint64_t> *> histograms_ GUARDED_BY(lock_);
  std::string name_;
  const std::string lock_name_;
  mutable Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  size_t index_ GUARDED_BY(lock_);
  size_t iterations_ GUARDED_BY(lock_);

  DISALLOW_COPY_AND_ASSIGN(CumulativeLogger);
};

namespace base {

// A replacement to timing logger that know when a split starts for the purposes of logging.
class TimingLogger {
 public:
  explicit TimingLogger(const char* name, bool precise, bool verbose);

  // Clears current splits and labels.
  void Reset();

  // Starts a split, a split shouldn't be in progress.
  void StartSplit(const char*  new_split_label);

  // Ends the current split and starts the one given by the label.
  void NewSplit(const char* new_split_label);

  // Ends the current split and records the end time.
  void EndSplit();

  uint64_t GetTotalNs() const;

  void Dump(std::ostream& os) const;

  const std::vector<std::pair<uint64_t, const char*> >& GetSplits() const {
    return splits_;
  }

 protected:
  // The name of the timing logger.
  const char* name_;

  // Do we want to print the exactly recorded split (true) or round down to the time unit being
  // used (false).
  const bool precise_;

  // Verbose logging.
  const bool verbose_;

  // The name of the current split.
  const char* current_split_;

  // The nanosecond time the current split started on.
  uint64_t current_split_start_ns_;

  // Splits are nanosecond times and split names.
  std::vector<std::pair<uint64_t, const char*> > splits_;

 private:
  DISALLOW_COPY_AND_ASSIGN(TimingLogger);
};

}  // namespace base
}  // namespace art

#endif  // ART_RUNTIME_BASE_TIMING_LOGGER_H_
