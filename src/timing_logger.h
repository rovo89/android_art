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

#include "utils.h"  // For NanoTime.

#include <stdint.h>
#include <string>
#include <vector>

namespace art {

class CumulativeLogger;

class TimingLogger {
 public:
  explicit TimingLogger(const std::string& name, bool precise = false)
      : name_(name), precise_(precise) {
    AddSplit("");
  }

  void Reset() {
    times_.clear();
    labels_.clear();
    AddSplit("");
  }

  void AddSplit(const std::string& label) {
    times_.push_back(NanoTime());
    labels_.push_back(label);
  }

  void Dump() const;

  void Dump(std::ostream& os) const;

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
  explicit CumulativeLogger(const std::string& name = "", bool precise = false)
    : name_(name),
      precise_(precise) {
    Reset();
  }

  void SetName(const std::string& name) {
    name_ = name;
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

  void Dump() const;

  void Dump(std::ostream& os) const;

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
