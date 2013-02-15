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
#ifndef ART_SRC_BASE_HISTOGRAM_H_
#define ART_SRC_BASE_HISTOGRAM_H_

#include <vector>
#include <string>

#include "base/logging.h"
#include "utils.h"

namespace art {

// Creates a data histogram  for a better understanding of statistical data.
// Histogram analysis goes beyond simple mean and standard deviation to provide
// percentiles values, describing where the $% of the input data lies.
// Designed to be simple and used with timing logger in art.

template <class Value> class Histogram {

  const double kAdjust;
  const Value kBucketWidth;
  const size_t kInitialBucketCount;

 public:
  Histogram(std::string);
  void AddValue(Value);
  void CreateHistogram();
  void Reset();
  double Mean() const;
  double Variance() const;
  double Percentile(double) const;
  void PrintConfidenceIntervals(std::ostream &, double) const;
  void PrintBins(std::ostream &);

  uint64_t SampleSize() const {
    return sample_size_;
  }

  Value Sum() const {
    return sum_;
  }

  Value Min() const {
    return min_value_added_;
  }

  Value Max() const {
    return max_value_added_;
  }

  const std::string &Name() const {
    return name_;
  }


 private:
  void BuildRanges();
  void Initialize();
  size_t FindBucket(Value);
  void BucketiseValue(Value);
  // Builds the cumulative distribution function from the frequency data.
  // Must be called before using the percentile function.
  void BuildCDF();
  // Add more buckets to the histogram to fill in a new value that exceeded
  // the max_read_value_.
  void GrowBuckets(Value);
  bool new_values_added_;
  std::string name_;
  // Number of samples placed in histogram.
  uint64_t sample_size_;
  // Width of the bucket range. The lower the value is the more accurate
  // histogram percentiles are.
  Value bucket_width_;
  // Number of bucket to have in the histogram. this value will increase
  // to accommodate for big values that don't fit in initial bucket ranges.
  size_t bucket_count_;
  // Represents the ranges of the histograms. Has SampleSize() + 1 elements
  // e.g. 0,5,10,15 represents ranges 0-5, 5-10, 10-15
  std::vector<Value> ranges_;
  // How many occurrences of values fall within a corresponding range that is
  // saved in the ranges_ vector.
  std::vector<uint64_t> frequency_;
  // Accumulative summation of frequencies.
  // cumulative_freq_[i] = sum(cumulative_freq_[j] : 0 < j < i )
  std::vector<uint64_t> cumulative_freq_;
  // Accumulative summation of percentiles; which is the frequency / SampleSize
  // cumulative_freq_[i] = sum(cumulative_freq_[j] : 0 < j < i )
  std::vector<double> cumulative_perc_;
  // Summation of all the elements inputed by the user.
  Value sum_;
  // Maximum value that can fit in the histogram, grows adaptively.
  Value min_;
  // Minimum value that can fit in the histogram. Fixed to zero for now.
  Value max_;
  // Summation of the values entered. Used to calculate variance.
  Value sum_of_squares_;
  // Maximum value entered in the histogram.
  Value min_value_added_;
  // Minimum value entered in the histogram.
  Value max_value_added_;

  DISALLOW_COPY_AND_ASSIGN(Histogram);
};
}

#endif  // ART_SRC_BASE_HISTOGRAM_H_
