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

#include "timing_logger.h"

#include "base/logging.h"
#include "utils.h"

#include <cmath>
#include <iomanip>

namespace art {

void TimingLogger::Dump() const {
  Dump(LOG(INFO));
}

void TimingLogger::Dump(std::ostream& os) const {
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

void CumulativeLogger::Dump() const {
  Dump(LOG(INFO));
}

void CumulativeLogger::Dump(std::ostream& os) const {
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
    os << StringPrintf("%s: %10s (std_dev %8s) %s\n",
                       name_.c_str(),
                       FormatDuration(mean * kAdjust, tu).c_str(),
                       FormatDuration(std_dev * kAdjust, tu).c_str(),
                       labels_[i].c_str());
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

}  // namespace art
