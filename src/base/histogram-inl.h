/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef SRC_BASE_HISTOGRAM_INL_H_
#define SRC_BASE_HISTOGRAM_INL_H_

#include "histogram.h"

#include "utils.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ostream>

namespace art {

template <class Value> inline void Histogram<Value>::AddValue(Value value) {
  CHECK_GE(value, 0.0);
  if (value >= max_) {
    Value new_max = ((value + 1) / bucket_width_ + 1) * bucket_width_;
    DCHECK_GT(new_max, max_);
    GrowBuckets(new_max);
  }

  BucketiseValue(value);
  new_values_added_ = true;
}

template <class Value>
inline Histogram<Value>::Histogram(const std::string name)
    : kAdjust(1000),
      kBucketWidth(5),
      kInitialBucketCount(10),
      bucket_width_(kBucketWidth),
      bucket_count_(kInitialBucketCount) {
  name_ = name;
  Reset();
}

template <class Value>
inline void Histogram<Value>::GrowBuckets(Value new_max) {
  while (max_ < new_max) {
    max_ += bucket_width_;
    ranges_.push_back(max_);
    frequency_.push_back(0);
    bucket_count_++;
  }
}

template <class Value> inline size_t Histogram<Value>::FindBucket(Value val) {
  // Since this is only a linear histogram, bucket index can be found simply with
  // dividing the value by the bucket width.
  DCHECK_GE(val, min_);
  DCHECK_LE(val, max_);
  size_t bucket_idx = static_cast<size_t>((double)(val - min_) / bucket_width_);
  DCHECK_GE(bucket_idx, 0ul);
  DCHECK_LE(bucket_idx, bucket_count_);
  return bucket_idx;
}

template <class Value>
inline void Histogram<Value>::BucketiseValue(Value value) {
  CHECK_LT(value, max_);
  sum_ += value;
  sum_of_squares_ += value * value;
  size_t bucket_idx = FindBucket(value);
  sample_size_++;
  if (value > max_value_added_) {
    max_value_added_ = value;
  }
  if (value < min_value_added_) {
    min_value_added_ = value;
  }
  frequency_[bucket_idx]++;
}

template <class Value> inline void Histogram<Value>::Initialize() {
  DCHECK_GT(bucket_count_, 0ul);
  size_t idx = 0;
  for (; idx < bucket_count_; idx++) {
    ranges_.push_back(min_ + static_cast<Value>(idx) * (bucket_width_));
    frequency_.push_back(0);
  }
  // Cumulative frequency and ranges has a length of 1 over frequency.
  ranges_.push_back(min_ + idx * bucket_width_);
  max_ = bucket_width_ * bucket_count_;
}

template <class Value> inline void Histogram<Value>::Reset() {
  bucket_width_ = kBucketWidth;
  bucket_count_ = kInitialBucketCount;
  max_ = bucket_width_ * bucket_count_;
  sum_of_squares_ = 0;
  sample_size_ = 0;
  min_ = 0;
  sum_ = 0;
  min_value_added_ = std::numeric_limits<Value>::max();
  max_value_added_ = std::numeric_limits<Value>::min();
  new_values_added_ = false;
  ranges_.clear();
  frequency_.clear();
  cumulative_freq_.clear();
  cumulative_perc_.clear();
  Initialize();
}

template <class Value> inline void Histogram<Value>::BuildRanges() {
  for (size_t idx = 0; idx < bucket_count_; ++idx) {
    ranges_.push_back(min_ + idx * bucket_width_);
  }
}

template <class Value> inline double Histogram<Value>::Mean() const {
  DCHECK_GT(sample_size_, 0ull);
  return static_cast<double>(sum_) / static_cast<double>(sample_size_);
}

template <class Value> inline double Histogram<Value>::Variance() const {
  DCHECK_GT(sample_size_, 0ull);
  // Using algorithms for calculating variance over a population:
  // http://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
  Value sum_squared = sum_ * sum_;
  double sum_squared_by_n_squared =
      static_cast<double>(sum_squared) /
      static_cast<double>(sample_size_ * sample_size_);
  double sum_of_squares_by_n =
      static_cast<double>(sum_of_squares_) / static_cast<double>(sample_size_);
  return sum_of_squares_by_n - sum_squared_by_n_squared;
}

template <class Value>
inline void Histogram<Value>::PrintBins(std::ostream &os) {
  DCHECK_GT(sample_size_, 0ull);
  DCHECK(new_values_added_);
  size_t bin_idx = 0;
  while (bin_idx < cumulative_freq_.size()) {
    if (bin_idx > 0 &&
        cumulative_perc_[bin_idx] == cumulative_perc_[bin_idx - 1]) {
      bin_idx++;
      continue;
    }
    os << ranges_[bin_idx] << ": " << cumulative_freq_[bin_idx] << "\t"
       << cumulative_perc_[bin_idx] * 100.0 << "%\n";
    bin_idx++;
  }
}

template <class Value>
inline void Histogram<Value>::PrintConfidenceIntervals(std::ostream &os,
                                                       double interval) const {
  DCHECK_GT(interval, 0);
  DCHECK_LT(interval, 1.0);

  double per_0 = (1.0 - interval) / 2.0;
  double per_1 = per_0 + interval;
  os << Name() << ":\t";
  TimeUnit unit = GetAppropriateTimeUnit(Mean() * kAdjust);
  os << interval << "% C.I. "
     << FormatDuration(Percentile(per_0) * kAdjust, unit);
  os << "-" << FormatDuration(Percentile(per_1) * kAdjust, unit) << " ";
  os << "Avg: " << FormatDuration(Mean() * kAdjust, unit) << " Max: ";
  os << FormatDuration(Max() * kAdjust, unit) << "\n";
}

template <class Value> inline void Histogram<Value>::BuildCDF() {
  DCHECK_EQ(cumulative_freq_.size(), 0ull);
  DCHECK_EQ(cumulative_perc_.size(), 0ull);
  uint64_t accumulated = 0;

  cumulative_freq_.push_back(accumulated);
  cumulative_perc_.push_back(0.0);
  for (size_t idx = 0; idx < frequency_.size(); idx++) {
    accumulated += frequency_[idx];
    cumulative_freq_.push_back(accumulated);
    cumulative_perc_.push_back(static_cast<double>(accumulated) /
                               static_cast<double>(sample_size_));
  }
  DCHECK_EQ(*(cumulative_freq_.end() - 1), sample_size_);
  DCHECK_EQ(*(cumulative_perc_.end() - 1), 1.0);
}

template <class Value> inline void Histogram<Value>::CreateHistogram() {
  DCHECK_GT(sample_size_, 0ull);

  // Create a histogram only if new values are added.
  if (!new_values_added_)
    return;

  // Reset cumulative values in case this is not the first time creating histogram.
  cumulative_freq_.clear();
  cumulative_perc_.clear();
  BuildCDF();
  new_values_added_ = false;
}
;

template <class Value>
inline double Histogram<Value>::Percentile(double per) const {
  DCHECK_GT(cumulative_perc_.size(), 0ull);
  size_t idx;
  for (idx = 0; idx < cumulative_perc_.size(); idx++) {
    if (per <= cumulative_perc_[idx + 1])
      break;
  }
  double lower_value = static_cast<double>(ranges_[idx]);
  double upper_value = static_cast<double>(ranges_[idx + 1]);
  double lower_perc = cumulative_perc_[idx];
  double upper_perc = cumulative_perc_[idx + 1];
  if (per == lower_perc) {
    return lower_value;
  }
  if (per == upper_perc) {
    return upper_value;
  }
  DCHECK_GT(upper_perc, lower_perc);
  double value = lower_value + (upper_value - lower_value) *
                               (per - lower_perc) / (upper_perc - lower_perc);

  return value;
}

}       // namespace art
#endif  // SRC_BASE_HISTOGRAM_INL_H_

