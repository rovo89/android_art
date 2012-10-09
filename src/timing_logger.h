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

#include "logging.h"
#include "utils.h"

#include <cmath>
#include <stdint.h>
#include <string>
#include <vector>

namespace art {

class CumulativeLogger;

class TimingLogger {
 public:
  explicit TimingLogger(const char* name, bool precise = false)
      : name_(name), precise_(precise) {
    AddSplit("");
  }

  void AddSplit(const std::string& label) {
    times_.push_back(NanoTime());
    labels_.push_back(label);
  }

  void Dump() const {
    Dump(LOG(INFO));
  }

  void Dump(std::ostream& os) const {
    uint64_t largest_time = 0;
    os << name_ << ": begin\n";
    for (size_t i = 1; i < times_.size(); ++i) {
      uint64_t delta_time = times_[i] - times_[i - 1];
      largest_time = std::max(largest_time, delta_time);
    }
    // Compute which type of unit we will use for printing the timings.
    TimeUnit tu = GetAppropriateTimeUnit(largest_time);
    uint64_t divisor = GetNsToTimeUnitDivisor(tu);
    for (size_t i = 1; i < times_.size(); ++i) {
      uint64_t delta_time = times_[i] - times_[i - 1];
      if (!precise_ && divisor >= 1000) {
        // Make the fraction 0.
        delta_time -= delta_time % (divisor / 1000);
      }
      os << name_ << ": " << std::setw(8) << FormatDuration(delta_time, tu) << " " << labels_[i]
                  << "\n";
    }
    os << name_ << ": end, " << NsToMs(GetTotalNs()) << " ms\n";
  }

  uint64_t GetTotalNs() const {
    return times_.back() - times_.front();
  }

 protected:
  std::string name_;
  bool precise_;
  std::vector<uint64_t> times_;
  std::vector<std::string> labels_;

  friend class CumulativeLogger;
};

class CumulativeLogger {
 public:
  explicit CumulativeLogger(const char* name = "", bool precise = false)
    : name_(name),
      precise_(precise) {
    Reset();
  }

  void Start() {
    index_ = 0;
    last_split_ = NanoTime();
  }

  void End() {
    iterations_++;
  }

  void AddSplit(const std::string& label) {
    uint64_t cur_time = NanoTime();
    AddPair(label, cur_time - last_split_);
    last_split_ = cur_time;
  }

  void Reset() {
    times_.clear();
    labels_.clear();
    times_squared_.clear();
    iterations_ = 0;
    total_time_squared_ = 0;
  }

  void AddPair(const std::string& label, uint64_t delta_time) {
    // Convert delta time to microseconds so that we don't overflow our counters.
    delta_time /= kAdjust;
    if (index_ >= times_.size()) {
      times_.push_back(delta_time);
      times_squared_.push_back(delta_time * delta_time);
      labels_.push_back(label);
    } else {
      times_[index_] += delta_time;
      times_squared_[index_] += delta_time * delta_time;
      DCHECK_EQ(labels_[index_], label);
    }
    index_++;
  }

  void AddLogger(const TimingLogger& logger) {
    DCHECK_EQ(logger.times_.size(), logger.labels_.size());
    uint64_t total_time = 0;
    for (size_t i = 1; i < logger.times_.size(); ++i) {
      const uint64_t delta_time = logger.times_[i] - logger.times_[i - 1];
      const std::string& label = logger.labels_[i];
      AddPair(label, delta_time);
      total_time += delta_time;
    }
    total_time /= kAdjust;
    total_time_squared_ += total_time * total_time;
  }

  void Dump() const {
    Dump(LOG(INFO));
  }

  void Dump(std::ostream& os) const {
    os << name_ << ": iterations " << iterations_ << " begin\n";
    //Find which unit we will use for the timing logger.
    uint64_t largest_mean = 0;
    for (size_t i = 0; i < times_.size(); ++i) {
      // Convert back to nanoseconds from microseconds.
      uint64_t mean = times_[i] / iterations_;
      largest_mean = std::max(largest_mean, mean);
    }
    // Convert largest mean back to ns
    TimeUnit tu = GetAppropriateTimeUnit(largest_mean * kAdjust);
    uint64_t divisor = GetNsToTimeUnitDivisor(tu);
    for (size_t i = 0; i < times_.size(); ++i) {
      uint64_t mean_x2 = times_squared_[i] / iterations_;
      uint64_t mean = times_[i] / iterations_;
      uint64_t variance = mean_x2 - (mean * mean);
      uint64_t std_dev = static_cast<uint64_t>(std::sqrt(static_cast<double>(variance)));
      if (!precise_ && divisor >= 1000) {
        // Make the fraction 0.
        mean -= mean % (divisor / 1000);
        std_dev -= std_dev % (divisor / 1000);
      }
      os << name_ << ": " << std::setw(8)
         << FormatDuration(mean * kAdjust, tu) << " std_dev "
         << FormatDuration(std_dev * kAdjust, tu) << " " << labels_[i] << "\n";
    }
    uint64_t total_mean_x2 = total_time_squared_;
    uint64_t mean_total_ns = GetTotalTime();
    if (iterations_ != 0) {
      total_mean_x2 /= iterations_;
      mean_total_ns /= iterations_;
    }
    uint64_t total_variance = total_mean_x2 - (mean_total_ns * mean_total_ns);
    uint64_t total_std_dev = static_cast<uint64_t>(
        std::sqrt(static_cast<double>(total_variance)));
    os << name_ << ": end, mean " << PrettyDuration(mean_total_ns * kAdjust)
       << " std_dev " << PrettyDuration(total_std_dev * kAdjust) << "\n";
  }

  uint64_t GetTotalNs() const {
    return GetTotalTime() * kAdjust;
  }

 private:

  uint64_t GetTotalTime() const {
    uint64_t total = 0;
    for (size_t i = 0; i < times_.size(); ++i) {
      total += times_[i];
    }
    return total;
  }

  static const uint64_t kAdjust = 1000;
  std::string name_;
  bool precise_;
  uint64_t total_time_squared_;
  std::vector<uint64_t> times_;
  std::vector<uint64_t> times_squared_;
  std::vector<std::string> labels_;
  size_t index_;
  size_t iterations_;
  uint64_t last_split_;
};

}  // namespace art

#endif  // ART_SRC_TIMING_LOGGER_H_
